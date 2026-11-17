#pragma once
#include <switube/ipc_protocol.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cards {

// Switube 0.1.0 background player. All service and SD access runs on a worker;
// the UI only copies snapshots and queues commands. No Spotify account is used.
class Switube {
public:
    struct Now {
        bool connected = false, active = false, playing = false, shuffle = false;
        int repeat = 0; // UI order: off, all, one (Switube's wire order differs)
        int progressMs = 0, durationMs = 0, volume = -1;
        std::string track, artist;
        std::uint64_t at = 0;
    };
    enum class Cmd { PlayPause, Next, Previous, Shuffle, Repeat, Volume };
    enum class Browse { Home, Library, Search, Open, Back, Play };
    struct Page {
        bool loading = false;
        std::uint64_t generation = 0;
        unsigned depth = 0;
        std::string title, message;
        std::vector<SwitubeIpcBrowseItem> items;
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
    std::thread m_thread;
    bool m_quit = false, m_watching = false, m_pageNew = false;
    std::deque<Request> m_requests;
    Now m_now;
    Page m_page;
    std::string m_error;
};

} // namespace cards
