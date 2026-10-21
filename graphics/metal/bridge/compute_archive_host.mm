// SPDX-License-Identifier: MIT
// Separate processes/cache directories distinguish archive consumption from live memo hits.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "NativeMetalObjects.h"
#include "../NativeBridgeMetalFrontend.h"
#include <cstdio>
#include <cstring>

#define REQUIRE(condition) do {if (!(condition)) {std::printf("FAIL line %d: %s; %s\n", __LINE__, #condition, error.localizedDescription.UTF8String ?: "");return 1;}} while (0)

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    @autoreleasepool {
        NSError *error = nil;
        const bool produce = !strcmp(argv[4], "produce");
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        REQUIRE(device);
        size_t bytes = 0;
        const void *air = nmb_function_constant_air(&bytes);
        id<MTLLibrary> library = native_bridge_metal_library_with_air(air, bytes);
        REQUIRE(library);
        auto constants = [MTLFunctionConstantValues new];
        int mode = 2;
        [constants setConstantValue:&mode type:MTLDataTypeInt atIndex:0];
        id<MTLFunction> function = [library newFunctionWithName:@"fcfold" constantValues:constants error:&error];
        REQUIRE(function && !error);
        REQUIRE([device respondsToSelector:@selector(newComputePipelineStateWithDescriptor:options:reflection:error:)]);
        auto descriptor = [MTLComputePipelineDescriptor new];
        descriptor.computeFunction = function;
        descriptor.maxTotalThreadsPerThreadgroup = 32;
        descriptor.buffers[1].mutability = MTLMutabilityMutable;
        descriptor.label = @"archived compute";
        auto archiveDescriptor = [MTLBinaryArchiveDescriptor new];
        NSURL *url = [NSURL fileURLWithPath:@(argv[3])];
        if (!produce) archiveDescriptor.url = url;
        id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:archiveDescriptor error:&error];
        REQUIRE(archive && !error);
        descriptor.binaryArchives = @[archive];
        if (produce) {
            id<MTLComputePipelineState> miss = [device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error];
            if (miss || !error) {
                std::printf("FAIL strict empty archive: state=%s error=%s\n", miss ? "created" : "nil", error.localizedDescription.UTF8String ?: "nil");
                return 1;
            }
            REQUIRE([archive addComputePipelineFunctionsWithDescriptor:descriptor error:&error] && !error);
            REQUIRE([archive serializeToURL:url error:&error] && !error);
        }
        id<MTLComputePipelineState> state = [device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error];
        REQUIRE(state && !error && state.maxTotalThreadsPerThreadgroup == 32);
        REQUIRE([state.label isEqualToString:descriptor.label]);
        // Both descriptor fields and function constants must retain their archive identity.
        descriptor.maxTotalThreadsPerThreadgroup = 16;
        REQUIRE(![device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error] && error);
        descriptor.maxTotalThreadsPerThreadgroup = 32;
        descriptor.buffers[1].mutability = MTLMutabilityDefault;
        REQUIRE(![device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error] && error);
        descriptor.buffers[1].mutability = MTLMutabilityMutable;
        mode = 3;
        [constants setConstantValue:&mode type:MTLDataTypeInt atIndex:0];
        descriptor.computeFunction = [library newFunctionWithName:@"fcfold" constantValues:constants error:&error];
        REQUIRE(descriptor.computeFunction && !error);
        REQUIRE(![device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error] && error);
        descriptor.computeFunction = function;
        descriptor.stageInputDescriptor = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
        descriptor.stageInputDescriptor.attributes[0].format = MTLAttributeFormatFloat;
        // Unsupported descriptor state must be refused by both population and creation.
        REQUIRE(![archive addComputePipelineFunctionsWithDescriptor:descriptor error:&error] && error);
        REQUIRE(![device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionNone reflection:nil error:&error] && error);
        descriptor.stageInputDescriptor = nil;
        const float inputValue = 2;
        id<MTLBuffer> input = [device newBufferWithBytes:&inputValue length:4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> output = [device newBufferWithLength:128 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [[device newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        REQUIRE(input && output && encoder);
        [encoder setComputePipelineState:state];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        REQUIRE(command.status == MTLCommandBufferStatusCompleted);
        REQUIRE(((const float *)output.contents)[24] == 4000);
        // Check the no-list behavior only after the cold archive consumer and GPU verdict.
        REQUIRE([device newComputePipelineStateWithFunction:function options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error] && !error);
        std::printf("PASS: %s compute archive, strict descriptor misses and host GPU output\n", produce ? "produced" : "reloaded");
    }
}
