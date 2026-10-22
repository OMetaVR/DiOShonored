#include "dish/thread.h"
#include "dish/call.h"
#include "dish/fault.h"
#include "dish/log.h"
#include "dish/sync.h"
#include "dish/vm.h"
#include <algorithm>
#include <bitset>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

namespace dish {

void (*g_thread_notify)(uint32_t reason) = nullptr;

namespace {
std::mutex g_tmu;
std::vector<RtThread *> g_threads;
uint32_t g_next_tid = 0x1004;
uint32_t g_peb = 0;
constexpr uint32_t kProcessId = 0x1000;

std::mutex g_tls_mu;
std::bitset<kTlsMaxSlots> g_tls_used;

struct FlsSlot { bool used = false; uint32_t cb = 0; };
std::vector<FlsSlot> g_fls_slots;
std::mutex g_fls_mu;

std::mutex g_petls_mu;
std::vector<PeTls> g_petls;

struct StartGate {
    std::mutex mu;
    std::condition_variable cv;
    bool go = false;
};
struct ThreadExtra {   // lives for the thread's lifetime, owned via RtThread::runtime_extra map below
    StartGate gate;
};
std::mutex g_extra_mu;
std::vector<std::pair<RtThread *, std::shared_ptr<ThreadExtra>>> g_extras;

std::shared_ptr<ThreadExtra> extra_for(RtThread *t, bool create) {
    std::lock_guard<std::mutex> lk(g_extra_mu);
    for (auto &p : g_extras) if (p.first == t) return p.second;
    if (!create) return nullptr;
    auto e = std::make_shared<ThreadExtra>();
    g_extras.emplace_back(t, e);
    return e;
}
void extra_drop(RtThread *t) {
    std::lock_guard<std::mutex> lk(g_extra_mu);
    g_extras.erase(std::remove_if(g_extras.begin(), g_extras.end(), [&](auto &p) { return p.first == t; }), g_extras.end());
}

uint32_t rup(uint32_t x, uint32_t a) { return (x + a - 1) & ~(a - 1); }

bool prepare(RtThread *t, uint32_t stack_size) {
    if (stack_size == 0) stack_size = 1u << 20;
    if (stack_size < 0x10000) stack_size = 0x10000;
    stack_size = rup(stack_size, 0x10000);
    uint32_t err;
    t->stack_alloc = vm_alloc(0, stack_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &err);
    if (!t->stack_alloc) { LOGE(LC_THREAD, "cannot allocate %u byte guest stack (err %u)", stack_size, err); return false; }
    t->stack_alloc_size = stack_size;
    vm_protect(t->stack_alloc, kGuestPage, PAGE_NOACCESS, nullptr);   // overflow guard
    t->teb = vm_alloc(0, TEB_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &err);
    if (!t->teb) { vm_free(t->stack_alloc, 0, MEM_RELEASE); return false; }
    {
        std::lock_guard<std::mutex> lk(g_tmu);
        t->tid = g_next_tid;
        g_next_tid += 4;
    }
    uint8_t *teb = guest_base + t->teb;
    memset(teb, 0, TEB_SIZE);
    auto w32 = [&](uint32_t off, uint32_t v) { memcpy(teb + off, &v, 4); };
    w32(TEB_EXCEPTION_LIST, 0xFFFFFFFFu);
    w32(TEB_STACK_BASE, t->stack_alloc + stack_size);
    w32(TEB_STACK_LIMIT, t->stack_alloc + kGuestPage);
    w32(TEB_SELF, t->teb);
    w32(TEB_PID, kProcessId);
    w32(TEB_TID, t->tid);
    w32(TEB_PEB, g_peb);
    t->gt.teb = t->teb;
    t->gt.x87_cw = 0x027F;
    t->gt.mxcsr = 0x1F80;
    t->gt.st0_ret = 0;
    t->gt.stack_base = t->stack_alloc + stack_size;
    t->gt.stack_limit = t->stack_alloc + kGuestPage;
    t->gt.host_tid = t->tid;
    t->gt.flags = 0;
    t->gt.runtime = t;
    t->cur_sp = (t->gt.stack_base - 64) & ~15u;
    return true;
}

void bind(RtThread *t) {
    guest_thread = &t->gt;
    t->host_self = pthread_self();
    fault_thread_init();
    {
        std::lock_guard<std::mutex> lk(g_tmu);
        g_threads.push_back(t);
    }
    pe_tls_init_thread(t);
}

void run_fls_callbacks(RtThread *t) {
    std::vector<std::pair<uint32_t, uint32_t>> todo;
    {
        std::lock_guard<std::mutex> lk(g_fls_mu);
        for (size_t i = 0; i < t->fls.size() && i < g_fls_slots.size(); i++)
            if (g_fls_slots[i].used && g_fls_slots[i].cb && t->fls[i]) todo.emplace_back(g_fls_slots[i].cb, t->fls[i]);
    }
    for (auto &p : todo) {
        t->cur_sp = (t->gt.stack_base - 64) & ~15u;
        guest_invoke(p.first, Conv::Stdcall, p.second);
    }
}

void release_resources(RtThread *t) {
    {
        std::lock_guard<std::mutex> lk(g_tmu);
        g_threads.erase(std::remove(g_threads.begin(), g_threads.end(), t), g_threads.end());
    }
    uint32_t ext = *(uint32_t *)(guest_base + t->teb + TEB_TLS_EXPANSION);
    if (ext) vm_free(ext, 0, MEM_RELEASE);
    uint32_t pet = *(uint32_t *)(guest_base + t->teb + 0xF98);   // our private: PE TLS block allocation
    if (pet) vm_free(pet, 0, MEM_RELEASE);
    // Exited threads' stacks are decommitted but stay reserved for a while instead of being released: PhysX recreates its
    // workers right after they exit, and handing the same stack memory to a new thread at once let it overwrite data that was
    // still referenced from the old stack (free-list corruption at gameplay start). A stale access now faults instead.
    vm_free(t->stack_alloc, 0, MEM_DECOMMIT);
    {
        static std::mutex qmu;
        static std::vector<uint32_t> quarantine;
        std::lock_guard<std::mutex> lk(qmu);
        quarantine.push_back(t->stack_alloc);
        if (quarantine.size() > 32) {
            vm_free(quarantine.front(), 0, MEM_RELEASE);
            quarantine.erase(quarantine.begin());
        }
    }
    vm_free(t->teb, 0, MEM_RELEASE);
}

struct StartArg { RtThread *t; };

void *thread_body(void *arg) {
    RtThread *t = ((StartArg *)arg)->t;
    delete (StartArg *)arg;
    bind(t);
    {
        char nm[16];
        snprintf(nm, sizeof nm, "g%u", t->tid);
#ifdef __linux__
        pthread_setname_np(pthread_self(), nm);
#else
        pthread_setname_np(nm);
#endif
    }
    if (t->suspended_start) {
        auto ex = extra_for(t, false);
        if (ex) {
            std::unique_lock<std::mutex> lk(ex->gate.mu);
            ex->gate.cv.wait(lk, [&] { return ex->gate.go; });
        }
    }
    if (setjmp(t->exit_jmp) == 0) {
        t->exit_jmp_valid = true;
        if (g_thread_notify) g_thread_notify(2);
        t->cur_sp = (t->gt.stack_base - 64) & ~15u;
        uint32_t code = guest_invoke(t->start_addr, Conv::Stdcall, t->start_param);
        t->exit_code = code;
    }
    t->exit_jmp_valid = false;
    LOGD(LC_THREAD, "thread %u exiting with code %u", t->tid, t->exit_code);
    t->cur_sp = (t->gt.stack_base - 64) & ~15u;
    if (g_thread_notify) g_thread_notify(3);
    run_fls_callbacks(t);
    KThread *kt = t->kthread;
    release_resources(t);
    extra_drop(t);
    guest_thread = nullptr;
    if (kt) {
        kt->rt = nullptr;
        thread_mark_done(kt, t->exit_code);
        kt->release();
    }
    delete t;
    return nullptr;
}

}  // namespace

KThread *thread_kthread(RtThread *t) {
    std::lock_guard<std::mutex> lk(g_tmu);
    if (!t->kthread) {
        auto *kt = new KThread();
        kt->tid = t->tid;
        kt->rt = t;
        t->kthread = kt;
    }
    return t->kthread;
}

RtThread *thread_current() { return guest_thread ? static_cast<RtThread *>(guest_thread->runtime) : nullptr; }

bool process_init_peb(uint32_t image_base, uint32_t n_cpus) {
    uint32_t err;
    g_peb = vm_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &err);
    if (!g_peb) return false;
    uint8_t *p = guest_base + g_peb;
    memset(p, 0, 0x1000);
    auto w32 = [&](uint32_t off, uint32_t v) { memcpy(p + off, &v, 4); };
    w32(0x08, image_base);
    w32(0x64, n_cpus);
    w32(0xA4, 6);        // OSMajorVersion
    w32(0xA8, 1);        // OSMinorVersion
    uint16_t build = 7601, csd = 0x0100;
    memcpy(p + 0xAC, &build, 2);
    memcpy(p + 0xAE, &csd, 2);
    w32(0xB0, 2);        // VER_PLATFORM_WIN32_NT
    return true;
}
uint32_t process_peb() { return g_peb; }
uint32_t process_id() { return kProcessId; }

