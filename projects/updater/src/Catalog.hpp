#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace updater {

// One .nro on the SD card, and where a newer one can come from.
struct App {
    std::string path, name, author, version; // installed (from the .nro's NACP)
    std::vector<std::uint8_t> icon;          // JPEG
    std::string latest;                      // newest known version ("" = unknown)
    std::string source;                      // "GitHub owner/repo" or "HB App Store"
    std::string downloadUrl;
    std::string zipEntry; // the .nro's path inside a zip download ("" = not a zip)
    bool zip = false;
};

// Every .nro in sdmc:/switch and one folder below, except `self`. Sorted by name.
std::vector<App> scanApps(const std::string& self);

// Where each app's updates come from and its newest version: GitHub releases
// for known projects (and ad3333m's public repos), else the HB App Store.
// Network; run it on a worker. `done` counts apps looked at.
void findUpdates(std::vector<App>& apps, std::atomic<int>& done);

// Downloads the newest .nro and puts it in place of the old one.
bool installUpdate(const App& app, std::atomic<float>& progress, std::string& error);

// Whether `latest` is a newer version than `installed` ("v1.2.0" > "1.1.9").
bool isNewer(const std::string& latest, const std::string& installed);

// Starts / stops sockets and curl.
bool startNetwork();
void stopNetwork();

} // namespace updater
