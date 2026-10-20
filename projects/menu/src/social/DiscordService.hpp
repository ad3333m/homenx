#pragma once
#include "discord/gateway.h"
#include "discord/remote_auth.h"
#include "discord/store.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// HomeNX's Discord connection for the Discord panel, built on SwitchCord's client
// code (discord/, net/). Main thread only: call update() every frame. It only
// connects once the panel has been opened. Keeps every account signed in on this
// console (plus SwitchCord's) so the panel can switch between them.
class DiscordService {
public:
    enum class State { SignedOut, QrLogin, Connecting, Ready, Failed };

    struct Account {
        std::string id, username, globalName, avatar, token;
        std::string name() const { return globalName.empty() ? username : globalName; }
        std::string avatarUrl() const;
    };
    struct Dm {
        Snowflake channelId = 0;
        std::string name, avatarUrl;
        std::string status; // online / idle / dnd / offline; empty for groups
        int members = 0;    // group DMs
        bool unread = false;
        int mentions = 0;
    };
    struct Server {
        Snowflake id = 0;
        std::string name, iconUrl, initials;
        bool unread = false;
        int mentions = 0;
    };
    struct ChannelRow {
        Snowflake id = 0;
        std::string name;
        bool category = false, voice = false, unread = false;
        int mentions = 0;
    };
    struct Line {
        uint32_t color = 0; // the author's role colour in a server, 0 = default
        Snowflake authorId = 0;
        std::string author, avatarUrl, text, time;
        int64_t ms = 0;
        bool mine = false;
        bool pending = false;
        bool failed = false;
        // Emoji are U+F0000 + i in `text`; emojis[i] is the picture for it.
        std::vector<std::string> emojis;
        struct Media {
            std::string url;      // tried first (the animated GIF when there is one)
            std::string fallback; // a still picture if that fails
            int w = 0, h = 0;
            bool animated = false;
        };
        std::vector<Media> media; // images and GIFs under the text
    };

    DiscordService();
    ~DiscordService();

    // First call starts the network and signs in (saved account or SwitchCord's).
    void activate();
    void update();
    // Stops every network thread. Must run before the sockets are shut down.
    void shutdown();
    // Starts stopping without waiting (a game is launching), so shutdown()
    // at exit only joins threads that are already gone.
    void beginShutdown();
    // A thread was still busy at shutdown(): keep the sockets up for it.
    bool lingering() const { return m_lingering; }

    State state() const { return m_state; }
    std::string error() const { return m_error; }
    uint64_t version() const { return (m_store ? m_store->version : 0) + m_accountsVersion; }

    // The signed-in user (valid when Ready).
    std::string myName() const;
    std::string myUsername() const;
    std::string myAvatarUrl() const;
    std::string myStatus() const;

    // Accounts saved on this console; the current one first.
    const std::vector<Account>& accounts() const { return m_accounts; }
    const std::string& currentAccount() const { return m_current; }
    void switchAccount(const std::string& id);
    void signOut(); // the current account; the next saved one takes over
    void retry();   // after a failure: reconnect, or sign in if nothing is saved

    // QR sign-in (also how another account is added).
    void startQrLogin();
    void cancelQrLogin();
    std::string qrUrl() const;      // empty until the code is ready
    std::string qrStatus() const;   // what the phone did so far

    std::vector<Dm> dms(size_t max = 30) const;
    int dmMentions() const;
    // Servers in your sidebar order, and a server's text channels by category.
    std::vector<Server> servers() const;
    std::vector<ChannelRow> channels(Snowflake guildId) const;
    // For the Social screen's Discord tile.
    bool started() const { return m_started && !m_stopping; }
    int friendsOnline() const;
    int unreadDms() const;

    // Messages of one DM (loads the newest page the first time).
    void openChannel(Snowflake channelId);
    std::vector<Line> lines(Snowflake channelId, size_t max = 40) const;
    bool loading(Snowflake channelId) const;
    void send(Snowflake channelId, const std::string& text);

    // HOME's alerts: the newest DM or mention since the last take (none in Do
    // Not Disturb or from a muted channel).
    struct Alert {
        Snowflake channelId = 0, guildId = 0;
        std::string title, text, avatarUrl; // the banner: who, what, their picture
        std::string chat, chatAvatar;       // the chat, as the panel titles it
    };
    bool takeAlert(Alert& out);
    bool hasSavedAccount() const; // this console's or SwitchCord's
    bool stopping() const { return m_stopping; }

private:
    void connect(const Account& account);
    void loadAccounts();
    void saveAccounts() const;
    void rememberMe();
    std::string plainText(const std::string& content, std::vector<std::string>* emojis = nullptr) const;
    void noteAlert(const Store::Change& ch, yyjson_val* d);
    void saveListsForOverlay();

    std::unique_ptr<Store> m_store;
    std::unique_ptr<Gateway> m_gateway;
    std::unique_ptr<RemoteAuth> m_auth;
    std::vector<std::unique_ptr<Gateway>> m_oldGateways;  // closing, see retire()
    std::vector<std::unique_ptr<RemoteAuth>> m_oldAuths;
    std::vector<Account> m_accounts;
    std::vector<std::string> m_signedOut; // ids not to take back from SwitchCord
    std::string m_current;  // account id ("" until Discord says who a new token is)
    std::string m_token;
    uint64_t m_accountsVersion = 0;
    bool m_rememberedMe = false;
    State m_state = State::SignedOut;
    std::string m_error;
    bool m_started = false;
    bool m_stopping = false;
    bool m_lingering = false;
    Alert m_alert;
    bool m_hasAlert = false;
    std::string m_savedLists;    // what discord_lists.json holds
    uint64_t m_listsNextTick = 0;
};
