#include "widgets/SpotifyPanel.hpp"

#include "net/async.h"
#include "net/http.h"
#include "spotify/SwitchVolume.hpp"

#include <nxui/core/Animation.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Input.hpp>
#include <nxui/core/Renderer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr float kPanelX = 16.f;
constexpr float kPanelY = 16.f;
constexpr float kPanelH = 688.f;
constexpr float kAnimDur = 0.25f;
// The web player's layout at 1280x720: a top bar, Your Library on the left,
// the main area, and the player bar.
constexpr float kTopH = 64.f;     // the top bar
constexpr float kBarH = 92.f;     // the player bar
constexpr int kSeekStepMs = 10000;
constexpr int kVolumeStep = 50;   // permille

// Your Library: its title, its chips, then its rows.
constexpr float kSideListTop = 100.f; // from the column's top
constexpr float kSideRowH = 64.f;
// Home: a grid of tiles (4 across), then shelves of cards; Search: shelves.
constexpr float kPad = 24.f;
constexpr float kHomeTop = 20.f, kSearchTop = 20.f;
constexpr float kTileH = 48.f, kTileStep = 56.f;
constexpr float kShelfTitleH = 40.f;
// A collection: its coloured header, its buttons, then a row a song.
constexpr float kRowH = 56.f;

constexpr std::size_t kCoverMax = 40;
constexpr int kCoverSide = 160;
constexpr int kCoverJobs = 3;

// A dark desktop-player look (own colours, no brand assets).
nxui::Color rgb(int r, int g, int b, float a) { return nxui::Color(r / 255.f, g / 255.f, b / 255.f, a); }
nxui::Color bgBase(float a) { return rgb(0, 0, 0, a); }
nxui::Color bgPanel(float a) { return rgb(18, 18, 18, a); }
nxui::Color bgCard(float a) { return rgb(24, 24, 24, a); }
nxui::Color bgRaised(float a) { return rgb(40, 40, 40, a); }
nxui::Color bgHover(float a) { return rgb(58, 58, 58, a); }
nxui::Color textMain(float a) { return rgb(255, 255, 255, a); }
nxui::Color textSub(float a) { return rgb(179, 179, 179, a); }
nxui::Color textDim(float a) { return rgb(110, 110, 110, a); }
nxui::Color accent(float a) { return rgb(30, 215, 96, a); }

// A muted colour per name, for covers that aren't there (yet).
nxui::Color tint(const std::string& name, float a, float light = 1.f) {
    static constexpr int kTints[][3] = {{80, 56, 160}, {32, 104, 160}, {160, 56, 88},
                                        {40, 128, 96}, {176, 104, 32}, {96, 96, 112}};
    unsigned h = 2166136261u;
    for (unsigned char c : name) h = (h ^ c) * 16777619u;
    const int* t = kTints[h % 6];
    return rgb((int)(t[0] * light), (int)(t[1] * light), (int)(t[2] * light), a);
}

bool inside(const nxui::Rect& r, float x, float y) {
    return x >= r.x && x <= r.x + r.width && y >= r.y && y <= r.y + r.height;
}

