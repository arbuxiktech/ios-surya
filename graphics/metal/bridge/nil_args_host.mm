// SPDX-License-Identifier: MIT
// Host check: a blit encoder call with a nil resource (what a refused allocation hands the caller) fails its command buffer instead of raising an
// exception. An exception escaping a worker thread of the host process aborts the process; on the tablet that killed SpringBoard (int-075/083).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include "../NativeBridgeMetalFrontend.h"

__attribute__((noinline)) static id nothing() {return nil;}   // not a literal: the compiler rejects nil for these nonnull parameters

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 3) return 2;
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        if (!device) {std::printf("FAIL device\n"); return 1;}
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
        id<MTLTexture> texture = [device newTextureWithDescriptor:d];
        int failed = 0;
        for (int variant = 0; variant < 4; ++variant) {
            @try {
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
                switch (variant) {
                    case 0: [blit copyFromBuffer:(id<MTLBuffer>)nothing() sourceOffset:0 toBuffer:buffer destinationOffset:0 size:16]; break;
                    case 1: [blit fillBuffer:(id<MTLBuffer>)nothing() range:NSMakeRange(0, 16) value:0]; break;
                    case 2: [blit copyFromTexture:(id<MTLTexture>)nothing() sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1)
                                         toBuffer:buffer destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256]; break;
                    default: [blit copyFromTexture:texture toTexture:(id<MTLTexture>)nothing()]; break;
                }
                [blit endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                if (cb.status != MTLCommandBufferStatusError) {std::printf("FAIL variant %d: status %ld, not an error\n", variant, (long)cb.status); ++failed;}
            } @catch (NSException *e) {
                std::printf("FAIL variant %d raised %s\n", variant, e.name.UTF8String); ++failed;
            }
        }
        if (failed) return 1;
        std::printf("PASS: a nil resource fails the command buffer in every blit variant, no exception\n");
    }
    return 0;
}
