#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

// The lock screen's own picture, shared by both looks (HomeNX's Settings > HomeNX
// and CardsNX's Settings > Look). It's shown sharp, not frosted. The file holds
// the picture's path; missing or empty means each look's usual background.
namespace switchu::lockpicture {

inline constexpr const char* kPath = "sdmc:/config/SwitchU/lock_picture.txt";

inline std::string get() {
    std::string picture;
    std::getline(std::ifstream(kPath), picture);
    return picture;
}

inline void set(const std::string& picture) {
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/SwitchU", ec);
    std::ofstream(kPath) << picture;
}

inline constexpr const char* kPlaylistPath = "sdmc:/config/SwitchU/lock_pictures.txt";
inline constexpr const char* kRotatePath = "sdmc:/config/SwitchU/lock_picture_shuffle";

inline std::vector<std::string> playlist() {
    std::vector<std::string> pictures;
    std::ifstream in(kPlaylistPath);
    std::string path;
    while (pictures.size() < 256 && std::getline(in, path)) {
        if (!path.empty() && path.back() == '\r') path.pop_back();
        if (!path.empty() && std::find(pictures.begin(), pictures.end(), path) == pictures.end()) pictures.push_back(path);
    }
    return pictures;
}

inline void savePlaylist(const std::vector<std::string>& pictures) {
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/SwitchU", ec);
    const std::string temporary = std::string(kPlaylistPath) + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        for (const auto& path : pictures) if (!path.empty() && path.find_first_of("\r\n") == std::string::npos) out << path << '\n';
        out.flush();
        if (!out) return;
    }
    std::filesystem::rename(temporary, kPlaylistPath, ec);
    if (ec) { // fsdev's SD filesystem does not replace an existing destination.
        std::filesystem::remove(kPlaylistPath, ec);
        if (!ec) std::filesystem::rename(temporary, kPlaylistPath, ec);
    }
}

inline bool rotationEnabled() {
    std::error_code ec;
    return std::filesystem::exists(kRotatePath, ec);
}

inline void setRotation(bool enabled) {
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/SwitchU", ec);
    if (enabled) std::ofstream(kRotatePath) << '1';
    else std::filesystem::remove(kRotatePath, ec);
}

// Choose at each lock, retaining the last usable picture if files were removed.
inline std::string next(std::uint64_t seed) {
    const std::string previous = get();
    if (!rotationEnabled()) return previous;
    auto pictures = playlist();
    std::error_code ec;
    pictures.erase(std::remove_if(pictures.begin(), pictures.end(), [&ec](const std::string& path) {
        return !std::filesystem::is_regular_file(path, ec);
    }), pictures.end());
    if (pictures.empty()) return previous;
    if (pictures.size() > 1) pictures.erase(std::remove(pictures.begin(), pictures.end(), previous), pictures.end());
    const std::string chosen = pictures[seed % pictures.size()];
    set(chosen);
    return chosen;
}

// Where the Album is: the SD card's, and emuMMC's own when it runs one (its
// emummc.ini's nintendo_path) -- its screenshots are there, not in
// sdmc:/Nintendo ("the Album doesn't show all your photos").
inline std::vector<std::string> albumFolders() {
    std::vector<std::string> out{"sdmc:/Nintendo/Album"};
    std::ifstream ini("sdmc:/emuMMC/emummc.ini");
    std::string line, nintendo;
    bool enabled = false;
    while (std::getline(ini, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("enabled=", 0) == 0) enabled = line.substr(8) != "0";
        if (line.rfind("nintendo_path=", 0) == 0) nintendo = line.substr(14);
    }
    while (!nintendo.empty() && nintendo.front() == '/') nintendo.erase(0, 1);
    if (enabled && !nintendo.empty()) out.push_back("sdmc:/" + nintendo + "/Album");
    return out;
}

// The newest screenshots in every Album, newest first (its folders are the date:
// year, month, day; the files start with the time).
inline std::vector<std::string> newestScreenshots(size_t count) {
    using Named = std::map<std::string, std::vector<std::string>, std::greater<>>;
    auto inside = [](const std::vector<std::string>& dirs, bool pictures) { // by name, the newest first
        Named out;
        std::error_code ec;
        for (const std::string& dir : dirs)
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (pictures) {
                    std::string ext = e.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (!e.is_regular_file(ec) || ext != ".jpg") continue;
                } else if (!e.is_directory(ec)) {
                    continue;
                }
                out[e.path().filename().string()].push_back(e.path().string());
            }
        return out;
    };
    std::vector<std::string> shots;
    for (const auto& year : inside(albumFolders(), false))
        for (const auto& month : inside(year.second, false))
            for (const auto& day : inside(month.second, false))
                for (const auto& shot : inside(day.second, true))
                    for (const std::string& file : shot.second) {
                        if (shots.size() >= count) return shots;
                        shots.push_back(file);
                    }
    return shots;
}

} // namespace switchu::lockpicture
