// HomeNX Loader: the HomeNX frontend installs this as sdmc:/hbmenu.nro.
// Atmosphere's homebrew loader starts sdmc:/hbmenu.nro on every visit and
// again after every app exits, so this hands over straight away: to the .nro
// HomeNX asked for (Homebrew page), else to Sphaira, else to the hbmenu that
// was there before. No UI, it only picks the next .nro.
#include <switch.h>
#include <cstdio>
#include <string>
#include <sys/stat.h>

namespace {

constexpr const char* kNext = "sdmc:/config/SwitchU/hbl_next.txt";        // one-shot request
constexpr const char* kSphairaPath = "sdmc:/config/SwitchU/sphaira_path.txt";
constexpr const char* kOriginal = "sdmc:/hbmenu.nro.homenx-original";

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

bool chain(const std::string& path) {
    if (!exists(path) || path == "sdmc:/hbmenu.nro") return false; // never ourselves
    const std::string args = "\"" + path + "\"";
    return R_SUCCEEDED(envSetNextLoad(path.c_str(), args.c_str()));
}

} // namespace

int main(int, char**) {
    const std::string next = readLine(kNext);
    if (!next.empty()) {
        std::remove(kNext);
        if (chain(next)) return 0;
    }
    if (chain(readLine(kSphairaPath))) return 0;
    for (const char* path : {"sdmc:/switch/sphaira/sphaira.nro", "sdmc:/switch/sphaira.nro"})
        if (chain(path)) return 0;
    if (chain(kOriginal)) return 0;
    // Nothing to run. Returning would make the loader start us again forever,
    // so end the process; the console goes back to the HOME menu.
    svcExitProcess();
    return 0;
}
