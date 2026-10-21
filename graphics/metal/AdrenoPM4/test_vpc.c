/* SPDX-License-Identifier: MIT */
#include "AdrenoVPC.h"
#include "AdrenoPM4.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef MESA_REFERENCE
#include "util/u_math.h"
#include "ir3_shader.h"
#include "adreno_common.xml.h"
#include "adreno_pm4.xml.h"
#include "a6xx_enums.xml.h"
#include "a7xx_enums.xml.h"
#include "a8xx_enums.xml.h"
#include "a6xx.xml.h"
#include "freedreno_pm4.h"
#endif
static uint32_t out[128],saved[128];
static unsigned positives,refusals;
static uint8_t names[15][32];
static struct AdrenoVPCOutput outputs[16];
static struct AdrenoVPCInput inputs[15];
static struct AdrenoVPCState state={ADRENO_A740_CHIP_ID,62,0,outputs,2,inputs,1};
static void reset(void) {memset(out,0x6b,sizeof out);memcpy(saved,out,sizeof out);}
static int equal(const uint8_t *a,size_t an,const uint8_t *b,size_t bn) {return an==bn && !memcmp(a,b,an);}
static void fixture(unsigned n) {
    state=(struct AdrenoVPCState){ADRENO_A740_CHIP_ID,62,0,outputs,n+1,inputs,n};
    outputs[0]=(struct AdrenoVPCOutput){(const uint8_t *)"air.position",12,0,4,32,0,0,0};
    for(unsigned i=0;i<n;i++) {
        snprintf((char *)names[i],sizeof names[i],"generated(v%u)",i);
        outputs[i+1]=(struct AdrenoVPCOutput){names[i],strlen((char *)names[i]),4+i*4,4,32,0,0,0};
        inputs[i]=(struct AdrenoVPCInput){names[i],strlen((char *)names[i]),4,32,15,i*4,0};
    }
}
static void positive(void) {
    reset();struct AdrenoVPCResult r=AdrenoA740VaryingMap(&state,out,128);
    assert(!r.status && r.words<=ADRENO_VPC_MAX_WORDS && r.varying_count==state.input_count && r.stride==r.position_location+4);
    assert(!memcmp(out+r.words,saved+r.words,sizeof out-r.words*4));
    AdrenoPM4Packet packets[16];size_t count=0;assert(!AdrenoPM4Plan(out,r.words,packets,16,&count) && count==12);
    uint32_t regs[64],values[64];unsigned nr=0;
    for(size_t i=0;i<count;i++) {
        AdrenoPM4Packet *p=packets+i;assert(p->type==4);
#ifdef MESA_REFERENCE
        assert(out[p->header_word]==pm4_pkt4_hdr(p->identifier,p->payload_words));
#endif
        for(unsigned j=0;j<p->payload_words;j++){regs[nr]=p->identifier+j;values[nr++]=out[p->header_word+1+j];}
    }
    assert(nr==r.registers);
#ifdef MESA_REFERENCE
    static struct ir3_shader_variant vs,fs;
    vs.type=MESA_SHADER_VERTEX;fs.type=MESA_SHADER_FRAGMENT;
    vs.outputs_count=state.output_count;fs.inputs_count=state.input_count;fs.total_in=124;
    for(unsigned i=0;i<state.output_count;i++) {
        vs.outputs[i].slot=equal(outputs[i].identity,outputs[i].identity_bytes,(const uint8_t *)"air.position",12)?VARYING_SLOT_POS:VARYING_SLOT_VAR0+i;
        vs.outputs[i].regid=outputs[i].register_id;
    }
    // Construct the primary helper's inputs canonically by actual inloc.
    unsigned order[15];for(unsigned i=0;i<state.input_count;i++)order[i]=i;
    for(unsigned i=0;i<state.input_count;i++)for(unsigned j=i+1;j<state.input_count;j++)if(inputs[order[j]].input_location<inputs[order[i]].input_location){unsigned t=order[i];order[i]=order[j];order[j]=t;}
    unsigned pos=16;for(unsigned i=0;i<state.output_count;i++)if(vs.outputs[i].slot==VARYING_SLOT_POS)pos=i;assert(pos<16);
    for(unsigned i=0;i<state.input_count;i++) {
        const struct AdrenoVPCInput *input=inputs+order[i];unsigned match=16;
        for(unsigned j=0;j<state.output_count;j++)if(equal(input->identity,input->identity_bytes,outputs[j].identity,outputs[j].identity_bytes))match=j;
        assert(match<16);fs.inputs[i].slot=VARYING_SLOT_VAR0+match;fs.inputs[i].compmask=input->component_mask;fs.inputs[i].inloc=input->input_location;fs.inputs[i].bary=true;
    }
    struct ir3_shader_linkage link={0};ir3_link_shaders(&link,&vs,&fs,true);
    uint32_t disabled[4];for(unsigned i=0;i<4;i++)disabled[i]=~link.varmask[i];
    unsigned position_location=link.max_loc;ir3_link_add(&link,VARYING_SLOT_POS,outputs[pos].register_id,15,link.max_loc);
    assert(link.cnt==state.input_count+1 && link.max_loc==r.stride && position_location==r.position_location);
    uint32_t packed[8]={0},dest[4]={0};
    for(unsigned i=0;i<link.cnt;i++) {
        uint32_t entry=A6XX_SP_VS_OUTPUT_REG_A_REGID(link.var[i].regid)|A6XX_SP_VS_OUTPUT_REG_A_COMPMASK(link.var[i].compmask);
        packed[i/2]|=entry<<((i%2)*16);dest[i/4]|=A6XX_SP_VS_VPC_DEST_REG_OUTLOC0(link.var[i].loc)<<((i%4)*8);
    }
    unsigned consumed=0;
#define EXPECT(reg,value) do {assert(regs[consumed]==(reg) && values[consumed]==(value));consumed++;}while(0)
    for(unsigned i=0;i<4;i++)EXPECT(REG_A6XX_VPC_VARYING_LM_TRANSFER_CNTL(i),disabled[i]);
    for(unsigned i=0;i<(link.cnt+1u)/2;i++)EXPECT(REG_A6XX_SP_VS_OUTPUT_REG(i),packed[i]);
    for(unsigned i=0;i<(link.cnt+3u)/4;i++)EXPECT(REG_A6XX_SP_VS_VPC_DEST_REG(i),dest[i]);
    EXPECT(REG_A6XX_VPC_VS_CNTL,A6XX_VPC_VS_CNTL_STRIDE_IN_VPC(link.max_loc)|A6XX_VPC_VS_CNTL_POSITIONLOC(position_location)|A6XX_VPC_VS_CNTL_PSIZELOC(255));
    uint32_t clip=A6XX_VPC_VS_CLIP_CULL_CNTL_CLIP_DIST_03_LOC(255)|A6XX_VPC_VS_CLIP_CULL_CNTL_CLIP_DIST_47_LOC(255);
    EXPECT(REG_A6XX_VPC_VS_CLIP_CULL_CNTL,clip);EXPECT(REG_A6XX_VPC_VS_CLIP_CULL_CNTL_V2,clip);EXPECT(REG_A6XX_GRAS_CL_VS_CLIP_CULL_DISTANCE,0);
    EXPECT(REG_A6XX_SP_VS_OUTPUT_CNTL,A6XX_SP_VS_OUTPUT_CNTL_OUT(link.cnt));
    uint32_t siv=A6XX_VPC_VS_SIV_CNTL_LAYERLOC(255)|A6XX_VPC_VS_SIV_CNTL_VIEWLOC(255)|A6XX_VPC_VS_SIV_CNTL_SHADINGRATELOC(255);
    EXPECT(REG_A6XX_VPC_VS_SIV_CNTL,siv);EXPECT(REG_A6XX_VPC_VS_SIV_CNTL_V2,siv);EXPECT(REG_A6XX_GRAS_SU_VS_SIV_CNTL,0);
    for(unsigned i=0;i<8;i++)EXPECT(REG_A6XX_VPC_VARYING_INTERP_MODE(i),0);
    for(unsigned i=0;i<8;i++)EXPECT(REG_A6XX_VPC_VARYING_REPLACE_MODE(i),0);
    assert(consumed==nr);
#undef EXPECT
#else
    (void)regs;(void)values;
#endif
    positives++;
}
static void refusal(uint32_t expected,size_t capacity) {
    reset();struct AdrenoVPCResult r=AdrenoA740VaryingMap(&state,out,capacity);
    assert(r.status==expected && !r.words && !r.registers && !r.varying_count && !r.stride && !r.position_location && !memcmp(out,saved,sizeof out));refusals++;
}
int main(void) {
    for(unsigned chip=0;chip<2;chip++)for(unsigned reg=0;reg<=248;reg++)for(unsigned components=1;components<=4;components++)for(unsigned mask=1;mask<(1u<<components);mask++) {
        fixture(1);state.chip_id=chip?ADRENO_A740_FALLBACK_CHIP_ID:ADRENO_A740_CHIP_ID;outputs[0].register_id=reg;
        outputs[1].components=inputs[0].components=components;inputs[0].component_mask=mask;positive();
    }
    for(unsigned chip=0;chip<2;chip++)for(unsigned loc=0;loc<=120;loc++)for(unsigned mask=1;mask<16;mask++) {fixture(1);state.chip_id=chip?ADRENO_A740_FALLBACK_CHIP_ID:ADRENO_A740_CHIP_ID;inputs[0].input_location=loc;inputs[0].component_mask=mask;positive();}
    for(unsigned count=0;count<=15;count++){fixture(count);positive();}
    fixture(2);struct AdrenoVPCOutput moved=outputs[0];outputs[0]=outputs[2];outputs[2]=moved;positive();
    fixture(15);positive();uint32_t canonical[128];memcpy(canonical,out,sizeof out);
    for(unsigned i=0;i<7;i++){struct AdrenoVPCInput tmp=inputs[i];inputs[i]=inputs[14-i];inputs[14-i]=tmp;}positive();assert(!memcmp(out,canonical,sizeof out));
#define BAD(change,status) do {fixture(1);change;refusal(status,128);}while(0)
    BAD(state.chip_id=0x43050a02,AdrenoVPCProfile);BAD(state.reserved=1,AdrenoVPCProfile);BAD(state.vertex_max_register=-1,AdrenoVPCProfile);BAD(state.vertex_max_register=63,AdrenoVPCProfile);
    BAD(state.output_count=0,AdrenoVPCProfile);BAD(state.output_count=17,AdrenoVPCProfile);BAD(state.input_count=16,AdrenoVPCProfile);BAD(state.outputs=0,AdrenoVPCArgument);BAD(state.inputs=0,AdrenoVPCArgument);
    BAD(outputs[0].register_id=249,AdrenoVPCProfile);BAD(outputs[0].components=3,AdrenoVPCIdentity);BAD(outputs[1].half=1,AdrenoVPCProfile);BAD(outputs[1].view=1,AdrenoVPCProfile);BAD(outputs[1].aliased_components=1,AdrenoVPCProfile);BAD(outputs[1].bit_size=16,AdrenoVPCProfile);
    BAD(outputs[1].identity=outputs[0].identity;outputs[1].identity_bytes=12,AdrenoVPCIdentity);
    BAD(outputs[0].identity=outputs[1].identity;outputs[0].identity_bytes=outputs[1].identity_bytes,AdrenoVPCIdentity);
    BAD(inputs[0].identity=(const uint8_t *)"generated(missing)";inputs[0].identity_bytes=18,AdrenoVPCMissingVarying);
    BAD(inputs[0].components=3,AdrenoVPCProfile);BAD(inputs[0].component_mask=0,AdrenoVPCProfile);BAD(inputs[0].component_mask=16,AdrenoVPCProfile);BAD(inputs[0].input_location=121,AdrenoVPCProfile);BAD(inputs[0].bit_size=16,AdrenoVPCProfile);BAD(inputs[0].interpolation=1,AdrenoVPCProfile);
    BAD(inputs[0].identity=0,AdrenoVPCArgument);BAD(inputs[0].identity_bytes=0,AdrenoVPCArgument);BAD(inputs[0].identity_bytes=257,AdrenoVPCArgument);
    BAD(inputs[0].identity=(const uint8_t *)(UINTPTR_MAX-3);inputs[0].identity_bytes=16,AdrenoVPCArgument);
    BAD(inputs[0].identity=(const uint8_t *)out,AdrenoVPCAlias);BAD(state.outputs=(const void *)out,AdrenoVPCAlias);BAD(state.inputs=(const void *)out,AdrenoVPCAlias);
#undef BAD
    fixture(2);inputs[1].input_location=2;refusal(AdrenoVPCProfile,128);
    fixture(2);inputs[1].identity=inputs[0].identity;inputs[1].identity_bytes=inputs[0].identity_bytes;refusal(AdrenoVPCIdentity,128);
    fixture(1);refusal(AdrenoVPCCapacity,0);refusal(AdrenoVPCCapacity,41);
    reset();assert(AdrenoA740VaryingMap(0,out,128).status==AdrenoVPCArgument && !memcmp(out,saved,sizeof out));refusals++;
    printf("PASS: %u VPC source-reference profiles, %u transactional refusals, exact identity and padded location linkage\n",positives,refusals);
    return 0;
}
