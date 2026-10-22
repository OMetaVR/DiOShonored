#include "dish/lifecycle.h"
#include "dish/watchdog.h"
#include "dish/log.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace dish {
namespace {
std::mutex g_mu;
std::condition_variable g_cv_parked, g_cv_resume;
bool g_paused = false;
int g_parked = 0;
bool g_ever_presented = false;
std::vector<LifecycleHook> g_hooks;
}  // namespace

void lifecycle_add_hook(LifecycleHook h) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_hooks.push_back(h);
}

bool lifecycle_pause(int wait_ms) {
    std::vector<LifecycleHook> hooks;
    {
        std::unique_lock<std::mutex> lk(g_mu);
        if (g_paused) return true;
        g_paused = true;
        watchdog_set_paused(true);
        hooks = g_hooks;
    }
    for (auto h : hooks) h(true);
    std::unique_lock<std::mutex> lk(g_mu);
    if (!g_ever_presented) { LOGI(LC_CORE, "lifecycle: paused (guest has not presented yet)"); return true; }
    bool ok = g_cv_parked.wait_for(lk, std::chrono::milliseconds(wait_ms), [] { return g_parked > 0; });
    LOGI(LC_CORE, "lifecycle: paused, render thread %s", ok ? "parked" : "did not park in time");
    return ok;
}

void lifecycle_resume() {
    std::vector<LifecycleHook> hooks;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_paused) return;
        g_paused = false;
        watchdog_set_paused(false);
        hooks = g_hooks;
    }
    g_cv_resume.notify_all();
    for (auto h : hooks) h(false);
    LOGI(LC_CORE, "lifecycle: resumed");
}

bool lifecycle_paused() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_paused;
}
bool lifecycle_parked() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_parked > 0;
}

void lifecycle_park(void (*drain)(void *), void *ctx) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_ever_presented = true;
        if (!g_paused) return;
    }
    if (drain) drain(ctx);
    std::unique_lock<std::mutex> lk(g_mu);
    g_parked++;
    g_cv_parked.notify_all();
    g_cv_resume.wait(lk, [] { return !g_paused; });
    g_parked--;
}

}  // namespace dish
