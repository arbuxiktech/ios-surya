/* SPDX-License-Identifier: MIT */
#include "AdrenoVPC.h"
#include "AdrenoPM4.h"
static struct AdrenoVPCResult failure(uint32_t s) { return (struct AdrenoVPCResult){s,0,0,0,0,0}; }
static int wraps(uintptr_t p,size_t n) { return n>UINTPTR_MAX-p; }
static int overlap(uintptr_t a,size_t n,uintptr_t b,size_t m) { return n && m && a<b+m && b<a+n; }
static unsigned last_bit(unsigned mask) { unsigned n=0;while(mask){n++;mask>>=1;}return n; }
static int same(const uint8_t *a,size_t n,const uint8_t *b,size_t m) {
    if(n!=m) return 0;
    for(size_t i=0;i<n;i++) if(a[i]!=b[i]) return 0;
    return 1;
}
static int position(const uint8_t *p,size_t n) { return same(p,n,(const uint8_t *)"air.position",12); }
static uint32_t name_check(const uint8_t *p,size_t n,uintptr_t output,size_t bytes) {
    if(!p || !n || n>256 || wraps((uintptr_t)p,n)) return AdrenoVPCArgument;
    if(overlap((uintptr_t)p,n,output,bytes)) return AdrenoVPCAlias;
    for(size_t i=0;i<n;i++) if(p[i]<32 || p[i]>126) return AdrenoVPCIdentity;
    if(position(p,n)) return 0;
    if(n<=11 || !same(p,10,(const uint8_t *)"generated(",10) || p[n-1]!=')') return AdrenoVPCIdentity;
    return 0;
}
static void packet(uint32_t **out,uint32_t reg,const uint32_t *values,uint32_t n) {
    uint32_t header=0;(void)AdrenoPM4Type4(reg,n,&header);*(*out)++=header;
    for(uint32_t i=0;i<n;i++) *(*out)++=values[i];
}
static void reg32(uint32_t **out,uint32_t reg,uint32_t v) { packet(out,reg,&v,1); }
struct AdrenoVPCResult AdrenoA740VaryingMap(const struct AdrenoVPCState *s,uint32_t *output,size_t capacity) {
    if(!s || !output || (uintptr_t)output%_Alignof(uint32_t) || wraps((uintptr_t)s,sizeof(*s)) || capacity>SIZE_MAX/4 || wraps((uintptr_t)output,capacity*4)) return failure(AdrenoVPCArgument);
    if((s->chip_id!=ADRENO_A740_CHIP_ID && s->chip_id!=ADRENO_A740_FALLBACK_CHIP_ID) || s->reserved || s->vertex_max_register<0 || s->vertex_max_register>62 || !s->output_count || s->output_count>ADRENO_VPC_MAX_OUTPUTS || s->input_count>ADRENO_VPC_MAX_INPUTS) return failure(AdrenoVPCProfile);
    if(!s->outputs || (s->input_count && !s->inputs) || wraps((uintptr_t)s->outputs,s->output_count*sizeof(*s->outputs)) || wraps((uintptr_t)s->inputs,s->input_count*sizeof(*s->inputs))) return failure(AdrenoVPCArgument);
    uint32_t entries=(uint32_t)s->input_count+1,nreg=(entries+1)/2,ndest=(entries+3)/4,words=40+nreg+ndest;
    if(capacity<words) return failure(AdrenoVPCCapacity);
    uintptr_t dst=(uintptr_t)output;size_t bytes=words*4;
    if(overlap(dst,bytes,(uintptr_t)s,sizeof(*s)) || overlap(dst,bytes,(uintptr_t)s->outputs,s->output_count*sizeof(*s->outputs)) || overlap(dst,bytes,(uintptr_t)s->inputs,s->input_count*sizeof(*s->inputs))) return failure(AdrenoVPCAlias);
    unsigned pos=ADRENO_VPC_MAX_OUTPUTS,limit=((unsigned)s->vertex_max_register+1)*4;
    for(size_t i=0;i<s->output_count;i++) {
        const struct AdrenoVPCOutput *o=s->outputs+i;
        uint32_t error=name_check(o->identity,o->identity_bytes,dst,bytes);if(error) return failure(error);
        if(!o->components || o->components>4 || o->bit_size!=32 || o->half || o->view || o->aliased_components || o->register_id>=limit || o->components>limit-o->register_id) return failure(AdrenoVPCProfile);
        if(position(o->identity,o->identity_bytes)) { if(pos!=ADRENO_VPC_MAX_OUTPUTS || o->components!=4) return failure(AdrenoVPCIdentity);pos=(unsigned)i; }
        for(size_t j=0;j<i;j++) if(same(o->identity,o->identity_bytes,s->outputs[j].identity,s->outputs[j].identity_bytes)) return failure(AdrenoVPCIdentity);
    }
    if(pos==ADRENO_VPC_MAX_OUTPUTS) return failure(AdrenoVPCIdentity);
    unsigned order[ADRENO_VPC_MAX_INPUTS],matched[ADRENO_VPC_MAX_INPUTS],max_loc=0;uint32_t mask[4]={0};
    for(size_t i=0;i<s->input_count;i++) {
        const struct AdrenoVPCInput *in=s->inputs+i;
        uint32_t error=name_check(in->identity,in->identity_bytes,dst,bytes);if(error) return failure(error);
        unsigned width=last_bit(in->component_mask);
        if(position(in->identity,in->identity_bytes) || !in->components || in->components>4 || in->bit_size!=32 || in->interpolation || !width || width>in->components || in->input_location>124 || width>124-in->input_location) return failure(AdrenoVPCProfile);
        for(size_t j=0;j<i;j++) {
            const struct AdrenoVPCInput *p=s->inputs+j;unsigned pw=last_bit(p->component_mask);
            if(same(in->identity,in->identity_bytes,p->identity,p->identity_bytes)) return failure(AdrenoVPCIdentity);
            if(overlap(in->input_location,width,p->input_location,pw)) return failure(AdrenoVPCProfile);
        }
        unsigned found=ADRENO_VPC_MAX_OUTPUTS;
        for(size_t j=0;j<s->output_count;j++) if(same(in->identity,in->identity_bytes,s->outputs[j].identity,s->outputs[j].identity_bytes)) found=(unsigned)j;
        if(found==ADRENO_VPC_MAX_OUTPUTS) return failure(AdrenoVPCMissingVarying);
        if(s->outputs[found].components!=in->components) return failure(AdrenoVPCProfile);
        matched[i]=found;order[i]=(unsigned)i;
        for(unsigned j=0;j<width;j++) mask[(in->input_location+j)/32]|=UINT32_C(1)<<((in->input_location+j)%32);
        if(in->input_location+width>max_loc) max_loc=in->input_location+width;
    }
    for(size_t i=1;i<s->input_count;i++) {
        unsigned v=order[i];size_t j=i;
        while(j && s->inputs[order[j-1]].input_location>s->inputs[v].input_location) {order[j]=order[j-1];j--;}
        order[j]=v;
    }
    uint32_t outputs[8]={0},destinations[4]={0},disabled[4],zeros[16]={0};
    for(unsigned i=0;i<entries;i++) {
        unsigned reg,comp,loc;
        if(i<s->input_count) {unsigned k=order[i];reg=s->outputs[matched[k]].register_id;comp=s->inputs[k].component_mask;loc=s->inputs[k].input_location;}
        else {reg=s->outputs[pos].register_id;comp=15;loc=max_loc;}
        outputs[i/2]|=(reg|(comp<<8))<<((i%2)*16);destinations[i/4]|=loc<<((i%4)*8);
    }
    for(unsigned i=0;i<4;i++) disabled[i]=~mask[i];
    uint32_t *out=output;
    packet(&out,0x9212,disabled,4);packet(&out,0xa803,outputs,nreg);packet(&out,0xa813,destinations,ndest);
    reg32(&out,0x9301,(max_loc+4)|(max_loc<<8)|UINT32_C(0xff0000));
    reg32(&out,0x9101,0xffff00);reg32(&out,0x9311,0xffff00);reg32(&out,0x8001,0);
    reg32(&out,0xa802,entries);reg32(&out,0x9104,0xffffff);reg32(&out,0x9314,0xffffff);reg32(&out,0x809b,0);
    packet(&out,0x9200,zeros,16);
    return (struct AdrenoVPCResult){AdrenoVPCOK,(uint32_t)(out-output),words-12,(uint32_t)s->input_count,max_loc+4,max_loc};
}
