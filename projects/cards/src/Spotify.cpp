#ifdef SWITCHU_MENU
#include "Spotify.hpp"
#include "Library.hpp"
#include "Settings.hpp"

#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <nlohmann/json.hpp>
#include <nxui/core/ThreadPool.hpp>
#include <nxui/third_party/stb/stb_image.h>
#include <switch.h>
#include <switchu/tls_roots.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

namespace cards {
namespace {

constexpr const char* kFile = "sdmc:/config/CardsNX/spotify.json";
constexpr const char* kRedirect = "https://homenx.pages.dev/spotify/";
constexpr const char* kScopes = "user-read-playback-state user-modify-playback-state user-read-currently-playing";
// A Client ID built in (its owner adds up to 25 people in Spotify's dashboard); else each person's own.
constexpr const char* kBuiltInClientId = "";
constexpr int kPort = 8955; // this console's sign-in page

size_t toString(char* p, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, size * n);
    return size * n;
}

// One request; the HTTP status (0: no answer).
long request(const std::string& method, const std::string& url, const std::string& body,
             const std::vector<std::string>& headers, std::string& out) {
    out.clear();
    CURL* c = curl_easy_init();
    if (!c) return 0;
    curl_slist* list = nullptr;
    for (const std::string& h : headers) list = curl_slist_append(list, h.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "HomeNX");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, toString);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    if (list) curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    if (method != "GET") {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    CURLcode rc = switchu::tls::perform(c, url, switchu::tls::kHomeCerts, [&out]() { out.clear(); });
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(list);
    curl_easy_cleanup(c);
    return status;
}

std::string enc(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string dec(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

std::string param(const std::string& query, const std::string& name) {
    size_t at = 0;
    while (at < query.size()) {
        const size_t end = query.find('&', at);
        const std::string kv = query.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const size_t eq = kv.find('=');
        if (kv.substr(0, eq) == name) return eq == std::string::npos ? std::string() : dec(kv.substr(eq + 1));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return {};
}

std::string base64url(const unsigned char* d, size_t n) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const unsigned v = (static_cast<unsigned>(d[i]) << 16) | (i + 1 < n ? static_cast<unsigned>(d[i + 1]) << 8 : 0u)
                         | (i + 2 < n ? static_cast<unsigned>(d[i + 2]) : 0u);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        if (i + 1 < n) out += k[(v >> 6) & 63];
        if (i + 2 < n) out += k[v & 63];
    }
    return out;
}

std::string randomText(size_t n) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::vector<unsigned char> raw(n);
    randomGet(raw.data(), raw.size());
    std::string out;
    for (unsigned char c : raw) out += k[c % 62];
    return out;
}

long long nowSeconds() { return static_cast<long long>(std::time(nullptr)); }

void sendAll(int fd, const std::string& s) {
    size_t sent = 0;
    while (sent < s.size()) {
        const ssize_t n = send(fd, s.data() + sent, s.size() - sent, 0);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

void page(int fd, const char* title, const char* text) {
    const std::string body = std::string("<!doctype html><meta name=viewport content='width=device-width'><title>HomeNX</title>"
        "<body style='font-family:sans-serif;background:#0d1430;color:#eef0fb;text-align:center;padding:48px 20px'>"
        "<h2>") + title + "</h2><p>" + text + "</p>";
    sendAll(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: "
                + std::to_string(body.size()) + "\r\n\r\n" + body);
}

} // namespace

void Spotify::start() {
    if (m_thread.joinable()) return;
    load();
    m_quit = false;
    m_thread = std::thread([this]() { run(); });
}

void Spotify::stop() {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_quit = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    closeServer();
}

void Spotify::load() {
    std::lock_guard<std::mutex> lk(m_mu);
    try {
        std::ifstream in(kFile);
        if (in) {
            const auto j = nlohmann::json::parse(in);
            m_clientId = j.value("clientId", std::string());
            m_refresh = j.value("refresh", std::string());
            m_access = j.value("access", std::string());
            m_expires = j.value("expires", 0LL);
        }
    } catch (...) {
    }
    if (m_clientId.empty()) m_clientId = kBuiltInClientId;
}

void Spotify::save() {
    writeLater(kFile, nlohmann::json{{"clientId", m_clientId}, {"refresh", m_refresh}, {"access", m_access},
                                     {"expires", m_expires}}.dump(2));
}

bool Spotify::signedIn() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return !m_refresh.empty();
}

std::string Spotify::clientId() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_clientId;
}

