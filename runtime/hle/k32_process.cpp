// kernel32: process and module services, environment, version, system info, exceptions, console, resources.
#include "dish/call.h"
#include "dish/config.h"
#include "dish/heap.h"
#include "dish/hle.h"
#include "dish/loader.h"
#include "dish/sync.h"
#include "dish/thread.h"
#include "dish/thunk.h"
#include "dish/util.h"
#include "dish/vfs.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <unistd.h>

namespace dish {

static void (*g_exit_hook)(int) = nullptr;
void set_exit_hook(void (*hook)(int)) { g_exit_hook = hook; }
void process_exit(int code) {
    LOGI(LC_CORE, "process exit, code %d", code);
    process_mark_done(kprocess(), (uint32_t)code);
    stub_report(true);
    if (g_exit_hook) g_exit_hook(code);
    log_flush();
    fflush(nullptr);
    _exit(code);
}

namespace {

// ---- environment
std::mutex g_env_mu;
std::map<std::string, std::string> g_env;   // key upper-case -> "Name=Value" display; value
std::map<std::string, std::pair<std::string, std::string>> g_envmap;   // upper -> (name, value)
bool g_env_init = false;

void env_init() {
    if (g_env_init) return;
    g_env_init = true;
    const RuntimeConfig &c = config();
    const std::string &u = c.guest_user_dir;
    auto set = [&](const char *n, const std::string &v) { std::string up = n; for (auto &ch : up) ch = (char)toupper((unsigned char)ch); g_envmap[up] = {n, v}; };
    set("ALLUSERSPROFILE", "C:\\ProgramData");
    set("APPDATA", u + "\\AppData\\Roaming");
    set("COMPUTERNAME", "DISHONORED-PC");
    set("ComSpec", "C:\\Windows\\system32\\cmd.exe");
    set("HOMEDRIVE", "C:");
    set("HOMEPATH", "\\Users\\Player");
    set("LOCALAPPDATA", u + "\\AppData\\Local");
    set("NUMBER_OF_PROCESSORS", std::to_string(std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN))));
    set("OS", "Windows_NT");
    set("Path", "C:\\Windows\\system32;C:\\Windows");
    set("PATHEXT", ".COM;.EXE;.BAT;.CMD");
    set("PROCESSOR_ARCHITECTURE", "x86");
    set("PROCESSOR_ARCHITEW6432", "AMD64");
    set("ProgramData", "C:\\ProgramData");
    set("ProgramFiles", "C:\\Program Files (x86)");
    set("ProgramFiles(x86)", "C:\\Program Files (x86)");
    set("SystemDrive", "C:");
    set("SystemRoot", "C:\\Windows");
    set("TEMP", u + "\\AppData\\Local\\Temp");
    set("TMP", u + "\\AppData\\Local\\Temp");
    set("USERNAME", "Player");
    set("USERPROFILE", u);
    set("windir", "C:\\Windows");
}
std::string env_key(const std::string &n) { std::string k = n; for (auto &ch : k) ch = (char)toupper((unsigned char)ch); return k; }

uint32_t get_env_var(const std::string &name, uint32_t buf, uint32_t size, bool wide) {
    std::lock_guard<std::mutex> lk(g_env_mu);
    env_init();
    auto it = g_envmap.find(env_key(name));
    if (it == g_envmap.end()) { set_last_error(203); return 0; }   // ERROR_ENVVAR_NOT_FOUND
    const std::string &v = it->second.second;
    if (wide) {
        std::u16string w = utf8_to_utf16(v);
        if (size <= w.size()) return (uint32_t)w.size() + 1;
        write_wstr(buf, size, w);
        return (uint32_t)w.size();
    }
    if (size <= v.size()) return (uint32_t)v.size() + 1;
    write_astr(buf, size, v);
    return (uint32_t)v.size();
}
uint32_t k32_GetEnvironmentVariableA(uint32_t n, uint32_t buf, uint32_t size) { return get_env_var(read_astr_utf8(n), buf, size, false); }
uint32_t k32_GetEnvironmentVariableW(uint32_t n, uint32_t buf, uint32_t size) { return get_env_var(read_wstr_utf8(n), buf, size, true); }
uint32_t set_env_var(const std::string &n, uint32_t v, bool wide) {
    std::lock_guard<std::mutex> lk(g_env_mu);
    env_init();
    if (!v) g_envmap.erase(env_key(n));
    else g_envmap[env_key(n)] = {n, wide ? read_wstr_utf8(v) : read_astr_utf8(v)};
    return 1;
}
uint32_t k32_SetEnvironmentVariableA(uint32_t n, uint32_t v) { return set_env_var(read_astr_utf8(n), v, false); }
uint32_t k32_SetEnvironmentVariableW(uint32_t n, uint32_t v) { return set_env_var(read_wstr_utf8(n), v, true); }
uint32_t env_block(bool wide) {
    std::lock_guard<std::mutex> lk(g_env_mu);
    env_init();
    std::u16string w;
    for (auto &kv : g_envmap) {
        w += utf8_to_utf16(kv.second.first + "=" + kv.second.second);
        w.push_back(0);
    }
    w.push_back(0);
    if (wide) {
        uint32_t p = heap_alloc(heap_process(), 0, (uint32_t)w.size() * 2);
        memcpy(guest_base + p, w.data(), w.size() * 2);
        return p;
    }
    std::string a(w.size() * 2, 0);
    int64_t n = wide_to_mb(1252, (const uint16_t *)w.data(), w.size(), (uint8_t *)a.data(), a.size(), nullptr, '?');
    uint32_t p = heap_alloc(heap_process(), 0, (uint32_t)n);
    memcpy(guest_base + p, a.data(), (size_t)n);
    return p;
}
uint32_t k32_GetEnvironmentStringsW() { return env_block(true); }
uint32_t k32_GetEnvironmentStrings() { return env_block(false); }
uint32_t k32_FreeEnvironmentStrings(uint32_t p) { return heap_free(heap_process(), 0, p); }

