// user32: window classes, windows, message queue, dispatch, SDL event translation.
#include "win_internal.h"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "dish/call.h"
#include "dish/heap.h"
#include "dish/hle.h"
#include "dish/lifecycle.h"
#include "dish/log.h"
#include "dish/memacct.h"
#include "dish/mainthread.h"
#include "dish/thread.h"
#include "dish/util.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>
#include <string>

namespace dish {

std::atomic<uint8_t> g_vk_down[256];
MouseAccum g_mouse;
std::atomic<int32_t> g_cursor_x{960}, g_cursor_y{540};

namespace {
std::mutex g_mev_mu;
std::deque<MouseEvent> g_mev;
uint32_t g_mev_seq = 0;
}  // namespace
void input_mouse_push(uint32_t ofs, uint32_t data) {
    std::lock_guard<std::mutex> lk(g_mev_mu);
    if (g_mev.size() >= 4096) g_mev.pop_front();
    g_mev.push_back({ofs, data, win::tick_ms(), ++g_mev_seq});
}
size_t input_mouse_pop(MouseEvent *out, size_t max, bool peek) {
    std::lock_guard<std::mutex> lk(g_mev_mu);
    size_t n = std::min(max, g_mev.size());
    for (size_t i = 0; i < n; i++) out[i] = g_mev[i];
    if (!peek) g_mev.erase(g_mev.begin(), g_mev.begin() + (long)n);
    return n;
}
void input_mouse_clear() {
    std::lock_guard<std::mutex> lk(g_mev_mu);
    g_mev.clear();
}

namespace win {

std::mutex g_mu;
static std::map<uint32_t, WinRef> g_windows;
static std::map<SDL_WindowID, uint32_t> g_sdl_ids;
static std::vector<std::shared_ptr<WndClass>> g_classes;
static uint16_t g_next_atom = 0xC001;
static uint32_t g_next_hwnd = 0x00010010;
std::atomic<uint32_t> g_focus{0}, g_active{0}, g_capture{0};
static std::atomic<bool> g_app_active{false};

struct Timer { uint32_t hwnd, id, ms, proc, tid; uint64_t next; };
static std::vector<Timer> g_timers;
static std::atomic<int> g_timer_count{0};

struct Queue {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Msg> q;
    std::atomic<int> count{0};
    bool quit = false;
    int quit_code = 0;
};
static std::map<uint32_t, std::unique_ptr<Queue>> g_queues;

static Queue *queue_for(uint32_t tid) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto &p = g_queues[tid];
    if (!p) p = std::make_unique<Queue>();
    return p.get();
}
static Queue *my_queue() {
    static thread_local Queue *q = nullptr;
    static thread_local uint32_t tid = 0;
    if (!q || tid != cur_tid()) { tid = cur_tid(); q = queue_for(tid); }
    return q;
}

uint32_t tick_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000) + 600000u;
}
static uint64_t now_ms64() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void on_main(const std::function<void()> &f) { mainthread_call(f); }

WinRef get_window(uint32_t hwnd) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_windows.find(hwnd);
    return it == g_windows.end() ? nullptr : it->second;
}
WinRef window_for_sdl(SDL_WindowID id) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sdl_ids.find(id);
    if (it == g_sdl_ids.end()) return nullptr;
    auto w = g_windows.find(it->second);
    return w == g_windows.end() ? nullptr : w->second;
}
std::vector<WinRef> all_windows() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<WinRef> v;
    for (auto &kv : g_windows) v.push_back(kv.second);
    return v;
}

void post_msg(uint32_t tid, const Msg &m) {
    Queue *q = queue_for(tid);
    {
        std::lock_guard<std::mutex> lk(q->mu);
        Msg mm = m;
        if (!mm.time) mm.time = tick_ms();
        mm.x = g_cursor_x;
        mm.y = g_cursor_y;
        q->q.push_back(mm);
        q->count++;
    }
    q->cv.notify_all();
}
void post_to_window(uint32_t hwnd, uint32_t msg, uint32_t w, uint32_t l) {
    WinRef win = get_window(hwnd);
    if (!win) return;
    Msg m;
    m.hwnd = hwnd; m.msg = msg; m.wparam = w; m.lparam = l;
    post_msg(win->tid, m);
}

uint32_t call_wndproc(uint32_t esp, const WinRef &w, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!w || !w->wndproc) return 0;
    return guest_invoke_from(esp, w->wndproc, Conv::Stdcall, w->hwnd, msg, wp, lp);
}

void compute_frame(uint32_t style, uint32_t exstyle, bool menu, int *l, int *t, int *r, int *b) {
    (void)exstyle;
    int border = 0, cap = 0;
    if (!(style & WS_POPUP) || (style & WS_CAPTION) == WS_CAPTION) {
        if (style & WS_THICKFRAME) border = 8;
        else if ((style & WS_CAPTION) == WS_CAPTION) border = 4;
        else if (style & WS_DLGFRAME) border = 3;
        else if (style & WS_BORDER) border = 1;
        if ((style & WS_CAPTION) == WS_CAPTION) cap = 23;
    }
    *l = *r = *b = border;
    *t = border + cap + (menu ? 20 : 0);
}

// ---- display ---------------------------------------------------------------------------------------------------
static DisplayInfo g_display;
const DisplayInfo &primary_display() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_display.valid) {
        g_display.valid = true;
        g_display.name = "\\\\.\\DISPLAY1";
        on_main([&] {
            if (!platform_video_init()) return;
            int n = 0;
            SDL_DisplayID *ids = SDL_GetDisplays(&n);
            if (!ids || n < 1) { if (ids) SDL_free(ids); return; }
            SDL_DisplayID id = ids[0];
            SDL_free(ids);
            SDL_Rect r;
            if (SDL_GetDisplayBounds(id, &r)) { g_display.x = r.x; g_display.y = r.y; g_display.w = r.w; g_display.h = r.h; }
            if (const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(id)) g_display.hz = dm->refresh_rate > 0 ? (int)(dm->refresh_rate + 0.5f) : 60;
            int cnt = 0;
            SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(id, &cnt);
            for (int i = 0; modes && i < cnt; i++)
                g_display.modes.push_back({modes[i]->w, modes[i]->h, 32, modes[i]->refresh_rate > 0 ? (int)(modes[i]->refresh_rate + 0.5f) : 60});
            if (modes) SDL_free(modes);
        });
#if defined(__APPLE__) && TARGET_OS_IPHONE
        // A phone screen in points (844x390 on an iPhone 13) is below what PC games accept (Dishonored needs 640x480) and says
        // nothing about the render size. Present a virtual desktop at the screen's aspect ratio with the Metal backend's internal
        // height (DISH_METAL_INTERNAL_HEIGHT, default 720), and the same mode list the backend offers (mtl_backend.mm), so the
        // game's default resolution is the one the backend renders at and then upscales to the drawable.
        {
            double aspect = (g_display.w > 0 && g_display.h > 0) ? (double)std::max(g_display.w, g_display.h) / std::min(g_display.w, g_display.h) : 16.0 / 9.0;
            const char *ih = getenv("DISH_METAL_INTERNAL_HEIGHT");
            int vh = ih ? atoi(ih) : 720;
            if (vh < 480) vh = 720;
            g_display.modes.clear();
            for (int h : {vh}) {   // one mode, the virtual desktop (see MtlD3D9::default_modes)
                int w = ((int)(h * aspect + 0.5)) & ~1;
                g_display.modes.push_back({w, h, 32, g_display.hz > 0 ? g_display.hz : 60});
            }
            g_display.x = 0;
            g_display.y = 0;
            g_display.w = g_display.modes[0].w;
            g_display.h = g_display.modes[0].h;
        }
#endif
        if (g_display.modes.empty()) {   // headless fallback
            for (auto m : std::initializer_list<DispMode>{{640, 480, 32, 60}, {800, 600, 32, 60}, {1280, 720, 32, 60}, {1920, 1080, 32, 60}}) g_display.modes.push_back(m);
        }
        std::sort(g_display.modes.begin(), g_display.modes.end(), [](const DispMode &a, const DispMode &b) {
            return std::tie(a.w, a.h, a.hz) < std::tie(b.w, b.h, b.hz);
        });
        g_display.modes.erase(std::unique(g_display.modes.begin(), g_display.modes.end(), [](const DispMode &a, const DispMode &b) { return a.w == b.w && a.h == b.h && a.hz == b.hz; }), g_display.modes.end());
    }
    return g_display;
}

