#include "remote_auth.h"
#include "net_mutex.h"
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <whb/log.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <curl/curl.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>

static const char *RA_WS_URL   = "wss://remote-auth-gateway.discord.gg/?v=2";
static const char *RA_REST_URL = "https://discord.com/api/v10/users/@me/remote-auth/login";

namespace Discord {

// base64url encode, no padding, '-' and '_' substitution
static std::string b64url_encode(const unsigned char *data, size_t len) {
    size_t out_len = 0;
    unsigned char buf[512];
    mbedtls_base64_encode(buf, sizeof(buf), &out_len, data, len);
    std::string s(reinterpret_cast<char *>(buf), out_len);
    while (!s.empty() && s.back() == '=') s.pop_back();
    for (char &c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return s;
}

// Pull a JSON string-value field: finds "key":"value", returns value.
// Handles simple \n \r \t \\ \" escapes.
static std::string json_str(const char *json, const char *key) {
    std::string pattern = std::string("\"") + key + "\":\"";
    const char *p = strstr(json, pattern.c_str());
    if (!p) return {};
    p += pattern.size();
    std::string val;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) { ++p; val += *p; }
        else val += *p;
        ++p;
    }
    return val;
}

// Pull a JSON integer field: finds "key":<integer>
static long json_int(const char *json, const char *key) {
    std::string pattern = std::string("\"") + key + "\":";
    const char *p = strstr(json, pattern.c_str());
    if (!p) return 0;
    p += pattern.size();
    while (*p == ' ') ++p;
    return strtol(p, nullptr, 10);
}

// ---- ctor/dtor ---------------------------------------------------------------

RemoteAuth::RemoteAuth() {
    mbedtls_pk_init(&pk_);
    mbedtls_entropy_init(&entropy_);
    mbedtls_ctr_drbg_init(&ctr_drbg_);
}

RemoteAuth::~RemoteAuth() {
    stop();
    mbedtls_pk_free(&pk_);
    mbedtls_entropy_free(&entropy_);
    mbedtls_ctr_drbg_free(&ctr_drbg_);
}

// ---- public accessors --------------------------------------------------------

std::string RemoteAuth::fingerprint() const {
    std::lock_guard<std::mutex> lk(mutex_); return fingerprint_;
}
std::string RemoteAuth::token() const {
    std::lock_guard<std::mutex> lk(mutex_); return token_;
}
std::string RemoteAuth::error_msg() const {
    std::lock_guard<std::mutex> lk(mutex_); return error_msg_;
}
std::string RemoteAuth::user_tag() const {
    std::lock_guard<std::mutex> lk(mutex_); return user_tag_;
}

// ---- lifecycle ---------------------------------------------------------------

void RemoteAuth::start() {
    stop_.store(false);
    state_.store(RemoteAuthState::CONNECTING);
    thread_ = std::thread(&RemoteAuth::run, this);
}

void RemoteAuth::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (curl_ws_) {
        size_t sent = 0;
        curl_ws_send(curl_ws_, "", 0, &sent, 0, CURLWS_CLOSE);
        curl_easy_cleanup(curl_ws_);
        curl_ws_ = nullptr;
    }
}

// ---- WebSocket helpers -------------------------------------------------------

bool RemoteAuth::ws_send(const std::string &json) {
    if (!curl_ws_) return false;
    size_t sent = 0;
    return curl_ws_send(curl_ws_, json.c_str(), json.size(),
                        &sent, 0, CURLWS_TEXT) == CURLE_OK;
}

bool RemoteAuth::ws_recv(std::string &out, int timeout_ms) {
    char buf[16384];
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    std::string accum;

    while (!stop_.load()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;

        size_t recv_len = 0;
        const struct curl_ws_frame *meta = nullptr;
        CURLcode rc = curl_ws_recv(curl_ws_, buf, sizeof(buf) - 1,
                                   &recv_len, &meta);

        if (rc == CURLE_AGAIN) {
            OSSleepTicks(OSMillisecondsToTicks(20));
            continue;
        }
        if (rc != CURLE_OK) return false;
        if (!meta || recv_len == 0) continue;

        if (meta->flags & CURLWS_CLOSE) return false;
        if (meta->flags & CURLWS_PING) {
            size_t s = 0;
            curl_ws_send(curl_ws_, buf, recv_len, &s, 0, CURLWS_PONG);
            // Reset deadline — still waiting for a real message
            deadline = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(timeout_ms);
            continue;
        }

        accum.append(buf, recv_len);
        if (meta->bytesleft == 0) {
            out = std::move(accum);
            return true;
        }
        // Got a fragment — extend deadline so mid-message we don't time out
        deadline = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(timeout_ms);
    }
    return false;
}

