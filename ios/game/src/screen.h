// A tiny text screen over SDL3's built-in debug font: the import / status screen needs no assets and no UIKit code.
#pragma once
#include <SDL3/SDL.h>
#include <string>
#include <vector>

struct ScreenLine {
    std::string text;
    SDL_Color color{220, 220, 220, 255};
};

class TextScreen {
public:
    bool open();                      // creates a fullscreen window + renderer on the calling (UIKit main) thread
    void close();
    bool is_open() const { return win_ != nullptr; }
    void draw(const std::vector<ScreenLine> &lines);   // word-wraps to the current width
    SDL_WindowID window_id() const { return win_ ? SDL_GetWindowID(win_) : 0; }

private:
    SDL_Window *win_ = nullptr;
    SDL_Renderer *ren_ = nullptr;
};

inline ScreenLine L(std::string t, SDL_Color c = {220, 220, 220, 255}) { return ScreenLine{std::move(t), c}; }
constexpr SDL_Color kTitle{255, 210, 120, 255}, kGood{120, 230, 140, 255}, kBad{255, 120, 110, 255}, kDim{150, 150, 160, 255};
