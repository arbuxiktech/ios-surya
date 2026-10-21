// SPDX-License-Identifier: MIT
// Host instrument: QuartzCore's own uber-shader (fixed_vert_lph_gen with fixed_frag_lph_cph / fixed_frag_lph_cpf) under recipes of its
// function constants, through the native frontend. Each recipe makes one render pipeline, draws it once over fixed pseudo-random inputs
// (every buffer and texture the stages may read, all values finite) into a 64x64 BGRA8 target, and prints the pipeline time, the draw time
// (the runtime makes the VkPipeline at the draw) and a hash of the pixels. Two runs of the same recipes over two builds of the translator,
// on the same loader, must print the same hashes: the instrument behind a change to what the translator emits for these modules. Over the
// Adreno Turnip host simulation the CP executes nothing, so there only the times mean anything.
// argv: loader (or apple), shader cache dir, QuartzCore AIR metallib, recipe file, [pixel dump dir]. A recipe line: fragment name, then name:type:value
// constants (bool, uchar, uchar4 as a,b,c,d); fixed_vert_lph_gen takes the ones it declares.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include "NativeBridgeMetalFrontend.h"

extern "C" size_t native_bridge_metal_telemetry(char *, size_t);

static double nowMs() {return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;}
static void missing(const char *c, const char *s) {std::fprintf(stderr, "missing %s %s\n", c, s);}
static uint32_t lcg(uint32_t &s) {s = s * 1664525u + 1013904223u; return s >> 8;}
// A half in [0, 1): sign 0, exponent 0..14, so any pair of them read as a float is finite too.
static uint16_t unitHalf(uint32_t &s) {return uint16_t((lcg(s) % 15) << 10 | (lcg(s) & 0x3ff));}
static uint64_t fnv(const uint8_t *p, size_t n) {uint64_t h = 1469598103934665603ull; while (n--) h = (h ^ *p++) * 1099511628211ull; return h;}

static MTLFunctionConstantValues *constants(const std::vector<std::string> &specs, MTLFunctionConstantValues *v) {
    for (const std::string &spec : specs) {
        size_t a = spec.find(':'), b = spec.find(':', a + 1);
        NSString *name = @(spec.substr(0, a).c_str());
        std::string type = spec.substr(a + 1, b - a - 1), value = spec.substr(b + 1);
        if (type == "bool") {bool x = value == "true" || value == "1"; [v setConstantValue:&x type:MTLDataTypeBool withName:name];}
        else if (type == "uchar") {uint8_t x = uint8_t(std::strtoul(value.c_str(), nullptr, 0)); [v setConstantValue:&x type:MTLDataTypeUChar withName:name];}
        else if (type == "uchar4") {
            uint8_t x[4] = {};
            const char *p = value.c_str();
            for (int i = 0; i < 4 && *p; ++i) {char *e; x[i] = uint8_t(std::strtoul(p, &e, 0)); p = *e ? e + 1 : e;}
            [v setConstantValue:x type:MTLDataTypeUChar4 withName:name];
        }
    }
    return v;
}

