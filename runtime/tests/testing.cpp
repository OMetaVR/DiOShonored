#include "testing.h"
#include "dish/config.h"
#include "dish/fault.h"
#include "dish/heap.h"
#include "dish/vfs.h"
#include <cstring>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace t {

std::vector<Case> &cases() { static std::vector<Case> c; return c; }
int g_failures = 0;
const char *g_current = "";

int run_all(int argc, char **argv) {
    int ran = 0;
    for (auto &c : cases()) {
        if (argc > 1 && strcmp(argv[1], c.name) != 0) continue;
        g_current = c.name;
        int before = g_failures;
        fprintf(stderr, "[ RUN  ] %s\n", c.name);
        c.fn();
        fprintf(stderr, "[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", c.name);
        ran++;
    }
    if (!ran) { fprintf(stderr, "no tests ran\n"); return 1; }
    fprintf(stderr, "%d test(s), %d failure(s)\n", ran, g_failures);
    return g_failures ? 1 : 0;
}

static std::mutex g_gl_mu;
static std::map<uint32_t, GuestFn> g_fns;
void register_guest_fn(uint32_t addr, GuestFn fn) { std::lock_guard<std::mutex> lk(g_gl_mu); g_fns[addr] = fn; }
void clear_guest_fns() { std::lock_guard<std::mutex> lk(g_gl_mu); g_fns.clear(); }

static std::string g_tmp;
std::string temp_dir() {
    if (g_tmp.empty()) {
        const char *base = getenv("TMPDIR");   // iOS has no writable /tmp; TMPDIR is the app container's tmp/
        std::string tmpl = std::string(base && *base ? base : "/tmp");
        if (tmpl.back() != '/') tmpl += '/';
        tmpl += "dishtestXXXXXX";
        const char *d = mkdtemp(tmpl.data());
        if (!d) { fprintf(stderr, "mkdtemp(%s) failed\n", tmpl.c_str()); exit(2); }
        g_tmp = d;
    }
    return g_tmp;
}

static bool g_ready = false;
void setup_runtime(const char *tag, void (*before_attach)()) {
    if (g_ready) return;
    g_ready = true;
    (void)tag;
    dish::log_init_from_env();
    if (!dish::vm_init()) { fprintf(stderr, "vm_init failed\n"); exit(2); }
    dish::fault_install();
    dish::config().save_dir = temp_dir() + "/save";
    dish::config().game_root = temp_dir() + "/game";
    mkdir(dish::config().game_root.c_str(), 0755);
    dish::vfs_init();
    if (before_attach) before_attach();
    dish::process_init_peb(0x400000, 4);
    if (!dish::thread_attach("test-main", 4u << 20, true)) { fprintf(stderr, "thread_attach failed\n"); exit(2); }
}

uint32_t stack_top() { return dish::thread_current()->cur_sp; }

Call call_addr(uint32_t addr, std::initializer_list<uint32_t> args, uint32_t ecx, uint32_t edx) {
    uint32_t sp = stack_top() & ~15u;
    sp -= 4 * (uint32_t)args.size();
    uint32_t i = 0;
    for (uint32_t a : args) dish::wr32(sp + 4 * i++, a);
    sp -= 4;
    dish::wr32(sp, dish::kReturnSentinel);
    return Call{guest_call(addr, sp, 0, ecx, edx)};
}

uint32_t hle_addr(const char *dll, const char *name) {
    uint32_t a = dish::hle_dynamic_proc(dll, name, 0);
    if (!a) a = dish::thunk_addr(dish::hle_bind(dll, name, 0, -1));
    return a;
}
Call hle(const char *dll, const char *name, std::initializer_list<uint32_t> args) { return call_addr(hle_addr(dll, name), args); }

uint32_t galloc(uint32_t n) { return dish::heap_alloc(dish::heap_process(), dish::HEAP_ZERO_MEMORY, n); }
uint32_t gstr(const std::string &s) { uint32_t p = galloc((uint32_t)s.size() + 1); memcpy(dish::gp(p), s.c_str(), s.size() + 1); return p; }
uint32_t gwstr(const std::u16string &s) { uint32_t p = galloc((uint32_t)(s.size() + 1) * 2); memcpy(dish::gp(p), s.data(), s.size() * 2); return p; }
uint32_t gwstr8(const std::string &u8) { return gwstr(dish::utf8_to_utf16(u8)); }

}  // namespace t

extern "C" GuestFn guest_lookup(uint32_t addr) {
    std::lock_guard<std::mutex> lk(t::g_gl_mu);
    auto it = t::g_fns.find(addr);
    return it == t::g_fns.end() ? nullptr : it->second;
}

#ifndef DISH_TESTING_NO_MAIN   // apps that drive the test helpers themselves (ios/d3d9test) bring their own main
int main(int argc, char **argv) {
    int rc = t::run_all(argc, argv);
    fflush(nullptr);
    _exit(rc);   // like ExitProcess: no static destructors / dlclose while DXVK and driver threads may still be running
}
#endif
