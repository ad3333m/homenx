#pragma once

#include "switube/ipc_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace switube::phase2 {

struct ControlMessage {
  SwitubeIpcCommand command = SwitubeIpcCommand_PlayPause;
  uint32_t value = 0;
  uint64_t sequence = 0;
  uint64_t generation = 0;
  std::array<char, SWITUBE_IPC_BROWSE_QUERY_SIZE> query{};
};

class ServiceCore {
 public:
  static constexpr size_t CommandCapacity = 16;

  ServiceCore();

  SwitubeIpcStatus snapshot() const;
  uint64_t capabilities() const;
  SwitubeIpcBrowseHeader browseHeader() const;
  bool browseItem(uint32_t index, SwitubeIpcBrowseItem& item) const;

  void publishServiceState(SwitubeServiceState state, uint64_t monotonicMs);
  void publishPlaybackState(SwitubePlaybackState state, uint32_t positionMs,
                            uint32_t durationMs, uint64_t monotonicMs);
  void publishTrack(std::string_view title, std::string_view artist,
                    std::string_view album, uint64_t monotonicMs);
  void clearTrack(uint64_t monotonicMs);
  void publishQueue(uint32_t index, uint32_t count, uint64_t monotonicMs);
  void publishVolume(uint32_t volumePermille, uint64_t monotonicMs);
  void publishConnection(bool connected, uint64_t monotonicMs);
  void publishSeekable(bool seekable, uint64_t monotonicMs);
  void publishResult(SwitubeControlResult result, uint64_t monotonicMs);
  uint64_t beginBrowse(std::string_view title, std::string_view subtitle,
                       uint32_t depth, uint64_t monotonicMs);
  bool publishBrowse(uint64_t generation, std::string_view title,
                     std::string_view subtitle,
                     const SwitubeIpcBrowseItem* items, size_t itemCount,
                     uint32_t depth, uint64_t monotonicMs);
  bool publishBrowseError(uint64_t generation, std::string_view message,
                          uint64_t monotonicMs);

  void configureCommands(bool accepting, uint64_t capabilities,
                         uint64_t monotonicMs);
  SwitubeControlResult submit(SwitubeIpcCommand command, uint32_t value,
                              uint64_t monotonicMs);
  SwitubeControlResult submitBrowse(SwitubeIpcCommand command,
                                    const SwitubeIpcBrowseRequest& request,
                                    uint64_t monotonicMs);
  bool tryPop(ControlMessage& message);

 private:
  static bool isControlCommand(SwitubeIpcCommand command);
  static SwitubeControlResult validate(SwitubeIpcCommand command,
                                       uint32_t value);
  static void copyText(char* destination, size_t capacity,
                       std::string_view text);
  void changedLocked(uint64_t monotonicMs);
  void setFlagLocked(uint32_t flag, bool enabled);

  mutable std::mutex mutex;
  SwitubeIpcStatus status{};
  uint64_t advertisedCapabilities = SwitubeIpcCapability_Status;
  bool acceptingCommands = false;
  std::array<ControlMessage, CommandCapacity> commands{};
  size_t commandHead = 0;
  size_t commandCount = 0;
  uint64_t nextCommandSequence = 1;
  SwitubeIpcBrowseHeader browser{};
  std::array<SwitubeIpcBrowseItem, 32> browseItems{};
  uint64_t nextBrowseGeneration = 1;
};

}  // namespace switube::phase2
