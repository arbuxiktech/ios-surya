/* SPDX-License-Identifier: MIT */
/* Owned QEMU-only PID 1 harness. Run the actual iOS UI compiler in four cold
 * children, flush own mutable Data, then expose a bounded manifest checkpoint.
 * Original dyld/AMFI, signatures, read-only OS and exact RAM-volume selection
 * remain in force. The linked software peer executes no shader. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <CommonCrypto/CommonDigest.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

extern char **environ;
static uint64_t result[48];
alignas(4096) static unsigned char snapshot[4096];
alignas(4096) static unsigned char workerOutput[4096];
static const char *workerLog="/private/var/tmp/quartzcore-compiler-worker.log";
static const char *driverPath="/System/Library/Extensions/OfflineTurnipCompiler/libvulkan_freedreno.dylib";
static const char *prefix="/private/var/tmp/quartzcore-ui";

[[noreturn]] static void stop(unsigned failure) {
    result[1]=failure;
    if(FILE *file=fopen(workerLog,"rb")) {
        result[11]=fread(workerOutput,1,sizeof workerOutput-1,file);
        if(fgetc(file)!=EOF || ferror(file))result[11]=sizeof workerOutput;
        fclose(file);
    }
    __asm__ volatile("mov x13, %0\nmov x14, %1\nmov x15, %2\nbrk #0"::"r"(result),"r"(snapshot),"r"(workerOutput):"x13","x14","x15","memory");
    _exit(99);
}
static void mountCompilerView() {
    char uuid[37]={};FILE *file=fopen("/private/var/tmp/compiler-data-uuid","rb");
    if(!file)stop(60);
    const size_t count=fread(uuid,1,37,file);fclose(file);
    if(count!=36 || uuid[36])stop(61);
    CFStringRef identity=CFStringCreateWithCString(nullptr,uuid,kCFStringEncodingASCII);
    CFMutableDictionaryRef match=IOServiceMatching("IOMedia");if(!identity || !match)stop(62);
    const void *keys[]={CFSTR("UUID")},*values[]={identity};
    CFDictionaryRef properties=CFDictionaryCreate(nullptr,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if(!properties)stop(63);
    CFDictionarySetValue(match,CFSTR("IOPropertyMatch"),properties);CFRelease(properties);
    io_iterator_t iterator=0;if(IOServiceGetMatchingServices(0,match,&iterator) || !iterator)stop(64);
    char device[64]={};unsigned found=0;
    while(io_service_t media=IOIteratorNext(iterator)) {
        ++found;
        CFTypeRef observed=IORegistryEntryCreateCFProperty(media,CFSTR("UUID"),nullptr,0);
        CFTypeRef name=IORegistryEntryCreateCFProperty(media,CFSTR("BSD Name"),nullptr,0);
        if(!observed || !CFEqual(observed,identity) || !name || CFGetTypeID(name)!=CFStringGetTypeID())stop(65);
        char bsd[56]={};if(!CFStringGetCString((CFStringRef)name,bsd,sizeof bsd,kCFStringEncodingASCII) || strncmp(bsd,"disk",4))stop(66);
        for(const char *p=bsd;*p;++p)if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')))stop(67);
        bool backed=false;io_registry_entry_t node=media;
        for(unsigned depth=0;node && depth<64;++depth) {
            if(IOObjectConformsTo(node,"NativeRamBlock")){backed=true;break;}
            io_registry_entry_t above=0;auto error=IORegistryEntryGetParentEntry(node,"IOService",&above);
            if(node!=media)IOObjectRelease(node);node=error?0:above;
        }
        if(node && node!=media)IOObjectRelease(node);if(!backed)stop(68);
        snprintf(device,sizeof device,"/dev/%s",bsd);CFRelease(observed);CFRelease(name);IOObjectRelease(media);
    }
    IOObjectRelease(iterator);CFRelease(identity);if(found!=1 || !device[0])stop(69);
    const char *point="/System/Library/Extensions",*directory="/private/var/tmp/native-compiler-addons";
    struct stat block={},source={},before={},after={},original={},code={};struct statfs fs={};
    if(stat(device,&block) || !S_ISBLK(block.st_mode) || stat(point,&before) || stat(directory,&source) || source.st_dev!=block.st_rdev)stop(70);
    if(mount("bindfs",point,MNT_RDONLY|MNT_UNION,(void *)directory))stop(71);
    if(statfs(point,&fs) || (fs.f_flags&(MNT_RDONLY|MNT_UNION))!=(MNT_RDONLY|MNT_UNION) ||
       strcmp(fs.f_mntfromname,directory) || strcmp(fs.f_fstypename,"bindfs") || stat(point,&after) ||
       after.st_dev==before.st_dev || stat(driverPath,&code) || code.st_dev!=after.st_dev ||
       stat("/System/Library/Extensions/AGXMetalA10.bundle/Info.plist",&original) || original.st_dev!=before.st_dev)stop(72);
    result[38]=fs.f_flags;result[39]=(uint32_t)before.st_dev;result[40]=(uint32_t)after.st_dev;
    result[41]=(uint32_t)original.st_dev;result[42]=(uint32_t)code.st_dev;result[43]=found;result[44]=1;
}
static std::array<unsigned char,32> digestFile(const std::filesystem::path &path,uint64_t *length) {
    struct stat st={};if(lstat(path.c_str(),&st) || !S_ISREG(st.st_mode) || st.st_size<0 || st.st_size>16*1024*1024)stop(80);
    FILE *file=fopen(path.c_str(),"rb");if(!file)stop(81);
    CC_SHA256_CTX hash;CC_SHA256_Init(&hash);unsigned char buffer[16384];uint64_t total=0;
    for(;;){const size_t n=fread(buffer,1,sizeof buffer,file);if(n){CC_SHA256_Update(&hash,buffer,(CC_LONG)n);total+=n;}if(n<sizeof buffer)break;}
    const bool good=!ferror(file) && feof(file) && total==(uint64_t)st.st_size;fclose(file);if(!good)stop(82);
    std::array<unsigned char,32> digest={};CC_SHA256_Final(digest.data(),&hash);*length=total;return digest;
}
static void summarize() {
    const std::filesystem::path directory=std::string(prefix)+".cache",archive=std::string(prefix)+".archive";
    std::error_code ec;std::vector<std::string> names;
    std::filesystem::recursive_directory_iterator iterator(directory,std::filesystem::directory_options::none,ec),end;
    if(ec)stop(83);
    for(;iterator!=end;iterator.increment(ec)) {
        if(ec)stop(84);
        const auto status=iterator->symlink_status(ec);if(ec || std::filesystem::is_symlink(status))stop(85);
        if(std::filesystem::is_directory(status))continue;
        if(!std::filesystem::is_regular_file(status) || names.size()>=2048)stop(86);
        auto name=iterator->path().lexically_relative(directory).generic_string();
        if(name.empty() || name.size()>192 || name.find("..")!=std::string::npos)stop(87);
        for(unsigned char c:name)if(c<33 || c>126)stop(88);
        names.push_back(name);
    }
    if(ec || names.empty())stop(89);std::sort(names.begin(),names.end());
    CC_SHA256_CTX manifest;CC_SHA256_Init(&manifest);uint64_t total=0;
    for(const auto &name:names) {
        uint64_t size=0;const auto digest=digestFile(directory/name,&size);total+=size;if(total>64*1024*1024)stop(90);
        const uint32_t length=(uint32_t)name.size();CC_SHA256_Update(&manifest,&length,sizeof length);
        CC_SHA256_Update(&manifest,name.data(),length);CC_SHA256_Update(&manifest,&size,sizeof size);CC_SHA256_Update(&manifest,digest.data(),digest.size());
    }
    uint64_t archiveBytes=0,sourceBytes=0;const auto archiveDigest=digestFile(archive,&archiveBytes);
    const auto sourceDigest=digestFile("/System/Library/Frameworks/QuartzCore.framework/default.metallib",&sourceBytes);
    auto *words=reinterpret_cast<uint64_t *>(snapshot);words[0]=0x515455495041434bull;words[1]=archiveBytes;
    memcpy(snapshot+16,archiveDigest.data(),32);words[6]=names.size();words[7]=total;
    CC_SHA256_Final(snapshot+64,&manifest);memcpy(snapshot+96,sourceDigest.data(),32);
    result[6]=128;result[7]=sourceBytes;result[8]=names.size();result[9]=total;result[10]=archiveBytes;
    int fd=open(archive.c_str(),O_RDONLY);if(fd<0 || fsync(fd))stop(91);close(fd);
    sync();result[45]=1;result[47]=1;
}
int main(int argc,char **argv) {
    (void)argv;result[0]=0x515443504b494f31ull;result[2]=(uint64_t)getpid();result[3]=getpagesize();result[4]=sysconf(_SC_PAGESIZE);
    if(argc!=1 || getpid()!=1 || result[3]!=4096 || result[4]!=4096)stop(1);
    mountCompilerView();
    if(setenv("MESA_SHADER_CACHE_DISABLE","true",1) || unsetenv("DYLD_INSERT_LIBRARIES") || unsetenv("HOSTSIM_PAGE_SIZE") ||
       unsetenv("NMB_TEST_GRAPHICS_CACHE_LOADER") || unsetenv("NMB_TEST_QUARTZCORE_LIBRARY"))stop(2);
    // A fresh fixture must never reuse the earlier run's products.
    struct stat old={};if(!lstat((std::string(prefix)+".archive").c_str(),&old) || errno!=ENOENT)stop(3);
    if(unlink(workerLog) && errno!=ENOENT)stop(5);
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions) || posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,workerLog,O_WRONLY|O_CREAT|O_APPEND,0600) ||
       posix_spawn_file_actions_adddup2(&actions,STDOUT_FILENO,STDERR_FILENO))stop(4);
    const char *modes[]={"produce","consume","uuid-miss","empty-driver"};
    for(unsigned i=0;i<4;++i) {
        char *arguments[]={const_cast<char *>("compiler-worker"),const_cast<char *>(driverPath),const_cast<char *>(prefix),const_cast<char *>(modes[i]),nullptr};
        pid_t child=0;int error=posix_spawn(&child,"/private/var/tmp/compiler-worker",&actions,nullptr,arguments,environ);
        result[12+3*i]=(uint64_t)child;result[13+3*i]=(uint64_t)error;if(error || child<=1)stop(10+i);
        int status=0;while(waitpid(child,&status,0)<0)if(errno!=EINTR)stop(20+i);
        result[14+3*i]=(uint64_t)status;if(!WIFEXITED(status) || WEXITSTATUS(status)!=0)stop(30+i);result[5]=i+1;
    }
    posix_spawn_file_actions_destroy(&actions);summarize();stop(0);
}
