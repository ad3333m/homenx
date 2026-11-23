#include "yt_engine.hpp"
#include "download_path.hpp"

#include <iostream>
#include <fstream>
#include <thread>
#include <algorithm>
#include <sys/stat.h>
#include <nlohmann/json.hpp>

#include "NXConsoleLog.h"
#include "thread_helper.hpp"
#include "yt_audio_player.hpp"

using json = nlohmann::json;

namespace switube {

namespace {
constexpr const char* CONFIG_DIR = "/config/SwitchU/media";
constexpr const char* FAVORITES_FILE = "/config/SwitchU/media/favorites.json";
constexpr const char* PLAYLISTS_FILE = "/config/SwitchU/media/playlists_cache.json";

std::string findLocalAudioTrack(const std::string& trackId) {
  if (trackId.rfind("native:", 0) == 0)
    return downloadedTrackPath(std::string_view(trackId).substr(7), kNativeDownloadsDirectory);
  if (trackId.rfind("switube:", 0) == 0)
    return downloadedTrackPath(std::string_view(trackId).substr(8));
  if (trackId.find_first_of("/\\:") != std::string::npos) return {};
  const char* exts[] = {".m4a", ".opus", ".mp3", ".webm"};
  for (const char* ext : exts) {
    std::string p = std::string(CONFIG_DIR) + "/downloads/" + trackId + ext;
    struct stat st{};
    if (stat(p.c_str(), &st) == 0 && st.st_size > 0) return p;
    std::string sp = std::string("sdmc:") + CONFIG_DIR + "/downloads/" + trackId + ext;
    if (stat(sp.c_str(), &st) == 0 && st.st_size > 0) return sp;
  }
  return "";
}

std::string findLocalTrackImage(const std::string& trackId) {
  const auto id = std::string_view(trackId);
  const bool legacy = id.starts_with("switube:");
  const auto key = legacy ? id.substr(8) : id.starts_with("native:") ? id.substr(7) : id;
  if (!validTrackId(key)) return {};
  const auto path = std::string(legacy ? kDownloadsDirectory : kNativeDownloadsDirectory) + std::string(key) + ".jpg";
  struct stat info{};
  return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 ? path : std::string{};
}
}

YtEngine& YtEngine::getInstance() {
  static YtEngine instance;
  return instance;
}

YtEngine::YtEngine() = default;

YtEngine::~YtEngine() {
  shutdown();
}

bool YtEngine::init() {
  mkdir("/config", 0755);
  mkdir(CONFIG_DIR, 0755);

  YtAudioPlayer::getInstance().init();
  YtAudioPlayer::getInstance().setCompletionCallback([this]() {
    next();
  });

  YtAudioPlayer::getInstance().setPlaybackStartCallback([this]() {
    {
      std::lock_guard<std::recursive_mutex> lock(dataMutex_);
      state_ = PlaybackState::Playing;
      positionMeasuredAt_ = std::chrono::steady_clock::now();
    }
    notifyObservers();
  });

  loadFavorites();
  loadUserPlaylists();
  isRunning_ = true;
  return true;
}

void YtEngine::shutdown() {
  isRunning_ = false;
  YtAudioPlayer::getInstance().shutdown();
  saveFavorites();
  saveUserPlaylists();
}

size_t YtEngine::addObserver(EngineObserver observer) {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  size_t id = nextObserverId_++;
  observers_.push_back({id, std::move(observer)});
  return id;
}

void YtEngine::removeObserver(size_t id) {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  observers_.erase(
      std::remove_if(observers_.begin(), observers_.end(),
                     [id](const auto& pair) { return pair.first == id; }),
      observers_.end());
}

void YtEngine::notifyObservers() {
  std::vector<EngineObserver> callbacks;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    for (const auto& pair : observers_) {
      callbacks.push_back(pair.second);
    }
  }
  for (const auto& cb : callbacks) {
    if (cb) cb();
  }
}

NowPlayingInfo YtEngine::getNowPlaying() const {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  NowPlayingInfo info = nowPlaying_;
  if (state_ == PlaybackState::Loading || state_ == PlaybackState::Stopped || state_ == PlaybackState::Error) {
    info.positionMs = 0;
    return info;
  }
  info.positionMs = YtAudioPlayer::getInstance().getPositionMs();
  return info;
}

std::vector<YtTrack> YtEngine::getQueue() const {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  return queue_;
}

void YtEngine::playTrack(const YtTrack& track) {
  playQueue({track}, 0);
}

