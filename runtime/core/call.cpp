#include "dish/call.h"
#include "dish/thunk.h"
#include "dish/thread.h"
#include "dish/vm.h"
#include "dish/log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <atomic>
#include <setjmp.h>

namespace dish {
namespace {
UnresolvedHandler g_unres_handler = nullptr;
std::string g_unres_path = "unresolved.log";
std::atomic<uint64_t> g_unres_count{0};
std::mutex g_pop_mu;
std::set<uint32_t> g_pop_warned;

GuestRet default_unresolved(uint32_t target, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {
    char line[256];
    snprintf(line, sizeof line, "UNRESOLVED guest_call target=%08x esp=%08x eax=%08x ecx=%08x edx=%08x", target, esp, eax, ecx, edx);
    LOGE(LC_CORE, "%s", line);
    FILE *f = fopen(g_unres_path.c_str(), "a");
    FILE *outs[2] = {f, nullptr};
    for (FILE *o : outs) {
        if (!o) continue;
        fprintf(o, "%s\n", line);
        RtThread *t = thread_current();
        if (t) fprintf(o, "  thread tid=%u name=%s stack=[%08x,%08x)\n", t->tid, t->name.c_str(), t->gt.stack_limit, t->gt.stack_base);
        fprintf(o, "  caller (return slot) = ");
        if (vm_is_committed(esp, 4)) fprintf(o, "%08x\n", gref<uint32_t>(esp)); else fprintf(o, "<unmapped>\n");
        fprintf(o, "  guest stack:\n");
        for (int i = 0; i < 48; i++) {
            uint32_t a = esp + 4u * i;
            if (!vm_is_committed(a, 4)) break;
            fprintf(o, "    [%08x] = %08x\n", a, gref<uint32_t>(a));
        }
    }
    if (f) fclose(f);
    log_flush();
    abort();
}
}  // namespace

void set_unresolved_handler(UnresolvedHandler h) { g_unres_handler = h; }
void set_unresolved_log_path(const char *p) { g_unres_path = p; }
uint64_t unresolved_count() { return g_unres_count.load(); }

}  // namespace dish

using namespace dish;

extern "C" {

_Thread_local GuestThread *guest_thread = nullptr;

GuestRet guest_unresolved(uint32_t target, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {
    g_unres_count++;
    return (g_unres_handler ? g_unres_handler : default_unresolved)(target, esp, eax, ecx, edx);
}

// ---- EH landing pads (docs/guest_abi.md section 8): non-local resumption into catch / __except continuations
static_assert(sizeof(jmp_buf) <= sizeof(((GuestPad *)nullptr)->jb), "GuestPad.jb is too small for this host's jmp_buf");

void guest_pad_push(GuestPad *p, uint32_t frame) {
    GuestThread *g = guest_thread;
    RtThread *t = thread_current();
    p->prev = g->pads;
    p->frame = frame;
    p->pushed = 1;
    p->cur_sp = t ? t->cur_sp : 0;
    g->pads = p;
}

void guest_pad_leave(GuestPad *p) {
    if (!p->pushed) return;
    p->pushed = 0;
    guest_thread->pads = p->prev;   // pads of frames a resume abandoned were dropped by the resume itself
}

// A jump to an address that is no function entry: when a registered frame's ebp (handed over in ext_regs) matches, it is a
// continuation of that frame's function. Unwind the host stack back into it (the guest stack and SEH chain were already
// unwound by the guest's own EH code) and let it continue there.
[[noreturn]] static void resume_into(GuestPad *p, uint32_t target, uint32_t ebp) {
    GuestThread *g = guest_thread;
    g->resume_target = target;
    g->resume_ebp = ebp;
    g->pads = p;
    if (RtThread *t = thread_current()) t->cur_sp = p->cur_sp;
    LOGD(LC_EXC, "resume at %08x in frame %08x", target, ebp);
    _longjmp(*reinterpret_cast<jmp_buf *>(p->jb), 1);
}
static GuestPad *pad_for(uint32_t ebp) {
    GuestThread *g = guest_thread;
    if (!g) return nullptr;
    for (GuestPad *p = g->pads; p; p = p->prev)
        if (p->frame == ebp) return p;
    return nullptr;
}

GuestRet guest_call(uint32_t target, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {
    if (__builtin_expect(is_thunk_addr(target), 0)) {
        uint32_t idx = thunk_index(target);
        const ThunkEntry &e = g_thunks[idx];
        if (__builtin_expect(e.fn == nullptr, 0)) return guest_unresolved(target, esp, eax, ecx, edx);
        if (__builtin_expect(e.flags != 0, 0)) tl_thunk_idx = idx;
        if (__builtin_expect(g_hle_trace, 0)) {
            const uint32_t *a = (const uint32_t *)(guest_base + esp);
            LOGT(LC_HLE, "call %s!%s esp=%08x args %08x %08x %08x %08x", e.dll ? e.dll : "?", e.name ? e.name : "?", esp,
                 a[1], a[2], a[3], a[4]);
        }
        // Lifted code keeps esp in a C local, so the runtime only learns the live guest stack position here. A host -> guest callback
        // made while this thunk runs (DllMain from LoadLibrary, a WndProc from DispatchMessage, ...) must build its frame below the
        // caller's frame, not at the stale position of the outermost guest_invoke.
        RtThread *t = thread_current();
        if (__builtin_expect(t == nullptr, 0)) return e.fn(esp, eax, ecx, edx);
        const uint32_t saved_sp = t->cur_sp;
        t->cur_sp = esp;
        GuestRet r = e.fn(esp, eax, ecx, edx);
        t->cur_sp = saved_sp;
        return r;
    }
    GuestFn f = guest_lookup(target);
    if (__builtin_expect(f != nullptr, 1)) return f(esp, eax, ecx, edx);
    if (target == kReturnSentinel) return guest_ret(eax, edx, 0);
    if (guest_thread) {
        const uint32_t ebp = guest_thread->ext_regs[3];
        if (GuestPad *p = pad_for(ebp)) resume_into(p, target, ebp);
    }
    return guest_unresolved(target, esp, eax, ecx, edx);
}

}  // extern "C"

namespace dish {

uint32_t guest_invoke_at(uint32_t esp, uint32_t target, Conv conv, const uint32_t *args, unsigned nargs, GuestRet *out) {
    RtThread *t = thread_current();
    uint32_t sp = esp & ~15u;
    unsigned in_regs = conv == Conv::Thiscall ? 1 : conv == Conv::Fastcall ? 2 : 0;
    if (in_regs > nargs) in_regs = nargs;
    uint32_t ecx = in_regs >= 1 ? args[0] : 0, edx = in_regs >= 2 ? args[1] : 0;
    unsigned nstack = nargs - in_regs;
    if (t && t->gt.stack_limit && sp - 4u * (nstack + 4) < t->gt.stack_limit + 256) {
        fatal("guest stack overflow in guest_invoke (esp=%08x limit=%08x target=%08x)", esp, t->gt.stack_limit, target);
    }
    for (unsigned i = nargs; i > in_regs; i--) {
        sp -= 4;
        gref<uint32_t>(sp) = args[i - 1];
    }
    sp -= 4;
    gref<uint32_t>(sp) = kReturnSentinel;
    uint32_t saved_sp = t ? t->cur_sp : 0;
    if (t) t->cur_sp = sp;
    GuestRet r = guest_call(target, sp, 0, ecx, edx);
    if (t) t->cur_sp = saved_sp;
    unsigned expect = conv == Conv::Cdecl ? 0 : 4 * nstack;
    if (guest_ret_pop(r) != expect) {
        std::lock_guard<std::mutex> lk(g_pop_mu);
        if (g_pop_warned.insert(target).second)
            LOGW(LC_CORE, "guest_invoke(%08x): callee popped %u bytes, expected %u for conv %d/%u args", target, guest_ret_pop(r), expect, (int)conv, nargs);
    }
    if (out) *out = r;
    return guest_ret_eax(r);
}

uint32_t guest_invoke_args(uint32_t target, Conv conv, const uint32_t *args, unsigned nargs, GuestRet *out) {
    RtThread *t = thread_current();
    if (!t) fatal("guest_invoke on a thread that is not attached");
    return guest_invoke_at(t->cur_sp, target, conv, args, nargs, out);
}

}  // namespace dish
