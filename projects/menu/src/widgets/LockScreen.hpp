#pragma once

#include <nxui/core/Font.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/core/Input.hpp>
#include <nxui/widgets/Widget.hpp>

#include <functional>
#include <string>
#include <vector>

#include <social/NotificationLog.hpp>
#include <switchu/lock_pin.hpp>

// Wake acknowledgement screen shared by HomeNX and CardsNX when used as HOME.
// It is deliberately a simple press-to-continue lock, like the Switch wake UI.
class LockScreen final : public nxui::Widget {
public:
    LockScreen();
    void setFonts(nxui::Font* large, nxui::Font* small);
    void onUnlock(std::function<void()> callback) { m_onUnlock = std::move(callback); }
    void onAutoSleep(std::function<void()> callback) { m_onAutoSleep = std::move(callback); }
    void setNotifications(const std::vector<notifications::Item>& items);
    // Call once on show: sets active/visible immediately so the first frame
    // covers the home screen. Wallpaper is loaded lazily on the first update.
    void show(nxui::GpuDevice& gpu, nxui::Renderer& renderer);
    void loadWallpaper(nxui::GpuDevice& gpu, nxui::Renderer& renderer);
    void hide();
    bool isActive() const { return m_active; }

protected:
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& renderer) override;

private:
    void unlock();
    void beginHide(); // the unlock's slide and fade
    void pinKey(int key); // 0 up, 1 down, 2 left, 3 right
    // Settings > HomeNX > Lock screen PIN: A asks for it (the wheels) before unlocking.
    bool m_pinSet = false, m_pinEntry = false;
    switchu::lockpin::Wheels m_pin;
    float m_pinWrong = 0.f, m_time = 0.f;
    void openSelectedNotif();
    void moveNotifSel(int delta);

    nxui::Font* m_large = nullptr;
    nxui::Font* m_small = nullptr;
    nxui::Texture m_wallpaper;
    nxui::GpuDevice* m_gpu = nullptr;
    nxui::Renderer* m_renderer = nullptr;
    std::function<void()> m_onUnlock;
    std::function<void()> m_onAutoSleep;
    bool m_active = false;
    int m_pressCount = 0;
    static constexpr int kRequiredPresses = 3;

    // Notifications shown on the lock screen (scrollable, does not unlock).
    std::vector<notifications::Item> m_notifItems;
    int m_notifSel = 0;
    int m_notifTop = 0;
    bool m_viewingNotifs = false;

    // Auto-sleep: returns to standby after this many seconds of no input.
    float m_idleTime = 0.f;
    static constexpr float kAutoSleepSeconds = 18.f;

    // Fade-in for a soft transition instead of an instant snap.
    float m_fade = 1.f;
    float m_fadeTarget = 1.f; // 1 = fully visible, 0 = hidden
    static constexpr float kFadeDur = 0.25f;
    // Slide animation for hide transition
    float m_slide = 0.f; // 0 = no slide, 1 = fully slid up
    float m_slideTarget = 0.f;
    static constexpr float kSlideDist = 80.f; // pixels to slide up when hiding
    bool m_wallpaperLoaded = false;
    std::string m_wallpaperFor; // the lock screen picture it was loaded for

    // Input debounce for A button
    float m_lastPressTime = 0.f;
    static constexpr float kInputDebounce = 0.12f;
    bool m_animatingHide = false; // true when hide animation in progress


    bool m_needNotifRefresh = false;   // deferred notification-log read pending
    int  m_wallpaperDelay   = 0;       // frames to wait before loading wallpaper
};
