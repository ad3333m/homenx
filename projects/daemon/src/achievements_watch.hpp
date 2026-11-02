#pragma once
// HomeNX achievements while a game is played (switchu/achievements.hpp): this
// thread counts the time the game is in front, and when a milestone is reached
// it's kept (config/SwitchU/achievements.txt) and shown over the game through
// Ultrahand's notifications (config/ultrahand/notifications/*.notify), whatever
// overlay is loaded. HOME (CardsNX) says what's new too, for consoles without it.
// A game with a pack (switchu/achievement_packs.hpp) also has its own: its memory
// is read every second (dmnt:cht) and each one unlocks the moment it happens.
#include <switch.h>
#include <switchu/achievement_packs.hpp>
#include <switchu/achievements.hpp>
#include <switchu/control_cache.hpp>
#include <switchu/dmnt_cheat.hpp>
#include <switchu/file_log.hpp>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace switchu::daemon::achievements_watch {

// Set by the daemon's main loop: the game in front now (0: none, HOME, an applet,
// or homebrew in a borrowed game), and a count that changes at every launch and
// exit (a new session: by then pdm has counted the one before).
inline std::atomic<std::uint64_t> g_title{0};
inline std::atomic<std::uint32_t> g_sessions{0};

namespace detail {
inline std::atomic<bool> g_running{false};
inline Thread g_thread;
inline UEvent g_wake;
inline bool g_started = false;

inline std::string jsonText(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

// Ultrahand shows <app>.rgba (50 x 50, RGBA8888) beside a notification from
// <app>-*.notify: a gold trophy, drawn here the first time.
inline void ensureIcon() {
    constexpr const char* kIcon = "sdmc:/config/ultrahand/assets/notifications/HomeNX.rgba";
    std::error_code ec;
    if (std::filesystem::exists(kIcon, ec)) return;
    std::filesystem::create_directories("sdmc:/config/ultrahand/assets/notifications", ec);
    constexpr int kDim = 50;
    std::vector<std::uint8_t> px(kDim * kDim * 4, 0);
    for (int y = 0; y < kDim; ++y)
        for (int x = 0; x < kDim; ++x) {
            const float bx = (x - 24.5f) / 14.f, by = (y - 9.f) / 20.f;
            const bool bowl = y >= 9 && y <= 29 && bx * bx + by * by <= 1.f;
            const float l = (x - 10.f) * (x - 10.f) + (y - 15.f) * (y - 15.f);
            const float r = (x - 39.f) * (x - 39.f) + (y - 15.f) * (y - 15.f);
            const bool handles = (l >= 16.f && l <= 36.f && x < 13) || (r >= 16.f && r <= 36.f && x > 36);
            const bool stem = x >= 22 && x <= 27 && y > 29 && y <= 37;
            const bool base = x >= 14 && x <= 35 && y > 37 && y <= 43;
            if (!(bowl || handles || stem || base)) continue;
            const float light = 1.f - 0.35f * static_cast<float>(x + y) / (2.f * kDim); // lighter top-left
            std::uint8_t* p = &px[static_cast<size_t>(y * kDim + x) * 4];
            p[0] = static_cast<std::uint8_t>(255 * light);
            p[1] = static_cast<std::uint8_t>(196 * light);
            p[2] = static_cast<std::uint8_t>(30 * light);
            p[3] = 255;
        }
    if (FILE* f = std::fopen(kIcon, "wb")) {
        std::fwrite(px.data(), 1, px.size(), f);
        std::fclose(f);
    }
}

inline void notify(const std::string& name, const std::string& game) {
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/ultrahand/notifications", ec);
    ensureIcon();
    char path[96];
    std::snprintf(path, sizeof(path), "sdmc:/config/ultrahand/notifications/HomeNX-%llu.notify",
                  static_cast<unsigned long long>(armGetSystemTick()));
    const std::string text = game.empty() ? name : name + " - " + game;
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f,
                     "{\"title\":\"Achievement unlocked\",\"text\":\"%s\",\"font_size\":22,\"priority\":30,"
                     "\"duration\":6000,\"show_time\":\"false\"}",
                     jsonText(text).c_str());
        std::fclose(f);
    }
    switchu::FileLog::log("[achievements] %s (%s)", name.c_str(), game.c_str());
}

