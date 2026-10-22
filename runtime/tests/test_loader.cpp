#include "testing.h"
#include "dish/config.h"
#include "dish/sha256.h"
#include "dish/sync.h"
#include <cstring>
#include <fstream>

using namespace dish;

#include "pe_builder.h"

// ---- fake lifted code ----------------------------------------------------------------------------------------
constexpr uint32_t DLL_PREF = 0x10000000, DLL_BASE = 0x20000000, EXE_BASE = 0x00400000, LAZY_BASE = 0x21000000;
static std::vector<std::string> g_order;
static uint32_t g_dll_seen_ptr, g_dll_seen_iat, g_dll_hinst, g_tid_from_iat;
static std::atomic<int> g_thread_attach{0}, g_thread_detach{0};

static GuestRet dll_main(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    uint32_t hinst = rd32(esp + 4), reason = rd32(esp + 8);
    if (reason == 1) {
        g_order.push_back("DllMain:attach");
        g_dll_hinst = hinst;
        g_dll_seen_ptr = rd32(DLL_BASE + 0x2000);              // relocated absolute pointer
        g_dll_seen_iat = rd32(DLL_BASE + 0x2100);              // IAT slot -> thunk
        uint32_t sp = esp - 4;                                 // call GetCurrentThreadId through the IAT
        wr32(sp, kReturnSentinel);
        g_tid_from_iat = guest_ret_eax(guest_call(g_dll_seen_iat, sp, 0, 0, 0));
    } else if (reason == 2) g_thread_attach++;
    else if (reason == 3) g_thread_detach++;
    return guest_ret(1, 0, 12);
}
static GuestRet tls_cb(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    if (rd32(esp + 8) == 1) g_order.push_back("TLS:attach");
    return guest_ret(0, 0, 12);
}
static GuestRet dll_answer(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(1234, 0, 0); }
static GuestRet exe_entry(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    g_order.push_back("exe:entry");
    uint32_t slot_import = rd32(EXE_BASE + 0x2100), slot_guest = rd32(EXE_BASE + 0x2104);
    uint32_t sp = esp - 8;
    wr32(sp, kReturnSentinel);
    wr32(sp + 4, 1);
    guest_call(slot_import, sp, 0, 0, 0);                      // Sleep(1) through the IAT
    wr32(sp, kReturnSentinel);
    uint32_t ans = guest_ret_eax(guest_call(slot_guest, sp, 0, 0, 0));   // recompiled export through a GUEST_ADDR slot
    return guest_ret(ans - 1192, 0, 0);                        // 42
}

// Lazy.dll: imported by nothing, so it stays hidden until LoadLibrary runs its DllMain
static int g_lazy_attach = 0;
static uint32_t g_lazy_esp = 0;
static GuestRet lazy_main(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    if (rd32(esp + 8) == 1) { g_lazy_attach++; g_lazy_esp = esp; }
    for (uint32_t a = esp - 64; a < esp; a += 4) wr32(a, 0xDEADBEEF);   // scribble below its own frame, like a real DllMain's locals
    return guest_ret(1, 0, 12);
}

static GuestModuleDesc g_mods[3];
static GuestImportDesc g_imps[2];
static GuestIatEntry g_iat[3];
static GuestProgram g_prog;