void Spotify::setClientId(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (id == m_clientId) return;
    m_clientId = id;
    m_refresh.clear(); // a sign-in belongs to the app it was made with
    m_access.clear();
    m_expires = 0;
    save();
}

void Spotify::signOut() {
    std::lock_guard<std::mutex> lk(m_mu);
    m_refresh.clear();
    m_access.clear();
    m_expires = 0;
    m_now = Now{};
    save();
}

std::string Spotify::beginLogin(const std::string& consoleIp, std::string& error) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (consoleIp.empty()) {
        error = "Connect the Switch to Wi-Fi first.";
        return {};
    }
    if (m_server < 0 && !openServer()) {
        error = "The sign-in page couldn't start on this console.";
        return {};
    }
    // PKCE: Spotify gets the verifier's hash now and the verifier with the code.
    m_verifier = randomText(64);
    std::string ipPart = consoleIp;
    m_state = ipPart + "_" + std::to_string(kPort) + "_" + randomText(16); // the page on homenx.pages.dev reads where to go
    m_code.clear();
    m_loginError.clear();
    m_login = Login::Waiting;
    m_cv.notify_all();
    return "http://" + consoleIp + ":" + std::to_string(kPort) + "/login";
}

void Spotify::cancelLogin() {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_login == Login::Waiting) m_login = Login::Idle;
}

Spotify::Login Spotify::login() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_login;
}

std::string Spotify::loginError() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_loginError;
}

void Spotify::setWatching(bool on) {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (on && !m_watching) m_pollAt = 0; // at once
        m_watching = on;
    }
    m_cv.notify_all();
}

Spotify::Now Spotify::now() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_now;
}

void Spotify::command(Cmd c, int value) {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_cmds.emplace_back(c, value);
        // Shown at once (Spotify's answer comes with the next read).
        if (c == Cmd::PlayPause) m_now.playing = !m_now.playing;
        if (c == Cmd::Shuffle) m_now.shuffle = !m_now.shuffle;
        if (c == Cmd::Repeat) m_now.repeat = (m_now.repeat + 1) % 3;
        if (c == Cmd::Volume) m_now.volume = value;
    }
    m_cv.notify_all();
}

std::string Spotify::takeError() {
    std::lock_guard<std::mutex> lk(m_mu);
    std::string e;
    e.swap(m_error);
    return e;
}

bool Spotify::takeCover(std::vector<std::uint8_t>& rgba, int& w, int& h) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_coverNew) return false;
    m_coverNew = false;
    rgba.swap(m_cover);
    w = m_coverW;
    h = m_coverH;
    return true;
}

