#include "TabBuilders.hpp"
#include <nxui/core/I18n.hpp>
#include <algorithm>
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
        SettingItem it;
        it.label = i18n.tr("settings.homenx.glass", "Glass effects");
        it.description = i18n.tr("settings.homenx.glass_desc",
            "Off keeps HomeNX at a steady 60 FPS. On draws the full liquid-glass and blur look, "
            "which can drop frames.");
        it.type = ItemType::Toggle;
        it.boolVal = screen.m_glassEffects;
        it.onChange = [&screen](SettingItem& self) {
            screen.m_glassEffects = self.boolVal;
            if (screen.m_glassEffectsCb) screen.m_glassEffectsCb(self.boolVal);
        };
        t.items.push_back(std::move(it));
    }
    {
        SettingItem it;
        it.label = i18n.tr("settings.homenx.custom_app", "Custom button: app");
        it.buttonLabel = i18n.tr("button.choose", "Choose");
        it.description = i18n.tr("settings.homenx.custom_app_desc",
            "Which homebrew the custom sidebar button opens. It shows that app's icon by default.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) { if (screen.m_customAppCb) screen.m_customAppCb(); };
        t.items.push_back(std::move(it));
    }
    {
        SettingItem it;
        it.label = i18n.tr("settings.homenx.custom_icon", "Custom button: icon");
        it.buttonLabel = i18n.tr("button.choose", "Choose");
        it.description = i18n.tr("settings.homenx.custom_icon_desc",
            "The app's own icon, or a picture from sdmc:/switch/SwitchU/custom_icons/.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) { if (screen.m_customIconCb) screen.m_customIconCb(); };
        t.items.push_back(std::move(it));
    }
    {
        SettingItem it;
        it.label = i18n.tr("settings.homenx.pages", "Home pages");
        it.buttonLabel = i18n.tr("button.choose", "Choose");
        it.description = i18n.tr("settings.homenx.pages_desc",
            "Choose how many pages HOME has. Empty pages are removed and software moves forward to fit.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) { if (screen.m_homePagesCb) screen.m_homePagesCb(); };
        t.items.push_back(std::move(it));
    }
    {
        SettingItem it;
        it.label = i18n.tr("settings.homenx.order", "Sidebar buttons");
        it.type = ItemType::Section;
        t.items.push_back(std::move(it));
    }
    auto slotLabel = [&i18n](int slot) {
        const bool left = slot < 5;
        return (left ? i18n.tr("settings.homenx.left", "Left") : i18n.tr("settings.homenx.right", "Right"))
             + " " + std::to_string(left ? slot + 1 : slot - 4);
    };
    for (int slot = 0; slot < (int)screen.m_sidebarOrder.size(); ++slot) {
        SettingItem it;
        it.label = slotLabel(slot);
        it.description = i18n.tr("settings.homenx.order_desc",
            "Choose the button for this spot; it swaps with the spot it was in. "
            "Takes effect next time HomeNX opens.");
        it.type = ItemType::Selector;
        it.options = screen.m_sidebarNames;
        it.intVal = screen.m_sidebarOrder[(std::size_t)slot];
        it.onChange = [&screen, slot, slotLabel](SettingItem& self) {
            auto& order = screen.m_sidebarOrder;
            const int previous = order[(std::size_t)slot];
            const auto other = std::find(order.begin(), order.end(), self.intVal);
            if (other == order.end() || other - order.begin() == slot) return;
            const int otherSlot = static_cast<int>(other - order.begin());
            *other = previous;
            order[(std::size_t)slot] = self.intVal;
            // Show the swap on the other row too (edit it in place; rebuilding
            // the tab here would destroy this callback while it runs).
            const std::string otherLabel = slotLabel(otherSlot);
            for (auto& tab : screen.m_tabs)
                for (auto& row : tab.items)
                    if (row.type == ItemType::Selector && row.label == otherLabel)
                        row.intVal = previous;
            if (screen.m_sidebarOrderCb) screen.m_sidebarOrderCb(order);
        };
        t.items.push_back(std::move(it));
    }
#ifdef SWITCHU_MENU
    {
        SettingItem it;
        it.label = "HomeNX";
        it.type = ItemType::Section;
        t.items.push_back(std::move(it));
    }
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
#endif
    return t;
}
