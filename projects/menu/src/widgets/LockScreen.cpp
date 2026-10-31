#include "widgets/LockScreen.hpp"

#include <nxui/core/I18n.hpp>
#include <switchu/lock_picture.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

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

    if (switchu::lockpicture::get() != m_wallpaperFor) m_wallpaperLoaded = false; // Settings changed it
    if (!m_wallpaperLoaded) {
        loadWallpaper(gpu, renderer);
    }
    m_wallpaperDelay = 0;

    setVisible(true);
    setFocusable(true);
    clearActions();

    m_pinSet = switchu::lockpin::isSet();
    m_pinEntry = false;
    m_pinWrong = 0.f;

    // A: unlock (with a PIN: ask for it first, then check it).
    addAction(static_cast<std::uint64_t>(nxui::Button::A), [this]() {
        if (!m_active || m_animatingHide) return;
        m_idleTime = 0.f;
        if (m_pinSet) {
            if (!m_pinEntry) {
                m_pinEntry = true;
                m_pin.reset();
                return;
            }
            if (!switchu::lockpin::check(m_pin.code())) {
                m_pinWrong = 1.f;
                m_pin.reset();
                return;
            }
            m_pinEntry = false;
        }
        beginHide();
    });
    addAction(static_cast<std::uint64_t>(nxui::Button::B), [this]() {
        m_idleTime = 0.f;
        m_pinEntry = false;
    });
    // The PIN's wheels: the D-pad or either stick.
    const std::pair<nxui::Button, int> moves[] = {
        {nxui::Button::DUp, 0}, {nxui::Button::DDown, 1}, {nxui::Button::DLeft, 2}, {nxui::Button::DRight, 3},
        {nxui::Button::LStickU, 0}, {nxui::Button::LStickD, 1}, {nxui::Button::LStickL, 2}, {nxui::Button::LStickR, 3},
        {nxui::Button::RStickU, 0}, {nxui::Button::RStickD, 1}, {nxui::Button::RStickL, 2}, {nxui::Button::RStickR, 3}};
    for (const auto& [button, key] : moves) {
        const int k = key;
        addAction(static_cast<std::uint64_t>(button), [this, k]() { pinKey(k); });
    }
    // Swallow the rest (they only keep it awake).
    for (const auto button : {nxui::Button::X, nxui::Button::Y, nxui::Button::Plus, nxui::Button::Minus, nxui::Button::L,
                              nxui::Button::R, nxui::Button::ZL, nxui::Button::ZR, nxui::Button::LStick, nxui::Button::RStick}) {
        addAction(static_cast<std::uint64_t>(button), [this]() { m_idleTime = 0.f; });
    }
}

void LockScreen::beginHide() {
    m_fadeTarget = 0.f;
    m_slideTarget = 1.f;
    m_animatingHide = true;
}

void LockScreen::pinKey(int key) {
    m_idleTime = 0.f;
    if (!m_pinEntry) return;
    if (key == 0) m_pin.up();
    else if (key == 1) m_pin.down();
    else if (key == 2) m_pin.left();
    else m_pin.right();
}

void LockScreen::loadWallpaper(nxui::GpuDevice& gpu, nxui::Renderer& renderer) {
    if (m_wallpaperLoaded) return;
    m_wallpaper = nxui::Texture{};
    m_wallpaperFor = switchu::lockpicture::get(); // Settings > HomeNX > Lock screen picture first
    if (m_wallpaperFor.empty() || !m_wallpaper.loadFromFile(gpu, renderer, m_wallpaperFor, 1920))
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
    m_time += dt;
    m_pinWrong = std::max(0.f, m_pinWrong - dt * 1.6f);

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
    if (m_wallpaper.valid()) { // filling the screen, cropped to its shape (not stretched)
        const float tw = static_cast<float>(m_wallpaper.width()), th = static_cast<float>(m_wallpaper.height());
        const float s = std::max(1280.f / tw, 720.f / th), sw = 1280.f / s, sh = 720.f / s;
        renderer.drawTextureSub(&m_wallpaper, {(tw - sw) * 0.5f, (th - sh) * 0.5f, sw, sh}, {0.f, yOffset, 1280.f, 720.f},
                                nxui::Color::white());
    }
    renderer.drawRect({0.f, yOffset, 1280.f, 720.f}, nxui::Color(0.015f, 0.02f, 0.04f, m_wallpaper.valid() ? 0.25f : 0.40f));

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

    // The PIN: four wheels where the prompt was.
    if (m_pinEntry) {
        constexpr float kBoxW = 70.f, kBoxH = 90.f, kGap = 18.f, kTop = 440.f;
        const float shake = std::sin(m_time * 46.f) * 14.f * m_pinWrong;
        const float x0 = (1280.f - (kBoxW * 4.f + kGap * 3.f)) * 0.5f + shake;
        const std::string title = m_pinWrong > 0.f ? "Wrong PIN" : "Enter your PIN";
        const nxui::Vec2 ts = m_small->measure(title);
        renderer.drawText(title, {(1280.f - ts.x) * 0.5f, kTop - 46.f + yOffset}, m_small,
                          m_pinWrong > 0.f ? nxui::Color(1.f, 0.55f, 0.5f, 1.f) : nxui::Color::white(), 1.f);
        for (int i = 0; i < 4; ++i) {
            const nxui::Rect box{x0 + (kBoxW + kGap) * static_cast<float>(i), kTop + yOffset, kBoxW, kBoxH};
            const bool on = i == m_pin.at;
            renderer.drawRoundedRect(box, nxui::Color(1.f, 1.f, 1.f, on ? 0.20f : 0.09f), 16.f);
            renderer.drawRoundedRectOutline(box, nxui::Color(1.f, 1.f, 1.f, on ? 0.85f : 0.25f), 16.f, on ? 2.5f : 1.2f);
            const std::string d(1, static_cast<char>('0' + m_pin.digit[i]));
            const nxui::Vec2 ds = m_large->measure(d);
            renderer.drawText(d, {box.x + (box.width - ds.x * 1.4f) * 0.5f, box.y + (box.height - ds.y * 1.4f) * 0.5f},
                              m_large, nxui::Color::white(), 1.4f);
            if (on) { // up and down arrows on the chosen wheel
                const float cx = box.x + box.width * 0.5f;
                renderer.drawTriangle({cx - 9.f, box.y - 8.f}, {cx + 9.f, box.y - 8.f}, {cx, box.y - 18.f},
                                      nxui::Color(1.f, 1.f, 1.f, 0.8f));
                renderer.drawTriangle({cx - 9.f, box.bottom() + 8.f}, {cx + 9.f, box.bottom() + 8.f}, {cx, box.bottom() + 18.f},
                                      nxui::Color(1.f, 1.f, 1.f, 0.8f));
            }
        }
        const std::string hint = "Up / Down change    Left / Right move    A unlock    B back";
        const nxui::Vec2 hs = m_small->measure(hint);
        renderer.drawText(hint, {(1280.f - hs.x * 0.85f) * 0.5f, kTop + kBoxH + 34.f + yOffset}, m_small,
                          nxui::Color(1.f, 1.f, 1.f, 0.7f), 0.85f);
    } else
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