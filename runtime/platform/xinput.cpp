// XInput 1.3 over the SDL3 gamepad API (GameController.framework on iOS). Raw values, no dead zone: the game applies its own.
#include "dish/hle.h"
#include "dish/log.h"
#include "dish/mainthread.h"
#include "dish/platform.h"
#include "dish/thread.h"
#include "dish/util.h"
#include <SDL3/SDL.h>
#include <cstring>
#include <mutex>

namespace dish {
namespace {

constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED_ = 1167, ERROR_EMPTY_ = 4306;
struct Slot {
    SDL_JoystickID id = 0;
    SDL_Gamepad *pad = nullptr;
    uint32_t packet = 0;
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
std::mutex g_mu;
Slot g_slots[4];
bool g_init = false;

void open_pad(SDL_JoystickID id) {   // g_mu held
    for (auto &s : g_slots) if (s.pad && s.id == id) return;
    for (auto &s : g_slots) {
        if (s.pad) continue;
        SDL_Gamepad *p = SDL_OpenGamepad(id);
        if (!p) { LOGW(LC_INPUT, "SDL_OpenGamepad(%u) failed: %s", id, SDL_GetError()); return; }
        s = Slot();
        s.id = id;
        s.pad = p;
        LOGI(LC_INPUT, "gamepad %u '%s' -> XInput slot %d", id, SDL_GetGamepadName(p) ? SDL_GetGamepadName(p) : "?", (int)(&s - g_slots));
        return;
    }
}
void close_pad(SDL_JoystickID id) {
    for (auto &s : g_slots)
        if (s.pad && s.id == id) { SDL_CloseGamepad(s.pad); s = Slot(); }
}
void ensure_init() {   // g_mu held
    if (g_init) return;
    g_init = true;
    mainthread_call([] {
        if (!SDL_WasInit(SDL_INIT_GAMEPAD)) SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS);
    });
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) open_pad(ids[i]);
    if (ids) SDL_free(ids);
}

uint8_t trig(SDL_Gamepad *p, SDL_GamepadAxis a) {
    int v = SDL_GetGamepadAxis(p, a);
    if (v < 0) v = 0;
    return (uint8_t)((v * 255 + 16383) / 32767);
}
int16_t stick_y(int v) { return (int16_t)(v == -32768 ? 32767 : -v); }   // SDL: down is positive; XInput: up is positive

void read_slot(Slot &s) {
    SDL_Gamepad *p = s.pad;
    struct B { SDL_GamepadButton b; uint16_t m; };
    static const B map[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
        {SDL_GAMEPAD_BUTTON_START, 0x0010}, {SDL_GAMEPAD_BUTTON_BACK, 0x0020}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200}, {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000}, {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
    };
    uint16_t b = 0;
    for (auto &m : map) if (SDL_GetGamepadButton(p, m.b)) b |= m.m;
    uint16_t guide = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_GUIDE) ? 0x0400 : 0;
    uint8_t lt = trig(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), rt = trig(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    int16_t lx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), ly = stick_y(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY));
    int16_t rx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX), ry = stick_y(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY));
    if (b != s.buttons) LOGD(LC_INPUT, "XInput buttons %04x -> %04x", s.buttons, b);
    if (b != s.buttons || lt != s.lt || rt != s.rt || lx != s.lx || ly != s.ly || rx != s.rx || ry != s.ry) s.packet++;
    s.buttons = b; s.lt = lt; s.rt = rt; s.lx = lx; s.ly = ly; s.rx = rx; s.ry = ry;
    (void)guide;
}

