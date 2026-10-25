#include "CardsActivity.hpp"

#include "social/NotificationLog.hpp"

#include <nxui/Application.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/third_party/stb/stb_image.h>
#include <nxui/widgets/Widget.hpp>
#include <switch.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace cards {
namespace {

// The layout: the reference screenshot (2000x920) on 1280x720. Sizes follow
// its height (x0.78), positions its width (x0.64).
constexpr float kTopY = 47.f, kCircle = 77.f;
constexpr float kCardW = 227.f, kCardH = 340.f, kCardGrowW = 9.f, kCardGrowH = 14.f;
constexpr float kCardMidY = 291.f, kCardPitch = 236.f, kCardFirstX = 136.f, kCardRadius = 19.f;
const nxui::Rect kPanel{186.f, 503.f, 907.f, 207.f};
constexpr float kPanelRadius = 31.f;

const nxui::Color kWhite(1.f, 1.f, 1.f, 1.f);
const nxui::Color kMuted(1.f, 1.f, 1.f, 0.82f);
const nxui::Color kGlass(0.42f, 0.46f, 0.56f, 0.55f);
const nxui::Color kGlassEdge(1.f, 1.f, 1.f, 0.22f);
const nxui::Color kGreen(0.204f, 0.78f, 0.349f, 1.f); // #34C759
const nxui::Color kRed(1.f, 0.231f, 0.188f, 1.f);     // #FF3B30

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

std::string fit(nxui::Font& font, std::string text, float maxW, float scale = 1.f) {
    if (font.measure(text).x * scale <= maxW) return text;
    while (!text.empty() && font.measure(text + "...").x * scale > maxW) {
        size_t cut = text.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.erase(cut);
    }
    return text + "...";
}

// Text drawn with its middle at y.
void textMid(nxui::Renderer& r, nxui::Font& font, const std::string& s, float x, float y, const nxui::Color& c,
             float scale = 1.f) {
    r.drawText(s, {x, y - font.measure(s.empty() ? std::string("A") : s).y * scale * 0.5f}, &font, c, scale);
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

// A picture drawn to fill `dst`, cropped to its shape (centred).
void drawCover(nxui::Renderer& r, const nxui::Texture* tex, const nxui::Rect& dst, float radius, const nxui::Color& tint) {
    const float tw = static_cast<float>(tex->width()), th = static_cast<float>(tex->height());
    const float scale = std::max(dst.width / tw, dst.height / th);
    const float sw = dst.width / scale, sh = dst.height / scale;
    const nxui::Rect src{(tw - sw) * 0.5f, (th - sh) * 0.5f, sw, sh};
    if (radius > 0.f) r.drawTextureSubRounded(tex, src, dst, radius, tint);
    else r.drawTextureSub(tex, src, dst, tint);
}

} // namespace

// ---------------------------------------------------------------- set up

bool CardsActivity::onCreate() {
    auto& gpu = app().gpu();
    auto& ren = app().renderer();
    ren.setBoxWireframeEnabled(false);
    // Names in other scripts (Japanese, Chinese, Korean) from the console's own fonts.
    if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
        for (PlSharedFontType type : {PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified,
                                      PlSharedFontType_ChineseTraditional, PlSharedFontType_KO,
                                      PlSharedFontType_NintendoExt}) {
            PlFontData font{};
            if (R_SUCCEEDED(plGetSharedFontByType(&font, type))) nxui::Font::addFallback(font.address, font.size);
        }
    }
    const char* f5 = "romfs:/fonts/Inter-500.ttf";
    const char* f6 = "romfs:/fonts/Inter-600.ttf";
    const char* f7 = "romfs:/fonts/Inter-700.ttf";
    if (!m_fTitle.load(gpu, ren, f7, 30) || !m_fPlay.load(gpu, ren, f7, 24) || !m_fLabel.load(gpu, ren, f6, 22)
        || !m_fPill.load(gpu, ren, f6, 19) || !m_fBody.load(gpu, ren, f5, 17) || !m_fBadge.load(gpu, ren, f7, 15))
        return false;
    for (const char* name : {"dots", "sort", "friends", "bell", "gear", "thermo", "tag", "play", "copy", "home"}) {
        auto tex = std::make_unique<nxui::Texture>();
        if (tex->loadFromFile(gpu, ren, std::string("romfs:/icons/") + name + ".png", 96)) m_icons[name] = std::move(tex);
    }

    m_games = listGames();
    sortGames(Sort::LastPlayed);

