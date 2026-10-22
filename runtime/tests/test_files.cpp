#include "testing.h"
#include "dish/config.h"
#include "dish/vfs.h"
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace dish;

static const char *GAME = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Dishonored";
static const char *DOCS = "C:\\Users\\Player\\Documents";

static void write_host(const std::string &p, const std::string &content) {
    std::string cmd = "mkdir -p '" + p.substr(0, p.rfind('/')) + "'";
    (void)!system(cmd.c_str());
    std::ofstream(p, std::ios::binary) << content;
}
static std::string read_host(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void make_game_tree() {
    static bool done = false;
    if (done) return;
    done = true;
    std::string g = config().game_root;
    write_host(g + "/Binaries/Win32/ReadMe.TXT", "hello from the game dir\n");
    write_host(g + "/DishonoredGame/CookedPCConsole/Alpha.upk", std::string(1000, 'a'));
    write_host(g + "/DishonoredGame/CookedPCConsole/beta.UPK", std::string(2000, 'b'));
    write_host(g + "/DishonoredGame/CookedPCConsole/Gamma.tfc", std::string(10, 'c'));
    write_host(g + "/DishonoredGame/Config/Default.ini", "[Core]\nx=1\n");
    vfs_clear_cache();
}
static std::string gp_(const std::string &rel) { return std::string(GAME) + "\\" + rel; }

static uint32_t create_file(const std::string &path, uint32_t access, uint32_t disp, uint32_t flags = 0) {
    return t::hle("kernel32.dll", "CreateFileW", {t::gwstr8(path), access, 3, 0, disp, flags, 0}).eax();
}
constexpr uint32_t GR = 0x80000000u, GW = 0x40000000u;

TEST(case_insensitive_read_from_game_dir) {
    t::setup_runtime("files");
    make_game_tree();
    uint32_t h = create_file(gp_("binaries\\win32\\README.txt"), GR, 3);
    CHECK(h != 0xFFFFFFFFu);
    uint32_t buf = t::galloc(64), nread = t::galloc(4);
    auto r = t::hle("kernel32.dll", "ReadFile", {h, buf, 64, nread, 0});
    CHECK_EQ(r.eax(), 1u);
    CHECK_EQ(r.pop(), 20u);
    CHECK_EQ(rd32(nread), 24u);
    CHECK(strncmp((char *)gp(buf), "hello from the game dir\n", 24) == 0);
    // EOF: ReadFile succeeds with 0 bytes
    r = t::hle("kernel32.dll", "ReadFile", {h, buf, 64, nread, 0});
    CHECK_EQ(r.eax(), 1u);
    CHECK_EQ(rd32(nread), 0u);
    CHECK_EQ(t::hle("kernel32.dll", "GetFileSize", {h, 0}).eax(), 24u);
    // seek
    CHECK_EQ(t::hle("kernel32.dll", "SetFilePointer", {h, 6, 0, 0}).eax(), 6u);
    t::hle("kernel32.dll", "ReadFile", {h, buf, 4, nread, 0});
    CHECK(strncmp((char *)gp(buf), "from", 4) == 0);
    CHECK_EQ(t::hle("kernel32.dll", "SetFilePointer", {h, (uint32_t)-4, 0, 2}).eax(), 20u);   // from end
    t::hle("kernel32.dll", "CloseHandle", {h});
    // missing file / missing directory error codes
    h = create_file(gp_("Binaries\\Win32\\nope.txt"), GR, 3);
    CHECK_EQ(h, 0xFFFFFFFFu);
    CHECK_EQ(get_last_error(), 2u);
    h = create_file(gp_("NoSuchDir\\nope.txt"), GR, 3);
    CHECK_EQ(get_last_error(), 3u);
    // the "A" variant takes ANSI paths
    h = t::hle("kernel32.dll", "CreateFileA", {t::gstr(gp_("BINARIES\\win32\\readme.txt")), GR, 1, 0, 3, 0, 0}).eax();
    CHECK(h != 0xFFFFFFFFu);
    t::hle("kernel32.dll", "CloseHandle", {h});
}

TEST(write_to_save_dir_and_directories) {
    t::setup_runtime("files");
    make_game_tree();
    std::string dir = std::string(DOCS) + "\\My Games\\Dishonored";
    uint32_t h = create_file(dir + "\\DishonoredGame\\Logs\\Launch.log", GW, 2);
    CHECK_EQ(h, 0xFFFFFFFFu);
    CHECK_EQ(get_last_error(), 3u);                                  // parent missing
    CHECK_EQ(t::hle("kernel32.dll", "CreateDirectoryW", {t::gwstr8(std::string(DOCS) + "\\My Games"), 0}).eax(), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "CreateDirectoryW", {t::gwstr8(dir), 0}).eax(), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "CreateDirectoryW", {t::gwstr8(dir), 0}).eax(), 0u);
    CHECK_EQ(get_last_error(), 183u);
    t::hle("kernel32.dll", "CreateDirectoryW", {t::gwstr8(dir + "\\DishonoredGame"), 0});
    t::hle("kernel32.dll", "CreateDirectoryW", {t::gwstr8(dir + "\\DishonoredGame\\Logs"), 0});
    h = create_file(dir + "\\DishonoredGame\\Logs\\Launch.log", GW, 2);
    CHECK(h != 0xFFFFFFFFu);
    uint32_t data = t::gstr("Log file open\r\n"), wrote = t::galloc(4);
    auto r = t::hle("kernel32.dll", "WriteFile", {h, data, 15, wrote, 0});
    CHECK_EQ(r.eax(), 1u);
    CHECK_EQ(rd32(wrote), 15u);
    t::hle("kernel32.dll", "CloseHandle", {h});
    std::string host = config().save_dir + "/Documents/My Games/Dishonored/DishonoredGame/Logs/Launch.log";
    CHECK(read_host(host) == "Log file open\r\n");
    // case-insensitive lookup of what we just created
    h = create_file(dir + "\\dishonoredgame\\LOGS\\launch.LOG", GR, 3);
    CHECK(h != 0xFFFFFFFFu);
    t::hle("kernel32.dll", "CloseHandle", {h});
    // CREATE_NEW on an existing file
    CHECK_EQ(create_file(dir + "\\DishonoredGame\\Logs\\Launch.log", GW, 1), 0xFFFFFFFFu);
    CHECK_EQ(get_last_error(), 80u);
    // OPEN_ALWAYS on an existing file reports ERROR_ALREADY_EXISTS but succeeds
    h = create_file(dir + "\\DishonoredGame\\Logs\\Launch.log", GR | GW, 4);
    CHECK(h != 0xFFFFFFFFu);
    CHECK_EQ(get_last_error(), 183u);
    t::hle("kernel32.dll", "CloseHandle", {h});
    // move / copy / delete
    std::string logs = dir + "\\DishonoredGame\\Logs\\";
    CHECK_EQ(t::hle("kernel32.dll", "CopyFileW", {t::gwstr8(logs + "Launch.log"), t::gwstr8(logs + "Copy.log"), 1}).eax(), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "CopyFileW", {t::gwstr8(logs + "Launch.log"), t::gwstr8(logs + "Copy.log"), 1}).eax(), 0u);
    CHECK_EQ(t::hle("kernel32.dll", "MoveFileW", {t::gwstr8(logs + "Copy.log"), t::gwstr8(logs + "Moved.log")}).eax(), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "GetFileAttributesW", {t::gwstr8(logs + "Copy.log")}).eax(), 0xFFFFFFFFu);
    CHECK_EQ(t::hle("kernel32.dll", "DeleteFileW", {t::gwstr8(logs + "Moved.log")}).eax(), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "DeleteFileW", {t::gwstr8(logs + "Moved.log")}).eax(), 0u);
    CHECK_EQ(get_last_error(), 2u);
}

