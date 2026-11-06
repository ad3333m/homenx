#include "spotify/SpotifyPlayer.hpp"

#include <switch.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

constexpr u64 kProgram = 0x4200000000535754;
constexpr const char* kReady = "sdmc:/config/SpotifyPlayer/background-ready.log";
constexpr const char* kArtwork = "sdmc:/config/SpotifyPlayer/current-artwork-url";
constexpr std::uint64_t kPollMs = 250;
constexpr std::uint64_t kPidCheckMs = 1000;
constexpr std::uint64_t kArtworkCheckMs = 1500;
constexpr std::uint64_t kStartTimeoutMs = 30000;

Service g_session; // this service's own session (the music card has its own)

bool processPid(u64& pid) {
    pid = 0;
    if (R_FAILED(pmdmntInitialize())) return false;
    const Result rc = pmdmntGetProcessId(&pid, kProgram);
    pmdmntExit();
    return R_SUCCEEDED(rc) && pid != 0;
}

// Starts the console's Spotify player (a sysmodule that isn't started at boot),
// like the overlay's switch does.
Result launch() {
    Result rc = pmshellInitialize();
    if (R_FAILED(rc)) return rc;
    NcmProgramLocation where{};
    where.program_id = kProgram;
    where.storageID = NcmStorageId_None;
    u64 pid = 0;
    rc = pmshellLaunchProgram(0, &where, &pid);
    pmshellExit();
    return rc;
}

// Atmosphere defers GetService while the service isn't registered, which would
// block this thread for good. The player writes its PID to this file once its
// own IPC answers, so a matching one means GetService answers at once.
bool readyFor(u64 pid) {
    FILE* f = std::fopen(kReady, "r");
    if (!f) return false;
    unsigned long long marker = 0;
    unsigned result = 1;
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        unsigned long long m = 0;
        unsigned r = 1;
        if (std::sscanf(line, "pass-v043 %llX 0x%X", &m, &r) == 2) {
            marker = m;
            result = r;
        }
    }
    std::fclose(f);
    return marker == pid && result == 0;
}

std::string readArtworkUrl() {
    FILE* f = std::fopen(kArtwork, "r");
    if (!f) return {};
    char line[512] = {};
    const bool ok = std::fgets(line, sizeof(line), f) != nullptr;
    std::fclose(f);
    if (!ok) return {};
    std::string url(line);
    while (!url.empty() && (url.back() == '\n' || url.back() == '\r' || url.back() == ' ')) url.pop_back();
    return url.rfind("http", 0) == 0 ? url : std::string();
}

std::string hex(Result rc) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%X", (unsigned)rc);
    return b;
}

} // namespace

std::uint64_t SpotifyPlayer::nowMs() {
    using namespace std::chrono;
    return (std::uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void SpotifyPlayer::activate() {
    if (m_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_quit = false;
        m_cmds.clear();
    }
    m_launchedAt = 0;
    m_thread = std::thread([this] { run(); });
}

void SpotifyPlayer::deactivate() {
    if (!m_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_quit = true;
    }
    m_cv.notify_all();
    m_thread.join();
    std::lock_guard<std::mutex> lock(m_mu);
    m_pub.link = Link::Off;
}

SpotifyPlayer::Snapshot SpotifyPlayer::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_pub;
}

void SpotifyPlayer::post(std::uint32_t command, bool hasValue, std::uint32_t value) {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (!m_thread.joinable()) return;
        // Seek and volume repeat while held: keep only the newest one.
        if (command == PlayerIpcCommand_Seek || command == PlayerIpcCommand_SetVolume) {
            for (auto it = m_cmds.begin(); it != m_cmds.end();) {
                if (it->id == command) it = m_cmds.erase(it);
                else ++it;
            }
        }
        m_cmds.push_back({command, hasValue, value});
    }
    m_cv.notify_all();
}

void SpotifyPlayer::playPause() { post(PlayerIpcCommand_PlayPause, false, 0); }
void SpotifyPlayer::next() { post(PlayerIpcCommand_Next, false, 0); }
void SpotifyPlayer::previous() { post(PlayerIpcCommand_Previous, false, 0); }
void SpotifyPlayer::seek(std::uint32_t ms) { post(PlayerIpcCommand_Seek, true, ms); }
void SpotifyPlayer::setVolume(std::uint32_t permille) {
    post(PlayerIpcCommand_SetVolume, true, permille > 1000 ? 1000 : permille);
}
void SpotifyPlayer::setShuffle(bool on) { post(PlayerIpcCommand_SetShuffle, true, on ? 1 : 0); }
void SpotifyPlayer::setRepeat(PlayerRepeatMode mode) {
    post(PlayerIpcCommand_SetRepeatMode, true, (std::uint32_t)mode);
}