static void build_files() {
    std::string game = config().game_root;
    // DLL: .text (RX), .data (RW) with reloc'd pointers, .reloc
    std::vector<uint8_t> text(0x200, 0x90);
    std::vector<uint8_t> data(0x400, 0);
    auto ab = [&](uint32_t off) { return DLL_PREF + 0x2000 + off; };     // absolute address at the preferred base
    put32(data, 0x000, ab(0x10));                                        // pointer to something at +0x10
    // TLS directory at +0x40
    put32(data, 0x040, ab(0x80)); put32(data, 0x044, ab(0x88)); put32(data, 0x048, ab(0x90)); put32(data, 0x04C, ab(0xA0));
    put32(data, 0x050, 8); put32(data, 0x054, 0);
    for (int i = 0; i < 8; i++) data[0x80 + i] = (uint8_t)(0xA0 + i);   // TLS template
    put32(data, 0x0A0, DLL_PREF + 0x1020); put32(data, 0x0A4, 0);       // TLS callbacks
    // export directory at +0x200: one function "Answer" -> RVA 0x1010
    put32(data, 0x200 + 12, 0x2000 + 0x260);                              // Name
    put32(data, 0x200 + 16, 1);                                           // Base
    put32(data, 0x200 + 20, 1); put32(data, 0x200 + 24, 1);               // NumberOfFunctions, NumberOfNames
    put32(data, 0x200 + 28, 0x2000 + 0x240); put32(data, 0x200 + 32, 0x2000 + 0x244); put32(data, 0x200 + 36, 0x2000 + 0x248);
    put32(data, 0x240, 0x1010);                                           // function RVA
    put32(data, 0x244, 0x2000 + 0x250);                                   // name RVA
    put16(data, 0x248, 0);                                                // ordinal index
    memcpy(&data[0x250], "Answer", 7);
    memcpy(&data[0x260], "mydll.dll", 10);
    // relocations: HIGHLOW entries for the absolute pointers above (page 0x2000)
    std::vector<uint8_t> reloc;
    std::vector<uint16_t> ents;
    for (uint32_t off : {0x000u, 0x040u, 0x044u, 0x048u, 0x04Cu}) ents.push_back((uint16_t)(0x3000 | off));
    ents.push_back(0x30A0 | 0);   // absolute fixup at +0xA0 (TLS callback pointer)
    put32(reloc, 0, 0x2000);
    put32(reloc, 4, (uint32_t)(8 + ents.size() * 2 + 2));
    for (size_t i = 0; i < ents.size(); i++) put16(reloc, 8 + 2 * i, ents[i]);
    put16(reloc, 8 + 2 * ents.size(), 0);
    reloc.resize((reloc.size() + 3) & ~3u);
    reloc[4] = (uint8_t)reloc.size();
    // fix the entry at +0xA0 (the TLS callback pointer) which is also relocatable
    auto dll = build_pe(DLL_PREF, 0x1000, true,
                        {{".text", 0x1000, 0x60000020, text, 0x200}, {".data", 0x2000, 0xC0000040, data, 0x400}, {".reloc", 0x3000, 0x42000040, reloc, 0x100}},
                        0x4000, {{0, {0x2200, 0x60}}, {5, {0x3000, (uint32_t)reloc.size()}}, {9, {0x2040, 24}}});
    write_file(game + "/Binaries/Win32/MyDll.dll", dll);
    // EXE at its preferred base
    std::vector<uint8_t> edata(0x400, 0);
    auto exe = build_pe(EXE_BASE, 0x1000, false, {{".text", 0x1000, 0x60000020, text, 0x200}, {".data", 0x2000, 0xC0000040, edata, 0x400}}, 0x3000, {});
    write_file(game + "/Binaries/Win32/fake.exe", exe);
    write_file(game + "/Binaries/Win32/bad.dll", std::vector<uint8_t>(dll.begin(), dll.end()));
    auto lazy = build_pe(LAZY_BASE, 0x1000, true, {{".text", 0x1000, 0x60000020, text, 0x200}}, 0x2000, {});
    write_file(game + "/Binaries/Win32/Lazy.dll", lazy);

    auto fill = [](GuestModuleDesc &d, const char *name, const char *rel, const std::vector<uint8_t> &bytes, uint32_t base, uint32_t pref, uint32_t size,
                   uint32_t entry, bool isdll) {
        d.name = name; d.relpath = rel;
        sha256(bytes.data(), bytes.size(), d.sha256);
        d.base = base; d.preferred_base = pref; d.image_size = size; d.entry = entry; d.is_dll = isdll;
    };
    fill(g_mods[0], "MyDll.dll", "Binaries/Win32/MyDll.dll", dll, DLL_BASE, DLL_PREF, 0x4000, DLL_BASE + 0x1000, true);
    fill(g_mods[1], "fake.exe", "Binaries/Win32/fake.exe", exe, EXE_BASE, EXE_BASE, 0x3000, EXE_BASE + 0x1000, false);
    fill(g_mods[2], "Lazy.dll", "Binaries/Win32/Lazy.dll", lazy, LAZY_BASE, LAZY_BASE, 0x2000, LAZY_BASE + 0x1000, true);
    g_imps[0] = {"kernel32.dll", "GetCurrentThreadId", 0};
    g_imps[1] = {"kernel32.dll", "Sleep", 0};
    g_iat[0] = {DLL_BASE + 0x2100, GUEST_IAT_IMPORT, 0};
    g_iat[1] = {EXE_BASE + 0x2100, GUEST_IAT_IMPORT, 1};
    g_iat[2] = {EXE_BASE + 0x2104, GUEST_IAT_GUEST_ADDR, DLL_BASE + 0x1010};
    g_prog = {GUEST_ABI_VERSION, g_mods, 3, g_imps, 2, g_iat, 3};
}

