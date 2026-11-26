#include "media_transfer.hpp"
#include "transport.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <memory>
#include <strings.h>
#include <vector>

namespace switube::native {
namespace {

constexpr std::size_t rangeSize = 256 * 1024;

struct Range {
    std::vector<std::uint8_t> bytes;
    std::size_t first = 0, last = 0, total = 0;
    bool hasRange = false;
};

bool isAudioContainer(std::span<const std::uint8_t> bytes) {
    constexpr std::uint8_t webm[] = {0x1a, 0x45, 0xdf, 0xa3};
    return (bytes.size() >= sizeof(webm) && std::memcmp(bytes.data(), webm, sizeof(webm)) == 0)
        || (bytes.size() >= 8 && std::memcmp(bytes.data() + 4, "ftyp", 4) == 0);
}

} // namespace

std::string transferAudio(const std::string& url, std::uint64_t expectedBytes,
    const std::atomic<bool>& cancelled,
    const std::function<bool(std::span<const std::uint8_t>)>& consume) {
    if (url.rfind("https://", 0) != 0 || expectedBytes > maxAudioBytes)
        return "Audio URL or size is unsupported.";
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return "Could not initialize the audio connection.";

    auto* handle = curl.get();
    configure(handle, &cancelled);
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(handle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 5L);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15");

    Range response;
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, +[](char* data, size_t size, size_t count, void* context) -> size_t {
        auto& range = *static_cast<Range*>(context);
        const auto length = size * count;
        // A redirect can have its own body and headers. Keep only the final response.
        if (length >= 5 && std::memcmp(data, "HTTP/", 5) == 0) {
            range.bytes.clear();
            range.hasRange = false;
        } else if (length > 14 && length < 128 && strncasecmp(data, "Content-Range:", 14) == 0) {
            char header[128]{};
            std::memcpy(header, data + 14, length - 14);
            unsigned long long first = 0;
            unsigned long long last = 0;
            unsigned long long total = 0;
            if (std::sscanf(header, " bytes %llu-%llu/%llu", &first, &last, &total) == 3 &&
                first <= last && last < total && total <= maxAudioBytes) {
                range.first = first;
                range.last = last;
                range.total = total;
                range.hasRange = true;
            }
        }
        return length;
    });
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, +[](char* data, size_t size, size_t count, void* context) -> size_t {
        auto& bytes = static_cast<Range*>(context)->bytes;
        const auto length = size * count;
#ifdef HOMENX_STREAMING_MODULE
        // Bound RAM even when the server ignores the Range header.
        if (length > rangeSize - bytes.size()) return 0;
#else
        if (length > maxAudioBytes - bytes.size()) return 0;
#endif
        try {
            bytes.insert(bytes.end(), data, data + length);
        } catch (...) {
            return 0;
        }
        return length;
    });

    std::size_t offset = 0;
    std::size_t total = static_cast<std::size_t>(expectedBytes);
    do {
        const auto end = std::min(total ? total : maxAudioBytes, offset + rangeSize) - 1;
        const auto request = std::to_string(offset) + "-" + std::to_string(end);
        curl_easy_setopt(handle, CURLOPT_RANGE, request.c_str());
        const auto reset = [&] { response.bytes.clear(); response.hasRange = false; };
        reset();
        const auto rc = switchu::tls::perform(handle, url, switchu::tls::kHomeCerts, reset);
        long status = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        if (cancelled) return "Audio request cancelled.";
        if (rc != CURLE_OK) {
            if (status >= 400) return "Audio server returned HTTP " + std::to_string(status) + ".";
            return std::string("Audio connection failed: ") + curl_easy_strerror(rc) + ".";
        }
        if (status == 200 && !offset && !response.bytes.empty() &&
            (!total || response.bytes.size() == total)) {
            total = response.bytes.size();
        } else if (status == 206 && response.hasRange && response.first == offset &&
                   response.bytes.size() == response.last - response.first + 1 &&
                   (!offset || response.total == total)) {
            total = response.total;
        } else {
            return "The audio server returned an incomplete byte range.";
        }

        if (!offset && !isAudioContainer(response.bytes))
            return "The audio server did not return a WebM or M4A stream.";
        if (!consume(response.bytes)) return "Could not save or buffer the audio.";
        offset += response.bytes.size();
    } while (offset < total && !cancelled);

    return cancelled ? "Audio request cancelled." : std::string{};
}

} // namespace switube::native
