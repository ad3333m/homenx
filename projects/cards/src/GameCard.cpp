#include "Library.hpp"
#include <switch.h>
#include <switchu/control_cache.hpp>
#include <switchu/ns_ext.hpp>
#include <algorithm>
#include <memory>
#include <new>

namespace cards {

GameCardState readGameCard(std::uint64_t knownTitle, const std::vector<Game>& knownGames) {
    GameCardState state;
    FsDeviceOperator device{};
    Result rc = fsOpenDeviceOperator(&device);
    if (R_FAILED(rc)) {
        state.status = GameCardStatus::Error;
        state.error = rc;
        return state;
    }
    bool inserted = false;
    rc = fsDeviceOperatorIsGameCardInserted(&device, &inserted);
    if (R_FAILED(rc) || !inserted) {
        fsDeviceOperatorClose(&device);
        state.status = R_FAILED(rc) ? GameCardStatus::Error : GameCardStatus::Absent;
        state.error = rc;
        return state;
    }

    NsApplicationRecord records[30]{};
    for (s32 offset = 0; offset < 1024;) {
        s32 count = 0;
        rc = nsListApplicationRecord(records, 30, offset, &count);
        if (R_FAILED(rc) || count <= 0) break;
        std::uint64_t ids[30]{};
        switchu::ns::ExtApplicationView views[30]{};
        for (s32 index = 0; index < count; ++index) ids[index] = records[index].application_id;
        rc = switchu::ns::queryApplicationViews(ids, count, views);
        if (R_FAILED(rc)) break;
        for (s32 index = 0; index < count; ++index) {
            const auto flags = views[index].flags;
            if (!(flags & switchu::ns::AppViewFlag_IsGameCard) ||
                !(flags & switchu::ns::AppViewFlag_IsGameCardInserted)) continue;
            if (state.contains(ids[index])) continue;
            Game game;
            game.id = ids[index];
            game.flags = flags;
            state.games.push_back(std::move(game));
        }
        offset += count;
        if (count < 30) break;
    }
    if (R_FAILED(rc)) {
        state.status = GameCardStatus::Error;
        state.error = rc;
    } else if (!state.games.empty()) {
        state.status = GameCardStatus::Ready;
        std::unique_ptr<NsApplicationControlData> control;
        for (Game& game : state.games) {
            const bool first = &game == &state.games.front();
            const auto cached = std::find_if(knownGames.begin(), knownGames.end(), [&](const Game& title) {
                return title.id == game.id && !title.name.empty();
            });
            if (cached != knownGames.end()) {
                const auto currentFlags = game.flags;
                game = *cached;
                game.flags = currentFlags;
                if (!first || game.id == knownTitle) continue;
            } else if (first && game.id == knownTitle) continue;
            if (!control) control.reset(new (std::nothrow) NsApplicationControlData{});
            u64 size = 0;
            rc = control ? nsGetApplicationControlData(NsApplicationControlSource_Storage, game.id,
                                                       control.get(), sizeof(NsApplicationControlData), &size)
                         : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
            switchu::control_cache::Meta meta{};
            if (R_SUCCEEDED(rc) && size >= sizeof(control->nacp) &&
                switchu::control_cache::fillMetaFromControlData(game.id, *control, meta)) {
                game.name = meta.name[0] ? meta.name : meta.english_name;
                const size_t iconSize = std::min<size_t>(size - sizeof(control->nacp), sizeof(control->icon));
                if (first) state.icon.assign(control->icon, control->icon + iconSize);
            }
            if (R_FAILED(rc) || game.name.empty()) {
                state.status = GameCardStatus::Error;
                state.error = rc;
                break;
            }
        }
        if (state.status == GameCardStatus::Ready) {
            for (size_t offset = 0; offset < state.games.size(); offset += 30) {
                const s32 count = static_cast<s32>(std::min<size_t>(30, state.games.size() - offset));
                u64 ids[30]{};
                switchu::ns::ExtApplicationView views[30]{};
                for (s32 index = 0; index < count; ++index) ids[index] = state.games[offset + index].id;
                rc = switchu::ns::queryApplicationViews(ids, count, views);
                if (R_FAILED(rc)) {
                    state.status = GameCardStatus::Error;
                    state.error = rc;
                    break;
                }
                for (s32 index = 0; index < count; ++index) {
                    const auto flags = views[index].flags;
                    if (!(flags & switchu::ns::AppViewFlag_IsGameCard) ||
                        !(flags & switchu::ns::AppViewFlag_IsGameCardInserted)) {
                        state = GameCardState{};
                        break;
                    }
                    state.games[offset + index].flags = flags;
                }
                if (state.status != GameCardStatus::Ready) break;
            }
            if (state.status == GameCardStatus::Ready) state.game = state.games.front();
        }
    }
    bool stillInserted = false;
    const Result presenceRc = fsDeviceOperatorIsGameCardInserted(&device, &stillInserted);
    fsDeviceOperatorClose(&device);
    if (R_FAILED(presenceRc)) {
        state = GameCardState{};
        state.status = GameCardStatus::Error;
        state.error = presenceRc;
    } else if (!stillInserted) {
        state = GameCardState{};
    }
    return state;
}

}