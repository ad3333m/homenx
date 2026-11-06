#pragma once

namespace switchu::lock_screen {

enum class WakeTarget { ExistingMenu, LockedMenu, Application, Menu };

constexpr bool startLocked(bool atBoot, bool wakeStart, bool lockEnabled) {
    return wakeStart || (atBoot && lockEnabled);
}

constexpr WakeTarget wakeTarget(bool lockEnabled, bool menuActive, bool applicationRunning) {
    if (menuActive) return WakeTarget::ExistingMenu;
    if (lockEnabled) return WakeTarget::LockedMenu;
    return applicationRunning ? WakeTarget::Application : WakeTarget::Menu;
}

class ControllerNoticeGate {
public:
    // Always adopt the latest baseline, including rail/wake reconnections
    // while locked. Unlocking must not replay a connection that happened there.
    int observe(int wirelessCount, bool locked, bool asleep) {
        const int change = m_previous < 0 ? 0 : wirelessCount - m_previous;
        m_previous = wirelessCount;
        return locked || asleep ? 0 : change;
    }
private:
    int m_previous = -1;
};
}
