// Minimal test harness + helpers to drive the runtime like lifted code would (fake "lifted" modules following the ABI contract).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include "guest_abi.h"
#include "dish/call.h"
#include "dish/hle.h"
#include "dish/loader.h"
#include "dish/log.h"
#include "dish/thread.h"
#include "dish/thunk.h"
#include "dish/util.h"
#include "dish/vm.h"

namespace t {

struct Case { const char *name; void (*fn)(); };
std::vector<Case> &cases();
struct Reg { Reg(const char *n, void (*f)()) { cases().push_back({n, f}); } };
extern int g_failures;
extern const char *g_current;
int run_all(int argc, char **argv);

#define TEST(name) \
    static void test_##name(); \
    static ::t::Reg reg_##name(#name, test_##name); \
    static void test_##name()
#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #cond, ::t::g_current); ::t::g_failures++; } } while (0)
#define CHECK_EQ(a, ...) \
    do { auto _a = (a); auto _b = (__VA_ARGS__); if (!(_a == _b)) { fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld) [%s]\n", __FILE__, __LINE__, #a, #__VA_ARGS__, (long long)_a, (long long)_b, ::t::g_current); ::t::g_failures++; } } while (0)

// ---- fake lifted modules
void register_guest_fn(uint32_t addr, GuestFn fn);
void clear_guest_fns();

// Bring up VM + PEB + a temp save dir + attach the calling thread as a guest thread. Idempotent per process.
void setup_runtime(const char *tag, void (*before_attach)() = nullptr);
std::string temp_dir();   // a fresh empty directory for this process

// Call helpers (use the attached thread's guest stack)
uint32_t stack_top();
struct Call {
    GuestRet r;
    uint32_t eax() const { return guest_ret_eax(r); }
    uint32_t pop() const { return guest_ret_pop(r); }
    uint32_t edx() const { return guest_ret_edx(r); }
};
Call call_addr(uint32_t addr, std::initializer_list<uint32_t> args, uint32_t ecx = 0, uint32_t edx = 0);
// Bind a system-DLL function through the same path as GetProcAddress, then call it with stdcall/cdecl args on the guest stack.
uint32_t hle_addr(const char *dll, const char *name);
Call hle(const char *dll, const char *name, std::initializer_list<uint32_t> args);
// Guest memory scratch (heap-backed). Strings are placed in guest memory.
uint32_t galloc(uint32_t n);
uint32_t gstr(const std::string &s);
uint32_t gwstr(const std::u16string &s);
uint32_t gwstr8(const std::string &utf8);

}  // namespace t
