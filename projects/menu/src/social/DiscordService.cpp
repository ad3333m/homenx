#include "social/DiscordService.hpp"

#include "core/DebugLog.hpp"
#include "discord/api.h"
#include "net/async.h"
#include "net/http.h"
#include "platform/platform.h"
#include "themeshop/ThemeHttp.hpp"
#include "util/json.h"
#include "util/log.h"
#include "util/strings.h"

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

// {"current": id, "accounts": [{id, username, global_name, avatar, token}],
//  "signed_out": [ids]} — the same account shape as SwitchCord's accounts.json.
constexpr const char* kAccountsPath = "sdmc:/config/SwitchU/discord.json";
constexpr const char* kSwitchCordAccounts = "sdmc:/switch/SwitchCord/accounts.json";

std::string readAll(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// "Today at 18:04", "Yesterday at 09:30" or "12/10/2026", in the console's time zone.
std::string when(int64_t ms) {
    TimeCalendarTime at{}, now{};
    TimeCalendarAdditionalInfo info{};
    if (R_FAILED(timeToCalendarTimeWithMyRule(ms / 1000, &at, &info))) return {};
    u64 nowSec = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &nowSec))
        || R_FAILED(timeToCalendarTimeWithMyRule(nowSec, &now, &info)))
        return {};
    char hm[8];
    std::snprintf(hm, sizeof(hm), "%02d:%02d", at.hour, at.minute);
    const bool sameMonth = at.year == now.year && at.month == now.month;
    if (sameMonth && at.day == now.day) return std::string("Today at ") + hm;
    if (sameMonth && at.day + 1 == now.day) return std::string("Yesterday at ") + hm;
    char d[16];
    std::snprintf(d, sizeof(d), "%02d/%02d/%04d", at.day, at.month, at.year);
    return d;
}

}  // namespace

// Stops a connection without waiting: update() drops it once its thread is done.
template <class T>
static void retire(std::unique_ptr<T>& p, std::vector<std::unique_ptr<T>>& bin) {
    if (!p) return;
    p->RequestStop();
    bin.push_back(std::move(p));
}

std::string DiscordService::Account::avatarUrl() const {
    User u;
    u.id = Str::ParseU64(id);
    u.avatar = avatar;
    return u.AvatarUrl(64);
}

DiscordService::DiscordService() = default;

DiscordService::~DiscordService() { shutdown(); }

void DiscordService::beginShutdown() {
    if (!m_started || m_stopping) return;
    m_stopping = true;
    Http::AbortAll();
    if (m_auth) m_auth->RequestStop();
    if (m_gateway) m_gateway->RequestStop();
    Async::RequestStop();
    Log::Close();
}

// Discord must never hold the menu up (closing for a game waited on it, and the
// old menu then never let go of HOME). Threads get a short time to stop; any
// still busy is left to finish on its own, with what it uses kept alive, and
// lingering() tells the menu to leave the sockets up for it.
void DiscordService::shutdown() {
    m_stopping = false;
    if (!m_started) return;
    Http::AbortAll(); // transfers in flight fail fast
    Log::Close();
    constexpr int kBudgetMs = 250;
    bool clean = true;
    if (m_auth && !m_auth->StopFor(kBudgetMs)) {
        (void)m_auth.release();
        clean = false;
    }
    if (m_gateway && !m_gateway->StopFor(kBudgetMs)) {
        (void)m_gateway.release();
        clean = false;
    }
    for (auto& g : m_oldGateways)
        if (!g->StopFor(kBudgetMs)) {
            (void)g.release();
            clean = false;
        }
    for (auto& a : m_oldAuths)
        if (!a->StopFor(kBudgetMs)) {
            (void)a.release();
            clean = false;
        }
    m_oldGateways.clear();
    m_oldAuths.clear();
    if (!Async::StopFor(kBudgetMs)) clean = false;
    if (!clean) (void)m_store.release(); // callbacks still running may touch it
    m_auth.reset();
    m_gateway.reset();
    m_lingering = m_lingering || !clean;
    m_started = false;
    DebugLog::log("[discord] stopped clean=%d", clean ? 1 : 0);
}

