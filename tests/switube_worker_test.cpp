#include "Switube.hpp"
#include <switch.h>
#include <cassert>
#include <filesystem>
#include <thread>

using cards::Switube;
using namespace std::chrono_literals;
template <class F> void eventually(F test) {
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (!test() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(10ms);
    assert(test());
}

int main() {
    namespace fake = switube_fake;
    std::filesystem::create_directories("sdmc:/config/Switube");
    Switube player;
    player.start();
    std::this_thread::sleep_for(50ms);
    assert(fake::launches == 0 && fake::connects == 0); // dormant until the UI asks
    fake::ready(0x99); // stale readiness from an earlier process must not permit IPC
    player.setWatching(true);
    eventually([] { return fake::launches == 1; });
    std::this_thread::sleep_for(50ms);
    assert(fake::connects == 0);
    player.command(Switube::Cmd::PlayPause); // keep this queued through startup
    fake::ready(0x55);
    eventually([&] { return player.now().playing; });
    assert(fake::connects == 1 && fake::launches == 1);
    player.command(Switube::Cmd::Volume, 55);
    player.command(Switube::Cmd::Repeat);
    player.command(Switube::Cmd::Shuffle);
    player.command(Switube::Cmd::Next);
    eventually([] { return fake::volume == 550 && fake::repeat == SwitubeRepeatMode_All && fake::shuffle == 1 && fake::nexts == 1; });
    player.command(Switube::Cmd::Repeat);
    eventually([] { return fake::repeat == SwitubeRepeatMode_One; });
    player.browse(Switube::Browse::Library);
    Switube::Page page;
    eventually([&] { player.takePage(page); return page.generation == 7; });
    assert(page.title == "Downloads" && page.items.size() == 1 && page.items[0].generation == 7);
    player.setWatching(false); // sleep/closed player must stop IPC, without stopping audio
    std::this_thread::sleep_for(100ms);
    const auto polls = fake::polls.load();
    player.command(Switube::Cmd::Next); // ignored while sleeping
    std::this_thread::sleep_for(600ms);
    assert(fake::polls == polls && fake::nexts == 1 && fake::playing);
    player.setWatching(true);
    eventually([&] { return player.now().connected; });
    assert(fake::launches == 1); // reattach to the same background player
    player.stop();
    assert(fake::playing); // exiting HOME leaves background playback alone
}
