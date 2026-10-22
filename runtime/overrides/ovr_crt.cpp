#include "dish/ovr.h"
#include "dish/hle.h"
#include "dish/thread.h"
#include "dish/util.h"
#include <cerrno>
#include <cmath>
#include <cstring>

using namespace dish;

namespace {
inline uint32_t arg(uint32_t esp, int i) { return *(const uint32_t *)(guest_base + esp + 4 + 4 * i); }
inline double argd(uint32_t esp, int byte_off) { double d; memcpy(&d, guest_base + esp + 4 + byte_off, 8); return d; }
inline GuestRet ret(uint32_t eax) { return guest_ret(eax, 0, 0); }
inline double &st1() { return *(double *)guest_thread->runtime; }   // first member of RtThread (static_assert'ed in tests)
inline GuestRet ret_st0(double v) { guest_thread->st0_ret = v; return guest_ret(0, 0, 0); }
}  // namespace

extern "C" {

GuestRet ovr_memcpy(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    uint32_t d = arg(esp, 0), s = arg(esp, 1), n = arg(esp, 2);
    memmove(guest_base + d, guest_base + s, n);
    return ret(d);
}
GuestRet ovr_memmove(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) { return ovr_memcpy(esp, eax, ecx, edx); }
GuestRet ovr_memset(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    uint32_t d = arg(esp, 0), c = arg(esp, 1), n = arg(esp, 2);
    memset(guest_base + d, (int)(c & 0xFF), n);
    return ret(d);
}
GuestRet ovr_memcmp(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    int r = memcmp(guest_base + arg(esp, 0), guest_base + arg(esp, 1), arg(esp, 2));
    return ret((uint32_t)(r < 0 ? -1 : r > 0 ? 1 : 0));
}
GuestRet ovr_memchr(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    uint32_t p = arg(esp, 0), n = arg(esp, 2);
    const void *r = memchr(guest_base + p, (int)(arg(esp, 1) & 0xFF), n);
    return ret(r ? (uint32_t)((const uint8_t *)r - guest_base) : 0);
}
GuestRet ovr_strlen(uint32_t esp, uint32_t, uint32_t, uint32_t) { return ret((uint32_t)strlen((const char *)(guest_base + arg(esp, 0)))); }
GuestRet ovr_wcslen(uint32_t esp, uint32_t, uint32_t, uint32_t) { return ret((uint32_t)gwcslen(arg(esp, 0))); }
GuestRet ovr_strcmp(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    const uint8_t *a = guest_base + arg(esp, 0), *b = guest_base + arg(esp, 1);
    while (*a && *a == *b) { a++; b++; }
    return ret((uint32_t)(*a < *b ? -1 : *a > *b ? 1 : 0));
}
GuestRet ovr_wcscmp(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    const uint16_t *a = (const uint16_t *)(guest_base + arg(esp, 0)), *b = (const uint16_t *)(guest_base + arg(esp, 1));
    while (*a && *a == *b) { a++; b++; }
    return ret((uint32_t)(*a < *b ? -1 : *a > *b ? 1 : 0));
}
GuestRet ovr__stricmp(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    const uint8_t *a = guest_base + arg(esp, 0), *b = guest_base + arg(esp, 1);
    int x, y;
    do {
        x = *a++; y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
    } while (x && x == y);
    return ret((uint32_t)(x - y));
}
GuestRet ovr__strnicmp(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    const uint8_t *a = guest_base + arg(esp, 0), *b = guest_base + arg(esp, 1);
    uint32_t n = arg(esp, 2);
    int x = 0, y = 0;
    while (n--) {
        x = *a++; y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (!x || x != y) break;
    }
    return ret((uint32_t)(x - y));
}
GuestRet ovr__wcsicmp(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    const uint16_t *a = (const uint16_t *)(guest_base + arg(esp, 0)), *b = (const uint16_t *)(guest_base + arg(esp, 1));
    int x, y;
    do {
        x = *a++; y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
    } while (x && x == y);
    return ret((uint32_t)(x - y));
}
GuestRet ovr__wcsnicmp(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    const uint16_t *a = (const uint16_t *)(guest_base + arg(esp, 0)), *b = (const uint16_t *)(guest_base + arg(esp, 1));
    uint32_t n = arg(esp, 2);
    int x = 0, y = 0;
    while (n--) {
        x = *a++; y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (!x || x != y) break;
    }
    return ret((uint32_t)(x - y));
}

GuestRet ovr_floor(uint32_t esp, uint32_t, uint32_t, uint32_t) { return ret_st0(floor(argd(esp, 0))); }
GuestRet ovr_ceil(uint32_t esp, uint32_t, uint32_t, uint32_t) { return ret_st0(ceil(argd(esp, 0))); }

// ---- _controlfp_s: x87 control word and MXCSR live in GuestThread
static uint32_t crt_control_word(const GuestThread *t) {
    uint32_t cw = t->x87_cw, c = 0;
    if (cw & 0x01) c |= 0x10;       // _EM_INVALID
    if (cw & 0x02) c |= 0x80000;    // _EM_DENORMAL
    if (cw & 0x04) c |= 0x08;       // _EM_ZERODIVIDE
    if (cw & 0x08) c |= 0x04;       // _EM_OVERFLOW
    if (cw & 0x10) c |= 0x02;       // _EM_UNDERFLOW
    if (cw & 0x20) c |= 0x01;       // _EM_INEXACT
    c |= ((cw >> 10) & 3) << 8;     // rounding
    switch ((cw >> 8) & 3) { case 0: c |= 0x20000; break; case 2: c |= 0x10000; break; default: break; }
    if (cw & 0x1000) c |= 0x40000;  // _IC_AFFINE
    if (t->mxcsr & 0x8000) c |= 0x01000000;   // _DN_FLUSH
    return c;
}
static void apply_crt_control(GuestThread *t, uint32_t c) {
    uint32_t cw = t->x87_cw & ~0x1F3Fu & ~0x1000u;
    uint32_t mx = t->mxcsr;
    if (c & 0x10) cw |= 0x01;
    if (c & 0x80000) cw |= 0x02;
    if (c & 0x08) cw |= 0x04;
    if (c & 0x04) cw |= 0x08;
    if (c & 0x02) cw |= 0x10;
    if (c & 0x01) cw |= 0x20;
    cw |= ((c >> 8) & 3) << 10;
    switch (c & 0x30000) { case 0x20000: break; case 0x10000: cw |= 2 << 8; break; default: cw |= 3 << 8; break; }
    if (c & 0x40000) cw |= 0x1000;
    // SSE side: exception masks (bits 7..12), rounding (13..14), flush-to-zero (15)
    mx &= ~(0x3Fu << 7);
    mx |= (cw & 0x3F) << 7;
    mx &= ~(3u << 13);
    mx |= ((c >> 8) & 3) << 13;
    mx = (mx & ~0x8000u) | ((c & 0x01000000) ? 0x8000u : 0);
    t->x87_cw = cw;
    t->mxcsr = mx;
}
GuestRet ovr__controlfp_s(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    uint32_t pcur = arg(esp, 0), newc = arg(esp, 1), mask = arg(esp, 2);
    constexpr uint32_t MCW_ALL = 0x0008001F | 0x00000300 | 0x00030000 | 0x00040000 | 0x03000000;
    if (mask & ~MCW_ALL) {
        if (pcur) *(uint32_t *)(guest_base + pcur) = crt_control_word(guest_thread);
        return ret(EINVAL);
    }
    uint32_t cur = crt_control_word(guest_thread);
    if (mask) apply_crt_control(guest_thread, (cur & ~mask) | (newc & mask));
    if (pcur) *(uint32_t *)(guest_base + pcur) = crt_control_word(guest_thread);
    return ret(0);
}

// ---- x87 math helpers
double dish_ci_pow(double b, double e) { return pow(b, e); }
double dish_ci_fmod(double x, double y) { return fmod(x, y); }
double dish_ci_atan2(double y, double x) { return atan2(y, x); }
#define CI1(name, expr) \
    GuestRet ovr__CI##name(uint32_t, uint32_t, uint32_t, uint32_t) { double x = guest_thread->st0_ret; return ret_st0(expr); }
CI1(sqrt, sqrt(x))
CI1(sin, sin(x))
CI1(cos, cos(x))
CI1(tan, tan(x))
CI1(asin, asin(x))
CI1(acos, acos(x))
CI1(atan, atan(x))
CI1(log, log(x))
CI1(log10, log10(x))
CI1(exp, exp(x))
CI1(sinh, sinh(x))
CI1(cosh, cosh(x))
CI1(tanh, tanh(x))
GuestRet ovr__CIpow(uint32_t, uint32_t, uint32_t, uint32_t) { return ret_st0(dish_ci_pow(st1(), guest_thread->st0_ret)); }
GuestRet ovr__CIfmod(uint32_t, uint32_t, uint32_t, uint32_t) { return ret_st0(dish_ci_fmod(st1(), guest_thread->st0_ret)); }
GuestRet ovr__CIatan2(uint32_t, uint32_t, uint32_t, uint32_t) { return ret_st0(dish_ci_atan2(st1(), guest_thread->st0_ret)); }

}  // extern "C"

