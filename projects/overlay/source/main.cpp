// HomeNX Discord: your Discord DMs and servers on top of a running game.
//
// A Tesla/Ultrahand overlay. L+ZL shows and hides it (HomeNX adds that combo
// to Ultrahand's overlays.ini); B on the first screen closes it. It uses the
// account HomeNX's Discord panel signed in with (sdmc:/config/SwitchU/discord.json,
// then SwitchCord's accounts.json), talking to Discord's REST API: the chat you
// have open is read again every few seconds while the overlay is up.
//
// SPDX-License-Identifier: GPL-2.0-or-later (built with libtesla, GPL-2.0)

#include <bit> // libtesla uses std::bit_cast without including it

#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <curl/curl.h>
#include <yyjson.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kApi = "https://discord.com/api/v9";
constexpr const char* kUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36";
constexpr u64 kCombo = HidNpadButton_L | HidNpadButton_ZL;

const tsl::Color kText(0xF, 0xF, 0xF, 0xF);
const tsl::Color kMuted(0xA, 0xA, 0xB, 0xF);
const tsl::Color kBlurple(0x5, 0x6, 0xF, 0xF);
const tsl::Color kSelected(0x5, 0x6, 0xF, 0x5);
const tsl::Color kAuthor(0xC, 0xC, 0xF, 0xF);
const tsl::Color kError(0xF, 0x6, 0x6, 0xF);
const tsl::Color kLine(0x5, 0x5, 0x6, 0xF);
const tsl::Color kKey(0x3, 0x3, 0x4, 0xF);

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

std::string nameOf(yyjson_val* user) {
    std::string n = str(user, "global_name");
    return n.empty() ? str(user, "username") : n;
}

// Snowflakes as strings: longer is newer, then by digits.
bool newer(const std::string& a, const std::string& b) {
    return a.size() != b.size() ? a.size() > b.size() : a > b;
}

// ---------------------------------------------------------------- account

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

struct Account {
    std::string token, name;
};

// Same rules as HomeNX's panel: the current account, else the first one, never
// one that was signed out in HomeNX.
Account loadAccount() {
    constexpr const char* kHomeNX = "sdmc:/config/SwitchU/discord.json";
    constexpr const char* kSwitchCord = "sdmc:/switch/SwitchCord/accounts.json";
    std::string current;
    std::vector<std::string> signedOut;
    std::vector<std::pair<std::string, Account>> all;
    for (const char* path : {kHomeNX, kSwitchCord}) {
        Json doc(readFile(path));
        yyjson_val* root = doc.root();
        if (!root) continue;
        size_t i, n;
        yyjson_val* v;
        if (path == kHomeNX) {
            current = str(root, "current");
            yyjson_arr_foreach(yyjson_obj_get(root, "signed_out"), i, n, v)
                if (yyjson_is_str(v)) signedOut.push_back(yyjson_get_str(v));
            if (!str(root, "token").empty()) all.push_back({"", {str(root, "token"), ""}});
        }
        yyjson_arr_foreach(yyjson_obj_get(root, "accounts"), i, n, v) {
            const std::string id = str(v, "id"), token = str(v, "token");
            if (token.empty()) continue;
            if (!id.empty() && std::find(signedOut.begin(), signedOut.end(), id) != signedOut.end()) continue;
            all.push_back({id, {token, nameOf(v)}});
        }
    }
    if (all.empty()) return {};
    for (const auto& [id, acc] : all)
        if (!id.empty() && id == current) return acc;
    return all.front().second;
}

// ---------------------------------------------------------------- network

struct Reply {
    std::atomic<bool> done{false};
    long status = 0; // HTTP status, or minus the curl error
    std::string body;
};

