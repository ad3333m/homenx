#include "Catalog.hpp"
#include "homebrew/HomebrewStore.hpp"

#include <curl/curl.h>
#include <minizip/unzip.h>
#include <nlohmann/json.hpp>
#include <switch.h>
#include <switchu/file_log.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

namespace updater {
namespace {

constexpr const char* kUserAgent = "HomeNX-Updater";
constexpr const char* kTmpZip = "sdmc:/switch/.homenx-updater.zip";

// GitHub projects for well-known homebrew, by their NACP name squashed to
// lowercase letters and digits.
const std::map<std::string, std::string> kKnown = {
    {"sphaira", "ITotalJustice/sphaira"},
    {"jksv", "J-D-K/JKSV"},
    {"goldleaf", "XorTroll/Goldleaf"},
    {"nxshell", "joel16/NX-Shell"},
    {"moonlight", "XITRIX/Moonlight-Switch"},
    {"nxmp", "proconsule/nxmp"},
    {"linkalho", "rdmrocha/linkalho"},
    {"nxthemesinstaller", "exelix11/SwitchThemeInjector"},
    {"awooinstaller", "Huntereb/Awoo-Installer"},
    {"hekatetoolbox", "WerWolv/Hekate-Toolbox"},
    {"edizon", "WerWolv/EdiZon"},
    {"pplay", "Cpasjuste/pplay"},
    {"ftpd", "mtheall/ftpd"},
    {"nxdumptool", "DarkMatterCore/nxdumptool"},
    {"nxactivitylog", "tallbl0nde/NX-Activity-Log"},
    {"lightbrowser", "ad3333m/switch-lite-duck"},
};
// Public repos of this account are matched by name too.
constexpr const char* kOwnAccount = "ad3333m";

std::string squash(const std::string& s) {
    std::string out;
    for (unsigned char c : s)
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
    return out;
}

size_t toString(char* p, size_t size, size_t n, void* user) {
    try {
        static_cast<std::string*>(user)->append(p, size * n);
        return size * n;
    } catch (...) {
        return 0;
    }
}

size_t toFile(char* p, size_t size, size_t n, void* user) {
    return std::fwrite(p, size, n, static_cast<FILE*>(user));
}

struct Progress {
    std::atomic<float>* value;
};

int onProgress(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto* p = static_cast<Progress*>(user);
    if (p && p->value && total > 0) p->value->store(static_cast<float>(now) / static_cast<float>(total));
    return 0;
}

CURL* newRequest(const std::string& url) {
    CURL* c = curl_easy_init();
    if (!c) return nullptr;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    return c;
}

// The body of a GET, or "" (status in `status`).
std::string getText(const std::string& url, long& status) {
    std::string body;
    status = 0;
    CURL* c = newRequest(url);
    if (!c) return body;
    curl_slist* headers = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, toString);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    const CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        switchu::FileLog::log("[updater] GET %s: %s", url.c_str(), curl_easy_strerror(rc));
        body.clear();
    }
    return body;
}

bool download(const std::string& url, const std::string& to, std::atomic<float>& progress, std::string& error) {
    FILE* f = std::fopen(to.c_str(), "wb");
    if (!f) {
        error = "Can't write " + to;
        return false;
    }
    CURL* c = newRequest(url);
    if (!c) {
        std::fclose(f);
        error = "curl didn't start";
        return false;
    }
    Progress p{&progress};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, toFile);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &p);
    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    std::fclose(f);
    if (rc != CURLE_OK || status < 200 || status >= 300) {
        error = rc != CURLE_OK ? curl_easy_strerror(rc) : "HTTP " + std::to_string(status);
        std::remove(to.c_str());
        return false;
    }
    return true;
}

