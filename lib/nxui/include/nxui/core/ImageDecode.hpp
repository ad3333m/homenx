#pragma once
// Picture decoding shared by Texture (both backends) and the menu's workers.
//
// stb_image decodes at full size before anything can be scaled down: a 4K
// picture is 33 MB of RGBA at once in the heap, whatever size it is shown at.
// So the size is read from the header first:
// - over kMaxPixels (3840x2160) it is refused with a log line. Theme
//   backgrounds, phone-size lock pictures and 3840x1240 SteamGridDB heroes
//   stay under it; CardsNX's own limit is 4 MiP (projects/cards Library).
// - from kBigPixels (2 MiP) on, one such decode runs at a time (BigDecode),
//   as CardsNX does, so two big pictures never sit in the heap together.
//
// Include this before defining STB_IMAGE_IMPLEMENTATION in a file that does.
#include <nxui/third_party/stb/stb_image.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace nxui::image {

constexpr long long kMaxPixels = 3840LL * 2160;
constexpr long long kBigPixels = 2LL * 1024 * 1024;

// Held around a decode of w x h: waits while another big one runs.
class BigDecode {
public:
    BigDecode(int w, int h) : m_big(static_cast<long long>(w) * h >= kBigPixels) {
        if (!m_big) return;
        std::unique_lock<std::mutex> lk(mutex());
        cv().wait(lk, [] { return !running(); });
        running() = true;
    }
    ~BigDecode() {
        if (!m_big) return;
        {
            std::lock_guard<std::mutex> lk(mutex());
            running() = false;
        }
        cv().notify_one();
    }
    BigDecode(const BigDecode&) = delete;
    BigDecode& operator=(const BigDecode&) = delete;

private:
    static std::mutex& mutex() { static std::mutex m; return m; }
    static std::condition_variable& cv() { static std::condition_variable c; return c; }
    static bool& running() { static bool r = false; return r; }
    bool m_big;
};

// False (with a log line) for a size that can't or shouldn't be decoded.
inline bool sizeAllowed(int w, int h, const char* what) {
    if (w <= 0 || h <= 0) {
        std::printf("[Texture] decoded %dx%d, refusing: %s\n", w, h, what ? what : "?");
        return false;
    }
    if (static_cast<long long>(w) * h > kMaxPixels) {
        std::printf("[Texture] %dx%d is over %lld pixels, refusing: %s\n", w, h, kMaxPixels, what ? what : "?");
        return false;
    }
    return true;
}

// Nearest-neighbour shrink to fit maxSide (0: keep). Takes the malloc'd/stbi
// buffer and returns a malloc'd one (std::free either way); w and h follow.
inline uint8_t* fitSide(uint8_t* data, int& w, int& h, int maxSide) {
    if (!data || maxSide <= 0 || (w <= maxSide && h <= maxSide)) return data;
    const float scale = std::min((float)maxSide / w, (float)maxSide / h);
    const int dw = std::max(1, (int)(w * scale));
    const int dh = std::max(1, (int)(h * scale));
    uint8_t* scaled = (uint8_t*)std::malloc((size_t)dw * dh * 4);
    if (!scaled) return data; // full size, as before
    for (int y = 0; y < dh; ++y) {
        const int sy = y * h / dh;
        for (int x = 0; x < dw; ++x) {
            const int sx = x * w / dw;
            std::memcpy(scaled + ((size_t)y * dw + x) * 4, data + ((size_t)sy * w + sx) * 4, 4);
        }
    }
    stbi_image_free(data);
    w = dw;
    h = dh;
    return scaled;
}

// RGBA8 at most maxSide per side, or nullptr. Free the result with std::free.
inline uint8_t* decodeFile(const char* path, int maxSide, int& w, int& h) {
    int ch = 0;
    w = h = 0;
    if (!path || !stbi_info(path, &w, &h, &ch)) {
        std::printf("[Texture] stbi_load FAILED: %s\n", path ? path : "?");
        return nullptr;
    }
    if (!sizeAllowed(w, h, path)) return nullptr;
    BigDecode one(w, h); // until shrunk below
    uint8_t* data = stbi_load(path, &w, &h, &ch, 4);
    if (!data) {
        std::printf("[Texture] stbi_load FAILED: %s\n", path);
        return nullptr;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(data);
        return nullptr;
    }
    return fitSide(data, w, h, maxSide);
}

inline uint8_t* decodeMemory(const uint8_t* bytes, size_t size, int maxSide, int& w, int& h) {
    int ch = 0;
    w = h = 0;
    if (!bytes || size == 0 || size > 0x7FFFFFFF) return nullptr;
    if (!stbi_info_from_memory(bytes, (int)size, &w, &h, &ch)) return nullptr;
    if (!sizeAllowed(w, h, "(in memory)")) return nullptr;
    BigDecode one(w, h); // until shrunk below
    uint8_t* data = stbi_load_from_memory(bytes, (int)size, &w, &h, &ch, 4);
    if (!data) return nullptr;
    if (w <= 0 || h <= 0) {
        stbi_image_free(data);
        return nullptr;
    }
    return fitSide(data, w, h, maxSide);
}

} // namespace nxui::image
