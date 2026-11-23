#pragma once
#include <atomic>
#include <switchu/tls_roots.hpp>

namespace switube::native {

inline thread_local const std::atomic<bool>* cancel = nullptr;
inline constexpr std::size_t maxAudioBytes = 24 * 1024 * 1024;

inline void configure(CURL* curl, const std::atomic<bool>* token) {
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, token);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
        +[](void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
            return data && static_cast<const std::atomic<bool>*>(data)->load() ? 1 : 0;
        });
}

} // namespace switube::native
