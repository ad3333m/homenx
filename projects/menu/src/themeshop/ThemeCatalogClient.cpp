#include "ThemeCatalogClient.hpp"

#include "ThemeHttp.hpp"

#include "core/DebugLog.hpp"

#include <nxui/core/ThreadPool.hpp>

#include <curlpp/Easy.hpp>
#include <curlpp/Exception.hpp>
#include <curlpp/Infos.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/cURLpp.hpp>
#include <nlohmann/json.hpp>
#include <switch.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace {

void readStringOpt(const nlohmann::json& j, const char* key, std::string& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return;
    out = it->get<std::string>();
}

void appendUniqueString(std::vector<std::string>& out, std::string value) {
    if (value.empty())
        return;
    if (std::find(out.begin(), out.end(), value) != out.end())
        return;
    out.push_back(std::move(value));
}

void readStringArrayOpt(const nlohmann::json& j, const char* key, std::vector<std::string>& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array())
        return;

    for (const auto& item : *it) {
        if (item.is_string())
            appendUniqueString(out, item.get<std::string>());
    }
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string trimSlashes(std::string value) {
    while (!value.empty() && value.front() == '/')
        value.erase(value.begin());
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    return value;
}

std::string parentDirectory(const std::string& path) {
    std::string clean = trimSlashes(path);
    std::size_t slash = clean.find_last_of('/');
    if (slash == std::string::npos)
        return {};
    return clean.substr(0, slash);
}

std::string joinPath(const std::string& base, const std::string& relative) {
    if (base.empty())
        return relative;
    if (relative.empty())
        return base;
    if (base.back() == '/')
        return base + relative;
    return base + "/" + relative;
}

std::string deriveManifestPath(const std::string& path) {
    if (path.empty())
        return {};
    return path + "/theme.json";
}

std::string resolvePreviewPath(const ThemeCatalogClient::Entry& entry, std::string previewPath) {
    previewPath = trimSlashes(std::move(previewPath));
    if (previewPath.empty())
        return {};
    if (startsWith(previewPath, "https://") || startsWith(previewPath, "http://"))
        return previewPath;

    if (!entry.path.empty())
        return joinPath(trimSlashes(entry.path), previewPath);

    return joinPath(parentDirectory(entry.manifest), previewPath);
}

void appendResolvedPreviewPath(const ThemeCatalogClient::Entry& entry,
                               std::vector<std::string>& out,
                               const std::string& previewPath) {
    appendUniqueString(out, resolvePreviewPath(entry, previewPath));
}

void readResolvedStringArrayOpt(const nlohmann::json& j,
                                const char* key,
                                const ThemeCatalogClient::Entry& entry,
                                std::vector<std::string>& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array())
        return;

    for (const auto& item : *it) {
        if (item.is_string())
            appendResolvedPreviewPath(entry, out, item.get<std::string>());
    }
}

void ensureCoverFirst(ThemeCatalogClient::Entry& entry) {
    if (entry.cover.empty()) {
        if (!entry.screenshots.empty())
            entry.cover = entry.screenshots.front();
        return;
    }

    auto it = std::find(entry.screenshots.begin(), entry.screenshots.end(), entry.cover);
    if (it == entry.screenshots.end()) {
        entry.screenshots.insert(entry.screenshots.begin(), entry.cover);
        return;
    }

    if (it != entry.screenshots.begin()) {
        std::string cover = *it;
        entry.screenshots.erase(it);
        entry.screenshots.insert(entry.screenshots.begin(), std::move(cover));
    }
}

struct PreviewData {
    std::string cover;
    std::vector<std::string> screenshots;
};

PreviewData derivePreviewDataFromManifest(const std::string& catalogUrl,
                                          const ThemeCatalogClient::Entry& entry) {
    if (entry.manifest.empty())
        return {};

    std::size_t slash = catalogUrl.find_last_of('/');
    if (slash == std::string::npos)
        return {};

    const std::string manifestUrl = catalogUrl.substr(0, slash + 1) + trimSlashes(entry.manifest);
    const std::string manifestText = themeshop::http::getText(manifestUrl);

    nlohmann::json manifest;
    try {
        manifest = nlohmann::json::parse(manifestText);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("Invalid theme manifest JSON: ") + ex.what());
    }

    PreviewData data;
    std::string cover;
    readStringOpt(manifest, "cover", cover);
    if (!cover.empty())
        data.cover = resolvePreviewPath(entry, cover);

    appendUniqueString(data.screenshots, data.cover);

    std::string screenshot;
    readStringOpt(manifest, "screenshot", screenshot);
    appendResolvedPreviewPath(entry, data.screenshots, screenshot);
    readResolvedStringArrayOpt(manifest, "screenshots", entry, data.screenshots);

    auto previewIt = manifest.find("preview");
    if (previewIt == manifest.end() || !previewIt->is_object()) {
        if (data.cover.empty() && !data.screenshots.empty())
            data.cover = data.screenshots.front();
        return data;
    }

    cover.clear();
    readStringOpt(*previewIt, "cover", cover);
    if (!cover.empty() && data.cover.empty())
        data.cover = resolvePreviewPath(entry, cover);
    appendResolvedPreviewPath(entry, data.screenshots, cover);

    auto screenshotsIt = previewIt->find("screenshots");
    if (screenshotsIt != previewIt->end() && screenshotsIt->is_array()) {
        for (const auto& screenshot : *screenshotsIt) {
            if (screenshot.is_string())
                appendResolvedPreviewPath(entry, data.screenshots, screenshot.get<std::string>());
        }
    }

    auto screenshotIt = previewIt->find("screenshot");
    if (screenshotIt != previewIt->end() && screenshotIt->is_string())
        appendResolvedPreviewPath(entry, data.screenshots, screenshotIt->get<std::string>());

    if (data.cover.empty() && !data.screenshots.empty())
        data.cover = data.screenshots.front();

    return data;
}

} // namespace

