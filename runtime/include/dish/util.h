// String and guest-memory helpers shared by the HLE modules. Guest WCHAR is uint16_t (UTF-16), never wchar_t.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include "dish/vm.h"

namespace dish {

// UTF-16 <-> UTF-8 (unpaired surrogates become U+FFFD)
std::string utf16_to_utf8(const uint16_t *s, size_t n);
inline std::string utf16_to_utf8(const std::u16string &s) { return utf16_to_utf8((const uint16_t *)s.data(), s.size()); }
std::u16string utf8_to_utf16(std::string_view s);

// Windows code pages used by the CRT and engine: 1252 (ANSI), 437/850 treated as 1252-compatible for ASCII, 65001 UTF-8.
constexpr uint32_t CP_ACP_ = 0, CP_OEMCP_ = 1, CP_MACCP_ = 2, CP_THREAD_ACP_ = 3, CP_UTF7_ = 65000, CP_UTF8_ = 65001;
uint32_t cp_resolve(uint32_t cp);           // maps 0/1/2/3 to a concrete code page (1252)
bool cp_supported(uint32_t cp);
// Convert; return number of units written to `out` (nullptr = count only), or -1 on invalid input when strict.
int64_t mb_to_wide(uint32_t cp, const uint8_t *src, size_t n, uint16_t *out, size_t cap, bool strict);
int64_t wide_to_mb(uint32_t cp, const uint16_t *src, size_t n, uint8_t *out, size_t cap, bool *used_default, uint8_t defchar);
std::string ansi_to_utf8(const char *s, size_t n);   // cp1252 -> UTF-8

// Simple (BMP) case mapping, enough for ASCII, Latin-1/Extended-A, Greek and Cyrillic.
uint16_t u16_lower(uint16_t c);
uint16_t u16_upper(uint16_t c);
// _wcsicmp-style compare using ASCII-only folding when ascii_only, else u16_lower folding
int u16_icmp(const uint16_t *a, const uint16_t *b, size_t maxn, bool ascii_only);
uint16_t u16_ctype(uint16_t c);              // CT_CTYPE1 flags (C1_UPPER...)

// Guest memory access
inline uint32_t rd32(uint32_t a) { return *(uint32_t *)(guest_base + a); }
inline uint16_t rd16(uint32_t a) { return *(uint16_t *)(guest_base + a); }
inline uint8_t rd8(uint32_t a) { return guest_base[a]; }
inline uint64_t rd64(uint32_t a) { return *(uint64_t *)(guest_base + a); }
inline void wr32(uint32_t a, uint32_t v) { *(uint32_t *)(guest_base + a) = v; }
inline void wr16(uint32_t a, uint16_t v) { *(uint16_t *)(guest_base + a) = v; }
inline void wr8(uint32_t a, uint8_t v) { guest_base[a] = v; }
inline void wr64(uint32_t a, uint64_t v) { *(uint64_t *)(guest_base + a) = v; }

size_t gstrlen(uint32_t a);                  // bytes (ANSI)
size_t gwcslen(uint32_t a);                  // UTF-16 units
std::string read_astr(uint32_t a, int64_t n = -1);        // raw bytes of an ANSI string
std::u16string read_wstr(uint32_t a, int64_t n = -1);
std::string read_astr_utf8(uint32_t a, int64_t n = -1);   // ANSI (cp1252) -> UTF-8
std::string read_wstr_utf8(uint32_t a, int64_t n = -1);
// Write a string (with terminator) into a guest buffer of `cap` units; returns units needed excluding NUL.
// Fails (returns needed, writes nothing meaningful) when it does not fit.
size_t write_wstr(uint32_t dst, size_t cap, const std::u16string &s);
size_t write_astr(uint32_t dst, size_t cap, const std::string &s);

std::string to_lower_ascii(std::string s);
std::string lower_utf8(const std::string &s);   // BMP-aware lower-casing of a UTF-8 string

}  // namespace dish
