#pragma once
#include <SDL2/SDL.h>

namespace UI {

// Discord dark theme colors
constexpr SDL_Color COL_BG_DARK    = { 32,  34,  37, 255 };  // #202225 server sidebar
constexpr SDL_Color COL_BG_MED     = { 47,  49,  54, 255 };  // #2f3136 channel sidebar
constexpr SDL_Color COL_BG_MAIN    = { 54,  57,  63, 255 };  // #36393f main area
constexpr SDL_Color COL_BG_INPUT   = { 64,  68,  75, 255 };  // #40444b input box
constexpr SDL_Color COL_BG_HOVER   = { 66,  70,  77, 255 };  // hover state
constexpr SDL_Color COL_BG_SELECT  = { 79,  84,  92, 255 };  // selected item
constexpr SDL_Color COL_ACCENT     = { 88, 101, 242, 255 };  // #5865f2 blurple
constexpr SDL_Color COL_ACCENT2    = { 71,  82, 196, 255 };  // #4752c4 blurple hover
constexpr SDL_Color COL_TEXT       = {220, 221, 222, 255 };  // #dcddde primary text
constexpr SDL_Color COL_TEXT_MUTED = {114, 118, 125, 255 };  // #72767d muted
constexpr SDL_Color COL_TEXT_LINK  = { 0,  168, 252, 255 };  // links
constexpr SDL_Color COL_ONLINE     = { 59, 165,  92, 255 };  // #3ba55c online
constexpr SDL_Color COL_OFFLINE    = {116, 127, 141, 255 };  // offline
constexpr SDL_Color COL_MENTION    = {250, 166,  26, 255 };  // #faa61a mention
constexpr SDL_Color COL_ERROR      = {237,  66,  69, 255 };  // #ed4245 red
constexpr SDL_Color COL_WHITE      = {255, 255, 255, 255 };
constexpr SDL_Color COL_TRANSPARENT= {  0,   0,   0,   0 };

// Layout (TV: 1280x720 window, rendered at half resolution via SDL logical size)
constexpr int SCREEN_W       = 1280;
constexpr int SCREEN_H       = 720;
constexpr int SCALE          = 2;
constexpr int L_W            = SCREEN_W / SCALE;  // 640 – logical canvas width
constexpr int L_H            = SCREEN_H / SCALE;  // 360 – logical canvas height
constexpr int GUILD_BAR_W    = 36;   // server icon bar
constexpr int CHANNEL_BAR_W  = 110;  // channel list
constexpr int CHAT_X         = GUILD_BAR_W + CHANNEL_BAR_W;
constexpr int CHAT_W         = L_W - CHAT_X;
constexpr int HEADER_H       = 24;
constexpr int INPUT_H        = 28;
constexpr int CHAT_BODY_H    = L_H - HEADER_H - INPUT_H;
constexpr int CHAT_BODY_Y    = HEADER_H;

// Font sizes
constexpr int FONT_SIZE_SM   = 10;
constexpr int FONT_SIZE_MD   = 11;
constexpr int FONT_SIZE_LG   = 14;

// Message layout
constexpr int MSG_AVATAR_W   = 20;
constexpr int MSG_AVATAR_H   = 20;
constexpr int MSG_PADDING    = 8;
constexpr int MSG_INDENT     = MSG_AVATAR_W + 6;

} // namespace UI
