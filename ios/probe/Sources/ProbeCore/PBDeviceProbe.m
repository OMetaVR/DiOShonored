#import "PBDeviceProbe.h"
#import "PBCommon.h"
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#import <objc/message.h>

static NSString *sysctlString(const char *name) {
    size_t n = 0; if (sysctlbyname(name, NULL, &n, NULL, 0) != 0 || !n) return @"";
    char *b = malloc(n); sysctlbyname(name, b, &n, NULL, 0);
    NSString *s = [NSString stringWithUTF8String:b]; free(b); return s ?: @"";
}
static uint64_t sysctlU64(const char *name) {
    uint64_t v = 0; size_t n = sizeof v; if (sysctlbyname(name, &v, &n, NULL, 0) != 0) { uint32_t w = 0; n = sizeof w; if (sysctlbyname(name, &w, &n, NULL, 0) == 0) return w; return 0; }
    return n == 4 ? (uint32_t)v : v;
}

NSDictionary *PBCollectDeviceInfo(void) {
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    NSProcessInfo *pi = [NSProcessInfo processInfo];
    d[@"hw_machine"] = sysctlString("hw.machine");
    d[@"hw_model"] = sysctlString("hw.model");
    d[@"os_version"] = pi.operatingSystemVersionString;
    d[@"kern_osversion"] = sysctlString("kern.osversion");
    d[@"physical_memory_mb"] = @(pi.physicalMemory >> 20);
    d[@"active_cpus"] = @(pi.activeProcessorCount);
    d[@"hw_ncpu"] = @(sysctlU64("hw.ncpu"));
    d[@"page_size"] = @(getpagesize());
    d[@"thermal_state"] = @(pi.thermalState);
    d[@"low_power_mode"] = @(pi.lowPowerModeEnabled);
    d[@"cpu_brand"] = sysctlString("machdep.cpu.brand_string");
    d[@"hw_cpufamily"] = @(sysctlU64("hw.cpufamily"));
    UIScreen *sc = [UIScreen mainScreen];
    d[@"screen_points"] = @{ @"w": @(sc.bounds.size.width), @"h": @(sc.bounds.size.height) };
    d[@"screen_native_pixels"] = @{ @"w": @(sc.nativeBounds.size.width), @"h": @(sc.nativeBounds.size.height) };
    d[@"screen_scale"] = @(sc.scale); d[@"screen_native_scale"] = @(sc.nativeScale);
    d[@"max_fps"] = @(sc.maximumFramesPerSecond);
    d[@"sandbox"] = @{
        @"bundle_path": [[NSBundle mainBundle] bundlePath] ?: @"",
        @"bundle_id": [[NSBundle mainBundle] bundleIdentifier] ?: @"",
        @"home": NSHomeDirectory(),
        @"documents": PBDocumentsPath(),
        @"frameworks_dir": PBFrameworksPath(),
        @"frameworks_exists": @([[NSFileManager defaultManager] fileExistsAtPath:PBFrameworksPath()]),
    };
    char exe[2048]; uint32_t sz = sizeof exe;
    d[@"executable_path"] = _NSGetExecutablePath(exe, &sz) == 0 ? @(exe) : @"?";
    /* LiveContainer detection: env keys it sets, plus the host bundle id from Info.plist */
    NSMutableDictionary *lc = [NSMutableDictionary dictionary];
    for (NSString *k in pi.environment) if ([k hasPrefix:@"LC_"] || [k hasPrefix:@"LIVECONTAINER"]) lc[k] = pi.environment[k];
    d[@"livecontainer_env"] = lc;
    d[@"likely_livecontainer_guest"] = @(lc.count > 0 || [d[@"executable_path"] containsString:@"LiveContainer"] || [NSHomeDirectory() containsString:@"LiveContainer"]);
    d[@"dyld_image_count"] = @(_dyld_image_count());
    NSMutableArray *interesting = [NSMutableArray array];
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *n = _dyld_get_image_name(i);
        if (n && (strstr(n, "LiveContainer") || strstr(n, "TweakLoader") || strstr(n, "MoltenVK") || strstr(n, "SDL3") || strstr(n, "dxvk"))) [interesting addObject:@(n)];
    }
    d[@"loaded_images_of_interest"] = interesting;
    return d;
}

static NSNumber *B(BOOL b) { return @(b); }