static void build_and_reserve() {
    build_files();
    // the real runtime_init reserves module ranges before the first thread exists; the fixture mimics that
    for (int i = 0; i < 3; i++) vm_reserve_image(g_mods[i].base, g_mods[i].image_size);
}

TEST(map_relocate_iat_and_run_entries) {
    t::setup_runtime("loader", build_and_reserve);
    t::register_guest_fn(DLL_BASE + 0x1000, dll_main);
    t::register_guest_fn(DLL_BASE + 0x1010, dll_answer);
    t::register_guest_fn(DLL_BASE + 0x1020, tls_cb);
    t::register_guest_fn(EXE_BASE + 0x1000, exe_entry);
    t::register_guest_fn(LAZY_BASE + 0x1000, lazy_main);
    CHECK(runtime_init(g_prog));
    std::string err;
    CHECK(loader_map_all(g_prog, &err));
    if (!err.empty()) fprintf(stderr, "  loader error: %s\n", err.c_str());
    // headers and sections are in guest memory, taken from the file
    CHECK_EQ(rd16(DLL_BASE), 0x5A4D);
    CHECK_EQ(rd8(DLL_BASE + 0x1000), 0x90);
    // the relocated absolute pointer now points into the new base
    CHECK_EQ(rd32(DLL_BASE + 0x2000), DLL_BASE + 0x2010);
    CHECK_EQ(rd32(DLL_BASE + 0x2040), DLL_BASE + 0x2080);                   // TLS directory fields relocated too
    // IAT slots: import -> thunk address, guest-addr -> value
    CHECK_EQ(rd32(DLL_BASE + 0x2100), thunk_addr(0));
    CHECK_EQ(rd32(EXE_BASE + 0x2100), thunk_addr(1));
    CHECK_EQ(rd32(EXE_BASE + 0x2104), DLL_BASE + 0x1010);
    CHECK(std::string(g_thunks[0].name) == "GetCurrentThreadId");
    // section protections follow the PE flags
    VmQuery q;
    vm_query(DLL_BASE + 0x1000, &q);
    CHECK_EQ(q.prot, (uint32_t)PAGE_READONLY);
    CHECK_EQ(q.type, (uint32_t)MEM_IMAGE);
    vm_query(DLL_BASE + 0x2000, &q);
    CHECK_EQ(q.prot, (uint32_t)PAGE_READWRITE);
    // module registry
    const LoadedModule *dll = module_by_name("mydll");
    CHECK(dll != nullptr && dll->base == DLL_BASE);
    CHECK(dll->exports.size() == 1 && dll->exports[0].name == "Answer");
    CHECK(module_main_exe() && module_main_exe()->base == EXE_BASE);
    CHECK(module_main_exe()->guest_path == "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Dishonored\\Binaries\\Win32\\fake.exe");
    // static TLS: index assigned, this thread's block holds the template
    uint32_t idx = rd32(DLL_BASE + 0x2090);
    uint32_t blk = rd32(rd32(cur_teb() + 0x2C) + 4 * idx);
    CHECK(blk != 0);
    CHECK_EQ(rd8(blk + 3), 0xA3);
    CHECK_EQ(rd8(blk + 9), 0);

    // run: TLS callback, DllMain(PROCESS_ATTACH), exe entry
    int rc = loader_run_entries(g_prog);
    CHECK_EQ(rc, 42);
    CHECK_EQ(g_order.size(), 3u);
    if (g_order.size() == 3) { CHECK(g_order[0] == "TLS:attach"); CHECK(g_order[1] == "DllMain:attach"); CHECK(g_order[2] == "exe:entry"); }
    CHECK_EQ(g_dll_hinst, DLL_BASE);
    CHECK_EQ(g_dll_seen_ptr, DLL_BASE + 0x2010);
    CHECK_EQ(g_dll_seen_iat, thunk_addr(0));
    CHECK_EQ(g_tid_from_iat, cur_tid());
}

