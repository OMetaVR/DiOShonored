#include "dish/util.h"
#include <cstring>

namespace dish {

std::string utf16_to_utf8(const uint16_t *s, size_t n) {
    std::string o;
    o.reserve(n);
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD;
        }
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    }
    return o;
}

std::u16string utf8_to_utf16(std::string_view s) {
    std::u16string o;
    o.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        uint8_t b = (uint8_t)s[i];
        uint32_t c;
        int len;
        if (b < 0x80) { c = b; len = 1; }
        else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0) { c = b & 0x07; len = 4; }
        else { o += (char16_t)0xFFFD; i++; continue; }
        if (i + len > n) { o += (char16_t)0xFFFD; break; }
        bool ok = true;
        for (int k = 1; k < len; k++) {
            uint8_t cc = (uint8_t)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            c = (c << 6) | (cc & 0x3F);
        }
        if (!ok) { o += (char16_t)0xFFFD; i++; continue; }
        i += len;
        if (c >= 0x10000) { c -= 0x10000; o += (char16_t)(0xD800 + (c >> 10)); o += (char16_t)(0xDC00 + (c & 0x3FF)); }
        else o += (char16_t)c;
    }
    return o;
}

// cp1252 0x80..0x9F
static const uint16_t kCp1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

uint32_t cp_resolve(uint32_t cp) {
    if (cp == CP_ACP_ || cp == CP_OEMCP_ || cp == CP_MACCP_ || cp == CP_THREAD_ACP_) return 1252;
    return cp;
}
bool cp_supported(uint32_t cp) {
    cp = cp_resolve(cp);
    return cp == 1252 || cp == 65001 || cp == 437 || cp == 850 || cp == 28591 || cp == 20127 || cp == 1250 || cp == 1251;
}

int64_t mb_to_wide(uint32_t cp, const uint8_t *src, size_t n, uint16_t *out, size_t cap, bool strict) {
    cp = cp_resolve(cp);
    size_t w = 0;
    auto put = [&](uint16_t c) -> bool {
        if (out) { if (w >= cap) return false; out[w] = c; }
        w++;
        return true;
    };
    if (cp == 65001) {
        size_t i = 0;
        while (i < n) {
            uint8_t b = src[i];
            uint32_t c;
            int len;
            if (b < 0x80) { c = b; len = 1; }
            else if ((b & 0xE0) == 0xC0 && b >= 0xC2) { c = b & 0x1F; len = 2; }
            else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; len = 3; }
            else if ((b & 0xF8) == 0xF0 && b <= 0xF4) { c = b & 0x07; len = 4; }
            else { if (strict) return -1; if (!put(0xFFFD)) return -2; i++; continue; }
            bool ok = i + len <= n;
            for (int k = 1; ok && k < len; k++) {
                if ((src[i + k] & 0xC0) != 0x80) ok = false;
                else c = (c << 6) | (src[i + k] & 0x3F);
            }
            if (!ok) { if (strict) return -1; if (!put(0xFFFD)) return -2; i++; continue; }
            i += len;
            if (c >= 0x10000) {
                c -= 0x10000;
                if (!put((uint16_t)(0xD800 + (c >> 10))) || !put((uint16_t)(0xDC00 + (c & 0x3FF)))) return -2;
            } else if (!put((uint16_t)c)) return -2;
        }
        return (int64_t)w;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t b = src[i];
        uint16_t c = b;
        if (b >= 0x80 && b < 0xA0 && cp == 1252) c = kCp1252[b - 0x80];
        if (!put(c)) return -2;
    }
    return (int64_t)w;
}

int64_t wide_to_mb(uint32_t cp, const uint16_t *src, size_t n, uint8_t *out, size_t cap, bool *used_default, uint8_t defchar) {
    cp = cp_resolve(cp);
    size_t w = 0;
    auto put = [&](uint8_t c) -> bool {
        if (out) { if (w >= cap) return false; out[w] = c; }
        w++;
        return true;
    };
    if (used_default) *used_default = false;
    if (cp == 65001) {
        std::string s = utf16_to_utf8(src, n);
        for (unsigned char c : s) if (!put(c)) return -2;
        return (int64_t)w;
    }
    for (size_t i = 0; i < n; i++) {
        uint16_t c = src[i];
        uint8_t b;
        if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) b = (uint8_t)c;
        else {
            b = 0;
            bool found = false;
            if (cp == 1252) {
                for (int k = 0; k < 32; k++) if (kCp1252[k] == c) { b = (uint8_t)(0x80 + k); found = true; break; }
            }
            if (!found) { b = defchar; if (used_default) *used_default = true; }
        }
        if (!put(b)) return -2;
    }
    return (int64_t)w;
}