    // You: the last account that played.
    AccountUid uid{};
    if (R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid)) {
        AccountProfile profile{};
        if (R_SUCCEEDED(accountGetProfile(&profile, uid))) {
            u32 size = 0;
            if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &size)) && size > 0) {
                std::vector<std::uint8_t> jpeg(size);
                u32 got = 0;
                if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpeg.data(), size, &got)) && got > 0) {
                    m_avatar = std::make_unique<nxui::Texture>();
                    if (!m_avatar->loadFromMemory(gpu, ren, jpeg.data(), got, 96)) m_avatar.reset();
                }
            }
            accountProfileClose(&profile);
        }
    }
    refreshStatus();
    m_unread = notifications::unreadCount();
    m_key = steamGridDbKey();

    m_painter = std::make_shared<Painter>([this](nxui::Renderer& r) { draw(r); });
    rootBox().addChild(m_painter);
    m_thread = std::thread([this]() { worker(); });
    wantArt();
    return true;
}

void CardsActivity::onDestroy() {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_quit = true;
        m_jobs.clear();
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

// ---------------------------------------------------------------- the worker

void CardsActivity::worker() {
    const bool net = startNetwork();
    std::uint64_t countsAt = 0;
    for (;;) {
        Job job;
        bool haveJob = false;
        {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait_for(lk, std::chrono::seconds(1), [this]() { return m_quit || !m_jobs.empty(); });
            if (m_quit) break;
            if (!m_jobs.empty()) {
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
                haveJob = true;
            }
        }
        if (!haveJob) { // pictures first; Nextendo's players every minute
            const std::uint64_t now = armTicksToNs(armGetSystemTick()) / 1000000ULL;
            if (net && now >= countsAt) {
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
        done.background = job.background;
        const std::vector<std::uint8_t> data =
            job.background ? backgroundArt(job.game, m_key) : cardArt(job.game, m_key, done.tall);
        if (!data.empty()) {
            int w = 0, h = 0, ch = 0;
            if (stbi_uc* px = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &ch, 4)) {
                done.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
                stbi_image_free(px);
                shrink(done.rgba, w, h, job.background ? 640 : 400);
                done.w = w;
                done.h = h;
            }
        }
        std::lock_guard<std::mutex> lk(m_mu);
        m_done.push_back(std::move(done));
    }
    if (net) stopNetwork();
}

void CardsActivity::wantArt() {
    std::vector<Job> jobs;
    auto add = [&](int i, bool background) {
        if (i < 0 || i >= static_cast<int>(m_games.size())) return;
        Art& a = m_art[m_games[(size_t)i].id];
        bool& asked = background ? a.bgAsked : a.cardAsked;
        if (asked) return;
        asked = true;
        jobs.push_back({m_games[(size_t)i], background});
    };
    add(m_sel, true);
    add(m_sel, false);
    for (int d = 1; d <= 6; ++d) {
        add(m_sel + d, false);
        add(m_sel - d, false);
        if (d == 1) {
            add(m_sel + 1, true);
            add(m_sel - 1, true);
        }
    }
    if (jobs.empty()) return;
    {
        std::lock_guard<std::mutex> lk(m_mu); // what's near the selection now goes first
        for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) m_jobs.push_front(std::move(*it));
    }
    m_cv.notify_one();
}

void CardsActivity::takeArt() {
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        const size_t n = std::min<size_t>(2, m_done.size()); // a couple of uploads a frame
        for (size_t i = 0; i < n; ++i) done.push_back(std::move(m_done[i]));
        m_done.erase(m_done.begin(), m_done.begin() + static_cast<std::ptrdiff_t>(n));
        if (m_playersFresh) {
            m_players = m_playersIn;
            m_playersFresh = false;
            m_playersTotal = 0;
            for (const auto& [id, n2] : m_players) m_playersTotal += n2;
        }
    }
    const std::uint64_t selected = m_games.empty() ? 0 : m_games[(size_t)m_sel].id;
    for (Done& d : done) {
        auto it = m_art.find(d.id);
        if (it == m_art.end() || d.rgba.empty()) continue;
        auto tex = std::make_unique<nxui::Texture>();
        if (!tex->loadFromPixels(app().gpu(), app().renderer(), d.rgba.data(), d.w, d.h)) {
            (d.background ? it->second.bgAsked : it->second.cardAsked) = false; // no room now: ask again later
            continue;
        }
        if (d.background) {
            it->second.bg = std::move(tex);
            if (d.id == selected && m_bgId != d.id) {
                m_bgPrevId = m_bgId;
                m_bgId = d.id;
                m_bgFade = 0.f;
            }
        } else {
            it->second.card = std::move(tex);
            it->second.tall = d.tall;
        }
    }

    // Far from the selection: let the pictures go (one a frame: freeing waits for the GPU).
    int freed = 0;
    for (size_t i = 0; i < m_games.size() && freed < 1; ++i) {
        auto it = m_art.find(m_games[i].id);
        if (it == m_art.end()) continue;
        const int dist = std::abs(static_cast<int>(i) - m_sel);
        Art& a = it->second;
        if (a.card && dist > 12) {
            a.card.reset();
            a.cardAsked = false;
            ++freed;
        } else if (a.bg && dist > 2 && it->first != m_bgId && it->first != m_bgPrevId) {
            a.bg.reset();
            a.bgAsked = false;
            ++freed;
        }
    }
}

