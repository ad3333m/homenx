#pragma once
#include <switch.h>
#include <string>
#include <vector>

// The console's Miis through mii:e: list, make (random), rename, delete.
// Faces come from Nintendo's Mii Studio renderer (needs the internet).
struct MiiEntry {
    MiiCharInfo info{};
    std::string name;    // UTF-8
    std::string faceUrl; // a PNG of the face
};

// Your own Miis (not the built-in ones), in the console's order.
Result loadMiis(std::vector<MiiEntry>& out);
// gender: 0 male, 1 female, 2 either.
Result buildRandomMii(int gender, MiiEntry& out);
Result addMii(MiiCharInfo info, const std::string& name);
// Keeps the Mii (same id) when the console accepts it, else re-adds it in the same spot.
Result renameMii(const MiiEntry& mii, int index, const std::string& name);
Result deleteMii(const MiiCharInfo& info);
std::string miiFaceUrl(const MiiCharInfo& info);
