// user32 over SDL3, driven the way UE3 drives it: a fake guest client that only talks through thunks and a guest WndProc.
#include "testing.h"
#include "dish/platform.h"
#include "dish/heap.h"
#include <SDL3/SDL.h>
#include <cstring>
#include <thread>

using namespace dish;

struct Seen { uint32_t hwnd, msg, wp, lp; };
static std::vector<Seen> g_seen;
constexpr uint32_t WNDPROC = 0x00405000;

static GuestRet wndproc(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    uint32_t hwnd = rd32(esp + 4), msg = rd32(esp + 8), wp = rd32(esp + 12), lp = rd32(esp + 16);
    g_seen.push_back({hwnd, msg, wp, lp});
    if (msg == 0x81) return guest_ret(1, 0, 16);                         // WM_NCCREATE
    if (msg == 0x10 /*WM_CLOSE*/ || msg == 0x14) {                        // forward to DefWindowProcW like a typical WndProc
        uint32_t sp = esp - 32;
        wr32(sp, kReturnSentinel); wr32(sp + 4, hwnd); wr32(sp + 8, msg); wr32(sp + 12, wp); wr32(sp + 16, lp);
        GuestRet r = guest_call(thunk_addr(hle_bind("user32.dll", "DefWindowProcW", 0, -1)), sp, 0, 0, 0);
        return guest_ret(guest_ret_eax(r), 0, 16);
    }
    return guest_ret(0, 0, 16);
}
static int count(uint32_t msg) { int n = 0; for (auto &s : g_seen) if (s.msg == msg) n++; return n; }
static const Seen *last(uint32_t msg) { for (size_t i = g_seen.size(); i-- > 0;) if (g_seen[i].msg == msg) return &g_seen[i]; return nullptr; }

static uint32_t g_hwnd = 0;
static bool video_ok() {
    t::setup_runtime("window");
    setenv("SDL_AUDIODRIVER", "dummy", 0);
    return platform_video_init();
}
#define NEED_VIDEO() do { if (!video_ok()) { fprintf(stderr, "no display: skipping\n"); exit(77); } } while (0)

static void pump_dispatch(int ms = 60) {
    uint32_t msg = t::galloc(32);
    uint64_t end = SDL_GetTicks() + (uint64_t)ms;
    do {
        while (t::hle("user32.dll", "PeekMessageW", {msg, 0, 0, 0, 1}).eax()) {
            t::hle("user32.dll", "TranslateMessage", {msg});
            t::hle("user32.dll", "DispatchMessageW", {msg});
        }
        SDL_Delay(2);
    } while (SDL_GetTicks() < end);
}
static void push_key(SDL_Scancode sc, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = SDL_GetWindowID(platform_sdl_window(g_hwnd));
    e.key.scancode = sc;
    e.key.down = down;
    SDL_PushEvent(&e);
}

