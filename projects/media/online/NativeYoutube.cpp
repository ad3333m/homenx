#include "NativeYoutube.hpp"
#include "NXConsoleLog.h"
#include "download_path.hpp"
#include "media_transfer.hpp"
#include "themeshop/ThemeHttp.hpp"
#include "transport.hpp"
#include "yt_audio_player.hpp"

#include <switch.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <cerrno>
#include <sys/stat.h>
#include <memory>
#include <random>
#include <utility>

namespace cards {
namespace {

using switube::YtAudioPlayer;

std::string coverOf(const switube::YtTrack& track) {
    return track.imageUrl.empty() ? "https://i.ytimg.com/vi/" + track.id + "/default.jpg"
                                  : track.imageUrl;
}

bool regularFile(const std::string& path) {
    struct stat info{};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
}

struct PartialFile {
    explicit PartialFile(const std::string& destination)
        : path(destination + ".part"), file(std::fopen(path.c_str(), "wb"), std::fclose) {}
    ~PartialFile() {
        file.reset();
        if (!path.empty()) std::remove(path.c_str());
    }
    bool commit(const std::string& destination) {
        if (std::fclose(file.release()) != 0 || std::rename(path.c_str(), destination.c_str()) != 0)
            return false;
        path.clear();
        return true;
    }

    std::string path;
    std::unique_ptr<FILE, decltype(&std::fclose)> file;
};

} // namespace

NativeYoutube::~NativeYoutube() {
    stop();
    switube::native::cancel = nullptr;
}

std::vector<std::uint8_t> NativeYoutube::fetchCover(const std::string& url,
                                                    const std::atomic<bool>& cancelled) {
    constexpr std::size_t MaxCoverBytes = 128 * 1024;
    constexpr std::string_view nativeDir = switube::kNativeDownloadsDirectory;
    constexpr std::string_view legacyDir = switube::kDownloadsDirectory;
    if (!cancelled && (url.starts_with(nativeDir) || url.starts_with(legacyDir))) {
        const auto name = std::string_view(url).substr(url.starts_with(nativeDir) ? nativeDir.size() : legacyDir.size());
        if (!name.ends_with(".jpg") || !switube::validTrackId(name.substr(0, name.size() - 4))) return {};
        std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(url.c_str(), "rb"), std::fclose);
        if (!file) return {};
        std::vector<std::uint8_t> bytes(MaxCoverBytes);
        const auto read = std::fread(bytes.data(), 1, bytes.size(), file.get());
        if (std::ferror(file.get()) || read == bytes.size()) return {};
        bytes.resize(read);
        return bytes;
    }
    if (cancelled || url.size() > 2048 || url.rfind("https://", 0) != 0) return {};
    const auto address = std::string_view(url).substr(8);
    const auto authority = address.substr(0, address.find('/'));
    if (authority != "i.ytimg.com" && !authority.ends_with(".googleusercontent.com") &&
        !authority.ends_with(".ggpht.com")) return {};
    if (authority.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") != std::string_view::npos)
        return {};

    if (!themeshop::http::initialize()) return {};
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return {};
    std::vector<std::uint8_t> bytes;
    auto* handle = curl.get();
    switube::native::configure(handle, &cancelled);
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &bytes);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, +[](char* data, size_t size, size_t count, void* context) -> size_t {
        auto& output = *static_cast<std::vector<std::uint8_t>*>(context);
        const auto length = size * count;
        if (length > MaxCoverBytes - output.size()) return 0;
        try {
            output.insert(output.end(), data, data + length);
        } catch (...) {
            return 0;
        }
        return length;
    });
    if (switchu::tls::perform(handle, url, switchu::tls::kHomeCerts, [&] { bytes.clear(); }) != CURLE_OK)
        return {};
    return bytes;
}

bool NativeYoutube::handles(Switube::Browse cmd) const {
    return cmd == Switube::Browse::OnlineHome || cmd == Switube::Browse::OnlineSearch ||
        cmd == Switube::Browse::OnlinePlay || cmd == Switube::Browse::OnlineDownload ||
        cmd == Switube::Browse::OnlineQueue;
}

bool NativeYoutube::ready() {
    if (m_cancel) return false;
    switube::native::cancel = &m_cancel;
    if (!m_initialized && themeshop::http::initialize()) {
        std::error_code ec;
        std::filesystem::create_directories("sdmc:/config/SwitchU/media", ec);
        nxlog::setQueueEnabled(false);
        nxlog::setLogFile("sdmc:/config/SwitchU/media/youtube.log", 64 * 1024);
        m_initialized = m_client.init();
    }
    return m_initialized;
}

Switube::Page NativeYoutube::page(std::string message) const {
    Switube::Page result;
    result.online = true;
    result.generation = m_generation;
    result.title = m_title;
    result.message = std::move(message);
    result.onlineTracks.reserve(m_tracks.size());
    for (const auto& track : m_tracks)
        result.onlineTracks.push_back({track.title, track.artist, coverOf(track)});
    return result;
}

