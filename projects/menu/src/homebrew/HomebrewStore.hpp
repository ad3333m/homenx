#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Homebrew on the SD card: every .nro in sdmc:/switch/ and one folder below,
// with the name, author, version and icon embedded in its asset section.
struct HomebrewApp {
    std::string path;
    std::string name;
    std::string author;
    std::string version;
    std::vector<std::uint8_t> icon; // JPEG, may be empty
};

// Sorted by name. `skipPath` (HomeNX's own .nro) is left out.
std::vector<HomebrewApp> scanHomebrew(const std::string& skipPath);
