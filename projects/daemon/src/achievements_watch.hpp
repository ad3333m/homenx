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
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// During a game: a controller coming or going, the battery running low. Ultrahand
// shows them too, each with its own icon (none without Ultrahand: the files would
// only pile up; none with Settings > Console > Battery and controller pop-ups off).
inline void drawStatusIcon(const char* path, bool battery) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return;
    std::filesystem::create_directories("sdmc:/config/ultrahand/assets/notifications", ec);
    constexpr int kDim = 50;
    std::vector<std::uint8_t> px(kDim * kDim * 4, 0);
    auto dot = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        std::uint8_t* p = &px[static_cast<size_t>(y * kDim + x) * 4];
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = 255;
    };
    for (int y = 0; y < kDim; ++y)
        for (int x = 0; x < kDim; ++x) {
            if (battery) { // an outline, a tip and a red end
                const bool edge = x >= 4 && x <= 41 && y >= 14 && y <= 35 && !(x >= 7 && x <= 38 && y >= 17 && y <= 32);
                const bool tip = x >= 42 && x <= 45 && y >= 20 && y <= 29;
                const bool left = x >= 10 && x <= 16 && y >= 20 && y <= 29;
                if (edge || tip) dot(x, y, 235, 235, 240);
                else if (left) dot(x, y, 255, 59, 48);
            } else { // a controller: its body, two grips, two buttons left out
                const bool body = x >= 8 && x <= 41 && y >= 15 && y <= 30;
                const float lx = (x - 12.f) / 6.f, rx = (x - 37.f) / 6.f, gy = (y - 30.f) / 11.f;
                const bool grips = y > 30 && (lx * lx + gy * gy <= 1.f || rx * rx + gy * gy <= 1.f);
                const bool holes = (std::abs(x - 16) <= 2 && std::abs(y - 22) <= 2) || (std::abs(x - 33) <= 2 && std::abs(y - 22) <= 2);
                if ((body || grips) && !holes) dot(x, y, 64, 156, 255);
            }
        }
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(px.data(), 1, px.size(), f);
        std::fclose(f);
    }
}

inline void statusPopup(const char* app, bool battery, const std::string& title, const std::string& text) {
    char icon[96];
    std::snprintf(icon, sizeof(icon), "sdmc:/config/ultrahand/assets/notifications/%s.rgba", app);
    drawStatusIcon(icon, battery);
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/ultrahand/notifications", ec);
    char path[96];
    std::snprintf(path, sizeof(path), "sdmc:/config/ultrahand/notifications/%s-%llu.notify", app,
                  static_cast<unsigned long long>(armGetSystemTick()));
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "{\"title\":\"%s\",\"text\":\"%s\",\"font_size\":22,\"priority\":20,\"duration\":4000,\"show_time\":\"false\"}",
                     jsonText(title).c_str(), jsonText(text).c_str());
        std::fclose(f);
    }
}

inline bool statusPopupsAllowed() {
    std::error_code ec;
    return std::filesystem::exists("sdmc:/switch/.overlays/ovlmenu.ovl", ec)
        && !std::filesystem::exists("sdmc:/config/SwitchU/no_popups", ec);
}