void DiscordService::loadAccounts() {
    m_accounts.clear();
    m_current.clear();
    m_signedOut.clear();
    auto add = [this](Account a) {
        if (a.token.empty()) return;
        if (!a.id.empty() && std::find(m_signedOut.begin(), m_signedOut.end(), a.id) != m_signedOut.end()) return;
        for (const Account& x : m_accounts)
            if (x.token == a.token || (!a.id.empty() && x.id == a.id)) return;
        m_accounts.push_back(std::move(a));
    };
    for (const char* path : {kAccountsPath, kSwitchCordAccounts}) {
        const std::string text = readAll(path);
        if (text.empty()) continue;
        J::Doc doc = J::Parse(text);
        yyjson_val* root = J::Root(doc);
        if (path == kAccountsPath) {
            m_current = J::S(root, "current");
            for (yyjson_val* id : J::Arr(root, "signed_out"))
                if (yyjson_is_str(id)) m_signedOut.push_back(yyjson_get_str(id));
            add({"", "", "", "", J::S(root, "token")}); // HomeNX's first format
        }
        for (yyjson_val* a : J::Arr(root, "accounts"))
            add({J::S(a, "id"), J::S(a, "username"), J::S(a, "global_name"), J::S(a, "avatar"), J::S(a, "token")});
    }
    ++m_accountsVersion;
}

void DiscordService::saveAccounts() const {
    std::string out = "{\"current\":" + Str::Json(m_current) + ",\"accounts\":[";
    for (size_t i = 0; i < m_accounts.size(); ++i) {
        const Account& a = m_accounts[i];
        out += (i ? ",\n  " : "\n  ");
        out += "{\"id\":" + Str::Json(a.id) + ",\"username\":" + Str::Json(a.username) +
               ",\"global_name\":" + Str::Json(a.globalName) + ",\"avatar\":" + Str::Json(a.avatar) +
               ",\"token\":" + Str::Json(a.token) + "}";
    }
    // Accounts signed out here stay out even though SwitchCord still lists them.
    out += "\n],\"signed_out\":[";
    bool first = true;
    for (const std::string& id : m_signedOut) {
        out += (first ? "" : ",") + Str::Json(id);
        first = false;
    }
    out += "]}\n";
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/SwitchU", ec);
    std::ofstream(kAccountsPath, std::ios::trunc) << out;
}

void DiscordService::activate() {
    if (m_stopping) shutdown(); // stopped for a launch that didn't happen: start again
    if (m_started) return;
    m_started = true;
    Log::Init("sdmc:/config/SwitchU/discord.log");
    // Sockets, nifm and curl: shared with the Theme Shop (initialised once).
    if (!themeshop::http::initialize()) {
        m_state = State::Failed;
        m_error = "No internet connection.";
        return;
    }
    Http::ResetAbort();
    Async::Start();
    m_store = std::make_unique<Store>();
    loadAccounts();
    if (m_accounts.empty()) {
        m_state = State::SignedOut;
        return;
    }
    const Account* pick = &m_accounts.front();
    for (const Account& a : m_accounts)
        if (a.id == m_current) pick = &a;
    connect(*pick);
}

void DiscordService::connect(const Account& account) {
    retire(m_gateway, m_oldGateways);
    m_store->Clear();
    m_token = account.token;
    m_current = account.id;
    m_rememberedMe = false;
    Api::SetToken(m_token);
    m_gateway = std::make_unique<Gateway>();
    m_gateway->Start(m_token);
    m_state = State::Connecting;
    m_error.clear();
    ++m_accountsVersion;
}