// ---- command line
std::mutex g_cl_mu;
uint32_t g_cl_a = 0, g_cl_w = 0;
std::string command_line() {
    const RuntimeConfig &c = config();
    const LoadedModule *exe = module_main_exe();
    std::string path = exe ? exe->guest_path : c.guest_game_dir + "\\Binaries\\Win32\\Dishonored.exe";
    std::string s = "\"" + path + "\"";
    for (auto &a : c.args) {
        s += " ";
        if (a.find_first_of(" \t\"") != std::string::npos) s += "\"" + a + "\"";
        else s += a;
    }
    return s;
}
uint32_t k32_GetCommandLineA() {
    std::lock_guard<std::mutex> lk(g_cl_mu);
    if (!g_cl_a) {
        std::string s = command_line();
        g_cl_a = heap_alloc(heap_process(), 0, (uint32_t)s.size() + 1);
        write_astr(g_cl_a, s.size() + 1, s);
    }
    return g_cl_a;
}
uint32_t k32_GetCommandLineW() {
    std::lock_guard<std::mutex> lk(g_cl_mu);
    if (!g_cl_w) {
        std::u16string s = utf8_to_utf16(command_line());
        g_cl_w = heap_alloc(heap_process(), 0, (uint32_t)(s.size() + 1) * 2);
        write_wstr(g_cl_w, s.size() + 1, s);
    }
    return g_cl_w;
}
void k32_GetStartupInfoA(uint32_t p) { memset(guest_base + p, 0, 68); wr32(p, 68); wr32(p + 48, 1); }
void k32_GetStartupInfoW(uint32_t p) { memset(guest_base + p, 0, 68); wr32(p, 68); wr32(p + 48, 1); }
uint32_t k32_SetHandleCount(uint32_t n) { return n; }

// ---- version / system
uint32_t k32_GetVersion() { return 0x1DB10106u; }
uint32_t get_version_ex(uint32_t p, bool wide) {
    uint32_t sz = rd32(p);
    uint32_t base = wide ? 276 : 148;
    if (sz != base && sz != base + 8) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    memset(guest_base + p + 4, 0, sz - 4);
    wr32(p + 4, 6);
    wr32(p + 8, 1);
    wr32(p + 12, 7601);
    wr32(p + 16, 2);
    if (wide) write_wstr(p + 20, 128, u"Service Pack 1");
    else write_astr(p + 20, 128, "Service Pack 1");
    if (sz == base + 8) {
        wr16(p + base, 1);          // wServicePackMajor
        wr16(p + base + 2, 0);
        wr16(p + base + 4, 0x100);  // VER_SUITE_SINGLEUSERTS
        wr8(p + base + 6, 1);       // VER_NT_WORKSTATION
    }
    return 1;
}
uint32_t k32_GetVersionExA(uint32_t p) { return get_version_ex(p, false); }
uint32_t k32_GetVersionExW(uint32_t p) { return get_version_ex(p, true); }
uint32_t k32_VerifyVersionInfoW(uint32_t, uint32_t, int64_t) { return 1; }
int64_t k32_VerSetConditionMask(int64_t mask, uint32_t type, uint32_t cond) {
    int bit = type == 1 ? 0 : type == 2 ? 1 : type == 4 ? 2 : type == 8 ? 3 : type == 16 ? 4 : type == 32 ? 5 : type == 64 ? 6 : 7;
    return mask | ((int64_t)(cond & 7) << (bit * 3));
}
uint32_t fill_sysinfo(uint32_t p, uint16_t arch) {
    long n = std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN));
    memset(guest_base + p, 0, 36);
    wr16(p, arch);
    wr32(p + 4, 4096);
    wr32(p + 8, 0x10000);
    wr32(p + 12, vm_user_max() - 1);
    wr32(p + 16, n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1);
    wr32(p + 20, (uint32_t)n);
    wr32(p + 24, 586);
    wr32(p + 28, 65536);
    wr16(p + 32, 6);
    wr16(p + 34, 0x2A07);
    return 0;
}
void k32_GetSystemInfo(uint32_t p) { fill_sysinfo(p, 0); }
void k32_GetNativeSystemInfo(uint32_t p) { fill_sysinfo(p, 9); }
uint32_t k32_IsWow64Process(uint32_t, uint32_t out) { wr32(out, 1); return 1; }
uint32_t k32_IsProcessorFeaturePresent(uint32_t f) {
    switch (f) {
        case 2: case 3: case 6: case 8: case 9: case 10: case 12: case 13: return 1;
        default: return 0;
    }
}
uint32_t k32_GetComputerNameW(uint32_t buf, uint32_t psz) {
    std::u16string n = u"DISHONORED-PC";
    if (rd32(psz) <= n.size()) { wr32(psz, (uint32_t)n.size() + 1); set_last_error(234); return 0; }
    write_wstr(buf, rd32(psz), n);
    wr32(psz, (uint32_t)n.size());
    return 1;
}
uint32_t k32_GetComputerNameA(uint32_t buf, uint32_t psz) {
    std::string n = "DISHONORED-PC";
    if (rd32(psz) <= n.size()) { wr32(psz, (uint32_t)n.size() + 1); set_last_error(234); return 0; }
    write_astr(buf, rd32(psz), n);
    wr32(psz, (uint32_t)n.size());
    return 1;
}
int32_t k32_MulDiv(int32_t a, int32_t b, int32_t c) {
    if (c == 0) return -1;
    int64_t p = (int64_t)a * b;
    int64_t r = (p >= 0 ? p + c / 2 : p - c / 2) / c;
    return (int32_t)r;
}

