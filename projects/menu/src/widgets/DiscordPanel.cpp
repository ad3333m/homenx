#include "widgets/DiscordPanel.hpp"
#include <nxui/third_party/stb/stb_image.h>
#include <switch.h>
#include <switchu/fancy_text.hpp>

#include "net/async.h"
#include "net/http.h"
#include "third_party/qrcodegen.hpp"
#include "util/strings.h"

#include <nxui/core/Animation.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/core/Input.hpp>
#include <nxui/core/Renderer.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace {

constexpr float kPanelX = 16.f;
constexpr float kPanelY = 16.f;
constexpr float kPanelW = 460.f;
constexpr float kPanelH = 688.f;
constexpr float kAnimDur = 0.25f;
constexpr float kTopH = 56.f;     // title bar
constexpr float kMeH = 64.f;      // your account, at the bottom
constexpr float kInputH = 60.f;   // the message bar in a chat
constexpr float kWelcomeH = 176.f;
constexpr float kRailW = 72.f;    // the server bar
constexpr float kRailIcon = 48.f;
constexpr float kChatTextX = 62.f;
constexpr float kChatTextW = kPanelW - kChatTextX - 20.f;

// Discord's dark theme.
nxui::Color rgb(int r, int g, int b, float a) { return nxui::Color(r / 255.f, g / 255.f, b / 255.f, a); }
nxui::Color bgPrimary(float a) { return rgb(49, 51, 56, a); }    // chat
nxui::Color bgSecondary(float a) { return rgb(43, 45, 49, a); }  // DM list
nxui::Color bgTertiary(float a) { return rgb(30, 31, 34, a); }
nxui::Color bgUser(float a) { return rgb(35, 36, 40, a); }       // your account
nxui::Color bgSelected(float a) { return rgb(64, 66, 73, a); }
nxui::Color bgInput(float a) { return rgb(56, 58, 64, a); }
nxui::Color textNormal(float a) { return rgb(219, 222, 225, a); }
nxui::Color textHeader(float a) { return rgb(242, 243, 245, a); }
nxui::Color textMuted(float a) { return rgb(148, 155, 164, a); }
nxui::Color blurple(float a) { return rgb(88, 101, 242, a); }
nxui::Color blurpleDark(float a) { return rgb(71, 82, 196, a); }
nxui::Color red(float a) { return rgb(242, 63, 67, a); }
nxui::Color green(float a) { return rgb(35, 165, 90, a); }

std::string statusLabel(const std::string& s) {
    auto& i18n = nxui::I18n::instance();
    if (s == "online") return i18n.tr("discord.status.online", "Online");
    if (s == "idle") return i18n.tr("discord.status.idle", "Idle");
    if (s == "dnd") return i18n.tr("discord.status.dnd", "Do Not Disturb");
    if (s == "invisible") return i18n.tr("discord.status.invisible", "Invisible");
    return i18n.tr("discord.status.offline", "Offline");
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
    if (!font) return lines;
    std::string line;
    size_t i = 0;
    while (i <= text.size()) {
        const size_t sp = text.find(' ', i);
        const std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (font->measure(candidate).x * scale <= maxW || line.empty()) {
            line = candidate;
        } else {
            lines.push_back(fit(font, line, maxW, scale));
            line = word;
            if ((int)lines.size() == maxLines - 1) {
                lines.push_back(fit(font, text.substr(i), maxW, scale));
                return lines;
            }
        }
        if (sp == std::string::npos) break;
        i = sp + 1;
    }
    if (!line.empty()) lines.push_back(fit(font, line, maxW, scale));
    return lines;
}

bool inside(const nxui::Rect& r, float x, float y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

constexpr uint32_t kMarkFirst = 0xF0000; // emoji pictures (DiscordService::Line::emojis)
constexpr uint32_t kMarkLast = 0xFFFFD;
constexpr std::size_t kPictureBudget = 24u << 20; // decoded images + GIFs on the GPU

// Fancy Unicode letters as plain ones when no font has them: switchu::plainLetter.
using switchu::plainLetter;

struct Decoded {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    std::vector<nxui::Rect> frames;
    std::vector<int> delays;
};

// A GIF's size and frame count from its blocks, without decoding it.
bool gifInfo(const std::string& d, int& w, int& h, int& frames) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(d.data());
    const size_t n = d.size();
    if (n < 13 || d.compare(0, 3, "GIF") != 0) return false;
    w = p[6] | (p[7] << 8);
    h = p[8] | (p[9] << 8);
    frames = 0;
    size_t i = 13;
    if (p[10] & 0x80) i += 3u << ((p[10] & 7) + 1);
    while (i < n) {
        const uint8_t b = p[i++];
        if (b == 0x3B) break;
        if (b == 0x21) { // extension: label, then sub-blocks
            ++i;
            while (i < n && p[i]) i += p[i] + 1u;
            ++i;
        } else if (b == 0x2C) { // image: descriptor, colour table, LZW data
            if (i + 9 > n) break;
            const uint8_t flags = p[i + 8];
            i += 9;
            if (flags & 0x80) i += 3u << ((flags & 7) + 1);
            ++i;
            while (i < n && p[i]) i += p[i] + 1u;
            ++i;
            ++frames;
        } else {
            break;
        }
    }
    return frames > 0 && w > 0 && h > 0;
}

// Area-average downscale (only ever shrinks).
void resizeInto(const uint8_t* src, int sw, int sh, int sstride, uint8_t* dst, int dw, int dh, int dstride) {
    for (int y = 0; y < dh; ++y) {
        const int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; ++x) {
            const int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            unsigned sum[4] = {0, 0, 0, 0};
            unsigned n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* px = src + (size_t)yy * sstride + (size_t)xx * 4;
                    sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2]; sum[3] += px[3];
                    ++n;
                }
            uint8_t* o = dst + (size_t)y * dstride + (size_t)x * 4;
            for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>(sum[c] / n);
        }
    }
}

// A still picture fitted into maxW x maxH, or a GIF's frames (up to 30, spread
// over the whole animation) side by side in one texture of at most 1024x1024.
// GIFs too big to decode in memory show their first frame.
bool decodePicture(const std::string& body, bool animated, int maxW, int maxH, Decoded& out) {
    const auto* data = reinterpret_cast<const stbi_uc*>(body.data());
    const int len = static_cast<int>(body.size());
    int gw = 0, gh = 0, count = 0;
    if (animated && gifInfo(body, gw, gh, count) && count > 1
        && static_cast<uint64_t>(gw) * gh * 4u * count <= (32ull << 20)) {
        int* delays = nullptr;
        int w = 0, h = 0, z = 0, comp = 0;
        stbi_uc* frames = stbi_load_gif_from_memory(data, len, &delays, &w, &h, &z, &comp, 4);
        if (frames && z > 0 && w > 0 && h > 0) {
            const float sc = std::min(1.f, std::min((float)maxW / w, (float)maxH / h));
            const int fw = std::max(1, (int)(w * sc)), fh = std::max(1, (int)(h * sc));
            const int cols = std::max(1, std::min(z, 1024 / fw));
            const int keep = std::max(1, std::min({z, 30, cols * std::max(1, 1024 / fh)}));
            const int rows = (keep + cols - 1) / cols;
            out.w = std::min(cols, keep) * fw;
            out.h = rows * fh;
            out.rgba.assign((size_t)out.w * out.h * 4, 0);
            for (int k = 0; k < keep; ++k) {
                const int first = k * z / keep, next = std::max(first + 1, (k + 1) * z / keep);
                int ms = 0;
                for (int f = first; f < next && f < z; ++f) ms += delays && delays[f] > 10 ? delays[f] : 100;
                const int cx = (k % cols) * fw, cy = (k / cols) * fh;
                resizeInto(frames + (size_t)first * w * h * 4, w, h, w * 4,
                           out.rgba.data() + ((size_t)cy * out.w + cx) * 4, fw, fh, out.w * 4);
                out.frames.push_back({(float)cx, (float)cy, (float)fw, (float)fh});
                out.delays.push_back(ms);
            }
            stbi_image_free(frames);
            if (delays) stbi_image_free(delays);
            return true;
        }
        if (frames) stbi_image_free(frames);
        if (delays) stbi_image_free(delays);
    }
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
    if (!px) return false;
    const float sc = std::min(1.f, std::min((float)maxW / w, (float)maxH / h));
    out.w = std::max(1, (int)(w * sc));
    out.h = std::max(1, (int)(h * sc));
    out.rgba.resize((size_t)out.w * out.h * 4);
    resizeInto(px, w, h, w * 4, out.rgba.data(), out.w, out.h, out.w * 4);
    stbi_image_free(px);
    return true;
}

