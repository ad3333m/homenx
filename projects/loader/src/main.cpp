// HomeNX Loader: the HomeNX frontend installs this as sdmc:/hbmenu.nro just
// before it starts homebrew. Atmosphere's homebrew loader starts sdmc:/hbmenu.nro
// on every visit and again after every app exits, so this hands over straight
// away: to the .nro HomeNX asked for (Homebrew page) or Sphaira. When that app
// exits it puts the hbmenu that was there before back, so the Album opens the
// user's own (and keeps working if HomeNX is removed). No UI.
#include <switch.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

namespace {

constexpr const char* kNext = "sdmc:/config/SwitchU/hbl_next.txt";        // one-shot request
constexpr const char* kSphairaPath = "sdmc:/config/SwitchU/sphaira_path.txt";
constexpr const char* kOriginal = "sdmc:/hbmenu.nro.homenx-original";
constexpr const char* kHbmenu = "sdmc:/hbmenu.nro";
constexpr const char* kBundledSphaira = "sdmc:/switch/SwitchU/bin/sphaira.nro"; // ships with HomeNX
// Process id we already handed over in. The homebrew loader starts us again
// whenever the .nro we chose exits, so seeing our own process id here means the
// user left it: end the process (closes the game it borrowed) instead of opening
// Sphaira again forever. HomeNX deletes this before every launch.
constexpr const char* kChained = "sdmc:/config/SwitchU/hbl_chained.txt";

bool exists(const std::string& path) {
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0;
}

std::string readLine(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return {};
    char buf[512] = {};
    std::fgets(buf, sizeof(buf), f);
    std::fclose(f);
    std::string line(buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return line;
}

std::string processId() {
    u64 pid = 0;
    svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
    return std::to_string(pid);
}

bool chain(const std::string& path) {
    if (!exists(path) || path == "sdmc:/hbmenu.nro") return false; // never ourselves
    const std::string args = "\"" + path + "\"";
    if (R_FAILED(envSetNextLoad(path.c_str(), args.c_str()))) return false;
    if (FILE* f = std::fopen(kChained, "w")) {
        std::fputs(processId().c_str(), f);
        std::fclose(f);
    }
    return true;
}

// The hbmenu that was there before, back in its place (this program is already
// loaded). None kept: the Loader stays, or the Album would have nothing to open.
void restoreHbmenu() {
    if (!exists(kOriginal)) return;
    std::remove(kHbmenu);
    std::rename(kOriginal, kHbmenu);
}

bool chainSphaira() {
    if (chain(kBundledSphaira) || chain(readLine(kSphairaPath))) return true;
    for (const char* path : {"sdmc:/switch/sphaira/sphaira.nro", "sdmc:/switch/sphaira.nro"})
        if (chain(path)) return true;
    return false;
}

// An NRO forwarder (an installed title, ID 05...) runs the homebrew loader with
// its own .nro; we only run in it once that .nro has exited.
bool inForwarder() {
    u64 program = 0;
    return appletGetAppletType() == AppletType_Application
        && R_SUCCEEDED(svcGetInfo(&program, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) && (program >> 56) == 0x05;
}

} // namespace

// libnx's own __appInit, without the time service (nor HID): this program never reads the clock, and its
// timeInitialize() aborted the launch (2345-0038, InitFail_Time) whenever the console's time service had run out of
// sessions (0x64274: it only has a few for every process together, and each timeInitialize() takes 6).
extern "C" void __appInit(void) {
    if (R_FAILED(smInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
    if (hosversionGet() == 0 && R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    if (R_FAILED(appletInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_AM));
    if (R_FAILED(fsInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    fsdevMountSdmc();
}

extern "C" void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    appletExit();
    smExit();
}

int main(int argc, char** argv) {
    if (readLine(kChained) == processId()) { // the app we started exited: back to HOME
        std::remove(kChained);
        restoreHbmenu();
        svcExitProcess();
    }
    // Started with an .nro to run (forwarders that go through hbmenu): run it.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.size() > 4 && arg.compare(arg.size() - 4, 4, ".nro") == 0 && chain(arg)) return 0;
    }
    const std::string next = readLine(kNext);
    if (!next.empty()) {
        std::remove(kNext);
        if (next == "sphaira" ? chainSphaira() : chain(next)) return 0;
    }
    // A forwarder's app closed: back to HOME, not into Sphaira.
    if (inForwarder()) svcExitProcess();
    // Opened with nothing asked (the Album): the user's own hbmenu, put back.
    if (exists(kOriginal)) {
        restoreHbmenu();
        if (R_SUCCEEDED(envSetNextLoad(kHbmenu, "\"sdmc:/hbmenu.nro\""))) return 0;
    }
    if (chainSphaira()) return 0;
    // Nothing to run. Returning would make the loader start us again forever,
    // so end the process; the console goes back to the HOME menu.
    svcExitProcess();
    return 0;
}
