#include "switube_service.hpp"

#include "ipc_server.h"
#include "switube/ipc_protocol.h"

#include <cstring>

namespace switube::phase2::switch_service {
namespace {

constexpr uint32_t ResultModule = 422;
SwitubeIpcServer server{};
ServiceCore* serviceCore = nullptr;

uint64_t monotonicMs() {
  return armTicksToNs(armGetSystemTick()) / UINT64_C(1000000);
}

Result controlResult(SwitubeControlResult result) {
  if (result == SwitubeControlResult_Ok) return 0;
  return MAKERESULT(ResultModule, (uint32_t)result);
}

template <typename T>
Result writeOutput(uint8_t* output, size_t* outputSize, const T& value) {
  if (sizeof(T) > SWITUBE_IPC_RESPONSE_MAX_DATA_SIZE)
    return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
  std::memcpy(output, &value, sizeof(value));
  *outputSize = sizeof(value);
  return 0;
}

bool readU32(const SwitubeIpcServerRequest* request, uint32_t& value) {
  if (request->data_size < sizeof(value) || !request->data) return false;
  std::memcpy(&value, request->data, sizeof(value));
  return true;
}

bool readBrowseRequest(const SwitubeIpcServerRequest* request,
                       SwitubeIpcBrowseRequest& value) {
  if (request->data_size < sizeof(value) || !request->data) return false;
  std::memcpy(&value, request->data, sizeof(value));
  value.query[sizeof(value.query) - 1] = '\0';
  return true;
}

Result handle(void*, const SwitubeIpcServerRequest* request, uint8_t* output,
              size_t* outputSize) {
  const auto command = static_cast<SwitubeIpcCommand>(request->command_id);
  switch (command) {
    case SwitubeIpcCommand_PlayOnlineTrack:
    case SwitubeIpcCommand_QueueOnlineTrack: {
      SwitubeIpcOnlineTrack track{};
      if (!request->data || request->data_size < sizeof(track))
        return controlResult(SwitubeControlResult_InvalidArgument);
      std::memcpy(&track, request->data, sizeof(track));
      track.id[sizeof(track.id)-1] = track.title[sizeof(track.title)-1] = track.artist[sizeof(track.artist)-1] = 0;
      return controlResult(serviceCore->submitBrowse(command, {}, monotonicMs(), track));
    }
    case SwitubeIpcCommand_Ping:
      return 0;
    case SwitubeIpcCommand_GetApiVersion: {
      const uint32_t version = SWITUBE_IPC_API_VERSION;
      return writeOutput(output, outputSize, version);
    }
    case SwitubeIpcCommand_GetCapabilities: {
      const uint64_t capabilities = serviceCore->capabilities();
      return writeOutput(output, outputSize, capabilities);
    }
    case SwitubeIpcCommand_GetStatus: {
      const SwitubeIpcStatus status = serviceCore->snapshot();
      return writeOutput(output, outputSize, status);
    }
    case SwitubeIpcCommand_GetBrowseHeader: {
      const SwitubeIpcBrowseHeader header = serviceCore->browseHeader();
      return writeOutput(output, outputSize, header);
    }
    case SwitubeIpcCommand_GetQueueItem:
    case SwitubeIpcCommand_GetBrowseItem: {
      uint32_t index = 0;
      SwitubeIpcBrowseItem item{};
      if (!readU32(request, index) || !(command == SwitubeIpcCommand_GetQueueItem ? serviceCore->queueItem(index, item) : serviceCore->browseItem(index, item)))
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
      return writeOutput(output, outputSize, item);
    }
    default:
      break;
  }

  if ((command >= SwitubeIpcCommand_BrowseHome &&
       command <= SwitubeIpcCommand_PlayQueueItem) || command == SwitubeIpcCommand_ListPlaylists ||
      command == SwitubeIpcCommand_SavePlaylist) {
    SwitubeIpcBrowseRequest browseRequest{};
    if (!readBrowseRequest(request, browseRequest))
      return controlResult(SwitubeControlResult_InvalidArgument);
    return controlResult(
        serviceCore->submitBrowse(command, browseRequest, monotonicMs()));
  }

  uint32_t value = 0;
  if (command == SwitubeIpcCommand_SetVolume ||
      command == SwitubeIpcCommand_Seek ||
      command == SwitubeIpcCommand_SetShuffle ||
      command == SwitubeIpcCommand_SetRepeatMode ||
      command == SwitubeIpcCommand_SetAudioQuality) {
    if (!readU32(request, value))
      return controlResult(SwitubeControlResult_InvalidArgument);
  }
  return controlResult(serviceCore->submit(command, value, monotonicMs()));
}

}  // namespace

Result initialize(ServiceCore& core, const char* name) {
  const Result rc = switubeIpcServerInitialize(&server, name, MaxWaiters - 1);
  if (R_SUCCEEDED(rc)) serviceCore = &core;
  return rc;
}

Result exit() {
  serviceCore = nullptr;
  return switubeIpcServerExit(&server);
}

int appendWaiters(Waiter* waiters) {
  for (uint32_t i = 0; i < server.handle_count; ++i)
    waiters[i] = waiterForHandle(server.handles[i]);
  return server.handle_count;
}

void poll() {
  switubeIpcServerProcess(&server, handle, nullptr, 0);
}

}  // namespace switube::phase2::switch_service
