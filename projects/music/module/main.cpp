#include "switube_service.hpp"
#include <switchu/time_budget.hpp>
#include "yt_music_client.hpp"
#include "yt_audio_player.hpp"
#include "pcm_output.hpp"
#include "download_catalog.hpp"
#include "playlists.hpp"
#include "NXConsoleLog.h"
#include <curl/curl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
u32 __nx_fs_num_sessions = 1;
u32 __nx_applet_type = AppletType_None;
// Back newlib with memory accounted for when Horizon loads this sysmodule.
void __libnx_initheap() {
    alignas(4096) static char heap[8 * 1024 * 1024];
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = heap;
    fake_heap_end = heap + sizeof(heap);
}

void __libnx_init_time();

void __appInit() {
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion version{};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&version)))
            hosversionSet(MAKEHOSVERSION(version.major, version.minor, version.micro));
        setsysExit();
    }
}

void __appExit() {
    smExit();
}
}

namespace {
using namespace switube;
using namespace switube::phase2;
ServiceCore core;
uint64_t milliseconds() { return armTicksToNs(armGetSystemTick()) / 1000000; }

// The sysmodule needs only stereo PCM. Using audout directly keeps SDL's
// window system and Mesa out of the resident process.
alignas(4096) int16_t samples[2][2048];
AudioOutBuffer buffers[2]{};
Thread outputThread{};
std::atomic<bool> outputRunning{false};

void pumpAudio(void*) {
    while (true) {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, UINT64_MAX))) break;
        if (!count || !released) continue;
        std::memset(released->buffer, 0, released->data_size);
        native::pcm::mix(static_cast<int16_t*>(released->buffer), released->data_size / 4);
        if (R_FAILED(audoutAppendAudioOutBuffer(released))) break;
    }
    native::pcm::setOutputRate(0);
    outputRunning = false;
}

bool openAudio() {
    if (outputRunning) return true;
    if (outputThread.handle) {
        threadWaitForExit(&outputThread);
        threadClose(&outputThread);
        outputThread = {};
        audoutStopAudioOut();
        audoutExit();
    }
    if (R_FAILED(audoutInitialize())) return false;
    Result rc = audoutStartAudioOut();
    for (unsigned i = 0; i < 2 && R_SUCCEEDED(rc); ++i) {
        std::memset(samples[i], 0, sizeof(samples[i]));
        buffers[i] = {nullptr, samples[i], sizeof(samples[i]), sizeof(samples[i]), 0};
        rc = audoutAppendAudioOutBuffer(&buffers[i]);
    }
    if (R_SUCCEEDED(rc)) rc = threadCreate(&outputThread, pumpAudio, nullptr, nullptr, 32*1024, 42, -2);
    if (R_SUCCEEDED(rc)) {
        native::pcm::setOutputRate(audoutGetSampleRate());
        outputRunning = true;
        rc = threadStart(&outputThread);
        if (R_SUCCEEDED(rc)) return true;
        threadClose(&outputThread);
    }
    outputThread = {};
    native::pcm::setOutputRate(0);
    outputRunning = false;
    audoutStopAudioOut();
    audoutExit();
    return false;
}

class Music {
    YtMusicClient client;
    YtAudioPlayer& player = YtAudioPlayer::getInstance();
    std::vector<YtTrack> results, queue;
    uint64_t generation = 0;
    unsigned index = 0;
    bool shuffle = false;
    u32 repeat = SwitubeRepeatMode_Off;
    std::atomic<bool> completed{false};

