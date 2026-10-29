#include "ReleaseUpdater.hpp"

#include <curl/curl.h>
#include <minizip/unzip.h>
#include <nlohmann/json.hpp>
#include <switch.h>
#include <switchu/file_log.hpp>
#ifdef HOMENX_UPDATER_SHARED_NETWORK
#include "themeshop/ThemeHttp.hpp"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace switchu::manager {
namespace {

// HomeNX: CI republishes one rolling release tagged "latest" and titles it
// "HomeNX build <N>" once the frontend zip is uploaded; N is the CI run number.
constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/PowerCmptr/HomeNX/releases/tags/latest";
constexpr const char* kWorkRoot = "sdmc:/config/SwitchU/update";
constexpr const char* kArchivePath = "sdmc:/config/SwitchU/update/SwitchU-update.zip";
constexpr const char* kArchivePartPath = "sdmc:/config/SwitchU/update/SwitchU-update.zip.part";
constexpr const char* kStagingRoot = "sdmc:/config/SwitchU/update/staging";
constexpr std::uint64_t kMaxArchiveBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxExtractedBytes = 512ULL * 1024ULL * 1024ULL;

std::mutex g_networkMutex;
bool g_nifmInitialized = false;
bool g_socketInitialized = false;
bool g_curlInitialized = false;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool endsWith(const std::string& value, std::string_view suffix) {
    return value.size() >= suffix.size()
        && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<int> versionParts(std::string version) {
    while (!version.empty() && (version.front() == 'v' || version.front() == 'V'))
        version.erase(version.begin());
    std::vector<int> parts;
    std::size_t offset = 0;
    while (offset < version.size()) {
        while (offset < version.size() && !std::isdigit(static_cast<unsigned char>(version[offset])))
            ++offset;
        if (offset >= version.size()) break;
        int value = 0;
        while (offset < version.size()
               && std::isdigit(static_cast<unsigned char>(version[offset]))) {
            value = std::min(1'000'000, value * 10 + (version[offset] - '0'));
            ++offset;
        }
        parts.push_back(value);
    }
    return parts;
}

bool isNewerVersion(const std::string& candidate, const std::string& current) {
    auto a = versionParts(candidate);
    auto b = versionParts(current);
    const std::size_t count = std::max(a.size(), b.size());
    a.resize(count, 0);
    b.resize(count, 0);
    return std::lexicographical_compare(b.begin(), b.end(), a.begin(), a.end());
}

#ifdef HOMENX_UPDATER_SHARED_NETWORK
// Inside the HomeNX menu: reuse the Theme Shop's network runtime instead of
// initialising (and later tearing down) sockets a second time.
void shutdownNetworkLocked() {}

void ensureNetworkLocked() {
    if (!themeshop::http::initialize())
        throw std::runtime_error("The internet connection isn't available");
}
#else
void shutdownNetworkLocked() {
    if (g_curlInitialized) {
        curl_global_cleanup();
        g_curlInitialized = false;
    }
    if (g_socketInitialized) {
        socketExit();
        g_socketInitialized = false;
    }
    if (g_nifmInitialized) {
        nifmExit();
        g_nifmInitialized = false;
    }
}

void ensureNetworkLocked() {
    if (g_nifmInitialized && g_socketInitialized && g_curlInitialized)
        return;
    Result rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc))
        throw std::runtime_error("nifmInitialize failed: 0x" + std::to_string(rc));
    g_nifmInitialized = true;
    rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        shutdownNetworkLocked();
        throw std::runtime_error("socketInitializeDefault failed: 0x" + std::to_string(rc));
    }
    g_socketInitialized = true;
    const CURLcode curlRc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curlRc != CURLE_OK) {
        shutdownNetworkLocked();
        throw std::runtime_error(std::string("curl_global_init failed: ")
                                 + curl_easy_strerror(curlRc));
    }
    g_curlInitialized = true;
}
#endif

void configureCurl(CURL* curl, const std::string& url) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L); // slow Switch Wi-Fi / DNS
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NXToggle/" HOMENX_BUILD);
}

size_t appendResponse(char* data, size_t size, size_t count, void* user) {
    const size_t bytes = size * count;
    static_cast<std::string*>(user)->append(data, bytes);
    return bytes;
}

std::string getTextOnceLocked(const std::string& url);