TEST(create_window_and_startup_messages) {
    NEED_VIDEO();
    t::register_guest_fn(WNDPROC, wndproc);
    uint32_t cls = t::gwstr(u"UnrealWindowsClass");
    uint32_t wc = t::galloc(48);
    wr32(wc, 48); wr32(wc + 4, 0x3 /*CS_HREDRAW|CS_VREDRAW*/); wr32(wc + 8, WNDPROC); wr32(wc + 20, 0x400000); wr32(wc + 40, cls);
    uint32_t atom = t::hle("user32.dll", "RegisterClassExW", {wc}).eax();
    CHECK(atom != 0);
    CHECK_EQ(t::hle("user32.dll", "RegisterClassExW", {wc}).eax(), 0u);       // duplicate rejected
    // AdjustWindowRect: ask for a 1280x720 client area, like UE3's windowed mode
    uint32_t rc = t::galloc(16);
    wr32(rc + 8, 1280); wr32(rc + 12, 720);
    uint32_t style = 0x00CF0000 /*WS_OVERLAPPEDWINDOW*/;
    CHECK_EQ(t::hle("user32.dll", "AdjustWindowRect", {rc, style, 0}).eax(), 1u);
    int outer_w = (int)rd32(rc + 8) - (int)rd32(rc), outer_h = (int)rd32(rc + 12) - (int)rd32(rc + 4);
    CHECK(outer_w > 1280 && outer_h > 720);
    g_seen.clear();
    auto r = t::hle("user32.dll", "CreateWindowExW", {0, cls, t::gwstr(u"Dishonored"), style | 0x10000000 /*WS_VISIBLE*/, 100, 80, (uint32_t)outer_w, (uint32_t)outer_h, 0, 0, 0x400000, 0});
    g_hwnd = r.eax();
    CHECK(g_hwnd != 0);
    CHECK_EQ(r.pop(), 48u);
    CHECK(platform_sdl_window(g_hwnd) != nullptr);
    CHECK_EQ(count(0x81), 1);   // WM_NCCREATE
    CHECK_EQ(count(0x01), 1);   // WM_CREATE
    CHECK_EQ(count(0x18), 1);   // WM_SHOWWINDOW
    CHECK_EQ(count(0x05), 1);   // WM_SIZE
    CHECK(count(0x06) >= 1);    // WM_ACTIVATE
    const Seen *sz = last(0x05);
    if (sz) { CHECK_EQ(sz->lp & 0xFFFF, 1280u); CHECK_EQ(sz->lp >> 16, 720u); }
    CHECK_EQ(t::hle("user32.dll", "GetClientRect", {g_hwnd, rc}).eax(), 1u);
    CHECK_EQ(rd32(rc + 8), 1280u);
    CHECK_EQ(rd32(rc + 12), 720u);
    CHECK_EQ(t::hle("user32.dll", "GetWindowRect", {g_hwnd, rc}).eax(), 1u);
    CHECK_EQ(rd32(rc + 8) - rd32(rc), (uint32_t)outer_w);
    // focus queries: the window is active after being shown
    CHECK_EQ(t::hle("user32.dll", "GetForegroundWindow", {}).eax(), g_hwnd);
    CHECK_EQ(t::hle("user32.dll", "GetFocus", {}).eax(), g_hwnd);
    CHECK_EQ(t::hle("user32.dll", "GetActiveWindow", {}).eax(), g_hwnd);
    CHECK_EQ(t::hle("user32.dll", "IsWindowVisible", {g_hwnd}).eax(), 1u);
    uint32_t pid = t::galloc(4);
    CHECK_EQ(t::hle("user32.dll", "GetWindowThreadProcessId", {g_hwnd, pid}).eax(), cur_tid());
    CHECK_EQ(rd32(pid), process_id());
    // window words
    CHECK_EQ(t::hle("user32.dll", "GetWindowLongW", {g_hwnd, (uint32_t)-4}).eax(), WNDPROC);
    t::hle("user32.dll", "SetWindowLongW", {g_hwnd, (uint32_t)-21, 0xABCD});
    CHECK_EQ(t::hle("user32.dll", "GetWindowLongW", {g_hwnd, (uint32_t)-21}).eax(), 0xABCDu);
    t::hle("user32.dll", "SetWindowTextW", {g_hwnd, t::gwstr(u"Dishonored (test)")});
    uint32_t tb = t::galloc(100);
    CHECK_EQ(t::hle("user32.dll", "GetWindowTextW", {g_hwnd, tb, 50}).eax(), 17u);
}