// Once Discord says who the token belongs to, keep the name and picture for the
// account switcher (and learn the id of a token saved before HomeNX kept ids).
void DiscordService::rememberMe() {
    m_rememberedMe = true;
    const User& me = m_store->me;
    const std::string id = Str::Id(me.id);
    m_accounts.erase(std::remove_if(m_accounts.begin(), m_accounts.end(),
                                    [&](const Account& a) { return a.id == id && a.token != m_token; }),
                     m_accounts.end());
    Account* mine = nullptr;
    for (Account& a : m_accounts)
        if (a.token == m_token) mine = &a;
    if (!mine) {
        m_accounts.push_back({});
        mine = &m_accounts.back();
    }
    *mine = {id, me.username, me.globalName, me.avatar, m_token};
    m_current = id;
    m_signedOut.erase(std::remove(m_signedOut.begin(), m_signedOut.end(), id), m_signedOut.end());
    saveAccounts();
    ++m_accountsVersion;
}

void DiscordService::update() {
    if (!m_started || m_stopping) return;
    Async::Pump();
    // Old connections (account switch, sign-in) close on their own threads.
    std::erase_if(m_oldGateways, [](const auto& g) { return g->Done(); });
    std::erase_if(m_oldAuths, [](const auto& a) { return a->Done(); });

    if (m_auth) {
        const RemoteAuth::Snapshot s = m_auth->Get();
        if (s.phase == RemoteAuth::Phase::Done && !s.token.empty()) {
            retire(m_auth, m_oldAuths);
            Account added{s.userId, s.username, "", s.avatar, s.token};
            m_accounts.erase(std::remove_if(m_accounts.begin(), m_accounts.end(),
                                            [&](const Account& a) { return !a.id.empty() && a.id == added.id; }),
                             m_accounts.end());
            m_accounts.push_back(added);
            m_signedOut.erase(std::remove(m_signedOut.begin(), m_signedOut.end(), added.id), m_signedOut.end());
            connect(added);
            saveAccounts();
        } else if (s.phase == RemoteAuth::Phase::Failed) {
            m_error = s.error.empty() ? "The QR sign-in failed. Try again." : s.error;
        }
    }

    if (!m_gateway) return;
    for (auto& e : m_gateway->Drain())
        m_store->Apply(e.type, e.d);
    State next = m_state;
    switch (m_gateway->GetState()) {
        case Gateway::State::Ready:
            if (m_store->ready) {
                next = State::Ready;
                if (!m_rememberedMe) rememberMe();
            }
            break;
        case Gateway::State::AuthFailed: {
            // The token no longer works (signed out elsewhere): forget it.
            retire(m_gateway, m_oldGateways);
            std::string who;
            for (const Account& a : m_accounts)
                if (a.token == m_token) who = a.name();
            m_accounts.erase(std::remove_if(m_accounts.begin(), m_accounts.end(),
                                            [&](const Account& a) { return a.token == m_token; }),
                             m_accounts.end());
            m_current.clear();
            saveAccounts();
            ++m_accountsVersion;
            next = State::SignedOut;
            m_error = (who.empty() ? std::string("This account") : who)
                      + " was signed out by Discord. Sign in again.";
            break;
        }
        case Gateway::State::Failed:
            next = State::Failed;
            m_error = m_gateway->LastError();
            break;
        default:
            if (m_state != State::Ready) next = State::Connecting;
            break;
    }
    if (!m_auth) m_state = next; // the QR code stays up while adding an account
}

std::string DiscordService::myName() const {
    return m_store ? m_store->me.DisplayName() : std::string();
}

std::string DiscordService::myUsername() const {
    return m_store ? m_store->me.username : std::string();
}

std::string DiscordService::myAvatarUrl() const {
    return m_store && m_store->me.id ? m_store->me.AvatarUrl(64) : std::string();
}

std::string DiscordService::myStatus() const {
    if (!m_store) return "offline";
    auto it = m_store->presence.find(m_store->me.id);
    return it != m_store->presence.end() && !it->second.empty() ? it->second : "online";
}

void DiscordService::switchAccount(const std::string& id) {
    for (const Account& a : m_accounts)
        if (a.id == id) {
            if (a.token == m_token && m_gateway) return;
            const Account pick = a;
            connect(pick);
            saveAccounts();
            return;
        }
}

