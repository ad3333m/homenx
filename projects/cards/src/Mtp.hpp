#pragma once
#include <atomic>
#include <thread>

namespace cards {

// USB file transfer (MTP) from HOME itself: plugged into a computer, the SD card
// shows up there like a phone's storage (copy, delete, new folders, rename).
// Settings > Console > Connect to a computer. Written for HomeNX from the MTP
// spec (Atmosphere's haze is GPLv2-only; HomeNX is GPL-3).
class Mtp {
public:
    enum class State { Off, Waiting, Connected, Failed };
    ~Mtp() { stop(); }
    void start();
    void stop();
    bool on() const { return m_thread.joinable(); }
    State state() const { return m_state; }

private:
    void run();
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<State> m_state{State::Off};
};

} // namespace cards
