#include "discord/remote_auth.h"

#include <nxui/core/ThreadPool.hpp>

#include "core/BackgroundThread.hpp"

#include <chrono>
#include <cstring>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#include "discord/api.h"
#include "discord/types.h"
#include "net/http.h"
#include "net/websocket.h"
#include "platform/platform.h"
#include "util/json.h"
#include "util/log.h"
#include "util/strings.h"

#include <switch.h>

#include <arpa/inet.h>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int Entropy(void*, unsigned char* out, size_t len) {
    Platform::Random(out, len);
    return 0;
}

int64_t Now() { return Platform::NowMs(); }

struct CaptchaAsk {
    std::string service, sitekey, rqdata, rqtoken, sessionId;
};

// captcha_key is either a message or a list of them. The site key is what the
// browser widget needs.
bool ReadCaptcha(yyjson_val* root, CaptchaAsk& out) {
    if (!root) return false;
    out.sitekey = J::S(root, "captcha_sitekey");
    out.service = J::S(root, "captcha_service");
    out.rqdata = J::S(root, "captcha_rqdata");
    out.rqtoken = J::S(root, "captcha_rqtoken");
    out.sessionId = J::S(root, "captcha_session_id");
    if (out.service.empty() && !out.sitekey.empty()) out.service = "hcaptcha";
    return !out.sitekey.empty();
}

std::string HtmlEscapeJson(std::string json) {
    std::string out;
    out.reserve(json.size());
    for (char c : json) {
        if (c == '<') out += "\\u003c";
        else out += c;
    }
    return out;
}

// Serves one page on this console's Wi-Fi address. The phone loads it, hCaptcha
// draws the check Discord just asked for, and the page posts the token back here.
// The Switch browser rejects 127.0.0.1, so the page is not opened on the console.
class CaptchaPage {
public:
    ~CaptchaPage() { Close(); }

