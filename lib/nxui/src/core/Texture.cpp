#include <nxui/core/Texture.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>

#include <nxui/core/ImageDecode.hpp> // before the implementation below
// stb_image only uses its NEON loops (JPEG IDCT, YCbCr->RGB, upsampling)
// when asked to; without this every icon and cover decodes in plain C.
#if defined(__aarch64__)
#define STBI_NEON
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace nxui {

Texture::Texture(Texture&& o) noexcept
    : m_image(o.m_image)
    , m_mem(static_cast<dk::MemBlock&&>(o.m_mem))
    , m_width(o.m_width), m_height(o.m_height)
    , m_slot(o.m_slot), m_valid(o.m_valid)
    , m_allocSize(o.m_allocSize)
    , m_gpu(o.m_gpu), m_renderer(o.m_renderer) {
    o.m_width = o.m_height = 0;
    o.m_slot = -1;
    o.m_valid = false;
    o.m_allocSize = 0;
    o.m_gpu = nullptr;
    o.m_renderer = nullptr;
}

Texture& Texture::operator=(Texture&& o) noexcept {
    if (this == &o) return *this;
    retireGpuResources();
    m_mem = nullptr;
    m_image = o.m_image;
    m_mem = static_cast<dk::MemBlock&&>(o.m_mem);
    m_width = o.m_width;
    m_height = o.m_height;
    m_slot = o.m_slot;
    m_valid = o.m_valid;
    m_allocSize = o.m_allocSize;
    m_gpu = o.m_gpu;
    m_renderer = o.m_renderer;
    o.m_width = o.m_height = 0;
    o.m_slot = -1;
    o.m_valid = false;
    o.m_allocSize = 0;
    o.m_gpu = nullptr;
    o.m_renderer = nullptr;
    return *this;
}

// Giving a descriptor slot and an image back is not free at any moment: the
// frame already handed to the GPU still samples that image through that slot.
// releaseSlot puts the index straight back on the renderer's free list, so the
// next texture loaded claims it and rewrites the descriptor, and destroying the
// UniqueMemBlock returns the pixels themselves. Do that mid-frame and the GPU
// reads a descriptor pointing at memory that is no longer there: the frame
// comes out as noise and deko3d ends the process with svcBreak.
//
// That is the theme-switch crash. Changing theme re-primes the installed
// previews, each one move-assigned over the texture the current frame is
// drawing. The user's file was never corrupt -- both default covers are the
// same 1280x720 JPEG, and his copy is byte-for-byte the right size.
//
// So both are retired instead of returned: the renderer frees them once no
// frame in flight can read them. Only a texture without a renderer waits for
// the GPU.
void Texture::retireGpuResources() {
    const bool holdsMemory = m_gpu && m_mem && m_allocSize > 0;
    if (m_slot < 0 && !holdsMemory)
        return;
    if (m_renderer) {
        // No waiting: the slot and the pixels go back a few frames from now,
        // once no frame in flight can read them (Renderer::beginFrame).
        if (m_slot >= 0)
            m_renderer->releaseTextureSlot(m_slot);
        if (holdsMemory)
            m_renderer->retireImageMemory(static_cast<dk::UniqueMemBlock&&>(m_mem), m_allocSize);
    } else {
        if (m_gpu)
            m_gpu->waitIdle();
        if (holdsMemory)
            m_gpu->freeImageMemory(m_allocSize);
    }
    m_slot = -1;
    m_allocSize = 0;
    m_valid = false;
}

Texture::~Texture() {
    retireGpuResources();
}

bool Texture::loadFromPixels(GpuDevice& gpu, Renderer& ren,
                             const uint8_t* rgba, int w, int h)
{
    m_gpu = &gpu;
    m_renderer = &ren;

    // Reloading in place is the same hazard as destruction: the old image and
    // its descriptor belong to the frames in flight until the GPU is done with
    // them. Waiting for it here stalled a frame or two each time (recycled
    // icon slots while scrolling, a new Spotify cover), so they are retired
    // like a destroyed texture's instead, and the picture gets new memory and
    // a new slot below. Never write the new pixels into the old block or point
    // the old slot at them without that wait: the GPU may still be reading
    // both.
    if (m_slot >= 0 || m_mem)
        retireGpuResources();

    m_valid = false;
    m_slot  = -1;
    m_allocSize = 0;

    // deko3d does not return an error for dimensions it cannot lay out: it
    // calls svcBreak, which kills the process mid-frame. A corrupt or truncated
    // image reaching this point must therefore be refused here, while refusing
    // is still possible. This is a guard, not the fix for the theme-switch
    // crash -- that one arrived with perfectly valid dimensions; see
    // retireGpuResources above.
    constexpr int kMaxSide = 16384;
    if (w <= 0 || h <= 0 || w > kMaxSide || h > kMaxSide) {
        std::printf("[Texture] refusing %dx%d image\n", w, h);
        m_width = m_height = 0;
        return false;
    }

    m_width  = w;
    m_height = h;

    dk::ImageLayout layout;
    dk::ImageLayoutMaker{gpu.device()}
        .setFlags(0)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(w, h)
        .initialize(layout);

    uint32_t needed = layout.getSize();

    m_mem = gpu.allocImageMemory(needed);
    if (!m_mem) {
        std::printf("[Texture] allocImageMemory FAILED (%dx%d) — GPU budget exhausted\n", w, h);
        // Not drawable and holding nothing: m_image still describes the old
        // block, which is retired, and a stale m_allocSize would give bytes
        // back to the budget twice. Both once showed up as crashes (svcBreak
        // drawing freed memory, then real exhaustion), not as a missing icon.
        m_valid = false;
        m_slot = -1;
        m_allocSize = 0;
        return false;
    }
    m_allocSize = needed;

    m_image.initialize(layout, m_mem, 0);

    if (!gpu.uploadTexture(m_image, rgba, w * h * 4, w, h)) {
        std::printf("[Texture] uploadTexture FAILED (%dx%d)\n", w, h);
        // Half a texture is worse than none: it can still be bound and drawn.
        m_valid = false;
        return false;
    }

    noteUploaded();
    dk::ImageView view{m_image};
    m_slot = ren.registerTexture(view);
    if (m_slot < 0) {
        std::printf("[Texture] registerTexture FAILED (%dx%d) — descriptor pool full\n", w, h);
        // Half a texture is worse than none: it can still be bound and drawn.
        m_valid = false;
        return false;
    }
    m_valid = true;
    return true;
}

bool Texture::loadFromPixelsPooled(GpuDevice& gpu, Renderer& ren,
                                    const uint8_t* rgba, int w, int h)
{
    m_gpu = &gpu;
    m_renderer = &ren;
    m_valid = false;
    m_slot  = -1;
    m_width  = w;
    m_height = h;

    dk::ImageLayout layout;
    dk::ImageLayoutMaker{gpu.device()}
        .setFlags(0)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(w, h)
        .initialize(layout);

    auto alloc = gpu.allocImageFromPool(layout.getSize(), layout.getAlignment());
    if (!alloc.valid()) {
        std::printf("[Texture] pool alloc FAILED (%dx%d) — budget exhausted\n", w, h);
        // Half a texture is worse than none: it can still be bound and drawn.
        m_valid = false;
        return false;
    }
    // m_mem stays empty — pool owns the memory.
    m_image.initialize(layout, alloc.block, alloc.offset);

    if (!gpu.uploadTexture(m_image, rgba, w * h * 4, w, h)) {
        std::printf("[Texture] uploadTexture FAILED (%dx%d)\n", w, h);
        // Half a texture is worse than none: it can still be bound and drawn.
        m_valid = false;
        return false;
    }

    noteUploaded();
    dk::ImageView view{m_image};
    m_slot = ren.registerTexture(view);
    if (m_slot < 0) {
        std::printf("[Texture] registerTexture FAILED (%dx%d) — descriptor pool full\n", w, h);
        // Half a texture is worse than none: it can still be bound and drawn.
        m_valid = false;
        return false;
    }
    m_valid = true;
    return true;
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
    // Convert to tightly packed RGBA if pitch != w*4
    if (pitch == w * 4) {
        return loadFromPixels(gpu, ren, data, w, h);
    }
    std::vector<uint8_t> tight(w * h * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(tight.data() + y * w * 4, data + y * pitch, w * 4);
    return loadFromPixels(gpu, ren, tight.data(), w, h);
}

} // namespace nxui
