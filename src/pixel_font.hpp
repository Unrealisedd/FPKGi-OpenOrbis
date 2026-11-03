#pragma once

#include <SDL2/SDL.h>
#include <string>

namespace fpkgi {
void draw_text(SDL_Renderer* renderer, int x, int y, int scale, const std::string& text, SDL_Color color);
int text_width(int scale, const std::string& text);
}
