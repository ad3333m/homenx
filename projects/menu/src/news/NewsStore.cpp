#include "NewsStore.hpp"
#include "core/DebugLog.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>

// IPC layouts from switchbrew "BCAT services" (news:*) and the yuzu/citron
// HLE signatures:
//   IServiceCreator   2 CreateNewsDataService, 3 CreateNewsDatabaseService
//   INewsDatabaseService 1000 GetList(u32 offset, out records[], where, order) -> s32 count
//   INewsDataService  1001 OpenWithNewsRecord(record), 3 GetSize -> u64,
//                     2 Read(u64 offset, out buf) -> u64 read
// Article files are msgpack (subject.text, body.text, topic_name, images as bin).
namespace {

struct NewsRecord {
    char news_id[0x18];
    char user_id[0x18];
    char topic_id[0x20];
    s64 received_id;
    u8 reserved[0x8];
    s32 decoration_type, read, newly, displayed, feedback, reserved2, extra1, extra2;
};
static_assert(sizeof(NewsRecord) == 0x80);

constexpr int kMaxRecords = 64;
constexpr u64 kMaxArticleBytes = 8u * 1024u * 1024u;

Result getList(Service& db, NewsRecord* records, s32& count) {
    // The SQL phrases aren't documented; try the forms that are valid either
    // way and sort by published_at ourselves.
    static const char* const attempts[][2] = {
        {"", ""}, {"1=1", ""}, {"", "received_id DESC"}, {"1=1", "received_id DESC"},
    };
    Result rc = 0;
    for (const auto& a : attempts) {
        const u32 offset = 0;
        count = 0;
        rc = serviceDispatchInOut(&db, 1000, offset, count,
            .buffer_attrs = {
                SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
                SfBufferAttr_HipcPointer | SfBufferAttr_In,
                SfBufferAttr_HipcPointer | SfBufferAttr_In,
            },
            .buffers = {
                {records, sizeof(NewsRecord) * kMaxRecords},
                {a[0], std::strlen(a[0]) + 1},
                {a[1], std::strlen(a[1]) + 1},
            });
        DebugLog::log("[news] GetList where='%s' order='%s' rc=0x%X count=%d", a[0], a[1], rc, count);
        if (R_SUCCEEDED(rc)) return rc;
    }
    return rc;
}

Result readArticle(Service& creator, const NewsRecord& record, std::vector<u8>& out) {
    Service data;
    Result rc = serviceDispatch(&creator, 2, .out_num_objects = 1, .out_objects = &data);
    if (R_FAILED(rc)) return rc;
    rc = serviceDispatch(&data, 1001,
        .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_In},
        .buffers = {{&record, sizeof(record)}});
    u64 size = 0;
    if (R_SUCCEEDED(rc)) rc = serviceDispatchOut(&data, 3, size);
    if (R_SUCCEEDED(rc) && (size == 0 || size > kMaxArticleBytes)) rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_SUCCEEDED(rc)) {
        out.resize(size);
        u64 done = 0;
        while (R_SUCCEEDED(rc) && done < size) {
            u64 got = 0;
            rc = serviceDispatchInOut(&data, 2, done, got,
                .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                .buffers = {{out.data() + done, std::min<u64>(size - done, 0x40000)}});
            if (got == 0) break;
            done += got;
        }
        out.resize(done);
    }
    serviceClose(&data);
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
    if (auto body = j.find("body"); body != j.end() && body->is_object()) {
        a.body = textOf(*body, "text");
        a.image = binaryOf(*body, "main_image");
    }
    if (a.image.empty()) a.image = binaryOf(j, "list_image");
    if (auto more = j.find("more"); more != j.end() && more->is_object())
        if (auto br = more->find("browser"); br != more->end() && br->is_object())
            a.linkUrl = textOf(*br, "url");
    return !a.subject.empty() || !a.body.empty();
}

} // namespace

std::vector<NewsArticle> loadNewsArticles(Result& rc, int maxArticles) {
    std::vector<NewsArticle> articles;
    Service creator, db;
    rc = smGetService(&creator, "news:a");
    if (R_FAILED(rc)) {
        DebugLog::log("[news] news:a rc=0x%X", rc);
        return articles;
    }
    rc = serviceDispatch(&creator, 3, .out_num_objects = 1, .out_objects = &db);
    if (R_SUCCEEDED(rc)) {
        static NewsRecord records[kMaxRecords];
        s32 count = 0;
        rc = getList(db, records, count);
        serviceClose(&db);
        count = std::clamp(count, 0, kMaxRecords);
        for (s32 i = 0; i < count && (int)articles.size() < maxArticles; ++i) {
            std::vector<u8> bytes;
            Result r = readArticle(creator, records[i], bytes);
            NewsArticle a;
            if (R_SUCCEEDED(r) && parseArticle(bytes, a))
                articles.push_back(std::move(a));
            else
                DebugLog::log("[news] article %.24s rc=0x%X bytes=%d", records[i].news_id, r, (int)bytes.size());
        }
    }
    serviceClose(&creator);
    std::stable_sort(articles.begin(), articles.end(),
                     [](const NewsArticle& a, const NewsArticle& b) { return a.publishedAt > b.publishedAt; });
    DebugLog::log("[news] loaded %d articles rc=0x%X", (int)articles.size(), rc);
    return articles;
}