TEST(keyboard_mouse_translation) {
    NEED_VIDEO();
    if (!g_hwnd) { fprintf(stderr, "needs the previous test\n"); return; }
    g_seen.clear();
    push_key(SDL_SCANCODE_A, true);
    SDL_Event e;
    SDL_zero(e); e.type = SDL_EVENT_TEXT_INPUT; e.text.windowID = SDL_GetWindowID(platform_sdl_window(g_hwnd)); e.text.text = "a"; SDL_PushEvent(&e);
    pump_dispatch(80);
    const Seen *kd = last(0x100);
    CHECK(kd != nullptr);
    if (kd) {
        CHECK_EQ(kd->wp, (uint32_t)'A');
        CHECK_EQ((kd->lp >> 16) & 0xFF, 0x1Eu);          // PS/2 scan code of A
        CHECK_EQ(kd->lp >> 31, 0u);
    }
    const Seen *ch = last(0x102);
    CHECK(ch != nullptr);
    if (ch) CHECK_EQ(ch->wp, (uint32_t)'a');
    CHECK((t::hle("user32.dll", "GetKeyState", {'A'}).eax() & 0x8000) != 0);
    CHECK((t::hle("user32.dll", "GetAsyncKeyState", {'A'}).eax() & 0x8000) != 0);
    push_key(SDL_SCANCODE_A, false);
    pump_dispatch(60);
    CHECK((t::hle("user32.dll", "GetKeyState", {'A'}).eax() & 0x8000) == 0);
    const Seen *ku = last(0x101);
    CHECK(ku && (ku->lp >> 31) == 1u);
    // modifiers and arrows (extended bit)
    push_key(SDL_SCANCODE_LSHIFT, true);
    push_key(SDL_SCANCODE_LEFT, true);
    pump_dispatch(60);
    CHECK((t::hle("user32.dll", "GetKeyState", {0x10}).eax() & 0x8000) != 0);   // VK_SHIFT
    CHECK((t::hle("user32.dll", "GetKeyState", {0xA0}).eax() & 0x8000) != 0);   // VK_LSHIFT
    kd = last(0x100);
    CHECK(kd && kd->wp == 0x25 && ((kd->lp >> 24) & 1) == 1);
    push_key(SDL_SCANCODE_LSHIFT, false);
    push_key(SDL_SCANCODE_LEFT, false);
    // Alt makes SYSKEY messages
    push_key(SDL_SCANCODE_LALT, true);
    pump_dispatch(60);
    CHECK(count(0x104) >= 1);
    push_key(SDL_SCANCODE_LALT, false);
    // mouse
    g_seen.clear();
    SDL_zero(e); e.type = SDL_EVENT_MOUSE_MOTION; e.motion.windowID = SDL_GetWindowID(platform_sdl_window(g_hwnd)); e.motion.x = 200; e.motion.y = 120; e.motion.xrel = 5; e.motion.yrel = -3;
    SDL_PushEvent(&e);
    SDL_zero(e); e.type = SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.windowID = SDL_GetWindowID(platform_sdl_window(g_hwnd)); e.button.button = SDL_BUTTON_LEFT; e.button.down = true; e.button.clicks = 1; e.button.x = 200; e.button.y = 120;
    SDL_PushEvent(&e);
    SDL_zero(e); e.type = SDL_EVENT_MOUSE_WHEEL; e.wheel.windowID = SDL_GetWindowID(platform_sdl_window(g_hwnd)); e.wheel.y = 2; SDL_PushEvent(&e);
    pump_dispatch(80);
    const Seen *mm = last(0x200);
    CHECK(mm != nullptr);
    if (mm) { CHECK_EQ(mm->lp & 0xFFFF, 200u); CHECK_EQ(mm->lp >> 16, 120u); }
    const Seen *md = last(0x201);
    CHECK(md && (md->wp & 1) == 1);
    const Seen *mw = last(0x20A);
    CHECK(mw && (int16_t)(mw->wp >> 16) == 240);
    CHECK((t::hle("user32.dll", "GetKeyState", {1}).eax() & 0x8000) != 0);     // VK_LBUTTON
    uint32_t pt = t::galloc(8);
    t::hle("user32.dll", "GetCursorPos", {pt});
    uint32_t rc = t::galloc(16);
    t::hle("user32.dll", "GetWindowRect", {g_hwnd, rc});
    t::hle("user32.dll", "ScreenToClient", {g_hwnd, pt});
    CHECK_EQ(rd32(pt), 200u);
    CHECK_EQ(rd32(pt + 4), 120u);
    CHECK_EQ(g_mouse.dx.load() != 0, true);
    // GetCursorInfo
    uint32_t ci = t::galloc(20);
    wr32(ci, 20);
    CHECK_EQ(t::hle("user32.dll", "GetCursorInfo", {ci}).eax(), 1u);
    CHECK_EQ(rd32(ci + 4), 1u);
}