inline void watchStatus(bool inGame, bool allowed) {
    static std::uint32_t styles[8] = {};
    static bool known = false;
    static std::uint32_t batteryWas = 101;
    static std::uint64_t batteryAt = 0;
    if (!inGame) {
        known = false; // (changes on HOME were said there)
        return;
    }
    for (int id = 0; id < 8; ++id) {
        const std::uint32_t now = hidGetNpadStyleSet(static_cast<HidNpadIdType>(HidNpadIdType_No1 + id));
        if (known && allowed && (now != 0) != (styles[id] != 0)) {
            const std::uint32_t s = now ? now : styles[id];
            const char* name = (s & HidNpadStyleTag_NpadJoyDual) ? "Joy-Con pair"
                             : (s & HidNpadStyleTag_NpadFullKey) ? "Pro Controller"
                             : (s & HidNpadStyleTag_NpadJoyLeft) ? "Joy-Con (L)"
                             : (s & HidNpadStyleTag_NpadJoyRight) ? "Joy-Con (R)" : "Controller";
            statusPopup("HomeNXPad", false, now ? "Controller connected" : "Controller disconnected", name);
        }
        styles[id] = now;
    }
    known = true;
    const std::uint64_t tick = armGetSystemTick();
    if (tick < batteryAt) return;
    batteryAt = tick + armNsToTicks(30'000'000'000ULL); // (the battery: every 30 s is plenty)
    std::uint32_t pct = 0;
    PsmChargerType charger = PsmChargerType_Unconnected;
    if (R_FAILED(psmGetBatteryChargePercentage(&pct)) || R_FAILED(psmGetChargerType(&charger))) return;
    if (allowed && charger == PsmChargerType_Unconnected && pct <= 15 && batteryWas > 15)
        statusPopup("HomeNXBattery", true, "Battery low", std::to_string(pct) + "% left");
    batteryWas = pct;
}

inline std::string gameName(std::uint64_t title) {
    switchu::control_cache::Meta meta;
    if (!switchu::control_cache::readMeta(title, meta)) return {};
    return meta.name[0] ? meta.name : meta.english_name;
}

// The hour on the console's clock, in its time zone (12 when it can't tell).
inline int localHour() {
    u64 now = 0;
    TimeCalendarTime t{};
    TimeCalendarAdditionalInfo info{};
    if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &now)) || R_FAILED(timeToCalendarTimeWithMyRule(now, &t, &info)))
        return 12;
    return t.hour;
}

