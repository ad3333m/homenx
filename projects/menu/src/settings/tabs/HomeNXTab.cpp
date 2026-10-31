#include "TabBuilders.hpp"
#include "core/FontChoice.hpp"
#include "core/PointerSetting.hpp"
#include <nxui/core/I18n.hpp>
#include <algorithm>
#include <filesystem>
#include <iterator>
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
        it.label = i18n.tr("settings.homenx.diagnostics", "Diagnostics");
        it.buttonLabel = i18n.tr("button.show", "Show");
        it.description = i18n.tr("settings.homenx.diagnostics_desc",
            "What HomeNX and the Discord overlay are doing right now: memory, Mii faces, Discord. "
            "A screenshot of it helps fix problems.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) { if (screen.m_diagnosticsCb) screen.m_diagnosticsCb(); };
        t.items.push_back(std::move(it));
    }
    {
        SettingItem it;
        it.label = i18n.tr("settings.homenx.discord_app", "Discord game icons");
        it.buttonLabel = i18n.tr("button.set", "Set");
        it.description = i18n.tr("settings.homenx.discord_app_desc",
            "Your Discord status shows the game's icon with a Discord application of your own: at "
            "discord.com/developers make a New Application (name it Nintendo Switch), then enter its "
            "Application ID here.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) { if (screen.m_discordAppIdCb) screen.m_discordAppIdCb(); };
        t.items.push_back(std::move(it));
    }
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
        it.label = i18n.tr("settings.homenx.fonts", "Fonts");
        it.type = ItemType::Section;
        t.items.push_back(std::move(it));
    }
    // One choice each for HOME, Discord in HOME and the Discord overlay.
    auto fontItem = [&](const char* which, std::string label, std::string usual, std::string desc) {
        SettingItem it;
        it.label = std::move(label);
        it.description = std::move(desc);
        it.type = ItemType::Selector;
        it.options.push_back(std::move(usual));
        const std::string current = fontchoice::chosen(which);
        it.intVal = 0;
        for (const auto& f : fontchoice::kFonts) {
            if (current == f.file) it.intVal = (int)it.options.size();
            it.options.push_back(f.name);
        }
        it.onChange = [&screen, which](SettingItem& self) {
            const int i = self.intVal - 1;
            fontchoice::choose(which, i >= 0 && i < (int)std::size(fontchoice::kFonts) ? fontchoice::kFonts[i].file : "");
            if (screen.m_fontsCb) screen.m_fontsCb();
        };
        t.items.push_back(std::move(it));
    };
    fontItem("home", i18n.tr("settings.homenx.font_home", "HOME font"),
             i18n.tr("settings.homenx.font_theme", "Theme's font"),
             i18n.tr("settings.homenx.font_home_desc", "The font for the whole HOME menu, over the theme's own."));
    fontItem("discord", i18n.tr("settings.homenx.font_discord", "Discord font"),
             i18n.tr("settings.homenx.font_same", "Same as HOME"),
             i18n.tr("settings.homenx.font_discord_desc", "The font for Discord in HOME."));
    fontItem("overlay", i18n.tr("settings.homenx.font_overlay", "Discord overlay font"),
             i18n.tr("settings.homenx.font_system", "Nintendo's font"),
             i18n.tr("settings.homenx.font_overlay_desc",
                     "The font for the Discord overlay in games. It changes the next time the overlay starts "
                     "(after a restart)."));
    {
        SettingItem it;
        it.label = i18n.tr("settings.homenx.pointer", "Pointer (L3)");
        it.description = i18n.tr("settings.homenx.pointer_desc",
            "What pressing L3 turns on. Motion: tilt the controller. Left stick: the stick moves the pointer "
            "(it stops moving the selection until L3 again). A clicks, R3 centres it.");
        it.type = ItemType::Selector;
        it.options = {i18n.tr("settings.homenx.pointer_off", "Off"),
                      i18n.tr("settings.homenx.pointer_gyro", "Motion (gyro)"),
                      i18n.tr("settings.homenx.pointer_stick", "Left stick")};
        it.intVal = pointersetting::read();
        it.onChange = [&screen](SettingItem& self) {
            pointersetting::write(self.intVal);
            if (screen.m_pointerModeCb) screen.m_pointerModeCb(self.intVal);
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
        it.label = i18n.tr("settings.homenx.nextendo_app", "Nextendo app");
        it.buttonLabel = i18n.tr("button.choose", "Choose");
        it.description = i18n.tr("settings.homenx.nextendo_app_desc",
            "The .nro the Nextendo sidebar button opens (sdmc:/switch/prelude.nro to start with). "
            "It always runs as an application. Put the button in a spot below.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) { if (screen.m_nextendoAppCb) screen.m_nextendoAppCb(); };
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
            "Choose the button for this spot; it swaps with the spot it was in.");
        it.type = ItemType::Selector;
        it.options = screen.m_sidebarNames;
        it.intVal = screen.m_sidebarOrder[(std::size_t)slot];
        it.onChange = [&screen, slot, slotLabel](SettingItem& self) {
            auto& order = screen.m_sidebarOrder;
            const int previous = order[(std::size_t)slot];
            const auto other = std::find(order.begin(), order.end(), self.intVal);
            if (other != order.end() && other - order.begin() == slot) return;
            if (other != order.end()) { // it was in another spot: they swap
                const int otherSlot = static_cast<int>(other - order.begin());
                *other = previous;
                // Show the swap on the other row too (edit it in place; rebuilding
                // the tab here would destroy this callback while it runs).
                const std::string otherLabel = slotLabel(otherSlot);
                for (auto& tab : screen.m_tabs)
                    for (auto& row : tab.items)
                        if (row.type == ItemType::Selector && row.label == otherLabel)
                            row.intVal = previous;
            } // else it wasn't on the sidebar (Nextendo): it replaces this spot's button
            order[(std::size_t)slot] = self.intVal;
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
        // Same flag the daemon drops when application mode fails on this console.
        static constexpr const char* kAppletFlag = "sdmc:/switch/SwitchU/homebrew_applet_mode";
        std::error_code ec;
        SettingItem it;
        it.label = i18n.tr("settings.homenx.hb_app", "Sphaira as an application");
        it.description = i18n.tr("settings.homenx.hb_app_desc",
            "On: Sphaira and the custom button get full memory by borrowing one of your games, so the "
            "Switch shows that game's Nintendo Switch logo while it starts. Off: they open instantly "
            "over the Album, with less memory.");
        it.type = ItemType::Toggle;
        it.boolVal = !std::filesystem::exists(kAppletFlag, ec);
        it.onChange = [](SettingItem& self) {
            std::error_code err;
            if (self.boolVal)
                std::filesystem::remove(kAppletFlag, err);
            else
                std::ofstream(kAppletFlag) << "settings";
        };
        t.items.push_back(std::move(it));
    }
    {
        // WiiUMenuApp::lockScreenOn reads the same flag.
        static constexpr const char* kLockFlag = "sdmc:/config/SwitchU/lock_screen";
        std::error_code ec;
        SettingItem it;
        it.label = i18n.tr("settings.homenx.lock", "Lock screen");
        it.description = i18n.tr("settings.homenx.lock_desc",
            "The time and date when the console starts or wakes, over your wallpaper "
            "(sdmc:/switch/SwitchU/lockscreen/wallpaper.png or .jpg). A opens HOME; after a while "
            "with nothing pressed the console sleeps again.");
        it.type = ItemType::Toggle;
        it.boolVal = std::filesystem::exists(kLockFlag, ec);
        it.onChange = [](SettingItem& self) {
            std::error_code err;
            if (self.boolVal)
                std::ofstream(kLockFlag) << "1";
            else
                std::filesystem::remove(kLockFlag, err);
        };
        t.items.push_back(std::move(it));
    }
    {
        // Read when HOME starts (main.cpp): 1920x1080 when docked.
        static constexpr const char* kResolution = "sdmc:/config/SwitchU/resolution.txt";
        std::string want;
        std::getline(std::ifstream(kResolution), want);
        SettingItem it;
        it.label = i18n.tr("settings.homenx.tv1080", "1080p on the TV");
        it.description = i18n.tr("settings.homenx.tv1080_desc",
            "Docked, HOME is drawn at 1920x1080: sharper text and pictures. Off: 1280x720, lighter "
            "for the console. Handheld is always 720p. It changes the next time HOME opens.");
        it.type = ItemType::Toggle;
        it.boolVal = want.rfind("1080", 0) == 0;
        it.onChange = [](SettingItem& self) {
            std::error_code err;
            if (self.boolVal)
                std::ofstream(kResolution) << "1080";
            else
                std::filesystem::remove(kResolution, err);
        };
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
        it.label = i18n.tr("settings.homenx.timing", "Return timing");
        it.buttonLabel = i18n.tr("button.open", "Open");
        it.description = i18n.tr("settings.homenx.timing_desc",
            "Shows how long leaving a game took, step by step. Screenshot it to report a slow return.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) { if (screen.m_returnTimingsCb) screen.m_returnTimingsCb(); };
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
            "Go back to the normal HOME menu. Open NXToggle from homebrew to turn it on again.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) {
            if (screen.m_homenxOffCb) screen.m_homenxOffCb();
        };
        t.items.push_back(std::move(it));
    }
#endif
    return t;
}
