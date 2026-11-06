#include "Settings.hpp"
#include "GameCardPresentation.hpp"
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
    assert(old.gameCardDisplay == 0 && !old.showGameCardTitle() && !old.pinGameCard());
    assert(old.gameCardIcons == 0 && !old.showInsertedGameCardIcon() && !old.showRequiredGameCardIcon());
    for (int mode = 0; mode < 4; ++mode) {
        old.gameCardDisplay = mode;
        for (int icons = 0; icons < 3; ++icons) {
            old.gameCardIcons = icons;
            old.save();
            cards::Settings loaded;
            loaded.load();
            assert(loaded.gameCardDisplay == mode && loaded.gameCardIcons == icons);
            assert(loaded.showInsertedGameCardIcon() == (icons >= 1));
            assert(loaded.showRequiredGameCardIcon() == (icons == 2));
            assert(loaded.pinGameCard() == (mode == 1 || mode == 3));
            assert(loaded.showGameCardTitle() == (mode == 2 || mode == 3));
        }
    }
    std::ofstream(cards::Settings::kPath) << R"({"gameCardDisplay":99})";
    cards::Settings invalid;
    invalid.load();
    assert(invalid.gameCardDisplay == 3);
    assert(invalid.gameCardIcons == 0);
    std::ofstream(cards::Settings::kPath) << R"({"gameCardDisplay":2})";
    cards::Settings previousCartridgeSettings;
    previousCartridgeSettings.load();
    assert(previousCartridgeSettings.gameCardDisplay == 2 && previousCartridgeSettings.gameCardIcons == 0);
    std::ofstream(cards::Settings::kPath) << R"({"gameCardDisplay":0,"showGameCardIcons":false})";
    cards::Settings noCartridgePresentation;
    noCartridgePresentation.load();
    assert(!noCartridgePresentation.showGameCardTitle() && !noCartridgePresentation.pinGameCard());
    assert(noCartridgePresentation.gameCardIcons == 0);
    std::ofstream(cards::Settings::kPath) << R"({"gameCardDisplay":3,"showGameCardIcons":true})";
    cards::Settings previousIconToggle;
    previousIconToggle.load();
    assert(previousIconToggle.gameCardDisplay == 3 && previousIconToggle.gameCardIcons == 0);
    for (int value : {-1, 99}) {
        std::ofstream(cards::Settings::kPath) << "{\"gameCardIcons\":" << value << "}";
        cards::Settings invalidIcons;
        invalidIcons.load();
        assert(invalidIcons.gameCardIcons == (value < 0 ? 0 : 2));
    }
    struct Entry { std::uint64_t id; std::string name; };
    const std::vector<Entry> page{{10, "First"}, {20, "Custom name"}, {30, "Third"}};
    const Entry inserted{20, "System name"}, other{40, "Other cartridge"};
    const auto pinned = cards::presentGameCard(page, &inserted);
    assert(pinned.size() == 3 && pinned[0].id == 20 && pinned[0].name == "Custom name");
    assert(pinned[1].id == 10 && pinned[2].id == 30);
    assert(page[0].id == 10 && page[1].id == 20 && page[2].id == 30);
    assert(cards::gameCardSelection(pinned, 10, 0) == 1);
    assert(cards::gameCardSelection(pinned, 30, 2) == 2);
    const auto replacement = cards::presentGameCard(page, &other);
    assert(replacement.size() == 4 && replacement[0].id == 40 && replacement[2].id == 20);
    const auto unpinned = cards::presentGameCard(page, static_cast<const Entry*>(nullptr));
    assert(unpinned.size() == 3 && unpinned[0].id == 10 && unpinned[1].id == 20);
    assert(cards::gameCardSelection(unpinned, 10, 1) == 0);
    assert(cards::gameCardSelection(unpinned, 40, 0) == 0);
    const auto filtered = cards::presentGameCard(std::vector<Entry>{}, &other);
    assert(filtered.size() == 1 && filtered[0].id == 40);
    const std::vector<Entry> collection{other, inserted, {50, "Third cartridge game"}, inserted};
    const auto group = cards::presentGameCards(page, collection);
    assert(group.size() == 5 && group[0].id == 40 && group[1].id == 20 && group[2].id == 50);
    assert(group[1].name == "Custom name" && group[3].id == 10 && group[4].id == 30);
    assert(cards::gameCardSelection(group, 10, 0) == 3 && cards::gameCardSelection(group, 30, 2) == 4);
    assert(cards::gameCardSelection(group, 20, 1) == 1);
    const auto emptyGroup = cards::presentGameCards(std::vector<Entry>{}, collection);
    assert(emptyGroup.size() == 3 && emptyGroup[0].id == 40 && emptyGroup[2].id == 50);
    const auto restoredGroup = cards::presentGameCards(page, std::vector<Entry>{});
    assert(restoredGroup.size() == 3 && restoredGroup[0].id == 10 && restoredGroup[1].id == 20);

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
