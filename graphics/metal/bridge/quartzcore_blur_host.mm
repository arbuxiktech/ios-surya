// SPDX-License-Identifier: MIT
// Execute unchanged target blur functions on a real host GPU. The reference
// is an authored seven-tap binomial convolution, not a deployment renderer.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../NativeBridgeMetalFrontend.h"
#include "NativeMetalObjects.h"

constexpr unsigned width=32,height=24;
struct Vertex {float position[4],uv[2],unusedColor[4];};
static const Vertex vertices[]={
    {{-1,-1,0,1},{0,1},{}},{{1,-1,0,1},{1,1},{}},{{-1,1,0,1},{0,0},{}},
    {{-1,1,0,1},{0,0},{}},{{1,-1,0,1},{1,1},{}},{{1,1,0,1},{1,0},{}}};
struct alignas(16) UniformF {float offset[8][2],weight[8],divide;unsigned char padding[12];};
struct alignas(8) UniformH {float offset[8][2];_Float16 weight[8],divide;unsigned char padding[6];};
static_assert(sizeof(UniformF)==112 && sizeof(UniformH)==88);

static MTLRenderPipelineDescriptor *blurDescriptor(id<MTLLibrary> library,bool half) {
    NSString *suffix=half?@"lph":@"lpf";auto d=[MTLRenderPipelineDescriptor new];
    d.vertexFunction=[library newFunctionWithName:[@"downsample_blur_vert_" stringByAppendingString:suffix]];
    d.fragmentFunction=[library newFunctionWithName:[@"narrow_blur_7_frag_" stringByAppendingString:suffix]];
    d.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA16Float;
    auto vd=[MTLVertexDescriptor vertexDescriptor];
    const unsigned indices[]={0,1,3},offsets[]={0,16,24};
    const MTLVertexFormat formats[]={MTLVertexFormatFloat4,MTLVertexFormatFloat2,MTLVertexFormatFloat4};
    for(unsigned i=0;i<3;++i){vd.attributes[indices[i]].format=formats[i];vd.attributes[indices[i]].offset=offsets[i];vd.attributes[indices[i]].bufferIndex=0;}
    vd.layouts[0].stride=sizeof(Vertex);d.vertexDescriptor=vd;return d;
}
static MTLRenderPipelineDescriptor *namedDescriptor(id<MTLLibrary> library,NSString *vertex,NSString *fragment) {
    auto d=[MTLRenderPipelineDescriptor new];d.vertexFunction=[library newFunctionWithName:vertex];d.fragmentFunction=[library newFunctionWithName:fragment];
    d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
    auto vd=[MTLVertexDescriptor vertexDescriptor];
    const MTLVertexFormat formats[]={MTLVertexFormatFloat4,MTLVertexFormatFloat2,MTLVertexFormatFloat2,MTLVertexFormatFloat4,MTLVertexFormatFloat};
    const unsigned offsets[]={0,16,24,32,48};
    for(unsigned i=0;i<5;++i){vd.attributes[i].format=formats[i];vd.attributes[i].bufferIndex=0;vd.attributes[i].offset=offsets[i];}
    vd.layouts[0].stride=64;d.vertexDescriptor=vd;
    if([vertex hasPrefix:@"path_blit_"]){d.colorAttachments[1].pixelFormat=MTLPixelFormatBGRA8Unorm;d.colorAttachments[2].pixelFormat=MTLPixelFormatR32Float;}
    return d;
}
static bool cacheState(const char *phase) {
    NmbShaderCacheStats s={};uint64_t work[10]={};
    if(!native_bridge_metal_shader_cache_stats(&s) || !native_bridge_metal_work_stats(work))return false;
    std::printf("cache %s artifact_hit=%llu miss=%llu write=%llu driver_load=%llu hit=%llu miss=%llu save=%llu same=%llu vk_makings=%llu\n",phase,
        (unsigned long long)s.hits,(unsigned long long)s.misses,(unsigned long long)s.writes,
        (unsigned long long)s.driver_loads,(unsigned long long)s.driver_hits,(unsigned long long)s.driver_misses,
        (unsigned long long)s.driver_saves,(unsigned long long)s.driver_unchanged,(unsigned long long)work[0]);
    return !s.rejected && !s.read_failures && !s.write_failures && !s.driver_truncated && !s.driver_mismatched && !work[4];
}
static bool inventory(id<MTLDevice> device,id<MTLLibrary> library) {
    unsigned vertex=0,fragment=0,kernels=0,ordinary=0,tile=0,pairs=0,bare=0,attributes=0,attachments=0;
    for(NSString *name in library.functionNames) {
        id<MTLFunction> f=[library newFunctionWithName:name];if(!f)return false;
        NSError *error=nil;
        if(f.functionType==MTLFunctionTypeKernel) {
            ++kernels;
            if([device newComputePipelineStateWithFunction:f error:&error])++ordinary;
            else if([error.localizedDescription containsString:@"NeedsInput(TileAttachments)"])++tile;
            else {std::printf("FAIL unclassified compute %s: %s\n",name.UTF8String,error.localizedDescription.UTF8String);return false;}
        } else if(f.functionType==MTLFunctionTypeFragment)++fragment;
        else if(f.functionType==MTLFunctionTypeVertex) {
            ++vertex;NSString *other=[name stringByReplacingOccurrencesOfString:@"_vert_" withString:@"_frag_"];
            if([other isEqualToString:name] || ![library.functionNames containsObject:other])continue;
            ++pairs;auto d=[MTLRenderPipelineDescriptor new];d.vertexFunction=f;d.fragmentFunction=[library newFunctionWithName:other];
            d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
            if([device newRenderPipelineStateWithDescriptor:d error:&error])++bare;
            else if([error.localizedDescription containsString:@"InvalidDescriptor(VertexAttribute"])++attributes;
            else if([error.localizedDescription containsString:@"InvalidDescriptor(ColorAttachment"])++attachments;
            else {std::printf("FAIL unclassified render %s: %s\n",name.UTF8String,error.localizedDescription.UTF8String);return false;}
        } else return false;
    }
    std::printf("inventory functions=167 vertex=%u fragment=%u kernels=%u ordinary=%u tile_requires=%u pairs=%u bare_ok=%u missing_vertex=%u missing_attachment=%u\n",
        vertex,fragment,kernels,ordinary,tile,pairs,bare,attributes,attachments);
    return vertex==41 && fragment==111 && kernels==15 && ordinary==4 && tile==11 && pairs==22 && bare==14 && attributes==6 && attachments==2;
}
static std::vector<_Float16> convolution(const std::vector<_Float16> &input,bool vertical) {
    // Independent integer stencil: coefficients sum to 64, clamp at the edge.
    constexpr unsigned weight[]={1,6,15,20,15,6,1};std::vector<_Float16> result(input.size());
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)for(unsigned c=0;c<4;++c) {
        float sum=0;
        for(int tap=-3;tap<=3;++tap) {
            const int sx=std::clamp((int)x+(vertical?0:tap),0,(int)width-1);
            const int sy=std::clamp((int)y+(vertical?tap:0),0,(int)height-1);
            sum+=(float)input[(sy*width+sx)*4+c]*weight[tap+3];
        }
        result[(y*width+x)*4+c]=(_Float16)(sum/64);
    }
    return result;
}
static bool encode(id<MTLCommandBuffer> command,id<MTLRenderPipelineState> state,id<MTLTexture> source,
                   id<MTLTexture> destination,id<MTLSamplerState> sampler,bool half,bool vertical) {
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=destination;
    pass.colorAttachments[0].loadAction=MTLLoadActionDontCare;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];if(!encoder)return false;
    [encoder setRenderPipelineState:state];[encoder setVertexBytes:vertices length:sizeof vertices atIndex:0];
    const float identity[]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},texmat[]={1,1,0,0};
    [encoder setVertexBytes:identity length:sizeof identity atIndex:2];[encoder setVertexBytes:texmat length:sizeof texmat atIndex:3];
    [encoder setFragmentTexture:source atIndex:3];[encoder setFragmentSamplerState:sampler atIndex:0];
    // Two bilinear pairs exactly represent the seven authored stencil taps.
    UniformF f={};UniformH h={};
    f.offset[0][vertical?1:0]=0.6f/(vertical?height:width);
    f.offset[1][vertical?1:0]=(15.0f/7)/(vertical?height:width);
    f.weight[0]=25.0f/64;f.weight[1]=7.0f/64;
    if(half) {
        memcpy(h.offset,f.offset,sizeof h.offset);h.weight[0]=(_Float16)f.weight[0];h.weight[1]=(_Float16)f.weight[1];
        const _Float16 scale=1;[encoder setFragmentBytes:&h length:sizeof h atIndex:1];[encoder setFragmentBytes:&scale length:sizeof scale atIndex:6];
    } else {
        const float scale=1;[encoder setFragmentBytes:&f length:sizeof f atIndex:1];[encoder setFragmentBytes:&scale length:sizeof scale atIndex:6];
    }
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];[encoder endEncoding];return true;
}
static bool runBlur(id<MTLDevice> device,id<MTLRenderPipelineState> state,bool half) {
    std::vector<_Float16> data(width*height*4);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        auto p=data.data()+(y*width+x)*4;p[0]=(_Float16)((x%7)/7.0f);p[1]=(_Float16)((y%5)/5.0f);
        p[2]=(_Float16)(x==width/2 && y==height/2);p[3]=1;
    }
    const auto expected=convolution(convolution(data,false),true);
    auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:width height:height mipmapped:NO];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
    id<MTLTexture> input=[device newTextureWithDescriptor:td],middle=[device newTextureWithDescriptor:td],output=[device newTextureWithDescriptor:td];
    auto sd=[MTLSamplerDescriptor new];sd.minFilter=MTLSamplerMinMagFilterLinear;sd.magFilter=MTLSamplerMinMagFilterLinear;
    sd.sAddressMode=MTLSamplerAddressModeClampToEdge;sd.tAddressMode=MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> sampler=[device newSamplerStateWithDescriptor:sd];id<MTLCommandQueue> queue=[device newCommandQueue];
    if(!input || !middle || !output || !sampler || !queue)return false;
    [input replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:data.data() bytesPerRow:width*8];
    float worst=0;
    for(unsigned round=0;round<6;++round){@autoreleasepool {
        NmbShaderCacheStats before={},after={};uint64_t workBefore[10]={},workAfter[10]={};
        if(!native_bridge_metal_shader_cache_stats(&before) || !native_bridge_metal_work_stats(workBefore))return false;
        id<MTLCommandBuffer> command=[queue commandBuffer];
        if(!encode(command,state,input,middle,sampler,half,false) || !encode(command,state,middle,output,sampler,half,true))return false;
        [command commit];[command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted){std::printf("FAIL blur command: %s\n",command.error.localizedDescription.UTF8String);return false;}
        std::vector<_Float16> got(data.size());[output getBytes:got.data() bytesPerRow:width*8 fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
        for(size_t i=0;i<got.size();++i){const float error=std::fabs((float)got[i]-(float)expected[i]);
            if(!std::isfinite((float)got[i]) || error>0.003f){std::printf("FAIL blur %s round=%u element=%zu got=%g expected=%g\n",half?"half":"float",round,i,(double)got[i],(double)expected[i]);return false;}
            worst=std::max(worst,error);
        }
        if(!native_bridge_metal_shader_cache_stats(&after) || !native_bridge_metal_work_stats(workAfter))return false;
        std::printf("round %s %u vk_makings=%llu artifact_hit=%llu miss=%llu driver_load=%llu hit=%llu miss=%llu\n",half?"half":"float",round,
            (unsigned long long)(workAfter[0]-workBefore[0]),(unsigned long long)(after.hits-before.hits),(unsigned long long)(after.misses-before.misses),
            (unsigned long long)(after.driver_loads-before.driver_loads),(unsigned long long)(after.driver_hits-before.driver_hits),(unsigned long long)(after.driver_misses-before.driver_misses));
        if(round && (workAfter[0]!=workBefore[0] || after.misses!=before.misses || after.driver_misses!=before.driver_misses)) {
            std::puts("FAIL repeated identical blur performs new pipeline/translation work");return false;
        }
    }}
    std::printf("blur %s rounds=6 passes=12 texels=%u max_error=%g GPU=pass\n",half?"half":"float",width*height,worst);return true;
}

