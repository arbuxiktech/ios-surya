/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_SHADER_H
#define ADRENO_SHADER_H
#include "AdrenoConstants.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ADRENO_SHADER_BIND_WORDS 24u
struct AdrenoShaderState {
    uint64_t chip_id;
    uint32_t stage, binary_bytes, instruction_groups;
    int32_t max_full_register, max_half_register;
    uint32_t branch_stack_hw, merged_registers, early_preamble;
    uint32_t double_threadsize, input_words, need_full_quad, need_pixel_lod;
    uint32_t private_bytes, constant_data_bytes;
};
/* A view into a caller-owned native-4K readonly shader mapping. This record
 * describes ownership; it does not verify or activate an SMMU context. */
struct AdrenoShaderMapping {
    uint64_t context, base, bytes, shader_offset;
    uint32_t page_shift, readonly;
};
enum AdrenoShaderStatus {
    AdrenoShaderOK, AdrenoShaderArgument, AdrenoShaderProfile,
    AdrenoShaderMappingError, AdrenoShaderCapacity, AdrenoShaderAlias
};
struct AdrenoShaderResult { uint32_t status, words, registers; };
/* Encode the A740 fd6_emit_shader register subset for real vertex/fragment
 * variants without private memory/constant-data blobs. All actual compiler
 * binding fields must be supplied; do not infer them from code size or names.
 * Both full catalog identities are checked. No A6xx instruction-preload packet
 * is emitted. Logical shader extent must fit its declared 4K resource view.
 * Before submission the driver must qualify/own that live native-4K context,
 * immutable shader bytes, complete pipeline/cache/barrier state and lifetime.
 * This function does not accept user command streams or authorize submission.
 * Failure preserves output, including capacity/metadata alias and pointer wrap.
 * No allocation, MMIO, CPU rendering, simulated mappings or completion. */
struct AdrenoShaderResult AdrenoA740ShaderBind(const struct AdrenoShaderState *,
    const struct AdrenoShaderMapping *, uint32_t *output, size_t output_words);
#ifdef __cplusplus
}
#endif
#endif