    bool sdMounted = false;
    bool networkReady = false;
    void ensureStorage() {
        if (!sdMounted) {
            Result rc = fsInitialize();
            if (R_FAILED(rc)) throw std::runtime_error("Filesystem unavailable");
            rc = fsdevMountSdmc();
            if (R_FAILED(rc)) {
                fsExit();
                throw std::runtime_error("Could not mount the SD card");
            }
            sdMounted = true;
        }
    }
    void ensureNetwork() {
        if (networkReady) return;
        ensureStorage();
        if (!switchu::timebudget::syncBootTime()) throw std::runtime_error("Clock service unavailable");
        if (R_FAILED(socketInitializeDefault())) throw std::runtime_error("Network service unavailable");
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            socketExit();
            throw std::runtime_error("Could not initialize HTTPS");
        }
        networkReady = true;
    }

    static constexpr const char* playlistDir = "sdmc:/config/SwitchU/media/playlists";
    bool listingPlaylists = false;
    std::string pageTitle;
    size_t offset = 0;
    void browse(const std::string& query, bool downloads = false) {
        generation = core.beginBrowse("YouTube Music", query, 0, milliseconds());
        listingPlaylists = false;
        pageTitle = downloads ? "Downloads" : "Background music";
        results.clear();
        offset = 0;
        if (downloads) {
            ensureStorage();
            results = loadDownloads();
        } else {
            ensureNetwork();
        }
        if (!downloads) {
            // Keep Discover to one bounded song search. The full home feed
            // builds several shelves and continuation responses in memory.
            std::string continuation;
            const std::string search = query.empty() ? "Top Hits" : query;
            if (!client.searchTracks(search, YtSearchFilter::Songs, results, continuation, "", 24))
                throw std::runtime_error(query.empty() ? "Could not load Discover" : "Search failed");
            std::erase_if(results, [](const auto& t) { return !validTrackId(t.id); });
            if (results.size() > 32) results.resize(32);
        }
        if (results.empty()) throw std::runtime_error("No songs found");
        publishPage();
    }

    void publishPage() {
        const size_t count = std::min<size_t>(30, results.size() - offset);
        std::vector<SwitubeIpcBrowseItem> items(count);
        for (size_t i = 0; i < count; ++i) {
            items[i].kind = listingPlaylists ? SwitubeBrowseItemKind_Playlist : SwitubeBrowseItemKind_Track;
            const auto& track = results[offset+i];
            std::snprintf(items[i].name, sizeof(items[i].name), "%s", track.title.c_str());
            std::snprintf(items[i].subtitle, sizeof(items[i].subtitle), "%s", track.artist.c_str());
        }
        auto link = [&](const char* label) {
            items.emplace_back();
            items.back().kind = SwitubeBrowseItemKind_Playlist;
            std::snprintf(items.back().name, sizeof(items.back().name), "%s", label);
        };
        if (offset + count < results.size()) link("Next page");
        if (offset) link("Previous page");
        core.publishBrowse(generation, pageTitle, "", items.data(), items.size(), 0, milliseconds());
    }

    void publishQueue(unsigned selected) {
        std::vector<SwitubeIpcBrowseItem> items(queue.size());
        for (size_t i = 0; i < queue.size(); ++i) {
            items[i].struct_size = sizeof(items[i]);
            items[i].abi_version = SWITUBE_IPC_BROWSE_ABI_VERSION;
            items[i].kind = SwitubeBrowseItemKind_Track;
            items[i].index = i;
            std::snprintf(items[i].name, sizeof(items[i].name), "%s", queue[i].title.c_str());
            std::snprintf(items[i].subtitle, sizeof(items[i].subtitle), "%s", queue[i].artist.c_str());
            std::snprintf(items[i].detail, sizeof(items[i].detail), "%s", queue[i].id.c_str());
        }
        core.replaceQueue(std::move(items), selected, milliseconds());
    }

    void play(const std::vector<YtTrack>& tracks, unsigned selected) {
        if (selected >= tracks.size()) return;
        const auto track = tracks[selected];
        AudioStreamUrl stream;
        if (track.id.starts_with("native:") || track.id.starts_with("switube:")) {
            const bool native = track.id.starts_with("native:");
            stream.url = downloadedTrackPath(track.id.substr(native ? 7 : 8),
                native ? kNativeDownloadsDirectory : kDownloadsDirectory);
            if (stream.url.empty()) throw std::runtime_error("Download no longer on SD card");
        } else {
            ensureNetwork();
            if (!client.resolveAudioStream(track.id, stream) || !stream.url.starts_with("https://"))
                throw std::runtime_error("Stream unavailable");
        }
        if (!openAudio()) throw std::runtime_error("Audio output unavailable");
        player.stop();
        completed = false;
        if (&tracks != &queue) {
            queue = tracks;
            publishQueue(selected);
        }
        index = selected;
        player.play(stream.url, stream.approxDurationMs ? stream.approxDurationMs : track.durationMs, stream.contentLength);
        core.publishTrack(track.title, track.artist, "", milliseconds());
        core.publishQueue(index, queue.size(), milliseconds());
    }

    void advance(bool automatic) {
        if (queue.empty()) return;
        if (automatic && repeat == SwitubeRepeatMode_One) play(queue, index);
        else if (shuffle && queue.size() > 1)
            play(queue, (index + 1 + armGetSystemTick() % (queue.size()-1)) % queue.size());
        else if (index + 1 < queue.size()) play(queue, index+1);
        else if (repeat == SwitubeRepeatMode_All) play(queue, 0);
    }

    void handle(const ControlMessage& msg) {
        switch (msg.command) {
        case SwitubeIpcCommand_QueueOnlineTrack:
        case SwitubeIpcCommand_PlayOnlineTrack: {
            YtTrack track;
            track.id = msg.track.id;
            if (!validTrackId(track.id)) throw std::runtime_error("Invalid song ID");
            track.title = msg.track.title;
            track.artist = msg.track.artist;
            track.durationMs = msg.track.duration_ms;
            if (queue.size() >= 256) throw std::runtime_error("Music queue is full");
            if (msg.command == SwitubeIpcCommand_QueueOnlineTrack) {
                queue.push_back(std::move(track));
                publishQueue(index);
            } else {
                auto tracks = queue;
                unsigned position = tracks.empty() ? 0 : index + 1;
                tracks.insert(tracks.begin() + position, std::move(track));
                play(tracks, position);
            }
            break;
        }
        case SwitubeIpcCommand_ListPlaylists:
        case SwitubeIpcCommand_SavePlaylist:
            generation = core.beginBrowse("Playlists", "", 0, milliseconds());
            listingPlaylists = true;
            pageTitle = "Playlists";
            offset = 0;
            results.clear();
            ensureStorage();
            if (msg.command == SwitubeIpcCommand_SavePlaylist)
                playlists::save(playlistDir, msg.query.data(), queue);
            results = playlists::list(playlistDir);
            if (results.empty()) throw std::runtime_error("No saved playlists yet");
            publishPage();
            break;
        case SwitubeIpcCommand_BrowseLibrary: browse("", true); break;
        case SwitubeIpcCommand_BrowseOpen: {
            if (msg.generation != generation) break;
            const size_t count = std::min<size_t>(30, results.size()-offset);
            const bool more = offset + count < results.size();
            if (listingPlaylists && msg.value < count) {
                pageTitle = results[offset + msg.value].title;
                generation = core.beginBrowse(pageTitle, "", 0, milliseconds());
                results = playlists::load(playlistDir, pageTitle);
                listingPlaylists = false;
                offset = 0;
                publishPage();
                break;
            }
            if (more && msg.value == count) offset += count;
            else if (offset && msg.value == count + more) offset -= 30;
            else break;
            generation = core.beginBrowse("Music", "", 0, milliseconds());
            publishPage();
            break;
        }
        case SwitubeIpcCommand_BrowseHome: browse(""); break;
        case SwitubeIpcCommand_BrowseSearch: browse(msg.query.data()); break;
        case SwitubeIpcCommand_BrowsePlay:
            if (!listingPlaylists && msg.generation == generation && msg.value < 30) play(results, offset + msg.value);
            break;
        case SwitubeIpcCommand_Pause:
            if (player.isPlaying()) player.pause();
            break;
        case SwitubeIpcCommand_PlayPause:
            if (player.isPaused()) player.resume();
            else if (player.isPlaying()) player.pause();
            else play(queue, index);
            break;
        case SwitubeIpcCommand_PlayQueueItem:
            if (msg.generation == core.snapshot().reserved0) play(queue, msg.value);
            break;
        case SwitubeIpcCommand_SetShuffle:
            shuffle = msg.value != 0;
            core.publishPlaybackOptions(shuffle, repeat, milliseconds());
            break;
        case SwitubeIpcCommand_SetRepeatMode:
            if (msg.value <= SwitubeRepeatMode_All) repeat = msg.value;
            core.publishPlaybackOptions(shuffle, repeat, milliseconds());
            break;
        case SwitubeIpcCommand_Previous: play(queue, index ? index - 1 : 0); break;
        case SwitubeIpcCommand_Next: advance(false); break;
        case SwitubeIpcCommand_SetVolume:
            player.setVolume(msg.value / 1000.f);
            core.publishVolume(msg.value, milliseconds());
            break;
        default: break;
        }
    }
