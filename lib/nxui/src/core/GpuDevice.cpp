#include <nxui/core/GpuDevice.hpp>
#include <cstdio>
#include <cstring>
#include <array>
#include <cstdarg>

namespace nxui {

void GpuDevice::logGpu(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (s_logSink) s_logSink(buf);
    else std::fprintf(stderr, "%s\n", buf);
}

static void dkDebugCallback(void* userData, const char* context, DkResult result,
                            const char* message) {
    (void)userData;
    GpuDevice::logGpu("[GpuDevice] deko3d ctx=%s result=%d msg=%s",
                      context ? context : "?", (int)result, message ? message : "");
}

bool GpuDevice::initialize() {
    m_dev   = dk::DeviceMaker{}.setCbDebug(dkDebugCallback).create();
    m_queue = dk::QueueMaker{m_dev}.setFlags(DkQueueFlags_Graphics).create();

    for (int i = 0; i < NUM_FB; ++i) {
        m_cmdPool[i].create(m_dev, CMD_BUF_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        m_cmdbuf[i] = dk::CmdBufMaker{m_dev}.create();
        m_cmdbuf[i].addMemory(m_cmdPool[i].block, 0, CMD_BUF_SIZE);
    }

    m_uploadCmdPool.create(m_dev, 64 * 1024, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    m_uploadCmdbuf = dk::CmdBufMaker{m_dev}.create();
    m_uploadCmdbuf.addMemory(m_uploadCmdPool.block, 0, 64 * 1024);

    m_codePool.create(m_dev, CODE_POOL_SIZE,
        DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);

    uint32_t dataSize = 0;
    for (int i = 0; i < NUM_FB; ++i) {
        dataSize += VTX_BUF_SIZE + 256;
        dataSize += IDX_BUF_SIZE + 256;
        dataSize += VS_UBO_SIZE + 256;
        dataSize += FS_UBO_SIZE * FS_UBO_RING + 256;
    }
    dataSize += MAX_TEXTURES * sizeof(DkImageDescriptor) + DK_IMAGE_DESCRIPTOR_ALIGNMENT;
    dataSize += MAX_SAMPLERS * sizeof(DkSamplerDescriptor) + DK_SAMPLER_DESCRIPTOR_ALIGNMENT;
    dataSize = (dataSize + kGpuAlign - 1) & ~(kGpuAlign - 1);
    m_dataPool.create(m_dev, dataSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);

    for (int i = 0; i < NUM_FB; ++i) {
        m_vtxOff[i]   = m_dataPool.alloc(VTX_BUF_SIZE, 256);
        m_idxOff[i]   = m_dataPool.alloc(IDX_BUF_SIZE, 256);
        m_vsUboOff[i] = m_dataPool.alloc(VS_UBO_SIZE, DK_UNIFORM_BUF_ALIGNMENT);
        m_fsUboOff[i] = m_dataPool.alloc(FS_UBO_SIZE * FS_UBO_RING, DK_UNIFORM_BUF_ALIGNMENT);
    }
    m_imgDescOff = m_dataPool.alloc(MAX_TEXTURES * sizeof(DkImageDescriptor), DK_IMAGE_DESCRIPTOR_ALIGNMENT);
    m_samDescOff = m_dataPool.alloc(MAX_SAMPLERS * sizeof(DkSamplerDescriptor), DK_SAMPLER_DESCRIPTOR_ALIGNMENT);

    m_stagingPool.create(m_dev, kUploadSlots * 256 * 1024, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);

    createFramebuffers();
    createDepthStencil();
    createOffscreenTargets();

    std::array<DkImage const*, NUM_FB> fbArray;
    for (int i = 0; i < NUM_FB; ++i) fbArray[i] = &m_fbImages[i];
    m_swapchain = dk::SwapchainMaker{m_dev, nwindowGetDefault(), fbArray}.create();

    return true;
}

void GpuDevice::createFramebuffers() {
    dk::ImageLayout fbLayout;
    dk::ImageLayoutMaker{m_dev}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent |
                  DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(FB_WIDTH, FB_HEIGHT)
        .initialize(fbLayout);

    uint64_t fbSize  = fbLayout.getSize();
    uint64_t fbAlign = fbLayout.getAlignment();
    uint32_t totalFb = 0;
    for (int i = 0; i < NUM_FB; ++i) {
        totalFb = (totalFb + fbAlign - 1) & ~(fbAlign - 1);
        totalFb += fbSize;
    }
    m_fbPool.create(m_dev, totalFb, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);

    uint32_t off = 0;
    for (int i = 0; i < NUM_FB; ++i) {
        off = (off + fbAlign - 1) & ~(fbAlign - 1);
        m_fbImages[i].initialize(fbLayout, m_fbPool.block, off);
        off += fbSize;
    }
}

void GpuDevice::createDepthStencil() {
    dk::ImageLayout dsLayout;
    dk::ImageLayoutMaker{m_dev}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_S8)
        .setDimensions(FB_WIDTH, FB_HEIGHT)
        .initialize(dsLayout);

    uint32_t dsSize = dsLayout.getSize();
    dsSize = (dsSize + kGpuAlign - 1) & ~(kGpuAlign - 1);
    m_dsPool.create(m_dev, dsSize, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    m_dsImage.initialize(dsLayout, m_dsPool.block, 0);
}

void GpuDevice::createOffscreenTargets() {
    // Half-resolution offscreen targets for blur (640x360)
    constexpr uint32_t offW = FB_WIDTH / 2;
    constexpr uint32_t offH = FB_HEIGHT / 2;

    dk::ImageLayout offLayout;
    dk::ImageLayoutMaker{m_dev}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(offW, offH)
        .initialize(offLayout);

    uint64_t offSize  = offLayout.getSize();
    uint64_t offAlign = offLayout.getAlignment();
    uint32_t totalOff = 0;
    for (int i = 0; i < NUM_OFFSCREEN; ++i) {
        totalOff = (totalOff + offAlign - 1) & ~(offAlign - 1);
        totalOff += offSize;
    }
    m_offPool.create(m_dev, totalOff, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);

    uint32_t off = 0;
    for (int i = 0; i < NUM_OFFSCREEN; ++i) {
        off = (off + offAlign - 1) & ~(offAlign - 1);
        m_offImages[i].initialize(offLayout, m_offPool.block, off);
        off += offSize;
    }
    m_offscreenReady = true;
}

int GpuDevice::beginFrame() {
    const u64 start = armGetSystemTick();
    m_slot = m_queue.acquireImage(m_swapchain);

    // Wait for the GPU to finish the PREVIOUS frame that used this slot's
    // command memory / vertex buffer before we overwrite them.
    m_frameFences[m_slot].wait();
    m_waits.frameMs += armTicksToNs(armGetSystemTick() - start) / 1e6f;

    // Reset command buffer and re-feed its memory (clear invalidates memory
    // tracking — following the deko3d sample framework pattern).
    m_cmdbuf[m_slot].clear();
    m_cmdbuf[m_slot].addMemory(m_cmdPool[m_slot].block, 0, CMD_BUF_SIZE);
    return m_slot;
}

void GpuDevice::endFrame() {
    // Signal the fence for this slot so the NEXT time beginFrame()
    // acquires the same slot, it can wait for completion.
    m_cmdbuf[m_slot].signalFence(m_frameFences[m_slot]);

    auto cmdList = m_cmdbuf[m_slot].finishList();
    m_queue.submitCommands(cmdList);
    m_queue.presentImage(m_swapchain, m_slot);
}

bool GpuDevice::downloadFramebufferRgba(std::vector<uint8_t>& outRgba,
                                        int& outW, int& outH,
                                        bool halfRes) {
    dk::UniqueMemBlock staging;
    if (!copyFramebuffer(staging, outW, outH, halfRes))
        return false;
    const size_t byteSize = static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4u;
    outRgba.resize(byteSize);
    std::memcpy(outRgba.data(), staging.getCpuAddr(), byteSize);
    return true;
}

bool GpuDevice::prepareFramebufferReadback() {
    if (!m_dev) return false;
    if (!m_readStaging) {
        const uint32_t bytes = static_cast<uint32_t>(FB_WIDTH) * static_cast<uint32_t>(FB_HEIGHT) * 4u;
        // CPU-cached: the 3.6 MB is read back by the CPU (and written to the SD
        // card) after every capture. Read from uncached memory that took ~5 s,
        // and launching a game or a sidebar app waited for it.
        m_readStaging = dk::MemBlockMaker{m_dev, (bytes + kGpuAlign - 1) & ~(kGpuAlign - 1)}
            .setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached)
            .create();
    }
    if (!m_readImageReady) {
        dk::ImageLayout layout;
        dk::ImageLayoutMaker{m_dev}
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(static_cast<uint32_t>(FB_WIDTH), static_cast<uint32_t>(FB_HEIGHT))
            .initialize(layout);
        const uint32_t imgSize =
            (static_cast<uint32_t>(layout.getSize()) + kGpuAlign - 1) & ~(kGpuAlign - 1);
        m_readImageMem = dk::MemBlockMaker{m_dev, imgSize}
            .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
            .create();
        if (m_readImageMem) {
            m_readImage.initialize(layout, m_readImageMem, 0);
            m_readImageReady = true;
        }
    }
    return m_readStaging && m_readImageReady;
}

const uint8_t* GpuDevice::readFramebufferPixels(int& outW, int& outH) {
    if (!prepareFramebufferReadback() || !copyFramebuffer(m_readStaging, outW, outH, false))
        return nullptr;
    // The GPU wrote behind the CPU's cache: drop any stale lines before reading.
    armDCacheFlush(m_readStaging.getCpuAddr(),
                   static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4u);
    return static_cast<const uint8_t*>(m_readStaging.getCpuAddr());
}

bool GpuDevice::beginFramebufferReadback() {
    if (m_slot < 0 || !m_queue || !prepareFramebufferReadback())
        return false;
    if (m_readInFlight) { // the previous one (long done in practice)
        m_readFence.wait();
        m_readInFlight = false;
    }
    if (!m_readCmdbuf) {
        // Its own command memory: the shared upload buffer is rewritten by the
        // next texture upload while this copy may still be queued.
        m_readCmdMem = dk::MemBlockMaker{m_dev, 0x4000}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
        if (!m_readCmdMem)
            return false;
        m_readCmdbuf = dk::CmdBufMaker{m_dev}.create();
    }
    m_readCmdbuf.clear();
    m_readCmdbuf.addMemory(m_readCmdMem, 0, 0x4000);

    // Queued after the frame that was just presented and before the next one,
    // so it copies exactly what is on screen. Same steps as copyFramebuffer().
    const DkImageRect rect{0, 0, 0, static_cast<uint32_t>(FB_WIDTH), static_cast<uint32_t>(FB_HEIGHT), 1};
    dk::ImageView srcView{m_fbImages[m_slot]};
    dk::ImageView dstView{m_readImage};
    m_readCmdbuf.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    m_readCmdbuf.blitImage(srcView, rect, dstView, rect, 0);
    m_readCmdbuf.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    m_readCmdbuf.copyImageToBuffer(dstView, rect, DkCopyBuf{m_readStaging.getGpuAddr(), 0, 0});
    const uint32_t threedNop = 0x80000040u;
    m_readCmdbuf.replayCmds({threedNop});
    m_readCmdbuf.barrier(DkBarrier_None, DkInvalidateFlags_L2Cache);
    m_readCmdbuf.signalFence(m_readFence, true);
    m_queue.submitCommands(m_readCmdbuf.finishList());
    m_queue.flush();
    m_readInFlight = true;
    return true;
}

const uint8_t* GpuDevice::finishFramebufferReadback(int& outW, int& outH) {
    if (!m_readInFlight)
        return nullptr;
    m_readFence.wait();
    m_readInFlight = false;
    outW = FB_WIDTH;
    outH = FB_HEIGHT;
    armDCacheFlush(m_readStaging.getCpuAddr(), static_cast<size_t>(FB_WIDTH) * FB_HEIGHT * 4u);
    return static_cast<const uint8_t*>(m_readStaging.getCpuAddr());
}

bool GpuDevice::copyFramebuffer(dk::UniqueMemBlock& staging, int& outW, int& outH,
                                bool halfRes) {
    if (m_slot < 0 || !m_queue)
        return false;

    waitForTextureUploads();
    waitIdle();

    const int fullW = FB_WIDTH;
    const int fullH = FB_HEIGHT;
    const bool useHalf = halfRes && m_offscreenReady;
    outW = useHalf ? fullW / 2 : fullW;
    outH = useHalf ? fullH / 2 : fullH;
    if (outW <= 0 || outH <= 0)
        return false;

    const uint32_t byteSize = static_cast<uint32_t>(outW) * static_cast<uint32_t>(outH) * 4u;
    const uint32_t stagingSize = (byteSize + kGpuAlign - 1) & ~(kGpuAlign - 1);
    // MemBlockMaker::create() returns a plain handle; only UniqueMemBlock frees it.
    // A caller's block that's already there (readFramebufferPixels) is reused.
    if (!staging)
        staging = dk::MemBlockMaker{m_dev, stagingSize}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
    if (!staging || !staging.getCpuAddr())
        return false;

    // Full-res path needs an uncompressed blit target: swapchain images use
    // HwCompression, and copyImageToBuffer from those is unreliable / soft.
    dk::Image tempImage;
    dk::UniqueMemBlock tempImageMem;
    // Prepared once and reused (no per-capture allocation) when available.
    dk::Image& blitTarget = (!useHalf && m_readImageReady) ? m_readImage : tempImage;
    if (!useHalf && !m_readImageReady) {
        dk::ImageLayout layout;
        dk::ImageLayoutMaker{m_dev}
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(static_cast<uint32_t>(fullW), static_cast<uint32_t>(fullH))
            .initialize(layout);
        const uint32_t imgSize =
            (static_cast<uint32_t>(layout.getSize()) + kGpuAlign - 1) & ~(kGpuAlign - 1);
        tempImageMem = dk::MemBlockMaker{m_dev, imgSize}
            .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
            .create();
        if (!tempImageMem)
            return false;
        tempImage.initialize(layout, tempImageMem, 0);
    }

    m_uploadCmdbuf.clear();
    m_uploadCmdbuf.addMemory(m_uploadCmdPool.block, 0, 64 * 1024);

    dk::ImageView srcView{m_fbImages[m_slot]};
    m_uploadCmdbuf.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    if (useHalf) {
        dk::ImageView dstView{m_offImages[0]};
        const DkImageRect srcRect{0, 0, 0,
                                  static_cast<uint32_t>(fullW),
                                  static_cast<uint32_t>(fullH), 1};
        const DkImageRect dstRect{0, 0, 0,
                                  static_cast<uint32_t>(outW),
                                  static_cast<uint32_t>(outH), 1};
        m_uploadCmdbuf.blitImage(srcView, srcRect, dstView, dstRect, 0);
        m_uploadCmdbuf.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
        srcView = dk::ImageView{m_offImages[0]};
    } else {
        dk::ImageView dstView{blitTarget};
        const DkImageRect rect{0, 0, 0,
                               static_cast<uint32_t>(fullW),
                               static_cast<uint32_t>(fullH), 1};
        m_uploadCmdbuf.blitImage(srcView, rect, dstView, rect, 0);
        m_uploadCmdbuf.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
        srcView = dstView;
    }

    const DkImageRect copyRect{0, 0, 0,
                               static_cast<uint32_t>(outW),
                               static_cast<uint32_t>(outH), 1};
    m_uploadCmdbuf.copyImageToBuffer(
        srcView, copyRect,
        DkCopyBuf{staging.getGpuAddr(), 0, 0});

    // Force a 3D-engine sync so the copy finishes before CPU reads.
    // See deko3d notes on copyImageToBuffer + L2 invalidation.
    const uint32_t threedNop = 0x80000040u;
    m_uploadCmdbuf.replayCmds({threedNop});
    m_uploadCmdbuf.barrier(DkBarrier_None, DkInvalidateFlags_L2Cache);

    m_queue.submitCommands(m_uploadCmdbuf.finishList());
    m_queue.waitIdle();
    (void)byteSize;
    return true;
}

void GpuDevice::waitIdle() {
    if (m_queue) m_queue.waitIdle();
}

dk::UniqueMemBlock GpuDevice::allocImageMemory(uint32_t size) {
    size = (size + kGpuAlign - 1) & ~(kGpuAlign - 1);
    if (m_imageMemUsed + size + imageReserve() > s_imageBudget) {
        GpuDevice::logGpu( "[GpuDevice] image budget exceeded (%llu + %u > %llu), skipping\n",
                     (unsigned long long)m_imageMemUsed, size,
                     (unsigned long long)s_imageBudget);
        return {};  // return empty MemBlock — caller should check validity
    }
    u64 total = 0;
    u64 used = 0;
    if (R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) &&
        R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)) &&
        total > used) {
        // Keep a small process free-memory cushion so concurrent sysmodules
        // (Frame Tap, Ultrahand, etc.) do not push the menu into soft-locks.
        // 2 MiB is intentionally lower than the prior 24 MiB reserve so more
        // artwork can still load while refusing before total exhaustion.
        constexpr u64 kAllocationHeadroom = 2ull * 1024ull * 1024ull;
        const u64 freeMemory = total - used;
        if (freeMemory < static_cast<u64>(size) + kAllocationHeadroom) {
            GpuDevice::logGpu(
                "[GpuDevice] refusing %u image bytes: free=%llu used=%llu total=%llu headroom=%llu\n",
                size,
                static_cast<unsigned long long>(freeMemory),
                static_cast<unsigned long long>(used),
                static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(kAllocationHeadroom));
            return {};
        }
    }
    auto blk = dk::MemBlockMaker{m_dev, size}
        .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();
    if (!blk) {
        GpuDevice::logGpu("[GpuDevice] image allocation failed (%u bytes)\n", size);
        return {};
    }
    m_imageMemUsed += size;
    return blk;
}

