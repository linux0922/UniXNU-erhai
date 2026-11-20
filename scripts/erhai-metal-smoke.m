#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            fprintf(stderr, "METAL_SMOKE stage=device no_default_device\n");
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
