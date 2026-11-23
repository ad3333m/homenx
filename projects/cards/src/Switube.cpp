#ifdef HOMENX_NATIVE_YOUTUBE
#include "NativeYoutube.hpp"
#endif
#include "Switube.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#ifdef SWITCHU_MENU
#include <switch.h>
#include "smi_commands.hpp"
#include <nxui/core/ThreadPool.hpp>
#endif

namespace cards {
namespace {
template <std::size_t N> std::string bounded(const char (&s)[N]) {
    const auto end = std::find(s, s + N, '\0');
    return {s, end};
}
}

bool Switube::valid(const SwitubeIpcStatus& s) {
    return s.struct_size == sizeof(s) && s.abi_version == SWITUBE_IPC_STATUS_ABI_VERSION;
}

Switube::Now Switube::decode(const SwitubeIpcStatus& s, std::uint64_t tick) {
    if (!valid(s)) return {};
    Now n;
    n.connected = true;
    n.active = (s.flags & SwitubeStatusFlag_HasTrack) != 0;
    n.playing = s.playback_state == SwitubePlaybackState_Playing;
    n.shuffle = (s.flags & SwitubeStatusFlag_Shuffle) != 0;
    n.repeat = (s.flags & SwitubeStatusFlag_RepeatOne) ? 2 : (s.flags & SwitubeStatusFlag_RepeatAll) ? 1 : 0;
    // Keep malformed or oversized values out of the UI's signed arithmetic.
    n.durationMs = static_cast<int>(std::min(s.duration_ms, std::uint32_t(INT32_MAX)));
    n.progressMs = static_cast<int>(std::min(s.position_ms, std::uint32_t(INT32_MAX)));
    if (n.durationMs) n.progressMs = std::min(n.progressMs, n.durationMs);
    n.volume = static_cast<int>(std::min(s.volume_permille, 1000u) / 10u);
    n.track = bounded(s.title);
    n.artist = bounded(s.artist);
    n.at = tick;
    return n;
}

std::uint32_t Switube::repeatWire(int uiRepeat) {
    return uiRepeat == 1 ? SwitubeRepeatMode_All : uiRepeat == 2 ? SwitubeRepeatMode_One : SwitubeRepeatMode_Off;
}

void Switube::start() {
#ifdef HOMENX_NATIVE_YOUTUBE
    if (m_thread.handle) return;
    m_quit = false;
    Result rc = threadCreate(&m_thread, [](void* context) {
        static_cast<Switube*>(context)->run();
    }, this, nullptr, 1024 * 1024, 49, -2);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&m_thread);
        if (R_FAILED(rc)) threadClose(&m_thread);
    }
    if (R_FAILED(rc)) {
        m_thread = {};
        std::lock_guard lock(m_mu);
        m_error = "Could not start the media worker.";
    }
#elif defined(SWITCHU_MENU)
    if (!m_thread.joinable()) {
        m_quit = false;
        m_thread = std::thread([this] { run(); });
    }
#endif
}
void Switube::stop() {
    { std::lock_guard<std::mutex> lock(m_mu); m_quit = true; m_onlineAbort = true; }
    m_cv.notify_all();
#ifdef HOMENX_NATIVE_YOUTUBE
    if (m_thread.handle) {
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
        m_thread = {};
    }
#else
    if (m_thread.joinable()) m_thread.join();
#endif
}
void Switube::setWatching(bool on) {
    { std::lock_guard<std::mutex> lock(m_mu);
      m_watching = on;
      m_onlineAbort = !on;
      if (!on) m_requests.clear(); // no delayed buttons after sleep or leaving HOME
    }
    m_cv.notify_all();
}
Switube::Now Switube::now() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_now;
}
void Switube::command(Cmd cmd, int value) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (!m_watching) return;
    // A dragged volume replaces its pending value rather than flooding IPC.
    if (cmd == Cmd::Volume)
        m_requests.erase(std::remove_if(m_requests.begin(), m_requests.end(), [](const Request& r) {
            return !r.browsing && r.cmd == Cmd::Volume;
        }), m_requests.end());
    if (m_requests.size() >= 32) return;
    Request r;
    r.cmd = cmd; r.value = value;
    m_requests.push_back(std::move(r));
    m_cv.notify_all();
}
void Switube::browse(Browse cmd, std::uint64_t generation, unsigned index, std::string query) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (!m_watching || m_requests.size() >= 32) return;
    Request r;
    r.browsing = true; r.browse = cmd; r.generation = generation; r.index = index; r.query = std::move(query);
    m_requests.push_back(std::move(r));
    m_cv.notify_all();
}
bool Switube::takePage(Page& page) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (!m_pageNew) return false;
    page = m_page; m_pageNew = false;
    return true;
}
std::string Switube::takeError() {
    std::lock_guard<std::mutex> lock(m_mu);
    return std::exchange(m_error, {});
}

