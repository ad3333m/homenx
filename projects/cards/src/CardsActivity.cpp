#include "CardsActivity.hpp"

#include "social/NotificationLog.hpp"

#include <nxui/Application.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/ThreadPool.hpp>
#include <nxui/third_party/stb/stb_image.h>
#include <switchu/find_controllers.hpp>
#include <nxui/widgets/Widget.hpp>
#include <switch.h>
#include "../../updater/src/Catalog.hpp"
#include "homebrew/HomebrewStore.hpp"
#include <nlohmann/json.hpp>
#include <switchu/control_cache.hpp>
#include <switchu/ns_ext.hpp>
#include <SDL2/SDL_mixer.h>

#ifdef SWITCHU_MENU
#include "core/FontChoice.hpp"
#include "news/NewsStore.hpp"
#include "smi_commands.hpp"
#include "themeshop/ThemeHttp.hpp"
#include <switchu/smi_protocol.hpp>
#include <set>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace cards {
namespace {

// The layout: the reference screenshot (2000x920) on 1280x720. Sizes follow
// its height (x0.78), positions its width (x0.64).
constexpr float kTopY = 47.f, kCircle = 77.f;
// Cards are the shape of a Switch case cover without its red band (352x499),
// so its whole picture fits them exactly.
constexpr float kCardW = 240.f, kCardH = 340.f, kCardGrowW = 10.f, kCardGrowH = 14.f;
constexpr float kCardMidY = 291.f, kCardPitch = 250.f, kCardFirstX = 136.f, kCardRadius = 26.f;
const nxui::Rect kPanel{186.f, 503.f, 907.f, 207.f};
constexpr float kPanelRadius = 35.f;
// The top bar: menu, Discord, sort and its label on the left; the pill (you,
// the time, battery, free space, Nextendo) ends at kPillRight; then the bell,
// music, power and settings.
constexpr float kMenuX = 134.f, kDiscordX = 219.f, kSortX = 304.f, kLabelX = 352.f, kPillRight = 872.f;
constexpr float kBellX = 908.f, kMusicX = 976.f, kPowerX = 1061.f, kGearX = 1146.f;
// Settings: sections on the left, rows on the right.
const nxui::Rect kSheet{120.f, 52.f, 1040.f, 616.f};
constexpr float kSecX = 142.f, kSecW = 236.f, kSecH = 54.f, kSheetTop = 146.f;
constexpr float kRowsX = 400.f, kRowsW = 736.f, kRowH = 72.f, kRowsBottom = 612.f;
// News: the list on the left, the article on the right.
constexpr float kNewsListW = 300.f, kNewsRowH = 72.f, kNewsTextX = 482.f, kNewsTextW = 640.f;

const nxui::Color kWhite(1.f, 1.f, 1.f, 1.f);
const nxui::Color kMuted(1.f, 1.f, 1.f, 0.82f);
const nxui::Color kGlassEdge(1.f, 1.f, 1.f, 0.20f);
const nxui::Color kGreen(0.204f, 0.78f, 0.349f, 1.f); // #34C759
const nxui::Color kRed(1.f, 0.231f, 0.188f, 1.f);     // #FF3B30

const char* const kPageNames[] = {"Games", "Homebrew", "Nintendo", "Media"};

// The Media page: the media apps Adam made, found by name; the ones with a
// public release can be downloaded from their card when they're not there.
struct MediaApp {
    const char* name;
    const char* keys[2]; // in the name or path, lower case
    const char* url;     // the .nro on GitHub (nullptr: shown only when it's on the SD card)
    const char* path;    // where it's put
};
const MediaApp kMediaApps[] = {
    {"SwitchTube", {"switchtube", "newpipe"}, nullptr, nullptr},
    {"TikTok", {"tiktok", nullptr}, "https://github.com/ad3333m/tiktok-nx/releases/download/latest/TikTok.nro",
     "sdmc:/switch/TikTok/TikTok.nro"},
    {"AnikkuNX", {"anikku", nullptr}, nullptr, nullptr},
    {"Cinejoy", {"cinejoy", nullptr}, nullptr, nullptr},
    {"ForYou", {"foryou", "for you"}, "https://github.com/ad3333m/switch-fyp/releases/download/v1.0.0/ForYou.nro",
     "sdmc:/switch/ForYou/ForYou.nro"},
};
const char* const kSortNames[] = {"Last Played", "Title", "Most Played", "My Order"};
constexpr int kSortCycle[] = {1, 3, 0, 2}; // Y: Title, My Order, Last Played, Most Played
struct Accent {
    const char* name;
    nxui::Color color;
};
const Accent kAccents[] = {
    {"White", nxui::Color(1.f, 1.f, 1.f, 1.f)},      {"Blue", nxui::Color(0.36f, 0.68f, 1.f, 1.f)},
    {"Green", nxui::Color(0.30f, 0.86f, 0.47f, 1.f)}, {"Pink", nxui::Color(1.f, 0.46f, 0.72f, 1.f)},
    {"Orange", nxui::Color(1.f, 0.63f, 0.26f, 1.f)},  {"Purple", nxui::Color(0.72f, 0.55f, 1.f, 1.f)},
    {"Gold", nxui::Color(1.f, 0.84f, 0.36f, 1.f)},
};
constexpr int kAccentCount = sizeof(kAccents) / sizeof(kAccents[0]);
const char* const kGlassNames[] = {"Clear", "Frosted", "Dark"};
const char* const kLaunchNames[] = {"Card jump", "Splash screen", "Card zoom", "Card flip", "Dive in", "Off"};
// Each style's length against the zoom's (callers give the zoom's seconds).
constexpr float kLaunchPace[] = {0.85f, 1.1f, 1.f, 0.95f, 0.8f, 0.15f};

constexpr const char* kBrowseNxUrl = "https://github.com/ad3333m/browsenx/releases/download/latest/BrowseNX.nro";
constexpr const char* kBrowseNxPath = "sdmc:/switch/BrowseNX/BrowseNX.nro";
constexpr const char* kToggle = "sdmc:/switch/NXToggle/NXToggle.nro";

class Painter final : public nxui::Widget {
public:
    explicit Painter(std::function<void(nxui::Renderer&)> fn) : m_fn(std::move(fn)) { setRect({0.f, 0.f, 1280.f, 720.f}); }

protected:
    void onRender(nxui::Renderer& r) override {
        if (m_fn) m_fn(r);
    }

private:
    std::function<void(nxui::Renderer&)> m_fn;
};

float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
float easeOut(float t) { t = clamp01(t); return 1.f - (1.f - t) * (1.f - t) * (1.f - t); }
float easeIn(float t) { t = clamp01(t); return t * t * t; }
float easeInOut(float t) {
    t = clamp01(t);
    return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) * 0.5f;
}
float smoothstep(float a, float b, float x) {
    const float t = clamp01((x - a) / (b - a));
    return t * t * (3.f - 2.f * t);
}
float easeOutBack(float t) { // a small overshoot, then settles
    t = clamp01(t);
    const float c1 = 1.4f, c3 = c1 + 1.f;
    return 1.f + c3 * std::pow(t - 1.f, 3.f) + c1 * std::pow(t - 1.f, 2.f);
}
nxui::Rect lerpRect(const nxui::Rect& a, const nxui::Rect& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.width + (b.width - a.width) * t,
            a.height + (b.height - a.height) * t};
}

// Text sizes, measured once: measuring is shaping the text again, and the
// Games page measured every long name letter by letter every frame (it lagged).
nxui::Vec2 measured(nxui::Font& font, const std::string& s) {
    static std::unordered_map<std::string, nxui::Vec2> cache;
    std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(&font)) + '|' + s;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 6000) cache.clear();
    return cache[key] = font.measure(s);
}

// As much of `text` as fits in maxW (cut between letters, "..." added), remembered.
std::string fit(nxui::Font& font, const std::string& text, float maxW, float scale = 1.f) {
    static std::unordered_map<std::string, std::string> cache;
    std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(&font)) + '|' + std::to_string(int(maxW)) + '|'
                    + std::to_string(int(scale * 100.f)) + '|' + text;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 3000) cache.clear();
    if (measured(font, text).x * scale <= maxW) return cache[key] = text;
    std::vector<size_t> cuts; // where letters start
    for (size_t i = 1; i < text.size(); ++i)
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) cuts.push_back(i);
    size_t lo = 0, hi = cuts.size(); // the longest start that fits
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (measured(font, text.substr(0, cuts[mid - 1]) + "...").x * scale <= maxW) lo = mid;
        else hi = mid - 1;
    }
    return cache[key] = (lo == 0 ? std::string() : text.substr(0, cuts[lo - 1])) + "...";
}

// The scale (down to minScale) that fits `text` in maxW: long names shrink a
// little before they're cut.
float fitScale(nxui::Font& font, const std::string& text, float maxW, float minScale = 0.78f) {
    const float w = measured(font, text).x;
    return w <= maxW ? 1.f : std::max(minScale, maxW / w);
}

// Text drawn with its middle at y.
void textMid(nxui::Renderer& r, nxui::Font& font, const std::string& s, float x, float y, const nxui::Color& c,
             float scale = 1.f) {
    r.drawText(s, {x, y - measured(font, s.empty() ? std::string("A") : s).y * scale * 0.5f}, &font, c, scale);
}

// Discord's emoji marks (4-byte letters past U+F0000) aren't letters here.
std::string plain(std::string s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (static_cast<unsigned char>(s[i]) >= 0xF3) s.erase(i--, 4);
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string playTime(std::uint64_t seconds) {
    const std::uint64_t m = seconds / 60, h = m / 60;
    if (m == 0) return seconds ? "<1m" : "0m";
    if (h == 0) return std::to_string(m) + "m";
    if (h >= 100 || m % 60 == 0) return std::to_string(h) + "h";
    return std::to_string(h) + "h " + std::to_string(m % 60) + "m";
}

std::string lastPlayed(std::uint64_t at) {
    if (at == 0) return "Never played";
    const std::uint64_t now = static_cast<std::uint64_t>(std::time(nullptr));
    const std::uint64_t s = now > at ? now - at : 0;
    auto unit = [](std::uint64_t n, const char* one) {
        return std::to_string(n) + " " + one + (n == 1 ? "" : "s") + " ago";
    };
    if (s < 60) return "Last played just now";
    if (s < 3600) return "Last played " + unit(s / 60, "minute");
    if (s < 86400) return "Last played " + unit(s / 3600, "hour");
    if (s < 30 * 86400) return "Last played " + unit(s / 86400, "day");
    TimeCalendarTime t{};
    TimeCalendarAdditionalInfo info{};
    if (R_FAILED(timeToCalendarTimeWithMyRule(at, &t, &info))) return "Last played a while ago";
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return "Last played " + std::to_string(t.day) + " " + kMonths[(t.month + 11) % 12] + " " + std::to_string(t.year);
}

// Halves a picture until it's at most maxW wide (a softer, cheaper texture).
void shrink(std::vector<std::uint8_t>& rgba, int& w, int& h, int maxW) {
    while (w > maxW && w >= 2 && h >= 2) {
        const int nw = w / 2, nh = h / 2;
        std::vector<std::uint8_t> out(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    const auto at = [&](int xx, int yy) { return rgba[(static_cast<size_t>(yy) * w + xx) * 4 + c]; };
                    out[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<std::uint8_t>(
                        (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1)) / 4);
                }
        rgba.swap(out);
        w = nw;
        h = nh;
    }
}

// A picture drawn to fill `dst`, cropped to its shape. trimTop / trimBottom:
// fractions left out first; zoom / panX / panY: a slow drift (the background).
void drawCover(nxui::Renderer& r, const nxui::Texture* tex, const nxui::Rect& dst, float radius, const nxui::Color& tint,
               float trimTop = 0.f, float trimBottom = 0.f, float zoom = 1.f, float panX = 0.f, float panY = 0.f) {
    const float tw = static_cast<float>(tex->width());
    const float top = static_cast<float>(tex->height()) * trimTop;
    const float th = static_cast<float>(tex->height()) * (1.f - trimTop - trimBottom);
    const float scale = std::max(dst.width / tw, dst.height / th) * zoom;
    const float sw = dst.width / scale, sh = dst.height / scale;
    const nxui::Rect src{(tw - sw) * (0.5f + panX), top + (th - sh) * (0.5f + panY), sw, sh};
    if (radius > 0.f) r.drawTextureSubRounded(tex, src, dst, radius, tint);
    else r.drawTextureSub(tex, src, dst, tint);
}

nxui::Rect scaled(const nxui::Rect& r, float s) {
    const float w = r.width * s, h = r.height * s;
    return {r.x + (r.width - w) * 0.5f, r.y + (r.height - h) * 0.5f, w, h};
}

// Text broken into lines of at most maxW (at spaces; newlines kept).
std::vector<std::string> wrap(nxui::Font& font, const std::string& text, float maxW) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t end = text.find('\n', start);
        const std::string para = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        std::string line;
        for (size_t i = 0; i <= para.size();) {
            size_t sp = para.find(' ', i);
            if (sp == std::string::npos) sp = para.size();
            const std::string word = para.substr(i, sp - i);
            const std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && font.measure(trial).x > maxW) {
                out.push_back(line);
                line = word;
            } else {
                line = trial;
            }
            i = sp + 1;
        }
        out.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

float ticksMs(std::uint64_t ticks) { return static_cast<float>(armTicksToNs(ticks)) / 1e6f; }

// A background job's thread: off the screen's core and below it (std::async
// threads start on core 0 at the screen's own priority, and took its frames).
void offScreenCore() {
    nxui::moveToWorkerCores();
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
}

const MediaApp* mediaAppOf(const std::string& name, const std::string& path) {
    std::string n = name, p = path;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const MediaApp& m : kMediaApps)
        for (const char* k : m.keys)
            if (k && (n.find(k) != std::string::npos || p.find(k) != std::string::npos)) return &m;
    return nullptr;
}

bool parseUid(const std::string& hex, AccountUidLike& out) {
    if (hex.size() != 32) return false;
    out.uid[0] = std::strtoull(hex.substr(0, 16).c_str(), nullptr, 16);
    out.uid[1] = std::strtoull(hex.substr(16).c_str(), nullptr, 16);
    return out.uid[0] || out.uid[1];
}

// Popup sizes: rows shrink a little when there are many (users).
void popupGeometry(size_t rows, nxui::Rect& box, float& rowH) {
    const float w = 600.f;
    rowH = std::min(56.f, (650.f - 84.f) / static_cast<float>(std::max<size_t>(1, rows)));
    const float h = 84.f + rowH * static_cast<float>(rows);
    box = {(1280.f - w) * 0.5f, (720.f - h) * 0.5f, w, h};
}

// Quick Settings: the panel on the right and where its parts sit in it.
constexpr float kQuickW = 460.f, kQuickH = 700.f, kQuickY = 10.f, kQuickPad = 18.f;
constexpr float kQStatusY = 58.f, kQStatusH = 58.f, kQPlayerY = 126.f, kQPlayerH = 150.f;
constexpr float kQSliderY[3] = {286.f, 346.f, 406.f}, kQSliderH = 54.f;
constexpr float kQToggleY[2] = {466.f, 522.f}, kQToggleH = 48.f, kQPowerY = 580.f, kQPowerH = 100.f;
constexpr int kQPlayerButtons = 6, kQPowerButtons = 4;

nxui::Rect quickPanel(float slide) { return {1280.f - 20.f - kQuickW + slide, kQuickY, kQuickW, kQuickH}; }
float quickInnerW() { return kQuickW - kQuickPad * 2.f; }
// The player's buttons: prev, play / pause, next, repeat, shuffle, playlist.
nxui::Vec2 quickPlayerButton(const nxui::Rect& p, int i) {
    const float step = quickInnerW() / kQPlayerButtons;
    return {p.x + kQuickPad + step * (static_cast<float>(i) + 0.5f), p.y + kQPlayerY + 84.f};
}
nxui::Rect quickPowerButton(const nxui::Rect& p, int i) {
    const float w = (quickInnerW() - 8.f * (kQPowerButtons - 1)) / kQPowerButtons;
    return {p.x + kQuickPad + (w + 8.f) * static_cast<float>(i), p.y + kQPowerY, w, kQPowerH};
}
nxui::Rect quickPlaylist(const nxui::Rect& p, size_t tracks) {
    const float h = std::min(540.f, 64.f + 46.f * static_cast<float>(std::max<size_t>(1, tracks)));
    return {p.x - 312.f, p.y + kQPlayerY, 300.f, h};
}
std::string clockText(float seconds) {
    const int s = std::max(0, static_cast<int>(seconds));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}

#ifdef SWITCHU_MENU
constexpr const char* kNewsReadPath = "sdmc:/config/SwitchU/news_read.txt"; // shared with HomeNX's HOME
#endif

} // namespace

// ---------------------------------------------------------------- rows

CardsActivity::MenuItem CardsActivity::action(std::string label, std::function<void()> run, bool danger) {
    MenuItem m;
    m.kind = MenuItem::Action;
    m.label = std::move(label);
    m.run = std::move(run);
    m.danger = danger;
    return m;
}

CardsActivity::MenuItem CardsActivity::toggle(std::string label, std::function<bool()> get, std::function<void()> flip,
                                              std::string detail) {
    MenuItem m;
    m.kind = MenuItem::Toggle;
    m.label = std::move(label);
    m.detail = std::move(detail);
    m.level = [get]() { return get() ? 1.f : 0.f; };
    m.change = [flip](int) { flip(); };
    return m;
}

CardsActivity::MenuItem CardsActivity::choice(std::string label, std::function<std::string()> value,
                                              std::function<void(int)> change, std::string detail) {
    MenuItem m;
    m.kind = MenuItem::Choice;
    m.label = std::move(label);
    m.detail = std::move(detail);
    m.value = std::move(value);
    m.change = std::move(change);
    return m;
}

CardsActivity::MenuItem CardsActivity::slider(std::string label, std::function<int()> get, std::function<void(int)> set,
                                              std::string detail) {
    MenuItem m;
    m.kind = MenuItem::Slider;
    m.label = std::move(label);
    m.detail = std::move(detail);
    m.level = [get]() { return static_cast<float>(get()) / 100.f; };
    m.value = [get]() { return std::to_string(get()) + "%"; };
    m.change = [get, set](int d) { set(std::clamp(get() + d * 10, 0, 100)); };
    return m;
}

CardsActivity::MenuItem CardsActivity::info(std::string label, std::function<std::string()> value) {
    MenuItem m;
    m.kind = MenuItem::Info;
    m.label = std::move(label);
    m.value = std::move(value);
    return m;
}

// ---------------------------------------------------------------- set up

CardsActivity::CardsActivity(std::uint64_t suspendedId, bool appRunning, bool atBoot) {
#ifdef SWITCHU_MENU
    m_startSuspended = suspendedId;
    m_startRunning = appRunning;
    m_atBoot = atBoot;
#else
    (void)suspendedId;
    (void)appRunning;
    (void)atBoot;
#endif
}

bool CardsActivity::onCreate() {
    m_perfCreateTick = armGetSystemTick();
    auto& gpu = app().gpu();
    auto& ren = app().renderer();
    ren.setBoxWireframeEnabled(false);
    app().setIdleThrottle(false); // it keeps moving: always 60 Hz

#ifdef SWITCHU_MENU
    pdmqryInitialize(); // play time (HOME's own menu reads it through am)
    nifmInitialize(NifmServiceType_User);
    m_launcher.init({
        [this]() { m_audio.play("hide_modal"); },
        [this]() { app().requestExit(); },
        [this]() { // before sleep / restart / power off: everything on the SD card
            m_settings.save();
            return R_SUCCEEDED(fsdevCommitDevice("sdmc"));
        },
    });
    m_launcher.setStartupStatus(m_startSuspended, m_startRunning);
#endif

    // More room for pictures than HOME has, when this process has it.
    u64 total = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    nxui::GpuDevice::setImageBudget(std::clamp<u64>(total / 4, 64ull << 20, 160ull << 20));

    m_settings.load();
    m_boxArt = m_settings.boxArt;
    m_titleArt = m_settings.titlePics;
    m_clock24 = m_settings.clock24;
    if (m_settings.perfLog) { // a new part of the log (the last one kept as perf.old.log once it's big)
        std::error_code ec;
        if (std::filesystem::file_size("sdmc:/config/CardsNX/perf.log", ec) > (512u << 10) && !ec) {
            std::filesystem::remove("sdmc:/config/CardsNX/perf.old.log", ec);
            std::filesystem::rename("sdmc:/config/CardsNX/perf.log", "sdmc:/config/CardsNX/perf.old.log", ec);
        }
        char head[96];
        std::snprintf(head, sizeof(head), "=== CardsNX %s %s ===", HOMENX_BUILD,
#ifdef SWITCHU_MENU
                      m_atBoot ? "(HOME, console start)" : "(HOME, back from a game or applet)"
#else
                      "(app)"
#endif
        );
        perfLine(head);
    }
    const std::uint64_t tFonts = armGetSystemTick();

    // Names in other scripts (Japanese, Chinese, Korean) from the console's own fonts.
    if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
        for (PlSharedFontType type : {PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified,
                                      PlSharedFontType_ChineseTraditional, PlSharedFontType_KO,
                                      PlSharedFontType_NintendoExt}) {
            PlFontData font{};
            if (R_SUCCEEDED(plGetSharedFontByType(&font, type))) nxui::Font::addFallback(font.address, font.size);
        }
    }
    const std::string fonts = std::string(kAssets) + "fonts/";
    const std::string f5 = fonts + "Inter-500.ttf", f6 = fonts + "Inter-600.ttf", f7 = fonts + "Inter-700.ttf";
    if (!m_fTitle.load(gpu, ren, f7, 30) || !m_fPlay.load(gpu, ren, f7, 24) || !m_fLabel.load(gpu, ren, f6, 22)
        || !m_fPill.load(gpu, ren, f6, 19) || !m_fBody.load(gpu, ren, f5, 17) || !m_fBadge.load(gpu, ren, f7, 15)
        || !m_fCount.load(gpu, ren, f7, 21))
        return false;
    const std::uint64_t tIcons = armGetSystemTick();
    for (const char* name : {"dots", "sort", "friends", "bell", "gear", "thermo", "tag", "play", "copy", "home",
                             "power", "music", "news", "discord", "arrows", "wifi", "airplane", "sun", "speaker",
                             "prev", "next", "pause", "repeat", "shuffle", "playlist", "moon", "restart", "hekate",
                             "download", "trash", "pad"}) {
        auto tex = std::make_unique<nxui::Texture>();
        nxui::GpuDevice::EssentialScope essential; // the buttons always draw
        if (tex->loadFromFile(gpu, ren, std::string(kAssets) + "icons/" + name + ".png", 96))
            m_icons[name] = std::move(tex);
    }

    // The pages: games; homebrew (.nro apps, then NRO forwarders); Nintendo.
    // Last time's lists show at once. Reading every game and .nro takes
    // seconds, so that happens only at console start, when the console says
    // the games changed, or when the lists are half an hour old (on a thread).
    const std::uint64_t tLists = armGetSystemTick();
    const long long nowSec = static_cast<long long>(std::time(nullptr));
#ifdef SWITCHU_MENU
    const bool atBoot = m_atBoot;
#else
    const bool atBoot = false; // the app: only when they're old
#endif
    const bool stale = atBoot || nowSec - m_settings.listsAt > 30 * 60 || nowSec < m_settings.listsAt;
    bool scanned = false;
    if (loadLists(m_pages[kGames], m_pages[kHomebrew])) {
        if (stale) startListRefresh();
    } else { // the very first time: read them now
        Lists l = readAllLists();
        m_pages[kGames] = std::move(l.games);
        m_pages[kHomebrew] = std::move(l.homebrew);
        saveLists(m_pages[kGames], homebrewToSave());
        m_settings.listsAt = nowSec;
        scanned = true;
    }
    splitMedia();
    m_pages[kNintendo] = listSystem();
    applyOrder(kHomebrew);
    applyOrder(kNintendo);

    // You: the one chosen in the pill, else the last account that played, else the first.
    AccountUidLike chosen;
    AccountUid last{};
    if (parseUid(m_settings.user, chosen)) { // still on the console?
        AccountUid uids[8] = {};
        s32 count = 0;
        bool exists = false;
        if (R_SUCCEEDED(accountListAllUsers(uids, 8, &count)))
            for (s32 i = 0; i < count; ++i)
                exists |= uids[i].uid[0] == chosen.uid[0] && uids[i].uid[1] == chosen.uid[1];
        if (!exists) chosen = AccountUidLike{};
    }
    if (!chosen.uid[0] && !chosen.uid[1] && R_SUCCEEDED(accountGetLastOpenedUser(&last)) && accountUidIsValid(&last)) {
        chosen.uid[0] = last.uid[0];
        chosen.uid[1] = last.uid[1];
    }
    if (!chosen.uid[0] && !chosen.uid[1]) {
        AccountUid uids[8] = {};
        s32 count = 0;
        if (R_SUCCEEDED(accountListAllUsers(uids, 8, &count)) && count > 0) {
            chosen.uid[0] = uids[0].uid[0];
            chosen.uid[1] = uids[0].uid[1];
        }
    }
    m_user = chosen;
    const std::uint64_t tRest = armGetSystemTick();

    m_unread = notifications::unreadCount();
    m_key = steamGridDbKey();

    m_painter = std::make_shared<Painter>([this](nxui::Renderer& r) { draw(r); });
    rootBox().addChild(m_painter);

#ifdef SWITCHU_MENU
    if (m_startSuspended || m_startRunning) m_introFor = 0.6f; // back from a game: quicker
#endif

    // Always the first card on the list (the sort is Title or My Order:
    // never "last played" on the way in).
    m_page = m_settings.pageOrder[0]; // the first page in your order
    sortGames(m_settings.sort == 3 ? 3 : 1, false);
    for (int& s : m_sels) s = 0;
    m_scroll = 0.f;
#ifdef SWITCHU_MENU
    // Back from a game (not at boot): the game you were playing is chosen.
    if (!m_atBoot) {
        // The game open now, else the last thing started from here. An .nro runs in
        // a game's place (its host), so when the last thing was an .nro, it's the .nro.
        const std::uint64_t last = std::strtoull(m_settings.lastGame.c_str(), nullptr, 16);
        const bool lastWasNro = (last >> 56) == 0xFF;
        m_backTo = m_startSuspended && !lastWasNro ? m_startSuspended : last;
        if (m_backTo) selectById(m_backTo);
        // Its play time now (the rest of the lists stay as they were).
        for (auto& page : m_pages)
            for (Game& g : page)
                if (m_backTo && g.id == m_backTo && g.path.empty() && g.action.empty()) {
                    PdmPlayStatistics st{};
                    if (R_SUCCEEDED(pdmqryQueryPlayStatisticsByApplicationId(g.id, false, &st))) {
                        g.lastPlayed = st.last_timestamp_user;
                        g.playSeconds = st.playtime / 1000000000ULL;
                        g.launches = st.total_launches;
                    }
                }
    }
#endif
    for (int i = 0; i < 5; ++i) m_threads.emplace_back([this, i]() { worker(i); });
    wantArt();
    m_intro = m_settings.motion ? 0.f : 1.f;
    const std::uint64_t tEnd = armGetSystemTick();
    char line[200];
    std::snprintf(line, sizeof(line),
                  "start: setup %.0f ms, fonts %.0f, icons %.0f, lists %.0f (%s), the rest %.0f; %.0f ms in all",
                  ticksMs(tFonts - m_perfCreateTick), ticksMs(tIcons - tFonts), ticksMs(tLists - tIcons),
                  ticksMs(tRest - tLists), scanned ? "read now" : stale ? "saved; reading on a thread" : "saved",
                  ticksMs(tEnd - tRest), ticksMs(tEnd - m_perfCreateTick));
    perfLine(line);
    return true;
}

CardsActivity::Lists CardsActivity::readAllLists() {
    const std::uint64_t t0 = armGetSystemTick();
    Lists l;
    for (Game& g : listGames()) (g.homebrew ? l.homebrew : l.games).push_back(std::move(g));
    std::vector<Game> nros = listHomebrew();
    l.homebrew.insert(l.homebrew.begin(), std::make_move_iterator(nros.begin()), std::make_move_iterator(nros.end()));
    for (size_t i = 0; i < l.homebrew.size(); ++i) l.homebrew[i].order = static_cast<int>(i);
    l.ms = ticksMs(armGetSystemTick() - t0);
    return l;
}

void CardsActivity::startListRefresh() {
    if (m_listJob.valid()) return;
    m_listJob = std::async(std::launch::async, []() {
        offScreenCore(); // this thread only (never the screen's)
        return readAllLists();
    });
}

float CardsActivity::sinceStartMs() const { return ticksMs(armGetSystemTick() - m_perfCreateTick); }

void CardsActivity::perfLine(const std::string& line) {
    if (!m_settings.perfLog) return;
    char head[24];
    std::snprintf(head, sizeof(head), "[%8.1f s] ", sinceStartMs() / 1000.f);
    std::lock_guard<std::mutex> lk(m_mu);
    m_perfOut += head + line + "\n";
}

void CardsActivity::onDestroy() {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_quit = true;
        m_jobs.clear();
    }
    m_cv.notify_all();
    for (std::thread& t : m_threads)
        if (t.joinable()) t.join();
    if (m_browserJob.valid()) m_browserJob.wait();
    if (m_getJob.valid()) m_getJob.wait();
    if (m_listJob.valid()) m_listJob.wait();
    if (m_updatesJob.valid()) m_updatesJob.wait();
    if (m_nroUpdateJob.valid()) m_nroUpdateJob.wait();
    if (!m_perfOut.empty()) {
        std::error_code ec;
        std::filesystem::create_directories("sdmc:/config/CardsNX", ec);
        std::ofstream("sdmc:/config/CardsNX/perf.log", std::ios::app) << m_perfOut;
    }
    if (m_deleteJob.valid()) m_deleteJob.wait();
    if (m_netTest) m_netTest->stop = true;
    if (m_netTestJob.valid()) m_netTestJob.wait();
    if (m_findJob.valid()) m_findJob.wait();
#ifdef SWITCHU_MENU
    stopPairing();
    if (m_newsJob.valid()) m_newsJob.wait();
    if (m_updateJob.valid()) m_updateJob.wait();
    // Discord's threads use the sockets: they stop before the sockets go.
    m_discord.shutdown();
    if (!m_discord.lingering()) themeshop::http::shutdown();
    switchu::menu::smi_cmd::menuClosing();
    nifmExit();
    pdmqryExit();
#endif
    if (m_netUp) stopNetwork();
    m_settings.save();
    m_audio.stop();
}

nxui::Widget* CardsActivity::focusRoot() {
#ifdef SWITCHU_MENU
    if (m_textEntry && m_textEntry->isActive()) return m_textEntry.get();
    if (m_discordPanel && m_discordPanel->isActive()) return m_discordPanel.get();
    if (m_social && m_social->isActive()) return m_social.get();
    if (m_padTest && m_padTest->isActive()) return m_padTest.get();
#endif
    return nullptr;
}

const Game* CardsActivity::current() {
    auto& g = games();
    return g.empty() ? nullptr : &g[(size_t)std::clamp(sel(), 0, static_cast<int>(g.size()) - 1)];
}

nxui::Color CardsActivity::accent(float a) const {
    return kAccents[std::clamp(m_settings.accent, 0, kAccentCount - 1)].color.withAlpha(a);
}

nxui::Color CardsActivity::glassTint(float a) const {
    switch (m_settings.glass) {
        case 0: return nxui::Color(0.20f, 0.23f, 0.30f, 0.16f * a);
        case 2: return nxui::Color(0.05f, 0.06f, 0.09f, 0.55f * a);
        default: return nxui::Color(0.17f, 0.20f, 0.27f, 0.30f * a);
    }
}

std::uint64_t CardsActivity::runningId() const {
#ifdef SWITCHU_MENU
    return m_launcher.suspendedTitleId();
#else
    return 0;
#endif
}

bool CardsActivity::isRunning(std::uint64_t id) const {
    return id != 0 && id == runningId();
}

void CardsActivity::loadAvatar() {
    AccountUid uid{};
    uid.uid[0] = m_user.uid[0];
    uid.uid[1] = m_user.uid[1];
    if (m_avatar) app().gpu().waitIdle(); // the old one may still be drawing
    m_avatar.reset();
    AccountProfile profile{};
    if (!accountUidIsValid(&uid) || R_FAILED(accountGetProfile(&profile, uid))) return;
    u32 size = 0;
    if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &size)) && size > 0) {
        std::vector<std::uint8_t> jpeg(size);
        u32 got = 0;
        if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpeg.data(), size, &got)) && got > 0) {
            m_avatar = std::make_unique<nxui::Texture>();
            if (!m_avatar->loadFromMemory(app().gpu(), app().renderer(), jpeg.data(), got, 96)) m_avatar.reset();
        }
    }
    accountProfileClose(&profile);
}

