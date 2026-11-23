#include "ecs.hpp"
#include <switchu/smi_protocol.hpp>
#include <switchu/file_log.hpp>

#include <switch/services/fsldr.h>
#include <switch/services/lr.h>

#include <stratosphere.hpp>
#include <stratosphere/fssrv/interface_adapters/fssrv_filesystem_interface_adapter.hpp>

#include <atomic>
#include <memory>
#include <mutex>

namespace ams::os {
    void Initialize();
}

namespace switchu::daemon {

namespace {

struct EcsServerOptions {
    static constexpr size_t PointerBufferSize = 0x800;
    static constexpr size_t MaxDomains = 0x40;
    static constexpr size_t MaxDomainObjects = 0x100;
    static constexpr bool CanDeferInvokeRequest = false;
    static constexpr bool CanManageMitmServers = false;
};

using EcsServerManager = ::ams::sf::hipc::ServerManager<0, EcsServerOptions, 8>;

constexpr size_t kAmsHeapSize = 2 * 1024 * 1024;
constexpr size_t kManagerThreadStackSize = 0x8000;
constexpr s32 kManagerThreadPriority = 10;

alignas(::ams::os::MemoryPageSize) uint8_t g_amsHeap[kAmsHeapSize] = {};
alignas(::ams::os::ThreadStackAlignment) uint8_t g_managerThreadStack[kManagerThreadStackSize] = {};

EcsServerManager g_manager;
::ams::os::ThreadType g_managerThread = {};
std::once_flag g_runtimeOnce;
std::atomic<bool> g_allocatorReady{false};
std::atomic<bool> g_runtimeReady{false};

void managerThreadEntry(void*) {
    switchu::FileLog::log("[ecs] stratosphere manager thread started");
    g_manager.LoopProcess();
    switchu::FileLog::log("[ecs] stratosphere manager thread exiting");
}

Result initializeRuntime() {
    Result rc = 0;
    std::call_once(g_runtimeOnce, [&] {
        initializeExternalContentAllocator();

        auto amsRc = ::ams::os::CreateThread(&g_managerThread, managerThreadEntry, nullptr,
                                             g_managerThreadStack, sizeof(g_managerThreadStack),
                                             kManagerThreadPriority);
        rc = amsRc.GetValue();
        if (R_FAILED(rc)) {
            switchu::FileLog::log("[ecs] stratosphere CreateThread FAIL: 0x%X", rc);
            return;
        }

        ::ams::os::StartThread(&g_managerThread);
        g_runtimeReady.store(true);
        switchu::FileLog::log("[ecs] stratosphere runtime ready");
    });

    return g_runtimeReady.load() ? 0 : rc;
}

Result ldrAtmosRegisterExternalCode(uint64_t program_id, Handle* out_h) {
    return serviceDispatchIn(ldrShellGetServiceSession(),
        smi::kLdrAtmosRegisterExternalCode, program_id,
        .out_handle_attrs = { SfOutHandleAttr_HipcMove },
        .out_handles = out_h,
    );
}

Result ldrAtmosUnregisterExternalCode(uint64_t program_id) {
    return serviceDispatchIn(ldrShellGetServiceSession(),
        smi::kLdrAtmosUnregisterExternalCode, program_id);
}

}

void initializeExternalContentAllocator() {
    bool expected = false;
    if (g_allocatorReady.compare_exchange_strong(expected, true)) {
        ::ams::init::InitializeAllocator(g_amsHeap, sizeof(g_amsHeap));
        ::ams::os::Initialize();
        ::ams::os::SetThreadNamePointer(::ams::os::GetCurrentThread(), "SwitchU.daemon.Main");
    }
}

static Result serveExternalContent(uint64_t programId, std::shared_ptr<::ams::fs::fsa::IFileSystem> fs) {
    Result rc = initializeRuntime();
    if (R_FAILED(rc))
        return rc;

    Handle handle = INVALID_HANDLE;
    rc = ldrAtmosRegisterExternalCode(programId, &handle);
    if (R_FAILED(rc))
        return rc;

    auto object = ::ams::sf::CreateSharedObjectEmplaced<
        ::ams::fssrv::sf::IFileSystem,
        ::ams::fssrv::impl::FileSystemInterfaceAdapter>(std::move(fs), false);
    const auto result = g_manager.RegisterSession(handle, ::ams::sf::cmif::ServiceObjectHolder(std::move(object)));
    if (result.IsFailure()) {
        ldrAtmosUnregisterExternalCode(programId);
        svcCloseHandle(handle);
    }
    return result.GetValue();
}

Result registerExternalContent(uint64_t program_id, const char* exefs_path) {
    Result rc = initializeRuntime(); // initialize the allocator before making filesystem objects
    if (R_FAILED(rc))
        return rc;

    FsFileSystem sd{};
    rc = fsOpenSdCardFileSystem(&sd);
    if (R_FAILED(rc))
        return rc;

    auto remote = std::make_shared<::ams::fs::RemoteFileSystem>(sd);
    auto subdir = std::make_shared<::ams::fssystem::SubDirectoryFileSystem>(std::move(remote));
    ::ams::fs::Path path;
    auto result = path.Initialize(exefs_path, std::strlen(exefs_path));
    if (result.IsSuccess())
        result = path.Normalize(::ams::fs::PathFlags{});
    if (result.IsSuccess())
        result = subdir->Initialize(path);
    if (result.IsFailure())
        return result.GetValue();
    return serveExternalContent(program_id, std::move(subdir));
}

Result registerSystemAppletCode(uint64_t programId) {
    Result rc = initializeRuntime();
    if (R_FAILED(rc))
        return rc;
    rc = lrInitialize();
    if (R_FAILED(rc))
        return rc;

    constexpr auto storage = NcmStorageId_BuiltInSystem;
    char path[FS_MAX_PATH]{};
    LrLocationResolver resolver{};
    rc = lrOpenLocationResolver(storage, &resolver);
    if (R_SUCCEEDED(rc)) {
        rc = lrLrResolveProgramPath(&resolver, programId, path);
        serviceClose(&resolver.s);
    }
    lrExit();
    if (R_FAILED(rc))
        return rc;

    rc = fsldrInitialize();
    if (R_FAILED(rc))
        return rc;
    FsFileSystem code{};
    FsCodeInfo info{};
    rc = fsldrOpenCodeFileSystem(&info, programId, storage, path, FsContentAttributes_None, &code);
    fsldrExit();
    if (R_FAILED(rc))
        return rc;
    return serveExternalContent(programId, std::make_shared<::ams::fs::RemoteFileSystem>(code));
}

Result unregisterExternalContent(uint64_t program_id) {
    Result rc = ldrAtmosUnregisterExternalCode(program_id);
    if (R_FAILED(rc))
        switchu::FileLog::log("[ecs] Unregister FAIL: 0x%X", rc);
    else
        switchu::FileLog::log("[ecs] unregistered 0x%016lX", program_id);
    return rc;
}

}
