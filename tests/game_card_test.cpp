#include "Library.hpp"
#include <switch.h>
#include <switchu/control_cache.hpp>
#include <switchu/ns_ext.hpp>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>

namespace {
constexpr u64 kFirst = 0x0100000000001000;
constexpr u64 kSecond = 0x0100000000002000;
constexpr u64 kThird = 0x0100000000003000;
constexpr Result kFailure = MAKERESULT(2, 7);
constexpr u32 kInserted = switchu::ns::AppViewFlag_IsGameCard | switchu::ns::AppViewFlag_IsGameCardInserted;
std::vector<u64> titles;
std::map<u64, u32> flags;
bool present, removedDuringRead, flagsChangedDuringRead, cardTypeChangedDuringRead;
Result openResult, presenceResult, recordResult, viewResult, controlResult;
int opened, closed, presenceCalls, controlCalls, viewCalls;
u64 controlSize;
u64 failingTitle, changedTitle;

void reset() {
    titles = {kFirst, kSecond};
    flags = {{kFirst, switchu::ns::AppViewFlag_IsGameCard}, {kSecond, kInserted}};
    present = true;
    removedDuringRead = flagsChangedDuringRead = cardTypeChangedDuringRead = false;
    openResult = presenceResult = recordResult = viewResult = controlResult = 0;
    opened = closed = presenceCalls = controlCalls = viewCalls = 0;
    controlSize = sizeof(NacpStruct) + 4;
    failingTitle = changedTitle = 0;
}

cards::GameCardState read(u64 known = 0) {
    auto state = cards::readGameCard(known);
    assert(closed == opened);
    return state;
}
}

extern "C" {
Result fsOpenDeviceOperator(FsDeviceOperator* device) {
    *device = {};
    if (R_SUCCEEDED(openResult)) ++opened;
    return openResult;
}
void fsDeviceOperatorClose(FsDeviceOperator*) { ++closed; }
Result fsDeviceOperatorIsGameCardInserted(FsDeviceOperator*, bool* inserted) {
    *inserted = present && !(removedDuringRead && presenceCalls > 0);
    ++presenceCalls;
    return presenceResult;
}
Result nsListApplicationRecord(NsApplicationRecord* records, s32 count, s32 offset, s32* got) {
    *got = std::min<s32>(count, static_cast<s32>(titles.size()) - offset);
    for (s32 index = 0; index < *got; ++index) {
        records[index] = {};
        records[index].application_id = titles[static_cast<size_t>(offset + index)];
    }
    return recordResult;
}
Result nsGetApplicationView(NsApplicationView* views, const u64* ids, s32 count) {
    ++viewCalls;
    auto* extended = reinterpret_cast<switchu::ns::ExtApplicationView*>(views);
    for (s32 index = 0; index < count; ++index) {
        extended[index] = {};
        extended[index].app_id = ids[index];
        extended[index].flags = flags[ids[index]];
        if (flagsChangedDuringRead && viewCalls > 1)
            extended[index].flags &= ~switchu::ns::AppViewFlag_IsGameCardInserted;
        if (cardTypeChangedDuringRead && viewCalls > 1)
            extended[index].flags &= ~switchu::ns::AppViewFlag_IsGameCard;
        if (changedTitle == ids[index] && controlCalls > 0)
            extended[index].flags &= ~switchu::ns::AppViewFlag_IsGameCardInserted;
    }
    return viewResult;
}
Result nsGetApplicationControlData(NsApplicationControlSource source, u64 id,
                                   NsApplicationControlData* control, size_t capacity, u64* actual) {
    assert(source == NsApplicationControlSource_Storage && capacity == sizeof(*control));
    ++controlCalls;
    std::memset(control, 0, sizeof(*control));
    auto* entries = switchu::control_cache::titleEntries(control->nacp);
    std::strcpy(entries[0].name, id == kFirst ? "First game" : id == kThird ? "Third game" : "Second game");
    std::memcpy(control->icon, "JPEG", 4);
    *actual = controlSize;
    return id == failingTitle ? kFailure : controlResult;
}
Result nacpGetLanguageEntry(NacpStruct* nacp, NacpLanguageEntry** entry) {
    *entry = switchu::control_cache::titleEntries(*nacp);
    return 0;
}
}

