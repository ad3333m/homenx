#pragma once
#include "Audio.hpp"
#include "Library.hpp"
#include "Settings.hpp"

#include <nxui/Activity.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Texture.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cards {

// CardsNX: a HOME screen. A row of box-art cards over the chosen game's art, a
// status bar on top (menu, sort, you, the time, battery, free space, Nextendo
// players, notifications, power, settings) and a glass panel with the game's
// details and Play below. Three pages: Games, Homebrew (.nro apps and NRO
// forwarders) and Nintendo (eShop, Switch Online, Album, ...).
class CardsActivity final : public nxui::Activity {
public:
    bool onCreate() override;
    void onDestroy() override;
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer&) override {}
    nxui::Widget* focusRoot() override { return nullptr; } // reads the buttons itself

private:
    enum class Zone { Top, Cards, Bottom };
    enum Page { kGames, kHomebrew, kNintendo, kPageCount };
    struct Art {
        std::unique_ptr<nxui::Texture> card, bg;
        ArtKind kind = ArtKind::Banner, bgKind = ArtKind::Banner;
        bool cardAsked = false, bgAsked = false;
        float cardIn = -10.f; // when it arrived (it fades in)
    };
    struct Job {
        Game game;
        bool background = false;
    };
    struct Done {
        std::uint64_t id = 0;
        bool background = false;
        ArtKind kind = ArtKind::Banner;
        std::vector<std::uint8_t> rgba;
        int w = 0, h = 0;
    };
    // A popup row: an action (run), or a setting (value shown, left/right change it).
    struct MenuItem {
        std::string label;
        std::function<void()> run;
        std::function<std::string()> value;
        std::function<void(int)> change;
    };

    std::vector<Game>& games() { return m_pages[m_page]; }
    int& sel() { return m_sels[m_page]; }
    const Game* current();

    void worker(int index);
    void wantArt();
    void takeArt();
    void forgetAllArt();
    void select(int index);
    void switchPage(int dir);
    void sortGames(int sort);
    void play();
    void openMenu(std::string title, std::vector<MenuItem> items);
    void openSettings();
    void openPower();
    void openNotifications();
    void say(const std::string& message);
    void refreshStatus();
    void activateTop(int index);
    void activateBottom(int index);
    void press(int zone, int index);
    void keepTextCachesSmall();

    void draw(nxui::Renderer& r);
    void drawBackground(nxui::Renderer& r);
    void drawTopBar(nxui::Renderer& r, float a);
    void drawCards(nxui::Renderer& r);
    void drawCardExtras(nxui::Renderer& r); // after the blur: badges, the sheen
    void drawPanel(nxui::Renderer& r, float a);
    void drawPopup(nxui::Renderer& r);
    void glassCircle(nxui::Renderer& r, float cx, float cy, float d, bool focused, const char* icon, float iconSize,
                     float a, float pressed);
    void frost(nxui::Renderer& r, const nxui::Rect& rect, float radius, const nxui::Color& tint);
    void icon(nxui::Renderer& r, const char* name, float cx, float cy, float size, const nxui::Color& tint);
    bool cardRect(int index, nxui::Rect& card, float& chosen);
    const nxui::Texture* gameIconTexture();

    Settings m_settings;
    Audio m_audio;
    AccountUidLike m_user;

    nxui::Font m_fTitle, m_fPlay, m_fLabel, m_fPill, m_fBody, m_fBadge, m_fCount;
    std::map<std::string, std::unique_ptr<nxui::Texture>> m_icons;
    std::shared_ptr<nxui::Widget> m_painter;
    bool m_frosted = false;

    std::vector<Game> m_pages[kPageCount];
    int m_sels[kPageCount] = {0, 0, 0};
    int m_page = kGames;
    std::map<std::uint64_t, Art> m_art;
    float m_scroll = 0.f;  // the row's position, easing to sel()
    float m_repeat = 0.f;  // held left / right
    Zone m_zone = Zone::Cards;
    int m_topSel = 0, m_bottomSel = 0;

    // Motion: everything eases on m_time.
    float m_time = 0.f;
    float m_intro = 0.f;          // 0 -> 1 as it opens
    float m_pageIn = 1.f;         // 0 -> 1 after a page switch
    int m_pageDir = 1;
    float m_detailIn = 1.f;       // 0 -> 1 when the chosen game changes (its details fade in)
    float m_sheen = 1.f;          // 0 -> 1: light across the chosen card
    float m_menuIn = 0.f;         // the popup opening
    int m_pressZone = -1, m_pressIndex = -1;
    float m_pressT = 1.f;         // 0 -> 1 after a button is pressed

    // The background: the chosen game's art, faded in over the last one.
    std::uint64_t m_bgId = 0, m_bgPrevId = 0;
    float m_bgFade = 1.f;
    std::unique_ptr<nxui::Texture> m_gameIcon;
    std::uint64_t m_gameIconId = 0;

    // The status bar.
    std::unique_ptr<nxui::Texture> m_avatar;
    std::string m_clock, m_battery, m_free;
    bool m_online = false;
    float m_statusIn = 0.f;
    int m_unread = 0;
    std::map<std::uint64_t, int> m_players; // Nextendo, by title
    int m_playersTotal = -1;                 // -1: not known yet

    // Popups: a list (menu, settings, power), or the notifications.
    std::string m_menuTitle;
    std::vector<MenuItem> m_menu;
    int m_menuSel = 0;
    bool m_notifOpen = false;
    std::vector<std::pair<std::string, std::string>> m_notifRows; // title, detail
    std::string m_message;
    float m_messageFor = 0.f;

    // The workers: pictures (three at a time) and Nextendo's counts.
    std::vector<std::thread> m_threads;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<Job> m_jobs;
    std::vector<Done> m_done;
    std::map<std::uint64_t, int> m_playersIn;
    bool m_playersFresh = false;
    bool m_quit = false;
    std::string m_key;        // SteamGridDB
    bool m_netUp = false;
    std::atomic<bool> m_boxArt{true};
    float m_evictIn = 0.f;

    // Touch: drag the row, tap a card (again to play), tap any button.
    bool m_dragging = false, m_touchOnCards = false;
    float m_touchScroll = 0.f;
    bool handleTouch();
};

} // namespace cards
