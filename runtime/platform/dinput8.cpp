// DirectInput 8: system mouse and keyboard over the SDL event stream (what UE3 creates: one mouse device, buffered GetDeviceData).
// Game controllers are not exposed here: the game reads pads through XInput.
#include "win_internal.h"
#include "dish/call.h"
#include "dish/com.h"
#include "dish/heap.h"
#include "dish/hle.h"
#include "dish/log.h"
#include "dish/mainthread.h"
#include "dish/platform.h"
#include "dish/thread.h"
#include "dish/util.h"
#include <cstring>

namespace dish {
namespace {

constexpr uint32_t DI_OK = 0, DI_NOEFFECT = 1, DIERR_INVALIDPARAM = 0x80070057u, DIERR_NOINTERFACE = 0x80004002u, DIERR_DEVICENOTREG = 0x80040154u,
                   DIERR_NOTACQUIRED = 0x8007000Cu, DIERR_UNSUPPORTED = 0x80004001u;
struct Guid { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; };
bool geq(uint32_t g, const Guid &x) { return memcmp(guest_base + g, &x, 16) == 0; }
const Guid IID_IUnknown_{0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const Guid IID_DI8A{0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const Guid IID_DI8W{0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const Guid IID_DID8A{0x54D41080, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};
const Guid IID_DID8W{0x54D41081, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};
const Guid GUID_SysMouse_{0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const Guid GUID_SysKeyboard_{0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

struct Input { bool wide; };
struct DiDevice {
    bool wide = true, mouse = true, acquired = false, exclusive = false;
    uint32_t data_size = 20;       // size of the data format set by SetDataFormat
    uint32_t buffer_size = 0;
    uint32_t hwnd = 0;
    uint32_t event_handle = 0;
};
uint32_t g_vt_di = 0, g_vt_dev = 0;
std::atomic<int> g_relative_users{0};
#define ARG(i) (*reinterpret_cast<const uint32_t *>(guest_base + esp + 4 + 4 * (i)))
DiDevice *D(uint32_t esp) { return static_cast<DiDevice *>(com_host_object_raw(ARG(0))); }

GuestRet di_QI(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *in = static_cast<Input *>(com_host_object_raw(ARG(0)));
    uint32_t riid = ARG(1), ppv = ARG(2);
    if (!in || !riid || !ppv) return guest_ret(DIERR_INVALIDPARAM, 0, 12);
    if (geq(riid, IID_IUnknown_) || geq(riid, in->wide ? IID_DI8W : IID_DI8A)) { com_addref(ARG(0)); wr32(ppv, ARG(0)); return guest_ret(DI_OK, 0, 12); }
    wr32(ppv, 0);
    return guest_ret(DIERR_NOINTERFACE, 0, 12);
}
GuestRet dd_QI(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    DiDevice *d = D(esp);
    uint32_t riid = ARG(1), ppv = ARG(2);
    if (!d || !riid || !ppv) return guest_ret(DIERR_INVALIDPARAM, 0, 12);
    if (geq(riid, IID_IUnknown_) || geq(riid, d->wide ? IID_DID8W : IID_DID8A)) { com_addref(ARG(0)); wr32(ppv, ARG(0)); return guest_ret(DI_OK, 0, 12); }
    wr32(ppv, 0);
    return guest_ret(DIERR_NOINTERFACE, 0, 12);
}
GuestRet ud_AddRef(uint32_t esp, uint32_t, uint32_t, uint32_t) { return guest_ret(com_addref(ARG(0)), 0, 4); }
GuestRet ud_Release(uint32_t esp, uint32_t, uint32_t, uint32_t) { return guest_ret(com_release(ARG(0)), 0, 4); }

void set_relative(DiDevice *d, bool on) {
    win::WinRef w = win::get_window(d->hwnd ? d->hwnd : platform_main_hwnd());
    if (!w || !w->sdl) return;
    mainthread_call([&] { SDL_SetWindowRelativeMouseMode(w->sdl, on); });
}
void destroy_device(void *p) {
    auto *d = static_cast<DiDevice *>(p);
    if (d->acquired && d->mouse && d->exclusive && --g_relative_users == 0) set_relative(d, false);
    delete d;
}
void destroy_di(void *p) { delete static_cast<Input *>(p); }

void fill_instance(uint32_t p, bool wide, bool mouse) {
    uint32_t size = wide ? 1100 : 580;
    memset(guest_base + p, 0, size);
    wr32(p, size);
    memcpy(guest_base + p + 4, mouse ? &GUID_SysMouse_ : &GUID_SysKeyboard_, 16);
    memcpy(guest_base + p + 20, mouse ? &GUID_SysMouse_ : &GUID_SysKeyboard_, 16);
    wr32(p + 36, mouse ? 0x112 : 0x413);
    if (wide) { write_wstr(p + 40, 260, mouse ? u"Mouse" : u"Keyboard"); write_wstr(p + 560, 260, mouse ? u"Mouse" : u"Keyboard"); }
    else { write_astr(p + 40, 260, mouse ? "Mouse" : "Keyboard"); write_astr(p + 300, 260, mouse ? "Mouse" : "Keyboard"); }
}

GuestRet di_CreateDevice(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, REFGUID, DID8**, IUnknown*)
    auto *in = static_cast<Input *>(com_host_object_raw(ARG(0)));
    if (!in || !ARG(1) || !ARG(2)) return guest_ret(DIERR_INVALIDPARAM, 0, 16);
    bool mouse = geq(ARG(1), GUID_SysMouse_);
    if (!mouse && !geq(ARG(1), GUID_SysKeyboard_)) { wr32(ARG(2), 0); return guest_ret(DIERR_DEVICENOTREG, 0, 16); }
    auto *d = new DiDevice();
    d->wide = in->wide;
    d->mouse = mouse;
    d->data_size = mouse ? 20 : 256;
    uint32_t g = com_new_object(g_vt_dev, d, destroy_device);
    wr32(ARG(2), g);
    LOGI(LC_INPUT, "DirectInput: created %s device", mouse ? "mouse" : "keyboard");
    return guest_ret(g ? DI_OK : 0x8007000Eu, 0, 16);
}
GuestRet di_EnumDevicesThunk(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, devType, callback, ctx, flags)
    auto *in = static_cast<Input *>(com_host_object_raw(ARG(0)));
    if (!in || !ARG(2)) return guest_ret(DIERR_INVALIDPARAM, 0, 20);
    uint32_t type = ARG(1), cb = ARG(2), ctx = ARG(3);
    uint32_t inst = heap_alloc(heap_process(), 0, 1100);
    uint32_t sp = esp - 0x40;
    for (int mouse = 1; mouse >= 0; mouse--) {
        uint32_t want = mouse ? 2 : 3;
        if (type != 0 && type != want) continue;
        fill_instance(inst, in->wide, mouse != 0);
        uint32_t r = guest_invoke_from(sp, cb, Conv::Stdcall, inst, ctx);
        if (r == 0) break;   // DIENUM_STOP
    }
    heap_free(heap_process(), 0, inst);
    return guest_ret(DI_OK, 0, 20);
}
GuestRet di_GetDeviceStatus(uint32_t esp, uint32_t, uint32_t, uint32_t) { return guest_ret(geq(ARG(1), GUID_SysMouse_) || geq(ARG(1), GUID_SysKeyboard_) ? DI_OK : 1 /*DI_NOTATTACHED*/, 0, 8); }
GuestRet di_RunControlPanel(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DI_OK, 0, 12); }
GuestRet di_Initialize(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DI_OK, 0, 12); }
GuestRet di_FindDevice(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DIERR_DEVICENOTREG, 0, 16); }
GuestRet di_unsupported6(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DIERR_UNSUPPORTED, 0, 24); }
GuestRet di_unsupported4(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DIERR_UNSUPPORTED, 0, 20); }

