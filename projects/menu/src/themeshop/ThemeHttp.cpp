#include "ThemeHttp.hpp"

#include "core/DebugLog.hpp"

#include <atomic>
#include <curl/curl.h>
#include <curlpp/Easy.hpp>
#include <curlpp/Infos.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/cURLpp.hpp>
#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <stdexcept>

namespace {

std::mutex g_themeHttpMutex;
bool g_nifmInitialized = false;
bool g_socketInitialized = false;
bool g_curlInitialized = false;
// HomeNX: set once the runtime is up, read without the lock: transfers hold the
// lock for their whole length, and HOME's main thread asking "ready?" while
// one ran (the weather, a theme) froze until it finished.
std::atomic<bool> g_runtimeReady{false};

constexpr int kRequestAttemptCount = 2;

// HomeNX: the body is written straight into the buffer that is returned. It
// went through an ostringstream, a string and a vector (and a string again for
// getText): a 5-8 MB theme file or SteamGridDB hero was in memory ~4 times.
// Larger answers are stopped: they'd use up HOME's memory.
constexpr std::size_t kMaxBodyBytes = 32u << 20;
struct BodyTooLarge : std::runtime_error { // not retried: it would be as large again
    using std::runtime_error::runtime_error;
};

std::string resultToString(Result rc) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned int)rc);
    return buf;
}

bool runtimeInitializedLocked() {
    return g_nifmInitialized && g_socketInitialized && g_curlInitialized;
}

void shutdownRuntimeLocked() {
    g_runtimeReady = false;
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

bool initializeRuntimeLocked() {
    if (runtimeInitializedLocked())
        return true;

    if (!g_nifmInitialized) {
        Result rc = nifmInitialize(NifmServiceType_User);
        if (R_FAILED(rc)) {
            DebugLog::log("[themeshop] nifmInitialize failed: %s", resultToString(rc).c_str());
            shutdownRuntimeLocked();
            return false;
        }
        g_nifmInitialized = true;
    }

    if (!g_socketInitialized) {
        // 6 BSD sessions instead of 3: each blocking socket call (a poll()
        // too) holds one, and the Discord gateway now waits in poll().
        SocketInitConfig cfg = *socketGetDefaultInitConfig();
        cfg.num_bsd_sessions = 6;
        Result rc = socketInitialize(&cfg);
        if (R_FAILED(rc)) {
            DebugLog::log("[themeshop] socketInitializeDefault failed: %s", resultToString(rc).c_str());
            shutdownRuntimeLocked();
            return false;
        }
        g_socketInitialized = true;
    }

    if (!g_curlInitialized) {
        CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK) {
            DebugLog::log("[themeshop] curl_global_init failed: %s", curl_easy_strerror(rc));
            shutdownRuntimeLocked();
            return false;
        }
        g_curlInitialized = true;
    }

    DebugLog::log("[themeshop] http runtime ready");
    g_runtimeReady = true;
    return true;
}

void ensureInternetConnectionReady(const std::string& url) {
    NifmInternetConnectionStatus status = NifmInternetConnectionStatus_ConnectingUnknown1;
    u32 strength = 0;
    Result rc = nifmGetInternetConnectionStatus(nullptr, &strength, &status);
    if (R_FAILED(rc)) {
        throw std::runtime_error("nifmGetInternetConnectionStatus failed: " + resultToString(rc));
    }
    if (status != NifmInternetConnectionStatus_Connected) {
        throw std::runtime_error("Internet connection is not ready for " + url);
    }
}

bool isAbortRequested(const themeshop::http::AbortCheck& shouldAbort) {
    return shouldAbort && shouldAbort();
}

template <class Body>
void configureRequest(curlpp::Easy& request,
                      const std::string& url,
                      Body& response,
                      bool& tooLarge,
                      const std::list<std::string>& headers,
                      const themeshop::http::ProgressCallback& onProgress = {},
                      const themeshop::http::AbortCheck& shouldAbort = {}) {
    request.setOpt<curlpp::options::Url>(url);
    request.setOpt<curlpp::options::FollowLocation>(true);
    request.setOpt<curlpp::options::NoSignal>(true);
    request.setOpt<curlpp::options::ConnectTimeout>(12L);
    request.setOpt<curlpp::options::Timeout>(30L);
    request.setOpt<curlpp::options::IpResolve>((long)CURL_IPRESOLVE_V4);
    request.setOpt<curlpp::options::UserAgent>(std::string("SwitchU/") + SWITCHU_VERSION);
    if (!headers.empty()) {
        request.setOpt<curlpp::options::HttpHeader>(headers);
    }
    CURL* handle = request.getHandle();
    request.setOpt<curlpp::options::WriteFunction>(
        [&response, &tooLarge, handle](char* data, std::size_t size, std::size_t count) -> std::size_t {
            const std::size_t bytes = size * count;
            if (response.empty()) {
                // Its announced size at once, instead of doubling up to it.
                curl_off_t total = -1;
                if (curl_easy_getinfo(handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total) == CURLE_OK
                    && total > 0 && static_cast<std::uint64_t>(total) <= kMaxBodyBytes)
                    response.reserve(static_cast<std::size_t>(total));
            }
            if (bytes > kMaxBodyBytes - response.size()) {
                tooLarge = true;
                return 0; // fewer bytes than given: curl stops the transfer
            }
            response.insert(response.end(), data, data + bytes);
            return bytes;
        });
    if (onProgress || shouldAbort) {
        request.setOpt<curlpp::options::NoProgress>(false);
        request.setOpt<curlpp::options::ProgressFunction>(
            [onProgress, shouldAbort](double downloadTotal, double downloaded, double, double) {
                if (isAbortRequested(shouldAbort))
                    return 1; // Non-zero aborts the curl transfer immediately.
                if (onProgress) {
                    onProgress(downloaded > 0.0 ? static_cast<std::uint64_t>(downloaded) : 0,
                               downloadTotal > 0.0 ? static_cast<std::uint64_t>(downloadTotal) : 0);
                }
                return 0;
            });
    }
}

