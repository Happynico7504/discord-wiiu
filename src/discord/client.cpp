#include "client.h"
#include "net_mutex.h"
#include <cJSON/cJSON.h>
#include <whb/log.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <thread>
#include <sys/stat.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>

static const char *CACHE_DIR      = "/vol/external01/wiiu/discord_wiiu/cache";
static const int   CACHE_TTL_SECS = 14400; // 4 hours

namespace Discord {

// ---- helpers ----------------------------------------------------------------

// Strip one "key":{...} entry from a JSON string in-place (object values).
static void strip_object_key(std::string &json, const char *key) {
    std::string needle = std::string("\"") + key + "\":{";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        size_t brace = pos + needle.size() - 1;
        int depth = 1;
        size_t i = brace + 1;
        while (i < json.size() && depth > 0) {
            char c = json[i];
            if (c == '\\') { i += 2; continue; }
            if (c == '"') {
                i++;
                while (i < json.size()) {
                    if (json[i] == '\\') { i += 2; continue; }
                    if (json[i] == '"') { i++; break; }
                    i++;
                }
                continue;
            }
            if (c == '{') depth++;
            else if (c == '}') depth--;
            i++;
        }
        json.replace(brace, i - brace, "{}");
        pos = brace + 2;
    }
}

// Strip one "key":[...] entry from a JSON string in-place.
static void strip_array_key(std::string &json, const char *key) {
    std::string needle = std::string("\"") + key + "\":[";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        size_t bracket = pos + needle.size() - 1; // points at '['
        int depth = 1;
        size_t i = bracket + 1;
        while (i < json.size() && depth > 0) {
            char c = json[i];
            if (c == '\\') { i += 2; continue; }
            if (c == '"') {
                i++;
                while (i < json.size()) {
                    if (json[i] == '\\') { i += 2; continue; }
                    if (json[i] == '"') { i++; break; }
                    i++;
                }
                continue;
            }
            if (c == '[') depth++;
            else if (c == ']') depth--;
            i++;
        }
        // Replace the array contents with []
        json.replace(bracket, i - bracket, "[]");
        pos = bracket + 2;
    }
}

// Strip string-typed key: "key":"value" → "key":"" (keeps valid JSON, clears the value).
// Used to zero out large inlined fields (waveform, placeholder) before staging.
static void strip_string_key(std::string &json, const char *key) {
    std::string needle = std::string("\"") + key + "\":\"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        size_t val = pos + needle.size();
        size_t i   = val;
        while (i < json.size()) {
            if (json[i] == '\\') { i += 2; continue; }
            if (json[i] == '"')  { break; }
            i++;
        }
        if (i > val) json.erase(val, i - val);
        pos = val;
    }
}

// Minimal JSON field extractors for the per-object channel fallback.
// These avoid cJSON (and thus malloc) in the worker thread, preventing
// heap corruption from concurrent malloc calls with the main thread.
static std::string json_str_field(const char *obj, const char *end, const char *key) {
    const size_t klen = strlen(key);
    for (const char *p = obj; p + klen + 3 < end; p++) {
        if (p[0] != '"' || memcmp(p + 1, key, klen) != 0) continue;
        if (p[klen+1] != '"' || p[klen+2] != ':' || p[klen+3] != '"') continue;
        const char *v = p + klen + 4;
        std::string result;
        while (v < end && *v != '"') {
            if (*v == '\\') {
                v++;
                if (v < end) {
                    switch (*v) {
                        case 'n': result += '\n'; break;
                        case 't': result += '\t'; break;
                        case 'r': result += '\r'; break;
                        case '"': result += '"';  break;
                        case '\\': result += '\\'; break;
                        case '/': result += '/';  break;
                        case 'b': result += '\b'; break;
                        case 'f': result += '\f'; break;
                        case 'u': {
                            // \uXXXX — decode 4 hex digits and emit UTF-8.
                            // Handles surrogate pairs (\uD800–\uDBFF followed by \uDC00–\uDFFF)
                            // for emoji above U+FFFF (e.g. 😀 = 😀).
                            auto hx = [](char c) -> uint32_t {
                                if (c >= '0' && c <= '9') return (uint32_t)(c - '0');
                                if (c >= 'a' && c <= 'f') return (uint32_t)(c - 'a' + 10);
                                if (c >= 'A' && c <= 'F') return (uint32_t)(c - 'A' + 10);
                                return 0;
                            };
                            if (v + 4 < end) {
                                uint32_t cp = (hx(v[1]) << 12) | (hx(v[2]) << 8)
                                            | (hx(v[3]) <<  4) |  hx(v[4]);
                                v += 4; // skip 4 hex digits; outer v++ skips 'u'
                                // Surrogate pair: high surrogate + \uXXXX low surrogate
                                if (cp >= 0xD800u && cp <= 0xDBFFu
                                        && v + 6 < end && v[1] == '\\' && v[2] == 'u') {
                                    uint32_t lo = (hx(v[3]) << 12) | (hx(v[4]) << 8)
                                                | (hx(v[5]) <<  4) |  hx(v[6]);
                                    if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                                        v += 6; // skip \uXXXX of low surrogate
                                    }
                                }
                                // Encode codepoint as UTF-8
                                if (cp < 0x80u) {
                                    result += (char)cp;
                                } else if (cp < 0x800u) {
                                    result += (char)(0xC0u | (cp >> 6));
                                    result += (char)(0x80u | (cp & 0x3Fu));
                                } else if (cp < 0x10000u) {
                                    result += (char)(0xE0u | (cp >> 12));
                                    result += (char)(0x80u | ((cp >> 6) & 0x3Fu));
                                    result += (char)(0x80u | (cp & 0x3Fu));
                                } else {
                                    result += (char)(0xF0u | (cp >> 18));
                                    result += (char)(0x80u | ((cp >> 12) & 0x3Fu));
                                    result += (char)(0x80u | ((cp >> 6) & 0x3Fu));
                                    result += (char)(0x80u | (cp & 0x3Fu));
                                }
                            } else {
                                result += 'u'; // truncated — treat literally
                            }
                            break;
                        }
                        default:  result += *v;   break;
                    }
                    v++;
                }
                continue;
            }
            result += *v++;
        }
        return result;
    }
    return {};
}

static int json_int_field(const char *obj, const char *end, const char *key, int def = 0) {
    const size_t klen = strlen(key);
    for (const char *p = obj; p + klen + 2 < end; p++) {
        if (p[0] != '"' || memcmp(p + 1, key, klen) != 0) continue;
        if (p[klen+1] != '"' || p[klen+2] != ':') continue;
        const char *v = p + klen + 3;
        while (v < end && (*v == ' ' || *v == '\t')) v++;
        if (v >= end || (*v != '-' && (*v < '0' || *v > '9'))) return def;
        int sign = 1; if (*v == '-') { sign = -1; v++; }
        int t = 0;
        while (v < end && *v >= '0' && *v <= '9') { t = t * 10 + (*v++ - '0'); }
        return sign * t;
    }
    return def;
}

static bool json_bool_field(const char *obj, const char *end, const char *key, bool def = false) {
    const size_t klen = strlen(key);
    for (const char *p = obj; p + klen + 2 < end; p++) {
        if (p[0] != '"' || memcmp(p + 1, key, klen) != 0) continue;
        if (p[klen+1] != '"' || p[klen+2] != ':') continue;
        const char *v = p + klen + 3;
        while (v < end && (*v == ' ' || *v == '\t')) v++;
        if (v + 4 <= end && memcmp(v, "true",  4) == 0) return true;
        if (v + 5 <= end && memcmp(v, "false", 5) == 0) return false;
        return def;
    }
    return def;
}

