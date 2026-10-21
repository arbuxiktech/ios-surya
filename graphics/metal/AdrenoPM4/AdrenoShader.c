/* SPDX-License-Identifier: MIT */
#include "AdrenoShader.h"
#include "AdrenoPM4.h"
static struct AdrenoShaderResult fail(uint32_t status)
{
    return (struct AdrenoShaderResult){status, 0, 0};
}
static int wraps(uintptr_t p, size_t n) { return n > UINTPTR_MAX - p; }
static int overlaps(uintptr_t a, size_t n, uintptr_t b, size_t m)
{
    return a < b + m && b < a + n;
}
static void reg32(uint32_t **out, uint32_t reg, uint32_t value)
{
    uint32_t header = 0;
    (void)AdrenoPM4Type4(reg, 1, &header);
    *(*out)++ = header;
    *(*out)++ = value;
}
static void reg64(uint32_t **out, uint32_t reg, uint64_t value)
{
    uint32_t header = 0;
    (void)AdrenoPM4Type4(reg, 2, &header);
    *(*out)++ = header;
    *(*out)++ = (uint32_t)value;
    *(*out)++ = (uint32_t)(value >> 32);
}
struct AdrenoShaderResult AdrenoA740ShaderBind(const struct AdrenoShaderState *s,
    const struct AdrenoShaderMapping *m, uint32_t *output, size_t capacity)
{
    if (!s || !m || !output || (uintptr_t)output % _Alignof(uint32_t) ||
        capacity > SIZE_MAX / 4 || wraps((uintptr_t)s, sizeof(*s)) ||
        wraps((uintptr_t)m, sizeof(*m)) || wraps((uintptr_t)output, capacity * 4))
        return fail(AdrenoShaderArgument);
    if ((s->chip_id != ADRENO_A740_CHIP_ID && s->chip_id != ADRENO_A740_FALLBACK_CHIP_ID) ||
        (s->stage != AdrenoConstantVertex && s->stage != AdrenoConstantFragment) ||
        !s->binary_bytes || s->binary_bytes > 1024 * 1024 || s->binary_bytes % 128 ||
        !s->instruction_groups || s->instruction_groups > s->binary_bytes / 128 ||
        s->max_full_register < -1 || s->max_full_register > 62 || s->max_half_register != -1 ||
        s->branch_stack_hw > 126 || s->branch_stack_hw % 2 || s->merged_registers > 1 ||
        s->early_preamble > 1 || s->double_threadsize > 1 || s->need_full_quad > 1 || s->need_pixel_lod > 1 ||
        s->input_words > 128 || s->private_bytes || s->constant_data_bytes ||
        (s->stage == AdrenoConstantVertex && (s->need_full_quad || s->need_pixel_lod)))
        return fail(AdrenoShaderProfile);
    if (!m->context || !m->base || m->base >= (UINT64_C(1) << 48) ||
        !m->bytes || m->bytes > (UINT64_C(1) << 48) - m->base ||
        (m->base & 4095) || (m->bytes & 4095) || m->page_shift != 12 || m->readonly != 1 ||
        (m->shader_offset & 127) || m->shader_offset > m->bytes || s->binary_bytes > m->bytes - m->shader_offset)
        return fail(AdrenoShaderMappingError);
    if (capacity < ADRENO_SHADER_BIND_WORDS)
        return fail(AdrenoShaderCapacity);
    if (overlaps((uintptr_t)output, ADRENO_SHADER_BIND_WORDS * 4, (uintptr_t)s, sizeof(*s)) ||
        overlaps((uintptr_t)output, ADRENO_SHADER_BIND_WORDS * 4, (uintptr_t)m, sizeof(*m)))
        return fail(AdrenoShaderAlias);
    uint32_t *out = output;
    /* Pinned A7xx GL path: shared constants off, constant demotion on.
     * TPL1 CLAMP_DISABLE is A8xx-only and must not leak into A740. */
    reg32(&out, 0xab00, 5);  /* SP_MODE_CNTL */
    reg32(&out, 0xb309, 0xa2); /* TPL1_MODE_CNTL */
    uint32_t control = ((uint32_t)(s->max_full_register + 1) << 7) | (s->branch_stack_hw << 13);
    uint32_t ctrl, instr, pc, base, param, pvt_base, pvt_size, stack, vgs;
    if (s->stage == AdrenoConstantVertex) {
        control |= (s->merged_registers << 20) | (s->early_preamble << 21);
        ctrl=0xa800; instr=0xa824; pc=0xa81b; base=0xa81c; param=0xa81e;
        pvt_base=0xa81f; pvt_size=0xa821; stack=0xa825; vgs=0xa82d;
    } else {
        control |= (s->double_threadsize << 20) | ((s->input_words != 0) << 22) |
            (s->need_full_quad << 23) | (1u << 24) | (s->need_pixel_lod << 26) |
            (s->early_preamble << 28) | (s->merged_registers << 31);
        ctrl=0xa980; instr=0xab05; pc=0xa982; base=0xa983; param=0xa985;
        pvt_base=0xa986; pvt_size=0xa988; stack=0xa9a9; vgs=0xaa01;
    }
    reg32(&out, ctrl, control);
    reg32(&out, instr, s->instruction_groups);
    reg32(&out, pc, 0);
    reg64(&out, base, m->base + m->shader_offset);
    reg32(&out, param, 0);
    reg64(&out, pvt_base, 0);
    reg32(&out, pvt_size, 0);
    reg32(&out, stack, 0);
    reg32(&out, vgs, 0);
    return (struct AdrenoShaderResult){AdrenoShaderOK, ADRENO_SHADER_BIND_WORDS, 13};
}