    bool Start(const CaptchaAsk& ask, std::string& error) {
        error.clear();
        uint8_t rnd[16];
        Platform::Random(rnd, sizeof(rnd));
        static const char* kHex = "0123456789abcdef";
        nonce_.resize(sizeof(rnd) * 2);
        for (size_t i = 0; i < sizeof(rnd); ++i) {
            nonce_[i * 2] = kHex[rnd[i] >> 4];
            nonce_[i * 2 + 1] = kHex[rnd[i] & 0xf];
        }
        rqtoken_ = ask.rqtoken;
        sessionId_ = ask.sessionId;
        started_ = Now();

        u32 ip = 0;
        if (R_FAILED(nifmGetCurrentIpAddress(&ip)) || ip == 0) {
            error = "This console has no Wi-Fi address. Connect it, then press A to try again.";
            return false;
        }
        char ipText[16];
        std::snprintf(ipText, sizeof(ipText), "%u.%u.%u.%u",
                      ip & 0xFFu, (ip >> 8) & 0xFFu, (ip >> 16) & 0xFFu, (ip >> 24) & 0xFFu);
        host_ = ipText;

        // This console's address starts with a length. Leaving it at 0 makes bind fail,
        // and a port of 0 is not assigned. Try a few fixed ports.
        int fail = 0;
        for (int port = 19850; port < 19870; ++port) {
            if (listen_ >= 0) {
                ::close(listen_);
                listen_ = -1;
            }
            listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_ < 0) {
                fail = errno;
                break;
            }
            int yes = 1;
            setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            sockaddr_in addr{};
            addr.sin_len = sizeof(addr);
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(port));
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(listen_, 4) == 0) {
                port_ = port;
                fail = 0;
                break;
            }
            fail = errno;
        }
        if (listen_ < 0 || port_ == 0) {
            LOG("captcha page: listen failed (%d)", fail);
            Close();
            error = "Couldn't open the check on this network (" + std::to_string(fail) + ").";
            return false;
        }
        fcntl(listen_, F_SETFL, O_NONBLOCK);
        const std::string page = Url();
        const std::string cfg = HtmlEscapeJson(
            "{\"sitekey\":" + Str::Json(ask.sitekey) + ",\"rqdata\":" + Str::Json(ask.rqdata) +
            ",\"tokenUrl\":" + Str::Json(page + "/token") + "}");
        // Discord issues this site key for discord.com. hCaptcha's script needs
        // that host or it refuses to show the check.
        html_ = std::string("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                            "<title>Discord check</title><script type=\"application/json\" id=\"cfg\">") +
                cfg + "</script><style>body{margin:0;background:#313338;color:#f2f3f5;font-family:sans-serif;"
                      "display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;}"
                      "p{max-width:32rem;text-align:center;line-height:1.4;font-size:1.4rem;}</style></head><body>"
                      "<p id=\"status\">Complete this check. When it says done, close this page.</p><div id=\"box\"></div><script>"
                      "function cfg(){return JSON.parse(document.getElementById('cfg').textContent);}"
                      "function post(payload){var c=cfg();var ok=!!payload.token;"
                      "fetch(c.tokenUrl,{method:'POST',headers:{'Content-Type':'application/json'},"
                      "body:JSON.stringify(payload)}).then(function(){"
                      "document.getElementById('status').textContent=ok?"
                      "'Done. You can close this page.':"
                      "'This check did not open. Close this page and scan the code again.';});}"
                      "function start(){var c=cfg();var id=hcaptcha.render('box',{sitekey:c.sitekey,theme:'dark',"
                      "size:c.rqdata?'invisible':'normal',callback:function(token){post({token:token});},"
                      "'error-callback':function(err){post({error:String(err)});}});"
                      "if(c.rqdata)hcaptcha.execute(id,{rqdata:c.rqdata});}"
                      "</script><script src=\"https://js.hcaptcha.com/1/api.js?render=explicit&onload=start"
                      "&recaptchacompat=off&host=discord.com\" async defer></script></body></html>";
        return true;
    }

    std::string Url() const { return "http://" + host_ + ":" + std::to_string(port_) + "/" + nonce_; }
    const std::string& Rqtoken() const { return rqtoken_; }
    const std::string& SessionId() const { return sessionId_; }
    bool HasToken() const { return !token_.empty(); }
    const std::string& Token() const { return token_; }
    const std::string& WidgetError() const { return widgetError_; }
    bool Expired() const { return Now() - started_ > 180000; }

    void Pump() {
        if (listen_ < 0) return;
        if (client_ < 0) {
            int fd = accept(listen_, nullptr, nullptr);
            if (fd < 0) return;
            fcntl(fd, F_SETFL, O_NONBLOCK);
            client_ = fd;
            buf_.clear();
        }
        char tmp[2048];
        const ssize_t n = recv(client_, tmp, sizeof(tmp), 0);
        if (n > 0) buf_.append(tmp, tmp + n);
        else if (n == 0) {
            FinishClient();
            return;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            FinishClient();
            return;
        }
        if (buf_.size() > 32768) {
            Reply(400, "text/plain", "too large");
            return;
        }
        const size_t headerEnd = buf_.find("\r\n\r\n");
        if (headerEnd == std::string::npos) return;
        const std::string header = buf_.substr(0, headerEnd);
        const size_t lineEnd = header.find("\r\n");
        const std::string req = lineEnd == std::string::npos ? header : header.substr(0, lineEnd);
        const size_t sp1 = req.find(' ');
        const size_t sp2 = sp1 == std::string::npos ? std::string::npos : req.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            Reply(400, "text/plain", "bad request");
            return;
        }
        const std::string method = req.substr(0, sp1);
        const std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
        int contentLength = 0;
        std::string origin;
        size_t at = 0;
        while (at < header.size()) {
            size_t next = header.find("\r\n", at);
            if (next == std::string::npos) next = header.size();
            const std::string line = header.substr(at, next - at);
            const std::string lower = Str::Lower(line);
            if (Str::StartsWith(lower, "content-length:")) contentLength = std::atoi(line.c_str() + 15);
            else if (Str::StartsWith(lower, "origin:")) origin = Str::Trim(line.substr(7));
            if (next == header.size()) break;
            at = next + 2;
        }
        if (contentLength < 0 || contentLength > 16384) {
            Reply(400, "text/plain", "bad length");
            return;
        }
        if (buf_.size() < headerEnd + 4 + (size_t)contentLength) return;
        const std::string body = buf_.substr(headerEnd + 4, (size_t)contentLength);
        const std::string page = "/" + nonce_;
        if (method == "GET" && path == page) {
            Reply(200, "text/html; charset=utf-8", html_);
            return;
        }
        if (method == "POST" && path == page + "/token") {
            const std::string expect = "http://" + host_ + ":" + std::to_string(port_);
            if (!origin.empty() && origin != expect) {
                Reply(403, "text/plain", "bad origin");
                return;
            }
            Reply(200, "application/json", "{\"ok\":true}");
            auto doc = J::Parse(body);
            yyjson_val* root = J::Root(doc);
            const std::string token = J::S(root, "token");
            if (!token.empty()) token_ = token;
            else widgetError_ = J::S(root, "error");
            return;
        }
        Reply(404, "text/plain", "not found");
    }