bool RemoteAuth::do_connect() {
    if (curl_ws_) { curl_easy_cleanup(curl_ws_); curl_ws_ = nullptr; }

    curl_ws_ = curl_easy_init();
    if (!curl_ws_) return false;

    // Discord's Remote Auth gateway requires browser-like headers to accept the upgrade
    struct curl_slist *hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Origin: https://discord.com");
    hdrs = curl_slist_append(hdrs,
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36");

    curl_easy_setopt(curl_ws_, CURLOPT_URL,            RA_WS_URL);
    curl_easy_setopt(curl_ws_, CURLOPT_CONNECT_ONLY,   2L);
    curl_easy_setopt(curl_ws_, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl_ws_, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl_ws_, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl_ws_, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl_ws_, CURLOPT_TIMEOUT,        0L);

    CURLcode rc;
    {
        std::lock_guard<std::mutex> net(g_http_mutex);
        rc = curl_easy_perform(curl_ws_);
    }
    curl_slist_free_all(hdrs);
    if (rc != CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl_ws_, CURLINFO_RESPONSE_CODE, &http_code);
        WHBLogPrintf("RemoteAuth connect: %s (HTTP %ld)", curl_easy_strerror(rc), http_code);
        curl_easy_cleanup(curl_ws_);
        curl_ws_ = nullptr;
        return false;
    }
    WHBLogPrint("RemoteAuth: WS connected");
    return true;
}

// ---- Crypto helpers ---------------------------------------------------------

void RemoteAuth::gen_keypair() {
    mbedtls_ctr_drbg_seed(&ctr_drbg_, mbedtls_entropy_func, &entropy_,
                           reinterpret_cast<const unsigned char *>("discord_ra"), 10);
    mbedtls_pk_setup(&pk_, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    // Set OAEP-SHA256 padding before and after keygen to be explicit
    mbedtls_rsa_set_padding(mbedtls_pk_rsa(pk_),
                            MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
    mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk_),
                        mbedtls_ctr_drbg_random, &ctr_drbg_, 2048, 65537);
    // Re-apply after keygen in case the key init reset padding
    mbedtls_rsa_set_padding(mbedtls_pk_rsa(pk_),
                            MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
}

std::string RemoteAuth::export_pubkey_b64() {
    unsigned char der[600];
    int len = mbedtls_pk_write_pubkey_der(&pk_, der, sizeof(der));
    if (len <= 0) return {};
    // mbedtls_pk_write_pubkey_der writes to the END of the buffer
    const unsigned char *start = der + sizeof(der) - (size_t)len;

    size_t b64_len = 0;
    unsigned char b64[900];
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, start, (size_t)len) != 0)
        return {};
    return std::string(reinterpret_cast<char *>(b64), b64_len);
}

bool RemoteAuth::decrypt_b64(const std::string &b64_enc, std::string &out) {
    unsigned char enc[512];
    size_t enc_len = 0;
    if (mbedtls_base64_decode(enc, sizeof(enc), &enc_len,
                              reinterpret_cast<const unsigned char *>(b64_enc.c_str()),
                              b64_enc.size()) != 0)
        return false;

    unsigned char dec[512];
    size_t dec_len = 0;
    if (mbedtls_pk_decrypt(&pk_, enc, enc_len, dec, &dec_len, sizeof(dec),
                           mbedtls_ctr_drbg_random, &ctr_drbg_) != 0)
        return false;

    out.assign(reinterpret_cast<char *>(dec), dec_len);
    return true;
}

std::string RemoteAuth::sha256_b64url(const std::string &data) {
    unsigned char hash[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char *>(data.data()),
                   data.size(), hash, 0 /* is224=false */);
    return b64url_encode(hash, 32);
}

// ---- REST ticket exchange ---------------------------------------------------

static size_t rest_write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    auto *s = static_cast<std::string *>(ud);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string RemoteAuth::exchange_ticket(const std::string &ticket) {
    CURL *curl = curl_easy_init();
    if (!curl) return {};

    std::string body     = "{\"ticket\":\"" + ticket + "\"}";
    std::string response;

    struct curl_slist *hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            RA_REST_URL);
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  rest_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        15L);

    {
        std::lock_guard<std::mutex> net(g_http_mutex);
        curl_easy_perform(curl);
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return response;
}

// ---- Main run loop -----------------------------------------------------------

