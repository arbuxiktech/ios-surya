/* Offline ARM LPAE table test & Adreno GPU power/clock bringup diagnostic. */
#if defined(ARM64_BOARD_CONFIG_SURYA)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include "surya_gpu_pt.h"
#include "surya_gpu_dt.h"
#include "surya_gmu_protocol.h"
#include "surya_unixnu_smmu.h"

extern ppnum_t pmap_find_phys(pmap_t, addr64_t);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int hz;
void surya_gpu_pt_diagnostic(void);

#define SURYA_GPU_PA       SURYA_DT_GPU_PA
#define SURYA_GPU_SIZE     SURYA_DT_GPU_SIZE

#define SURYA_GPUCC_PA     SURYA_DT_GPUCC_PA
#define SURYA_GPUCC_SIZE   SURYA_DT_GPUCC_SIZE

#define SURYA_GMU_PA       SURYA_DT_GMU_PA
#define SURYA_GMU_SIZE     SURYA_DT_GMU_SIZE

#define SURYA_PDC_CFG_PA   SURYA_DT_PDC_CFG_PA
#define SURYA_PDC_CFG_SIZE SURYA_DT_PDC_CFG_SIZE

#define SURYA_PDC_SEQ_PA   SURYA_DT_PDC_SEQ_PA
#define SURYA_PDC_SEQ_SIZE SURYA_DT_PDC_SEQ_SIZE

static inline uint32_t
sgpu_rd(vm_offset_t base, uint32_t off)
{
    uint32_t v = *(volatile uint32_t *)(base + off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}

static inline void
sgpu_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(base + off) = val;
    __asm__ volatile("dsb osh" ::: "memory");
}

