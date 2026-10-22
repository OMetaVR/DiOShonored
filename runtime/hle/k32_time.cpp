// kernel32 / winmm: clocks and time conversions.
#include "dish/hle.h"
#include "dish/sync.h"
#include "dish/thread.h"
#include "dish/util.h"
#include <ctime>
#include <sys/time.h>

namespace dish {
namespace {

constexpr uint64_t kEpochDiff100ns = 116444736000000000ull;   // 1601 -> 1970
constexpr uint64_t kQpcFreq = 10000000ull;                    // 10 MHz

uint64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t start_ns() {
    static uint64_t s = mono_ns();
    return s;
}
uint64_t since_start_ns() {   // start_ns() must be evaluated before the clock is read
    uint64_t s = start_ns();
    return mono_ns() - s;
}
uint64_t real_filetime() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100 + kEpochDiff100ns;
}
uint32_t tick_ms() { return (uint32_t)(since_start_ns() / 1000000ull + 600000ull); }   // pretend 10 min uptime

uint32_t k32_GetTickCount() { return tick_ms(); }
uint64_t k32_GetTickCount64() { return since_start_ns() / 1000000ull + 600000ull; }
uint32_t k32_timeGetTime() { return tick_ms(); }
uint32_t k32_QueryPerformanceCounter(uint32_t p) {
    // Windows' counter counts from boot, so games only ever see large values with a non-zero high dword. Starting near 0 gave
    // values that are unrealistic for them (high dword 0, low dword turning negative after 214 s) and coincided with a PhysX
    // free-list corruption at that time. Pretend the machine has been up for 3 hours.
    wr64(p, since_start_ns() / 100ull + 3ull * 3600ull * kQpcFreq);
    return 1;
}
uint32_t k32_QueryPerformanceFrequency(uint32_t p) { wr64(p, kQpcFreq); return 1; }
void k32_GetSystemTimeAsFileTime(uint32_t p) { wr64(p, real_filetime()); }
void k32_GetSystemTimePreciseAsFileTime(uint32_t p) { wr64(p, real_filetime()); }

void put_systemtime(uint32_t p, const tm &t, uint32_t ms) {
    wr16(p + 0, (uint16_t)(t.tm_year + 1900));
    wr16(p + 2, (uint16_t)(t.tm_mon + 1));
    wr16(p + 4, (uint16_t)t.tm_wday);
    wr16(p + 6, (uint16_t)t.tm_mday);
    wr16(p + 8, (uint16_t)t.tm_hour);
    wr16(p + 10, (uint16_t)t.tm_min);
    wr16(p + 12, (uint16_t)t.tm_sec);
    wr16(p + 14, (uint16_t)ms);
}
void k32_GetSystemTime(uint32_t p) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tm t;
    gmtime_r(&ts.tv_sec, &t);
    put_systemtime(p, t, (uint32_t)(ts.tv_nsec / 1000000));
}
void k32_GetLocalTime(uint32_t p) {   // the guest time zone is UTC (see GetTimeZoneInformation)
    k32_GetSystemTime(p);
}
uint32_t k32_SystemTimeToFileTime(uint32_t st, uint32_t ft) {
    tm t{};
    t.tm_year = rd16(st) - 1900;
    t.tm_mon = rd16(st + 2) - 1;
    t.tm_mday = rd16(st + 6);
    t.tm_hour = rd16(st + 8);
    t.tm_min = rd16(st + 10);
    t.tm_sec = rd16(st + 12);
    if (t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    time_t s = timegm(&t);
    wr64(ft, ((uint64_t)s * 10000000ull + (uint64_t)rd16(st + 14) * 10000ull) + kEpochDiff100ns);
    return 1;
}
uint32_t k32_FileTimeToSystemTime(uint32_t ft, uint32_t st) {
    uint64_t v = rd64(ft);
    if (v < kEpochDiff100ns) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    v -= kEpochDiff100ns;
    time_t s = (time_t)(v / 10000000ull);
    tm t;
    gmtime_r(&s, &t);
    put_systemtime(st, t, (uint32_t)((v % 10000000ull) / 10000));
    return 1;
}
uint32_t k32_FileTimeToLocalFileTime(uint32_t in, uint32_t out) { wr64(out, rd64(in)); return 1; }
uint32_t k32_LocalFileTimeToFileTime(uint32_t in, uint32_t out) { wr64(out, rd64(in)); return 1; }
uint32_t k32_SetLocalTime(uint32_t) { return 1; }
uint32_t k32_SetSystemTime(uint32_t) { set_last_error(5); return 0; }
uint32_t k32_GetTimeZoneInformation(uint32_t p) {
    for (uint32_t i = 0; i < 172; i += 4) wr32(p + i, 0);
    const char16_t *name = u"UTC";
    for (int i = 0; name[i]; i++) { wr16(p + 4 + 2 * i, name[i]); wr16(p + 88 + 2 * i, name[i]); }
    return 0;   // TIME_ZONE_ID_UNKNOWN
}
uint32_t k32_GetDateFormatA(uint32_t, uint32_t, uint32_t st, uint32_t fmt, uint32_t buf, uint32_t cch) {
    char tmp[64];
    uint16_t y, m, d;
    if (st) { y = rd16(st); m = rd16(st + 2); d = rd16(st + 6); }
    else { time_t s = time(nullptr); tm t; gmtime_r(&s, &t); y = t.tm_year + 1900; m = t.tm_mon + 1; d = t.tm_mday; }
    snprintf(tmp, sizeof tmp, "%u/%u/%u", m, d, y);
    (void)fmt;
    size_t n = strlen(tmp) + 1;
    if (!cch) return (uint32_t)n;
    if (cch < n) { set_last_error(122); return 0; }
    memcpy(guest_base + buf, tmp, n);
    return (uint32_t)n;
}
uint32_t k32_GetTimeFormatA(uint32_t, uint32_t, uint32_t st, uint32_t fmt, uint32_t buf, uint32_t cch) {
    char tmp[64];
    uint16_t h, mi, s;
    if (st) { h = rd16(st + 8); mi = rd16(st + 10); s = rd16(st + 12); }
    else { time_t t0 = time(nullptr); tm t; gmtime_r(&t0, &t); h = t.tm_hour; mi = t.tm_min; s = t.tm_sec; }
    snprintf(tmp, sizeof tmp, "%u:%02u:%02u %s", h % 12 == 0 ? 12 : h % 12, mi, s, h < 12 ? "AM" : "PM");
    (void)fmt;
    size_t n = strlen(tmp) + 1;
    if (!cch) return (uint32_t)n;
    if (cch < n) { set_last_error(122); return 0; }
    memcpy(guest_base + buf, tmp, n);
    return (uint32_t)n;
}
uint32_t winmm_timeBeginPeriod(uint32_t) { return 0; }
uint32_t winmm_timeEndPeriod(uint32_t) { return 0; }
uint32_t winmm_timeGetDevCaps(uint32_t p, uint32_t cb) { if (cb >= 8) { wr32(p, 1); wr32(p + 4, 1000000); } return 0; }
uint32_t winmm_timeGetSystemTime(uint32_t p, uint32_t cb) { if (cb >= 8) { wr32(p, 0); wr32(p + 4, tick_ms()); } return 0; }

}  // namespace

