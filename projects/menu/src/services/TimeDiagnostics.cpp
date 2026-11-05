#include "TimeDiagnostics.hpp"
#include "TimeIpcDiagnostics.hpp"
#include "core/DebugLog.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {
Result __real_timeInitialize(void);
Result __real_smGetServiceWrapper(Service*, SmServiceName);
Result __real_svcSendSyncRequest(Handle);
void __real_shmemLoadRemote(SharedMemory*, Handle, size_t, Permission);
Result __real_shmemMap(SharedMemory*);
Result __real_svcMapSharedMemory(Handle, void*, size_t, u32);
#ifndef SWITCHU_HOMEBREW
void __libnx_init_time(void);
#endif
}

namespace {
// No wall clock, file I/O, dynamic allocation or logger singleton during startup.
char earlyLines[256][256];
size_t lineCount;
size_t droppedLines;
void* tracedTls;
Handle tracedService;

bool tracing() { return tracedTls == armGetTls(); }

void log(const char* format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    svcOutputDebugString(line, std::strlen(line));
    if (lineCount < 256)
        std::snprintf(earlyLines[lineCount++], sizeof(earlyLines[0]), "%s", line);
    else
        ++droppedLines;
}

void result(const char* step, Result rc) {
    log("[time-debug] %s Result=0x%08X (module=%u description=%u)",
        step, rc, R_MODULE(rc), R_DESCRIPTION(rc));
}

const char* commandName(u32 id) {
    switch (id) {
        case 0: return "GetStandardUserSystemClock";
        case 1: return "GetStandardNetworkSystemClock";
        case 2: return "GetStandardSteadyClock";
        case 3: return "GetTimeZoneService";
        case 4: return "GetStandardLocalSystemClock";
        case 20: return "GetSharedMemoryNativeHandle";
        default: return "other/control";
    }
}

void probeService(const char* name) {
    Service root{};
    log("[time-debug] independent probe %s: smGetService begin (access is process-specific)", name);
    Result rc = smGetService(&root, name);
    result(name, rc);
    if (R_FAILED(rc)) {
        log("[time-debug] %s unavailable to this process; skipping its IPC probes", name);
        return;
    }

    // Do not chain success conditions: a failed subservice must not hide others.
    for (u32 id = 0; id <= 4; ++id) {
        Service child{};
        log("[time-debug] %s IPC %u %s begin", name, id, commandName(id));
        rc = serviceDispatch(&root, id, .out_num_objects = 1, .out_objects = &child);
        result(commandName(id), rc);
        if (R_SUCCEEDED(rc)) {
            if (id == 2) {
                TimeSteadyClockTimePoint point{};
                log("[time-debug] %s ISteadyClock.GetCurrentTimePoint IPC 0 begin", name);
                rc = serviceDispatchOut(&child, 0, point);
                result("ISteadyClock.GetCurrentTimePoint", rc);
            } else if (id == 3) {
                TimeLocationName location{};
                log("[time-debug] %s ITimeZoneService.GetDeviceLocationName IPC 0 begin", name);
                rc = serviceDispatchOut(&child, 0, location);
                result("ITimeZoneService.GetDeviceLocationName", rc);
                struct {
                    TimeCalendarTime calendar;
                    TimeCalendarAdditionalInfo additional;
                } calendar{};
                const u64 timestamp = 0; // Fixed input, independent of any clock read.
                log("[time-debug] %s ITimeZoneService.ToCalendarTimeWithMyRule IPC 101 begin (timestamp=0)", name);
                rc = serviceDispatchInOut(&child, 101, timestamp, calendar);
                result("ITimeZoneService.ToCalendarTimeWithMyRule", rc);
            } else {
                u64 timestamp = 0;
                log("[time-debug] %s %s.GetCurrentTime IPC 0 begin", name, commandName(id));
                rc = serviceDispatchOut(&child, 0, timestamp);
                result("ISystemClock.GetCurrentTime", rc);
                if (R_SUCCEEDED(rc))
                    log("[time-debug] %s clock command=%u timestamp=%llu", name, id,
                        static_cast<unsigned long long>(timestamp));
            }
            serviceClose(&child);
        }
    }

    if (hosversionAtLeast(6, 0, 0)) {
        Handle handle = INVALID_HANDLE;
        log("[time-debug] %s GetSharedMemoryNativeHandle IPC 20 begin", name);
        rc = serviceDispatch(&root, 20,
            .out_handle_attrs = { SfOutHandleAttr_HipcCopy }, .out_handles = &handle);
        result("GetSharedMemoryNativeHandle", rc);
        if (R_SUCCEEDED(rc)) {
            SharedMemory memory{};
            log("[time-debug] %s shmemLoadRemote handle=0x%X size=0x1000 perm=Perm_R (void API)", name, handle);
            shmemLoadRemote(&memory, handle, 0x1000, Perm_R);
            log("[time-debug] %s shmemMap begin", name);
            rc = shmemMap(&memory);
            result("shmemMap", rc);
            log("[time-debug] %s shared memory address=%p", name, shmemGetAddr(&memory));
            result("shmemClose", shmemClose(&memory));
        }
    } else {
        log("[time-debug] %s IPC 20/mapping skipped: firmware before 6.0.0", name);
    }
    serviceClose(&root);
    log("[time-debug] %s independent probe complete; sessions closed", name);
}
}

// Linker wrappers observe the installed libnx implementation without replacing
// its service guard, cleanup, IPC serialization, or shared-memory clock reads.
extern "C" Result __wrap_timeInitialize(void) {
    void* previous = tracedTls;
    const Handle previousService = tracedService;
    tracedTls = armGetTls();
    tracedService = INVALID_HANDLE;
    log("[time-debug] timeInitialize begin hosversion=0x%08X", hosversionGet());
    const Result rc = __real_timeInitialize();
    result("timeInitialize", rc);
    log("[time-debug] timeInitialize shared memory=%p (no IPC may mean service guard already initialized)", timeGetSharedmemAddr());
    tracedTls = previous;
    tracedService = previousService;
    return rc;
}

