#include "app.h"
#include "../discord/net_mutex.h"
#include <whb/proc.h>
#include <whb/log.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <coreinit/memory.h>
#include <curl/curl.h>
#include <png.h>
#include <malloc.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <vector>
#include <sys/stat.h>
#include <qrcodegen/qrcodegen.h>

// Font search paths (SD card then romfs)
static const char *FONT_PATHS[] = {
    "/vol/external01/wiiu/discord_wiiu/font.ttf",
    "/vol/external01/wiiu/font.ttf",
    "romfs:/font.ttf",
    nullptr
};

// ---- Avatar PNG helpers (file-scope) ----------------------------------------

static size_t avatar_dl_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    auto *s = static_cast<std::string *>(ud);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

struct PngMemSrc { const uint8_t *buf; size_t pos, len; };

static void png_read_mem_cb(png_structp p, png_bytep out, png_size_t n) {
    auto *m = static_cast<PngMemSrc *>(png_get_io_ptr(p));
    if (m->pos + n > m->len) { png_error(p, "eof"); return; }
    memcpy(out, m->buf + m->pos, n);
    m->pos += n;
}

static const char *MEDIA_CACHE_DIR  = "/vol/external01/wiiu/discord_wiiu/media";
static const int   MEDIA_MAX_W      = 200;
static const int   MEDIA_MAX_H      = 90;
static const int   MEDIA_MAX_BYTES  = 1024 * 1024;  // skip inline for attachments > 1 MB

static std::string infer_content_type(const std::string &filename) {
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos) return {};
    std::string ext = filename.substr(dot + 1);
    for (char &c : ext) c = (char)tolower((unsigned char)c);
    if (ext == "png")              return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")              return "image/gif";
    if (ext == "webp")             return "image/webp";
    if (ext == "mp4" || ext == "m4v") return "video/mp4";
    if (ext == "mov")              return "video/quicktime";
    if (ext == "webm")             return "video/webm";
    if (ext == "mkv")              return "video/x-matroska";
    if (ext == "mp3")              return "audio/mpeg";
    if (ext == "ogg")              return "audio/ogg";
    if (ext == "wav")              return "audio/wav";
    if (ext == "flac")             return "audio/flac";
    if (ext == "m4a")              return "audio/mp4";
    return {};
}

static std::string format_bytes(int bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%d B", bytes);
    return buf;
}

// Decode a PNG buffer to an RGBA SDL_Surface.
// circle_mask=true applies a circular alpha mask (for avatars).
static SDL_Surface *decode_png(const void *data, size_t size, bool circle_mask) {
    if (size < 8 || png_sig_cmp(static_cast<const png_byte *>(data), 0, 8) != 0)
        return nullptr;

    PngMemSrc ms = { static_cast<const uint8_t *>(data), 8, size };
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             nullptr, nullptr, nullptr);
    if (!png) return nullptr;

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, nullptr, nullptr); return nullptr; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        return nullptr;
    }

    png_set_read_fn(png, &ms, png_read_mem_cb);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    int w  = (int)png_get_image_width(png, info);
    int h  = (int)png_get_image_height(png, info);
    png_byte ct = png_get_color_type(png, info);
    png_byte bd = png_get_bit_depth(png, info);

    // Normalise to 8-bit RGBA
    if (bd == 16)                             png_set_strip_16(png);
    if (ct == PNG_COLOR_TYPE_PALETTE)         png_set_palette_to_rgb(png);
    if (ct == PNG_COLOR_TYPE_GRAY && bd < 8)  png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (!(ct & PNG_COLOR_MASK_ALPHA))         png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    // Wii U is big-endian PPC: SDL_PIXELFORMAT_RGBA32 == SDL_PIXELFORMAT_RGBA8888
    // Byte layout in memory: R G B A — matches libpng RGBA output.
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                       SDL_PIXELFORMAT_RGBA32);
    if (!surf) { png_destroy_read_struct(&png, &info, nullptr); return nullptr; }

    SDL_LockSurface(surf);
    std::vector<png_bytep> rows(h);
    for (int i = 0; i < h; i++)
        rows[i] = static_cast<png_bytep>(static_cast<uint8_t *>(surf->pixels)
                                         + i * surf->pitch);
    png_read_image(png, rows.data());

    if (circle_mask) {
        int cx = w / 2, cy = h / 2, r2 = std::min(cx, cy);
        r2 = r2 * r2;
        for (int py = 0; py < h; py++) {
            uint8_t *row = static_cast<uint8_t *>(surf->pixels) + py * surf->pitch;
            for (int px = 0; px < w; px++) {
                int dx = px - cx, dy = py - cy;
                if (dx*dx + dy*dy > r2) row[px * 4 + 3] = 0;
            }
        }
    }
    SDL_UnlockSurface(surf);

    png_destroy_read_struct(&png, &info, nullptr);
    return surf;
}

static SDL_Surface *decode_png_circle(const void *data, size_t size) {
    return decode_png(data, size, true);
}
static SDL_Surface *decode_png_rect(const void *data, size_t size) {
    return decode_png(data, size, false);
}

namespace UI {

// ---- ctor/dtor ---------------------------------------------------------------

App::App(std::shared_ptr<Discord::Client> client) : client_(client) {
    if (client_) wire_callbacks();
}

void App::wire_callbacks() {
    client_->on_ready = [this]() {
        ready_ = true;
        state_ = AppState::GUILD_LIST;
        client_->fetch_guilds();
    };
    client_->on_message_create = [this](const Discord::Message &) {
        if (msg_scroll_ == 0) { /* already at bottom */ }
    };
    client_->on_typing_start = [this](const Discord::TypingEvent &ev) {
        if (ev.channel_id == client_->state().selected_channel_id)
            typing_users_[ev.user_id] = SDL_GetTicks() + 8000;
    };
}

void App::save_token_to_disk(const std::string &token) {
    static const char *paths[] = {
        "/vol/external01/wiiu/discord_token.txt",
        "/vol/external01/discord_token.txt",
        nullptr,
    };
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "w");
        if (!f) continue;
        fputs(token.c_str(), f);
        fclose(f);
        WHBLogPrintf("Token saved to %s", paths[i]);
        return;
    }
    WHBLogPrint("WARNING: could not save token to SD card");
}

App::~App() { teardown(); }

// ---- SDL setup ---------------------------------------------------------------

