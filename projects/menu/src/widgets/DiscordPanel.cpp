#include "widgets/DiscordPanel.hpp"

#include "third_party/qrcodegen.hpp"

#include <nxui/core/Animation.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/core/Input.hpp>
#include <nxui/core/Renderer.hpp>

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPanelW = 440.f;
constexpr float kPanelH = 652.f;
constexpr float kPanelTop = 16.f;
constexpr float kMarginLeft = 20.f;
constexpr float kAnimDur = 0.25f;
constexpr float kRowH = 46.f;
constexpr float kHeaderH = 34.f;
constexpr float kListTop = 74.f;    // below the title block
constexpr float kListBottom = 50.f; // above the button hints
const nxui::Color kBlurple(0.345f, 0.396f, 0.949f, 1.f);

nxui::Color statusColor(const std::string& s) {
    if (s == "online") return nxui::Color(0.137f, 0.647f, 0.353f, 1.f);
    if (s == "idle") return nxui::Color(0.941f, 0.698f, 0.196f, 1.f);
    if (s == "dnd") return nxui::Color(0.949f, 0.247f, 0.263f, 1.f);
    return nxui::Color(0.5f, 0.52f, 0.56f, 1.f);
}

// Drop whole UTF-8 characters until the text (plus "...") fits.
std::string fit(nxui::Font* font, std::string text, float maxW, float scale) {
    if (!font || font->measure(text).x * scale <= maxW) return text;
    while (!text.empty()) {
        while (!text.empty()) {
            const unsigned char c = static_cast<unsigned char>(text.back());
            text.pop_back();
            if ((c & 0xC0) != 0x80) break;
        }
        if (font->measure(text + "...").x * scale <= maxW) return text + "...";
    }
    return "...";
}

// Greedy word wrap, at most maxLines (the last one ellipsized).
std::vector<std::string> wrap(nxui::Font* font, const std::string& text, float maxW, float scale, int maxLines) {
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i <= text.size()) {
        const size_t sp = text.find(' ', i);
        const std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (font->measure(candidate).x * scale <= maxW || line.empty()) {
            line = candidate;
        } else {
            lines.push_back(line);
            line = word;
            if ((int)lines.size() == maxLines - 1) {
                const std::string rest = text.substr(i);
                lines.push_back(fit(font, rest, maxW, scale));
                return lines;
            }
        }
        if (sp == std::string::npos) break;
        i = sp + 1;
    }
    if (!line.empty()) lines.push_back(fit(font, line, maxW, scale));
    return lines;
}

}  // namespace

DiscordPanel::DiscordPanel() {
    setRect({0.f, 0.f, 1280.f, 720.f});
    setVisible(false);
}

void DiscordPanel::show() {
    m_active = true;
    m_view = View::Home;
    m_chat = 0;
    m_sel = -1;
    m_scroll = 0.f;
    m_builtVersion = ~0ull;
    m_builtState = -1;
    setVisible(true);
    setFocusable(true);
    if (m_service) m_service->activate();
    bindInput();
}

void DiscordPanel::hide() {
    if (!m_active) return;
    m_active = false;
    clearActions();
    if (m_cb.onClosed) m_cb.onClosed();
}

void DiscordPanel::bindInput() {
    clearActions();
    addDirectionAction(nxui::FocusDirection::UP, [this]() { move(-1); });
    addDirectionAction(nxui::FocusDirection::DOWN, [this]() { move(+1); });
    addDirectionAction(nxui::FocusDirection::LEFT, [] {});
    addDirectionAction(nxui::FocusDirection::RIGHT, [] {});
    addAction(static_cast<uint64_t>(nxui::Button::A), [this]() { activate(); });
    addAction(static_cast<uint64_t>(nxui::Button::B), [this]() { back(); });
    addAction(static_cast<uint64_t>(nxui::Button::X), [this]() {
        if (m_view == View::Chat && m_chat && m_cb.onReply) m_cb.onReply(m_chat, m_chatName);
    });
}