Switube::Page NativeYoutube::browse(Switube::Browse cmd, std::uint64_t generation, unsigned index,
                                   const std::string& query) {
    using Browse = Switube::Browse;
    if (cmd == Browse::OnlinePlay || cmd == Browse::OnlineDownload || cmd == Browse::OnlineQueue) {
        if (generation != m_generation || index >= m_tracks.size())
            return page("This result is no longer available.");
        if (cmd == Browse::OnlineQueue || cmd == Browse::OnlinePlay) {
            constexpr std::size_t MaxQueue = 64;
            if (m_queue.size() >= MaxQueue && m_index > 0) {
                m_queue.erase(m_queue.begin(), m_queue.begin() + m_index);
                m_index = 0;
            }
            if (m_queue.size() >= MaxQueue) return page("The queue is full.");
            if (cmd == Browse::OnlineQueue) {
                m_queue.push_back(m_tracks[index]);
                return page("Added to queue.");
            }

            const auto position = m_index + 1;
            const auto inserted = m_queue.insert(m_queue.begin() + position, m_tracks[index]);
            if (play(position)) return page("Opening stream…");
            m_queue.erase(inserted);
            return page("Could not open this stream.");
        }
        if (!ready()) return page("Could not connect to YouTube Music.");
        try {
            if (download(index)) return page("Saved to Downloads.");
        } catch (const std::exception& ex) {
            nxlog::pushf("YouTube download failed: %s", ex.what());
            m_error = "Could not finish the audio download.";
        }
        auto message = std::exchange(m_error, {});
        return page(message.empty() ? "Could not save the download to the SD card." : std::move(message));
    }

    ++m_generation;
    m_tracks.clear();
    m_title = cmd == Browse::OnlineSearch ? "Results for " + query : "YouTube Music";
    if (!ready()) return page("Could not connect to YouTube Music.");
    auto addTrack = [&](const switube::YtTrack& track) {
        if (m_tracks.size() == 32 || !switube::validTrackId(track.id) || track.title.empty()) return;
        if (std::none_of(m_tracks.begin(), m_tracks.end(), [&](const auto& item) { return item.id == track.id; }))
            m_tracks.push_back(track);
    };
    bool ok = false;
    if (cmd == Browse::OnlineSearch) {
        std::vector<switube::YtTrack> results;
        std::string continuation;
        ok = m_client.searchTracks(query, switube::YtSearchFilter::Songs, results, continuation);
        for (const auto& result : results) addTrack(result);
    } else {
        std::vector<switube::YtShelf> shelves;
        ok = m_client.getHomeFeed(shelves);
        for (const auto& shelf : shelves)
            for (const auto& result : shelf.tracks) addTrack(result);
    }
    return page(!ok ? "YouTube Music could not load. Try again." :
        m_tracks.empty() ? "No songs found." : "");
}

bool NativeYoutube::play(unsigned index) {
    if (index >= m_queue.size() || !ready()) return false;
    switube::AudioStreamUrl stream;
    const auto& track = m_queue[index];
    if (!m_client.resolveAudioStream(track.id, stream) || m_cancel ||
        stream.url.rfind("https://", 0) != 0 ||
        stream.contentLength > switube::native::maxAudioBytes)
        return false;
    m_index = index;
    m_started = true;
    auto& player = YtAudioPlayer::getInstance();
    player.stop();
    m_completed->store(false);
    player.setCompletionCallback([completed = m_completed] { completed->store(true); });
    player.setVolume(m_volume / 100.f);
    player.play(stream.url, stream.approxDurationMs ? stream.approxDurationMs : track.durationMs,
                stream.contentLength);
    return true;
}

bool NativeYoutube::download(unsigned index) {
    const auto& track = m_tracks[index];
    m_error.clear();
    nxlog::pushf("Download: resolving %s", track.id.c_str());
    switube::AudioStreamUrl stream;
    if (!m_client.resolveAudioStream(track.id, stream) || m_cancel ||
        stream.url.rfind("https://", 0) != 0 || stream.contentLength > switube::native::maxAudioBytes) {
        m_error = "YouTube did not offer downloadable audio for this track.";
        return false;
    }

    const std::string directory = switube::kNativeDownloadsDirectory;
    const auto parent = directory.substr(0, directory.size() - 1);
    if (mkdir(parent.c_str(), 0755) != 0 && errno != EEXIST) {
        m_error = "Could not create the music downloads folder.";
        return false;
    }

    const auto file = directory + track.id + (stream.codec == "opus" ? ".opus" : ".m4a");
    if (!regularFile(file)) {
        PartialFile output(file);
        if (!output.file) {
            m_error = "Could not open the SD download file.";
            return false;
        }
        nxlog::pushf("Download: transferring %s (%llu bytes)", track.id.c_str(),
                     static_cast<unsigned long long>(stream.contentLength));
        m_error = switube::native::transferAudio(stream.url, stream.contentLength, m_cancel,
            [&](std::span<const std::uint8_t> bytes) {
                return std::fwrite(bytes.data(), 1, bytes.size(), output.file.get()) == bytes.size();
            });
        if (!m_error.empty() || m_cancel) return false;
        if (!output.commit(file)) {
            m_error = "Could not save the audio file to the SD card.";
            return false;
        }
    }

    const auto metadata = directory + track.id + ".json";
    if (!regularFile(metadata)) {
        const auto json = nlohmann::json{{"title", track.title.substr(0, 256)},
            {"artist", track.artist.substr(0, 256)}, {"imageUrl", coverOf(track)},
            {"durationMs", stream.approxDurationMs ? stream.approxDurationMs : track.durationMs}}
            .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        PartialFile output(metadata);
        if (!output.file || std::fwrite(json.data(), 1, json.size(), output.file.get()) != json.size()
            || !output.commit(metadata)) {
            m_error = "Audio saved, but its track details could not be written.";
            return false;
        }
    }

    nxlog::pushf("Download: saved %s", track.id.c_str());
    return true;
}

