// From cristowo/SwitchU (branch switify-integration, 7199904), GPL-3.0:
// the client for Switify's background service (github.com/DomazinUS/Switify).
#include "SwitifyClient.hpp"

#include "DebugLog.hpp"
#include "themeshop/ThemeHttp.hpp"

#include <nxui/third_party/stb/stb_image.h>
#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <sys/stat.h>

namespace switchu::switify {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kActivePeriod = std::chrono::milliseconds(250);
constexpr auto kBackgroundPeriod = std::chrono::milliseconds(1000);
constexpr auto kLevelsPeriod = std::chrono::milliseconds(50);
constexpr auto kStartupGrace = std::chrono::seconds(25);
constexpr auto kCoverRetry = std::chrono::seconds(20);
constexpr std::size_t kMaxCoverBytes = 512 * 1024;
constexpr std::uint32_t kMaxBrowseItems = 64;
constexpr auto kBrowseAcceptTimeout = std::chrono::seconds(5);

// Only the worker thread touches the session.
Service g_service{};

const char* stateName(ServiceState state) {
    switch (state) {
        case ServiceState::NotInstalled: return "not-installed";
        case ServiceState::NotRunning: return "not-running";
        case ServiceState::Starting: return "starting";
        case ServiceState::WaitingForCredentials: return "credentials";
        case ServiceState::Connecting: return "connecting";
        case ServiceState::Ready: return "ready";
    }
    return "?";
}

bool fileExists(const std::string& path) {
    struct stat info{};
    return stat(path.c_str(), &info) == 0 && info.st_size > 0;
}

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {};
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
    if (bytes.size() > kMaxCoverBytes) bytes.clear();
    return bytes;
}

bool isJpeg(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF;
}

void writeFileAtomically(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    mkdir("sdmc:/config", 0755);
    mkdir("sdmc:/config/SwitchU", 0755);
    mkdir("sdmc:/config/SwitchU/cache", 0755);
    mkdir(kOwnCoverCacheDir, 0755);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return;
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!file.good()) {
            file.close();
            std::remove(temporary.c_str());
            return;
        }
    }
    std::remove(path.c_str());
    if (std::rename(temporary.c_str(), path.c_str()) != 0)
        std::remove(temporary.c_str());
}

// Atmosphere defers GetService for an unregistered name, which would block
// this thread until Switify starts. sm:AtmosphereHasService (65100) answers
// immediately.
bool serviceRegistered() {
    std::uint8_t present = 0;
    const SmServiceName name = smEncodeName(kServiceName);
    if (R_FAILED(tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, present)))
        return false;
    return (present & 1) != 0;
}

} // namespace

void Client::start() {
    // stop() is final: HOME is shutting down.
    if (m_thread.joinable() || m_stop) return;
    m_thread = std::thread([this]() { run(); });
}

void Client::stop() {
    m_stop = true;
    if (!m_thread.joinable()) return;
    m_wake.notify_all();
    m_thread.join();
}

void Client::setDemand(Demand demand) {
    if (m_demand.exchange(demand) != demand && demand == Demand::Active)
        m_wake.notify_all();
}

void Client::setLevelsWanted(bool wanted) {
    if (m_levelsWanted.exchange(wanted) != wanted)
        m_wake.notify_all();
}

void Client::levels(float* out) const {
    std::lock_guard lock(m_mutex);
    for (std::size_t i = 0; i < m_levels.size(); ++i)
        out[i] = static_cast<float>(m_levels[i]) / 255.f;
}

void Client::pollLevels() {
    if (!m_connected) return;
    std::array<std::uint8_t, kLevelBands> levels{};
    static bool firstRequest = true;
    if (firstRequest) DebugLog::log("[switify] first audio levels request");
    if (R_FAILED(serviceDispatchOut(&g_service,
            static_cast<std::uint32_t>(IpcCommand::GetAudioLevels), levels))) {
        m_levelsAvailable.store(false);
        DebugLog::log("[switify] this build has no audio levels");
        return;
    }
    if (firstRequest) DebugLog::log("[switify] first audio levels received");
    firstRequest = false;
    std::lock_guard lock(m_mutex);
    m_levels = levels;
}

