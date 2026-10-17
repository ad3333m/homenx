// HomeNX Discord: your Discord DMs and servers on top of a running game.
//
// A Tesla/Ultrahand overlay. L+ZL (HomeNX adds that combo to Ultrahand's
// overlays.ini) or B shows and hides it, only over a game: never over HOME.
// Hidden, it stays loaded, so your "Playing <game>" status stays up. − closes
// it, and so does Ultrahand's own combo while it's hidden (that opens Ultrahand).
//
// It uses the accounts HomeNX's Discord panel signed in with
// (sdmc:/config/SwitchU/discord.json, then SwitchCord's accounts.json; Y
// switches between them): Discord's REST API for DMs, servers, channels and
// messages, and a small gateway connection for who's online, what they're
// doing, typing, new messages, and your own game status.
//
// Looks like Discord: the server bar on the left (icons), DMs with profile
// pictures and status dots, channels under their categories, and chats with
// avatars, times, replies, reactions, emoji, images, GIFs and stickers. You
// can write with emoji, accents and GIFs. Everything is drawn into the
// overlay's 448x720 RGBA4444 framebuffer: pictures are decoded small and
// dithered on the network thread, within about 1 MB, because an overlay only
// has a few MB in all (running out of it ends in std::abort).
//
// SPDX-License-Identifier: GPL-2.0-or-later (built with libtesla, GPL-2.0)

#include <bit> // libtesla uses std::bit_cast without including it

#define TESLA_INIT_IMPL
#include <tesla.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#include <stb_image.h>

#include <curl/curl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <switchu/fancy_text.hpp>
#include <yyjson.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr const char* kApi = "https://discord.com/api/v9";
constexpr const char* kGatewayHost = "gateway.discord.gg";
constexpr const char* kSelfPath = "sdmc:/switch/.overlays/homenx-discord.ovl";
constexpr const char* kChoicePath = "sdmc:/config/SwitchU/discord_overlay_account.txt";
constexpr const char* kUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36";
// Discord shows a status's picture only through a Discord application. Its ID
// comes from sdmc:/config/SwitchU/discord_app_id.txt, else this; without one
// the status still says the game and "Nintendo Switch", with no picture.
constexpr const char* kGameAppId = "";
constexpr u64 kCombo = HidNpadButton_L | HidNpadButton_ZL;
constexpr u64 kHomeMenu = 0x010000000000100DULL; // HomeNX's HOME menu
constexpr s32 kW = 448, kH = 720;               // the overlay's framebuffer
constexpr s32 kRailW = 64, kPanelH = 52;
// Memory: decoded pictures kept, download sizes, and how big a GIF may animate.
constexpr std::size_t kPictureBudget = 1000u * 1024u;
constexpr std::size_t kPictureBody = 1024u * 1024u, kApiBody = 1536u * 1024u;
constexpr int kGifFrames = 6, kGifPixels = 140000;

// Discord's dark theme, as close as 4 bits a channel get.
const tsl::Color kBg(0x3, 0x3, 0x3, 0xF);       // #313338 chat, #2B2D31 lists
const tsl::Color kRailBg(0x2, 0x2, 0x2, 0xF);   // #1E1F22 server bar
const tsl::Color kPanelBg(0x2, 0x2, 0x2, 0xF);  // #232428 your panel
const tsl::Color kInput(0x4, 0x4, 0x4, 0xF);    // #383A40 message box
const tsl::Color kSelected(0x4, 0x4, 0x5, 0xF); // #404249
const tsl::Color kText(0xD, 0xD, 0xE, 0xF);     // #DBDEE1
const tsl::Color kWhite(0xF, 0xF, 0xF, 0xF);    // #F2F3F5 names, titles
const tsl::Color kMuted(0x9, 0x9, 0xA, 0xF);    // #949BA4
const tsl::Color kChannel(0x8, 0x8, 0x9, 0xF);  // #80848E
const tsl::Color kBlurple(0x5, 0x6, 0xF, 0xF);  // #5865F2
const tsl::Color kMine(0x3, 0x3, 0x6, 0xF);     // your reactions
const tsl::Color kError(0xF, 0x6, 0x6, 0xF);
const tsl::Color kOnline(0x2, 0xA, 0x5, 0xF);   // #23A55A
const tsl::Color kIdle(0xF, 0xB, 0x3, 0xF);     // #F0B232
const tsl::Color kDnd(0xF, 0x4, 0x4, 0xF);      // #F23F43

u64 nowMs() { return armTicksToNs(armGetSystemTick()) / 1'000'000ULL; }

// ---------------------------------------------------------------- JSON

struct Json {
    yyjson_doc* doc;
    explicit Json(const std::string& s) : doc(yyjson_read(s.data(), s.size(), 0)) {}
    ~Json() { if (doc) yyjson_doc_free(doc); }
    Json(const Json&) = delete;
    Json& operator=(const Json&) = delete;
    yyjson_val* root() const { return doc ? yyjson_doc_get_root(doc) : nullptr; }
};

std::string str(yyjson_val* o, const char* key) {
    yyjson_val* v = yyjson_obj_get(o, key);
    return yyjson_is_str(v) ? std::string(yyjson_get_str(v)) : std::string();
}

long num(yyjson_val* o, const char* key) {
    yyjson_val* v = yyjson_obj_get(o, key);
    return yyjson_is_int(v) ? static_cast<long>(yyjson_get_sint(v)) : 0;
}

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\u%04x", c);
            out += b;
        } else out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string urlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char b[4];
            std::snprintf(b, sizeof(b), "%%%02X", c);
            out += b;
        }
    }
    return out;
}

// Snowflakes as strings: longer is newer, then by digits.
bool newer(const std::string& a, const std::string& b) {
    return a.size() != b.size() ? a.size() > b.size() : a > b;
}

unsigned long long snowflake(const std::string& id) { return std::strtoull(id.c_str(), nullptr, 10); }

std::string readFile(const char* path) {
    std::string s;
    FILE* f = std::fopen(path, "rb");
    if (!f) return s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

std::string trimmed(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    return s.substr(a);
}

// ---------------------------------------------------------------- text

uint32_t nextCp(const char*& p, const char* end) {
    const unsigned char c = static_cast<unsigned char>(*p++);
    if (c < 0x80) return c;
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
    if (!extra) return 0xFFFD;
    uint32_t cp = extra == 1 ? (c & 0x1Fu) : extra == 2 ? (c & 0x0Fu) : (c & 0x07u);
    while (extra-- > 0 && p < end && (static_cast<unsigned char>(*p) & 0xC0) == 0x80)
        cp = (cp << 6) | (static_cast<unsigned char>(*p++) & 0x3Fu);
    return cp;
}

void appendCp(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

std::vector<uint32_t> codepoints(const std::string& s) {
    std::vector<uint32_t> cps;
    for (const char* p = s.data(), *end = p + s.size(); p < end;) cps.push_back(nextCp(p, end));
    return cps;
}

void popChar(std::string& s) {
    while (!s.empty()) {
        const unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;
    }
}

uint32_t lastCp(const std::string& s) {
    if (s.empty()) return 0;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    const char* p = s.data() + i;
    return nextCp(p, s.data() + s.size());
}

constexpr uint32_t kMarkFirst = 0xF0000; // emoji pictures: U+F0000 + index
constexpr uint32_t kMarkLast = 0xFFFFD;

bool isMark(uint32_t c) { return c >= kMarkFirst && c <= kMarkLast; }

bool emojiStart(uint32_t c) {
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x2B05 && c <= 0x2B55)
        || c == 0x203C || c == 0x2049 || (c >= 0x2194 && c <= 0x21AA) || c == 0x231A || c == 0x231B
        || c == 0x2328 || c == 0x23CF || (c >= 0x23E9 && c <= 0x23FA) || c == 0x24C2 || c == 0x25AA
        || c == 0x25AB || c == 0x25B6 || c == 0x25C0 || (c >= 0x25FB && c <= 0x25FE) || c == 0x2934
        || c == 0x2935 || c == 0x3030 || c == 0x303D || c == 0x3297 || c == 0x3299;
}

bool emojiModifier(uint32_t c) {
    return c == 0xFE0F || c == 0x20E3 || (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F);
}

// Removes the last thing typed: a whole emoji (with its skin tone, family
// joiners or flag pair) or a whole server emoji tag, not half of one.
void popToken(std::string& s) {
    if (!s.empty() && s.back() == '>') {
        const size_t open = s.rfind('<');
        if (open != std::string::npos && (s.compare(open, 2, "<:") == 0 || s.compare(open, 3, "<a:") == 0)) {
            s.erase(open);
            return;
        }
    }
    for (;;) {
        uint32_t c;
        do {
            c = lastCp(s);
            popChar(s);
        } while (emojiModifier(c) && !s.empty());
        if (c >= 0x1F1E6 && c <= 0x1F1FF) {
            const uint32_t before = lastCp(s);
            if (before >= 0x1F1E6 && before <= 0x1F1FF) popChar(s);
        }
        if (lastCp(s) != 0x200D) return;
        popChar(s); // a joiner: the emoji before it goes too
    }
}

// Text as the overlay can draw it. Letters no console font has become the
// plain ones they're styled after (switchu::plainLetter); what can't be drawn
// at all (marks typed apart, zero-width characters, scripts with no font) is
// left out instead of showing as boxes. Emoji marks stay.
std::string forScreen(const std::string& s) {
    tsl::gfx::Renderer& r = tsl::gfx::Renderer::instance();
    std::string out;
    out.reserve(s.size());
    for (const char* p = s.data(), *end = p + s.size(); p < end;) {
        uint32_t c = nextCp(p, end);
        if (isMark(c) || c == '\n' || c == ' ') {
            appendCp(out, c);
            continue;
        }
        if (c < 0x20 || switchu::dropFromScreen(c)) continue;
        if (!r.hasGlyph(c)) {
            c = switchu::plainLetter(c);
            if (!c) continue;
        }
        appendCp(out, c);
    }
    return out;
}

// Names: no pictures (emoji are left out), fancy letters plain.
std::string cleanName(const std::string& s) {
    std::string kept;
    for (const char* p = s.data(), *end = p + s.size(); p < end;) {
        const uint32_t c = nextCp(p, end);
        if ((emojiStart(c) && !switchu::plainLetter(c)) || emojiModifier(c)) continue;
        appendCp(kept, c);
    }
    return trimmed(forScreen(kept));
}

// Message text with emoji marks taken out (for one-line previews).
std::string withoutMarks(const std::string& s) {
    std::string out;
    for (const char* p = s.data(), *end = p + s.size(); p < end;) {
        const uint32_t c = nextCp(p, end);
        if (!isMark(c)) appendCp(out, c == '\n' ? ' ' : c);
    }
    return trimmed(out);
}

std::string twemoji(const std::vector<uint32_t>& seq) {
    const bool zwj = std::find(seq.begin(), seq.end(), 0x200D) != seq.end();
    std::string name;
    for (uint32_t cp : seq) {
        if (cp == 0xFE0F && !zwj) continue; // Twemoji's file naming
        char hex[12];
        std::snprintf(hex, sizeof(hex), "%s%x", name.empty() ? "" : "-", static_cast<unsigned>(cp));
        name += hex;
    }
    return "https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/" + name + ".png";
}

std::string customEmojiUrl(const std::string& id) { return "https://cdn.discordapp.com/emojis/" + id + ".png?size=48"; }

void mark(std::string& out, std::vector<std::string>& emojis, std::string url) {
    if (emojis.size() >= 200) return;
    appendCp(out, kMarkFirst + static_cast<uint32_t>(emojis.size()));
    emojis.push_back(std::move(url));
}

// Unicode emoji (skin tones, ZWJ families, flags, keycaps) as Twemoji marks,
// then the text made drawable. Styled squared letters stay letters unless
// they're written as emoji (with U+FE0F).
std::string markUnicodeEmoji(const std::string& tags, std::vector<std::string>& emojis) {
    const std::vector<uint32_t> cps = codepoints(tags);
    std::string out;
    for (size_t i = 0; i < cps.size();) {
        const uint32_t c = cps[i];
        const bool styledLetter = switchu::plainLetter(c) && !(i + 1 < cps.size() && cps[i + 1] == 0xFE0F);
        const bool keycap = ((c >= '0' && c <= '9') || c == '#' || c == '*') && i + 1 < cps.size()
            && (cps[i + 1] == 0x20E3 || (cps[i + 1] == 0xFE0F && i + 2 < cps.size() && cps[i + 2] == 0x20E3));
        if (styledLetter || (!emojiStart(c) && !keycap)) {
            appendCp(out, c);
            ++i;
            continue;
        }
        std::vector<uint32_t> seq{c};
        size_t j = i + 1;
        if (c >= 0x1F1E6 && c <= 0x1F1FF && j < cps.size() && cps[j] >= 0x1F1E6 && cps[j] <= 0x1F1FF)
            seq.push_back(cps[j++]);
        for (;;) {
            if (j < cps.size() && emojiModifier(cps[j])) {
                seq.push_back(cps[j++]);
            } else if (j + 1 < cps.size() && cps[j] == 0x200D) {
                seq.push_back(cps[j++]);
                seq.push_back(cps[j++]);
            } else {
                break;
            }
        }
        mark(out, emojis, twemoji(seq));
        i = j;
    }
    return forScreen(out);
}

struct Media {
    std::string url, fallback;
    int w = 0, h = 0;
    bool animated = false;
};

// A Discord emoji or sticker link: how emoji and stickers from other servers
// are sent (without Nitro, or by "fake Nitro" plugins). Its id, and whether it moves.
bool discordPicture(const std::string& url, const char* kind, std::string& id, bool& animated) {
    for (const char* host : {"https://cdn.discordapp.com/", "https://media.discordapp.net/"}) {
        const std::string prefix = std::string(host) + kind + "/";
        if (url.compare(0, prefix.size(), prefix) != 0) continue;
        size_t q = prefix.size();
        while (q < url.size() && std::isdigit(static_cast<unsigned char>(url[q]))) ++q;
        if (q == prefix.size() || q - prefix.size() > 20) return false;
        id = url.substr(prefix.size(), q - prefix.size());
        animated = url.compare(q, 4, ".gif") == 0 || url.find("animated=true", q) != std::string::npos;
        return true;
    }
    return false;
}

bool isDiscordPicture(const std::string& url) {
    std::string id;
    bool animated = false;
    return discordPicture(url, "emojis", id, animated) || discordPicture(url, "stickers", id, animated);
}

Media stickerMedia(const std::string& id, bool animated) {
    Media md;
    md.w = md.h = 120;
    md.animated = animated;
    md.fallback = "https://media.discordapp.net/stickers/" + id + ".png?size=160";
    md.url = animated ? "https://media.discordapp.net/stickers/" + id + ".gif?size=160" : md.fallback;
    return md;
}

// Message text for the chat: mentions, channels and timestamps in words,
// server emoji, Unicode emoji and emoji links as picture marks (sticker links
// go to `stickers`: the link, and the id in it, never show), Markdown markers
// removed, fancy letters plain.
std::string messageText(const std::string& s, yyjson_val* msg, std::vector<std::string>& emojis,
                        std::vector<Media>* stickers) {
    std::string tags;
    auto pictureLink = [&](const std::string& url) {
        std::string id;
        bool animated = false;
        if (discordPicture(url, "emojis", id, animated)) {
            mark(tags, emojis, customEmojiUrl(id));
            return true;
        }
        if (!discordPicture(url, "stickers", id, animated)) return false;
        if (stickers && stickers->size() < 2) stickers->push_back(stickerMedia(id, animated));
        return true;
    };
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '[') { // [name](emoji link), as "fake Nitro" sends them
            const size_t mid = s.find("](", i);
            const size_t close = mid == std::string::npos ? mid : s.find(')', mid + 2);
            if (close != std::string::npos && mid - i < 80 && pictureLink(s.substr(mid + 2, close - mid - 2))) {
                i = close + 1;
                continue;
            }
        }
        const bool angled = s.compare(i, 9, "<https://") == 0;
        if (angled || s.compare(i, 8, "https://") == 0) {
            size_t end = angled ? s.find('>', i) : s.find_first_of(" \n\t", i);
            if (end == std::string::npos) end = s.size();
            const size_t from = i + (angled ? 1 : 0);
            if (pictureLink(s.substr(from, end - from))) {
                i = angled ? end + 1 : end;
                continue;
            }
        }
        if (s[i] == '<') {
            const size_t end = s.find('>', i);
            if (end != std::string::npos && end - i < 96) {
                const std::string tag = s.substr(i + 1, end - i - 1);
                std::string repl;
                bool done = false;
                if (tag.rfind("@&", 0) == 0) {
                    repl = "@role";
                } else if (tag.rfind("@", 0) == 0) {
                    const std::string id = tag.substr(tag.size() > 1 && tag[1] == '!' ? 2 : 1);
                    repl = "@user";
                    size_t k, n;
                    yyjson_val* u;
                    yyjson_arr_foreach(yyjson_obj_get(msg, "mentions"), k, n, u)
                        if (str(u, "id") == id) {
                            std::string nm = str(u, "global_name");
                            repl = "@" + (nm.empty() ? str(u, "username") : nm);
                        }
                } else if (tag.rfind("#", 0) == 0) {
                    repl = "#channel";
                } else if (tag.rfind(":", 0) == 0 || tag.rfind("a:", 0) == 0) {
                    const size_t a = tag.find(':'), b = tag.find(':', a + 1);
                    if (b != std::string::npos) {
                        mark(tags, emojis, customEmojiUrl(tag.substr(b + 1)));
                        done = true;
                    }
                } else if (tag.rfind("t:", 0) == 0) {
                    repl = "(a time)";
                }
                if (done || !repl.empty()) {
                    tags += repl;
                    i = end + 1;
                    continue;
                }
            }
        }
        // Markdown markers: **, __, ~~, ||, ` and line-start #, ##, ###, -#, >
        if (i == 0 || s[i - 1] == '\n') {
            bool cut = false;
            for (const char* prefix : {"### ", "## ", "# ", "-# ", "> "})
                if (s.compare(i, std::strlen(prefix), prefix) == 0) {
                    i += std::strlen(prefix);
                    cut = true;
                    break;
                }
            if (cut) continue;
        }
        if (i + 1 < s.size() && ((s[i] == '*' && s[i + 1] == '*') || (s[i] == '_' && s[i + 1] == '_')
                                 || (s[i] == '~' && s[i + 1] == '~') || (s[i] == '|' && s[i + 1] == '|'))) {
            i += 2;
            continue;
        }
        if (s[i] == '`') {
            ++i;
            continue;
        }
        tags += s[i++];
    }
    return markUnicodeEmoji(tags, emojis);
}

