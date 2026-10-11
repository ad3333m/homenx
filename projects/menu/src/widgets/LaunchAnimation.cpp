#include "LaunchAnimation.hpp"
#include <nxui/core/Renderer.hpp>
#include <switch.h>


void LaunchAnimation::start(const nxui::Rect& from, const nxui::Texture* tex, float cornerRadius,
                            const nxui::Color& panelColor, const nxui::Color& borderColor,
                            uint64_t titleId, AccountUid uid,
                            LaunchCallback onLaunch, nxui::VoidCallback onDone)
{
    m_from         = from;
    m_tex          = tex;
    m_cornerRadius = cornerRadius;
    m_panelColor   = panelColor;
    m_borderColor  = borderColor;
    m_titleId      = titleId;
    m_uid          = uid;
    m_onLaunch     = std::move(onLaunch);
    m_onDone       = std::move(onDone);
    m_timer        = 0.f;
    m_startTick    = armGetSystemTick();
    m_playing      = true;
    m_resume       = false;
    m_launched     = false;
    m_doneCalled   = false;
    m_postLaunchHold = false;

    float side = std::min(from.width, from.height) * 1.8f;
    m_target = {(1280 - side) * 0.5f, (720 - side) * 0.5f, side, side};
}

void LaunchAnimation::startResume(nxui::VoidCallback onDone) {
    m_from = {};
    m_target = {};
    m_tex = nullptr;
    m_titleId = 0;
    m_uid = {};
    m_onLaunch = {};
    m_onDone = std::move(onDone);
    m_timer = 0.f;
    m_startTick = armGetSystemTick();
    m_playing = true;
    m_resume = true;
    m_launched = false;
    m_doneCalled = false;
    m_postLaunchHold = false;
}

void LaunchAnimation::onUpdate(float) {
    if (!m_playing) return;
    // Real elapsed time, not the frame dt (which is clamped after a long frame).
    m_timer = static_cast<float>(armTicksToNs(armGetSystemTick() - m_startTick)) / 1e9f;

    if (m_resume) {
        if (m_timer >= kResumeTotalDur) {
            if (!m_doneCalled) {
                m_doneCalled = true;
                if (m_onDone)
                    m_onDone();
                m_postLaunchHold = true;
            }
            // Keep black briefly after the resume command so the menu does not
            // flash back while the applet is handing off.
            if (!m_postLaunchHold || m_timer >= kResumeTotalDur + 0.08f)
                m_playing = false;
        }
        return;
    }

    if (m_timer >= kTotalDur) {
        if (!m_launched) {
            m_launched = true;
            if (m_titleId != 0 && m_onLaunch) {
                m_onLaunch(m_titleId, m_uid);
                m_postLaunchHold = true;
            }
        }
        if (!m_doneCalled) {
            m_doneCalled = true;
            if (m_onDone) {
                m_onDone();
                m_postLaunchHold = true;
            }
        }
        if (!m_postLaunchHold || m_timer >= kTotalDur + kPostLaunchBlackHold) {
            m_playing = false;
        }
    }
}

void LaunchAnimation::onRender(nxui::Renderer& ren) {
    if (!m_playing) return;

    if (m_resume) {
        float alpha = 1.f;
        if (m_timer < kResumeFadeDur)
            alpha = nxui::Easing::outCubic(std::min(m_timer / kResumeFadeDur, 1.f));
        ren.drawRect({0, 0, 1280, 720}, nxui::Color(0, 0, 0, alpha));
        return;
    }

    const float t = m_timer;
    constexpr float kDiveStart = kZoomDur + kHoldDur;
    if (t >= kDiveStart + kDiveDur) {
        ren.drawRect({0, 0, 1280, 720}, nxui::Color(0, 0, 0, 1.f));
        return;
    }

    // 1. Fly + grow to the centre (the rest of HOME dims behind it).
    const float ez = nxui::Easing::outCubic(std::min(t / kZoomDur, 1.f));
    nxui::Rect r = {m_from.x + (m_target.x - m_from.x) * ez,
                    m_from.y + (m_target.y - m_from.y) * ez,
                    m_from.width  + (m_target.width  - m_from.width)  * ez,
                    m_from.height + (m_target.height - m_from.height) * ez};
    float scale = 1.f;
    // 2. Settle: a gentle 4% swell.
    if (t > kZoomDur && t < kDiveStart) {
        const float st = (t - kZoomDur) / kHoldDur;
        scale = 1.f + 0.04f * nxui::Easing::outCubic(st);
    }
    // 3. Dive: grow past the screen edges while everything goes to black.
    float dive = 0.f;
    if (t >= kDiveStart) {
        dive = nxui::Easing::inCubic(std::min((t - kDiveStart) / kDiveDur, 1.f));
        scale = 1.04f + 5.f * dive;
    }
    const float w = r.width * scale, h = r.height * scale;
    const nxui::Rect anim = {r.x + (r.width - w) * 0.5f, r.y + (r.height - h) * 0.5f, w, h};
    const float cr = m_cornerRadius * (1.f + 0.3f * ez) * (1.f + dive);
    const float alpha = 1.f - 0.5f * dive;

    ren.drawRect({0, 0, 1280, 720}, nxui::Color(0, 0, 0, 0.55f * ez));
    ren.drawRoundedRect(anim, m_panelColor.withAlpha(m_panelColor.a * alpha), cr);
    ren.drawRoundedRect({anim.x, anim.y, anim.width, anim.height * 0.4f},
                        nxui::Color(1, 1, 1, 0.06f * alpha), cr);
    ren.drawRoundedRectOutline(anim, m_borderColor.withAlpha(m_borderColor.a * alpha), cr, 1.f);
    if (m_tex && m_tex->valid())
        ren.drawTextureRounded(m_tex, anim.shrunk(8.f * scale), std::max(0.f, cr - 4.f),
                               nxui::Color::white().withAlpha(alpha));
    // Black rises over the dive, so the last frame before launch is fully black.
    if (dive > 0.f)
        ren.drawRect({0, 0, 1280, 720}, nxui::Color(0, 0, 0, dive));
}
