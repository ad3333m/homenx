#include "service_core.hpp"

#include <algorithm>
#include <cstring>

namespace switube::phase2 {

ServiceCore::ServiceCore() {
  status.struct_size = sizeof(status);
  status.abi_version = SWITUBE_IPC_STATUS_ABI_VERSION;
  status.service_state = SwitubeServiceState_Booting;
  status.playback_state = SwitubePlaybackState_Stopped;
  status.volume_permille = 800;
  status.queue_index = SWITUBE_IPC_INVALID_INDEX;
  status.last_result = SwitubeControlResult_BackendUnavailable;
  browser.struct_size = sizeof(browser);
  browser.abi_version = SWITUBE_IPC_BROWSE_ABI_VERSION;
  browser.state = SwitubeBrowseState_Idle;
}

SwitubeIpcStatus ServiceCore::snapshot() const {
  std::scoped_lock lock(mutex);
  return status;
}

uint64_t ServiceCore::capabilities() const {
  std::scoped_lock lock(mutex);
  return advertisedCapabilities;
}

SwitubeIpcBrowseHeader ServiceCore::browseHeader() const {
  std::scoped_lock lock(mutex);
  return browser;
}

bool ServiceCore::browseItem(uint32_t index, SwitubeIpcBrowseItem& item) const {
  std::scoped_lock lock(mutex);
  if (browser.state != SwitubeBrowseState_Ready ||
      index >= browser.item_count || index >= browseItems.size())
    return false;
  item = browseItems[index];
  return true;
}

void ServiceCore::changedLocked(uint64_t monotonicMs) {
  ++status.sequence;
  status.monotonic_ms = monotonicMs;
}

void ServiceCore::setFlagLocked(uint32_t flag, bool enabled) {
  if (enabled)
    status.flags |= flag;
  else
    status.flags &= ~flag;
}

void ServiceCore::publishServiceState(SwitubeServiceState state,
                                      uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  status.service_state = state;
  changedLocked(monotonicMs);
}

void ServiceCore::publishPlaybackState(SwitubePlaybackState state,
                                       uint32_t positionMs,
                                       uint32_t durationMs,
                                       uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  status.playback_state = state;
  status.position_ms = positionMs;
  status.duration_ms = durationMs;
  changedLocked(monotonicMs);
}

void ServiceCore::copyText(char* destination, size_t capacity,
                           std::string_view text) {
  if (capacity == 0) return;
  const size_t length = std::min(capacity - 1, text.size());
  if (length != 0) std::memcpy(destination, text.data(), length);
  destination[length] = '\0';
  if (length + 1 < capacity)
    std::memset(destination + length + 1, 0, capacity - length - 1);
}

void ServiceCore::publishTrack(std::string_view title,
                               std::string_view artist,
                               std::string_view album,
                               uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  copyText(status.title, sizeof(status.title), title);
  copyText(status.artist, sizeof(status.artist), artist);
  copyText(status.album, sizeof(status.album), album);
  setFlagLocked(SwitubeStatusFlag_HasTrack, !title.empty());
  changedLocked(monotonicMs);
}

void ServiceCore::clearTrack(uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  std::memset(status.title, 0, sizeof(status.title));
  std::memset(status.artist, 0, sizeof(status.artist));
  std::memset(status.album, 0, sizeof(status.album));
  setFlagLocked(SwitubeStatusFlag_HasTrack, false);
  status.position_ms = 0;
  status.duration_ms = 0;
  changedLocked(monotonicMs);
}

void ServiceCore::publishQueue(uint32_t index, uint32_t count,
                               uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  status.queue_index = index;
  status.queue_count = count;
  changedLocked(monotonicMs);
}

void ServiceCore::replaceQueue(std::vector<SwitubeIpcBrowseItem> items, uint32_t index, uint64_t now) {
  std::scoped_lock lock(mutex);
  queueItems = std::move(items);
  ++status.reserved0; // Queue revision for clients with the Queue capability.
  status.queue_index = index;
  status.queue_count = queueItems.size();
  changedLocked(now);
}

bool ServiceCore::queueItem(uint32_t index, SwitubeIpcBrowseItem& item) const {
  std::scoped_lock lock(mutex);
  if (index >= queueItems.size()) return false;
  item = queueItems[index];
  item.generation = status.reserved0;
  return true;
}

void ServiceCore::publishPlaybackOptions(bool shuffle, uint32_t repeat, uint64_t now) {
  std::scoped_lock lock(mutex);
  setFlagLocked(SwitubeStatusFlag_Shuffle, shuffle);
  setFlagLocked(SwitubeStatusFlag_RepeatOne, repeat == SwitubeRepeatMode_One);
  setFlagLocked(SwitubeStatusFlag_RepeatAll, repeat == SwitubeRepeatMode_All);
  changedLocked(now);
}

void ServiceCore::publishVolume(uint32_t volumePermille, uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  status.volume_permille = std::min(volumePermille, 1000u);
  changedLocked(monotonicMs);
}

void ServiceCore::publishConnection(bool connected, uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  setFlagLocked(SwitubeStatusFlag_Connected, connected);
  changedLocked(monotonicMs);
}

void ServiceCore::publishSeekable(bool seekable, uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  setFlagLocked(SwitubeStatusFlag_CanSeek, seekable);
  changedLocked(monotonicMs);
}

void ServiceCore::publishResult(SwitubeControlResult result,
                                uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  status.last_result = result;
  changedLocked(monotonicMs);
}

uint64_t ServiceCore::beginBrowse(std::string_view title,
                                  std::string_view subtitle, uint32_t depth,
                                  uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  const uint64_t generation = nextBrowseGeneration++;
  browser.struct_size = sizeof(browser);
  browser.abi_version = SWITUBE_IPC_BROWSE_ABI_VERSION;
  browser.generation = generation;
  browser.state = SwitubeBrowseState_Loading;
  browser.depth = depth;
  browser.item_count = 0;
  browser.flags = 0;
  copyText(browser.title, sizeof(browser.title), title);
  copyText(browser.subtitle, sizeof(browser.subtitle), subtitle);
  std::memset(browser.message, 0, sizeof(browser.message));
  changedLocked(monotonicMs);
  return generation;
}

bool ServiceCore::publishBrowse(uint64_t generation, std::string_view title,
                                std::string_view subtitle,
                                const SwitubeIpcBrowseItem* items,
                                size_t itemCount, uint32_t depth,
                                uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  if (browser.generation != generation) return false;
  browser.struct_size = sizeof(browser);
  browser.abi_version = SWITUBE_IPC_BROWSE_ABI_VERSION;
  browser.generation = generation;
  browser.state = SwitubeBrowseState_Ready;
  browser.depth = depth;
  browser.item_count =
      static_cast<uint32_t>(std::min(itemCount, browseItems.size()));
  browser.flags = 0;
  copyText(browser.title, sizeof(browser.title), title);
  copyText(browser.subtitle, sizeof(browser.subtitle), subtitle);
  std::memset(browser.message, 0, sizeof(browser.message));
  for (size_t i = 0; i < browser.item_count; ++i) {
    browseItems[i] = items[i];
    browseItems[i].struct_size = sizeof(SwitubeIpcBrowseItem);
    browseItems[i].abi_version = SWITUBE_IPC_BROWSE_ABI_VERSION;
    browseItems[i].generation = generation;
    browseItems[i].index = static_cast<uint32_t>(i);
  }
  changedLocked(monotonicMs);
  return true;
}

bool ServiceCore::publishBrowseError(uint64_t generation,
                                     std::string_view message,
                                     uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  if (browser.generation != generation) return false;
  browser.state = SwitubeBrowseState_Error;
  browser.item_count = 0;
  copyText(browser.message, sizeof(browser.message), message);
  changedLocked(monotonicMs);
  return true;
}

void ServiceCore::configureCommands(bool accepting, uint64_t capabilities,
                                    uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  acceptingCommands = accepting;
  advertisedCapabilities = capabilities;
  changedLocked(monotonicMs);
}

bool ServiceCore::isControlCommand(SwitubeIpcCommand command) {
  switch (command) {
    case SwitubeIpcCommand_PlayPause:
    case SwitubeIpcCommand_Play:
    case SwitubeIpcCommand_Pause:
    case SwitubeIpcCommand_Next:
    case SwitubeIpcCommand_Previous:
    case SwitubeIpcCommand_SetVolume:
    case SwitubeIpcCommand_Seek:
    case SwitubeIpcCommand_SetShuffle:
    case SwitubeIpcCommand_SetRepeatMode:
    case SwitubeIpcCommand_SetAudioQuality:
      return true;
    default:
      return false;
  }
}

SwitubeControlResult ServiceCore::validate(SwitubeIpcCommand command,
                                           uint32_t value) {
  if (command == SwitubeIpcCommand_SetVolume && value > 1000)
    return SwitubeControlResult_InvalidArgument;
  return SwitubeControlResult_Ok;
}

SwitubeControlResult ServiceCore::submit(SwitubeIpcCommand command,
                                         uint32_t value,
                                         uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  if (!acceptingCommands) {
    status.last_result = SwitubeControlResult_BackendUnavailable;
    changedLocked(monotonicMs);
    return SwitubeControlResult_BackendUnavailable;
  }
  if (!isControlCommand(command)) {
    status.last_result = SwitubeControlResult_InvalidCommand;
    changedLocked(monotonicMs);
    return SwitubeControlResult_InvalidCommand;
  }
  const auto validation = validate(command, value);
  if (validation != SwitubeControlResult_Ok) {
    status.last_result = validation;
    changedLocked(monotonicMs);
    return validation;
  }
  if (commandCount == commands.size()) {
    status.last_result = SwitubeControlResult_Busy;
    changedLocked(monotonicMs);
    return SwitubeControlResult_Busy;
  }

  const size_t tail = (commandHead + commandCount) % commands.size();
  commands[tail] = {
      .command = command,
      .value = value,
      .sequence = nextCommandSequence++,
      .generation = 0,
      .query = {},
  };
  ++commandCount;
  status.last_result = SwitubeControlResult_Ok;
  changedLocked(monotonicMs);
  return SwitubeControlResult_Ok;
}

SwitubeControlResult ServiceCore::submitBrowse(
    SwitubeIpcCommand command, const SwitubeIpcBrowseRequest& request,
    uint64_t monotonicMs, const SwitubeIpcOnlineTrack& track) {
  std::scoped_lock lock(mutex);
  if (!acceptingCommands) {
    status.last_result = SwitubeControlResult_BackendUnavailable;
    changedLocked(monotonicMs);
    return SwitubeControlResult_BackendUnavailable;
  }
  if (commandCount == commands.size()) {
    status.last_result = SwitubeControlResult_Busy;
    changedLocked(monotonicMs);
    return SwitubeControlResult_Busy;
  }

  const size_t tail = (commandHead + commandCount) % commands.size();
  commands[tail] = {
      .command = command,
      .value = request.index,
      .sequence = nextCommandSequence++,
      .generation = request.generation,
      .query = {},
  };
  commands[tail].track = track;
  std::memcpy(commands[tail].query.data(), request.query,
              std::min(sizeof(commands[tail].query), sizeof(request.query)));
  commands[tail].query.back() = '\0';
  ++commandCount;
  status.last_result = SwitubeControlResult_Ok;
  changedLocked(monotonicMs);
  return SwitubeControlResult_Ok;
}

bool ServiceCore::tryPop(ControlMessage& message) {
  std::scoped_lock lock(mutex);
  if (commandCount == 0) return false;
  message = commands[commandHead];
  commandHead = (commandHead + 1) % commands.size();
  --commandCount;
  return true;
}

}  // namespace switube::phase2
