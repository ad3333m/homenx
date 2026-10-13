#include "FriendStore.hpp"
#include "core/DebugLog.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
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
            e.networkId = f.networkId;
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

// ---------------------------------------------------------------------------
// HomeNX Social: profile, play history, friend requests.
//
// The request and profile layouts aren't public. Argument order comes from
// the SDK's IPC definitions (daeken's generated ipcdefs) with natural alignment,
// which is what made GetFriendList work above. Where a layout is guessed, the
// raw reply goes to menu.log so it can be checked against a real console.
namespace {

struct FriendsSession {
    Result rc;
    FriendsSession() {
        rc = friendsInitialize(FriendsServiceType_Administrator);
        if (R_FAILED(rc)) rc = friendsInitialize(FriendsServiceType_System);
    }
    ~FriendsSession() {
        if (R_SUCCEEDED(rc)) friendsExit();
    }
    Service* s() { return friendsGetServiceSession_IFriendsService(); }
};

void logHex(const char* what, const u8* p, size_t n) {
    for (size_t o = 0; o < n; o += 32) {
        const size_t end = std::min(n, o + 32);
        if (std::all_of(p + o, p + end, [](u8 b) { return b == 0; })) continue;
        char line[32 * 3 + 1];
        size_t k = 0;
        for (size_t i = o; i < end; ++i) k += std::snprintf(line + k, sizeof(line) - k, "%02X ", p[i]);
        DebugLog::log("[friends] %s +%03X %s", what, (unsigned)o, line);
    }
}

// A name at p: 1..32 bytes of text ending in NUL.
bool nameAt(const u8* p, size_t avail, std::string& out) {
    size_t n = 0;
    while (n < avail && n < 0x21 && p[n]) {
        if (p[n] < 0x20 || p[n] == 0x7F) return false;
        ++n;
    }
    if (n == 0 || n >= 0x21 || n >= avail) return false;
    out.assign(reinterpret_cast<const char*>(p), n);
    return true;
}

std::int64_t posixOf(std::uint64_t t) {
    return t != 0 && t < 100'000'000ULL ? static_cast<std::int64_t>(t * 60) : static_cast<std::int64_t>(t);
}

// Play log entries are found by their game id (0x0100...000), each assumed to be
// {u64 id, s32 play count, s32 minutes, s64 first played, s64 last played}.
std::vector<PlayRecord> scanPlayLog(const u8* data, size_t size) {
    std::vector<PlayRecord> out;
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    auto plausible = [now](std::int64_t t) { return t > 1488326400 && t < now + 86400; }; // since 2017
    for (size_t o = 0; o + 8 <= size; o += 8) {
        u64 id;
        std::memcpy(&id, data + o, 8);
        if ((id >> 56) != 0x01 || (id & 0xFFF) != 0 || id < 0x0100000000010000ULL) continue;
        if (std::any_of(out.begin(), out.end(), [id](const PlayRecord& r) { return r.titleId == id; })) continue;
        PlayRecord r;
        r.titleId = id;
        if (o + 32 <= size) {
            u32 count, minutes;
            std::int64_t first, last;
            std::memcpy(&count, data + o + 8, 4);
            std::memcpy(&minutes, data + o + 12, 4);
            std::memcpy(&first, data + o + 16, 8);
            std::memcpy(&last, data + o + 24, 8);
            if (count < 1'000'000 && minutes < 10'000'000 && (count | minutes) != 0) {
                r.launches = count;
                r.minutes = minutes;
                r.haveTime = true;
            }
            if (plausible(first)) r.firstPlayed = first;
            if (plausible(last)) r.lastPlayed = last;
        }
        out.push_back(r);
    }
    return out;
}

std::string formatFriendCode(const std::string& digits, bool withPrefix) {
    if (digits.size() != 12) return digits;
    std::string s = withPrefix ? "SW-" : "";
    s += digits.substr(0, 4) + "-" + digits.substr(4, 4) + "-" + digits.substr(8, 4);
    return s;
}

std::string digitsOf(const std::string& s) {
    std::string d;
    for (char c : s)
        if (c >= '0' && c <= '9') d += c;
    return d;
}

// Needs an open FriendsSession.
std::string ownFriendCode(AccountUid uid) {
    auto set = std::make_unique<FriendsUserSetting>();
    std::memset(set.get(), 0, sizeof(*set));
    const Result rc = friendsGetUserSetting(uid, set.get());
    if (R_FAILED(rc)) {
        DebugLog::log("[friends] GetUserSetting rc=0x%X", rc);
        return {};
    }
    return std::string(set->friend_code,
                       std::find(set->friend_code, set->friend_code + sizeof(set->friend_code), '\0'));
}

} // namespace

std::string loadFriendCode(AccountUid uid) {
    FriendsSession fs;
    if (R_FAILED(fs.rc)) return {};
    const std::string raw = ownFriendCode(uid);
    const std::string d = digitsOf(raw);
    DebugLog::log("[friends] own friend code format '%s'", raw.c_str());
    return d.size() == 12 ? formatFriendCode(d, true) : raw;
}

