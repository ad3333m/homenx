#include "media_service.hpp"

#include "background_backend.hpp"
#include "NXConsoleLog.h"
#include <switube/ipc_protocol.h>

#include <cstdio>
#include <memory>
#include <new>
#include <sys/stat.h>

namespace switchu::daemon::media {
namespace {

using switube::phase2::BackgroundBackend;
using switube::phase2::ServiceCore;
namespace ipc = switube::phase2::switch_service;

std::unique_ptr<ServiceCore> core;
std::unique_ptr<BackgroundBackend> backend;

Result startBackend() {
    if (backend) return 0;

    auto nextCore = std::unique_ptr<ServiceCore>(new (std::nothrow) ServiceCore);
    if (!nextCore) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    auto nextBackend = std::unique_ptr<BackgroundBackend>(
        new (std::nothrow) BackgroundBackend(*nextCore));
    if (!nextBackend) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    Result rc = nextBackend->start();
    if (R_FAILED(rc)) return rc;
    rc = ipc::initialize(*nextCore);
    if (R_FAILED(rc)) return rc;

    core = std::move(nextCore);
    backend = std::move(nextBackend);
    return 0;
}

}

void start() {
    // The Switube engine runs inside the existing HOME daemon on demand.
    mkdir("sdmc:/config", 0755);
    mkdir("sdmc:/config/SwitchU", 0755);
    mkdir("sdmc:/config/SwitchU/media", 0755);
    nxlog::setLogFile("sdmc:/config/SwitchU/media/background.log", 64 * 1024);
    nxlog::setQueueEnabled(false);

    const Result rc = startBackend();
    nxlog::pushf("HomeNX daemon media startup: 0x%08X", rc);

    if (R_SUCCEEDED(rc)) return;

    constexpr auto temporaryPath = HOMENX_MEDIA_READY_PATH ".tmp";
    if (FILE* file = std::fopen(temporaryPath, "w")) {
        const int written = std::fprintf(file, HOMENX_MEDIA_READY_TAG " 0x%08X\n", rc);
        if (std::fclose(file) == 0 && written > 0)
            std::rename(temporaryPath, HOMENX_MEDIA_READY_PATH);
    }
}
void stop() {
    if (!backend) return;
    ipc::exit();
    backend.reset();
    core.reset();
}

}
