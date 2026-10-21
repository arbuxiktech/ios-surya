/* SPDX-License-Identifier: MIT */
/* Host-test Vulkan forwarding loader. Every request reaches the real driver;
 * count successful external-host-memory imports without changing any request,
 * code, driver identity, memory contents, fence or GPU result. Never deployed. */
#include <vulkan/vulkan.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <thread>

static PFN_vkGetInstanceProcAddr realInstance;
static PFN_vkGetDeviceProcAddr realDevice;
static std::once_flag initialized;
static std::atomic<uint64_t> imported;
static std::atomic<unsigned> importDelayMs;
static void initialize() {
    std::call_once(initialized,[]{
        const char *path=std::getenv("NMB_TEST_REAL_VULKAN_LOADER");
        void *library=path?dlopen(path,RTLD_NOW|RTLD_LOCAL):nullptr;
        if(!library)std::abort();
        realInstance=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vkGetInstanceProcAddr"));
        realDevice=reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(library,"vkGetDeviceProcAddr"));
        if(!realInstance || !realDevice)std::abort();
    });
}
static VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice device,const VkMemoryAllocateInfo *info,
                                              const VkAllocationCallbacks *allocator,VkDeviceMemory *memory) {
    initialize();
    bool hostImport=false;
    for(auto node=static_cast<const VkBaseInStructure *>(info->pNext);node;node=node->pNext)
        if(node->sType==VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT){hostImport=true;break;}
    // A bounded, one-shot delay makes a concurrent first-lookup miss
    // reproducible. The actual allocation and its result are unchanged.
    if(hostImport) {
        const unsigned delay=importDelayMs.exchange(0);
        if(delay)std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    auto real=reinterpret_cast<PFN_vkAllocateMemory>(realDevice(device,"vkAllocateMemory"));
    const VkResult result=real(device,info,allocator,memory);
    if(result==VK_SUCCESS && hostImport)imported++;
    return result;
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,const char *name) {
    initialize();
    if(!std::strcmp(name,"vkAllocateMemory"))return reinterpret_cast<PFN_vkVoidFunction>(allocate);
    return realDevice(device,name);
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,const char *name) {
    initialize();
    if(!std::strcmp(name,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if(!std::strcmp(name,"vkAllocateMemory"))return reinterpret_cast<PFN_vkVoidFunction>(allocate);
    return realInstance(instance,name);
}
extern "C" uint64_t nmb_test_vk_host_import_count() {return imported.load();}
extern "C" void nmb_test_vk_host_import_delay(unsigned milliseconds) {
    importDelayMs.store(milliseconds<=100?milliseconds:100);
}
