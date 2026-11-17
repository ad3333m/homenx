#pragma once

// Stable C ABI between the Switube background sysmodule and its independent
// overlay (plus developer diagnostics). The standalone NRO does not depend on
// this runtime connection.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITUBE_IPC_SERVICE_NAME "switube"

#define SWITUBE_IPC_API_VERSION_MAJOR 1u
#define SWITUBE_IPC_API_VERSION_MINOR 0u
#define SWITUBE_IPC_API_VERSION \
    ((SWITUBE_IPC_API_VERSION_MAJOR << 16u) | SWITUBE_IPC_API_VERSION_MINOR)

#define SWITUBE_IPC_STATUS_ABI_VERSION 1u
#define SWITUBE_IPC_STATUS_SIZE 216u
#define SWITUBE_IPC_INVALID_INDEX UINT32_MAX

typedef enum SwitubeIpcCommand {
    SwitubeIpcCommand_GetApiVersion = 0,
    SwitubeIpcCommand_GetCapabilities = 1,
    SwitubeIpcCommand_GetStatus = 2,
    SwitubeIpcCommand_Ping = 3,

    SwitubeIpcCommand_PlayPause = 10,
    SwitubeIpcCommand_Play = 11,
    SwitubeIpcCommand_Pause = 12,
    SwitubeIpcCommand_Next = 13,
    SwitubeIpcCommand_Previous = 14,
    SwitubeIpcCommand_SetVolume = 15,
    SwitubeIpcCommand_Seek = 16,
    SwitubeIpcCommand_SetShuffle = 17,
    SwitubeIpcCommand_SetRepeatMode = 18,
    SwitubeIpcCommand_SetAudioQuality = 19,

    SwitubeIpcCommand_GetBrowseHeader = 20,
    SwitubeIpcCommand_GetBrowseItem = 21,

    SwitubeIpcCommand_BrowseHome = 30,
    SwitubeIpcCommand_BrowseLibrary = 31,
    SwitubeIpcCommand_BrowseSearch = 32,
    SwitubeIpcCommand_BrowseOpen = 33,
    SwitubeIpcCommand_BrowseBack = 34,
    SwitubeIpcCommand_BrowsePlay = 35,
    SwitubeIpcCommand_BrowseCurrentPlaylist = 36,
    SwitubeIpcCommand_BrowseQueue = 37,
} SwitubeIpcCommand;

typedef enum SwitubeIpcCapability {
    SwitubeIpcCapability_Status = UINT64_C(1) << 0u,
    SwitubeIpcCapability_PlaybackControl = UINT64_C(1) << 1u,
    SwitubeIpcCapability_Seek = UINT64_C(1) << 2u,
    SwitubeIpcCapability_Queue = UINT64_C(1) << 3u,
    SwitubeIpcCapability_Browsing = UINT64_C(1) << 5u,
    SwitubeIpcCapability_Search = UINT64_C(1) << 6u,
    SwitubeIpcCapability_AudioSettings = UINT64_C(1) << 7u,
} SwitubeIpcCapability;

typedef enum SwitubeServiceState {
    SwitubeServiceState_Booting = 0,
    SwitubeServiceState_Connecting = 3,
    SwitubeServiceState_Ready = 4,
    SwitubeServiceState_Error = 5,
    SwitubeServiceState_Stopping = 6,
} SwitubeServiceState;

typedef enum SwitubePlaybackState {
    SwitubePlaybackState_Stopped = 0,
    SwitubePlaybackState_Buffering = 1,
    SwitubePlaybackState_Playing = 2,
    SwitubePlaybackState_Paused = 3,
} SwitubePlaybackState;

typedef enum SwitubeRepeatMode {
    SwitubeRepeatMode_Off = 0,
    SwitubeRepeatMode_One = 1,
    SwitubeRepeatMode_All = 2,
} SwitubeRepeatMode;

