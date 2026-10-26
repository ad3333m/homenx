#pragma once
#include <future>
#include <string>
#include <utility>
#include <vector>

struct _Mix_Music;
struct Mix_Chunk;

namespace cards {

// HomeNX's HOME music and sounds, from its install on the SD card
// (sdmc:/switch/SwitchU/sounds/wiiu): the music tracks in turn (or shuffled),
// and the sounds HOME makes when moving, choosing and opening things.
class Audio {
public:
    bool start();
    void stop();
    void update();                      // the next track once one ends
    void setMusic(bool on, int volume); // volume 0..100
    void setShuffle(bool on) { m_shuffle = on; }
    void setRepeat(bool on) { m_repeat = on; } // the same song again
    void skip(int dir);                 // the next (1) or previous (-1) track
    std::string trackName() const;      // what's playing, by its title ("" = nothing)
    void setSounds(bool on, int volume = -1);
    void play(const char* sound);       // "navigation", "activation", "launch_game", ...
    void fadeOut(int ms);               // before launching something
    void pause();                       // the console sleeps / an applet opens
    void resume();
    // The player (Quick Settings): paused or not, where it is, the songs.
    bool paused() const;
    void togglePause();
    float position() const { return m_position; }  // seconds into the song
    float duration() const;                         // its length (0 = not known)
    const std::vector<std::string>& titles() const { return m_titles; }
    long playing() const { return m_playing; }
    // The music's bars (the Music screen): what was just heard, as kBands
    // levels 0..1 from 50 Hz to 16 kHz.
    static constexpr int kBands = 48;
    void spectrum(float* out) const;
    void playTrack(size_t index);

private:
    void startTrack(size_t index);
    void loadWanted();

    bool m_open = false, m_sounds = true, m_music = true, m_shuffle = false, m_repeat = false;
    int m_volume = 60, m_soundVolume = 80;
    std::vector<std::string> m_tracks, m_titles;
    std::vector<float> m_lengths;  // seconds, 0 = not known
    float m_position = 0.f;
    unsigned long long m_lastTick = 0;
    size_t m_track = 0;   // the next one
    long m_playing = -1;  // the one playing
    _Mix_Music* m_current = nullptr;
    std::vector<unsigned char> m_trackData; // the song playing, in memory: SDL reads it, not the SD card
    bool m_isPlaying = false, m_userPaused = false, m_sysPaused = false; // ours: SDL's asks took its lock
    long m_wantTrack = -1, m_jobTrack = -1;  // the song to play next, and the one being read
    std::future<std::vector<unsigned char>> m_trackJob;
    struct Loaded {
        std::vector<std::string> tracks, titles;
        std::vector<float> lengths;
        std::vector<std::pair<std::string, std::vector<unsigned char>>> sounds;
    };
    std::future<Loaded> m_infoJob;
    std::vector<std::pair<std::string, Mix_Chunk*>> m_chunks;
};

} // namespace cards