// The .nro in a zip: `entry` exactly if given, else one with our file name,
// else the only .nro. Written to `to`.
bool extractNro(const std::string& zipPath, const std::string& entry, const std::string& wantName,
                const std::string& to, std::string& error) {
    unzFile z = unzOpen64(zipPath.c_str());
    if (!z) {
        error = "The download isn't a zip";
        return false;
    }
    std::string chosen, anyNro;
    int nros = 0;
    char name[512];
    for (int rc = unzGoToFirstFile(z); rc == UNZ_OK; rc = unzGoToNextFile(z)) {
        unz_file_info64 info{};
        if (unzGetCurrentFileInfo64(z, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK) continue;
        std::string n = name, lower = n;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (!entry.empty() && n == entry) chosen = n;
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".nro") == 0) {
            ++nros;
            anyNro = n;
            const size_t slash = lower.find_last_of('/');
            if (chosen.empty() && lower.substr(slash == std::string::npos ? 0 : slash + 1) == wantName) chosen = n;
        }
    }
    if (chosen.empty() && nros == 1) chosen = anyNro;
    if (chosen.empty() || unzLocateFile(z, chosen.c_str(), 1) != UNZ_OK || unzOpenCurrentFile(z) != UNZ_OK) {
        unzClose(z);
        error = "No .nro in the download";
        return false;
    }
    FILE* f = std::fopen(to.c_str(), "wb");
    bool ok = f != nullptr;
    std::vector<char> buf(64 * 1024);
    for (int n; ok && (n = unzReadCurrentFile(z, buf.data(), (unsigned)buf.size())) > 0;)
        ok = std::fwrite(buf.data(), 1, (size_t)n, f) == (size_t)n;
    if (f) std::fclose(f);
    unzCloseCurrentFile(z);
    unzClose(z);
    if (!ok) {
        error = "Couldn't unpack the .nro";
        std::remove(to.c_str());
    }
    return ok;
}

std::string lowerFileName(const std::string& path) {
    std::string n = path.substr(path.find_last_of('/') + 1);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
    return n;
}

// The newest GitHub release of `repo`: its .nro (or a zip with one).
bool fromGitHub(App& app, const std::string& repo) {
    long status = 0;
    const std::string body = getText("https://api.github.com/repos/" + repo + "/releases/latest", status);
    if (status != 200 || body.empty()) return false;
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    std::string nro, nroNamed, zip;
    const std::string want = lowerFileName(app.path);
    for (const auto& a : j.value("assets", nlohmann::json::array())) {
        std::string name = a.value("name", std::string());
        const std::string url = a.value("browser_download_url", std::string());
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".nro") == 0) {
            if (nro.empty()) nro = url;
            if (name == want) nroNamed = url;
        } else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0 && zip.empty()) {
            zip = url;
        }
    }
    std::string tag = j.value("tag_name", std::string());
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    if (!nroNamed.empty() || !nro.empty()) {
        app.downloadUrl = !nroNamed.empty() ? nroNamed : nro;
        app.zip = false;
    } else if (!zip.empty()) {
        app.downloadUrl = zip;
        app.zip = true;
    } else {
        return false;
    }
    app.latest = tag;
    app.source = "GitHub " + repo;
    return true;
}

} // namespace

std::vector<App> scanApps(const std::string& self) {
    std::vector<App> apps;
    for (HomebrewApp& h : scanHomebrew(self, true)) {
        App a;
        a.path = h.path;
        a.name = h.name;
        a.author = h.author;
        a.version = h.version;
        a.icon = std::move(h.icon);
        apps.push_back(std::move(a));
    }
    return apps;
}

