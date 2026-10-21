// SPDX-License-Identifier: MIT
// Host run of the frontend checks (FrontendChecks.h) over the bridge and a host Vulkan driver as the loader (MoltenVK on the Mac: an offline
// instrument; on the tablet the plugin runs the same checks over Turnip). Prints one PASS line or the failures.
#include "FrontendChecks.h"
#include <string>
#include "../NativeBridgeMetalFrontend.h"
#include "NativeMetalObjects.h"

static std::vector<std::string> missing;
static void onMissing(const char *cls, const char *sel) {missing.push_back(std::string(cls) + " " + sel);}

// argv: loader, shader cache, [QuartzCore default.metallib or ""]
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 3) return 2;
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, onMissing);
        if (!device) {std::printf("FAIL device\n"); return 1;}
        size_t bytes = 0;
        const void *air = nmb_fixture_air(&bytes);
        id<MTLLibrary> library = native_bridge_metal_library_with_air(air, bytes);
        air = nmb_function_constant_air(&bytes);
        id<MTLLibrary> fcLibrary = native_bridge_metal_library_with_air(air, bytes);
        if (!library || !fcLibrary) {std::printf("FAIL fixture libraries\n"); return 1;}
        frontendchecks::Report r = {};
        frontendchecks::run(device, library, fcLibrary, argc > 3 && argv[3][0] ? @(argv[3]) : nil, &r);
        if (r.qcFunctions) std::printf("QuartzCore bare-descriptor probe: %u functions, kernels %u/%u, pairs %u/%u (not complete shader coverage)\n", r.qcFunctions, r.qcKernelsOk, r.qcKernels, r.qcPairsOk, r.qcPairs);
        for (auto &m : missing) std::printf("missing selector: %s\n", m.c_str());
        if (r.failed) {std::printf("FAILED %u checks, first: %s\n", r.failed, r.first); return 1;}
        std::printf("PASS: native Metal frontend over the bridge on %s: compute, render, IOSurface render, surface sampling, texture compute, "
                    "handlers, refusal, function constants, events and fences\n", device.name.UTF8String);
        return 0;
    }
}
