// kernel32: threads, TLS, FLS, affinity, process identity.
#include "dish/call.h"
#include "dish/hle.h"
#include "dish/loader.h"
#include "dish/sync.h"
#include "dish/thread.h"
#include "dish/util.h"
#include <cstring>
#include <unistd.h>

namespace dish {
namespace {

uint32_t k32_CreateThread(uint32_t, uint32_t stack, uint32_t start, uint32_t param, uint32_t flags, uint32_t ptid) {
    uint32_t tid = 0;
    uint32_t h = thread_create(start, param, stack, (flags & 4) != 0, &tid);   // CREATE_SUSPENDED = 4
    if (!h) { set_last_error(ERR_NOT_ENOUGH_MEMORY); return 0; }
    if (ptid) wr32(ptid, tid);
    return h;
}
uint32_t k32_CreateRemoteThread(uint32_t proc, uint32_t a, uint32_t stack, uint32_t start, uint32_t param, uint32_t flags, uint32_t ptid) {
    if (proc != HANDLE_CUR_PROCESS) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    return k32_CreateThread(a, stack, start, param, flags, ptid);
}
void k32_ExitThread(uint32_t code) { thread_exit_current(code); }
uint32_t k32_GetCurrentThreadId() { return cur_tid(); }
uint32_t k32_GetCurrentThread() { return HANDLE_CUR_THREAD; }
uint32_t k32_GetCurrentProcess() { return HANDLE_CUR_PROCESS; }
uint32_t k32_GetCurrentProcessId() { return process_id(); }
uint32_t k32_GetThreadId(uint32_t h) {
    KObject *o = kobj_resolve(h);
    if (!o || o->type != KType::Thread) { if (o) o->release(); set_last_error(ERR_INVALID_HANDLE); return 0; }
    uint32_t t = static_cast<KThread *>(o)->tid;
    o->release();
    return t;
}
uint32_t k32_GetProcessId(uint32_t) { return process_id(); }
uint32_t k32_GetExitCodeThread(uint32_t h, uint32_t out) {
    KObject *o = kobj_resolve(h);
    if (!o || o->type != KType::Thread) { if (o) o->release(); set_last_error(ERR_INVALID_HANDLE); return 0; }
    auto *t = static_cast<KThread *>(o);
    wr32(out, t->done ? t->exit_code : 0x103);
    o->release();
    return 1;
}
uint32_t k32_GetExitCodeProcess(uint32_t h, uint32_t out) {
    KObject *o = kobj_resolve(h);
    if (!o || o->type != KType::Process) { if (o) o->release(); set_last_error(ERR_INVALID_HANDLE); return 0; }
    auto *p = static_cast<KProcess *>(o);
    wr32(out, p->done ? p->exit_code : 0x103);
    o->release();
    return 1;
}
uint32_t k32_ResumeThread(uint32_t h) {
    KObject *o = kobj_resolve(h);
    if (!o || o->type != KType::Thread) { if (o) o->release(); set_last_error(ERR_INVALID_HANDLE); return 0xFFFFFFFFu; }
    uint32_t r = thread_resume(static_cast<KThread *>(o));
    o->release();
    return r;
}
uint32_t k32_SuspendThread(uint32_t h) {
    KObject *o = kobj_resolve(h);
    if (!o || o->type != KType::Thread) { if (o) o->release(); set_last_error(ERR_INVALID_HANDLE); return 0xFFFFFFFFu; }
    uint32_t r = thread_suspend(static_cast<KThread *>(o));
    o->release();
    return r;
}
uint32_t k32_TerminateThread(uint32_t h, uint32_t code) {
    KObject *o = kobj_resolve(h);
    if (!o || o->type != KType::Thread) { if (o) o->release(); set_last_error(ERR_INVALID_HANDLE); return 0; }
    LOGW(LC_THREAD, "TerminateThread(tid %u) is not supported; marking it done (the host thread keeps running)", static_cast<KThread *>(o)->tid);
    thread_mark_done(static_cast<KThread *>(o), code);
    o->release();
    return 1;
}
int g_prio_dummy = 0;
uint32_t k32_SetThreadPriority(uint32_t, int32_t) { (void)g_prio_dummy; return 1; }
uint32_t k32_GetThreadPriority(uint32_t) { return 0; }
uint32_t k32_SetThreadPriorityBoost(uint32_t, uint32_t) { return 1; }
uint32_t k32_SetThreadAffinityMask(uint32_t, uint32_t mask) { (void)mask; return 0xFF; }
uint32_t k32_SetThreadIdealProcessor(uint32_t, uint32_t) { return 0; }
uint32_t k32_GetProcessAffinityMask(uint32_t, uint32_t pproc, uint32_t psys) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    uint32_t mask = n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1);
    if (pproc) wr32(pproc, mask);
    if (psys) wr32(psys, mask);
    return 1;
}
uint32_t k32_SetProcessAffinityMask(uint32_t, uint32_t) { return 1; }
uint32_t k32_GetCurrentProcessorNumber() { return 0; }
uint32_t k32_SetThreadStackGuarantee(uint32_t) { return 1; }
uint32_t k32_GetThreadTimes(uint32_t, uint32_t c, uint32_t e, uint32_t k, uint32_t u) {
    for (uint32_t p : {c, e, k, u}) if (p) wr64(p, 0);
    return 1;
}
uint32_t k32_GetProcessTimes(uint32_t, uint32_t c, uint32_t e, uint32_t k, uint32_t u) { return k32_GetThreadTimes(0, c, e, k, u); }
uint32_t k32_SetThreadDescription(uint32_t, uint32_t) { return 0; }
uint32_t k32_QueueUserAPC(uint32_t, uint32_t, uint32_t) { LOGW(LC_THREAD, "QueueUserAPC is not supported"); return 0; }

