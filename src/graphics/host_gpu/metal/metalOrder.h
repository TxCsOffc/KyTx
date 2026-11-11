#pragma once

#import <Metal/Metal.h>
#include <cstdlib>
#include <mutex>

// Encoder fences (default on; KYTY_ENCODER_FENCES=0 turns them off): the encoders of the shared command stream run one after the other on
// the GPU, like the PS5 command processor runs its packets. Guest memory is reached through several Metal objects at once (a buffer per start page, buffer-backed texture views, raw GPU
// addresses inside argument buffers); Metal only orders encoders that use the same object, so a producer kernel and the kernel that reads
// its output can overlap (observed: indirect dispatch arguments read before the producer wrote them; a render pass over a linear render target
// writing into the memory of a compute queue buffer while the kernel still read it - the GPU hang of cs=0xf5875e8c tile queue, its per-tile
// counts turned into the target's half floats 0x38003800, um136 vs codex145-147). Each encoder updates a fence when it
// ends and the next one waits for it before its first command. No CPU wait.
//
// Call OrderBegin right after the encoder is created and OrderEnd right before endEncoding, for every encoder recorded into the shared
// command buffers (the helper buffers that commit and wait on their own need no fences).
namespace Libs::Graphics::Metal {

inline bool EncoderFencesEnabled() {
	static const bool on = std::getenv("KYTY_ENCODER_FENCES") == nullptr || std::getenv("KYTY_ENCODER_FENCES")[0] != '0';
	return on;
}

namespace OrderDetail {
struct State {
	std::mutex     mutex;
	id<MTLFence>   previous = nil;
	__strong id<MTLFence> ring[64] = {};
	unsigned       next = 0;
};
inline State& Get() {
	static State s;
	return s;
}
inline id<MTLFence> TakePrevious() {
	if (!EncoderFencesEnabled()) return nil;
	State& s = Get();
	std::lock_guard<std::mutex> lock(s.mutex);
	return s.previous;
}
inline id<MTLFence> NextFence(id<MTLDevice> device) {
	if (!EncoderFencesEnabled() || device == nil) return nil;
	State& s = Get();
	std::lock_guard<std::mutex> lock(s.mutex);
	__strong id<MTLFence>& slot = s.ring[s.next++ % 64];
	if (slot == nil) slot = [device newFence];
	s.previous = slot;
	return slot;
}
} // namespace OrderDetail

inline void OrderBegin(id<MTLComputeCommandEncoder> e) {
	if (id<MTLFence> f = OrderDetail::TakePrevious()) [e waitForFence:f];
}
inline void OrderBegin(id<MTLBlitCommandEncoder> e) {
	if (id<MTLFence> f = OrderDetail::TakePrevious()) [e waitForFence:f];
}
inline void OrderBegin(id<MTLRenderCommandEncoder> e) {
	if (id<MTLFence> f = OrderDetail::TakePrevious()) [e waitForFence:f beforeStages:MTLRenderStageVertex];
}
inline void OrderEnd(id<MTLComputeCommandEncoder> e) {
	if (id<MTLFence> f = OrderDetail::NextFence(e.device)) [e updateFence:f];
}
inline void OrderEnd(id<MTLBlitCommandEncoder> e) {
	if (id<MTLFence> f = OrderDetail::NextFence(e.device)) [e updateFence:f];
}
inline void OrderEnd(id<MTLRenderCommandEncoder> e) {
	if (id<MTLFence> f = OrderDetail::NextFence(e.device)) [e updateFence:f afterStages:MTLRenderStageFragment];
}

} // namespace Libs::Graphics::Metal
