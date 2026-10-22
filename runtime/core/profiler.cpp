// Sampling profiler for whole-process CPU profiles on Linux and on the phone (no debugger or Instruments needed).
//
//   DISH_PROFILE=<start seconds>:<duration seconds>[:<interval ms>]   e.g. DISH_PROFILE=240:30
//
// A sampler thread wakes every interval (default 1 ms) and records the program counter of every thread that is running at that
// moment: Apple suspends each thread with Mach (thread_suspend / thread_get_state) and reads pc and lr; Linux signals it
// (tgkill + SIGPROF handler reading the ucontext). At the end it writes <DISH_PROFILE_OUT or the log directory>/profile.txt:
// per thread its name, the number of running samples and its share of one core, then "module+offset count" lines of the sampled
// pcs (and, on Apple, the callers' lr). tools/analysis/profile_report.py resolves the offsets with the binary's symbol table and
// sums them per function (lifted guest functions are g_XXXXXXXX).
#include "dish/log.h"
#include "dish/profiler.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef __APPLE__
#include <mach/mach.h>
#include <pthread.h>
#else
#include <dirent.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace dish {
namespace {

struct ThreadProf {
    std::string name;
    uint64_t running = 0, seen = 0;
    std::unordered_map<uintptr_t, uint32_t> pcs, lrs;
};

std::string g_out_dir;

#ifndef __APPLE__
std::atomic<uintptr_t> g_sig_pc{0};
std::atomic<int> g_sig_done{0};
void on_sigprof(int, siginfo_t *, void *ctx) {
    auto *uc = static_cast<ucontext_t *>(ctx);
#if defined(__x86_64__)
    g_sig_pc.store((uintptr_t)uc->uc_mcontext.gregs[REG_RIP], std::memory_order_relaxed);
#elif defined(__aarch64__)
    g_sig_pc.store((uintptr_t)uc->uc_mcontext.pc, std::memory_order_relaxed);
#endif
    g_sig_done.store(1, std::memory_order_release);
}
bool read_small(const std::string &path, char *buf, size_t n) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return false;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return r > 0;
}
#endif

void write_report(std::map<uint64_t, ThreadProf> &threads, double seconds, double interval_ms) {
    std::string path = g_out_dir.empty() ? "profile.txt" : g_out_dir + "/profile.txt";
    FILE *f = fopen(path.c_str(), "w");
    if (!f) { LOGW(LC_CORE, "profiler: cannot write %s", path.c_str()); return; }
    fprintf(f, "# dish profile: %.1f s, interval %.2f ms\n", seconds, interval_ms);
    std::vector<ThreadProf *> order;
    for (auto &kv : threads) order.push_back(&kv.second);
    std::sort(order.begin(), order.end(), [](ThreadProf *a, ThreadProf *b) { return a->running > b->running; });
    for (ThreadProf *t : order) {
        if (!t->running) continue;
        fprintf(f, "thread %s running %llu of %llu samples (%.0f%% of a core)\n", t->name.c_str(), (unsigned long long)t->running,
                (unsigned long long)t->seen, 100.0 * t->running / std::max<uint64_t>(1, t->seen));
    }
    Dl_info self{};
    dladdr((void *)&profiler_start, &self);   // the main executable (runtime and lifted code): resolved offline with its symbols
    auto dump = [&](const char *kind, ThreadProf *t, std::unordered_map<uintptr_t, uint32_t> &m) {
        for (auto &kv : m) {
            Dl_info di;
            if (dladdr((void *)kv.first, &di) && di.dli_fbase) {
                if (di.dli_fbase == self.dli_fbase) {
                    fprintf(f, "%s\t%s\texe\t%lx\t%u\n", kind, t->name.c_str(), (unsigned long)(kv.first - (uintptr_t)di.dli_fbase), kv.second);
                    continue;
                }
                const char *mod = di.dli_fname ? strrchr(di.dli_fname, '/') : nullptr;
                fprintf(f, "%s\t%s\t%s:%s\t%lx\t%u\n", kind, t->name.c_str(), mod ? mod + 1 : (di.dli_fname ? di.dli_fname : "?"),
                        di.dli_sname ? di.dli_sname : "?", (unsigned long)(kv.first - (uintptr_t)di.dli_fbase), kv.second);
            } else {
                fprintf(f, "%s\t%s\t?\t%lx\t%u\n", kind, t->name.c_str(), (unsigned long)kv.first, kv.second);
            }
        }
    };
    for (ThreadProf *t : order) {
        if (!t->running) continue;
        dump("pc", t, t->pcs);
        dump("lr", t, t->lrs);
    }
    fclose(f);
    LOGI(LC_CORE, "profiler: wrote %s", path.c_str());
}

void sampler(double start_s, double dur_s, double interval_ms) {
    using Clock = std::chrono::steady_clock;
    std::this_thread::sleep_for(std::chrono::duration<double>(start_s));
    LOGI(LC_CORE, "profiler: sampling for %.0f s every %.2f ms", dur_s, interval_ms);
    std::map<uint64_t, ThreadProf> threads;
    const auto t0 = Clock::now();
    const auto step = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(interval_ms));
    auto next = t0;
