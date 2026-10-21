/* UniXNU ArmSmmu4K integration for SURYA. Live context and stream routing. */
#ifndef SURYA_UNIXNU_SMMU_H
#define SURYA_UNIXNU_SMMU_H
#include "ArmSmmu4KContext.h"

struct surya_unixnu_smmu_report {
    uint32_t memnoc_before, memnoc_after, id0, id1, id2, scr0;
    uint32_t cb_sctlr, cb_fsr, cb_tcr, cb_offset;
    uint64_t cb_ttbr0;
    unsigned table_pages, leaves, clocks_ready;
    int stage, code;
    uint32_t active_sctlr, active_fsr, smr0, s2cr0;
    unsigned context_enabled, streams_attached;
    unsigned sqe_loaded, cp_started;
    uint32_t cp_wptr, cp_rptr, cp_scratch0, cp_scratch1, cp_status1;
    uint32_t cp_intr, cp_sqe_pc, cp_sqe_op;
    unsigned attach_phase, attach_writes;
};

/* Decode real ID1 before touching a context bank. This only authorizes reads
 * inside the mapping, never a bank lease, activation or a stream reassignment. */
static inline int
surya_unixnu_cb0(uint32_t id1, size_t bytes, uint32_t *offset)
{
    if (!offset) return 1;
    *offset = 0;
    unsigned shift = (id1 >> 31) ? 16 : 12;
    size_t pages = (size_t)1 << (((id1 >> 28) & 7) + 1);
    size_t stride = (size_t)1 << shift;
    unsigned banks = id1 & 255, s2 = (id1 >> 16) & 255;
    if (!banks || banks > pages || s2 > banks || s2 != 0 ||
        bytes < 2 * pages * stride) return 1;
    *offset = (uint32_t)(pages * stride);
    return 0;
}

void surya_unixnu_smmu_capture(struct surya_unixnu_smmu_report *);
#endif