void DiscordPanel::back() {
    if (m_view == View::Chat) {
        m_view = View::Home;
        m_chat = 0;
        m_builtVersion = ~0ull;
        if (m_cb.onNavigate) m_cb.onNavigate();
        return;
    }
    if (m_service && m_service->state() == DiscordService::State::QrLogin) {
        m_service->signOut(); // cancels the QR sign-in, back to the sign-in button
        m_builtVersion = ~0ull;
        return;
    }
    hide();
}

void DiscordPanel::move(int dir) {
    if (m_view == View::Chat) { // scroll the conversation
        m_scroll = std::max(0.f, m_scroll + (dir < 0 ? 60.f : -60.f));
        return;
    }
    if (m_rows.empty()) return;
    int i = m_sel;
    for (int step = 0; step < (int)m_rows.size(); ++step) {
        i += dir;
        if (i < 0 || i >= (int)m_rows.size()) return;
        if (selectable(m_rows[(size_t)i])) {
            m_sel = i;
            if (m_cb.onNavigate) m_cb.onNavigate();
            return;
        }
    }
}

void DiscordPanel::activate() {
    if (!m_service || m_view != View::Home || m_sel < 0 || m_sel >= (int)m_rows.size()) return;
    const Row& r = m_rows[(size_t)m_sel];
    if (m_cb.onActivate) m_cb.onActivate();
    if (r.kind == Row::Dm) {
        m_view = View::Chat;
        m_chat = r.channelId;
        m_chatName = r.text;
        m_scroll = 0.f;
        m_service->openChannel(r.channelId);
    } else if (r.action == 1) {
        m_service->startQrLogin();
    } else if (r.action == 2) {
        m_service->signOut();
    } else if (r.action == 3) {
        m_service->startQrLogin();
    }
    m_builtVersion = ~0ull;
}

void DiscordPanel::rebuildRows() {
    auto& i18n = nxui::I18n::instance();
    const int state = m_service ? static_cast<int>(m_service->state()) : -1;
    const uint64_t version = m_service ? m_service->version() : 0;
    if (state == m_builtState && version == m_builtVersion) return;
    m_builtState = state;
    m_builtVersion = version;

    const Snowflake keep = (m_sel >= 0 && m_sel < (int)m_rows.size()) ? m_rows[(size_t)m_sel].channelId : 0;
    m_rows.clear();
    using S = DiscordService::State;
    const S st = m_service ? m_service->state() : S::Failed;
    if (st == S::SignedOut) {
        if (m_service && !m_service->error().empty())
            m_rows.push_back({Row::Info, m_service->error()});
        m_rows.push_back({Row::Info, i18n.tr("discord.signin_body",
            "Sign in once with the Discord app on your phone. HomeNX also uses your SwitchCord account if you have one.")});
        m_rows.push_back({Row::Action, i18n.tr("discord.signin_qr", "Sign in with a QR code"), "", 0, false, 0, 1});
    } else if (st == S::Failed) {
        m_rows.push_back({Row::Info, m_service && !m_service->error().empty() ? m_service->error()
                                                                               : i18n.tr("discord.failed", "Couldn't reach Discord.")});
        m_rows.push_back({Row::Action, i18n.tr("discord.retry", "Sign in again"), "", 0, false, 0, 3});
    } else if (st == S::Ready && m_service) {
        const auto friends = m_service->onlineFriends();
        m_rows.push_back({Row::Header, i18n.tr("discord.online", "Online") + " - " + std::to_string(friends.size())});
        if (friends.empty())
            m_rows.push_back({Row::Info, i18n.tr("discord.nobody", "Nobody's online right now.")});
        for (size_t i = 0; i < friends.size() && i < 12; ++i)
            m_rows.push_back({Row::Friend, friends[i].name, friends[i].status});
        m_rows.push_back({Row::Header, i18n.tr("discord.dms", "Direct messages")});
        for (const auto& d : m_service->dms(25))
            m_rows.push_back({Row::Dm, d.name, "", d.channelId, d.unread, d.mentions});
        m_rows.push_back({Row::Action, i18n.tr("discord.signout", "Sign out of Discord"), "", 0, false, 0, 2});
    }

    // Keep the selection on the same DM across refreshes, else the first selectable row.
    m_sel = -1;
    for (int i = 0; i < (int)m_rows.size(); ++i)
        if (keep && m_rows[(size_t)i].channelId == keep) m_sel = i;
    if (m_sel < 0)
        for (int i = 0; i < (int)m_rows.size(); ++i)
            if (selectable(m_rows[(size_t)i])) { m_sel = i; break; }
}

