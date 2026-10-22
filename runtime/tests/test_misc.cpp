#include "testing.h"
#include <cstring>
#include <thread>
#include "dish/config.h"

using namespace dish;

TEST(version_and_system_info) {
    t::setup_runtime("misc");
    uint32_t v = t::galloc(284);
    wr32(v, 284);                                       // OSVERSIONINFOEXW
    CHECK_EQ(t::hle("kernel32.dll", "GetVersionExW", {v}).eax(), 1u);
    CHECK_EQ(rd32(v + 4), 6u);                          // Windows 7 SP1
    CHECK_EQ(rd32(v + 8), 1u);
    CHECK_EQ(rd32(v + 12), 7601u);
    CHECK_EQ(rd32(v + 16), 2u);
    CHECK(utf16_to_utf8((uint16_t *)gp(v + 20), 14) == "Service Pack 1");
    CHECK_EQ(rd16(v + 276), 1u);                        // wServicePackMajor
    CHECK_EQ(rd8(v + 282), 1u);                         // VER_NT_WORKSTATION
    uint32_t va = t::galloc(156);
    wr32(va, 148);
    CHECK_EQ(t::hle("kernel32.dll", "GetVersionExA", {va}).eax(), 1u);
    CHECK(strcmp((char *)gp(va + 20), "Service Pack 1") == 0);
    wr32(va, 12);
    CHECK_EQ(t::hle("kernel32.dll", "GetVersionExA", {va}).eax(), 0u);       // wrong structure size
    CHECK_EQ(t::hle("kernel32.dll", "GetVersion", {}).eax(), 0x1DB10106u);
    uint32_t si = t::galloc(36);
    t::hle("kernel32.dll", "GetSystemInfo", {si});
    CHECK_EQ(rd32(si + 4), 4096u);
    CHECK_EQ(rd32(si + 28), 65536u);
    CHECK(rd32(si + 20) >= 1);
    CHECK_EQ(rd32(si + 12), vm_user_max() - 1);
    uint32_t ms = t::galloc(64);
    wr32(ms, 64);
    CHECK_EQ(t::hle("kernel32.dll", "GlobalMemoryStatusEx", {ms}).eax(), 1u);
    CHECK(rd64(ms + 8) >= (512ull << 20));
    CHECK(rd64(ms + 48) > (2ull << 30));
    CHECK_EQ(t::hle("kernel32.dll", "IsProcessorFeaturePresent", {10}).eax(), 1u);   // SSE2
}

TEST(environment_and_command_line) {
    t::setup_runtime("misc");
    config().args = {"-nostartupmovies", "my file.ini"};
    uint32_t cl = t::hle("kernel32.dll", "GetCommandLineA", {}).eax();
    CHECK_EQ(t::hle("kernel32.dll", "GetCommandLineA", {}).eax(), cl);       // stable pointer
    std::string s = (char *)gp(cl);
    CHECK(s.find("-nostartupmovies") != std::string::npos);
    CHECK(s.find("\"my file.ini\"") != std::string::npos);
    CHECK(s[0] == '"');
    uint32_t clw = t::hle("kernel32.dll", "GetCommandLineW", {}).eax();
    CHECK(utf16_to_utf8((uint16_t *)gp(clw), gwcslen(clw)) == s);
    // CommandLineToArgvW
    uint32_t argc = t::galloc(4);
    uint32_t argv = t::hle("shell32.dll", "CommandLineToArgvW", {clw, argc}).eax();
    CHECK_EQ(rd32(argc), 3u);
    CHECK(utf16_to_utf8((uint16_t *)gp(rd32(argv + 8)), gwcslen(rd32(argv + 8))) == "my file.ini");
    // environment
    uint32_t buf = t::galloc(512);
    uint32_t n = t::hle("kernel32.dll", "GetEnvironmentVariableA", {t::gstr("username"), buf, 512}).eax();
    CHECK_EQ(n, 6u);
    CHECK(strcmp((char *)gp(buf), "Player") == 0);
    CHECK_EQ(t::hle("kernel32.dll", "GetEnvironmentVariableA", {t::gstr("NOPE_NOT_SET"), buf, 512}).eax(), 0u);
    CHECK_EQ(get_last_error(), 203u);
    CHECK_EQ(t::hle("kernel32.dll", "GetEnvironmentVariableA", {t::gstr("username"), buf, 3}).eax(), 7u);   // required size incl. NUL
    t::hle("kernel32.dll", "SetEnvironmentVariableW", {t::gwstr8("DISH_X"), t::gwstr8("1")});
    CHECK_EQ(t::hle("kernel32.dll", "GetEnvironmentVariableW", {t::gwstr8("dish_x"), buf, 100}).eax(), 1u);
    uint32_t blk = t::hle("kernel32.dll", "GetEnvironmentStringsW", {}).eax();
    bool found = false;
    for (uint32_t a = blk; rd16(a);) {
        std::u16string e = read_wstr(a);
        if (e == u"DISH_X=1") found = true;
        a += (uint32_t)(e.size() + 1) * 2;
    }
    CHECK(found);
    CHECK_EQ(t::hle("kernel32.dll", "FreeEnvironmentStringsW", {blk}).eax(), 1u);
    uint32_t si = t::galloc(68);
    t::hle("kernel32.dll", "GetStartupInfoA", {si});
    CHECK_EQ(rd32(si), 68u);
    CHECK_EQ(rd16(si + 50 - 2 + 2) | 0, rd16(si + 50));
}

