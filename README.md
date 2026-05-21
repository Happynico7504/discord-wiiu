# Discord Wii U

A Discord client for the Nintendo Wii U, running as an [Aroma](https://aroma.foryour.cafe/) homebrew application. Built with SDL2, libcurl WebSockets, and the Discord Gateway v10 API.

## Features

- Real-time messaging via Discord Gateway (WebSocket v10, auto-reconnect)
- Server and channel list with full `VIEW_CHANNEL` permission filtering
- Message history with avatars, timestamps, and scroll
- `@mention` resolution — displays names instead of raw `<@id>` tags
- Attachment display — inline images and file badges with size
- Typing indicators
- Direct Messages (non-blocking async load)
- On-screen touch keyboard — three layers covering all printable ASCII and common Unicode
- Discord dark theme UI rendered at 1280×720

## Screenshots

```
┌──────┬────────────────┬──────────────────────────────────────────────────┐
│  S1  │  # general     │  # general                                       │
│      │  # random      ├──────────────────────────────────────────────────┤
│  S2  │  # memes       │  [●] Alice          Today at 14:22               │
│      │                │      Hello from Wii U!                           │
│  S3  │  ──────────    │                                                  │
│      │  # off-topic   │  [●] Bob            Today at 14:23               │
│ DMs  │                │      That's awesome, @Alice nice work            │
│      │                │                                                  │
│      │                │  [●] Alice          Today at 14:24               │
│      │                │      📎 screenshot.png  1.2 MB                   │
│      │                ├──────────────────────────────────────────────────┤
│      │                │  Message #general...                             │
└──────┴────────────────┴──────────────────────────────────────────────────┘
```

Touch keyboard (Y button or tap input bar on the GamePad):
```
┌──────────────────────────────────────────────────────────────────────────┐
│  q  w  e  r  t  y  u  i  o  p                                           │
│   a  s  d  f  g  h  j  k  l                                             │
│     z  x  c  v  b  n  m                                                 │
│  [CAPS] [123]  [       SPACE       ]  [DEL]  [SEND]                     │
│  Hello, world!▌                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

## Requirements

### Build dependencies

[devkitPro](https://devkitpro.org/wiki/Getting_Started) with the following packages:

```bash
(dkp-)pacman -S wut wiiu-sdl2 wiiu-sdl2_ttf wiiu-curl wiiu-mbedtls wiiu-zlib
```

The full library chain pulled in transitively: `harfbuzz`, `freetype`, `libpng`, `brotli`, `bzip2`.

### Runtime

- Nintendo Wii U running [Aroma CFW](https://aroma.foryour.cafe/)
- SD card
- A TTF font file (see Installation)
- USB keyboard (optional — the on-screen touch keyboard covers all input)

## Building

```bash
export DEVKITPRO=/opt/devkitpro
make
# Produces: discord-wiiu.wuhb
```

Using Docker:

```bash
docker run --rm -v "$(pwd):/project" devkitpro/devkitppc:latest bash -c \
  "cd /project && \
   (dkp-)pacman -Syu --noconfirm && \
   (dkp-)pacman -S --noconfirm wut wiiu-sdl2 wiiu-sdl2_ttf wiiu-curl wiiu-mbedtls wiiu-zlib && \
   make"
```

## Installation

1. **Copy the app** — place `discord-wiiu.wuhb` at `SD:/wiiu/apps/discord-wiiu/discord-wiiu.wuhb`

2. **Token file** — create `SD:/wiiu/discord_token.txt`

   For a **user account** (personal use):
   ```
   YOUR_USER_TOKEN_HERE
   ```
   To find your token: open Discord in a browser → F12 → Network tab → send any message → find a request with an `Authorization` header and copy its value.

   For a **bot account**, prefix with `Bot `:
   ```
   Bot YOUR_BOT_TOKEN_HERE
   ```

   > **Warning:** Sharing or committing your token gives full account access. Keep this file off any public repository.

3. **Font file** — place any `.ttf` font at `SD:/wiiu/discord_wiiu/font.ttf`

   Recommended free fonts with broad Unicode coverage:
   - [Noto Sans](https://fonts.google.com/noto/specimen/Noto+Sans)
   - [DejaVu Sans](https://dejavu-fonts.github.io/)

4. Launch **Discord Wii U** from the Aroma Homebrew Launcher.

### SD card layout

```
SD:/
├── wiiu/
│   └── apps/
│       └── discord-wiiu/
│           └── discord-wiiu.wuhb
└── wiiu/
    ├── discord_token.txt          ← your token (keep private)
    └── discord_wiiu/
        ├── font.ttf               ← required font
        ├── cache/                 ← channel list cache (auto-created)
        ├── avatars/               ← avatar image cache (auto-created)
        └── media/                 ← attachment image cache (auto-created)
```

## Controls

### Navigation

| Input | Action |
|-------|--------|
| D-Pad ↑ / ↓ | Move selection up / down |
| D-Pad ← | Focus server list from channel list |
| D-Pad → | Focus channel list from server list |
| A | Select highlighted server or channel |
| B | Go back one panel |
| ZL / ZR (hold) | Scroll message history |
| − (Minus) | Open Direct Messages |

### Typing

Press **Y** (or tap the input bar on the GamePad) to open the keyboard.

| Input | Action |
|-------|--------|
| GamePad touch | Type using the on-screen keyboard |
| USB keyboard | Type characters directly |
| **CAPS** | Toggle caps lock |
| **123** | Switch to numbers / punctuation layer |
| **SYM** | Switch to extended symbols layer |
| **ABC** | Return to letters layer |
| **DEL** | Delete one character (UTF-8 aware) |
| **SEND** / USB Enter | Send message |
| B / USB Escape | Cancel and close input |

#### Keyboard layers

| Layer | Contents |
|-------|----------|
| **Letters** (default) | a–z / A–Z (QWERTY) |
| **Numbers** (`123`) | `1–0`, `! @ # $ % ^ & * ( )`, `- _ = + . , ? ' " /` |
| **Symbols** (`SYM`) | `: ; ~ ` \| \ < > [ ] { }`, `€ £ ¥ © ® ™ → ✓`, `♥ ★ ← ↑ ↓ ≠ ≤ ≥ × ÷` |

## Architecture

```
src/
├── main.cpp                    Entry point, system init
├── discord/
│   ├── types.h                 User, Guild, Channel, Message, Attachment structs
│   ├── event_queue.h           Lock-free gateway event queue
│   ├── net_mutex.{h,cpp}       Global HTTP mutex (one curl transfer at a time)
│   ├── rest.{h,cpp}            Discord REST API via libcurl
│   ├── gateway.{h,cpp}         Discord Gateway WebSocket (curl ≥ 7.86)
│   └── client.{h,cpp}         State manager, async worker, event handlers
└── ui/
    ├── theme.h                 Colors, layout constants, font sizes
    ├── renderer.{h,cpp}        SDL2 drawing primitives + SDL_ttf text
    └── app.{h,cpp}             Main loop, all UI panels, touch keyboard

vendor/
└── cJSON/                      Embedded JSON parser (MIT)
```

### Threading model

| Thread | Responsibility |
|--------|----------------|
| **Main** | SDL render loop, VPAD input, `client->poll()` |
| **Gateway** | WebSocket recv loop + heartbeat timer |
| **Channel worker** | Async channel list / message / DM fetches |
| **Avatar/media worker** | Background PNG downloads and decoding |

Gateway events are pushed into a thread-safe `EventQueue` and drained on the main thread by `client->poll()`. UI rendering never blocks on network I/O.

> **Heap note:** The Wii U's newlib `malloc` is not thread-safe. All gateway event handlers use zero-allocation raw byte scanners instead of cJSON to avoid concurrent heap corruption between the gateway thread and the main thread's rendering allocations.

## Notes

- **SSL:** Peer verification is disabled (`CURLOPT_SSL_VERIFYPEER = 0`) for compatibility with the Wii U environment. Be cautious on untrusted networks.
- **libcurl version:** Requires curl ≥ 7.86.0 for built-in WebSocket support (`CURLOPT_CONNECT_ONLY = 2L`).
- **Discord intents:** `GUILDS | GUILD_MESSAGES | DIRECT_MESSAGES | MESSAGE_CONTENT`. The `MESSAGE_CONTENT` intent is privileged — bot accounts must enable it in the [Discord Developer Portal](https://discord.com/developers/applications).
- **User tokens:** Using a user account token is against Discord's Terms of Service for automated clients. This project is intended for personal, interactive use only.
- **Caching:** Channel lists are cached to SD card with a 4-hour TTL; avatars and media images persist until manually cleared.

## License

MIT — see [LICENSE](LICENSE). Vendor dependency `cJSON` is also MIT.