TEST(timers_and_posted_messages) {
    NEED_VIDEO();
    if (!g_hwnd) return;
    g_seen.clear();
    uint32_t id = t::hle("user32.dll", "SetTimer", {g_hwnd, 7, 20, 0}).eax();
    CHECK_EQ(id, 7u);
    pump_dispatch(120);
    CHECK(count(0x113) >= 2);
    CHECK_EQ(t::hle("user32.dll", "KillTimer", {g_hwnd, 7}).eax(), 1u);
    int before = count(0x113);
    pump_dispatch(80);
    CHECK_EQ(count(0x113), before);
    // PostMessage / SendMessage
    t::hle("user32.dll", "PostMessageW", {g_hwnd, 0x8001, 11, 22});
    CHECK_EQ(count(0x8001), 0);                       // queued, not delivered yet
    pump_dispatch(30);
    CHECK_EQ(count(0x8001), 1);
    t::hle("user32.dll", "SendMessageW", {g_hwnd, 0x8002, 33, 44});
    CHECK_EQ(count(0x8002), 1);                       // delivered synchronously
    // filtering in PeekMessage
    t::hle("user32.dll", "PostMessageW", {g_hwnd, 0x8003, 0, 0});
    t::hle("user32.dll", "PostMessageW", {g_hwnd, 0x8004, 0, 0});
    uint32_t msg = t::galloc(32);
    CHECK_EQ(t::hle("user32.dll", "PeekMessageW", {msg, 0, 0x8004, 0x8004, 0}).eax(), 1u);   // PM_NOREMOVE
    CHECK_EQ(rd32(msg + 4), 0x8004u);
    CHECK_EQ(t::hle("user32.dll", "PeekMessageW", {msg, 0, 0x8004, 0x8004, 1}).eax(), 1u);
    CHECK_EQ(t::hle("user32.dll", "PeekMessageW", {msg, 0, 0x8004, 0x8004, 1}).eax(), 0u);
    pump_dispatch(30);
    // GetMessage blocks until something arrives
    std::thread poster([&] { SDL_Delay(40); });
    t::hle("user32.dll", "PostMessageW", {g_hwnd, 0x8005, 0, 0});
    CHECK_EQ(t::hle("user32.dll", "GetMessageW", {msg, 0, 0, 0}).eax(), 1u);
    CHECK_EQ(rd32(msg + 4), 0x8005u);
    poster.join();
}