// Returns true if the field exists and its value is not JSON null.
static bool json_field_is_nonnull(const char *obj, const char *end, const char *key) {
    const size_t klen = strlen(key);
    for (const char *p = obj; p + klen + 2 < end; p++) {
        if (p[0] != '"' || memcmp(p + 1, key, klen) != 0) continue;
        if (p[klen+1] != '"' || p[klen+2] != ':') continue;
        const char *v = p + klen + 3;
        while (v < end && (*v == ' ' || *v == '\t')) v++;
        if (v + 4 <= end && memcmp(v, "null", 4) == 0) return false;
        return v < end;
    }
    return false;
}

// Locate the span of a nested object value: "key":{...} → [out_s, out_e).
static void json_obj_bounds(const char *obj, const char *end, const char *key,
                             const char **out_s, const char **out_e) {
    *out_s = *out_e = nullptr;
    const size_t klen = strlen(key);
    for (const char *p = obj; p + klen + 3 <= end; p++) {
        if (p[0] != '"' || memcmp(p + 1, key, klen) != 0) continue;
        if (p[klen+1] != '"' || p[klen+2] != ':' || p[klen+3] != '{') continue;
        const char *s = p + klen + 3;
        int depth = 0; bool in_str = false;
        const char *q = s;
        while (q < end) {
            char c = *q;
            if (in_str) {
                if (c == '\\') { q++; if (q < end) q++; continue; }
                if (c == '"')  { in_str = false; q++; continue; }
            } else {
                if (c == '"')  { in_str = true;  q++; continue; }
                if (c == '{')  { depth++; q++; continue; }
                if (c == '}')  { depth--; q++; if (!depth) break; continue; }
            }
            q++;
        }
        *out_s = s; *out_e = q;
        return;
    }
}

// Parse message attachments without cJSON — walks the "attachments":[{...},...] array.
static std::vector<Attachment> parse_attachments_raw(const char *obj, const char *end) {
    std::vector<Attachment> result;
    const char needle[] = "\"attachments\":[";
    const size_t nl = sizeof(needle) - 1;
    const char *p = obj;
    while (p + nl <= end) {
        if (memcmp(p, needle, nl) == 0) break;
        p++;
    }
    if (p + nl > end) return result;
    p += nl;

    while (p < end) {
        while (p < end && (*p==' '||*p==','||*p=='\t'||*p=='\r'||*p=='\n')) p++;
        if (p >= end || *p == ']') break;
        if (*p != '{') break;
        const char *as = p;
        int depth = 0; bool in_str = false;
        while (p < end) {
            char c = *p;
            if (in_str) {
                if (c=='\\') { p++; if (p<end) p++; continue; }
                if (c=='"')  { in_str=false; p++; continue; }
            } else {
                if (c=='"')  { in_str=true;  p++; continue; }
                if (c=='{')  { depth++; p++; continue; }
                if (c=='}')  { depth--; p++; if (!depth) break; continue; }
            }
            p++;
        }
        Attachment att;
        att.id           = json_str_field(as, p, "id");
        att.filename     = json_str_field(as, p, "filename");
        att.content_type = json_str_field(as, p, "content_type");
        att.url          = json_str_field(as, p, "url");
        att.size         = json_int_field(as, p, "size");
        result.push_back(att);
    }
    return result;
}

// Parse a single message object from raw bytes — no cJSON, no separate heap allocation.
// Used in the fallback path to avoid concurrent malloc races with the gateway thread.
static Message parse_message_raw(const char *start, const char *end) {
    Message m;
    m.id         = json_str_field(start, end, "id");
    m.channel_id = json_str_field(start, end, "channel_id");
    m.guild_id   = json_str_field(start, end, "guild_id");
    m.content    = json_str_field(start, end, "content");
    m.timestamp  = json_str_field(start, end, "timestamp");
    m.pinned     = json_bool_field(start, end, "pinned");
    m.edited     = json_field_is_nonnull(start, end, "edited_timestamp");

    const char *auth_s = nullptr, *auth_e = nullptr;
    json_obj_bounds(start, end, "author", &auth_s, &auth_e);
    if (auth_s && auth_e) {
        m.author.id            = json_str_field(auth_s, auth_e, "id");
        m.author.username      = json_str_field(auth_s, auth_e, "username");
        m.author.discriminator = json_str_field(auth_s, auth_e, "discriminator");
        m.author.global_name   = json_str_field(auth_s, auth_e, "global_name");
        m.author.avatar        = json_str_field(auth_s, auth_e, "avatar");
        m.author.bot           = json_bool_field(auth_s, auth_e, "bot");
    }

    m.attachments = parse_attachments_raw(start, end);
    return m;
}

// Collect (user_id, display_name) pairs from all "mentions":[...] arrays in raw JSON.
// Scans for every occurrence so it works on both a single message object and a
// full messages-array response (multiple messages, each with their own mentions).
static std::vector<std::pair<std::string,std::string>>
parse_mentions_from_raw(const char *obj, const char *end) {
    std::vector<std::pair<std::string,std::string>> result;
    const char needle[] = "\"mentions\":[";
    const size_t nl = sizeof(needle) - 1;
    const char *p = obj;
    while (p + (ptrdiff_t)nl <= end) {
        if (memcmp(p, needle, nl) != 0) { p++; continue; }
        p += nl; // past '['
        while (p < end) {
            while (p < end && (*p==' '||*p==','||*p=='\t'||*p=='\r'||*p=='\n')) p++;
            if (p >= end || *p == ']') break;
            if (*p != '{') break;
            const char *ms = p;
            int depth = 0; bool in_str = false;
            while (p < end) {
                char c = *p;
                if (in_str) {
                    if (c=='\\') { p++; if (p<end) p++; continue; }
                    if (c=='"')  { in_str=false; p++; continue; }
                } else {
                    if (c=='"')  { in_str=true;  p++; continue; }
                    if (c=='{')  { depth++; p++; continue; }
                    if (c=='}')  { depth--; p++; if (!depth) break; continue; }
                }
                p++;
            }
            std::string id = json_str_field(ms, p, "id");
            if (!id.empty()) {
                bool dup = false;
                for (const auto &r : result) if (r.first == id) { dup = true; break; }
                if (!dup) {
                    std::string name = json_str_field(ms, p, "global_name");
                    if (name.empty()) name = json_str_field(ms, p, "username");
                    result.emplace_back(std::move(id), std::move(name));
                }
            }
        }
        // outer loop continues scanning for further "mentions":[ occurrences
    }
    return result;
}

// Replace <@id> and <@!id> in content with @display_name for all known mentions.
static void apply_mention_replacements(
    std::string &content,
    const std::vector<std::pair<std::string,std::string>> &mentions)
{
    for (const auto &mn : mentions) {
        const std::string &id   = mn.first;
        const std::string disp  = "@" + mn.second;
        // <@!id> — older mobile mention format
        {
            const std::string pat = "<@!" + id + ">";
            size_t pos = 0;
            while ((pos = content.find(pat, pos)) != std::string::npos) {
                content.replace(pos, pat.size(), disp);
                pos += disp.size();
            }
        }
        // <@id>
        {
            const std::string pat = "<@" + id + ">";
            size_t pos = 0;
            while ((pos = content.find(pat, pos)) != std::string::npos) {
                content.replace(pos, pat.size(), disp);
                pos += disp.size();
            }
        }
    }
}

static std::string safe_str(cJSON *j, const char *key) {
    if (!j) return {};
    cJSON *item = cJSON_GetObjectItem(j, key);
    if (item && item->valuestring) return item->valuestring;
    return {};
}

