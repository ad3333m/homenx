#include "background_backend.hpp"

#include "NXConsoleLog.h"
#include "download_path.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <dirent.h>
#include <string_view>
#include <fstream>
#include <sys/stat.h>
#include <nlohmann/json.hpp>

namespace switube::phase2 {
namespace {

constexpr auto PollInterval = std::chrono::milliseconds(50);
constexpr auto SnapshotInterval = std::chrono::milliseconds(250);
constexpr const char* ArtworkUrlFile = "/config/SwitchU/media/current-artwork-url";
constexpr const char* ArtworkUrlTemporary = "/config/SwitchU/media/current-artwork-url.tmp";
constexpr size_t MaxBrowseItems = 32;

std::vector<YtTrack> loadSwitubeDownloads() {
  const auto manifest = std::string(kDownloadsDirectory) + "downloads.json";
  struct stat info{};
  std::vector<YtTrack> list;
  if (stat(manifest.c_str(), &info) != 0 || info.st_size <= 0 || info.st_size > 64 * 1024)
    return list;
  try {
    std::ifstream file(manifest);
    const auto downloads = nlohmann::json::parse(file, nullptr, false);
    if (!downloads.is_array()) return list;
    for (const auto& item : downloads) {
      if (!item.is_object()) continue;
      const auto id = item.value("id", std::string{});
      if (downloadedTrackPath(id).empty()) continue;
      YtTrack track;
      track.id = "switube:" + id;
      track.title = item.value("title", id);
      track.artist = item.value("artist", std::string{});
      track.durationMs = item.value("durationMs", 0u);
      track.imageUrl = item.value("imageUrl", std::string{});
      if (track.imageUrl.empty()) track.imageUrl = "https://i.ytimg.com/vi/" + id + "/default.jpg";
      list.push_back(std::move(track));
      if (list.size() == 256) break;
    }
  } catch (const std::exception&) {
    nxlog::push("Could not read Switube download metadata");
  }
  return list;
}

void appendNativeDownloads(std::vector<YtTrack>& list) {
  DIR* directory = opendir(kNativeDownloadsDirectory);
  if (!directory) return;
  while (list.size() < 256) {
    const auto* entry = readdir(directory);
    if (!entry) break;
    const std::string_view name(entry->d_name);
    if (!name.ends_with(".json")) continue;
    const auto id = name.substr(0, name.size() - 5);
    if (downloadedTrackPath(id, kNativeDownloadsDirectory).empty()) continue;
    const auto metadata = kNativeDownloadsDirectory + std::string(name);
    struct stat info{};
    if (stat(metadata.c_str(), &info) != 0 || info.st_size > 4096) continue;
    try {
      std::ifstream file(metadata);
      const auto item = nlohmann::json::parse(file, nullptr, false);
      if (!item.is_object()) continue;
      YtTrack track;
      track.id = "native:" + std::string(id);
      track.title = item.value("title", std::string(id));
      track.artist = item.value("artist", std::string{});
      track.durationMs = item.value("durationMs", 0u);
      track.imageUrl = item.value("imageUrl", std::string{});
      if (track.imageUrl.empty()) track.imageUrl = "https://i.ytimg.com/vi/" + std::string(id) + "/default.jpg";
      list.push_back(std::move(track));
    } catch (const std::exception&) {
      nxlog::push("Could not read a download's metadata");
    }
  }
  closedir(directory);
}

std::vector<YtTrack> loadDownloads() {
  std::vector<YtTrack> list;
  appendNativeDownloads(list);
  for (auto& legacy : loadSwitubeDownloads()) {
    const auto id = std::string_view(legacy.id).substr(8);
    if (std::none_of(list.begin(), list.end(), [&](const YtTrack& track) {
          return std::string_view(track.id).ends_with(id);
        }))
      list.push_back(std::move(legacy));
  }
  std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
    if (a.title != b.title) return a.title < b.title;
    return a.id < b.id;
  });
  return list;
}

alignas(0x1000) static uint8_t s_workerStack[0x20000];  // 128 KiB static stack in BSS

void copyBrowseText(char* destination, size_t capacity, const std::string& source) {
  if (capacity == 0) return;
  const size_t length = std::min(capacity - 1, source.size());
  if (length != 0) std::memcpy(destination, source.data(), length);
  destination[length] = '\0';
}

