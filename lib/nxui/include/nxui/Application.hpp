#pragma once
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/Input.hpp>
#include <atomic>
#include <cstdint>
#include <memory>

namespace nxui {

// What the screen's thread is doing now (a watchdog on another thread names
// the step a long freeze happened in). Set at each step; a static string.
inline std::atomic<const char*> g_phase{"starting"};
inline std::atomic<std::uint32_t> g_phaseSerial{0};
inline void setPhase(const char* what) {
    g_phase.store(what, std::memory_order_relaxed);
    g_phaseSerial.fetch_add(1, std::memory_order_relaxed);
}

class Activity;

/// Top-level application object.
/// Owns the GPU device, Renderer, and Input, runs the main loop,
/// and delegates lifecycle events to the attached Activity.
///
/// Usage:
///   nxui::Application app;
///   app.setActivity(std::make_unique<MyActivity>());
///   if (app.initialize()) app.run();
///   app.shutdown();
class Application {
public:
    Application() = default;
    ~Application();

    /// Attach the main activity. Must be called before initialize().
    void setActivity(std::unique_ptr<Activity> activity);

    /// Queue an activity change from inside the main loop.
    void requestActivity(std::unique_ptr<Activity> activity);

    /// Initialise GPU, Renderer, Input, then call activity->onCreate().
    bool initialize();

    /// Enter the main loop (blocks until requestExit() is called).
    void run();

    /// Shutdown: activity->onDestroy(), then GPU/Renderer cleanup.
    void shutdown();

    // Accessors used by Activity
    GpuDevice& gpu()       { return m_gpu; }
    Renderer&  renderer()  { return *m_renderer; }
    Input&     input()     { return m_input; }

    void requestExit()       { m_running = false; }
    bool isRunning() const   { return m_running; }

    /// Disable/enable GPU rendering.  When disabled the main loop still
    /// runs input + update, but skips beginFrame/endFrame so the GPU is
    /// free for whichever app currently owns the foreground.
    void setRenderEnabled(bool e) {
        if (e && !m_renderEnabled) wakeRendering();
        m_renderEnabled = e;
    }
    // Off: every frame is drawn even when nothing moves (render tiers off).
    void setIdleThrottle(bool on) { m_idleThrottle = on; }
    bool renderEnabled() const    { return m_renderEnabled; }

    /// Render tiers. Input and update always run every ~16 ms; what changes
    /// is how often a frame is drawn and shown:
    ///  - active: every turn while there is input, a held button, an
    ///    AnimationManager tween or activity->wantsFullRate(), and for
    ///    kCalmAfter s after; also (without restarting that clock) after a
    ///    redraw request, a texture upload or a timer firing, while text
    ///    is still to draw and while released textures wait to be freed;
    ///  - calm (kCalmAfter s without any of those): one turn in three;
    ///  - deep (kDeepAfter s, activities that opt in): only after
    ///    requestRedraw(), plus a safety frame each second.
    static constexpr float kCalmAfter = 3.f;
    static constexpr float kDeepAfter = 30.f;
    /// Draw the next turn even if the screen is idle (something changed).
    void requestRedraw() { m_redrawRequested = true; }
    /// In the deep idle tier: freeze decorations (also told by onIdleDeep).
    bool deepIdle() const { return m_deepIdle; }
    /// Calm or deep idle: turns are paced on purpose (one in three takes
    /// ~33 ms), so a perf log mustn't count them as slow frames.
    bool pacedTurns() const { return m_idleThrottle && m_quietSeconds >= kCalmAfter; }
    /// Counters for perf logs: main loop turns, and frames actually shown.
    std::uint64_t loopTurns() const       { return m_loopTurns; }
    std::uint64_t presentedFrames() const { return m_presentedFrames; }

    /// Low-power console sleep.  No frame is presented, no input is dispatched
    /// and the loop parks on a 50 ms tick; the activity is still updated slowly
    /// so it can read the wake notification and resume.
    void setSleeping(bool s);
    bool sleeping() const    { return m_sleeping; }

private:
    void renderSleepFrames();
    void wakeRendering() { m_quietSeconds = 0.f; m_redrawRequested = true; }
    bool shouldPresent(float dt);
    void setDeepIdle(bool deep);
    void dispatchInput();
    bool applyPendingActivity();

    GpuDevice  m_gpu;
    std::unique_ptr<Renderer> m_renderer;
    Input      m_input;

    std::unique_ptr<Activity> m_activity;
    std::unique_ptr<Activity> m_pendingActivity;
    bool m_running = true;
    bool m_renderEnabled = true;
    bool m_idleThrottle = true;
    bool m_sleeping = false;
    bool m_redrawRequested = true;
    bool m_deepIdle = false;
    float m_quietSeconds = 0.f;     // since the last reason to draw every frame
    float m_textMissingFor = 0.f;   // text left out of frames, in a row
    float m_sinceLastPresent = 0.f;
    int m_turnsSincePresent = 0;
    std::uint64_t m_loopTurns = 0;
    std::uint64_t m_presentedFrames = 0;
    uint64_t m_navHeldMask = 0;
    int m_navHeldFrames = 0;
    int m_navRepeatCountdown = 0;
};

} // namespace nxui