bool App::setup_sdl() {
    window_ = SDL_CreateWindow(
        "Discord Wii U",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W, SCREEN_H,
        SDL_WINDOW_SHOWN);
    if (!window_) {
        WHBLogPrintf("SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        WHBLogPrintf("SDL_CreateRenderer: %s", SDL_GetError());
        return false;
    }

    SDL_RenderSetLogicalSize(renderer_, L_W, L_H);
    draw_.init(renderer_);
    return true;
}

bool App::load_fonts(const char *path) {
    draw_.font_sm   = TTF_OpenFont(path, FONT_SIZE_SM);
    draw_.font_md   = TTF_OpenFont(path, FONT_SIZE_MD);
    draw_.font_lg   = TTF_OpenFont(path, FONT_SIZE_LG);
    draw_.font_bold = TTF_OpenFont(path, FONT_SIZE_MD);
    if (draw_.font_bold) TTF_SetFontStyle(draw_.font_bold, TTF_STYLE_BOLD);
    return draw_.font_md != nullptr;
}

void App::teardown() {
    // Stop login worker if still running
    if (remote_auth_) {
        remote_auth_->stop();
        remote_auth_.reset();
    }

    // Stop avatar worker first so it doesn't touch renderer during cleanup
    avatar_stop_.store(true);
    if (avatar_thread_.joinable()) avatar_thread_.join();

    // Free any surfaces that completed but weren't drained yet
    {
        std::lock_guard<std::mutex> lock(avatar_mutex_);
        while (!avatar_done_.empty()) {
            auto [id, surf] = avatar_done_.front();
            avatar_done_.pop();
            if (surf) SDL_FreeSurface(surf);
        }
        while (!media_done_.empty()) {
            auto [id, surf] = media_done_.front();
            media_done_.pop();
            if (surf) SDL_FreeSurface(surf);
        }
    }
    for (auto &[id, tex] : avatar_cache_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    avatar_cache_.clear();
    for (auto &[id, tex] : media_cache_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    media_cache_.clear();

    draw_.destroy();
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);     window_   = nullptr; }
}

// ---- main run loop -----------------------------------------------------------

void App::run() {
    if (!setup_sdl()) return;

    // Start avatar worker only if we already have a Discord client
    if (client_) {
        avatar_stop_.store(false);
        avatar_thread_ = std::thread(&App::avatar_worker, this);
    }

    // If no client provided, start Remote Auth login flow
    if (!client_) {
        state_ = AppState::LOGIN;
        remote_auth_ = std::make_unique<Discord::RemoteAuth>();
        remote_auth_->start();
        WHBLogPrint("App: starting Remote Auth login");
    }

    // Try each font path
    bool fonts_ok = false;
    for (int i = 0; FONT_PATHS[i]; i++) {
        if (load_fonts(FONT_PATHS[i])) { fonts_ok = true; break; }
    }
    if (!fonts_ok) {
        WHBLogPrint("No font found. Place font.ttf at /vol/external01/wiiu/discord_wiiu/font.ttf");
        state_ = AppState::ERROR;
        error_msg_ = "Missing font.ttf\nPlace font at:\n/vol/external01/wiiu/discord_wiiu/font.ttf";
    }

    SDL_StopTextInput();  // ensure keyboard is hidden at startup
    last_frame_ = SDL_GetTicks();

    while (WHBProcIsRunning()) {
        update();
        render();

        // Frame rate cap (~60 fps)
        Uint32 now = SDL_GetTicks();
        int elapsed = (int)(now - last_frame_);
        if (elapsed < 16) SDL_Delay(16 - elapsed);
        last_frame_ = SDL_GetTicks();
    }

    teardown();
}

// ---- update ------------------------------------------------------------------

// Total MEM2 available to this app (set once at first call via OSGetMemBound).
// Tells us "system memory" (~256MB) vs "game memory" (~1.5 GB).
static uint32_t mem2_total_kb() {
    static uint32_t kb = 0;
    if (!kb) {
        uint32_t addr = 0, size = 0;
        OSGetMemBound(OS_MEM2, &addr, &size);
        kb = size / 1024;
    }
    return kb;
}

static void log_memory_stats() {
    struct mallinfo mi = mallinfo();
    uint32_t total_kb = mem2_total_kb();
    // arena = pages committed to C heap from MEM2; free ≈ total − arena
    WHBLogPrintf("MEM: used=%uKB arena=%uKB | MEM2 total=%uKB free~%uKB",
                 (unsigned)mi.uordblks / 1024,
                 (unsigned)mi.arena    / 1024,
                 total_kb,
                 total_kb > (unsigned)mi.arena / 1024
                     ? total_kb - (unsigned)mi.arena / 1024 : 0);
}

void App::update() {
    // Handle Remote Auth login completion
    if (state_ == AppState::LOGIN && remote_auth_) {
        auto ra = remote_auth_->state();
        if (ra == Discord::RemoteAuthState::DONE) {
            std::string tok = remote_auth_->token();
            remote_auth_->stop();
            remote_auth_.reset();

            save_token_to_disk(tok);

            client_ = std::make_shared<Discord::Client>(tok);
            wire_callbacks();

            // Start avatar worker now that we have a client
            avatar_stop_.store(false);
            avatar_thread_ = std::thread(&App::avatar_worker, this);

            if (!client_->init()) {
                error_msg_ = "Discord init failed after login";
                state_ = AppState::ERROR;
            } else {
                state_ = AppState::LOADING;
            }
        }
        // In login state: only handle SDL events (for quit), skip Discord polls
        handle_sdl_events();
        return;
    }

    if (client_) {
        // Drain Discord events
        client_->poll();

        // Apply background channel/message fetch results when ready
        client_->drain_channel_result();
        client_->drain_message_result();
    }

    // Promote completed avatar/media downloads to GPU textures
    drain_avatar_results();
    drain_media_results();

    // Log memory stats every 5 seconds
    {
        static Uint32 mem_log_timer = 0;
        Uint32 now = SDL_GetTicks();
        if (now - mem_log_timer >= 5000) {
            log_memory_stats();
            mem_log_timer = now;
        }
    }

    handle_sdl_events();
    handle_vpad();

    // Clean up expired typing indicators
    Uint32 now = SDL_GetTicks();
    for (auto it = typing_users_.begin(); it != typing_users_.end(); ) {
        if (now > it->second) it = typing_users_.erase(it);
        else ++it;
    }

    // Cursor blink
    if (now - cursor_tick_ > 500) {
        input_cursor_blink_ = !input_cursor_blink_;
        cursor_tick_ = now;
    }
}

// ---- SDL event handling ------------------------------------------------------

// Map SDL keycode + modifier to printable ASCII character.
// Used as fallback when SDL_TEXTINPUT events are not generated (Wii U port).
static char keycode_to_char(SDL_Keycode k, SDL_Keymod mod) {
    bool shift = (mod & KMOD_SHIFT) != 0;
    bool caps  = (mod & KMOD_CAPS)  != 0;
    bool upper = shift ^ caps;
    if (k >= SDLK_a && k <= SDLK_z)
        return upper ? (char)('A' + (k - SDLK_a)) : (char)('a' + (k - SDLK_a));
    if (!shift) {
        if (k >= SDLK_0 && k <= SDLK_9) return (char)k;
        switch (k) {
            case SDLK_SPACE:        return ' ';
            case SDLK_MINUS:        return '-';
            case SDLK_EQUALS:       return '=';
            case SDLK_LEFTBRACKET:  return '[';
            case SDLK_RIGHTBRACKET: return ']';
            case SDLK_BACKSLASH:    return '\\';
            case SDLK_SEMICOLON:    return ';';
            case SDLK_QUOTE:        return '\'';
            case SDLK_COMMA:        return ',';
            case SDLK_PERIOD:       return '.';
            case SDLK_SLASH:        return '/';
            case SDLK_BACKQUOTE:    return '`';
            default: break;
        }
    } else {
        switch (k) {
            case SDLK_1: return '!'; case SDLK_2: return '@';
            case SDLK_3: return '#'; case SDLK_4: return '$';
            case SDLK_5: return '%'; case SDLK_6: return '^';
            case SDLK_7: return '&'; case SDLK_8: return '*';
            case SDLK_9: return '('; case SDLK_0: return ')';
            case SDLK_MINUS:        return '_';
            case SDLK_EQUALS:       return '+';
            case SDLK_LEFTBRACKET:  return '{';
            case SDLK_RIGHTBRACKET: return '}';
            case SDLK_BACKSLASH:    return '|';
            case SDLK_SEMICOLON:    return ':';
            case SDLK_QUOTE:        return '"';
            case SDLK_COMMA:        return '<';
            case SDLK_PERIOD:       return '>';
            case SDLK_SLASH:        return '?';
            case SDLK_BACKQUOTE:    return '~';
            default: break;
        }
    }
    return 0;
}

void App::handle_sdl_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            WHBProcStopRunning();
            return;
        }

        if (ev.type == SDL_KEYDOWN) {
            SDL_Keycode k  = ev.key.keysym.sym;
            SDL_Keymod  md = (SDL_Keymod)ev.key.keysym.mod;
            if (state_ == AppState::TYPING) {
                if (k == SDLK_BACKSPACE) {
                    if (!input_text_.empty()) {
                        size_t len = input_text_.size();
                        while (len > 0 && (input_text_[len-1] & 0xC0) == 0x80) len--;
                        if (len > 0) len--;
                        input_text_.resize(len);
                    }
                } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    send_current_input();
                } else if (k == SDLK_ESCAPE) {
                    state_ = AppState::CHAT;
                    input_text_.clear();
                } else {
                    char c = keycode_to_char(k, md);
                    if (c) input_text_ += c;
                }
            } else {
                if (k == SDLK_UP)       scroll_messages(1);
                if (k == SDLK_DOWN)     scroll_messages(-1);
                if (k == SDLK_PAGEUP)   scroll_messages(10);
                if (k == SDLK_PAGEDOWN) scroll_messages(-10);
                if (k == SDLK_RETURN && state_ == AppState::CHAT) {
                    state_ = AppState::TYPING;
                    input_text_.clear();
                }
            }
        }
    }
}

// ---- VPAD (GamePad) input ----------------------------------------------------

static VPADStatus vpad_prev;
static bool vpad_first = true;

