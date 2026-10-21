// SPDX-License-Identifier: MIT
// Actual public Metal no-copy calls through the native bridge. Every buffer
// remains a distinct owner; count real Vulkan imports and check real GPU bytes.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "../NativeBridgeMetalFrontend.h"

constexpr unsigned workers=16;
struct State {std::array<std::atomic<unsigned>,workers> released{};std::atomic<unsigned> errors{0};};

static bool creation(id<MTLDevice> device,bool mixed,uint64_t (*imports)(),void (*delay)(unsigned)) {
    auto state=std::make_shared<State>();bool gpu=true;
    void *mapping=nullptr;size_t bytes=0;IOSurfaceRef surface=nullptr;
    uint64_t allocated=0;
    @autoreleasepool {
        if(mixed) {
            surface=IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@64,(id)kIOSurfaceHeight:@16,
                (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@256,(id)kIOSurfacePixelFormat:@((uint32_t)0x52474241)});
            if(!surface)return false;
            mapping=IOSurfaceGetBaseAddress(surface);bytes=IOSurfaceGetAllocSize(surface);
        } else {
            bytes=(size_t)getpagesize();mapping=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
            if(mapping==MAP_FAILED)return false;
        }
        if(!mapping || bytes<4096)return false;
        memset(mapping,0xcc,bytes);
        auto buffers=[NSMutableArray array],textures=[NSMutableArray array];
        std::mutex startLock;std::condition_variable ready;unsigned waiting=0;bool start=false;
        std::vector<std::thread> threads;
        const uint64_t before=imports();delay(25);
        for(unsigned i=0;i<workers;++i)threads.emplace_back([&,i]{
            {std::unique_lock<std::mutex> lock(startLock);++waiting;ready.notify_all();ready.wait(lock,[&]{return start;});}
            @autoreleasepool {
                if(mixed && (i&1)) {
                    auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:16 mipmapped:NO];
                    d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
                    id<MTLTexture> texture=[device newTextureWithDescriptor:d iosurface:surface plane:0];
                    if(!texture){state->errors++;return;}
                    @synchronized(textures){[textures addObject:texture];}
                } else {
                    auto owner=state;const unsigned ordinal=i;
                    id<MTLBuffer> buffer=[device newBufferWithBytesNoCopy:mapping length:bytes options:MTLResourceStorageModeShared
                        deallocator:^(void *,NSUInteger){owner->released[ordinal]++;}];
                    if(!buffer){state->errors++;return;}
                    @synchronized(buffers){[buffers addObject:buffer];}
                }
            }
        });
        {std::unique_lock<std::mutex> lock(startLock);ready.wait(lock,[&]{return waiting==workers;});start=true;ready.notify_all();}
        for(auto &thread:threads)thread.join();
        allocated=imports()-before;
        const unsigned expected=mixed?workers/2:workers;
        if(state->errors || buffers.count!=expected || textures.count!=(mixed?workers/2:0))return false;
        @autoreleasepool {
            // The API still returns distinct buffer objects and independent
            // callbacks; only the driver's import is shared.
            if([NSSet setWithArray:buffers].count!=expected)return false;
        }
        id<MTLCommandQueue> queue=[device newCommandQueue];
        id<MTLCommandBuffer> write=[queue commandBuffer];id<MTLBlitCommandEncoder> fill=[write blitCommandEncoder];
        [fill fillBuffer:buffers[0] range:NSMakeRange(0,4096) value:0x5a];[fill endEncoding];[write commit];[write waitUntilCompleted];
        // Use an explicit completed producer, so correctness here does not
        // assume implicit synchronization of arbitrary no-copy Metal aliases.
        id<MTLBuffer> output=[device newBufferWithLength:workers*4096 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> read=[queue commandBuffer];id<MTLBlitCommandEncoder> copy=[read blitCommandEncoder];
        if(!queue || !write || !fill || !output || !read || !copy)return false;
        NSUInteger at=0;
        for(id<MTLBuffer> buffer in buffers){[copy copyFromBuffer:buffer sourceOffset:0 toBuffer:output destinationOffset:at size:4096];at+=4096;}
        for(id<MTLTexture> texture in textures){
            [copy copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,16,1)
                        toBuffer:output destinationOffset:at destinationBytesPerRow:256 destinationBytesPerImage:4096];at+=4096;
        }
        [copy endEncoding];[read commit];[read waitUntilCompleted];
        gpu=write.status==MTLCommandBufferStatusCompleted && read.status==MTLCommandBufferStatusCompleted && at==workers*4096;
        for(unsigned i=0;i<workers*4096;++i)if(((const unsigned char *)output.contents)[i]!=0x5a){gpu=false;break;}
    }
    bool owners=false;
    for(unsigned attempt=0;attempt<300;++attempt) {
        owners=true;
        for(unsigned i=0;i<workers;++i)if(state->released[i].load()!=(mixed&&(i&1)?0u:1u))owners=false;
        if(owners)break;usleep(1000);
    }
    if(surface)CFRelease(surface);else munmap(mapping,bytes);
    std::printf("mode=%s buffers=%u vk_imports=%llu gpu=%s independent_owners=%s\n",mixed?"mixed":"raw",mixed?workers/2:workers,
        (unsigned long long)allocated,gpu?"pass":"FAIL",owners?"pass":"FAIL");
    return allocated==1 && gpu && owners;
}

static bool reentry(id<MTLDevice> device) {
    const size_t page=(size_t)getpagesize();void *mapping=mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(mapping==MAP_FAILED)return false;
    auto finished=dispatch_semaphore_create(0);auto state=std::make_shared<State>();
    @autoreleasepool {
        id<MTLBuffer> outer=[device newBufferWithBytesNoCopy:mapping length:page options:MTLResourceStorageModeShared
            deallocator:^(void *,NSUInteger){
                state->released[0]++;
                dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{
                    @autoreleasepool {
                        id<MTLBuffer> nested=[device newBufferWithBytesNoCopy:(char *)mapping+page length:page options:MTLResourceStorageModeShared
                            deallocator:^(void *,NSUInteger){state->released[1]++;}];
                        if(!nested)state->errors++;
                    }
                    dispatch_semaphore_signal(finished);
                });
                if(dispatch_semaphore_wait(finished,dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC)))state->errors++;
            }];
        if(!outer)return false;
    }
    const bool ok=!state->errors && state->released[0]==1 && state->released[1]==1;
    // On a timeout keep the test-owned mapping alive until process exit;
    // never turn a lifetime failure into a submitted dangling pointer.
    if(ok)munmap(mapping,page*2);
    std::printf("deallocator cross-thread reentry=%s\n",ok?"pass":"FAIL");return ok;
}

int main(int argc,char **argv) {
    if(argc!=4)return 2;
    @autoreleasepool {
        id<MTLDevice> device=native_bridge_metal_open(argv[1],argv[2],0x6e6d62,nullptr);if(!device)return 2;
        void *loader=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!loader)return 2;
        auto imports=(uint64_t (*)())dlsym(loader,"nmb_test_vk_host_import_count");
        auto delay=(void (*)(unsigned))dlsym(loader,"nmb_test_vk_host_import_delay");
        if(!imports || !delay)return 2;
        bool ok=false;
        if(!strcmp(argv[3],"raw"))ok=creation(device,false,imports,delay);
        else if(!strcmp(argv[3],"mixed"))ok=creation(device,true,imports,delay);
        else if(!strcmp(argv[3],"reentry"))ok=reentry(device);
        if(!ok)return 1;
        std::puts("PASS: native no-copy construction coalesces real imports while retaining independent objects, GPU bytes and callbacks");
    }
}
