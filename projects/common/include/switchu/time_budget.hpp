#pragma once

// The console's time service only has a few sessions, for every process together, and libnx's timeInitialize() takes
// 6 at once (the service, four clocks, the time zone service). With a few sysmodules running only ~3 are left by the
// time HOME starts: timeInitialize() failed with 0x64274 (time: ResultOutOfMemory), and with it the clock, the time
// zone and time().
//
// HOME only reads the user clock and converts with the time zone, so it opens just those two sessions, straight into
// libnx's own (timeGetCurrentTime(), timeToCalendarTimeWithMyRule(), localtime() then work as usual), and tries
// again until it gets them. The few calls that need the service itself or another clock (setting the clock,
// automatic correction) open them for a moment (FullScope).
#include <switch.h>

#include <initializer_list>

extern "C" {
extern TimeServiceType __nx_time_service_type;
void __libnx_init_time(void);
}

namespace switchu::timebudget {

inline const char* portName() {
    switch (__nx_time_service_type) {
        case TimeServiceType_Menu: return "time:a";
        case TimeServiceType_System: return "time:s";
        case TimeServiceType_Repair: return "time:r";
        case TimeServiceType_SystemUser: return "time:su";
        default: return "time:u";
    }
}

inline Result openObject(Service* srv, u32 cmd, Service* out) {
    return serviceDispatch(srv, cmd, .out_num_objects = 1, .out_objects = out);
}

// The user clock and the time zone service, at most every 2 s until they open. True once the time is available.
// Main thread only: __libnx_init_time() sets TZ for localtime().
inline bool poll() {
    Service* clock = timeGetServiceSession_SystemClock(TimeType_UserSystemClock);
    Service* zone = timeGetServiceSession_TimeZoneService();
    if (serviceIsActive(zone) && serviceIsActive(clock)) return true;
    static u64 nextTry = 0;
    const u64 now = armGetSystemTick();
    if (now < nextTry) return false;
    nextTry = now + armNsToTicks(2'000'000'000ULL);

    Service srv{};
    if (R_FAILED(smGetService(&srv, portName()))) return false;
    if (!serviceIsActive(clock)) openObject(&srv, 0, clock);
    if (!serviceIsActive(zone)) openObject(&srv, 3, zone);
    serviceClose(&srv);
    if (!serviceIsActive(zone) || !serviceIsActive(clock)) return false;
    __libnx_init_time();
    return true;
}

// For a process that only needs time() and localtime() (they count from the boot time libnx reads once): opens the
// user clock and the time zone service just long enough for __libnx_init_time(), at most every 2 s until it works.
inline bool syncBootTime() {
    static bool done = false;
    static u64 nextTry = 0;
    if (done) return true;
    const u64 now = armGetSystemTick();
    if (now < nextTry) return false;
    nextTry = now + armNsToTicks(2'000'000'000ULL);

    Service srv{};
    if (R_FAILED(smGetService(&srv, portName()))) return false;
    Service* clock = timeGetServiceSession_SystemClock(TimeType_UserSystemClock);
    Service* zone = timeGetServiceSession_TimeZoneService();
    const bool ownClock = !serviceIsActive(clock) && R_SUCCEEDED(openObject(&srv, 0, clock));
    const bool ownZone = !serviceIsActive(zone) && R_SUCCEEDED(openObject(&srv, 3, zone));
    serviceClose(&srv);
    if (serviceIsActive(clock) && serviceIsActive(zone)) {
        __libnx_init_time();
        done = true;
    }
    if (ownZone) serviceClose(zone);
    if (ownClock) serviceClose(clock);
    return done;
}

// The time service, the clocks and the time zone service, whichever are not open, open for this scope (setting the
// clock, automatic correction).
class FullScope {
public:
    FullScope() {
        Service* srv = timeGetServiceSession();
        if (serviceIsActive(srv) || R_FAILED(smGetService(srv, portName()))) return; // timeInitialize() worked
        m_opened = true;
        constexpr u32 kGetClock[3] = {0, 1, 4}; // user, network, local (IStaticService commands)
        for (TimeType type : {TimeType_UserSystemClock, TimeType_NetworkSystemClock, TimeType_LocalSystemClock}) {
            Service* clock = timeGetServiceSession_SystemClock(type);
            if (!serviceIsActive(clock) && R_SUCCEEDED(openObject(srv, kGetClock[type], clock))) m_clocks[type] = true;
        }
        Service* zone = timeGetServiceSession_TimeZoneService();
        m_zone = !serviceIsActive(zone) && R_SUCCEEDED(openObject(srv, 3, zone));
    }
    ~FullScope() {
        if (!m_opened) return;
        if (m_zone) serviceClose(timeGetServiceSession_TimeZoneService());
        for (TimeType type : {TimeType_UserSystemClock, TimeType_NetworkSystemClock, TimeType_LocalSystemClock})
            if (m_clocks[type]) serviceClose(timeGetServiceSession_SystemClock(type));
        serviceClose(timeGetServiceSession());
    }
    FullScope(const FullScope&) = delete;
    FullScope& operator=(const FullScope&) = delete;

private:
    bool m_opened = false;
    bool m_clocks[3] = {};
    bool m_zone = false;
};

} // namespace switchu::timebudget
