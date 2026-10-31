#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>

#ifdef NXUI_BACKEND_DEKO3D
#include <deko3d.hpp>
#include <switch.h>
#else
// SDL2 backend — forward declare what we need
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
#include <switch.h>
#endif

namespace nxui {

#ifdef NXUI_BACKEND_DEKO3D
inline constexpr uint32_t kGpuAlign = DK_MEMBLOCK_ALIGNMENT;   // 0x1000

struct GpuPool {
    dk::UniqueMemBlock block;
    uint32_t size  = 0;
    uint32_t used  = 0;
    void*    cpuBase = nullptr;
    DkGpuAddr gpuBase = 0;

    bool create(dk::Device dev, uint32_t sz, uint32_t flags) {
        size = (sz + kGpuAlign - 1) & ~(kGpuAlign - 1);
        block = dk::MemBlockMaker{dev, size}.setFlags(flags).create();
        if (flags & DkMemBlockFlags_CpuUncached)
            cpuBase = block.getCpuAddr();
        gpuBase = block.getGpuAddr();
        used = 0;
        return true;
    }

    uint32_t alloc(uint32_t bytes, uint32_t align = 256) {
        uint32_t off = (used + align - 1) & ~(align - 1);
        if (off + bytes > size) return UINT32_MAX;
        used = off + bytes;
        return off;
    }

    DkGpuAddr gpuAddr(uint32_t off = 0) const { return gpuBase + off; }
    void*     cpuAddr(uint32_t off = 0) const { return (uint8_t*)cpuBase + off; }
};
#endif // NXUI_BACKEND_DEKO3D

class GpuDevice {
public:
    // Screens are laid out at 1280x720 (the logical size: FB_WIDTH, FB_HEIGHT);
    // the framebuffers can be bigger (1920x1080 on a TV, setResolution): the
    // renderer scales everything and text is rendered at the real size. Captures
    // (readback) stay 1280x720, as before.
    static constexpr int FB_WIDTH  = 1280;
    static constexpr int FB_HEIGHT = 720;
    static constexpr int NUM_FB    = 2;
    // Before initialize(): the framebuffers' real size (1280x720 or 1920x1080).
    static void setResolution(int w, int h) { s_fbW = w; s_fbH = h; }
    // Once the first frame is on screen (a new resolution that works: see the menu's main).
    static void setFirstFrameHook(void (*hook)()) { s_firstFrameHook = hook; }

    static constexpr int MAX_TEXTURES   = 2048;
    static constexpr int MAX_SAMPLERS   = 16;
    static constexpr int MAX_VERTICES   = 32768;
    static constexpr int VTX_BUF_SIZE   = MAX_VERTICES * 56;
    static constexpr int IDX_BUF_SIZE   = 256;
    static constexpr int VS_UBO_SIZE    = 256;
    static constexpr int FS_UBO_SIZE    = 256;
    static constexpr int FS_UBO_RING    = 512;
    static constexpr int CMD_BUF_SIZE   = 256 * 1024;
    static constexpr int CODE_POOL_SIZE = 256 * 1024;

    bool initialize();
    void shutdown();

    int  beginFrame();
    void endFrame();
    void waitIdle();

    /// Copy the most recently presented framebuffer into tightly-packed RGBA8.
    /// When halfRes is true, downsamples to FB_WIDTH/2 x FB_HEIGHT/2 via the
    /// existing offscreen target. Must be called outside beginFrame/endFrame
    /// (e.g. from Activity::onAfterPresent).
    bool downloadFramebufferRgba(std::vector<uint8_t>& outRgba,
                                 int& outW, int& outH,
                                 bool halfRes = true);
#ifdef NXUI_BACKEND_DEKO3D
    /// Full-res framebuffer copy left in a CPU-visible block, so a worker thread
    /// can read the pixels (no 3.6 MB copy on the render thread). The pixels live
    /// in a block the device keeps and reuses: valid until the next call.
    const uint8_t* readFramebufferPixels(int& outW, int& outH);
    /// Allocate that block (and the blit target) ahead of time, so the first
    /// capture (a press on a game) doesn't stall on ~7 MB of GPU allocation.
    bool prepareFramebufferReadback();
    /// The same capture without waiting for the GPU (no hitch on the render
    /// thread): begin...() queues the copy of the frame just presented, and
    /// finish...() (any thread) waits for it and returns the pixels.
    bool beginFramebufferReadback();
    const uint8_t* finishFramebufferReadback(int& outW, int& outH);
#endif

    int  width()  const { return m_fbW; }  // the framebuffers' real size
    int  height() const { return m_fbH; }
    float scale() const { return static_cast<float>(m_fbW) / static_cast<float>(FB_WIDTH); } // real / logical

#ifdef NXUI_BACKEND_DEKO3D
    // deko3d-specific accessors
    dk::Device   device()  const { return m_dev; }
    dk::Queue    queue()   const { return m_queue; }
    dk::CmdBuf  cmdBuf()  const { return m_cmdbuf[m_slot]; }
    int          slot()    const { return m_slot; }

