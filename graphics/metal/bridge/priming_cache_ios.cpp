/* SPDX-License-Identifier: MIT */
/* Owned offline QEMU harness for the real iOS device-priming cache. Shared
 * helpers retain exact RAM-volume identity, readonly view and bounded capture.
 * The optional first-consumer marker is part of the hashed initial test image. */
#define main unused_ui_cold_suite_main
#include "quartzcore_cache_ios.cpp"
#undef main

int main(int argc,char **argv) {
    (void)argv;prefix="/private/var/tmp/priming";workerLog="/private/var/tmp/priming-worker.log";
    bool first=false;
    if(FILE *file=fopen("/private/var/tmp/priming-first-consumer","rb")) {
        char marker[32]={};const auto n=fread(marker,1,sizeof marker,file);fclose(file);
        if(n!=strlen("priming-first-v1\n") || memcmp(marker,"priming-first-v1\n",n))stop(96);
        first=true;
    } else if(errno!=ENOENT)stop(97);
    result[0]=first?0x5052494d494f5332ull:0x5052494d494f5331ull;
    result[2]=getpid();result[3]=getpagesize();result[4]=sysconf(_SC_PAGESIZE);
    if(argc!=1 || getpid()!=1 || result[3]!=4096 || result[4]!=4096)stop(1);
    mountCompilerView();
    if(setenv("MESA_SHADER_CACHE_DISABLE","true",1) || unsetenv("DYLD_INSERT_LIBRARIES") || unsetenv("HOSTSIM_PAGE_SIZE") ||
       setenv("NMB_TEST_PRIMING_CACHE_LOADER","/System/Library/Extensions/OfflineTurnipCompiler/libOfflinePrimingCacheControl.dylib",1))stop(2);
    const std::string cache=std::string(prefix)+".cache",empty=std::string(prefix)+".empty",copy=std::string(prefix)+".archive";
    std::array<unsigned char,128> initial={};
    if(first){summarize();memcpy(initial.data(),snapshot,initial.size());}
    else {struct stat old={};if(!lstat(copy.c_str(),&old) || errno!=ENOENT)stop(3);}
    if(unlink(workerLog) && errno!=ENOENT)stop(5);
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions) || posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,workerLog,O_WRONLY|O_CREAT|O_APPEND,0600) ||
       posix_spawn_file_actions_adddup2(&actions,STDOUT_FILENO,STDERR_FILENO))stop(4);
    const char *coldModes[]={"cold-miss","produce","consume","uuid-miss"},*firstModes[]={"consume","uuid-miss","cold-miss"};
    const unsigned cases=first?3:4;
    for(unsigned i=0;i<cases;++i) {
        const char *mode=first?firstModes[i]:coldModes[i];const auto &directory=first && i==2?empty:cache;
        char *arguments[]={const_cast<char *>("compiler-worker"),const_cast<char *>(driverPath),const_cast<char *>(directory.c_str()),const_cast<char *>(mode),nullptr};
        pid_t child=0;int error=posix_spawn(&child,"/private/var/tmp/compiler-worker",&actions,nullptr,arguments,environ);
        result[12+3*i]=child;result[13+3*i]=error;if(error || child<=1)stop(10+i);
        int status=0;while(waitpid(child,&status,0)<0)if(errno!=EINTR)stop(20+i);
        result[14+3*i]=status;if(!WIFEXITED(status) || WEXITSTATUS(status)!=0)stop(30+i);result[5]=i+1;
    }
    posix_spawn_file_actions_destroy(&actions);
    // A diagnostic duplicate of the sole actual driver record, not a Metal
    // binary archive. The full directory is independently hashed by summarize.
    if(!first) {
        std::error_code ec;std::filesystem::path record;unsigned files=0;
        std::filesystem::recursive_directory_iterator iterator(cache,ec),end;if(ec)stop(98);
        for(;iterator!=end;iterator.increment(ec)) {
            if(ec)stop(99);const auto status=iterator->symlink_status(ec);
            if(ec || std::filesystem::is_symlink(status))stop(100);
            if(std::filesystem::is_directory(status))continue;
            if(!std::filesystem::is_regular_file(status))stop(101);
            record=iterator->path();++files;
        }
        if(ec || files!=1 || !std::filesystem::copy_file(record,copy,std::filesystem::copy_options::none,ec) || ec)stop(102);
    }
    summarize();
    if(first) {
        if(memcmp(initial.data(),snapshot,initial.size()))stop(103);
        memcpy(snapshot+256,initial.data(),initial.size());result[6]=384;result[45]=2;result[46]=1;
    }
    stop(0);
}
