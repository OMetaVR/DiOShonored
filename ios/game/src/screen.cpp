#include "screen.h"
#include <algorithm>

bool TextScreen::open() {
    if (win_) return true;
    if (!SDL_CreateWindowAndRenderer("Dishonored", 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY, &win_, &ren_)) {
        SDL_Log("TextScreen: cannot create window: %s", SDL_GetError());
        win_ = nullptr;
        ren_ = nullptr;
        return false;
    }
    return true;
}

void TextScreen::close() {
    if (ren_) SDL_DestroyRenderer(ren_);
    if (win_) SDL_DestroyWindow(win_);
    ren_ = nullptr;
    win_ = nullptr;
}

// word-wrap `text` to `cols` columns
static std::vector<std::string> wrap(const std::string &text, int cols) {
    std::vector<std::string> out;
    std::string rest = text;
    while (!rest.empty()) {
        if ((int)rest.size() <= cols) { out.push_back(rest); break; }
        size_t cut = rest.rfind(' ', (size_t)cols);
        if (cut == std::string::npos || cut == 0) cut = (size_t)cols;
        out.push_back(rest.substr(0, cut));
        rest.erase(0, std::min(rest.size(), cut + (rest[cut] == ' ' ? 1 : 0)));
    }
    return out;
}

void TextScreen::draw(const std::vector<ScreenLine> &lines) {
    if (!ren_) return;
    int pw = 0, ph = 0;
    SDL_GetCurrentRenderOutputSize(ren_, &pw, &ph);
    // leave room for the notch / home indicator
    SDL_Rect safe{0, 0, pw, ph};
    if (win_) {
        SDL_Rect sr;
        if (SDL_GetWindowSafeArea(win_, &sr)) {
            float d = SDL_GetWindowPixelDensity(win_);
            safe = SDL_Rect{(int)(sr.x * d), (int)(sr.y * d), (int)(sr.w * d), (int)(sr.h * d)};
        }
    }
    // the largest scale (8 px glyphs, ~56 columns at most) at which every wrapped line fits the safe area's height
    float scale = std::max(1.0f, (float)safe.w / (8.0f * 56.0f));
    for (; scale > 1.0f; scale -= 0.25f) {
        int cols = std::max(16, (int)((float)safe.w / scale / 8.0f) - 2);
        float h = 28.0f;
        for (const ScreenLine &ln : lines) h += ln.text.empty() ? 6.0f : 11.0f * (float)wrap(ln.text, cols).size() + 3.0f;
        if (h * scale <= (float)safe.h) break;
    }
    scale = std::max(1.0f, scale);
    SDL_SetRenderDrawColor(ren_, 18, 18, 24, 255);
    SDL_RenderClear(ren_);
    SDL_SetRenderScale(ren_, scale, scale);
    const int cols = std::max(16, (int)((float)safe.w / scale / 8.0f) - 2);
    const float x0 = 8.0f + (float)safe.x / scale;
    float y = 14.0f + (float)safe.y / scale;
    for (const ScreenLine &ln : lines) {
        SDL_SetRenderDrawColor(ren_, ln.color.r, ln.color.g, ln.color.b, 255);
        if (ln.text.empty()) { y += 6.0f; continue; }
        for (const std::string &part : wrap(ln.text, cols)) {
            SDL_RenderDebugText(ren_, x0, y, part.c_str());
            y += 11.0f;
        }
        y += 3.0f;
    }
    SDL_SetRenderScale(ren_, 1.0f, 1.0f);
    SDL_RenderPresent(ren_);
}
