#include "FriendStore.hpp"
#include "core/DebugLog.hpp"
#include <algorithm>
#include <cstddef>
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

} // namespace

std::vector<FriendEntry> loadFriends(AccountUid uid, Result& rc) {
    std::vector<FriendEntry> out;
    rc = friendsInitialize(FriendsServiceType_Administrator);
    if (R_FAILED(rc)) rc = friendsInitialize(FriendsServiceType_System);
    if (R_FAILED(rc)) {
        DebugLog::log("[friends] init rc=0x%X", rc);
        return out;
    }
    Service* s = friendsGetServiceSession_IFriendsService();

    static FriendImpl list[kMaxFriends];
    struct {
        AccountUid uid;
        u8 filter[0x10]; // all zero: every friend, any status
        u64 pidPlaceholder;
        s32 offset;
        u32 pad;
    } in{uid, {}, 0, 0, 0};
    s32 count = 0;
    rc = serviceDispatchInOut(s, 10101, in, count,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
        .buffers = {{list, sizeof(list)}},
        .in_send_pid = true);
    DebugLog::log("[friends] GetFriendList rc=0x%X count=%d", rc, count);

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
