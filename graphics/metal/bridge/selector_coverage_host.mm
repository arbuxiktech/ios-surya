// SPDX-License-Identifier: MIT
// Host instrument: which of the Metal methods the target's QuartzCore references (scripts/audit-quartzcore-metal-calls.py, JSON path as the third
// argument) the native frontend's live objects respond to. One object of each kind is made on the frontend's device (bridge over MoltenVK); a
// kind that cannot be made is reported as such. Prints every selector an object lacks. A reference proves QuartzCore can send it; it does not
// show that the system UI does.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstring>
#include "../NativeBridgeMetalFrontend.h"
#include "FrontendChecks.h"
#include "../bridge/NativeMetalObjects.h"

static id N(id object) {return object ?: (id)[NSNull null];}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 4) return 2;
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        if (!device) {std::printf("FAIL device\n"); return 1;}
        NSData *json = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[3]]];
        NSDictionary *all = [NSJSONSerialization JSONObjectWithData:json options:0 error:nil][@"kinds"];
        size_t bytes = 0;
        const void *air = nmb_fixture_air(&bytes);
        id<MTLLibrary> library = native_bridge_metal_library_with_air(air, bytes);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:8 height:8 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [device newTextureWithDescriptor:td];
        id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> render = [cb renderCommandEncoderWithDescriptor:pass];
        [render endEncoding];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit endEncoding];
        id<MTLComputeCommandEncoder> compute = [cb computeCommandEncoder];
        [compute endEncoding];
        id<MTLFunction> function = [library newFunctionWithName:@"scale_and_offset"];
        id<MTLComputePipelineState> computePipeline = [device newComputePipelineStateWithFunction:function error:nil];
        id<MTLRenderPipelineState> renderPipeline = frontendchecks::pipeline(device, library, @"colored_fragment", MTLPixelFormatRGBA8Unorm);
        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        id<MTLDepthStencilState> depth = [device newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
        id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:nil];
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.size = 65536;
        id heap = [device respondsToSelector:@selector(newHeapWithDescriptor:)] ? [device newHeapWithDescriptor:hd] : nil;
        NSDictionary *objects = @{@"device": N(device), @"texture": N(texture), @"buffer": N(buffer), @"heap": N(heap), @"command queue": N(queue),
                                  @"command buffer": N(cb), @"render encoder": N(render), @"blit encoder": N(blit), @"compute encoder": N(compute),
                                  @"render pipeline": N(renderPipeline), @"compute pipeline": N(computePipeline), @"sampler": N(sampler),
                                  @"depth stencil": N(depth), @"library": N(library), @"function": N(function), @"binary archive": N(archive)};
        unsigned total = 0;
        for (NSString *kind in [all.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
            id object = objects[kind];
            NSArray *selectors = [all[kind][@"selectors"] allKeys];
            if (!object || object == [NSNull null]) {std::printf("%-18s cannot be created (%lu selectors referenced)\n", kind.UTF8String, (unsigned long)selectors.count); ++total; continue;}
            NSMutableArray *missing = [NSMutableArray array];
            for (id entry in selectors) {
                NSString *name = entry;
                if ([name hasPrefix:@"."]) continue;
                if (![object respondsToSelector:NSSelectorFromString(name)]) [missing addObject:name];
            }
            total += missing.count;
            std::printf("%-18s %lu referenced, %lu missing%s%s\n", kind.UTF8String, (unsigned long)selectors.count, (unsigned long)missing.count,
                        missing.count ? ": " : "", [missing componentsJoinedByString:@" "].UTF8String);
        }
        std::printf("%s: %u selectors the target's QuartzCore references are not answered by the frontend's objects\n", total ? "GAPS" : "PASS", total);
    }
    return 0;
}
