// clock_gettime() for HOME.
//
// From the HOME menu process on firmware 20.2.0 the libc clock_gettime() fails (-1) for CLOCK_MONOTONIC and
// CLOCK_REALTIME alike (seen in discord.log), while the system tick works. libcurl times everything with it, so
// every request ended "Timeout was reached" within milliseconds, before a name was even looked up (ip=none): the
// Discord login, the Theme Shop catalog. This one replaces the libc's: the monotonic clock comes from the system
// tick, the wall clock from the console's time service (falling back to the tick, so it still moves).
#include <switch.h>

#include <cerrno>
#include <ctime>

extern "C" int clock_gettime(clockid_t clk, struct timespec* ts) {
    if (!ts) {
        errno = EFAULT;
        return -1;
    }
    const u64 ns = armTicksToNs(armGetSystemTick());
    if (clk == CLOCK_REALTIME) {
        u64 sec = 0;
        Result rc = timeGetCurrentTime(TimeType_UserSystemClock, &sec);
        if (R_FAILED(rc)) rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &sec);
        if (R_FAILED(rc)) rc = timeGetCurrentTime(TimeType_LocalSystemClock, &sec);
        if (R_SUCCEEDED(rc)) {
            ts->tv_sec = static_cast<time_t>(sec);
            ts->tv_nsec = static_cast<long>(ns % 1000000000ULL);
            return 0;
        }
    }
    ts->tv_sec = static_cast<time_t>(ns / 1000000000ULL);
    ts->tv_nsec = static_cast<long>(ns % 1000000000ULL);
    return 0;
}
