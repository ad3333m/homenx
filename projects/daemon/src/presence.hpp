#pragma once
// Discord: "Playing <game>" on your profile, with the game's icon and how long, while a game runs.
// CardsNX's Settings > Internet turns it on (sdmc:/config/SwitchU/presence.json). HOME's own Discord
// connection stops during games, so this one lives here, in the daemon, which keeps running: it signs
// in with the HomeNX Discord account when a game starts and signs out when it closes, and Discord then
// takes the status away. It keeps nothing it doesn't need: the first message (megabytes for an
// account in many servers) and anything big are skipped as they arrive. Off unless turned on: setting
// a status from a user account is automating it, which Discord's rules don't allow.
#include <switch.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <switchu/file_log.hpp>
#include <switchu/tls_roots.hpp>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace switchu::daemon::presence {
namespace detail {

// The Discord application the icons are shown under (Discord Developer Portal; its ID is public).
// Empty: the status shows without an icon. presence.json's "app_id" overrides it.
inline constexpr const char* kAppId = "1575624687994798201"; // ("Nintendo Switch", Adam's)
inline constexpr const char* kUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36";
inline constexpr size_t kKeep = 16 * 1024; // messages bigger than this are skipped, never stored

inline Thread g_thread;
inline std::atomic<bool> g_run{false};
inline bool g_started = false;

struct Config {
    bool enabled = false;
    std::string appId, token;
};

inline void plog(const char* fmt, ...) {
    constexpr const char* kPath = "sdmc:/config/SwitchU/presence.log";
    FILE* f = std::fopen(kPath, "a");
    if (!f) return;
    std::fseek(f, 0, SEEK_END);
    if (std::ftell(f) > 64 * 1024) { // (kept small: the newest only)
        std::fclose(f);
        f = std::fopen(kPath, "w");
        if (!f) return;
    }
    std::fprintf(f, "[%lld] ", static_cast<long long>(std::time(nullptr)));
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fclose(f);
}

inline Config readConfig() {
    Config c;
    try {
        std::ifstream f("sdmc:/config/SwitchU/presence.json");
        if (f) {
            const auto j = nlohmann::json::parse(f, nullptr, false);
            if (j.is_object()) {
                c.enabled = j.value("enabled", false);
                c.appId = j.value("app_id", std::string());
            }
        }
        if (c.appId.empty()) c.appId = kAppId;
        std::ifstream d("sdmc:/config/SwitchU/discord.json"); // (the HomeNX Discord sign-in)
        if (d) {
            const auto j = nlohmann::json::parse(d, nullptr, false);
            if (j.is_object() && j.contains("accounts") && j["accounts"].is_array()) {
                const std::string current = j.value("current", std::string());
                for (const auto& a : j["accounts"]) {
                    if (!a.is_object()) continue;
                    const std::string token = a.value("token", std::string());
                    if (!token.empty() && (c.token.empty() || a.value("id", std::string()) == current)) c.token = token;
                }
            }
        }
    } catch (...) {
        c.enabled = false;
    }
    return c;
}

inline std::string jsonText(const std::string& s) {
    std::string out = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += static_cast<char>(ch);
        } else if (ch < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
            out += buf;
        } else {
            out += static_cast<char>(ch);
        }
    }
    return out + "\"";
}

inline std::string base64(const unsigned char* d, size_t n) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const unsigned v = (d[i] << 16) | ((i + 1 < n ? d[i + 1] : 0) << 8) | (i + 2 < n ? d[i + 2] : 0);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < n ? k[(v >> 6) & 63] : '=';
        out += i + 2 < n ? k[v & 63] : '=';
    }
    return out;
}

// A number after "key": in a small JSON message (no parser needed for op, s, heartbeat_interval).
inline long long numberAfter(const std::string& s, const char* key, long long fallback) {
    const size_t at = s.find(key);
    if (at == std::string::npos) return fallback;
    const char* p = s.c_str() + at + std::strlen(key);
    while (*p == ' ' || *p == ':') ++p;
    if (*p < '0' || *p > '9') return fallback;
    return std::strtoll(p, nullptr, 10);
}