void CardsActivity::setUser(AccountUidLike uid) {
    m_user = uid;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%016llX%016llX", static_cast<unsigned long long>(uid.uid[0]),
                  static_cast<unsigned long long>(uid.uid[1]));
    m_settings.user = buf;
    m_settings.save();
    loadAvatar();
}

// ---------------------------------------------------------------- the workers

void CardsActivity::worker(int index) {
    // Off the screen's core and below it: decoding a picture used to take the
    // frames' time (the Games page stuttered).
    nxui::moveToWorkerCores();
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
    // The network starts here, not on the screen's thread; the others wait for it
    // (a picture fetched before it's up fails and is asked for again).
    if (index == 0) {
        const std::uint64_t t0 = armGetSystemTick();
        m_netUp = startNetwork();
        m_netReady = true;
        char line[64];
        std::snprintf(line, sizeof(line), "network %s in %.0f ms", m_netUp ? "up" : "not up", ticksMs(armGetSystemTick() - t0));
        perfLine(line);
    } else {
        while (!m_netReady && !m_quit) svcSleepThread(5'000'000ULL);
    }
    std::uint64_t countsAt = 0, statusAt = 0, logAt = 0;
    for (;;) {
        // The bar's numbers, once a second: the time, battery, network, the SD
        // card's free space (that one waited on the card on the screen's thread).
        const std::uint64_t now = armTicksToNs(armGetSystemTick()) / 1000000ULL;
        if (index == 1 && now >= logAt) { // the performance log, a few seconds at a time
            logAt = now + 5000;
            std::string out;
            {
                std::lock_guard<std::mutex> lk(m_mu);
                out.swap(m_perfOut);
            }
            if (!out.empty()) {
                std::error_code ec;
                std::filesystem::create_directories("sdmc:/config/CardsNX", ec);
                std::ofstream("sdmc:/config/CardsNX/perf.log", std::ios::app) << out;
            }
        }
        if (index == 1 && now >= statusAt) {
            statusAt = now + 1000;
            Status s;
            TimeCalendarTime t{};
            TimeCalendarAdditionalInfo info{};
            u64 posix = 0;
            if (R_SUCCEEDED(timeGetCurrentTime(TimeType_Default, &posix))
                && R_SUCCEEDED(timeToCalendarTimeWithMyRule(posix, &t, &info))) {
                char buf[16];
                if (m_clock24) std::snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.minute);
                else std::snprintf(buf, sizeof(buf), "%d:%02d %s", (t.hour + 11) % 12 + 1, t.minute, t.hour < 12 ? "AM" : "PM");
                s.clock = buf;
            }
            u32 charge = 0;
            if (R_SUCCEEDED(psmGetBatteryChargePercentage(&charge))) s.battery = std::to_string(charge) + "%";
            NifmInternetConnectionType type{};
            u32 strength = 0;
            NifmInternetConnectionStatus status{};
            s.online = R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status))
                    && status == NifmInternetConnectionStatus_Connected;
            if (FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc")) {
                s64 free = 0;
                if (R_SUCCEEDED(fsFsGetFreeSpace(fs, "/", &free))) {
                    char buf[32];
                    const double gb = static_cast<double>(free) / 1e9;
                    std::snprintf(buf, sizeof(buf), gb >= 100.0 ? "%.0f GB" : "%.1f GB", gb);
                    s.free = buf;
                }
            }
            std::lock_guard<std::mutex> lk(m_mu);
            m_statusIn = std::move(s);
            m_statusFresh = true;
        }

        Job job;
        bool haveJob = false;
        {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait_for(lk, std::chrono::milliseconds(index == 1 ? 500 : 1000),
                          [this]() { return m_quit || !m_jobs.empty(); });
            if (m_quit) break;
            if (!m_jobs.empty()) {
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
                haveJob = true;
            }
        }
        if (!haveJob) { // pictures first; Nextendo's players every minute
            if (index == 0 && m_netUp && now >= countsAt) {
                countsAt = now + 60000;
                std::map<std::uint64_t, int> counts;
                if (nextendoOnline(counts)) {
                    std::lock_guard<std::mutex> lk(m_mu);
                    m_playersIn = std::move(counts);
                    m_playersFresh = true;
                }
            }
            continue;
        }
        Done done;
        done.id = job.game.id;
        done.what = job.what;
        const std::uint64_t jobStart = armGetSystemTick();
        std::vector<std::uint8_t> data;
        if (job.what == kBackground || job.what == kLaunch) {
            data = backgroundArt(job.game, m_key, done.kind);
        } else if (job.what == kCard) {
            data = cardArt(job.game, m_key, m_boxArt.load(), done.kind);
        } else if (!job.game.action.empty()) {
            std::ifstream in(systemIconPath(job.game.action), std::ios::binary);
            data.assign(std::istreambuf_iterator<char>(in), {});
        } else {
            done.kind = ArtKind::Title;
            if (m_titleArt && !job.game.homebrew) data = titleArt(job.game, m_key);
            if (data.empty()) {
                done.kind = ArtKind::Icon;
                data = gameIcon(job.game);
            }
        }
        if (!data.empty()) {
            int w = 0, h = 0, ch = 0;
            stbi_uc* px = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &ch, 4);
            // SteamGridDB's notice for a picture taken down (DMCA), saved before:
            // its next picture instead (once a session, as an eShop banner can be plain too).
            static thread_local std::unordered_set<std::uint64_t> rechecked;
            const bool fromSgdb = !m_key.empty() && ((job.what == kCard && done.kind == ArtKind::Grid)
                                  || ((job.what == kBackground || job.what == kLaunch) && done.kind == ArtKind::Banner));
            if (px && fromSgdb && removedNotice(px, w, h) && rechecked.insert(job.game.id ^ (job.what == kCard)).second) {
                stbi_image_free(px);
                forgetPicture(job.game.id, job.what != kCard);
                data = job.what == kCard ? cardArt(job.game, m_key, m_boxArt.load(), done.kind)
                                         : backgroundArt(job.game, m_key, done.kind);
                px = data.empty() ? nullptr
                                  : stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &ch, 4);
            }
            if (px) {
                done.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
                stbi_image_free(px);
                // Backgrounds sharp (a box cover softer, an icon a wash of its colours); cards as drawn.
                const int maxW = job.what == kLaunch ? 1280
                               : job.what == kIcon ? 160
                               : job.what == kCard ? 400
                               : done.kind == ArtKind::Icon ? 32
                               : done.kind == ArtKind::Box ? 400
                                                           : 960;
                shrink(done.rgba, w, h, maxW);
                done.w = w;
                done.h = h;
            }
        }
        const float took = ticksMs(armGetSystemTick() - jobStart);
        if (took > 800.f) { // a picture that took long (the internet, mostly)
            char line[160];
            std::snprintf(line, sizeof(line), "%s for %.60s took %.0f ms%s",
                          job.what == kBackground ? "background" : job.what == kIcon ? "icon" : "card", job.game.name.c_str(),
                          took, done.rgba.empty() ? " (nothing came)" : "");
            perfLine(line);
        }
        std::lock_guard<std::mutex> lk(m_mu);
        m_done.push_back(std::move(done));
    }
    threadDone();
}

void CardsActivity::wantArt() {
    auto& page = games();
    std::vector<Job> jobs;
    auto add = [&](int i, int what) {
        if (i < 0 || i >= static_cast<int>(page.size())) return;
        Art& a = m_art[page[(size_t)i].id];
        bool& asked = what == kBackground ? a.bgAsked : what == kIcon ? a.iconAsked : a.cardAsked;
        if (asked || m_time < a.retryAt) return;
        if (what == kBackground && page[(size_t)i].action == "get") return; // a download glyph isn't a background
        asked = true;
        jobs.push_back({page[(size_t)i], what});
    };
    const int s = sel();
    add(s, kIcon);
    add(s, kCard);
    add(s, kBackground);
    for (int d = 1; d <= 6; ++d) {
        add(s + d, kCard);
        add(s - d, kCard);
        if (d <= 2) {
            add(s + d, kIcon);
            add(s - d, kIcon);
        }
        if (d == 1) {
            add(s + 1, kBackground);
            add(s - 1, kBackground);
        }
    }
    for (int d = 7; d <= 12; ++d) { // further ahead, both ways (scrolling fast)
        add(s + d, kCard);
        if (d <= 9) add(s - d, kCard);
    }
    if (jobs.empty()) return;
    {
        std::lock_guard<std::mutex> lk(m_mu); // what's near the selection now goes first
        for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) m_jobs.push_front(std::move(*it));
    }
    m_cv.notify_all();
}

void CardsActivity::takeArt() {
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        const size_t n = std::min<size_t>(3, m_done.size()); // a few a frame (uploads don't wait any more)
        for (size_t i = 0; i < n; ++i) done.push_back(std::move(m_done[i]));
        m_done.erase(m_done.begin(), m_done.begin() + static_cast<std::ptrdiff_t>(n));
        if (m_playersFresh) {
            m_players = m_playersIn;
            m_playersFresh = false;
            m_playersTotal = 0;
            for (const auto& p : m_players) m_playersTotal += p.second;
        }
        if (m_statusFresh) {
            m_statusFresh = false;
            if (!m_statusIn.clock.empty()) m_clock = m_statusIn.clock;
            if (!m_statusIn.battery.empty()) m_battery = m_statusIn.battery;
            if (!m_statusIn.free.empty()) m_free = m_statusIn.free;
            m_online = m_statusIn.online;
        }
    }
    const Game* cur = current();
    const std::uint64_t selected = cur ? cur->id : 0;
    const std::uint64_t until = armGetSystemTick() + armNsToTicks(4'000'000ULL);
    for (Done& d : done) {
        if (armGetSystemTick() > until) { // the rest next frame
            std::lock_guard<std::mutex> lk(m_mu);
            m_done.insert(m_done.begin(), std::make_move_iterator(&d), std::make_move_iterator(done.data() + done.size()));
            break;
        }
        auto it = m_art.find(d.id);
        if (it == m_art.end()) continue;
        bool* asked = d.what == kBackground ? &it->second.bgAsked
                    : d.what == kIcon       ? &it->second.iconAsked
                    : d.what == kCard       ? &it->second.cardAsked
                                            : nullptr;
        if (d.rgba.empty()) { // nothing came (the network not up yet?): asked again in a while
            if (asked) *asked = false;
            it->second.retryAt = m_time + (m_time < 10.f ? 2.f : 15.f);
            continue;
        }
        auto tex = std::make_unique<nxui::Texture>();
        if (!tex->loadFromPixels(app().gpu(), app().renderer(), d.rgba.data(), d.w, d.h)) {
            // No room in the picture memory: far ones go now and this one is asked
            // for again in a moment (it stayed blank until something moved).
            if (asked) *asked = false;
            it->second.retryAt = m_time + 0.5f;
            m_evictNow = true;
            m_retryIn = std::min(m_retryIn, 0.6f);
            continue;
        }
        if (d.what == kLaunch) {
            if (m_leaving && d.id == m_leaveId) {
                m_launchTex = std::move(tex);
                m_launchTexId = d.id;
                m_launchTexIn = m_time;
            }
        } else if (d.what == kIcon) {
            it->second.icon = std::move(tex);
            it->second.iconKind = d.kind;
        } else if (d.what == kBackground) {
            it->second.bg = std::move(tex);
            it->second.bgKind = d.kind;
            if (d.id == selected && m_bgId != d.id) {
                m_bgPrevId = m_bgId;
                m_bgId = d.id;
                m_bgFade = 0.f;
            }
        } else {
            it->second.card = std::move(tex);
            it->second.kind = d.kind;
            it->second.cardIn = m_time;
            if (!m_perfFirstCard && d.id == selected) {
                m_perfFirstCard = true;
                char line[64];
                std::snprintf(line, sizeof(line), "the chosen card's picture showed at %.0f ms", sinceStartMs());
                perfLine(line);
            }
        }
        ++m_perfUploads;
    }

    // Far from the selection, on any page: let the pictures go, now and then and
    // all together (freeing one waits for the GPU; the rest are then free).
    // Also when the picture memory runs low (fonts and HomeNX's screens use it
    // too), or a picture just had no room.
    const bool tight = app().gpu().imageMemoryAvailable() < (32ull << 20);
    const bool now = m_evictNow;
    if (!now && ((m_evictIn -= 1.f / 60.f) > 0.f || m_leaving)) return;
    m_evictIn = 2.f;
    m_evictNow = false;
    int loaded = 0;
    for (const auto& [id, a] : m_art) loaded += (a.card ? 1 : 0) + (a.bg ? 3 : 0);
    if (loaded < 56 && !tight && !now) return;
    const int keepCards = tight || now ? 14 : 24; // still well past the ~6 on screen either side
    const int keepBackgrounds = tight || now ? 1 : 4;
    std::map<std::uint64_t, int> distance;
    for (int p = 0; p < kPageCount; ++p) {
        const auto& page = m_pages[p];
        const int s = m_sels[p];
        for (size_t i = 0; i < page.size(); ++i) // another page's: as if 8 further away
            distance[page[i].id] = std::abs(static_cast<int>(i) - s) + (p == m_page ? 0 : 8);
    }
    for (auto& [id, a] : m_art) {
        auto d = distance.find(id);
        const int dist = d == distance.end() ? 1000 : d->second;
        if (a.card && dist > keepCards) {
            a.card.reset();
            a.cardAsked = false;
        }
        if (a.icon && dist > keepCards) {
            a.icon.reset();
            a.iconAsked = false;
        }
        if (a.bg && dist > keepBackgrounds && id != m_bgId && id != m_bgPrevId) {
            a.bg.reset();
            a.bgAsked = false;
        }
    }
}

void CardsActivity::forgetAllArt() {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_jobs.clear();
        m_done.clear();
    }
    app().gpu().waitIdle(); // they may still be drawing
    for (auto& page : m_pages)
        for (const Game& g : page) forgetArt(g.id);
    m_art.clear();
    m_bgId = m_bgPrevId = 0;
    wantArt();
}

// ---------------------------------------------------------------- the lists

void CardsActivity::selectById(std::uint64_t id) {
    for (int p = 0; p < kPageCount; ++p)
        for (size_t i = 0; i < m_pages[p].size(); ++i)
            if (m_pages[p][i].id == id) {
                m_page = p;
                m_sels[p] = static_cast<int>(i);
                m_scroll = static_cast<float>(i);
                return;
            }
}

void CardsActivity::select(int index) {
    m_backTo = 0; // you've moved on
    auto& page = games();
    if (page.empty()) return;
    index = std::clamp(index, 0, static_cast<int>(page.size()) - 1);
    if (index == sel()) return;
    sel() = index;
    m_audio.play("navigation");
    m_detailIn = m_settings.motion ? 0.f : 1.f;
    m_sheen = m_settings.motion ? 0.f : 1.f;
    const std::uint64_t id = page[(size_t)index].id;
    auto it = m_art.find(id);
    if (it != m_art.end() && it->second.bg && m_bgId != id) {
        m_bgPrevId = m_bgId;
        m_bgId = id;
        m_bgFade = 0.f;
    }
    wantArt();
}

int CardsActivity::pagePos(int page) const {
    for (int i = 0; i < kPageCount; ++i)
        if (m_settings.pageOrder[(size_t)i] == page) return i;
    return 0;
}

void CardsActivity::switchPage(int dir) {
    const int pos = (pagePos(m_page) + (dir > 0 ? 1 : -1) + kPageCount) % kPageCount;
    goToPage(m_settings.pageOrder[(size_t)pos], dir > 0 ? 1 : -1);
}

void CardsActivity::goToPage(int page, int dir) {
    if (page == m_page) return;
    if (dir == 0) dir = pagePos(page) > pagePos(m_page) ? 1 : -1;
    m_backTo = 0;
    // The Homebrew page: .nro apps added a while ago show (read on a thread).
    if (page == kHomebrew && static_cast<long long>(std::time(nullptr)) - m_settings.listsAt > 120) startListRefresh();
    if (m_moving) finishMove(true);
    m_page = page;
    m_pageDir = dir > 0 ? 1 : -1;
    m_pageIn = m_settings.motion ? 0.f : 1.f;
    m_detailIn = m_pageIn;
    m_scroll = static_cast<float>(sel());
    m_zone = Zone::Cards;
    m_audio.play("tab_transition");
    m_bgPrevId = m_bgId; // the new page's art fades in over the old
    m_bgFade = 0.f;
    const Game* cur = current();
    m_bgId = cur ? cur->id : 0;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_jobs.clear(); // what's near has changed
    }
    for (auto& [id, art] : m_art) {
        if (!art.card) art.cardAsked = false;
        if (!art.bg) art.bgAsked = false;
        if (!art.icon) art.iconAsked = false;
    }
    wantArt();
}

void CardsActivity::sortGames(int sort, bool keepChoice) {
    auto& page = m_pages[kGames];
    const std::uint64_t keep = page.empty() ? 0 : page[(size_t)std::clamp(m_sels[kGames], 0, (int)page.size() - 1)].id;
    m_settings.sort = std::clamp(sort, 0, 3);
    if (m_settings.sort == 3) {
        applyOrder(kGames);
    } else {
        std::stable_sort(page.begin(), page.end(), [&](const Game& a, const Game& b) {
            switch (m_settings.sort) {
                case 1: return lower(a.name) < lower(b.name);
                case 2: return a.playSeconds != b.playSeconds ? a.playSeconds > b.playSeconds : a.order < b.order;
                default: return a.lastPlayed != b.lastPlayed ? a.lastPlayed > b.lastPlayed : a.order < b.order;
            }
        });
    }
    m_sels[kGames] = 0;
    for (size_t i = 0; keepChoice && i < page.size(); ++i)
        if (page[i].id == keep) m_sels[kGames] = static_cast<int>(i);
    if (m_page == kGames) m_scroll = static_cast<float>(m_sels[kGames]);
    for (auto& [id, art] : m_art) {
        if (!art.card) art.cardAsked = false;
        if (!art.bg) art.bgAsked = false;
    }
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_jobs.clear();
    }
    wantArt();
}

// My order (Plus > Move): the saved ids first, in their order; the rest after
// (games by title, the rest as they were).
void CardsActivity::applyOrder(int page) {
    const auto& saved = m_settings.order[page];
    auto& list = m_pages[page];
    std::unordered_map<std::string, int> at;
    for (size_t i = 0; i < saved.size(); ++i) at.emplace(saved[i], static_cast<int>(i));
    auto rank = [&](const Game& g) {
        auto it = at.find(idText(g.id));
        return it == at.end() ? 1 << 30 : it->second;
    };
    std::stable_sort(list.begin(), list.end(), [&](const Game& a, const Game& b) {
        const int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        return page == kGames ? lower(a.name) < lower(b.name) : a.order < b.order;
    });
}

void CardsActivity::saveOrder(int page) {
    auto& saved = m_settings.order[page];
    saved.clear();
    for (const Game& g : m_pages[page]) saved.push_back(idText(g.id));
    m_settings.save();
}

// A game was installed, removed or a game card went in or out: read them
// again on a thread (it took the screen's thread seconds).
void CardsActivity::reloadGames() {
    startListRefresh();
}

// The real lists, read on a thread, replace last time's. Where you are stays:
// the first card stays first, a card further along stays chosen.
void CardsActivity::takeLists(Lists lists) {
    char line[80];
    std::snprintf(line, sizeof(line), "lists read again in %.0f ms (%zu games, %zu homebrew)", lists.ms,
                  lists.games.size(), lists.homebrew.size());
    perfLine(line);
    m_settings.listsAt = static_cast<long long>(std::time(nullptr));
    m_settings.save();
    auto chosen = [this](int p) -> std::uint64_t {
        const auto& page = m_pages[p];
        return m_sels[p] > 0 && m_sels[p] < static_cast<int>(page.size()) ? page[(size_t)m_sels[p]].id : 0;
    };
    const std::uint64_t keep[2] = {chosen(kGames), chosen(kHomebrew)};
    m_pages[kGames] = std::move(lists.games);
    m_pages[kHomebrew] = std::move(lists.homebrew);
    splitMedia();
    applyOrder(kHomebrew);
    sortGames(m_settings.sort, false);
    for (int p = kGames; p <= kHomebrew; ++p) {
        m_sels[p] = 0;
        for (size_t i = 0; keep[p] && i < m_pages[p].size(); ++i)
            if (m_pages[p][i].id == keep[p]) m_sels[p] = static_cast<int>(i);
    }
    m_scroll = static_cast<float>(sel());
    if (m_backTo) selectById(m_backTo);
    saveLists(m_pages[kGames], homebrewToSave());
    wantArt();
}

void CardsActivity::splitMedia() {
    auto& homebrew = m_pages[kHomebrew];
    std::vector<Game> media;
    for (auto it = homebrew.begin(); it != homebrew.end();) {
        if (!it->path.empty() && mediaAppOf(it->name, it->path)) {
            media.push_back(std::move(*it));
            it = homebrew.erase(it);
        } else {
            ++it;
        }
    }
    for (const MediaApp& m : kMediaApps) { // not on the SD card, but on GitHub: a card to get it
        if (!m.url) continue;
        bool have = false;
        for (const Game& g : media) have |= mediaAppOf(g.name, g.path) == &m;
        if (have) continue;
        Game g;
        g.name = m.name;
        g.author = "Get it from GitHub";
        g.path = m.path;
        g.action = "get";
        g.homebrew = true;
        g.id = 0xFD00000000000000ULL | (std::hash<std::string>{}(g.path) & 0x00FFFFFFFFFFFFFFULL);
        media.push_back(std::move(g));
    }
    for (size_t i = 0; i < media.size(); ++i) media[i].order = static_cast<int>(i);
    m_pages[kMedia] = std::move(media);
    applyOrder(kMedia);
    m_sels[kMedia] = std::clamp(m_sels[kMedia], 0, std::max(0, static_cast<int>(m_pages[kMedia].size()) - 1));
}

std::vector<Game> CardsActivity::homebrewToSave() const {
    std::vector<Game> all = m_pages[kHomebrew];
    for (const Game& g : m_pages[kMedia])
        if (g.action.empty()) all.push_back(g);
    return all;
}

void CardsActivity::getMedia(const Game& g) {
    const MediaApp* m = mediaAppOf(g.name, g.path);
    if (!m || !m->url) return;
    if (m_getJob.valid()) {
        say("Still getting " + m_getName + "...");
        return;
    }
    if (!m_netUp) {
        say("Connect to the internet to get " + g.name + ".");
        return;
    }
    say("Getting " + g.name + " from GitHub...");
    m_getPath = g.path;
    m_getName = g.name;
    const std::string url = m->url, path = g.path;
    m_getJob = std::async(std::launch::async, [url, path]() {
        offScreenCore();
        const bool ok = download(url, path);
        threadDone();
        return ok;
    });
}

void CardsActivity::openPageOrder(int select) {
    std::vector<MenuItem> items;
    auto label = [this](int pos) {
        return std::to_string(pos + 1) + ".  " + kPageNames[m_settings.pageOrder[(size_t)pos]];
    };
    for (int pos = 0; pos < kPageCount; ++pos)
        items.push_back(choice(label(pos), []() { return std::string("Move"); }, [this, pos, label](int d) {
            const int to = pos + d;
            if (to < 0 || to >= kPageCount) return;
            std::swap(m_settings.pageOrder[(size_t)pos], m_settings.pageOrder[(size_t)to]);
            m_settings.save();
            for (int i = 0; i < kPageCount && i < static_cast<int>(m_menu.size()); ++i) m_menu[(size_t)i].label = label(i);
            m_menuSel = to; // the page you're moving stays chosen
        }));
    items.push_back(action("Done", [] {}));
    openMenu("Page order  (< > moves a page)", std::move(items));
    m_menuSel = std::clamp(select, 0, static_cast<int>(kPageCount));
}

bool CardsActivity::glassCard(const Game& g) {
    if (g.homebrew || !g.action.empty()) return true;
    auto it = m_art.find(g.id);
    return it != m_art.end() && it->second.card && it->second.kind == ArtKind::Icon;
}

// ---------------------------------------------------------------- starting things

void CardsActivity::leave(int index, float seconds, std::function<void()> then) {
    if (m_leaving) return;
#ifdef SWITCHU_MENU
    if (m_update == static_cast<int>(Update::Installing)) {
        say("Wait for the update to finish first.");
        return;
    }
#endif
    m_leaving = true;
    m_left = false;
    m_leaveT = m_settings.motion ? 0.f : 0.98f;
    m_leaveFor = seconds * kLaunchPace[launchStyle()];
    m_leaveIndex = -1;
    m_leaveId = 0;
    float t = 0.f;
    if (index >= 0 && index < static_cast<int>(games().size()) && cardRect(index, m_leaveFrom, t)) {
        m_leaveIndex = index;
        m_leaveId = games()[(size_t)index].id;
        const int style = launchStyle();
        if (!glassCard(games()[(size_t)index]) && (style == kSplash || style == kZoom || style == kFlip)) {
            // its art at full size for the end, first in line
            {
                std::lock_guard<std::mutex> lk(m_mu);
                m_jobs.push_front({games()[(size_t)index], kLaunch});
            }
            m_cv.notify_one();
        }
    }
    m_leaveThen = std::move(then);
    m_menu.clear();
    m_notifOpen = false;
    m_sheetOpen = false;
    m_quickOpen = false;
    m_powerOpen = false;
    m_dotsOpen = false;
    m_optOpen = false;
    m_moving = false;
#ifdef SWITCHU_MENU
    m_newsOpen = false;
#endif
    m_audio.play("launch_game");
    m_audio.fadeOut(static_cast<int>(seconds * 1000.f));
}

void CardsActivity::comeBack() {
    m_leaving = false;
    m_left = false;
    m_leaveT = 0.f;
    m_leaveThen = nullptr;
    m_intro = std::min(m_intro, 0.6f); // the cards rise back in
    m_audio.setMusic(m_settings.music, m_settings.musicVolume);
}

void CardsActivity::play() {
    const Game* g = current();
    if (!g || m_leaving) return;
    if (!g->action.empty()) {
        openNintendo(*g);
        return;
    }
    if (!g->path.empty()) {
        openNro(g->path);
        return;
    }
    launchGame(*g);
}

void CardsActivity::launchGame(const Game& g) {
    using namespace switchu::ns;
    const std::uint64_t id = g.id;
    m_settings.lastGame = idText(id); // back from it, it's chosen
    if (!isRunning(id) && g.flags && !(g.flags & AppViewFlag_CanLaunch)) {
        if ((g.flags & AppViewFlag_IsGameCard) && !(g.flags & AppViewFlag_IsGameCardInserted))
            say("Put the game card in to play " + g.name + ".");
        else if (g.flags & AppViewFlag_NeedsUpdate) say(g.name + " needs an update first.");
        else if (g.flags & AppViewFlag_NeedsVerify) say(g.name + "'s data needs checking (System Settings > Data).");
        else if (g.flags & AppViewFlag_IsDownloading) say(g.name + " is still downloading.");
        else say(g.name + " can't start right now.");
        return;
    }
    // Which card it is on this page (the zoom starts from it).
    int index = -1;
    for (size_t i = 0; i < games().size(); ++i)
        if (games()[i].id == id) index = static_cast<int>(i);
#ifdef SWITCHU_MENU
    if (isRunning(id)) { // back to it
        leave(index, 0.85f, [this]() {
            m_discord.beginShutdown();
            m_launcher.resumeApplication();
        });
        return;
    }
    if (const std::uint64_t open = runningId()) { // another one is open: it closes first
        std::string name = "the game that's open";
        for (const auto& page : m_pages)
            for (const Game& o : page)
                if (o.id == open) name = o.name;
        const std::string title = g.name;
        openMenu("Close " + name + "?", {
            action("Close it and start " + title, [this, id, name]() {
                m_pendingLaunch = id;
                m_launcher.terminateApplication();
                say("Closing " + name + "...");
            }, true),
            action("Cancel", [] {}),
        });
        return;
    }
    leave(index, 1.3f, [this, id]() {
        m_discord.beginShutdown(); // its threads stop while the screen goes black
        if (!m_launcher.requestLaunch(id, launchUid())) {
            comeBack();
            say("The game didn't start. Try again.");
            return;
        }
        m_launcher.closeForLaunch();
    });
#else
    leave(index, 1.3f, [this, id]() {
        if (launch(id, m_user)) {
            app().requestExit(); // the game starts once this has closed
            return;
        }
        comeBack();
        say("Couldn't start it: update HomeNX, then try again.");
    });
#endif
}

void CardsActivity::openNro(const std::string& path) {
    int index = -1;
    for (size_t i = 0; i < games().size(); ++i)
        if (games()[i].path == path) index = static_cast<int>(i);
    if (index >= 0) m_settings.lastGame = idText(games()[(size_t)index].id);
#ifdef SWITCHU_MENU
    leave(index, 0.85f, [this, path]() {
        m_discord.beginShutdown();
        if (!m_launcher.launchNro(path, hostTitleId(), launchUid())) {
            comeBack();
            const std::string why = AppletLauncher::loaderError();
            say(why.empty() ? std::string("That app didn't start.") : why);
        }
    });
#else
    if (!envHasNextLoad()) {
        say("Open CardsNX from HOME to start apps.");
        return;
    }
    leave(index, 0.85f, [this, path]() { // it replaces this app, as hbmenu starts it
        envSetNextLoad(path.c_str(), ("\"" + path + "\"").c_str());
        app().requestExit();
    });
#endif
}

std::string CardsActivity::findNro(const char* nameHas) {
    for (int p : {static_cast<int>(kHomebrew), static_cast<int>(kMedia)})
        for (const Game& g : m_pages[p])
        if (!g.path.empty()
            && (lower(g.name).find(nameHas) != std::string::npos || lower(g.path).find(nameHas) != std::string::npos))
            return g.path;
    return {};
}

// The Browser card opens BrowseNX; the first time, it comes from GitHub.
void CardsActivity::openBrowser() {
    const std::string path = findNro("browsenx");
    if (!path.empty()) {
        openNro(path);
        return;
    }
    std::error_code ec;
    if (std::filesystem::exists(kBrowseNxPath, ec)) {
        openNro(kBrowseNxPath);
        return;
    }
    if (m_browserJob.valid()) {
        say("Still downloading BrowseNX...");
        return;
    }
    if (!m_netUp) {
        say("Connect to the internet to get BrowseNX.");
        return;
    }
    say("Getting BrowseNX from GitHub...");
    m_browserJob = std::async(std::launch::async, []() {
        offScreenCore();
        const bool ok = download(kBrowseNxUrl, kBrowseNxPath);
        threadDone();
        return ok;
    });
}

#ifdef SWITCHU_MENU
// HomeNX's keyboard, kept above every screen that asks for it.
void CardsActivity::ensureTextEntry() {
    ensureHxFonts();
    if (!m_textEntry) {
        m_textEntry = std::make_shared<TextEntryScreen>();
        m_textEntry->setFont(&m_hxFont);
        m_textEntry->setSmallFont(&m_hxSmall);
        m_textEntry->setTheme(&m_theme);
        m_textEntry->onKeySfx([this]() { m_audio.play("activation"); });
        m_textEntry->onNavigateSfx([this]() { m_audio.play("navigation"); });
        m_textEntry->onCloseSfx([this]() { m_audio.play("hide_modal"); });
    } else {
        rootBox().removeChild(m_textEntry.get());
    }
    rootBox().addChild(m_textEntry);
}

void CardsActivity::enterText(const std::string& title, const std::string& guide, const std::string& initial,
                              int maxLength, std::function<void(const std::string&)> done, nxui::Widget* back) {
    ensureTextEntry();
    if (m_textEntry->isActive()) return;
    m_textEntry->onAccept([this, done, back](const std::string& text) {
        if (done) done(text);
        if (back) focusManager().setFocus(back);
    });
    m_textEntry->onCancel([this, back]() {
        if (back) focusManager().setFocus(back);
    });
    m_audio.play("show_modal");
    m_textEntry->show({title, guide, initial, std::max(1, maxLength), false});
    focusManager().setFocus(m_textEntry.get());
}

