#include <switchu/recovery.hpp>
#include <switchu/diagnostic.hpp>
namespace diag = switchu::diagnostic;

#include "core/WiiUMenuApp.hpp"
#include "core/DebugLog.hpp"
#include "core/Config.hpp"
#include "tutorial/TutorialActivity.hpp"
#include <nxui/Application.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <fmt/format.h>
#ifdef SWITCHU_MENU
#include "CardsActivity.hpp"
#include "smi_commands.hpp"
#include <nxui/core/Renderer.hpp>
#include <switchu/smi_protocol.hpp>
#include <switchu/file_log.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#endif
#include <switch.h>
#include <switchu/time_budget.hpp>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#ifdef SWITCHU_MENU
#include <cstring>
#endif
#include <memory>

namespace {
constexpr size_t kMenuAppletHeapSize = 224u * 1024u * 1024u;
}

extern "C" {
#ifdef SWITCHU_HOMEBREW
    u32 __nx_applet_type = AppletType_Application;

    size_t __nx_heap_size = 0xD000000;
#else
    u32 __nx_applet_type = AppletType_LibraryApplet;
    u32 __nx_fs_num_sessions = 1;

    size_t __nx_heap_size = kMenuAppletHeapSize;

    TimeServiceType __nx_time_service_type = TimeServiceType_Menu;

    void __libnx_init_time(void);
#endif
}

#ifndef SWITCHU_HOMEBREW
namespace {
constinit Result lastHeapFailure{};
constexpr size_t kHeapLadder[] = {
    416u * 1024u * 1024u,
    400u * 1024u * 1024u,
    384u * 1024u * 1024u,
    368u * 1024u * 1024u,
    352u * 1024u * 1024u,
    320u * 1024u * 1024u,
    288u * 1024u * 1024u,
    256u * 1024u * 1024u,
    kMenuAppletHeapSize,
    // With a title suspended the applet pool can be short (Spotify playing in
    // a game): with no heap at all the menu can't even open its log, and
    // closes itself to a black screen. Peak use is ~50-80 MiB.
    192u * 1024u * 1024u,
    160u * 1024u * 1024u,
    128u * 1024u * 1024u,
    96u * 1024u * 1024u,
};
}

extern "C" size_t g_switchuHeapSize = 0;

extern "C" void __libnx_initheap(void) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;

    void* address = nullptr;
    size_t granted = 0;
    // 64 MiB of the applet pool stay free (cristowo/SwitchU's fix): a sysmodule
    // in the applet pool, Switify's service, takes its memory from what this
    // limit says is free. With the 416 MiB rung taken, Spotify playing left
    // 124 KiB: every picture was refused (the covers went) and HOME over a game
    // couldn't make its first texture (a black screen, 2026-11-10).
    size_t available = 0;
    u64 total = 0, used = 0;
    if (R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) &&
        R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)) && total > used)
        available = static_cast<size_t>(total - used);
    constexpr size_t kHeadroom = 64u * 1024u * 1024u;
    const size_t ceiling = available > kHeadroom ? available - kHeadroom : 0;
    for (size_t requested : kHeapLadder) {
        if (ceiling != 0 && requested > ceiling && requested > kHeapLadder[std::size(kHeapLadder) - 1]) continue;
        const Result rc = svcSetHeapSize(&address, requested);
        if (R_SUCCEEDED(rc)) {
            granted = requested;
            break;
        }
        lastHeapFailure = rc;
    }

    g_switchuHeapSize = granted;
    fake_heap_start = static_cast<char*>(address);
    fake_heap_end = static_cast<char*>(address) + granted;
}
#endif

#ifdef SWITCHU_HOMEBREW
extern "C" void userAppInit(void) {
    timeInitialize();
    plInitialize(PlServiceType_System);
    setInitialize();
    setsysInitialize();
    accountInitialize(AccountServiceType_Application);
    psmInitialize();
    lblInitialize();
    nsInitialize();
    romfsInit();
}