// ---- device ---------------------------------------------------------------------------------------------------------------------------
GuestRet dd_GetCapabilities(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    DiDevice *d = D(esp);
    uint32_t p = ARG(1);
    if (!d || !p || rd32(p) < 44) return guest_ret(DIERR_INVALIDPARAM, 0, 8);
    uint32_t sz = rd32(p);
    memset(guest_base + p, 0, sz);
    wr32(p, sz); wr32(p + 4, 1 /*DIDC_ATTACHED*/); wr32(p + 8, d->mouse ? 0x112 : 0x413);
    wr32(p + 12, d->mouse ? 3 : 0); wr32(p + 16, d->mouse ? 8 : 128);
    return guest_ret(DI_OK, 0, 8);
}
GuestRet dd_EnumObjects(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DI_OK, 0, 16); }
// The predefined DIPROP_* "GUIDs" are MAKEDIPROP(n): the integer n cast to a pointer, not a pointer to a GUID.
uint32_t di_prop_id(uint32_t refguid) { return refguid < 0x10000 ? refguid : 0xFFFFFFFFu; }
GuestRet dd_GetProperty(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, REFGUID prop, DIPROPHEADER*)
    DiDevice *d = D(esp);
    if (!d || !ARG(2)) return guest_ret(DIERR_INVALIDPARAM, 0, 12);
    uint32_t prop = di_prop_id(ARG(1));
    if (prop == 1 /*DIPROP_BUFFERSIZE*/) { wr32(ARG(2) + 16, d->buffer_size); return guest_ret(DI_OK, 0, 12); }
    return guest_ret(DIERR_UNSUPPORTED, 0, 12);
}
GuestRet dd_SetProperty(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, REFGUID prop, const DIPROPHEADER*)
    DiDevice *d = D(esp);
    if (!d || !ARG(1) || !ARG(2)) return guest_ret(DIERR_INVALIDPARAM, 0, 12);
    uint32_t prop = di_prop_id(ARG(1));
    if (prop == 1) d->buffer_size = rd32(ARG(2) + 16);          // DIPROP_BUFFERSIZE
    return guest_ret(DI_OK, 0, 12);                              // AXISMODE (relative), RANGE, DEADZONE ...: accepted
}
GuestRet dd_Acquire(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    DiDevice *d = D(esp);
    if (!d) return guest_ret(DIERR_INVALIDPARAM, 0, 4);
    bool was = d->acquired;
    d->acquired = true;
    if (d->mouse) {
        g_mouse.dx = 0; g_mouse.dy = 0; g_mouse.wheel = 0;
        input_mouse_clear();
        if (!was && d->exclusive && g_relative_users++ == 0) set_relative(d, true);
    }
    return guest_ret(was ? DI_NOEFFECT : DI_OK, 0, 4);
}
GuestRet dd_Unacquire(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    DiDevice *d = D(esp);
    if (!d) return guest_ret(DIERR_INVALIDPARAM, 0, 4);
    bool was = d->acquired;
    d->acquired = false;
    if (was && d->mouse && d->exclusive && --g_relative_users == 0) set_relative(d, false);
    return guest_ret(was ? DI_OK : DI_NOEFFECT, 0, 4);
}
uint32_t dik_for_vk(int vk) {
    bool ext;
    int scan = win::vk_to_set1_scan(vk, &ext);
    return scan ? (uint32_t)(scan | (ext ? 0x80 : 0)) : 0;
}
GuestRet dd_GetDeviceState(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, cb, void*)
    DiDevice *d = D(esp);
    uint32_t cb = ARG(1), p = ARG(2);
    if (!d || !p) return guest_ret(DIERR_INVALIDPARAM, 0, 12);
    if (!d->acquired) return guest_ret(DIERR_NOTACQUIRED, 0, 12);
    if (!mainthread_active()) platform_pump();
    if (d->mouse) {
        if (cb != 16 && cb != 20) return guest_ret(DIERR_INVALIDPARAM, 0, 12);
        memset(guest_base + p, 0, cb);
        wr32(p, (uint32_t)g_mouse.dx.exchange(0));
        wr32(p + 4, (uint32_t)g_mouse.dy.exchange(0));
        wr32(p + 8, (uint32_t)g_mouse.wheel.exchange(0));
        uint32_t b = g_mouse.buttons.load();
        for (uint32_t i = 0; i < (cb == 16 ? 4u : 8u); i++) wr8(p + 12 + i, (b >> i) & 1 ? 0x80 : 0);
    } else {
        if (cb != 256) return guest_ret(DIERR_INVALIDPARAM, 0, 12);
        memset(guest_base + p, 0, 256);
        for (int vk = 1; vk < 256; vk++)
            if (g_vk_down[vk].load(std::memory_order_relaxed) & 1) {
                uint32_t dik = dik_for_vk(vk);
                if (dik && dik < 256 && vk != 0x10 && vk != 0x11 && vk != 0x12) wr8(p + dik, 0x80);
            }
    }
    return guest_ret(DI_OK, 0, 12);
}
GuestRet dd_GetDeviceData(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, cbObjectData, DIDEVICEOBJECTDATA*, DWORD *pdwInOut, DWORD flags)
    DiDevice *d = D(esp);
    uint32_t cbo = ARG(1), buf = ARG(2), pcount = ARG(3), flags = ARG(4);
    if (!d || !pcount || (cbo != 16 && cbo != 20)) return guest_ret(DIERR_INVALIDPARAM, 0, 20);
    if (!d->acquired) return guest_ret(DIERR_NOTACQUIRED, 0, 20);
    if (!mainthread_active()) platform_pump();
    if (!d->mouse) { wr32(pcount, 0); return guest_ret(DI_OK, 0, 20); }
    uint32_t want = rd32(pcount);
    if (!buf) {   // flush
        MouseEvent tmp[64];
        while (input_mouse_pop(tmp, 64, false) == 64) {}
        wr32(pcount, 0);
        return guest_ret(DI_OK, 0, 20);
    }
    std::vector<MouseEvent> ev(std::min<uint32_t>(want, 1024));
    size_t n = input_mouse_pop(ev.data(), ev.size(), (flags & 1 /*DIGDD_PEEK*/) != 0);
    for (size_t i = 0; i < n; i++) {
        uint32_t o = buf + (uint32_t)i * cbo;
        wr32(o, ev[i].ofs); wr32(o + 4, ev[i].data); wr32(o + 8, ev[i].time); wr32(o + 12, ev[i].seq);
        if (cbo == 20) wr32(o + 16, 0);
    }
    wr32(pcount, (uint32_t)n);
    return guest_ret(DI_OK, 0, 20);
}
GuestRet dd_SetDataFormat(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    DiDevice *d = D(esp);
    if (!d || !ARG(1)) return guest_ret(DIERR_INVALIDPARAM, 0, 8);
    uint32_t ds = rd32(ARG(1) + 12);   // DIDATAFORMAT::dwDataSize
    if (d->mouse && ds != 16 && ds != 20) return guest_ret(DIERR_INVALIDPARAM, 0, 8);
    if (!d->mouse && ds != 256) return guest_ret(DIERR_INVALIDPARAM, 0, 8);
    d->data_size = ds;
    return guest_ret(DI_OK, 0, 8);
}
GuestRet dd_SetEventNotification(uint32_t esp, uint32_t, uint32_t, uint32_t) { if (DiDevice *d = D(esp)) d->event_handle = ARG(1); return guest_ret(DI_OK, 0, 8); }
GuestRet dd_SetCooperativeLevel(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    DiDevice *d = D(esp);
    if (!d) return guest_ret(DIERR_INVALIDPARAM, 0, 12);
    d->hwnd = ARG(1);
    d->exclusive = (ARG(2) & 1) != 0;
    return guest_ret(DI_OK, 0, 12);
}
GuestRet dd_GetObjectInfo(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DIERR_UNSUPPORTED, 0, 16); }
GuestRet dd_GetDeviceInfo(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    DiDevice *d = D(esp);
    if (!d || !ARG(1)) return guest_ret(DIERR_INVALIDPARAM, 0, 8);
    fill_instance(ARG(1), d->wide, d->mouse);
    return guest_ret(DI_OK, 0, 8);
}
GuestRet dd_RunControlPanel(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DI_OK, 0, 12); }
GuestRet dd_Initialize(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DI_OK, 0, 16); }
GuestRet dd_Poll(uint32_t esp, uint32_t, uint32_t, uint32_t) { DiDevice *d = D(esp); return guest_ret(d && d->acquired ? DI_NOEFFECT : DIERR_NOTACQUIRED, 0, 4); }
#define UNSUP(name, pop) GuestRet name(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DIERR_UNSUPPORTED, 0, pop); }
UNSUP(dd_u8, 8) UNSUP(dd_u12, 12) UNSUP(dd_u16, 16) UNSUP(dd_u20, 20)

