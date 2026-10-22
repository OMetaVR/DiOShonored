// PE loader and module registry.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "guest_abi.h"

namespace dish {

struct ExportEntry { std::string name; uint32_t ordinal; uint32_t rva; };

struct LoadedModule {
    std::string name;        // "Dishonored.exe" (as in GuestModuleDesc) or "kernel32.dll" for fakes
    std::string lname;       // lower-case
    std::string guest_path;  // C:\... path reported by GetModuleFileName
    uint32_t base = 0, size = 0, entry = 0, preferred = 0;
    bool is_dll = false;
    bool fake = false;       // system DLL stand-in (HLE); not recompiled
    bool attached = false;   // DllMain(PROCESS_ATTACH) completed
    bool hidden = false;     // recompiled DLL that nothing imports statically: mapped, but invisible until the first LoadLibrary
    bool no_thread_calls = false;
    uint32_t stack_reserve = 0;
    uint32_t ordinal_base = 0;
    std::vector<ExportEntry> exports;
    std::vector<uint32_t> tls_callbacks;
    uint32_t export_dir_rva = 0, export_dir_size = 0;
    uint32_t res_rva = 0, res_size = 0;   // resource directory
};

// Verify + map + relocate + write IAT + register TLS for every module in `prog`. Returns false and fills *err on failure.
// Needs the VM, the thunk table (hle bound) and a vfs. Does not run any guest code.
bool loader_map_all(const GuestProgram &prog, std::string *err);
// Run PE TLS callbacks, DLL entries (PROCESS_ATTACH) in array order, then the exe entry. Must run on a guest thread.
// Returns the exe entry's return value (process exit code); -1 if no exe or a DllMain failed (see log).
int loader_run_entries(const GuestProgram &prog);
// DllMain(THREAD_ATTACH / THREAD_DETACH) + TLS callbacks, called by the thread machinery (g_thread_notify).
void loader_thread_notify(uint32_t reason);
void loader_reset();   // tests

const LoadedModule *module_by_name(const std::string &name);     // "kernel32", "kernel32.dll", with or without path (loaded modules only)
// LoadLibrary of a hidden recompiled DLL: loads its hidden dependencies, runs TLS callbacks and DllMain(PROCESS_ATTACH) on the calling
// guest thread, then makes it visible. Returns its base, or 0 when `name` is not a hidden module or DllMain failed.
uint32_t module_load_on_demand(const std::string &name);
const LoadedModule *module_by_base(uint32_t base);
const LoadedModule *module_main_exe();
std::vector<const LoadedModule *> module_list();
LoadedModule *module_mutable(uint32_t base);
// A handle for a system DLL name (creates a fake header page on first use); 0 if the name is not a DLL we simulate.
uint32_t module_fake_handle(const std::string &name);
// GetProcAddress semantics. `name` null = by ordinal. Returns a guest address (a lifted function or a thunk) or 0.
uint32_t module_get_proc(uint32_t hmod, const char *name, uint32_t ordinal);
bool is_simulated_system_dll(const std::string &lname);
// ExitProcess: report stubs, flush logs, terminate. Tests can intercept with set_exit_hook (the hook may longjmp).
[[noreturn]] void process_exit(int code);
void set_exit_hook(void (*hook)(int code));

// One-shot process initialisation: vm, fault handler, vfs, PEB, thunk binding of guest_program.imports.
bool runtime_init(const GuestProgram &prog);
// Convenience used by the host exe: runtime_init + main guest thread + loader. Returns the process exit code.
int runtime_run(const GuestProgram &prog);

}  // namespace dish