uint32_t get_state(uint32_t idx, uint32_t out, bool ex) {
    if (idx >= 4) return 87;   // ERROR_BAD_ARGUMENTS
    std::lock_guard<std::mutex> lk(g_mu);
    ensure_init();
    if (!g_slots[idx].pad) {
        if (!mainthread_active()) platform_pump();   // lets hot-plug events through when no host loop is running
        if (!g_slots[idx].pad) return ERROR_DEVICE_NOT_CONNECTED_;
    }
    Slot &s = g_slots[idx];
    static bool polled[4];
    if (!polled[idx]) { polled[idx] = true; LOGI(LC_INPUT, "XInput slot %u: first read by the game", idx); }
    static uint32_t reads[4];
    if (++reads[idx] % 600 == 0) LOGD(LC_INPUT, "XInput slot %u: %u reads, buttons %04x", idx, reads[idx], s.buttons);
    if (!mainthread_active()) SDL_UpdateGamepads();
    read_slot(s);
    wr32(out, s.packet);
    uint16_t b = s.buttons | (ex && SDL_GetGamepadButton(s.pad, SDL_GAMEPAD_BUTTON_GUIDE) ? 0x0400 : 0);
    wr16(out + 4, b);
    wr8(out + 6, s.lt); wr8(out + 7, s.rt);
    wr16(out + 8, (uint16_t)s.lx); wr16(out + 10, (uint16_t)s.ly); wr16(out + 12, (uint16_t)s.rx); wr16(out + 14, (uint16_t)s.ry);
    return 0;
}
uint32_t xi_GetState(uint32_t idx, uint32_t out) { return get_state(idx, out, false); }
uint32_t xi_GetStateEx(uint32_t idx, uint32_t out) { return get_state(idx, out, true); }
uint32_t xi_SetState(uint32_t idx, uint32_t vib) {
    if (idx >= 4) return 87;
    std::lock_guard<std::mutex> lk(g_mu);
    ensure_init();
    if (!g_slots[idx].pad) return ERROR_DEVICE_NOT_CONNECTED_;
    uint16_t l = rd16(vib), r = rd16(vib + 2);
    SDL_RumbleGamepad(g_slots[idx].pad, l, r, (l || r) ? 120000 : 0);   // XInput keeps rumbling until told otherwise
    return 0;
}
uint32_t xi_GetCapabilities(uint32_t idx, uint32_t flags, uint32_t caps) {
    if (idx >= 4) return 87;
    std::lock_guard<std::mutex> lk(g_mu);
    ensure_init();
    if (!g_slots[idx].pad) return ERROR_DEVICE_NOT_CONNECTED_;
    memset(guest_base + caps, 0, 20);
    wr8(caps, 1);            // XINPUT_DEVTYPE_GAMEPAD
    wr8(caps + 1, 1);        // XINPUT_DEVSUBTYPE_GAMEPAD
    wr16(caps + 2, 0);       // flags
    wr16(caps + 4, 0xF3FF);  // all buttons
    wr8(caps + 6, 255); wr8(caps + 7, 255);
    wr16(caps + 8, 0xFFFF); wr16(caps + 10, 0xFFFF); wr16(caps + 12, 0xFFFF); wr16(caps + 14, 0xFFFF);
    wr16(caps + 16, 0xFFFF); wr16(caps + 18, 0xFFFF);   // vibration motors
    (void)flags;
    return 0;
}
void xi_Enable(uint32_t) {}
uint32_t xi_GetBatteryInformation(uint32_t idx, uint32_t type, uint32_t info) {
    if (idx >= 4) return 87;
    std::lock_guard<std::mutex> lk(g_mu);
    ensure_init();
    if (!g_slots[idx].pad) return ERROR_DEVICE_NOT_CONNECTED_;
    (void)type;
    wr8(info, 1);       // BATTERY_TYPE_WIRED
    wr8(info + 1, 3);   // BATTERY_LEVEL_FULL
    return 0;
}
uint32_t xi_GetKeystroke(uint32_t idx, uint32_t, uint32_t) {
    std::lock_guard<std::mutex> lk(g_mu);
    ensure_init();
    return idx < 4 && g_slots[idx].pad ? ERROR_EMPTY_ : ERROR_DEVICE_NOT_CONNECTED_;
}
uint32_t xi_GetDSoundAudioDeviceGuids(uint32_t, uint32_t, uint32_t) { return ERROR_DEVICE_NOT_CONNECTED_; }

}  // namespace

void xinput_on_event(const SDL_Event &e) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_init) return;
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) open_pad(e.gdevice.which);
    else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) close_pad(e.gdevice.which);
}
void xinput_shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &s : g_slots) if (s.pad) { SDL_CloseGamepad(s.pad); s = Slot(); }
    g_init = false;
}

HLE("xinput1_3.dll", "XInputGetState", Stdcall, xi_GetState);
HLE("xinput1_3.dll", "XInputGetStateEx", Stdcall, xi_GetStateEx);
HLE("xinput1_3.dll", "XInputSetState", Stdcall, xi_SetState);
HLE("xinput1_3.dll", "XInputGetCapabilities", Stdcall, xi_GetCapabilities);
HLE("xinput1_3.dll", "XInputEnable", Stdcall, xi_Enable);
HLE("xinput1_3.dll", "XInputGetBatteryInformation", Stdcall, xi_GetBatteryInformation);
HLE("xinput1_3.dll", "XInputGetKeystroke", Stdcall, xi_GetKeystroke);
HLE("xinput1_3.dll", "XInputGetDSoundAudioDeviceGuids", Stdcall, xi_GetDSoundAudioDeviceGuids);

}  // namespace dish