bool Spotify::openServer() {
    m_server = socket(AF_INET, SOCK_STREAM, 0);
    if (m_server < 0) return false;
    int one = 1;
    setsockopt(m_server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(m_server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || listen(m_server, 4) < 0) {
        close(m_server);
        m_server = -1;
        return false;
    }
    return true;
}

void Spotify::closeServer() {
    if (m_server >= 0) close(m_server);
    m_server = -1;
}

// One visit to the sign-in page: /login sends the phone to Spotify, /callback is
// Spotify's answer (through homenx.pages.dev/spotify/).
void Spotify::serveOnce() {
    pollfd p{m_server, POLLIN, 0};
    if (m_server < 0 || ::poll(&p, 1, 0) <= 0) return;
    const int fd = accept(m_server, nullptr, nullptr);
    if (fd < 0) return;
    std::string req;
    char buf[1024];
    for (int i = 0; i < 30 && req.find("\r\n") == std::string::npos && req.size() < 8192; ++i) {
        pollfd q{fd, POLLIN, 0};
        if (::poll(&q, 1, 100) <= 0) continue;
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, static_cast<size_t>(n));
    }
    const size_t sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? std::string::npos : req.find(' ', sp1 + 1);
    const std::string target = sp2 == std::string::npos ? std::string() : req.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    const std::string path = target.substr(0, q), query = q == std::string::npos ? std::string() : target.substr(q + 1);
    std::unique_lock<std::mutex> lk(m_mu);
    if (path == "/login" && m_login == Login::Waiting) {
        const std::string pasted = param(query, "id");
        if (pasted.size() == 32 && pasted.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
            m_clientId = pasted; // the phone gave it: kept for next time
            m_refresh.clear();
            m_access.clear();
            save();
        }
    }
    if (path == "/login" && m_login == Login::Waiting && m_clientId.empty()) {
        lk.unlock();
        page(fd, "One-time setup",
             "Spotify signs in through an app of yours (free, a minute):</p><ol style='text-align:left'>"
             "<li>Open <a style='color:#1ed760' href='https://developer.spotify.com/dashboard'>developer.spotify.com/dashboard</a>"
             " and log in.</li><li>Create app: any name, Redirect URI <b>https://homenx.pages.dev/spotify/</b>,"
             " tick Web API, save.</li><li>Copy its Client ID into this box:</li></ol>"
             "<form action='/login'><input name=id autocomplete=off placeholder='Client ID' "
             "style='width:100%;box-sizing:border-box;padding:12px;font-size:16px;border-radius:10px;border:0'>"
             "<button style='margin-top:14px;padding:12px 28px;font-size:16px;border:0;border-radius:24px;"
             "background:#1ed760;color:#08130c;font-weight:600'>Continue</button></form><p>");
    } else if (path == "/login" && m_login == Login::Waiting) {
        unsigned char digest[32];
#if MBEDTLS_VERSION_MAJOR >= 3
        mbedtls_sha256(reinterpret_cast<const unsigned char*>(m_verifier.data()), m_verifier.size(), digest, 0);
#else
        mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(m_verifier.data()), m_verifier.size(), digest, 0);
#endif
        const std::string url = "https://accounts.spotify.com/authorize?client_id=" + enc(m_clientId)
            + "&response_type=code&redirect_uri=" + enc(kRedirect) + "&code_challenge_method=S256&code_challenge="
            + base64url(digest, sizeof(digest)) + "&scope=" + enc(kScopes) + "&state=" + enc(m_state);
        lk.unlock();
        sendAll(fd, "HTTP/1.1 302 Found\r\nLocation: " + url + "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    } else if (path == "/callback" && m_login == Login::Waiting && param(query, "state") == m_state) {
        const std::string code = param(query, "code"), error = param(query, "error");
        if (!code.empty()) {
            m_code = code;
            m_login = Login::Exchanging;
            lk.unlock();
            page(fd, "Signed in", "Go back to your Switch: Spotify is in HOME's music player now.");
        } else {
            m_loginError = error == "access_denied" ? tr("You didn't allow HomeNX on Spotify's page.") : trf("Spotify said: {error}", {error});
            m_login = Login::Failed;
            lk.unlock();
            page(fd, "Not signed in", "Try again from your Switch.");
        }
        m_cv.notify_all();
    } else {
        lk.unlock();
        page(fd, "HomeNX", "Start the Spotify sign-in from your Switch's Settings first.");
    }
    close(fd);
}

