#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cards {

// Spotify in HOME's music player: what's playing on your phone, computer or
// speaker (Spotify Connect, through Spotify's Web API) and its buttons. The
// Switch doesn't play Spotify's audio itself.
//
// Signing in: a QR code opens this console's little web server (same Wi-Fi),
// which sends the phone to Spotify's sign-in; Spotify answers at
// homenx.pages.dev/spotify/, which hands the answer back to this console. The
// sign-in stays in sdmc:/config/CardsNX/spotify.json. Each person uses their own
// Spotify app's Client ID (developer.spotify.com, redirect URI
// https://homenx.pages.dev/spotify/): Spotify lets an app have 25 users unless it
// approves it, and it doesn't approve apps of people on their own.
// (Built in HOME only: Spotify.cpp is empty in the .nro.)
class Spotify {
public:
    struct Now {
        bool active = false;  // something is playing or paused on a device
        bool playing = false, shuffle = false;
        int repeat = 0;       // 0 off, 1 the playlist, 2 the song
        int progressMs = 0, durationMs = 0, volume = -1;
        std::string track, artist, device, coverUrl;
        std::uint64_t at = 0; // the tick it was read at (the song moves on from there)
    };
    enum class Login { Idle, Waiting, Exchanging, Done, Failed };
    enum class Cmd { PlayPause, Next, Previous, Shuffle, Repeat, Volume };

    ~Spotify() { stop(); }
    void start(); // its thread (once the network is up)
    void stop();
    bool started() const { return m_thread.joinable(); }

    bool signedIn() const;
    std::string clientId() const;
    void setClientId(const std::string& id);
    void signOut();
    // The link for the QR code (this console's address); "" and why when it can't.
    std::string beginLogin(const std::string& consoleIp, std::string& error);
    void cancelLogin();
    Login login() const;
    std::string loginError() const;

    void setWatching(bool on); // what's playing, every few seconds, while on
    Now now() const;
    void command(Cmd c, int value = 0);
    std::string takeError(); // a button Spotify refused (Premium, no device), once
    bool takeCover(std::vector<std::uint8_t>& rgba, int& w, int& h); // decoded, when it changed

private:
    void run();
    void load();
    void save(); // (under m_mu)
    bool openServer();
    void closeServer();
    void serveOnce();
    void exchange();
    bool refresh();
    long api(const char* method, const std::string& path, std::string& out);
    void poll();
    void doCommand(Cmd c, int value);

    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    std::thread m_thread;
    bool m_quit = false, m_watching = false;
    std::deque<std::pair<Cmd, int>> m_cmds;

    std::string m_clientId, m_access, m_refresh;
    long long m_expires = 0; // seconds since 1970
    Now m_now;
    std::string m_error;
    std::vector<std::uint8_t> m_cover;
    int m_coverW = 0, m_coverH = 0;
    bool m_coverNew = false;
    std::string m_coverFor;
    std::uint64_t m_pollAt = 0;

    Login m_login = Login::Idle;
    std::string m_loginError, m_verifier, m_state, m_code;
    int m_server = -1;
};

} // namespace cards