private:
    void Reply(int code, const char* type, const std::string& body) {
        if (client_ < 0) return;
        const char* status = code == 200 ? "200 OK" : code == 403 ? "403 Forbidden" : code == 404 ? "404 Not Found" : "400 Bad Request";
        std::string msg = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type +
                          "\r\nContent-Length: " + std::to_string(body.size()) +
                          "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
        int spins = 0;
        for (size_t off = 0; off < msg.size();) {
            const ssize_t n = ::send(client_, msg.data() + off, msg.size() - off, 0);
            if (n > 0) {
                off += (size_t)n;
                spins = 0;
            } else if ((errno == EAGAIN || errno == EWOULDBLOCK) && spins < 200) {
                ++spins;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else break;
        }
        FinishClient();
    }
    void FinishClient() {
        if (client_ >= 0) ::close(client_);
        client_ = -1;
        buf_.clear();
    }
    void Close() {
        FinishClient();
        if (listen_ >= 0) ::close(listen_);
        listen_ = -1;
    }

    int listen_ = -1;
    int client_ = -1;
    int port_ = 0;
    int64_t started_ = 0;
    std::string host_, nonce_, buf_, html_, token_, widgetError_, rqtoken_, sessionId_;
};

Http::Response PostTicket(const std::string& ticket, const std::string& captchaKey, const std::string& rqtoken,
                          const std::string& sessionId) {
    std::string body = "{\"ticket\":" + Str::Json(ticket);
    if (!captchaKey.empty()) body += ",\"captcha_key\":" + Str::Json(captchaKey);
    if (!rqtoken.empty()) body += ",\"captcha_rqtoken\":" + Str::Json(rqtoken);
    body += "}";
    Http::Request req;
    req.method = "POST";
    req.url = std::string(Api::kBase) + "/users/@me/remote-auth/login";
    req.body = body;
    req.headers.push_back(std::string("User-Agent: ") + Client::kUserAgent);
    req.headers.push_back("X-Super-Properties: " + Client::SuperPropertiesB64());
    req.headers.push_back("X-Discord-Locale: en-US");
    req.headers.push_back("Origin: https://discord.com");
    req.headers.push_back("Referer: https://discord.com/channels/@me");
    req.headers.push_back("Content-Type: application/json");
    if (!captchaKey.empty()) req.headers.push_back("X-Captcha-Key: " + captchaKey);
    if (!rqtoken.empty()) req.headers.push_back("X-Captcha-Rqtoken: " + rqtoken);
    if (!sessionId.empty()) req.headers.push_back("X-Captcha-Session-Id: " + sessionId);
    return Http::Perform(req);
}

}  // namespace

struct RemoteAuth::Key {
    mbedtls_pk_context pk;
    mbedtls_ctr_drbg_context drbg;
    bool ok = false;
    std::string spkiB64;

