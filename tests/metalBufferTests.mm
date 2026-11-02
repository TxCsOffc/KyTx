// Standalone regression: queued GPU reads must survive transient-buffer rollover/reset.
#include "graphics/host_gpu/metal/metalBuffer.h"
#include "graphics/host_gpu/metal/metalContext.h"
#include "common/logging/log.h"
#include <cstdio>
#include <cstring>

namespace Libs::Graphics::Metal {
MetalContext::MetalContext() { m_device = MTLCreateSystemDefaultDevice(); }
MetalContext::~MetalContext() = default;
MetalContext& MetalContext::Instance() { static MetalContext context; return context; }
}
namespace Log {
bool IsSilent() { return false; }
void Write(fmt::text_style, std::string_view text) { std::fprintf(stderr, "%.*s", int(text.size()), text.data()); }
}

int main() {
	@autoreleasepool {
		using namespace Libs::Graphics::Metal;
		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		if (!device) return 1;
		MetalDynamicRingBuffer ring(64);
		if (!ring.Initialize()) return 1;
		id<MTLCommandQueue> queue = [device newCommandQueue];
		id<MTLCommandBuffer> command = [queue commandBuffer];
		id<MTLBuffer> result = [device newBufferWithLength:192 options:MTLResourceStorageModeShared];
		const uint8_t values[] = {0x5a, 0xa5, 0x3c};
		for (unsigned i = 0; i < 3; ++i) {
			if (i == 2) ring.Reset();
			auto allocation = ring.Allocate(64, 16);
			if (!allocation.IsValid()) return 1;
			std::memset(allocation.cpu_addr, values[i], 64);
			id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
			[encoder copyFromBuffer:allocation.buffer sourceOffset:allocation.offset toBuffer:result destinationOffset:i * 64 size:64];
			[encoder endEncoding];
		}
		// Deliberately submit only after all CPU writes and the reset.
		[command commit];
		[command waitUntilCompleted];
		unsigned failures = command.status == MTLCommandBufferStatusError;
		for (unsigned i = 0; i < 3; ++i) {
			const auto* bytes = static_cast<const uint8_t*>(result.contents) + i * 64;
			for (unsigned j = 0; j < 64; ++j) if (bytes[j] != values[i]) { ++failures; break; }
		}
		std::printf("Transient buffer queued reads: 3 cases, %u failures\n", failures);

		// The guest reuses a 512-KiB upload ring before the queued GPU copy executes.
		// Verify the actual production snapshot at a nonzero buffer offset, including
		// survival of transient reset. Keep a live-read control to demonstrate reuse.
		constexpr size_t upload_bytes = 512 * 1024;
		constexpr size_t upload_offset = 256;
		id<MTLBuffer> upload = [device newBufferWithLength:upload_bytes + upload_offset options:MTLResourceStorageModeShared];
		id<MTLBuffer> readback = [device newBufferWithLength:2 * upload_bytes options:MTLResourceStorageModeShared];
		auto* source = static_cast<uint8_t*>(upload.contents) + upload_offset;
		for (size_t i = 0; i < upload_bytes; ++i) source[i] = static_cast<uint8_t>((i * 13 + i / 4096) & 255);
		MetalBufferAllocation input {upload, upload_offset, upload_bytes, source, upload.gpuAddress + upload_offset};
		auto& manager = MetalBufferManager::Instance();
		auto snapshot = manager.SnapshotReadOnly(input, upload_bytes);
		if (!snapshot.IsValid()) return 1;
		unsigned snapshot_failures = 0;
		if (manager.SnapshotReadOnly(input, upload_bytes + 1).IsValid()) ++snapshot_failures;
		if (manager.SnapshotReadOnly(input, 0).IsValid()) ++snapshot_failures;
		if (manager.SnapshotReadOnly({}, 16).IsValid()) ++snapshot_failures;
		auto bad_extent = input;
		bad_extent.offset = upload.length - 16;
		if (manager.SnapshotReadOnly(bad_extent, 32).IsValid()) ++snapshot_failures;
		bad_extent.offset = upload.length + 1;
		if (manager.SnapshotReadOnly(bad_extent, 16).IsValid()) ++snapshot_failures;
		id<MTLCommandBuffer> snapshot_command = [queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [snapshot_command blitCommandEncoder];
		[blit copyFromBuffer:snapshot.buffer sourceOffset:snapshot.offset toBuffer:readback destinationOffset:0 size:upload_bytes];
		[blit copyFromBuffer:upload sourceOffset:upload_offset toBuffer:readback destinationOffset:upload_bytes size:upload_bytes];
		[blit endEncoding];
		manager.ResetTransientBuffers();
		std::memset(source, 0xa7, upload_bytes);
		[snapshot_command commit];
		[snapshot_command waitUntilCompleted];
		if (snapshot_command.status == MTLCommandBufferStatusError) ++snapshot_failures;
		const auto* actual = static_cast<const uint8_t*>(readback.contents);
		for (size_t i = 0; i < upload_bytes; ++i) {
			if (actual[i] != static_cast<uint8_t>((i * 13 + i / 4096) & 255) || actual[upload_bytes + i] != 0xa7) {
				++snapshot_failures;
				break;
			}
		}
		std::printf("512-KiB CPU upload snapshot + live control, reset, 5 bounds checks: %u failures\n", snapshot_failures);
		failures += snapshot_failures;
		return failures ? 1 : 0;
	}
}
