#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALDRAW_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALDRAW_H_

#include "common/common.h"
#include "graphics/shader/shader.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/metal/metalBuffer.h"
#include "graphics/host_gpu/metal/metalPipeline.h"
#include "graphics/host_gpu/metal/metalTexture.h"
#include "graphics/host_gpu/metal/metalPixelRect.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
typedef void* id;
typedef unsigned long NSUInteger;
enum MTLPrimitiveType : NSUInteger;
enum MTLIndexType : NSUInteger;
enum MTLLoadAction : NSUInteger;
enum MTLStoreAction : NSUInteger;
struct MTLViewport {
	double originX, originY, width, height, znear, zfar;
};
struct MTLScissorRect {
	NSUInteger x, y, width, height;
};
#endif

namespace Libs::Graphics::Metal {

MTLPrimitiveType TranslatePrimitiveType(Prospero::PrimitiveType type);
MTLIndexType     TranslateIndexType(Prospero::IndexType type);

struct MetalColorAttachmentConfig {
#ifdef __OBJC__
	id<MTLTexture>  texture      = nil;
#else
	id              texture      = nullptr;
#endif
	uint32_t        level        = 0;
	uint32_t        slice        = 0;
	MTLLoadAction   load_action  = static_cast<MTLLoadAction>(0); // MTLLoadActionDontCare
	MTLStoreAction  store_action = static_cast<MTLStoreAction>(1); // MTLStoreActionStore
	double          clear_color[4] = {0.0, 0.0, 0.0, 1.0};
	// Optional canonical-memory adapters for a private tiled subresource. They
	// stay with the pass description, including when an interrupted pass reopens.
	std::function<void(id)> before_load;
	std::function<void(id)> after_store;
	// Conservative pixel regions in which draws could write, including every
	// scissor of a viewport array. A clear covers the whole attachment.
	std::function<void(id,std::span<const PixelRect>)> after_store_regions;
};

struct MetalDepthStencilAttachmentConfig {
#ifdef __OBJC__
	id<MTLTexture>  texture      = nil;
#else
	id              texture      = nullptr;
#endif
	uint32_t        level        = 0;
	uint32_t        slice        = 0;
	MTLLoadAction   load_action  = static_cast<MTLLoadAction>(0); // MTLLoadActionDontCare
	MTLStoreAction  store_action = static_cast<MTLStoreAction>(1); // MTLStoreActionStore
	double          clear_depth  = 1.0;
	uint32_t        clear_stencil = 0;
	std::function<void(id)> before_load;
	std::function<void(id)> after_store;
};

struct MetalRenderPassDesc {
	uint32_t                                 render_target_array_length = 1;
	std::array<MetalColorAttachmentConfig, 8> color_attachments {};
	uint32_t                                 color_attachment_count = 0;
	MetalDepthStencilAttachmentConfig        depth_attachment {};
	MetalDepthStencilAttachmentConfig        stencil_attachment {};
	bool                                     has_depth   = false;
	bool                                     has_stencil = false;
};

struct MetalImageWriteback { id image; std::function<void(id)> encode; };

class MetalDrawExecutor {
public:
	static MetalDrawExecutor& Instance();

	MetalDrawExecutor();
	~MetalDrawExecutor();

	KYTY_CLASS_NO_COPY(MetalDrawExecutor);

	// Render pass lifecycle
	bool BeginRenderPass(const MetalRenderPassDesc& pass_desc);
	[[nodiscard]] const MetalRenderPassDesc& LastRenderPassDesc() const { return m_last_pass_desc; }
	// KYTY_PASS_TRACE_SENTINEL: count the samples the next pass writes (occlusion counting); the pass trace reads the count after the draw.
	void     SetTraceVisibility(bool on) { m_trace_visibility = on; }
	uint64_t LastPassVisibleSamples() const;
	void EndRenderPass();
	void SetGraphicsImageWritebacks(const std::vector<MetalImageWriteback>& images) { m_image_writebacks=images; }

#ifdef __OBJC__
	void SetRenderPipeline(id<MTLRenderPipelineState> pipeline);
	void SetDepthStencil(id<MTLDepthStencilState> depth_stencil);
	void SetDepthBounds(bool enabled, float minimum, float maximum);
#else
	void SetRenderPipeline(id pipeline);
	void SetDepthStencil(id depth_stencil);
#endif

	void SetRasterizerState(const MetalRasterizerState& rasterizer);
	void SetViewport(double x, double y, double width, double height, double znear = 0.0, double zfar = 1.0);
	void SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
	void SetGuestViewports(const HW::ScreenViewport&, const HW::ScanModeControl&, const HW::ClipControl&, bool indexed);
	void SetGuestScissor(const HW::ScreenViewport& viewport, const HW::ScanModeControl& scan_mode);
	void SetDepthBias(float depth_bias, float slope_scale, float clamp);
	void SetStencilReference(uint32_t ref);
	void SetStencilReferences(uint32_t front, uint32_t back);
	void SetBlendColor(float r, float g, float b, float a);