// ---- modules
uint32_t k32_GetModuleHandleW(uint32_t name) {
    if (!name) { const LoadedModule *e = module_main_exe(); return e ? e->base : 0; }
    std::string n = read_wstr_utf8(name);
    if (const LoadedModule *m = module_by_name(n)) return m->base;
    if (uint32_t h = module_fake_handle(n)) return h;
    set_last_error(126);
    return 0;
}
uint32_t k32_GetModuleHandleA(uint32_t name) {
    if (!name) { const LoadedModule *e = module_main_exe(); return e ? e->base : 0; }
    std::string n = read_astr_utf8(name);
    if (const LoadedModule *m = module_by_name(n)) return m->base;
    if (uint32_t h = module_fake_handle(n)) return h;
    set_last_error(126);
    return 0;
}
uint32_t k32_GetModuleHandleExW(uint32_t flags, uint32_t name, uint32_t out) {
    uint32_t h = 0;
    if (flags & 4) {   // FROM_ADDRESS
        for (const LoadedModule *m : module_list())
            if (!m->fake && name >= m->base && name < m->base + m->size) h = m->base;
        if (!h) { set_last_error(126); return 0; }
    } else {
        h = k32_GetModuleHandleW(name);
        if (!h) return 0;
    }
    if (out) wr32(out, h);
    return 1;
}
uint32_t k32_GetModuleHandleExA(uint32_t flags, uint32_t name, uint32_t out) {
    if (flags & 4) return k32_GetModuleHandleExW(flags, name, out);
    uint32_t h = k32_GetModuleHandleA(name);
    if (!h) return 0;
    if (out) wr32(out, h);
    return 1;
}
const LoadedModule *mod_for_handle(uint32_t h) {
    if (!h) return module_main_exe();
    return module_by_base(h);
}
uint32_t k32_GetModuleFileNameW(uint32_t h, uint32_t buf, uint32_t size) {
    const LoadedModule *m = mod_for_handle(h);
    if (!m) { set_last_error(126); return 0; }
    std::u16string w = utf8_to_utf16(m->guest_path);
    if (size == 0) return 0;
    if (w.size() >= size) {
        memcpy(guest_base + buf, w.data(), (size_t)(size - 1) * 2);
        wr16(buf + (size - 1) * 2, 0);
        set_last_error(122);
        return size;
    }
    write_wstr(buf, size, w);
    return (uint32_t)w.size();
}
uint32_t k32_GetModuleFileNameA(uint32_t h, uint32_t buf, uint32_t size) {
    const LoadedModule *m = mod_for_handle(h);
    if (!m) { set_last_error(126); return 0; }
    const std::string &s = m->guest_path;
    if (size == 0) return 0;
    if (s.size() >= size) {
        memcpy(guest_base + buf, s.data(), size - 1);
        wr8(buf + size - 1, 0);
        set_last_error(122);
        return size;
    }
    write_astr(buf, size, s);
    return (uint32_t)s.size();
}
uint32_t k32_GetProcAddress(uint32_t h, uint32_t name) {
    uint32_t r;
    if (name < 0x10000) r = module_get_proc(h, nullptr, name);
    else r = module_get_proc(h, gs(name), 0);
    if (!r) {
        set_last_error(127);
        LOGD(LC_HLE, "GetProcAddress(%08x, %s) -> NULL", h, name < 0x10000 ? ("#" + std::to_string(name)).c_str() : gs(name));
    }
    return r;
}
uint32_t load_library(const std::string &n) {
    if (const LoadedModule *m = module_by_name(n)) return m->base;
    if (uint32_t b = module_load_on_demand(n)) return b;
    if (uint32_t h = module_fake_handle(n)) return h;
    LOGI(LC_LOADER, "LoadLibrary(%s): not available -> NULL", n.c_str());
    set_last_error(126);
    return 0;
}
uint32_t k32_LoadLibraryA(uint32_t n) { return load_library(read_astr_utf8(n)); }
uint32_t k32_LoadLibraryW(uint32_t n) { return load_library(read_wstr_utf8(n)); }
uint32_t k32_LoadLibraryExA(uint32_t n, uint32_t, uint32_t) { return load_library(read_astr_utf8(n)); }
uint32_t k32_LoadLibraryExW(uint32_t n, uint32_t, uint32_t) { return load_library(read_wstr_utf8(n)); }
uint32_t k32_FreeLibrary(uint32_t) { return 1; }
uint32_t k32_DisableThreadLibraryCalls(uint32_t h) {
    if (LoadedModule *m = module_mutable(h)) m->no_thread_calls = true;
    return 1;
}
uint32_t k32_AddDllDirectory(uint32_t) { return 1; }

