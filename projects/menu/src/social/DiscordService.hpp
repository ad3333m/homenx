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
// connects once the panel has been opened, and signs in with HomeNX's own saved
// account, else the first SwitchCord account, else a QR code.
class DiscordService {
public:
    enum class State { SignedOut, QrLogin, Connecting, Ready, Failed };

    struct Friend {
        std::string name;
        std::string status; // online / idle / dnd
    };
    struct Dm {
        Snowflake channelId = 0;
        std::string name;
        bool unread = false;
        int mentions = 0;
    };
    struct Line {
        std::string author;
        std::string text;
        bool mine = false;
        bool pending = false;
        bool failed = false;
    };

    DiscordService();
    ~DiscordService();

    // First call starts the network and signs in (saved token or SwitchCord's).
    void activate();
    void update();

    State state() const { return m_state; }
    std::string error() const { return m_error; }
    std::string myName() const;
    uint64_t version() const { return m_store ? m_store->version : 0; }

    // QR sign-in.
    void startQrLogin();
    std::string qrUrl() const;      // empty until the code is ready
    std::string qrStatus() const;   // what the phone did so far
    void signOut();

    std::vector<Friend> onlineFriends() const;
    std::vector<Dm> dms(size_t max = 30) const;

    // Messages of one DM (loads the newest page the first time).
    void openChannel(Snowflake channelId);
    std::vector<Line> lines(Snowflake channelId, size_t max = 40) const;
    bool loading(Snowflake channelId) const;
    std::string channelName(Snowflake channelId) const;
    void send(Snowflake channelId, const std::string& text);

private:
    void connect(const std::string& token);
    static std::string loadToken();
    static void saveToken(const std::string& token);

    std::unique_ptr<Store> m_store;
    std::unique_ptr<Gateway> m_gateway;
    std::unique_ptr<RemoteAuth> m_auth;
    State m_state = State::SignedOut;
    std::string m_error;
    bool m_started = false;
};