bool overlaps(const nxui::Rect& a, const nxui::Rect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

// Drop whole UTF-8 characters until the text (plus "...") fits.
std::string fit(nxui::Font* font, std::string text, float maxW, float scale) {
    if (!font || font->measure(text).x * scale <= maxW) return text;
    while (!text.empty()) {
        while (!text.empty()) {
            const unsigned char c = static_cast<unsigned char>(text.back());
            text.pop_back();
            if ((c & 0xC0) != 0x80) break;
        }
        if (font->measure(text + "...").x * scale <= maxW) return text + "...";
    }
    return "...";
}

// The first character, for a cover's placeholder.
std::string initial(const std::string& name) {
    if (name.empty()) return "?";
    std::size_t n = 1;
    while (n < name.size() && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) ++n;
    std::string s = name.substr(0, n);
    if (n == 1 && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
    return s;
}

// The IPC's fixed-size text fields (NUL-terminated unless full).
std::string fixed(const char* s, std::size_t n) {
    std::size_t len = 0;
    while (len < n && s[len]) ++len;
    return std::string(s, len);
}

std::string clock(std::uint32_t ms) {
    const std::uint32_t s = ms / 1000;
    char b[16];
    std::snprintf(b, sizeof(b), "%u:%02u", s / 60, s % 60);
    return b;
}

std::string tr(const char* english) { return SpotifyBrowser::text(english); }

std::string kindLabel(SpotifyBrowser::Kind kind) {
    using K = SpotifyBrowser::Kind;
    switch (kind) {
    case K::Playlist: case K::Liked: return tr("Playlist");
    case K::Album: return tr("Album");
    case K::Artist: return tr("Artist");
    case K::Show: return tr("Podcast");
    case K::Track: return tr("Song");
    }
    return {};
}

// "{n} songs" with its number.
std::string trf(std::string text, const std::string& n) {
    const auto at = text.find("{n}");
    if (at != std::string::npos) text.replace(at, 3, n);
    return text;
}

float approach(float shown, float target, float dt) {
    const float k = 1.f - std::exp(-dt * 14.f);
    const float next = shown + (target - shown) * k;
    return std::fabs(target - next) < 0.5f ? target : next;
}

} // namespace

SpotifyPanel::SpotifyPanel() {
    setRect({0.f, 0.f, 1280.f, 720.f});
    setVisible(false);
}

// Two sizes: Discord's (the column turns into a rail of covers) or full.
void SpotifyPanel::setBig(bool big) {
    const bool changed = big != m_l.big;
    Layout l;
    if (big) {
        l.big = true;
        l.panelW = 1248.f;
        l.sideW = 320.f;
        l.tileCols = 4;
        l.cardW = 178.f, l.cardArt = 154.f, l.cardH = 248.f, l.shelfH = 300.f;
        l.headArt = 192.f, l.headH = 244.f, l.rowsTop = 384.f;
        l.albumColumn = true;
    }
    m_l = l;
    m_focus = std::clamp(m_focus, firstControl(), lastControl());
    if (m_view != View::Collection) m_shelf = std::clamp(m_shelf, -tileRows(), std::max(0, (int)shelves().size() - 1));
    m_col = 0;
    m_scroll = m_scrollShown = 0.f;
    std::fill(m_shelfScroll.begin(), m_shelfScroll.end(), 0.f);
    std::fill(m_shelfScrollShown.begin(), m_shelfScrollShown.end(), 0.f);
    if (m_side < 0 && !big) m_side = 0; // (the rail has no chips)
    if (!big && m_zone == Zone::Top) m_zone = Zone::Main;
    if (changed && m_active) {
        if (m_cb.onSizeChanged) m_cb.onSizeChanged(big);
        if (m_cb.onNavigate) m_cb.onNavigate();
    }
}

void SpotifyPanel::show() {
    m_active = true;
    m_dragging = 0;
    m_zone = Zone::Main;
    m_menu = false;
    m_focus = Play;
    m_fullSent = false;
    m_sysVolumeAt = 0;
    if (m_view != View::Collection) setView(m_view);
    if (!canBrowse()) m_zone = Zone::Side;
    setVisible(true);
    setFocusable(true);
    if (m_service) m_service->activate();
    if (m_browser) m_browser->start(); // (Home loads once signed in: see onUpdate)
    bindInput();
}

// A font keeps 144 strings at most and turns new ones away once full (they
// weren't drawn at all): HOME trims its own fonts, so the panel trims its
// four, at most twice a second, and empties them when it closes.
void SpotifyPanel::trimText(bool all) {
    nxui::Font* fonts[] = {m_font, m_smallFont, m_boldFont, m_titleFont};
    const std::uint64_t now = SpotifyPlayer::nowMs();
    if (!all && now < m_textTrimAt) return;
    bool any = all;
    for (nxui::Font* f : fonts) any = any || (f && f->maintenanceRequested());
    if (!any) return;
    m_textTrimAt = now + 500;
    // (its own two fonts emptied on close; the bold and title ones are HOME's too)
    for (nxui::Font* f : fonts)
        if (f) f->trimCache(all && (f == m_font || f == m_smallFont) ? 24 : 72);
}

void SpotifyPanel::hide() {
    if (!m_active) return;
    trimText(true);
    m_active = false;
    clearActions();
    setFocusable(false);
    // The music keeps playing in the player; only HOME's link and covers go.
    if (m_browser) m_browser->stop();
    if (m_service) m_service->deactivate();
    dropCovers();
    if (m_cb.onClosed) m_cb.onClosed();
}

// ---------------------------------------------------------------- covers

const nxui::Texture* SpotifyPanel::cover(const std::string& url) {
    if (url.empty()) return nullptr;
    auto it = m_covers.find(url);
    if (it == m_covers.end()) {
        it = m_covers.emplace(url, Cover{}).first;
        m_coverQueue.push_back(url);
    }
    it->second.usedFrame = m_frame;
    return it->second.tex && it->second.tex->valid() ? it->second.tex.get() : nullptr;
}

void SpotifyPanel::pumpCovers() {
    if (!m_gpu || !m_ren) return;
    // Newest wishes first: what's on screen now beats what scrolled past.
    while (m_coversLoading < kCoverJobs && !m_coverQueue.empty()) {
        const std::string url = m_coverQueue.back();
        m_coverQueue.pop_back();
        auto it = m_covers.find(url);
        if (it == m_covers.end() || it->second.tex || it->second.loading) continue;
        if (it->second.usedFrame + 2 < m_frame) { // scrolled away before its turn
            m_covers.erase(it);
            continue;
        }
        it->second.loading = true;
        ++m_coversLoading;
        std::weak_ptr<bool> alive = m_alive;
        const std::uint64_t epoch = m_coverEpoch;
        Async::Run(Async::Pool::Images, [this, url, alive, epoch]() {
            Http::Request req;
            req.url = url;
            req.timeoutMs = 15000;
            req.maxBytes = 1u << 20;
            Http::Response res = Http::Perform(req);
            std::string body = res.Ok() ? std::move(res.body) : std::string();
            Async::Post([this, url, alive, epoch, body = std::move(body)]() {
                if (alive.expired() || epoch != m_coverEpoch) return;
                --m_coversLoading;
                auto it = m_covers.find(url);
                if (it == m_covers.end()) return;
                it->second.loading = false;
                auto tex = std::make_unique<nxui::Texture>();
                if (!body.empty() && tex->loadFromMemory(*m_gpu, *m_ren, reinterpret_cast<const uint8_t*>(body.data()),
                                                         body.size(), kCoverSide))
                    it->second.tex = std::move(tex);
            });
        });
    }
    if (m_covers.size() <= kCoverMax) return;
    std::vector<std::pair<std::uint64_t, std::string>> old;
    for (const auto& [url, c] : m_covers)
        if (!c.loading && c.usedFrame + 1 < m_frame) old.emplace_back(c.usedFrame, url);
    std::sort(old.begin(), old.end());
    for (const auto& o : old) {
        if (m_covers.size() <= kCoverMax) break;
        m_covers.erase(o.second);
    }
}

void SpotifyPanel::dropCovers() {
    ++m_coverEpoch;
    m_icons.clear();
    m_covers.clear();
    m_coverQueue.clear();
    m_coversLoading = 0;
    m_nowCover.reset();
    m_nowCoverUrl.clear();
}

// The current song's cover, from the file the player writes.
void SpotifyPanel::loadNowCover() {
    const std::string wanted = hasTrack() ? m_snap.artworkUrl : std::string();
    if (wanted == m_nowCoverUrl || !m_gpu || !m_ren) return;
    m_nowCoverUrl = wanted;
    m_nowCover.reset();
    if (wanted.empty()) return;
    std::weak_ptr<bool> alive = m_alive;
    Async::Run(Async::Pool::Images, [this, wanted, alive]() {
        Http::Request req;
        req.url = wanted;
        req.timeoutMs = 15000;
        req.maxBytes = 2u << 20;
        Http::Response res = Http::Perform(req);
        std::string body = res.Ok() ? std::move(res.body) : std::string();
        Async::Post([this, wanted, alive, body = std::move(body)]() {
            if (alive.expired() || body.empty() || wanted != m_nowCoverUrl || !m_active) return;
            auto tex = std::make_unique<nxui::Texture>();
            if (tex->loadFromMemory(*m_gpu, *m_ren, reinterpret_cast<const uint8_t*>(body.data()), body.size(), 160))
                m_nowCover = std::move(tex);
        });
    });
}

// ---------------------------------------------------------------- input

void SpotifyPanel::bindInput() {
    clearActions();
    addDirectionAction(nxui::FocusDirection::UP, [this]() { move(nxui::FocusDirection::UP); });
    addDirectionAction(nxui::FocusDirection::DOWN, [this]() { move(nxui::FocusDirection::DOWN); });
    addDirectionAction(nxui::FocusDirection::LEFT, [this]() { move(nxui::FocusDirection::LEFT); });
    addDirectionAction(nxui::FocusDirection::RIGHT, [this]() { move(nxui::FocusDirection::RIGHT); });
    addAction(static_cast<uint64_t>(nxui::Button::A), [this]() {
        // HOME's focus turns a tap on the (full-screen) panel into A: taps go
        // through handleTouch instead, so only the button counts here.
        if (m_input && !m_input->isDown(nxui::Button::A)) return;
        press();
    });
    addAction(static_cast<uint64_t>(nxui::Button::B), [this]() { back(); });
    addAction(static_cast<uint64_t>(nxui::Button::X), [this]() { // reload what's shown (or start the player again)
        if (m_service && m_snap.link != SpotifyPlayer::Link::Connected) {
            m_service->deactivate();
            m_service->activate();
        }
        if (!m_browser || !canBrowse()) return;
        m_browser->loadLibrary(true);
        switch (m_view) {
        case View::Home: m_browser->loadHome(true); break;
        case View::Search: if (!m_browse.query.empty()) m_browser->search(m_browse.query); break;
        case View::Collection: m_browser->open(m_current); break;
        }
    });
    addAction(static_cast<uint64_t>(nxui::Button::Y), [this]() { activate(Play); });
    addAction(static_cast<uint64_t>(nxui::Button::Plus), [this]() { setBig(true); });
    addAction(static_cast<uint64_t>(nxui::Button::Minus), [this]() { setBig(false); });
    addAction(static_cast<uint64_t>(nxui::Button::ZL), [this]() { seekBy(-kSeekStepMs); });
    addAction(static_cast<uint64_t>(nxui::Button::ZR), [this]() { seekBy(kSeekStepMs); });
    addAction(static_cast<uint64_t>(nxui::Button::L), [this]() { volumeBy(-kVolumeStep); });
    addAction(static_cast<uint64_t>(nxui::Button::R), [this]() { volumeBy(kVolumeStep); });
}

void SpotifyPanel::navigated() {
    if (m_cb.onNavigate) m_cb.onNavigate();
    keepVisible();
}

// Your Library's items through its filter (Liked Songs counts as a playlist).
std::vector<int> SpotifyPanel::libraryRows() const {
    using K = SpotifyBrowser::Kind;
    std::vector<int> rows;
    for (int i = 0; i < (int)m_browse.saved.size(); ++i) {
        const K k = m_browse.saved[(std::size_t)i].kind;
        const bool keep = m_filter < 0 || (m_filter == 0 && (k == K::Playlist || k == K::Liked))
                          || (m_filter == 1 && k == K::Album) || (m_filter == 2 && k == K::Artist);
        if (keep) rows.push_back(i);
    }
    return rows;
}

void SpotifyPanel::move(nxui::FocusDirection dir) {
    if (m_menu) { // the account menu takes the arrows
        using M = nxui::FocusDirection;
        if (dir == M::UP && m_menuSel > 0) --m_menuSel;
        else if (dir == M::DOWN && m_menuSel < 1) ++m_menuSel;
        else return;
        if (m_cb.onNavigate) m_cb.onNavigate();
        return;
    }
    if (!m_l.big) {
        compactMove(dir);
        return;
    }
    using D = nxui::FocusDirection;
    const auto& list = shelves();
    const int count = (int)list.size();
    const bool content = canBrowse();
    const int libCount = (int)libraryRows().size();
    switch (m_zone) {
    case Zone::Top:
        if (dir == D::LEFT && m_top > 0) --m_top;
        else if (dir == D::RIGHT && m_top < (content ? 2 : 1)) ++m_top;
        else if (dir == D::DOWN && content) m_zone = Zone::Main;
        else if (dir == D::DOWN) m_zone = Zone::Side;
        else return;
        break;
    case Zone::Side:
        if (m_side < 0) { // the chips
            if (dir == D::LEFT && m_chip > 0) --m_chip;
            else if (dir == D::RIGHT && m_chip + 1 < kChips) ++m_chip;
            else if (dir == D::RIGHT && content) m_zone = Zone::Main;
            else if (dir == D::UP) m_zone = Zone::Top;
            else if (dir == D::DOWN && libCount > 0) m_side = 0;
            else return;
        } else {
            if (dir == D::UP && (m_side > 0 || m_l.big)) --m_side;
            else if (dir == D::UP) m_zone = Zone::Top;
            else if (dir == D::DOWN && m_side + 1 < libCount) ++m_side;
            else if (dir == D::DOWN) m_zone = Zone::Bar;
            else if (dir == D::RIGHT && content) m_zone = Zone::Main;
            else return;
        }
        break;
    case Zone::Main:
        if (!content) {
            if (dir == D::LEFT) m_zone = Zone::Side;
            else if (dir == D::UP) m_zone = Zone::Top;
            else if (dir == D::DOWN) m_zone = Zone::Bar;
            else return;
        } else if (m_view == View::Home || m_view == View::Search) {
            const int first = -tileRows(); // Home's grid rows, then the shelves
            auto cols = [&](int row) {
                if (row < 0) {
                    const int start = (row - first) * m_l.tileCols;
                    return std::clamp((int)m_browse.tiles.size() - start, 0, m_l.tileCols);
                }
                return row < count ? (int)list[(std::size_t)row].items.size() : 0;
            };
            if (dir == D::LEFT && m_col > 0) --m_col;
            else if (dir == D::LEFT) m_zone = Zone::Side;
            else if (dir == D::RIGHT && m_col + 1 < cols(m_shelf)) ++m_col;
            else if (dir == D::UP && m_shelf > first) --m_shelf;
            else if (dir == D::UP) m_zone = Zone::Top;
            else if (dir == D::DOWN && m_shelf + 1 < count) ++m_shelf;
            else if (dir == D::DOWN) m_zone = Zone::Bar;
            else return;
            m_col = std::clamp(m_col, 0, std::max(0, cols(m_shelf) - 1));
        } else { // a collection: its buttons, then its songs
            const int rows = (int)m_browse.collection.items.size();
            if (m_row < 0 && dir == D::LEFT && m_action > 0) --m_action;
            else if (m_row < 0 && dir == D::RIGHT && m_action < 1) ++m_action;
            else if (dir == D::LEFT) m_zone = Zone::Side;
            else if (dir == D::UP && m_row > -1) --m_row;
            else if (dir == D::UP) m_zone = Zone::Top;
            else if (dir == D::DOWN && m_row + 1 < rows) ++m_row;
            else if (dir == D::DOWN) m_zone = Zone::Bar;
            else return;
        }
        break;
    case Zone::Bar:
        if (dir == D::LEFT && m_focus > firstControl()) --m_focus;
        else if (dir == D::RIGHT && m_focus < lastControl()) ++m_focus;
        else if (dir == D::UP) m_zone = content ? Zone::Main : Zone::Side;
        else return;
        break;
    }
    navigated();
}

void SpotifyPanel::press() {
    if (m_menu) {
        menuPress();
        return;
    }
    if (m_zone == Zone::Top && m_top == 2 && canBrowse()) { // the account
        m_menu = true;
        m_menuSel = 0;
        if (m_cb.onActivate) m_cb.onActivate();
        return;
    }
    if (m_zone == Zone::Bar) {
        activate(m_focus);
        return; // (activate() plays its own sound)
    }
    if (!m_l.big) {
        compactPress();
        return;
    }
    if (!canBrowse()) {
        if (m_cb.onSignIn) m_cb.onSignIn();
        return;
    }
    if (!m_browser) return;
    switch (m_zone) {
    case Zone::Top:
        if (m_top == 0) setView(View::Home);
        else {
            askSearch();
            return;
        }
        m_zone = Zone::Main;
        break;
    case Zone::Side:
        if (m_side < 0) {
            m_filter = m_filter == m_chip ? -1 : m_chip; // a chip filters; again, it stops
        } else {
            const std::vector<int> rows = libraryRows();
            if (m_side >= (int)rows.size()) return;
            openItem(m_browse.saved[(std::size_t)rows[(std::size_t)m_side]]);
        }
        break;
    case Zone::Main:
        if (m_view == View::Home || m_view == View::Search) {
            if (m_shelf < 0) {
                const int index = (m_shelf + tileRows()) * m_l.tileCols + m_col;
                if (index >= 0 && index < (int)m_browse.tiles.size()) openItem(m_browse.tiles[(std::size_t)index]);
                break;
            }
            const auto& list = shelves();
            if (m_shelf >= (int)list.size()) return;
            const auto& shelf = list[(std::size_t)m_shelf];
            if (m_col >= (int)shelf.items.size()) return;
            openItem(shelf.items[(std::size_t)m_col]);
        } else if (m_row < 0) {
            if (m_action == 1) activate(Shuffle);
            else if (hasTrack() && m_playingUri == m_current.uri) activate(Play); // pause / go on
            else {
                m_browser->playCollection(m_current);
                m_playingUri = m_current.uri;
            }
        } else if (m_row < (int)m_browse.collection.items.size()) {
            m_browser->play(m_browse.collection.items[(std::size_t)m_row]);
            m_playingUri = m_current.uri;
        }
        break;
    case Zone::Bar:
        break;
    }
    if (m_cb.onActivate) m_cb.onActivate();
    keepVisible();
}

void SpotifyPanel::openItem(const Item& e) {
    if (!m_browser) return;
    if (e.isTrack()) {
        m_browser->play(e);
        m_playingUri = e.pageUri;
    }
    else openCollection(e, m_view == View::Collection && m_zone == Zone::Main);
}

void SpotifyPanel::askSearch() {
    if (!m_cb.onAskText || !m_browser) return;
    m_cb.onAskText(m_browse.query, [this](const std::string& text) {
        if (text.empty() || !m_browser) return;
        setView(View::Search);
        m_browser->search(text);
        m_zone = Zone::Main;
    });
}

void SpotifyPanel::back() {
    if (m_menu) {
        m_menu = false;
        if (m_cb.onNavigate) m_cb.onNavigate();
        return;
    }
    if (!m_l.big) { // like Discord's: out of what a row opened, else closed
        if (m_view == View::Collection && m_rail < 2) {
            setView(m_from);
            m_zone = Zone::Main;
            m_listRow = 0;
            if (m_cb.onNavigate) m_cb.onNavigate();
            return;
        }
        hide();
        return;
    }
    if (m_view == View::Collection) {
        if (!m_history.empty()) {
            const Item prev = m_history.back();
            m_history.pop_back();
            openCollection(prev, false);
        } else {
            setView(m_from);
        }
        if (m_cb.onNavigate) m_cb.onNavigate();
        return;
    }
    if (m_view != View::Home) {
        setView(View::Home);
        if (m_cb.onNavigate) m_cb.onNavigate();
        return;
    }
    hide();
}

void SpotifyPanel::setView(View v) {
    if (v != View::Collection) m_history.clear();
    m_view = v;
    m_openUri.clear();
    m_shelf = -tileRows();
    m_col = 0;
    m_row = -1;
    m_action = 0;
    m_scroll = m_scrollShown = 0.f;
    const std::size_t n = shelves().size();
    m_shelfScroll.assign(n, 0.f);
    m_shelfScrollShown.assign(n, 0.f);
    m_shelfScrollMax.assign(n, 0.f);
}

void SpotifyPanel::openCollection(const Item& e, bool remember) {
    if (!m_browser) return;
    if (remember && m_view == View::Collection) m_history.push_back(m_current);
    else if (m_view != View::Collection) m_from = m_view;
    if (!remember) m_history.clear();
    m_current = e;
    m_browser->open(e);
    m_view = View::Collection;
    m_openUri = e.uri;
    m_row = -1;
    m_action = 0;
    m_scroll = m_scrollShown = 0.f;
}

// The focused thing scrolls into view.
void SpotifyPanel::keepVisible() {
    const float viewH = m_mainRect.height > 0.f ? m_mainRect.height : 520.f;
    auto show = [&](float& scroll, float top, float h, float view) {
        if (top < scroll) scroll = top;
        if (top + h > scroll + view) scroll = top + h - view;
        scroll = std::max(0.f, scroll);
    };
    if (m_zone == Zone::Main && (m_view == View::Home || m_view == View::Search)) {
        if (m_shelf < 0) {
            m_scroll = 0.f;
            return;
        }
        const float top = shelvesTop() + m_shelf * m_l.shelfH;
        show(m_scroll, top - 12.f, m_l.shelfH, viewH);
        if (m_shelf < (int)m_shelfScroll.size()) {
            const float viewW = (m_mainRect.width > 0.f ? m_mainRect.width : 904.f) - kPad * 2.f;
            show(m_shelfScroll[(std::size_t)m_shelf], m_col * m_l.cardW, m_l.cardW, viewW);
        }
    } else if (m_zone == Zone::Main) {
        if (m_row < 0) m_scroll = 0.f;
        else show(m_scroll, m_l.rowsTop + m_row * kRowH - kRowH, kRowH * 3.f, viewH);
    } else if (m_zone == Zone::Side && m_side >= 0) {
        const float listH = m_listRect.height > 0.f ? m_listRect.height : 400.f;
        show(m_sideScroll, m_side * kSideRowH, kSideRowH, listH);
    }
}

bool SpotifyPanel::hasTrack() const {
    return m_snap.link == SpotifyPlayer::Link::Connected && (m_snap.status.flags & PlayerStatusFlag_HasTrack);
}

bool SpotifyPanel::ready() const {
    return m_snap.link == SpotifyPlayer::Link::Connected && m_snap.status.service_state == PlayerServiceState_Ready;
}

bool SpotifyPanel::canBrowse() const { return m_cb.canBrowse && m_cb.canBrowse(); }

void SpotifyPanel::activate(int control) {
    if (!m_service) return;
    if (m_cb.onActivate) m_cb.onActivate();
    const auto& s = m_snap.status;
    switch (control) {
    case Shuffle:
        m_service->setShuffle(!(s.flags & PlayerStatusFlag_Shuffle));
        break;
    case Previous:
        m_service->previous();
        break;
    case Play: {
        const bool playing = m_pendingPlaying >= 0 ? m_pendingPlaying == 1
                                                   : s.playback_state == PlayerPlaybackState_Playing;
        if (hasTrack() && playing) { // freeze the clock where it is
            m_pendingSeek = (int)livePositionMs();
            m_seekHoldUntil = SpotifyPlayer::nowMs() + 1200;
        }
        m_service->playPause();
        if (hasTrack()) {
            m_pendingPlaying = playing ? 0 : 1;
            m_pendingUntil = SpotifyPlayer::nowMs() + 1200;
        }
        break;
    }
    case Next:
        m_service->next();
        break;
    case Repeat: {
        const PlayerRepeatMode next = (s.flags & PlayerStatusFlag_RepeatOne)   ? PlayerRepeatMode_Off
                                       : (s.flags & PlayerStatusFlag_RepeatAll) ? PlayerRepeatMode_One
                                                                                 : PlayerRepeatMode_All;
        m_service->setRepeat(next);
        break;
    }
    default:
        break;
    }
}

std::uint32_t SpotifyPanel::livePositionMs() const {
    const auto& s = m_snap.status;
    const std::uint64_t now = SpotifyPlayer::nowMs();
    if (m_pendingSeek >= 0 && now < m_seekHoldUntil) return (std::uint32_t)m_pendingSeek;
    std::uint64_t pos = s.position_ms;
    if (s.playback_state == PlayerPlaybackState_Playing && m_snap.readAtMs) pos += now - m_snap.readAtMs;
    return (std::uint32_t)std::min<std::uint64_t>(pos, s.duration_ms);
}

void SpotifyPanel::seekBy(int deltaMs) {
    if (!m_service || !hasTrack() || !m_snap.status.duration_ms) return;
    const int target = std::clamp((int)livePositionMs() + deltaMs, 0, (int)m_snap.status.duration_ms);
    m_service->seek((std::uint32_t)target);
    m_pendingSeek = target;
    m_seekHoldUntil = SpotifyPlayer::nowMs() + 1200;
}

// The console's own volume (its buttons' one), a step at a time.
void SpotifyPanel::volumeBy(int delta) {
    switchvolume::step(delta);
    m_sysVolume = switchvolume::get();
    if (m_sysVolume >= 0) {
        m_pendingVolume = m_sysVolume * 10;
        m_volumeHoldUntil = SpotifyPlayer::nowMs() + 600;
    }
}

// ---------------------------------------------------------------- update

void SpotifyPanel::onUpdate(float dt) {
    const float target = m_active ? 1.f : 0.f;
    if (m_anim != target) {
        const float step = dt / kAnimDur;
        m_anim = m_anim < target ? std::min(target, m_anim + step) : std::max(target, m_anim - step);
        if (m_anim <= 0.f && !m_active) setVisible(false);
    }
    if (!m_active || !m_service) return;
    ++m_frame;
    trimText(false);
    const auto snap = m_service->snapshot();
    if (snap.version != m_snap.version) {
        m_snap = snap;
        const std::uint64_t now = SpotifyPlayer::nowMs();
        if (now >= m_pendingUntil) m_pendingPlaying = -1;
        if (now >= m_volumeHoldUntil) m_pendingVolume = -1;
        if (now >= m_seekHoldUntil) m_pendingSeek = -1;
        // One volume, the console's: the player itself stays at full.
        if (!m_fullSent && m_snap.link == SpotifyPlayer::Link::Connected && m_snap.status.volume_permille != 1000) {
            m_service->setVolume(1000);
            m_fullSent = true;
        }
    }
    if (SpotifyPlayer::nowMs() >= m_sysVolumeAt) { // (the console's buttons change it too)
        m_sysVolumeAt = SpotifyPlayer::nowMs() + 400;
        m_sysVolume = switchvolume::get();
    }
    if (m_browser) {
        if (m_browser->version() != m_browseVersion) {
            m_browseVersion = m_browser->version();
            m_browse = m_browser->state();
            const int shelfCount = (int)shelves().size();
            if (m_view != View::Collection) m_shelf = std::clamp(m_shelf, -tileRows(), std::max(0, shelfCount - 1));
            m_side = std::clamp(m_side, -1, std::max(-1, (int)libraryRows().size() - 1));
            m_row = std::clamp(m_row, -1, std::max(-1, (int)m_browse.collection.items.size() - 1));
            m_rail = std::clamp(m_rail, 0, railCount() - 1);
            m_listRow = std::clamp(m_listRow, 0, std::max(0, (int)compactRows().size() - 1));
        }
        // Home and the playlists load once you're signed in; after a failure, X tries again.
        if (canBrowse() && m_browse.home == SpotifyBrowser::Load::None) m_browser->loadHome();
        if (canBrowse() && m_browse.library == SpotifyBrowser::Load::None) m_browser->loadLibrary();
        if (canBrowse() && m_browse.account.empty()) m_browser->loadAccount();
    }
    // Scrolls ease toward their targets (a finger moves them at once).
    m_scroll = std::clamp(m_scroll, 0.f, m_scrollMax);
    m_sideScroll = std::clamp(m_sideScroll, 0.f, m_sideScrollMax);
    m_scrollShown = m_dragging == 3 ? m_scroll : approach(m_scrollShown, m_scroll, dt);
    m_sideScrollShown = m_dragging == 4 ? m_sideScroll : approach(m_sideScrollShown, m_sideScroll, dt);
    for (std::size_t i = 0; i < m_shelfScroll.size(); ++i) {
        m_shelfScroll[i] = std::clamp(m_shelfScroll[i], 0.f, m_shelfScrollMax[i]);
        m_shelfScrollShown[i] = (m_dragging == 5 && m_dragShelf == (int)i)
                                    ? m_shelfScroll[i]
                                    : approach(m_shelfScrollShown[i], m_shelfScroll[i], dt);
    }
    loadNowCover();
    pumpCovers();
}

nxui::Rect SpotifyPanel::panelRect() const {
    const float closedX = -m_l.panelW - 10.f; // slides in from the left, like Discord's
    const float x = closedX + (kPanelX - closedX) * nxui::Easing::outCubic(m_anim);
    return {x, kPanelY, m_l.panelW, kPanelH};
}

// ---------------------------------------------------------------- Discord's size

namespace {
constexpr float kCompactTopH = 56.f;
constexpr float kRailW = 72.f, kRailCell = 64.f;
constexpr float kListRowH = 56.f, kListHeaderH = 40.f;

float rowHeight(const SpotifyPanel::Row& r) { return r.kind == SpotifyPanel::Row::Header ? kListHeaderH : kListRowH; }
} // namespace

// The list beside the rail: Home's (its grid, then each shelf under its
// title), Search's (the field, then the results), or a collection's songs.
std::vector<SpotifyPanel::Row> SpotifyPanel::compactRows() const {
    std::vector<Row> rows;
    if (m_view == View::Collection) {
        rows.push_back({Row::PlayAll, m_current.name, m_current});
        for (const Item& e : m_browse.collection.items) rows.push_back({Row::Entry, std::string(), e});
        return rows;
    }
    if (m_view == View::Search) rows.push_back({Row::SearchField, std::string(), Item{}});
    else
        for (const Item& e : m_browse.tiles) rows.push_back({Row::Entry, std::string(), e});
    for (const Shelf& shelf : shelves()) {
        rows.push_back({Row::Header, shelf.title, Item{}});
        for (const Item& e : shelf.items) rows.push_back({Row::Entry, std::string(), e});
    }
    return rows;
}

void SpotifyPanel::compactEnterRail() {
    if (!m_browser) return;
    if (m_rail == 0) setView(View::Home);
    else if (m_rail == 1) {
        setView(View::Search);
        if (m_browse.query.empty()) askSearch();
    } else if (m_rail - 2 < (int)m_browse.saved.size()) {
        m_from = View::Home;
        openCollection(m_browse.saved[(std::size_t)(m_rail - 2)], false);
    }
    const std::vector<Row> rows = compactRows();
    m_listRow = 0;
    while (m_listRow < (int)rows.size() && rows[(std::size_t)m_listRow].kind == Row::Header) ++m_listRow;
    m_scroll = m_scrollShown = 0.f;
    m_zone = Zone::Main;
}

void SpotifyPanel::compactMove(nxui::FocusDirection dir) {
    using D = nxui::FocusDirection;
    const std::vector<Row> rows = compactRows();
    switch (m_zone) {
    case Zone::Top:
    case Zone::Side:
        if (m_zone == Zone::Top) { // (the account button: A opens its menu)
            if (dir == D::DOWN) m_zone = Zone::Side;
            else return;
            break;
        }
        m_zone = Zone::Side;
        if (dir == D::UP && m_rail == 0 && canBrowse()) { m_zone = Zone::Top; m_top = 2; }
        else if (dir == D::UP && m_rail > 0) --m_rail;
        else if (dir == D::DOWN && m_rail + 1 < railCount()) ++m_rail;
        else if (dir == D::DOWN) m_zone = Zone::Bar;
        else if (dir == D::RIGHT && canBrowse()) compactEnterRail();
        else return;
        break;
    case Zone::Main: {
        auto step = [&](int from, int by) { // the next row that isn't a title
            for (int r = from + by; r >= 0 && r < (int)rows.size(); r += by)
                if (rows[(std::size_t)r].kind != Row::Header) return r;
            return -1;
        };
        if (dir == D::LEFT) m_zone = Zone::Side;
        else if (dir == D::UP && step(m_listRow, -1) >= 0) m_listRow = step(m_listRow, -1);
        else if (dir == D::DOWN && step(m_listRow, +1) >= 0) m_listRow = step(m_listRow, +1);
        else if (dir == D::DOWN) m_zone = Zone::Bar;
        else return;
        break;
    }
    case Zone::Bar:
        if (dir == D::LEFT && m_focus > firstControl()) --m_focus;
        else if (dir == D::RIGHT && m_focus < lastControl()) ++m_focus;
        else if (dir == D::UP) m_zone = canBrowse() ? Zone::Main : Zone::Side;
        else return;
        break;
    }
    if (m_cb.onNavigate) m_cb.onNavigate();
    // The focused row and rail cell scroll into view.
    float y = 0.f;
    for (int r = 0; r < (int)rows.size() && r < m_listRow; ++r) y += rowHeight(rows[(std::size_t)r]);
    const float viewH = m_mainRect.height > 0.f ? m_mainRect.height : 480.f;
    if (y - kListHeaderH < m_scroll) m_scroll = std::max(0.f, y - kListHeaderH);
    if (y + kListRowH > m_scroll + viewH) m_scroll = y + kListRowH - viewH;
    const float railH = m_listRect.height > 0.f ? m_listRect.height : 480.f;
    const float ry = m_rail * kRailCell;
    if (ry < m_sideScroll) m_sideScroll = ry;
    if (ry + kRailCell > m_sideScroll + railH) m_sideScroll = ry + kRailCell - railH;
}

void SpotifyPanel::compactPress() {
    if (m_zone == Zone::Top && m_top == 2 && canBrowse()) {
        m_menu = true;
        m_menuSel = 0;
        if (m_cb.onActivate) m_cb.onActivate();
        return;
    }
    if (!canBrowse()) {
        if (m_cb.onSignIn) m_cb.onSignIn();
        return;
    }
    if (!m_browser) return;
    if (m_zone != Zone::Main) { // the rail: into what it shows (A on a Discord server opens its channels)
        compactEnterRail();
        if (m_cb.onActivate) m_cb.onActivate();
        return;
    }
    const std::vector<Row> rows = compactRows();
    if (m_listRow < 0 || m_listRow >= (int)rows.size()) return;
    const Row& row = rows[(std::size_t)m_listRow];
    if (row.kind == Row::SearchField) {
        askSearch();
        return;
    }
    if (m_cb.onActivate) m_cb.onActivate();
    if (row.kind == Row::PlayAll) {
        if (hasTrack() && m_playingUri == m_current.uri) activate(Play); // pause / go on
        else {
            m_browser->playCollection(m_current);
            m_playingUri = m_current.uri;
        }
    } else if (row.item.isTrack()) {
        m_browser->play(row.item);
        m_playingUri = m_view == View::Collection ? m_current.uri : row.item.pageUri;
    } else if (row.kind == Row::Entry) { // a playlist or album from Home or Search: its songs, B comes back
        m_from = m_view;
        openCollection(row.item, false);
        m_listRow = 0;
        m_scroll = m_scrollShown = 0.f;
    }
}

void SpotifyPanel::drawCompact(nxui::Renderer& ren, const nxui::Rect& p, float a) {
    m_shelfRows.clear(); // (no shelves to drag sideways here)
    // The title bar: the logo and what the list shows.
    const float topCy = p.y + kCompactTopH * 0.5f;
    if (!m_logo && !m_logoPath.empty() && m_gpu && m_ren) {
        m_logo = std::make_unique<nxui::Texture>();
        if (!m_logo->loadFromFile(*m_gpu, *m_ren, m_logoPath, 64)) m_logo.reset();
        m_logoPath.clear();
    }
    if (m_logo) ren.drawTexture(m_logo.get(), {p.x + 16.f, topCy - 20.f, 40.f, 40.f}, textMain(a));
    const std::string title = m_view == View::Collection ? m_current.name
                              : m_view == View::Search   ? tr("Search")
                                                         : tr("Home");
    const std::string hint = tr("+ Bigger");
    const float hintW = m_smallFont->measure(hint).x * 0.6f;
    const bool account = canBrowse();
    const float right = account ? 56.f : 0.f; // (the account button's room)
    if (account) drawAccount(ren, {p.x + p.width - 34.f, topCy}, m_zone == Zone::Top && m_top == 2, a);
    ren.drawText(fit(m_boldFont, title, p.width - kRailW - hintW - right - 40.f, 0.8f), {p.x + kRailW + 8.f, topCy - 13.f},
                 m_boldFont, textMain(a), 0.8f);
    ren.drawText(hint, {p.x + p.width - 16.f - right - hintW, topCy - 9.f}, m_smallFont, textDim(a), 0.6f);

    const float bodyY = p.y + kCompactTopH, bodyH = p.height - kCompactTopH - kBarH;
    // The rail: Home, Search, then Your Library's covers.
    const nxui::Rect rail{p.x, bodyY, kRailW, bodyH};
    m_listRect = rail;
    ren.drawRect(rail, bgBase(a));
    m_sideScrollMax = std::max(0.f, railCount() * kRailCell - rail.height);
    ren.pushClipRect(rail);
    for (int i = 0; i < railCount(); ++i) {
        const float cy = rail.y + i * kRailCell - m_sideScrollShown + kRailCell * 0.5f;
        if (cy + kRailCell < rail.y || cy - kRailCell > rail.y + rail.height) continue;
        const bool chosen = (i == 0 && m_view == View::Home) || (i == 1 && m_view == View::Search)
                            || (i >= 2 && m_view == View::Collection && m_browse.saved[(std::size_t)(i - 2)].uri == m_openUri);
        const bool focused = m_zone == Zone::Side && m_rail == i;
        if (chosen) ren.drawRoundedRect({rail.x, cy - 20.f, 4.f, 40.f}, textMain(a), 2.f); // Discord's pill
        const nxui::Rect cell{rail.x + 12.f, cy - 24.f, 48.f, 48.f};
        if (i < 2) {
            ren.drawCircle({cell.x + 24.f, cy}, 24.f, chosen ? bgHover(a) : bgRaised(a), 40);
            drawIcon(ren, i == 0 ? "home" : "search", {cell.x + 12.f, cy - 12.f, 24.f, 24.f},
                     chosen ? textMain(a) : textSub(a));
        } else {
            const Item& e = m_browse.saved[(std::size_t)(i - 2)];
            drawArt(ren, cell, e, e.kind == SpotifyBrowser::Kind::Artist ? 24.f : 6.f, a);
        }
        if (focused)
            ren.drawRoundedRectOutline({cell.x - 4.f, cell.y - 4.f, 56.f, 56.f}, accent(a),
                                       i < 2 || (i >= 2 && m_browse.saved[(std::size_t)(i - 2)].kind == SpotifyBrowser::Kind::Artist) ? 28.f : 9.f,
                                       2.f);
        m_hits.push_back({{rail.x, cy - kRailCell * 0.5f, kRailW, kRailCell}, [this, i]() {
                              m_zone = Zone::Side;
                              m_rail = i;
                              compactPress();
                          }});
    }
    ren.popClipRect();

    // The list.
    const nxui::Rect list{p.x + kRailW, bodyY, p.width - kRailW, bodyH};
    m_mainRect = list;
    ren.drawRect(list, bgPanel(a));
    if (!canBrowse()) {
        ren.drawText(fit(m_boldFont, tr("Connect your Spotify account"), list.width - 32.f, 0.8f),
                     {list.x + 16.f, list.y + 40.f}, m_boldFont, textMain(a), 0.8f);
        ren.drawText(fit(m_smallFont, tr("A: sign in with a QR code on your phone"), list.width - 32.f, 0.7f),
                     {list.x + 16.f, list.y + 76.f}, m_smallFont, textSub(a), 0.7f);
        m_hits.push_back({list, [this]() {
                              if (m_cb.onSignIn) m_cb.onSignIn();
                          }});
        m_scrollMax = 0.f;
        return;
    }
    const std::vector<Row> rows = compactRows();
    float total = 0.f;
    for (const Row& r : rows) total += rowHeight(r);
    m_scrollMax = std::max(0.f, total + 16.f - list.height);
    const std::string playing = hasTrack() ? fixed(m_snap.status.title, sizeof(m_snap.status.title)) : std::string();
    ren.pushClipRect(list);
    float y = list.y + 8.f - m_scrollShown;
    for (int i = 0; i < (int)rows.size(); ++i) {
        const Row& r = rows[(std::size_t)i];
        const float h = rowHeight(r);
        const nxui::Rect row{list.x + 8.f, y, list.width - 16.f, h};
        y += h;
        if (row.y + row.height < list.y || row.y > list.y + list.height) continue;
        const bool focused = m_zone == Zone::Main && m_listRow == i;
        if (focused && r.kind != Row::Header) ren.drawRoundedRect(row, bgHover(a), 6.f);
        switch (r.kind) {
        case Row::Header:
            ren.drawText(fit(m_boldFont, r.title, row.width - 16.f, 0.72f), {row.x + 8.f, row.y + 12.f}, m_boldFont,
                         textMain(a), 0.72f);
            continue;
        case Row::SearchField: {
            const nxui::Rect field{row.x + 4.f, row.y + 6.f, row.width - 8.f, 44.f};
            ren.drawRoundedRect(field, focused ? bgHover(a) : bgRaised(a), 22.f);
            drawIcon(ren, "search", {field.x + 12.f, field.y + 11.f, 22.f, 22.f}, textSub(a));
            const bool empty = m_browse.query.empty();
            ren.drawText(fit(m_font, empty ? tr("What do you want to play?") : m_browse.query, field.width - 60.f, 0.66f),
                         {field.x + 44.f, field.y + 11.f}, m_font, empty ? textSub(a) : textMain(a), 0.66f);
            break;
        }
        case Row::PlayAll: {
            drawPlayButton(ren, row.x + 30.f, row.y + h * 0.5f, 22.f, false, playingHere(m_current.uri), a);
            std::string sub = m_current.subtitle;
            const int songs = (int)m_browse.collection.items.size();
            if (songs > 0)
                sub += (sub.empty() ? "" : "  \xE2\x80\xA2  ")
                       + trf(songs == 1 ? tr("{n} song") : tr("{n} songs"), std::to_string(songs));
            ren.drawText(fit(m_boldFont, kindLabel(m_current.kind), row.width - 80.f, 0.62f), {row.x + 64.f, row.y + 8.f},
                         m_boldFont, textMain(a), 0.62f);
            ren.drawText(fit(m_smallFont, sub, row.width - 80.f, 0.64f), {row.x + 64.f, row.y + 31.f}, m_smallFont,
                         textSub(a), 0.64f);
            break;
        }
        case Row::Entry: {
            const Item& e = r.item;
            drawArt(ren, {row.x + 8.f, row.y + 8.f, 40.f, 40.f}, e,
                    e.kind == SpotifyBrowser::Kind::Artist ? 20.f : 4.f, a);
            const bool now = e.isTrack() && !playing.empty() && e.name == playing;
            const float tw = row.width - 72.f - (e.durationMs ? 48.f : 0.f);
            ren.drawText(fit(m_font, e.name, tw, 0.64f), {row.x + 60.f, row.y + 8.f}, m_font,
                         now ? accent(a) : textMain(a), 0.64f);
            std::string sub = e.subtitle;
            if (!e.isTrack()) {
                const std::string kind = kindLabel(e.kind);
                sub = kind + (sub.empty() || sub == kind ? std::string() : "  \xE2\x80\xA2  " + sub);
            }
            ren.drawText(fit(m_smallFont, sub, tw, 0.62f), {row.x + 60.f, row.y + 31.f}, m_smallFont, textSub(a), 0.62f);
            if (e.durationMs)
                ren.drawText(clock(e.durationMs), {row.x + row.width - 50.f, row.y + 19.f}, m_smallFont, textSub(a),
                             0.64f);
            break;
        }
        }
        nxui::Rect hit = row;
        hit.y = std::max(row.y, list.y);
        hit.height = std::min(row.y + row.height, list.y + list.height) - hit.y;
        if (hit.height > 0.f)
            m_hits.push_back({hit, [this, i]() {
                                  m_zone = Zone::Main;
                                  m_listRow = i;
                                  compactPress();
                              }});
    }
    ren.popClipRect();
    using L = SpotifyBrowser::Load;
    std::string msg;
    if (m_view == View::Collection && m_browse.collection.load == L::Loading) msg = tr("Loading...");
    else if (m_view == View::Collection && m_browse.collection.load == L::Failed)
        msg = m_browse.collection.error + "  -  " + tr("X to try again");
    else if (m_view == View::Home && rows.empty())
        msg = m_browse.home == L::Failed ? m_browse.homeError + "  -  " + tr("X to try again") : tr("Loading...");
    else if (m_view == View::Search && m_browse.search == L::Loading) msg = tr("Loading...");
    else if (m_view == View::Search && m_browse.search == L::Ready && m_browse.results.empty()) msg = tr("No results");
    if (!msg.empty() && y + 24.f < list.y + list.height)
        ren.drawText(fit(m_smallFont, msg, list.width - 32.f, 0.66f), {list.x + 16.f, y + 8.f}, m_smallFont,
                     textDim(a), 0.66f);
    if (!m_browse.playError.empty())
        ren.drawText(fit(m_smallFont, m_browse.playError, list.width - 32.f, 0.64f),
                     {list.x + 16.f, list.y + list.height - 28.f}, m_smallFont, rgb(240, 110, 110, a), 0.64f);
}

// ---------------------------------------------------------------- drawing

float SpotifyPanel::shelvesTop() const {
    if (m_view == View::Search) return kSearchTop;
    const int rows = tileRows();
    return kHomeTop + (rows > 0 ? rows * kTileStep + 18.f : 0.f);
}

// A cover, Liked Songs' heart, or the name's first letter on a colour while there's none.
void SpotifyPanel::drawArt(nxui::Renderer& ren, const nxui::Rect& r, const Item& item, float radius, float a) {
    if (item.kind == SpotifyBrowser::Kind::Liked) {
        ren.drawGradientRect(r, rgb(72, 24, 220, a), rgb(120, 150, 210, a));
        const float s = r.width * 0.16f, cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.47f;
        ren.drawCircle({cx - s * 0.55f, cy - s * 0.2f}, s * 0.62f, textMain(a), 24);
        ren.drawCircle({cx + s * 0.55f, cy - s * 0.2f}, s * 0.62f, textMain(a), 24);
        ren.drawTriangle({cx - s * 1.13f, cy + s * 0.05f}, {cx + s * 1.13f, cy + s * 0.05f}, {cx, cy + s * 1.25f},
                         textMain(a));
        return;
    }
    if (const nxui::Texture* tex = cover(item.imageUrl)) {
        ren.drawTextureRounded(tex, r, radius, nxui::Color::white().withAlpha(a));
        return;
    }
    ren.drawRoundedRect(r, tint(item.name, a, 0.55f), radius);
    const std::string letter = initial(item.name);
    const float scale = r.width / 60.f;
    const nxui::Vec2 m = m_boldFont->measure(letter);
    ren.drawText(letter, {r.x + (r.width - m.x * scale) * 0.5f, r.y + (r.height - m.y * scale) * 0.5f}, m_boldFont,
                 textMain(0.85f * a), scale);
}

// The logo, Home and the search field, like the web player's.
void SpotifyPanel::drawTopBar(nxui::Renderer& ren, const nxui::Rect& area, float a) {
    if (!m_logo && !m_logoPath.empty() && m_gpu && m_ren) {
        m_logo = std::make_unique<nxui::Texture>();
        if (!m_logo->loadFromFile(*m_gpu, *m_ren, m_logoPath, 64)) m_logo.reset();
        m_logoPath.clear(); // (one try)
    }
    const float cy = area.y + area.height * 0.5f;
    if (m_logo) ren.drawTexture(m_logo.get(), {area.x + 12.f, cy - 22.f, 44.f, 44.f}, textMain(a));
    else ren.drawCircle({area.x + 34.f, cy}, 16.f, textMain(a), 32);

    const bool top = m_zone == Zone::Top;
    // Home and Search: round buttons (Discord's size), or Home beside the field.
    const float pillW = m_l.big ? 440.f : 0.f, pillH = 48.f;
    const nxui::Rect pill{area.x + (area.width - pillW) * 0.5f - (m_l.big ? 60.f : 0.f), cy - pillH * 0.5f, pillW, pillH};
    const nxui::Vec2 home{m_l.big ? pill.x - 34.f : area.x + area.width - 90.f, cy};
    const bool homeFocus = top && m_top == 0;
    ren.drawCircle(home, 24.f, homeFocus ? bgHover(a) : bgRaised(a), 40);
    if (homeFocus) ren.drawRoundedRectOutline({home.x - 26.f, home.y - 26.f, 52.f, 52.f}, textMain(a), 26.f, 2.f);
    drawIcon(ren, "home", {home.x - 12.f, home.y - 12.f, 24.f, 24.f}, m_view == View::Home ? textMain(a) : textSub(a));
    m_hits.push_back({{home.x - 24.f, home.y - 24.f, 48.f, 48.f}, [this]() {
                          m_zone = Zone::Top;
                          m_top = 0;
                          press();
                      }});
    if (!m_l.big) {
        const nxui::Vec2 find{area.x + area.width - 34.f, cy};
        const bool findFocus = top && m_top == 1;
        ren.drawCircle(find, 24.f, findFocus ? bgHover(a) : bgRaised(a), 40);
        if (findFocus) ren.drawRoundedRectOutline({find.x - 26.f, find.y - 26.f, 52.f, 52.f}, textMain(a), 26.f, 2.f);
        drawIcon(ren, "search", {find.x - 12.f, find.y - 12.f, 24.f, 24.f},
                 m_view == View::Search ? textMain(a) : textSub(a));
        m_hits.push_back({{find.x - 24.f, find.y - 24.f, 48.f, 48.f}, [this]() {
                              m_zone = Zone::Top;
                              m_top = 1;
                              press();
                          }});
        const std::string hint = tr("+ Bigger");
        ren.drawText(hint, {area.x + 66.f, cy - 9.f}, m_smallFont, textDim(a), 0.6f);
        return;
    }
    // The search field.
    const bool fieldFocus = top && m_top == 1;
    ren.drawRoundedRect(pill, fieldFocus ? bgHover(a) : bgRaised(a), pillH * 0.5f);
    if (fieldFocus) ren.drawRoundedRectOutline(pill, textMain(a), pillH * 0.5f, 2.f);
    drawIcon(ren, "search", {pill.x + 14.f, cy - 12.f, 24.f, 24.f}, textSub(a));
    const bool empty = m_browse.query.empty() || m_view != View::Search;
    ren.drawText(fit(m_font, empty ? tr("What do you want to play?") : m_browse.query, pillW - 70.f, 0.72f),
                 {pill.x + 50.f, cy - 12.f}, m_font, empty ? textSub(a) : textMain(a), 0.72f);
    m_hits.push_back({pill, [this]() {
                          m_zone = Zone::Top;
                          m_top = 1;
                          press();
                      }});
    // The buttons, on the right.
    const std::string hint = tr("- Smaller   B Back   X Reload   Y Play/Pause   L/R Volume");
    const float hintW = m_smallFont->measure(hint).x * 0.58f;
    const bool account = canBrowse();
    if (account) drawAccount(ren, {area.x + area.width - 40.f, cy}, top && m_top == 2, a);
    ren.drawText(hint, {area.x + area.width - (account ? 76.f : 16.f) - hintW, cy - 9.f}, m_smallFont, textDim(a), 0.58f);
}

// The account: a round button with the name's first letter (open.spotify.com's avatar).
void SpotifyPanel::drawAccount(nxui::Renderer& ren, const nxui::Vec2& c, bool focused, float a) {
    m_accountPos = c;
    ren.drawCircle(c, 21.f, nxui::Color(0.11f, 0.73f, 0.33f, a), 40);
    const nxui::Texture* pic = cover(m_browse.accountImage); // (their picture, else the letter)
    if (pic) ren.drawTextureRounded(pic, {c.x - 21.f, c.y - 21.f, 42.f, 42.f}, 21.f, nxui::Color::white().withAlpha(a));
    if (focused) ren.drawRoundedRectOutline({c.x - 25.f, c.y - 25.f, 50.f, 50.f}, textMain(a), 25.f, 2.f);
    const std::string& n = m_browse.account;
    if (!pic) {
        std::size_t len = n.empty() ? 0 : 1; // (one whole UTF-8 letter)
        while (len < n.size() && (static_cast<unsigned char>(n[len]) & 0xC0) == 0x80) ++len;
        const std::string letter = n.empty() ? std::string("?") : n.substr(0, len);
        const float w = m_boldFont->measure(letter).x * 0.8f;
        ren.drawText(letter, {c.x - w * 0.5f, c.y - 11.f}, m_boldFont, nxui::Color(0.f, 0.f, 0.f, a), 0.8f);
    }
    m_hits.push_back({{c.x - 24.f, c.y - 24.f, 48.f, 48.f}, [this]() {
                          m_zone = Zone::Top;
                          m_top = 2;
                          press();
                      }});
}

// Its menu: whose account, Sign out, Switch account (B or a tap outside closes it).
void SpotifyPanel::drawAccountMenu(nxui::Renderer& ren, float a) {
    const nxui::Rect p = panelRect();
    m_hits.push_back({{0.f, 0.f, 1280.f, 720.f}, [this]() { m_menu = false; }}); // (under the menu's own)
    const float w = 280.f, rowH = 44.f, headH = 62.f, pad = 8.f;
    const float h = headH + rowH * 2.f + pad;
    const float x = std::min(m_accountPos.x + 24.f, p.x + p.width - 8.f) - w;
    const nxui::Rect box{std::max(p.x + 8.f, x), m_accountPos.y + 30.f, w, h};
    ren.drawRoundedRect({box.x - 1.f, box.y - 1.f, box.width + 2.f, box.height + 2.f}, bgHover(a), 12.f);
    ren.drawRoundedRect(box, bgRaised(a), 12.f);
    ren.drawText(tr("Signed in as"), {box.x + 16.f, box.y + 10.f}, m_smallFont, textDim(a), 0.6f);
    ren.drawText(fit(m_boldFont, m_browse.account.empty() ? std::string("Spotify") : m_browse.account, w - 32.f, 0.75f),
                 {box.x + 16.f, box.y + 30.f}, m_boldFont, textMain(a), 0.75f);
    m_hits.push_back({box, []() {}}); // (a tap on the menu itself keeps it)
    const char* labels[2] = {"Sign out", "Switch account"};
    for (int i = 0; i < 2; ++i) {
        const nxui::Rect row{box.x + pad, box.y + headH + rowH * i, w - pad * 2.f, rowH - 4.f};
        const bool on = m_menuSel == i;
        if (on) {
            ren.drawRoundedRect(row, bgHover(a), 8.f);
            ren.drawRoundedRectOutline(row, textMain(a), 8.f, 2.f);
        }
        ren.drawText(fit(m_font, tr(labels[i]), row.width - 28.f, 0.72f), {row.x + 14.f, row.y + 10.f}, m_font,
                     on ? textMain(a) : textSub(a), 0.72f);
        m_hits.push_back({row, [this, i]() {
                              m_menuSel = i;
                              menuPress();
                          }});
    }
}

void SpotifyPanel::menuPress() {
    m_menu = false;
    if (m_menuSel == 0) {
        if (m_cb.onSignOut) m_cb.onSignOut();
        m_zone = Zone::Side; // (signed out: the panel asks to sign in)
        m_top = 0;
        if (m_cb.onActivate) m_cb.onActivate();
    } else if (m_cb.onSwitchAccount) {
        m_cb.onSwitchAccount();
    }
}


// Your Library: its filters, then your playlists, albums and artists.
void SpotifyPanel::drawSidebar(nxui::Renderer& ren, const nxui::Rect& area, float a) {
    m_sideRect = area;
    ren.drawRoundedRect(area, bgPanel(a), 8.f);
    const bool sideFocus = m_zone == Zone::Side;
    if (!m_l.big) { // Discord's size: a rail of covers, like the web player's collapsed library
        const nxui::Rect list{area.x, area.y + 8.f, area.width, area.height - 16.f};
        m_listRect = list;
        const std::vector<int> rows = libraryRows();
        m_sideScrollMax = std::max(0.f, rows.size() * kSideRowH - list.height);
        ren.pushClipRect(list);
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const float ry = list.y + r * kSideRowH - m_sideScrollShown;
            if (ry + kSideRowH < list.y || ry > list.y + list.height) continue;
            const Item& e = m_browse.saved[(std::size_t)rows[r]];
            const nxui::Rect cell{area.x + 4.f, ry, area.width - 8.f, kSideRowH};
            const bool open = m_view == View::Collection && !m_openUri.empty() && e.uri == m_openUri;
            if (sideFocus && m_side == (int)r) ren.drawRoundedRect(cell, bgHover(a), 6.f);
            else if (open) ren.drawRoundedRect(cell, bgRaised(a), 6.f);
            drawArt(ren, {cell.x + (cell.width - 48.f) * 0.5f, cell.y + 8.f, 48.f, 48.f}, e,
                    e.kind == SpotifyBrowser::Kind::Artist ? 24.f : 4.f, a);
            m_hits.push_back({cell, [this, r]() {
                                  m_zone = Zone::Side;
                                  m_side = (int)r;
                                  press();
                              }});
        }
        ren.popClipRect();
        return;
    }
    ren.drawText(tr("Your Library"), {area.x + 18.f, area.y + 18.f}, m_boldFont, textMain(a), 0.72f);

    // The chips.
    static const char* kChipNames[kChips] = {"Playlists", "Albums", "Artists"};
    float cx = area.x + 16.f;
    const float cy = area.y + 56.f;
    for (int i = 0; i < kChips; ++i) {
        const std::string label = tr(kChipNames[i]);
        const float w = m_smallFont->measure(label).x * 0.72f + 28.f;
        const nxui::Rect chip{cx, cy, w, 32.f};
        const bool on = m_filter == i;
        ren.drawRoundedRect(chip, on ? textMain(a) : bgRaised(a), 16.f);
        if (sideFocus && m_side < 0 && m_chip == i)
            ren.drawRoundedRectOutline({chip.x - 3.f, chip.y - 3.f, chip.width + 6.f, chip.height + 6.f}, accent(a),
                                       19.f, 2.f);
        ren.drawText(label, {chip.x + 14.f, chip.y + 7.f}, m_smallFont, on ? bgBase(a) : textMain(a), 0.72f);
        m_hits.push_back({chip, [this, i]() {
                              m_zone = Zone::Side;
                              m_side = -1;
                              m_chip = i;
                              press();
                          }});
        cx += w + 8.f;
    }

    // The list, scrolling under them.
    const nxui::Rect list{area.x, area.y + kSideListTop, area.width, area.height - kSideListTop - 8.f};
    m_listRect = list;
    const std::vector<int> rows = libraryRows();
    m_sideScrollMax = std::max(0.f, rows.size() * kSideRowH - list.height);
    if (rows.empty()) {
        if (canBrowse() && m_browse.library != SpotifyBrowser::Load::Ready)
            ren.drawText(m_browse.library == SpotifyBrowser::Load::Failed ? m_browse.libraryError : tr("Loading..."),
                         {list.x + 18.f, list.y + 4.f}, m_smallFont, textDim(a), 0.7f);
        return;
    }
    ren.pushClipRect(list);
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const float ry = list.y + r * kSideRowH - m_sideScrollShown;
        const nxui::Rect row{list.x + 8.f, ry, list.width - 16.f, kSideRowH};
        if (row.y + row.height < list.y || row.y > list.y + list.height) continue;
        const Item& e = m_browse.saved[(std::size_t)rows[r]];
        const bool focused = sideFocus && m_side == (int)r;
        const bool open = m_view == View::Collection && !m_openUri.empty() && e.uri == m_openUri;
        if (focused) ren.drawRoundedRect(row, bgHover(a), 6.f);
        else if (open) ren.drawRoundedRect(row, bgRaised(a), 6.f);
        drawArt(ren, {row.x + 8.f, row.y + 8.f, 48.f, 48.f}, e, e.kind == SpotifyBrowser::Kind::Artist ? 24.f : 4.f, a);
        const float tx = row.x + 68.f, tw = row.width - 76.f;
        ren.drawText(fit(m_font, e.name, tw, 0.66f), {tx, row.y + 12.f}, m_font, open ? accent(a) : textMain(a), 0.66f);
        std::string sub = kindLabel(e.kind);
        if (!e.subtitle.empty() && e.subtitle != sub) sub += "  \xE2\x80\xA2  " + e.subtitle;
        ren.drawText(fit(m_smallFont, sub, tw, 0.66f), {tx, row.y + 36.f}, m_smallFont, textSub(a), 0.66f);
        const nxui::Rect hit = {row.x, std::max(row.y, list.y), row.width,
                                std::min(row.y + row.height, list.y + list.height) - std::max(row.y, list.y)};
        m_hits.push_back({hit, [this, r]() {
                              m_zone = Zone::Side;
                              m_side = (int)r;
                              press();
                          }});
    }
    ren.popClipRect();
}