TEST(display_and_cursor_apis) {
    NEED_VIDEO();
    uint32_t dm = t::galloc(224);
    wr16(dm + 68, 220);
    CHECK_EQ(t::hle("user32.dll", "EnumDisplaySettingsW", {0, (uint32_t)-1, dm}).eax(), 1u);       // ENUM_CURRENT_SETTINGS
    uint32_t cw = rd32(dm + 172), ch = rd32(dm + 176);
    CHECK(cw >= 640 && ch >= 480);
    CHECK_EQ(rd32(dm + 168), 32u);
    CHECK_EQ(t::hle("user32.dll", "GetSystemMetrics", {0}).eax(), cw);
    CHECK_EQ(t::hle("user32.dll", "GetSystemMetrics", {1}).eax(), ch);
    int n = 0;
    bool saw_current = false;
    for (uint32_t i = 0; i < 512; i++) {
        if (!t::hle("user32.dll", "EnumDisplaySettingsW", {0, i, dm}).eax()) break;
        n++;
        if (rd32(dm + 172) == cw && rd32(dm + 176) == ch) saw_current = true;
        CHECK(rd32(dm + 72) & 0x80000);   // DM_PELSWIDTH
    }
    CHECK(n >= 1);
    CHECK(saw_current || n < 3);
    uint32_t dd = t::galloc(848);
    wr32(dd, 840);
    CHECK_EQ(t::hle("user32.dll", "EnumDisplayDevicesW", {0, 0, dd, 0}).eax(), 1u);
    CHECK(utf16_to_utf8((uint16_t *)gp(dd + 4), gwcslen(dd + 4)) == "\\\\.\\DISPLAY1");
    CHECK(rd32(dd + 324) & 4);   // primary
    CHECK_EQ(t::hle("user32.dll", "EnumDisplayDevicesW", {0, 1, dd, 0}).eax(), 0u);
    uint32_t mi = t::galloc(80);
    wr32(mi, 40);
    uint32_t hmon = t::hle("user32.dll", "MonitorFromWindow", {g_hwnd, 2}).eax();
    CHECK(hmon != 0);
    CHECK_EQ(t::hle("user32.dll", "GetMonitorInfoW", {hmon, mi}).eax(), 1u);
    CHECK_EQ(rd32(mi + 12) - rd32(mi + 4), cw);
    CHECK_EQ(rd32(mi + 36) & 1, 1u);
    // cursor visibility counter and clipping
    int32_t c0 = (int32_t)t::hle("user32.dll", "ShowCursor", {0}).eax();
    CHECK_EQ((int32_t)t::hle("user32.dll", "ShowCursor", {1}).eax(), c0 + 1);
    uint32_t rc = t::galloc(16), rc2 = t::galloc(16);
    wr32(rc, 10); wr32(rc + 4, 20); wr32(rc + 8, 300); wr32(rc + 12, 400);
    CHECK_EQ(t::hle("user32.dll", "ClipCursor", {rc}).eax(), 1u);
    t::hle("user32.dll", "GetClipCursor", {rc2});
    CHECK_EQ(rd32(rc2 + 8), 300u);
    t::hle("user32.dll", "ClipCursor", {0});
    t::hle("user32.dll", "GetClipCursor", {rc2});
    CHECK_EQ(rd32(rc2 + 8), cw);                      // unclipped = whole screen
    CHECK_EQ(t::hle("user32.dll", "MessageBoxW", {0, t::gwstr(u"hello"), t::gwstr(u"caption"), 0}).eax(), 1u);   // IDOK
    CHECK(t::hle("user32.dll", "LoadCursorW", {0, 32512}).eax() != 0);
    uint32_t prev = t::hle("user32.dll", "SetCursor", {0x1234}).eax();
    CHECK_EQ(t::hle("user32.dll", "SetCursor", {prev}).eax(), 0x1234u);
}

TEST(close_destroy_and_quit) {
    NEED_VIDEO();
    if (!g_hwnd) return;
    g_seen.clear();
    SDL_Event e;
    SDL_zero(e); e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED; e.window.windowID = SDL_GetWindowID(platform_sdl_window(g_hwnd));
    SDL_PushEvent(&e);
    pump_dispatch(60);
    CHECK_EQ(count(0x10), 1);                          // WM_CLOSE
    CHECK_EQ(count(0x02), 1);                          // WM_DESTROY (DefWindowProc -> DestroyWindow)
    CHECK_EQ(count(0x82), 1);                          // WM_NCDESTROY
    CHECK_EQ(t::hle("user32.dll", "IsWindow", {g_hwnd}).eax(), 0u);
    CHECK(platform_sdl_window(g_hwnd) == nullptr);
    CHECK_EQ(t::hle("user32.dll", "GetFocus", {}).eax(), 0u);
    t::hle("user32.dll", "PostQuitMessage", {3});
    uint32_t msg = t::galloc(32);
    CHECK_EQ(t::hle("user32.dll", "GetMessageW", {msg, 0, 0, 0}).eax(), 0u);       // WM_QUIT ends the loop
    CHECK_EQ(rd32(msg + 4), 0x12u);
    CHECK_EQ(rd32(msg + 8), 3u);
}