// ---------------------------------------------------------------- actions

void CardsActivity::select(int index) {
    if (m_games.empty()) return;
    index = std::clamp(index, 0, static_cast<int>(m_games.size()) - 1);
    if (index == m_sel) return;
    m_sel = index;
    const std::uint64_t id = m_games[(size_t)m_sel].id;
    auto it = m_art.find(id);
    if (it != m_art.end() && it->second.bg && m_bgId != id) {
        m_bgPrevId = m_bgId;
        m_bgId = id;
        m_bgFade = 0.f;
    }
    wantArt();
}

void CardsActivity::sortGames(Sort sort) {
    const std::uint64_t keep = m_games.empty() ? 0 : m_games[(size_t)m_sel].id;
    m_sort = sort;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::stable_sort(m_games.begin(), m_games.end(), [&](const Game& a, const Game& b) {
        switch (sort) {
            case Sort::Title: return lower(a.name) < lower(b.name);
            case Sort::MostPlayed: return a.playSeconds != b.playSeconds ? a.playSeconds > b.playSeconds : a.order < b.order;
            case Sort::LastPlayed:
            default: return a.lastPlayed != b.lastPlayed ? a.lastPlayed > b.lastPlayed : a.order < b.order;
        }
    });
    m_sel = 0;
    for (size_t i = 0; i < m_games.size(); ++i)
        if (m_games[i].id == keep) m_sel = static_cast<int>(i);
    m_scroll = static_cast<float>(m_sel);
    for (auto& [id, art] : m_art) { // what's near has changed
        if (!art.card) art.cardAsked = false;
        if (!art.bg) art.bgAsked = false;
    }
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_jobs.clear();
    }
    wantArt();
}

void CardsActivity::play() {
    if (m_games.empty()) return;
    const Game& g = m_games[(size_t)m_sel];
    if (launch(g.id)) {
        app().requestExit(); // the system starts it once this has closed
        return;
    }
    say("Games start from here only when HomeNX Cards runs as an application (open it from HOME, "
        "or hold R while starting a game).");
}

void CardsActivity::openMenu(std::string title, std::vector<MenuItem> items) {
    m_menuTitle = std::move(title);
    m_menu = std::move(items);
    m_menuSel = 0;
}

void CardsActivity::openNotifications() {
    m_notifRows.clear();
    for (const auto& it : notifications::recent(6)) {
        std::string title = it.title;
        for (size_t i = 0; i < title.size(); ++i) // Discord's emoji marks aren't letters here
            if (static_cast<unsigned char>(title[i]) >= 0xF3) title.erase(i--, 4);
        m_notifRows.push_back({(it.kind.rfind("discord", 0) == 0 ? "Discord - " : it.kind == "battery" ? "" : "") + title,
                               it.text});
    }
    notifications::markSeen();
    m_unread = 0;
    m_notifOpen = true;
}

void CardsActivity::say(const std::string& message) {
    m_message = message;
    m_messageFor = 4.f;
}

void CardsActivity::refreshStatus() {
    TimeCalendarTime t{};
    TimeCalendarAdditionalInfo info{};
    u64 now = 0;
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_Default, &now)) && R_SUCCEEDED(timeToCalendarTimeWithMyRule(now, &t, &info))) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.minute);
        m_clock = buf;
    }
    u32 charge = 0;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&charge))) m_battery = std::to_string(charge) + "%";
    NifmInternetConnectionType type{};
    u32 strength = 0;
    NifmInternetConnectionStatus status{};
    m_online = R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status))
            && status == NifmInternetConnectionStatus_Connected;
    if (FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc")) {
        s64 free = 0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(fs, "/", &free))) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f GB free", static_cast<double>(free) / 1e9);
            m_free = buf;
        }
    }
}

