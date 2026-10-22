// SDL scancode <-> Windows virtual key <-> PS/2 set-1 scan code tables.
#include "win_internal.h"

namespace dish::win {
namespace {
struct Key { int sc; int vk; int scan; bool ext; };
const Key kKeys[] = {
    {SDL_SCANCODE_A, 'A', 0x1E, 0}, {SDL_SCANCODE_B, 'B', 0x30, 0}, {SDL_SCANCODE_C, 'C', 0x2E, 0}, {SDL_SCANCODE_D, 'D', 0x20, 0},
    {SDL_SCANCODE_E, 'E', 0x12, 0}, {SDL_SCANCODE_F, 'F', 0x21, 0}, {SDL_SCANCODE_G, 'G', 0x22, 0}, {SDL_SCANCODE_H, 'H', 0x23, 0},
    {SDL_SCANCODE_I, 'I', 0x17, 0}, {SDL_SCANCODE_J, 'J', 0x24, 0}, {SDL_SCANCODE_K, 'K', 0x25, 0}, {SDL_SCANCODE_L, 'L', 0x26, 0},
    {SDL_SCANCODE_M, 'M', 0x32, 0}, {SDL_SCANCODE_N, 'N', 0x31, 0}, {SDL_SCANCODE_O, 'O', 0x18, 0}, {SDL_SCANCODE_P, 'P', 0x19, 0},
    {SDL_SCANCODE_Q, 'Q', 0x10, 0}, {SDL_SCANCODE_R, 'R', 0x13, 0}, {SDL_SCANCODE_S, 'S', 0x1F, 0}, {SDL_SCANCODE_T, 'T', 0x14, 0},
    {SDL_SCANCODE_U, 'U', 0x16, 0}, {SDL_SCANCODE_V, 'V', 0x2F, 0}, {SDL_SCANCODE_W, 'W', 0x11, 0}, {SDL_SCANCODE_X, 'X', 0x2D, 0},
    {SDL_SCANCODE_Y, 'Y', 0x15, 0}, {SDL_SCANCODE_Z, 'Z', 0x2C, 0},
    {SDL_SCANCODE_1, '1', 0x02, 0}, {SDL_SCANCODE_2, '2', 0x03, 0}, {SDL_SCANCODE_3, '3', 0x04, 0}, {SDL_SCANCODE_4, '4', 0x05, 0},
    {SDL_SCANCODE_5, '5', 0x06, 0}, {SDL_SCANCODE_6, '6', 0x07, 0}, {SDL_SCANCODE_7, '7', 0x08, 0}, {SDL_SCANCODE_8, '8', 0x09, 0},
    {SDL_SCANCODE_9, '9', 0x0A, 0}, {SDL_SCANCODE_0, '0', 0x0B, 0},
    {SDL_SCANCODE_RETURN, 0x0D, 0x1C, 0}, {SDL_SCANCODE_ESCAPE, 0x1B, 0x01, 0}, {SDL_SCANCODE_BACKSPACE, 0x08, 0x0E, 0},
    {SDL_SCANCODE_TAB, 0x09, 0x0F, 0}, {SDL_SCANCODE_SPACE, 0x20, 0x39, 0}, {SDL_SCANCODE_MINUS, 0xBD, 0x0C, 0},
    {SDL_SCANCODE_EQUALS, 0xBB, 0x0D, 0}, {SDL_SCANCODE_LEFTBRACKET, 0xDB, 0x1A, 0}, {SDL_SCANCODE_RIGHTBRACKET, 0xDD, 0x1B, 0},
    {SDL_SCANCODE_BACKSLASH, 0xDC, 0x2B, 0}, {SDL_SCANCODE_NONUSHASH, 0xDC, 0x2B, 0}, {SDL_SCANCODE_SEMICOLON, 0xBA, 0x27, 0},
    {SDL_SCANCODE_APOSTROPHE, 0xDE, 0x28, 0}, {SDL_SCANCODE_GRAVE, 0xC0, 0x29, 0}, {SDL_SCANCODE_COMMA, 0xBC, 0x33, 0},
    {SDL_SCANCODE_PERIOD, 0xBE, 0x34, 0}, {SDL_SCANCODE_SLASH, 0xBF, 0x35, 0}, {SDL_SCANCODE_CAPSLOCK, 0x14, 0x3A, 0},
    {SDL_SCANCODE_F1, 0x70, 0x3B, 0}, {SDL_SCANCODE_F2, 0x71, 0x3C, 0}, {SDL_SCANCODE_F3, 0x72, 0x3D, 0}, {SDL_SCANCODE_F4, 0x73, 0x3E, 0},
    {SDL_SCANCODE_F5, 0x74, 0x3F, 0}, {SDL_SCANCODE_F6, 0x75, 0x40, 0}, {SDL_SCANCODE_F7, 0x76, 0x41, 0}, {SDL_SCANCODE_F8, 0x77, 0x42, 0},
    {SDL_SCANCODE_F9, 0x78, 0x43, 0}, {SDL_SCANCODE_F10, 0x79, 0x44, 0}, {SDL_SCANCODE_F11, 0x7A, 0x57, 0}, {SDL_SCANCODE_F12, 0x7B, 0x58, 0},
    {SDL_SCANCODE_PRINTSCREEN, 0x2C, 0x37, 1}, {SDL_SCANCODE_SCROLLLOCK, 0x91, 0x46, 0}, {SDL_SCANCODE_PAUSE, 0x13, 0x45, 0},
    {SDL_SCANCODE_INSERT, 0x2D, 0x52, 1}, {SDL_SCANCODE_HOME, 0x24, 0x47, 1}, {SDL_SCANCODE_PAGEUP, 0x21, 0x49, 1},
    {SDL_SCANCODE_DELETE, 0x2E, 0x53, 1}, {SDL_SCANCODE_END, 0x23, 0x4F, 1}, {SDL_SCANCODE_PAGEDOWN, 0x22, 0x51, 1},
    {SDL_SCANCODE_RIGHT, 0x27, 0x4D, 1}, {SDL_SCANCODE_LEFT, 0x25, 0x4B, 1}, {SDL_SCANCODE_DOWN, 0x28, 0x50, 1}, {SDL_SCANCODE_UP, 0x26, 0x48, 1},
    {SDL_SCANCODE_NUMLOCKCLEAR, 0x90, 0x45, 0}, {SDL_SCANCODE_KP_DIVIDE, 0x6F, 0x35, 1}, {SDL_SCANCODE_KP_MULTIPLY, 0x6A, 0x37, 0},
    {SDL_SCANCODE_KP_MINUS, 0x6D, 0x4A, 0}, {SDL_SCANCODE_KP_PLUS, 0x6B, 0x4E, 0}, {SDL_SCANCODE_KP_ENTER, 0x0D, 0x1C, 1},
    {SDL_SCANCODE_KP_1, 0x61, 0x4F, 0}, {SDL_SCANCODE_KP_2, 0x62, 0x50, 0}, {SDL_SCANCODE_KP_3, 0x63, 0x51, 0}, {SDL_SCANCODE_KP_4, 0x64, 0x4B, 0},
    {SDL_SCANCODE_KP_5, 0x65, 0x4C, 0}, {SDL_SCANCODE_KP_6, 0x66, 0x4D, 0}, {SDL_SCANCODE_KP_7, 0x67, 0x47, 0}, {SDL_SCANCODE_KP_8, 0x68, 0x48, 0},
    {SDL_SCANCODE_KP_9, 0x69, 0x49, 0}, {SDL_SCANCODE_KP_0, 0x60, 0x52, 0}, {SDL_SCANCODE_KP_PERIOD, 0x6E, 0x53, 0},
    {SDL_SCANCODE_NONUSBACKSLASH, 0xE2, 0x56, 0}, {SDL_SCANCODE_APPLICATION, 0x5D, 0x5D, 1},
    {SDL_SCANCODE_LCTRL, 0xA2, 0x1D, 0}, {SDL_SCANCODE_LSHIFT, 0xA0, 0x2A, 0}, {SDL_SCANCODE_LALT, 0xA4, 0x38, 0}, {SDL_SCANCODE_LGUI, 0x5B, 0x5B, 1},
    {SDL_SCANCODE_RCTRL, 0xA3, 0x1D, 1}, {SDL_SCANCODE_RSHIFT, 0xA1, 0x36, 0}, {SDL_SCANCODE_RALT, 0xA5, 0x38, 1}, {SDL_SCANCODE_RGUI, 0x5C, 0x5C, 1},
};
}  // namespace

int sdl_scancode_to_vk(int sc) {
    static int table[SDL_SCANCODE_COUNT];
    static bool init = false;
    if (!init) {
        for (auto &k : kKeys) table[k.sc] = k.vk;
        init = true;
    }
    return sc >= 0 && sc < SDL_SCANCODE_COUNT ? table[sc] : 0;
}
int sdl_scancode_to_set1(int sc, bool *ext) {
    for (auto &k : kKeys)
        if (k.sc == sc) { *ext = k.ext; return k.scan; }
    *ext = false;
    return 0;
}
int vk_to_set1_scan(int vk, bool *ext) {
    // generic VK_SHIFT/CONTROL/MENU map to the left keys
    if (vk == 0x10) vk = 0xA0;
    if (vk == 0x11) vk = 0xA2;
    if (vk == 0x12) vk = 0xA4;
    for (auto &k : kKeys)
        if (k.vk == vk) { if (ext) *ext = k.ext; return k.scan; }
    if (ext) *ext = false;
    return 0;
}
int set1_scan_to_vk(int scan, bool ext) {
    for (auto &k : kKeys)
        if (k.scan == scan && k.ext == ext) return k.vk;
    for (auto &k : kKeys)
        if (k.scan == scan) return k.vk;
    return 0;
}

}  // namespace dish::win
