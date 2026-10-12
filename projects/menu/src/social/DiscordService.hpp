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
    struct Line {
        Snowflake authorId = 0;
        std::string author, avatarUrl, text, time;
        int64_t ms = 0;
        bool mine = false;
        bool pending = false;
        bool failed = false;
    };

    DiscordService();
    ~DiscordService();

    // First call starts the network and signs in (saved account or SwitchCord's).
    void activate();
    void update();
    // Stops every network thread. Must run before the sockets are shut down.
    void shutdown();

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

    // Messages of one DM (loads the newest page the first time).
    void openChannel(Snowflake channelId);
    std::vector<Line> lines(Snowflake channelId, size_t max = 40) const;
    bool loading(Snowflake channelId) const;
    void send(Snowflake channelId, const std::string& text);

private:
    void connect(const Account& account);
    void loadAccounts();
    void saveAccounts() const;
    void rememberMe();
    std::string plainText(const std::string& content) const;

    std::unique_ptr<Store> m_store;
    std::unique_ptr<Gateway> m_gateway;
    std::unique_ptr<RemoteAuth> m_auth;
    std::vector<Account> m_accounts;
    std::vector<std::string> m_signedOut; // ids not to take back from SwitchCord
    std::string m_current;  // account id ("" until Discord says who a new token is)
    std::string m_token;
    uint64_t m_accountsVersion = 0;
    bool m_rememberedMe = false;
    State m_state = State::SignedOut;
    std::string m_error;
    bool m_started = false;
};