NSDictionary *PBRunMetalProbe(void) {
    NSMutableDictionary *m = [NSMutableDictionary dictionary];
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { m[@"error"] = @"MTLCreateSystemDefaultDevice returned nil"; return m; }
    m[@"name"] = dev.name;
    m[@"registry_id"] = @(dev.registryID);
    m[@"has_unified_memory"] = B(dev.hasUnifiedMemory);
    /* isLowPower / isHeadless / isRemovable are macOS-only: the A15 throws "unrecognized selector" */
    m[@"max_buffer_length_mb"] = @(dev.maxBufferLength >> 20);
    m[@"max_threadgroup_memory_length"] = @(dev.maxThreadgroupMemoryLength);
    m[@"max_threads_per_threadgroup"] = @{ @"w": @(dev.maxThreadsPerThreadgroup.width), @"h": @(dev.maxThreadsPerThreadgroup.height), @"d": @(dev.maxThreadsPerThreadgroup.depth) };
    { /* marked unavailable on iOS in the headers; ask the runtime whether the selector exists anyway */
        SEL sel = NSSelectorFromString(@"recommendedMaxWorkingSetSize");
        if ([dev respondsToSelector:sel]) { uint64_t (*fn)(id, SEL) = (uint64_t (*)(id, SEL))objc_msgSend; m[@"recommended_max_working_set_size_mb"] = @(fn(dev, sel) >> 20); }
        else m[@"recommended_max_working_set_size_mb"] = @"selector not available on this OS";
    }
    m[@"current_allocated_size_mb"] = @(dev.currentAllocatedSize >> 20);
    m[@"argument_buffers_tier"] = dev.argumentBuffersSupport == MTLArgumentBuffersTier2 ? @"Tier2" : @"Tier1";
    m[@"read_write_texture_tier"] = @(dev.readWriteTextureSupport); /* 0 none, 1 tier1, 2 tier2 */
    m[@"supports_32bit_float_filtering"] = B(dev.supports32BitFloatFiltering);
    m[@"supports_32bit_msaa"] = B(dev.supports32BitMSAA);
    m[@"supports_pull_model_interpolation"] = B(dev.supportsPullModelInterpolation);
    m[@"supports_shader_barycentric_coordinates"] = B(dev.supportsShaderBarycentricCoordinates);
    m[@"supports_function_pointers"] = B(dev.supportsFunctionPointers);
    m[@"supports_dynamic_libraries"] = B(dev.supportsDynamicLibraries);
    m[@"supports_raytracing"] = B(dev.supportsRaytracing);
    m[@"supports_msaa_4"] = B([dev supportsTextureSampleCount:4]);
    m[@"supports_msaa_8"] = B([dev supportsTextureSampleCount:8]);

    PBBoot("metal: device props ok");
    /* GPU families: raw values so the probe also works with SDKs that predate Apple9/10 */
    struct { const char *n; NSInteger v; } fam[] = {
        {"Apple1",1001},{"Apple2",1002},{"Apple3",1003},{"Apple4",1004},{"Apple5",1005},{"Apple6",1006},
        {"Apple7",1007},{"Apple8",1008},{"Apple9",1009},{"Apple10",1010},
        {"Common1",3001},{"Common2",3002},{"Common3",3003},{"Metal3",5001},{"Metal4",5002},
    };
    NSMutableDictionary *f = [NSMutableDictionary dictionary];
    NSString *best = @"none";
    for (size_t i = 0; i < sizeof fam / sizeof *fam; i++) {
        BOOL s = [dev supportsFamily:(MTLGPUFamily)fam[i].v];
        f[@(fam[i].n)] = B(s);
        if (s && strncmp(fam[i].n, "Apple", 5) == 0) best = @(fam[i].n);
    }
    m[@"gpu_families"] = f; m[@"highest_apple_family"] = best;

    PBBoot("metal: families ok");
    /* The question that decides the texture path: does an A15 sample BCn natively? */
    if (@available(iOS 16.4, *)) m[@"supportsBCTextureCompression"] = B(dev.supportsBCTextureCompression);
    else m[@"supportsBCTextureCompression"] = @"API unavailable (< iOS 16.4)";

    /* Behavioural cross-check: can we actually create/sample textures in these formats? */
    struct { const char *n; NSUInteger fmt; } tf[] = {
        {"BC1_RGBA",130},{"BC2_RGBA",132},{"BC3_RGBA",134},{"BC4_RUnorm",140},{"BC5_RGUnorm",142},{"BC6H_RGBUfloat",151},{"BC7_RGBAUnorm",152},
        {"ASTC_4x4_LDR",204},{"ASTC_6x6_LDR",208},{"ASTC_8x8_LDR",212},{"ETC2_RGB8",180},{"RGBA8Unorm",70},{"BGRA8Unorm",80},
        {"RGB10A2Unorm",90},{"RG11B10Float",92},{"RGBA16Float",115},{"Depth32Float_Stencil8",260},
    };
    NSMutableDictionary *tx = [NSMutableDictionary dictionary];
    BOOL bcOK = NO;
    if (@available(iOS 16.4, *)) bcOK = dev.supportsBCTextureCompression;
    for (size_t i = 0; i < sizeof tf / sizeof *tf; i++) {
        if (strncmp(tf[i].n, "BC", 2) == 0 && !bcOK) { tx[@(tf[i].n)] = @"skipped (no native BC)"; continue; }
        if (strncmp(tf[i].n, "Depth32Float_Stencil8", 21) == 0) { tx[@(tf[i].n)] = @"skipped"; continue; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)tf[i].fmt width:64 height:64 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        PBBoot([NSString stringWithFormat:@"metal: texture %s", tf[i].n].UTF8String);
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        tx[@(tf[i].n)] = t ? @"created" : @"FAILED";
    }
    m[@"texture_create_test"] = tx;

    PBBoot("metal: textures ok");
    /* MetalFX via runtime lookup so the app still launches where the framework or class is absent */
    NSMutableDictionary *fx = [NSMutableDictionary dictionary];
    void *h = dlopen("/System/Library/Frameworks/MetalFX.framework/MetalFX", RTLD_LAZY);
    fx[@"framework_loaded"] = B(h != NULL);
    const char *classes[] = { "MTLFXSpatialScalerDescriptor", "MTLFXTemporalScalerDescriptor",
                              "MTLFXTemporalDenoisedScalerDescriptor", "MTLFXFrameInterpolatorDescriptor" };
    for (size_t i = 0; i < sizeof classes / sizeof *classes; i++) {
        Class c = NSClassFromString(@(classes[i]));
        if (!c) { fx[@(classes[i])] = @"class not present"; continue; }
        if (![c respondsToSelector:@selector(supportsDevice:)]) { fx[@(classes[i])] = @"no supportsDevice:"; continue; }
        BOOL (*fn)(id, SEL, id) = (BOOL (*)(id, SEL, id))objc_msgSend;
        fx[@(classes[i])] = B(fn(c, @selector(supportsDevice:), dev));
    }
    m[@"metalfx"] = fx;
    return m;
}
