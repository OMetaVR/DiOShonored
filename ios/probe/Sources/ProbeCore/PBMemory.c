#include "PBMemory.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <os/proc.h>
uint64_t pb_avail_bytes(void) { return (uint64_t)os_proc_available_memory(); }
uint64_t pb_footprint_bytes(void) {
    task_vm_info_data_t info; mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &cnt) != KERN_SUCCESS) return 0;
    return (uint64_t)info.phys_footprint;
}
#else
/* Linux test shim: pretend the jetsam limit is PB_FAKE_LIMIT_MB (default 1024) and footprint = RSS. */
uint64_t pb_footprint_bytes(void) {
    FILE *f = fopen("/proc/self/statm", "r"); unsigned long sz = 0, rss = 0;
    if (f) { if (fscanf(f, "%lu %lu", &sz, &rss) != 2) rss = 0; fclose(f); }
    return (uint64_t)rss * (uint64_t)sysconf(_SC_PAGESIZE);
}
uint64_t pb_avail_bytes(void) {
    const char *e = getenv("PB_FAKE_LIMIT_MB");
    uint64_t lim = (uint64_t)(e ? atoll(e) : 1024) << 20, fp = pb_footprint_bytes();
    return lim > fp ? lim - fp : 0;
}
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static void plog(const PBMemConfig *c, const char *fmt, ...) {
    char buf[256]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (c->on_log) c->on_log(buf, c->user);
}

void pb_memory_default_config(PBMemConfig *c) {
    memset(c, 0, sizeof *c);
    c->reserve_bytes = 4ull << 30;
    c->step_mb = 64;
    c->stop_avail_mb = 150;
    c->max_commit_mb = 0;
}

/* Fill with incompressible data: iOS can compress dirty pages, zero pages would under-report. */
static void touch_random(uint8_t *p, size_t n, uint64_t *seed) {
    uint64_t x = *seed;
    uint64_t *w = (uint64_t *)p;
    for (size_t i = 0; i < n / 8; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; w[i] = x; }
    *seed = x;
}

static void emit(const PBMemConfig *c, PBMemSample *s, uint32_t step, uint64_t committed_mb, double t0) {
    s->step = step; s->committed_mb = committed_mb;
    s->avail_bytes = pb_avail_bytes(); s->footprint_bytes = pb_footprint_bytes();
    s->implied_limit_bytes = s->avail_bytes + s->footprint_bytes;
    s->elapsed_ms = now_ms() - t0;
    if (c->on_sample) c->on_sample(s, c->user);
}

int pb_memory_probe(const PBMemConfig *cfg, PBMemResult *r) {
    memset(r, 0, sizeof *r);
    const double t0 = now_ms();
    r->avail_at_start = pb_avail_bytes();
    r->footprint_at_start = pb_footprint_bytes();
    plog(cfg, "start: available=%llu MB footprint=%llu MB", (unsigned long long)(r->avail_at_start >> 20),
         (unsigned long long)(r->footprint_at_start >> 20));

    /* 1. the required reservation */
    uint8_t *base = (uint8_t *)mmap(NULL, cfg->reserve_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) {
        r->reserve_ok = 0; r->reserve_errno = errno; base = NULL;
        plog(cfg, "reserve %llu GB PROT_NONE: FAILED errno=%d (%s)", (unsigned long long)(cfg->reserve_bytes >> 30), errno, strerror(errno));
        snprintf(r->stop_reason, sizeof r->stop_reason, "reservation failed");
    } else {
        r->reserve_ok = 1;
        plog(cfg, "reserve %llu GB PROT_NONE: ok at %p", (unsigned long long)(cfg->reserve_bytes >> 30), (void *)base);
    }

    /* 2. VA headroom experiments (not committed, released immediately) */
    static const uint64_t extra_gb[4] = { 8, 16, 32, 64 };
    for (int i = 0; i < 4; i++) {
        void *p = mmap(NULL, extra_gb[i] << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) { r->extra_reserve_ok[i] = 0; r->extra_reserve_errno[i] = errno; }
        else { r->extra_reserve_ok[i] = 1; munmap(p, extra_gb[i] << 30); }
        plog(cfg, "extra reserve %llu GB: %s", (unsigned long long)extra_gb[i], r->extra_reserve_ok[i] ? "ok" : "failed");
    }

    PBMemSample s;
    emit(cfg, &s, 0, 0, t0);
    if (!base) { r->avail_at_stop = s.avail_bytes; return -1; }

    /* 3. commit + touch progressively */
    const uint64_t step = (uint64_t)cfg->step_mb << 20;
    uint64_t cap = cfg->max_commit_mb ? ((uint64_t)cfg->max_commit_mb << 20) : cfg->reserve_bytes;
    if (cap > cfg->reserve_bytes) cap = cfg->reserve_bytes;
    uint64_t committed = 0, seed = 0x9E3779B97F4A7C15ull;
    uint32_t n = 0;
    const char *reason = "cap reached";
    for (;;) {
        if (cfg->should_abort && cfg->should_abort(cfg->user)) { reason = "aborted (memory warning or user)"; break; }
        if (committed + step > cap) { reason = "cap reached"; break; }
        uint64_t avail = pb_avail_bytes();
        if (avail < ((uint64_t)cfg->stop_avail_mb << 20) + step) { reason = "available memory below safe threshold"; break; }
        if (mprotect(base + committed, step, PROT_READ | PROT_WRITE) != 0) { reason = "mprotect failed"; plog(cfg, "mprotect errno=%d", errno); break; }
        touch_random(base + committed, step, &seed);
        committed += step; n++;
        emit(cfg, &s, n, committed >> 20, t0);
        plog(cfg, "step %u: committed=%llu MB avail=%llu MB footprint=%llu MB", n, (unsigned long long)(committed >> 20),
             (unsigned long long)(s.avail_bytes >> 20), (unsigned long long)(s.footprint_bytes >> 20));
    }
    r->steps = n;
    r->max_committed_mb = committed >> 20;
    r->avail_at_stop = s.avail_bytes;
    r->implied_limit_bytes = s.implied_limit_bytes;
    snprintf(r->stop_reason, sizeof r->stop_reason, "%s", reason);

    /* 4. release */
    munmap(base, cfg->reserve_bytes);
    r->avail_after_release = pb_avail_bytes();
    plog(cfg, "stopped: %s; max committed %llu MB; available after release %llu MB", reason,
         (unsigned long long)r->max_committed_mb, (unsigned long long)(r->avail_after_release >> 20));
    return 0;
}
