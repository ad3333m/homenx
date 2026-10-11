#include "LaunchAnimation.hpp"
#include <nxui/core/Renderer.hpp>
#include <switch.h>
#include <cmath>

void LaunchAnimation::start(const nxui::Rect& from, const nxui::Texture* tex, float cornerRadius,
                            const nxui::Color& panelColor, const nxui::Color& borderColor,
                            uint64_t titleId, AccountUid uid, std::string title,
                            LaunchCallback onLaunch, nxui::VoidCallback onDone)
{
    m_from         = from;
    m_tex          = tex;
    m_cornerRadius = cornerRadius;
    m_panelColor   = panelColor;
    m_borderColor  = borderColor;
    m_titleId      = titleId;
    m_uid          = uid;
    m_title        = std::move(title);
    m_onLaunch     = std::move(onLaunch);
    m_onDone       = std::move(onDone);
    m_timer        = 0.f;
    m_startTick    = armGetSystemTick();
    m_playing      = true;
    m_resume       = false;
    m_launched     = false;
    m_doneCalled   = false;
    m_target = {(1280.f - kCardSize) * 0.5f, 178.f, kCardSize, kCardSize};
}

void LaunchAnimation::startResume(nxui::VoidCallback onDone) {
    m_from = {};
    m_target = {};
    m_tex = nullptr;
    m_titleId = 0;
    m_uid = {};
    m_title.clear();
    m_onLaunch = {};
    m_onDone = std::move(onDone);
    m_timer = 0.f;
    m_startTick = armGetSystemTick();
    m_playing = true;
    m_resume = true;
    m_launched = false;
    m_doneCalled = false;
}

void LaunchAnimation::onUpdate(float) {
    if (!m_playing) return;
    // Real elapsed time, not the frame dt (which is clamped after a long frame):
    // a slow frame jumps ahead instead of stalling the motion.
    m_timer = static_cast<float>(armTicksToNs(armGetSystemTick() - m_startTick)) / 1e9f;

    if (m_resume) {
        if (m_timer >= kResumeTotalDur && !m_doneCalled) {
            m_doneCalled = true;
            if (m_onDone) m_onDone();
        }
        return; // stays black until the menu has exited
    }

    if (m_timer >= kLaunchAt && !m_launched) {
        m_launched = true;
        if (m_titleId != 0 && m_onLaunch)
            m_onLaunch(m_titleId, m_uid);
    }
    if (m_timer >= kTotalDur && !m_doneCalled) {
        m_doneCalled = true;
        if (m_onDone) m_onDone();
    }
    // Keeps drawing black after onDone while the menu closes; give the menu back
    // if it never did (a failed launch).
    if (m_doneCalled && m_timer >= kTotalDur + 3.f)
        m_playing = false;
}

void LaunchAnimation::onRender(nxui::Renderer& ren) {
    if (!m_playing) return;
    const nxui::Rect screen{0.f, 0.f, 1280.f, 720.f};

    if (m_resume) {
        const float a = nxui::Easing::outCubic(std::min(m_timer / kResumeFadeDur, 1.f));
        ren.drawRect(screen, nxui::Color(0, 0, 0, a));
        return;
    }

    const float t = m_timer;
    if (t >= kTotalDur) {
        ren.drawRect(screen, nxui::Color(0, 0, 0, 1.f));
        return;
    }

    const float rise = std::min(t / kRiseDur, 1.f);
    const float er = nxui::Easing::outBack(rise);          // slight overshoot, then settles
    const float ef = nxui::Easing::outCubic(rise);
    const float out = t > kCardUntil ? nxui::Easing::inOutCubic(std::min((t - kCardUntil) / kOutDur, 1.f)) : 0.f;
    const float alpha = 1.f - out;

    // HOME fades to black behind the rising card (fully black once it's up, so
    // the menu underneath stops being drawn; see coversScreen()).
    ren.drawRect(screen, nxui::Color(0, 0, 0, ef));

    // The card: from the pressed tile to the centre.
    const nxui::Rect card{m_from.x + (m_target.x - m_from.x) * er,
                          m_from.y + (m_target.y - m_from.y) * er,
                          m_from.width + (m_target.width - m_from.width) * ef,
                          m_from.height + (m_target.height - m_from.height) * ef};
    const float radius = m_cornerRadius + (30.f - m_cornerRadius) * ef;

    // Soft glow: a few widening, fading rings in the theme's panel colour.
    for (int i = 3; i >= 1; --i) {
        const float grow = 10.f * i * ef;
        ren.drawRoundedRect(card.expanded(grow), nxui::Color(m_panelColor.r, m_panelColor.g, m_panelColor.b,
                                                             0.07f * (4 - i) * ef * alpha),
                            radius + grow);
    }
    ren.drawRoundedRect(card, m_panelColor.withAlpha(m_panelColor.a * alpha), radius);
    if (m_tex && m_tex->valid())
        ren.drawTextureRounded(m_tex, card.shrunk(6.f), std::max(0.f, radius - 5.f),
                               nxui::Color::white().withAlpha(alpha));
    ren.drawRoundedRectOutline(card, m_borderColor.withAlpha(m_borderColor.a * alpha), radius, 1.5f);

    // The game's name, then a shimmer while it starts.
    const float titleA = std::clamp((t - kTitleIn) / kTitleDur, 0.f, 1.f) * alpha;
    float y = m_target.y + kCardSize + 30.f;
    if (m_font && !m_title.empty() && titleA <= 0.f && t < 0.1f) {
        // Draw it once, invisibly, right at the start: the font prepares the title's
        // letters now, inside the press frame, not as a hitch when it fades in.
        ren.drawText(m_title, {0.f, 0.f}, m_font, nxui::Color(1.f, 1.f, 1.f, 0.f), 1.05f);
    }
    if (m_font && !m_title.empty() && titleA > 0.f) {
        const float scale = 1.05f;
        const nxui::Vec2 size = m_font->measure(m_title);
        const float rise2 = 10.f * (1.f - nxui::Easing::outCubic(std::clamp((t - kTitleIn) / kTitleDur, 0.f, 1.f)));
        ren.drawText(m_title, {640.f - size.x * scale * 0.5f, y + rise2}, m_font,
                     nxui::Color(1.f, 1.f, 1.f, titleA), scale);
        y += size.y * scale + 22.f;
    } else {
        y += 50.f;
    }
    if (titleA > 0.f) {
        const nxui::Rect track{640.f - 110.f, y, 220.f, 4.f};
        ren.drawRoundedRect(track, nxui::Color(1.f, 1.f, 1.f, 0.16f * titleA), 2.f);
        const float phase = std::fmod(t, 1.1f) / 1.1f;
        const float segW = 70.f;
        const float x = track.x - segW + (track.width + segW) * nxui::Easing::inOutCubic(phase);
        ren.pushClipRect(track);
        ren.drawRoundedRect({x, track.y, segW, track.height}, nxui::Color(1.f, 1.f, 1.f, 0.85f * titleA), 2.f);
        ren.popClipRect();
    }
}