// Not signed in (or without the library's rights): the way to sign in.
bool SpotifyPanel::drawStatus(nxui::Renderer& ren, const nxui::Rect& area, float a) {
    if (canBrowse()) return false;
    const float w = area.width - kPad * 4.f;
    ren.drawText(fit(m_titleFont, tr("Connect your Spotify account"), w, 1.0f), {area.x + kPad * 2.f, area.y + 120.f},
                 m_titleFont, textMain(a), 1.0f);
    ren.drawText(fit(m_font, tr("A: sign in with a QR code on your phone"), w, 0.8f),
                 {area.x + kPad * 2.f, area.y + 170.f}, m_font, textSub(a), 0.8f);
    m_hits.push_back({area, [this]() {
                          if (m_cb.onSignIn) m_cb.onSignIn();
                      }});
    return true;
}

// Rows of cards under a bold title: Home's and Search's.
void SpotifyPanel::drawShelves(nxui::Renderer& ren, const nxui::Rect& area, float top, const std::vector<Shelf>& list,
                               float a) {
    m_shelfRows.assign(list.size(), nxui::Rect{});
    if (m_shelfScroll.size() != list.size()) {
        m_shelfScroll.assign(list.size(), 0.f);
        m_shelfScrollShown.assign(list.size(), 0.f);
        m_shelfScrollMax.assign(list.size(), 0.f);
    }
    const bool mainFocus = m_zone == Zone::Main;
    const float rowW = area.width - kPad * 2.f;
    for (std::size_t s = 0; s < list.size(); ++s) {
        const float sy = top + s * m_l.shelfH;
        if (sy + m_l.shelfH < area.y || sy > area.y + area.height) continue;
        const auto& shelf = list[s];
        ren.drawText(fit(m_titleFont, shelf.title, rowW, 0.8f), {area.x + kPad, sy}, m_titleFont, textMain(a), 0.8f);
        const nxui::Rect row{area.x + kPad - 12.f, sy + kShelfTitleH, rowW + 24.f, m_l.cardH};
        m_shelfRows[s] = row;
        m_shelfScrollMax[s] = std::max(0.f, shelf.items.size() * m_l.cardW - rowW);
        const float sx = m_shelfScrollShown[s];
        ren.pushClipRect(row);
        for (std::size_t c = 0; c < shelf.items.size(); ++c) {
            const nxui::Rect card{area.x + kPad - 12.f + c * m_l.cardW - sx, row.y, m_l.cardW, m_l.cardH};
            if (card.x + card.width < row.x || card.x > row.x + row.width) continue;
            const Item& e = shelf.items[c];
            const bool focused = mainFocus && m_shelf == (int)s && m_col == (int)c;
            if (focused) ren.drawRoundedRect(card, bgRaised(a), 8.f); // (cards have no background until focused)
            const nxui::Rect art{card.x + 12.f, card.y + 12.f, m_l.cardArt, m_l.cardArt};
            const bool round = e.kind == SpotifyBrowser::Kind::Artist;
            drawArt(ren, art, e, round ? m_l.cardArt * 0.5f : 6.f, a);
            if (focused)
                drawPlayButton(ren, art.x + art.width - 30.f, art.y + art.height - 30.f, 24.f, false,
                               playingHere(e.isTrack() ? std::string() : e.uri), a);
            ren.drawText(fit(m_font, e.name, m_l.cardArt, 0.66f), {art.x, art.y + m_l.cardArt + 10.f}, m_font, textMain(a),
                         0.66f);
            const std::string sub = !e.subtitle.empty() ? e.subtitle : kindLabel(e.kind);
            ren.drawText(fit(m_smallFont, sub, m_l.cardArt, 0.66f), {art.x, art.y + m_l.cardArt + 34.f}, m_smallFont,
                         textSub(a), 0.66f);
            if (!overlaps(card, area) || !overlaps(card, row)) continue;
            nxui::Rect hit = card;
            hit.x = std::max(card.x, row.x);
            hit.width = std::min(card.x + card.width, row.x + row.width) - hit.x;
            m_hits.push_back({hit, [this, s, c]() {
                                  m_zone = Zone::Main;
                                  m_shelf = (int)s;
                                  m_col = (int)c;
                                  press();
                              }});
        }
        ren.popClipRect();
    }
}

