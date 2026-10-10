#include "TabBuilders.hpp"
#include <nxui/core/I18n.hpp>
#include <switch.h>
#include <algorithm>
#include <cstdio>

SettingsScreen::Tab settings::tabs::SleepTab::build(SettingsScreen& screen) {
    using Tab = SettingsScreen::Tab;
    using SettingItem = SettingsScreen::SettingItem;
    using ItemType = SettingsScreen::ItemType;
    auto& i18n = nxui::I18n::instance();
    Tab t;
    t.name = i18n.tr("settings.tabs.sleep", "Sleep");

    const std::vector<std::string> handLabels = {
        i18n.tr("settings.sleep.1min", "1 min"),
        i18n.tr("settings.sleep.3min", "3 min"),
        i18n.tr("settings.sleep.5min", "5 min"),
        i18n.tr("settings.sleep.10min", "10 min"),
        i18n.tr("settings.sleep.30min", "30 min"),
        i18n.tr("settings.sleep.never", "Never")
    };
    const std::vector<std::string> dockLabels = {
        i18n.tr("settings.sleep.1h", "1 hour"),
        i18n.tr("settings.sleep.2h", "2 hours"),
        i18n.tr("settings.sleep.3h", "3 hours"),
        i18n.tr("settings.sleep.6h", "6 hours"),
        i18n.tr("settings.sleep.12h", "12 hours"),
        i18n.tr("settings.sleep.never", "Never")
    };

    SetSysSleepSettings sleep{};
    setsysGetSleepSettings(&sleep);

    {
        SettingItem it; it.label = i18n.tr("settings.sleep.power_actions", "Power");
        it.type = ItemType::Section;
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.sleep.sleep_now_title", "Sleep Mode");
        it.buttonLabel = i18n.tr("power.sleep", "Sleep");
        it.type = ItemType::Action;
        it.description = i18n.tr("settings.sleep.sleep_now_desc", "Put the console into sleep mode.");
        it.onChange = [&screen, &i18n](SettingItem&) {
            screen.requestDialog(
                i18n.tr("settings.sleep.sleep_now", "Sleep"),
                i18n.tr("settings.sleep.sleep_confirm", "Put the console into sleep mode?"),
                {
                    { i18n.tr("settings.sleep.sleep_now", "Sleep"), [&screen]() {
                        if (screen.m_sleepCb) screen.m_sleepCb();
                    } },
                    { i18n.tr("button.cancel", "Cancel"), []() {} }
                });
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.sleep.shutdown_title", "Power Off");
        it.buttonLabel = i18n.tr("power.shutdown", "Shutdown");
        it.type = ItemType::Action;
        it.description = i18n.tr("settings.sleep.shutdown_desc", "Completely power off the console.");
        it.onChange = [&screen, &i18n](SettingItem&) {
            screen.requestDialog(
                i18n.tr("settings.sleep.shutdown", "Shutdown"),
                i18n.tr("settings.sleep.shutdown_confirm", "Power off the console?"),
                {
                    { i18n.tr("settings.sleep.shutdown", "Shutdown"), [&screen]() {
                        if (screen.m_shutdownCb) screen.m_shutdownCb();
                    } },
                    { i18n.tr("button.cancel", "Cancel"), []() {} }
                });
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.sleep.reboot_title", "Restart Console");
        it.buttonLabel = i18n.tr("power.reboot", "Reboot");
        it.type = ItemType::Action;
        it.description = i18n.tr("settings.sleep.reboot_desc", "Restart the console.");
        it.onChange = [&screen, &i18n](SettingItem&) {
            screen.requestDialog(
                i18n.tr("settings.sleep.reboot", "Reboot"),
                i18n.tr("settings.sleep.reboot_confirm", "Restart the console?"),
                {
                    { i18n.tr("settings.sleep.reboot", "Reboot"), [&screen]() {
                        if (screen.m_rebootCb) screen.m_rebootCb();
                    } },
                    { i18n.tr("button.cancel", "Cancel"), []() {} }
                });
        };
        t.items.push_back(std::move(it));
    }

#ifdef SWITCHU_MENU
    {
        // HomeNX: give the normal HOME menu back without a PC. Same rename as
        // HomeNX Toggle / SwitchU-Manager (exefs.nsp -> exefs.nsp.disabled).
        SettingItem it; it.label = i18n.tr("settings.homenx_off", "Turn Off HomeNX");
        it.buttonLabel = i18n.tr("settings.homenx_off_button", "Turn off");
        it.type = ItemType::Action;
        it.description = i18n.tr("settings.homenx_off_desc",
            "Go back to the normal HOME menu. Run HomeNX Toggle from homebrew to turn it on again.");
        it.onChange = [&screen, &i18n](SettingItem&) {
            screen.requestDialog(
                i18n.tr("settings.homenx_off", "Turn Off HomeNX"),
                i18n.tr("settings.homenx_off_confirm",
                        "The console restarts with the normal HOME menu."),
                {
                    { i18n.tr("settings.homenx_off_button", "Turn off"), [&screen, &i18n]() {
                        const char* active = "sdmc:/atmosphere/contents/0100000000001000/exefs.nsp";
                        const char* off = "sdmc:/atmosphere/contents/0100000000001000/exefs.nsp.disabled";
                        std::remove(off);
                        if (std::rename(active, off) == 0 ||
                            std::rename("sdmc:/atmosphere/contents/0100000000001000",
                                        "sdmc:/atmosphere/contents/0100000000001000-homenx-off") == 0) {
                            if (screen.m_rebootCb) screen.m_rebootCb();
                        } else {
                            screen.requestToast(i18n.tr("settings.homenx_off_failed",
                                "Couldn't turn it off. Run HomeNX Toggle from homebrew instead."), 4.f);
                        }
                    } },
                    { i18n.tr("button.cancel", "Cancel"), []() {} }
                });
        };
        t.items.push_back(std::move(it));
    }
#endif

    {
        SettingItem it; it.label = i18n.tr("settings.sleep.auto_sleep", "Auto-Sleep");
        it.type = ItemType::Section;
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.sleep.handheld", "Handheld Auto-Sleep"); it.type = ItemType::Selector;
        it.description = i18n.tr("settings.sleep.handheld_desc", "Idle delay before automatic sleep in handheld mode.");
        it.options = handLabels;
        it.intVal = std::clamp((int)sleep.handheld_sleep_plan, 0, 5);
        it.onChange = [](SettingItem& self) {
            SetSysSleepSettings s{};
            setsysGetSleepSettings(&s);
            s.handheld_sleep_plan = std::clamp(self.intVal, 0, 5);
            setsysSetSleepSettings(&s);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.sleep.docked", "Docked Auto-Sleep"); it.type = ItemType::Selector;
        it.description = i18n.tr("settings.sleep.docked_desc", "Idle delay before automatic sleep while docked.");
        it.options = dockLabels;
        it.intVal = std::clamp((int)sleep.console_sleep_plan, 0, 5);
        it.onChange = [](SettingItem& self) {
            SetSysSleepSettings s{};
            setsysGetSleepSettings(&s);
            s.console_sleep_plan = std::clamp(self.intVal, 0, 5);
            setsysSetSleepSettings(&s);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.sleep.media_sleep", "Sleep While Playing Media"); it.type = ItemType::Toggle;
        it.description = i18n.tr("settings.sleep.media_sleep_desc", "Allow the console to auto-sleep even during media playback.");
        bool val = (sleep.flags & (1u << 0)) != 0;
        it.boolVal = val;
        it.anim01 = val ? 1.f : 0.f;
        it.onChange = [](SettingItem& self) {
            SetSysSleepSettings s{};
            setsysGetSleepSettings(&s);
            if (self.boolVal)
                s.flags |= (1u << 0);
            else
                s.flags &= ~(1u << 0);
            setsysSetSleepSettings(&s);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.sleep.wake_power", "Wake at Power State Change"); it.type = ItemType::Toggle;
        it.description = i18n.tr("settings.sleep.wake_power_desc", "Wake the console when the AC adapter is connected or disconnected.");
        bool val = (sleep.flags & (1u << 1)) != 0;
        it.boolVal = val;
        it.anim01 = val ? 1.f : 0.f;
        it.onChange = [](SettingItem& self) {
            SetSysSleepSettings s{};
            setsysGetSleepSettings(&s);
            if (self.boolVal)
                s.flags |= (1u << 1);
            else
                s.flags &= ~(1u << 1);
            setsysSetSleepSettings(&s);
        };
        t.items.push_back(std::move(it));
    }

    return t;
}
