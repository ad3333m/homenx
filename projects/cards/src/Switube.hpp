#pragma once
#include <switube/ipc_protocol.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef HOMENX_NATIVE_YOUTUBE
#include <switch.h>
#endif

namespace cards {

// Switube 0.1.0 background player. All service and SD access runs on a worker;
// the UI only copies snapshots and queues commands. No Spotify account is used.
class Switube {
public:
    struct Now {
        bool connected = false, active = false, playing = false, shuffle = false;
        bool online = false;
        int repeat = 0; // UI order: off, all, one (Switube's wire order differs)
        int progressMs = 0, durationMs = 0, volume = -1;
        std::string track, artist, cover;
        struct QueueTrack { std::string title, artist; std::string trackId{}; };
        std::vector<QueueTrack> queue;
        int queueIndex = -1;
        std::uint64_t at = 0;
    };
    enum class Cmd { PlayPause, Next, Previous, Shuffle, Repeat, Volume, PlayQueued };
    enum class Browse { Home, Library, Search, Open, Back, Play, OnlineHome, OnlineSearch, OnlinePlay, OnlineDownload, OnlineQueue, Playlists, SavePlaylist };
    struct OnlineTrack {
        std::string title, artist, cover;
    };
    struct Page {
        bool loading = false, online = false, background = false;
        std::uint64_t generation = 0;
        unsigned depth = 0;
        std::string title, message;
        std::vector<SwitubeIpcBrowseItem> items;
        std::vector<OnlineTrack> onlineTracks;
    };

    ~Switube() { stop(); }
    void start();
    void stop();
    void setWatching(bool on);
    Now now() const;
    void command(Cmd cmd, int value = 0);
    void browse(Browse cmd, std::uint64_t generation = 0, unsigned index = 0, std::string query = {});
    bool takePage(Page& page);
    std::string takeError();

    static bool valid(const SwitubeIpcStatus& status);
    static Now decode(const SwitubeIpcStatus& status, std::uint64_t tick);
    static std::uint32_t repeatWire(int uiRepeat);

private:
    struct Request {
        bool browsing = false;
        Cmd cmd = Cmd::PlayPause;
        int value = 0;
        Browse browse = Browse::Home;
        std::uint64_t generation = 0;
        unsigned index = 0;
        std::string query;
    };
    void run();
    mutable std::mutex m_mu;
    std::condition_variable m_cv;
#ifdef HOMENX_NATIVE_YOUTUBE
    Thread m_thread{};
#else
    std::thread m_thread;
#endif
    std::atomic<bool> m_onlineAbort{true};
    bool m_quit = false, m_watching = false, m_pageNew = false;
    std::deque<Request> m_requests;
    Now m_now;
    Page m_page;
    std::string m_error;
};

} // namespace cards