int main(int argc, char **argv) {
    if (argc < 5) {std::fprintf(stderr, "usage: loader cache metallib recipes [dump dir]\n"); return 2;}
    @autoreleasepool {
        // Loader "apple": the host's own Metal device compiles the same AIR, the reference the translator's results are held to.
        const bool apple = !std::strcmp(argv[1], "apple");
        id<MTLDevice> device = apple ? MTLCreateSystemDefaultDevice() : native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!device) {std::printf("open failed\n"); return 1;}
        NSError *error = nil;
        id<MTLLibrary> qc = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        if (!qc) {std::printf("library failed: %s\n", error.localizedDescription.UTF8String); return 1;}
        FILE *recipes = std::fopen(argv[4], "r");
        if (!recipes) {std::printf("no recipes\n"); return 1;}
        const char *dump = argc > 5 ? argv[5] : nullptr;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        constexpr unsigned W = 64, H = 64;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:td];
        // Inputs, the same in every run: a full-screen pair of triangles (SurfaceVertexXC: float4 position, two float2 texcoords, half4
        // color, padding; 48 bytes), the identity MVP and texture matrices, a zero vertex config (layout 0), and finite pseudo-random
        // words for every other buffer and texel.
        uint32_t seed = 0x5eed1234u;
        struct Vertex {float position[4], tc0[2], tc1[2]; uint16_t color[4]; uint8_t pad[8];};
        static_assert(sizeof(Vertex) == 48);
        const float corners[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {-1, 1}, {1, -1}, {1, 1}};
        Vertex vertices[6] = {};
        for (int i = 0; i < 6; ++i) {
            vertices[i].position[0] = corners[i][0]; vertices[i].position[1] = corners[i][1]; vertices[i].position[3] = 1;
            vertices[i].tc0[0] = vertices[i].tc1[1] = (corners[i][0] + 1) / 2;
            vertices[i].tc0[1] = vertices[i].tc1[0] = (1 - corners[i][1]) / 2;
            for (int c = 0; c < 4; ++c) vertices[i].color[c] = unitHalf(seed);
        }
        id<MTLBuffer> vertexBuffer = [device newBufferWithBytes:vertices length:sizeof vertices options:MTLResourceStorageModeShared];
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        id<MTLBuffer> matrix = [device newBufferWithBytes:identity length:sizeof identity options:MTLResourceStorageModeShared];
        id<MTLBuffer> zeros = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        std::vector<id<MTLBuffer>> words;
        for (int i = 0; i < 9; ++i) {
            std::vector<uint16_t> w(131072 / 2 + 512);
            for (uint16_t &x : w) x = unitHalf(seed);
            if (i == 1) {
                // CA::OGL::Metal::Uniform at its own layout: floats in [0, 1) at [6 x float4] (0..96), [2 x float4] and float4
                // (128..176); halves in [0, 1) elsewhere; its two flag bytes (120, 121) and the trailing [8 x i8] zero.
                auto *bytes = reinterpret_cast<uint8_t *>(w.data());
                auto unitFloat = [&](size_t at) {float f = float(lcg(seed) % 65536) / 65536.0f; std::memcpy(bytes + at, &f, 4);};
                for (size_t at = 0; at < 96; at += 4) unitFloat(at);
                for (size_t at = 128; at < 176; at += 4) unitFloat(at);
                bytes[120] = bytes[121] = 0;
                std::memset(bytes + 216, 0, 8);
            }
            words.push_back([device newBufferWithBytes:w.data() length:w.size() * 2 options:MTLResourceStorageModeShared]);
        }
        MTLTextureDescriptor *xd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:16 height:16 mipmapped:NO];
        xd.usage = MTLTextureUsageShaderRead;
        std::vector<id<MTLTexture>> textures;
        for (int i = 0; i < 12; ++i) {
            id<MTLTexture> t = [device newTextureWithDescriptor:xd];
            std::vector<uint16_t> texels(16 * 16 * 4);
            for (uint16_t &x : texels) x = unitHalf(seed);
            [t replaceRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0 withBytes:texels.data() bytesPerRow:16 * 8];
            textures.push_back(t);
        }
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
        char line[4096];
        unsigned n = 0, made = 0;
        double pipeTotal = 0, drawTotal = 0;
        while (std::fgets(line, sizeof line, recipes)) {
            std::vector<std::string> tokens;
            for (char *t = std::strtok(line, " \t\n"); t; t = std::strtok(nullptr, " \t\n")) tokens.emplace_back(t);
            if (tokens.empty() || tokens[0][0] == '#') continue;
            ++n;
            std::vector<std::string> specs(tokens.begin() + 1, tokens.end());
            double f0 = nowMs();
            id<MTLFunction> vertex = [qc newFunctionWithName:@"fixed_vert_lph_gen" constantValues:constants(specs, [MTLFunctionConstantValues new]) error:&error];
            id<MTLFunction> fragment = vertex ? [qc newFunctionWithName:@(tokens[0].c_str()) constantValues:constants(specs, [MTLFunctionConstantValues new]) error:&error] : nil;
            double f1 = nowMs();
            if (!fragment) {std::printf("R%03u function refused: %s\n", n, error.localizedDescription.UTF8String); continue;}
            MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = vertex;
            d.fragmentFunction = fragment;
            d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            double p0 = nowMs();
            id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&error];
            double p1 = nowMs();
            if (!state) {std::printf("R%03u pipeline refused: %s\n", n, error.localizedDescription.UTF8String); continue;}
            ++made;
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            double d0 = nowMs();
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:state];
            [encoder setVertexBuffer:zeros offset:0 atIndex:0];
            [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:1];
            [encoder setVertexBuffer:matrix offset:0 atIndex:2];
            [encoder setVertexBuffer:matrix offset:0 atIndex:3];
            [encoder setVertexBuffer:matrix offset:0 atIndex:4];
            for (NSUInteger i = 0; i < 9; ++i) [encoder setFragmentBuffer:words[i] offset:0 atIndex:i];
            for (NSUInteger i = 0; i < 12; ++i) [encoder setFragmentTexture:textures[i] atIndex:i];
            [encoder setFragmentSamplerState:sampler atIndex:0];
            [encoder setFragmentSamplerState:sampler atIndex:1];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            [encoder endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            double d1 = nowMs();
            std::vector<uint8_t> pixels(W * H * 4);
            [target getBytes:pixels.data() bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
            if (dump) {
                char path[1024];
                std::snprintf(path, sizeof path, "%s/R%03u.bgra", dump, n);
                if (FILE *out = std::fopen(path, "wb")) {std::fwrite(pixels.data(), 1, pixels.size(), out); std::fclose(out);}
            }
            pipeTotal += p1 - p0;
            drawTotal += d1 - d0;
            std::printf("R%03u %s fn=%.1f pipe=%.1f draw=%.1f status=%ld hash=%016llx\n", n, tokens[0].c_str(), f1 - f0, p1 - p0, d1 - d0,
                        (long)commands.status, (unsigned long long)fnv(pixels.data(), pixels.size()));
            std::fflush(stdout);
        }
        static char table[8192];
        if (!apple) native_bridge_metal_telemetry(table, sizeof table);
        for (char *l = std::strtok(table, "\n"); l; l = std::strtok(nullptr, "\n"))
            if (l[0] == 'V' || l[0] == 'T') std::printf("TELEMETRY %s\n", l);
        std::printf("SUMMARY recipes=%u made=%u pipe=%.0f ms draw=%.0f ms\n", n, made, pipeTotal, drawTotal);
    }
    return 0;
}