void YtEngine::playQueue(const std::vector<YtTrack>& tracks, size_t startIndex) {
  if (tracks.empty() || startIndex >= tracks.size()) return;

  ++playRequestGen_;

  // Detener inmediatamente la pista anterior para que no continúe sonando ni reporte posición
  YtAudioPlayer::getInstance().stop();

  std::string trackId;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    queue_ = tracks;
    currentQueueIndex_ = startIndex;
    const auto& current = queue_[currentQueueIndex_];

    trackId = current.id;
    std::string localImg = findLocalTrackImage(current.id);
    nowPlaying_.id = current.id;
    nowPlaying_.title = current.title;
    nowPlaying_.artist = current.artist;
    nowPlaying_.album = current.album;
    nowPlaying_.imageUrl = !localImg.empty() ? localImg : current.imageUrl;
    nowPlaying_.durationMs = current.durationMs;
    nowPlaying_.positionMs = 0;
    nowPlaying_.isFavorite = isFavorite(current.id);

    state_ = PlaybackState::Loading;
    positionMeasuredAt_ = std::chrono::steady_clock::now();
  }
  notifyObservers();

  // Si la pista ya está descargada en la SD, reproducirla de inmediato sin conexión
  std::string localPath = findLocalAudioTrack(trackId);
  if (!localPath.empty()) {
    nxlog::pushf("playQueue: playing local downloaded track '%s' (%s)", trackId.c_str(), localPath.c_str());
    uint32_t dur = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(dataMutex_);
      dur = nowPlaying_.durationMs;
    }
    YtAudioPlayer::getInstance().play(localPath, dur, 0);
    return;
  }

  nxlog::pushf("playQueue: local file not found for '%s' in background service", trackId.c_str());
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    state_ = PlaybackState::Error;
  }
  notifyObservers();
  return;
}

void YtEngine::playPause() {
  bool playing = false;
  bool changed = false;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    if (state_ == PlaybackState::Playing) {
      state_ = PlaybackState::Paused;
      playing = false;
      changed = true;
    } else if (state_ == PlaybackState::Paused) {
      state_ = PlaybackState::Playing;
      positionMeasuredAt_ = std::chrono::steady_clock::now();
      playing = true;
      changed = true;
    }
  }
  if (!changed) return;
  if (playing) {
    YtAudioPlayer::getInstance().resume();
  } else {
    YtAudioPlayer::getInstance().pause();
  }
  notifyObservers();
}

void YtEngine::next() {
  std::vector<YtTrack> q;
  size_t nextIdx = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    if (queue_.empty()) return;
    nextIdx = (currentQueueIndex_ + 1) % queue_.size();
    q = queue_;
  }
  playQueue(q, nextIdx);
}

void YtEngine::previous() {
  std::vector<YtTrack> q;
  size_t prevIdx = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    if (queue_.empty()) return;
    prevIdx = (currentQueueIndex_ == 0) ? queue_.size() - 1 : currentQueueIndex_ - 1;
    q = queue_;
  }
  playQueue(q, prevIdx);
}

void YtEngine::seekTo(uint32_t positionMs) {
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    nowPlaying_.positionMs = positionMs;
    positionMeasuredAt_ = std::chrono::steady_clock::now();
  }
  YtAudioPlayer::getInstance().seekTo(positionMs);
  notifyObservers();
}

void YtEngine::setVolume(float volume) {
  float clamped = 0.8f;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    nowPlaying_.volume = std::clamp(volume, 0.0f, 1.0f);
    clamped = nowPlaying_.volume;
  }
  YtAudioPlayer::getInstance().setVolume(clamped);
  notifyObservers();
}

void YtEngine::onTrackDeleted(const std::string& trackId) {
  if (trackId.empty()) return;
  bool wasPlayingThis = false;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    wasPlayingThis = (nowPlaying_.id == trackId);
  }

  if (wasPlayingThis) {
    ++playRequestGen_;
    YtAudioPlayer::getInstance().stop();
  }

  bool notify = false;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    for (size_t i = 0; i < queue_.size();) {
      if (queue_[i].id == trackId) {
        queue_.erase(queue_.begin() + i);
        if (currentQueueIndex_ > i && currentQueueIndex_ > 0) {
          currentQueueIndex_--;
        } else if (currentQueueIndex_ >= queue_.size() && !queue_.empty()) {
          currentQueueIndex_ = 0;
        }
      } else {
        ++i;
      }
    }
    if (nowPlaying_.id == trackId) {
      nowPlaying_ = NowPlayingInfo{};
      state_ = PlaybackState::Stopped;
      notify = true;
    }
  }
  if (notify) {
    notifyObservers();
  }
}