// ---- classes -----------------------------------------------------------------------------------------------------
static std::shared_ptr<WndClass> find_class(uint32_t name_or_atom, bool wide) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (name_or_atom < 0x10000) {
        for (auto &c : g_classes) if (c->atom == name_or_atom) return c;
        return nullptr;
    }
    std::string n = lower_utf8(wide ? read_wstr_utf8(name_or_atom) : read_astr_utf8(name_or_atom));
    for (auto &c : g_classes) if (lower_utf8(c->name) == n) return c;
    return nullptr;
}
static uint32_t register_class(uint32_t wc, bool ex, bool wide) {
    // WNDCLASS: style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor, hbrBackground, lpszMenuName, lpszClassName (+ cbSize first / hIconSm last for Ex)
    uint32_t o = ex ? 4 : 0;
    auto c = std::make_shared<WndClass>();
    c->style = rd32(wc + o);
    c->wndproc = rd32(wc + o + 4);
    c->cls_extra = rd32(wc + o + 8);
    c->wnd_extra = rd32(wc + o + 12);
    c->hinst = rd32(wc + o + 16);
    c->icon = rd32(wc + o + 20);
    c->cursor = rd32(wc + o + 24);
    c->brush = rd32(wc + o + 28);
    c->menu = rd32(wc + o + 32);
    uint32_t nm = rd32(wc + o + 36);
    c->name = nm < 0x10000 ? "#" + std::to_string(nm) : (wide ? read_wstr_utf8(nm) : read_astr_utf8(nm));
    c->extra.assign(c->cls_extra, 0);
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &x : g_classes)
        if (lower_utf8(x->name) == lower_utf8(c->name)) { set_last_error(1410); return 0; }   // ERROR_CLASS_ALREADY_EXISTS
    c->atom = g_next_atom++;
    g_classes.push_back(c);
    LOGD(LC_WIN, "RegisterClass '%s' wndproc=%08x -> atom %04x", c->name.c_str(), c->wndproc, c->atom);
    return c->atom;
}
static uint32_t u32_RegisterClassW(uint32_t wc) { return register_class(wc, false, true); }
static uint32_t u32_RegisterClassA(uint32_t wc) { return register_class(wc, false, false); }
static uint32_t u32_RegisterClassExW(uint32_t wc) { return register_class(wc, true, true); }
static uint32_t u32_RegisterClassExA(uint32_t wc) { return register_class(wc, true, false); }
static uint32_t unregister_class(uint32_t name, bool wide) {
    auto c = find_class(name, wide);
    if (!c) { set_last_error(1411); return 0; }
    std::lock_guard<std::mutex> lk(g_mu);
    g_classes.erase(std::remove(g_classes.begin(), g_classes.end(), c), g_classes.end());
    return 1;
}
static uint32_t u32_UnregisterClassW(uint32_t n, uint32_t) { return unregister_class(n, true); }
static uint32_t u32_UnregisterClassA(uint32_t n, uint32_t) { return unregister_class(n, false); }
static uint32_t u32_GetClassLongA(uint32_t hwnd, int32_t idx) {
    WinRef w = get_window(hwnd);
    if (!w) { set_last_error(1400); return 0; }
    switch (idx) {
        case -26: return w->cls->style;      // GCL_STYLE
        case -24: return w->cls->wndproc;    // GCL_WNDPROC
        case -16: return w->cls->hinst;      // GCL_HMODULE
        case -14: return w->cls->icon;       // GCL_HICON
        case -12: return w->cls->cursor;     // GCL_HCURSOR
        case -10: return w->cls->brush;      // GCL_HBRBACKGROUND
        case -32: return w->cls->atom;       // GCW_ATOM
        default: return 0;
    }
}

// ---- window creation ---------------------------------------------------------------------------------------------
static bool create_sdl_window(Window &w) {
    bool ok = false;
    on_main([&] {
        if (!platform_video_init()) return;
        auto make = [&](bool vulkan) -> SDL_Window * {
            SDL_PropertiesID p = SDL_CreateProperties();
            SDL_SetStringProperty(p, SDL_PROP_WINDOW_CREATE_TITLE_STRING, w.title.c_str());
            SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, std::max(w.cx, 1));
            SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, std::max(w.cy, 1));
            SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_X_NUMBER, w.x + w.cl_left);
            SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_Y_NUMBER, w.y + w.cl_top);
            SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
#ifdef __APPLE__
            // iOS: the d3d9 backend is Metal; the window needs a CAMetalLayer view and a native-resolution drawable
            SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_METAL_BOOLEAN, true);
            SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
            (void)vulkan;
#else
            SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, vulkan);
#endif
            SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, (w.style & WS_CAPTION) != WS_CAPTION && !(w.style & WS_THICKFRAME));
            SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, (w.style & WS_THICKFRAME) != 0);
            SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_ALWAYS_ON_TOP_BOOLEAN, (w.exstyle & WS_EX_TOPMOST) != 0);
            SDL_Window *s = SDL_CreateWindowWithProperties(p);
            SDL_DestroyProperties(p);
            return s;
        };
        w.sdl = make(true);
        if (!w.sdl) { LOGW(LC_WIN, "Vulkan-capable SDL window failed (%s); retrying without", SDL_GetError()); w.sdl = make(false); }
        if (!w.sdl) { LOGE(LC_WIN, "SDL_CreateWindow failed: %s", SDL_GetError()); return; }
        w.sdl_id = SDL_GetWindowID(w.sdl);
        SDL_StartTextInput(w.sdl);
        ok = true;
    });
    return ok;
}

static uint32_t create_window(const HleCtx &c, uint32_t exstyle, uint32_t clsarg, uint32_t name, uint32_t style, int32_t x, int32_t y, int32_t cw, int32_t ch,
                              uint32_t parent, uint32_t menu, uint32_t hinst, uint32_t param, bool wide) {
    auto cls = find_class(clsarg, wide);
    if (!cls) {
        LOGW(LC_WIN, "CreateWindowEx: unknown class %s", clsarg < 0x10000 ? "(atom)" : (wide ? read_wstr_utf8(clsarg) : read_astr_utf8(clsarg)).c_str());
        set_last_error(1407);   // ERROR_CANNOT_FIND_WND_CLASS
        return 0;
    }
    auto w = std::make_shared<Window>();
    w->cls = cls;
    w->wndproc = cls->wndproc;
    w->style = style & ~WS_VISIBLE;
    w->exstyle = exstyle;
    w->parent = parent;
    w->hinst = hinst;
    w->id = (style & WS_CHILD) ? menu : 0;
    w->tid = cur_tid();
    w->title = name ? (wide ? read_wstr_utf8(name) : read_astr_utf8(name)) : "";
    w->extra.assign(cls->wnd_extra, 0);
    const DisplayInfo &d = primary_display();
    int fl, ft, fr, fb;
    compute_frame(style, exstyle, menu && !(style & WS_CHILD), &fl, &ft, &fr, &fb);
    if (cw == (int32_t)CW_USEDEFAULT || cw <= 0) cw = 1280 + fl + fr;
    if (ch == (int32_t)CW_USEDEFAULT || ch <= 0) ch = 720 + ft + fb;
    w->w = cw; w->h = ch;
    w->cl_left = fl; w->cl_top = ft;
    w->cx = cw - fl - fr; w->cy = ch - ft - fb;
    w->x = x == (int32_t)CW_USEDEFAULT ? d.x + (d.w - cw) / 2 : x;
    w->y = y == (int32_t)CW_USEDEFAULT ? d.y + (d.h - ch) / 2 : y;
    bool top_level = !(style & WS_CHILD);
    if (top_level && !create_sdl_window(*w)) { set_last_error(1400); return 0; }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        w->hwnd = g_next_hwnd;
        g_next_hwnd += 0x10;
        g_windows[w->hwnd] = w;
        if (w->sdl) g_sdl_ids[w->sdl_id] = w->hwnd;
    }
    LOGI(LC_WIN, "CreateWindowEx '%s' class '%s' style=%08x ex=%08x -> hwnd %08x client %dx%d at %d,%d", w->title.c_str(), cls->name.c_str(), style, exstyle, w->hwnd, w->cx, w->cy, w->x, w->y);
    // CREATESTRUCT
    uint32_t cs = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 48);
    wr32(cs + 0, param); wr32(cs + 4, hinst); wr32(cs + 8, menu); wr32(cs + 12, parent); wr32(cs + 16, ch); wr32(cs + 20, cw);
    wr32(cs + 24, (uint32_t)y); wr32(cs + 28, (uint32_t)x); wr32(cs + 32, style); wr32(cs + 36, name); wr32(cs + 40, clsarg); wr32(cs + 44, exstyle);
    uint32_t r = call_wndproc(c.esp, w, WM_NCCREATE, 0, cs);
    if (!r) { heap_free(heap_process(), 0, cs); LOGW(LC_WIN, "WM_NCCREATE failed"); return 0; }
    call_wndproc(c.esp, w, WM_NCCALCSIZE, 0, 0);
    r = call_wndproc(c.esp, w, WM_CREATE, 0, cs);
    heap_free(heap_process(), 0, cs);
    if ((int32_t)r == -1) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_windows.erase(w->hwnd);
        return 0;
    }
    w->created_msg_done = true;
    if (style & WS_VISIBLE) {
        // ShowWindow(SW_SHOW) path
        extern uint32_t win_show_window(uint32_t esp, const WinRef &w, uint32_t cmd);
        win_show_window(c.esp, w, 5);
    }
    return w->hwnd;
}
static uint32_t u32_CreateWindowExW(const HleCtx &c, uint32_t ex, uint32_t cls, uint32_t nm, uint32_t st, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t par,
                                    uint32_t menu, uint32_t hi, uint32_t prm) { return create_window(c, ex, cls, nm, st, x, y, w, h, par, menu, hi, prm, true); }
static uint32_t u32_CreateWindowExA(const HleCtx &c, uint32_t ex, uint32_t cls, uint32_t nm, uint32_t st, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t par,
                                    uint32_t menu, uint32_t hi, uint32_t prm) { return create_window(c, ex, cls, nm, st, x, y, w, h, par, menu, hi, prm, false); }

