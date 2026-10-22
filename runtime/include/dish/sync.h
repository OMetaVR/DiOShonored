// Synchronisation: events, mutexes, semaphores, waits, critical sections (guest memory), SRW locks, parking lot.
#pragma once
#include <cstdint>
#include "dish/handles.h"

namespace dish {

struct KEvent : KObject {
    bool manual, signaled;
    KEvent(bool m, bool s) : KObject(KType::Event), manual(m), signaled(s) {}
    bool waitable() const override { return true; }
    bool try_acquire(uint32_t) override { if (!signaled) return false; if (!manual) signaled = false; return true; }
    bool is_signaled(uint32_t) const override { return signaled; }
};
struct KMutex : KObject {
    uint32_t owner = 0, count = 0;
    bool abandoned = false;
    KMutex() : KObject(KType::Mutex) {}
    bool waitable() const override { return true; }
    bool try_acquire(uint32_t tid) override {
        if (owner == 0 || owner == tid) { owner = tid; count++; return true; }
        return false;
    }
    bool is_signaled(uint32_t tid) const override { return owner == 0 || owner == tid; }
};
struct KSemaphore : KObject {
    int32_t count, max;
    KSemaphore(int32_t c, int32_t m) : KObject(KType::Semaphore), count(c), max(m) {}
    bool waitable() const override { return true; }
    bool try_acquire(uint32_t) override { if (count > 0) { count--; return true; } return false; }
    bool is_signaled(uint32_t) const override { return count > 0; }
};
struct KThread : KObject {
    bool done = false;
    uint32_t exit_code = 0x103;   // STILL_ACTIVE
    uint32_t tid = 0;
    void *rt = nullptr;           // RtThread*, valid while !done
    int suspend_count = 0;
    KThread() : KObject(KType::Thread) {}
    bool waitable() const override { return true; }
    bool try_acquire(uint32_t) override { return done; }
    bool is_signaled(uint32_t) const override { return done; }
};
struct KProcess : KObject {
    bool done = false;
    uint32_t exit_code = 0x103;
    KProcess() : KObject(KType::Process) {}
    bool waitable() const override { return true; }
    bool try_acquire(uint32_t) override { return done; }
    bool is_signaled(uint32_t) const override { return done; }
};

enum : uint32_t { WAIT_OBJECT_0 = 0, WAIT_ABANDONED_0 = 0x80, WAIT_IO_COMPLETION = 0xC0, WAIT_TIMEOUT = 0x102, WAIT_FAILED = 0xFFFFFFFF,
                  INFINITE_TIMEOUT = 0xFFFFFFFF };

// All of these take the single sync mutex; state changes wake the waiters of that object only.
void ksync_signal(KObject *o);                                   // after changing state under sync lock helper below
bool event_set(KEvent *e);
bool event_reset(KEvent *e);
bool event_pulse(KEvent *e);
bool mutex_release(KMutex *m, uint32_t tid);                     // false if not owner
bool semaphore_release(KSemaphore *s, int32_t n, int32_t *prev); // false if it would exceed max
void thread_mark_done(KThread *t, uint32_t code);
void process_mark_done(KProcess *p, uint32_t code);

// Wait on up to 64 objects. Returns WAIT_OBJECT_0+i, WAIT_TIMEOUT, WAIT_FAILED.
uint32_t wait_objects(KObject *const *objs, uint32_t n, bool wait_all, uint32_t timeout_ms, uint32_t tid);
void sleep_ms(uint32_t ms);

// Parking lot: block while *(uint32_t*)(guest_base+addr) == expected; wake wakes all/one waiters on an address.
// timeout_ms == INFINITE_TIMEOUT blocks forever. Returns false on timeout.
bool park(uint32_t addr, uint32_t expected, uint32_t timeout_ms);
void unpark(uint32_t addr, bool all);

// Resolve a handle value, including the pseudo handles (-1 current process, -2 current thread). New reference or nullptr.
KObject *kobj_resolve(uint32_t h);
KProcess *kprocess();

// Critical section living in guest memory (RTL_CRITICAL_SECTION layout:
//   +0 DebugInfo +4 LockCount(-1 = free) +8 RecursionCount ... see sync.cpp)
void cs_init(uint32_t cs, uint32_t spin);
void cs_delete(uint32_t cs);
void cs_enter(uint32_t cs);
bool cs_try_enter(uint32_t cs);
void cs_leave(uint32_t cs);

}  // namespace dish
