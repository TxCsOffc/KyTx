#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALCONTEXT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALCONTEXT_H_

#include "common/common.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#else
typedef void* id;
#endif

namespace Libs::Graphics::Metal {

struct MetalDeviceCapabilities {
	std::string device_name;
	bool        has_unified_memory          = true;
	bool        supports_metal3             = false;
	bool        supports_metal4             = false;
	bool        supports_family_apple7      = false;
	bool        supports_family_apple8      = false;
	bool        supports_family_apple9      = false;
	bool        supports_family_apple10     = false;
	bool        supports_barycentrics       = false;
	bool        supports_argument_buffers_tier2 = false;
	uint32_t    max_threads_per_threadgroup = 1024;
	uint32_t    max_buffer_length_mb        = 0;
};

// Registers a command buffer that a helper commits and waits on itself with the GPU watchdog (call before commit); `what` names the path.
void KytyWatchCommandBuffer(id commandBuffer, const char* what);
// Submit through the shared host duty-cycle/memory budget, including helper queues.
void KytyBudgetedCommit(id commandBuffer);

class MetalContext {
public:
	static MetalContext& Instance();

	MetalContext();
	~MetalContext();

	KYTY_CLASS_NO_COPY(MetalContext);

	[[nodiscard]] bool Initialize();
	void               Shutdown();
	[[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }

#ifdef __OBJC__
	[[nodiscard]] id<MTLDevice>              GetDevice() const noexcept { return m_device; }
	[[nodiscard]] id<MTLCommandQueue>        GetCommandQueue() const noexcept { return m_command_queue; }
	[[nodiscard]] id<MTLCommandBuffer>       GetCurrentCommandBuffer();
	[[nodiscard]] id<MTLRenderCommandEncoder>  GetActiveRenderEncoder() const noexcept { return m_active_render_encoder; }
	[[nodiscard]] id<MTLComputeCommandEncoder> GetActiveComputeEncoder() const noexcept { return m_active_compute_encoder; }

	void SetActiveRenderEncoder(id<MTLRenderCommandEncoder> encoder) noexcept;
	void SetActiveComputeEncoder(id<MTLComputeCommandEncoder> encoder) noexcept;
#else
	[[nodiscard]] id GetDevice() const noexcept { return m_device; }
	[[nodiscard]] id GetCommandQueue() const noexcept { return m_command_queue; }
	[[nodiscard]] id GetCurrentCommandBuffer();
	[[nodiscard]] id GetActiveRenderEncoder() const noexcept { return m_active_render_encoder; }
	[[nodiscard]] id GetActiveComputeEncoder() const noexcept { return m_active_compute_encoder; }

	void SetActiveRenderEncoder(id encoder) noexcept;
	void SetActiveComputeEncoder(id encoder) noexcept;
#endif

	void EndCurrentEncoder();
	// Grows with every encoder the context starts. Residency declared on an encoder holds for that encoder only, so callers keying a
	// cache of "already declared" resources on it must drop the cache when this changes.
	[[nodiscard]] uint64_t EncoderSerial() const noexcept { return m_encoder_serial; }
	// A byte fill of guest memory requested while a render pass is open: it runs, in GPU order after that pass, in one blit encoder
	// when the pass ends, instead of splitting the pass after every draw (hundreds of 1920x1080 load/store round trips per frame).
	void QueueBlitFill(id buffer, uint64_t offset, uint64_t length, uint8_t value);
	// A whole-texture copy (slices [0, slices) and mips [0, levels)) requested while an encoder is open: it runs when that encoder ends
	// (a layered image grown during a storage binding receives its old slices this way; the open dispatch writes only new ones).
	void QueueTextureCopy(id src, id dst, uint32_t slices, uint32_t levels);
	// Materialize private image writes after the producing encoder and before
	// queued DMA fills/copies or any subsequent consumer. No CPU wait is needed.
	void QueueEncoderWriteback(std::function<void(id)> writeback, uint64_t key = 0);
	void CommitCommandBuffer(bool wait = false);
	void WaitUntilIdle();

	[[nodiscard]] const MetalDeviceCapabilities& GetCapabilities() const noexcept { return m_capabilities; }

private:
	void QueryCapabilities();

#ifdef __OBJC__
	id<MTLDevice>              m_device                 = nil;
	id<MTLCommandQueue>        m_command_queue          = nil;
	id<MTLCommandBuffer>       m_current_command_buffer = nil;
	// Bounds committed-but-unfinished command buffers; IOGPU blocks forever in
	// -[MTLCommandQueue commandBuffer] once its in-flight limit is reached.
	dispatch_semaphore_t       m_inflight_semaphore     = nullptr;
	id<MTLSharedEvent>         m_cb_order_event         = nil; // KYTY_CB_SERIAL: GPU-side order of command buffers
	uint64_t                   m_cb_order_value         = 0;
	static constexpr intptr_t  kMaxInflightCommandBuffers = 64;
	id<MTLRenderCommandEncoder>  m_active_render_encoder  = nil;
	id<MTLComputeCommandEncoder> m_active_compute_encoder = nil;
#else
	id m_device                 = nullptr;
	id m_command_queue          = nullptr;
	id m_current_command_buffer = nullptr;
	id m_active_render_encoder  = nullptr;
	id m_active_compute_encoder = nullptr;
#endif

	struct PendingBlitFill {
		id       buffer = nullptr;
		uint64_t offset = 0;
		uint64_t length = 0;
		uint8_t  value  = 0;
	};
	std::vector<PendingBlitFill> m_pending_fills;
	struct PendingTextureCopy {
		id       src = nullptr;
		id       dst = nullptr;
		uint32_t slices = 0;
		uint32_t levels = 0;
	};
	std::vector<PendingTextureCopy> m_pending_texture_copies;
	struct PendingWriteback { uint64_t key; std::function<void(id)> encode; };
	std::vector<PendingWriteback> m_pending_writebacks;
	uint64_t                     m_encoder_serial = 0;
	bool                         m_command_buffer_handed_out = false;

	MetalDeviceCapabilities m_capabilities {};
	std::mutex              m_mutex;
	bool                    m_initialized = false;
};

// KYTY_GPU_CAPTURE=<frame>: start a Metal GPU trace (device capture) at the start of that frame and
// stop it at the start of the next one. Writes <KYTY_GPU_CAPTURE_DIR or runs dir>/frame<N>.gputrace.
// Needs MTL_CAPTURE_ENABLED=1 in the environment of the process.
void GpuCaptureTick(uint64_t frame);
// Hotkey support (F9 in the game window): capture the next presented frame as a .gputrace (needs MTL_CAPTURE_ENABLED=1 at launch),
// and a short status string for the window title ("" when idle).
void        RequestGpuCapture();
std::string GpuCaptureTitleSuffix();
// Vulkan (MoltenVK) counterpart, driven by KYTY_VK_GPU_CAPTURE.
void VkGpuCaptureTick(uint64_t frame);

} // namespace Libs::Graphics::Metal

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALCONTEXT_H_