// HomeNX: the release check retries too (a slow or flaky connection used to fail
// it on the first timeout).
std::string getTextLocked(const std::string& url) {
    for (int attempt = 1;; ++attempt) {
        try {
            return getTextOnceLocked(url);
        } catch (const std::exception& ex) {
            if (attempt >= 3) throw;
            switchu::FileLog::log("[updater] check attempt %d failed: %s", attempt, ex.what());
            svcSleepThread(static_cast<s64>(attempt) * 2'000'000'000LL);
        }
    }
}

std::string getTextOnceLocked(const std::string& url) {
    ensureNetworkLocked();
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");
    std::string response;
    configureCurl(curl, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("GitHub request failed: ") + curl_easy_strerror(rc));
    if (status < 200 || status >= 300)
        throw std::runtime_error("GitHub returned HTTP " + std::to_string(status));
    return response;
}

struct DownloadContext {
    std::FILE* file = nullptr;
    Sha256Context sha{};
    std::atomic<float>* progress = nullptr;
    std::uint64_t received = 0;
    std::uint64_t total = 0;       // whole asset, so progress spans resumed attempts
    std::uint64_t resumeFrom = 0;  // bytes this attempt asked the server to skip
    CURL* curl = nullptr;
    bool checkedResume = false;
};

size_t writeDownload(char* data, size_t size, size_t count, void* user) {
    const size_t bytes = size * count;
    auto* context = static_cast<DownloadContext*>(user);
    if (!context->checkedResume) {
        // HomeNX: a resumed attempt only continues if the server honoured the
        // range (206). Otherwise the body starts at byte 0: start the file over.
        context->checkedResume = true;
        long code = 0;
        curl_easy_getinfo(context->curl, CURLINFO_RESPONSE_CODE, &code);
        if (context->resumeFrom > 0 && code != 206) {
            context->file = std::freopen(kArchivePartPath, "wb", context->file);
            sha256ContextCreate(&context->sha);
            context->received = 0;
            context->resumeFrom = 0;
        }
    }
    if (context->received + bytes > kMaxArchiveBytes)
        return 0;
    if (!context->file || std::fwrite(data, 1, bytes, context->file) != bytes)
        return 0;
    sha256ContextUpdate(&context->sha, data, bytes);
    context->received += bytes;
    return bytes;
}

int updateDownloadProgress(void* user, curl_off_t total, curl_off_t current,
                           curl_off_t, curl_off_t) {
    auto* context = static_cast<DownloadContext*>(user);
    const double whole = context->total > 0
        ? static_cast<double>(context->total)
        : static_cast<double>(context->resumeFrom) + static_cast<double>(total);
    if (whole > 0)
        context->progress->store(std::clamp(
            static_cast<float>((static_cast<double>(context->resumeFrom) + current) / whole), 0.f, 1.f));
    return 0;
}

std::string hexDigest(const std::array<std::uint8_t, SHA256_HASH_SIZE>& digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (std::uint8_t byte : digest) {
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0f]);
    }
    return out;
}

// HomeNX: what a partial download in kArchivePartPath belongs to ("<size> <sha256>"),
// so a later run can continue it instead of starting over.
constexpr const char* kPartInfoPath = "sdmc:/config/SwitchU/update/SwitchU-update.zip.part.info";

std::string partKey(const ReleaseInfo& release) {
    return std::to_string(release.downloadSize) + " " + lower(release.sha256);
}

// Resumes a partial file from an earlier run of the same release: hashes what's
// already there so the final SHA-256 check still covers the whole archive.
std::uint64_t adoptPartialDownload(const ReleaseInfo& release, Sha256Context& sha) {
    std::error_code ec;
    std::string key;
    {
        std::ifstream info(kPartInfoPath);
        std::getline(info, key);
    }
    const std::uint64_t have = std::filesystem::exists(kArchivePartPath, ec)
        ? std::filesystem::file_size(kArchivePartPath, ec) : 0;
    if (release.downloadSize == 0 || release.sha256.empty() || key != partKey(release)
        || have == 0 || have >= release.downloadSize || ec) {
        std::filesystem::remove(kArchivePartPath, ec);
        std::ofstream(kPartInfoPath, std::ios::trunc) << partKey(release) << '\n';
        return 0;
    }
    std::FILE* in = std::fopen(kArchivePartPath, "rb");
    if (!in) return 0;
    std::vector<char> buffer(64 * 1024);
    std::uint64_t hashed = 0;
    for (size_t n; (n = std::fread(buffer.data(), 1, buffer.size(), in)) > 0;) {
        sha256ContextUpdate(&sha, buffer.data(), n);
        hashed += n;
    }
    std::fclose(in);
    switchu::FileLog::log("[updater] continuing an earlier download at %lu of %lu bytes",
                          static_cast<unsigned long>(hashed),
                          static_cast<unsigned long>(release.downloadSize));
    return hashed;
}