NowPlaying Client::nowPlaying() const {
    std::lock_guard lock(m_mutex);
    return m_now;
}

BrowsePage Client::browsePage() const {
    std::lock_guard lock(m_mutex);
    return m_browse;
}

std::shared_ptr<const Cover> Client::cover() const {
    std::lock_guard lock(m_mutex);
    return m_cover;
}

void Client::queue(Request request) {
    {
        std::lock_guard lock(m_mutex);
        m_requests.push_back(std::move(request));
    }
    m_wake.notify_all();
}

bool Client::pendingRequests() const {
    std::lock_guard lock(m_mutex);
    return !m_requests.empty();
}

void Client::send(IpcCommand command, std::uint32_t value) {
    Request request;
    request.command = command;
    request.value = value;
    queue(std::move(request));
}

void Client::browse(IpcCommand command, std::uint64_t generation,
                    std::uint32_t index, std::string query) {
    Request request;
    request.command = command;
    request.generation = generation;
    request.index = index;
    request.query = std::move(query);
    request.isBrowse = true;
    // Show "loading" at once instead of the previous page for one poll.
    // Playing an item keeps the page where it is.
    if (command != IpcCommand::BrowsePlay) {
        std::lock_guard lock(m_mutex);
        m_staleGeneration = m_browse.generation;
        m_awaitingBrowseSince = Clock::now();
        m_awaitingBrowse = true;
        m_browse.state = kBrowseLoading;
        m_browse.items.clear();
    }
    queue(std::move(request));
}

void Client::launchService() {
    Request request;
    request.isLaunch = true;
    queue(std::move(request));
}

bool Client::connect() {
    if (m_connected) return true;
    if (!serviceRegistered()) return false;
    if (R_FAILED(smGetService(&g_service, kServiceName))) return false;
    m_connected = true;
    m_levelsAvailable.store(true);
    DebugLog::log("[switify] connected");
    return true;
}

void Client::disconnect() {
    if (!m_connected) return;
    serviceClose(&g_service);
    m_connected = false;
    DebugLog::log("[switify] disconnected");
}

void Client::execute(const Request& request) {
    if (request.isLaunch) {
        if (m_connected || serviceRegistered()) return;
        Result rc = pmshellInitialize();
        if (R_SUCCEEDED(rc)) {
            NcmProgramLocation location{};
            location.program_id = kProgramId;
            location.storageID = NcmStorageId_None;
            u64 pid = 0;
            rc = pmshellLaunchProgram(0, &location, &pid);
            pmshellExit();
        }
        DebugLog::log("[switify] launch rc=0x%X", static_cast<unsigned>(rc));
        // An already-running process (still registering) also fails here;
        // either way the next polls report Starting until it registers.
        m_launchPending = true;
        m_launchedAt = Clock::now();
        return;
    }

    if (!connect()) return;
    Result rc = 0;
    const auto id = static_cast<std::uint32_t>(request.command);
    if (request.isBrowse) {
        IpcBrowseRequest body{};
        body.generation = request.generation;
        body.index = request.index;
        request.query.copy(body.query, sizeof(body.query) - 1);
        rc = serviceDispatchIn(&g_service, id, body);
    } else if (request.command == IpcCommand::SetVolume ||
               request.command == IpcCommand::Seek ||
               request.command == IpcCommand::SetShuffle ||
               request.command == IpcCommand::SetRepeatMode) {
        rc = serviceDispatchIn(&g_service, id, request.value);
    } else {
        rc = serviceDispatch(&g_service, id);
    }
    // The official Switify does not know it: the best it can do is carry on.
    if (R_FAILED(rc) && request.command == IpcCommand::ReloadCurrent)
        rc = serviceDispatch(&g_service, static_cast<std::uint32_t>(IpcCommand::Play));
    if (R_FAILED(rc)) {
        DebugLog::log("[switify] command %u failed rc=0x%X", static_cast<unsigned>(id),
                      static_cast<unsigned>(rc));
        // A control rejection (module 0x1A0 range) keeps the session; a
        // transport error means the service went away.
        if (R_MODULE(rc) == Module_Libnx || R_MODULE(rc) == Module_Kernel)
            disconnect();
    }
}

