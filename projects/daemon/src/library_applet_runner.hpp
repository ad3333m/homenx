#pragma once

#include <switch.h>

#include <cstddef>
#include <cstdint>

namespace switchu::daemon {

struct LibraryAppletInput {
    const void* data = nullptr;
    std::size_t size = 0;
};

struct LibraryAppletRequest {
    AppletId id = AppletId_None;
    const char* name = "LibraryApplet";
    std::uint32_t version = 0;
    bool pushCommonArgs = true;
    bool playStartupSound = true;
    const LibraryAppletInput* inputs = nullptr;
    std::size_t inputCount = 0;
    void* output = nullptr;
    std::size_t outputSize = 0;
    std::size_t* outputTransferSize = nullptr;
};

// Handles what is waiting on the applet-manager channels; returns whether it
// found anything.
using LibraryAppletPump = bool (*)();
using LibraryAppletExitRequested = bool (*)();

// Runs a foreground library applet without delegating its lifetime to a
// blocking libnx convenience wrapper. This keeps the daemon able to process
// HOME and guarantees join/close on every successful start path.
// The pump runs whenever the applet's state, the applet message event or
// extraWakeEvent (the general channel, may be null) is signalled.
Result runLibraryApplet(const LibraryAppletRequest& request,
                        LibraryAppletPump pump,
                        LibraryAppletExitRequested exitRequested,
                        Event* extraWakeEvent = nullptr);

} // namespace switchu::daemon