void App::handle_vpad() {
    if (state_ == AppState::LOGIN) return;

    VPADStatus vpad;
    VPADReadError err = VPAD_READ_SUCCESS;
    VPADRead(VPAD_CHAN_0, &vpad, 1, &err);
    if (err != VPAD_READ_SUCCESS && err != VPAD_READ_NO_SAMPLES) return;

    if (vpad_first) { vpad_prev = vpad; vpad_first = false; return; }

    uint32_t pressed = vpad.trigger;  // buttons just pressed this frame

    if (state_ == AppState::TYPING) {
        if (pressed & VPAD_BUTTON_B) {
            state_ = AppState::CHAT;
            input_text_.clear();
        }
        if (pressed & VPAD_BUTTON_PLUS) {
            send_current_input();
        }

        // GamePad touchscreen → virtual keyboard (detect press edge, not hold)
        // Use VPADGetTPCalibratedPointEx so coordinates are in 1280×720 space
        VPADTouchData tp;
        VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_1280X720, &tp, &vpad.tpFiltered1);
        bool touching = tp.touched != 0 && tp.validity == VPAD_VALID;
        if (touching && !vpad_touching_) {
            handle_keyboard_touch((int)tp.x / SCALE, (int)tp.y / SCALE);
        }
        vpad_touching_ = touching;

        vpad_prev = vpad;
        return;
    }
    vpad_touching_ = false;

    if (!client_) return;

    // -- Navigation in GUILD_LIST / CHANNEL_LIST / CHAT --
    const auto &guilds   = client_->state().guilds;
    const auto &channels = client_->state().channels;

    if (pressed & VPAD_BUTTON_UP) {
        if (state_ == AppState::GUILD_LIST) {
            guild_sel_ = std::max(0, guild_sel_ - 1);
        } else if (state_ == AppState::CHANNEL_LIST) {
            channel_sel_ = std::max(0, channel_sel_ - 1);
        } else if (state_ == AppState::CHAT) {
            scroll_messages(3);
        }
    }
    if (pressed & VPAD_BUTTON_DOWN) {
        if (state_ == AppState::GUILD_LIST) {
            guild_sel_ = std::min((int)guilds.size() - 1, guild_sel_ + 1);
        } else if (state_ == AppState::CHANNEL_LIST) {
            channel_sel_ = std::min((int)channels.size() - 1, channel_sel_ + 1);
        } else if (state_ == AppState::CHAT) {
            scroll_messages(-3);
        }
    }

    if (pressed & VPAD_BUTTON_A) {
        if (state_ == AppState::GUILD_LIST && !guilds.empty()) {
            select_guild(guild_sel_);
        } else if (state_ == AppState::CHANNEL_LIST && !channels.empty()) {
            select_channel(channel_sel_);
        }
    }

    if (pressed & VPAD_BUTTON_B) {
        if (state_ == AppState::CHAT) {
            state_ = AppState::CHANNEL_LIST;
            channel_sel_ = 0;
        } else if (state_ == AppState::CHANNEL_LIST) {
            state_ = AppState::GUILD_LIST;
        }
    }

    if (pressed & VPAD_BUTTON_Y && state_ == AppState::CHAT) {
        state_ = AppState::TYPING;
        input_text_.clear();
        kb_caps_ = false;
    }

    // ZL / ZR fast scroll
    if (vpad.hold & VPAD_BUTTON_ZL) scroll_messages(1);
    if (vpad.hold & VPAD_BUTTON_ZR) scroll_messages(-1);

    // Left/Right to switch panels
    if (pressed & VPAD_BUTTON_LEFT) {
        if (state_ == AppState::CHAT || state_ == AppState::CHANNEL_LIST)
            state_ = AppState::GUILD_LIST;
    }
    if (pressed & VPAD_BUTTON_RIGHT) {
        if (state_ == AppState::GUILD_LIST && !guilds.empty())
            state_ = AppState::CHANNEL_LIST;
    }

    // MINUS opens DM list
    if (pressed & VPAD_BUTTON_MINUS) {
        open_dm_list();
    }

    vpad_prev = vpad;
}

// ---- navigation actions ------------------------------------------------------

void App::select_guild(int index) {
    const auto &guilds = client_->state().guilds;
    if (index < 0 || index >= (int)guilds.size()) return;

    state_ = AppState::CHANNEL_LIST;
    channel_sel_ = 0;
    msg_scroll_ = 0;
    // start_channel_load sets selected_guild_id and clears channels internally
    client_->start_channel_load(guilds[index].id);
}

void App::select_channel(int index) {
    const auto &channels = client_->state().channels;
    if (index < 0 || index >= (int)channels.size()) return;

    // Free attachment image textures from the previous channel to reclaim GPU memory.
    for (auto &[id, tex] : media_cache_) { if (tex) SDL_DestroyTexture(tex); }
    media_cache_.clear();

    state_ = AppState::CHAT;
    msg_scroll_ = 0;
    bool is_heavy = (channels[index].type == Discord::ChannelType::GUILD_FORUM ||
                     channels[index].type == Discord::ChannelType::GUILD_MEDIA);
    client_->request_messages(channels[index].id, is_heavy ? 50 : 100);
}

void App::open_dm_list() {
    state_ = AppState::CHANNEL_LIST;
    channel_sel_ = 0;
    client_->start_dm_load();
}

void App::scroll_messages(int delta) {
    const auto &msgs = client_->state().messages;
    if (msgs.empty()) return;
    msg_scroll_ = std::clamp(msg_scroll_ + delta, 0, (int)msgs.size() - 1);
}

void App::send_current_input() {
    if (input_text_.empty()) return;
    const std::string &ch = client_->state().selected_channel_id;
    if (ch.empty()) return;

    client_->send_message(ch, input_text_);
    input_text_.clear();
    state_ = AppState::CHAT;
    msg_scroll_ = 0;
}

// ---- avatar loading ----------------------------------------------------------

void App::request_avatar(const std::string &user_id, const std::string &hash) {
    avatar_cache_[user_id] = nullptr;  // mark in-progress so we don't re-queue
    std::lock_guard<std::mutex> lock(avatar_mutex_);
    avatar_queue_.push({user_id, hash});
}

void App::drain_avatar_results() {
    std::queue<std::pair<std::string, SDL_Surface*>> done;
    {
        std::lock_guard<std::mutex> lock(avatar_mutex_);
        std::swap(done, avatar_done_);
    }
    while (!done.empty()) {
        auto [user_id, surf] = done.front();
        done.pop();
        if (surf) {
            SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer_, surf);
            SDL_FreeSurface(surf);
            if (tex) {
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                avatar_cache_[user_id] = tex;
            }
        }
        // nullptr surface = download/decode failed; cache entry stays nullptr
    }
}

static const char *AVATAR_CACHE_DIR =
    "/vol/external01/wiiu/discord_wiiu/avatars";