// Friends, your profile and Mii Maker: HomeNX's own screen, not Nintendo's.
void CardsActivity::openSocial(bool miis) {
    ensureHxFonts();
    if (!m_social) {
        m_social = std::make_shared<SocialScreen>();
        m_social->setFonts(&m_hxFont, &m_hxSmall);
        m_social->setGpu(&app().gpu(), &app().renderer());
        m_social->setTheme(&m_theme);
        m_social->setIconDir("sdmc:/switch/SwitchU/icons");
        m_social->setDiscord(&m_discord);
        SocialScreen::Callbacks cb;
        cb.onClosed = [this]() { m_audio.play("hide_modal"); };
        cb.onEnterText = [this](const std::string& title, const std::string& guide, const std::string& initial,
                                int maxLength, std::function<void(const std::string&)> done) {
            enterText(title, guide, initial, maxLength, std::move(done), m_social.get());
        };
        cb.onDiscord = [this]() {
            if (m_social) m_social->hide();
            openDiscord();
        };
        cb.onMiiEditor = [this]() { if (m_social) m_social->showMiisPage(); };
        cb.onNavigate = [this]() { m_audio.play("navigation"); };
        cb.onActivate = [this]() { m_audio.play("activation"); };
        m_social->setCallbacks(std::move(cb));
        rootBox().addChild(m_social);
    }
    if (m_social->isActive()) return;
    std::vector<std::pair<std::uint64_t, std::string>> titles;
    for (const Game& g : m_pages[kGames]) titles.emplace_back(g.id, g.name);
    m_social->setTitles(std::move(titles));
    // You, with your picture.
    AccountUid uid{};
    uid.uid[0] = m_user.uid[0];
    uid.uid[1] = m_user.uid[1];
    std::string name;
    std::shared_ptr<nxui::Texture> face;
    for (const User& u : listUsers())
        if (u.uid.uid[0] == m_user.uid[0] && u.uid.uid[1] == m_user.uid[1]) {
            name = u.name;
            auto tex = std::make_shared<nxui::Texture>();
            if (!u.picture.empty() && tex->loadFromMemory(app().gpu(), app().renderer(), u.picture.data(), u.picture.size(), 128))
                face = tex;
        }
    m_audio.play("show_modal");
    m_social->show(uid, name, face);
    if (miis) m_social->showMiisPage();
    focusManager().setFocus(m_social.get());
}

void CardsActivity::ensureHxFonts() {
    if (m_hxFonts) return;
    m_hxFonts = true;
    auto& gpu = app().gpu();
    auto& ren = app().renderer();
    const std::string dejavu = "sdmc:/switch/SwitchU/fonts/DejaVuSans.ttf";
    const std::string chosen = fontchoice::chosen("discord"); // HomeNX Settings > Fonts
    const std::string path = chosen.empty() ? dejavu : "sdmc:/switch/SwitchU/fonts/" + chosen;
    if (!m_hxFont.load(gpu, ren, path, 24) || !m_hxSmall.load(gpu, ren, path, 18)) {
        m_hxFont.load(gpu, ren, dejavu, 24);
        m_hxSmall.load(gpu, ren, dejavu, 18);
    }
}
#endif

void CardsActivity::openDiscord() {
#ifdef SWITCHU_MENU
    ensureHxFonts();
    if (!m_discordPanel) {
        m_discordPanel = std::make_shared<DiscordPanel>();
        m_discordPanel->setFont(&m_hxFont);
        m_discordPanel->setSmallFont(&m_hxSmall);
        m_discordPanel->setGpu(&app().gpu(), &app().renderer());
        m_discordPanel->setLogoPath("sdmc:/switch/SwitchU/icons/discord.png");
        m_discordPanel->setService(&m_discord);
        DiscordPanel::Callbacks cb;
        cb.onClosed = [this]() { m_audio.play("hide_modal"); };
        cb.onReply = [this](Snowflake channelId, const std::string& name) {
            enterText("Message " + name, std::string(), std::string(), 2000, [this, channelId](const std::string& text) {
                if (!text.empty()) m_discord.send(channelId, text);
            }, m_discordPanel.get());
        };
        cb.onNavigate = [this]() { m_audio.play("navigation"); };
        cb.onActivate = [this]() { m_audio.play("activation"); };
        m_discordPanel->setCallbacks(std::move(cb));
        rootBox().addChild(m_discordPanel);
    }
    if (m_discordPanel->isActive()) return;
    m_audio.play("show_modal");
    m_discordPanel->show();
    focusManager().setFocus(m_discordPanel.get());
#else
    std::string path = findNro("switchcord");
    if (path.empty()) path = findNro("discord");
    if (!path.empty()) {
        openNro(path);
        return;
    }
    say("Get SwitchCord for Discord here, or make CardsNX your HOME in NXToggle.");
#endif
}

void CardsActivity::openNintendo(const Game& g) {
    const std::string& a = g.action;
    if (a == "get") { // a Media app that isn't on the SD card yet
        getMedia(g);
        return;
    }
    if (a == "settings") {
        openSettings();
        return;
    }
    if (a == "browser") {
        openBrowser();
        return;
    }
#ifdef SWITCHU_MENU
    if (a == "news") {
        openNews();
        return;
    }
    if (a == "mii" || a == "friends") { // HomeNX's own Mii Maker and Friends, inside CardsNX
        openSocial(a == "mii");
        return;
    }
    const int index = sel();
    // The daemon opens it and starts HOME again afterwards.
    leave(index, 0.75f, [this, a]() {
        m_discord.beginShutdown();
        const AccountUid uid = launchUid();
        if (a == "eshop") m_launcher.launchEshop(uid);
        else if (a == "album") m_launcher.launchAlbum();
        else if (a == "controllers") m_launcher.launchControllerPairing();
        else if (a == "mii") m_launcher.launchMiiEditor();
        else if (a == "friends") {
            if (R_SUCCEEDED(switchu::menu::smi_cmd::launchFriendsScreen(uid, FriendsLaArgType_ShowFriendList)))
                app().requestExit();
        }
        // Each asks HOME to close once the daemon has it; a refusal comes back
        // as OperationFailed (pumpDaemon), which brings the screen back.
    });
#else
    m_audio.play("activation");
    m_audio.pause();
    std::string error;
    const bool ok = openSystem(a, m_user, error);
    m_audio.resume();
    if (!ok) say(error);
#endif
}

// ---------------------------------------------------------------- homebrew updates

namespace {
constexpr const char* kUpdatesPath = "sdmc:/config/CardsNX/updates.json";

// HomeNX's own apps come with HomeNX's updates.
bool homenxOwn(const std::string& path) {
    for (const char* own : {"/switch/NXToggle/", "/switch/Homebrew-Manager/", "/switch/CardsNX/", "/switch/SwitchU/",
                            "/switch/HomeNX", "sdmc:/hbmenu.nro"})
        if (path.find(own) != std::string::npos) return true;
    return false;
}
} // namespace

const CardsActivity::NroUpdate* CardsActivity::nroUpdate(const Game& g) const {
    if (g.path.empty()) return nullptr;
    auto it = m_updates.find(g.path);
    if (it == m_updates.end() || !updater::isNewer(it->second.latest, g.version)) return nullptr;
    return &it->second;
}

// What's newer: from the saved list when it's fresh (a few hours), else asked
// again on a thread (a request per app to GitHub, which allows 60 an hour).
void CardsActivity::checkHomebrewUpdates(bool force) {
    if (m_updatesJob.valid()) return;
    const long long now = static_cast<long long>(std::time(nullptr));
    if (!force) {
        try {
            std::ifstream in(kUpdatesPath);
            if (in) {
                const auto j = nlohmann::json::parse(in);
                const long long at = j.value("checkedAt", 0LL);
                m_updates.clear();
                const auto saved = j.value("apps", nlohmann::json::object());
                for (auto it = saved.begin(); it != saved.end(); ++it) {
                    NroUpdate u;
                    u.latest = it.value().value("latest", std::string());
                    u.source = it.value().value("source", std::string());
                    u.url = it.value().value("url", std::string());
                    u.zipEntry = it.value().value("zipEntry", std::string());
                    u.zip = it.value().value("zip", false);
                    m_updates[it.key()] = u;
                }
                if (now - at < 6 * 3600 && now >= at) return; // fresh enough
            }
        } catch (...) {
        }
    }
    if (!m_netUp) return;
    std::vector<updater::App> apps;
    for (Page p : {kHomebrew, kMedia})
        for (const Game& g : m_pages[p])
        if (!g.path.empty() && g.action.empty() && !homenxOwn(g.path)) {
            updater::App a;
            a.path = g.path;
            a.name = g.name;
            a.author = g.author;
            a.version = g.version;
            apps.push_back(std::move(a));
        }
    m_updatesJob = std::async(std::launch::async, [apps, now]() mutable {
        offScreenCore();
        std::atomic<int> done{0};
        updater::findUpdates(apps, done);
        std::map<std::string, NroUpdate> found;
        nlohmann::json saved = {{"checkedAt", now}, {"apps", nlohmann::json::object()}};
        for (const updater::App& a : apps) {
            if (a.downloadUrl.empty() || !updater::isNewer(a.latest, a.version)) continue;
            NroUpdate u{a.latest, a.source, a.downloadUrl, a.zipEntry, a.zip};
            found[a.path] = u;
            saved["apps"][a.path] = {{"latest", u.latest}, {"source", u.source}, {"url", u.url},
                                     {"zipEntry", u.zipEntry}, {"zip", u.zip}};
        }
        std::error_code ec;
        std::filesystem::create_directories("sdmc:/config/CardsNX", ec);
        std::ofstream(kUpdatesPath, std::ios::trunc) << saved.dump();
        threadDone();
        return found;
    });
}

// One .nro to its newest version (the Homebrew Manager's way: the old one is
// kept aside until the new one is in place).
void CardsActivity::updateNro(const std::string& path) {
    auto it = m_updates.find(path);
    if (it == m_updates.end()) return;
    if (m_nroUpdateJob.valid()) { // one at a time: it's next
        if (std::find(m_nroQueue.begin(), m_nroQueue.end(), path) == m_nroQueue.end()) m_nroQueue.push_back(path);
        return;
    }
    updater::App a;
    a.path = path;
    a.latest = it->second.latest;
    a.source = it->second.source;
    a.downloadUrl = it->second.url;
    a.zipEntry = it->second.zipEntry;
    a.zip = it->second.zip;
    m_nroUpdating = path;
    m_nroProgress = 0.f;
    m_nroUpdateJob = std::async(std::launch::async, [this, a]() {
        offScreenCore();
        std::string error;
        const bool ok = updater::installUpdate(a, m_nroProgress, error);
        threadDone();
        return ok ? std::string() : (error.empty() ? std::string("it didn't download") : error);
    });
}

void CardsActivity::openHomebrewUpdates() {
    std::vector<MenuItem> items;
    std::vector<std::string> paths;
    for (Page p : {kHomebrew, kMedia})
        for (const Game& g : m_pages[p])
        if (const NroUpdate* u = nroUpdate(g)) {
            const std::string path = g.path;
            paths.push_back(path);
            MenuItem row = action(g.name, [this, path]() { updateNro(path); });
            const std::string latest = u->latest;
            row.value = [latest]() { return "v" + latest; };
            items.push_back(std::move(row));
        }
    if (paths.empty()) {
        items.push_back(info("Everything is up to date", [] { return std::string(); }));
    } else {
        items.insert(items.begin(), action("Update all (" + std::to_string(paths.size()) + ")", [this, paths]() {
            for (const std::string& p : paths) updateNro(p);
        }));
    }
    items.push_back(action("Check again", [this]() {
        checkHomebrewUpdates(true);
        say(m_netUp ? "Checking for homebrew updates..." : "Connect to the internet to check.");
    }));
    items.push_back(action("Done", [] {}));
    openMenu("Homebrew updates", std::move(items));
}

// ---------------------------------------------------------------- HOME (the daemon)

#ifdef SWITCHU_MENU
// A game whose code the daemon swaps for hbloader while homebrew runs, so
// homebrew gets application mode (installed, startable, not a game card, not
// the one open). 0: over the Album instead.
std::uint64_t CardsActivity::hostTitleId() const {
    using namespace switchu::ns;
    for (const Game& g : m_pages[kGames])
        if (g.id && !isRunning(g.id) && (g.flags == 0 || ((g.flags & AppViewFlag_CanLaunch) && !(g.flags & AppViewFlag_IsGameCard))))
            return g.id;
    return 0;
}

AccountUid CardsActivity::launchUid() const {
    AccountUid uid{};
    uid.uid[0] = m_user.uid[0];
    uid.uid[1] = m_user.uid[1];
    return uid;
}

void CardsActivity::pumpDaemon() {
    AppletStorage st;
    while (R_SUCCEEDED(appletPopInteractiveInData(&st))) {
        switchu::smi::DaemonNotification n{};
        s64 size = 0;
        appletStorageGetSize(&st, &size);
        if (size >= static_cast<s64>(sizeof(n))) appletStorageRead(&st, 0, &n, sizeof(n));
        appletStorageClose(&st);
        if (n.magic != switchu::smi::kNotifyMagic) continue;
        using M = switchu::smi::MenuMessage;
        switch (n.msg) {
            case M::HomeRequest:
                if (!m_asleep) handleHome();
                break;
            case M::ApplicationExited: {
                m_launcher.setAppRunning(false);
                m_launcher.setAppHasForeground(false);
                m_launcher.setSuspendedTitleId(0);
                const std::uint64_t next = m_pendingLaunch;
                m_pendingLaunch = 0;
                if (next)
                    for (const auto& page : m_pages)
                        for (const Game& g : page)
                            if (g.id == next && g.action.empty() && g.path.empty()) {
                                const Game copy = g;
                                launchGame(copy);
                                return;
                            }
                break;
            }
            case M::ApplicationSuspended:
                m_launcher.setAppRunning(true);
                m_launcher.setAppHasForeground(false);
                m_launcher.setSuspendedTitleId(n.app_id);
                break;
            case M::AppRecordsChanged:
            case M::GameCardMountFailure:
                reloadGames();
                break;
            case M::AppViewFlagsUpdate:
                for (auto& page : m_pages)
                    for (Game& g : page)
                        if (g.id == n.app_id) g.flags = n.payload;
                break;
            case M::BatteryStatusChanged:
                m_battery = std::to_string(switchu::smi::batteryPayloadPercentage(n.payload)) + "%";
                break;
            case M::SleepSequence: // stop drawing (a loop that keeps drawing never lets it sleep)
                m_asleep = true;
                m_audio.pause();
                app().setSleeping(true);
                break;
            case M::WakeUp:
                m_asleep = false;
                app().setSleeping(false);
                m_audio.resume();
                break;
            case M::OperationFailed: {
                char buf[80];
                std::snprintf(buf, sizeof(buf), "That didn't work (0x%X).", n.payload);
                if (m_leaving) comeBack();
                say(buf);
                break;
            }
            default:
                break;
        }
    }
}
#endif

// Everything open closes; true if something was.
bool CardsActivity::closeEverything() {
    bool any = !m_menu.empty() || m_notifOpen || m_sheetOpen || m_moving || m_quickOpen || m_powerOpen || m_dotsOpen
            || m_optOpen;
    m_powerOpen = false;
    m_dotsOpen = false;
    m_optOpen = false;
    if (m_moving) finishMove(false);
    m_menu.clear();
    m_notifOpen = false;
    m_sheetOpen = false;
    m_quickOpen = false;
#ifdef SWITCHU_MENU
    if (m_textEntry && m_textEntry->isActive()) {
        m_textEntry->hide(false);
        any = true;
    }
    if (m_discordPanel && m_discordPanel->isOpen()) {
        m_discordPanel->hide();
        any = true;
    }
    if (m_social && m_social->isOpen()) {
        m_social->hide();
        any = true;
    }
    if (m_padTest && m_padTest->isActive()) {
        m_padTest->hide();
        any = true;
    }
    if (m_newsOpen) {
        m_newsOpen = false;
        any = true;
    }
#endif
    return any;
}

// HOME pressed while HOME shows: things close; else back to the open game;
// else the first card.
void CardsActivity::handleHome() {
    if (m_leaving) return;
    if (closeEverything()) {
        m_audio.play("hide_modal");
        return;
    }
    if (const std::uint64_t open = runningId()) {
        for (const auto& page : m_pages)
            for (const Game& g : page)
                if (g.id == open) {
                    const Game copy = g;
                    launchGame(copy);
                    return;
                }
        return;
    }
    goToPage(m_settings.pageOrder[0]);
    m_zone = Zone::Cards;
    select(0);
}

// ---------------------------------------------------------------- popups

void CardsActivity::openMenu(std::string title, std::vector<MenuItem> items) {
    m_menuTitle = std::move(title);
    m_menu = std::move(items);
    m_menuSel = 0;
    m_menuIn = m_settings.motion ? 0.f : 1.f;
    m_audio.play("show_modal");
}

// Plus (and the panel's "..."): what can be done with the chosen card.
void CardsActivity::openOptions() {
    const Game* gp = current();
    if (!gp) return;
    const Game g = *gp;
    std::vector<MenuItem> items;
    const bool game = g.action.empty() && g.path.empty() && !g.homebrew;
    const bool running = isRunning(g.id);
    items.push_back(action(running ? "Resume" : g.action == "get" ? "Get" : game ? "Play" : "Open", [this]() { play(); }));
    items.back().glyph = g.action == "get" ? "download" : "play";
#ifdef SWITCHU_MENU
    if (running) {
        items.push_back(action("Close software", [this]() {
            m_launcher.terminateApplication();
            say("Closing it...");
        }, true));
        items.back().glyph = "power";
    }
#endif
    items.push_back(action("Move", [this]() { startMove(); }));
    items.back().glyph = "arrows";
    if (g.action.empty()) {
        const std::uint64_t id = g.id;
        items.push_back(action("Download new pictures", [this, id]() {
            forgetArt(id);
            auto it = m_art.find(id);
            if (it != m_art.end()) {
                it->second.cardAsked = it->second.bgAsked = false;
                app().gpu().waitIdle(); // they may still be drawing
                it->second.card.reset();
                it->second.bg.reset();
            }
            wantArt();
        }));
        items.back().glyph = "restart";
    }
    if (const NroUpdate* u = nroUpdate(g)) { // a newer version
        const std::string path = g.path;
        MenuItem up = action("Update to v" + u->latest, [this, path]() { updateNro(path); });
        up.glyph = "download";
        const std::string source = u->source;
        up.value = [source]() { return source; };
        items.insert(items.begin() + 1, std::move(up));
    }
    if (!g.path.empty() && g.action.empty()) { // a .nro: off the SD card
        const std::string path = g.path, name = g.name;
        const std::uint64_t id = g.id;
        const int pg = m_page;
        items.push_back(action("Delete", [this, path, name, id, pg]() {
            openMenu("Delete " + name + "?", {
                action("Delete it from the SD card", [this, path, name, id, pg]() {
                    std::error_code ec;
                    if (!std::filesystem::remove(path, ec) || ec) {
                        say("Couldn't delete " + name + ".");
                        return;
                    }
                    auto& page = m_pages[pg];
                    page.erase(std::remove_if(page.begin(), page.end(), [id](const Game& x) { return x.id == id; }),
                               page.end());
                    m_sels[pg] = std::clamp(m_sels[pg], 0, std::max(0, (int)page.size() - 1));
                    if (m_page == pg) m_scroll = static_cast<float>(sel());
                    saveOrder(pg);
                    saveLists(m_pages[kGames], homebrewToSave());
                    m_audio.play("toggle_off");
                    say(name + " was deleted.");
                    wantArt();
                }, true),
                action("Cancel", [] {}),
            });
        }, true));
        items.back().glyph = "trash";
    }
    if (g.path.empty() && g.homebrew && g.action.empty()) { // an NRO forwarder: uninstalled from the console
        const std::uint64_t id = g.id;
        const std::string name = g.name;
        items.push_back(action("Delete", [this, id, name]() {
            if (isRunning(id)) {
                say("Close " + name + " first.");
                return;
            }
            openMenu("Delete " + name + "?", {
                action("Delete it from the console", [this, id, name]() {
                    if (m_deleteJob.valid()) {
                        say("Still deleting " + m_deleteName + "...");
                        return;
                    }
                    m_deleteId = id;
                    m_deleteName = name;
                    say("Deleting " + name + "...");
                    m_deleteJob = std::async(std::launch::async, [id]() {
                        offScreenCore();
                        return static_cast<std::uint32_t>(nsDeleteApplicationCompletely(id));
                    });
                }, true),
                action("Cancel", [] {}),
            });
        }, true));
        items.back().glyph = "trash";
    }
    items.push_back(action("Cancel", [] {}));
    m_optItems = std::move(items);
    m_optGame = g;
    m_optSel = 0;
    m_optOpen = true;
    m_optIn = m_settings.motion ? 0.f : 1.f;
    m_audio.play("show_modal");
}

namespace {
const nxui::Rect kOptCard{196.f, 124.f, 888.f, 472.f};
constexpr float kOptRowsY = 150.f; // below the title and its line, within the card
nxui::Rect optionRow(size_t n, size_t i, float dy) {
    const float x = kOptCard.x + 32.f + 260.f + 36.f - 10.f, w = kOptCard.x + kOptCard.width - 26.f - x;
    const float rowH = std::min(56.f, (kOptCard.height - kOptRowsY - 24.f) / static_cast<float>(std::max<size_t>(1, n)));
    return {x, kOptCard.y + kOptRowsY + rowH * static_cast<float>(i) + dy, w, rowH - 8.f};
}
} // namespace

void CardsActivity::updateOptions() {
    auto& in = app().input();
    using B = nxui::Button;
    const int n = static_cast<int>(m_optItems.size());
    if ((in.isDown(B::DUp) || in.isDown(B::LStickU)) && m_optSel > 0) { --m_optSel; m_audio.play("navigation"); }
    if ((in.isDown(B::DDown) || in.isDown(B::LStickD)) && m_optSel < n - 1) { ++m_optSel; m_audio.play("navigation"); }
    if (in.isDown(B::B) || in.isDown(B::Plus) || in.isDown(B::X)) {
        m_optOpen = false;
        m_audio.play("hide_modal");
        return;
    }
    if (in.isDown(B::A) && m_optSel >= 0 && m_optSel < n) {
        auto run = m_optItems[(size_t)m_optSel].run;
        m_optOpen = false;
        m_audio.play("activation");
        if (run) run();
    }
}

bool CardsActivity::optionsTouch() {
    auto& in = app().input();
    if (!in.touchUp()) return in.isTouching();
    const float x = in.touchX(), y = in.touchY();
    for (size_t i = 0; i < m_optItems.size(); ++i)
        if (optionRow(m_optItems.size(), i, 0.f).contains(x, y)) {
            m_optSel = static_cast<int>(i);
            auto run = m_optItems[i].run;
            m_optOpen = false;
            if (run) run();
            return true;
        }
    if (!kOptCard.contains(x, y)) {
        m_optOpen = false;
        m_audio.play("hide_modal");
    }
    return true;
}

void CardsActivity::drawOptions(nxui::Renderer& r) {
    if (!m_optOpen) return;
    const float a = easeOut(m_optIn);
    const float ring = 0.8f + 0.2f * std::sin(m_time * 5.f);
    r.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.5f * a));
    const nxui::Rect card = scaled(kOptCard, 0.95f + 0.05f * easeOutBack(m_optIn));
    frost(r, card, 34.f, nxui::Color(0.08f, 0.09f, 0.13f, 0.86f * a));
    r.drawRoundedRectOutline(card, nxui::Color(1.f, 1.f, 1.f, 0.16f * a), 34.f, 1.5f);
    r.drawGradientRect({card.x + 2.f, card.y + 2.f, card.width - 4.f, 90.f}, nxui::Color(1.f, 1.f, 1.f, 0.05f * a),
                       nxui::Color(1.f, 1.f, 1.f, 0.f));
    const float dy = card.y - kOptCard.y;
    const Game& g = m_optGame;

    // Its art, as on its card (an app: its icon on glass).
    const nxui::Rect art{kOptCard.x + 32.f, kOptCard.y + 50.f + dy, 260.f, 368.f};
    r.drawRoundedRect({art.x - 3.f, art.y + 8.f, art.width + 6.f, art.height + 6.f}, nxui::Color(0.f, 0.f, 0.f, 0.3f * a), 27.f);
    auto it = m_art.find(g.id);
    const nxui::Texture* tex = it != m_art.end() && it->second.card && it->second.card->valid() ? it->second.card.get() : nullptr;
    if (glassCard(g)) {
        frost(r, art, 24.f, glassTint(1.3f * a));
        r.drawRoundedRectOutline(art, nxui::Color(1.f, 1.f, 1.f, 0.18f * a), 24.f, 1.5f);
        if (tex) {
            const float side = 170.f;
            const nxui::Rect ic{art.x + (art.width - side) * 0.5f, art.y + (art.height - side) * 0.42f, side, side};
            if (!g.action.empty()) r.drawTexture(tex, ic, kWhite.withAlpha(a));
            else r.drawTextureRounded(tex, ic, 36.f, kWhite.withAlpha(a));
        }
    } else if (tex) {
        drawCover(r, tex, art, 24.f, kWhite.withAlpha(a), it->second.kind == ArtKind::Box ? 0.125f : 0.f, 0.f);
    } else {
        r.drawRoundedRect(art, nxui::Color(0.28f, 0.31f, 0.40f, 0.9f * a), 24.f);
    }

    // Its name and a line about it.
    const float tx = art.x + art.width + 36.f, maxW = kOptCard.x + kOptCard.width - 36.f - tx;
    const float ts = fitScale(m_fTitle, g.name, maxW, 0.75f);
    r.drawText(fit(m_fTitle, g.name, maxW, ts), {tx, kOptCard.y + 50.f + dy}, &m_fTitle, kWhite.withAlpha(a), ts);
    std::string about;
    if (!g.action.empty()) about = g.author;
    else if (g.homebrew) about = g.path.empty() ? "NRO forwarder" : (g.author.empty() ? "Homebrew" : "Homebrew by " + g.author);
    else about = playTime(g.playSeconds) + " played    " + lastPlayed(g.lastPlayed);
    if (!g.version.empty()) about += "    v" + (g.version[0] == 'v' || g.version[0] == 'V' ? g.version.substr(1) : g.version);
    r.drawText(fit(m_fBody, about, maxW), {tx, kOptCard.y + 94.f + dy}, &m_fBody, kMuted.withAlpha(a));

    // The buttons: an icon on a disc, its name; the first one (Play) white.
    for (size_t i = 0; i < m_optItems.size(); ++i) {
        const MenuItem& item = m_optItems[i];
        const float ta = a * easeOut((m_optIn - 0.04f * static_cast<float>(i)) / 0.7f);
        nxui::Rect row = optionRow(m_optItems.size(), i, dy);
        row.x += 16.f * (1.f - ta);
        const bool chosen = static_cast<int>(i) == m_optSel, primary = i == 0;
        if (!item.glyph) { // Cancel: just the word
            const std::string w = item.label;
            textMid(r, m_fPill, w, row.x + (row.width - measured(m_fPill, w).x) * 0.5f, row.y + row.height * 0.5f,
                    (chosen ? kWhite : kMuted).withAlpha(ta));
            if (chosen) r.drawRoundedRectOutline(row, accent(ring * ta), row.height * 0.5f, 2.f);
            continue;
        }
        r.drawRoundedRect(row, primary ? kWhite.withAlpha((chosen ? 1.f : 0.88f) * ta)
                                       : nxui::Color(1.f, 1.f, 1.f, (chosen ? 0.16f : 0.07f) * ta), row.height * 0.5f);
        if (chosen) r.drawRoundedRectOutline({row.x - 3.f, row.y - 3.f, row.width + 6.f, row.height + 6.f}, accent(ring * ta),
                                             row.height * 0.5f + 3.f, 2.5f);
        const float cy = row.y + row.height * 0.5f, d = row.height - 12.f;
        const nxui::Color disc = primary ? nxui::Color(0.f, 0.f, 0.f, 0.85f * ta)
                               : item.danger ? kRed.withAlpha(0.9f * ta) : accent(0.35f * ta);
        r.drawCircle({row.x + 10.f + d * 0.5f, cy}, d * 0.5f, disc, 32);
        icon(r, item.glyph, row.x + 10.f + d * 0.5f, cy, d * 0.55f, kWhite.withAlpha(ta));
        const nxui::Color ink = primary ? nxui::Color(0.f, 0.f, 0.f, ta) : item.danger ? nxui::Color(1.f, 0.5f, 0.47f, ta)
                                                                                      : kWhite.withAlpha(ta);
        textMid(r, m_fPill, item.label, row.x + 22.f + d, cy, ink);
        if (item.value) {
            const std::string v = fit(m_fBody, item.value(), row.width * 0.45f);
            textMid(r, m_fBody, v, row.x + row.width - 18.f - measured(m_fBody, v).x, cy,
                    primary ? nxui::Color(0.f, 0.f, 0.f, 0.6f * ta) : kMuted.withAlpha(ta));
        }
    }
}

void CardsActivity::startMove() {
    if (games().size() < 2) {
        say("There's nothing to move it past.");
        return;
    }
    m_moving = true;
    m_moveBefore = games();
    m_moveBeforeSel = sel();
    m_zone = Zone::Cards;
    m_audio.play("activation");
}

void CardsActivity::finishMove(bool keep) {
    if (!m_moving) return;
    m_moving = false;
    if (!keep) {
        games() = std::move(m_moveBefore);
        sel() = m_moveBeforeSel;
        m_scroll = static_cast<float>(sel());
        m_audio.play("hide_modal");
        return;
    }
    m_moveBefore.clear();
    saveOrder(m_page);
    if (m_page == kGames) m_settings.sort = 3; // the order shown is yours now
    m_settings.save();
    m_audio.play("toggle_on");
}

std::vector<CardsActivity::MenuItem> CardsActivity::powerItems() {
    std::vector<MenuItem> items;
#ifdef SWITCHU_MENU
    items.push_back(action("Sleep", [this]() { m_launcher.enterSleep(); }));
    items.push_back(action("Restart", [this]() { leave(-1, 0.4f, [this]() { m_launcher.reboot(); }); }));
    items.push_back(action("Shut down", [this]() { leave(-1, 0.4f, [this]() { m_launcher.shutdown(); }); }));
    items.push_back(action("Reboot to Hekate", [this]() {
        // Atmosphere sends hbl's reboots to its payload (not HOME's): CardsNX
        // (the .nro) does it, asked by a file, before it shows anything.
        std::error_code ec;
        const char* cards = "sdmc:/switch/CardsNX/CardsNX.nro";
        if (std::filesystem::exists(cards, ec) && AppletLauncher::canRebootToPayload()
            && !AppletLauncher::hekatePayload().empty()) {
            std::ofstream("sdmc:/config/SwitchU/cardsnx_reboot_hekate") << "1";
            leave(-1, 0.4f, [this, cards]() {
                if (!m_launcher.launchNro(cards, hostTitleId(), launchUid())) {
                    std::remove("sdmc:/config/SwitchU/cardsnx_reboot_hekate");
                    comeBack();
                    say("Hekate didn't start.");
                }
            });
            return;
        }
        if (!AppletLauncher::canRebootToPayload()) { // newer models: the power chip's clock, then a restart
            leave(-1, 0.45f, [this]() {
                if (!m_launcher.rebootToHekate()) {
                    comeBack();
                    say("The power chip didn't take the request for Hekate's menu.");
                }
            });
            return;
        }
        if (!m_launcher.rebootToHekate())
            say(AppletLauncher::hekatePayload().empty() ? "Hekate isn't on the SD card (bootloader/update.bin)."
                                                        : "Atmosphere didn't take the Hekate payload.");
    }));
#else
    auto doPower = [this](Power p) {
        return [this, p]() {
            m_settings.save();
            m_audio.fadeOut(200);
            std::string error;
            if (!power(p, error)) say(error);
            else if (p != Power::Sleep) app().requestExit();
        };
    };
    items.push_back(action("Sleep", doPower(Power::Sleep)));
    items.push_back(action("Restart", doPower(Power::Restart)));
    items.push_back(action("Shut down", doPower(Power::Shutdown)));
    items.push_back(action("Reboot to Hekate", doPower(Power::Hekate)));
#endif
    return items;
}

// ---------------------------------------------------------------- the three dots

namespace {
constexpr float kDotsX = 96.f, kDotsY = 92.f, kDotsW = 470.f, kDotsPad = 18.f;
constexpr float kDotsPagesH = 52.f, kDotsTileW = 102.f, kDotsTileH = 104.f, kDotsGap = 10.f;
constexpr int kDotsCols = 4;
nxui::Rect dotsPanel(size_t tiles) {
    const int rows = static_cast<int>((tiles + kDotsCols - 1) / kDotsCols);
    return {kDotsX, kDotsY, kDotsW, kDotsPad + 30.f + kDotsPagesH + 14.f + rows * (kDotsTileH + kDotsGap) - kDotsGap + kDotsPad};
}
nxui::Rect dotsPage(const nxui::Rect& p, int pos) {
    const float w = (p.width - kDotsPad * 2.f - 8.f * 3.f) / 4.f;
    return {p.x + kDotsPad + (w + 8.f) * static_cast<float>(pos), p.y + kDotsPad + 30.f, w, kDotsPagesH};
}
nxui::Rect dotsTile(const nxui::Rect& p, int i) {
    const float x0 = p.x + (p.width - (kDotsTileW * kDotsCols + kDotsGap * (kDotsCols - 1))) * 0.5f;
    return {x0 + (kDotsTileW + kDotsGap) * static_cast<float>(i % kDotsCols),
            p.y + kDotsPad + 30.f + kDotsPagesH + 14.f + (kDotsTileH + kDotsGap) * static_cast<float>(i / kDotsCols), kDotsTileW,
            kDotsTileH};
}
} // namespace

