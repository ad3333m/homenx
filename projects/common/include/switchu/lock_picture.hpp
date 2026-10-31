#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
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

// The newest screenshots in the Album, newest first (its folders are the date).
inline std::vector<std::string> newestScreenshots(size_t count) {
    auto newestDirs = [](const std::string& dir) {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.is_directory(ec)) out.push_back(e.path().string());
        std::sort(out.rbegin(), out.rend());
        return out;
    };
    std::vector<std::string> shots;
    std::error_code ec;
    for (const std::string& year : newestDirs("sdmc:/Nintendo/Album"))
        for (const std::string& month : newestDirs(year))
            for (const std::string& day : newestDirs(month)) {
                std::vector<std::string> files;
                for (const auto& e : std::filesystem::directory_iterator(day, ec)) {
                    std::string ext = e.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (e.is_regular_file(ec) && ext == ".jpg") files.push_back(e.path().string());
                }
                std::sort(files.rbegin(), files.rend());
                for (const std::string& f : files) {
                    if (shots.size() >= count) return shots;
                    shots.push_back(f);
                }
            }
    return shots;
}

} // namespace switchu::lockpicture
