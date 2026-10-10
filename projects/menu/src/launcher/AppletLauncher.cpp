#include "AppletLauncher.hpp"
#include "core/DebugLog.hpp"
#ifdef SWITCHU_MENU
#include "smi_commands.hpp"
#include <switchu/smi_protocol.hpp>
#endif
#include <switch.h>
#include <sys/stat.h>
#include <cstdio>

void AppletLauncher::init(Callbacks cbs) {
    m_cb = std::move(cbs);
}

#ifdef SWITCHU_MENU
bool AppletLauncher::isAppRunning() const  { return m_appRunning; }
bool AppletLauncher::appHasForeground() const { return m_appHasForeground; }
bool AppletLauncher::isAppSuspended(uint64_t titleId) const {
    return m_suspendedTitleId != 0 && m_suspendedTitleId == titleId;
}
uint64_t AppletLauncher::suspendedTitleId() const { return m_suspendedTitleId; }

void AppletLauncher::setAppRunning(bool v)          { m_appRunning = v; }
void AppletLauncher::setAppHasForeground(bool v)    { m_appHasForeground = v; }
void AppletLauncher::setSuspendedTitleId(uint64_t v){ m_suspendedTitleId = v; }

void AppletLauncher::setStartupStatus(uint64_t suspendedTitleId, bool appRunning) {
    m_suspendedTitleId = suspendedTitleId;
    m_appRunning       = appRunning;
    m_appHasForeground = false;
    DebugLog::log("[launcher] startup status: suspended=0x%016lX running=%d",
                  suspendedTitleId, appRunning);
}

bool AppletLauncher::preparePowerAction() {
    if (!m_cb.beforePowerAction)
        return true;
    if (m_cb.beforePowerAction())
        return true;
    DebugLog::log("[launcher] power action cancelled: SD commit failed");
    return false;
}

