#include "NewsStore.hpp"
#include "core/BackgroundThread.hpp"
#include "core/DebugLog.hpp"
#include <nlohmann/json.hpp>
#include <nxui/core/ThreadPool.hpp>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <functional>
#include <set>
#include <thread>

// Uses libnx's news:* wrapper (services/news.h). Article files are msgpack
// (subject.text, body.text, topic_name, images as bin).
namespace {

// Enough to always include the newest: the list can come back unsorted, and
// with only the first 64 of a long history the newest articles were left out.
constexpr u32 kMaxRecords = 300;
constexpr u64 kMaxArticleBytes = 8u * 1024u * 1024u;

Result getList(NewsDatabaseService& db, NewsRecord* records, u32& count) {
    // The SQL phrases aren't documented; try forms that are valid either way
    // and sort by received_at ourselves. One can succeed with nothing while
    // another finds every article (the first success used to be taken, and
    // News said there was none): keep the form that finds the most.
    static const char* const attempts[][2] = {
        {"", "received_at DESC"}, {"", ""}, {"1=1", "received_at DESC"}, {"1=1", ""}, {"1", ""},
    };
    Result rc = 0, firstRc = 0;
    int best = -1;
    u32 bestCount = 0;
    for (int i = 0; i < (int)(sizeof(attempts) / sizeof(attempts[0])); ++i) {
        u32 n = 0;
        rc = newsDatabaseGetList(&db, records, kMaxRecords, attempts[i][0], attempts[i][1], &n, 0);
        DebugLog::log("[news] GetList where='%s' order='%s' rc=0x%X count=%u", attempts[i][0], attempts[i][1], rc, n);
        if (i == 0) firstRc = rc;
        if (R_SUCCEEDED(rc) && (best < 0 || n > bestCount)) {
            best = i;
            bestCount = n;
        }
    }
    count = 0;
    if (best < 0) return firstRc;
    if (best == (int)(sizeof(attempts) / sizeof(attempts[0])) - 1 && R_SUCCEEDED(rc)) { // already in `records`
        count = bestCount;
        return 0;
    }
    rc = newsDatabaseGetList(&db, records, kMaxRecords, attempts[best][0], attempts[best][1], &count, 0);
    return rc;
}

Result readArticle(NewsRecord& record, std::vector<u8>& out) {
    NewsDataService data;
    Result rc = newsCreateNewsDataService(&data);
    if (R_FAILED(rc)) return rc;
    rc = newsDataOpenWithNewsRecord(&data, &record);
    u64 size = 0;
    if (R_SUCCEEDED(rc)) rc = newsDataGetSize(&data, &size);
    if (R_SUCCEEDED(rc) && (size == 0 || size > kMaxArticleBytes))
        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_SUCCEEDED(rc)) {
        out.resize(size);
        u64 done = 0;
        while (R_SUCCEEDED(rc) && done < size) {
            u64 got = 0;
            rc = newsDataRead(&data, &got, done, out.data() + done, std::min<u64>(size - done, 0x40000));
            if (got == 0) break;
            done += got;
        }
        out.resize(done);
    }
    newsDataClose(&data);
    return rc;
}

std::string textOf(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_object()) {
        auto t = it->find("text");
        if (t != it->end() && t->is_string()) return t->get<std::string>();
    }
    return {};
}

std::vector<u8> binaryOf(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_binary()) return {};
    const auto& bin = it->get_binary();
    return std::vector<u8>(bin.begin(), bin.end());
}