#ifdef SWITCHU_MENU
void Switube::run() {
    nxui::moveToWorkerCores();
    using Clock = std::chrono::steady_clock;
    Service service{};
#ifdef HOMENX_NATIVE_YOUTUBE
    NativeYoutube youtube(m_onlineAbort);
    bool usingYoutube = false;
#endif
    bool startupPending = false, browsing = false;
    auto startupDeadline = Clock::now();
    auto nextPoll = Clock::now();
    std::uint64_t pageGeneration = UINT64_MAX;
    std::uint32_t pageState = UINT32_MAX;
    auto disconnect = [&] {
        serviceClose(&service);
        std::lock_guard<std::mutex> lock(m_mu);
        m_now = {};
    };
    auto error = [&](const std::string& text) {
        std::lock_guard<std::mutex> lock(m_mu); m_error = text;
    };
    auto failed = [&](Result rc) {
        char text[100];
        std::snprintf(text, sizeof(text), "HomeNX Media request failed (0x%X). Reopen the music player to retry.", rc);
        error(text); disconnect();
    };
    while (true) {
        Request request;
        bool haveRequest = false;
        {
            std::unique_lock<std::mutex> lock(m_mu);
            m_cv.wait_until(lock, nextPoll, [&] { return m_quit || !m_watching || !m_requests.empty(); });
            if (m_quit) break;
            if (!m_watching) {
                lock.unlock();
#ifdef HOMENX_NATIVE_YOUTUBE
                youtube.stop();
#endif
                disconnect(); startupPending = false; browsing = false;
                lock.lock();
                m_cv.wait(lock, [&] { return m_quit || m_watching; });
                nextPoll = Clock::now();
                continue;
            }
            if (!m_requests.empty()) {
                request = std::move(m_requests.front()); m_requests.pop_front(); haveRequest = true;
            }
        }
        nextPoll = Clock::now() + std::chrono::milliseconds(500);
#ifdef HOMENX_NATIVE_YOUTUBE
        if (!haveRequest) youtube.tick();
        if (const auto failure = youtube.takePlaybackError(); !failure.empty()) error(failure);
        if (haveRequest && request.browsing && youtube.handles(request.browse)) {
            Page page;
            try {
                page = youtube.browse(request.browse, request.generation, request.index, request.query);
            } catch (const std::exception& ex) {
                error(std::string("YouTube Music request failed: ") + ex.what());
            }
            if (request.browse == Browse::OnlinePlay && youtube.active()) {
                usingYoutube = true;
                if (serviceIsActive(&service))
                    serviceDispatch(&service, SwitubeIpcCommand_Pause);
            }
            if (!m_onlineAbort) {
                std::lock_guard lock(m_mu);
                if (request.browse == Browse::OnlineQueue && youtube.hasQueue() && !m_now.active)
                    usingYoutube = true;
                if (page.online) {
                    m_page = std::move(page);
                    m_pageNew = true;
                }
                if (usingYoutube) {
                    m_now = youtube.now();
                } else {
                    m_now.queue.clear();
                    youtube.describeQueue(m_now);
                    m_now.queueIndex = -1;
                    m_now.at = armGetSystemTick();
                }
            }
            browsing = false;
            continue;
        }
        if (haveRequest && request.browsing && request.browse == Browse::Play) {
            youtube.stop();
            usingYoutube = false;
        }
        if (haveRequest && !request.browsing && request.cmd == Cmd::PlayQueued && youtube.hasQueue()) {
            if (youtube.command(request.cmd, request.value)) {
                usingYoutube = true;
                if (serviceIsActive(&service))
                    serviceDispatch(&service, SwitubeIpcCommand_Pause);
            } else {
                error("Could not play the queued song.");
                continue;
            }
            haveRequest = browsing = false;
        }
        if (usingYoutube && ((!haveRequest && !browsing) || (haveRequest && !request.browsing))) {
            if (haveRequest && !youtube.command(request.cmd, request.value))
                error("Could not play the queued song.");
            std::lock_guard lock(m_mu);
            m_now = youtube.now();
            continue;
        }
#endif
        if (!serviceIsActive(&service)) {
            // Atmosphère SM command 65100 checks registration without waiting
            // for the service to start. GetService can block on an absent port.
            const auto name = smEncodeName(SWITUBE_IPC_SERVICE_NAME);
            u8 registered = 0;
            Result rc = tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, registered);
            if (R_FAILED(rc)) {
                if (haveRequest) failed(rc);
                startupPending = false;
                continue;
            }
            if (!(registered & 1)) {
                if (!startupPending) {
                    if (!haveRequest) continue;
                    std::remove(HOMENX_MEDIA_READY_PATH);
                    rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::StartMedia);
                    if (R_FAILED(rc)) { failed(rc); continue; }
                    startupPending = true;
                    startupDeadline = Clock::now() + std::chrono::seconds(8);
                }

                unsigned startupResult = 0;
                if (FILE* file = std::fopen(HOMENX_MEDIA_READY_PATH, "r")) {
                    char line[80]{};
                    if (std::fgets(line, sizeof(line), file))
                        std::sscanf(line, HOMENX_MEDIA_READY_TAG " 0x%X", &startupResult);
                    std::fclose(file);
                }
                if (startupResult || Clock::now() >= startupDeadline) {
                    startupPending = false;
                    if (startupResult) failed(startupResult);
                    else error("HomeNX Media did not respond. Check config/SwitchU/media/background.log.");
                    continue;
                }
                std::unique_lock<std::mutex> lock(m_mu);
                if (haveRequest && m_requests.size() < 32)
                    m_requests.push_front(std::move(request));
                m_cv.wait_for(lock, std::chrono::milliseconds(250), [&] { return m_quit || !m_watching; });
                continue;
            }

            startupPending = false;
            rc = smGetService(&service, SWITUBE_IPC_SERVICE_NAME);
            u32 version = 0;
            if (R_SUCCEEDED(rc)) rc = serviceDispatchOut(&service, SwitubeIpcCommand_GetApiVersion, version);
            if (R_FAILED(rc)) { failed(rc); continue; }
            if (version != SWITUBE_IPC_API_VERSION) {
                error("HomeNX Media and the menu have different versions. Restart after updating.");
                disconnect();
                continue;
            }
        }
        SwitubeIpcStatus status{};
        Result rc = serviceDispatchOut(&service, SwitubeIpcCommand_GetStatus, status);
        if (R_FAILED(rc)) { failed(rc); continue; }
        if (!valid(status)) { error("HomeNX Media returned an incompatible status."); disconnect(); continue; }
        if (haveRequest) {
            if (request.browsing) {
                SwitubeIpcBrowseRequest wire{};
                wire.generation = request.generation; wire.index = request.index;
                std::snprintf(wire.query, sizeof(wire.query), "%s", request.query.c_str());
                const u32 cmds[] = {SwitubeIpcCommand_BrowseHome, SwitubeIpcCommand_BrowseLibrary,
                    SwitubeIpcCommand_BrowseSearch, SwitubeIpcCommand_BrowseOpen,
                    SwitubeIpcCommand_BrowseBack, SwitubeIpcCommand_BrowsePlay};
                const auto command = static_cast<unsigned>(request.browse);
                if (command >= std::size(cmds)) continue;
                rc = serviceDispatchIn(&service, cmds[command], wire);
                browsing = true; pageGeneration = UINT64_MAX; pageState = UINT32_MAX;
            } else {
                const Now n = decode(status, 0);
                u32 value = 0, cmd = 0;
                bool input = false;
                switch (request.cmd) {
                    case Cmd::PlayQueued: continue;
                    case Cmd::PlayPause: cmd = SwitubeIpcCommand_PlayPause; break;
                    case Cmd::Next: cmd = SwitubeIpcCommand_Next; break;
                    case Cmd::Previous: cmd = SwitubeIpcCommand_Previous; break;
                    case Cmd::Shuffle: cmd = SwitubeIpcCommand_SetShuffle; value = !n.shuffle; input = true; break;
                    case Cmd::Repeat: cmd = SwitubeIpcCommand_SetRepeatMode; value = repeatWire((n.repeat + 1) % 3); input = true; break;
                    case Cmd::Volume: cmd = SwitubeIpcCommand_SetVolume; value = std::clamp(request.value, 0, 100) * 10; input = true; break;
                }
                rc = input ? serviceDispatchIn(&service, cmd, value) : serviceDispatch(&service, cmd);
            }
            if (R_FAILED(rc)) { failed(rc); continue; }
            rc = serviceDispatchOut(&service, SwitubeIpcCommand_GetStatus, status);
            if (R_FAILED(rc)) { failed(rc); continue; }
        }
        auto snapshot = decode(status, armGetSystemTick());
