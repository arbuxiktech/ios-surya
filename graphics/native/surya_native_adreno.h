/* SPDX-License-Identifier: MIT */
/*
 * NativeAdreno IOKit Driver for Xiaomi Poco X3 NFC (surya, SM7150 / Adreno 618).
 * 1:1 UniXNU architectural adherence: publishes AGXAccelerator (IOAcceleratorES)
 * and AGXDeviceUserClient implementing the 16 ABI wire methods from NativeAdrenoABI.h.
 */
#ifndef _SURYA_NATIVE_ADRENO_H
#define _SURYA_NATIVE_ADRENO_H

#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_SURYA) || defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include "SuryaNativeAdrenoABI.h"

#define SURYA_ADRENO_MAX_BOS 1024
#define SURYA_ADRENO_MAX_QUEUES 8

class AGXAccelerator;
class AGXDeviceUserClient;

struct surya_adreno_bo {
    uint64_t handle;
    uint64_t iova;
    uint64_t bytes;
    IOMemoryDescriptor *desc;
    bool mapped;
    bool imported;
    bool used;
};

class IOAcceleratorES : public IOService {
    OSDeclareDefaultStructors(IOAcceleratorES);
};

class AGXAccelerator : public IOAcceleratorES {
    OSDeclareDefaultStructors(AGXAccelerator);

    IOLock *fLock;

public:
    bool init(OSDictionary *dictionary = NULL) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
        OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
    OSObject *copyProperty(const char *key) const APPLE_KEXT_OVERRIDE;
    OSObject *copyProperty(const OSSymbol *key) const APPLE_KEXT_OVERRIDE;
    OSObject *copyProperty(const OSString *key) const APPLE_KEXT_OVERRIDE;
};

class AGXDeviceUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AGXDeviceUserClient);

    task_t fTask;
    AGXAccelerator *fOwner;
    IOLock *fLock;
    surya_adreno_bo fBOs[SURYA_ADRENO_MAX_BOS];
    uint32_t fNextHandle;
    char fProcessName[20];
    char fMetadata[NativeAdrenoAbiMetadataBytes];
    uint32_t fMetadataBytes;

public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
        OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;

    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
        IOExternalMethodDispatch *dispatch, OSObject *target,
        void *reference) APPLE_KEXT_OVERRIDE;

    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
        IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;

private:
    surya_adreno_bo *findBo(uint64_t handle);
    surya_adreno_bo *allocBoEntry(void);
    void freeBoEntry(surya_adreno_bo *bo);
};

#endif /* ARM64_BOARD_CONFIG_SURYA || SWEET */
#endif /* _SURYA_NATIVE_ADRENO_H */
