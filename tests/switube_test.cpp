#include "Switube.hpp"
#include <cassert>
#include <cstring>
#include <limits>

using cards::Switube;

int main() {
    SwitubeIpcStatus status{};
    // Reject truncated or mismatched service responses before interpreting fields.
    assert(!Switube::valid(status));
    status.struct_size = sizeof(status);
    status.abi_version = SWITUBE_IPC_STATUS_ABI_VERSION + 1;
    assert(!Switube::decode(status, 123).connected);
    status.abi_version = SWITUBE_IPC_STATUS_ABI_VERSION;
    status.playback_state = SwitubePlaybackState_Playing;
    status.flags = SwitubeStatusFlag_HasTrack | SwitubeStatusFlag_Shuffle | SwitubeStatusFlag_RepeatOne;
    status.position_ms = 12000;
    status.duration_ms = 240000;
    status.volume_permille = 735;
    std::strcpy(status.title, "A song");
    std::strcpy(status.artist, "An artist");
    auto now = Switube::decode(status, 123);
    assert(now.connected && now.active && now.playing && now.shuffle);
    assert(now.track == "A song" && now.artist == "An artist");
    assert(now.progressMs == 12000 && now.durationMs == 240000 && now.volume == 73 && now.at == 123);
    // The UI cycles Off -> All -> One, while the service's enum is Off/One/All.
    assert(now.repeat == 2 && Switube::repeatWire(now.repeat) == SwitubeRepeatMode_One);
    status.flags = SwitubeStatusFlag_RepeatAll;
    now = Switube::decode(status, 124);
    assert(!now.active && !now.shuffle && now.repeat == 1);
    assert(Switube::repeatWire(now.repeat) == SwitubeRepeatMode_All);
    assert(Switube::repeatWire(0) == SwitubeRepeatMode_Off);
    status.playback_state = SwitubePlaybackState_Paused;
    status.flags = 0;
    assert(!Switube::decode(status, 125).playing);
    // Fixed-width IPC strings need not be terminated; never read past them.
    std::memset(status.title, 'x', sizeof(status.title));
    std::memset(status.artist, 'y', sizeof(status.artist));
    status.volume_permille = 9999;
    status.position_ms = std::numeric_limits<std::uint32_t>::max();
    status.duration_ms = 15000;
    now = Switube::decode(status, 126);
    assert(now.track.size() == sizeof(status.title) && now.artist.size() == sizeof(status.artist));
    assert(now.volume == 100 && now.progressMs == 15000);
    status.duration_ms = std::numeric_limits<std::uint32_t>::max();
    now = Switube::decode(status, 127);
    assert(now.durationMs == INT32_MAX && now.progressMs == INT32_MAX);
}