void GpuDevice::freeImageMemory(uint32_t size) {
    // A Texture owns a standalone dk::MemBlock which is released immediately
    // after this bookkeeping hook returns. Waiting only for the upload queue
    // is insufficient: an already submitted graphics frame may still sample
    // that image, producing a GPU read page fault when edit-mode destroys its
    // temporary ghost texture. Drain both upload work and graphics work before
    // allowing the MemBlock destructor to run.
    waitForTextureUploads();
    waitIdle();
    size = (size + kGpuAlign - 1) & ~(kGpuAlign - 1);
    if (size <= m_imageMemUsed)
        m_imageMemUsed -= size;
    else
        m_imageMemUsed = 0;
}

void GpuDevice::forgetImageMemory(uint32_t size) {
    size = (size + kGpuAlign - 1) & ~(kGpuAlign - 1);
    m_imageMemUsed = size <= m_imageMemUsed ? m_imageMemUsed - size : 0;
}

GpuDevice::ImageAlloc GpuDevice::allocImageFromPool(uint32_t size, uint32_t alignment) {
    if (alignment < kGpuAlign) alignment = kGpuAlign;
    size = (size + kGpuAlign - 1) & ~(kGpuAlign - 1);
    if (m_imageMemUsed + size + imageReserve() > s_imageBudget) {
        GpuDevice::logGpu( "[GpuDevice] pool budget exceeded (%llu + %u > %llu)\n",
                     (unsigned long long)m_imageMemUsed, size,
                     (unsigned long long)s_imageBudget);
        return {};
    }

    u64 total = 0, used = 0;
    if (R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) &&
        R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)) &&
        total > used) {
        constexpr u64 kAllocationHeadroom = 2ull * 1024ull * 1024ull;
        const u64 freeMemory = total - used;
        if (freeMemory < static_cast<u64>(size) + kAllocationHeadroom) {
            GpuDevice::logGpu(
                "[GpuDevice] pool refusing %u bytes: free=%llu used=%llu total=%llu headroom=%llu\n",
                size,
                static_cast<unsigned long long>(freeMemory),
                static_cast<unsigned long long>(used),
                static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(kAllocationHeadroom));
            return {};
        }
    }

    // Try to fit in an existing chunk
    for (auto& chunk : m_imageChunks) {
        uint32_t aligned = (chunk.used + alignment - 1) & ~(alignment - 1);
        if (aligned + size <= chunk.size) {
            chunk.used = aligned + size;
            m_imageMemUsed += size;
            m_poolMemUsed  += size;
            return {chunk.block, aligned};
        }
    }
    // Allocate a new chunk
    uint32_t chunkSize = std::max(kImageChunkSize, size);
    chunkSize = (chunkSize + kGpuAlign - 1) & ~(kGpuAlign - 1);
    auto blk = dk::MemBlockMaker{m_dev, chunkSize}
        .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();
    if (!blk) {
        GpuDevice::logGpu("[GpuDevice] pooled image allocation failed (%u bytes)\n",
                          chunkSize);
        return {};
    }
    m_imageChunks.push_back({std::move(blk), chunkSize, size});
    m_imageMemUsed += size;
    m_poolMemUsed  += size;
    return {m_imageChunks.back().block, 0};
}

