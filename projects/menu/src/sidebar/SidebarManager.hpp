#pragma once
#include "SidebarAnimation.hpp"
#include "widgets/AppletButton.hpp"
#include <nxui/core/Texture.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/Theme.hpp>
#include <nxui/widgets/Widget.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SidebarManager {
public:
    // Button ids = icon slots. The order setting puts ten of them in the ten spots.
    enum : int {
        kButtonSphaira = 0, kButtonMii = 1, kButtonControllers = 2, kButtonSleep = 3,
        kButtonThemeShop = 4, kButtonSettings = 5, kButtonEshop = 6, kButtonNews = 7,
        kButtonCustom = 8, kButtonFriends = 9,
        kButtonNextendo = 10, // opens Nextendo (prelude.nro); placed from Settings
        kButtonMusic = 11,    // HomeNX: the music player (Quick Settings' playlist)
        kButtonStorage = 12,  // HomeNX: Settings > Storage (data management)
        kButtonMiiMaker = 13, // HomeNX: Social > Miis, HomeNX's own Mii Maker
        kButtonToggle = 14,   // HomeNX: opens HomeNX Toggle (updates, turning HomeNX off)
        kButtonCount = 15,
        kSlotCount = 10,
    };
    using Order = std::array<int, kSlotCount>; // left column top-down, then right
    static constexpr Order kDefaultOrder = {
        kButtonSphaira, kButtonMii, kButtonNews, kButtonCustom, kButtonSettings,
        kButtonEshop, kButtonControllers, kButtonFriends, kButtonThemeShop, kButtonSleep,
    };

    struct Actions {
        std::function<void()> onAlbum; // homebrew build: opens Sphaira
        std::function<void()> onMiiEditor;
        std::function<void()> onControllers;
        std::function<void()> onSleep;
        std::function<void()> onSettings;
        std::function<void()> onMiiverse;
        std::function<void()> onEshop;
        std::function<void()> onNews;
        std::function<void()> onCustom; // the button you set up in Settings > HomeNX
        std::function<void()> onFriends;
        std::function<void()> onNextendo;
        std::function<void()> onMusic;
        std::function<void()> onStorage;
        std::function<void()> onMiiMaker;
        std::function<void()> onToggle;
    };

    void build(nxui::GpuDevice& gpu, nxui::Renderer& ren,
               const std::string& assetsBase, const Actions& actions,
               const Order& order = kDefaultOrder);
    // Icon + label of the custom button (empty image = no icon).
    void setCustomButton(nxui::GpuDevice& gpu, nxui::Renderer& ren,
                         const std::vector<std::uint8_t>& image, const std::string& label);

    void reloadAssets(nxui::GpuDevice& gpu, nxui::Renderer& ren,
                      const std::string& assetsBase,
                      const std::string& customIconsBase = std::string());
    void invalidateAssetsCache();

    void update(float dt, nxui::Widget* focusedWidget);

    // The dynamic-line HOME layout turns the two side columns into one
    // horizontal control strip between the profile bar and the applications.
    void setDynamicLineLayout(bool enabled);
    void setDynamicLineDownAction(std::function<void()> action);
    void setDynamicLineUpTarget(nxui::Widget* target);
    void setDynamicLineProfileTarget(nxui::Widget* profile);

    void applyTheme(const nxui::Theme& theme);

    std::vector<std::shared_ptr<AppletButton>>& leftButtons()  { return m_leftButtons; }
    std::vector<std::shared_ptr<AppletButton>>& rightButtons() { return m_rightButtons; }
    const std::vector<std::shared_ptr<AppletButton>>& leftButtons() const  { return m_leftButtons; }
    const std::vector<std::shared_ptr<AppletButton>>& rightButtons() const { return m_rightButtons; }

    AppletButton*  albumButton()    const { return m_albumButton; }
    AppletButton*  button(int id)   const { return id >= 0 && id < kButtonCount ? m_byId[(std::size_t)id] : nullptr; }
    nxui::Widget*  settingsButton() const { return m_settingsButton; }
    nxui::Widget*  themeShopButton() const { return m_themeShopButton; }

private:
    void loadAssets(nxui::GpuDevice& gpu, nxui::Renderer& ren,
                    const std::string& assetsBase,
                    const std::string& customIconsBase);
    void tryLoadAnimation(nxui::GpuDevice& gpu, nxui::Renderer& ren,
                          const std::string& webpPath, AppletButton* button,
                          nxui::Texture* staticIcon);

    std::vector<nxui::Texture> m_icons;
    std::array<AppletButton*, kButtonCount> m_byId{};
    std::vector<std::shared_ptr<AppletButton>> m_leftButtons;
    std::vector<std::shared_ptr<AppletButton>> m_rightButtons;
    AppletButton*  m_albumButton    = nullptr;
    nxui::Widget*  m_settingsButton = nullptr;
    nxui::Widget*  m_themeShopButton = nullptr;

    struct AnimEntry {
        SidebarAnimation anim;
        AppletButton*    button    = nullptr;
        nxui::Texture*   staticTex = nullptr;
    };
    std::vector<AnimEntry> m_anims;
    std::string m_loadedAssetsBase;
    std::string m_loadedCustomIconsBase;
    bool m_assetsLoaded = false;
};
