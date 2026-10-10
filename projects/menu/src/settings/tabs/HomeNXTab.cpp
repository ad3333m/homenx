#include "TabBuilders.hpp"
#include <nxui/core/I18n.hpp>
#include <fstream>
#include <string>

// HomeNX: frontend updates (same release the HomeNX Toggle app installs) and
// a way back to the normal HOME menu.
SettingsScreen::Tab settings::tabs::HomeNXTab::build(SettingsScreen& screen) {
    using SettingItem = SettingsScreen::SettingItem;
    using ItemType = SettingsScreen::ItemType;
    auto& i18n = nxui::I18n::instance();
    SettingsScreen::Tab t;
    t.name = "HomeNX";

    {
        std::string installed = "build 0";
        std::ifstream in("sdmc:/switch/SwitchU/homenx-build.txt");
        std::getline(in, installed);
        SettingItem it;
        it.label = i18n.tr("settings.homenx.installed", "Installed version");
        it.type = ItemType::Info;
        it.infoText = "HomeNX " + installed;
        t.items.push_back(std::move(it));
    }
    {
        SettingItem it;
        it.label = i18n.tr("settings.homenx.update", "Check for updates");
        it.buttonLabel = i18n.tr("settings.homenx.update_button", "Check");
        it.description = i18n.tr("settings.homenx.update_desc",
            "Download and install the newest HomeNX from GitHub. Needs the internet.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) {
            if (screen.m_homenxUpdateCb) screen.m_homenxUpdateCb();
        };
        t.items.push_back(std::move(it));
    }
    {
        SettingItem it;
        it.label = i18n.tr("settings.homenx_off", "Turn Off HomeNX");
        it.buttonLabel = i18n.tr("settings.homenx_off_button", "Turn off");
        it.description = i18n.tr("settings.homenx_off_desc",
            "Go back to the normal HOME menu. Open HomeNX Toggle from homebrew to turn it on again.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) {
            if (screen.m_homenxOffCb) screen.m_homenxOffCb();
        };
        t.items.push_back(std::move(it));
    }
    return t;
}
