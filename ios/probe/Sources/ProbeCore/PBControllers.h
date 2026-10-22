#import <Foundation/Foundation.h>
NS_ASSUME_NONNULL_BEGIN
/// Tracks GameController.framework controllers; produces a live text dump and a JSON-able summary.
@interface PBControllers : NSObject
+ (instancetype)shared;
- (void)start;
/// Multi-line live readout of every controller's buttons, axes and dpads (call at display rate).
- (NSString *)liveText;
/// Summary incl. every element that was ever pressed/moved since launch.
- (NSDictionary *)summary;
@end
NS_ASSUME_NONNULL_END