void CardsActivity::openDots() {
    m_dotsTiles.clear();
#ifdef SWITCHU_MENU
    m_dotsTiles.push_back({"News", "news", nxui::Color(0.95f, 0.25f, 0.25f, 1.f), [this]() { openNews(); }});
#endif
    m_dotsTiles.push_back({"Discord", "discord", nxui::Color(0.35f, 0.40f, 0.95f, 1.f), [this]() { openDiscord(); }});
    m_dotsTiles.push_back({"Users", "friends", nxui::Color(1.f, 0.6f, 0.2f, 1.f), [this]() { openUsers(); }});
    m_dotsTiles.push_back({"Music", "music", nxui::Color(0.95f, 0.35f, 0.6f, 1.f), [this]() { openMusic(); }});
    m_dotsTiles.push_back({"Quick", "sun", nxui::Color(1.f, 0.78f, 0.25f, 1.f), [this]() { openQuick(); }});
    m_dotsTiles.push_back({"Updates", "sort", nxui::Color(0.25f, 0.8f, 0.5f, 1.f), [this]() { openHomebrewUpdates(); }});
    m_dotsTiles.push_back({"Settings", "gear", nxui::Color(0.55f, 0.6f, 0.68f, 1.f), [this]() { openSettings(); }});
    m_dotsTiles.push_back({"Power", "power", nxui::Color(1.f, 0.36f, 0.36f, 1.f), [this]() { openPower(); }});
#ifndef SWITCHU_MENU
    m_dotsTiles.push_back({"Close", "home", nxui::Color(0.5f, 0.55f, 0.62f, 1.f), [this]() {
        m_settings.save();
        app().requestExit();
    }});
#endif
    m_dotsOpen = true;
    m_dotsRow = 1;
    m_dotsCol = 0;
    m_dotsIn = m_settings.motion ? 0.f : 1.f;
    m_audio.play("show_modal");
}

void CardsActivity::updateDots() {
    auto& in = app().input();
    using B = nxui::Button;
    const int tiles = static_cast<int>(m_dotsTiles.size());
    const int rows = 1 + (tiles + kDotsCols - 1) / kDotsCols; // the pages, then the tiles
    auto colsIn = [&](int row) { return row == 0 ? kPageCount : std::min(kDotsCols, tiles - (row - 1) * kDotsCols); };
    const bool up = in.isDown(B::DUp) || in.isDown(B::LStickU), down = in.isDown(B::DDown) || in.isDown(B::LStickD);
    const bool left = in.isDown(B::DLeft) || in.isDown(B::LStickL), right = in.isDown(B::DRight) || in.isDown(B::LStickR);
    if (in.isDown(B::B) || in.isDown(B::Plus)) {
        m_dotsOpen = false;
        m_audio.play("hide_modal");
        return;
    }
    if (up && m_dotsRow > 0) { --m_dotsRow; m_audio.play("navigation"); }
    if (down && m_dotsRow < rows - 1) { ++m_dotsRow; m_audio.play("navigation"); }
    m_dotsCol = std::clamp(m_dotsCol, 0, colsIn(m_dotsRow) - 1);
    if (left && m_dotsCol > 0) { --m_dotsCol; m_audio.play("navigation"); }
    if (right && m_dotsCol < colsIn(m_dotsRow) - 1) { ++m_dotsCol; m_audio.play("navigation"); }
    if (!in.isDown(B::A)) return;
    m_dotsOpen = false;
    if (m_dotsRow == 0) {
        goToPage(m_settings.pageOrder[(size_t)m_dotsCol]);
        return;
    }
    const int i = (m_dotsRow - 1) * kDotsCols + m_dotsCol;
    if (i >= 0 && i < tiles && m_dotsTiles[(size_t)i].run) {
        auto run = m_dotsTiles[(size_t)i].run;
        m_audio.play("activation");
        run();
    }
}

bool CardsActivity::dotsTouch() {
    auto& in = app().input();
    if (!in.touchUp()) return in.isTouching();
    const float x = in.touchX(), y = in.touchY();
    const nxui::Rect p = dotsPanel(m_dotsTiles.size());
    m_dotsOpen = false;
    if (!p.contains(x, y)) {
        m_audio.play("hide_modal");
        return true;
    }
    for (int pos = 0; pos < kPageCount; ++pos)
        if (dotsPage(p, pos).contains(x, y)) {
            goToPage(m_settings.pageOrder[(size_t)pos]);
            return true;
        }
    for (size_t i = 0; i < m_dotsTiles.size(); ++i)
        if (dotsTile(p, static_cast<int>(i)).contains(x, y) && m_dotsTiles[i].run) {
            auto run = m_dotsTiles[i].run;
            m_audio.play("activation");
            run();
            return true;
        }
    m_dotsOpen = true; // between the tiles: it stays open
    return true;
}

void CardsActivity::drawDots(nxui::Renderer& r) {
    if (!m_dotsOpen) return;
    const float a = easeOut(m_dotsIn);
    const float ring = 0.8f + 0.2f * std::sin(m_time * 5.f);
    r.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.32f * a));
    const nxui::Rect full = dotsPanel(m_dotsTiles.size());
    // It drops from the dots button: grows from its top-left corner.
    const float s = 0.92f + 0.08f * easeOutBack(m_dotsIn);
    const nxui::Rect p{full.x, full.y - 10.f * (1.f - a), full.width * s, full.height * s};
    frost(r, p, 28.f, nxui::Color(0.08f, 0.09f, 0.13f, 0.86f * a));
    r.drawRoundedRectOutline(p, nxui::Color(1.f, 1.f, 1.f, 0.16f * a), 28.f, 1.5f);
    r.drawGradientRect({p.x + 2.f, p.y + 2.f, p.width - 4.f, 70.f}, nxui::Color(1.f, 1.f, 1.f, 0.05f * a),
                       nxui::Color(1.f, 1.f, 1.f, 0.f));
    r.drawTriangle({kMenuX - 10.f, kDotsY + 1.f}, {kMenuX + 10.f, kDotsY + 1.f}, {kMenuX, kDotsY - 9.f},
                   nxui::Color(0.08f, 0.09f, 0.13f, 0.86f * a)); // pointing at the button
    if (s < 0.999f) r.pushClipRect(p);
    textMid(r, m_fPill, "Pages", full.x + kDotsPad + 4.f, full.y + kDotsPad + 12.f - 10.f * (1.f - a), kMuted.withAlpha(a), 0.9f);
    // The pages, in your order: the one you're on filled.
    for (int pos = 0; pos < kPageCount; ++pos) {
        nxui::Rect b = dotsPage(full, pos);
        b.y -= 10.f * (1.f - a);
        const int page = m_settings.pageOrder[(size_t)pos];
        const bool here = page == m_page, focused = m_dotsRow == 0 && m_dotsCol == pos;
        r.drawRoundedRect(b, here ? kWhite.withAlpha(0.92f * a) : nxui::Color(1.f, 1.f, 1.f, 0.08f * a), b.height * 0.5f);
        if (focused) r.drawRoundedRectOutline({b.x - 4.f, b.y - 4.f, b.width + 8.f, b.height + 8.f}, accent(ring * a),
                                              b.height * 0.5f + 4.f, 2.5f);
        const std::string n = kPageNames[page];
        const float ns = fitScale(m_fPill, n, b.width - 14.f, 0.75f);
        textMid(r, m_fPill, n, b.x + (b.width - measured(m_fPill, n).x * ns) * 0.5f, b.y + b.height * 0.5f,
                here ? nxui::Color(0.f, 0.f, 0.f, a) : kWhite.withAlpha(a), ns);
    }
    // The tiles: an icon on a disc of its colour, its name below; they drop in one by one.
    for (size_t i = 0; i < m_dotsTiles.size(); ++i) {
        const DotsTile& t = m_dotsTiles[i];
        const float ta = a * easeOut((m_dotsIn - 0.04f * static_cast<float>(i)) / 0.7f);
        nxui::Rect b = dotsTile(full, static_cast<int>(i));
        b.y -= 14.f * (1.f - ta);
        const bool focused = m_dotsRow == 1 + static_cast<int>(i) / kDotsCols && m_dotsCol == static_cast<int>(i) % kDotsCols;
        if (focused) b = scaled(b, 1.05f);
        r.drawRoundedRect(b, nxui::Color(1.f, 1.f, 1.f, (focused ? 0.14f : 0.06f) * ta), 20.f);
        r.drawRoundedRectOutline(b, focused ? accent(ring * ta) : nxui::Color(1.f, 1.f, 1.f, 0.10f * ta), 20.f, focused ? 2.5f : 1.f);
        const nxui::Vec2 c{b.x + b.width * 0.5f, b.y + 40.f};
        r.drawCircle(c, 25.f, t.tone.withAlpha(0.9f * ta), 36);
        icon(r, t.glyph, c.x, c.y, 26.f, kWhite.withAlpha(ta));
        const std::string n = t.name;
        textMid(r, m_fBody, n, b.x + (b.width - measured(m_fBody, n).x * 0.9f) * 0.5f, b.y + 84.f, kWhite.withAlpha(ta), 0.9f);
    }
    if (s < 0.999f) r.popClipRect();
}

void CardsActivity::openPower() {
    m_powerItems = powerItems();
    m_powerSel = 0;
    m_powerOpen = true;
    m_powerIn = m_settings.motion ? 0.f : 1.f;
    for (float& l : m_powerLift) l = 0.f;
    m_audio.play("show_modal");
}

namespace {
const nxui::Rect kPowerCard{208.f, 200.f, 864.f, 320.f};
constexpr float kPowerTileW = 192.f, kPowerTileH = 212.f, kPowerTileGap = 16.f;
nxui::Rect powerTile(int i) {
    const float x0 = kPowerCard.x + (kPowerCard.width - (kPowerTileW * 4.f + kPowerTileGap * 3.f)) * 0.5f;
    return {x0 + (kPowerTileW + kPowerTileGap) * static_cast<float>(i), kPowerCard.y + 82.f, kPowerTileW, kPowerTileH};
}
struct PowerLook {
    const char* name;
    const char* about;
    const char* glyph;
    nxui::Color tone;
};
const PowerLook kPowerLooks[4] = {
    {"Sleep", "Rest, back where you were", "moon", nxui::Color(0.38f, 0.56f, 1.f, 1.f)},
    {"Restart", "Off and on again", "restart", nxui::Color(0.25f, 0.82f, 0.68f, 1.f)},
    {"Power Off", "Turn the console off", "power", nxui::Color(1.f, 0.36f, 0.36f, 1.f)},
    {"Hekate", "Hekate's menu", "hekate", nxui::Color(0.70f, 0.50f, 1.f, 1.f)},
};
} // namespace

void CardsActivity::updatePower() {
    auto& in = app().input();
    using B = nxui::Button;
    const int n = std::min<int>(4, static_cast<int>(m_powerItems.size()));
    const bool left = in.isDown(B::DLeft) || in.isDown(B::LStickL), right = in.isDown(B::DRight) || in.isDown(B::LStickR);
    if (left && m_powerSel > 0) { --m_powerSel; m_audio.play("navigation"); }
    if (right && m_powerSel < n - 1) { ++m_powerSel; m_audio.play("navigation"); }
    if (in.isDown(B::B) || in.isDown(B::Plus)) {
        m_powerOpen = false;
        m_audio.play("hide_modal");
        return;
    }
    if (in.isDown(B::A) && m_powerSel < n) {
        auto run = m_powerItems[(size_t)m_powerSel].run;
        m_powerOpen = false;
        m_audio.play("activation");
        if (run) run();
    }
}

bool CardsActivity::powerTouch() {
    auto& in = app().input();
    if (!in.touchUp()) return in.isTouching();
    const float x = in.touchX(), y = in.touchY();
    for (int i = 0; i < std::min<int>(4, static_cast<int>(m_powerItems.size())); ++i)
        if (powerTile(i).contains(x, y)) {
            m_powerSel = i;
            auto run = m_powerItems[(size_t)i].run;
            m_powerOpen = false;
            m_audio.play("activation");
            if (run) run();
            return true;
        }
    if (!kPowerCard.contains(x, y)) {
        m_powerOpen = false;
        m_audio.play("hide_modal");
    }
    return true;
}

void CardsActivity::drawPower(nxui::Renderer& r) {
    if (!m_powerOpen) return;
    const float a = easeOut(m_powerIn);
    const float ring = 0.8f + 0.2f * std::sin(m_time * 5.f);
    r.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.5f * a));
    const nxui::Rect card = scaled(kPowerCard, 0.94f + 0.06f * easeOutBack(m_powerIn));
    frost(r, card, 34.f, nxui::Color(0.08f, 0.09f, 0.13f, 0.86f * a));
    r.drawRoundedRectOutline(card, nxui::Color(1.f, 1.f, 1.f, 0.16f * a), 34.f, 1.5f);
    r.drawGradientRect({card.x + 2.f, card.y + 2.f, card.width - 4.f, 90.f}, nxui::Color(1.f, 1.f, 1.f, 0.06f * a),
                       nxui::Color(1.f, 1.f, 1.f, 0.f));
    const float dy = card.y - kPowerCard.y;
    icon(r, "power", kPowerCard.x + 46.f, kPowerCard.y + 44.f + dy, 30.f, kWhite.withAlpha(a));
    textMid(r, m_fTitle, "Power", kPowerCard.x + 72.f, kPowerCard.y + 44.f + dy, kWhite.withAlpha(a));
    const std::string hint = "A Choose    B Back";
    textMid(r, m_fBody, hint, kPowerCard.x + kPowerCard.width - 36.f - measured(m_fBody, hint).x, kPowerCard.y + 44.f + dy,
            kMuted.withAlpha(0.8f * a));
    const int n = std::min<int>(4, static_cast<int>(m_powerItems.size()));
    for (int i = 0; i < n; ++i) {
        // The chosen one rises a little and glows in its colour.
        float& lift = m_powerLift[i];
        const float want = i == m_powerSel ? 1.f : 0.f;
        lift += (want - lift) * (m_settings.motion ? 0.25f : 1.f);
        const PowerLook& look = kPowerLooks[i];
        nxui::Rect tile = scaled(powerTile(i), 1.f + 0.05f * lift);
        tile.y += dy - 8.f * lift + 30.f * (1.f - easeOut((m_powerIn - 0.06f * i) / 0.8f)); // they rise in, one by one
        const float ta = a * easeOut((m_powerIn - 0.06f * i) / 0.8f);
        r.drawRoundedRect({tile.x - 2.f, tile.y + 8.f + 6.f * lift, tile.width + 4.f, tile.height + 4.f},
                          nxui::Color(0.f, 0.f, 0.f, (0.18f + 0.12f * lift) * ta), 28.f);
        frost(r, tile, 26.f, nxui::Color(0.12f, 0.14f, 0.19f, (0.6f + 0.15f * lift) * ta));
        r.drawGradientRect({tile.x + 2.f, tile.y + 2.f, tile.width - 4.f, tile.height * 0.55f},
                           look.tone.withAlpha(0.20f * lift * ta), look.tone.withAlpha(0.f));
        r.drawRoundedRectOutline(tile, lift > 0.5f ? accent(ring * ta) : nxui::Color(1.f, 1.f, 1.f, 0.12f * ta), 26.f,
                                 lift > 0.5f ? 3.f : 1.2f);
        // Its icon on a disc of its colour.
        const nxui::Vec2 c{tile.x + tile.width * 0.5f, tile.y + 78.f};
        r.drawCircle(c, 44.f + 4.f * lift, look.tone.withAlpha((0.22f + 0.15f * lift) * ta), 40);
        r.drawCircle(c, 34.f, look.tone.withAlpha(0.9f * ta), 40);
        icon(r, look.glyph, c.x, c.y, 34.f, kWhite.withAlpha(ta));
        const std::string name = look.name;
        textMid(r, m_fLabel, name, tile.x + (tile.width - measured(m_fLabel, name).x) * 0.5f, tile.y + 150.f, kWhite.withAlpha(ta));
        const std::string about = fit(m_fBody, look.about, tile.width - 20.f, 0.85f);
        textMid(r, m_fBody, about, tile.x + (tile.width - measured(m_fBody, about).x * 0.85f) * 0.5f, tile.y + 180.f,
                kMuted.withAlpha(0.85f * ta), 0.85f);
    }
}

// R: Quick Settings, as HomeNX's (the status, the music player, volumes and
// brightness, Airplane mode and Wi-Fi, power) in CardsNX's glass.
void CardsActivity::openQuick() {
    m_quick = readQuickStatus();
    m_quickRefreshIn = 3.f;
    m_quickPower = powerItems();
    m_quickRow = kQPlayer;
    m_quickCol = 1; // play / pause
    m_quickPlaylist = false;
    m_quickOpen = true;
    m_quickIn = m_settings.motion ? 0.f : 1.f;
    m_audio.play("show_modal");
}

void CardsActivity::quickActivate(int row, int col) {
    if (row == kQPlayer) {
        switch (col) {
            case 0: m_audio.skip(-1); m_settings.music = true; break;
            case 1:
                if (!m_settings.music) {
                    m_settings.music = true;
                    m_audio.setMusic(true, m_settings.musicVolume);
                } else {
                    m_audio.togglePause();
                }
                break;
            case 2: m_audio.skip(1); m_settings.music = true; break;
            case 3:
                m_settings.repeat = !m_settings.repeat;
                m_audio.setRepeat(m_settings.repeat);
                break;
            case 4:
                m_settings.shuffle = !m_settings.shuffle;
                m_audio.setShuffle(m_settings.shuffle);
                break;
            default:
                m_quickPlaylist = true;
                m_quickListIn = m_settings.motion ? 0.f : 1.f;
                m_quickListSel = std::max(0L, m_audio.playing());
                break;
        }
        m_settings.save();
        m_audio.play("activation");
    } else if (row == kQAirplane) {
        if (setAirplaneMode(!m_quick.airplane)) m_quick.airplane = !m_quick.airplane;
        else say("Airplane mode didn't change.");
        m_audio.play(m_quick.airplane ? "toggle_on" : "toggle_off");
    } else if (row == kQWifi) {
        const bool on = !(m_quick.wifi && !m_quick.airplane);
        if (setWifi(on)) {
            m_quick.wifi = on;
            if (on) m_quick.airplane = false;
        } else {
            say("Wi-Fi didn't change.");
        }
        m_audio.play(on ? "toggle_on" : "toggle_off");
    } else if (row == kQPower && col >= 0 && col < static_cast<int>(m_quickPower.size())) {
        auto run = m_quickPower[(size_t)col].run;
        m_quickOpen = false;
        if (run) run();
    }
}

void CardsActivity::quickChange(int row, int delta) {
    if (row == kQMusic) {
        m_settings.musicVolume = std::clamp(m_settings.musicVolume + delta * 10, 0, 100);
        m_audio.setMusic(m_settings.music, m_settings.musicVolume);
    } else if (row == kQSounds) {
        m_settings.soundVolume = std::clamp(m_settings.soundVolume + delta * 10, 0, 100);
        m_audio.setSounds(m_settings.sounds, m_settings.soundVolume);
    } else if (row == kQBrightness && m_quick.brightness >= 0.f) {
        m_quick.brightness = std::clamp(m_quick.brightness + static_cast<float>(delta) * 0.1f, 0.05f, 1.f);
        setBrightness(m_quick.brightness);
    } else {
        return;
    }
    m_settings.save();
    m_audio.play("navigation");
}

void CardsActivity::updateQuick() {
    auto& in = app().input();
    using B = nxui::Button;
    const bool up = in.isDown(B::DUp) || in.isDown(B::LStickU), down = in.isDown(B::DDown) || in.isDown(B::LStickD);
    const bool left = in.isDown(B::DLeft) || in.isDown(B::LStickL), right = in.isDown(B::DRight) || in.isDown(B::LStickR);
    if ((m_quickRefreshIn -= 1.f / 60.f) <= 0.f) { // the battery and the chip's temperature
        m_quickRefreshIn = 3.f;
        u32 charge = 0;
        if (R_SUCCEEDED(psmGetBatteryChargePercentage(&charge))) m_quick.battery = static_cast<int>(charge);
        PsmChargerType charger = PsmChargerType_Unconnected;
        if (R_SUCCEEDED(psmGetChargerType(&charger))) m_quick.charging = charger != PsmChargerType_Unconnected;
    }
    if (m_quickPlaylist) { // the playlist: choose a song
        const int n = static_cast<int>(m_audio.titles().size());
        if (up && m_quickListSel > 0) { --m_quickListSel; m_audio.play("navigation"); }
        if (down && m_quickListSel < n - 1) { ++m_quickListSel; m_audio.play("navigation"); }
        if (in.isDown(B::A) && n > 0) {
            m_settings.music = true;
            m_settings.save();
            m_audio.playTrack(static_cast<size_t>(m_quickListSel));
            m_audio.play("activation");
        }
        if (in.isDown(B::B) || left) {
            m_quickPlaylist = false;
            m_audio.play("hide_modal");
        }
        return;
    }
    if (in.isDown(B::B) || in.isDown(B::R) || in.isDown(B::Plus)) {
        m_quickOpen = false;
        m_audio.play("hide_modal");
        return;
    }
    if (up && m_quickRow > 0) { --m_quickRow; m_audio.play("navigation"); }
    if (down && m_quickRow < kQRows - 1) { ++m_quickRow; m_audio.play("navigation"); }
    const int cols = m_quickRow == kQPlayer ? kQPlayerButtons : m_quickRow == kQPower ? kQPowerButtons : 1;
    m_quickCol = std::clamp(m_quickCol, 0, cols - 1);
    if (left || right) {
        if (cols > 1) {
            const int next = std::clamp(m_quickCol + (left ? -1 : 1), 0, cols - 1);
            if (next != m_quickCol) m_audio.play("navigation");
            m_quickCol = next;
        } else {
            quickChange(m_quickRow, left ? -1 : 1);
        }
    }
    if (in.isDown(B::A)) quickActivate(m_quickRow, m_quickCol);
}

// Touch: a button, a switch, a slider (where the finger is), a song; beside it closes.
bool CardsActivity::quickTouch() {
    auto& in = app().input();
    if (!in.touchUp()) return in.isTouching();
    const float x = in.touchX(), y = in.touchY();
    const nxui::Rect p = quickPanel(0.f);
    if (m_quickPlaylist) {
        const nxui::Rect list = quickPlaylist(p, m_audio.titles().size());
        if (list.contains(x, y)) {
            const int row = static_cast<int>((y - list.y - 56.f) / 46.f);
            if (y >= list.y + 56.f && row >= 0 && row < static_cast<int>(m_audio.titles().size())) {
                m_quickListSel = row;
                m_settings.music = true;
                m_settings.save();
                m_audio.playTrack(static_cast<size_t>(row));
            }
            return true;
        }
        m_quickPlaylist = false;
        if (!p.contains(x, y)) return true;
    }
    if (!p.contains(x, y)) {
        m_quickOpen = false;
        m_audio.play("hide_modal");
        return true;
    }
    for (int i = 0; i < kQPlayerButtons; ++i) {
        const nxui::Vec2 c = quickPlayerButton(p, i);
        if ((x - c.x) * (x - c.x) + (y - c.y) * (y - c.y) <= 28.f * 28.f) {
            m_quickRow = kQPlayer;
            m_quickCol = i;
            quickActivate(kQPlayer, i);
            return true;
        }
    }
    for (int i = 0; i < 3; ++i) {
        const nxui::Rect row{p.x + kQuickPad, p.y + kQSliderY[i], quickInnerW(), kQSliderH};
        if (!row.contains(x, y)) continue;
        m_quickRow = kQMusic + i;
        const float barX = row.x + 52.f, barW = row.width - 72.f;
        const int want = static_cast<int>(std::lround(clamp01((x - barX) / barW) * 10.f)) * 10;
        const int now = i == 0 ? m_settings.musicVolume : i == 1 ? m_settings.soundVolume
                                                                 : static_cast<int>(std::lround(m_quick.brightness * 100.f));
        if (want != now) quickChange(kQMusic + i, (want - now) / 10);
        return true;
    }
    for (int i = 0; i < 2; ++i)
        if (nxui::Rect{p.x + kQuickPad, p.y + kQToggleY[i], quickInnerW(), kQToggleH}.contains(x, y)) {
            m_quickRow = kQAirplane + i;
            quickActivate(kQAirplane + i, 0);
            return true;
        }
    for (int i = 0; i < kQPowerButtons; ++i)
        if (quickPowerButton(p, i).contains(x, y)) {
            m_quickRow = kQPower;
            m_quickCol = i;
            quickActivate(kQPower, i);
            return true;
        }
    return true;
}

void CardsActivity::openMusic() {
    std::vector<MenuItem> items;
    items.push_back(info("Now playing", [this]() {
        const std::string t = m_audio.trackName();
        return m_settings.music && !t.empty() ? t : std::string("Nothing");
    }));
    items.push_back(toggle("Music", [this]() { return m_settings.music; }, [this]() {
        m_settings.music = !m_settings.music;
        m_audio.setMusic(m_settings.music, m_settings.musicVolume);
        m_settings.save();
    }));
    MenuItem next = action("Next song", [this]() {
        m_settings.music = true;
        m_audio.skip(1);
        m_settings.save();
    });
    next.stay = true;
    items.push_back(next);
    MenuItem prev = action("Previous song", [this]() {
        m_settings.music = true;
        m_audio.skip(-1);
        m_settings.save();
    });
    prev.stay = true;
    items.push_back(prev);
    items.push_back(toggle("Shuffle", [this]() { return m_settings.shuffle; }, [this]() {
        m_settings.shuffle = !m_settings.shuffle;
        m_audio.setShuffle(m_settings.shuffle);
        m_settings.save();
    }));
    items.push_back(toggle("Repeat this song", [this]() { return m_settings.repeat; }, [this]() {
        m_settings.repeat = !m_settings.repeat;
        m_audio.setRepeat(m_settings.repeat);
        m_settings.save();
    }));
    items.push_back(slider("Volume", [this]() { return m_settings.musicVolume; }, [this](int v) {
        m_settings.musicVolume = v;
        m_audio.setMusic(m_settings.music, v);
        m_settings.save();
    }));
    items.push_back(action("Done", [] {}));
    openMenu("Music", std::move(items));
}

// The pill's picture: who games start as; add or remove users.
void CardsActivity::openUsers() {
    std::vector<User> users = listUsers();
    app().gpu().waitIdle(); // the last popup's pictures may still be drawing
    m_userPictures.clear();
    std::vector<MenuItem> items;
    for (const User& u : users) {
        auto tex = std::make_unique<nxui::Texture>();
        const nxui::Texture* pic = nullptr;
        if (!u.picture.empty() && tex->loadFromMemory(app().gpu(), app().renderer(), u.picture.data(), u.picture.size(), 96)) {
            pic = tex.get();
            m_userPictures.push_back(std::move(tex));
        }
        const AccountUidLike uid = u.uid;
        const std::string name = u.name;
        MenuItem row = action(name, [this, uid, name]() {
            setUser(uid);
            m_audio.play("toggle_on");
            say("Games start as " + name + " now.");
        });
        row.picture = pic;
        row.value = [this, uid]() {
            return uid.uid[0] == m_user.uid[0] && uid.uid[1] == m_user.uid[1] ? std::string("Playing") : std::string();
        };
        items.push_back(std::move(row));
    }
    items.push_back(action("Add a user", [this]() {
#ifdef SWITCHU_MENU
        leave(-1, 0.4f, [this]() { m_launcher.launchUserCreator(); });
#else
        m_audio.pause();
        const Result rc = pselShowUserCreator();
        m_audio.resume();
        if (R_FAILED(rc)) say("Adding users works when CardsNX runs over a game.");
        else openUsers();
#endif
    }));
    if (!users.empty()) {
        items.push_back(action("Remove a user", [this, users]() {
            std::vector<MenuItem> who;
            for (const User& u : users) {
                const AccountUidLike uid = u.uid;
                const std::string name = u.name;
                who.push_back(action(name, [this, uid, name]() {
                    openMenu("Remove " + name + "?", {
                        info("Back up their saves first", [] { return std::string(); }),
                        action("Remove " + name + " from this console", [this, uid, name]() {
                            std::string error;
                            if (!deleteUser(uid, error)) {
                                say(error);
                                return;
                            }
                            m_audio.play("toggle_off");
                            say(name + " was removed.");
                            if (uid.uid[0] == m_user.uid[0] && uid.uid[1] == m_user.uid[1]) {
                                const std::vector<User> left = listUsers();
                                setUser(left.empty() ? AccountUidLike{} : left.front().uid);
                            }
                        }, true),
                        action("Cancel", [] {}),
                    });
                }, true));
            }
            who.push_back(action("Cancel", [] {}));
            openMenu("Remove which user?", std::move(who));
        }, true));
    }
    items.push_back(action("Cancel", [] {}));
    openMenu("Who's playing?", std::move(items));
}

void CardsActivity::openNotifications() {
    m_notifRows.clear();
    for (const auto& it : notifications::recent(6))
        m_notifRows.push_back({(it.kind.rfind("discord", 0) == 0 ? "Discord - " : "") + plain(it.title), plain(it.text)});
    notifications::markSeen();
    m_unread = 0;
    m_notifOpen = true;
    m_menuIn = m_settings.motion ? 0.f : 1.f;
    m_audio.play("show_modal");
}

void CardsActivity::say(const std::string& message) {
    m_message = plain(message);
    m_messageFor = 4.5f;
}

void CardsActivity::press(int zone, int index) {
    m_pressZone = zone;
    m_pressIndex = index;
    m_pressT = 0.f;
}

// ---------------------------------------------------------------- settings

