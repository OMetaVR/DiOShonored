#include "testing.h"
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>

using namespace dish;

// ---- fake lifted functions, written exactly as the lifter would emit them
static GuestRet g_add2(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {      // int __stdcall add2(int a, int b)
    uint32_t a = rd32(esp + 4), b = rd32(esp + 8);
    return guest_ret(a + b, 0, 8);
}
static GuestRet g_thiscall(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {  // int __thiscall m(int x): this in ecx
    return guest_ret(rd32(ecx) + rd32(esp + 4), 0x1234, 4);
}
static GuestRet g_cdecl3(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {    // cdecl: caller pops
    return guest_ret(rd32(esp + 4) * rd32(esp + 8) + rd32(esp + 12), 0, 0);
}
static GuestRet g_fret(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {      // double f(double) -> st0_ret
    double d;
    memcpy(&d, gp(esp + 4), 8);
    guest_thread->st0_ret = d * 2.0;
    return guest_ret(0, 0, 0);
}
static GuestRet g_fastcall(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {
    return guest_ret(ecx * 100 + edx * 10 + rd32(esp + 4), 0, 4);
}
// calls a thunk that calls back into guest code: qsort-like
static uint32_t g_cmp_calls = 0;
static GuestRet g_cmp(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {       // int cmp(const int *a, const int *b)
    g_cmp_calls++;
    int a = (int)rd32(rd32(esp + 4)), b = (int)rd32(rd32(esp + 8));
    return guest_ret((uint32_t)(a < b ? -1 : a > b ? 1 : 0), 0, 0);
}
static int host_sorter(const HleCtx &c, uint32_t base, uint32_t n, uint32_t cmp) {   // bubble sort through guest_invoke
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j + 1 < n - i; j++) {
            uint32_t r = guest_invoke_from(c.esp, cmp, Conv::Cdecl, base + 4 * j, base + 4 * j + 4);
            if ((int32_t)r > 0) { uint32_t x = rd32(base + 4 * j); wr32(base + 4 * j, rd32(base + 4 * j + 4)); wr32(base + 4 * j + 4, x); }
        }
    return 0;
}
HLE("test.dll", "sorter", Cdecl, host_sorter);
static GuestRet g_sort_caller(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {  // lifted code calling the thunk via guest_call
    uint32_t base = rd32(esp + 4), n = rd32(esp + 8);
    uint32_t sp = esp - 16;                       // callee frame: [sp]=ret, args above
    wr32(sp + 4, base); wr32(sp + 8, n); wr32(sp + 12, 0x00500000);
    wr32(sp, kReturnSentinel);
    GuestRet r = guest_call(thunk_addr(hle_bind("test.dll", "sorter", 0, -1)), sp, 0, 0, 0);
    return guest_ret(guest_ret_eax(r), 0, 0);
}

static GuestRet g_nested(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // depth recursion through guest_call
    uint32_t n = rd32(esp + 4);
    if (n == 0) return guest_ret(0, 0, 4);
    uint32_t sp = esp - 8;
    wr32(sp, kReturnSentinel);
    wr32(sp + 4, n - 1);
    GuestRet r = guest_call(0x00501000, sp, 0, 0, 0);
    return guest_ret(guest_ret_eax(r) + 1, 0, 4);
}

TEST(call_dispatch) {
    t::setup_runtime("call");
    t::register_guest_fn(0x00401000, g_add2);
    t::register_guest_fn(0x00401010, g_thiscall);
    t::register_guest_fn(0x00401020, g_cdecl3);
    t::register_guest_fn(0x00401030, g_fret);
    t::register_guest_fn(0x00401040, g_fastcall);
    auto r = t::call_addr(0x00401000, {30, 12});
    CHECK_EQ(r.eax(), 42u);
    CHECK_EQ(r.pop(), 8u);
    uint32_t obj = t::galloc(8);
    wr32(obj, 1000);
    r = t::call_addr(0x00401010, {7}, obj);
    CHECK_EQ(r.eax(), 1007u);
    CHECK_EQ(r.edx(), 0x1234u);
    CHECK_EQ(r.pop(), 4u);
    r = t::call_addr(0x00401020, {6, 7, 8});
    CHECK_EQ(r.eax(), 50u);
    CHECK_EQ(r.pop(), 0u);
}

TEST(guest_invoke_conventions) {
    t::setup_runtime("call");
    t::register_guest_fn(0x00401000, g_add2);
    t::register_guest_fn(0x00401010, g_thiscall);
    t::register_guest_fn(0x00401020, g_cdecl3);
    t::register_guest_fn(0x00401040, g_fastcall);
    CHECK_EQ(guest_invoke(0x00401000, Conv::Stdcall, 1, 2), 3u);
    CHECK_EQ(guest_invoke(0x00401020, Conv::Cdecl, 3, 4, 5), 17u);
    uint32_t obj = t::galloc(8);
    wr32(obj, 5);
    GuestRet full;
    CHECK_EQ(guest_invoke_args(0x00401010, Conv::Thiscall, std::vector<uint32_t>{obj, 9}.data(), 2, &full), 14u);
    CHECK_EQ(guest_ret_edx(full), 0x1234u);
    CHECK_EQ(guest_invoke(0x00401040, Conv::Fastcall, 1, 2, 3), 123u);
    // stack pointer is restored after nested use
    uint32_t before = t::stack_top();
    guest_invoke(0x00401000, Conv::Stdcall, 1, 2);
    CHECK_EQ(t::stack_top(), before);
}

TEST(float_return_through_st0) {
    t::setup_runtime("call");
    t::register_guest_fn(0x00401030, g_fret);
    uint32_t sp = t::stack_top() & ~15u;
    sp -= 8;
    double d = 1.25;
    memcpy(gp(sp), &d, 8);
    sp -= 4;
    wr32(sp, kReturnSentinel);
    guest_call(0x00401030, sp, 0, 0, 0);
    CHECK(guest_thread->st0_ret == 2.5);
}

TEST(thunk_calls_back_into_guest) {
    t::setup_runtime("call");
    t::register_guest_fn(0x00500000, g_cmp);
    t::register_guest_fn(0x00500100, g_sort_caller);
    uint32_t arr = t::galloc(32);
    int v[] = {5, 3, 9, 1, 7, 2, 8, 6};
    for (int i = 0; i < 8; i++) wr32(arr + 4 * i, (uint32_t)v[i]);
    auto r = t::call_addr(0x00500100, {arr, 8});
    CHECK_EQ(r.pop(), 0u);
    for (int i = 0; i < 7; i++) CHECK((int32_t)rd32(arr + 4 * i) <= (int32_t)rd32(arr + 4 * i + 4));
    CHECK(g_cmp_calls > 8);
}

TEST(deep_nesting) {
    t::setup_runtime("call");
    t::register_guest_fn(0x00501000, g_nested);
    auto r = t::call_addr(0x00501000, {200});
    CHECK_EQ(r.eax(), 200u);
}

static int g_unres_hits = 0;
static uint32_t g_unres_target = 0;
static GuestRet unresolved_h(uint32_t target, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {
    g_unres_hits++;
    g_unres_target = target;
    return guest_ret(0xBAD, 0, 0);
}

TEST(unresolved_goes_to_handler_and_counts) {
    t::setup_runtime("call");
    set_unresolved_handler(unresolved_h);
    auto r = t::call_addr(0x00DEAD00, {1, 2});
    CHECK_EQ(r.eax(), 0xBADu);
    CHECK_EQ(g_unres_hits, 1);
    CHECK_EQ(g_unres_target, 0x00DEAD00u);
    CHECK_EQ(unresolved_count(), 1u);
    // unallocated thunk slot is also unresolved
    r = t::call_addr(guest_thunk_addr(60000), {});
    CHECK_EQ(g_unres_hits, 2);
    set_unresolved_handler(nullptr);
}

TEST(sentinel_return_is_not_unresolved) {
    t::setup_runtime("call");
    set_unresolved_handler(unresolved_h);
    int before = g_unres_hits;
    auto r = t::call_addr(kReturnSentinel, {});
    (void)r;
    CHECK_EQ(g_unres_hits, before);
    set_unresolved_handler(nullptr);
}

TEST(unresolved_default_writes_log_and_aborts) {
    // verify the default handler dumps a guest stack to unresolved.log (run in a child so the abort does not kill the test)
    t::setup_runtime("call");
    std::string log = t::temp_dir() + "/unresolved.log";
    set_unresolved_log_path(log.c_str());
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        t::call_addr(0x00BEEF00, {0x11111111, 0x22222222});
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st));
    FILE *f = fopen(log.c_str(), "r");
    CHECK(f != nullptr);
    char buf[4096] = {0};
    if (f) { fread(buf, 1, sizeof buf - 1, f); fclose(f); }
    CHECK(strstr(buf, "target=00beef00") != nullptr);
    CHECK(strstr(buf, "11111111") != nullptr);
}
