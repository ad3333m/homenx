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
// Cards are the shape of a Switch case cover without its red band (352x499),
// so its whole picture fits them exactly.
constexpr float kCardW = 240.f, kCardH = 340.f, kCardGrowW = 10.f, kCardGrowH = 14.f;
constexpr float kCardMidY = 291.f, kCardPitch = 250.f, kCardFirstX = 136.f, kCardRadius = 26.f;
const nxui::Rect kPanel{186.f, 503.f, 907.f, 207.f};
constexpr float kPanelRadius = 35.f;

const nxui::Color kWhite(1.f, 1.f, 1.f, 1.f);
const nxui::Color kMuted(1.f, 1.f, 1.f, 0.82f);
const nxui::Color kGlass(0.17f, 0.20f, 0.27f, 0.30f);     // over the blurred frame
const nxui::Color kGlassEdge(1.f, 1.f, 1.f, 0.20f);
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

// A picture drawn to fill `dst`, cropped to its shape (centred). trimTop /
// trimBottom: fractions of it left out first.
void drawCover(nxui::Renderer& r, const nxui::Texture* tex, const nxui::Rect& dst, float radius, const nxui::Color& tint,
               float trimTop = 0.f, float trimBottom = 0.f) {
    const float tw = static_cast<float>(tex->width());
    const float top = static_cast<float>(tex->height()) * trimTop;
    const float th = static_cast<float>(tex->height()) * (1.f - trimTop - trimBottom);
    const float scale = std::max(dst.width / tw, dst.height / th);
    const float sw = dst.width / scale, sh = dst.height / scale;
    const nxui::Rect src{(tw - sw) * 0.5f, top + (th - sh) * 0.5f, sw, sh};
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
        || !m_fPill.load(gpu, ren, f6, 19) || !m_fBody.load(gpu, ren, f5, 17) || !m_fBadge.load(gpu, ren, f7, 15)
        || !m_fCount.load(gpu, ren, f7, 21))
        return false;
    for (const char* name : {"dots", "sort", "friends", "bell", "gear", "thermo", "tag", "play", "copy", "home"}) {
        auto tex = std::make_unique<nxui::Texture>();
        if (tex->loadFromFile(gpu, ren, std::string("romfs:/icons/") + name + ".png", 96)) m_icons[name] = std::move(tex);
    }

    // Games, and Homebrew: NRO forwarders (installed, ID 05...) and the .nro apps.
    for (Game& g : listGames()) (g.homebrew ? m_otherPage : m_games).push_back(std::move(g));
    std::vector<Game> nros = listHomebrew();
    m_otherPage.insert(m_otherPage.begin(), std::make_move_iterator(nros.begin()), std::make_move_iterator(nros.end()));
    for (size_t i = 0; i < m_otherPage.size(); ++i) m_otherPage[i].order = static_cast<int>(i);
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
            job.background ? backgroundArt(job.game, m_key) : cardArt(job.game, m_key, done.kind);
        if (!data.empty()) {
            int w = 0, h = 0, ch = 0;
            if (stbi_uc* px = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &ch, 4)) {
                done.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
                stbi_image_free(px);
                // An app's icon as a background: tiny, so drawn full screen it's a soft wash of its colours.
                shrink(done.rgba, w, h, job.background ? (job.game.homebrew ? 32 : 640) : 400);
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
            it->second.kind = d.kind;
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

void CardsActivity::switchPage() {
    std::swap(m_games, m_otherPage);
    std::swap(m_sel, m_otherSel);
    m_homebrewPage = !m_homebrewPage;
    m_scroll = static_cast<float>(m_sel);
    m_zone = Zone::Cards;
    m_bgPrevId = m_bgId; // the new page's art fades in over the old
    m_bgFade = 0.f;
    m_bgId = m_games.empty() ? 0 : m_games[(size_t)m_sel].id;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_jobs.clear(); // what's near has changed
    }
    for (auto& [id, art] : m_art) {
        if (!art.card) art.cardAsked = false;
        if (!art.bg) art.bgAsked = false;
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
    if (!g.path.empty()) { // a .nro: it replaces this app, as hbmenu starts it
        if (!envHasNextLoad()) {
            say("Apps can't be opened from here: start CardsNX from HOME or the homebrew menu.");
            return;
        }
        envSetNextLoad(g.path.c_str(), ("\"" + g.path + "\"").c_str());
        app().requestExit();
        return;
    }
    if (launch(g.id)) {
        app().requestExit(); // the system starts it once this has closed
        return;
    }
    say("Games start from here only when CardsNX runs as an application (open it from HOME, "
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
            openMenu("CardsNX", {
                {"Sort by last played", [this]() { sortGames(Sort::LastPlayed); }},
                {"Sort by title", [this]() { sortGames(Sort::Title); }},
                {"Sort by most played", [this]() { sortGames(Sort::MostPlayed); }},
                {m_key.empty() ? "Box art: add a SteamGridDB key in HomeNX" : "Box art: SteamGridDB", [] {}},
                {"Close CardsNX", [this]() { app().requestExit(); }},
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
        case 2: switchPage(); break;
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
    if (in.isDown(B::L)) select(m_sel - 5);
    if (in.isDown(B::R)) select(m_sel + 5);
    if (in.isDown(B::ZL) || in.isDown(B::ZR)) switchPage();
    if (in.isDown(B::Plus)) app().requestExit();
}

void CardsActivity::onRender(nxui::Renderer&) {}

void CardsActivity::draw(nxui::Renderer& r) {
    drawBackground(r);
    drawCards(r);
    // One blurred copy of the frame for every glass shape on top of it.
    m_frosted = r.gpu().offscreenReady();
    if (m_frosted) {
        r.captureToOffscreen(false);
        r.applyBlur(2.5f, 3);
    }
    drawCardBadges(r);
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

void CardsActivity::frost(nxui::Renderer& r, const nxui::Rect& rect, float radius, const nxui::Color& tint) {
    if (m_frosted) r.drawOffscreenRounded(0, rect, radius, kWhite);
    r.drawRoundedRect(rect, tint, radius);
}

void CardsActivity::glassCircle(nxui::Renderer& r, float cx, float cy, float d, bool focused, const char* name,
                                float iconSize) {
    const nxui::Rect box{cx - d * 0.5f, cy - d * 0.5f, d, d};
    frost(r, box, d * 0.5f, focused ? nxui::Color(1.f, 1.f, 1.f, 0.22f) : kGlass);
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
    r.drawRect(screen, nxui::Color(0.38f, 0.43f, 0.56f, 0.45f));
}

void CardsActivity::drawTopBar(nxui::Renderer& r) {
    const bool top = m_zone == Zone::Top;
    glassCircle(r, 134.f, kTopY, kCircle, top && m_topSel == 0, "dots", 34.f);
    glassCircle(r, 219.f, kTopY, kCircle, top && m_topSel == 1, "sort", 34.f);
    static const char* const kSortNames[] = {"Last Played", "Title", "Most Played"};
    textMid(r, m_fLabel, m_homebrewPage ? std::string("Homebrew") : std::string(kSortNames[static_cast<int>(m_sort)]),
            268.f, kTopY, kWhite);

    // The pill: you, online, the time, battery, free space.
    const std::string word = m_online ? "Online" : "Offline";
    const float avatar = 32.f, gap = 16.f;
    const float wWord = m_fPill.measure(word).x, wClock = m_fPill.measure(m_clock).x;
    const float wBattery = m_fPill.measure(m_battery).x, wFree = m_fPill.measure(m_free).x;
    const float width = 12.f + avatar + 12.f + wWord + gap + wClock + gap + wBattery + gap + 18.f + 8.f + wFree + 18.f;
    const nxui::Rect pill{640.f - width * 0.5f, 21.f, width, 52.f};
    frost(r, pill, 26.f, kGlass);
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
        textMid(r, m_fLabel, m_homebrewPage ? "No homebrew in sdmc:/switch" : "No games installed", 480.f, kCardMidY,
                kWhite);
        return;
    }
    const bool cardsFocused = m_zone == Zone::Cards;
    for (int i = 0; i < static_cast<int>(m_games.size()); ++i) {
        nxui::Rect card;
        float t = 0.f;
        if (!cardRect(i, card, t)) continue;
        const Game& g = m_games[(size_t)i];

        r.drawRoundedRect({card.x - 2.f, card.y + 6.f, card.width + 4.f, card.height + 4.f},
                          nxui::Color(0.f, 0.f, 0.f, 0.22f), kCardRadius + 2.f); // shadow
        auto it = m_art.find(g.id);
        const nxui::Texture* tex = it != m_art.end() && it->second.card && it->second.card->valid() ? it->second.card.get()
                                                                                                    : nullptr;
        if (tex && it->second.kind == ArtKind::Icon) {
            // An app: its icon over a dimmed, enlarged copy of itself, its name below.
            drawCover(r, tex, card, kCardRadius, nxui::Color(0.55f, 0.55f, 0.55f, 1.f));
            r.drawRoundedRect(card, nxui::Color(0.05f, 0.06f, 0.08f, 0.35f), kCardRadius);
            const float side = card.width - 48.f;
            r.drawTextureRounded(tex, {card.x + 24.f, card.y + 30.f, side, side}, 26.f, kWhite);
            const std::string name = fit(m_fLabel, g.name, card.width - 28.f);
            r.drawText(name, {card.x + (card.width - m_fLabel.measure(name).x) * 0.5f, card.y + 46.f + side}, &m_fLabel,
                       kWhite);
            if (!g.author.empty()) {
                const std::string by = fit(m_fBody, g.author, card.width - 28.f);
                r.drawText(by, {card.x + (card.width - m_fBody.measure(by).x) * 0.5f, card.y + 78.f + side}, &m_fBody,
                           kMuted);
            }
        } else if (tex) {
            // A Switch case cover without the red Nintendo Switch band on top:
            // the rest is the card's own shape, so all of it shows.
            const bool box = it->second.kind == ArtKind::Box;
            drawCover(r, tex, card, kCardRadius, kWhite, box ? 0.125f : 0.f, 0.f);
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
    }
}

bool CardsActivity::cardRect(int index, nxui::Rect& card, float& chosen) const {
    const float rel = static_cast<float>(index) - m_scroll;
    const float x0 = kCardFirstX + rel * kCardPitch;
    if (x0 > 1290.f || x0 + kCardPitch < -10.f) return false;
    chosen = std::clamp(1.f - std::abs(rel), 0.f, 1.f);
    const float w = kCardW + kCardGrowW * chosen, h = kCardH + kCardGrowH * chosen;
    card = {x0 + kCardW * 0.5f - w * 0.5f, kCardMidY - h * 0.5f, w, h};
    return true;
}

// Nextendo: players online in each game now, a glass pill with a green dot.
void CardsActivity::drawCardBadges(nxui::Renderer& r) {
    for (int i = 0; i < static_cast<int>(m_games.size()); ++i) {
        auto p = m_players.find(m_games[(size_t)i].id);
        if (p == m_players.end() || p->second <= 0) continue;
        nxui::Rect card;
        float t = 0.f;
        if (!cardRect(i, card, t)) continue;
        const std::string n = std::to_string(p->second);
        const nxui::Rect badge{card.x + 14.f, card.y + 14.f, 34.f + m_fCount.measure(n).x + 14.f, 34.f};
        frost(r, badge, 17.f, nxui::Color(0.05f, 0.07f, 0.09f, 0.42f));
        r.drawRoundedRectOutline(badge, nxui::Color(1.f, 1.f, 1.f, 0.16f), 17.f, 1.5f);
        r.drawCircle({badge.x + 18.f, badge.y + 17.f}, 8.f, kGreen, 24);
        textMid(r, m_fCount, n, badge.x + 33.f, badge.y + 17.f, kWhite);
        if (i < m_sel) // like its card, a little back
            r.drawRoundedRect(badge, nxui::Color(0.f, 0.f, 0.f, 0.32f * (1.f - t)), 17.f);
    }
}

const nxui::Texture* CardsActivity::gameIconTexture() {
    if (m_games.empty()) return nullptr;
    const std::uint64_t id = m_games[(size_t)m_sel].id;
    if (id != m_gameIconId) {
        m_gameIconId = id;
        const std::vector<std::uint8_t> jpeg = gameIcon(m_games[(size_t)m_sel]);
        if (m_gameIcon) app().gpu().waitIdle(); // the last one may still be drawing
        m_gameIcon = std::make_unique<nxui::Texture>();
        if (jpeg.empty() || !m_gameIcon->loadFromMemory(app().gpu(), app().renderer(), jpeg.data(), jpeg.size(), 160))
            m_gameIcon.reset();
    }
    return m_gameIcon.get();
}

void CardsActivity::drawPanel(nxui::Renderer& r) {
    frost(r, kPanel, kPanelRadius, nxui::Color(0.16f, 0.19f, 0.26f, 0.34f));
    r.drawRoundedRectOutline(kPanel, nxui::Color(1.f, 1.f, 1.f, 0.22f), kPanelRadius, 1.5f);
    if (m_games.empty()) return;
    const Game& g = m_games[(size_t)m_sel];

    const nxui::Rect iconRect{kPanel.x + 25.f, kPanel.y + 20.f, 78.f, 78.f};
    if (const nxui::Texture* ic = gameIconTexture()) r.drawTextureRounded(ic, iconRect, 18.f, kWhite);
    const float tx = iconRect.x + iconRect.width + 15.f;
    const float maxW = kPanel.x + kPanel.width - 30.f - tx;
    r.drawText(fit(m_fTitle, g.name, maxW), {tx, kPanel.y + 20.f}, &m_fTitle, kWhite);
    const std::string about = !g.path.empty() ? (g.author.empty() ? std::string("Homebrew") : "Homebrew by " + g.author)
                              : playTime(g.playSeconds) + "    " + lastPlayed(g.lastPlayed);
    r.drawText(fit(m_fBody, about, maxW), {tx, kPanel.y + 60.f}, &m_fBody, kMuted);
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
    const char* playWord = g.path.empty() ? "Play" : "Open";
    const float playW = 24.f + 10.f + m_fPlay.measure(playWord).x;
    const float px = playRect.x + (playRect.width - playW) * 0.5f;
    icon(r, "play", px + 12.f, playRect.y + playRect.height * 0.5f, 24.f, ink);
    textMid(r, m_fPlay, playWord, px + 34.f, playRect.y + playRect.height * 0.5f, ink);
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