void Client::pollStatus() {
    NowPlaying now;
    IpcStatus status{};
    const bool ok = connect() &&
        R_SUCCEEDED(serviceDispatchOut(&g_service,
            static_cast<std::uint32_t>(IpcCommand::GetStatus), status)) &&
        status.structSize == sizeof(status) && status.abiVersion == 1;

    if (!ok) {
        disconnect();
        m_installed = fileExists(kExefsPath);
        if (m_launchPending && Clock::now() - m_launchedAt < kStartupGrace)
            now.state = ServiceState::Starting;
        else
            now.state = m_installed ? ServiceState::NotRunning : ServiceState::NotInstalled;
    } else {
        m_launchPending = false;
        switch (status.serviceState) {
            case 1: case 2: now.state = ServiceState::WaitingForCredentials; break;
            case 4: now.state = ServiceState::Ready; break;
            case 5: case 6: now.state = ServiceState::NotRunning; break;
            default: now.state = ServiceState::Connecting; break;
        }
        now.hasTrack = (status.flags & kStatusFlagHasTrack) != 0;
        // Buffering (1) counts as playing so labels do not flicker.
        now.playing = now.hasTrack &&
            (status.playbackState == 1 || status.playbackState == 2);
        now.shuffle = (status.flags & kStatusFlagShuffle) != 0;
        now.canSeek = (status.flags & kStatusFlagCanSeek) != 0;
        now.reloading = (status.flags & kStatusFlagReloading) != 0;
        now.repeat = menuRepeatFromFlags(status.flags);
        now.positionMs = status.positionMs;
        now.durationMs = status.durationMs;
        now.volumePermille = std::min<std::uint32_t>(status.volumePermille, 1000);
        if (now.hasTrack) {
            now.title = boundedString(status.title);
            now.artist = boundedString(status.artist);
            now.album = boundedString(status.album);
        }
    }

    std::lock_guard lock(m_mutex);
    if (now.state != m_now.state)
        DebugLog::log("[switify] state %s -> %s", stateName(m_now.state), stateName(now.state));
    m_now = std::move(now);
}

void Client::pollBrowse() {
    if (!m_connected) return;
    IpcBrowseHeader header{};
    if (R_FAILED(serviceDispatchOut(&g_service,
            static_cast<std::uint32_t>(IpcCommand::GetBrowseHeader), header)) ||
        header.structSize != sizeof(header) || header.abiVersion != 1)
        return;

    {
        std::lock_guard lock(m_mutex);
        if (m_awaitingBrowse) {
            // Until the service starts the requested page it still reports the
            // previous one; showing it again would look like the press failed.
            if (header.generation == m_staleGeneration &&
                Clock::now() - m_awaitingBrowseSince < kBrowseAcceptTimeout)
                return;
            m_awaitingBrowse = false;
        }
        if (header.generation == m_browse.generation && header.state == m_browse.state &&
            header.itemCount == m_browse.items.size() && header.state != kBrowseLoading)
            return;
    }

    BrowsePage page;
    page.generation = header.generation;
    page.state = header.state;
    page.depth = header.depth;
    page.title = boundedString(header.title);
    page.subtitle = boundedString(header.subtitle);
    page.message = boundedString(header.message);
    if (header.state == kBrowseReady) {
        const std::uint32_t count = std::min(header.itemCount, kMaxBrowseItems);
        page.items.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            IpcBrowseItem item{};
            if (R_FAILED(serviceDispatchInOut(&g_service,
                    static_cast<std::uint32_t>(IpcCommand::GetBrowseItem), index, item)) ||
                item.generation != header.generation)
                return; // The page moved on; the next poll reads the new one.
            BrowseItem entry;
            entry.track = item.kind == kBrowseKindTrack;
            entry.kind = item.kind;
            entry.name = boundedString(item.name);
            entry.subtitle = boundedString(item.subtitle);
            entry.detail = boundedString(item.detail);
            entry.durationMs = item.durationMs;
            page.items.push_back(std::move(entry));
        }
    }
    std::lock_guard lock(m_mutex);
    m_browse = std::move(page);
}

