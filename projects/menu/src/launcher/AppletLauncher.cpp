#include "AppletLauncher.hpp"
#include "core/DebugLog.hpp"
#ifdef SWITCHU_MENU
#include "smi_commands.hpp"
#include <switchu/smi_protocol.hpp>
#endif
#include <switch.h>
#include <sys/stat.h>
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

void AppletLauncher::launchApplication(uint64_t titleId, AccountUid uid) {
    DebugLog::log("[launcher] tid=%016lX", titleId);
    Result rc = switchu::menu::smi_cmd::launchApplication(titleId, uid);
    if (R_FAILED(rc)) {
        DebugLog::log("[launcher] FAIL: 0x%X", rc);
        return;
    }
    DebugLog::log("[launcher] command sent, closing menu");
    if (m_cb.requestExit) m_cb.requestExit();
}

void AppletLauncher::resumeApplication() {
    if (m_suspendedTitleId == 0) {
        DebugLog::log("[launcher] no app suspended!");
        return;
    }
    switchu::FileLog::logCommit("[launcher] resume: sending command");
    const Result rc = switchu::menu::smi_cmd::resumeApplication();
    switchu::FileLog::logCommit("[launcher] resume: command returned 0x%X", rc);
    if (m_cb.requestExit) m_cb.requestExit();
    switchu::FileLog::logCommit("[launcher] resume: exit requested");
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
bool AppletLauncher::launchSphaira() { return false; }
void AppletLauncher::launchEshop()    {}
void AppletLauncher::launchNews()     {}
void AppletLauncher::launchBrowser()  {}
void AppletLauncher::launchFriends()  {}

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

Result showWeb(const char* url, bool news) {
    WebCommonConfig cfg;
    Result rc = news ? webNewsCreate(&cfg, url) : webPageCreate(&cfg, url);
    if (R_SUCCEEDED(rc)) rc = webConfigSetJsExtension(&cfg, true);
    if (R_SUCCEEDED(rc)) rc = webConfigShow(&cfg, nullptr);
    return rc;
}

AccountUid currentUser() {
    AccountUid uid{};
    if (R_FAILED(accountGetPreselectedUser(&uid)) || !accountUidIsValid(&uid))
        accountGetLastOpenedUser(&uid);
    return uid;
}

} // namespace

void AppletLauncher::launchAlbum()     { logRc("album", albumLaShowAllAlbumFiles()); }

// Hand the process to Sphaira through hbloader, so it keeps HomeNX's mode
// (application mode when HomeNX was started over a game).
bool AppletLauncher::launchSphaira() {
    static const char* const paths[] = {
        "sdmc:/switch/sphaira/sphaira.nro",
        "sdmc:/switch/sphaira.nro",
        "sdmc:/hbmenu.nro", // sphaira installs itself here when set as the menu
    };
    struct stat st;
    for (const char* path : paths) {
        if (stat(path, &st) != 0) continue;
        char args[0x300];
        std::snprintf(args, sizeof(args), "\"%s\"", path);
        Result rc = envSetNextLoad(path, args);
        logRc(path, rc);
        if (R_FAILED(rc)) return false;
        if (m_cb.requestExit) m_cb.requestExit();
        return true;
    }
    DebugLog::log("[launcher] sphaira not found");
    return false;
}
void AppletLauncher::launchMiiEditor() { logRc("mii", miiLaShowMiiEdit(MiiSpecialKeyCode_Normal)); }
void AppletLauncher::launchControllerPairing() {
    HidLaControllerSupportArg arg;
    hidLaCreateControllerSupportArg(&arg);
    logRc("controllers", hidLaShowControllerSupportForSystem(nullptr, &arg, false));
}

void AppletLauncher::launchEshop() {
    // ponytail: ShopN takes the web TLV args with ShimKind_Shop (switchbrew
    // "Internet Browser"); untested on hardware, so fall back to the web store.
    WebCommonConfig cfg;
    Result rc = webPageCreate(&cfg, "https://bugyo.hac.lp1.eshop.nintendo.net/ashigaru/");
    if (R_SUCCEEDED(rc)) {
        reinterpret_cast<WebArgHeader*>(cfg.arg.data)->shimKind = WebShimKind_Shop;
        cfg.appletid = AppletId_LibraryAppletShop;
        rc = webConfigShow(&cfg, nullptr);
    }
    logRc("eshop", rc);
    if (R_FAILED(rc))
        logRc("eshop-web", showWeb("https://www.nintendo.com/store/", false));
}

void AppletLauncher::launchNews()     { logRc("news", showWeb("https://www.nintendo.com/whatsnew/", true)); }
// Full browser tuned for the OLED panel: true-black boot and page background,
// a dark start page, touch on page contents, the left-stick pointer, and a
// footer that hides while scrolling so pages get the whole 720p screen.
// Optional setters may be missing on old firmware, so their results are ignored.
void AppletLauncher::launchBrowser() {
    WebCommonConfig cfg;
    Result rc = webPageCreate(&cfg, "https://duckduckgo.com/?kae=d&kak=-1&kax=-1&kaq=-1");
    if (R_FAILED(rc)) return logRc("browser", rc);
    webConfigSetBootDisplayKind(&cfg, WebBootDisplayKind_Black);
    webConfigSetBootLoadingIcon(&cfg, true);
    webConfigSetPointer(&cfg, true);
    webConfigSetLeftStickMode(&cfg, WebLeftStickMode_Pointer);
    webConfigSetTouchEnabledOnContents(&cfg, true);
    webConfigSetFooter(&cfg, true);
    webConfigSetFooterFixedKind(&cfg, WebFooterFixedKind_Default);
    webConfigSetPageScrollIndicator(&cfg, true);
    webConfigSetPageCache(&cfg, true);
    webConfigSetWebAudio(&cfg, true);
    webConfigSetMediaAutoPlay(&cfg, true);
    webConfigSetMediaPlayerSpeedControl(&cfg, true);
    webConfigSetScreenShot(&cfg, true);
    webConfigSetJsExtension(&cfg, true);
    webConfigSetUserAgentAdditionalString(&cfg, "HomeNX");
    logRc("browser", webConfigShow(&cfg, nullptr));
}

void AppletLauncher::launchFriends() {
    AccountUid uid = currentUser();
    if (!accountUidIsValid(&uid)) {
        PselUserSelectionSettings sel{};
        if (R_FAILED(pselShowUserSelector(&uid, &sel))) return;
    }
    logRc("friends", friendsLaShowFriendList(uid));
}

void AppletLauncher::launchControllerRemapping() {}
void AppletLauncher::launchNetConnect()        {}
void AppletLauncher::launchUserPage(AccountUid) {}
void AppletLauncher::launchUserCreator()       {}
void AppletLauncher::enterSleep()              {}
void AppletLauncher::shutdown()                {}
void AppletLauncher::reboot()                  {}
void AppletLauncher::launchApplication(uint64_t, AccountUid) {}
void AppletLauncher::resumeApplication()       {}
void AppletLauncher::terminateApplication()    {}
void AppletLauncher::checkRunningApplication() {}

#endif
