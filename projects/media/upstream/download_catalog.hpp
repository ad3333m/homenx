#pragma once
#include "download_path.hpp"
#include "track.hpp"
#include "NXConsoleLog.h"
#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <nlohmann/json.hpp>

namespace switube {
inline std::vector<YtTrack> loadSwitubeDownloads() {
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

inline void appendNativeDownloads(std::vector<YtTrack>& list) {
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

inline std::vector<YtTrack> loadDownloads() {
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

} // namespace switube