inline size_t keepText(char* data, size_t size, size_t count, void* out) {
    auto* s = static_cast<std::string*>(out);
    if (s->size() < 64 * 1024) s->append(data, std::min(size * count, 64 * 1024 - s->size()));
    return size * count;
}

// One HTTPS request to Discord's API with the account's token. Returns the body ("" if it failed).
// The daemon has two allocators: Atmosphere's malloc/free/realloc, and newlib's own _malloc_r,
// which strdup still uses. curl strdup'ed its strings (the user agent, the URL) and free'd them
// into the other heap: HOME crashed right after the first request (2026-11-28, found with
// symbols: Curl_hash_pick read "nux x86_" as a pointer). curl gets one allocator for everything.
inline char* sameHeapStrdup(const char* s) {
    const size_t n = std::strlen(s) + 1;
    char* copy = static_cast<char*>(::malloc(n));
    if (copy) std::memcpy(copy, s, n);
    return copy;
}

// curl's own account of each step, into presence.log (what it was doing when HOME crashed).
inline int curlSays(CURL*, curl_infotype type, char* data, size_t size, void*) {
    if (type != CURLINFO_TEXT) return 0;
    std::string line(data, std::min<size_t>(size, 160));
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    plog("  curl: %s", line.c_str());
    return 0;
}

