/* SPDX-License-Identifier: MIT */
#include "AdrenoConstants.h"
#include "AdrenoPM4.h"

static struct AdrenoConstantResult fail(uint32_t status)
{
    return (struct AdrenoConstantResult){status, 0, 0};
}
static int wraps(uintptr_t pointer, size_t bytes)
{
    return bytes > UINTPTR_MAX - pointer;
}
static int overlaps(uintptr_t a, size_t an, uintptr_t b, size_t bn)
{
    return an && bn && a < b + bn && b < a + an;
}
struct AdrenoConstantResult AdrenoA740Constants(
    const struct AdrenoConstantConfig *cfg, const struct AdrenoConstantSpan *spans, size_t count,
    const uint8_t *source, size_t source_bytes, uint32_t *output, size_t capacity)
{
    if (!cfg || count > ADRENO_CONSTANT_MAX_SPANS || (count && (!spans || !source || !output)) ||
        source_bytes > 16384 || capacity > SIZE_MAX / sizeof(*output) ||
        (count && ((uintptr_t)output % _Alignof(uint32_t))) ||
        wraps((uintptr_t)cfg, sizeof(*cfg)) || wraps((uintptr_t)spans, count * sizeof(*spans)) ||
        wraps((uintptr_t)source, source_bytes) || wraps((uintptr_t)output, capacity * sizeof(*output)))
        return fail(AdrenoConstantArgument);
    if ((cfg->chip_id != ADRENO_A740_CHIP_ID && cfg->chip_id != ADRENO_A740_FALLBACK_CHIP_ID) || cfg->stage > AdrenoConstantCompute ||
        cfg->constant_bytes > ADRENO_CONSTANT_MAX_BYTES || cfg->constant_bytes % 16)
        return fail(AdrenoConstantProfile);
    size_t words = 0;
    for (size_t i = 0; i < count; ++i) {
        const struct AdrenoConstantSpan *s = &spans[i];
        if (!s->bytes || s->bytes % 16 || s->destination_byte % 16 ||
            s->source_byte > source_bytes || s->bytes > source_bytes - s->source_byte ||
            s->destination_byte > cfg->constant_bytes || s->bytes > cfg->constant_bytes - s->destination_byte)
            return fail(AdrenoConstantRange);
        for (size_t j = 0; j < i; ++j) {
            const struct AdrenoConstantSpan *p = &spans[j];
            if (overlaps(s->destination_byte, s->bytes, p->destination_byte, p->bytes))
                return fail(AdrenoConstantRange);
        }
        words += 4 + s->bytes / 4;
    }
    if (words > capacity)
        return fail(AdrenoConstantCapacity);
    size_t bytes = words * sizeof(*output);
    uintptr_t dest = (uintptr_t)output;
    if (overlaps(dest, bytes, (uintptr_t)cfg, sizeof(*cfg)) ||
        overlaps(dest, bytes, (uintptr_t)spans, count * sizeof(*spans)) ||
        overlaps(dest, bytes, (uintptr_t)source, source_bytes))
        return fail(AdrenoConstantAlias);
    size_t at = 0;
    uint32_t opcode = cfg->stage <= AdrenoConstantGeometry ? 0x32 : 0x34;
    uint32_t block = 8 + cfg->stage;
    for (size_t i = 0; i < count; ++i) {
        const struct AdrenoConstantSpan *s = &spans[i];
        uint32_t num_unit = s->bytes / 16;
        uint32_t header = 0;
        (void)AdrenoPM4Type7(opcode, 3 + s->bytes / 4, &header);
        output[at++] = header;
        /* XML: DST_OFF[13:0], TYPE[15:14]=CONSTANTS(1),
         * SRC[17:16]=DIRECT(0), BLOCK[21:18], UNIT[31:22]. */
        output[at++] = s->destination_byte / 16 | (1u << 14) | (block << 18) | (num_unit << 22);
        output[at++] = 0; /* direct source: no external GPU address */
        output[at++] = 0;
        for (uint32_t j = 0; j < s->bytes; j += 4) {
            const uint8_t *p = source + s->source_byte + j;
            output[at++] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
    }
    return (struct AdrenoConstantResult){AdrenoConstantOK, (uint32_t)words, (uint32_t)count};
}