inline std::string gameName(std::uint64_t title) {
    switchu::control_cache::Meta meta;
    if (!switchu::control_cache::readMeta(title, meta)) return {};
    return meta.name[0] ? meta.name : meta.english_name;
}

// What the game's play reached: kept, and shown. (The first time, when there's no
// file yet, what was earned before HomeNX counted is kept without a word.)
inline void award(std::uint64_t title, const switchu::achievements::Play& play, std::uint64_t sessionSeconds) {
    namespace ach = switchu::achievements;
    ach::Earned e = ach::load(); // (HOME may have written it since)
    const bool quiet = !e.existed;
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    std::vector<const ach::Milestone*> mine, overall;
    for (const ach::Milestone& m : ach::kPerGame)
        if (ach::progress(m, play) >= 1.f && e.when.emplace(ach::key(title, m.id), now).second) mine.push_back(&m);
    const ach::Play all{e.totalSeconds + sessionSeconds, e.gamesPlayed};
    for (const ach::Milestone& m : ach::kOverall)
        if (ach::progress(m, all) >= 1.f && e.when.emplace(ach::overallKey(m.id), now).second) overall.push_back(&m);
    if (mine.empty() && overall.empty() && !quiet) return;
    ach::save(e);
    if (quiet) return;
    const std::string game = mine.empty() ? std::string() : gameName(title);
    for (const ach::Milestone* m : mine) notify(m->name, game);
    for (const ach::Milestone* m : overall) notify(m->name, std::string());
}

// The game in front's pack, and its memory as dmnt sees it.
struct Live {
    std::uint64_t title = 0;
    switchu::packs::Pack pack;
    std::set<std::string> done; // its achievements earned already
    std::map<std::string, int> streak; // seconds in a row each one's condition has held
    bool attached = false;
    switchu::packs::Bases bases;
    std::string build;          // this version's build id (upper hex)
    std::uint64_t retryAt = 0;  // (ticks) attaching didn't work: again then
};

inline void loadPack(Live& l, std::uint64_t title) {
    namespace ach = switchu::achievements;
    l = Live{};
    l.title = title;
    l.pack = switchu::packs::load(title);
    if (!l.pack.valid()) return;
    const ach::Earned e = ach::load();
    for (const auto& a : l.pack.list)
        if (e.when.count(ach::key(title, ("p." + a.id).c_str()))) l.done.insert(a.id);
    switchu::FileLog::log("[achievements] pack for %016llX: %zu achievements, %zu earned",
                          static_cast<unsigned long long>(title), l.pack.list.size(), l.done.size());
}

// (Again at every check: a collection's games are programs of one application, its ID + 1,
// + 2..., and starting one swaps the process.)
inline bool attach(Live& l) {
    namespace dm = switchu::dmnt;
    if (!dm::open()) return false;
    if (!dm::hasCheatProcess() && !dm::forceOpenCheatProcess()) return false;
    dm::Metadata m{};
    if (!dm::metadata(m) || (m.program_id & ~0x1FFFULL) != (l.title & ~0x1FFFULL)) return false;
    l.bases = {m.main_nso.base, m.heap.base, m.alias.base};
    char hex[17] = {};
    for (int i = 0; i < 8; ++i) std::snprintf(hex + i * 2, 3, "%02X", m.main_nso_build_id[i]);
    if (l.build != hex) {
        l.build = hex;
        l.streak.clear();
        switchu::FileLog::log("[achievements] %s: build %s", l.pack.game.c_str(), l.build.c_str());
    }
    return true;
}

