// Stall watchdog: when the game makes no visible progress (no Present, no file opened) for a while, it records the host call
// stack of every guest thread (lifted functions are named g_<guest address>, so this is the guest call chain) to a file.
// Meant for devices without a debugger. Apple only (Mach thread sampling); a no-op elsewhere (use gdb).
#pragma once
namespace dish {
void watchdog_start(const char *log_path, int stall_seconds = 20);
void watchdog_note_progress();
void watchdog_set_paused(bool paused);   // the app is in the background: not a stall
}  // namespace dish
