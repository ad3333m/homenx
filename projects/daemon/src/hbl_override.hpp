#pragma once
// HomeNX runs application-mode homebrew in a borrowed game whose code it swaps
// for hbloader (ECS). Atmosphère doesn't know that game is hbl, and its bpc
// mitm only sends the reboots of am, fatal and hbl to the reboot payload: a
// homebrew that restarts the console itself (Prelude after switching to
// Nextendo, Daybreak after an update: bpcRebootSystem) got a plain reboot,
// which on a console with AutoRCM is RCM, a black screen, Hekate to inject.
//
// Atmosphère's loader reads override_config.ini again at every launch: while
// homebrew runs in the borrowed game, [hbl_config] names that game as an hbl
// program (slot 7, unless R is held, like the Album), so the process is hbl
// and its reboots go to the payload.
//
// The loader only ever overwrites what the file names: a slot taken out of
// it (or a file deleted) stays in its memory until the console restarts, and
// the game itself then started as hbl. So when the homebrew closes, slot 7 is
// set to program 0 (no game), read at the next launch; only at boot, with
// the loader fresh, does the entry go.
#include <switchu/file_log.hpp>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace switchu::daemon::hbl_override {

inline constexpr const char* kPath = "sdmc:/atmosphere/config/override_config.ini";
inline constexpr const char* kTempPath = "sdmc:/atmosphere/config/override_config.ini.homenx";
inline constexpr const char* kMarker = "; HomeNX: the game homebrew runs in (removed when it closes)";
inline constexpr const char* kNoProgram = "program_id_7=0x0000000000000000";

namespace detail {

inline std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

inline std::string keyOf(const std::string& line) {
    const std::string t = trimmed(line);
    const size_t eq = t.find('=');
    std::string k = trimmed(eq == std::string::npos ? t : t.substr(0, eq));
    for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}

// The file without HomeNX's entry (the marker and the two lines after it).
inline std::vector<std::string> readWithoutOurs(bool& hadOurs, bool& exists) {
    std::vector<std::string> lines;
    hadOurs = false;
    std::ifstream in(kPath);
    exists = in.is_open();
    std::string line;
    int skip = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trimmed(line) == kMarker) {
            hadOurs = true;
            skip = 2;
            continue;
        }
        if (skip > 0) {
            const std::string k = keyOf(line);
            if (k == "program_id_7" || k == "override_key_7") {
                --skip;
                continue;
            }
            skip = 0;
        }
        lines.push_back(line);
    }
    return lines;
}

inline bool write(const std::vector<std::string>& lines) {
    std::FILE* f = std::fopen(kTempPath, "w");
    if (!f) return false;
    bool ok = true;
    for (const auto& l : lines) ok = ok && std::fprintf(f, "%s\n", l.c_str()) >= 0;
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        std::remove(kTempPath);
        return false;
    }
    std::remove(kPath);
    return std::rename(kTempPath, kPath) == 0;
}

// Puts `ours` at the top of [hbl_config] (made at the end when missing).
inline void insertOurs(std::vector<std::string>& lines, const std::vector<std::string>& ours) {
    size_t at = 0;
    bool found = false;
    for (size_t i = 0; i < lines.size() && !found; ++i) {
        std::string t = trimmed(lines[i]);
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (t == "[hbl_config]") {
            at = i + 1;
            found = true;
        }
    }
    if (!found) {
        if (!lines.empty() && !trimmed(lines.back()).empty()) lines.push_back("");
        lines.push_back("[hbl_config]");
        at = lines.size();
    }
    lines.insert(lines.begin() + static_cast<long>(at), ours.begin(), ours.end());
}

} // namespace detail

// The homebrew closed: slot 7 back to no program, for the loader's next read
// (taking the lines out would leave the game hbl in its memory).
inline void release(const char* why) {
    bool hadOurs = false, exists = false;
    auto lines = detail::readWithoutOurs(hadOurs, exists);
    if (!hadOurs) return;
    detail::insertOurs(lines, {kMarker, kNoProgram});
    const bool ok = detail::write(lines);
    switchu::FileLog::log("[hbl-override] released (%s) ok=%d", why, ok ? 1 : 0);
}

// At boot the loader starts from its defaults: HomeNX's entry goes, and an
// override_config.ini that held nothing else goes with it.
inline void clearAtBoot() {
    bool hadOurs = false, exists = false;
    auto lines = detail::readWithoutOurs(hadOurs, exists);
    if (!hadOurs) return;
    bool onlyHeader = true;
    for (const auto& l : lines) {
        const std::string t = detail::trimmed(l);
        if (!t.empty() && t != "[hbl_config]") onlyHeader = false;
    }
    const bool ok = onlyHeader ? std::remove(kPath) == 0 : detail::write(lines);
    switchu::FileLog::log("[hbl-override] removed at boot ok=%d", ok ? 1 : 0);
}

// Names `host` as an hbl program until release(). Leaves a slot 7 the user
// set alone (then reboots from that homebrew stay plain, as before).
inline void set(std::uint64_t host) {
    bool hadOurs = false, exists = false;
    auto lines = detail::readWithoutOurs(hadOurs, exists);
    for (const auto& l : lines) {
        const std::string k = detail::keyOf(l);
        if (k == "program_id_7" || k == "title_id_7") {
            switchu::FileLog::log("[hbl-override] slot 7 is the user's: not set");
            return;
        }
    }
    char program[48];
    std::snprintf(program, sizeof(program), "program_id_7=0x%016lX", static_cast<unsigned long>(host));
    detail::insertOurs(lines, {kMarker, program, "override_key_7=!R"});
    const bool ok = detail::write(lines);
    switchu::FileLog::log("[hbl-override] 0x%016lX as hbl ok=%d", static_cast<unsigned long>(host), ok ? 1 : 0);
}

} // namespace switchu::daemon::hbl_override
