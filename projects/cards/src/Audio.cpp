#include "Audio.hpp"

#include <SDL2/SDL_mixer.h>

#include <algorithm>
#include <atomic>
#include <filesystem>

namespace cards {
namespace {

constexpr const char* kSounds = "sdmc:/switch/SwitchU/sounds/wiiu";
std::atomic<bool> g_trackEnded{false};

void onTrackEnded() { g_trackEnded = true; } // SDL_mixer's thread: just a flag

} // namespace

bool Audio::start() {
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 4096) < 0) return false;
    Mix_AllocateChannels(8);
    m_open = true;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::string(kSounds) + "/music", ec)) {
        const std::string ext = e.path().extension().string();
        if (ext == ".mp3" || ext == ".ogg" || ext == ".MP3" || ext == ".OGG") m_tracks.push_back(e.path().string());
    }
    std::sort(m_tracks.begin(), m_tracks.end());
    for (const char* name : {"navigation", "activation", "confirm", "launch_game", "show_modal", "hide_modal",
                             "tab_transition", "toggle_on", "toggle_off"}) {
        if (Mix_Chunk* c = Mix_LoadWAV((std::string(kSounds) + "/sfx/" + name + ".wav").c_str()))
            m_chunks.push_back({name, c});
    }
    Mix_HookMusicFinished(onTrackEnded);
    return true;
}

void Audio::stop() {
    if (!m_open) return;
    Mix_HookMusicFinished(nullptr);
    Mix_HaltMusic();
    Mix_HaltChannel(-1);
    if (m_current) Mix_FreeMusic(m_current);
    m_current = nullptr;
    for (auto& [name, c] : m_chunks) Mix_FreeChunk(c);
    m_chunks.clear();
    Mix_CloseAudio();
    m_open = false;
}

void Audio::setMusic(bool on, int volume) {
    m_music = on;
    m_volume = std::clamp(volume, 0, 100);
    if (!m_open) return;
    Mix_VolumeMusic(MIX_MAX_VOLUME * m_volume / 100);
    if (!on) {
        Mix_HaltMusic();
        return;
    }
    if (!Mix_PlayingMusic()) g_trackEnded = true; // start one now (update)
}

void Audio::update() {
    if (!m_open || !m_music || m_tracks.empty() || !g_trackEnded.exchange(false)) return;
    if (m_current) Mix_FreeMusic(m_current);
    m_current = Mix_LoadMUS(m_tracks[m_track % m_tracks.size()].c_str());
    ++m_track;
    if (m_current) {
        Mix_VolumeMusic(MIX_MAX_VOLUME * m_volume / 100);
        Mix_FadeInMusic(m_current, 1, 600);
    }
}

void Audio::play(const char* sound) {
    if (!m_open || !m_sounds) return;
    for (auto& [name, c] : m_chunks)
        if (name == sound) {
            Mix_PlayChannel(-1, c, 0);
            return;
        }
}

void Audio::fadeOut(int ms) {
    if (m_open && Mix_PlayingMusic()) Mix_FadeOutMusic(ms);
}

} // namespace cards