static int safe_int(cJSON *j, const char *key, int def = 0) {
    if (!j) return def;
    cJSON *item = cJSON_GetObjectItem(j, key);
    if (item && cJSON_IsNumber(item)) return (int)item->valuedouble;
    return def;
}

// Forward declarations for file-local JSON parsers (defined at bottom of file)
static User    parse_user(cJSON *j);
static Guild   parse_guild(cJSON *j);
static Message parse_message(cJSON *j);

// ---- disk channel cache -------------------------------------------------
// Format: line 1 = unix timestamp, subsequent lines = TSV rows:
//   channel_id TAB type TAB position TAB name TAB parent_id
// Written with plain fprintf (no cJSON) to avoid malloc in the worker thread.

static std::string channel_cache_path(const std::string &guild_id) {
    return std::string(CACHE_DIR) + "/" + guild_id + ".ch";
}

static void save_channel_cache(const std::string &guild_id,
                                const std::vector<Discord::Channel> &channels) {
    mkdir(CACHE_DIR, 0755); // no-op if already exists
    std::string path = channel_cache_path(guild_id);
    FILE *f = fopen(path.c_str(), "w");
    if (!f) { WHBLogPrintf("Cache: failed to open %s for write", path.c_str()); return; }
    fprintf(f, "%lld\n", (long long)time(nullptr));
    for (const auto &ch : channels) {
        fprintf(f, "%s\t%d\t%d\t%s\t%s\n",
                ch.id.c_str(), (int)ch.type, ch.position,
                ch.name.c_str(), ch.parent_id.c_str());
    }
    fclose(f);
    WHBLogPrintf("Cache: saved %zu channels for %s", channels.size(), guild_id.c_str());
}

static bool load_channel_cache(const std::string &guild_id,
                                std::vector<Discord::Channel> &out) {
    std::string path = channel_cache_path(guild_id);
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return false;

    char line[1024];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return false; }

    long long cached_at = strtoll(line, nullptr, 10);
    if (time(nullptr) - (time_t)cached_at > CACHE_TTL_SECS) {
        fclose(f);
        WHBLogPrintf("Cache: expired for %s", guild_id.c_str());
        return false;
    }

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        char *id     = strtok(line, "\t");
        char *type_s = strtok(nullptr, "\t");
        char *pos_s  = strtok(nullptr, "\t");
        char *name   = strtok(nullptr, "\t");
        char *parent = strtok(nullptr, "\t");
        if (!id || !type_s || !pos_s || !name) continue;

        Discord::Channel ch;
        ch.guild_id  = guild_id;
        ch.id        = id;
        ch.type      = (Discord::ChannelType)atoi(type_s);
        ch.position  = atoi(pos_s);
        ch.name      = name;
        if (parent) ch.parent_id = parent;
        out.push_back(ch);
    }
    fclose(f);
    WHBLogPrintf("Cache: loaded %zu channels for %s", out.size(), guild_id.c_str());
    return !out.empty();
}

// --- permission helpers (zero heap allocation) ---

static inline bool ow_id_eq(const char *ow_s, const char *ow_e,
                              const char *id, size_t id_len) {
    for (const char *q = ow_s; q + (ptrdiff_t)(id_len + 6) <= ow_e; q++) {
        if (q[0]=='"' && q[1]=='i' && q[2]=='d' && q[3]=='"' && q[4]==':' && q[5]=='"'
                && memcmp(q+6, id, id_len)==0 && q[6+id_len]=='"')
            return true;
    }
    return false;
}

static inline int ow_type(const char *ow_s, const char *ow_e) {
    for (const char *q = ow_s; q + 7 <= ow_e; q++) {
        if (memcmp(q, "\"type\":", 7)==0) {
            const char *v = q + 7;
            while (v < ow_e && *v==' ') v++;
            if (v < ow_e && *v>='0' && *v<='9') {
                int t = *v - '0';
                if (v+1 >= ow_e || v[1]<'0' || v[1]>'9') return t;
            }
        }
    }
    return -1;
}

// Extract "key":"DIGITS" — Discord sends permission bitfields as strings.
static inline long long ow_bits(const char *ow_s, const char *ow_e, const char *key, size_t kl) {
    for (const char *q = ow_s; q + (ptrdiff_t)(kl + 4) <= ow_e; q++) {
        if (q[0]=='"' && memcmp(q+1, key, kl)==0 && q[kl+1]=='"' && q[kl+2]==':' && q[kl+3]=='"') {
            long long v = 0;
            const char *p = q + kl + 4;
            while (p < ow_e && *p>='0' && *p<='9') v = v*10 + (*p++ - '0');
            return v;
        }
    }
    return 0;
}

// Parse role ID list from /guilds/{id}/members/@me response — no cJSON.
static std::vector<std::string> parse_role_ids(const std::string &json) {
    std::vector<std::string> roles;
    const char *p = json.c_str(), *end = p + json.size();
    while (p + 9 <= end) {
        if (memcmp(p, "\"roles\":[", 9) == 0) {
            p += 9;
            while (p < end && *p != ']') {
                while (p < end && *p != '"' && *p != ']') p++;
                if (p >= end || *p == ']') break;
                const char *s = ++p;
                while (p < end && *p != '"') p++;
                if (p < end) { roles.emplace_back(s, p); p++; }
            }
            break;
        }
        p++;
    }
    return roles;
}

// Full Discord VIEW_CHANNEL resolution for a single channel object.
// Applies: @everyone overwrite → role overwrites (accumulated) → member overwrite.
// No heap allocations — uses only memcmp and integer arithmetic.
static bool user_can_view_channel(const char *obj, const char *obj_end,
                                   const std::string &guild_id,
                                   const std::string &user_id,
                                   const std::vector<std::string> &user_roles) {
    static const long long VIEW = 1024LL;

    // Locate "permission_overwrites":[
    const char po[] = "\"permission_overwrites\":";
    const size_t pol = sizeof(po) - 1;
    const char *p = obj;
    while (p + pol <= obj_end && memcmp(p, po, pol) != 0) p++;
    if (p + pol > obj_end) return true; // no overwrites → inherits default (visible)
    p += pol;
    while (p < obj_end && *p != '[') p++;
    if (p >= obj_end) return true;
    p++;

    long long ev_deny=0, ev_allow=0; // @everyone
    long long ro_deny=0, ro_allow=0; // role (accumulated)
    long long me_deny=0, me_allow=0; // member

    while (p < obj_end) {
        while (p<obj_end && (*p==' '||*p==','||*p=='\t'||*p=='\r'||*p=='\n')) p++;
        if (p>=obj_end || *p==']') break;
        if (*p!='{') break;

        const char *ow_s = p;
        int depth=0; bool in_s=false;
        while (p < obj_end) {
            char c=*p;
            if (in_s) {
                if (c=='\\') { p++; if (p<obj_end) p++; continue; }
                if (c=='"')  { in_s=false; p++; continue; }
            } else {
                if (c=='"')  { in_s=true;  p++; continue; }
                if (c=='{')  { depth++; p++; continue; }
                if (c=='}')  { depth--; p++; if (!depth) break; continue; }
            }
            p++;
        }
        const char *ow_e = p;
        int t = ow_type(ow_s, ow_e);

        if (t == 0) {
            if (ow_id_eq(ow_s, ow_e, guild_id.c_str(), guild_id.size())) {
                // @everyone role overwrite
                ev_deny  = ow_bits(ow_s, ow_e, "deny",  4);
                ev_allow = ow_bits(ow_s, ow_e, "allow", 5);
            } else {
                // Check if user has this role
                for (const auto &rid : user_roles) {
                    if (ow_id_eq(ow_s, ow_e, rid.c_str(), rid.size())) {
                        ro_deny  |= ow_bits(ow_s, ow_e, "deny",  4);
                        ro_allow |= ow_bits(ow_s, ow_e, "allow", 5);
                        break;
                    }
                }
            }
        } else if (t == 1 && !user_id.empty()) {
            if (ow_id_eq(ow_s, ow_e, user_id.c_str(), user_id.size())) {
                me_deny  = ow_bits(ow_s, ow_e, "deny",  4);
                me_allow = ow_bits(ow_s, ow_e, "allow", 5);
            }
        }
    }

    // Apply resolution: base → @everyone → roles → member
    long long eff = VIEW;
    eff &= ~ev_deny;  eff |= ev_allow;
    eff &= ~ro_deny;  eff |= ro_allow;
    eff &= ~me_deny;  eff |= me_allow;
    return (eff & VIEW) != 0;
}