void Spotify::exchange() {
    std::string clientId, code, verifier;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        clientId = m_clientId;
        code = m_code;
        verifier = m_verifier;
    }
    std::string out;
    const long status = request("POST", "https://accounts.spotify.com/api/token",
        "grant_type=authorization_code&code=" + enc(code) + "&redirect_uri=" + enc(kRedirect) + "&client_id=" + enc(clientId)
            + "&code_verifier=" + enc(verifier),
        {"Content-Type: application/x-www-form-urlencoded"}, out);
    std::lock_guard<std::mutex> lk(m_mu);
    try {
        const auto j = nlohmann::json::parse(out);
        if (status == 200 && j.contains("refresh_token")) {
            m_access = j.value("access_token", std::string());
            m_refresh = j.value("refresh_token", std::string());
            m_expires = nowSeconds() + j.value("expires_in", 3600) - 60;
            m_login = Login::Done;
            m_pollAt = 0;
            save();
            return;
        }
        m_loginError = "Spotify refused the sign-in: " + j.value("error_description", j.value("error", std::string("?")));
    } catch (...) {
        m_loginError = status ? trf("Spotify's answer couldn't be read (HTTP {code}).", {std::to_string(status)})
                              : "Spotify couldn't be reached.";
    }
    m_login = Login::Failed;
}

// A new access token from the refresh token (they last an hour).
bool Spotify::refresh() {
    std::string clientId, token;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        clientId = m_clientId;
        token = m_refresh;
    }
    if (token.empty()) return false;
    std::string out;
    const long status = request("POST", "https://accounts.spotify.com/api/token",
        "grant_type=refresh_token&refresh_token=" + enc(token) + "&client_id=" + enc(clientId),
        {"Content-Type: application/x-www-form-urlencoded"}, out);
    std::lock_guard<std::mutex> lk(m_mu);
    try {
        const auto j = nlohmann::json::parse(out);
        if (status == 200 && j.contains("access_token")) {
            m_access = j.value("access_token", std::string());
            if (j.contains("refresh_token")) m_refresh = j.value("refresh_token", m_refresh);
            m_expires = nowSeconds() + j.value("expires_in", 3600) - 60;
            save();
            return true;
        }
        if (status == 400 && j.value("error", std::string()) == "invalid_grant") { // revoked: sign in again
            m_refresh.clear();
            m_access.clear();
            save();
        }
    } catch (...) {
    }
    return false;
}

long Spotify::api(const char* method, const std::string& path, std::string& out) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string access;
        bool stale = false;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            access = m_access;
            stale = access.empty() || nowSeconds() >= m_expires;
        }
        if (stale && !refresh()) return 0;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            access = m_access;
        }
        const long status = request(method, "https://api.spotify.com/v1" + path, "",
                                    {"Authorization: Bearer " + access, "Content-Type:"}, out);
        if (status != 401) return status;
        std::lock_guard<std::mutex> lk(m_mu);
        m_expires = 0; // refreshed on the next try
    }
    return 401;
}

void Spotify::poll() {
    std::string out;
    const long status = api("GET", "/me/player?additional_types=episode", out);
    Now n;
    n.at = armGetSystemTick();
    if (status == 200) {
        try {
            const auto j = nlohmann::json::parse(out);
            n.active = true;
            n.playing = j.value("is_playing", false);
            n.shuffle = j.value("shuffle_state", false);
            const std::string repeat = j.value("repeat_state", std::string("off"));
            n.repeat = repeat == "track" ? 2 : repeat == "context" ? 1 : 0;
            n.progressMs = j.value("progress_ms", 0);
            if (j.contains("device") && j["device"].is_object()) {
                n.device = j["device"].value("name", std::string());
                if (j["device"].contains("volume_percent") && j["device"]["volume_percent"].is_number())
                    n.volume = j["device"]["volume_percent"].get<int>();
            }
            if (j.contains("item") && j["item"].is_object()) {
                const auto& item = j["item"];
                n.track = item.value("name", std::string());
                n.durationMs = item.value("duration_ms", 0);
                if (item.contains("artists") && item["artists"].is_array())
                    for (const auto& a : item["artists"])
                        n.artist += (n.artist.empty() ? "" : ", ") + a.value("name", std::string());
                const nlohmann::json* images = nullptr;
                if (item.contains("album") && item["album"].contains("images")) images = &item["album"]["images"];
                else if (item.contains("show") && item["show"].contains("images")) images = &item["show"]["images"]; // a podcast
                if (n.artist.empty() && item.contains("show")) n.artist = item["show"].value("name", std::string());
                if (images && images->is_array() && !images->empty()) { // the one nearest 300 px
                    for (const auto& im : *images)
                        if (n.coverUrl.empty() || im.value("width", 0) >= 250) n.coverUrl = im.value("url", n.coverUrl);
                }
            }
        } catch (...) {
            n.active = false;
        }
    }
    std::string coverUrl;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (status == 0 || status == 401) { // no answer: keep what was there
            m_now.at = n.at;
            return;
        }
        m_now = n;
        if (n.coverUrl != m_coverFor) coverUrl = m_coverFor = n.coverUrl;
    }
    if (coverUrl.empty()) return;
    std::string body;
    if (request("GET", coverUrl, "", {}, body) != 200) return;
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(body.data()), static_cast<int>(body.size()), &w, &h, &ch, 4);
    if (!px) return;
    std::lock_guard<std::mutex> lk(m_mu);
    m_cover.assign(px, px + static_cast<size_t>(w) * h * 4);
    m_coverW = w;
    m_coverH = h;
    m_coverNew = true;
    stbi_image_free(px);
}