void CardsActivity::buildSettings() {
    auto save = [this]() { m_settings.save(); };
    m_sheet.clear();

    Section home{"Home", "home", {}};
    MenuItem order = action("Page order", [this]() {
        m_sheetOpen = false;
        openPageOrder(0);
    });
    order.value = [this]() {
        std::string s;
        for (int p : m_settings.pageOrder) s += (s.empty() ? "" : ", ") + std::string(kPageNames[p]);
        return s;
    };
    order.detail = "CardsNX opens on the first; ZL / ZR go in this order";
    home.rows.push_back(std::move(order));
    home.rows.push_back(choice("Sort games", [this]() { return std::string(kSortNames[m_settings.sort]); },
        [this, save](int d) {
            int at = 0;
            for (int i = 0; i < 4; ++i)
                if (kSortCycle[i] == m_settings.sort) at = i;
            sortGames(kSortCycle[(at + d + 4) % 4]);
            save();
        }, "My Order is how you arranged them (Plus > Move)"));
    home.rows.push_back(toggle("Nextendo players", [this]() { return m_settings.players; },
        [this, save]() { m_settings.players = !m_settings.players; save(); }, "Green dots on games played online now"));
    home.rows.push_back(toggle("Animations", [this]() { return m_settings.motion; },
        [this, save]() { m_settings.motion = !m_settings.motion; save(); }));
    home.rows.push_back(action("Reset my order", [this]() {
        for (auto& o : m_settings.order) o.clear();
        m_settings.save();
        auto& homebrew = m_pages[kHomebrew];
        std::stable_sort(homebrew.begin(), homebrew.end(), [](const Game& a, const Game& b) { return a.order < b.order; });
        m_pages[kNintendo] = listSystem();
        sortGames(1, false);
        m_audio.play("toggle_off");
        say("Every page is back in its usual order.");
    }));
    m_sheet.push_back(std::move(home));

    Section look{"Look", "gear", {}};
    look.rows.push_back(choice("Highlight colour",
        [this]() { return std::string(kAccents[std::clamp(m_settings.accent, 0, kAccentCount - 1)].name); },
        [this, save](int d) { m_settings.accent = (m_settings.accent + d + kAccentCount) % kAccentCount; save(); },
        "The ring around what's chosen"));
    look.rows.push_back(choice("Launch animation",
        [this]() { return std::string(kLaunchNames[std::clamp(m_settings.launchStyle, 0, kLaunchStyles - 1)]); },
        [this, save](int d) {
            m_settings.launchStyle = (m_settings.launchStyle + d + kLaunchStyles) % kLaunchStyles;
            save();
        }, "When a game starts"));
    look.rows.push_back(choice("Glass", [this]() { return std::string(kGlassNames[std::clamp(m_settings.glass, 0, 2)]); },
        [this, save](int d) { m_settings.glass = (m_settings.glass + d + 3) % 3; save(); }, "Buttons, the bar and the panel"));
    look.rows.push_back(slider("Background softness", [this]() { return m_settings.haze * 100 / 80; },
        [this, save](int v) { m_settings.haze = v * 80 / 100; save(); }, "A haze over the game's art"));
    look.rows.push_back(toggle("Moving background", [this]() { return m_settings.bgMotion; },
        [this, save]() { m_settings.bgMotion = !m_settings.bgMotion; save(); }, "The art drifts slowly"));
    look.rows.push_back(choice("Card pictures",
        [this]() { return std::string(m_settings.boxArt ? "Box covers" : "eShop banners"); },
        [this, save](int) {
            m_settings.boxArt = !m_settings.boxArt;
            m_boxArt = m_settings.boxArt;
            save();
            forgetAllArt();
        }));
    look.rows.push_back(choice("Clock", [this]() { return std::string(m_settings.clock24 ? "24-hour" : "12-hour"); },
        [this, save](int) {
            m_settings.clock24 = !m_settings.clock24;
            m_clock24 = m_settings.clock24;
            save();
        }));
    look.rows.push_back(toggle("Title pictures", [this]() { return m_settings.titlePics; }, [this, save]() {
        m_settings.titlePics = !m_settings.titlePics;
        m_titleArt = m_settings.titlePics;
        save();
        for (auto& [id, a] : m_art) { // the icons again, the other kind
            a.icon.reset();
            a.iconAsked = false;
        }
        wantArt();
    }, "The game's own picture, no background (SteamGridDB)"));
    look.rows.push_back(toggle("Free space in the bar", [this]() { return m_settings.showFree; },
        [this, save]() { m_settings.showFree = !m_settings.showFree; save(); }));
    m_sheet.push_back(std::move(look));

    Section sound{"Sound", "music", {}};
    sound.rows.push_back(toggle("Music", [this]() { return m_settings.music; }, [this, save]() {
        m_settings.music = !m_settings.music;
        m_audio.setMusic(m_settings.music, m_settings.musicVolume);
        save();
    }, "HomeNX's HOME music"));
    sound.rows.push_back(slider("Music volume", [this]() { return m_settings.musicVolume; }, [this, save](int v) {
        m_settings.musicVolume = v;
        m_audio.setMusic(m_settings.music, v);
        save();
    }));
    MenuItem next = action("Next song", [this, save]() {
        m_settings.music = true;
        m_audio.skip(1);
        save();
    });
    next.value = [this]() { return m_audio.trackName(); };
    sound.rows.push_back(next);
    sound.rows.push_back(toggle("Shuffle", [this]() { return m_settings.shuffle; }, [this, save]() {
        m_settings.shuffle = !m_settings.shuffle;
        m_audio.setShuffle(m_settings.shuffle);
        save();
    }));
    sound.rows.push_back(toggle("Repeat song", [this]() { return m_settings.repeat; }, [this, save]() {
        m_settings.repeat = !m_settings.repeat;
        m_audio.setRepeat(m_settings.repeat);
        save();
    }, "Play the same song again"));
    sound.rows.push_back(toggle("Sounds", [this]() { return m_settings.sounds; }, [this, save]() {
        m_settings.sounds = !m_settings.sounds;
        m_audio.setSounds(m_settings.sounds, m_settings.soundVolume);
        save();
    }, "Moving, choosing and opening things"));
    sound.rows.push_back(slider("Sound volume", [this]() { return m_settings.soundVolume; }, [this, save](int v) {
        m_settings.soundVolume = v;
        m_audio.setSounds(m_settings.sounds, v);
        save();
    }));
    m_sheet.push_back(std::move(sound));

    // The console's own settings, as HomeNX's Settings has them (Storage stays there).
    Section net{"Internet", "wifi", {}};
    net.rows.push_back(toggle("Wi-Fi", [this]() { return m_quick.wifi; }, [this]() {
        if (setWifi(!m_quick.wifi)) m_quick.wifi = !m_quick.wifi;
    }));
#ifdef SWITCHU_MENU
    MenuItem network = action("Network", [this]() {
        m_sheetOpen = false;
        leave(sel(), 0.75f, [this]() {
            m_discord.beginShutdown();
            m_launcher.launchNetConnect();
        });
    });
    network.detail = "Choose another in Nintendo's Internet settings";
#else
    MenuItem network = info("Network", nullptr);
#endif
    network.value = [this]() { return m_console.network.empty() ? std::string("Not connected") : m_console.network; };
    net.rows.push_back(std::move(network));
    net.rows.push_back(info("IP address", [this]() { return m_console.ip.empty() ? std::string("None") : m_console.ip; }));
    MenuItem netTest = action("Connection test", [this]() { startConnectionTest(); });
    netTest.value = [this]() { return netTestText(0); };
    netTest.detail = "Ping, download and upload speed";
    net.rows.push_back(std::move(netTest));
    for (int row : {4, 1, 2, 3})
        net.rows.push_back(info(row == 4 ? "Signal" : row == 1 ? "Ping" : row == 2 ? "Download" : "Upload",
                                [this, row]() { return netTestText(row); }));
    m_sheet.push_back(std::move(net));

    Section pads{"Controllers", "pad", {}};
#ifdef SWITCHU_MENU
    MenuItem pair = action("Pair a controller", [this]() { startPairing(); });
    pair.value = [this]() {
        return m_pairing ? "Looking... " + std::to_string(static_cast<int>(m_pairingLeft + 0.99f)) + " s" : std::string();
    };
    pair.detail = "Then hold the SYNC button on the controller";
    pads.rows.push_back(std::move(pair));
#endif
    MenuItem find = action("Find controllers", [this]() { findControllersNow(); });
    find.detail = "Every controller rumbles three times";
    pads.rows.push_back(std::move(find));
#ifdef SWITCHU_MENU
    MenuItem padTest = action("Test buttons and sticks", [this]() { openPadTest(); });
    padTest.detail = "Sticks, buttons and the touch screen";
    pads.rows.push_back(std::move(padTest));
    auto nintendo = [this](void (AppletLauncher::*open)()) -> std::function<void()> {
        return [this, open]() {
            m_sheetOpen = false;
            leave(sel(), 0.75f, [this, open]() {
                m_discord.beginShutdown();
                (m_launcher.*open)();
            });
        };
    };
    MenuItem grip = action("Change grip/order", nintendo(&AppletLauncher::launchControllerPairing));
    MenuItem remap = action("Button mapping", nintendo(&AppletLauncher::launchControllerRemapping));
#else
    auto nintendo = [this](const char* what) -> std::function<void()> {
        return [this, what]() {
            m_audio.pause();
            std::string error;
            if (!openSystem(what, m_user, error)) say(error);
            m_audio.resume();
        };
    };
    MenuItem grip = action("Pair, grip and order", nintendo("controllers"));
    MenuItem remap = action("Button mapping", nintendo("remap"));
#endif
    grip.detail = remap.detail = "Nintendo's screen";
    pads.rows.push_back(std::move(grip));
    pads.rows.push_back(std::move(remap));
    pads.rows.push_back(slider("Vibration", [this]() { return static_cast<int>(m_console.vibration * 100.f + 0.5f); },
        [this](int v) {
            m_console.vibration = static_cast<float>(v) / 100.f;
            writeConsoleSettings(m_console);
        }));
    pads.rows.push_back(toggle("Pro Controller over USB", [this]() { return m_console.proWired; }, [this]() {
        m_console.proWired = !m_console.proWired;
        writeConsoleSettings(m_console);
    }, "Wired when it's plugged in"));
    m_sheet.push_back(std::move(pads));

    Section console{"Console", "moon", {}};
    static const char* const kHandheldSleep[] = {"1 minute", "3 minutes", "5 minutes", "10 minutes", "30 minutes", "Never"};
    static const char* const kTvSleep[] = {"1 hour", "2 hours", "3 hours", "6 hours", "12 hours", "Never"};
    console.rows.push_back(choice("Sleep in handheld", [this]() { return std::string(kHandheldSleep[m_console.handheldSleep]); },
        [this](int d) {
            m_console.handheldSleep = (m_console.handheldSleep + d + 6) % 6;
            writeConsoleSettings(m_console);
        }, "After this long with nothing pressed"));
    console.rows.push_back(choice("Sleep on the TV", [this]() { return std::string(kTvSleep[m_console.tvSleep]); },
        [this](int d) {
            m_console.tvSleep = (m_console.tvSleep + d + 6) % 6;
            writeConsoleSettings(m_console);
        }));
    console.rows.push_back(toggle("Auto-brightness", [this]() { return m_console.autoBrightness; }, [this]() {
        m_console.autoBrightness = !m_console.autoBrightness;
        writeConsoleSettings(m_console);
    }, "In handheld it follows the room's light"));
    console.rows.push_back(info("System version", [this]() { return m_console.firmware.empty() ? std::string("-") : m_console.firmware; }));
    console.rows.push_back(info("Atmosphere", [this]() { return m_console.atmosphere.empty() ? std::string("-") : m_console.atmosphere; }));
    m_sheet.push_back(std::move(console));

    Section pictures{"Pictures", "tag", {}};
    pictures.rows.push_back(action("Download all pictures again", [this]() {
        forgetAllArt();
        say("Pictures will download again.");
    }));
    pictures.rows.push_back(info("SteamGridDB key", [this]() {
        return m_key.empty() ? std::string("None (HomeNX's Settings)") : std::string("Set");
    }));
    m_sheet.push_back(std::move(pictures));

    Section updates{"Updates", "sort", {}};
#ifdef SWITCHU_MENU
    updates.rows.push_back(info("Installed", []() { return switchu::manager::ReleaseUpdater::installedVersion(); }));
#else
    updates.rows.push_back(info("CardsNX", []() { return std::string(HOMENX_BUILD); }));
#endif
    MenuItem upd = action("HomeNX updates", [this]() { runUpdate(); });
    upd.value = [this]() { return updateRowLabel(); };
    upd.detail = "Downloads the newest HomeNX and CardsNX";
    updates.rows.push_back(std::move(upd));
    MenuItem hb = action("Homebrew updates", [this]() {
        m_sheetOpen = false;
        openHomebrewUpdates();
    });
    hb.value = [this]() {
        if (m_updatesJob.valid()) return std::string("Checking...");
        int n = 0;
        for (Page p : {kHomebrew, kMedia}) for (const Game& g : m_pages[p]) n += nroUpdate(g) ? 1 : 0;
        return n ? std::to_string(n) + " to update" : std::string("Up to date");
    };
    hb.detail = "Newer versions of your .nro apps";
    updates.rows.push_back(std::move(hb));
    updates.rows.push_back(action("Home style (NXToggle)", [this]() {
        std::error_code ec;
        if (!std::filesystem::exists(kToggle, ec)) {
            say("NXToggle isn't there yet: restart the console once.");
            return;
        }
        openNro(kToggle);
    }));
    m_sheet.push_back(std::move(updates));

    Section about{"About", "dots", {}};
    about.rows.push_back(info("CardsNX", []() { return std::string(HOMENX_BUILD); }));
    about.rows.push_back(info("Games", [this]() { return std::to_string(m_pages[kGames].size()); }));
    about.rows.push_back(info("Homebrew", [this]() { return std::to_string(m_pages[kHomebrew].size()); }));
    about.rows.push_back(info("Free on the SD card", [this]() { return m_free; }));
    about.rows.push_back(toggle("Performance log", [this]() { return m_settings.perfLog; }, [this]() {
        m_settings.perfLog = !m_settings.perfLog;
        m_settings.save();
    }, "config/CardsNX/perf.log: slow frames and start-up times"));
#ifndef SWITCHU_MENU
    about.rows.push_back(action("Close CardsNX", [this]() {
        m_settings.save();
        app().requestExit();
    }));
#endif
    m_sheet.push_back(std::move(about));
}

void CardsActivity::startConnectionTest() {
    if (m_netTestJob.valid() && m_netTestJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    if (!m_netReady) {
        say("The network isn't up yet.");
        return;
    }
    m_netTest = std::make_shared<NetTest>();
    m_netTestJob = std::async(std::launch::async, [t = m_netTest]() {
        offScreenCore();
        testConnection(*t);
    });
    m_audio.play("activation");
}

std::string CardsActivity::netTestText(int row) {
    if (!m_netTest) return row == 0 ? "Test" : "-";
    const NetTest& t = *m_netTest;
    const int stage = t.stage;
    auto mbps = [](int kbps) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%.1f Mbps", static_cast<float>(kbps) / 1000.f);
        return std::string(buf);
    };
    switch (row) {
    case 0: {
        static const char* const kStages[] = {"Looking...", "Reaching the Internet...", "Downloading...", "Uploading..."};
        return stage < 4 ? kStages[stage] : t.internet ? "Test again" : "No Internet: test again";
    }
    case 1: return t.pingMs >= 0 ? std::to_string(t.pingMs.load()) + " ms" : stage <= 1 ? "..." : "-";
    case 2: return t.downKbps > 0 ? mbps(t.downKbps) : stage == 2 ? "..." : stage > 2 && t.internet ? "Couldn't test" : "-";
    case 3: return t.upKbps > 0 ? mbps(t.upKbps) : stage == 3 ? "..." : stage > 3 && t.internet ? "Couldn't test" : "-";
    default: {
        static const char* const kBars[] = {"Weak", "Fair", "Good", "Strong"};
        return t.signal >= 0 ? kBars[std::min(t.signal.load(), 3)] : stage == 0 ? "..." : "-";
    }
    }
}

void CardsActivity::findControllersNow() {
    if (m_findJob.valid() && m_findJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    m_findJob = std::async(std::launch::async, []() {
        offScreenCore();
        switchu::findControllers();
    });
    say("Every controller rumbles now.");
}

#ifdef SWITCHU_MENU
void CardsActivity::openPadTest() {
    ensureHxFonts();
    if (!m_padTest) {
        m_padTest = std::make_shared<ControllerTestScreen>();
        m_padTest->setFont(&m_hxFont);
        m_padTest->setSmallFont(&m_hxSmall);
        m_padTest->setTheme(&m_theme);
        m_padTest->setInput(&app().input());
        m_padTest->onCloseSfx([this]() { m_audio.play("hide_modal"); });
        rootBox().addChild(m_padTest);
    }
    if (m_padTest->isActive()) return;
    m_audio.play("show_modal");
    m_padTest->show();
    focusManager().setFocus(m_padTest.get());
}

// As HomeNX pairs: the console looks for a controller holding SYNC for 30 s.
void CardsActivity::startPairing() {
    if (m_pairing) {
        stopPairing();
        say("Stopped looking for controllers.");
        return;
    }
    u8 paired = 0;
    Result rc = btmsysInitialize();
    if (R_SUCCEEDED(rc)) {
        btmsysGetPairedGamepadCount(&paired);
        rc = btmsysStartGamepadPairing();
        if (R_FAILED(rc)) btmsysExit();
    }
    if (R_FAILED(rc)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Pairing couldn't start (0x%X).", rc);
        say(buf);
        return;
    }
    m_pairing = true;
    m_pairedBefore = paired;
    m_pairingLeft = 30.f;
    m_pairingPoll = 0.5f;
    m_audio.play("show_modal");
    say("Hold the SYNC button on the controller until its lights move.");
}

void CardsActivity::stopPairing() {
    if (!m_pairing) return;
    m_pairing = false;
    btmsysCancelGamepadPairing();
    btmsysExit();
}

void CardsActivity::updatePairing(float dt) {
    if (!m_pairing) return;
    m_pairingLeft -= dt;
    if ((m_pairingPoll -= dt) > 0.f && m_pairingLeft > 0.f) return;
    m_pairingPoll = 0.5f;
    u8 paired = 0;
    if (R_SUCCEEDED(btmsysGetPairedGamepadCount(&paired)) && paired > m_pairedBefore) {
        stopPairing();
        m_audio.play("toggle_on");
        say("The controller is paired.");
    } else if (m_pairingLeft <= 0.f) {
        stopPairing();
        say("No controller paired: try again and hold SYNC a little longer.");
    }
}
#endif

void CardsActivity::openSettings(int section) {
    m_console = readConsoleSettings();
    m_quick = readQuickStatus();
    buildSettings();
    m_sheetOpen = true;
    m_sheetSec = std::clamp(section, 0, static_cast<int>(m_sheet.size()) - 1);
    m_sheetRow = 0;
    m_sheetOnRows = false;
    m_sheetScroll = 0.f;
    m_sheetIn = m_settings.motion ? 0.f : 1.f;
    m_sheetRowsIn = 1.f;
    m_audio.play("show_modal");
}

std::string CardsActivity::updateRowLabel() {
#ifdef SWITCHU_MENU
    switch (static_cast<Update>(m_update.load())) {
        case Update::Checking: return "Checking...";
        case Update::UpToDate: return "Up to date";
        case Update::Available: {
            std::lock_guard<std::mutex> lk(m_mu);
            return "Install " + m_release.version;
        }
        case Update::Installing: {
            const bool downloading = m_updateStage.load() == static_cast<int>(switchu::manager::UpdateWorkerStage::Downloading);
            const int pct = static_cast<int>((downloading ? m_updateDownload.load() : m_updateInstall.load()) * 100.f);
            return (downloading ? "Downloading " : "Installing ") + std::to_string(std::clamp(pct, 0, 100)) + "%";
        }
        case Update::Installed: return "Restart to finish";
        case Update::Failed: return "Didn't work: try again";
        default: return "Check now";
    }
#else
    return "Opens NXToggle";
#endif
}

// Settings > Updates: check, then install right here (HomeNX's own updater;
// files that are open now are swapped in when the console restarts).
void CardsActivity::runUpdate() {
#ifdef SWITCHU_MENU
    using switchu::manager::ReleaseUpdater;
    const Update state = static_cast<Update>(m_update.load());
    if (state == Update::Checking || state == Update::Installing) return;
    if (state == Update::Installed) {
        leave(-1, 0.4f, [this]() { m_launcher.reboot(); });
        return;
    }
    if (m_updateJob.valid()) m_updateJob.get();
    if (state == Update::Available) {
        m_update = static_cast<int>(Update::Installing);
        m_updateDownload = 0.f;
        m_updateInstall = 0.f;
        m_updateJob = std::async(std::launch::async, [this]() {
            offScreenCore();
            switchu::manager::ReleaseInfo release;
            {
                std::lock_guard<std::mutex> lk(m_mu);
                release = m_release;
            }
            const auto result = ReleaseUpdater::install(release, false, m_updateDownload, m_updateInstall, m_updateStage);
            std::lock_guard<std::mutex> lk(m_mu);
            m_updateError = result.error;
            m_update = static_cast<int>(result.success ? Update::Installed : Update::Failed);
        });
        return;
    }
    m_update = static_cast<int>(Update::Checking);
    m_updateJob = std::async(std::launch::async, [this]() {
        offScreenCore();
        try {
            const auto release = ReleaseUpdater::checkLatest();
            std::lock_guard<std::mutex> lk(m_mu);
            m_release = release;
            m_update = static_cast<int>(release.updateAvailable ? Update::Available : Update::UpToDate);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(m_mu);
            m_updateError = e.what();
            m_update = static_cast<int>(Update::Failed);
        }
    });
#else
    // The .nro: NXToggle checks and installs (the autostart file asks it to).
    std::error_code ec;
    if (!std::filesystem::exists(kToggle, ec)) {
        say("NXToggle isn't on the SD card yet.");
        return;
    }
    std::filesystem::create_directories("sdmc:/config/SwitchU/update", ec);
    std::ofstream("sdmc:/config/SwitchU/update/autostart") << "1";
    openNro(kToggle);
#endif
}

// ---------------------------------------------------------------- News (HOME)

#ifdef SWITCHU_MENU
void CardsActivity::openNews() {
    if (m_newsOpen || m_newsJob.valid()) return; // open, or on its way
    if (!m_newsItems.empty() && m_time - m_newsLoadedAt < 120.f) { // read a moment ago
        m_newsOpen = true;
        m_newsIn = m_settings.motion ? 0.f : 1.f;
        m_audio.play("show_modal");
        showNewsItem(std::clamp(m_newsSel, 0, static_cast<int>(m_newsItems.size()) - 1));
        return;
    }
    m_newsWanted = true;
    say("Opening News...");
    // Reading ~40 articles takes a second or two: not on the screen's thread.
    m_newsJob = std::async(std::launch::async, [this]() {
        offScreenCore();
        requestNewsUpdate(120);
        Result rc = 0;
        NewsLoadStats stats;
        std::vector<NewsArticle> raw = loadNewsArticles(rc, 40, &stats);
        std::set<std::string> read;
        {
            std::ifstream in(kNewsReadPath);
            for (std::string line; std::getline(in, line);)
                if (!line.empty()) read.insert(line);
        }
        std::vector<NewsItem> items;
        for (auto& a : raw) {
            NewsItem n;
            n.id = a.id;
            n.title = a.subject.empty() ? a.topic : a.subject;
            n.topic = a.topic;
            n.body = a.body;
            n.footer = a.footer;
            if (a.publishedAt > 0) {
                const time_t t = static_cast<time_t>(a.publishedAt);
                char when[48];
                std::strftime(when, sizeof(when), "%d %B %Y", std::localtime(&t));
                n.topic += (n.topic.empty() ? "" : "   ") + std::string(when);
            }
            n.unread = !a.read && !read.count(a.id);
            if (!a.image.empty()) n.picture = std::make_shared<std::vector<std::uint8_t>>(std::move(a.image));
            items.push_back(std::move(n));
        }
        char code[16];
        std::snprintf(code, sizeof(code), "0x%X", rc);
        const std::string empty = R_FAILED(rc) ? "The console's news couldn't be read (" + std::string(code) + ")."
                                               : std::string("There's no news saved on this console yet.");
        std::lock_guard<std::mutex> lk(m_mu);
        m_newsReady = std::move(items);
        m_newsEmpty = empty;
    });
}

// An article on the right: its text wrapped once, marked read, its picture loaded.
void CardsActivity::showNewsItem(int index) {
    if (index < 0 || index >= static_cast<int>(m_newsItems.size())) return;
    m_newsSel = index;
    m_newsScroll = 0.f;
    NewsItem& n = m_newsItems[(size_t)index];
    if (n.lines.empty()) {
        n.titleLines = wrap(m_fLabel, n.title, kNewsTextW);
        n.lines = wrap(m_fBody, n.footer.empty() ? n.body : n.body + "\n\n" + n.footer, kNewsTextW);
    }
    if (n.unread && !n.id.empty()) {
        n.unread = false;
        std::error_code ec;
        std::filesystem::create_directories("sdmc:/config/SwitchU", ec);
        std::ofstream(kNewsReadPath, std::ios::app) << n.id << '\n';
    }
    if (n.picture && !m_newsPictures.count(index)) {
        if (m_newsPictures.size() >= 4) { // a few stay; the farthest goes
            auto far = m_newsPictures.begin();
            for (auto it = m_newsPictures.begin(); it != m_newsPictures.end(); ++it)
                if (std::abs(it->first - index) > std::abs(far->first - index)) far = it;
            m_newsPictures.erase(far);
        }
        auto tex = std::make_unique<nxui::Texture>();
        if (tex->loadFromMemory(app().gpu(), app().renderer(), n.picture->data(), n.picture->size(), 720))
            m_newsPictures[index] = std::move(tex);
    }
}

bool CardsActivity::updateNews(float dt) {
    auto& in = app().input();
    using B = nxui::Button;
    const int n = static_cast<int>(m_newsItems.size());
    // Touch: a row chooses it, dragging the article scrolls it, beside the sheet closes.
    const float x = in.touchX(), y = in.touchY();
    if (in.touchDown()) {
        m_newsDragging = false;
        m_newsTouchY = y;
        m_newsTouchScroll = m_newsScroll;
    }
    if (in.isTouching() && x > kNewsTextX - 20.f && kSheet.contains(x, y)) {
        if (std::abs(y - m_newsTouchY) > 10.f) m_newsDragging = true;
        if (m_newsDragging) m_newsScroll = std::max(0.f, m_newsTouchScroll - (y - m_newsTouchY));
    }
    if (in.touchUp()) {
        if (!kSheet.contains(x, y)) {
            m_newsOpen = false;
            m_audio.play("hide_modal");
            return true;
        }
        if (!m_newsDragging && x < kNewsTextX - 20.f) {
            const int row = static_cast<int>((y - kSheetTop + m_newsListScroll) / kNewsRowH);
            if (y >= kSheetTop && row >= 0 && row < n && row != m_newsSel) {
                m_audio.play("navigation");
                showNewsItem(row);
            }
        }
        m_newsDragging = false;
    }
    if (in.isDown(B::B) || in.isDown(B::Minus)) {
        m_newsOpen = false;
        m_audio.play("hide_modal");
        return true;
    }
    if ((in.isDown(B::DUp) || in.isDown(B::LStickU)) && m_newsSel > 0) {
        m_audio.play("navigation");
        showNewsItem(m_newsSel - 1);
    }
    if ((in.isDown(B::DDown) || in.isDown(B::LStickD)) && m_newsSel < n - 1) {
        m_audio.play("navigation");
        showNewsItem(m_newsSel + 1);
    }
    // The article scrolls with ZL / ZR, L / R or the right stick.
    float speed = 0.f;
    if (in.isHeld(B::ZR) || in.isHeld(B::R) || in.isHeld(B::RStickD)) speed += 700.f;
    if (in.isHeld(B::ZL) || in.isHeld(B::L) || in.isHeld(B::RStickU)) speed -= 700.f;
    m_newsScroll = std::max(0.f, m_newsScroll + speed * dt);
    return true;
}

void CardsActivity::drawNews(nxui::Renderer& r) {
    if (!m_newsOpen) return;
    const float a = easeOut(m_newsIn);
    r.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.45f * a));
    const nxui::Rect sheet = scaled(kSheet, 0.95f + 0.05f * a);
    frost(r, sheet, 34.f, nxui::Color(0.08f, 0.09f, 0.13f, 0.88f * a));
    r.drawRoundedRectOutline(sheet, nxui::Color(1.f, 1.f, 1.f, 0.16f * a), 34.f, 1.5f);
    r.drawGradientRect({sheet.x + 2.f, sheet.y + 2.f, sheet.width - 4.f, 80.f}, nxui::Color(1.f, 1.f, 1.f, 0.05f * a),
                       nxui::Color(1.f, 1.f, 1.f, 0.f));
    const float dy = sheet.y - kSheet.y;
    icon(r, "news", kSecX + 20.f, kSheet.y + 46.f + dy, 30.f, kWhite.withAlpha(a));
    textMid(r, m_fTitle, "News", kSecX + 46.f, kSheet.y + 46.f + dy, kWhite.withAlpha(a));
    const std::string hint = "Up/Down Choose    ZL/ZR Scroll    B Back";
    textMid(r, m_fBody, hint, kSheet.x + kSheet.width - 30.f - measured(m_fBody, hint).x, kSheet.y + 46.f + dy,
            kMuted.withAlpha(0.8f * a));
    if (m_newsItems.empty()) {
        textMid(r, m_fPill, fit(m_fPill, m_newsEmpty, kSheet.width - 80.f), kSecX, 340.f + dy, kMuted.withAlpha(a));
        return;
    }
    const float ring = 0.8f + 0.2f * std::sin(m_time * 5.f);
    const float visible = kRowsBottom - kSheetTop;

    // The list, kept around the chosen one.
    const float want = std::clamp(kNewsRowH * static_cast<float>(m_newsSel) - visible * 0.5f + kNewsRowH * 0.5f, 0.f,
                                  std::max(0.f, kNewsRowH * static_cast<float>(m_newsItems.size()) - visible));
    m_newsListScroll += (want - m_newsListScroll) * (m_settings.motion ? 0.25f : 1.f);
    r.pushClipRect({kSecX - 8.f, kSheetTop + dy - 4.f, kNewsListW + 16.f, visible + 8.f});
    for (size_t i = 0; i < m_newsItems.size(); ++i) {
        const nxui::Rect row{kSecX, kSheetTop + kNewsRowH * static_cast<float>(i) - m_newsListScroll + dy, kNewsListW,
                             kNewsRowH - 8.f};
        if (row.y > kRowsBottom + dy || row.y + row.height < kSheetTop + dy) continue;
        const NewsItem& n = m_newsItems[i];
        const bool on = static_cast<int>(i) == m_newsSel;
        r.drawRoundedRect(row, nxui::Color(1.f, 1.f, 1.f, (on ? 0.14f : 0.04f) * a), 16.f);
        if (on) r.drawRoundedRectOutline(row, accent(ring * a), 16.f, 2.f);
        if (n.unread) r.drawCircle({row.x + 14.f, row.y + 22.f}, 5.f, kGreen.withAlpha(a), 16);
        r.drawText(fit(m_fPill, n.title, row.width - 40.f), {row.x + 26.f, row.y + 9.f}, &m_fPill, kWhite.withAlpha(a));
        r.drawText(fit(m_fBody, n.topic, (row.width - 40.f) / 0.85f, 0.85f), {row.x + 26.f, row.y + 37.f}, &m_fBody,
                   kMuted.withAlpha(0.85f * a), 0.85f);
    }
    r.popClipRect();
    r.drawRect({kNewsTextX - 20.f, kSheetTop + dy, 1.5f, visible}, nxui::Color(1.f, 1.f, 1.f, 0.10f * a));

    // The article: its picture, title, where and when, then the text.
    const NewsItem& n = m_newsItems[(size_t)m_newsSel];
    const float top = kSheetTop + dy;
    r.pushClipRect({kNewsTextX - 6.f, top - 4.f, kNewsTextW + 12.f, visible + 8.f});
    float y = top - m_newsScroll;
    auto pic = m_newsPictures.find(m_newsSel);
    if (pic != m_newsPictures.end() && pic->second && pic->second->valid()) {
        const nxui::Rect frame{kNewsTextX, y, kNewsTextW, kNewsTextW * 9.f / 16.f};
        drawCover(r, pic->second.get(), frame, 18.f, kWhite.withAlpha(a));
        y += frame.height + 18.f;
    }
    for (const std::string& line : n.titleLines) {
        r.drawText(line, {kNewsTextX, y}, &m_fLabel, kWhite.withAlpha(a));
        y += 30.f;
    }
    y += 4.f;
    r.drawText(n.topic, {kNewsTextX, y}, &m_fBody, kMuted.withAlpha(0.85f * a), 0.9f);
    y += 34.f;
    for (const std::string& line : n.lines) {
        if (y > top - 30.f && y < top + visible) r.drawText(line, {kNewsTextX, y}, &m_fBody, kWhite.withAlpha(0.92f * a));
        y += 26.f;
    }
    r.popClipRect();
    // How far it scrolls, and a bar for it.
    const float content = y + m_newsScroll - top;
    const float most = std::max(0.f, content - visible + 24.f);
    if (m_newsScroll > most) m_newsScroll = most;
    if (most > 0.f) {
        const float barH = std::max(40.f, visible * visible / content);
        const float barY = top + (visible - barH) * (m_newsScroll / most);
        r.drawRoundedRect({kNewsTextX + kNewsTextW + 10.f, top, 4.f, visible}, nxui::Color(1.f, 1.f, 1.f, 0.08f * a), 2.f);
        r.drawRoundedRect({kNewsTextX + kNewsTextW + 10.f, barY, 4.f, barH}, kWhite.withAlpha(0.5f * a), 2.f);
    }
}
#endif

// ---------------------------------------------------------------- top bar and panel

void CardsActivity::activateTop(int index) {
    press(0, index);
    switch (index) {
        case kTopMenu: openDots(); break;
        case kTopDiscord: openDiscord(); break;
        case kTopSort:
            if (m_page == kGames) {
                int at = 0;
                for (int i = 0; i < 4; ++i)
                    if (kSortCycle[i] == m_settings.sort) at = i;
                sortGames(kSortCycle[(at + 1) % 4]);
                m_settings.save();
                m_audio.play("toggle_on");
            } else {
                switchPage(1);
            }
            break;
        case kTopUser: openUsers(); break;
        case kTopPlayers:
            say(m_playersTotal < 0 ? "Counting Nextendo players..."
                                   : std::to_string(m_playersTotal) + " players online in Nextendo.");
            break;
        case kTopBell: openNotifications(); break;
        case kTopMusic: openMusic(); break;
        case kTopPower: openPower(); break;
        case kTopGear: openSettings(); break;
        default: break;
    }
}

void CardsActivity::activateBottom(int index) {
    press(2, index);
    switch (index) {
        case 0: play(); break;
        case 1: openOptions(); break;
        case 2: switchPage(1); break;
        case 3:
#ifdef SWITCHU_MENU
            openNews();
#else
            m_settings.save();
            app().requestExit();
#endif
            break;
        default: break;
    }
}