typedef enum SwitubeStatusFlag {
    SwitubeStatusFlag_Connected = 1u << 0u,
    SwitubeStatusFlag_Shuffle = 1u << 2u,
    SwitubeStatusFlag_RepeatOne = 1u << 3u,
    SwitubeStatusFlag_RepeatAll = 1u << 4u,
    SwitubeStatusFlag_CanSeek = 1u << 5u,
    SwitubeStatusFlag_HasTrack = 1u << 6u,
    SwitubeStatusFlag_HighQuality = 1u << 7u,
} SwitubeStatusFlag;

typedef enum SwitubeControlResult {
    SwitubeControlResult_Ok = 0,
    SwitubeControlResult_InvalidCommand = 1,
    SwitubeControlResult_InvalidArgument = 2,
    SwitubeControlResult_BackendUnavailable = 3,
    SwitubeControlResult_Busy = 4,
} SwitubeControlResult;

typedef enum SwitubeBrowseState {
    SwitubeBrowseState_Idle = 0,
    SwitubeBrowseState_Loading = 1,
    SwitubeBrowseState_Ready = 2,
    SwitubeBrowseState_Error = 3,
} SwitubeBrowseState;

typedef enum SwitubeBrowseItemKind {
    SwitubeBrowseItemKind_Section = 0,
    SwitubeBrowseItemKind_Track = 1,
    SwitubeBrowseItemKind_Playlist = 2,
    SwitubeBrowseItemKind_Album = 3,
    SwitubeBrowseItemKind_Artist = 4,
} SwitubeBrowseItemKind;

#define SWITUBE_IPC_BROWSE_ABI_VERSION 1u
#define SWITUBE_IPC_BROWSE_HEADER_SIZE 216u
#define SWITUBE_IPC_BROWSE_ITEM_SIZE 216u
#define SWITUBE_IPC_BROWSE_QUERY_SIZE 128u

typedef struct SwitubeIpcBrowseHeader {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t generation;
    uint32_t state;
    uint32_t depth;
    uint32_t item_count;
    uint32_t flags;
    char title[64];
    char subtitle[64];
    char message[56];
} SwitubeIpcBrowseHeader;

typedef struct SwitubeIpcBrowseItem {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t generation;
    uint32_t kind;
    uint32_t index;
    uint32_t duration_ms;
    uint32_t flags;
    char name[80];
    char subtitle[64];
    char detail[40];
} SwitubeIpcBrowseItem;

typedef struct SwitubeIpcBrowseRequest {
    uint64_t generation;
    uint32_t index;
    uint32_t reserved;
    char query[SWITUBE_IPC_BROWSE_QUERY_SIZE];
} SwitubeIpcBrowseRequest;

// Exactly one TLS-backed inline CMIF response (216 bytes).
typedef struct SwitubeIpcStatus {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t sequence;
    uint64_t monotonic_ms;
    uint32_t service_state;
    uint32_t playback_state;
    uint32_t flags;
    uint32_t position_ms;
    uint32_t duration_ms;
    uint32_t volume_permille;
    uint32_t queue_index;
    uint32_t queue_count;
    uint32_t last_result;
    uint32_t reserved0;
    char title[64];
    char artist[48];
    char album[40];
} SwitubeIpcStatus;

#ifdef __cplusplus
}

static_assert(sizeof(SwitubeIpcStatus) == SWITUBE_IPC_STATUS_SIZE,
              "Switube IPC status ABI changed");
static_assert(sizeof(SwitubeIpcBrowseHeader) == SWITUBE_IPC_BROWSE_HEADER_SIZE,
              "Switube browse header ABI changed");
static_assert(sizeof(SwitubeIpcBrowseItem) == SWITUBE_IPC_BROWSE_ITEM_SIZE,
              "Switube browse item ABI changed");
static_assert(sizeof(SwitubeIpcBrowseRequest) == 144,
              "Switube browse request ABI changed");
#endif
