#include "Audio.hpp"

#include <SDL2/SDL_mixer.h>
#include <nxui/core/ThreadPool.hpp>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
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

// An MP3's length: its Xing/Info frame count if it has one, else its size at
// the first frame's bit rate. 0 when it can't tell (not an MP3).
float mp3Seconds(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return 0.f;
    const long long fileSize = static_cast<long long>(in.tellg());
    in.seekg(0);
    std::vector<unsigned char> b(64 * 1024);
    in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
    b.resize(static_cast<size_t>(in.gcount()));
    size_t at = 0;
    if (b.size() > 10 && b[0] == 'I' && b[1] == 'D' && b[2] == '3')
        at = 10 + ((b[6] << 21) | (b[7] << 14) | (b[8] << 7) | b[9]);
    long long audioStart = static_cast<long long>(at);
    if (at >= b.size()) { // a big tag: read on from after it
        in.clear();
        in.seekg(static_cast<std::streamoff>(at));
        b.assign(4096, 0);
        in.read(reinterpret_cast<char*>(b.data()), 4096);
        b.resize(static_cast<size_t>(in.gcount()));
        at = 0;
    }
    for (; at + 4 < b.size(); ++at)
        if (b[at] == 0xFF && (b[at + 1] & 0xE0) == 0xE0) break;
    if (at + 4 >= b.size()) return 0.f;
    const int version = (b[at + 1] >> 3) & 3; // 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5
    const int layer = (b[at + 1] >> 1) & 3;   // 1 = Layer III
    const int rateIndex = (b[at + 2] >> 4) & 15, freqIndex = (b[at + 2] >> 2) & 3;
    const bool mono = ((b[at + 3] >> 6) & 3) == 3;
    if (layer != 1 || version == 1 || rateIndex == 0 || rateIndex == 15 || freqIndex == 3) return 0.f;
    static const int kRates1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int kRates2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    static const int kFreqs[] = {44100, 48000, 32000};
    const bool mpeg1 = version == 3;
    const int kbps = (mpeg1 ? kRates1 : kRates2)[rateIndex];
    const int freq = kFreqs[freqIndex] >> (mpeg1 ? 0 : version == 2 ? 1 : 2);
    const int samples = mpeg1 ? 1152 : 576;
    // A Xing/Info frame (variable bit rate): the number of frames.
    const size_t side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    const size_t x = at + 4 + side;
    if (x + 12 <= b.size() && (std::equal(b.begin() + x, b.begin() + x + 4, "Xing") || std::equal(b.begin() + x, b.begin() + x + 4, "Info"))
        && (b[x + 7] & 1)) {
        const unsigned frames = (static_cast<unsigned>(b[x + 8]) << 24) | (b[x + 9] << 16) | (b[x + 10] << 8) | b[x + 11];
        return static_cast<float>(frames) * samples / freq;
    }
    return static_cast<float>(fileSize - audioStart) * 8.f / (kbps * 1000.f);
}

// A whole file (empty if it isn't there).
std::vector<unsigned char> readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    std::vector<unsigned char> data(static_cast<size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(in.gcount()));
    return data;
}

// Off the screen's core, below its priority (std::async threads start on its).
void offTheScreensCore() {
    nxui::moveToWorkerCores();
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
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

// The songs, their titles and lengths, and the sounds: read on a thread (the
// SD card can be busy for seconds; HOME's start waited 250 ms on these).
bool Audio::start() {
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 4096) < 0) return false;
    Mix_AllocateChannels(8);
    m_open = true;
    Mix_Volume(-1, MIX_MAX_VOLUME * m_soundVolume / 100);
    Mix_HookMusicFinished(onTrackEnded);
    m_infoJob = std::async(std::launch::async, []() {
        offTheScreensCore();
        Loaded l;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(std::string(kSounds) + "/music", ec)) {
            const std::string ext = e.path().extension().string();
            if (ext == ".mp3" || ext == ".ogg" || ext == ".MP3" || ext == ".OGG") l.tracks.push_back(e.path().string());
        }
        std::sort(l.tracks.begin(), l.tracks.end());
        for (const std::string& t : l.tracks) {
            const std::string title = id3Title(t);
            l.titles.push_back(title.empty() ? fromFileName(t) : title);
            l.lengths.push_back(mp3Seconds(t));
        }
        for (const char* name : {"navigation", "activation", "confirm", "launch_game", "show_modal", "hide_modal",
                                 "tab_transition", "toggle_on", "toggle_off"})
            l.sounds.emplace_back(name, readAll(std::string(kSounds) + "/sfx/" + name + ".wav"));
        return l;
    });
    return true;
}