// What's being typed, with its emoji as pictures (nothing else changed).
std::string draftText(const std::string& s, std::vector<std::string>& emojis) {
    std::string tags;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '<' && (s.compare(i, 2, "<:") == 0 || s.compare(i, 3, "<a:") == 0)) {
            const size_t end = s.find('>', i);
            const std::string tag = end == std::string::npos ? std::string() : s.substr(i + 1, end - i - 1);
            const size_t b = tag.rfind(':');
            if (b != std::string::npos && b > 0) {
                mark(tags, emojis, customEmojiUrl(tag.substr(b + 1)));
                i = end + 1;
                continue;
            }
        }
        tags += s[i++];
    }
    return markUnicodeEmoji(tags, emojis);
}

std::string avatarUrl(const std::string& id, const std::string& hash) {
    if (!hash.empty()) return "https://cdn.discordapp.com/avatars/" + id + "/" + hash + ".png?size=128";
    return "https://cdn.discordapp.com/embed/avatars/" + std::to_string((snowflake(id) >> 22) % 6) + ".png";
}

std::string avatarUrl(yyjson_val* user) { return avatarUrl(str(user, "id"), str(user, "avatar")); }

// Discord's media proxy shrinks a picture (and makes PNG of WebP) for us.
std::string sized(const std::string& url, int w, int h) {
    if (url.empty()) return url;
    std::string out = url + (url.find('?') == std::string::npos ? "?" : "&") + "format=png";
    if (w > 0 && h > 0) {
        const float s = std::min(1.f, std::min(260.f / w, 160.f / h));
        out += "&width=" + std::to_string(std::max(1, static_cast<int>(w * s)))
             + "&height=" + std::to_string(std::max(1, static_cast<int>(h * s)));
    }
    return out;
}

// Tenor's small GIF of the same clip: .../<id><5-letter format>/<name>.<ext>,
// AAAAM being the 220 px "tinygif" (the full one is megabytes).
std::string tenorTiny(const std::string& v) {
    const size_t host = v.find("media.tenor.com/");
    if (host == std::string::npos) return {};
    const size_t slash = v.find('/', host + 16);
    const size_t dot = v.rfind('.');
    if (slash == std::string::npos || slash < host + 21 || dot == std::string::npos || dot < slash) return {};
    return v.substr(0, slash - 5) + "AAAAM" + v.substr(slash, dot - slash) + ".gif";
}

// Tenor and Giphy "GIFs" are MP4s; both also serve a small real GIF.
std::string gifOf(yyjson_val* embed) {
    std::string v = str(yyjson_obj_get(embed, "video"), "url");
    if (v.empty()) v = str(yyjson_obj_get(embed, "thumbnail"), "url");
    if (v.find("giphy.com/media/") != std::string::npos) {
        const size_t slash = v.rfind('/');
        if (slash != std::string::npos) return v.substr(0, slash) + "/200w.gif";
    }
    return tenorTiny(v);
}

// ---------------------------------------------------------------- accounts

struct Account {
    std::string id, token, name, avatar;
};

// The signed-in accounts: HomeNX's (never one signed out in HomeNX), then
// SwitchCord's. `current` is the one to use: the overlay's own last choice,
// else HomeNX's current account, else the first.
std::vector<Account> loadAccounts(size_t& current) {
    constexpr const char* kHomeNX = "sdmc:/config/SwitchU/discord.json";
    constexpr const char* kSwitchCord = "sdmc:/switch/SwitchCord/accounts.json";
    std::string homeCurrent;
    std::vector<std::string> signedOut;
    std::vector<Account> all;
    for (const char* path : {kHomeNX, kSwitchCord}) {
        Json doc(readFile(path));
        yyjson_val* root = doc.root();
        if (!root) continue;
        size_t i, n;
        yyjson_val* v;
        if (path == kHomeNX) {
            homeCurrent = str(root, "current");
            yyjson_arr_foreach(yyjson_obj_get(root, "signed_out"), i, n, v)
                if (yyjson_is_str(v)) signedOut.push_back(yyjson_get_str(v));
            if (!str(root, "token").empty()) all.push_back({"", str(root, "token"), "", ""});
        }
        yyjson_arr_foreach(yyjson_obj_get(root, "accounts"), i, n, v) {
            Account a{str(v, "id"), str(v, "token"), std::string(), std::string()};
            if (a.token.empty()) continue;
            if (!a.id.empty() && std::find(signedOut.begin(), signedOut.end(), a.id) != signedOut.end()) continue;
            if (std::any_of(all.begin(), all.end(), [&](const Account& b) {
                    return b.token == a.token || (!a.id.empty() && b.id == a.id);
                }))
                continue;
            const std::string nm = str(v, "global_name");
            a.name = cleanName(nm.empty() ? str(v, "username") : nm);
            if (!a.id.empty()) a.avatar = avatarUrl(a.id, str(v, "avatar"));
            all.push_back(std::move(a));
        }
    }
    current = 0;
    const std::string chosen = trimmed(readFile(kChoicePath));
    for (const std::string& want : {chosen, homeCurrent})
        for (size_t i = 0; !want.empty() && i < all.size(); ++i)
            if (all[i].id == want) {
                current = i;
                return all;
            }
    return all;
}

// ---------------------------------------------------------------- addresses

std::atomic<bool> g_lookupsWork{true};

// Where a host is. The console answers an overlay's own lookups with "busy,
// try again", so after one failed try this uses the addresses HOME saved
// (sdmc:/config/SwitchU/discord_hosts.txt, "host address" lines; HOME's lookups
// work), then Discord's long-standing Cloudflare addresses for Discord's hosts.
// Only the address comes from here: TLS still checks the host's certificate.
std::vector<std::string> addressesOf(const std::string& host) {
    std::vector<std::string> ips;
    if (g_lookupsWork) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), "443", &hints, &res) == 0 && res) {
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, ip, sizeof(ip));
            ips.push_back(ip);
        } else {
            g_lookupsWork = false; // don't wait on it again
        }
        if (res) freeaddrinfo(res);
    }
    auto valid = [](const std::string& ip) {
        in_addr check{};
        return inet_pton(AF_INET, ip.c_str(), &check) == 1;
    };
    const std::string saved = readFile("sdmc:/config/SwitchU/discord_hosts.txt");
    for (size_t pos = 0; pos < saved.size();) {
        size_t nl = saved.find('\n', pos);
        if (nl == std::string::npos) nl = saved.size();
        const std::string line = trimmed(saved.substr(pos, nl - pos));
        pos = nl + 1;
        const size_t sp = line.find(' ');
        if (sp == host.size() && line.compare(0, sp, host) == 0) {
            const std::string ip = trimmed(line.substr(sp + 1));
            if (valid(ip)) ips.push_back(ip);
        }
    }
    if (host == "discord.com") { // an earlier HOME saved just this one
        const std::string old = trimmed(readFile("sdmc:/config/SwitchU/discord_ip.txt"));
        if (valid(old)) ips.push_back(old);
    }
    auto endsWith = [&](const char* tail) {
        const size_t n = std::strlen(tail);
        return host.size() >= n && host.compare(host.size() - n, n, tail) == 0;
    };
    if (endsWith("discord.gg"))
        for (const char* ip : {"162.159.130.234", "162.159.133.234", "162.159.134.234", "162.159.135.234",
                               "162.159.136.234"})
            ips.push_back(ip);
    if (endsWith("discord.com") || endsWith("discordapp.com") || endsWith("discordapp.net"))
        for (const char* ip : {"162.159.128.233", "162.159.135.232", "162.159.136.232",
                               "162.159.137.232", "162.159.138.232"})
            ips.push_back(ip);
    return ips;
}

// ---------------------------------------------------------------- pictures

struct Picture {
    int w = 0, h = 0, frames = 1;
    std::vector<u16> px;     // RGBA4444, one frame after another
    std::vector<int> delays; // ms per frame
    int totalMs = 0;
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
        if (b == 0x21) {
            ++i;
            while (i < n && p[i]) i += p[i] + 1u;
            ++i;
        } else if (b == 0x2C) {
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

// Area-average shrink of RGBA8 into RGBA4444. 4 bits a channel shows steps in
// pictures, so stills get error diffusion (Floyd-Steinberg) and animation a
// fixed pattern (so frames don't shimmer). Round ones (avatars, server icons)
// get a smooth circular edge.
void shrinkInto(const uint8_t* src, int sw, int sh, u16* dst, int dw, int dh, bool round, bool animated) {
    static const uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const size_t rowLen = static_cast<size_t>(dw + 2) * 3;
    std::vector<float> carry(animated ? 0 : rowLen * 2, 0.f); // this row's and the next row's error
    const float cx = dw * 0.5f, cy = dh * 0.5f, rr = std::min(dw, dh) * 0.5f;
    for (int y = 0; y < dh; ++y) {
        float* cur = animated ? nullptr : carry.data() + (y & 1) * rowLen;
        float* next = animated ? nullptr : carry.data() + ((y + 1) & 1) * rowLen;
        if (next) std::fill(next, next + rowLen, 0.f);
        const int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; ++x) {
            const int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            unsigned sum[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* px = src + ((size_t)yy * sw + xx) * 4;
                    sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2]; sum[3] += px[3];
                    ++n;
                }
            float a = static_cast<float>(sum[3]) / n;
            if (round) {
                const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                a *= std::clamp(rr - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.f, 1.f);
            }
            u8 q[3];
            for (int c = 0; c < 3; ++c) {
                const float v = static_cast<float>(sum[c]) / n;
                if (animated) {
                    q[c] = static_cast<u8>(std::min(15.f, v * (15.f / 255.f) + (kBayer[y & 3][x & 3] + 0.5f) / 16.f));
                    continue;
                }
                const float want = v + cur[(x + 1) * 3 + c];
                const int level = std::clamp(static_cast<int>(std::lround(want * 15.f / 255.f)), 0, 15);
                q[c] = static_cast<u8>(level);
                if (a < 8.f) continue; // nothing shows here: no error to spread
                const float e = want - level * 17.f;
                cur[(x + 2) * 3 + c] += e * (7.f / 16.f);
                next[x * 3 + c] += e * (3.f / 16.f);
                next[(x + 1) * 3 + c] += e * (5.f / 16.f);
                next[(x + 2) * 3 + c] += e * (1.f / 16.f);
            }
            dst[(size_t)y * dw + x] =
                tsl::Color(q[0], q[1], q[2], static_cast<u8>(std::lround(a * 15.f / 255.f))).rgba;
        }
    }
}

void fitInto(int w, int h, int maxW, int maxH, Picture& out) {
    const float s = std::min(1.f, std::min((float)maxW / w, (float)maxH / h));
    out.w = std::max(1, (int)(w * s));
    out.h = std::max(1, (int)(h * s));
}

// A GIF animated, decoded a frame at a time (stb's own loader holds every
// frame at once, which an overlay hasn't the memory for). Keeps up to
// kGifFrames frames spread over the clip.
bool decodeGif(const std::string& body, int maxW, int maxH, bool round, Picture& out) {
    int gw = 0, gh = 0, count = 0;
    if (!gifInfo(body, gw, gh, count) || count < 2 || gw * gh > kGifPixels) return false;
    struct Gif { // stb's GIF state holds a 32 KB table: on the heap, not the stack
        stbi__gif g{};
        ~Gif() {
            STBI_FREE(g.out);
            STBI_FREE(g.history);
            STBI_FREE(g.background);
        }
    };
    std::unique_ptr<Gif> gif(new Gif());
    stbi__context s;
    stbi__start_mem(&s, reinterpret_cast<const stbi_uc*>(body.data()), static_cast<int>(body.size()));
    fitInto(gw, gh, maxW, maxH, out);
    const int keep = std::min(count, kGifFrames);
    out.px.assign((size_t)out.w * out.h * keep, 0);
    int comp = 0, frame = 0, kept = -1;
    for (;;) {
        stbi_uc* u = stbi__gif_load_next(&s, &gif->g, &comp, 4, nullptr);
        if (!u || u == reinterpret_cast<stbi_uc*>(&s)) break; // done (or broken)
        if (gif->g.w != gw || gif->g.h != gh) return false;
        const int k = std::min(keep - 1, frame * keep / count);
        if (k != kept) {
            shrinkInto(u, gw, gh, out.px.data() + (size_t)k * out.w * out.h, out.w, out.h, round, true);
            out.delays.push_back(0);
            kept = k;
        }
        out.delays.back() += gif->g.delay > 10 ? gif->g.delay : 100;
        ++frame;
    }
    if (kept < 0) return false;
    out.frames = kept + 1;
    out.px.resize((size_t)out.w * out.h * out.frames);
    out.totalMs = 0;
    for (int d : out.delays) out.totalMs += d;
    return true;
}

// A picture fitted into maxW x maxH: animated GIFs when they're small enough,
// else the first frame. Pictures too big to decode here are refused.
bool decodePicture(const std::string& body, int maxW, int maxH, bool animated, bool round, Picture& out) {
    if (animated && decodeGif(body, maxW, maxH, round, out)) return true;
    out = Picture{};
    const auto* data = reinterpret_cast<const stbi_uc*>(body.data());
    const int len = static_cast<int>(body.size());
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(data, len, &w, &h, &comp)) return false;
    const bool gif = body.compare(0, 3, "GIF") == 0;
    if (static_cast<uint64_t>(w) * h * (gif ? 9u : 4u) > (1600u << 10)) return false;
    stbi_uc* px = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
    if (!px) return false;
    fitInto(w, h, maxW, maxH, out);
    out.px.resize((size_t)out.w * out.h);
    shrinkInto(px, w, h, out.px.data(), out.w, out.h, round, false);
    stbi_image_free(px);
    return true;
}

// ---------------------------------------------------------------- network

struct Reply {
    std::atomic<bool> done{false};
    long status = 0;   // HTTP status, or minus the curl error
    std::string body;  // API replies
    std::string error; // curl's own words when it failed
    Picture picture;   // pictures: decoded on the network thread
};

std::string g_startProblem; // set when the network didn't start (shown on the first screen)

// One thread runs every request in order (Discord's API first, then pictures),
// so the overlay never waits on the network.
class Net {
public:
    void start(std::string token) {
        m_token = std::move(token);
        // Its own small stack: an overlay has only a few MB in all.
        m_running = R_SUCCEEDED(threadCreate(&m_thread, [](void* self) { static_cast<Net*>(self)->run(); },
                                             this, nullptr, 0x40000, 0x2C, -2))
                    && R_SUCCEEDED(threadStart(&m_thread));
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_quit = true;
        }
        m_cv.notify_all();
        if (m_running) {
            threadWaitForExit(&m_thread);
            threadClose(&m_thread);
            m_running = false;
        }
    }

    // Discord's API (the only requests that carry the token).
    std::shared_ptr<Reply> api(std::string method, std::string path, std::string body = {}) {
        Job job;
        job.api = true;
        job.method = std::move(method);
        job.url = std::string(kApi) + path;
        job.body = std::move(body);
        return queue(std::move(job));
    }

    // A picture from any host, no token. `fallback` is tried if `url` fails.
    std::shared_ptr<Reply> picture(std::string url, std::string fallback, int maxW, int maxH, bool animated,
                                   bool round) {
        Job job;
        job.method = "GET";
        job.url = std::move(url);
        job.fallback = std::move(fallback);
        job.maxW = maxW;
        job.maxH = maxH;
        job.animated = animated;
        job.round = round;
        return queue(std::move(job));
    }

    size_t waitingPictures() {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_pictures.size();
    }

