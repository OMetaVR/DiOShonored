#import <Foundation/Foundation.h>
NS_ASSUME_NONNULL_BEGIN
/// Runs the memory probe on a background queue. Results are flushed to the JSON file after every step, so a
/// jetsam kill mid-probe still leaves the data on disk (status stays "running" and is reported next launch).
void PBRunMemoryProbe(void (^done)(NSDictionary *result));
NS_ASSUME_NONNULL_END
