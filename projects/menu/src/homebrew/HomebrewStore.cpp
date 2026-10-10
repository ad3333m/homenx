#include "HomebrewStore.hpp"
#include "core/DebugLog.hpp"
#include <switch.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <strings.h>

namespace {

bool endsWithNro(const char* name) {
    const size_t n = std::strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".nro") == 0;
}

// Fixed-size text field that may lack a NUL (newlib hides strnlen in strict C++).
std::string field(const char* s, size_t n) {
    return std::string(s, std::find(s, s + n, '\0'));
}

std::string fileStem(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    return name.substr(0, name.size() - 4);
}

} // namespace

// NRO layout (libnx nro.h): NroStart, NroHeader (size = end of code), then an
// NroAssetHeader whose section offsets are relative to itself.
bool readHomebrew(const std::string& path, HomebrewApp& app, bool withIcon) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    app.path = path;
    app.name = fileStem(path);

    NroStart start;
    NroHeader header;
    NroAssetHeader asset;
    const bool ok = std::fread(&start, sizeof(start), 1, f) == 1
        && std::fread(&header, sizeof(header), 1, f) == 1
        && header.magic == 0x304F524E // "NRO0"
        && std::fseek(f, header.size, SEEK_SET) == 0
        && std::fread(&asset, sizeof(asset), 1, f) == 1
        && asset.magic == 0x54455341; // "ASET"
    if (!ok) {
        std::fclose(f);
        return true; // still listed, just without metadata
    }

    if (asset.nacp.size >= sizeof(NacpStruct)) {
        auto nacp = std::make_unique<NacpStruct>();
        if (std::fseek(f, header.size + asset.nacp.offset, SEEK_SET) == 0
            && std::fread(nacp.get(), sizeof(NacpStruct), 1, f) == 1) {
            NacpLanguageEntry* entry = nullptr;
            if (R_FAILED(nacpGetLanguageEntry(nacp.get(), &entry)) || !entry || !entry->name[0]) {
                entry = nullptr;
                for (auto& lang : nacp->lang)
                    if (lang.name[0]) { entry = &lang; break; }
            }
            if (entry) {
                app.name = field(entry->name, sizeof(entry->name));
                app.author = field(entry->author, sizeof(entry->author));
            }
            app.version = field(nacp->display_version, sizeof(nacp->display_version));
        }
    }
    if (withIcon && asset.icon.size > 0 && asset.icon.size <= 0x40000) {
        app.icon.resize(asset.icon.size);
        if (std::fseek(f, header.size + asset.icon.offset, SEEK_SET) != 0
            || std::fread(app.icon.data(), app.icon.size(), 1, f) != 1)
            app.icon.clear();
    }
    std::fclose(f);
    return true;
}

namespace {

void scanDir(const std::string& dir, int depth, const std::string& skip, bool withIcons,
             std::vector<HomebrewApp>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string path = dir + "/" + e->d_name;
        if (e->d_type == DT_DIR) {
            if (depth > 0) scanDir(path, depth - 1, skip, withIcons, out);
        } else if (endsWithNro(e->d_name) && strcasecmp(path.c_str(), skip.c_str()) != 0) {
            HomebrewApp app;
            if (readHomebrew(path, app, withIcons)) out.push_back(std::move(app));
        }
    }
    closedir(d);
}

} // namespace

std::vector<HomebrewApp> scanHomebrew(const std::string& skipPath, bool withIcons) {
    std::vector<HomebrewApp> apps;
    scanDir("sdmc:/switch", 1, skipPath, withIcons, apps);
    std::sort(apps.begin(), apps.end(), [](const HomebrewApp& a, const HomebrewApp& b) {
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    DebugLog::log("[homebrew] found %d nro", (int)apps.size());
    return apps;
}

std::vector<std::uint8_t> readHomebrewIcon(const std::string& path) {
    HomebrewApp app;
    readHomebrew(path, app, true);
    return std::move(app.icon);
}

std::string findSphaira(bool allowHbmenu) {
    auto isSphaira = [](const std::string& path) {
        HomebrewApp app;
        return readHomebrew(path, app, false) && strcasecmp(app.name.c_str(), "sphaira") == 0;
    };
    for (const char* path : {"sdmc:/switch/sphaira/sphaira.nro", "sdmc:/switch/sphaira.nro"})
        if (isSphaira(path)) return path;
    for (const HomebrewApp& app : scanHomebrew("", false))
        if (strcasecmp(app.name.c_str(), "sphaira") == 0) return app.path;
    if (allowHbmenu && isSphaira("sdmc:/hbmenu.nro")) return "sdmc:/hbmenu.nro";
    return {};
}
