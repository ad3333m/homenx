#include "AppletLauncher.hpp"
#include "core/DebugLog.hpp"
#ifdef SWITCHU_MENU
#include "smi_commands.hpp"
#include <switchu/smi_protocol.hpp>
#endif
#include <switch.h>
#include <sys/stat.h>
#include <strings.h>
#include "homebrew/HomebrewStore.hpp"
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <cerrno>
#include <vector>
#include <iterator>
#include <fstream>
#include <cstdio>

void AppletLauncher::init(Callbacks cbs) {
    m_cb = std::move(cbs);
}

#ifdef SWITCHU_MENU
bool AppletLauncher::isAppRunning() const  { return m_appRunning; }
bool AppletLauncher::appHasForeground() const { return m_appHasForeground; }
bool AppletLauncher::isAppSuspended(uint64_t titleId) const {
    return m_suspendedTitleId != 0 && m_suspendedTitleId == titleId;
}
uint64_t AppletLauncher::suspendedTitleId() const { return m_suspendedTitleId; }

void AppletLauncher::setAppRunning(bool v)          { m_appRunning = v; }
void AppletLauncher::setAppHasForeground(bool v)    { m_appHasForeground = v; }
void AppletLauncher::setSuspendedTitleId(uint64_t v){ m_suspendedTitleId = v; }

void AppletLauncher::setStartupStatus(uint64_t suspendedTitleId, bool appRunning) {
    m_suspendedTitleId = suspendedTitleId;
    m_appRunning       = appRunning;
    m_appHasForeground = false;
    DebugLog::log("[launcher] startup status: suspended=0x%016lX running=%d",
                  suspendedTitleId, appRunning);
}

bool AppletLauncher::preparePowerAction() {
    if (!m_cb.beforePowerAction)
        return true;
    if (m_cb.beforePowerAction())
        return true;
    DebugLog::log("[launcher] power action cancelled: SD commit failed");
    return false;
}

