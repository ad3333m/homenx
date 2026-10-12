#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// "Scan the QR code with your phone" login (Discord's remote auth v2). The phone app
// approves the session and we receive the account token, so no password or captcha
// has to be typed on the console.
class RemoteAuth {
public:
    enum class Phase { Idle, Preparing, ShowQr, Confirm, LoggingIn, Done, Cancelled, Failed };

    struct Snapshot {
        Phase phase = Phase::Idle;
        std::string qrUrl;
        std::string userId, username, avatar;  // after the phone scans
        std::string token;                     // when Done
        std::string error;
        int64_t expiresMs = 0;
    };

    RemoteAuth();
    ~RemoteAuth();
    void Start();
    void Stop();
    Snapshot Get() const;

private:
    struct Key;
    void Run();
    bool Session(Key& key);
    void Set(Phase p);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    Snapshot snap_;
    bool hashedProof_ = false;
};
