#import "PBD3D9Runner.h"
#import "PBCommon.h"
#import "d3d9_scene.h"
#import <dlfcn.h>
#import <QuartzCore/QuartzCore.h>

/* Minimal SDL3 surface we need, resolved with dlsym (same approach DXVK's own WSI uses). */
typedef struct SDL_Window SDL_Window;
static struct {
    void *h;
    void (*SetMainReady)(void);
    bool (*SetHint)(const char *, const char *);
    bool (*Init)(uint32_t);
    SDL_Window *(*CreateWindow)(const char *, int, int, uint64_t);
    bool (*GetWindowSizeInPixels)(SDL_Window *, int *, int *);
    void (*PumpEvents)(void);
    bool (*PollEvent)(void *);
    void (*DestroyWindow)(SDL_Window *);
    const char *(*GetError)(void);
} S;

static BOOL loadSDL(NSString *dir, NSMutableDictionary *out) {
    if (S.h) return YES;
    NSString *p = [dir stringByAppendingPathComponent:@"libSDL3.0.dylib"];
    S.h = dlopen(p.UTF8String, RTLD_NOW | RTLD_GLOBAL);
    if (!S.h) { out[@"error"] = [NSString stringWithFormat:@"dlopen %@: %s", p, dlerror()]; return NO; }
#define L(n) do { *(void **)&S.n = dlsym(S.h, "SDL_" #n); if (!S.n) { out[@"error"] = @"missing SDL_" #n; return NO; } } while (0)
    L(SetMainReady); L(SetHint); L(Init); L(CreateWindow); L(GetWindowSizeInPixels); L(PumpEvents); L(PollEvent); L(DestroyWindow); L(GetError);
#undef L
    return YES;
}

@interface PBD3D9Driver : NSObject
@property (nonatomic) SDL_Window *win; @property (nonatomic) d3d9_scene *scene;
@property (nonatomic, strong) CADisplayLink *link;
@property (nonatomic) CFTimeInterval t0, tEnd; @property (nonatomic) NSUInteger frames; @property (nonatomic) double worstFrameMs, cpuMsSum;
@property (nonatomic, copy) void (^done)(NSDictionary *);
@property (nonatomic, strong) NSMutableDictionary *res; @property (nonatomic, strong) NSMutableArray *lines;
@end

static void logcb(const char *line, void *u) { PBLog(@"d3d9: %s", line); [((__bridge PBD3D9Driver *)u).lines addObject:@(line)]; }