// What the game's play reached: kept, and shown. (The first time, when there's no
// file yet, what was earned before HomeNX counted is kept without a word.)
inline void award(std::uint64_t title, std::uint64_t gameSeconds, std::uint64_t sessionSeconds) {
    namespace ach = switchu::achievements;
    if (!ach::milestonesEnabled()) return;
    ach::Earned e = ach::load(); // (HOME may have written it since)
    const bool quiet = !e.existed;
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    std::vector<const ach::Milestone*> mine, overall;
    const ach::Facts played{gameSeconds};
    for (const ach::Milestone& m : ach::kPerGame)
        if (ach::progress(m, played) >= 1.f && e.when.emplace(ach::key(title, m.id), now).second) mine.push_back(&m);
    if (sessionSeconds >= 3 * 3600 && e.when.emplace(ach::key(title, "session3"), now).second)
        mine.push_back(ach::milestone("session3"));
    const ach::Facts all{e.totalSeconds + sessionSeconds, e.gamesPlayed, 0, ach::earnedFromPacks(e)};
    for (const ach::Milestone& m : ach::kOverall)
        if (ach::progress(m, all) >= 1.f && e.when.emplace(ach::overallKey(m.id), now).second) overall.push_back(&m);
    // Past the first minutes of a game, between midnight and 4 a.m.
    if (sessionSeconds >= 300 && localHour() < 4 && e.when.emplace(ach::overallKey("night"), now).second)
        overall.push_back(ach::milestone("night"));
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
    std::uint64_t processId = 0; // the process attached to
    bool readFailed = false;    // a memory read failed in the last check
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

// (Again when a read fails or the process changes: a collection's games are programs of one
// application, its ID + 1, + 2..., and starting one swaps the process.)
inline bool attach(Live& l) {
    namespace dm = switchu::dmnt;
    if (!dm::open()) return false;
    if (!dm::hasCheatProcess() && !dm::forceOpenCheatProcess()) return false;
    dm::Metadata m{};
    if (!dm::metadata(m) || (m.program_id & ~0x1FFFULL) != (l.title & ~0x1FFFULL)) return false;
    l.bases = {m.main_nso.base, m.heap.base, m.alias.base};
    l.processId = m.process_id;
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
    l.readFailed = false;
    const switchu::packs::Reader read = [&l](std::uint64_t address, void* out, std::size_t size) {
        const bool ok = switchu::dmnt::read(address, out, size);
        if (!ok) l.readFailed = true;
        return ok;
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
    // What they bring: all of the game's own (Perfect), and the count across games.
    std::vector<const ach::Milestone*> more;
    if (l.done.size() >= l.pack.list.size() && e.when.emplace(ach::key(l.title, "perfect"), when).second)
        more.push_back(ach::milestone("perfect"));
    const ach::Facts f{0, 0, 0, ach::earnedFromPacks(e)};
    for (const ach::Milestone& m : ach::kOverall)
        if (m.kind == ach::Kind::Earned && ach::progress(m, f) >= 1.f && e.when.emplace(ach::overallKey(m.id), when).second)
            more.push_back(&m);
    ach::save(e);
    const std::string game = l.pack.game.empty() ? gameName(l.title) : l.pack.game;
    for (const auto* a : now) notify(a->name, game);
    for (const ach::Milestone* m : more) notify(m->name, m->kind == ach::Kind::Moment ? game : std::string());
}

inline void threadFunc(void*) {
    const bool pdm = R_SUCCEEDED(pdmqryInitialize());
    std::uint64_t title = 0, base = 0, playedNs = 0, checkedNs = 0, last = armGetSystemTick();
    std::uint32_t session = ~0u;
    bool known = false, counted = false; // counted: a game (not a media app: achievements_skip.txt)
    // Read from the SD once each time a game comes to the front (Settings change only in HOME).
    bool popups = false, milestones = false;
    std::uint64_t front = 0;
    Live live;
    while (g_running.load()) {
        // Every second while a game with a pack is in front (it unlocks at once), every 2 s
        // with another game (a controller's pop-up shouldn't be late), else every 10 s.
        const bool fast = live.pack.valid() && live.title == g_title.load();
        waitSingle(waiterForUEvent(&g_wake),
                   fast ? 1'000'000'000ULL : g_title.load() ? 2'000'000'000ULL : 10'000'000'000ULL);
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
            counted = t && !switchu::achievements::loadSkipped().count(t);
            live.attached = false; // (a new process)
        }
        if (t && t != front) {
            popups = statusPopupsAllowed();
            milestones = switchu::achievements::milestonesEnabled();
        }
        front = t;
        watchStatus(t != 0, popups);
        if (!t) continue; // (HOME or nothing in front: the time isn't play)
        if (t != live.title) loadPack(live, t);
        if (live.pack.valid()) {
            // Attached: one metadata call tells whether it's still the same process
            // (dmnt can attach to a swapped-in program by itself); the full attach
            // (open, has/force-open, metadata) only when it isn't or a read failed.
            bool recheck = !live.attached && now >= live.retryAt;
            if (live.attached) {
                switchu::dmnt::Metadata m{};
                recheck = live.readFailed || !switchu::dmnt::metadata(m) || m.process_id != live.processId;
            }
            if (recheck) {
                live.attached = attach(live);
                if (!live.attached) live.retryAt = now + armNsToTicks(15'000'000'000ULL);
            }
            if (live.attached) checkPack(live);
        }
        if (!pdm || !counted || !milestones) continue;
        bool check = false;
        if (!known) {
            PdmPlayStatistics st{};
            if (R_FAILED(pdmqryQueryPlayStatisticsByApplicationId(t, false, &st))) continue;
            base = st.playtime / 1'000'000'000ULL;
            known = check = true;
        } else {
            playedNs += ns;
            check = playedNs - checkedNs >= 60'000'000'000ULL; // the hours: once a minute is plenty
        }
        if (check) {
            checkedNs = playedNs;
            award(t, base + playedNs / 1'000'000'000ULL, playedNs / 1'000'000'000ULL);
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
