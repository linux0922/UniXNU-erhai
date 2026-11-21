#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

@protocol ErhaiMetalPlugin <NSObject>
- (instancetype)initWithAcceleratorPort:(unsigned)port;
@end

static const char ramLogPath[] = "/private/var/tmp/erhai-s9-metal-smoke-private/stderr.log";

__attribute__((constructor))
static void recordRamStartup(void)
{
    if (getpid() != 1) return;
    int fd = open(ramLogPath, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    static const char message[] = "METAL_SMOKE stage=ram_constructor\n";
    (void)write(fd, message, sizeof(message) - 1);
    close(fd);
}

static id<MTLDevice> loadOwnedPlugin(NSString *directory, unsigned serviceAttempts)
{
    NSString *marker = [directory stringByAppendingPathComponent:@"NativeAdrenoMetal.publish"];
    if (![[NSFileManager defaultManager] isReadableFileAtPath:marker]) {
        fprintf(stderr, "METAL_SMOKE stage=plugin missing_private_marker\n");
        return nil;
    }
    NSBundle *bundle = [NSBundle bundleWithPath:
        [directory stringByAppendingPathComponent:@"NativeAdrenoMetal.bundle"]];
    NSError *error = nil;
    if (![bundle loadAndReturnError:&error]) {
        fprintf(stderr, "METAL_SMOKE stage=plugin load_failed=%s\n",
                error.localizedDescription.UTF8String ?: "bundle not found");
        return nil;
    }
    Class factory = bundle.principalClass;
    if (!class_getInstanceMethod(factory, @selector(initWithAcceleratorPort:))) {
        fprintf(stderr, "METAL_SMOKE stage=plugin invalid_factory\n");
        return nil;
    }

    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    if (!iokit) {
        fprintf(stderr, "METAL_SMOKE stage=service missing_iokit\n");
        return nil;
    }
    CFMutableDictionaryRef (*matching)(const char *) = dlsym(iokit, "IOServiceMatching");
    unsigned (*getService)(unsigned, CFDictionaryRef) = dlsym(iokit, "IOServiceGetMatchingService");
    int (*releaseService)(unsigned) = dlsym(iokit, "IOObjectRelease");
    unsigned port = 0;
    if (matching && getService && releaseService) {
        for (unsigned attempt = 0; attempt < serviceAttempts; ++attempt) {
            CFMutableDictionaryRef match = matching("NativeAdrenoGpuPublished");
            port = match ? getService(0, match) : 0;
            if (port) break;
            if (attempt + 1 < serviceAttempts) sleep(1);
        }
    }
    if (!port) {
        fprintf(stderr, "METAL_SMOKE stage=service unavailable\n");
        dlclose(iokit);
        return nil;
    }
    id<ErhaiMetalPlugin> instance = [factory alloc];
    id<MTLDevice> device = (id<MTLDevice>)[instance initWithAcceleratorPort:port];
    releaseService(port);
    dlclose(iokit);
    return device;
}

static int runProbe(int argc, const char *argv[], BOOL ramInit)
{
    @autoreleasepool {
        id<MTLDevice> device;
        if (ramInit) {
            device = loadOwnedPlugin(@"/private/var/tmp/erhai-s9-metal-smoke-private", 90);
        } else if (argc == 1) {
            device = MTLCreateSystemDefaultDevice();
        } else if (argc == 3 && strcmp(argv[1], "--plugin-dir") == 0) {
            NSString *directory = [NSString stringWithUTF8String:argv[2]];
            if (![directory isAbsolutePath]) {
                fprintf(stderr, "METAL_SMOKE stage=arguments absolute_plugin_path_required\n");
                return 6;
            }
            device = loadOwnedPlugin(directory, 1);
        } else {
            fprintf(stderr, "Usage: erhai-metal-smoke [--plugin-dir /path/to/private/stage]\n");
            return 6;
        }
        if (!device) {
            fprintf(stderr, "METAL_SMOKE stage=device unavailable\n");
            return 1;
        }
        fprintf(stderr, "METAL_SMOKE stage=device name=%s class=%s\n",
                device.name.UTF8String, object_getClassName(device));

        const NSUInteger bytes = 4096;
        const uint8_t expected = 0x6d;
        id<MTLBuffer> buffer = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
        if (!buffer.contents || !queue || !command || !encoder) {
            fprintf(stderr, "METAL_SMOKE stage=allocate failed\n");
            return 2;
        }

        memset(buffer.contents, 0, bytes);
        command.label = @"Erhai one-shot Metal fill";
        [encoder fillBuffer:buffer range:NSMakeRange(0, bytes) value:expected];
        [encoder endEncoding];
        dispatch_semaphore_t finished = dispatch_semaphore_create(0);
        [command addCompletedHandler:^(__unused id<MTLCommandBuffer> result) {
            dispatch_semaphore_signal(finished);
        }];
        [command commit];
        if (dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) {
            fprintf(stderr, "METAL_SMOKE stage=completion timeout\n");
            return 3;
        }
        if (command.status != MTLCommandBufferStatusCompleted) {
            fprintf(stderr, "METAL_SMOKE stage=completion status=%ld error=%s\n",
                    (long)command.status, command.error.localizedDescription.UTF8String ?: "unknown");
            return 4;
        }

        const uint8_t *data = buffer.contents;
        for (NSUInteger index = 0; index < bytes; ++index) {
            if (data[index] != expected) {
                fprintf(stderr, "METAL_SMOKE stage=readback mismatch_at=%lu\n", (unsigned long)index);
                return 5;
            }
        }
        fprintf(stderr, "METAL_SMOKE stage=readback verified_bytes=%lu\n", (unsigned long)bytes);
        return 0;
    }
}

int main(int argc, const char *argv[])
{
    const BOOL ramInit = getpid() == 1;
    if (!ramInit) return runProbe(argc, argv, NO);

    // Keep PID 1 alive after the one-shot run in this dedicated diagnostic boot.
    int fd = open(ramLogPath, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        const int attached = dup2(fd, STDERR_FILENO);
        if (fd != STDERR_FILENO) close(fd);
        if (attached == STDERR_FILENO) {
            setvbuf(stderr, NULL, _IONBF, 0);
            fprintf(stderr, "METAL_SMOKE stage=ram_init waiting_for_s9\n");
            int result = runProbe(argc, argv, YES);
            fprintf(stderr, "METAL_SMOKE stage=ram_init result=%d\n", result);
        }
    }
    for (;;) pause();
}
