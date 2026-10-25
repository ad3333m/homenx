#pragma once
#include <functional>
#include <string>
#include <atomic>
#include <switch.h>

class AppletLauncher {
public:
    struct Callbacks {
        std::function<void()>     playSfxModalHide;
        std::function<void()>     requestExit;
        std::function<bool()>     beforePowerAction;
    };

    void init(Callbacks cbs);

    void launchAlbum();
    void launchMiiEditor();
    void launchControllerPairing();
    void launchControllerRemapping();
    void launchNetConnect();
    // Frontend: with a host title (an installed game) and hbloader unpacked,
    // homebrew runs in application mode; otherwise over the Album (applet).
    bool launchSphaira(uint64_t hostTitleId = 0, AccountUid uid = {});
    // Frontend: makes sdmc:/hbmenu.nro the HomeNX Loader (keeping the old one
    // as hbmenu.nro.homenx-original). Call when the menu starts.
    static bool installLoader(); // false if the Loader file is missing or the copy failed
    static const std::string& loaderError(); // why installLoader() failed
    // Frontend: copies NXToggle shipped in switch/SwitchU/bin/ to
    // switch/NXToggle/ (a running Toggle can't replace its own .nro).
    static void installBundledApps(); // the Toggle and the Discord overlay
    // Frontend: unpacks Atmosphere's sdmc:/atmosphere/hbl.nsp into
    // switch/SwitchU/bin/hbl/ (main + main.npdm) for application-mode homebrew.
    static bool prepareHbl();
    // Hands the process to an .nro through hbloader (.nro build only).
    // applicationOnly: never the Album (applet mode); false if that's not possible.
    bool launchNro(const std::string& path, uint64_t hostTitleId = 0, AccountUid uid = {},
                   bool applicationOnly = false);
    Result createMii();
    enum class FriendScreen { AddFriend, Requests };
    Result openFriendScreen(FriendScreen screen, AccountUid uid);
    void findControllers();
    // Homebrew build: these block until the applet closes and return its result.
    Result launchEshop(AccountUid uid);
    Result launchBrowser(AccountUid uid, const char* url = nullptr);
    Result launchFriends(AccountUid uid);
    void launchUserPage(AccountUid uid);
    void launchUserCreator();
    void enterSleep();
    void shutdown();
    void reboot();
    // HomeNX: restart into hekate (its payload handed to Atmosphère first).
    // The payload used ("" when there's none), and false if it couldn't start.
    static std::string hekatePayload();
    // Erista (the first Switch) only: newer ones can't boot a payload from a reboot.
    static bool canRebootToPayload();
    bool rebootToHekate();

    // Ask for the launch without closing the menu (the launch card keeps playing
    // while the game loads); closeForLaunch() then exits. false = not sent.
    bool requestLaunch(uint64_t titleId, AccountUid uid);
    void closeForLaunch();
    void resumeApplication();
    void terminateApplication();

    void checkRunningApplication();

    bool     isAppRunning()  const;
    bool     appHasForeground() const;
    bool     isAppSuspended(uint64_t titleId) const;
    uint64_t suspendedTitleId() const;

    void setAppRunning(bool v);
    void setAppHasForeground(bool v);
    void setSuspendedTitleId(uint64_t v);

#ifdef SWITCHU_MENU
    void setStartupStatus(uint64_t suspendedTitleId, bool appRunning);
#endif

private:
#ifdef SWITCHU_MENU
    bool preparePowerAction();
#endif
#ifdef SWITCHU_MENU
    std::atomic<bool>     m_appRunning{false};
    std::atomic<bool>     m_appHasForeground{false};
    std::atomic<uint64_t> m_suspendedTitleId{0};
#endif

    Callbacks m_cb;
};