SwitubePlaybackState translatePlaybackState(PlaybackState state) {
  switch (state) {
    case PlaybackState::Playing:
      return SwitubePlaybackState_Playing;
    case PlaybackState::Paused:
      return SwitubePlaybackState_Paused;
    case PlaybackState::Loading:
      return SwitubePlaybackState_Buffering;
    case PlaybackState::Stopped:
    case PlaybackState::Error:
    default:
      return SwitubePlaybackState_Stopped;
  }
}

}  // namespace

BackgroundBackend::BackgroundBackend(ServiceCore& core)
    : core(core), engine(YtEngine::getInstance()) {}

BackgroundBackend::~BackgroundBackend() { stop(); }

uint64_t BackgroundBackend::monotonicMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

Result BackgroundBackend::start() {
  if (running.load()) return 0;

  if (!engine.init()) {
    nxlog::push("BackgroundBackend: YtEngine init failed");
    return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
  }

  running.store(true);
  const uint64_t capabilities =
      SwitubeIpcCapability_Status | SwitubeIpcCapability_PlaybackControl |
      SwitubeIpcCapability_Seek | SwitubeIpcCapability_Queue |
      SwitubeIpcCapability_Browsing | SwitubeIpcCapability_Search |
      SwitubeIpcCapability_AudioSettings;

  core.configureCommands(true, capabilities, monotonicMs());
  core.publishServiceState(SwitubeServiceState_Ready, monotonicMs());
  core.publishConnection(true, monotonicMs());
  core.publishSeekable(true, monotonicMs());

  Result rc = threadCreate(&workerThread, &BackgroundBackend::threadEntry, this,
                           s_workerStack, sizeof(s_workerStack), 49, 3);
  if (R_FAILED(rc)) {
    nxlog::pushf("BackgroundBackend: threadCreate failed: 0x%08X", rc);
    running.store(false);
    engine.shutdown();
    return rc;
  }
  rc = threadStart(&workerThread);
  if (R_FAILED(rc)) {
    nxlog::pushf("BackgroundBackend: threadStart failed: 0x%08X", rc);
    threadClose(&workerThread);
    running.store(false);
    engine.shutdown();
    return rc;
  }
  threadStarted = true;
  nxlog::push("BackgroundBackend: worker thread started (128 KB static stack)");
  return 0;
}

void BackgroundBackend::stop() {
  if (!running.exchange(false)) return;

  core.configureCommands(false, SwitubeIpcCapability_Status, monotonicMs());
  core.publishServiceState(SwitubeServiceState_Stopping, monotonicMs());

  if (threadStarted) {
    threadWaitForExit(&workerThread);
    threadClose(&workerThread);
    threadStarted = false;
  }

  engine.shutdown();
}

void BackgroundBackend::threadEntry(void* arg) {
  static_cast<BackgroundBackend*>(arg)->run();
}

void BackgroundBackend::publishArtworkUrl(const std::string& url) {
  if (url == lastPublishedArtworkUrl) return;
  lastPublishedArtworkUrl = url;

  if (url.empty()) {
    std::remove(ArtworkUrlFile);
    return;
  }

  std::ofstream out(ArtworkUrlTemporary, std::ios::trunc);
  if (out.is_open()) {
    out << url << "\n";
    out.close();
    std::rename(ArtworkUrlTemporary, ArtworkUrlFile);
  }
}

void BackgroundBackend::publishSnapshot() {
  const auto now = monotonicMs();
  const auto playing = engine.getNowPlaying();
  const auto state = engine.getState();
  const auto switubeState = translatePlaybackState(state);

  if (playing.title.empty()) {
    core.clearTrack(now);
  } else {
    core.publishTrack(playing.title, playing.artist, playing.album, now);
  }

  core.publishPlaybackState(switubeState, playing.positionMs, playing.durationMs, now);
  core.publishVolume(static_cast<uint32_t>(playing.volume * 1000.0f), now);

  const auto queue = engine.getQueue();
  size_t currentIdx = 0;
  for (size_t i = 0; i < queue.size(); ++i) {
    if (queue[i].id == playing.id) {
      currentIdx = i;
      break;
    }
  }
  core.publishQueue(static_cast<uint32_t>(currentIdx), static_cast<uint32_t>(queue.size()), now);

  publishArtworkUrl(playing.imageUrl);
}

void BackgroundBackend::run() {
  auto nextSnapshot = std::chrono::steady_clock::now();

  while (running.load()) {
    ControlMessage message{};
    while (core.tryPop(message)) {
      process(message);
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= nextSnapshot) {
      publishSnapshot();
      nextSnapshot = now + SnapshotInterval;
    }

    std::this_thread::sleep_for(PollInterval);
  }
}

