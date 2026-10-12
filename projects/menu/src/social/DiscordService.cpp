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
}

void DiscordService::shutdown() {
    m_stopping = false;
    if (!m_started) return;
    // Transfers in flight fail fast instead of holding the menu open.
    Http::AbortAll();
    if (m_auth) m_auth->Stop();
    if (m_gateway) m_gateway->Stop();
    Async::Stop();
    m_auth.reset();
    m_gateway.reset();
    m_started = false;
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
    if (m_gateway) m_gateway->Stop();
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

    if (m_auth) {
        const RemoteAuth::Snapshot s = m_auth->Get();
        if (s.phase == RemoteAuth::Phase::Done && !s.token.empty()) {
            m_auth->Stop();
            m_auth.reset();
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
            m_gateway->Stop();
            m_gateway.reset();
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
    if (m_auth) m_auth->Stop();
    m_auth = std::make_unique<RemoteAuth>();
    m_auth->Start();
    m_state = State::QrLogin;
    m_error.clear();
}

void DiscordService::cancelQrLogin() {
    if (m_auth) m_auth->Stop();
    m_auth.reset();
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
    if (m_gateway) m_gateway->Stop();
    m_gateway.reset();
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

void DiscordService::openChannel(Snowflake channelId) {
    if (!m_store || !channelId) return;
    ChannelMessages& cm = m_store->Messages(channelId);
    if (cm.loaded || cm.loading) return;
    cm.loading = true;
    Store* store = m_store.get();
    Api::FetchMessages(channelId, 0, 40, [store, channelId](bool ok, std::vector<Message> msgs, const std::string&) {
        ChannelMessages& c = store->Messages(channelId);
        c.loading = false;
        if (!ok) { c.error = true; return; }
        store->AddHistory(channelId, std::move(msgs), false, 40);
        const auto& list = store->Messages(channelId).list;
        if (!list.empty()) {
            store->MarkRead(channelId, list.back().id);
            Api::Ack(channelId, list.back().id);
        }
    });
}

// Mentions, channels and custom emoji as Discord shows them: @name, #channel, :emoji:.
std::string DiscordService::plainText(const std::string& s) const {
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
        std::string text = m.IsSystem() ? m.systemText : plainText(m.content);
        if (!m.attachments.empty()) text += (text.empty() ? "" : " ") + std::string("[attachment]");
        if (text.empty() && !m.embeds.empty()) text = "[embed]";
        if (text.empty() && !m.stickers.empty()) text = "[sticker]";
        Line l;
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
