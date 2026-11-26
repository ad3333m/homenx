#ifdef HOMENX_NATIVE_YOUTUBE
#include "NativeYoutube.hpp"
#include "download_path.hpp"
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
    bool module = false;
    u32 queueRevision = UINT32_MAX;
    std::vector<Now::QueueTrack> moduleQueue;
#endif
    bool startupPending = false, browsing = false;
    auto startupDeadline = Clock::now();
    auto nextPoll = Clock::now();
    std::uint64_t pageGeneration = UINT64_MAX;
    std::uint32_t pageState = UINT32_MAX;
    auto disconnect = [&] {
#ifdef HOMENX_NATIVE_YOUTUBE
        module = false;
        queueRevision = UINT32_MAX;
        moduleQueue.clear();
#endif
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
        const bool onlinePlayback = haveRequest && request.browsing &&
            (request.browse == Browse::OnlinePlay || request.browse == Browse::OnlineQueue);
        auto connectModule = [&] {
            const auto name = smEncodeName(HOMENX_MUSIC_SERVICE);
            u8 registered = 0;
            Service candidate{};
            if (R_FAILED(tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, registered)) ||
                !registered || R_FAILED(smGetService(&candidate, HOMENX_MUSIC_SERVICE))) return false;
            if (serviceIsActive(&service))
                serviceDispatch(&service, SwitubeIpcCommand_Pause);
            serviceClose(&service);
            service = candidate;
            module = true;
            browsing = startupPending = false;
            return true;
        };
        if (!module && !connectModule() && (onlinePlayback || (haveRequest && request.browsing &&
            (request.browse == Browse::Playlists || request.browse == Browse::SavePlaylist)))) {
            Result rc = pmshellInitialize();
            if (R_SUCCEEDED(rc)) {
                NcmProgramLocation location{HOMENX_MUSIC_PROGRAM_ID, NcmStorageId_None, {}};
                u64 pid;
                rc = pmshellLaunchProgram(0, &location, &pid);
                pmshellExit();
            }
            for (int i = 0; R_SUCCEEDED(rc) && i < 40 && !m_onlineAbort && !connectModule(); ++i)
                svcSleepThread(100000000);
            if (!module) {
                char detail[96];
                std::snprintf(detail, sizeof(detail), "Could not start HomeNX Music (0x%X).", rc);
                std::lock_guard lock(m_mu);
                m_page = youtube.page();
                m_page.message = R_FAILED(rc) ? detail : "Music service did not register.";
                m_pageNew = true;
                continue;
            }
        }
        if (onlinePlayback) {
            const auto* track = youtube.selectedTrack(request.generation, request.index);
            std::string message = "This result is no longer available.";
            if (track) {
                SwitubeIpcOnlineTrack wire{};
                std::snprintf(wire.id, sizeof(wire.id), "%s", track->id.c_str());
                std::snprintf(wire.title, sizeof(wire.title), "%s", track->title.c_str());
                std::snprintf(wire.artist, sizeof(wire.artist), "%s", track->artist.c_str());
                wire.duration_ms = track->durationMs;
                bool play = request.browse == Browse::OnlinePlay;
                Result rc = serviceDispatchIn(&service, play ? SwitubeIpcCommand_PlayOnlineTrack : SwitubeIpcCommand_QueueOnlineTrack, wire);
                message = R_FAILED(rc) ? "Music service rejected the track." : play ? "Opening stream…" : "Sent to queue.";
            }
            std::lock_guard lock(m_mu);
            m_page = youtube.page(message);
            m_pageNew = true;
            browsing = false;
            continue;
        }
        if (haveRequest && request.browsing && youtube.handles(request.browse)) {
            Page page;
            try {
                page = youtube.browse(request.browse, request.generation, request.index, request.query);
            } catch (const std::exception& ex) {
                error(std::string("YouTube Music request failed: ") + ex.what());
            }
            if (!m_onlineAbort && page.online) {
                std::lock_guard lock(m_mu);
                m_page = std::move(page);
                m_pageNew = true;
            }
            browsing = false;
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
                u32 wireCommand;
#ifdef HOMENX_NATIVE_YOUTUBE
                if (module && request.browse == Browse::Home) wireCommand = SwitubeIpcCommand_BrowseLibrary;
                else if (module && request.browse == Browse::Playlists) wireCommand = SwitubeIpcCommand_ListPlaylists;
                else if (module && request.browse == Browse::SavePlaylist) wireCommand = SwitubeIpcCommand_SavePlaylist;
                else
#endif
                {
                    if (command >= std::size(cmds)) continue;
                    wireCommand = cmds[command];
                }
                rc = serviceDispatchIn(&service, wireCommand, wire);
                browsing = true; pageGeneration = UINT64_MAX; pageState = UINT32_MAX;
            } else {
                const Now n = decode(status, 0);
                u32 value = 0, cmd = 0;
                bool input = false;
                switch (request.cmd) {
                    case Cmd::PlayQueued:
#ifdef HOMENX_NATIVE_YOUTUBE
                        if (module) {
                            SwitubeIpcBrowseRequest wire{};
                            wire.generation = queueRevision;
                            wire.index = request.value;
                            rc = serviceDispatchIn(&service, SwitubeIpcCommand_PlayQueueItem, wire);
                            if (R_FAILED(rc)) failed(rc);
                        }
#endif
                        continue;
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
        if (module) {
            if (queueRevision != status.reserved0) {
                std::vector<Now::QueueTrack> tracks;
                for (u32 i = 0; i < std::min(status.queue_count, 256u); ++i) {
                    SwitubeIpcBrowseItem item{};
                    if (R_FAILED(serviceDispatchInOut(&service, SwitubeIpcCommand_GetQueueItem, i, item))
                        || item.generation != status.reserved0) break;
                    tracks.push_back({bounded(item.name), bounded(item.subtitle), bounded(item.detail)});
                }
                if (tracks.size() == status.queue_count) {
                    moduleQueue = std::move(tracks);
                    queueRevision = status.reserved0;
                }
            }
            snapshot.queue = moduleQueue;
            snapshot.queueIndex = status.queue_index < snapshot.queue.size() ? static_cast<int>(status.queue_index) : -1;
            if (snapshot.active && snapshot.queueIndex >= 0) {
                std::string_view id = snapshot.queue[snapshot.queueIndex].trackId;
                const char* directory = nullptr;
                if (id.starts_with("native:")) { id.remove_prefix(7); directory = switube::kNativeDownloadsDirectory; }
                else if (id.starts_with("switube:")) { id.remove_prefix(8); directory = switube::kDownloadsDirectory; }
                if (switube::validTrackId(id)) {
                    snapshot.cover = "https://i.ytimg.com/vi/" + std::string(id) + "/default.jpg";
                    if (directory) {
                        const auto file = directory + std::string(id) + ".jpg";
                        struct stat info{};
                        if (stat(file.c_str(), &info) == 0 && S_ISREG(info.st_mode)) snapshot.cover = file;
                    }
                }
            }
        } else {
            youtube.describeQueue(snapshot);
            snapshot.queueIndex = -1;
            if (snapshot.active && !module) {
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
#ifdef HOMENX_NATIVE_YOUTUBE
        page.background = module;
#endif
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