// Parse an array of channel objects from a JSON string.
// Tries the whole array first; falls back to per-object parsing so one bad
// entry doesn't discard all channels.  Strips permission_overwrites before
// parsing (call strip_array_key before passing here if needed).
static std::vector<Channel> parse_channels_from_str(std::string &json,
                                                     const std::string &guild_id,
                                                     const std::string &user_id,
                                                     const std::vector<std::string> &user_roles) {
    // Sanitize: null bytes inside the string would truncate the C-string view
    for (char &c : json) if (c == '\0') c = ' ';

    // Release excess capacity: after stripping, json.size() may be much smaller
    // than json.capacity() (e.g. 68KB used, 164KB allocated).  Freeing the slack
    // gives the per-object parse more contiguous heap to work with.
    json.shrink_to_fit();

    // Per-object parse — never uses cJSON or any bulk malloc.
    // Running cJSON_Parse in the worker thread races with the main thread's
    // rendering mallocs on newlib's non-thread-safe heap; the per-object scanner
    // avoids this by using only stack variables and fixed-size string returns.
    std::vector<Channel> out;
    int total_objs = 0, parse_ok = 0;
    const char *p = json.c_str();
    const char *end = p + json.size();
    while (p < end && *p != '[') p++;
    if (p >= end) return out;
    p++; // skip '['
    WHBLogPrintf("Client: per-object scan starting, %zu bytes remaining", (size_t)(end - p));

    while (p < end) {
        // Skip whitespace and commas
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) p++;
        if (p >= end || *p != '{') break;
        total_objs++;

        // Find end of this object, tracking depth and strings
        const char *start = p;
        int depth = 0;
        bool in_str = false;
        while (p < end) {
            char c = *p;
            if (in_str) {
                if (c == '\\') { p++; if (p < end) p++; continue; }
                if (c == '"')  { in_str = false; p++; continue; }
            } else {
                if (c == '"')  { in_str = true;  p++; continue; }
                if (c == '{')  { depth++; p++; continue; }
                if (c == '}')  { depth--; p++; if (depth == 0) break; continue; }
            }
            p++;
        }
        if (depth != 0) break;

        // Diagnostic: log first object size and periodic progress.
        if (total_objs == 1) {
            WHBLogPrintf("Client: first channel obj: %zu bytes (of %zu total)",
                         (size_t)(p - start), (size_t)(end - json.c_str()));
        } else if (total_objs % 50 == 0) {
            WHBLogPrintf("Client: per-obj %d so far, %zu bytes left",
                         total_objs, (size_t)(end - p));
            OSSleepTicks(OSMillisecondsToTicks(1));
        }

        // Find the channel type with a raw byte scan.
        int ch_type = -1;
        for (const char *q = start; q + 6 < p && ch_type < 0; q++) {
            if (q[0] == '"' && q[1] == 't' && q[2] == 'y' && q[3] == 'p'
                            && q[4] == 'e' && q[5] == '"' && q[6] == ':') {
                const char *v = q + 7;
                int t = 0;
                while (v < p && *v >= '0' && *v <= '9') { t = t*10 + (*v++ - '0'); }
                ch_type = t;
            }
        }
        // Skip voice/category/stage/thread channels — only keep text-like types.
        if (ch_type != 0 && ch_type != 5 && ch_type != 15 && ch_type != 16) continue;

        // Skip channels the user cannot view (full role-aware permission check).
        if (!user_can_view_channel(start, p, guild_id, user_id, user_roles)) continue;

        // Extract fields without cJSON to avoid malloc in the worker thread.
        // Concurrent malloc from main thread + worker corrupts newlib's heap.
        Channel ch;
        ch.type      = (ChannelType)ch_type;
        ch.guild_id  = guild_id;
        ch.id        = json_str_field(start, p, "id");
        ch.name      = json_str_field(start, p, "name");
        ch.parent_id = json_str_field(start, p, "parent_id");
        ch.position  = json_int_field(start, p, "position");
        if (!ch.id.empty() && !ch.name.empty()) {
            parse_ok++;
            out.push_back(ch);
        }
    }
    WHBLogPrintf("Client: per-object parse: %d objs, %d parsed, %zu kept (text/forum)",
                 total_objs, parse_ok, out.size());
    return out;
}

// Parse DM / group-DM channels from /users/@me/channels response — no cJSON.
static std::vector<Channel> parse_dm_channels_from_str(const std::string &json) {
    std::vector<Channel> out;
    const char *p = json.c_str(), *end = p + json.size();
    while (p < end && *p != '[') p++;
    if (p >= end) return out;
    p++; // past '['

    while (p < end) {
        while (p < end && (*p==' '||*p=='\t'||*p=='\r'||*p=='\n'||*p==',')) p++;
        if (p >= end || *p != '{') break;
        const char *start = p;
        int depth = 0; bool in_str = false;
        while (p < end) {
            char c = *p;
            if (in_str) {
                if (c=='\\') { p++; if (p<end) p++; continue; }
                if (c=='"')  { in_str=false; p++; continue; }
            } else {
                if (c=='"')  { in_str=true;  p++; continue; }
                if (c=='{')  { depth++; p++; continue; }
                if (c=='}')  { depth--; p++; if (depth==0) break; continue; }
            }
            p++;
        }
        int type = json_int_field(start, p, "type", -1);
        if (type != 1 && type != 3) continue; // only DM and GROUP_DM

        Channel ch;
        ch.type = (ChannelType)type;
        ch.id   = json_str_field(start, p, "id");
        ch.name = json_str_field(start, p, "name"); // GROUP_DM may have explicit name

        if (ch.name.empty()) {
            // Extract first recipient — find "recipients":[ then scan the first {...}
            const char rn[] = "\"recipients\":[";
            const size_t rnl = sizeof(rn) - 1;
            const char *rp = start;
            while (rp + (ptrdiff_t)rnl <= p && memcmp(rp, rn, rnl) != 0) rp++;
            if (rp + (ptrdiff_t)rnl <= p) {
                rp += rnl;
                while (rp < p && (*rp==' '||*rp=='\t'||*rp=='\r'||*rp=='\n')) rp++;
                if (rp < p && *rp == '{') {
                    const char *rs = rp;
                    int rd = 0; bool ri = false;
                    while (rp < p) {
                        char c = *rp;
                        if (ri) {
                            if (c=='\\') { rp++; if (rp<p) rp++; continue; }
                            if (c=='"')  { ri=false; rp++; continue; }
                        } else {
                            if (c=='"')  { ri=true;  rp++; continue; }
                            if (c=='{')  { rd++; rp++; continue; }
                            if (c=='}')  { rd--; rp++; if (!rd) break; continue; }
                        }
                        rp++;
                    }
                    ch.name = json_str_field(rs, rp, "global_name");
                    if (ch.name.empty()) ch.name = json_str_field(rs, rp, "username");
                }
            }
        }

        if (!ch.id.empty() && !ch.name.empty())
            out.push_back(ch);
    }
    WHBLogPrintf("Client: parsed %zu DM channels", out.size());
    return out;
}

