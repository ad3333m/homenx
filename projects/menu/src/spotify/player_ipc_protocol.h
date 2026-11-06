// The IPC ABI (1.2) of the console's background Spotify player: stable and
// append-only. Keep it in sync with the player's own copy, never edit values here.
#pragma once

// Stable C ABI between the Spotify player (a background sysmodule) and its independent
// overlay (plus developer diagnostics). The standalone NRO does not depend on
// this runtime connection.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYER_IPC_SERVICE_NAME "spotplay"

#define PLAYER_IPC_API_VERSION_MAJOR 1u
#define PLAYER_IPC_API_VERSION_MINOR 2u
#define PLAYER_IPC_API_VERSION \
    ((PLAYER_IPC_API_VERSION_MAJOR << 16u) | PLAYER_IPC_API_VERSION_MINOR)

#define PLAYER_IPC_STATUS_ABI_VERSION 1u
#define PLAYER_IPC_STATUS_SIZE 216u
#define PLAYER_IPC_INVALID_INDEX UINT32_MAX

typedef enum PlayerIpcCommand {
    PlayerIpcCommand_GetApiVersion = 0,
    PlayerIpcCommand_GetCapabilities = 1,
    PlayerIpcCommand_GetStatus = 2,
    PlayerIpcCommand_Ping = 3,

    PlayerIpcCommand_PlayPause = 10,
    PlayerIpcCommand_Play = 11,
    PlayerIpcCommand_Pause = 12,
    PlayerIpcCommand_Next = 13,
    PlayerIpcCommand_Previous = 14,
    PlayerIpcCommand_SetVolume = 15,
    PlayerIpcCommand_Seek = 16,
    PlayerIpcCommand_SetShuffle = 17,
    PlayerIpcCommand_SetRepeatMode = 18,
    PlayerIpcCommand_SetAudioQuality = 19,

    PlayerIpcCommand_GetBrowseHeader = 20,
    PlayerIpcCommand_GetBrowseItem = 21,

    PlayerIpcCommand_BrowseHome = 30,
    PlayerIpcCommand_BrowseLibrary = 31,
    PlayerIpcCommand_BrowseSearch = 32,
    PlayerIpcCommand_BrowseOpen = 33,
    PlayerIpcCommand_BrowseBack = 34,
    PlayerIpcCommand_BrowsePlay = 35,
    PlayerIpcCommand_BrowseCurrentPlaylist = 36,
    PlayerIpcCommand_BrowseQueue = 37,
} PlayerIpcCommand;

typedef enum PlayerIpcCapability {
    PlayerIpcCapability_Status = UINT64_C(1) << 0u,
    PlayerIpcCapability_PlaybackControl = UINT64_C(1) << 1u,
    PlayerIpcCapability_Seek = UINT64_C(1) << 2u,
    PlayerIpcCapability_Queue = UINT64_C(1) << 3u,
    PlayerIpcCapability_Authentication = UINT64_C(1) << 4u,
    PlayerIpcCapability_Browsing = UINT64_C(1) << 5u,
    PlayerIpcCapability_Search = UINT64_C(1) << 6u,
    PlayerIpcCapability_AudioSettings = UINT64_C(1) << 7u,
} PlayerIpcCapability;

typedef enum PlayerServiceState {
    PlayerServiceState_Booting = 0,
    PlayerServiceState_WaitingForCredentials = 1,
    PlayerServiceState_Pairing = 2,
    PlayerServiceState_Connecting = 3,
    PlayerServiceState_Ready = 4,
    PlayerServiceState_Error = 5,
    PlayerServiceState_Stopping = 6,
} PlayerServiceState;

typedef enum PlayerPlaybackState {
    PlayerPlaybackState_Stopped = 0,
    PlayerPlaybackState_Buffering = 1,
    PlayerPlaybackState_Playing = 2,
    PlayerPlaybackState_Paused = 3,
} PlayerPlaybackState;

typedef enum PlayerRepeatMode {
    PlayerRepeatMode_Off = 0,
    PlayerRepeatMode_One = 1,
    PlayerRepeatMode_All = 2,
} PlayerRepeatMode;

typedef enum PlayerStatusFlag {
    PlayerStatusFlag_Connected = 1u << 0u,
    PlayerStatusFlag_HasCredentials = 1u << 1u,
    PlayerStatusFlag_Shuffle = 1u << 2u,
    PlayerStatusFlag_RepeatOne = 1u << 3u,
    PlayerStatusFlag_RepeatAll = 1u << 4u,
    PlayerStatusFlag_CanSeek = 1u << 5u,
    PlayerStatusFlag_HasTrack = 1u << 6u,
    PlayerStatusFlag_HighQuality = 1u << 7u,
} PlayerStatusFlag;

typedef enum PlayerControlResult {
    PlayerControlResult_Ok = 0,
    PlayerControlResult_InvalidCommand = 1,
    PlayerControlResult_InvalidArgument = 2,
    PlayerControlResult_BackendUnavailable = 3,
    PlayerControlResult_Busy = 4,
} PlayerControlResult;

typedef enum PlayerBrowseState {
    PlayerBrowseState_Idle = 0,
    PlayerBrowseState_Loading = 1,
    PlayerBrowseState_Ready = 2,
    PlayerBrowseState_Error = 3,
} PlayerBrowseState;

typedef enum PlayerBrowseItemKind {
    PlayerBrowseItemKind_Section = 0,
    PlayerBrowseItemKind_Track = 1,
    PlayerBrowseItemKind_Playlist = 2,
    PlayerBrowseItemKind_Album = 3,
    PlayerBrowseItemKind_Artist = 4,
} PlayerBrowseItemKind;

#define PLAYER_IPC_BROWSE_ABI_VERSION 1u
#define PLAYER_IPC_BROWSE_HEADER_SIZE 216u
#define PLAYER_IPC_BROWSE_ITEM_SIZE 216u
#define PLAYER_IPC_BROWSE_QUERY_SIZE 128u

typedef struct PlayerIpcBrowseHeader {
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
} PlayerIpcBrowseHeader;

typedef struct PlayerIpcBrowseItem {
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
} PlayerIpcBrowseItem;

typedef struct PlayerIpcBrowseRequest {
    uint64_t generation;
    uint32_t index;
    uint32_t reserved;
    char query[PLAYER_IPC_BROWSE_QUERY_SIZE];
} PlayerIpcBrowseRequest;

// Exactly one TLS-backed inline CMIF response (216 bytes). Adding fields
// requires a new
// status ABI version; old fields must retain their offsets.
typedef struct PlayerIpcStatus {
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
} PlayerIpcStatus;

#ifdef __cplusplus
}

static_assert(sizeof(PlayerIpcStatus) == PLAYER_IPC_STATUS_SIZE,
              "Spotify player IPC status ABI changed");
static_assert(sizeof(PlayerIpcBrowseHeader) == PLAYER_IPC_BROWSE_HEADER_SIZE,
              "Spotify player browse header ABI changed");
static_assert(sizeof(PlayerIpcBrowseItem) == PLAYER_IPC_BROWSE_ITEM_SIZE,
              "Spotify player browse item ABI changed");
static_assert(sizeof(PlayerIpcBrowseRequest) == 144,
              "Spotify player browse request ABI changed");
#endif
