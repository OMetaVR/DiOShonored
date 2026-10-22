#include "testing.h"
#include <cstring>

using namespace dish;

static std::u16string w(const char16_t *s) { return s; }

TEST(utf_helpers) {
    CHECK(utf16_to_utf8(w(u"héllo € \U0001F600")) == "h\xc3\xa9llo \xe2\x82\xac \xf0\x9f\x98\x80");
    CHECK(utf8_to_utf16("h\xc3\xa9llo \xe2\x82\xac \xf0\x9f\x98\x80") == w(u"héllo € \U0001F600"));
    CHECK(utf8_to_utf16("\xff\xfe").size() == 2);   // invalid bytes become U+FFFD, never crash
    char16_t lone[] = {0xD800, 0};
    CHECK(utf16_to_utf8((const uint16_t *)lone, 1) == "\xef\xbf\xbd");
    CHECK_EQ(sizeof(char16_t), 2u);
    CHECK(u16_lower(0xC9) == 0xE9 && u16_upper(0xE9) == 0xC9);
    CHECK(u16_lower(0x41) == 0x61 && u16_lower(0x416) == 0x436);
    CHECK(lower_utf8("ABC\xc3\x89") == "abc\xc3\xa9");
}

TEST(multibyte_to_widechar) {
    t::setup_runtime("str");
    uint32_t src = t::gstr("Caf\xe9 \x80 ok");     // cp1252: e9 = e-acute, 80 = euro
    uint32_t dst = t::galloc(64);
    auto r = t::hle("kernel32.dll", "MultiByteToWideChar", {1252, 0, src, (uint32_t)-1, dst, 32});
    CHECK_EQ(r.eax(), 10u);                           // 9 chars + NUL
    CHECK_EQ(r.pop(), 24u);
    CHECK_EQ(rd16(dst + 6), 0xE9);
    CHECK_EQ(rd16(dst + 10), 0x20AC);
    // size query
    CHECK_EQ(t::hle("kernel32.dll", "MultiByteToWideChar", {0, 0, src, 4, 0, 0}).eax(), 4u);
    // buffer too small
    CHECK_EQ(t::hle("kernel32.dll", "MultiByteToWideChar", {1252, 0, src, (uint32_t)-1, dst, 3}).eax(), 0u);
    CHECK_EQ(get_last_error(), 122u);
    // UTF-8 including a 4-byte sequence
    uint32_t u8 = t::gstr("a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80");
    auto r2 = t::hle("kernel32.dll", "MultiByteToWideChar", {65001, 0, u8, (uint32_t)-1, dst, 32});
    CHECK_EQ(r2.eax(), 6u);                           // a, e-acute, euro, surrogate pair (2 units), NUL
    CHECK_EQ(rd16(dst + 6), 0xD83D);
    CHECK_EQ(rd16(dst + 8), 0xDE00);
    // strict mode rejects malformed UTF-8
    uint32_t bad = t::gstr("a\xff");
    CHECK_EQ(t::hle("kernel32.dll", "MultiByteToWideChar", {65001, 8, bad, (uint32_t)-1, dst, 32}).eax(), 0u);
    CHECK_EQ(get_last_error(), 1113u);
    // unsupported code page
    CHECK_EQ(t::hle("kernel32.dll", "MultiByteToWideChar", {99999, 0, src, 4, dst, 32}).eax(), 0u);
}

TEST(widechar_to_multibyte) {
    t::setup_runtime("str");
    uint32_t src = t::gwstr(u"Café € 中");
    uint32_t dst = t::galloc(64), used = t::galloc(4);
    auto r = t::hle("kernel32.dll", "WideCharToMultiByte", {1252, 0, src, (uint32_t)-1, dst, 64, 0, used});
    CHECK_EQ(r.pop(), 32u);
    CHECK_EQ(r.eax(), 9u);                             // "Caf\xe9 \x80 ?" + NUL
    CHECK(strcmp((char *)gp(dst), "Caf\xe9 \x80 ?") == 0);
    CHECK_EQ(rd32(used), 1u);                          // the CJK character had no mapping
    auto u = t::hle("kernel32.dll", "WideCharToMultiByte", {65001, 0, src, (uint32_t)-1, dst, 64, 0, 0});
    CHECK_EQ(u.eax(), 14u);   // 13 bytes plus the terminator
    CHECK(strcmp((char *)gp(dst), "Caf\xc3\xa9 \xe2\x82\xac \xe4\xb8\xad") == 0);
    CHECK_EQ(t::hle("kernel32.dll", "WideCharToMultiByte", {65001, 0, src, (uint32_t)-1, 0, 0, 0, 0}).eax(), 14u);
    CHECK_EQ(t::hle("kernel32.dll", "WideCharToMultiByte", {65001, 0, src, (uint32_t)-1, dst, 4, 0, 0}).eax(), 0u);
    CHECK_EQ(get_last_error(), 122u);
    // explicit length without terminator
    auto e = t::hle("kernel32.dll", "WideCharToMultiByte", {1252, 0, src, 3, dst, 64, 0, 0});
    CHECK_EQ(e.eax(), 3u);
}