static void activate(uint32_t esp, const WinRef &w) {
    uint32_t prev = g_active.exchange(w->hwnd);
    g_focus = w->hwnd;
    if (prev == w->hwnd) return;
    on_main([&] { if (w->sdl) SDL_RaiseWindow(w->sdl); });
    if (!g_app_active.exchange(true)) call_wndproc(esp, w, WM_ACTIVATEAPP, 1, 0);
    call_wndproc(esp, w, WM_ACTIVATE, 1 /*WA_ACTIVE*/, 0);
    call_wndproc(esp, w, WM_SETFOCUS, prev, 0);
}

uint32_t win_show_window(uint32_t esp, const WinRef &w, uint32_t cmd) {
    bool was = w->visible;
    bool show = cmd != 0;
    bool activate_it = cmd != 4 /*SW_SHOWNOACTIVATE*/ && cmd != 8 /*SW_SHOWNA*/ && cmd != 7 /*SW_SHOWMINNOACTIVE*/ && cmd != 6 && cmd != 2 && !(w->exstyle & WS_EX_NOACTIVATE);
    w->visible = show;
    if (cmd == 3) { w->maximized = true; w->minimized = false; }
    else if (cmd == 2 || cmd == 6 || cmd == 7) { w->minimized = true; }
    else if (cmd == 9 || cmd == 1 || cmd == 5) { w->minimized = false; w->maximized = false; }
    on_main([&] {
        if (!w->sdl) return;
        if (!show) { SDL_HideWindow(w->sdl); return; }
        SDL_ShowWindow(w->sdl);
        if (cmd == 3) SDL_MaximizeWindow(w->sdl);
        else if (cmd == 2 || cmd == 6 || cmd == 7) SDL_MinimizeWindow(w->sdl);
        else if (cmd == 9) SDL_RestoreWindow(w->sdl);
    });
    if (was != show) {
        call_wndproc(esp, w, WM_SHOWWINDOW, show ? 1 : 0, 0);
        if (show && !w->ever_shown) {
            w->ever_shown = true;
            call_wndproc(esp, w, WM_SIZE, w->maximized ? 2 : w->minimized ? 1 : 0, ((uint32_t)w->cx & 0xFFFF) | ((uint32_t)w->cy << 16));
            call_wndproc(esp, w, WM_MOVE, 0, (((uint32_t)(w->x + w->cl_left)) & 0xFFFF) | ((uint32_t)(w->y + w->cl_top) << 16));
        }
    }
    if (show && activate_it && !(w->style & WS_CHILD)) activate(esp, w);
    if (!show && g_focus == w->hwnd) { g_focus = 0; if (g_active == w->hwnd) g_active = 0; }
    return was;
}
static uint32_t u32_ShowWindow(const HleCtx &c, uint32_t hwnd, uint32_t cmd) {
    WinRef w = get_window(hwnd);
    if (!w) { set_last_error(1400); return 0; }
    return win_show_window(c.esp, w, cmd);
}
static uint32_t u32_UpdateWindow(uint32_t hwnd) { return get_window(hwnd) != nullptr; }

static void destroy_tree(uint32_t esp, const WinRef &w) {
    if (w->destroyed) return;
    for (auto &c : all_windows())
        if (c->parent == w->hwnd) destroy_tree(esp, c);
    w->destroyed = true;
    if (g_focus == w->hwnd) g_focus = 0;
    if (g_active == w->hwnd) g_active = 0;
    if (g_capture == w->hwnd) g_capture = 0;
    call_wndproc(esp, w, WM_DESTROY, 0, 0);
    call_wndproc(esp, w, WM_NCDESTROY, 0, 0);
    on_main([&] { if (w->sdl) { SDL_DestroyWindow(w->sdl); } });
    std::lock_guard<std::mutex> lk(g_mu);
    g_windows.erase(w->hwnd);
    if (w->sdl_id) g_sdl_ids.erase(w->sdl_id);
    g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(), [&](const Timer &t) { return t.hwnd == w->hwnd; }), g_timers.end());
    g_timer_count = (int)g_timers.size();
}
static uint32_t u32_DestroyWindow(const HleCtx &c, uint32_t hwnd) {
    WinRef w = get_window(hwnd);
    if (!w) { set_last_error(1400); return 0; }
    destroy_tree(c.esp, w);
    return 1;
}

static uint32_t u32_IsWindow(uint32_t hwnd) { return get_window(hwnd) != nullptr; }
static uint32_t u32_IsWindowVisible(uint32_t hwnd) { WinRef w = get_window(hwnd); return w && w->visible; }
static uint32_t u32_IsIconic(uint32_t hwnd) { WinRef w = get_window(hwnd); return w && w->minimized; }
static uint32_t u32_IsZoomed(uint32_t hwnd) { WinRef w = get_window(hwnd); return w && w->maximized; }
static uint32_t u32_IsWindowEnabled(uint32_t hwnd) { WinRef w = get_window(hwnd); return w && w->enabled; }
static uint32_t u32_EnableWindow(uint32_t hwnd, uint32_t en) {
    WinRef w = get_window(hwnd);
    if (!w) return 0;
    bool was = !w->enabled;
    w->enabled = en != 0;
    return was;
}
static uint32_t u32_GetParent(uint32_t hwnd) { WinRef w = get_window(hwnd); return w ? (w->parent ? w->parent : w->owner) : 0; }
static uint32_t u32_GetDesktopWindow() { return 0x00010000; }
static uint32_t u32_GetWindow(uint32_t hwnd, uint32_t cmd) {
    WinRef w = get_window(hwnd);
    if (!w) return 0;
    auto all = all_windows();
    switch (cmd) {
        case 0: case 1: case 2: case 3: {   // GW_HWNDFIRST/LAST/NEXT/PREV among top-level siblings
            std::vector<uint32_t> sib;
            for (auto &x : all) if (x->parent == w->parent) sib.push_back(x->hwnd);
            if (sib.empty()) return 0;
            if (cmd == 0) return sib.front();
            if (cmd == 1) return sib.back();
            auto it = std::find(sib.begin(), sib.end(), hwnd);
            if (it == sib.end()) return 0;
            if (cmd == 2) return ++it == sib.end() ? 0 : *it;
            return it == sib.begin() ? 0 : *--it;
        }
        case 4: return w->owner;
        case 5: for (auto &x : all) if (x->parent == hwnd) return x->hwnd; return 0;
        default: return 0;
    }
}
static uint32_t u32_GetTopWindow(uint32_t hwnd) {
    for (auto &x : all_windows()) if (x->parent == hwnd && (hwnd || !(x->style & WS_CHILD))) return x->hwnd;
    return 0;
}
static uint32_t u32_FindWindowW(uint32_t cls, uint32_t title) {
    std::string c = cls ? lower_utf8(read_wstr_utf8(cls)) : "", t = title ? read_wstr_utf8(title) : "";
    for (auto &x : all_windows())
        if ((c.empty() || lower_utf8(x->cls->name) == c) && (t.empty() || x->title == t)) return x->hwnd;
    return 0;
}
static uint32_t u32_FindWindowA(uint32_t cls, uint32_t title) {
    std::string c = cls ? lower_utf8(read_astr_utf8(cls)) : "", t = title ? read_astr_utf8(title) : "";
    for (auto &x : all_windows())
        if ((c.empty() || lower_utf8(x->cls->name) == c) && (t.empty() || x->title == t)) return x->hwnd;
    return 0;
}
static uint32_t u32_GetWindowThreadProcessId(uint32_t hwnd, uint32_t ppid) {
    WinRef w = get_window(hwnd);
    if (!w) return 0;
    if (ppid) wr32(ppid, process_id());
    return w->tid;
}

// ---- geometry ------------------------------------------------------------------------------------------------
static uint32_t u32_GetWindowRect(uint32_t hwnd, uint32_t r) {
    WinRef w = get_window(hwnd);
    if (!w) { set_last_error(1400); return 0; }
    wr32(r, (uint32_t)w->x); wr32(r + 4, (uint32_t)w->y); wr32(r + 8, (uint32_t)(w->x + w->w)); wr32(r + 12, (uint32_t)(w->y + w->h));
    return 1;
}
static uint32_t u32_GetClientRect(uint32_t hwnd, uint32_t r) {
    WinRef w = get_window(hwnd);
    if (!w) { set_last_error(1400); return 0; }
    wr32(r, 0); wr32(r + 4, 0); wr32(r + 8, (uint32_t)w->cx); wr32(r + 12, (uint32_t)w->cy);
    return 1;
}
static uint32_t u32_ClientToScreen(uint32_t hwnd, uint32_t pt) {
    WinRef w = get_window(hwnd);
    if (!w) return 0;
    wr32(pt, rd32(pt) + (uint32_t)(w->x + w->cl_left));
    wr32(pt + 4, rd32(pt + 4) + (uint32_t)(w->y + w->cl_top));
    return 1;
}
static uint32_t u32_ScreenToClient(uint32_t hwnd, uint32_t pt) {
    WinRef w = get_window(hwnd);
    if (!w) return 0;
    wr32(pt, rd32(pt) - (uint32_t)(w->x + w->cl_left));
    wr32(pt + 4, rd32(pt + 4) - (uint32_t)(w->y + w->cl_top));
    return 1;
}
static uint32_t u32_MapWindowPoints(uint32_t from, uint32_t to, uint32_t pts, uint32_t n) {
    WinRef a = from ? get_window(from) : nullptr, b = to ? get_window(to) : nullptr;
    int32_t dx = (a ? a->x + a->cl_left : 0) - (b ? b->x + b->cl_left : 0), dy = (a ? a->y + a->cl_top : 0) - (b ? b->y + b->cl_top : 0);
    for (uint32_t i = 0; i < n * 2; i += 2) {
        wr32(pts + 4 * i, rd32(pts + 4 * i) + (uint32_t)dx);
        wr32(pts + 4 * i + 4, rd32(pts + 4 * i + 4) + (uint32_t)dy);
    }
    return ((uint32_t)dy << 16) | ((uint32_t)dx & 0xFFFF);
}
static uint32_t u32_AdjustWindowRectEx(uint32_t r, uint32_t style, uint32_t menu, uint32_t ex) {
    int l, t, rr, b;
    compute_frame(style, ex, menu != 0, &l, &t, &rr, &b);
    wr32(r, rd32(r) - (uint32_t)l); wr32(r + 4, rd32(r + 4) - (uint32_t)t); wr32(r + 8, rd32(r + 8) + (uint32_t)rr); wr32(r + 12, rd32(r + 12) + (uint32_t)b);
    return 1;
}
static uint32_t u32_AdjustWindowRect(uint32_t r, uint32_t style, uint32_t menu) { return u32_AdjustWindowRectEx(r, style, menu, 0); }

