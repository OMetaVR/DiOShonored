#include "dish/mainthread.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace dish {
namespace {
std::mutex g_mu;
std::condition_variable g_cv;
std::deque<std::function<void()>> g_q;
std::thread::id g_main;
bool g_active = false;
void (*g_waker)() = nullptr;
}  // namespace

void mainthread_init() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_main = std::this_thread::get_id();
    g_active = true;
}
void mainthread_shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_active = false;
}
bool mainthread_active() { return g_active; }
bool on_mainthread() { return g_active && std::this_thread::get_id() == g_main; }
void mainthread_set_waker(void (*w)()) { g_waker = w; }

void mainthread_post(std::function<void()> f) {
    if (!g_active || on_mainthread()) { f(); return; }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_q.push_back(std::move(f));
    }
    g_cv.notify_one();
    if (g_waker) g_waker();
}

void mainthread_call(std::function<void()> f) {
    if (!g_active || on_mainthread()) { f(); return; }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    mainthread_post([&] {
        f();
        std::lock_guard<std::mutex> lk(m);
        done = true;
        cv.notify_all();
    });
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return done; });
}

size_t mainthread_pump(int timeout_ms) {
    std::deque<std::function<void()>> local;
    {
        std::unique_lock<std::mutex> lk(g_mu);
        if (g_q.empty() && timeout_ms != 0) {
            if (timeout_ms < 0) g_cv.wait(lk, [] { return !g_q.empty() || !g_active; });
            else g_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] { return !g_q.empty() || !g_active; });
        }
        local.swap(g_q);
    }
    for (auto &f : local) f();
    return local.size();
}
}  // namespace dish
