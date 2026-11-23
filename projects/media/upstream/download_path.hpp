#pragma once

#include <string>
#include <string_view>
#include <sys/stat.h>

namespace switube {

inline constexpr char kDownloadsDirectory[] = "sdmc:/config/Switube/downloads/";

inline constexpr char kNativeDownloadsDirectory[] = "sdmc:/config/SwitchU/media/downloads/";

inline bool validTrackId(std::string_view id) {
    return id.size() == 11 &&
        id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string_view::npos;
}

inline std::string downloadedTrackPath(std::string_view id, const char* directory = kDownloadsDirectory) {
    if (!validTrackId(id)) return {};

    for (const char* extension : {".opus", ".m4a"}) {
        const auto path = directory + std::string(id) + extension;
        struct stat info{};
        if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0)
            return path;
    }
    return {};
}

} // namespace switube
