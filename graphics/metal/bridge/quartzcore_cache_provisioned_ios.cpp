/* SPDX-License-Identifier: MIT */
/* QEMU-only first-consumer harness. Reuse the exact volume/manifest/checkpoint
 * helpers, but never call the cold producer suite. Initial Data already holds
 * the actual iOS-generated archive and caches at the consumer's normal paths. */
#define main unused_cold_suite_main
#include "quartzcore_cache_ios.cpp"
#undef main

int main(int argc,char **argv) {
    (void)argv;result[0]=0x515443504b494f32ull;result[2]=(uint64_t)getpid();result[3]=getpagesize();result[4]=sysconf(_SC_PAGESIZE);
    if(argc!=1 || getpid()!=1 || result[3]!=4096 || result[4]!=4096)stop(1);
    mountCompilerView();
    if(setenv("MESA_SHADER_CACHE_DISABLE","true",1) || unsetenv("DYLD_INSERT_LIBRARIES") || unsetenv("HOSTSIM_PAGE_SIZE") ||
       unsetenv("NMB_TEST_GRAPHICS_CACHE_LOADER") || unsetenv("NMB_TEST_QUARTZCORE_LIBRARY"))stop(2);
    // Hash the actual initial-image bytes before any compiler worker is spawned.
    summarize();std::array<unsigned char,128> initial={};memcpy(initial.data(),snapshot,initial.size());
    if(unlink(workerLog) && errno!=ENOENT)stop(5);
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions) || posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,workerLog,O_WRONLY|O_CREAT|O_APPEND,0600) ||
       posix_spawn_file_actions_adddup2(&actions,STDOUT_FILENO,STDERR_FILENO))stop(4);
    const char *modes[]={"consume","uuid-miss","empty-driver"};
    for(unsigned i=0;i<3;++i) {
        char *arguments[]={const_cast<char *>("compiler-worker"),const_cast<char *>(driverPath),const_cast<char *>(prefix),const_cast<char *>(modes[i]),nullptr};
        pid_t child=0;int error=posix_spawn(&child,"/private/var/tmp/compiler-worker",&actions,nullptr,arguments,environ);
        result[12+3*i]=(uint64_t)child;result[13+3*i]=(uint64_t)error;if(error || child<=1)stop(10+i);
        int status=0;while(waitpid(child,&status,0)<0)if(errno!=EINTR)stop(20+i);
        result[14+3*i]=(uint64_t)status;if(!WIFEXITED(status) || WEXITSTATUS(status)!=0)stop(30+i);result[5]=i+1;
    }
    posix_spawn_file_actions_destroy(&actions);summarize();
    if(memcmp(initial.data(),snapshot,initial.size()))stop(95);
    memcpy(snapshot+256,initial.data(),initial.size());result[6]=384;result[45]=2;result[46]=1;stop(0);
}
