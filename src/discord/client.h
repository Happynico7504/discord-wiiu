#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include "types.h"
#include "event_queue.h"
#include "rest.h"
#include "gateway.h"

namespace Discord {

struct ClientState {
    User            self;
    std::vector<Guild>   guilds;
    std::vector<Channel> channels;      // channels for selected guild
    std::vector<Message> messages;      // messages for selected channel
    std::string     selected_guild_id;
    std::string     selected_channel_id;
    bool            ready = false;
};

class Client {
public:
    explicit Client(const std::string &token);
    ~Client();

    bool init();
    void shutdown();

    // Call from main/UI thread to drain the event queue and update state.
    void poll();

    // REST operations – called synchronously
    bool fetch_guilds();
    bool fetch_channels(const std::string &guild_id);
    bool fetch_messages(const std::string &channel_id, int limit = 50);
    bool send_message(const std::string &channel_id, const std::string &content);

    // Async DM loading — queues work on the channel_worker thread.
    bool start_dm_load();

    // Async channel/message loading — all HTTPS happens on the worker thread,
    // so the main thread never blocks on network.
    bool start_channel_load(const std::string &guild_id);
    bool drain_channel_result();
    bool is_channels_loading() const { return channels_loading_.load(); }

    void request_messages(const std::string &channel_id, int limit = 50);
    void request_messages_refresh(const std::string &channel_id, int limit = 50);
    bool drain_message_result();
    bool is_messages_loading() const { return msg_loading_.load(); }

    const ClientState &state() const { return state_; }
    ClientState       &state()       { return state_; }

    // Optional callbacks set by the UI layer
    std::function<void()>                   on_ready;
    std::function<void(const Message &)>    on_message_create;
    std::function<void(const std::string &, const std::string &)> on_message_delete; // channel_id, message_id
    std::function<void(const Guild &)>      on_guild_available;
    std::function<void(const Channel &)>    on_channel_update;
    std::function<void(const TypingEvent &)> on_typing_start;

private:
    void handle_event(GatewayEvent &ev);
    void handle_ready(const std::string &json);
    void handle_message_create(const std::string &json);
    void handle_message_update(const std::string &json);
    void handle_message_delete(const std::string &json);
    void handle_guild_create(const std::string &json);
    void handle_channel_create(const std::string &json);
    void handle_typing_start(const std::string &json);

    std::string    token_;
    EventQueue     event_queue_;
    std::unique_ptr<RestClient> rest_;
    std::unique_ptr<Gateway>    gateway_;
    ClientState    state_;
    std::mutex     state_mutex_;
    // channels received via GUILD_CREATE, keyed by guild_id
    std::unordered_map<std::string, std::vector<Channel>> guild_channels_cache_;

    // Background network worker — handles both channel loading and message fetching.
    // Main thread never acquires g_http_mutex, so it can never be blocked by network.
    void channel_worker();
    std::atomic<bool>    channels_loading_{false};
    std::atomic<bool>    msg_loading_{false};
    std::atomic<bool>    msg_soft_refresh_{false};
    std::thread          ch_worker_thread_;
    std::mutex           ch_work_mutex_;
    std::string          ch_pending_guild_;    // "" = none
    std::string          ch_pending_channel_;  // "" = none
    int                  ch_pending_msg_limit_{50};
    bool                 ch_pending_dm_{false};
    std::atomic<bool>    ch_stop_{false};
    // Channel result staging
    std::mutex           ch_result_mutex_;
    bool                 ch_result_ready_{false};
    std::string          ch_result_guild_id_;
    std::vector<Channel> ch_result_channels_;
    // Message result staging — raw JSON is parsed on the main thread to avoid
    // concurrent malloc races with the worker's network I/O.
    std::mutex           msg_result_mutex_;
    bool                 msg_result_ready_{false};
    std::string          msg_result_channel_id_;
    std::string          msg_result_raw_json_;
    int                  msg_result_http_code_{0};
    std::vector<std::pair<std::string,std::string>> msg_result_mentions_; // (user_id, display_name)
};

} // namespace Discord