static void apply_geometry(const HleCtx &c, const WinRef &w, int nx, int ny, int nw, int nh, bool moved, bool sized) {
    int fl, ft, fr, fb;
    compute_frame(w->style, w->exstyle, false, &fl, &ft, &fr, &fb);
    w->cl_left = fl; w->cl_top = ft;
    if (moved) { w->x = nx; w->y = ny; }
    if (sized) { w->w = nw; w->h = nh; w->cx = std::max(nw - fl - fr, 0); w->cy = std::max(nh - ft - fb, 0); }
    on_main([&] {
        if (!w->sdl) return;
        if (sized) SDL_SetWindowSize(w->sdl, std::max(w->cx, 1), std::max(w->cy, 1));
        if (moved) SDL_SetWindowPosition(w->sdl, w->x + w->cl_left, w->y + w->cl_top);
    });
    if (sized) call_wndproc(c.esp, w, WM_SIZE, w->maximized ? 2 : w->minimized ? 1 : 0, ((uint32_t)w->cx & 0xFFFF) | ((uint32_t)w->cy << 16));
    if (moved) call_wndproc(c.esp, w, WM_MOVE, 0, (((uint32_t)(w->x + w->cl_left)) & 0xFFFF) | ((uint32_t)(w->y + w->cl_top) << 16));
}
static uint32_t u32_SetWindowPos(const HleCtx &c, uint32_t hwnd, uint32_t after, int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t flags) {
    WinRef w = get_window(hwnd);
    if (!w) { set_last_error(1400); return 0; }
    bool moved = !(flags & 2), sized = !(flags & 1);
    if (moved || sized) apply_geometry(c, w, x, y, cx, cy, moved, sized);
    if (!(flags & 4) && (after == 0xFFFFFFFFu || after == 0xFFFFFFFEu)) {   // HWND_TOPMOST / HWND_NOTOPMOST
        bool top = after == 0xFFFFFFFFu;
        on_main([&] { if (w->sdl) SDL_SetWindowAlwaysOnTop(w->sdl, top); });
    }
    if (flags & 0x40) win_show_window(c.esp, w, (flags & 0x10) ? 8 : 5);
    if (flags & 0x80) win_show_window(c.esp, w, 0);
    return 1;
}
static uint32_t u32_MoveWindow(const HleCtx &c, uint32_t hwnd, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t) {
    WinRef win = get_window(hwnd);
    if (!win) return 0;
    apply_geometry(c, win, x, y, w, h, true, true);
    return 1;
}
static uint32_t u32_GetWindowPlacement(uint32_t hwnd, uint32_t p) {
    WinRef w = get_window(hwnd);
    if (!w) return 0;
    wr32(p, 44); wr32(p + 4, 0);
    wr32(p + 8, w->minimized ? 2 : w->maximized ? 3 : 1);
    wr32(p + 12, 0xFFFFFFFFu); wr32(p + 16, 0xFFFFFFFFu); wr32(p + 20, 0xFFFFFFFFu); wr32(p + 24, 0xFFFFFFFFu);
    wr32(p + 28, (uint32_t)w->x); wr32(p + 32, (uint32_t)w->y); wr32(p + 36, (uint32_t)(w->x + w->w)); wr32(p + 40, (uint32_t)(w->y + w->h));
    return 1;
}
static uint32_t u32_SetWindowPlacement(const HleCtx &c, uint32_t hwnd, uint32_t p) {
    WinRef w = get_window(hwnd);
    if (!w) return 0;
    int32_t l = (int32_t)rd32(p + 28), t = (int32_t)rd32(p + 32), r = (int32_t)rd32(p + 36), b = (int32_t)rd32(p + 40);
    apply_geometry(c, w, l, t, r - l, b - t, true, true);
    uint32_t cmd = rd32(p + 8);
    if (cmd) win_show_window(c.esp, w, cmd);
    return 1;
}

// ---- window words ------------------------------------------------------------------------------------------------
static uint32_t get_long(uint32_t hwnd, int32_t idx) {
    WinRef w = get_window(hwnd);
    if (!w) { set_last_error(1400); return 0; }
    switch (idx) {
        case -4: return w->wndproc;
        case -6: return w->hinst;
        case -8: return w->parent ? w->parent : w->owner;
        case -12: return w->id;
        case -16: return w->style | (w->visible ? WS_VISIBLE : 0);
        case -20: return w->exstyle;
        case -21: return (uint32_t)w->userdata;
        default:
            if (idx >= 0 && (size_t)idx + 4 <= w->extra.size()) { uint32_t v; memcpy(&v, &w->extra[idx], 4); return v; }
            set_last_error(1413);
            return 0;
    }
}
static uint32_t set_long(uint32_t hwnd, int32_t idx, uint32_t v) {
    WinRef w = get_window(hwnd);
    if (!w) { set_last_error(1400); return 0; }
    uint32_t old = get_long(hwnd, idx);
    switch (idx) {
        case -4: w->wndproc = v; break;
        case -6: w->hinst = v; break;
        case -12: w->id = v; break;
        case -16: {
            bool cap_changed = (w->style & (WS_CAPTION | WS_THICKFRAME)) != (v & (WS_CAPTION | WS_THICKFRAME));
            w->style = v & ~WS_VISIBLE;
            if (cap_changed) {
                int fl, ft, fr, fb;
                compute_frame(w->style, w->exstyle, false, &fl, &ft, &fr, &fb);
                w->cl_left = fl; w->cl_top = ft;
                w->w = w->cx + fl + fr; w->h = w->cy + ft + fb;
                bool bordered = (v & WS_CAPTION) == WS_CAPTION || (v & WS_THICKFRAME);
                on_main([&] { if (w->sdl) { SDL_SetWindowBordered(w->sdl, bordered); SDL_SetWindowResizable(w->sdl, (v & WS_THICKFRAME) != 0); } });
            }
            break;
        }
        case -20: w->exstyle = v; break;
        case -21: w->userdata = (int32_t)v; break;
        default:
            if (idx >= 0 && (size_t)idx + 4 <= w->extra.size()) memcpy(&w->extra[idx], &v, 4);
            else { set_last_error(1413); return 0; }
    }
    return old;
}
static uint32_t u32_GetWindowLongA(uint32_t h, int32_t i) { return get_long(h, i); }
static uint32_t u32_GetWindowLongW(uint32_t h, int32_t i) { return get_long(h, i); }
static uint32_t u32_SetWindowLongA(uint32_t h, int32_t i, uint32_t v) { return set_long(h, i, v); }
static uint32_t u32_SetWindowLongW(uint32_t h, int32_t i, uint32_t v) { return set_long(h, i, v); }
static uint32_t set_title(uint32_t hwnd, std::string t) {
    WinRef w = get_window(hwnd);
    if (!w) return 0;
    w->title = t;
    on_main([&] { if (w->sdl) SDL_SetWindowTitle(w->sdl, t.c_str()); });
    return 1;
}
static uint32_t u32_SetWindowTextW(uint32_t h, uint32_t s) { return set_title(h, s ? read_wstr_utf8(s) : ""); }
static uint32_t u32_SetWindowTextA(uint32_t h, uint32_t s) { return set_title(h, s ? read_astr_utf8(s) : ""); }
static uint32_t u32_GetWindowTextW(uint32_t h, uint32_t buf, int32_t n) {
    WinRef w = get_window(h);
    if (!w || n <= 0) return 0;
    std::u16string t = utf8_to_utf16(w->title);
    if ((size_t)n <= t.size()) t.resize((size_t)n - 1);
    write_wstr(buf, (size_t)n, t);
    return (uint32_t)t.size();
}
static uint32_t u32_GetWindowTextA(uint32_t h, uint32_t buf, int32_t n) {
    WinRef w = get_window(h);
    if (!w || n <= 0) return 0;
    std::string t = w->title;
    if ((size_t)n <= t.size()) t.resize((size_t)n - 1);
    write_astr(buf, (size_t)n, t);
    return (uint32_t)t.size();
}
static uint32_t u32_GetWindowTextLengthW(uint32_t h) { WinRef w = get_window(h); return w ? (uint32_t)utf8_to_utf16(w->title).size() : 0; }

