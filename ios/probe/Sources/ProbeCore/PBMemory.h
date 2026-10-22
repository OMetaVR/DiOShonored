/* Memory budget probe: reserve VA, commit+touch in steps, stop at a safe threshold.
 * Portable C so the stepping logic can be unit-tested on Linux (see tests/mem_test.c). */
#ifndef PB_MEMORY_H
#define PB_MEMORY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct PBMemSample {
    uint32_t step;            /* 0 = baseline before any commit */
    uint64_t committed_mb;    /* mb we have committed + touched so far */
    uint64_t avail_bytes;     /* os_proc_available_memory() */
    uint64_t footprint_bytes; /* task_vm_info.phys_footprint */
    uint64_t implied_limit_bytes; /* footprint + avail: the jetsam limit as seen by this process */
    double   elapsed_ms;
} PBMemSample;

typedef struct PBMemConfig {
    uint64_t reserve_bytes;     /* VA reservation, default 4 GiB */
    uint32_t step_mb;           /* default 64 */
    uint32_t stop_avail_mb;     /* stop before a step that would leave less than this; default 150 */
    uint32_t max_commit_mb;     /* hard cap (0 = reserve size) */
    void (*on_sample)(const PBMemSample *s, void *user);   /* after every step, incl. baseline */
    int  (*should_abort)(void *user);                      /* polled between steps; nonzero = stop */
    void (*on_log)(const char *line, void *user);
    void *user;
} PBMemConfig;

typedef struct PBMemResult {
    int      reserve_ok;
    int      reserve_errno;
    uint64_t avail_at_start;
    uint64_t footprint_at_start;
    uint64_t max_committed_mb;     /* max safely committed (the last completed step) */
    uint64_t avail_at_stop;
    uint64_t avail_after_release;
    uint64_t implied_limit_bytes;  /* from the final sample */
    uint32_t steps;
    char     stop_reason[96];      /* "avail below threshold", "memory warning", "cap reached", ... */
    /* extra VA reservation experiments (PROT_NONE): 8, 16, 32, 64 GiB */
    int      extra_reserve_ok[4];
    int      extra_reserve_errno[4];
} PBMemResult;

void     pb_memory_default_config(PBMemConfig *c);
int      pb_memory_probe(const PBMemConfig *c, PBMemResult *r);
uint64_t pb_avail_bytes(void);
uint64_t pb_footprint_bytes(void);

#ifdef __cplusplus
}
#endif
#endif
