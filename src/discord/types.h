#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace Discord {

struct User {
    std::string id;
    std::string username;
    std::string discriminator;
    std::string global_name;   // display name (empty for bots / old accounts)
    std::string avatar;        // avatar hash; empty = no custom avatar
    bool bot = false;
};

struct Guild {
    std::string id;
    std::string name;
    std::string icon;
    bool available = true;
};

enum class ChannelType : int {
    GUILD_TEXT      = 0,
    DM              = 1,
    GUILD_VOICE     = 2,
    GROUP_DM        = 3,
    GUILD_CATEGORY  = 4,
    GUILD_NEWS      = 5,
    GUILD_FORUM     = 15,
    GUILD_MEDIA     = 16,
};

struct Channel {
    std::string id;
    std::string guild_id;
    std::string name;
    std::string topic;
    ChannelType type = ChannelType::GUILD_TEXT;
    int position = 0;
    std::string parent_id;
};

struct Attachment {
    std::string id;
    std::string filename;
    std::string content_type;  // MIME type from Discord API ("image/png", "video/mp4", …)
    std::string url;
    int size = 0;
};

struct Message {
    std::string id;
    std::string channel_id;
    std::string guild_id;
    User author;
    std::string content;
    std::string timestamp;
    bool edited = false;
    bool pinned = false;
    std::vector<Attachment> attachments;
};

struct TypingEvent {
    std::string channel_id;
    std::string user_id;
    uint64_t timestamp = 0;
};

} // namespace Discord
