#include "gateway.h"
#include "net_mutex.h"
#include <cJSON/cJSON.h>
#include <whb/log.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <cstring>
#include <sstream>

static const char *GATEWAY_URL =
    "wss://gateway.discord.gg/?v=10&encoding=json";

namespace Discord {

Gateway::Gateway(const std::string &token, EventQueue &queue)
    : token_(token), queue_(queue) {}

Gateway::~Gateway() {
    disconnect();
}

bool Gateway::do_ws_connect() {
    if (curl_ws_) {
        curl_easy_cleanup(curl_ws_);
        curl_ws_ = nullptr;
    }

    curl_ws_ = curl_easy_init();
    if (!curl_ws_) return false;

    const char *url = resume_gateway_url_.empty()
                      ? GATEWAY_URL
                      : resume_gateway_url_.c_str();

    curl_easy_setopt(curl_ws_, CURLOPT_URL, url);
    curl_easy_setopt(curl_ws_, CURLOPT_CONNECT_ONLY, 2L);  // WebSocket mode
    curl_easy_setopt(curl_ws_, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl_ws_, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl_ws_, CURLOPT_TIMEOUT, 0L);         // no timeout on recv
    curl_easy_setopt(curl_ws_, CURLOPT_CONNECTTIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl_ws_);
    if (res != CURLE_OK) {
        WHBLogPrintf("GW connect failed: %s", curl_easy_strerror(res));
        curl_easy_cleanup(curl_ws_);
        curl_ws_ = nullptr;
        return false;
    }

    WHBLogPrint("GW WebSocket connected");
    return true;
}

bool Gateway::connect() {
    if (!do_ws_connect()) return false;
    connected_.store(true);
    stop_.store(false);
    recv_thread_ = std::thread(&Gateway::recv_loop, this);
    return true;
}

void Gateway::disconnect() {
    stop_.store(true);
    connected_.store(false);
    if (recv_thread_.joinable()) recv_thread_.join();
    if (curl_ws_) {
        // Send close frame
        size_t sent = 0;
        curl_ws_send(curl_ws_, "", 0, &sent, 0, CURLWS_CLOSE);
        curl_easy_cleanup(curl_ws_);
        curl_ws_ = nullptr;
    }
    queue_.clear();
}

bool Gateway::send_message(const std::string &json) {
    if (!curl_ws_ || !connected_.load()) return false;
    size_t sent = 0;
    CURLcode res = curl_ws_send(curl_ws_, json.c_str(), json.size(),
                                &sent, 0, CURLWS_TEXT);
    return res == CURLE_OK;
}

// Remove a top-level JSON array by key, replacing its content with [].
// Operates on the raw string so we can strip before cJSON_Parse.
static void strip_array_key(std::string &json, const char *key) {
    std::string pattern = std::string("\"") + key + "\":[";
    size_t pos = json.find(pattern);
    while (pos != std::string::npos) {
        size_t arr_start = pos + pattern.size() - 1;  // index of '['
        int depth = 0;
        bool in_str = false;
        size_t p = arr_start;
        while (p < json.size()) {
            char c = json[p];
            if (in_str) {
                if (c == '\\') p++;
                else if (c == '"') in_str = false;
            } else {
                if (c == '"') in_str = true;
                else if (c == '[' || c == '{') depth++;
                else if (c == ']' || c == '}') {
                    depth--;
                    if (depth == 0) { p++; break; }
                }
            }
            p++;
        }
        json.replace(arr_start, p - arr_start, "[]");
        pos = json.find(pattern, arr_start + 2);
    }
}

void Gateway::recv_loop() {
    char buf[65536];

    while (!stop_.load()) {
        // ---- heartbeat timer ----
        auto now = std::chrono::steady_clock::now();
        if (now >= next_heartbeat_ && connected_.load()) {
            if (!heartbeat_ack_received_) {
                WHBLogPrint("GW heartbeat ACK missing, reconnecting");
                connected_.store(false);
                // Reconnect logic below
            } else {
                send_heartbeat();
                heartbeat_ack_received_ = false;
                next_heartbeat_ = now + heartbeat_interval_;
            }
        }

        if (!connected_.load()) {
            // Attempt reconnect — hold g_http_mutex so the new TLS handshake is
            // serialised with any other TLS work (avatar_worker, channel_worker).
            // Concurrent TLS handshakes deadlock the Wii U mbedTLS stack.
            frag_buf_.clear();
            bool ok;
            {
                std::lock_guard<std::mutex> net(g_http_mutex);
                ok = do_ws_connect();
            }
            if (ok) {
                connected_.store(true);
                heartbeat_ack_received_ = true;
                if (!session_id_.empty())
                    send_resume();
                // HELLO will set up heartbeat again
            } else {
                OSSleepTicks(OSMillisecondsToTicks(5000));
            }
            continue;
        }

        size_t recv_len = 0;
        const struct curl_ws_frame *meta = nullptr;
        CURLcode res = curl_ws_recv(curl_ws_, buf, sizeof(buf) - 1,
                                    &recv_len, &meta);

        if (res == CURLE_AGAIN) {
            OSSleepTicks(OSMillisecondsToTicks(10));
            continue;
        }

        if (res != CURLE_OK) {
            WHBLogPrintf("GW recv error: %s", curl_easy_strerror(res));
            connected_.store(false);
            continue;
        }

        if (!meta || recv_len == 0) continue;

        // Handle close frames
        if (meta->flags & CURLWS_CLOSE) {
            WHBLogPrint("GW received CLOSE frame");
            connected_.store(false);
            continue;
        }

        // Handle ping
        if (meta->flags & CURLWS_PING) {
            size_t sent = 0;
            curl_ws_send(curl_ws_, buf, recv_len, &sent, 0, CURLWS_PONG);
            continue;
        }

        // Accumulate fragmented frames; bytesleft > 0 means more chunks incoming
        frag_buf_.append(buf, recv_len);
        if (meta->bytesleft > 0) continue;

        // Strip arrays we never use from GUILD_CREATE and READY before cJSON
        // ever sees them.  Roles, emojis, and stickers alone can be several MB
        // on large servers; members/presences can be tens of MB.
        // strip_array_key is a no-op when a key is absent, so this is safe for
        // any event type.
        {
            bool is_guild = frag_buf_.find("\"GUILD_CREATE\"") != std::string::npos;
            bool is_ready = !is_guild &&
                            frag_buf_.find("\"READY\"") != std::string::npos;
            if (is_guild || is_ready) {
                static const char *BIG[] = {
                    "members", "presences", "voice_states", "threads",
                    "stage_instances", "embedded_activities",
                    "roles", "emojis", "stickers",
                    "guild_scheduled_events", "soundboard_sounds",
                    "permission_overwrites",  // per-channel, can be large on big servers
                    nullptr
                };
                size_t before = frag_buf_.size();
                for (int i = 0; BIG[i]; i++) strip_array_key(frag_buf_, BIG[i]);
                WHBLogPrintf("GW: stripped %s %zu -> %zu bytes",
                             is_guild ? "GUILD_CREATE" : "READY",
                             before, frag_buf_.size());
            }
        }

        process_payload(frag_buf_);
        frag_buf_.clear();
    }
}

static GatewayEventType event_name_to_type(const char *name) {
    if (!name) return GatewayEventType::UNKNOWN;
    if (strcmp(name, "READY")          == 0) return GatewayEventType::READY;
    if (strcmp(name, "RESUMED")        == 0) return GatewayEventType::RESUMED;
    if (strcmp(name, "MESSAGE_CREATE") == 0) return GatewayEventType::MESSAGE_CREATE;
    if (strcmp(name, "MESSAGE_UPDATE") == 0) return GatewayEventType::MESSAGE_UPDATE;
    if (strcmp(name, "MESSAGE_DELETE") == 0) return GatewayEventType::MESSAGE_DELETE;
    if (strcmp(name, "GUILD_CREATE")   == 0) return GatewayEventType::GUILD_CREATE;
    if (strcmp(name, "GUILD_UPDATE")   == 0) return GatewayEventType::GUILD_UPDATE;
    if (strcmp(name, "GUILD_DELETE")   == 0) return GatewayEventType::GUILD_DELETE;
    if (strcmp(name, "CHANNEL_CREATE") == 0) return GatewayEventType::CHANNEL_CREATE;
    if (strcmp(name, "CHANNEL_UPDATE") == 0) return GatewayEventType::CHANNEL_UPDATE;
    if (strcmp(name, "CHANNEL_DELETE") == 0) return GatewayEventType::CHANNEL_DELETE;
    if (strcmp(name, "TYPING_START")   == 0) return GatewayEventType::TYPING_START;
    return GatewayEventType::UNKNOWN;
}

void Gateway::process_payload(const std::string &json) {
    cJSON *root = cJSON_Parse(json.c_str());
    if (!root) {
        WHBLogPrintf("GW failed to parse JSON payload (%zu bytes)", json.size());
        return;
    }

    cJSON *op_item = cJSON_GetObjectItem(root, "op");
    cJSON *d_item  = cJSON_GetObjectItem(root, "d");
    cJSON *s_item  = cJSON_GetObjectItem(root, "s");
    cJSON *t_item  = cJSON_GetObjectItem(root, "t");

    int op = op_item ? (int)op_item->valuedouble : -1;

    if (s_item && cJSON_IsNumber(s_item))
        last_sequence_ = (int)s_item->valuedouble;

    switch ((GatewayOp)op) {
        case GatewayOp::HELLO: {
            cJSON *hi = cJSON_GetObjectItem(d_item, "heartbeat_interval");
            if (hi) handle_hello(hi->valuedouble);
            break;
        }
        case GatewayOp::HEARTBEAT_ACK:
            heartbeat_ack_received_ = true;
            break;
        case GatewayOp::HEARTBEAT:
            send_heartbeat();
            break;
        case GatewayOp::RECONNECT: {
            GatewayEvent ev;
            ev.type = GatewayEventType::RECONNECT;
            queue_.push(ev);
            connected_.store(false);
            break;
        }
        case GatewayOp::INVALID_SESSION: {
            GatewayEvent ev;
            ev.type = GatewayEventType::INVALID_SESSION;
            queue_.push(ev);
            bool resumable = d_item && cJSON_IsTrue(d_item);
            if (!resumable) session_id_.clear();
            connected_.store(false);
            break;
        }
        case GatewayOp::DISPATCH: {
            const char *t = t_item ? t_item->valuestring : nullptr;
            char *d_str = d_item ? cJSON_PrintUnformatted(d_item) : nullptr;

            if (t && strcmp(t, "READY") == 0 && d_item) {
                cJSON *sid = cJSON_GetObjectItem(d_item, "session_id");
                cJSON *rgw = cJSON_GetObjectItem(d_item, "resume_gateway_url");
                if (sid && sid->valuestring)  session_id_ = sid->valuestring;
                if (rgw && rgw->valuestring)  resume_gateway_url_ = rgw->valuestring;
                WHBLogPrintf("GW READY, session %s", session_id_.c_str());
            }

            GatewayEvent ev;
            ev.type = event_name_to_type(t);
            ev.event_name = t ? t : "";
            ev.raw_json = d_str ? d_str : "null";
            ev.sequence = last_sequence_;
            if (d_str) cJSON_free(d_str);

            queue_.push(std::move(ev));
            break;
        }
        default:
            break;
    }

    cJSON_Delete(root);
}

void Gateway::handle_hello(double interval_ms) {
    heartbeat_interval_ = std::chrono::milliseconds((long long)interval_ms);
    next_heartbeat_ = std::chrono::steady_clock::now() + heartbeat_interval_;
    heartbeat_ack_received_ = true;
    WHBLogPrintf("GW HELLO, heartbeat every %d ms", (int)interval_ms);
    send_identify();
}

void Gateway::send_heartbeat() {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "op", (double)GatewayOp::HEARTBEAT);
    if (last_sequence_ >= 0)
        cJSON_AddNumberToObject(root, "d", last_sequence_);
    else
        cJSON_AddNullToObject(root, "d");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        send_message(json);
        cJSON_free(json);
    }
}