int main(int argc,char **argv) {
    if(argc!=6 || (strcmp(argv[5],"produce") && strcmp(argv[5],"consume") && strcmp(argv[5],"inventory")))return 2;
    if(setenv("METAL2VULKAN_LOG_PIPELINES","1",1))return 2;
    const bool produce=!strcmp(argv[5],"produce");
    @autoreleasepool {
        id<MTLDevice> device=native_bridge_metal_open(argv[1],argv[2],0x6e6d62,nullptr);if(!device)return 2;
        NSError *error=nil;id<MTLLibrary> library=[device newLibraryWithFile:@(argv[3]) error:&error];if(!library)return 2;
        if(library.functionNames.count!=167)return 1;
        if(!strcmp(argv[5],"inventory"))return inventory(device,library)?0:1;
        auto ad=[MTLBinaryArchiveDescriptor new];NSURL *url=[NSURL fileURLWithPath:@(argv[4])];if(!produce)ad.url=url;
        id<MTLBinaryArchive> archive=[device newBinaryArchiveWithDescriptor:ad error:&error];if(!archive)return 1;
        if(!cacheState("opened"))return 1;
        if(produce) {
            auto empty=blurDescriptor(library,false);empty.binaryArchives=@[archive];error=nil;
            if([device newRenderPipelineStateWithDescriptor:empty options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error] || !error)return 1;
        }
        unsigned pairs=0;
        for(NSString *name in library.functionNames) {
            id<MTLFunction> function=[library newFunctionWithName:name];if(function.functionType!=MTLFunctionTypeVertex)continue;
            NSString *fragment=[name stringByReplacingOccurrencesOfString:@"_vert_" withString:@"_frag_"];
            if([fragment isEqualToString:name] || ![library.functionNames containsObject:fragment])continue;
            auto d=namedDescriptor(library,name,fragment);d.binaryArchives=@[archive];
            if(produce && ![archive addRenderPipelineFunctionsWithDescriptor:d error:&error]){
                std::printf("FAIL archive add %s: %s\n",name.UTF8String,error.localizedDescription.UTF8String);return 1;}
            error=nil;
            if(![device newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error] || error){
                std::printf("FAIL strict pair %s: %s\n",name.UTF8String,error.localizedDescription.UTF8String);return 1;}
            ++pairs;
        }
        if(pairs!=22)return 1;
        std::printf("valid descriptor pairs=%u strict archive=pass\n",pairs);
        for(bool half:{false,true}) {
            auto d=blurDescriptor(library,half);d.binaryArchives=@[archive];
            if(produce && ![archive addRenderPipelineFunctionsWithDescriptor:d error:&error])return 1;
            error=nil;id<MTLRenderPipelineState> state=[device newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error];
            if(!state){std::printf("FAIL blur pipeline: %s\n",error.localizedDescription.UTF8String);return 1;}
            const auto format=d.colorAttachments[0].pixelFormat;d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;error=nil;
            if([device newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error] || !error)return 1;
            d.colorAttachments[0].pixelFormat=format;
            if(!runBlur(device,state,half))return 1;
        }
        if(produce && ![archive serializeToURL:url error:&error])return 1;
        if(!cacheState("finished"))return 1;
        uint64_t work[10]={};if(!native_bridge_metal_work_stats(work) || !work[0])return 1; // logging must be active, never a zero-counter shortcut
        std::puts("PASS: unchanged target blur functions execute repeated separable Gaussian reference cases on the host GPU");
    }
}
