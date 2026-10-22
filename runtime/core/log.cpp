#include "dish/log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>

namespace dish {

uint8_t g_log_level[LC_COUNT] = {
    LL_INFO, LL_INFO, LL_INFO, LL_INFO, LL_INFO, LL_INFO, LL_INFO, LL_INFO, LL_INFO, LL_INFO, LL_INFO,
    LL_INFO, LL_INFO, LL_INFO, LL_INFO, LL_INFO, LL_INFO, LL_INFO, LL_INFO,
};
std::atomic<uint64_t> g_unimpl_calls{0};
void (*g_log_hook)(LogCh, LogLvl, const char *) = nullptr;

static const char *kChNames[LC_COUNT] = {"core", "mem", "loader", "thread", "sync", "file", "reg", "hle", "stub", "fault",
                                         "exc", "str", "gfx", "audio", "input", "win", "net", "engine", "test"};
static const char *kLvlNames[] = {"error", "warn", "info", "debug", "trace"};
static std::mutex g_log_mu;
static FILE *g_log_file = nullptr;
static timespec g_t0;
static bool g_t0_set = false;

const char *log_ch_name(LogCh ch) { return kChNames[ch]; }

static bool parse_lvl(const char *s, LogLvl *out) {
    for (int i = 0; i < 5; i++)
        if (!strcmp(s, kLvlNames[i])) { *out = (LogLvl)i; return true; }
    if (!strcmp(s, "off") || !strcmp(s, "none")) { *out = (LogLvl)0; return true; }
    return false;
}

void log_set_level(LogCh ch, LogLvl lvl) { g_log_level[ch] = lvl; }

bool log_set_spec(const char *spec) {
    bool ok = true;
    std::string s(spec ? spec : "");
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(',', pos);
        if (e == std::string::npos) e = s.size();
        std::string tok = s.substr(pos, e - pos);
        pos = e + 1;
        if (tok.empty()) continue;
        size_t eq = tok.find('=');
        LogLvl lvl;
        if (eq == std::string::npos) {
            if (!parse_lvl(tok.c_str(), &lvl)) { ok = false; continue; }
            for (int c = 0; c < LC_COUNT; c++) g_log_level[c] = lvl;
        } else {
            std::string ch = tok.substr(0, eq), lv = tok.substr(eq + 1);
            if (!parse_lvl(lv.c_str(), &lvl)) { ok = false; continue; }
            bool found = false;
            for (int c = 0; c < LC_COUNT; c++)
                if (ch == kChNames[c]) { g_log_level[c] = lvl; found = true; }
            if (!found) ok = false;
        }
    }
    return ok;
}

static std::string g_log_dir;
void log_set_file(const char *path) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    if (g_log_file) { fclose(g_log_file); g_log_file = nullptr; }
    if (path && *path) g_log_file = fopen(path, "a");
    std::string p = path ? path : "";
    size_t sl = p.rfind('/');
    g_log_dir = sl == std::string::npos ? std::string() : p.substr(0, sl);
}
std::string log_dir() {
    std::lock_guard<std::mutex> lk(g_log_mu);
    return g_log_dir;
}

void log_init_from_env() {
    if (const char *e = getenv("DISH_LOG")) log_set_spec(e);
    if (const char *f = getenv("DISH_LOG_FILE")) log_set_file(f);
}

void log_write(LogCh ch, LogLvl lvl, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    std::lock_guard<std::mutex> lk(g_log_mu);
    if (!g_t0_set) { g_t0 = now; g_t0_set = true; }
    double t = (now.tv_sec - g_t0.tv_sec) + (now.tv_nsec - g_t0.tv_nsec) * 1e-9;
#ifdef __linux__
    long tid = syscall(SYS_gettid);
#else
    uint64_t tid64 = 0; pthread_threadid_np(nullptr, &tid64); long tid = (long)tid64;
#endif
    char line[2200];
    snprintf(line, sizeof line, "[%9.4f %-5s %-6s t%ld] %s\n", t, kLvlNames[lvl], kChNames[ch], tid, buf);
    fputs(line, stderr);
    if (g_log_file) { fputs(line, g_log_file); fflush(g_log_file); }
    if (g_log_hook) g_log_hook(ch, lvl, buf);
}

void log_flush() {
    std::lock_guard<std::mutex> lk(g_log_mu);
    fflush(stderr);
    if (g_log_file) fflush(g_log_file);
}

void fatal(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_write(LC_CORE, LL_ERROR, "FATAL: %s", buf);
    log_flush();
    abort();
}

}  // namespace dish
