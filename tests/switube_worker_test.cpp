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
    std::filesystem::create_directories("sdmc:/config/SwitchU/media");
    Switube player;
    player.start();
    std::this_thread::sleep_for(50ms);
    assert(fake::starts == 0 && fake::connects == 0); // dormant until the UI asks
    if (FILE* file = std::fopen(HOMENX_MEDIA_READY_PATH, "w")) {
        std::fputs(HOMENX_MEDIA_READY_TAG " 0xF00\n", file);
        std::fclose(file);
    }
    player.setWatching(true);
    std::this_thread::sleep_for(50ms);
    assert(fake::starts == 0); // opening the UI can be for YouTube alone
    player.command(Switube::Cmd::PlayPause); // keep this queued through startup
    eventually([] { return fake::starts == 1; });
    std::this_thread::sleep_for(50ms);
    assert(fake::connects == 0);
    FILE* failure = std::fopen(HOMENX_MEDIA_READY_PATH, "w");
    assert(failure);
    std::fputs(HOMENX_MEDIA_READY_TAG " 0x10801\n", failure);
    std::fclose(failure);
    std::string startupError;
    eventually([&] {
        if (startupError.empty()) startupError = player.takeError();
        return startupError.find("0x10801") != std::string::npos;
    });
    assert(fake::connects == 0);

    player.command(Switube::Cmd::PlayPause);
    eventually([] { return fake::starts == 2; });
    fake::registered = true;
    eventually([&] { return player.now().playing; });
    assert(fake::connects == 1 && fake::starts == 2);
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
    assert(fake::starts == 2); // reattach to the same background player
    player.stop();
    assert(fake::playing); // exiting HOME leaves background playback alone
}