// Home: a grid of what you played lately (4 across), then the shelves.
void SpotifyPanel::drawHome(nxui::Renderer& ren, const nxui::Rect& area, float a) {
    using L = SpotifyBrowser::Load;
    const float top = area.y - m_scrollShown;
    const auto& list = m_browse.shelves;
    m_scrollMax = std::max(0.f, shelvesTop() + list.size() * m_l.shelfH + 12.f - area.height);
    if (list.empty() && m_browse.tiles.empty()) {
        const std::string msg = m_browse.home == L::Failed ? m_browse.homeError + "  -  " + tr("X to try again")
                                                           : tr("Loading...");
        ren.drawText(fit(m_font, msg, area.width - kPad * 2.f, 0.8f), {area.x + kPad, top + kHomeTop}, m_font,
                     textSub(a), 0.8f);
        return;
    }
    const int rows = tileRows();
    const int cols = m_l.tileCols;
    const float tileW = (area.width - kPad * 2.f - (cols - 1) * 8.f) / cols;
    for (int i = 0; i < (int)m_browse.tiles.size(); ++i) {
        const Item& e = m_browse.tiles[(std::size_t)i];
        const int r = i / cols, c = i % cols;
        const nxui::Rect tile{area.x + kPad + c * (tileW + 8.f), top + kHomeTop + r * kTileStep, tileW, kTileH};
        const bool focused = m_zone == Zone::Main && m_shelf == r - rows && m_col == c;
        ren.drawRoundedRect(tile, nxui::Color(1.f, 1.f, 1.f, (focused ? 0.2f : 0.1f) * a), 4.f);
        drawArt(ren, {tile.x, tile.y, kTileH, kTileH}, e, 4.f, a);
        const float tw = tileW - kTileH - (focused ? 56.f : 20.f);
        ren.drawText(fit(m_boldFont, e.name, tw, 0.62f), {tile.x + kTileH + 10.f, tile.y + 14.f}, m_boldFont,
                     textMain(a), 0.62f);
        if (focused)
            drawPlayButton(ren, tile.x + tile.width - 24.f, tile.y + kTileH * 0.5f, 16.f, false, playingHere(e.uri), a);
        m_hits.push_back({tile, [this, r, c, rows]() {
                              m_zone = Zone::Main;
                              m_shelf = r - rows;
                              m_col = c;
                              press();
                          }});
    }
    drawShelves(ren, area, top + shelvesTop(), list, a);
}

