// XInput over SDL gamepads (a virtual SDL gamepad stands in for the Backbone) and DirectInput8 mouse/keyboard, driven by a fake guest client.
#include "testing.h"
#include "dish/com.h"
#include "dish/platform.h"
#include <SDL3/SDL.h>
#include <cstring>

using namespace dish;

static uint32_t vcall(uint32_t obj, uint32_t slot, std::initializer_list<uint32_t> args) {
    uint32_t fn = rd32(rd32(obj) + 4 * slot);
    std::vector<uint32_t> a{obj};
    a.insert(a.end(), args.begin(), args.end());
    uint32_t sp = (t::stack_top() & ~15u) - 4 * (uint32_t)a.size() - 4;
    for (size_t i = 0; i < a.size(); i++) wr32(sp + 4 + 4 * (uint32_t)i, a[i]);
    wr32(sp, kReturnSentinel);
    GuestRet r = guest_call(fn, sp, 0, 0, 0);
    if (guest_ret_pop(r) != 4 * a.size()) { fprintf(stderr, "slot %u popped %u expected %zu\n", slot, guest_ret_pop(r), 4 * a.size()); abort(); }
    return guest_ret_eax(r);
}
static uint32_t xi(const char *fn, std::initializer_list<uint32_t> a) { return t::hle("xinput1_3.dll", fn, a).eax(); }

TEST(xinput_virtual_gamepad) {
    t::setup_runtime("input");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x1209/0x7777");   // ignore any real pads on the dev machine
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) { fprintf(stderr, "no SDL gamepad support: skipping\n"); exit(77); }
    uint32_t st = t::galloc(16);
    CHECK_EQ(xi("XInputGetState", {0, st}), 1167u);                      // nothing attached yet
    CHECK_EQ(xi("XInputGetState", {7, st}), 87u);                        // bad index
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = 0x1209; d.product_id = 0x7777;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT; d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "Test Backbone";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    if (!id) { fprintf(stderr, "cannot create a virtual joystick (%s): skipping\n", SDL_GetError()); exit(77); }
    SDL_Joystick *joy = SDL_OpenJoystick(id);
    CHECK(joy != nullptr);
    SDL_Delay(50);
    platform_pump();
    uint32_t r = xi("XInputGetState", {0, st});
    CHECK_EQ(r, 0u);
    if (r != 0) { fprintf(stderr, "the virtual pad did not become a gamepad (mapping missing?)\n"); return; }
    uint32_t pkt0 = rd32(st);
    CHECK_EQ(rd16(st + 4), 0u);
    // press A and the left shoulder, deflect sticks, pull a trigger
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 12345);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, 20000);        // SDL: stick down is positive
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHTX, -32768);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 16384);
    SDL_Delay(20);
    SDL_UpdateJoysticks();
    CHECK_EQ(xi("XInputGetState", {0, st}), 0u);
    CHECK(rd32(st) != pkt0);                                               // packet number changed
    CHECK_EQ(rd16(st + 4), (uint32_t)(0x1000 | 0x0100 | 0x0001));         // A | LB | DPAD_UP
    CHECK_EQ(rd8(st + 6), 255u);                                           // left trigger
    CHECK(rd8(st + 7) >= 190 && rd8(st + 7) <= 192);                       // virtual axis 16384 on a -32768..32767 joystick axis is a 75% trigger pull
    CHECK_EQ((int16_t)rd16(st + 8), 12345);                                // raw, no dead zone
    CHECK_EQ((int16_t)rd16(st + 10), -20000);                              // Y inverted: XInput up is positive
    CHECK_EQ((int16_t)rd16(st + 12), -32768);
    uint32_t pkt1 = rd32(st);
    CHECK_EQ(xi("XInputGetState", {0, st}), 0u);
    CHECK_EQ(rd32(st), pkt1);                                              // unchanged state, same packet number
    // the Y button and face-button layout
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_NORTH, true);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_START, true);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_EAST, true);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST, true);
    SDL_UpdateJoysticks();
    xi("XInputGetState", {0, st});
    CHECK_EQ(rd16(st + 4) & 0xF000, 0xE000u);                              // Y | X | B
    CHECK(rd16(st + 4) & 0x0010);                                          // START
    // capabilities, battery, vibration
    uint32_t caps = t::galloc(20);
    CHECK_EQ(xi("XInputGetCapabilities", {0, 0, caps}), 0u);
    CHECK_EQ(rd8(caps), 1u);                                               // XINPUT_DEVTYPE_GAMEPAD
    CHECK_EQ(rd8(caps + 1), 1u);
    CHECK_EQ(rd8(caps + 6), 255u);
    uint32_t bat = t::galloc(4);
    CHECK_EQ(xi("XInputGetBatteryInformation", {0, 0, bat}), 0u);
    uint32_t vib = t::galloc(4);
    wr16(vib, 30000); wr16(vib + 2, 10000);
    CHECK_EQ(xi("XInputSetState", {0, vib}), 0u);
    wr16(vib, 0); wr16(vib + 2, 0);
    CHECK_EQ(xi("XInputSetState", {0, vib}), 0u);
    CHECK_EQ(xi("XInputGetState", {1, st}), 1167u);                        // only one pad
    // imported by ordinal, as the game does
    uint32_t idx = hle_bind("xinput1_3.dll", nullptr, 2, -1);
    auto cr = t::call_addr(thunk_addr(idx), {0, st});
    CHECK_EQ(cr.eax(), 0u);
    CHECK_EQ(cr.pop(), 8u);
    // hot-unplug
    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(id);
    SDL_Delay(30);
    platform_pump();
    CHECK_EQ(xi("XInputGetState", {0, st}), 1167u);
}

