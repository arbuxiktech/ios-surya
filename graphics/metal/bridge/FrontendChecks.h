// SPDX-License-Identifier: MIT
// The native Metal frontend's qualification through the public Metal API only, shared by the host test (frontend_host.mm, over MoltenVK) and
// the plugin (NativeMetalBridgePhase.h, over Turnip on the Adreno 740). Every check reads back what the GPU wrote. Steps (markers 0x6601..):
// 1 compute; 2 render to a private texture with a blit readback; 3 render into a real IOSurface read by the CPU, 4 sampling it into a
// second surface (the composition shape); 5 a compute kernel reading and writing textures; 6 completion handler order; 7 an encoder refusal
// failing its command buffer; 8 the target's QuartzCore library (when a path is given); 9 function constants (when that fixture is given);
// 10 a shared event ordering two queues (GPU wait, GPU signal, CPU signal, listener notification) and a fence between two encoders;
// 11 heaps (automatic and placement), a visibility result buffer and setDepthBias.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace frontendchecks {
struct Report {
    unsigned failed, last;
    char first[96];
    unsigned qcFunctions, qcKernels, qcKernelsOk, qcPairs, qcPairsOk;
    void (*mark)(uint32_t);
    bool hostSimulation;  // the host simulation's IOKit shim hides IOSurface (steps 3-4 skipped) and Metal's listener (none is made)
};
static Report *gReport;
__attribute__((format(printf, 2, 3))) static void fail(Report *r, const char *format, ...) {
    char text[160];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    std::printf("FAIL %s\n", text);
    if (!r->failed++) snprintf(r->first, sizeof r->first, "%x %s", r->last, text);
}
static void step(Report *r, uint32_t marker) {
    r->last = marker;
    if (r->mark) r->mark(marker);
}
#define CHECK(c, ...) do { if (!(c)) fail(gReport, __VA_ARGS__); } while (0)
static bool completed(id<MTLCommandBuffer> cb, const char *what) {
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        fail(gReport, "%s: status %lu %s", what, (unsigned long)cb.status, cb.error.localizedDescription.UTF8String ?: "");
        return false;
    }
    return true;
}
static bool finish(id<MTLCommandBuffer> cb, const char *what) {
    [cb commit];
    return completed(cb, what);
}
// The fixture's picture: top-left quarter (255, 51, 0, 255), the rest cleared blue; `bgra` swaps red and blue in memory.
static unsigned wrongTexels(const uint8_t *bytes, size_t row, unsigned side, bool bgra = false) {
    unsigned bad = 0;
    for (unsigned y = 0; y < side; ++y)
        for (unsigned x = 0; x < side; ++x) {
            const bool inside = x < side / 2 && y < side / 2;
            uint8_t e[4] = {inside ? (uint8_t)255 : (uint8_t)0, inside ? (uint8_t)51 : (uint8_t)0, inside ? (uint8_t)0 : (uint8_t)255, 255};
            if (bgra) std::swap(e[0], e[2]);
            if (memcmp(bytes + y * row + x * 4, e, 4)) ++bad;
        }
    return bad;
}
static IOSurfaceRef surface(unsigned side, OSType format) {
    NSDictionary *p = @{(id)kIOSurfaceWidth: @(side), (id)kIOSurfaceHeight: @(side), (id)kIOSurfaceBytesPerElement: @4,
                        (id)kIOSurfacePixelFormat: @(format)};
    return IOSurfaceCreate((__bridge CFDictionaryRef)p);
}
static id<MTLRenderPipelineState> pipeline(id<MTLDevice> device, id<MTLLibrary> library, NSString *fragment, MTLPixelFormat format) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [library newFunctionWithName:@"colored_vertex"];
    d.fragmentFunction = [library newFunctionWithName:fragment];
    d.colorAttachments[0].pixelFormat = format;
    NSError *error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&error];
    CHECK(state, "render pipeline %s: %s", fragment.UTF8String, error.localizedDescription.UTF8String);
    return state;
}
// Draws the fixture quad (vertex positions/colors at buffers 0/1) into `target`, cleared blue first.
static void drawQuad(id<MTLCommandBuffer> cb, id<MTLRenderPipelineState> state, id<MTLTexture> target, id<MTLTexture> sampled,
                     const float (*positions)[2], unsigned count) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:state];
    [e setVertexBytes:positions length:sizeof(float) * 2 * count atIndex:0];
    std::vector<float> colors;
    for (unsigned i = 0; i < count; ++i) colors.insert(colors.end(), {1.0f, 0.2f, 0.0f, 1.0f});
    [e setVertexBytes:colors.data() length:colors.size() * sizeof(float) atIndex:1];
    if (sampled) [e setFragmentTexture:sampled atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:count];
    [e endEncoding];
}
static const float kQuarter[6][2] = {{-1, 1}, {0, 1}, {-1, 0}, {0, 1}, {0, 0}, {-1, 0}};
static const float kFull[6][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, 1}, {1, -1}, {-1, -1}};