private:
    struct Job {
        bool api = false;
        std::string method, url, body, fallback;
        int maxW = 0, maxH = 0;
        bool animated = false, round = false;
        std::shared_ptr<Reply> reply;
    };
    struct Sink {
        CURL* c;
        std::string* body;
        size_t cap;
    };

    std::shared_ptr<Reply> queue(Job job) {
        auto reply = std::make_shared<Reply>();
        if (!m_running) {
            reply->status = -1;
            reply->error = "the network thread didn't start";
            reply->done = true;
            return reply;
        }
        job.reply = reply;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            (job.api ? m_apis : m_pictures).push_back(std::move(job));
        }
        m_cv.notify_one();
        return reply;
    }

    static size_t onBody(char* p, size_t size, size_t n, void* user) {
        Sink* sink = static_cast<Sink*>(user);
        if (sink->body->empty()) { // room for all of it at once: no doubling copies
            curl_off_t length = -1;
            curl_easy_getinfo(sink->c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
            if (length > static_cast<curl_off_t>(sink->cap)) return 0;
            if (length > 0) sink->body->reserve(static_cast<size_t>(length));
        }
        if (sink->body->size() + size * n > sink->cap) return 0; // too big for an overlay: give up
        sink->body->append(p, size * n);
        return size * n;
    }

    static int onProgress(void* self, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        return static_cast<Net*>(self)->m_quit.load() ? 1 : 0; // closing: give up now
    }

    void run() {
        CURL* c = curl_easy_init();
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(m_mu);
                m_cv.wait(lk, [this] { return m_quit.load() || !m_apis.empty() || !m_pictures.empty(); });
                if (m_quit) break;
                std::deque<Job>& q = !m_apis.empty() ? m_apis : m_pictures;
                job = std::move(q.front());
                q.pop_front();
            }
            try {
                if (!c) {
                    job.reply->status = -1;
                } else if (job.api) {
                    fetch(c, job.method, job.url, job.body, true, kApiBody, *job.reply, job.reply->body);
                } else {
                    std::string body;
                    for (const std::string& u : {job.url, job.fallback}) {
                        if (u.empty()) continue;
                        body.clear();
                        if (fetch(c, "GET", u, std::string(), false, kPictureBody, *job.reply, body)
                            && decodePicture(body, job.maxW, job.maxH, job.animated && u == job.url, job.round,
                                             job.reply->picture))
                            break;
                        job.reply->picture = Picture{};
                    }
                }
            } catch (...) { // out of memory: this one fails, the overlay goes on
                job.reply->body = std::string();
                job.reply->picture = Picture{};
                job.reply->status = -static_cast<long>(CURLE_OUT_OF_MEMORY);
                job.reply->error = "out of memory";
            }
            job.reply->done = true;
        }
        if (c) curl_easy_cleanup(c);
    }

    static std::string hostOf(const std::string& url) {
        size_t a = url.find("://");
        a = a == std::string::npos ? 0 : a + 3;
        const size_t b = url.find('/', a);
        return url.substr(a, b == std::string::npos ? std::string::npos : b - a);
    }

    bool fetch(CURL* c, const std::string& method, const std::string& url, const std::string& body, bool auth,
               size_t cap, Reply& reply, std::string& out) {
        curl_easy_reset(c);
        const std::string host = hostOf(url);
        std::vector<std::string>& ips = m_ips[host];
        if (ips.empty()) ips = addressesOf(host);
        if (ips.empty()) {
            reply.status = -static_cast<long>(CURLE_COULDNT_RESOLVE_HOST);
            reply.error = "no address for " + host;
            return false;
        }
        size_t& index = m_ipIndex[host];
        curl_slist* pinned = curl_slist_append(nullptr, (host + ":443:" + ips[index % ips.size()]).c_str());
        curl_easy_setopt(c, CURLOPT_RESOLVE, pinned);
        curl_slist* headers = nullptr;
        if (auth) headers = curl_slist_append(headers, ("Authorization: " + m_token).c_str());
        headers = curl_slist_append(headers, (std::string("User-Agent: ") + kUserAgent).c_str());
        if (!body.empty()) headers = curl_slist_append(headers, "Content-Type: application/json");
        Sink sink{c, &out, cap};
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, auth ? 0L : 1L); // no redirects for the token
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 20000L);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        // As Ultrahand's own downloads (which work in an overlay): HTTP/1.1,
        // IPv4 and TLS 1.2. The certificate is still checked.
        curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(c, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onBody);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, this);
        if (method == "POST") {
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
        const CURLcode rc = curl_easy_perform(c);
        long status = 0;
        if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        reply.status = rc == CURLE_OK ? status : -static_cast<long>(rc);
        reply.error = rc == CURLE_OK ? std::string() : std::string(curl_easy_strerror(rc));
        if (rc == CURLE_COULDNT_CONNECT || rc == CURLE_OPERATION_TIMEDOUT) ++index; // the next address
        curl_slist_free_all(headers);
        curl_slist_free_all(pinned);
        return rc == CURLE_OK && status >= 200 && status < 300;
    }

    std::string m_token;
    std::map<std::string, std::vector<std::string>> m_ips;
    std::map<std::string, size_t> m_ipIndex;
    Thread m_thread{};
    bool m_running = false;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<Job> m_apis, m_pictures;
    std::atomic<bool> m_quit{false};
};

Net g_net;

std::string problem(const Reply& r) {
    if (r.status == 401) return "Discord signed this account out. Sign in again in HomeNX: Social > Discord.";
    if (r.status == 403) return "You can't see this.";
    if (r.status < 0) {
        if (!g_startProblem.empty()) return g_startProblem;
        NifmInternetConnectionStatus net = NifmInternetConnectionStatus_ConnectingUnknown1;
        u32 strength = 0;
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(nullptr, &strength, &net))
            && net != NifmInternetConnectionStatus_Connected)
            return "The console isn't online.";
        return "Couldn't reach Discord: " + r.error + " (" + std::to_string(-r.status) + ").";
    }
    return "Discord said no (" + std::to_string(r.status) + ").";
}

// ---------------------------------------------------------------- gateway state

struct Presence {
    std::string status;   // online, idle, dnd, offline
    std::string activity; // "Playing ...", or their custom status
};

// Written by the gateway thread, copied by the screen when the version moves.
std::mutex g_presenceMu;
std::map<std::string, Presence> g_presences; // friends, by user id
std::string g_myStatus, g_myGame;             // ours: status, and the game we say we play
bool g_presencesLoaded = false;
std::atomic<unsigned> g_presenceVersion{0};

// The chat on screen, so the gateway can say when it changes and who's typing.
std::mutex g_chatMu;
std::string g_openChannel, g_openGuild, g_typingId, g_typingName;
bool g_subscribe = false; // the screen opened a server channel: subscribe to it
u64 g_typingUntil = 0;
std::atomic<bool> g_chatChanged{false};

Presence presenceOf(yyjson_val* p) {
    Presence out{str(p, "status"), {}};
    std::string custom;
    size_t i, n;
    yyjson_val* a;
    yyjson_arr_foreach(yyjson_obj_get(p, "activities"), i, n, a) {
        const long type = num(a, "type");
        if (type == 4) {
            if (custom.empty()) custom = cleanName(str(a, "state"));
            continue;
        }
        const std::string name = cleanName(str(a, "name"));
        if (!out.activity.empty() || name.empty()) continue;
        static const char* const kVerb[] = {"Playing ", "Streaming ", "Listening to ", "Watching ", "", "Competing in "};
        out.activity = (type >= 0 && type <= 5 ? kVerb[type] : "") + name;
    }
    if (out.activity.empty()) out.activity = custom;
    return out;
}

std::string base64(const u8* d, size_t n) {
    static const char* const k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const u32 v = (u32(d[i]) << 16) | (i + 1 < n ? u32(d[i + 1]) << 8 : 0) | (i + 2 < n ? u32(d[i + 2]) : 0);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < n ? k[(v >> 6) & 63] : '=';
        out += i + 2 < n ? k[v & 63] : '=';
    }
    return out;
}

// A WebSocket for the gateway. The console's curl has no WebSocket support, so
// curl only opens the TLS connection (CONNECT_ONLY) and this does the upgrade
// and the framing, like HOME's net/websocket.cpp. Payloads are handed on as
// they arrive, never held whole (READY is megabytes).
class Socket {
public:
    explicit Socket(const std::atomic<bool>& quit) : m_quit(quit) {}
    ~Socket() {
        if (!m_c) return;
        if (m_open) frame(0x8, "\x03\xE8", 2); // 1000: going away
        curl_easy_cleanup(m_c);
    }

    bool open(const char* host, const char* path, const std::string& ip) {
        m_c = curl_easy_init();
        if (!m_c) return false;
        curl_slist* pinned = curl_slist_append(nullptr, (std::string(host) + ":443:" + ip).c_str());
        curl_easy_setopt(m_c, CURLOPT_URL, (std::string("https://") + host + "/").c_str());
        curl_easy_setopt(m_c, CURLOPT_RESOLVE, pinned);
        curl_easy_setopt(m_c, CURLOPT_CONNECT_ONLY, 1L);
        curl_easy_setopt(m_c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(m_c, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(m_c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(m_c, CURLOPT_SSL_ENABLE_ALPN, 0L); // WebSockets need HTTP/1.1
        curl_easy_setopt(m_c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(m_c, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
        const CURLcode rc = curl_easy_perform(m_c);
        curl_slist_free_all(pinned);
        if (rc != CURLE_OK) return false;
        m_open = true;

        u8 key[16];
        randomGet(key, sizeof(key));
        const std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: " + host
            + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + base64(key, sizeof(key))
            + "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: " + kUserAgent + "\r\nOrigin: https://discord.com\r\n\r\n";
        if (!sendAll(req.data(), req.size())) return false;
        const u64 deadline = nowMs() + 10000;
        char buf[2048];
        while (nowMs() < deadline && !m_quit) {
            size_t n = 0;
            const CURLcode r = curl_easy_recv(m_c, buf, sizeof(buf), &n);
            if (r == CURLE_AGAIN) {
                svcSleepThread(5'000'000ULL);
                continue;
            }
            if (r != CURLE_OK || n == 0) break;
            m_in.append(buf, n);
            const size_t end = m_in.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (m_in.size() > 16384) break;
                continue;
            }
            const bool upgraded = m_in.compare(0, 12, "HTTP/1.1 101") == 0;
            m_in.erase(0, end + 4); // anything after the head is WebSocket data already
            return upgraded;
        }
        return false;
    }

    bool sendText(const std::string& s) { return frame(0x1, s.data(), s.size()); }

    // Reads what has arrived: payload bytes go to onData(binary, p, n) as they
    // come, onEnd() follows each message's last byte. false once it's closed.
    template <class Data, class End>
    bool poll(Data& onData, End& onEnd) {
        if (!m_open) return false;
        char buf[16384];
        for (;;) {
            size_t n = 0;
            const CURLcode rc = curl_easy_recv(m_c, buf, sizeof(buf), &n);
            if (rc == CURLE_AGAIN) return true;
            if (rc != CURLE_OK || n == 0) {
                m_open = false;
                return false;
            }
            m_in.append(buf, n);
            if (!parse(onData, onEnd)) {
                m_open = false;
                return false;
            }
        }
    }

    int closeCode = 0;

private:
    template <class Data, class End>
    bool parse(Data& onData, End& onEnd) {
        for (;;) {
            if (!m_inFrame) {
                if (m_in.size() < 2) return true;
                const u8* b = reinterpret_cast<const u8*>(m_in.data());
                if (b[1] & 0x80) return false; // servers never mask
                u64 len = b[1] & 0x7F;
                size_t pos = 2;
                if (len == 126) {
                    if (m_in.size() < 4) return true;
                    len = (u64(b[2]) << 8) | b[3];
                    pos = 4;
                } else if (len == 127) {
                    if (m_in.size() < 10) return true;
                    len = 0;
                    for (int i = 0; i < 8; ++i) len = (len << 8) | b[2 + i];
                    pos = 10;
                }
                m_fin = b[0] & 0x80;
                m_op = b[0] & 0x0F;
                if (m_op != 0 && m_op < 0x8) m_msgOp = m_op;
                m_left = len;
                m_inFrame = true;
                m_in.erase(0, pos);
            }
            if (m_op >= 0x8) { // close / ping / pong: small, read whole
                if (m_in.size() < m_left) return true;
                const std::string payload = m_in.substr(0, (size_t)m_left);
                m_in.erase(0, (size_t)m_left);
                m_inFrame = false;
                if (m_op == 0x8) {
                    closeCode = payload.size() >= 2 ? (u8(payload[0]) << 8) | u8(payload[1]) : 1005;
                    frame(0x8, payload.data(), std::min<size_t>(2, payload.size()));
                    m_open = false;
                    return false;
                }
                if (m_op == 0x9) frame(0xA, payload.data(), payload.size());
                continue;
            }
            const size_t take = (size_t)std::min<u64>(m_left, m_in.size());
            if (take) {
                if (!onData(m_msgOp == 0x2, m_in.data(), take)) return false;
                m_in.erase(0, take);
                m_left -= take;
            }
            if (m_left) return true;
            m_inFrame = false;
            if (m_fin) onEnd();
        }
    }

    bool sendAll(const char* p, size_t n) {
        const u64 deadline = nowMs() + 10000;
        while (n > 0) {
            size_t sent = 0;
            const CURLcode rc = curl_easy_send(m_c, p, n, &sent);
            if (rc == CURLE_OK) {
                p += sent;
                n -= sent;
            } else if (rc == CURLE_AGAIN && nowMs() < deadline) {
                svcSleepThread(2'000'000ULL);
            } else {
                m_open = false;
                return false;
            }
        }
        return true;
    }

    bool frame(int op, const char* p, size_t n) {
        if (!m_open) return false;
        std::string f;
        f.reserve(n + 14);
        f += static_cast<char>(0x80 | op);
        if (n < 126) {
            f += static_cast<char>(0x80 | n);
        } else if (n < 65536) {
            f += static_cast<char>(0x80 | 126);
            f += static_cast<char>(n >> 8);
            f += static_cast<char>(n & 0xFF);
        } else {
            f += static_cast<char>(0x80 | 127);
            for (int i = 7; i >= 0; --i) f += static_cast<char>((u64(n) >> (i * 8)) & 0xFF);
        }
        u8 mask[4];
        randomGet(mask, sizeof(mask));
        f.append(reinterpret_cast<const char*>(mask), 4);
        for (size_t i = 0; i < n; ++i) f += static_cast<char>(p[i] ^ mask[i & 3]);
        return sendAll(f.data(), f.size());
    }

    const std::atomic<bool>& m_quit;
    CURL* m_c = nullptr;
    bool m_open = false;
    std::string m_in;
    bool m_inFrame = false, m_fin = false;
    int m_op = 0, m_msgOp = 0;
    u64 m_left = 0;
};

// The gateway's zlib stream, inflated as it arrives.
class Inflate {
public:
    Inflate() { m_ok = inflateInit(&m_z) == Z_OK; }
    ~Inflate() {
        if (m_ok) inflateEnd(&m_z);
    }
    bool ok() const { return m_ok; }

    template <class Out>
    bool feed(const char* p, size_t n, Out& out) {
        m_z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(p));
        m_z.avail_in = static_cast<uInt>(n);
        for (;;) {
            m_z.next_out = reinterpret_cast<Bytef*>(m_buf);
            m_z.avail_out = sizeof(m_buf);
            const int rc = inflate(&m_z, Z_SYNC_FLUSH);
            if (rc != Z_OK && rc != Z_BUF_ERROR && rc != Z_STREAM_END) return false;
            const size_t got = sizeof(m_buf) - m_z.avail_out;
            if (got) out(m_buf, got);
            if (m_z.avail_out != 0 || (rc == Z_BUF_ERROR && !got)) return true;
        }
    }

private:
    z_stream m_z{};
    bool m_ok = false;
    char m_buf[16384];
};

// Picks one array (merged_presences.friends) out of a message as it streams
// by, so READY_SUPPLEMENTAL is never held whole.
struct ArrayScan {
    bool inString = false, escaped = false, wantArray = false, capturing = false, done = false;
    int depth = 0, captureDepth = 0;
    std::string word, lastWord, out;

    void feed(const char* p, size_t n) {
        for (size_t i = 0; i < n && !done; ++i) {
            const char c = p[i];
            if (capturing) {
                out += c;
                if (out.size() > (384u << 10)) { // more friends online than an overlay holds
                    out.clear();
                    capturing = false;
                    done = true;
                    return;
                }
            }
            if (inString) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') {
                    inString = false;
                    lastWord.swap(word);
                } else if (word.size() < 12) word += c;
                continue;
            }
            switch (c) {
                case '"':
                    inString = true;
                    word.clear();
                    wantArray = false;
                    break;
                case ':':
                    wantArray = lastWord == "friends";
                    break;
                case '[':
                    ++depth;
                    if (wantArray && !capturing) {
                        capturing = true;
                        captureDepth = depth;
                        out = "[";
                    }
                    wantArray = false;
                    break;
                case '{':
                    ++depth;
                    wantArray = false;
                    break;
                case ']':
                case '}':
                    if (capturing && depth == captureDepth) {
                        capturing = false;
                        done = true;
                        return;
                    }
                    --depth;
                    break;
                case ' ': case '\n': case '\r': case '\t':
                    break;
                default:
                    wantArray = false;
                    break;
            }
        }
    }
};

// The game on screen: Discord shows it as "Playing <game>" with "Nintendo
// Switch" under it, and the game's icon when an application is set.
struct Game {
    u64 id = 0;
    std::string name;
};

Game runningGame() {
    Game g;
    u64 pid = 0, program = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&pid)) || R_FAILED(pminfoGetProgramId(&program, pid))) return g;
    // Homebrew run through a game (Atmosphère's hbl) isn't that game:
    // pm:info 65000 is Atmosphère's GetProcessInfo (location, override status).
    struct {
        u64 programId;
        u8 storage, pad[7];
        u64 keysHeld, flags;
    } info{};
    if (R_SUCCEEDED(serviceDispatchInOut(pminfoGetServiceSession(), 65000, pid, info)) && (info.flags & 1))
        return g;
    g.id = program & ~0xFFFULL;
    std::unique_ptr<NsApplicationControlData> data(new NsApplicationControlData);
    u64 size = 0;
    NacpLanguageEntry* entry = nullptr;
    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, g.id, data.get(), sizeof(*data),
                                                &size))
        && R_SUCCEEDED(nacpGetLanguageEntry(&data->nacp, &entry)) && entry)
        g.name = trimmed(std::string(entry->name, strnlen(entry->name, sizeof(entry->name))));
    if (g.name.empty()) g.name = "a game";
    return g;
}