inline std::string api(const std::string& token, const std::string& path, const std::string& post = std::string()) {
    plog("api %s: start", path.c_str());
    CURL* c = curl_easy_init();
    if (!c) return {};
    curl_easy_setopt(c, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(c, CURLOPT_DEBUGFUNCTION, curlSays);
    std::string body;
    const std::string url = "https://discord.com/api/v9" + path;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: " + token).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, keepText);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    if (!post.empty()) curl_easy_setopt(c, CURLOPT_POSTFIELDS, post.c_str());
    plog("api %s: sending", path.c_str());
    const CURLcode rc = switchu::tls::perform(c, url, switchu::tls::kHomeCerts, [&body]() { body.clear(); });
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    plog("api %s: done (curl %d, HTTP %ld, %zu bytes)", path.c_str(), static_cast<int>(rc), status, body.size());
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return rc == CURLE_OK && status >= 200 && status < 300 ? body : std::string();
}

// The Discord gateway, kept small: a WebSocket over curl that keeps only small messages.
class Gateway {
public:
    ~Gateway() { close(); }
    bool isOpen() const { return m_curl != nullptr; }

    bool open() {
        close();
        CURL* c = curl_easy_init();
        if (!c) return false;
        m_curl = c;
        const std::string target = "https://gateway.discord.gg:443/";
        curl_easy_setopt(c, CURLOPT_URL, target.c_str());
        curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 1L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 12000L);
        curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(c, CURLOPT_SSL_ENABLE_ALPN, 0L);
        if (switchu::tls::perform(c, target, switchu::tls::kHomeCerts) != CURLE_OK) {
            close();
            return false;
        }
        unsigned char key[16];
        randomGet(key, sizeof(key));
        const std::string req = std::string("GET /?v=10&encoding=json HTTP/1.1\r\nHost: gateway.discord.gg\r\n")
                              + "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                              + "Sec-WebSocket-Key: " + base64(key, sizeof(key)) + "\r\n"
                              + "Origin: https://discord.com\r\nUser-Agent: " + kUserAgent + "\r\n\r\n";
        if (!sendAll(req.data(), req.size())) return false;
        std::string head;
        const u64 deadline = armGetSystemTick() + armNsToTicks(12'000'000'000ULL);
        char buf[1024];
        while (armGetSystemTick() < deadline) {
            size_t n = 0;
            const CURLcode rc = curl_easy_recv(c, buf, sizeof(buf), &n);
            if (rc == CURLE_OK && n > 0) {
                head.append(buf, n);
                const size_t end = head.find("\r\n\r\n");
                if (end == std::string::npos) {
                    if (head.size() > 16384) break;
                    continue;
                }
                if (head.compare(0, 12, "HTTP/1.1 101") != 0) break;
                feed(head.data() + end + 4, head.size() - end - 4); // (anything after it is already frames)
                return true;
            }
            if (rc != CURLE_AGAIN) break;
            svcSleepThread(5'000'000ULL);
        }
        close();
        return false;
    }

    void close() {
        if (!m_curl) return;
        const unsigned char bye[2] = {0x03, 0xE8}; // 1000: done
        sendFrame(0x8, bye, 2);
        curl_easy_cleanup(m_curl);
        m_curl = nullptr;
        m_have = 0;
        m_left = 0;
        m_skip = false;
        m_message.clear();
        m_ready.clear();
    }

    bool sendText(const std::string& s) { return sendFrame(0x1, reinterpret_cast<const unsigned char*>(s.data()), s.size()); }

    // Reads what has arrived. Complete small messages go to `out`; false once the connection is gone.
    bool poll(std::vector<std::string>& out) {
        if (!m_curl) return false;
        char buf[4096];
        for (int i = 0; i < 64; ++i) {
            size_t n = 0;
            const CURLcode rc = curl_easy_recv(m_curl, buf, sizeof(buf), &n);
            if (rc == CURLE_AGAIN) break;
            if (rc != CURLE_OK || n == 0) {
                close();
                return false;
            }
            if (!feed(buf, n)) {
                close();
                return false;
            }
        }
        out.swap(m_ready);
        m_ready.clear();
        return m_curl != nullptr;
    }

private:
    bool sendAll(const char* d, size_t n) {
        const u64 deadline = armGetSystemTick() + armNsToTicks(10'000'000'000ULL);
        while (n > 0 && m_curl) {
            size_t sent = 0;
            const CURLcode rc = curl_easy_send(m_curl, d, n, &sent);
            if (rc == CURLE_OK) {
                d += sent;
                n -= sent;
            } else if (rc == CURLE_AGAIN && armGetSystemTick() < deadline) {
                svcSleepThread(2'000'000ULL);
            } else {
                curl_easy_cleanup(m_curl);
                m_curl = nullptr;
                return false;
            }
        }
        return m_curl != nullptr;
    }

    bool sendFrame(int opcode, const unsigned char* d, size_t n) {
        if (!m_curl) return false;
        std::string f;
        f += static_cast<char>(0x80 | opcode);
        if (n < 126) {
            f += static_cast<char>(0x80 | n);
        } else if (n < 65536) {
            f += static_cast<char>(0x80 | 126);
            f += static_cast<char>((n >> 8) & 0xFF);
            f += static_cast<char>(n & 0xFF);
        } else {
            f += static_cast<char>(0x80 | 127);
            for (int i = 7; i >= 0; --i) f += static_cast<char>((static_cast<u64>(n) >> (i * 8)) & 0xFF);
        }
        unsigned char mask[4];
        randomGet(mask, sizeof(mask));
        f.append(reinterpret_cast<const char*>(mask), 4);
        for (size_t i = 0; i < n; ++i) f += static_cast<char>(d[i] ^ mask[i & 3]);
        return sendAll(f.data(), f.size());
    }

    // The frame reader: a header, then its payload (kept if small, else skipped as it streams past).
    bool feed(const char* d, size_t n) {
        while (n > 0) {
            if (m_left == 0 && m_have < 14) { // collecting a header
                m_head[m_have++] = static_cast<unsigned char>(*d++);
                --n;
                if (m_have < 2) continue;
                const unsigned len7 = m_head[1] & 0x7F;
                const size_t need = 2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0) + ((m_head[1] & 0x80) ? 4 : 0);
                if (m_have < need) continue;
                u64 len = len7;
                if (len7 == 126) len = (static_cast<u64>(m_head[2]) << 8) | m_head[3];
                if (len7 == 127) {
                    len = 0;
                    for (int i = 0; i < 8; ++i) len = (len << 8) | m_head[2 + i];
                }
                m_op = m_head[0] & 0x0F;
                m_fin = (m_head[0] & 0x80) != 0;
                m_have = 0;
                if (m_op != 0x0) { // a new message (not a continuation)
                    m_message.clear();
                    m_skip = false;
                }
                if (m_message.size() + len > kKeep && m_op <= 0x2) m_skip = true;
                m_left = len;
                if (len == 0 && !frameDone()) return false;
                continue;
            }
            const size_t take = static_cast<size_t>(std::min<u64>(m_left, n));
            if (!m_skip || m_op >= 0x8) m_message.append(d, take);
            else if (m_message.size() < 96) m_message.append(d, std::min<size_t>(take, 96 - m_message.size())); // (its start: "s")
            d += take;
            n -= take;
            m_left -= take;
            if (m_left == 0 && !frameDone()) return false;
        }
        return true;
    }

    bool frameDone() {
        if (m_op == 0x8) return false; // closed
        if (m_op == 0x9) { // ping: pong with the same
            const std::string ping = m_message;
            m_message.clear();
            return sendFrame(0xA, reinterpret_cast<const unsigned char*>(ping.data()), ping.size());
        }
        if (m_op == 0xA) {
            m_message.clear();
            return true;
        }
        if (m_fin) {
            if (m_skip) m_ready.push_back("{\"skipped\":1," + m_message.substr(1 < m_message.size() ? 1 : 0)); // (op and s only)
            else m_ready.push_back(m_message);
            m_message.clear();
            m_skip = false;
        }
        return true;
    }

    CURL* m_curl = nullptr;
    unsigned char m_head[14]{};
    size_t m_have = 0;
    u64 m_left = 0;
    int m_op = 0;
    bool m_fin = true, m_skip = false;
    std::string m_message;
    std::vector<std::string> m_ready;
};

// The game running now (0: none), from the process manager.
inline u64 runningGame() {
    u64 pid = 0, tid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&pid)) || R_FAILED(pminfoGetProgramId(&tid, pid))) return 0;
    return tid;
}

