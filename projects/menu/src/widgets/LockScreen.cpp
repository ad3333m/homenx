#include "widgets/LockScreen.hpp"

#include <nxui/core/I18n.hpp>

#include <chrono>

#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace {
constexpr const char* kWallpaper = "sdmc:/switch/SwitchU/lockscreen/wallpaper.png";

// Let this many frames be presented *before* we touch the SD card for the
// wallpaper.
constexpr int kWarmupFrames = 2;
} // namespace

LockScreen::LockScreen() {
    setRect({0.f, 0.f, 1280.f, 720.f});
    setVisible(false);
    setFocusable(true);
}

void LockScreen::setFonts(nxui::Font* large, nxui::Font* small) {
    m_large = large;
    m_small = small;
}

void LockScreen::setNotifications(const std::vector<notifications::Item>& items) {
    // Notifications are no longer surfaced on the lock screen.
    (void)items;
}

void LockScreen::show(nxui::GpuDevice& gpu, nxui::Renderer& renderer) {
    m_gpu = &gpu;
    m_renderer = &renderer;
    m_active = true;
    m_pressCount = 0;
    m_idleTime = 0.f;
    m_fade = 1.f;
    m_fadeTarget = 1.f;
    m_slide = 0.f;
    m_slideTarget = 0.f;
    m_animatingHide = false;

    if (!m_wallpaperLoaded) {
        loadWallpaper(gpu, renderer);
    }
    m_wallpaperDelay = 0;

    setVisible(true);
    setFocusable(true);
    clearActions();

    // A – immediate unlock (no debounce, no confirmation).
    addAction(static_cast<std::uint64_t>(nxui::Button::A), [this]() {
        if (!m_active) return;
        m_idleTime = 0.f;
        m_fadeTarget = 0.f;
        m_slideTarget = 1.f;
        m_animatingHide = true;
    });

    // Swallow other buttons to reset idle timer.
    for (const auto button : {nxui::Button::X, nxui::Button::Y,
                              nxui::Button::Plus, nxui::Button::Minus, nxui::Button::R,
                              nxui::Button::ZL, nxui::Button::ZR, nxui::Button::LStick,
                              nxui::Button::RStick, nxui::Button::LStickL, nxui::Button::LStickR,
                              nxui::Button::RStickL, nxui::Button::RStickR}) {
        addAction(static_cast<std::uint64_t>(button), [this]() { m_idleTime = 0.f; });
    }
    for (const auto button : {nxui::Button::DUp, nxui::Button::DDown,
                              nxui::Button::DLeft, nxui::Button::DRight,
                              nxui::Button::B, nxui::Button::L}) {
        addAction(static_cast<std::uint64_t>(button), [this]() { m_idleTime = 0.f; });
    }
}

void LockScreen::loadWallpaper(nxui::GpuDevice& gpu, nxui::Renderer& renderer) {
    if (m_wallpaperLoaded) return;
    m_wallpaper = nxui::Texture{};
    if (!m_wallpaper.loadFromFile(gpu, renderer, kWallpaper, 1920))
        m_wallpaper.loadFromFile(gpu, renderer, "sdmc:/switch/SwitchU/lockscreen/wallpaper.jpg", 1920);
    m_wallpaperLoaded = true;
}

void LockScreen::hide() {
    if (!m_active) return;
    m_active = false;
    setVisible(false);
    setFocusable(false);
    clearActions();
}

void LockScreen::unlock() {
    // Called after hide animation finishes
    hide();
    if (m_onUnlock) m_onUnlock();
}

void LockScreen::moveNotifSel(int delta) {
    // Kept only to satisfy the public interface; notifications are gone.
    (void)delta;
}

void LockScreen::openSelectedNotif() {
    // Kept only to satisfy the public interface; notifications are gone.
}

void LockScreen::onUpdate(float dt) {
    if (!m_active) return;

    if (!m_wallpaperLoaded && m_gpu && m_renderer) {
        loadWallpaper(*m_gpu, *m_renderer);
    }

    // Fade animation
    if (m_fade < m_fadeTarget) {
        m_fade = std::min(m_fadeTarget, m_fade + dt / kFadeDur);
    } else if (m_fade > m_fadeTarget) {
        m_fade = std::max(m_fadeTarget, m_fade - dt / kFadeDur);
    }
    // Slide animation
    if (m_slide < m_slideTarget) {
        m_slide = std::min(m_slideTarget, m_slide + dt / kFadeDur);
    } else if (m_slide > m_slideTarget) {
        m_slide = std::max(m_slideTarget, m_slide - dt / kFadeDur);
    }

    // When hide animation has finished, actually unlock.
    if (m_animatingHide && m_fade == 0.f && m_slide == 1.f) {
        m_animatingHide = false;
        unlock();
    }

    m_idleTime += dt;
    if (m_idleTime >= kAutoSleepSeconds && m_onAutoSleep) {
        m_idleTime = 0.f;
        m_onAutoSleep();
    }
}

