#import "PBMemoryRunner.h"
#import "PBCommon.h"
#import "PBMemory.h"
#import <UIKit/UIKit.h>
#import <stdatomic.h>

static atomic_int gMemWarnings;
static NSMutableArray *gSamples;
static NSMutableDictionary *gDoc;

static void flush(void) { [[PBResults shared] setSection:@"memory" value:[gDoc copy]]; }

static void on_sample(const PBMemSample *s, void *u) {
    NSDictionary *d = @{ @"step": @(s->step), @"committed_mb": @(s->committed_mb), @"avail_mb": @(s->avail_bytes >> 20),
                         @"footprint_mb": @(s->footprint_bytes >> 20), @"implied_limit_mb": @(s->implied_limit_bytes >> 20),
                         @"elapsed_ms": @((long)s->elapsed_ms) };
    [gSamples addObject:d];
    gDoc[@"last_committed_mb"] = @(s->committed_mb);
    gDoc[@"memory_warnings"] = @(atomic_load(&gMemWarnings));
    flush();
}
static void on_log(const char *l, void *u) { PBLog(@"mem: %s", l); }
static int should_abort(void *u) { return atomic_load(&gMemWarnings) > 0; }

void PBRunMemoryProbe(void (^done)(NSDictionary *)) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        [[NSNotificationCenter defaultCenter] addObserverForName:UIApplicationDidReceiveMemoryWarningNotification object:nil
            queue:nil usingBlock:^(NSNotification *n) { atomic_fetch_add(&gMemWarnings, 1); PBLog(@"MEMORY WARNING received"); }];
    });
    atomic_store(&gMemWarnings, 0);
    gSamples = [NSMutableArray array];
    gDoc = [NSMutableDictionary dictionaryWithDictionary:@{ @"status": @"running", @"samples": gSamples,
        @"note": @"status stays 'running' if the process was killed (jetsam) mid-probe; last_committed_mb is then the kill point" }];
    flush();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        PBMemConfig c; pb_memory_default_config(&c);
        c.on_sample = on_sample; c.on_log = on_log; c.should_abort = should_abort;
        PBMemResult r;
        pb_memory_probe(&c, &r);
        NSMutableArray *extra = [NSMutableArray array];
        static const int gb[4] = { 8, 16, 32, 64 };
        for (int i = 0; i < 4; i++) [extra addObject:@{ @"gb": @(gb[i]), @"ok": @(r.extra_reserve_ok[i]), @"errno": @(r.extra_reserve_errno[i]) }];
        gDoc[@"status"] = @"finished";
        gDoc[@"reserve_4gb_prot_none_ok"] = @(r.reserve_ok);
        gDoc[@"reserve_4gb_errno"] = @(r.reserve_errno);
        gDoc[@"extra_reservations"] = extra;
        gDoc[@"avail_at_start_mb"] = @(r.avail_at_start >> 20);
        gDoc[@"footprint_at_start_mb"] = @(r.footprint_at_start >> 20);
        gDoc[@"max_safely_committed_mb"] = @(r.max_committed_mb);
        gDoc[@"avail_at_stop_mb"] = @(r.avail_at_stop >> 20);
        gDoc[@"avail_after_release_mb"] = @(r.avail_after_release >> 20);
        gDoc[@"implied_jetsam_limit_mb"] = @(r.implied_limit_bytes >> 20);
        gDoc[@"stop_reason"] = @(r.stop_reason);
        gDoc[@"memory_warnings"] = @(atomic_load(&gMemWarnings));
        flush();
        dispatch_async(dispatch_get_main_queue(), ^{ done([gDoc copy]); });
    });
}
