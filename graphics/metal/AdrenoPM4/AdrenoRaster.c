/* SPDX-License-Identifier: MIT */
#include "AdrenoRaster.h"
#include "AdrenoPM4.h"
static struct AdrenoRasterResult fail(uint32_t s) {
    return (struct AdrenoRasterResult){s,0,0};
}
static int wraps(uintptr_t p,size_t n) { return n>UINTPTR_MAX-p; }
static int overlap(uintptr_t a,size_t n,uintptr_t b,size_t m) {
    return n && m && a<b+m && b<a+n;
}
static int finite32(uint32_t v) { return (v&UINT32_C(0x7f800000))!=UINT32_C(0x7f800000); }
static void packet(uint32_t **out,uint32_t address,const uint32_t *values,uint32_t n) {
    uint32_t header=0;(void)AdrenoPM4Type4(address,n,&header);*(*out)++=header;
    for(uint32_t i=0;i<n;i++) *(*out)++=values[i];
}
static void reg32(uint32_t **out,uint32_t address,uint32_t value) { packet(out,address,&value,1); }
struct AdrenoRasterResult AdrenoA740Raster(const struct AdrenoRasterState *s,
    uint32_t *output,size_t capacity) {
    if(!s || !output || (uintptr_t)s%_Alignof(struct AdrenoRasterState) ||
       (uintptr_t)output%_Alignof(uint32_t) || wraps((uintptr_t)s,sizeof(*s)) ||
       capacity>SIZE_MAX/4 || wraps((uintptr_t)output,capacity*4)) return fail(AdrenoRasterArgument);
    if((s->chip_id!=ADRENO_A740_CHIP_ID && s->chip_id!=ADRENO_A740_FALLBACK_CHIP_ID) ||
       (s->flags&~ADRENO_RASTER_FLAGS) || s->reserved || s->polygon_mode<1 || s->polygon_mode>3 ||
       s->point_size_q4>65472 || s->line_width_q1>255 ||
       !finite32(s->offset_scale_bits) || !finite32(s->offset_units_bits) ||
       !finite32(s->offset_clamp_bits)) return fail(AdrenoRasterProfile);
    uint32_t f=s->flags;
    int clamp=(f&(AdrenoRasterClipNear|AdrenoRasterClipFar))!=
        (AdrenoRasterClipNear|AdrenoRasterClipFar);
    uint32_t words=clamp?24:60;
    if(capacity<words) return fail(AdrenoRasterCapacity);
    if(overlap((uintptr_t)s,sizeof(*s),(uintptr_t)output,words*4)) return fail(AdrenoRasterAlias);
    uint32_t point_min=s->point_size_q4,point_max=s->point_size_q4;
    if(f&AdrenoRasterPointSizePerVertex) {
        point_min=(f&(AdrenoRasterPointQuad|AdrenoRasterPointSmooth|AdrenoRasterMultisample))?0:16;
        point_max=65472;
    }
    uint32_t cl=0xa0u|((f&AdrenoRasterClipNear)?0:2u)|
        ((f&AdrenoRasterClipFar)?0:4u)|((f&AdrenoRasterHalfZ)?64u:0);
    uint32_t su=((f&AdrenoRasterCullFront)?1u:0)|((f&AdrenoRasterCullBack)?2u:0)|
        ((f&AdrenoRasterFrontCCW)?0:4u)|(s->line_width_q1<<3)|
        ((f&AdrenoRasterOffsetTriangles)?0x800u:0)|((f&AdrenoRasterMultisample)?0x2000u:0);
    uint32_t pc=((f&AdrenoRasterPrimitiveRestart)?1u:0)|((f&AdrenoRasterFlatFirst)?0:2u);
    uint32_t *out=output;
    reg32(&out,0x8000,cl);reg32(&out,0x8090,su);
    reg32(&out,0x8091,point_min|(point_max<<16));reg32(&out,0x8092,s->point_size_q4);
    reg32(&out,0x8095,s->offset_scale_bits);reg32(&out,0x8096,s->offset_units_bits);
    reg32(&out,0x8097,s->offset_clamp_bits);
    reg32(&out,0x9b00,pc);reg32(&out,0x9109,pc);
    reg32(&out,0x9108,s->polygon_mode);reg32(&out,0x9809,s->polygon_mode);
    reg32(&out,0x9307,s->polygon_mode);
    if(!clamp) {
        uint32_t ranges[32];
        for(unsigned i=0;i<16;i++) { ranges[2*i]=0;ranges[2*i+1]=UINT32_C(0x3f800000); }
        packet(&out,0x8070,ranges,32);packet(&out,0x88c0,ranges,2);
    }
    return (struct AdrenoRasterResult){AdrenoRasterOK,(uint32_t)(out-output),clamp?12u:46u};
}