std::string downloadLocked(const ReleaseInfo& release, std::atomic<float>& progress) {
    std::filesystem::create_directories(kWorkRoot);
    std::error_code ec;

    // HomeNX: never give up while data is still arriving. A cut-off download (a
    // stall, a dropped connection, a timeout, the app closed) continues where it
    // stopped with an HTTP range, also across runs (see adoptPartialDownload).
    // It only stops after 5 attempts in a row that receive nothing.
    DownloadContext context;
    context.progress = &progress;
    context.total = release.downloadSize;
    sha256ContextCreate(&context.sha);
    context.received = adoptPartialDownload(release, context.sha);
    CURLcode rc = CURLE_OK;
    long status = 0;
    int idleAttempts = 0;
    for (int attempt = 1; idleAttempts < 5; ++attempt) {
        const std::uint64_t before = context.received;
        const bool resume = context.received > 0
            && (release.downloadSize == 0 || context.received < release.downloadSize);
        if (!resume) {
            context.received = 0;
            sha256ContextCreate(&context.sha);
        }
        context.file = std::fopen(kArchivePartPath, resume ? "ab" : "wb");
        if (!context.file)
            throw std::runtime_error("Unable to create the update archive on the SD card");
        context.resumeFrom = resume ? context.received : 0;
        context.checkedResume = false;
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::fclose(context.file);
            throw std::runtime_error("curl_easy_init failed");
        }
        context.curl = curl;
        configureCurl(curl, release.downloadUrl);
        // No overall time limit; only a full minute with no data counts as a stall.
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        if (resume)
            curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(context.received));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeDownload);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, updateDownloadProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
        rc = curl_easy_perform(curl);
        status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
        context.curl = nullptr;
        std::fclose(context.file);
        context.file = nullptr;
        const bool complete = rc == CURLE_OK && status >= 200 && status < 300
            && (release.downloadSize == 0 || context.received == release.downloadSize);
        if (complete) break;
        idleAttempts = context.received > before ? 0 : idleAttempts + 1;
        switchu::FileLog::log("[updater] download attempt %d stopped rc=%d status=%ld got=%lu/%lu",
                              attempt, static_cast<int>(rc), status,
                              static_cast<unsigned long>(context.received),
                              static_cast<unsigned long>(release.downloadSize));
        if (status == 416)
            context.received = 0; // range no longer valid: start over
        else if (status >= 400 && status != 429 && status < 500)
            break; // e.g. 404: retrying won't help
        svcSleepThread(static_cast<s64>(std::min(idleAttempts + 1, 5)) * 1'000'000'000LL);
    }
    if (rc != CURLE_OK || status < 200 || status >= 300) {
        // Keep the partial file: the next "Check for updates" continues it.
        if (rc != CURLE_OK)
            throw std::runtime_error(std::string("Update download stopped: ")
                                     + curl_easy_strerror(rc)
                                     + ". Try again: it continues where it stopped.");
        throw std::runtime_error("Update download returned HTTP " + std::to_string(status));
    }
    if (context.received == 0 || context.received > kMaxArchiveBytes) {
        std::filesystem::remove(kArchivePartPath, ec);
        throw std::runtime_error("Downloaded archive size is invalid");
    }
    if (release.downloadSize != 0 && context.received != release.downloadSize) {
        throw std::runtime_error("The download isn't complete yet. Try again: it continues where it stopped.");
    }
    std::array<std::uint8_t, SHA256_HASH_SIZE> digest{};
    sha256ContextGetHash(&context.sha, digest.data());
    const std::string actualDigest = hexDigest(digest);
    if (!release.sha256.empty() && lower(release.sha256) != actualDigest) {
        std::filesystem::remove(kArchivePartPath, ec);
        std::filesystem::remove(kPartInfoPath, ec);
        throw std::runtime_error("Downloaded archive SHA-256 does not match GitHub");
    }
    std::filesystem::remove(kPartInfoPath, ec);
    std::filesystem::remove(kArchivePath, ec);
    std::filesystem::rename(kArchivePartPath, kArchivePath, ec);
    if (ec)
        throw std::runtime_error("Unable to finalize the downloaded archive");
    progress.store(1.f);
    return actualDigest;
}