std::vector<PlayRecord> loadMyPlayHistory(AccountUid uid, const std::vector<std::uint64_t>& titleIds) {
    std::vector<PlayRecord> out;
    const Result rc = pdmqryInitialize();
    if (R_FAILED(rc)) {
        DebugLog::log("[social] pdm:qry unavailable rc=0x%X", rc);
        return out;
    }
    for (std::uint64_t id : titleIds) {
        PdmPlayStatistics st{};
        if (R_FAILED(pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(id, uid, false, &st))) continue;
        if (st.total_launches == 0 && st.playtime == 0) continue;
        PlayRecord r;
        r.titleId = id;
        r.minutes = st.playtime / 60'000'000'000ULL;
        r.launches = st.total_launches;
        r.firstPlayed = posixOf(st.first_timestamp_user);
        r.lastPlayed = posixOf(st.last_timestamp_user);
        r.haveTime = true;
        out.push_back(r);
    }
    pdmqryExit();
    std::sort(out.begin(), out.end(),
              [](const PlayRecord& a, const PlayRecord& b) { return a.lastPlayed > b.lastPlayed; });
    DebugLog::log("[social] play history: %d of %d games played", (int)out.size(), (int)titleIds.size());
    return out;
}

std::vector<PlayRecord> loadFriendPlayLog(AccountUid uid, std::uint64_t networkId, Result& rc) {
    std::vector<PlayRecord> out;
    FriendsSession fs;
    rc = fs.rc;
    if (R_FAILED(rc)) return out;

    // 20102 GetFriendDetailedInfo(Uid, nsa id) -> FriendDetailedInfoImpl (0x800, pointer).
    auto buf = std::make_unique<u8[]>(0x800);
    std::memset(buf.get(), 0, 0x800);
    struct { AccountUid uid; u64 networkId; } in{uid, networkId};
    rc = serviceDispatchIn(fs.s(), 20102, in,
        .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_Out | SfBufferAttr_FixedSize},
        .buffers = {{buf.get(), 0x800}});
    DebugLog::log("[friends] detailed info %016lX rc=0x%X", (unsigned long)networkId, rc);
    if (R_SUCCEEDED(rc)) {
        logHex("detailed", buf.get(), 0x800);
        out = scanPlayLog(buf.get(), 0x800);
    }
    if (out.empty()) {
        // 20500 GetProfileExtraList(Uid, in nsa ids) -> ProfileExtraImpl[] (0x400 each).
        std::memset(buf.get(), 0, 0x400);
        const Result r2 = serviceDispatchIn(fs.s(), 20500, uid,
            .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
                             SfBufferAttr_HipcPointer | SfBufferAttr_In},
            .buffers = {{buf.get(), 0x400}, {&networkId, sizeof(networkId)}});
        DebugLog::log("[friends] profile extra %016lX rc=0x%X", (unsigned long)networkId, r2);
        if (R_SUCCEEDED(r2)) {
            logHex("extra", buf.get(), 0x400);
            out = scanPlayLog(buf.get(), 0x400);
            rc = r2;
        }
    }
    std::sort(out.begin(), out.end(),
              [](const PlayRecord& a, const PlayRecord& b) { return a.lastPlayed > b.lastPlayed; });
    return out;
}

std::vector<FriendRequestEntry> loadFriendRequests(AccountUid uid, Result& rc) {
    std::vector<FriendRequestEntry> out;
    FriendsSession fs;
    rc = fs.rc;
    if (R_FAILED(rc)) return out;

    // 20200 GetReceivedFriendRequestCount(Uid) -> (s32, s32): the inbox size.
    struct { s32 a, b; } counts{};
    const Result rcCount = serviceDispatchInOut(fs.s(), 20200, uid, counts);
    const s32 received = std::max(counts.a, counts.b);
    DebugLog::log("[friends] received requests rc=0x%X %d/%d", rcCount, counts.a, counts.b);
    if (R_SUCCEEDED(rcCount) && received <= 0) return out;

    // 20201 GetFriendRequestList(u32, u32, Uid) -> s32 count, out FriendRequestImpl[].
    // The two numbers are an offset and the list type, in an order nobody has
    // written down, so equal and swapped pairs are tried until the inbox shows.
    constexpr size_t kBuf = 0x800 * 16;
    auto buf = std::make_unique<u8[]>(kBuf);
    static const u32 kTries[][2] = {{0, 0}, {0, 1}, {1, 0}, {0, 2}, {2, 0}};
    s32 count = 0;
    for (const auto& t : kTries) {
        std::memset(buf.get(), 0, kBuf);
        struct { u32 a, b; AccountUid uid; } in{t[0], t[1], uid};
        count = 0;
        rc = serviceDispatchInOut(fs.s(), 20201, in, count,
            .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
            .buffers = {{buf.get(), kBuf}});
        DebugLog::log("[friends] request list (%u,%u) rc=0x%X count=%d", t[0], t[1], rc, count);
        if (R_SUCCEEDED(rc) && count > 0 && (R_FAILED(rcCount) || count == received)) break;
        count = 0;
    }
    if (count <= 0) return out;
    logHex("request", buf.get(), 0x200);

    // Entry 0's name, then the entry size: where entry 1 has a name at the same place.
    size_t nick = 0;
    std::string name;
    for (size_t o = 8; o <= 0x40 && !nick; o += 8)
        if (nameAt(buf.get() + o, kBuf - o, name)) nick = o;
    size_t stride = 0;
    if (count > 1)
        for (size_t s : {size_t(0x100), size_t(0x200), size_t(0x400), size_t(0x800)})
            if (s * (count - 1) + nick < kBuf && nameAt(buf.get() + s + nick, kBuf - s - nick, name)) {
                stride = s;
                break;
            }
    const s32 usable = stride ? std::min<s32>(count, (s32)(kBuf / stride)) : 1;
    DebugLog::log("[friends] request layout name@0x%X stride=0x%X", (unsigned)nick, (unsigned)stride);
    for (s32 i = 0; i < usable; ++i) {
        const u8* e = buf.get() + (size_t)i * stride;
        FriendRequestEntry r;
        std::memcpy(&r.requestId, e, 8);
        if (nick >= 0x10) std::memcpy(&r.networkId, e + 8, 8);
        if (nick) nameAt(e + nick, kBuf - (size_t)(e - buf.get()) - nick, r.nickname);
        out.push_back(std::move(r));
    }
    return out;
}