void App::avatar_worker() {
    // Ensure cache directory exists (harmless if already present)
    mkdir(AVATAR_CACHE_DIR, 0755);

    CURL *curl = curl_easy_init();
    if (!curl) return;

    while (!avatar_stop_.load()) {
        std::pair<std::string, std::string> job;
        {
            std::lock_guard<std::mutex> lock(avatar_mutex_);
            if (!avatar_queue_.empty()) {
                job = std::move(avatar_queue_.front());
                avatar_queue_.pop();
            }
        }

        if (job.first.empty()) {
            // No avatar job — try to drain one media job before sleeping.
            std::pair<std::string, std::string> mjob;
            {
                std::lock_guard<std::mutex> lock(avatar_mutex_);
                if (!media_queue_.empty()) {
                    mjob = std::move(media_queue_.front());
                    media_queue_.pop();
                }
            }
            if (!mjob.first.empty()) {
                char mcache[320];
                snprintf(mcache, sizeof(mcache), "%s/%s.png",
                         MEDIA_CACHE_DIR, mjob.first.c_str());
                std::string mraw;
                FILE *mf = fopen(mcache, "rb");
                if (mf) {
                    fseek(mf, 0, SEEK_END); long msz = ftell(mf); rewind(mf);
                    if (msz > 0) { mraw.resize((size_t)msz); fread(&mraw[0], 1, msz, mf); }
                    fclose(mf);
                }
                if (mraw.empty()) {
                    curl_easy_reset(curl);
                    curl_easy_setopt(curl, CURLOPT_URL, mjob.second.c_str());
                    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, avatar_dl_write);
                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mraw);
                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
                    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
                    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                    {
                        std::lock_guard<std::mutex> net_lock(Discord::g_http_mutex);
                        curl_easy_perform(curl);
                    }
                    if (!mraw.empty()) {
                        mkdir(MEDIA_CACHE_DIR, 0755);
                        FILE *mwf = fopen(mcache, "wb");
                        if (mwf) { fwrite(mraw.data(), 1, mraw.size(), mwf); fclose(mwf); }
                    }
                }
                SDL_Surface *msurf = mraw.empty() ? nullptr
                                                  : decode_png_rect(mraw.data(), mraw.size());
                {
                    std::lock_guard<std::mutex> lock(avatar_mutex_);
                    media_done_.push({mjob.first, msurf});
                }
            } else {
                OSSleepTicks(OSMillisecondsToTicks(50));
            }
            continue;
        }

        // job.second is either an avatar hash or a full URL (for default avatars).
        // Full URLs are distinguished by starting with "https://".
        bool is_full_url = (job.second.size() >= 8 &&
                            job.second.compare(0, 8, "https://") == 0);

        char cache_path[320];
        if (is_full_url) {
            snprintf(cache_path, sizeof(cache_path), "%s/%s_default.png",
                     AVATAR_CACHE_DIR, job.first.c_str());
        } else {
            // Hash-based: stale files replaced automatically when hash changes.
            snprintf(cache_path, sizeof(cache_path), "%s/%s_%s.png",
                     AVATAR_CACHE_DIR, job.first.c_str(), job.second.c_str());
        }

        // Try SD card first
        std::string raw;
        FILE *f = fopen(cache_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            rewind(f);
            if (sz > 0) {
                raw.resize((size_t)sz);
                fread(&raw[0], 1, (size_t)sz, f);
            }
            fclose(f);
        }

        // Download from CDN if not cached
        if (raw.empty()) {
            std::string url = is_full_url
                ? job.second
                : ("https://cdn.discordapp.com/avatars/" +
                   job.first + "/" + job.second + ".png?size=64");
            curl_easy_reset(curl);
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, avatar_dl_write);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &raw);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            {
                std::lock_guard<std::mutex> net_lock(Discord::g_http_mutex);
                curl_easy_perform(curl);
            }

            // Persist to SD card for next launch
            if (!raw.empty()) {
                FILE *wf = fopen(cache_path, "wb");
                if (wf) {
                    fwrite(raw.data(), 1, raw.size(), wf);
                    fclose(wf);
                }
            }
        }

        SDL_Surface *surf = raw.empty() ? nullptr
                                        : decode_png_circle(raw.data(), raw.size());
        {
            std::lock_guard<std::mutex> lock(avatar_mutex_);
            avatar_done_.push({job.first, surf});
        }

        // After servicing an avatar job, immediately check for a media job so
        // avatar downloads don't starve attachment image downloads.
        std::pair<std::string, std::string> mjob;
        {
            std::lock_guard<std::mutex> lock(avatar_mutex_);
            if (!media_queue_.empty()) {
                mjob = std::move(media_queue_.front());
                media_queue_.pop();
            }
        }
        if (!mjob.first.empty()) {
            char mcache[320];
            snprintf(mcache, sizeof(mcache), "%s/%s.png",
                     MEDIA_CACHE_DIR, mjob.first.c_str());
            std::string mraw;
            FILE *mf = fopen(mcache, "rb");
            if (mf) {
                fseek(mf, 0, SEEK_END); long msz = ftell(mf); rewind(mf);
                if (msz > 0) { mraw.resize((size_t)msz); fread(&mraw[0], 1, msz, mf); }
                fclose(mf);
            }
            if (mraw.empty()) {
                curl_easy_reset(curl);
                curl_easy_setopt(curl, CURLOPT_URL, mjob.second.c_str());
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, avatar_dl_write);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mraw);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                {
                    std::lock_guard<std::mutex> net_lock(Discord::g_http_mutex);
                    curl_easy_perform(curl);
                }
                if (!mraw.empty()) {
                    mkdir(MEDIA_CACHE_DIR, 0755);
                    FILE *mwf = fopen(mcache, "wb");
                    if (mwf) { fwrite(mraw.data(), 1, mraw.size(), mwf); fclose(mwf); }
                }
            }
            SDL_Surface *msurf = mraw.empty() ? nullptr
                                              : decode_png_rect(mraw.data(), mraw.size());
            {
                std::lock_guard<std::mutex> lock(avatar_mutex_);
                media_done_.push({mjob.first, msurf});
            }
        }
    }

    curl_easy_cleanup(curl);
}

void App::request_media(const Discord::Attachment &att) {
    media_cache_[att.id] = nullptr;  // mark in-progress
    std::lock_guard<std::mutex> lock(avatar_mutex_);
    media_queue_.push({att.id, att.url});
}

void App::drain_media_results() {
    std::queue<std::pair<std::string, SDL_Surface*>> done;
    {
        std::lock_guard<std::mutex> lock(avatar_mutex_);
        std::swap(done, media_done_);
    }
    while (!done.empty()) {
        auto [att_id, surf] = done.front();
        done.pop();
        if (surf) {
            SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer_, surf);
            SDL_FreeSurface(surf);
            media_cache_[att_id] = tex ? tex : nullptr;
        } else {
            media_cache_[att_id] = nullptr;
        }
    }
}

// ---- render ------------------------------------------------------------------

void App::render() {
    SDL_SetRenderDrawColor(renderer_, COL_BG_MAIN.r, COL_BG_MAIN.g, COL_BG_MAIN.b, 255);
    SDL_RenderClear(renderer_);

    switch (state_) {
        case AppState::LOGIN:
            render_login();
            break;
        case AppState::LOADING:
            render_loading();
            break;
        case AppState::ERROR:
            render_loading();  // reuses loading screen with error_msg_
            break;
        default:
            render_guild_list();
            render_channel_list();
            render_header();
            render_chat();
            if (state_ == AppState::TYPING) render_keyboard();
            render_input_box();
            break;
    }

    SDL_RenderPresent(renderer_);
}

// ---- Login / Remote Auth QR screen ------------------------------------------

