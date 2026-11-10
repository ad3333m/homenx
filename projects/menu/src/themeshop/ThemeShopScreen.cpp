#include "ThemeShopScreen.hpp"

#include "ThemeHttp.hpp"

#include "core/DebugLog.hpp"

#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/ImageDecode.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/ThreadPool.hpp>

#include <curlpp/Easy.hpp>
#include <curlpp/Infos.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/cURLpp.hpp>
#include <switch.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <unordered_set>

namespace {

constexpr int kCommunityPreviewGridCols = 2;
constexpr int kCommunityPreviewVisibleRows = 2;
constexpr int kCommunityPreviewEntriesPerPage = kCommunityPreviewGridCols * kCommunityPreviewVisibleRows;
constexpr int kMaxPreviewUploadsPerFrame = 1;
constexpr int kMaxPreviewDownloadsInFlight = kCommunityPreviewEntriesPerPage;
constexpr int kCommunityPreviewMaxSide = 960;
// The installed grid's cards show their preview at most ~460 px wide (two
// columns beside the tabs; logical pixels, times gpu.scale() when loaded),
// the detail and full-screen views up to 960 px.
constexpr int kInstalledGridPreviewMaxSide = 480;

std::string installedPreviewKey(const std::string& previewPath, bool detail) {
    return detail ? previewPath + "|960" : previewPath;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string trimLeadingSlashes(std::string value) {
    while (!value.empty() && value.front() == '/')
        value.erase(value.begin());
    return value;
}

std::string trimWhitespace(std::string value) {
    auto isSpace = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };

    while (!value.empty() && isSpace((unsigned char)value.front()))
        value.erase(value.begin());
    while (!value.empty() && isSpace((unsigned char)value.back()))
        value.pop_back();
    return value;
}

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return (char)std::tolower(ch);
    });
    return value;
}

bool containsInsensitive(const std::string& haystack, const std::string& needleLower) {
    if (needleLower.empty())
        return true;
    return asciiLower(haystack).find(needleLower) != std::string::npos;
}

bool matchesSearch(const ThemeShopScreen::ThemeShopEntry& entry, const std::string& needleLower) {
    return needleLower.empty()
        || containsInsensitive(entry.name, needleLower)
        || containsInsensitive(entry.author, needleLower)
        || containsInsensitive(entry.id, needleLower)
        || containsInsensitive(entry.version, needleLower)
        || containsInsensitive(entry.source, needleLower)
        || containsInsensitive(entry.soundPreset, needleLower);
}

bool matchesSearch(const ThemeCatalogClient::Entry& entry, const std::string& needleLower) {
    return needleLower.empty()
        || containsInsensitive(entry.name, needleLower)
        || containsInsensitive(entry.id, needleLower)
        || containsInsensitive(entry.version, needleLower)
        || containsInsensitive(entry.author, needleLower);
}

} // namespace

ThemeShopScreen::ThemeShopScreen()
    : TabbedOverlayScreen(ScreenMode::ThemeShop) {
}

void ThemeShopScreen::setThreadPool(nxui::ThreadPool* pool) {
    m_threadPool = pool;
}

void ThemeShopScreen::setRenderContext(nxui::GpuDevice* gpu, nxui::Renderer* renderer) {
    m_gpu = gpu;
    m_renderer = renderer;
}

void ThemeShopScreen::refreshCommunityCatalog() {
    if (!m_threadPool)
        return;

    m_catalogClient.refresh(*m_threadPool);
    syncCommunityCatalog(m_catalogClient.snapshot());
}

void ThemeShopScreen::setThemeShopState(const std::vector<ThemeShopEntry>& entries,
                                        const std::string& activeId) {
    m_allThemeShopEntries = entries;
    clearInstalledPreviewCache(); // and cancel the decodes still running
    applySearchFilter();

    auto hasId = [&](const std::string& id) {
        return std::any_of(m_themeShopEntries.begin(), m_themeShopEntries.end(),
                           [&](const ThemeShopEntry& entry) {
                               return entry.id == id;
                           });
    };

    if (m_themeShopEntries.empty()) {
        m_themeShopSelectedId.clear();
        return;
    }

    if (!hasId(m_themeShopSelectedId))
        m_themeShopSelectedId = activeId;
    if (!hasId(m_themeShopSelectedId))
        m_themeShopSelectedId = m_themeShopEntries.front().id;
}

