#pragma once

#import <Metal/Metal.h>

#include <cstdint>

// KYTY_GPU_PASS_TIME=1: GPU timestamps at the start and end of every render / compute encoder (counter sample buffers at
// stage boundaries), summed per pass label and reported every ~3 s as the top passes by share of GPU time ("[GpuPass]").
namespace Libs::Graphics::Metal {

[[nodiscard]] bool PassTimerEnabled();
// Attach timestamps to a render pass about to be encoded into `cmd`.
void PassTimerAttachRender(id<MTLCommandBuffer> cmd, MTLRenderPassDescriptor* desc);
// A compute pass descriptor with timestamps for `cmd`, or nil (encode without one).
[[nodiscard]] MTLComputePassDescriptor* PassTimerComputeDescriptor(id<MTLCommandBuffer> cmd);
// The name of the draw / dispatch being prepared (shader hash, target): the pass that executes it takes the name of its first item.
void PassTimerNote(const char* label);
// One draw / dispatch encoded into the current pass.
void PassTimerItem();
// Hand the samples of `cmd` to its completion handler; call right before `cmd` is committed.
void PassTimerOnCommit(id<MTLCommandBuffer> cmd);

} // namespace Libs::Graphics::Metal