ThemeCatalogClient::ThemeCatalogClient(std::string catalogUrl)
    : m_catalogUrl(std::move(catalogUrl)) {
}

void ThemeCatalogClient::setCatalogUrl(std::string catalogUrl) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_catalogUrl = std::move(catalogUrl);
}

void ThemeCatalogClient::refresh(nxui::ThreadPool& pool) {
    if (m_future.valid() && m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    if (m_future.valid())
        m_future.get();

    std::string url;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        url = m_catalogUrl;
        m_loading.begin("Loading theme catalog...");
        ++m_revision;
    }

    DebugLog::log("[themeshop] catalog refresh start: %s", url.c_str());

    m_future = pool.submit([this, url]() {
        // HomeNX's wallpapers, then the community's themes; either one is enough.
        Snapshot loaded;
        std::string error;
        for (const std::string& u : {std::string(kHomeNXCatalogUrl), url}) {
            try {
                Snapshot one = loadCatalog(u);
                for (auto& entry : one.entries)
                    loaded.entries.push_back(std::move(entry));
                DebugLog::log("[themeshop] catalog %s: %d entries", u.c_str(), (int)one.entries.size());
            } catch (const std::exception& ex) {
                error = ex.what();
                DebugLog::log("[themeshop] catalog %s failed: %s", u.c_str(), ex.what());
            } catch (...) {
                error = "Unknown network error";
                DebugLog::log("[themeshop] catalog %s failed: unknown error", u.c_str());
            }
        }
        if (loaded.entries.empty() && !error.empty())
            loaded.loading.fail(error);
        else
            loaded.loading.succeed(std::to_string(loaded.entries.size()) + " themes available.");

        std::lock_guard<std::mutex> lk(m_mutex);
        if (loaded.loading.isReady())
            m_entries = std::move(loaded.entries);
        m_loading = loaded.loading;
        ++m_revision;
    });
}

bool ThemeCatalogClient::poll() {
    if (!m_future.valid())
        return false;
    if (m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return false;

    m_future.get();
    return true;
}

ThemeCatalogClient::Snapshot ThemeCatalogClient::snapshot() const {
    std::lock_guard<std::mutex> lk(m_mutex);

    Snapshot snapshot;
    snapshot.loading = m_loading;
    snapshot.entries = m_entries;
    snapshot.revision = m_revision;
    return snapshot;
}

ThemeCatalogClient::Snapshot ThemeCatalogClient::loadCatalog(const std::string& url) {
    Snapshot snapshot;

    const std::string body = themeshop::http::getText(url);

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("Invalid catalog JSON: ") + ex.what());
    }

    auto themesIt = root.find("themes");
    if (themesIt == root.end() || !themesIt->is_array())
        throw std::runtime_error("Catalog is missing the themes array");

    snapshot.entries.reserve(themesIt->size());
    for (const auto& item : *themesIt) {
        if (!item.is_object())
            continue;

        Entry entry;
        readStringOpt(item, "id", entry.id);
        readStringOpt(item, "name", entry.name);
        readStringOpt(item, "author", entry.author);
        readStringOpt(item, "version", entry.version);
        readStringOpt(item, "path", entry.path);
        readStringOpt(item, "manifest", entry.manifest);
        readStringOpt(item, "cover", entry.cover);
        readStringArrayOpt(item, "screenshots", entry.screenshots);

        std::string screenshot;
        readStringOpt(item, "screenshot", screenshot);
        appendUniqueString(entry.screenshots, screenshot);

        if (entry.id.empty() || entry.name.empty())
            continue;
        if (entry.manifest.empty())
            entry.manifest = deriveManifestPath(entry.path);
        if ((entry.cover.empty() || entry.screenshots.empty()) && !entry.manifest.empty()) {
            try {
                PreviewData preview = derivePreviewDataFromManifest(url, entry);
                if (entry.cover.empty())
                    entry.cover = preview.cover;
                for (auto& screenshotPath : preview.screenshots)
                    appendUniqueString(entry.screenshots, std::move(screenshotPath));
            } catch (const std::exception& ex) {
                DebugLog::log("[themeshop] manifest preview fallback failed for %s: %s",
                              entry.id.c_str(),
                              ex.what());
            }
        }

        ensureCoverFirst(entry);

        // Pictures as full links: the shop shows entries from more than one catalogue.
        auto absolute = [&url](std::string& path) {
            const std::size_t slash = url.find_last_of('/');
            if (path.empty() || startsWith(path, "https://") || startsWith(path, "http://") || slash == std::string::npos)
                return;
            path = url.substr(0, slash + 1) + trimSlashes(path);
        };
        absolute(entry.cover);
        for (auto& shot : entry.screenshots)
            absolute(shot);
        entry.catalogUrl = url;

        snapshot.entries.push_back(std::move(entry));
    }

    std::sort(snapshot.entries.begin(), snapshot.entries.end(), [](const Entry& lhs, const Entry& rhs) {
        if (lhs.name != rhs.name)
            return lhs.name < rhs.name;
        return lhs.id < rhs.id;
    });

    if (snapshot.entries.empty())
        snapshot.loading.succeed("Catalog is empty.");
    else
        snapshot.loading.succeed(std::to_string(snapshot.entries.size()) + " themes available.");

    return snapshot;
}