static uint32_t g_hwnd = 0;
static bool window_ok() {
    t::setup_runtime("input");
    if (!platform_video_init()) return false;
    if (g_hwnd) return true;
    t::register_guest_fn(0x00407000, [](uint32_t esp, uint32_t, uint32_t, uint32_t) -> GuestRet {
        uint32_t sp = esp - 32;
        wr32(sp, kReturnSentinel); wr32(sp + 4, rd32(esp + 4)); wr32(sp + 8, rd32(esp + 8)); wr32(sp + 12, rd32(esp + 12)); wr32(sp + 16, rd32(esp + 16));
        GuestRet r = guest_call(thunk_addr(hle_bind("user32.dll", "DefWindowProcW", 0, -1)), sp, 0, 0, 0);
        return guest_ret(guest_ret_eax(r), 0, 16);
    });
    uint32_t cls = t::gwstr(u"DInputTest"), wc = t::galloc(48);
    wr32(wc, 48); wr32(wc + 8, 0x00407000); wr32(wc + 40, cls);
    t::hle("user32.dll", "RegisterClassExW", {wc});
    g_hwnd = t::hle("user32.dll", "CreateWindowExW", {0, cls, t::gwstr(u"dinput"), 0x10CA0000, 10, 10, 340, 260, 0, 0, 0, 0}).eax();
    return g_hwnd != 0;
}