void DiscordService::retry() {
    if (!m_store) { // the network never came up
        m_started = false;
        activate();
        return;
    }
    for (const Account& a : m_accounts)
        if (a.token == m_token) {
            const Account pick = a;
            connect(pick);
            return;
        }
    if (!m_accounts.empty()) {
        const Account pick = m_accounts.front();
        connect(pick);
        return;
    }
    startQrLogin();
}

void DiscordService::startQrLogin() {
    if (!m_started) activate();
    retire(m_auth, m_oldAuths);
    m_auth = std::make_unique<RemoteAuth>();
    m_auth->Start();
    m_state = State::QrLogin;
    m_error.clear();
}

void DiscordService::cancelQrLogin() {
    retire(m_auth, m_oldAuths);
    m_error.clear();
    // Back to whatever account was connected (update() works out its state).
    m_state = m_gateway ? (m_store && m_store->ready ? State::Ready : State::Connecting) : State::SignedOut;
}

std::string DiscordService::qrUrl() const {
    return m_auth ? m_auth->Get().qrUrl : std::string();
}

std::string DiscordService::qrStatus() const {
    if (!m_auth) return {};
    const RemoteAuth::Snapshot s = m_auth->Get();
    switch (s.phase) {
        case RemoteAuth::Phase::Preparing: return "Getting a code...";
        case RemoteAuth::Phase::ShowQr:    return "Scan this with the Discord mobile app: Settings > Scan QR Code.";
        case RemoteAuth::Phase::Confirm:   return "Confirm on your phone" + (s.username.empty() ? std::string() : " (" + s.username + ")") + ".";
        case RemoteAuth::Phase::LoggingIn: return "Signing in...";
        case RemoteAuth::Phase::Failed:    return s.error.empty() ? "The sign-in failed. Press A to try again." : s.error;
        default:                           return {};
    }
}

void DiscordService::signOut() {
    cancelQrLogin();
    retire(m_gateway, m_oldGateways);
    if (m_store) m_store->Clear();
    Api::SetToken("");
    const std::string token = m_token;
    m_token.clear();
    for (const Account& a : m_accounts)
        if (a.token == token && !a.id.empty()) m_signedOut.push_back(a.id);
    m_accounts.erase(std::remove_if(m_accounts.begin(), m_accounts.end(),
                                    [&](const Account& a) { return a.token == token; }),
                     m_accounts.end());
    m_current.clear();
    m_state = State::SignedOut;
    ++m_accountsVersion;
    if (!m_accounts.empty()) {
        const Account next = m_accounts.front();
        connect(next);
    }
    saveAccounts();
}

std::vector<DiscordService::Dm> DiscordService::dms(size_t max) const {
    std::vector<Dm> out;
    if (!m_store) return out;
    for (Snowflake id : m_store->dms) {
        const Channel* c = m_store->FindChannel(id);
        if (!c) continue;
        Dm d;
        d.channelId = id;
        d.name = m_store->ChannelName(*c);
        d.avatarUrl = m_store->DmAvatarUrl(*c);
        if (c->type == CH_DM && !c->recipients.empty()) {
            auto it = m_store->presence.find(c->recipients[0]);
            d.status = it != m_store->presence.end() && !it->second.empty() && it->second != "invisible"
                           ? it->second : "offline";
        } else {
            d.members = (int)c->recipients.size() + 1;
        }
        d.unread = m_store->ChannelUnread(id);
        d.mentions = m_store->ChannelMentions(id);
        out.push_back(std::move(d));
        if (out.size() >= max) break;
    }
    return out;
}

int DiscordService::dmMentions() const {
    return m_store ? m_store->DmMentions() : 0;
}