// ---- TLS / FLS
uint32_t k32_TlsAlloc() {
    uint32_t i = tls_alloc();
    if (i == 0xFFFFFFFFu) set_last_error(ERR_NOT_ENOUGH_MEMORY);
    return i;
}
uint32_t k32_TlsFree(uint32_t i) {
    if (!tls_free(i)) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    return 1;
}
uint32_t k32_TlsGetValue(uint32_t i) {
    set_last_error(0);
    return tls_get(i);
}
uint32_t k32_TlsSetValue(uint32_t i, uint32_t v) {
    if (!tls_set(i, v)) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    return 1;
}
uint32_t k32_FlsAlloc(uint32_t cb) {
    uint32_t i = fls_alloc(cb);
    if (i == 0xFFFFFFFFu) set_last_error(ERR_NOT_ENOUGH_MEMORY);
    return i;
}
uint32_t k32_FlsFree(uint32_t i) { return fls_free(i) ? 1 : (set_last_error(ERR_INVALID_PARAMETER), 0); }
uint32_t k32_FlsGetValue(uint32_t i) {
    bool ok;
    uint32_t v = fls_get(i, &ok);
    if (!ok) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    set_last_error(0);
    LOGT(LC_THREAD, "FlsGetValue(%u) = %08x", i, v);
    return v;
}
uint32_t k32_FlsSetValue(uint32_t i, uint32_t v) { return fls_set(i, v) ? 1 : (set_last_error(ERR_INVALID_PARAMETER), 0); }

uint32_t k32_GetLastError() { return get_last_error(); }
void k32_SetLastError(uint32_t e) { set_last_error(e); }
void k32_RestoreLastError(uint32_t e) { set_last_error(e); }

}  // namespace

HLE("kernel32.dll", "CreateThread", Stdcall, k32_CreateThread);
HLE("kernel32.dll", "CreateRemoteThread", Stdcall, k32_CreateRemoteThread);
HLE("kernel32.dll", "ExitThread", Stdcall, k32_ExitThread);
HLE("kernel32.dll", "GetCurrentThreadId", Stdcall, k32_GetCurrentThreadId);
HLE("kernel32.dll", "GetCurrentThread", Stdcall, k32_GetCurrentThread);
HLE("kernel32.dll", "GetCurrentProcess", Stdcall, k32_GetCurrentProcess);
HLE("kernel32.dll", "GetCurrentProcessId", Stdcall, k32_GetCurrentProcessId);
HLE("kernel32.dll", "GetThreadId", Stdcall, k32_GetThreadId);
HLE("kernel32.dll", "GetProcessId", Stdcall, k32_GetProcessId);
HLE("kernel32.dll", "GetExitCodeThread", Stdcall, k32_GetExitCodeThread);
HLE("kernel32.dll", "GetExitCodeProcess", Stdcall, k32_GetExitCodeProcess);
HLE("kernel32.dll", "ResumeThread", Stdcall, k32_ResumeThread);
HLE("kernel32.dll", "SuspendThread", Stdcall, k32_SuspendThread);
HLE("kernel32.dll", "TerminateThread", Stdcall, k32_TerminateThread);
HLE("kernel32.dll", "SetThreadPriority", Stdcall, k32_SetThreadPriority);
HLE("kernel32.dll", "GetThreadPriority", Stdcall, k32_GetThreadPriority);
HLE("kernel32.dll", "SetThreadPriorityBoost", Stdcall, k32_SetThreadPriorityBoost);
HLE("kernel32.dll", "SetThreadAffinityMask", Stdcall, k32_SetThreadAffinityMask);
HLE("kernel32.dll", "SetThreadIdealProcessor", Stdcall, k32_SetThreadIdealProcessor);
HLE("kernel32.dll", "GetProcessAffinityMask", Stdcall, k32_GetProcessAffinityMask);
HLE("kernel32.dll", "SetProcessAffinityMask", Stdcall, k32_SetProcessAffinityMask);
HLE("kernel32.dll", "GetCurrentProcessorNumber", Stdcall, k32_GetCurrentProcessorNumber);
HLE("kernel32.dll", "SetThreadStackGuarantee", Stdcall, k32_SetThreadStackGuarantee);
HLE("kernel32.dll", "GetThreadTimes", Stdcall, k32_GetThreadTimes);
HLE("kernel32.dll", "GetProcessTimes", Stdcall, k32_GetProcessTimes);
HLE("kernel32.dll", "SetThreadDescription", Stdcall, k32_SetThreadDescription);
HLE("kernel32.dll", "QueueUserAPC", Stdcall, k32_QueueUserAPC);
HLE("kernel32.dll", "TlsAlloc", Stdcall, k32_TlsAlloc);
HLE("kernel32.dll", "TlsFree", Stdcall, k32_TlsFree);
HLE("kernel32.dll", "TlsGetValue", Stdcall, k32_TlsGetValue);
HLE("kernel32.dll", "TlsSetValue", Stdcall, k32_TlsSetValue);
HLE("kernel32.dll", "FlsAlloc", Stdcall, k32_FlsAlloc);
HLE("kernel32.dll", "FlsFree", Stdcall, k32_FlsFree);
HLE("kernel32.dll", "FlsGetValue", Stdcall, k32_FlsGetValue);
HLE("kernel32.dll", "FlsSetValue", Stdcall, k32_FlsSetValue);
HLE("kernel32.dll", "GetLastError", Stdcall, k32_GetLastError);
HLE("kernel32.dll", "SetLastError", Stdcall, k32_SetLastError);
HLE("kernel32.dll", "RestoreLastError", Stdcall, k32_RestoreLastError);

}  // namespace dish
