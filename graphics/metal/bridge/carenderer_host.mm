// SPDX-License-Identifier: MIT
// Host instrument: the Mac's own QuartzCore renders a layer tree through CARenderer into a texture of the native frontend's device (bridge
// over a host Vulkan driver, MoltenVK on the Mac). CoreAnimation's Metal renderer then drives the frontend as the target's render server
// will, and the pixels it produced are checked. With NMB_DEVICE_BASE_NSOBJECT=1 every device selector the renderer sends that the frontend
// lacks is reported as missing. The Mac's QuartzCore is not the target's: this names gaps and checks results, it qualifies nothing on iPadOS.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
#import <objc/runtime.h>
#include <cstdio>
#include <vector>
#include <set>
#include <string>
#include "../NativeBridgeMetalFrontend.h"
#include <cstring>
#include <ctime>
#import <CoreImage/CoreImage.h>
extern "C" size_t native_bridge_metal_telemetry(char *, size_t);

static std::set<std::string> missing;
static void onMissing(const char *cls, const char *sel) {
    if (missing.insert(std::string(cls) + " " + sel).second) std::fprintf(stderr, "missing selector: %s %s\n", cls, sel);
}

// NMB_DISCOVER=1 (instrument only): an unimplemented selector is logged and answers zero instead of raising, so one run lists every gap on
// the renderer's path. Results after a zero answer mean nothing; the frontend itself never answers this way.
static const char *protocolTypes(SEL selector) {
    Protocol *protocols[] = {@protocol(MTLDevice), @protocol(MTLTexture), @protocol(MTLBuffer), @protocol(MTLResource), @protocol(MTLCommandQueue),
                             @protocol(MTLCommandBuffer), @protocol(MTLRenderCommandEncoder), @protocol(MTLBlitCommandEncoder),
                             @protocol(MTLComputeCommandEncoder), @protocol(MTLCommandEncoder), @protocol(MTLLibrary), @protocol(MTLFunction),
                             @protocol(MTLRenderPipelineState), @protocol(MTLComputePipelineState), @protocol(MTLSamplerState),
                             @protocol(MTLDepthStencilState), @protocol(MTLHeap), @protocol(MTLFence), @protocol(MTLEvent), @protocol(MTLSharedEvent)};
    for (Protocol *p : protocols)
        for (BOOL required : {YES, NO}) {
            objc_method_description m = protocol_getMethodDescription(p, selector, required, YES);
            if (m.types) return m.types;
        }
    return nullptr;
}
static NSMethodSignature *discoverSignature(id self, SEL _cmd, SEL selector) {
    const char *types = protocolTypes(selector);
    if (!types) {onMissing(class_getName(object_getClass(self)), (std::string(sel_getName(selector)) + " (no protocol signature)").c_str()); types = "@@:";}
    return [NSMethodSignature signatureWithObjCTypes:types];
}
static void discoverForward(id self, SEL _cmd, NSInvocation *invocation) {
    onMissing(class_getName(object_getClass(self)), sel_getName(invocation.selector));
    const NSUInteger length = invocation.methodSignature.methodReturnLength;
    if (length) {std::vector<uint8_t> zero(length); [invocation setReturnValue:zero.data()];}
}
static void discover(Class cls) {
    class_addMethod(cls, @selector(methodSignatureForSelector:), (IMP)discoverSignature, "@@::");
    class_addMethod(cls, @selector(forwardInvocation:), (IMP)discoverForward, "v@:@");
}

static CGColorRef rgba(double r, double g, double b, double a) {
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGFloat c[4] = {r, g, b, a};
    CGColorRef color = CGColorCreate(space, c);
    CGColorSpaceRelease(space);
    return color;
}