extern "C" void userAppExit(void) {
    nsExit();
    lblExit();
    psmExit();
    accountExit();
    setsysExit();
    setExit();
    plExit();
    timeExit();
    romfsExit();
}
#else
namespace {

ViDisplay g_menuDisplay{};
ViLayer g_menuLayer{};
NWindow g_menuWindow{};

void initializeMenuWindow() {
    auto step = [](const char* name, auto call) {
        diag::begin("VI", name);
        return diag::end("VI", "END", call());
    };
    Result rc = step("viInitialize BEGIN", [] { return viInitialize(ViServiceType_Default); });
    if (R_SUCCEEDED(rc)) rc = step("viOpenDefaultDisplay BEGIN", [] { return viOpenDefaultDisplay(&g_menuDisplay); });
    if (R_SUCCEEDED(rc)) rc = step("viCreateLayer BEGIN", [] { return viCreateLayer(&g_menuDisplay, &g_menuLayer); });
    if (R_SUCCEEDED(rc)) {
        rc = step("VI preserve aspect BEGIN", [] { return viSetLayerScalingMode(&g_menuLayer, ViScalingMode_PreserveAspectRatio); });
        if (R_FAILED(rc))
            rc = step("VI fit to layer BEGIN", [] { return viSetLayerScalingMode(&g_menuLayer, ViScalingMode_FitToLayer); });
    }
    if (R_SUCCEEDED(rc)) rc = step("nwindowCreateFromLayer BEGIN", [] { return nwindowCreateFromLayer(&g_menuWindow, &g_menuLayer); });
    if (R_FAILED(rc)) {
        diag::stop();
        diagAbortWithResult(rc);
    }
    nxui::GpuDevice::setWindow(&g_menuWindow);
}

} // namespace

extern "C" void __appInit(void) {
    diag::origin = armGetSystemTick();
    diag::component = "menu";
    diag::begin("BOOT", "__appInit BEGIN");
    if (lastHeapFailure)
        diag::log(g_switchuHeapSize ? diag::Level::Warn : diag::Level::Error, "MEM",
                  g_switchuHeapSize ? "Heap request rejected before fallback" : "All heap requests rejected",
                  lastHeapFailure);
    Result rc;
    g_homenxStartTick = armGetSystemTick();

    svcOutputDebugString("[SwitchU-menu] __appInit start", 30);

    diag::begin("IPC", "smInitialize BEGIN");
    rc = diag::end("IPC", "smInitialize END", smInitialize());
    if (R_FAILED(rc)) diagAbortWithResult(rc);

    diag::begin("IPC", "fsInitialize BEGIN");
    rc = diag::end("IPC", "fsInitialize END", fsInitialize());
    if (R_FAILED(rc)) diagAbortWithResult(rc);

    diag::begin("IPC", "appletInitialize BEGIN");
    rc = diag::end("IPC", "appletInitialize END", appletInitialize());
    if (R_FAILED(rc)) {
        svcOutputDebugString("[SwitchU-menu] appletInitialize FAIL", 37);
        diagAbortWithResult(rc);
    }
    svcOutputDebugString("[SwitchU-menu] appletInitialize OK", 34);

    diag::begin("IPC", "hidInitialize BEGIN");
    rc = diag::end("IPC", "hidInitialize END", hidInitialize());
    if (R_FAILED(rc)) svcOutputDebugString("[SwitchU-menu] hidInitialize FAIL", 33);

    diag::begin("IPC", "setsysInitialize BEGIN");
    diag::end("IPC", "setsysInitialize END", setsysInitialize());
    diag::begin("IPC", "setInitialize BEGIN");
    diag::end("IPC", "setInitialize END", setInitialize());
    diag::begin("IPC", "plInitialize BEGIN");
    diag::end("IPC", "plInitialize END", plInitialize(PlServiceType_System));
    diag::begin("IPC", "psmInitialize BEGIN");
    diag::end("IPC", "psmInitialize END", psmInitialize());
    diag::begin("IPC", "lblInitialize BEGIN");
    diag::end("IPC", "lblInitialize END", lblInitialize());
    diag::begin("IPC", "splInitialize BEGIN");
    diag::end("IPC", "splInitialize END", splInitialize());
    diag::begin("IPC", "accountInitialize BEGIN");
    diag::end("IPC", "accountInitialize END", accountInitialize(AccountServiceType_System));

    {
        SetSysFirmwareVersion fw = {};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro) | BIT(31));
    }
    // Not timeInitialize(): the time service rarely has its 6 sessions free for HOME (see switchu/time_budget.hpp).
    // ClockService tries again if it fails here.
    switchu::timebudget::poll();

    diag::begin("FS", "SD mount BEGIN");
    rc = diag::end("FS", "SD mount END", fsdevMountSdmc());
    if (R_FAILED(rc)) {
        svcOutputDebugString("[SwitchU-menu] fsdevMountSdmc FAIL, retry", 42);
        svcSleepThread(100'000'000ULL);
        rc = diag::end("FS", "SD mount END", fsdevMountSdmc());
    }

    if (R_SUCCEEDED(rc))
        diag::start("sdmc:/config/SwitchU/diagnostic-menu.log",
                    "sdmc:/config/SwitchU/diagnostic-menu.old", &nxui::g_phase, &nxui::g_phaseSerial);
    switchu::FileLog::open("menu");
    DebugLog::openFileLog();
    DebugLog::log("[menu] __appInit complete (sd mount: 0x%X)", rc);

    diag::begin("IPC", "nsInitialize BEGIN");
    rc = diag::end("IPC", "nsInitialize END", nsInitialize());
    if (R_FAILED(rc))
        DebugLog::log("[menu] nsInitialize FAILED: 0x%X", rc);
    else
        DebugLog::log("[menu] nsInitialize OK");

    initializeMenuWindow();
    DebugLog::log("[menu] display window initialized");

    svcOutputDebugString("[SwitchU-menu] __appInit done", 29);
}

