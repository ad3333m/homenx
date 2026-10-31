#pragma once
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#include <cstdio>
#include <fstream>
#include <string>

// The lock screen's PIN (Settings > HomeNX, or CardsNX's Settings > Look): kept as
// a salted SHA-256 in sdmc:/config/SwitchU/lock_pin, never the digits themselves.
// Either look's lock screen asks for it; both use the same file.
namespace switchu::lockpin {

inline constexpr const char* kPath = "sdmc:/config/SwitchU/lock_pin";

inline std::string hash(const std::string& pin) {
    const std::string s = "homenx-lock:" + pin;
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

inline std::string stored() {
    std::string h;
    std::getline(std::ifstream(kPath), h);
    return h;
}

inline bool isSet() { return stored().size() == 64; }
inline bool check(const std::string& pin) { return isSet() && hash(pin) == stored(); }

// 4 digits sets it, empty turns it off; false (and nothing changes) for anything else.
inline bool save(const std::string& pin) {
    if (pin.empty()) {
        std::remove(kPath);
        return true;
    }
    if (pin.size() != 4 || pin.find_first_not_of("0123456789") != std::string::npos) return false;
    std::ofstream(kPath) << hash(pin);
    return true;
}

// Entering it like a combination lock (any controller, no keyboard): four wheels,
// up / down turn the chosen one, left / right choose.
struct Wheels {
    int digit[4] = {0, 0, 0, 0};
    int at = 0;
    void up() { digit[at] = (digit[at] + 1) % 10; }
    void down() { digit[at] = (digit[at] + 9) % 10; }
    void left() { if (at > 0) --at; }
    void right() { if (at < 3) ++at; }
    void reset() {
        for (int& d : digit) d = 0;
        at = 0;
    }
    std::string code() const {
        std::string s;
        for (int d : digit) s += static_cast<char>('0' + d);
        return s;
    }
};

} // namespace switchu::lockpin
