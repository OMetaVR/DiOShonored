#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

/// Application delegate with the whole probe UI. The Swift target subclasses it with @main;
/// the clang-only build (scripts/build_ipa_linux.sh) uses it from main.m.
@interface PBAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong, nullable) UIWindow *window;
@end

/// Scene delegate (UIScene lifecycle; required with the iOS 27 SDK). Owns the window.
@interface PBSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong, nullable) UIWindow *window;
@end

NS_ASSUME_NONNULL_END
