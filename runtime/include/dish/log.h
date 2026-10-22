// Structured logging with per-subsystem channels.
//
//   DISH_LOG=info                 default level for all channels
//   DISH_LOG=warn,file=debug,hle=trace   per-channel overrides (names below)
//   DISH_LOG_FILE=path            also append to this file
#pragma once
#include <string>
#include <atomic>
#include <cstdarg>
#include <cstdint>

namespace dish {

enum LogCh : uint8_t {
    LC_CORE, LC_MEM, LC_LOADER, LC_THREAD, LC_SYNC, LC_FILE, LC_REG, LC_HLE, LC_STUB, LC_FAULT, LC_EXC,
    LC_STR, LC_GFX, LC_AUDIO, LC_INPUT, LC_WIN, LC_NET, LC_ENGINE, LC_TEST, LC_COUNT
};
enum LogLvl : uint8_t { LL_ERROR = 0, LL_WARN = 1, LL_INFO = 2, LL_DEBUG = 3, LL_TRACE = 4 };

extern uint8_t g_log_level[LC_COUNT];
extern std::atomic<uint64_t> g_unimpl_calls;   // total calls into auto-generated stubs

void log_init_from_env();
void log_set_level(LogCh ch, LogLvl lvl);
bool log_set_spec(const char *spec);           // "info,file=debug"; returns false on a bad token
void log_set_file(const char *path);
std::string log_dir();   // directory of the DISH_LOG_FILE / log_set_file file ("" when none)
const char *log_ch_name(LogCh ch);
void log_write(LogCh ch, LogLvl lvl, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void log_flush();
// Capture hook for tests: when non-null every formatted line is also passed here.
extern void (*g_log_hook)(LogCh, LogLvl, const char *line);

#define DLOG(ch, lvl, ...) \
    do { if (::dish::g_log_level[::dish::ch] >= ::dish::lvl) ::dish::log_write(::dish::ch, ::dish::lvl, __VA_ARGS__); } while (0)
#define LOGE(ch, ...) DLOG(ch, LL_ERROR, __VA_ARGS__)
#define LOGW(ch, ...) DLOG(ch, LL_WARN, __VA_ARGS__)
#define LOGI(ch, ...) DLOG(ch, LL_INFO, __VA_ARGS__)
#define LOGD(ch, ...) DLOG(ch, LL_DEBUG, __VA_ARGS__)
#define LOGT(ch, ...) DLOG(ch, LL_TRACE, __VA_ARGS__)

[[noreturn]] void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace dish