    Key() {
        mbedtls_pk_init(&pk);
        mbedtls_ctr_drbg_init(&drbg);
    }
    ~Key() {
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&drbg);
    }

    bool Generate() {
        static const unsigned char kPers[] = "switchcord-remote-auth";
        if (mbedtls_ctr_drbg_seed(&drbg, Entropy, nullptr, kPers, sizeof(kPers) - 1) != 0) return false;
        if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) return false;
        mbedtls_rsa_context* rsa = mbedtls_pk_rsa(pk);
        if (mbedtls_rsa_gen_key(rsa, mbedtls_ctr_drbg_random, &drbg, 2048, 65537) != 0) return false;
        mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
        unsigned char der[1024];
        int len = mbedtls_pk_write_pubkey_der(&pk, der, sizeof(der));
        if (len <= 0) return false;
        spkiB64 = Str::Base64(der + sizeof(der) - len, (size_t)len);
        ok = true;
        return true;
    }

    bool Decrypt(const std::string& b64, std::vector<uint8_t>& out) {
        std::vector<uint8_t> in = Str::Base64Decode(b64);
        mbedtls_rsa_context* rsa = mbedtls_pk_rsa(pk);
        if (in.size() != mbedtls_rsa_get_len(rsa)) return false;
        out.assign(512, 0);
        size_t olen = 0;
#if MBEDTLS_VERSION_MAJOR >= 3
        int rc = mbedtls_rsa_rsaes_oaep_decrypt(rsa, mbedtls_ctr_drbg_random, &drbg, nullptr, 0, &olen, in.data(),
                                                out.data(), out.size());
#else
        int rc = mbedtls_rsa_rsaes_oaep_decrypt(rsa, mbedtls_ctr_drbg_random, &drbg, MBEDTLS_RSA_PRIVATE, nullptr, 0,
                                                &olen, in.data(), out.data(), out.size());
#endif
        if (rc != 0) {
            LOG("remote auth: decrypt failed (-0x%04x)", -rc);
            return false;
        }
        out.resize(olen);
        return true;
    }
};

RemoteAuth::RemoteAuth() = default;
RemoteAuth::~RemoteAuth() { Stop(); }

void RemoteAuth::Start() {
    Stop();
    stop_ = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_ = Snapshot{};
        snap_.phase = Phase::Preparing;
    }
    done_ = false;
    thread_ = std::thread([this] {
        nxui::moveToWorkerCores(); // HomeNX: keep core 0 for the menu's frames
        Run();
        done_ = true;
    });
}

bool RemoteAuth::StopFor(int timeoutMs) {
    stop_ = true;
    for (int t = 0; t < timeoutMs && !done_; t += 5) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!thread_.joinable()) return true;
    if (!done_) {
        abandonThread(thread_); // libnx can't detach
        return false;
    }
    thread_.join();
    return true;
}

void RemoteAuth::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

RemoteAuth::Snapshot RemoteAuth::Get() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_;
}

void RemoteAuth::Set(Phase p) {
    std::lock_guard<std::mutex> lk(mu_);
    snap_.phase = p;
}

