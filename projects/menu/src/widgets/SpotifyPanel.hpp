#pragma once
#include "spotify/SpotifyBrowser.hpp"
#include "spotify/SpotifyPlayer.hpp"

#include <nxui/core/Font.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/widgets/Widget.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nxui { class GpuDevice; class Input; class Renderer; }

// HomeNX Spotify panel, laid out like Spotify's web player (open.spotify.com):
// a top bar (Home and the search field), Your Library on the left (filters and
// your playlists, albums and artists), the main area (Home's grid and shelves,
// search results, or a playlist or album with its songs) and the player bar. It slides in from the left over HOME, like
// Discord's. HOME never plays the audio: Spotify's background player on this
// Switch does (SpotifyPlayer), and SpotifyBrowser reads what to show. Closing
// the panel (or leaving HOME) leaves the music playing.
class SpotifyPanel : public nxui::Widget {
public:
    struct Callbacks {
        std::function<void()> onClosed;
        std::function<void()> onNavigate; // sounds
        std::function<void()> onActivate;
        std::function<bool()> canBrowse;  // signed in to Spotify with the library's rights
        std::function<void()> onSignIn;   // (the panel closes first)
        std::function<void()> onSignOut;  // the account menu: out of this account
        std::function<void()> onSwitchAccount; // ... out, then the QR code for another (the panel closes first)
        // HomeNX's keyboard: `done` gets the text.
        std::function<void(const std::string& initial, std::function<void(const std::string&)> done)> onAskText;
        std::function<void(bool big)> onSizeChanged; // + or - (HOME remembers it)
    };

    // A line of the list at Discord's size.
    struct Row {
        enum Kind { Header, Entry, PlayAll, SearchField } kind = Entry;
        std::string title;
        SpotifyBrowser::Item item;
    };

    SpotifyPanel();
    void setFont(nxui::Font* f) { m_font = f; }
    void setSmallFont(nxui::Font* f) { m_smallFont = f; }
    void setBoldFont(nxui::Font* f) { m_boldFont = f; }   // names and headings
    void setTitleFont(nxui::Font* f) { m_titleFont = f; } // big titles
    void setGpu(nxui::GpuDevice* gpu, nxui::Renderer* ren) { m_gpu = gpu; m_ren = ren; }
    void setService(SpotifyPlayer* s) { m_service = s; }
    void setBrowser(SpotifyBrowser* b) { m_browser = b; }
    void setLogoPath(std::string path) { m_logoPath = std::move(path); }
    void setIconDir(std::string dir) { m_iconDir = std::move(dir); } // sp_play.png, sp_pause.png...
    void setBig(bool big);  // full size, else Discord's
    void setCallbacks(Callbacks cb) { m_cb = std::move(cb); }

    void show();
    void hide();
    bool isActive() const { return m_active || m_anim > 0.001f; }
    bool isOpen() const { return m_active; }
    void handleTouch(const nxui::Input& in);

protected:
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& ren) override;