u64 epochMs() {
    u64 s = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_NetworkSystemClock, &s))) timeGetCurrentTime(TimeType_UserSystemClock, &s);
    return s * 1000ULL;
}

// The gateway on its own thread: friends' presences, the open chat's news
// (new messages, typing), and our own "Playing" status, which lasts while
// this connection does.
class Gateway {
public:
    void start(std::string token) {
        m_token = std::move(token);
        m_appId = trimmed(readFile("sdmc:/config/SwitchU/discord_app_id.txt"));
        if (m_appId.empty()) m_appId = kGameAppId;
        m_running = R_SUCCEEDED(threadCreate(&m_thread, [](void* self) { static_cast<Gateway*>(self)->run(); },
                                             this, nullptr, 0x20000, 0x2C, -2))
                    && R_SUCCEEDED(threadStart(&m_thread));
    }

    void stop() {
        m_quit = true;
        if (!m_running) return;
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
        m_running = false;
    }

private:
    struct Incoming { // one gateway message, as it inflates
        static constexpr size_t kHeadCap = 64u << 10;
        std::string head; // the first 64 KB: all of a small message
        bool cut = false;
        ArrayScan friends;
        void add(const char* p, size_t n) {
            const size_t room = head.size() < kHeadCap ? kHeadCap - head.size() : 0;
            head.append(p, std::min(room, n));
            if (n > room) cut = true;
            friends.feed(p, n);
        }
    };

    void run() {
        static const int kWaitS[] = {2, 5, 10, 30, 60};
        int attempt = 0;
        while (!m_quit) {
            bool reached = false;
            try {
                const std::vector<std::string> ips = addressesOf(kGatewayHost);
                reached = !ips.empty() && session(ips[m_ipIndex % ips.size()]);
            } catch (...) { // out of memory: drop this connection, try again in a while
                m_closeCode = 0;
            }
            if (m_quit || m_closeCode == 4004) break; // signed out: the screen says so
            if (reached) attempt = 0;
            else ++m_ipIndex;
            const int wait = kWaitS[std::min(attempt, 4)];
            ++attempt;
            for (int t = 0; t < wait * 10 && !m_quit; ++t) svcSleepThread(100'000'000ULL);
        }
    }

    bool session(const std::string& ip) {
        Socket ws(m_quit);
        m_closeCode = 0;
        if (!ws.open(kGatewayHost, "/?v=9&encoding=json&compress=zlib-stream", ip)) return false;
        Inflate z;
        if (!z.ok()) return false;
        m_msg = Incoming{};
        m_helloed = m_ready = m_reconnect = false;
        m_acked = true;
        m_seq = -1;
        m_sent = m_game.id == 0; // a game already showing goes up again after READY
        m_myStatus.clear();
        auto take = [this](const char* p, size_t n) { m_msg.add(p, n); };
        auto onData = [&](bool binary, const char* p, size_t n) {
            if (binary) return z.feed(p, n, take);
            take(p, n);
            return true;
        };
        auto onEnd = [&]() {
            handle(ws);
            m_msg = Incoming{};
        };
        while (!m_quit && !m_reconnect) {
            if (!ws.poll(onData, onEnd)) break;
            const u64 now = nowMs();
            if (m_helloed && now >= m_nextBeat) {
                if (!m_acked) break; // no answer to the last one: a dead connection
                ws.sendText(m_seq >= 0 ? "{\"op\":1,\"d\":" + std::to_string(m_seq) + "}" : "{\"op\":1,\"d\":null}");
                m_acked = false;
                m_nextBeat = now + m_interval;
            }
            if (m_ready) {
                subscribe(ws);
                game(ws, now);
            }
            svcSleepThread(30'000'000ULL);
        }
        m_closeCode = ws.closeCode;
        return m_ready;
    }

    void identify(Socket& ws) {
        const std::string props =
            std::string("{\"os\":\"Linux\",\"browser\":\"Chrome\",\"device\":\"\",\"system_locale\":\"en-US\","
                        "\"has_client_mods\":false,\"browser_user_agent\":")
            + jsonString(kUserAgent)
            + ",\"browser_version\":\"134.0.0.0\",\"os_version\":\"\",\"referrer\":\"\",\"referring_domain\":\"\","
              "\"referrer_current\":\"\",\"referring_domain_current\":\"\",\"release_channel\":\"stable\","
              "\"client_build_number\":440000,\"client_event_source\":null}";
        // Capabilities as HOME's gateway: a smaller READY, presences in READY_SUPPLEMENTAL.
        ws.sendText("{\"op\":2,\"d\":{\"token\":" + jsonString(m_token) + ",\"capabilities\":51,\"properties\":" + props
                    + ",\"presence\":{\"status\":\"unknown\",\"since\":0,\"activities\":[],\"afk\":false},"
                      "\"compress\":false,\"client_state\":{\"guild_versions\":{}}}}");
    }

    // A server channel's messages and typing come only once subscribed to it
    // (as HOME's gateway does); DMs always come.
    void subscribe(Socket& ws) {
        std::string guild, channel;
        {
            std::lock_guard<std::mutex> lk(g_chatMu);
            if (!g_subscribe) return;
            g_subscribe = false;
            guild = g_openGuild;
            channel = g_openChannel;
        }
        if (guild.empty() || channel.empty()) return;
        ws.sendText("{\"op\":37,\"d\":{\"subscriptions\":{\"" + guild + "\":{\"typing\":true,\"threads\":true,"
                    "\"activities\":false,\"channels\":{\"" + channel + "\":[[0,99]]}}}}}");
    }

    // A field near the start of a big message (Discord writes t and s first).
    static std::string headField(const std::string& head, const char* key) {
        const std::string k = std::string("\"") + key + "\":";
        const size_t at = head.find(k);
        if (at == std::string::npos) return {};
        const size_t a = at + k.size();
        if (a < head.size() && head[a] == '"') {
            const size_t b = head.find('"', a + 1);
            return b == std::string::npos ? std::string() : head.substr(a + 1, b - a - 1);
        }
        size_t b = a;
        while (b < head.size() && (std::isdigit(static_cast<unsigned char>(head[b])) || head[b] == '-')) ++b;
        return head.substr(a, b - a);
    }

    void handle(Socket& ws) {
        std::string t;
        if (m_msg.cut) {
            t = headField(m_msg.head, "t");
            const std::string s = headField(m_msg.head, "s");
            if (!s.empty()) m_seq = std::strtol(s.c_str(), nullptr, 10);
        } else {
            Json doc(m_msg.head);
            yyjson_val* root = doc.root();
            if (!root) return;
            yyjson_val* s = yyjson_obj_get(root, "s");
            if (yyjson_is_int(s)) m_seq = static_cast<long>(yyjson_get_sint(s));
            t = str(root, "t");
            yyjson_val* d = yyjson_obj_get(root, "d");
            switch (num(root, "op")) {
                case 10: { // HELLO
                    const long every = num(d, "heartbeat_interval");
                    m_interval = every > 0 ? static_cast<u64>(every) : 41250;
                    u32 r = 0;
                    randomGet(&r, sizeof(r));
                    m_nextBeat = nowMs() + m_interval * (r % 1000) / 1000;
                    m_helloed = true;
                    identify(ws);
                    break;
                }
                case 11: m_acked = true; break;
                case 1: m_nextBeat = 0; break;
                case 7: case 9: m_reconnect = true; break; // reconnect / invalid session: start over
                default: break;
            }
            if (t == "PRESENCE_UPDATE") {
                const std::string id = str(yyjson_obj_get(d, "user"), "id");
                if (!id.empty()) {
                    std::lock_guard<std::mutex> lk(g_presenceMu);
                    g_presences[id] = presenceOf(d);
                    ++g_presenceVersion;
                }
            } else if (t == "SESSIONS_REPLACE") { // our own status, kept when we set the game
                size_t i, n;
                yyjson_val* sess;
                yyjson_arr_foreach(d, i, n, sess)
                    if (str(sess, "session_id") == "all") m_myStatus = str(sess, "status");
                if (m_myStatus == "offline") m_myStatus = "invisible";
                std::lock_guard<std::mutex> lk(g_presenceMu);
                g_myStatus = m_myStatus;
                ++g_presenceVersion;
            } else if (t.rfind("MESSAGE_", 0) == 0 || t == "TYPING_START") {
                std::lock_guard<std::mutex> lk(g_chatMu);
                if (str(d, "channel_id") == g_openChannel && !g_openChannel.empty()) {
                    if (t == "TYPING_START") {
                        yyjson_val* member = yyjson_obj_get(d, "member");
                        yyjson_val* user = yyjson_obj_get(member, "user");
                        std::string name = str(member, "nick");
                        if (name.empty()) name = str(user, "global_name");
                        if (name.empty()) name = str(user, "username");
                        g_typingId = str(d, "user_id");
                        g_typingName = cleanName(name);
                        g_typingUntil = nowMs() + 9000;
                    } else {
                        if (t == "MESSAGE_CREATE") g_typingUntil = 0;
                        g_chatChanged = true;
                    }
                }
            }
        }
        if (t == "READY") {
            m_ready = true;
            m_nextGame = nowMs() + 2000; // our status (SESSIONS_REPLACE) comes just after
            std::lock_guard<std::mutex> lk(g_chatMu);
            g_subscribe = !g_openGuild.empty(); // a new connection: subscribe again
        } else if (t == "READY_SUPPLEMENTAL" && m_msg.friends.done) {
            Json doc(m_msg.friends.out);
            std::lock_guard<std::mutex> lk(g_presenceMu);
            size_t i, n;
            yyjson_val* p;
            yyjson_arr_foreach(doc.root(), i, n, p) g_presences[str(p, "user_id")] = presenceOf(p);
            g_presencesLoaded = true;
            ++g_presenceVersion;
        }
    }

    // Every few seconds: the game being played, and Discord told when it changes.
    void game(Socket& ws, u64 now) {
        if (now >= m_nextGame) {
            m_nextGame = now + 5000;
            Game g = runningGame();
            if (g.id != m_game.id) {
                m_game = std::move(g);
                m_since = epochMs();
                m_image.clear();
                m_assets.reset();
                m_sent = false;
                if (m_game.id && !m_appId.empty()) {
                    // Discord fetches the icon itself; it never comes to the console.
                    char url[96];
                    std::snprintf(url, sizeof(url), "https://tinfoil.media/ti/%016llX/256/256/",
                                  static_cast<unsigned long long>(m_game.id));
                    m_assets = g_net.api("POST", "/applications/" + m_appId + "/external-assets",
                                         std::string("{\"urls\":[\"") + url + "\"]}");
                }
                std::lock_guard<std::mutex> lk(g_presenceMu);
                g_myGame = m_game.name;
                ++g_presenceVersion;
            }
        }
        if (m_assets && m_assets->done) {
            Json doc(m_assets->body);
            const std::string path = str(yyjson_arr_get_first(doc.root()), "external_asset_path");
            if (m_assets->status == 200 && !path.empty()) m_image = "mp:" + path;
            m_assets.reset();
        }
        if (m_sent || m_assets) return;
        m_sent = true;
        std::string activities = "[]";
        if (m_game.id) {
            activities = "[{\"type\":0,\"name\":" + jsonString(m_game.name)
                + ",\"details\":\"Nintendo Switch\",\"timestamps\":{\"start\":" + std::to_string(m_since) + "}";
            if (!m_appId.empty()) activities += ",\"application_id\":" + jsonString(m_appId);
            if (!m_image.empty())
                activities += ",\"assets\":{\"large_image\":" + jsonString(m_image)
                    + ",\"large_text\":" + jsonString(m_game.name) + "}";
            activities += "}]";
        }
        ws.sendText("{\"op\":3,\"d\":{\"status\":" + jsonString(m_myStatus.empty() ? "unknown" : m_myStatus)
                    + ",\"since\":0,\"activities\":" + activities + ",\"afk\":false}}");
    }

    std::string m_token, m_appId;
    Thread m_thread{};
    bool m_running = false;
    std::atomic<bool> m_quit{false};
    size_t m_ipIndex = 0;
    int m_closeCode = 0;

    Incoming m_msg;
    bool m_helloed = false, m_acked = true, m_ready = false, m_reconnect = false;
    long m_seq = -1;
    u64 m_interval = 41250, m_nextBeat = 0, m_nextGame = 0;
    std::string m_myStatus;

    Game m_game;
    u64 m_since = 0;
    std::string m_image;
    std::shared_ptr<Reply> m_assets;
    bool m_sent = true;
};

Gateway g_gateway;

// ---------------------------------------------------------------- the screen

// The whole overlay is one drawing: libtesla's own frame would take a third of it.
class Canvas : public tsl::elm::Element {
public:
    explicit Canvas(std::function<void(tsl::gfx::Renderer*)> fn) : m_fn(std::move(fn)) {}
    void draw(tsl::gfx::Renderer* r) override { m_fn(r); }
    void layout(u16 x, u16 y, u16 w, u16 h) override { setBoundaries(x, y, w, h); }

private:
    std::function<void(tsl::gfx::Renderer*)> m_fn;
};

s32 widthOf(tsl::gfx::Renderer* r, const std::string& s, float size) {
    return static_cast<s32>(r->drawString(s.c_str(), false, 0, 0, size, tsl::style::color::ColorTransparent).first);
}

std::string fit(tsl::gfx::Renderer* r, std::string s, float size, s32 maxW) {
    if (widthOf(r, s, size) <= maxW) return s;
    while (!s.empty() && widthOf(r, s + "...", size) > maxW) popChar(s);
    return s + "...";
}

std::string firstLetter(const std::string& s) {
    if (s.empty()) return s;
    const char* p = s.data();
    nextCp(p, s.data() + s.size());
    return s.substr(0, p - s.data());
}

// Scissoring for a block of drawing, undone however the block ends.
struct Scissor {
    tsl::gfx::Renderer* r;
    Scissor(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 w, s32 h) : r(renderer) { r->enableScissoring(x, y, w, h); }
    ~Scissor() { r->disableScissoring(); }
};

// A rectangle with round corners (Discord's message box, selections).
void roundRect(tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h, s32 rad, tsl::Color c) {
    r->drawRect(x + rad, y, w - 2 * rad, h, r->a(c));
    r->drawRect(x, y + rad, rad, h - 2 * rad, r->a(c));
    r->drawRect(x + w - rad, y + rad, rad, h - 2 * rad, r->a(c));
    for (s32 cx : {x + rad, x + w - rad - 1})
        for (s32 cy : {y + rad, y + h - rad - 1}) r->drawCircle(cx, cy, static_cast<u16>(rad), true, r->a(c));
}

// Discord's status dot, ringed in the colour behind it.
void statusDot(tsl::gfx::Renderer* r, s32 cx, s32 cy, const std::string& status, tsl::Color behind) {
    if (status.empty()) return;
    r->drawCircle(cx, cy, 7, true, r->a(behind));
    if (status == "online") {
        r->drawCircle(cx, cy, 5, true, r->a(kOnline));
    } else if (status == "idle") { // a moon
        r->drawCircle(cx, cy, 5, true, r->a(kIdle));
        r->drawCircle(cx - 2, cy - 2, 3, true, r->a(behind));
    } else if (status == "dnd") {
        r->drawCircle(cx, cy, 5, true, r->a(kDnd));
        r->drawRect(cx - 3, cy - 1, 7, 2, r->a(behind));
    } else { // offline / invisible: a grey ring
        r->drawCircle(cx, cy, 5, true, r->a(kChannel));
        r->drawCircle(cx, cy, 2, true, r->a(behind));
    }
}

