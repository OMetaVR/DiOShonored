#include "dish/watchdog.h"
#include "dish/log.h"
#include "dish/thread.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#define DISH_WATCHDOG 1
#endif

namespace dish {
namespace {
std::atomic<int64_t> g_last_progress_ms{0};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_started{false};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#if DISH_WATCHDOG
constexpr int kMaxThreads = 96, kMaxFrames = 48;
struct Sample {
    uint32_t tid;
    char name[32];
    int n;
    uintptr_t pc[kMaxFrames];
};
Sample g_samples[kMaxThreads];
int g_nsamples;

// Runs with the thread list locked and the target suspended: no allocation, no locks, only reads of the target's stack.
void sample_thread(RtThread *t, void *) {
    if (g_nsamples >= kMaxThreads || !t->host_self || pthread_equal(t->host_self, pthread_self())) return;
    mach_port_t port = pthread_mach_thread_np(t->host_self);
    if (port == MACH_PORT_NULL) return;
    void *stack_hi = pthread_get_stackaddr_np(t->host_self);
    size_t stack_sz = pthread_get_stacksize_np(t->host_self);
    uintptr_t hi = (uintptr_t)stack_hi, lo = hi - stack_sz;
    if (::thread_suspend(port) != KERN_SUCCESS) return;
    Sample &s = g_samples[g_nsamples];
    s.tid = t->tid;
    snprintf(s.name, sizeof s.name, "%s", t->name.c_str());
    s.n = 0;
    arm_thread_state64_t st;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (::thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
        uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(st), fp = (uintptr_t)arm_thread_state64_get_fp(st);
        s.pc[s.n++] = pc;
        s.pc[s.n++] = (uintptr_t)arm_thread_state64_get_lr(st);
        // arm64 frame records: [fp] = caller's fp, [fp + 8] = return address
        while (s.n < kMaxFrames && fp >= lo && fp + 16 <= hi && (fp & 15) == 0) {
            uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
            if (!ret) break;
            s.pc[s.n++] = ret;
            if (next <= fp) break;
            fp = next;
        }
    }
    ::thread_resume(port);
    g_nsamples++;
}

void dump(const char *path) {
    g_nsamples = 0;
    thread_for_each(sample_thread, nullptr);
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "\n=== STALL: no Present and no file opened for a while; guest thread stacks (host frames; g_XXXXXXXX = guest function) ===\n");
    for (int i = 0; i < g_nsamples; i++) {
        const Sample &s = g_samples[i];
        fprintf(f, "thread tid=%u %s\n", s.tid, s.name);
        for (int k = 0; k < s.n; k++) {
            Dl_info di;
            if (dladdr((void *)s.pc[k], &di) && di.dli_sname)
                fprintf(f, "  #%-2d %s+0x%lx\n", k, di.dli_sname, (unsigned long)(s.pc[k] - (uintptr_t)di.dli_saddr));
            else
                fprintf(f, "  #%-2d %p\n", k, (void *)s.pc[k]);
        }
    }
    fclose(f);
    LOGW(LC_CORE, "watchdog: stall detected, %d guest thread stacks written to %s", g_nsamples, path);
    log_flush();
}
#endif
}  // namespace

void watchdog_note_progress() { g_last_progress_ms.store(now_ms(), std::memory_order_relaxed); }
void watchdog_set_paused(bool p) {
    g_paused = p;
    watchdog_note_progress();
}

void watchdog_start(const char *log_path, int stall_seconds) {
#if DISH_WATCHDOG
    if (g_started.exchange(true)) return;
    watchdog_note_progress();
    std::string path = log_path;
    std::thread([path, stall_seconds] {
        bool dumped = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            int64_t idle = now_ms() - g_last_progress_ms.load(std::memory_order_relaxed);
            if (g_paused || idle < (int64_t)stall_seconds * 1000) { dumped = false; continue; }
            if (!dumped) { dump(path.c_str()); dumped = true; }   // once per stall; progress re-arms it
        }
    }).detach();
#else
    (void)log_path;
    (void)stall_seconds;
#endif
}

}  // namespace dish
