#import <Foundation/Foundation.h>
NS_ASSUME_NONNULL_BEGIN
/// Device / OS / LiveContainer environment facts. Safe to run at launch.
NSDictionary *PBCollectDeviceInfo(void);
/// Metal capability probe (GPU family, BC/ASTC, MetalFX, limits). Safe to run at launch.
NSDictionary *PBRunMetalProbe(void);
NS_ASSUME_NONNULL_END