// The pack's achievements whose condition has held for 3 checks in a row (a second each: a
// pointer read during a loading screen can briefly give nonsense): kept and shown, at once.
inline void checkPack(Live& l) {
    namespace ach = switchu::achievements;
    const switchu::packs::Reader read = [](std::uint64_t address, void* out, std::size_t size) {
        return switchu::dmnt::read(address, out, size);
    };
    std::vector<const switchu::packs::Achievement*> now;
    for (const auto& a : l.pack.list) {
        if (l.done.count(a.id)) continue;
        if (!switchu::packs::anyHolds(a, l.build, l.bases, read)) l.streak.erase(a.id);
        else if (++l.streak[a.id] >= 3) now.push_back(&a);
    }
    if (now.empty()) return;
    ach::Earned e = ach::load();
    const std::int64_t when = static_cast<std::int64_t>(std::time(nullptr));
    for (const auto* a : now) {
        e.when.emplace(ach::key(l.title, ("p." + a->id).c_str()), when);
        l.done.insert(a->id);
    }
    ach::save(e);
    const std::string game = l.pack.game.empty() ? gameName(l.title) : l.pack.game;
    for (const auto* a : now) notify(a->name, game);
}

inline void threadFunc(void*) {
    const bool pdm = R_SUCCEEDED(pdmqryInitialize());
    std::uint64_t title = 0, base = 0, playedNs = 0, checkedNs = 0, last = armGetSystemTick();
    std::uint32_t launches = 0, session = ~0u;
    bool known = false;
    Live live;
    while (g_running.load()) {
        // Every second while a game with a pack is in front (it unlocks at once), else every 10 s.
        const bool fast = live.pack.valid() && live.title == g_title.load();
        waitSingle(waiterForUEvent(&g_wake), fast ? 1'000'000'000ULL : 10'000'000'000ULL);
        if (!g_running.load()) break;
        const std::uint64_t now = armGetSystemTick();
        std::uint64_t ns = armTicksToNs(now - last);
        last = now;
        if (ns > 20'000'000'000ULL) ns = 0; // (the console slept meanwhile: that wasn't play)
        const std::uint64_t t = g_title.load();
        const std::uint32_t s = g_sessions.load();
        if (s != session || (t && t != title)) { // a new launch: pdm has the time before it
            session = s;
            title = t;
            playedNs = checkedNs = 0;
            known = false;
            live.attached = false; // (a new process)
        }
        if (!t) continue; // (HOME or nothing in front: the time isn't play)
        if (t != live.title) loadPack(live, t);
        if (live.pack.valid()) {
            if (live.attached || now >= live.retryAt) {
                live.attached = attach(live);
                if (!live.attached) live.retryAt = now + armNsToTicks(15'000'000'000ULL);
            }
            if (live.attached) checkPack(live);
        }
        if (!pdm) continue;
        bool check = false;
        if (!known) {
            PdmPlayStatistics st{};
            if (R_FAILED(pdmqryQueryPlayStatisticsByApplicationId(t, false, &st))) continue;
            base = st.playtime / 1'000'000'000ULL;
            launches = st.total_launches;
            known = check = true; // (a launch milestone: right away)
        } else {
            playedNs += ns;
            check = playedNs - checkedNs >= 60'000'000'000ULL; // the hours: once a minute is plenty
        }
        if (check) {
            checkedNs = playedNs;
            award(t, {base + playedNs / 1'000'000'000ULL, launches ? launches : 1u}, playedNs / 1'000'000'000ULL);
        }
    }
    switchu::dmnt::close();
    if (pdm) pdmqryExit();
}
} // namespace detail

inline void start() {
    ueventCreate(&detail::g_wake, false);
    detail::g_running = true;
    if (R_SUCCEEDED(threadCreate(&detail::g_thread, detail::threadFunc, nullptr, nullptr, 0x10000, 0x3B, -2))
        && R_SUCCEEDED(threadStart(&detail::g_thread)))
        detail::g_started = true;
    switchu::FileLog::log("[achievements] watch %s", detail::g_started ? "started" : "didn't start");
}

inline void stop() {
    if (!detail::g_started) return;
    detail::g_running = false;
    ueventSignal(&detail::g_wake);
    threadWaitForExit(&detail::g_thread);
    threadClose(&detail::g_thread);
    detail::g_started = false;
}

} // namespace switchu::daemon::achievements_watch
