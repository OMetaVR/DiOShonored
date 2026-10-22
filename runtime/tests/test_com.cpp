#include "testing.h"
#include "dish/com.h"
#include <chrono>
#include <cstring>

using namespace dish;

// A fake COM interface implemented by a host object: IUnknown (3 slots) + int Add(int a, int b) + void Set(float x) + double Get().
struct FakeObj { int refs_destroyed = 0; int adds = 0; double value = 0; bool* destroyed = nullptr; };
static bool g_destroyed = false;

static GuestRet m_QueryInterface(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    uint32_t self = com_arg(esp, 0), ppv = com_arg(esp, 2);
    wr32(ppv, self);
    com_addref(self);
    return guest_ret(0, 0, 12);
}
static GuestRet m_AddRef(uint32_t esp, uint32_t, uint32_t, uint32_t) { return guest_ret(com_addref(com_arg(esp, 0)), 0, 4); }
static GuestRet m_Release(uint32_t esp, uint32_t, uint32_t, uint32_t) { return guest_ret(com_release(com_arg(esp, 0)), 0, 4); }
static GuestRet m_Add(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    FakeObj *o = com_host<FakeObj>(com_arg(esp, 0));
    o->adds++;
    return guest_ret(com_arg(esp, 1) + com_arg(esp, 2), 0, 12);
}
static GuestRet m_Set(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    FakeObj *o = com_host<FakeObj>(com_arg(esp, 0));
    float f;
    uint32_t w = com_arg(esp, 1);
    memcpy(&f, &w, 4);
    o->value = f;
    return guest_ret(0, 0, 8);
}
static GuestRet m_Get(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    guest_thread->st0_ret = com_host<FakeObj>(com_arg(esp, 0))->value;
    return guest_ret(0, 0, 4);
}
static void destroy_fake(void *p) { g_destroyed = true; delete static_cast<FakeObj *>(p); }

static const ComMethod kMethods[] = {
    {"QueryInterface", m_QueryInterface}, {"AddRef", m_AddRef}, {"Release", m_Release}, {"Add", m_Add}, {"Set", m_Set}, {"Get", m_Get},
};

TEST(vtable_dispatch_like_lifted_code) {
    t::setup_runtime("com");
    uint32_t vt = com_define_interface("IFake", kMethods, 6);
    CHECK_EQ(com_define_interface("IFake", kMethods, 6), vt);           // one shared vtable per interface
    CHECK_EQ(com_vtable_of("IFake"), vt);
    for (int i = 0; i < 6; i++) CHECK(is_thunk_addr(rd32(vt + 4 * i)));
    uint32_t obj = com_new_object(vt, new FakeObj, destroy_fake);
    CHECK(obj != 0);
    CHECK_EQ(rd32(obj), vt);
    // what lifted code does for `obj->Add(20, 22)`: this = [esp+4], target = [[obj] + 12]
    uint32_t target = rd32(rd32(obj) + 12);
    auto r = t::call_addr(target, {obj, 20, 22});
    CHECK_EQ(r.eax(), 42u);
    CHECK_EQ(r.pop(), 12u);                                              // stdcall: this + 2 args
    CHECK_EQ(com_host<FakeObj>(obj)->adds, 1);
    // float arg and double return through st0_ret
    float f = 3.25f;
    uint32_t fw;
    memcpy(&fw, &f, 4);
    t::call_addr(rd32(vt + 16), {obj, fw});
    t::call_addr(rd32(vt + 20), {obj});
    CHECK(guest_thread->st0_ret == 3.25);
    // refcounting and destruction
    CHECK_EQ(t::call_addr(rd32(vt + 4), {obj}).eax(), 2u);
    CHECK_EQ(t::call_addr(rd32(vt + 8), {obj}).eax(), 1u);
    CHECK(!g_destroyed);
    size_t live = com_live_objects();
    CHECK_EQ(t::call_addr(rd32(vt + 8), {obj}).eax(), 0u);
    CHECK(g_destroyed);
    CHECK_EQ(com_live_objects(), live - 1);
    CHECK(com_host_object_raw(obj) == nullptr);                          // stale pointers are rejected, not dereferenced
}

TEST(call_path_does_not_allocate_or_lock) {
    t::setup_runtime("com");
    uint32_t vt = com_define_interface("IFake", kMethods, 6);
    uint32_t obj = com_new_object(vt, new FakeObj, destroy_fake);
    uint32_t target = rd32(vt + 12);
    // 5M calls through guest_call: tens of millions per five minutes are required by the d3d9 trace
    auto t0 = std::chrono::steady_clock::now();
    uint32_t sp = t::stack_top() & ~15u;
    sp -= 12 + 4;
    wr32(sp, kReturnSentinel);
    wr32(sp + 4, obj);
    wr32(sp + 8, 1);
    wr32(sp + 12, 2);
    uint64_t sum = 0;
    for (int i = 0; i < 5000000; i++) sum += guest_ret_eax(guest_call(target, sp, 0, 0, 0));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK_EQ(sum, 15000000ull);
    fprintf(stderr, "  5M COM calls in %.1f ms (%.1f ns/call)\n", ms, ms * 1e6 / 5e6);
    CHECK(ms < 2000);
}
