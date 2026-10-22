#include "dish/fault.h"
#include "dish/thread.h"
#include "dish/vm.h"
#include "dish/log.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <dlfcn.h>
#if defined(__APPLE__) && defined(__aarch64__)
#define DISH_FAULT_MACH 1
#include <mach/mach.h>
#include <mach/exception.h>
#include <mach/exc.h>
#include <mach/thread_status.h>
#include <pthread.h>
#elif defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

namespace dish {

uint32_t g_last_fault_addr = 0;
static FaultHook g_hook = nullptr;
static volatile sig_atomic_t g_in_fault = 0;
static bool g_installed = false;

void fault_set_hook(FaultHook h) { g_hook = h; }
static char g_log_path[1024] = "fault.log";
void fault_set_log_path(const char *path) { snprintf(g_log_path, sizeof g_log_path, "%s", path ? path : "fault.log"); }

// Shared reporter: prints the guest/host diagnostics, appends to fault.log and aborts. Runs on the faulting thread
// (inside the POSIX signal handler, or in the trampoline the Mach handler redirected the thread into).
[[noreturn]] static void fault_report(const char *kind, bool in_guest, uintptr_t fa, void *pc, int w) {
    g_in_fault = 1;
    RtThread *t = thread_current();
    char buf[512];
    int n;
    if (in_guest) {
        uint32_t ga = (uint32_t)(fa - (uintptr_t)guest_base);
        n = snprintf(buf, sizeof buf,
                     "\n=== GUEST ACCESS VIOLATION (%s) ===\n  guest address : 0x%08x (%s)\n  host pc       : %p\n  thread        : tid=%u name=%s teb=%08x stack=[%08x,%08x)\n",
                     kind, ga, w == 1 ? "write" : w == 0 ? "read" : "access", pc,
                     t ? t->tid : 0, t ? t->name.c_str() : "?", t ? t->teb : 0, t ? t->gt.stack_limit : 0, t ? t->gt.stack_base : 0);
    } else {
        n = snprintf(buf, sizeof buf, "\n=== HOST FAULT (%s) at address %p, pc %p, thread tid=%u ===\n", kind, (void *)fa, pc,
                     t ? t->tid : 0);
    }
    // the whole report goes to stderr and to fault.log (on a phone stderr goes nowhere); the host backtrace carries the lifted
    // functions' names (g_XXXXXXXX), i.e. the guest call chain
    int lf = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    auto out = [&](const char *p, size_t len) {
        (void)!write(2, p, len);
        if (lf >= 0) (void)!write(lf, p, len);
    };
    out(buf, (size_t)n);
    Dl_info di;
    if (pc && dladdr(pc, &di) && di.dli_sname) {
        n = snprintf(buf, sizeof buf, "  host symbol   : %s+0x%lx\n", di.dli_sname, (unsigned long)((char *)pc - (char *)di.dli_saddr));
        out(buf, (size_t)n);
    }
    void *frames[64];
    int nf = backtrace(frames, 64);
    out("  host backtrace:\n", 18);
    backtrace_symbols_fd(frames, nf, 2);
    if (lf >= 0) {
        backtrace_symbols_fd(frames, nf, lf);
        close(lf);
    }
    abort();
}

static inline bool addr_in_guest(uintptr_t fa) {
    return guest_base && fa >= (uintptr_t)guest_base && fa < (uintptr_t)guest_base + vm_space_bytes() + 0x10000;
}

#if DISH_FAULT_MACH
// ---------------------------------------------------------------------------------------------------------------
// Apple arm64 (iOS): Mach exception port. Each guest thread registers a per-thread EXC_BAD_ACCESS port (so task-level
// handlers such as a debugger's or LiveContainer's stay in place for everything that is not ours). A dedicated
// handler thread receives mach_exception_raise_state_identity messages (64-bit codes). A guest-memory fault gets
// the faulting thread redirected, by editing its arm64 thread state in the reply, into fault_trampoline running on
// that same thread (valid stack, so the diagnostic backtrace and the test hooks work as on Linux). Any other fault
// is answered with KERN_FAILURE, which makes the kernel fall through to the task/host handlers, i.e. the normal
// crash path. Nothing here uses signals (no SIGSEGV handler is installed on this platform).
// ---------------------------------------------------------------------------------------------------------------
#pragma pack(push, 4)
struct MachExcRequest {   // __Request__mach_exception_raise_state_identity_t (mach_exc.defs, id 2407)
    mach_msg_header_t Head;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t thread, task;
    NDR_record_t NDR;
    exception_type_t exception;
    mach_msg_type_number_t codeCnt;
    int64_t code[2];
    int flavor;
    mach_msg_type_number_t old_stateCnt;
    natural_t old_state[1296];
};
struct MachExcReply {
    mach_msg_header_t Head;
    NDR_record_t NDR;
    kern_return_t RetCode;
    int flavor;
    mach_msg_type_number_t new_stateCnt;
    natural_t new_state[1296];
};
#pragma pack(pop)
// The SDK ships exc.h (32-bit codes) but not mach_exc.h; ours is the same message with int64 code[2] (8 bytes larger).
static_assert(sizeof(MachExcRequest) == sizeof(__Request__exception_raise_state_identity_t) + 8, "mach_exception_raise_state_identity layout");
static_assert(sizeof(MachExcReply) == sizeof(__Reply__exception_raise_state_identity_t), "mach_exception_raise_state_identity reply layout");
static_assert(offsetof(MachExcRequest, code) == offsetof(__Request__exception_raise_state_identity_t, code), "code offset");
constexpr mach_msg_id_t kMachExcRaiseStateIdentity = 2407;

static mach_port_t g_port = MACH_PORT_NULL;
static volatile long g_claimed = 0;
static struct { uintptr_t fa; uintptr_t pc; int w; } g_rec;

extern "C" __attribute__((noreturn, used)) void dish_fault_trampoline() {
    uintptr_t fa = g_rec.fa;
    void *pc = (void *)g_rec.pc;
    int w = g_rec.w;
    bool in_guest = addr_in_guest(fa);
    if (in_guest) {
        uint32_t ga = (uint32_t)(fa - (uintptr_t)guest_base);
        g_last_fault_addr = ga;
        if (g_hook) {
            // The hook may longjmp out (tests). Resuming the faulting instruction is not supported with Mach
            // exceptions; a hook that merely returns true is treated as unhandled.
            if (g_hook(ga, w, nullptr)) {
                const char m[] = "note: fault hook returned true; resuming is unsupported on Mach, aborting\n";
                (void)!write(2, m, sizeof m - 1);
            }
        }
    }
    fault_report("EXC_BAD_ACCESS", in_guest, fa, pc, w);
}

static kern_return_t mach_decide(MachExcRequest *rq, MachExcReply *rp) {
    if (rq->exception != EXC_BAD_ACCESS || rq->codeCnt < 2 || rq->flavor != ARM_THREAD_STATE64 ||
        rq->old_stateCnt < ARM_THREAD_STATE64_COUNT)
        return KERN_FAILURE;
    uintptr_t fa = (uintptr_t)rq->code[1];
    if (!addr_in_guest(fa)) return KERN_FAILURE;
    int w = -1;
    arm_exception_state64_t es;
    mach_msg_type_number_t cnt = ARM_EXCEPTION_STATE64_COUNT;
    if (thread_get_state(rq->thread.name, ARM_EXCEPTION_STATE64, (thread_state_t)&es, &cnt) == KERN_SUCCESS) {
        uint32_t ec = es.__esr >> 26;
        if (ec == 0x24 || ec == 0x25) w = (es.__esr >> 6) & 1;   // data abort: WnR
    }
    if (__sync_lock_test_and_set(&g_claimed, 1)) {
        // another thread is already reporting a fatal fault: park this one
        memcpy(rp->new_state, rq->old_state, sizeof(arm_thread_state64_t));
        arm_thread_state64_t *ts = (arm_thread_state64_t *)rp->new_state;
        __darwin_arm_thread_state64_set_pc_fptr(*ts, (void *)&pause);
        rp->flavor = ARM_THREAD_STATE64;
        rp->new_stateCnt = ARM_THREAD_STATE64_COUNT;
        return KERN_SUCCESS;
    }
    arm_thread_state64_t *ts = (arm_thread_state64_t *)rp->new_state;
    memcpy(ts, rq->old_state, sizeof(arm_thread_state64_t));
    g_rec.fa = fa;
    g_rec.pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(*ts);
    g_rec.w = w;
    uintptr_t sp = (uintptr_t)__darwin_arm_thread_state64_get_sp(*ts);
    sp = (sp - 512) & ~(uintptr_t)15;   // step over any red zone
    __darwin_arm_thread_state64_set_sp(*ts, (void *)sp);
    __darwin_arm_thread_state64_set_lr_fptr(*ts, (void *)g_rec.pc);   // so the backtrace shows the faulting frame
    __darwin_arm_thread_state64_set_pc_fptr(*ts, (void *)&dish_fault_trampoline);
    rp->flavor = ARM_THREAD_STATE64;
    rp->new_stateCnt = ARM_THREAD_STATE64_COUNT;
    return KERN_SUCCESS;
}

static void *mach_handler_thread(void *) {
    pthread_setname_np("dish-exc");
    static MachExcRequest rq;
    static MachExcReply rp;
    for (;;) {
        memset(&rq.Head, 0, sizeof rq.Head);
        rq.Head.msgh_size = sizeof rq;
        rq.Head.msgh_local_port = g_port;
        kern_return_t kr = mach_msg(&rq.Head, MACH_RCV_MSG | MACH_RCV_LARGE, 0, sizeof rq, g_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) {
            if (kr == MACH_RCV_TOO_LARGE) continue;
            break;
        }
        memset(&rp.Head, 0, sizeof rp.Head);
        rp.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(rq.Head.msgh_bits), 0);
        rp.Head.msgh_remote_port = rq.Head.msgh_remote_port;
        rp.Head.msgh_local_port = MACH_PORT_NULL;
        rp.Head.msgh_id = rq.Head.msgh_id + 100;
        rp.NDR = NDR_record;
        rp.flavor = 0;
        rp.new_stateCnt = 0;
        kern_return_t ret = KERN_FAILURE;
        if (rq.Head.msgh_id == kMachExcRaiseStateIdentity) ret = mach_decide(&rq, &rp);
        rp.RetCode = ret;
        rp.Head.msgh_size = ret == KERN_SUCCESS ? sizeof rp - sizeof rp.new_state + rp.new_stateCnt * sizeof(natural_t)
                                                : offsetof(MachExcReply, flavor);
        // The request's port descriptors carry send rights to the thread and task; release them.
        if (rq.Head.msgh_bits & MACH_MSGH_BITS_COMPLEX) {
            mach_port_deallocate(mach_task_self(), rq.thread.name);
            mach_port_deallocate(mach_task_self(), rq.task.name);
        }
        mach_msg(&rp.Head, MACH_SEND_MSG, rp.Head.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    return nullptr;
}

void fault_install() {
    if (g_installed) return;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &g_port) != KERN_SUCCESS) return;
    if (mach_port_insert_right(mach_task_self(), g_port, g_port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) return;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_t th;
    if (pthread_create(&th, &at, mach_handler_thread, nullptr) != 0) return;
    pthread_attr_destroy(&at);
    g_installed = true;
}

void fault_thread_init() {
    static thread_local bool done = false;
    if (done) return;
    if (!g_installed) fault_install();
    if (!g_installed) return;
    done = true;
    mach_port_t self = mach_thread_self();
    thread_set_exception_ports(self, EXC_MASK_BAD_ACCESS, g_port, EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
    mach_port_deallocate(mach_task_self(), self);
}

#else   // POSIX signals (Linux, macOS x86_64)

static struct sigaction g_old_segv, g_old_bus;

static void *uc_pc(void *ucv) {
    ucontext_t *uc = (ucontext_t *)ucv;
#if defined(__linux__) && defined(__x86_64__)
    return (void *)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(__aarch64__)
    return (void *)uc->uc_mcontext.pc;
#elif defined(__APPLE__) && defined(__x86_64__)
    return (void *)uc->uc_mcontext->__ss.__rip;
#else
    (void)uc; return nullptr;
#endif
}

static int uc_is_write(void *ucv) {
#if defined(__linux__) && defined(__x86_64__)
    ucontext_t *uc = (ucontext_t *)ucv;
    return (uc->uc_mcontext.gregs[REG_ERR] & 2) ? 1 : 0;
#else
    (void)ucv; return -1;
#endif
}

static void on_fault(int sig, siginfo_t *si, void *ucv) {
    uintptr_t fa = (uintptr_t)si->si_addr;
    bool in_guest = addr_in_guest(fa);
    if (in_guest) {
        uint32_t ga = (uint32_t)(fa - (uintptr_t)guest_base);
        g_last_fault_addr = ga;
        if (g_hook && !g_in_fault) {
            if (g_hook(ga, uc_is_write(ucv), ucv)) return;
        }
    }
    if (g_in_fault) {   // fault inside the fault handler
        const char m[] = "fatal: nested fault in fault handler\n";
        (void)!write(2, m, sizeof m - 1);
        _exit(139);
    }
    // restore default so the abort in fault_report dies with the right core / signal semantics
    signal(sig, SIG_DFL);
    fault_report(sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", in_guest, fa, uc_pc(ucv), in_guest ? uc_is_write(ucv) : -1);
}

void fault_install() {
    if (g_installed) return;
    g_installed = true;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
}

void fault_thread_init() {
    static thread_local bool done = false;
    if (done) return;
    done = true;
    size_t sz = 128 * 1024;
    void *stk = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stk == MAP_FAILED) return;
    stack_t ss;
    ss.ss_sp = stk;
    ss.ss_size = sz;
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);
}

#endif

}  // namespace dish
