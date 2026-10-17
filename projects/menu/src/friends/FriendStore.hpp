#pragma once
#include <switch.h>
#include <cstdint>
#include <string>
#include <vector>

// The friend list the console keeps for a user (works offline from the
// friends service's cache), read straight from IFriendService.
struct FriendEntry {
    std::string nickname;
    std::uint64_t networkId = 0; // the friend's Nintendo Account (network service) id
    std::uint32_t status = 0;   // 0 offline, 1 online, 2 playing online
    std::int64_t lastOnline = 0; // POSIX seconds, 0 if unknown
    bool favourite = false;
    std::vector<std::uint8_t> avatar; // JPEG from the friends cache, may be empty
};

// Online first, then favourites, then by name. On failure returns empty and sets rc.
// `diag` describes what the service answered, for showing when the list is empty.
std::vector<FriendEntry> loadFriends(AccountUid uid, Result& rc, std::string& diag);

// One game in someone's play history.
struct PlayRecord {
    std::uint64_t titleId = 0;
    std::uint64_t minutes = 0;    // total play time
    std::uint32_t launches = 0;
    std::int64_t firstPlayed = 0; // POSIX seconds, 0 if unknown
    std::int64_t lastPlayed = 0;
    bool haveTime = false;
};

// "3 days ago", "12 Oct 2026, 18:05", "4 h 20 min" (in SocialScreen.cpp).
std::string ago(std::int64_t when);
std::string dateOf(std::int64_t when);
std::string hoursText(std::uint64_t minutes);

// "SW-1234-5678-9012", or empty if the console doesn't know it yet.
std::string loadFriendCode(AccountUid uid);

// This console's play records (pdm) for the given games, most recent first.
std::vector<PlayRecord> loadMyPlayHistory(AccountUid uid, const std::vector<std::uint64_t>& titleIds);

// A friend's play activity, as the friends service has it (only what they share).
std::vector<PlayRecord> loadFriendPlayLog(AccountUid uid, std::uint64_t networkId, Result& rc);

struct FriendRequestEntry {
    std::uint64_t requestId = 0;
    std::uint64_t networkId = 0;
    std::string nickname;
};

// Friend requests sent to this user. These need Nintendo's servers.
std::vector<FriendRequestEntry> loadFriendRequests(AccountUid uid, Result& rc);
Result answerFriendRequest(AccountUid uid, std::uint64_t requestId, bool accept);

// Looks a friend code up; fills the account's id and name.
Result findByFriendCode(AccountUid uid, const std::string& code, std::uint64_t& networkId,
                        std::string& nickname);
Result sendFriendRequest(AccountUid uid, std::uint64_t networkId);

int receivedFriendRequestCount(AccountUid uid); // -1 if unknown
