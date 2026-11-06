#include <switchu/lock_screen_policy.hpp>
#include <switchu/lock_screen_preferences.hpp>
#include <switchu/smi_protocol.hpp>
#include <cassert>
#include <cstdlib>

int main() {
    using namespace switchu::lock_screen;
    // A game/homebrew with no menu process must wake into a fresh locked menu.
    // Turning locking off retains the original direct-resume behavior.
    assert(wakeTarget(true, false, true) == WakeTarget::LockedMenu);
    assert(wakeTarget(true, false, false) == WakeTarget::LockedMenu);
    assert(wakeTarget(true, true, true) == WakeTarget::ExistingMenu);
    assert(wakeTarget(true, true, false) == WakeTarget::ExistingMenu);
    assert(wakeTarget(false, false, true) == WakeTarget::Application);
    assert(wakeTarget(false, false, false) == WakeTarget::Menu);
    assert(wakeTarget(false, true, true) == WakeTarget::ExistingMenu);
    assert(wakeTarget(false, true, false) == WakeTarget::ExistingMenu);
    assert(startLocked(false, true, true));
    assert(startLocked(false, true, false)); // the explicit wake intent survives a settings reload
    assert(startLocked(true, false, true));
    assert(!startLocked(false, false, true)); // an ordinary HOME press does not relock
    assert(!startLocked(true, false, false));
    static_assert(static_cast<unsigned>(switchu::smi::MenuStartMode::WakeUp) == 3);
    static_assert(sizeof(switchu::smi::SystemStatus) == 80);

    ControllerNoticeGate gate;
    assert(gate.observe(0, false, false) == 0); // initial state is silent
    assert(gate.observe(1, true, false) == 0);  // reconnect while locked
    assert(gate.observe(0, true, false) == 0);  // input settles on the rails
    assert(gate.observe(0, false, false) == 0); // no replay after unlock
    assert(gate.observe(1, false, false) == 1); // real wireless connection
    assert(gate.observe(0, false, false) == -1);
    assert(gate.observe(1, false, true) == 0);  // sleep-time update is silent
    assert(gate.observe(1, true, false) == 0);
    assert(gate.observe(1, false, false) == 0);

    // The daemon must use the selected frontend's preference. CardsNX has a
    // JSON preference; checking only HOME's legacy flag misses enabled locks.
    namespace fs = std::filesystem;
    const auto original = fs::current_path();
    char temporary[] = "/tmp/homenx-wake-tests-XXXXXX";
    assert(mkdtemp(temporary));
    fs::current_path(temporary);
    fs::create_directories("sdmc:/config/CardsNX");
    fs::create_directories("sdmc:/config/SwitchU");
    std::ofstream("sdmc:/config/SwitchU/frontend.txt") << "cards\n";
    std::ofstream("sdmc:/config/CardsNX/settings.json") << R"({"lockScreen":true})";
    assert(enabled());
    std::ofstream("sdmc:/config/CardsNX/settings.json") << R"({"lockScreen":false})";
    assert(!enabled());
    std::ofstream("sdmc:/config/SwitchU/lock_screen") << '1';
    assert(!enabled()); // another frontend's flag does not enable CardsNX
    std::ofstream("sdmc:/config/CardsNX/settings.json") << "invalid JSON";
    assert(!enabled());
    std::ofstream("sdmc:/config/CardsNX/settings.json") << R"({"lockScreen":"false"})";
    assert(!enabled());
    std::ofstream("sdmc:/config/SwitchU/frontend.txt") << "homenx\n";
    assert(enabled());
    fs::remove("sdmc:/config/SwitchU/lock_screen");
    assert(!enabled());
    fs::current_path(original);
    fs::remove_all(temporary);
}
