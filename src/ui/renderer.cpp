#include "renderer.h"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <sstream>
#include <whb/log.h>

namespace UI {

bool Renderer::init(SDL_Renderer *r) {
    renderer_ = r;
    return true;
}

void Renderer::destroy() {
    if (font_sm)   { TTF_CloseFont(font_sm);   font_sm   = nullptr; }
    if (font_md)   { TTF_CloseFont(font_md);   font_md   = nullptr; }
    if (font_lg)   { TTF_CloseFont(font_lg);   font_lg   = nullptr; }
    if (font_bold) { TTF_CloseFont(font_bold); font_bold = nullptr; }
}

void Renderer::fill_rect(int x, int y, int w, int h, SDL_Color col) {
    SDL_SetRenderDrawColor(renderer_, col.r, col.g, col.b, col.a);
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderFillRect(renderer_, &rect);
}

void Renderer::draw_rect(int x, int y, int w, int h, SDL_Color col, int thickness) {
    SDL_SetRenderDrawColor(renderer_, col.r, col.g, col.b, col.a);
    for (int i = 0; i < thickness; i++) {
        SDL_Rect rect = { x + i, y + i, w - 2*i, h - 2*i };
        SDL_RenderDrawRect(renderer_, &rect);
    }
}

void Renderer::fill_rounded_rect(int x, int y, int w, int h, int r, SDL_Color col) {
    // Simple filled rounded rect via three rects + four circle corners
    SDL_SetRenderDrawColor(renderer_, col.r, col.g, col.b, col.a);
    SDL_Rect center = { x + r, y,     w - 2*r, h };
    SDL_Rect left   = { x,     y + r, r,       h - 2*r };
    SDL_Rect right  = { x + w - r, y + r, r,   h - 2*r };
    SDL_RenderFillRect(renderer_, &center);
    SDL_RenderFillRect(renderer_, &left);
    SDL_RenderFillRect(renderer_, &right);
    fill_circle(x + r,       y + r,       r, col);
    fill_circle(x + w - r,   y + r,       r, col);
    fill_circle(x + r,       y + h - r,   r, col);
    fill_circle(x + w - r,   y + h - r,   r, col);
}

void Renderer::fill_circle(int cx, int cy, int radius, SDL_Color col) {
    SDL_SetRenderDrawColor(renderer_, col.r, col.g, col.b, col.a);
    int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)sqrt((double)(r2 - dy * dy));
        SDL_RenderDrawLine(renderer_, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

int Renderer::draw_text(int x, int y, const std::string &text, SDL_Color col, TTF_Font *font) {
    if (!font) font = font_md;
    if (!font || text.empty()) return 0;

    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text.c_str(), col);
    if (!surf) return 0;

    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer_, surf);
    int w = surf->w, h = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) return 0;

    SDL_Rect dst = { x, y, w, h };
    SDL_RenderCopy(renderer_, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    return w;
}

int Renderer::text_width(const std::string &text, TTF_Font *font) {
    if (!font) font = font_md;
    if (!font || text.empty()) return 0;
    int w = 0, h = 0;
    TTF_SizeUTF8(font, text.c_str(), &w, &h);
    return w;
}

int Renderer::text_height(TTF_Font *font) {
    if (!font) font = font_md;
    if (!font) return 16;
    return TTF_FontHeight(font);
}

std::vector<std::string> Renderer::wrap_text(const std::string &text, int max_width, TTF_Font *font) {
    if (!font) font = font_md;
    std::vector<std::string> lines;
    if (!font || text.empty()) return lines;

    // Split by existing newlines first
    std::string remaining = text;
    std::string current_line;

    auto flush_word_wrap = [&](const std::string &para) {
        if (para.empty()) { lines.push_back(""); return; }
        std::istringstream iss(para);
        std::string word;
        std::string line;
        while (iss >> word) {
            std::string test = line.empty() ? word : (line + " " + word);
            int w = 0, h = 0;
            TTF_SizeUTF8(font, test.c_str(), &w, &h);
            if (w <= max_width) {
                line = test;
            } else {
                if (!line.empty()) lines.push_back(line);
                // Check if single word fits
                TTF_SizeUTF8(font, word.c_str(), &w, &h);
                if (w > max_width) {
                    // Force-break long word
                    std::string partial;
                    for (char c : word) {
                        std::string test2 = partial + c;
                        TTF_SizeUTF8(font, test2.c_str(), &w, &h);
                        if (w > max_width) { lines.push_back(partial); partial = std::string(1, c); }
                        else partial = test2;
                    }
                    line = partial;
                } else {
                    line = word;
                }
            }
        }
        if (!line.empty()) lines.push_back(line);
    };

    size_t pos = 0;
    while (pos <= remaining.size()) {
        size_t nl = remaining.find('\n', pos);
        if (nl == std::string::npos) {
            flush_word_wrap(remaining.substr(pos));
            break;
        }
        flush_word_wrap(remaining.substr(pos, nl - pos));
        pos = nl + 1;
    }

    return lines;
}

} // namespace UI