void GpuDevice::resetImagePool() {
    m_imageChunks.clear();
    // Only subtract pool memory; individual allocations remain tracked.
    if (m_poolMemUsed <= m_imageMemUsed)
        m_imageMemUsed -= m_poolMemUsed;
    else
        m_imageMemUsed = 0;
    m_poolMemUsed = 0;
}

bool GpuDevice::uploadTexture(dk::Image& dst, const void* pixels, uint32_t size,
                              uint32_t w, uint32_t h)
{
    // deko3d does not report a bad upload, it calls svcBreak: a crash report
    // resolving to dk::detail::RaiseError under ImageLayout::calcLevelOffset is
    // what that looks like, and one arrived from a 1TB card where the menu had
    // uploaded a great many icons. Nothing here checked the arguments first, so
    // a zero-sized or mismatched upload took the process with it instead of
    // failing. These are the cases that can be caught without asking deko3d.
    if (!pixels || size == 0 || w == 0 || h == 0) {
        std::fprintf(stderr,
                     "[GpuDevice] refusing texture upload %ux%u size=%u pixels=%p\n",
                     w, h, size, pixels);
        return false;
    }
    if (size < (uint64_t)w * h * 4) {
        std::fprintf(stderr,
                     "[GpuDevice] refusing texture upload %ux%u: %u bytes is short of %llu\n",
                     w, h, size, (unsigned long long)((uint64_t)w * h * 4));
        return false;
    }

    if (m_uploadBatchActive) {
        const uint32_t stagingOffset = m_stagingPool.alloc(size, 256);
        if (stagingOffset == UINT32_MAX) {
            GpuDevice::logGpu( "[GpuDevice] upload batch staging budget exceeded (%u bytes)\n", size);
            return false;
        }

        std::memcpy(m_stagingPool.cpuAddr(stagingOffset), pixels, size);
        dk::ImageView view{dst};
        m_uploadCmdbuf.copyBufferToImage(
            {m_stagingPool.gpuAddr(stagingOffset), 0, 0},
            view,
            {0, 0, 0, w, h, 1});
        ++m_uploadBatchCount;
        return true;
    }

    // A batch or a capture used all of the upload memory: it finishes first.
    if (m_uploadInFlight) {
        m_uploadFence.wait();
        m_uploadInFlight = false;
    }
    // This upload's slot. Its last copy ran long ago, unless 16 came at once.
    const int index = m_uploadNext;
    m_uploadNext = (m_uploadNext + 1) % kUploadSlots;
    UploadSlot& slot = m_uploadSlots[index];
    if (slot.busy) {
        const u64 start = armGetSystemTick();
        slot.fence.wait();
        m_waits.uploadMs += armTicksToNs(armGetSystemTick() - start) / 1e6f;
        slot.busy = false;
    }
    slot.staging = {};

    const void* srcCpu = nullptr;
    DkGpuAddr srcGpu = 0;
    const uint32_t slotStaging = m_stagingPool.size / kUploadSlots;

    if (size <= slotStaging) {
        srcCpu = static_cast<uint8_t*>(m_stagingPool.cpuBase) + index * slotStaging;
        srcGpu = m_stagingPool.block.getGpuAddr() + index * slotStaging;
    } else {
        uint32_t allocSize = (size + kGpuAlign - 1) & ~(kGpuAlign - 1);
        slot.staging = dk::MemBlockMaker{m_dev, allocSize}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();

        srcCpu = slot.staging.getCpuAddr();
        srcGpu = slot.staging.getGpuAddr();

        if (!srcCpu || !srcGpu) {
            GpuDevice::logGpu( "[GpuDevice] Failed temp staging allocation for texture (%u bytes)\n", size);
            return false;
        }
    }

    std::memcpy(const_cast<void*>(srcCpu), pixels, size);

    // Use dedicated upload command buffer, in this slot's part of its memory
    // (the copies before may still be waiting to run from theirs).
    constexpr uint32_t slotCmd = 64 * 1024 / kUploadSlots;
    m_uploadCmdbuf.clear();
    m_uploadCmdbuf.addMemory(m_uploadCmdPool.block, index * slotCmd, slotCmd);

    dk::ImageView view{dst};
    m_uploadCmdbuf.copyBufferToImage(
        {srcGpu, 0, 0},
        view,
        {0, 0, 0, w, h, 1}
    );
    // Drawing that follows on this queue sees the new pixels.
    m_uploadCmdbuf.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    m_uploadCmdbuf.signalFence(slot.fence);

    // No waitIdle: that waited for every frame in flight as well, once per
    // picture, and the screen stuttered while pictures arrived (scrolling a
    // row of game cards). The queue runs the copy before anything drawn after it.
    // flush() hands it to the GPU now: a fence on work still sitting in the
    // queue never signals, and the next upload's wait hung HOME at start.
    m_queue.submitCommands(m_uploadCmdbuf.finishList());
    m_queue.flush();
    slot.busy = true;
    return true;
}

