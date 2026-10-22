// App lifecycle (iOS background/foreground). iOS forbids submitting GPU work from the background and suspends the process
// shortly after, so on WILL_ENTER_BACKGROUND the host pauses the guest at a safe point and drains the GPU first.
// The safe point is the d3d9 Present (the render thread parks there); other guest threads stall behind it.
#pragma once
#include <cstdint>

namespace dish {

// Main thread (SDL event watch). Marks the runtime paused and waits up to wait_ms for the render thread to finish its
// frame, drain the GPU and park. Returns true when a guest thread parked (or none ever presented, nothing to park).
bool lifecycle_pause(int wait_ms);
void lifecycle_resume();
bool lifecycle_paused();
bool lifecycle_parked();     // some guest thread is currently parked

// Guest render thread, after a Present: if paused, call drain(ctx) (waits for the GPU to go idle), then block until resumed.
// Returns immediately when not paused.
void lifecycle_park(void (*drain)(void *), void *ctx);

// Hooks other subsystems register to be told about pause/resume (audio devices ...). Called on the thread that calls
// lifecycle_pause / lifecycle_resume.
using LifecycleHook = void (*)(bool paused);
void lifecycle_add_hook(LifecycleHook h);

}  // namespace dish