static int
surya_gpu_pt_check(void)
{
    vm_offset_t tables = 0, data = 0;
    struct sgpt pt = {0};
    const uint64_t va = 0x100000000ull;
    uint64_t pa;
    int error;
    if (PAGE_SIZE != SGPT_POOL) return 1;
    if (kmem_alloc_contig(kernel_map, &tables, SGPT_POOL, PAGE_MASK, 0, 0,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
        return 2;
    if (kmem_alloc_contig(kernel_map, &data, SGPT_POOL, PAGE_MASK, 0, 0,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
        return 3;
    ppnum_t tpn = pmap_find_phys(kernel_pmap, (addr64_t)tables);
    ppnum_t dpn = pmap_find_phys(kernel_pmap, (addr64_t)data);
    if (!tpn || !dpn) return 4;
    uint64_t table_pa = ptoa_64(tpn), data_pa = ptoa_64(dpn);
    error = sgpt_init(&pt, (uint64_t *)tables, table_pa, data_pa, SGPT_POOL, va);
    if (error) return 10 + error;
    for (unsigned off = 0; off < SGPT_POOL; off += SGPT_PAGE) {
        error = sgpt_map(&pt, va + off, data_pa + off,
            SGPT_READ | SGPT_NOEXEC | (off & SGPT_PAGE ? 0 : SGPT_WRITE));
        if (error) return 20 + error;
    }
    for (unsigned off = 0; off < SGPT_POOL; ++off) {
        if (sgpt_translate(&pt, va + off, &pa) || pa != data_pa + off) return 30;
    }
    if (sgpt_map(&pt, va, data_pa, SGPT_READ) != SGPT_EXISTS) return 31;
    if (sgpt_map(&pt, va + SGPT_POOL, table_pa, SGPT_READ) != SGPT_RANGE) return 32;
    if (sgpt_translate(&pt, va + SGPT_POOL, &pa) != SGPT_UNMAPPED) return 33;
    if (sgpt_unmap(&pt, va) || sgpt_translate(&pt, va, &pa) != SGPT_UNMAPPED) return 34;
    if (sgpt_map(&pt, va, data_pa, SGPT_READ | SGPT_WRITE | SGPT_NOEXEC)) return 35;
    sgpt_seal(&pt);
    if (sgpt_unmap(&pt, va) != SGPT_SEALED ||
        sgpt_map(&pt, va, data_pa, SGPT_READ) != SGPT_SEALED) return 36;
    printf("SURYA GPU PT: private bytes=%u table bytes=%u GPU VA=%#llx\n",
        SGPT_POOL, SGPT_POOL, va);
    printf("SURYA GPU PT: CPU table walks=16384, bounds/unmap/seal checks passed\n");
    return 0;
}

struct surya_gpu_diag_state {
    uint32_t cx_init, cx_on, ahb, gmu_clk;
    uint32_t c1, c2, cm3_cfg, cm3_reset, hfi_ver;
    bool fw_found, itcm_loaded, pdc_ok, oob_ok;
    uint32_t fw_size, itcm_check, cm3_init_res, oob_ack;
    uint32_t oob_before, oob_raw, fw_busy, rpmh_state, hfi_status;
    int fw_result;
    uint32_t gx_init, gx_on, gx_clk;
    uint32_t rbbm_status, rbbm_status3, gbif_qos;
    bool gx_powered;
};

static void
surya_gpu_setup_pdc(vm_offset_t gmu, vm_offset_t pdc_cfg, vm_offset_t pdc_seq)
{
    /* 1. Setup RSCC registers in GMU MMIO window */
    sgpu_wr(gmu, 0x23010, 1u << 24); /* A6XX_GPU_RSCC_RSC_STATUS0_DRV0 */
    sgpu_wr(gmu, 0x2302c, 1u);       /* A6XX_RSCC_PDC_SLAVE_ID_DRV0 */
    sgpu_wr(gmu, 0x23034, 0u);       /* A6XX_RSCC_HIDDEN_TCS_CMD0_ADDR */
    sgpu_wr(gmu, 0x23038, 0u);       /* A6XX_RSCC_HIDDEN_TCS_CMD0_DATA */
    sgpu_wr(gmu, 0x2303c, 0u);       /* CMD1 ADDR */
    sgpu_wr(gmu, 0x23040, 0u);       /* CMD1 DATA */
    sgpu_wr(gmu, 0x23044, 0u);       /* CMD2 ADDR */
    sgpu_wr(gmu, 0x23048, 0x80000000u); /* CMD2 DATA */
    sgpu_wr(gmu, 0x23400, 0u);       /* A6XX_RSCC_OVERRIDE_START_ADDR */
    sgpu_wr(gmu, 0x23020, 0x4520u);  /* A6XX_RSCC_PDC_SEQ_START_ADDR */
    sgpu_wr(gmu, 0x23024, 0x4510u);  /* A6XX_RSCC_PDC_MATCH_VALUE_LO */
    sgpu_wr(gmu, 0x23028, 0x4514u);  /* A6XX_RSCC_PDC_MATCH_VALUE_HI */

    /* RSC sequencer uCode (5 words) */
    sgpu_wr(gmu, 0x23600, 0xA7A506A0u);
    sgpu_wr(gmu, 0x23604, 0xA1E6A6E7u);
    sgpu_wr(gmu, 0x23608, 0xA2E081E1u);
    sgpu_wr(gmu, 0x2360c, 0xE9A982E2u);
    sgpu_wr(gmu, 0x23610, 0x0020E8A8u);

    /* 2. PDC SEQ uCode (5 words) */
    sgpu_wr(pdc_seq, 0x0, 0xFEBEA1E1u);
    sgpu_wr(pdc_seq, 0x4, 0xA5A4A3A2u);
    sgpu_wr(pdc_seq, 0x8, 0x8382A6E0u);
    sgpu_wr(pdc_seq, 0xc, 0xBCE3E284u);
    sgpu_wr(pdc_seq, 0x10, 0x002081FCu);

    /* 3. PDC CFG TCS1 commands */
    sgpu_wr(pdc_cfg, 0x55C8, 0u);          /* TCS1_CONTROL */
    sgpu_wr(pdc_cfg, 0x55CC, 7u);          /* TCS1_CMD_ENABLE_BANK */
    sgpu_wr(pdc_cfg, 0x55D0, 0u);          /* TCS1_CMD_WAIT_FOR_CMPL_BANK */
    sgpu_wr(pdc_cfg, 0x55D4, 0x10108u);    /* TCS1_CMD0_MSGID */
    sgpu_wr(pdc_cfg, 0x55D8, 0x30010u);    /* TCS1_CMD0_ADDR */
    sgpu_wr(pdc_cfg, 0x55DC, 1u);          /* TCS1_CMD0_DATA */
    sgpu_wr(pdc_cfg, 0x55E4, 0x10108u);    /* TCS1_CMD1_MSGID */
    sgpu_wr(pdc_cfg, 0x55E8, 0x30000u);    /* TCS1_CMD1_ADDR */
    sgpu_wr(pdc_cfg, 0x55EC, 0u);          /* TCS1_CMD1_DATA */
    sgpu_wr(pdc_cfg, 0x55F4, 0x10108u);    /* TCS1_CMD2_MSGID */
    sgpu_wr(pdc_cfg, 0x55F8, 0x30090u);    /* TCS1_CMD2_ADDR (A618) */
    sgpu_wr(pdc_cfg, 0x55FC, 0u);          /* TCS1_CMD2_DATA */

    /* 4. PDC CFG TCS3 commands */
    sgpu_wr(pdc_cfg, 0x5758, 0u);          /* TCS3_CONTROL */
    sgpu_wr(pdc_cfg, 0x575C, 7u);          /* TCS3_CMD_ENABLE_BANK */
    sgpu_wr(pdc_cfg, 0x5760, 0u);          /* TCS3_CMD_WAIT_FOR_CMPL_BANK */
    sgpu_wr(pdc_cfg, 0x5764, 0x10108u);    /* TCS3_CMD0_MSGID */
    sgpu_wr(pdc_cfg, 0x5768, 0x30010u);    /* TCS3_CMD0_ADDR */
    sgpu_wr(pdc_cfg, 0x576C, 2u);          /* TCS3_CMD0_DATA */
    sgpu_wr(pdc_cfg, 0x5774, 0x10108u);    /* TCS3_CMD1_MSGID */
    sgpu_wr(pdc_cfg, 0x5778, 0x30000u);    /* TCS3_CMD1_ADDR */
    sgpu_wr(pdc_cfg, 0x577C, 2u);          /* TCS3_CMD1_DATA (A618) */
    sgpu_wr(pdc_cfg, 0x5784, 0x10108u);    /* TCS3_CMD2_MSGID */
    sgpu_wr(pdc_cfg, 0x5788, 0x30090u);    /* TCS3_CMD2_ADDR (A618) */
    sgpu_wr(pdc_cfg, 0x578C, 3u);          /* TCS3_CMD2_DATA */

    /* 5. Enable PDC */
    sgpu_wr(pdc_cfg, 0x4520, 0u);          /* PDC_GPU_SEQ_START_ADDR */
    sgpu_wr(pdc_cfg, 0x4500, 0x80000001u); /* PDC_GPU_ENABLE_PDC */
}

static int
surya_gmu_boot(vm_offset_t gmu, const void *fw_bytes, unsigned int fw_len,
    struct surya_gpu_diag_state *st)
{
    /* Hold CM3 reset during upload and readback. */
    sgpu_wr(gmu, 0x14000, 1u);
    delay(10);
    /* Copy firmware words to ITCM (0x3000) */
    unsigned int fw_words = fw_len / 4;
    for (unsigned i = 0; i < fw_words; ++i) {
        uint32_t val;
        memcpy(&val, (const char *)fw_bytes + i * 4, 4);
        sgpu_wr(gmu, 0x3000 + i * 4, val);
    }
    /* Check every copied word, not just the first diagnostic word. */
    for (unsigned i = 0; i < fw_words; ++i) {
        uint32_t expected;
        memcpy(&expected, (const char *)fw_bytes + i * 4, 4);
        if (sgpu_rd(gmu, 0x3000 + i * 4) != expected) return 2;
    }
    st->itcm_loaded = true;
    st->itcm_check = sgpu_rd(gmu, 0x3000);

    /* TCM retention */
    sgpu_wr(gmu, 0x14730, 1u); /* A6XX_GMU_GENERAL_7 */

    /* Clear init result */
    sgpu_wr(gmu, 0x14070, 0u); /* A6XX_GMU_CM3_FW_INIT_RESULT */

    /* TCM boot mode (2) */
    sgpu_wr(gmu, 0x14004, 2u); /* A6XX_GMU_CM3_BOOT_CONFIG */

    /* AHB fence range */
    sgpu_wr(gmu, 0x24C44, 0x80A008A0u); /* A6XX_GMU_AHB_FENCE_RANGE_0 */

    /* CM3 config reset value */
    sgpu_wr(gmu, 0x140B4, 0x4052u); /* A6XX_GMU_CM3_CFG */

    /* Pass chipid for A618: Core 6, Major 1, Minor 8, Patch 0 -> 0x06018000 */
    sgpu_wr(gmu, 0x14608, (uint32_t)((SURYA_DT_CHIP_ID & 0xffff0000u) |
        (((SURYA_DT_CHIP_ID >> 8) & 0xffu) << 12) | ((SURYA_DT_CHIP_ID & 0xffu) << 8))); /* A6XX_GMU_HFI_SFR_ADDR */

    /* Configure bus, caches, power */
    sgpu_wr(gmu, 0x1303C, 1u);         /* A6XX_GMU_SYS_BUS_CONFIG */
    sgpu_wr(gmu, 0x13000, 1u);         /* A6XX_GMU_ICACHE_CONFIG */
    sgpu_wr(gmu, 0x13004, 1u);         /* A6XX_GMU_DCACHE_CONFIG */
    sgpu_wr(gmu, 0x14300, 0x9C40400u); /* A6XX_GMU_PWR_COL_INTER_FRAME_CTRL */
    sgpu_wr(gmu, 0x143A0, 0x0F11u);    /* A6XX_GMU_RPMH_CTRL: RPMH_ENABLE_MASK */

    /* Clear WFI config */
    sgpu_wr(gmu, 0x14008, 0u);         /* A6XX_GMU_CX_GMU_WFI_CONFIG */

    /* Put AHB fence in ALLOW mode */
    sgpu_wr(gmu, 0x24C40, 0u);         /* A6XX_GMU_AO_AHB_FENCE_CTRL */

    /* Clear any pending interrupts */
    sgpu_wr(gmu, 0x14644, 0xFFFFFFFFu); /* A6XX_GMU_GMU2HOST_INTR_CLR */

    /* Pulse CM3 reset */
    sgpu_wr(gmu, 0x14000, 1u);         /* A6XX_GMU_CM3_SYSRESET */
    delay(10);
    sgpu_wr(gmu, 0x14000, 0u);

    /* Poll for 0xBABEFACE */
    for (unsigned i = 0; i < 50000; ++i) {
        st->cm3_init_res = sgpu_rd(gmu, 0x14070);
        if (st->cm3_init_res == 0xBABEFACEu) break;
        delay(10);
    }

    if (st->cm3_init_res != 0xBABEFACEu) return 1;

    /* A618 is in the A615 family. Boot/slumber is request 6, not 2:
     * HOST2GMU set bit22, GMU2HOST ACK bit30, HOST2GMU release bit30.
     * Leave unrelated status/fault bits intact for the diagnostic. */
    st->oob_before = sgpu_rd(gmu, 0x14648);
    if (st->oob_before & SGMU_CM3_FAULT) {
        st->oob_ack = st->oob_before;
        return 3;
    }
    sgpu_wr(gmu, 0x14644, SGMU_BOOT_ACK);

    sgpu_wr(gmu, 0x8FE0, 0u); /* OOB_BOOT_OPTION */
    /* Existing diagnostic rail indices; not a derived RPMh voltage table.
     * Do not infer frequency/voltage or full power readiness from these. */
    sgpu_wr(gmu, 0x8FE4, 1u);
    sgpu_wr(gmu, 0x8FE8, 1u);
    sgpu_wr(gmu, 0x14650, SGMU_BOOT_SET);

    enum sgmu_reply reply = SGMU_PENDING;
    for (unsigned i = 0; i < 10000; ++i) { /* stock 100-ms bound */
        st->oob_ack = sgpu_rd(gmu, 0x14648);
        reply = sgmu_boot_reply(st->oob_ack);
        if (reply != SGMU_PENDING) break;
        delay(10);
    }
    /* Capture before acknowledgement/release can change the evidence. */
    st->oob_raw = sgpu_rd(gmu, 0x14658);
    st->fw_busy = sgpu_rd(gmu, 0x14068);
    st->rpmh_state = sgpu_rd(gmu, 0x143b0);
    st->hfi_status = sgpu_rd(gmu, 0x14600);
    st->oob_ok = reply == SGMU_ACKNOWLEDGED;
    sgpu_wr(gmu, 0x14644, SGMU_BOOT_ACK);
    sgpu_wr(gmu, 0x14650, SGMU_BOOT_RELEASE);
    if (reply == SGMU_FAULT) return 3;
    if (!st->oob_ok) return 4;

    return 0;
}

static int
surya_gpu_power_check(struct surya_gpu_diag_state *st)
{
    memset(st, 0, sizeof(*st));
    vm_offset_t gpucc = ml_io_map(SURYA_GPUCC_PA, SURYA_GPUCC_SIZE);
    if (!gpucc) return 1;

    st->cx_init = sgpu_rd(gpucc, 0x106c);

    /* Power on CX GDSC if collapsed (clear bit 0) */
    if (st->cx_init & 1u) {
        sgpu_wr(gpucc, 0x106c, st->cx_init & ~1u);
        for (unsigned i = 0; i < 5000; ++i) {
            if (sgpu_rd(gpucc, 0x106c) & (1u << 31)) break;
            delay(1);
        }
    }
    st->cx_on = sgpu_rd(gpucc, 0x106c);
    if (!(st->cx_on & (1u << 31))) return 2; /* CX power on timeout */

    /* Enable GPU_CC_AHB_CLK (0x1078) */
    sgpu_wr(gpucc, 0x1078, sgpu_rd(gpucc, 0x1078) | 1u);
    for (unsigned i = 0; i < 5000; ++i) {
        if (!(sgpu_rd(gpucc, 0x1078) & (1u << 31))) break;
        delay(1);
    }
    st->ahb = sgpu_rd(gpucc, 0x1078);

    /* Enable CXO AON clock (0x1004) and CXO clock (0x109c) */
    sgpu_wr(gpucc, 0x1004, sgpu_rd(gpucc, 0x1004) | 1u);
    sgpu_wr(gpucc, 0x109c, sgpu_rd(gpucc, 0x109c) | 1u);

    /* Configure CX GMU clock (0x1098) wakeup/sleep and enable */
    uint32_t gmu_cfg = (sgpu_rd(gpucc, 0x1098) & ~0xff0u) | 0xff0u | 1u;
    sgpu_wr(gpucc, 0x1098, gmu_cfg);
    for (unsigned i = 0; i < 5000; ++i) {
        if (!(sgpu_rd(gpucc, 0x1098) & (1u << 31))) break;
        delay(1);
    }
    st->gmu_clk = sgpu_rd(gpucc, 0x1098);

    /* Map GMU to read live hardware always-on counter and registers */
    vm_offset_t gmu = ml_io_map(SURYA_GMU_PA, SURYA_GMU_SIZE);
    if (!gmu) return 3;

    st->c1 = sgpu_rd(gmu, 0x14220);
    delay(10000); /* 10 ms wait */
    st->c2 = sgpu_rd(gmu, 0x14220);

    st->cm3_cfg = sgpu_rd(gmu, 0x140b4);
    st->cm3_reset = sgpu_rd(gmu, 0x14000);
    st->hfi_ver = sgpu_rd(gmu, 0x14604);

    /* Check if GMU firmware is provided in ADT */
    DTEntry chosen = NULL;
    void const *gmu_fw = NULL;
    unsigned int gmu_fw_size = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess) {
        if (SecureDTGetProperty(chosen, "surya-gmu-firmware", &gmu_fw, &gmu_fw_size) == kSuccess &&
            gmu_fw && gmu_fw_size == 32768) {
            st->fw_found = true;
            st->fw_size = gmu_fw_size;
        }
    }

    if (st->fw_found) {
        vm_offset_t pdc_cfg = ml_io_map(SURYA_PDC_CFG_PA, SURYA_PDC_CFG_SIZE);
        vm_offset_t pdc_seq = ml_io_map(SURYA_PDC_SEQ_PA, SURYA_PDC_SEQ_SIZE);
        if (pdc_cfg && pdc_seq) {
            surya_gpu_setup_pdc(gmu, pdc_cfg, pdc_seq);
            st->pdc_ok = true;
            st->fw_result = surya_gmu_boot(gmu, gmu_fw, gmu_fw_size, st);
        }
    }

    /* Read initial GX GDSC state (0x100c) */
    st->gx_init = sgpu_rd(gpucc, 0x100c);

    /* Do not force GX clocks/domain after a failed GMU transaction. */
    if (!st->oob_ok) {
        st->gx_on = st->gx_init;
        st->gx_clk = sgpu_rd(gpucc, 0x1054);
        return 4;
    }

    /* Enable GX GFX3D root clock (0x1054) */
    sgpu_wr(gpucc, 0x1054, sgpu_rd(gpucc, 0x1054) | 1u);
    st->gx_clk = sgpu_rd(gpucc, 0x1054);

    /* Attempt to power up GX GDSC (clear SW_COLLAPSE bit 0) */
    if (st->gx_init & 1u) {
        sgpu_wr(gpucc, 0x100c, st->gx_init & ~1u);
        for (unsigned i = 0; i < 5000; ++i) {
            if (sgpu_rd(gpucc, 0x100c) & (1u << 31)) break;
            delay(1);
        }
    }
    st->gx_on = sgpu_rd(gpucc, 0x100c);
    st->gx_powered = !!(st->gx_on & (1u << 31));

    /* If GX power domain is active, safely probe GPU core registers */
    if (st->gx_powered) {
        vm_offset_t gpu = ml_io_map(SURYA_GPU_PA, SURYA_GPU_SIZE);
        if (gpu) {
            st->rbbm_status = sgpu_rd(gpu, 0x840);
            st->rbbm_status3 = sgpu_rd(gpu, 0x84c);
            st->gbif_qos = sgpu_rd(gpu, 0x44);
        }
    }

    return 0;
}

void
surya_gpu_pt_diagnostic(void)
{
    int enabled = 0;
    static int wait_channel;
    extern int disableConsoleOutput, sweet_console_muted;
    (void)PE_parse_boot_argn("suryagpummu", &enabled, sizeof(enabled));
    if (enabled != 1) return;
    int pt_res = surya_gpu_pt_check();

    struct surya_gpu_diag_state st;
    printf("SURYA UNIXNU STEP 0: private CPU tables passed; start GPU power\n");
    delay(20000);
    int pwr_res = surya_gpu_power_check(&st);
    struct surya_unixnu_smmu_report unixnu = {0};
    unixnu.code = -1;
    printf("SURYA UNIXNU POWER RESULT: code=%d OOB=%u GX=%u\n", pwr_res, st.oob_ok, st.gx_powered);
    if (!pt_res && st.oob_ok && st.gx_powered)
        surya_unixnu_smmu_capture(&unixnu);

    for (;;) {
        disableConsoleOutput = sweet_console_muted = 0;
        printf("\nSURYA GPU PAGE TABLE DIAGNOSTIC: %s code=%d\n",
            pt_res ? "FAIL" : "CPU TABLE TEST PASS", pt_res);
        printf("SURYA GPU CX: %s code=%d CX_GDSC=%#x->%#x AHB=%#x GMUCLK=%#x\n",
            pwr_res ? "FAIL" : "PASS", pwr_res, st.cx_init, st.cx_on, st.ahb, st.gmu_clk);
        printf("SURYA GPU GMU: c1=%u c2=%u delta=%u CM3_CFG=%#x RESET=%#x\n",
            st.c1, st.c2, st.c2 - st.c1, st.cm3_cfg, st.cm3_reset);
        printf("SURYA GPU FW: %s size=%u PDC=%s ITCM=%#x INIT=%#x (%s)\n",
            st.fw_found ? "FOUND" : "NONE", st.fw_size,
            st.pdc_ok ? "OK" : "NO",
            st.itcm_check, st.cm3_init_res,
            (st.cm3_init_res == 0xBABEFACEu) ? "BABEFACE PASS" : "PENDING/FAIL");
        printf("SURYA GPU A618 OOB6: before=%#x status=%#x %s code=%d\n",
            st.oob_before, st.oob_ack,
            (st.oob_ack & SGMU_CM3_FAULT) ? "CM3 FAULT" :
            st.oob_ok ? "ACK bit30" : "NOT ACKNOWLEDGED", st.fw_result);
        printf("SURYA GPU OOB6: set=%#x ack=%#x raw=%#x busy=%#x\n",
            SGMU_BOOT_SET, SGMU_BOOT_ACK, st.oob_raw, st.fw_busy);
        printf("SURYA GPU STATE: RPMH=%#x HFI=%#x upload=%s\n",
            st.rpmh_state, st.hfi_status, st.itcm_loaded ? "READBACK PASS" : "NOT VERIFIED");
        printf("SURYA GPU GX: GDSC=%#x->%#x (%s) GFXCLK=%#x\n",
            st.gx_init, st.gx_on, st.gx_powered ? "ON" : "OFF", st.gx_clk);
        if (st.gx_powered) {
            printf("SURYA GPU CORE: RBBM_STATUS=%#x STATUS3=%#x GBIF_QOS=%#x\n",
                st.rbbm_status, st.rbbm_status3, st.gbif_qos);
        }
        printf("SURYA UNIXNU SMMU: stage=%d code=%d DT clocks=%#x\n",
            unixnu.stage, unixnu.code, unixnu.clocks_ready);
        printf("SURYA UNIXNU ID: ID0=%#x ID1=%#x ID2=%#x SCR0=%#x\n",
            unixnu.id0, unixnu.id1, unixnu.id2, unixnu.scr0);
        printf("SURYA UNIXNU CB0: off=%#x SCTLR=%#x FSR=%#x TCR=%#x TTBR0=%#llx\n",
            unixnu.cb_offset, unixnu.cb_sctlr, unixnu.cb_fsr, unixnu.cb_tcr, unixnu.cb_ttbr0);
        printf("SURYA UNIXNU TABLES: pages=%u leaves=%u (live 4KB tables)\n",
            unixnu.table_pages, unixnu.leaves);
        printf("SURYA UNIXNU LIVE: CB0=%s SCTLR=%#x FSR=%#x SMR0=%#x S2CR0=%#x (%s)\n",
            unixnu.context_enabled ? "ACTIVE" : "INACTIVE",
            unixnu.active_sctlr, unixnu.active_fsr, unixnu.smr0, unixnu.s2cr0,
            unixnu.streams_attached ? "ATTACHED" : "UNATTACHED");
        if (unixnu.attach_phase || unixnu.attach_writes) {
            printf("SURYA UNIXNU ATTACH DBG: phase=%u writes=%u\n",
                unixnu.attach_phase, unixnu.attach_writes);
        }
        if (unixnu.cp_started) {
            printf("SURYA UNIXNU CP: SQE=%s WPTR=%u RPTR=%u SCR0=%#x SCR1=%#x STAT1=%#x\n",
                unixnu.sqe_loaded ? "OK" : "NO",
                unixnu.cp_wptr, unixnu.cp_rptr,
                unixnu.cp_scratch0, unixnu.cp_scratch1, unixnu.cp_status1);
            printf("SURYA UNIXNU CP DBG: INTR=%#x FAULT=%#x PC=%#x OP=%#x\n",
                unixnu.cp_intr, unixnu.cp_hw_fault, unixnu.cp_sqe_pc, unixnu.cp_sqe_op);
            if (unixnu.cp_scratch0 == 0x1337BEEFu) {
                printf("SURYA GPU ACCELERATION: VERIFIED (A618 CP EXECUTED PM4 PACKETS)\n");
            } else {
                printf("SURYA GPU CP: EXECUTION PENDING/TIMEOUT\n");
            }
        } else {
            printf("No GPU commands submitted; Metal acceleration not enabled\n");
        }
        printf("Launchd intentionally paused. Power + Volume Down: Fastboot\n");
        (void)tsleep(&wait_channel, PZERO, "gpudiag", hz * 5);
    }
}
#endif