// ---- focus -------------------------------------------------------------------------------------------------------
void set_focus_window(uint32_t hwnd) { g_focus = hwnd; }
static uint32_t u32_GetFocus() { return g_focus.load(std::memory_order_relaxed); }
static uint32_t u32_GetForegroundWindow() { return g_active.load(std::memory_order_relaxed); }
static uint32_t u32_GetActiveWindow() { return g_active.load(std::memory_order_relaxed); }
static uint32_t u32_SetFocus(const HleCtx &c, uint32_t hwnd) {
    WinRef w = get_window(hwnd);
    uint32_t prev = g_focus.exchange(hwnd);
    if (w && prev != hwnd) {
        if (prev) if (WinRef p = get_window(prev)) call_wndproc(c.esp, p, WM_KILLFOCUS, hwnd, 0);
        call_wndproc(c.esp, w, WM_SETFOCUS, prev, 0);
    }
    return prev;
}
static uint32_t u32_SetActiveWindow(const HleCtx &c, uint32_t hwnd) {
    WinRef w = get_window(hwnd);
    uint32_t prev = g_active.load();
    if (w) activate(c.esp, w);
    return prev;
}
static uint32_t u32_SetForegroundWindow(const HleCtx &c, uint32_t hwnd) {
    WinRef w = get_window(hwnd);
    if (!w) return 0;
    activate(c.esp, w);
    return 1;
}
static uint32_t u32_BringWindowToTop(uint32_t hwnd) { WinRef w = get_window(hwnd); if (w) on_main([&] { if (w->sdl) SDL_RaiseWindow(w->sdl); }); return w != nullptr; }
static uint32_t u32_GetCapture() { return g_capture.load(); }
static uint32_t u32_SetCapture(uint32_t hwnd) {
    uint32_t prev = g_capture.exchange(hwnd);
    WinRef w = get_window(hwnd);
    on_main([&] { SDL_CaptureMouse(true); (void)w; });
    return prev;
}
static uint32_t u32_ReleaseCapture() { g_capture = 0; on_main([] { SDL_CaptureMouse(false); }); return 1; }

// ---- messages ----------------------------------------------------------------------------------------------------
static bool take_timer_msg(uint32_t tid, Msg *m, uint32_t hwnd_filter, uint32_t mn, uint32_t mx) {
    if (!g_timer_count.load(std::memory_order_relaxed)) return false;
    if (mn && mn > WM_TIMER) return false;
    if (mx && mx < WM_TIMER) return false;
    uint64_t now = now_ms64();
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &t : g_timers) {
        if (t.tid != tid || t.next > now) continue;
        if (hwnd_filter && t.hwnd != hwnd_filter) continue;
        t.next = now + t.ms;
        m->hwnd = t.hwnd; m->msg = WM_TIMER; m->wparam = t.id; m->lparam = t.proc; m->time = tick_ms();
        return true;
    }
    return false;
}
static bool peek(Msg *out, uint32_t hwnd, uint32_t mn, uint32_t mx, bool remove) {
    if (!mainthread_active()) platform_pump();
    Queue *q = my_queue();
    if (!q->count.load(std::memory_order_relaxed) && !g_timer_count.load(std::memory_order_relaxed) && !q->quit) return false;
    {
        std::lock_guard<std::mutex> lk(q->mu);
        for (auto it = q->q.begin(); it != q->q.end(); ++it) {
            if (hwnd && it->hwnd != hwnd) continue;
            if (mn || mx) { if (it->msg < mn || (mx && it->msg > mx)) continue; }
            *out = *it;
            if (remove) { q->q.erase(it); q->count--; }
            return true;
        }
        if (q->quit && (!mn && !mx ? true : (mn <= WM_QUIT && (!mx || mx >= WM_QUIT)))) {
            *out = Msg{};
            out->msg = WM_QUIT;
            out->wparam = (uint32_t)q->quit_code;
            out->time = tick_ms();
            if (remove) q->quit = false;
            return true;
        }
    }
    return take_timer_msg(cur_tid(), out, hwnd, mn, mx);
}
static void store_msg(uint32_t p, const Msg &m) {
    wr32(p, m.hwnd); wr32(p + 4, m.msg); wr32(p + 8, m.wparam); wr32(p + 12, m.lparam); wr32(p + 16, m.time);
    wr32(p + 20, (uint32_t)m.x); wr32(p + 24, (uint32_t)m.y);
}
static uint32_t u32_PeekMessage(uint32_t pmsg, uint32_t hwnd, uint32_t mn, uint32_t mx, uint32_t flags) {
    Msg m;
    if (!peek(&m, hwnd, mn, mx, (flags & 1) != 0)) return 0;
    store_msg(pmsg, m);
    return 1;
}
static uint32_t u32_GetMessage(uint32_t pmsg, uint32_t hwnd, uint32_t mn, uint32_t mx) {
    Queue *q = my_queue();
    for (;;) {
        Msg m;
        if (peek(&m, hwnd, mn, mx, true)) {
            store_msg(pmsg, m);
            return m.msg == WM_QUIT ? 0 : 1;
        }
        std::unique_lock<std::mutex> lk(q->mu);
        q->cv.wait_for(lk, std::chrono::milliseconds(mainthread_active() ? 20 : 4));
    }
}
static uint32_t u32_WaitMessage() {
    Queue *q = my_queue();
    for (;;) {
        if (!mainthread_active()) platform_pump();
        if (q->count.load() || q->quit) return 1;
        std::unique_lock<std::mutex> lk(q->mu);
        q->cv.wait_for(lk, std::chrono::milliseconds(mainthread_active() ? 20 : 4));
    }
}
static uint32_t u32_TranslateMessage(uint32_t pmsg) { uint32_t m = rd32(pmsg + 4); return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP; }
static uint32_t dispatch(const HleCtx &c, uint32_t pmsg) {
    uint32_t hwnd = rd32(pmsg), msg = rd32(pmsg + 4), wp = rd32(pmsg + 8), lp = rd32(pmsg + 12);
    if (msg == WM_TIMER && lp) {   // timer callback
        return guest_invoke_from(c.esp, lp, Conv::Stdcall, hwnd, WM_TIMER, wp, tick_ms());
    }
    WinRef w = get_window(hwnd);
    if (!w) return 0;
    return call_wndproc(c.esp, w, msg, wp, lp);
}
static uint32_t u32_DispatchMessage(const HleCtx &c, uint32_t pmsg) { return dispatch(c, pmsg); }
static uint32_t u32_SendMessage(const HleCtx &c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (hwnd == 0xFFFF || hwnd == 0xFFFFFFFFu) {   // HWND_BROADCAST
        for (auto &w : all_windows()) if (!(w->style & WS_CHILD)) call_wndproc(c.esp, w, msg, wp, lp);
        return 0;
    }
    WinRef w = get_window(hwnd);
    if (!w) return 0;
    return call_wndproc(c.esp, w, msg, wp, lp);
}
static uint32_t u32_SendMessageTimeout(const HleCtx &c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, uint32_t, uint32_t, uint32_t res) {
    uint32_t r = u32_SendMessage(c, hwnd, msg, wp, lp);
    if (res) wr32(res, r);
    return 1;
}
static uint32_t u32_PostMessage(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!hwnd) { Msg m; m.msg = msg; m.wparam = wp; m.lparam = lp; post_msg(cur_tid(), m); return 1; }
    WinRef w = get_window(hwnd);
    if (!w) { set_last_error(1400); return 0; }
    post_to_window(hwnd, msg, wp, lp);
    return 1;
}
static uint32_t u32_PostThreadMessage(uint32_t tid, uint32_t msg, uint32_t wp, uint32_t lp) {
    Msg m; m.msg = msg; m.wparam = wp; m.lparam = lp;
    post_msg(tid, m);
    return 1;
}
static void u32_PostQuitMessage(uint32_t code) {
    Queue *q = my_queue();
    {
        std::lock_guard<std::mutex> lk(q->mu);
        q->quit = true;
        q->quit_code = (int)code;
    }
    q->cv.notify_all();
}
static uint32_t def_window_proc(const HleCtx &c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    WinRef w = get_window(hwnd);
    switch (msg) {
        case WM_NCCREATE: return 1;
        case WM_CLOSE: if (w) destroy_tree(c.esp, w); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NCHITTEST: return 1;           // HTCLIENT
        case WM_MOUSEACTIVATE: return 1;       // MA_ACTIVATE
        case WM_NCACTIVATE: return 1;
        case WM_SETCURSOR: return 0;
        case WM_SETTEXT: set_title(hwnd, lp ? read_wstr_utf8(lp) : ""); return 1;
        case WM_GETTEXT: return u32_GetWindowTextW(hwnd, lp, (int32_t)wp);
        case WM_GETTEXTLENGTH: return u32_GetWindowTextLengthW(hwnd);
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == 0xF060 && w) { call_wndproc(c.esp, w, WM_CLOSE, 0, 0); return 0; }   // SC_CLOSE
            return 0;
        case WM_SYSKEYDOWN:
            if (wp == 0x73 && (lp & (1u << 29)) && w) { call_wndproc(c.esp, w, WM_CLOSE, 0, 0); return 0; }   // Alt+F4
            return 0;
        case WM_ACTIVATE:
            if ((wp & 0xFFFF) != 0 && w) g_focus = hwnd;
            return 0;
        default: return 0;
    }
}
static uint32_t u32_DefWindowProc(const HleCtx &c, uint32_t h, uint32_t m, uint32_t w, uint32_t l) { return def_window_proc(c, h, m, w, l); }
static uint32_t u32_CallWindowProc(const HleCtx &c, uint32_t proc, uint32_t h, uint32_t m, uint32_t w, uint32_t l) {
    return guest_invoke_from(c.esp, proc, Conv::Stdcall, h, m, w, l);
}
static uint32_t u32_RegisterWindowMessage(uint32_t name) {
    static std::map<std::string, uint32_t> msgs;
    static uint32_t next = 0xC000;
    std::lock_guard<std::mutex> lk(g_mu);
    std::string n = lower_utf8(read_wstr_utf8(name));
    auto it = msgs.find(n);
    if (it != msgs.end()) return it->second;
    return msgs[n] = next++;
}
static uint32_t u32_InSendMessage() { return 0; }

