#pragma once
#include <cstdio>

// Settings > HomeNX > Pointer (L3): 0 off, 1 motion (gyro), 2 left stick, in
// sdmc:/config/SwitchU/pointer_mode.txt. Motion is what HOME always did.
namespace pointersetting {

inline constexpr const char* kPath = "sdmc:/config/SwitchU/pointer_mode.txt";

inline int read() {
    int mode = 1;
    if (FILE* f = std::fopen(kPath, "r")) {
        if (std::fscanf(f, "%d", &mode) != 1 || mode < 0 || mode > 2) mode = 1;
        std::fclose(f);
    }
    return mode;
}

inline void write(int mode) {
    if (FILE* f = std::fopen(kPath, "w")) {
        std::fprintf(f, "%d", mode);
        std::fclose(f);
    }
}

} // namespace pointersetting
