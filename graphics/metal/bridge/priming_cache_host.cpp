/* SPDX-License-Identifier: MIT */
/* Device open/close only. Actual Turnip compiler; mandatory software peer.
 * No authored command, shader execution, timing or hardware claim. */
#include "NativeMetalObjects.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#include <TargetConditionals.h>
#define REQUIRE(x) do {if(!(x)){std::fprintf(stderr,"FAIL priming line %d: %s; %s\n",__LINE__,#x,nmb_last_error());return 1;}} while(0)
int main(int argc,char **argv) {
    if(argc!=4)return 2;
    const bool produce=!std::strcmp(argv[3],"produce"),consume=!std::strcmp(argv[3],"consume"),uuid=!std::strcmp(argv[3],"uuid-miss"),cold=!std::strcmp(argv[3],"cold-miss");
    REQUIRE(produce||consume||uuid||cold);
#if TARGET_OS_IPHONE
    REQUIRE(getpagesize()==4096 && sysconf(_SC_PAGESIZE)==4096 && !getenv("HOSTSIM_PAGE_SIZE"));
#endif
    REQUIRE(!setenv("NMB_TEST_REAL_VULKAN_LOADER",argv[1],1) && !setenv("MESA_SHADER_CACHE_DISABLE","true",1) && !setenv("NMB_TEST_FORCE_PRIME_STRICT",produce?"0":"1",1));
    const char *path=getenv("NMB_TEST_PRIMING_CACHE_LOADER");REQUIRE(path);
    void *loader=dlopen(path,RTLD_NOW|RTLD_LOCAL);REQUIRE(loader);
    auto control=reinterpret_cast<void(*)(bool,bool)>(dlsym(loader,"nmb_test_graphics_cache_control"));
    auto graphics=reinterpret_cast<void(*)(uint64_t *)>(dlsym(loader,"nmb_test_graphics_cache_counts"));
    auto compute=reinterpret_cast<void(*)(uint64_t *)>(dlsym(loader,"nmb_test_prime_compute_counts"));
    auto live=reinterpret_cast<unsigned(*)()>(dlsym(RTLD_DEFAULT,"host_gpu_live_objects"));
    auto submitted=reinterpret_cast<unsigned(*)()>(dlsym(RTLD_DEFAULT,"host_gpu_submissions"));
    REQUIRE(control&&graphics&&compute&&live&&submitted);control(false,uuid);
    NmbDevice *device=nmb_device_open(path,argv[2]);uint64_t c[5]={},g[10]={};compute(c);graphics(g);
    if(cold||uuid){REQUIRE(!device && c[2]>0);if(uuid)REQUIRE(g[5]>0);}
    else {
        REQUIRE(device && c[0] && c[1]==c[0] && !c[2] && c[3]==c[0] && g[0] && g[1]==g[0] && !g[2] && g[3]==g[0]);
        if(consume)REQUIRE(c[4]==c[0] && g[4]==g[0]);
        nmb_device_release(device);
    }
    REQUIRE(live()==0);
    std::printf("PASS: device priming %s pid=%d compute=%llu/%llu/%llu/%llu/%llu graphics=%llu/%llu/%llu/%llu/%llu uuid_mutations=%llu submissions=%u live=0; software peer, no shader execution\n",argv[3],getpid(),
        (unsigned long long)c[0],(unsigned long long)c[1],(unsigned long long)c[2],(unsigned long long)c[3],(unsigned long long)c[4],
        (unsigned long long)g[0],(unsigned long long)g[1],(unsigned long long)g[2],(unsigned long long)g[3],(unsigned long long)g[4],(unsigned long long)g[5],submitted());
}