RtThread *thread_attach(const char *name, uint32_t guest_stack_size, bool foreign) {
    auto *t = new RtThread();
    t->name = name ? name : "";
    t->foreign = foreign;
    if (!prepare(t, guest_stack_size)) { delete t; return nullptr; }
    bind(t);
    return t;
}

void thread_detach(RtThread *t) {
    if (!t) return;
    if (t->kthread && (t->foreign || !t->has_pt)) {
        thread_mark_done(t->kthread, t->exit_code);
        t->kthread->rt = nullptr;
        t->kthread->release();
        t->kthread = nullptr;
    }
    run_fls_callbacks(t);
    release_resources(t);
    guest_thread = nullptr;
    delete t;
}

uint32_t thread_create(uint32_t start, uint32_t param, uint32_t stack_size, bool suspended, uint32_t *tid_out) {
    auto *t = new RtThread();
    t->start_addr = start;
    t->start_param = param;
    t->suspended_start = suspended;
    if (!prepare(t, stack_size)) { delete t; return 0; }
    auto *kt = new KThread();
    kt->tid = t->tid;
    kt->rt = t;
    kt->suspend_count = suspended ? 1 : 0;
    t->kthread = kt;                 // the thread's own reference
    kt->add_ref();                   // reference for the handle
    if (suspended) extra_for(t, true);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kHostStackSize);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#ifdef __APPLE__
    // without a QoS class iOS may keep the game's worker threads (UE3's render/audio/streaming threads) on the efficiency cores
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INITIATED, 0);
#endif
    if (tid_out) *tid_out = t->tid;
    uint32_t h = handle_alloc(kt);
    auto *sa = new StartArg{t};
    int rc = pthread_create(&t->pt, &attr, thread_body, sa);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        LOGE(LC_THREAD, "pthread_create failed: %d", rc);
        delete sa;
        handle_close(h);
        kt->release();
        release_resources(t);   // not registered yet; harmless
        delete t;
        return 0;
    }
    t->has_pt = true;
    LOGD(LC_THREAD, "CreateThread start=%08x param=%08x -> tid %u handle %x%s", start, param, kt->tid, h, suspended ? " (suspended)" : "");
    return h;
}