// Strip optional "Bot " prefix — IDENTIFY and RESUME always use the raw token.
static std::string raw_token(const std::string &token) {
    if (token.size() > 4 && token.substr(0, 4) == "Bot ")
        return token.substr(4);
    return token;
}

static bool is_bot_token(const std::string &token) {
    return token.size() > 4 && token.substr(0, 4) == "Bot ";
}

void Gateway::send_identify() {
    const std::string tok  = raw_token(token_);
    const bool        bot  = is_bot_token(token_);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "op", (double)GatewayOp::IDENTIFY);

    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "token", tok.c_str());

    if (bot) {
        // Bot tokens use intents to opt into event categories
        cJSON_AddNumberToObject(d, "intents", DEFAULT_INTENTS);
    } else {
        // User tokens use capabilities instead of intents;
        // 16381 enables guilds, messages, presence, and most client events
        cJSON_AddNumberToObject(d, "capabilities", 16381);
    }

    cJSON *props = cJSON_CreateObject();
    if (bot) {
        cJSON_AddStringToObject(props, "os",      "linux");
        cJSON_AddStringToObject(props, "browser", "wiiu-discord");
        cJSON_AddStringToObject(props, "device",  "Nintendo Wii U");
    } else {
        // User connections must present browser-like properties or Discord
        // will close the session with an INVALID_SESSION.
        cJSON_AddStringToObject(props, "os",      "Windows");
        cJSON_AddStringToObject(props, "browser", "Chrome");
        cJSON_AddStringToObject(props, "device",  "");
        cJSON_AddStringToObject(props, "system_locale",      "en-US");
        cJSON_AddStringToObject(props, "browser_user_agent",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/128.0.0.0 Safari/537.36");
        cJSON_AddStringToObject(props, "browser_version", "128.0.0.0");
        cJSON_AddStringToObject(props, "os_version",      "10");
        cJSON_AddStringToObject(props, "release_channel", "stable");
        cJSON_AddNumberToObject(props, "client_build_number", 322432);
        cJSON_AddNullToObject(props,   "client_event_source");
    }
    cJSON_AddItemToObject(d, "properties", props);

    cJSON *presence = cJSON_CreateObject();
    cJSON_AddNumberToObject(presence, "since", 0);
    cJSON_AddItemToObject(presence, "activities", cJSON_CreateArray());
    cJSON_AddStringToObject(presence, "status", "online");
    cJSON_AddFalseToObject(presence, "afk");
    cJSON_AddItemToObject(d, "presence", presence);

    if (!bot) {
        // client_state lets the gateway know we start from a clean slate
        cJSON *cs = cJSON_CreateObject();
        cJSON_AddItemToObject(cs, "guild_versions", cJSON_CreateObject());
        cJSON_AddStringToObject(cs, "highest_last_message_id", "0");
        cJSON_AddNumberToObject(cs, "read_state_version", 0);
        cJSON_AddNumberToObject(cs, "user_guild_settings_version", -1);
        cJSON_AddNumberToObject(cs, "user_settings_version", -1);
        cJSON_AddStringToObject(cs, "private_channels_version", "0");
        cJSON_AddNumberToObject(cs, "api_code_version", 0);
        cJSON_AddItemToObject(d, "client_state", cs);
    }

    cJSON_AddItemToObject(root, "d", d);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        send_message(json);
        cJSON_free(json);
        WHBLogPrintf("GW IDENTIFY sent (%s)", bot ? "bot" : "user");
    }
}

void Gateway::send_resume() {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "op", (double)GatewayOp::RESUME);

    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "token",      raw_token(token_).c_str());
    cJSON_AddStringToObject(d, "session_id", session_id_.c_str());
    cJSON_AddNumberToObject(d, "seq",        last_sequence_);
    cJSON_AddItemToObject(root, "d", d);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        send_message(json);
        cJSON_free(json);
        WHBLogPrint("GW RESUME sent");
    }
}

} // namespace Discord
