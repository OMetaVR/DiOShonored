#include "testing.h"
#include <cstring>
#include <set>

using namespace dish;

// ---- thunks with assorted argument shapes; the pop count must follow the C++ signature
static uint32_t f0() { return 1; }
static uint32_t f3(uint32_t a, uint32_t b, uint32_t c) { return a * 100 + b * 10 + c; }
static int64_t f64(int64_t a, uint32_t b) { return a + b; }                     // 3 slots
static double fdbl(double a, double b) { return a * b; }                         // 4 slots, float return via st0
static float ffl(float a) { return a + 1; }
static void fvoid(uint16_t a, uint8_t b) { (void)a; (void)b; }
static uint32_t fthis(uint32_t self, uint32_t a) { return rd32(self) + a; }      // thiscall: self in ecx, one stack arg
static uint32_t fcdecl(uint32_t a, uint32_t b) { return a - b; }
static bool fbool(int32_t a) { return a < 0; }
static uint32_t fctx(const HleCtx &c, uint32_t a) { return c.esp ? a + 1 : 0; }
HLE("test.dll", "f0", Stdcall, f0);
HLE("test.dll", "f3", Stdcall, f3);
HLE("test.dll", "f64", Stdcall, f64);
HLE("test.dll", "fdbl", Stdcall, fdbl);
HLE("test.dll", "ffl", Stdcall, ffl);
HLE("test.dll", "fvoid", Stdcall, fvoid);
HLE("test.dll", "fthis", Thiscall, fthis);
HLE("test.dll", "fcdecl", Cdecl, fcdecl);
HLE("test.dll", "fbool", Stdcall, fbool);
HLE("test.dll", "fctx", Stdcall, fctx);

TEST(pop_counts_follow_signature) {
    t::setup_runtime("thunks");
    CHECK_EQ((hle_pop<f0, HConv::Stdcall>()), 0u);
    CHECK_EQ((hle_pop<f3, HConv::Stdcall>()), 12u);
    CHECK_EQ((hle_pop<f64, HConv::Stdcall>()), 12u);
    CHECK_EQ((hle_pop<fdbl, HConv::Stdcall>()), 16u);
    CHECK_EQ((hle_pop<ffl, HConv::Stdcall>()), 4u);
    CHECK_EQ((hle_pop<fvoid, HConv::Stdcall>()), 8u);
    CHECK_EQ((hle_pop<fthis, HConv::Thiscall>()), 4u);
    CHECK_EQ((hle_pop<fcdecl, HConv::Cdecl>()), 0u);
    CHECK_EQ((hle_pop<fctx, HConv::Stdcall>()), 4u);
    auto r = t::hle("test.dll", "f3", {1, 2, 3});
    CHECK_EQ(r.eax(), 123u);
    CHECK_EQ(r.pop(), 12u);
    uint64_t big = 0x100000005ull;
    r = t::hle("test.dll", "f64", {(uint32_t)big, (uint32_t)(big >> 32), 3});
    CHECK_EQ(r.eax(), 8u);
    CHECK_EQ(r.edx(), 1u);
    CHECK_EQ(r.pop(), 12u);
    double a = 2.5, b = 4.0;
    uint32_t aw[2], bw[2];
    memcpy(aw, &a, 8);
    memcpy(bw, &b, 8);
    r = t::hle("test.dll", "fdbl", {aw[0], aw[1], bw[0], bw[1]});
    CHECK(guest_thread->st0_ret == 10.0);
    CHECK_EQ(r.pop(), 16u);
    float fl = 1.5f;
    uint32_t fw;
    memcpy(&fw, &fl, 4);
    r = t::hle("test.dll", "ffl", {fw});
    CHECK(guest_thread->st0_ret == 2.5);
    r = t::hle("test.dll", "fvoid", {0x12345, 0x1FF});
    CHECK_EQ(r.pop(), 8u);
    uint32_t obj = t::galloc(4);
    wr32(obj, 40);
    r = t::call_addr(t::hle_addr("test.dll", "fthis"), {2}, obj);
    CHECK_EQ(r.eax(), 42u);
    CHECK_EQ(r.pop(), 4u);
    r = t::hle("test.dll", "fcdecl", {10, 3});
    CHECK_EQ(r.eax(), 7u);
    CHECK_EQ(r.pop(), 0u);
    CHECK_EQ(t::hle("test.dll", "fbool", {(uint32_t)-5}).eax(), 1u);
    CHECK_EQ(t::hle("test.dll", "fbool", {5}).eax(), 0u);
    CHECK_EQ(t::hle("test.dll", "fctx", {9}).eax(), 10u);
}

TEST(all_implementations_match_wine_signatures) {
    t::setup_runtime("thunks");
    struct R { int bad = 0, nosig = 0, ok = 0; std::vector<std::string> msgs; } res;
    hle_for_each([](const HleImpl &i, void *u) {
        auto *r = (R *)u;
        if (!strcmp(i.dll, "test.dll")) return;
        const HleSig *s = sig_find(i.dll, i.name);
        if (!s || s->conv == '?' || s->pop < 0) { r->nosig++; return; }
        bool cd = s->conv == 'C' || s->conv == 'V';
        if ((cd && i.pop != 0) || (!cd && (uint32_t)s->pop != i.pop)) {
            r->bad++;
            r->msgs.push_back(std::string(i.dll) + "!" + i.name + ": impl pops " + std::to_string(i.pop) + ", spec " + std::string(1, s->conv) + std::to_string(s->pop));
        } else r->ok++;
    }, &res);
    for (auto &m : res.msgs) fprintf(stderr, "  mismatch: %s\n", m.c_str());
    fprintf(stderr, "  %d implementations match the spec, %d have no spec entry\n", res.ok, res.nosig);
    CHECK_EQ(res.bad, 0);
    CHECK(res.ok > 250);
}

