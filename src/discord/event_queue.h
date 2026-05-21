#pragma once
#include <queue>
#include <string>
#include <mutex>
#include <condition_variable>

namespace Discord {

enum class GatewayEventType {
    READY,
    RESUMED,
    MESSAGE_CREATE,
    MESSAGE_UPDATE,
    MESSAGE_DELETE,
    GUILD_CREATE,
    GUILD_UPDATE,
    GUILD_DELETE,
    CHANNEL_CREATE,
    CHANNEL_UPDATE,
    CHANNEL_DELETE,
    TYPING_START,
    RECONNECT,
    INVALID_SESSION,
    UNKNOWN,
};

struct GatewayEvent {
    GatewayEventType type = GatewayEventType::UNKNOWN;
    std::string raw_json;  // full "d" payload as JSON string
    std::string event_name;
    int sequence = -1;
};

class EventQueue {
public:
    void push(GatewayEvent ev) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(ev));
        cv_.notify_one();
    }

    bool try_pop(GatewayEvent &ev) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        ev = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    bool wait_pop(GatewayEvent &ev, int timeout_ms = 100) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [this] { return !queue_.empty(); })) {
            ev = std::move(queue_.front());
            queue_.pop();
            return true;
        }
        return false;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) queue_.pop();
    }

private:
    std::queue<GatewayEvent> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace Discord
