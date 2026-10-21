// SPDX-License-Identifier: MIT
// Count actual frontend cache-maintenance calls while forwarding every call
// to libSystem. GPU writes and alias reads must still reach the real pages.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>
#include "../NativeBridgeMetalFrontend.h"

static std::atomic<uint64_t> calls, bytes;
extern "C" void sys_dcache_flush(void *address, size_t length) {
    using Flush = void (*)(void *, size_t);
    static Flush real = reinterpret_cast<Flush>(dlsym(RTLD_NEXT, "sys_dcache_flush"));
    if (!real || real == sys_dcache_flush) std::abort();
    calls++;bytes += length;
    real(address, length);
}

static bool run(id<MTLDevice> device, unsigned aliases, unsigned allocations) {
    @autoreleasepool {
        constexpr unsigned width = 64, height = 16, row = width * 4, frameBytes = row * height;
        std::vector<IOSurfaceRef> surfaces;
        auto textures = [NSMutableArray array];
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                          width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
        for (unsigned i = 0; i < allocations; ++i) {
            NSDictionary *properties = @{(id)kIOSurfaceWidth:@(width), (id)kIOSurfaceHeight:@(height),
                (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfaceBytesPerRow:@(row),
                (id)kIOSurfacePixelFormat:@((uint32_t)0x52474241)};
            IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
            if (!surface || IOSurfaceLock(surface, 0, nullptr)) return false;
            memset(IOSurfaceGetBaseAddress(surface), i ? 0xa7 : 0xcc, IOSurfaceGetAllocSize(surface));
            if (IOSurfaceUnlock(surface, 0, nullptr)) return false;
            surfaces.push_back(surface);
        }
        for (unsigned i = 0; i < aliases; ++i) {
            // Include independent imports as well as nested views of an import.
            id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor iosurface:surfaces[i % allocations] plane:0];
            if (i & 1) texture = [[texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm]
                                           newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
            if (!texture) return false;
            [textures addObject:texture];
        }
        std::vector<unsigned char> upload(frameBytes, 0x5a);
        id<MTLBuffer> input = [device newBufferWithBytes:upload.data() length:frameBytes options:MTLResourceStorageModeShared];
        id<MTLBuffer> output = [device newBufferWithLength:aliases * frameBytes options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [[device newCommandQueue] commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!input || !output || !blit) return false;
        [blit copyFromBuffer:input sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:frameBytes
                 sourceSize:MTLSizeMake(width,height,1) toTexture:textures[0] destinationSlice:0 destinationLevel:0
          destinationOrigin:MTLOriginMake(0,0,0)];
        for (unsigned i = 0; i < aliases; ++i)
            [blit copyFromTexture:textures[i] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                      sourceSize:MTLSizeMake(width,height,1) toBuffer:output destinationOffset:i * frameBytes
           destinationBytesPerRow:row destinationBytesPerImage:frameBytes];
        [blit endEncoding];
        const uint64_t beforeCalls = calls.load(), beforeBytes = bytes.load();
        [command commit];[command waitUntilCompleted];
        uint64_t expectedBytes = 0;
        for (IOSurfaceRef surface : surfaces) expectedBytes += 2 * IOSurfaceGetAllocSize(surface);
        bool ok = command.status == MTLCommandBufferStatusCompleted;
        for (unsigned i = 0; i < aliases; ++i) for (unsigned j = 0; j < frameBytes; ++j)
            if (((const unsigned char *)output.contents)[i * frameBytes + j] != (i % allocations ? 0xa7 : 0x5a)) ok = false;
        // The GPU also changed the first surface; check its actual shared bytes.
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < row; ++x)
            if (((const unsigned char *)IOSurfaceGetBaseAddress(surfaces[0]))[y * IOSurfaceGetBytesPerRow(surfaces[0]) + x] != 0x5a) ok = false;
        const uint64_t actualCalls = calls.load() - beforeCalls, actualBytes = bytes.load() - beforeBytes;
        std::printf("aliases=%u allocations=%u flushes=%llu bytes=%llu expected=%u/%llu pixels=%s\n", aliases, allocations,
            (unsigned long long)actualCalls, (unsigned long long)actualBytes, 2 * allocations,
            (unsigned long long)expectedBytes, ok ? "pass" : "FAIL");
        ok &= actualCalls == 2 * allocations && actualBytes == expectedBytes;
        for (IOSurfaceRef surface : surfaces) CFRelease(surface);
        return ok;
    }
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    @autoreleasepool {
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        if (!device) return 2;
        bool ok = run(device, 1, 1);
        ok &= run(device, 128, 1);
        ok &= run(device, 128, 2);
        char telemetry[6144] = {};
        native_bridge_metal_telemetry(telemetry, sizeof telemetry);
        const char *line = strstr(telemetry, "J surface cache ");
        unsigned long long count = 0, maintained = 0, duplicates = 0;
        if (!line || sscanf(line, "J surface cache n=%llu bytes=%llu dup=%llu", &count, &maintained, &duplicates) != 3 ||
            count != calls.load() || maintained != bytes.load() || duplicates != 506) ok = false;
        if (!ok) {std::puts("FAIL: duplicate IOSurface cache maintenance");return 1;}
        std::puts("PASS: each live surface range flushed once per phase, independent imports/views deduplicated, distinct surfaces and GPU bytes preserved");
    }
}