// Register the overrides as msvcr90 / msvcr80 exports as well (used when the CRT is bound as an HLE module instead of recompiled).
namespace {
struct OvrReg {
    OvrReg(const char *name, GuestFn fn) {
        for (const char *dll : {"msvcr90.dll", "msvcr80.dll"}) hle_register({dll, name, fn, 0, HConv::Cdecl});
    }
};
#define OVR(n) OvrReg _ovr_reg_##n(#n, ovr_##n)
OVR(memcpy); OVR(memmove); OVR(memset); OVR(memcmp); OVR(memchr); OVR(strlen); OVR(wcslen); OVR(strcmp); OVR(wcscmp);
OVR(_stricmp); OVR(_strnicmp); OVR(_wcsicmp); OVR(_wcsnicmp); OVR(floor); OVR(ceil); OVR(_controlfp_s);
OVR(_CIpow); OVR(_CIfmod); OVR(_CIatan2); OVR(_CIsqrt); OVR(_CIsin); OVR(_CIcos); OVR(_CItan); OVR(_CIasin); OVR(_CIacos); OVR(_CIatan);
OVR(_CIlog); OVR(_CIlog10); OVR(_CIexp); OVR(_CIsinh); OVR(_CIcosh); OVR(_CItanh);
}  // namespace
