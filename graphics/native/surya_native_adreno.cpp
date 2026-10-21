/* SPDX-License-Identifier: MIT */
/*
 * NativeAdreno IOKit Driver for Xiaomi Poco X3 NFC (surya, SM7150 / Adreno 618).
 * 1:1 UniXNU architectural adherence: publishes AGXAccelerator (IOAcceleratorES)
 * and AGXDeviceUserClient implementing the 16 ABI wire methods from NativeAdrenoABI.h.
 */
#include "SuryaNativeAdreno.h"

#if defined(ARM64_BOARD_CONFIG_SURYA) || defined(SWEET)
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <string.h>

extern "C" {
int surya_adreno_is_ready(void);
int surya_adreno_smmu_map(uint64_t iova, uint64_t pa, size_t bytes, uint32_t perms);
int surya_adreno_smmu_unmap(uint64_t iova, size_t bytes);
int surya_adreno_cp_submit(uint64_t ib_iova, uint32_t dwords, uint64_t *fence);
int surya_adreno_cp_wait_fence(uint64_t fence, uint64_t timeout_ns);
uint64_t surya_adreno_cp_get_timestamp(void);
void surya_adreno_get_device_info(void *info);
void proc_selfname(char *buf, int size);
}

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);
OSDefineMetaClassAndStructors(AGXAccelerator, IOAcceleratorES);
OSDefineMetaClassAndStructors(AGXDeviceUserClient, IOUserClient);

/* ---------------- AGXAccelerator implementation ---------------- */

bool
AGXAccelerator::init(OSDictionary *dict)
{
    if (!IOAcceleratorES::init(dict)) return false;
    fLock = IOLockAlloc();
    return fLock != NULL;
}

bool
AGXAccelerator::start(IOService *provider)
{
    IOLog("SURYA: AGXAccelerator matching provider %s\n", provider ? provider->getName() : "null");
    if (!IOAcceleratorES::start(provider)) return false;

    if (!fLock) fLock = IOLockAlloc();

    /* Properties required for iOS 16 Metal framework discovery */
    setProperty("MetalPluginName", "NativeAdrenoMetal");
    setProperty("IOGLBundleName", "NativeAdrenoMetal");
    setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    setProperty("NativeSandboxGrantProcess", "backboardd");
    setProperty("IOGPUMaxSubmissions", 16ULL, 32);
    setProperty("device-id", 0x06010800ULL, 32);
    setProperty("vendor-id", 0x5143ULL, 32);
    setProperty("IOClass", "AGXAccelerator");
    setProperty("IOMatchCategory", "IOAccelerator");
    setProperty("compatible", "qcom,adreno");

    registerService();
    IOLog("SURYA: AGXAccelerator registered for Adreno 618 at /arm-io/gpu (MetalPlugin=NativeAdrenoMetal)\n");
    return true;
}

void
AGXAccelerator::stop(IOService *provider)
{
    if (fLock) {
        IOLockFree(fLock);
        fLock = NULL;
    }
    IOAcceleratorES::stop(provider);
}

IOReturn
AGXAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
    char caller[20] = {};
    proc_selfname(caller, sizeof(caller));
    IOLog("SURYA: AGXAccelerator::newUserClient by '%s' (type=%#x)\n", caller, (unsigned)type);

    AGXDeviceUserClient *client = new AGXDeviceUserClient;
    if (!client) return kIOReturnNoMemory;

    if (!client->initWithTask(owningTask, securityID, type, properties)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->attach(this)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }

    *handler = client;
    return kIOReturnSuccess;
}

OSObject *
AGXAccelerator::copyProperty(const char *key) const
{
    if (key && !strcmp(key, "MetalPluginName")) {
        char caller[20] = {};
        proc_selfname(caller, sizeof(caller));
        IOLog("SURYA: Metal discovery: '%s' queried MetalPluginName -> NativeAdrenoMetal\n", caller);
    }
    return IORegistryEntry::copyProperty(key);
}

OSObject *
AGXAccelerator::copyProperty(const OSSymbol *key) const
{
    if (key) return copyProperty(key->getCStringNoCopy());
    return NULL;
}

OSObject *
AGXAccelerator::copyProperty(const OSString *key) const
{
    if (key) return copyProperty(key->getCStringNoCopy());
    return NULL;
}

/* ---------------- AGXDeviceUserClient implementation ---------------- */

bool
AGXDeviceUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties)
{
    if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) return false;
    fTask = owningTask;
    fLock = IOLockAlloc();
    fNextHandle = 1;
    fMetadataBytes = 0;
    bzero(fBOs, sizeof(fBOs));
    bzero(fMetadata, sizeof(fMetadata));
    bzero(fProcessName, sizeof(fProcessName));
    proc_selfname(fProcessName, sizeof(fProcessName));
    return fLock != NULL;
}