TEST(game_root_is_never_modified) {
    t::setup_runtime("files");
    make_game_tree();
    std::string host_ini = config().game_root + "/DishonoredGame/Config/Default.ini";
    std::string orig = read_host(host_ini);
    // opening an existing game file for write copies it up into the overlay
    uint32_t h = create_file(gp_("DishonoredGame\\Config\\default.ini"), GW | GR, 3);
    CHECK(h != 0xFFFFFFFFu);
    uint32_t d = t::gstr("[Core]\nx=2\n"), w = t::galloc(4);
    t::hle("kernel32.dll", "WriteFile", {h, d, 11, w, 0});
    t::hle("kernel32.dll", "CloseHandle", {h});
    CHECK(read_host(host_ini) == orig);                              // untouched
    CHECK(read_host(config().save_dir + "/GameOverlay/DishonoredGame/Config/Default.ini").substr(0, 11) == "[Core]\nx=2\n");
    // later reads see the overlay copy
    h = create_file(gp_("DishonoredGame\\Config\\Default.ini"), GR, 3);
    uint32_t buf = t::galloc(32), n = t::galloc(4);
    t::hle("kernel32.dll", "ReadFile", {h, buf, 32, n, 0});
    CHECK(strncmp((char *)gp(buf), "[Core]\nx=2", 10) == 0);
    t::hle("kernel32.dll", "CloseHandle", {h});
    // creating a new file next to game files goes to the overlay too
    h = create_file(gp_("DishonoredGame\\Config\\New.ini"), GW, 2);
    CHECK(h != 0xFFFFFFFFu);
    t::hle("kernel32.dll", "CloseHandle", {h});
    struct stat st;
    CHECK(stat((config().game_root + "/DishonoredGame/Config/New.ini").c_str(), &st) != 0);
    CHECK(stat((config().save_dir + "/GameOverlay/DishonoredGame/Config/New.ini").c_str(), &st) == 0);
    // deleting a pristine game file is refused
    CHECK_EQ(t::hle("kernel32.dll", "DeleteFileW", {t::gwstr8(gp_("Binaries\\Win32\\ReadMe.TXT"))}).eax(), 0u);
    CHECK_EQ(get_last_error(), 5u);
    CHECK_EQ(t::hle("kernel32.dll", "RemoveDirectoryW", {t::gwstr8(gp_("Binaries"))}).eax(), 0u);
}

