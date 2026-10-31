#pragma once
#include <nxui/core/Types.hpp>
#include "Texture.hpp"
#include <SDL2/SDL_ttf.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <list>
#include <utility>
#include <vector>

namespace nxui {

class Renderer;
class GpuDevice;

class Font {
public:
    Font() = default;
    ~Font();

    bool load(GpuDevice& gpu, Renderer& ren,
              const std::string& path, int ptSize);

    // Render a string to a texture (cached) and draw it. False when it
    // couldn't be drawn this frame (no room for its texture yet).
    bool draw(Renderer& ren, const std::string& text,
              const Vec2& pos, const Color& color, float scale = 1.f);

    // Renders text into a texture the caller keeps (outside the cache, so it
    // is never evicted). False if there's no room for it right now.
    bool renderTo(Renderer& ren, const std::string& text, Texture& out);

    // Measure text dimensions
    Vec2 measure(const std::string& text) const;
    // Whether the font has a glyph for this Unicode codepoint.
    bool hasGlyph(std::uint32_t cp) const;

    // Clear all cached glyph textures (call before GPU descriptor reset)
    void clearCache();

    bool maintenanceRequested() const { return m_maintenanceRequested; }
    std::size_t cacheBytes() const { return m_cacheBytes; }
    std::size_t cacheEntryCount() const { return m_lruList.size(); }
    void trimCache(std::size_t maxEntries = 72,
                   std::size_t maxBytes = 2u * 1024u * 1024u);

    int ptSize() const { return m_ptSize; }
    // Text is rendered this many times its size (the framebuffer's scale: 1.5 at
    // 1080p) so it stays sharp; measure() and draw() are in logical units. A
    // texture from renderTo() is drawn at its size divided by this.
    float pixelScale() const { return m_pixelScale; }
    std::uint64_t revision() const { return m_revision; }

    // Fonts every Font falls back to for characters it doesn't have (the
    // console's Japanese/Chinese/Korean/symbol fonts). The data must stay valid.
    static void addFallback(const void* data, std::size_t size);

private:
    // Render full string to texture (cache by string)
    Texture* getOrRender(GpuDevice& gpu, Renderer& ren, const std::string& text);
    // The font for a character: this one, else a fallback that has it, else null.
    TTF_Font* fontFor(std::uint32_t cp) const;
    // True (and the text in pieces, one font each) when this font lacks some of it.
    bool runs(const std::string& text, std::vector<std::pair<TTF_Font*, std::string>>& out) const;
    SDL_Surface* renderRuns(const std::vector<std::pair<TTF_Font*, std::string>>& pieces) const;

    struct FallbackData { const void* data; std::size_t size; };
    static std::vector<FallbackData>& fallbackData();
    mutable std::vector<TTF_Font*> m_fallbacks; // the fallbacks at this size, opened on first need

    TTF_Font* m_font = nullptr;
    int       m_ptSize = 0;    // logical
    int       m_pxSize = 0;    // what it's opened at
    float     m_pixelScale = 1.f;
    GpuDevice* m_gpu = nullptr;
    Renderer*  m_ren = nullptr;
    std::uint64_t m_revision = 0;

    static constexpr std::size_t kMaxCacheEntries = 144;
    static constexpr std::size_t kMaxCacheBytes = 4u * 1024u * 1024u;
    static constexpr std::size_t kGpuHeadroom = 768u * 1024u;

    struct CacheEntry {
        std::string key;
        Texture     tex;
        int         w = 0, h = 0;
    };
    using LruList = std::list<CacheEntry>;
    using LruMap  = std::unordered_map<std::string, LruList::iterator>;

    LruList m_lruList;
    LruMap  m_lruMap;
    std::size_t m_cacheBytes = 0;
    bool m_maintenanceRequested = false;
};

} // namespace nxui
