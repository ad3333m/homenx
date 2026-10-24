#pragma once
#include "Catalog.hpp"

#include <nxui/Activity.hpp>
#include <nxui/Theme.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Texture.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace updater {

// HomeNX Updater: every .nro on the SD card with its installed and newest
// version, updated one at a time (A) or all at once (X).
class UpdaterActivity final : public nxui::Activity {
public:
    bool onCreate() override;
    void onDestroy() override;
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& r) override;
    nxui::Widget* focusRoot() override { return nullptr; } // the list reads the buttons itself

private:
    enum class Row { Checking, UpToDate, Update, NoSource, Updating, Updated, Failed };
    struct Item {
        App app;
        Row row = Row::Checking;
        std::string error;
        std::unique_ptr<nxui::Texture> icon;
    };

    void startCheck();
    void startUpdate(std::vector<int> which);
    void move(int delta);
    void draw(nxui::Renderer& r);
    // The selected app's menu (A): open it, update it, delete it.
    enum class Action { Open, Update, Delete, ConfirmDelete, Cancel };
    std::vector<std::pair<Action, std::string>> menuFor(int index) const;
    void runAction(Action action);
    void openApp(const App& app);
    void deleteApp(int index);
    void drawMenu(nxui::Renderer& r);
    int m_menuFor = -1;         // the app whose menu is open (-1 = none)
    int m_menuSel = 0;
    bool m_confirmDelete = false;

    nxui::Theme m_theme = nxui::Theme::dark();
    nxui::Font m_title, m_body, m_small;
    nxui::Texture m_appIcon;
    std::shared_ptr<nxui::Widget> m_painter;

    std::vector<Item> m_items;
    int m_sel = 0;
    float m_scroll = 0.f;
    float m_repeat = 0.f;

    // The worker: checks, then updates (one thing at a time).
    std::mutex m_mu;
    std::thread m_worker;
    std::atomic<bool> m_busy{false};
    std::atomic<int> m_checked{0};
    std::atomic<float> m_progress{0.f};
    std::atomic<int> m_updating{-1}; // the item being updated
    std::vector<App> m_found;        // the check's results, taken by the UI
    bool m_foundReady = false;
    std::vector<std::pair<int, std::string>> m_finished; // updates done: item, error ("" = ok)
    std::string m_status;
    bool m_network = false;
};

} // namespace updater
