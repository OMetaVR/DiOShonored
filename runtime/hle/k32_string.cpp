// kernel32: string conversion, comparison, locale.
#include "dish/call.h"
#include "dish/heap.h"
#include "dish/hle.h"
#include "dish/thread.h"
#include "dish/util.h"
#include <cstring>

namespace dish {
namespace {

constexpr uint32_t MB_ERR_INVALID_CHARS = 8;

uint32_t k32_MultiByteToWideChar(uint32_t cp, uint32_t flags, uint32_t src, int32_t cb, uint32_t dst, int32_t cch) {
    if (!src || cb == 0 || cch < 0 || (!dst && cch > 0)) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    if (!cp_supported(cp)) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    size_t n = cb < 0 ? gstrlen(src) + 1 : (size_t)cb;
    const uint8_t *s = guest_base + src;
    int64_t r = mb_to_wide(cp, s, n, dst ? (uint16_t *)(guest_base + dst) : nullptr, (size_t)cch, (flags & MB_ERR_INVALID_CHARS) != 0);
    if (r == -2) { set_last_error(122); return 0; }   // ERROR_INSUFFICIENT_BUFFER
    if (r < 0) { set_last_error(1113); return 0; }    // ERROR_NO_UNICODE_TRANSLATION
    return (uint32_t)r;
}

uint32_t k32_WideCharToMultiByte(uint32_t cp, uint32_t flags, uint32_t src, int32_t cw, uint32_t dst, int32_t cb, uint32_t defchar, uint32_t used) {
    if (!src || cw == 0 || cb < 0 || (!dst && cb > 0)) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    if (!cp_supported(cp)) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    size_t n = cw < 0 ? gwcslen(src) + 1 : (size_t)cw;
    bool used_def = false;
    uint8_t dc = defchar ? rd8(defchar) : '?';
    int64_t r = wide_to_mb(cp, (const uint16_t *)(guest_base + src), n, dst ? guest_base + dst : nullptr, (size_t)cb, &used_def, dc);
    if (r == -2) { set_last_error(122); return 0; }
    if (r < 0) { set_last_error(1113); return 0; }
    if (used) wr32(used, used_def ? 1 : 0);
    (void)flags;
    return (uint32_t)r;
}

uint32_t k32_lstrlenA(uint32_t p) { return p ? (uint32_t)gstrlen(p) : 0; }
uint32_t k32_lstrlenW(uint32_t p) { return p ? (uint32_t)gwcslen(p) : 0; }
uint32_t k32_lstrcpyA(uint32_t d, uint32_t s) { memcpy(guest_base + d, guest_base + s, gstrlen(s) + 1); return d; }
uint32_t k32_lstrcpyW(uint32_t d, uint32_t s) { memcpy(guest_base + d, guest_base + s, (gwcslen(s) + 1) * 2); return d; }
uint32_t k32_lstrcpynA(uint32_t d, uint32_t s, int32_t n) {
    if (n <= 0) return 0;
    size_t l = std::min<size_t>(gstrlen(s), (size_t)n - 1);
    memcpy(guest_base + d, guest_base + s, l);
    guest_base[d + l] = 0;
    return d;
}
uint32_t k32_lstrcpynW(uint32_t d, uint32_t s, int32_t n) {
    if (n <= 0) return 0;
    size_t l = std::min<size_t>(gwcslen(s), (size_t)n - 1);
    memcpy(guest_base + d, guest_base + s, l * 2);
    wr16(d + (uint32_t)l * 2, 0);
    return d;
}
uint32_t k32_lstrcatA(uint32_t d, uint32_t s) { memcpy(guest_base + d + gstrlen(d), guest_base + s, gstrlen(s) + 1); return d; }
uint32_t k32_lstrcatW(uint32_t d, uint32_t s) { memcpy(guest_base + d + gwcslen(d) * 2, guest_base + s, (gwcslen(s) + 1) * 2); return d; }

int cmp_units(const uint16_t *a, size_t na, const uint16_t *b, size_t nb, bool icase) {
    size_t n = std::min(na, nb);
    for (size_t i = 0; i < n; i++) {
        uint16_t x = a[i], y = b[i];
        if (icase) { x = u16_lower(x); y = u16_lower(y); }
        if (x != y) return x < y ? -1 : 1;
    }
    return na == nb ? 0 : (na < nb ? -1 : 1);
}
int32_t k32_lstrcmpA(uint32_t a, uint32_t b) { return strcmp(gs(a), gs(b)) < 0 ? -1 : strcmp(gs(a), gs(b)) > 0 ? 1 : 0; }
int32_t k32_lstrcmpiA(uint32_t a, uint32_t b) { return strcasecmp(gs(a), gs(b)); }
int32_t k32_lstrcmpW(uint32_t a, uint32_t b) { return cmp_units(gw(a), gwcslen(a), gw(b), gwcslen(b), false); }
int32_t k32_lstrcmpiW(uint32_t a, uint32_t b) { return cmp_units(gw(a), gwcslen(a), gw(b), gwcslen(b), true); }

// CompareString: NORM_IGNORECASE 1, NORM_IGNORENONSPACE 2, NORM_IGNORESYMBOLS 4, SORT_STRINGSORT 0x1000
uint32_t compare_w(uint32_t flags, const uint16_t *a, int32_t na, const uint16_t *b, int32_t nb) {
    size_t la = na < 0 ? 0 : (size_t)na, lb = nb < 0 ? 0 : (size_t)nb;
    if (na < 0) while (a[la]) la++;
    if (nb < 0) while (b[lb]) lb++;
    bool icase = flags & 1;
    int r = cmp_units(a, la, b, lb, icase);
    if (r == 0 && !icase) return 2;
    if (r == 0) return 2;
    // Windows collation orders letters alphabetically ignoring case first, lowercase before uppercase on ties.
    if (!icase) {
        int r2 = cmp_units(a, la, b, lb, true);
        if (r2 != 0) return r2 < 0 ? 1 : 3;
        // same ignoring case: lowercase sorts first
        size_t n = std::min(la, lb);
        for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return (u16_lower(a[i]) == a[i]) ? 1 : 3;
    }
    return r < 0 ? 1 : 3;
}
uint32_t k32_CompareStringW(uint32_t, uint32_t flags, uint32_t a, int32_t na, uint32_t b, int32_t nb) {
    if (!a || !b) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    return compare_w(flags, gw(a), na, gw(b), nb);
}
uint32_t k32_CompareStringA(uint32_t, uint32_t flags, uint32_t a, int32_t na, uint32_t b, int32_t nb) {
    if (!a || !b) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    size_t la = na < 0 ? gstrlen(a) : (size_t)na, lb = nb < 0 ? gstrlen(b) : (size_t)nb;
    std::u16string wa(la, 0), wb(lb, 0);
    mb_to_wide(1252, guest_base + a, la, (uint16_t *)wa.data(), la, false);
    mb_to_wide(1252, guest_base + b, lb, (uint16_t *)wb.data(), lb, false);
    return compare_w(flags, (const uint16_t *)wa.data(), (int32_t)la, (const uint16_t *)wb.data(), (int32_t)lb);
}

// LCMapString: LCMAP_LOWERCASE 0x100, LCMAP_UPPERCASE 0x200, LCMAP_SORTKEY 0x400, LCMAP_HALFWIDTH/FULLWIDTH ignored
uint32_t k32_LCMapStringW(uint32_t, uint32_t flags, uint32_t src, int32_t cs, uint32_t dst, int32_t cd) {
    if (!src || cs == 0 || cd < 0) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    size_t n = cs < 0 ? gwcslen(src) + 1 : (size_t)cs;
    if (flags & 0x400) {   // sort key: bytes of the case-folded string, good enough for ordering
        size_t need = n * 2;
        if (cd == 0) return (uint32_t)need;
        if ((size_t)cd < need) { set_last_error(122); return 0; }
        for (size_t i = 0; i < n; i++) { uint16_t c = u16_lower(rd16(src + 2 * (uint32_t)i)); guest_base[dst + 2 * i] = c >> 8; guest_base[dst + 2 * i + 1] = (uint8_t)c; }
        return (uint32_t)need;
    }
    if (cd == 0) return (uint32_t)n;
    if ((size_t)cd < n) { set_last_error(122); return 0; }
    for (size_t i = 0; i < n; i++) {
        uint16_t c = rd16(src + 2 * (uint32_t)i);
        if (flags & 0x100) c = u16_lower(c);
        else if (flags & 0x200) c = u16_upper(c);
        wr16(dst + 2 * (uint32_t)i, c);
    }
    return (uint32_t)n;
}
uint32_t k32_LCMapStringA(uint32_t lcid, uint32_t flags, uint32_t src, int32_t cs, uint32_t dst, int32_t cd) {
    if (!src || cs == 0 || cd < 0) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    size_t n = cs < 0 ? gstrlen(src) + 1 : (size_t)cs;
    if (flags & 0x400) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    if (cd == 0) return (uint32_t)n;
    if ((size_t)cd < n) { set_last_error(122); return 0; }
    for (size_t i = 0; i < n; i++) {
        uint8_t c = guest_base[src + i];
        if (flags & 0x100) { if (c >= 'A' && c <= 'Z') c += 32; else if (c >= 0xC0 && c <= 0xDE && c != 0xD7) c += 32; }
        else if (flags & 0x200) { if (c >= 'a' && c <= 'z') c -= 32; else if (c >= 0xE0 && c <= 0xFE && c != 0xF7) c -= 32; }
        guest_base[dst + i] = c;
    }
    (void)lcid;
    return (uint32_t)n;
}

// GetStringType: CT_CTYPE1 = 1, CT_CTYPE2 = 2, CT_CTYPE3 = 4
uint32_t k32_GetStringTypeW(uint32_t type, uint32_t src, int32_t cch, uint32_t out) {
    if (!src || !out) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    size_t n = cch < 0 ? gwcslen(src) + 1 : (size_t)cch;
    for (size_t i = 0; i < n; i++) {
        uint16_t c = rd16(src + 2 * (uint32_t)i);
        uint16_t f = 0;
        if (type == 1) f = u16_ctype(c);
        else if (type == 2) f = 0x1;   // C2_LEFTTORIGHT
        else if (type == 4) f = (c < 0x80 && !(u16_ctype(c) & 0x100)) ? 0x0 : 0x0;
        wr16(out + 2 * (uint32_t)i, f);
    }
    return 1;
}
uint32_t k32_GetStringTypeA(uint32_t, uint32_t type, uint32_t src, int32_t cch, uint32_t out) {
    if (!src || !out) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    size_t n = cch < 0 ? gstrlen(src) + 1 : (size_t)cch;
    for (size_t i = 0; i < n; i++) {
        uint16_t w;
        mb_to_wide(1252, guest_base + src + i, 1, &w, 1, false);
        wr16(out + 2 * (uint32_t)i, type == 1 ? u16_ctype(w) : 1);
    }
    return 1;
}

uint32_t k32_GetCPInfo(uint32_t cp, uint32_t out) {
    cp = cp_resolve(cp);
    if (!cp_supported(cp)) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    memset(guest_base + out, 0, 20);
    wr32(out, cp == 65001 ? 4 : 1);
    guest_base[out + 4] = '?';
    return 1;
}
uint32_t k32_IsValidCodePage(uint32_t cp) { return cp_supported(cp); }
uint32_t k32_GetACP() { return 1252; }
uint32_t k32_GetOEMCP() { return 437; }
uint32_t k32_GetConsoleCP() { return 437; }
uint32_t k32_GetConsoleOutputCP() { return 437; }
uint32_t k32_IsDBCSLeadByteEx(uint32_t, uint32_t) { return 0; }
uint32_t k32_IsDBCSLeadByte(uint32_t) { return 0; }
uint32_t k32_GetUserDefaultLCID() { return 0x409; }
uint32_t k32_GetUserDefaultUILanguage() { return 0x409; }
uint32_t k32_GetUserDefaultLangID() { return 0x409; }
uint32_t k32_GetSystemDefaultLCID() { return 0x409; }
uint32_t k32_GetSystemDefaultLangID() { return 0x409; }
uint32_t k32_GetThreadLocale() { return 0x409; }
uint32_t k32_SetThreadLocale(uint32_t) { return 1; }
uint32_t k32_IsValidLocale(uint32_t lcid, uint32_t) { return (lcid & 0xFFFF) == 0x409 || lcid == 0x800 || lcid == 0x400; }

const char *locale_str(uint32_t lctype) {
    switch (lctype & 0x0FFFFFFF) {
        case 0x01: return "0409";             // LOCALE_ILANGUAGE
        case 0x02: return "English (United States)";   // SLANGUAGE
        case 0x03: return "ENU";              // SABBREVLANGNAME
        case 0x04: return "English";          // SNATIVELANGNAME
        case 0x05: return "United States";    // SCOUNTRY
        case 0x06: return "1";                // ICOUNTRY
        case 0x07: return "USA";              // SABBREVCTRYNAME
        case 0x08: return "United States";    // SNATIVECTRYNAME
        case 0x09: return "9";                // IDEFAULTLANGUAGE
        case 0x0A: return "1";                // IDEFAULTCOUNTRY
        case 0x0B: return "437";              // IDEFAULTCODEPAGE
        case 0x0C: return ",";                // SLIST
        case 0x0D: return "0";                // IMEASURE
        case 0x0E: return ".";                // SDECIMAL
        case 0x0F: return ",";                // STHOUSAND
        case 0x10: return "3;0";              // SGROUPING
        case 0x11: return "2";                // IDIGITS
        case 0x12: return "1";                // ILZERO
        case 0x13: return "1";                // INEGNUMBER
        case 0x14: return "$";                // SCURRENCY
        case 0x15: return "USD";              // SINTLSYMBOL
        case 0x16: return ".";                // SMONDECIMALSEP
        case 0x17: return ",";                // SMONTHOUSANDSEP
        case 0x18: return "3;0";              // SMONGROUPING
        case 0x19: return "2";                // ICURRDIGITS
        case 0x1A: return "0";                // ICURRENCY
        case 0x1B: return "0";                // INEGCURR
        case 0x1C: return "M/d/yyyy";         // SSHORTDATE (0x1F)
        case 0x1D: return ":";                // STIME
        case 0x1E: return "/";                // SDATE
        case 0x1F: return "M/d/yyyy";         // SSHORTDATE
        case 0x20: return "dddd, MMMM dd, yyyy";   // SLONGDATE
        case 0x21: return "h:mm:ss tt";       // STIMEFORMAT
        case 0x22: return "0";                // IDATE
        case 0x23: return "0";                // ILDATE
        case 0x24: return "0";                // ITIME
        case 0x25: return "0";                // ICENTURY
        case 0x26: return "0";                // ITLZERO
        case 0x28: return "AM";               // S1159
        case 0x29: return "PM";               // S2359
        case 0x2A: return "0";                // ICALENDARTYPE
        case 0x1001: return "English";        // SENGLANGUAGE
        case 0x1002: return "United States";  // SENGCOUNTRY
        case 0x1003: return "0";              // IPAPERSIZE
        case 0x1004: return "1252";           // IDEFAULTANSICODEPAGE
        case 0x1005: return "United States";  // SENGCURRNAME
        case 0x1006: return "0";
        case 0x1007: return "0";
        case 0x1009: return "0";
        case 0x1010: return "0";
        case 0x58: return "0";
        case 0x59: return "en";               // SISO639LANGNAME
        case 0x5A: return "US";               // SISO3166CTRYNAME
        case 0x5C: return "United States";    // SNATIVECURRNAME
        case 0x5D: return "en-US";            // SNAME (0x5C in some)
        case 0x6F: return "Gregorian";
        default: return nullptr;
    }
}
uint32_t locale_info(uint32_t lctype, std::u16string *w) {
    const char *s = locale_str(lctype);
    if ((lctype & 0x0FFFFFFF) == 0x5C) s = "$";
    if (!s) s = "";
    *w = utf8_to_utf16(s);
    return 0;
}
uint32_t k32_GetLocaleInfoW(uint32_t lcid, uint32_t lctype, uint32_t buf, int32_t cch) {
    (void)lcid;
    std::u16string w;
    locale_info(lctype, &w);
    if (lctype & 0x20000000) {   // LOCALE_RETURN_NUMBER
        if (cch < 2) { set_last_error(122); return 0; }
        wr32(buf, (uint32_t)strtoul(utf16_to_utf8(w).c_str(), nullptr, 10));
        return 2;
    }
    size_t need = w.size() + 1;
    if (cch == 0) return (uint32_t)need;
    if ((size_t)cch < need) { set_last_error(122); return 0; }
    write_wstr(buf, (size_t)cch, w);
    return (uint32_t)need;
}
uint32_t k32_GetLocaleInfoA(uint32_t lcid, uint32_t lctype, uint32_t buf, int32_t cch) {
    (void)lcid;
    std::u16string w;
    locale_info(lctype, &w);
    std::string s = utf16_to_utf8(w);
    if (lctype & 0x20000000) {
        if (cch < 4) { set_last_error(122); return 0; }
        wr32(buf, (uint32_t)strtoul(s.c_str(), nullptr, 10));
        return 4;
    }
    size_t need = s.size() + 1;
    if (cch == 0) return (uint32_t)need;
    if ((size_t)cch < need) { set_last_error(122); return 0; }
    write_astr(buf, (size_t)cch, s);
    return (uint32_t)need;
}
uint32_t k32_EnumSystemLocalesA(const HleCtx &c, uint32_t proc, uint32_t flags) {
    (void)flags;
    uint32_t buf = heap_alloc(heap_process(), 0, 16);
    memcpy(guest_base + buf, "00000409", 9);
    guest_invoke_from(c.esp, proc, Conv::Stdcall, buf);
    heap_free(heap_process(), 0, buf);
    return 1;
}
uint32_t k32_GetNumberFormatA(uint32_t, uint32_t, uint32_t val, uint32_t, uint32_t buf, int32_t cch) {
    std::string s = read_astr(val);
    if (cch == 0) return (uint32_t)s.size() + 1;
    if ((size_t)cch < s.size() + 1) { set_last_error(122); return 0; }
    write_astr(buf, (size_t)cch, s);
    return (uint32_t)s.size() + 1;
}
uint32_t k32_FoldStringW(uint32_t, uint32_t src, int32_t cs, uint32_t dst, int32_t cd) {
    size_t n = cs < 0 ? gwcslen(src) + 1 : (size_t)cs;
    if (cd == 0) return (uint32_t)n;
    if ((size_t)cd < n) { set_last_error(122); return 0; }
    memcpy(guest_base + dst, guest_base + src, n * 2);
    return (uint32_t)n;
}
uint32_t k32_CharUpperW_impl(uint32_t p) {
    if (p < 0x10000) return u16_upper((uint16_t)p);
    for (uint32_t a = p; rd16(a); a += 2) wr16(a, u16_upper(rd16(a)));
    return p;
}
uint32_t k32_CharLowerW_impl(uint32_t p) {
    if (p < 0x10000) return u16_lower((uint16_t)p);
    for (uint32_t a = p; rd16(a); a += 2) wr16(a, u16_lower(rd16(a)));
    return p;
}

}  // namespace

HLE("kernel32.dll", "MultiByteToWideChar", Stdcall, k32_MultiByteToWideChar);
HLE("kernel32.dll", "WideCharToMultiByte", Stdcall, k32_WideCharToMultiByte);
HLE("kernel32.dll", "lstrlenA", Stdcall, k32_lstrlenA);
HLE("kernel32.dll", "lstrlenW", Stdcall, k32_lstrlenW);
HLE("kernel32.dll", "lstrcpyA", Stdcall, k32_lstrcpyA);
HLE("kernel32.dll", "lstrcpyW", Stdcall, k32_lstrcpyW);
HLE("kernel32.dll", "lstrcpynA", Stdcall, k32_lstrcpynA);
HLE("kernel32.dll", "lstrcpynW", Stdcall, k32_lstrcpynW);
HLE("kernel32.dll", "lstrcatA", Stdcall, k32_lstrcatA);
HLE("kernel32.dll", "lstrcatW", Stdcall, k32_lstrcatW);
HLE("kernel32.dll", "lstrcmpA", Stdcall, k32_lstrcmpA);
HLE("kernel32.dll", "lstrcmpiA", Stdcall, k32_lstrcmpiA);
HLE("kernel32.dll", "lstrcmpW", Stdcall, k32_lstrcmpW);
HLE("kernel32.dll", "lstrcmpiW", Stdcall, k32_lstrcmpiW);
HLE("kernel32.dll", "CompareStringW", Stdcall, k32_CompareStringW);
HLE("kernel32.dll", "CompareStringA", Stdcall, k32_CompareStringA);
HLE("kernel32.dll", "LCMapStringW", Stdcall, k32_LCMapStringW);
HLE("kernel32.dll", "LCMapStringA", Stdcall, k32_LCMapStringA);
HLE("kernel32.dll", "GetStringTypeW", Stdcall, k32_GetStringTypeW);
HLE("kernel32.dll", "GetStringTypeA", Stdcall, k32_GetStringTypeA);
HLE("kernel32.dll", "GetCPInfo", Stdcall, k32_GetCPInfo);
HLE("kernel32.dll", "IsValidCodePage", Stdcall, k32_IsValidCodePage);
HLE("kernel32.dll", "GetACP", Stdcall, k32_GetACP);
HLE("kernel32.dll", "GetOEMCP", Stdcall, k32_GetOEMCP);
HLE("kernel32.dll", "GetConsoleCP", Stdcall, k32_GetConsoleCP);
HLE("kernel32.dll", "GetConsoleOutputCP", Stdcall, k32_GetConsoleOutputCP);
HLE("kernel32.dll", "IsDBCSLeadByteEx", Stdcall, k32_IsDBCSLeadByteEx);
HLE("kernel32.dll", "IsDBCSLeadByte", Stdcall, k32_IsDBCSLeadByte);
HLE("kernel32.dll", "GetUserDefaultLCID", Stdcall, k32_GetUserDefaultLCID);
HLE("kernel32.dll", "GetUserDefaultUILanguage", Stdcall, k32_GetUserDefaultUILanguage);
HLE("kernel32.dll", "GetUserDefaultLangID", Stdcall, k32_GetUserDefaultLangID);
HLE("kernel32.dll", "GetSystemDefaultLCID", Stdcall, k32_GetSystemDefaultLCID);
HLE("kernel32.dll", "GetSystemDefaultLangID", Stdcall, k32_GetSystemDefaultLangID);
HLE("kernel32.dll", "GetThreadLocale", Stdcall, k32_GetThreadLocale);
HLE("kernel32.dll", "SetThreadLocale", Stdcall, k32_SetThreadLocale);
HLE("kernel32.dll", "IsValidLocale", Stdcall, k32_IsValidLocale);
HLE("kernel32.dll", "GetLocaleInfoW", Stdcall, k32_GetLocaleInfoW);
HLE("kernel32.dll", "GetLocaleInfoA", Stdcall, k32_GetLocaleInfoA);
HLE("kernel32.dll", "EnumSystemLocalesA", Stdcall, k32_EnumSystemLocalesA);
HLE("kernel32.dll", "GetNumberFormatA", Stdcall, k32_GetNumberFormatA);
HLE("kernel32.dll", "FoldStringW", Stdcall, k32_FoldStringW);
HLE("user32.dll", "CharUpperW", Stdcall, k32_CharUpperW_impl);
HLE("user32.dll", "CharLowerW", Stdcall, k32_CharLowerW_impl);

}  // namespace dish
