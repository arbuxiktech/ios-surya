// SPDX-License-Identifier: MIT
// Actual compiler/cache instrument. No draw is encoded. The runtime may initialize
// neutral resources over the mandatory software peer; it executes no shader.
#import <Metal/Metal.h>
#include <TargetConditionals.h>
#include <CommonCrypto/CommonDigest.h>
#include "NativeMetalObjects.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>

#define REQUIRE(x) do {if(!(x)){std::fprintf(stderr,"FAIL UI cache line %d: %s; %s\n",__LINE__,#x,nmb_last_error());return 1;}} while(0)
static NmbRenderPipelineDescriptor descriptor(NmbLibrary *library,const std::string &vertex,const std::string &fragment,bool blur) {
    NmbRenderPipelineDescriptor d={};d.vertex=nmb_function_new(library,vertex.c_str());d.fragment=nmb_function_new(library,fragment.c_str());
    d.colors[0].format=blur?MTLPixelFormatRGBA16Float:MTLPixelFormatBGRA8Unorm;
    if(vertex.rfind("path_blit_",0)==0){d.colors[1].format=MTLPixelFormatBGRA8Unorm;d.colors[2].format=MTLPixelFormatR32Float;}
    for(auto &c:d.colors)if(c.format){c.write_mask=MTLColorWriteMaskAll;c.source_rgb=MTLBlendFactorOne;c.source_alpha=MTLBlendFactorOne;}
    d.samples=1;d.rasterization=1;d.vertex_inputs=1;
    if(blur) {
        d.attributes[0]={MTLVertexFormatFloat4,0,0};d.attributes[1]={MTLVertexFormatFloat2,16,0};d.attributes[3]={MTLVertexFormatFloat4,24,0};
        d.layouts[0]={40,MTLVertexStepFunctionPerVertex,1};
    } else {
        const uint64_t formats[]={MTLVertexFormatFloat4,MTLVertexFormatFloat2,MTLVertexFormatFloat2,MTLVertexFormatFloat4,MTLVertexFormatFloat};
        const uint64_t offsets[]={0,16,24,32,48};for(unsigned i=0;i<5;++i)d.attributes[i]={formats[i],offsets[i],0};
        d.layouts[0]={64,MTLVertexStepFunctionPerVertex,1};
    }
    return d;
}
int main(int argc,char **argv) {
    if(argc!=4)return 2;
    const bool produce=!std::strcmp(argv[3],"produce"),consume=!std::strcmp(argv[3],"consume"),uuid=!std::strcmp(argv[3],"uuid-miss"),empty=!std::strcmp(argv[3],"empty-driver");
    REQUIRE(produce||consume||uuid||empty);
#if TARGET_OS_IPHONE
    REQUIRE(getpagesize()==4096 && sysconf(_SC_PAGESIZE)==4096 && !getenv("HOSTSIM_PAGE_SIZE"));
#endif
    REQUIRE(!setenv("MESA_SHADER_CACHE_DISABLE","true",1) && !setenv("METAL2VULKAN_LOG_PIPELINES","1",1) && !setenv("NMB_TEST_REAL_VULKAN_LOADER",argv[1],1));
    std::string path=argv[1];path=path.substr(0,path.find_last_of('/')+1)+"libOfflineGraphicsCacheControl.dylib";
    if(const char *override=getenv("NMB_TEST_GRAPHICS_CACHE_LOADER"))path=override;
    void *loader=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);REQUIRE(loader);
    auto control=reinterpret_cast<void(*)(bool,bool)>(dlsym(loader,"nmb_test_graphics_cache_control"));
    auto counters=reinterpret_cast<void(*)(uint64_t *)>(dlsym(loader,"nmb_test_graphics_cache_counts"));
    auto submitted=reinterpret_cast<unsigned(*)()>(dlsym(RTLD_DEFAULT,"host_gpu_submissions"));REQUIRE(control&&counters&&submitted);
    const unsigned submissions=submitted();control(false,false);
    const std::string prefix=argv[2],archivePath=prefix+".archive",cachePath=prefix+(empty?".empty":".cache");
    NmbDevice *device=nmb_device_open(path.c_str(),cachePath.c_str());REQUIRE(device);
    uint64_t deviceCounts[10]={};counters(deviceCounts);control(!produce,uuid);
    const char *libraryPath="/System/Library/Frameworks/QuartzCore.framework/default.metallib";
    if(const char *override=getenv("NMB_TEST_QUARTZCORE_LIBRARY"))libraryPath=override;
    std::ifstream input(libraryPath,std::ios::binary);REQUIRE(input);
    std::vector<char> bytes(9670832);input.read(bytes.data(),bytes.size());REQUIRE(input.good() && input.peek()==EOF);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes.data(),(CC_LONG)bytes.size(),digest);
    char digestHex[65]={};for(unsigned i=0;i<32;++i)std::snprintf(digestHex+2*i,3,"%02x",digest[i]);
    REQUIRE(!std::strcmp(digestHex,"606f913adf520420f9b4bb666efb782a68c830f082f1e559e17b20d0d13adbc9"));
    NmbLibrary *library=nmb_library_new(device,bytes.data(),bytes.size(),false);REQUIRE(library&&nmb_library_function_count(library)==167);
    std::vector<std::string> names;for(unsigned i=0;i<167;++i){char name[256]={};REQUIRE(nmb_library_function_name(library,i,name,sizeof name)<sizeof name);names.emplace_back(name);}
    std::vector<NmbRenderPipelineDescriptor> recipes;
    for(const auto &name:names) {
        const auto marker=name.find("_vert_");if(marker==std::string::npos)continue;
        auto fragment=name;fragment.replace(marker,6,"_frag_");if(std::find(names.begin(),names.end(),fragment)==names.end())continue;
        recipes.push_back(descriptor(library,name,fragment,false));
    }
    REQUIRE(recipes.size()==22);
    for(const auto *suffix:{"lpf","lph"})recipes.push_back(descriptor(library,std::string("downsample_blur_vert_")+suffix,std::string("narrow_blur_7_frag_")+suffix,true));
    for(const auto &d:recipes)REQUIRE(d.vertex&&d.fragment&&nmb_function_type(d.vertex)==1&&nmb_function_type(d.fragment)==2);
    NmbBinaryArchive *archive=nmb_binary_archive_new(device,produce?nullptr:archivePath.c_str());REQUIRE(archive);
    NmbPipelineOptions options={MTLPipelineOptionFailOnBinaryArchiveMiss,&archive,1};
    uint64_t coldRequired=0;bool coldObject=false;
    if(produce) {
        // Exercise the real driver's compile-forbidden cold refusal, separately
        // from strict binary-archive misses. Clear only the test policy afterwards.
        control(true,false);auto cold=nmb_render_pipeline_new(device,&recipes.back());
        coldObject=cold!=nullptr;if(cold)nmb_render_pipeline_release(cold);
        uint64_t counts[10]={};counters(counts);coldRequired=counts[2];REQUIRE(coldRequired>0);control(false,false);
    }
    uint64_t before[10]={};counters(before);unsigned made=0;
    for(const auto &d:recipes) {
        if(produce)REQUIRE(nmb_binary_archive_add_render(archive,&d));
        NmbRenderPipeline *state=nmb_render_pipeline_new_with_options(device,&d,&options);
        if(uuid||empty){if(state)nmb_render_pipeline_release(state);break;}
        REQUIRE(state);nmb_render_pipeline_release(state);++made;
    }
    uint64_t counts[10]={};counters(counts);
    std::fprintf(stderr,"graphics results %s: calls=%llu success=%llu required=%llu valid=%llu hits=%llu cached=%llu/%llu/%llu\n",argv[3],
        (unsigned long long)(counts[0]-before[0]),(unsigned long long)(counts[1]-before[1]),(unsigned long long)(counts[2]-before[2]),
        (unsigned long long)(counts[3]-before[3]),(unsigned long long)(counts[4]-before[4]),(unsigned long long)(counts[7]-before[7]),
        (unsigned long long)(counts[8]-before[8]),(unsigned long long)(counts[9]-before[9]));
    if(uuid||empty) {REQUIRE(counts[2]>before[2]);if(uuid)REQUIRE(counts[5]>before[5]);}
    else REQUIRE(made==24 && counts[1]>before[1]);
    if(consume)REQUIRE(counts[2]==before[2] && counts[7]>before[7] && counts[8]-before[8]==counts[7]-before[7] && counts[9]-before[9]==counts[7]-before[7]);
    if(produce)REQUIRE(nmb_binary_archive_serialize(archive,archivePath.c_str()));
    NmbShaderCacheStats stats={};REQUIRE(nmb_device_shader_cache_stats(device,&stats));
    REQUIRE(!stats.rejected&&!stats.read_failures&&!stats.write_failures&&!stats.driver_truncated&&!stats.driver_mismatched);
    if(consume)REQUIRE(!stats.misses&&!stats.writes&&!stats.driver_misses&&stats.driver_loads);
    const unsigned initializationSubmissions=submitted()-submissions;
    std::printf("PASS: QuartzCore compiler %s pid=%d recipes=%u calls=%llu success=%llu required=%llu valid=%llu hits=%llu uuid_mutations=%llu devices=%llu artifacts=%llu/%llu/%llu driver=%llu/%llu/%llu/%llu cached_calls=%llu/%llu/%llu device_graphics=%llu cold_required=%llu cold_object=%u init_submissions=%u; no shader execution\n",argv[3],getpid(),made,
        (unsigned long long)(counts[0]-before[0]),(unsigned long long)(counts[1]-before[1]),(unsigned long long)(counts[2]-before[2]),
        (unsigned long long)(counts[3]-before[3]),(unsigned long long)(counts[4]-before[4]),(unsigned long long)(counts[5]-before[5]),(unsigned long long)counts[6],
        (unsigned long long)stats.hits,(unsigned long long)stats.misses,(unsigned long long)stats.writes,
        (unsigned long long)stats.driver_loads,(unsigned long long)stats.driver_hits,(unsigned long long)stats.driver_misses,(unsigned long long)stats.driver_saves,(unsigned long long)(counts[7]-before[7]),
        (unsigned long long)(counts[8]-before[8]),(unsigned long long)(counts[9]-before[9]),(unsigned long long)deviceCounts[0],(unsigned long long)coldRequired,(unsigned)coldObject,initializationSubmissions);
    nmb_binary_archive_release(archive);for(const auto &d:recipes){nmb_function_release(d.vertex);nmb_function_release(d.fragment);}
    nmb_library_release(library);nmb_device_release(device);
    return 0;
}