void ThemeShopScreen::setPackageTransferState(const ThemeTransferState& state,
                                              std::string themeId,
                                              bool installMode) {
    m_packageTransferState = state;
    m_packageTransferThemeId = std::move(themeId);
    m_packageTransferInstallMode = installMode;

    if (m_packageTransferState.isRunning()) {
        clearCommunityPreviewCache();
        m_lastPreviewPrimeKey.clear();
    }
}

const ThemeShopScreen::ThemeShopEntry* ThemeShopScreen::selectedThemeShopEntry() const {
    if (m_themeShopEntries.empty())
        return nullptr;

    for (const auto& entry : m_themeShopEntries) {
        if (entry.id == m_themeShopSelectedId)
            return &entry;
    }

    return &m_themeShopEntries.front();
}

const ThemeCatalogClient::Entry* ThemeShopScreen::selectedCommunityThemeEntry() const {
    if (m_communityEntries.empty())
        return nullptr;

    for (const auto& entry : m_communityEntries) {
        if (entry.id == m_communitySelectedId)
            return &entry;
    }

    return &m_communityEntries.front();
}

const ThemeCatalogClient::Entry* ThemeShopScreen::findCommunityThemeEntry(const std::string& themeId) const {
    for (const auto& entry : m_allCommunityEntries) {
        if (entry.id == themeId)
            return &entry;
    }
    return nullptr;
}

int ThemeShopScreen::detailScreenshotCount() const {
    if (!isCommunityTab())
        return 0;

    const auto* entry = selectedCommunityThemeEntry();
    if (!entry)
        return 0;

    return (int)entry->screenshots.size();
}

std::string ThemeShopScreen::currentDetailCommunityPreviewPath() const {
    const auto* entry = selectedCommunityThemeEntry();
    if (!entry)
        return {};

    if (!entry->screenshots.empty()) {
        int index = std::clamp(m_detailScreenshotIndex, 0, (int)entry->screenshots.size() - 1);
        return entry->screenshots[(size_t)index];
    }

    return entry->cover;
}

void ThemeShopScreen::clampDetailScreenshotIndex() {
    int count = detailScreenshotCount();
    if (count <= 0) {
        m_detailScreenshotIndex = 0;
        m_detailFullscreen = false;
        if (m_detailFocusArea == DetailFocusArea::Preview)
            m_detailFocusArea = DetailFocusArea::Buttons;
        return;
    }

    m_detailScreenshotIndex = std::clamp(m_detailScreenshotIndex, 0, count - 1);
}

void ThemeShopScreen::stepDetailScreenshot(int delta) {
    int count = detailScreenshotCount();
    if (count <= 0)
        return;

    int nextIndex = std::clamp(m_detailScreenshotIndex + delta, 0, count - 1);
    if (nextIndex == m_detailScreenshotIndex)
        return;

    m_detailScreenshotIndex = nextIndex;
    primeCommunityPreview(currentDetailCommunityPreviewPath());
}

void ThemeShopScreen::applySearchFilter() {
    const std::string needleLower = asciiLower(trimWhitespace(m_searchQuery));

    m_themeShopEntries.clear();
    m_themeShopEntries.reserve(m_allThemeShopEntries.size());
    for (const auto& entry : m_allThemeShopEntries) {
        if (matchesSearch(entry, needleLower))
            m_themeShopEntries.push_back(entry);
    }

    m_communityEntries.clear();
    m_communityEntries.reserve(m_allCommunityEntries.size());
    for (const auto& entry : m_allCommunityEntries) {
        if (matchesSearch(entry, needleLower))
            m_communityEntries.push_back(entry);
    }

    auto syncSelection = [](const auto& entries, std::string& selectedId) {
        auto hasId = [&](const std::string& id) {
            return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
                return entry.id == id;
            });
        };

        if (entries.empty()) {
            selectedId.clear();
            return;
        }

        if (!hasId(selectedId))
            selectedId = entries.front().id;
    };

    syncSelection(m_themeShopEntries, m_themeShopSelectedId);
    syncSelection(m_communityEntries, m_communitySelectedId);

    if (m_themeShopEntries.empty())
        m_installedScrollRow = 0;
    if (m_communityEntries.empty())
        m_communityScrollRow = 0;
}

