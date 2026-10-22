#import "ProbeCore.h"
#import "PBCommon.h"
#import "PBControllers.h"
#import "PBDeviceProbe.h"
#import "PBMemoryRunner.h"
#import "PBD3D9Runner.h"

@interface PBViewController : UIViewController
@end

@implementation PBViewController {
    UITextView *_text;
    UISegmentedControl *_seg;
    CADisplayLink *_link;
    NSUInteger _tick;
    UIButton *_memBtn, *_d3dBtn;
}

- (UIButton *)button:(NSString *)title action:(SEL)sel {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    b.titleLabel.font = [UIFont boldSystemFontOfSize:14];
    b.backgroundColor = [UIColor colorWithWhite:0.2 alpha:1];
    b.layer.cornerRadius = 8;
    [b addTarget:self action:sel forControlEvents:UIControlEventTouchUpInside];
    return b;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];
    UILabel *title = [UILabel new]; title.text = [NSString stringWithFormat:@"Dishonored iOS - platform probe (build %s %s)", __DATE__, __TIME__];
    title.textColor = [UIColor whiteColor]; title.font = [UIFont boldSystemFontOfSize:16];

    if ([[[NSBundle mainBundle] objectForInfoDictionaryKey:@"PBMinimal"] boolValue]) {
        /* bisect build: UI shell only, no probes, no display link, no frameworks */
        PBBoot("minimal mode");
        title.text = @"DIOS Probe (minimal) launched OK";
        title.numberOfLines = 0; title.textAlignment = NSTextAlignmentCenter;
        title.translatesAutoresizingMaskIntoConstraints = NO;
        [self.view addSubview:title];
        [NSLayoutConstraint activateConstraints:@[
            [title.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
            [title.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        ]];
        return;
    }
    _memBtn = [self button:@"Run memory probe" action:@selector(runMemory)];
    _d3dBtn = [self button:@"Run D3D9 test" action:@selector(runD3D9)];
    UIButton *metal = [self button:@"Re-run Metal probe" action:@selector(runMetal)];
    UIButton *share = [self button:@"Share results JSON" action:@selector(share)];
    UIStackView *row1 = [[UIStackView alloc] initWithArrangedSubviews:@[_memBtn, _d3dBtn]];
    UIStackView *row2 = [[UIStackView alloc] initWithArrangedSubviews:@[metal, share]];
    for (UIStackView *r in @[row1, row2]) { r.distribution = UIStackViewDistributionFillEqually; r.spacing = 8; }

    _seg = [[UISegmentedControl alloc] initWithItems:@[@"Log", @"Controllers", @"Results JSON"]];
    _seg.selectedSegmentIndex = 0;
    [_seg addTarget:self action:@selector(segChanged) forControlEvents:UIControlEventValueChanged];

    _text = [UITextView new];
    _text.editable = NO; _text.backgroundColor = [UIColor colorWithWhite:0.08 alpha:1]; _text.textColor = [UIColor colorWithRed:0.6 green:1 blue:0.6 alpha:1];
    _text.font = [UIFont monospacedSystemFontOfSize:10.5 weight:UIFontWeightRegular];
    _text.layer.cornerRadius = 6;

    UIStackView *col = [[UIStackView alloc] initWithArrangedSubviews:@[title, row1, row2, _seg, _text]];
    col.axis = UILayoutConstraintAxisVertical; col.spacing = 8; col.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:col];
    UILayoutGuide *g = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [col.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:8], [col.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-8],
        [col.topAnchor constraintEqualToAnchor:g.topAnchor constant:4], [col.bottomAnchor constraintEqualToAnchor:g.bottomAnchor constant:-4],
        [row1.heightAnchor constraintEqualToConstant:40], [row2.heightAnchor constraintEqualToConstant:40],
    ]];

    PBBoot("ui built");
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(logChanged) name:PBLogNotification object:nil];
    PBBoot("begin:displaylink");
    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame)];
    _link.preferredFramesPerSecond = 30;
    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];

    PBBoot("end:displaylink");
    PBBoot("viewDidLoad done");
    /* run probes after the first frame is on screen, so a probe crash can't look like a launch crash */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        @try { [self launchProbes]; }
        @catch (NSException *e) { PBLog(@"launchProbes threw %@: %@", e.name, e.reason); }
    });
}

