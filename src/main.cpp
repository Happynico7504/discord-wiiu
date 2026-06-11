#include <cstdio>
#include <cstring>
#include <string>
#include <memory>

#include <wut.h>
#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <nn/ac.h>
#include <nsysnet/nssl.h>
#include <curl/curl.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "discord/client.h"
#include "ui/app.h"

static std::string load_token() {
    static const char *paths[] = {
        "/vol/external01/wiiu/discord_token.txt",
        "/vol/external01/discord_token.txt",
        nullptr
    };
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        char buf[512] = {};
        if (fgets(buf, sizeof(buf), f)) {
            fclose(f);
            std::string token(buf);
            while (!token.empty() &&
                   (token.back() == '\n' || token.back() == '\r' ||
                    token.back() == ' '  || token.back() == '\t'))
                token.pop_back();
            return token;
        }
        fclose(f);
    }
    return {};
}

// Returns 0 on clean exit
static int run_app() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
        WHBLogPrintf("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    if (TTF_Init() < 0) {
        WHBLogPrintf("TTF_Init: %s", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    // Load token from SD card. If absent, App starts in Remote Auth (QR login) mode.
    std::string token = load_token();

    std::shared_ptr<Discord::Client> client;
    if (!token.empty())
        client = std::make_shared<Discord::Client>(token);

    {
        UI::App app(client);

        if (client && !client->init()) {
            WHBLogPrint("Discord client init failed");
            TTF_Quit();
            SDL_Quit();
            return 1;
        }

        app.run();

        // Shutdown whichever client is active (may have been created during login)
        auto active = app.get_client();
        if (active) active->shutdown();
    }

    TTF_Quit();
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv) {
    WHBProcInit();
    WHBLogUdpInit();
    WHBLogPrint("Discord Wii U v0.2.2 starting");

    // Network
    ACInitialize();
    ACConnect();

    // SSL
    NSSLInit();

    // curl
    curl_global_init(CURL_GLOBAL_ALL);

    int ret = run_app();

    curl_global_cleanup();
    NSSLFinish();
    ACFinalize();

    WHBLogPrint("Discord Wii U exiting");
    WHBLogUdpDeinit();
    WHBProcShutdown();
    return ret;
}