// ---- Client -----------------------------------------------------------------

Client::Client(const std::string &token) : token_(token) {}

Client::~Client() { shutdown(); }

bool Client::init() {
    rest_    = std::make_unique<RestClient>(token_);
    gateway_ = std::make_unique<Gateway>(token_, event_queue_);

    // Fetch current user
    std::string me_json = rest_->get("/users/@me");
    if (me_json.empty()) {
        WHBLogPrint("Client: failed to fetch /users/@me");
        return false;
    }
    cJSON *me = cJSON_Parse(me_json.c_str());
    if (me) {
        state_.self = parse_user(me);
        cJSON_Delete(me);
        WHBLogPrintf("Client: logged in as %s#%s",
                     state_.self.username.c_str(),
                     state_.self.discriminator.c_str());
    }

    if (!gateway_->connect()) {
        WHBLogPrint("Client: gateway connect failed");
        return false;
    }

    ch_worker_thread_ = std::thread(&Client::channel_worker, this);
    return true;
}

void Client::shutdown() {
    if (gateway_) gateway_->disconnect();
    ch_stop_.store(true);
    if (ch_worker_thread_.joinable()) ch_worker_thread_.join();
}

void Client::poll() {
    GatewayEvent ev;
    while (event_queue_.try_pop(ev)) {
        handle_event(ev);
    }
}

// ---- event dispatch ---------------------------------------------------------

void Client::handle_event(GatewayEvent &ev) {
    switch (ev.type) {
        case GatewayEventType::READY:
            handle_ready(ev.raw_json);
            break;
        case GatewayEventType::MESSAGE_CREATE:
            handle_message_create(ev.raw_json);
            break;
        case GatewayEventType::MESSAGE_UPDATE:
            handle_message_update(ev.raw_json);
            break;
        case GatewayEventType::MESSAGE_DELETE:
            handle_message_delete(ev.raw_json);
            break;
        case GatewayEventType::GUILD_CREATE:
            handle_guild_create(ev.raw_json);
            break;
        case GatewayEventType::CHANNEL_CREATE:
        case GatewayEventType::CHANNEL_UPDATE:
            handle_channel_create(ev.raw_json);
            break;
        case GatewayEventType::TYPING_START:
            handle_typing_start(ev.raw_json);
            break;
        case GatewayEventType::RECONNECT:
        case GatewayEventType::INVALID_SESSION:
            // Gateway handles reconnect internally; just log
            WHBLogPrintf("Client: gateway %s event",
                ev.type == GatewayEventType::RECONNECT ? "RECONNECT" : "INVALID_SESSION");
            break;
        default:
            break;
    }
}

// ---- event handlers ---------------------------------------------------------

void Client::handle_ready(const std::string &json) {
    // self was already fetched from /users/@me in init() before any threads
    // started — do NOT re-parse it here. Parsing the large READY JSON with
    // cJSON races with the gateway WebSocket thread's concurrent mallocs on
    // newlib's non-thread-safe heap. Use raw byte scanners instead.

    const char *p = json.c_str(), *end = p + json.size();

    // Locate "guilds":[
    const char gneedle[] = "\"guilds\":[";
    const size_t gnl = sizeof(gneedle) - 1;
    const char *gp = p;
    while (gp + (ptrdiff_t)gnl <= end && memcmp(gp, gneedle, gnl) != 0) gp++;

    std::vector<Guild> guilds;
    std::unordered_map<std::string, std::vector<Channel>> ch_cache;

    if (gp + (ptrdiff_t)gnl <= end) {
        gp += gnl; // past '['
        while (gp < end) {
            while (gp < end && (*gp==' '||*gp=='\t'||*gp=='\r'||*gp=='\n'||*gp==',')) gp++;
            if (gp >= end || *gp == ']') break;
            if (*gp != '{') break;

            // Find bounds of this guild object
            const char *gs = gp;
            int depth = 0; bool in_str = false;
            while (gp < end) {
                char c = *gp;
                if (in_str) {
                    if (c=='\\') { gp++; if (gp<end) gp++; continue; }
                    if (c=='"')  { in_str=false; gp++; continue; }
                } else {
                    if (c=='"')  { in_str=true;  gp++; continue; }
                    if (c=='{')  { depth++; gp++; continue; }
                    if (c=='}')  { depth--; gp++; if (!depth) break; continue; }
                }
                gp++;
            }
            const char *ge = gp;

            Guild guild;
            guild.id        = json_str_field(gs, ge, "id");
            guild.name      = json_str_field(gs, ge, "name");
            guild.available = !json_bool_field(gs, ge, "unavailable", false);
            if (guild.id.empty()) continue;
            guilds.push_back(guild);

            // Cache channels embedded in READY (user-token clients get full data here)
            const char chneedle[] = "\"channels\":[";
            const size_t chnl = sizeof(chneedle) - 1;
            const char *cp = gs;
            while (cp + (ptrdiff_t)chnl <= ge && memcmp(cp, chneedle, chnl) != 0) cp++;
            if (cp + (ptrdiff_t)chnl <= ge) {
                cp += chnl;
                std::vector<Channel> chans;
                while (cp < ge) {
                    while (cp<ge && (*cp==' '||*cp=='\t'||*cp=='\r'||*cp=='\n'||*cp==',')) cp++;
                    if (cp >= ge || *cp == ']') break;
                    if (*cp != '{') break;
                    const char *cs = cp;
                    int d2 = 0; bool is2 = false;
                    while (cp < ge) {
                        char c = *cp;
                        if (is2) {
                            if (c=='\\') { cp++; if (cp<ge) cp++; continue; }
                            if (c=='"')  { is2=false; cp++; continue; }
                        } else {
                            if (c=='"')  { is2=true;  cp++; continue; }
                            if (c=='{')  { d2++; cp++; continue; }
                            if (c=='}')  { d2--; cp++; if (!d2) break; continue; }
                        }
                        cp++;
                    }
                    int ch_type = json_int_field(cs, cp, "type", -1);
                    if (ch_type != 0 && ch_type != 5 && ch_type != 15 && ch_type != 16)
                        continue;
                    Channel ch;
                    ch.type      = (ChannelType)ch_type;
                    ch.guild_id  = guild.id;
                    ch.id        = json_str_field(cs, cp, "id");
                    ch.name      = json_str_field(cs, cp, "name");
                    ch.parent_id = json_str_field(cs, cp, "parent_id");
                    ch.position  = json_int_field(cs, cp, "position");
                    if (!ch.id.empty() && !ch.name.empty())
                        chans.push_back(ch);
                }
                if (!chans.empty()) {
                    std::sort(chans.begin(), chans.end(),
                        [](const Channel &a, const Channel &b){ return a.position < b.position; });
                    ch_cache[guild.id] = std::move(chans);
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!guilds.empty()) state_.guilds = std::move(guilds);
        for (auto &kv : ch_cache) guild_channels_cache_[kv.first] = std::move(kv.second);
        WHBLogPrintf("Client: READY — %zu guilds, %zu channel caches",
                     state_.guilds.size(), guild_channels_cache_.size());
    }

    state_.ready = true;
    if (on_ready) on_ready();
}

void Client::handle_message_create(const std::string &json) {
    const char *p = json.c_str(), *end = p + json.size();
    auto mentions = parse_mentions_from_raw(p, end);
    Message msg = parse_message_raw(p, end);
    apply_mention_replacements(msg.content, mentions);

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (msg.channel_id == state_.selected_channel_id) {
            state_.messages.push_back(msg);
            if (state_.messages.size() > 200)
                state_.messages.erase(state_.messages.begin());
        }
    }
    if (on_message_create) on_message_create(msg);
}

void Client::handle_message_update(const std::string &json) {
    const char *p = json.c_str(), *end = p + json.size();
    std::string id         = json_str_field(p, end, "id");
    std::string channel_id = json_str_field(p, end, "channel_id");
    std::string content    = json_str_field(p, end, "content");
    if (channel_id == state_.selected_channel_id) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto &msg : state_.messages) {
            if (msg.id == id) {
                if (!content.empty()) msg.content = content;
                msg.edited = true;
                break;
            }
        }
    }
}

