#include <cmath>
#include <nxui/core/Font.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(SDL_TTF_VERSION_ATLEAST)
#if SDL_TTF_VERSION_ATLEAST(2, 0, 18)
#define NXUI_TTF_GLYPH32 1
#endif
#endif

namespace nxui {
namespace {

std::uint32_t nextCp(const char*& p, const char* end) {
    const unsigned char c = static_cast<unsigned char>(*p++);
    if (c < 0x80) return c;
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
    if (extra == 0) return 0xFFFD;
    std::uint32_t cp = extra == 1 ? (c & 0x1Fu) : extra == 2 ? (c & 0x0Fu) : (c & 0x07u);
    while (extra-- > 0 && p < end && (static_cast<unsigned char>(*p) & 0xC0) == 0x80)
        cp = (cp << 6) | (static_cast<unsigned char>(*p++) & 0x3Fu);
    return cp;
}

bool provides(TTF_Font* f, std::uint32_t cp) {
#ifdef NXUI_TTF_GLYPH32
    return TTF_GlyphIsProvided32(f, cp) != 0;
#else
    return cp <= 0xFFFF && TTF_GlyphIsProvided(f, static_cast<Uint16>(cp)) != 0;
#endif
}

} // namespace

std::vector<Font::FallbackData>& Font::fallbackData() {
    static std::vector<FallbackData> data;
    return data;
}

void Font::addFallback(const void* data, std::size_t size) {
    if (data && size) fallbackData().push_back({data, size});
}

Font::~Font() {
    for (TTF_Font* f : m_fallbacks)
        if (f) TTF_CloseFont(f);
    if (m_font) TTF_CloseFont(m_font);
}

TTF_Font* Font::fontFor(std::uint32_t cp) const {
    if (!m_font) return nullptr;
    if (provides(m_font, cp)) return m_font;
    const auto& data = fallbackData();
    while (m_fallbacks.size() < data.size()) {
        const FallbackData& d = data[m_fallbacks.size()];
        SDL_RWops* rw = SDL_RWFromConstMem(d.data, static_cast<int>(d.size));
        m_fallbacks.push_back(rw ? TTF_OpenFontRW(rw, 1, m_pxSize) : nullptr);
    }
    for (TTF_Font* f : m_fallbacks)
        if (f && provides(f, cp)) return f;
    return nullptr;
}

// True when this font can't draw all of the text: then `out` holds the text
// in pieces, each with the font that has those characters (none: left out).
bool Font::runs(const std::string& text, std::vector<std::pair<TTF_Font*, std::string>>& out) const {
    out.clear();
    bool mixed = false;
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        const char* start = p;
        const std::uint32_t cp = nextCp(p, end);
        TTF_Font* f = (cp == ' ' && !out.empty()) ? out.back().first : fontFor(cp);
        if (f != m_font) mixed = true;
        if (!f) continue;
        if (out.empty() || out.back().first != f) out.emplace_back(f, std::string());
        out.back().second.append(start, p);
    }
    return mixed;
}

SDL_Surface* Font::renderRuns(const std::vector<std::pair<TTF_Font*, std::string>>& pieces) const {
    const SDL_Color white = {255, 255, 255, 255};
    int ascent = 0;
    for (const auto& pc : pieces) ascent = std::max(ascent, TTF_FontAscent(pc.first));
    std::vector<std::pair<SDL_Surface*, int>> parts;
    int w = 0, h = 0;
    for (const auto& pc : pieces) {
        SDL_Surface* part = TTF_RenderUTF8_Blended(pc.first, pc.second.c_str(), white);
        if (!part) continue;
        const int top = ascent - TTF_FontAscent(pc.first); // one baseline for every font
        parts.emplace_back(part, top);
        w += part->w;
        h = std::max(h, top + part->h);
    }
    SDL_Surface* out = (w > 0 && h > 0)
        ? SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888) : nullptr;
    if (out) {
        SDL_FillRect(out, nullptr, 0);
        int x = 0;
        for (auto& part : parts) {
            SDL_SetSurfaceBlendMode(part.first, SDL_BLENDMODE_NONE);
            SDL_Rect dst{x, part.second, part.first->w, part.first->h};
            SDL_BlitSurface(part.first, nullptr, out, &dst);
            x += part.first->w;
        }
    }
    for (auto& part : parts) SDL_FreeSurface(part.first);
    return out;
}

bool Font::load(GpuDevice& gpu, Renderer& ren,
                const std::string& path, int ptSize)
{
    const int pxSize = std::max(1, static_cast<int>(std::lround(ptSize * gpu.scale()))); // sharp at 1080p
    TTF_Font* newFont = TTF_OpenFont(path.c_str(), pxSize);
    if (!newFont) {
        std::fprintf(stderr, "[Font] TTF_OpenFont failed: %s\n", TTF_GetError());
        return false;
    }

    if (m_font)
        TTF_CloseFont(m_font);
    for (TTF_Font* f : m_fallbacks)
        if (f) TTF_CloseFont(f);
    m_fallbacks.clear(); // reopened at the new size when needed

    m_font = newFont;
    m_gpu = &gpu;
    m_ren = &ren;
    m_ptSize = ptSize;
    m_pxSize = pxSize;
    m_pixelScale = static_cast<float>(pxSize) / static_cast<float>(std::max(1, ptSize));
    ++m_revision;
    clearCache();
    return true;
}

// This font or one of the fallbacks.
bool Font::hasGlyph(std::uint32_t cp) const {
    return fontFor(cp) != nullptr;
}