// Text is cached per font as pictures (144 or 4 MB at most). A full cache
// stops making new ones and text went missing for a frame (a flicker), so a
// few of the oldest go before it fills. Freed textures wait out the frames in
// flight themselves (nxui), so nothing waits for the GPU.
void CardsActivity::keepTextCachesSmall() {
    auto keep = [](nxui::Font& f) {
        if (f.maintenanceRequested() || f.cacheEntryCount() > 120 || f.cacheBytes() > (3u << 20) + (256u << 10))
            f.trimCache(100, 3u << 20);
    };
    for (nxui::Font* f : {&m_fTitle, &m_fPlay, &m_fLabel, &m_fPill, &m_fBody, &m_fBadge, &m_fCount}) keep(*f);
#ifdef SWITCHU_MENU
    // Discord, Social and the keyboard draw with HomeNX's font: nothing trimmed
    // its cache, and once full no new channel names or messages showed.
    if (m_hxFonts) {
        keep(m_hxFont);
        keep(m_hxSmall);
    }
#endif
}

// ---------------------------------------------------------------- input: popups and settings

bool CardsActivity::updatePopup() {
    if (m_menu.empty()) return false;
    listInput(m_menu, m_menuSel, [this]() { m_menu.clear(); });
    return true;
}

void CardsActivity::listInput(std::vector<MenuItem>& items, int& sel, const std::function<void()>& close) {
    auto& in = app().input();
    using B = nxui::Button;
    const int n = static_cast<int>(items.size());
    if (n == 0) return;
    sel = std::clamp(sel, 0, n - 1);
    if ((in.isDown(B::DUp) || in.isDown(B::LStickU)) && sel > 0) {
        --sel;
        m_audio.play("navigation");
    }
    if ((in.isDown(B::DDown) || in.isDown(B::LStickD)) && sel < n - 1) {
        ++sel;
        m_audio.play("navigation");
    }
    MenuItem& item = items[(size_t)sel];
    const bool left = in.isDown(B::DLeft) || in.isDown(B::LStickL);
    const bool right = in.isDown(B::DRight) || in.isDown(B::LStickR);
    if ((left || right) && (item.kind == MenuItem::Choice || item.kind == MenuItem::Slider) && item.change) {
        item.change(left ? -1 : 1);
        m_audio.play("navigation");
    }
    if (in.isDown(B::B)) {
        m_audio.play("hide_modal");
        close();
        return;
    }
    if (in.isDown(B::A)) {
        if (item.kind == MenuItem::Toggle || item.kind == MenuItem::Choice || item.kind == MenuItem::Slider) {
            if (item.change) item.change(1);
            m_audio.play(item.kind == MenuItem::Toggle && item.level && item.level() < 0.5f ? "toggle_off" : "toggle_on");
        } else if (item.kind == MenuItem::Action && item.run) {
            auto run = item.run; // close() may take the list (and the row) away
            if (!item.stay) close();
            else m_audio.play("activation");
            run();
        }
    }
}

bool CardsActivity::updateSheet(float) {
    auto& in = app().input();
    using B = nxui::Button;
    if (m_sheet.empty()) return false;
    const int sections = static_cast<int>(m_sheet.size());
    auto rows = [this]() -> std::vector<MenuItem>& { return m_sheet[(size_t)m_sheetSec].rows; };
    auto goSection = [&](int s) {
        s = (s + sections) % sections;
        if (s == m_sheetSec) return;
        m_sheetSec = s;
        m_sheetRow = 0;
        m_sheetScroll = 0.f;
        m_sheetRowsIn = m_settings.motion ? 0.f : 1.f;
        m_audio.play("tab_transition");
    };
    const bool up = in.isDown(B::DUp) || in.isDown(B::LStickU);
    const bool down = in.isDown(B::DDown) || in.isDown(B::LStickD);
    const bool left = in.isDown(B::DLeft) || in.isDown(B::LStickL);
    const bool right = in.isDown(B::DRight) || in.isDown(B::LStickR);
    if (in.isDown(B::L) || in.isDown(B::ZL)) goSection(m_sheetSec - 1);
    if (in.isDown(B::R) || in.isDown(B::ZR)) goSection(m_sheetSec + 1);
    if (in.isDown(B::B)) {
        if (m_sheetOnRows) {
            m_sheetOnRows = false;
            m_audio.play("navigation");
        } else {
            m_sheetOpen = false;
            m_audio.play("hide_modal");
        }
        return true;
    }
    if (!m_sheetOnRows) {
        if (up) goSection(m_sheetSec - 1);
        if (down) goSection(m_sheetSec + 1);
        if ((right || in.isDown(B::A)) && !rows().empty()) {
            m_sheetOnRows = true;
            m_audio.play("navigation");
        }
        return true;
    }
    const int n = static_cast<int>(rows().size());
    if (up && m_sheetRow > 0) {
        --m_sheetRow;
        m_audio.play("navigation");
    }
    if (down && m_sheetRow < n - 1) {
        ++m_sheetRow;
        m_audio.play("navigation");
    }
    MenuItem& item = rows()[(size_t)std::clamp(m_sheetRow, 0, n - 1)];
    const bool steps = item.kind == MenuItem::Choice || item.kind == MenuItem::Slider;
    if (left) {
        if (steps && item.change) {
            item.change(-1);
            m_audio.play("navigation");
        } else {
            m_sheetOnRows = false;
            m_audio.play("navigation");
        }
    }
    if (right && steps && item.change) {
        item.change(1);
        m_audio.play("navigation");
    }
    if (in.isDown(B::A)) {
        if (item.kind == MenuItem::Action && item.run) {
            m_audio.play("activation");
            auto run = item.run;
            run();
        } else if (item.change && item.kind != MenuItem::Info) {
            item.change(1);
            m_audio.play(item.kind == MenuItem::Toggle && item.level && item.level() < 0.5f ? "toggle_off" : "toggle_on");
        }
    }
    return true;
}

// ---------------------------------------------------------------- touch

bool CardsActivity::handleTouch() {
    auto& in = app().input();
    const float x = in.touchX(), y = in.touchY();
    auto inCircle = [&](float cx, float cy, float r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; };
    // Where a row was tapped: a toggle flips, a choice steps (left half back,
    // right half on), a slider goes to the finger, an action runs.
    auto tapRow = [&](MenuItem& item, const nxui::Rect& row, float sliderX, float sliderW) {
        switch (item.kind) {
            case MenuItem::Toggle:
                if (item.change) item.change(1);
                m_audio.play("toggle_on");
                return false;
            case MenuItem::Choice:
                if (item.change) item.change(x < row.x + row.width * 0.5f ? -1 : 1);
                m_audio.play("navigation");
                return false;
            case MenuItem::Slider:
                if (item.change && item.level) {
                    const int want = static_cast<int>(std::lround(clamp01((x - sliderX) / sliderW) * 10.f)) * 10;
                    const int now = static_cast<int>(std::lround(item.level() * 100.f));
                    if (want != now) item.change((want - now) / 10);
                }
                m_audio.play("navigation");
                return false;
            case MenuItem::Action:
                return true;
            default:
                return false;
        }
    };

    if (m_notifOpen) {
        if (in.touchUp()) {
            m_notifOpen = false;
            m_audio.play("hide_modal");
        }
        return in.isTouching() || in.touchUp();
    }
    if (m_sheetOpen) {
        if (!in.touchUp()) return in.isTouching();
        if (!kSheet.contains(x, y)) {
            m_sheetOpen = false;
            m_audio.play("hide_modal");
            return true;
        }
        if (x < kRowsX - 10.f) { // a section
            const int s = static_cast<int>((y - kSheetTop) / kSecH);
            if (y >= kSheetTop && s >= 0 && s < static_cast<int>(m_sheet.size()) && s != m_sheetSec) {
                m_sheetSec = s;
                m_sheetRow = 0;
                m_sheetScroll = 0.f;
                m_sheetRowsIn = m_settings.motion ? 0.f : 1.f;
                m_audio.play("tab_transition");
            }
            m_sheetOnRows = false;
            return true;
        }
        auto& rows = m_sheet[(size_t)m_sheetSec].rows;
        const int r = static_cast<int>((y - kSheetTop + m_sheetScroll) / kRowH);
        if (y < kSheetTop || y > kRowsBottom || r < 0 || r >= static_cast<int>(rows.size())) return true;
        m_sheetOnRows = true;
        m_sheetRow = r;
        const nxui::Rect row{kRowsX, kSheetTop + kRowH * static_cast<float>(r) - m_sheetScroll, kRowsW, kRowH - 8.f};
        if (tapRow(rows[(size_t)r], row, row.x + row.width - 18.f - 220.f, 220.f) && rows[(size_t)r].run) {
            m_audio.play("activation");
            auto run = rows[(size_t)r].run;
            run();
        }
        return true;
    }
    if (m_quickOpen) return quickTouch();
    if (m_powerOpen) return powerTouch();
    if (m_dotsOpen) return dotsTouch();
    if (m_optOpen) return optionsTouch();
    if (!m_menu.empty()) {
        if (!in.touchUp()) return in.isTouching();
        nxui::Rect box;
        float rowH = 56.f;
        popupGeometry(m_menu.size(), box, rowH);
        if (!box.contains(x, y)) {
            m_menu.clear();
            m_audio.play("hide_modal");
            return true;
        }
        const int row = static_cast<int>((y - box.y - 66.f) / rowH);
        if (y < box.y + 66.f || row < 0 || row >= static_cast<int>(m_menu.size())) return true;
        m_menuSel = row;
        MenuItem& item = m_menu[(size_t)row];
        const nxui::Rect r{box.x + 14.f, box.y + 66.f + rowH * static_cast<float>(row), box.width - 28.f, rowH - 6.f};
        if (tapRow(item, r, r.x + r.width - 18.f - 150.f, 150.f) && item.run) {
            auto run = item.run;
            if (!item.stay) m_menu.clear();
            run();
        }
        return true;
    }
    if (m_moving) { // a tap puts it down
        if (in.touchUp()) finishMove(true);
        return in.isTouching() || in.touchUp();
    }

    if (in.touchDown()) {
        m_touchOnCards = y > 100.f && y < 490.f;
        m_touchScroll = m_scroll;
        m_dragging = false;
    }
    // Dragging the row: it follows the finger, the card under the middle is chosen.
    if (in.isTouching() && m_touchOnCards) {
        const float dx = x - in.touchStartX();
        if (!m_dragging && std::abs(dx) > 14.f) m_dragging = true;
        if (m_dragging && !games().empty()) {
            const float last = static_cast<float>(games().size() - 1);
            m_scroll = std::clamp(m_touchScroll - dx / kCardPitch, -0.35f, last + 0.35f);
            const int near = std::clamp(static_cast<int>(std::lround(m_scroll)), 0, static_cast<int>(last));
            if (near != sel()) select(near);
        }
        return true;
    }
    if (!in.touchUp()) return false;
    if (m_dragging) { // let go: it settles on the nearest card
        m_dragging = false;
        return true;
    }

    // A tap.
    if (m_touchOnCards) {
        for (int i = 0; i < static_cast<int>(games().size()); ++i) {
            nxui::Rect card;
            float t = 0.f;
            if (!cardRect(i, card, t) || !card.contains(x, y)) continue;
            m_zone = Zone::Cards;
            if (i == sel()) {
                press(1, i);
                play();
            } else {
                select(i);
            }
            return true;
        }
        return true;
    }
    if (y < 95.f) { // the top bar
        int hit = -1;
        if (inCircle(kMenuX, kTopY, 40.f)) hit = kTopMenu;
        else if (inCircle(kDiscordX, kTopY, 40.f)) hit = kTopDiscord;
        else if (inCircle(kSortX, kTopY, 40.f) || (x > kLabelX && x < m_userHit.x - 8.f)) hit = kTopSort;
        else if (m_userHit.contains(x, y)) hit = kTopUser;
        else if (m_settings.players && m_playersHit.contains(x, y)) hit = kTopPlayers;
        else if (inCircle(kBellX, kTopY, 32.f)) hit = kTopBell;
        else if (inCircle(kMusicX, kTopY, 40.f)) hit = kTopMusic;
        else if (inCircle(kPowerX, kTopY, 40.f)) hit = kTopPower;
        else if (inCircle(kGearX, kTopY, 40.f)) hit = kTopGear;
        if (hit >= 0) {
            m_zone = Zone::Top;
            m_topSel = hit;
            activateTop(hit);
        }
        return true;
    }
    if (kPanel.contains(x, y)) { // the panel's buttons
        const float by = kPanel.y + 108.f, cy = by + 38.5f, right = kPanel.x + kPanel.width - 22.f;
        int hit = -1;
        if (nxui::Rect{kPanel.x + 22.f, by, 262.f, 77.f}.contains(x, y)) hit = 0;
        else if (inCircle(kPanel.x + 22.f + 262.f + 8.f + 38.5f, cy, 40.f)) hit = 1;
        else if (inCircle(right - 77.f - 8.f - 38.5f, cy, 40.f)) hit = 2;
        else if (inCircle(right - 38.5f, cy, 40.f)) hit = 3;
        if (hit >= 0) {
            m_zone = Zone::Bottom;
            m_bottomSel = hit;
            activateBottom(hit);
        }
        return true;
    }
    return true;
}

// ---------------------------------------------------------------- frame

void CardsActivity::perfMark(const char* step) {
    const std::uint64_t now = armGetSystemTick();
    const float ms = ticksMs(now - m_perfMarkTick);
    if (ms > m_perfMarkMs) {
        m_perfMarkMs = ms;
        m_perfMarkName = step;
    }
    m_perfMarkTick = now;
}

void CardsActivity::onUpdate(float dt) {
    // Frame times for the log: the time since the last frame, and what the last one did.
    const std::uint64_t frameTick = armGetSystemTick();
    if (m_perfLastTick) {
        const float interval = ticksMs(frameTick - m_perfLastTick);
        ++m_perfFrames;
        m_perfWindow += interval;
        m_perfWorst = std::max(m_perfWorst, interval);
        if (interval > 20.f) ++m_perfSlow;
        if (interval > 34.f) ++m_perfVerySlow;
        const auto waits = app().gpu().takeWaitStats();
        const auto frees = app().renderer().takeFreeStats();
        if (interval > 24.f && m_settings.perfLog) {
            char line[400];
            std::snprintf(line, sizeof(line),
                          "slow frame %.1f ms: daemon %.1f, pictures %.1f (%d uploaded), discord %.1f, the rest of update %.1f "
                          "(most: %s %.1f), drawing %.1f, GPU/system %.1f (GPU waits: frame %.1f, uploads %.1f; %d freed in %.1f)",
                          interval, m_perfPumpMs, m_perfArtMs, m_perfUploads, m_perfDiscordMs,
                          std::max(0.f, m_perfUpdateMs - m_perfPumpMs - m_perfArtMs - m_perfDiscordMs), m_perfMarkName,
                          m_perfMarkMs, m_perfDrawMs, std::max(0.f, interval - m_perfUpdateMs - m_perfDrawMs), waits.frameMs,
                          waits.uploadMs, frees.count, frees.ms);
            perfLine(line);
        }
        if (m_perfWindow >= 10000.f) {
            char line[160];
            std::snprintf(line, sizeof(line), "10 s: %.1f fps, worst frame %.1f ms, %d over 20 ms, %d over 34 ms",
                          m_perfFrames * 1000.f / m_perfWindow, m_perfWorst, m_perfSlow, m_perfVerySlow);
            perfLine(line);
            m_perfFrames = m_perfSlow = m_perfVerySlow = 0;
            m_perfWindow = m_perfWorst = 0.f;
        }
    } else {
        char line[64];
        std::snprintf(line, sizeof(line), "first frame at %.0f ms", ticksMs(frameTick - m_perfCreateTick));
        perfLine(line);
    }
    m_perfLastTick = frameTick;
    m_perfMarkTick = frameTick;
    m_perfMarkMs = 0.f;
    m_perfMarkName = "";
    m_perfPumpMs = m_perfArtMs = m_perfDiscordMs = 0.f;
    m_perfUploads = 0;
    struct UpdateTimer {
        CardsActivity* self;
        std::uint64_t start;
        ~UpdateTimer() {
            self->perfMark("buttons and screens");
            self->m_perfUpdateMs = ticksMs(armGetSystemTick() - start);
        }
    } updateTimer{this, frameTick};

    // A slow frame no longer jumps everything ahead (it looked like stutter):
    // things move a little slower for a moment instead.
    dt = std::min(dt, 1.f / 30.f);
#ifdef SWITCHU_MENU
    std::uint64_t t = armGetSystemTick();
    pumpDaemon();
    m_perfPumpMs = ticksMs(armGetSystemTick() - t);
    if (m_asleep) return;
#endif
    // What the first frame doesn't need starts just after it, so HOME shows sooner.
    if (m_startStage < 3) {
        ++m_startStage;
        if (m_startStage == 2) {
            m_audio.start();
            m_audio.setSounds(m_settings.sounds, m_settings.soundVolume);
            m_audio.setShuffle(m_settings.shuffle);
            m_audio.setRepeat(m_settings.repeat);
            m_audio.setMusic(m_settings.music, m_settings.musicVolume);
        } else if (m_startStage == 3) {
            loadAvatar();
        }
    }
    m_time += dt;
    perfMark("start-up");
    keepTextCachesSmall();
    perfMark("text caches");
    const std::uint64_t tArt = armGetSystemTick();
    takeArt();
    m_perfArtMs = ticksMs(armGetSystemTick() - tArt);
    perfMark("pictures");
    m_audio.update();
    perfMark("music");
    const bool motion = m_settings.motion;
    auto step = [&](float& v, float seconds) { v = motion ? std::min(1.f, v + dt / seconds) : 1.f; };
    step(m_intro, m_introFor);
    step(m_pageIn, 0.38f);
    step(m_detailIn, 0.25f);
    step(m_sheen, 0.8f);
    step(m_menuIn, 0.2f);
    step(m_pressT, 0.22f);
    step(m_bgFade, 0.45f);
    step(m_sheetIn, 0.24f);
    step(m_sheetRowsIn, 0.22f);
    step(m_quickIn, 0.22f);
    step(m_powerIn, 0.28f);
    step(m_dotsIn, 0.26f);
    step(m_optIn, 0.26f);
    step(m_quickListIn, 0.2f);
#ifdef SWITCHU_MENU
    step(m_newsIn, 0.24f);
#endif
    const float target = static_cast<float>(sel());
    if (!m_dragging) {
        m_scroll = motion ? m_scroll + (target - m_scroll) * (1.f - std::exp(-dt * 13.f)) : target;
        if (std::abs(m_scroll - target) < 0.001f) m_scroll = target;
    }
    if (m_messageFor > 0.f) m_messageFor -= dt;

    // Pictures that didn't come are asked for again now and then.
    if ((m_retryIn -= dt) <= 0.f) {
        m_retryIn = 5.f;
        wantArt();
    }
    // The real lists arrived (not while a card is being moved or launched).
    perfMark("scrolling");
    if (m_listJob.valid() && !m_moving && !m_leaving
        && m_listJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        takeLists(m_listJob.get());
    perfMark("lists");

    // Homebrew updates: the saved list at once; asked again a little after start.
    if (!m_updatesChecked && m_netReady && m_time > 6.f) {
        m_updatesChecked = true;
        checkHomebrewUpdates(false);
    }
    if (m_updatesJob.valid() && m_updatesJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_updates = m_updatesJob.get();
        int n = 0;
        for (Page p : {kHomebrew, kMedia}) for (const Game& g : m_pages[p]) n += nroUpdate(g) ? 1 : 0;
        if (n > 0) say(std::to_string(n) + (n == 1 ? " homebrew app has" : " homebrew apps have") + " an update (+ on it, or Settings).");
    }
    if (m_nroUpdateJob.valid()) {
        std::string name = m_nroUpdating;
        for (Page p : {kHomebrew, kMedia})
            for (const Game& g : m_pages[p])
                if (g.path == m_nroUpdating) name = g.name;
        if (m_nroUpdateJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const std::string error = m_nroUpdateJob.get();
            if (error.empty()) {
                const std::string latest = m_updates.count(m_nroUpdating) ? m_updates[m_nroUpdating].latest : std::string();
                for (Page p : {kHomebrew, kMedia})
                    for (Game& g : m_pages[p])
                    if (g.path == m_nroUpdating) {
                        g.version = latest;
                        auto art = m_art.find(g.id); // its new icon
                        if (art != m_art.end()) {
                            art->second.card.reset();
                            art->second.icon.reset();
                            art->second.cardAsked = art->second.iconAsked = false;
                        }
                    }
                m_updates.erase(m_nroUpdating);
                saveLists(m_pages[kGames], homebrewToSave());
                m_audio.play("toggle_on");
                say(name + " is updated" + (latest.empty() ? std::string(".") : " to v" + latest + "."));
                wantArt();
            } else {
                say("Couldn't update " + name + ": " + error);
            }
            m_nroUpdating.clear();
            if (!m_nroQueue.empty()) {
                const std::string next = m_nroQueue.front();
                m_nroQueue.erase(m_nroQueue.begin());
                updateNro(next);
            }
        } else {
            m_message = "Updating " + name + "  " + std::to_string(static_cast<int>(m_nroProgress.load() * 100.f)) + "%";
            m_messageFor = std::max(m_messageFor, 1.f);
        }
    }

    // An NRO forwarder was uninstalled (or wasn't).
    if (m_deleteJob.valid() && m_deleteJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::uint32_t rc = m_deleteJob.get();
        if (R_SUCCEEDED(rc)) {
            switchu::control_cache::remove(m_deleteId);
            auto& page = m_pages[kHomebrew];
            const std::uint64_t id = m_deleteId;
            page.erase(std::remove_if(page.begin(), page.end(), [id](const Game& x) { return x.id == id; }), page.end());
            m_sels[kHomebrew] = std::clamp(m_sels[kHomebrew], 0, std::max(0, static_cast<int>(page.size()) - 1));
            if (m_page == kHomebrew) m_scroll = static_cast<float>(sel());
            saveOrder(kHomebrew);
            saveLists(m_pages[kGames], homebrewToSave());
            m_audio.play("toggle_off");
            say(m_deleteName + " was deleted.");
            wantArt();
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " (0x%X).", rc);
            say("Couldn't delete " + m_deleteName + buf);
        }
    }

    perfMark("updates and deleting");
    // A Media app arrived (or didn't): its card becomes the app's.
    if (m_getJob.valid() && m_getJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        if (m_getJob.get()) {
            for (Game& g : m_pages[kMedia])
                if (g.path == m_getPath && g.action == "get") {
                    HomebrewApp app;
                    if (readHomebrew(g.path, app, false)) {
                        if (!app.name.empty()) g.name = app.name;
                        g.author = app.author;
                        g.version = app.version;
                    } else {
                        g.author.clear();
                    }
                    g.action.clear();
                    g.id = 0xFF00000000000000ULL | (std::hash<std::string>{}(g.path) & 0x00FFFFFFFFFFFFFFULL);
                }
            saveLists(m_pages[kGames], homebrewToSave());
            m_audio.play("toggle_on");
            say(m_getName + " is ready: A to open it.");
            wantArt();
        } else {
            say(m_getName + " didn't download. Check the internet connection.");
        }
    }

    // BrowseNX arrived (or didn't).
    if (m_browserJob.valid() && m_browserJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        if (m_browserJob.get()) {
            Game g;
            g.path = kBrowseNxPath;
            g.name = "BrowseNX";
            g.author = "ad3333m";
            g.homebrew = true;
            g.id = 0xFF00000000000000ULL | (std::hash<std::string>{}(g.path) & 0x00FFFFFFFFFFFFFFULL);
            g.order = static_cast<int>(m_pages[kHomebrew].size());
            m_pages[kHomebrew].push_back(g);
            openNro(kBrowseNxPath);
        } else {
            say("BrowseNX didn't download. Check the internet connection.");
        }
    }

    perfMark("downloads");
#ifdef SWITCHU_MENU
    // Discord: a saved account connects a few seconds in (its messages show
    // here and in the bell); the panel opens with L.
    if (!m_discordStarted && !m_leaving && (m_discordStartIn -= dt) <= 0.f) {
        m_discordStarted = true;
        if (!m_discord.started() && m_discord.hasSavedAccount()) m_discord.activate();
    }
    const std::uint64_t tDiscord = armGetSystemTick();
    m_discord.update();
    m_perfDiscordMs = ticksMs(armGetSystemTick() - tDiscord);
    DiscordService::Alert alert;
    if (m_discord.takeAlert(alert)) {
        notifications::add(("discord/" + std::to_string(alert.channelId) + "/" + std::to_string(alert.guildId)).c_str(),
                           alert.title, alert.text);
        ++m_unread;
        if (!(m_discordPanel && m_discordPanel->isActive())) say(alert.title + ": " + alert.text);
    }
    perfMark("discord");
    // News is ready.
    if (m_newsJob.valid() && m_newsJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_newsJob.get();
        app().gpu().waitIdle(); // the last articles' pictures go
        m_newsPictures.clear();
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_newsItems = std::move(m_newsReady);
        }
        m_newsLoadedAt = m_time;
        if (m_newsWanted && !m_leaving) {
            m_newsWanted = false;
            m_newsOpen = true;
            m_newsIn = m_settings.motion ? 0.f : 1.f;
            m_newsSel = 0;
            m_newsListScroll = 0.f;
            m_messageFor = 0.f;
            m_audio.play("show_modal");
            showNewsItem(0);
        }
    }
    // Updates: say when it's done.
    {
        static int lastUpdate = -1;
        const int now = m_update.load();
        if (now != lastUpdate) {
            if (lastUpdate >= 0) {
                if (now == static_cast<int>(Update::Installed)) say("HomeNX is updated. Restart to finish.");
                else if (now == static_cast<int>(Update::UpToDate)) say("HomeNX is up to date.");
                else if (now == static_cast<int>(Update::Available)) say("An update is ready: choose Install.");
                else if (now == static_cast<int>(Update::Failed)) {
                    std::lock_guard<std::mutex> lk(m_mu);
                    say("The update didn't work: " + m_updateError);
                }
            }
            lastUpdate = now;
        }
    }
    perfMark("news");
    updatePairing(dt);
    // HomeNX's screens have the buttons while they're open.
    if (m_textEntry && m_textEntry->isActive()) { // the keyboard: buttons and touch
        m_textEntry->handleTouch(app().input());
        return;
    }
    if (m_discordPanel && m_discordPanel->isActive()) {
        m_discordPanel->handleTouch(app().input());
        return;
    }
    if (m_social && m_social->isActive()) {
        m_social->handleTouch(app().input());
        return;
    }
    if (m_padTest && m_padTest->isActive()) {
        m_padTest->handleTouch(app().input());
        return;
    }
#endif

    // The launch: the card grows, then black, then it starts.
#ifdef SWITCHU_MENU
    if (m_newsOpen && !m_leaving) {
        updateNews(dt);
        return;
    }
#endif
    if (m_leaving) {
        m_leaveT = std::min(1.f, m_leaveT + dt / std::max(0.05f, m_leaveFor));
        if (m_leaveT >= 1.f && !m_left) {
            m_left = true;
            m_settings.save();
            auto then = std::move(m_leaveThen);
            m_leaveThen = nullptr;
            if (then) then();
        }
        return;
    }

    auto& in = app().input();
    using B = nxui::Button;
    const bool aDown = in.isDown(B::A), bDown = in.isDown(B::B);
    const bool upDown = in.isDown(B::DUp) || in.isDown(B::LStickU);
    const bool downDown = in.isDown(B::DDown) || in.isDown(B::LStickD);
    const bool leftDown = in.isDown(B::DLeft) || in.isDown(B::LStickL);
    const bool rightDown = in.isDown(B::DRight) || in.isDown(B::LStickR);

    if (handleTouch()) return;

    if (m_notifOpen) {
        if (bDown || aDown || in.isDown(B::Minus)) {
            m_notifOpen = false;
            m_audio.play("hide_modal");
        }
        if (in.isDown(B::X)) {
            notifications::clear();
            m_notifRows.clear();
            m_audio.play("toggle_off");
        }
        return;
    }
    if (m_sheetOpen) {
        updateSheet(dt);
        return;
    }
    if (m_quickOpen) {
        updateQuick();
        return;
    }
    if (m_powerOpen) {
        updatePower();
        return;
    }
    if (m_dotsOpen) {
        updateDots();
        return;
    }
    if (m_optOpen) {
        updateOptions();
        return;
    }
    if (!m_menu.empty()) {
        updatePopup();
        return;
    }

    // Left / right: repeating while held.
    const bool left = in.isHeld(B::DLeft) || in.isHeld(B::LStickL);
    const bool right = in.isHeld(B::DRight) || in.isHeld(B::LStickR);
    int move = 0;
    if (leftDown) {
        move = -1;
        m_repeat = 0.32f;
    } else if (rightDown) {
        move = 1;
        m_repeat = 0.32f;
    } else if (left || right) {
        if ((m_repeat -= dt) <= 0.f) {
            move = left ? -1 : 1;
            m_repeat = m_moving ? 0.14f : 0.075f;
        }
    }

    // Plus > Move: left / right carry the card; A or Plus put it down, B puts it back.
    if (m_moving) {
        auto& page = games();
        const int s = sel(), to = s + move;
        if (move && to >= 0 && to < static_cast<int>(page.size())) {
            std::swap(page[(size_t)s], page[(size_t)to]);
            sel() = to;
            m_audio.play("navigation");
            wantArt();
        }
        if (aDown || in.isDown(B::Plus)) finishMove(true);
        else if (bDown) finishMove(false);
        return;
    }

    if (move) {
        if (m_zone == Zone::Cards) {
            select(sel() + move);
        } else if (m_zone == Zone::Top) {
            int next = m_topSel;
            do next += move;
            while (next == kTopPlayers && !m_settings.players);
            next = std::clamp(next, 0, kTopCount - 1);
            if (next == kTopPlayers && !m_settings.players) next = m_topSel;
            if (next != m_topSel) m_audio.play("navigation");
            m_topSel = next;
        } else {
            const int next = std::clamp(m_bottomSel + move, 0, 3);
            if (next != m_bottomSel) m_audio.play("navigation");
            m_bottomSel = next;
        }
    }
    if (upDown && m_zone != Zone::Top) {
        m_zone = m_zone == Zone::Bottom ? Zone::Cards : Zone::Top;
        m_audio.play("navigation");
    }
    if (downDown && m_zone != Zone::Bottom) {
        m_zone = m_zone == Zone::Top ? Zone::Cards : Zone::Bottom;
        m_audio.play("navigation");
    }
    if (aDown) {
        if (m_zone == Zone::Cards) {
            press(1, sel());
            play();
        } else if (m_zone == Zone::Top) {
            activateTop(m_topSel);
        } else {
            activateBottom(m_bottomSel);
        }
    }
    if (bDown && m_zone != Zone::Cards) m_zone = Zone::Cards;
    if (in.isDown(B::Plus) || in.isDown(B::X)) openOptions();
    if (in.isDown(B::Y)) activateTop(kTopSort);
    if (in.isDown(B::Minus)) openNotifications();
    if (in.isDown(B::L)) openDiscord();
    if (in.isDown(B::R)) openQuick();
    if (in.isDown(B::ZL)) switchPage(-1);
    if (in.isDown(B::ZR)) switchPage(1);
}

void CardsActivity::draw(nxui::Renderer& r) {
    struct DrawTimer {
        CardsActivity* self;
        std::uint64_t start;
        ~DrawTimer() { self->m_perfDrawMs = ticksMs(armGetSystemTick() - start); }
    } drawTimer{this, armGetSystemTick()};
    drawBackground(r);
    drawCards(r);
    // One blurred copy of the frame for every glass shape on top of it.
    m_frosted = r.gpu().offscreenReady();
    if (m_frosted) {
        r.captureToOffscreen(false);
        r.applyBlur(2.5f, 2);
    }
    drawCardExtras(r);
    // Launching: the bar and the panel go first.
    const int leaveStyle = launchStyle();
    const float ui = m_leaving && leaveStyle != kNoAnimation && leaveStyle != kFade ? 1.f - easeOut(m_leaveT / 0.3f) : 1.f;
    drawTopBar(r, easeOut(m_intro * 2.2f) * ui);
    drawPanel(r, easeOut((m_intro - 0.25f) * 2.f) * ui);
    if (m_moving) { // what Move does
        const std::string hint = "Move it with  < >      A  Done      B  Put it back";
        const float w = measured(m_fBody, hint).x + 44.f;
        const nxui::Rect pill{640.f - w * 0.5f, 452.f, w, 40.f};
        frost(r, pill, 20.f, nxui::Color(0.06f, 0.07f, 0.10f, 0.7f));
        r.drawRoundedRectOutline(pill, accent(0.6f), 20.f, 1.5f);
        textMid(r, m_fBody, hint, pill.x + 22.f, pill.y + 20.f, kWhite);
    }
    if (m_messageFor > 0.f && !m_message.empty()) {
        const float a = std::min(1.f, m_messageFor * 3.f) * easeOut((4.5f - m_messageFor) * 5.f);
        // Two lines when it's long: words up to the width, then the rest.
        std::string first = m_message, second;
        if (measured(m_fBody, m_message).x > 820.f) {
            size_t cut = m_message.size();
            while (cut != std::string::npos && cut > 0 && measured(m_fBody, m_message.substr(0, cut)).x > 820.f)
                cut = m_message.rfind(' ', cut - 1);
            if (cut != std::string::npos && cut > 0) {
                first = m_message.substr(0, cut);
                second = fit(m_fBody, m_message.substr(cut + 1), 820.f);
            } else {
                first = fit(m_fBody, m_message, 820.f);
            }
        }
        const float w = std::max(measured(m_fBody, first).x, measured(m_fBody, second).x) + 44.f;
        const float h = second.empty() ? 42.f : 68.f;
        const nxui::Rect pill{640.f - w * 0.5f, 450.f + 10.f * (1.f - a) - (h - 42.f) * 0.5f - (m_moving ? 52.f : 0.f), w, h};
        frost(r, pill, 21.f, nxui::Color(0.06f, 0.07f, 0.10f, 0.75f * a));
        r.drawRoundedRectOutline(pill, nxui::Color(1.f, 1.f, 1.f, 0.15f * a), 21.f, 1.5f);
        textMid(r, m_fBody, first, pill.x + 22.f, pill.y + (second.empty() ? 21.f : 22.f), kWhite.withAlpha(a));
        if (!second.empty()) textMid(r, m_fBody, second, pill.x + 22.f, pill.y + 46.f, kWhite.withAlpha(a));
    }
    drawPopup(r);
    drawSheet(r);
    drawQuick(r);
    drawDots(r);
    drawOptions(r);
    drawPower(r);
#ifdef SWITCHU_MENU
    drawNews(r);
#endif
    drawLaunch(r);
}

