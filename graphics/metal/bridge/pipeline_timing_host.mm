// SPDX-License-Identifier: MIT
// Host instrument: how long the target's own QuartzCore shaders take through the native frontend (metal2vulkan, then the Vulkan driver given as
// loader: the Adreno Turnip over the host simulation, or MoltenVK). Every <name>_vert_<p> / <name>_frag_<p> pair becomes a BGRA8 render pipeline
// and every kernel a compute pipeline, each timed; with METAL2VULKAN_LOG_PIPELINES=1 the runtime also prints the VkPipeline creation time of each,
// which separates the driver's compile from the translation. The host CPU is not the tablet's: compare the split, not the absolute numbers.
// argv: loader, shader cache dir, QuartzCore default.metallib, [limit], [draw]. With draw=1 each render state made is drawn once (three
// vertices into a 64x64 BGRA8 target, a zeroed buffer at every vertex and fragment index 0..7), which makes its VkPipeline: the runtime
// makes it at the draw, so without a draw the creation time is the translation alone.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include "NativeBridgeMetalFrontend.h"

extern "C" size_t native_bridge_metal_telemetry(char *, size_t);

static double nowMs() {return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;}
static void missing(const char *c, const char *s) {std::fprintf(stderr, "missing %s %s\n", c, s);}

int main(int argc, char **argv) {
    if (argc < 4) {std::fprintf(stderr, "usage: loader cache metallib [limit]\n"); return 2;}
    @autoreleasepool {
        const unsigned limit = argc > 4 ? (unsigned)std::atoi(argv[4]) : 0;
        const bool draw = argc > 5 && std::atoi(argv[5]) == 1;
        double t0 = nowMs();
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!device) {std::printf("open failed\n"); return 1;}
        std::printf("open %.1f ms\n", nowMs() - t0);
        // draw=2: the plugin's on-device benchmark (native_bridge_metal_benchmark) with the same pair count, for the host side of the comparison.
        if (argc > 5 && std::atoi(argv[5]) == 2) {
            static char rows[512];
            native_bridge_metal_benchmark(device, argv[3], limit ? limit : 8, rows, sizeof rows);
            std::printf("%s", rows);
            return 0;
        }
        NSError *error = nil;
        t0 = nowMs();
        id<MTLLibrary> qc = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        if (!qc) {std::printf("library failed: %s\n", error.localizedDescription.UTF8String); return 1;}
        NSArray<NSString *> *names = qc.functionNames;
        std::printf("library %.1f ms, %lu functions\n", nowMs() - t0, (unsigned long)names.count);
        std::vector<double> function, pipeline, drawn;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> target = [device newTextureWithDescriptor:td];
        id<MTLBuffer> zeros = [device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
        unsigned made = 0, refused = 0, n = 0;
        for (NSString *name in names) {
            if (limit && n >= limit) break;
            double f0 = nowMs();
            id<MTLFunction> f = [qc newFunctionWithName:name];
            double f1 = nowMs();
            function.push_back(f1 - f0);
            if (f.functionType == MTLFunctionTypeKernel) {
                ++n;
                double p0 = nowMs();
                id state = [device newComputePipelineStateWithFunction:f error:&error];
                double p1 = nowMs();
                pipeline.push_back(p1 - p0);
                std::printf("K %-60s fn=%7.1f pipe=%8.1f %s\n", name.UTF8String, f1 - f0, p1 - p0, state ? "ok" : error.localizedDescription.UTF8String);
                state ? ++made : ++refused;
            } else if (f.functionType == MTLFunctionTypeVertex) {
                NSString *fragmentName = [name stringByReplacingOccurrencesOfString:@"_vert_" withString:@"_frag_"];
                if ([fragmentName isEqualToString:name] || ![names containsObject:fragmentName]) continue;
                ++n;
                double g0 = nowMs();
                id<MTLFunction> fragment = [qc newFunctionWithName:fragmentName];
                double g1 = nowMs();
                MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
                d.vertexFunction = f;
                d.fragmentFunction = fragment;
                d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                double p0 = nowMs();
                id state = [device newRenderPipelineStateWithDescriptor:d error:&error];
                double p1 = nowMs();
                pipeline.push_back(p1 - p0);
                double took = 0;
                if (state && draw) {
                    double d0 = nowMs();
                    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                    pass.colorAttachments[0].texture = target;
                    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTLCommandBuffer> commands = [queue commandBuffer];
                    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
                    [encoder setRenderPipelineState:state];
                    for (NSUInteger i = 0; i < 8; ++i) {
                        [encoder setVertexBuffer:zeros offset:0 atIndex:i];
                        [encoder setFragmentBuffer:zeros offset:0 atIndex:i];
                    }
                    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [encoder endEncoding];
                    [commands commit];
                    [commands waitUntilCompleted];
                    took = nowMs() - d0;
                    drawn.push_back(took);
                }
                std::printf("R %-60s fn=%7.1f+%7.1f pipe=%8.1f draw=%8.1f %s\n", name.UTF8String, f1 - f0, g1 - g0, p1 - p0, took,
                            state ? "ok" : error.localizedDescription.UTF8String);
                state ? ++made : ++refused;
            }
            std::fflush(stdout);
        }
        std::sort(pipeline.begin(), pipeline.end());
        double total = 0;
        for (double v : pipeline) total += v;
        double ftotal = 0;
        for (double v : function) ftotal += v;
        if (!pipeline.empty())
            std::printf("SUMMARY pipelines=%zu made=%u refused=%u total=%.0f ms median=%.1f ms p90=%.1f ms max=%.1f ms | functions=%zu total=%.0f ms\n",
                        pipeline.size(), made, refused, total, pipeline[pipeline.size() / 2], pipeline[pipeline.size() * 9 / 10], pipeline.back(),
                        function.size(), ftotal);
        if (!drawn.empty()) {
            std::sort(drawn.begin(), drawn.end());
            double dt = 0;
            for (double v : drawn) dt += v;
            static char table[8192];
            native_bridge_metal_telemetry(table, sizeof table);
            for (char *line = std::strtok(table, "\n"); line; line = std::strtok(nullptr, "\n"))
                if (line[0] == 'V' || line[0] == 'W' || line[0] == 'S') std::printf("TELEMETRY %s\n", line);
            std::printf("DRAWS n=%zu total=%.0f ms median=%.1f ms max=%.1f ms\n", drawn.size(), dt, drawn[drawn.size() / 2], drawn.back());
        }
    }
    return 0;
}
