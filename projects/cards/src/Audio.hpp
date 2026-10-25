#pragma once
#include <string>
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
    void skip(int dir);                 // the next (1) or previous (-1) track
    std::string trackName() const;      // what's playing ("" = nothing)
    void setSounds(bool on, int volume = -1);
    void play(const char* sound);       // "navigation", "activation", "launch_game", ...
    void fadeOut(int ms);               // before launching something
    void pause();                       // the console sleeps / an applet opens
    void resume();

private:
    void startTrack(size_t index);

    bool m_open = false, m_sounds = true, m_music = true, m_shuffle = false;
    int m_volume = 60, m_soundVolume = 80;
    std::vector<std::string> m_tracks;
    size_t m_track = 0;   // the next one
    long m_playing = -1;  // the one playing
    _Mix_Music* m_current = nullptr;
    std::vector<std::pair<std::string, Mix_Chunk*>> m_chunks;
};

} // namespace cards
