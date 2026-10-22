#import "PBCommon.h"
#import <UIKit/UIKit.h>

NSNotificationName const PBLogNotification = @"PBLogNotification";

static NSMutableString *gLog;
static dispatch_queue_t gLogQ;

NSString *PBDocumentsPath(void) {
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
}
NSString *PBFrameworksPath(void) {
    /* bisect: some builds ship the libraries in Libs/ instead of Frameworks/ (LiveContainer treats Frameworks specially) */
    NSString *libs = [[[NSBundle mainBundle] bundlePath] stringByAppendingPathComponent:@"Libs"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:libs]) return libs;
    return [[NSBundle mainBundle] privateFrameworksPath] ?: [[[NSBundle mainBundle] bundlePath] stringByAppendingPathComponent:@"Frameworks"];
}

void PBLog(NSString *fmt, ...) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ gLog = [NSMutableString string]; gLogQ = dispatch_queue_create("pb.log", DISPATCH_QUEUE_SERIAL); });
    va_list ap; va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    NSLog(@"[probe] %@", s);
    dispatch_async(gLogQ, ^{
        NSDateFormatter *df = [NSDateFormatter new]; df.dateFormat = @"HH:mm:ss.SSS";
        NSString *line = [NSString stringWithFormat:@"%@ %@\n", [df stringFromDate:[NSDate date]], s];
        [gLog appendString:line];
        NSString *p = [PBDocumentsPath() stringByAppendingPathComponent:@"probe_log.txt"];
        NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:p];
        if (!h) { [line writeToFile:p atomically:NO encoding:NSUTF8StringEncoding error:nil]; }
        else { [h seekToEndOfFile]; [h writeData:[line dataUsingEncoding:NSUTF8StringEncoding]]; [h closeFile]; }
        dispatch_async(dispatch_get_main_queue(), ^{ [[NSNotificationCenter defaultCenter] postNotificationName:PBLogNotification object:nil]; });
    });
}
NSString *PBLogText(void) {
    __block NSString *t = @"";
    if (gLogQ) dispatch_sync(gLogQ, ^{ t = [gLog copy]; });
    return t;
}

@implementation PBResults {
    NSMutableDictionary *_data;
    dispatch_queue_t _q;
}
+ (instancetype)shared {
    static PBResults *r; static dispatch_once_t once;
    dispatch_once(&once, ^{ r = [PBResults new]; });
    return r;
}
- (instancetype)init {
    if ((self = [super init])) {
        _q = dispatch_queue_create("pb.results", DISPATCH_QUEUE_SERIAL);
        _path = [PBDocumentsPath() stringByAppendingPathComponent:@"probe_results.json"];
        NSData *old = [NSData dataWithContentsOfFile:_path];
        if (old) _previousRun = [NSJSONSerialization JSONObjectWithData:old options:0 error:nil];
        _data = [NSMutableDictionary dictionary];
    }
    return self;
}
- (void)setSection:(NSString *)name value:(id)value {
    if (!value) value = [NSNull null];
    if (![NSJSONSerialization isValidJSONObject:@{ @"v": value }]) value = [value description] ?: @"(invalid)";
    dispatch_sync(_q, ^{
        self->_data[name] = value;
        self->_data[@"written_at"] = [[NSISO8601DateFormatter new] stringFromDate:[NSDate date]];
        NSData *d = [NSJSONSerialization dataWithJSONObject:self->_data options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nil];
        [d writeToFile:self->_path atomically:YES];
    });
}
- (id)sectionNamed:(NSString *)name { __block id v; dispatch_sync(_q, ^{ v = self->_data[name]; }); return v; }
- (NSString *)jsonString {
    __block NSString *s;
    dispatch_sync(_q, ^{
        NSData *d = [NSJSONSerialization dataWithJSONObject:self->_data options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nil];
        s = d ? [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] : @"{}";
    });
    return s;
}
@end