void SpotifyPanel::drawSearch(nxui::Renderer& ren, const nxui::Rect& area, float a) {
    using L = SpotifyBrowser::Load;
    const float top = area.y - m_scrollShown;
    const auto& list = m_browse.results;
    m_scrollMax = std::max(0.f, shelvesTop() + list.size() * m_l.shelfH + 12.f - area.height);
    std::string msg;
    if (m_browse.query.empty()) msg = tr("What do you want to play?");
    else if (m_browse.search == L::Loading) msg = tr("Loading...");
    else if (m_browse.search == L::Failed) msg = m_browse.searchError + "  -  " + tr("X to try again");
    else if (m_browse.search == L::Ready && list.empty()) msg = tr("No results");
    if (!msg.empty()) {
        ren.drawText(fit(m_font, msg, area.width - kPad * 2.f, 0.8f), {area.x + kPad, top + kSearchTop}, m_font,
                     textSub(a), 0.8f);
        return;
    }
    drawShelves(ren, area, top + kSearchTop, list, a);
}

// A song in a collection: number, cover, name and artist, album, duration.
void SpotifyPanel::drawRow(nxui::Renderer& ren, const nxui::Rect& row, int number, const Item& e, bool focused,
                           const std::string& playing, float a) {
    if (focused) ren.drawRoundedRect(row, bgHover(a), 4.f);
    const bool now = e.isTrack() && !playing.empty() && e.name == playing;
    if (focused) { // the number turns into a play sign
        const float cx = row.x + 22.f, cy = row.y + row.height * 0.5f;
        ren.drawTriangle({cx - 5.f, cy - 7.f}, {cx - 5.f, cy + 7.f}, {cx + 7.f, cy}, textMain(a));
    } else {
        const std::string n = std::to_string(number);
        ren.drawText(n, {row.x + 26.f - m_font->measure(n).x * 0.33f, row.y + 16.f}, m_font,
                     now ? accent(a) : textSub(a), 0.66f);
    }
    drawArt(ren, {row.x + 52.f, row.y + 8.f, 40.f, 40.f}, e, 4.f, a);
    const float nameX = row.x + 104.f;
    const float albumX = row.x + row.width * 0.56f, durX = row.x + row.width - 60.f;
    const float nameW = (m_l.albumColumn ? albumX : durX) - nameX - 16.f;
    ren.drawText(fit(m_font, e.name, nameW, 0.66f), {nameX, row.y + 8.f}, m_font, now ? accent(a) : textMain(a), 0.66f);
    ren.drawText(fit(m_smallFont, e.subtitle, nameW, 0.66f), {nameX, row.y + 31.f}, m_smallFont, textSub(a), 0.66f);
    if (m_l.albumColumn)
        ren.drawText(fit(m_smallFont, e.detail, durX - albumX - 16.f, 0.7f), {albumX, row.y + 19.f}, m_smallFont,
                     textSub(a), 0.7f);
    if (e.durationMs) ren.drawText(clock(e.durationMs), {durX, row.y + 19.f}, m_smallFont, textSub(a), 0.7f);
}