HLE("kernel32.dll", "GetTickCount", Stdcall, k32_GetTickCount);
HLE("kernel32.dll", "GetTickCount64", Stdcall, k32_GetTickCount64);
HLE("winmm.dll", "timeGetTime", Stdcall, k32_timeGetTime);
HLE("winmm.dll", "timeBeginPeriod", Stdcall, winmm_timeBeginPeriod);
HLE("winmm.dll", "timeEndPeriod", Stdcall, winmm_timeEndPeriod);
HLE("winmm.dll", "timeGetDevCaps", Stdcall, winmm_timeGetDevCaps);
HLE("winmm.dll", "timeGetSystemTime", Stdcall, winmm_timeGetSystemTime);
HLE("kernel32.dll", "QueryPerformanceCounter", Stdcall, k32_QueryPerformanceCounter);
HLE("kernel32.dll", "QueryPerformanceFrequency", Stdcall, k32_QueryPerformanceFrequency);
HLE("kernel32.dll", "GetSystemTimeAsFileTime", Stdcall, k32_GetSystemTimeAsFileTime);
HLE("kernel32.dll", "GetSystemTimePreciseAsFileTime", Stdcall, k32_GetSystemTimePreciseAsFileTime);
HLE("kernel32.dll", "GetSystemTime", Stdcall, k32_GetSystemTime);
HLE("kernel32.dll", "GetLocalTime", Stdcall, k32_GetLocalTime);
HLE("kernel32.dll", "SystemTimeToFileTime", Stdcall, k32_SystemTimeToFileTime);
HLE("kernel32.dll", "FileTimeToSystemTime", Stdcall, k32_FileTimeToSystemTime);
HLE("kernel32.dll", "FileTimeToLocalFileTime", Stdcall, k32_FileTimeToLocalFileTime);
HLE("kernel32.dll", "LocalFileTimeToFileTime", Stdcall, k32_LocalFileTimeToFileTime);
HLE("kernel32.dll", "SetLocalTime", Stdcall, k32_SetLocalTime);
HLE("kernel32.dll", "SetSystemTime", Stdcall, k32_SetSystemTime);
HLE("kernel32.dll", "GetTimeZoneInformation", Stdcall, k32_GetTimeZoneInformation);
HLE("kernel32.dll", "GetDateFormatA", Stdcall, k32_GetDateFormatA);
HLE("kernel32.dll", "GetTimeFormatA", Stdcall, k32_GetTimeFormatA);

}  // namespace dish