void App::render_login() {
    draw_.fill_rect(0, 0, L_W, L_H, COL_BG_DARK);

    // Title bar
    draw_.fill_rect(0, 0, L_W, HEADER_H, COL_BG_MED);
    {
        const char *title = "Login with Discord";
        int tw = draw_.text_width(title, draw_.font_lg);
        draw_.draw_text((L_W - tw) / 2, (HEADER_H - draw_.text_height(draw_.font_lg)) / 2,
                        title, COL_TEXT, draw_.font_lg);
    }
    draw_.fill_rect(0, HEADER_H - 1, L_W, 1, COL_BG_DARK);

    if (!remote_auth_) return;

    auto ra = remote_auth_->state();

    if (ra == Discord::RemoteAuthState::CONNECTING) {
        const char *msg = "Connecting and generating encryption keys...";
        int tw = draw_.text_width(msg, draw_.font_md);
        draw_.draw_text((L_W - tw) / 2, L_H / 2 - 8, msg, COL_TEXT_MUTED, draw_.font_md);
        draw_.fill_rect(0, L_H - 4, L_W, 4, COL_ACCENT);
        return;
    }

    if (ra == Discord::RemoteAuthState::FAILED) {
        std::string emsg = remote_auth_->error_msg();
        if (emsg.empty()) emsg = "Login failed";
        int tw = draw_.text_width(emsg, draw_.font_md);
        draw_.draw_text((L_W - tw) / 2, L_H / 2 - 8, emsg, COL_ERROR, draw_.font_md);
        const char *hint = "Restart the app to try again";
        int hw = draw_.text_width(hint, draw_.font_sm);
        draw_.draw_text((L_W - hw) / 2, L_H / 2 + 10, hint, COL_TEXT_MUTED, draw_.font_sm);
        return;
    }

    // WAITING_SCAN or WAITING_CONFIRM
    std::string fp = remote_auth_->fingerprint();

    // Generate (or regenerate) QR code when fingerprint changes
    if (!fp.empty() && fp != qr_fingerprint_) {
        std::string url = "https://discord.com/ra/" + fp;
        qr_ok_ = qrcodegen_encodeText(url.c_str(), qr_tmp_, qr_buf_,
                                      qrcodegen_Ecc_MEDIUM,
                                      qrcodegen_VERSION_MIN, 10,
                                      qrcodegen_Mask_AUTO, true);
        qr_fingerprint_ = fp;
    }

    // QR code rendering — cell size chosen so the code fits in ~140px
    if (qr_ok_) {
        int size     = qrcodegen_getSize(qr_buf_);
        int border   = 4;              // quiet-zone modules
        int total    = size + border * 2;
        int cell     = 140 / total;    // pixels per module
        if (cell < 2) cell = 2;
        int qr_px    = total * cell;
        int qr_x     = L_W / 2 - qr_px / 2;
        int qr_y     = HEADER_H + 10;

        // White background (quiet zone)
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        SDL_Rect bg = { qr_x, qr_y, qr_px, qr_px };
        SDL_RenderFillRect(renderer_, &bg);

        // Dark modules
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        for (int my = 0; my < size; my++) {
            for (int mx = 0; mx < size; mx++) {
                if (qrcodegen_getModule(qr_buf_, mx, my)) {
                    SDL_Rect r = {
                        qr_x + (border + mx) * cell,
                        qr_y + (border + my) * cell,
                        cell, cell
                    };
                    SDL_RenderFillRect(renderer_, &r);
                }
            }
        }

        int text_y = qr_y + qr_px + 8;

        if (ra == Discord::RemoteAuthState::WAITING_CONFIRM) {
            std::string tag = remote_auth_->user_tag();
            std::string msg = tag.empty() ? "Approve on your phone..."
                                          : "Approve login as " + tag;
            int tw = draw_.text_width(msg, draw_.font_md);
            draw_.draw_text((L_W - tw) / 2, text_y, msg, COL_MENTION, draw_.font_md);
            text_y += draw_.text_height(draw_.font_md) + 4;
        } else {
            const char *step1 = "Open Discord on your phone";
            const char *step2 = "Go to Settings > Scan QR Code";
            int w1 = draw_.text_width(step1, draw_.font_md);
            int w2 = draw_.text_width(step2, draw_.font_md);
            draw_.draw_text((L_W - w1) / 2, text_y, step1, COL_TEXT, draw_.font_md);
            text_y += draw_.text_height(draw_.font_md) + 3;
            draw_.draw_text((L_W - w2) / 2, text_y, step2, COL_TEXT_MUTED, draw_.font_sm);
            text_y += draw_.text_height(draw_.font_sm) + 4;
        }

        // Show truncated URL for manual entry
        std::string url_hint = "discord.com/ra/" + fp.substr(0, 16) + "...";
        int uw = draw_.text_width(url_hint, draw_.font_sm);
        draw_.draw_text((L_W - uw) / 2, text_y, url_hint, COL_TEXT_MUTED, draw_.font_sm);
    } else {
        // QR generation failed (shouldn't happen for our short URL)
        const char *msg = "Waiting for login URL...";
        int tw = draw_.text_width(msg, draw_.font_md);
        draw_.draw_text((L_W - tw) / 2, L_H / 2 - 8, msg, COL_TEXT_MUTED, draw_.font_md);
    }

    draw_.fill_rect(0, L_H - 4, L_W, 4, COL_ACCENT);
}

// ---- Loading / error screen --------------------------------------------------

void App::render_loading() {
    draw_.fill_rect(0, 0, L_W, L_H, COL_BG_DARK);

    std::string msg = error_msg_.empty() ? "Connecting to Discord..." : error_msg_;
    // Split on \n
    int y = L_H / 2 - 15;
    std::istringstream ss(msg);
    std::string line;
    while (std::getline(ss, line)) {
        int w = draw_.text_width(line, draw_.font_lg);
        draw_.draw_text((L_W - w) / 2, y, line,
                        error_msg_.empty() ? COL_TEXT : COL_ERROR,
                        draw_.font_lg);
        y += draw_.text_height(draw_.font_lg) + 4;
    }

    // Blurple accent bar at bottom
    draw_.fill_rect(0, L_H - 4, L_W, 4, COL_ACCENT);
}

// ---- Guild sidebar -----------------------------------------------------------

SDL_Color App::username_color(const std::string &user_id) {
    static const SDL_Color PALETTE[] = {
        {255, 99, 71, 255}, {255, 165, 0, 255}, {144, 238, 144, 255},
        {135, 206, 250, 255}, {221, 160, 221, 255}, {255, 218, 185, 255},
    };
    if (user_id.empty()) return COL_ACCENT;
    uint32_t hash = 0;
    for (char c : user_id) hash = hash * 31 + (unsigned char)c;
    return PALETTE[hash % 6];
}

void App::draw_guild_icon(int x, int y, const Discord::Guild &g, bool selected) {
    int r = 12;
    int cx = x + r, cy = y + r;

    if (selected) {
        // Blurple pill indicator on left
        draw_.fill_rounded_rect(0, cy - 10, 2, 20, 1, COL_ACCENT);
        draw_.fill_circle(cx, cy, r, COL_ACCENT);
    } else {
        draw_.fill_circle(cx, cy, r, COL_BG_MED);
    }

    // First letter of server name
    if (!g.name.empty()) {
        std::string letter(1, g.name[0]);
        int tw = draw_.text_width(letter, draw_.font_md);
        int th = draw_.text_height(draw_.font_md);
        draw_.draw_text(cx - tw/2, cy - th/2, letter, COL_WHITE, draw_.font_md);
    }
}

void App::render_guild_list() {
    draw_.fill_rect(0, 0, GUILD_BAR_W, L_H, COL_BG_DARK);

    // Discord home icon
    draw_.fill_rounded_rect(6, 6, 24, 24, 4, COL_ACCENT);
    int lw = draw_.text_width("D", draw_.font_lg);
    int lh = draw_.text_height(draw_.font_lg);
    draw_.draw_text(18 - lw/2, 18 - lh/2, "D", COL_WHITE, draw_.font_lg);

    // Separator
    draw_.fill_rect(8, 34, 20, 1, COL_BG_MED);

    const auto &guilds = client_->state().guilds;
    static const int GU_ITEM_H   = 28;   // 24px icon + 4px gap
    static const int GU_LIST_Y   = 39;
    static const int GU_LIST_BOT = L_H - 5;
    int visible = (GU_LIST_BOT - GU_LIST_Y) / GU_ITEM_H;

    // Keep selected guild icon visible (same pattern as channel list)
    int start_i = 0;
    if (guild_sel_ >= visible)
        start_i = guild_sel_ - visible + 1;

    int y = GU_LIST_Y;
    for (int i = start_i; i < (int)guilds.size(); i++) {
        if (y + 24 > GU_LIST_BOT) break;
        bool sel = (state_ == AppState::GUILD_LIST && i == guild_sel_) ||
                   guilds[i].id == client_->state().selected_guild_id;
        draw_guild_icon(6, y, guilds[i], sel);
        y += GU_ITEM_H;
    }
}

// ---- Channel list ------------------------------------------------------------

void App::draw_channel_item(int x, int y, int w, const Discord::Channel &c, bool selected) {
    if (selected)
        draw_.fill_rounded_rect(x + 2, y, w - 4, 16, 2, COL_BG_SELECT);

    bool is_active = c.id == client_->state().selected_channel_id;
    SDL_Color text_col = (selected || is_active) ? COL_TEXT : COL_TEXT_MUTED;

    std::string label = "# " + c.name;
    draw_.draw_text(x + 6, y + 3, label, text_col, draw_.font_sm);
}

void App::render_channel_list() {
    int x = GUILD_BAR_W;
    draw_.fill_rect(x, 0, CHANNEL_BAR_W, L_H, COL_BG_MED);

    // Server name header
    std::string guild_name = "Direct Messages";
    const auto &guilds = client_->state().guilds;
    for (auto &g : guilds) {
        if (g.id == client_->state().selected_guild_id) {
            guild_name = g.name;
            break;
        }
    }
    // Trim to fit
    while (draw_.text_width(guild_name, draw_.font_bold) > CHANNEL_BAR_W - 12 && guild_name.size() > 4)
        guild_name.resize(guild_name.size() - 1);

    draw_.fill_rect(x, 0, CHANNEL_BAR_W, HEADER_H, COL_BG_MED);
    // subtle separator
    draw_.fill_rect(x, HEADER_H - 1, CHANNEL_BAR_W, 1, COL_BG_DARK);
    draw_.draw_text(x + 6, 7, guild_name, COL_TEXT, draw_.font_bold);

    const auto &channels = client_->state().channels;
    static const int CH_ITEM_H  = 17;
    static const int CH_LIST_Y  = HEADER_H + 4;
    static const int CH_LIST_BOT = L_H - 4;
    int visible = (CH_LIST_BOT - CH_LIST_Y) / CH_ITEM_H;

    // Scroll window so selected item is always visible
    int start_i = 0;
    if (channel_sel_ >= visible)
        start_i = channel_sel_ - visible + 1;

    int y = CH_LIST_Y;
    if (channels.empty() && client_->is_channels_loading()) {
        draw_.draw_text(x + 16, y, "Loading channels...", COL_TEXT_MUTED, draw_.font_sm);
    } else {
        for (int i = start_i; i < (int)channels.size(); i++) {
            if (y + CH_ITEM_H > CH_LIST_BOT) break;
            bool sel = (state_ == AppState::CHANNEL_LIST && i == channel_sel_);
            draw_channel_item(x, y, CHANNEL_BAR_W, channels[i], sel);
            y += CH_ITEM_H;
        }
    }

}

