// SPDX-License-Identifier: MIT
// Host check: the system's own device registration (MTLAddDevice, the call the target's discovery makes with a plugin's device) accepts the frontend's
// device. It asserts that the device conforms to Metal's device protocols and runs the base class's initLimits/initFeatureQueries/initWorkarounds on it;
// the Mac's version differs from the target's, but it caught the missing MTLDeviceSPI conformance that the target's _MTLAddDevice also asserts.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#import <objc/runtime.h>
#include <cstdio>
#include "../NativeBridgeMetalFrontend.h"

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 3) return 2;
        void *add = dlsym(dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW), "MTLAddDevice");
        if (!add) {std::printf("SKIP: this system has no MTLAddDevice\n"); return 0;}
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 1, nullptr);
        if (!device) {std::printf("FAIL device\n"); return 1;}
        if (![(id)device conformsToProtocol:objc_getProtocol("MTLDeviceSPI")]) {std::printf("FAIL MTLDeviceSPI conformance\n"); return 1;}
        ((void (*)(id))add)(device);
        bool listed = false;
        for (id<MTLDevice> d in MTLCopyAllDevices()) listed |= (d == device);
        if (!listed) {std::printf("FAIL the registered device is not among the system's devices\n"); return 1;}
        std::printf("PASS: the system's device registration accepted the frontend's device and lists it\n");
    }
    return 0;
}
