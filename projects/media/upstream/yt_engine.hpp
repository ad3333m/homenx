#pragma once

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <memory>
#include <chrono>

#include "track.hpp"

namespace switube {

enum class PlaybackState {
  Stopped,
  Loading,
  Playing,
  Paused,
  Error
};

struct NowPlayingInfo {
  std::string id;
  std::string title;
  std::string artist;
  std::string album;
  std::string imageUrl;
  uint32_t durationMs = 0;
  uint32_t positionMs = 0;
  float volume = 1.0f;
  bool isFavorite = false;
};

using EngineObserver = std::function<void()>;

class YtEngine {
 public:
  static YtEngine& getInstance();

  bool init();
  void shutdown();

  // Control de reproducción
  void playTrack(const YtTrack& track);
  void playQueue(const std::vector<YtTrack>& tracks, size_t startIndex = 0);
  void playPause();
  void next();
  void previous();
  void seekTo(uint32_t positionMs);
  void setVolume(float volume);
  void onTrackDeleted(const std::string& trackId);

  // Favoritos y Biblioteca sincronizada
  void toggleFavoriteCurrent();
  bool isFavorite(const std::string& trackId);
  std::vector<YtTrack> getFavorites();
  std::vector<YtTrack> getUserPlaylists();
  void syncAccountLibrary();
  void clearAccountLibrary();
  bool isSyncingLibrary() const { return syncingLibrary_.load(); }
  bool hasMoreFavorites() const;
  bool hasMorePlaylists() const;
  void loadMoreFavorites();
  void loadMorePlaylists();
  bool isLoadingMoreFavorites() const { return loadingMoreFavorites_.load(); }
  bool isLoadingMorePlaylists() const { return loadingMorePlaylists_.load(); }

  // Estado
  PlaybackState getState() const { return state_; }
  NowPlayingInfo getNowPlaying() const;
  std::vector<YtTrack> getQueue() const;

  // Cliente de YouTube Music

  // Observadores para actualización de UI
  size_t addObserver(EngineObserver observer);
  void removeObserver(size_t id);

 private:
  YtEngine();
  ~YtEngine();

  void notifyObservers();
  void playbackThreadLoop();
  void loadFavorites();
  void saveFavorites();
  void loadUserPlaylists();
  void saveUserPlaylists();

  PlaybackState state_ = PlaybackState::Stopped;
  NowPlayingInfo nowPlaying_;
  std::vector<YtTrack> queue_;
  size_t currentQueueIndex_ = 0;
  std::vector<YtTrack> favorites_;
  std::vector<YtTrack> userPlaylists_;
  std::string likedContinuationToken_;
  std::string playlistsContinuationToken_;
  std::atomic<bool> syncingLibrary_{false};
  std::atomic<bool> loadingMoreFavorites_{false};
  std::atomic<bool> loadingMorePlaylists_{false};

  mutable std::recursive_mutex dataMutex_;
  std::vector<std::pair<size_t, EngineObserver>> observers_;
  size_t nextObserverId_ = 1;

  std::atomic<bool> isRunning_{false};
  std::atomic<uint64_t> playRequestGen_{0};
  std::atomic<int> consecutiveResolveFails_{0};
  std::chrono::steady_clock::time_point positionMeasuredAt_;
};

} // namespace switube
