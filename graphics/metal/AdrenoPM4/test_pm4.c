/* SPDX-License-Identifier: MIT */
#include "AdrenoPM4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned population(uint32_t word)
{
    unsigned sum = 0;
    for (unsigned bit = 0; bit < 32; ++bit)
        sum += (word >> bit) & 1;
    return sum;
}
static void refusal(const uint32_t *words, size_t length, size_t capacity, AdrenoPM4Status expected)
{
    AdrenoPM4Packet output[8], before[8];
    size_t count = 99;
    memset(output, 0xa5, sizeof(output));
    memcpy(before, output, sizeof(output));
    CHECK(AdrenoPM4Plan(words, length, output, capacity, &count) == expected);
    CHECK(!count && !memcmp(output, before, sizeof(output)));
}
int main(void)
{
    uint32_t header, words[0x4000] = {0};
    AdrenoPM4Packet packet[4];
    size_t count;
    unsigned flips = 0;
    CHECK(AdrenoPM4Type7(0x26, 0, &header) == AdrenoPM4Success && header == 0x70268000);
    CHECK(AdrenoPM4Type7(0x10, 1, &header) == AdrenoPM4Success && header == 0x70100001);
    CHECK(AdrenoPM4Type7(0x3d, 3, &header) == AdrenoPM4Success && header == 0x703d8003);
    CHECK(AdrenoPM4Type4(0, 1, &header) == AdrenoPM4Success && header == 0x48000001);
    /* Entire type-7 field domain: check independent population invariants,
     * exact field preservation, reserved bits and real plan boundaries. */
    for (uint32_t opcode = 0; opcode <= 127; ++opcode) {
        for (uint32_t n = 0; n <= 0x3fff; ++n) {
            CHECK(AdrenoPM4Type7(opcode, n, &header) == AdrenoPM4Success);
            CHECK((header & 0xff004000) == 0x70000000);
            CHECK(((header >> 16) & 127) == opcode && (header & 0x3fff) == n);
            CHECK(population(header & 0xffff) % 2 == 1);
            CHECK(population((header >> 16) & 255) % 2 == 1);
            words[0] = header;
            CHECK(AdrenoPM4Plan(words, n + 1, packet, 1, &count) == AdrenoPM4Success);
            CHECK(count == 1 && packet[0].type == 7 && packet[0].identifier == opcode && packet[0].payload_words == n);
            if (n == 0 || n == 0x3fff) {
                for (unsigned bit = 0; bit < 32; ++bit) {
                    words[0] = header ^ (1u << bit);
                    refusal(words, n + 1, 1, AdrenoPM4InvalidHeader);
                    ++flips;
                }
            }
        }
    }
    /* All supported register indices and all count values. */
    for (uint32_t reg = 0; reg <= 0x3ffff; ++reg) {
        CHECK(AdrenoPM4Type4(reg, 1, &header) == AdrenoPM4Success);
        CHECK(((header >> 8) & 0x3ffff) == reg && (header & 127) == 1);
        CHECK(population(header & 255) % 2 == 1 && population((header >> 8) & 0xfffff) % 2 == 1);
        words[0] = header;
        CHECK(AdrenoPM4Plan(words, 2, packet, 1, &count) == AdrenoPM4Success);
        CHECK(count == 1 && packet[0].type == 4 && packet[0].identifier == reg);
    }
    for (uint32_t n = 1; n <= 127; ++n) {
        CHECK(AdrenoPM4Type4(0, n, &header) == AdrenoPM4Success);
        words[0] = header;
        CHECK(AdrenoPM4Plan(words, n + 1, packet, 1, &count) == AdrenoPM4Success);
        CHECK(packet[0].payload_words == n);
        for (unsigned bit = 0; bit < 32; ++bit) {
            words[0] = header ^ (1u << bit);
            refusal(words, n + 1, 1, AdrenoPM4InvalidHeader);
            ++flips;
        }
    }
    /* Mixed packets, payload containing header-like data and transactional
     * late failures: payload must never be interpreted as packet headers. */
    uint32_t mixed[] = {0x70100001, 0xffffffff, 0x48000001, 0x70000000, 0x70268000};
    CHECK(AdrenoPM4Plan(mixed, 5, packet, 3, &count) == AdrenoPM4Success && count == 3);
    CHECK(packet[0].header_word == 0 && packet[1].header_word == 2 && packet[2].header_word == 4);
    refusal(mixed, 5, 2, AdrenoPM4Capacity);
    refusal(mixed, 3, 3, AdrenoPM4Truncated);
    mixed[4] ^= 1u << 15;
    refusal(mixed, 5, 3, AdrenoPM4InvalidHeader);
    refusal(NULL, 1, 1, AdrenoPM4InvalidArgument);
    refusal(words, SIZE_MAX, 1, AdrenoPM4InvalidArgument);
    CHECK(AdrenoPM4Plan(NULL, 0, NULL, 0, &count) == AdrenoPM4Success && !count);
    CHECK(AdrenoPM4Plan(words, 1, NULL, 1, &count) == AdrenoPM4InvalidArgument && !count);
    CHECK(AdrenoPM4Plan(NULL, 0, NULL, 0, NULL) == AdrenoPM4InvalidArgument);
    header = 0xa5a5a5a5;
    CHECK(AdrenoPM4Type7(128, 1, &header) == AdrenoPM4InvalidArgument);
    CHECK(AdrenoPM4Type7(0, 0x4000, &header) == AdrenoPM4InvalidArgument);
    CHECK(AdrenoPM4Type4(0x40000, 1, &header) == AdrenoPM4InvalidArgument);
    CHECK(AdrenoPM4Type4(0x3ffff, 2, &header) == AdrenoPM4InvalidArgument);
    CHECK(AdrenoPM4Type4(0, 0, &header) == AdrenoPM4InvalidArgument);
    CHECK(AdrenoPM4Type4(0, 128, &header) == AdrenoPM4InvalidArgument);
    CHECK(header == 0xa5a5a5a5);
    CHECK(AdrenoPM4Type7(0, 0, NULL) == AdrenoPM4InvalidArgument);
    CHECK(AdrenoPM4Type4(0, 1, NULL) == AdrenoPM4InvalidArgument);
    printf("PASS: 2097152 type-7 pairs, 262144 registers, 127 counts, %u single-bit corruptions; transactional stream refusals\n", flips);
    return 0;
}
