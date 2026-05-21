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

    std::string token = load_token();
    if (token.empty()) {
        WHBLogPrint("No Discord token found.");
        WHBLogPrint("Place your token in /vol/external01/wiiu/discord_token.txt");
        // Show error on screen for a few seconds via a minimal SDL window
        SDL_Window *w = SDL_CreateWindow("Discord Wii U",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, 0);
        if (w) {
            SDL_Renderer *r = SDL_CreateRenderer(w, -1, 0);
            if (r) {
                SDL_SetRenderDrawColor(r, 32, 34, 37, 255);
                SDL_RenderClear(r);
                SDL_RenderPresent(r);
                SDL_Delay(3000);
                SDL_DestroyRenderer(r);
            }
            SDL_DestroyWindow(w);
        }
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    {
        auto client = std::make_shared<Discord::Client>(token);
        UI::App app(client);

        if (!client->init()) {
            WHBLogPrint("Discord client init failed");
            TTF_Quit();
            SDL_Quit();
            return 1;
        }

        app.run();
        client->shutdown();
    }

    TTF_Quit();
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv) {
    WHBProcInit();
    WHBLogUdpInit();
    WHBLogPrint("Discord Wii U v1.0.0 starting");

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
