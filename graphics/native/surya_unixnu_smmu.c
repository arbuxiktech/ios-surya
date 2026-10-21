/* SURYA bridge to pinned UniXNU ArmSmmu4K: live context & stream attachment.
 * DT-ordered KGSL clocks use branch definitions from the matching driver.
 */
#if defined(ARM64_BOARD_CONFIG_SURYA)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <vm/pmap.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
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

static inline uint32_t odd_bit(uint32_t field)
{
    uint32_t parity = 1;
    while (field) {
        parity ^= 1;
        field &= field - 1;
    }
    return parity;
}

static inline uint32_t pm4_type4(uint32_t reg, uint32_t count)
{
    return 0x40000000u | count | (odd_bit(count) << 7) | (reg << 8) | (odd_bit(reg) << 27);
}

static inline uint32_t pm4_type7(uint32_t opcode, uint32_t count)
{
    return 0x70000000u | count | (odd_bit(count) << 15) | (opcode << 16) | (odd_bit(opcode) << 23);
}

static inline void cp_wr(vm_offset_t gpu, uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(gpu + (reg * 4)) = val;
    __asm__ volatile("dmb oshst" ::: "memory");
}

static inline uint32_t cp_rd(vm_offset_t gpu, uint32_t reg)
{
    uint32_t v = *(volatile uint32_t *)(gpu + (reg * 4));
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}