static std::string find_name(uint32_t data) { return utf16_to_utf8((const uint16_t *)gp(data + 44), gwcslen(data + 44)); }

TEST(find_files) {
    t::setup_runtime("files");
    make_game_tree();
    uint32_t data = t::galloc(600);
    auto r = t::hle("kernel32.dll", "FindFirstFileW", {t::gwstr8(gp_("DishonoredGame\\CookedPCConsole\\*.upk")), data});
    CHECK(r.eax() != 0xFFFFFFFFu);
    CHECK_EQ(r.pop(), 8u);
    std::vector<std::string> names;
    names.push_back(find_name(data));
    uint32_t h = r.eax();
    while (t::hle("kernel32.dll", "FindNextFileW", {h, data}).eax()) names.push_back(find_name(data));
    CHECK_EQ(get_last_error(), 18u);                                 // ERROR_NO_MORE_FILES
    CHECK_EQ(names.size(), 2u);                                      // matching is case-insensitive: Alpha.upk and beta.UPK
    CHECK(names[0] == "Alpha.upk" && names[1] == "beta.UPK");
    CHECK_EQ(rd32(data + 28) == 0 ? 1u : 0u, 1u);
    CHECK_EQ(t::hle("kernel32.dll", "FindClose", {h}).eax(), 1u);
    // all entries, with "." and ".."
    r = t::hle("kernel32.dll", "FindFirstFileW", {t::gwstr8(gp_("DishonoredGame\\CookedPCConsole\\*")), data});
    h = r.eax();
    int count = 1;
    std::string first = find_name(data);
    CHECK(first == ".");
    while (t::hle("kernel32.dll", "FindNextFileW", {h, data}).eax()) count++;
    CHECK_EQ(count, 5);                                              // . .. Alpha beta Gamma
    t::hle("kernel32.dll", "FindClose", {h});
    // a specific file: size and attributes
    r = t::hle("kernel32.dll", "FindFirstFileW", {t::gwstr8(gp_("dishonoredgame\\cookedpcconsole\\ALPHA.UPK")), data});
    CHECK(r.eax() != 0xFFFFFFFFu);
    CHECK_EQ(rd32(data + 32), 1000u);
    CHECK(find_name(data) == "Alpha.upk");                           // real on-disk case is reported
    t::hle("kernel32.dll", "FindClose", {r.eax()});
    // directories carry the directory attribute
    r = t::hle("kernel32.dll", "FindFirstFileW", {t::gwstr8(gp_("DishonoredGame\\*")), data});
    bool saw_dir = false;
    do { if (rd32(data) & 0x10) saw_dir = true; } while (t::hle("kernel32.dll", "FindNextFileW", {r.eax(), data}).eax());
    CHECK(saw_dir);
    // missing
    r = t::hle("kernel32.dll", "FindFirstFileW", {t::gwstr8(gp_("DishonoredGame\\Nope\\*")), data});
    CHECK_EQ(r.eax(), 0xFFFFFFFFu);
    CHECK_EQ(get_last_error(), 3u);
    r = t::hle("kernel32.dll", "FindFirstFileW", {t::gwstr8(gp_("DishonoredGame\\Config\\*.xyz")), data});
    CHECK_EQ(get_last_error(), 2u);
    // virtual directories (ancestors of mount points) can be listed
    r = t::hle("kernel32.dll", "FindFirstFileW", {t::gwstr8("C:\\Users\\*"), data});
    CHECK(r.eax() != 0xFFFFFFFFu);
    bool saw_player = false;
    do { if (find_name(data) == "Player") saw_player = true; } while (t::hle("kernel32.dll", "FindNextFileW", {r.eax(), data}).eax());
    CHECK(saw_player);
}