void AppletLauncher::launchAlbum() {
    DebugLog::log("[launcher] requesting Album launch via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::LaunchAlbum);
    DebugLog::log("[launcher] Album rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchMiiEditor() {
    DebugLog::log("[launcher] requesting Mii Editor launch via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::LaunchMiiEditor);
    DebugLog::log("[launcher] Mii Editor rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchControllerPairing() {
    DebugLog::log("[launcher] requesting Controller pairing via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::LaunchControllers);
    DebugLog::log("[launcher] Controller pairing rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchControllerRemapping() {
    DebugLog::log("[launcher] requesting controller remapping via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(
        switchu::smi::SystemMessage::LaunchControllerRemapping);
    DebugLog::log("[launcher] controller remapping rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchNetConnect() {
    DebugLog::log("[launcher] requesting NetConnect launch via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::LaunchNetConnect);
    DebugLog::log("[launcher] NetConnect rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchUserPage(AccountUid uid) {
    DebugLog::log("[launcher] requesting User Page launch via daemon");
    Result rc = switchu::menu::smi_cmd::launchUserPage(uid);
    DebugLog::log("[launcher] User Page rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchUserCreator() {
    DebugLog::log("[launcher] requesting system user creator via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(
        switchu::smi::SystemMessage::LaunchUserCreator);
    DebugLog::log("[launcher] user creator rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::enterSleep() {
    if (!preparePowerAction()) return;
    DebugLog::log("[launcher] requesting sleep");
    switchu::menu::smi_cmd::enterSleep();
}

void AppletLauncher::shutdown() {
    if (!preparePowerAction()) return;
    DebugLog::log("[launcher] requesting shutdown");
    switchu::menu::smi_cmd::shutdown();
}

void AppletLauncher::reboot() {
    if (!preparePowerAction()) return;
    DebugLog::log("[launcher] requesting reboot");
    switchu::menu::smi_cmd::reboot();
}

void AppletLauncher::launchApplication(uint64_t titleId, AccountUid uid) {
    DebugLog::log("[launcher] tid=%016lX", titleId);
    Result rc = switchu::menu::smi_cmd::launchApplication(titleId, uid);
    if (R_FAILED(rc)) {
        DebugLog::log("[launcher] FAIL: 0x%X", rc);
        return;
    }
    DebugLog::log("[launcher] command sent, closing menu");
    if (m_cb.requestExit) m_cb.requestExit();
}

void AppletLauncher::resumeApplication() {
    if (m_suspendedTitleId == 0) {
        DebugLog::log("[launcher] no app suspended!");
        return;
    }
    switchu::FileLog::logCommit("[launcher] resume: sending command");
    const Result rc = switchu::menu::smi_cmd::resumeApplication();
    switchu::FileLog::logCommit("[launcher] resume: command returned 0x%X", rc);
    if (m_cb.requestExit) m_cb.requestExit();
    switchu::FileLog::logCommit("[launcher] resume: exit requested");
}

void AppletLauncher::terminateApplication() {
    if (m_suspendedTitleId == 0) {
        DebugLog::log("[launcher] no app suspended, nothing to terminate");
        return;
    }
    DebugLog::log("[launcher] requesting terminate 0x%016lX", (uint64_t)m_suspendedTitleId);
    const Result rc = switchu::menu::smi_cmd::terminateApplication();
    if (R_FAILED(rc))
        DebugLog::log("[launcher] terminate command FAIL: 0x%X", rc);
}

void AppletLauncher::checkRunningApplication() {
}

// ponytail: the daemon has no eShop/News/Web/Friends messages yet; homebrew build only.
bool AppletLauncher::launchSphaira() { return false; }
void AppletLauncher::findControllers() {}
Result AppletLauncher::launchEshop(AccountUid)   { return 0; }
Result AppletLauncher::launchBrowser(AccountUid, const char*) { return 0; }
Result AppletLauncher::launchFriends(AccountUid) { return 0; }

#else

bool AppletLauncher::isAppRunning() const            { return false; }
bool AppletLauncher::appHasForeground() const        { return false; }
bool AppletLauncher::isAppSuspended(uint64_t) const  { return false; }
uint64_t AppletLauncher::suspendedTitleId() const    { return 0; }
void AppletLauncher::setAppRunning(bool)             {}
void AppletLauncher::setAppHasForeground(bool)       {}
void AppletLauncher::setSuspendedTitleId(uint64_t)   {}

// Homebrew .nro: open the system applets directly. Each call blocks until the
// applet closes. The web-based ones need the .nro launched over a game
// (title override); in applet/album mode they fail and only get logged.
namespace {

void logRc(const char* what, Result rc) {
    DebugLog::log("[launcher] %s rc=0x%X", what, rc);
}

AccountUid currentUser() {
    AccountUid uid{};
    if (R_FAILED(accountGetPreselectedUser(&uid)) || !accountUidIsValid(&uid))
        if (R_FAILED(accountTrySelectUserWithoutInteraction(&uid, false)) || !accountUidIsValid(&uid))
            accountGetLastOpenedUser(&uid);
    return uid;
}

AccountUid orCurrent(AccountUid uid) {
    return accountUidIsValid(&uid) ? uid : currentUser();
}

// The web-applet setup Sphaira uses (sphaira/source/web.cpp), which is known to
// work on current firmware: open whitelist, transfer memory, the user's uid.
Result showWeb(const char* url, AccountUid uid) {
    WebCommonConfig cfg{};
    WebCommonReply reply{};
    Result rc = webPageCreate(&cfg, url);
    if (R_FAILED(rc)) return rc;
    webConfigSetWhitelist(&cfg, ".*");
    webConfigSetEcClientCert(&cfg, true);
    webConfigSetScreenShot(&cfg, true);
    webConfigSetBootDisplayKind(&cfg, WebBootDisplayKind_Black);
    webConfigSetBackgroundKind(&cfg, WebBackgroundKind_Default);
    webConfigSetPointer(&cfg, true);
    webConfigSetLeftStickMode(&cfg, WebLeftStickMode_Pointer);
    webConfigSetBootAsMediaPlayer(&cfg, false);
    webConfigSetJsExtension(&cfg, true);
    webConfigSetMediaPlayerAutoClose(&cfg, false);
    webConfigSetPageCache(&cfg, true);
    webConfigSetFooterFixedKind(&cfg, WebFooterFixedKind_Default);
    webConfigSetPageFade(&cfg, true);
    webConfigSetPageScrollIndicator(&cfg, true);
    webConfigSetBootMode(&cfg, WebSessionBootMode_AllForeground);
    webConfigSetTransferMemory(&cfg, true);
    webConfigSetTouchEnabledOnContents(&cfg, true);
    webConfigSetWebAudio(&cfg, false);
    uid = orCurrent(uid);
    if (accountUidIsValid(&uid)) webConfigSetUid(&cfg, uid);
    return webConfigShow(&cfg, &reply);
}

} // namespace

void AppletLauncher::launchAlbum()     { logRc("album", albumLaShowAllAlbumFiles()); }

// Hand the process to Sphaira through hbloader, so it keeps HomeNX's mode
// (application mode when HomeNX was started over a game).
bool AppletLauncher::launchSphaira() {
    static const char* const paths[] = {
        "sdmc:/switch/sphaira/sphaira.nro",
        "sdmc:/switch/sphaira.nro",
        "sdmc:/hbmenu.nro", // sphaira installs itself here when set as the menu
    };
    struct stat st;
    for (const char* path : paths) {
        if (stat(path, &st) != 0) continue;
        char args[0x300];
        std::snprintf(args, sizeof(args), "\"%s\"", path);
        Result rc = envSetNextLoad(path, args);
        logRc(path, rc);
        if (R_FAILED(rc)) return false;
        if (m_cb.requestExit) m_cb.requestExit();
        return true;
    }
    DebugLog::log("[launcher] sphaira not found");
    return false;
}
void AppletLauncher::launchMiiEditor() { logRc("mii", miiLaShowMiiEdit(MiiSpecialKeyCode_Normal)); }
void AppletLauncher::launchControllerPairing() {
    HidLaControllerSupportArg arg;
    hidLaCreateControllerSupportArg(&arg);
    logRc("controllers", hidLaShowControllerSupportForSystem(nullptr, &arg, false));
}

Result AppletLauncher::launchEshop(AccountUid uid) {
    // ponytail: ShopN takes the web TLV args with ShimKind_Shop (switchbrew
    // "Internet Browser"); untested on hardware, so fall back to the web store.
    uid = orCurrent(uid);
    WebCommonConfig cfg{};
    Result rc = webPageCreate(&cfg, "https://bugyo.hac.lp1.eshop.nintendo.net/ashigaru/");
    if (R_SUCCEEDED(rc)) {
        if (accountUidIsValid(&uid)) webConfigSetUid(&cfg, uid);
        reinterpret_cast<WebArgHeader*>(cfg.arg.data)->shimKind = WebShimKind_Shop;
        cfg.appletid = AppletId_LibraryAppletShop;
        rc = webConfigShow(&cfg, nullptr);
    }
    logRc("eshop", rc);
    if (R_SUCCEEDED(rc)) return rc;
    rc = showWeb("https://www.nintendo.com/store/", uid);
    logRc("eshop-web", rc);
    return rc;
}

// Dark DuckDuckGo start page: black background suits the OLED panel.
Result AppletLauncher::launchBrowser(AccountUid uid, const char* url) {
    Result rc = showWeb(url ? url : "https://duckduckgo.com/?kae=d&kak=-1&kax=-1&kaq=-1", uid);
    logRc("browser", rc);
    return rc;
}

Result AppletLauncher::launchFriends(AccountUid uid) {
    uid = orCurrent(uid);
    if (!accountUidIsValid(&uid)) {
        PselUserSelectionSettings sel{};
        Result rc = pselShowUserSelector(&uid, &sel);
        if (R_FAILED(rc)) return rc;
    }
    Result rc = friendsLaShowFriendList(uid);
    logRc("friends", rc);
    return rc;
}

void AppletLauncher::launchControllerRemapping() {
    HidLaControllerKeyRemappingArg arg;
    hidLaCreateControllerKeyRemappingArg(&arg);
    logRc("remap", hidLaShowControllerKeyRemappingForSystem(&arg, HidLaControllerSupportCaller_System));
}

// Three short rumbles on every connected controller, like the system's
// "Find Controllers".
void AppletLauncher::findControllers() {
    static const HidNpadIdType ids[] = {
        HidNpadIdType_Handheld, HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3,
        HidNpadIdType_No4, HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8,
    };
    HidVibrationDeviceHandle handles[18];
    int count = 0;
    for (HidNpadIdType id : ids) {
        const u32 styles = hidGetNpadStyleSet(id);
        HidNpadStyleTag style;
        int n = 2;
        if (styles & HidNpadStyleTag_NpadHandheld) style = HidNpadStyleTag_NpadHandheld;
        else if (styles & HidNpadStyleTag_NpadFullKey) style = HidNpadStyleTag_NpadFullKey;
        else if (styles & HidNpadStyleTag_NpadJoyDual) style = HidNpadStyleTag_NpadJoyDual;
        else if (styles & HidNpadStyleTag_NpadJoyLeft) { style = HidNpadStyleTag_NpadJoyLeft; n = 1; }
        else if (styles & HidNpadStyleTag_NpadJoyRight) { style = HidNpadStyleTag_NpadJoyRight; n = 1; }
        else continue;
        if (R_SUCCEEDED(hidInitializeVibrationDevices(handles + count, n, id, style)))
            count += n;
    }
    DebugLog::log("[launcher] find controllers: %d motors", count);
    if (count == 0) return;
    HidVibrationValue on[18], off[18];
    for (int i = 0; i < count; ++i) {
        on[i] = {0.7f, 160.f, 0.7f, 320.f};
        off[i] = {0.f, 160.f, 0.f, 320.f};
    }
    // ponytail: blocks the UI ~1 s; fine for a button you press on purpose.
    for (int pulse = 0; pulse < 3; ++pulse) {
        hidSendVibrationValues(handles, on, count);
        svcSleepThread(220'000'000ull);
        hidSendVibrationValues(handles, off, count);
        svcSleepThread(140'000'000ull);
    }
}

void AppletLauncher::launchNetConnect()        {}
void AppletLauncher::launchUserPage(AccountUid) {}
void AppletLauncher::launchUserCreator()       {}
void AppletLauncher::enterSleep()              {}
void AppletLauncher::shutdown()                {}
void AppletLauncher::reboot()                  {}
void AppletLauncher::launchApplication(uint64_t titleId, AccountUid) {
    // The system launches it after HomeNX exits. It can't carry a user, so
    // titles that need one get the system's own picker if it wants one.
    Result rc = appletRequestLaunchApplication(titleId, nullptr);
    DebugLog::log("[launcher] request launch %016lX rc=0x%X", titleId, rc);
    if (R_SUCCEEDED(rc) && m_cb.requestExit) m_cb.requestExit();
}
void AppletLauncher::resumeApplication()       {}
void AppletLauncher::terminateApplication()    {}
void AppletLauncher::checkRunningApplication() {}

#endif