private:
    enum Control { Shuffle, Previous, Play, Next, Repeat, ControlCount };
    enum class Zone { Top, Side, Main, Bar };
    enum class View { Home, Search, Collection };
    using Item = SpotifyBrowser::Item;
    using Shelf = SpotifyBrowser::Shelf;
    static constexpr int kChips = 3;  // Your Library's filters: playlists, albums, artists

    // Sizes for each of the two sizes (Discord's, or full).
    struct Layout {
        bool big = false;
        float panelW = 460.f, sideW = 72.f;
        int tileCols = 2;
        float cardW = 150.f, cardArt = 126.f, cardH = 220.f, shelfH = 272.f;
        float headArt = 128.f, headH = 196.f, rowsTop = 330.f;
        bool albumColumn = false;
    };
    Layout m_l;

    // Covers by URL: downloaded on the image workers, a few at a time, at most
    // kCoverMax kept (the least recently drawn go first), all freed on close.
    struct Cover {
        std::unique_ptr<nxui::Texture> tex;
        bool loading = false;
        std::uint64_t usedFrame = 0;
    };
    const nxui::Texture* cover(const std::string& url);
    void pumpCovers();
    void dropCovers();

    void bindInput();
    void move(nxui::FocusDirection dir);
    void press();
    void back();
    void navigated();
    void setView(View v);
    void openCollection(const Item& e, bool remember);
    void openItem(const Item& e);
    void askSearch();
    void activate(int control);
    void seekBy(int deltaMs);
    void volumeBy(int deltaPermille);
    std::uint32_t livePositionMs() const;
    bool hasTrack() const;
    bool ready() const;     // the player answers and is signed in
    bool canBrowse() const; // and HOME may read your Spotify
    std::vector<int> libraryRows() const;  // Your Library's items, through its filter
    const std::vector<Shelf>& shelves() const { return m_view == View::Search ? m_browse.results : m_browse.shelves; }
    int tileRows() const {
        return m_view == View::Home ? ((int)m_browse.tiles.size() + m_l.tileCols - 1) / m_l.tileCols : 0;
    }
    int firstControl() const { return m_l.big ? Shuffle : Previous; } // (small: no shuffle and repeat)
    int lastControl() const { return m_l.big ? Repeat : Next; }
    bool playingHere(const std::string& uri) const; // what plays came from there, and it plays
    void drawIcon(nxui::Renderer& ren, const char* name, const nxui::Rect& r, const nxui::Color& c);
    float shelvesTop() const;  // where the shelves start in the main area
    nxui::Rect panelRect() const;
    void loadNowCover();
    void keepVisible();

    void drawArt(nxui::Renderer& ren, const nxui::Rect& r, const Item& item, float radius, float a);
    void drawTopBar(nxui::Renderer& ren, const nxui::Rect& area, float a);

    // Discord's size works like Discord's panel: a rail down the left (Home,
    // Search, then Your Library), the list of what's chosen beside it.
    std::vector<Row> compactRows() const;
    int railCount() const { return 2 + (int)m_browse.saved.size(); } // Home, Search, then the library
    void compactMove(nxui::FocusDirection dir);
    void compactPress();
    void compactEnterRail(); // what the rail's selection shows, in the list
    void drawCompact(nxui::Renderer& ren, const nxui::Rect& p, float a);
    void drawSidebar(nxui::Renderer& ren, const nxui::Rect& area, float a);
    void drawMain(nxui::Renderer& ren, const nxui::Rect& area, float a);
    bool drawStatus(nxui::Renderer& ren, const nxui::Rect& area, float a); // true: nothing else to show
    void drawShelves(nxui::Renderer& ren, const nxui::Rect& area, float top, const std::vector<Shelf>& list, float a);
    void drawHome(nxui::Renderer& ren, const nxui::Rect& area, float a);
    void drawSearch(nxui::Renderer& ren, const nxui::Rect& area, float a);
    void drawCollection(nxui::Renderer& ren, const nxui::Rect& area, float a);
    void drawRow(nxui::Renderer& ren, const nxui::Rect& row, int number, const Item& e, bool focused,
                 const std::string& playing, float a);
    void drawPlayerBar(nxui::Renderer& ren, const nxui::Rect& area, float a);
    void drawControl(nxui::Renderer& ren, int control, float cx, float cy, float a);
    void drawBar(nxui::Renderer& ren, const nxui::Rect& r, float value, bool hot, float a);
    void drawPlayButton(nxui::Renderer& ren, float cx, float cy, float rad, bool focused, bool playing, float a);

    SpotifyPlayer* m_service = nullptr;
    SpotifyBrowser* m_browser = nullptr;
    nxui::Font* m_font = nullptr;
    nxui::Font* m_smallFont = nullptr;
    nxui::Font* m_boldFont = nullptr;
    nxui::Font* m_titleFont = nullptr;
    nxui::GpuDevice* m_gpu = nullptr;
    nxui::Renderer* m_ren = nullptr;
    Callbacks m_cb;

    bool m_active = false;
    float m_anim = 0.f;
    std::uint64_t m_frame = 0;
    std::uint64_t m_textTrimAt = 0; // its fonts' text caches: trimmed when full (HOME only trims its own)
    void trimText(bool all);

    // Where the focus is.
    Zone m_zone = Zone::Main;
    View m_view = View::Home;
    View m_from = View::Home;  // where a collection was opened from (B goes back there)
    int m_focus = Play;        // player bar button
    int m_top = 0;             // the top bar: 0 Home, 1 the search field, 2 the account
    nxui::Vec2 m_accountPos{};  // the account button's centre
    bool m_menu = false;       // the account menu is open
    int m_menuSel = 0;         // 0 Sign out, 1 Switch account
    void drawAccount(nxui::Renderer& ren, const nxui::Vec2& c, bool focused, float a);
    void drawAccountMenu(nxui::Renderer& ren, float a);
    void menuPress();
    int m_filter = -1;         // Your Library's filter (a chip), -1 none
    int m_chip = 0;            // the focused chip
    int m_side = 0;            // Your Library: -1 its chips, then its rows (filtered)
    std::string m_openUri;     // the collection shown in the main area
    int m_shelf = 0, m_col = 0; // Home and Search; Home's grid rows first (negative)
    int m_row = -1;            // a collection: -1 its buttons (m_action), then its songs
    int m_action = 0;          // 0 Play, 1 Shuffle
    // Discord's size: the rail or the list, and where in each.
    int m_column = 0;          // 0 the rail, 1 the list
    int m_rail = 0;            // 0 Home, 1 Search, then Your Library
    int m_listRow = 0;         // (the list scrolls with m_scroll, the rail with m_sideScroll)
    std::vector<Item> m_history; // collections opened from a collection, for B
    Item m_current;            // the open collection

    // Scrolling (target and drawn).
    float m_scroll = 0.f, m_scrollShown = 0.f;
    float m_sideScroll = 0.f, m_sideScrollShown = 0.f;
    std::vector<float> m_shelfScroll, m_shelfScrollShown;
    float m_scrollMax = 0.f, m_sideScrollMax = 0.f; // from the last frame's content
    std::vector<float> m_shelfScrollMax;

    SpotifyPlayer::Snapshot m_snap;
    SpotifyBrowser::State m_browse;
    std::uint64_t m_browseVersion = ~0ull;
    // Button feedback before the next status arrives.
    int m_pendingPlaying = -1;           // -1 none, 0 paused, 1 playing
    std::uint64_t m_pendingUntil = 0;
    int m_pendingVolume = -1;            // permille shown while L/R or a drag set it
    int m_sysVolume = -1;                // the console's volume (0..100), read every so often
    std::uint64_t m_sysVolumeAt = 0;
    bool m_fullSent = false;             // the player was put at full volume (the console's is the volume)
    std::uint64_t m_volumeHoldUntil = 0;
    int m_pendingSeek = -1;              // ms
    std::uint64_t m_seekHoldUntil = 0;

    std::unordered_map<std::string, Cover> m_covers;
    std::vector<std::string> m_coverQueue;
    int m_coversLoading = 0;
    std::uint64_t m_coverEpoch = 0;  // bumped on close: late downloads are dropped
    // The current song's cover (from the player's file).
    std::unique_ptr<nxui::Texture> m_nowCover;
    std::string m_nowCoverUrl;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::string m_logoPath, m_iconDir;
    std::unordered_map<std::string, std::unique_ptr<nxui::Texture>> m_icons; // freed on close
    std::string m_playingUri;  // the collection (or album) the last Play here came from
    std::unique_ptr<nxui::Texture> m_logo;  // white, drawn green

    // Touch: targets from the last frame, and a drag in progress.
    struct Hit {
        nxui::Rect rect;
        std::function<void()> tap;
    };
    std::vector<Hit> m_hits;
    nxui::Rect m_controlRects[ControlCount]{};
    nxui::Rect m_seekRect{}, m_volumeRect{}, m_mainRect{}, m_sideRect{}, m_listRect{};
    std::vector<nxui::Rect> m_shelfRows;  // each shelf's row of cards, for sideways drags
    int m_dragging = 0;   // 1 seek, 2 volume, 3 main scroll, 4 side scroll, 5 a shelf
    int m_dragShelf = -1;
    float m_dragFrom = 0.f;
    bool m_touchMoved = false;
    bool m_touchOutside = false; // the finger came down outside the panel (and closed it)
    const nxui::Input* m_input = nullptr; // (HOME's, from handleTouch: tells a real A from a tap)
};