TEST(attributes_and_full_paths) {
    t::setup_runtime("files");
    make_game_tree();
    CHECK_EQ(t::hle("kernel32.dll", "GetFileAttributesW", {t::gwstr8(gp_("DishonoredGame"))}).eax() & 0x10, 0x10u);
    CHECK_EQ(t::hle("kernel32.dll", "GetFileAttributesW", {t::gwstr8(gp_("Binaries\\Win32\\readme.txt"))}).eax() & 0x10, 0u);
    CHECK_EQ(t::hle("kernel32.dll", "GetFileAttributesW", {t::gwstr8(gp_("Nope.txt"))}).eax(), 0xFFFFFFFFu);
    CHECK_EQ(get_last_error(), 2u);
    CHECK_EQ(t::hle("kernel32.dll", "GetFileAttributesW", {t::gwstr8("C:\\")}).eax() & 0x10, 0x10u);
    uint32_t ex = t::galloc(40);
    CHECK_EQ(t::hle("kernel32.dll", "GetFileAttributesExW", {t::gwstr8(gp_("DishonoredGame\\CookedPCConsole\\Alpha.upk")), 0, ex}).eax(), 1u);
    CHECK_EQ(rd32(ex + 32), 1000u);
    CHECK(rd64(ex + 20) > 116444736000000000ull);                    // last-write FILETIME after 1970
    // canonical forms
    CHECK(vfs_canon("c:/foo/./bar/../baz") == "C:\\foo\\baz");
    CHECK(vfs_canon("\\\\?\\C:\\a\\b") == "C:\\a\\b");
    CHECK(vfs_canon("C:\\a\\b\\") == "C:\\a\\b");
    CHECK(vfs_canon("rel.txt") == vfs_getcwd() + "\\rel.txt");
    CHECK(vfs_canon("..\\x") == std::string(GAME) + "\\Binaries\\x");
    // GetFullPathName / GetCurrentDirectory
    uint32_t buf = t::galloc(1040), part = t::galloc(4);
    uint32_t n = t::hle("kernel32.dll", "GetFullPathNameW", {t::gwstr8("..\\..\\Foo\\bar.txt"), 520, buf, part}).eax();
    std::string full = utf16_to_utf8((uint16_t *)gp(buf), n);
    CHECK(full == std::string(GAME) + "\\Foo\\bar.txt");
    CHECK_EQ(rd32(part), buf + (uint32_t)(full.rfind('\\') + 1) * 2);
    CHECK_EQ(t::hle("kernel32.dll", "GetFullPathNameW", {t::gwstr8("..\\..\\Foo\\bar.txt"), 4, buf, 0}).eax(), (uint32_t)full.size() + 1);   // too small: needed size
    n = t::hle("kernel32.dll", "GetCurrentDirectoryW", {520, buf}).eax();
    CHECK(utf16_to_utf8((uint16_t *)gp(buf), n) == std::string(GAME) + "\\Binaries\\Win32");
    CHECK_EQ(t::hle("kernel32.dll", "SetCurrentDirectoryW", {t::gwstr8(gp_("DishonoredGame"))}).eax(), 1u);
    // relative open after chdir
    uint32_t h = create_file("Config\\default.ini", GR, 3);
    CHECK(h != 0xFFFFFFFFu);
    t::hle("kernel32.dll", "CloseHandle", {h});
    t::hle("kernel32.dll", "SetCurrentDirectoryW", {t::gwstr8(gp_("Binaries\\Win32"))});
    // known folders
    CHECK_EQ(t::hle("shell32.dll", "SHGetFolderPathW", {0, 0x8005, 0, 0, buf}).eax(), 0u);       // CSIDL_PERSONAL | CREATE
    CHECK(utf16_to_utf8((uint16_t *)gp(buf), gwcslen(buf)) == DOCS);
    CHECK_EQ(t::hle("shell32.dll", "SHGetFolderPathW", {0, 0x1C, 0, 0, buf}).eax(), 0u);
    CHECK(utf16_to_utf8((uint16_t *)gp(buf), gwcslen(buf)) == "C:\\Users\\Player\\AppData\\Local");
    n = t::hle("kernel32.dll", "GetTempPathW", {520, buf}).eax();
    CHECK(utf16_to_utf8((uint16_t *)gp(buf), n) == "C:\\Users\\Player\\AppData\\Local\\Temp\\");
}