bool
AGXDeviceUserClient::start(IOService *provider)
{
    if (!IOUserClient::start(provider)) return false;
    fOwner = OSDynamicCast(AGXAccelerator, provider);
    return fOwner != NULL;
}

void
AGXDeviceUserClient::stop(IOService *provider)
{
    IOUserClient::stop(provider);
}

IOReturn
AGXDeviceUserClient::clientClose(void)
{
    if (fLock) IOLockLock(fLock);
    for (size_t i = 0; i < SURYA_ADRENO_MAX_BOS; ++i) {
        if (fBOs[i].used) {
            if (fBOs[i].mapped) {
                surya_adreno_smmu_unmap(fBOs[i].iova, (size_t)fBOs[i].bytes);
                fBOs[i].mapped = false;
            }
            if (fBOs[i].desc) {
                fBOs[i].desc->complete();
                fBOs[i].desc->release();
                fBOs[i].desc = NULL;
            }
            fBOs[i].used = false;
        }
    }
    if (fLock) {
        IOLockUnlock(fLock);
        IOLockFree(fLock);
        fLock = NULL;
    }
    terminate();
    return kIOReturnSuccess;
}

surya_adreno_bo *
AGXDeviceUserClient::findBo(uint64_t handle)
{
    if (!handle) return NULL;
    for (size_t i = 0; i < SURYA_ADRENO_MAX_BOS; ++i) {
        if (fBOs[i].used && fBOs[i].handle == handle) return &fBOs[i];
    }
    return NULL;
}

surya_adreno_bo *
AGXDeviceUserClient::allocBoEntry(void)
{
    for (size_t i = 0; i < SURYA_ADRENO_MAX_BOS; ++i) {
        if (!fBOs[i].used) {
            fBOs[i].used = true;
            fBOs[i].handle = (uint64_t)fNextHandle++;
            fBOs[i].mapped = false;
            fBOs[i].imported = false;
            fBOs[i].iova = 0;
            fBOs[i].bytes = 0;
            fBOs[i].desc = NULL;
            return &fBOs[i];
        }
    }
    return NULL;
}

void
AGXDeviceUserClient::freeBoEntry(surya_adreno_bo *bo)
{
    if (!bo) return;
    if (bo->mapped) {
        surya_adreno_smmu_unmap(bo->iova, (size_t)bo->bytes);
        bo->mapped = false;
    }
    if (bo->desc) {
        bo->desc->complete();
        bo->desc->release();
        bo->desc = NULL;
    }
    bo->used = false;
    bo->handle = 0;
}

