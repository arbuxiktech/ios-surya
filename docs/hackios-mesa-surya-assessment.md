# HackiOS graphics path and what SURYA needs

Inspected local snapshots on 2026-10-22:

- `oosexgang/hackios-xnu-userland` at `906425b75e02b4f99eb5d7bb1ef7c9583472d772`
- `oosexgang/hackios-xnu-kernel` at `849bfffab5e013780bae119cc1859da2d233b22e`

## Mesa / CoreAnimation

HackiOS builds Mesa 25.0.7 with Gallium `softpipe,freedreno`, the MSM kernel
backend, OpenGL ES 2, and OSMesa. Its Darwin `redwood-drm` shim replaces the
Linux libdrm calls with IOKit calls to the kernel's `RedwoodGPU` user client.
When the window server starts, `inject.c` loads `libredwood-gl.dylib` behind
EAGL only if the `RedwoodGPU` service is present (or `gl=1` explicitly selects
Mesa's software path). This is how CoreAnimation can send GLES work to the
Adreno; it is not Vulkan/Turnip. `tools/mk-mesa.sh` sets an empty Vulkan driver
list and disables EGL.

## Metal

Metal uses a separate path from GLES: the userland `redwoodmetal.m` implements
the Metal device/resource/encoder APIs, `tools/mesa/air` translates Apple's AIR
shader bitcode to Mesa NIR, and Freedreno's `ir3` compiles it. Both routes share
the `RedwoodGPU` service and the same kernel command submission. The source
contains real Metal-facing code and device-test harnesses; that does not imply
complete Apple Metal compatibility. The AIR notes still list unsupported Metal
features, and reported shader results are specific to the Redwood device and
the named test corpus.

The kernel driver in `xnu/files/bsd/dev/redwood_adreno.c` is for the Poco X5
Pro's Adreno 642L. `gpu=1` only starts it when the platform peripheral base
matches and its four A660 firmware files are present. It then performs the
642L-specific clock, power, GMU, firmware, SMMU and command-processor setup and
publishes `RedwoodGPU`; `RedwoodGPU.cpp` exposes the MSM-shaped IOKit interface
that both Mesa and the Metal implementation use.

## SURYA port boundary

Poco X3 NFC is SM7150 with Adreno 618 (`0x06010800`), at a different register,
clock, power, SMMU and firmware configuration. The A660 firmware names and
642L sequence cannot be enabled on SURYA by setting `gpu=1` or relabeling
firmware. The matching Mesa GLES and AIR code may be reusable after a genuine
A618 kernel backend provides the same checked buffer, mapping, submission and
fence contract. Port work must first bind resources from SURYA's captured
device tree and its own A618 firmware, then validate power/firmware/SMMU and a
bounded command/fence test before enabling Freedreno. Only after that should
the GLES/CoreAnimation and Metal paths be exercised on this phone.

The current iOS 16 desktop boots with the CPU renderer. This source review did
not enable GPU boot arguments, modify the HackiOS clones, change firmware, or
claim Metal/Mesa rendering on SURYA.