bool ThemeShopScreen::promptSearchQuery() {
    if (m_searchRequestCb) m_searchRequestCb(m_searchQuery);
    return true;
}

void ThemeShopScreen::setSearchQuery(std::string query) {
    m_searchQuery = trimWhitespace(query);
    closeDetail();
    applySearchFilter();
    ensureSelectionVisible();
}

bool ThemeShopScreen::pollCommunityCatalog() {
    if (!m_catalogClient.poll())
        return false;

    auto snapshot = m_catalogClient.snapshot();
    if (snapshot.revision == m_communityRevision)
        return false;

    syncCommunityCatalog(snapshot);
    return true;
}

std::string ThemeShopScreen::resolveCommunityPreviewUrl(const ThemeCatalogClient::Entry& entry) const {
    return resolveCommunityPreviewUrl(entry.cover);
}

std::string ThemeShopScreen::resolveCommunityPreviewUrl(const std::string& previewPath) const {
    if (previewPath.empty())
        return {};
    if (startsWith(previewPath, "https://") || startsWith(previewPath, "http://"))
        return previewPath;

    const std::string& catalogUrl = m_catalogClient.catalogUrl();
    std::size_t slash = catalogUrl.find_last_of('/');
    if (slash == std::string::npos)
        return previewPath;
    return catalogUrl.substr(0, slash + 1) + trimLeadingSlashes(previewPath);
}

std::shared_ptr<ThemeShopScreen::PreviewImageState> ThemeShopScreen::communityPreviewState(const std::string& previewPath) const {
    std::string url = resolveCommunityPreviewUrl(previewPath);
    if (url.empty())
        return nullptr;

    auto it = m_communityPreviewCache.find(url);
    if (it == m_communityPreviewCache.end())
        return nullptr;
    return it->second;
}

std::shared_ptr<ThemeShopScreen::PreviewImageState> ThemeShopScreen::communityPreviewState(const ThemeCatalogClient::Entry& entry) const {
    return communityPreviewState(entry.cover);
}

ThemeShopScreen::PreviewPhase ThemeShopScreen::communityPreviewPhase(const std::string& previewPath) const {
    auto state = communityPreviewState(previewPath);
    if (!state)
        return previewPath.empty() ? PreviewPhase::Failed : PreviewPhase::Idle;

    std::lock_guard<std::mutex> lk(state->mutex);
    return state->phase;
}

ThemeShopScreen::PreviewPhase ThemeShopScreen::communityPreviewPhase(const ThemeCatalogClient::Entry& entry) const {
    return communityPreviewPhase(entry.cover);
}

const nxui::Texture* ThemeShopScreen::communityPreviewTexture(const std::string& previewPath) const {
    auto state = communityPreviewState(previewPath);
    if (!state)
        return nullptr;

    std::lock_guard<std::mutex> lk(state->mutex);
    if (state->phase != PreviewPhase::Ready || !state->texture.valid())
        return nullptr;
    return &state->texture;
}

const nxui::Texture* ThemeShopScreen::communityPreviewTexture(const ThemeCatalogClient::Entry& entry) const {
    return communityPreviewTexture(entry.cover);
}

ThemeShopScreen::PreviewPhase ThemeShopScreen::installedPreviewPhase(const std::string& previewPath,
                                                                    bool detail) const {
    if (previewPath.empty())
        return PreviewPhase::Failed;

    auto it = m_installedPreviewCache.find(installedPreviewKey(previewPath, detail));
    if (it == m_installedPreviewCache.end() || !it->second)
        return PreviewPhase::Idle;

    std::lock_guard<std::mutex> lk(it->second->mutex);
    return it->second->phase;
}

