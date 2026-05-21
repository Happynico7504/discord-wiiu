#pragma once
#include <string>
#include <vector>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "theme.h"

namespace UI {

class Renderer {
public:
    Renderer() = default;
    ~Renderer() { destroy(); }

    bool init(SDL_Renderer *r);
    void destroy();

    // Primitives
    void fill_rect(int x, int y, int w, int h, SDL_Color col);
    void draw_rect(int x, int y, int w, int h, SDL_Color col, int thickness = 1);
    void fill_rounded_rect(int x, int y, int w, int h, int radius, SDL_Color col);
    void fill_circle(int cx, int cy, int radius, SDL_Color col);

    // Text (returns rendered width)
    int  draw_text(int x, int y, const std::string &text, SDL_Color col,
                   TTF_Font *font = nullptr);
    int  text_width(const std::string &text, TTF_Font *font = nullptr);
    int  text_height(TTF_Font *font = nullptr);

    // Word-wrap text into lines that fit within max_width pixels
    std::vector<std::string> wrap_text(const std::string &text, int max_width,
                                       TTF_Font *font = nullptr);

    TTF_Font *font_sm = nullptr;
    TTF_Font *font_md = nullptr;
    TTF_Font *font_lg = nullptr;
    TTF_Font *font_bold = nullptr;

private:
    SDL_Renderer *renderer_ = nullptr;
};

} // namespace UI