// One thread runs the requests in order, so the overlay never waits on Discord.
class Net {
public:
    void start(std::string token) {
        m_token = std::move(token);
        m_thread = std::thread([this] { run(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_quit = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    std::shared_ptr<Reply> send(std::string method, std::string path, std::string body = {}) {
        auto reply = std::make_shared<Reply>();
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_queue.push_back({std::move(method), std::move(path), std::move(body), reply});
        }
        m_cv.notify_one();
        return reply;
    }

private:
    struct Job {
        std::string method, path, body;
        std::shared_ptr<Reply> reply;
    };

    static size_t onBody(char* p, size_t size, size_t n, void* user) {
        static_cast<std::string*>(user)->append(p, size * n);
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
                m_cv.wait(lk, [this] { return m_quit.load() || !m_queue.empty(); });
                if (m_quit) break;
                job = std::move(m_queue.front());
                m_queue.pop_front();
            }
            if (c) perform(c, job);
            else job.reply->status = -1;
            job.reply->done = true;
        }
        if (c) curl_easy_cleanup(c);
    }

    void perform(CURL* c, Job& job) {
        curl_easy_reset(c);
        const std::string url = std::string(kApi) + job.path;
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Authorization: " + m_token).c_str());
        headers = curl_slist_append(headers, (std::string("User-Agent: ") + kUserAgent).c_str());
        if (!job.body.empty()) headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 15000L);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onBody);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &job.reply->body);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, this);
        if (job.method == "POST") {
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, job.body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(job.body.size()));
        }
        const CURLcode rc = curl_easy_perform(c);
        long status = 0;
        if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        job.reply->status = rc == CURLE_OK ? status : -static_cast<long>(rc);
        curl_slist_free_all(headers);
    }

    std::string m_token;
    std::thread m_thread;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<Job> m_queue;
    std::atomic<bool> m_quit{false};
};

Net g_net;

std::string problem(const Reply& r) {
    if (r.status == 401) return "Discord signed this account out. Sign in again in HomeNX: Social > Discord.";
    if (r.status == 403) return "You can't see this.";
    if (r.status < 0) return "No connection to Discord. Check the console is online.";
    return "Discord said no (" + std::to_string(r.status) + ").";
}

// ---------------------------------------------------------------- text

s32 widthOf(tsl::gfx::Renderer* r, const std::string& s, float size) {
    return static_cast<s32>(r->drawString(s.c_str(), false, 0, 0, size, tsl::style::color::ColorTransparent).first);
}

void popChar(std::string& s) {
    while (!s.empty()) {
        const unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;
    }
}

std::string fit(tsl::gfx::Renderer* r, std::string s, float size, s32 maxW) {
    if (widthOf(r, s, size) <= maxW) return s;
    while (!s.empty() && widthOf(r, s + "...", size) > maxW) popChar(s);
    return s + "...";
}

