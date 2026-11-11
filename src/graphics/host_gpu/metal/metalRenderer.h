#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALRENDERER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALRENDERER_H_

#include "common/common.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/metal/metalBuffer.h"
#include "graphics/host_gpu/metal/metalContext.h"
#include "graphics/host_gpu/metal/metalDraw.h"
#include "graphics/host_gpu/metal/metalPipeline.h"
#include "graphics/host_gpu/metal/metalShader.h"
#include "graphics/host_gpu/metal/metalTexture.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace HW {
class Shader;
class UserConfig;
} // namespace HW

namespace Libs::Graphics {
class PipelineCache;
namespace Pm4 { struct AcquireMemInfo; }
} // namespace Libs::Graphics

namespace Libs::Graphics::Metal {

class MetalRenderer {
public:
	static MetalRenderer& Instance();

	MetalRenderer();
	~MetalRenderer();

	KYTY_CLASS_NO_COPY(MetalRenderer);

	bool Initialize();
	void Shutdown();
	[[nodiscard]] bool IsActive() const noexcept { return m_active; }

	// Draw and Compute commands directly called from PM4 command processing
	void DrawIndex(uint64_t submit_id, const HW::Context& context, const HW::Shader& sh_ctx,
	               const HW::UserConfig& ucfg, PipelineCache& pipeline_cache,
	               Prospero::PrimitiveType prim_type,
	               uint32_t index_count, const void* index_addr, uint32_t instance_count,
	               uint32_t index_type_and_size, int32_t base_vertex, uint32_t first_instance);

	void DrawAuto(uint64_t submit_id, const HW::Context& context, const HW::Shader& sh_ctx,
	              const HW::UserConfig& ucfg, PipelineCache& pipeline_cache,
	              Prospero::PrimitiveType prim_type,
	              uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
	              uint32_t first_instance);

	void DispatchDirect(uint64_t submit_id, const HW::Context& context, const HW::Shader& sh_ctx,
	                    PipelineCache& pipeline_cache, uint32_t thread_group_x,
	                    uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode);

	void DispatchIndirect(uint64_t submit_id, const HW::Context& context, const HW::Shader& sh_ctx,
	                      PipelineCache& pipeline_cache, uint64_t args_addr,
	                      uint32_t mode);

	struct LastRenderTarget {
		uint64_t addr = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t format = 0;
	};
	[[nodiscard]] LastRenderTarget GetLastRenderTarget() const;

	[[nodiscard]] void* GetLastRenderTexture() const;

	// Most recently drawn displayable surface with the given Metal pixel format.
	[[nodiscard]] void* GetRenderTextureForFormat(uint32_t format) const;

	// Color surface whose guest memory starts at `addr`.
	[[nodiscard]] void* GetRenderTextureForAddress(uint64_t addr) const;
	[[nodiscard]] void* GetRenderTextureForAddressFormat(uint64_t addr, uint32_t format) const;

	struct TrackedRenderTarget {
		uint64_t addr   = 0;
		uint32_t width  = 0;
		uint32_t height = 0;
		uint32_t format = 0;
		void*    texture = nullptr;
	};
	[[nodiscard]] std::vector<TrackedRenderTarget> GetTrackedRenderTargets() const;

	void Flush();
	void CaptureFrameTick(uint64_t frame);
	void WaitIdle();
	// GPU execution boundary only: copied-image writeback is a separate
	// coherence operation. This never submits or waits on the CPU.
	void MemoryBarrier(const Pm4::AcquireMemInfo* acquire = nullptr);
	// GDS copies run under the encoding lock (Present flushes from another thread).
	void GdsDmaGuarded(bool dst_gds, bool src_gds, uint64_t dst, uint64_t src, uint32_t bytes);
	void GdsFillGuarded(uint64_t dst_offset, uint32_t bytes, uint32_t value);

private:
	// Both packet types must resolve the current shader and bind its resources.
	// Nonzero args_addr selects GPU-produced dispatch dimensions.
	void DispatchCompute(uint64_t submit_id, const HW::Context& context, const HW::Shader& sh_ctx,
	                     PipelineCache& pipeline_cache, uint32_t group_x, uint32_t group_y,
	                     uint32_t group_z, uint32_t mode, uint64_t args_addr);
	std::recursive_mutex m_mutex;
	bool m_active = false;
};

// Compute storage images, keyed by their guest base address (the presenter resolves the flip
// address through this when no render target covers it).
// GPU-side indirect draw: the command processor sets the guest address of the draw arguments
// around DrawIndexAuto for DRAW_INDIRECT; DrawAuto consumes it (TakePendingIndirectDraw).
// count_addr != 0 marks a multi-draw packet: max_count entries of `stride` bytes at args_addr, with the
// draw count read on the GPU from count_addr (0 = no count, draw max_count).
struct PendingIndirectDraw {
	uint64_t args_addr  = 0;
	uint64_t count_addr = 0;
	uint32_t max_count  = 1;
	uint32_t stride     = 16;
	bool     multi      = false; // a multi-draw packet (count_addr may be 0: then max_count is the count)
	bool     indexed    = false; // indexed multi-draw: index buffer at index_base (index_bytes long)
	uint64_t index_base  = 0;
	uint64_t index_bytes = 0;
	uint32_t index_type  = 0;    // 0 = u16, 1 = u32
};
void SetPendingIndirectDraw(PendingIndirectDraw pending);
[[nodiscard]] PendingIndirectDraw TakePendingIndirectDraw();

// Dispatch accounting per guest flip: kinds are 0 = CP dispatch packet, 1 = renderer entry, 2 = encoded
// (dispatchThreadgroups issued), 3 = no pipeline, 4 = no compute encoder, 5 = empty grid.
void CountDispatchStat(int kind);

// Per-flip draw accounting (submitted / executed / dropped by stage), printed every 25 flips.
void MetalDrawFrameStatsTick(uint64_t flip);

void RegisterComputeWrittenImage(uint64_t addr, void* texture);
[[nodiscard]] void* GetComputeWrittenImageForAddress(uint64_t addr);

// Monotonic count of bind/draw operations, used by debug instrumentation to gate
// experiments to late frames.
[[nodiscard]] uint64_t MetalDrawSequence();

// Check if native Metal backend is enabled (e.g. KYTY_BACKEND=metal or KYTY_USE_METAL=1)
[[nodiscard]] bool UseNativeMetalBackend() noexcept;

// Global data share, as seen by compute kernels (ds_append/ds_consume counters). The guest moves counters between GDS and
// memory with DMA_DATA packets; the copies are ordered with the GPU work on the Metal queue.
void GdsDma(bool dst_gds, bool src_gds, uint64_t dst, uint64_t src, uint32_t bytes);
void GdsFill(uint64_t dst_offset, uint32_t bytes, uint32_t value);


// Flip-buffer write tracing (KYTY_TRACE_FLIP_WRITES=1). Ranges come from VideoOut flip
// registration (swapchain PrepareFrame); TraceFlipOverlap logs any op whose guest range
// overlaps a registered flip buffer.
void RegisterFlipBuffer(uint64_t addr, uint64_t bytes);
void TraceFlipOverlap(const char* op, uint64_t addr, uint64_t bytes);

} // namespace Libs::Graphics::Metal

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALRENDERER_H_
