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
    std::uint64_t publishedAt = 0;
    std::vector<std::uint8_t> image; // JPEG: body main image, else the list image
};

// Newest first. On failure returns empty and sets rc.
std::vector<NewsArticle> loadNewsArticles(Result& rc, int maxArticles = 40);