std::string ansi_to_utf8(const char *s, size_t n) {
    std::u16string w(n, 0);
    int64_t k = mb_to_wide(1252, (const uint8_t *)s, n, (uint16_t *)w.data(), n, false);
    w.resize(k < 0 ? 0 : (size_t)k);
    return utf16_to_utf8(w);
}

uint16_t u16_lower(uint16_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c : c + 1;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
        if (c == 0x178) return 0xFF;
        return c;
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}
uint16_t u16_upper(uint16_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c - 1 : c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        return c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

int u16_icmp(const uint16_t *a, const uint16_t *b, size_t maxn, bool ascii_only) {
    for (size_t i = 0; i < maxn; i++) {
        uint16_t x = a[i], y = b[i];
        if (ascii_only) {
            if (x >= 'A' && x <= 'Z') x += 32;
            if (y >= 'A' && y <= 'Z') y += 32;
        } else {
            x = u16_lower(x);
            y = u16_lower(y);
        }
        if (x != y) return x < y ? -1 : 1;
        if (x == 0) return 0;
    }
    return 0;
}

uint16_t u16_ctype(uint16_t c) {
    uint16_t f = 0;
    if (c < 0x80) {
        if (c >= 'A' && c <= 'Z') f |= 0x1 | 0x100 | 0x200;
        else if (c >= 'a' && c <= 'z') f |= 0x2 | 0x100 | 0x200;
        else if (c >= '0' && c <= '9') f |= 0x4 | 0x80 | 0x200;
        else if (c == ' ') f |= 0x8 | 0x40 | 0x200;
        else if (c == '\t') f |= 0x8 | 0x20 | 0x40 | 0x200;
        else if (c == '\n' || c == '\v' || c == '\f' || c == '\r') f |= 0x8 | 0x20 | 0x200;
        else if (c < 0x20 || c == 0x7F) f |= 0x20 | 0x200;
        else f |= 0x10 | 0x200;
        if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) f |= 0x80;
        return f;
    }
    f |= 0x200;
    if (c == 0xA0) return f | 0x8 | 0x40;
    if (c >= 0x80 && c < 0xA0) return f | 0x20;
    uint16_t lo = u16_lower(c), up = u16_upper(c);
    if (lo != c) f |= 0x1 | 0x100;
    else if (up != c) f |= 0x2 | 0x100;
    else if ((c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7) f |= 0x10;
    else if (c >= 0xC0 && c < 0x2000) f |= 0x100;
    else if (c >= 0x2000 && c <= 0x206F) f |= 0x10;
    else if (c >= 0x3040 && c < 0xA000) f |= 0x100;
    return f;
}

size_t gstrlen(uint32_t a) { return strlen((const char *)(guest_base + a)); }
size_t gwcslen(uint32_t a) {
    const uint16_t *p = (const uint16_t *)(guest_base + a);
    size_t n = 0;
    while (p[n]) n++;
    return n;
}
std::string read_astr(uint32_t a, int64_t n) {
    if (!a) return {};
    return std::string((const char *)(guest_base + a), n < 0 ? gstrlen(a) : (size_t)n);
}
std::u16string read_wstr(uint32_t a, int64_t n) {
    if (!a) return {};
    return std::u16string((const char16_t *)(guest_base + a), n < 0 ? gwcslen(a) : (size_t)n);
}
std::string read_astr_utf8(uint32_t a, int64_t n) {
    std::string s = read_astr(a, n);
    return ansi_to_utf8(s.data(), s.size());
}
std::string read_wstr_utf8(uint32_t a, int64_t n) {
    if (!a) return {};
    size_t len = n < 0 ? gwcslen(a) : (size_t)n;
    return utf16_to_utf8((const uint16_t *)(guest_base + a), len);
}
size_t write_wstr(uint32_t dst, size_t cap, const std::u16string &s) {
    if (cap > s.size()) {
        memcpy(guest_base + dst, s.data(), s.size() * 2);
        ((uint16_t *)(guest_base + dst))[s.size()] = 0;
    }
    return s.size();
}
size_t write_astr(uint32_t dst, size_t cap, const std::string &s) {
    if (cap > s.size()) memcpy(guest_base + dst, s.c_str(), s.size() + 1);
    return s.size();
}
std::string to_lower_ascii(std::string s) {
    for (auto &c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}
std::string lower_utf8(const std::string &s) {
    bool ascii = true;
    for (unsigned char c : s) if (c >= 0x80) { ascii = false; break; }
    if (ascii) return to_lower_ascii(s);
    std::u16string w = utf8_to_utf16(s);
    for (auto &c : w) c = u16_lower(c);
    return utf16_to_utf8(w);
}

}  // namespace dish