Vec2 Font::measure(const std::string& text) const {
    if (!m_font || text.empty()) return {0, 0};
    // Rendered strings already carry their exact dimensions. Avoid invoking
    // SDL_ttf again for every label on every frame.
    const auto cached = m_lruMap.find(text);
    if (cached != m_lruMap.end())
        return {static_cast<float>(cached->second->w) / m_pixelScale,
                static_cast<float>(cached->second->h) / m_pixelScale};
    int w = 0, h = 0;
    std::vector<std::pair<TTF_Font*, std::string>> pieces;
    if (!runs(text, pieces)) {
        TTF_SizeUTF8(m_font, text.c_str(), &w, &h);
        return {(float)w / m_pixelScale, (float)h / m_pixelScale};
    }
    int ascent = 0;
    for (const auto& pc : pieces) ascent = std::max(ascent, TTF_FontAscent(pc.first));
    for (const auto& pc : pieces) {
        int pw = 0, ph = 0;
        TTF_SizeUTF8(pc.first, pc.second.c_str(), &pw, &ph);
        w += pw;
        h = std::max(h, ascent - TTF_FontAscent(pc.first) + ph);
    }
    return {(float)w / m_pixelScale, (float)h / m_pixelScale};
}

void Font::clearCache() {
    m_lruList.clear();
    m_lruMap.clear();
    m_cacheBytes = 0;
    m_maintenanceRequested = false;
}

void Font::trimCache(std::size_t maxEntries, std::size_t maxBytes) {
    maxBytes = static_cast<std::size_t>(static_cast<float>(maxBytes) * m_pixelScale * m_pixelScale);
    while (!m_lruList.empty() &&
           (m_lruList.size() > maxEntries || m_cacheBytes > maxBytes)) {
        auto victim = std::prev(m_lruList.end());
        const std::size_t bytes = victim->tex.allocationSize();
        m_lruMap.erase(victim->key);
        m_lruList.erase(victim);
        m_cacheBytes = bytes <= m_cacheBytes ? m_cacheBytes - bytes : 0;
    }
    m_maintenanceRequested = false;
}

bool Font::renderTo(Renderer& ren, const std::string& text, Texture& out) {
    if (!m_font || !m_gpu || text.empty()) return false;
    SDL_Color white = {255, 255, 255, 255};
    std::vector<std::pair<TTF_Font*, std::string>> pieces;
    SDL_Surface* surface = runs(text, pieces) ? renderRuns(pieces)
                                              : TTF_RenderUTF8_Blended(m_font, text.c_str(), white);
    if (!surface) return false;
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(surface);
    if (!rgba) return false;
    SDL_LockSurface(rgba);
    GpuDevice::EssentialScope essential; // text: may use the reserve
    const bool ok = out.loadFromSurface(*m_gpu, ren, static_cast<const uint8_t*>(rgba->pixels),
                                        rgba->w, rgba->h, rgba->pitch);
    SDL_UnlockSurface(rgba);
    SDL_FreeSurface(rgba);
    return ok;
}

Texture* Font::getOrRender(GpuDevice& gpu, Renderer& ren, const std::string& text) {
    // Cache hit: promote to the front.
    auto it = m_lruMap.find(text);
    if (it != m_lruMap.end()) {
        // Move to front (most-recently-used)
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
        return &it->second->tex;
    }

    // Render the string via SDL_ttf.
    SDL_Color white = {255, 255, 255, 255};
    std::vector<std::pair<TTF_Font*, std::string>> pieces;
    SDL_Surface* surface = runs(text, pieces) ? renderRuns(pieces)
                                              : TTF_RenderUTF8_Blended(m_font, text.c_str(), white);
    if (!surface) return nullptr;

    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(surface);
    if (!rgba) return nullptr;

    SDL_LockSurface(rgba);
    const uint8_t* pixels = static_cast<const uint8_t*>(rgba->pixels);
    int w = rgba->w, h = rgba->h, pitch = rgba->pitch;

    constexpr std::size_t kTextureAllocationAlignment = 4096u;
    const std::size_t estimatedBytes =
        (static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u +
         kTextureAllocationAlignment - 1u) &
        ~(kTextureAllocationAlignment - 1u);
    if (m_lruList.size() >= kMaxCacheEntries ||
        m_cacheBytes + estimatedBytes > static_cast<std::size_t>(kMaxCacheBytes * m_pixelScale * m_pixelScale) ||
        gpu.imageMemoryAvailable() < estimatedBytes + kGpuHeadroom) {
        m_maintenanceRequested = true;
        SDL_UnlockSurface(rgba);
        SDL_FreeSurface(rgba);
        return nullptr;
    }

    m_lruList.emplace_front();
    auto& entry = m_lruList.front();
    entry.key = text;
    entry.w = w;
    entry.h = h;
    GpuDevice::EssentialScope essential; // text: may use the reserve
    if (!entry.tex.loadFromSurface(gpu, ren, pixels, w, h, pitch)) {
        m_lruList.pop_front();
        m_maintenanceRequested = true;
        SDL_UnlockSurface(rgba);
        SDL_FreeSurface(rgba);
        return nullptr;
    }
    m_cacheBytes += entry.tex.allocationSize();

    SDL_UnlockSurface(rgba);
    SDL_FreeSurface(rgba);

    m_lruMap[text] = m_lruList.begin();
    return &m_lruList.front().tex;
}

bool Font::draw(Renderer& ren, const std::string& text,
                const Vec2& pos, const Color& color, float scale) {
    if (!m_font || !m_gpu || text.empty()) return false;

    Texture* tex = getOrRender(*m_gpu, ren, text);
    if (!tex || !tex->valid()) return false;

    float w = tex->width()  * scale / m_pixelScale;
    float h = tex->height() * scale / m_pixelScale;
    ren.drawTexture(tex, {pos.x, pos.y, w, h}, color);
    return true;
}

} // namespace nxui