void Audio::stop() {
    if (!m_open) return;
    if (m_infoJob.valid()) m_infoJob.wait();
    if (m_trackJob.valid()) m_trackJob.wait();
    Mix_HookMusicFinished(nullptr);
    Mix_HaltMusic();
    Mix_HaltChannel(-1);
    if (m_current) Mix_FreeMusic(m_current);
    m_current = nullptr;
    m_trackData.clear();
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
        m_isPlaying = false;
        m_playing = m_wantTrack = -1;
        g_trackEnded = false;
        return;
    }
    if (!m_isPlaying && m_wantTrack < 0) g_trackEnded = true; // start one now (update)
}

// The song is read into memory on a thread and played from there (update):
// streamed from the SD card, SDL's audio thread read it while holding the
// lock every sound and frame needed, so a busy card froze the screen.
void Audio::startTrack(size_t index) {
    if (m_tracks.empty()) return;
    index %= m_tracks.size();
    g_trackEnded = false;
    m_playing = static_cast<long>(index);
    m_track = index + 1;
    m_position = 0.f;
    m_wantTrack = static_cast<long>(index);
    loadWanted();
}

void Audio::loadWanted() {
    if (m_trackJob.valid() || m_wantTrack < 0) return; // one at a time: the newest next
    m_jobTrack = m_wantTrack;
    const std::string path = m_tracks[(size_t)m_jobTrack];
    m_trackJob = std::async(std::launch::async, [path]() {
        offTheScreensCore();
        return readAll(path);
    });
}

void Audio::update() {
    // Where the song is (for the progress bar): playing time since the last frame.
    const unsigned long long now = armGetSystemTick();
    if (m_lastTick && m_isPlaying && !m_userPaused && !m_sysPaused)
        m_position += static_cast<float>(armTicksToNs(now - m_lastTick)) / 1e9f;
    m_lastTick = now;
    if (!m_open) return;
    if (m_infoJob.valid()) { // the songs and sounds are read
        if (m_infoJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        Loaded l = m_infoJob.get();
        m_tracks = std::move(l.tracks);
        m_titles = std::move(l.titles);
        m_lengths = std::move(l.lengths);
        for (auto& [name, bytes] : l.sounds)
            if (!bytes.empty())
                if (SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), static_cast<int>(bytes.size())))
                    if (Mix_Chunk* c = Mix_LoadWAV_RW(rw, 1)) m_chunks.push_back({name, c}); // decoded: the bytes can go
    }
    if (m_trackJob.valid() && m_trackJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::vector<unsigned char> data = m_trackJob.get();
        if (m_jobTrack == m_wantTrack && m_music) { // the one wanted (not one skipped past meanwhile)
            m_wantTrack = -1;
            Mix_HookMusicFinished(nullptr); // halting the old one isn't "it ended"
            Mix_HaltMusic();
            Mix_HookMusicFinished(onTrackEnded);
            g_trackEnded = false;
            if (m_current) Mix_FreeMusic(m_current);
            m_current = nullptr;
            m_trackData = std::move(data); // after the old song let go of its bytes
            if (!m_trackData.empty())
                if (SDL_RWops* rw = SDL_RWFromConstMem(m_trackData.data(), static_cast<int>(m_trackData.size())))
                    m_current = Mix_LoadMUS_RW(rw, 1);
            m_isPlaying = m_current != nullptr;
            m_userPaused = false;
            m_position = 0.f;
            if (m_current) {
                Mix_VolumeMusic(MIX_MAX_VOLUME * m_volume / 100);
                Mix_FadeInMusic(m_current, 1, 600);
                if (m_sysPaused) Mix_PauseMusic();
            }
        }
        loadWanted(); // one asked for while this was read
    }
    if (!m_music || m_tracks.empty() || !g_trackEnded.exchange(false)) return;
    m_isPlaying = false;
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
    if (m_open && m_isPlaying) Mix_FadeOutMusic(ms);
}

bool Audio::paused() const {
    return !m_open || !m_isPlaying || m_userPaused;
}

void Audio::togglePause() {
    if (!m_open) return;
    if (!m_isPlaying) { // nothing playing: a song now
        m_music = true;
        if (m_wantTrack < 0) g_trackEnded = true;
        return;
    }
    m_userPaused = !m_userPaused;
    if (m_userPaused) Mix_PauseMusic();
    else if (!m_sysPaused) Mix_ResumeMusic();
}

float Audio::duration() const {
    return m_playing >= 0 && m_playing < static_cast<long>(m_lengths.size()) ? m_lengths[(size_t)m_playing] : 0.f;
}

void Audio::playTrack(size_t index) {
    if (!m_open || index >= m_tracks.size()) return;
    m_music = true;
    startTrack(index);
}

void Audio::pause() {
    m_sysPaused = true;
    if (m_open) Mix_PauseMusic();
}

void Audio::resume() {
    m_sysPaused = false;
    if (m_open && !m_userPaused) Mix_ResumeMusic();
}

} // namespace cards