std::string safeRelativePath(std::string raw) {
    std::replace(raw.begin(), raw.end(), '\\', '/');
    while (raw.rfind("./", 0) == 0) raw.erase(0, 2);
    if (raw.rfind("sd_out/", 0) == 0) raw.erase(0, 7);
    if (raw.empty() || raw.front() == '/' || raw.find(':') != std::string::npos)
        return {};
    std::filesystem::path path(raw);
    for (const auto& part : path) {
        if (part == ".." || part == ".") return {};
    }
    const bool daemon = raw.rfind("atmosphere/contents/0100000000001000/", 0) == 0;
    const bool menu = raw.rfind("switch/SwitchU/", 0) == 0;
    const bool manager = raw.rfind("switch/SwitchU-Manager/", 0) == 0
        || raw.rfind("switch/HomeNX-Toggle/", 0) == 0
        || raw.rfind("switch/NXToggle/", 0) == 0
        || raw.rfind("switch/HomeNX-Loader/", 0) == 0;
    return daemon || menu || manager ? raw : std::string();
}

std::vector<std::string> extractArchive(std::atomic<float>& progress,
                                        float progressBase,
                                        float progressSpan) {
    std::error_code ec;
    std::filesystem::remove_all(kStagingRoot, ec);
    std::filesystem::create_directories(kStagingRoot, ec);
    if (ec) throw std::runtime_error("Unable to create update staging directory");

    unzFile archive = unzOpen64(kArchivePath);
    if (!archive) throw std::runtime_error("Downloaded file is not a readable ZIP archive");
    unz_global_info64 globalInfo{};
    const bool hasEntryCount = unzGetGlobalInfo64(archive, &globalInfo) == UNZ_OK
        && globalInfo.number_entry > 0;
    std::vector<std::string> files;
    std::uint64_t extractedBytes = 0;
    bool hasDaemon = false;
    bool hasMenuMain = false;
    bool hasMenuNpdm = false;
    std::uint64_t processedEntries = 0;
    int rc = unzGoToFirstFile(archive);
    while (rc == UNZ_OK) {
        unz_file_info64 info{};
        std::array<char, 1024> name{};
        if (unzGetCurrentFileInfo64(archive, &info, name.data(), name.size(),
                                    nullptr, 0, nullptr, 0) != UNZ_OK) {
            unzClose(archive);
            throw std::runtime_error("Unable to inspect ZIP entry");
        }
        const std::string raw(name.data());
        const std::string relative = safeRelativePath(raw);
        const bool directory = !raw.empty() && (raw.back() == '/' || raw.back() == '\\');
        if (!relative.empty() && !directory) {
            extractedBytes += info.uncompressed_size;
            if (extractedBytes > kMaxExtractedBytes) {
                unzClose(archive);
                throw std::runtime_error("Update archive expands beyond the safety limit");
            }
            const std::filesystem::path destination =
                std::filesystem::path(kStagingRoot) / relative;
            std::filesystem::create_directories(destination.parent_path(), ec);
            if (ec || unzOpenCurrentFile(archive) != UNZ_OK) {
                unzClose(archive);
                throw std::runtime_error("Unable to extract ZIP entry");
            }
            std::ofstream output(destination, std::ios::binary | std::ios::trunc);
            std::array<char, 64 * 1024> buffer{};
            int read = 0;
            while (output && (read = unzReadCurrentFile(archive, buffer.data(), buffer.size())) > 0)
                output.write(buffer.data(), read);
            const int closeRc = unzCloseCurrentFile(archive);
            if (!output || read < 0 || closeRc != UNZ_OK) {
                unzClose(archive);
                throw std::runtime_error("ZIP entry extraction failed");
            }
            files.push_back(relative);
            hasDaemon |= relative == "atmosphere/contents/0100000000001000/exefs.nsp";
            hasMenuMain |= relative == "switch/SwitchU/bin/menu/main";
            hasMenuNpdm |= relative == "switch/SwitchU/bin/menu/main.npdm";
        }
        rc = unzGoToNextFile(archive);
        ++processedEntries;
        if (hasEntryCount) {
            const float fraction = std::clamp(
                static_cast<float>(processedEntries)
                    / static_cast<float>(globalInfo.number_entry),
                0.f, 1.f);
            progress.store(progressBase + progressSpan * fraction);
        }
    }
    unzClose(archive);
    if (rc != UNZ_END_OF_LIST_OF_FILE)
        throw std::runtime_error("ZIP directory is corrupted");
    if (!hasDaemon || !hasMenuMain || !hasMenuNpdm)
        throw std::runtime_error("Release archive does not contain a complete SwitchU installation");
    progress.store(progressBase + progressSpan);
    return files;
}

