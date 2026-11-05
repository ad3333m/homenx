#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

// HomeNX achievements: milestones every game has, from its play data (pdm) and what
// happens while it's played, besides a game's own (achievement_packs.hpp). HOME
// (CardsNX) keeps what's earned and says the new ones when you're back from a game;
// the daemon shows them the moment they happen; the overlay (homenx-achievements.ovl)
// lists them over a game. Only games count: not media apps (achievements_skip.txt).
namespace switchu::achievements {

// What a milestone counts.
enum class Kind : std::uint8_t {
    Hours,     // play time: the game's (kPerGame), every game's (kOverall)
    Games,     // different games played
    Installed, // games installed (HOME counts them)
    Earned,    // the games' own achievements earned (their packs)
    Moment,    // earned the moment it happens: no count to show
};

// The milestones work for every game; each game's own goals (its pack) stay either way.
// Settings > Achievements turns them off. What was earned is kept.
inline constexpr const char* kMilestonesOffFlag = "sdmc:/config/SwitchU/achievement_milestones_off";
inline bool milestonesEnabled() {
    std::error_code ec;
    return !std::filesystem::exists(kMilestonesOffFlag, ec);
}
inline void setMilestonesEnabled(bool enabled) {
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/SwitchU", ec);
    if (enabled) std::filesystem::remove(kMilestonesOffFlag, ec);
    else std::ofstream(kMilestonesOffFlag) << '1';
}

struct Milestone {
    const char* id;   // as kept in the file
    const char* name;
    const char* about;
    Kind kind;
    std::uint32_t goal; // hours, games or achievements (a Moment: unused)
};

inline constexpr Milestone kPerGame[] = {
    {"h10", "Hooked", "Play it for 10 hours", Kind::Hours, 10},
    {"h25", "Dedicated", "Play it for 25 hours", Kind::Hours, 25},
    {"h50", "Devoted", "Play it for 50 hours", Kind::Hours, 50},
    {"h100", "Legend", "Play it for 100 hours", Kind::Hours, 100},
    {"session3", "Lost in it", "Play it for 3 hours in one go", Kind::Moment, 0},
    {"back", "Welcome back", "Play it again after 6 months away", Kind::Moment, 0},
    {"perfect", "Perfect", "Earn every one of its own achievements", Kind::Moment, 0}, // (only with a pack)
};

inline constexpr Milestone kOverall[] = {
    {"games5", "Explorer", "Play 5 different games", Kind::Games, 5},
    {"games15", "Globetrotter", "Play 15 different games", Kind::Games, 15},
    {"lib25", "Collector", "Have 25 games installed", Kind::Installed, 25},
    {"lib50", "Curator", "Have 50 games installed", Kind::Installed, 50},
    {"lib100", "Archivist", "Have 100 games installed", Kind::Installed, 100},
    {"hunt10", "Achievement hunter", "Earn 10 of your games' own achievements", Kind::Earned, 10},
    {"hunt50", "Trophy room", "Earn 50 of your games' own achievements", Kind::Earned, 50},
    {"night", "Night owl", "Play a game between midnight and 4 a.m.", Kind::Moment, 0},
    {"total100", "Marathon", "Play games for 100 hours in all", Kind::Hours, 100},
    {"total500", "Lifer", "Play games for 500 hours in all", Kind::Hours, 500},
    {"total1000", "Unstoppable", "Play games for 1,000 hours in all", Kind::Hours, 1000},
};

inline const Milestone* milestone(const char* id) {
    for (const Milestone& m : kPerGame)
        if (std::strcmp(m.id, id) == 0) return &m;
    for (const Milestone& m : kOverall)
        if (std::strcmp(m.id, id) == 0) return &m;
    return nullptr;
}

// The numbers milestones count: one game's (seconds only), or every game's.
struct Facts {
    std::uint64_t seconds = 0;
    std::uint32_t games = 0;
    std::uint32_t installed = 0;
    std::uint32_t earned = 0;
};

// How far along, 0..1 (1: earned). A Moment has no count: 0 until it happens.
inline float progress(const Milestone& m, const Facts& f) {
    double have = 0.0;
    switch (m.kind) {
        case Kind::Hours: have = static_cast<double>(f.seconds) / 3600.0; break;
        case Kind::Games: have = f.games; break;
        case Kind::Installed: have = f.installed; break;
        case Kind::Earned: have = f.earned; break;
        case Kind::Moment: return 0.f;
    }
    const double r = have / static_cast<double>(m.goal);
    return r >= 1.0 ? 1.f : static_cast<float>(r);
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

// The games' own achievements earned (their packs' are kept as "p.<id>").
inline std::uint32_t earnedFromPacks(const Earned& e) {
    std::uint32_t n = 0;
    for (const auto& [k, t] : e.when) n += k.find(" p.") != std::string::npos ? 1 : 0;
    return n;
}

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

// Apps that aren't games (HOME's Media page: YouTube and the like): their time isn't play.
inline constexpr const char* kSkipPath = "sdmc:/config/SwitchU/achievements_skip.txt";

inline std::set<std::uint64_t> loadSkipped() {
    std::set<std::uint64_t> out;
    std::ifstream in(kSkipPath);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty()) out.insert(std::strtoull(line.c_str(), nullptr, 16));
    return out;
}

inline void saveSkipped(const std::set<std::uint64_t>& ids) {
    if (ids == loadSkipped()) return;
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/SwitchU", ec);
    std::ofstream out(kSkipPath, std::ios::trunc);
    char buf[24];
    for (const std::uint64_t id : ids) {
        std::snprintf(buf, sizeof(buf), "%016llX\n", static_cast<unsigned long long>(id));
        out << buf;
    }
}

} // namespace switchu::achievements