void CardsActivity::activateTop(int index) {
    switch (index) {
        case 0:
        case 4:
            openMenu("HomeNX Cards", {
                {"Sort by last played", [this]() { sortGames(Sort::LastPlayed); }},
                {"Sort by title", [this]() { sortGames(Sort::Title); }},
                {"Sort by most played", [this]() { sortGames(Sort::MostPlayed); }},
                {m_key.empty() ? "Box art: add a SteamGridDB key in HomeNX" : "Box art: SteamGridDB", [] {}},
                {"Close HomeNX Cards", [this]() { app().requestExit(); }},
            });
            break;
        case 1:
            sortGames(m_sort == Sort::LastPlayed ? Sort::Title : m_sort == Sort::Title ? Sort::MostPlayed : Sort::LastPlayed);
            break;
        case 2:
            say(m_playersTotal < 0 ? "Counting the players online in Nextendo..."
                                   : std::to_string(m_playersTotal) + " players online in Nextendo right now.");
            break;
        case 3: openNotifications(); break;
        default: break;
    }
}

void CardsActivity::activateBottom(int index) {
    if (m_games.empty() && index < 2) return;
    switch (index) {
        case 0: play(); break;
        case 1: {
            const Game g = m_games[(size_t)m_sel];
            openMenu(g.name, {
                {"Play", [this]() { play(); }},
                {"Download new pictures", [this, id = g.id]() {
                     forgetArt(id);
                     auto it = m_art.find(id);
                     if (it != m_art.end()) {
                         it->second.cardAsked = it->second.bgAsked = false;
                         app().gpu().waitIdle(); // they may still be drawing
                         it->second.card.reset();
                         it->second.bg.reset();
                     }
                     wantArt();
                 }},
                {"Cancel", [] {}},
            });
            break;
        }
        case 2: { // the Homebrew Manager, if HOME has put it there
            static constexpr const char* kManager = "sdmc:/switch/Homebrew-Manager/Homebrew-Manager.nro";
            std::error_code ec;
            if (!std::filesystem::exists(kManager, ec) || !envHasNextLoad()) {
                say("The Homebrew Manager isn't on the SD card yet (HomeNX puts it there).");
                break;
            }
            envSetNextLoad(kManager, "\"sdmc:/switch/Homebrew-Manager/Homebrew-Manager.nro\"");
            app().requestExit();
            break;
        }
        case 3: app().requestExit(); break;
        default: break;
    }
}

// ---------------------------------------------------------------- frame

void CardsActivity::onUpdate(float dt) {
    takeArt();
    m_scroll += (static_cast<float>(m_sel) - m_scroll) * std::min(1.f, dt * 14.f);
    if (std::abs(m_scroll - m_sel) < 0.001f) m_scroll = static_cast<float>(m_sel);
    m_bgFade = std::min(1.f, m_bgFade + dt / 0.35f);
    if (m_messageFor > 0.f) m_messageFor -= dt;
    if ((m_statusIn -= dt) <= 0.f) {
        m_statusIn = 1.f;
        refreshStatus();
    }

    auto& in = app().input();
    using B = nxui::Button;
    const bool aDown = in.isDown(B::A), bDown = in.isDown(B::B);
    const bool upDown = in.isDown(B::DUp) || in.isDown(B::LStickU);
    const bool downDown = in.isDown(B::DDown) || in.isDown(B::LStickD);

    if (m_notifOpen) {
        if (bDown || aDown || in.isDown(B::Minus)) m_notifOpen = false;
        if (in.isDown(B::X)) {
            notifications::clear();
            m_notifRows.clear();
        }
        return;
    }
    if (!m_menu.empty()) {
        if (upDown) m_menuSel = std::max(0, m_menuSel - 1);
        if (downDown) m_menuSel = std::min(static_cast<int>(m_menu.size()) - 1, m_menuSel + 1);
        if (bDown) m_menu.clear();
        if (aDown) {
            auto run = m_menu[(size_t)m_menuSel].run;
            m_menu.clear();
            if (run) run();
        }
        return;
    }

    // Left / right: repeating while held.
    const bool left = in.isHeld(B::DLeft) || in.isHeld(B::LStickL);
    const bool right = in.isHeld(B::DRight) || in.isHeld(B::LStickR);
    int step = 0;
    if (in.isDown(B::DLeft) || in.isDown(B::LStickL)) {
        step = -1;
        m_repeat = 0.32f;
    } else if (in.isDown(B::DRight) || in.isDown(B::LStickR)) {
        step = 1;
        m_repeat = 0.32f;
    } else if (left || right) {
        if ((m_repeat -= dt) <= 0.f) {
            step = left ? -1 : 1;
            m_repeat = 0.07f;
        }
    }
    if (step) {
        if (m_zone == Zone::Cards) select(m_sel + step);
        else if (m_zone == Zone::Top) m_topSel = std::clamp(m_topSel + step, 0, 4);
        else m_bottomSel = std::clamp(m_bottomSel + step, 0, 3);
    }
    if (upDown) m_zone = m_zone == Zone::Bottom ? Zone::Cards : Zone::Top;
    if (downDown) m_zone = m_zone == Zone::Top ? Zone::Cards : Zone::Bottom;
    if (aDown) {
        if (m_zone == Zone::Cards) play();
        else if (m_zone == Zone::Top) activateTop(m_topSel);
        else activateBottom(m_bottomSel);
    }
    if (bDown && m_zone != Zone::Cards) m_zone = Zone::Cards;
    if (in.isDown(B::X)) activateBottom(1);
    if (in.isDown(B::Y)) activateTop(1);
    if (in.isDown(B::Minus)) openNotifications();
    if (in.isDown(B::L) || in.isDown(B::ZL)) select(m_sel - 5);
    if (in.isDown(B::R) || in.isDown(B::ZR)) select(m_sel + 5);
    if (in.isDown(B::Plus)) app().requestExit();
}