struct Replacement {
    std::filesystem::path source;
    std::filesystem::path destination;
    std::filesystem::path temporary;
    std::filesystem::path backup;
    bool hadOriginal = false;
    bool applied = false;
};

void rollback(std::vector<Replacement>& replacements) {
    std::error_code ec;
    for (auto it = replacements.rbegin(); it != replacements.rend(); ++it) {
        if (it->applied) {
            std::filesystem::remove(it->destination, ec);
            ec.clear();
            if (it->hadOriginal)
                std::filesystem::rename(it->backup, it->destination, ec);
        }
        ec.clear();
        std::filesystem::remove(it->temporary, ec);
    }
    fsdevCommitDevice("sdmc");
}

// HomeNX: files that can't be swapped right now (the running Toggle's own
// .nro, a font the menu is reading) are parked next to the original as
// <file>.homenx-pending and listed here; ReleaseUpdater::applyPending() swaps
// them in when HomeNX next starts, before it opens anything.
constexpr const char* kPendingList = "sdmc:/config/SwitchU/update/pending.txt";
constexpr const char* kPendingSuffix = ".homenx-pending";

bool sameContents(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(b, ec) ||
        std::filesystem::file_size(a, ec) != std::filesystem::file_size(b, ec) || ec)
        return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    std::vector<char> ba(1 << 16), bb(1 << 16);
    while (fa && fb) {
        fa.read(ba.data(), ba.size());
        fb.read(bb.data(), bb.size());
        if (fa.gcount() != fb.gcount() ||
            std::memcmp(ba.data(), bb.data(), static_cast<std::size_t>(fa.gcount())) != 0)
            return false;
    }
    return fa.eof() && fb.eof();
}

void addPending(const std::filesystem::path& destination) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(kPendingList).parent_path(), ec);
    std::ofstream(kPendingList, std::ios::app) << destination.string() << '\n';
}

void copyFileContents(const std::filesystem::path& source,
                      const std::filesystem::path& destination) {
    std::FILE* input = std::fopen(source.string().c_str(), "rb");
    if (!input)
        throw std::runtime_error("Unable to open staged update file: "
                                 + std::string(std::strerror(errno)));

    std::FILE* output = std::fopen(destination.string().c_str(), "wb");
    if (!output) {
        const int savedErrno = errno;
        std::fclose(input);
        throw std::runtime_error("Unable to create update destination: "
                                 + std::string(std::strerror(savedErrno)));
    }

    std::array<unsigned char, 64 * 1024> buffer{};
    bool failed = false;
    while (!failed) {
        const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), input);
        if (count > 0 && std::fwrite(buffer.data(), 1, count, output) != count)
            failed = true;
        if (count < buffer.size()) {
            if (std::ferror(input))
                failed = true;
            break;
        }
    }
    if (std::fclose(input) != 0)
        failed = true;
    if (std::fclose(output) != 0)
        failed = true;

    if (failed) {
        std::error_code removeEc;
        std::filesystem::remove(destination, removeEc);
        throw std::runtime_error("Unable to copy an update file to the SD card");
    }
}