// How big a picture is drawn in the chat: Discord's own size, fitted to 300x220.
nxui::Vec2 mediaSize(const DiscordService::Line::Media& m) {
    const float w = m.w > 0 ? (float)m.w : 220.f, h = m.h > 0 ? (float)m.h : 160.f;
    const float sc = std::min(1.f, std::min(300.f / w, 220.f / h));
    return {std::max(24.f, w * sc), std::max(24.f, h * sc)};
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
    m_followSel = true;
    m_builtVersion = ~0ull;
    m_builtState = -1;
    setVisible(true);
    setFocusable(true);
    if (m_service) m_service->activate();
    bindInput();
}

void DiscordPanel::openChat(Snowflake channelId, Snowflake guildId, const std::string& name,
                            const std::string& avatarUrl) {
    if (!m_active || !m_service || !channelId) return;
    m_guild = guildId;
    m_view = View::Chat;
    m_chat = channelId;
    m_chatName = clean(name);
    m_chatIsChannel = guildId != 0;
    m_chatAvatar = m_chatIsChannel ? std::string() : avatarUrl;
    m_chatStatus.clear();
    m_chatScroll = 0.f;
    m_chatBuiltFor = 0;
    m_builtVersion = ~0ull;
    m_service->openChannel(channelId);
}

void DiscordPanel::hide() {
    if (!m_active) return;
    m_active = false;
    clearActions();
    // A focusable full-screen widget left behind is what the selection ring
    // wrapped (the whole screen turned blue): hand focus back for good.
    setFocusable(false);
    if (m_cb.onClosed) m_cb.onClosed();
}

void DiscordPanel::bindInput() {
    clearActions();
    addDirectionAction(nxui::FocusDirection::UP, [this]() { listFocused() ? move(-1) : moveRail(-1); });
    addDirectionAction(nxui::FocusDirection::DOWN, [this]() { listFocused() ? move(+1) : moveRail(+1); });
    // Left/Right: between the server bar and the list.
    addDirectionAction(nxui::FocusDirection::LEFT, [this] {
        if (railShown() && m_column == 1) {
            m_column = 0;
            if (m_cb.onNavigate) m_cb.onNavigate();
        }
    });
    addDirectionAction(nxui::FocusDirection::RIGHT, [this] {
        if (railShown() && m_column == 0) {
            m_column = 1;
            if (m_cb.onNavigate) m_cb.onNavigate();
        }
    });
    addAction(static_cast<uint64_t>(nxui::Button::A), [this]() {
        if (!listFocused()) { // A on a server goes into its channels
            m_column = 1;
            if (m_cb.onNavigate) m_cb.onNavigate();
            return;
        }
        activateRow();
    });
    addAction(static_cast<uint64_t>(nxui::Button::B), [this]() { back(); });
    addAction(static_cast<uint64_t>(nxui::Button::X), [this]() {
        if (m_view == View::Chat && m_chat && m_cb.onReply) m_cb.onReply(m_chat, m_chatName);
    });
    addAction(static_cast<uint64_t>(nxui::Button::Y), [this]() { openAccounts(); });
}

void DiscordPanel::openAccounts() {
    if (!m_service || m_view == View::Accounts) return;
    const auto st = m_service->state();
    if (st != DiscordService::State::Ready && st != DiscordService::State::Connecting) return;
    m_view = View::Accounts;
    m_sel = -1;
    m_scroll = 0.f;
    m_followSel = true;
    m_builtVersion = ~0ull;
    if (m_cb.onActivate) m_cb.onActivate();
}

bool DiscordPanel::railShown() const {
    if (!m_service || m_view != View::Home) return false;
    const auto st = m_service->state();
    return st == DiscordService::State::Ready || st == DiscordService::State::Connecting;
}

void DiscordPanel::moveRail(int dir) {
    int cur = 0;
    for (size_t i = 0; i < m_servers.size(); ++i)
        if (m_servers[i].id == m_guild) cur = (int)i + 1;
    const int next = std::clamp(cur + dir, 0, (int)m_servers.size());
    if (next == cur) return;
    m_guild = next == 0 ? 0 : m_servers[(size_t)next - 1].id;
    m_sel = -1;
    m_scroll = 0.f;
    m_followSel = true;
    m_builtVersion = ~0ull;
    if (m_cb.onNavigate) m_cb.onNavigate();
}

void DiscordPanel::back() {
    if (m_service && m_service->state() == DiscordService::State::QrLogin) {
        m_service->cancelQrLogin();
        m_builtVersion = ~0ull;
        if (m_cb.onNavigate) m_cb.onNavigate();
        return;
    }
    if (m_view != View::Home) {
        m_view = View::Home;
        m_chat = 0;
        m_builtVersion = ~0ull;
        if (m_cb.onNavigate) m_cb.onNavigate();
        return;
    }
    hide();
}

void DiscordPanel::move(int dir) {
    if (!m_service || m_service->state() == DiscordService::State::QrLogin) return;
    if (m_view == View::Chat) { // scroll the conversation; Up goes back in time
        m_chatScroll = std::max(0.f, m_chatScroll + (dir < 0 ? 80.f : -80.f));
        return;
    }
    if (m_rows.empty()) return;
    int i = m_sel;
    for (int step = 0; step < (int)m_rows.size(); ++step) {
        i += dir;
        if (i < 0 || i >= (int)m_rows.size()) return;
        if (selectable(m_rows[(size_t)i])) {
            m_sel = i;
            m_followSel = true;
            if (m_cb.onNavigate) m_cb.onNavigate();
            return;
        }
    }
}

void DiscordPanel::activateRow() {
    if (!m_service) return;
    if (m_service->state() == DiscordService::State::QrLogin) {
        m_service->startQrLogin(); // a fresh code
        if (m_cb.onActivate) m_cb.onActivate();
        return;
    }
    if (m_view == View::Chat) {
        if (m_chat && m_cb.onReply) m_cb.onReply(m_chat, m_chatName);
        return;
    }
    if (m_sel < 0 || m_sel >= (int)m_rows.size()) return;
    const Row r = m_rows[(size_t)m_sel]; // the service calls below rebuild the rows
    if (!selectable(r)) return;
    if (m_cb.onActivate) m_cb.onActivate();
    switch (r.kind) {
        case Row::Dm:
        case Row::Channel:
            m_view = View::Chat;
            m_chat = r.channelId;
            m_chatName = r.text;
            m_chatIsChannel = r.kind == Row::Channel;
            m_chatAvatar = m_chatIsChannel ? std::string() : r.avatarUrl;
            m_chatStatus = m_chatIsChannel ? std::string() : r.status;
            m_chatScroll = 0.f;
            m_chatBuiltFor = 0;
            m_service->openChannel(r.channelId);
            break;
        case Row::Me:
            openAccounts();
            break;
        case Row::Account:
            m_service->switchAccount(r.accountId);
            m_view = View::Home;
            m_sel = -1;
            break;
        case Row::Button:
            if (r.action == 1) m_service->startQrLogin();
            if (r.action == 2) {
                m_service->signOut();
                m_view = View::Home;
                m_sel = -1;
            }
            if (r.action == 3) m_service->retry();
            break;
        default:
            break;
    }
    m_builtVersion = ~0ull;
}

std::string DiscordPanel::rowKey(const Row& r) const {
    switch (r.kind) {
        case Row::Dm:
        case Row::Channel: return "c" + std::to_string(r.channelId);
        case Row::Account: return "a" + r.accountId;
        case Row::Button: return "b" + std::to_string(r.action);
        case Row::Me: return "me";
        default: return {};
    }
}