TEST(overlapped_read_signals_event) {
    t::setup_runtime("files");
    make_game_tree();
    uint32_t h = create_file(gp_("DishonoredGame\\CookedPCConsole\\Alpha.upk"), GR, 3, 0x40000000);   // FILE_FLAG_OVERLAPPED
    CHECK(h != 0xFFFFFFFFu);
    uint32_t ev = t::hle("kernel32.dll", "CreateEventW", {0, 1, 0, 0}).eax();
    uint32_t ov = t::galloc(20), buf = t::galloc(100), n = t::galloc(4);
    wr32(ov + 8, 500);                                               // Offset
    wr32(ov + 16, ev);
    auto r = t::hle("kernel32.dll", "ReadFile", {h, buf, 100, 0, ov});
    CHECK_EQ(r.eax(), 1u);                                           // completes synchronously
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {ev, 0}).eax(), 0u);
    CHECK_EQ(rd32(ov + 4), 100u);
    CHECK_EQ(t::hle("kernel32.dll", "GetOverlappedResult", {h, ov, n, 1}).eax(), 1u);
    CHECK_EQ(rd32(n), 100u);
    CHECK_EQ(gp(buf)[99], (uint8_t)'a');
    // past EOF -> ERROR_HANDLE_EOF
    wr32(ov + 8, 5000);
    r = t::hle("kernel32.dll", "ReadFile", {h, buf, 100, 0, ov});
    CHECK_EQ(r.eax(), 0u);
    CHECK_EQ(get_last_error(), 38u);
    t::hle("kernel32.dll", "CloseHandle", {h});
}