// ---- resources
uint32_t res_find_entry(uint32_t base, uint32_t dir_rva, uint32_t root_rva, uint32_t id_or_name, bool is_name_ptr, bool *is_subdir) {
    uint32_t d = base + dir_rva;
    uint16_t nnamed = rd16(d + 12), nid = rd16(d + 14);
    uint32_t ent = d + 16;
    std::u16string want;
    if (is_name_ptr) {
        want = read_wstr(id_or_name);
        for (auto &c : want) c = u16_upper(c);
    }
    for (uint32_t i = 0; i < (uint32_t)nnamed + nid; i++, ent += 8) {
        uint32_t nm = rd32(ent), off = rd32(ent + 4);
        bool match = false;
        if (i < nnamed) {
            if (is_name_ptr && (nm & 0x80000000u)) {
                uint32_t s = base + root_rva + (nm & 0x7FFFFFFF);
                uint16_t len = rd16(s);
                std::u16string have((const char16_t *)(guest_base + s + 2), len);
                for (auto &c : have) c = u16_upper(c);
                match = have == want;
            }
        } else if (!is_name_ptr) match = nm == id_or_name;
        if (match) {
            *is_subdir = (off & 0x80000000u) != 0;
            return off & 0x7FFFFFFF;
        }
    }
    return 0xFFFFFFFFu;
}
uint32_t k32_FindResourceW(uint32_t hmod, uint32_t name, uint32_t type) {
    const LoadedModule *m = mod_for_handle(hmod);
    if (!m || !m->res_rva) { set_last_error(1814); return 0; }   // ERROR_RESOURCE_DATA_NOT_FOUND
    bool sub;
    uint32_t t = res_find_entry(m->base, m->res_rva, m->res_rva, type, type >= 0x10000, &sub);
    if (t == 0xFFFFFFFFu || !sub) { set_last_error(1813); return 0; }
    uint32_t n = res_find_entry(m->base, m->res_rva + t, m->res_rva, name, name >= 0x10000, &sub);
    if (n == 0xFFFFFFFFu || !sub) { set_last_error(1814); return 0; }
    // language level: take the first entry
    uint32_t d = m->base + m->res_rva + n;
    uint32_t cnt = (uint32_t)rd16(d + 12) + rd16(d + 14);
    if (!cnt) { set_last_error(1814); return 0; }
    uint32_t off = rd32(d + 16 + 4);
    return m->base + m->res_rva + (off & 0x7FFFFFFF);
}
uint32_t k32_FindResourceExW(uint32_t hmod, uint32_t type, uint32_t name, uint32_t) { return k32_FindResourceW(hmod, name, type); }
uint32_t k32_LoadResource(uint32_t hmod, uint32_t hres) {
    const LoadedModule *m = mod_for_handle(hmod);
    if (!m || !hres) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    return m->base + rd32(hres);
}
uint32_t k32_LockResource(uint32_t h) { return h; }
uint32_t k32_SizeofResource(uint32_t, uint32_t hres) { return hres ? rd32(hres + 4) : 0; }

// ---- process control, debug, exceptions
void k32_ExitProcess(uint32_t code) { process_exit((int)code); }
uint32_t k32_TerminateProcess(uint32_t h, uint32_t code) {
    if (h == HANDLE_CUR_PROCESS) process_exit((int)code);
    set_last_error(ERR_ACCESS_DENIED);
    return 0;
}
void k32_FatalExit(uint32_t code) { process_exit((int)code); }
uint32_t k32_IsDebuggerPresent() { return 0; }
uint32_t k32_CheckRemoteDebuggerPresent(uint32_t, uint32_t out) { wr32(out, 0); return 1; }
void k32_DebugBreak() { LOGW(LC_EXC, "DebugBreak called by the guest (ignored)"); }
uint32_t k32_SetErrorMode(uint32_t) { return 0; }
uint32_t k32_Beep(uint32_t, uint32_t) { return 1; }
uint32_t g_unhandled_filter = 0;
uint32_t k32_SetUnhandledExceptionFilter(uint32_t f) { uint32_t o = g_unhandled_filter; g_unhandled_filter = f; return o; }
uint32_t k32_UnhandledExceptionFilter(uint32_t ptrs) {
    uint32_t rec = rd32(ptrs);
    LOGE(LC_EXC, "UnhandledExceptionFilter: code %08x at %08x", rd32(rec), rd32(rec + 12));
    return 1;   // EXCEPTION_EXECUTE_HANDLER
}
void output_debug(const std::string &s) {
    std::string t = s;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    if (!t.empty()) LOGI(LC_ENGINE, "%s", t.c_str());
}
void k32_OutputDebugStringA(uint32_t p) { if (p) output_debug(read_astr_utf8(p)); }
void k32_OutputDebugStringW(uint32_t p) { if (p) output_debug(read_wstr_utf8(p)); }

// ExecuteHandler (Wine: EXC_CallHandler): while a handler runs, the TEB head is a dispatcher frame {Prev, Handler, establisher}
// below the dispatcher's records. Code relies on it: MSVC's _UnwindNestedFrames saves the head, calls RtlUnwind and re-links the
// saved frame in front of the target; with the establisher itself at the head that link would point the frame at itself.
uint32_t call_handler_framed(uint32_t sp, uint32_t nested_handler, uint32_t handler, uint32_t rec, uint32_t frame, uint32_t ctx, uint32_t disp) {
    const uint32_t nf = sp - 0x40;
    wr32(nf, rd32(cur_teb() + TEB_EXCEPTION_LIST));
    wr32(nf + 4, nested_handler);
    wr32(nf + 8, frame);
    wr32(cur_teb() + TEB_EXCEPTION_LIST, nf);
    uint32_t r = guest_invoke_from(nf - 0x10, handler, Conv::Cdecl, rec, frame, ctx, disp);
    wr32(cur_teb() + TEB_EXCEPTION_LIST, rd32(nf));   // __wine_pop_frame (Prev may have been re-linked by the handler)
    return r;
}
// The dispatcher frame's handler, reached when an exception is raised inside a handler (Wine: raise_handler / unwind_handler).
uint32_t nested_raise_handler(uint32_t rec, uint32_t frame, uint32_t, uint32_t disp) {
    if (rd32(rec + 4) & 6u) return 1;   // unwinding: ExceptionContinueSearch
    wr32(disp, rd32(frame + 8));
    return 2;                            // ExceptionNestedException
}
uint32_t nested_unwind_handler(uint32_t rec, uint32_t frame, uint32_t, uint32_t disp) {
    if (!(rd32(rec + 4) & 6u)) return 1;
    wr32(disp, rd32(frame + 8));
    return 3;                            // ExceptionCollidedUnwind
}