bool SpotifyPlayer::withSession(const std::function<void(void*)>& fn) {
    std::lock_guard<std::mutex> lock(m_ipcMu);
    if (!m_session) return false;
    fn(&g_session);
    return true;
}

void SpotifyPlayer::disconnect() {
    {
        std::lock_guard<std::mutex> lock(m_ipcMu);
        if (m_session) serviceClose(&g_session);
        m_session = false;
    }
    m_pid = 0;
    m_cur.apiVersion = 0;
    std::memset(&m_cur.status, 0, sizeof(m_cur.status));
}

void SpotifyPlayer::refresh() {
    const std::uint64_t now = nowMs();
    if (!m_session || now - m_pidCheckedAt >= kPidCheckMs) {
        m_pidCheckedAt = now;
        u64 pid = 0;
        if (!processPid(pid)) {
            disconnect();
            // Not running: start it, once a panel opening.
            if (m_launchedAt == 0) {
                m_launchedAt = now;
                const Result rc = launch();
                if (R_FAILED(rc)) {
                    m_cur.link = Link::Failed;
                    m_cur.error = "Start " + hex(rc);
                    return;
                }
            }
            const bool waiting = m_launchedAt != 0 && now - m_launchedAt < kStartTimeoutMs;
            m_cur.link = waiting ? Link::Starting : Link::NotRunning;
            return;
        }
        if (m_session && pid != m_pid) disconnect(); // the Spotify player restarted
        if (!m_session) {
            if (!readyFor(pid)) {
                m_cur.link = Link::Starting;
                return;
            }
            std::lock_guard<std::mutex> lock(m_ipcMu);
            const Result rc = smGetService(&g_session, PLAYER_IPC_SERVICE_NAME);
            if (R_FAILED(rc)) {
                m_cur.link = Link::Failed;
                m_cur.error = "IPC " + hex(rc);
                return;
            }
            m_session = true;
            m_pid = pid;
            u32 version = 0;
            m_cur.apiVersion = R_SUCCEEDED(serviceDispatchOut(&g_session, PlayerIpcCommand_GetApiVersion, version))
                                   ? version : 0;
        }
    }
    PlayerIpcStatus st{};
    const Result rc = serviceDispatchOut(&g_session, PlayerIpcCommand_GetStatus, st);
    if (R_FAILED(rc) || st.struct_size != sizeof(PlayerIpcStatus)) {
        disconnect();
        m_cur.link = Link::Failed;
        m_cur.error = "Status " + hex(rc);
        return;
    }
    const bool newSong = std::strncmp(st.title, m_cur.status.title, sizeof(st.title)) != 0;
    m_cur.status = st;
    m_cur.readAtMs = nowMs();
    m_cur.link = Link::Connected;
    if (newSong || now - m_artCheckedAt >= kArtworkCheckMs) {
        m_artCheckedAt = now;
        m_cur.artworkUrl = (st.flags & PlayerStatusFlag_HasTrack) ? readArtworkUrl() : std::string();
    }
}

void SpotifyPlayer::run() {
    m_cur = {};
    m_artCheckedAt = 0;
    m_pidCheckedAt = 0;
    while (true) {
        std::deque<Cmd> cmds;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (m_quit) break;
            cmds.swap(m_cmds);
        }
        for (const Cmd& c : cmds) {
            if (!m_session) break;
            const Result rc = c.hasValue ? serviceDispatchIn(&g_session, c.id, c.value)
                                         : serviceDispatch(&g_session, c.id);
            if (R_FAILED(rc)) m_cur.error = "Button refused (" + hex(rc) + ")";
            else m_cur.error.clear();
        }
        refresh();
        {
            std::unique_lock<std::mutex> lock(m_mu);
            m_cur.version = m_pub.version + 1;
            m_pub = m_cur;
            m_cv.wait_for(lock, std::chrono::milliseconds(kPollMs), [this] { return m_quit || !m_cmds.empty(); });
        }
    }
    disconnect();
}
