#include "UpdaterActivity.hpp"

#include <nxui/Application.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/widgets/Widget.hpp>
#include <switch.h>
#include <switchu/file_log.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

namespace updater {
namespace {

constexpr float kRowH = 76.f;
const nxui::Rect kList{60.f, 128.f, 1160.f, 500.f};

class Painter final : public nxui::Widget {
public:
    explicit Painter(std::function<void(nxui::Renderer&)> fn) : m_fn(std::move(fn)) { setRect({0.f, 0.f, 1280.f, 720.f}); }

protected:
    void onRender(nxui::Renderer& r) override {
        if (m_fn) m_fn(r);
    }

private:
    std::function<void(nxui::Renderer&)> m_fn;
};

std::string fit(nxui::Font& font, std::string text, float maxW, float scale) {
    if (font.measure(text).x * scale <= maxW) return text;
    while (!text.empty() && font.measure(text + "...").x * scale > maxW) {
        size_t cut = text.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.erase(cut);
    }
    return text + "...";
}

} // namespace

bool UpdaterActivity::onCreate() {
    auto& ren = app().renderer();
    ren.setBoxWireframeEnabled(false);
    if (!m_title.load(app().gpu(), ren, "romfs:/fonts/DejaVuSans.ttf", 34)
        || !m_body.load(app().gpu(), ren, "romfs:/fonts/DejaVuSans.ttf", 24)
        || !m_small.load(app().gpu(), ren, "romfs:/fonts/DejaVuSans.ttf", 18))
        return false;
    m_appIcon.loadFromFile(app().gpu(), ren, "romfs:/icon.jpg", 128);
    m_painter = std::make_shared<Painter>([this](nxui::Renderer& r) { draw(r); });
    rootBox().addChild(m_painter);
    m_network = startNetwork();
    startCheck();
    return true;
}

void UpdaterActivity::onDestroy() {
    if (m_worker.joinable()) m_worker.join();
    if (m_network) stopNetwork();
}

// The .nro list now, then where each can be updated from on the worker.
void UpdaterActivity::startCheck() {
    if (m_busy) return;
    if (m_worker.joinable()) m_worker.join();
    m_items.clear();
    for (App& a : scanApps("sdmc:/switch/HomeNX-Updater/HomeNX-Updater.nro")) {
        Item it;
        it.app = std::move(a);
        if (!it.app.icon.empty()) {
            it.icon = std::make_unique<nxui::Texture>();
            if (!it.icon->loadFromMemory(app().gpu(), app().renderer(), it.app.icon.data(), it.app.icon.size(), 96))
                it.icon.reset();
            it.app.icon.clear();
        }
        m_items.push_back(std::move(it));
    }
    m_sel = 0;
    m_scroll = 0.f;
    if (!m_network) {
        m_status = "No internet connection.";
        for (Item& it : m_items) it.row = Row::NoSource;
        return;
    }
    m_status.clear();
    m_checked = 0;
    m_busy = true;
    std::vector<App> apps;
    for (const Item& it : m_items) apps.push_back(it.app);
    m_worker = std::thread([this, apps = std::move(apps)]() mutable {
        findUpdates(apps, m_checked);
        std::lock_guard<std::mutex> lk(m_mu);
        m_found = std::move(apps);
        m_foundReady = true;
        m_busy = false;
    });
}

void UpdaterActivity::startUpdate(std::vector<int> which) {
    if (m_busy || which.empty()) return;
    if (m_worker.joinable()) m_worker.join();
    std::vector<std::pair<int, App>> jobs;
    for (int i : which) {
        m_items[(size_t)i].row = Row::Updating;
        jobs.emplace_back(i, m_items[(size_t)i].app);
    }
    m_busy = true;
    m_worker = std::thread([this, jobs = std::move(jobs)]() {
        for (const auto& [index, a] : jobs) {
            m_updating = index;
            m_progress = 0.f;
            std::string error;
            const bool ok = installUpdate(a, m_progress, error);
            std::lock_guard<std::mutex> lk(m_mu);
            m_finished.emplace_back(index, ok ? std::string() : (error.empty() ? std::string("failed") : error));
        }
        m_updating = -1;
        m_busy = false;
    });
}

void UpdaterActivity::move(int delta) {
    if (m_items.empty()) return;
    m_sel = std::clamp(m_sel + delta, 0, (int)m_items.size() - 1);
}

void UpdaterActivity::onUpdate(float dt) {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_foundReady) {
            m_foundReady = false;
            for (size_t i = 0; i < m_items.size() && i < m_found.size(); ++i) {
                App& a = m_items[i].app;
                a.latest = m_found[i].latest;
                a.source = m_found[i].source;
                a.downloadUrl = m_found[i].downloadUrl;
                a.zip = m_found[i].zip;
                a.zipEntry = m_found[i].zipEntry;
                m_items[i].row = a.downloadUrl.empty() ? Row::NoSource
                               : isNewer(a.latest, a.version) ? Row::Update : Row::UpToDate;
            }
            m_found.clear();
        }
        for (auto& [i, error] : m_finished) {
            Item& it = m_items[(size_t)i];
            it.error = error;
            it.row = error.empty() ? Row::Updated : Row::Failed;
            if (error.empty()) it.app.version = it.app.latest;
        }
        m_finished.clear();
    }

    auto& in = app().input();
    // Up / down, repeating while held.
    const bool up = in.isHeld(nxui::Button::DUp) || in.isHeld(nxui::Button::LStickU);
    const bool down = in.isHeld(nxui::Button::DDown) || in.isHeld(nxui::Button::LStickD);
    if (in.isDown(nxui::Button::DUp) || in.isDown(nxui::Button::LStickU)) {
        move(-1);
        m_repeat = 0.35f;
    } else if (in.isDown(nxui::Button::DDown) || in.isDown(nxui::Button::LStickD)) {
        move(+1);
        m_repeat = 0.35f;
    } else if (up || down) {
        if ((m_repeat -= dt) <= 0.f) {
            move(up ? -1 : 1);
            m_repeat = 0.08f;
        }
    }
    if (in.isDown(nxui::Button::B) || in.isDown(nxui::Button::Plus)) {
        if (!m_busy || m_updating < 0) app().requestExit();
        return;
    }
    if (m_busy) return;
    if (in.isDown(nxui::Button::A) && m_sel < (int)m_items.size()) {
        const Row row = m_items[(size_t)m_sel].row;
        if (row == Row::Update || row == Row::Failed || row == Row::UpToDate) // up to date: install it again
            if (!m_items[(size_t)m_sel].app.downloadUrl.empty()) startUpdate({m_sel});
    }
    if (in.isDown(nxui::Button::X)) {
        std::vector<int> all;
        for (int i = 0; i < (int)m_items.size(); ++i)
            if (m_items[(size_t)i].row == Row::Update) all.push_back(i);
        startUpdate(std::move(all));
    }
    if (in.isDown(nxui::Button::Y)) startCheck();
}