/* Auto-bisect: every probe step writes begin:/end: breadcrumbs. A step that began but never ended on the
 * previous launch crashed the app, so it is skipped from then on (remembered in NSUserDefaults). */
- (NSSet<NSString *> *)skippedSteps {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    NSMutableSet *skip = [NSMutableSet setWithArray:[ud arrayForKey:@"PBSkippedSteps"] ?: @[]];
    NSString *prev = [NSString stringWithContentsOfFile:[PBDocumentsPath() stringByAppendingPathComponent:@"probe_boot_previous.txt"]
                                               encoding:NSUTF8StringEncoding error:nil];
    NSMutableSet *begun = [NSMutableSet set], *ended = [NSMutableSet set];
    for (NSString *line in [prev componentsSeparatedByString:@"\n"]) {
        if ([line hasPrefix:@"begin:"]) [begun addObject:[line substringFromIndex:6]];
        if ([line hasPrefix:@"end:"]) [ended addObject:[line substringFromIndex:4]];
    }
    [begun minusSet:ended];
    for (NSString *step in begun) PBLog(@"PREVIOUS RUN CRASHED inside step '%@' - skipping it from now on", step);
    [skip unionSet:begun];
    [ud setObject:skip.allObjects forKey:@"PBSkippedSteps"];
    return skip;
}

- (void)step:(NSString *)name skip:(NSSet *)skip run:(void (^)(void))block {
    if ([skip containsObject:name]) { PBLog(@"step '%@' SKIPPED (crashed on an earlier launch)", name); return; }
    PBBoot([@"begin:" stringByAppendingString:name].UTF8String);
    @try { block(); }
    @catch (NSException *e) { PBLog(@"step '%@' threw %@: %@", name, e.name, e.reason); }
    PBBoot([@"end:" stringByAppendingString:name].UTF8String);
}

- (void)launchProbes {
    NSSet *skip = [self skippedSteps];
    [[PBResults shared] setSection:@"skipped_steps" value:skip.allObjects];
    __block PBResults *r = nil;
    [self step:@"results" skip:skip run:^{
        r = [PBResults shared];
        NSString *crash = PBTakePreviousCrash();
        if (crash) { PBLog(@"PREVIOUS RUN CRASH REPORT:\n%@", crash); [r setSection:@"previous_crash" value:crash]; }
        if (r.previousRun) {
            NSDictionary *m = r.previousRun[@"memory"];
            if ([m isKindOfClass:[NSDictionary class]] && [m[@"status"] isEqual:@"running"])
                PBLog(@"PREVIOUS RUN: memory probe did NOT finish - most likely jetsam-killed at ~%@ MB committed", m[@"last_committed_mb"]);
            NSString *prevPath = [PBDocumentsPath() stringByAppendingPathComponent:@"probe_results_previous.json"];
            NSData *d0 = [NSData dataWithContentsOfFile:r.path]; if (d0) [d0 writeToFile:prevPath atomically:YES];
        }
    }];
    [self step:@"device" skip:skip run:^{
        NSDictionary *dev = PBCollectDeviceInfo();
        [[PBResults shared] setSection:@"device" value:dev];
        PBLog(@"device: %@ | %@ | RAM %@ MB | livecontainer guest: %@", dev[@"hw_machine"], dev[@"os_version"], dev[@"physical_memory_mb"], dev[@"likely_livecontainer_guest"]);
        PBLog(@"avail memory at launch: %.0f MB", [self availMB]);
    }];
    [self step:@"metal" skip:skip run:^{ [self runMetal]; }];
    [self step:@"controllers" skip:skip run:^{ [[PBControllers shared] start]; }];
    PBBoot("probes done");
    PBLog(@"launch probes finished (skipped: %@)", skip.count ? [skip.allObjects componentsJoinedByString:@", "] : @"none");
}