std::string statusWord(const std::string& status) {
    if (status == "online") return "Online";
    if (status == "idle") return "Idle";
    if (status == "dnd") return "Do Not Disturb";
    if (status == "invisible") return "Invisible";
    return {};
}

// "Today at 14:05", "Yesterday at 09:30", "12/10/2026 18:00" from a snowflake.
std::string whenOf(const std::string& id) {
    const u64 seconds = ((snowflake(id) >> 22) + 1420070400000ULL) / 1000ULL;
    u64 now = 0;
    TimeCalendarTime at{}, today{}, yesterday{};
    TimeCalendarAdditionalInfo info{};
    if (R_FAILED(timeToCalendarTimeWithMyRule(seconds, &at, &info))) return {};
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &now))) {
        timeToCalendarTimeWithMyRule(now, &today, &info);
        timeToCalendarTimeWithMyRule(now - 86400, &yesterday, &info);
    }
    auto same = [&](const TimeCalendarTime& d) { return d.year == at.year && d.month == at.month && d.day == at.day; };
    char buf[40];
    if (same(today)) std::snprintf(buf, sizeof(buf), "Today at %02d:%02d", at.hour, at.minute);
    else if (same(yesterday)) std::snprintf(buf, sizeof(buf), "Yesterday at %02d:%02d", at.hour, at.minute);
    else std::snprintf(buf, sizeof(buf), "%02d/%02d/%d %02d:%02d", at.day, at.month, at.year, at.hour, at.minute);
    return buf;
}

// The emoji picker's own list: the ones people use most.
const char* const kStandardEmoji =
    "😀 😃 😄 😁 😆 😅 😂 🤣 😊 😇 🙂 🙃 😉 😌 😍 🥰 😘 😗 😋 😛 😜 🤪 😝 🤑 🤗 🤭 🤫 🤔 🤐 🤨 "
    "😐 😑 😶 😏 😒 🙄 😬 🤥 😔 😪 🤤 😴 😷 🤒 🤕 🤢 🤮 🥵 🥶 🥴 😵 🤯 🤠 🥳 😎 🤓 🧐 😕 😟 🙁 "
    "😮 😯 😲 😳 🥺 😦 😧 😨 😰 😥 😢 😭 😱 😖 😣 😞 😓 😩 😫 🥱 😤 😡 😠 🤬 😈 👿 💀 ☠️ 💩 🤡 "
    "👻 👽 👾 🤖 😺 😸 😹 😻 😼 😽 🙀 😿 😾 🙈 🙉 🙊 ❤️ 🧡 💛 💚 💙 💜 🖤 🤍 🤎 💔 ❣️ 💕 💞 💓 "
    "💗 💖 💘 💝 💯 💢 💥 💫 💦 💨 💬 💤 👋 🤚 ✋ 🖖 👌 🤌 🤏 ✌️ 🤞 🤟 🤘 🤙 👈 👉 👆 👇 ☝️ 👍 "
    "👎 ✊ 👊 🤛 🤜 👏 🙌 👐 🤲 🤝 🙏 💪 👀 🧠 🗿 🔥 ✨ ⭐ 🌟 🎉 🎊 🎁 🏆 🎮 🕹️ 🍕 🍔 🍟 🍿 ☕ "
    "🍺 🎂 🌈 ☀️ 🌙 ⚡ ❄️ 🌸 🌹 🐶 🐱 🐸 🐼 🦊 🐧 ✅ ❌ ❓ ❗ ⚠️ 🚫 🆗 🆒 🆕";

struct Row {
    enum Kind { Header, Dm, Channel } kind;
    std::string id, title, avatar, userId; // userId: the other person in a 1:1 DM
    std::string shown;                     // title cut to fit, made on first draw
};

struct Server {
    std::string id, name, icon, initials;
};

struct ReactionPill {
    std::string emoji; // its picture
    int count = 0;
    bool mine = false;
};

struct Msg {
    std::string id, authorId, author, avatar, text, time, reply;
    std::vector<std::string> emojis;
    std::vector<Media> media;
    std::vector<ReactionPill> reactions;
    std::vector<std::string> lines;
    bool wrapped = false, header = true, big = false;
};

struct EmojiItem {
    std::string insert, url; // what goes in the message, and its picture
};

struct GifItem {
    std::string send, preview; // the link sent (Discord shows it as the GIF), and the small GIF
};

class DiscordGui : public tsl::Gui {
public:
    DiscordGui(std::vector<Account> accounts, size_t current) : m_accountList(std::move(accounts)), m_current(current) {
        m_servers.push_back({"", "Direct Messages", "", ""}); // the top of the bar
        if (m_current < m_accountList.size()) {
            m_account = m_accountList[m_current].name;
            m_meAvatar = m_accountList[m_current].avatar;
        }
        if (m_accountList.empty()) {
            m_dmsError = "Sign in to Discord in HomeNX first: Social > Discord.";
            m_dmsLoaded = true;
            return;
        }
        m_meReq = g_net.api("GET", "/users/@me");
        m_dmReq = g_net.api("GET", "/users/@me/channels");
        m_guildsReq = g_net.api("GET", "/users/@me/guilds");
        const std::string list = kStandardEmoji;
        for (size_t i = 0; i < list.size();) {
            const size_t sp = list.find(' ', i);
            const std::string e = list.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            if (!e.empty()) m_standardEmoji.push_back({e, twemoji(codepoints(e))});
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
    }

    tsl::elm::Element* createUI() override {
        return new Canvas([this](tsl::gfx::Renderer* r) {
            try {
                draw(r);
            } catch (...) { // out of memory: let the pictures go rather than the overlay
                m_pictures.clear();
            }
        });
    }

    void update() override {
        try {
            takeReplies();
        } catch (...) {
            m_pictures.clear();
        }
    }

    bool handleInput(u64 down, u64 held, const HidTouchState&, HidAnalogStickState, HidAnalogStickState) override {
        try {
            return input(down, held);
        } catch (...) {
            m_pictures.clear();
            return true;
        }
    }

private:
    struct Held {
        std::shared_ptr<Reply> reply;
        u64 used = 0;
    };
    enum class Typing { Message, GifSearch };

    // ------------------------------------------------------------ replies

    void takeReplies() {
        if (m_meReq && m_meReq->done) {
            if (m_meReq->status == 200) {
                Json doc(m_meReq->body);
                yyjson_val* me = doc.root();
                m_meId = str(me, "id");
                m_meAvatar = avatarUrl(me);
                const std::string nm = cleanName(str(me, "global_name"));
                m_account = nm.empty() ? cleanName(str(me, "username")) : nm;
            }
            m_meReq.reset();
        }
        if (m_dmReq && m_dmReq->done) {
            buildDms();
            m_dmReq.reset();
        }
        if (m_guildsReq && m_guildsReq->done) {
            buildServers();
            m_guildsReq.reset();
        }
        if (m_channelsReq && m_channelsReq->done) {
            buildChannels();
            m_channelsReq.reset();
        }
        if (m_msgReq && m_msgReq->done) {
            takeMessages();
            m_msgReq.reset();
        }
        if (m_sendReq && m_sendReq->done) {
            if (m_sendReq->status == 200) m_lastPoll = 0; // show it straight away
            else m_chatNote = "Couldn't send: " + problem(*m_sendReq);
            m_sendReq.reset();
        }
        if (m_emojiReq && m_emojiReq->done) {
            takeServerEmoji();
            m_emojiReq.reset();
        }
        if (m_gifReq && m_gifReq->done) {
            takeGifs();
            m_gifReq.reset();
        }
        if (m_chat && g_chatChanged.exchange(false)) m_lastPoll = 0; // the gateway says it changed
        if (m_chat && !m_msgReq && armTicksToNs(armGetSystemTick() - m_lastPoll) > 4'000'000'000ULL)
            pollMessages();
        if (g_presenceVersion != m_presenceSeen) {
            std::lock_guard<std::mutex> lk(g_presenceMu);
            m_presenceSeen = g_presenceVersion;
            m_presences = g_presences;
            m_presencesLoaded = g_presencesLoaded;
            m_myStatus = g_myStatus;
            m_myGame = g_myGame;
        }
        trimPictures();
    }

    // ------------------------------------------------------------ input

    bool input(u64 down, u64 held) {
        const u64 dir = repeat(down, held);
        if (m_keyboard) {
            if (m_emojiOpen) return emojiInput(down, dir);
            return keyboardInput(down, dir);
        }
        if (m_gifsOpen) return gifInput(down, dir);
        if (down & HidNpadButton_Minus) { // back to Ultrahand; the game status goes too
            tsl::Overlay::get()->close();
            return true;
        }
        if (m_accountsOpen) return accountsInput(down, dir);
        if (m_chat) {
            if (down & HidNpadButton_B) {
                closeChat();
                return true;
            }
            if (down & (HidNpadButton_A | HidNpadButton_Y)) {
                openKeyboard(Typing::Message);
                if (down & HidNpadButton_Y) openEmoji();
                return true;
            }
            if (down & HidNpadButton_X) {
                openGifs();
                return true;
            }
            if (dir & HidNpadButton_AnyUp) m_chatScroll += 60;
            if (dir & HidNpadButton_AnyDown) m_chatScroll = std::max(0, m_chatScroll - 60);
            return true;
        }
        if (down & HidNpadButton_Y) {
            m_accountsOpen = true;
            m_accountSel = (int)m_current;
            return true;
        }
        if (m_onBar) {
            if (down & HidNpadButton_B) { // hidden it stays loaded, and the game status stays up
                tsl::Overlay::get()->hide();
                return true;
            }
            if (dir & HidNpadButton_AnyUp) m_serverSel = std::max(0, m_serverSel - 1);
            if (dir & HidNpadButton_AnyDown) m_serverSel = std::min((int)m_servers.size() - 1, m_serverSel + 1);
            if ((down & HidNpadButton_A) || (dir & HidNpadButton_AnyRight)) openServer(m_serverSel);
            return true;
        }
        std::vector<Row>& rows = list();
        if ((down & HidNpadButton_B) || (dir & HidNpadButton_AnyLeft)) {
            m_onBar = true;
            m_serverSel = m_shownServer;
            return true;
        }
        if (dir & (HidNpadButton_AnyUp | HidNpadButton_AnyDown)) {
            const int step = (dir & HidNpadButton_AnyUp) ? -1 : 1;
            for (int i = m_rowSel + step; i >= 0 && i < (int)rows.size(); i += step)
                if (rows[(size_t)i].kind != Row::Header) {
                    m_rowSel = i;
                    break;
                }
        }
        if ((down & HidNpadButton_A) && m_rowSel >= 0 && m_rowSel < (int)rows.size())
            openChat(rows[(size_t)m_rowSel]);
        return true;
    }

    u64 repeat(u64 down, u64 held) {
        constexpr u64 kDirs = HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
        if (down & kDirs) {
            m_heldFrames = 0;
            return down & kDirs;
        }
        if (held & kDirs) {
            if (++m_heldFrames > 18 && m_heldFrames % 4 == 0) return held & kDirs;
        } else {
            m_heldFrames = 0;
        }
        return 0;
    }

    bool accountsInput(u64 down, u64 dir) {
        if (down & HidNpadButton_B) {
            m_accountsOpen = false;
            return true;
        }
        if (dir & HidNpadButton_AnyUp) m_accountSel = std::max(0, m_accountSel - 1);
        if (dir & HidNpadButton_AnyDown) m_accountSel = std::min((int)m_accountList.size() - 1, m_accountSel + 1);
        if ((down & HidNpadButton_A) && m_accountSel >= 0 && m_accountSel < (int)m_accountList.size()) {
            if ((size_t)m_accountSel == m_current) {
                m_accountsOpen = false;
                return true;
            }
            // The overlay starts again as that account: every connection made fresh.
            if (FILE* f = std::fopen(kChoicePath, "w")) {
                std::fputs(m_accountList[(size_t)m_accountSel].id.c_str(), f);
                std::fclose(f);
            }
            tsl::setNextOverlay(kSelfPath, "");
            tsl::Overlay::get()->close();
        }
        return true;
    }

    std::vector<Row>& list() { return m_shownServer == 0 ? m_dms : m_channels; }

    static int firstItem(const std::vector<Row>& rows) {
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].kind != Row::Header) return (int)i;
        return -1;
    }

    // What the gateway knows of someone (friends only).
    const Presence* presence(const std::string& userId) const {
        auto it = m_presences.find(userId);
        return it == m_presences.end() ? nullptr : &it->second;
    }

    // ------------------------------------------------------------ pictures

    const Picture* picture(const std::string& url, const std::string& fallback, int w, int h, bool animated,
                           bool round) {
        if (url.empty()) return nullptr;
        // One copy per size: the same emoji in a message and in the picker.
        const std::string key = url + '#' + std::to_string(w) + 'x' + std::to_string(h) + (animated ? "a" : "");
        auto it = m_pictures.find(key);
        if (it == m_pictures.end()) {
            if (g_net.waitingPictures() > 16) return nullptr; // ask again in a moment
            it = m_pictures.emplace(key, Held{g_net.picture(url, fallback, w, h, animated, round), 0}).first;
        }
        it->second.used = armGetSystemTick();
        const Reply& r = *it->second.reply;
        return r.done && !r.picture.px.empty() ? &r.picture : nullptr;
    }

    // The least recently drawn pictures go when they take more than the budget.
    void trimPictures() {
        size_t bytes = 0;
        for (const auto& [key, held] : m_pictures)
            if (held.reply->done) bytes += held.reply->picture.px.size() * 2;
        while (bytes > kPictureBudget) {
            auto oldest = m_pictures.end();
            for (auto it = m_pictures.begin(); it != m_pictures.end(); ++it)
                if (it->second.reply->done && (oldest == m_pictures.end() || it->second.used < oldest->second.used))
                    oldest = it;
            if (oldest == m_pictures.end()) break;
            bytes -= std::min(bytes, oldest->second.reply->picture.px.size() * 2);
            m_pictures.erase(oldest);
        }
    }

    static void blit(tsl::gfx::Renderer* r, const Picture& p, s32 x, s32 y) {
        int f = 0;
        if (p.frames > 1 && p.totalMs > 0) {
            int t = static_cast<int>(nowMs() % (u64)p.totalMs);
            while (f + 1 < p.frames && t >= p.delays[(size_t)f]) t -= p.delays[(size_t)f++];
        }
        const u16* src = p.px.data() + (size_t)f * p.w * p.h;
        for (s32 j = 0; j < p.h; ++j)
            for (s32 i = 0; i < p.w; ++i) {
                const tsl::Color c(src[(size_t)j * p.w + i]);
                if (c.a) r->setPixelBlendSrc(x + i, y + j, r->a(c));
            }
    }

    // A round picture, or a coloured circle with letters while it loads.
    void drawRound(tsl::gfx::Renderer* r, const std::string& url, const std::string& letters, s32 x, s32 y,
                   s32 size) {
        if (const Picture* p = picture(url, std::string(), size, size, false, true)) {
            blit(r, *p, x, y);
            return;
        }
        r->drawCircle(x + size / 2, y + size / 2, size / 2, true, r->a(kSelected));
        if (!letters.empty()) {
            const float fs = size * 0.38f;
            r->drawString(letters.c_str(), false, x + (size - widthOf(r, letters, fs)) / 2,
                          y + size / 2 + (s32)(fs * 0.36f), fs, r->a(kText));
        }
    }

    // ------------------------------------------------------------ text with emoji

    s32 richWidth(tsl::gfx::Renderer* r, const std::string& s, float size, s32 emoji) {
        s32 w = 0;
        std::string seg;
        for (const char* p = s.data(), *end = p + s.size(); p < end;) {
            const char* start = p;
            const uint32_t cp = nextCp(p, end);
            if (isMark(cp)) {
                if (!seg.empty()) w += widthOf(r, seg, size);
                seg.clear();
                w += emoji + 2;
            } else {
                seg.append(start, p);
            }
        }
        if (!seg.empty()) w += widthOf(r, seg, size);
        return w;
    }