std::string NativeYoutube::takePlaybackError() {
    if (!m_error.empty()) return std::exchange(m_error, {});
    return m_started ? YtAudioPlayer::getInstance().takeError() : std::string{};
}

int NativeYoutube::nextIndex() const {
    const int next = m_index + 1;
    const int size = static_cast<int>(m_queue.size());
    if (next >= size) {
        if (size > 0 && m_repeat == 1) return 0;
        return -1;
    }
    return next;
}

bool NativeYoutube::playNext() {
    const int next = nextIndex();
    if (next < 0) return true;
    const int last = static_cast<int>(m_queue.size()) - 1;
    int selected = next;
    if (m_shuffle && next < last) {
        static thread_local std::mt19937 random(static_cast<unsigned>(armGetSystemTick()));
        selected = std::uniform_int_distribution<int>(next, last)(random);
        std::swap(m_queue[next], m_queue[selected]);
    }
    if (play(next)) return true;
    std::swap(m_queue[next], m_queue[selected]);
    return false;
}

void NativeYoutube::tick() {
    if (m_cancel || !m_started || !m_completed->exchange(false) || m_queue.empty()) return;
    const bool opened = m_repeat == 2 ? play(m_index) : playNext();
    if (!opened) m_error = "Could not open the next queued song.";
}

void NativeYoutube::stop() {
    if (m_started) {
        auto& player = YtAudioPlayer::getInstance();
        player.setCompletionCallback({});
        player.stop();
    }
    m_completed->store(false);
    m_started = false;
}

bool NativeYoutube::active() const {
    if (!m_started) return false;
    const auto& player = YtAudioPlayer::getInstance();
    return player.isPlaying() || player.isPaused();
}

void NativeYoutube::describeQueue(Switube::Now& now) const {
    now.queueIndex = m_index;
    now.queue.reserve(m_queue.size());
    for (const auto& track : m_queue)
        now.queue.push_back({track.title, track.artist});
}

Switube::Now NativeYoutube::now() const {
    if (!hasQueue()) return {};
    const auto& player = YtAudioPlayer::getInstance();
    Switube::Now result;
    result.connected = result.online = true;
    result.active = active();
    result.playing = result.active && player.isPlaying();
    result.volume = m_volume;
    result.repeat = m_repeat;
    result.shuffle = m_shuffle;
    describeQueue(result);
    if (result.active && m_index >= 0) {
        const auto& track = m_queue[m_index];
        result.track = track.title;
        result.artist = track.artist;
        result.cover = coverOf(track);
        result.progressMs = static_cast<int>(std::min<std::uint32_t>(player.getPositionMs(), INT32_MAX));
        result.durationMs = static_cast<int>(std::min<std::uint32_t>(player.getDurationMs(), INT32_MAX));
    }
    result.at = armGetSystemTick();
    return result;
}

bool NativeYoutube::command(Switube::Cmd cmd, int value) {
    auto& player = YtAudioPlayer::getInstance();
    switch (cmd) {
        case Switube::Cmd::PlayPause:
            if (player.isPaused()) player.resume();
            else if (player.isPlaying()) player.pause();
            else {
                const int next = nextIndex();
                return next >= 0 ? playNext() : play(0);
            }
            break;
        case Switube::Cmd::Next:
            return playNext();
        case Switube::Cmd::Previous:
            if (m_index >= 0) return play(std::max(0, m_index - 1));
            break;
        case Switube::Cmd::PlayQueued:
            return value >= 0 && play(value);
        case Switube::Cmd::Volume:
            m_volume = std::clamp(value, 0, 100);
            player.setVolume(m_volume / 100.f);
            break;
        case Switube::Cmd::Shuffle:
            m_shuffle = !m_shuffle;
            break;
        case Switube::Cmd::Repeat:
            m_repeat = (m_repeat + 1) % 3;
            break;
    }
    return true;
}

} // namespace cards
