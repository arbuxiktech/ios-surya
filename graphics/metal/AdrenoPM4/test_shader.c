/* SPDX-License-Identifier: MIT */
#include "AdrenoShader.h"
#include "AdrenoPM4.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef MESA_REFERENCE
#include "util/u_math.h"
#include "adreno_common.xml.h"
#include "adreno_pm4.xml.h"
#include "a6xx_enums.xml.h"
#include "a7xx_enums.xml.h"
#include "a8xx_enums.xml.h"
#include "a6xx.xml.h"
#include "freedreno_pm4.h"
#endif
static uint32_t out[128], before[128];
static unsigned positives, refusals;
static struct AdrenoShaderState state = {ADRENO_A740_CHIP_ID, AdrenoConstantVertex, 128, 1,
    0, -1, 0, 1, 0, 0, 0, 0, 0, 0, 0};
static struct AdrenoShaderMapping mapping = {1, UINT64_C(0x100000000), 4096, 0, 12, 1};
static void reset(void) { memset(out, 0x6b, sizeof(out)); memcpy(before, out, sizeof(out)); }
static void positive(void)
{
    reset();
    struct AdrenoShaderResult r = AdrenoA740ShaderBind(&state, &mapping, out, 128);
    assert(!r.status && r.words == 24 && r.registers == 13);
    AdrenoPM4Packet packets[12];size_t count=0;
    assert(!AdrenoPM4Plan(out, r.words, packets, 12, &count) && count == 11);
    uint32_t regs[13], values[13];unsigned n=0;
    for (size_t i=0;i<count;i++) {
        AdrenoPM4Packet *p=&packets[i];
        assert(p->type==4 && p->payload_words>=1 && p->payload_words<=2);
#ifdef MESA_REFERENCE
        assert(out[p->header_word]==pm4_pkt4_hdr(p->identifier,p->payload_words));
#endif
        for (unsigned j=0;j<p->payload_words;j++) {regs[n]=p->identifier+j;values[n++]=out[p->header_word+1+j];}
    }
    assert(n==13);
    assert(regs[0]==0xab00 && values[0]==5 && regs[1]==0xb309 && values[1]==0xa2);
    unsigned fragment=state.stage==AdrenoConstantFragment;
    const uint32_t vs[]={0xa800,0xa824,0xa81b,0xa81c,0xa81d,0xa81e,0xa81f,0xa820,0xa821,0xa825,0xa82d};
    const uint32_t fs[]={0xa980,0xab05,0xa982,0xa983,0xa984,0xa985,0xa986,0xa987,0xa988,0xa9a9,0xaa01};
    assert(!memcmp(regs+2,fragment?fs:vs,sizeof(vs)));
    uint32_t control;
#ifdef MESA_REFERENCE
    assert(values[0]==(A6XX_SP_MODE_CNTL_CONSTANT_DEMOTION_ENABLE | A6XX_SP_MODE_CNTL_ISAMMODE(ISAMMODE_GL)));
    assert(values[1]==(A6XX_TPL1_MODE_CNTL_ISAMMODE(ISAMMODE_GL) |
        A6XX_TPL1_MODE_CNTL_TEXCOORDROUNDMODE(COORD_TRUNCATE) |
        A6XX_TPL1_MODE_CNTL_ARRAYCOORDROUNDMODE(COORD_TRUNCATE) |
        A6XX_TPL1_MODE_CNTL_NEARESTMIPSNAP(CLAMP_ROUND_TRUNCATE) | A6XX_TPL1_MODE_CNTL_DESTDATATYPEOVERRIDE));
    if (fragment) {
        assert(regs[2]==REG_A6XX_SP_PS_CNTL_0 && regs[3]==REG_A6XX_SP_PS_INSTR_SIZE &&
               regs[5]==REG_A6XX_SP_PS_BASE && regs[12]==REG_A7XX_SP_PS_VGS_CNTL);
        control=A6XX_SP_PS_CNTL_0_FULLREGFOOTPRINT(state.max_full_register+1) |
            A6XX_SP_PS_CNTL_0_BRANCHSTACK(state.branch_stack_hw) |
            A6XX_SP_PS_CNTL_0_THREADSIZE(state.double_threadsize?THREAD128:THREAD64) |
            A6XX_SP_PS_CNTL_0_INOUTREGOVERLAP;
        if (state.input_words) control|=A6XX_SP_PS_CNTL_0_VARYING;
        if (state.need_full_quad) control|=A6XX_SP_PS_CNTL_0_LODPIXMASK;
        if (state.need_pixel_lod) control|=A6XX_SP_PS_CNTL_0_PIXLODENABLE;
        if (state.merged_registers) control|=A6XX_SP_PS_CNTL_0_MERGEDREGS;
        if (state.early_preamble) control|=A6XX_SP_PS_CNTL_0_EARLYPREAMBLE;
    } else {
        assert(regs[2]==REG_A6XX_SP_VS_CNTL_0 && regs[3]==REG_A6XX_SP_VS_INSTR_SIZE &&
               regs[5]==REG_A6XX_SP_VS_BASE && regs[12]==REG_A7XX_SP_VS_VGS_CNTL);
        control=A6XX_SP_VS_CNTL_0_FULLREGFOOTPRINT(state.max_full_register+1) |
            A6XX_SP_VS_CNTL_0_BRANCHSTACK(state.branch_stack_hw);
        if (state.merged_registers) control|=A6XX_SP_VS_CNTL_0_MERGEDREGS;
        if (state.early_preamble) control|=A6XX_SP_VS_CNTL_0_EARLYPREAMBLE;
    }
#else
    control=(uint32_t)(state.max_full_register+1)<<7 | state.branch_stack_hw<<13;
    if (fragment) control|=state.double_threadsize<<20 | (state.input_words!=0)<<22 | state.need_full_quad<<23 |
        1u<<24 | state.need_pixel_lod<<26 | state.early_preamble<<28 | state.merged_registers<<31;
    else control|=state.merged_registers<<20 | state.early_preamble<<21;
#endif
    assert(values[2]==control && values[3]==state.instruction_groups && !values[4]);
    uint64_t address=mapping.base+mapping.shader_offset;
    assert(values[5]==(uint32_t)address && values[6]==address>>32);
    for (unsigned i=7;i<13;i++) assert(!values[i]);
    assert(!memcmp(out+24,before+24,sizeof(out)-24*4));
    ++positives;
}
static void rejected(const struct AdrenoShaderState *s,const struct AdrenoShaderMapping *m,
                     uint32_t *destination,size_t capacity,unsigned expected)
{
    memcpy(before,out,sizeof(out));
    struct AdrenoShaderResult r=AdrenoA740ShaderBind(s,m,destination,capacity);
    assert(r.status==expected && !r.words && !r.registers && !memcmp(out,before,sizeof(out)));
    ++refusals;
}
int main(void)
{
    for (unsigned profile=0;profile<2;profile++) {
        state.chip_id=profile?ADRENO_A740_FALLBACK_CHIP_ID:ADRENO_A740_CHIP_ID;
        for (unsigned stage=0;stage<2;stage++) {
            state.stage=stage?AdrenoConstantFragment:AdrenoConstantVertex;
            state.merged_registers=1;state.early_preamble=0;state.double_threadsize=0;
            state.input_words=0;state.need_full_quad=0;state.need_pixel_lod=0;
            for (int full=-1;full<63;full++) for (unsigned stack=0;stack<=126;stack+=2) {
                state.max_full_register=full;state.branch_stack_hw=stack;positive();
            }
            state.max_full_register=0;state.branch_stack_hw=0;
            for (unsigned flags=0;flags<64;flags++) {
                state.merged_registers=flags&1;state.early_preamble=flags>>1&1;state.double_threadsize=flags>>2&1;
                state.input_words=(flags>>3&1)?128:0;
                state.need_full_quad=stage?(flags>>4&1):0;state.need_pixel_lod=stage?(flags>>5&1):0;
                positive();
            }
        }
    }
    state=(struct AdrenoShaderState){ADRENO_A740_CHIP_ID,AdrenoConstantVertex,128,1,0,-1,0,1,0,0,0,0,0,0,0};
    state.binary_bytes=384;state.instruction_groups=2;positive();
    state.binary_bytes=128;state.instruction_groups=1;
    mapping.shader_offset=3968;positive();
    mapping.shader_offset=0;mapping.base=(UINT64_C(1)<<48)-4096;positive();
    mapping.base=UINT64_C(0x100000000);mapping.bytes=1024*1024;
    state.binary_bytes=1024*1024;state.instruction_groups=8192;positive();
    state.binary_bytes=128;state.instruction_groups=1;mapping.bytes=4096;
    reset();
    rejected(NULL,&mapping,out,128,AdrenoShaderArgument);
    rejected(&state,NULL,out,128,AdrenoShaderArgument);
    rejected(&state,&mapping,NULL,128,AdrenoShaderArgument);
    rejected(&state,&mapping,(void *)((uint8_t *)out+1),127,AdrenoShaderArgument);
    rejected(&state,&mapping,out,SIZE_MAX,AdrenoShaderArgument);
    rejected((void *)(UINTPTR_MAX-3),&mapping,out,128,AdrenoShaderArgument);
    rejected(&state,(void *)(UINTPTR_MAX-3),out,128,AdrenoShaderArgument);
    rejected(&state,&mapping,(void *)(UINTPTR_MAX-3),24,AdrenoShaderArgument);
    rejected(&state,&mapping,out,23,AdrenoShaderCapacity);
    memcpy(out,&state,sizeof(state));rejected((void *)out,&mapping,out,128,AdrenoShaderAlias);
    memcpy(out,&mapping,sizeof(mapping));rejected(&state,(void *)out,out,128,AdrenoShaderAlias);
    for (unsigned i=0;i<19;i++) {
        struct AdrenoShaderState bad=state;
        switch(i) {
        case 0:bad.chip_id^=UINT64_C(1)<<63;break;
        case 1:bad.stage=AdrenoConstantCompute;break;
        case 2:bad.binary_bytes=0;break;
        case 3:bad.binary_bytes=127;break;
        case 4:bad.binary_bytes=1024*1024+128;break;
        case 5:bad.instruction_groups=0;break;
        case 6:bad.max_full_register=63;break;
        case 7:bad.max_full_register=-2;break;
        case 8:bad.max_half_register=0;break;
        case 9:bad.branch_stack_hw=1;break;
        case 10:bad.branch_stack_hw=128;break;
        case 11:bad.merged_registers=2;break;
        case 12:bad.early_preamble=2;break;
        case 13:bad.double_threadsize=2;break;
        case 14:bad.input_words=129;break;
        case 15:bad.need_full_quad=1;break;
        case 16:bad.private_bytes=16;break;
        case 17:bad.constant_data_bytes=16;break;
        default:bad.instruction_groups=2;break;
        }
        rejected(&bad,&mapping,out,128,AdrenoShaderProfile);
    }
    for (unsigned i=0;i<12;i++) {
        struct AdrenoShaderMapping bad=mapping;
        switch(i) {
        case 0:bad.context=0;break;
        case 1:bad.base=0;break;
        case 2:bad.base=UINT64_C(1)<<48;break;
        case 3:bad.bytes=0;break;
        case 4:bad.bytes=UINT64_MAX;break;
        case 5:bad.base++;break;
        case 6:bad.bytes--;break;
        case 7:bad.page_shift=14;break;
        case 8:bad.readonly=0;break;
        case 9:bad.shader_offset=1;break;
        case 10:bad.shader_offset=8192;break;
        default:bad.shader_offset=4096;break;
        }
        rejected(&state,&bad,out,128,AdrenoShaderMappingError);
    }
    printf("PASS: %u shader register cases, both full identities and full-register/branch fields; %u transactional refusals, native-4K view required\n",positives,refusals);
    return 0;
}