const nxui::Texture* ThemeShopScreen::installedPreviewTexture(const std::string& previewPath,
                                                              bool detail) const {
    if (previewPath.empty())
        return nullptr;

    auto it = m_installedPreviewCache.find(installedPreviewKey(previewPath, detail));
    if (it == m_installedPreviewCache.end() || !it->second)
        return nullptr;

    std::lock_guard<std::mutex> lk(it->second->mutex);
    if (it->second->phase != PreviewPhase::Ready || !it->second->texture.valid())
        return nullptr;
    return &it->second->texture;
}

// Installed previews are decoded on the thread pool (a 1280x720 JPEG took the
// UI thread for a frame or more each) and uploaded one a frame by
// syncFinishedInstalledPreviewLoads. Nothing new starts while closing: the
// ones already there are drawn during the fade, then go (onFullyClosed).
void ThemeShopScreen::primeInstalledPreview(const std::string& previewPath, bool detail) {
    if (!m_gpu || !m_renderer || previewPath.empty() || !m_showing)
        return;

    // In real pixels: 720 px for the grid on a 1080p framebuffer (scale 1.5).
    const int maxSide = detail ? kCommunityPreviewMaxSide
        : std::min(kCommunityPreviewMaxSide,
                   static_cast<int>(std::ceil(kInstalledGridPreviewMaxSide * m_gpu->scale())));
    auto& slot = m_installedPreviewCache[installedPreviewKey(previewPath, detail)];
    if (!slot)
        slot = std::make_shared<PreviewImageState>();

    {
        std::lock_guard<std::mutex> lk(slot->mutex);
        if (slot->url != previewPath) {
            slot->url = previewPath;
            slot->phase = PreviewPhase::Idle;
            slot->failureCount = 0;
            slot->texture = nxui::Texture();
        }
        if (slot->phase != PreviewPhase::Idle)
            return;
        slot->phase = PreviewPhase::Loading;
    }

    if (m_threadPool) {
        m_hasPendingInstalledPreviewWork = true;
        slot->future = m_threadPool->submit([slot, previewPath, maxSide]() {
            int w = 0, h = 0;
            std::uint8_t* px = nxui::image::decodeFile(previewPath.c_str(), maxSide, w, h);
            std::lock_guard<std::mutex> lk(slot->mutex);
            if (slot->cancelled) {
                std::free(px);
                return;
            }
            if (!px) {
                slot->phase = PreviewPhase::Failed;
                slot->failureCount += 1;
                return;
            }
            std::free(slot->pixels);
            slot->pixels = px;
            slot->pixelW = w;
            slot->pixelH = h;
            slot->phase = PreviewPhase::Downloaded;
        });
        return;
    }

    nxui::Texture uploaded;
    bool ok = uploaded.loadFromFile(*m_gpu, *m_renderer, previewPath, maxSide);
    std::lock_guard<std::mutex> lk(slot->mutex);
    if (ok) {
        slot->texture = std::move(uploaded);
        slot->phase = PreviewPhase::Ready;
        slot->failureCount = 0;
    } else {
        slot->phase = PreviewPhase::Failed;
        slot->failureCount += 1;
    }
}

void ThemeShopScreen::syncFinishedInstalledPreviewLoads() {
    if (!m_gpu || !m_renderer || !m_hasPendingInstalledPreviewWork)
        return;

    bool hasPendingWork = false;
    int uploadsThisFrame = 0;
    for (auto& entry : m_installedPreviewCache) {
        auto state = entry.second;
        if (!state)
            continue;

        if (state->future.valid()) {
            if (state->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                    state->future.get();
                } catch (...) {
                    std::lock_guard<std::mutex> lk(state->mutex);
                    state->phase = PreviewPhase::Failed;
                    state->failureCount += 1;
                    DebugLog::log("[themeshop] installed preview task failed");
                }
            } else {
                hasPendingWork = true;
            }
        }

        std::uint8_t* px = nullptr;
        int w = 0, h = 0;
        {
            std::lock_guard<std::mutex> lk(state->mutex);
            if (state->phase == PreviewPhase::Downloaded) {
                if (uploadsThisFrame >= kMaxPreviewUploadsPerFrame) {
                    hasPendingWork = true;
                } else {
                    px = state->pixels;
                    w = state->pixelW;
                    h = state->pixelH;
                    state->pixels = nullptr;
                }
            }
        }
        if (!px)
            continue;

        ++uploadsThisFrame;
        nxui::Texture uploaded;
        const bool ok = uploaded.loadFromPixels(*m_gpu, *m_renderer, px, w, h);
        std::free(px);
        std::lock_guard<std::mutex> lk(state->mutex);
        if (ok) {
            state->texture = std::move(uploaded);
            state->failureCount = 0;
            state->phase = PreviewPhase::Ready;
        } else {
            state->failureCount += 1;
            state->phase = PreviewPhase::Failed;
        }
    }

    m_hasPendingInstalledPreviewWork = hasPendingWork;
}