TEST(compare_and_map) {
    t::setup_runtime("str");
    uint32_t a = t::gwstr(u"Hello"), b = t::gwstr(u"hello"), c = t::gwstr(u"World");
    auto cmp = [&](uint32_t flags, uint32_t x, uint32_t y) { return t::hle("kernel32.dll", "CompareStringW", {0x409, flags, x, (uint32_t)-1, y, (uint32_t)-1}).eax(); };
    CHECK_EQ(cmp(1, a, b), 2u);                        // NORM_IGNORECASE: equal
    CHECK_EQ(cmp(0, a, b), 3u);                        // uppercase sorts after lowercase on ties
    CHECK_EQ(cmp(1, a, c), 1u);
    CHECK_EQ(cmp(1, c, a), 3u);
    CHECK_EQ(cmp(0, a, a), 2u);
    uint32_t ca = t::gstr("abc"), cb = t::gstr("ABD");
    CHECK_EQ(t::hle("kernel32.dll", "CompareStringA", {0x409, 1, ca, 3, cb, 3}).eax(), 1u);
    uint32_t dst = t::galloc(32);
    CHECK_EQ(t::hle("kernel32.dll", "LCMapStringW", {0x409, 0x200, b, (uint32_t)-1, dst, 16}).eax(), 6u);
    CHECK(utf16_to_utf8((uint16_t *)gp(dst), 5) == "HELLO");
    CHECK_EQ(t::hle("kernel32.dll", "LCMapStringW", {0x409, 0x100, a, 5, dst, 16}).eax(), 5u);
    CHECK(utf16_to_utf8((uint16_t *)gp(dst), 5) == "hello");
    CHECK_EQ(t::hle("kernel32.dll", "LCMapStringA", {0x409, 0x200, ca, (uint32_t)-1, dst, 16}).eax(), 4u);
    CHECK(strcmp((char *)gp(dst), "ABC") == 0);
    CHECK_EQ(t::hle("kernel32.dll", "lstrlenW", {a}).eax(), 5u);
    CHECK_EQ(t::hle("kernel32.dll", "lstrlenA", {ca}).eax(), 3u);
    CHECK_EQ(t::hle("kernel32.dll", "lstrlenA", {0}).eax(), 0u);
}

TEST(string_types_and_locale) {
    t::setup_runtime("str");
    uint32_t s = t::gwstr(u"aZ5 !\t");
    uint32_t out = t::galloc(32);
    CHECK_EQ(t::hle("kernel32.dll", "GetStringTypeW", {1, s, 6, out}).eax(), 1u);
    CHECK(rd16(out) & 0x2);                            // C1_LOWER
    CHECK(rd16(out + 2) & 0x1);                        // C1_UPPER
    CHECK(rd16(out + 4) & 0x4);                        // C1_DIGIT
    CHECK(rd16(out + 6) & 0x8);                        // C1_SPACE
    CHECK(rd16(out + 8) & 0x10);                       // C1_PUNCT
    CHECK(rd16(out + 10) & 0x20);                      // C1_CNTRL
    CHECK_EQ(t::hle("kernel32.dll", "GetACP", {}).eax(), 1252u);
    uint32_t cp = t::galloc(20);
    CHECK_EQ(t::hle("kernel32.dll", "GetCPInfo", {1252, cp}).eax(), 1u);
    CHECK_EQ(rd32(cp), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "IsValidCodePage", {1252}).eax(), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "IsValidCodePage", {123456}).eax(), 0u);
    uint32_t buf = t::galloc(64);
    auto li = t::hle("kernel32.dll", "GetLocaleInfoW", {0x409, 0x1004, buf, 16});   // LOCALE_IDEFAULTANSICODEPAGE
    CHECK_EQ(li.eax(), 5u);
    CHECK(utf16_to_utf8((uint16_t *)gp(buf), 4) == "1252");
    CHECK_EQ(t::hle("kernel32.dll", "GetLocaleInfoA", {0x409, 0x0E, buf, 0}).eax(), 2u);   // size query for "."
    CHECK_EQ(t::hle("kernel32.dll", "GetUserDefaultLCID", {}).eax(), 0x409u);
}