void GpuDevice::beginTextureUploadBatch() {
    if (m_uploadBatchActive)
        return;
    waitForTextureUploads();

    m_stagingPool.used = 0;
    m_uploadCmdbuf.clear();
    m_uploadCmdbuf.addMemory(m_uploadCmdPool.block, 0, 64 * 1024);
    m_uploadBatchCount = 0;
    m_uploadBatchActive = true;
}

void GpuDevice::waitForTextureUploads() {
    for (UploadSlot& slot : m_uploadSlots)
        if (slot.busy) {
            slot.fence.wait();
            slot.busy = false;
            slot.staging = {};
        }
    if (!m_uploadInFlight)
        return;
    m_uploadFence.wait();
    m_uploadInFlight = false;
}

void GpuDevice::endTextureUploadBatch() {
    if (!m_uploadBatchActive)
        return;
    m_uploadBatchActive = false;
    if (m_uploadBatchCount == 0)
        return;

    m_uploadCmdbuf.signalFence(m_uploadFence);
    m_queue.submitCommands(m_uploadCmdbuf.finishList());
    m_queue.flush(); // see uploadTexture: the fence only signals once the GPU has the work
    m_uploadInFlight = true;
    m_uploadBatchCount = 0;
}

void GpuDevice::shutdown() {
    if (m_uploadBatchActive)
        endTextureUploadBatch();
    if (m_queue) m_queue.waitIdle();
    m_uploadInFlight = false;
    for (UploadSlot& slot : m_uploadSlots) {
        slot.busy = false;
        slot.staging = {};
    }
    m_swapchain    = {};
    for (int i = 0; i < NUM_FB; ++i)
        m_cmdbuf[i] = {};
    m_uploadCmdbuf = {};
    m_queue        = {};
    m_imageChunks.clear();
    m_imageMemUsed = 0;
    m_poolMemUsed  = 0;
    m_fbPool      = {};
    m_dsPool      = {};
    m_offPool     = {};
    m_readStaging  = nullptr;
    m_readImageMem = nullptr;
    m_readImageReady = false;
    m_offscreenReady = false;
    for (int i = 0; i < NUM_FB; ++i)
        m_cmdPool[i] = {};
    m_uploadCmdPool = {};
    m_codePool    = {};
    m_dataPool  = {};
    m_imagePool = {};
    m_stagingPool = {};
    m_dev       = {};
}

} // namespace nxui