void ThemeShopScreen::clearInstalledPreviewCache() {
    if (m_installedPreviewCache.empty())
        return;

    if (m_gpu)
        m_gpu->waitIdle(); // as clearCommunityPreviewCache: nothing drawn is freed under the GPU

    for (auto& entry : m_installedPreviewCache) {
        auto& state = entry.second;
        if (!state)
            continue;

        std::lock_guard<std::mutex> lk(state->mutex);
        state->cancelled = true; // a decode still running drops its pixels
        std::free(state->pixels);
        state->pixels = nullptr;
        state->texture = nxui::Texture();
    }

    m_installedPreviewCache.clear();
    m_hasPendingInstalledPreviewWork = false;
}

// Closed: the installed previews (one or two per theme looked at, up to 960 px)
// stayed until the theme list changed. They are read again on the next visit.
void ThemeShopScreen::onFullyClosed() {
    clearInstalledPreviewCache();
    clearCommunityPreviewCache();
}

void ThemeShopScreen::primeCommunityPreview(const std::string& previewPath) {
    if (!m_threadPool)
        return;

    constexpr int kMaxPreviewFailureCount = 2;

    std::string url = resolveCommunityPreviewUrl(previewPath);
    if (url.empty())
        return;

    auto& slot = m_communityPreviewCache[url];
    if (!slot)
        slot = std::make_shared<PreviewImageState>();

    bool canStart = false;
    {
        std::lock_guard<std::mutex> lk(slot->mutex);
        if (slot->url != url) {
            slot->phase = PreviewPhase::Idle;
            slot->failureCount = 0;
            slot->cancelled = false;
            slot->url = url;
            slot->bytes.clear();
            slot->future = std::future<void>();
            slot->texture = nxui::Texture();
        } else {
            slot->cancelled = false;
        }

        if (slot->phase == PreviewPhase::Idle
            || (slot->phase == PreviewPhase::Failed && slot->failureCount < kMaxPreviewFailureCount)) {
            canStart = true;
        }
    }

    if (!canStart)
        return;

    int inFlightDownloads = 0;
    for (const auto& entry : m_communityPreviewCache) {
        const auto& state = entry.second;
        if (!state)
            continue;

        std::lock_guard<std::mutex> lk(state->mutex);
        if (state->phase == PreviewPhase::Loading || state->phase == PreviewPhase::Downloaded)
            ++inFlightDownloads;
    }

    if (inFlightDownloads >= kMaxPreviewDownloadsInFlight)
        return;

    bool shouldStart = false;
    {
        std::lock_guard<std::mutex> lk(slot->mutex);
        if (slot->phase == PreviewPhase::Idle
            || (slot->phase == PreviewPhase::Failed && slot->failureCount < kMaxPreviewFailureCount)) {
            slot->phase = PreviewPhase::Loading;
            shouldStart = true;
        }
    }

    if (!shouldStart)
        return;

    m_hasPendingCommunityPreviewWork = true;
    slot->future = m_threadPool->submit([slot, url]() {
        try {
            {
                std::lock_guard<std::mutex> lk(slot->mutex);
                if (slot->cancelled)
                    return;
            }

            std::vector<std::uint8_t> bytes = themeshop::http::getBytes(url);
            std::lock_guard<std::mutex> lk(slot->mutex);
            if (slot->cancelled) {
                slot->bytes.clear();
                return;
            }
            slot->bytes = std::move(bytes);
            if (slot->bytes.empty()) {
                slot->failureCount += 1;
                slot->phase = PreviewPhase::Failed;
            } else {
                slot->failureCount = 0;
                slot->phase = PreviewPhase::Downloaded;
            }
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lk(slot->mutex);
            if (slot->cancelled)
                return;
            slot->bytes.clear();
            slot->failureCount += 1;
            slot->phase = PreviewPhase::Failed;
            DebugLog::log("[themeshop] preview fetch failed: %s", ex.what());
        } catch (...) {
            std::lock_guard<std::mutex> lk(slot->mutex);
            if (slot->cancelled)
                return;
            slot->bytes.clear();
            slot->failureCount += 1;
            slot->phase = PreviewPhase::Failed;
            DebugLog::log("[themeshop] preview fetch failed: unknown error");
        }
    });
}