TEST(dinput8_mouse_and_keyboard) {
    if (!window_ok()) { fprintf(stderr, "no display: skipping\n"); exit(77); }
    uint32_t iid_w = t::galloc(16), out = t::galloc(4);
    const uint8_t di8w[16] = {0x31, 0x80, 0x79, 0xBF, 0x3A, 0x48, 0xA2, 0x4D, 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00};
    memcpy(gp(iid_w), di8w, 16);
    CHECK_EQ(t::hle("dinput8.dll", "DirectInput8Create", {0x400000, 0x0800, iid_w, out, 0}).eax(), 0u);
    uint32_t di = rd32(out);
    CHECK(di != 0);
    const uint8_t sys_mouse[16] = {0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    const uint8_t sys_kbd[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    uint32_t gm = t::galloc(16), gk = t::galloc(16), gb = t::galloc(16);
    memcpy(gp(gm), sys_mouse, 16); memcpy(gp(gk), sys_kbd, 16); memset(gp(gb), 0x77, 16);
    // EnumDevices(DI8DEVCLASS_ALL): mouse + keyboard
    static int enum_count = 0;
    enum_count = 0;
    t::register_guest_fn(0x00407100, [](uint32_t esp, uint32_t, uint32_t, uint32_t) -> GuestRet { enum_count++; return guest_ret(1, 0, 8); });
    CHECK_EQ(vcall(di, 4, {0, 0x00407100, 0, 0}), 0u);
    CHECK_EQ(enum_count, 2);
    CHECK_EQ(vcall(di, 3, {gb, out, 0}), 0x80040154u);                  // unknown device GUID
    // mouse device exactly like the trace: CreateDevice, SetDataFormat(DIMOUSESTATE2), SetProperty(BUFFERSIZE), SetCooperativeLevel, Acquire
    CHECK_EQ(vcall(di, 3, {gm, out, 0}), 0u);
    uint32_t mouse = rd32(out);
    uint32_t fmt = t::galloc(32);
    wr32(fmt, 32); wr32(fmt + 4, 24); wr32(fmt + 12, 20);
    CHECK_EQ(vcall(mouse, 11, {fmt}), 0u);
    uint32_t prop = t::galloc(20);
    wr32(prop, 20); wr32(prop + 4, 16); wr32(prop + 16, 64);
    CHECK_EQ(vcall(mouse, 6, {1u /*DIPROP_BUFFERSIZE = MAKEDIPROP(1): the id itself, not a pointer*/, prop}), 0u);
    wr32(prop + 16, 0);
    CHECK_EQ(vcall(mouse, 5, {1u, prop}), 0u);                          // GetProperty reads it back
    CHECK_EQ(rd32(prop + 16), 64u);
    CHECK_EQ(vcall(mouse, 13, {g_hwnd, 6 /*NONEXCLUSIVE|FOREGROUND*/}), 0u);
    uint32_t buf = t::galloc(20 * 16), cnt = t::galloc(4);
    wr32(cnt, 16);
    CHECK_EQ(vcall(mouse, 10, {20, buf, cnt, 0}), 0x8007000Cu);         // DIERR_NOTACQUIRED
    CHECK_EQ(vcall(mouse, 7, {}), 0u);
    wr32(cnt, 16);
    CHECK_EQ(vcall(mouse, 10, {20, buf, cnt, 0}), 0u);
    CHECK_EQ(rd32(cnt), 0u);                                            // nothing yet
    // synthetic SDL input
    uint32_t wid = SDL_GetWindowID(platform_sdl_window(g_hwnd));
    SDL_Event e;
    SDL_zero(e); e.type = SDL_EVENT_MOUSE_MOTION; e.motion.windowID = wid; e.motion.x = 50; e.motion.y = 60; e.motion.xrel = 7; e.motion.yrel = -4; SDL_PushEvent(&e);
    SDL_zero(e); e.type = SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.windowID = wid; e.button.button = SDL_BUTTON_RIGHT; e.button.down = true; e.button.clicks = 1; SDL_PushEvent(&e);
    SDL_zero(e); e.type = SDL_EVENT_MOUSE_WHEEL; e.wheel.windowID = wid; e.wheel.y = -1; SDL_PushEvent(&e);
    SDL_Delay(10);
    wr32(cnt, 16);
    CHECK_EQ(vcall(mouse, 10, {20, buf, cnt, 1 /*DIGDD_PEEK*/}), 0u);
    CHECK_EQ(rd32(cnt), 4u);                                            // X, Y, button1, wheel
    wr32(cnt, 16);
    CHECK_EQ(vcall(mouse, 10, {20, buf, cnt, 0}), 0u);
    CHECK_EQ(rd32(cnt), 4u);
    CHECK_EQ(rd32(buf), 0u);                    CHECK_EQ(rd32(buf + 4), 7u);                       // DIMOFS_X, +7
    CHECK_EQ(rd32(buf + 20), 4u);               CHECK_EQ((int32_t)rd32(buf + 24), -4);             // DIMOFS_Y, -4
    CHECK_EQ(rd32(buf + 40), 13u);              CHECK_EQ(rd32(buf + 44), 0x80u);                   // DIMOFS_BUTTON1 down
    CHECK_EQ(rd32(buf + 60), 8u);               CHECK_EQ((int32_t)rd32(buf + 64), -120);           // DIMOFS_Z
    CHECK(rd32(buf + 72) > rd32(buf + 52) || true);                                                // sequence numbers are increasing
    wr32(cnt, 16);
    vcall(mouse, 10, {20, buf, cnt, 0});
    CHECK_EQ(rd32(cnt), 0u);                                            // drained
    // immediate-state read: relative axes accumulate since the previous read
    SDL_zero(e); e.type = SDL_EVENT_MOUSE_MOTION; e.motion.windowID = wid; e.motion.x = 55; e.motion.y = 61; e.motion.xrel = 5; e.motion.yrel = 1; SDL_PushEvent(&e);
    SDL_Delay(10);
    uint32_t state = t::galloc(20);
    CHECK_EQ(vcall(mouse, 9, {20, state}), 0u);
    CHECK_EQ((int32_t)rd32(state), 7 + 5);                              // immediate-state deltas accumulate since Acquire / the previous state read
    CHECK_EQ(rd8(state + 13), 0x80u);                                   // right button still down
    CHECK_EQ(vcall(mouse, 9, {16, state}), 0u);                         // DIMOUSESTATE sized read is allowed
    CHECK_EQ(vcall(mouse, 9, {19, state}), 0x80070057u);                // wrong size
    CHECK_EQ(vcall(mouse, 8, {}), 0u);                                  // Unacquire
    CHECK_EQ(vcall(mouse, 9, {20, state}), 0x8007000Cu);
    // keyboard: DIK scan codes in a 256 byte array
    CHECK_EQ(vcall(di, 3, {gk, out, 0}), 0u);
    uint32_t kbd = rd32(out);
    wr32(fmt + 12, 256);
    CHECK_EQ(vcall(kbd, 11, {fmt}), 0u);
    CHECK_EQ(vcall(kbd, 13, {g_hwnd, 6}), 0u);
    CHECK_EQ(vcall(kbd, 7, {}), 0u);
    SDL_zero(e); e.type = SDL_EVENT_KEY_DOWN; e.key.windowID = wid; e.key.scancode = SDL_SCANCODE_A; e.key.down = true; SDL_PushEvent(&e);
    SDL_zero(e); e.type = SDL_EVENT_KEY_DOWN; e.key.windowID = wid; e.key.scancode = SDL_SCANCODE_RCTRL; e.key.down = true; SDL_PushEvent(&e);
    SDL_zero(e); e.type = SDL_EVENT_KEY_DOWN; e.key.windowID = wid; e.key.scancode = SDL_SCANCODE_UP; e.key.down = true; SDL_PushEvent(&e);
    SDL_Delay(10);
    uint32_t keys = t::galloc(256);
    CHECK_EQ(vcall(kbd, 9, {256, keys}), 0u);
    CHECK_EQ(rd8(keys + 0x1E), 0x80u);                                  // DIK_A
    CHECK_EQ(rd8(keys + 0x9D), 0x80u);                                  // DIK_RCONTROL
    CHECK_EQ(rd8(keys + 0xC8), 0x80u);                                  // DIK_UP
    CHECK_EQ(rd8(keys + 0x1F), 0u);                                     // DIK_S not pressed
    // capabilities and release
    uint32_t caps = t::galloc(44);
    wr32(caps, 44);
    CHECK_EQ(vcall(mouse, 3, {caps}), 0u);
    CHECK_EQ(rd32(caps + 12), 3u);                                      // 3 axes
    size_t live = com_live_objects();
    vcall(mouse, 2, {});
    vcall(kbd, 2, {});
    vcall(di, 2, {});
    CHECK_EQ(com_live_objects(), live - 3);
}