std::vector<DiscordService::Server> DiscordService::servers() const {
    std::vector<Server> out;
    if (!m_store) return out;
    std::vector<Snowflake> seen;
    auto add = [&](Snowflake id) {
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) return;
        const Guild* g = m_store->FindGuild(id);
        if (!g || g->unavailable) return;
        seen.push_back(id);
        Server s;
        s.id = id;
        s.name = g->name;
        s.iconUrl = g->IconUrl(64);
        s.initials = g->Initials();
        s.unread = m_store->GuildUnread(id) && !m_store->GuildMuted(id);
        s.mentions = m_store->GuildMentions(id);
        out.push_back(std::move(s));
    };
    for (const GuildFolder& f : m_store->folders)
        for (Snowflake id : f.guildIds) add(id);
    std::vector<const Guild*> rest; // not in a folder yet: by name
    for (const auto& [id, g] : m_store->guilds)
        if (std::find(seen.begin(), seen.end(), id) == seen.end()) rest.push_back(&g);
    std::sort(rest.begin(), rest.end(), [](const Guild* a, const Guild* b) { return Str::Lower(a->name) < Str::Lower(b->name); });
    for (const Guild* g : rest) add(g->id);
    return out;
}

std::vector<DiscordService::ChannelRow> DiscordService::channels(Snowflake guildId) const {
    std::vector<ChannelRow> out;
    if (!m_store) return out;
    for (const Store::Row& r : m_store->ChannelList(guildId)) {
        const Channel* c = m_store->FindChannel(r.id);
        if (!c) continue;
        ChannelRow row;
        row.id = r.id;
        row.name = c->name;
        row.category = r.category;
        if (!r.category) {
            if (!c->HasMessages()) continue; // forums and directories
            row.voice = c->IsVoice();
            row.unread = m_store->ChannelUnread(r.id) && !m_store->ChannelMuted(*c);
            row.mentions = m_store->ChannelMentions(r.id);
        }
        out.push_back(std::move(row));
    }
    // A category with nothing left under it isn't shown.
    std::vector<ChannelRow> tidy;
    for (size_t i = 0; i < out.size(); ++i)
        if (!out[i].category || (i + 1 < out.size() && !out[i + 1].category)) tidy.push_back(std::move(out[i]));
    return tidy;
}

int DiscordService::friendsOnline() const {
    if (!m_store) return 0;
    int n = 0;
    for (const auto& [id, status] : m_store->presence)
        if (id != m_store->me.id && (status == "online" || status == "idle" || status == "dnd")) ++n;
    return n;
}

int DiscordService::unreadDms() const {
    if (!m_store) return 0;
    int n = 0;
    for (Snowflake id : m_store->dms)
        if (m_store->ChannelUnread(id) || m_store->ChannelMentions(id) > 0) ++n;
    return n;
}

void DiscordService::openChannel(Snowflake channelId) {
    if (!m_store || !channelId) return;
    const Channel* ch = m_store->FindChannel(channelId);
    const Snowflake guildId = ch ? ch->guildId : 0;
    // A server channel only sends live messages once it's subscribed to.
    if (guildId && m_gateway) m_gateway->Subscribe(guildId, channelId);
    ChannelMessages& cm = m_store->Messages(channelId);
    if (cm.loaded || cm.loading) return;
    cm.loading = true;
    Store* store = m_store.get();
    Api::FetchMessages(channelId, 0, 40, [this, store, channelId, guildId](bool ok, std::vector<Message> msgs, const std::string&) {
        if (store != m_store.get()) return; // signed into another account meanwhile
        ChannelMessages& c = store->Messages(channelId);
        c.loading = false;
        if (!ok) { c.error = true; return; }
        // Server nicknames and role colours for the authors on this page.
        if (guildId && m_gateway) {
            const std::vector<Snowflake> unknown = store->UnknownMembers(guildId, msgs);
            if (!unknown.empty()) m_gateway->RequestMembers(guildId, unknown);
        }
        store->AddHistory(channelId, std::move(msgs), false, 40);
        const auto& list = store->Messages(channelId).list;
        if (!list.empty()) {
            store->MarkRead(channelId, list.back().id);
            Api::Ack(channelId, list.back().id);
        }
    });
}

