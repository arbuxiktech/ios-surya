/* SURYA bridge to pinned UniXNU ArmSmmu4K: live context & stream attachment.
 * DT-ordered KGSL clocks use branch definitions from the matching driver.
 */
#if defined(ARM64_BOARD_CONFIG_SURYA)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <vm/pmap.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include "surya_unixnu_smmu.h"
#include "surya_gpu_dt.h"
#include "ArmSmmu4KProgram.h"
#include "ArmSmmu4KStreams.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t, addr64_t);

static uint32_t read32(vm_offset_t base, unsigned off)
{
    uint32_t value = *(volatile uint32_t *)(base + off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static uint64_t read64(vm_offset_t base, unsigned off)
{
    uint64_t value = *(volatile uint64_t *)(base + off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static int io_read32(void *cookie, size_t at, uint32_t *out)
{
    vm_offset_t base = (vm_offset_t)cookie;
    *out = read32(base, (unsigned)at);
    return 0;
}

static int io_read64(void *cookie, size_t at, uint64_t *out)
{
    vm_offset_t base = (vm_offset_t)cookie;
    *out = read64(base, (unsigned)at);
    return 0;
}

static int io_write32(void *cookie, size_t at, uint32_t val)
{
    vm_offset_t base = (vm_offset_t)cookie;
    *(volatile uint32_t *)(base + at) = val;
    __asm__ volatile("dmb oshst" ::: "memory");
    return 0;
}

static int io_write64(void *cookie, size_t at, uint64_t val)
{
    vm_offset_t base = (vm_offset_t)cookie;
    *(volatile uint64_t *)(base + at) = val;
    __asm__ volatile("dmb oshst" ::: "memory");
    return 0;
}

static int io_barrier(void *cookie)
{
    (void)cookie;
    __asm__ volatile("dsb osh; isb" ::: "memory");
    return 0;
}

static int live_tables(struct surya_unixnu_smmu_report *r, vm_offset_t smmu)
{
    vm_offset_t tables = 0, data = 0;
    if (PAGE_SIZE != 16384) return 100;
    if (kmem_alloc_contig(kernel_map, &tables, 16384, PAGE_MASK, 0, 0,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
        return 101;
    if (kmem_alloc_contig(kernel_map, &data, 16384, PAGE_MASK, 0, 0,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
        return 102;
    ppnum_t tp = pmap_find_phys(kernel_pmap, (addr64_t)tables);
    ppnum_t dp = pmap_find_phys(kernel_pmap, (addr64_t)data);
    if (!tp || !dp) return 103;
    const uint64_t physical = ptoa_64(tp), data_physical = ptoa_64(dp);
    const uint64_t iova = UINT64_C(0x100000000);
    ArmSmmu4KConfig config = {39, 36, physical, 4};
    ArmSmmu4KRange range = {iova, data_physical, 16384,
        ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2};
    ArmSmmu4KResult built = {0};
    ArmSmmu4KStatus status = ArmSmmu4KBuild(&config, &range, 1,
        (uint64_t *)tables, 4, &built);
    if (status) return status;
    r->table_pages = (unsigned)built.table_pages;
    r->leaves = (unsigned)built.leaf_pages;
    /* Independent CPU walk over the actual newly allocated descriptors. */
    for (unsigned off = 0; off < 16384; ++off) {
        uint64_t table = physical, descriptor = 0;
        for (unsigned shift = 30; ; shift -= 9) {
            if (table < physical || table - physical >= 16384 || (table & 4095))
                return 104;
            descriptor = ((uint64_t *)tables)[(table - physical) / 8 +
                (((iova + off) >> shift) & 511)];
            if ((descriptor & 3) != 3) return 105;
            if (shift == 12) break;
            table = descriptor & UINT64_C(0x0000fffffffff000);
        }
        if ((descriptor & UINT64_C(0x0000fffffffff000)) + (off & 4095) !=
            data_physical + off) return 106;
    }

    /* Clean cache to Point of Coherency (PoC) for table memory so SMMU hardware walker sees descriptors in DDR */
    for (uintptr_t p = (uintptr_t)tables; p < (uintptr_t)tables + 16384; p += 64) {
        __asm__ volatile("dc cvac, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy; isb" ::: "memory");

    /* Populate test data buffer with sentinel pattern */
    *(volatile uint64_t *)data = 0xCAFEBABEDEADBEEFULL;
    for (uintptr_t p = (uintptr_t)data; p < (uintptr_t)data + 16384; p += 64) {
        __asm__ volatile("dc cvac, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy; isb" ::: "memory");

    ArmSmmu4KContextConfig context = {0};
    context.lower = &config;
    context.lower_tables = &built;
    context.id0 = r->id0; context.id2 = r->id2;
    context.asid = 1;
    context.stall_on_fault = 0;
    context.continue_after_fault = 1;
    context.coherent_walk = 0;

    ArmSmmu4KIo io = {
        .cookie = (void *)smmu,
        .register_bytes = SURYA_DT_SMMU_SIZE,
        .read32 = io_read32,
        .read64 = io_read64,
        .write32 = io_write32,
        .write64 = io_write64,
        .barrier = io_barrier,
    };

    printf("SURYA UNIXNU STEP 12: program and activate context bank 0\n");
    delay(20000);
    ArmSmmu4KProgramResult prog_res = {0};
    ArmSmmu4KStatus prog_status = ArmSmmu4KProgramDisabledContext(&io, 0, &context, 10000, &prog_res);
    printf("SURYA UNIXNU PROG: status=%d phase=%u enabled=%u writes=%u fault=%#x\n",
        prog_status, prog_res.phase, prog_res.enabled, prog_res.writes, prog_res.fault_status);
    delay(20000);
    if (prog_status != ArmSmmu4KSuccess) return 30 + (int)prog_status;

    r->active_sctlr = read32(smmu, r->cb_offset);
    r->active_fsr = read32(smmu, r->cb_offset + 0x58);
    r->context_enabled = prog_res.enabled;
    printf("SURYA UNIXNU STEP 13: CB0 SCTLR=%#x FSR=%#x (%s)\n",
        r->active_sctlr, r->active_fsr, r->context_enabled ? "ACTIVE" : "INACTIVE");
    delay(20000);

    printf("SURYA UNIXNU STEP 14: attach GPU stream %llu to context bank 0\n", SURYA_DT_USER_SID);
    delay(20000);

    /* Pre-attach requirement: slot 0 S2CR set to FAULT */
    io_write32((void *)smmu, 0xc00 + 0 * 4, 2u << 16);
    io_barrier(NULL);

    ArmSmmu4KStream stream = { .slot = 0, .sid = (uint16_t)SURYA_DT_USER_SID };
    ArmSmmu4KStreamState stream_state = {0};
    ArmSmmu4KStreamResult stream_res = {0};
    ArmSmmu4KStatus attach_status = ArmSmmu4KAttachStreams(&io, &stream_state, &stream, 1, 0, &context, &stream_res);
    printf("SURYA UNIXNU ATTACH: status=%d verified=%u writes=%u\n",
        attach_status, stream_res.streams_verified, stream_res.writes);
    delay(20000);

    r->smr0 = read32(smmu, 0x800 + 0 * 4);
    r->s2cr0 = read32(smmu, 0xc00 + 0 * 4);
    if (attach_status != ArmSmmu4KSuccess) return 40 + (int)attach_status;
    r->streams_attached = stream_state.attached;
    printf("SURYA UNIXNU STEP 15: SMR0=%#x S2CR0=%#x (%s)\n",
        r->smr0, r->s2cr0, r->streams_attached ? "ATTACHED" : "UNATTACHED");
    delay(20000);

    return 0;
}

static int prepare_dt_clocks(struct surya_unixnu_smmu_report *r)
{
    const uint32_t ids[] = {SURYA_DT_IOMMU_CLK0_ID, SURYA_DT_IOMMU_CLK1_ID, SURYA_DT_IOMMU_CLK2_ID};
    const uint32_t offsets[] = {SURYA_DT_IOMMU_CLK0_OFFSET, SURYA_DT_IOMMU_CLK1_OFFSET, SURYA_DT_IOMMU_CLK2_OFFSET};
    const uint32_t masks[] = {SURYA_DT_IOMMU_CLK0_MASK, SURYA_DT_IOMMU_CLK1_MASK, SURYA_DT_IOMMU_CLK2_MASK};
    printf("SURYA DT CLOCKS: map captured GCC=%#llx size=%#llx\n", SURYA_DT_GCC_PA, SURYA_DT_GCC_SIZE);
    delay(20000);
    vm_offset_t gcc = ml_io_map(SURYA_DT_GCC_PA, SURYA_DT_GCC_SIZE);
    if (!gcc) return 10;
    for (unsigned i = 0; i < 3; ++i) {
        printf("SURYA DT CLOCK %u: ID=%u read branch=%#llx\n", i, ids[i], SURYA_DT_GCC_PA + offsets[i]);
        delay(20000);
        r->memnoc_before = read32(gcc, offsets[i]);
        if (r->memnoc_before == UINT32_MAX || r->memnoc_before == 0xdeafbead) return 11 + i;
        __asm__ volatile("dmb oshst" ::: "memory");
        *(volatile uint32_t *)(gcc + offsets[i]) = r->memnoc_before | masks[i];
        __asm__ volatile("dsb osh" ::: "memory");
        for (unsigned poll = 0; poll < 5000; ++poll) {
            r->memnoc_after = read32(gcc, offsets[i]);
            if (!(r->memnoc_after & (1u << 31))) break;
            delay(1);
        }
        printf("SURYA DT CLOCK %u: before=%#x after=%#x\n", i, r->memnoc_before, r->memnoc_after);
        delay(20000);
        if ((r->memnoc_after & (1u << 31)) || !(r->memnoc_after & masks[i])) return 21 + i;
        r->clocks_ready |= 1u << i;
    }

    printf("SURYA DT GPUCC: map captured GPUCC=%#llx size=%#llx\n", SURYA_DT_GPUCC_PA, SURYA_DT_GPUCC_SIZE);
    delay(20000);
    vm_offset_t gpucc = ml_io_map(SURYA_DT_GPUCC_PA, SURYA_DT_GPUCC_SIZE);
    if (!gpucc) return 14;
    uint32_t smmu_vote = read32(gpucc, 0x5000);
    printf("SURYA DT GPUCC: SMMU vote branch before=%#x\n", smmu_vote);
    delay(20000);
    *(volatile uint32_t *)(gpucc + 0x5000) = smmu_vote | 1u;
    __asm__ volatile("dsb osh" ::: "memory");
    smmu_vote = read32(gpucc, 0x5000);
    printf("SURYA DT GPUCC: SMMU vote branch after=%#x\n", smmu_vote);
    delay(20000);
    if (!(smmu_vote & 1u)) return 24;
    r->clocks_ready |= 1u << 3;

    return 0;
}

void
surya_unixnu_smmu_capture(struct surya_unixnu_smmu_report *r)
{
    *r = (struct surya_unixnu_smmu_report){0};
    r->stage = 1;
    r->code = prepare_dt_clocks(r);
    if (r->code) return;
    printf("SURYA UNIXNU STEP 4: KGSL DT clocks ready; read-only SMMU probe\n");
    delay(20000);
    printf("SURYA DT: SMMU=%#llx bytes=%#llx userSID=%llu secureSID=%llu CB=%#llx\n",
        SURYA_DT_SMMU_PA, SURYA_DT_SMMU_SIZE, SURYA_DT_USER_SID,
        SURYA_DT_SECURE_SID, SURYA_DT_USER_CB_OFFSET);
    printf("SURYA DT: GCC=%#llx MEMNOC clock ID=%llu (clock plan)\n",
        SURYA_DT_GCC_PA, SURYA_DT_MEMNOC_CLOCK_ID);
    r->stage = 2;
    vm_offset_t smmu = ml_io_map(SURYA_DT_SMMU_PA, SURYA_DT_SMMU_SIZE);
    if (!smmu) {r->code = 4; return;}
    ppnum_t mapped = pmap_find_phys(kernel_pmap, (addr64_t)smmu);
    uint64_t translated = ptoa_64(mapped) + (smmu & PAGE_MASK);
    printf("SURYA SMMU CPU MAP: VA=%#llx PA=%#llx expected=%#llx\n",
        (uint64_t)smmu, translated, SURYA_DT_SMMU_PA);
    delay(20000);
    if (!mapped || translated != SURYA_DT_SMMU_PA) {r->code = 6; return;}
    printf("SURYA UNIXNU STEP 5: read ID0 at 0x5040020\n");
    delay(20000);
    r->id0 = read32(smmu, 0x20);
    printf("SURYA UNIXNU STEP 6: ID0=%#x; read ID1\n", r->id0);
    delay(20000);
    r->id1 = read32(smmu, 0x24);
    printf("SURYA UNIXNU STEP 7: ID1=%#x; read ID2\n", r->id1);
    delay(20000);
    r->id2 = read32(smmu, 0x28);
    printf("SURYA UNIXNU STEP 8: ID2=%#x; read SCR0\n", r->id2);
    delay(20000);
    r->scr0 = read32(smmu, 0);
    if (surya_unixnu_cb0(r->id1, SURYA_DT_SMMU_SIZE, &r->cb_offset) ||
        r->cb_offset != SURYA_DT_USER_CB_OFFSET) {
        r->code = 5; return;
    }
    printf("SURYA UNIXNU STEP 9: SCR0=%#x; read CB0 off=%#x\n", r->scr0, r->cb_offset);
    delay(20000);
    r->stage = 3;
    r->cb_sctlr = read32(smmu, r->cb_offset);
    r->cb_fsr = read32(smmu, r->cb_offset + 0x58);
    r->cb_tcr = read32(smmu, r->cb_offset + 0x30);
    printf("SURYA UNIXNU STEP 10: SCTLR=%#x FSR=%#x; read TTBR0\n", r->cb_sctlr, r->cb_fsr);
    delay(20000);
    r->cb_ttbr0 = read64(smmu, r->cb_offset + 0x20);
    printf("SURYA UNIXNU STEP 11: CB0 read; build & activate live tables\n");
    delay(20000);
    r->stage = 4;
    r->code = live_tables(r, smmu);
    if (!r->code) {
        r->stage = 5;
        if (r->context_enabled) r->stage = 6;
        if (r->streams_attached) r->stage = 7;
    }
}
#endif
