#include "testing.h"
#include "dish/sync.h"
#include <atomic>
#include <cstring>
#include <thread>

using namespace dish;

static std::atomic<int> g_runs{0};
static std::atomic<uint32_t> g_tid_seen[16];

static GuestRet g_thread_proc(uint32_t esp, uint32_t, uint32_t, uint32_t) {     // DWORD WINAPI proc(void *param)
    uint32_t param = rd32(esp + 4);
    int slot = g_runs++;
    if (slot < 16) g_tid_seen[slot] = cur_tid();
    // use TLS in the guest thread
    uint32_t idx = rd32(param);               // param points at {tls_index, result}
    tls_set(idx, 0x1000 + cur_tid());
    wr32(param + 4, tls_get(idx));
    return guest_ret(param == 0 ? 0 : 77, 0, 4);
}
static GuestRet g_exit_proc(uint32_t esp, uint32_t, uint32_t, uint32_t) {       // calls ExitThread(5) midway
    uint32_t sp = esp - 8;
    wr32(sp, kReturnSentinel);
    wr32(sp + 4, 5);
    guest_call(thunk_addr(hle_bind("kernel32.dll", "ExitThread", 0, -1)), sp, 0, 0, 0);
    return guest_ret(99, 0, 4);              // must never be reached
}
static std::atomic<uint32_t> g_fls_cb_arg{0};
static GuestRet g_fls_cb(uint32_t esp, uint32_t, uint32_t, uint32_t) { g_fls_cb_arg = rd32(esp + 4); return guest_ret(0, 0, 4); }
static GuestRet g_fls_proc(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    uint32_t slot = rd32(esp + 4);
    fls_set(slot, 0xF15);
    return guest_ret(0, 0, 4);
}

TEST(create_thread_runs_guest_code_and_exit_code) {
    t::setup_runtime("threads");
    t::register_guest_fn(0x00402000, g_thread_proc);
    uint32_t idx = tls_alloc();
    uint32_t param = t::galloc(8);
    wr32(param, idx);
    uint32_t tidp = t::galloc(4);
    auto h = t::hle("kernel32.dll", "CreateThread", {0, 0, 0x00402000, param, 0, tidp});
    CHECK(h.eax() != 0);
    CHECK_EQ(h.pop(), 24u);
    auto w = t::hle("kernel32.dll", "WaitForSingleObject", {h.eax(), 5000});
    CHECK_EQ(w.eax(), 0u);
    uint32_t codep = t::galloc(4);
    t::hle("kernel32.dll", "GetExitCodeThread", {h.eax(), codep});
    CHECK_EQ(rd32(codep), 77u);
    CHECK_EQ(rd32(param + 4), 0x1000 + rd32(tidp));       // thread-private TLS slot
    CHECK(tls_get(idx) == 0);                              // not visible in this thread
    CHECK(rd32(tidp) != cur_tid());
    t::hle("kernel32.dll", "CloseHandle", {h.eax()});
    tls_free(idx);
}

TEST(host_stack_and_guest_stack_sizes) {
    t::setup_runtime("threads");
    RtThread *me = thread_current();
    CHECK(me->gt.stack_base - me->gt.stack_limit >= 0x100000);
    uint32_t teb = me->teb;
    CHECK_EQ(rd32(teb + 0x18), teb);                       // fs:[0x18] self pointer
    CHECK_EQ(rd32(teb + 0x04), me->gt.stack_base);
    CHECK_EQ(rd32(teb + 0x08), me->gt.stack_limit);
    CHECK_EQ(rd32(teb + 0x00), 0xFFFFFFFFu);               // empty SEH chain
    CHECK_EQ(rd32(teb + 0x24), me->tid);
    CHECK(rd32(teb + 0x30) == process_peb());
    CHECK_EQ(me->gt.x87_cw, 0x027Fu);
    CHECK_EQ(me->gt.mxcsr, 0x1F80u);
    CHECK_EQ(kHostStackSize, (size_t)(16u << 20));
    // a deep host recursion on a created thread must survive: guest threads get >= 16 MiB of host stack
    static std::atomic<size_t> depth;
    depth = 0;
    struct R { static size_t rec(size_t n) { volatile char pad[2048]; pad[0] = (char)n; if (n == 0) return pad[0]; return rec(n - 1) + pad[0]; } };
    t::register_guest_fn(0x00402100, [](uint32_t, uint32_t, uint32_t, uint32_t) -> GuestRet {
        depth = R::rec(5000) >= 0 ? 5000 : 0;   // ~10 MB of host stack
        return guest_ret(0, 0, 4);
    });
    uint32_t th = thread_create(0x00402100, 0, 0, false, nullptr);
    CHECK(th != 0);
    t::hle("kernel32.dll", "WaitForSingleObject", {th, 10000});
    CHECK_EQ(depth.load(), 5000u);
}

TEST(exit_thread_unwinds) {
    t::setup_runtime("threads");
    t::register_guest_fn(0x00402200, g_exit_proc);
    uint32_t h = thread_create(0x00402200, 0, 0, false, nullptr);
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {h, 5000}).eax(), 0u);
    uint32_t codep = t::galloc(4);
    t::hle("kernel32.dll", "GetExitCodeThread", {h, codep});
    CHECK_EQ(rd32(codep), 5u);
}

TEST(suspended_start_and_resume) {
    t::setup_runtime("threads");
    t::register_guest_fn(0x00402000, g_thread_proc);
    uint32_t param = t::galloc(8);
    wr32(param, tls_alloc());
    int before = g_runs.load();
    uint32_t h = thread_create(0x00402000, param, 0, true, nullptr);
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {h, 100}).eax(), (uint32_t)WAIT_TIMEOUT);
    CHECK_EQ(g_runs.load(), before);
    CHECK_EQ(t::hle("kernel32.dll", "ResumeThread", {h}).eax(), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {h, 5000}).eax(), 0u);
    CHECK_EQ(g_runs.load(), before + 1);
}

