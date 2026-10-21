// SPDX-License-Identifier: MIT
// Actual host GPU work waits on an unsignaled event while every caller-owned
// no-copy buffer/view reference is dropped. Its deallocator must not run yet.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>
#include "../NativeBridgeMetalFrontend.h"
#include "NativeMetalObjects.h"

struct State {std::atomic<unsigned> released{0};};

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    @autoreleasepool {
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        if (!device) return 2;
        unsigned failures = 0;
        size_t airBytes = 0;
        const void *air = nmb_fixture_air(&airBytes);
        id<MTLLibrary> library = native_bridge_metal_library_with_air(air, airBytes);
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"scale_and_offset"] error:nil];
        if (!pipeline) return 2;
        for (unsigned variant = 0; variant < 7; ++variant) {
            auto state = std::make_shared<State>();
            const size_t page = (size_t)getpagesize();
            void *pages = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (pages == MAP_FAILED) return 2;
            memset(pages, variant == 6 ? 0xcc : 0x5a, page);
            if (variant == 5) for (unsigned i = 0; i < 256; ++i) ((float *)pages)[i] = (float)i;
            id<MTLBuffer> output = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
            id<MTLSharedEvent> event = [device newSharedEvent];
            id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
            __weak id weakBuffer;
            if (!output || !event || !commands) return 2;
            @autoreleasepool {
                // Deliberately keep the mapping owned by the test, so the old
                // early-deallocator failure cannot make the pending GPU read
                // unmapped memory. No pixel or GPU completion is synthesized.
                id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:pages length:page options:MTLResourceStorageModeShared
                    deallocator:^(void *, NSUInteger){state->released.fetch_add(1);}];
                if (!buffer) return 2;
                weakBuffer = buffer;
                [commands encodeWaitForEvent:event value:1];
                if (variant == 5) {
                    id<MTLComputeCommandEncoder> compute = [commands computeCommandEncoder];
                    [compute setComputePipelineState:pipeline];
                    [compute setBuffer:buffer offset:0 atIndex:0];
                    [compute setBuffer:output offset:0 atIndex:1];
                    const float scale = 2;
                    [compute setBytes:&scale length:sizeof scale atIndex:2];
                    [compute dispatchThreads:MTLSizeMake(256,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
                    [compute endEncoding];
                } else if (variant == 1 || variant == 6) {
                    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
                    d.storageMode = MTLStorageModeShared;
                    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView | MTLTextureUsageRenderTarget;
                    id<MTLTexture> root = [buffer newTextureWithDescriptor:d offset:0 bytesPerRow:256];
                    id<MTLTexture> view = [[root newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm]
                                               newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
                    if (!view) return 2;
                    if (variant == 6) {
                        auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
                        pass.colorAttachments[0].texture = view;
                        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                        pass.colorAttachments[0].clearColor = MTLClearColorMake(90.0/255,90.0/255,90.0/255,90.0/255);
                        [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
                    }
                    if (variant == 1) {
                        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
                        [blit copyFromTexture:view sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(16,16,1)
                                 toBuffer:output destinationOffset:0 destinationBytesPerRow:64 destinationBytesPerImage:1024];
                        [blit endEncoding];
                    }
                } else {
                    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
                    if (variant == 2) [blit fillBuffer:buffer range:NSMakeRange(0, 1024) value:0x37];
                    [blit copyFromBuffer:buffer sourceOffset:0 toBuffer:output destinationOffset:0 size:1024];
                    id<MTLBuffer> missing = nil;
                    if (variant == 3) [blit fillBuffer:missing range:NSMakeRange(0, 16) value:0]; // local refusal, never submit
                    [blit endEncoding];
                }
            }
            const unsigned before = state->released.load();
            if (before || !weakBuffer) ++failures;
            unsigned pending = before;
            if (variant != 4) {
                [commands commit];
                // Completion must remain pending until the actual event is
                // signaled, except a locally refused command which submitted
                // no GPU work. Check with bounded dispatch waits.
                auto finished = dispatch_semaphore_create(0);
                dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
                    @autoreleasepool {[commands waitUntilCompleted];}
                    dispatch_semaphore_signal(finished);
                });
                const bool early = dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) == 0;
                pending = state->released.load();
                if (variant != 3 && (early || pending || !weakBuffer)) ++failures;
                event.signaledValue = 1;
                if (!early && dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC))) return 1;
                if (commands.status != (variant == 3 ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted)) ++failures;
                if (variant != 3) for (unsigned i = 0; i < (variant == 5 ? 256 : 1024); ++i) {
                    const bool bad = variant == 5 ? ((const float *)output.contents)[i] != 3.0f*i
                        : variant == 6 ? ((const unsigned char *)pages)[(i / 64) * 256 + i % 64] != 0x5a
                        : ((const unsigned char *)output.contents)[i] != (variant == 2 ? 0x37 : 0x5a);
                    if (bad) {++failures;break;}
                }
                if (state->released.load() != 1 || weakBuffer) ++failures; // even while commands remains retained
            }
            commands = nil;
            // The real completion callback can release its own reference just
            // after waitUntilCompleted returns. Bound that scheduling margin.
            for (unsigned i = 0; i < 300 && state->released.load() == 0; ++i) usleep(1000);
            if (state->released.load() != 1 || weakBuffer) ++failures;
            std::printf("case %u before=%u pending=%u released=%u\n", variant, before, pending, state->released.load());
            munmap(pages, page);
        }
        if (failures) {std::printf("FAIL: resource owner lifetime (%u checks)\n", failures);return 1;}
        std::puts("PASS: no-copy owners/views survive encoding and gated GPU work, release once after completion/refusal/discard; actual GPU bytes match");
    }
}