public:
    void run() {
        client.init();
        player.setCompletionCallback([this] { completed = true; });
        player.setVolume(.8f);
        core.publishVolume(800, milliseconds());
        for (;;) {
            try {
                ControlMessage msg;
                if (core.tryPop(msg)) handle(msg);
                if (completed.exchange(false)) advance(true);
                auto error = player.takeError();
                if (!error.empty()) core.publishBrowseError(generation, error, milliseconds());
            } catch (const std::exception& e) {
                core.publishBrowseError(generation, e.what(), milliseconds());
            }
            core.publishPlaybackState(player.isPaused() ? SwitubePlaybackState_Paused :
                player.isPlaying() ? SwitubePlaybackState_Playing : SwitubePlaybackState_Stopped,
                player.getPositionMs(), player.getDurationMs(), milliseconds());
            svcSleepThread(100000000);
        }
    }
};
}

int main() {
    nxlog::setQueueEnabled(false);
    Result rc = switch_service::initialize(core, HOMENX_MUSIC_SERVICE);
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    core.configureCommands(true, SwitubeIpcCapability_Status | SwitubeIpcCapability_PlaybackControl |
        SwitubeIpcCapability_Queue | SwitubeIpcCapability_Search | SwitubeIpcCapability_Browsing, milliseconds());
    Thread thread{};
    rc = threadCreate(&thread, [](void*) { Music music; music.run(); }, nullptr,
            nullptr, 512 * 1024, 49, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&thread);
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    for (;;) {
        Waiter waiters[switch_service::MaxWaiters];
        const int count = switch_service::appendWaiters(waiters);
        int ready;
        if (R_SUCCEEDED(waitObjects(&ready, waiters, count, UINT64_MAX))) switch_service::poll();
    }
}