Result answerFriendRequest(AccountUid uid, std::uint64_t requestId, bool accept) {
    FriendsSession fs;
    if (R_FAILED(fs.rc)) return fs.rc;
    // 30203 AcceptFriendRequest / 30204 RejectFriendRequest(Uid, RequestId)
    struct { AccountUid uid; u64 requestId; } in{uid, requestId};
    const Result rc = serviceDispatchIn(fs.s(), accept ? 30203 : 30204, in);
    DebugLog::log("[friends] %s request %016lX rc=0x%X", accept ? "accept" : "reject",
                  (unsigned long)requestId, rc);
    return rc;
}

Result findByFriendCode(AccountUid uid, const std::string& typed, std::uint64_t& networkId,
                        std::string& nickname) {
    networkId = 0;
    nickname.clear();
    const std::string d = digitsOf(typed);
    if (d.size() != 12) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    FriendsSession fs;
    if (R_FAILED(fs.rc)) return fs.rc;
    // Same format as the console's own code (normally "SW-1234-5678-9012").
    const std::string own = ownFriendCode(uid);
    const std::string code = formatFriendCode(d, own.empty() || own.rfind("SW-", 0) == 0);

    // 30500 GetProfileExtraFromFriendCode(FriendCode, Uid) -> ProfileExtraImpl (0x400, pointer)
    struct { char code[0x20]; AccountUid uid; } in{};
    std::memcpy(in.code, code.c_str(), std::min(code.size(), sizeof(in.code) - 1));
    in.uid = uid;
    auto buf = std::make_unique<u8[]>(0x400);
    std::memset(buf.get(), 0, 0x400);
    const Result rc = serviceDispatchIn(fs.s(), 30500, in,
        .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_Out | SfBufferAttr_FixedSize},
        .buffers = {{buf.get(), 0x400}});
    DebugLog::log("[friends] lookup '%s' rc=0x%X", code.c_str(), rc);
    if (R_FAILED(rc)) return rc;
    logHex("lookup", buf.get(), 0x100);
    std::memcpy(&networkId, buf.get(), 8);
    for (size_t o = 8; o <= 0x40 && nickname.empty(); o += 8) nameAt(buf.get() + o, 0x400 - o, nickname);
    return networkId != 0 ? 0 : MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

Result sendFriendRequest(AccountUid uid, std::uint64_t networkId) {
    FriendsSession fs;
    if (R_FAILED(fs.rc)) return fs.rc;
    // 30200 SendFriendRequest(RequestType, Uid, nsa id). The RequestType values
    // aren't public: 2 is thought to be "by friend code", the others are tried
    // only if the server refuses it.
    Result rc = 0;
    for (u32 type : {2u, 1u, 3u, 4u}) {
        struct { u32 type; u32 pad; AccountUid uid; u64 networkId; } in{type, 0, uid, networkId};
        rc = serviceDispatchIn(fs.s(), 30200, in);
        DebugLog::log("[friends] send request type=%u rc=0x%X", type, rc);
        if (R_SUCCEEDED(rc)) break;
    }
    return rc;
}

int receivedFriendRequestCount(AccountUid uid) {
    FriendsSession fs;
    if (R_FAILED(fs.rc)) return -1;
    struct { s32 a, b; } counts{};
    if (R_FAILED(serviceDispatchInOut(fs.s(), 20200, uid, counts))) return -1;
    return std::max(counts.a, counts.b);
}