void Client::handle_message_delete(const std::string &json) {
    const char *p = json.c_str(), *end = p + json.size();
    std::string id         = json_str_field(p, end, "id");
    std::string channel_id = json_str_field(p, end, "channel_id");
    if (channel_id == state_.selected_channel_id) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto &msgs = state_.messages;
        msgs.erase(std::remove_if(msgs.begin(), msgs.end(),
            [&](const Message &m) { return m.id == id; }), msgs.end());
    }
    if (on_message_delete) on_message_delete(channel_id, id);
}

void Client::handle_guild_create(const std::string &json) {
    const char *p = json.c_str(), *end = p + json.size();

    Guild guild;
    guild.id        = json_str_field(p, end, "id");
    guild.name      = json_str_field(p, end, "name");
    guild.available = !json_bool_field(p, end, "unavailable", false);
    if (guild.id.empty()) return;

    // Extract channels sub-array — same type filtering as READY handler
    std::vector<Channel> chans;
    const char chneedle[] = "\"channels\":[";
    const size_t chnl = sizeof(chneedle) - 1;
    const char *cp = p;
    while (cp + (ptrdiff_t)chnl <= end && memcmp(cp, chneedle, chnl) != 0) cp++;
    if (cp + (ptrdiff_t)chnl <= end) {
        cp += chnl;
        while (cp < end) {
            while (cp<end && (*cp==' '||*cp=='\t'||*cp=='\r'||*cp=='\n'||*cp==',')) cp++;
            if (cp >= end || *cp == ']') break;
            if (*cp != '{') break;
            const char *cs = cp;
            int depth = 0; bool in_str = false;
            while (cp < end) {
                char c = *cp;
                if (in_str) {
                    if (c=='\\') { cp++; if (cp<end) cp++; continue; }
                    if (c=='"')  { in_str=false; cp++; continue; }
                } else {
                    if (c=='"')  { in_str=true;  cp++; continue; }
                    if (c=='{')  { depth++; cp++; continue; }
                    if (c=='}')  { depth--; cp++; if (!depth) break; continue; }
                }
                cp++;
            }
            int ch_type = json_int_field(cs, cp, "type", -1);
            if (ch_type != 0 && ch_type != 5 && ch_type != 15 && ch_type != 16) continue;
            Channel ch;
            ch.type      = (ChannelType)ch_type;
            ch.guild_id  = guild.id;
            ch.id        = json_str_field(cs, cp, "id");
            ch.name      = json_str_field(cs, cp, "name");
            ch.parent_id = json_str_field(cs, cp, "parent_id");
            ch.position  = json_int_field(cs, cp, "position");
            if (!ch.id.empty() && !ch.name.empty())
                chans.push_back(ch);
        }
        std::sort(chans.begin(), chans.end(),
            [](const Channel &a, const Channel &b){ return a.position < b.position; });
        WHBLogPrintf("GW GUILD_CREATE %s: %zu text channels", guild.id.c_str(), chans.size());
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        bool found = false;
        for (auto &g : state_.guilds) {
            if (g.id == guild.id) { g = guild; found = true; break; }
        }
        if (!found) state_.guilds.push_back(guild);
        if (!chans.empty()) {
            guild_channels_cache_[guild.id] = chans;
            if (guild.id == state_.selected_guild_id)
                state_.channels = chans;
        }
    }

    if (on_guild_available) on_guild_available(guild);
}

void Client::handle_channel_create(const std::string &json) {
    const char *p = json.c_str(), *end = p + json.size();
    int ch_type = json_int_field(p, end, "type", -1);
    // Only track text-like channel types
    if (ch_type != 0 && ch_type != 5 && ch_type != 15 && ch_type != 16) return;

    Channel ch;
    ch.type      = (ChannelType)ch_type;
    ch.id        = json_str_field(p, end, "id");
    ch.guild_id  = json_str_field(p, end, "guild_id");
    ch.name      = json_str_field(p, end, "name");
    ch.topic     = json_str_field(p, end, "topic");
    ch.parent_id = json_str_field(p, end, "parent_id");
    ch.position  = json_int_field(p, end, "position");
    if (ch.id.empty()) return;

    if (ch.guild_id == state_.selected_guild_id) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        bool found = false;
        for (auto &c : state_.channels) {
            if (c.id == ch.id) { c = ch; found = true; break; }
        }
        if (!found) state_.channels.push_back(ch);
        if (on_channel_update) on_channel_update(ch);
    }
}

void Client::handle_typing_start(const std::string &json) {
    const char *p = json.c_str(), *end = p + json.size();
    TypingEvent ev;
    ev.channel_id = json_str_field(p, end, "channel_id");
    ev.user_id    = json_str_field(p, end, "user_id");
    ev.timestamp  = (uint64_t)json_int_field(p, end, "timestamp", 0);
    if (on_typing_start) on_typing_start(ev);
}

// ---- REST operations --------------------------------------------------------

bool Client::fetch_guilds() {
    std::string json = rest_->get("/users/@me/guilds?with_counts=true");
    if (json.empty()) return false;

    cJSON *arr = cJSON_Parse(json.c_str());
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return false; }

    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.guilds.clear();
    cJSON *g;
    cJSON_ArrayForEach(g, arr) {
        state_.guilds.push_back(parse_guild(g));
    }
    cJSON_Delete(arr);
    return true;
}

bool Client::fetch_channels(const std::string &guild_id) {
    // Use GUILD_CREATE cache if available (avoids REST round-trip, works for user tokens)
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = guild_channels_cache_.find(guild_id);
        if (it != guild_channels_cache_.end() && !it->second.empty()) {
            state_.selected_guild_id    = guild_id;
            state_.channels             = it->second;
            state_.selected_channel_id.clear();
            state_.messages.clear();
            WHBLogPrintf("Client: channels from cache for %s (%zu)",
                         guild_id.c_str(), it->second.size());
            return true;
        }
    }

    // Fallback: fetch via REST
    WHBLogPrintf("Client: fetching channels via REST for %s", guild_id.c_str());
    std::string json = rest_->get("/guilds/" + guild_id + "/channels");
    if (json.empty()) {
        WHBLogPrintf("Client: REST channels response empty (http %d)", rest_->last_http_code());
        return false;
    }

    size_t before = json.size();
    strip_array_key(json, "available_tags");
    strip_array_key(json, "applied_tags");
    WHBLogPrintf("Client: channels JSON %zu -> %zu bytes", before, json.size());

    std::string self_id;
    { std::lock_guard<std::mutex> lk(state_mutex_); self_id = state_.self.id; }
    std::vector<Channel> chans = parse_channels_from_str(json, guild_id, self_id, {});
    if (chans.empty()) {
        WHBLogPrintf("Client: no channels parsed for %s", guild_id.c_str());
        return false;
    }

    std::sort(chans.begin(), chans.end(),
        [](const Channel &a, const Channel &b){ return a.position < b.position; });

    WHBLogPrintf("Client: REST channels for %s: %zu", guild_id.c_str(), chans.size());

    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.selected_guild_id = guild_id;
    state_.channels = std::move(chans);
    state_.selected_channel_id.clear();
    state_.messages.clear();
    return true;
}

