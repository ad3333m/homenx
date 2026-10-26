#pragma once
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace cards {

// Writes `data` to `path` on CardsNX's writer thread (Library.cpp): the
// screen's thread never waits on the SD card, which an install or the
// pictures can keep busy for seconds. The newest data for a path wins.
void writeLater(const std::string& path, std::string data);
void flushWrites(); // everything written (before HOME closes)

// CardsNX's settings (the gear, or the Nintendo page > Settings), in
// sdmc:/config/CardsNX/settings.json.
struct Settings {
    bool music = true;
    int musicVolume = 60;     // 0..100
    bool shuffle = false;
    bool repeat = false;      // the same song again
    bool sounds = true;
    int soundVolume = 80;     // 0..100
    bool players = true;      // Nextendo's green dots
    bool motion = true;       // animations (off: things move straight to place)
    bool bgMotion = true;     // the background art drifts
    int startPage = 0;        // (old: the page order's first is where it opens now)
    std::vector<int> pageOrder = {0, 1, 2, 3}; // the pages in order: 0 games, 1 homebrew, 2 Nintendo, 3 media
    int sort = 1;             // 0 last played, 1 title, 2 most played, 3 my order (Plus > Move)
    int haze = 45;            // how much the background art is softened, 0..80
    bool boxArt = true;       // box covers on cards (off: the eShop banner)
    int accent = 0;           // the focus colour (see kAccents)
    int glass = 1;            // 0 clear, 1 frosted, 2 dark
    int launchStyle = 0;      // starting a game: 0 simple fade, 1 splash, 2 card zoom, 3 card flip, 4 dive in, 5 off
    bool clock24 = true;
    bool showFree = true;     // free space in the status pill
    bool titlePics = true;    // a 3D picture by the game's name in the panel
    // Homebrew moved by hand (+ > Move to Media / Move to Homebrew): .nro paths,
    // NRO forwarders' title IDs. Out: the ones Media takes by itself, kept out.
    std::vector<std::string> mediaIn, mediaOut;
    std::vector<std::string> sheetOrder; // Settings' tabs by name, in his order (new ones after)
    std::string user;         // who games start as (hex), "" = the last one who played
    std::string lastGame;     // the last game (or app) started from here, hex
    long long listsAt = 0;    // when the games and apps were last read in full (seconds since 1970)
    bool perfLog = true;      // sdmc:/config/CardsNX/perf.log: start-up times and slow frames
    std::vector<std::string> order[4]; // My order, per page: ids (hex)

    static constexpr const char* kPath = "sdmc:/config/CardsNX/settings.json";

    void load() {
        try {
            std::ifstream in(kPath);
            if (!in) return;
            const auto j = nlohmann::json::parse(in);
            music = j.value("music", music);
            musicVolume = j.value("musicVolume", musicVolume);
            shuffle = j.value("shuffle", shuffle);
            repeat = j.value("repeat", repeat);
            sounds = j.value("sounds", sounds);
            soundVolume = j.value("soundVolume", soundVolume);
            players = j.value("players", players);
            motion = j.value("motion", motion);
            bgMotion = j.value("bgMotion", bgMotion);
            startPage = j.value("startPage", startPage);
            pageOrder = j.value("pageOrder", pageOrder);
            std::vector<int> check = pageOrder; // each page once, else the usual order
            std::sort(check.begin(), check.end());
            if (check == std::vector<int>{0, 1, 2}) pageOrder.push_back(3); // from before there was Media
            else if (check != std::vector<int>{0, 1, 2, 3}) pageOrder = {0, 1, 2, 3};
            sort = j.value("sortOrder", sort);
            haze = j.value("haze", haze);
            boxArt = j.value("boxArt", boxArt);
            accent = j.value("accent", accent);
            glass = j.value("glass", glass);
            launchStyle = j.value("launchStyle", launchStyle);
            clock24 = j.value("clock24", clock24);
            showFree = j.value("showFree", showFree);
            titlePics = j.value("titlePics", titlePics);
            mediaIn = j.value("mediaIn", mediaIn);
            mediaOut = j.value("mediaOut", mediaOut);
            sheetOrder = j.value("sheetOrder", sheetOrder);
            user = j.value("user", user);
            lastGame = j.value("lastGame", lastGame);
            listsAt = j.value("listsAt", listsAt);
            perfLog = j.value("perfLog", perfLog);
            const char* const keys[4] = {"orderGames", "orderHomebrew", "orderNintendo", "orderMedia"};
            for (int p = 0; p < 4; ++p) order[p] = j.value(keys[p], order[p]);
        } catch (...) {
        }
    }

    void save() const {
        nlohmann::json j = {{"music", music}, {"musicVolume", musicVolume}, {"shuffle", shuffle}, {"repeat", repeat},
                            {"sounds", sounds}, {"soundVolume", soundVolume}, {"players", players},
                            {"motion", motion}, {"bgMotion", bgMotion}, {"startPage", startPage}, {"pageOrder", pageOrder},
                            {"sortOrder", sort}, {"haze", haze}, {"boxArt", boxArt}, {"accent", accent},
                            {"glass", glass}, {"launchStyle", launchStyle}, {"clock24", clock24}, {"showFree", showFree}, {"titlePics", titlePics}, {"mediaIn", mediaIn}, {"mediaOut", mediaOut}, {"sheetOrder", sheetOrder}, {"user", user}, {"lastGame", lastGame}, {"listsAt", listsAt}, {"perfLog", perfLog},
                            {"orderGames", order[0]}, {"orderHomebrew", order[1]}, {"orderNintendo", order[2]},
                            {"orderMedia", order[3]}};
        writeLater(kPath, j.dump(2));
    }
};

} // namespace cards