	// Resource binding
#ifdef __OBJC__
	void SetVertexBuffer(id<MTLBuffer> buffer, uint64_t offset, uint32_t index);
	void SetFragmentBuffer(id<MTLBuffer> buffer, uint64_t offset, uint32_t index);
	void SetFragmentTexture(id<MTLTexture> texture, uint32_t index);
	void SetFragmentSampler(id<MTLSamplerState> sampler, uint32_t index);
#else
	void SetVertexBuffer(id buffer, uint64_t offset, uint32_t index);
	void SetFragmentBuffer(id buffer, uint64_t offset, uint32_t index);
	void SetFragmentTexture(id texture, uint32_t index);
	void SetFragmentSampler(id sampler, uint32_t index);
#endif
	void SetMeshState(const ShaderMeshInputInfo* mesh);
	void SetVertexBytes(const void* bytes, size_t length, uint32_t index);
	void SetFragmentBytes(const void* bytes, size_t length, uint32_t index);
	void SetComputeBytes(const void* bytes, size_t length, uint32_t index);
#ifdef __OBJC__
	void UseResource(id<MTLResource> resource, MTLResourceUsage usage);
#else
	void UseResource(id resource, NSUInteger usage);
#endif


	// Direct draws
	void Draw(Prospero::PrimitiveType prim_type,
	          uint32_t vertex_count,
	          uint32_t instance_count = 1,
	          uint32_t first_vertex = 0,
	          uint32_t first_instance = 0);

#ifdef __OBJC__
	// Draw with the arguments read from a GPU buffer (DrawPrimitivesIndirectArguments layout).
	// Defer diagnostic pass splitting until the last element of a multi-draw;
	// ending its encoder earlier would discard pipeline/resource state for the rest.
	void DrawIndirect(Prospero::PrimitiveType prim_type, id<MTLBuffer> args_buffer, uint64_t args_offset,
	                  bool last_in_batch = true);
	void DrawIndexedIndirect(Prospero::PrimitiveType prim_type, MTLIndexType index_type, id<MTLBuffer> index_buffer,
	                         uint64_t index_offset, id<MTLBuffer> args_buffer, uint64_t args_offset,
	                         bool last_in_batch = true);

	void DrawIndexed(Prospero::PrimitiveType prim_type,
	                 uint32_t index_count,
	                 id<MTLBuffer> index_buffer,
	                 uint64_t index_buffer_offset,
	                 MTLIndexType index_type,
	                 uint32_t instance_count = 1,
	                 int32_t base_vertex = 0,
	                 uint32_t first_instance = 0);
#else
	void DrawIndexed(Prospero::PrimitiveType prim_type,
	                 uint32_t index_count,
	                 id index_buffer,
	                 uint64_t index_buffer_offset,
	                 MTLIndexType index_type,
	                 uint32_t instance_count = 1,
	                 int32_t base_vertex = 0,
	                 uint32_t first_instance = 0);
#endif

	// Compute pass lifecycle
	bool BeginComputePass();
	void EndComputePass();

	// KYTY_CS_OVERLAP: the guest memory the next dispatch of this pass reads and writes (called while its resources are bound). A dispatch
	// that declares nothing, or that reaches memory through raw pointers, is never overlapped with its neighbours (see metalOrder.h).
	void ComputeAccessAdd(uint64_t begin, uint64_t end, bool write);
	void ComputeAccessUnknown();

#ifdef __OBJC__
	void SetComputePipeline(id<MTLComputePipelineState> pipeline);
	void SetComputeBuffer(id<MTLBuffer> buffer, uint64_t offset, uint32_t index);
	void SetComputeTexture(id<MTLTexture> texture, uint32_t index);
#else
	void SetComputePipeline(id pipeline);
	void SetComputeBuffer(id buffer, uint64_t offset, uint32_t index);
	void SetComputeTexture(id texture, uint32_t index);
#endif

	void Dispatch(uint32_t group_x, uint32_t group_y, uint32_t group_z,
	              uint32_t threads_per_group_x, uint32_t threads_per_group_y, uint32_t threads_per_group_z);

#ifdef __OBJC__
	void DispatchIndirect(id<MTLBuffer> indirect_args, uint64_t indirect_offset,
	                      uint32_t threads_per_group_x, uint32_t threads_per_group_y, uint32_t threads_per_group_z);
#else
	void DispatchIndirect(id indirect_args, uint64_t indirect_offset,
	                      uint32_t threads_per_group_x, uint32_t threads_per_group_y, uint32_t threads_per_group_z);
#endif

private:
	void QueueGraphicsImageWritebacks();
	std::vector<MetalImageWriteback> m_image_writebacks;
	MetalRenderPassDesc m_last_pass_desc {};
	std::array<std::shared_ptr<std::vector<PixelRect>>,8> m_color_regions {};
	std::vector<PixelRect> m_scissors;
	bool                m_trace_visibility = false;
#ifdef __OBJC__
	id<MTLBuffer>       m_visibility_buffer = nil;
#else
	void*               m_visibility_buffer = nullptr;
#endif
	uint32_t m_render_width = 0;
	uint32_t m_render_height = 0;
	bool m_scissor_empty = false;
	bool m_has_render_pipeline = false;
	bool m_mesh_active = false;
	ShaderMeshInputInfo m_mesh {};
	void DrawMesh(uint32_t count, uint32_t instances, uint32_t base_vertex, uint32_t first_instance, uint32_t index_bytes);
	bool m_has_compute_pipeline = false;
	struct ComputeAccessRec {
		uint64_t begin = 0;
		uint64_t end   = 0;
		bool     write = false;
	};
	std::vector<ComputeAccessRec> m_access;
	bool                          m_access_unknown = false;
	bool                          m_order_pending  = false;
#ifdef __OBJC__
	void BeginDispatchOrder(id<MTLComputeCommandEncoder> encoder);
#endif
};

} // namespace Libs::Graphics::Metal

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALDRAW_H_