bool Client::fetch_messages(const std::string &channel_id, int limit) {
    char ep[128];
    snprintf(ep, sizeof(ep), "/channels/%s/messages?limit=%d",
             channel_id.c_str(), limit);
    std::string json = rest_->get(ep);
    if (json.empty()) return false;

    // Strip fields we don't render that can be very large (forum thread objects,
    // embeds, interactive components, reactions, mention user objects).
    strip_object_key(json, "thread");
    strip_object_key(json, "referenced_message");
    strip_array_key(json, "embeds");
    strip_array_key(json, "components");
    strip_array_key(json, "reactions");
    strip_array_key(json, "mentions");
    strip_array_key(json, "sticker_items");

    // Null-byte sanitization
    for (char &c : json) if (c == '\0') c = ' ';

    cJSON *arr = cJSON_Parse(json.c_str());
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return false; }

    std::vector<Message> msgs;
    cJSON *m;
    cJSON_ArrayForEach(m, arr) {
        msgs.push_back(parse_message(m));
    }
    cJSON_Delete(arr);

    // Discord returns newest first; reverse for chronological display
    std::reverse(msgs.begin(), msgs.end());

    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.selected_channel_id = channel_id;
    state_.messages = std::move(msgs);
    return true;
}

bool Client::send_message(const std::string &channel_id, const std::string &content) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "content", content.c_str());
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return false;

    std::string ep = "/channels/" + channel_id + "/messages";
    std::string resp = rest_->post(ep, json);
    cJSON_free(json);
    return rest_->last_http_code() == 200;
}

void Client::channel_worker() {
    RestClient bg_rest(token_);

    while (!ch_stop_.load()) {
        std::string guild_id, channel_id;
        int msg_limit = 50;
        bool do_dm = false;
        {
            std::lock_guard<std::mutex> lock(ch_work_mutex_);
            if (!ch_pending_guild_.empty()) {
                guild_id = std::move(ch_pending_guild_);
            } else if (!ch_pending_channel_.empty()) {
                channel_id        = std::move(ch_pending_channel_);
                msg_limit         = ch_pending_msg_limit_;
            } else if (ch_pending_dm_) {
                ch_pending_dm_ = false;
                do_dm = true;
            }
        }

        if (guild_id.empty() && channel_id.empty() && !do_dm) {
            OSSleepTicks(OSMillisecondsToTicks(50));
            continue;
        }

        if (do_dm) {
            // ---- DM channel list fetch ----
            WHBLogPrintf("Client worker: fetching DM list");
            std::string json;
            {
                std::lock_guard<std::mutex> net(g_http_mutex);
                json = bg_rest.get("/users/@me/channels");
            }
            auto chans = parse_dm_channels_from_str(json);
            {
                std::lock_guard<std::mutex> lock(ch_result_mutex_);
                ch_result_guild_id_ = "@me";
                ch_result_channels_ = std::move(chans);
                ch_result_ready_    = true;
            }
            channels_loading_.store(false);

        } else if (!guild_id.empty()) {
            // ---- channel list fetch ----
            WHBLogPrintf("Client worker: channels for %s", guild_id.c_str());

            // Fetch current user's role IDs in this guild for permission filtering.
            std::string user_id;
            std::vector<std::string> user_roles;
            {
                std::lock_guard<std::mutex> slk(state_mutex_);
                user_id = state_.self.id;
            }
            {
                std::lock_guard<std::mutex> net(g_http_mutex);
                std::string member_json = bg_rest.get("/guilds/" + guild_id + "/members/@me");
                if (!member_json.empty())
                    user_roles = parse_role_ids(member_json);
            }
            WHBLogPrintf("Client worker: user %s has %zu roles in %s",
                         user_id.c_str(), user_roles.size(), guild_id.c_str());

            std::string json;
            {
                std::lock_guard<std::mutex> net(g_http_mutex);
                json = bg_rest.get("/guilds/" + guild_id + "/channels");
            }
            if (json.empty()) {
                WHBLogPrintf("Client worker: empty channels response for %s", guild_id.c_str());
                channels_loading_.store(false);
                continue;
            }
            size_t before = json.size();
            strip_array_key(json, "available_tags");
            strip_array_key(json, "applied_tags");
            WHBLogPrintf("Client worker: %zu -> %zu bytes for %s", before, json.size(), guild_id.c_str());

            auto chans = parse_channels_from_str(json, guild_id, user_id, user_roles);
            std::sort(chans.begin(), chans.end(),
                [](const Channel &a, const Channel &b){ return a.position < b.position; });
            WHBLogPrintf("Client worker: %zu channels for %s", chans.size(), guild_id.c_str());
            if (!chans.empty()) save_channel_cache(guild_id, chans);

            {
                std::lock_guard<std::mutex> lock(ch_result_mutex_);
                ch_result_guild_id_ = guild_id;
                ch_result_channels_ = std::move(chans);
                ch_result_ready_    = true;
            }
            channels_loading_.store(false);

        } else {
            // ---- message fetch ----
            WHBLogPrintf("Client worker: messages for %s (limit %d)", channel_id.c_str(), msg_limit);
            char ep[192];
            snprintf(ep, sizeof(ep), "/channels/%s/messages?limit=%d",
                     channel_id.c_str(), msg_limit);
            std::string json;
            {
                std::lock_guard<std::mutex> net(g_http_mutex);
                json = bg_rest.get(ep);
            }
            int http_code = bg_rest.last_http_code();
            if (json.empty()) {
                WHBLogPrintf("Client worker: empty messages response for %s (HTTP %d)",
                             channel_id.c_str(), http_code);
                msg_loading_.store(false);
                continue;
            }

            // Collect mention display names BEFORE stripping the array — the
            // raw scanner reads the full user objects inside "mentions":[...].
            auto mentions = parse_mentions_from_raw(json.c_str(), json.c_str() + json.size());

            // Strip before staging — string ops only, no cJSON/malloc here.
            strip_object_key(json, "thread");
            strip_object_key(json, "referenced_message");
            strip_object_key(json, "interaction_metadata");
            strip_object_key(json, "call");
            strip_object_key(json, "application");
            strip_object_key(json, "poll");
            strip_array_key(json, "embeds");
            strip_array_key(json, "components");
            strip_array_key(json, "reactions");
            strip_array_key(json, "mentions");
            strip_array_key(json, "sticker_items");
            strip_array_key(json, "message_snapshots");
            strip_array_key(json, "role_subscription_data");
            strip_string_key(json, "waveform");
            strip_string_key(json, "placeholder");
            strip_string_key(json, "proxy_url");
            for (char &c : json) if (c == '\0') c = ' ';

            // Stage raw JSON and mention map for the main thread to parse.
            {
                std::lock_guard<std::mutex> lock(msg_result_mutex_);
                msg_result_channel_id_ = channel_id;
                msg_result_raw_json_   = std::move(json);
                msg_result_http_code_  = http_code;
                msg_result_mentions_   = std::move(mentions);
                msg_result_ready_      = true;
            }
            msg_loading_.store(false);
        }
    }
}