inline std::string gameName(u64 tid) {
    auto* data = new (std::nothrow) NsApplicationControlData();
    if (!data) return {};
    u64 size = 0;
    std::string name;
    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, tid & ~0xFFFULL, data, sizeof(*data), &size))) {
        NacpLanguageEntry* entry = nullptr;
        if (R_SUCCEEDED(nacpGetLanguageEntry(&data->nacp, &entry)) && entry) name = entry->name;
    }
    delete data;
    return name;
}

// Its icon as Discord shows external pictures ("mp:external/..."), asked once per game.
inline std::string gameIcon(const Config& cfg, u64 tid) {
    static std::map<u64, std::string> known;
    if (cfg.appId.empty()) return {};
    auto it = known.find(tid);
    if (it != known.end()) return it->second;
    char url[96];
    std::snprintf(url, sizeof(url), "https://tinfoil.media/ti/%016lX/512/512/", static_cast<unsigned long>(tid & ~0xFFFULL));
    std::string path;
    const std::string body = api(cfg.token, "/applications/" + cfg.appId + "/external-assets",
                                 std::string("{\"urls\":[") + jsonText(url) + "]}");
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_array() && !j.empty() && j[0].is_object()) path = j[0].value("external_asset_path", std::string());
    if (!path.empty()) path = "mp:" + path;
    known[tid] = path;
    return path;
}

inline std::string presenceJson(const Config& cfg, const std::string& status, const std::string& name, const std::string& icon,
                                long long startMs) {
    std::string activity = "{\"name\":" + jsonText(name.empty() ? std::string("a game") : name) + ",\"type\":0"
                         + ",\"state\":\"on Nintendo Switch\",\"timestamps\":{\"start\":" + std::to_string(startMs) + "}";
    if (!cfg.appId.empty()) activity += ",\"application_id\":" + jsonText(cfg.appId);
    if (!icon.empty())
        activity += ",\"assets\":{\"large_image\":" + jsonText(icon) + ",\"large_text\":" + jsonText(name) + "}";
    activity += "}";
    return "{\"status\":" + jsonText(status) + ",\"since\":0,\"activities\":[" + activity + "],\"afk\":false}";
}

