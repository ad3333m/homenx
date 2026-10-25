#include "Audio.hpp"

#include <SDL2/SDL_mixer.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace cards {
namespace {

constexpr const char* kSounds = "sdmc:/switch/SwitchU/sounds/wiiu";
std::atomic<bool> g_trackEnded{false};

void onTrackEnded() { g_trackEnded = true; } // SDL_mixer's thread: just a flag

void putUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// A song's title from its ID3v2 tag (the TIT2 frame), "" if it has none.
std::string id3Title(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char head[10];
    if (!in.read(reinterpret_cast<char*>(head), 10) || head[0] != 'I' || head[1] != 'D' || head[2] != '3') return {};
    const int version = head[3];
    auto syncsafe = [](const unsigned char* p) { return (p[0] << 21) | (p[1] << 14) | (p[2] << 7) | p[3]; };
    const size_t size = std::min<size_t>(static_cast<size_t>(syncsafe(head + 6)), 256 * 1024);
    std::string tag(size, '\0');
    in.read(&tag[0], static_cast<std::streamsize>(size));
    tag.resize(static_cast<size_t>(in.gcount()));
    const auto* t = reinterpret_cast<const unsigned char*>(tag.data());
    for (size_t i = 0; i + 10 <= tag.size();) {
        if (t[i] == 0) break; // padding
        const size_t frame = version >= 4 ? static_cast<size_t>(syncsafe(t + i + 4))
                                          : (static_cast<size_t>(t[i + 4]) << 24) | (t[i + 5] << 16) | (t[i + 6] << 8) | t[i + 7];
        if (frame == 0 || i + 10 + frame > tag.size()) break;
        if (tag.compare(i, 4, "TIT2") == 0) {
            const unsigned char* p = t + i + 11;
            const size_t n = frame - 1;
            std::string out;
            switch (t[i + 10]) { // the text's encoding
                case 0: // Latin-1
                    for (size_t k = 0; k < n; ++k) putUtf8(out, p[k]);
                    break;
                case 1: // UTF-16 with a byte order mark
                case 2: { // UTF-16 big endian
                    bool little = false;
                    size_t k = 0;
                    if (t[i + 10] == 1 && n >= 2) {
                        little = p[0] == 0xFF && p[1] == 0xFE;
                        k = 2;
                    }
                    for (; k + 1 < n; k += 2) putUtf8(out, little ? p[k] | (p[k + 1] << 8) : (p[k] << 8) | p[k + 1]);
                    break;
                }
                default: // UTF-8
                    out.assign(reinterpret_cast<const char*>(p), n);
                    break;
            }
            while (!out.empty() && (out.back() == '\0' || out.back() == ' ')) out.pop_back();
            return out;
        }
        i += 10 + frame;
    }
    return {};
}

// Without a tag: "bg_3" -> "Bg 3".
std::string fromFileName(const std::string& path) {
    std::string s = std::filesystem::path(path).stem().string();
    for (char& c : s)
        if (c == '_' || c == '-') c = ' ';
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

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
    for (const std::string& t : m_tracks) {
        const std::string title = id3Title(t);
        m_titles.push_back(title.empty() ? fromFileName(t) : title);
    }
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
    if (m_repeat && m_playing >= 0) startTrack(static_cast<size_t>(m_playing));
    else startTrack(m_shuffle && m_tracks.size() > 1 ? static_cast<size_t>(std::rand()) % m_tracks.size() : m_track);
}

void Audio::skip(int dir) {
    if (!m_open || m_tracks.empty()) return;
    m_music = true;
    const long n = static_cast<long>(m_tracks.size());
    const long from = m_playing < 0 ? 0 : m_playing;
    startTrack(static_cast<size_t>(((from + dir) % n + n) % n));
}

std::string Audio::trackName() const {
    if (m_playing < 0 || m_playing >= static_cast<long>(m_titles.size())) return {};
    return m_titles[(size_t)m_playing];
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
