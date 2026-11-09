#include <nxui/core/Texture.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>
#include <SDL2/SDL.h>

#include <nxui/core/ImageDecode.hpp> // before the implementation below
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>

namespace nxui {

Texture::~Texture() {
    if (m_sdlTex) {
        SDL_DestroyTexture(m_sdlTex);
        m_sdlTex = nullptr;
    }
}

Texture::Texture(Texture&& o) noexcept
    : m_sdlTex(o.m_sdlTex)
    , m_gpu(o.m_gpu)
    , m_width(o.m_width), m_height(o.m_height)
    , m_slot(o.m_slot), m_valid(o.m_valid)
{
    o.m_sdlTex = nullptr;
    o.m_width = o.m_height = 0;
    o.m_slot = -1;
    o.m_valid = false;
    o.m_gpu = nullptr;
}

Texture& Texture::operator=(Texture&& o) noexcept {
    if (this != &o) {
        if (m_sdlTex) SDL_DestroyTexture(m_sdlTex);
        m_sdlTex = o.m_sdlTex;
        m_gpu    = o.m_gpu;
        m_width  = o.m_width;
        m_height = o.m_height;
        m_slot   = o.m_slot;
        m_valid  = o.m_valid;
        o.m_sdlTex = nullptr;
        o.m_width = o.m_height = 0;
        o.m_slot = -1;
        o.m_valid = false;
        o.m_gpu = nullptr;
    }
    return *this;
}

bool Texture::loadFromPixels(GpuDevice& gpu, Renderer& ren,
                             const uint8_t* rgba, int w, int h)
{
    m_gpu = &gpu;
    m_valid = false;
    m_width  = w;
    m_height = h;

    // Destroy old texture if reusing
    if (m_sdlTex) {
        SDL_DestroyTexture(m_sdlTex);
        m_sdlTex = nullptr;
    }

    m_sdlTex = SDL_CreateTexture(gpu.sdlRenderer(),
        SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!m_sdlTex) {
        std::fprintf(stderr, "[Texture-SDL2] CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_SetTextureBlendMode(m_sdlTex, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(m_sdlTex, nullptr, rgba, w * 4);

    // Register in the renderer's slot table
    if (m_slot < 0) {
        m_slot = ren.gpu().slot(); // Will use the renderer's internal tracking
        // The SDL2 renderer tracks textures differently — slot is just an ID
    }

    m_valid = true;
    return true;
}

bool Texture::loadFromPixelsPooled(GpuDevice& gpu, Renderer& ren,
                                    const uint8_t* rgba, int w, int h)
{
    // SDL2 has no pool concept — just use the normal path
    return loadFromPixels(gpu, ren, rgba, w, h);
}

bool Texture::loadFromFile(GpuDevice& gpu, Renderer& ren, const std::string& path, int maxSide) {
    // Size read first, over 4 MP refused, then shrunk to maxSide (ImageDecode.hpp).
    int w = 0, h = 0;
    uint8_t* data = image::decodeFile(path.c_str(), maxSide, w, h);
    if (!data) {
        // Half a texture is worse than none: it can still be bound and drawn.
        m_valid = false;
        return false;
    }
    bool ok = loadFromPixels(gpu, ren, data, w, h);
    // stbi_image_free uses free(), and malloc'd data also uses free()
    std::free(data);
    return ok;
}

bool Texture::loadFromMemory(GpuDevice& gpu, Renderer& ren,
                             const uint8_t* data, size_t dataSize, int maxSide)
{
    int w = 0, h = 0;
    uint8_t* pixels = image::decodeMemory(data, dataSize, maxSide, w, h);
    if (!pixels) return false;
    bool ok = loadFromPixels(gpu, ren, pixels, w, h);
    std::free(pixels);
    return ok;
}

bool Texture::loadFromSurface(GpuDevice& gpu, Renderer& ren,
                              const uint8_t* data, int w, int h, int pitch)
{
    if (pitch == w * 4) {
        return loadFromPixels(gpu, ren, data, w, h);
    }
    std::vector<uint8_t> tight(w * h * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(tight.data() + y * w * 4, data + y * pitch, w * 4);
    return loadFromPixels(gpu, ren, tight.data(), w, h);
}

} // namespace nxui