void LockScreen::onRender(nxui::Renderer& renderer) {
    if (!m_active || !m_large || !m_small) return;

    // Apply slide offset
    const float yOffset = m_slide * kSlideDist;

    renderer.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.035f, 0.045f, 0.075f, 1.f));
    if (m_wallpaper.valid())
        renderer.drawTexture(&m_wallpaper, {0.f, yOffset, 1280.f, 720.f}, nxui::Color::white());
    renderer.drawRect({0.f, yOffset, 1280.f, 720.f}, nxui::Color(0.015f, 0.02f, 0.04f, 0.40f));

    // Clock
    std::time_t now = std::time(nullptr);
    std::tm local{};
    if (now != static_cast<std::time_t>(-1)) {
        localtime_r(&now, &local);
        char timeText[16]{};
        char dateText[64]{};
        std::strftime(timeText, sizeof(timeText), "%H:%M", &local);
        std::strftime(dateText, sizeof(dateText), "%A, %B %e", &local);
        const nxui::Vec2 timeSize = m_large->measure(timeText);
        renderer.drawText(timeText, {(1280.f - timeSize.x * 2.2f) * 0.5f, 180.f + yOffset}, m_large,
                           nxui::Color::white(), 2.2f);
        const nxui::Vec2 dateSize = m_small->measure(dateText);
        renderer.drawText(dateText, {(1280.f - dateSize.x) * 0.5f, 292.f + yOffset}, m_small,
                           nxui::Color(1.f, 1.f, 1.f, 0.92f), 1.f);
    }

    // --- Single-press unlock prompt ---------------------------------------
    {
        auto& i18n = nxui::I18n::instance();
        const std::string prompt =
            i18n.tr("lockscreen.press_to_unlock", "Press A to unlock");

        const nxui::Vec2 promptSize = m_small->measure(prompt);
        const nxui::Color pillBg(0.02f, 0.03f, 0.06f, 0.72f);
        const nxui::Color pillBorder(1.f, 1.f, 1.f, 0.28f);

        constexpr float kPillH   = 76.f;
        constexpr float kPillY   = 596.f;
        constexpr float kKeySize = 44.f;
        constexpr float kKeyGap  = 16.f;
        constexpr float kPadX    = 26.f;

        const float contentW = kKeySize + kKeyGap + promptSize.x;
        const float pillW = std::max(300.f, contentW + kPadX * 2.f);
        const float pillX = (1280.f - pillW) * 0.5f;

        renderer.drawRoundedRect({pillX, kPillY, pillW, kPillH}, pillBg, kPillH * 0.5f);
        renderer.drawRoundedRectOutline({pillX, kPillY, pillW, kPillH}, pillBorder,
                                        kPillH * 0.5f, 1.2f);

        // "A" key-cap
        const float keyX = pillX + kPadX;
        const float keyY = kPillY + (kPillH - kKeySize) * 0.5f;

        renderer.drawRoundedRect({keyX, keyY, kKeySize, kKeySize},
                                 nxui::Color(1.f, 1.f, 1.f, 0.16f), 10.f);
        renderer.drawRoundedRectOutline({keyX, keyY, kKeySize, kKeySize},
                                        nxui::Color(1.f, 1.f, 1.f, 0.45f), 10.f, 1.2f);

        const nxui::Vec2 aSize = m_small->measure("A");
        renderer.drawText("A",
                          {keyX + (kKeySize - aSize.x) * 0.5f,
                           keyY + (kKeySize - aSize.y) * 0.5f},
                          m_small, nxui::Color::white(), 1.f);

        renderer.drawText(prompt,
                          {keyX + kKeySize + kKeyGap,
                           kPillY + (kPillH - promptSize.y) * 0.5f},
                          m_small, nxui::Color::white(), 1.f);
    }

    // Fade‑out overlay when unlocking
    if (m_fade < 1.f)
        renderer.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 1.f - m_fade));
}