extern "C" Result __wrap_smGetServiceWrapper(Service* service, SmServiceName name) {
    const bool trace = tracing();
    if (trace) log("[time-debug] smGetService %.8s begin", name.name);
    const Result rc = __real_smGetServiceWrapper(service, name);
    if (trace) {
        result("smGetService", rc);
        if (R_SUCCEEDED(rc)) {
            tracedService = service->session;
            log("[time-debug] %.8s session=0x%X", name.name, service->session);
        }
    }
    return rc;
}

extern "C" Result __wrap_svcSendSyncRequest(Handle handle) {
    if (!tracing()) return __real_svcSendSyncRequest(handle);
    void* tls = armGetTls();
    // time.c uses non-domain CMIF sessions. SM can use TIPC; do not decode it as CMIF.
    u32 id = 0;
    u32 type = 0;
    const bool cmif = switchu::time_debug::readCommand(tls, type, id);
    log("[time-debug] IPC begin session=0x%X type=%u cmif=%d command=%u (%s)",
        handle, type, cmif, id,
        cmif && handle == tracedService && type == CmifCommandType_Request
            ? commandName(id) : "SM/control/other");
    const Result rc = __real_svcSendSyncRequest(handle);
    // Capture the IPC result before any logging; leave TLS intact for libnx.
    Result ipcResult = 0;
    const bool hasResult = R_SUCCEEDED(rc) && cmif && switchu::time_debug::readResult(tls, ipcResult);
    result("svcSendSyncRequest (kernel)", rc);
    if (hasResult) result("IPC response (service)", ipcResult);
    else if (R_SUCCEEDED(rc) && cmif) log("[time-debug] IPC response has no valid CMIF Result header");
    return rc;
}

extern "C" void __wrap_shmemLoadRemote(SharedMemory* memory, Handle handle, size_t size, Permission permission) {
    if (tracing()) log("[time-debug] shmemLoadRemote begin handle=0x%X size=0x%zX perm=%u (void API)", handle, size, permission);
    __real_shmemLoadRemote(memory, handle, size, permission);
    if (tracing()) log("[time-debug] shmemLoadRemote complete");
}

extern "C" Result __wrap_shmemMap(SharedMemory* memory) {
    if (tracing()) log("[time-debug] shmemMap begin handle=0x%X size=0x%zX perm=%u", memory->handle, memory->size, memory->perm);
    const Result rc = __real_shmemMap(memory);
    if (tracing()) {
        result("shmemMap", rc);
        log("[time-debug] shmemMap address=%p", memory->map_addr);
    }
    return rc;
}

extern "C" Result __wrap_svcMapSharedMemory(Handle handle, void* address, size_t size, u32 permission) {
    if (tracing()) log("[time-debug] svcMapSharedMemory begin handle=0x%X address=%p size=0x%llX perm=%u",
        handle, address, static_cast<unsigned long long>(size), permission);
    const Result rc = __real_svcMapSharedMemory(handle, address, size, permission);
    if (tracing()) result("svcMapSharedMemory (kernel)", rc);
    return rc;
}

namespace switchu::time_debug {
#ifndef SWITCHU_HOMEBREW
void initializeLibcTime(Result initializeResult) {
    if (R_FAILED(initializeResult)) {
        log("[time-debug] __libnx_init_time skipped: timeInitialize failed");
        return;
    }
    log("[time-debug] __libnx_init_time begin (void API)");
    __libnx_init_time();
    log("[time-debug] __libnx_init_time complete");
}
#endif

void probe(Result initializeResult) {
    probeService("time:u");
    probeService("time:a");

    if (R_FAILED(initializeResult)) {
        log("[time-debug] libnx API probes skipped: initialization failed; independent IPC probes above still ran");
        return;
    }
    for (auto type : {TimeType_UserSystemClock, TimeType_NetworkSystemClock, TimeType_LocalSystemClock}) {
        u64 timestamp = 0;
        log("[time-debug] timeGetCurrentTime type=%d begin (libnx shared-memory/IPC path)", type);
        result("timeGetCurrentTime", timeGetCurrentTime(type, &timestamp));
    }
    TimeSteadyClockTimePoint point{};
    log("[time-debug] timeGetStandardSteadyClockTimePoint begin");
    result("timeGetStandardSteadyClockTimePoint", timeGetStandardSteadyClockTimePoint(&point));
    s64 offset = 0;
    log("[time-debug] timeGetStandardSteadyClockInternalOffset begin");
    result("timeGetStandardSteadyClockInternalOffset", timeGetStandardSteadyClockInternalOffset(&offset));
    TimeLocationName location{};
    log("[time-debug] timeGetDeviceLocationName begin");
    result("timeGetDeviceLocationName", timeGetDeviceLocationName(&location));
    TimeCalendarTime calendar{};
    TimeCalendarAdditionalInfo additional{};
    log("[time-debug] timeToCalendarTimeWithMyRule begin (timestamp=0)");
    result("timeToCalendarTimeWithMyRule", timeToCalendarTimeWithMyRule(0, &calendar, &additional));
}

void flush() {
    for (size_t i = 0; i < lineCount; ++i) DebugLog::log("%s", earlyLines[i]);
    if (droppedLines) DebugLog::log("[time-debug] dropped %zu early diagnostic lines", droppedLines);
    lineCount = 0;
    droppedLines = 0;
}
}