extern "C" void __appExit(void) {
    DebugLog::closeFileLog();
    switchu::FileLog::close();

    nwindowClose(&g_menuWindow);
    viCloseLayer(&g_menuLayer);
    viCloseDisplay(&g_menuDisplay);
    viExit();

    nsExit();
    accountExit();
    splExit();
    lblExit();
    psmExit();
    plExit();

    hidExit();
    appletExit();

    setExit();
    setsysExit();
    timeExit();

    diag::stop();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

// Settings: TV resolution (config/SwitchU/resolution.txt "1080": 1920x1080 when
// docked; handheld and otherwise 1280x720). A 1080p start that never put a frame
// on screen (its marker is still there next time) falls back to 720p and turns
// the setting off, so HOME can't stay black.
static constexpr const char* kResolutionFile = "sdmc:/config/SwitchU/resolution.txt";
static constexpr const char* kResolutionTry = "sdmc:/config/SwitchU/resolution.try";

static bool displaySupports1080p() {
    s32 width = 0, height = 0;
    return appletGetOperationMode() == AppletOperationMode_Console
        && R_SUCCEEDED(appletGetDefaultDisplayResolution(&width, &height))
        && width >= 1920 && height >= 1080;
}

static bool chooseResolution() {
    std::string want;
    std::getline(std::ifstream(kResolutionFile), want);
    if (want.rfind("1080", 0) != 0) return false;
    std::error_code ec;
    if (std::filesystem::exists(kResolutionTry, ec)) {
        std::filesystem::remove(kResolutionTry, ec);
        std::filesystem::remove(kResolutionFile, ec);
        DebugLog::log("[menu] 1080p never showed a frame last time: 720p, and the setting is off");
        return false;
    }
    if (!displaySupports1080p()) return true;
    std::ofstream(kResolutionTry) << "1";
    nxui::GpuDevice::setResolution(1920, 1080);
    nxui::GpuDevice::setFirstFrameHook([] {
        std::error_code err;
        std::filesystem::remove(kResolutionTry, err);
    });
    DebugLog::log("[menu] 1920x1080 (on the TV)");
    return true;
}

struct DisplayModeHook {
    nxui::Application& app;
    bool prefer1080p;
};

static void onDisplayMode(AppletHookType type, void* context) {
    if (type == AppletHookType_OnFocusState) {
        diag::log(diag::Level::Info, "APPLET", appletGetFocusState() == AppletFocusState_InFocus
            ? "Focus gained" : "Focus lost");
        return;
    }
    if (type != AppletHookType_OnOperationMode && type != AppletHookType_OnResume)
        return;
    diag::log(diag::Level::Info, "APPLET", type == AppletHookType_OnResume ? "Resume" : "Operation mode changed");
    const auto& hook = *static_cast<DisplayModeHook*>(context);
    auto& app = hook.app;
    app.requestRedraw();
    if (!hook.prefer1080p || !app.isRunning()
        || (app.gpu().height() == 1080) == displaySupports1080p())
        return;
    if (R_SUCCEEDED(switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::RestartMenuForDisplay)))
        app.requestExit();
}