// ---- timers ------------------------------------------------------------------------------------------------------
static uint32_t u32_SetTimer(uint32_t hwnd, uint32_t id, uint32_t ms, uint32_t proc) {
    if (ms < 10) ms = 10;
    std::lock_guard<std::mutex> lk(g_mu);
    static uint32_t next_id = 0x1000;
    if (!hwnd) id = next_id++;
    for (auto &t : g_timers)
        if (t.hwnd == hwnd && t.id == id) { t.ms = ms; t.proc = proc; t.next = now_ms64() + ms; return id; }
    g_timers.push_back({hwnd, id, ms, proc, cur_tid(), now_ms64() + ms});
    g_timer_count = (int)g_timers.size();
    return id;
}
static uint32_t u32_KillTimer(uint32_t hwnd, uint32_t id) {
    std::lock_guard<std::mutex> lk(g_mu);
    size_t n = g_timers.size();
    g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(), [&](const Timer &t) { return t.hwnd == hwnd && t.id == id; }), g_timers.end());
    g_timer_count = (int)g_timers.size();
    return n != g_timers.size();
}

// ---- SDL event translation ---------------------------------------------------------------------------------------
static uint32_t make_lparam(int x, int y) { return ((uint32_t)(int16_t)x & 0xFFFF) | ((uint32_t)(uint16_t)(int16_t)y << 16); }
static uint32_t mouse_key_flags() {
    uint32_t f = 0, b = g_mouse.buttons.load();
    if (b & 1) f |= 1;
    if (b & 2) f |= 2;
    if (b & 4) f |= 0x10;
    if (b & 8) f |= 0x20;
    if (b & 16) f |= 0x40;
    if (g_vk_down[0x10] & 1) f |= 4;
    if (g_vk_down[0x11] & 1) f |= 8;
    return f;
}
static void set_vk(int vk, bool down) {
    if (vk <= 0 || vk > 255) return;
    uint8_t cur = g_vk_down[vk].load();
    uint8_t nv = down ? 1 : 0;
    if (down && !(cur & 1) && (vk == 0x14 || vk == 0x90 || vk == 0x91)) nv |= (cur & 2) ? 0 : 2;   // toggle keys
    else nv |= (cur & 2);
    g_vk_down[vk] = nv;
    // aggregate modifiers
    auto agg = [&](int g, int l, int r) { g_vk_down[g] = ((g_vk_down[l] | g_vk_down[r]) & 1); };
    agg(0x10, 0xA0, 0xA1); agg(0x11, 0xA2, 0xA3); agg(0x12, 0xA4, 0xA5);
}
static void route_window_event(const SDL_Event &e, const WinRef &w) {
    switch (e.type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: post_to_window(w->hwnd, WM_CLOSE, 0, 0); break;
        case SDL_EVENT_WINDOW_RESIZED: {
            int nw = e.window.data1, nh = e.window.data2;
            if (nw == w->cx && nh == w->cy) break;
            int fl, ft, fr, fb;
            compute_frame(w->style, w->exstyle, false, &fl, &ft, &fr, &fb);
            w->cx = nw; w->cy = nh; w->w = nw + fl + fr; w->h = nh + ft + fb;
            post_to_window(w->hwnd, WM_SIZE, w->maximized ? 2 : w->minimized ? 1 : 0, make_lparam(nw, nh));
            break;
        }
        case SDL_EVENT_WINDOW_MOVED:
            w->x = e.window.data1 - w->cl_left; w->y = e.window.data2 - w->cl_top;
            post_to_window(w->hwnd, WM_MOVE, 0, make_lparam(e.window.data1, e.window.data2));
            break;
        case SDL_EVENT_WINDOW_MINIMIZED: w->minimized = true; post_to_window(w->hwnd, WM_SIZE, 1, 0); break;
        case SDL_EVENT_WINDOW_MAXIMIZED: w->maximized = true; w->minimized = false; break;
        case SDL_EVENT_WINDOW_RESTORED: w->minimized = false; w->maximized = false; break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: {
            uint32_t prev = g_active.exchange(w->hwnd);
            g_focus = w->hwnd;
            if (prev == w->hwnd) break;
            if (!g_app_active.exchange(true)) post_to_window(w->hwnd, WM_ACTIVATEAPP, 1, 0);
            post_to_window(w->hwnd, WM_ACTIVATE, 1, 0);
            post_to_window(w->hwnd, WM_SETFOCUS, prev, 0);
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            if (g_active.load() != w->hwnd) break;
            g_active = 0; g_focus = 0;
            for (auto &k : g_vk_down) k &= 2;
            g_mouse.buttons = 0;
            post_to_window(w->hwnd, WM_ACTIVATE, 0, 0);
            post_to_window(w->hwnd, WM_KILLFOCUS, 0, 0);
            g_app_active = false;
            post_to_window(w->hwnd, WM_ACTIVATEAPP, 0, 0);
            break;
        }
        default: break;
    }
}
static void route_key(const SDL_Event &e, const WinRef &w) {
    bool down = e.type == SDL_EVENT_KEY_DOWN;
    int vk = sdl_scancode_to_vk(e.key.scancode);
    if (!vk) return;
    bool ext;
    int scan = sdl_scancode_to_set1(e.key.scancode, &ext);
    bool was_down = g_vk_down[vk].load() & 1;
    set_vk(vk, down);
    bool alt = (g_vk_down[0x12] & 1) != 0;
    uint32_t lp = 1 | ((uint32_t)scan << 16) | (ext ? 1u << 24 : 0) | (alt ? 1u << 29 : 0) | (was_down || !down ? 1u << 30 : 0) | (!down ? 1u << 31 : 0);
    bool sys = alt && !(g_vk_down[0x11] & 1) ? true : (vk == 0x12 || vk == 0xA4 || vk == 0xA5 || vk == 0x79);
    uint32_t msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
    uint32_t wp = (uint32_t)(vk == 0xA0 || vk == 0xA1 ? 0x10 : vk == 0xA2 || vk == 0xA3 ? 0x11 : vk == 0xA4 || vk == 0xA5 ? 0x12 : vk);
    uint32_t target = g_focus.load() ? g_focus.load() : w->hwnd;
    if (!e.key.repeat || down) post_to_window(target, msg, wp, lp);
    if (down && !(g_vk_down[0x11] & 1)) {   // characters that SDL text input does not produce
        uint32_t ch = vk == 0x0D ? 13 : vk == 0x08 ? 8 : vk == 0x09 ? 9 : vk == 0x1B ? 27 : 0;
        if (ch) post_to_window(target, WM_CHAR, ch, lp);
    }
}
static void route_mouse(const SDL_Event &e, const WinRef &w) {
    uint32_t target = w->hwnd;
    // SDL reports window coordinates; the guest window may have another client size (the iOS virtual desktop: points vs 720p)
    float fx = 1.0f, fy = 1.0f;
    {
        int sw = 0, sh = 0;
        if (w->sdl && SDL_GetWindowSize(w->sdl, &sw, &sh) && sw > 0 && sh > 0 && w->cx > 0 && w->cy > 0) {
            fx = (float)w->cx / (float)sw;
            fy = (float)w->cy / (float)sh;
        }
    }
    SDL_Event se = e;
    switch (se.type) {
        case SDL_EVENT_MOUSE_MOTION: se.motion.x *= fx; se.motion.y *= fy; break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: se.button.x *= fx; se.button.y *= fy; break;
        default: break;
    }
    const SDL_Event &ev = se;
    auto cursor_update = [&](float x, float y) {
        g_cursor_x = w->x + w->cl_left + (int)x;
        g_cursor_y = w->y + w->cl_top + (int)y;
    };
    switch (ev.type) {
        case SDL_EVENT_MOUSE_MOTION:
            cursor_update(ev.motion.x, ev.motion.y);
            g_mouse.dx += (int32_t)ev.motion.xrel;
            g_mouse.dy += (int32_t)ev.motion.yrel;
            if ((int32_t)ev.motion.xrel) input_mouse_push(0, (uint32_t)(int32_t)ev.motion.xrel);
            if ((int32_t)ev.motion.yrel) input_mouse_push(4, (uint32_t)(int32_t)ev.motion.yrel);
            post_to_window(target, WM_MOUSEMOVE, mouse_key_flags(), make_lparam((int)ev.motion.x, (int)ev.motion.y));
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            bool down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            cursor_update(ev.button.x, ev.button.y);
            int bit = ev.button.button == SDL_BUTTON_LEFT ? 0 : ev.button.button == SDL_BUTTON_RIGHT ? 1 : ev.button.button == SDL_BUTTON_MIDDLE ? 2 : ev.button.button == SDL_BUTTON_X1 ? 3 : 4;
            if (down) g_mouse.buttons |= 1u << bit; else g_mouse.buttons &= ~(1u << bit);
            int vk = bit == 0 ? 1 : bit == 1 ? 2 : bit == 2 ? 4 : bit == 3 ? 5 : 6;
            set_vk(vk, down);
            input_mouse_push(12 + (uint32_t)bit, down ? 0x80 : 0);
            bool dbl = down && ev.button.clicks >= 2;
            uint32_t msg = bit == 0 ? (down ? (dbl ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN) : WM_LBUTTONUP)
                         : bit == 1 ? (down ? (dbl ? WM_RBUTTONDBLCLK : WM_RBUTTONDOWN) : WM_RBUTTONUP)
                         : bit == 2 ? (down ? (dbl ? WM_MBUTTONDBLCLK : WM_MBUTTONDOWN) : WM_MBUTTONUP)
                                    : (down ? (dbl ? WM_XBUTTONDBLCLK : WM_XBUTTONDOWN) : WM_XBUTTONUP);
            uint32_t wp = mouse_key_flags();
            if (bit >= 3) wp |= (uint32_t)(bit == 3 ? 1 : 2) << 16;
            post_to_window(target, msg, wp, make_lparam((int)ev.button.x, (int)ev.button.y));
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            int delta = (int)(ev.wheel.y * 120.0f);
            g_mouse.wheel += delta;
            input_mouse_push(8, (uint32_t)delta);
            post_to_window(target, WM_MOUSEWHEEL, mouse_key_flags() | ((uint32_t)(uint16_t)(int16_t)delta << 16),
                           make_lparam(g_cursor_x.load(), g_cursor_y.load()));
            break;
        }
        default: break;
    }
}
static void route_text(const SDL_Event &e, const WinRef &w) {
    std::u16string u = utf8_to_utf16(e.text.text ? e.text.text : "");
    uint32_t target = g_focus.load() ? g_focus.load() : w->hwnd;
    for (char16_t c : u) post_to_window(target, WM_CHAR, c, 1);
}

}  // namespace win

