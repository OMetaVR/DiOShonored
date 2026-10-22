#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

extern NSNotificationName const PBLogNotification;

/// Append a line to the in-memory log, Documents/probe_log.txt and notify the UI.
void PBLog(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);
NSString *PBLogText(void);

/// Results store -> Documents/probe_results.json (rewritten atomically on every change).
@interface PBResults : NSObject
+ (instancetype)shared;
@property (readonly) NSString *path;
- (void)setSection:(NSString *)name value:(id)value;
- (nullable id)sectionNamed:(NSString *)name;
- (NSString *)jsonString;
/// The JSON as found on disk at launch, i.e. from the previous run (detects jetsam kills mid-probe).
@property (readonly, nullable) NSDictionary *previousRun;
@end

NSString *PBDocumentsPath(void);
/// Launch breadcrumb (fsync'd line in Documents/probe_boot.txt). Safe to call very early.
void PBBoot(const char *step);
/// Crash report left by the previous launch (signal or uncaught exception + steps reached), or nil.
NSString *_Nullable PBTakePreviousCrash(void);
NSString *PBFrameworksPath(void);

NS_ASSUME_NONNULL_END