#ifdef HOMENX_NATIVE_YOUTUBE
        if (usingYoutube) {
            snapshot = youtube.now();
        } else {
            youtube.describeQueue(snapshot);
            snapshot.queueIndex = -1;
            if (snapshot.active) {
                if (FILE* file = std::fopen("sdmc:/config/SwitchU/media/current-artwork-url", "r")) {
                    char cover[2049]{};
                    if (std::fgets(cover, sizeof(cover), file)) {
                        snapshot.cover = cover;
                        if (!snapshot.cover.empty() && snapshot.cover.back() == '\n') snapshot.cover.pop_back();
                    }
                    std::fclose(file);
                }
            }
        }
#endif
        {
            std::lock_guard lock(m_mu);
            m_now = std::move(snapshot);
        }
        if (!browsing) continue;
        SwitubeIpcBrowseHeader header{};
        rc = serviceDispatchOut(&service, SwitubeIpcCommand_GetBrowseHeader, header);
        if (R_FAILED(rc)) { failed(rc); continue; }
        if (header.struct_size != sizeof(header) || header.abi_version != SWITUBE_IPC_BROWSE_ABI_VERSION) {
            error("Switube returned an incompatible library."); browsing = false; continue;
        }
        if (header.generation == pageGeneration && header.state == pageState) continue;
        Page page;
        page.loading = header.state == SwitubeBrowseState_Loading;
        page.generation = header.generation; page.depth = header.depth;
        page.title = bounded(header.title); page.message = bounded(header.message);
        if (header.state == SwitubeBrowseState_Ready) {
            // Upstream 0.1.0 holds at most 32 entries. Reject an oversized
            // response rather than publishing a silently truncated playlist.
            if (header.item_count > 32) { error("Switube returned an oversized library page."); browsing = false; continue; }
            bool changed = false;
            for (u32 i = 0; i < header.item_count; ++i) {
                SwitubeIpcBrowseItem item{};
                rc = serviceDispatchInOut(&service, SwitubeIpcCommand_GetBrowseItem, i, item);
                if (R_FAILED(rc) || item.struct_size != sizeof(item) || item.abi_version != SWITUBE_IPC_BROWSE_ABI_VERSION
                    || item.generation != header.generation) { changed = true; break; }
                page.items.push_back(item);
            }
            if (R_FAILED(rc)) { failed(rc); continue; }
            if (changed) continue; // another client changed the page: retry a coherent snapshot
        }
        pageGeneration = header.generation; pageState = header.state;
        { std::lock_guard<std::mutex> lock(m_mu); m_page = std::move(page); m_pageNew = true; }
    }
    disconnect();
}
#endif
} // namespace cards
