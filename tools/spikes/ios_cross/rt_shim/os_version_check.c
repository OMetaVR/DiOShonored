/* Minimal replacement for compiler-rt's Darwin os_version_check.c.
 * Linux clang ships no libclang_rt.ios, but every @available(...)/API_AVAILABLE check and
 * -Wunguarded-availability guard compiles to a call to __isPlatformVersionAtLeast.
 * dyld/libSystem exports __availability_version_check, which is what the real runtime calls. */
#include <stdint.h>
#include <stdbool.h>

typedef struct { uint32_t platform; uint32_t version; } dyld_build_version_t; /* version = major<<16 | minor<<8 | patch */
extern bool _availability_version_check(uint32_t count, dyld_build_version_t versions[]);

int32_t __isPlatformVersionAtLeast(uint32_t platform, uint32_t major, uint32_t minor, uint32_t subminor) {
    dyld_build_version_t v = { platform, (major << 16) | (minor << 8) | subminor };
    return _availability_version_check(1, &v);
}

/* Old-style entry point (pre-LLVM 11 codegen / some prebuilt objects). Platform is implicit (iOS = 2). */
int32_t __isOSVersionAtLeast(int32_t major, int32_t minor, int32_t subminor) {
    return __isPlatformVersionAtLeast(2, (uint32_t)major, (uint32_t)minor, (uint32_t)subminor);
}