TEST(large_offsets_and_get_file_size_ex) {
    t::setup_runtime("files");
    std::string dir = std::string(DOCS) + "\\big";
    t::hle("kernel32.dll", "CreateDirectoryW", {t::gwstr8(dir), 0});
    uint32_t h = create_file(dir + "\\sparse.bin", GR | GW, 2);
    CHECK(h != 0xFFFFFFFFu);
    uint32_t newpos = t::galloc(8);
    int64_t off = 5ll << 30;
    CHECK_EQ(t::hle("kernel32.dll", "SetFilePointerEx", {h, (uint32_t)off, (uint32_t)(off >> 32), newpos, 0}).eax(), 1u);
    CHECK_EQ((int64_t)rd64(newpos), off);
    uint32_t d = t::gstr("Z"), w = t::galloc(4);
    t::hle("kernel32.dll", "WriteFile", {h, d, 1, w, 0});
    uint32_t sz = t::galloc(8), hi = t::galloc(4);
    CHECK_EQ(t::hle("kernel32.dll", "GetFileSizeEx", {h, sz}).eax(), 1u);
    CHECK_EQ((int64_t)rd64(sz), off + 1);
    CHECK_EQ(t::hle("kernel32.dll", "GetFileSize", {h, hi}).eax(), (uint32_t)(off + 1));
    CHECK_EQ(rd32(hi), 1u);
    t::hle("kernel32.dll", "CloseHandle", {h});
    t::hle("kernel32.dll", "DeleteFileW", {t::gwstr8(dir + "\\sparse.bin")});
}

TEST(file_mapping_views) {
    t::setup_runtime("files");
    make_game_tree();
    uint32_t h = create_file(gp_("DishonoredGame\\CookedPCConsole\\beta.upk"), GR, 3);
    uint32_t m = t::hle("kernel32.dll", "CreateFileMappingW", {h, 0, 2 /*PAGE_READONLY*/, 0, 0, 0}).eax();
    CHECK(m != 0);
    uint32_t view = t::hle("kernel32.dll", "MapViewOfFile", {m, 4 /*FILE_MAP_READ*/, 0, 0, 0}).eax();
    CHECK(view != 0);
    CHECK_EQ(gp(view)[1999], (uint8_t)'b');
    CHECK_EQ(t::hle("kernel32.dll", "UnmapViewOfFile", {view}).eax(), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "UnmapViewOfFile", {view}).eax(), 0u);
    t::hle("kernel32.dll", "CloseHandle", {m});
    t::hle("kernel32.dll", "CloseHandle", {h});
    // anonymous named shared memory: two views of the same mapping alias
    uint32_t name = t::gwstr8("Local\\DishShm");
    uint32_t sm = t::hle("kernel32.dll", "CreateFileMappingW", {0xFFFFFFFFu, 0, 4 /*PAGE_READWRITE*/, 0, 65536, name}).eax();
    CHECK(sm != 0);
    uint32_t v1 = t::hle("kernel32.dll", "MapViewOfFile", {sm, 2, 0, 0, 0}).eax();
    uint32_t sm2 = t::hle("kernel32.dll", "CreateFileMappingW", {0xFFFFFFFFu, 0, 4, 0, 65536, name}).eax();
    CHECK_EQ(get_last_error(), 183u);
    uint32_t v2 = t::hle("kernel32.dll", "MapViewOfFile", {sm2, 2, 0, 0, 0}).eax();
    CHECK(v1 && v2 && v1 != v2);
    wr32(v1 + 100, 0xCAFEBABE);
    CHECK_EQ(rd32(v2 + 100), 0xCAFEBABEu);
    VmQuery q;
    vm_query(v1, &q);
    CHECK_EQ(q.type, (uint32_t)MEM_MAPPED);
    t::hle("kernel32.dll", "UnmapViewOfFile", {v1});
    t::hle("kernel32.dll", "UnmapViewOfFile", {v2});
}

TEST(std_handles_and_file_type) {
    t::setup_runtime("files");
    uint32_t out = t::hle("kernel32.dll", "GetStdHandle", {(uint32_t)-11}).eax();
    CHECK(out != 0 && out != 0xFFFFFFFFu);
    CHECK_EQ(t::hle("kernel32.dll", "GetStdHandle", {(uint32_t)-11}).eax(), out);
    uint32_t ft = t::hle("kernel32.dll", "GetFileType", {out}).eax();
    CHECK(ft == 1 || ft == 2);
    uint32_t h = create_file(gp_("Binaries\\Win32\\ReadMe.TXT"), GR, 3);
    CHECK_EQ(t::hle("kernel32.dll", "GetFileType", {h}).eax(), 1u);
    t::hle("kernel32.dll", "CloseHandle", {h});
}
