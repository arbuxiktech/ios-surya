// SPDX-License-Identifier: MIT
// The default compute-function path used by the target; no explicit binary archive.
// Runs over Turnip and the software kernel peer: no shader output is claimed.
#include "NativeMetalObjects.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

#define REQUIRE(x) do {if (!(x)) {std::fprintf(stderr,"FAIL line %d: %s; %s\n",__LINE__,#x,nmb_last_error());return 1;}} while (0)

int main(int argc,char **argv) {
    if(argc!=4)return 2;
    const bool consume=!std::strcmp(argv[3],"consume");
    REQUIRE(consume||!std::strcmp(argv[3],"produce"));
    auto submissions=(unsigned(*)())dlsym(RTLD_DEFAULT,"host_gpu_submissions");REQUIRE(submissions);
    NmbDevice *device=nmb_device_open(argv[1],argv[2]);REQUIRE(device);
    NmbShaderCacheStats before={},after={};REQUIRE(nmb_device_shader_cache_stats(device,&before));
    size_t size=0;const void *air=nmb_fixture_air(&size);
    NmbLibrary *library=nmb_library_new(device,air,size,true);REQUIRE(library);
    NmbFunction *function=nmb_function_new(library,"scale_and_offset");REQUIRE(function);
    NmbComputePipeline *pipeline=nmb_compute_pipeline_new(device,function);REQUIRE(pipeline);
    float input[64]={};
    NmbBuffer *source=nmb_buffer_new(device,sizeof input,0,input);
    NmbBuffer *destination=nmb_buffer_new(device,sizeof input,0,nullptr);REQUIRE(source&&destination);
    NmbQueue *queue=nmb_queue_new(device);REQUIRE(queue);
    NmbCommandBuffer *command=nmb_command_buffer_new(queue);REQUIRE(command);
    NmbEncoder *encoder=nmb_compute_encoder_new(command);REQUIRE(encoder);
    REQUIRE(nmb_compute_set_pipeline(encoder,pipeline));
    REQUIRE(nmb_encoder_set_buffer(encoder,0,source,0,0));
    REQUIRE(nmb_encoder_set_buffer(encoder,0,destination,0,1));
    const float scale=2;REQUIRE(nmb_encoder_set_bytes(encoder,0,&scale,sizeof scale,2));
    const uint64_t threads[]={64,1,1};
    REQUIRE(nmb_compute_dispatch(encoder,threads,threads,true));
    REQUIRE(nmb_encoder_end(encoder));
    const unsigned submitted=submissions();
    REQUIRE(nmb_command_buffer_commit(command));
    REQUIRE(nmb_command_buffer_wait(command,10000)==0);
    REQUIRE(nmb_command_buffer_status(command)==4&&submissions()>submitted);
    REQUIRE(nmb_device_shader_cache_stats(device,&after));
    std::printf("cache %s: artifacts hit=%llu miss=%llu write=%llu; driver load=%llu hit=%llu miss=%llu save=%llu same=%llu; rejected=%llu readfail=%llu writefail=%llu truncated=%llu mismatched=%llu\n",argv[3],
        (unsigned long long)(after.hits-before.hits),(unsigned long long)(after.misses-before.misses),(unsigned long long)(after.writes-before.writes),
        (unsigned long long)(after.driver_loads-before.driver_loads),(unsigned long long)(after.driver_hits-before.driver_hits),
        (unsigned long long)(after.driver_misses-before.driver_misses),(unsigned long long)(after.driver_saves-before.driver_saves),
        (unsigned long long)(after.driver_unchanged-before.driver_unchanged),(unsigned long long)after.rejected,(unsigned long long)after.read_failures,
        (unsigned long long)after.write_failures,(unsigned long long)after.driver_truncated,(unsigned long long)after.driver_mismatched);
    if(consume){
        REQUIRE(after.hits>before.hits&&after.misses==before.misses);
        REQUIRE(after.driver_loads>before.driver_loads&&after.driver_hits>before.driver_hits);
        REQUIRE(after.driver_misses==before.driver_misses);
    }else{
        REQUIRE(after.misses>before.misses&&after.writes>before.writes);
        REQUIRE(after.driver_misses>before.driver_misses&&after.driver_saves>before.driver_saves);
    }
    REQUIRE(!after.read_failures&&!after.write_failures&&!after.rejected&&!after.driver_truncated&&!after.driver_mismatched);
    nmb_command_buffer_release(command);nmb_queue_release(queue);
    nmb_buffer_release(destination);nmb_buffer_release(source);nmb_compute_pipeline_release(pipeline);
    nmb_function_release(function);nmb_library_release(library);nmb_device_release(device);
    std::printf("PASS: default compute %s uses separate artifact/Turnip caches; software transport, results unchecked\n",argv[3]);
    return 0;
}