template <class Body>
Body performRequestBytes(const std::string& url,
                         const std::list<std::string>& headers,
                         const themeshop::http::ProgressCallback& onProgress,
                         const themeshop::http::AbortCheck& shouldAbort) {
    if (isAbortRequested(shouldAbort))
        throw std::runtime_error("Cancelled");

    Body response;
    bool tooLarge = false;
    curlpp::Easy request;
    configureRequest(request, url, response, tooLarge, headers, onProgress, shouldAbort);
    try {
        request.perform();
    } catch (...) {
        if (isAbortRequested(shouldAbort))
            throw std::runtime_error("Cancelled");
        if (tooLarge)
            throw BodyTooLarge("Response larger than " + std::to_string(kMaxBodyBytes >> 20) + " MB");
        throw;
    }

    if (isAbortRequested(shouldAbort))
        throw std::runtime_error("Cancelled");

    long statusCode = curlpp::infos::ResponseCode::get(request);
    // (the start of an error page, for the message)
    std::string body = statusCode < 200 || statusCode >= 300
        ? std::string(reinterpret_cast<const char*>(response.data()), std::min<std::size_t>(response.size(), 240))
        : std::string();
    if (statusCode < 200 || statusCode >= 300) {
        std::string detail = body.substr(0, 240);
        std::replace(detail.begin(), detail.end(), '\n', ' ');
        std::replace(detail.begin(), detail.end(), '\r', ' ');
        throw std::runtime_error("HTTP error " + std::to_string(statusCode)
                                 + (detail.empty() ? std::string() : ": " + detail));
    }

    return response;
}

template <class Body>
Body performBytes(const std::string& url,
                  const std::list<std::string>& headers,
                  const themeshop::http::ProgressCallback& onProgress = {},
                  const themeshop::http::AbortCheck& shouldAbort = {}) {
    std::lock_guard<std::mutex> lk(g_themeHttpMutex);

    std::string lastError = "Theme Shop HTTP request failed";
    for (int attempt = 1; attempt <= kRequestAttemptCount; ++attempt) {
        if (isAbortRequested(shouldAbort))
            throw std::runtime_error("Cancelled");
        try {
            if (!initializeRuntimeLocked()) {
                throw std::runtime_error("Theme Shop HTTP runtime is unavailable");
            }

            ensureInternetConnectionReady(url);
            auto bytes = performRequestBytes<Body>(url, headers, onProgress, shouldAbort);
            if (attempt > 1) {
                DebugLog::log("[themeshop] request recovered on retry %d: %s", attempt, url.c_str());
            }
            return bytes;
        } catch (const std::exception& ex) {
            lastError = ex.what();
            if (lastError == "Cancelled" || isAbortRequested(shouldAbort))
                throw std::runtime_error("Cancelled");
            if (dynamic_cast<const BodyTooLarge*>(&ex))
                throw std::runtime_error(lastError);
            DebugLog::log("[themeshop] request failed (%d/%d): %s -> %s",
                          attempt,
                          kRequestAttemptCount,
                          url.c_str(),
                          ex.what());
        } catch (...) {
            if (isAbortRequested(shouldAbort))
                throw std::runtime_error("Cancelled");
            lastError = "Unknown HTTP error";
            DebugLog::log("[themeshop] request failed (%d/%d): %s -> unknown error",
                          attempt,
                          kRequestAttemptCount,
                          url.c_str());
        }

        if (attempt < kRequestAttemptCount) {
            shutdownRuntimeLocked();
        }
    }

    throw std::runtime_error(lastError);
}

} // namespace

namespace themeshop::http {

bool initialize() {
    if (g_runtimeReady.load()) return true;
    std::lock_guard<std::mutex> lk(g_themeHttpMutex);
    return initializeRuntimeLocked();
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_themeHttpMutex);
    shutdownRuntimeLocked();
}

bool isInitialized() {
    if (g_runtimeReady.load()) return true;
    std::lock_guard<std::mutex> lk(g_themeHttpMutex);
    return runtimeInitializedLocked();
}

std::vector<std::uint8_t> getBytes(const std::string& url,
                                   const std::list<std::string>& headers,
                                   const ProgressCallback& onProgress,
                                   const AbortCheck& shouldAbort) {
    return performBytes<std::vector<std::uint8_t>>(url, headers, onProgress, shouldAbort);
}

std::string getText(const std::string& url,
                    const std::list<std::string>& headers,
                    const AbortCheck& shouldAbort) {
    return performBytes<std::string>(url, headers, {}, shouldAbort);
}

} // namespace themeshop::http
