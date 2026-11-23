#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstdint>
#include <memory>

#include <switch.h>

namespace switube {

class YtAudioPlayer {
 public:
  static YtAudioPlayer& getInstance();

  // Inicia la reproducción de una URL directa de audio (Opus/WebM/AAC)
  void play(const std::string& streamUrl, uint32_t durationMs = 0, uint64_t contentLength = 0);
  void pause();
  void resume();
  void stop();
  void setVolume(float volume);
  std::string takeError();

  bool isPlaying() const { return isPlaying_.load(); }
  bool isPaused() const { return isPaused_.load(); }
  uint32_t getPositionMs() const;
  uint32_t getDurationMs() const { return durationMs_.load(); }

  void setCompletionCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    onComplete_ = std::move(cb);
  }

 private:
  YtAudioPlayer() = default;
  ~YtAudioPlayer();

  void audioWorker(std::string url, uint32_t durMs, uint64_t contentLength,
                   std::shared_ptr<std::atomic<bool>> stopToken);

  Thread decoder_{};
  std::atomic<bool> isPlaying_{false};
  std::atomic<bool> isPaused_{false};
  std::atomic<uint32_t> durationMs_{0};

  std::shared_ptr<std::atomic<bool>> currentToken_;
  std::mutex controlMutex_;
  std::function<void()> onComplete_;
  std::string lastError_;
};

} // namespace switube