// ---------------------------------------------------------------- drawing

void CardsActivity::icon(nxui::Renderer& r, const char* name, float cx, float cy, float size, const nxui::Color& tint) {
    auto it = m_icons.find(name);
    if (it == m_icons.end() || !it->second->valid()) return;
    r.drawTexture(it->second.get(), {cx - size * 0.5f, cy - size * 0.5f, size, size}, tint);
}

void CardsActivity::frost(nxui::Renderer& r, const nxui::Rect& rect, float radius, const nxui::Color& tint) {
    if (m_frosted) r.drawOffscreenRounded(0, rect, radius, kWhite.withAlpha(std::min(1.f, tint.a * 3.f)));
    r.drawRoundedRect(rect, tint, radius);
}

void CardsActivity::glassCircle(nxui::Renderer& r, float cx, float cy, float d, bool focused, const char* name,
                                float iconSize, float a, float pressed) {
    const float s = 1.f - 0.08f * pressed;
    d *= s;
    const nxui::Rect box{cx - d * 0.5f, cy - d * 0.5f, d, d};
    frost(r, box, d * 0.5f, focused ? nxui::Color(1.f, 1.f, 1.f, 0.22f * a) : glassTint(a));
    const float pulse = 0.8f + 0.2f * std::sin(m_time * 5.f);
    r.drawRoundedRectOutline(box, focused ? accent(pulse * a) : kGlassEdge.withAlpha(kGlassEdge.a * a), d * 0.5f,
                             focused ? 3.f : 1.5f);
    icon(r, name, cx, cy, iconSize * s, kWhite.withAlpha(a));
}

void CardsActivity::drawBackground(nxui::Renderer& r) {
    r.drawGradientRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.36f, 0.41f, 0.55f, 1.f),
                       nxui::Color(0.22f, 0.25f, 0.34f, 1.f));
    auto bgOf = [this](std::uint64_t id) -> const nxui::Texture* {
        auto it = m_art.find(id);
        return it != m_art.end() && it->second.bg && it->second.bg->valid() ? it->second.bg.get() : nullptr;
    };
    const nxui::Rect screen{0.f, 0.f, 1280.f, 720.f};
    // A slow drift (zoom and pan), so the art feels alive; launching, it leans in.
    const bool motion = m_settings.motion && m_settings.bgMotion;
    const int style = launchStyle();
    const float lean = !m_leaving ? 0.f
                     : style == kDive ? 0.45f * easeIn(m_leaveT)
                     : style == kZoom || style == kFlip ? 0.10f * easeInOut(m_leaveT) : 0.f;
    const float zoom = (motion ? 1.06f + 0.03f * std::sin(m_time * 0.11f) : 1.04f) + lean;
    const float panX = motion ? 0.25f * std::sin(m_time * 0.07f) : 0.f;
    const float panY = motion ? 0.2f * std::cos(m_time * 0.05f) : 0.f;
    if (m_bgFade < 1.f)
        if (const nxui::Texture* prev = bgOf(m_bgPrevId)) drawCover(r, prev, screen, 0.f, kWhite, 0.f, 0.f, zoom, panX, panY);
    if (const nxui::Texture* cur = bgOf(m_bgId))
        drawCover(r, cur, screen, 0.f, kWhite.withAlpha(easeOut(m_bgFade)), 0.f, 0.f, zoom, panX, panY);
    // The haze over it (Settings > Background softness), and a little depth at the edges.
    const float haze = static_cast<float>(m_settings.haze) / 100.f
                     * (1.f - (m_leaving && (style == kZoom || style == kFlip) ? easeOut(m_leaveT * 2.f) : 0.f));
    r.drawRect(screen, nxui::Color(0.38f, 0.43f, 0.56f, haze));
    r.drawGradientRect({0.f, 0.f, 1280.f, 140.f}, nxui::Color(0.05f, 0.06f, 0.10f, 0.35f), nxui::Color(0.05f, 0.06f, 0.10f, 0.f));
    r.drawGradientRect({0.f, 560.f, 1280.f, 160.f}, nxui::Color(0.05f, 0.06f, 0.10f, 0.f), nxui::Color(0.05f, 0.06f, 0.10f, 0.40f));
}

void CardsActivity::drawTopBar(nxui::Renderer& r, float a) {
    if (a <= 0.001f) return;
    const bool top = m_zone == Zone::Top && !m_moving;
    const float y = kTopY - 18.f * (1.f - a);
    const float ring = 0.8f + 0.2f * std::sin(m_time * 5.f);
    auto pressedOf = [this](int index) {
        return m_pressZone == 0 && m_pressIndex == index ? std::sin(clamp01(m_pressT) * 3.14159f) : 0.f;
    };
    auto badge = [&](float x, float cy, int count) {
        const std::string n = count > 99 ? "99+" : std::to_string(count);
        const float bw = std::max(24.f, measured(m_fBadge, n).x + 14.f);
        const float s = 1.f + (m_settings.motion ? 0.06f * std::sin(m_time * 4.f) : 0.f);
        const nxui::Rect b = scaled({x, cy, bw, 24.f}, s);
        r.drawRoundedRect(b, kRed.withAlpha(a), b.height * 0.5f);
        textMid(r, m_fBadge, n, b.x + (b.width - measured(m_fBadge, n).x) * 0.5f, b.y + b.height * 0.5f, kWhite.withAlpha(a));
    };
    glassCircle(r, kMenuX, y, kCircle, top && m_topSel == kTopMenu, "dots", 34.f, a, pressedOf(kTopMenu));
    glassCircle(r, kDiscordX, y, kCircle, top && m_topSel == kTopDiscord, "discord", 40.f, a, pressedOf(kTopDiscord));
#ifdef SWITCHU_MENU
    const int dms = m_discord.started() ? m_discord.unreadDms() : 0;
    if (dms > 0) badge(kDiscordX + 14.f, y - 40.f, dms);
#endif
    glassCircle(r, kSortX, y, kCircle, top && m_topSel == kTopSort, "sort", 34.f, a, pressedOf(kTopSort));

    // The pill: you (and whether you're online), the time, battery, free space, Nextendo.
    const float face = 34.f, gap = 16.f;
    const bool showFree = m_settings.showFree && !m_free.empty();
    const std::string players = m_playersTotal < 0 ? "?" : std::to_string(m_playersTotal);
    const float wClock = measured(m_fPill, m_clock).x, wBattery = measured(m_fPill, m_battery).x;
    const float wFree = showFree ? measured(m_fPill, m_free).x : 0.f;
    const float wPlayers = 30.f + 6.f + measured(m_fPill, players).x;
    const float width = 9.f + face + 14.f + wClock + gap + wBattery + (showFree ? gap + 26.f + wFree : 0.f)
                      + (m_settings.players ? gap + wPlayers : 0.f) + 18.f;
    const nxui::Rect pill{kPillRight - width, y - 26.f, width, 52.f};
    frost(r, pill, 26.f, glassTint(a));
    r.drawRoundedRectOutline(pill, kGlassEdge.withAlpha(kGlassEdge.a * a), 26.f, 1.5f);
    float x = pill.x + 9.f;
    const nxui::Rect faceRect{x, y - face * 0.5f, face, face};
    if (m_avatar && m_avatar->valid()) r.drawTextureRounded(m_avatar.get(), faceRect, face * 0.5f, kWhite.withAlpha(a));
    else r.drawCircle({x + face * 0.5f, y}, face * 0.5f, nxui::Color(1.f, 1.f, 1.f, 0.3f * a), 32);
    // Online: a green dot on the picture.
    r.drawCircle({x + face - 3.f, y + face * 0.5f - 3.f}, 7.f, nxui::Color(0.08f, 0.09f, 0.12f, a), 16);
    r.drawCircle({x + face - 3.f, y + face * 0.5f - 3.f}, 5.f,
                 (m_online ? kGreen : nxui::Color(0.6f, 0.62f, 0.66f, 1.f)).withAlpha(a), 16);
    m_userHit = {x - 9.f, y - 26.f, face + 18.f, 52.f};
    if (top && m_topSel == kTopUser)
        r.drawRoundedRectOutline(scaled(faceRect, 1.3f), accent(ring * a), face * 0.65f, 3.f);
    x += face + 14.f;
    textMid(r, m_fPill, m_clock, x, y, kWhite.withAlpha(a));
    x += wClock + gap;
    textMid(r, m_fPill, m_battery, x, y, kWhite.withAlpha(a));
    x += wBattery;
    if (showFree) {
        x += gap;
        icon(r, "thermo", x + 9.f, y, 24.f, nxui::Color(0.39f, 0.82f, 1.f, a));
        x += 26.f;
        textMid(r, m_fPill, m_free, x, y, kWhite.withAlpha(a));
        x += wFree;
    }
    if (m_settings.players) {
        x += gap;
        m_playersHit = {x - 8.f, y - 22.f, wPlayers + 16.f, 44.f};
        icon(r, "friends", x + 15.f, y, 30.f, kWhite.withAlpha(a));
        textMid(r, m_fPill, players, x + 36.f, y, kWhite.withAlpha(a));
        if (top && m_topSel == kTopPlayers) r.drawRoundedRectOutline(m_playersHit, accent(ring * a), 22.f, 3.f);
    }

    // The label: the sort (or page) and how many, in the room left of the pill.
    const std::string label = (m_page == kGames ? std::string(kSortNames[m_settings.sort]) : std::string(kPageNames[m_page]))
                            + "  " + std::to_string(games().size());
    const float room = std::max(40.f, pill.x - 14.f - kLabelX);
    const float ls = fitScale(m_fLabel, label, room, 0.72f);
    textMid(r, m_fLabel, fit(m_fLabel, label, room, ls), kLabelX, y, kWhite.withAlpha(a), ls);

    // The bell, music, power, settings.
    icon(r, "bell", kBellX, y, 36.f, kWhite.withAlpha(a));
    if (top && m_topSel == kTopBell)
        r.drawRoundedRectOutline({kBellX - 26.f, y - 26.f, 52.f, 52.f}, accent(ring * a), 26.f, 3.f);
    if (m_unread > 0) badge(kBellX + 6.f, y - 37.f, m_unread);
    glassCircle(r, kMusicX, y, kCircle, top && m_topSel == kTopMusic, "music", 34.f, a, pressedOf(kTopMusic));
    if (m_settings.music) { // playing: a little level meter under the note
        for (int i = 0; i < 3; ++i) {
            const float hgt = 3.f + 5.f * (0.5f + 0.5f * std::sin(m_time * (5.f + i * 1.7f) + i));
            r.drawRect({kMusicX - 8.f + i * 6.f, y + 25.f - hgt, 3.f, hgt}, kWhite.withAlpha(0.8f * a));
        }
    }
    glassCircle(r, kPowerX, y, kCircle, top && m_topSel == kTopPower, "power", 34.f, a, pressedOf(kTopPower));
    glassCircle(r, kGearX, y, kCircle, top && m_topSel == kTopGear, "gear", 36.f, a, pressedOf(kTopGear));
}

bool CardsActivity::cardRect(int index, nxui::Rect& card, float& chosen) {
    const float rel = static_cast<float>(index) - m_scroll;
    // A page switch slides the row in from the side.
    float slide = (1.f - easeOut(m_pageIn)) * 160.f * static_cast<float>(m_pageDir);
    // Launching: the others make way.
    if (m_leaving && m_leaveIndex >= 0 && index != m_leaveIndex && launchStyle() == kZoom)
        slide += (index < m_leaveIndex ? -1.f : 1.f) * 420.f * easeIn(m_leaveT / 0.5f);
    const float x0 = kCardFirstX + rel * kCardPitch + slide;
    if (x0 > 1290.f || x0 + kCardPitch < -10.f) return false;
    chosen = clamp01(1.f - std::abs(rel));
    const float grow = easeOutBack(chosen);
    float w = kCardW + kCardGrowW * grow, h = kCardH + kCardGrowH * grow;
    // Opening: the cards rise in one after another.
    const float k = std::max(0.f, rel + 1.f);
    const float rise = 1.f - easeOut((m_intro - 0.1f - 0.07f * k) / 0.5f);
    float lift = 7.f * chosen;
    if (m_moving && index == sel()) { // picked up
        w *= 1.05f;
        h *= 1.05f;
        lift += 22.f + 3.f * std::sin(m_time * 6.f);
    }
    card = {x0 + kCardW * 0.5f - w * 0.5f, kCardMidY - h * 0.5f - lift + 60.f * rise, w, h};
    if (m_leaving) {
        const int style = launchStyle();
        if (style == kFade && index == m_leaveIndex) { // a smooth hop: up, a little bigger, and back down
            const float hop = 0.5f - 0.5f * std::cos(6.28318f * clamp01(m_leaveT / 0.6f));
            card = scaled(card, 1.f + 0.05f * hop);
            card.y -= 34.f * hop;
        }
        if (style == kDive) { // the whole row rushes at you
            const float k = 1.f + 2.4f * easeIn(m_leaveT / 0.8f);
            card = {640.f + (card.x - 640.f) * k, 330.f + (card.y - 330.f) * k, card.width * k, card.height * k};
        }
    }
    return true;
}

void CardsActivity::drawCards(nxui::Renderer& r) {
    auto& page = games();
    if (page.empty()) {
        textMid(r, m_fLabel, m_page == kHomebrew ? "No homebrew in sdmc:/switch"
                             : m_page == kMedia  ? "No media apps yet"
                                                 : "No games installed", 480.f, kCardMidY,
                kWhite);
        return;
    }
    const bool cardsFocused = m_zone == Zone::Cards;
    const float pageAlpha = easeOut(m_pageIn);
    const float making = othersAlpha(); // launching: the others fade
    for (int i = 0; i < static_cast<int>(page.size()); ++i) {
        if (launchDrawsCard() && i == m_leaveIndex) continue; // drawn on top (drawLaunch)
        nxui::Rect card;
        float t = 0.f;
        if (!cardRect(i, card, t)) continue;
        const Game& g = page[(size_t)i];
        if (glassCard(g)) continue; // glass: drawn over the blur (drawCardExtras)
        const float k = std::max(0.f, static_cast<float>(i) - m_scroll + 1.f);
        const float alpha = pageAlpha * easeOut((m_intro - 0.1f - 0.07f * k) / 0.5f) * making;
        if (alpha <= 0.01f) continue;
        const float pressed = m_pressZone == 1 && m_pressIndex == i ? std::sin(clamp01(m_pressT) * 3.14159f) : 0.f;
        if (pressed > 0.f) card = scaled(card, 1.f - 0.05f * pressed);

        const float shadow = 0.18f + 0.18f * t + (m_moving && i == sel() ? 0.2f : 0.f);
        r.drawRoundedRect({card.x - 3.f, card.y + 6.f + 6.f * t, card.width + 6.f, card.height + 6.f},
                          nxui::Color(0.f, 0.f, 0.f, shadow * alpha), kCardRadius + 3.f);
        auto it = m_art.find(g.id);
        const nxui::Texture* tex = it != m_art.end() && it->second.card && it->second.card->valid() ? it->second.card.get()
                                                                                                    : nullptr;
        const float artIn = tex ? easeOut((m_time - it->second.cardIn) / 0.35f) : 0.f;
        // Under the art (and while it's coming): the name on glass.
        if (artIn < 1.f) {
            r.drawRoundedRect(card, nxui::Color(0.28f, 0.31f, 0.40f, 0.85f * alpha), kCardRadius);
            const std::string name = fit(m_fLabel, g.name, card.width - 28.f, 0.9f);
            textMid(r, m_fLabel, name, card.x + (card.width - measured(m_fLabel, name).x * 0.9f) * 0.5f,
                    card.y + card.height * 0.5f, kMuted.withAlpha(alpha * (1.f - artIn)), 0.9f);
        }
        if (tex) {
            // A Switch case cover without the red Nintendo Switch band on top:
            // the rest is the card's own shape, so all of it shows.
            const bool box = it->second.kind == ArtKind::Box;
            drawCover(r, tex, card, kCardRadius, kWhite.withAlpha(alpha * artIn), box ? 0.125f : 0.f, 0.f);
        }
        if (i < sel() && !m_moving) // the ones already passed sit back a little
            r.drawRoundedRect(card, nxui::Color(0.f, 0.f, 0.f, 0.30f * (1.f - t) * alpha), kCardRadius);
        if (t > 0.01f) {
            const float ring = cardsFocused ? 0.85f + 0.15f * std::sin(m_time * 4.f) : 0.55f;
            r.drawRoundedRectOutline(card, accent(ring * t * alpha), kCardRadius, 3.5f);
        }
    }
}

// Nextendo: players online in each game now (a glass pill, its dot breathing);
// what's open now; and the light that crosses the card just chosen.
void CardsActivity::drawCardExtras(nxui::Renderer& r) {
    auto& page = games();
    const float making = othersAlpha();
    for (int i = 0; i < static_cast<int>(page.size()); ++i) {
        if (launchDrawsCard() && i == m_leaveIndex) continue;
        nxui::Rect card;
        float t = 0.f;
        if (!cardRect(i, card, t)) continue;
        const float k = std::max(0.f, static_cast<float>(i) - m_scroll + 1.f);
        const float alpha = easeOut(m_pageIn) * easeOut((m_intro - 0.1f - 0.07f * k) / 0.5f) * making;
        if (alpha <= 0.01f) continue;

        const Game& g = page[(size_t)i];
        if (glassCard(g)) {
            // An app, in the glass of the rest of the screen: the blurred
            // background through it, its icon floating on a soft glow of its
            // own colours, its name and what it is below.
            const float pressed = m_pressZone == 1 && m_pressIndex == i ? std::sin(clamp01(m_pressT) * 3.14159f) : 0.f;
            if (pressed > 0.f) card = scaled(card, 1.f - 0.05f * pressed);
            r.drawRoundedRect({card.x - 3.f, card.y + 6.f + 6.f * t, card.width + 6.f, card.height + 6.f},
                              nxui::Color(0.f, 0.f, 0.f, (0.10f + 0.10f * t) * alpha), kCardRadius + 3.f);
            frost(r, card, kCardRadius, glassTint((1.f + 0.25f * t) * alpha));
            r.drawGradientRect({card.x + 2.f, card.y + 2.f, card.width - 4.f, 70.f},
                               nxui::Color(1.f, 1.f, 1.f, 0.07f * alpha), nxui::Color(1.f, 1.f, 1.f, 0.f)); // a sheen of glass
            auto it = m_art.find(g.id);
            const nxui::Texture* tex = it != m_art.end() && it->second.card && it->second.card->valid() ? it->second.card.get()
                                                                                                        : nullptr;
            const float artIn = tex ? easeOut((m_time - it->second.cardIn) / 0.35f) : 0.f;
            // The icon inside the card's border, with room around it. Nintendo's
            // (HomeNX's own pictures) as they're drawn; an app's square icon rounded.
            const bool nintendo = !g.action.empty();
            const float side = (nintendo ? 160.f : 150.f) + 8.f * t;
            const nxui::Rect iconRect{card.x + (card.width - side) * 0.5f, card.y + (nintendo ? 36.f : 40.f) - 4.f * t,
                                      side, side};
            if (tex) {
                const float a = alpha * artIn;
                if (nintendo) {
                    r.drawTexture(tex, iconRect, kWhite.withAlpha(a));
                } else {
                    r.drawRoundedRect({iconRect.x, iconRect.y + 6.f, iconRect.width, iconRect.height},
                                      nxui::Color(0.f, 0.f, 0.f, 0.18f * a), 34.f); // a soft shadow under it
                    r.drawTextureRounded(tex, iconRect, 34.f, kWhite.withAlpha(a));
                }
            } else if (!nintendo) {
                r.drawRoundedRect(iconRect, nxui::Color(1.f, 1.f, 1.f, 0.08f * alpha), 34.f);
            }
            const float ns = fitScale(m_fLabel, g.name, card.width - 24.f);
            const std::string name = fit(m_fLabel, g.name, card.width - 24.f, ns);
            r.drawText(name, {card.x + (card.width - measured(m_fLabel, name).x * ns) * 0.5f, iconRect.y + side + 26.f},
                       &m_fLabel, kWhite.withAlpha(alpha), ns);
            const std::string about = !g.author.empty() ? g.author : (g.path.empty() ? "NRO forwarder" : "Homebrew");
            const std::string by = fit(m_fBody, about, card.width - 28.f);
            r.drawText(by, {card.x + (card.width - measured(m_fBody, by).x) * 0.5f, iconRect.y + side + 58.f}, &m_fBody,
                       kMuted.withAlpha(alpha));
            r.drawRoundedRectOutline(card, nxui::Color(1.f, 1.f, 1.f, 0.20f * alpha), kCardRadius, 1.5f);
            if (nroUpdate(g)) { // a newer version: a green "Update" tag in the corner
                const bool busy = g.path == m_nroUpdating;
                const std::string tag = busy ? std::to_string(static_cast<int>(m_nroProgress.load() * 100.f)) + "%" : "Update";
                const float tw = measured(m_fBadge, tag).x + 22.f;
                const nxui::Rect pill{card.x + card.width - tw - 12.f, card.y + 12.f, tw, 26.f};
                r.drawRoundedRect(pill, kGreen.withAlpha(alpha), 13.f);
                textMid(r, m_fBadge, tag, pill.x + 11.f, pill.y + 13.f, kWhite.withAlpha(alpha));
            }
            if (i < sel() && !m_moving) r.drawRoundedRect(card, nxui::Color(0.f, 0.f, 0.f, 0.22f * (1.f - t) * alpha), kCardRadius);
            if (t > 0.01f) {
                const float ring = m_zone == Zone::Cards ? 0.85f + 0.15f * std::sin(m_time * 4.f) : 0.55f;
                r.drawRoundedRectOutline(card, accent(ring * t * alpha), kCardRadius, 3.5f);
            }
        }

        if (m_moving && i == sel()) { // picked up: arrows either side
            const float bob = 4.f * std::sin(m_time * 6.f);
            glassCircle(r, card.x - 30.f - bob, card.y + card.height * 0.5f, 46.f, false, "arrows", 26.f, alpha, 0.f);
            glassCircle(r, card.x + card.width + 30.f + bob, card.y + card.height * 0.5f, 46.f, false, "arrows", 26.f,
                        alpha, 0.f);
        }

        if (i == sel() && m_sheen < 1.f && t > 0.5f) {
            const float e = easeOut(m_sheen);
            const float x = card.x - 120.f + (card.width + 240.f) * e;
            const float fade = std::sin(e * 3.14159f) * 0.28f * alpha;
            r.pushClipRect(card);
            for (int band = 0; band < 2; ++band) { // a wide soft band and a narrow bright one
                const float w = band ? 26.f : 80.f, al = band ? fade : fade * 0.45f;
                const nxui::Color c(1.f, 1.f, 1.f, al);
                r.drawTriangle({x, card.y}, {x + w, card.y}, {x - 90.f, card.y + card.height}, c);
                r.drawTriangle({x + w, card.y}, {x + w - 90.f, card.y + card.height}, {x - 90.f, card.y + card.height}, c);
            }
            r.popClipRect();
        }

        if (isRunning(g.id)) { // open now: a pill at the bottom of its card
            const std::string word = "Open now";
            const float w = 20.f + 10.f + measured(m_fBadge, word).x + 16.f;
            const nxui::Rect pill{card.x + (card.width - w) * 0.5f, card.y + card.height - 46.f, w, 32.f};
            frost(r, pill, 16.f, nxui::Color(0.05f, 0.07f, 0.09f, 0.55f * alpha));
            r.drawRoundedRectOutline(pill, nxui::Color(1.f, 1.f, 1.f, 0.2f * alpha), 16.f, 1.5f);
            icon(r, "play", pill.x + 20.f, pill.y + 16.f, 16.f, kGreen.withAlpha(alpha));
            textMid(r, m_fBadge, word, pill.x + 34.f, pill.y + 16.f, kWhite.withAlpha(alpha));
        }

        if (!m_settings.players) continue;
        auto p = m_players.find(page[(size_t)i].id);
        if (p == m_players.end() || p->second <= 0) continue;
        const std::string n = std::to_string(p->second);
        const nxui::Rect badge{card.x + 14.f, card.y + 14.f, 34.f + measured(m_fCount, n).x + 14.f, 34.f};
        frost(r, badge, 17.f, nxui::Color(0.05f, 0.07f, 0.09f, 0.42f * alpha));
        r.drawRoundedRectOutline(badge, nxui::Color(1.f, 1.f, 1.f, 0.16f * alpha), 17.f, 1.5f);
        const float breathe = m_settings.motion ? 0.5f + 0.5f * std::sin(m_time * 3.f) : 0.5f;
        r.drawCircle({badge.x + 18.f, badge.y + 17.f}, 8.f + 4.f * breathe, kGreen.withAlpha(0.25f * (1.f - breathe) * alpha), 24);
        r.drawCircle({badge.x + 18.f, badge.y + 17.f}, 8.f, kGreen.withAlpha(alpha), 24);
        textMid(r, m_fCount, n, badge.x + 33.f, badge.y + 17.f, kWhite.withAlpha(alpha));
        if (i < sel() && !m_moving) r.drawRoundedRect(badge, nxui::Color(0.f, 0.f, 0.f, 0.30f * (1.f - t) * alpha), 17.f);
    }
}

int CardsActivity::launchStyle() const {
    return m_settings.motion ? std::clamp(m_settings.launchStyle, 0, kLaunchStyles - 1) : kNoAnimation;
}

bool CardsActivity::launchDrawsCard() const {
    const int style = launchStyle();
    return m_leaving && m_leaveIndex >= 0 && (style == kZoom || style == kFlip);
}

float CardsActivity::othersAlpha() const {
    if (!m_leaving) return 1.f;
    switch (launchStyle()) {
        case kFade:
        case kNoAnimation: return 1.f; // the screen dims over them
        case kDive: return 1.f - smoothstep(0.45f, 0.85f, m_leaveT);
        default: return 1.f - easeIn(m_leaveT / 0.5f);
    }
}

// Starting a game (Settings > Look > Launch animation):
//  Card jump     the card hops, then the screen fades into the game.
//  Splash screen the screen goes soft, the game's art fills it with its icon and name.
//  Card zoom     the card lifts and opens out to the whole screen.
//  Card flip     the card turns over to the game's banner, which fills the screen.
//  Dive in       the row rushes at you, a flash, then black.
void CardsActivity::drawLaunch(nxui::Renderer& r) {
    if (!m_leaving) return;
    const nxui::Rect screen{0.f, 0.f, 1280.f, 720.f};
    const float t = clamp01(m_leaveT);
    const int style = launchStyle();
    const Game* g = m_leaveIndex >= 0 && m_leaveIndex < static_cast<int>(games().size()) ? &games()[(size_t)m_leaveIndex]
                                                                                        : nullptr;
    auto it = g ? m_art.find(g->id) : m_art.end();
    const Art* art = it != m_art.end() ? &it->second : nullptr;
    const bool glass = g && glassCard(*g);
    auto valid = [](const std::unique_ptr<nxui::Texture>& tex) { return tex && tex->valid(); };
    // The game's art: at full size once it's here, else the background's.
    auto bigArt = [&]() -> const nxui::Texture* {
        if (g && valid(m_launchTex) && m_launchTexId == g->id) return m_launchTex.get();
        if (art && valid(art->bg) && art->bgKind != ArtKind::Icon) return art->bg.get();
        return nullptr;
    };
    auto iconOf = [&]() -> const nxui::Texture* {
        if (art && valid(art->icon)) return art->icon.get();
        if (glass && art && valid(art->card)) return art->card.get();
        return nullptr;
    };
    auto drawIcon = [&](const nxui::Texture* ic, const nxui::Rect& rect, float a) {
        if (g && !g->action.empty()) r.drawTexture(ic, rect, kWhite.withAlpha(a)); // HomeNX's icon as it's drawn
        else r.drawTextureRounded(ic, rect, rect.width * 0.22f, kWhite.withAlpha(a));
    };
    // The card's face (box art, or an app's icon on glass) in a rect.
    auto drawFace = [&](const nxui::Rect& rect, float radius) {
        if (glass) {
            frost(r, rect, radius, nxui::Color(0.10f, 0.12f, 0.17f, 0.55f));
            if (const nxui::Texture* ic = art && valid(art->card) ? art->card.get() : nullptr) {
                const float side = std::min(rect.width, rect.height) * 0.6f;
                drawIcon(ic, {rect.x + (rect.width - side) * 0.5f, rect.y + (rect.height - side) * 0.42f, side, side}, 1.f);
            }
        } else if (art && valid(art->card)) {
            drawCover(r, art->card.get(), rect, radius, kWhite, art->kind == ArtKind::Box ? 0.125f : 0.f, 0.f);
        } else {
            r.drawRoundedRect(rect, nxui::Color(0.28f, 0.31f, 0.40f, 1.f), radius);
        }
    };
    float black = 0.f;
    if (!g) {
        black = smoothstep(0.1f, 1.f, t); // nothing to show (power, a system applet): a fade
    } else if (style == kFade) {
        // Card jump: the card hops (cardRect), then everything fades into the game.
        black = smoothstep(0.5f, 1.f, t);
    } else if (style == kSplash) {
        // The screen goes soft and dark; the game's art comes up through it.
        const float soft = smoothstep(0.f, 0.3f, t);
        if (m_frosted) r.drawOffscreenRounded(0, screen, 0.f, kWhite.withAlpha(soft));
        r.drawRect(screen, nxui::Color(0.03f, 0.04f, 0.06f, 0.6f * soft));
        const float in = smoothstep(0.18f, 0.5f, t);
        if (const nxui::Texture* big = bigArt())
            drawCover(r, big, screen, 0.f, kWhite.withAlpha(in), 0.f, 0.f, 1.10f - 0.06f * easeOut(t));
        r.drawGradientRect({0.f, 300.f, 1280.f, 420.f}, nxui::Color(0.f, 0.f, 0.f, 0.f), nxui::Color(0.f, 0.f, 0.f, 0.6f * in));
        const float show = smoothstep(0.3f, 0.55f, t);
        if (const nxui::Texture* ic = iconOf()) {
            const float side = 140.f * (0.92f + 0.08f * easeOut((t - 0.3f) / 0.4f));
            drawIcon(ic, {640.f - side * 0.5f, 300.f - side * 0.5f, side, side}, show);
        }
        const float ts = fitScale(m_fTitle, g->name, 1000.f);
        const std::string name = fit(m_fTitle, g->name, 1000.f, ts);
        textMid(r, m_fTitle, name, 640.f - measured(m_fTitle, name).x * ts * 0.5f, 410.f, kWhite.withAlpha(show), ts);
        const std::string starting = std::string("Starting") + std::string(1 + static_cast<int>(m_time * 3.f) % 3, '.');
        textMid(r, m_fBody, starting, 640.f - measured(m_fBody, "Starting..").x * 0.5f, 450.f, kMuted.withAlpha(show));
        black = smoothstep(0.8f, 1.f, t);
    } else if (style == kZoom) {
        const float lift = easeOut(t / 0.14f);
        const float open = easeInOut((t - 0.12f) / 0.6f);
        nxui::Rect from = scaled(m_leaveFrom, 1.f + 0.06f * lift);
        from.y -= 12.f * lift;
        const nxui::Rect rect = lerpRect(from, screen, open);
        const float radius = kCardRadius * (1.f - open);
        r.drawRoundedRect({rect.x - 8.f, rect.y + 12.f, rect.width + 16.f, rect.height + 16.f},
                          nxui::Color(0.f, 0.f, 0.f, 0.35f * (1.f - open)), radius + 8.f);
        drawFace(rect, radius);
        // The game's own art takes over as it opens out.
        const float artA = smoothstep(0.f, 0.45f, open);
        if (!glass && artA > 0.f)
            if (const nxui::Texture* big = bigArt()) drawCover(r, big, rect, radius, kWhite.withAlpha(artA), 0.f, 0.f, 1.f + 0.06f * open);
        if (rect.width > 1100.f) // its name, briefly, before black
            textMid(r, m_fTitle, g->name, 64.f, 650.f, kWhite.withAlpha(smoothstep(0.55f, 0.7f, t) * (1.f - smoothstep(0.8f, 0.95f, t))));
        black = smoothstep(0.62f, 1.f, t);
    } else if (style == kFlip) {
        const float turn = easeInOut(t / 0.45f);           // to the middle, turning over
        const float open = easeInOut((t - 0.45f) / 0.38f); // the banner fills the screen
        const nxui::Rect middle{320.f, 180.f, 640.f, 360.f};
        nxui::Rect rect = open > 0.f ? lerpRect(middle, screen, open) : lerpRect(m_leaveFrom, middle, turn);
        const float radius = 22.f * (1.f - open);
        const bool back = turn >= 0.5f;
        if (open <= 0.f) { // turning: narrow at the half-way point
            const float w = rect.width * std::max(0.02f, std::abs(std::cos(3.14159f * turn)));
            rect = {rect.x + (rect.width - w) * 0.5f, rect.y, w, rect.height};
        }
        r.drawRoundedRect({rect.x - 6.f, rect.y + 10.f, rect.width + 12.f, rect.height + 12.f},
                          nxui::Color(0.f, 0.f, 0.f, 0.3f * (1.f - open)), radius + 6.f);
        if (!back) {
            drawFace(rect, radius);
        } else if (const nxui::Texture* big = glass ? nullptr : bigArt()) {
            drawCover(r, big, rect, radius, kWhite);
        } else { // no banner: its icon on glass
            frost(r, rect, radius, nxui::Color(0.10f, 0.12f, 0.17f, 0.7f));
            if (const nxui::Texture* ic = iconOf()) {
                const float side = std::min(rect.width, rect.height) * 0.5f;
                drawIcon(ic, {rect.x + (rect.width - side) * 0.5f, rect.y + (rect.height - side) * 0.5f, side, side}, 1.f);
            }
        }
        black = smoothstep(0.7f, 1.f, t);
    } else if (style == kDive) {
        const float flash = 0.85f * smoothstep(0.55f, 0.72f, t) * (1.f - smoothstep(0.72f, 0.9f, t));
        if (flash > 0.f) r.drawRect(screen, nxui::Color(1.f, 1.f, 1.f, flash));
        black = smoothstep(0.72f, 1.f, t);
    } else {
        black = t; // off: straight to black
    }
    if (m_left) black = 1.f;
    if (black > 0.f) r.drawRect(screen, nxui::Color(0.f, 0.f, 0.f, black));
}

