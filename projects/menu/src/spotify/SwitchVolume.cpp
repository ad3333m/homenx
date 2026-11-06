#include "spotify/SwitchVolume.hpp"

#include <switch.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace switchvolume {
namespace {

std::mutex g_mu;
bool g_open = false;

// The output in use and its range of steps (0..15 on most); false when audctl can't say.
bool target(AudioTarget& t, s32& lo, s32& hi) {
    if (!g_open) g_open = R_SUCCEEDED(audctlInitialize()); // (kept open: it's read a few times a second)
    if (!g_open) return false;
    if (R_FAILED(audctlGetActiveOutputTarget(&t)) || t == AudioTarget_Invalid) t = AudioTarget_Speaker;
    if (R_FAILED(audctlGetTargetVolumeMin(&lo)) || R_FAILED(audctlGetTargetVolumeMax(&hi)) || hi <= lo) {
        lo = 0;
        hi = 15;
    }
    return true;
}

} // namespace

int get() {
    std::lock_guard<std::mutex> lock(g_mu);
    AudioTarget t;
    s32 lo, hi, v = 0;
    if (!target(t, lo, hi) || R_FAILED(audctlGetTargetVolume(&v, t))) return -1;
    return (int)std::lround((v - lo) * 100.0 / (hi - lo));
}

bool set(int percent) {
    std::lock_guard<std::mutex> lock(g_mu);
    AudioTarget t;
    s32 lo, hi;
    if (!target(t, lo, hi)) return false;
    const s32 v = lo + (s32)std::lround(std::clamp(percent, 0, 100) * (hi - lo) / 100.0);
    return R_SUCCEEDED(audctlSetTargetVolume(t, v));
}

bool step(int direction) {
    std::lock_guard<std::mutex> lock(g_mu);
    AudioTarget t;
    s32 lo, hi, v = 0;
    if (!target(t, lo, hi) || R_FAILED(audctlGetTargetVolume(&v, t))) return false;
    return R_SUCCEEDED(audctlSetTargetVolume(t, std::clamp(v + (direction > 0 ? 1 : -1), lo, hi)));
}

} // namespace switchvolume
