// SPDX-License-Identifier: MIT
// Two processes, distinct initially empty caches: archive selection cannot be satisfied by
// the producer's live pipeline memo. A strict empty/changed-descriptor miss must refuse.
#include "FrontendChecks.h"
#include "NativeMetalObjects.h"
#include "../NativeBridgeMetalFrontend.h"

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    @autoreleasepool {
        const bool produce = !strcmp(argv[4], "produce");
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        if (!device) return 2;
        size_t bytes = 0;
        const void *air = nmb_fixture_air(&bytes);
        id<MTLLibrary> library = native_bridge_metal_library_with_air(air, bytes);
        if (!library) return 2;
        auto descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"colored_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"colored_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        auto archiveDescriptor = [MTLBinaryArchiveDescriptor new];
        NSURL *url = [NSURL fileURLWithPath:@(argv[3])];
        if (!produce) archiveDescriptor.url = url;
        NSError *error = nil;
        id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:archiveDescriptor error:&error];
        if (!archive) {std::printf("FAIL open archive: %s\n", error.localizedDescription.UTF8String);return 1;}
        descriptor.binaryArchives = @[archive];
        if (produce) {
            id state = [device newRenderPipelineStateWithDescriptor:descriptor options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error];
            if (state || !error) {std::puts("FAIL empty archive miss was silently compiled");return 1;}
            if (![archive addRenderPipelineFunctionsWithDescriptor:descriptor error:&error] || ![archive serializeToURL:url error:&error]) {
                std::printf("FAIL archive add/save: %s\n", error.localizedDescription.UTF8String);return 1;
            }
        }
        error = nil;
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:descriptor options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error];
        if (!state || error) {std::printf("FAIL strict archive hit: %s\n", error.localizedDescription.UTF8String);return 1;}
        // A format not added to the archive must still miss, even after a valid pipeline is live.
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        if ([device newRenderPipelineStateWithDescriptor:descriptor options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error] || !error) {
            std::puts("FAIL changed-descriptor archive miss");return 1;
        }
        auto td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> target = [device newTextureWithDescriptor:td];
        id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
        if (!target || !cb) return 2;
        frontendchecks::Report report = {};
        frontendchecks::gReport = &report;
        frontendchecks::drawQuad(cb, state, target, nil, frontendchecks::kQuarter, 6);
        if (!frontendchecks::finish(cb, "archived draw")) return 1;
        uint8_t pixels[16 * 16 * 4] = {};
        [target getBytes:pixels bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
        if (frontendchecks::wrongTexels(pixels, 64, 16)) {std::puts("FAIL archived render pixels");return 1;}
        std::printf("PASS: %s render archive, strict hit/miss and GPU pixel check (host only)\n", produce ? "produced" : "reloaded");
    }
}
