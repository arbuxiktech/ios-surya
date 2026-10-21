// SPDX-License-Identifier: MIT
// Interleave encoding on two queues, then reverse commit order. Global deltas assign
// the first buffer's render work to the second buffer's blits; per-buffer counts must not.
#include "FrontendChecks.h"
#include "NativeMetalObjects.h"
#include "../NativeBridgeMetalFrontend.h"
#include <map>
#include <string>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    @autoreleasepool {
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        if (!device) return 2;
        size_t length = 0;
        const void *air = nmb_fixture_air(&length);
        id<MTLLibrary> library = native_bridge_metal_library_with_air(air, length);
        frontendchecks::Report report = {};
        frontendchecks::gReport = &report;
        id<MTLRenderPipelineState> pipeline = frontendchecks::pipeline(device, library, @"colored_fragment", MTLPixelFormatRGBA8Unorm);
        auto td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [device newTextureWithDescriptor:td];
        id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> render = [[device newCommandQueue] commandBuffer];
        id<MTLCommandBuffer> copy = [[device newCommandQueue] commandBuffer];
        if (!library || !pipeline || !texture || !buffer || !render || !copy) return 2;
        frontendchecks::drawQuad(render, pipeline, texture, nil, frontendchecks::kQuarter, 6);
        for (unsigned i = 0; i < 3; ++i) {
            id<MTLBlitCommandEncoder> blit = [copy blitCommandEncoder];
            [blit fillBuffer:buffer range:NSMakeRange(0, 256) value:0x5a];
            [blit endEncoding];
        }
        frontendchecks::drawQuad(render, pipeline, texture, nil, frontendchecks::kQuarter, 6);
        [copy commit];[render commit];
        if (!frontendchecks::completed(copy, "blit") || !frontendchecks::completed(render, "render")) return 1;
        char text[4608] = {};
        native_bridge_metal_telemetry(text, sizeof text);
        std::map<unsigned, std::pair<unsigned, unsigned>> counts;
        for (char *line = strtok(text, "\n"); line; line = strtok(nullptr, "\n")) {
            unsigned index, sec, ms, passes, draws, kpx, dispatches, blits;
            if (sscanf(line, "K%u t=%u.%us rp%u dr%u kpx%u cs%u bl%u", &index, &sec, &ms, &passes, &draws, &kpx, &dispatches, &blits) != 8) continue;
            std::printf("%s\n", line);
            counts[index] = {passes, blits};
            if (draws != (index == 2 ? 2u : 0u)) {std::puts("FAIL cross-buffer draw attribution");return 1;}
        }
        if (counts.size() != 2 || counts[1] != std::make_pair(0u, 3u) || counts[2] != std::make_pair(2u, 0u)) {
            std::puts("FAIL interleaved render/blit attribution");return 1;
        }
        uint8_t pixels[16 * 16 * 4] = {};
        [texture getBytes:pixels bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
        if (frontendchecks::wrongTexels(pixels, 64, 16)) return 1;
        for (unsigned i = 0; i < 256; ++i) if (((const uint8_t *)buffer.contents)[i] != 0x5a) return 1;
        std::puts("PASS: opposite encode/commit orders retain per-buffer render/blit counts and correct host GPU pixels");
    }
}
