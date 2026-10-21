/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_VPC_H
#define ADRENO_VPC_H
#include "AdrenoConstants.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ADRENO_VPC_MAX_OUTPUTS 16u
#define ADRENO_VPC_MAX_INPUTS 15u
#define ADRENO_VPC_MAX_WORDS 52u
struct AdrenoVPCOutput {
    const uint8_t *identity;
    size_t identity_bytes;
    uint32_t register_id, components, bit_size, half, view, aliased_components;
};
struct AdrenoVPCInput {
    const uint8_t *identity;
    size_t identity_bytes;
    uint32_t components, bit_size, component_mask, input_location, interpolation;
};
struct AdrenoVPCState {
    uint64_t chip_id;
    int32_t vertex_max_register;
    uint32_t reserved;
    const struct AdrenoVPCOutput *outputs;
    size_t output_count;
    const struct AdrenoVPCInput *inputs;
    size_t input_count;
};
enum AdrenoVPCStatus {
    AdrenoVPCOK, AdrenoVPCArgument, AdrenoVPCProfile, AdrenoVPCIdentity,
    AdrenoVPCMissingVarying, AdrenoVPCCapacity, AdrenoVPCAlias
};
struct AdrenoVPCResult {
    uint32_t status, words, registers, varying_count, stride, position_location;
};
/* Link exact semantic identities to actual IR3 registers/inloc, then encode
 * the A740 VS->VPC linkage subset. Requires float32 smooth-center inputs,
 * actual compiler output/register metadata and one float4 air.position.
 * Other builtins, alias/half outputs and tessellation/geometry/streamout are
 * outside this profile. Missing/mismatched varyings refuse; no zero synthesis.
 * Output is transactional, including metadata/name aliases. Immutable readable
 * metadata/backing lifetime is the caller's responsibility. No allocation,
 * MMIO, mappings, submission or completion; the driver still needs the complete
 * primitive/raster/resource/cache state and a live owned native-4K GPU context. */
struct AdrenoVPCResult AdrenoA740VaryingMap(const struct AdrenoVPCState *, uint32_t *, size_t);
#ifdef __cplusplus
}
#endif
#endif
