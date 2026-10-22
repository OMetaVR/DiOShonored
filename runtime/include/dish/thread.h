// Guest threads: host pthreads (>= 16 MiB host stack), guest stack + TEB in guest memory, TLS, FLS.
#pragma once
#include <csetjmp>
#include <cstdint>
#include <string>
#include <vector>
#include <pthread.h>
#include "guest_abi.h"
#include "dish/sync.h"

namespace dish {

// TEB layout (offsets used by lifted code and the runtime)
enum : uint32_t {
    TEB_EXCEPTION_LIST = 0x00, TEB_STACK_BASE = 0x04, TEB_STACK_LIMIT = 0x08, TEB_SELF = 0x18, TEB_PID = 0x20,
    TEB_TID = 0x24, TEB_PEB = 0x30, TEB_TLS_ARRAY = 0x2C, TEB_LAST_ERROR = 0x34, TEB_TLS_SLOTS = 0xE10,
    TEB_TLS_EXPANSION = 0xF94, TEB_SIZE = 0x1000,
};
constexpr uint32_t kTlsMinSlots = 64;
constexpr uint32_t kTlsMaxSlots = 1088;

constexpr size_t kHostStackSize = 16u << 20;      // minimum host stack for any thread that runs guest code
constexpr size_t kMainHostStackSize = 64u << 20;

struct RtThread {
    double st1_arg = 0;          // MUST be the first member: interim x87 st(1) argument slot, see docs/abi_change_requests.md
    GuestThread gt{};
    uint32_t tid = 0;
    uint32_t teb = 0;
    uint32_t stack_alloc = 0, stack_alloc_size = 0;
    uint32_t cur_sp = 0;
    uint32_t start_addr = 0, start_param = 0;
    KThread *kthread = nullptr;  // owning reference while the thread runs
    pthread_t pt{};
    bool has_pt = false;
    pthread_t host_self{};       // the host thread running this guest thread (set when it binds; the stall watchdog samples it)
    bool foreign = false;
    bool suspended_start = false;
    jmp_buf exit_jmp;
    bool exit_jmp_valid = false;
    uint32_t exit_code = 0;
    std::vector<uint32_t> fls;                 // FLS slot values
    std::string name;
    void *altstack = nullptr;
};

// Calls fn for every live guest thread with the thread list locked (fn must not create or end threads).
void thread_for_each(void (*fn)(RtThread *t, void *user), void *user);
KThread *thread_kthread(RtThread *t);       // the thread's kernel object (created on demand for main/foreign threads)
RtThread *thread_current();                 // nullptr on a host thread that never attached
inline uint32_t cur_teb() { return guest_thread->teb; }
inline uint32_t cur_tid() { return guest_thread->host_tid; }

bool process_init_peb(uint32_t image_base, uint32_t n_cpus);   // allocates the PEB; called by the runtime init
uint32_t process_peb();
uint32_t process_id();

// Turn the calling host thread into a guest thread (allocates TEB + stack). `stack_size` 0 = 1 MiB.
RtThread *thread_attach(const char *name, uint32_t guest_stack_size, bool foreign);
void thread_detach(RtThread *t);            // frees TEB/stack, runs FLS callbacks
// Create a new guest thread running `start(param)` (stdcall). Returns the thread handle (0 on failure).
uint32_t thread_create(uint32_t start, uint32_t param, uint32_t stack_size, bool suspended, uint32_t *tid_out);
uint32_t thread_resume(KThread *kt);
uint32_t thread_suspend(KThread *kt);
[[noreturn]] void thread_exit_current(uint32_t code);
// Run `fn` on a fresh host thread with a 64 MiB stack after attaching as the guest main thread; join it.
int thread_run_main(int (*fn)(void *), void *arg, uint32_t guest_stack_size);

// Win32-style last error through the TEB
inline void set_last_error(uint32_t e) { *(uint32_t *)(guest_base + guest_thread->teb + TEB_LAST_ERROR) = e; }
inline uint32_t get_last_error() { return *(uint32_t *)(guest_base + guest_thread->teb + TEB_LAST_ERROR); }

// TLS (TlsAlloc & friends)
uint32_t tls_alloc();                       // 0xFFFFFFFF on failure
bool tls_free(uint32_t idx);
uint32_t tls_get(uint32_t idx);
bool tls_set(uint32_t idx, uint32_t v);

// PE static TLS: registered by the loader per module with a TLS directory.
struct PeTls {
    uint32_t start_raw, end_raw, zero_fill, index_addr, callbacks_addr;
    uint32_t index = 0;
};
void pe_tls_register(const PeTls &t);
void pe_tls_init_thread(RtThread *t);       // allocates this thread's TLS blocks and the fs:[0x2C] array

// FLS
uint32_t fls_alloc(uint32_t callback);
bool fls_free(uint32_t idx);
uint32_t fls_get(uint32_t idx, bool *ok);
bool fls_set(uint32_t idx, uint32_t v);

std::vector<RtThread *> thread_list();      // snapshot
uint32_t thread_count();

// Hooks used by the loader for DllMain(THREAD_ATTACH/DETACH)
extern void (*g_thread_notify)(uint32_t reason);

}  // namespace dish