// ---- Header bar --------------------------------------------------------------

void App::render_header() {
    int x = CHAT_X;
    draw_.fill_rect(x, 0, CHAT_W, HEADER_H, COL_BG_MAIN);
    draw_.fill_rect(x, HEADER_H - 1, CHAT_W, 1, COL_BG_DARK);

    const auto &channels = client_->state().channels;
    const std::string &sel_ch = client_->state().selected_channel_id;

    std::string ch_name = "Select a channel";
    std::string ch_topic;
    for (auto &c : channels) {
        if (c.id == sel_ch) {
            ch_name = "# " + c.name;
            ch_topic = c.topic;
            break;
        }
    }

    draw_.draw_text(x + 8, 5, ch_name, COL_TEXT, draw_.font_bold);
    if (!ch_topic.empty()) {
        // Truncate topic
        std::string t = ch_topic;
        while (draw_.text_width(t, draw_.font_sm) > CHAT_W - 100 && t.size() > 4)
            t.resize(t.size() - 1);
        if (t != ch_topic) t += "...";
        draw_.draw_text(x + 8 + draw_.text_width(ch_name, draw_.font_bold) + 6,
                        6, t, COL_TEXT_MUTED, draw_.font_sm);
    }

    // Controls hint
    std::string hint = "Y:Type  B:Back  ZL/ZR:Scroll  -:DMs";
    int hw = draw_.text_width(hint, draw_.font_sm);
    draw_.draw_text(x + CHAT_W - hw - 4, 8, hint, COL_TEXT_MUTED, draw_.font_sm);
}

// ---- Chat messages -----------------------------------------------------------

void App::draw_message(int x, int &y, const Discord::Message &msg, bool group) {
    int avail_w = CHAT_W - MSG_PADDING * 2 - MSG_INDENT;

    if (!group) {
        // Avatar — use real image if downloaded, otherwise fall back to letter circle
        auto av_it = avatar_cache_.find(msg.author.id);
        if (av_it != avatar_cache_.end() && av_it->second) {
            SDL_Rect dst = { x + MSG_PADDING, y, MSG_AVATAR_W, MSG_AVATAR_H };
            SDL_RenderCopy(renderer_, av_it->second, nullptr, &dst);
        } else {
            SDL_Color acol = username_color(msg.author.id);
            draw_.fill_circle(x + MSG_PADDING + MSG_AVATAR_W/2,
                              y + MSG_AVATAR_H/2, MSG_AVATAR_W/2, acol);
            const std::string &dn = msg.author.global_name.empty()
                                    ? msg.author.username : msg.author.global_name;
            std::string initial = dn.empty() ? "?" : std::string(1, dn[0]);
            int iw = draw_.text_width(initial, draw_.font_sm);
            int ih = draw_.text_height(draw_.font_sm);
            draw_.draw_text(x + MSG_PADDING + MSG_AVATAR_W/2 - iw/2,
                            y + MSG_AVATAR_H/2 - ih/2, initial, COL_WHITE, draw_.font_sm);

            // Queue download if not already requested and hash is known
            if (av_it == avatar_cache_.end() && !msg.author.avatar.empty())
                request_avatar(msg.author.id, msg.author.avatar);
        }

        // Display name (global_name preferred over @username)
        const std::string &display_name = msg.author.global_name.empty()
                                          ? msg.author.username : msg.author.global_name;
        draw_.draw_text(x + MSG_PADDING + MSG_INDENT, y,
                        display_name, username_color(msg.author.id), draw_.font_bold);

        // Timestamp
        std::string ts = msg.timestamp;
        if (ts.size() >= 16) ts = ts.substr(11, 5);
        int uw = draw_.text_width(display_name, draw_.font_bold);
        draw_.draw_text(x + MSG_PADDING + MSG_INDENT + uw + 4, y + 1,
                        ts, COL_TEXT_MUTED, draw_.font_sm);
        y += draw_.text_height(draw_.font_bold) + 2;
    }

    // Message content
    if (!msg.content.empty()) {
        auto lines = draw_.wrap_text(msg.content, avail_w, draw_.font_md);
        for (auto &line : lines) {
            draw_.draw_text(x + MSG_PADDING + MSG_INDENT, y,
                            line, COL_TEXT, draw_.font_md);
            y += draw_.text_height(draw_.font_md) + 1;
        }
    }

    // Attachments
    for (const auto &att : msg.attachments) {
        std::string ctype = att.content_type;
        if (ctype.empty()) ctype = infer_content_type(att.filename);

        bool is_png = (ctype == "image/png") && att.size <= MEDIA_MAX_BYTES;

        if (is_png && !att.url.empty()) {
            auto it = media_cache_.find(att.id);
            if (it == media_cache_.end()) {
                request_media(att);
                it = media_cache_.find(att.id);
            }

            if (it->second) {
                // Render inline, scaled to fit MEDIA_MAX_W x MEDIA_MAX_H
                int tw, th;
                SDL_QueryTexture(it->second, nullptr, nullptr, &tw, &th);
                int dw = tw, dh = th;
                if (dw > MEDIA_MAX_W) { dh = dh * MEDIA_MAX_W / dw; dw = MEDIA_MAX_W; }
                if (dh > MEDIA_MAX_H) { dw = dw * MEDIA_MAX_H / dh; dh = MEDIA_MAX_H; }
                SDL_Rect dst = { x + MSG_PADDING + MSG_INDENT, y, dw, dh };
                SDL_RenderCopy(renderer_, it->second, nullptr, &dst);
                y += dh + 6;
                continue;
            }
            // nullptr = still loading — show placeholder text below
        }

        // Non-PNG images and all other attachment types: type-labelled badge.
        bool is_image = ctype.rfind("image/", 0) == 0;
        bool is_video = ctype.rfind("video/", 0) == 0;
        bool is_audio = ctype.rfind("audio/", 0) == 0;

        std::string badge_prefix;
        SDL_Color   badge_col = COL_TEXT_MUTED;
        if (is_png && att.size > MEDIA_MAX_BYTES) {
            badge_prefix = "PNG";     badge_col = { 88, 101, 242, 255 };
        } else if (is_image) {
            // Uppercase the sub-type (jpeg → JPEG, gif → GIF, …)
            std::string sub = ctype.size() > 6 ? ctype.substr(6) : "IMG";
            for (char &c : sub) c = (char)toupper((unsigned char)c);
            badge_prefix = sub;       badge_col = { 88, 101, 242, 255 };
        } else if (is_video) {
            badge_prefix = "VIDEO";   badge_col = { 237,  66,  69, 255 };
        } else if (is_audio) {
            badge_prefix = "AUDIO";   badge_col = {  87, 242, 135, 255 };
        } else {
            badge_prefix = "FILE";    badge_col = COL_TEXT_MUTED;
        }

        std::string label = badge_prefix + "  " + att.filename
                          + "  " + format_bytes(att.size);
        if (is_png && att.size <= MEDIA_MAX_BYTES) label = "Loading...  " + att.filename;

        int label_w = std::min(draw_.text_width(label, draw_.font_sm) + 10, avail_w);
        draw_.fill_rounded_rect(x + MSG_PADDING + MSG_INDENT, y, label_w, 13, 2, COL_BG_MED);
        SDL_Rect clip2 = { x + MSG_PADDING + MSG_INDENT, y, label_w, 13 };
        SDL_RenderSetClipRect(renderer_, &clip2);
        draw_.draw_text(x + MSG_PADDING + MSG_INDENT + 4, y + 2, label, badge_col, draw_.font_sm);
        SDL_RenderSetClipRect(renderer_, nullptr);
        y += 15;
    }

    if (msg.edited) {
        draw_.draw_text(x + MSG_PADDING + MSG_INDENT, y,
                        "(edited)", COL_TEXT_MUTED, draw_.font_sm);
        y += draw_.text_height(draw_.font_sm) + 1;
    }

    if (!group) y += 2;  // extra spacing between message groups
}

