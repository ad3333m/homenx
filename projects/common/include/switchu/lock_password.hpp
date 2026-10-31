#pragma once
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <nxui/core/Renderer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

// The lock screen's password: 4 to 8 D-pad presses (Up, Down, Left, Right), kept
// as a salted SHA-256 with its length in sdmc:/config/HomeNX/lock_password (the
// lock checks once that many are pressed). Both looks' lock screens use it, and
// both make it the same way: on the lock screen itself, twice.
namespace switchu::lockpw {

inline constexpr const char* kPath = "sdmc:/config/HomeNX/lock_password";
inline constexpr const char* kOldPin = "sdmc:/config/SwitchU/lock_pin"; // (the 4-digit PIN it replaces)
inline constexpr size_t kMin = 4, kMax = 8;

inline std::string hash(const std::string& presses) {
    const std::string s = "homenx-lock:" + presses;
    unsigned char d[32];
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), d, 0);
#else
    mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(s.data()), s.size(), d, 0);
#endif
    char hex[65] = {};
    for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", d[i]);
    return std::string(hex, 64);
}

inline size_t length() { // 0: none
    std::ifstream in(kPath);
    size_t n = 0;
    std::string h;
    in >> n >> h;
    return h.size() == 64 && n >= kMin && n <= kMax ? n : 0;
}
inline bool isSet() { return length() > 0; }
inline bool check(const std::string& presses) {
    std::ifstream in(kPath);
    size_t n = 0;
    std::string h;
    in >> n >> h;
    return n == presses.size() && h.size() == 64 && hash(presses) == h;
}
// Empty turns it off.
inline bool save(const std::string& presses) {
    std::error_code ec;
    std::filesystem::remove(kOldPin, ec);
    if (presses.empty()) {
        std::filesystem::remove(kPath, ec);
        return true;
    }
    if (presses.size() < kMin || presses.size() > kMax) return false;
    std::filesystem::create_directories("sdmc:/config/HomeNX", ec);
    std::ofstream(kPath) << presses.size() << ' ' << hash(presses);
    return true;
}

// Entering it, or making it (then once more to check), on either lock screen.
struct Entry {
    enum Mode { Unlock, Make, Again };
    enum Result { None, Unlocked, Wrong, Made, Mismatch, TooShort };
    Mode mode = Unlock;
    std::string presses, first, note;
    size_t want = 0;               // Unlock: how many presses it is
    float wrong = 0.f, pop = 0.f;  // a wrong try (the dots shake), a press (the newest grows)

    void startUnlock() {
        mode = Unlock;
        presses.clear();
        want = length();
    }
    void startMaking() {
        mode = Make;
        presses.clear();
        first.clear();
    }
    void update(float dt) {
        wrong = std::max(0.f, wrong - dt * 1.4f);
        pop = std::max(0.f, pop - dt * 5.f);
    }
    // A D-pad press: 'U', 'D', 'L' or 'R'.
    Result press(char c) {
        if (presses.size() >= kMax) return None;
        presses += c;
        pop = 1.f;
        if (mode == Unlock && presses.size() == want) {
            const bool ok = check(presses);
            presses.clear();
            if (ok) return Unlocked;
            return fail("Wrong password", Wrong);
        }
        if (mode == Again && presses.size() == first.size()) {
            const bool same = presses == first;
            presses.clear();
            if (same && save(first)) return Made;
            startMaking();
            return fail("That didn't match: make it again", Mismatch);
        }
        return None;
    }
    // A while making it: these presses are the password (4 at least).
    Result confirm() {
        if (mode != Make) return None;
        if (presses.size() < kMin) return fail("At least 4 presses, then A", TooShort);
        first = presses;
        presses.clear();
        mode = Again;
        return None;
    }
    size_t dots() const { return mode == Unlock ? want : mode == Again ? first.size() : std::max(presses.size(), kMin); }
    std::string prompt() const {
        if (wrong > 0.f && !note.empty()) return note;
        return mode == Unlock ? "Enter your password with the D-pad"
             : mode == Make   ? "Make a password: press the D-pad 4 to 8 times, then A"
                              : "Press it once more to check";
    }

private:
    Result fail(const char* why, Result r) {
        note = why;
        wrong = 1.f;
        return r;
    }
};

inline char directionOf(int key) { return "UDLR"[std::clamp(key, 0, 3)]; } // 0 up, 1 down, 2 left, 3 right

inline void arrow(nxui::Renderer& r, char c, float x, float y, float s, const nxui::Color& col) {
    if (c == 'U') r.drawTriangle({x, y - s}, {x - s, y + s * 0.7f}, {x + s, y + s * 0.7f}, col);
    else if (c == 'D') r.drawTriangle({x, y + s}, {x + s, y - s * 0.7f}, {x - s, y - s * 0.7f}, col);
    else if (c == 'L') r.drawTriangle({x - s, y}, {x + s * 0.7f, y + s}, {x + s * 0.7f, y - s}, col);
    else r.drawTriangle({x + s, y}, {x - s * 0.7f, y - s}, {x - s * 0.7f, y + s}, col);
}

// Its dots across the screen at cy: filled as it's pressed (arrows while making it).
inline void drawDots(nxui::Renderer& r, const Entry& e, float cy, float alpha, float time) {
    const size_t n = std::max<size_t>(e.dots(), 1);
    const float gap = 40.f;
    const float x0 = 640.f - gap * (static_cast<float>(n) - 1.f) * 0.5f + std::sin(time * 46.f) * 12.f * e.wrong;
    const nxui::Color white(1.f, 1.f, 1.f, alpha), red(1.f, 0.45f, 0.42f, alpha);
    for (size_t i = 0; i < n; ++i) {
        const float x = x0 + gap * static_cast<float>(i);
        const bool filled = i < e.presses.size();
        const float grow = filled && i + 1 == e.presses.size() ? 1.f + 0.45f * e.pop : 1.f;
        if (filled && e.mode == Entry::Make) arrow(r, e.presses[i], x, cy, 10.f * grow, white);
        else if (filled) r.drawCircle({x, cy}, 9.f * grow, e.wrong > 0.f ? red : white, 24);
        else r.drawRoundedRectOutline({x - 9.f, cy - 9.f, 18.f, 18.f}, e.wrong > 0.f ? red : white.withAlpha(0.5f * alpha), 9.f, 2.f);
    }
}

} // namespace switchu::lockpw
