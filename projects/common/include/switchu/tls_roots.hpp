#pragma once
#include <curl/curl.h>

#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>

// HomeNX's root certificates (CardsNX's certs/roots.pem, from Mozilla's list):
// Let's Encrypt (ISRG X1, X2: its new Root YR chain is cross-signed by X1) and
// Google Trust Services (Cloudflare's sites: nlib.cc, SteamGridDB, Discord).
// The console's own store is older and refused those chains, so covers and the
// Discord QR code never came. A request the console refuses is tried once more
// with these (still verified); the host then goes straight to them.
namespace switchu::tls {

// HOME's copy (the frontend installs CardsNX's files there).
inline constexpr const char* kHomeRoots = "sdmc:/switch/SwitchU/cards/certs/roots.pem";

inline bool certRefused(CURLcode rc) {
    return rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_SSL_ISSUER_ERROR
        || rc == CURLE_SSL_INVALIDCERTSTATUS || rc == CURLE_SSL_CACERT_BADFILE;
}

inline std::string hostOf(const std::string& url) {
    size_t at = url.find("://");
    at = at == std::string::npos ? 0 : at + 3;
    const size_t end = url.find_first_of(":/?#", at);
    return url.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

inline std::mutex& hostsMutex() {
    static std::mutex mu;
    return mu;
}
inline std::set<std::string>& rootsHosts() { // these needed the roots this session
    static std::set<std::string> hosts;
    return hosts;
}

inline bool usesRoots(const std::string& url) {
    std::lock_guard<std::mutex> lk(hostsMutex());
    return rootsHosts().count(hostOf(url)) != 0;
}

inline bool exists(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

// After a refusal: true when it's worth one more try with `roots` (it's there,
// and this host hasn't had them yet). Also notes it for next time.
inline bool retryWithRoots(const std::string& url, CURLcode rc, const char* roots) {
    if (!certRefused(rc) || !exists(roots)) return false;
    std::lock_guard<std::mutex> lk(hostsMutex());
    return rootsHosts().insert(hostOf(url)).second;
}

// The first failure per host, kept for the next bug report (a few lines at most).
inline void noteFailure(const std::string& url, CURLcode rc, const char* logPath) {
    static std::mutex mu;
    static std::set<std::string> noted;
    const std::string key = hostOf(url) + "#" + std::to_string(static_cast<int>(rc));
    std::lock_guard<std::mutex> lk(mu);
    if (!noted.insert(key).second) return;
    if (std::FILE* f = std::fopen(logPath, "a")) {
        std::fprintf(f, "%s: curl error %d (%s)\n", hostOf(url).c_str(), static_cast<int>(rc), curl_easy_strerror(rc));
        std::fclose(f);
    }
}

} // namespace switchu::tls
