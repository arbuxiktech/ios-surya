/* SPDX-License-Identifier: MIT */
#include "AdrenoConstants.h"
#include "AdrenoPM4.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef MESA_REFERENCE
#include "freedreno_pm4.h"
#endif
#define WORDS 2304
static uint32_t output[WORDS], before[WORDS];
static unsigned checks, refusals;
static struct AdrenoConstantConfig config = {ADRENO_A740_CHIP_ID, AdrenoConstantVertex, 8192};
static uint32_t reference_state(unsigned stage, unsigned destination, unsigned units)
{
#ifdef MESA_REFERENCE
    const enum a6xx_state_block blocks[] = {SB6_VS_SHADER, SB6_HS_SHADER, SB6_DS_SHADER,
        SB6_GS_SHADER, SB6_FS_SHADER, SB6_CS_SHADER};
    return CP_LOAD_STATE6_0_DST_OFF(destination) | CP_LOAD_STATE6_0_STATE_TYPE(ST6_CONSTANTS) |
        CP_LOAD_STATE6_0_STATE_SRC(SS6_DIRECT) | CP_LOAD_STATE6_0_STATE_BLOCK(blocks[stage]) |
        CP_LOAD_STATE6_0_NUM_UNIT(units);
#else
    const uint32_t blocks[] = {0x200000, 0x240000, 0x280000, 0x2c0000, 0x300000, 0x340000};
    return destination | 0x4000 | blocks[stage] | units * 0x400000;
#endif
}
static void reset(void)
{
    memset(output, 0x6b, sizeof(output));
    memcpy(before, output, sizeof(output));
}
static void rejected(const struct AdrenoConstantConfig *cfg, const struct AdrenoConstantSpan *spans,
                     size_t count, const uint8_t *data, size_t bytes, uint32_t *dst, size_t capacity, unsigned expected)
{
    memcpy(before, output, sizeof(output));
    struct AdrenoConstantResult r = AdrenoA740Constants(cfg, spans, count, data, bytes, dst, capacity);
    assert(r.status == expected && !r.words && !r.packets);
    assert(!memcmp(output, before, sizeof(output)));
    ++refusals;
}
static void positive(const struct AdrenoConstantSpan *spans, size_t count, const uint8_t *data, size_t bytes)
{
    reset();
    struct AdrenoConstantResult r = AdrenoA740Constants(&config, spans, count, data, bytes, output, WORDS);
    assert(r.status == AdrenoConstantOK && r.packets == count);
    AdrenoPM4Packet packets[64];
    size_t packet_count = 0;
    assert(AdrenoPM4Plan(output, r.words, packets, 64, &packet_count) == AdrenoPM4Success && packet_count == count);
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) {
        const struct AdrenoConstantSpan *s = &spans[i];
        unsigned opcode = config.stage < 4 ? 0x32 : 0x34;
#ifdef MESA_REFERENCE
        assert(output[at] == pm4_pkt7_hdr(opcode, 3 + s->bytes / 4));
#endif
        assert(packets[i].header_word == at && packets[i].identifier == opcode && packets[i].type == 7 &&
               packets[i].payload_words == 3 + s->bytes / 4);
        assert(output[at+1] == reference_state(config.stage, s->destination_byte / 16, s->bytes / 16));
        assert(!output[at+2] && !output[at+3]);
        assert(!memcmp(output + at + 4, data + s->source_byte, s->bytes));
        at += 4 + s->bytes / 4;
    }
    assert(r.words == at && !memcmp(output + at, before + at, sizeof(output) - at * 4));
    ++checks;
}
int main(void)
{
    long page = sysconf(_SC_PAGESIZE);
    assert(page >= 4096);
    uint8_t *mapping = mmap(NULL, (size_t)page*3, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(mapping != MAP_FAILED && !mprotect(mapping+2*page, (size_t)page, PROT_NONE));
    uint8_t *data = mapping + 2*page - 8192;
    for (unsigned i = 0; i < 8192; ++i) data[i] = (uint8_t)(i * 7 + 3);
    struct AdrenoConstantSpan one = {0, 0, 16};
    for (unsigned stage = 0; stage < 6; ++stage) {
        config.stage = stage;
        for (unsigned off = 0; off < 512; ++off) {
            one.destination_byte = off*16;
            const unsigned units[] = {1, 512-off, 4};
            for (unsigned i = 0; i < 3; ++i) if (units[i] <= 512-off) {
                one.bytes = units[i]*16;
                one.source_byte = 8192-one.bytes;
                positive(&one, 1, data, 8192);
            }
        }
    }
    config.stage = 0;
    config.chip_id = ADRENO_A740_FALLBACK_CHIP_ID;
    one = (struct AdrenoConstantSpan){0, 0, 16};
    positive(&one, 1, data, 8192);
    config.chip_id = ADRENO_A740_CHIP_ID;
    struct AdrenoConstantSpan spans[64];
    for (unsigned i = 0; i < 64; ++i) spans[i] = (struct AdrenoConstantSpan){i*16, i*32, 16};
    positive(spans, 64, data, 8192);
    /* An unaligned byte source is legal and reads exactly through its logical end. */
    one = (struct AdrenoConstantSpan){0, 0, 16};
    positive(&one, 1, data+1, 16);
    positive(NULL, 0, NULL, 0);
    reset();
    rejected(NULL, &one, 1, data, 8192, output, WORDS, AdrenoConstantArgument);
    rejected(&config, NULL, 1, data, 8192, output, WORDS, AdrenoConstantArgument);
    rejected(&config, &one, 65, data, 8192, output, WORDS, AdrenoConstantArgument);
    rejected(&config, &one, 1, NULL, 8192, output, WORDS, AdrenoConstantArgument);
    rejected(&config, &one, 1, data, 8192, NULL, WORDS, AdrenoConstantArgument);
    rejected(&config, &one, 1, data, 16385, output, WORDS, AdrenoConstantArgument);
    rejected(&config, &one, 1, data, 8192, output, SIZE_MAX, AdrenoConstantArgument);
    rejected(&config, &one, 1, data, 8192, (uint32_t *)((uint8_t *)output+1), WORDS-1, AdrenoConstantArgument);
    rejected(&config, &one, 1, (void *)(UINTPTR_MAX-3), 16, output, WORDS, AdrenoConstantArgument);
    rejected(&config, &one, 1, data, 8192, (void *)(UINTPTR_MAX-3), 8, AdrenoConstantArgument);
    rejected((void *)(UINTPTR_MAX-3), &one, 1, data, 8192, output, WORDS, AdrenoConstantArgument);
    rejected(&config, (void *)(UINTPTR_MAX-3), 1, data, 8192, output, WORDS, AdrenoConstantArgument);
    struct AdrenoConstantConfig bad = config;
    bad.chip_id ^= 1;
    rejected(&bad, &one, 1, data, 8192, output, WORDS, AdrenoConstantProfile);
    bad=config;bad.chip_id=UINT64_C(0xffff000043050a01);
    rejected(&bad, &one, 1, data, 8192, output, WORDS, AdrenoConstantProfile);
    bad=config;bad.stage=6;
    rejected(&bad, &one, 1, data, 8192, output, WORDS, AdrenoConstantProfile);
    bad=config;bad.constant_bytes=8208;
    rejected(&bad, &one, 1, data, 8192, output, WORDS, AdrenoConstantProfile);
    bad=config;bad.constant_bytes=17;
    rejected(&bad, &one, 1, data, 8192, output, WORDS, AdrenoConstantProfile);
    rejected(&config, &one, 1, data, 8192, output, 7, AdrenoConstantCapacity);
    rejected(&config, &one, 1, (const uint8_t *)output, 16, output, WORDS, AdrenoConstantAlias);
    memcpy(output, &config, sizeof(config));
    rejected((const void *)output, &one, 1, data, 8192, output, WORDS, AdrenoConstantAlias);
    memcpy(output, &one, sizeof(one));
    rejected(&config, (const void *)output, 1, data, 8192, output, WORDS, AdrenoConstantAlias);
    const struct AdrenoConstantSpan invalid[] = {{0,0,0},{0,0,15},{0,1,16},{8192,0,16},
        {0,8192,16},{UINT32_MAX,0,16},{0,UINT32_MAX-15,16},{0,0,UINT32_MAX-15}};
    reset();
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
        rejected(&config, &invalid[i], 1, data, 8192, output, WORDS, AdrenoConstantRange);
    struct AdrenoConstantSpan late[] = {{0,0,16},{16,0,16}};
    rejected(&config, late, 2, data, 8192, output, WORDS, AdrenoConstantRange);
    late[1].destination_byte=8192;
    rejected(&config, late, 2, data, 8192, output, WORDS, AdrenoConstantRange);
    assert(!munmap(mapping, (size_t)page*3));
    printf("PASS: %u direct upload cases, six stages/all 512 offsets, maximum extents, guard and %u transactional refusals\n", checks, refusals);
    return 0;
}