void installFiles(const std::vector<std::string>& files,
                  bool preserveDisabledOverride,
                  std::atomic<float>& progress,
                  float progressBase,
                  float progressSpan) {
    std::vector<Replacement> replacements;
    replacements.reserve(files.size());
    const std::string daemonRelative = "atmosphere/contents/0100000000001000/exefs.nsp";
    for (const std::string& relative : files) {
        Replacement item;
        item.source = std::filesystem::path(kStagingRoot) / relative;
        std::string destinationRelative = relative;
        if (preserveDisabledOverride && relative == daemonRelative)
            destinationRelative += ".disabled";
        item.destination = std::filesystem::path("sdmc:/") / destinationRelative;
        // Unchanged files (fonts, sounds, most assets) are left alone: some are
        // open while the menu runs, and rewriting them gains nothing.
        if (sameContents(item.source, item.destination))
            continue;
        item.temporary = item.destination.string() + ".switchu-update-new";
        item.backup = item.destination.string() + ".switchu-update-old";
        replacements.push_back(std::move(item));
    }

    std::error_code ec;
    for (std::size_t i = 0; i < replacements.size(); ++i) {
        auto& item = replacements[i];
        std::filesystem::create_directories(item.destination.parent_path(), ec);
        std::filesystem::remove(item.temporary, ec);
        ec.clear();
        try {
            copyFileContents(item.source, item.temporary);
        } catch (const std::exception& ex) {
            rollback(replacements);
            throw std::runtime_error("Unable to stage " + item.destination.string()
                                     + ": " + ex.what());
        }
        const float fraction = static_cast<float>(i + 1)
            / std::max<std::size_t>(1, replacements.size());
        progress.store(progressBase + progressSpan * 0.46f * fraction);
    }

    for (std::size_t i = 0; i < replacements.size(); ++i) {
        auto& item = replacements[i];
        ec.clear();
        std::filesystem::remove(item.backup, ec);
        ec.clear();
        item.hadOriginal = std::filesystem::is_regular_file(item.destination, ec) && !ec;
        if (item.hadOriginal) {
            std::filesystem::rename(item.destination, item.backup, ec);
            if (ec) {
                // In use (e.g. the running Toggle's own .nro): park it as pending.
                ec.clear();
                const std::filesystem::path pending = item.destination.string() + kPendingSuffix;
                std::filesystem::remove(pending, ec);
                ec.clear();
                std::filesystem::rename(item.temporary, pending, ec);
                if (ec) {
                    rollback(replacements);
                    throw std::runtime_error("Unable to update " + item.destination.string());
                }
                addPending(item.destination);
                switchu::FileLog::log("[updater] %s is in use; pending", item.destination.string().c_str());
                continue;
            }
        }
        ec.clear();
        std::filesystem::rename(item.temporary, item.destination, ec);
        if (ec) {
            if (item.hadOriginal) {
                std::error_code restoreEc;
                std::filesystem::rename(item.backup, item.destination, restoreEc);
            }
            rollback(replacements);
            throw std::runtime_error("Unable to install " + item.destination.string());
        }
        item.applied = true;
        const float fraction = static_cast<float>(i + 1)
            / std::max<std::size_t>(1, replacements.size());
        progress.store(progressBase + progressSpan * (0.46f + 0.49f * fraction));
    }

    // The SD card isn't a journaled save; a failing "commit" doesn't undo the
    // renames, so it's no reason to roll the whole update back.
    const Result commitRc = fsdevCommitDevice("sdmc");
    if (R_FAILED(commitRc))
        switchu::FileLog::log("[updater] SD commit rc=0x%X (ignored)", commitRc);
    for (auto& item : replacements) {
        ec.clear();
        std::filesystem::remove(item.backup, ec);
    }
    fsdevCommitDevice("sdmc");
    progress.store(progressBase + progressSpan);
}

} // namespace

ReleaseInfo ReleaseUpdater::checkLatest() {
    std::lock_guard<std::mutex> lock(g_networkMutex);
    const std::string response = getTextLocked(kLatestReleaseUrl);
    const auto json = nlohmann::json::parse(response);
    if (!json.is_object() || json.value("draft", true) || json.value("prerelease", true))
        throw std::runtime_error("GitHub did not return a stable release");

    ReleaseInfo release;
    release.name = json.value("name", std::string());
    // "HomeNX build 57" -> "build 57"; versionParts() reads the digits.
    const std::size_t at = release.name.find("build ");
    release.version = at == std::string::npos ? std::string() : release.name.substr(at);
    int bestScore = -1;
    if (json.contains("assets") && json["assets"].is_array()) {
        for (const auto& asset : json["assets"]) {
            const std::string name = asset.value("name", "");
            const std::string normalized = lower(name);
            if (!endsWith(normalized, ".zip")) continue;
            int score = 0;
            if (normalized == "homenx-frontend.zip") score += 100;
            if (normalized.find("homenx") != std::string::npos) score += 20;
            if (normalized.find("source") != std::string::npos) score -= 50;
            if (score <= bestScore) continue;
            bestScore = score;
            release.downloadUrl = asset.value("browser_download_url", "");
            release.downloadSize = asset.value("size", 0ULL);
            std::string digest = asset.value("digest", "");
            if (digest.rfind("sha256:", 0) == 0) digest.erase(0, 7);
            release.sha256 = digest;
        }
    }
    if (release.version.empty() || release.downloadUrl.empty())
        throw std::runtime_error("The latest HomeNX release has no frontend ZIP yet");
    if (release.downloadSize > kMaxArchiveBytes)
        throw std::runtime_error("GitHub release archive exceeds the safety limit");
    release.updateAvailable = isNewerVersion(release.version, installedVersion());
    switchu::FileLog::log("[updater] current=%s latest=%s available=%d size=%lu",
                          installedVersion().c_str(), release.version.c_str(),
                          release.updateAvailable ? 1 : 0,
                          static_cast<unsigned long>(release.downloadSize));
    return release;
}

