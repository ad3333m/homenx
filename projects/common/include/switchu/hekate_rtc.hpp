#pragma once
#include <switch.h>

// Restarting into hekate's menu on the Switch models that can't reboot into a
// payload (Mariko: Switch OLED, Lite and the patched model, booted by a
// modchip). The modchip starts hekate on every boot, and hekate reads a
// "reboot reason" that the Max77620 power chip's clock keeps through a
// restart: the magic 0x77 in both alarms' weekday registers and the reason,
// split into two 6-bit halves, in their year registers. Reason 2 opens hekate's
// menu. Horizon's alarms are switched off first (those registers carry the
// reason now; Horizon sets them again when it starts). Then restart as usual.
namespace switchu::hekate_rtc {

inline bool requestMenuOnNextBoot() {
    constexpr u8 kUpdate = 0x04;                           // UPDATE0
    constexpr u8 kRegsFromClock = 1u << 4, kClockFromRegs = 1u << 0;
    constexpr u8 kAlarm1 = 0x0E, kAlarm2 = 0x15;           // seconds .. date, 7 registers each
    constexpr u8 kWeekday = 3, kYear = 5;                  // within an alarm
    constexpr u8 kEnabled = 1u << 7, kMagic = 0x77;
    constexpr u16 kReasonMenu = 2;

    if (R_FAILED(i2cInitialize())) return false;
    I2cSession rtc{};
    bool ok = R_SUCCEEDED(i2cOpenSession(&rtc, I2cDevice_Max77620Rtc));
    auto put = [&](u8 reg, u8 value) {
        const u8 cmd[2] = {reg, value};
        return ok && R_SUCCEEDED(i2csessionSendAuto(&rtc, cmd, sizeof(cmd), I2cTransactionOption_All));
    };
    auto get = [&](u8 reg, u8& value) {
        return ok && R_SUCCEEDED(i2csessionSendAuto(&rtc, &reg, 1, I2cTransactionOption_All))
            && R_SUCCEEDED(i2csessionReceiveAuto(&rtc, &value, 1, I2cTransactionOption_All));
    };
    if (ok) {
        ok = put(kUpdate, kRegsFromClock);
        svcSleepThread(16'000'000ULL);
        for (u8 reg = kAlarm1; ok && reg < kAlarm1 + 14; ++reg) {
            u8 value = 0;
            ok = get(reg, value) && put(reg, value & ~kEnabled);
        }
        ok = ok && put(kAlarm1 + kYear, kReasonMenu & 0x3F) && put(kAlarm2 + kYear, (kReasonMenu >> 6) & 0x3F)
            && put(kAlarm1 + kWeekday, kMagic) && put(kAlarm2 + kWeekday, kMagic) && put(kUpdate, kClockFromRegs);
        svcSleepThread(16'000'000ULL);
        i2csessionClose(&rtc);
    }
    i2cExit();
    return ok;
}

} // namespace switchu::hekate_rtc