void BackgroundBackend::process(const ControlMessage& message) {
  switch (message.command) {
    case SwitubeIpcCommand_PlayPause:
      engine.playPause();
      break;
    case SwitubeIpcCommand_Play:
      if (engine.getState() != PlaybackState::Playing) engine.playPause();
      break;
    case SwitubeIpcCommand_Pause:
      if (engine.getState() == PlaybackState::Playing) engine.playPause();
      break;
    case SwitubeIpcCommand_Next:
      engine.next();
      break;
    case SwitubeIpcCommand_Previous:
      engine.previous();
      break;
    case SwitubeIpcCommand_SetVolume:
      engine.setVolume(message.value / 1000.0f);
      break;
    case SwitubeIpcCommand_Seek:
      engine.seekTo(message.value);
      break;
    default:
      processBrowse(message);
      break;
  }
  publishSnapshot();
}

uint64_t BackgroundBackend::beginRoot(std::string_view title, std::string_view subtitle) {
  std::scoped_lock lock(browseMutex);
  currentBrowseGeneration = core.beginBrowse(title, subtitle, 0, monotonicMs());
  return currentBrowseGeneration;
}

void BackgroundBackend::completeBrowse(uint64_t generation, BrowsePage page) {
  std::scoped_lock lock(browseMutex);
  if (generation != currentBrowseGeneration) return;

  std::vector<SwitubeIpcBrowseItem> ipcItems;
  ipcItems.reserve(std::min(page.entries.size(), MaxBrowseItems));

  for (size_t i = 0; i < page.entries.size() && i < MaxBrowseItems; ++i) {
    const auto& entry = page.entries[i];
    SwitubeIpcBrowseItem item{};
    item.struct_size = sizeof(item);
    item.abi_version = SWITUBE_IPC_BROWSE_ABI_VERSION;
    item.generation = generation;
    item.index = static_cast<uint32_t>(i);
    copyBrowseText(item.name, sizeof(item.name), entry.name);
    copyBrowseText(item.subtitle, sizeof(item.subtitle), entry.subtitle);

    if (entry.type == BrowseEntryType::Track) {
      item.kind = SwitubeBrowseItemKind_Track;
      item.duration_ms = entry.track.durationMs;
    } else {
      item.kind = entry.type == BrowseEntryType::Page ? SwitubeBrowseItemKind_Playlist
                                                      : SwitubeBrowseItemKind_Section;
    }
    ipcItems.push_back(item);
  }

  currentPage = std::move(page);
  core.publishBrowse(generation, currentPage.title, currentPage.subtitle,
                     ipcItems.data(), ipcItems.size(), currentPage.depth,
                     monotonicMs());
}

void BackgroundBackend::failBrowse(uint64_t generation, const std::string& message) {
  std::scoped_lock lock(browseMutex);
  if (generation != currentBrowseGeneration) return;
  core.publishBrowseError(generation, message, monotonicMs());
}

void BackgroundBackend::requestHome() {
  requestLibrary();
}

void BackgroundBackend::requestLibrary(std::size_t offset) {
  const auto gen = beginRoot("Downloads", "");
  try {
    const auto downloads = loadDownloads();
    constexpr std::size_t TracksPerPage = MaxBrowseItems - 2;
    if (offset >= downloads.size())
      offset = downloads.empty() ? 0 : (downloads.size() - 1) / TracksPerPage * TracksPerPage;

    BrowsePage page{};
    page.title = "Downloads";
    page.subtitle = std::to_string(downloads.size()) + " tracks";

    auto addPage = [&](const char* label, std::size_t target) {
      BrowseEntry entry{};
      entry.type = BrowseEntryType::Page;
      entry.name = label;
      entry.pageOffset = target;
      page.entries.push_back(std::move(entry));
    };

    if (offset) addPage("Previous page", offset - TracksPerPage);
    const auto end = std::min(offset + TracksPerPage, downloads.size());
    for (std::size_t i = offset; i < end; ++i) {
      const auto& track = downloads[i];
      BrowseEntry entry{};
      entry.name = track.title;
      entry.subtitle = track.artist;
      entry.track = track;
      page.entries.push_back(std::move(entry));
    }
    if (end < downloads.size()) addPage("Next page", end);
    if (page.entries.empty()) {
      BrowseEntry empty{};
      empty.type = BrowseEntryType::Section;
      empty.name = "No downloads yet";
      empty.subtitle = "Download a song from YouTube Music.";
      page.entries.push_back(std::move(empty));
    }
    completeBrowse(gen, std::move(page));
  } catch (const std::exception& e) {
    failBrowse(gen, e.what());
  }
}

