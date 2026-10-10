// HomeNX Toggle: turns the HomeNX frontend (the HOME-menu replacement) off or
// back on, then restarts the console. It deliberately skips the SD "commit"
// SwitchU-Manager does: when that call reports an error the Manager rolls the
// change back, which left people unable to turn the frontend off.
#include <switch.h>
#include <cstdio>
#include <sys/stat.h>

namespace {

constexpr const char* kActive = "sdmc:/atmosphere/contents/0100000000001000/exefs.nsp";
constexpr const char* kDisabled = "sdmc:/atmosphere/contents/0100000000001000/exefs.nsp.disabled";
constexpr const char* kDir = "sdmc:/atmosphere/contents/0100000000001000";
constexpr const char* kDirOff = "sdmc:/atmosphere/contents/0100000000001000-homenx-off";

bool exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool reboot() {
    if (R_SUCCEEDED(spsmInitialize())) {
        spsmShutdown(true); // returns only on failure
        spsmExit();
    }
    if (R_SUCCEEDED(bpcInitialize())) {
        bpcRebootSystem();
        bpcExit();
    }
    return false;
}

} // namespace

int main(int, char**) {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    std::printf("\n  HomeNX Toggle\n  =============\n\n");

    bool changed = false;
    if (exists(kActive)) {
        if (exists(kDisabled)) std::remove(kDisabled); // stale copy from an older toggle
        // If the file is held open, move the whole folder instead.
        changed = std::rename(kActive, kDisabled) == 0
               || (!exists(kDirOff) && std::rename(kDir, kDirOff) == 0);
        std::printf(changed ? "  HomeNX frontend is now OFF.\n  The normal HOME menu comes back after a restart.\n"
                            : "  Couldn't turn it off.\n"
                              "  On a PC, delete atmosphere/contents/0100000000001000\n");
    } else if (exists(kDisabled) || (exists(kDirOff) && !exists(kDir))) {
        changed = exists(kDisabled) ? std::rename(kDisabled, kActive) == 0
                                    : std::rename(kDirOff, kDir) == 0;
        std::printf(changed ? "  HomeNX frontend is now ON.\n  It takes over the HOME menu after a restart.\n"
                            : "  Couldn't turn it on (rename failed).\n");
    } else {
        std::printf("  The HomeNX frontend isn't installed on this SD card.\n");
    }

    if (changed)
        std::printf("\n  Restarting in 5 seconds.\n  Press B to stay here and restart later.\n");
    std::printf("\n  Press + to exit.\n");
    consoleUpdate(nullptr);

    u64 deadline = armGetSystemTick() + armNsToTicks(5'000'000'000ull);
    bool rebootPending = changed;
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) break;
        if (rebootPending && (down & HidNpadButton_B)) {
            rebootPending = false;
            std::printf("\n  OK, not restarting. Restart the console yourself\n  for the change to take effect.\n");
        }
        if (rebootPending && armGetSystemTick() >= deadline) {
            rebootPending = false;
            if (!reboot())
                std::printf("\n  Couldn't restart automatically.\n"
                            "  Hold POWER > Power Options > Restart.\n");
        }
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
    return 0;
}