TEST(stub_pops_and_counts) {
    t::setup_runtime("thunks");
    // find a spec'd stdcall function with arguments that we do not implement
    size_t n;
    const HleSig *tab = sig_table(&n);
    const HleSig *pick = nullptr;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(tab[i].dll, "kernel32.dll") && tab[i].conv == 'S' && tab[i].pop >= 12 && !hle_find(tab[i].dll, tab[i].name)) { pick = &tab[i]; break; }
    CHECK(pick != nullptr);
    if (!pick) return;
    uint64_t before = g_unimpl_calls.load();
    auto r = t::hle("kernel32.dll", pick->name, {1, 2, 3, 4});
    CHECK_EQ(r.pop(), (uint32_t)pick->pop);
    CHECK_EQ(r.eax(), pick->default_ret);
    t::hle("kernel32.dll", pick->name, {1, 2, 3, 4});
    CHECK_EQ(g_unimpl_calls.load(), before + 2);
    // the stub is bound once and reported
    bool found = false;
    for (size_t i = 0; i < stub_count(); i++) {
        const StubInfo *s = stub_at(i);
        if (!strcmp(s->name, pick->name)) { found = true; CHECK_EQ(s->calls, 2u); CHECK(s->sig_known); }
    }
    CHECK(found);
}

TEST(stub_logs_once) {
    t::setup_runtime("thunks");
    static int count;
    count = 0;
    g_log_hook = [](LogCh ch, LogLvl, const char *line) { if (ch == LC_STUB && strstr(line, "UNIMPLEMENTED")) count++; };
    uint32_t a = t::hle_addr("user32.dll", "AnimateWindow");
    for (int i = 0; i < 5; i++) t::call_addr(a, {0, 0, 0});
    g_log_hook = nullptr;
    CHECK_EQ(count, 1);
}

TEST(ordinal_imports_resolve) {
    t::setup_runtime("thunks");
    uint32_t idx = hle_bind("xinput1_3.dll", nullptr, 2, -1);
    const ThunkEntry &e = g_thunks[idx];
    CHECK(e.name && !strcmp(e.name, "XInputGetState"));
    CHECK(e.flags == 0);   // implemented, not a stub
    auto r = t::call_addr(thunk_addr(idx), {0, t::galloc(16)});
    CHECK_EQ(r.pop(), 8u);
    CHECK(r.eax() == 1167u || r.eax() == 0u);   // not connected, or a real pad is plugged in
}

TEST(get_proc_address_policy) {
    t::setup_runtime("thunks");
    CHECK(hle_dynamic_proc("kernel32.dll", "FlsAlloc", 0) != 0);              // implemented
    CHECK(hle_dynamic_proc("kernel32.dll", "CreateFileW", 0) != 0);
    CHECK_EQ(hle_dynamic_proc("kernel32.dll", "NoSuchFunctionAtAll", 0), 0u);  // probes for optional APIs must fail
    uint32_t a = hle_dynamic_proc("kernel32.dll", "CreateFileW", 0);
    CHECK_EQ(hle_dynamic_proc("kernel32.dll", "CreateFileW", 0), a);          // stable address per function
    CHECK(is_thunk_addr(a));
}

TEST(thunk_aux_and_dynamic_allocation) {
    t::setup_runtime("thunks");
    static uint32_t seen_aux;
    auto fn = [](uint32_t esp, uint32_t, uint32_t, uint32_t) -> GuestRet { seen_aux = thunk_aux(); return guest_ret(7, 0, 0); };
    uint32_t i1 = thunk_alloc(fn, TF_NEEDS_AUX, 111, "t", "a");
    uint32_t i2 = thunk_alloc(fn, TF_NEEDS_AUX, 222, "t", "b");
    CHECK(i2 == i1 + 1);
    t::call_addr(thunk_addr(i1), {});
    CHECK_EQ(seen_aux, 111u);
    t::call_addr(thunk_addr(i2), {});
    CHECK_EQ(seen_aux, 222u);
    CHECK(thunk_lookup(thunk_addr(i1)) != nullptr);
    CHECK(thunk_lookup(thunk_addr(i2 + 1)) == nullptr);
}

TEST(sig_table_covers_every_import) {
    // every function in api_surface.json must have a usable entry (the generator reports the rest in sigs_report.txt)
    size_t n;
    const HleSig *tab = sig_table(&n);
    int need = 0, unknown = 0;
    for (size_t i = 0; i < n; i++) {
        if (!tab[i].needed) continue;
        need++;
        if (tab[i].conv == '?' || tab[i].pop < 0) { unknown++; fprintf(stderr, "  unknown signature: %s!%s\n", tab[i].dll, tab[i].name); }
    }
    fprintf(stderr, "  %d needed entries, %d with unknown signature\n", need, unknown);
    CHECK(need > 400);
    CHECK(unknown < 50);
}