static switchu::smi::MenuStartMode readStartMode() {
    LibAppletArgs args{};
    AppletStorage stor{};
    if (R_SUCCEEDED(appletPopInData(&stor))) {
        s64 sz = 0;
        appletStorageGetSize(&stor, &sz);
        if (sz >= (s64)sizeof(LibAppletArgs)) {
            appletStorageRead(&stor, 0, &args, sizeof(args));
        }
        appletStorageClose(&stor);
    }
    return static_cast<switchu::smi::MenuStartMode>(args.LaVersion);
}

static switchu::smi::SystemStatus readSystemStatus() {
    switchu::smi::SystemStatus status{};
    AppletStorage stor{};
    if (R_SUCCEEDED(appletPopInData(&stor))) {
        s64 sz = 0;
        appletStorageGetSize(&stor, &sz);
        // An older daemon (right after an update, before a restart) sends less.
        if (sz >= 56)
            appletStorageRead(&stor, 0, &status, std::min<size_t>((size_t)sz, sizeof(status)));
        appletStorageClose(&stor);
    }
    return status;
}
#endif

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    std::srand(static_cast<unsigned>(std::time(nullptr)));

#ifdef SWITCHU_HOMEBREW
    g_homenxStartTick = armGetSystemTick();
    DebugLog::openFileLog();
    DebugLog::log("[hb] main() entry");
    DebugLog::log("[main] applet config...");
#else
    DebugLog::log("[menu] main() entry");
    {
        u64 total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        DebugLog::log("[menu] heap %zu MiB; applet pool free after it %llu MiB", g_switchuHeapSize >> 20,
                      static_cast<unsigned long long>(total > used ? (total - used) >> 20 : 0));
    }

    char environment[140];
    const u32 firmware = hosversionGet();
    std::snprintf(environment, sizeof(environment), "%s git=%s HOS=%u.%u.%u heap=%zu MiB",
                  HOMENX_BUILD, HOMENX_GIT, (firmware >> 16) & 255,
                  (firmware >> 8) & 255, firmware & 255, g_switchuHeapSize >> 20);
    diag::log(diag::Level::Info, "BOOT", environment);
    u64 atmosphere{};
    const Result atmosphereRc = splGetConfig(static_cast<SplConfigItem>(65000), &atmosphere);
    if (R_SUCCEEDED(atmosphereRc)) {
        std::snprintf(environment, sizeof(environment), "Atmosphere %u.%u.%u",
                      static_cast<unsigned>((atmosphere >> 56) & 255),
                      static_cast<unsigned>((atmosphere >> 48) & 255),
                      static_cast<unsigned>((atmosphere >> 40) & 255));
        diag::log(diag::Level::Info, "BOOT", environment);
    } else {
        diag::log(diag::Level::Warn, "BOOT", "Atmosphere version unavailable", atmosphereRc);
    }
    u64 total{}, used{};
    Result totalRc = svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    Result usedRc = svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (R_SUCCEEDED(totalRc) && R_SUCCEEDED(usedRc)) {
        std::snprintf(environment, sizeof(environment), "Process memory limit=%llu used=%llu bytes",
                      static_cast<unsigned long long>(total), static_cast<unsigned long long>(used));
        diag::log(diag::Level::Info, "MEM", environment);
    } else {
        diag::end("MEM", "Memory limit query END", totalRc);
        diag::end("MEM", "Memory used query END", usedRc);
    }
    diag::begin("BOOT", "Read startup applet storage BEGIN");
    auto startMode = readStartMode();
    auto sysStatus = readSystemStatus();
    diag::log(diag::Level::Info, "BOOT", "Read startup applet storage END");

    // HomeNX housekeeping only when the console boots, not on every HOME press
    // (returning from a game has to be quick): finish an update (files that were
    // open while it ran, before any font is opened), put the Loader in place as
    // sdmc:/hbmenu.nro and copy the shipped Toggle. The Sphaira button re-checks
    // the Loader itself.
    if (!switchu::recovery::enabled() && startMode == switchu::smi::MenuStartMode::StartupBoot) {
        diag::begin("BOOT", "Apply pending update and prepare loader BEGIN");
        switchu::manager::ReleaseUpdater::applyPending();
        AppletLauncher::restoreHbmenu(); // (the Loader goes in only to start homebrew)
        AppletLauncher::installBundledApps();
        AppletLauncher::prepareHbl();
        diag::log(diag::Level::Info, "BOOT", "Apply pending update and prepare loader END");
    }

    DebugLog::log("[menu] start mode=%d  suspended=0x%016lX running=%d",
                  (int)startMode, sysStatus.suspended_app_id, sysStatus.app_running);

    {
        std::string sdPath = fmt::format("{}/shaders/", SD_ASSETS);
        nxui::Renderer::setShaderBasePath(sdPath);
        DebugLog::log("[menu] shader path: %s", sdPath.c_str());
    }
    diag::begin("VI", "Select display resolution BEGIN");
    const bool recovery = switchu::recovery::enabled();
    if (recovery) diag::log(diag::Level::Info, "BOOT", "Recovery mode active");
    const bool prefer1080p = !recovery && chooseResolution();
    diag::log(diag::Level::Info, "VI", "Select display resolution END");