constexpr uint32_t kMsvcThreadName = 0x406D1388;
void raise_exception(const HleCtx &c, uint32_t code, uint32_t flags, uint32_t nargs, uint32_t args) {
    // MSVC SetThreadName passes THREADNAME_INFO {dwType=0x1000, szName, dwThreadID, dwFlags} itself as the argument array
    if (code == kMsvcThreadName && nargs >= 3 && args) {
        uint32_t info = args;
        if (rd32(info) == 0x1000) {
            std::string name = read_astr(rd32(info + 4));
            uint32_t tid = rd32(info + 8);
            RtThread *t = thread_current();
            if (tid != 0xFFFFFFFFu && tid != 0) {
                t = nullptr;
                for (RtThread *x : thread_list()) if (x->tid == tid) t = x;
            }
            if (t) {
                t->name = name;
                if (t == thread_current()) {
                    char nm[16];
                    snprintf(nm, sizeof nm, "%s", name.c_str());
#ifdef __linux__
                    pthread_setname_np(pthread_self(), nm);
#else
                    pthread_setname_np(nm);
#endif
                }
            }
            LOGD(LC_THREAD, "thread %u named '%s'", t ? t->tid : tid, name.c_str());
            return;
        }
    }
    // Build EXCEPTION_RECORD and CONTEXT below the caller's stack and walk the guest SEH chain.
    uint32_t sp = (c.esp - 0x400) & ~15u;
    uint32_t rec = sp, ctx = sp + 0x80, disp = sp + 0x3A0;
    memset(guest_base + rec, 0, 0x80);
    memset(guest_base + ctx, 0, 0x2CC);
    wr32(rec, code);
    wr32(rec + 4, flags);
    wr32(rec + 12, rd32(c.esp));   // address = return address slot (best effort)
    wr32(rec + 16, std::min<uint32_t>(nargs, 15));
    for (uint32_t i = 0; i < std::min<uint32_t>(nargs, 15) && args; i++) wr32(rec + 20 + 4 * i, rd32(args + 4 * i));
    wr32(ctx, 0x10007);   // CONTEXT_FULL
    LOGW(LC_EXC, "RaiseException(code=%08x flags=%x nargs=%u) from %08x", code, flags, nargs, rd32(c.esp));
    // MSVC C++ throw: args = {0x19930520, object, ThrowInfo*}. Log the thrown type and, for std::exception or a thrown string (UE3
    // throws its error text as a TCHAR*), the message.
    if (code == 0xE06D7363u && nargs >= 3 && args && rd32(args) == 0x19930520u) {
        auto astr_max = [](uint32_t a, size_t max) { size_t n = 0; while (n < max && rd8(a + (uint32_t)n)) n++; return read_astr(a, (int64_t)n); };
        auto wstr_max = [](uint32_t a, size_t max) { size_t n = 0; while (n < max && rd16(a + 2 * (uint32_t)n)) n++; return read_wstr_utf8(a, (int64_t)n); };
        uint32_t obj = rd32(args + 4), ti = rd32(args + 8);
        uint32_t cta = ti ? rd32(ti + 12) : 0;
        uint32_t ct = cta && rd32(cta) ? rd32(cta + 4) : 0;
        uint32_t td = ct ? rd32(ct + 4) : 0;
        std::string type = td ? astr_max(td + 8, 128) : "?";
        std::string what;
        if (obj && type.find("exception") != std::string::npos && rd32(obj + 4)) what = astr_max(rd32(obj + 4), 400);
        if (obj && (type == ".PA_W" || type == ".PB_W") && rd32(obj)) what = wstr_max(rd32(obj), 1024);
        if (obj && (type == ".PAD" || type == ".PBD") && rd32(obj)) what = astr_max(rd32(obj), 1024);
        LOGW(LC_EXC, "  C++ exception %s%s%s", type.c_str(), what.empty() ? "" : ": ", what.c_str());
    }
    // Walk the guest SEH chain like ntdll's RtlDispatchException (Wine: call_stack_handlers), each handler wrapped in a dispatcher frame.
    uint32_t frame = rd32(cur_teb() + TEB_EXCEPTION_LIST), nested_frame = 0;
    const uint32_t nested_handler = thunk_addr(hle_bind("ntdll.dll", "__dish_raise_nested_handler", 0, -1));
    int guard = 0;
    while (frame != 0xFFFFFFFFu && frame != 0 && guard++ < 1024) {
        uint32_t handler = rd32(frame + 4);
        wr32(disp, 0);
        uint32_t r = call_handler_framed(sp, nested_handler, handler, rec, frame, ctx, disp);
        LOGD(LC_EXC, "  handler %08x (frame %08x) -> %u", handler, frame, r);
        if (frame == nested_frame) {   // no longer nested
            nested_frame = 0;
            wr32(rec + 4, rd32(rec + 4) & ~0x10u);
        }
        if (r == 0) {   // ExceptionContinueExecution
            if (flags & 1) { LOGE(LC_EXC, "handler continued a non-continuable exception"); break; }
            return;
        }
        if (r == 2) {   // ExceptionNestedException: an exception raised inside an earlier dispatch's handler
            if (nested_frame < rd32(disp)) nested_frame = rd32(disp);
            wr32(rec + 4, rd32(rec + 4) | 0x10u);   // EXCEPTION_NESTED_CALL
        } else if (r != 1) {
            LOGE(LC_EXC, "handler %08x returned invalid disposition %u", handler, r);
            break;
        }
        frame = rd32(frame);
    }
    uint32_t ptrs = sp + 0x3C0;
    wr32(ptrs, rec);
    wr32(ptrs + 4, ctx);
    if (g_unhandled_filter) {
        uint32_t r = guest_invoke_from(sp - 0x10, g_unhandled_filter, Conv::Stdcall, ptrs);
        LOGE(LC_EXC, "unhandled exception filter returned %u", r);
    }
    LOGE(LC_EXC, "UNHANDLED guest exception %08x at %08x; the runtime cannot resume into lifted catch blocks yet (docs/guest_abi.md section 8). Terminating.", code, rd32(rec + 12));
    process_exit(3);
}
void k32_RaiseException(const HleCtx &c, uint32_t code, uint32_t flags, uint32_t nargs, uint32_t args) { raise_exception(c, code, flags, nargs, args); }
void k32_RtlUnwind(const HleCtx &c, uint32_t target, uint32_t ip, uint32_t record, uint32_t retval) {
    (void)ip; (void)retval;
    uint32_t sp = (c.esp - 0x400) & ~15u;
    uint32_t rec = record, ctx = sp + 0x80;
    memset(guest_base + ctx, 0, 0x2CC);
    if (!rec) {
        rec = sp;
        memset(guest_base + rec, 0, 0x50);
        wr32(rec, 0xC0000027u);
    }
    wr32(rec + 4, rd32(rec + 4) | (target ? 2u : 6u));   // EXCEPTION_UNWINDING (| EXCEPTION_EXIT_UNWIND without a target)
    // Like ntdll's RtlUnwind (Wine): call each handler above the target in a dispatcher frame, popping the frames from the TEB head.
    const uint32_t nested_handler = thunk_addr(hle_bind("ntdll.dll", "__dish_unwind_nested_handler", 0, -1));
    const uint32_t disp = sp + 0x3A0;
    uint32_t frame = rd32(cur_teb() + TEB_EXCEPTION_LIST);
    int guard = 0;
    while (frame != 0xFFFFFFFFu && frame != 0 && frame != target && guard++ < 1024) {
        uint32_t handler = rd32(frame + 4);
        wr32(disp, 0);
        uint32_t r = call_handler_framed(sp, nested_handler, handler, rec, frame, ctx, disp);
        LOGD(LC_EXC, "  unwind handler %08x (frame %08x) -> %u", handler, frame, r);
        if (r == 3) frame = rd32(disp);   // ExceptionCollidedUnwind: continue from the frame whose unwind we interrupted
        else if (r != 1) LOGW(LC_EXC, "unwind handler %08x returned disposition %u", handler, r);
        uint32_t prev = rd32(frame);
        wr32(cur_teb() + TEB_EXCEPTION_LIST, prev);   // __wine_pop_frame
        frame = prev;
    }
}
void k32_RtlCaptureContext(uint32_t ctx) {
    memset(guest_base + ctx, 0, 0x2CC);
    wr32(ctx, 0x10007);
}
uint32_t k32_RtlCaptureStackBackTrace(uint32_t, uint32_t, uint32_t, uint32_t hash) { if (hash) wr32(hash, 0); return 0; }