inline void threadFunc(void*) {
    // Small socket buffers: this is a few messages a minute.
    SocketInitConfig net = *socketGetDefaultInitConfig();
    net.tcp_tx_buf_size = 0x2000;
    net.tcp_rx_buf_size = 0x4000;
    net.tcp_tx_buf_max_size = 0x4000;
    net.tcp_rx_buf_max_size = 0x8000;
    net.udp_tx_buf_size = 0x800;
    net.udp_rx_buf_size = 0x800;
    net.sb_efficiency = 1;
    net.num_bsd_sessions = 1;
    bool netUp = false, pmUp = false;
    Gateway gw;
    Config cfg;
    u64 cfgAt = 0, retryAt = 0, beatAt = 0, beatEvery = 0;
    u64 shownTid = 0, startedTid = 0;
    long long seq = -1, startMs = 0;
    int failures = 0;
    std::string status = "online", name, icon;
    auto now = []() { return armTicksToNs(armGetSystemTick()) / 1'000'000ULL; }; // ms
    std::vector<std::string> msgs;
    while (g_run.load()) {
        try { // (nothing in here may take HOME down: anything unexpected waits and tries again)
        const u64 t = now();
        if (!pmUp) pmUp = R_SUCCEEDED(pmdmntInitialize()) && R_SUCCEEDED(pminfoInitialize());
        if (t >= cfgAt) {
            cfg = readConfig();
            cfgAt = t + 15000;
        }
        const u64 tid = pmUp ? runningGame() : 0;
        if (!cfg.enabled || cfg.token.empty() || tid == 0) { // nothing to show: signed out (the status goes)
            if (gw.isOpen()) {
                gw.close();
                switchu::FileLog::log("[presence] game closed: signed out");
                plog("game closed: signed out");
            }
            shownTid = 0;
            svcSleepThread(1'000'000'000ULL);
            continue;
        }
        if (tid != startedTid) { // a game started: its name, when, and the account's own status
            startedTid = tid;
            plog("game %016lX started", static_cast<unsigned long>(tid));
            name = gameName(tid);
            plog("its name: %s", name.c_str());
            startMs = static_cast<long long>(std::time(nullptr)) * 1000;
            icon.clear();
        }
        if (!gw.isOpen() && t >= retryAt) { // nothing started without room to spare (HOME itself needs it)
            void* probe = std::malloc(3u << 20);
            const bool roomy = probe != nullptr;
            std::free(probe);
            if (!roomy) {
                plog("under 3 MB free: not connecting, trying again in 30 s");
                retryAt = t + 30000;
                svcSleepThread(1'000'000'000ULL);
                continue;
            }
        }
        if (!netUp) {
            plog("starting the network");
            netUp = R_SUCCEEDED(socketInitialize(&net));
            plog(netUp ? "network up" : "network didn't start");
            if (netUp) curl_global_init_mem(CURL_GLOBAL_DEFAULT, ::malloc, ::free, ::realloc, sameHeapStrdup, ::calloc);
            if (!netUp) {
                svcSleepThread(5'000'000'000ULL);
                continue;
            }
        }
        if (!gw.isOpen()) {
            if (t < retryAt) {
                svcSleepThread(250'000'000ULL);
                continue;
            }
            // Invisible on Discord stays invisible: nothing is shown then.
            plog("asking Discord for your status");
            const auto settings = nlohmann::json::parse(api(cfg.token, "/users/@me/settings"), nullptr, false);
            status = settings.is_object() && settings.contains("status") && settings["status"].is_string()
                ? settings["status"].get<std::string>() : std::string("online");
            if (status == "invisible") {
                retryAt = t + 60000;
                continue;
            }
            plog("status: %s; getting the game's icon", status.c_str());
            if (icon.empty()) icon = gameIcon(cfg, tid);
            plog("connecting to the gateway");
            if (!gw.open()) {
                plog("the gateway didn't open");
                ++failures;
                retryAt = t + (failures > 8 ? 600000ULL : std::min<u64>(60000, 5000ULL * static_cast<u64>(failures)));
                switchu::FileLog::log("[presence] couldn't reach Discord (try %d)", failures);
                continue;
            }
            seq = -1;
            beatEvery = 0;
            shownTid = 0;
        }
        if (beatEvery && shownTid != tid) { // signed in: another game now
            if (icon.empty()) icon = gameIcon(cfg, tid);
            gw.sendText("{\"op\":3,\"d\":" + presenceJson(cfg, status, name, icon, startMs) + "}");
            shownTid = tid;
        }
        msgs.clear();
        if (!gw.poll(msgs)) {
            ++failures;
            retryAt = now() + std::min<u64>(60000, 5000ULL * static_cast<u64>(failures));
            continue;
        }
        for (const std::string& m : msgs) {
            const long long s = numberAfter(m, "\"s\"", -1);
            if (s >= 0) seq = s;
            switch (numberAfter(m, "\"op\"", -1)) {
                case 10: { // hello: how often to beat; then sign in, with the game as the status
                    beatEvery = static_cast<u64>(numberAfter(m, "\"heartbeat_interval\"", 41250));
                    beatAt = now() + beatEvery / 2;
                    gw.sendText("{\"op\":2,\"d\":{\"token\":" + jsonText(cfg.token) + ",\"capabilities\":51,\"properties\":"
                                + "{\"os\":\"Linux\",\"browser\":\"Chrome\",\"device\":\"\",\"system_locale\":\"en-US\","
                                + "\"browser_user_agent\":" + jsonText(kUserAgent) + ",\"browser_version\":\"134.0.0.0\","
                                + "\"os_version\":\"\",\"referrer\":\"\",\"referring_domain\":\"\",\"release_channel\":\"stable\"},"
                                + "\"presence\":" + presenceJson(cfg, status, name, icon, startMs) + ",\"compress\":false}}");
                    shownTid = tid;
                    failures = 0;
                    switchu::FileLog::log("[presence] signed in: showing %016lX", static_cast<unsigned long>(tid));
                    plog("signed in, showing %016lX", static_cast<unsigned long>(tid));
                    break;
                }
                case 1: beatAt = 0; break; // asked for a beat now
                case 7:
                case 9: // reconnect / the session isn't valid: sign in again, later each time
                    gw.close();
                    ++failures;
                    retryAt = now() + (failures > 8 ? 600000ULL : std::min<u64>(60000, 5000ULL * static_cast<u64>(failures)));
                    break;
                default: break;
            }
        }
        if (beatEvery && now() >= beatAt) {
            gw.sendText(seq >= 0 ? "{\"op\":1,\"d\":" + std::to_string(seq) + "}" : std::string("{\"op\":1,\"d\":null}"));
            beatAt = now() + beatEvery;
        }
        svcSleepThread(100'000'000ULL);
        } catch (...) {
            gw.close();
            retryAt = now() + 30000;
            switchu::FileLog::log("[presence] something went wrong: trying again in 30 s");
            plog("something went wrong: trying again in 30 s");
            svcSleepThread(1'000'000'000ULL);
        }
    }
    gw.close();
    if (netUp) {
        curl_global_cleanup();
        socketExit();
    }
    if (pmUp) {
        pminfoExit();
        pmdmntExit();
    }
}

} // namespace detail

inline void start() {
    if (detail::g_started) return;
    detail::g_run.store(true);
    // Low priority, a roomy stack (TLS needs it).
    if (R_SUCCEEDED(threadCreate(&detail::g_thread, detail::threadFunc, nullptr, nullptr, 0x80000, 0x3F, -2))
        && R_SUCCEEDED(threadStart(&detail::g_thread))) {
        detail::g_started = true;
    }
}

inline void stop() {
    if (!detail::g_started) return;
    detail::g_run.store(false);
    threadWaitForExit(&detail::g_thread);
    threadClose(&detail::g_thread);
    detail::g_started = false;
}

} // namespace switchu::daemon::presence