TEST(module_apis_through_hle) {
    t::setup_runtime("loader");
    uint32_t exe_h = t::hle("kernel32.dll", "GetModuleHandleW", {0}).eax();
    CHECK_EQ(exe_h, EXE_BASE);
    CHECK_EQ(t::hle("kernel32.dll", "GetModuleHandleW", {t::gwstr8("MYDLL.DLL")}).eax(), DLL_BASE);
    CHECK_EQ(t::hle("kernel32.dll", "GetModuleHandleA", {t::gstr("mydll")}).eax(), DLL_BASE);
    CHECK_EQ(t::hle("kernel32.dll", "LoadLibraryW", {t::gwstr8("C:\\anywhere\\mydll.dll")}).eax(), DLL_BASE);
    uint32_t k32 = t::hle("kernel32.dll", "GetModuleHandleW", {t::gwstr8("KERNEL32.dll")}).eax();
    CHECK(k32 != 0);
    CHECK_EQ(rd16(k32), 0x5A4D);                                              // a readable fake PE header
    CHECK_EQ(t::hle("kernel32.dll", "LoadLibraryA", {t::gstr("kernel32")}).eax(), k32);
    CHECK_EQ(t::hle("kernel32.dll", "LoadLibraryA", {t::gstr("nvapi.dll")}).eax(), 0u);
    CHECK_EQ(get_last_error(), 126u);
    CHECK_EQ(t::hle("kernel32.dll", "GetModuleHandleW", {t::gwstr8("nosuch.dll")}).eax(), 0u);
    // GetProcAddress: recompiled export, system function (a thunk), missing
    CHECK_EQ(t::hle("kernel32.dll", "GetProcAddress", {DLL_BASE, t::gstr("Answer")}).eax(), DLL_BASE + 0x1010);
    CHECK_EQ(t::hle("kernel32.dll", "GetProcAddress", {DLL_BASE, 1}).eax(), DLL_BASE + 0x1010);       // by ordinal
    uint32_t p = t::hle("kernel32.dll", "GetProcAddress", {k32, t::gstr("FlsAlloc")}).eax();
    CHECK(is_thunk_addr(p));
    CHECK_EQ(t::hle("kernel32.dll", "GetProcAddress", {k32, t::gstr("Sleep")}).eax(), thunk_addr(1));    // same thunk as the IAT import
    CHECK_EQ(t::hle("kernel32.dll", "GetProcAddress", {k32, t::gstr("NoSuchApi")}).eax(), 0u);
    CHECK_EQ(get_last_error(), 127u);
    // the resolved thunk works
    auto r = t::call_addr(p, {0});
    CHECK(r.eax() != 0xFFFFFFFFu);
    // module file names
    uint32_t buf = t::galloc(1040);
    uint32_t n = t::hle("kernel32.dll", "GetModuleFileNameW", {0, buf, 520}).eax();
    CHECK(utf16_to_utf8((uint16_t *)gp(buf), n).find("Binaries\\Win32\\fake.exe") != std::string::npos);
    n = t::hle("kernel32.dll", "GetModuleFileNameA", {k32, buf, 520}).eax();
    CHECK(std::string((char *)gp(buf), n) == "C:\\Windows\\System32\\kernel32.dll");
    // truncation sets ERROR_INSUFFICIENT_BUFFER and still terminates
    n = t::hle("kernel32.dll", "GetModuleFileNameA", {k32, buf, 8}).eax();
    CHECK_EQ(n, 8u);
    CHECK_EQ(get_last_error(), 122u);
    CHECK_EQ(gp(buf)[7], 0);
}