// ---- console
uint32_t k32_AllocConsole() { return 1; }
uint32_t k32_FreeConsole() { return 1; }
uint32_t k32_GetConsoleWindow() { return 0; }
uint32_t k32_GetConsoleMode(uint32_t, uint32_t) { return 0; }
uint32_t k32_SetConsoleMode(uint32_t, uint32_t) { return 1; }
uint32_t k32_SetConsoleTitleW(uint32_t) { return 1; }
uint32_t k32_SetConsoleCtrlHandler(uint32_t, uint32_t) { return 1; }
uint32_t k32_SetConsoleTextAttribute(uint32_t, uint32_t) { return 1; }
uint32_t k32_SetConsoleScreenBufferSize(uint32_t, uint32_t) { return 1; }
uint32_t k32_GetConsoleScreenBufferInfo(uint32_t, uint32_t) { return 0; }
uint32_t k32_GetNumberOfConsoleInputEvents(uint32_t, uint32_t) { return 0; }
uint32_t k32_ReadConsole(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t k32_PeekConsoleInput(uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t k32_WriteConsoleA(uint32_t, uint32_t buf, uint32_t n, uint32_t pw, uint32_t) {
    (void)!write(1, guest_base + buf, n);
    if (pw) wr32(pw, n);
    return 1;
}
uint32_t k32_WriteConsoleW(uint32_t, uint32_t buf, uint32_t n, uint32_t pw, uint32_t) {
    std::string s = utf16_to_utf8((const uint16_t *)(guest_base + buf), n);
    (void)!write(1, s.data(), s.size());
    if (pw) wr32(pw, n);
    return 1;
}
uint32_t k32_CreateProcessA(uint32_t, uint32_t cmd, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
    LOGW(LC_HLE, "CreateProcessA('%s') refused", cmd ? read_astr(cmd).c_str() : "");
    set_last_error(ERR_ACCESS_DENIED);
    return 0;
}
uint32_t k32_CreateProcessW(uint32_t, uint32_t cmd, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
    LOGW(LC_HLE, "CreateProcessW('%s') refused", cmd ? read_wstr_utf8(cmd).c_str() : "");
    set_last_error(ERR_ACCESS_DENIED);
    return 0;
}
uint32_t k32_FormatMessageW(uint32_t flags, uint32_t, uint32_t id, uint32_t, uint32_t buf, uint32_t size, uint32_t) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, "Error %u.\r\n", id);
    std::u16string w = utf8_to_utf16(tmp);
    if (flags & 0x100) {
        uint32_t p = heap_alloc(heap_process(), 0, (uint32_t)(w.size() + 1) * 2);
        write_wstr(p, w.size() + 1, w);
        wr32(buf, p);
        return (uint32_t)w.size();
    }
    if (size <= w.size()) { set_last_error(122); return 0; }
    write_wstr(buf, size, w);
    return (uint32_t)w.size();
}
uint32_t k32_FormatMessageA(uint32_t flags, uint32_t, uint32_t id, uint32_t, uint32_t buf, uint32_t size, uint32_t) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, "Error %u.\r\n", id);
    std::string s = tmp;
    if (flags & 0x100) {
        uint32_t p = heap_alloc(heap_process(), 0, (uint32_t)s.size() + 1);
        write_astr(p, s.size() + 1, s);
        wr32(buf, p);
        return (uint32_t)s.size();
    }
    if (size <= s.size()) { set_last_error(122); return 0; }
    write_astr(buf, size, s);
    return (uint32_t)s.size();
}

}  // namespace

