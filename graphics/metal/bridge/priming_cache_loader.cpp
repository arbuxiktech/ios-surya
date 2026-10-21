/* SPDX-License-Identifier: MIT */
/* Offline only. Preserve the existing graphics/UUID counters and forward real
 * calls, additionally forbid/observe compute and graphics during device open. */
#define vkGetInstanceProcAddr baselineGetInstanceProcAddr
#define vkGetDeviceProcAddr baselineGetDeviceProcAddr
#include "graphics_cache_loader.cpp"
#undef vkGetInstanceProcAddr
#undef vkGetDeviceProcAddr

static std::atomic<uint64_t> computeCounts[5]; // calls, successes, required, valid, app hits
static bool forceCompilationForbidden() {
    const char *value=std::getenv("NMB_TEST_FORCE_PRIME_STRICT");return value && !std::strcmp(value,"1");
}
static bool hasFlags2(const void *next) {
    for(auto p=static_cast<const VkBaseInStructure *>(next);p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR)return true;
    return false;
}
static VKAPI_ATTR VkResult VKAPI_CALL primeGraphics(VkDevice device,VkPipelineCache cache,uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos,const VkAllocationCallbacks *allocator,VkPipeline *pipelines) {
    if(!count || count>64)return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkGraphicsPipelineCreateInfo> copies(infos,infos+count);
    if(forceCompilationForbidden())for(auto &info:copies) {
        // Explicit refusal rather than silently missing flags promoted to pNext.
        if(hasFlags2(info.pNext))return VK_ERROR_FEATURE_NOT_PRESENT;
        info.flags|=VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    }
    return createGraphics(device,cache,count,copies.data(),allocator,pipelines);
}
static VKAPI_ATTR VkResult VKAPI_CALL primeCompute(VkDevice device,VkPipelineCache cache,uint32_t count,
    const VkComputePipelineCreateInfo *infos,const VkAllocationCallbacks *allocator,VkPipeline *pipelines) {
    if(!count || count>64)return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkComputePipelineCreateInfo> copies(infos,infos+count);
    std::vector<VkPipelineCreationFeedback> feedback(count);
    std::vector<VkPipelineCreationFeedbackCreateInfo> additions(count);
    std::vector<const VkPipelineCreationFeedback *> observed(count);
    for(uint32_t i=0;i<count;++i) {
        if(forceCompilationForbidden()) {
            if(hasFlags2(copies[i].pNext))return VK_ERROR_FEATURE_NOT_PRESENT;
            copies[i].flags|=VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        }
        for(auto p=static_cast<const VkBaseInStructure *>(copies[i].pNext);p;p=p->pNext)
            if(p->sType==VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO)
                observed[i]=reinterpret_cast<const VkPipelineCreationFeedbackCreateInfo *>(p)->pPipelineCreationFeedback;
        if(!observed[i]) {
            auto &a=additions[i];a.sType=VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
            a.pNext=copies[i].pNext;a.pPipelineCreationFeedback=&feedback[i];copies[i].pNext=&a;observed[i]=&feedback[i];
        }
    }
    auto real=reinterpret_cast<PFN_vkCreateComputePipelines>(realDevice.load()(device,"vkCreateComputePipelines"));
    computeCounts[0]+=count;const auto result=real(device,cache,count,copies.data(),allocator,pipelines);
    if(result==VK_PIPELINE_COMPILE_REQUIRED)computeCounts[2]++;
    for(uint32_t i=0;i<count;++i)if(pipelines[i]) {
        computeCounts[1]++;
        if(observed[i] && (observed[i]->flags&VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)) {
            computeCounts[3]++;
            if(observed[i]->flags&VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)computeCounts[4]++;
        }
    }
    return result;
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,const char *name) {
    if(!std::strcmp(name,"vkCreateGraphicsPipelines"))return reinterpret_cast<PFN_vkVoidFunction>(primeGraphics);
    if(!std::strcmp(name,"vkCreateComputePipelines"))return reinterpret_cast<PFN_vkVoidFunction>(primeCompute);
    return baselineGetDeviceProcAddr(device,name);
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,const char *name) {
    const auto result=baselineGetInstanceProcAddr(instance,name);
    if(!std::strcmp(name,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if(!std::strcmp(name,"vkCreateGraphicsPipelines"))return reinterpret_cast<PFN_vkVoidFunction>(primeGraphics);
    if(!std::strcmp(name,"vkCreateComputePipelines"))return reinterpret_cast<PFN_vkVoidFunction>(primeCompute);
    return result;
}
extern "C" void nmb_test_prime_compute_counts(uint64_t out[5]) {for(unsigned i=0;i<5;++i)out[i]=computeCounts[i].load();}
