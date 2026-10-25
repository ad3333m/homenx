#pragma once
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace cards {

// CardsNX's settings (the Nintendo page > CardsNX Settings, or the gear), in
// sdmc:/config/CardsNX/settings.json.
struct Settings {
    bool music = true;
    int musicVolume = 60;     // 0..100
    bool sounds = true;
    bool players = true;      // Nextendo's green dots
    bool motion = true;       // animations (off: things move straight to place)
    int startPage = 0;        // 0 games, 1 homebrew, 2 Nintendo
    int sort = 0;             // 0 last played, 1 title, 2 most played
    int haze = 45;            // how much the background art is softened, 0..80
    bool boxArt = true;       // box covers on cards (off: the eShop banner)

    static constexpr const char* kPath = "sdmc:/config/CardsNX/settings.json";

    void load() {
        try {
            std::ifstream in(kPath);
            if (!in) return;
            const auto j = nlohmann::json::parse(in);
            music = j.value("music", music);
            musicVolume = j.value("musicVolume", musicVolume);
            sounds = j.value("sounds", sounds);
            players = j.value("players", players);
            motion = j.value("motion", motion);
            startPage = j.value("startPage", startPage);
            sort = j.value("sort", sort);
            haze = j.value("haze", haze);
            boxArt = j.value("boxArt", boxArt);
        } catch (...) {
        }
    }

    void save() const {
        std::error_code ec;
        std::filesystem::create_directories("sdmc:/config/CardsNX", ec);
        nlohmann::json j = {{"music", music}, {"musicVolume", musicVolume}, {"sounds", sounds},
                            {"players", players}, {"motion", motion}, {"startPage", startPage},
                            {"sort", sort}, {"haze", haze}, {"boxArt", boxArt}};
        std::ofstream(kPath, std::ios::trunc) << j.dump(2);
    }
};

} // namespace cards
