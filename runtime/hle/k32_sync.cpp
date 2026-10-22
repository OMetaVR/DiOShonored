// kernel32: critical sections, events, mutexes, semaphores, waits, Sleep, Interlocked*, SLists, SRW locks, handles.
#include "dish/hle.h"
#include "dish/call.h"
#include "dish/sync.h"
#include "dish/thread.h"
#include "dish/util.h"
#include <cstring>

namespace dish {

KProcess *kprocess() {
    static KProcess *p = [] { auto *k = new KProcess(); return k; }();
    return p;
}

KObject *kobj_resolve(uint32_t h) {
    if (h == HANDLE_CUR_PROCESS) { KProcess *p = kprocess(); p->add_ref(); return p; }
    if (h == HANDLE_CUR_THREAD) {
        RtThread *t = thread_current();
        if (!t) return nullptr;
        KThread *kt = thread_kthread(t);
        kt->add_ref();
        return kt;
    }
    return handle_get(h);
}

namespace {

// ---- critical sections
void k32_InitializeCriticalSection(uint32_t cs) { cs_init(cs, 0); }
uint32_t k32_InitializeCriticalSectionAndSpinCount(uint32_t cs, uint32_t spin) { cs_init(cs, spin); return 1; }
uint32_t k32_InitializeCriticalSectionEx(uint32_t cs, uint32_t spin, uint32_t) { cs_init(cs, spin); return 1; }
void k32_DeleteCriticalSection(uint32_t cs) { cs_delete(cs); }
void k32_EnterCriticalSection(uint32_t cs) { cs_enter(cs); }
void k32_LeaveCriticalSection(uint32_t cs) { cs_leave(cs); }
uint32_t k32_TryEnterCriticalSection(uint32_t cs) { return cs_try_enter(cs); }
uint32_t k32_SetCriticalSectionSpinCount(uint32_t cs, uint32_t spin) {
    uint32_t old = rd32(cs + 20);
    wr32(cs + 20, spin & 0x00FFFFFF);
    return old;
}

// ---- named objects helper
std::string obj_name(uint32_t addr, bool wide) {
    if (!addr) return {};
    return lower_utf8(wide ? read_wstr_utf8(addr) : read_astr_utf8(addr));
}

template <class T> uint32_t create_named(KType t, const std::string &name, T *(*make)(void *), void *arg) {
    KObject *obj = nullptr;
    bool existed = false;
    if (!name.empty()) {
        obj = named_lookup(t, name);
        if (obj) existed = true;
    }
    if (!obj) {
        obj = make(arg);
        if (!name.empty()) { obj->name = name; named_insert(obj); }
    }
    uint32_t h = handle_alloc(obj);
    set_last_error(existed ? 183 : 0);
    return h;
}

uint32_t create_event(uint32_t manual, uint32_t initial, uint32_t name, bool wide) {
    struct A { bool m, s; } a{manual != 0, initial != 0};
    return create_named<KEvent>(KType::Event, obj_name(name, wide), [](void *p) { auto *x = (A *)p; return new KEvent(x->m, x->s); }, &a);
}
uint32_t k32_CreateEventA(uint32_t, uint32_t manual, uint32_t initial, uint32_t name) { return create_event(manual, initial, name, false); }
uint32_t k32_CreateEventW(uint32_t, uint32_t manual, uint32_t initial, uint32_t name) { return create_event(manual, initial, name, true); }
uint32_t k32_CreateEventExW(uint32_t, uint32_t name, uint32_t flags, uint32_t) { return create_event(flags & 1, flags & 2, name, true); }
uint32_t open_named(KType t, uint32_t name, bool wide) {
    KObject *o = named_lookup(t, obj_name(name, wide));
    if (!o) { set_last_error(2); return 0; }
    return handle_alloc(o);
}
uint32_t k32_OpenEventA(uint32_t, uint32_t, uint32_t name) { return open_named(KType::Event, name, false); }
uint32_t k32_OpenEventW(uint32_t, uint32_t, uint32_t name) { return open_named(KType::Event, name, true); }
uint32_t k32_OpenMutexA(uint32_t, uint32_t, uint32_t name) { return open_named(KType::Mutex, name, false); }
uint32_t k32_OpenMutexW(uint32_t, uint32_t, uint32_t name) { return open_named(KType::Mutex, name, true); }
uint32_t k32_OpenSemaphoreW(uint32_t, uint32_t, uint32_t name) { return open_named(KType::Semaphore, name, true); }

uint32_t event_op(uint32_t h, int op) {
    auto e = handle_ref<KEvent>(h, KType::Event);
    if (!e) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    switch (op) {
        case 0: event_set(e.get()); break;
        case 1: event_reset(e.get()); break;
        case 2: event_pulse(e.get()); break;
    }
    return 1;
}
uint32_t k32_SetEvent(uint32_t h) { return event_op(h, 0); }
uint32_t k32_ResetEvent(uint32_t h) { return event_op(h, 1); }
uint32_t k32_PulseEvent(uint32_t h) { return event_op(h, 2); }

uint32_t create_mutex(uint32_t initial_owner, uint32_t name, bool wide) {
    struct A { bool own; } a{initial_owner != 0};
    uint32_t tid = cur_tid();
    uint32_t h = create_named<KMutex>(KType::Mutex, obj_name(name, wide), [](void *) { return new KMutex(); }, &a);
    if (a.own && h) {
        auto m = handle_ref<KMutex>(h, KType::Mutex);
        uint32_t last = get_last_error();
        if (m && last != 183) { m->owner = tid; m->count = 1; }
    }
    return h;
}
uint32_t k32_CreateMutexA(uint32_t, uint32_t own, uint32_t name) { return create_mutex(own, name, false); }
uint32_t k32_CreateMutexW(uint32_t, uint32_t own, uint32_t name) { return create_mutex(own, name, true); }
uint32_t k32_ReleaseMutex(uint32_t h) {
    auto m = handle_ref<KMutex>(h, KType::Mutex);
    if (!m) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    if (!mutex_release(m.get(), cur_tid())) { set_last_error(288); return 0; }   // ERROR_NOT_OWNER
    return 1;
}

uint32_t create_sem(int32_t init, int32_t max, uint32_t name, bool wide) {
    if (max <= 0 || init < 0 || init > max) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    struct A { int32_t i, m; } a{init, max};
    return create_named<KSemaphore>(KType::Semaphore, obj_name(name, wide), [](void *p) { auto *x = (A *)p; return new KSemaphore(x->i, x->m); }, &a);
}
uint32_t k32_CreateSemaphoreA(uint32_t, int32_t i, int32_t m, uint32_t name) { return create_sem(i, m, name, false); }
uint32_t k32_CreateSemaphoreW(uint32_t, int32_t i, int32_t m, uint32_t name) { return create_sem(i, m, name, true); }
uint32_t k32_ReleaseSemaphore(uint32_t h, int32_t n, uint32_t prev_ptr) {
    auto s = handle_ref<KSemaphore>(h, KType::Semaphore);
    if (!s) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    int32_t prev = 0;
    if (!semaphore_release(s.get(), n, &prev)) { set_last_error(298); return 0; }   // ERROR_TOO_MANY_POSTS
    if (prev_ptr) wr32(prev_ptr, (uint32_t)prev);
    return 1;
}

// ---- waiting
uint32_t wait_one(uint32_t h, uint32_t ms) {
    KObject *o = kobj_resolve(h);
    if (!o) { set_last_error(ERR_INVALID_HANDLE); return WAIT_FAILED; }
    if (!o->waitable()) { o->release(); set_last_error(ERR_INVALID_HANDLE); return WAIT_FAILED; }
    uint32_t r = wait_objects(&o, 1, false, ms, cur_tid());
    o->release();
    return r;
}
uint32_t k32_WaitForSingleObject(uint32_t h, uint32_t ms) { return wait_one(h, ms); }
uint32_t k32_WaitForSingleObjectEx(uint32_t h, uint32_t ms, uint32_t) { return wait_one(h, ms); }
uint32_t wait_many(uint32_t n, uint32_t harr, uint32_t all, uint32_t ms) {
    if (n == 0 || n > 64) { set_last_error(ERR_INVALID_PARAMETER); return WAIT_FAILED; }
    KObject *objs[64];
    uint32_t got = 0;
    uint32_t r = WAIT_FAILED;
    for (; got < n; got++) {
        objs[got] = kobj_resolve(rd32(harr + 4 * got));
        if (!objs[got] || !objs[got]->waitable()) { if (objs[got]) objs[got]->release(); set_last_error(ERR_INVALID_HANDLE); goto out; }
    }
    r = wait_objects(objs, n, all != 0, ms, cur_tid());
    --got;
    for (;;) { objs[got]->release(); if (got == 0) break; --got; }
    return r;
out:
    for (uint32_t i = 0; i < got; i++) objs[i]->release();
    return r;
}
uint32_t k32_WaitForMultipleObjects(uint32_t n, uint32_t h, uint32_t all, uint32_t ms) { return wait_many(n, h, all, ms); }
uint32_t k32_WaitForMultipleObjectsEx(uint32_t n, uint32_t h, uint32_t all, uint32_t ms, uint32_t) { return wait_many(n, h, all, ms); }
uint32_t k32_SignalObjectAndWait(uint32_t sig, uint32_t wait, uint32_t ms, uint32_t) {
    KObject *o = kobj_resolve(sig);
    if (!o) { set_last_error(ERR_INVALID_HANDLE); return WAIT_FAILED; }
    if (o->type == KType::Event) event_set(static_cast<KEvent *>(o));
    else if (o->type == KType::Mutex) mutex_release(static_cast<KMutex *>(o), cur_tid());
    else if (o->type == KType::Semaphore) semaphore_release(static_cast<KSemaphore *>(o), 1, nullptr);
    o->release();
    return wait_one(wait, ms);
}
void k32_Sleep(uint32_t ms) { sleep_ms(ms); }
uint32_t k32_SleepEx(uint32_t ms, uint32_t) { sleep_ms(ms); return 0; }
uint32_t k32_SwitchToThread() { sleep_ms(0); return 1; }

uint32_t k32_CloseHandle(uint32_t h) {
    if (h == HANDLE_CUR_PROCESS || h == HANDLE_CUR_THREAD) return 1;
    if (!handle_close(h)) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    return 1;
}
uint32_t k32_DuplicateHandle(uint32_t src_proc, uint32_t src, uint32_t dst_proc, uint32_t out, uint32_t, uint32_t, uint32_t options) {
    if (src_proc != HANDLE_CUR_PROCESS || dst_proc != HANDLE_CUR_PROCESS) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    KObject *o = kobj_resolve(src);
    if (!o) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    uint32_t h = handle_alloc(o);   // adopts the new reference
    if (out) wr32(out, h);
    if (options & 1) handle_close(src);   // DUPLICATE_CLOSE_SOURCE
    return 1;
}
uint32_t k32_GetHandleInformation(uint32_t h, uint32_t out) {
    KObject *o = kobj_resolve(h);
    if (!o) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    o->release();
    if (out) wr32(out, 0);
    return 1;
}
uint32_t k32_SetHandleInformation(uint32_t, uint32_t, uint32_t) { return 1; }

// ---- Interlocked (stdcall in Win32)
inline int32_t *ip(uint32_t a) { return (int32_t *)(guest_base + a); }
int32_t k32_InterlockedIncrement(uint32_t p) { return __atomic_add_fetch(ip(p), 1, __ATOMIC_SEQ_CST); }
int32_t k32_InterlockedDecrement(uint32_t p) { return __atomic_sub_fetch(ip(p), 1, __ATOMIC_SEQ_CST); }
int32_t k32_InterlockedExchange(uint32_t p, int32_t v) { return __atomic_exchange_n(ip(p), v, __ATOMIC_SEQ_CST); }
int32_t k32_InterlockedExchangeAdd(uint32_t p, int32_t v) { return __atomic_fetch_add(ip(p), v, __ATOMIC_SEQ_CST); }
int32_t k32_InterlockedCompareExchange(uint32_t p, int32_t ex, int32_t cmp) {
    __atomic_compare_exchange_n(ip(p), &cmp, ex, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
int64_t k32_InterlockedCompareExchange64(uint32_t p, int64_t ex, int64_t cmp) {
    __atomic_compare_exchange_n((int64_t *)(guest_base + p), &cmp, ex, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

// SLIST_HEADER (8 bytes): +0 Next (u32), +4 Depth (u16), +6 Sequence (u16)
void k32_InitializeSListHead(uint32_t h) { wr64(h, 0); }
uint32_t k32_InterlockedPushEntrySList(uint32_t h, uint32_t e) {
    auto *hp = (uint64_t *)(guest_base + h);
    uint64_t old = __atomic_load_n(hp, __ATOMIC_ACQUIRE), nw;
    do {
        wr32(e, (uint32_t)old);   // entry->Next = head.Next
        uint16_t depth = (uint16_t)(old >> 32), seq = (uint16_t)(old >> 48);
        nw = (uint64_t)e | ((uint64_t)(uint16_t)(depth + 1) << 32) | ((uint64_t)(uint16_t)(seq + 1) << 48);
    } while (!__atomic_compare_exchange_n(hp, &old, nw, false, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE));
    return (uint32_t)old;
}
uint32_t k32_InterlockedPopEntrySList(uint32_t h) {
    auto *hp = (uint64_t *)(guest_base + h);
    uint64_t old = __atomic_load_n(hp, __ATOMIC_ACQUIRE), nw;
    do {
        uint32_t e = (uint32_t)old;
        if (!e) return 0;
        uint16_t depth = (uint16_t)(old >> 32), seq = (uint16_t)(old >> 48);
        nw = (uint64_t)rd32(e) | ((uint64_t)(uint16_t)(depth - 1) << 32) | ((uint64_t)(uint16_t)(seq + 1) << 48);
    } while (!__atomic_compare_exchange_n(hp, &old, nw, false, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE));
    return (uint32_t)old;
}
uint32_t k32_InterlockedFlushSList(uint32_t h) {
    auto *hp = (uint64_t *)(guest_base + h);
    uint64_t old = __atomic_load_n(hp, __ATOMIC_ACQUIRE), nw;
    do {
        if (!(uint32_t)old) return 0;
        nw = ((uint64_t)(uint16_t)((old >> 48) + 1)) << 48;
    } while (!__atomic_compare_exchange_n(hp, &old, nw, false, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE));
    return (uint32_t)old;
}
uint32_t k32_QueryDepthSList(uint32_t h) { return (uint16_t)(rd64(h) >> 32); }

// ---- SRW locks (pointer-sized word): 0 free, 0x80000000 exclusive, else shared count
void k32_InitializeSRWLock(uint32_t p) { wr32(p, 0); }
void k32_AcquireSRWLockExclusive(uint32_t p) {
    uint32_t *w = (uint32_t *)(guest_base + p);
    for (;;) {
        uint32_t exp = 0;
        if (__atomic_compare_exchange_n(w, &exp, 0x80000000u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        park(p, exp, 1);
    }
}
void k32_ReleaseSRWLockExclusive(uint32_t p) {
    __atomic_store_n((uint32_t *)(guest_base + p), 0u, __ATOMIC_RELEASE);
    unpark(p, true);
}
uint32_t k32_TryAcquireSRWLockExclusive(uint32_t p) {
    uint32_t exp = 0;
    return __atomic_compare_exchange_n((uint32_t *)(guest_base + p), &exp, 0x80000000u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
void k32_AcquireSRWLockShared(uint32_t p) {
    uint32_t *w = (uint32_t *)(guest_base + p);
    for (;;) {
        uint32_t cur = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (!(cur & 0x80000000u) && __atomic_compare_exchange_n(w, &cur, cur + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        park(p, cur, 1);
    }
}
void k32_ReleaseSRWLockShared(uint32_t p) {
    __atomic_sub_fetch((uint32_t *)(guest_base + p), 1, __ATOMIC_RELEASE);
    unpark(p, true);
}
// Condition variables (pointer-sized word used as a futex sequence)
void k32_InitializeConditionVariable(uint32_t p) { wr32(p, 0); }
void k32_WakeConditionVariable(uint32_t p) { __atomic_add_fetch((uint32_t *)(guest_base + p), 1, __ATOMIC_SEQ_CST); unpark(p, false); }
void k32_WakeAllConditionVariable(uint32_t p) { __atomic_add_fetch((uint32_t *)(guest_base + p), 1, __ATOMIC_SEQ_CST); unpark(p, true); }
uint32_t k32_SleepConditionVariableCS(uint32_t cv, uint32_t cs, uint32_t ms) {
    uint32_t seq = __atomic_load_n((uint32_t *)(guest_base + cv), __ATOMIC_SEQ_CST);
    uint32_t rec = rd32(cs + 8);
    // fully release the (possibly recursive) section
    for (uint32_t i = 0; i < rec; i++) cs_leave(cs);
    bool ok = park(cv, seq, ms);
    cs_enter(cs);
    for (uint32_t i = 1; i < rec; i++) cs_enter(cs);
    if (!ok) { set_last_error(WAIT_TIMEOUT); return 0; }
    return 1;
}
// InitOnce
uint32_t k32_InitOnceExecuteOnce(const HleCtx &c, uint32_t once, uint32_t fn, uint32_t param, uint32_t ctx_out) {
    uint32_t *w = (uint32_t *)(guest_base + once);
    if (__atomic_load_n(w, __ATOMIC_ACQUIRE) & 2) return 1;
    // 0 = uninit, 1 = running, 3 = complete (value bit 1 = done)
    for (;;) {
        uint32_t exp = 0;
        if (__atomic_compare_exchange_n(w, &exp, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) break;
        if (exp & 2) return 1;
        park(once, exp, 10);
    }
    uint32_t ctx = 0;
    uint32_t ok = guest_invoke_from(c.esp, fn, Conv::Stdcall, once, param, ctx_out ? ctx_out : 0u);
    __atomic_store_n(w, ok ? 2u : 0u, __ATOMIC_RELEASE);
    unpark(once, true);
    (void)ctx;
    return ok;
}

}  // namespace

HLE("kernel32.dll", "InitializeCriticalSection", Stdcall, k32_InitializeCriticalSection);
HLE("kernel32.dll", "InitializeCriticalSectionAndSpinCount", Stdcall, k32_InitializeCriticalSectionAndSpinCount);
HLE("kernel32.dll", "InitializeCriticalSectionEx", Stdcall, k32_InitializeCriticalSectionEx);
HLE("kernel32.dll", "DeleteCriticalSection", Stdcall, k32_DeleteCriticalSection);
HLE("kernel32.dll", "EnterCriticalSection", Stdcall, k32_EnterCriticalSection);
HLE("kernel32.dll", "LeaveCriticalSection", Stdcall, k32_LeaveCriticalSection);
HLE("kernel32.dll", "TryEnterCriticalSection", Stdcall, k32_TryEnterCriticalSection);
HLE("kernel32.dll", "SetCriticalSectionSpinCount", Stdcall, k32_SetCriticalSectionSpinCount);
HLE("kernel32.dll", "CreateEventA", Stdcall, k32_CreateEventA);
HLE("kernel32.dll", "CreateEventW", Stdcall, k32_CreateEventW);
HLE("kernel32.dll", "CreateEventExW", Stdcall, k32_CreateEventExW);
HLE("kernel32.dll", "OpenEventA", Stdcall, k32_OpenEventA);
HLE("kernel32.dll", "OpenEventW", Stdcall, k32_OpenEventW);
HLE("kernel32.dll", "OpenMutexA", Stdcall, k32_OpenMutexA);
HLE("kernel32.dll", "OpenMutexW", Stdcall, k32_OpenMutexW);
HLE("kernel32.dll", "OpenSemaphoreW", Stdcall, k32_OpenSemaphoreW);
HLE("kernel32.dll", "SetEvent", Stdcall, k32_SetEvent);
HLE("kernel32.dll", "ResetEvent", Stdcall, k32_ResetEvent);
HLE("kernel32.dll", "PulseEvent", Stdcall, k32_PulseEvent);
HLE("kernel32.dll", "CreateMutexA", Stdcall, k32_CreateMutexA);
HLE("kernel32.dll", "CreateMutexW", Stdcall, k32_CreateMutexW);
HLE("kernel32.dll", "ReleaseMutex", Stdcall, k32_ReleaseMutex);
HLE("kernel32.dll", "CreateSemaphoreA", Stdcall, k32_CreateSemaphoreA);
HLE("kernel32.dll", "CreateSemaphoreW", Stdcall, k32_CreateSemaphoreW);
HLE("kernel32.dll", "ReleaseSemaphore", Stdcall, k32_ReleaseSemaphore);
HLE("kernel32.dll", "WaitForSingleObject", Stdcall, k32_WaitForSingleObject);
HLE("kernel32.dll", "WaitForSingleObjectEx", Stdcall, k32_WaitForSingleObjectEx);
HLE("kernel32.dll", "WaitForMultipleObjects", Stdcall, k32_WaitForMultipleObjects);
HLE("kernel32.dll", "WaitForMultipleObjectsEx", Stdcall, k32_WaitForMultipleObjectsEx);
HLE("kernel32.dll", "SignalObjectAndWait", Stdcall, k32_SignalObjectAndWait);
HLE("kernel32.dll", "Sleep", Stdcall, k32_Sleep);
HLE("kernel32.dll", "SleepEx", Stdcall, k32_SleepEx);
HLE("kernel32.dll", "SwitchToThread", Stdcall, k32_SwitchToThread);
HLE("kernel32.dll", "CloseHandle", Stdcall, k32_CloseHandle);
HLE("kernel32.dll", "DuplicateHandle", Stdcall, k32_DuplicateHandle);
HLE("kernel32.dll", "GetHandleInformation", Stdcall, k32_GetHandleInformation);
HLE("kernel32.dll", "SetHandleInformation", Stdcall, k32_SetHandleInformation);
HLE("kernel32.dll", "InterlockedIncrement", Stdcall, k32_InterlockedIncrement);
HLE("kernel32.dll", "InterlockedDecrement", Stdcall, k32_InterlockedDecrement);
HLE("kernel32.dll", "InterlockedExchange", Stdcall, k32_InterlockedExchange);
HLE("kernel32.dll", "InterlockedExchangeAdd", Stdcall, k32_InterlockedExchangeAdd);
HLE("kernel32.dll", "InterlockedCompareExchange", Stdcall, k32_InterlockedCompareExchange);
HLE("kernel32.dll", "InterlockedCompareExchange64", Stdcall, k32_InterlockedCompareExchange64);
HLE("kernel32.dll", "InitializeSListHead", Stdcall, k32_InitializeSListHead);
HLE("kernel32.dll", "InterlockedPushEntrySList", Stdcall, k32_InterlockedPushEntrySList);
HLE("kernel32.dll", "InterlockedPopEntrySList", Stdcall, k32_InterlockedPopEntrySList);
HLE("kernel32.dll", "InterlockedFlushSList", Stdcall, k32_InterlockedFlushSList);
HLE("kernel32.dll", "QueryDepthSList", Stdcall, k32_QueryDepthSList);
HLE("kernel32.dll", "InitializeSRWLock", Stdcall, k32_InitializeSRWLock);
HLE("kernel32.dll", "AcquireSRWLockExclusive", Stdcall, k32_AcquireSRWLockExclusive);
HLE("kernel32.dll", "ReleaseSRWLockExclusive", Stdcall, k32_ReleaseSRWLockExclusive);
HLE("kernel32.dll", "TryAcquireSRWLockExclusive", Stdcall, k32_TryAcquireSRWLockExclusive);
HLE("kernel32.dll", "AcquireSRWLockShared", Stdcall, k32_AcquireSRWLockShared);
HLE("kernel32.dll", "ReleaseSRWLockShared", Stdcall, k32_ReleaseSRWLockShared);
HLE("kernel32.dll", "InitializeConditionVariable", Stdcall, k32_InitializeConditionVariable);
HLE("kernel32.dll", "WakeConditionVariable", Stdcall, k32_WakeConditionVariable);
HLE("kernel32.dll", "WakeAllConditionVariable", Stdcall, k32_WakeAllConditionVariable);
HLE("kernel32.dll", "SleepConditionVariableCS", Stdcall, k32_SleepConditionVariableCS);
HLE("kernel32.dll", "InitOnceExecuteOnce", Stdcall, k32_InitOnceExecuteOnce);

}  // namespace dish