void CardsActivity::onRender(nxui::Renderer&) {}

void CardsActivity::draw(nxui::Renderer& r) {
    drawBackground(r);
    drawCards(r);
    drawTopBar(r);
    drawPanel(r);
    if (m_messageFor > 0.f && !m_message.empty()) {
        const float a = std::min(1.f, m_messageFor * 3.f);
        const std::string s = fit(m_fBody, m_message, 820.f);
        const float w = m_fBody.measure(s).x + 44.f;
        const nxui::Rect pill{640.f - w * 0.5f, 452.f, w, 40.f};
        r.drawRoundedRect(pill, nxui::Color(0.08f, 0.09f, 0.12f, 0.88f * a), 20.f);
        textMid(r, m_fBody, s, pill.x + 22.f, pill.y + 20.f, kWhite.withAlpha(a));
    }
    drawPopup(r);
}

// ---------------------------------------------------------------- drawing

void CardsActivity::icon(nxui::Renderer& r, const char* name, float cx, float cy, float size, const nxui::Color& tint) {
    auto it = m_icons.find(name);
    if (it == m_icons.end() || !it->second->valid()) return;
    r.drawTexture(it->second.get(), {cx - size * 0.5f, cy - size * 0.5f, size, size}, tint);
}

void CardsActivity::glassCircle(nxui::Renderer& r, float cx, float cy, float d, bool focused, const char* name,
                                float iconSize) {
    const nxui::Vec2 c{cx, cy};
    r.drawCircle(c, d * 0.5f, focused ? nxui::Color(0.55f, 0.59f, 0.68f, 0.7f) : kGlass, 48);
    const nxui::Rect box{cx - d * 0.5f, cy - d * 0.5f, d, d};
    r.drawRoundedRectOutline(box, focused ? kWhite : kGlassEdge, d * 0.5f, focused ? 3.f : 1.5f);
    icon(r, name, cx, cy, iconSize, kWhite);
}

void CardsActivity::drawBackground(nxui::Renderer& r) {
    r.drawGradientRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.36f, 0.41f, 0.55f, 1.f),
                       nxui::Color(0.22f, 0.25f, 0.34f, 1.f));
    auto bgOf = [this](std::uint64_t id) -> const nxui::Texture* {
        auto it = m_art.find(id);
        return it != m_art.end() && it->second.bg && it->second.bg->valid() ? it->second.bg.get() : nullptr;
    };
    const nxui::Rect screen{0.f, 0.f, 1280.f, 720.f};
    if (m_bgFade < 1.f)
        if (const nxui::Texture* prev = bgOf(m_bgPrevId)) drawCover(r, prev, screen, 0.f, kWhite);
    if (const nxui::Texture* cur = bgOf(m_bgId)) drawCover(r, cur, screen, 0.f, kWhite.withAlpha(m_bgFade));
    // The haze over it: the art shows, but softly, like the reference.
    r.drawRect(screen, nxui::Color(0.60f, 0.65f, 0.78f, 0.40f));
}