bool parseArticle(const std::vector<u8>& bytes, NewsArticle& a) {
    auto j = nlohmann::json::from_msgpack(bytes, true, false);
    if (j.is_discarded() || !j.is_object()) return false;
    a.subject = textOf(j, "subject");
    a.topic = textOf(j, "topic_name");
    a.footer = textOf(j, "footer");
    if (auto it = j.find("published_at"); it != j.end() && it->is_number())
        a.publishedAt = it->get<std::uint64_t>();
    if (a.publishedAt > 100000000000ull) a.publishedAt /= 1000; // milliseconds
    if (auto body = j.find("body"); body != j.end() && body->is_object()) {
        a.body = textOf(*body, "text");
        a.image = binaryOf(*body, "main_image");
    }
    if (a.image.empty()) a.image = binaryOf(j, "list_image");
    if (auto more = j.find("more"); more != j.end() && more->is_object())
        if (auto br = more->find("browser"); br != more->end() && br->is_object())
            a.linkUrl = textOf(*br, "url");
    if (a.subject.empty() && a.body.empty()) {
        // HomeNX: a layout these keys don't fit (Nintendo changes it now and
        // then, and every article was skipped: "no news"). Take a title-ish
        // string, the longest text and the first JPEG, wherever they are.
        std::string title, longest;
        std::vector<u8> jpeg;
        std::function<void(const nlohmann::json&, const std::string&)> walk = [&](const nlohmann::json& v,
                                                                                 const std::string& key) {
            if (v.is_string()) {
                const std::string& text = v.get_ref<const std::string&>();
                if (text.rfind("http", 0) == 0) return;
                if (title.empty() && (key.find("subject") != std::string::npos || key.find("title") != std::string::npos))
                    title = text;
                if (text.size() > longest.size()) longest = text;
            } else if (v.is_binary()) {
                const auto& bin = v.get_binary();
                if (jpeg.empty() && bin.size() > 2 && bin[0] == 0xFF && bin[1] == 0xD8) jpeg.assign(bin.begin(), bin.end());
            } else if (v.is_object()) {
                for (auto it = v.begin(); it != v.end(); ++it) walk(it.value(), key + "/" + it.key());
            } else if (v.is_array()) {
                for (const auto& e : v) walk(e, key);
            }
        };
        walk(j, "");
        a.subject = title;
        if (longest != title) a.body = longest;
        if (a.image.empty()) a.image = std::move(jpeg);
        std::string keys;
        for (auto it = j.begin(); it != j.end() && keys.size() < 160; ++it) keys += it.key() + " ";
        DebugLog::log("[news] other layout, keys: %s", keys.c_str());
    }
    return !a.subject.empty() || !a.body.empty();
}

} // namespace

std::vector<NewsArticle> loadNewsArticles(Result& rc, int maxArticles) {
    std::vector<NewsArticle> articles;
    rc = newsInitialize(NewsServiceType_Administrator);
    if (R_FAILED(rc)) {
        DebugLog::log("[news] newsInitialize rc=0x%X", rc);
        return articles;
    }
    NewsDatabaseService db;
    rc = newsCreateNewsDatabaseService(&db);
    if (R_SUCCEEDED(rc)) {
        static NewsRecord records[kMaxRecords];
        u32 count = 0;
        rc = getList(db, records, count);
        newsDatabaseClose(&db);
        count = std::min(count, kMaxRecords);
        std::sort(records, records + count, [](const NewsRecord& a, const NewsRecord& b) {
            return a.received_at > b.received_at;
        });
        for (u32 i = 0; i < count && (int)articles.size() < maxArticles; ++i) {
            std::vector<u8> bytes;
            Result r = readArticle(records[i], bytes);
            NewsArticle a;
            if (R_SUCCEEDED(r) && parseArticle(bytes, a)) {
                a.id.assign(records[i].news_id,
                            std::find(records[i].news_id, records[i].news_id + sizeof(records[i].news_id), '\0'));
                a.read = records[i].read != 0;
                if (a.publishedAt == 0 && records[i].received_at > 0)
                    a.publishedAt = static_cast<std::uint64_t>(records[i].received_at);
                articles.push_back(std::move(a));
            }
            else
                DebugLog::log("[news] article %.24s rc=0x%X bytes=%d", records[i].news_id, r, (int)bytes.size());
        }
    }
    newsExit();
    std::stable_sort(articles.begin(), articles.end(),
                     [](const NewsArticle& a, const NewsArticle& b) { return a.publishedAt > b.publishedAt; });
    DebugLog::log("[news] loaded %d articles rc=0x%X", (int)articles.size(), rc);
    return articles;
}

void requestNewsUpdate(int minIntervalSec) {
    constexpr const char* kStamp = "sdmc:/config/SwitchU/news_requested.txt";
    const long long now = static_cast<long long>(std::time(nullptr));
    long long last = 0;
    {
        std::ifstream in(kStamp);
        in >> last;
    }
    if (now >= last && now - last < minIntervalSec) return;
    std::ofstream(kStamp, std::ios::trunc) << now;
    runInBackground([] {
        nxui::moveToWorkerCores();
        if (R_FAILED(newsInitialize(NewsServiceType_Administrator))) return;
        // Every topic the console has news for (plus Nintendo's main one).
        std::set<std::string> topics{"nx_news"};
        NewsDatabaseService db;
        if (R_SUCCEEDED(newsCreateNewsDatabaseService(&db))) {
            std::vector<NewsRecord> records(kMaxRecords);
            u32 count = 0;
            if (R_SUCCEEDED(getList(db, records.data(), count)))
                for (u32 i = 0; i < std::min(count, kMaxRecords); ++i) {
                    const NewsTopicName& t = records[i].topic_id;
                    std::string name(t.name, std::find(t.name, t.name + sizeof(t.name), '\0'));
                    if (!name.empty()) topics.insert(name);
                }
            newsDatabaseClose(&db);
        }
        for (const std::string& t : topics) {
            const Result rc = newsRequestImmediateReception(t.c_str());
            DebugLog::log("[news] request reception '%s' rc=0x%X", t.c_str(), rc);
        }
        newsExit();
    });
}
