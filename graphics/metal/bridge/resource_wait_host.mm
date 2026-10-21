// SPDX-License-Identifier: MIT
// Keep real work pending behind an unsignaled shared event. Waiting for an unrelated
// resource must return; waiting for its storage, views or IOSurface alias must not.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>
#include "../NativeBridgeMetalFrontend.h"

@protocol ResourceWait
- (void)waitUntilComplete;
@end

static dispatch_time_t seconds(unsigned n) {return dispatch_time(DISPATCH_TIME_NOW, n * NSEC_PER_SEC);}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    @autoreleasepool {
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        if (!device) return 2;
        for (unsigned variant = 0; variant < 9; ++variant) {
            @autoreleasepool {
                id<MTLBuffer> output = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
                id<MTLBuffer> input = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
                id query = output;
                id<MTLTexture> target = nil;
                IOSurfaceRef surface = nullptr;
                void *pages = nullptr;
                auto td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
                td.storageMode = MTLStorageModeShared;
                td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
                if (variant == 0) query = input;    // never used by the pending fill
                if (variant == 2) {
                    id<MTLTexture> texture = [output newTextureWithDescriptor:td offset:0 bytesPerRow:256];
                    query = [texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
                }
                if (variant == 3 || variant == 7) {
                    if (variant == 7) td.storageMode = MTLStorageModePrivate;
                    target = [device newTextureWithDescriptor:td];
                    query = [[target newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm] newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
                }
                if (variant == 4) {
                    NSDictionary *p = @{(id)kIOSurfaceWidth:@16, (id)kIOSurfaceHeight:@16,
                        (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@((uint32_t)0x52474241)};
                    surface = IOSurfaceCreate((__bridge CFDictionaryRef)p);
                    target = [device newTextureWithDescriptor:td iosurface:surface plane:0];
                    query = [device newTextureWithDescriptor:td iosurface:surface plane:0];
                }
                if (variant == 5) {
                    auto hd = [MTLHeapDescriptor new];hd.size = 65536;hd.storageMode = MTLStorageModeShared;
                    id<MTLHeap> heap = [device newHeapWithDescriptor:hd];
                    output = [heap newBufferWithLength:4096 options:MTLResourceStorageModeShared];
                    query = [heap newBufferWithLength:4096 options:MTLResourceStorageModeShared];
                }
                if (variant == 6) {
                    const size_t page = (size_t)getpagesize();
                    pages = mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
                    if (pages == MAP_FAILED) return 2;
                    auto keep = std::shared_ptr<void>(pages, [page](void *p){munmap(p, page * 2);});
                    auto release = ^(void *, NSUInteger){(void)keep;};
                    output = [device newBufferWithBytesNoCopy:pages length:page * 2 options:MTLResourceStorageModeShared deallocator:release];
                    query = [device newBufferWithBytesNoCopy:(char *)pages + page length:page options:MTLResourceStorageModeShared deallocator:release];
                }
                if (variant == 8) {
                    output = [device newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
                    query = output;
                }
                if (!output || !input || !query || ((variant == 3 || variant == 4 || variant == 7) && !target)) {
                    std::printf("FAIL resource allocation %u\n", variant);return 2;
                }
                memset(input.contents, 0x5a, 4096);
                id<MTLSharedEvent> event = [device newSharedEvent];
                id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
                [commands encodeWaitForEvent:event value:1];
                id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
                if (target) [blit copyFromBuffer:input sourceOffset:0 sourceBytesPerRow:64 sourceBytesPerImage:1024 sourceSize:MTLSizeMake(16,16,1)
                                      toTexture:target destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
                else [blit fillBuffer:output range:NSMakeRange(0, output.length) value:0x5a];
                [blit endEncoding];[commands commit];
                auto entered = dispatch_semaphore_create(0), returned = dispatch_semaphore_create(0);
                dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
                    dispatch_semaphore_signal(entered);
                    [(id<ResourceWait>)query waitUntilComplete];
                    dispatch_semaphore_signal(returned);
                });
                if (dispatch_semaphore_wait(entered, seconds(3))) return 1;
                const bool early = dispatch_semaphore_wait(returned, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC)) == 0;
                event.signaledValue = 1;
                if (!early && dispatch_semaphore_wait(returned, seconds(3))) {std::puts("FAIL resource wait stuck");return 1;}
                [commands waitUntilCompleted];
                if (commands.status != MTLCommandBufferStatusCompleted || early != (variant == 0)) {
                    std::printf("FAIL variant=%u early=%d status=%lu\n", variant, early, (unsigned long)commands.status);return 1;
                }
                if (target) {
                    unsigned char pixels[1024] = {};
                    if (variant == 7) {
                        memset(input.contents, 0, 4096);
                        id<MTLCommandBuffer> readback = [[device newCommandQueue] commandBuffer];
                        id<MTLBlitCommandEncoder> copy = [readback blitCommandEncoder];
                        [copy copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(16,16,1)
                                   toBuffer:input destinationOffset:0 destinationBytesPerRow:64 destinationBytesPerImage:1024];
                        [copy endEncoding];[readback commit];[readback waitUntilCompleted];
                        if (readback.status != MTLCommandBufferStatusCompleted) return 1;
                        memcpy(pixels, input.contents, sizeof pixels);
                    } else [target getBytes:pixels bytesPerRow:64 fromRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0];
                    for (unsigned char pixel : pixels) if (pixel != 0x5a) return 1;
                } else if (variant == 8) {
                    memset(input.contents, 0, 4096);
                    id<MTLCommandBuffer> readback = [[device newCommandQueue] commandBuffer];
                    id<MTLBlitCommandEncoder> copy = [readback blitCommandEncoder];
                    [copy copyFromBuffer:output sourceOffset:0 toBuffer:input destinationOffset:0 size:4096];
                    [copy endEncoding];[readback commit];[readback waitUntilCompleted];
                    if (readback.status != MTLCommandBufferStatusCompleted) return 1;
                    for (NSUInteger i = 0; i < 4096; ++i) if (((const unsigned char *)input.contents)[i] != 0x5a) return 1;
                } else for (NSUInteger i = 0; i < output.length; ++i) if (((const unsigned char *)output.contents)[i] != 0x5a) return 1;
                // The shared deallocator owns no-copy pages until both imports release them.
                query = nil;output = nil;target = nil;
                if (surface) CFRelease(surface);
            }
        }
        std::puts("PASS: unrelated resource wait returns while work is gated; storage, views, heap and IOSurface/no-copy aliases wait for real completion");
    }
}