void RemoteAuth::run() {
    auto fail = [this](const char *msg) {
        WHBLogPrintf("RemoteAuth FAIL: %s", msg);
        std::lock_guard<std::mutex> lk(mutex_);
        error_msg_ = msg;
        state_.store(RemoteAuthState::FAILED);
    };

    // --- Step 1: Connect ---
    if (!do_connect()) { fail("WebSocket connection failed"); return; }

    // --- Step 2: Generate RSA-2048 key pair (may take ~5s on Wii U PPC) ---
    WHBLogPrint("RemoteAuth: generating RSA-2048 key...");
    gen_keypair();
    std::string pubkey_b64 = export_pubkey_b64();
    if (pubkey_b64.empty()) { fail("Key generation failed"); return; }
    WHBLogPrint("RemoteAuth: key ready");

    // Heartbeat state (will be updated from HELLO)
    std::chrono::milliseconds hb_interval{41250};
    auto next_hb = std::chrono::steady_clock::now() + hb_interval;

    // --- Step 3: Receive HELLO ---
    {
        std::string msg;
        if (!ws_recv(msg, 20000)) { fail("No HELLO from server"); return; }
        long ms = json_int(msg.c_str(), "heartbeat_interval");
        if (ms > 0) hb_interval = std::chrono::milliseconds(ms);
        next_hb = std::chrono::steady_clock::now() + hb_interval;
        WHBLogPrintf("RemoteAuth: HELLO hb=%ldms", (long)hb_interval.count());
    }

    // --- Step 4: Send INIT ---
    if (!ws_send("{\"op\":\"init\",\"encoded_public_key\":\"" + pubkey_b64 + "\"}")) {
        fail("Failed to send init"); return;
    }

    // --- Step 5: Receive nonce_proof ---
    {
        std::string msg;
        if (!ws_recv(msg, 15000)) { fail("No nonce_proof from server"); return; }

        std::string enc_nonce = json_str(msg.c_str(), "encrypted_nonce");
        if (enc_nonce.empty()) { fail("Missing encrypted_nonce"); return; }

        std::string raw_nonce;
        if (!decrypt_b64(enc_nonce, raw_nonce)) { fail("Nonce decrypt failed"); return; }

        std::string proof = sha256_b64url(raw_nonce);
        if (!ws_send("{\"op\":\"nonce_proof\",\"proof\":\"" + proof + "\"}")) {
            fail("Failed to send nonce_proof"); return;
        }
        WHBLogPrint("RemoteAuth: nonce proof sent");
    }

    // --- Step 6: Receive pending_remote_init (fingerprint) ---
    {
        std::string msg;
        if (!ws_recv(msg, 15000)) { fail("No pending_remote_init"); return; }

        std::string fp = json_str(msg.c_str(), "fingerprint");
        if (fp.empty()) { fail("No fingerprint received"); return; }

        {
            std::lock_guard<std::mutex> lk(mutex_);
            fingerprint_ = fp;
        }
        state_.store(RemoteAuthState::WAITING_SCAN);
        WHBLogPrintf("RemoteAuth: fingerprint=%s", fp.c_str());
    }

    // --- Step 7: Wait for pending_ticket then pending_login ---
    while (!stop_.load()) {
        // Send heartbeat if due
        auto now = std::chrono::steady_clock::now();
        if (now >= next_hb) {
            ws_send("{\"op\":\"heartbeat\"}");
            next_hb = now + hb_interval;
        }

        std::string msg;
        if (!ws_recv(msg, 3000)) continue; // timeout = check heartbeat, try again

        std::string op = json_str(msg.c_str(), "op");

        if (op == "pending_ticket") {
            // User scanned — decrypt user payload: "user_id:discriminator:avatar:username"
            std::string enc = json_str(msg.c_str(), "encrypted_user_payload");
            if (!enc.empty()) {
                std::string payload;
                if (decrypt_b64(enc, payload)) {
                    // Last colon-delimited field is the username
                    size_t pos = payload.rfind(':');
                    std::string tag = (pos != std::string::npos)
                                      ? payload.substr(pos + 1) : payload;
                    std::lock_guard<std::mutex> lk(mutex_);
                    user_tag_ = tag;
                }
            }
            state_.store(RemoteAuthState::WAITING_CONFIRM);
            WHBLogPrint("RemoteAuth: pending_ticket (user scanned)");
        }
        else if (op == "pending_login") {
            std::string ticket = json_str(msg.c_str(), "ticket");
            if (ticket.empty()) { fail("Missing ticket in pending_login"); return; }

            WHBLogPrint("RemoteAuth: exchanging ticket...");
            std::string resp = exchange_ticket(ticket);
            WHBLogPrintf("RemoteAuth: ticket resp (%zu bytes)", resp.size());

            std::string enc_token = json_str(resp.c_str(), "encrypted_token");
            if (enc_token.empty()) {
                // Older API versions or error path may return plain token
                std::string plain = json_str(resp.c_str(), "token");
                if (!plain.empty()) {
                    std::lock_guard<std::mutex> lk(mutex_);
                    token_ = plain;
                    state_.store(RemoteAuthState::DONE);
                    WHBLogPrint("RemoteAuth: DONE (plain token)");
                    return;
                }
                fail("Ticket exchange failed — no encrypted_token");
                return;
            }

            std::string tok;
            if (!decrypt_b64(enc_token, tok)) { fail("Token decrypt failed"); return; }

            {
                std::lock_guard<std::mutex> lk(mutex_);
                token_ = tok;
            }
            state_.store(RemoteAuthState::DONE);
            WHBLogPrint("RemoteAuth: DONE");
            return;
        }
        else if (op == "cancel") {
            fail("Cancelled on mobile");
            return;
        }
        // heartbeat_ack: silently ignore
    }
}

} // namespace Discord