void AppletLauncher::launchAlbum() {
    DebugLog::log("[launcher] requesting Album launch via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::LaunchAlbum);
    DebugLog::log("[launcher] Album rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchMiiEditor() {
    DebugLog::log("[launcher] requesting Mii Editor launch via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::LaunchMiiEditor);
    DebugLog::log("[launcher] Mii Editor rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchControllerPairing() {
    DebugLog::log("[launcher] requesting Controller pairing via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::LaunchControllers);
    DebugLog::log("[launcher] Controller pairing rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchControllerRemapping() {
    DebugLog::log("[launcher] requesting controller remapping via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(
        switchu::smi::SystemMessage::LaunchControllerRemapping);
    DebugLog::log("[launcher] controller remapping rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchNetConnect() {
    DebugLog::log("[launcher] requesting NetConnect launch via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::LaunchNetConnect);
    DebugLog::log("[launcher] NetConnect rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchUserPage(AccountUid uid) {
    DebugLog::log("[launcher] requesting User Page launch via daemon");
    Result rc = switchu::menu::smi_cmd::launchUserPage(uid);
    DebugLog::log("[launcher] User Page rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchUserCreator() {
    DebugLog::log("[launcher] requesting system user creator via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(
        switchu::smi::SystemMessage::LaunchUserCreator);
    DebugLog::log("[launcher] user creator rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::enterSleep() {
    if (!preparePowerAction()) return;
    DebugLog::log("[launcher] requesting sleep");
    switchu::menu::smi_cmd::enterSleep();
}

void AppletLauncher::shutdown() {
    if (!preparePowerAction()) return;
    DebugLog::log("[launcher] requesting shutdown");
    switchu::menu::smi_cmd::shutdown();
}

void AppletLauncher::reboot() {
    if (!preparePowerAction()) return;
    DebugLog::log("[launcher] requesting reboot");
    switchu::menu::smi_cmd::reboot();
}

bool AppletLauncher::requestLaunch(uint64_t titleId, AccountUid uid) {
    DebugLog::log("[launcher] tid=%016lX", titleId);
    const Result rc = switchu::menu::smi_cmd::launchApplication(titleId, uid);
    if (R_FAILED(rc))
        DebugLog::log("[launcher] FAIL: 0x%X", rc);
    else
        DebugLog::log("[launcher] command sent; the daemon starts it while the menu closes");
    return R_SUCCEEDED(rc);
}

void AppletLauncher::closeForLaunch() {
    if (m_cb.requestExit) m_cb.requestExit();
}

void AppletLauncher::resumeApplication() {
    if (m_suspendedTitleId == 0) {
        DebugLog::log("[launcher] no app suspended!");
        return;
    }
    switchu::FileLog::log("[launcher] resume: sending command");
    const Result rc = switchu::menu::smi_cmd::resumeApplication();
    switchu::FileLog::log("[launcher] resume: command returned 0x%X", rc);
    if (m_cb.requestExit) m_cb.requestExit();
    switchu::FileLog::log("[launcher] resume: exit requested");
}

void AppletLauncher::terminateApplication() {
    if (m_suspendedTitleId == 0) {
        DebugLog::log("[launcher] no app suspended, nothing to terminate");
        return;
    }
    DebugLog::log("[launcher] requesting terminate 0x%016lX", (uint64_t)m_suspendedTitleId);
    const Result rc = switchu::menu::smi_cmd::terminateApplication();
    if (R_FAILED(rc))
        DebugLog::log("[launcher] terminate command FAIL: 0x%X", rc);
}

void AppletLauncher::checkRunningApplication() {
}

// ponytail: the daemon has no eShop/News/Web/Friends messages yet; homebrew build only.
namespace {
constexpr const char* kHbmenu = "sdmc:/hbmenu.nro";
constexpr const char* kHbmenuOriginal = "sdmc:/hbmenu.nro.homenx-original";
// The Loader ships inside HomeNX's own folder (every updater writes there);
// switch/HomeNX-Loader/ was build 13's spot, which older updaters skipped.
constexpr const char* kLoaderSources[] = {
    "sdmc:/switch/SwitchU/bin/homenx-loader.nro",
    "sdmc:/switch/HomeNX-Loader/HomeNX-Loader.nro",
};
constexpr const char* kLoaderNext = "sdmc:/config/SwitchU/hbl_next.txt";

bool nroNamed(const char* path, const char* name) {
    HomebrewApp app;
    return readHomebrew(path, app) && strcasecmp(app.name.c_str(), name) == 0;
}

void writeLine(const char* path, const std::string& line) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (FILE* f = std::fopen(path, "w")) {
        std::fputs((line + "\n").c_str(), f);
        std::fclose(f);
    }
}
} // namespace

// Atmosphere's homebrew loader (over the Album applet) always starts
// sdmc:/hbmenu.nro, also after every app exits. HomeNX puts its own tiny
// Loader there, which hands over to the requested .nro or to Sphaira.
// The Loader is compiled into the menu (CI generates loader_nro.inc from the
// built HomeNX-Loader.nro), so it can't go missing; the copies in
// switch/SwitchU/bin/ are only a fallback for builds without it.
#if __has_include("generated/loader_nro.inc")
#include "generated/loader_nro.inc" // const unsigned char kLoaderNroData[]
#define HOMENX_EMBEDDED_LOADER 1
#endif

namespace {
std::string g_loaderError;

std::vector<std::uint8_t> loaderBytes() {
#ifdef HOMENX_EMBEDDED_LOADER
    return std::vector<std::uint8_t>(std::begin(kLoaderNroData), std::end(kLoaderNroData));
#else
    for (const char* candidate : kLoaderSources) {
        std::ifstream in(candidate, std::ios::binary);
        std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (!data.empty()) return data;
    }
    return {};
#endif
}

std::vector<std::uint8_t> fileBytes(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

const std::string& AppletLauncher::loaderError() { return g_loaderError; }

bool AppletLauncher::installLoader() {
    g_loaderError.clear();
    const std::vector<std::uint8_t> loader = loaderBytes();
    if (loader.empty()) {
        g_loaderError = "The Loader isn't in this HomeNX build or in switch/SwitchU/bin/.";
        return false;
    }
    std::error_code ec;
    if (std::filesystem::exists(kHbmenu, ec)) {
        if (fileBytes(kHbmenu) == loader) return true; // already installed
        if (!nroNamed(kHbmenu, "HomeNX-Loader")) {
            // Keep the user's own hbmenu (newest one wins) for the Loader's fallback.
            std::filesystem::remove(kHbmenuOriginal, ec);
            ec.clear();
            std::filesystem::rename(kHbmenu, kHbmenuOriginal, ec);
            if (ec) DebugLog::log("[launcher] keeping hbmenu.nro failed ec=%d", ec.value());
        }
    }
    // Write through a temporary file so a cut-off write never leaves a broken hbmenu.
    const std::string tmp = std::string(kHbmenu) + ".homenx-tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        g_loaderError = "Couldn't create sdmc:/hbmenu.nro (" + std::string(std::strerror(errno)) + ").";
        return false;
    }
    const bool written = std::fwrite(loader.data(), 1, loader.size(), f) == loader.size();
    std::fclose(f);
    ec.clear();
    if (written) {
        std::filesystem::remove(kHbmenu, ec);
        ec.clear();
        std::filesystem::rename(tmp, kHbmenu, ec);
    }
    if (!written || ec || fileBytes(kHbmenu) != loader) {
        g_loaderError = "Couldn't write sdmc:/hbmenu.nro (" +
            (ec ? ec.message() : std::string(written ? "verify failed" : std::strerror(errno))) + ").";
        std::filesystem::remove(tmp, ec);
        return false;
    }
    DebugLog::log("[launcher] HomeNX Loader installed as hbmenu.nro (%zu bytes)", loader.size());
    return true;
}

// The updaters only write inside HomeNX's folders, so apps that live elsewhere
// ship in switch/SwitchU/bin and are copied out here.
void AppletLauncher::installBundledApps() {
    static const struct { const char* source; const char* dir; const char* target; } kApps[] = {
        {"sdmc:/switch/SwitchU/bin/homenx-toggle.nro", "sdmc:/switch/HomeNX-Toggle",
         "sdmc:/switch/HomeNX-Toggle/HomeNX-Toggle.nro"},
        {"sdmc:/switch/SwitchU/bin/homenx-updater.nro", "sdmc:/switch/HomeNX-Updater",
         "sdmc:/switch/HomeNX-Updater/HomeNX-Updater.nro"},
        // Discord over games: a Tesla/Ultrahand overlay.
        {"sdmc:/switch/SwitchU/bin/homenx-discord.ovl", "sdmc:/switch/.overlays",
         "sdmc:/switch/.overlays/homenx-discord.ovl"},
    };
    // Same bytes, not just the same size: .nro/.ovl segments are page-aligned,
    // so a new build is often exactly as big as the old one (that kept an old
    // Discord overlay installed).
    auto sameFile = [](const char* a, const char* b) {
        FILE* fa = std::fopen(a, "rb");
        FILE* fb = fa ? std::fopen(b, "rb") : nullptr;
        bool same = fa && fb;
        std::vector<char> ba(64 * 1024), bb(64 * 1024);
        while (same) {
            const size_t na = std::fread(ba.data(), 1, ba.size(), fa);
            const size_t nb = std::fread(bb.data(), 1, bb.size(), fb);
            same = na == nb && std::memcmp(ba.data(), bb.data(), na) == 0;
            if (na == 0) break;
        }
        if (fa) std::fclose(fa);
        if (fb) std::fclose(fb);
        return same;
    };
    for (const auto& app : kApps) {
        std::error_code ec;
        if (!std::filesystem::exists(app.source, ec)) continue;
        if (sameFile(app.source, app.target)) continue;
        std::filesystem::create_directories(app.dir, ec);
        // A plain read/write copy (std::filesystem::copy_file left an empty
        // file on the console, which nx-ovlloader then failed on: 2347-0004),
        // under another name and renamed only once it's complete and the
        // right size, so nothing ever sees a half-written file.
        const std::string tmp = std::string(app.target) + ".tmp";
        bool ok = false;
        if (FILE* in = std::fopen(app.source, "rb")) {
            if (FILE* out = std::fopen(tmp.c_str(), "wb")) {
                std::vector<char> buf(256 * 1024);
                ok = true;
                size_t n;
                while (ok && (n = std::fread(buf.data(), 1, buf.size(), in)) > 0)
                    ok = std::fwrite(buf.data(), 1, n, out) == n;
                ok = std::fclose(out) == 0 && ok && !std::ferror(in);
            }
            std::fclose(in);
        }
        ok = ok && std::filesystem::file_size(tmp, ec) == std::filesystem::file_size(app.source, ec);
        if (ok) {
            std::filesystem::remove(app.target, ec);
            std::filesystem::rename(tmp, app.target, ec);
            ok = !ec;
        } else {
            std::filesystem::remove(tmp, ec);
        }
        DebugLog::log("[launcher] installed %s ok=%d", app.target, ok ? 1 : 0);
    }

    // Ultrahand settings for the overlay, in sdmc:/config/ultrahand/overlays.ini:
    // - key_combo=L+ZL, once (a combo he changes in Ultrahand later is left alone);
    // - force_support=true: it's built with the new libnx (MOD0 "LNY2"), but an
    //   Ultrahand that looked at it while this copy was being written keeps
    //   calling it "Incompatible on AMS v1.10+".
    constexpr const char* kComboDone = "sdmc:/config/SwitchU/discord_overlay_combo.txt";
    constexpr const char* kIni = "sdmc:/config/ultrahand/overlays.ini";
    std::error_code ec;
    if (!std::filesystem::exists("sdmc:/switch/.overlays/homenx-discord.ovl", ec)) return;
    std::string ini;
    if (FILE* f = std::fopen(kIni, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) ini.append(buf, n);
        std::fclose(f);
    }
    const std::string original = ini;
    const std::string section = "[homenx-discord.ovl]";
    if (ini.find(section) == std::string::npos) {
        if (!ini.empty() && ini.back() != '\n') ini += '\n';
        ini += section + "\n";
    }
    // The value of `key` in our section: npos if it isn't there.
    auto valueAt = [&](const std::string& key) {
        const size_t at = ini.find(section);
        const size_t next = ini.find("\n[", at + section.size());
        const size_t k = ini.find("\n" + key + "=", at);
        return k != std::string::npos && (next == std::string::npos || k < next) ? k + key.size() + 2
                                                                                  : std::string::npos;
    };
    auto setKey = [&](const std::string& key, const std::string& value) {
        const size_t v = valueAt(key);
        if (v != std::string::npos) {
            const size_t end = ini.find_first_of("\r\n", v);
            ini.replace(v, (end == std::string::npos ? ini.size() : end) - v, value);
        } else {
            const size_t lineEnd = ini.find('\n', ini.find(section));
            ini.insert(lineEnd == std::string::npos ? ini.size() : lineEnd + 1, key + "=" + value + "\n");
        }
    };
    if (!std::filesystem::exists(kComboDone, ec)) {
        const size_t v = valueAt("key_combo");
        if (v == std::string::npos || v >= ini.size() || ini[v] == '\n' || ini[v] == '\r')
            setKey("key_combo", "L+ZL");
    }
    setKey("force_support", "true");
    bool ok = ini == original;
    if (!ok) {
        std::filesystem::create_directories("sdmc:/config/ultrahand", ec);
        if (FILE* f = std::fopen(kIni, "wb")) {
            ok = std::fwrite(ini.data(), 1, ini.size(), f) == ini.size();
            std::fclose(f);
        }
        DebugLog::log("[launcher] Ultrahand settings for the Discord overlay ok=%d", ok ? 1 : 0);
    }
    if (ok && !std::filesystem::exists(kComboDone, ec))
        if (std::FILE* done = std::fopen(kComboDone, "wb")) std::fclose(done);

    // An earlier Discord overlay set Ultrahand's overlay memory to 6 MB. That
    // comes from the system's own pool, and hid then fataled (2001-0132) when
    // HOME or a game started: back to the 4 MB default, once (the overlay does
    // the same). A size chosen in Ultrahand afterwards is left alone.
    constexpr const char* kHeap = "sdmc:/config/nx-ovlloader/heap_size.bin";
    constexpr const char* kHeapRestored = "sdmc:/config/SwitchU/overlay_heap_restored";
    if (!std::filesystem::exists(kHeapRestored, ec)) {
        std::uint64_t size = 0;
        if (std::FILE* f = std::fopen(kHeap, "rb")) {
            if (std::fread(&size, sizeof(size), 1, f) != 1) size = 0;
            std::fclose(f);
        }
        if (size == 0x600000) std::filesystem::remove(kHeap, ec);
        if (std::FILE* done = std::fopen(kHeapRestored, "wb")) std::fclose(done);
        DebugLog::log("[launcher] overlay memory was %llu bytes%s", (unsigned long long)size,
                      size == 0x600000 ? ", back to the 4 MB default" : "");
    }
}

namespace {
constexpr const char* kHblDir = "sdmc:/switch/SwitchU/bin/hbl";
constexpr const char* kHblNsp = "sdmc:/atmosphere/hbl.nsp";
}

// hbl.nsp's npdm declares the process an applet (kernel capability "application
// type" = 2). Atmosphère's own hbl-over-a-game path swaps in the game's
// capabilities; launching it as external code doesn't, so AM rejected the
// borrowed game and Sphaira fell back to HOME. Declare it an application (1), in
// both the ACI0 and the ACID kernel capabilities (the ACI0 must fit the ACID).
// Capability word for the type: (type << 14) | 0x1FFF.
static bool patchHblNpdmAsApplication(std::vector<char>& npdm) {
    auto u32At = [&](std::size_t off) -> u32 {
        u32 v = 0;
        if (off + 4 <= npdm.size()) std::memcpy(&v, npdm.data() + off, 4);
        return v;
    };
    auto patchKac = [&](std::size_t kacOff, std::size_t kacSize) {
        int patched = 0;
        for (std::size_t off = kacOff; off + 4 <= kacOff + kacSize && off + 4 <= npdm.size(); off += 4) {
            const u32 cap = u32At(off);
            if ((cap & 0x3FFF) == 0x1FFF && ((cap >> 14) & 7) != 1) {
                const u32 app = (1u << 14) | 0x1FFF;
                std::memcpy(npdm.data() + off, &app, 4);
                ++patched;
            }
        }
        return patched;
    };
    if (npdm.size() < 0x80 || u32At(0) != 0x4154454D /* META */) return false;
    const std::size_t aci = u32At(0x70), acid = u32At(0x78);
    if (u32At(aci) != 0x30494341 /* ACI0 */ || u32At(acid + 0x200) != 0x44494341 /* ACID */) return false;
    const int a = patchKac(aci + u32At(aci + 0x30), u32At(aci + 0x34));
    const int b = patchKac(acid + u32At(acid + 0x230), u32At(acid + 0x234));
    DebugLog::log("[launcher] hbl npdm application type patched aci=%d acid=%d", a, b);
    return true;
}

bool AppletLauncher::prepareHbl() {
    std::error_code ec;
    const std::string mainPath = std::string(kHblDir) + "/main";
    const std::string npdmPath = std::string(kHblDir) + "/main.npdm";
    const std::string stampPath = std::string(kHblDir) + "/source-size";
    if (!std::filesystem::exists(kHblNsp, ec)) return false;
    // "-app": stamp of npdms patched to application type (older ones get redone).
    const std::string size = std::to_string(std::filesystem::file_size(kHblNsp, ec)) + "-app";
    {
        std::ifstream stamp(stampPath);
        std::string seen;
        if (std::getline(stamp, seen) && seen == size
            && std::filesystem::exists(mainPath, ec) && std::filesystem::exists(npdmPath, ec))
            return true;
    }
    // PFS0: header, file entries {u64 offset, u64 size, u32 name, u32 pad},
    // string table, then the file data.
    std::ifstream in(kHblNsp, std::ios::binary);
    std::vector<char> nsp{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    struct Header { u32 magic, count, strings, reserved; };
    struct Entry { u64 offset, size; u32 name, reserved; };
    if (nsp.size() < sizeof(Header)) return false;
    Header h;
    std::memcpy(&h, nsp.data(), sizeof(h));
    const std::size_t entriesEnd = sizeof(Header) + std::size_t(h.count) * sizeof(Entry);
    if (h.magic != 0x30534650 /* PFS0 */ || h.count > 16 || entriesEnd + h.strings > nsp.size()) return false;
    const std::size_t dataStart = entriesEnd + h.strings;
    std::filesystem::create_directories(kHblDir, ec);
    bool gotMain = false, gotNpdm = false;
    for (u32 i = 0; i < h.count; ++i) {
        Entry e;
        std::memcpy(&e, nsp.data() + sizeof(Header) + i * sizeof(Entry), sizeof(e));
        if (e.name >= h.strings || dataStart + e.offset + e.size > nsp.size()) return false;
        const char* name = nsp.data() + entriesEnd + e.name;
        const char* stringsEnd = nsp.data() + entriesEnd + h.strings;
        const std::string n(name, std::find(name, stringsEnd, '\0'));
        if (n != "main" && n != "main.npdm") continue;
        std::vector<char> data(nsp.begin() + dataStart + e.offset,
                               nsp.begin() + dataStart + e.offset + e.size);
        if (n == "main.npdm" && !patchHblNpdmAsApplication(data)) return false;
        std::ofstream(std::string(kHblDir) + "/" + n, std::ios::binary | std::ios::trunc)
            .write(data.data(), static_cast<std::streamsize>(data.size()));
        (n == "main" ? gotMain : gotNpdm) = true;
    }
    if (gotMain && gotNpdm) std::ofstream(stampPath) << size;
    DebugLog::log("[launcher] hbl unpacked main=%d npdm=%d", gotMain, gotNpdm);
    return gotMain && gotNpdm;
}

// Application mode when we can borrow a game; the Album (applet) otherwise.
// applicationOnly (Nextendo): never the Album; false if it can't start that way.
static bool startHomebrew(AppletLauncher& self, uint64_t hostTitleId, AccountUid uid,
                          const AppletLauncher::Callbacks& cb, bool applicationOnly = false) {
    std::error_code ec;
    // A fresh session: the Loader ends the process when it's started again in the
    // one it recorded here (the homebrew was closed). See projects/loader.
    std::filesystem::remove("sdmc:/config/SwitchU/hbl_chained.txt", ec);
    // Tells the daemon not to fall back to the Album if this launch fails.
    constexpr const char* kApplicationOnly = "sdmc:/config/SwitchU/homebrew_application_only";
    if (applicationOnly) {
        if (FILE* f = std::fopen(kApplicationOnly, "w")) std::fclose(f);
    } else {
        std::filesystem::remove(kApplicationOnly, ec);
    }
    // The daemon drops this flag once application mode fails on this console.
    const bool appletOnly = !applicationOnly
        && std::filesystem::exists("sdmc:/switch/SwitchU/homebrew_applet_mode", ec);
    if (!appletOnly && hostTitleId != 0 && std::filesystem::exists(std::string(kHblDir) + "/main", ec)) {
        const Result rc = switchu::menu::smi_cmd::launchHomebrewApp(uid, hostTitleId);
        DebugLog::log("[launcher] homebrew as application via %016lX rc=0x%X", hostTitleId, rc);
        if (R_SUCCEEDED(rc)) {
            if (cb.requestExit) cb.requestExit();
            return true;
        }
    }
    if (applicationOnly) {
        std::filesystem::remove(kApplicationOnly, ec);
        DebugLog::log("[launcher] application mode unavailable (host=%016lX); not using the Album",
                      static_cast<unsigned long>(hostTitleId));
        return false;
    }
    self.launchAlbum();
    return true;
}

bool AppletLauncher::launchSphaira(uint64_t hostTitleId, AccountUid uid) {
    std::remove(kLoaderNext); // no request: the Loader opens Sphaira
    if (!installLoader()) return false;
    startHomebrew(*this, hostTitleId, uid, m_cb);
    return true;
}

bool AppletLauncher::launchNro(const std::string& path, uint64_t hostTitleId, AccountUid uid,
                               bool applicationOnly) {
    if (!installLoader()) return false;
    writeLine(kLoaderNext, path);
    return startHomebrew(*this, hostTitleId, uid, m_cb, applicationOnly);
}

Result AppletLauncher::createMii() { launchMiiEditor(); return 0; }
// The daemon runs the friends applet (MyPage) on the requested screen.
Result AppletLauncher::openFriendScreen(FriendScreen screen, AccountUid uid) {
    const uint32_t type = screen == FriendScreen::AddFriend
        ? FriendsLaArgType_ShowMethodsOfSendingFriendRequest
        : FriendsLaArgType_ShowReceivedFriendRequestList;
    Result rc = switchu::menu::smi_cmd::launchFriendsScreen(uid, type);
    DebugLog::log("[launcher] friends screen rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
    return rc;
}
void AppletLauncher::findControllers() {}
Result AppletLauncher::launchEshop(AccountUid uid) {
    Result rc = switchu::menu::smi_cmd::launchShop(uid);
    DebugLog::log("[launcher] Shop rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
    return rc;
}
Result AppletLauncher::launchBrowser(AccountUid, const char*) { return 0; }
Result AppletLauncher::launchFriends(AccountUid) { return 0; }

#else

bool AppletLauncher::isAppRunning() const            { return false; }
bool AppletLauncher::appHasForeground() const        { return false; }
bool AppletLauncher::isAppSuspended(uint64_t) const  { return false; }
uint64_t AppletLauncher::suspendedTitleId() const    { return 0; }
void AppletLauncher::setAppRunning(bool)             {}
void AppletLauncher::setAppHasForeground(bool)       {}
void AppletLauncher::setSuspendedTitleId(uint64_t)   {}

// Homebrew .nro: open the system applets directly. Each call blocks until the
// applet closes. The web-based ones need the .nro launched over a game
// (title override); in applet/album mode they fail and only get logged.
namespace {

void logRc(const char* what, Result rc) {
    DebugLog::log("[launcher] %s rc=0x%X", what, rc);
}

AccountUid currentUser() {
    AccountUid uid{};
    if (R_FAILED(accountGetPreselectedUser(&uid)) || !accountUidIsValid(&uid))
        if (R_FAILED(accountTrySelectUserWithoutInteraction(&uid, false)) || !accountUidIsValid(&uid))
            accountGetLastOpenedUser(&uid);
    return uid;
}

AccountUid orCurrent(AccountUid uid) {
    return accountUidIsValid(&uid) ? uid : currentUser();
}

// The web-applet setup Sphaira uses (sphaira/source/web.cpp), which is known to
// work on current firmware: open whitelist, transfer memory, the user's uid.
Result showWeb(const char* url, AccountUid uid) {
    WebCommonConfig cfg{};
    WebCommonReply reply{};
    Result rc = webPageCreate(&cfg, url);
    if (R_FAILED(rc)) return rc;
    webConfigSetWhitelist(&cfg, ".*");
    webConfigSetEcClientCert(&cfg, true);
    webConfigSetScreenShot(&cfg, true);
    webConfigSetBootDisplayKind(&cfg, WebBootDisplayKind_Black);
    webConfigSetBackgroundKind(&cfg, WebBackgroundKind_Default);
    webConfigSetPointer(&cfg, true);
    webConfigSetLeftStickMode(&cfg, WebLeftStickMode_Pointer);
    webConfigSetBootAsMediaPlayer(&cfg, false);
    webConfigSetJsExtension(&cfg, true);
    webConfigSetMediaPlayerAutoClose(&cfg, false);
    webConfigSetPageCache(&cfg, true);
    webConfigSetFooterFixedKind(&cfg, WebFooterFixedKind_Default);
    webConfigSetPageFade(&cfg, true);
    webConfigSetPageScrollIndicator(&cfg, true);
    webConfigSetBootMode(&cfg, WebSessionBootMode_AllForeground);
    webConfigSetTransferMemory(&cfg, true);
    webConfigSetTouchEnabledOnContents(&cfg, true);
    webConfigSetWebAudio(&cfg, false);
    uid = orCurrent(uid);
    if (accountUidIsValid(&uid)) webConfigSetUid(&cfg, uid);
    return webConfigShow(&cfg, &reply);
}

} // namespace

void AppletLauncher::launchAlbum()     { logRc("album", albumLaShowAllAlbumFiles()); }

// Hand the process to Sphaira through hbloader, so it keeps HomeNX's mode
// (application mode when HomeNX was started over a game).
bool AppletLauncher::launchNro(const std::string& path, uint64_t, AccountUid, bool) {
    std::string args = "\"" + path + "\"";
    Result rc = envSetNextLoad(path.c_str(), args.c_str());
    logRc(path.c_str(), rc);
    if (R_FAILED(rc)) return false;
    if (m_cb.requestExit) m_cb.requestExit();
    return true;
}

bool AppletLauncher::installLoader() { return true; }
const std::string& AppletLauncher::loaderError() { static const std::string none; return none; }
void AppletLauncher::installBundledApps() {}
bool AppletLauncher::prepareHbl() { return false; }

// HomeNX.nro carries Sphaira in its romfs; copy it to the SD once (an .nro
// has to be a real file for hbloader), then use the user's own as a fallback.
bool AppletLauncher::launchSphaira(uint64_t, AccountUid) {
    const char* bundled = "sdmc:/switch/SwitchU/bin/sphaira.nro";
    std::error_code ec;
    if (std::filesystem::exists("romfs:/bin/sphaira.nro", ec)
        && (!std::filesystem::exists(bundled, ec)
            || std::filesystem::file_size(bundled, ec) != std::filesystem::file_size("romfs:/bin/sphaira.nro", ec))) {
        std::filesystem::create_directories("sdmc:/switch/SwitchU/bin", ec);
        std::filesystem::copy_file("romfs:/bin/sphaira.nro", bundled,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (std::filesystem::exists(bundled, ec)) return launchNro(bundled);
    const std::string path = findSphaira(true);
    if (path.empty()) {
        DebugLog::log("[launcher] sphaira not found");
        return false;
    }
    return launchNro(path);
}

Result AppletLauncher::createMii() {
    s32 index = 0;
    Result rc = miiLaAppendMii(MiiSpecialKeyCode_Normal, &index);
    logRc("mii-create", rc);
    return rc;
}

Result AppletLauncher::openFriendScreen(FriendScreen screen, AccountUid uid) {
    uid = orCurrent(uid);
    Result rc = screen == FriendScreen::AddFriend
        ? friendsLaShowMethodsOfSendingFriendRequest(uid)
        : friendsLaShowReceivedFriendRequestList(uid);
    logRc("friends-screen", rc);
    return rc;
}

void AppletLauncher::launchMiiEditor() { logRc("mii", miiLaShowMiiEdit(MiiSpecialKeyCode_Normal)); }
void AppletLauncher::launchControllerPairing() {
    HidLaControllerSupportArg arg;
    hidLaCreateControllerSupportArg(&arg);
    logRc("controllers", hidLaShowControllerSupportForSystem(nullptr, &arg, false));
}

Result AppletLauncher::launchEshop(AccountUid uid) {
    // ponytail: ShopN takes the web TLV args with ShimKind_Shop (switchbrew
    // "Internet Browser"); untested on hardware, so fall back to the web store.
    uid = orCurrent(uid);
    WebCommonConfig cfg{};
    Result rc = webPageCreate(&cfg, "https://bugyo.hac.lp1.eshop.nintendo.net/ashigaru/");
    if (R_SUCCEEDED(rc)) {
        if (accountUidIsValid(&uid)) webConfigSetUid(&cfg, uid);
        reinterpret_cast<WebArgHeader*>(cfg.arg.data)->shimKind = WebShimKind_Shop;
        cfg.appletid = AppletId_LibraryAppletShop;
        rc = webConfigShow(&cfg, nullptr);
    }
    logRc("eshop", rc);
    if (R_SUCCEEDED(rc)) return rc;
    rc = showWeb("https://www.nintendo.com/store/", uid);
    logRc("eshop-web", rc);
    return rc;
}

// Dark DuckDuckGo start page: black background suits the OLED panel.
Result AppletLauncher::launchBrowser(AccountUid uid, const char* url) {
    Result rc = showWeb(url ? url : "https://duckduckgo.com/?kae=d&kak=-1&kax=-1&kaq=-1", uid);
    logRc("browser", rc);
    return rc;
}

Result AppletLauncher::launchFriends(AccountUid uid) {
    uid = orCurrent(uid);
    if (!accountUidIsValid(&uid)) {
        PselUserSelectionSettings sel{};
        Result rc = pselShowUserSelector(&uid, &sel);
        if (R_FAILED(rc)) return rc;
    }
    Result rc = friendsLaShowFriendList(uid);
    logRc("friends", rc);
    return rc;
}

void AppletLauncher::launchControllerRemapping() {
    HidLaControllerKeyRemappingArg arg;
    hidLaCreateControllerKeyRemappingArg(&arg);
    logRc("remap", hidLaShowControllerKeyRemappingForSystem(&arg, HidLaControllerSupportCaller_System));
}

// Three short rumbles on every connected controller, like the system's
// "Find Controllers".
void AppletLauncher::findControllers() {
    static const HidNpadIdType ids[] = {
        HidNpadIdType_Handheld, HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3,
        HidNpadIdType_No4, HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8,
    };
    HidVibrationDeviceHandle handles[18];
    int count = 0;
    for (HidNpadIdType id : ids) {
        const u32 styles = hidGetNpadStyleSet(id);
        HidNpadStyleTag style;
        int n = 2;
        if (styles & HidNpadStyleTag_NpadHandheld) style = HidNpadStyleTag_NpadHandheld;
        else if (styles & HidNpadStyleTag_NpadFullKey) style = HidNpadStyleTag_NpadFullKey;
        else if (styles & HidNpadStyleTag_NpadJoyDual) style = HidNpadStyleTag_NpadJoyDual;
        else if (styles & HidNpadStyleTag_NpadJoyLeft) { style = HidNpadStyleTag_NpadJoyLeft; n = 1; }
        else if (styles & HidNpadStyleTag_NpadJoyRight) { style = HidNpadStyleTag_NpadJoyRight; n = 1; }
        else continue;
        if (R_SUCCEEDED(hidInitializeVibrationDevices(handles + count, n, id, style)))
            count += n;
    }
    DebugLog::log("[launcher] find controllers: %d motors", count);
    if (count == 0) return;
    HidVibrationValue on[18], off[18];
    for (int i = 0; i < count; ++i) {
        on[i] = {0.7f, 160.f, 0.7f, 320.f};
        off[i] = {0.f, 160.f, 0.f, 320.f};
    }
    // ponytail: blocks the UI ~1 s; fine for a button you press on purpose.
    for (int pulse = 0; pulse < 3; ++pulse) {
        hidSendVibrationValues(handles, on, count);
        svcSleepThread(220'000'000ull);
        hidSendVibrationValues(handles, off, count);
        svcSleepThread(140'000'000ull);
    }
}

void AppletLauncher::launchNetConnect()        {}
void AppletLauncher::launchUserPage(AccountUid) {}
void AppletLauncher::launchUserCreator()       {}
void AppletLauncher::enterSleep()              {}
void AppletLauncher::shutdown()                {}
void AppletLauncher::reboot()                  {}
bool AppletLauncher::requestLaunch(uint64_t titleId, AccountUid) {
    // The system launches it after HomeNX exits. It can't carry a user, so
    // titles that need one get the system's own picker if it wants one.
    Result rc = appletRequestLaunchApplication(titleId, nullptr);
    DebugLog::log("[launcher] request launch %016lX rc=0x%X", titleId, rc);
    return R_SUCCEEDED(rc);
}
void AppletLauncher::closeForLaunch() {
    if (m_cb.requestExit) m_cb.requestExit();
}
void AppletLauncher::resumeApplication()       {}
void AppletLauncher::terminateApplication()    {}
void AppletLauncher::checkRunningApplication() {}

#endif

// HomeNX: restarting into hekate. Atmosphère boots a payload it's handed
// (bpc:ams) instead of the system on the next restart: hekate's own copy in
// bootloader/, else Atmosphère's reboot payload (usually hekate too).
std::string AppletLauncher::hekatePayload() {
    for (const char* path : {"sdmc:/bootloader/update.bin", "sdmc:/atmosphere/reboot_payload.bin"}) {
        struct stat st{};
        if (stat(path, &st) == 0 && st.st_size > 0 && st.st_size <= 0x24000) return path;
    }
    return {};
}

bool AppletLauncher::rebootToHekate() {
    static constexpr size_t kPayloadMax = 0x24000; // what fits in IRAM
    alignas(0x1000) static u8 payload[kPayloadMax];
    const std::string path = hekatePayload();
    if (path.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::memset(payload, 0, sizeof(payload));
    const size_t got = std::fread(payload, 1, sizeof(payload), f);
    std::fclose(f);
    if (got == 0) return false;
    // Atmosphère's own port (not in libnx): command 65001 takes the payload.
    Handle port = 0;
    Result rc = svcConnectToNamedPort(&port, "bpc:ams");
    if (R_SUCCEEDED(rc)) {
        Service bpcAms;
        serviceCreate(&bpcAms, port);
        rc = serviceDispatch(&bpcAms, 65001,
            .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
            .buffers = { { payload, sizeof(payload) } },
        );
        serviceClose(&bpcAms);
    }
    DebugLog::log("[launcher] hekate payload %s (%u bytes) rc=0x%X", path.c_str(), (unsigned)got, rc);
    if (R_FAILED(rc)) return false;
#ifdef SWITCHU_MENU
    reboot();
#else
    if (R_SUCCEEDED(spsmInitialize())) {
        spsmShutdown(true);
        spsmExit();
    }
#endif
    return true;
}