// A guest function with a live frame calls LoadLibraryA; the hidden DLL's DllMain runs below that frame (not over it).
static uint32_t g_lazy_name = 0, g_caller_sp = 0;
static bool g_canary_ok = false;
static GuestRet lazy_caller(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    for (uint32_t a = esp - 32; a < esp; a += 4) wr32(a, 0x5AFE0000 + (a & 0xFFFF));   // "locals" (a frame-pointer-less function)
    uint32_t sp = esp - 40;
    wr32(sp, kReturnSentinel);
    wr32(sp + 4, g_lazy_name);
    g_caller_sp = sp;
    GuestRet r = guest_call(t::hle_addr("kernel32.dll", "LoadLibraryA"), sp, 0, 0, 0);
    g_canary_ok = true;
    for (uint32_t a = esp - 32; a < esp; a += 4) if (rd32(a) != 0x5AFE0000 + (a & 0xFFFF)) g_canary_ok = false;
    return guest_ret(guest_ret_eax(r), 0, 0);
}

TEST(hidden_dll_loads_on_first_loadlibrary_below_the_caller) {
    t::setup_runtime("loader");
    CHECK_EQ(g_lazy_attach, 0);                                                // not initialised at startup
    CHECK_EQ(t::hle("kernel32.dll", "GetModuleHandleA", {t::gstr("lazy.dll")}).eax(), 0u);   // and invisible
    g_lazy_name = t::gstr("C:\\somewhere\\Lazy.dll");
    t::register_guest_fn(0x00404000, lazy_caller);
    CHECK_EQ(t::call_addr(0x00404000, {}).eax(), LAZY_BASE);
    CHECK_EQ(g_lazy_attach, 1);
    CHECK(g_canary_ok);                                                        // the caller's frame survived DllMain
    CHECK(g_lazy_esp < g_caller_sp);
    CHECK_EQ(t::hle("kernel32.dll", "GetModuleHandleA", {t::gstr("lazy.dll")}).eax(), LAZY_BASE);
    CHECK_EQ(t::hle("kernel32.dll", "LoadLibraryA", {t::gstr("Lazy")}).eax(), LAZY_BASE);
    CHECK_EQ(g_lazy_attach, 1);                                                // DllMain runs once
}

TEST(thread_attach_notifications_and_disable_calls) {
    t::setup_runtime("loader");
    t::register_guest_fn(0x00403000, [](uint32_t, uint32_t, uint32_t, uint32_t) -> GuestRet { return guest_ret(0, 0, 4); });
    int a0 = g_thread_attach.load(), d0 = g_thread_detach.load();
    uint32_t h = thread_create(0x00403000, 0, 0, false, nullptr);
    t::hle("kernel32.dll", "WaitForSingleObject", {h, 5000});
    CHECK_EQ(g_thread_attach.load(), a0 + 1);
    CHECK_EQ(g_thread_detach.load(), d0 + 1);
    CHECK_EQ(t::hle("kernel32.dll", "DisableThreadLibraryCalls", {DLL_BASE}).eax(), 1u);
    h = thread_create(0x00403000, 0, 0, false, nullptr);
    t::hle("kernel32.dll", "WaitForSingleObject", {h, 5000});
    CHECK_EQ(g_thread_attach.load(), a0 + 1);                                // no further notifications
}

TEST(rejects_wrong_hash_and_missing_files) {
    t::setup_runtime("loader");
    GuestModuleDesc bad = g_mods[0];
    bad.name = "bad.dll";
    bad.relpath = "binaries/win32/BAD.DLL";           // case differs from the file on disk
    bad.base = 0x30000000;
    bad.sha256[0] ^= 0xFF;
    GuestProgram p = {GUEST_ABI_VERSION, &bad, 1, nullptr, 0, nullptr, 0};
    std::string err;
    CHECK(!loader_map_all(p, &err));
    CHECK(err.find("SHA-256 mismatch") != std::string::npos);
    CHECK(err.find("bad.dll") != std::string::npos);
    VmQuery q;
    vm_query(0x30000000, &q);
    CHECK_EQ(q.state, (uint32_t)MEM_FREE);            // nothing was mapped
    bad.relpath = "Binaries/Win32/missing.dll";
    CHECK(!loader_map_all(p, &err));
    CHECK(err.find("missing file") != std::string::npos);
    // verification can be disabled for development
    config().verify_hashes = false;
    bad.relpath = "Binaries/Win32/bad.dll";
    err.clear();
    CHECK(loader_map_all(p, &err));
    config().verify_hashes = true;
}
