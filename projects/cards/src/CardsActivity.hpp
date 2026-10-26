#pragma once
#include "Audio.hpp"
#include "Library.hpp"
#include "Settings.hpp"

#include <nxui/Activity.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Texture.hpp>

#ifdef SWITCHU_MENU
#include "ReleaseUpdater.hpp"
#include "launcher/AppletLauncher.hpp"
#include "settings/TextEntryScreen.hpp"
#include "social/DiscordService.hpp"
#include "widgets/DiscordPanel.hpp"
#include "widgets/SocialScreen.hpp"
#include "settings/ControllerTestScreen.hpp"
#include <nxui/Theme.hpp>
#endif

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cards {

// CardsNX: a HOME screen. A row of box-art cards over the chosen game's art, a
// status bar on top (menu, Discord, sort, a pill with you, the time, battery,
// free space and Nextendo players, notifications, music, power, settings) and
// a glass panel with the game's details and Play below. Three pages: Games,
// Homebrew (.nro apps and NRO forwarders) and Nintendo (News or Switch Online,
// eShop, Album, ...).
//
// As an .nro it runs over hbl. Built into HomeNX's menu (SWITCHU_MENU) it is
// HOME itself, chosen in NXToggle: games, applets and power go through HomeNX's
// daemon, and Discord (L), News and updates come from HomeNX.
class CardsActivity final : public nxui::Activity {
public:
    // As HOME: what the daemon said at start (a game suspended, one running,
    // the console just booted).
    explicit CardsActivity(std::uint64_t suspendedId = 0, bool appRunning = false, bool atBoot = true);
    bool onCreate() override;
    void onDestroy() override;
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer&) override {}
    nxui::Widget* focusRoot() override; // HomeNX's screens when one is open; else it reads the buttons itself

private:
    enum class Zone { Top, Cards, Bottom };
    enum Page { kGames, kHomebrew, kNintendo, kMedia, kPageCount };
    // The top bar's buttons, left to right.
    enum Top { kTopMenu, kTopDiscord, kTopSort, kTopUser, kTopPlayers, kTopBell, kTopMusic, kTopPower, kTopGear,
               kTopCount };
    struct Art {
        std::unique_ptr<nxui::Texture> card, bg, icon;
        ArtKind kind = ArtKind::Banner, bgKind = ArtKind::Banner, iconKind = ArtKind::Icon;
        bool cardAsked = false, bgAsked = false, iconAsked = false;
        float cardIn = -10.f; // when it arrived (it fades in)
        float retryAt = 0.f;  // nothing came: asked again after this
    };
    enum What { kCard, kBackground, kIcon, kLaunch }; // kLaunch: the art at full size, for the launch
    // The lists, read on a thread (HOME shows last time's at once).
    struct Lists {
        std::vector<Game> games, homebrew;
        float ms = 0.f;         // how long reading them took
        bool gamesOnly = false; // the installed titles only: the .nro apps stay as they are
    };
    // Every game and .nro (slow: seconds), on whatever thread calls it; or only
    // the installed titles (a game installed, updated or removed).
    static Lists readAllLists(bool gamesOnly = false);
    struct Job {
        Game game;
        int what = kCard;
    };
    struct Done {
        std::uint64_t id = 0;
        int what = kCard;
        ArtKind kind = ArtKind::Banner;
        std::vector<std::uint8_t> rgba;
        int w = 0, h = 0;
    };
    // A row in a popup or in Settings.
    struct MenuItem {
        enum Kind { Action, Toggle, Choice, Slider, Info };
        Kind kind = Action;
        std::string label, detail;              // detail: a line under it (Settings)
        std::function<void()> run;              // Action
        std::function<std::string()> value;     // Choice / Info (and an Action's note)
        std::function<void(int)> change;        // Choice, Slider: -1 / +1; Toggle: flip
        std::function<float()> level;           // Toggle 0 / 1, Slider 0..1
        const nxui::Texture* picture = nullptr; // a user's picture
        bool danger = false;                    // red (Delete)
        bool stay = false;                      // an action that keeps the popup open
        const char* glyph = nullptr;            // its icon (Plus's options)
        float anim = -1.f;                      // a toggle's knob, easing
    };
    struct Section {
        std::string name;
        const char* icon;
        std::vector<MenuItem> rows;
    };

    static MenuItem action(std::string label, std::function<void()> run, bool danger = false);
    static MenuItem toggle(std::string label, std::function<bool()> get, std::function<void()> flip,
                           std::string detail = {});
    static MenuItem choice(std::string label, std::function<std::string()> value, std::function<void(int)> change,
                           std::string detail = {});
    static MenuItem slider(std::string label, std::function<int()> get, std::function<void(int)> set,
                           std::string detail = {});
    static MenuItem info(std::string label, std::function<std::string()> value);

    std::vector<Game>& games() { return m_pages[m_page]; }
    int& sel() { return m_sels[m_page]; }
    const Game* current();

    void worker(int index);
    void wantArt();
    void takeArt();
    void forgetAllArt();
    void select(int index);
    void switchPage(int dir);     // the next / previous page in your order
    void goToPage(int page, int dir = 0);
    int pagePos(int page) const;  // where it is in your order
    void sortGames(int sort, bool keepChoice = true);
    void selectById(std::uint64_t id); // its page and card, straight away
    std::uint64_t m_backTo = 0;        // back from this game: it stays chosen until you move
    void takeLists(Lists lists);
    // The Media page: your media apps off the Homebrew page, and the ones
    // GitHub has that aren't on the SD card yet ("Get").
    void splitMedia();
    std::vector<Game> homebrewToSave() const; // Homebrew and Media's .nro apps, for lists.json
    void getMedia(const Game& g);
    std::atomic<bool> m_updatesSaved{false}; // the last updates came from the saved list
    // Settings > Pictures > Save every picture now: every game's, on the SD card
    // for good (one taken down later stays), one game at a time on a thread.
    std::future<void> m_saveAllJob;
    std::atomic<int> m_saveAllDone{0}, m_saveAllTotal{0};
    std::atomic<bool> m_saveAllStop{false};
    void saveAllPictures();
    std::future<bool> m_getJob;
    std::string m_getPath, m_getName;
    void openPageOrder(int select);
    void startListRefresh(bool gamesOnly = false); // read the lists again on a thread (unless that's running)
    float m_gamesChangedIn = 0.f; // the installed titles changed: read again once they stop (an update sends many)
    bool glassCard(const Game& g); // an app or a game without box art: its icon on glass
    void applyOrder(int page);
    void saveOrder(int page);
    void reloadGames();

    // Starting things.
    void play();
    void launchGame(const Game& g);
    void openNro(const std::string& path);
    void openNintendo(const Game& g);
    void openDiscord();
    void openBrowser();
    std::string findNro(const char* nameHas);
    // The launch: the card grows to fill the screen, the rest falls away, then
    // black; `then` runs at the end (index -1: no card, only the fade).
    void leave(int index, float seconds, std::function<void()> then);
    enum LaunchStyle { kFade, kSplash, kZoom, kFlip, kDive, kNoAnimation, kLaunchStyles };
    int launchStyle() const;
    bool launchDrawsCard() const; // the chosen card is drawn by drawLaunch (zoom, flip)
    float othersAlpha() const;    // the rest of the row while launching
    void comeBack(); // it didn't happen: the screen as it was
    void handleHome();
    bool closeEverything();

    // Popups and screens.
    void openMenu(std::string title, std::vector<MenuItem> items);
    void openOptions();
    // Plus: the card's art on the left, its name and details, big buttons.
    bool m_optOpen = false;
    int m_optSel = 0;
    float m_optIn = 0.f;
    Game m_optGame;
    std::vector<MenuItem> m_optItems;
    void updateOptions();
    bool optionsTouch();
    void drawOptions(nxui::Renderer& r);
    void openSettings(int section = 0);
    void buildSettings();
    void openPower();
    // The three dots: a menu dropping from its button, the pages on top and
    // tiles for everything else.
    struct DotsTile {
        const char* name;
        const char* glyph;
        nxui::Color tone;
        std::function<void()> run;
    };
    bool m_dotsOpen = false;
    int m_dotsRow = 1, m_dotsCol = 0;
    float m_dotsIn = 0.f;
    std::vector<DotsTile> m_dotsTiles;
    void openDots();
    void updateDots();
    bool dotsTouch();
    void drawDots(nxui::Renderer& r);
    void drawPower(nxui::Renderer& r);
    void updatePower();
    bool powerTouch();
    // Power: four big glass tiles (Sleep, Restart, Power Off, Hekate).
    bool m_powerOpen = false;
    int m_powerSel = 0;
    float m_powerIn = 0.f, m_powerLift[4] = {0.f, 0.f, 0.f, 0.f};
    std::vector<MenuItem> m_powerItems;
    std::vector<MenuItem> powerItems(); // Sleep, Restart, Shut down, Reboot to Hekate
    void openMusic();
    void openQuick();                   // R: Quick Settings, a glass panel on the right
    void drawQuick(nxui::Renderer& r);
    void updateQuick();
    bool quickTouch();
    void quickActivate(int row, int col);   // A on a button
    void quickChange(int row, int delta);   // left / right on a slider
    // Up / down, left / right, A and B on a list of rows; close() shuts it.
    void listInput(std::vector<MenuItem>& items, int& sel, const std::function<void()>& close);
    void openUsers();
    void openNotifications();
    void startMove();
    void finishMove(bool keep);
    void say(const std::string& message);
    void activateTop(int index);
    void activateBottom(int index);
    void press(int zone, int index);
    void keepTextCachesSmall();
    void setUser(AccountUidLike uid);
    void loadAvatar();
    std::string updateRowLabel();
    void runUpdate();

    void draw(nxui::Renderer& r);
    void drawBackground(nxui::Renderer& r);
    void drawTopBar(nxui::Renderer& r, float a);
    void drawCards(nxui::Renderer& r);
    void drawCardExtras(nxui::Renderer& r); // after the blur: glass cards, badges, the sheen
    void drawLaunch(nxui::Renderer& r);
    void drawPanel(nxui::Renderer& r, float a);
    void drawPopup(nxui::Renderer& r);
    void drawSheet(nxui::Renderer& r);
    void drawRow(nxui::Renderer& r, MenuItem& item, const nxui::Rect& row, bool chosen, float a, bool wide);
    void glassCircle(nxui::Renderer& r, float cx, float cy, float d, bool focused, const char* icon, float iconSize,
                     float a, float pressed);
    void frost(nxui::Renderer& r, const nxui::Rect& rect, float radius, const nxui::Color& tint);
    void icon(nxui::Renderer& r, const char* name, float cx, float cy, float size, const nxui::Color& tint);
    bool cardRect(int index, nxui::Rect& card, float& chosen);
    const nxui::Texture* gameIconTexture();
    nxui::Color accent(float a = 1.f) const;
    nxui::Color glassTint(float a = 1.f) const;
    bool isRunning(std::uint64_t id) const;
    std::uint64_t runningId() const;

    bool updateSheet(float dt);
    bool updatePopup();
    bool handleTouch();

    Settings m_settings;
    Audio m_audio;
    AccountUidLike m_user;

    nxui::Font m_fTitle, m_fPlay, m_fLabel, m_fPill, m_fBody, m_fBadge, m_fCount;
    std::map<std::string, std::unique_ptr<nxui::Texture>> m_icons;
    std::shared_ptr<nxui::Widget> m_painter;
    bool m_frosted = false;

    std::vector<Game> m_pages[kPageCount];
    int m_sels[kPageCount] = {0, 0, 0, 0};
    int m_page = kGames;
    std::map<std::uint64_t, Art> m_art;
    float m_scroll = 0.f;  // the row's position, easing to sel()
    float m_repeat = 0.f;  // held left / right
    Zone m_zone = Zone::Cards;
    int m_topSel = 0, m_bottomSel = 0;

    // Motion: everything eases on m_time.
    float m_time = 0.f;
    float m_intro = 0.f;          // 0 -> 1 as it opens
    float m_introFor = 1.1f;      // seconds (back from a game: quicker)
    float m_retryIn = 5.f;        // pictures that didn't come are asked again
    std::future<Lists> m_listJob;
    float m_pageIn = 1.f;         // 0 -> 1 after a page switch
    int m_pageDir = 1;
    float m_detailIn = 1.f;       // 0 -> 1 when the chosen game changes (its details fade in)
    float m_sheen = 1.f;          // 0 -> 1: light across the chosen card
    float m_menuIn = 0.f;         // the popup opening
    int m_pressZone = -1, m_pressIndex = -1;
    float m_pressT = 1.f;         // 0 -> 1 after a button is pressed

    // The launch (see leave()).
    bool m_leaving = false;
    float m_leaveT = 0.f, m_leaveFor = 0.9f;
    int m_leaveIndex = -1;
    std::uint64_t m_leaveId = 0;
    nxui::Rect m_leaveFrom{};
    std::function<void()> m_leaveThen;
    bool m_left = false; // `then` ran: hold the black frame until the app closes
    std::unique_ptr<nxui::Texture> m_launchTex; // the game's art at full size
    std::uint64_t m_launchTexId = 0;
    float m_launchTexIn = 0.f;

    // Plus > Move: the chosen card moves with left / right.
    bool m_moving = false;
    std::vector<Game> m_moveBefore;
    int m_moveBeforeSel = 0;

    // The background: the chosen game's art, faded in over the last one.
    std::uint64_t m_bgId = 0, m_bgPrevId = 0;
    float m_bgFade = 1.f;

    // The status bar (numbers from worker 1: the SD card and network calls
    // took the screen's time once a second).
    std::unique_ptr<nxui::Texture> m_avatar;
    std::string m_clock, m_battery, m_free;
    bool m_online = false;
    struct Status {
        std::string clock, battery, free;
        bool online = false;
    };
    Status m_statusIn;
    bool m_statusFresh = false;
    int m_unread = 0;
    std::map<std::uint64_t, int> m_players; // Nextendo, by title
    int m_playersTotal = -1;                 // -1: not known yet
    nxui::Rect m_userHit{}, m_playersHit{};  // in the pill, for touch
    std::future<bool> m_browserJob;          // BrowseNX downloading

    // Homebrew updates, found as the Homebrew Manager finds them (GitHub
    // releases, the HB App Store) and remembered for a few hours in
    // sdmc:/config/CardsNX/updates.json. By .nro path; only newer versions.
    struct NroUpdate {
        std::string latest, source, url, zipEntry;
        bool zip = false;
    };
    std::map<std::string, NroUpdate> m_updates;
    std::future<std::map<std::string, NroUpdate>> m_updatesJob;
    std::future<std::string> m_nroUpdateJob;   // one being installed: "" done, else what went wrong
    std::string m_nroUpdating;                 // its path
    std::vector<std::string> m_nroQueue;       // Update all: the ones after it
    std::atomic<float> m_nroProgress{0.f};
    bool m_updatesChecked = false;
    void checkHomebrewUpdates(bool force);
    void updateNro(const std::string& path);
    const NroUpdate* nroUpdate(const Game& g) const;
    void openHomebrewUpdates();
    std::future<std::uint32_t> m_deleteJob;  // an NRO forwarder being uninstalled (its Result)
    std::uint64_t m_deleteId = 0;
    std::string m_deleteName;

    // Popups: a list (menu, power, music, users, a game's options), or the notifications.
    std::string m_menuTitle;
    std::vector<MenuItem> m_menu;
    int m_menuSel = 0;
    bool m_notifOpen = false;
    std::vector<std::pair<std::string, std::string>> m_notifRows; // title, detail
    std::string m_message;
    float m_messageFor = 0.f;
    std::vector<std::unique_ptr<nxui::Texture>> m_userPictures;

    // Quick Settings (R), laid out as HomeNX's: the status, the music player,
    // three sliders, two switches, the power buttons; the playlist beside it.
    enum QuickRow { kQPlayer, kQMusic, kQSounds, kQBrightness, kQAirplane, kQWifi, kQPower, kQRows };
    bool m_quickOpen = false, m_quickPlaylist = false;
    float m_quickIn = 0.f, m_quickRefreshIn = 0.f, m_quickListIn = 0.f;
    int m_quickRow = kQPlayer, m_quickCol = 1, m_quickListSel = 0;
    std::vector<MenuItem> m_quickPower;  // Sleep, Restart, Shut down, Reboot to Hekate
    QuickStatus m_quick;
    // Settings > Internet, Controllers, Console.
    ConsoleSettings m_console;
    std::shared_ptr<NetTest> m_netTest;
    std::future<void> m_netTestJob, m_findJob;
    void startConnectionTest();
    std::string netTestText(int row); // 0 the test, 1 ping, 2 download, 3 upload, 4 signal
    void findControllersNow();


    // Settings: sections on the left, their rows on the right.
    std::vector<Section> m_sheet;
    bool m_sheetOpen = false, m_sheetOnRows = false;
    int m_sheetSec = 0, m_sheetRow = 0;
    float m_sheetIn = 0.f, m_sheetRowsIn = 1.f, m_sheetScroll = 0.f;

    // The workers: pictures (three at a time), Nextendo's counts, the status bar.
    std::vector<std::thread> m_threads;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<Job> m_jobs;
    std::vector<Done> m_done;
    std::map<std::uint64_t, int> m_playersIn;
    bool m_playersFresh = false;
    bool m_quit = false;
    std::string m_key;        // SteamGridDB
    std::atomic<bool> m_netUp{false};    // worker 0 starts the network
    std::atomic<bool> m_netReady{false}; // ...the other workers wait for that first
    int m_startStage = 0;                // what the first frame doesn't need starts after it

    // The performance log (sdmc:/config/CardsNX/perf.log): how long starting
    // took, slow frames and what they were doing, pictures that took long.
    void perfLine(const std::string& line);
    float sinceStartMs() const;
    std::string m_perfOut; // waiting for worker 1 to write it (under m_mu)
    std::vector<std::array<std::string, 3>> m_notifyOut; // notifications for worker 1 to save (under m_mu)
    std::uint64_t m_perfCreateTick = 0, m_perfLastTick = 0;
    float m_perfPumpMs = 0.f, m_perfArtMs = 0.f, m_perfDiscordMs = 0.f, m_perfUpdateMs = 0.f, m_perfDrawMs = 0.f;
    int m_perfUploads = 0, m_perfFrames = 0, m_perfSlow = 0, m_perfVerySlow = 0;
    // The longest step of the last update (perfMark between steps), and the
    // GPU waits and picture frees nxui counted.
    std::uint64_t m_perfMarkTick = 0;
    float m_perfMarkMs = 0.f;
    const char* m_perfMarkName = "";
    void perfMark(const char* step);
    // Every 50 ms: when the screen's thread stays in one step for long, the log
    // says which; when this thread itself didn't run for a second, the whole
    // process was stopped (the system paused HOME), not CardsNX.
    void watchdog();
    float m_perfWindow = 0.f, m_perfWorst = 0.f;
    bool m_perfFirstCard = false;
    std::atomic<bool> m_boxArt{true};
    std::atomic<bool> m_titleArt{true}; // Settings > Look > Title pictures
    std::atomic<bool> m_clock24{true};
    std::atomic<bool> m_downloading{false}; // BrowseNX
    float m_evictIn = 0.f;
    bool m_evictNow = false; // a picture had no room: far ones go now

    // Touch: drag the row, tap a card (again to play), tap any button.
    bool m_dragging = false, m_touchOnCards = false;
    float m_touchScroll = 0.f;

#ifdef SWITCHU_MENU
    // As HOME.
    std::uint64_t m_startSuspended = 0;
    bool m_startRunning = false;
    bool m_atBoot = true;
    AppletLauncher m_launcher;
    std::uint64_t m_pendingLaunch = 0; // starts once the open game has closed
    bool m_asleep = false;
    void pumpDaemon();
    std::uint64_t hostTitleId() const;
    AccountUid launchUid() const;

    // HomeNX's Discord and its keyboard, in HOME's font (loaded when first needed).
    nxui::Theme m_theme = nxui::Theme::dark();
    nxui::Font m_hxFont, m_hxSmall;
    bool m_hxFonts = false;
    void ensureHxFonts();
    DiscordService m_discord;
    std::shared_ptr<DiscordPanel> m_discordPanel;
    std::shared_ptr<TextEntryScreen> m_textEntry;
    std::shared_ptr<SocialScreen> m_social;   // Friends, your profile, Mii Maker (HomeNX's, not Nintendo's)
    void ensureTextEntry();
    void enterText(const std::string& title, const std::string& guide, const std::string& initial, int maxLength,
                   std::function<void(const std::string&)> done, nxui::Widget* back);
    void openSocial(bool miis);
    std::shared_ptr<ControllerTestScreen> m_padTest; // Settings > Controllers: HomeNX's test screen
    void openPadTest();
    bool m_pairing = false;                          // HomeNX's own pairing, 30 s
    std::uint8_t m_pairedBefore = 0;
    float m_pairingLeft = 0.f, m_pairingPoll = 0.f;
    void startPairing();
    void stopPairing();
    void updatePairing(float dt);
    float m_discordStartIn = 4.f;
    bool m_discordStarted = false;

    // News, in CardsNX's glass: the list on the left, the article on the right.
    struct NewsItem {
        std::string id, title, topic, body, footer;
        bool unread = false;
        std::shared_ptr<std::vector<std::uint8_t>> picture; // JPEG
        std::vector<std::string> titleLines, lines;         // wrapped (when first shown)
    };
    void openNews();
    void showNewsItem(int index);
    bool updateNews(float dt);
    void drawNews(nxui::Renderer& r);
    std::future<void> m_newsJob;
    std::vector<NewsItem> m_newsReady, m_newsItems;
    std::string m_newsEmpty;
    bool m_newsWanted = false, m_newsOpen = false;
    int m_newsSel = 0;
    float m_newsIn = 0.f, m_newsListScroll = 0.f, m_newsScroll = 0.f, m_newsTouchY = 0.f, m_newsTouchScroll = 0.f;
    bool m_newsDragging = false;
    std::map<int, std::unique_ptr<nxui::Texture>> m_newsPictures; // a few, by index
    float m_newsLoadedAt = -1000.f;

    // Settings > Updates: HomeNX's release, installed from here.
    enum class Update { Idle, Checking, UpToDate, Available, Installing, Installed, Failed };
    std::atomic<int> m_update{static_cast<int>(Update::Idle)};
    std::future<void> m_updateJob;
    switchu::manager::ReleaseInfo m_release;
    std::string m_updateError;
    std::atomic<float> m_updateDownload{0.f}, m_updateInstall{0.f};
    std::atomic<int> m_updateStage{0};
#endif
};

} // namespace cards