void findUpdates(std::vector<App>& apps, std::atomic<int>& done) {
    // This account's public repos, by squashed name.
    std::map<std::string, std::string> own;
    long status = 0;
    const std::string repos = getText(std::string("https://api.github.com/users/") + kOwnAccount + "/repos?per_page=100",
                                      status);
    const auto rj = nlohmann::json::parse(repos, nullptr, false);
    if (!rj.is_discarded() && rj.is_array())
        for (const auto& r : rj)
            if (!r.value("private", false)) own[squash(r.value("name", std::string()))] = r.value("full_name", std::string());

    // The HB App Store: by .nro path, then by title.
    std::map<std::string, nlohmann::json> byPath, byTitle;
    const std::string store = getText("https://switch.cdn.fortheusers.org/repo.json", status);
    {
        auto sj = nlohmann::json::parse(store, nullptr, false);
        if (!sj.is_discarded() && sj.is_object())
            for (auto& p : sj.value("packages", nlohmann::json::array())) {
                std::string bin = p.value("binary", std::string());
                std::transform(bin.begin(), bin.end(), bin.begin(), [](unsigned char c) { return std::tolower(c); });
                if (!bin.empty()) byPath[bin] = p;
                byTitle[squash(p.value("title", std::string()))] = p;
            }
    }

    for (App& app : apps) {
        const std::string key = squash(app.name);
        std::string repo;
        if (auto it = kKnown.find(key); it != kKnown.end()) repo = it->second;
        else
            for (const std::string& k : {key, key + "nx", key + "switch", "switch" + key})
                if (auto o = own.find(k); o != own.end()) {
                    repo = o->second;
                    break;
                }
        bool found = !repo.empty() && fromGitHub(app, repo);
        if (!found) {
            std::string rel = app.path.substr(std::strlen("sdmc:"));
            std::transform(rel.begin(), rel.end(), rel.begin(), [](unsigned char c) { return std::tolower(c); });
            const nlohmann::json* p = nullptr;
            if (auto it = byPath.find(rel); it != byPath.end()) p = &it->second;
            else if (auto t = byTitle.find(key); t != byTitle.end()) p = &t->second;
            if (p) {
                app.latest = p->value("version", std::string());
                app.downloadUrl = "https://switch.cdn.fortheusers.org/zips/" + p->value("name", std::string()) + ".zip";
                app.zip = true;
                std::string bin = p->value("binary", std::string());
                if (!bin.empty() && bin[0] == '/') bin.erase(0, 1);
                app.zipEntry = bin;
                app.source = "HB App Store";
            }
        }
        ++done;
    }
}

bool isNewer(const std::string& latest, const std::string& installed) {
    if (latest.empty()) return false;
    auto parts = [](const std::string& v) {
        std::vector<long> out;
        long cur = -1;
        for (char c : v) {
            if (std::isdigit((unsigned char)c)) cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
            else if (cur >= 0) {
                out.push_back(cur);
                cur = -1;
            }
        }
        if (cur >= 0) out.push_back(cur);
        return out;
    };
    const auto a = parts(latest), b = parts(installed);
    if (a.empty() || b.empty()) return latest != installed;
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const long x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

bool installUpdate(const App& app, std::atomic<float>& progress, std::string& error) {
    if (app.downloadUrl.empty()) {
        error = "No download";
        return false;
    }
    const std::string fresh = app.path + ".new", old = app.path + ".old";
    if (app.zip) {
        if (!download(app.downloadUrl, kTmpZip, progress, error)) return false;
        const bool ok = extractNro(kTmpZip, app.zipEntry, lowerFileName(app.path), fresh, error);
        std::remove(kTmpZip);
        if (!ok) return false;
    } else if (!download(app.downloadUrl, fresh, progress, error)) {
        return false;
    }
    // The old one aside, the new one in, then the old one gone.
    std::remove(old.c_str());
    if (std::rename(app.path.c_str(), old.c_str()) != 0) {
        std::remove(fresh.c_str());
        error = "Couldn't replace the old .nro";
        return false;
    }
    if (std::rename(fresh.c_str(), app.path.c_str()) != 0) {
        std::rename(old.c_str(), app.path.c_str());
        error = "Couldn't put the new .nro in place";
        return false;
    }
    std::remove(old.c_str());
    switchu::FileLog::log("[updater] %s updated to %s", app.path.c_str(), app.latest.c_str());
    return true;
}

bool startNetwork() {
    if (R_FAILED(socketInitializeDefault())) return false;
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void stopNetwork() {
    curl_global_cleanup();
    socketExit();
}

} // namespace updater
