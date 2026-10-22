#include "dish/sync.h"
#include "dish/thread.h"
#include "dish/vm.h"
#include "dish/log.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <sched.h>
#include <pthread.h>

namespace dish {

struct Waiter {
    std::condition_variable cv;
    KObject *forced = nullptr;
};

namespace {
std::mutex g_sync_mu;
using Clock = std::chrono::steady_clock;

void wake_all_locked(KObject *o) {
    for (Waiter *w : o->waiters) w->cv.notify_one();
}
}  // namespace

void ksync_signal(KObject *o) {
    std::lock_guard<std::mutex> lk(g_sync_mu);
    wake_all_locked(o);
}

bool event_set(KEvent *e) {
    std::lock_guard<std::mutex> lk(g_sync_mu);
    e->signaled = true;
    wake_all_locked(e);
    return true;
}
bool event_reset(KEvent *e) {
    std::lock_guard<std::mutex> lk(g_sync_mu);
    e->signaled = false;
    return true;
}
bool event_pulse(KEvent *e) {
    std::lock_guard<std::mutex> lk(g_sync_mu);
    bool first = true;
    for (Waiter *w : e->waiters) {
        w->forced = e;
        w->cv.notify_one();
        if (!e->manual) { first = false; break; }   // auto-reset pulse releases a single waiter
    }
    (void)first;
    e->signaled = false;
    return true;
}
bool mutex_release(KMutex *m, uint32_t tid) {
    std::lock_guard<std::mutex> lk(g_sync_mu);
    if (m->owner != tid || m->count == 0) return false;
    if (--m->count == 0) { m->owner = 0; wake_all_locked(m); }
    return true;
}
bool semaphore_release(KSemaphore *s, int32_t n, int32_t *prev) {
    std::lock_guard<std::mutex> lk(g_sync_mu);
    if (n <= 0 || s->count + (int64_t)n > s->max) return false;
    if (prev) *prev = s->count;
    s->count += n;
    wake_all_locked(s);
    return true;
}
void thread_mark_done(KThread *t, uint32_t code) {
    std::lock_guard<std::mutex> lk(g_sync_mu);
    t->done = true;
    t->exit_code = code;
    wake_all_locked(t);
}
void process_mark_done(KProcess *p, uint32_t code) {
    std::lock_guard<std::mutex> lk(g_sync_mu);
    p->done = true;
    p->exit_code = code;
    wake_all_locked(p);
}

uint32_t wait_objects(KObject *const *objs, uint32_t n, bool wait_all, uint32_t timeout_ms, uint32_t tid) {
    if (n == 0 || n > 64) return WAIT_FAILED;
    for (uint32_t i = 0; i < n; i++) if (!objs[i] || !objs[i]->waitable()) return WAIT_FAILED;
    std::unique_lock<std::mutex> lk(g_sync_mu);
    auto check = [&]() -> int {
        if (wait_all) {
            for (uint32_t i = 0; i < n; i++) if (!objs[i]->is_signaled(tid)) return -1;
            for (uint32_t i = 0; i < n; i++) objs[i]->try_acquire(tid);
            return 0;
        }
        for (uint32_t i = 0; i < n; i++)
            if (objs[i]->is_signaled(tid)) { objs[i]->try_acquire(tid); return (int)i; }
        return -1;
    };
    int r = check();
    if (r >= 0) return WAIT_OBJECT_0 + r;
    if (timeout_ms == 0) return WAIT_TIMEOUT;
    Waiter w;
    for (uint32_t i = 0; i < n; i++) objs[i]->waiters.push_back(&w);
    auto unreg = [&]() {
        for (uint32_t i = 0; i < n; i++) {
            auto &v = objs[i]->waiters;
            v.erase(std::remove(v.begin(), v.end(), &w), v.end());
        }
    };
    bool inf = timeout_ms == INFINITE_TIMEOUT;
    auto deadline = Clock::now() + std::chrono::milliseconds(inf ? 0 : timeout_ms);
    uint32_t result = WAIT_TIMEOUT;
    for (;;) {
        if (w.forced && !wait_all) {
            for (uint32_t i = 0; i < n; i++) if (objs[i] == w.forced) { result = WAIT_OBJECT_0 + i; break; }
            break;
        }
        r = check();
        if (r >= 0) { result = WAIT_OBJECT_0 + r; break; }
        if (inf) w.cv.wait(lk);
        else if (w.cv.wait_until(lk, deadline) == std::cv_status::timeout) {
            r = check();
            result = r >= 0 ? WAIT_OBJECT_0 + r : WAIT_TIMEOUT;
            break;
        }
    }
    unreg();
    return result;
}

void sleep_ms(uint32_t ms) {
    if (ms == 0) { sched_yield(); return; }
    timespec ts{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0) {}
}

// ---- parking lot -------------------------------------------------------------------------------------------
namespace {
struct Bucket {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv;
    Bucket() {
        pthread_condattr_t a;
        pthread_condattr_init(&a);
#ifdef __linux__
        pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
        pthread_cond_init(&cv, &a);
    }
};
Bucket g_buckets[128];
Bucket &bucket_for(uint32_t addr) { return g_buckets[(addr >> 2) * 2654435761u >> 25 & 127]; }
}  // namespace

bool park(uint32_t addr, uint32_t expected, uint32_t timeout_ms) {
    Bucket &b = bucket_for(addr);
    pthread_mutex_lock(&b.mu);
    bool ok = true;
    if (__atomic_load_n((uint32_t *)(guest_base + addr), __ATOMIC_ACQUIRE) == expected) {
        if (timeout_ms == INFINITE_TIMEOUT) {
            pthread_cond_wait(&b.cv, &b.mu);
        } else {
            timespec ts;
#ifdef __linux__
            clock_gettime(CLOCK_MONOTONIC, &ts);
#else
            clock_gettime(CLOCK_REALTIME, &ts);
#endif
            ts.tv_sec += timeout_ms / 1000;
            ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            ok = pthread_cond_timedwait(&b.cv, &b.mu, &ts) != ETIMEDOUT;
        }
    }
    pthread_mutex_unlock(&b.mu);
    return ok;
}
void unpark(uint32_t addr, bool all) {
    (void)all;
    Bucket &b = bucket_for(addr);
    pthread_mutex_lock(&b.mu);
    pthread_cond_broadcast(&b.cv);
    pthread_mutex_unlock(&b.mu);
}

// ---- critical sections -------------------------------------------------------------------------------------
// RTL_CRITICAL_SECTION: +0 DebugInfo, +4 LockCount, +8 RecursionCount, +12 OwningThread, +16 LockSemaphore, +20 SpinCount
// LockCount: -1 free, 0 held, 1 held with (possible) waiters.
namespace {
inline uint32_t *csw(uint32_t cs, int off) { return (uint32_t *)(guest_base + cs + off); }
}

void cs_init(uint32_t cs, uint32_t spin) {
    *csw(cs, 0) = 0;
    __atomic_store_n(csw(cs, 4), 0xFFFFFFFFu, __ATOMIC_RELEASE);
    *csw(cs, 8) = 0;
    *csw(cs, 12) = 0;
    *csw(cs, 16) = 0;
    *csw(cs, 20) = spin & 0x00FFFFFF;
}
void cs_delete(uint32_t cs) {
    *csw(cs, 4) = 0xFFFFFFFFu;
    *csw(cs, 12) = 0;
}
bool cs_try_enter(uint32_t cs) {
    uint32_t tid = guest_thread->host_tid;
    if (__atomic_load_n(csw(cs, 12), __ATOMIC_RELAXED) == tid) { (*csw(cs, 8))++; return true; }
    uint32_t exp = 0xFFFFFFFFu;
    if (__atomic_compare_exchange_n(csw(cs, 4), &exp, 0u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        __atomic_store_n(csw(cs, 12), tid, __ATOMIC_RELAXED);
        *csw(cs, 8) = 1;
        return true;
    }
    return false;
}
void cs_enter(uint32_t cs) {
    uint32_t tid = guest_thread->host_tid;
    uint32_t *lc = csw(cs, 4);
    if (__atomic_load_n(csw(cs, 12), __ATOMIC_RELAXED) == tid) { (*csw(cs, 8))++; return; }
    uint32_t exp = 0xFFFFFFFFu;
    if (!__atomic_compare_exchange_n(lc, &exp, 0u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        uint32_t spin = *csw(cs, 20);
        for (uint32_t i = 0; i < spin && i < 4000; i++) {
            if (__atomic_load_n(lc, __ATOMIC_RELAXED) == 0xFFFFFFFFu) {
                exp = 0xFFFFFFFFu;
                if (__atomic_compare_exchange_n(lc, &exp, 0u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) goto acquired;
            }
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield");
#endif
        }
        while (__atomic_exchange_n(lc, 1u, __ATOMIC_ACQUIRE) != 0xFFFFFFFFu) park(cs + 4, 1, INFINITE_TIMEOUT);
    }
acquired:
    __atomic_store_n(csw(cs, 12), tid, __ATOMIC_RELAXED);
    *csw(cs, 8) = 1;
}
void cs_leave(uint32_t cs) {
    uint32_t *rc = csw(cs, 8);
    if (*rc == 0) { LOGW(LC_SYNC, "LeaveCriticalSection(%08x) on an unowned section", cs); return; }
    if (--(*rc) > 0) return;
    __atomic_store_n(csw(cs, 12), 0u, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(csw(cs, 4), 0xFFFFFFFFu, __ATOMIC_RELEASE) == 1u) unpark(cs + 4, false);
}

}  // namespace dish
