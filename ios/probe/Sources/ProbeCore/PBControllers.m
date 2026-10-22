#import "PBControllers.h"
#import "PBCommon.h"
#import <GameController/GameController.h>

@implementation PBControllers {
    NSMutableDictionary<NSString *, NSMutableSet<NSString *> *> *_seen; /* controller key -> elements seen active */
    NSMutableArray<NSString *> *_events;
}
+ (instancetype)shared { static PBControllers *c; static dispatch_once_t o; dispatch_once(&o, ^{ c = [PBControllers new]; }); return c; }

static NSString *keyFor(GCController *c, NSUInteger idx) { return [NSString stringWithFormat:@"%lu:%@", (unsigned long)idx, c.vendorName ?: @"?"]; }

- (void)start {
    _seen = [NSMutableDictionary dictionary]; _events = [NSMutableArray array];
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    __weak typeof(self) ws = self;
    [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        GCController *c = n.object; PBLog(@"controller CONNECTED: %@ / category=%@ / profile=%@", c.vendorName, c.productCategory, NSStringFromClass([c.extendedGamepad class]));
        [[ws events] addObject:[NSString stringWithFormat:@"connected %@ (%@)", c.vendorName, c.productCategory]];
        [ws publish];
    }];
    [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        GCController *c = n.object; PBLog(@"controller DISCONNECTED: %@", c.vendorName);
        [[ws events] addObject:[NSString stringWithFormat:@"disconnected %@", c.vendorName]];
        [ws publish];
    }];
    if (@available(iOS 16.0, *)) {} /* GCController.shouldMonitorBackgroundEvents is irrelevant here */
    PBBoot("controllers: observers ok");
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:^{}];
    PBBoot("controllers: discovery started");
    PBLog(@"GameController: initial controllers = %lu", (unsigned long)[GCController controllers].count);
    [self publish];
}
- (NSMutableArray *)events { return _events; }

static NSString *fmtF(float v) { return [NSString stringWithFormat:@"%+.2f", v]; }

/* All elements via physicalInputProfile: this covers extra buttons (Backbone capture/share/etc.) generically. */
- (void)enumerate:(GCController *)c key:(NSString *)key into:(NSMutableString *)out {
    NSMutableSet *seen = _seen[key] ?: (_seen[key] = [NSMutableSet set]);
    GCPhysicalInputProfile *p = c.physicalInputProfile;
    NSArray *bnames = [p.buttons.allKeys sortedArrayUsingSelector:@selector(compare:)];
    [out appendString:@" buttons:\n"];
    for (NSString *k in bnames) {
        GCControllerButtonInput *b = p.buttons[k];
        if (b.isPressed || b.value > 0.05) [seen addObject:[@"btn " stringByAppendingString:k]];
        [out appendFormat:@"  %@ %-28s %@\n", b.isPressed ? @"[#]" : @"[ ]", k.UTF8String, fmtF(b.value)];
    }
    NSArray *dnames = [p.dpads.allKeys sortedArrayUsingSelector:@selector(compare:)];
    for (NSString *k in dnames) {
        GCControllerDirectionPad *d = p.dpads[k];
        if (fabsf(d.xAxis.value) > 0.2 || fabsf(d.yAxis.value) > 0.2) [seen addObject:[@"dpad " stringByAppendingString:k]];
        [out appendFormat:@" dpad %-24s x=%@ y=%@\n", k.UTF8String, fmtF(d.xAxis.value), fmtF(d.yAxis.value)];
    }
    NSArray *anames = [p.axes.allKeys sortedArrayUsingSelector:@selector(compare:)];
    for (NSString *k in anames) {
        GCControllerAxisInput *a = p.axes[k];
        if (fabsf(a.value) > 0.2) [seen addObject:[@"axis " stringByAppendingString:k]];
        [out appendFormat:@" axis %-24s %@\n", k.UTF8String, fmtF(a.value)];
    }
}

- (NSString *)liveText {
    NSArray<GCController *> *cs = [GCController controllers];
    NSMutableString *s = [NSMutableString string];
    if (!cs.count) return @"No controllers connected.\nConnect the Backbone (or pair a pad) and press any button.\n";
    NSUInteger i = 0;
    for (GCController *c in cs) {
        NSString *key = keyFor(c, i++);
        [s appendFormat:@"== %@  category=%@  profile=%@\n", c.vendorName, c.productCategory, NSStringFromClass([c.extendedGamepad class]) ?: @"(no extendedGamepad)"];
        GCExtendedGamepad *g = c.extendedGamepad;
        if (g) {
            [s appendFormat:@" ext: A%d B%d X%d Y%d L1:%d R1:%d L2:%@ R2:%@ LS(%@,%@) RS(%@,%@) dpad(%@,%@)\n",
             g.buttonA.isPressed, g.buttonB.isPressed, g.buttonX.isPressed, g.buttonY.isPressed,
             g.leftShoulder.isPressed, g.rightShoulder.isPressed, fmtF(g.leftTrigger.value), fmtF(g.rightTrigger.value),
             fmtF(g.leftThumbstick.xAxis.value), fmtF(g.leftThumbstick.yAxis.value),
             fmtF(g.rightThumbstick.xAxis.value), fmtF(g.rightThumbstick.yAxis.value),
             fmtF(g.dpad.xAxis.value), fmtF(g.dpad.yAxis.value)];
            [s appendFormat:@" ext: menu:%d options:%@ home:%@ L3:%@ R3:%@\n", g.buttonMenu.isPressed,
             g.buttonOptions ? @(g.buttonOptions.isPressed) : @"n/a", g.buttonHome ? @(g.buttonHome.isPressed) : @"n/a",
             g.leftThumbstickButton ? @(g.leftThumbstickButton.isPressed) : @"n/a", g.rightThumbstickButton ? @(g.rightThumbstickButton.isPressed) : @"n/a"];
        }
        if (c.battery) [s appendFormat:@" battery: %.0f%% state=%ld\n", c.battery.batteryLevel * 100, (long)c.battery.batteryState];
        [self enumerate:c key:key into:s];
        [s appendString:@"\n"];
    }
    return s;
}

- (NSDictionary *)summary {
    NSMutableArray *list = [NSMutableArray array];
    NSUInteger i = 0;
    for (GCController *c in [GCController controllers]) {
        NSString *key = keyFor(c, i++);
        GCPhysicalInputProfile *p = c.physicalInputProfile;
        GCExtendedGamepad *g = c.extendedGamepad;
        [list addObject:@{
            @"vendor_name": c.vendorName ?: @"", @"product_category": c.productCategory ?: @"",
            @"extended_gamepad_class": NSStringFromClass([g class]) ?: @"none",
            @"has_extended_gamepad": @(g != nil),
            @"buttonOptions_present": @(g.buttonOptions != nil), @"buttonHome_present": @(g.buttonHome != nil),
            @"thumbstick_buttons_present": @(g.leftThumbstickButton != nil && g.rightThumbstickButton != nil),
            @"buttons": [p.buttons.allKeys sortedArrayUsingSelector:@selector(compare:)],
            @"dpads": [p.dpads.allKeys sortedArrayUsingSelector:@selector(compare:)],
            @"axes": [p.axes.allKeys sortedArrayUsingSelector:@selector(compare:)],
            @"elements_seen_active": [[_seen[key] allObjects] sortedArrayUsingSelector:@selector(compare:)] ?: @[],
        }];
    }
    return @{ @"controllers": list, @"events": [_events copy] ?: @[] };
}
- (void)publish { [[PBResults shared] setSection:@"controllers" value:[self summary]]; }
@end
