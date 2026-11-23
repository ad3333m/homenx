#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstdint>
#include <memory>

struct nestegg;
class NXAudioSink;

namespace switube {

class YtAudioPlayer {
 public:
  static YtAudioPlayer& getInstance();

  bool init();
  void shutdown();

  // Inicia la reproducción de una URL directa de audio (Opus/WebM/AAC)
  void play(const std::string& streamUrl, uint32_t durationMs = 0, uint64_t contentLength = 0);
  void pause();
  void resume();
  void stop();
  void seekTo(uint32_t positionMs);
  void setVolume(float volume);

  bool isPlaying() const { return isPlaying_.load(); }
  bool isPaused() const { return isPaused_.load(); }
  uint32_t getPositionMs() const { return currentPositionMs_.load(); }
  uint32_t getDurationMs() const { return durationMs_.load(); }

  void setCompletionCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    onComplete_ = std::move(cb);
  }

  void setPlaybackStartCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    onStart_ = std::move(cb);
  }

 private:
  YtAudioPlayer();
  ~YtAudioPlayer();

  void playWebmOpus(nestegg* file, unsigned track, const std::shared_ptr<std::atomic<bool>>& stop);
  void audioWorker(std::string url, uint32_t durMs, uint64_t contentLength, uint64_t generation,
                   std::shared_ptr<std::atomic<bool>> stopToken);

  std::unique_ptr<NXAudioSink> sink_;
  std::atomic<bool> isPlaying_{false};
  std::atomic<bool> isPaused_{false};
  std::atomic<uint32_t> currentPositionMs_{0};
  std::atomic<uint32_t> durationMs_{0};
  std::atomic<uint64_t> currentGeneration_{0};
  std::atomic<float> volume_{0.8f};
  std::atomic<int64_t> seekTargetMs_{-1};

  std::shared_ptr<std::atomic<bool>> currentToken_;
  std::mutex controlMutex_;
  std::mutex sinkMutex_;
  std::mutex workerMutex_;
  std::function<void()> onComplete_;
  std::function<void()> onStart_;
};

} // namespace switube