@implementation PBD3D9Driver
- (void)tick:(CADisplayLink *)l {
    CFTimeInterval now = CACurrentMediaTime(), t = now - self.t0;
    BOOL stop = t >= self.tEnd - self.t0;
    S.PumpEvents();
    uint8_t ev[128];
    while (S.PollEvent(ev)) { uint32_t type = *(uint32_t *)ev; if (type == 0x100 /*QUIT*/ || type == 0x700 /*FINGER_DOWN*/) stop = YES; }
    CFTimeInterval c0 = CACurrentMediaTime();
    int r = d3d9_scene_frame(self.scene, (float)t);
    double ms = (CACurrentMediaTime() - c0) * 1000.0;
    self.cpuMsSum += ms; if (ms > self.worstFrameMs && self.frames > 5) self.worstFrameMs = ms;
    self.frames++;
    if (r) { self.res[@"frame_error"] = @(r); stop = YES; }
    if (stop) [self finish:t];
}
- (void)finish:(CFTimeInterval)t {
    [self.link invalidate]; self.link = nil;
    char rep[1024] = {0};
    int st = d3d9_scene_selftest(self.scene, rep, sizeof rep);
    self.res[@"used_real_dxt_textures"] = @(d3d9_scene_used_dxt(self.scene));
    self.res[@"selftest_pass"] = @(st == 0); self.res[@"selftest_report"] = @(rep);
    uint8_t *px = NULL; int w = 0, h = 0;
    if (d3d9_scene_read_backbuffer(self.scene, &px, &w, &h) == 0) {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef cg = CGBitmapContextCreate(px, w, h, 8, w * 4, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
        CGImageRef img = CGBitmapContextCreateImage(cg);
        NSData *png = UIImagePNGRepresentation([UIImage imageWithCGImage:img]);
        NSString *p = [PBDocumentsPath() stringByAppendingPathComponent:@"d3d9_test.png"];
        [png writeToFile:p atomically:YES];
        self.res[@"screenshot"] = p; self.res[@"screenshot_size"] = @{ @"w": @(w), @"h": @(h) };
        CGImageRelease(img); CGContextRelease(cg); CGColorSpaceRelease(cs); free(px);
    }
    self.res[@"frames"] = @(self.frames);
    self.res[@"avg_fps"] = @(self.frames / MAX(t, 0.001));
    self.res[@"avg_cpu_ms_per_frame_submit_and_present"] = @(self.cpuMsSum / MAX(self.frames, 1));
    self.res[@"worst_frame_ms_after_warmup"] = @(self.worstFrameMs);
    self.res[@"log"] = self.lines;
    d3d9_scene_destroy(self.scene); self.scene = NULL;
    S.DestroyWindow(self.win); self.win = NULL;
    self.res[@"status"] = @"finished";
    [[PBResults shared] setSection:@"d3d9" value:[self.res copy]];
    PBLog(@"d3d9 test finished: selftest=%@ avg_fps=%@", st == 0 ? @"PASS" : @"FAIL", self.res[@"avg_fps"]);
    void (^d)(NSDictionary *) = self.done; NSDictionary *r = [self.res copy];
    dispatch_async(dispatch_get_main_queue(), ^{ d(r); });
}
@end

static PBD3D9Driver *gDriver;

void PBRunD3D9Test(double seconds, void (^done)(NSDictionary *)) {
    NSMutableDictionary *res = [NSMutableDictionary dictionaryWithDictionary:@{ @"status": @"starting" }];
    [[PBResults shared] setSection:@"d3d9" value:res];   /* if we crash/get killed the file says "starting" */
    NSString *fw = PBFrameworksPath();
    NSString *mvk = [fw stringByAppendingPathComponent:@"MoltenVK.framework/MoltenVK"];
    NSString *dxvk = [fw stringByAppendingPathComponent:@"libdxvk_d3d9.0.dylib"];
    for (NSString *f in @[mvk, dxvk, [fw stringByAppendingPathComponent:@"libSDL3.0.dylib"]])
        if (![[NSFileManager defaultManager] fileExistsAtPath:f]) {
            res[@"status"] = @"missing libraries"; res[@"error"] = [NSString stringWithFormat:@"not bundled: %@ (build with scripts/build_ipa.sh --with-d3d9)", f];
            [[PBResults shared] setSection:@"d3d9" value:res]; done(res); return;
        }
    if (!loadSDL(fw, res)) { res[@"status"] = @"failed"; [[PBResults shared] setSection:@"d3d9" value:res]; done(res); return; }

    /* environment DXVK / SDL read. Absolute paths: LiveContainer rewrites @executable_path, so do not rely on it. */
    /* DXVK also logs to stderr: capture it (LiveContainer shows nothing otherwise) */
    {
        NSString *errLog = [PBDocumentsPath() stringByAppendingPathComponent:@"dxvk_stderr.log"];
        freopen(errLog.fileSystemRepresentation, "w", stderr);
        setvbuf(stderr, NULL, _IONBF, 0);
    }
    setenv("DXVK_WSI_DRIVER", "SDL3", 1);
    setenv("DXVK_VULKAN_LIBRARY", mvk.UTF8String, 1);
    setenv("SDL_VULKAN_LIBRARY", mvk.UTF8String, 1);
    setenv("DXVK_LOG_PATH", PBDocumentsPath().UTF8String, 1);
    setenv("DXVK_LOG_LEVEL", "info", 1);
    NSString *cache = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@"dxvk"];
    [[NSFileManager defaultManager] createDirectoryAtPath:cache withIntermediateDirectories:YES attributes:nil error:nil];
    setenv("DXVK_STATE_CACHE_PATH", cache.UTF8String, 1);

    S.SetMainReady();
    S.SetHint("SDL_VULKAN_LIBRARY", mvk.UTF8String);
    if (!S.Init(0x20 /*SDL_INIT_VIDEO*/)) { res[@"error"] = [NSString stringWithFormat:@"SDL_Init: %s", S.GetError()]; res[@"status"] = @"failed"; [[PBResults shared] setSection:@"d3d9" value:res]; done(res); return; }
    SDL_Window *win = S.CreateWindow("d3d9", 0, 0, 0x1ull /*FULLSCREEN*/ | 0x2000ull /*HIGH_PIXEL_DENSITY*/ | 0x10000000ull /*VULKAN*/);
    if (!win) { res[@"error"] = [NSString stringWithFormat:@"SDL_CreateWindow: %s", S.GetError()]; res[@"status"] = @"failed"; [[PBResults shared] setSection:@"d3d9" value:res]; done(res); return; }
    int w = 0, h = 0; S.GetWindowSizeInPixels(win, &w, &h);
    res[@"window_pixels"] = @{ @"w": @(w), @"h": @(h) };

    void *lib = dlopen(dxvk.UTF8String, RTLD_NOW | RTLD_LOCAL);
    void *create9 = lib ? dlsym(lib, "Direct3DCreate9") : NULL;
    if (!create9) { res[@"error"] = [NSString stringWithFormat:@"dlopen/dlsym DXVK: %s", dlerror() ?: "Direct3DCreate9 missing"]; res[@"status"] = @"failed"; S.DestroyWindow(win); [[PBResults shared] setSection:@"d3d9" value:res]; done(res); return; }

    PBD3D9Driver *d = [PBD3D9Driver new];
    d.res = res; d.lines = [NSMutableArray array]; d.done = done; d.win = win;
    d3d9_scene_desc desc = { .create9 = (void *(*)(unsigned))create9, .window = win, .width = w, .height = h, .vsync = 1 };
    char err[512] = {0};
    d3d9_scene *sc = NULL;
    res[@"status"] = @"creating device"; [[PBResults shared] setSection:@"d3d9" value:res];
    if (d3d9_scene_create(&desc, &sc, err, sizeof err, logcb, (__bridge void *)d)) {
        res[@"error"] = @(err); res[@"status"] = @"failed"; res[@"log"] = d.lines;
        [[PBResults shared] setSection:@"d3d9" value:res];
        if (sc) d3d9_scene_destroy(sc);
        S.DestroyWindow(win); done(res); return;
    }
    d.scene = sc; d.t0 = CACurrentMediaTime(); d.tEnd = d.t0 + seconds;
    res[@"status"] = @"rendering"; [[PBResults shared] setSection:@"d3d9" value:res];
    gDriver = d;
    d.link = [CADisplayLink displayLinkWithTarget:d selector:@selector(tick:)];
    [d.link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}
