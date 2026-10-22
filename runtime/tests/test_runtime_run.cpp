// End-to-end: runtime_run() on a fake one-module program. Runs in a forked child because ExitProcess terminates the process.
#include "testing.h"
#include "pe_builder.h"
#include "dish/config.h"
#include "dish/sha256.h"
#include <sys/wait.h>
#include <unistd.h>

using namespace dish;

constexpr uint32_t EXE_BASE = 0x00400000;

static GuestRet exe_entry(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    // mainCRTStartup-like: OutputDebugStringA("Log: hello"), RaiseException thread naming is skipped, ExitProcess(7)
    uint32_t sp = esp - 16;
    wr32(sp, kReturnSentinel);
    wr32(sp + 4, EXE_BASE + 0x2200);
    guest_call(rd32(EXE_BASE + 0x2100), sp, 0, 0, 0);
    wr32(sp, kReturnSentinel);
    wr32(sp + 4, 7);
    guest_call(rd32(EXE_BASE + 0x2104), sp, 0, 0, 0);
    return guest_ret(99, 0, 0);   // unreachable
}

TEST(runtime_run_executes_program_and_exit_code_propagates) {
    std::string root = t::temp_dir();
    config().game_root = root + "/game";
    config().save_dir = root + "/save";
    std::vector<uint8_t> text(0x200, 0x90), data(0x400, 0);
    memcpy(&data[0x200], "Log: hello from guest\0", 22);
    auto exe = build_pe(EXE_BASE, 0x1000, false, {{".text", 0x1000, 0x60000020, text, 0x200}, {".data", 0x2000, 0xC0000040, data, 0x400}}, 0x3000, {});
    write_file(config().game_root + "/Binaries/Win32/Dishonored.exe", exe);
    static GuestModuleDesc mod;
    mod = {"Dishonored.exe", "Binaries/Win32/Dishonored.exe", {0}, EXE_BASE, EXE_BASE, 0x3000, EXE_BASE + 0x1000, 0};
    sha256(exe.data(), exe.size(), mod.sha256);
    static GuestImportDesc imps[2] = {{"KERNEL32.dll", "OutputDebugStringA", 0}, {"kernel32.dll", "ExitProcess", 0}};   // header says lower-case; tolerate upper
    static GuestIatEntry iat[2] = {{EXE_BASE + 0x2100, GUEST_IAT_IMPORT, 0}, {EXE_BASE + 0x2104, GUEST_IAT_IMPORT, 1}};
    static GuestProgram prog = {GUEST_ABI_VERSION, &mod, 1, imps, 2, iat, 2};
    t::register_guest_fn(EXE_BASE + 0x1000, exe_entry);
    std::string logf = root + "/run.log";
    pid_t pid = fork();
    if (pid == 0) {
        log_set_file(logf.c_str());
        int rc = runtime_run(prog);
        _exit(100 + (rc & 0x3F));   // only reached if ExitProcess did not terminate
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st));
    CHECK_EQ(WEXITSTATUS(st), 7);
    std::ifstream f(logf);
    std::string log((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(log.find("Log: hello from guest") != std::string::npos);
    CHECK(log.find("process exit, code 7") != std::string::npos);
    CHECK(log.find("calling exe entry") != std::string::npos);
}