void ThemeShopScreen::primeCommunityPreview(const ThemeCatalogClient::Entry& entry) {
    primeCommunityPreview(entry.cover);
}

void ThemeShopScreen::primeVisibleCommunityPreviews() {
    if (!isCommunityTab() || m_communityEntries.empty())
        return;

    std::unordered_set<std::string> queuedUrls;
    auto queuePreview = [&](const std::string& previewPath) {
        std::string url = resolveCommunityPreviewUrl(previewPath);
        if (url.empty() || !queuedUrls.emplace(url).second)
            return;
        primeCommunityPreview(previewPath);
    };

    const auto* selected = selectedCommunityThemeEntry();
    if (selected) {
        if (m_detailOpen) {
            std::string current = currentDetailCommunityPreviewPath();
            if (!current.empty())
                queuePreview(current);
        }

        if (!selected->cover.empty())
            queuePreview(selected->cover);
    }

    int scrollRow = m_communityScrollRow;
    int start = scrollRow * kCommunityPreviewGridCols;
    int end = std::min((int)m_communityEntries.size(), start + kCommunityPreviewEntriesPerPage);
    for (int i = start; i < end; ++i)
        queuePreview(m_communityEntries[(size_t)i].cover);
}

void ThemeShopScreen::syncFinishedCommunityPreviewLoads() {
    if (!m_gpu || !m_renderer || !m_hasPendingCommunityPreviewWork)
        return;

    bool hasPendingWork = false;
    int uploadsThisFrame = 0;
    for (auto& entry : m_communityPreviewCache) {
        auto state = entry.second;
        if (!state)
            continue;

        if (state->future.valid()) {
            if (state->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                    state->future.get();
                } catch (const std::exception& ex) {
                    std::lock_guard<std::mutex> lk(state->mutex);
                    state->phase = PreviewPhase::Failed;
                    state->failureCount += 1;
                    state->bytes.clear();
                    DebugLog::log("[themeshop] preview task failed: %s", ex.what());
                } catch (...) {
                    std::lock_guard<std::mutex> lk(state->mutex);
                    state->phase = PreviewPhase::Failed;
                    state->failureCount += 1;
                    state->bytes.clear();
                    DebugLog::log("[themeshop] preview task failed: unknown error");
                }
            } else {
                hasPendingWork = true;
            }
        }

        std::vector<std::uint8_t> bytes;
        {
            std::lock_guard<std::mutex> lk(state->mutex);
            if (state->phase == PreviewPhase::Downloaded) {
                if (uploadsThisFrame >= kMaxPreviewUploadsPerFrame) {
                    hasPendingWork = true;
                } else {
                    bytes = std::move(state->bytes);
                }
            }
        }

        if (bytes.empty())
            continue;

        ++uploadsThisFrame;
        nxui::Texture uploaded;
        bool ok = uploaded.loadFromMemory(*m_gpu, *m_renderer, bytes.data(), bytes.size(), kCommunityPreviewMaxSide);
        std::lock_guard<std::mutex> lk(state->mutex);
        state->bytes.clear();
        if (ok) {
            state->texture = std::move(uploaded);
            state->failureCount = 0;
            state->phase = PreviewPhase::Ready;
        } else {
            state->failureCount += 1;
            state->phase = PreviewPhase::Failed;
        }
    }

    m_hasPendingCommunityPreviewWork = hasPendingWork;
}