void Client::pollCover(bool hasTrack) {
    std::string url;
    if (hasTrack) {
        std::ifstream file(kArtworkUrlPath);
        if (file.is_open()) url.assign(std::istreambuf_iterator<char>(file), {});
    }
    if (url == m_coverUrl) return;
    if (url.empty()) {
        m_coverUrl.clear();
        std::lock_guard lock(m_mutex);
        m_cover.reset();
        return;
    }
    if (url == m_failedCoverUrl && Clock::now() < m_coverRetryAt) return;

    std::vector<std::uint8_t> bytes = readFile(switifyCoverPath(url));
    if (!isJpeg(bytes)) bytes = readFile(ownCoverPath(url));
    if (!isJpeg(bytes) && url.rfind("http", 0) == 0) {
        try {
            // Abort as soon as a control command is waiting: covers are nice
            // to have, play/pause must stay responsive.
            bytes = themeshop::http::getBytes(url, {}, {}, [this]() {
                return m_stop.load() || pendingRequests();
            });
            if (isJpeg(bytes) && bytes.size() <= kMaxCoverBytes)
                writeFileAtomically(ownCoverPath(url), bytes);
        } catch (const std::exception& error) {
            DebugLog::log("[switify] cover download failed: %s", error.what());
            bytes.clear();
        }
    }
    if (!isJpeg(bytes)) {
        m_failedCoverUrl = url;
        m_coverRetryAt = Clock::now() + kCoverRetry;
        return;
    }

    auto cover = std::make_shared<Cover>();
    cover->key = url;
    int width = 0, height = 0, channels = 0;
    if (stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                &width, &height, &channels, 4)) {
        const float scale = std::min(1.f, static_cast<float>(kCoverDecodeSide) /
                                              static_cast<float>(std::max(width, height)));
        cover->width = std::max(1, static_cast<int>(width * scale));
        cover->height = std::max(1, static_cast<int>(height * scale));
        cover->rgba.resize(static_cast<std::size_t>(cover->width) * cover->height * 4);
        for (int y = 0; y < cover->height; ++y) {
            const int sy = y * height / cover->height;
            for (int x = 0; x < cover->width; ++x) {
                const int sx = x * width / cover->width;
                std::copy_n(pixels + (static_cast<std::size_t>(sy) * width + sx) * 4, 4,
                            cover->rgba.data() +
                                (static_cast<std::size_t>(y) * cover->width + x) * 4);
            }
        }
        stbi_image_free(pixels);
    }
    cover->bytes = std::move(bytes);
    m_coverUrl = url;
    std::lock_guard lock(m_mutex);
    m_cover = std::move(cover);
}

void Client::run() {
    DebugLog::log("[switify] worker started");
    auto nextFullPoll = Clock::now();
    while (!m_stop) {
        std::vector<Request> requests;
        {
            std::lock_guard lock(m_mutex);
            requests.swap(m_requests);
        }
        for (const auto& request : requests)
            execute(request);

        const bool active = m_demand.load() == Demand::Active;
        const auto period = active ? std::chrono::milliseconds(kActivePeriod)
                                   : std::chrono::milliseconds(kBackgroundPeriod);
        if (!requests.empty() || Clock::now() >= nextFullPoll) {
            pollStatus();
            if (active) pollBrowse();
            pollCover(nowPlaying().hasTrack);
            nextFullPoll = Clock::now() + period;
        }

        // Faster than the status, and only while bars are on screen.
        const bool wanted = m_levelsWanted.load();
        const bool polling = wanted && m_levelsAvailable.load();
        if (polling) pollLevels();

        std::unique_lock lock(m_mutex);
        m_wake.wait_for(lock, polling ? kLevelsPeriod : period, [this, wanted]() {
            return m_stop.load() || !m_requests.empty() || m_levelsWanted.load() != wanted;
        });
    }
    disconnect();
    DebugLog::log("[switify] worker stopped");
}

} // namespace switchu::switify