void App::render_chat() {
    int x = CHAT_X;
    int chat_y = CHAT_BODY_Y;
    int chat_h = CHAT_BODY_H - 4;  // 4px breathing room above the input bar

    draw_.fill_rect(x, chat_y, CHAT_W, chat_h, COL_BG_MAIN);

    const auto &msgs = client_->state().messages;
    if (msgs.empty()) {
        std::string empty_msg;
        if (client_->is_messages_loading())
            empty_msg = "Loading messages...";
        else if (client_->state().selected_channel_id.empty())
            empty_msg = "Select a channel to start chatting";
        else
            empty_msg = "No messages yet";
        int w = draw_.text_width(empty_msg, draw_.font_md);
        draw_.draw_text(x + (CHAT_W - w) / 2,
                        chat_y + chat_h / 2,
                        empty_msg, COL_TEXT_MUTED, draw_.font_md);
        return;
    }

    // Determine which messages to render (bottom up, with scrolling)
    // We'll do a first pass to compute heights, then render
    // For simplicity: render from bottom, stop when we run out of space
    // msg_scroll_ = 0 means we see the newest messages at the bottom

    int total = (int)msgs.size();
    // Start index into msgs (0 = oldest, total-1 = newest)
    int end_idx = total - 1 - msg_scroll_;  // newest visible msg
    end_idx = std::clamp(end_idx, 0, total - 1);

    // We'll render bottom-up and crop at the top
    // Use a temporary render to measure, then blit
    struct LineInfo { int start_msg; int line_y; };

    // Simple approach: render from top for the visible window
    // Determine start: walk backwards accumulating height until chat_h
    struct MsgHeight {
        int idx;
        int h;
        bool group;  // grouped with previous author
    };
    std::vector<MsgHeight> visible;
    int total_h = 0;

    for (int i = end_idx; i >= 0; i--) {
        const auto &msg = msgs[i];
        bool group = i > 0 &&
                     msgs[i-1].author.id == msg.author.id &&
                     msg.content.find('\n') == std::string::npos;

        int h = 0;
        if (!group) h += draw_.text_height(draw_.font_bold) + 2 + 4;

        auto lines = draw_.wrap_text(msg.content,
                                     CHAT_W - MSG_PADDING*2 - MSG_INDENT, draw_.font_md);
        h += (int)lines.size() * (draw_.text_height(draw_.font_md) + 1);

        for (const auto &att : msg.attachments) {
            std::string ctype = att.content_type;
            if (ctype.empty()) ctype = infer_content_type(att.filename);
            bool is_png = (ctype == "image/png") && att.size <= MEDIA_MAX_BYTES;
            if (is_png) {
                auto it = media_cache_.find(att.id);
                if (it != media_cache_.end() && it->second) {
                    int tw, th;
                    SDL_QueryTexture(it->second, nullptr, nullptr, &tw, &th);
                    int dh = th;
                    if (tw > MEDIA_MAX_W) dh = dh * MEDIA_MAX_W / tw;
                    if (dh > MEDIA_MAX_H) dh = MEDIA_MAX_H;
                    h += dh + 6;
                } else {
                    h += 15;  // placeholder badge height
                }
            } else {
                h += 15;  // type badge height
            }
        }
        if (msg.edited) h += draw_.text_height(draw_.font_sm) + 1;

        total_h += h;
        visible.push_back({ i, h, group });
        if (total_h > chat_h + 100) break;  // enough
    }

    // Render in chronological order (reverse the visible list)
    std::reverse(visible.begin(), visible.end());

    // Anchor the newest message at the bottom. When total_h <= chat_h the
    // content is short and we push it down. When total_h > chat_h we start
    // above the clip rect so the excess scrolls off the top — the clip rect
    // hides everything above chat_y. Previously y_offset was always >= 0 which
    // caused the newest messages to be cut off at the bottom.
    int y = chat_y + (chat_h - total_h);

    // Set clip to chat area
    SDL_Rect clip = { x, chat_y, CHAT_W, chat_h };
    SDL_RenderSetClipRect(renderer_, &clip);

    for (auto &mh : visible) {
        if (y >= chat_y + chat_h) break;
        draw_message(x, y, msgs[mh.idx], mh.group);
        SDL_RenderSetClipRect(renderer_, &clip);  // draw_message may clear clip for badge rendering
    }

    SDL_RenderSetClipRect(renderer_, nullptr);

    // Scroll indicator
    if (msg_scroll_ > 0) {
        std::string scroll_msg = "^ " + std::to_string(msg_scroll_) + " newer messages ^";
        int sw = draw_.text_width(scroll_msg, draw_.font_sm);
        draw_.fill_rounded_rect(x + (CHAT_W - sw) / 2 - 4,
                                chat_y + chat_h - 14, sw + 8, 11, 2, COL_ACCENT);
        draw_.draw_text(x + (CHAT_W - sw) / 2, chat_y + chat_h - 12,
                        scroll_msg, COL_WHITE, draw_.font_sm);
    }

    // Typing indicator
    if (!typing_users_.empty()) {
        std::string typing_str;
        int count = 0;
        for (auto &[uid, _] : typing_users_) {
            // We don't have username lookup by id easily, use uid truncated
            if (count > 0) typing_str += ", ";
            typing_str += uid.substr(0, 6) + "...";
            if (++count >= 3) { typing_str += " and others"; break; }
        }
        typing_str += (count == 1 ? " is typing..." : " are typing...");
        draw_.draw_text(x + MSG_PADDING, chat_y + chat_h - 9,
                        typing_str, COL_TEXT_MUTED, draw_.font_sm);
    }
}

// ---- Virtual keyboard --------------------------------------------------------
// Three layers, each with 3 character rows and a shared special row.
// Layer 0 = letters (QWERTY), Layer 1 = numbers + punctuation, Layer 2 = symbols.
// All 32 printable ASCII non-alphanumeric chars are covered across layers 1-2,
// plus a selection of common Unicode extras on layer 2.

static const char * const KB_DATA[3][3][10] = {
    // Layer 0: QWERTY letters
    {
        { "q","w","e","r","t","y","u","i","o","p" },
        { "a","s","d","f","g","h","j","k","l", nullptr },
        { "z","x","c","v","b","n","m", nullptr, nullptr, nullptr },
    },
    // Layer 1: numbers + common punctuation
    {
        { "1","2","3","4","5","6","7","8","9","0" },
        { "!","@","#","$","%","^","&","*","(",")" },
        { "-","_","=","+",".","," ,"?","'","\"","/" },
    },
    // Layer 2: remaining ASCII symbols + useful Unicode extras
    {
        { ":",";","~","`","|","\\","<",">","[","]" },
        { "{","}","€","£","¥","©","®","™","→","✓" },
        { "♥","★","←","↑","↓","≠","≤","≥","×","÷" },
    },
};
static const int KB_ROW_LENS[3][3] = {
    { 10, 9, 7 },
    { 10, 10, 10 },
    { 10, 10, 10 },
};
static const int KB_KEY_W   = 44;
static const int KB_KEY_H   = 25;
static const int KB_KEY_GAP = 2;
static const int KB_ROW_H   = KB_KEY_H + KB_KEY_GAP;
static const int KB_SP_W    = 50;    // special-key width (CAPS/ABC, 123/SYM, DEL, SEND)
static const int KB_TOTAL_H = 4 * KB_ROW_H + 6;
static const int KB_TOP_Y   = L_H - INPUT_H - KB_TOTAL_H;

// Return the display/input string for a key, uppercasing layer-0 letters when caps is on.
static std::string kb_key_str(const char *raw, bool caps, int layer) {
    if (!raw) return {};
    if (layer == 0 && caps && raw[0] >= 'a' && raw[0] <= 'z' && raw[1] == '\0')
        return std::string(1, (char)(raw[0] - 'a' + 'A'));
    return raw;
}

