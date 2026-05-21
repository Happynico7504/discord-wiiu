#pragma once
#include <string>
#include <memory>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <vpad/input.h>
#include "renderer.h"
#include "theme.h"
#include "../discord/client.h"
#include "../discord/remote_auth.h"

namespace UI {

enum class AppState {
    LOGIN,
    LOADING,
    GUILD_LIST,
    CHANNEL_LIST,
    CHAT,
    TYPING,
    ERROR,
};

class App {
public:
    explicit App(std::shared_ptr<Discord::Client> client);
    ~App();

    void run();
    std::shared_ptr<Discord::Client> get_client() const { return client_; }

private:
    // -- lifecycle --
    bool setup_sdl();
    bool load_fonts(const char *font_path);
    void teardown();
    void wire_callbacks();
    static void save_token_to_disk(const std::string &token);

    // -- main loop --
    void update();
    void render();

    // -- input --
    void handle_sdl_events();
    void handle_vpad();

    // -- per-state render --
    void render_login();
    void render_loading();
    void render_guild_list();
    void render_channel_list();
    void render_chat();
    void render_keyboard();
    void render_input_box();
    void render_header();

    // -- navigation helpers --
    void select_guild(int index);
    void select_channel(int index);
    void open_dm_list();
    void scroll_messages(int delta);
    void send_current_input();
    void handle_keyboard_touch(int tx, int ty);

    // -- avatar loading --
    void avatar_worker();
    void request_avatar(const std::string &user_id, const std::string &hash);
    void drain_avatar_results();

    // -- attachment image loading --
    void request_media(const Discord::Attachment &att);
    void drain_media_results();

    // -- draw helpers --
    void draw_guild_icon(int x, int y, const Discord::Guild &g, bool selected);
    void draw_channel_item(int x, int y, int w, const Discord::Channel &c, bool selected);
    void draw_message(int x, int &y, const Discord::Message &msg, bool group);
    SDL_Color username_color(const std::string &user_id);

    // SDL
    SDL_Window   *window_   = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    Renderer      draw_;

    // Discord
    std::shared_ptr<Discord::Client>        client_;
    std::unique_ptr<Discord::RemoteAuth>    remote_auth_;

    // QR code buffers for login screen (qrcodegen version ≤ 10 → 408 bytes each)
    static constexpr int QR_BUF_LEN = 420;
    uint8_t  qr_buf_[QR_BUF_LEN];
    uint8_t  qr_tmp_[QR_BUF_LEN];
    bool     qr_ok_          = false;
    std::string qr_fingerprint_;  // fingerprint for which qr_buf_ was generated

    // State
    AppState state_       = AppState::LOADING;
    int guild_sel_        = 0;
    int channel_sel_      = 0;
    int msg_scroll_       = 0;   // how many messages from bottom are scrolled up
    bool ready_           = false;

    // Input box
    std::string input_text_;
    bool        input_cursor_blink_ = true;
    Uint32      cursor_tick_        = 0;

    // Virtual keyboard state
    bool        kb_caps_       = false;
    bool        vpad_touching_ = false;
    int         kb_layer_      = 0;   // 0=letters, 1=numbers/punct, 2=symbols

    // Typing indicators (user_id -> tick when typing expires)
    std::unordered_map<std::string, Uint32> typing_users_;

    // Error display
    std::string error_msg_;

    // Avatar + media share one mutex and one worker thread.
    // nullptr texture = in-progress or failed; no map entry = never requested.
    std::unordered_map<std::string, SDL_Texture*>          avatar_cache_;
    std::unordered_map<std::string, SDL_Texture*>          media_cache_;   // key: att.id
    std::mutex                                              avatar_mutex_;
    std::queue<std::pair<std::string, std::string>>        avatar_queue_;  // (user_id, hash)
    std::queue<std::pair<std::string, SDL_Surface*>>       avatar_done_;
    std::queue<std::pair<std::string, std::string>>        media_queue_;   // (att_id, url)
    std::queue<std::pair<std::string, SDL_Surface*>>       media_done_;
    std::thread                                            avatar_thread_;
    std::atomic<bool>                                      avatar_stop_{false};

    // Frame timing
    Uint32 last_frame_    = 0;
};

} // namespace UI
