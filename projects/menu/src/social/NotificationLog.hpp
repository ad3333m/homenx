#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

// HomeNX's notification centre: what HOME and the Discord overlay told you, one
// line each in sdmc:/config/SwitchU/notifications.txt (time, kind, title, text,
// tab-separated; the overlay appends the same way while a game is on). The bell
// counts the ones newer than notifications_seen.txt (when it was last opened).
namespace notifications {

struct Item {
    std::uint64_t at = 0;
    std::string kind, title, text;
};

inline constexpr const char* kPath = "sdmc:/config/SwitchU/notifications.txt";
inline constexpr const char* kSeenPath = "sdmc:/config/SwitchU/notifications_seen.txt";

inline std::string flat(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s.size() > 300 ? s.substr(0, 300) : s;
}

inline void add(const char* kind, const std::string& title, const std::string& text) {
    if (FILE* f = std::fopen(kPath, "a")) {
        std::fprintf(f, "%llu\t%s\t%s\t%s\n", (unsigned long long)std::time(nullptr), kind, flat(title).c_str(),
                     flat(text).c_str());
        std::fclose(f);
    }
}

// Newest first, at most `max`. Keeps the file to its last 100 lines.
inline std::vector<Item> recent(std::size_t max) {
    std::vector<Item> all;
    FILE* f = std::fopen(kPath, "r");
    if (!f) return all;
    char buf[2048];
    while (std::fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        std::vector<std::string> parts;
        std::size_t from = 0;
        for (int i = 0; i < 3; ++i) {
            const std::size_t tab = line.find('\t', from);
            if (tab == std::string::npos) break;
            parts.push_back(line.substr(from, tab - from));
            from = tab + 1;
        }
        if (parts.size() != 3) continue;
        all.push_back({std::strtoull(parts[0].c_str(), nullptr, 10), parts[1], parts[2], line.substr(from)});
    }
    std::fclose(f);
    if (all.size() > 200 && (f = std::fopen(kPath, "w"))) {
        all.erase(all.begin(), all.end() - 100);
        for (const Item& it : all)
            std::fprintf(f, "%llu\t%s\t%s\t%s\n", (unsigned long long)it.at, it.kind.c_str(), it.title.c_str(),
                         it.text.c_str());
        std::fclose(f);
    }
    std::reverse(all.begin(), all.end());
    if (all.size() > max) all.resize(max);
    return all;
}

inline std::uint64_t seenAt() {
    unsigned long long at = 0;
    if (FILE* f = std::fopen(kSeenPath, "r")) {
        if (std::fscanf(f, "%llu", &at) != 1) at = 0;
        std::fclose(f);
    }
    return at;
}

inline void markSeen() {
    if (FILE* f = std::fopen(kSeenPath, "w")) {
        std::fprintf(f, "%llu", (unsigned long long)std::time(nullptr));
        std::fclose(f);
    }
}

inline int unreadCount() {
    const std::uint64_t seen = seenAt();
    int n = 0;
    for (const Item& it : recent(100))
        if (it.at > seen) ++n;
    return n;
}

inline void clear() {
    std::remove(kPath);
}

} // namespace notifications