void App::render_keyboard() {
    draw_.fill_rect(CHAT_X, KB_TOP_Y, CHAT_W, KB_TOTAL_H, { 28, 29, 33, 255 });

    // Character rows
    for (int r = 0; r < 3; r++) {
        int n       = KB_ROW_LENS[kb_layer_][r];
        int total_w = n * KB_KEY_W + (n - 1) * KB_KEY_GAP;
        int start_x = CHAT_X + (CHAT_W - total_w) / 2;
        int ky      = KB_TOP_Y + 4 + r * KB_ROW_H;
        for (int i = 0; i < n; i++) {
            std::string label = kb_key_str(KB_DATA[kb_layer_][r][i], kb_caps_, kb_layer_);
            int kx = start_x + i * (KB_KEY_W + KB_KEY_GAP);
            draw_.fill_rounded_rect(kx, ky, KB_KEY_W, KB_KEY_H, 3, { 55, 57, 63, 255 });
            int tw = draw_.text_width(label.c_str(), draw_.font_sm);
            int th = draw_.text_height(draw_.font_sm);
            draw_.draw_text(kx + (KB_KEY_W - tw) / 2, ky + (KB_KEY_H - th) / 2,
                            label.c_str(), COL_TEXT, draw_.font_sm);
        }
    }

    // Special row: [CAPS/ABC] [123/SYM] [___SPACE___] [DEL] [SEND]
    // space_w derived so the row fills CHAT_W with 4px side padding each side
    int ky      = KB_TOP_Y + 4 + 3 * KB_ROW_H;
    int space_w = CHAT_W - 4 * KB_SP_W - 4 * KB_KEY_GAP - 8;
    int kx      = CHAT_X + 4;

    // CAPS (layer 0) or ABC (layers 1/2)
    {
        const char *lbl = (kb_layer_ == 0) ? "CAPS" : "ABC";
        SDL_Color   bg  = (kb_layer_ == 0 && kb_caps_) ? COL_ACCENT : SDL_Color{ 70, 72, 80, 255 };
        draw_.fill_rounded_rect(kx, ky, KB_SP_W, KB_KEY_H, 3, bg);
        int tw = draw_.text_width(lbl, draw_.font_sm), th = draw_.text_height(draw_.font_sm);
        draw_.draw_text(kx + (KB_SP_W - tw)/2, ky + (KB_KEY_H - th)/2, lbl, COL_TEXT, draw_.font_sm);
    }
    kx += KB_SP_W + KB_KEY_GAP;

    // 123 (layer 0→1) or SYM (layer 1→2) or 123 (layer 2→1)
    {
        const char *lbl = (kb_layer_ == 1) ? "SYM" : "123";
        draw_.fill_rounded_rect(kx, ky, KB_SP_W, KB_KEY_H, 3, { 70, 72, 80, 255 });
        int tw = draw_.text_width(lbl, draw_.font_sm), th = draw_.text_height(draw_.font_sm);
        draw_.draw_text(kx + (KB_SP_W - tw)/2, ky + (KB_KEY_H - th)/2, lbl, COL_TEXT, draw_.font_sm);
    }
    kx += KB_SP_W + KB_KEY_GAP;

    // Space bar
    draw_.fill_rounded_rect(kx, ky, space_w, KB_KEY_H, 3, { 55, 57, 63, 255 });
    { int tw = draw_.text_width("SPACE", draw_.font_sm), th = draw_.text_height(draw_.font_sm);
      draw_.draw_text(kx + (space_w - tw)/2, ky + (KB_KEY_H - th)/2, "SPACE", COL_TEXT_MUTED, draw_.font_sm); }
    kx += space_w + KB_KEY_GAP;

    // DEL
    draw_.fill_rounded_rect(kx, ky, KB_SP_W, KB_KEY_H, 3, { 80, 40, 40, 255 });
    { int tw = draw_.text_width("DEL", draw_.font_sm), th = draw_.text_height(draw_.font_sm);
      draw_.draw_text(kx + (KB_SP_W - tw)/2, ky + (KB_KEY_H - th)/2, "DEL", COL_TEXT, draw_.font_sm); }
    kx += KB_SP_W + KB_KEY_GAP;

    // SEND
    draw_.fill_rounded_rect(kx, ky, KB_SP_W, KB_KEY_H, 3, COL_ACCENT);
    { int tw = draw_.text_width("SEND", draw_.font_sm), th = draw_.text_height(draw_.font_sm);
      draw_.draw_text(kx + (KB_SP_W - tw)/2, ky + (KB_KEY_H - th)/2, "SEND", COL_WHITE, draw_.font_sm); }
}

void App::handle_keyboard_touch(int tx, int ty) {
    // Character rows
    for (int r = 0; r < 3; r++) {
        int n       = KB_ROW_LENS[kb_layer_][r];
        int total_w = n * KB_KEY_W + (n - 1) * KB_KEY_GAP;
        int start_x = CHAT_X + (CHAT_W - total_w) / 2;
        int ky      = KB_TOP_Y + 4 + r * KB_ROW_H;
        if (ty < ky || ty >= ky + KB_KEY_H) continue;
        for (int i = 0; i < n; i++) {
            int kx = start_x + i * (KB_KEY_W + KB_KEY_GAP);
            if (tx >= kx && tx < kx + KB_KEY_W) {
                input_text_ += kb_key_str(KB_DATA[kb_layer_][r][i], kb_caps_, kb_layer_);
                return;
            }
        }
        return;
    }

    // Special row
    int ky      = KB_TOP_Y + 4 + 3 * KB_ROW_H;
    if (ty < ky || ty >= ky + KB_KEY_H) return;
    int space_w = CHAT_W - 4 * KB_SP_W - 4 * KB_KEY_GAP - 8;
    int kx      = CHAT_X + 4;

    // CAPS / ABC
    if (tx >= kx && tx < kx + KB_SP_W) {
        if (kb_layer_ == 0) kb_caps_ = !kb_caps_;
        else kb_layer_ = 0;
        return;
    }
    kx += KB_SP_W + KB_KEY_GAP;

    // 123 / SYM / 123
    if (tx >= kx && tx < kx + KB_SP_W) {
        if      (kb_layer_ == 0) kb_layer_ = 1;
        else if (kb_layer_ == 1) kb_layer_ = 2;
        else                     kb_layer_ = 1;
        return;
    }
    kx += KB_SP_W + KB_KEY_GAP;

    // Space
    if (tx >= kx && tx < kx + space_w) { input_text_ += ' '; return; }
    kx += space_w + KB_KEY_GAP;

    // DEL — UTF-8 aware: back off one full codepoint
    if (tx >= kx && tx < kx + KB_SP_W) {
        if (!input_text_.empty()) {
            size_t len = input_text_.size();
            while (len > 0 && (input_text_[len-1] & 0xC0) == 0x80) len--;
            if (len > 0) len--;
            input_text_.resize(len);
        }
        return;
    }
    kx += KB_SP_W + KB_KEY_GAP;

    // SEND
    if (tx >= kx && tx < kx + KB_SP_W) { send_current_input(); }
}

// ---- Input box ---------------------------------------------------------------

void App::render_input_box() {
    int x = CHAT_X;
    int y = L_H - INPUT_H;

    draw_.fill_rect(x, y, CHAT_W, INPUT_H, COL_BG_MAIN);

    if (state_ == AppState::TYPING || state_ == AppState::CHAT) {
        int bx = x + 8, by = y + 5, bw = CHAT_W - 16, bh = INPUT_H - 10;
        draw_.fill_rounded_rect(bx, by, bw, bh, 3, COL_BG_INPUT);

        if (state_ == AppState::TYPING) {
            std::string display = input_text_;
            if (input_cursor_blink_) display += "|";
            // Clip to box
            SDL_Rect clip = { bx + 4, by, bw - 8, bh };
            SDL_RenderSetClipRect(renderer_, &clip);
            draw_.draw_text(bx + 8, by + (bh - draw_.text_height(draw_.font_md)) / 2,
                            display, COL_TEXT, draw_.font_md);
            SDL_RenderSetClipRect(renderer_, nullptr);
        } else {
            // Placeholder
            const auto &channels = client_->state().channels;
            const std::string &sel = client_->state().selected_channel_id;
            std::string placeholder = "Press Y or Enter to type a message...";
            for (auto &c : channels) {
                if (c.id == sel) { placeholder = "Message #" + c.name; break; }
            }
            draw_.draw_text(bx + 8, by + (bh - draw_.text_height(draw_.font_md)) / 2,
                            placeholder, COL_TEXT_MUTED, draw_.font_md);
        }
    }
}

// ---- Status bar --------------------------------------------------------------

} // namespace UI