void DiscordPanel::onUpdate(float dt) {
    const float target = m_active ? 1.f : 0.f;
    if (m_anim != target) {
        const float step = dt / kAnimDur;
        m_anim = m_anim < target ? std::min(target, m_anim + step) : std::max(target, m_anim - step);
        if (m_anim <= 0.f && !m_active) setVisible(false);
    }
    if (m_active && m_view == View::Home) rebuildRows();
}

nxui::Rect DiscordPanel::panelRect() const {
    const float openX = kMarginLeft;
    const float closedX = -kPanelW - 10.f;
    const float x = closedX + (openX - closedX) * nxui::Easing::outCubic(m_anim);
    return {x, kPanelTop, kPanelW, kPanelH};
}

void DiscordPanel::drawQr(nxui::Renderer& ren, const std::string& text, const nxui::Rect& area, float alpha) {
    if (text != m_qrText) {
        m_qrText = text;
        m_qrModules.clear();
        m_qrSize = 0;
        if (!text.empty()) {
            const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            m_qrSize = qr.getSize();
            m_qrModules.resize((size_t)m_qrSize * (size_t)m_qrSize);
            for (int y = 0; y < m_qrSize; ++y)
                for (int x = 0; x < m_qrSize; ++x)
                    m_qrModules[(size_t)y * (size_t)m_qrSize + (size_t)x] = qr.getModule(x, y);
        }
    }
    ren.drawRoundedRect(area, nxui::Color(1.f, 1.f, 1.f, alpha), 14.f);
    if (m_qrSize <= 0) return;
    const float quiet = 4.f;
    const float cell = std::floor(area.width / (m_qrSize + quiet * 2.f));
    const float side = cell * m_qrSize;
    const float ox = area.x + (area.width - side) * 0.5f, oy = area.y + (area.height - side) * 0.5f;
    const nxui::Color dark(0.f, 0.f, 0.f, alpha);
    for (int y = 0; y < m_qrSize; ++y) {
        int run = -1; // draw horizontal runs as one rect
        for (int x = 0; x <= m_qrSize; ++x) {
            const bool on = x < m_qrSize && m_qrModules[(size_t)y * (size_t)m_qrSize + (size_t)x];
            if (on && run < 0) run = x;
            if (!on && run >= 0) {
                ren.drawRect({ox + run * cell, oy + y * cell, (x - run) * cell, cell}, dark);
                run = -1;
            }
        }
    }
}