void CardsActivity::drawTopBar(nxui::Renderer& r) {
    const bool top = m_zone == Zone::Top;
    glassCircle(r, 134.f, kTopY, kCircle, top && m_topSel == 0, "dots", 34.f);
    glassCircle(r, 219.f, kTopY, kCircle, top && m_topSel == 1, "sort", 34.f);
    static const char* const kSortNames[] = {"Last Played", "Title", "Most Played"};
    textMid(r, m_fLabel, kSortNames[static_cast<int>(m_sort)], 268.f, kTopY, kWhite);

    // The pill: you, online, the time, battery, free space.
    const std::string word = m_online ? "Online" : "Offline";
    const float avatar = 32.f, gap = 16.f;
    const float wWord = m_fPill.measure(word).x, wClock = m_fPill.measure(m_clock).x;
    const float wBattery = m_fPill.measure(m_battery).x, wFree = m_fPill.measure(m_free).x;
    const float width = 12.f + avatar + 12.f + wWord + gap + wClock + gap + wBattery + gap + 18.f + 8.f + wFree + 18.f;
    const nxui::Rect pill{640.f - width * 0.5f, 21.f, width, 52.f};
    r.drawRoundedRect(pill, kGlass, 26.f);
    r.drawRoundedRectOutline(pill, kGlassEdge, 26.f, 1.5f);
    float x = pill.x + 12.f;
    const nxui::Rect face{x, kTopY - avatar * 0.5f, avatar, avatar};
    if (m_avatar && m_avatar->valid()) r.drawTextureRounded(m_avatar.get(), face, avatar * 0.5f, kWhite);
    else r.drawCircle({face.x + avatar * 0.5f, kTopY}, avatar * 0.5f, nxui::Color(1.f, 1.f, 1.f, 0.3f), 32);
    x += avatar + 12.f;
    textMid(r, m_fPill, word, x, kTopY, m_online ? kGreen : nxui::Color(0.75f, 0.77f, 0.8f, 1.f));
    x += wWord + gap;
    textMid(r, m_fPill, m_clock, x, kTopY, kWhite);
    x += wClock + gap;
    textMid(r, m_fPill, m_battery, x, kTopY, kWhite);
    x += wBattery + gap;
    icon(r, "thermo", x + 9.f, kTopY, 24.f, nxui::Color(0.39f, 0.82f, 1.f, 1.f));
    x += 18.f + 8.f;
    textMid(r, m_fPill, m_free, x, kTopY, kWhite);

    // Nextendo's players, notifications, settings.
    const nxui::Color focusRing = kWhite;
    icon(r, "friends", 980.f, kTopY, 34.f, kWhite);
    textMid(r, m_fLabel, m_playersTotal < 0 ? "?" : std::to_string(m_playersTotal), 1001.f, kTopY, kWhite);
    if (top && m_topSel == 2)
        r.drawRoundedRectOutline({956.f, kTopY - 26.f, 80.f, 52.f}, focusRing, 26.f, 3.f);
    icon(r, "bell", 1074.f, kTopY, 36.f, kWhite);
    if (top && m_topSel == 3) r.drawRoundedRectOutline({1048.f, kTopY - 26.f, 52.f, 52.f}, focusRing, 26.f, 3.f);
    if (m_unread > 0) {
        const std::string n = m_unread > 99 ? "99+" : std::to_string(m_unread);
        const float w = std::max(24.f, m_fBadge.measure(n).x + 14.f);
        const nxui::Rect badge{1080.f, 10.f, w, 24.f};
        r.drawRoundedRect(badge, kRed, 12.f);
        textMid(r, m_fBadge, n, badge.x + (w - m_fBadge.measure(n).x) * 0.5f, badge.y + 12.f, kWhite);
    }
    glassCircle(r, 1146.f, kTopY, kCircle, top && m_topSel == 4, "gear", 36.f);
}