HLE("kernel32.dll", "GetEnvironmentVariableA", Stdcall, k32_GetEnvironmentVariableA);
HLE("kernel32.dll", "GetEnvironmentVariableW", Stdcall, k32_GetEnvironmentVariableW);
HLE("kernel32.dll", "SetEnvironmentVariableA", Stdcall, k32_SetEnvironmentVariableA);
HLE("kernel32.dll", "SetEnvironmentVariableW", Stdcall, k32_SetEnvironmentVariableW);
HLE("kernel32.dll", "GetEnvironmentStringsW", Stdcall, k32_GetEnvironmentStringsW);
HLE("kernel32.dll", "GetEnvironmentStrings", Stdcall, k32_GetEnvironmentStrings);
HLE("kernel32.dll", "GetEnvironmentStringsA", Stdcall, k32_GetEnvironmentStrings);
HLE("kernel32.dll", "FreeEnvironmentStringsA", Stdcall, k32_FreeEnvironmentStrings);
HLE("kernel32.dll", "FreeEnvironmentStringsW", Stdcall, k32_FreeEnvironmentStrings);
HLE("kernel32.dll", "GetCommandLineA", Stdcall, k32_GetCommandLineA);
HLE("kernel32.dll", "GetCommandLineW", Stdcall, k32_GetCommandLineW);
HLE("kernel32.dll", "GetStartupInfoA", Stdcall, k32_GetStartupInfoA);
HLE("kernel32.dll", "GetStartupInfoW", Stdcall, k32_GetStartupInfoW);
HLE("kernel32.dll", "SetHandleCount", Stdcall, k32_SetHandleCount);
HLE("kernel32.dll", "GetVersion", Stdcall, k32_GetVersion);
HLE("kernel32.dll", "GetVersionExA", Stdcall, k32_GetVersionExA);
HLE("kernel32.dll", "GetVersionExW", Stdcall, k32_GetVersionExW);
HLE("kernel32.dll", "VerifyVersionInfoW", Stdcall, k32_VerifyVersionInfoW);
HLE("kernel32.dll", "VerSetConditionMask", Stdcall, k32_VerSetConditionMask);
HLE("kernel32.dll", "GetSystemInfo", Stdcall, k32_GetSystemInfo);
HLE("kernel32.dll", "GetNativeSystemInfo", Stdcall, k32_GetNativeSystemInfo);
HLE("kernel32.dll", "IsWow64Process", Stdcall, k32_IsWow64Process);
HLE("kernel32.dll", "IsProcessorFeaturePresent", Stdcall, k32_IsProcessorFeaturePresent);
HLE("kernel32.dll", "GetComputerNameW", Stdcall, k32_GetComputerNameW);
HLE("kernel32.dll", "GetComputerNameA", Stdcall, k32_GetComputerNameA);
HLE("kernel32.dll", "MulDiv", Stdcall, k32_MulDiv);
HLE("kernel32.dll", "GetModuleHandleW", Stdcall, k32_GetModuleHandleW);
HLE("kernel32.dll", "GetModuleHandleA", Stdcall, k32_GetModuleHandleA);
HLE("kernel32.dll", "GetModuleHandleExW", Stdcall, k32_GetModuleHandleExW);
HLE("kernel32.dll", "GetModuleHandleExA", Stdcall, k32_GetModuleHandleExA);
HLE("kernel32.dll", "GetModuleFileNameW", Stdcall, k32_GetModuleFileNameW);
HLE("kernel32.dll", "GetModuleFileNameA", Stdcall, k32_GetModuleFileNameA);
HLE("kernel32.dll", "GetProcAddress", Stdcall, k32_GetProcAddress);
HLE("kernel32.dll", "LoadLibraryA", Stdcall, k32_LoadLibraryA);
HLE("kernel32.dll", "LoadLibraryW", Stdcall, k32_LoadLibraryW);
HLE("kernel32.dll", "LoadLibraryExA", Stdcall, k32_LoadLibraryExA);
HLE("kernel32.dll", "LoadLibraryExW", Stdcall, k32_LoadLibraryExW);
HLE("kernel32.dll", "FreeLibrary", Stdcall, k32_FreeLibrary);
HLE("kernel32.dll", "DisableThreadLibraryCalls", Stdcall, k32_DisableThreadLibraryCalls);
HLE("kernel32.dll", "AddDllDirectory", Stdcall, k32_AddDllDirectory);
HLE("kernel32.dll", "FindResourceW", Stdcall, k32_FindResourceW);
HLE("kernel32.dll", "FindResourceExW", Stdcall, k32_FindResourceExW);
HLE("kernel32.dll", "LoadResource", Stdcall, k32_LoadResource);
HLE("kernel32.dll", "LockResource", Stdcall, k32_LockResource);
HLE("kernel32.dll", "SizeofResource", Stdcall, k32_SizeofResource);
HLE("kernel32.dll", "ExitProcess", Stdcall, k32_ExitProcess);
HLE("kernel32.dll", "TerminateProcess", Stdcall, k32_TerminateProcess);
HLE("kernel32.dll", "FatalExit", Stdcall, k32_FatalExit);
HLE("kernel32.dll", "IsDebuggerPresent", Stdcall, k32_IsDebuggerPresent);
HLE("kernel32.dll", "CheckRemoteDebuggerPresent", Stdcall, k32_CheckRemoteDebuggerPresent);
HLE("kernel32.dll", "DebugBreak", Stdcall, k32_DebugBreak);
HLE("kernel32.dll", "SetErrorMode", Stdcall, k32_SetErrorMode);
HLE("kernel32.dll", "Beep", Stdcall, k32_Beep);
HLE("kernel32.dll", "SetUnhandledExceptionFilter", Stdcall, k32_SetUnhandledExceptionFilter);
HLE("kernel32.dll", "UnhandledExceptionFilter", Stdcall, k32_UnhandledExceptionFilter);
HLE("kernel32.dll", "OutputDebugStringA", Stdcall, k32_OutputDebugStringA);
HLE("kernel32.dll", "OutputDebugStringW", Stdcall, k32_OutputDebugStringW);
HLE("kernel32.dll", "RaiseException", Stdcall, k32_RaiseException);
HLE("kernel32.dll", "RtlUnwind", Stdcall, k32_RtlUnwind);
HLE("ntdll.dll", "__dish_raise_nested_handler", Cdecl, nested_raise_handler);
HLE("ntdll.dll", "__dish_unwind_nested_handler", Cdecl, nested_unwind_handler);
HLE("kernel32.dll", "RtlCaptureContext", Stdcall, k32_RtlCaptureContext);
HLE("kernel32.dll", "RtlCaptureStackBackTrace", Stdcall, k32_RtlCaptureStackBackTrace);
HLE("kernel32.dll", "AllocConsole", Stdcall, k32_AllocConsole);
HLE("kernel32.dll", "FreeConsole", Stdcall, k32_FreeConsole);
HLE("kernel32.dll", "GetConsoleWindow", Stdcall, k32_GetConsoleWindow);
HLE("kernel32.dll", "GetConsoleMode", Stdcall, k32_GetConsoleMode);
HLE("kernel32.dll", "SetConsoleMode", Stdcall, k32_SetConsoleMode);
HLE("kernel32.dll", "SetConsoleTitleW", Stdcall, k32_SetConsoleTitleW);
HLE("kernel32.dll", "SetConsoleCtrlHandler", Stdcall, k32_SetConsoleCtrlHandler);
HLE("kernel32.dll", "SetConsoleTextAttribute", Stdcall, k32_SetConsoleTextAttribute);
HLE("kernel32.dll", "SetConsoleScreenBufferSize", Stdcall, k32_SetConsoleScreenBufferSize);
HLE("kernel32.dll", "GetConsoleScreenBufferInfo", Stdcall, k32_GetConsoleScreenBufferInfo);
HLE("kernel32.dll", "GetNumberOfConsoleInputEvents", Stdcall, k32_GetNumberOfConsoleInputEvents);
HLE("kernel32.dll", "ReadConsoleA", Stdcall, k32_ReadConsole);
HLE("kernel32.dll", "ReadConsoleW", Stdcall, k32_ReadConsole);
HLE("kernel32.dll", "PeekConsoleInputA", Stdcall, k32_PeekConsoleInput);
HLE("kernel32.dll", "ReadConsoleInputA", Stdcall, k32_PeekConsoleInput);
HLE("kernel32.dll", "ReadConsoleInputW", Stdcall, k32_PeekConsoleInput);
HLE("kernel32.dll", "WriteConsoleA", Stdcall, k32_WriteConsoleA);
HLE("kernel32.dll", "WriteConsoleW", Stdcall, k32_WriteConsoleW);
HLE("kernel32.dll", "CreateProcessA", Stdcall, k32_CreateProcessA);
HLE("kernel32.dll", "CreateProcessW", Stdcall, k32_CreateProcessW);
HLE("kernel32.dll", "FormatMessageW", Stdcall, k32_FormatMessageW);
HLE("kernel32.dll", "FormatMessageA", Stdcall, k32_FormatMessageA);

}  // namespace dish