- (double)availMB { extern size_t os_proc_available_memory(void); return os_proc_available_memory() / 1048576.0; }

- (void)runMetal {
    PBBoot("metal probe start");
    NSDictionary *m = nil;
    @try { m = PBRunMetalProbe(); }
    @catch (NSException *e) { PBLog(@"metal probe threw %@: %@", e.name, e.reason); return; }
    PBBoot("metal probe done");
    [[PBResults shared] setSection:@"metal" value:m];
    PBLog(@"metal: %@ | highest family %@ | BC native: %@ | MetalFX: %@", m[@"name"], m[@"highest_apple_family"], m[@"supportsBCTextureCompression"], m[@"metalfx"]);
}

- (void)runMemory {
    _memBtn.enabled = NO;
    PBLog(@"memory probe starting (results are flushed every step; if the app dies, relaunch to see how far it got)");
    PBRunMemoryProbe(^(NSDictionary *r) {
        self->_memBtn.enabled = YES;
        PBLog(@"memory probe done: max safely committed %@ MB, implied limit %@ MB, stop: %@", r[@"max_safely_committed_mb"], r[@"implied_jetsam_limit_mb"], r[@"stop_reason"]);
    });
}

- (void)runD3D9 {
    _d3dBtn.enabled = NO;
    PBLog(@"D3D9 test starting: an SDL window will cover the screen for ~8 s (tap to end early)");
    PBRunD3D9Test(8.0, ^(NSDictionary *r) {
        self->_d3dBtn.enabled = YES;
        PBLog(@"D3D9 test: %@ %@", r[@"status"], r[@"error"] ?: (r[@"selftest_pass"] ? @"selftest ok" : @"selftest FAILED"));
    });
}

- (void)share {
    NSURL *u = [NSURL fileURLWithPath:[PBResults shared].path];
    UIActivityViewController *a = [[UIActivityViewController alloc] initWithActivityItems:@[u] applicationActivities:nil];
    a.popoverPresentationController.sourceView = self.view;
    [self presentViewController:a animated:YES completion:nil];
}

- (void)segChanged { [self refreshText]; }
- (void)logChanged { if (_seg.selectedSegmentIndex == 0) [self refreshText]; }
- (void)frame {
    _tick++;
    if (_seg.selectedSegmentIndex == 1) [self refreshText];
    if (_tick == 1) PBBoot("first frame");
    if (_tick % 60 == 0) [[PBResults shared] setSection:@"controllers" value:[[PBControllers shared] summary]];
}
- (void)refreshText {
    NSString *s;
    switch (_seg.selectedSegmentIndex) {
        case 0: s = PBLogText(); break;
        case 1: s = [[PBControllers shared] liveText]; break;
        default: s = [[PBResults shared] jsonString]; break;
    }
    if ([s isEqualToString:_text.text]) return;
    BOOL atEnd = _seg.selectedSegmentIndex == 0;
    _text.text = s;
    if (atEnd && s.length) [_text scrollRangeToVisible:NSMakeRange(s.length - 1, 1)];
}
- (BOOL)prefersStatusBarHidden { return YES; }
@end

@implementation PBAppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
    PBBoot("didFinishLaunching");
    return YES;
}
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)session
                              options:(UISceneConnectionOptions *)options {
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass = [PBSceneDelegate class];
    return c;
}
@end

@implementation PBSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    PBBoot("scene willConnect");
    if (![scene isKindOfClass:[UIWindowScene class]]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [PBViewController new];
    [self.window makeKeyAndVisible];
    PBBoot("window visible");
}
@end