void CardsActivity::drawCards(nxui::Renderer& r) {
    if (m_games.empty()) {
        textMid(r, m_fLabel, "No games installed", 520.f, kCardMidY, kWhite);
        return;
    }
    const bool cardsFocused = m_zone == Zone::Cards;
    for (int i = 0; i < static_cast<int>(m_games.size()); ++i) {
        const float rel = static_cast<float>(i) - m_scroll;
        const float x0 = kCardFirstX + rel * kCardPitch;
        if (x0 > 1290.f || x0 + kCardPitch < -10.f) continue;
        const float t = std::clamp(1.f - std::abs(rel), 0.f, 1.f); // how chosen it is
        const float w = kCardW + kCardGrowW * t, h = kCardH + kCardGrowH * t;
        const nxui::Rect card{x0 + kCardW * 0.5f - w * 0.5f, kCardMidY - h * 0.5f, w, h};
        const Game& g = m_games[(size_t)i];

        r.drawRoundedRect({card.x - 2.f, card.y + 6.f, card.width + 4.f, card.height + 4.f},
                          nxui::Color(0.f, 0.f, 0.f, 0.22f), kCardRadius + 2.f); // shadow
        auto it = m_art.find(g.id);
        const nxui::Texture* tex = it != m_art.end() && it->second.card && it->second.card->valid() ? it->second.card.get()
                                                                                                    : nullptr;
        if (tex) {
            drawCover(r, tex, card, kCardRadius, kWhite);
        } else { // still coming: the name on glass
            r.drawRoundedRect(card, nxui::Color(0.28f, 0.31f, 0.40f, 0.85f), kCardRadius);
            const std::string name = fit(m_fLabel, g.name, card.width - 28.f, 0.9f);
            textMid(r, m_fLabel, name, card.x + (card.width - m_fLabel.measure(name).x * 0.9f) * 0.5f,
                    card.y + card.height * 0.5f, kMuted, 0.9f);
        }
        if (i < m_sel) // the ones already passed sit back a little
            r.drawRoundedRect(card, nxui::Color(0.f, 0.f, 0.f, 0.32f * (1.f - t)), kCardRadius);
        if (t > 0.01f) {
            r.drawRoundedRectOutline(card, kWhite.withAlpha((cardsFocused ? 1.f : 0.55f) * t), kCardRadius, 3.5f);
        }

        // Nextendo: players online in it now.
        auto p = m_players.find(g.id);
        if (p != m_players.end() && p->second > 0) {
            const std::string n = std::to_string(p->second);
            const float bw = 30.f + m_fBadge.measure(n).x + 10.f;
            const nxui::Rect badge{card.x + 12.f, card.y + 12.f, bw, 28.f};
            r.drawRoundedRect(badge, nxui::Color(0.08f, 0.10f, 0.13f, 0.72f), 14.f);
            r.drawCircle({badge.x + 15.f, badge.y + 14.f}, 6.f, kGreen, 20);
            textMid(r, m_fBadge, n, badge.x + 28.f, badge.y + 14.f, kWhite);
        }
    }
}

const nxui::Texture* CardsActivity::gameIconTexture() {
    if (m_games.empty()) return nullptr;
    const std::uint64_t id = m_games[(size_t)m_sel].id;
    if (id != m_gameIconId) {
        m_gameIconId = id;
        const std::vector<std::uint8_t> jpeg = gameIcon(id);
        if (m_gameIcon) app().gpu().waitIdle(); // the last one may still be drawing
        m_gameIcon = std::make_unique<nxui::Texture>();
        if (jpeg.empty() || !m_gameIcon->loadFromMemory(app().gpu(), app().renderer(), jpeg.data(), jpeg.size(), 160))
            m_gameIcon.reset();
    }
    return m_gameIcon.get();
}

void CardsActivity::drawPanel(nxui::Renderer& r) {
    r.drawRoundedRect(kPanel, nxui::Color(0.40f, 0.45f, 0.56f, 0.50f), kPanelRadius);
    r.drawRoundedRectOutline(kPanel, nxui::Color(1.f, 1.f, 1.f, 0.25f), kPanelRadius, 1.5f);
    if (m_games.empty()) return;
    const Game& g = m_games[(size_t)m_sel];

    const nxui::Rect iconRect{kPanel.x + 25.f, kPanel.y + 20.f, 78.f, 78.f};
    if (const nxui::Texture* ic = gameIconTexture()) r.drawTextureRounded(ic, iconRect, 18.f, kWhite);
    const float tx = iconRect.x + iconRect.width + 15.f;
    const float maxW = kPanel.x + kPanel.width - 30.f - tx;
    r.drawText(fit(m_fTitle, g.name, maxW), {tx, kPanel.y + 20.f}, &m_fTitle, kWhite);
    r.drawText(fit(m_fBody, playTime(g.playSeconds) + "    " + lastPlayed(g.lastPlayed), maxW), {tx, kPanel.y + 60.f},
               &m_fBody, kMuted);
    if (!g.version.empty()) {
        icon(r, "tag", tx + 8.f, kPanel.y + 95.f, 17.f, kMuted);
        const std::string v = (g.version[0] == 'v' || g.version[0] == 'V') ? g.version : "v" + g.version;
        textMid(r, m_fBody, v, tx + 22.f, kPanel.y + 95.f, kMuted, 0.95f);
    }

    // Play, its options, the Homebrew Manager, back.
    const bool bottom = m_zone == Zone::Bottom;
    const float by = kPanel.y + 108.f;
    const nxui::Rect playRect{kPanel.x + 22.f, by, 262.f, 77.f};
    r.drawRoundedRect(playRect, kWhite, 38.5f);
    if (bottom && m_bottomSel == 0)
        r.drawRoundedRectOutline({playRect.x - 4.f, playRect.y - 4.f, playRect.width + 8.f, playRect.height + 8.f},
                                 kWhite, 42.5f, 3.f);
    const nxui::Color ink(0.f, 0.f, 0.f, 1.f);
    const float playW = 24.f + 10.f + m_fPlay.measure("Play").x;
    const float px = playRect.x + (playRect.width - playW) * 0.5f;
    icon(r, "play", px + 12.f, playRect.y + playRect.height * 0.5f, 24.f, ink);
    textMid(r, m_fPlay, "Play", px + 34.f, playRect.y + playRect.height * 0.5f, ink);
    glassCircle(r, playRect.x + playRect.width + 8.f + 38.5f, by + 38.5f, 77.f, bottom && m_bottomSel == 1, "dots", 32.f);
    const float right = kPanel.x + kPanel.width - 22.f;
    glassCircle(r, right - 77.f - 8.f - 38.5f, by + 38.5f, 77.f, bottom && m_bottomSel == 2, "copy", 32.f);
    glassCircle(r, right - 38.5f, by + 38.5f, 77.f, bottom && m_bottomSel == 3, "home", 34.f);
}