using namespace win;

bool platform_route_event(const SDL_Event &e) {
    switch (e.type) {
        case SDL_EVENT_QUIT: {
            uint32_t main = platform_main_hwnd();
            if (main) post_to_window(main, WM_CLOSE, 0, 0);
            return true;
        }
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            lifecycle_pause(1500);
            return true;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            lifecycle_resume();
            return true;
        case SDL_EVENT_LOW_MEMORY:
            memacct_note_warning();
            return true;
        case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_REMOVED: case SDL_EVENT_GAMEPAD_REMAPPED:
            xinput_on_event(e);
            // UE3 only looks for new XInput pads at start-up and on WM_DEVICECHANGE (UWindowsClient's WndProc), and iOS reports
            // controllers after the game has started, so tell the window like Windows does on a device arrival or removal
            // (FWindowsViewport defers only DBT_DEVICEARRIVAL and DBT_DEVICEREMOVECOMPLETE, and does not read lParam)
            if (e.type != SDL_EVENT_GAMEPAD_REMAPPED) {
                uint32_t main = platform_main_hwnd();
                if (main) post_to_window(main, 0x0219 /*WM_DEVICECHANGE*/, e.type == SDL_EVENT_GAMEPAD_ADDED ? 0x8000u : 0x8004u, 0);
            }
            return true;
        default: break;
    }
    SDL_WindowID wid = 0;
    if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST) wid = e.window.windowID;
    else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) wid = e.key.windowID;
    else if (e.type == SDL_EVENT_TEXT_INPUT) wid = e.text.windowID;
    else if (e.type == SDL_EVENT_MOUSE_MOTION) wid = e.motion.windowID;
    else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) wid = e.button.windowID;
    else if (e.type == SDL_EVENT_MOUSE_WHEEL) wid = e.wheel.windowID;
    else return false;
    WinRef w = window_for_sdl(wid);
    if (!w) return false;
    if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST) route_window_event(e, w);
    else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) route_key(e, w);
    else if (e.type == SDL_EVENT_TEXT_INPUT) route_text(e, w);
    else route_mouse(e, w);
    return true;
}

static std::atomic<bool> g_video_tried{false}, g_video_ok{false};
bool platform_video_init() {
    if (g_video_tried.exchange(true)) return g_video_ok;
    bool ok = false;
    mainthread_call([&] {
        // controller input must not depend on SDL's idea of window focus (it drops joystick input for unfocused apps)
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_WasInit(SDL_INIT_VIDEO)) {
            if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
                LOGW(LC_WIN, "SDL video init failed: %s", SDL_GetError());
                return;
            }
        }
        ok = true;
    });
    g_video_ok = ok;
    return ok;
}

// Scripted input for unattended test runs. DISH_DEV_KEYS is a comma list of "<seconds>:<input>[~<hold seconds>]" (default hold
// 0.1 s), timed from the first event pump, delivered as real SDL events:
//   <input> = an SDL scancode name ("Return", "W"), "pad.<button>" ("pad.a", "pad.start", "pad.dpup") or "pad.<axis>=<-1..1>"
//   ("pad.leftx=1", "pad.righttrigger=1"). The first pad input attaches an SDL virtual gamepad, which goes through the same
//   hot-plug path as a real controller.
void platform_dev_input() {
    struct Step { double at, hold; SDL_Scancode sc; int button = -1, axis = -1; float value = 0; int phase = 0; };
    static std::vector<Step> steps = [] {
        std::vector<Step> v;
        const char *spec = getenv("DISH_DEV_KEYS");
        for (const char *p = spec; p && *p;) {
            const char *end = strchr(p, ',');
            std::string item(p, end ? (size_t)(end - p) : strlen(p));
            p = end ? end + 1 : nullptr;
            size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            Step st{atof(item.c_str()), 0.1, SDL_SCANCODE_UNKNOWN};
            std::string what = item.substr(colon + 1);
            if (size_t t = what.find('~'); t != std::string::npos) { st.hold = atof(what.c_str() + t + 1); what.resize(t); }
            if (what.rfind("pad.", 0) == 0) {
                std::string name = what.substr(4);
                if (size_t eq = name.find('='); eq != std::string::npos) {
                    st.value = (float)atof(name.c_str() + eq + 1);
                    name.resize(eq);
                    st.axis = SDL_GetGamepadAxisFromString(name.c_str());
                } else {
                    st.button = SDL_GetGamepadButtonFromString(name.c_str());
                }
                if (st.axis < 0 && st.button < 0) { LOGW(LC_WIN, "DISH_DEV_KEYS: unknown pad input '%s'", what.c_str()); continue; }
            } else {
                st.sc = SDL_GetScancodeFromName(what.c_str());
                if (st.sc == SDL_SCANCODE_UNKNOWN) { LOGW(LC_WIN, "DISH_DEV_KEYS: unknown key '%s'", what.c_str()); continue; }
            }
            v.push_back(st);
        }
        return v;
    }();
    if (steps.empty()) return;
    static const auto t0 = std::chrono::steady_clock::now();
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    uint32_t main = platform_main_hwnd();
    SDL_Window *win = main ? platform_sdl_window(main) : nullptr;
    if (!win) return;
    static SDL_Joystick *vpad = nullptr;
    for (Step &st : steps) {
        if (st.phase == 2 || now < st.at + (st.phase ? st.hold : 0.0)) continue;
        bool down = st.phase == 0;
        if (st.button >= 0 || st.axis >= 0) {
            if (!vpad) {
                SDL_VirtualJoystickDesc d;
                SDL_INIT_INTERFACE(&d);
                d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
                d.naxes = SDL_GAMEPAD_AXIS_COUNT;
                d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
                d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;   // virtual button i = SDL_GamepadButton i
                d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
                d.name = "dish virtual pad";
                SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
                vpad = id ? SDL_OpenJoystick(id) : nullptr;
                LOGI(LC_WIN, "DISH_DEV_KEYS: virtual gamepad %u %s", id, vpad ? "attached" : SDL_GetError());
                if (!vpad) { st.phase = 2; continue; }
            }
            if (st.button >= 0) SDL_SetJoystickVirtualButton(vpad, st.button, down);
            else {
                float v = down ? st.value : 0.0f;
                SDL_SetJoystickVirtualAxis(vpad, st.axis, (Sint16)(v >= 0 ? v * 32767.0f : v * 32768.0f));
            }
            LOGI(LC_WIN, "DISH_DEV_KEYS: pad %s %s at %.1f s", st.button >= 0 ? SDL_GetGamepadStringForButton((SDL_GamepadButton)st.button)
                                                                   : SDL_GetGamepadStringForAxis((SDL_GamepadAxis)st.axis), down ? "down" : "up", now);
        } else {
            SDL_Event e{};
            e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.windowID = SDL_GetWindowID(win);
            e.key.scancode = st.sc;
            e.key.key = SDL_GetKeyFromScancode(st.sc, SDL_KMOD_NONE, false);
            e.key.down = down;
            SDL_PushEvent(&e);
            LOGI(LC_WIN, "DISH_DEV_KEYS: %s %s at %.1f s", SDL_GetScancodeName(st.sc), down ? "down" : "up", now);
        }
        st.phase++;
    }
}

