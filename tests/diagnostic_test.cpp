#include <switchu/diagnostic.hpp>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char**) {
    namespace diag = switchu::diagnostic;
    namespace fs = std::filesystem;
    char temp[] = "/tmp/homenx-diagnostic-XXXXXX";
    assert(mkdtemp(temp));
    const fs::path root(temp);
    const bool missing = argc > 1;
    const auto path = root / (missing ? "absent/diagnostic.log" : "diagnostic.log");
    const auto archive = root / "diagnostic.old";

    if (!missing)
        std::ofstream(path) << std::string(512 * 1024, 'x');

    diag::component = "test";
    diag::origin = armGetSystemTick();
    diag::begin("BOOT", "Startup BEGIN");
    auto emit = [] {
        for (unsigned i = 0; i < 128; ++i)
            diag::log(diag::Level::Info, "INPUT", "Concurrent record");
    };
    if (!missing) emit(); // Guarantee overflow before the worker can drain.
    diag::start(path.c_str(), archive.c_str());
    std::thread first(emit), second(emit);
    first.join();
    second.join();
    diag::end("IPC", "Operation END", 0x1234);
    const unsigned beforeStop = debugRecords.load();
    diag::stop();

    if (missing) {
        assert(debugRecords.load() > beforeStop || beforeStop > 2);
        assert(!fs::exists(path));
    } else {
        assert(fs::file_size(archive) == 512 * 1024);
        std::ifstream stream(path);
        const std::string records{std::istreambuf_iterator<char>(stream), {}};
        assert(records.find("] [ERROR] [test/IPC] [T42] rc=0x00001234 Operation END\n") != std::string::npos);
        assert(records.find("Concurrent record\n") != std::string::npos);
        assert(records.find("Dropped ") != std::string::npos);
        assert(records.find(std::string(20, 'x')) == std::string::npos);
    }
    fs::remove_all(root);
}
