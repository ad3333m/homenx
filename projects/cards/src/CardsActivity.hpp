#pragma once
#include "Library.hpp"

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

// HomeNX Cards: a test HOME screen. A row of box-art cards over the chosen
// game's art, a status bar on top (menu, sort, you, the time, battery, free
// space, Nextendo players, notifications, settings) and a glass panel with
// the game's details and Play below. Green dots: players online in Nextendo.
class CardsActivity final : public nxui::Activity {
public:
    bool onCreate() override;
    void onDestroy() override;
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& r) override;
    nxui::Widget* focusRoot() override { return nullptr; } // reads the buttons itself

private:
    enum class Zone { Top, Cards, Bottom };
    enum class Sort { LastPlayed, Title, MostPlayed };
    struct Art {
        std::unique_ptr<nxui::Texture> card, bg;
        bool tall = false;          // real box art (else the banner, cropped)
        bool cardAsked = false, bgAsked = false;
    };
    struct Job {
        Game game;
        bool background = false;
    };
    struct Done {
        std::uint64_t id = 0;
        bool background = false, tall = false;
        std::vector<std::uint8_t> rgba;
        int w = 0, h = 0;
    };
    struct MenuItem {
        std::string label;
        std::function<void()> run;
    };

    void worker();
    void wantArt();          // what the worker fetches next: around the selection first
    void takeArt();          // its results onto the GPU, a couple a frame
    void select(int index);
    void sortGames(Sort sort);
    void play();
    void openMenu(std::string title, std::vector<MenuItem> items);
    void openNotifications();
    void say(const std::string& message);
    void refreshStatus();
    void activateTop(int index);
    void activateBottom(int index);

    void draw(nxui::Renderer& r);
    void drawBackground(nxui::Renderer& r);
    void drawTopBar(nxui::Renderer& r);
    void drawCards(nxui::Renderer& r);
    void drawPanel(nxui::Renderer& r);
    void drawPopup(nxui::Renderer& r);
    void glassCircle(nxui::Renderer& r, float cx, float cy, float d, bool focused, const char* icon, float iconSize);
    void icon(nxui::Renderer& r, const char* name, float cx, float cy, float size, const nxui::Color& tint);
    const nxui::Texture* gameIconTexture();

    nxui::Font m_fTitle, m_fPlay, m_fLabel, m_fPill, m_fBody, m_fBadge;
    std::map<std::string, std::unique_ptr<nxui::Texture>> m_icons;
    std::shared_ptr<nxui::Widget> m_painter;

    std::vector<Game> m_games;
    std::map<std::uint64_t, Art> m_art;
    int m_sel = 0;
    float m_scroll = 0.f;      // the row's position, easing to m_sel
    float m_repeat = 0.f;      // held left / right
    Zone m_zone = Zone::Cards;
    int m_topSel = 0, m_bottomSel = 0;
    Sort m_sort = Sort::LastPlayed;

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

    // Popups: a list of choices, or the notifications.
    std::string m_menuTitle;
    std::vector<MenuItem> m_menu;
    int m_menuSel = 0;
    bool m_notifOpen = false;
    std::vector<std::pair<std::string, std::string>> m_notifRows; // title, detail
    std::string m_message;
    float m_messageFor = 0.f;

    // The worker: pictures and Nextendo's counts.
    std::thread m_thread;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<Job> m_jobs;
    std::vector<Done> m_done;
    std::map<std::uint64_t, int> m_playersIn;
    bool m_playersFresh = false;
    bool m_quit = false;
    std::string m_key; // SteamGridDB
};

} // namespace cards
