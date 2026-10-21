// SPDX-License-Identifier: MIT
// The plugin's bridge phase (NativeMetalBridgePhase.h) in order over the host simulation: the bridge self-test, then the native frontend on a
// second device over the same Turnip and the frontend checks. The software CP executes nothing, so results are wrong by design; this
// proves every step builds, submits and retires over the host kernel stack and that the second device opens after the first is gone.
// IOSurface steps are skipped (the simulation's IOKit shim hides that service). Prints "frontend failed <n> first <..> last <marker>".
#include "bridge/FrontendChecks.h"
#include "bridge/NativeMetalBridge.h"
#include "bridge/NativeMetalObjects.h"
#include "NativeBridgeMetalFrontend.h"
static void mark(uint32_t v) {std::printf("mark %x\n", v);
    std::fflush(stdout);}
static void missing(const char *c, const char *s) {std::printf("missing %s %s\n", c, s);}
int main(int, char **argv) {
    NmbSelftest r{};
    uint32_t code = nmb_selftest(argv[1], argv[2], nullptr, 0, mark, &r);
    std::printf("selftest code %x stage %x msg %s\n", code, r.stage, r.message);
    id device = native_bridge_metal_open(argv[1], argv[2], 0, missing);
    if (!device) {std::printf("open failed: %s\n", nmb_last_error()); return 1;}
    size_t n = 0;
    const void *a = nmb_fixture_air(&n);
    id lib = native_bridge_metal_library_with_air(a, n);
    a = nmb_function_constant_air(&n);
    id fc = native_bridge_metal_library_with_air(a, n);
    frontendchecks::Report rep = {};
    rep.mark = mark;
    rep.hostSimulation = true;
    frontendchecks::run(device, lib, fc, nil, &rep);
    std::printf("frontend failed %u first %s last %x\n", rep.failed, rep.first, rep.last);
    return 0;
}