// Runs every step on `device` (the frontend's); `library` is the bridge's runtime_examples fixture, `fcLibrary` (may be nil) the
// function-constant fixture, `qcPath` (may be nil) the target's QuartzCore default.metallib.
static void run(id<MTLDevice> device, id<MTLLibrary> library, id<MTLLibrary> fcLibrary, NSString *qcPath, Report *r) {
    gReport = r;
    @autoreleasepool {
        CHECK([library.functionNames containsObject:@"sampled_fragment"], "function names");
        CHECK(![library newFunctionWithName:@"no_such_function"], "a missing function must be nil");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        CHECK(queue, "command queue");
        if (!queue) return;

        step(r, 0x6601);
        // 1. compute: out[i] = 3 * in[i] (the fixture's scale_and_offset with scale 2)
        {
            NSError *error = nil;
            id<MTLFunction> f = [library newFunctionWithName:@"scale_and_offset"];
            CHECK(f.functionType == MTLFunctionTypeKernel, "kernel type %lu", (unsigned long)f.functionType);
            id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:f error:&error];
            CHECK(state, "compute pipeline: %s", error.localizedDescription.UTF8String);
            std::vector<float> in(1000);
            for (unsigned i = 0; i < in.size(); ++i) in[i] = (float)i;
            id<MTLBuffer> input = [device newBufferWithBytes:in.data() length:in.size() * 4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> output = [device newBufferWithLength:in.size() * 4 options:MTLResourceStorageModeShared];
            const float scale = 2;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:state];
            [e setBuffer:input offset:0 atIndex:0];
            [e setBuffer:output offset:0 atIndex:1];
            [e setBytes:&scale length:4 atIndex:2];
            [e dispatchThreads:MTLSizeMake(in.size(), 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [e endEncoding];
            if (finish(cb, "compute")) {
                const float *v = (const float *)output.contents;
                unsigned bad = 0;
                for (unsigned i = 0; i < in.size(); ++i) bad += v[i] != 3.0f * i;
                CHECK(!bad, "compute: %u wrong outputs", bad);
            }
        }
        id<MTLRenderPipelineState> colored = pipeline(device, library, @"colored_fragment", MTLPixelFormatRGBA8Unorm);
        step(r, 0x6602);
        // 2. render into a private texture, blit to a buffer
        {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget;
            d.storageMode = MTLStorageModePrivate;
            id<MTLTexture> target = [device newTextureWithDescriptor:d];
            CHECK(target && target.width == 8 && target.pixelFormat == MTLPixelFormatRGBA8Unorm, "private texture");
            id<MTLBuffer> readback = [device newBufferWithLength:8 * 8 * 4 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            drawQuad(cb, colored, target, nil, kQuarter, 6);
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1)
                      toBuffer:readback destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
            [b endEncoding];
            if (finish(cb, "render")) CHECK(!wrongTexels((const uint8_t *)readback.contents, 32, 8), "render: wrong texels");
        }
        step(r, 0x6603);
        // 3. render into an IOSurface (its pages imported, no copy), then 4. sample it into a second surface
        IOSurfaceRef layer = r->hostSimulation ? nullptr : surface(64, 'RGBA'), screen = r->hostSimulation ? nullptr : surface(64, 'RGBA');
        CHECK(r->hostSimulation || (layer && screen), "IOSurfaceCreate");
        if (layer && screen) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModeShared;
            id<MTLTexture> layerTexture = [device newTextureWithDescriptor:d iosurface:layer plane:0];
            id<MTLTexture> screenTexture = [device newTextureWithDescriptor:d iosurface:screen plane:0];
            CHECK(layerTexture && screenTexture && layerTexture.iosurface == layer, "IOSurface textures");
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            drawQuad(cb, colored, layerTexture, nil, kQuarter, 6);
            if (finish(cb, "surface render")) {
                IOSurfaceLock(layer, kIOSurfaceLockReadOnly, nullptr);
                CHECK(!wrongTexels((const uint8_t *)IOSurfaceGetBaseAddress(layer), IOSurfaceGetBytesPerRow(layer), 64), "surface render: wrong texels");
                IOSurfaceUnlock(layer, kIOSurfaceLockReadOnly, nullptr);
            }
            step(r, 0x6604);
            // sampled_fragment samples its texture at position * 1/8 (normalized), so an 8x8 viewport of the screen reads the layer whole
            id<MTLRenderPipelineState> sampled = pipeline(device, library, @"sampled_fragment", MTLPixelFormatRGBA8Unorm);
            cb = [queue commandBuffer];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = screenTexture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
            [e setRenderPipelineState:sampled];
            [e setViewport:(MTLViewport){0, 0, 8, 8, 0, 1}];
            [e setVertexBytes:kFull length:sizeof kFull atIndex:0];
            float colors[6][4] = {};
            [e setVertexBytes:colors length:sizeof colors atIndex:1];
            [e setFragmentTexture:layerTexture atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            [e endEncoding];
            if (finish(cb, "composition")) {
                IOSurfaceLock(screen, kIOSurfaceLockReadOnly, nullptr);
                const uint8_t *p = (const uint8_t *)IOSurfaceGetBaseAddress(screen);
                std::vector<uint8_t> corner(8 * 8 * 4);
                for (unsigned y = 0; y < 8; ++y) memcpy(&corner[y * 32], p + y * IOSurfaceGetBytesPerRow(screen), 32);
                const unsigned bad = wrongTexels(corner.data(), 32, 8);
                const uint8_t *outside = p + 20 * IOSurfaceGetBytesPerRow(screen) + 20 * 4;
                CHECK(!bad, "composition: %u wrong texels in the sampled corner", bad);
                CHECK(outside[0] == 0 && outside[1] == 255 && outside[2] == 0, "composition: the rest keeps its clear color");
                IOSurfaceUnlock(screen, kIOSurfaceLockReadOnly, nullptr);
            }
        }
        step(r, 0x6605);
        // 5. compute over textures: invert_texels writes 1 - rgb of its input texel
        {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            id<MTLTexture> src = [device newTextureWithDescriptor:d], dst = [device newTextureWithDescriptor:d];
            std::vector<uint8_t> texels(16 * 16 * 4);
            for (size_t i = 0; i < texels.size(); ++i) texels[i] = (uint8_t)(i * 7);
            [src replaceRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0 withBytes:texels.data() bytesPerRow:64];
            NSError *error = nil;
            id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"invert_texels"] error:&error];
            CHECK(state, "invert pipeline: %s", error.localizedDescription.UTF8String);
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:state];
            [e setTexture:src atIndex:0];
            [e setTexture:dst atIndex:1];
            [e dispatchThreads:MTLSizeMake(16, 16, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [e endEncoding];
            if (finish(cb, "texture compute")) {
                std::vector<uint8_t> out(texels.size());
                [dst getBytes:out.data() bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
                unsigned bad = 0;
                for (size_t i = 0; i < out.size(); ++i) {
                    const int expected = i % 4 == 3 ? texels[i] : 255 - texels[i];
                    bad += abs(out[i] - expected) > 1;
                }
                CHECK(!bad, "texture compute: %u wrong channels", bad);
            }
        }
        step(r, 0x6606);
        // 6. handlers: scheduled before completed, each once, after the GPU finished
        {
            __block int order = 0, scheduled = -1, done = -1;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            id<MTLBuffer> buffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
            [b fillBuffer:buffer range:NSMakeRange(0, 4096) value:0xa5];
            [b endEncoding];
            [cb addScheduledHandler:^(id<MTLCommandBuffer>) {scheduled = order++;}];
            __block MTLCommandBufferStatus seen = MTLCommandBufferStatusNotEnqueued;
            [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {seen = c.status; done = order++;}];
            if (finish(cb, "handlers")) {
                CHECK(scheduled == 0 && done == 1 && order == 2, "handler order %d %d", scheduled, done);
                CHECK(seen == MTLCommandBufferStatusCompleted, "status inside the completed handler");
                const uint8_t *p = (const uint8_t *)buffer.contents;
                CHECK(p[0] == 0xa5 && p[4095] == 0xa5, "fill");
            }
        }
        step(r, 0x6607);
        // 7. a refused encoder call fails its command buffer (no silent drop), and the handlers still run
        {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> e = nil;
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget;
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = [device newTextureWithDescriptor:d];
            e = [cb renderCommandEncoderWithDescriptor:pass];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];  // no pipeline set
            [e endEncoding];
            __block bool ran = false;
            [cb addCompletedHandler:^(id<MTLCommandBuffer>) {ran = true;}];
            [cb commit];
            [cb waitUntilCompleted];
            CHECK(cb.status == MTLCommandBufferStatusError && cb.error, "a draw without a pipeline must fail the command buffer");
            CHECK(ran, "the completed handler runs for a failed command buffer");
        }
        step(r, 0x6609);
        // 9. function constants (newFunctionWithName:constantValues:error:): the runtime's fixture kernel_function_constant_gate_fold, whose
        // out[24] is in * 1000 * mode for mode 1..3 and in otherwise.
        if (fcLibrary) {
            for (int mode : {0, 2, 3}) {
                MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
                [values setConstantValue:&mode type:MTLDataTypeInt atIndex:0];
                NSError *error = nil;
                id<MTLFunction> f = [fcLibrary newFunctionWithName:@"fcfold" constantValues:values error:&error];
                CHECK(f, "specialized function: %s", error.localizedDescription.UTF8String);
                id<MTLComputePipelineState> state = f ? [device newComputePipelineStateWithFunction:f error:&error] : nil;
                CHECK(state, "specialized pipeline: %s", error.localizedDescription.UTF8String);
                if (!state) continue;
                const float in = 2;
                id<MTLBuffer> input = [device newBufferWithBytes:&in length:4 options:MTLResourceStorageModeShared];
                id<MTLBuffer> output = [device newBufferWithLength:32 * 4 options:MTLResourceStorageModeShared];
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
                [e setComputePipelineState:state];
                [e setBuffer:input offset:0 atIndex:0];
                [e setBuffer:output offset:0 atIndex:1];
                [e dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [e endEncoding];
                if (finish(cb, "specialized compute")) {
                    const float got = ((const float *)output.contents)[24], expected = mode ? in * 1000 * mode : in;
                    CHECK(got == expected, "mode %d: out[24] %g, expected %g", mode, got, expected);
                }
            }
        }
        step(r, 0x6608);
        // 8. the target's own QuartzCore library through newLibraryWithURL: (a universal metallib; its AIR64 slice): every kernel into a
        // compute pipeline and every <name>_vert_<p> / <name>_frag_<p> pair into a BGRA8 render pipeline. Refusals are counted, not hidden.
        if (qcPath) {
            NSError *error = nil;
            id<MTLLibrary> qc = [device newLibraryWithURL:[NSURL fileURLWithPath:qcPath] error:&error];
            CHECK(qc, "QuartzCore library: %s", error.localizedDescription.UTF8String);
            unsigned kernels = 0, kernelsOk = 0, pairs = 0, pairsOk = 0;
            NSArray<NSString *> *names = qc.functionNames;
            for (NSString *name in names) {
                id<MTLFunction> f = [qc newFunctionWithName:name];
                if (f.functionType == MTLFunctionTypeKernel) {
                    ++kernels;
                    kernelsOk += [device newComputePipelineStateWithFunction:f error:nil] != nil;
                } else if (f.functionType == MTLFunctionTypeVertex) {
                    NSString *fragment = [name stringByReplacingOccurrencesOfString:@"_vert_" withString:@"_frag_"];
                    if ([fragment isEqualToString:name] || ![names containsObject:fragment]) continue;
                    ++pairs;
                    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
                    d.vertexFunction = f;
                    d.fragmentFunction = [qc newFunctionWithName:fragment];
                    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                    pairsOk += [device newRenderPipelineStateWithDescriptor:d error:nil] != nil;
                }
            }
            CHECK(names.count && kernelsOk + pairsOk, "QuartzCore: nothing translated");
            r->qcFunctions = (unsigned)names.count; r->qcKernels = kernels; r->qcKernelsOk = kernelsOk; r->qcPairs = pairs; r->qcPairsOk = pairsOk;
        }
        step(r, 0x660a);
        // 10. A second queue's command buffer waits for value 1, then copies; the first queue fills and signals 1. The waiting one is committed
        // first, so its copy only sees the fill if the GPU wait held. A third waits for 3, which only the CPU signals. Then a fence orders two
        // blit encoders of one command buffer, and the listener is notified at 3.
        {
            id<MTLSharedEvent> event = [device newSharedEvent];
            id<MTLFence> fence = [device newFence];
            id<MTLCommandQueue> other = [device newCommandQueue];
            CHECK(event && fence && other, "event, fence, second queue");
            id<MTLBuffer> a = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
            id<MTLBuffer> b = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
            id<MTLBuffer> c = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
            if (event && fence && other) {
                id<MTLCommandBuffer> waiting = [other commandBuffer];
                [waiting encodeWaitForEvent:event value:1];
                id<MTLBlitCommandEncoder> copy = [waiting blitCommandEncoder];
                [copy copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:4096];
                [copy endEncoding];
                [waiting commit];
                id<MTLCommandBuffer> signalling = [queue commandBuffer];
                id<MTLBlitCommandEncoder> fill = [signalling blitCommandEncoder];
                [fill fillBuffer:a range:NSMakeRange(0, 4096) value:0x3c];
                [fill endEncoding];
                [signalling encodeSignalEvent:event value:1];
                id<MTLCommandBuffer> cpu = [other commandBuffer];
                [cpu encodeWaitForEvent:event value:3];
                id<MTLBlitCommandEncoder> second = [cpu blitCommandEncoder];
                [second fillBuffer:c range:NSMakeRange(0, 64) value:0x77];
                [second updateFence:fence];
                [second endEncoding];
                id<MTLBlitCommandEncoder> third = [cpu blitCommandEncoder];
                [third waitForFence:fence];
                [third copyFromBuffer:c sourceOffset:0 toBuffer:c destinationOffset:2048 size:64];
                [third endEncoding];
                [cpu commit];
                if (finish(signalling, "event signal") && completed(waiting, "event wait")) {
                    CHECK(((const uint8_t *)b.contents)[4095] == 0x3c, "the GPU wait did not order the copy after the fill");
                    CHECK(event.signaledValue == 1, "signaled value %llu", (unsigned long long)event.signaledValue);
                }
                CHECK(cpu.status != MTLCommandBufferStatusCompleted, "a wait for 3 completed before 3 was signaled");
                __block uint64_t heard = 0;
                dispatch_semaphore_t notified = dispatch_semaphore_create(0);
                MTLSharedEventListener *listener = r->hostSimulation ? nil : [[MTLSharedEventListener alloc] init];
                [event notifyListener:listener atValue:3 block:^(id<MTLSharedEvent>, uint64_t value) {heard = value; dispatch_semaphore_signal(notified);}];
                event.signaledValue = 3;
                if (completed(cpu, "CPU-signaled wait")) {
                    const uint8_t *p = (const uint8_t *)c.contents;
                    CHECK(p[2048] == 0x77 && p[2111] == 0x77, "the fence did not order the copy after the fill");
                }
                CHECK(!dispatch_semaphore_wait(notified, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) && heard >= 3,
                      "listener notification %llu", (unsigned long long)heard);
                CHECK([event waitUntilSignaledValue:3 timeoutMS:10], "waitUntilSignaledValue");
            }
        }
        step(r, 0x660b);
        // 11. heaps, a visibility result and a depth bias. An automatic heap's buffer is a buffer of that heap (`heap`, `usedSize`); a placement
        // heap's two buffers are distinct bytes at their offsets; a pass counts the visible quad into its visibility result buffer (Boolean: 1);
        // setDepthBias reaches the runtime without failing the command buffer.
        {
            MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
            hd.size = 65536;
            hd.storageMode = MTLStorageModeShared;
            id<MTLHeap> heap = [device newHeapWithDescriptor:hd];
            CHECK(heap && heap.size >= 65536, "automatic heap");
            if (heap) {
                id<MTLBuffer> hb = [heap newBufferWithLength:4096 options:MTLResourceStorageModeShared];
                CHECK(hb && hb.heap == heap, "a heap buffer names its heap");
                CHECK(heap.usedSize >= 4096, "heap used size %lu", (unsigned long)heap.usedSize);
                if (hb) {
                    id<MTLCommandBuffer> cb = [queue commandBuffer];
                    id<MTLBlitCommandEncoder> fill = [cb blitCommandEncoder];
                    [fill fillBuffer:hb range:NSMakeRange(0, 4096) value:0x5a];
                    [fill endEncoding];
                    if (finish(cb, "heap fill")) CHECK(((const uint8_t *)hb.contents)[0] == 0x5a && ((const uint8_t *)hb.contents)[4095] == 0x5a, "heap buffer contents");
                }
            }
            MTLHeapDescriptor *pd = [MTLHeapDescriptor new];
            pd.size = 65536;
            pd.type = MTLHeapTypePlacement;
            pd.storageMode = MTLStorageModeShared;
            id<MTLHeap> placed = [device newHeapWithDescriptor:pd];
            CHECK(placed && placed.type == MTLHeapTypePlacement, "placement heap");
            if (placed) {
                id<MTLBuffer> first = [placed newBufferWithLength:4096 options:MTLResourceStorageModeShared offset:0];
                id<MTLBuffer> second = [placed newBufferWithLength:4096 options:MTLResourceStorageModeShared offset:4096];
                CHECK(first && second && second.heapOffset == 4096, "placed buffers");
                if (first && second) {
                    id<MTLCommandBuffer> cb = [queue commandBuffer];
                    id<MTLBlitCommandEncoder> fill = [cb blitCommandEncoder];
                    [fill fillBuffer:first range:NSMakeRange(0, 4096) value:0x11];
                    [fill fillBuffer:second range:NSMakeRange(0, 4096) value:0x22];
                    [fill endEncoding];
                    if (finish(cb, "placement fill"))
                        CHECK(((const uint8_t *)first.contents)[4095] == 0x11 && ((const uint8_t *)second.contents)[0] == 0x22, "placed buffers overlap");
                }
            }
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> target = [device newTextureWithDescriptor:td];
            id<MTLBuffer> visible = [device newBufferWithLength:8 options:MTLResourceStorageModeShared];
            if (target && visible) {
                memset(visible.contents, 0, 8);
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = target;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.visibilityResultBuffer = visible;
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
                [e setRenderPipelineState:colored];
                [e setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:0];
                [e setDepthBias:0 slopeScale:0 clamp:0];
                std::vector<float> colors;
                for (unsigned i = 0; i < 6; ++i) colors.insert(colors.end(), {1.0f, 0.2f, 0.0f, 1.0f});
                [e setVertexBytes:kQuarter length:sizeof(float) * 2 * 6 atIndex:0];
                [e setVertexBytes:colors.data() length:colors.size() * sizeof(float) atIndex:1];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                [e endEncoding];
                if (finish(cb, "visibility result")) CHECK(*(const uint64_t *)visible.contents != 0, "visibility result stayed 0 for a visible quad");
            }
        }
        if (layer) CFRelease(layer);
        if (screen) CFRelease(screen);
    }
    step(r, 0x660f);
    gReport = nullptr;
}
#undef CHECK
}  // namespace frontendchecks
