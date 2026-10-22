// user32: keyboard/mouse state, cursor, display and monitor enumeration, metrics, timers' neighbours, dialogs and misc.
#include "win_internal.h"
#include "dish/call.h"
#include "dish/hle.h"
#include "dish/log.h"
#include "dish/mainthread.h"
#include "dish/thread.h"
#include "dish/util.h"
#include <cstring>

namespace dish {
using namespace win;
namespace {

// ---- keyboard ---------------------------------------------------------------------------------------------------
int16_t u32_GetKeyState(int32_t vk) {
    if ((uint32_t)vk > 255) return 0;
    uint8_t s = g_vk_down[vk].load(std::memory_order_relaxed);
    return (int16_t)(((s & 1) ? 0x8000 : 0) | ((s & 2) ? 1 : 0));
}
int16_t u32_GetAsyncKeyState(int32_t vk) {
    if ((uint32_t)vk > 255) return 0;
    return (int16_t)((g_vk_down[vk].load(std::memory_order_relaxed) & 1) ? 0x8000 : 0);
}
uint32_t u32_GetKeyboardState(uint32_t p) {
    for (int i = 0; i < 256; i++) wr8(p + (uint32_t)i, (uint8_t)(((g_vk_down[i] & 1) ? 0x80 : 0) | ((g_vk_down[i] & 2) ? 1 : 0)));
    return 1;
}
uint32_t u32_GetKeyboardLayout(uint32_t) { return 0x04090409; }
uint32_t u32_LoadKeyboardLayoutW(uint32_t, uint32_t) { return 0x04090409; }
uint32_t u32_UnloadKeyboardLayout(uint32_t) { return 1; }
uint32_t u32_MapVirtualKey(uint32_t code, uint32_t type) {
    bool ext = false;
    switch (type) {
        case 0: return (uint32_t)vk_to_set1_scan((int)code, &ext);                 // MAPVK_VK_TO_VSC
        case 1: case 3: return (uint32_t)set1_scan_to_vk((int)(code & 0xFF), (code & 0xE000) != 0);   // VSC_TO_VK(_EX)
        case 2: {                                                                  // VK_TO_CHAR
            if (code >= 'A' && code <= 'Z') return code;
            if (code >= '0' && code <= '9') return code;
            if (code == 0x20) return ' ';
            return 0;
        }
        default: return 0;
    }
}
uint32_t u32_MapVirtualKeyEx(uint32_t code, uint32_t type, uint32_t) { return u32_MapVirtualKey(code, type); }
uint32_t u32_SendInput(uint32_t n, uint32_t, uint32_t) { return n; }   // synthetic input is accepted and ignored
uint32_t u32_GetKeyNameTextW(uint32_t lparam, uint32_t buf, int32_t n) {
    int vk = set1_scan_to_vk((int)((lparam >> 16) & 0xFF), (lparam & (1u << 24)) != 0);
    char tmp[16];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) snprintf(tmp, sizeof tmp, "%c", vk);
    else if (vk >= 0x70 && vk <= 0x87) snprintf(tmp, sizeof tmp, "F%d", vk - 0x6F);
    else snprintf(tmp, sizeof tmp, "Key%02X", vk);
    std::u16string w = utf8_to_utf16(tmp);
    if (n <= 0 || (size_t)n <= w.size()) return 0;
    write_wstr(buf, (size_t)n, w);
    return (uint32_t)w.size();
}

// ---- cursor ----------------------------------------------------------------------------------------------------
std::atomic<int32_t> g_show_count{0};          // Windows starts at 0 with a visible cursor
std::atomic<uint32_t> g_cursor_handle{0};
int32_t g_clip[4] = {0, 0, 0, 0};
bool g_clipped = false;
std::mutex g_cur_mu;

int32_t u32_ShowCursor(uint32_t show) {
    int32_t c = show ? ++g_show_count : --g_show_count;
    on_main([&] { if (c >= 0) SDL_ShowCursor(); else SDL_HideCursor(); });
    return c;
}
uint32_t u32_SetCursor(uint32_t h) { return g_cursor_handle.exchange(h); }
uint32_t u32_GetCursor() { return g_cursor_handle.load(); }
uint32_t u32_GetCursorPos(uint32_t p) {
    wr32(p, (uint32_t)g_cursor_x.load(std::memory_order_relaxed));
    wr32(p + 4, (uint32_t)g_cursor_y.load(std::memory_order_relaxed));
    return 1;
}
uint32_t u32_SetCursorPos(int32_t x, int32_t y) {
    g_cursor_x = x; g_cursor_y = y;
    WinRef w = get_window(g_focus.load());
    if (w && w->sdl) on_main([&] { SDL_WarpMouseInWindow(w->sdl, (float)(x - w->x - w->cl_left), (float)(y - w->y - w->cl_top)); });
    return 1;
}
uint32_t u32_GetCursorInfo(uint32_t p) {
    if (rd32(p) != 20) { set_last_error(87); return 0; }
    wr32(p + 4, g_show_count.load() >= 0 ? 1 : 0);   // CURSOR_SHOWING
    wr32(p + 8, g_cursor_handle.load());
    wr32(p + 12, (uint32_t)g_cursor_x.load());
    wr32(p + 16, (uint32_t)g_cursor_y.load());
    return 1;
}
uint32_t u32_ClipCursor(uint32_t rect) {
    std::lock_guard<std::mutex> lk(g_cur_mu);
    WinRef w = get_window(g_focus.load() ? g_focus.load() : platform_main_hwnd());
    if (!rect) {
        g_clipped = false;
        if (w && w->sdl) on_main([&] { SDL_SetWindowMouseRect(w->sdl, nullptr); SDL_SetWindowMouseGrab(w->sdl, false); });
        return 1;
    }
    for (int i = 0; i < 4; i++) g_clip[i] = (int32_t)rd32(rect + 4 * (uint32_t)i);
    g_clipped = true;
    if (w && w->sdl) {
        SDL_Rect r = {g_clip[0] - w->x - w->cl_left, g_clip[1] - w->y - w->cl_top, g_clip[2] - g_clip[0], g_clip[3] - g_clip[1]};
        on_main([&] { SDL_SetWindowMouseRect(w->sdl, &r); SDL_SetWindowMouseGrab(w->sdl, true); });
    }
    return 1;
}
uint32_t u32_GetClipCursor(uint32_t rect) {
    std::lock_guard<std::mutex> lk(g_cur_mu);
    if (g_clipped) { for (int i = 0; i < 4; i++) wr32(rect + 4 * (uint32_t)i, (uint32_t)g_clip[i]); }
    else {
        const DisplayInfo &d = primary_display();
        wr32(rect, (uint32_t)d.x); wr32(rect + 4, (uint32_t)d.y); wr32(rect + 8, (uint32_t)(d.x + d.w)); wr32(rect + 12, (uint32_t)(d.y + d.h));
    }
    return 1;
}
uint32_t fake_handle(uint32_t base, uint32_t res) { return base + (res & 0xFFFF) * 4; }
uint32_t u32_LoadCursor(uint32_t, uint32_t name) { return fake_handle(0x00020000, name < 0x10000 ? name : 0x400); }
uint32_t u32_LoadIcon(uint32_t, uint32_t name) { return fake_handle(0x00040000, name < 0x10000 ? name : 0x400); }
uint32_t u32_LoadImage(uint32_t, uint32_t name, uint32_t type, uint32_t, uint32_t, uint32_t) { return fake_handle(type == 1 ? 0x00040000 : type == 2 ? 0x00020000 : 0x00060000, name < 0x10000 ? name : 0x400); }
uint32_t u32_DestroyCursor(uint32_t) { return 1; }

// ---- metrics and system parameters -----------------------------------------------------------------------------------
uint32_t u32_GetSystemMetrics(int32_t i) {
    const DisplayInfo &d = primary_display();
    switch (i) {
        case 0: return (uint32_t)d.w;  case 1: return (uint32_t)d.h;                  // SM_CXSCREEN / CYSCREEN
        case 2: return 17;  case 3: return 17;                                        // SM_CXVSCROLL / CYHSCROLL
        case 4: return 23;                                                            // SM_CYCAPTION
        case 5: case 6: return 1;                                                     // SM_CXBORDER / CYBORDER
        case 7: case 8: return 3;                                                     // SM_CXDLGFRAME / CYDLGFRAME
        case 11: case 12: return 32;                                                  // SM_CXICON / CYICON
        case 13: case 14: return 32;                                                  // SM_CXCURSOR / CYCURSOR
        case 15: return 20;                                                           // SM_CYMENU
        case 16: return (uint32_t)d.w; case 17: return (uint32_t)d.h - 40;            // SM_CXFULLSCREEN / CYFULLSCREEN
        case 19: return 1;                                                            // SM_MOUSEPRESENT
        case 21: case 20: return 17;
        case 28: case 29: return 136;                                                 // SM_CXMIN / CYMIN
        case 30: case 31: return 18;
        case 32: case 33: return 8;                                                   // SM_CXSIZEFRAME / CYSIZEFRAME
        case 34: case 35: return 136;
        case 36: case 37: return 4;                                                   // SM_CXDOUBLECLK / CYDOUBLECLK
        case 43: return 5;                                                            // SM_CMOUSEBUTTONS
        case 49: case 50: return 16;                                                  // SM_CXSMICON / CYSMICON
        case 76: return (uint32_t)d.x; case 77: return (uint32_t)d.y;                 // SM_XVIRTUALSCREEN / YVIRTUALSCREEN
        case 78: return (uint32_t)d.w; case 79: return (uint32_t)d.h;                 // SM_CXVIRTUALSCREEN / CYVIRTUALSCREEN
        case 80: return 1;                                                            // SM_CMONITORS
        case 81: return 1;                                                            // SM_SAMEDISPLAYFORMAT
        case 4096: return 0;                                                          // SM_REMOTESESSION
        default: return 0;
    }
}
uint32_t spi(uint32_t action, uint32_t param, uint32_t pv) {
    const DisplayInfo &d = primary_display();
    switch (action) {
        case 0x30:   // SPI_GETWORKAREA
            if (pv) { wr32(pv, (uint32_t)d.x); wr32(pv + 4, (uint32_t)d.y); wr32(pv + 8, (uint32_t)(d.x + d.w)); wr32(pv + 12, (uint32_t)(d.y + d.h - 40)); }
            return 1;
        case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x10: case 0x11: case 0x13: case 0x16: case 0x0F: case 0x14: case 0x15:
        default:
            if (pv && param >= 4 && param < 0x1000) memset(guest_base + pv, 0, param);   // STICKYKEYS/FILTERKEYS/TOGGLEKEYS style structures: cbSize in param
            if (pv && action >= 0x3A && action <= 0x3F && rd32(pv) >= 8) wr32(pv + 4, 0);
            return 1;
    }
}
uint32_t u32_SystemParametersInfoW(uint32_t a, uint32_t p, uint32_t pv, uint32_t) { return spi(a, p, pv); }
uint32_t u32_SystemParametersInfoA(uint32_t a, uint32_t p, uint32_t pv, uint32_t) { return spi(a, p, pv); }

// ---- display and monitors ------------------------------------------------------------------------------------------
constexpr uint32_t DM_FIELDS = 0x00040000 | 0x00080000 | 0x00100000 | 0x00400000;   // BITSPERPEL | PELSWIDTH | PELSHEIGHT | DISPLAYFREQUENCY
uint32_t fill_devmode(uint32_t p, const DispMode &m, bool wide) {
    uint32_t size = wide ? 220 : 156;
    memset(guest_base + p, 0, size);
    uint32_t o_size = wide ? 68 : 36, o_fields = wide ? 72 : 40, o_bpp = wide ? 168 : 104, o_w = wide ? 172 : 108, o_h = wide ? 176 : 112, o_freq = wide ? 184 : 120;
    if (wide) write_wstr(p, 32, u"\\\\.\\DISPLAY1"); else write_astr(p, 32, "\\\\.\\DISPLAY1");
    wr16(p + (wide ? 64 : 32), 0x0401);
    wr16(p + o_size, (uint16_t)size);
    wr32(p + o_fields, DM_FIELDS | 0x20 /*DM_POSITION*/);
    wr32(p + o_bpp, (uint32_t)m.bpp);
    wr32(p + o_w, (uint32_t)m.w);
    wr32(p + o_h, (uint32_t)m.h);
    wr32(p + o_freq, (uint32_t)m.hz);
    return 1;
}
uint32_t enum_settings(uint32_t devname, int32_t mode, uint32_t p, bool wide) {
    (void)devname;
    const DisplayInfo &d = primary_display();
    if (mode == -1 || mode == -2) {   // ENUM_CURRENT_SETTINGS / ENUM_REGISTRY_SETTINGS
        return fill_devmode(p, {d.w, d.h, 32, d.hz}, wide);
    }
    if (mode < 0 || (size_t)mode >= d.modes.size()) return 0;
    return fill_devmode(p, d.modes[(size_t)mode], wide);
}
uint32_t u32_EnumDisplaySettingsW(uint32_t n, int32_t m, uint32_t p) { return enum_settings(n, m, p, true); }
uint32_t u32_EnumDisplaySettingsA(uint32_t n, int32_t m, uint32_t p) { return enum_settings(n, m, p, false); }
uint32_t u32_EnumDisplaySettingsExW(uint32_t n, int32_t m, uint32_t p, uint32_t) { return enum_settings(n, m, p, true); }
uint32_t u32_EnumDisplaySettingsExA(uint32_t n, int32_t m, uint32_t p, uint32_t) { return enum_settings(n, m, p, false); }
uint32_t enum_devices(uint32_t, uint32_t idx, uint32_t p, bool wide) {
    if (idx != 0) return 0;
    uint32_t cb = rd32(p);
    if (cb < (wide ? 840u : 424u)) return 0;
    memset(guest_base + p + 4, 0, cb - 4);
    if (wide) {
        write_wstr(p + 4, 32, u"\\\\.\\DISPLAY1");
        write_wstr(p + 68, 128, u"DXVK-native display adapter");
        wr32(p + 324, 1 | 4);   // DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | PRIMARY_DEVICE
        write_wstr(p + 328, 128, u"PCI\\VEN_10DE&DEV_0000");
    } else {
        write_astr(p + 4, 32, "\\\\.\\DISPLAY1");
        write_astr(p + 36, 128, "DXVK-native display adapter");
        wr32(p + 164, 1 | 4);
        write_astr(p + 168, 128, "PCI\\VEN_10DE&DEV_0000");
    }
    return 1;
}
uint32_t u32_EnumDisplayDevicesW(uint32_t d, uint32_t i, uint32_t p, uint32_t f) { return enum_devices(d, i, p, true); }
uint32_t u32_EnumDisplayDevicesA(uint32_t d, uint32_t i, uint32_t p, uint32_t f) { return enum_devices(d, i, p, false); }
uint32_t u32_ChangeDisplaySettings(uint32_t, uint32_t) { return 0; }                   // DISP_CHANGE_SUCCESSFUL; D3D9 drives real mode switches
uint32_t u32_ChangeDisplaySettingsEx(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
constexpr uint32_t kMonitor = 0x00010001;
uint32_t u32_MonitorFromWindow(uint32_t, uint32_t) { return kMonitor; }
uint32_t u32_MonitorFromPoint(int32_t, int32_t, uint32_t) { return kMonitor; }
uint32_t u32_MonitorFromRect(uint32_t, uint32_t) { return kMonitor; }
uint32_t get_monitor_info(uint32_t mon, uint32_t p, bool wide) {
    if (mon != kMonitor) { set_last_error(1461); return 0; }
    uint32_t cb = rd32(p);
    if (cb != 40 && cb != 72) return 0;
    const DisplayInfo &d = primary_display();
    wr32(p + 4, (uint32_t)d.x); wr32(p + 8, (uint32_t)d.y); wr32(p + 12, (uint32_t)(d.x + d.w)); wr32(p + 16, (uint32_t)(d.y + d.h));
    wr32(p + 20, (uint32_t)d.x); wr32(p + 24, (uint32_t)d.y); wr32(p + 28, (uint32_t)(d.x + d.w)); wr32(p + 32, (uint32_t)(d.y + d.h - 40));
    wr32(p + 36, 1);   // MONITORINFOF_PRIMARY
    if (cb == 72) { if (wide) write_wstr(p + 40, 32, u"\\\\.\\DISPLAY1"); else write_astr(p + 40, 32, "\\\\.\\DISPLAY1"); }
    return 1;
}
uint32_t u32_GetMonitorInfoW(uint32_t m, uint32_t p) { return get_monitor_info(m, p, true); }
uint32_t u32_GetMonitorInfoA(uint32_t m, uint32_t p) { return get_monitor_info(m, p, false); }
uint32_t u32_EnumDisplayMonitors(const HleCtx &c, uint32_t hdc, uint32_t clip, uint32_t proc, uint32_t data) {
    (void)hdc; (void)clip;
    const DisplayInfo &d = primary_display();
    uint32_t r = c.esp - 0x100;
    wr32(r, (uint32_t)d.x); wr32(r + 4, (uint32_t)d.y); wr32(r + 8, (uint32_t)(d.x + d.w)); wr32(r + 12, (uint32_t)(d.y + d.h));
    guest_invoke_from(c.esp - 0x140, proc, Conv::Stdcall, kMonitor, 0u, r, data);
    return 1;
}

// ---- dialogs, clipboard, hooks, DCs ------------------------------------------------------------------------------------
uint32_t mbox(uint32_t text, uint32_t caption, uint32_t flags, bool wide) {
    std::string t = text ? (wide ? read_wstr_utf8(text) : read_astr_utf8(text)) : "", c = caption ? (wide ? read_wstr_utf8(caption) : read_astr_utf8(caption)) : "";
    LOGW(LC_WIN, "MessageBox [%s]: %s", c.c_str(), t.c_str());
    (void)flags;
    return 1;   // IDOK
}
uint32_t u32_MessageBoxW(uint32_t, uint32_t t, uint32_t c, uint32_t f) { return mbox(t, c, f, true); }
uint32_t u32_MessageBoxA(uint32_t, uint32_t t, uint32_t c, uint32_t f) { return mbox(t, c, f, false); }
uint32_t u32_MessageBoxIndirectW(uint32_t p) { return mbox(rd32(p + 16), rd32(p + 20), rd32(p + 24), true); }
int32_t u32_DialogBoxParamW(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { LOGW(LC_WIN, "DialogBoxParamW is not supported; returning -1"); return -1; }
uint32_t u32_EndDialog(uint32_t, uint32_t) { return 1; }
uint32_t u32_GetDlgItem(uint32_t, uint32_t) { return 0; }
uint32_t u32_SetDlgItemTextW(uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t u32_OpenClipboard(uint32_t) { return 0; }
uint32_t u32_CloseClipboard() { return 1; }
uint32_t u32_EmptyClipboard() { return 0; }
uint32_t u32_GetClipboardData(uint32_t) { return 0; }
uint32_t u32_SetClipboardData(uint32_t, uint32_t) { return 0; }
uint32_t g_next_hook = 0x70001;
uint32_t u32_SetWindowsHookExW(uint32_t id, uint32_t proc, uint32_t, uint32_t) {
    LOGD(LC_WIN, "SetWindowsHookEx(%d, %08x) accepted, never invoked", (int)id, proc);
    return g_next_hook++;
}
uint32_t u32_UnhookWindowsHookEx(uint32_t) { return 1; }
uint32_t u32_CallNextHookEx(uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t u32_RegisterHotKey(uint32_t, uint32_t, uint32_t, uint32_t) { return 1; }
uint32_t u32_UnregisterHotKey(uint32_t, uint32_t) { return 1; }
uint32_t u32_RegisterDeviceNotificationW(uint32_t, uint32_t, uint32_t) { return 0x71001; }
uint32_t u32_UnregisterDeviceNotification(uint32_t) { return 1; }
uint32_t u32_GetDC(uint32_t hwnd) { return 0x00080000 + (hwnd & 0xFFFF0) / 4; }
uint32_t u32_GetDCEx(uint32_t hwnd, uint32_t, uint32_t) { return u32_GetDC(hwnd); }
uint32_t u32_ReleaseDC(uint32_t, uint32_t) { return 1; }
uint32_t u32_BeginPaint(uint32_t hwnd, uint32_t ps) {
    WinRef w = get_window(hwnd);
    memset(guest_base + ps, 0, 64);
    wr32(ps, u32_GetDC(hwnd));
    if (w) { wr32(ps + 12, 0); wr32(ps + 16, 0); wr32(ps + 20, (uint32_t)w->cx); wr32(ps + 24, (uint32_t)w->cy); }
    return rd32(ps);
}
uint32_t u32_EndPaint(uint32_t, uint32_t) { return 1; }
uint32_t u32_InvalidateRect(uint32_t, uint32_t, uint32_t) { return 1; }
uint32_t u32_DrawStateW(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, uint32_t) { return 1; }

}  // namespace

HLE("user32.dll", "GetKeyState", Stdcall, u32_GetKeyState);
HLE("user32.dll", "GetAsyncKeyState", Stdcall, u32_GetAsyncKeyState);
HLE("user32.dll", "GetKeyboardState", Stdcall, u32_GetKeyboardState);
HLE("user32.dll", "GetKeyboardLayout", Stdcall, u32_GetKeyboardLayout);
HLE("user32.dll", "LoadKeyboardLayoutW", Stdcall, u32_LoadKeyboardLayoutW);
HLE("user32.dll", "UnloadKeyboardLayout", Stdcall, u32_UnloadKeyboardLayout);
HLE("user32.dll", "MapVirtualKeyW", Stdcall, u32_MapVirtualKey);
HLE("user32.dll", "MapVirtualKeyA", Stdcall, u32_MapVirtualKey);
HLE("user32.dll", "MapVirtualKeyExW", Stdcall, u32_MapVirtualKeyEx);
HLE("user32.dll", "SendInput", Stdcall, u32_SendInput);
HLE("user32.dll", "GetKeyNameTextW", Stdcall, u32_GetKeyNameTextW);
HLE("user32.dll", "ShowCursor", Stdcall, u32_ShowCursor);
HLE("user32.dll", "SetCursor", Stdcall, u32_SetCursor);
HLE("user32.dll", "GetCursor", Stdcall, u32_GetCursor);
HLE("user32.dll", "GetCursorPos", Stdcall, u32_GetCursorPos);
HLE("user32.dll", "SetCursorPos", Stdcall, u32_SetCursorPos);
HLE("user32.dll", "GetCursorInfo", Stdcall, u32_GetCursorInfo);
HLE("user32.dll", "ClipCursor", Stdcall, u32_ClipCursor);
HLE("user32.dll", "GetClipCursor", Stdcall, u32_GetClipCursor);
HLE("user32.dll", "LoadCursorW", Stdcall, u32_LoadCursor);
HLE("user32.dll", "LoadCursorA", Stdcall, u32_LoadCursor);
HLE("user32.dll", "LoadIconW", Stdcall, u32_LoadIcon);
HLE("user32.dll", "LoadIconA", Stdcall, u32_LoadIcon);
HLE("user32.dll", "LoadImageW", Stdcall, u32_LoadImage);
HLE("user32.dll", "LoadImageA", Stdcall, u32_LoadImage);
HLE("user32.dll", "DestroyCursor", Stdcall, u32_DestroyCursor);
HLE("user32.dll", "DestroyIcon", Stdcall, u32_DestroyCursor);
HLE("user32.dll", "GetSystemMetrics", Stdcall, u32_GetSystemMetrics);
HLE("user32.dll", "SystemParametersInfoW", Stdcall, u32_SystemParametersInfoW);
HLE("user32.dll", "SystemParametersInfoA", Stdcall, u32_SystemParametersInfoA);
HLE("user32.dll", "EnumDisplaySettingsW", Stdcall, u32_EnumDisplaySettingsW);
HLE("user32.dll", "EnumDisplaySettingsA", Stdcall, u32_EnumDisplaySettingsA);
HLE("user32.dll", "EnumDisplaySettingsExW", Stdcall, u32_EnumDisplaySettingsExW);
HLE("user32.dll", "EnumDisplaySettingsExA", Stdcall, u32_EnumDisplaySettingsExA);
HLE("user32.dll", "EnumDisplayDevicesW", Stdcall, u32_EnumDisplayDevicesW);
HLE("user32.dll", "EnumDisplayDevicesA", Stdcall, u32_EnumDisplayDevicesA);
HLE("user32.dll", "ChangeDisplaySettingsA", Stdcall, u32_ChangeDisplaySettings);
HLE("user32.dll", "ChangeDisplaySettingsW", Stdcall, u32_ChangeDisplaySettings);
HLE("user32.dll", "ChangeDisplaySettingsExA", Stdcall, u32_ChangeDisplaySettingsEx);
HLE("user32.dll", "ChangeDisplaySettingsExW", Stdcall, u32_ChangeDisplaySettingsEx);
HLE("user32.dll", "MonitorFromWindow", Stdcall, u32_MonitorFromWindow);
HLE("user32.dll", "MonitorFromPoint", Stdcall, u32_MonitorFromPoint);
HLE("user32.dll", "MonitorFromRect", Stdcall, u32_MonitorFromRect);
HLE("user32.dll", "GetMonitorInfoW", Stdcall, u32_GetMonitorInfoW);
HLE("user32.dll", "GetMonitorInfoA", Stdcall, u32_GetMonitorInfoA);
HLE("user32.dll", "EnumDisplayMonitors", Stdcall, u32_EnumDisplayMonitors);
HLE("user32.dll", "MessageBoxW", Stdcall, u32_MessageBoxW);
HLE("user32.dll", "MessageBoxA", Stdcall, u32_MessageBoxA);
HLE("user32.dll", "MessageBoxIndirectW", Stdcall, u32_MessageBoxIndirectW);
HLE("user32.dll", "DialogBoxParamW", Stdcall, u32_DialogBoxParamW);
HLE("user32.dll", "EndDialog", Stdcall, u32_EndDialog);
HLE("user32.dll", "GetDlgItem", Stdcall, u32_GetDlgItem);
HLE("user32.dll", "SetDlgItemTextW", Stdcall, u32_SetDlgItemTextW);
HLE("user32.dll", "OpenClipboard", Stdcall, u32_OpenClipboard);
HLE("user32.dll", "CloseClipboard", Stdcall, u32_CloseClipboard);
HLE("user32.dll", "EmptyClipboard", Stdcall, u32_EmptyClipboard);
HLE("user32.dll", "GetClipboardData", Stdcall, u32_GetClipboardData);
HLE("user32.dll", "SetClipboardData", Stdcall, u32_SetClipboardData);
HLE("user32.dll", "SetWindowsHookExW", Stdcall, u32_SetWindowsHookExW);
HLE("user32.dll", "UnhookWindowsHookEx", Stdcall, u32_UnhookWindowsHookEx);
HLE("user32.dll", "CallNextHookEx", Stdcall, u32_CallNextHookEx);
HLE("user32.dll", "RegisterHotKey", Stdcall, u32_RegisterHotKey);
HLE("user32.dll", "UnregisterHotKey", Stdcall, u32_UnregisterHotKey);
HLE("user32.dll", "RegisterDeviceNotificationW", Stdcall, u32_RegisterDeviceNotificationW);
HLE("user32.dll", "UnregisterDeviceNotification", Stdcall, u32_UnregisterDeviceNotification);
HLE("user32.dll", "GetDC", Stdcall, u32_GetDC);
HLE("user32.dll", "GetDCEx", Stdcall, u32_GetDCEx);
HLE("user32.dll", "GetWindowDC", Stdcall, u32_GetDC);
HLE("user32.dll", "ReleaseDC", Stdcall, u32_ReleaseDC);
HLE("user32.dll", "BeginPaint", Stdcall, u32_BeginPaint);
HLE("user32.dll", "EndPaint", Stdcall, u32_EndPaint);
HLE("user32.dll", "InvalidateRect", Stdcall, u32_InvalidateRect);
HLE("user32.dll", "DrawStateW", Stdcall, u32_DrawStateW);

}  // namespace dish
