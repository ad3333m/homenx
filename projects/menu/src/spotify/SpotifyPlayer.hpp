#pragma once
#include "spotify/player_ipc_protocol.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// HOME's link to the console's Spotify player (title 4200000000535754) through
// its IPC service. HOME never plays the audio: it reads the status
// and sends the buttons. The thread and the IPC session only live while
// something needs them (the Spotify panel is open): activate()/deactivate().
class SpotifyPlayer {
public:
    enum class Link { Off, NotRunning, Starting, Connected, Failed };
    struct Snapshot {
        Link link = Link::Off;
        PlayerIpcStatus status{};
        std::uint64_t readAtMs = 0;  // when status was read (the song moves on from there)
        std::string artworkUrl;      // the current song's cover, from the Spotify player's file
        std::string error;           // the last refused button, if any
        std::uint32_t apiVersion = 0; // the service's IPC API (major << 16 | minor)
        std::uint64_t version = 0;
    };

    ~SpotifyPlayer() { deactivate(); }
    void activate();
    void deactivate();
    bool active() const { return m_thread.joinable(); }

    Snapshot snapshot() const;

    void playPause();
    void next();
    void previous();
    void seek(std::uint32_t ms);
    void setVolume(std::uint32_t permille);
    void setShuffle(bool on);
    void setRepeat(PlayerRepeatMode mode);

    // Runs fn with the open IPC session (a libnx Service*) from any thread;
    // false, without calling it, while there is none.
    bool withSession(const std::function<void(void* service)>& fn);

    static std::uint64_t nowMs();

private:
    void run();
    void post(std::uint32_t command, bool hasValue, std::uint32_t value);
    void refresh();
    void disconnect();

    mutable std::mutex m_mu;
    std::mutex m_ipcMu;  // the session's opening and closing, against withSession()
    std::condition_variable m_cv;
    std::thread m_thread;
    bool m_quit = false;
    struct Cmd { std::uint32_t id; bool hasValue; std::uint32_t value; };
    std::deque<Cmd> m_cmds;
    Snapshot m_pub;  // published (under m_mu)

    // Worker only.
    Snapshot m_cur;
    std::uint64_t m_pid = 0, m_pidCheckedAt = 0;
    std::uint64_t m_artCheckedAt = 0;
    std::uint64_t m_launchedAt = 0;  // when this panel opening started the player (0: not yet)
    bool m_session = false;
};