void BackgroundBackend::requestSearch(const std::string& query) {
  if (query.empty()) return;
  const auto gen = beginRoot("Search downloads", query);
  try {
    nxlog::pushf("requestSearch: searching '%s' in downloads...", query.c_str());
    auto allTracks = loadDownloads();
    std::string qLower = query;
    std::transform(qLower.begin(), qLower.end(), qLower.begin(), [](unsigned char ch) { return std::tolower(ch); });

    BrowsePage page{};
    page.title = "Search results";
    page.subtitle = query;
    page.depth = 0;

    for (const auto& trk : allTracks) {
      std::string tLower = trk.title;
      std::string aLower = trk.artist;
      std::transform(tLower.begin(), tLower.end(), tLower.begin(), [](unsigned char ch) { return std::tolower(ch); });
      std::transform(aLower.begin(), aLower.end(), aLower.begin(), [](unsigned char ch) { return std::tolower(ch); });

      if (tLower.find(qLower) != std::string::npos || aLower.find(qLower) != std::string::npos) {
        BrowseEntry entry{};
        entry.type = BrowseEntryType::Track;
        entry.name = trk.title;
        entry.subtitle = trk.artist;
        entry.track = trk;
        page.entries.push_back(entry);
      }
    }

    if (page.entries.empty()) {
      failBrowse(gen, "No results for: " + query);
      return;
    }

    nxlog::pushf("requestSearch: found %zu matching tracks", page.entries.size());
    completeBrowse(gen, std::move(page));
  } catch (const std::exception& e) {
    failBrowse(gen, e.what());
  } catch (...) {
    failBrowse(gen, "Error en la búsqueda");
  }
}

void BackgroundBackend::browseCurrentQueue() {
  const auto gen = beginRoot("Cola de reproducción", "En reproducción");
  auto q = engine.getQueue();
  BrowsePage page{};
  page.title = "Cola actual";
  page.subtitle = std::to_string(q.size()) + " tracks";
  page.depth = 0;

  for (const auto& trk : q) {
    BrowseEntry entry{};
    entry.type = BrowseEntryType::Track;
    entry.name = trk.title;
    entry.subtitle = trk.artist;
    entry.track = trk;
    page.entries.push_back(entry);
  }

  completeBrowse(gen, std::move(page));
}

void BackgroundBackend::openBrowseItem(uint64_t generation, uint32_t index) {
  std::scoped_lock lock(browseMutex);
  if (generation != currentBrowseGeneration || index >= currentPage.entries.size()) return;

  const auto& entry = currentPage.entries[index];
  if (entry.type == BrowseEntryType::Page)
    requestLibrary(entry.pageOffset);
  else if (entry.type == BrowseEntryType::Track)
    playBrowseItem(generation, index);
}

void BackgroundBackend::playBrowseItem(uint64_t generation, uint32_t index) {
  std::scoped_lock lock(browseMutex);
  if (generation != currentBrowseGeneration || index >= currentPage.entries.size()) return;

  const auto& selected = currentPage.entries[index];
  if (selected.type == BrowseEntryType::Track) {
    std::vector<YtTrack> tracks;
    size_t playIndex = 0;
    for (size_t i = 0; i < currentPage.entries.size(); ++i) {
      if (currentPage.entries[i].type == BrowseEntryType::Track) {
        if (i == index) playIndex = tracks.size();
        tracks.push_back(currentPage.entries[i].track);
      }
    }
    if (!tracks.empty()) {
      engine.playQueue(tracks, playIndex);
    }
  }
}

void BackgroundBackend::processBrowse(const ControlMessage& message) {
  switch (message.command) {
    case SwitubeIpcCommand_BrowseHome:
      requestHome();
      break;
    case SwitubeIpcCommand_BrowseLibrary:
      requestLibrary();
      break;
    case SwitubeIpcCommand_BrowseSearch:
      requestSearch(message.query.data());
      break;
    case SwitubeIpcCommand_BrowseOpen:
      openBrowseItem(message.generation, message.value);
      break;
    case SwitubeIpcCommand_BrowsePlay:
      playBrowseItem(message.generation, message.value);
      break;
    case SwitubeIpcCommand_BrowseCurrentPlaylist:
    case SwitubeIpcCommand_BrowseQueue:
      browseCurrentQueue();
      break;
    default:
      break;
  }
}

}  // namespace switube::phase2