// argv: loader, shader cache. Prints PASS or the first wrong pixel.
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 3) return 2;
        // NMB_SYSTEM_DEVICE=1: the Mac's own Metal device, the instrument's baseline.
        id<MTLDevice> device = getenv("NMB_SYSTEM_DEVICE") ? MTLCreateSystemDefaultDevice() : native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, onMissing);
        if (!device) {std::printf("FAIL device\n"); return 1;}
        if (getenv("NMB_DISCOVER")) {discover(object_getClass(device)); discover(objc_getClass("NBObject"));}
        const NSUInteger side = 64;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:side height:side mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:d];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!target || !queue) {std::printf("FAIL target or queue\n"); return 1;}

        // Blue background; a red square in the top-left quarter; a half-transparent green square in the bottom-right quarter.
        CALayer *root = [CALayer layer];
        root.frame = CGRectMake(0, 0, side, side);
        root.backgroundColor = rgba(0, 0, 1, 1);
        CALayer *red = [CALayer layer];
        red.frame = CGRectMake(0, 0, side / 2, side / 2);
        red.backgroundColor = rgba(1, 0, 0, 1);
        CALayer *green = [CALayer layer];
        green.frame = CGRectMake(side / 2, side / 2, side / 2, side / 2);
        green.backgroundColor = rgba(0, 1, 0, 1);
        green.opacity = 0.5;
        [root addSublayer:red];
        [root addSublayer:green];

        CARenderer *renderer = [CARenderer rendererWithMTLTexture:target options:@{kCARendererMetalCommandQueue: queue}];
        if (!renderer) {std::printf("FAIL CARenderer refused the frontend's texture\n"); return 1;}
        renderer.layer = root;
        renderer.bounds = CGRectMake(0, 0, side, side);
        // The layers' implicit transaction would commit at the run loop's end; the renderer sees only committed trees.
        // NMB_BLUR_TIMING=1: also render a blurred, shadowed layer for several frames and print the frontend's creation telemetry (pipeline
        // counts and times, VkPipeline makings), so the cost of the pipelines CoreAnimation's own renderer asks for can be measured.
        if (getenv("NMB_BLUR_TIMING")) {
            CALayer *blurred = [CALayer layer];
            blurred.frame = CGRectMake(4, 4, side - 8, side - 8);
            blurred.backgroundColor = rgba(1, 1, 0, 0.7);
            blurred.cornerRadius = 9;
            blurred.shadowOpacity = 0.8;
            blurred.shadowRadius = 5;
            CIFilter *blur = [CIFilter filterWithName:@"CIGaussianBlur"];
            [blur setValue:@6 forKey:kCIInputRadiusKey];
            blurred.filters = @[blur];
            [root addSublayer:blurred];
            [CATransaction flush];
            for (int frame = 0; frame < 3; ++frame) {
                const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                [renderer beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
                [renderer addUpdateRect:renderer.bounds];
                [renderer render];
                [renderer endFrame];
                id<MTLCommandBuffer> fence = [queue commandBuffer];
                [fence commit];
                [fence waitUntilCompleted];
                std::printf("BLUR frame %d %.1f ms\n", frame, (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6);
            }
            static char table[16384];
            native_bridge_metal_telemetry(table, sizeof table);
            for (char *line = std::strtok(table, "\n"); line; line = std::strtok(nullptr, "\n"))
                if (line[0] == 'T' || line[0] == 'V' || line[0] == 'S' || line[0] == 'W') std::printf("TELEMETRY %s\n", line);
            [blurred removeFromSuperlayer];
            [CATransaction flush];
        }
        [renderer beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
        [renderer addUpdateRect:renderer.bounds];
        [renderer render];
        [renderer endFrame];
        // CARenderer commits its own command buffers; one more on the same queue completes after them.
        id<MTLCommandBuffer> last = [queue commandBuffer];
        [last commit];
        [last waitUntilCompleted];

        std::vector<uint8_t> pixels(side * side * 4);
        [target getBytes:pixels.data() bytesPerRow:side * 4 fromRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:0];
        // BGRA. Row 0 of the texture is the layer tree's y 0 (the Mac's Metal device renders it so, NMB_SYSTEM_DEVICE=1): red top-left,
        // green bottom-right.
        auto at = [&](NSUInteger x, NSUInteger y) {return &pixels[(y * side + x) * 4];};
        struct Probe {NSUInteger x, y; int b, g, r;} probes[] = {
            {8, 8, 0, 0, 255},               // red quarter
            {side - 8, side - 8, 128, 128, 0}, // green at 0.5 over blue: (0, 0.5, 0.5)
            {side - 8, 8, 255, 0, 0},        // background
            {8, side - 8, 255, 0, 0},        // background
        };
        bool ok = true;
        for (auto &p : probes) {
            const uint8_t *px = at(p.x, p.y);
            if (std::abs(px[0] - p.b) > 3 || std::abs(px[1] - p.g) > 3 || std::abs(px[2] - p.r) > 3 || px[3] != 255) {
                std::printf("FAIL pixel (%lu,%lu) = b%u g%u r%u a%u, want b%d g%d r%d\n", (unsigned long)p.x, (unsigned long)p.y, px[0], px[1], px[2], px[3],
                            p.b, p.g, p.r);
                ok = false;
            }
        }
        std::printf("missing selectors: %zu\n", missing.size());
        if (!ok) return 1;
        std::printf("PASS: the Mac's CARenderer rendered a layer tree on %s\n", device.name.UTF8String);
        return 0;
    }
}