UpdateInstallResult ReleaseUpdater::install(const ReleaseInfo& release,
                                            bool preserveDisabledOverride,
                                            std::atomic<float>& downloadProgress,
                                            std::atomic<float>& installProgress,
                                            std::atomic<int>& stage) {
    UpdateInstallResult result;
    result.version = release.version;
    try {
        {
            std::lock_guard<std::mutex> lock(g_networkMutex);
            ensureNetworkLocked();
            stage.store(static_cast<int>(UpdateWorkerStage::Downloading));
            downloadProgress.store(0.f);
            installProgress.store(0.f);
            const std::string digest = downloadLocked(release, downloadProgress);
            switchu::FileLog::log("[updater] download complete sha256=%s", digest.c_str());
        }
        stage.store(static_cast<int>(UpdateWorkerStage::Verifying));
        downloadProgress.store(1.f);
        stage.store(static_cast<int>(UpdateWorkerStage::Extracting));
        installProgress.store(0.f);
        constexpr float kExtractionShare = 0.30f;
        const auto files = extractArchive(installProgress, 0.f, kExtractionShare);
        switchu::FileLog::log("[updater] archive validated files=%lu",
                              static_cast<unsigned long>(files.size()));
        stage.store(static_cast<int>(UpdateWorkerStage::Installing));
        installFiles(files, preserveDisabledOverride, installProgress,
                     kExtractionShare, 1.f - kExtractionShare);
        std::error_code ec;
        std::filesystem::remove_all(kStagingRoot, ec);
        std::filesystem::remove(kArchivePath, ec);
        fsdevCommitDevice("sdmc");
        result.success = true;
        switchu::FileLog::log("[updater] installed version=%s preserveDisabled=%d",
                              release.version.c_str(), preserveDisabledOverride ? 1 : 0);
    } catch (const std::exception& ex) {
        result.error = ex.what();
        switchu::FileLog::log("[updater] install failed: %s", ex.what());
    } catch (...) {
        result.error = "Unknown update error";
        switchu::FileLog::log("[updater] install failed: unknown error");
    }
    stage.store(static_cast<int>(UpdateWorkerStage::Idle));
    return result;
}

std::string ReleaseUpdater::installedVersion() {
    std::ifstream in("sdmc:/switch/SwitchU/homenx-build.txt");
    std::string line;
    if (in && std::getline(in, line) && line.rfind("build ", 0) == 0) return line;
    return "build 0";
}

void ReleaseUpdater::applyPending() {
    std::ifstream in(kPendingList);
    if (!in) return;
    std::vector<std::string> keep;
    std::error_code ec;
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        const std::filesystem::path destination(line);
        const std::filesystem::path pending = line + kPendingSuffix;
        if (!std::filesystem::exists(pending, ec)) continue;
        const std::filesystem::path old = line + ".homenx-old";
        std::filesystem::remove(old, ec);
        ec.clear();
        if (std::filesystem::exists(destination, ec))
            std::filesystem::rename(destination, old, ec);
        if (!ec) {
            std::filesystem::rename(pending, destination, ec);
            if (ec) std::filesystem::rename(old, destination, ec); // put it back
        }
        if (ec) {
            keep.push_back(line); // still in use (e.g. our own .nro); next start
            ec.clear();
        } else {
            std::filesystem::remove(old, ec);
            switchu::FileLog::log("[updater] applied pending %s", line.c_str());
        }
    }
    in.close();
    std::ofstream out(kPendingList, std::ios::trunc);
    for (const auto& line : keep) out << line << '\n';
}

void ReleaseUpdater::shutdownNetwork() {
    std::lock_guard<std::mutex> lock(g_networkMutex);
    shutdownNetworkLocked();
}

} // namespace switchu::manager
