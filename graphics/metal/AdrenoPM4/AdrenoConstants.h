/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_CONSTANTS_H
#define ADRENO_CONSTANTS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ADRENO_A740_CHIP_ID UINT64_C(0x43050a01)
#define ADRENO_A740_FALLBACK_CHIP_ID UINT64_C(0xffff43050a01)
#define ADRENO_CONSTANT_MAX_SPANS 64u
#define ADRENO_CONSTANT_MAX_BYTES 8192u
/* Values are this API's stage identities, not an external compiler enum. */
enum AdrenoConstantStage {
    AdrenoConstantVertex, AdrenoConstantTessControl, AdrenoConstantTessEvaluation,
    AdrenoConstantGeometry, AdrenoConstantFragment, AdrenoConstantCompute
};
struct AdrenoConstantConfig {
    uint64_t chip_id; /* Complete identity; never implicitly truncate UAPI IDs. */
    uint32_t stage, constant_bytes;
};
struct AdrenoConstantSpan {
    uint32_t source_byte, destination_byte, bytes;
};
enum AdrenoConstantStatus {
    AdrenoConstantOK, AdrenoConstantArgument, AdrenoConstantProfile,
    AdrenoConstantRange, AdrenoConstantCapacity, AdrenoConstantAlias
};
struct AdrenoConstantResult {
    uint32_t status, words, packets;
};
/* Build inline CP_LOAD_STATE6_GEOM/FRAG constant uploads for exact A740.
 * Accept the two explicitly catalogued base/no-speedbin identities only.
 * Only explicit spans are copied; undeclared constant allocations are untouched.
 * Offsets/sizes are bytes, destination and sizes must be vec4-aligned. Sources
 * are little-endian bytes and need no host alignment. Output uses target-native
 * logical uint32 words; output storage must be aligned and actually accessible.
 * Failure preserves every output byte and returns zero words/packets. Metadata,
 * source and output must not overlap, wrap, or change during the call.
 * Caller must own readable metadata/source lifetimes. No user-copy validation,
 * untrusted command acceptance, cache barriers, indirect GPU addresses, MMIO,
 * submission, fence completion, shader/pixel execution or rendering fallback.
 * Runtime shader/context authorization and pipeline constant sharing are still
 * the driver's responsibility; successful encoding does not authorize submission.
 */
struct AdrenoConstantResult AdrenoA740Constants(
    const struct AdrenoConstantConfig *, const struct AdrenoConstantSpan *, size_t span_count,
    const uint8_t *source, size_t source_bytes, uint32_t *output, size_t output_words);
#ifdef __cplusplus
}
#endif
#endif