bool YtEngine::isFavorite(const std::string& trackId) {
  for (const auto& fav : favorites_) {
    if (fav.id == trackId) return true;
  }
  return false;
}

void YtEngine::toggleFavoriteCurrent() {
  std::string targetId;
  bool nowLiked = false;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    if (nowPlaying_.id.empty()) return;
    targetId = nowPlaying_.id;

    auto it = std::find_if(favorites_.begin(), favorites_.end(),
                           [this](const YtTrack& t) { return t.id == nowPlaying_.id; });

    if (it != favorites_.end()) {
      favorites_.erase(it);
      nowPlaying_.isFavorite = false;
      nowLiked = false;
    } else {
      YtTrack t;
      t.id = nowPlaying_.id;
      t.title = nowPlaying_.title;
      t.artist = nowPlaying_.artist;
      t.album = nowPlaying_.album;
      t.imageUrl = nowPlaying_.imageUrl;
      t.durationMs = nowPlaying_.durationMs;
      favorites_.insert(favorites_.begin(), t);
      nowPlaying_.isFavorite = true;
      nowLiked = true;
    }
    saveFavorites();
  }
  notifyObservers();

}

std::vector<YtTrack> YtEngine::getFavorites() {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  return favorites_;
}

std::vector<YtTrack> YtEngine::getUserPlaylists() {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  return userPlaylists_;
}

void YtEngine::clearAccountLibrary() {
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    userPlaylists_.clear();
    likedContinuationToken_.clear();
    playlistsContinuationToken_.clear();
    saveUserPlaylists();
  }
  notifyObservers();
}

bool YtEngine::hasMoreFavorites() const {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  return !likedContinuationToken_.empty();
}

bool YtEngine::hasMorePlaylists() const {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  return !playlistsContinuationToken_.empty();
}

void YtEngine::loadMoreFavorites() {
}

void YtEngine::loadMorePlaylists() {
}

void YtEngine::syncAccountLibrary() {
}

void YtEngine::loadFavorites() {
  std::ifstream f(FAVORITES_FILE);
  if (!f.is_open()) return;

  try {
    json j;
    f >> j;
    favorites_.clear();
    for (const auto& item : j) {
      YtTrack t;
      t.id = item.value("id", "");
      t.title = item.value("title", "");
      t.artist = item.value("artist", "");
      t.album = item.value("album", "");
      t.imageUrl = item.value("imageUrl", "");
      t.durationMs = item.value("durationMs", 0u);
      if (!t.id.empty()) {
        favorites_.push_back(t);
      }
    }
  } catch (...) {}
}

void YtEngine::saveFavorites() {
  std::ofstream f(FAVORITES_FILE);
  if (!f.is_open()) return;

  try {
    json j = json::array();
    for (const auto& t : favorites_) {
      j.push_back({
          {"id", t.id},
          {"title", t.title},
          {"artist", t.artist},
          {"album", t.album},
          {"imageUrl", t.imageUrl},
          {"durationMs", t.durationMs}
      });
    }
    f << j.dump(2);
  } catch (...) {}
}

void YtEngine::loadUserPlaylists() {
  std::ifstream f(PLAYLISTS_FILE);
  if (!f.is_open()) return;

  try {
    json j;
    f >> j;
    userPlaylists_.clear();
    for (const auto& item : j) {
      YtTrack t;
      t.id = item.value("id", "");
      t.title = item.value("title", "");
      t.artist = item.value("artist", "");
      t.imageUrl = item.value("imageUrl", "");
      t.isPlaylist = item.value("isPlaylist", true);
      t.isArtist = item.value("isArtist", false);
      if (!t.id.empty()) {
        userPlaylists_.push_back(t);
      }
    }
  } catch (...) {}
}

void YtEngine::saveUserPlaylists() {
  std::ofstream f(PLAYLISTS_FILE);
  if (!f.is_open()) return;

  try {
    json j = json::array();
    for (const auto& t : userPlaylists_) {
      j.push_back({
          {"id", t.id},
          {"title", t.title},
          {"artist", t.artist},
          {"imageUrl", t.imageUrl},
          {"isPlaylist", t.isPlaylist},
          {"isArtist", t.isArtist}
      });
    }
    f << j.dump(2);
  } catch (...) {}
}

} // namespace switube