// Mentions, channels and custom emoji as Discord shows them: @name, #channel, :emoji:.
namespace {

constexpr std::uint32_t kEmojiMark = 0xF0000; // + index into Line::emojis

void mark(std::string& out, std::vector<std::string>* emojis, std::string url) {
    if (!emojis || emojis->size() >= 0xFFFD) return;
    Str::AppendCp(out, kEmojiMark + static_cast<std::uint32_t>(emojis->size()));
    emojis->push_back(std::move(url));
}

bool emojiStart(std::uint32_t c) {
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x2B05 && c <= 0x2B55)
        || c == 0x203C || c == 0x2049 || (c >= 0x2194 && c <= 0x21AA) || c == 0x231A || c == 0x231B
        || c == 0x2328 || c == 0x23CF || (c >= 0x23E9 && c <= 0x23FA) || c == 0x24C2 || c == 0x25AA
        || c == 0x25AB || c == 0x25B6 || c == 0x25C0 || (c >= 0x25FB && c <= 0x25FE) || c == 0x2934
        || c == 0x2935 || c == 0x3030 || c == 0x303D || c == 0x3297 || c == 0x3299;
}

// Unicode emoji (with skin tones, ZWJ families, flags, keycaps) become pictures:
// Twemoji, the set Discord itself draws, named by their codepoints.
std::string markEmoji(const std::string& text, std::vector<std::string>* emojis) {
    std::vector<std::uint32_t> cps;
    for (const char* p = text.data(), *end = p + text.size(); p < end;) cps.push_back(Str::NextCp(p, end));
    std::string out;
    for (size_t i = 0; i < cps.size();) {
        const std::uint32_t c = cps[i];
        const bool keycap = ((c >= '0' && c <= '9') || c == '#' || c == '*') && i + 1 < cps.size()
            && (cps[i + 1] == 0x20E3 || (cps[i + 1] == 0xFE0F && i + 2 < cps.size() && cps[i + 2] == 0x20E3));
        const bool marked = (c == 0xA9 || c == 0xAE || c == 0x2122) && i + 1 < cps.size() && cps[i + 1] == 0xFE0F;
        if (!emojiStart(c) && !keycap && !marked) {
            Str::AppendCp(out, c);
            ++i;
            continue;
        }
        std::vector<std::uint32_t> seq{c};
        size_t j = i + 1;
        if (c >= 0x1F1E6 && c <= 0x1F1FF && j < cps.size() && cps[j] >= 0x1F1E6 && cps[j] <= 0x1F1FF)
            seq.push_back(cps[j++]); // a flag: two regional letters
        for (;;) {
            if (j < cps.size() && (cps[j] == 0xFE0F || cps[j] == 0x20E3 || (cps[j] >= 0x1F3FB && cps[j] <= 0x1F3FF)
                                   || (cps[j] >= 0xE0020 && cps[j] <= 0xE007F))) {
                seq.push_back(cps[j++]);
            } else if (j + 1 < cps.size() && cps[j] == 0x200D) {
                seq.push_back(cps[j++]);
                seq.push_back(cps[j++]);
            } else {
                break;
            }
        }
        const bool zwj = std::find(seq.begin(), seq.end(), 0x200D) != seq.end();
        std::string name;
        for (std::uint32_t cp : seq) {
            if (cp == 0xFE0F && !zwj) continue; // Twemoji's file naming
            char hex[12];
            std::snprintf(hex, sizeof(hex), "%s%x", name.empty() ? "" : "-", cp);
            name += hex;
        }
        mark(out, emojis, "https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/" + name + ".png");
        i = j;
    }
    return out;
}

// Markdown Discord would style: the markers go, the words stay.
std::string stripMarkdown(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (i == 0 || s[i - 1] == '\n') { // headings, small text and quotes at line starts
            for (const char* prefix : {"### ", "## ", "# ", "-# ", "> "})
                if (s.compare(i, std::strlen(prefix), prefix) == 0) {
                    i += std::strlen(prefix);
                    break;
                }
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
        out += s[i++];
    }
    return out;
}

