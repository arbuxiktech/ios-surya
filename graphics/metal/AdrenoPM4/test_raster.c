/* SPDX-License-Identifier: MIT */
#include "AdrenoRaster.h"
#include "AdrenoPM4.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef MESA_REFERENCE
size_t AdrenoRasterReference(const struct AdrenoRasterState *,uint32_t *,uint32_t *,size_t);
#endif
static unsigned positives,refusals;
static uint32_t out[80],saved[80];
static struct AdrenoRasterState state;
static void fixture(void) {
    state=(struct AdrenoRasterState){ADRENO_A740_CHIP_ID,
        AdrenoRasterClipNear|AdrenoRasterClipFar|AdrenoRasterHalfZ|AdrenoRasterFrontCCW,
        3,16,2,0,0,0,0};
}
static void reset(void) { memset(out,0x6b,sizeof out);memcpy(saved,out,sizeof out); }
static void positive_capacity(size_t capacity) {
    reset();struct AdrenoRasterResult r=AdrenoA740Raster(&state,out,capacity);
    int clamp=(state.flags&3)!=3;
    assert(!r.status && r.words==(clamp?24u:60u) && r.registers==(clamp?12u:46u));
    assert(!memcmp(out+r.words,saved+r.words,sizeof out-r.words*4));
    AdrenoPM4Packet packets[16];size_t count=0;
    assert(!AdrenoPM4Plan(out,r.words,packets,16,&count) && count==(clamp?12u:14u));
#ifdef MESA_REFERENCE
    uint32_t regs[64],values[64];size_t nr=AdrenoRasterReference(&state,regs,values,64),at=0;
    assert(nr==r.registers);
    for(size_t i=0;i<count;i++) {
        assert(packets[i].type==4);
        for(unsigned j=0;j<packets[i].payload_words;j++,at++)
            assert(at<nr && regs[at]==packets[i].identifier+j &&
                values[at]==out[packets[i].header_word+1+j]);
    }
    assert(at==nr);
#endif
    positives++;
}
static void positive(void) { positive_capacity(80); }
static void refusal(uint32_t status,size_t capacity) {
    reset();struct AdrenoRasterResult r=AdrenoA740Raster(&state,out,capacity);
    assert(r.status==status && !r.words && !r.registers && !memcmp(out,saved,sizeof out));refusals++;
}
int main(void) {
    /* Every supported flag combination and polygon mode, for both full IDs. */
    for(unsigned chip=0;chip<2;chip++)for(unsigned flags=0;flags<=ADRENO_RASTER_FLAGS;flags++)for(unsigned mode=1;mode<=3;mode++) {
        fixture();state.chip_id=chip?ADRENO_A740_FALLBACK_CHIP_ID:ADRENO_A740_CHIP_ID;
        state.flags=flags;state.polygon_mode=mode;positive();
    }
    /* Every fixed-point point/line field value, avoiding float conversions. */
    for(unsigned i=0;i<=65472;i++) {fixture();state.point_size_q4=i;positive();}
    for(unsigned i=0;i<=255;i++) {fixture();state.line_width_q1=i;positive();}
    static const uint32_t finite[]={0,0x80000000,1,0x80000001,0x007fffff,0x00800000,
        0x3f000000,0x3f800000,0xbf800000,0x7f7fffff,0xff7fffff};
    for(unsigned i=0;i<sizeof finite/sizeof finite[0];i++)for(unsigned j=0;j<3;j++) {
        fixture();uint32_t *v=j==0?&state.offset_scale_bits:j==1?&state.offset_units_bits:&state.offset_clamp_bits;*v=finite[i];positive();
    }
    fixture();positive_capacity(60);state.flags=0;positive_capacity(24);
#define BAD(change,status) do {fixture();change;refusal(status,80);}while(0)
    BAD(state.chip_id=0x43050a02,AdrenoRasterProfile);
    BAD(state.chip_id=UINT64_C(0x123443050a01),AdrenoRasterProfile);
    BAD(state.flags=8192,AdrenoRasterProfile);BAD(state.reserved=1,AdrenoRasterProfile);
    BAD(state.polygon_mode=0,AdrenoRasterProfile);BAD(state.polygon_mode=4,AdrenoRasterProfile);
    BAD(state.point_size_q4=65473,AdrenoRasterProfile);BAD(state.line_width_q1=256,AdrenoRasterProfile);
    const uint32_t bad[]={0x7f800000,0xff800000,0x7fc00000,0xff800001};
    for(unsigned i=0;i<4;i++)for(unsigned j=0;j<3;j++) {
        fixture();uint32_t *v=j==0?&state.offset_scale_bits:j==1?&state.offset_units_bits:&state.offset_clamp_bits;*v=bad[i];refusal(AdrenoRasterProfile,80);
    }
#undef BAD
    fixture();refusal(AdrenoRasterCapacity,0);refusal(AdrenoRasterCapacity,59);
    state.flags=0;refusal(AdrenoRasterCapacity,23);
    reset();assert(AdrenoA740Raster(0,out,80).status==AdrenoRasterArgument && !memcmp(out,saved,sizeof out));refusals++;
    fixture();reset();assert(AdrenoA740Raster(&state,0,80).status==AdrenoRasterArgument);refusals++;
    assert(AdrenoA740Raster(&state,out,SIZE_MAX).status==AdrenoRasterArgument && !memcmp(out,saved,sizeof out));refusals++;
    assert(AdrenoA740Raster((const void *)(UINTPTR_MAX-7),out,80).status==AdrenoRasterArgument && !memcmp(out,saved,sizeof out));refusals++;
    assert(AdrenoA740Raster((const void *)((uintptr_t)&state+1),out,80).status==AdrenoRasterArgument && !memcmp(out,saved,sizeof out));refusals++;
    assert(AdrenoA740Raster(&state,(void *)((uintptr_t)out+1),80).status==AdrenoRasterArgument && !memcmp(out,saved,sizeof out));refusals++;
    assert(AdrenoA740Raster(&state,(void *)(UINTPTR_MAX-3),2).status==AdrenoRasterArgument && !memcmp(out,saved,sizeof out));refusals++;
    union { struct AdrenoRasterState s;uint32_t words[80]; } alias;
    for(unsigned i=0;i<sizeof(state)/4;i++) {
        memset(&alias,0x6b,sizeof alias);alias.s=state;unsigned char before[sizeof alias];memcpy(before,&alias,sizeof alias);
        assert(AdrenoA740Raster(&alias.s,alias.words+i,60).status==AdrenoRasterAlias && !memcmp(before,&alias,sizeof alias));refusals++;
    }
    _Alignas(struct AdrenoRasterState) uint32_t preceding[96];
    memset(preceding,0x6b,sizeof preceding);memcpy(preceding+8,&state,sizeof state);
    unsigned char before[sizeof preceding];memcpy(before,preceding,sizeof preceding);
    assert(AdrenoA740Raster((const void *)(preceding+8),preceding,60).status==AdrenoRasterAlias && !memcmp(before,preceding,sizeof preceding));refusals++;
    printf("PASS: %u raster profiles, %u transactional refusals\n",positives,refusals);
    return 0;
}
