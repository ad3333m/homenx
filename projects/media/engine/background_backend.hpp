#pragma once

#include "yt_engine.hpp"
#include "service_core.hpp"

#include <switch.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace switube::phase2 {

class BackgroundBackend {
 public:
  explicit BackgroundBackend(ServiceCore& core);
  ~BackgroundBackend();

  Result start();
  void stop();

 private:
  static uint64_t monotonicMs();
  static void threadEntry(void* arg);
  void run();
  void process(const ControlMessage& message);
  void publishSnapshot();
  void publishArtworkUrl(const std::string& url);
  void processBrowse(const ControlMessage& message);

  enum class BrowseEntryType {
    Section,
    Page,
    Track,
  };

  struct BrowseEntry {
    BrowseEntryType type = BrowseEntryType::Track;
    std::string name;
    std::string subtitle;
    YtTrack track;
    std::size_t pageOffset = 0;
  };

  struct BrowsePage {
    std::string title;
    std::string subtitle;
    uint32_t depth = 0;
    std::vector<BrowseEntry> entries;
  };

  uint64_t beginRoot(std::string_view title, std::string_view subtitle);
  void completeBrowse(uint64_t generation, BrowsePage page);
  void failBrowse(uint64_t generation, const std::string& message);
  void requestHome();
  void requestLibrary(std::size_t offset = 0);
  void requestSearch(const std::string& query);
  void openBrowseItem(uint64_t generation, uint32_t index);
  void playBrowseItem(uint64_t generation, uint32_t index);
  void browseCurrentQueue();

  ServiceCore& core;
  YtEngine& engine;
  std::atomic<bool> running{false};
  Thread workerThread{};
  bool threadStarted = false;
  std::string lastPublishedArtworkUrl;

  std::recursive_mutex browseMutex;
  BrowsePage currentPage;
  uint64_t currentBrowseGeneration = 0;
};

}  // namespace switube::phase2
