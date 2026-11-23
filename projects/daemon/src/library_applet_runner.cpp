#include "library_applet_runner.hpp"

#include <switch/applets/libapplet.h>
#include <switchu/file_log.hpp>

namespace switchu::daemon {
namespace {

Result waitForLaunchable() {
    Handle handle = INVALID_HANDLE;
    Service* controller = appletGetServiceSession_SelfController();
    serviceAssumeDomain(controller);
    Result rc = serviceDispatch(controller, 9, // GetLibraryAppletLaunchableEvent
        .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
        .out_handles = &handle);
    if (R_FAILED(rc))
        return rc;
    Event event{};
    eventLoadRemote(&event, handle, false);
    rc = eventWait(&event, 250'000'000ULL);
    eventClose(&event);
    return rc;
}

class HolderGuard final {
public:
    AppletHolder holder{};
    bool created = false;

    ~HolderGuard() {
        if (created)
            appletHolderClose(&holder);
    }
};

Result exitReasonResult(LibAppletExitReason reason) {
    // Closing a settings-style applet with B/HOME reports Canceled even
    // though ownership is restored normally. It is a user outcome, not a
    // platform failure that should survive into the next menu session.
    return reason == LibAppletExitReason_Normal || reason == LibAppletExitReason_Canceled
        ? 0
        : MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit);
}

} // namespace

Result createForegroundApplet(AppletHolder* holder, AppletId id) {
    Result rc = waitForLaunchable();
    if (R_FAILED(rc))
        return rc;

    // libnx's create and start wrappers can wait indefinitely for AM when
    // the foreground changes. Use the same commands with a bounded wait.
    *holder = {};
    holder->mode = LibAppletMode_AllForeground;
    const struct {
        u32 id;
        u32 mode;
    } in{static_cast<u32>(id), static_cast<u32>(holder->mode)};
    Service* creator = appletGetServiceSession_LibraryAppletCreator();
    serviceAssumeDomain(creator);
    rc = serviceDispatchIn(creator, 0, in,
        .out_num_objects = 1,
        .out_objects = &holder->s);
    if (R_FAILED(rc))
        return rc;

    Handle handle = INVALID_HANDLE;
    serviceAssumeDomain(&holder->s);
    rc = serviceDispatch(&holder->s, 0, // GetAppletStateChangedEvent
        .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
        .out_handles = &handle);
    if (R_SUCCEEDED(rc))
        eventLoadRemote(&holder->StateChangedEvent, handle, false);
    else
        appletHolderClose(holder);
    return rc;
}

Result startForegroundApplet(AppletHolder* holder) {
    Result rc = waitForLaunchable();
    if (R_FAILED(rc))
        return rc;
    serviceAssumeDomain(&holder->s);
    return serviceDispatch(&holder->s, 10); // Start
}