// Names and messages often carry emoji or scripts the menu's font doesn't have:
// leave those letters out instead of drawing boxes.
std::string DiscordPanel::clean(const std::string& text) const {
    if (!m_font) return text;
    std::string out;
    out.reserve(text.size());
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        uint32_t cp = Str::NextCp(p, end);
        if (cp >= kMarkFirst && cp <= kMarkLast) { // an emoji picture
            Str::AppendCp(out, cp);
            continue;
        }
        if (switchu::dropFromScreen(cp)) continue; // marks typed apart, zalgo, zero-width
        bool ok = cp == ' ';
        if (!ok && cp >= 0x20 && cp != 0xFFFD) {
            auto it = m_glyphs.find(cp);
            if (it == m_glyphs.end()) it = m_glyphs.emplace(cp, m_font->hasGlyph(cp)).first;
            ok = it->second;
            if (!ok) { // no font has it: a fancy letter reads as the plain one
                const uint32_t plain = plainLetter(cp);
                if (plain) {
                    cp = plain;
                    ok = true;
                }
            }
        }
        if (!ok) continue;
        if (cp == ' ' && (out.empty() || out.back() == ' ')) continue; // no gaps left behind
        Str::AppendCp(out, cp);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

void DiscordPanel::rebuildRows() {
    if (!m_service) return;
    auto& i18n = nxui::I18n::instance();
    using S = DiscordService::State;
    const S st = m_service->state();
    const uint64_t version = m_service->version();
    if ((int)st == m_builtState && version == m_builtVersion && m_view == m_builtView && m_guild == m_builtGuild)
        return;
    m_builtState = (int)st;
    m_builtVersion = version;
    m_builtView = m_view;
    if (st == S::Ready) {
        m_servers = m_service->servers();
        if (m_guild && std::none_of(m_servers.begin(), m_servers.end(),
                                    [this](const auto& s) { return s.id == m_guild; }))
            m_guild = 0; // left that server
    }
    m_builtGuild = m_guild;
    if (!railShown()) m_column = 1;

    const std::string keep = (m_sel >= 0 && m_sel < (int)m_rows.size()) ? rowKey(m_rows[(size_t)m_sel]) : std::string();
    m_rows.clear();
    const float textW = (railShown() ? kPanelW - kRailW : kPanelW) - 32.f;
    auto info = [&](Row::Kind kind, const std::string& text) {
        Row r;
        r.kind = kind;
        r.lines = wrap(m_smallFont, text, textW, 0.78f, 5);
        m_rows.push_back(std::move(r));
    };
    auto button = [&](int action, const std::string& text, bool primary, bool danger) {
        Row r;
        r.kind = Row::Button;
        r.text = text;
        r.action = action;
        r.primary = primary;
        r.danger = danger;
        m_rows.push_back(std::move(r));
    };
    auto accountRows = [&](bool markCurrent) {
        for (const auto& acc : m_service->accounts()) {
            Row r;
            r.kind = Row::Account;
            r.text = clean(acc.name());
            if (r.text.empty()) r.text = clean(acc.username);
            r.detail = acc.username.empty() ? std::string() : "@" + clean(acc.username);
            r.avatarUrl = acc.avatarUrl();
            r.accountId = acc.id;
            r.current = markCurrent && acc.id == m_service->currentAccount();
            m_rows.push_back(std::move(r));
        }
    };

    if (st == S::QrLogin) {
        // Drawn as the QR page, no rows.
    } else if (m_view == View::Accounts) {
        info(Row::Info, i18n.tr("discord.accounts_body", "Switch between your Discord accounts, or add another one."));
        accountRows(true);
        button(1, i18n.tr("discord.add_account", "Add an account"), false, false);
        const std::string me = clean(m_service->myName());
        button(2, i18n.tr("discord.log_out", "Log out") + (me.empty() ? std::string() : " " + me), false, true);
    } else if (st == S::SignedOut || st == S::Failed) {
        if (!m_service->error().empty()) info(Row::Error, m_service->error());
        if (st == S::Failed) button(3, i18n.tr("discord.retry", "Try again"), true, false);
        accountRows(false);
        button(1, i18n.tr("discord.signin_qr", "Sign in with a QR code"), st != S::Failed, false);
        info(Row::Info, i18n.tr("discord.signin_body",
            "HomeNX also signs in with your SwitchCord accounts. Third-party Discord clients are "
            "against Discord's Terms of Service, so use this at your own risk."));
    } else {
        if (st == S::Connecting) {
            info(Row::Info, i18n.tr("discord.connecting", "Connecting..."));
        } else if (m_guild != 0) {
            // A server: its channels under their categories.
            for (const auto& c : m_service->channels(m_guild)) {
                Row r;
                r.kind = c.category ? Row::Category : Row::Channel;
                r.text = clean(c.name);
                if (c.category)
                    for (char& ch : r.text) ch = (char)std::toupper((unsigned char)ch);
                r.channelId = c.id;
                r.voice = c.voice;
                r.unread = c.unread;
                r.mentions = c.mentions;
                m_rows.push_back(std::move(r));
            }
            if (m_rows.empty())
                info(Row::Info, i18n.tr("discord.no_channels", "There are no channels here you can read."));
        } else {
            for (const auto& d : m_service->dms(40)) {
                Row r;
                r.kind = Row::Dm;
                r.text = clean(d.name);
                r.avatarUrl = d.avatarUrl;
                r.status = d.status;
                if (d.members > 0)
                    r.detail = std::to_string(d.members) + " " + i18n.tr("discord.members", "Members");
                else if (d.status != "offline")
                    r.detail = statusLabel(d.status);
                r.channelId = d.channelId;
                r.unread = d.unread;
                r.mentions = d.mentions;
                m_rows.push_back(std::move(r));
            }
            if (m_rows.empty()) info(Row::Info, i18n.tr("discord.no_dms", "No direct messages yet."));
        }
        Row me;
        me.kind = Row::Me;
        m_rows.push_back(std::move(me));
    }

    // Keep the selection on the same thing across refreshes, else the first one
    // (the sign-in button when nothing is saved).
    m_sel = -1;
    for (int i = 0; i < (int)m_rows.size(); ++i)
        if (!keep.empty() && rowKey(m_rows[(size_t)i]) == keep) m_sel = i;
    if (m_sel < 0)
        for (int i = 0; i < (int)m_rows.size(); ++i)
            if (selectable(m_rows[(size_t)i])) { m_sel = i; break; }
}

void DiscordPanel::rebuildChat() {
    if (!m_service) return;
    const uint64_t version = m_service->version();
    if (m_chatBuiltFor == m_chat && m_chatVersion == version) return;
    m_chatBuiltFor = m_chat;
    m_chatVersion = version;
    m_chatItems.clear();
    Snowflake prevAuthor = 0;
    int64_t prevMs = 0;
    for (const auto& l : m_service->lines(m_chat, 50)) {
        ChatItem it;
        // Like Discord: a run of messages from one person within a few minutes
        // shares one avatar and name.
        it.header = l.authorId != prevAuthor || l.ms - prevMs > 7 * 60 * 1000;
        it.author = clean(l.author);
        it.color = l.color;
        it.avatarUrl = l.avatarUrl;
        it.time = l.time;
        const std::string body = clean(l.text);
        it.emojis = l.emojis;
        it.media = l.media;
        // Only emoji (up to 27): big, like Discord.
        bool onlyEmoji = !l.emojis.empty() && l.emojis.size() <= 27;
        for (const char* p = body.data(), *end = p + body.size(); onlyEmoji && p < end;) {
            const uint32_t cp = Str::NextCp(p, end);
            if (cp != ' ' && (cp < kMarkFirst || cp > kMarkLast)) onlyEmoji = false;
        }
        it.bigEmoji = onlyEmoji;
        it.lines = wrapRich(m_font, body, kChatTextW, 0.66f, 12, onlyEmoji ? 40.f : 20.f);
        if (it.lines.empty() && it.media.empty()) it.lines.push_back(std::string());
        it.tone = l.failed ? 2 : l.pending ? 1 : 0;
        float mediaH = 0.f;
        for (const auto& md : it.media) mediaH += mediaSize(md).y + 8.f;
        it.height = (it.header ? 40.f : 2.f) + it.lines.size() * (onlyEmoji ? 46.f : 21.f) + mediaH + 2.f;
        prevAuthor = l.authorId;
        prevMs = l.ms;
        m_chatItems.push_back(std::move(it));
    }
}

void DiscordPanel::onUpdate(float dt) {
    const float target = m_active ? 1.f : 0.f;
    if (m_anim != target) {
        const float step = dt / kAnimDur;
        m_anim = m_anim < target ? std::min(target, m_anim + step) : std::max(target, m_anim - step);
        if (m_anim <= 0.f && !m_active) setVisible(false);
    }
    if (!m_active || !m_service) return;
    const auto st = m_service->state();
    // Signed out or switching accounts: the chat or account list no longer applies.
    if (m_view == View::Chat && st != DiscordService::State::Ready) m_view = View::Home;
    if (m_view == View::Accounts && st != DiscordService::State::Ready && st != DiscordService::State::Connecting)
        m_view = View::Home;
    if (m_view == View::Chat) rebuildChat();
    else rebuildRows();
}

nxui::Rect DiscordPanel::panelRect() const {
    const float closedX = -kPanelW - 10.f;
    const float x = closedX + (kPanelX - closedX) * nxui::Easing::outCubic(m_anim);
    return {x, kPanelY, kPanelW, kPanelH};
}

// Profile pictures load in the background; until one arrives (or if it can't),
// drawAvatar shows a coloured circle with the first letter.
const nxui::Texture* DiscordPanel::avatar(const std::string& url) {
    if (url.empty() || !m_gpu || !m_ren) return nullptr;
    auto it = m_avatars.find(url);
    if (it != m_avatars.end()) return it->second.get();
    if (m_avatars.size() >= 160) return nullptr; // ponytail: no eviction, plenty for a DM list
    m_avatars.emplace(url, nullptr);
    std::weak_ptr<bool> alive = m_alive;
    Async::Run(Async::Pool::Images, [this, url, alive]() {
        Http::Request req;
        req.url = url;
        req.timeoutMs = 15000;
        req.maxBytes = 2u << 20;
        Http::Response res = Http::Perform(req);
        std::string body = res.Ok() ? std::move(res.body) : std::string();
        Async::Post([this, url, alive, body = std::move(body)]() {
            if (alive.expired() || body.empty()) return;
            auto tex = std::make_unique<nxui::Texture>();
            if (tex->loadFromMemory(*m_gpu, *m_ren, reinterpret_cast<const uint8_t*>(body.data()), body.size(), 64))
                m_avatars[url] = std::move(tex);
        });
    });
    return nullptr;
}

// Images, GIFs and emoji: downloaded and decoded on the image workers (a GIF's
// frames packed into one texture), uploaded here. Oldest go when over budget.
const DiscordPanel::Picture* DiscordPanel::picture(const std::string& url, const std::string& fallback,
                                                   bool animated, int maxW, int maxH) {
    if (url.empty() || !m_gpu || !m_ren) return nullptr;
    auto it = m_pictures.find(url);
    if (it != m_pictures.end()) return it->second.get();
    while (m_pictureOrder.size() > 300 || (m_pictureBytes > kPictureBudget && !m_pictureOrder.empty())) {
        auto old = m_pictures.find(m_pictureOrder.front());
        if (old != m_pictures.end()) {
            if (old->second) m_pictureBytes -= std::min(m_pictureBytes, old->second->bytes);
            m_pictures.erase(old);
        }
        m_pictureOrder.erase(m_pictureOrder.begin());
    }
    m_pictures.emplace(url, nullptr);
    m_pictureOrder.push_back(url);
    std::weak_ptr<bool> alive = m_alive;
    Async::Run(Async::Pool::Images, [this, url, fallback, animated, maxW, maxH, alive]() {
        auto d = std::make_shared<Decoded>();
        for (const std::string& u : {url, fallback}) {
            if (u.empty()) continue;
            Http::Request req;
            req.url = u;
            req.timeoutMs = 20000;
            req.maxBytes = 8u << 20;
            Http::Response res = Http::Perform(req);
            if (res.Ok() && decodePicture(res.body, animated && u == url, maxW, maxH, *d)) break;
            *d = Decoded{};
        }
        Async::Post([this, url, d, alive]() {
            if (alive.expired() || d->rgba.empty()) return;
            auto slot = m_pictures.find(url);
            if (slot == m_pictures.end()) return; // dropped meanwhile
            const std::size_t bytes = d->rgba.size();
            if (m_gpu->imageMemoryAvailable() < bytes + (16u << 20)) return; // leave room for HOME
            auto pic = std::make_unique<Picture>();
            if (!pic->tex.loadFromPixels(*m_gpu, *m_ren, d->rgba.data(), d->w, d->h)) return;
            pic->frames = std::move(d->frames);
            pic->delays = std::move(d->delays);
            for (int ms : pic->delays) pic->totalMs += ms;
            pic->start = armGetSystemTick();
            pic->bytes = bytes;
            m_pictureBytes += bytes;
            slot->second = std::move(pic);
        });
    });
    return nullptr;
}

void DiscordPanel::drawPicture(nxui::Renderer& ren, const Picture& pic, const nxui::Rect& r, float a, float radius) {
    const nxui::Color tint = nxui::Color::white().withAlpha(a);
    if (pic.frames.empty() || pic.totalMs <= 0) {
        ren.drawTextureRounded(&pic.tex, r, radius, tint);
        return;
    }
    int t = static_cast<int>(armTicksToNs(armGetSystemTick() - pic.start) / 1'000'000ULL % (uint64_t)pic.totalMs);
    size_t f = 0;
    while (f + 1 < pic.frames.size() && t >= pic.delays[f]) t -= pic.delays[f++];
    ren.drawTextureSubRounded(&pic.tex, pic.frames[f], r, radius, tint);
}

// Text with emoji marks: the words measured by the font, each emoji a square.
float DiscordPanel::richWidth(nxui::Font* font, const std::string& s, float scale, float emoji) const {
    float w = 0.f;
    std::string seg;
    for (const char* p = s.data(), *end = p + s.size(); p < end;) {
        const char* start = p;
        const uint32_t cp = Str::NextCp(p, end);
        if (cp >= kMarkFirst && cp <= kMarkLast) {
            if (!seg.empty()) w += font->measure(seg).x * scale;
            seg.clear();
            w += emoji + 2.f;
        } else {
            seg.append(start, p);
        }
    }
    if (!seg.empty()) w += font->measure(seg).x * scale;
    return w;
}

std::vector<std::string> DiscordPanel::wrapRich(nxui::Font* font, const std::string& text, float maxW, float scale,
                                                int maxLines, float emoji) const {
    std::vector<std::string> lines;
    if (!font) return lines;
    auto fitRich = [&](std::string t) {
        if (richWidth(font, t, scale, emoji) <= maxW) return t;
        while (!t.empty()) {
            while (!t.empty()) {
                const unsigned char c = static_cast<unsigned char>(t.back());
                t.pop_back();
                if ((c & 0xC0) != 0x80) break;
            }
            if (richWidth(font, t + "...", scale, emoji) <= maxW) return t + "...";
        }
        return std::string("...");
    };
    std::string line;
    size_t i = 0;
    while (i <= text.size()) {
        const size_t sp = text.find(' ', i);
        const std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (richWidth(font, candidate, scale, emoji) <= maxW || line.empty()) {
            line = candidate;
        } else {
            lines.push_back(fitRich(line));
            line = word;
            if ((int)lines.size() == maxLines - 1) {
                lines.push_back(fitRich(text.substr(i)));
                return lines;
            }
        }
        if (sp == std::string::npos) break;
        i = sp + 1;
    }
    if (!line.empty()) lines.push_back(fitRich(line));
    return lines;
}

void DiscordPanel::drawRich(nxui::Renderer& ren, const std::string& s, float x, float y, nxui::Font* font,
                            const nxui::Color& color, float scale, const std::vector<std::string>& emojis,
                            float emoji) {
    std::string seg;
    auto flush = [&]() {
        if (seg.empty()) return;
        ren.drawText(seg, {x, y + (emoji > 30.f ? 12.f : 0.f)}, font, color, scale);
        x += font->measure(seg).x * scale;
        seg.clear();
    };
    for (const char* p = s.data(), *end = p + s.size(); p < end;) {
        const char* start = p;
        const uint32_t cp = Str::NextCp(p, end);
        if (cp < kMarkFirst || cp > kMarkLast) {
            seg.append(start, p);
            continue;
        }
        flush();
        const size_t idx = cp - kMarkFirst;
        if (idx < emojis.size()) {
            const std::string& url = emojis[idx];
            const bool animated = url.find(".gif") != std::string::npos;
            if (const Picture* pic = picture(url, std::string(), animated, 72, 72))
                drawPicture(ren, *pic, {x + 1.f, y - (emoji > 30.f ? 2.f : 1.f), emoji, emoji}, color.a, 0.f);
        }
        x += emoji + 2.f;
    }
    flush();
}

void DiscordPanel::drawAvatar(nxui::Renderer& ren, const std::string& url, const std::string& name,
                              float x, float y, float size, float a) {
    if (const nxui::Texture* t = avatar(url); t && t->valid()) {
        ren.drawTextureRounded(t, {x, y, size, size}, size * 0.5f, nxui::Color::white().withAlpha(a));
        return;
    }
    static const int kColors[5][3] = {{88, 101, 242}, {116, 127, 141}, {59, 165, 93}, {250, 166, 26}, {237, 66, 69}};
    const auto* c = kColors[std::hash<std::string>{}(name) % 5];
    ren.drawCircle({x + size * 0.5f, y + size * 0.5f}, size * 0.5f, rgb(c[0], c[1], c[2], a), 32);
    if (name.empty() || !m_font) return;
    const char* p = name.data();
    uint32_t cp = Str::NextCp(p, name.data() + name.size());
    if (cp >= 'a' && cp <= 'z') cp -= 32;
    std::string letter;
    Str::AppendCp(letter, cp);
    const float s = size / 48.f;
    const nxui::Vec2 m = m_font->measure(letter);
    ren.drawText(letter, {x + (size - m.x * s) * 0.5f, y + (size - m.y * s) * 0.5f}, m_font,
                 nxui::Color(1.f, 1.f, 1.f, a), s);
}

// Discord's status badges: a green dot, a yellow moon, a red "no entry" and a
// grey ring, each cut out of the background behind the avatar.
void DiscordPanel::drawStatus(nxui::Renderer& ren, const std::string& s, float cx, float cy, float r,
                              const nxui::Color& ring, float a) {
    ren.drawCircle({cx, cy}, r + 3.f, ring, 24);
    if (s == "online") {
        ren.drawCircle({cx, cy}, r, green(a), 24);
    } else if (s == "idle") {
        ren.drawCircle({cx, cy}, r, rgb(240, 178, 50, a), 24);
        ren.drawCircle({cx - r * 0.45f, cy - r * 0.45f}, r * 0.62f, ring, 20);
    } else if (s == "dnd") {
        ren.drawCircle({cx, cy}, r, red(a), 24);
        ren.drawRoundedRect({cx - r * 0.6f, cy - r * 0.2f, r * 1.2f, r * 0.4f}, ring, r * 0.2f);
    } else {
        ren.drawCircle({cx, cy}, r, rgb(128, 132, 142, a), 24);
        ren.drawCircle({cx, cy}, r * 0.5f, ring, 20);
    }
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
    ren.drawRoundedRect(area, nxui::Color(1.f, 1.f, 1.f, alpha), 10.f);
    if (m_qrSize <= 0) return;
    const float quiet = 3.f;
    const float cell = std::floor(area.width / (m_qrSize + quiet * 2.f));
    const float side = cell * m_qrSize;
    const float ox = area.x + std::floor((area.width - side) * 0.5f), oy = area.y + std::floor((area.height - side) * 0.5f);
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

float DiscordPanel::rowHeight(const Row& r) const {
    switch (r.kind) {
        case Row::Dm: return 50.f;
        case Row::Account: return 58.f;
        case Row::Button: return 54.f;
        case Row::Channel: return 40.f;
        case Row::Category: return 36.f;
        case Row::Info:
        case Row::Error: return 22.f * r.lines.size() + 14.f;
        default: return 0.f; // Me is drawn at the bottom
    }
}

void DiscordPanel::drawTopBar(nxui::Renderer& ren, const nxui::Rect& p, float a) {
    auto& i18n = nxui::I18n::instance();
    using S = DiscordService::State;
    const S st = m_service ? m_service->state() : S::Failed;
    const bool qr = st == S::QrLogin;
    const bool withBack = qr || m_view != View::Home;
    // With the server bar the title sits over the list column.
    const float bx = railShown() ? p.x + kRailW : p.x;
    const float bw = railShown() ? kPanelW - kRailW : kPanelW;
    m_backRect = withBack ? nxui::Rect{bx, p.y, 64.f, kTopH} : nxui::Rect{};

    float tx = bx + 18.f;
    if (withBack) {
        const float cx = bx + 26.f, cy = p.y + kTopH * 0.5f;
        ren.drawLine({cx + 4.f, cy - 8.f}, {cx - 4.f, cy}, textMuted(a), 2.5f);
        ren.drawLine({cx - 4.f, cy}, {cx + 4.f, cy + 8.f}, textMuted(a), 2.5f);
        tx = bx + 46.f;
    }

    std::string title, hint;
    if (qr) {
        title = i18n.tr("discord.qr_title", "Log in with QR Code");
        hint = i18n.tr("discord.hint_cancel", "B  Cancel");
    } else if (m_view == View::Chat) {
        if (m_chatIsChannel) { // a server channel: "# name"
            ren.drawText("#", {tx + 4.f, p.y + 14.f}, m_font, textMuted(a), 0.9f);
            tx += 26.f;
        } else {
            drawAvatar(ren, m_chatAvatar, m_chatName, tx, p.y + 14.f, 28.f, a);
            if (!m_chatStatus.empty())
                drawStatus(ren, m_chatStatus, tx + 23.f, p.y + 37.f, 4.5f, bgPrimary(a), a);
            tx += 38.f;
        }
        title = m_chatName;
        hint = i18n.tr("discord.hint_reply", "X  Reply");
    } else if (m_view == View::Accounts) {
        title = i18n.tr("discord.accounts_title", "Switch Accounts");
        hint = i18n.tr("discord.hint_back", "B  Back");
    } else if (railShown()) {
        title = i18n.tr("discord.dms", "Direct Messages");
        for (const auto& sv : m_servers)
            if (sv.id == m_guild) title = clean(sv.name);
        hint = i18n.tr("discord.hint_home", "Y  Accounts");
    } else {
        if (m_logo.valid())
            ren.drawTexture(&m_logo, {tx, p.y + 14.f, 28.f, 28.f}, nxui::Color::white().withAlpha(a));
        else
            ren.drawCircle({tx + 14.f, p.y + 28.f}, 14.f, blurple(a), 28);
        tx += 40.f;
        title = "Discord";
        hint = i18n.tr("discord.hint_close", "B  Close");
    }
    const float hintW = m_smallFont->measure(hint).x * 0.72f;
    ren.drawText(hint, {bx + bw - 18.f - hintW, p.y + 19.f}, m_smallFont, textMuted(a), 0.72f);
    ren.drawText(fit(m_font, title, bx + bw - 30.f - hintW - tx, 0.78f), {tx, p.y + 16.f}, m_font,
                 textHeader(a), 0.78f);
    // Discord's header edge: a dark line and a soft shadow.
    ren.drawRect({bx, p.y + kTopH - 1.f, bw, 1.f}, rgb(31, 32, 35, a));
    ren.drawGradientRect({bx, p.y + kTopH, bw, 5.f}, nxui::Color(0.f, 0.f, 0.f, 0.22f * a),
                         nxui::Color(0.f, 0.f, 0.f, 0.f));
}

// Discord's server bar: DMs (the logo) on top, a divider, then your servers.
// The open one has a tall white pill and a rounded-square icon; unread ones a
// short pill; mentions a red count.
void DiscordPanel::drawRail(nxui::Renderer& ren, const nxui::Rect& area, float a) {
    m_railHits.clear();
    auto itemY = [](int i) { return 12.f + i * 56.f + (i >= 1 ? 10.f : 0.f); };
    int sel = 0;
    for (size_t i = 0; i < m_servers.size(); ++i)
        if (m_servers[i].id == m_guild) sel = (int)i + 1;
    // Keep the open server in view.
    const float total = itemY((int)m_servers.size()) + kRailIcon + 12.f;
    const float selY = itemY(sel);
    if (selY - m_railScroll < 8.f) m_railScroll = selY - 8.f;
    if (selY + kRailIcon - m_railScroll > area.height - 8.f) m_railScroll = selY + kRailIcon - area.height + 8.f;
    m_railScroll = std::clamp(m_railScroll, 0.f, std::max(0.f, total - area.height));

    ren.pushClipRect(area);
    const float x = area.x + (kRailW - kRailIcon) * 0.5f;
    for (int i = 0; i <= (int)m_servers.size(); ++i) {
        const float y = area.y + itemY(i) - m_railScroll;
        if (y + kRailIcon < area.y || y > area.y + area.height) continue;
        const nxui::Rect icon{x, y, kRailIcon, kRailIcon};
        m_railHits.push_back({i, icon});
        const bool open = i == sel;
        const float radius = open ? 16.f : kRailIcon * 0.5f;
        bool unread = false;
        int mentions = 0;
        if (i == 0) {
            mentions = m_service ? m_service->dmMentions() : 0;
            ren.drawRoundedRect(icon, open ? blurple(a) : bgPrimary(a), radius);
            if (m_mark.valid())
                ren.drawTexture(&m_mark, {x + 10.f, y + 10.f, kRailIcon - 20.f, kRailIcon - 20.f},
                                nxui::Color::white().withAlpha(a));
        } else {
            const auto& sv = m_servers[(size_t)i - 1];
            unread = sv.unread;
            mentions = sv.mentions;
            if (const nxui::Texture* t = avatar(sv.iconUrl); t && t->valid()) {
                ren.drawTextureRounded(t, icon, radius, nxui::Color::white().withAlpha(a));
            } else {
                ren.drawRoundedRect(icon, open ? blurple(a) : bgPrimary(a), radius);
                const std::string ini = fit(m_smallFont, clean(sv.initials), kRailIcon - 6.f, 0.74f);
                const nxui::Vec2 m = m_smallFont->measure(ini);
                ren.drawText(ini, {x + (kRailIcon - m.x * 0.74f) * 0.5f, y + (kRailIcon - m.y * 0.74f) * 0.5f},
                             m_smallFont, textHeader(a), 0.74f);
            }
        }
        if (open || unread) { // the white pill at the edge
            const float ph = open ? 40.f : 8.f;
            ren.drawRoundedRect({area.x, y + (kRailIcon - ph) * 0.5f, 4.f, ph}, nxui::Color(1.f, 1.f, 1.f, a), 2.f);
        }
        if (mentions > 0) {
            const std::string n = mentions > 99 ? "99+" : std::to_string(mentions);
            const float nw = m_smallFont->measure(n).x * 0.62f;
            const float bw = std::max(18.f, nw + 10.f);
            const nxui::Rect badge{x + kRailIcon - bw + 4.f, y + kRailIcon - 14.f, bw, 18.f};
            ren.drawRoundedRect({badge.x - 3.f, badge.y - 3.f, badge.width + 6.f, badge.height + 6.f}, bgTertiary(a), 12.f);
            ren.drawRoundedRect(badge, red(a), 9.f);
            ren.drawText(n, {badge.x + (bw - nw) * 0.5f, badge.y + 1.f}, m_smallFont, nxui::Color(1.f, 1.f, 1.f, a), 0.62f);
        }
        if (open && m_column == 0) // the controller is on the bar: ring the open server
            ren.drawRoundedRectOutline({icon.x - 4.f, icon.y - 4.f, icon.width + 8.f, icon.height + 8.f},
                                       nxui::Color(1.f, 1.f, 1.f, 0.9f * a), radius + 4.f, 2.5f);
        if (i == 0) // the divider under DMs
            ren.drawRect({area.x + (kRailW - 32.f) * 0.5f, y + kRailIcon + 4.f, 32.f, 2.f}, rgb(53, 54, 60, a));
    }
    ren.popClipRect();
}

void DiscordPanel::drawRows(nxui::Renderer& ren, const nxui::Rect& area, float a) {
    auto& i18n = nxui::I18n::instance();
    using S = DiscordService::State;
    const S st = m_service ? m_service->state() : S::Failed;
    float top = area.y + 8.f;

    // Signed out: a welcome above the sign-in choices.
    if (m_view == View::Home && (st == S::SignedOut || st == S::Failed)) {
        const float cx = area.x + kPanelW * 0.5f;
        const float logoY = area.y + 22.f;
        if (m_logo.valid())
            ren.drawTexture(&m_logo, {cx - 36.f, logoY, 72.f, 72.f}, nxui::Color::white().withAlpha(a));
        else
            ren.drawCircle({cx, logoY + 36.f}, 36.f, blurple(a), 40);
        const std::string hi = i18n.tr("discord.welcome", "Welcome to Discord");
        ren.drawText(hi, {cx - m_font->measure(hi).x * 0.45f, logoY + 84.f}, m_font, textHeader(a), 0.9f);
        const std::string sub = i18n.tr("discord.welcome_sub", "Sign in to see your DMs and who's online.");
        ren.drawText(sub, {cx - m_smallFont->measure(sub).x * 0.38f, logoY + 118.f}, m_smallFont,
                     textMuted(a), 0.76f);
        top = area.y + kWelcomeH;
    }

    // Keep the selected row in view (unless a finger is scrolling).
    float total = 0.f, selTop = 0.f, selH = 0.f;
    for (int i = 0; i < (int)m_rows.size(); ++i) {
        const float h = rowHeight(m_rows[(size_t)i]);
        if (i == m_sel) { selTop = total; selH = h; }
        total += h;
    }
    const float viewH = area.y + area.height - 8.f - top;
    if (m_followSel && selH > 0.f) {
        if (selTop - m_scroll < 0.f) m_scroll = selTop;
        if (selTop + selH - m_scroll > viewH) m_scroll = selTop + selH - viewH;
    }
    m_scroll = std::clamp(m_scroll, 0.f, std::max(0.f, total - viewH));

    const nxui::Rect clip{area.x, top - 4.f, area.width, area.y + area.height - top + 4.f};
    ren.pushClipRect(clip);
    float y = top - m_scroll;
    const float x = area.x + 8.f, w = area.width - 16.f;
    for (int i = 0; i < (int)m_rows.size(); ++i) {
        const Row& r = m_rows[(size_t)i];
        const float h = rowHeight(r);
        if (h <= 0.f) continue;
        const nxui::Rect row{x, y, w, h};
        y += h;
        if (row.y + h < clip.y || row.y > clip.y + clip.height) continue;
        const bool sel = i == m_sel && listFocused();
        if (selectable(r)) m_hits.push_back({i, row});

        if (r.kind == Row::Info || r.kind == Row::Error) {
            float ly = row.y + 6.f;
            for (const auto& line : r.lines) {
                ren.drawText(line, {x + 8.f, ly}, m_smallFont, r.kind == Row::Error ? red(a) : textMuted(a), 0.78f);
                ly += 22.f;
            }
            continue;
        }
        if (r.kind == Row::Category) { // a server's category: small caps-style label
            ren.drawText(fit(m_smallFont, r.text, w - 16.f, 0.66f), {x + 6.f, row.y + 14.f}, m_smallFont,
                         textMuted(a), 0.66f);
            continue;
        }
        if (r.kind == Row::Channel) {
            if (sel) ren.drawRoundedRect({x, row.y + 2.f, w, h - 4.f}, bgSelected(a), 6.f);
            const float cy = row.y + h * 0.5f;
            const nxui::Color ic = (sel || r.unread) ? textHeader(a) : textMuted(a);
            if (r.voice) { // a speaker for voice channels (their text chat opens)
                ren.drawRect({x + 11.f, cy - 4.f, 5.f, 8.f}, ic);
                ren.drawTriangle({x + 16.f, cy - 4.f}, {x + 23.f, cy - 10.f}, {x + 23.f, cy + 10.f}, ic);
                ren.drawTriangle({x + 16.f, cy - 4.f}, {x + 23.f, cy + 10.f}, {x + 16.f, cy + 4.f}, ic);
            } else {
                ren.drawText("#", {x + 11.f, cy - 12.f}, m_font, ic, 0.78f);
            }
            const float tw = w - 44.f - (r.mentions > 0 ? 36.f : 0.f);
            ren.drawText(fit(m_font, r.text, tw, 0.7f), {x + 34.f, cy - 11.f}, m_font, ic, 0.7f);
            if (r.mentions > 0) {
                const std::string n = r.mentions > 99 ? "99+" : std::to_string(r.mentions);
                const float nw = m_smallFont->measure(n).x * 0.7f;
                const float bw = std::max(20.f, nw + 12.f);
                ren.drawRoundedRect({x + w - 10.f - bw, cy - 10.f, bw, 20.f}, red(a), 10.f);
                ren.drawText(n, {x + w - 10.f - bw + (bw - nw) * 0.5f, cy - 8.f}, m_smallFont,
                             nxui::Color(1.f, 1.f, 1.f, a), 0.7f);
            } else if (r.unread) {
                ren.drawRoundedRect({area.x, cy - 4.f, 4.f, 8.f}, nxui::Color(1.f, 1.f, 1.f, a), 2.f);
            }
            continue;
        }
        if (r.kind == Row::Button && r.primary) {
            const nxui::Rect b{x + 8.f, row.y + 6.f, w - 16.f, h - 12.f};
            ren.drawRoundedRect(b, sel ? blurpleDark(a) : blurple(a), 5.f);
            if (sel) ren.drawRoundedRectOutline(b, nxui::Color(1.f, 1.f, 1.f, 0.85f * a), 5.f, 2.f);
            const float tw = m_font->measure(r.text).x * 0.72f;
            ren.drawText(r.text, {b.x + (b.width - tw) * 0.5f, b.y + 10.f}, m_font, nxui::Color(1.f, 1.f, 1.f, a), 0.72f);
            continue;
        }

        const nxui::Color rowBg = sel ? bgSelected(a) : bgSecondary(a);
        if (sel) ren.drawRoundedRect(row, rowBg, 6.f);
        const float av = r.kind == Row::Account ? 38.f : 34.f;
        const float ax = x + 8.f, ay = row.y + (h - av) * 0.5f;
        if (r.kind == Row::Button) {
            ren.drawCircle({ax + av * 0.5f, ay + av * 0.5f}, av * 0.5f, bgTertiary(a), 32);
            if (r.action == 1) { // a plus
                ren.drawRect({ax + av * 0.5f - 8.f, ay + av * 0.5f - 1.25f, 16.f, 2.5f}, green(a));
                ren.drawRect({ax + av * 0.5f - 1.25f, ay + av * 0.5f - 8.f, 2.5f, 16.f}, green(a));
            } else { // an arrow out of a door
                ren.drawLine({ax + 11.f, ay + av * 0.5f}, {ax + 25.f, ay + av * 0.5f}, red(a), 2.5f);
                ren.drawLine({ax + 20.f, ay + av * 0.5f - 5.f}, {ax + 25.f, ay + av * 0.5f}, red(a), 2.5f);
                ren.drawLine({ax + 20.f, ay + av * 0.5f + 5.f}, {ax + 25.f, ay + av * 0.5f}, red(a), 2.5f);
            }
            const nxui::Color col = r.danger ? red(a) : (sel ? textHeader(a) : textNormal(a));
            ren.drawText(fit(m_font, r.text, w - av - 32.f, 0.72f), {ax + av + 12.f, row.y + (h - 20.f) * 0.5f},
                         m_font, col, 0.72f);
            continue;
        }

        drawAvatar(ren, r.avatarUrl, r.text, ax, ay, av, a);
        if (!r.status.empty()) drawStatus(ren, r.status, ax + av - 5.f, ay + av - 5.f, 5.5f, rowBg, a);
        const float tx = ax + av + 12.f;
        const float badgeW = r.mentions > 0 ? 40.f : (r.current ? 36.f : 8.f);
        const float textW = x + w - tx - badgeW;
        const bool bright = sel || r.unread || r.kind == Row::Account;
        const nxui::Color nameCol = bright ? textHeader(a) : textMuted(a);
        if (r.detail.empty()) {
            ren.drawText(fit(m_font, r.text, textW, 0.72f), {tx, row.y + (h - 20.f) * 0.5f}, m_font, nameCol, 0.72f);
        } else {
            ren.drawText(fit(m_font, r.text, textW, 0.72f), {tx, row.y + h * 0.5f - 21.f}, m_font, nameCol, 0.72f);
            ren.drawText(fit(m_smallFont, r.detail, textW, 0.7f), {tx, row.y + h * 0.5f + 2.f}, m_smallFont,
                         textMuted(a), 0.7f);
        }
        const float cy = row.y + h * 0.5f;
        if (r.mentions > 0) {
            const std::string n = r.mentions > 99 ? "99+" : std::to_string(r.mentions);
            const float nw = m_smallFont->measure(n).x * 0.7f;
            const float bw = std::max(20.f, nw + 12.f);
            ren.drawRoundedRect({x + w - 12.f - bw, cy - 10.f, bw, 20.f}, red(a), 10.f);
            ren.drawText(n, {x + w - 12.f - bw + (bw - nw) * 0.5f, cy - 8.f}, m_smallFont,
                         nxui::Color(1.f, 1.f, 1.f, a), 0.7f);
        } else if (r.unread) { // Discord's white pill at the edge
            ren.drawRoundedRect({area.x, cy - 4.f, 4.f, 8.f}, nxui::Color(1.f, 1.f, 1.f, a), 2.f);
        }
        if (r.current) { // the account in use: a green tick
            const nxui::Vec2 c{x + w - 22.f, cy};
            ren.drawCircle(c, 11.f, green(a), 24);
            ren.drawLine({c.x - 5.f, c.y + 0.5f}, {c.x - 1.5f, c.y + 4.f}, nxui::Color(1.f, 1.f, 1.f, a), 2.5f);
            ren.drawLine({c.x - 1.5f, c.y + 4.f}, {c.x + 5.5f, c.y - 4.f}, nxui::Color(1.f, 1.f, 1.f, a), 2.5f);
        }
    }
    ren.popClipRect();
}

// Your account, pinned to the bottom like Discord's user panel.
void DiscordPanel::drawMe(nxui::Renderer& ren, const nxui::Rect& area, float a) {
    auto& i18n = nxui::I18n::instance();
    ren.drawRoundedRect(area, bgUser(a), 14.f);
    ren.drawRect({area.x, area.y, area.width, 16.f}, bgUser(a)); // square top corners
    int meIndex = -1;
    for (int i = 0; i < (int)m_rows.size(); ++i)
        if (m_rows[(size_t)i].kind == Row::Me) meIndex = i;
    const bool sel = meIndex >= 0 && meIndex == m_sel && listFocused();
    const nxui::Rect inner{area.x + 8.f, area.y + 7.f, area.width - 16.f, area.height - 14.f};
    if (meIndex >= 0) m_hits.push_back({meIndex, inner});
    if (sel) ren.drawRoundedRect(inner, bgSelected(a), 6.f);

    const bool ready = m_service && m_service->state() == DiscordService::State::Ready;
    std::string name = ready ? clean(m_service->myName()) : std::string();
    if (name.empty() && ready) name = clean(m_service->myUsername());
    const std::string status = ready ? m_service->myStatus() : std::string("offline");
    const float av = 36.f, ax = inner.x + 8.f, ay = inner.y + (inner.height - av) * 0.5f;
    drawAvatar(ren, ready ? m_service->myAvatarUrl() : std::string(), name, ax, ay, av, a);
    drawStatus(ren, status, ax + av - 5.f, ay + av - 5.f, 5.5f, sel ? bgSelected(a) : bgUser(a), a);

    const std::string chip = i18n.tr("discord.switch", "Switch");
    const float chipW = m_smallFont->measure(chip).x * 0.68f + 34.f;
    const nxui::Rect c{inner.x + inner.width - chipW - 8.f, inner.y + (inner.height - 26.f) * 0.5f, chipW, 26.f};
    ren.drawRoundedRect(c, bgSecondary(a), 13.f);
    ren.drawText("Y", {c.x + 10.f, c.y + 5.f}, m_smallFont, textHeader(a), 0.68f);
    ren.drawText(chip, {c.x + 24.f, c.y + 5.f}, m_smallFont, textMuted(a), 0.68f);

    const float tx = ax + av + 10.f, tw = c.x - tx - 8.f;
    ren.drawText(fit(m_font, ready ? name : i18n.tr("discord.connecting", "Connecting..."), tw, 0.68f),
                 {tx, inner.y + 6.f}, m_font, textHeader(a), 0.68f);
    ren.drawText(fit(m_smallFont, ready ? statusLabel(status) : std::string(), tw, 0.66f),
                 {tx, inner.y + 28.f}, m_smallFont, textMuted(a), 0.66f);
}

void DiscordPanel::drawChat(nxui::Renderer& ren, const nxui::Rect& area, float a) {
    auto& i18n = nxui::I18n::instance();
    float total = 16.f;
    for (const auto& it : m_chatItems) total += it.height;
    m_chatScroll = std::clamp(m_chatScroll, 0.f, std::max(0.f, total - area.height));

    if (m_chatItems.empty()) {
        // The start of a conversation, as Discord shows it.
        std::string heading = m_chatName, start;
        if (m_chatIsChannel) {
            ren.drawCircle({area.x + 54.f, area.y + 58.f}, 36.f, bgInput(a), 40);
            ren.drawText("#", {area.x + 40.f, area.y + 34.f}, m_font, textHeader(a), 1.5f);
            heading = i18n.tr("discord.channel_welcome", "Welcome to") + " #" + m_chatName + "!";
            start = i18n.tr("discord.channel_start", "This is the start of the") + " #" + m_chatName + " "
                  + i18n.tr("discord.channel_word", "channel.");
        } else {
            drawAvatar(ren, m_chatAvatar, m_chatName, area.x + 18.f, area.y + 22.f, 72.f, a);
            start = i18n.tr("discord.dm_start", "This is the beginning of your direct message history with") + " "
                  + m_chatName + ".";
        }
        ren.drawText(fit(m_font, heading, kPanelW - 40.f, 1.0f), {area.x + 18.f, area.y + 106.f}, m_font,
                     textHeader(a), 1.0f);
        const bool loading = m_service && m_service->loading(m_chat);
        const std::string body = loading ? i18n.tr("discord.loading", "Loading...") : start;
        float ly = area.y + 144.f;
        for (const auto& line : wrap(m_smallFont, body, kPanelW - 40.f, 0.78f, 3)) {
            ren.drawText(line, {area.x + 18.f, ly}, m_smallFont, textMuted(a), 0.78f);
            ly += 22.f;
        }
    }

    ren.pushClipRect(area);
    float y = area.y + area.height - 8.f + m_chatScroll; // newest at the bottom
    for (auto it = m_chatItems.rbegin(); it != m_chatItems.rend(); ++it) {
        y -= it->height;
        if (y > area.y + area.height) continue;
        if (y + it->height < area.y) break;
        float ly = y + 2.f;
        if (it->header) {
            drawAvatar(ren, it->avatarUrl, it->author, area.x + 14.f, y + 10.f, 36.f, a);
            const std::string name = fit(m_font, it->author, kChatTextW - 110.f, 0.7f);
            // Role colour in servers, like Discord.
            const nxui::Color nameCol = it->color ? rgb((int)((it->color >> 16) & 0xFF), (int)((it->color >> 8) & 0xFF),
                                                        (int)(it->color & 0xFF), a)
                                                  : textHeader(a);
            ren.drawText(name, {area.x + kChatTextX, y + 8.f}, m_font, nameCol, 0.7f);
            const float nw = m_font->measure(name).x * 0.7f;
            ren.drawText(it->time, {area.x + kChatTextX + nw + 8.f, y + 12.f}, m_smallFont, textMuted(a), 0.62f);
            ly = y + 34.f;
        }
        const nxui::Color col = it->tone == 2 ? red(a) : it->tone == 1 ? textMuted(a) : textNormal(a);
        for (const auto& line : it->lines) {
            drawRich(ren, line, area.x + kChatTextX, ly, m_font, col, 0.66f, it->emojis, it->bigEmoji ? 40.f : 20.f);
            ly += it->bigEmoji ? 46.f : 21.f;
        }
        for (const auto& md : it->media) {
            const nxui::Vec2 sz = mediaSize(md);
            const nxui::Rect box{area.x + kChatTextX, ly + 4.f, sz.x, sz.y};
            if (const Picture* pic = picture(md.url, md.fallback, md.animated, 300, 220))
                drawPicture(ren, *pic, box, a, 8.f);
            else
                ren.drawRoundedRect(box, bgInput(a), 8.f);
            ly += sz.y + 8.f;
        }
    }
    ren.popClipRect();

    // The message bar: X (or a tap) opens the keyboard.
    m_inputRect = {area.x + 12.f, area.y + area.height + 8.f, kPanelW - 24.f, kInputH - 20.f};
    ren.drawRoundedRect(m_inputRect, bgInput(a), 8.f);
    const std::string hint = i18n.tr("discord.message", "Message") + (m_chatIsChannel ? " #" : " @") + m_chatName;
    ren.drawText(fit(m_smallFont, hint, m_inputRect.width - 64.f, 0.76f), {m_inputRect.x + 16.f, m_inputRect.y + 10.f},
                 m_smallFont, textMuted(a), 0.76f);
    const nxui::Rect key{m_inputRect.x + m_inputRect.width - 36.f, m_inputRect.y + 8.f, 24.f, 24.f};
    ren.drawRoundedRect(key, bgSecondary(a), 12.f);
    ren.drawText("X", {key.x + 7.f, key.y + 3.f}, m_smallFont, textHeader(a), 0.7f);
}

void DiscordPanel::drawQrView(nxui::Renderer& ren, const nxui::Rect& area, float a) {
    auto& i18n = nxui::I18n::instance();
    const std::string url = m_service->qrUrl();
    const float side = 250.f;
    const nxui::Rect box{area.x + (kPanelW - side) * 0.5f, area.y + 28.f, side, side};
    drawQr(ren, url, box, a);
    if (url.empty()) {
        const std::string wait = i18n.tr("discord.qr_wait", "Getting a code...");
        ren.drawText(wait, {box.x + (side - m_smallFont->measure(wait).x * 0.8f) * 0.5f, box.y + side * 0.5f - 10.f},
                     m_smallFont, rgb(30, 31, 34, a), 0.8f);
    }
    float y = box.y + side + 24.f;
    for (const auto& line : wrap(m_font, i18n.tr("discord.qr_body",
             "Scan this with the Discord mobile app to log in instantly."), kPanelW - 48.f, 0.74f, 3)) {
        ren.drawText(line, {area.x + (kPanelW - m_font->measure(line).x * 0.74f) * 0.5f, y}, m_font,
                     textHeader(a), 0.74f);
        y += 25.f;
    }
    y += 8.f;
    for (const auto& line : wrap(m_smallFont, m_service->qrStatus(), kPanelW - 48.f, 0.76f, 4)) {
        ren.drawText(line, {area.x + (kPanelW - m_smallFont->measure(line).x * 0.76f) * 0.5f, y}, m_smallFont,
                     textMuted(a), 0.76f);
        y += 22.f;
    }
}

void DiscordPanel::onRender(nxui::Renderer& ren) {
    if (m_anim <= 0.001f || !m_font || !m_smallFont || !m_service) return;
    if (!m_logoTried && m_gpu && m_ren && !m_logoPath.empty()) {
        m_logoTried = true;
        m_logo.loadFromFile(*m_gpu, *m_ren, m_logoPath, 96);
    }
    if (!m_markTried && m_gpu && m_ren && !m_logoPath.empty()) {
        m_markTried = true; // the white logo next to discord.png
        std::string mark = m_logoPath;
        const size_t dot = mark.rfind(".png");
        if (dot != std::string::npos) mark.insert(dot, "_mark");
        m_mark.loadFromFile(*m_gpu, *m_ren, mark, 96);
    }
    const float a = m_anim;
    m_hits.clear();
    m_inputRect = {};

    ren.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.35f * a));
    const nxui::Rect p = panelRect();
    ren.drawRoundedRect(p, m_view == View::Chat ? bgPrimary(a) : bgSecondary(a), 14.f);
    ren.drawRoundedRectOutline(p, bgTertiary(a), 14.f, 1.f);

    using S = DiscordService::State;
    const S st = m_service->state();
    const nxui::Rect body{p.x, p.y + kTopH, kPanelW, kPanelH - kTopH};
    if (st == S::QrLogin) {
        drawQrView(ren, body, a);
    } else if (m_view == View::Chat) {
        drawChat(ren, {body.x, body.y, body.width, body.height - kInputH}, a);
    } else if (railShown()) {
        // The server bar down the left, the DMs or channels beside it.
        ren.drawRoundedRect({p.x, p.y, kRailW + 16.f, kPanelH - kMeH}, bgTertiary(a), 14.f);
        ren.drawRect({p.x + kRailW, p.y, 16.f, kPanelH - kMeH}, bgSecondary(a));
        drawRail(ren, {p.x, p.y, kRailW, kPanelH - kMeH}, a);
        drawRows(ren, {p.x + kRailW, body.y, kPanelW - kRailW, body.height - kMeH}, a);
        drawMe(ren, {p.x, p.y + kPanelH - kMeH, kPanelW, kMeH}, a);
    } else {
        drawRows(ren, body, a);
    }
    drawTopBar(ren, p, a); // last: its shadow falls over the list
}

