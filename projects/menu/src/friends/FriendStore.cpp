#include "FriendStore.hpp"
#include "core/DebugLog.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <strings.h>

// libnx opens IFriendService but doesn't wrap the list calls. Layouts are from
// Ryujinx.Horizon (Sdk/Friends/Detail): FriendImpl 0x200, UserPresenceImpl 0xE0.
// In-raw data is packed by alignment, largest first (as Nintendo's sf does).
//   10101 GetFriendList(Uid, SizedFriendFilter, u64 pid_placeholder, s32 offset,
//                       pid) -> s32 count, out MapAlias FriendImpl[]
//   10110 GetFriendProfileImage(Uid, u64 nsa_id) -> s32 size, out MapAlias jpeg
namespace {

struct UserPresence {
    AccountUid uid;
    s64 lastOnline;
    u32 status;
    u8 samePresenceGroup;
    u8 pad[3];
    u8 appKeyValue[0xC0];
};
static_assert(sizeof(UserPresence) == 0xE0);

struct FriendImpl {
    AccountUid uid;
    u64 networkId;
    char nickname[0x21];
    u8 pad[7];
    UserPresence presence;
    u8 favourite;
    u8 isNew;
    u8 unknown[6];
    u8 valid;
    u8 reserved[0x200 - 0x129];
};
static_assert(sizeof(FriendImpl) == 0x200);
static_assert(offsetof(FriendImpl, presence) == 0x40);

constexpr int kMaxFriends = 300;

// GetFriendList's in-raw order isn't confirmed (Ryujinx implies alignment
// order with the offset last, yuzu reads the offset first), so try both.
Result getFriendList(Service* s, AccountUid uid, FriendImpl* list, s32& count, bool offsetFirst) {
    count = 0;
    if (!offsetFirst) {
        struct { AccountUid uid; u8 filter[0x10]; u64 pidPlaceholder; s32 offset; u32 pad; }
            in{uid, {}, 0, 0, 0};
        return serviceDispatchInOut(s, 10101, in, count,
            .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
            .buffers = {{list, sizeof(FriendImpl) * kMaxFriends}},
            .in_send_pid = true);
    }
    struct { s32 offset; u32 pad; AccountUid uid; u8 filter[0x10]; u64 pidPlaceholder; }
        in{0, 0, uid, {}, 0};
    return serviceDispatchInOut(s, 10101, in, count,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
        .buffers = {{list, sizeof(FriendImpl) * kMaxFriends}},
        .in_send_pid = true);
}

} // namespace

std::vector<FriendEntry> loadFriends(AccountUid uid, Result& rc, std::string& diag) {
    std::vector<FriendEntry> out;
    char buf[160];
    rc = friendsInitialize(FriendsServiceType_Administrator);
    if (R_FAILED(rc)) rc = friendsInitialize(FriendsServiceType_System);
    if (R_FAILED(rc)) {
        std::snprintf(buf, sizeof(buf), "friends service unavailable (0x%X)", rc);
        diag = buf;
        return out;
    }
    Service* s = friendsGetServiceSession_IFriendsService();

    // 20100 GetFriendCount(Uid, SizedFriendFilter, u64 pid_placeholder, pid) -> s32
    struct { AccountUid uid; u8 filter[0x10]; u64 pidPlaceholder; } countIn{uid, {}, 0};
    s32 total = -1;
    const Result rcCount = serviceDispatchInOut(s, 20100, countIn, total, .in_send_pid = true);

    static FriendImpl list[kMaxFriends];
    s32 count = 0;
    rc = getFriendList(s, uid, list, count, false);
    const Result rcA = rc;
    const s32 countA = count;
    if (R_FAILED(rc) || (count == 0 && total > 0))
        rc = getFriendList(s, uid, list, count, true);
    std::snprintf(buf, sizeof(buf), "count %d (0x%X), list %d (0x%X) / %d (0x%X)",
                  total, rcCount, countA, rcA, count, rc);
    diag = buf;
    DebugLog::log("[friends] %s", buf);

    if (R_SUCCEEDED(rc)) {
        count = std::clamp(count, 0, kMaxFriends);
        static u8 image[0x20000];
        for (s32 i = 0; i < count; ++i) {
            const FriendImpl& f = list[i];
            FriendEntry e;
            e.nickname.assign(f.nickname, std::find(f.nickname, f.nickname + sizeof(f.nickname), '\0'));
            e.status = f.presence.status;
            e.lastOnline = f.presence.lastOnline;
            e.favourite = f.favourite != 0;

            struct { AccountUid uid; u64 networkId; } imgIn{uid, f.networkId};
            s32 size = 0;
            Result ri = serviceDispatchInOut(s, 10110, imgIn, size,
                .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                .buffers = {{image, sizeof(image)}});
            if (R_SUCCEEDED(ri) && size > 0 && (size_t)size <= sizeof(image))
                e.avatar.assign(image, image + size);
            out.push_back(std::move(e));
        }
    }
    friendsExit();

    std::stable_sort(out.begin(), out.end(), [](const FriendEntry& a, const FriendEntry& b) {
        if ((a.status != 0) != (b.status != 0)) return a.status != 0;
        if (a.favourite != b.favourite) return a.favourite;
        return strcasecmp(a.nickname.c_str(), b.nickname.c_str()) < 0;
    });
    return out;
}
