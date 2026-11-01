#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>

// HomeNX achievements: milestones every game has, from its play data (pdm).
// HOME (CardsNX) keeps what's earned and says the new ones when you're back from
// a game; the overlay (homenx-achievements.ovl) shows them over a game. (A game's
// own events -- a boss beaten -- would need its memory read, game by game.)
namespace switchu::achievements {

struct Milestone {
    const char* id;   // as kept in the file
    const char* name;
    const char* about;
    bool hours;       // a goal in hours played, else in times started (games played, for kOverall)
    std::uint32_t goal;
};

inline constexpr Milestone kPerGame[] = {
    {"first", "First steps", "Start it", false, 1},
    {"h1", "Getting into it", "Play it for 1 hour", true, 1},
    {"h10", "Hooked", "Play it for 10 hours", true, 10},
    {"h25", "Dedicated", "Play it for 25 hours", true, 25},
    {"h50", "Devoted", "Play it for 50 hours", true, 50},
    {"h100", "Legend", "Play it for 100 hours", true, 100},
    {"l10", "Regular player", "Start it 10 times", false, 10},
    {"l50", "Old friend", "Start it 50 times", false, 50},
};

// Across every game: Play.launches is the games played, Play.seconds all the time.
inline constexpr Milestone kOverall[] = {
    {"games5", "Explorer", "Play 5 different games", false, 5},
    {"games15", "Globetrotter", "Play 15 different games", false, 15},
    {"total100", "Marathon", "Play for 100 hours in all", true, 100},
    {"total500", "Lifer", "Play for 500 hours in all", true, 500},
};

struct Play {
    std::uint64_t seconds = 0;
    std::uint32_t launches = 0;
};

// How far along, 0..1 (1: earned).
inline float progress(const Milestone& m, const Play& p) {
    const double have = m.hours ? static_cast<double>(p.seconds) / 3600.0 : static_cast<double>(p.launches);
    const double f = have / static_cast<double>(m.goal);
    return f >= 1.0 ? 1.f : static_cast<float>(f);
}

inline constexpr const char* kPath = "sdmc:/config/SwitchU/achievements.txt";

// What's earned: "<title id> <achievement>" (or "all <achievement>") -> when
// (POSIX seconds); and the totals HOME saw last (the overlay shows them).
struct Earned {
    std::map<std::string, std::int64_t> when;
    std::uint64_t totalSeconds = 0;
    std::uint32_t gamesPlayed = 0;
    bool existed = false; // the file was there (not the first time)
};

inline std::string key(std::uint64_t titleId, const char* id) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%016llX %s", static_cast<unsigned long long>(titleId), id);
    return buf;
}

inline std::string overallKey(const char* id) { return std::string("all ") + id; }

inline Earned load() {
    Earned e;
    std::ifstream in(kPath);
    if (!in) return e;
    e.existed = true;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream words(line);
        std::string a, b;
        long long c = 0;
        if (!(words >> a >> b >> c)) continue;
        if (a == "totals") {
            e.totalSeconds = std::strtoull(b.c_str(), nullptr, 10);
            e.gamesPlayed = static_cast<std::uint32_t>(c);
        } else {
            e.when[a + " " + b] = c;
        }
    }
    return e;
}

// Written whole, through a .tmp renamed into place (the overlay may be reading it).
inline void save(const Earned& e) {
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/SwitchU", ec);
    const std::string tmp = std::string(kPath) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "# HomeNX achievements: what was earned (title id, achievement, when)\n";
        out << "totals " << e.totalSeconds << " " << e.gamesPlayed << "\n";
        for (const auto& [k, t] : e.when) out << k << " " << t << "\n";
        if (!out) return;
    }
    std::filesystem::remove(kPath, ec);
    std::filesystem::rename(tmp, kPath, ec);
}

} // namespace switchu::achievements