void DiscordPanel::handleTouch(const nxui::Input& in) {
    if (!m_active || !m_service) return;
    const float tx = in.touchX(), ty = in.touchY();
    if (in.touchDown()) {
        if (tx > kPanelX + kPanelW) { // a tap beside the panel closes it
            hide();
            return;
        }
        m_touchDragging = false;
        m_touchScrollStart = m_view == View::Chat ? m_chatScroll : m_scroll;
    }
    if (!in.isTouching() && !in.touchUp()) return;
    if (in.touchStartX() > kPanelX + kPanelW) return;
    if (std::abs(in.touchDeltaY()) > 14.f) m_touchDragging = true;
    if (m_touchDragging) {
        if (m_view == View::Chat) {
            m_chatScroll = std::max(0.f, m_touchScrollStart + in.touchDeltaY());
        } else {
            m_scroll = std::max(0.f, m_touchScrollStart - in.touchDeltaY());
            m_followSel = false;
        }
        return;
    }
    if (!in.touchUp()) return;
    if (m_backRect.width > 0.f && inside(m_backRect, tx, ty)) {
        back();
        return;
    }
    if (m_view == View::Chat && m_inputRect.width > 0.f && inside(m_inputRect, tx, ty)) {
        if (m_chat && m_cb.onReply) m_cb.onReply(m_chat, m_chatName);
        return;
    }
    if (railShown())
        for (const auto& [i, r] : m_railHits)
            if (inside(r, tx, ty)) { // a tap on a server opens its channels
                const Snowflake guild = i == 0 ? 0 : m_servers[(size_t)i - 1].id;
                if (guild != m_guild) {
                    m_guild = guild;
                    m_sel = -1;
                    m_scroll = 0.f;
                    m_builtVersion = ~0ull;
                }
                m_column = 1;
                if (m_cb.onNavigate) m_cb.onNavigate();
                return;
            }
    for (const auto& [i, r] : m_hits)
        if (inside(r, tx, ty)) {
            m_sel = i;
            m_column = 1;
            activateRow();
            return;
        }
}
