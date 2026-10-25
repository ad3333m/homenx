#pragma once
#include <string>
#include <vector>

struct _Mix_Music;
struct Mix_Chunk;

namespace cards {

// HomeNX's HOME music and sounds, from its install on the SD card
// (sdmc:/switch/SwitchU/sounds/wiiu): the music tracks in turn, and the
// sounds HOME makes when moving, choosing and opening things.
class Audio {
public:
    bool start();
    void stop();
    void update();                     // the next track once one ends
    void setMusic(bool on, int volume); // volume 0..100
    void setSounds(bool on) { m_sounds = on; }
    void play(const char* sound);      // "navigation", "activation", "launch_game", ...
    void fadeOut(int ms);              // before launching something

private:
    bool m_open = false, m_sounds = true, m_music = true;
    int m_volume = 60;
    std::vector<std::string> m_tracks;
    size_t m_track = 0;
    _Mix_Music* m_current = nullptr;
    std::vector<std::pair<std::string, Mix_Chunk*>> m_chunks;
};

} // namespace cards
