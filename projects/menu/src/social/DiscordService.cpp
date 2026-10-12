#include "social/DiscordService.hpp"

#include "core/DebugLog.hpp"
#include "discord/api.h"
#include "net/async.h"
#include "platform/platform.h"
#include "themeshop/ThemeHttp.hpp"
#include "util/json.h"
#include "util/log.h"
#include "util/strings.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

constexpr const char* kTokenPath = "sdmc:/config/SwitchU/discord.json";
constexpr const char* kSwitchCordAccounts = "sdmc:/switch/SwitchCord/accounts.json";

std::string readAll(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

}  // namespace

DiscordService::DiscordService() = default;

DiscordService::~DiscordService() {
    if (m_auth) m_auth->Stop();
    if (m_gateway) m_gateway->Stop();
    if (m_started) Async::Stop();
}

std::string DiscordService::loadToken() {
    // HomeNX's own sign-in first, then the account SwitchCord already has.
    for (const char* path : {kTokenPath, kSwitchCordAccounts}) {
        const std::string text = readAll(path);
        if (text.empty()) continue;
        J::Doc doc = J::Parse(text);
        yyjson_val* root = J::Root(doc);
        std::string token = J::S(root, "token");
        if (token.empty())
            for (yyjson_val* a : J::Arr(root, "accounts")) {
                token = J::S(a, "token");
                if (!token.empty()) break;
            }
        if (!token.empty()) return token;
    }
    return {};
}

void DiscordService::saveToken(const std::string& token) {
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/SwitchU", ec);
    std::ofstream(kTokenPath, std::ios::trunc) << "{\"token\":" << Str::Json(token) << "}\n";
}

void DiscordService::activate() {
    if (m_started) return;
    m_started = true;
    Log::Init("sdmc:/config/SwitchU/discord.log");
    // Sockets, nifm and curl: shared with the Theme Shop (initialised once).
    if (!themeshop::http::initialize()) {
        m_state = State::Failed;
        m_error = "No internet connection.";
        return;
    }
    Async::Start();
    m_store = std::make_unique<Store>();
    const std::string token = loadToken();
    if (token.empty())
        m_state = State::SignedOut;
    else
        connect(token);
}

void DiscordService::connect(const std::string& token) {
    if (m_gateway) m_gateway->Stop();
    m_store->Clear();
    Api::SetToken(token);
    m_gateway = std::make_unique<Gateway>();
    m_gateway->Start(token);
    m_state = State::Connecting;
    m_error.clear();
}

void DiscordService::update() {
    if (!m_started) return;
    Async::Pump();

    if (m_auth) {
        const RemoteAuth::Snapshot s = m_auth->Get();
        if (s.phase == RemoteAuth::Phase::Done && !s.token.empty()) {
            m_auth->Stop();
            m_auth.reset();
            saveToken(s.token);
            connect(s.token);
        } else if (s.phase == RemoteAuth::Phase::Failed) {
            m_error = s.error.empty() ? "The QR sign-in failed. Try again." : s.error;
        }
    }

    if (!m_gateway) return;
    for (auto& e : m_gateway->Drain())
        m_store->Apply(e.type, e.d);
    switch (m_gateway->GetState()) {
        case Gateway::State::Ready:
            if (m_store->ready) m_state = State::Ready;
            break;
        case Gateway::State::AuthFailed:
            // The saved token no longer works (signed out elsewhere): ask again.
            m_gateway->Stop();
            m_gateway.reset();
            {
                std::error_code ec;
                std::filesystem::remove(kTokenPath, ec);
            }
            m_state = State::SignedOut;
            m_error = "Discord signed this console out. Sign in again.";
            break;
        case Gateway::State::Failed:
            m_state = State::Failed;
            m_error = m_gateway->LastError();
            break;
        default:
            if (m_state != State::Ready) m_state = State::Connecting;
            break;
    }
}

std::string DiscordService::myName() const {
    return m_store ? m_store->me.DisplayName() : std::string();
}

void DiscordService::startQrLogin() {
    if (!m_started) activate();
    if (m_auth) m_auth->Stop();
    m_auth = std::make_unique<RemoteAuth>();
    m_auth->Start();
    m_state = State::QrLogin;
    m_error.clear();
}

std::string DiscordService::qrUrl() const {
    return m_auth ? m_auth->Get().qrUrl : std::string();
}

std::string DiscordService::qrStatus() const {
    if (!m_auth) return {};
    const RemoteAuth::Snapshot s = m_auth->Get();
    switch (s.phase) {
        case RemoteAuth::Phase::Preparing: return "Getting a code...";
        case RemoteAuth::Phase::ShowQr:    return "Scan it in the Discord app: Settings > Scan QR Code.";
        case RemoteAuth::Phase::Confirm:   return "Confirm on your phone" + (s.username.empty() ? std::string() : " (" + s.username + ")") + ".";
        case RemoteAuth::Phase::LoggingIn: return "Signing in...";
        case RemoteAuth::Phase::Failed:    return s.error.empty() ? "The sign-in failed." : s.error;
        default:                           return {};
    }
}

void DiscordService::signOut() {
    if (m_auth) m_auth->Stop();
    m_auth.reset();
    m_error.clear();
    if (m_gateway) m_gateway->Stop();
    m_gateway.reset();
    if (m_store) m_store->Clear();
    Api::SetToken("");
    std::error_code ec;
    std::filesystem::remove(kTokenPath, ec);
    m_state = State::SignedOut;
}

std::vector<DiscordService::Friend> DiscordService::onlineFriends() const {
    std::vector<Friend> out;
    if (!m_store) return out;
    for (const auto& [id, status] : m_store->presence) {
        if (status.empty() || status == "offline" || status == "invisible") continue;
        const User* u = m_store->FindUser(id);
        out.push_back({u ? u->DisplayName() : Str::Id(id), status});
    }
    std::sort(out.begin(), out.end(), [](const Friend& a, const Friend& b) {
        return Str::Lower(a.name) < Str::Lower(b.name);
    });
    return out;
}

std::vector<DiscordService::Dm> DiscordService::dms(size_t max) const {
    std::vector<Dm> out;
    if (!m_store) return out;
    for (Snowflake id : m_store->dms) {
        const Channel* c = m_store->FindChannel(id);
        if (!c) continue;
        out.push_back({id, m_store->ChannelName(*c), m_store->ChannelUnread(id), m_store->ChannelMentions(id)});
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

std::vector<DiscordService::Line> DiscordService::lines(Snowflake channelId, size_t max) const {
    std::vector<Line> out;
    if (!m_store) return out;
    auto it = m_store->messages.find(channelId);
    if (it == m_store->messages.end()) return out;
    const auto& list = it->second.list;
    const size_t start = list.size() > max ? list.size() - max : 0;
    for (size_t i = start; i < list.size(); ++i) {
        const Message& m = list[i];
        std::string text = m.IsSystem() ? m.systemText : m.content;
        if (text.empty() && !m.attachments.empty()) text = "[attachment]";
        if (text.empty() && !m.embeds.empty()) text = "[embed]";
        if (text.empty() && !m.stickers.empty()) text = "[sticker]";
        out.push_back({m_store->AuthorName(m), Str::OneLine(text), m.author.id == m_store->me.id,
                       m.pending, m.failed});
    }
    return out;
}

bool DiscordService::loading(Snowflake channelId) const {
    if (!m_store) return false;
    auto it = m_store->messages.find(channelId);
    return it != m_store->messages.end() && it->second.loading;
}

std::string DiscordService::channelName(Snowflake channelId) const {
    return m_store ? m_store->ChannelName(channelId) : std::string();
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
