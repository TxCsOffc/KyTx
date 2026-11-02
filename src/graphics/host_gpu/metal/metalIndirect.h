#pragma once

#import <Metal/Metal.h>
#include <cstdint>
#include <string>

namespace Libs::Graphics::Metal {

// Convert GPU-produced, nonindexed three-vertex RECTLIST draws into four-vertex
// triangle strips. Each output record contains the draw arguments followed by
// the original arguments for the vertex wrapper (buffer 26). No CPU readback.
class MetalRectListIndirect {
public:
	static constexpr NSUInteger RecordBytes = 32;
	static constexpr NSUInteger OriginalArgumentsOffset = 16;
	id<MTLBuffer> Encode(id<MTLCommandBuffer> command, id<MTLBuffer> source,
	                     NSUInteger offset, uint32_t count, uint32_t stride);

private:
	id<MTLComputePipelineState> m_pipeline = nil;
	id<MTLBuffer> m_output = nil;
};

// Snapshot GPU-produced compute arguments after their producer has executed.
// The first 12 bytes are Metal group counts, the original counts start at 16.
// Thread-dimension conversion uses the guest workgroup, before wave64 packing.
// The caller ends its active encoder first, then binds the consumer afresh.
class MetalComputeIndirect {
public:
	static constexpr NSUInteger OriginalArgumentsOffset = 16;
	id<MTLBuffer> Encode(id<MTLCommandBuffer> command, id<MTLBuffer> source,
	                     NSUInteger offset, MTLSize guest_threads, bool thread_dimensions,
	                     uint32_t max_groups);

private:
	id<MTLComputePipelineState> m_pipeline = nil;
	id<MTLBuffer> m_output = nil;
	id<MTLBuffer> m_guard = nil; // counts the dispatches the total-groups guard zeroed (see KytyPollIndirectGuard)
};

// Prepare multi-draw arguments in GPU order. Predicate by the GPU draw count,
// and bound indexed ranges to the index buffer actually bound by the consumer.
// Each 64-byte record holds Metal arguments at 0 and the original five dwords
// at 32, followed by the raw count, index limit, and indexed flag. CPU inspection
// is diagnostic only and requires command-buffer completion first.
class MetalMultiDrawIndirect {
public:
	static constexpr NSUInteger RecordBytes = 64;
	static constexpr NSUInteger OriginalArgumentsOffset = 32;
	id<MTLBuffer> Encode(id<MTLCommandBuffer> command, id<MTLBuffer> source, NSUInteger offset,
	                     id<MTLBuffer> count_buffer, NSUInteger count_offset,
	                     uint32_t max_count, uint32_t stride, bool indexed, uint32_t index_limit);

private:
	id<MTLComputePipelineState> m_pipeline = nil;
	id<MTLBuffer> m_output = nil;
};

// Text for the GPU watchdog: the compute-indirect snapshot and the first multi-draw records as the GPU last wrote them (shared buffers, read
// without waiting). Meant for a hung command buffer, where the stuck indirect op's arguments are the ones still in the shared buffer.
std::string KytyDescribeIndirectArguments();
// One line when the total-groups guard zeroed a dispatch since the last call (not thread-safe: one poller), empty otherwise.
std::string KytyPollIndirectGuard();
// Called right after MetalComputeIndirect::Encode for the same dispatch: which kernel it feeds, where its arguments live in guest memory, and
// which UMA surfaces (GPU-written images over guest memory) cover that address (`cover` may be empty).
void KytyNoteComputeIndirect(uint64_t hash, uint64_t addr, const char* cover);

} // namespace Libs::Graphics::Metal
