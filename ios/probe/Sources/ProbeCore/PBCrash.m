// Launch breadcrumbs + crash capture. Everything lands in Documents so the next launch (or the Files app) can show it:
//   probe_boot.txt   steps reached this launch (previous launch's copy: probe_boot_previous.txt)
//   probe_crash.txt  signal / uncaught exception with backtrace (read and cleared by PBTakePreviousCrash)
#import "PBCommon.h"
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <mach-o/dyld.h>

static int gBootFd = -1, gCrashFd = -1;
/* previous handlers (LiveContainer installs its own crash reporter): we record, then chain to them */
static struct sigaction gPrevSig[NSIG];
static NSUncaughtExceptionHandler *gPrevExc;
static char gCrashPath[1024];

static void writeStr(int fd, const char *s) { if (fd >= 0) (void)!write(fd, s, strlen(s)); }

void PBBoot(const char *step) {
    writeStr(gBootFd, step); writeStr(gBootFd, "\n");
    if (gBootFd >= 0) fsync(gBootFd);
}

static void openCrashFd(void) {
    if (gCrashFd < 0) gCrashFd = open(gCrashPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

static void dumpImages(int fd) {
    char line[1200];
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *n = _dyld_get_image_name(i);
        if (!n || strstr(n, "/System/") || strstr(n, "/usr/lib/")) continue;   /* only our / LiveContainer images */
        snprintf(line, sizeof line, "image %p slide 0x%lx %s\n", (void *)_dyld_get_image_header(i), (long)_dyld_get_image_vmaddr_slide(i), n);
        writeStr(fd, line);
    }
}

static void onSignal(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    openCrashFd();
    char line[256];
    snprintf(line, sizeof line, "signal %d (%s) addr %p\n", sig, strsignal(sig), si ? si->si_addr : NULL);
    writeStr(gCrashFd, line);
    void *bt[64]; int n = backtrace(bt, 64);
    backtrace_symbols_fd(bt, n, gCrashFd);
    dumpImages(gCrashFd);
    fsync(gCrashFd);
    /* hand the signal to whoever was installed before us (LiveContainer's log), else default */
    sigaction(sig, &gPrevSig[sig], NULL);
    raise(sig);
}

static void onException(NSException *e) {
    openCrashFd();
    NSString *s = [NSString stringWithFormat:@"uncaught %@: %@\n%@\n", e.name, e.reason, [e.callStackSymbols componentsJoinedByString:@"\n"]];
    writeStr(gCrashFd, s.UTF8String);
    dumpImages(gCrashFd);
    fsync(gCrashFd);
    if (gPrevExc) gPrevExc(e);
}

__attribute__((constructor)) static void PBCrashInit(void) {
    @autoreleasepool {
        NSString *docs = PBDocumentsPath();
        if (!docs) return;
        [[NSFileManager defaultManager] createDirectoryAtPath:docs withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *boot = [docs stringByAppendingPathComponent:@"probe_boot.txt"];
        NSString *prev = [docs stringByAppendingPathComponent:@"probe_boot_previous.txt"];
        [[NSFileManager defaultManager] removeItemAtPath:prev error:nil];
        [[NSFileManager defaultManager] moveItemAtPath:boot toPath:prev error:nil];
        gBootFd = open(boot.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        strlcpy(gCrashPath, [docs stringByAppendingPathComponent:@"probe_crash.txt"].fileSystemRepresentation, sizeof gCrashPath);
    }
    PBBoot("constructor: probe image loaded");
    gPrevExc = NSGetUncaughtExceptionHandler();
    NSSetUncaughtExceptionHandler(onException);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = onSignal; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE, SIGTRAP };
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, &gPrevSig[sigs[i]]);
}

NSString *PBTakePreviousCrash(void) {
    NSString *docs = PBDocumentsPath();
    NSString *p = [docs stringByAppendingPathComponent:@"probe_crash.txt"];
    NSString *s = [NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:nil];
    if (!s.length) return nil;
    NSString *bootPrev = [NSString stringWithContentsOfFile:[docs stringByAppendingPathComponent:@"probe_boot_previous.txt"] encoding:NSUTF8StringEncoding error:nil];
    NSString *keep = [docs stringByAppendingPathComponent:@"probe_crash_previous.txt"];
    [[NSFileManager defaultManager] removeItemAtPath:keep error:nil];
    [[NSFileManager defaultManager] moveItemAtPath:p toPath:keep error:nil];
    return [NSString stringWithFormat:@"%@\n-- steps reached --\n%@", s, bootPrev ?: @"(none)"];
}
