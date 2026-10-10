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

// Sorted by name. `skipPath` (HomeNX's own .nro) is left out. Without
// `withIcons` the icons stay on the SD (see readHomebrewIcon) so it's quick.
std::vector<HomebrewApp> scanHomebrew(const std::string& skipPath, bool withIcons = true);
std::vector<std::uint8_t> readHomebrewIcon(const std::string& path);

// Reads one .nro's name/author/version/icon. False if the file can't be opened.
bool readHomebrew(const std::string& path, HomebrewApp& app, bool withIcon = true);
// Sphaira's .nro: the usual folders first, then any .nro whose name is
// "sphaira". `allowHbmenu` also accepts sdmc:/hbmenu.nro when it is Sphaira.
std::string findSphaira(bool allowHbmenu);