#endif

#ifndef SWITCHU_HOMEBREW
    nxui::Application::setDiagnosticSink([](const char* phase, bool input) {
        if (input)
            diag::log(std::strncmp(phase, "A pressed", 9) == 0 ? diag::Level::Debug : diag::Level::Trace,
                      "INPUT", phase);
        else diag::begin("UI", phase);
    });
    diag::begin("BOOT", "SDL and font initialization BEGIN");
#endif
    nxui::GpuDevice::setLogSink([](const char* msg) {
#ifndef SWITCHU_HOMEBREW
        diag::log(diag::Level::Warn, "GPU", msg);
#endif
        DebugLog::log("%s", msg);
    });

    // The audio device opens with the music, well after HOME's first frame:
    // opening it here was part of the freeze before the first frame every time
    // HOME came back from a game (Audio::start brings SDL's audio up).
    DebugLog::log("[main] SDL_Init");
    if (SDL_Init(0) < 0)
        DebugLog::log("[main] SDL_Init FAILED: %s", SDL_GetError());

    DebugLog::log("[main] TTF_Init");
    if (TTF_Init() < 0)
        DebugLog::log("[main] TTF_Init FAILED: %s", TTF_GetError());

    DebugLog::log("[main] creating app...");
    {
        nxui::Application app;
#ifdef SWITCHU_HOMEBREW
        auto makeMenuActivity = [](bool fromTutorial = false) -> std::unique_ptr<nxui::Activity> {
            auto activity = std::make_unique<WiiUMenuApp>();
            activity->setTutorialStartupFade(fromTutorial);
            return activity;
        };
        AppConfig startupConfig;
        startupConfig.load();
        if (startupConfig.tutorialCompleted)
            app.setActivity(makeMenuActivity(false));
        else
            app.setActivity(std::make_unique<TutorialActivity>(makeMenuActivity));
        DebugLog::log("[hb] app.initialize...");
        if (app.initialize()) {
            DebugLog::log("[hb] app.run...");
            app.run();
        } else {
            DebugLog::log("[hb] app.initialize FAILED");
        }
        DebugLog::log("[hb] app.shutdown...");
        app.shutdown();
#else
        bool cardsFallback = false; // set below; the lambda reads it when it builds the activity
        auto makeMenuActivity = [sysStatus, startMode, &cardsFallback](bool fromTutorial = false) -> std::unique_ptr<nxui::Activity> {
            auto activity = std::make_unique<WiiUMenuApp>();
            activity->setCardsFallbackNotice(cardsFallback);
            activity->setStartupStatus(sysStatus,
                startMode == switchu::smi::MenuStartMode::AppletReturn);
            activity->setBootStart(startMode == switchu::smi::MenuStartMode::StartupBoot);
            activity->setWakeStart(startMode == switchu::smi::MenuStartMode::WakeUp);
            activity->setTutorialStartupFade(fromTutorial);
            return activity;
        };
        AppConfig startupConfig;
        diag::begin("CONFIG", "Load startup settings BEGIN");
        const bool configLoaded = startupConfig.load();
        diag::log(configLoaded ? diag::Level::Info : diag::Level::Warn, "CONFIG",
                  configLoaded ? "Load startup settings END" : "No readable settings, using defaults");
        diag::begin("UI", "Select and construct activity BEGIN");
        // NXToggle's Home screen choice: CardsNX's card row instead of HomeNX's.
        std::string homeStyle;
        std::getline(std::ifstream("sdmc:/config/SwitchU/frontend.txt"), homeStyle);
        // A CardsNX start that crashes is followed by a safe one; if that crashes too, HomeNX's
        // HOME until the console restarts, not a black screen that flickers for ever.
        // (The count is cleared once CardsNX has run a moment.)
        bool useCards = !recovery && homeStyle.rfind("cards", 0) == 0;
        if (useCards) {
            const bool atBoot = startMode == switchu::smi::MenuStartMode::StartupBoot;
            std::error_code ec;
            int starts = 0;
            std::ifstream(cards::kCardsStarting) >> starts;
            if (atBoot) {
                starts = 0;
                std::filesystem::remove(cards::kCardsSafe, ec);
            }
            useCards = starts < 2;
            if (useCards) {
                std::ofstream(cards::kCardsStarting) << starts + 1;
                cards::g_safeStart = starts > 0 || std::filesystem::exists(cards::kCardsSafe, ec);
                if (atBoot || starts > 0)
                    cards::startLogBegin(atBoot, fmt::format("=== start {} ({}), mode {} ===", starts + 1,
                                                             cards::g_safeStart ? "safe" : "normal", static_cast<u32>(startMode)));
            } else {
                cards::startLogBegin(false, "=== the safe start crashed too: HomeNX's HOME instead ===");
                cards::startLogEnd();
                // Tell the user once: the counter goes past 2, so later starts stay on HomeNX's HOME silently.
                cardsFallback = (starts == 2);
                if (cardsFallback) std::ofstream(cards::kCardsStarting) << starts + 1;
                DebugLog::log("[menu] CardsNX stopped %d times just after starting: HomeNX's HOME instead (frontend.txt=\"%s\", start mode %d)",
                              starts, homeStyle.c_str(), (int)startMode);
            }
        }
        if (useCards) {
            auto activity = std::make_unique<cards::CardsActivity>(sysStatus.suspended_app_id, sysStatus.app_running,
                startMode == switchu::smi::MenuStartMode::StartupBoot,
                startMode == switchu::smi::MenuStartMode::AppletReturn);
            activity->setWakeStart(startMode == switchu::smi::MenuStartMode::WakeUp);
            app.setActivity(std::move(activity));
        }
        else if (startupConfig.tutorialCompleted || startMode == switchu::smi::MenuStartMode::WakeUp)
            app.setActivity(makeMenuActivity(false));
        else
            app.setActivity(std::make_unique<TutorialActivity>(makeMenuActivity));
        DebugLog::log("[menu] app.initialize...");
        try {
            if (app.initialize()) {
                diag::begin("UI", "UI loop");
                DebugLog::log("[menu] app.run...");
                DisplayModeHook display{app, prefer1080p};
                AppletHookCookie displayHook{};
                appletHook(&displayHook, onDisplayMode, &display);
                app.run();
                appletUnhook(&displayHook);
            } else {
                diag::log(diag::Level::Error, "UI", "Application initialization failed");
                DebugLog::log("[menu] app.initialize FAILED");
                cards::startStep("app.initialize failed");
            }
        } catch (const std::exception& e) { // (for the log: it still ends the process, as it did)
            diag::log(diag::Level::Error, "UI", "Unhandled application initialization or run exception");
            if (dynamic_cast<const std::bad_alloc*>(&e))
                diag::log(diag::Level::Error, "MEM", "C++ allocation failed");
            cards::startStep(std::string("exception: ") + e.what());
            throw;
        }
        diag::begin("GPU", "Application shutdown BEGIN");
        DebugLog::log("[menu] app.shutdown...");
        app.shutdown();
        diag::log(diag::Level::Info, "GPU", "Application shutdown END");
#endif
    }

    TTF_Quit();
    SDL_Quit();
#ifdef SWITCHU_HOMEBREW
    DebugLog::log("[hb] exit");
    DebugLog::closeFileLog();
#endif
#ifdef SWITCHU_MENU
    DebugLog::log("[menu] exit");
#endif
    return 0;
}
