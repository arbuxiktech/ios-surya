/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_RASTER_H
#define ADRENO_RASTER_H
#include "AdrenoConstants.h"
#ifdef __cplusplus
extern "C" {
#endif
enum AdrenoRasterFlags {
    AdrenoRasterClipNear=1u, AdrenoRasterClipFar=2u,
    AdrenoRasterHalfZ=4u, AdrenoRasterCullFront=8u,
    AdrenoRasterCullBack=16u, AdrenoRasterFrontCCW=32u,
    AdrenoRasterOffsetTriangles=64u, AdrenoRasterMultisample=128u,
    AdrenoRasterPrimitiveRestart=256u, AdrenoRasterFlatFirst=512u,
    AdrenoRasterPointSizePerVertex=1024u, AdrenoRasterPointQuad=2048u,
    AdrenoRasterPointSmooth=4096u
};
#define ADRENO_RASTER_FLAGS 8191u
#define ADRENO_RASTER_MAX_WORDS 60u
/* polygon_mode: 1 points, 2 lines, 3 triangles. Fixed-point sizes are
 * explicit hardware units: point_size_q4 / 16 pixels, line_width_q1 / 2
 * pixels. The caller performs any API float quantization. Offset values
 * are exact finite binary32 representations. No CPU FP operation is used. */
struct AdrenoRasterState {
    uint64_t chip_id;
    uint32_t flags, polygon_mode, point_size_q4, line_width_q1;
    uint32_t offset_scale_bits, offset_units_bits, offset_clamp_bits, reserved;
};
enum AdrenoRasterStatus {
    AdrenoRasterOK, AdrenoRasterArgument, AdrenoRasterProfile,
    AdrenoRasterCapacity, AdrenoRasterAlias
};
struct AdrenoRasterResult { uint32_t status, words, registers; };
/* Encode the complete pinned fd6 A740 rasterizer-state register set for
 * these explicit fields. Dynamic viewport state, fragment/depth/blend/
 * sample/primitive-draw/input/resource/cache state and live GPU ownership
 * are separate requirements. In particular, when either depth clip flag
 * is disabled, the viewport path MUST program matching GRAS/RB Z clamps.
 * Point-size exports must agree with the separately bound shader/VPC.
 * No allocation, MMIO, submission or rendering. Failure preserves output. */
struct AdrenoRasterResult AdrenoA740Raster(const struct AdrenoRasterState *,
    uint32_t *output, size_t output_words);
#ifdef __cplusplus
}
#endif
#endif