std::vector<std::string> wrap(tsl::gfx::Renderer* r, const std::string& text, float size, s32 maxW) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        const std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::string line;
        size_t i = 0;
        while (i <= para.size()) {
            size_t sp = para.find(' ', i);
            std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (widthOf(r, candidate, size) <= maxW) {
                line = candidate;
            } else {
                if (!line.empty()) lines.push_back(line);
                // A word wider than the line goes over several lines.
                while (widthOf(r, word, size) > maxW) {
                    std::string head = word;
                    while (head.size() > 1 && widthOf(r, head, size) > maxW) popChar(head);
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

// Discord's markup in words: mentions, channels, custom emoji, timestamps.
std::string cleanContent(const std::string& s, yyjson_val* msg) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<') {
            const size_t end = s.find('>', i);
            if (end != std::string::npos && end - i < 96) {
                const std::string tag = s.substr(i + 1, end - i - 1);
                std::string repl;
                if (tag.rfind("@&", 0) == 0) {
                    repl = "@role";
                } else if (tag.rfind("@", 0) == 0) {
                    const std::string id = tag.substr(tag.size() > 1 && tag[1] == '!' ? 2 : 1);
                    repl = "@user";
                    size_t k, n;
                    yyjson_val* u;
                    yyjson_arr_foreach(yyjson_obj_get(msg, "mentions"), k, n, u)
                        if (str(u, "id") == id) repl = "@" + nameOf(u);
                } else if (tag.rfind("#", 0) == 0) {
                    repl = "#channel";
                } else if (tag.rfind(":", 0) == 0 || tag.rfind("a:", 0) == 0) {
                    const size_t a = tag.find(':'), b = tag.find(':', a + 1);
                    if (b != std::string::npos) repl = ":" + tag.substr(a + 1, b - a - 1) + ":";
                } else if (tag.rfind("t:", 0) == 0) {
                    repl = "(a time)";
                }
                if (!repl.empty()) {
                    out += repl;
                    i = end + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

// ---------------------------------------------------------------- the screen

struct Row {
    enum Kind { Header, Dm, Guild, Channel } kind;
    std::string id, title;
    std::string shown; // title cut to fit, made on first draw
};

class DiscordGui : public tsl::Gui {
public:
    DiscordGui(std::string account, bool signedIn) : m_account(std::move(account)) {
        if (!signedIn) {
            m_homeError = "Sign in to Discord in HomeNX first: Social > Discord.";
            m_homeLoaded = true;
            return;
        }
        m_dmReq = g_net.send("GET", "/users/@me/channels");
        m_guildsReq = g_net.send("GET", "/users/@me/guilds");
    }

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Discord", m_account.empty() ? "HomeNX" : m_account);
        frame->setContent(new tsl::elm::CustomDrawer(
            [this](tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h) { draw(r, x, y, w, h); }));
        return frame;
    }

    void update() override {
        if (m_dmReq && m_guildsReq && m_dmReq->done && m_guildsReq->done) {
            buildHome();
            m_dmReq.reset();
            m_guildsReq.reset();
        }
        if (m_channelsReq && m_channelsReq->done) {
            buildGuild();
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
        if (m_screen == Screen::Chat && !m_msgReq
            && armTicksToNs(armGetSystemTick() - m_lastPoll) > 4'000'000'000ULL)
            pollMessages();
    }

    bool handleInput(u64 down, u64 held, const HidTouchState&, HidAnalogStickState, HidAnalogStickState) override {
        const u64 dir = repeat(down, held);
        if (m_keyboard) return keyboardInput(down, dir);
        if (m_screen == Screen::Chat) {
            if (down & HidNpadButton_B) {
                m_screen = m_chatFrom;
                return true;
            }
            if (down & (HidNpadButton_A | HidNpadButton_Y)) {
                m_keyboard = true;
                m_kx = 0;
                m_ky = 1;
                return true;
            }
            if (dir & HidNpadButton_AnyUp) m_chatScroll += 60;
            if (dir & HidNpadButton_AnyDown) m_chatScroll = std::max(0, m_chatScroll - 60);
            return true;
        }
        std::vector<Row>& rows = m_screen == Screen::Home ? m_home : m_guild;
        int& sel = m_screen == Screen::Home ? m_homeSel : m_guildSel;
        if (down & HidNpadButton_B) {
            if (m_screen == Screen::Home) return false; // libtesla closes the overlay
            m_screen = Screen::Home;
            return true;
        }
        if (dir & (HidNpadButton_AnyUp | HidNpadButton_AnyDown)) {
            const int step = (dir & HidNpadButton_AnyUp) ? -1 : 1;
            for (int i = sel + step; i >= 0 && i < (int)rows.size(); i += step)
                if (rows[(size_t)i].kind != Row::Header) {
                    sel = i;
                    break;
                }
        }
        if ((down & HidNpadButton_A) && sel >= 0 && sel < (int)rows.size()) {
            const Row& row = rows[(size_t)sel];
            if (row.kind == Row::Guild) openGuild(row);
            else openChat(row);
        }
        return true;
    }

private:
    enum class Screen { Home, Guild, Chat };
    struct Msg {
        std::string id, author, text;
        std::vector<std::string> lines;
        bool wrapped = false;
    };

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

    static int firstItem(const std::vector<Row>& rows) {
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].kind != Row::Header) return (int)i;
        return -1;
    }

    void buildHome() {
        m_homeLoaded = true;
        m_home.clear();
        if (m_dmReq->status != 200 && m_guildsReq->status != 200) {
            m_homeError = problem(*m_dmReq);
            return;
        }
        struct Dm { std::string id, title, last; };
        std::vector<Dm> dms;
        Json dmDoc(m_dmReq->body);
        size_t i, n;
        yyjson_val* c;
        yyjson_arr_foreach(dmDoc.root(), i, n, c) {
            Dm dm{str(c, "id"), str(c, "name"), str(c, "last_message_id")};
            if (dm.title.empty()) {
                size_t k, m;
                yyjson_val* u;
                yyjson_arr_foreach(yyjson_obj_get(c, "recipients"), k, m, u)
                    dm.title += (dm.title.empty() ? "" : ", ") + nameOf(u);
            }
            if (!dm.id.empty()) dms.push_back(std::move(dm));
        }
        std::sort(dms.begin(), dms.end(), [](const Dm& a, const Dm& b) { return newer(a.last, b.last); });
        if (!dms.empty()) m_home.push_back({Row::Header, "", "DIRECT MESSAGES"});
        for (size_t k = 0; k < dms.size() && k < 40; ++k)
            m_home.push_back({Row::Dm, dms[k].id, dms[k].title.empty() ? "Conversation" : dms[k].title});

        Json guildDoc(m_guildsReq->body);
        bool header = false;
        yyjson_arr_foreach(guildDoc.root(), i, n, c) {
            if (!header) m_home.push_back({Row::Header, "", "SERVERS"});
            header = true;
            m_home.push_back({Row::Guild, str(c, "id"), str(c, "name")});
        }
        if (m_home.empty()) m_homeError = "No conversations or servers yet.";
        m_homeSel = firstItem(m_home);
        m_homeScroll = 0;
    }

    void openGuild(const Row& row) {
        m_screen = Screen::Guild;
        m_guildName = row.title;
        m_guild.clear();
        m_guildSel = -1;
        m_guildScroll = 0;
        m_guildLoaded = false;
        m_guildError.clear();
        m_channelsReq = g_net.send("GET", "/guilds/" + row.id + "/channels");
    }

    void buildGuild() {
        m_guildLoaded = true;
        if (m_channelsReq->status != 200) {
            m_guildError = problem(*m_channelsReq);
            return;
        }
        struct Ch { std::string id, name, parent; long pos, type; };
        std::vector<Ch> cats, texts;
        Json doc(m_channelsReq->body);
        size_t i, n;
        yyjson_val* c;
        yyjson_arr_foreach(doc.root(), i, n, c) {
            Ch ch{str(c, "id"), str(c, "name"), str(c, "parent_id"), num(c, "position"), num(c, "type")};
            if (ch.type == 4) cats.push_back(ch);
            else if (ch.type == 0 || ch.type == 5) texts.push_back(ch); // text and announcements
        }
        auto byPos = [](const Ch& a, const Ch& b) { return a.pos < b.pos; };
        std::sort(cats.begin(), cats.end(), byPos);
        std::sort(texts.begin(), texts.end(), byPos);
        for (const Ch& t : texts)
            if (t.parent.empty()) m_guild.push_back({Row::Channel, t.id, t.name});
        for (const Ch& cat : cats) {
            bool header = false;
            for (const Ch& t : texts) {
                if (t.parent != cat.id) continue;
                if (!header) {
                    std::string upper = cat.name;
                    for (char& ch : upper) ch = (char)std::toupper((unsigned char)ch);
                    m_guild.push_back({Row::Header, "", upper});
                    header = true;
                }
                m_guild.push_back({Row::Channel, t.id, t.name});
            }
        }
        if (m_guild.empty()) m_guildError = "No text channels you can open here.";
        m_guildSel = firstItem(m_guild);
    }

    void openChat(const Row& row) {
        m_chatFrom = m_screen;
        m_screen = Screen::Chat;
        m_chatId = row.id;
        m_chatTitle = (row.kind == Row::Channel ? "#" : "@") + row.title;
        m_msgs.clear();
        m_chatLoaded = false;
        m_chatError.clear();
        m_chatNote.clear();
        m_chatScroll = 0;
        m_msgReq.reset(); // a reply for the last chat is ignored
        pollMessages();
    }

    void pollMessages() {
        m_lastPoll = armGetSystemTick();
        m_msgReq = g_net.send("GET", "/channels/" + m_chatId + "/messages?limit=30");
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
            msg.author = nameOf(yyjson_obj_get(m, "author"));
            msg.text = cleanContent(str(m, "content"), m);
            size_t k, cnt;
            yyjson_val* a;
            yyjson_arr_foreach(yyjson_obj_get(m, "attachments"), k, cnt, a)
                msg.text += (msg.text.empty() ? "" : "\n") + std::string("[file] ") + str(a, "filename");
            yyjson_arr_foreach(yyjson_obj_get(m, "sticker_items"), k, cnt, a)
                msg.text += (msg.text.empty() ? "" : "\n") + std::string("[sticker] ") + str(a, "name");
            if (msg.text.empty() && yyjson_arr_size(yyjson_obj_get(m, "embeds")) > 0) msg.text = "[embed]";
            if (msg.text.empty()) continue;
            // Keep the lines already worked out for messages that didn't change.
            for (const Msg& old : m_msgs)
                if (old.id == msg.id && old.text == msg.text && old.wrapped) {
                    msg.lines = old.lines;
                    msg.wrapped = true;
                }
            fresh.push_back(std::move(msg));
        }
        std::reverse(fresh.begin(), fresh.end()); // oldest first
        m_msgs = std::move(fresh);
    }

    bool keyboardInput(u64 down, u64 dir) {
        if (dir & HidNpadButton_AnyUp) m_ky = std::max(0, m_ky - 1);
        if (dir & HidNpadButton_AnyDown) m_ky = std::min(4, m_ky + 1);
        if (m_ky < 4) {
            if (dir & HidNpadButton_AnyLeft) m_kx = (m_kx + 9) % 10;
            if (dir & HidNpadButton_AnyRight) m_kx = (m_kx + 1) % 10;
        } else { // Shift | Space | Delete | Send
            const int key = specialKey();
            if (dir & HidNpadButton_AnyLeft) m_kx = kSpecialStart[(key + 3) % 4];
            if (dir & HidNpadButton_AnyRight) m_kx = kSpecialStart[(key + 1) % 4];
        }
        if (down & HidNpadButton_A) press();
        if (down & HidNpadButton_X) m_draft += ' ';
        if (down & HidNpadButton_Y) m_shift = !m_shift;
        if (down & HidNpadButton_Plus) sendDraft();
        if (down & HidNpadButton_B) {
            if (m_draft.empty()) m_keyboard = false;
            else popChar(m_draft);
        }
        return true;
    }

    static constexpr int kSpecialStart[4] = {0, 2, 6, 8};
    int specialKey() const { return m_kx >= 8 ? 3 : m_kx >= 6 ? 2 : m_kx >= 2 ? 1 : 0; }

    std::string keyLabel(int row, int col) const {
        static const char* const kLower[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"};
        static const char* const kUpper[4] = {"!@#$%&*()-", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:/"};
        return std::string(1, (m_shift ? kUpper : kLower)[row][col]);
    }

    void press() {
        if (m_ky < 4) {
            if (m_draft.size() < 1900) m_draft += keyLabel(m_ky, m_kx);
            m_shift = false;
            return;
        }
        switch (specialKey()) {
            case 0: m_shift = !m_shift; break;
            case 1: m_draft += ' '; break;
            case 2: popChar(m_draft); break;
            case 3: sendDraft(); break;
        }
    }

    void sendDraft() {
        if (m_draft.empty() || m_sendReq) return;
        m_sendReq = g_net.send("POST", "/channels/" + m_chatId + "/messages",
                               "{\"content\":" + jsonString(m_draft) + ",\"tts\":false}");
        m_draft.clear();
        m_keyboard = false;
        m_chatNote.clear();
        m_chatScroll = 0;
    }

    // ------------------------------------------------------------ drawing

    void draw(tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h) {
        const std::string title = m_screen == Screen::Home ? "Home"
                                : m_screen == Screen::Guild ? m_guildName : m_chatTitle;
        r->drawString(fit(r, title, 22, w).c_str(), false, x, y + 22, 22, r->a(kText));
        r->drawRect(x, y + 34, w, 1, r->a(kLine));
        const s32 top = y + 44;
        if (m_screen == Screen::Chat) drawChat(r, x, top, w, y + h - top);
        else if (m_screen == Screen::Home) drawList(r, x, top, w, y + h - top, m_home, m_homeSel, m_homeScroll, m_homeLoaded, m_homeError);
        else drawList(r, x, top, w, y + h - top, m_guild, m_guildSel, m_guildScroll, m_guildLoaded, m_guildError);
    }

    void drawText(tsl::gfx::Renderer* r, const std::string& text, s32 x, s32 y, s32 w, tsl::Color c) {
        for (const std::string& line : wrap(r, text, 18, w)) {
            r->drawString(line.c_str(), false, x, y, 18, r->a(c));
            y += 24;
        }
    }

    void drawList(tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h, std::vector<Row>& rows, int sel,
                  s32& scroll, bool loaded, const std::string& error) {
        if (rows.empty()) {
            drawText(r, !loaded ? "Loading..." : error, x, y + 20, w, loaded ? kError : kMuted);
            return;
        }
        auto heightOf = [](const Row& row) { return row.kind == Row::Header ? 34 : 44; };
        s32 selTop = 0, total = 0;
        for (int i = 0; i < (int)rows.size(); ++i) {
            if (i == sel) selTop = total;
            total += heightOf(rows[(size_t)i]);
        }
        if (sel >= 0) {
            if (selTop < scroll) scroll = std::max(0, selTop - 34);
            if (selTop + 44 > scroll + h) scroll = selTop + 44 - h;
        }
        scroll = std::clamp(scroll, 0, std::max(0, total - h));
        s32 rowY = y - scroll;
        for (int i = 0; i < (int)rows.size(); ++i) {
            Row& row = rows[(size_t)i];
            const s32 rh = heightOf(row);
            if (rowY + rh >= y && rowY <= y + h) {
                if (row.kind == Row::Header) {
                    r->drawString(fit(r, row.title, 14, w).c_str(), false, x + 4, rowY + 26, 14, r->a(kMuted));
                } else {
                    if (i == sel) {
                        r->drawRect(x - 6, rowY, w + 12, rh, r->a(kSelected));
                        r->drawRect(x - 6, rowY, 4, rh, r->a(kBlurple));
                    }
                    const char* prefix = row.kind == Row::Channel ? "# " : row.kind == Row::Guild ? "" : "@ ";
                    if (row.shown.empty()) row.shown = fit(r, prefix + row.title, 19, w - 16);
                    r->drawString(row.shown.c_str(), false, x + 8, rowY + 29, 19, r->a(kText));
                }
            }
            rowY += rh;
        }
    }

    void drawChat(tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h) {
        const s32 kbH = m_keyboard ? 236 : 30;
        const s32 bottom = y + h - kbH;
        if (!m_chatLoaded) {
            drawText(r, "Loading...", x, y + 20, w, kMuted);
        } else if (!m_chatError.empty()) {
            drawText(r, m_chatError, x, y + 20, w, kError);
        } else if (m_msgs.empty()) {
            drawText(r, "No messages yet. Say hi!", x, y + 20, w, kMuted);
        } else {
            for (Msg& m : m_msgs)
                if (!m.wrapped) {
                    m.lines = wrap(r, m.text, 17, w - 4);
                    m.wrapped = true;
                }
            s32 total = 0;
            for (const Msg& m : m_msgs) total += 30 + (s32)m.lines.size() * 21;
            m_chatScroll = std::clamp(m_chatScroll, 0, std::max(0, total - (bottom - y)));
            r->enableScissoring(x - 6, y, w + 12, bottom - y);
            s32 cy = bottom + m_chatScroll;
            for (auto it = m_msgs.rbegin(); it != m_msgs.rend(); ++it) {
                const s32 mh = 30 + (s32)it->lines.size() * 21;
                cy -= mh;
                if (cy > bottom) continue;
                if (cy + mh < y) break;
                r->drawString(fit(r, it->author, 16, w).c_str(), false, x, cy + 18, 16, r->a(kAuthor));
                s32 ly = cy + 39;
                for (const std::string& line : it->lines) {
                    r->drawString(line.c_str(), false, x, ly, 17, r->a(kText));
                    ly += 21;
                }
            }
            r->disableScissoring();
        }
        if (m_keyboard) {
            drawKeyboard(r, x, bottom + 6, w);
        } else {
            const std::string hint = !m_chatNote.empty() ? m_chatNote
                                   : m_sendReq ? "Sending..."
                                   : " Write a message      Scroll";
            r->drawString(fit(r, hint, 15, w).c_str(), false, x, y + h - 8, 15,
                          r->a(m_chatNote.empty() ? kMuted : kError));
        }
    }

    void drawKeyboard(tsl::gfx::Renderer* r, s32 x, s32 y, s32 w) {
        // What's been typed: the end of it, two lines at most.
        r->drawRect(x - 4, y, w + 8, 52, r->a(kKey));
        std::vector<std::string> lines = wrap(r, m_draft + "_", 17, w - 12);
        if (lines.size() > 2) lines.erase(lines.begin(), lines.end() - 2);
        s32 ty = y + 20;
        for (const std::string& line : lines) {
            r->drawString(line.c_str(), false, x + 2, ty, 17, r->a(kText));
            ty += 22;
        }
        const s32 kw = w / 10, kh = 30, ky = y + 58;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 10; ++col) {
                const s32 kx = x + col * kw, kyy = ky + row * (kh + 3);
                const bool sel = m_ky == row && m_kx == col;
                r->drawRect(kx + 1, kyy, kw - 2, kh, r->a(sel ? kBlurple : kKey));
                const std::string label = keyLabel(row, col);
                r->drawString(label.c_str(), false, kx + (kw - widthOf(r, label, 18)) / 2, kyy + 22, 18, r->a(kText));
            }
        static const char* const kSpecial[4] = {"Shift", "Space", "Delete", "Send"};
        static const int kSpan[4] = {2, 4, 2, 2};
        const s32 sy = ky + 4 * (kh + 3);
        for (int k = 0; k < 4; ++k) {
            const s32 sx = x + kSpecialStart[k] * kw, sw = kSpan[k] * kw;
            const bool sel = m_ky == 4 && specialKey() == k;
            const bool lit = (k == 0 && m_shift) || (k == 3 && !m_draft.empty());
            r->drawRect(sx + 1, sy, sw - 2, kh, r->a(sel ? kBlurple : lit ? kSelected : kKey));
            r->drawString(kSpecial[k], false, sx + (sw - widthOf(r, kSpecial[k], 16)) / 2, sy + 21, 16, r->a(kText));
        }
        r->drawString(" Delete   Space   Shift  + Send", false, x, sy + kh + 22, 14, r->a(kMuted));
    }

    std::string m_account;
    Screen m_screen = Screen::Home;
    std::vector<Row> m_home, m_guild;
    int m_homeSel = -1, m_guildSel = -1;
    s32 m_homeScroll = 0, m_guildScroll = 0;
    bool m_homeLoaded = false, m_guildLoaded = false;
    std::string m_homeError, m_guildError, m_guildName;
    std::shared_ptr<Reply> m_dmReq, m_guildsReq, m_channelsReq, m_msgReq, m_sendReq;

    Screen m_chatFrom = Screen::Home;
    std::string m_chatId, m_chatTitle, m_msgFor, m_chatError, m_chatNote;
    std::vector<Msg> m_msgs;
    bool m_chatLoaded = false;
    s32 m_chatScroll = 0; // pixels up from the newest message
    u64 m_lastPoll = 0;

    bool m_keyboard = false;
    std::string m_draft;
    int m_kx = 0, m_ky = 1;
    bool m_shift = false;
    int m_heldFrames = 0;
};

class DiscordOverlay : public tsl::Overlay {
public:
    void initServices() override {
        tsl::cfg::extraLaunchCombo = kCombo;
        fsdevMountSdmc();
        // Small socket buffers: an overlay has a few MB in total.
        static const SocketInitConfig kSockets = {
            .tcp_tx_buf_size = 0x4000,
            .tcp_rx_buf_size = 0x8000,
            .tcp_tx_buf_max_size = 0x10000,
            .tcp_rx_buf_max_size = 0x20000,
            .udp_tx_buf_size = 0x2400,
            .udp_rx_buf_size = 0xA500,
            .sb_efficiency = 1,
            .num_bsd_sessions = 2,
            .bsd_service_type = BsdServiceType_Auto,
        };
        m_sockets = R_SUCCEEDED(socketInitialize(&kSockets));
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    void exitServices() override {
        g_net.stop();
        curl_global_cleanup();
        if (m_sockets) socketExit();
        fsdevUnmountAll();
    }

    void onShow() override { tsl::cfg::extraLaunchCombo = kCombo; }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        const Account account = loadAccount();
        if (!account.token.empty()) g_net.start(account.token);
        return initially<DiscordGui>(account.name, !account.token.empty());
    }

private:
    bool m_sockets = false;
};

} // namespace

int main(int argc, char** argv) {
    return tsl::loop<DiscordOverlay>(argc, argv);
}
