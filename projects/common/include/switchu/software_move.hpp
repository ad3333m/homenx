#pragma once
extern "C" {
#include <switch/services/ns.h>
#include <switch/runtime/hosversion.h>
}

namespace switchu::storage {

// Modern NS command 1310 (10.0.0+). NS owns the migration and consistency of
// application records; save data is deliberately not part of this operation.
inline Result moveApplication(u64 title, NcmStorageId destination) {
    if (destination != NcmStorageId_SdCard && destination != NcmStorageId_BuiltInUser)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    const Result initialized = nsInitialize();
    if (R_FAILED(initialized)) return initialized;
    struct Exit { ~Exit() { nsExit(); } } exit;
    if (hosversionBefore(10, 0, 0)) return nsMoveApplicationEntity(title, destination);

    u8 source = destination == NcmStorageId_SdCard ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
    s64 required = 0, total = 0, free = 0;
    Result rc = nsEstimateSizeToMove(&source, 1, destination, 0, title, &required);
    if (R_FAILED(rc)) return rc;
    rc = nsGetStorageSize(destination, &total, &free);
    if (R_FAILED(rc)) return rc;
    if (required <= 0 || free < required) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Service manager{};
    rc = nsGetApplicationManagerInterface(&manager);
    if (R_FAILED(rc)) return rc;
    TransferMemory memory{};
    constexpr u64 size = 0x100000;
    rc = tmemCreate(&memory, size, Perm_None);
    if (R_FAILED(rc)) { serviceClose(&manager); return rc; }

    struct {
        u8 destination;
        u8 padding[3];
        u32 flags;
        u64 title;
        u64 memorySize;
    } input{static_cast<u8>(destination), {}, 0, title, size};
    static_assert(sizeof(input) == 0x18);
    NsProgressAsyncResult operation{};
    Handle event = INVALID_HANDLE;
    rc = serviceDispatchIn(&manager, 1310, input,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = {{&source, sizeof(source)}},
        .in_num_handles = 1, .in_handles = {memory.handle},
        .out_num_objects = 1, .out_objects = &operation.s,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy }, .out_handles = &event);
    if (R_SUCCEEDED(rc)) {
        eventLoadRemote(&operation.event, event, false);
        rc = nsProgressAsyncResultGet(&operation);
        nsProgressAsyncResultClose(&operation);
    }
    tmemClose(&memory);
    serviceClose(&manager);
    return rc;
}
}
