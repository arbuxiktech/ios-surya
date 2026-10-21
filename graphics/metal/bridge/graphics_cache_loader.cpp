/* SPDX-License-Identifier: MIT */
/* Offline qualification only: forward to the actual Vulkan compiler, enabling
 * its reported cache-control feature and optionally forbidding compilation.
 * A negative control corrupts only a private copy of a cache UUID. Never deploy. */
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <vector>

static PFN_vkGetInstanceProcAddr realInstance;
static std::atomic<PFN_vkGetDeviceProcAddr> realDevice;
static std::once_flag initialized;
static std::atomic<VkInstance> activeInstance;
static std::atomic<bool> strict, badUuid;
// calls, success, compile-required, valid feedback, whole-cache hit, UUID mutations, devices,
// calls with an application cache, their valid feedback and their cache hits
static std::atomic<uint64_t> counts[10];
static void initialize() {
    std::call_once(initialized,[]{
        const char *path=std::getenv("NMB_TEST_REAL_VULKAN_LOADER");
        void *library=path?dlopen(path,RTLD_NOW|RTLD_LOCAL):nullptr;
        if(!library)std::abort();
        realInstance=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vkGetInstanceProcAddr"));
        if(!realInstance)realInstance=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vk_icdGetInstanceProcAddr"));
        if(!realInstance)std::abort();
    });
}
static VKAPI_ATTR VkResult VKAPI_CALL createDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo *info,
                                                   const VkAllocationCallbacks *allocator,VkDevice *device) {
    initialize();
    // The qualified runtime requests Vulkan 1.2. Refuse an unexpected chain
    // rather than duplicate a promoted feature or alter caller-owned structures.
    for(auto p=static_cast<const VkBaseInStructure *>(info->pNext);p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES ||
           p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES)return VK_ERROR_FEATURE_NOT_PRESENT;
    auto query=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(realInstance(activeInstance.load(),"vkGetPhysicalDeviceFeatures2"));
    if(!query)return VK_ERROR_FEATURE_NOT_PRESENT;
    VkPhysicalDevicePipelineCreationCacheControlFeatures supported={};supported.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES;
    VkPhysicalDeviceFeatures2 features={};features.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;features.pNext=&supported;
    query(physical,&features);if(!supported.pipelineCreationCacheControl)return VK_ERROR_FEATURE_NOT_PRESENT;
    std::vector<const char *> extensions;
    if(info->enabledExtensionCount)extensions.assign(info->ppEnabledExtensionNames,info->ppEnabledExtensionNames+info->enabledExtensionCount);
    bool listed=false;for(auto name:extensions)listed|=!std::strcmp(name,VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
    if(!listed)extensions.push_back(VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
    supported.pNext=const_cast<void *>(info->pNext);
    VkDeviceCreateInfo copy=*info;copy.pNext=&supported;copy.enabledExtensionCount=(uint32_t)extensions.size();copy.ppEnabledExtensionNames=extensions.data();
    auto real=reinterpret_cast<PFN_vkCreateDevice>(realInstance(activeInstance.load(),"vkCreateDevice"));
    if(!real)return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=real(physical,&copy,allocator,device);if(result==VK_SUCCESS)counts[6]++;
    return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL createCache(VkDevice device,const VkPipelineCacheCreateInfo *info,
                                                  const VkAllocationCallbacks *allocator,VkPipelineCache *cache) {
    VkPipelineCacheCreateInfo copy=*info;std::vector<unsigned char> bytes;
    if(badUuid.load() && info->initialDataSize) {
        if(info->initialDataSize<32)return VK_ERROR_INITIALIZATION_FAILED;
        const auto *source=static_cast<const unsigned char *>(info->pInitialData);
        bytes.assign(source,source+info->initialDataSize);bytes[16]^=1;copy.pInitialData=bytes.data();counts[5]++;
    }
    auto real=reinterpret_cast<PFN_vkCreatePipelineCache>(realDevice.load()(device,"vkCreatePipelineCache"));
    return real(device,&copy,allocator,cache);
}
static VKAPI_ATTR VkResult VKAPI_CALL createGraphics(VkDevice device,VkPipelineCache cache,uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos,const VkAllocationCallbacks *allocator,VkPipeline *pipelines) {
    if(!count || count>64)return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkGraphicsPipelineCreateInfo> copies(infos,infos+count);
    std::vector<VkPipelineCreationFeedback> feedback(count);
    std::vector<VkPipelineCreationFeedbackCreateInfo> additions(count);
    std::vector<const VkPipelineCreationFeedback *> observed(count);
    for(uint32_t i=0;i<count;++i) {
        if(strict.load())copies[i].flags|=VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        for(auto p=static_cast<const VkBaseInStructure *>(copies[i].pNext);p;p=p->pNext)
            if(p->sType==VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO)
                observed[i]=reinterpret_cast<const VkPipelineCreationFeedbackCreateInfo *>(p)->pPipelineCreationFeedback;
        if(!observed[i]) {
            auto &a=additions[i];a.sType=VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
            a.pNext=copies[i].pNext;a.pPipelineCreationFeedback=&feedback[i];copies[i].pNext=&a;observed[i]=&feedback[i];
        }
    }
    auto real=reinterpret_cast<PFN_vkCreateGraphicsPipelines>(realDevice.load()(device,"vkCreateGraphicsPipelines"));
    counts[0]+=count;if(cache)counts[7]+=count;const auto result=real(device,cache,count,copies.data(),allocator,pipelines);
    if(result==VK_PIPELINE_COMPILE_REQUIRED)counts[2]++;
    for(uint32_t i=0;i<count;++i)if(pipelines[i]) {
        counts[1]++;
        if(observed[i] && (observed[i]->flags&VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)) {
            counts[3]++;if(cache)counts[8]++;
            if(observed[i]->flags&VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT){counts[4]++;if(cache)counts[9]++;}
        }
    }
    return result;
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,const char *name) {
    initialize();
    if(!std::strcmp(name,"vkCreateGraphicsPipelines"))return reinterpret_cast<PFN_vkVoidFunction>(createGraphics);
    if(!std::strcmp(name,"vkCreatePipelineCache"))return reinterpret_cast<PFN_vkVoidFunction>(createCache);
    return realDevice.load()(device,name);
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,const char *name) {
    initialize();
    if(instance){
        activeInstance=instance;
        realDevice=reinterpret_cast<PFN_vkGetDeviceProcAddr>(realInstance(instance,"vkGetDeviceProcAddr"));
        if(!realDevice.load())std::abort();
    }
    if(!std::strcmp(name,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if(!std::strcmp(name,"vkCreateDevice"))return reinterpret_cast<PFN_vkVoidFunction>(createDevice);
    if(!std::strcmp(name,"vkCreateGraphicsPipelines"))return reinterpret_cast<PFN_vkVoidFunction>(createGraphics);
    if(!std::strcmp(name,"vkCreatePipelineCache"))return reinterpret_cast<PFN_vkVoidFunction>(createCache);
    return realInstance(instance,name);
}
extern "C" void nmb_test_graphics_cache_control(bool forbidCompilation,bool corruptUuid) {strict=forbidCompilation;badUuid=corruptUuid;}
extern "C" void nmb_test_graphics_cache_counts(uint64_t out[10]) {for(unsigned i=0;i<10;++i)out[i]=counts[i].load();}
