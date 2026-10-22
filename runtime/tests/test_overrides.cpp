#include "testing.h"
#include "dish/ovr.h"
#include <cmath>
#include <cstring>
#include <thread>

using namespace dish;

static GuestRet call_ovr(GuestFn f, std::initializer_list<uint32_t> args) {
    uint32_t sp = t::stack_top() & ~15u;
    sp -= 4 * (uint32_t)args.size();
    uint32_t i = 0;
    for (uint32_t a : args) wr32(sp + 4 * i++, a);
    sp -= 4;
    wr32(sp, kReturnSentinel);
    return f(sp, 0, 0, 0);
}

TEST(mem_functions) {
    t::setup_runtime("ovr");
    uint32_t a = t::galloc(64), b = t::galloc(64);
    for (int i = 0; i < 64; i++) gp(a)[i] = (uint8_t)i;
    GuestRet r = call_ovr(ovr_memcpy, {b, a, 64});
    CHECK_EQ(guest_ret_eax(r), b);
    CHECK_EQ(guest_ret_pop(r), 0u);                    // cdecl
    CHECK(memcmp(gp(a), gp(b), 64) == 0);
    call_ovr(ovr_memmove, {a + 8, a, 32});             // overlapping, forward
    for (int i = 0; i < 32; i++) CHECK_EQ(gp(a)[8 + i], (uint8_t)i);
    call_ovr(ovr_memset, {b, 0x1FF, 16});              // only the low byte is used
    CHECK_EQ(gp(b)[15], 0xFF);
    CHECK_EQ(gp(b)[16], 16);
    CHECK_EQ(guest_ret_eax(call_ovr(ovr_memcmp, {a, a, 10})), 0u);
    CHECK((int32_t)guest_ret_eax(call_ovr(ovr_memcmp, {b, a, 10})) > 0);
    CHECK_EQ(guest_ret_eax(call_ovr(ovr_memchr, {a, 20, 64})), a + 28u);   // after the overlapping move a[8+i] == i, so 20 sits at index 28
}

TEST(string_functions) {
    t::setup_runtime("ovr");
    CHECK_EQ(guest_ret_eax(call_ovr(ovr_strlen, {t::gstr("hello")})), 5u);
    CHECK_EQ(guest_ret_eax(call_ovr(ovr_wcslen, {t::gwstr(u"héllo w")})), 7u);
    CHECK_EQ(guest_ret_eax(call_ovr(ovr__stricmp, {t::gstr("AbC"), t::gstr("aBc")})), 0u);
    CHECK((int32_t)guest_ret_eax(call_ovr(ovr__stricmp, {t::gstr("abc"), t::gstr("abd")})) < 0);
    CHECK((int32_t)guest_ret_eax(call_ovr(ovr__stricmp, {t::gstr("b"), t::gstr("A")})) > 0);
    CHECK_EQ(guest_ret_eax(call_ovr(ovr__strnicmp, {t::gstr("HELLOx"), t::gstr("helloy"), 5})), 0u);
    CHECK_EQ(guest_ret_eax(call_ovr(ovr__wcsicmp, {t::gwstr(u"Engine\\Config"), t::gwstr(u"ENGINE\\config")})), 0u);
    CHECK((int32_t)guest_ret_eax(call_ovr(ovr__wcsicmp, {t::gwstr(u"abc"), t::gwstr(u"abcd")})) < 0);
    CHECK_EQ(guest_ret_eax(call_ovr(ovr__wcsnicmp, {t::gwstr(u"UPK_a"), t::gwstr(u"upk_B"), 4})), 0u);
    CHECK_EQ(guest_ret_eax(call_ovr(ovr_strcmp, {t::gstr("a"), t::gstr("a")})), 0u);
    CHECK_EQ(guest_ret_eax(call_ovr(ovr_wcscmp, {t::gwstr(u"a"), t::gwstr(u"b")})), 0xFFFFFFFFu);
}

static double st0() { return guest_thread->st0_ret; }