void RemoteAuth::Run() {
    LOG("remote auth: key generation start");
    Platform::SetCpuBoost(true);
    Key key;
    bool generated = key.Generate();
    Platform::SetCpuBoost(false);
    LOG("remote auth: key generation done (ok=%d)", generated ? 1 : 0);
    if (!generated) {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.phase = Phase::Failed;
        snap_.error = "Couldn't create the login key.";
        return;
    }
    // A QR code only lives for a couple of minutes; keep offering fresh ones.
    int failures = 0;
    while (!stop_) {
        bool again = Session(key);
        Phase p = Get().phase;
        if (!again || p == Phase::Done || p == Phase::Failed) break;
        if (p == Phase::Cancelled) {
            // Show "cancelled" briefly, then a new code.
            for (int i = 0; i < 30 && !stop_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (++failures > 20) break;
        std::lock_guard<std::mutex> lk(mu_);
        snap_ = Snapshot{};
        snap_.phase = Phase::Preparing;
    }
}

bool RemoteAuth::Session(Key& key) {
    WebSocket ws;
    std::string err;
    std::vector<std::string> headers = {"Origin: https://discord.com",
                                        std::string("User-Agent: ") + Client::kUserAgent};
    LOG("remote auth: connecting");
    if (!ws.Connect("wss://remote-auth-gateway.discord.gg/?v=2", headers, err)) {
        LOG("remote auth: %s", err.c_str());
        std::lock_guard<std::mutex> lk(mu_);
        snap_.error = "Can't reach Discord (" + err + ")";
        // Retry after a pause; the network may still be coming up.
        for (int i = 0; i < 30 && !stop_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return true;
    }

    int64_t hbInterval = 41250, nextHb = 0;
    bool proofSent = false;
    std::unique_ptr<CaptchaPage> captcha;
    std::string captchaTicket;
    std::vector<WebSocket::Message> msgs;

    // true: keep this socket open (the check is on screen). false: this session is over.
    auto beginCaptcha = [&](const CaptchaAsk& ask, const std::string& ticket) -> bool {
        if (ask.service != "hcaptcha") {
            std::lock_guard<std::mutex> lk(mu_);
            snap_.phase = Phase::Failed;
            snap_.error = "Discord asked for a check this menu can't show.";
            return false;
        }
        auto page = std::make_unique<CaptchaPage>();
        std::string why;
        if (!page->Start(ask, why)) {
            std::lock_guard<std::mutex> lk(mu_);
            snap_.phase = Phase::Failed;
            snap_.error = why.empty() ? "Couldn't open the check on this network." : why;
            return false;
        }
        const std::string url = page->Url();
        captcha = std::move(page);
        captchaTicket = ticket;
        std::lock_guard<std::mutex> lk(mu_);
        snap_.phase = Phase::Captcha;
        snap_.captchaUrl = url;
        snap_.error.clear();
        LOG("remote auth: showing captcha");
        return true;
    };
    auto finishTicket = [&](const std::string& ticket, const std::string& captchaKey, const std::string& rq,
                            const std::string& session) -> bool {
        Set(Phase::LoggingIn);
        Http::Response r = PostTicket(ticket, captchaKey, rq, session);
        if (!r.Ok()) {
            auto doc = J::Parse(r.body);
            CaptchaAsk ask;
            if (captchaKey.empty() && ReadCaptcha(J::Root(doc), ask)) return beginCaptcha(ask, ticket);
            {
                std::lock_guard<std::mutex> lk(mu_);
                snap_.phase = Phase::Failed;
                snap_.error = Api::ErrorText(r);
            }
            LOG("remote auth: ticket exchange failed: HTTP %ld %s", r.status, r.body.substr(0, 200).c_str());
            ws.Close();
            return false;
        }
        auto rdoc = J::Parse(r.body);
        const std::string encrypted = J::S(J::Root(rdoc), "encrypted_token");
        std::vector<uint8_t> raw;
        std::lock_guard<std::mutex> lk(mu_);
        if (!encrypted.empty() && key.Decrypt(encrypted, raw)) {
            snap_.token.assign(raw.begin(), raw.end());
            snap_.phase = Phase::Done;
            snap_.captchaUrl.clear();
            LOG("remote auth: logged in as %s", snap_.username.c_str());
        } else {
            snap_.phase = Phase::Failed;
            snap_.error = "Discord sent an unreadable token.";
        }
        ws.Close();
        return false;
    };

    while (!stop_) {
        msgs.clear();
        bool alive = ws.Poll(msgs);
        for (auto& m : msgs) {
            J::Doc doc = J::Parse(m.data);
            yyjson_val* root = J::Root(doc);
            std::string op = J::S(root, "op");
            if (op == "hello") {
                hbInterval = J::I(root, "heartbeat_interval", 41250);
                nextHb = Now() + hbInterval;
                int64_t timeout = J::I(root, "timeout_ms", 120000);
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    snap_.expiresMs = Now() + timeout;
                }
                ws.SendText("{\"op\":\"init\",\"encoded_public_key\":\"" + key.spkiB64 + "\"}");
            } else if (op == "nonce_proof") {
                std::vector<uint8_t> nonce;
                if (!key.Decrypt(J::S(root, "encrypted_nonce"), nonce)) {
                    std::lock_guard<std::mutex> lk(mu_);
                    snap_.phase = Phase::Failed;
                    snap_.error = "Login handshake failed.";
                    return false;
                }
                std::string nonceB64 = Str::Base64Url(nonce.data(), nonce.size());
                if (hashedProof_) {
                    // Older protocol revision: SHA-256 of the nonce as "proof".
                    unsigned char digest[32];
#if MBEDTLS_VERSION_MAJOR >= 3
                    mbedtls_sha256(nonce.data(), nonce.size(), digest, 0);
#else
                    mbedtls_sha256_ret(nonce.data(), nonce.size(), digest, 0);
#endif
                    ws.SendText("{\"op\":\"nonce_proof\",\"proof\":\"" + Str::Base64Url(digest, 32) + "\"}");
                } else {
                    ws.SendText("{\"op\":\"nonce_proof\",\"nonce\":\"" + nonceB64 + "\"}");
                }
                proofSent = true;
            } else if (op == "pending_remote_init") {
                std::lock_guard<std::mutex> lk(mu_);
                snap_.qrUrl = "https://discord.com/ra/" + J::S(root, "fingerprint");
                snap_.captchaUrl.clear();
                snap_.phase = Phase::ShowQr;
                snap_.error.clear();
            } else if (op == "captcha") {
                if (!captcha) {
                    CaptchaAsk ask;
                    if (!ReadCaptcha(root, ask)) {
                        std::lock_guard<std::mutex> lk(mu_);
                        snap_.phase = Phase::Failed;
                        snap_.error = "Discord asked for a check this menu can't show.";
                        return false;
                    }
                    if (!beginCaptcha(ask, "")) return false;
                }
            } else if (op == "pending_ticket") {
                std::vector<uint8_t> payload;
                if (key.Decrypt(J::S(root, "encrypted_user_payload"), payload)) {
                    std::string s(payload.begin(), payload.end());
                    auto parts = Str::Split(s, ':');
                    std::lock_guard<std::mutex> lk(mu_);
                    if (parts.size() >= 4) {
                        snap_.userId = parts[0];
                        snap_.avatar = parts[2] == "0" ? "" : parts[2];
                        // Usernames can't contain ':', but join the rest just in case.
                        std::string name = parts[3];
                        for (size_t i = 4; i < parts.size(); i++) name += ":" + parts[i];
                        snap_.username = name;
                    }
                    snap_.phase = Phase::Confirm;
                }
            } else if (op == "pending_login") {
                if (!finishTicket(J::S(root, "ticket"), "", "", "")) return false;
            } else if (op == "cancel") {
                LOG("remote auth: cancelled on the phone");
                Set(Phase::Cancelled);
                ws.Close();
                return true;
            }
        }
        if (captcha) {
            captcha->Pump();
            if (captcha->HasToken()) {
                const std::string token = captcha->Token();
                const std::string rq = captcha->Rqtoken();
                const std::string session = captcha->SessionId();
                const std::string ticket = captchaTicket;
                captcha.reset();
                captchaTicket.clear();
                if (!ticket.empty()) {
                    if (!finishTicket(ticket, token, rq, session)) return false;
                } else {
                    std::string msg = "{\"op\":\"captcha\",\"captcha_key\":" + Str::Json(token);
                    if (!rq.empty()) msg += ",\"captcha_rqtoken\":" + Str::Json(rq);
                    msg += "}";
                    if (alive) ws.SendText(msg);
                    std::lock_guard<std::mutex> lk(mu_);
                    snap_.captchaUrl.clear();
                    if (snap_.phase == Phase::Captcha) snap_.phase = Phase::Preparing;
                    snap_.error.clear();
                }
            } else if (captcha->Expired()) {
                std::lock_guard<std::mutex> lk(mu_);
                snap_.phase = Phase::Failed;
                snap_.error = "The check timed out. Press A to try again.";
                return false;
            } else if (!captcha->WidgetError().empty()) {
                std::lock_guard<std::mutex> lk(mu_);
                snap_.error = "The check didn't open. Scan the code again.";
            }
        }
        // A ticket is already in hand: the browser can still finish after Discord
        // drops the socket. A check that still needs the socket cannot.
        if (!alive) {
            if (captcha && captchaTicket.empty()) {
                std::lock_guard<std::mutex> lk(mu_);
                snap_.phase = Phase::Failed;
                snap_.error = "Discord closed the sign-in before the check finished.";
                return false;
            }
            if (!captcha) {
                int code = ws.CloseCode();
                LOG("remote auth: closed with %d", code);
                if (proofSent && Get().phase == Phase::Preparing) {
                    // Closed after the proof and before a QR code: the proof format was
                    // rejected (whatever the close code), so the other revision next time.
                    hashedProof_ = !hashedProof_;
                }
                return true;  // timeout (4003) or network drop: new code
            }
        }
        if (alive && nextHb && Now() >= nextHb) {
            ws.SendText("{\"op\":\"heartbeat\"}");
            nextHb = Now() + hbInterval;
        }
        if (msgs.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ws.Close();
    return false;
}