// A playlist or album: its colour, cover and big title, its buttons, its songs.
void SpotifyPanel::drawCollection(nxui::Renderer& ren, const nxui::Rect& area, float a) {
    using L = SpotifyBrowser::Load;
    const auto& c = m_browse.collection;
    const float top = area.y - m_scrollShown;
    const nxui::Color wash = m_current.kind == SpotifyBrowser::Kind::Liked ? rgb(80, 56, 160, a)
                                                                            : tint(m_current.name, a, 0.75f);
    const nxui::Color deep(wash.r * 0.55f, wash.g * 0.55f, wash.b * 0.55f, a);
    ren.drawGradientRect({area.x, top, area.width, m_l.headH}, wash, deep);
    ren.drawGradientRect({area.x, top + m_l.headH, area.width, 200.f}, deep.withAlpha(0.6f * a), bgPanel(0.f));

    // Full size: the cover, its title beside it. Discord's size: one under the other.
    const bool big = m_l.big;
    const nxui::Rect art{big ? area.x + kPad : area.x + kPad, top + (big ? 28.f : 20.f), m_l.headArt, m_l.headArt};
    drawArt(ren, art, m_current, m_current.kind == SpotifyBrowser::Kind::Artist ? m_l.headArt * 0.5f : 6.f, a);
    const float tx = big ? art.x + m_l.headArt + 24.f : art.x + m_l.headArt + 16.f;
    const float tw = area.x + area.width - kPad - tx;
    const float kindY = big ? art.y + 58.f : art.y + 20.f;
    ren.drawText(kindLabel(m_current.kind), {tx, kindY}, m_font, textMain(a), 0.62f);
    // The title as big as it fits (the web player's 72 px down to 32).
    float scale = m_l.big ? 2.0f : 1.1f;
    while (scale > 0.75f && m_titleFont->measure(m_current.name).x * scale > tw) scale -= 0.05f;
    ren.drawText(fit(m_titleFont, m_current.name, tw, scale), {tx, big ? art.y + 80.f : art.y + 44.f}, m_titleFont,
                 textMain(a), scale);
    std::string sub = m_current.subtitle;
    const int songs = (int)c.items.size();
    if (songs > 0) {
        std::string count = trf(songs == 1 ? tr("{n} song") : tr("{n} songs"), std::to_string(songs));
        if (c.load == L::Loading) count += "+";
        sub += (sub.empty() ? "" : "  \xE2\x80\xA2  ") + count;
    }
    ren.drawText(fit(m_boldFont, sub, tw, 0.62f), {tx, big ? art.y + 162.f : art.y + 92.f}, m_boldFont, textMain(a),
                 0.62f);

    // Its buttons: Play, Shuffle.
    const bool mainFocus = m_zone == Zone::Main;
    const float by = top + m_l.headH + 44.f;
    const float px = area.x + kPad + 28.f;
    drawPlayButton(ren, px, by, 28.f, mainFocus && m_row < 0 && m_action == 0, playingHere(m_current.uri), a);
    m_hits.push_back({{px - 28.f, by - 28.f, 56.f, 56.f}, [this]() {
                          m_zone = Zone::Main;
                          m_row = -1;
                          m_action = 0;
                          press();
                      }});
    const float sx = px + 66.f;
    const bool shuffle = m_snap.status.flags & PlayerStatusFlag_Shuffle;
    if (mainFocus && m_row < 0 && m_action == 1) ren.drawCircle({sx, by}, 22.f, bgHover(a), 32);
    drawIcon(ren, "shuffle", {sx - 16.f, by - 16.f, 32.f, 32.f}, shuffle ? accent(a) : textSub(a));
    if (shuffle) ren.drawCircle({sx, by + 19.f}, 2.f, accent(a), 10);
    m_hits.push_back({{sx - 22.f, by - 22.f, 44.f, 44.f}, [this]() {
                          m_zone = Zone::Main;
                          m_row = -1;
                          m_action = 1;
                          press();
                      }});

    // The songs.
    const float listX = area.x + kPad - 8.f, listW = area.width - kPad * 2.f + 16.f;
    const float hy = top + m_l.rowsTop - 34.f;
    ren.drawText("#", {listX + 20.f, hy}, m_smallFont, textSub(a), 0.7f);
    ren.drawText(tr("Title"), {listX + 104.f, hy}, m_smallFont, textSub(a), 0.7f);
    if (m_l.albumColumn) ren.drawText(tr("Album"), {listX + listW * 0.56f, hy}, m_smallFont, textSub(a), 0.7f);
    drawIcon(ren, "clock", {listX + listW - 58.f, hy, 18.f, 18.f}, textSub(a));
    ren.drawRect({listX, hy + 24.f, listW, 1.f}, bgHover(a));
    m_scrollMax = std::max(0.f, m_l.rowsTop + c.items.size() * kRowH + 40.f - area.height);

    const std::string playing = hasTrack() ? fixed(m_snap.status.title, sizeof(m_snap.status.title)) : std::string();
    for (std::size_t i = 0; i < c.items.size(); ++i) {
        const float ry = top + m_l.rowsTop + i * kRowH;
        if (ry + kRowH < area.y || ry > area.y + area.height) continue;
        const nxui::Rect row{listX, ry, listW, kRowH};
        drawRow(ren, row, (int)i + 1, c.items[i], mainFocus && m_row == (int)i, playing, a);
        nxui::Rect hit = row;
        hit.y = std::max(row.y, area.y);
        hit.height = std::min(row.y + row.height, area.y + area.height) - hit.y;
        if (hit.height > 0.f)
            m_hits.push_back({hit, [this, i]() {
                                  m_zone = Zone::Main;
                                  m_row = (int)i;
                                  press();
                              }});
    }
    const float endY = top + m_l.rowsTop + c.items.size() * kRowH + 8.f;
    if (c.load == L::Loading)
        ren.drawText(tr("Loading..."), {listX + 20.f, endY}, m_smallFont, textDim(a), 0.7f);
    else if (c.load == L::Failed)
        ren.drawText(fit(m_smallFont, c.error + "  -  " + tr("X to try again"), listW, 0.7f), {listX + 20.f, endY},
                     m_smallFont, rgb(240, 110, 110, a), 0.7f);
}