TEST(floor_ceil_and_x87_helpers) {
    t::setup_runtime("ovr");
    uint32_t w[2];
    double v = -2.5;
    memcpy(w, &v, 8);
    call_ovr(ovr_floor, {w[0], w[1]});
    CHECK(st0() == -3.0);
    call_ovr(ovr_ceil, {w[0], w[1]});
    CHECK(st0() == -2.0);
    // _CI* helpers: st0 in GuestThread::st0_ret, st1 in the runtime-private block's first double (interim convention)
    static_assert(offsetof(RtThread, st1_arg) == 0, "interim ABI: st1 must be the first member of the runtime block");
    double &st1 = *(double *)guest_thread->runtime;
    guest_thread->st0_ret = 16.0;
    call_ovr(ovr__CIsqrt, {});
    CHECK(st0() == 4.0);
    st1 = 2.0;                                         // base
    guest_thread->st0_ret = 10.0;                      // exponent
    call_ovr(ovr__CIpow, {});
    CHECK(st0() == 1024.0);
    st1 = 7.5;
    guest_thread->st0_ret = 2.0;
    call_ovr(ovr__CIfmod, {});
    CHECK(st0() == 1.5);                               // fmod(st1, st0)
    st1 = 1.0;
    guest_thread->st0_ret = 1.0;
    call_ovr(ovr__CIatan2, {});
    CHECK(fabs(st0() - M_PI / 4) < 1e-15);
    guest_thread->st0_ret = 0.5;
    call_ovr(ovr__CIasin, {});
    CHECK(fabs(st0() - M_PI / 6) < 1e-15);
    guest_thread->st0_ret = 1.0;
    call_ovr(ovr__CIlog, {});
    CHECK(st0() == 0.0);
    guest_thread->st0_ret = 100.0;
    call_ovr(ovr__CIlog10, {});
    CHECK(st0() == 2.0);
}

TEST(controlfp_s) {
    t::setup_runtime("ovr");
    uint32_t cur = t::galloc(4);
    // defaults: x87 0x027F = 53-bit precision, round to nearest, all exceptions masked
    CHECK_EQ(guest_ret_eax(call_ovr(ovr__controlfp_s, {cur, 0, 0})), 0u);
    uint32_t c = rd32(cur);
    CHECK_EQ(c & 0x30000, 0x10000u);                   // _PC_53
    CHECK_EQ(c & 0x300, 0u);                           // _RC_NEAR
    CHECK_EQ(c & 0x8001F, 0x8001Fu);                   // all exceptions masked
    // switch to 24-bit precision and round toward zero, as D3D9 does
    CHECK_EQ(guest_ret_eax(call_ovr(ovr__controlfp_s, {cur, 0x20000 | 0x300, 0x30000 | 0x300})), 0u);
    CHECK_EQ(rd32(cur) & 0x30000, 0x20000u);
    CHECK_EQ(rd32(cur) & 0x300, 0x300u);
    CHECK_EQ((guest_thread->x87_cw >> 8) & 3, 0u);     // x87 PC field = 24-bit
    CHECK_EQ((guest_thread->x87_cw >> 10) & 3, 3u);    // x87 RC field = chop
    CHECK_EQ((guest_thread->mxcsr >> 13) & 3, 3u);     // mirrored into MXCSR
    // unmask invalid-operation
    call_ovr(ovr__controlfp_s, {cur, 0, 0x10});
    CHECK_EQ(rd32(cur) & 0x10, 0u);
    CHECK_EQ(guest_thread->x87_cw & 1, 0u);
    // invalid mask -> EINVAL, no change
    CHECK_EQ(guest_ret_eax(call_ovr(ovr__controlfp_s, {cur, 0, 0x80000000u})), 22u);
    // restore the default
    call_ovr(ovr__controlfp_s, {cur, 0x10000 | 0x8001F, 0x30000 | 0x8001F | 0x300});
    CHECK_EQ(guest_thread->x87_cw, 0x027Fu);
    // per-thread state
    uint32_t cw_other = 0;
    std::thread th([&] {
        RtThread *me = thread_attach("fp", 0x20000, true);
        cw_other = guest_thread->x87_cw;
        thread_detach(me);
    });
    th.join();
    CHECK_EQ(cw_other, 0x027Fu);
}

TEST(registered_as_msvcr90_exports) {
    t::setup_runtime("ovr");
    for (const char *dll : {"msvcr90.dll", "msvcr80.dll"})
        for (const char *n : {"memcpy", "memset", "memmove", "_wcsicmp", "_stricmp", "floor", "ceil", "_controlfp_s", "_CIpow", "wcslen", "strlen"})
            CHECK(hle_find(dll, n) != nullptr);
    uint32_t dst = t::galloc(8), src = t::gstr("abc");
    auto r = t::hle("msvcr90.dll", "memcpy", {dst, src, 4});
    CHECK_EQ(r.eax(), dst);
    CHECK_EQ(r.pop(), 0u);
}