// Tenor and Giphy "GIFs" are MP4s; both also serve a real GIF.
std::string gifOf(const Embed& e) {
    const std::string& v = !e.videoUrl.empty() ? e.videoUrl : e.thumbOrig;
    if (v.find("giphy.com") != std::string::npos) {
        const size_t dot = v.rfind(".mp4");
        if (dot != std::string::npos) return v.substr(0, dot) + ".gif";
    }
    const size_t host = v.find("media.tenor.com/");
    if (host != std::string::npos) { // .../<id><5-letter format>/<name>.<ext>; AAAAC is the GIF
        const size_t slash = v.find('/', host + 16);
        const size_t dot = v.rfind('.');
        if (slash != std::string::npos && slash >= host + 21 && dot != std::string::npos && dot > slash)
            return v.substr(0, slash - 5) + "AAAAC" + v.substr(slash, dot - slash) + ".gif";
    }
    return {};
}

// Discord's media proxy resizes (and turns WebP into PNG) for us.
std::string sized(const std::string& url, int w, int h) {
    if (url.empty()) return url;
    int tw = w, th = h;
    if (tw > 0 && th > 0) {
        const float s = std::min(1.f, std::min(400.f / tw, 300.f / th));
        tw = std::max(1, static_cast<int>(tw * s));
        th = std::max(1, static_cast<int>(th * s));
    }
    std::string out = url + (url.find('?') == std::string::npos ? "?" : "&") + "format=png";
    if (tw > 0 && th > 0) out += "&width=" + std::to_string(tw) + "&height=" + std::to_string(th);
    return out;
}

} // namespace

std::string DiscordService::plainText(const std::string& s, std::vector<std::string>* emojis) const {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const size_t close = s[i] == '<' ? s.find('>', i) : std::string::npos;
        if (close == std::string::npos || close - i > 80) {
            out += s[i++];
            continue;
        }
        const std::string tag = s.substr(i + 1, close - i - 1);
        std::string rep;
        if (tag.rfind("@&", 0) == 0) {
            rep = "@role";
        } else if (tag.rfind("@", 0) == 0) {
            const User* u = m_store->FindUser(Str::ParseU64(tag.substr(tag[1] == '!' ? 2 : 1)));
            rep = "@" + (u ? u->DisplayName() : std::string("unknown-user"));
        } else if (tag.rfind("#", 0) == 0) {
            rep = "#" + m_store->ChannelName(Str::ParseU64(tag.substr(1)));
        } else if (tag.rfind(":", 0) == 0 || tag.rfind("a:", 0) == 0) {
            const size_t a = tag.find(':'), b = tag.find(':', a + 1);
            if (b != std::string::npos && emojis) { // a server's own emoji: its picture
                const bool animated = tag[0] == 'a';
                const std::string id = tag.substr(b + 1);
                std::string pic;
                mark(pic, emojis, "https://cdn.discordapp.com/emojis/" + id + (animated ? ".gif" : ".png") + "?size=48");
                out += pic;
                i = close + 1;
                continue;
            }
            if (b != std::string::npos) rep = tag.substr(a, b - a + 1);
        }
        if (rep.empty()) {
            out += s[i++];
            continue;
        }
        out += rep;
        i = close + 1;
    }
    return out;
}