void SpotifyPanel::drawMain(nxui::Renderer& ren, const nxui::Rect& area, float a) {
    m_mainRect = area;
    ren.drawRoundedRect(area, bgPanel(a), 8.f);
    ren.pushClipRect(area);
    // The web player's soft colour at the top of Home and Search.
    if (m_view != View::Collection)
        ren.drawGradientRect({area.x, area.y, area.width, 240.f}, rgb(52, 44, 84, 0.9f * a), bgPanel(0.f));
    if (drawStatus(ren, area, a)) m_scrollMax = 0.f;
    else if (m_view == View::Home) drawHome(ren, area, a);
    else if (m_view == View::Search) drawSearch(ren, area, a);
    else drawCollection(ren, area, a);
    ren.popClipRect();
    if (!m_browse.playError.empty()) {
        const std::string e = fit(m_smallFont, m_browse.playError, area.width - kPad * 2.f, 0.7f);
        const float w = m_smallFont->measure(e).x * 0.7f + 24.f;
        const nxui::Rect toast{area.x + (area.width - w) * 0.5f, area.y + area.height - 48.f, w, 34.f};
        ren.drawRoundedRect(toast, rgb(233, 20, 41, 0.95f * a), 6.f);
        ren.drawText(e, {toast.x + 12.f, toast.y + 8.f}, m_smallFont, textMain(a), 0.7f);
    }
}

void SpotifyPanel::drawPlayButton(nxui::Renderer& ren, float cx, float cy, float rad, bool focused, bool playing,
                                  float a) {
    if (focused) ren.drawCircle({cx, cy}, rad + 4.f, textMain(a), 40);
    ren.drawCircle({cx, cy}, rad, accent(a), 40);
    const float s = rad * 1.05f;
    drawIcon(ren, playing ? "pause" : "play", {cx - s * 0.5f, cy - s * 0.5f, s, s}, bgBase(a));
}

// Spotify's own icons (white, tinted when drawn), read from the SD card once.
void SpotifyPanel::drawIcon(nxui::Renderer& ren, const char* name, const nxui::Rect& r, const nxui::Color& c) {
    auto it = m_icons.find(name);
    if (it == m_icons.end()) {
        auto tex = std::make_unique<nxui::Texture>();
        if (m_gpu && m_ren && !m_iconDir.empty()
            && !tex->loadFromFile(*m_gpu, *m_ren, m_iconDir + "sp_" + name + ".png", 96))
            tex.reset();
        it = m_icons.emplace(name, std::move(tex)).first;
    }
    if (it->second) ren.drawTexture(it->second.get(), r, c);
}

bool SpotifyPanel::playingHere(const std::string& uri) const {
    if (uri.empty() || uri != m_playingUri || !hasTrack()) return false;
    return m_pendingPlaying >= 0 ? m_pendingPlaying == 1 : m_snap.status.playback_state == PlayerPlaybackState_Playing;
}

void SpotifyPanel::drawBar(nxui::Renderer& ren, const nxui::Rect& r, float value, bool hot, float a) {
    const float h = hot ? 6.f : 4.f;
    const float cy = r.y + r.height * 0.5f;
    ren.drawRoundedRect({r.x, cy - h * 0.5f, r.width, h}, bgHover(a), h * 0.5f);
    const float w = r.width * std::clamp(value, 0.f, 1.f);
    if (w > 1.f) ren.drawRoundedRect({r.x, cy - h * 0.5f, w, h}, hot ? accent(a) : textMain(a), h * 0.5f);
    if (hot) ren.drawCircle({r.x + w, cy}, 7.f, textMain(a), 20);
}