void CardsActivity::drawPopup(nxui::Renderer& r) {
    if (m_notifOpen) {
        const float rows = static_cast<float>(std::max<size_t>(1, m_notifRows.size()));
        const nxui::Rect box{1184.f - 440.f, 84.f, 440.f, 60.f + rows * 64.f + 40.f};
        r.drawRoundedRect(box, nxui::Color(0.12f, 0.13f, 0.17f, 0.96f), 24.f);
        r.drawRoundedRectOutline(box, nxui::Color(1.f, 1.f, 1.f, 0.12f), 24.f, 1.5f);
        r.drawText("Notifications", {box.x + 22.f, box.y + 16.f}, &m_fLabel, kWhite);
        if (m_notifRows.empty())
            r.drawText("You're all caught up", {box.x + 22.f, box.y + 70.f}, &m_fBody, kMuted);
        for (size_t i = 0; i < m_notifRows.size(); ++i) {
            const float y = box.y + 60.f + static_cast<float>(i) * 64.f;
            r.drawText(fit(m_fPill, m_notifRows[i].first, box.width - 44.f), {box.x + 22.f, y + 6.f}, &m_fPill, kWhite);
            r.drawText(fit(m_fBody, m_notifRows[i].second, box.width - 44.f), {box.x + 22.f, y + 32.f}, &m_fBody, kMuted);
        }
        r.drawText(m_notifRows.empty() ? "B  Close" : "X  Clear all      B  Close", {box.x + 22.f, box.y + box.height - 32.f},
                   &m_fBody, kMuted);
        return;
    }
    if (m_menu.empty()) return;
    r.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.4f));
    const float w = 500.f, rowH = 56.f, h = 80.f + rowH * static_cast<float>(m_menu.size());
    const nxui::Rect box{(1280.f - w) * 0.5f, (720.f - h) * 0.5f, w, h};
    r.drawRoundedRect(box, nxui::Color(0.12f, 0.13f, 0.17f, 0.97f), 28.f);
    r.drawRoundedRectOutline(box, nxui::Color(1.f, 1.f, 1.f, 0.12f), 28.f, 1.5f);
    r.drawText(fit(m_fLabel, m_menuTitle, w - 48.f), {box.x + 24.f, box.y + 20.f}, &m_fLabel, kWhite);
    for (size_t i = 0; i < m_menu.size(); ++i) {
        const nxui::Rect row{box.x + 14.f, box.y + 64.f + rowH * static_cast<float>(i), w - 28.f, rowH - 6.f};
        if (static_cast<int>(i) == m_menuSel) {
            r.drawRoundedRect(row, nxui::Color(1.f, 1.f, 1.f, 0.14f), 16.f);
            r.drawRoundedRectOutline(row, kWhite, 16.f, 2.f);
        }
        textMid(r, m_fPill, fit(m_fPill, m_menu[i].label, row.width - 32.f), row.x + 16.f, row.y + row.height * 0.5f, kWhite);
    }
}

} // namespace cards
