// From cristowo/SwitchU (branch switify-integration, 7199904), GPL-3.0:
// the client for Switify's background service (github.com/DomazinUS/Switify).
#pragma once

// Wire format of the "switify" service published by the Switify background
// sysmodule (github.com/DomazinUS/Switify, phase2/common/include/switify/
// ipc_protocol.h, API 1.2). Kept free of libnx so the host test can check it.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace switchu::switify {

inline constexpr const char* kServiceName = "switify";
inline constexpr std::uint64_t kProgramId = 0x4200000000535754ULL;
inline constexpr const char* kExefsPath =
    "sdmc:/atmosphere/contents/4200000000535754/exefs.nsp";
inline constexpr const char* kArtworkUrlPath = "sdmc:/config/Switify/current-artwork-url";
inline constexpr const char* kArtworkCacheDir = "sdmc:/config/Switify/artwork-cache";
inline constexpr const char* kOwnCoverCacheDir = "sdmc:/config/SwitchU/cache/switify";

enum class IpcCommand : std::uint32_t {
    GetStatus = 2,
    PlayPause = 10,
    Play = 11,
    Pause = 12,
    Next = 13,
    Previous = 14,
    SetVolume = 15,
    Seek = 16,
    SetShuffle = 17,
    SetRepeatMode = 18,
    GetBrowseHeader = 20,
    GetBrowseItem = 21,
    BrowseHome = 30,
    BrowseLibrary = 31,
    BrowseSearch = 32,
    BrowseOpen = 33,
    BrowseBack = 34,
    BrowsePlay = 35,
    BrowseCurrentPlaylist = 36,
    BrowseQueue = 37,
    GetAudioLevels = 38, // our Switify fork only; official builds reject it
    ReloadCurrent = 39,  // same
};

inline constexpr std::uint32_t kStatusFlagConnected = 1u << 0;
inline constexpr std::uint32_t kStatusFlagShuffle = 1u << 2;
inline constexpr std::uint32_t kStatusFlagRepeatOne = 1u << 3;
inline constexpr std::uint32_t kStatusFlagRepeatAll = 1u << 4;
inline constexpr std::uint32_t kStatusFlagCanSeek = 1u << 5;
inline constexpr std::uint32_t kStatusFlagHasTrack = 1u << 6;
// SwitchU's Switify only: the song waits to be loaded again (after a sleep).
inline constexpr std::uint32_t kStatusFlagReloading = 1u << 8;

inline constexpr std::uint32_t kBrowseIdle = 0;
inline constexpr std::uint32_t kBrowseLoading = 1;
inline constexpr std::uint32_t kBrowseReady = 2;
inline constexpr std::uint32_t kBrowseError = 3;
inline constexpr std::uint32_t kBrowseKindSection = 0;
inline constexpr std::uint32_t kBrowseKindTrack = 1;

// SwitifyIpcStatus, status ABI 1.
struct IpcStatus {
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    std::uint64_t sequence;
    std::uint64_t monotonicMs;
    std::uint32_t serviceState;
    std::uint32_t playbackState;
    std::uint32_t flags;
    std::uint32_t positionMs;
    std::uint32_t durationMs;
    std::uint32_t volumePermille;
    std::uint32_t queueIndex;
    std::uint32_t queueCount;
    std::uint32_t lastResult;
    std::uint32_t reserved0;
    char title[64];
    char artist[48];
    char album[40];
};
static_assert(sizeof(IpcStatus) == 216);
static_assert(offsetof(IpcStatus, title) == 64);
static_assert(offsetof(IpcStatus, album) == 176);

// SwitifyIpcBrowseHeader / Item / Request, browse ABI 1.
struct IpcBrowseHeader {
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    std::uint64_t generation;
    std::uint32_t state;
    std::uint32_t depth;
    std::uint32_t itemCount;
    std::uint32_t flags;
    char title[64];
    char subtitle[64];
    char message[56];
};
static_assert(sizeof(IpcBrowseHeader) == 216);
static_assert(offsetof(IpcBrowseHeader, message) == 160);

struct IpcBrowseItem {
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    std::uint64_t generation;
    std::uint32_t kind;
    std::uint32_t index;
    std::uint32_t durationMs;
    std::uint32_t flags;
    char name[80];
    char subtitle[64];
    char detail[40];
};
static_assert(sizeof(IpcBrowseItem) == 216);
static_assert(offsetof(IpcBrowseItem, detail) == 176);

struct IpcBrowseRequest {
    std::uint64_t generation;
    std::uint32_t index;
    std::uint32_t reserved;
    char query[128];
};
static_assert(sizeof(IpcBrowseRequest) == 144);

// SwitchU's MusicRepeatMode is Off=0, All=1, One=2; Switify's is Off=0, One=1, All=2.
inline constexpr std::uint32_t switifyRepeatFromMenu(int menuMode) {
    return menuMode == 1 ? 2u : menuMode == 2 ? 1u : 0u;
}

inline constexpr int menuRepeatFromFlags(std::uint32_t flags) {
    return (flags & kStatusFlagRepeatAll) ? 1 : (flags & kStatusFlagRepeatOne) ? 2 : 0;
}

inline std::uint64_t fnv1a64(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::string hashName(const std::string& url) {
    char encoded[17]{};
    std::snprintf(encoded, sizeof(encoded), "%016llx",
                  static_cast<unsigned long long>(fnv1a64(url)));
    return encoded;
}

// Where the Switify overlay caches a cover JPEG for this URL.
inline std::string switifyCoverPath(const std::string& url) {
    return std::string(kArtworkCacheDir) + "/" + hashName(url) + ".img";
}

// Where SwitchU caches a cover it had to download itself.
inline std::string ownCoverPath(const std::string& url) {
    return std::string(kOwnCoverCacheDir) + "/" + hashName(url) + ".jpg";
}

template <std::size_t N>
std::string boundedString(const char (&value)[N]) {
    std::size_t length = 0;
    while (length < N && value[length] != '\0') ++length;
    return std::string(value, length);
}

} // namespace switchu::switify