IOReturn
AGXDeviceUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
    IOMemoryDescriptor **memory)
{
    if (!fLock) return kIOReturnNotReady;
    IOLockLock(fLock);
    surya_adreno_bo *bo = findBo((uint64_t)type);
    if (!bo || !bo->desc) {
        IOLockUnlock(fLock);
        return kIOReturnNotFound;
    }
    if (options) *options = kIOMapAnywhere | kIOMapInhibitCache;
    bo->desc->retain();
    *memory = bo->desc;
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn
AGXDeviceUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
    if (!args) return kIOReturnBadArgument;
    if (!fLock) return kIOReturnNotReady;

    switch (selector) {
    case NativeAdrenoMethodQuery: {
        if (args->structureOutputSize < sizeof(NativeAdrenoAbiParams)) return kIOReturnBadArgument;
        NativeAdrenoAbiParams *rep = (NativeAdrenoAbiParams *)args->structureOutput;
        rep->abi_version = NativeAdrenoAbiVersion;
        rep->input_bits = 39;
        rep->output_bits = 36;
        rep->flags = NativeAdrenoParamsHostImport;
        rep->max_bo_bytes = UINT64_C(256) * 1024 * 1024;
        args->structureOutputSize = sizeof(NativeAdrenoAbiParams);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodBoAlloc: {
        if (args->structureInputSize < sizeof(NativeAdrenoAbiBoAlloc) ||
            args->structureOutputSize < sizeof(NativeAdrenoAbiBoAllocReply)) return kIOReturnBadArgument;
        const NativeAdrenoAbiBoAlloc *req = (const NativeAdrenoAbiBoAlloc *)args->structureInput;
        NativeAdrenoAbiBoAllocReply *rep = (NativeAdrenoAbiBoAllocReply *)args->structureOutput;
        if (!req->bytes || req->bytes > (UINT64_C(256) * 1024 * 1024)) return kIOReturnBadArgument;

        uint64_t alloc_bytes = (req->bytes + 4095ULL) & ~4095ULL;
        IOOptionBits opts = kIODirectionInOut;
        if (req->contiguous) opts |= kIOMemoryPhysicallyContiguous;

        IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::inTaskWithOptions(
            kernel_task, opts, (vm_size_t)alloc_bytes, 4096);
        if (!buf) return kIOReturnNoMemory;

        IOLockLock(fLock);
        surya_adreno_bo *bo = allocBoEntry();
        if (!bo) {
            IOLockUnlock(fLock);
            buf->release();
            return kIOReturnNoResources;
        }
        bo->bytes = alloc_bytes;
        bo->desc = buf;
        rep->handle = bo->handle;
        IOLockUnlock(fLock);
        args->structureOutputSize = sizeof(NativeAdrenoAbiBoAllocReply);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodBoMap: {
        if (args->structureInputSize < sizeof(NativeAdrenoAbiBoMap) ||
            args->structureOutputSize < sizeof(NativeAdrenoAbiBoMapReply)) return kIOReturnBadArgument;
        const NativeAdrenoAbiBoMap *req = (const NativeAdrenoAbiBoMap *)args->structureInput;
        NativeAdrenoAbiBoMapReply *rep = (NativeAdrenoAbiBoMapReply *)args->structureOutput;

        IOLockLock(fLock);
        surya_adreno_bo *bo = findBo(req->handle);
        if (!bo || !bo->desc) {
            IOLockUnlock(fLock);
            return kIOReturnNotFound;
        }

        IOByteCount seg_len = 0;
        addr64_t pa = bo->desc->getPhysicalSegment(0, &seg_len);
        if (!pa) {
            IOLockUnlock(fLock);
            return kIOReturnVMError;
        }

        int err = surya_adreno_smmu_map(req->iova, (uint64_t)pa, (size_t)bo->bytes, req->permissions);
        if (err) {
            IOLockUnlock(fLock);
            return kIOReturnIOError;
        }

        bo->iova = req->iova;
        bo->mapped = true;
        rep->iova = req->iova;
        IOLockUnlock(fLock);
        args->structureOutputSize = sizeof(NativeAdrenoAbiBoMapReply);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodBoFree: {
        if (args->structureInputSize < sizeof(NativeAdrenoAbiBoFree)) return kIOReturnBadArgument;
        const NativeAdrenoAbiBoFree *req = (const NativeAdrenoAbiBoFree *)args->structureInput;
        IOLockLock(fLock);
        surya_adreno_bo *bo = findBo(req->handle);
        if (!bo) {
            IOLockUnlock(fLock);
            return kIOReturnNotFound;
        }
        freeBoEntry(bo);
        IOLockUnlock(fLock);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodSubmit: {
        if (args->structureInputSize < sizeof(NativeAdrenoAbiSubmit) ||
            args->structureOutputSize < sizeof(NativeAdrenoAbiSubmitReply)) return kIOReturnBadArgument;
        const NativeAdrenoAbiSubmit *req = (const NativeAdrenoAbiSubmit *)args->structureInput;
        NativeAdrenoAbiSubmitReply *rep = (NativeAdrenoAbiSubmitReply *)args->structureOutput;
        if (!req->buffer_count || req->buffer_count > NativeAdrenoAbiMaxBuffers) return kIOReturnBadArgument;

        if (req->wait_fence) {
            surya_adreno_cp_wait_fence(req->wait_fence, req->wait_timeout_ns);
        }

        uint64_t fence = 0;
        for (uint32_t i = 0; i < req->buffer_count; ++i) {
            const NativeAdrenoAbiSubmitBuffer *b = &req->buffers[i];
            if (!b->bytes || (b->bytes & 3)) return kIOReturnBadArgument;
            uint32_t dwords = (uint32_t)(b->bytes / 4);
            int err = surya_adreno_cp_submit(b->iova, dwords, &fence);
            if (err) return kIOReturnIOError;
        }
        rep->fence = fence;
        args->structureOutputSize = sizeof(NativeAdrenoAbiSubmitReply);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodWaitFence: {
        if (args->structureInputSize < sizeof(NativeAdrenoAbiWaitFence)) return kIOReturnBadArgument;
        const NativeAdrenoAbiWaitFence *req = (const NativeAdrenoAbiWaitFence *)args->structureInput;
        int timeout = surya_adreno_cp_wait_fence(req->fence, req->timeout_ns);
        return timeout ? kIOReturnTimeout : kIOReturnSuccess;
    }

    case NativeAdrenoMethodGetTimestamp: {
        if (args->structureOutputSize < sizeof(NativeAdrenoAbiTimestamp)) return kIOReturnBadArgument;
        NativeAdrenoAbiTimestamp *rep = (NativeAdrenoAbiTimestamp *)args->structureOutput;
        rep->ticks = surya_adreno_cp_get_timestamp();
        args->structureOutputSize = sizeof(NativeAdrenoAbiTimestamp);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodGetStatus: {
        if (args->structureOutputSize < sizeof(NativeAdrenoAbiStatus)) return kIOReturnBadArgument;
        NativeAdrenoAbiStatus *rep = (NativeAdrenoAbiStatus *)args->structureOutput;
        rep->faults = 0;
        rep->suspends = 0;
        args->structureOutputSize = sizeof(NativeAdrenoAbiStatus);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodSetMetadata: {
        if (args->structureInputSize < sizeof(NativeAdrenoAbiMetadata)) return kIOReturnBadArgument;
        const NativeAdrenoAbiMetadata *req = (const NativeAdrenoAbiMetadata *)args->structureInput;
        IOLockLock(fLock);
        fMetadataBytes = req->bytes < sizeof(fMetadata) ? req->bytes : (uint32_t)sizeof(fMetadata);
        bcopy(req->text, fMetadata, fMetadataBytes);
        IOLockUnlock(fLock);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodGetMetadata: {
        if (args->structureOutputSize < sizeof(NativeAdrenoAbiMetadata)) return kIOReturnBadArgument;
        NativeAdrenoAbiMetadata *rep = (NativeAdrenoAbiMetadata *)args->structureOutput;
        IOLockLock(fLock);
        rep->bytes = fMetadataBytes;
        bcopy(fMetadata, rep->text, fMetadataBytes);
        IOLockUnlock(fLock);
        args->structureOutputSize = sizeof(NativeAdrenoAbiMetadata);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodCreateQueue: {
        if (args->structureOutputSize < sizeof(NativeAdrenoAbiQueueReply)) return kIOReturnBadArgument;
        NativeAdrenoAbiQueueReply *rep = (NativeAdrenoAbiQueueReply *)args->structureOutput;
        rep->queue = 1;
        args->structureOutputSize = sizeof(NativeAdrenoAbiQueueReply);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodCloseQueue: {
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodBoCpuMap: {
        if (args->structureInputSize < sizeof(NativeAdrenoAbiBoCpuMap) ||
            args->structureOutputSize < sizeof(NativeAdrenoAbiBoCpuMapReply)) return kIOReturnBadArgument;
        const NativeAdrenoAbiBoCpuMap *req = (const NativeAdrenoAbiBoCpuMap *)args->structureInput;
        NativeAdrenoAbiBoCpuMapReply *rep = (NativeAdrenoAbiBoCpuMapReply *)args->structureOutput;

        IOLockLock(fLock);
        surya_adreno_bo *bo = findBo(req->handle);
        if (!bo || !bo->desc) {
            IOLockUnlock(fLock);
            return kIOReturnNotFound;
        }
        rep->token = (uint32_t)bo->handle;
        rep->bytes = bo->bytes;
        rep->reserved = 0;
        IOLockUnlock(fLock);
        args->structureOutputSize = sizeof(NativeAdrenoAbiBoCpuMapReply);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodBoCpuUnmap: {
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodGetDeviceInfo: {
        if (args->structureOutputSize < sizeof(NativeAdrenoAbiDeviceInfo)) return kIOReturnBadArgument;
        NativeAdrenoAbiDeviceInfo *rep = (NativeAdrenoAbiDeviceInfo *)args->structureOutput;
        surya_adreno_get_device_info(rep);
        args->structureOutputSize = sizeof(NativeAdrenoAbiDeviceInfo);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodBoImport: {
        if (args->structureInputSize < sizeof(NativeAdrenoAbiBoImport) ||
            args->structureOutputSize < sizeof(NativeAdrenoAbiBoAllocReply)) return kIOReturnBadArgument;
        const NativeAdrenoAbiBoImport *req = (const NativeAdrenoAbiBoImport *)args->structureInput;
        NativeAdrenoAbiBoAllocReply *rep = (NativeAdrenoAbiBoAllocReply *)args->structureOutput;
        if (!req->bytes || (req->address & 4095ULL) || (req->bytes & 4095ULL)) return kIOReturnBadArgument;

        IOMemoryDescriptor *desc = IOMemoryDescriptor::withAddressRange(
            (mach_vm_address_t)req->address, (mach_vm_size_t)req->bytes,
            kIODirectionInOut, fTask);
        if (!desc) return kIOReturnNoMemory;

        IOReturn prep = desc->prepare();
        if (prep != kIOReturnSuccess) {
            desc->release();
            return prep;
        }

        IOLockLock(fLock);
        surya_adreno_bo *bo = allocBoEntry();
        if (!bo) {
            IOLockUnlock(fLock);
            desc->complete();
            desc->release();
            return kIOReturnNoResources;
        }
        bo->bytes = req->bytes;
        bo->desc = desc;
        bo->imported = true;
        rep->handle = bo->handle;
        IOLockUnlock(fLock);
        args->structureOutputSize = sizeof(NativeAdrenoAbiBoAllocReply);
        return kIOReturnSuccess;
    }

    default:
        return kIOReturnUnsupported;
    }
}

#endif /* ARM64_BOARD_CONFIG_SURYA || SWEET */