uint32_t thread_resume(KThread *kt) {
    uint32_t prev = (uint32_t)kt->suspend_count;
    if (kt->suspend_count > 0) {
        kt->suspend_count--;
        if (kt->suspend_count == 0 && kt->rt) {
            auto ex = extra_for((RtThread *)kt->rt, false);
            if (ex) {
                std::lock_guard<std::mutex> lk(ex->gate.mu);
                ex->gate.go = true;
                ex->gate.cv.notify_all();
            }
        }
    }
    return prev;
}
uint32_t thread_suspend(KThread *kt) {
    static bool warned = false;
    if (!warned) { warned = true; LOGW(LC_THREAD, "SuspendThread on a running thread is only counted, not enforced"); }
    return (uint32_t)kt->suspend_count++;
}

void thread_exit_current(uint32_t code) {
    RtThread *t = thread_current();
    if (!t) _exit((int)code);
    t->exit_code = code;
    if (t->exit_jmp_valid) longjmp(t->exit_jmp, 1);
    LOGE(LC_THREAD, "ExitThread on a thread without an exit point; terminating process");
    log_flush();
    _exit((int)code);
}

namespace {
struct MainArg { int (*fn)(void *); void *arg; uint32_t stack; int rc; };
void *main_body(void *p) {
    auto *m = (MainArg *)p;
    RtThread *t = thread_attach("guest-main", m->stack, false);
    if (!t) { m->rc = -1; return nullptr; }
    t->kthread = nullptr;
    if (setjmp(t->exit_jmp) == 0) {
        t->exit_jmp_valid = true;
        m->rc = m->fn(m->arg);
    } else {
        m->rc = (int)t->exit_code;
    }
    t->exit_jmp_valid = false;
    thread_detach(t);
    return nullptr;
}
}  // namespace

