#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <curl/curl.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

namespace Discord {

enum class RemoteAuthState {
    IDLE,
    CONNECTING,
    WAITING_SCAN,     // QR fingerprint ready, waiting for mobile scan
    WAITING_CONFIRM,  // Mobile scanned, waiting for approval tap
    DONE,
    FAILED,
};

class RemoteAuth {
public:
    RemoteAuth();
    ~RemoteAuth();

    void start();
    void stop();

    RemoteAuthState state()     const { return state_.load(); }
    std::string fingerprint()   const;
    std::string token()         const;
    std::string error_msg()     const;
    std::string user_tag()      const;  // username shown after scan

private:
    void        run();
    bool        ws_send(const std::string &json);
    bool        ws_recv(std::string &out, int timeout_ms);
    bool        do_connect();
    void        gen_keypair();
    std::string export_pubkey_b64();
    bool        decrypt_b64(const std::string &b64_enc, std::string &out);
    std::string sha256_b64url(const std::string &data);
    std::string exchange_ticket(const std::string &ticket);

    CURL *curl_ws_ = nullptr;

    mbedtls_pk_context       pk_;
    mbedtls_entropy_context  entropy_;
    mbedtls_ctr_drbg_context ctr_drbg_;

    std::thread              thread_;
    std::atomic<bool>        stop_{false};
    std::atomic<RemoteAuthState> state_{RemoteAuthState::IDLE};

    mutable std::mutex mutex_;
    std::string fingerprint_;
    std::string token_;
    std::string error_msg_;
    std::string user_tag_;
};

} // namespace Discord