TEST(tls_alloc_free_and_expansion) {
    t::setup_runtime("threads");
    std::vector<uint32_t> idx;
    for (int i = 0; i < 100; i++) idx.push_back(tls_alloc());          // crosses the 64-slot TEB array into expansion slots
    for (uint32_t i : idx) CHECK(i != 0xFFFFFFFFu);
    for (size_t k = 0; k < idx.size(); k++) CHECK(tls_set(idx[k], 0x100 + (uint32_t)k));
    for (size_t k = 0; k < idx.size(); k++) CHECK_EQ(tls_get(idx[k]), 0x100 + (uint32_t)k);
    // freed slots read as zero in every thread and can be reallocated
    uint32_t victim = idx[70];
    CHECK(tls_free(victim));
    CHECK_EQ(tls_get(victim), 0u);
    CHECK(!tls_free(victim));                                           // double free rejected
    CHECK_EQ(tls_alloc(), victim);
    // through the HLE thunks (GetLastError is reset by TlsGetValue)
    set_last_error(1234);
    auto r = t::hle("kernel32.dll", "TlsGetValue", {idx[3]});
    CHECK_EQ(r.eax(), 0x103u);
    CHECK_EQ(get_last_error(), 0u);
    CHECK_EQ(t::hle("kernel32.dll", "TlsSetValue", {idx[3], 5}).eax(), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "TlsGetValue", {idx[3]}).eax(), 5u);
    CHECK_EQ(t::hle("kernel32.dll", "TlsSetValue", {5000, 5}).eax(), 0u);   // invalid index
    for (uint32_t i : idx) tls_free(i);
}

TEST(tls_is_per_thread) {
    t::setup_runtime("threads");
    uint32_t i = tls_alloc();
    tls_set(i, 111);
    std::atomic<uint32_t> other{0xFFFF};
    std::thread th([&] {
        RtThread *t2 = thread_attach("tls2", 0x20000, true);
        other = tls_get(i);
        tls_set(i, 222);
        thread_detach(t2);
    });
    th.join();
    CHECK_EQ(other.load(), 0u);
    CHECK_EQ(tls_get(i), 111u);
    tls_free(i);
}

TEST(fls_callbacks_run_at_thread_exit) {
    t::setup_runtime("threads");
    t::register_guest_fn(0x00402300, g_fls_cb);
    t::register_guest_fn(0x00402310, g_fls_proc);
    auto a = t::hle("kernel32.dll", "FlsAlloc", {0x00402300});
    CHECK(a.eax() != 0xFFFFFFFFu);
    uint32_t h = thread_create(0x00402310, a.eax(), 0, false, nullptr);
    t::hle("kernel32.dll", "WaitForSingleObject", {h, 5000});
    CHECK_EQ(g_fls_cb_arg.load(), 0xF15u);
    CHECK_EQ(t::hle("kernel32.dll", "FlsSetValue", {a.eax(), 9}).eax(), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "FlsGetValue", {a.eax()}).eax(), 9u);
    CHECK_EQ(t::hle("kernel32.dll", "FlsFree", {a.eax()}).eax(), 1u);
}

static std::atomic<int> g_pe_tls_hits{0};
TEST(pe_static_tls_blocks) {
    t::setup_runtime("threads");
    // a module image with a TLS template: [start,end) = 8 bytes {0x11,0x22,...}, 8 bytes zero fill
    uint32_t img = vm_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    for (int i = 0; i < 8; i++) wr8(img + i, (uint8_t)(0x11 * (i + 1)));
    uint32_t idx_addr = img + 0x100;
    PeTls pt{img, img + 8, 8, idx_addr, 0};
    pe_tls_register(pt);
    uint32_t tls_index = rd32(idx_addr);
    CHECK_EQ(tls_index, 0u);
    pe_tls_init_thread(thread_current());
    uint32_t arr = rd32(cur_teb() + 0x2C);                  // fs:[0x2C]
    uint32_t blk = rd32(arr + 4 * tls_index);
    CHECK(blk != 0);
    CHECK_EQ(rd8(blk + 3), 0x44);
    CHECK_EQ(rd8(blk + 9), 0);                              // zero fill
    wr8(blk, 0x99);
    // another thread gets its own pristine copy
    std::atomic<uint32_t> v{0};
    std::thread th([&] {
        RtThread *t2 = thread_attach("tls3", 0x20000, true);
        uint32_t a2 = rd32(cur_teb() + 0x2C);
        uint32_t b2 = rd32(a2 + 4 * tls_index);
        v = rd8(b2) | (b2 == blk ? 0x1000u : 0u);
        thread_detach(t2);
    });
    th.join();
    CHECK_EQ(v.load(), 0x11u);
    (void)g_pe_tls_hits;
}

TEST(thread_naming_exception) {
    t::setup_runtime("threads");
    // MSVC SetThreadName: RaiseException(0x406D1388, 0, 4, (ULONG_PTR *)&info) must be consumed without side effects
    uint32_t info = t::galloc(16);
    wr32(info, 0x1000);
    wr32(info + 4, t::gstr("RenderThread"));
    wr32(info + 8, 0xFFFFFFFFu);
    wr32(info + 12, 0);
    // RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR *)&info)
    auto r = t::hle("kernel32.dll", "RaiseException", {0x406D1388, 0, 4, info});
    CHECK_EQ(r.pop(), 16u);
    CHECK(thread_current()->name == "RenderThread");
}