int thread_run_main(int (*fn)(void *), void *arg, uint32_t guest_stack_size) {
    MainArg m{fn, arg, guest_stack_size, 0};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kMainHostStackSize);
#ifdef __APPLE__
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);   // the game loop: performance cores, highest clocks
#endif
    pthread_t th;
    if (pthread_create(&th, &attr, main_body, &m) != 0) return -1;
    pthread_attr_destroy(&attr);
    pthread_join(th, nullptr);
    return m.rc;
}

void thread_for_each(void (*fn)(RtThread *t, void *user), void *user) {
    std::lock_guard<std::mutex> lk(g_tmu);
    for (RtThread *t : g_threads) fn(t, user);
}
std::vector<RtThread *> thread_list() {
    std::lock_guard<std::mutex> lk(g_tmu);
    return g_threads;
}
uint32_t thread_count() {
    std::lock_guard<std::mutex> lk(g_tmu);
    return (uint32_t)g_threads.size();
}

// ---- TLS -----------------------------------------------------------------------------------------------------
uint32_t tls_alloc() {
    std::lock_guard<std::mutex> lk(g_tls_mu);
    for (uint32_t i = 0; i < kTlsMaxSlots; i++)
        if (!g_tls_used[i]) { g_tls_used[i] = true; return i; }
    return 0xFFFFFFFFu;
}
static uint32_t *tls_slot_ptr(RtThread *t, uint32_t idx, bool create) {
    uint8_t *teb = guest_base + t->teb;
    if (idx < kTlsMinSlots) return (uint32_t *)(teb + TEB_TLS_SLOTS + 4 * idx);
    uint32_t ext = *(uint32_t *)(teb + TEB_TLS_EXPANSION);
    if (!ext) {
        if (!create) return nullptr;
        ext = vm_alloc(0, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, nullptr);
        if (!ext) return nullptr;
        *(uint32_t *)(teb + TEB_TLS_EXPANSION) = ext;
    }
    return (uint32_t *)(guest_base + ext + 4 * (idx - kTlsMinSlots));
}
bool tls_free(uint32_t idx) {
    if (idx >= kTlsMaxSlots) return false;
    {
        std::lock_guard<std::mutex> lk(g_tls_mu);
        if (!g_tls_used[idx]) return false;
        g_tls_used[idx] = false;
    }
    for (RtThread *t : thread_list())
        if (uint32_t *p = tls_slot_ptr(t, idx, false)) *p = 0;
    return true;
}
uint32_t tls_get(uint32_t idx) {
    if (idx >= kTlsMaxSlots) return 0;
    uint32_t *p = tls_slot_ptr(thread_current(), idx, false);
    return p ? *p : 0;
}
bool tls_set(uint32_t idx, uint32_t v) {
    if (idx >= kTlsMaxSlots) return false;
    {
        std::lock_guard<std::mutex> lk(g_tls_mu);
        if (!g_tls_used[idx]) return false;
    }
    uint32_t *p = tls_slot_ptr(thread_current(), idx, true);
    if (!p) return false;
    *p = v;
    return true;
}

