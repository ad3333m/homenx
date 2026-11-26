#pragma once

#include "track.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace switube::playlists {
namespace fs = std::filesystem;

inline bool validName(std::string_view name) {
    return !name.empty() && name.size() <= 60 && name.front() != '.' &&
        name.back() != '.' && name.back() != ' ' &&
        name.find_first_of("/\\:*?\"<>|") == name.npos &&
        std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; });
}

inline std::vector<YtTrack> list(const fs::path& directory) {
    std::vector<YtTrack> result;
    if (!fs::exists(directory)) return result;
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto name = entry.path().stem().string();
        if (entry.path().extension() != ".json" || !validName(name) || !entry.is_regular_file()) continue;
        YtTrack item;
        item.title = name;
        result.push_back(std::move(item));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.title < b.title; });
    return result;
}

inline void save(const fs::path& directory, const std::string& name, const std::vector<YtTrack>& tracks) {
    if (!validName(name)) throw std::runtime_error("Choose a different playlist name");
    if (tracks.empty()) throw std::runtime_error("Add songs to the queue first");
    fs::create_directories(directory);
    const auto path = directory / (name + ".json");
    if (fs::exists(path)) throw std::runtime_error("A playlist with that name already exists");
    const auto temp = directory / (name + ".tmp");
    std::ofstream out(temp, std::ios::trunc);
    out << '[';
    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto& t = tracks[i];
        if (i) out << ',';
        out << nlohmann::json::array({t.id, t.title, t.artist, t.durationMs}).dump(
            -1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    out << ']';
    out.close();
    if (!out) throw std::runtime_error("Could not save playlist");
    fs::rename(temp, path);
}

inline std::vector<YtTrack> load(const fs::path& directory, const std::string& name) {
    if (!validName(name)) throw std::runtime_error("Invalid playlist name");
    const auto path = directory / (name + ".json");
    if (fs::file_size(path) > 512 * 1024) throw std::runtime_error("Playlist too large");
    std::ifstream in(path);
    auto data = nlohmann::json::parse(in, nullptr, false);
    if (!data.is_array() || data.empty() || data.size() > 256) throw std::runtime_error("Invalid playlist");
    std::vector<YtTrack> result;
    for (const auto& row : data) {
        YtTrack t;
        t.id = row.at(0).get<std::string>();
        t.title = row.at(1).get<std::string>();
        t.artist = row.at(2).get<std::string>();
        t.durationMs = row.at(3).get<uint32_t>();
        result.push_back(std::move(t));
    }
    return result;
}
}
