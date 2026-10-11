#pragma once
#include <nxui/widgets/Widget.hpp>
#include <nxui/core/Animation.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Texture.hpp>
#include <switch/services/acc.h>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>


using LaunchCallback = std::function<void(uint64_t titleId, AccountUid uid)>;

// HomeNX launch card (our own; replaced SwitchU's zoom). The pressed icon rises
// to the centre over a darkening HOME, the game's name fades in under it and a
// shimmer runs while the game starts; then everything fades to black.
//
// onLaunch then onDone fire at the end, on a black screen; the daemon starts the
// game while the menu closes.
class LaunchAnimation : public nxui::Widget {
public:
    LaunchAnimation() = default;

    void setFont(nxui::Font* font) { m_font = font; }

    void start(const nxui::Rect& from, const nxui::Texture* tex, float cornerRadius,
               const nxui::Color& panelColor, const nxui::Color& borderColor,
               uint64_t titleId, AccountUid uid, std::string title,
               LaunchCallback onLaunch = {}, nxui::VoidCallback onDone = {});

    // Resume an already-open title: brief fade to black, then onDone.
    void startResume(nxui::VoidCallback onDone);

    bool isPlaying() const { return m_playing; }
    // The card's backdrop is fully black from here on: HOME underneath needn't be drawn.
    bool coversScreen() const { return m_playing && !m_resume && m_timer >= kRiseDur; }
    void stop() { m_playing = false; m_tex = nullptr; m_resume = false; }

    float musicFadeProgress() const { // between 0 and 1, where 0 is the initial state
        if (!m_playing) return 1.f;
        const float duration = m_resume ? kResumeTotalDur : kMusicFadeDur;
        return std::min(m_timer / duration, 1.f);
    }

protected:
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& ren) override;

private:
    bool  m_playing  = false;
    bool  m_resume   = false;
    float m_timer    = 0.f;
    std::uint64_t m_startTick = 0;

    static constexpr float kRiseDur   = 0.45f; // icon to the centre
    static constexpr float kTitleIn   = 0.30f; // name fades in from here...
    static constexpr float kTitleDur  = 0.35f; // ...over this long
    static constexpr float kCardUntil = 0.90f; // card fully shown until here
    static constexpr float kOutDur    = 0.30f; // then fades to black
    static constexpr float kTotalDur  = kCardUntil + kOutDur;
    // The launch goes out once the screen is black. Starting the game while the
    // card played froze it: the game's start-up threads and GPU work starved
    // the card, and the system can put the game's logo up as soon as it starts.
    static constexpr float kLaunchAt  = kTotalDur;
    static constexpr float kMusicFadeDur = kTotalDur;
    static constexpr float kCardSize  = 196.f;

    // Resume: soft blackout only.
    static constexpr float kResumeFadeDur = 0.16f;
    static constexpr float kResumeHoldDur = 0.06f;
    static constexpr float kResumeTotalDur = kResumeFadeDur + kResumeHoldDur;

    nxui::Rect     m_from;
    nxui::Rect     m_target;
    float    m_cornerRadius = 24.f;
    const nxui::Texture* m_tex = nullptr;
    nxui::Font* m_font = nullptr;
    std::string m_title;
    nxui::Color   m_panelColor;
    nxui::Color   m_borderColor;
    uint64_t m_titleId = 0;
    AccountUid m_uid = {};
    LaunchCallback m_onLaunch;
    nxui::VoidCallback m_onDone;
    bool    m_launched = false;
    bool    m_doneCalled = false;
};