void UpdaterActivity::onRender(nxui::Renderer&) {}

void UpdaterActivity::draw(nxui::Renderer& r) {
    const nxui::Color text = m_theme.textPrimary, muted = m_theme.textSecondary, accent = m_theme.cursorNormal;
    r.drawGradientRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.035f, 0.045f, 0.085f, 1.f),
                       nxui::Color(0.06f, 0.09f, 0.17f, 1.f));

    // Header: icon, title, what's going on.
    if (m_appIcon.valid()) r.drawTextureRounded(&m_appIcon, {60.f, 34.f, 64.f, 64.f}, 16.f);
    r.drawText("Homebrew Updater", {140.f, 34.f}, &m_title, text);
    int updates = 0;
    for (const Item& it : m_items) updates += it.row == Row::Update ? 1 : 0;
    std::string status = m_status;
    if (status.empty()) {
        if (m_busy && m_updating < 0)
            status = "Checking " + std::to_string(m_checked.load()) + " of " + std::to_string(m_items.size()) + "...";
        else if (m_updating >= 0)
            status = "Updating " + m_items[(size_t)m_updating.load()].app.name + "...";
        else
            status = std::to_string(m_items.size()) + " apps, " + (updates ? std::to_string(updates) + " to update"
                                                                            : std::string("all up to date"));
    }
    r.drawText(status, {142.f, 78.f}, &m_small, muted);

    // The list.
    if (m_items.empty()) {
        r.drawText("No .nro apps in sdmc:/switch.", {kList.x, kList.y + 20.f}, &m_body, muted);
    } else {
        const float selTop = m_sel * kRowH;
        if (selTop < m_scroll) m_scroll = selTop;
        if (selTop + kRowH > m_scroll + kList.height) m_scroll = selTop + kRowH - kList.height;
        r.pushClipRect(kList);
        for (int i = 0; i < (int)m_items.size(); ++i) {
            const float y = kList.y + i * kRowH - m_scroll;
            if (y + kRowH < kList.y || y > kList.y + kList.height) continue;
            const Item& it = m_items[(size_t)i];
            const nxui::Rect row{kList.x, y + 3.f, kList.width, kRowH - 6.f};
            const bool sel = i == m_sel;
            r.drawRoundedRect(row, sel ? accent.withAlpha(0.18f) : nxui::Color(1.f, 1.f, 1.f, 0.04f), 16.f);
            if (sel) r.drawRoundedRectOutline(row, accent, 16.f, 2.5f);
            const nxui::Rect ic{row.x + 12.f, row.y + 8.f, row.height - 16.f, row.height - 16.f};
            if (it.icon && it.icon->valid()) r.drawTextureRounded(it.icon.get(), ic, 10.f);
            else r.drawRoundedRect(ic, nxui::Color(1.f, 1.f, 1.f, 0.08f), 10.f);
            const float tx = ic.x + ic.width + 16.f;
            r.drawText(fit(m_body, it.app.name, 520.f, 0.9f), {tx, row.y + 9.f}, &m_body, text, 0.9f);
            std::string line = it.app.version.empty() ? std::string("no version") : it.app.version;
            if (it.row == Row::Update || it.row == Row::Updating) line += "  ->  " + it.app.latest;
            if (!it.app.source.empty()) line += "    " + it.app.source;
            if (it.row == Row::Failed) line = it.error;
            r.drawText(fit(m_small, line, 640.f, 0.9f), {tx, row.y + 40.f}, &m_small, muted, 0.9f);

            // Its state on the right.
            std::string pill;
            nxui::Color pillColor = muted;
            switch (it.row) {
                case Row::Checking: pill = "Checking"; break;
                case Row::UpToDate: pill = "Up to date"; pillColor = nxui::Color(0.35f, 0.8f, 0.5f, 1.f); break;
                case Row::Update: pill = "Update"; pillColor = accent; break;
                case Row::NoSource: pill = "No source"; break;
                case Row::Updating: pill = "Updating"; pillColor = accent; break;
                case Row::Updated: pill = "Updated"; pillColor = nxui::Color(0.35f, 0.8f, 0.5f, 1.f); break;
                case Row::Failed: pill = "Failed"; pillColor = nxui::Color(0.95f, 0.35f, 0.35f, 1.f); break;
            }
            const float pw = m_small.measure(pill).x * 0.9f + 28.f;
            const nxui::Rect pr{row.x + row.width - pw - 18.f, row.y + (row.height - 32.f) * 0.5f, pw, 32.f};
            r.drawRoundedRect(pr, pillColor.withAlpha(0.2f), 16.f);
            r.drawText(pill, {pr.x + 14.f, pr.y + 6.f}, &m_small, pillColor, 0.9f);
            if (it.row == Row::Updating && m_updating == i) {
                const nxui::Rect track{tx, row.y + row.height - 9.f, 520.f, 4.f};
                r.drawRoundedRect(track, nxui::Color(1.f, 1.f, 1.f, 0.12f), 2.f);
                r.drawRoundedRect({track.x, track.y, track.width * std::clamp(m_progress.load(), 0.f, 1.f), track.height},
                                  accent, 2.f);
            }
        }
        r.popClipRect();
    }

    r.drawText(m_busy ? "B  Exit" : "A  Update     X  Update all     Y  Check again     B  Exit", {60.f, 662.f}, &m_small,
               muted);
}

} // namespace updater