void define_interfaces() {
    static bool done = false;
    if (done) return;
    done = true;
    static const ComMethod di[] = {
        {"QueryInterface", di_QI}, {"AddRef", ud_AddRef}, {"Release", ud_Release}, {"CreateDevice", di_CreateDevice}, {"EnumDevices", di_EnumDevicesThunk},
        {"GetDeviceStatus", di_GetDeviceStatus}, {"RunControlPanel", di_RunControlPanel}, {"Initialize", di_Initialize}, {"FindDevice", di_FindDevice},
        {"EnumDevicesBySemantics", di_unsupported6}, {"ConfigureDevices", di_unsupported4},
    };
    static const ComMethod dd[] = {
        {"QueryInterface", dd_QI}, {"AddRef", ud_AddRef}, {"Release", ud_Release}, {"GetCapabilities", dd_GetCapabilities}, {"EnumObjects", dd_EnumObjects},
        {"GetProperty", dd_GetProperty}, {"SetProperty", dd_SetProperty}, {"Acquire", dd_Acquire}, {"Unacquire", dd_Unacquire}, {"GetDeviceState", dd_GetDeviceState},
        {"GetDeviceData", dd_GetDeviceData}, {"SetDataFormat", dd_SetDataFormat}, {"SetEventNotification", dd_SetEventNotification},
        {"SetCooperativeLevel", dd_SetCooperativeLevel}, {"GetObjectInfo", dd_GetObjectInfo}, {"GetDeviceInfo", dd_GetDeviceInfo}, {"RunControlPanel", dd_RunControlPanel},
        {"Initialize", dd_Initialize}, {"CreateEffect", dd_u20}, {"EnumEffects", dd_u16}, {"GetEffectInfo", dd_u12},
        {"GetForceFeedbackState", dd_u8}, {"SendForceFeedbackCommand", dd_u8}, {"EnumCreatedEffectObjects", dd_u16}, {"Escape", dd_u8},
        {"Poll", dd_Poll}, {"SendDeviceData", dd_u20}, {"EnumEffectsInFile", dd_u20}, {"WriteEffectToFile", dd_u20},
        {"BuildActionMap", dd_u16}, {"SetActionMap", dd_u16}, {"GetImageInfo", dd_u8},
    };
    g_vt_di = com_define_interface("IDirectInput8", di, sizeof di / sizeof di[0]);
    g_vt_dev = com_define_interface("IDirectInputDevice8", dd, sizeof dd / sizeof dd[0]);
}

uint32_t DirectInput8Create(uint32_t hinst, uint32_t version, uint32_t riid, uint32_t ppv, uint32_t outer) {
    (void)hinst; (void)version;
    if (!riid || !ppv) return DIERR_INVALIDPARAM;
    if (outer) { wr32(ppv, 0); return 0x80040110u; }
    bool wide;
    if (geq(riid, IID_DI8W) || geq(riid, IID_IUnknown_)) wide = true;
    else if (geq(riid, IID_DI8A)) wide = false;
    else { wr32(ppv, 0); return DIERR_NOINTERFACE; }
    define_interfaces();
    auto *in = new Input{wide};
    uint32_t g = com_new_object(g_vt_di, in, destroy_di);
    wr32(ppv, g);
    return g ? DI_OK : 0x8007000Eu;
}

}  // namespace

HLE("dinput8.dll", "DirectInput8Create", Stdcall, DirectInput8Create);

}  // namespace dish