// ---- PE static TLS ----------------------------------------------------------------------------------------------
void pe_tls_register(const PeTls &t) {
    std::lock_guard<std::mutex> lk(g_petls_mu);
    PeTls c = t;
    c.index = (uint32_t)g_petls.size();
    if (c.index_addr) gref<uint32_t>(c.index_addr) = c.index;
    g_petls.push_back(c);
}

void pe_tls_init_thread(RtThread *t) {
    std::vector<PeTls> mods;
    {
        std::lock_guard<std::mutex> lk(g_petls_mu);
        mods = g_petls;
    }
    uint32_t arr_bytes = rup((uint32_t)std::max<size_t>(mods.size(), 1) * 4, 16);
    uint32_t total = arr_bytes;
    std::vector<uint32_t> offs;
    for (auto &m : mods) {
        offs.push_back(total);
        total += rup((m.end_raw - m.start_raw) + m.zero_fill, 16);
    }
    uint32_t err;
    uint32_t blk = vm_alloc(0, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &err);
    if (!blk) fatal("cannot allocate PE TLS block (%u bytes)", total);
    for (size_t i = 0; i < mods.size(); i++) {
        uint32_t dst = blk + offs[i];
        uint32_t n = mods[i].end_raw - mods[i].start_raw;
        if (n) memcpy(guest_base + dst, guest_base + mods[i].start_raw, n);
        gref<uint32_t>(blk + 4 * mods[i].index) = dst;
    }
    uint8_t *teb = guest_base + t->teb;
    uint32_t old = *(uint32_t *)(teb + 0xF98);
    *(uint32_t *)(teb + TEB_TLS_ARRAY) = blk;
    *(uint32_t *)(teb + 0xF98) = blk;
    if (old) vm_free(old, 0, MEM_RELEASE);
}

// ---- FLS -----------------------------------------------------------------------------------------------------------
uint32_t fls_alloc(uint32_t cb) {
    std::lock_guard<std::mutex> lk(g_fls_mu);
    for (size_t i = 0; i < g_fls_slots.size(); i++)
        if (!g_fls_slots[i].used) { g_fls_slots[i] = {true, cb}; return (uint32_t)i; }
    if (g_fls_slots.size() >= 128) return 0xFFFFFFFFu;
    g_fls_slots.push_back({true, cb});
    return (uint32_t)g_fls_slots.size() - 1;
}
bool fls_free(uint32_t idx) {
    uint32_t cb = 0;
    {
        std::lock_guard<std::mutex> lk(g_fls_mu);
        if (idx >= g_fls_slots.size() || !g_fls_slots[idx].used) return false;
        cb = g_fls_slots[idx].cb;
        g_fls_slots[idx].used = false;
    }
    for (RtThread *t : thread_list()) {
        if (idx < t->fls.size()) {
            uint32_t v = t->fls[idx];
            t->fls[idx] = 0;
            (void)cb; (void)v;   // callbacks for other threads cannot be run from here
        }
    }
    RtThread *cur = thread_current();
    (void)cur;
    return true;
}
uint32_t fls_get(uint32_t idx, bool *ok) {
    RtThread *t = thread_current();
    std::lock_guard<std::mutex> lk(g_fls_mu);
    if (idx >= g_fls_slots.size() || !g_fls_slots[idx].used) { *ok = false; return 0; }
    *ok = true;
    return idx < t->fls.size() ? t->fls[idx] : 0;
}
bool fls_set(uint32_t idx, uint32_t v) {
    RtThread *t = thread_current();
    std::lock_guard<std::mutex> lk(g_fls_mu);
    if (idx >= g_fls_slots.size() || !g_fls_slots[idx].used) return false;
    if (t->fls.size() <= idx) t->fls.resize(idx + 1, 0);
    t->fls[idx] = v;
    return true;
}

}  // namespace dish
