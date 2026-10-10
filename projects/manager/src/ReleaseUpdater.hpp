#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace switchu::manager {

struct ReleaseInfo {
    std::string version;
    std::string name;
    std::string downloadUrl;
    std::string sha256;
    std::uint64_t downloadSize = 0;
    bool updateAvailable = false;
};

struct UpdateInstallResult {
    bool success = false;
    std::string version;
    std::string error;
};

enum class UpdateWorkerStage : int {
    Idle,
    Downloading,
    Verifying,
    Extracting,
    Installing,
};

class ReleaseUpdater final {
public:
    static constexpr const char* kCurrentVersion = HOMENX_BUILD; // this app's "build <N>"
    // Build of the frontend on the SD card ("build <N>", from the
    // homenx-build.txt the frontend zip carries); "build 0" when unknown, so
    // any older frontend is offered the update.
    static std::string installedVersion();

    static ReleaseInfo checkLatest();
    static UpdateInstallResult install(const ReleaseInfo& release,
                                       bool preserveDisabledOverride,
                                       std::atomic<float>& downloadProgress,
                                       std::atomic<float>& installProgress,
                                       std::atomic<int>& stage);
    static void shutdownNetwork();
    // Swaps in files an update couldn't replace while they were open. Call at
    // start-up before fonts or other assets are opened.
    static void applyPending();
};

} // namespace switchu::manager