int main() {
    using cards::GameCardStatus;
    reset();
    auto state = read();
    assert(state.status == GameCardStatus::Ready && state.game.id == kSecond);
    assert(state.game.name == "Second game" && state.icon.size() == 4);
    assert(controlCalls == 1);
    std::reverse(titles.begin(), titles.end());
    state = read(kSecond);
    assert(state.status == GameCardStatus::Ready && state.game.id == kSecond && controlCalls == 1);
    flags[kFirst] = kInserted;
    flags[kSecond] = switchu::ns::AppViewFlag_IsGameCard;
    state = read(kSecond);
    assert(state.status == GameCardStatus::Ready && state.game.id == kFirst && state.game.name == "First game");

    reset();
    present = false;
    assert(read().status == GameCardStatus::Absent && viewCalls == 0 && controlCalls == 0);
    reset();
    flags[kSecond] = switchu::ns::AppViewFlag_IsGameCard;
    assert(read().status == GameCardStatus::Checking);
    reset();
    flags[kFirst] = kInserted;
    state = read();
    assert(state.status == GameCardStatus::Ready && state.games.size() == 2 && controlCalls == 2);
    assert(state.games[0].id == kFirst && state.games[1].id == kSecond);
    assert(state.games[0].name == "First game" && state.games[1].name == "Second game");
    assert(state.contains(kFirst) && state.contains(kSecond) && !state.contains(kThird));
    const auto cached = cards::readGameCard(kFirst, state.games);
    assert(cached.status == GameCardStatus::Ready && cached.games.size() == 2 && controlCalls == 2);
    assert(cached.games[1].name == "Second game" && cached.icon.empty());
    assert(closed == opened);
    titles = {kThird, kSecond, kFirst, kSecond};
    flags[kThird] = kInserted;
    state = cards::readGameCard(kFirst, cached.games);
    assert(state.status == GameCardStatus::Ready && state.games.size() == 3 && controlCalls == 3);
    assert(state.games[0].id == kThird && state.games[1].id == kSecond && state.games[2].id == kFirst);
    assert(state.game.id == kThird && state.icon.size() == 4 && closed == opened);
    assert(state.games[0].name == "Third game");
    reset();
    flags[kFirst] = kInserted;
    changedTitle = kSecond;
    state = read();
    assert(state.status == GameCardStatus::Checking && state.games.empty());
    reset();
    flags[kFirst] = kInserted;
    failingTitle = kSecond;
    state = read();
    assert(state.status == GameCardStatus::Error && state.error == kFailure && controlCalls == 2);
    reset();
    flags[kFirst] = kInserted;
    flagsChangedDuringRead = true;
    assert(read().status == GameCardStatus::Checking);
    reset();
    removedDuringRead = true;
    assert(read().status == GameCardStatus::Checking);
    reset();
    flagsChangedDuringRead = true;
    assert(read().status == GameCardStatus::Checking);
    reset();
    cardTypeChangedDuringRead = true;
    assert(read().status == GameCardStatus::Checking);
    reset();
    flags[kSecond] = switchu::ns::AppViewFlag_IsGameCardInserted;
    assert(read().status == GameCardStatus::Checking);

    for (Result* error : {&openResult, &presenceResult, &recordResult, &viewResult, &controlResult}) {
        reset();
        *error = kFailure;
        state = read();
        assert(state.status == GameCardStatus::Error && state.error == kFailure);
        *error = 0;
        assert(read().status == GameCardStatus::Ready);
    }
    reset();
    controlSize = 4;
    assert(read().status == GameCardStatus::Error);
    reset();
    controlSize = sizeof(NacpStruct);
    assert(read().status == GameCardStatus::Ready && read().icon.empty());
    reset();
    titles.clear();
    flags.clear();
    for (u64 index = 1; index <= 65; ++index) titles.push_back(index);
    flags[65] = kInserted;
    assert(read().game.id == 65);
    flags[29] = flags[31] = kInserted;
    state = read();
    assert(state.status == GameCardStatus::Ready && state.games.size() == 3);
    assert(state.games[0].id == 29 && state.games[1].id == 31 && state.games[2].id == 65);
    std::cout << "Game card reader tests passed.\n";
}