void platform_pump() {
    if (!SDL_WasInit(SDL_INIT_EVENTS)) return;
    if (mainthread_active() && !on_mainthread()) return;
    platform_dev_input();
    SDL_Event e;
    while (SDL_PollEvent(&e)) platform_route_event(e);
}

SDL_Window *platform_sdl_window(uint32_t hwnd) {
    WinRef w = get_window(hwnd);
    return w ? w->sdl : nullptr;
}
uint32_t platform_main_hwnd() {
    for (auto &w : all_windows()) if (w->sdl) return w->hwnd;
    return 0;
}
void platform_drawable_size(uint32_t hwnd, int *pw, int *ph) {
    WinRef w = get_window(hwnd);
    *pw = w ? w->cx : 0;
    *ph = w ? w->cy : 0;
    if (w && w->sdl) mainthread_call([&] { SDL_GetWindowSizeInPixels(w->sdl, pw, ph); });
}

void platform_shutdown() {
    for (auto &w : all_windows()) if (w->sdl) mainthread_call([&] { SDL_DestroyWindow(w->sdl); });
    xinput_shutdown();
    dsound_shutdown();
}

#define U(name, conv) HLE("user32.dll", #name, conv, win::u32_##name)
HLE("user32.dll", "RegisterClassW", Stdcall, win::u32_RegisterClassW);
HLE("user32.dll", "RegisterClassA", Stdcall, win::u32_RegisterClassA);
HLE("user32.dll", "RegisterClassExW", Stdcall, win::u32_RegisterClassExW);
HLE("user32.dll", "RegisterClassExA", Stdcall, win::u32_RegisterClassExA);
HLE("user32.dll", "UnregisterClassW", Stdcall, win::u32_UnregisterClassW);
HLE("user32.dll", "UnregisterClassA", Stdcall, win::u32_UnregisterClassA);
HLE("user32.dll", "GetClassLongA", Stdcall, win::u32_GetClassLongA);
HLE("user32.dll", "CreateWindowExW", Stdcall, win::u32_CreateWindowExW);
HLE("user32.dll", "CreateWindowExA", Stdcall, win::u32_CreateWindowExA);
HLE("user32.dll", "ShowWindow", Stdcall, win::u32_ShowWindow);
HLE("user32.dll", "UpdateWindow", Stdcall, win::u32_UpdateWindow);
HLE("user32.dll", "DestroyWindow", Stdcall, win::u32_DestroyWindow);
HLE("user32.dll", "IsWindow", Stdcall, win::u32_IsWindow);
HLE("user32.dll", "IsWindowVisible", Stdcall, win::u32_IsWindowVisible);
HLE("user32.dll", "IsIconic", Stdcall, win::u32_IsIconic);
HLE("user32.dll", "IsZoomed", Stdcall, win::u32_IsZoomed);
HLE("user32.dll", "IsWindowEnabled", Stdcall, win::u32_IsWindowEnabled);
HLE("user32.dll", "EnableWindow", Stdcall, win::u32_EnableWindow);
HLE("user32.dll", "GetParent", Stdcall, win::u32_GetParent);
HLE("user32.dll", "GetDesktopWindow", Stdcall, win::u32_GetDesktopWindow);
HLE("user32.dll", "GetWindow", Stdcall, win::u32_GetWindow);
HLE("user32.dll", "GetTopWindow", Stdcall, win::u32_GetTopWindow);
HLE("user32.dll", "FindWindowW", Stdcall, win::u32_FindWindowW);
HLE("user32.dll", "FindWindowA", Stdcall, win::u32_FindWindowA);
HLE("user32.dll", "GetWindowThreadProcessId", Stdcall, win::u32_GetWindowThreadProcessId);
HLE("user32.dll", "GetWindowRect", Stdcall, win::u32_GetWindowRect);
HLE("user32.dll", "GetClientRect", Stdcall, win::u32_GetClientRect);
HLE("user32.dll", "ClientToScreen", Stdcall, win::u32_ClientToScreen);
HLE("user32.dll", "ScreenToClient", Stdcall, win::u32_ScreenToClient);
HLE("user32.dll", "MapWindowPoints", Stdcall, win::u32_MapWindowPoints);
HLE("user32.dll", "AdjustWindowRect", Stdcall, win::u32_AdjustWindowRect);
HLE("user32.dll", "AdjustWindowRectEx", Stdcall, win::u32_AdjustWindowRectEx);
HLE("user32.dll", "SetWindowPos", Stdcall, win::u32_SetWindowPos);
HLE("user32.dll", "MoveWindow", Stdcall, win::u32_MoveWindow);
HLE("user32.dll", "GetWindowPlacement", Stdcall, win::u32_GetWindowPlacement);
HLE("user32.dll", "SetWindowPlacement", Stdcall, win::u32_SetWindowPlacement);
HLE("user32.dll", "GetWindowLongA", Stdcall, win::u32_GetWindowLongA);
HLE("user32.dll", "GetWindowLongW", Stdcall, win::u32_GetWindowLongW);
HLE("user32.dll", "SetWindowLongA", Stdcall, win::u32_SetWindowLongA);
HLE("user32.dll", "SetWindowLongW", Stdcall, win::u32_SetWindowLongW);
HLE("user32.dll", "SetWindowTextW", Stdcall, win::u32_SetWindowTextW);
HLE("user32.dll", "SetWindowTextA", Stdcall, win::u32_SetWindowTextA);
HLE("user32.dll", "GetWindowTextW", Stdcall, win::u32_GetWindowTextW);
HLE("user32.dll", "GetWindowTextA", Stdcall, win::u32_GetWindowTextA);
HLE("user32.dll", "GetWindowTextLengthW", Stdcall, win::u32_GetWindowTextLengthW);
HLE("user32.dll", "GetFocus", Stdcall, win::u32_GetFocus);
HLE("user32.dll", "GetForegroundWindow", Stdcall, win::u32_GetForegroundWindow);
HLE("user32.dll", "GetActiveWindow", Stdcall, win::u32_GetActiveWindow);
HLE("user32.dll", "SetFocus", Stdcall, win::u32_SetFocus);
HLE("user32.dll", "SetActiveWindow", Stdcall, win::u32_SetActiveWindow);
HLE("user32.dll", "SetForegroundWindow", Stdcall, win::u32_SetForegroundWindow);
HLE("user32.dll", "BringWindowToTop", Stdcall, win::u32_BringWindowToTop);
HLE("user32.dll", "GetCapture", Stdcall, win::u32_GetCapture);
HLE("user32.dll", "SetCapture", Stdcall, win::u32_SetCapture);
HLE("user32.dll", "ReleaseCapture", Stdcall, win::u32_ReleaseCapture);
HLE("user32.dll", "PeekMessageW", Stdcall, win::u32_PeekMessage);
HLE("user32.dll", "PeekMessageA", Stdcall, win::u32_PeekMessage);
HLE("user32.dll", "GetMessageW", Stdcall, win::u32_GetMessage);
HLE("user32.dll", "GetMessageA", Stdcall, win::u32_GetMessage);
HLE("user32.dll", "WaitMessage", Stdcall, win::u32_WaitMessage);
HLE("user32.dll", "TranslateMessage", Stdcall, win::u32_TranslateMessage);
HLE("user32.dll", "DispatchMessageW", Stdcall, win::u32_DispatchMessage);
HLE("user32.dll", "DispatchMessageA", Stdcall, win::u32_DispatchMessage);
HLE("user32.dll", "SendMessageW", Stdcall, win::u32_SendMessage);
HLE("user32.dll", "SendMessageA", Stdcall, win::u32_SendMessage);
HLE("user32.dll", "SendMessageTimeoutW", Stdcall, win::u32_SendMessageTimeout);
HLE("user32.dll", "SendMessageTimeoutA", Stdcall, win::u32_SendMessageTimeout);
HLE("user32.dll", "PostMessageW", Stdcall, win::u32_PostMessage);
HLE("user32.dll", "PostMessageA", Stdcall, win::u32_PostMessage);
HLE("user32.dll", "PostThreadMessageW", Stdcall, win::u32_PostThreadMessage);
HLE("user32.dll", "PostThreadMessageA", Stdcall, win::u32_PostThreadMessage);
HLE("user32.dll", "PostQuitMessage", Stdcall, win::u32_PostQuitMessage);
HLE("user32.dll", "DefWindowProcW", Stdcall, win::u32_DefWindowProc);
HLE("user32.dll", "DefWindowProcA", Stdcall, win::u32_DefWindowProc);
HLE("user32.dll", "CallWindowProcW", Stdcall, win::u32_CallWindowProc);
HLE("user32.dll", "CallWindowProcA", Stdcall, win::u32_CallWindowProc);
HLE("user32.dll", "RegisterWindowMessageW", Stdcall, win::u32_RegisterWindowMessage);
HLE("user32.dll", "InSendMessage", Stdcall, win::u32_InSendMessage);
HLE("user32.dll", "SetTimer", Stdcall, win::u32_SetTimer);
HLE("user32.dll", "KillTimer", Stdcall, win::u32_KillTimer);

}  // namespace dish
