#pragma once

#include <switch.h>

namespace switchu::services {

struct CalendarSnapshot {
    bool valid = false;
    TimeCalendarTime calendar{};
    TimeCalendarAdditionalInfo additional{};
    Result result = 0;
};

// Converts the Horizon clock through the device's active timezone rule. This
// avoids relying on process-global libc TZ state in the custom library applet.
class ClockService final {
public:
    CalendarSnapshot refresh();
    // (The clock changed under us: a wake, the time set by hand. Readers
    // that only ask once a minute look at the generation to ask at once.)
    void invalidate() { m_valid = false; ++m_generation; }
    unsigned generation() const { return m_generation; }

private:
    bool m_valid = false;
    unsigned m_generation = 0;
    CalendarSnapshot m_snapshot{};
};

} // namespace switchu::services
