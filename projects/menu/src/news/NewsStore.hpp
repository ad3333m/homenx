#pragma once
#include <switch.h>
#include <cstdint>
#include <string>
#include <vector>

// The console's own HOME-menu news (the articles qlaunch's News shows), read
// through the news:a service. The news save is held open by bcat, so the
// service is the only way in (mounting it gives 2002-0007).
struct NewsArticle {
    std::string topic;     // channel name, e.g. "Nintendo Switch"
    std::string subject;   // headline
    std::string body;
    std::string footer;
    std::string linkUrl;   // "more" browser button, if any
    std::string id;             // news_id, for remembering what was read
    bool read = false;          // the console's own "read" flag
    std::uint64_t publishedAt = 0; // POSIX seconds (published, else received)
    std::vector<std::uint8_t> image; // JPEG: body main image, else the list image
};

// What loading found, for News to show when it comes up empty.
struct NewsLoadStats {
    unsigned records = 0;   // in the console's news list
    unsigned readFails = 0; // records whose article couldn't be read
    unsigned parseFails = 0; // articles read but not understood
    Result lastReadRc = 0;
};

// Newest first (oldest last). On failure returns empty and sets rc.
std::vector<NewsArticle> loadNewsArticles(Result& rc, int maxArticles = 40, NewsLoadStats* stats = nullptr);

// Asks the console to download news now, on its own thread. The real HOME menu
// does this; without it the articles saved on the console stay the old ones.
// At most once per minIntervalSec (remembered on the SD card).
void requestNewsUpdate(int minIntervalSec = 600);
