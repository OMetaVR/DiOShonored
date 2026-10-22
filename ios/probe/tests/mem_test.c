/* Linux sanity test of the stepping/stop logic: PB_FAKE_LIMIT_MB emulates the jetsam limit. */
#include "PBMemory.h"
#include <stdio.h>
static void on_sample(const PBMemSample *s, void *u) { (void)u;
    printf("  sample step=%u committed=%lluMB avail=%lluMB fp=%lluMB\n", s->step, (unsigned long long)s->committed_mb,
           (unsigned long long)(s->avail_bytes >> 20), (unsigned long long)(s->footprint_bytes >> 20)); }
static void on_log(const char *l, void *u) { (void)u; printf("%s\n", l); }
int main(void) {
    PBMemConfig c; pb_memory_default_config(&c); c.on_sample = on_sample; c.on_log = on_log;
    PBMemResult r; pb_memory_probe(&c, &r);
    printf("RESULT reserve_ok=%d max_committed=%lluMB steps=%u reason='%s' avail_stop=%lluMB\n", r.reserve_ok,
           (unsigned long long)r.max_committed_mb, r.steps, r.stop_reason, (unsigned long long)(r.avail_at_stop >> 20));
    return 0;
}