static void execute_cp_ring(struct surya_unixnu_smmu_report *r, vm_offset_t ring)
{
    vm_offset_t gpu = ml_io_map(SURYA_DT_GPU_PA, SURYA_DT_GPU_SIZE);
    if (!gpu) return;

    printf("SURYA UNIXNU STEP 16: initialize Adreno 618 CP ringbuffer\n");
    delay(20000);

    /* Exit secure mode so CP non-secure translations operate */
    cp_wr(gpu, 0xF400, 0);  /* A6XX_RBBM_SECVID_TRUST_CNTL */

    /* Power on SPTPRAC if GMU mapped */
    vm_offset_t gmu = ml_io_map(SURYA_DT_GMU_PA, SURYA_DT_GMU_SIZE);
    if (gmu) {
        *(volatile uint32_t *)(gmu + (0x1A881 * 4)) = 0x00778000;
        __asm__ volatile("dmb oshst" ::: "memory");
    }

    /* Enable 64-bit addressing mode across GPU pipeline */
    cp_wr(gpu, 0x842, 1);  /* A6XX_CP_ADDR_MODE_CNTL */
    cp_wr(gpu, 0xC01, 1);  /* A6XX_VSC_ADDR_MODE_CNTL */
    cp_wr(gpu, 0xE00, 1);  /* A6XX_UCHE_ADDR_MODE_CNTL */
    cp_wr(gpu, 0x8601, 1); /* A6XX_GRAS_ADDR_MODE_CNTL */
    cp_wr(gpu, 0x8E05, 1); /* A6XX_RB_ADDR_MODE_CNTL */
    cp_wr(gpu, 0x9601, 1); /* A6XX_VPC_ADDR_MODE_CNTL */
    cp_wr(gpu, 0x9E01, 1); /* A6XX_PC_ADDR_MODE_CNTL */
    cp_wr(gpu, 0xA601, 1); /* A6XX_VFD_ADDR_MODE_CNTL */
    cp_wr(gpu, 0xAE01, 1); /* A6XX_SP_ADDR_MODE_CNTL */
    cp_wr(gpu, 0xB601, 1); /* A6XX_TPL1_ADDR_MODE_CNTL */
    cp_wr(gpu, 0xBE05, 1); /* A6XX_HLSQ_ADDR_MODE_CNTL */
    cp_wr(gpu, 0xF810, 1); /* A6XX_RBBM_SECVID_TSB_ADDR_MODE_CNTL */

    /* Setup UCHE (Unified Cache Hierarchy Engine) */
    cp_wr(gpu, 0xE3A, 0x10200F9);  /* A6XX_UCHE_GBIF_GX_CONFIG */
    cp_wr(gpu, 0xE01, 1u << 23);   /* A6XX_UCHE_MODE_CNTL: mal=64 */
    cp_wr(gpu, 0xE05, 0xffffffc0); /* A6XX_UCHE_WRITE_RANGE_MAX_LO */
    cp_wr(gpu, 0xE06, 0x0001ffff); /* A6XX_UCHE_WRITE_RANGE_MAX_HI */
    cp_wr(gpu, 0xE07, 0xfffff000); /* A6XX_UCHE_WRITE_THRU_BASE_LO */
    cp_wr(gpu, 0xE08, 0x0001ffff); /* A6XX_UCHE_WRITE_THRU_BASE_HI */
    cp_wr(gpu, 0xE09, 0xfffff000); /* A6XX_UCHE_TRAP_BASE_LO */
    cp_wr(gpu, 0xE0A, 0x0001ffff); /* A6XX_UCHE_TRAP_BASE_HI */
    cp_wr(gpu, 0xE17, 4);          /* A6XX_UCHE_CACHE_WAYS */
    cp_wr(gpu, 0xE18, 0x804);      /* A6XX_UCHE_FILTER_CNTL */
    cp_wr(gpu, 0xE19, 1);          /* A6XX_UCHE_CLIENT_PF */

    /* Setup CP ROQ (Read-Only Queue) and Mem Pool for A618 */
    cp_wr(gpu, 0x8C1, 0x8040362C); /* A6XX_CP_ROQ_THRESHOLDS_1 */
    cp_wr(gpu, 0x8C2, 0x010000C0); /* A6XX_CP_ROQ_THRESHOLDS_2 */
    cp_wr(gpu, 0x8C3, 128);        /* A6XX_CP_MEM_POOL_SIZE */
    cp_wr(gpu, 0x9E07, 0x300 << 11); /* A6XX_PC_DBG_ECO_CNTL */
    cp_wr(gpu, 0x98D, 1);          /* A6XX_CP_AHB_CNTL */

    /* Performance counters & busy contribution */
    cp_wr(gpu, 0x500, 1);          /* A6XX_RBBM_PERFCTR_CNTL */
    cp_wr(gpu, 0x50B, 0xFFFFFFFF); /* A6XX_RBBM_PERFCTR_GPU_BUSY_MASKED */

    /* Disable register protection initially */
    cp_wr(gpu, 0x84F, 0);          /* A6XX_CP_PROTECT_CNTL */

    const uint64_t sqe_iova = UINT64_C(0x100010000);
    const uint64_t ring_iova = UINT64_C(0x100020000);
    const uint64_t rptr_iova = UINT64_C(0x100030000);

    /* Program SQE instruction base */
    cp_wr(gpu, 0x830, (uint32_t)sqe_iova);
    cp_wr(gpu, 0x831, (uint32_t)(sqe_iova >> 32));

    /* Program RPTR shadow address */
    cp_wr(gpu, 0x804, (uint32_t)rptr_iova);
    cp_wr(gpu, 0x805, (uint32_t)(rptr_iova >> 32));

    /* Program ringbuffer control: 16KB -> 0x20b */
    cp_wr(gpu, 0x802, 0x20b);

    /* Program ringbuffer base */
    cp_wr(gpu, 0x800, (uint32_t)ring_iova);
    cp_wr(gpu, 0x801, (uint32_t)(ring_iova >> 32));

    /* Zero WPTR and RPTR */
    cp_wr(gpu, 0x806, 0);
    cp_wr(gpu, 0x807, 0);

    /* Clear scratch registers */
    cp_wr(gpu, 0x880, 0);
    cp_wr(gpu, 0x881, 0);
    cp_wr(gpu, 0x883, 0);
    cp_wr(gpu, 0x884, 0);
    cp_wr(gpu, 0x885, 0);

    /* Clear SQE_HALT to start CP engine */
    cp_wr(gpu, 0x808, 1);
    r->cp_started = 1;

    printf("SURYA UNIXNU STEP 17: CP SQE unhalted; emit PM4 packets\n");
    delay(20000);

    /* Emit PM4 packets into ring buffer */
    volatile uint32_t *rb = (volatile uint32_t *)ring;
    unsigned idx = 0;

    /* Packet 0: CP_ME_INIT (opcode 0x48, 11 words payload) */
    rb[idx++] = pm4_type7(0x48, 11);
    rb[idx++] = 0x0000016f; /* CP_INIT_MASK: max contexts, error detect, hdr dump, reset, ucode, op mode, pwrup */
    rb[idx++] = 0x00000003; /* 3 hardware contexts */
    rb[idx++] = 0x20000000; /* error detection control */
    rb[idx++] = 0x00000000; /* header dump address */
    rb[idx++] = 0x00000000; /* header dump enable/size */
    rb[idx++] = 0x00000000; /* ucode workaround */
    rb[idx++] = 0x00000002; /* operation mode */
    rb[idx++] = 0x00000000; /* pwrup reglist low */
    rb[idx++] = 0x00000000; /* pwrup reglist high */
    rb[idx++] = 0x00000000; /* pwrup reglist size */
    rb[idx++] = 0x00000000; /* pad to 11 words */

    /* Packet 1: CP_SET_PROTECTED_MODE (opcode 0x5f, 1 word: 0 = unprotect) */
    rb[idx++] = pm4_type7(0x5f, 1);
    rb[idx++] = 0x00000000;

    /* Packet 2: Type 4 writing to A6xx scratch registers (0x883..0x884) */
    rb[idx++] = pm4_type4(0x883, 2);
    rb[idx++] = 0x1337BEEF;
    rb[idx++] = 0xFEEDCAFE;

    /* Packet 3: Type 4 writing to legacy/A5xx scratch registers (0x880..0x881) */
    rb[idx++] = pm4_type4(0x880, 2);
    rb[idx++] = 0x1337BEEF;
    rb[idx++] = 0xFEEDCAFE;

    /* Clean and invalidate cache for ring buffer */
    for (uintptr_t p = (uintptr_t)ring; p < (uintptr_t)ring + 16384; p += 64) {
        __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy; isb" ::: "memory");

    /* Advance WPTR */
    r->cp_wptr = idx;
    cp_wr(gpu, 0x807, idx);

    printf("SURYA UNIXNU STEP 18: WPTR written (%u words); polling execution\n", idx);
    delay(20000);

    /* Poll for execution */
    for (unsigned poll = 0; poll < 3000; ++poll) {
        uint32_t s0_883 = cp_rd(gpu, 0x883);
        uint32_t s1_884 = cp_rd(gpu, 0x884);
        uint32_t s0_880 = cp_rd(gpu, 0x880);
        uint32_t s1_881 = cp_rd(gpu, 0x881);
        r->cp_rptr = cp_rd(gpu, 0x806);
        r->cp_status1 = cp_rd(gpu, 0x825);
        r->cp_intr = cp_rd(gpu, 0x823);
        r->cp_hw_fault = cp_rd(gpu, 0x821);
        cp_wr(gpu, 0x908, 0);
        r->cp_sqe_pc = cp_rd(gpu, 0x909);
        cp_wr(gpu, 0x908, 1);
        r->cp_sqe_op = cp_rd(gpu, 0x909);
        if (s0_883 == 0x1337BEEFu || s0_880 == 0x1337BEEFu) {
            r->cp_scratch0 = (s0_883 == 0x1337BEEFu) ? s0_883 : s0_880;
            r->cp_scratch1 = (s0_883 == 0x1337BEEFu) ? s1_884 : s1_881;
            break;
        }
        r->cp_scratch0 = s0_883 ? s0_883 : s0_880;
        r->cp_scratch1 = s1_884 ? s1_884 : s1_881;
        if (r->cp_rptr == r->cp_wptr) break;
        delay(100);
    }

    printf("SURYA UNIXNU CP RESULT: WPTR=%u RPTR=%u SCR0=%#x SCR1=%#x STAT1=%#x INTR=%#x FAULT=%#x PC=%#x OP=%#x\n",
        r->cp_wptr, r->cp_rptr, r->cp_scratch0, r->cp_scratch1, r->cp_status1,
        r->cp_intr, r->cp_hw_fault, r->cp_sqe_pc, r->cp_sqe_op);
    delay(20000);
}

static int live_tables(struct surya_unixnu_smmu_report *r, vm_offset_t smmu)
{
    vm_offset_t tables = 0, data = 0, sqe = 0, ring = 0, rptr = 0;
    if (PAGE_SIZE != 16384) return 100;
    if (kmem_alloc_contig(kernel_map, &tables, 16384, PAGE_MASK, 0, 0,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
        return 101;
    if (kmem_alloc_contig(kernel_map, &data, 16384, PAGE_MASK, 0, 0,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
        return 102;
    if (kmem_alloc_contig(kernel_map, &sqe, 32768, PAGE_MASK, 0, 0,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
        return 107;
    if (kmem_alloc_contig(kernel_map, &ring, 16384, PAGE_MASK, 0, 0,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
        return 108;
    if (kmem_alloc_contig(kernel_map, &rptr, 16384, PAGE_MASK, 0, 0,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
        return 109;

    ppnum_t tp = pmap_find_phys(kernel_pmap, (addr64_t)tables);
    ppnum_t dp = pmap_find_phys(kernel_pmap, (addr64_t)data);
    ppnum_t sp = pmap_find_phys(kernel_pmap, (addr64_t)sqe);
    ppnum_t rp = pmap_find_phys(kernel_pmap, (addr64_t)ring);
    ppnum_t rptp = pmap_find_phys(kernel_pmap, (addr64_t)rptr);
    if (!tp || !dp || !sp || !rp || !rptp) return 103;

    const uint64_t physical = ptoa_64(tp), data_physical = ptoa_64(dp);
    const uint64_t sqe_physical = ptoa_64(sp), ring_physical = ptoa_64(rp), rptr_physical = ptoa_64(rptp);

    /* Check for SQE microcode in device tree */
    DTEntry chosen = NULL;
    void const *sqe_fw = NULL;
    unsigned int sqe_fw_size = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess) {
        if (SecureDTGetProperty(chosen, "surya-sqe-firmware", &sqe_fw, &sqe_fw_size) == kSuccess &&
            sqe_fw && sqe_fw_size > 4 && sqe_fw_size <= 32768) {
            /* Skip first 4-byte dword (firmware header) as required by Adreno SQE format */
            bcopy((const char *)sqe_fw + 4, (void *)sqe, sqe_fw_size - 4);
            r->sqe_loaded = 1;
        }
    }

    /* Populate test data buffer with sentinel pattern */
    *(volatile uint64_t *)data = 0xCAFEBABEDEADBEEFULL;

    /* Clean and invalidate cache to Point of Coherency (PoC) for data and firmware buffers */
    for (uintptr_t p = (uintptr_t)data; p < (uintptr_t)data + 16384; p += 64) {
        __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
    }
    for (uintptr_t p = (uintptr_t)sqe; p < (uintptr_t)sqe + 32768; p += 64) {
        __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
    }
    for (uintptr_t p = (uintptr_t)ring; p < (uintptr_t)ring + 16384; p += 64) {
        __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
    }
    for (uintptr_t p = (uintptr_t)rptr; p < (uintptr_t)rptr + 16384; p += 64) {
        __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy; isb" ::: "memory");

    const uint64_t iova = UINT64_C(0x100000000);
    ArmSmmu4KConfig config = {39, 36, physical, 32};
    ArmSmmu4KRange ranges[4] = {
        {iova, data_physical, 16384, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2},
        {UINT64_C(0x100010000), sqe_physical, 32768, ArmSmmu4KRead | ArmSmmu4KWrite | ArmSmmu4KExecute, 0, 2},
        {UINT64_C(0x100020000), ring_physical, 16384, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2},
        {UINT64_C(0x100030000), rptr_physical, 16384, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2},
    };
    ArmSmmu4KResult built = {0};
    ArmSmmu4KStatus status = ArmSmmu4KBuild(&config, ranges, 4,
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

    /* Clean and invalidate cache to Point of Coherency (PoC) for table memory so SMMU hardware walker sees descriptors in DDR */
    for (uintptr_t p = (uintptr_t)tables; p < (uintptr_t)tables + 16384; p += 64) {
        __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy; isb" ::: "memory");

    /* Populate test data buffer with sentinel pattern */
    *(volatile uint64_t *)data = 0xCAFEBABEDEADBEEFULL;
    for (uintptr_t p = (uintptr_t)data; p < (uintptr_t)data + 16384; p += 64) {
        __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
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

    /* Program stock DTB attach-impl-defs into SMMU GR0 */
    static const struct { uint32_t reg; uint32_t val; } kAttachImplDefs[] = {
        {0x6000, 0x00002378}, {0x6060, 0x00001055}, {0x678c, 0x00000008},
        {0x6794, 0x00000028}, {0x6800, 0x00000006}, {0x6900, 0x000003ff},
        {0x6924, 0x00000204}, {0x6928, 0x00011000}, {0x6930, 0x00000800},
        {0x6960, 0xffffffff}, {0x6b64, 0x001a5551}, {0x6b68, 0x9a82a382}
    };
    for (unsigned i = 0; i < sizeof(kAttachImplDefs)/sizeof(kAttachImplDefs[0]); ++i) {
        io_write32((void *)smmu, kAttachImplDefs[i].reg, kAttachImplDefs[i].val);
    }
    io_barrier(NULL);

    /* Qualcomm QSMMUv2 Auxiliary Control: NSH, ISH, OSH override */
    io_write32((void *)smmu, r->cb_offset + 0x4, 0x70000000);
    io_barrier(NULL);

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
    printf("SURYA UNIXNU ATTACH: status=%d phase=%u verified=%u writes=%u\n",
        attach_status, stream_res.phase, stream_res.streams_verified, stream_res.writes);
    delay(20000);

    r->attach_phase = stream_res.phase;
    r->attach_writes = stream_res.writes;
    r->smr0 = read32(smmu, 0x800 + 0 * 4);
    r->s2cr0 = read32(smmu, 0xc00 + 0 * 4);
    if (stream_state.attached || (attach_status == ArmSmmu4KSuccess) ||
        (r->smr0 == (0x80000000u | (uint32_t)SURYA_DT_USER_SID) && (r->s2cr0 & 0x30000) == 0)) {
        r->streams_attached = 1;
    }
    printf("SURYA UNIXNU STEP 15: SMR0=%#x S2CR0=%#x (%s)\n",
        r->smr0, r->s2cr0, r->streams_attached ? "ATTACHED" : "UNATTACHED");
    delay(20000);

    /* Full SMMU CB0 TLB invalidation */
    io_write32((void *)smmu, r->cb_offset + 0x618, 0);
    io_write32((void *)smmu, r->cb_offset + 0x7f0, 0);
    for (unsigned poll = 0; poll < 1000; ++poll) {
        if (!(read32(smmu, r->cb_offset + 0x7f4) & 1)) break;
        delay(1);
    }
    io_barrier(NULL);

    if (r->sqe_loaded && r->streams_attached) {
        execute_cp_ring(r, ring);
    }

    if (attach_status != ArmSmmu4KSuccess && !r->streams_attached) return 40 + (int)attach_status;
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
    if (!r->code || r->streams_attached) {
        r->stage = 5;
        if (r->context_enabled) r->stage = 6;
        if (r->streams_attached) r->stage = 7;
        if (r->cp_scratch0 == 0x1337BEEFu) r->stage = 8;
    }
}
#endif
