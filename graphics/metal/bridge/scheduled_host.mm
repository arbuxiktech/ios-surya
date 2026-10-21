// SPDX-License-Identifier: MIT
// A blocked completed handler must not hold waitUntilScheduled. No hardware timing claim:
// the semaphores create a deterministic distinction between scheduling and completion.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <atomic>
#include <cstdio>
#include <initializer_list>
#include "../NativeBridgeMetalFrontend.h"

static dispatch_time_t deadline() {return dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC);}
__attribute__((noinline)) static id nothing() {return nil;}
@protocol CompletionQueue
- (void)setCompletionQueue:(dispatch_queue_t)queue;
@end

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    @autoreleasepool {
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        if (!device) return 2;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        if (!queue || !buffer) return 2;
        for (unsigned variant : {0u, 1u, 2u, 3u}) {
            const bool refused = variant == 1 || variant == 3;
            [(id<CompletionQueue>)queue setCompletionQueue:variant == 2 ? dispatch_queue_create("scheduled-test", DISPATCH_QUEUE_SERIAL) : nil];
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            auto entered = dispatch_semaphore_create(0), release = dispatch_semaphore_create(0);
            auto returned = dispatch_semaphore_create(0);
            std::atomic<unsigned> scheduledCount{0}, completedCount{0};
            auto scheduled = &scheduledCount, completed = &completedCount;
            [cb addScheduledHandler:^(id<MTLCommandBuffer> command) {
                // This also catches callbacks deferred to Completed by the frontend.
                if (command.status == MTLCommandBufferStatusScheduled) ++*scheduled;
            }];
            [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
                ++*completed;
                dispatch_semaphore_signal(entered);
                dispatch_semaphore_wait(release, deadline());
            }];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            if (variant == 1) {
                // A frontend encoding failure has no scheduling event to announce.
                [blit fillBuffer:(id<MTLBuffer>)nothing() range:NSMakeRange(0, 16) value:0];
            } else {
                [blit fillBuffer:buffer range:NSMakeRange(0, 256) value:0x5a];
            }
            if (variant != 3) [blit endEncoding];   // variant 3: runtime refuses commit with an encoder open
            [cb commit];
            if (dispatch_semaphore_wait(entered, deadline())) {std::puts("FAIL completion never arrived");return 1;}
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
                [cb waitUntilScheduled];
                dispatch_semaphore_signal(returned);
            });
            const bool independent = dispatch_semaphore_wait(returned, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC)) == 0;
            dispatch_semaphore_signal(release);
            if (!independent && dispatch_semaphore_wait(returned, deadline())) {std::puts("FAIL scheduled wait stuck");return 1;}
            [cb waitUntilCompleted];
            if (variant == 3) [blit endEncoding];
            if (!independent || completedCount != 1 || scheduledCount != (refused ? 0u : 1u) ||
                cb.status != (refused ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted)) {
                std::printf("FAIL variant=%u independent=%d scheduled=%u completed=%u status=%lu\n",
                    variant, independent, scheduledCount.load(), completedCount.load(), (unsigned long)cb.status);
                return 1;
            }
        }
        std::puts("PASS: scheduling precedes completion on runtime/serial queues; scheduled waits ignore blocked completed handlers; frontend/runtime refusals announce no scheduling");
    }
}