    GpuPool& codePool()  { return m_codePool; }
    GpuPool& dataPool()  { return m_dataPool; }
    GpuPool& imagePool() { return m_imagePool; }

    DkGpuAddr imgDescGpuAddr()  const { return m_dataPool.gpuAddr(m_imgDescOff); }
    DkGpuAddr samDescGpuAddr()  const { return m_dataPool.gpuAddr(m_samDescOff); }
    void*     imgDescCpuAddr()  const { return m_dataPool.cpuAddr(m_imgDescOff); }
    void*     samDescCpuAddr()  const { return m_dataPool.cpuAddr(m_samDescOff); }

    DkGpuAddr vtxGpuAddr(int frame)   const { return m_dataPool.gpuAddr(m_vtxOff[frame]); }
    void*     vtxCpuAddr(int frame)   const { return m_dataPool.cpuAddr(m_vtxOff[frame]); }
    DkGpuAddr idxGpuAddr(int frame)   const { return m_dataPool.gpuAddr(m_idxOff[frame]); }
    void*     idxCpuAddr(int frame)   const { return m_dataPool.cpuAddr(m_idxOff[frame]); }
    DkGpuAddr vsUboGpuAddr(int frame) const { return m_dataPool.gpuAddr(m_vsUboOff[frame]); }
    void*     vsUboCpuAddr(int frame) const { return m_dataPool.cpuAddr(m_vsUboOff[frame]); }
    DkGpuAddr fsUboGpuAddr(int frame) const { return m_dataPool.gpuAddr(m_fsUboOff[frame]); }
    void*     fsUboCpuAddr(int frame) const { return m_dataPool.cpuAddr(m_fsUboOff[frame]); }
    DkGpuAddr nextFsUboGpuAddr(int frame) {
        const uint32_t index = m_fsUboRingPos[frame];
        m_fsUboRingPos[frame] = (index + 1u) % FS_UBO_RING;
        return m_dataPool.gpuAddr(m_fsUboOff[frame] + index * FS_UBO_SIZE);
    }
    void resetFsUboRing(int frame) { m_fsUboRingPos[frame] = 0; }

    struct ImageAlloc {
        dk::MemBlock block;
        uint32_t     offset = 0;
        bool valid() const { return (bool)block; }
    };

    ImageAlloc allocImageFromPool(uint32_t size, uint32_t alignment = 0);
    void resetImagePool();
    dk::UniqueMemBlock allocImageMemory(uint32_t size);
    void freeImageMemory(uint32_t size);
    // The budget's side only: the caller has made sure the GPU is done with it
    // (Renderer::retireImageMemory frees it frames later, without waiting).
    void forgetImageMemory(uint32_t size);

    // The menu applet reserves a 224 MiB heap. Keep the dynamic image cache
    // well below that process ceiling so CPU-side decodes, audio and Deko's
    // fixed framebuffer/offscreen pools retain comfortable headroom.
    static constexpr uint64_t kDefaultImageBudget = 64u * 1024u * 1024u;
    // HomeNX: the last 8 MiB of the budget only go to what a screen can't do
    // without: text (the clock too) and app icons, uploaded inside an
    // EssentialScope. Other pictures (artwork, news, Discord) are refused
    // first, so they can't starve the text cache. When they did, it evicted and
    // re-rendered labels every frame behind a GPU wait: lag and blank text.
    static constexpr uint64_t kEssentialReserve = 8u * 1024u * 1024u;
    struct EssentialScope {
        EssentialScope() { ++s_essential; }
        ~EssentialScope() { --s_essential; }
    };
    uint64_t imageMemoryUsed() const { return m_imageMemUsed; }
    uint64_t imageMemoryBudget() const { return s_imageBudget; }
    // An app with more memory than HOME (CardsNX) can allow more pictures.
    static void setImageBudget(uint64_t bytes) { s_imageBudget = bytes; }
    uint64_t imageMemoryAvailable() const {
        return m_imageMemUsed < s_imageBudget
            ? s_imageBudget - m_imageMemUsed : 0;
    }


    using LogSink = void (*)(const char* message);
    static void setLogSink(LogSink sink) { s_logSink = sink; }
    static void logGpu(const char* fmt, ...);

    bool uploadTexture(dk::Image& dst, const void* pixels, uint32_t size, uint32_t width, uint32_t height);
    void beginTextureUploadBatch();
    void endTextureUploadBatch();
    void waitForTextureUploads();

    // Time the CPU spent waiting on the GPU since the last call: for frames
    // (the swapchain and the slot's fence) and for uploads (a slot still busy).
    struct WaitStats { float frameMs = 0.f, uploadMs = 0.f; };
    WaitStats takeWaitStats() { WaitStats s = m_waits; m_waits = {}; return s; }

    static constexpr int NUM_OFFSCREEN = 3;
    dk::Image&       offscreenImage(int i)       { return m_offImages[i]; }
    const dk::Image& offscreenImage(int i) const { return m_offImages[i]; }
    bool offscreenReady() const { return m_offscreenReady; }

