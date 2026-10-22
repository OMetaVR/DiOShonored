#import <UIKit/UIKit.h>
NS_ASSUME_NONNULL_BEGIN
/// Runs the shared d3d9 scene (see tools/spikes/d3d9_test) through DXVK -> MoltenVK -> Metal inside an SDL3 window.
/// Needs <bundle>/Frameworks/{libSDL3.0.dylib, libdxvk_d3d9.0.dylib, MoltenVK.framework/MoltenVK}; all are dlopen'ed,
/// so the app still launches (and every other probe works) when they are absent.
/// Tap the screen to end early; otherwise it runs for `seconds`.
void PBRunD3D9Test(double seconds, void (^done)(NSDictionary *result));
NS_ASSUME_NONNULL_END
