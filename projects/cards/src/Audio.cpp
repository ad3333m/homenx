#include "Audio.hpp"

#include <SDL2/SDL_mixer.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
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
    Mix_Volume(-1, MIX_MAX_VOLUME * m_soundVolume / 100);
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
        m_playing = -1;
        g_trackEnded = false;
        return;
    }
    if (!Mix_PlayingMusic()) g_trackEnded = true; // start one now (update)
}

void Audio::startTrack(size_t index) {
    if (m_tracks.empty()) return;
    index %= m_tracks.size();
    Mix_HookMusicFinished(nullptr); // halting the old one isn't "it ended"
    Mix_HaltMusic();
    Mix_HookMusicFinished(onTrackEnded);
    g_trackEnded = false;
    if (m_current) Mix_FreeMusic(m_current);
    m_current = Mix_LoadMUS(m_tracks[index].c_str());
    m_playing = static_cast<long>(index);
    m_track = index + 1;
    if (m_current) {
        Mix_VolumeMusic(MIX_MAX_VOLUME * m_volume / 100);
        Mix_FadeInMusic(m_current, 1, 600);
    }
}

void Audio::update() {
    if (!m_open || !m_music || m_tracks.empty() || !g_trackEnded.exchange(false)) return;
    startTrack(m_shuffle && m_tracks.size() > 1 ? static_cast<size_t>(std::rand()) % m_tracks.size() : m_track);
}

void Audio::skip(int dir) {
    if (!m_open || m_tracks.empty()) return;
    m_music = true;
    const long n = static_cast<long>(m_tracks.size());
    const long from = m_playing < 0 ? 0 : m_playing;
    startTrack(static_cast<size_t>(((from + dir) % n + n) % n));
}

std::string Audio::trackName() const {
    if (m_playing < 0 || m_playing >= static_cast<long>(m_tracks.size())) return {};
    return std::filesystem::path(m_tracks[(size_t)m_playing]).stem().string();
}

void Audio::setSounds(bool on, int volume) {
    m_sounds = on;
    if (volume >= 0) m_soundVolume = std::clamp(volume, 0, 100);
    if (m_open) Mix_Volume(-1, MIX_MAX_VOLUME * m_soundVolume / 100);
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

void Audio::pause() {
    if (m_open) Mix_PauseMusic();
}

void Audio::resume() {
    if (m_open) Mix_ResumeMusic();
}

} // namespace cards