    dk::Image&       fbImage(int i)       { return m_fbImages[i]; }
    const dk::Image& fbImage(int i) const { return m_fbImages[i]; }
    dk::Image&       dsImage()            { return m_dsImage; }
    const dk::Image& dsImage() const      { return m_dsImage; }
#else
    // SDL2-specific accessors
    SDL_Renderer* sdlRenderer() const { return m_sdlRenderer; }
    SDL_Window*   sdlWindow()   const { return m_sdlWindow; }
    int           slot()        const { return m_slot; }

    // Stubs for pool-based allocation (SDL2 uses SDL_CreateTexture directly)
    void resetImagePool() {}
    static constexpr int NUM_OFFSCREEN = 3;
    bool offscreenReady() const { return false; }
    uint64_t imageMemoryUsed() const { return 0; }
    uint64_t imageMemoryBudget() const { return UINT64_MAX; }
    uint64_t imageMemoryAvailable() const { return UINT64_MAX; }
    struct EssentialScope {}; // no budget here
#endif

private:
#ifdef NXUI_BACKEND_DEKO3D
    bool copyFramebuffer(dk::UniqueMemBlock& staging, int& outW, int& outH, bool halfRes);
#endif
#ifdef NXUI_BACKEND_DEKO3D
    void createFramebuffers();
    void createDepthStencil();
    void createOffscreenTargets();

    dk::UniqueDevice    m_dev;
    dk::UniqueQueue     m_queue;
    dk::UniqueCmdBuf    m_cmdbuf[NUM_FB];
    dk::UniqueCmdBuf    m_uploadCmdbuf;
    dk::UniqueSwapchain m_swapchain;

    GpuPool m_fbPool;
    GpuPool m_dsPool;
    GpuPool m_cmdPool[NUM_FB];
    GpuPool m_uploadCmdPool;
    GpuPool m_codePool;
    GpuPool m_dataPool;
    GpuPool m_imagePool;
    GpuPool m_stagingPool;

    dk::Image  m_fbImages[NUM_FB];
    dk::Image  m_dsImage;
    dk::Image  m_offImages[NUM_OFFSCREEN];
    dk::UniqueMemBlock m_readStaging;   // readFramebufferPixels() target
    dk::UniqueMemBlock m_readImageMem;  // uncompressed full-res blit target
    dk::Image          m_readImage;
    bool               m_readImageReady = false;
    dk::UniqueMemBlock m_readCmdMem;    // beginFramebufferReadback()'s own commands
    dk::UniqueCmdBuf   m_readCmdbuf;
    dk::Fence          m_readFence{};
    bool               m_readInFlight = false;
    GpuPool    m_offPool;
    bool       m_offscreenReady = false;

    // Per-slot fences: signalled in endFrame, waited in beginFrame, so that
    // the CPU never overwrites command/vertex memory the GPU is still reading.
    dk::Fence  m_frameFences[NUM_FB];
    dk::Fence  m_uploadFence;        // a batch's (or a capture's): it uses all the upload memory
    // Single uploads take turns through these: each has its own fence, a
    // part of the command memory and of the staging pool (a big picture gets
    // its own block). One waits only when its slot's copy, 16 uploads back, is
    // still running -- not for the frame being drawn, as one shared fence did.
    static constexpr int kUploadSlots = 16;
    struct UploadSlot {
        dk::Fence fence;
        dk::UniqueMemBlock staging;
        bool busy = false;
    };
    UploadSlot m_uploadSlots[kUploadSlots];
    int        m_uploadNext = 0;
    WaitStats  m_waits;
    bool       m_uploadBatchActive = false;
    bool       m_uploadInFlight = false;
    unsigned   m_uploadBatchCount = 0;

    uint32_t m_vtxOff[NUM_FB] {};
    uint32_t m_idxOff[NUM_FB] {};
    uint32_t m_vsUboOff[NUM_FB] {};
    uint32_t m_fsUboOff[NUM_FB] {};
    uint32_t m_fsUboRingPos[NUM_FB] {};
    uint32_t m_imgDescOff = 0;
    uint32_t m_samDescOff = 0;

    uint64_t m_imageMemUsed = 0;
    uint64_t m_poolMemUsed  = 0;
    static inline LogSink s_logSink = nullptr;
    static inline thread_local int s_essential = 0;
    static inline uint64_t s_imageBudget = kDefaultImageBudget;
    uint64_t imageReserve() const { return s_essential ? 0 : kEssentialReserve; }

    static constexpr uint32_t kImageChunkSize = 2u * 1024u * 1024u;
    struct ImagePoolChunk {
        dk::UniqueMemBlock block;
        uint32_t size = 0;
        uint32_t used = 0;
    };
    std::vector<ImagePoolChunk> m_imageChunks;
#else
    SDL_Window*   m_sdlWindow   = nullptr;
    SDL_Renderer* m_sdlRenderer = nullptr;
#endif
    int m_slot = -1;
    int m_fbW = FB_WIDTH, m_fbH = FB_HEIGHT;
    static inline int s_fbW = FB_WIDTH, s_fbH = FB_HEIGHT;
    static inline void (*s_firstFrameHook)() = nullptr;
};

} // namespace nxui