void DiscordPanel::onRender(nxui::Renderer& ren) {
    if (m_anim <= 0.001f || !m_font || !m_smallFont) return;
    auto& i18n = nxui::I18n::instance();
    const float a = m_anim;
    const bool light = m_theme && m_theme->mode == nxui::ThemeMode::Light;
    const nxui::Color text = light ? nxui::Color(0.06f, 0.07f, 0.09f, a) : nxui::Color(1.f, 1.f, 1.f, a);
    const nxui::Color muted = light ? nxui::Color(0.31f, 0.34f, 0.40f, a) : nxui::Color(0.70f, 0.76f, 0.86f, a);
    const nxui::Color fill = light ? nxui::Color(0.96f, 0.97f, 0.99f, 0.97f * a)
                                   : (m_theme ? m_theme->panelBase.withAlpha(0.94f * a)
                                              : nxui::Color(0.10f, 0.13f, 0.18f, 0.94f * a));
    const nxui::Color border = light ? nxui::Color(0.08f, 0.10f, 0.14f, 0.14f * a) : nxui::Color(1.f, 1.f, 1.f, 0.14f * a);
    const nxui::Color cursor = m_theme ? m_theme->cursorNormal.withAlpha(a) : kBlurple.withAlpha(a);

    ren.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.25f * a));
    const nxui::Rect p = panelRect();
    ren.drawRoundedRect(p, fill, 24.f);
    ren.drawRoundedRectOutline(p, border, 24.f, 1.2f);

    // Title: a blurple badge, "Discord", and who's signed in.
    const nxui::Rect badge{p.x + 20.f, p.y + 18.f, 38.f, 38.f};
    ren.drawRoundedRect(badge, kBlurple.withAlpha(a), 11.f);
    ren.drawText("D", {badge.x + 11.f, badge.y + 5.f}, m_font, nxui::Color(1.f, 1.f, 1.f, a), 1.0f);
    ren.drawText("Discord", {badge.x + 50.f, p.y + 16.f}, m_font, text, 1.0f);
    std::string who;
    if (m_service) {
        switch (m_service->state()) {
            case DiscordService::State::Ready: who = m_service->myName(); break;
            case DiscordService::State::Connecting: who = i18n.tr("discord.connecting", "Connecting..."); break;
            case DiscordService::State::QrLogin: who = i18n.tr("discord.signing_in", "Signing in"); break;
            case DiscordService::State::SignedOut: who = i18n.tr("discord.signed_out", "Not signed in"); break;
            default: who = i18n.tr("discord.offline", "Offline"); break;
        }
    }
    ren.drawText(fit(m_smallFont, who, kPanelW - 110.f, 0.85f), {badge.x + 50.f, p.y + 44.f}, m_smallFont, muted, 0.85f);

    const float contentX = p.x + 20.f, contentW = kPanelW - 40.f;
    const float top = p.y + kListTop, bottom = p.y + kPanelH - kListBottom;
    ren.pushClipRect({p.x, top, kPanelW, bottom - top});

    if (m_service && m_service->state() == DiscordService::State::QrLogin) {
        const std::string url = m_service->qrUrl();
        const nxui::Rect area{p.x + (kPanelW - 280.f) * 0.5f, top + 10.f, 280.f, 280.f};
        drawQr(ren, url, area, a);
        if (url.empty())
            ren.drawText(i18n.tr("discord.qr_wait", "Getting a code..."), {area.x + 70.f, area.y + 128.f}, m_smallFont,
                         nxui::Color(0.f, 0.f, 0.f, a), 0.9f);
        float y = area.y + area.height + 22.f;
        for (const auto& line : wrap(m_smallFont, m_service->qrStatus(), contentW, 0.88f, 4)) {
            ren.drawText(line, {contentX, y}, m_smallFont, text, 0.88f);
            y += 26.f;
        }
    } else if (m_view == View::Chat && m_service) {
        ren.drawText(fit(m_font, m_chatName, contentW, 0.9f), {contentX, top}, m_font, text, 0.9f);
        const auto lines = m_service->lines(m_chat, 40);
        // Newest at the bottom; Up scrolls back.
        float y = bottom - 8.f + m_scroll;
        if (lines.empty())
            ren.drawText(m_service->loading(m_chat) ? i18n.tr("discord.loading", "Loading...")
                                                    : i18n.tr("discord.no_messages", "No messages yet."),
                         {contentX, top + 44.f}, m_smallFont, muted, 0.88f);
        for (auto it = lines.rbegin(); it != lines.rend() && y > top + 30.f; ++it) {
            const auto body = wrap(m_smallFont, it->text.empty() ? std::string(" ") : it->text, contentW, 0.86f, 4);
            const float blockH = 24.f + body.size() * 23.f + 10.f;
            y -= blockH;
            const nxui::Color nameCol = it->mine ? kBlurple.withAlpha(a) : text;
            ren.drawText(fit(m_smallFont, it->author, contentW, 0.82f), {contentX, y}, m_smallFont, nameCol, 0.82f);
            float ly = y + 24.f;
            const nxui::Color bodyCol = it->failed ? statusColor("dnd").withAlpha(a)
                                       : it->pending ? muted : text;
            for (const auto& l : body) {
                ren.drawText(l, {contentX, ly}, m_smallFont, bodyCol, 0.86f);
                ly += 23.f;
            }
        }
    } else {
        // Keep the selected row in view.
        float total = 0.f, selTop = 0.f, selH = 0.f;
        for (int i = 0; i < (int)m_rows.size(); ++i) {
            const float h = m_rows[(size_t)i].kind == Row::Header ? kHeaderH
                          : m_rows[(size_t)i].kind == Row::Info ? 26.f * wrap(m_smallFont, m_rows[(size_t)i].text, contentW, 0.86f, 4).size() + 10.f
                          : kRowH;
            if (i == m_sel) { selTop = total; selH = h; }
            total += h;
        }
        const float viewH = bottom - top;
        if (selTop - m_scroll < 0.f) m_scroll = selTop;
        if (selTop + selH - m_scroll > viewH) m_scroll = selTop + selH - viewH;
        m_scroll = std::clamp(m_scroll, 0.f, std::max(0.f, total - viewH));

        float y = top - m_scroll;
        if (m_rows.empty())
            ren.drawText(i18n.tr("discord.connecting", "Connecting..."), {contentX, top + 6.f}, m_smallFont, muted, 0.9f);
        for (int i = 0; i < (int)m_rows.size(); ++i) {
            const Row& r = m_rows[(size_t)i];
            if (r.kind == Row::Header) {
                ren.drawText(r.text, {contentX, y + 8.f}, m_smallFont, muted, 0.78f);
                y += kHeaderH;
                continue;
            }
            if (r.kind == Row::Info) {
                for (const auto& line : wrap(m_smallFont, r.text, contentW, 0.86f, 4)) {
                    ren.drawText(line, {contentX, y + 4.f}, m_smallFont, muted, 0.86f);
                    y += 26.f;
                }
                y += 10.f;
                continue;
            }
            const nxui::Rect row{contentX - 6.f, y + 3.f, contentW + 12.f, kRowH - 6.f};
            if (i == m_sel) {
                ren.drawRoundedRect(row, cursor.withAlpha(0.18f * a), 12.f);
                ren.drawRoundedRectOutline(row, cursor, 12.f, 2.f);
            }
            float tx = contentX + 6.f;
            if (r.kind == Row::Friend) {
                ren.drawCircle({tx + 6.f, y + kRowH * 0.5f}, 6.f, statusColor(r.detail).withAlpha(a), 20);
                tx += 22.f;
            }
            if (r.kind == Row::Action) {
                ren.drawText(fit(m_font, r.text, contentW - 12.f, 0.8f), {tx, y + 9.f}, m_font, cursor, 0.8f);
            } else {
                ren.drawText(fit(m_font, r.text, contentW - (tx - contentX) - 40.f, 0.8f), {tx, y + 9.f}, m_font, text, 0.8f);
            }
            if (r.kind == Row::Dm && (r.unread || r.mentions > 0)) {
                const nxui::Vec2 c{contentX + contentW - 10.f, y + kRowH * 0.5f};
                if (r.mentions > 0) {
                    ren.drawCircle(c, 11.f, statusColor("dnd").withAlpha(a), 24);
                    const std::string n = r.mentions > 9 ? "9+" : std::to_string(r.mentions);
                    const float w = m_smallFont->measure(n).x * 0.7f;
                    ren.drawText(n, {c.x - w * 0.5f, c.y - 9.f}, m_smallFont, nxui::Color(1.f, 1.f, 1.f, a), 0.7f);
                } else {
                    ren.drawCircle(c, 5.f, text, 16);
                }
            }
            y += kRowH;
        }
    }
    ren.popClipRect();

    // Button hints.
    std::string hints;
    if (m_service && m_service->state() == DiscordService::State::QrLogin)
        hints = i18n.tr("discord.hint_qr", "B  Cancel");
    else if (m_view == View::Chat)
        hints = i18n.tr("discord.hint_chat", "X  Reply     Up/Down  Scroll     B  Back");
    else
        hints = i18n.tr("discord.hint_home", "A  Open     B  Close");
    ren.drawText(hints, {contentX, p.y + kPanelH - 36.f}, m_smallFont, muted, 0.8f);
}