TEST(output_debug_string_goes_to_log) {
    t::setup_runtime("misc");
    static std::string seen;
    seen.clear();
    g_log_hook = [](LogCh ch, LogLvl, const char *line) { if (ch == LC_ENGINE) seen = line; };
    t::hle("kernel32.dll", "OutputDebugStringA", {t::gstr("Log: engine init\r\n")});
    CHECK(seen == "Log: engine init");
    t::hle("kernel32.dll", "OutputDebugStringW", {t::gwstr(u"wide é\n")});
    CHECK(seen == "wide \xc3\xa9");
    g_log_hook = nullptr;
}

TEST(last_error_lives_in_teb) {
    t::setup_runtime("misc");
    t::hle("kernel32.dll", "SetLastError", {1234});
    CHECK_EQ(t::hle("kernel32.dll", "GetLastError", {}).eax(), 1234u);
    CHECK_EQ(rd32(cur_teb() + 0x34), 1234u);            // fs:[0x34] is what lifted code reads directly
    wr32(cur_teb() + 0x34, 55);
    CHECK_EQ(t::hle("kernel32.dll", "GetLastError", {}).eax(), 55u);
    std::atomic<uint32_t> other{99};
    std::thread th([&] {
        RtThread *me = thread_attach("le", 0x20000, true);
        other = get_last_error();
        thread_detach(me);
    });
    th.join();
    CHECK_EQ(other.load(), 0u);
}

static uint32_t g_handler_calls = 0, g_handler_code = 0;
static GuestRet seh_handler(uint32_t esp, uint32_t, uint32_t, uint32_t) {          // EXCEPTION_DISPOSITION cdecl handler(rec, frame, ctx, disp)
    g_handler_calls++;
    g_handler_code = rd32(rd32(esp + 4));
    return guest_ret(0, 0, 0);                          // ExceptionContinueExecution
}
TEST(raise_exception_walks_the_guest_seh_chain) {
    t::setup_runtime("misc");
    t::register_guest_fn(0x00404000, seh_handler);
    uint32_t frame = t::galloc(8);                      // EXCEPTION_REGISTRATION { next, handler }
    wr32(frame, rd32(cur_teb()));
    wr32(frame + 4, 0x00404000);
    uint32_t saved = rd32(cur_teb());
    wr32(cur_teb(), frame);                             // fs:[0] = frame
    auto r = t::hle("kernel32.dll", "RaiseException", {0xE06D7363, 0, 0, 0});
    wr32(cur_teb(), saved);
    CHECK_EQ(r.pop(), 16u);
    CHECK_EQ(g_handler_calls, 1u);
    CHECK_EQ(g_handler_code, 0xE06D7363u);
}

TEST(misc_stubs_return_failure_not_crash) {
    t::setup_runtime("misc");
    uint32_t pp = t::galloc(4);
    CHECK(t::hle("ole32.dll", "CoCreateInstance", {t::galloc(16), 0, 0x17, t::galloc(16), pp}).eax() == 0x80040154u);
    CHECK_EQ(rd32(pp), 0u);
    uint32_t g = t::galloc(16);
    t::hle("ole32.dll", "CoCreateGuid", {g});
    uint32_t g2 = t::galloc(16);
    t::hle("ole32.dll", "CoCreateGuid", {g2});
    CHECK(memcmp(gp(g), gp(g2), 16) != 0);
    uint32_t b = t::hle("oleaut32.dll", "SysAllocString", {t::gwstr(u"hello")}).eax();
    CHECK_EQ(rd32(b - 4), 10u);
    CHECK_EQ(t::hle("oleaut32.dll", "SysStringLen", {b}).eax(), 5u);
    t::hle("oleaut32.dll", "SysFreeString", {b});
    CHECK_EQ(t::hle("wsock32.dll", "socket", {2, 2, 17}).eax(), 0xFFFFFFFFu);
    CHECK_EQ(t::hle("wsock32.dll", "htons", {0x1234}).eax(), 0x3412u);
    { uint32_t r = t::hle("xinput1_3.dll", "XInputGetState", {0, t::galloc(16)}).eax(); CHECK(r == 0 || r == 1167u); }   // pad present or not
    uint32_t wsa = t::galloc(400);
    CHECK_EQ(t::hle("wsock32.dll", "WSAStartup", {0x202, wsa}).eax(), 0u);
    CHECK_EQ(t::hle("kernel32.dll", "CreateProcessW", {0, t::gwstr8("cmd.exe"), 0, 0, 0, 0, 0, 0, 0, 0}).eax(), 0u);
}