Result runLibraryApplet(const LibraryAppletRequest& request,
                        LibraryAppletPump pump,
                        LibraryAppletExitRequested exitRequested,
                        Event* extraWakeEvent) {
    if (request.id == AppletId_None ||
        (request.inputCount != 0 && request.inputs == nullptr)) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    switchu::FileLog::log("[applet] %s create id=0x%X version=0x%X inputs=%zu",
                          request.name, static_cast<unsigned>(request.id),
                          request.version, request.inputCount);

    HolderGuard guard;
    Result rc = createForegroundApplet(&guard.holder, request.id);
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[applet] %s create FAIL: 0x%X", request.name, rc);
        return rc;
    }
    guard.created = true;

    if (request.pushCommonArgs) {
        LibAppletArgs commonArgs{};
        libappletArgsCreate(&commonArgs, request.version);
        libappletArgsSetPlayStartupSound(&commonArgs, request.playStartupSound);
        rc = libappletArgsPush(&commonArgs, &guard.holder);
    }

    for (std::size_t index = 0; R_SUCCEEDED(rc) && index < request.inputCount; ++index) {
        const auto& input = request.inputs[index];
        if (!input.data || input.size == 0) {
            rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
            break;
        }
        rc = libappletPushInData(&guard.holder, input.data, input.size);
    }

    if (R_FAILED(rc)) {
        switchu::FileLog::log("[applet] %s input FAIL: 0x%X", request.name, rc);
        return rc;
    }

    rc = startForegroundApplet(&guard.holder);
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[applet] %s start FAIL: 0x%X", request.name, rc);
        return rc;
    }

    // Sleep until the applet changes state or a message arrives instead of
    // polling every 10 ms for as long as the applet is open. Everything the
    // pump handles (HOME, sleep, wake, SAMS) arrives on these events.
    Waiter waiters[3]{};
    s32 waiterCount = 0;
    waiters[waiterCount++] = waiterForEvent(&guard.holder.StateChangedEvent);
    if (Event* messageEvent = appletGetMessageEvent())
        waiters[waiterCount++] = waiterForEvent(messageEvent);
    if (extraWakeEvent)
        waiters[waiterCount++] = waiterForEvent(extraWakeEvent);

    bool exitWasRequested = false;
    bool wokeOnMessage = false;
    while (appletHolderActive(&guard.holder) &&
           !appletHolderCheckFinished(&guard.holder)) {
        const bool handled = pump && pump();

        if (!exitWasRequested && exitRequested && exitRequested()) {
            exitWasRequested = true;
            switchu::FileLog::log("[applet] %s HOME exit requested", request.name);
            const Result exitRc = appletHolderRequestExitOrTerminate(
                &guard.holder, 500'000'000ULL);
            if (R_FAILED(exitRc)) {
                switchu::FileLog::log("[applet] %s HOME exit FAIL: 0x%X",
                                      request.name, exitRc);
                rc = exitRc;
            }
            break;
        }

        if (handled)
            continue; // one message per pass, as before: look again at once

        // A channel event that stays signalled with nothing to pop must not
        // turn into a hot loop: back off like the old 10 ms poll.
        if (wokeOnMessage)
            svcSleepThread(10'000'000ULL);
        wokeOnMessage = false;

        // The timeout is a safety net only: a missed signal costs at most this.
        s32 signalled = -1;
        const Result waitRc = waitObjects(&signalled, waiters, waiterCount,
                                          1'000'000'000ULL);
        if (R_FAILED(waitRc) && waitRc != KERNELRESULT(TimedOut))
            svcSleepThread(10'000'000ULL); // a rejected waiter: poll as before
        else if (R_SUCCEEDED(waitRc) && signalled > 0)
            wokeOnMessage = true;
    }

    const Result waitRc = eventWait(&guard.holder.StateChangedEvent, 1'000'000'000ULL);
    if (R_FAILED(waitRc)) {
        switchu::FileLog::log("[applet] %s exit wait rc=0x%X", request.name, waitRc);
        appletRequestToGetForeground();
        return R_FAILED(rc) ? rc : waitRc;
    }
    appletHolderJoin(&guard.holder);
    const LibAppletExitReason exitReason = appletHolderGetExitReason(&guard.holder);
    if (R_SUCCEEDED(rc))
        rc = exitReasonResult(exitReason);

    // A canceled applet does not guarantee an output storage. Trying to pop
    // it turns a clean B/HOME return into a misleading IPC failure.
    if (R_SUCCEEDED(rc) && exitReason == LibAppletExitReason_Normal
        && request.output && request.outputSize != 0) {
        rc = libappletPopOutData(&guard.holder, request.output,
                                 request.outputSize,
                                 request.outputTransferSize);
    }

    const Result foregroundRc = appletRequestToGetForeground();
    switchu::FileLog::log(
        "[applet] %s closed reason=%d rc=0x%X module=%u desc=%u restore_fg=0x%X",
        request.name, static_cast<int>(exitReason), rc,
        R_MODULE(rc), R_DESCRIPTION(rc), foregroundRc);
    return rc;
}

} // namespace switchu::daemon