const nxui::Texture* CardsActivity::gameIconTexture() {
    const Game* g = current();
    if (!g) return nullptr;
    auto it = m_art.find(g->id);
    return it != m_art.end() && it->second.icon && it->second.icon->valid() ? it->second.icon.get() : nullptr;
}

void CardsActivity::drawPanel(nxui::Renderer& r, float a) {
    if (a <= 0.001f) return;
    const nxui::Rect panel{kPanel.x, kPanel.y + 40.f * (1.f - a), kPanel.width, kPanel.height};
    frost(r, panel, kPanelRadius, glassTint(1.12f * a));
    r.drawRoundedRectOutline(panel, nxui::Color(1.f, 1.f, 1.f, 0.22f * a), kPanelRadius, 1.5f);
    const Game* gp = current();
    if (!gp) return;
    const Game& g = *gp;
    const bool running = isRunning(g.id);
    // Its details fade and slide in when the chosen game changes.
    const float d = easeOut(m_detailIn);
    const float da = a * d, dx = 14.f * (1.f - d);

    const nxui::Rect iconRect{panel.x + 25.f + dx, panel.y + 20.f, 78.f, 78.f};
    if (const nxui::Texture* ic = gameIconTexture()) {
        auto art = m_art.find(g.id);
        if (art != m_art.end() && art->second.iconKind == ArtKind::Title) // its own picture, no background: a little bigger
            r.drawTexture(ic, {iconRect.x - 6.f, iconRect.y - 6.f, iconRect.width + 12.f, iconRect.height + 12.f},
                          kWhite.withAlpha(da));
        else if (!g.action.empty()) r.drawTexture(ic, iconRect, kWhite.withAlpha(da)); // HomeNX's icon as it's drawn
        else r.drawTextureRounded(ic, iconRect, 18.f, kWhite.withAlpha(da));
    }
    const float tx = iconRect.x + iconRect.width + 15.f;
    const float maxW = panel.x + panel.width - 30.f - tx;
    const float ts = fitScale(m_fTitle, g.name, maxW);
    r.drawText(fit(m_fTitle, g.name, maxW, ts), {tx, panel.y + 20.f + (1.f - ts) * 14.f}, &m_fTitle, kWhite.withAlpha(da), ts);
    std::string about;
    if (!g.action.empty()) about = g.author;
    else if (g.homebrew) about = g.path.empty() ? "NRO forwarder" : (g.author.empty() ? "Homebrew" : "Homebrew by " + g.author);
    else if (running) about = playTime(g.playSeconds) + "    Open now: Resume carries on";
    else about = playTime(g.playSeconds) + "    " + lastPlayed(g.lastPlayed);
    if (const NroUpdate* u = nroUpdate(g)) about += "    Update to v" + u->latest + " (+)";
    r.drawText(fit(m_fBody, about, maxW), {tx, panel.y + 60.f}, &m_fBody, kMuted.withAlpha(da));
    if (!g.version.empty()) {
        icon(r, "tag", tx + 8.f, panel.y + 95.f, 17.f, kMuted.withAlpha(da));
        const std::string v = (g.version[0] == 'v' || g.version[0] == 'V') ? g.version : "v" + g.version;
        textMid(r, m_fBody, v, tx + 22.f, panel.y + 95.f, kMuted.withAlpha(da), 0.95f);
    }

    // Play, its options, the page button, back (as HOME: News).
    const bool bottom = m_zone == Zone::Bottom;
    auto pressedOf = [this](int index) {
        return m_pressZone == 2 && m_pressIndex == index ? std::sin(clamp01(m_pressT) * 3.14159f) : 0.f;
    };
    const float by = panel.y + 108.f;
    const nxui::Rect playRect = scaled({panel.x + 22.f, by, 262.f, 77.f}, 1.f - 0.06f * pressedOf(0));
    r.drawRoundedRect(playRect, kWhite.withAlpha(a), playRect.height * 0.5f);
    if (bottom && m_bottomSel == 0) {
        const float ring = 0.8f + 0.2f * std::sin(m_time * 5.f);
        r.drawRoundedRectOutline({playRect.x - 4.f, playRect.y - 4.f, playRect.width + 8.f, playRect.height + 8.f},
                                 accent(ring * a), playRect.height * 0.5f + 4.f, 3.f);
    }
    const nxui::Color ink(0.f, 0.f, 0.f, a);
    const char* word = running ? "Resume" : g.action == "get" ? "Get" : g.path.empty() && g.action.empty() && !g.homebrew ? "Play" : "Open";
    const float playW = 24.f + 10.f + measured(m_fPlay, word).x;
    const float px = playRect.x + (playRect.width - playW) * 0.5f;
    icon(r, "play", px + 12.f, playRect.y + playRect.height * 0.5f, 24.f, ink);
    textMid(r, m_fPlay, word, px + 34.f, playRect.y + playRect.height * 0.5f, ink);
    glassCircle(r, panel.x + 22.f + 262.f + 8.f + 38.5f, by + 38.5f, 77.f, bottom && m_bottomSel == 1, "dots", 32.f, a,
                pressedOf(1));
    const float right = panel.x + panel.width - 22.f;
    glassCircle(r, right - 77.f - 8.f - 38.5f, by + 38.5f, 77.f, bottom && m_bottomSel == 2, "copy", 32.f, a, pressedOf(2));
#ifdef SWITCHU_MENU
    glassCircle(r, right - 38.5f, by + 38.5f, 77.f, bottom && m_bottomSel == 3, "news", 32.f, a, pressedOf(3));
#else
    glassCircle(r, right - 38.5f, by + 38.5f, 77.f, bottom && m_bottomSel == 3, "home", 34.f, a, pressedOf(3));
#endif
    // Which page this is: three dots under the page button, in your order.
    for (int p = 0; p < kPageCount; ++p) {
        const bool here = m_settings.pageOrder[(size_t)p] == m_page;
        const float cx = right - 77.f - 8.f - 38.5f + (static_cast<float>(p) - 1.f) * 12.f;
        r.drawCircle({cx, by + 77.f + 9.f}, here ? 3.5f : 2.5f, kWhite.withAlpha((here ? 0.95f : 0.4f) * a), 12);
    }
}

// One row: a toggle switch, a slider, a choice (< value >), a value, or an action.
void CardsActivity::drawRow(nxui::Renderer& r, MenuItem& item, const nxui::Rect& row, bool chosen, float a, bool wide) {
    const float ring = 0.8f + 0.2f * std::sin(m_time * 5.f);
    if (chosen) {
        r.drawRoundedRect(row, nxui::Color(1.f, 1.f, 1.f, 0.12f * a), 16.f);
        r.drawRoundedRectOutline(row, accent(ring * a), 16.f, 2.f);
    }
    const float cy = row.y + row.height * 0.5f;
    float x = row.x + 18.f;
    if (item.picture) {
        const float s = std::min(40.f, row.height - 10.f);
        r.drawTextureRounded(item.picture, {x, cy - s * 0.5f, s, s}, s * 0.5f, kWhite.withAlpha(a));
        x += s + 14.f;
    }
    // The control on the right, and the room it leaves for the label.
    const float right = row.x + row.width - 18.f;
    float control = 0.f;
    const nxui::Color blue = nxui::Color(0.55f, 0.75f, 1.f, a);
    switch (item.kind) {
        case MenuItem::Toggle: {
            const float on = item.level ? item.level() : 0.f;
            if (item.anim < 0.f) item.anim = on;
            item.anim += (on - item.anim) * (m_settings.motion ? 0.3f : 1.f);
            const nxui::Rect track{right - 56.f, cy - 16.f, 56.f, 32.f};
            r.drawRoundedRect(track, nxui::Color(1.f, 1.f, 1.f, 0.22f * a), 16.f);
            r.drawRoundedRect(track, kGreen.withAlpha(item.anim * a), 16.f);
            r.drawCircle({track.x + 16.f + 24.f * item.anim, cy}, 13.f, kWhite.withAlpha(a), 24);
            control = 70.f;
            break;
        }
        case MenuItem::Slider: {
            const float v = item.level ? clamp01(item.level()) : 0.f;
            const float bw = wide ? 220.f : 150.f;
            const nxui::Rect bar{right - bw, cy - 4.f, bw, 8.f};
            r.drawRoundedRect(bar, nxui::Color(1.f, 1.f, 1.f, 0.22f * a), 4.f);
            r.drawRoundedRect({bar.x, bar.y, std::max(8.f, bar.width * v), bar.height}, accent(a), 4.f);
            r.drawCircle({bar.x + bar.width * v, cy}, 11.f, kWhite.withAlpha(a), 24);
            const std::string pct = item.value ? item.value() : std::string();
            textMid(r, m_fBody, pct, bar.x - 16.f - measured(m_fBody, pct).x, cy, kMuted.withAlpha(a));
            control = bw + 70.f;
            break;
        }
        case MenuItem::Choice: {
            const std::string v = item.value ? item.value() : std::string();
            const float vw = measured(m_fPill, v).x;
            textMid(r, m_fPill, "<", right - vw - 40.f, cy, kMuted.withAlpha(0.7f * a));
            textMid(r, m_fPill, v, right - vw - 16.f, cy, blue);
            textMid(r, m_fPill, ">", right - 8.f, cy, kMuted.withAlpha(0.7f * a));
            control = vw + 56.f;
            break;
        }
        default: {
            const std::string v = item.value ? item.value() : std::string();
            if (!v.empty()) { // as much room as the label leaves (a song's whole title)
                const float maxV = std::max(120.f, right - x - measured(m_fPill, item.label).x - 30.f);
                const std::string shown = fit(m_fPill, v, maxV);
                textMid(r, m_fPill, shown, right - measured(m_fPill, shown).x, cy,
                        item.kind == MenuItem::Info ? kMuted.withAlpha(a) : blue);
                control = measured(m_fPill, shown).x + 16.f;
            }
            break;
        }
    }
    const float room = right - control - x;
    const nxui::Color labelColour = item.danger ? nxui::Color(1.f, 0.45f, 0.42f, a)
                                  : item.kind == MenuItem::Info && !item.value ? kMuted.withAlpha(a)
                                                                               : kWhite.withAlpha(a);
    if (wide && !item.detail.empty()) {
        textMid(r, m_fPill, fit(m_fPill, item.label, room), x, cy - 11.f, labelColour);
        textMid(r, m_fBody, fit(m_fBody, item.detail, room, 0.88f), x, cy + 13.f, kMuted.withAlpha(0.8f * a), 0.88f);
    } else {
        textMid(r, m_fPill, fit(m_fPill, item.label, room), x, cy, labelColour);
    }
}

void CardsActivity::drawPopup(nxui::Renderer& r) {
    const float a = easeOut(m_menuIn);
    if (m_notifOpen) {
        const float rows = static_cast<float>(std::max<size_t>(1, m_notifRows.size()));
        nxui::Rect box{kGearX + 38.f - 460.f, 84.f - 12.f * (1.f - a), 460.f, 60.f + rows * 64.f + 40.f};
        frost(r, box, 24.f, nxui::Color(0.10f, 0.11f, 0.15f, 0.82f * a));
        r.drawRoundedRectOutline(box, nxui::Color(1.f, 1.f, 1.f, 0.14f * a), 24.f, 1.5f);
        r.drawText("Notifications", {box.x + 22.f, box.y + 16.f}, &m_fLabel, kWhite.withAlpha(a));
        if (m_notifRows.empty())
            r.drawText("You're all caught up", {box.x + 22.f, box.y + 70.f}, &m_fBody, kMuted.withAlpha(a));
        for (size_t i = 0; i < m_notifRows.size(); ++i) {
            const float y = box.y + 60.f + static_cast<float>(i) * 64.f;
            r.drawText(fit(m_fPill, m_notifRows[i].first, box.width - 44.f), {box.x + 22.f, y + 6.f}, &m_fPill,
                       kWhite.withAlpha(a));
            r.drawText(fit(m_fBody, m_notifRows[i].second, box.width - 44.f), {box.x + 22.f, y + 32.f}, &m_fBody,
                       kMuted.withAlpha(a));
        }
        r.drawText(m_notifRows.empty() ? "B  Close" : "X  Clear all      B  Close",
                   {box.x + 22.f, box.y + box.height - 32.f}, &m_fBody, kMuted.withAlpha(a));
        return;
    }
    if (m_menu.empty()) return;
    r.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.42f * a));
    nxui::Rect base;
    float rowH = 56.f;
    popupGeometry(m_menu.size(), base, rowH);
    const nxui::Rect box = scaled(base, 0.94f + 0.06f * a);
    frost(r, box, 28.f, nxui::Color(0.10f, 0.11f, 0.15f, 0.85f * a));
    r.drawRoundedRectOutline(box, nxui::Color(1.f, 1.f, 1.f, 0.14f * a), 28.f, 1.5f);
    r.drawText(fit(m_fLabel, m_menuTitle, box.width - 48.f), {box.x + 26.f, box.y + 22.f}, &m_fLabel, kWhite.withAlpha(a));
    for (size_t i = 0; i < m_menu.size(); ++i) {
        const nxui::Rect row{box.x + 14.f, box.y + 66.f + rowH * static_cast<float>(i), box.width - 28.f, rowH - 6.f};
        drawRow(r, m_menu[i], row, static_cast<int>(i) == m_menuSel, a, false);
    }
}

void CardsActivity::drawQuick(nxui::Renderer& r) {
    if (!m_quickOpen) return;
    const float a = easeOut(m_quickIn);
    const float ring = 0.8f + 0.2f * std::sin(m_time * 5.f);
    r.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.30f * a));
    const nxui::Rect p = quickPanel((1.f - a) * 480.f);
    frost(r, p, 30.f, nxui::Color(0.08f, 0.09f, 0.13f, 0.86f * a));
    r.drawRoundedRectOutline(p, nxui::Color(1.f, 1.f, 1.f, 0.16f * a), 30.f, 1.5f);
    r.drawGradientRect({p.x + 2.f, p.y + 2.f, p.width - 4.f, 70.f}, nxui::Color(1.f, 1.f, 1.f, 0.05f * a),
                       nxui::Color(1.f, 1.f, 1.f, 0.f));
    const float x0 = p.x + kQuickPad, w = quickInnerW();
    const nxui::Color card(1.f, 1.f, 1.f, 0.06f * a), cardEdge(1.f, 1.f, 1.f, 0.10f * a);
    auto box = [&](const nxui::Rect& rect, bool focused) {
        r.drawRoundedRect(rect, focused ? nxui::Color(1.f, 1.f, 1.f, 0.12f * a) : card, 18.f);
        r.drawRoundedRectOutline(rect, focused ? accent(ring * a) : cardEdge, 18.f, focused ? 2.5f : 1.f);
    };
    textMid(r, m_fLabel, "Quick Settings", x0 + 4.f, p.y + 32.f, kWhite.withAlpha(a));
    textMid(r, m_fPill, m_clock, p.x + p.width - kQuickPad - measured(m_fPill, m_clock).x, p.y + 32.f, kMuted.withAlpha(a));

    // The status: battery (a little battery, filled), how warm the console is.
    const nxui::Rect status{x0, p.y + kQStatusY, w, kQStatusH};
    box(status, false);
    const float sy = status.y + status.height * 0.5f;
    const nxui::Rect batt{status.x + 18.f, sy - 9.f, 34.f, 18.f};
    r.drawRoundedRectOutline(batt, kWhite.withAlpha(0.9f * a), 4.f, 2.f);
    r.drawRect({batt.x + batt.width + 1.f, sy - 4.f, 3.f, 8.f}, kWhite.withAlpha(0.9f * a));
    const float level = m_quick.battery < 0 ? 0.f : static_cast<float>(m_quick.battery) / 100.f;
    r.drawRoundedRect({batt.x + 3.f, batt.y + 3.f, (batt.width - 6.f) * level, batt.height - 6.f},
                      (m_quick.battery >= 0 && m_quick.battery <= 15 ? kRed : kGreen).withAlpha(a), 2.f);
    std::string power = m_quick.battery < 0 ? std::string("-") : std::to_string(m_quick.battery) + "%";
    if (m_quick.charging) power += "  Charging";
    textMid(r, m_fPill, power, batt.x + batt.width + 14.f, sy, kWhite.withAlpha(a));
    if (m_quick.temperature >= 0) {
        const int t = m_quick.temperature;
        const nxui::Color tone = t >= 60 ? kRed : t >= 48 ? nxui::Color(1.f, 0.72f, 0.3f, 1.f) : kGreen;
        const std::string temp = std::to_string(t) + "\xC2\xB0" "C  " + (t >= 60 ? "Hot" : t >= 48 ? "Warm" : "Optimal");
        const float tx = status.x + status.width - 18.f - measured(m_fPill, temp).x;
        r.drawCircle({tx - 12.f, sy}, 5.f, tone.withAlpha(a), 16);
        textMid(r, m_fPill, temp, tx, sy, kWhite.withAlpha(a));
    }

    // The music player: the song, its buttons, how far it is.
    const nxui::Rect player{x0, p.y + kQPlayerY, w, kQPlayerH};
    box(player, false);
    const std::string song = m_settings.music && !m_audio.trackName().empty() ? m_audio.trackName() : std::string("Not playing");
    const float ss = fitScale(m_fPill, song, w - 36.f, 0.8f);
    textMid(r, m_fPill, fit(m_fPill, song, w - 36.f, ss), player.x + 18.f, player.y + 26.f, kWhite.withAlpha(a), ss);
    textMid(r, m_fBody, "HOME music", player.x + 18.f, player.y + 50.f, kMuted.withAlpha(0.8f * a), 0.9f);
    const bool playing = m_settings.music && !m_audio.paused();
    const char* buttons[kQPlayerButtons] = {"prev", playing ? "pause" : "play", "next", "repeat", "shuffle", "playlist"};
    for (int i = 0; i < kQPlayerButtons; ++i) {
        const nxui::Vec2 c = quickPlayerButton(p, i);
        const bool on = (i == 3 && m_settings.repeat) || (i == 4 && m_settings.shuffle) || (i == 5 && m_quickPlaylist);
        const bool focused = m_quickRow == kQPlayer && m_quickCol == i && !m_quickPlaylist;
        const float d = i == 1 ? 52.f : 44.f;
        r.drawCircle(c, d * 0.5f, (i == 1 ? kWhite.withAlpha(0.92f * a) : on ? accent(0.35f * a) : nxui::Color(1.f, 1.f, 1.f, 0.08f * a)), 32);
        if (focused)
            r.drawRoundedRectOutline({c.x - d * 0.5f - 4.f, c.y - d * 0.5f - 4.f, d + 8.f, d + 8.f}, accent(ring * a), d * 0.5f + 4.f, 2.5f);
        icon(r, buttons[i], c.x, c.y, i == 1 ? 24.f : 22.f, i == 1 ? nxui::Color(0.f, 0.f, 0.f, a) : kWhite.withAlpha(a));
    }
    const float length = m_audio.duration();
    if (length > 0.f && m_settings.music) {
        const float done = clamp01(m_audio.position() / length);
        const float by = player.y + kQPlayerH - 18.f;
        const nxui::Rect bar{player.x + 62.f, by - 2.f, w - 124.f, 4.f};
        r.drawRoundedRect(bar, nxui::Color(1.f, 1.f, 1.f, 0.18f * a), 2.f);
        r.drawRoundedRect({bar.x, bar.y, bar.width * done, bar.height}, accent(a), 2.f);
        textMid(r, m_fBody, clockText(m_audio.position()), player.x + 16.f, by, kMuted.withAlpha(a), 0.85f);
        const std::string total = clockText(length);
        textMid(r, m_fBody, total, player.x + w - 16.f - measured(m_fBody, total).x * 0.85f, by, kMuted.withAlpha(a), 0.85f);
    }

    // Music, Sound Effects, Brightness: a label, the value, a bar to slide.
    struct Slider { const char* name; const char* glyph; int value; };
    const Slider sliders[3] = {
        {"Music", "music", m_settings.musicVolume},
        {"Sound Effects", "speaker", m_settings.soundVolume},
        {"Brightness", "sun", m_quick.brightness < 0.f ? -1 : static_cast<int>(std::lround(m_quick.brightness * 100.f))},
    };
    for (int i = 0; i < 3; ++i) {
        const nxui::Rect row{x0, p.y + kQSliderY[i], w, kQSliderH};
        box(row, m_quickRow == kQMusic + i && !m_quickPlaylist);
        icon(r, sliders[i].glyph, row.x + 26.f, row.y + row.height * 0.5f, 24.f, kWhite.withAlpha(a));
        textMid(r, m_fPill, sliders[i].name, row.x + 52.f, row.y + 18.f, kWhite.withAlpha(a), 0.92f);
        const std::string v = sliders[i].value < 0 ? std::string("-") : std::to_string(sliders[i].value) + "%";
        textMid(r, m_fBody, v, row.x + row.width - 18.f - measured(m_fBody, v).x, row.y + 18.f, kMuted.withAlpha(a));
        const nxui::Rect bar{row.x + 52.f, row.y + 36.f, row.width - 72.f, 6.f};
        const float f = sliders[i].value < 0 ? 0.f : static_cast<float>(sliders[i].value) / 100.f;
        r.drawRoundedRect(bar, nxui::Color(1.f, 1.f, 1.f, 0.18f * a), 3.f);
        r.drawRoundedRect({bar.x, bar.y, std::max(6.f, bar.width * f), bar.height}, accent(a), 3.f);
        r.drawCircle({bar.x + bar.width * f, bar.y + 3.f}, 8.f, kWhite.withAlpha(a), 20);
    }

    // Airplane Mode, Wi-Fi: switches.
    struct Switch { const char* name; const char* glyph; bool on; };
    const Switch switches[2] = {{"Airplane Mode", "airplane", m_quick.airplane},
                                {"Wi-Fi", "wifi", m_quick.wifi && !m_quick.airplane}};
    for (int i = 0; i < 2; ++i) {
        const nxui::Rect row{x0, p.y + kQToggleY[i], w, kQToggleH};
        box(row, m_quickRow == kQAirplane + i && !m_quickPlaylist);
        const float cy = row.y + row.height * 0.5f;
        icon(r, switches[i].glyph, row.x + 26.f, cy, 24.f, kWhite.withAlpha(a));
        textMid(r, m_fPill, switches[i].name, row.x + 52.f, cy, kWhite.withAlpha(a), 0.92f);
        const nxui::Rect track{row.x + row.width - 18.f - 52.f, cy - 14.f, 52.f, 28.f};
        r.drawRoundedRect(track, switches[i].on ? kGreen.withAlpha(a) : nxui::Color(1.f, 1.f, 1.f, 0.22f * a), 14.f);
        r.drawCircle({track.x + (switches[i].on ? 38.f : 14.f), cy}, 11.f, kWhite.withAlpha(a), 24);
    }

    // Power: Sleep, Restart, Power Off, Hekate.
    const char* powerGlyph[kQPowerButtons] = {"moon", "restart", "power", "hekate"};
    const char* powerName[kQPowerButtons] = {"Sleep", "Restart", "Power Off", "Hekate"};
    for (int i = 0; i < kQPowerButtons; ++i) {
        const nxui::Rect b = quickPowerButton(p, i);
        box(b, m_quickRow == kQPower && m_quickCol == i && !m_quickPlaylist);
        icon(r, powerGlyph[i], b.x + b.width * 0.5f, b.y + 38.f, 32.f, kWhite.withAlpha(a));
        const std::string n = powerName[i];
        textMid(r, m_fBody, n, b.x + (b.width - measured(m_fBody, n).x * 0.9f) * 0.5f, b.y + 76.f, kWhite.withAlpha(a), 0.9f);
    }

    // The playlist, beside the panel.
    if (m_quickPlaylist) {
        const float la = a * easeOut(m_quickListIn);
        const auto& titles = m_audio.titles();
        const nxui::Rect list = quickPlaylist(p, titles.size());
        const nxui::Rect shown{list.x + 20.f * (1.f - la), list.y, list.width, list.height};
        frost(r, shown, 24.f, nxui::Color(0.08f, 0.09f, 0.13f, 0.88f * la));
        r.drawRoundedRectOutline(shown, nxui::Color(1.f, 1.f, 1.f, 0.16f * la), 24.f, 1.5f);
        textMid(r, m_fLabel, "Playlist", shown.x + 20.f, shown.y + 28.f, kWhite.withAlpha(la));
        if (titles.empty()) textMid(r, m_fBody, "No songs", shown.x + 20.f, shown.y + 76.f, kMuted.withAlpha(la));
        r.pushClipRect({shown.x, shown.y + 52.f, shown.width, shown.height - 58.f});
        const float scroll = std::max(0.f, 46.f * static_cast<float>(m_quickListSel) - (shown.height - 120.f));
        for (size_t i = 0; i < titles.size(); ++i) {
            const nxui::Rect row{shown.x + 10.f, shown.y + 56.f + 46.f * static_cast<float>(i) - scroll, shown.width - 20.f, 40.f};
            const bool now = static_cast<long>(i) == m_audio.playing() && m_settings.music;
            if (static_cast<int>(i) == m_quickListSel) {
                r.drawRoundedRect(row, nxui::Color(1.f, 1.f, 1.f, 0.12f * la), 14.f);
                r.drawRoundedRectOutline(row, accent(ring * la), 14.f, 2.f);
            }
            if (now) icon(r, "music", row.x + 18.f, row.y + 20.f, 18.f, accent(la));
            textMid(r, m_fBody, fit(m_fBody, titles[i], row.width - 50.f), row.x + 36.f, row.y + 20.f,
                    (now ? accent(1.f) : kWhite).withAlpha(la));
        }
        r.popClipRect();
    }
}

// Settings: a glass sheet, its sections on the left and their rows on the right.
void CardsActivity::drawSheet(nxui::Renderer& r) {
    if (!m_sheetOpen || m_sheet.empty()) return;
    const float a = easeOut(m_sheetIn);
    r.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.45f * a));
    const nxui::Rect sheet = scaled(kSheet, 0.95f + 0.05f * a);
    frost(r, sheet, 34.f, nxui::Color(0.08f, 0.09f, 0.13f, 0.86f * a));
    r.drawRoundedRectOutline(sheet, nxui::Color(1.f, 1.f, 1.f, 0.16f * a), 34.f, 1.5f);
    r.drawGradientRect({sheet.x + 2.f, sheet.y + 2.f, sheet.width - 4.f, 80.f}, nxui::Color(1.f, 1.f, 1.f, 0.05f * a),
                       nxui::Color(1.f, 1.f, 1.f, 0.f));
    const float dy = sheet.y - kSheet.y;
    icon(r, "gear", kSecX + 20.f, kSheet.y + 46.f + dy, 32.f, kWhite.withAlpha(a));
    textMid(r, m_fTitle, "Settings", kSecX + 46.f, kSheet.y + 46.f + dy, kWhite.withAlpha(a));
    const std::string hint = "A Choose    < > Change    L R Section    B Back";
    textMid(r, m_fBody, hint, kSheet.x + kSheet.width - 30.f - measured(m_fBody, hint).x, kSheet.y + 46.f + dy,
            kMuted.withAlpha(0.8f * a));

    // Sections.
    const float ring = 0.8f + 0.2f * std::sin(m_time * 5.f);
    for (size_t i = 0; i < m_sheet.size(); ++i) {
        const nxui::Rect row{kSecX, kSheetTop + kSecH * static_cast<float>(i) + dy, kSecW, kSecH - 8.f};
        const bool on = static_cast<int>(i) == m_sheetSec;
        if (on) {
            r.drawRoundedRect(row, nxui::Color(1.f, 1.f, 1.f, (m_sheetOnRows ? 0.10f : 0.18f) * a), 16.f);
            if (!m_sheetOnRows) r.drawRoundedRectOutline(row, accent(ring * a), 16.f, 2.f);
        }
        icon(r, m_sheet[i].icon, row.x + 26.f, row.y + row.height * 0.5f, 24.f, kWhite.withAlpha((on ? 1.f : 0.7f) * a));
        textMid(r, m_fPill, m_sheet[i].name, row.x + 50.f, row.y + row.height * 0.5f, kWhite.withAlpha((on ? 1.f : 0.75f) * a));
    }
    r.drawRect({kRowsX - 18.f, kSheetTop + dy, 1.5f, kRowsBottom - kSheetTop}, nxui::Color(1.f, 1.f, 1.f, 0.10f * a));

    // Rows (scrolled to keep the chosen one in view; they slide in on a new section).
    auto& rows = m_sheet[(size_t)m_sheetSec].rows;
    const float visible = kRowsBottom - kSheetTop;
    const float want = std::clamp(kRowH * static_cast<float>(m_sheetRow) - visible * 0.5f + kRowH * 0.5f, 0.f,
                                  std::max(0.f, kRowH * static_cast<float>(rows.size()) - visible));
    m_sheetScroll += (want - m_sheetScroll) * (m_settings.motion ? 0.25f : 1.f);
    const float ra = a * easeOut(m_sheetRowsIn), slide = 24.f * (1.f - easeOut(m_sheetRowsIn));
    r.pushClipRect({kRowsX - 8.f, kSheetTop + dy - 4.f, kRowsW + 16.f, visible + 8.f});
    for (size_t i = 0; i < rows.size(); ++i) {
        const nxui::Rect row{kRowsX + slide, kSheetTop + kRowH * static_cast<float>(i) - m_sheetScroll + dy, kRowsW, kRowH - 8.f};
        if (row.y > kRowsBottom + dy || row.y + row.height < kSheetTop + dy) continue;
        if (static_cast<int>(i) != m_sheetRow || !m_sheetOnRows)
            r.drawRoundedRect(row, nxui::Color(1.f, 1.f, 1.f, 0.04f * ra), 16.f);
        drawRow(r, rows[i], row, m_sheetOnRows && static_cast<int>(i) == m_sheetRow, ra, true);
    }
    r.popClipRect();
}

} // namespace cards
