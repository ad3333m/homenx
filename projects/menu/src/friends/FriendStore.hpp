#pragma once
#include <switch.h>
#include <cstdint>
#include <string>
#include <vector>

// The friend list the console keeps for a user (works offline from the
// friends service's cache), read straight from IFriendService.
struct FriendEntry {
    std::string nickname;
    std::uint32_t status = 0;   // 0 offline, 1 online, 2 playing online
    std::int64_t lastOnline = 0; // POSIX seconds, 0 if unknown
    bool favourite = false;
    std::vector<std::uint8_t> avatar; // JPEG from the friends cache, may be empty
};

// Online first, then favourites, then by name. On failure returns empty and sets rc.
// `diag` describes what the service answered, for showing when the list is empty.
std::vector<FriendEntry> loadFriends(AccountUid uid, Result& rc, std::string& diag);
