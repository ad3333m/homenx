// Use libnx's real CMIF serialization with a simulated kernel/service boundary.
// The ARM TLS accessor remains unused; host tests supply the IPC buffer.
#include <switch/arm/tls.h>
alignas(16) unsigned char ipcBuffer[0x1000];
static void* testTls() { return ipcBuffer; }
#define armGetTls testTls
#include <switchu/software_move.hpp>
#undef armGetTls
#include <cassert>
#include <cstring>
#include <initializer_list>

namespace {
constexpr u64 kTitle = 0x0100000000001234;
constexpr Result kDenied = MAKERESULT(21, 2);
u32 firmware;
Result initializeResult, managerResult, estimateResult, storageResult, memoryResult, transportResult, serviceResult, completionResult;
int initialized, exited, legacy, requests, created, released, waited, closed;
s64 required, available;
NcmStorageId destination;

void reset() {
    firmware = MAKEHOSVERSION(22, 0, 0);
    initializeResult = managerResult = estimateResult = storageResult = memoryResult = transportResult = serviceResult = completionResult = 0;
    initialized = exited = legacy = requests = created = released = waited = closed = 0;
    required = 1024;
    available = 4096;
    destination = NcmStorageId_SdCard;
    std::memset(ipcBuffer, 0, sizeof(ipcBuffer));
}
}

extern "C" {
u32 hosversionGet() { return firmware; }
Result nsInitialize() { ++initialized; return initializeResult; }
void nsExit() { ++exited; }
Result nsMoveApplicationEntity(u64 id, NcmStorageId to) {
    assert(id == kTitle && to == destination);
    ++legacy;
    return completionResult;
}
Result nsEstimateSizeToMove(u8* sources, s32 count, NcmStorageId to, u32 flags, u64 id, s64* out) {
    assert(id == kTitle && to == destination && count == 1 && flags == 0);
    assert(sources[0] == (to == NcmStorageId_SdCard ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard));
    *out = required;
    return estimateResult;
}
Result nsGetStorageSize(NcmStorageId to, s64* total, s64* free) {
    assert(to == destination);
    *total = 8192;
    *free = available;
    return storageResult;
}
Result nsGetApplicationManagerInterface(Service* out) {
    if (R_FAILED(managerResult)) return managerResult;
    *out = {};
    out->session = 10;
    out->own_handle = 1;
    return 0;
}
Result tmemCreate(TransferMemory* memory, size_t size, Permission permission) {
    assert(size >= 0x1000 && size % 0x1000 == 0 && permission == Perm_None);
    if (R_FAILED(memoryResult)) return memoryResult;
    *memory = {};
    memory->handle = 20;
    memory->size = size;
    ++created;
    return 0;
}
Result tmemClose(TransferMemory*) {
    // The backing memory must stay alive until the async result is consumed
    // and its service/event handles are closed, including operation failures.
    if (R_SUCCEEDED(transportResult) && R_SUCCEEDED(serviceResult)) assert(waited == 1 && closed == 1);
    ++released;
    return 0;
}
void eventLoadRemote(Event* event, Handle handle, bool clear) {
    assert(handle == 30 && !clear);
    *event = {handle, INVALID_HANDLE, clear};
}
Result nsProgressAsyncResultGet(NsProgressAsyncResult* result) {
    assert(result->s.session == 40 && result->event.revent == 30 && released == 0);
    ++waited;
    return completionResult;
}
void nsProgressAsyncResultClose(NsProgressAsyncResult*) { assert(released == 0); ++closed; }
Result svcCloseHandle(Handle) { return 0; }
Result svcSendSyncRequest(Handle handle) {
    assert(handle == 10);
    const auto request = hipcParseRequest(ipcBuffer);
    if (request.meta.type == CmifCommandType_Close) return 0;
    ++requests;
    const auto* header = static_cast<const CmifInHeader*>(cmifGetAlignedDataStart(request.data.data_words, ipcBuffer));
    assert(header->magic == CMIF_IN_HEADER_MAGIC && header->command_id == 1310);
    struct Input { u8 to; u8 pad[3]; u32 flags; u64 title, size; } input;
    std::memcpy(&input, header + 1, sizeof(input));
    assert(input.to == destination && input.flags == 0 && input.title == kTitle && input.size == 0x100000);
    assert(request.meta.num_send_buffers == 1 && hipcGetBufferSize(request.data.send_buffers) == 1);
    assert(request.meta.num_copy_handles == 1 && request.data.copy_handles[0] == 20);
    if (R_FAILED(transportResult)) return transportResult;
    const bool success = R_SUCCEEDED(serviceResult);
    auto response = hipcMakeRequest(ipcBuffer, {.num_data_words = 8,
        .num_copy_handles = success ? 1u : 0u, .num_move_handles = success ? 1u : 0u});
    auto* output = static_cast<CmifOutHeader*>(cmifGetAlignedDataStart(response.data_words, ipcBuffer));
    *output = {CMIF_OUT_HEADER_MAGIC, 0, serviceResult, 0};
    if (success) { response.copy_handles[0] = 30; response.move_handles[0] = 40; }
    return 0;
}
}

int main() {
    using switchu::storage::moveApplication;
    for (const auto to : {NcmStorageId_SdCard, NcmStorageId_BuiltInUser}) {
        reset(); destination = to;
        assert(moveApplication(kTitle, to) == 0);
        assert(requests == 1 && waited == 1 && created == 1 && released == 1 && exited == 1);
    }
    reset(); initializeResult = kDenied;
    assert(moveApplication(kTitle, destination) == kDenied && requests == 0 && exited == 0);
    reset(); managerResult = kDenied;
    assert(moveApplication(kTitle, destination) == kDenied && created == 0 && exited == 1);
    reset(); estimateResult = kDenied;
    assert(moveApplication(kTitle, destination) == kDenied && requests == 0 && created == 0);
    reset(); storageResult = kDenied;
    assert(moveApplication(kTitle, destination) == kDenied && requests == 0 && created == 0);
    reset(); memoryResult = kDenied;
    assert(moveApplication(kTitle, destination) == kDenied && requests == 0 && released == 0 && exited == 1);
    reset(); available = required - 1;
    assert(R_FAILED(moveApplication(kTitle, destination)) && requests == 0 && created == 0);
    reset(); required = 0;
    assert(R_FAILED(moveApplication(kTitle, destination)) && requests == 0);
    reset(); transportResult = kDenied;
    assert(moveApplication(kTitle, destination) == kDenied && released == 1 && waited == 0);
    reset(); serviceResult = kDenied;
    assert(moveApplication(kTitle, destination) == kDenied && released == 1 && waited == 0);
    reset(); completionResult = kDenied;
    assert(moveApplication(kTitle, destination) == kDenied && waited == 1 && closed == 1 && released == 1);
    reset(); firmware = MAKEHOSVERSION(9, 2, 0);
    assert(moveApplication(kTitle, destination) == 0 && legacy == 1 && requests == 0 && exited == 1);
    reset();
    assert(R_FAILED(moveApplication(kTitle, NcmStorageId_BuiltInSystem)) && initialized == 0);
}