void Spotify::doCommand(Cmd c, int value) {
    bool playing = false, shuffle = false;
    int repeat = 0;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        playing = m_now.playing; // (already flipped by command())
        shuffle = m_now.shuffle;
        repeat = m_now.repeat;
    }
    std::string out;
    long status = 0;
    switch (c) {
        case Cmd::PlayPause: status = api("PUT", playing ? "/me/player/play" : "/me/player/pause", out); break;
        case Cmd::Next: status = api("POST", "/me/player/next", out); break;
        case Cmd::Previous: status = api("POST", "/me/player/previous", out); break;
        case Cmd::Shuffle: status = api("PUT", std::string("/me/player/shuffle?state=") + (shuffle ? "true" : "false"), out); break;
        case Cmd::Repeat:
            status = api("PUT", std::string("/me/player/repeat?state=") + (repeat == 2 ? "track" : repeat == 1 ? "context" : "off"), out);
            break;
        case Cmd::Volume: status = api("PUT", "/me/player/volume?volume_percent=" + std::to_string(value), out); break;
    }
    std::lock_guard<std::mutex> lk(m_mu);
    if (status == 403) m_error = "Spotify Premium is needed for the buttons (Spotify's rule).";
    else if (status == 404) m_error = "Start Spotify on your phone or computer first.";
    else if (status == 0) m_error = "Spotify couldn't be reached.";
    m_pollAt = armGetSystemTick() + armNsToTicks(400'000'000ULL); // what changed, soon
}

void Spotify::run() {
    nxui::moveToWorkerCores(); // off the screen's core, below its priority
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
    for (;;) {
        Login login;
        bool watching = false, signedIn = false, due = false;
        std::pair<Cmd, int> cmd{Cmd::Next, -1};
        {
            std::unique_lock<std::mutex> lk(m_mu);
            // Quick while the sign-in page waits or the player shows; idle, every 2 s.
            m_cv.wait_for(lk, std::chrono::milliseconds(m_login == Login::Waiting ? 60 : m_watching ? 250 : 2000), [this]() {
                return m_quit || !m_cmds.empty() || m_login == Login::Exchanging || (m_watching && m_pollAt == 0);
            });
            if (m_quit) break;
            if (m_server >= 0 && m_login != Login::Waiting && m_login != Login::Exchanging) closeServer(); // done with the page
            login = m_login;
            watching = m_watching;
            signedIn = !m_refresh.empty();
            due = armGetSystemTick() >= m_pollAt;
            if (!m_cmds.empty()) {
                cmd = m_cmds.front();
                m_cmds.pop_front();
            }
        }
        if (login == Login::Waiting) serveOnce();
        if (login == Login::Exchanging) exchange();
        if (cmd.second != -1 && signedIn) doCommand(cmd.first, cmd.second);
        if (watching && signedIn && due) {
            {
                std::lock_guard<std::mutex> lk(m_mu);
                m_pollAt = armGetSystemTick() + armNsToTicks(3'000'000'000ULL);
            }
            poll();
        }
    }
}

} // namespace cards
#endif
