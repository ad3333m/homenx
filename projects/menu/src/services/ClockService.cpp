#include "ClockService.hpp"

#include <switchu/time_budget.hpp>

namespace switchu::services {

CalendarSnapshot ClockService::refresh() {
    timebudget::poll(); // the time service may have had no session to spare when HOME started
    u64 timestamp = 0;
    Result rc = timeGetCurrentTime(TimeType_UserSystemClock, &timestamp);
    if (R_FAILED(rc))
        rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &timestamp);
    if (R_FAILED(rc))
        rc = timeGetCurrentTime(TimeType_LocalSystemClock, &timestamp);

    CalendarSnapshot next{};
    next.result = rc;
    if (R_SUCCEEDED(rc)) {
        rc = timeToCalendarTimeWithMyRule(timestamp, &next.calendar, &next.additional);
        next.result = rc;
        next.valid = R_SUCCEEDED(rc);
    }

    if (next.valid || !m_valid) {
        m_snapshot = next;
        m_valid = next.valid;
    }
    return m_snapshot;
}

} // namespace switchu::services