void ThemeShopScreen::clearCommunityPreviewCache() {
    if (m_communityPreviewCache.empty())
        return;

    if (m_gpu)
        m_gpu->waitIdle();

    for (auto& entry : m_communityPreviewCache) {
        auto& state = entry.second;
        if (!state)
            continue;

        std::lock_guard<std::mutex> lk(state->mutex);
        state->cancelled = true;
        state->bytes.clear();
        state->texture = nxui::Texture();
    }

    m_communityPreviewCache.clear();
    m_hasPendingCommunityPreviewWork = false;
}

void ThemeShopScreen::trimCommunityPreviewCache() {
    if (m_communityPreviewCache.empty())
        return;

    if (!isCommunityTab()) {
        clearCommunityPreviewCache();
        return;
    }

    std::unordered_set<std::string> retainedUrls;
    auto retainPreview = [&](const std::string& previewPath) {
        std::string url = resolveCommunityPreviewUrl(previewPath);
        if (!url.empty())
            retainedUrls.insert(std::move(url));
    };

    int start = m_communityScrollRow * kCommunityPreviewGridCols;
    int end = std::min((int)m_communityEntries.size(), start + kCommunityPreviewEntriesPerPage);
    for (int i = start; i < end; ++i)
        retainPreview(m_communityEntries[(size_t)i].cover);

    const auto* selected = selectedCommunityThemeEntry();
    if (selected) {
        retainPreview(selected->cover);
        if (m_detailOpen)
            retainPreview(currentDetailCommunityPreviewPath());
    }

    bool willErase = false;
    for (auto it = m_communityPreviewCache.begin(); it != m_communityPreviewCache.end();) {
        if (retainedUrls.find(it->first) == retainedUrls.end()) {
            willErase = true;
            break;
        }
        ++it;
    }

    if (willErase && m_gpu)
        m_gpu->waitIdle();

    int erased = 0;
    for (auto it = m_communityPreviewCache.begin(); it != m_communityPreviewCache.end();) {
        if (retainedUrls.find(it->first) == retainedUrls.end()) {
            if (it->second) {
                std::lock_guard<std::mutex> lk(it->second->mutex);
                it->second->cancelled = true;
                it->second->bytes.clear();
                it->second->texture = nxui::Texture();
            }
            it = m_communityPreviewCache.erase(it);
            ++erased;
        } else {
            ++it;
        }
    }

    if (erased > 0) {
        DebugLog::log("[themeshop] preview cache trimmed: erased=%d retained=%zu",
                      erased,
                      m_communityPreviewCache.size());
    }
}

void ThemeShopScreen::syncCommunityCatalog(const ThemeCatalogClient::Snapshot& snapshot) {
    m_communityTransferState = snapshot.loading;
    m_allCommunityEntries = snapshot.entries;
    m_communityRevision = snapshot.revision;

    std::unordered_set<std::string> validUrls;
    for (const auto& entry : m_allCommunityEntries) {
        std::string coverUrl = resolveCommunityPreviewUrl(entry.cover);
        if (!coverUrl.empty())
            validUrls.insert(std::move(coverUrl));
        for (const auto& screenshot : entry.screenshots) {
            std::string screenshotUrl = resolveCommunityPreviewUrl(screenshot);
            if (!screenshotUrl.empty())
                validUrls.insert(std::move(screenshotUrl));
        }
    }
    for (auto it = m_communityPreviewCache.begin(); it != m_communityPreviewCache.end();) {
        if (validUrls.find(it->first) == validUrls.end())
            it = m_communityPreviewCache.erase(it);
        else
            ++it;
    }

    applySearchFilter();

    auto hasId = [&](const std::string& id) {
        return std::any_of(m_communityEntries.begin(), m_communityEntries.end(),
                           [&](const ThemeCatalogClient::Entry& entry) {
                               return entry.id == id;
                           });
    };

    if (m_communityEntries.empty()) {
        m_communitySelectedId.clear();
        return;
    }

    if (!hasId(m_communitySelectedId))
        m_communitySelectedId = m_communityEntries.front().id;
}
