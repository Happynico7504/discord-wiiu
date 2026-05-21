#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <curl/curl.h>
#include "event_queue.h"

namespace Discord {

// Gateway opcodes
enum class GatewayOp : int {
    DISPATCH          = 0,
    HEARTBEAT         = 1,
    IDENTIFY          = 2,
    PRESENCE_UPDATE   = 3,
    VOICE_STATE       = 4,
    RESUME            = 6,
    RECONNECT         = 7,
    REQUEST_MEMBERS   = 8,
    INVALID_SESSION   = 9,
    HELLO             = 10,
    HEARTBEAT_ACK     = 11,
};

// Intents bitmask
static constexpr int INTENT_GUILDS           = 1 << 0;
static constexpr int INTENT_GUILD_MESSAGES   = 1 << 9;
static constexpr int INTENT_GUILD_REACTIONS  = 1 << 10;
static constexpr int INTENT_DM_MESSAGES      = 1 << 12;
static constexpr int INTENT_MESSAGE_CONTENT  = 1 << 15;

static constexpr int DEFAULT_INTENTS =
    INTENT_GUILDS | INTENT_GUILD_MESSAGES |
    INTENT_GUILD_REACTIONS | INTENT_DM_MESSAGES |
    INTENT_MESSAGE_CONTENT;

class Gateway {
public:
    explicit Gateway(const std::string &token, EventQueue &queue);
    ~Gateway();

    bool connect();
    void disconnect();
    bool send_message(const std::string &json);

    bool is_connected() const { return connected_.load(); }
    const std::string &session_id() const { return session_id_; }

private:
    void recv_loop();
    void process_payload(const std::string &json);
    void handle_hello(double heartbeat_interval_ms);
    void send_heartbeat();
    void send_identify();
    void send_resume();

    static size_t ws_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata);
    bool do_ws_connect();

    std::string       token_;
    EventQueue       &queue_;
    CURL             *curl_ws_  = nullptr;
    std::thread       recv_thread_;
    std::atomic<bool> connected_   { false };
    std::atomic<bool> stop_        { false };

    int       last_sequence_        = -1;
    std::string session_id_;
    std::string resume_gateway_url_;

    std::chrono::milliseconds        heartbeat_interval_ { 41250 };
    std::chrono::steady_clock::time_point next_heartbeat_;
    bool heartbeat_ack_received_    = true;

    // Reassembly buffer for fragmented WS frames
    std::string frag_buf_;
};

} // namespace Discord