#ifdef __APPLE__
    const thread_act_t self = mach_thread_self();
#else
    struct sigaction sa {};
    sa.sa_sigaction = on_sigprof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    const pid_t pid = getpid(), me = (pid_t)syscall(SYS_gettid);
    std::vector<pid_t> tids;
    auto last_enum = Clock::time_point();
#endif
    while (Clock::now() - t0 < std::chrono::duration<double>(dur_s)) {
        next += step;
#ifdef __APPLE__
        thread_act_array_t list = nullptr;
        mach_msg_type_number_t n = 0;
        if (task_threads(mach_task_self(), &list, &n) == KERN_SUCCESS) {
            for (mach_msg_type_number_t i = 0; i < n; i++) {
                thread_act_t th = list[i];
                if (th != self) {
                    thread_identifier_info_data_t idi;
                    mach_msg_type_number_t c = THREAD_IDENTIFIER_INFO_COUNT;
                    uint64_t id = th;
                    if (thread_info(th, THREAD_IDENTIFIER_INFO, (thread_info_t)&idi, &c) == KERN_SUCCESS) id = idi.thread_id;
                    ThreadProf &tp = threads[id];
                    if (tp.name.empty()) {
                        thread_extended_info_data_t ext;
                        c = THREAD_EXTENDED_INFO_COUNT;
                        if (thread_info(th, THREAD_EXTENDED_INFO, (thread_info_t)&ext, &c) == KERN_SUCCESS && ext.pth_name[0]) tp.name = ext.pth_name;
                        else tp.name = "t" + std::to_string(id);
                    }
                    tp.seen++;
                    thread_basic_info_data_t bi;
                    c = THREAD_BASIC_INFO_COUNT;
                    if (thread_info(th, THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS && bi.run_state == TH_STATE_RUNNING &&
                        thread_suspend(th) == KERN_SUCCESS) {
                        arm_thread_state64_t st;
                        c = ARM_THREAD_STATE64_COUNT;
                        if (thread_get_state(th, ARM_THREAD_STATE64, (thread_state_t)&st, &c) == KERN_SUCCESS) {
                            tp.running++;
                            tp.pcs[(uintptr_t)arm_thread_state64_get_pc(st)]++;
                            tp.lrs[(uintptr_t)arm_thread_state64_get_lr(st)]++;
                        }
                        thread_resume(th);
                    }
                }
                mach_port_deallocate(mach_task_self(), th);
            }
            vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(thread_act_t));
        }
#else
        if (Clock::now() - last_enum > std::chrono::seconds(1)) {
            last_enum = Clock::now();
            tids.clear();
            if (DIR *d = opendir("/proc/self/task")) {
                while (dirent *e = readdir(d)) if (e->d_name[0] != '.') { pid_t t = atoi(e->d_name); if (t != me) tids.push_back(t); }
                closedir(d);
            }
        }
        for (pid_t t : tids) {
            char buf[512];
            std::string base = "/proc/self/task/" + std::to_string(t);
            if (!read_small(base + "/stat", buf, sizeof buf)) continue;
            const char *rp = strrchr(buf, ')');
            ThreadProf &tp = threads[(uint64_t)t];
            if (tp.name.empty()) {
                char nm[64];
                tp.name = read_small(base + "/comm", nm, sizeof nm) ? std::string(nm, strcspn(nm, "\n")) : "t" + std::to_string(t);
                tp.name += "/" + std::to_string(t);
            }
            tp.seen++;
            if (!rp || rp[2] != 'R') continue;
            g_sig_done.store(0);
            if (syscall(SYS_tgkill, pid, t, SIGPROF) != 0) continue;
            for (int spin = 0; spin < 20000 && !g_sig_done.load(std::memory_order_acquire); spin++) std::this_thread::yield();
            if (!g_sig_done.load()) continue;
            tp.running++;
            tp.pcs[g_sig_pc.load()]++;
        }
#endif
        std::this_thread::sleep_until(next);
    }
    write_report(threads, std::chrono::duration<double>(Clock::now() - t0).count(), interval_ms);
}

}  // namespace

void profiler_start(const std::string &out_dir) {
    const char *spec = getenv("DISH_PROFILE");
    if (!spec || !*spec) return;
    double start = 0, dur = 30, iv = 1.0;
    sscanf(spec, "%lf:%lf:%lf", &start, &dur, &iv);
    if (iv < 0.1) iv = 0.1;
    g_out_dir = getenv("DISH_PROFILE_OUT") ? getenv("DISH_PROFILE_OUT") : out_dir;
    std::thread([=] {
#ifdef __APPLE__
        pthread_setname_np("dish-profiler");
#else
        pthread_setname_np(pthread_self(), "dish-profiler");
#endif
        sampler(start, dur, iv);
    }).detach();
}

}  // namespace dish
