#pragma once
// An Album video (.mp4 on the SD card), played in CardsNX: ffmpeg decodes it on a thread to RGBA
// at 960x540 (the screen shows it scaled up), timed to its sound; the sound goes to SDL_mixer in
// place of HOME's music, which carries on where it was afterwards. Built only when the build found
// ffmpeg (CARDS_VIDEO); without it open() says no.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cards {

class VideoPlayer {
public:
    VideoPlayer() = default;
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    bool open(const std::string& path); // starts playing it
    void close();

    // The newest frame, when one came since the last call (RGBA, w*h*4).
    bool takeFrame(std::vector<std::uint8_t>& rgba, int& w, int& h);
    float position() const { return m_pos.load(); }
    float duration() const { return m_dur.load(); }
    bool paused() const { return m_paused.load(); }
    bool ended() const { return m_ended.load(); }
    void togglePause() { m_paused.store(!m_paused.load()); }
    void skip(float seconds); // from where it is now

    // SDL_mixer's music hook: the sound, as it's needed.
    static void mixHook(void* self, std::uint8_t* stream, int len);

private:
    void run(std::string path);
    double clock() const;

    std::thread m_thread;
    std::atomic<bool> m_stop{false}, m_paused{false}, m_ended{false};
    std::atomic<float> m_pos{0.f}, m_dur{0.f};
    std::atomic<float> m_seekBy{0.f};
    std::atomic<bool> m_seekWanted{false};

    std::mutex m_frameMu;
    std::vector<std::uint8_t> m_frame;
    int m_w = 0, m_h = 0;
    bool m_fresh = false;

    // The sound: decoded samples waiting for the mixer (16-bit stereo at its rate).
    std::mutex m_soundMu;
    std::vector<std::int16_t> m_sound;
    size_t m_soundAt = 0;
    int m_rate = 0;
    std::atomic<std::uint64_t> m_heard{0}; // sample frames the mixer has taken
    std::atomic<double> m_base{0.0};       // the time m_heard (or the wall clock) counts from
    std::atomic<std::uint64_t> m_wallStart{0};
    bool m_hooked = false;
};

} // namespace cards
