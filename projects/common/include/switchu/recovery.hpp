#pragma once

#include <sys/stat.h>

namespace switchu::recovery {

inline bool validMarker(const char* path) {
    struct stat marker{};
    return stat(path, &marker) == 0 && S_ISREG(marker.st_mode) && marker.st_size == 0;
}

inline bool enabled() {
    // Evaluated once per HOME process so loads and saves use the same mode.
    static const bool active = validMarker("sdmc:/config/SwitchU/recovery.flag");
    return active;
}

} // namespace switchu::recovery