bool Client::start_channel_load(const std::string &guild_id) {
    // Check cache first — instant path, worker thread not involved
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = guild_channels_cache_.find(guild_id);
        if (it != guild_channels_cache_.end() && !it->second.empty()) {
            state_.selected_guild_id    = guild_id;
            state_.channels             = it->second;
            state_.selected_channel_id.clear();
            state_.messages.clear();
            WHBLogPrintf("Client: channels from cache for %s (%zu)",
                         guild_id.c_str(), it->second.size());
            return true;
        }
        state_.channels.clear();
        state_.selected_guild_id    = guild_id;
        state_.selected_channel_id.clear();
        state_.messages.clear();
    }

    // Check disk cache before queuing a REST fetch
    {
        std::vector<Channel> disk_chans;
        if (load_channel_cache(guild_id, disk_chans) && !disk_chans.empty()) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_.selected_guild_id    = guild_id;
            state_.channels             = disk_chans;
            state_.selected_channel_id.clear();
            state_.messages.clear();
            guild_channels_cache_[guild_id] = std::move(disk_chans);
            return true;
        }
    }

    // Queue work for the persistent worker thread — never blocks the main thread
    {
        std::lock_guard<std::mutex> lock(ch_result_mutex_);
        ch_result_ready_ = false;
    }
    channels_loading_.store(true);
    {
        std::lock_guard<std::mutex> lock(ch_work_mutex_);
        ch_pending_guild_ = guild_id;   // latest request wins
    }
    return false;
}

bool Client::drain_channel_result() {
    std::string        guild_id;
    std::vector<Channel> chans;
    {
        std::lock_guard<std::mutex> lock(ch_result_mutex_);
        if (!ch_result_ready_) return false;
        ch_result_ready_ = false;
        guild_id = ch_result_guild_id_;
        chans    = std::move(ch_result_channels_);
    }
    {
        std::lock_guard<std::mutex> slock(state_mutex_);
        // Only apply if the user hasn't navigated away to a different guild
        if (guild_id == state_.selected_guild_id) {
            guild_channels_cache_[guild_id] = chans;
            state_.channels = std::move(chans);
            state_.selected_channel_id.clear();
        }
    }
    return true;
}

void Client::request_messages(const std::string &channel_id, int limit) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.selected_channel_id = channel_id;
        state_.messages.clear();
    }
    msg_loading_.store(true);
    {
        std::lock_guard<std::mutex> lock(ch_work_mutex_);
        ch_pending_channel_   = channel_id;
        ch_pending_msg_limit_ = limit;
    }
}

bool Client::drain_message_result() {
    std::string channel_id, raw_json;
    int http_code = 0;
    std::vector<std::pair<std::string,std::string>> mentions;
    {
        std::lock_guard<std::mutex> lock(msg_result_mutex_);
        if (!msg_result_ready_) return false;
        msg_result_ready_     = false;
        channel_id            = msg_result_channel_id_;
        raw_json              = std::move(msg_result_raw_json_);
        http_code             = msg_result_http_code_;
        mentions              = std::move(msg_result_mentions_);
        msg_result_http_code_ = 0;
    }

    // 403 = user can't access this channel. Remove from memory and re-save the
    // disk cache without it, so the next launch loads a clean list immediately.
    if (http_code == 403) {
        WHBLogPrintf("Client: channel %s inaccessible (403), removing", channel_id.c_str());
        std::string guild_id;
        std::vector<Channel> updated;
        {
            std::lock_guard<std::mutex> slock(state_mutex_);
            auto rm = [&](std::vector<Channel> &v) {
                v.erase(std::remove_if(v.begin(), v.end(),
                        [&](const Channel &c){ return c.id == channel_id; }), v.end());
            };
            rm(state_.channels);
            guild_id = state_.selected_guild_id;
            auto it = guild_channels_cache_.find(guild_id);
            if (it != guild_channels_cache_.end()) {
                rm(it->second);
                updated = it->second;
            }
        }
        if (!guild_id.empty()) save_channel_cache(guild_id, updated);
        return true;
    }

    // Zero-allocation per-object message parse.
    // cJSON_Parse on the main thread races with the gateway thread's WebSocket
    // malloc calls on newlib's non-thread-safe heap. parse_message_raw avoids
    // all bulk allocations: it only constructs the output std::string fields.
    std::vector<Message> msgs;
    {
        const char *p   = raw_json.c_str();
        const char *end = p + raw_json.size();
        while (p < end && *p != '[') p++;
        if (p < end) p++;
        while (p < end) {
            while (p < end && (*p==' '||*p=='\t'||*p=='\r'||*p=='\n'||*p==',')) p++;
            if (p >= end || *p != '{') break;
            const char *start = p;
            int depth = 0; bool in_str = false;
            while (p < end) {
                char c = *p;
                if (in_str) {
                    if (c=='\\') { p++; if (p<end) p++; continue; }
                    if (c=='"')  { in_str=false; p++; continue; }
                } else {
                    if (c=='"')  { in_str=true;  p++; continue; }
                    if (c=='{')  { depth++; p++; continue; }
                    if (c=='}')  { depth--; p++; if (depth==0) break; continue; }
                }
                p++;
            }
            msgs.push_back(parse_message_raw(start, p));
        }
    }
    std::reverse(msgs.begin(), msgs.end());

    if (!mentions.empty()) {
        for (auto &m : msgs)
            apply_mention_replacements(m.content, mentions);
    }

    {
        std::lock_guard<std::mutex> slock(state_mutex_);
        if (channel_id == state_.selected_channel_id)
            state_.messages = std::move(msgs);
    }
    return true;
}

bool Client::start_dm_load() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.selected_guild_id = "@me";
        state_.channels.clear();
        state_.selected_channel_id.clear();
        state_.messages.clear();
    }
    {
        std::lock_guard<std::mutex> lock(ch_result_mutex_);
        ch_result_ready_ = false;
    }
    channels_loading_.store(true);
    {
        std::lock_guard<std::mutex> lock(ch_work_mutex_);
        ch_pending_dm_ = true;
    }
    return false;
}

// ---- parsers (file-local) ---------------------------------------------------

static User parse_user(cJSON *j) {
    User u;
    u.id            = safe_str(j, "id");
    u.username      = safe_str(j, "username");
    u.discriminator = safe_str(j, "discriminator");
    u.global_name   = safe_str(j, "global_name");
    u.avatar        = safe_str(j, "avatar");
    u.bot           = cJSON_IsTrue(cJSON_GetObjectItem(j, "bot"));
    return u;
}

static Guild parse_guild(cJSON *j) {
    Guild g;
    g.id        = safe_str(j, "id");
    g.name      = safe_str(j, "name");
    g.icon      = safe_str(j, "icon");
    g.available = !cJSON_IsTrue(cJSON_GetObjectItem(j, "unavailable"));
    return g;
}


static Message parse_message(cJSON *j) {
    Message m;
    m.id         = safe_str(j, "id");
    m.channel_id = safe_str(j, "channel_id");
    m.guild_id   = safe_str(j, "guild_id");
    m.content    = safe_str(j, "content");
    m.timestamp  = safe_str(j, "timestamp");
    m.pinned     = cJSON_IsTrue(cJSON_GetObjectItem(j, "pinned"));

    cJSON *author = cJSON_GetObjectItem(j, "author");
    if (author) m.author = parse_user(author);

    cJSON *edit_ts = cJSON_GetObjectItem(j, "edited_timestamp");
    m.edited = edit_ts && !cJSON_IsNull(edit_ts) && edit_ts->valuestring;

    cJSON *attachments = cJSON_GetObjectItem(j, "attachments");
    if (attachments && cJSON_IsArray(attachments)) {
        cJSON *a;
        cJSON_ArrayForEach(a, attachments) {
            Attachment att;
            att.id           = safe_str(a, "id");
            att.filename     = safe_str(a, "filename");
            att.content_type = safe_str(a, "content_type");
            att.url          = safe_str(a, "url");
            att.size         = safe_int(a, "size");
            m.attachments.push_back(att);
        }
    }

    return m;
}

} // namespace Discord