    std::vector<std::string> wrapRich(tsl::gfx::Renderer* r, const std::string& text, float size, s32 maxW,
                                      s32 emoji) {
        std::vector<std::string> lines;
        size_t start = 0;
        while (start <= text.size()) {
            const size_t nl = text.find('\n', start);
            const std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            std::string line;
            size_t i = 0;
            while (i <= para.size()) {
                const size_t sp = para.find(' ', i);
                std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
                const std::string candidate = line.empty() ? word : line + " " + word;
                if (richWidth(r, candidate, size, emoji) <= maxW) {
                    line = candidate;
                } else {
                    if (!line.empty()) lines.push_back(line);
                    while (richWidth(r, word, size, emoji) > maxW && word.size() > 1) { // a very long word
                        std::string head = word;
                        while (head.size() > 1 && richWidth(r, head, size, emoji) > maxW) popChar(head);
                        lines.push_back(head);
                        word.erase(0, head.size());
                    }
                    line = word;
                }
                if (sp == std::string::npos) break;
                i = sp + 1;
            }
            lines.push_back(line);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        while (lines.size() > 1 && lines.back().empty()) lines.pop_back();
        return lines;
    }

    void drawRich(tsl::gfx::Renderer* r, const std::string& s, s32 x, s32 baseline, float size, tsl::Color color,
                  const std::vector<std::string>& emojis, s32 emoji) {
        std::string seg;
        auto flush = [&]() {
            if (seg.empty()) return;
            x += static_cast<s32>(r->drawString(seg.c_str(), false, x, baseline, size, r->a(color)).first);
            seg.clear();
        };
        for (const char* p = s.data(), *end = p + s.size(); p < end;) {
            const char* start = p;
            const uint32_t cp = nextCp(p, end);
            if (!isMark(cp)) {
                seg.append(start, p);
                continue;
            }
            flush();
            const size_t i = cp - kMarkFirst;
            if (i < emojis.size())
                if (const Picture* pic = picture(emojis[i], std::string(), emoji, emoji, false, false))
                    blit(r, *pic, x + 1, baseline - emoji + 3);
            x += emoji + 2;
        }
        flush();
    }

    void drawText(tsl::gfx::Renderer* r, const std::string& text, s32 x, s32 y, s32 w, tsl::Color c) {
        std::vector<std::string> none;
        for (const std::string& line : wrapRich(r, text, 17, w, 18)) {
            drawRich(r, line, x, y, 17, c, none, 18);
            y += 23;
        }
    }

    // ------------------------------------------------------------ data

    void buildDms() {
        m_dmsLoaded = true;
        m_dms.clear();
        if (m_dmReq->status != 200) {
            m_dmsError = problem(*m_dmReq);
            return;
        }
        struct Dm { std::string id, title, last, avatar, userId; };
        std::vector<Dm> dms;
        Json doc(m_dmReq->body);
        size_t i, n;
        yyjson_val* c;
        yyjson_arr_foreach(doc.root(), i, n, c) {
            Dm dm{str(c, "id"), cleanName(str(c, "name")), str(c, "last_message_id"), std::string(), std::string()};
            yyjson_val* people = yyjson_obj_get(c, "recipients");
            if (num(c, "type") == 1 && yyjson_arr_size(people) == 1)
                dm.userId = str(yyjson_arr_get_first(people), "id");
            size_t k, m;
            yyjson_val* u;
            yyjson_arr_foreach(people, k, m, u) {
                if (dm.avatar.empty()) dm.avatar = avatarUrl(u);
                if (str(c, "name").empty()) {
                    std::string nm = str(u, "global_name");
                    dm.title += (dm.title.empty() ? "" : ", ") + cleanName(nm.empty() ? str(u, "username") : nm);
                }
            }
            if (!str(c, "icon").empty())
                dm.avatar = "https://cdn.discordapp.com/channel-icons/" + dm.id + "/" + str(c, "icon") + ".png?size=128";
            if (!dm.id.empty()) dms.push_back(std::move(dm));
        }
        std::sort(dms.begin(), dms.end(), [](const Dm& a, const Dm& b) { return newer(a.last, b.last); });
        m_dms.push_back({Row::Header, "", "DIRECT MESSAGES", "", "", ""});
        for (size_t k = 0; k < dms.size() && k < 50; ++k)
            m_dms.push_back({Row::Dm, dms[k].id, dms[k].title.empty() ? "Conversation" : dms[k].title,
                             dms[k].avatar, dms[k].userId, ""});
        if (m_dms.size() == 1) m_dmsError = "No conversations yet.";
        if (m_shownServer == 0) m_rowSel = firstItem(m_dms);
    }

    void buildServers() {
        if (m_guildsReq->status != 200) return; // DMs still work; the bar stays empty
        Json doc(m_guildsReq->body);
        size_t i, n;
        yyjson_val* g;
        yyjson_arr_foreach(doc.root(), i, n, g) {
            Server s{str(g, "id"), cleanName(str(g, "name")), std::string(), std::string()};
            if (!str(g, "icon").empty())
                s.icon = "https://cdn.discordapp.com/icons/" + s.id + "/" + str(g, "icon") + ".png?size=128";
            // Initials, like Discord's icons for servers without a picture.
            bool start = true;
            for (const char* p = s.name.data(), *end = p + s.name.size(); p < end && s.initials.size() < 3;) {
                const char* from = p;
                const uint32_t cp = nextCp(p, end);
                if (cp == ' ') {
                    start = true;
                    continue;
                }
                if (start) s.initials.append(from, p);
                start = false;
            }
            m_servers.push_back(std::move(s));
        }
    }

    void openServer(int index) {
        m_onBar = false;
        if (index == m_shownServer) return;
        m_shownServer = index;
        m_listScroll = 0;
        if (index == 0) {
            m_rowSel = firstItem(m_dms);
            return;
        }
        m_channels.clear();
        m_channelsLoaded = false;
        m_channelsError.clear();
        m_rowSel = -1;
        m_channelsReq = g_net.api("GET", "/guilds/" + m_servers[(size_t)index].id + "/channels");
        m_channelsFor = index;
    }

    void buildChannels() {
        if (m_channelsFor != m_shownServer) return;
        m_channelsLoaded = true;
        if (m_channelsReq->status != 200) {
            m_channelsError = problem(*m_channelsReq);
            return;
        }
        struct Ch { std::string id, name, parent; long pos, type; };
        std::vector<Ch> cats, texts;
        Json doc(m_channelsReq->body);
        size_t i, n;
        yyjson_val* c;
        yyjson_arr_foreach(doc.root(), i, n, c) {
            Ch ch{str(c, "id"), cleanName(str(c, "name")), str(c, "parent_id"), num(c, "position"), num(c, "type")};
            if (ch.type == 4) cats.push_back(ch);
            else if (ch.type == 0 || ch.type == 5) texts.push_back(ch); // text and announcements
        }
        auto byPos = [](const Ch& a, const Ch& b) { return a.pos < b.pos; };
        std::sort(cats.begin(), cats.end(), byPos);
        std::sort(texts.begin(), texts.end(), byPos);
        for (const Ch& t : texts)
            if (t.parent.empty()) m_channels.push_back({Row::Channel, t.id, t.name, "", "", ""});
        for (const Ch& cat : cats) {
            bool header = false;
            for (const Ch& t : texts) {
                if (t.parent != cat.id) continue;
                if (!header) {
                    std::string upper = cat.name;
                    for (char& ch : upper) ch = (char)std::toupper((unsigned char)ch);
                    m_channels.push_back({Row::Header, "", upper, "", "", ""});
                    header = true;
                }
                m_channels.push_back({Row::Channel, t.id, t.name, "", "", ""});
            }
        }
        if (m_channels.empty()) m_channelsError = "No text channels you can open here.";
        m_rowSel = firstItem(m_channels);
    }

    void openChat(const Row& row) {
        m_chat = true;
        m_chatId = row.id;
        m_chatTitle = row.title;
        m_chatIsChannel = row.kind == Row::Channel;
        m_chatGuild = m_chatIsChannel ? m_servers[(size_t)m_shownServer].id : std::string();
        m_msgs.clear();
        m_chatLoaded = false;
        m_chatError.clear();
        m_chatNote.clear();
        m_chatScroll = 0;
        m_msgReq.reset(); // a reply for the last chat is ignored
        {
            std::lock_guard<std::mutex> lk(g_chatMu);
            g_openChannel = m_chatId;
            g_openGuild = m_chatGuild;
            g_subscribe = !m_chatGuild.empty();
            g_typingUntil = 0;
        }
        g_chatChanged = false;
        pollMessages();
    }

    void closeChat() {
        m_chat = false;
        std::lock_guard<std::mutex> lk(g_chatMu);
        g_openChannel.clear();
        g_openGuild.clear();
    }

    void pollMessages() {
        m_lastPoll = armGetSystemTick();
        m_msgReq = g_net.api("GET", "/channels/" + m_chatId + "/messages?limit=30");
        m_msgFor = m_chatId;
    }

    void takeMessages() {
        if (m_msgFor != m_chatId) return;
        if (m_msgReq->status != 200) {
            if (!m_chatLoaded) m_chatError = problem(*m_msgReq);
            m_chatLoaded = true;
            return;
        }
        m_chatLoaded = true;
        m_chatError.clear();
        Json doc(m_msgReq->body);
        std::vector<Msg> fresh;
        size_t i, n;
        yyjson_val* m;
        yyjson_arr_foreach(doc.root(), i, n, m) {
            Msg msg;
            msg.id = str(m, "id");
            msg.time = whenOf(msg.id);
            yyjson_val* author = yyjson_obj_get(m, "author");
            msg.authorId = str(author, "id");
            std::string nm = str(author, "global_name");
            msg.author = cleanName(nm.empty() ? str(author, "username") : nm);
            msg.avatar = avatarUrl(author);
            msg.text = messageText(str(m, "content"), m, msg.emojis, &msg.media);
            yyjson_val* edited = yyjson_obj_get(m, "edited_timestamp");
            if (edited && !yyjson_is_null(edited) && !msg.text.empty()) msg.text += " (edited)";
            // A reply: who and what it answers, on one line above it.
            yyjson_val* ref = yyjson_obj_get(m, "referenced_message");
            if (yyjson_is_obj(ref)) {
                yyjson_val* who = yyjson_obj_get(ref, "author");
                std::string rn = str(who, "global_name");
                std::vector<std::string> ignored;
                const std::string said = withoutMarks(messageText(str(ref, "content"), ref, ignored, nullptr));
                msg.reply = cleanName(rn.empty() ? str(who, "username") : rn) + ": "
                    + (said.empty() ? std::string("(a picture)") : said);
            }
            size_t k, cnt;
            yyjson_val* a;
            yyjson_arr_foreach(yyjson_obj_get(m, "attachments"), k, cnt, a) {
                const std::string type = str(a, "content_type"), file = str(a, "filename");
                if (type.rfind("image/", 0) == 0 && msg.media.size() < 2) {
                    Media md;
                    md.w = (int)num(a, "width");
                    md.h = (int)num(a, "height");
                    md.animated = type == "image/gif";
                    md.fallback = sized(str(a, "proxy_url"), md.w, md.h);
                    md.url = md.animated ? str(a, "url") : md.fallback;
                    msg.media.push_back(std::move(md));
                } else {
                    msg.text += (msg.text.empty() ? "" : "\n") + std::string("[file] ") + forScreen(file);
                }
            }
            yyjson_arr_foreach(yyjson_obj_get(m, "embeds"), k, cnt, a) {
                if (msg.media.size() >= 2) break;
                const std::string type = str(a, "type");
                yyjson_val* thumb = yyjson_obj_get(a, "thumbnail");
                yyjson_val* image = yyjson_obj_get(a, "image");
                if (isDiscordPicture(str(a, "url")) || isDiscordPicture(str(thumb, "url"))
                    || isDiscordPicture(str(image, "url")))
                    continue; // an emoji or sticker link: shown from the link already
                Media md;
                if (type == "gifv") {
                    md.w = (int)num(thumb, "width");
                    md.h = (int)num(thumb, "height");
                    md.fallback = sized(str(thumb, "proxy_url"), md.w, md.h);
                    const std::string gif = gifOf(a);
                    md.animated = !gif.empty();
                    md.url = md.animated ? gif : md.fallback;
                    // A message that's only the GIF's link shows just the GIF, like Discord.
                    const std::string content = trimmed(str(m, "content"));
                    if (content.rfind("http", 0) == 0 && content.find(' ') == std::string::npos) msg.text.clear();
                } else if (image || type == "image") {
                    yyjson_val* src = image ? image : thumb;
                    md.w = (int)num(src, "width");
                    md.h = (int)num(src, "height");
                    md.fallback = sized(str(src, "proxy_url"), md.w, md.h);
                    const std::string orig = str(src, "url");
                    md.animated = orig.size() > 4 && orig.compare(orig.size() - 4, 4, ".gif") == 0;
                    md.url = md.animated ? orig : md.fallback;
                    if (type == "image" && trimmed(str(m, "content")) == str(a, "url")) msg.text.clear();
                } else {
                    if (msg.text.empty() && !str(a, "title").empty()) msg.text = cleanName(str(a, "title"));
                    continue;
                }
                if (!md.url.empty()) msg.media.push_back(std::move(md));
            }
            yyjson_arr_foreach(yyjson_obj_get(m, "sticker_items"), k, cnt, a) {
                const long format = num(a, "format_type");
                if (format == 3 || msg.media.size() >= 2) { // Lottie stickers aren't pictures
                    msg.text += (msg.text.empty() ? "" : " ") + std::string("[sticker] ") + cleanName(str(a, "name"));
                    continue;
                }
                msg.media.push_back(stickerMedia(str(a, "id"), format == 4));
            }
            yyjson_arr_foreach(yyjson_obj_get(m, "reactions"), k, cnt, a) {
                if (msg.reactions.size() >= 8) break;
                yyjson_val* e = yyjson_obj_get(a, "emoji");
                ReactionPill pill;
                pill.count = (int)num(a, "count");
                pill.mine = yyjson_get_bool(yyjson_obj_get(a, "me"));
                pill.emoji = !str(e, "id").empty() ? customEmojiUrl(str(e, "id")) : twemoji(codepoints(str(e, "name")));
                msg.reactions.push_back(std::move(pill));
            }
            if (msg.text.empty() && msg.media.empty()) msg.text = "[embed]";
            // Only emoji (up to 27): big, like Discord.
            msg.big = !msg.emojis.empty() && msg.emojis.size() <= 27;
            for (const char* p = msg.text.data(), *end = p + msg.text.size(); msg.big && p < end;) {
                const uint32_t cp = nextCp(p, end);
                if (cp != ' ' && !isMark(cp)) msg.big = false;
            }
            // Keep the lines already worked out for messages that didn't change.
            for (const Msg& old : m_msgs)
                if (old.id == msg.id && old.text == msg.text && old.wrapped) {
                    msg.lines = old.lines;
                    msg.wrapped = true;
                }
            fresh.push_back(std::move(msg));
        }
        std::reverse(fresh.begin(), fresh.end()); // oldest first
        // One person's messages a few minutes apart share one name and picture.
        for (size_t k = 0; k < fresh.size(); ++k)
            fresh[k].header = k == 0 || fresh[k].authorId != fresh[k - 1].authorId || !fresh[k].reply.empty()
                || ((snowflake(fresh[k].id) >> 22) - (snowflake(fresh[k - 1].id) >> 22)) > 7u * 60u * 1000u;
        m_msgs = std::move(fresh);
    }

    // ------------------------------------------------------------ keyboard

    // Keyboard layers, 4 rows of 10 keys: letters, shifted, accents, shifted accents.
    static constexpr const char* kLayers[4][4] = {
        {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"},
        {"!@#$%&*()-", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:/"},
        {"àáâäãåæçèé", "êëìíîïñòóô", "öõøœùúûüýÿ", "ß¡¿€£¥©°…~"},
        {"ÀÁÂÄÃÅÆÇÈÉ", "ÊËÌÍÎÏÑÒÓÔ", "ÖÕØŒÙÚÛÜÝŸ", "_+=[]{}<>|"},
    };
    struct Special {
        const char* label;
        int start, span;
    };
    // The bottom row: Shift | àé | Space | Emoji | GIF | Delete | Send.
    static constexpr Special kSpecials[7] = {{"Shift", 0, 1}, {"àé", 1, 1}, {"Space", 2, 3}, {"", 5, 1},
                                             {"GIF", 6, 1},   {"Del", 7, 1}, {"Send", 8, 2}};

    std::string& typed() { return m_typing == Typing::Message ? m_draft : m_query; }

    void openKeyboard(Typing what) {
        m_keyboard = true;
        m_typing = what;
        m_emojiOpen = false;
        m_kx = 0;
        m_ky = 1;
    }

    int specialKey() const {
        for (int k = 0; k < 7; ++k)
            if (m_kx >= kSpecials[k].start && m_kx < kSpecials[k].start + kSpecials[k].span) return k;
        return 0;
    }

    std::string keyLabel(int row, int col) const {
        const char* text = kLayers[(m_accents ? 2 : 0) + (m_shift ? 1 : 0)][row];
        const char* p = text, *end = text + std::strlen(text);
        for (int i = 0; i < col && p < end; ++i) nextCp(p, end);
        const char* from = p;
        if (p < end) nextCp(p, end);
        return std::string(from, p);
    }

    bool keyboardInput(u64 down, u64 dir) {
        if (dir & HidNpadButton_AnyUp) m_ky = std::max(0, m_ky - 1);
        if (dir & HidNpadButton_AnyDown) m_ky = std::min(4, m_ky + 1);
        if (m_ky < 4) {
            if (dir & HidNpadButton_AnyLeft) m_kx = (m_kx + 9) % 10;
            if (dir & HidNpadButton_AnyRight) m_kx = (m_kx + 1) % 10;
        } else {
            const int key = specialKey();
            if (dir & HidNpadButton_AnyLeft) m_kx = kSpecials[(key + 6) % 7].start;
            if (dir & HidNpadButton_AnyRight) m_kx = kSpecials[(key + 1) % 7].start;
        }
        if (down & HidNpadButton_A) press();
        if (down & HidNpadButton_X) typed() += ' ';
        if (down & HidNpadButton_Y) m_shift = !m_shift;
        if (down & HidNpadButton_ZL) m_accents = !m_accents;
        if (down & HidNpadButton_L) openEmoji();
        if ((down & HidNpadButton_R) && m_typing == Typing::Message) openGifs();
        if (down & HidNpadButton_Plus) submit();
        if (down & HidNpadButton_B) {
            if (typed().empty()) m_keyboard = false;
            else popToken(typed());
        }
        return true;
    }

    void press() {
        if (m_ky < 4) {
            if (typed().size() < 1900) typed() += keyLabel(m_ky, m_kx);
            m_shift = false;
            return;
        }
        switch (specialKey()) {
            case 0: m_shift = !m_shift; break;
            case 1: m_accents = !m_accents; break;
            case 2: typed() += ' '; break;
            case 3: openEmoji(); break;
            case 4: if (m_typing == Typing::Message) openGifs(); break;
            case 5: popToken(typed()); break;
            case 6: submit(); break;
        }
    }

    void submit() {
        if (m_typing == Typing::GifSearch) {
            m_keyboard = false;
            m_emojiOpen = false;
            loadGifs();
            return;
        }
        sendDraft();
    }

    void sendDraft() {
        if (m_draft.empty() || m_sendReq) return;
        sendContent(m_draft);
        m_draft.clear();
        m_keyboard = false;
        m_emojiOpen = false;
    }

    void sendContent(const std::string& content) {
        m_sendReq = g_net.api("POST", "/channels/" + m_chatId + "/messages",
                              "{\"content\":" + jsonString(content) + ",\"tts\":false}");
        m_chatNote.clear();
        m_chatScroll = 0;
    }

    // ------------------------------------------------------------ emoji picker

    void openEmoji() {
        m_emojiOpen = !m_emojiOpen;
        m_eSel = 0;
        m_eScroll = 0;
        if (m_emojiOpen && !m_chatGuild.empty() && !m_serverEmoji.count(m_chatGuild) && !m_emojiReq) {
            m_serverEmoji[m_chatGuild]; // asked once per server
            m_emojiReq = g_net.api("GET", "/guilds/" + m_chatGuild + "/emojis");
            m_emojiFor = m_chatGuild;
        }
    }

    void takeServerEmoji() {
        if (m_emojiReq->status != 200) return;
        Json doc(m_emojiReq->body);
        std::vector<EmojiItem>& items = m_serverEmoji[m_emojiFor];
        size_t i, n;
        yyjson_val* e;
        yyjson_arr_foreach(doc.root(), i, n, e) {
            if (items.size() >= 120) break;
            const std::string id = str(e, "id"), name = str(e, "name");
            if (id.empty() || name.empty() || (yyjson_obj_get(e, "available") && !yyjson_get_bool(yyjson_obj_get(e, "available"))))
                continue;
            const bool moving = yyjson_get_bool(yyjson_obj_get(e, "animated"));
            items.push_back({std::string(moving ? "<a:" : "<:") + name + ":" + id + ">", customEmojiUrl(id)});
        }
    }

    std::vector<const EmojiItem*> emojiChoices() const {
        std::vector<const EmojiItem*> out;
        auto it = m_serverEmoji.find(m_chatGuild);
        if (!m_chatGuild.empty() && it != m_serverEmoji.end())
            for (const EmojiItem& e : it->second) out.push_back(&e);
        for (const EmojiItem& e : m_standardEmoji) out.push_back(&e);
        return out;
    }

    bool emojiInput(u64 down, u64 dir) {
        const int count = (int)emojiChoices().size();
        if (dir & HidNpadButton_AnyLeft) m_eSel = std::max(0, m_eSel - 1);
        if (dir & HidNpadButton_AnyRight) m_eSel = std::min(count - 1, m_eSel + 1);
        if (dir & HidNpadButton_AnyUp) m_eSel = std::max(0, m_eSel - 10);
        if (dir & HidNpadButton_AnyDown) m_eSel = std::min(count - 1, m_eSel + 10);
        if ((down & HidNpadButton_A) && m_eSel >= 0 && m_eSel < count && typed().size() < 1900)
            typed() += emojiChoices()[(size_t)m_eSel]->insert;
        if (down & HidNpadButton_X) typed() += ' ';
        if (down & HidNpadButton_Plus) submit();
        if (down & (HidNpadButton_B | HidNpadButton_L)) m_emojiOpen = false;
        return true;
    }

    // ------------------------------------------------------------ GIF picker

    void openGifs() {
        m_gifsOpen = true;
        m_keyboard = false;
        m_emojiOpen = false;
        m_gSel = 0;
        m_gScroll = 0;
        if (m_gifList.empty() && !m_gifReq) loadGifs();
    }

    void loadGifs() {
        m_gifList.clear();
        m_gSel = 0;
        m_gScroll = 0;
        m_gifError.clear();
        const std::string q = trimmed(m_query);
        m_gifReq = g_net.api("GET", q.empty() ? "/gifs/trending-gifs?media_format=gif&provider=tenor&locale=en-US"
                                              : "/gifs/search?q=" + urlEncode(q)
                                                    + "&media_format=gif&provider=tenor&locale=en-US");
        m_gifLoading = true;
    }

    void takeGifs() {
        m_gifLoading = false;
        if (m_gifReq->status != 200) {
            m_gifError = problem(*m_gifReq);
            return;
        }
        Json doc(m_gifReq->body);
        yyjson_val* list = yyjson_is_arr(doc.root()) ? doc.root() : yyjson_obj_get(doc.root(), "gifs");
        size_t i, n;
        yyjson_val* g;
        yyjson_arr_foreach(list, i, n, g) {
            if (m_gifList.size() >= 40) break;
            GifItem item;
            item.send = str(g, "url");
            std::string src = str(g, "gif_src");
            if (src.empty()) src = str(g, "src");
            item.preview = tenorTiny(src);
            if (item.preview.empty()) item.preview = str(g, "preview");
            if (!item.send.empty() && !item.preview.empty()) m_gifList.push_back(std::move(item));
        }
        if (m_gifList.empty()) m_gifError = "No GIFs found.";
    }

    bool gifInput(u64 down, u64 dir) {
        const int count = (int)m_gifList.size();
        if (dir & HidNpadButton_AnyLeft) m_gSel = std::max(0, m_gSel - 1);
        if (dir & HidNpadButton_AnyRight) m_gSel = std::min(std::max(0, count - 1), m_gSel + 1);
        if (dir & HidNpadButton_AnyUp) m_gSel = std::max(0, m_gSel - 2);
        if (dir & HidNpadButton_AnyDown) m_gSel = std::min(std::max(0, count - 1), m_gSel + 2);
        if (down & HidNpadButton_B) m_gifsOpen = false;
        if (down & HidNpadButton_Y) openKeyboard(Typing::GifSearch);
        if ((down & HidNpadButton_A) && m_gSel >= 0 && m_gSel < count && !m_sendReq) {
            sendContent(m_gifList[(size_t)m_gSel].send);
            m_gifsOpen = false;
        }
        return true;
    }

    // ------------------------------------------------------------ drawing

    void draw(tsl::gfx::Renderer* r) {
        r->fillScreen(r->a(kBg)); // one fast fill; drawRect goes pixel by pixel
        if (m_gifsOpen) drawGifs(r);
        else if (m_chat) drawChat(r);
        else drawLists(r);
        if (m_keyboard) drawKeyboard(r, 12, kH - 252, kW - 24);
    }

    // Your panel, as at the bottom of Discord's channel list, with the buttons.
    void drawPanel(tsl::gfx::Renderer* r) {
        const s32 x = kRailW, y = kH - kPanelH, w = kW - kRailW;
        r->drawRect(x, y, w, kPanelH, r->a(kPanelBg));
        const std::string hint1 = m_onBar ? " Open   Hide" : " Open   Servers";
        const std::string hint2 = " Accounts   Close";
        const s32 hw = std::max(widthOf(r, hint1, 13), widthOf(r, hint2, 13));
        r->drawString(hint1.c_str(), false, kW - 10 - hw, y + 21, 13, r->a(kMuted));
        r->drawString(hint2.c_str(), false, kW - 10 - hw, y + 40, 13, r->a(kMuted));
        drawRound(r, m_meAvatar, firstLetter(m_account), x + 8, y + 10, 32);
        const std::string status = !m_myStatus.empty() ? m_myStatus : m_presencesLoaded ? "online" : "";
        statusDot(r, x + 35, y + 37, status, kPanelBg);
        const s32 nameW = w - 56 - hw - 18;
        const std::string sub = !m_myGame.empty() ? "Playing " + m_myGame
                              : !status.empty() ? statusWord(status) : "Connecting...";
        r->drawString(fit(r, m_account.empty() ? "You" : m_account, 15, nameW).c_str(), false, x + 48, y + 23, 15,
                      r->a(kWhite));
        r->drawString(fit(r, sub, 12, nameW).c_str(), false, x + 48, y + 40, 12, r->a(kMuted));
    }

    void drawAccounts(tsl::gfx::Renderer* r) {
        const s32 rows = std::max<s32>(1, (s32)m_accountList.size());
        const s32 h = 44 + rows * 46 + (m_accountList.size() < 2 ? 22 : 0);
        const s32 x = kRailW + 10, w = kW - kRailW - 20, y = kH - kPanelH - 10 - h;
        roundRect(r, x, y, w, h, 8, kRailBg);
        r->drawString("Switch account", false, x + 14, y + 28, 17, r->a(kWhite));
        s32 ry = y + 40;
        for (int i = 0; i < (int)m_accountList.size(); ++i) {
            const Account& a = m_accountList[(size_t)i];
            if (i == m_accountSel) roundRect(r, x + 6, ry, w - 12, 42, 4, kSelected);
            drawRound(r, a.avatar, firstLetter(a.name), x + 14, ry + 5, 32);
            const std::string name = a.name.empty() ? std::string("Account") : a.name;
            r->drawString(fit(r, name, 16, w - 140).c_str(), false, x + 56, ry + 27, 16, r->a(kText));
            if ((size_t)i == m_current) r->drawString("in use", false, x + w - 60, ry + 27, 13, r->a(kMuted));
            ry += 46;
        }
        if (m_accountList.size() < 2)
            r->drawString("Add accounts in HOME: Social > Discord.", false, x + 14, ry + 14, 13, r->a(kMuted));
        r->drawString(" Switch   Close", false, x + w - 140, y + 28, 13, r->a(kMuted));
    }

    void drawLists(tsl::gfx::Renderer* r) {
        // The server bar.
        r->drawRect(0, 0, kRailW, kH, r->a(kRailBg));
        const s32 item = 58, top = 10, barH = kH - top;
        if (m_serverSel * item < m_barScroll) m_barScroll = m_serverSel * item;
        if (m_serverSel * item + item + 10 > m_barScroll + barH) m_barScroll = m_serverSel * item + item + 10 - barH;
        {
        Scissor clip(r, 0, top, kRailW, barH);
        for (int i = 0; i < (int)m_servers.size(); ++i) {
            const s32 y = top + i * item - m_barScroll + (i > 0 ? 10 : 0);
            if (y + item < 0 || y > kH) continue;
            const Server& s = m_servers[(size_t)i];
            if (i == 0) { // Direct Messages: Discord's blurple
                r->drawCircle(kRailW / 2, y + 24, 24, true, r->a(kBlurple));
                r->drawString("DM", false, kRailW / 2 - widthOf(r, "DM", 17) / 2, y + 30, 17, r->a(kWhite));
            } else {
                drawRound(r, s.icon, s.initials, (kRailW - 48) / 2, y, 48);
            }
            if (i == m_shownServer) r->drawRect(0, y + 8, 4, 32, r->a(kWhite)); // the white pill
            if (i == m_serverSel && m_onBar) {
                r->drawCircle(kRailW / 2, y + 24, 26, false, r->a(kWhite));
                r->drawCircle(kRailW / 2, y + 24, 27, false, r->a(kWhite));
            }
            if (i == 0) r->drawRect(18, y + 56, kRailW - 36, 2, r->a(kInput));
        }
        }

        // The list: DMs or the server's channels.
        const s32 lx = kRailW, lw = kW - kRailW;
        const std::string title = m_shownServer == 0 ? "Direct Messages" : m_servers[(size_t)m_shownServer].name;
        r->drawString(fit(r, title, 19, lw - 28).c_str(), false, lx + 14, 32, 19, r->a(kWhite));
        r->drawRect(lx, 48, lw, 1, r->a(kRailBg));

        std::vector<Row>& rows = list();
        const bool loaded = m_shownServer == 0 ? m_dmsLoaded : m_channelsLoaded;
        const std::string& error = m_shownServer == 0 ? m_dmsError : m_channelsError;
        const s32 y0 = 52, h = kH - kPanelH - y0;
        if (rows.size() <= (m_shownServer == 0 ? 1u : 0u)) {
            drawText(r, !loaded ? "Loading..." : error, lx + 14, y0 + 30, lw - 28, loaded ? kError : kMuted);
        } else {
            auto heightOf = [](const Row& row) { return row.kind == Row::Header ? 30 : row.kind == Row::Dm ? 48 : 36; };
            s32 selTop = 0, selH = 0, total = 0;
            for (int i = 0; i < (int)rows.size(); ++i) {
                if (i == m_rowSel) {
                    selTop = total;
                    selH = heightOf(rows[(size_t)i]);
                }
                total += heightOf(rows[(size_t)i]);
            }
            if (m_rowSel >= 0) {
                if (selTop < m_listScroll) m_listScroll = std::max(0, selTop - 30);
                if (selTop + selH > m_listScroll + h) m_listScroll = selTop + selH - h;
            }
            m_listScroll = std::clamp(m_listScroll, 0, std::max(0, total - h));
            Scissor clip(r, lx, y0, lw, h);
            s32 rowY = y0 - m_listScroll;
            for (int i = 0; i < (int)rows.size(); ++i) {
                Row& row = rows[(size_t)i];
                const s32 rh = heightOf(row);
                if (rowY + rh >= y0 && rowY <= y0 + h) {
                    const bool sel = i == m_rowSel;
                    const tsl::Color back = sel ? (m_onBar ? kInput : kSelected) : kBg;
                    if (row.kind == Row::Header) {
                        r->drawString(fit(r, row.title, 12, lw - 28).c_str(), false, lx + 14, rowY + 22, 12,
                                      r->a(kMuted));
                    } else if (row.kind == Row::Dm) {
                        if (sel) roundRect(r, lx + 8, rowY + 2, lw - 16, rh - 4, 4, back);
                        drawRound(r, row.avatar, firstLetter(row.title), lx + 14, rowY + 6, 36);
                        // Friends the gateway doesn't list are offline.
                        const Presence* p = presence(row.userId);
                        if (!row.userId.empty() && (p || m_presencesLoaded))
                            statusDot(r, lx + 44, rowY + 36, p ? p->status : "offline", back);
                        const s32 tx = lx + 60;
                        if (row.shown.empty()) row.shown = fit(r, row.title, 17, lx + lw - tx - 14);
                        const bool away = !p || p->status == "offline";
                        const tsl::Color nameColor = sel && !m_onBar ? kWhite : away ? kMuted : kText;
                        if (p && !p->activity.empty()) {
                            r->drawString(row.shown.c_str(), false, tx, rowY + 22, 17, r->a(nameColor));
                            r->drawString(fit(r, p->activity, 12, lx + lw - tx - 14).c_str(), false, tx, rowY + 38,
                                          12, r->a(kMuted));
                        } else {
                            r->drawString(row.shown.c_str(), false, tx, rowY + 30, 17, r->a(nameColor));
                        }
                    } else {
                        if (sel) roundRect(r, lx + 8, rowY + 2, lw - 16, rh - 4, 4, back);
                        r->drawString("#", false, lx + 16, rowY + 25, 20, r->a(kChannel));
                        const s32 tx = lx + 36;
                        if (row.shown.empty()) row.shown = fit(r, row.title, 17, lx + lw - tx - 14);
                        r->drawString(row.shown.c_str(), false, tx, rowY + 24, 17,
                                      r->a(sel && !m_onBar ? kWhite : kChannel));
                    }
                }
                rowY += rh;
            }
        }
        drawPanel(r);
        if (m_accountsOpen) drawAccounts(r);
    }

    static s32 mediaHeight(const Media& md) {
        const float w = md.w > 0 ? (float)md.w : 260.f, hh = md.h > 0 ? (float)md.h : 146.f;
        const float s = std::min(1.f, std::min(260.f / w, 160.f / hh));
        return std::max(24, (int)(hh * s));
    }

    static constexpr s32 kPillW = 54, kPillGap = 6;

    s32 messageHeight(const Msg& m, s32 tw) const {
        s32 h = (m.reply.empty() ? 0 : 18) + (m.header ? 26 : 2);
        h += (s32)m.lines.size() * (m.big ? 42 : 21);
        for (const Media& md : m.media) h += mediaHeight(md) + 6;
        if (!m.reactions.empty()) {
            const s32 perRow = std::max<s32>(1, tw / (kPillW + kPillGap));
            h += (((s32)m.reactions.size() + perRow - 1) / perRow) * 28;
        }
        return std::max(h, m.header ? 44 + (m.reply.empty() ? 0 : 18) : 0) + 6;
    }

    void drawChat(tsl::gfx::Renderer* r) {
        // The channel's name, as Discord's header.
        r->drawString(m_chatIsChannel ? "#" : "@", false, 16, 33, 22, r->a(kChannel));
        r->drawString(fit(r, m_chatTitle, 19, kW - 60).c_str(), false, 38, 32, 19, r->a(kWhite));
        r->drawRect(0, 48, kW, 1, r->a(kRailBg));

        const s32 top = 52;
        const s32 bottom = m_keyboard ? kH - 258 : kH - 74;
        const s32 tx = 60, tw = kW - tx - 12;
        if (!m_chatLoaded) {
            drawText(r, "Loading...", 16, top + 30, kW - 32, kMuted);
        } else if (!m_chatError.empty()) {
            drawText(r, m_chatError, 16, top + 30, kW - 32, kError);
        } else if (m_msgs.empty()) {
            drawText(r, "No messages yet. Say hi!", 16, top + 30, kW - 32, kMuted);
        } else {
            for (Msg& m : m_msgs)
                if (!m.wrapped) {
                    m.lines = m.text.empty() ? std::vector<std::string>()
                                             : wrapRich(r, m.text, 17, tw, m.big ? 36 : 18);
                    m.wrapped = true;
                }
            s32 total = 0;
            for (const Msg& m : m_msgs) total += messageHeight(m, tw);
            m_chatScroll = std::clamp(m_chatScroll, 0, std::max(0, total - (bottom - top)));
            Scissor clip(r, 0, top, kW, bottom - top);
            s32 cy = bottom + m_chatScroll;
            for (auto it = m_msgs.rbegin(); it != m_msgs.rend(); ++it) {
                const s32 mh = messageHeight(*it, tw);
                cy -= mh;
                if (cy > bottom) continue;
                if (cy + mh < top) break;
                s32 my = cy;
                if (!it->reply.empty()) { // the message it answers
                    r->drawRect(30, my + 9, 22, 2, r->a(kInput));
                    r->drawRect(30, my + 9, 2, 10, r->a(kInput));
                    r->drawString(fit(r, it->reply, 13, tw).c_str(), false, tx, my + 15, 13, r->a(kMuted));
                    my += 18;
                }
                s32 ly = my + 4;
                if (it->header) {
                    drawRound(r, it->avatar, firstLetter(it->author), 12, my + 4, 36);
                    if (const Presence* p = presence(it->authorId)) statusDot(r, 42, my + 34, p->status, kBg);
                    const std::string name = fit(r, it->author, 16, tw - 110);
                    r->drawString(name.c_str(), false, tx, my + 20, 16, r->a(kWhite));
                    r->drawString(it->time.c_str(), false, tx + widthOf(r, name, 16) + 8, my + 20, 12, r->a(kMuted));
                    ly = my + 26;
                }
                const s32 lineH = it->big ? 42 : 21;
                for (const std::string& line : it->lines) {
                    drawRich(r, line, tx, ly + (it->big ? 36 : 16), 17, kText, it->emojis, it->big ? 36 : 18);
                    ly += lineH;
                }
                for (const Media& md : it->media) {
                    const float w = md.w > 0 ? (float)md.w : 260.f, hh = md.h > 0 ? (float)md.h : 146.f;
                    const float s = std::min(1.f, std::min(260.f / w, 160.f / hh));
                    const s32 boxW = std::max(24, (int)(w * s)), boxH = mediaHeight(md);
                    if (const Picture* p = picture(md.url, md.fallback, 260, 160, md.animated, false))
                        blit(r, *p, tx, ly + 2);
                    else
                        roundRect(r, tx, ly + 2, boxW, boxH, 4, kInput);
                    ly += boxH + 6;
                }
                s32 px = tx;
                for (const ReactionPill& pill : it->reactions) { // reactions, as Discord's pills
                    if (px + kPillW > tx + tw) {
                        px = tx;
                        ly += 28;
                    }
                    roundRect(r, px, ly + 2, kPillW, 24, 6, pill.mine ? kMine : kInput);
                    if (const Picture* e = picture(pill.emoji, std::string(), 16, 16, false, false))
                        blit(r, *e, px + 7, ly + 6);
                    r->drawString(std::to_string(pill.count).c_str(), false, px + 28, ly + 19, 13, r->a(kText));
                    px += kPillW + kPillGap;
                }
            }
        }
        if (m_keyboard) return;
        std::string typing;
        {
            std::lock_guard<std::mutex> lk(g_chatMu);
            if (g_typingUntil > nowMs() && g_typingId != m_meId) {
                typing = g_typingName;
                for (const Msg& m : m_msgs)
                    if (typing.empty() && m.authorId == g_typingId) typing = m.author;
                if (typing.empty()) typing = m_chatIsChannel ? "Someone" : m_chatTitle;
                typing += " is typing...";
            }
        }
        if (!typing.empty())
            r->drawString(fit(r, typing, 13, kW - 40).c_str(), false, 16, kH - 58, 13, r->a(kMuted));
        // Discord's message box, with the buttons in it.
        const s32 by = kH - 52, bh = 40;
        roundRect(r, 12, by, kW - 24, bh, 8, kInput);
        const std::string keys = " Write   Emoji   GIF   Back";
        const s32 kw = widthOf(r, keys, 13);
        r->drawString(keys.c_str(), false, kW - 24 - kw, by + 25, 13, r->a(kMuted));
        const std::string prompt = !m_chatNote.empty() ? m_chatNote
                                 : m_sendReq ? "Sending..."
                                 : std::string("Message ") + (m_chatIsChannel ? "#" : "@") + m_chatTitle;
        r->drawString(fit(r, prompt, 15, kW - 64 - kw).c_str(), false, 26, by + 26, 15,
                      r->a(m_chatNote.empty() ? kChannel : kError));
    }

    void drawGifs(tsl::gfx::Renderer* r) {
        const std::string q = trimmed(m_query);
        r->drawString(fit(r, q.empty() ? "GIFs: trending" : "GIFs: " + q, 19, kW - 40).c_str(), false, 16, 32, 19,
                      r->a(kWhite));
        r->drawRect(0, 48, kW, 1, r->a(kRailBg));
        const s32 top = 56, bottom = m_keyboard ? kH - 258 : kH - 44;
        const s32 cw = (kW - 36) / 2, ch = 130;
        if (m_gifLoading || m_gifList.empty()) {
            drawText(r, m_gifLoading ? "Loading..." : m_gifError, 16, top + 30, kW - 32,
                     m_gifLoading ? kMuted : kError);
        } else {
            const s32 selRow = m_gSel / 2;
            if (selRow * (ch + 8) < m_gScroll) m_gScroll = selRow * (ch + 8);
            if (selRow * (ch + 8) + ch > m_gScroll + (bottom - top)) m_gScroll = selRow * (ch + 8) + ch - (bottom - top);
            Scissor clip(r, 0, top, kW, bottom - top);
            for (int i = 0; i < (int)m_gifList.size(); ++i) {
                const s32 x = 12 + (i % 2) * (cw + 12), y = top + (i / 2) * (ch + 8) - m_gScroll;
                if (y + ch < top || y > bottom) continue;
                const bool sel = i == m_gSel;
                roundRect(r, x, y, cw, ch, 6, sel ? kBlurple : kInput);
                // Every GIF still; the chosen one moves.
                if (const Picture* p = picture(m_gifList[(size_t)i].preview, std::string(), cw - 8, ch - 8, sel, false))
                    blit(r, *p, x + (cw - p->w) / 2, y + (ch - p->h) / 2);
            }
        }
        if (m_keyboard) return;
        const std::string keys = " Send   Search   Back";
        r->drawString(keys.c_str(), false, kW - 16 - widthOf(r, keys, 14), kH - 16, 14, r->a(kMuted));
    }

    void drawKeyboard(tsl::gfx::Renderer* r, s32 x, s32 y, s32 w) {
        // What's been typed (with its emoji): the end of it, two lines at most.
        roundRect(r, x - 4, y, w + 8, 52, 8, kInput);
        std::vector<std::string> emojis;
        const std::string shown = draftText(typed(), emojis) + "_";
        std::vector<std::string> lines = wrapRich(r, shown, 17, w - 12, 18);
        if (lines.size() > 2) lines.erase(lines.begin(), lines.end() - 2);
        s32 ty = y + 20;
        if (typed().empty())
            r->drawString(m_typing == Typing::GifSearch ? "Search Tenor" : "", false, x + 16, ty, 17, r->a(kChannel));
        for (const std::string& line : lines) {
            drawRich(r, line, x + 6, ty, 17, kText, emojis, 18);
            ty += 22;
        }
        const s32 ky = y + 58;
        if (m_emojiOpen) {
            drawEmojiPicker(r, x, ky, w);
            return;
        }
        const s32 kw = w / 10, kh = 30;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 10; ++col) {
                const s32 kx = x + col * kw, kyy = ky + row * (kh + 3);
                const bool sel = m_ky == row && m_kx == col;
                r->drawRect(kx + 1, kyy, kw - 2, kh, r->a(sel ? kBlurple : kInput));
                const std::string label = keyLabel(row, col);
                r->drawString(label.c_str(), false, kx + (kw - widthOf(r, label, 18)) / 2, kyy + 22, 18, r->a(kText));
            }
        const s32 sy = ky + 4 * (kh + 3);
        for (int k = 0; k < 7; ++k) {
            const s32 sx = x + kSpecials[k].start * kw, sw = kSpecials[k].span * kw;
            const bool sel = m_ky == 4 && specialKey() == k;
            const bool lit = (k == 0 && m_shift) || (k == 1 && m_accents) || (k == 6 && !typed().empty());
            r->drawRect(sx + 1, sy, sw - 2, kh, r->a(sel ? kBlurple : lit ? kSelected : kInput));
            if (k == 3) { // the emoji key: a smiley
                if (const Picture* e = picture(m_standardEmoji.empty() ? std::string() : m_standardEmoji[0].url,
                                               std::string(), 20, 20, false, false))
                    blit(r, *e, sx + (sw - 20) / 2, sy + 5);
                continue;
            }
            const char* label = k == 6 && m_typing == Typing::GifSearch ? "Search" : kSpecials[k].label;
            r->drawString(label, false, sx + (sw - widthOf(r, label, 14)) / 2, sy + 20, 14, r->a(kText));
        }
        const std::string keys = m_typing == Typing::GifSearch
            ? " Delete   Space   Shift   àé   Search"
            : " Del   Space   Shift   àé   Emoji   GIF   Send";
        r->drawString(keys.c_str(), false, x, sy + kh + 20, 13, r->a(kMuted));
    }

    void drawEmojiPicker(tsl::gfx::Renderer* r, s32 x, s32 y, s32 w) {
        const std::vector<const EmojiItem*> items = emojiChoices();
        const s32 cell = w / 10, rows = 4;
        const s32 selRow = m_eSel / 10;
        if (selRow < m_eScroll) m_eScroll = selRow;
        if (selRow >= m_eScroll + rows) m_eScroll = selRow - rows + 1;
        for (int i = m_eScroll * 10; i < (int)items.size() && i < (m_eScroll + rows) * 10; ++i) {
            const s32 cx = x + (i % 10) * cell, cy = y + (i / 10 - m_eScroll) * (cell + 2);
            if (i == m_eSel) roundRect(r, cx + 1, cy, cell - 2, cell, 6, kBlurple);
            if (const Picture* p = picture(items[(size_t)i]->url, std::string(), 30, 30, false, false))
                blit(r, *p, cx + (cell - p->w) / 2, cy + (cell - p->h) / 2);
        }
        const s32 hy = y + rows * (cell + 2) + 18;
        const std::string where = !m_chatGuild.empty() && m_serverEmoji.count(m_chatGuild)
                && !m_serverEmoji.at(m_chatGuild).empty()
            ? "This server's emoji first.  "
            : "";
        r->drawString((where + " Add   Send   Keyboard").c_str(), false, x, hy, 13, r->a(kMuted));
    }

    std::vector<Account> m_accountList;
    size_t m_current = 0;
    bool m_accountsOpen = false;
    int m_accountSel = 0;

    std::string m_account, m_meAvatar, m_meId;
    std::vector<Server> m_servers; // [0] = Direct Messages
    int m_serverSel = 0, m_shownServer = 0, m_channelsFor = -1;
    s32 m_barScroll = 0;
    bool m_onBar = false;
    std::vector<Row> m_dms, m_channels;
    int m_rowSel = -1;
    s32 m_listScroll = 0;
    bool m_dmsLoaded = false, m_channelsLoaded = false;
    std::string m_dmsError, m_channelsError;
    std::shared_ptr<Reply> m_meReq, m_dmReq, m_guildsReq, m_channelsReq, m_msgReq, m_sendReq, m_emojiReq, m_gifReq;
    std::map<std::string, Held> m_pictures;

    unsigned m_presenceSeen = ~0u;
    std::map<std::string, Presence> m_presences;
    bool m_presencesLoaded = false;
    std::string m_myStatus, m_myGame;

    bool m_chat = false, m_chatIsChannel = false;
    std::string m_chatId, m_chatTitle, m_chatGuild, m_msgFor, m_chatError, m_chatNote;
    std::vector<Msg> m_msgs;
    bool m_chatLoaded = false;
    s32 m_chatScroll = 0; // pixels up from the newest message
    u64 m_lastPoll = 0;

    bool m_keyboard = false, m_shift = false, m_accents = false;
    Typing m_typing = Typing::Message;
    std::string m_draft, m_query;
    int m_kx = 0, m_ky = 1;
    int m_heldFrames = 0;

    bool m_emojiOpen = false;
    int m_eSel = 0, m_eScroll = 0;
    std::vector<EmojiItem> m_standardEmoji;
    std::map<std::string, std::vector<EmojiItem>> m_serverEmoji; // by server id
    std::string m_emojiFor;

    bool m_gifsOpen = false, m_gifLoading = false;
    std::vector<GifItem> m_gifList;
    std::string m_gifError;
    int m_gSel = 0;
    s32 m_gScroll = 0;
};

class DiscordOverlay : public tsl::Overlay {
public:
    void initServices() override {
        tsl::cfg::extraLaunchCombo = kCombo;
        tsl::cfg::launchComboCloses = true;
        // Never over HOME (HomeNX's own Discord is there): only while its menu isn't running.
        tsl::cfg::showAllowed = [] {
            u64 pid = 0;
            return R_FAILED(pmdmntGetProcessId(&pid, kHomeMenu));
        };
        fsdevMountSdmc();
        // nifm for the connection state, pm:info + ns for the game being
        // played, time for when it started and for message times.
        m_nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
        m_pminfo = R_SUCCEEDED(pminfoInitialize());
        m_ns = R_SUCCEEDED(nsInitialize());
        m_time = R_SUCCEEDED(timeInitialize());
        // Sockets sized like Ultrahand's downloader (an overlay has a few MB in total).
        static const SocketInitConfig kSockets = {
            .tcp_tx_buf_size = 16 * 1024,
            .tcp_rx_buf_size = 32 * 1024,
            .tcp_tx_buf_max_size = 64 * 1024,
            .tcp_rx_buf_max_size = 96 * 1024,
            .udp_tx_buf_size = 0x2400,
            .udp_rx_buf_size = 0xA500,
            .sb_efficiency = 1,
            .bsd_service_type = BsdServiceType_Auto,
        };
        const Result sockRc = socketInitialize(&kSockets);
        m_sockets = R_SUCCEEDED(sockRc);
        // curl's TLS here is the console's ssl service, opened by curl_global_init.
        const CURLcode curlRc = curl_global_init(CURL_GLOBAL_DEFAULT);
        char buf[96];
        if (!m_sockets) {
            std::snprintf(buf, sizeof(buf), "The network didn't start (0x%X).", sockRc);
            g_startProblem = buf;
        } else if (curlRc != CURLE_OK) {
            std::snprintf(buf, sizeof(buf), "Secure connections didn't start: %s.", curl_easy_strerror(curlRc));
            g_startProblem = buf;
        }
    }

    void exitServices() override {
        g_gateway.stop();
        g_net.stop();
        curl_global_cleanup();
        if (m_sockets) socketExit();
        if (m_time) timeExit();
        if (m_ns) nsExit();
        if (m_pminfo) pminfoExit();
        if (m_nifm) nifmExit();
        fsdevUnmountAll();
    }

    void onShow() override { tsl::cfg::extraLaunchCombo = kCombo; }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        size_t current = 0;
        std::vector<Account> accounts = loadAccounts(current);
        if (!accounts.empty() && g_startProblem.empty()) {
            g_net.start(accounts[current].token);
            if (m_pminfo && m_ns) g_gateway.start(accounts[current].token);
        }
        return initially<DiscordGui>(std::move(accounts), current);
    }

private:
    bool m_sockets = false, m_nifm = false, m_pminfo = false, m_ns = false, m_time = false;
};

} // namespace

int main(int argc, char** argv) {
    return tsl::loop<DiscordOverlay>(argc, argv);
}
