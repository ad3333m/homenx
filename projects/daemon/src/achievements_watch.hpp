#pragma once
// HomeNX achievements while a game is played (switchu/achievements.hpp): this
// thread counts the time the game is in front, and when a milestone is reached
// it's kept (config/SwitchU/achievements.txt) and shown over the game through
// Ultrahand's notifications (config/ultrahand/notifications/*.notify), whatever
// overlay is loaded. HOME (CardsNX) says what's new too, for consoles without it.
#include <switch.h>
#include <switchu/achievements.hpp>
#include <switchu/control_cache.hpp>
#include <switchu/file_log.hpp>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
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

inline void notify(const switchu::achievements::Milestone& m, const std::string& game) {
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/ultrahand/notifications", ec);
    ensureIcon();
    char path[96];
    std::snprintf(path, sizeof(path), "sdmc:/config/ultrahand/notifications/HomeNX-%llu.notify",
                  static_cast<unsigned long long>(armGetSystemTick()));
    const std::string text = game.empty() ? std::string(m.name) : std::string(m.name) + " - " + game;
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f,
                     "{\"title\":\"Achievement unlocked\",\"text\":\"%s\",\"font_size\":22,\"priority\":30,"
                     "\"duration\":6000,\"show_time\":\"false\"}",
                     jsonText(text).c_str());
        std::fclose(f);
    }
    switchu::FileLog::log("[achievements] %s (%s)", m.name, game.c_str());
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
    for (const ach::Milestone* m : mine) notify(*m, game);
    for (const ach::Milestone* m : overall) notify(*m, std::string());
}

inline void threadFunc(void*) {
    const bool pdm = R_SUCCEEDED(pdmqryInitialize());
    std::uint64_t title = 0, base = 0, played = 0, last = armGetSystemTick();
    std::uint32_t launches = 0, session = ~0u;
    bool known = false;
    int ticks = 0;
    while (g_running.load()) {
        waitSingle(waiterForUEvent(&g_wake), 10'000'000'000ULL); // every 10 s (or to stop)
        if (!g_running.load()) break;
        const std::uint64_t now = armGetSystemTick();
        std::uint64_t dt = armTicksToNs(now - last) / 1'000'000'000ULL;
        last = now;
        if (dt > 20) dt = 10; // (the console slept meanwhile: that wasn't play)
        const std::uint64_t t = g_title.load();
        const std::uint32_t s = g_sessions.load();
        if (s != session || (t && t != title)) { // a new launch: pdm has the time before it
            session = s;
            title = t;
            played = 0;
            known = false;
        }
        if (!t || !pdm) continue; // (HOME or nothing in front: the time isn't play)
        bool check = false;
        if (!known) {
            PdmPlayStatistics st{};
            if (R_FAILED(pdmqryQueryPlayStatisticsByApplicationId(t, false, &st))) continue;
            base = st.playtime / 1'000'000'000ULL;
            launches = st.total_launches;
            known = check = true; // (a launch milestone: right away)
        } else {
            played += dt;
            check = ++ticks % 6 == 0; // the hours: once a minute is plenty
        }
        if (check) award(t, {base + played, launches ? launches : 1u}, played);
    }
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
