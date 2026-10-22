// Faults inside guest memory -> guest access violation diagnostics (dump + abort). POSIX: SIGSEGV/SIGBUS handler.
// Apple arm64 (iOS): per-thread Mach EXC_BAD_ACCESS port + handler thread (see fault.cpp).
#pragma once
#include <cstdint>
#include <csignal>

namespace dish {

// Install process-wide handlers (idempotent).
void fault_install();
// Give the calling thread a sigaltstack so a host stack overflow can be reported.
void fault_thread_init();
// Optional hook, called first with the guest address and access type (-1 unknown, 0 read, 1 write).
// Return true if the fault was handled (the handler returns and the instruction is retried) or the hook
// longjmp'ed itself (tests). Return false for the default dump + abort.
using FaultHook = bool (*)(uint32_t guest_addr, int is_write, void *ucontext);
void fault_set_hook(FaultHook h);
// Where the diagnostic dump is appended (default "fault.log" in the working directory; read-only on iOS, so the host sets it).
void fault_set_log_path(const char *path);
// Written by the dump: the last fault, for tests that longjmp out of the hook.
extern uint32_t g_last_fault_addr;

}  // namespace dish
