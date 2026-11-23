#pragma once
// Host-only IPC double for the production Switube worker.
#include <switube/ipc_protocol.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <type_traits>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Result = unsigned;
#define R_SUCCEEDED(rc) ((rc) == 0)
#define R_FAILED(rc) ((rc) != 0)
struct Service { bool active = false; };
namespace switube_fake {
inline std::atomic<unsigned> starts{0}, connects{0}, polls{0}, nexts{0};
inline std::atomic<bool> registered{false};
inline std::atomic<u32> volume{700}, repeat{0}, shuffle{0};
inline std::atomic<bool> playing{false};
template <class T> Result output(unsigned command, T& out) {
    if constexpr (std::is_same_v<T, SwitubeIpcStatus>) {
        if (command != SwitubeIpcCommand_GetStatus) return 1;
        ++polls;
        out = {};
        out.struct_size = sizeof(out); out.abi_version = SWITUBE_IPC_STATUS_ABI_VERSION;
        out.flags = SwitubeStatusFlag_HasTrack | (shuffle ? SwitubeStatusFlag_Shuffle : 0)
            | (repeat == SwitubeRepeatMode_All ? SwitubeStatusFlag_RepeatAll : 0)
            | (repeat == SwitubeRepeatMode_One ? SwitubeStatusFlag_RepeatOne : 0);
        out.playback_state = playing ? SwitubePlaybackState_Playing : SwitubePlaybackState_Paused;
        out.volume_permille = volume;
        std::strcpy(out.title, "Background song");
    } else if constexpr (std::is_same_v<T, SwitubeIpcBrowseHeader>) {
        out = {};
        out.struct_size = sizeof(out); out.abi_version = SWITUBE_IPC_BROWSE_ABI_VERSION;
        out.generation = 7; out.state = SwitubeBrowseState_Ready; out.item_count = 1;
        std::strcpy(out.title, "Downloads");
    } else {
        out = SWITUBE_IPC_API_VERSION;
    }
    return 0;
}
template <class T> Result input(unsigned command, const T& in) {
    if constexpr (std::is_same_v<T, SwitubeIpcBrowseRequest>) {
        return command >= SwitubeIpcCommand_BrowseHome ? 0 : 1;
    } else {
        if (command == SwitubeIpcCommand_SetVolume) volume = in;
        else if (command == SwitubeIpcCommand_SetRepeatMode) repeat = in;
        else if (command == SwitubeIpcCommand_SetShuffle) shuffle = in;
        else return 1;
        return 0;
    }
}
inline Result noInput(unsigned command) {
    if (command == SwitubeIpcCommand_PlayPause) playing = !playing;
    if (command == SwitubeIpcCommand_Next) ++nexts;
    return 0;
}
inline Result item(unsigned, u32, SwitubeIpcBrowseItem& out) {
    out = {};
    out.struct_size = sizeof(out); out.abi_version = SWITUBE_IPC_BROWSE_ABI_VERSION;
    out.generation = 7; out.kind = SwitubeBrowseItemKind_Track;
    std::strcpy(out.name, "Downloaded song");
    return 0;
}
}
inline bool serviceIsActive(Service* s) { return s->active; }
inline void serviceClose(Service* s) { s->active = false; }
inline Result smGetService(Service* s, const char*) { s->active = true; ++switube_fake::connects; return 0; }
inline const char* smEncodeName(const char* name) { return name; }
inline void* smGetServiceSessionTipc() { return nullptr; }
inline Result tipcDispatchInOut(void*, unsigned, const char*, u8& out) {
    out = switube_fake::registered;
    return 0;
}
inline u64 armGetSystemTick() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}
#define serviceDispatch(s, c) switube_fake::noInput(c)
#define serviceDispatchIn(s, c, in) switube_fake::input(c, in)
#define serviceDispatchOut(s, c, out) switube_fake::output(c, out)
#define serviceDispatchInOut(s, c, in, out) switube_fake::item(c, in, out)