void SpotifyPanel::drawControl(nxui::Renderer& ren, int control, float cx, float cy, float a) {
    const auto& s = m_snap.status;
    const bool focused = m_zone == Zone::Bar && m_focus == control;
    const float rad = control == Play ? 20.f : 18.f;
    m_controlRects[control] = {cx - rad, cy - rad, rad * 2.f, rad * 2.f};
    if (control == Play) ren.drawCircle({cx, cy}, rad, textMain(a), 36);
    else if (focused) ren.drawCircle({cx, cy}, rad, bgHover(a), 32);
    if (focused)
        ren.drawRoundedRectOutline({cx - rad - 4.f, cy - rad - 4.f, rad * 2.f + 8.f, rad * 2.f + 8.f}, accent(a),
                                   rad + 4.f, 2.f);
    auto icon = [&](const char* name, float size, const nxui::Color& c) {
        drawIcon(ren, name, {cx - size * 0.5f, cy - size * 0.5f, size, size}, c);
    };
    switch (control) {
    case Play: {
        const bool playing = m_pendingPlaying >= 0 ? m_pendingPlaying == 1
                                                   : s.playback_state == PlayerPlaybackState_Playing;
        icon(playing ? "pause" : "play", 22.f, bgBase(a));
        break;
    }
    case Previous: icon("prev", 22.f, textSub(a)); break;
    case Next: icon("next", 22.f, textSub(a)); break;
    case Shuffle: {
        const bool on = s.flags & PlayerStatusFlag_Shuffle;
        icon("shuffle", 24.f, on ? accent(a) : textSub(a));
        if (on) ren.drawCircle({cx, cy + 15.f}, 2.f, accent(a), 10);
        break;
    }
    case Repeat: {
        const bool one = s.flags & PlayerStatusFlag_RepeatOne;
        const bool on = one || (s.flags & PlayerStatusFlag_RepeatAll);
        icon("repeat", 24.f, on ? accent(a) : textSub(a));
        if (one) { // Spotify's little "1" on it
            ren.drawCircle({cx + 9.f, cy - 9.f}, 6.f, accent(a), 16);
            ren.drawText("1", {cx + 6.f, cy - 16.f}, m_smallFont, bgBase(a), 0.55f);
        }
        if (on) ren.drawCircle({cx, cy + 15.f}, 2.f, accent(a), 10);
        break;
    }
    default:
        break;
    }
}

void SpotifyPanel::drawPlayerBar(nxui::Renderer& ren, const nxui::Rect& area, float a) {
    ren.drawRect(area, bgBase(a));
    const auto& s = m_snap.status;
    const bool big = m_l.big;

    // Left: cover and song.
    const float artS = big ? 56.f : 48.f;
    const nxui::Rect art{area.x + 16.f, area.y + (big ? 16.f : 10.f), artS, artS};
    if (m_nowCover && m_nowCover->valid()) {
        ren.drawTextureRounded(m_nowCover.get(), art, 4.f, nxui::Color::white().withAlpha(a));
    } else {
        ren.drawRoundedRect(art, bgRaised(a), 4.f);
    }
    const float tx = art.x + artS + 12.f;
    const float lw = big ? 220.f : area.width - (tx - area.x) - 150.f;
    if (hasTrack()) {
        ren.drawText(fit(m_font, fixed(s.title, sizeof(s.title)), lw, 0.62f), {tx, art.y + 6.f}, m_font, textMain(a),
                     0.62f);
        ren.drawText(fit(m_smallFont, fixed(s.artist, sizeof(s.artist)), lw, 0.62f), {tx, art.y + 30.f}, m_smallFont,
                     textSub(a), 0.62f);
    } else {
        // Nothing yet: what the player is doing.
        using L = SpotifyPlayer::Link;
        std::string line = tr("Nothing playing");
        if (m_snap.link == L::Starting || m_snap.link == L::Off) line = tr("Starting Spotify...");
        else if (m_snap.link == L::NotRunning) line = tr("Spotify isn't running (X starts it)");
        else if (m_snap.link == L::Failed) line = tr("Spotify couldn't start (X: try again)");
        else if (m_snap.status.service_state == PlayerServiceState_Pairing)
            line = tr("Pick this Switch in Spotify on your phone");
        else if (m_snap.status.service_state == PlayerServiceState_WaitingForCredentials)
            line = tr("Spotify isn't signed in on this Switch");
        else if (!ready()) line = tr("Connecting...");
        ren.drawText(fit(m_smallFont, line, lw, 0.66f), {tx, art.y + 16.f}, m_smallFont, textSub(a), 0.66f);
    }

    const std::uint32_t pos = livePositionMs();
    const float value = s.duration_ms ? (float)pos / (float)s.duration_ms : 0.f;
    const std::string left = clock(pos), right = clock(s.duration_ms);
    if (!big) { // Discord's size: previous, play, next on the right; the time below, the whole width
        const float by = art.y + artS * 0.5f;
        for (int c = Previous; c <= Next; ++c)
            drawControl(ren, c, area.x + area.width - 40.f - (Next - c) * 46.f, by, a);
        const nxui::Rect seek{area.x + 56.f, area.y + area.height - 24.f, area.width - 112.f, 16.f};
        m_seekRect = {seek.x, seek.y - 8.f, seek.width, seek.height + 12.f};
        m_volumeRect = {};
        drawBar(ren, seek, value, m_dragging == 1, a);
        ren.drawText(left, {seek.x - 10.f - m_smallFont->measure(left).x * 0.58f, seek.y}, m_smallFont, textSub(a),
                     0.58f);
        ren.drawText(right, {seek.x + seek.width + 10.f, seek.y}, m_smallFont, textSub(a), 0.58f);
        return;
    }

    // Center: buttons above the seek bar.
    const float cx = area.x + area.width * 0.5f;
    const float by = area.y + 30.f;
    const float gap = 52.f;
    for (int c = 0; c < ControlCount; ++c) drawControl(ren, c, cx + (c - Play) * gap, by, a);

    const float seekW = 480.f;
    const nxui::Rect seek{cx - seekW * 0.5f, area.y + 62.f, seekW, 18.f};
    m_seekRect = {seek.x, seek.y - 8.f, seek.width, seek.height + 16.f};
    drawBar(ren, seek, value, m_dragging == 1, a);
    ren.drawText(left, {seek.x - 14.f - m_smallFont->measure(left).x * 0.6f, seek.y}, m_smallFont, textSub(a), 0.6f);
    ren.drawText(right, {seek.x + seek.width + 14.f, seek.y}, m_smallFont, textSub(a), 0.6f);

    // Right: volume.
    const int vol = (m_pendingVolume >= 0 && SpotifyPlayer::nowMs() < m_volumeHoldUntil) ? m_pendingVolume
                    : m_sysVolume >= 0                                                     ? m_sysVolume * 10
                                                                                           : (int)s.volume_permille;
    const nxui::Rect volume{area.x + area.width - 40.f - 130.f, area.y + 36.f, 130.f, 20.f};
    m_volumeRect = {volume.x, volume.y - 10.f, volume.width, volume.height + 20.f};
    drawIcon(ren, "volume", {volume.x - 32.f, volume.y, 20.f, 20.f}, textSub(a));
    drawBar(ren, volume, vol / 1000.f, m_dragging == 2, a);
}

void SpotifyPanel::onRender(nxui::Renderer& ren) {
    if (m_anim <= 0.001f || !m_font || !m_smallFont || !m_boldFont || !m_titleFont || !m_service) return;
    const float a = m_anim;
    m_hits.clear();
    for (auto& r : m_controlRects) r = {}; // (Discord's size draws only three: no stale ones)
    m_seekRect = m_volumeRect = {};
    ren.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, (m_l.big ? 0.45f : 0.35f) * a));
    const nxui::Rect p = panelRect();
    ren.drawRoundedRect(p, bgBase(a), 14.f);
    if (!m_l.big) { // Discord's size: its rail and list
        ren.drawRoundedRectOutline(p, bgRaised(a), 14.f, 1.f);
        ren.pushClipRect(p);
        drawCompact(ren, p, a);
        ren.popClipRect();
        drawPlayerBar(ren, {p.x, p.y + kPanelH - kBarH, m_l.panelW, kBarH - 6.f}, a);
        if (m_menu) drawAccountMenu(ren, a);
        return;
    }
    const float inner = 8.f;
    const float bodyY = p.y + kTopH, bodyH = kPanelH - kTopH - kBarH - inner;
    drawTopBar(ren, {p.x + inner, p.y, m_l.panelW - inner * 2.f, kTopH}, a);
    drawSidebar(ren, {p.x + inner, bodyY, m_l.sideW, bodyH}, a);
    drawMain(ren, {p.x + inner * 2.f + m_l.sideW, bodyY, m_l.panelW - m_l.sideW - inner * 3.f, bodyH}, a);
    drawPlayerBar(ren, {p.x, p.y + kPanelH - kBarH, m_l.panelW, kBarH - 6.f}, a);
    if (m_menu) drawAccountMenu(ren, a);
}

void SpotifyPanel::handleTouch(const nxui::Input& in) {
    m_input = &in;
    if (!m_active || !m_service) return;
    const float tx = in.touchX(), ty = in.touchY();
    auto setFromBar = [&](int which) {
        const nxui::Rect& r = which == 1 ? m_seekRect : m_volumeRect;
        const float v = std::clamp((tx - r.x) / std::max(1.f, r.width), 0.f, 1.f);
        if (which == 1 && m_snap.status.duration_ms) {
            m_pendingSeek = (int)(v * m_snap.status.duration_ms);
            m_seekHoldUntil = SpotifyPlayer::nowMs() + 1500;
        } else if (which == 2) {
            m_pendingVolume = (int)std::lround(v * 1000.f);
            m_volumeHoldUntil = SpotifyPlayer::nowMs() + 1500;
        }
    };
    if (in.touchDown()) {
        m_dragging = 0;
        m_touchMoved = false;
        m_touchOutside = !inside(panelRect(), tx, ty);
        if (m_touchOutside) { // a finger outside the panel closes it at once, like Discord's
            hide();
            return;
        }
        if (inside(m_seekRect, tx, ty) && hasTrack()) m_dragging = 1;
        else if (inside(m_volumeRect, tx, ty) && m_snap.link == SpotifyPlayer::Link::Connected) m_dragging = 2;
        if (m_dragging) setFromBar(m_dragging);
        return;
    }
    if (m_touchOutside) { // (the rest of a touch that closed the panel)
        if (in.touchUp()) m_touchOutside = false;
        return;
    }
    if (in.isTouching()) {
        if (m_dragging == 1 || m_dragging == 2) {
            setFromBar(m_dragging);
            return;
        }
        const float dx = in.touchDeltaX(), dy = ty - in.touchStartY();
        if (!m_touchMoved && std::fabs(dx) < 12.f && std::fabs(dy) < 12.f) return;
        if (!m_touchMoved) { // the finger moved: which way scrolls
            m_touchMoved = true;
            const float sx = in.touchStartX(), sy = in.touchStartY();
            m_dragShelf = -1;
            if ((m_view == View::Home || m_view == View::Search) && std::fabs(dx) > std::fabs(dy))
                for (std::size_t i = 0; i < m_shelfRows.size(); ++i)
                    if (inside(m_shelfRows[i], sx, sy) && i < m_shelfScroll.size()) m_dragShelf = (int)i;
            if (m_dragShelf >= 0) {
                m_dragging = 5;
                m_dragFrom = m_shelfScroll[(std::size_t)m_dragShelf];
            } else if (inside(m_mainRect, sx, sy)) {
                m_dragging = 3;
                m_dragFrom = m_scroll;
            } else if (inside(m_listRect, sx, sy)) {
                m_dragging = 4;
                m_dragFrom = m_sideScroll;
            }
        }
        if (m_dragging == 3) m_scroll = std::clamp(m_dragFrom - dy, 0.f, m_scrollMax);
        else if (m_dragging == 4) m_sideScroll = std::clamp(m_dragFrom - dy, 0.f, m_sideScrollMax);
        else if (m_dragging == 5 && m_dragShelf < (int)m_shelfScroll.size())
            m_shelfScroll[(std::size_t)m_dragShelf] =
                std::clamp(m_dragFrom - dx, 0.f, m_shelfScrollMax[(std::size_t)m_dragShelf]);
        return;
    }
    if (!in.touchUp()) return;
    const int dragged = m_dragging;
    m_dragging = 0;
    if (dragged == 1 || dragged == 2) { // the finger let go: send it
        setFromBar(dragged);
        if (dragged == 1 && m_pendingSeek >= 0) m_service->seek((std::uint32_t)m_pendingSeek);
        if (dragged == 2 && m_pendingVolume >= 0) {
            switchvolume::set(m_pendingVolume / 10);
            m_sysVolume = switchvolume::get();
        }
        return;
    }
    if (m_touchMoved) return; // a scroll, not a tap
    for (int c = 0; c < ControlCount; ++c)
        if (inside(m_controlRects[c], tx, ty)) {
            m_zone = Zone::Bar;
            m_focus = c;
            activate(c);
            return;
        }
    for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it)
        if (inside(it->rect, tx, ty)) {
            const auto tap = it->tap; // (it may rebuild the list)
            tap();
            return;
        }
}
