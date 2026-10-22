// Internal declarations shared by the user32 implementation files.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include "dish/platform.h"

namespace dish::win {

enum : uint32_t {
    WS_POPUP = 0x80000000u, WS_CHILD = 0x40000000u, WS_MINIMIZE = 0x20000000u, WS_VISIBLE = 0x10000000u, WS_DISABLED = 0x08000000u,
    WS_MAXIMIZE = 0x01000000u, WS_CAPTION = 0x00C00000u, WS_BORDER = 0x00800000u, WS_DLGFRAME = 0x00400000u, WS_SYSMENU = 0x00080000u,
    WS_THICKFRAME = 0x00040000u, WS_MINIMIZEBOX = 0x00020000u, WS_MAXIMIZEBOX = 0x00010000u,
    WS_EX_TOPMOST = 0x8, WS_EX_TOOLWINDOW = 0x80, WS_EX_APPWINDOW = 0x40000, WS_EX_NOACTIVATE = 0x08000000,
    CW_USEDEFAULT = 0x80000000u,
};
enum : uint32_t {
    WM_NULL = 0, WM_CREATE = 1, WM_DESTROY = 2, WM_MOVE = 3, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_KILLFOCUS = 8, WM_ENABLE = 0xA,
    WM_PAINT = 0xF, WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_ERASEBKGND = 0x14, WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1C, WM_SETCURSOR = 0x20,
    WM_MOUSEACTIVATE = 0x21, WM_GETMINMAXINFO = 0x24, WM_WINDOWPOSCHANGING = 0x46, WM_WINDOWPOSCHANGED = 0x47, WM_NCCREATE = 0x81,
    WM_NCDESTROY = 0x82, WM_NCCALCSIZE = 0x83, WM_NCHITTEST = 0x84, WM_NCACTIVATE = 0x86, WM_GETTEXT = 0xD, WM_GETTEXTLENGTH = 0xE, WM_SETTEXT = 0xC,
    WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105, WM_SYSCHAR = 0x106, WM_SYSCOMMAND = 0x112,
    WM_TIMER = 0x113, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_LBUTTONDBLCLK = 0x203, WM_RBUTTONDOWN = 0x204,
    WM_RBUTTONUP = 0x205, WM_RBUTTONDBLCLK = 0x206, WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208, WM_MBUTTONDBLCLK = 0x209, WM_MOUSEWHEEL = 0x20A,
    WM_XBUTTONDOWN = 0x20B, WM_XBUTTONUP = 0x20C, WM_XBUTTONDBLCLK = 0x20D, WM_MOUSELEAVE = 0x2A3, WM_DEVICECHANGE = 0x219, WM_INPUT = 0xFF,
    WM_USER = 0x400,
};

struct WndClass {
    uint16_t atom = 0;
    std::string name;           // as registered (UTF-8)
    uint32_t style = 0, wndproc = 0, cls_extra = 0, wnd_extra = 0, hinst = 0, icon = 0, cursor = 0, brush = 0, menu = 0;
    std::vector<uint8_t> extra;
};
struct Window {
    uint32_t hwnd = 0;
    std::shared_ptr<WndClass> cls;
    uint32_t wndproc = 0;
    uint32_t style = 0, exstyle = 0, id = 0, hinst = 0, parent = 0, owner = 0;
    int32_t x = 0, y = 0, w = 0, h = 0;          // window rect (screen coordinates, outer size)
    int32_t cx = 0, cy = 0;                      // client size
    int32_t cl_left = 0, cl_top = 0;             // client origin relative to the window origin
    std::string title;
    bool visible = false, destroyed = false, enabled = true, minimized = false, maximized = false, ever_shown = false;
    uint32_t tid = 0;                            // creating (owning) guest thread
    std::vector<uint8_t> extra;
    int32_t userdata = 0;
    SDL_Window *sdl = nullptr;
    SDL_WindowID sdl_id = 0;
    bool created_msg_done = false;
};
using WinRef = std::shared_ptr<Window>;

struct Msg { uint32_t hwnd = 0, msg = 0, wparam = 0, lparam = 0, time = 0; int32_t x = 0, y = 0; };

extern std::mutex g_mu;                          // window table, classes, timers
WinRef get_window(uint32_t hwnd);
WinRef window_for_sdl(SDL_WindowID id);
std::vector<WinRef> all_windows();
void post_msg(uint32_t tid, const Msg &m);
void post_to_window(uint32_t hwnd, uint32_t msg, uint32_t w, uint32_t l);
uint32_t call_wndproc(uint32_t esp, const WinRef &w, uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t tick_ms();
void on_main(const std::function<void()> &f);
void compute_frame(uint32_t style, uint32_t exstyle, bool menu, int *l, int *t, int *r, int *b);

// display info (cached on first use, main thread)
struct DispMode { int w, h, bpp, hz; };
struct DisplayInfo { int x = 0, y = 0, w = 1920, h = 1080, hz = 60; std::string name; std::vector<DispMode> modes; bool valid = false; };
const DisplayInfo &primary_display();

// keyboard tables
int sdl_scancode_to_vk(int sc);
int sdl_scancode_to_set1(int sc, bool *extended);
int vk_to_set1_scan(int vk, bool *extended);
int set1_scan_to_vk(int scan, bool extended);

// input helpers
void set_focus_window(uint32_t hwnd);
extern std::atomic<uint32_t> g_focus, g_active, g_capture;

}  // namespace dish::win
