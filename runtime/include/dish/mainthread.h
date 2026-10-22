// Main-thread service queue. On iOS (and for SDL video on most platforms) windows and event pumping must live on the process
// main thread, while guest code runs on its own pthread. Guest-side HLE (CreateWindow, message loop, ...) posts work here.
#pragma once
#include <functional>

namespace dish {
void mainthread_init();                              // call from the real main thread before starting the guest
void mainthread_shutdown();
bool mainthread_active();
bool on_mainthread();
void mainthread_post(std::function<void()> f);       // fire and forget
void mainthread_call(std::function<void()> f);       // run on the main thread and wait (inline when not active / already there)
size_t mainthread_pump(int timeout_ms);              // main thread only: run queued tasks; returns how many ran
void mainthread_set_waker(void (*waker)());          // called after a post so a blocking event wait can be interrupted
}  // namespace dish