std::vector<DiscordService::Line> DiscordService::lines(Snowflake channelId, size_t max) const {
    std::vector<Line> out;
    if (!m_store) return out;
    auto it = m_store->messages.find(channelId);
    if (it == m_store->messages.end()) return out;
    const auto& list = it->second.list;
    const size_t start = list.size() > max ? list.size() - max : 0;
    for (size_t i = start; i < list.size(); ++i) {
        const Message& m = list[i];
        Line l;
        std::string text = m.IsSystem() ? m.systemText : stripMarkdown(plainText(m.content, &l.emojis));
        text = markEmoji(text, &l.emojis);
        for (const Attachment& a : m.attachments) {
            if (a.IsImage() && !a.Spoiler() && l.media.size() < 2) {
                Line::Media md;
                md.w = a.width;
                md.h = a.height;
                md.animated = Str::EndsWith(Str::Lower(a.filename), ".gif") || a.contentType == "image/gif";
                md.fallback = sized(a.proxyUrl.empty() ? a.url : a.proxyUrl, a.width, a.height);
                md.url = md.animated ? a.url : md.fallback;
                l.media.push_back(std::move(md));
            } else {
                text += (text.empty() ? "" : " ") + std::string("[file] ") + a.filename;
            }
        }
        for (const Embed& e : m.embeds) {
            if (l.media.size() >= 2) break;
            Line::Media md;
            if (e.type == "gifv") {
                md.w = e.thumbW;
                md.h = e.thumbH;
                md.fallback = sized(!e.thumbUrl.empty() ? e.thumbUrl : e.imageUrl, md.w, md.h);
                const std::string gif = gifOf(e);
                md.animated = !gif.empty();
                md.url = md.animated ? gif : md.fallback;
                // A message that's only the GIF's link shows just the GIF, like Discord.
                const std::string only = Str::Trim(m.content);
                if (only.rfind("http", 0) == 0 && only.find(' ') == std::string::npos) text.clear();
            } else if (!e.imageUrl.empty() || (e.type == "image" && !e.thumbUrl.empty())) {
                md.w = e.imageUrl.empty() ? e.thumbW : e.imageW;
                md.h = e.imageUrl.empty() ? e.thumbH : e.imageH;
                const std::string& orig = e.imageOrig.empty() ? e.thumbOrig : e.imageOrig;
                md.fallback = sized(e.imageUrl.empty() ? e.thumbUrl : e.imageUrl, md.w, md.h);
                md.animated = Str::EndsWith(Str::Lower(orig), ".gif");
                md.url = md.animated ? orig : md.fallback;
                if (e.type == "image" && Str::Trim(m.content) == e.url) text.clear();
            } else {
                if (text.empty() && !e.title.empty()) text = e.title;
                continue;
            }
            if (!md.url.empty()) l.media.push_back(std::move(md));
        }
        for (const StickerItem& st : m.stickers) {
            if (st.format == 3 || l.media.size() >= 2) { // Lottie stickers aren't pictures
                text += (text.empty() ? "" : " ") + std::string("[sticker] ") + st.name;
                continue;
            }
            Line::Media md;
            md.w = md.h = 160;
            md.animated = st.format == 4;
            md.fallback = "https://media.discordapp.net/stickers/" + Str::Id(st.id) + ".png?size=160";
            md.url = md.animated ? "https://media.discordapp.net/stickers/" + Str::Id(st.id) + ".gif?size=160"
                                 : md.fallback;
            l.media.push_back(std::move(md));
        }
        if (text.empty() && l.media.empty() && !m.embeds.empty()) text = "[embed]";
        l.color = m_store->AuthorColor(m);
        l.authorId = m.author.id;
        l.author = m_store->AuthorName(m);
        l.avatarUrl = m.author.AvatarUrl(64);
        l.text = Str::OneLine(text);
        l.ms = m.TimeMs();
        l.time = when(l.ms);
        l.mine = m.author.id == m_store->me.id;
        l.pending = m.pending;
        l.failed = m.failed;
        out.push_back(std::move(l));
    }
    return out;
}

bool DiscordService::loading(Snowflake channelId) const {
    if (!m_store) return false;
    auto it = m_store->messages.find(channelId);
    return it != m_store->messages.end() && it->second.loading;
}

void DiscordService::send(Snowflake channelId, const std::string& raw) {
    const std::string text = Str::Trim(raw);
    if (!m_store || !channelId || text.empty()) return;
    Message m;
    m.id = Str::SnowflakeFromMs(Platform::NowMs()) | 0x3FFFFF;
    m.channelId = channelId;
    m.author = m_store->me;
    m.content = text;
    m.nonce = Api::NewNonce();
    const std::string nonce = m.nonce;
    m_store->AddLocalEcho(std::move(m));
    Store* store = m_store.get();
    Api::SendMessage(channelId, text, nonce, 0, false, [store, nonce, channelId](bool ok, Message sent, const std::string&) {
        if (ok) store->ConfirmSent(nonce, std::move(sent));
        else store->MarkFailed(channelId, nonce);
    });
}
