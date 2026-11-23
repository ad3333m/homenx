#include <switchu/boot_modules.hpp>
#include <switchu/recovery.hpp>

#include <cassert>
#include <cstdlib>
#include <fstream>

int main() {
    namespace m = switchu::boot_modules;
    namespace fs = std::filesystem;
    char temp[] = "/tmp/homenx-boot-modules-XXXXXX";
    assert(mkdtemp(temp));
    const fs::path root(temp);
    const auto known = root / "00ff0000636c6bff";
    const auto unknown = root / "0100000000001000";
    fs::create_directories(known / "flags");
    fs::create_directories(unknown / "flags");
    fs::create_directories(root / "0100000000009999" / "romfs");
    std::ofstream(known / "exefs.nsp") << "PFS0";
    std::ofstream(unknown / "flags/boot2.flag");
    std::error_code error;
    auto installed = m::discover(root, error);
    assert(!error && installed.size() == 2);
    assert(m::state(known) == m::State::Unknown);

    std::ofstream(known / m::activeFlag) << "original";
    const m::Module module{"00FF0000636C6BFF", "sys-clk", known, m::State::Enabled, true};
    const m::Change disable{module, m::State::Parked};
    assert(m::apply({disable, disable}).find("failed") != std::string::npos);
    assert(m::state(known) == m::State::Enabled); // first rename rolled back
    assert(m::apply({disable}).empty());
    assert(m::state(known) == m::State::Parked);
    assert(!m::apply({disable}).empty()); // stale UI snapshot

    auto parked = module;
    parked.state = m::State::Parked;
    const m::Change restore{parked, m::State::Enabled};
    std::ofstream(known / m::activeFlag).put('x');
    assert(m::state(known) == m::State::Conflict);
    assert(!m::apply({restore}).empty());
    fs::remove(known / m::activeFlag);
    assert(m::apply({restore}).empty());
    std::string original;
    std::ifstream(known / m::activeFlag) >> original;
    assert(original == "original");
    assert(m::apply({{module, m::State::Enabled}}).empty());
    const auto marker = root / "recovery.flag";
    assert(!switchu::recovery::validMarker(marker.c_str()));
    assert(!switchu::recovery::validMarker(root.c_str()));
    std::ofstream(marker).close();
    assert(switchu::recovery::validMarker(marker.c_str()));
    std::ofstream(marker).put('x');
    assert(!switchu::recovery::validMarker(marker.c_str()));
    fs::remove_all(root);
}
