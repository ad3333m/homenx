#include "Settings.hpp"
#include <switchu/lock_picture.hpp>
#include <switchu/achievements.hpp>
#include <cassert>
#include <cstdlib>
#include <set>

// The production writer is asynchronous. Use a synchronous sink to verify the
// exact JSON the UI queues, then exercise the real settings loader.
namespace cards {
void writeLater(const std::string& path, std::string data) { std::ofstream(path) << data; }
}

int main() {
    namespace fs = std::filesystem;
    const auto original = fs::current_path();
    char temporary[] = "/tmp/homenx-personalization-XXXXXX";
    assert(mkdtemp(temporary));
    fs::current_path(temporary);
    fs::create_directories("sdmc:/config/CardsNX");
    fs::create_directories("sdmc:/config/SwitchU");

    // Old three-page settings keep their chosen order and gain Media.
    std::ofstream(cards::Settings::kPath) << R"({"pageOrder":[2,0,1],"sortOrder":2})";
    cards::Settings old;
    old.load();
    assert((old.pageOrder == std::vector<int>{2,0,1,3}));
    assert(old.sort == 2 && old.customPages.empty());

    // Duplicate/out-of-range page ids and an entirely hidden library are
    // repaired without losing the valid order or custom-page memberships.
    std::ofstream(cards::Settings::kPath) << R"({
        "customPages":[{"name":"Co-op","titles":["0000000000000001","0000000000000001",3]},
                       {"name":"Backlog","titles":["0000000000000002"]}],
        "pageOrder":[4,4,-1,99,0],"hiddenPages":[0,1,2,3,4,5,5,99]})";
    cards::Settings custom;
    custom.load();
    assert(custom.customPages.size() == 2 && custom.customPages[0].titles.size() == 1);
    assert((custom.pageOrder == std::vector<int>{4,0,1,2,3,5}));
    assert(custom.hiddenPages.size() == 5);
    assert(std::set<int>(custom.pageOrder.begin(), custom.pageOrder.end()).size() == 6);
    custom.save();
    cards::Settings restored;
    restored.load();
    assert(restored.customPages[1].name == "Backlog");
    assert(restored.customPages[1].titles == custom.customPages[1].titles);
    assert(restored.pageOrder == custom.pageOrder);

    // Limit restored collections before ids are used to index fixed UI arrays.
    custom.customPages.resize(50, {"Extra", {}});
    custom.normalizePages();
    assert(custom.customPages.size() == cards::Settings::kMaxCustomPages);
    assert(custom.pageOrder.size() == cards::Settings::kMaxPages);

    // Rotation never repeats an image when another usable image is selected.
    namespace lock = switchu::lockpicture;
    for (const char* file : {"one.png", "two.jpg", "three.png"}) std::ofstream(file) << "image fixture";
    lock::set("one.png");
    lock::savePlaylist({"one.png", "one.png", "two.jpg", "missing.png"});
    assert(lock::playlist().size() == 3);
    lock::setRotation(true);
    std::string previous = lock::get();
    for (int seed = 0; seed < 100; ++seed) {
        const auto next = lock::next(seed);
        assert(next != previous && (next == "one.png" || next == "two.jpg"));
        previous = next;
    }
    // Persist a second selection, then handle removed images and an empty pool.
    lock::savePlaylist({"three.png"});
    assert(lock::next(0) == "three.png");
    fs::remove("three.png");
    assert(lock::next(1) == "three.png");
    lock::savePlaylist({});
    assert(lock::next(2) == "three.png");
    lock::savePlaylist({"one.png"});
    lock::setRotation(false);
    assert(lock::next(3) == "three.png");

    // Trivial first-launch awards are gone; milestones for every game can be turned off.
    namespace ach = switchu::achievements;
    assert(ach::milestonesEnabled());
    ach::setMilestonesEnabled(false);
    assert(!ach::milestonesEnabled());
    ach::setMilestonesEnabled(true);
    assert(ach::milestonesEnabled());
    for (const auto& milestone : ach::kPerGame) {
        assert(std::string(milestone.id) != "first");
        assert(ach::progress(milestone, {0, 1}) < 1.f);
    }

    fs::current_path(original);
    fs::remove_all(temporary);
}