TEST(registry_in_memory_store) {
    t::setup_runtime("str");
    uint32_t hkey = t::galloc(4), disp = t::galloc(4);
    // create / set / query / reopen
    auto r = t::hle("advapi32.dll", "RegCreateKeyExW", {0x80000001u, t::gwstr8("Software\\Arkane\\Dishonored"), 0, 0, 0, 0xF003F, 0, hkey, disp});
    CHECK_EQ(r.eax(), 0u);
    CHECK_EQ(r.pop(), 36u);
    CHECK_EQ(rd32(disp), 1u);
    uint32_t k = rd32(hkey);
    uint32_t val = t::gwstr(u"Dishonored");
    CHECK_EQ(t::hle("advapi32.dll", "RegSetValueExW", {k, t::gwstr8("InstallPath"), 0, 1, val, 22}).eax(), 0u);
    uint32_t dw = t::galloc(4);
    wr32(dw, 1920);
    t::hle("advapi32.dll", "RegSetValueExW", {k, t::gwstr8("ResX"), 0, 4, dw, 4});
    uint32_t type = t::galloc(4), cb = t::galloc(4), data = t::galloc(64);
    wr32(cb, 64);
    CHECK_EQ(t::hle("advapi32.dll", "RegQueryValueExW", {k, t::gwstr8("installpath"), 0, type, data, cb}).eax(), 0u);   // names are case-insensitive
    CHECK_EQ(rd32(type), 1u);
    CHECK_EQ(rd32(cb), 22u);
    CHECK(utf16_to_utf8((uint16_t *)gp(data), 10) == "Dishonored");
    wr32(cb, 4);
    CHECK_EQ(t::hle("advapi32.dll", "RegQueryValueExW", {k, t::gwstr8("InstallPath"), 0, type, data, cb}).eax(), 234u);   // ERROR_MORE_DATA
    CHECK_EQ(rd32(cb), 22u);
    wr32(cb, 64);
    CHECK_EQ(t::hle("advapi32.dll", "RegQueryValueExW", {k, t::gwstr8("Missing"), 0, type, data, cb}).eax(), 2u);
    // ANSI access converts the string
    wr32(cb, 64);
    CHECK_EQ(t::hle("advapi32.dll", "RegQueryValueExA", {k, t::gstr("InstallPath"), 0, type, data, cb}).eax(), 0u);
    CHECK_EQ(rd32(cb), 11u);
    CHECK(strcmp((char *)gp(data), "Dishonored") == 0);
    // enumeration
    uint32_t name = t::galloc(64), cname = t::galloc(4);
    wr32(cname, 32);
    CHECK_EQ(t::hle("advapi32.dll", "RegEnumValueW", {k, 0, name, cname, 0, type, 0, 0}).eax(), 0u);
    wr32(cname, 32);
    CHECK_EQ(t::hle("advapi32.dll", "RegEnumValueW", {k, 2, name, cname, 0, type, 0, 0}).eax(), 259u);
    t::hle("advapi32.dll", "RegCloseKey", {k});
    // reopen shows the data; opening a missing key fails
    CHECK_EQ(t::hle("advapi32.dll", "RegOpenKeyExW", {0x80000001u, t::gwstr8("software\\arkane\\dishonored"), 0, 0x20019, hkey}).eax(), 0u);
    CHECK_EQ(t::hle("advapi32.dll", "RegOpenKeyExW", {0x80000001u, t::gwstr8("Software\\Nobody"), 0, 0x20019, hkey}).eax(), 2u);
    CHECK_EQ(t::hle("advapi32.dll", "RegCloseKey", {0xDEAD0}).eax(), 6u);
}
