#include "graphics/host_gpu/metal/metalPassTimer.h"
#include "graphics/host_gpu/metal/metalOrder.h"
#include "graphics/host_gpu/metal/metalEnv.h"
#include "graphics/host_gpu/metal/metalDraw.h"
#include <mutex>
#include <chrono>
#include <atomic>
#include <unordered_map>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <string>
#include <algorithm>
#include <limits>
#include "graphics/host_gpu/metal/metalContext.h"
#include "common/logging/log.h"
#include "graphics/presentation/traceWindow.h"

namespace Libs::Graphics::Metal {

// KYTY_MTL_FAULT_INFO=1: label encoders and insert a signpost per draw/dispatch so a GPU fault can be
// attributed to the encoder op that caused it (see the completion handler in metalContext.mm).
static bool FaultInfoOn() {
	static const bool on = KYTY_ENV("KYTY_MTL_FAULT_INFO") != nullptr;
	return on;
}
static std::string s_fault_op;
static std::string s_cb_ops;
static bool GpuTimeOn() {
	static const bool on = KYTY_ENV("KYTY_MTL_GPU_TIME") != nullptr;
	return on;
}
// KYTY_MTL_SYNC_EACH=1 (with KYTY_MTL_SYNC_EACH_FROM=<flip>, default 0): every compute dispatch runs alone from that flip on.
static bool SyncEachActive() {
	static const bool on = KYTY_ENV("KYTY_MTL_SYNC_EACH") != nullptr;
	static const uint64_t from = KYTY_ENV("KYTY_MTL_SYNC_EACH_FROM") != nullptr ? std::strtoull(KYTY_ENV("KYTY_MTL_SYNC_EACH_FROM"), nullptr, 0) : 0ull;
	return on && Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed) >= from;
}
void KytyFaultOpSet(const char* label) {
	if (FaultInfoOn() || KYTY_ENV("KYTY_MTL_SYNC_EACH") != nullptr || KYTY_ENV("KYTY_MTL_SPLIT_OPS") != nullptr) s_fault_op = label;
	if (GpuTimeOn() && s_cb_ops.size() < 16000) {
		s_cb_ops += label;
		s_cb_ops += "; ";
	}
}
// Op labels recorded since the last commit (KYTY_MTL_GPU_TIME): becomes the command buffer label.
std::string KytyTakeCbOps() {
	std::string out;
	out.swap(s_cb_ops);
	return out;
}
static void FaultSignpost(id<MTLCommandEncoder> enc) {
	if (FaultInfoOn() && enc) {
		const char* str = s_fault_op.empty() ? "unlabelled_op" : s_fault_op.c_str();
		[enc insertDebugSignpost:[NSString stringWithUTF8String:str]];
	}
}

void CountDispatchStat(int kind);
}

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

namespace Libs::Graphics::Metal {

MTLPrimitiveType TranslatePrimitiveType(Prospero::PrimitiveType type) {
	switch (type) {
		case Prospero::PrimitiveType::kPointList:
			return MTLPrimitiveTypePoint;
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineLoop:
			return MTLPrimitiveTypeLine;
		case Prospero::PrimitiveType::kLineStrip:
			return MTLPrimitiveTypeLineStrip;
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
			return MTLPrimitiveTypeTriangle;
		case Prospero::PrimitiveType::kTriStrip:
			return MTLPrimitiveTypeTriangleStrip;
		default:
			return MTLPrimitiveTypeTriangle;
	}
}

MTLIndexType TranslateIndexType(Prospero::IndexType type) {
	switch (type) {
		case Prospero::IndexType::kIndex16:
			return MTLIndexTypeUInt16;
		case Prospero::IndexType::kIndex32:
			return MTLIndexTypeUInt32;
		default:
			return MTLIndexTypeUInt16;
	}
}

MetalDrawExecutor& MetalDrawExecutor::Instance() {
	static MetalDrawExecutor executor;
	return executor;
}

MetalDrawExecutor::MetalDrawExecutor() = default;

MetalDrawExecutor::~MetalDrawExecutor() {
	EndRenderPass();
	EndComputePass();
}

bool MetalDrawExecutor::BeginRenderPass(const MetalRenderPassDesc& pass_desc) {
	m_last_pass_desc = pass_desc;
	@autoreleasepool {
		NSUInteger width = std::numeric_limits<NSUInteger>::max();
		NSUInteger height = width;
		const auto include_attachment = [&](id<MTLTexture> texture, NSUInteger level) {
			if (texture == nil) return;
			width = std::min(width, std::max<NSUInteger>(1, texture.width >> level));
			height = std::min(height, std::max<NSUInteger>(1, texture.height >> level));
		};
		for (uint32_t i = 0; i < pass_desc.color_attachment_count; ++i)
			include_attachment(pass_desc.color_attachments[i].texture, pass_desc.color_attachments[i].level);
		if (pass_desc.has_depth) include_attachment(pass_desc.depth_attachment.texture, pass_desc.depth_attachment.level);
		if (pass_desc.has_stencil) include_attachment(pass_desc.stencil_attachment.texture, pass_desc.stencil_attachment.level);
		m_render_width = width == std::numeric_limits<NSUInteger>::max() ? 0 : static_cast<uint32_t>(width);
		m_render_height = height == std::numeric_limits<NSUInteger>::max() ? 0 : static_cast<uint32_t>(height);
		m_scissor_empty = false;
		MTLRenderPassDescriptor* desc = [MTLRenderPassDescriptor renderPassDescriptor];
		// Guest vertex shaders may write render_target_array_index; Metal needs a nonzero array length.
		desc.renderTargetArrayLength = 1;
		if (!desc) {
			return false;
		}

		for (uint32_t i = 0; i < pass_desc.color_attachment_count; ++i) {
			const auto& color_cfg = pass_desc.color_attachments[i];
			if (!color_cfg.texture) {
				continue;
			}

			MTLRenderPassColorAttachmentDescriptor* att = desc.colorAttachments[i];
			att.texture = color_cfg.texture;
			att.level = color_cfg.level;
			att.slice = color_cfg.slice;
			att.loadAction = color_cfg.load_action;
			att.storeAction = color_cfg.store_action;
			att.clearColor = MTLClearColorMake(color_cfg.clear_color[0],
			                                   color_cfg.clear_color[1],
			                                   color_cfg.clear_color[2],
			                                   color_cfg.clear_color[3]);
		}

		if (pass_desc.has_depth && pass_desc.depth_attachment.texture) {
			MTLRenderPassDepthAttachmentDescriptor* depth_att = desc.depthAttachment;
			depth_att.texture = pass_desc.depth_attachment.texture;
			depth_att.level = pass_desc.depth_attachment.level;
			depth_att.slice = pass_desc.depth_attachment.slice;
			depth_att.loadAction = pass_desc.depth_attachment.load_action;
			depth_att.storeAction = pass_desc.depth_attachment.store_action;
			depth_att.clearDepth = pass_desc.depth_attachment.clear_depth;
		}

		if (pass_desc.has_stencil && pass_desc.stencil_attachment.texture) {
			MTLRenderPassStencilAttachmentDescriptor* stencil_att = desc.stencilAttachment;
			stencil_att.texture = pass_desc.stencil_attachment.texture;
			stencil_att.level = pass_desc.stencil_attachment.level;
			stencil_att.slice = pass_desc.stencil_attachment.slice;
			stencil_att.loadAction = pass_desc.stencil_attachment.load_action;
			stencil_att.storeAction = pass_desc.stencil_attachment.store_action;
			stencil_att.clearStencil = pass_desc.stencil_attachment.clear_stencil;
		}

		MetalContext::Instance().EndCurrentEncoder();

		id<MTLCommandBuffer> cmd_buf = MetalContext::Instance().GetCurrentCommandBuffer();
		if (!cmd_buf) {
			return false;
		}

		if (m_trace_visibility) {
			if (m_visibility_buffer == nil) m_visibility_buffer = [MetalContext::Instance().GetDevice() newBufferWithLength:4096 options:MTLResourceStorageModeShared];
			std::memset(m_visibility_buffer.contents, 0, 8);
			desc.visibilityResultBuffer = m_visibility_buffer;
		}
		PassTimerAttachRender(cmd_buf, desc);
		id<MTLRenderCommandEncoder> encoder = [cmd_buf renderCommandEncoderWithDescriptor:desc];
		if (!encoder) {
			return false;
		}
		OrderBegin(encoder);
		if (m_trace_visibility) [encoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:0];
		if (FaultInfoOn()) encoder.label = @"render";

		MetalContext::Instance().SetActiveRenderEncoder(encoder);
		m_has_render_pipeline = false;
		return true;
	}
}

uint64_t MetalDrawExecutor::LastPassVisibleSamples() const {
	return m_visibility_buffer != nil ? *static_cast<const uint64_t*>(m_visibility_buffer.contents) : 0;
}

void MetalDrawExecutor::EndRenderPass() {
	m_has_render_pipeline = false;
	MetalContext::Instance().EndCurrentEncoder();
}

void MetalDrawExecutor::SetRenderPipeline(id<MTLRenderPipelineState> pipeline) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder && pipeline) {
		[encoder setRenderPipelineState:pipeline];
		m_has_render_pipeline = true;
	} else {
		m_has_render_pipeline = false;
	}
}

void MetalDrawExecutor::SetDepthStencil(id<MTLDepthStencilState> depth_stencil) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder && depth_stencil) {
		[encoder setDepthStencilState:depth_stencil];
	}
}

static MTLWinding s_last_winding = MTLWindingClockwise;

void MetalDrawExecutor::SetRasterizerState(const MetalRasterizerState& rasterizer) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (!encoder) {
		return;
	}

	static const bool s_log_vp = KYTY_ENV("KYTY_LOG_VP") != nullptr;
	if (s_log_vp) {
		std::fprintf(stderr, "[VpRast] cull=%lu winding=%lu fill=%lu\n", (unsigned long)rasterizer.cull_mode,
		             (unsigned long)rasterizer.winding, (unsigned long)rasterizer.fill_mode);
	}
	[encoder setCullMode:rasterizer.cull_mode];
	s_last_winding = rasterizer.winding;
	[encoder setFrontFacingWinding:rasterizer.winding];
	[encoder setTriangleFillMode:rasterizer.fill_mode];
	[encoder setDepthClipMode:rasterizer.depth_clip_mode];

	// Encoder state outlives the draw: a draw without polygon offset must clear the previous draw's bias.
	if (rasterizer.depth_bias_enable) {
		[encoder setDepthBias:rasterizer.depth_bias_constant slopeScale:rasterizer.depth_bias_slope clamp:rasterizer.depth_bias_clamp];
	} else {
		[encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
	}
}

void MetalDrawExecutor::SetViewport(double x, double y, double width, double height, double znear, double zfar) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (!encoder) {
		return;
	}

	// Guest/Vulkan viewports may carry a negative height to flip Y. Metal requires a
	// positive viewport height; translate the flip into the origin instead.
	bool flipped = false;
	if (height < 0.0) {
		y += height;
		height = -height;
		flipped = true;
	}
	// Experimental (KYTY_FLIP_WINDING=1): the Y flip mirrors the image and inverts triangle winding.
	// Not verified to change the output of the black draws; off by default.
	static const bool s_flip_winding = KYTY_ENV("KYTY_FLIP_WINDING") != nullptr;
	if (flipped && s_flip_winding) {
		[encoder setFrontFacingWinding:(s_last_winding == MTLWindingClockwise ? MTLWindingCounterClockwise : MTLWindingClockwise)];
	} else {
		[encoder setFrontFacingWinding:s_last_winding];
	}
	static const bool s_log_vp2 = KYTY_ENV("KYTY_LOG_VP") != nullptr;
	if (s_log_vp2) {
		std::fprintf(stderr, "[VpView] flip=%llu x=%g y=%g w=%g h=%g zn=%g zf=%g flipped=%d\n", static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)), x, y, width, height, znear, zfar, flipped ? 1 : 0);
	}
	MTLViewport vp {x, y, width, height, znear, zfar};
	[encoder setViewport:vp];
}

void MetalDrawExecutor::SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (!encoder) {
		return;
	}

	width = x >= m_render_width ? 0 : std::min(width, m_render_width - x);
	height = y >= m_render_height ? 0 : std::min(height, m_render_height - y);
	m_scissor_empty = width == 0 || height == 0;
	if (m_scissor_empty) return;
	static const bool s_log_sc = KYTY_ENV("KYTY_LOG_VP") != nullptr;
	if (s_log_sc) {
		std::fprintf(stderr, "[VpScissor] flip=%llu rt=%ux%u x=%u y=%u w=%u h=%u\n",
		             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)), m_render_width, m_render_height, x, y, width, height);
	}
	MTLScissorRect scissor {static_cast<NSUInteger>(x),
	                        static_cast<NSUInteger>(y),
	                        static_cast<NSUInteger>(width),
	                        static_cast<NSUInteger>(height)};
	[encoder setScissorRect:scissor];
}

void MetalDrawExecutor::SetDepthBias(float depth_bias, float slope_scale, float clamp) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder) {
		[encoder setDepthBias:depth_bias slopeScale:slope_scale clamp:clamp];
	}
}

void MetalDrawExecutor::SetStencilReferences(uint32_t front, uint32_t back) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder) {
		[encoder setStencilFrontReferenceValue:front backReferenceValue:back];
	}
}

void MetalDrawExecutor::SetStencilReference(uint32_t ref) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder) {
		[encoder setStencilReferenceValue:ref];
	}
}

void MetalDrawExecutor::SetBlendColor(float r, float g, float b, float a) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder) {
		[encoder setBlendColorRed:r green:g blue:b alpha:a];
	}
}

void MetalDrawExecutor::SetVertexBuffer(id<MTLBuffer> buffer, uint64_t offset, uint32_t index) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder && buffer) {
		[encoder setVertexBuffer:buffer offset:static_cast<NSUInteger>(offset) atIndex:index];
	}
}

void MetalDrawExecutor::SetFragmentBuffer(id<MTLBuffer> buffer, uint64_t offset, uint32_t index) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder && buffer) {
		[encoder setFragmentBuffer:buffer offset:static_cast<NSUInteger>(offset) atIndex:index];
	}
}

void MetalDrawExecutor::SetFragmentTexture(id<MTLTexture> texture, uint32_t index) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder && texture) {
		[encoder setFragmentTexture:texture atIndex:index];
	}
}

void MetalDrawExecutor::SetFragmentSampler(id<MTLSamplerState> sampler, uint32_t index) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder && sampler) {
		[encoder setFragmentSamplerState:sampler atIndex:index];
	}
}

void MetalDrawExecutor::SetVertexBytes(const void* bytes, size_t length, uint32_t index) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder && bytes && length > 0) {
		[encoder setVertexBytes:bytes length:length atIndex:index];
	}
}

void MetalDrawExecutor::SetFragmentBytes(const void* bytes, size_t length, uint32_t index) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder && bytes && length > 0) {
		[encoder setFragmentBytes:bytes length:length atIndex:index];
	}
}

void MetalDrawExecutor::SetComputeBytes(const void* bytes, size_t length, uint32_t index) {
	id<MTLComputeCommandEncoder> encoder = MetalContext::Instance().GetActiveComputeEncoder();
	if (encoder && bytes && length > 0) {
		[encoder setBytes:bytes length:length atIndex:index];
	}
}

void MetalDrawExecutor::UseResource(id<MTLResource> resource, MTLResourceUsage usage) {
	if (!resource) return;
	// Residency declared on an encoder lasts for the whole encoder, and every draw used to declare all the buffers and textures it
	// binds again (hundreds per draw, the same guest-memory chunks over and over; with the GPU capture layer each call is far dearer).
	// Declare a resource once per encoder per usage.
	{
		static std::mutex                             s_mutex;
		static uint64_t                               s_serial = ~0ull;
		static std::unordered_map<uintptr_t, uint32_t> s_declared;
		std::lock_guard<std::mutex> lock(s_mutex);
		const uint64_t serial = MetalContext::Instance().EncoderSerial();
		if (serial != s_serial) {
			s_declared.clear();
			s_serial = serial;
		}
		uint32_t& mask = s_declared[reinterpret_cast<uintptr_t>((__bridge void*)resource)];
		if ((mask & static_cast<uint32_t>(usage)) == static_cast<uint32_t>(usage)) return;
		mask |= static_cast<uint32_t>(usage);
	}
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder) {
		[encoder useResource:resource usage:usage stages:MTLRenderStageVertex | MTLRenderStageFragment];
	}
	id<MTLComputeCommandEncoder> c_encoder = MetalContext::Instance().GetActiveComputeEncoder();
	if (c_encoder) {
		[c_encoder useResource:resource usage:usage];
	}
}


// KYTY_MTL_SPLIT_DRAWS=1 (from flip KYTY_MTL_SYNC_EACH_FROM): every draw ends its render pass and its command buffer (commit without waiting), so the
// buffer a GPU hang names (watchdog, KYTY_MTL_GPU_TIME=1) holds that one draw. Later draws reopen the pass with the loaded contents. Called under the
// renderer mutex (a destructor of the draw function would run after it and race with Flush on the present thread).
static void SplitAfterDraw() {
	static const bool on = KYTY_ENV("KYTY_MTL_SPLIT_DRAWS") != nullptr;
	static const uint64_t from = KYTY_ENV("KYTY_MTL_SYNC_EACH_FROM") != nullptr ? std::strtoull(KYTY_ENV("KYTY_MTL_SYNC_EACH_FROM"), nullptr, 0) : 0ull;
	if (!on || Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed) < from) return;
	if (MetalContext::Instance().GetActiveRenderEncoder() == nil) return;
	MetalDrawExecutor::Instance().EndRenderPass();
	MetalContext::Instance().CommitCommandBuffer(false);
}

void MetalDrawExecutor::Draw(Prospero::PrimitiveType prim_type,
                             uint32_t vertex_count,
                             uint32_t instance_count,
                             uint32_t first_vertex,
                             uint32_t first_instance) {
	FaultSignpost(MetalContext::Instance().GetActiveRenderEncoder());
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	{
		static std::atomic<uint64_t> s_calls {0};
		static std::atomic<uint64_t> s_skipped {0};
		const uint64_t call = s_calls.fetch_add(1);
		if (!encoder || !m_has_render_pipeline || m_scissor_empty || vertex_count == 0 || instance_count == 0) {
			const uint64_t skipped = s_skipped.fetch_add(1) + 1;
			if (call < 10 || call % 2000 == 0) {
				std::fprintf(stderr, "[Draw] skip calls=%llu skipped=%llu enc=%d pipe=%d vtx=%u\n",
				             (unsigned long long)call, (unsigned long long)skipped,
				             encoder ? 1 : 0, m_has_render_pipeline ? 1 : 0, vertex_count);
			}
			return;
		}
		if (call % 2000 == 0) {
			std::fprintf(stderr, "[Draw] ok calls=%llu skipped=%llu\n",
			             (unsigned long long)call, (unsigned long long)s_skipped.load());
		}
	}

	MTLPrimitiveType mtl_prim = TranslatePrimitiveType(prim_type);
	// RECTLIST: three corners define a rectangle; the vertex shader wrapper synthesises vertex 3, so a single
	// rectangle is drawn as a four-vertex strip (two triangles).
	if (Prospero::IsRectList(prim_type) && vertex_count == 3) {
		mtl_prim     = MTLPrimitiveTypeTriangleStrip;
		vertex_count = 4;
	}
	PassTimerItem();
	[encoder drawPrimitives:mtl_prim
	            vertexStart:first_vertex
	            vertexCount:vertex_count
	          instanceCount:instance_count
	           baseInstance:first_instance];
	SplitAfterDraw();
}

void MetalDrawExecutor::DrawIndirect(Prospero::PrimitiveType prim_type, id<MTLBuffer> args_buffer,
                                     uint64_t args_offset, bool last_in_batch) {
	FaultSignpost(MetalContext::Instance().GetActiveRenderEncoder());
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (!encoder || !m_has_render_pipeline || m_scissor_empty || args_buffer == nil) {
		return;
	}
	// Expanded nonindexed RECTLIST arguments contain four vertices, which
	// must form two triangles. The indexed RECTLIST path uses a triangle list.
	const MTLPrimitiveType primitive = Prospero::IsRectList(prim_type) ? MTLPrimitiveTypeTriangleStrip
	                                                                : TranslatePrimitiveType(prim_type);
	PassTimerItem();
	[encoder drawPrimitives:primitive
	         indirectBuffer:args_buffer
	   indirectBufferOffset:args_offset];
	if (last_in_batch) SplitAfterDraw();
}

void MetalDrawExecutor::DrawIndexedIndirect(Prospero::PrimitiveType prim_type, MTLIndexType index_type,
                                             id<MTLBuffer> index_buffer, uint64_t index_offset,
                                             id<MTLBuffer> args_buffer, uint64_t args_offset, bool last_in_batch) {
	FaultSignpost(MetalContext::Instance().GetActiveRenderEncoder());
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (!encoder || !m_has_render_pipeline || m_scissor_empty || index_buffer == nil || args_buffer == nil) {
		return;
	}
	PassTimerItem();
	[encoder drawIndexedPrimitives:TranslatePrimitiveType(prim_type)
	                     indexType:index_type
	                   indexBuffer:index_buffer
	             indexBufferOffset:index_offset
	                indirectBuffer:args_buffer
	          indirectBufferOffset:args_offset];
	if (last_in_batch) SplitAfterDraw();
}

void MetalDrawExecutor::DrawIndexed(Prospero::PrimitiveType prim_type,
                                    uint32_t index_count,
                                    id<MTLBuffer> index_buffer,
                                    uint64_t index_buffer_offset,
                                    MTLIndexType index_type,
                                    uint32_t instance_count,
                                    int32_t base_vertex,
                                    uint32_t first_instance) {
	FaultSignpost(MetalContext::Instance().GetActiveRenderEncoder());
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (!encoder || !m_has_render_pipeline || m_scissor_empty || index_count == 0 || instance_count == 0 || !index_buffer) {
		return;
	}

	const MTLPrimitiveType mtl_prim = TranslatePrimitiveType(prim_type);
	PassTimerItem();
	[encoder drawIndexedPrimitives:mtl_prim
	                    indexCount:index_count
	                     indexType:index_type
	                   indexBuffer:index_buffer
	             indexBufferOffset:static_cast<NSUInteger>(index_buffer_offset)
	                 instanceCount:instance_count
	                    baseVertex:base_vertex
	                  baseInstance:first_instance];
	SplitAfterDraw();
}

bool MetalDrawExecutor::BeginComputePass() {
	MetalContext::Instance().EndCurrentEncoder();

	id<MTLCommandBuffer> cmd_buf = MetalContext::Instance().GetCurrentCommandBuffer();
	if (!cmd_buf) {
		return false;
	}

	MTLComputePassDescriptor*    timed   = PassTimerComputeDescriptor(cmd_buf);
	id<MTLComputeCommandEncoder> encoder = timed != nil ? [cmd_buf computeCommandEncoderWithDescriptor:timed] : [cmd_buf computeCommandEncoder];
	if (!encoder) {
		return false;
	}
	OrderBegin(encoder);
	if (FaultInfoOn()) encoder.label = @"compute";

	MetalContext::Instance().SetActiveComputeEncoder(encoder);
	m_has_compute_pipeline = false;
	return true;
}

void MetalDrawExecutor::EndComputePass() {
	m_has_compute_pipeline = false;
	MetalContext::Instance().EndCurrentEncoder();
}

void MetalDrawExecutor::SetComputePipeline(id<MTLComputePipelineState> pipeline) {
	id<MTLComputeCommandEncoder> encoder = MetalContext::Instance().GetActiveComputeEncoder();
	if (encoder && pipeline) {
		[encoder setComputePipelineState:pipeline];
		m_has_compute_pipeline = true;
	} else {
		m_has_compute_pipeline = false;
	}
}

void MetalDrawExecutor::SetComputeBuffer(id<MTLBuffer> buffer, uint64_t offset, uint32_t index) {
	id<MTLComputeCommandEncoder> encoder = MetalContext::Instance().GetActiveComputeEncoder();
	if (encoder && buffer) {
		[encoder setBuffer:buffer offset:static_cast<NSUInteger>(offset) atIndex:index];
	}
}

void MetalDrawExecutor::SetComputeTexture(id<MTLTexture> texture, uint32_t index) {
	id<MTLComputeCommandEncoder> encoder = MetalContext::Instance().GetActiveComputeEncoder();
	if (encoder && texture) {
		[encoder setTexture:texture atIndex:index];
	}
}

// KYTY_MTL_SPLIT_OPS=1 (with KYTY_MTL_SYNC_EACH_FROM=<flip>): every compute dispatch ends its command buffer (commit without waiting), so the
// buffer the GPU watchdog names holds that one dispatch (label with KYTY_MTL_GPU_TIME=1) and the run stays close to full speed.
static bool SplitOpsActive() {
	static const bool on = KYTY_ENV("KYTY_MTL_SPLIT_OPS") != nullptr;
	static const uint64_t from = KYTY_ENV("KYTY_MTL_SYNC_EACH_FROM") != nullptr ? std::strtoull(KYTY_ENV("KYTY_MTL_SYNC_EACH_FROM"), nullptr, 0) : 0ull;
	return on && Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed) >= from;
}

static void SplitOp(uint32_t gx, uint32_t gy, uint32_t gz, uint32_t tx, uint32_t ty, uint32_t tz) {
	char extra[96];
	std::snprintf(extra, sizeof(extra), "groups=%ux%ux%u threads=%ux%ux%u; ", gx, gy, gz, tx, ty, tz);
	if (GpuTimeOn() && s_cb_ops.size() < 16000) s_cb_ops += extra;
	MetalContext::Instance().CommitCommandBuffer(false);
}

// KYTY_MTL_SYNC_EACH: commit and wait for the dispatch just encoded; the begin line is printed first so a hung op is the last begin.
// KYTY_MTL_SYNC_EACH=quiet: only the commit and wait (no lines, no pause), fast enough to reach the menu: tells a race between a
// dispatch and later encode-time CPU work (uploads from guest memory) from a wrong result.
static void SyncEachOp(const char* kind, uint32_t gx, uint32_t gy, uint32_t gz, uint32_t tx, uint32_t ty, uint32_t tz) {
	static const bool s_quiet = KYTY_ENV("KYTY_MTL_SYNC_EACH") != nullptr && std::string_view(KYTY_ENV("KYTY_MTL_SYNC_EACH")) == "quiet";
	if (s_quiet) {
		MetalContext::Instance().CommitCommandBuffer(true);
		return;
	}
	std::fprintf(stderr, "[SyncOp] begin %s %s groups=%ux%ux%u threads=%ux%ux%u\n", kind, s_fault_op.c_str(), gx, gy, gz, tx, ty, tz);
	std::fflush(stderr);
	const auto t0 = std::chrono::steady_clock::now();
	MetalContext::Instance().CommitCommandBuffer(true);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	usleep(3000);
	std::fprintf(stderr, "[SyncOp] end %s %s ms=%.1f%s\n", kind, s_fault_op.c_str(), ms, ms > 500.0 ? " SLOW" : "");
}

void MetalDrawExecutor::Dispatch(uint32_t group_x, uint32_t group_y, uint32_t group_z,
                                 uint32_t threads_per_group_x, uint32_t threads_per_group_y, uint32_t threads_per_group_z) {
	FaultSignpost(MetalContext::Instance().GetActiveComputeEncoder());
	static std::atomic<uint32_t> s_dispatch_total {0};
	static std::atomic<uint32_t> s_dispatch_dropped {0};
	id<MTLComputeCommandEncoder> encoder = MetalContext::Instance().GetActiveComputeEncoder();
	const uint32_t total = s_dispatch_total.fetch_add(1) + 1;
	if (!encoder || !m_has_compute_pipeline || group_x == 0 || group_y == 0 || group_z == 0) {
		CountDispatchStat(!encoder ? 4 : !m_has_compute_pipeline ? 3 : 5);
		// Dropped dispatch: no encoder, no pipeline, or an empty grid. Must stay 0 on a healthy run.
		const uint32_t dropped = s_dispatch_dropped.fetch_add(1) + 1;
		if (dropped <= 20 || dropped % 500 == 0) {
			std::fprintf(stderr, "[DispatchDrop] dropped=%u of total=%u encoder=%d pipeline=%d groups=%ux%ux%u\n", dropped, total,
			             encoder ? 1 : 0, m_has_compute_pipeline ? 1 : 0, group_x, group_y, group_z);
		}
		return;
	}
	if (total % 2000 == 0) {
		std::fprintf(stderr, "[DispatchStats] total=%u dropped=%u\n", total, s_dispatch_dropped.load());
	}

	MTLSize threadgroups = MTLSizeMake(group_x, group_y, group_z);
	MTLSize threads_per_group = MTLSizeMake(threads_per_group_x, threads_per_group_y, threads_per_group_z);

	CountDispatchStat(2);
	PassTimerItem();
	[encoder dispatchThreadgroups:threadgroups threadsPerThreadgroup:threads_per_group];
	// KYTY_MTL_SYNC_EACH=1: run this dispatch to completion alone, so validation/fault reports printed
	// between the begin/end markers belong to this op (shader hash in the op label). The end line carries the wall time of the
	// op (it also covers draws recorded since the previous commit); an op that never ends is the one a GPU hang is in.
	if (SyncEachActive()) {
		SyncEachOp("dispatch", group_x, group_y, group_z, threads_per_group_x, threads_per_group_y, threads_per_group_z);
	} else if (SplitOpsActive()) {
		SplitOp(group_x, group_y, group_z, threads_per_group_x, threads_per_group_y, threads_per_group_z);
	}
}

void MetalDrawExecutor::DispatchIndirect(id<MTLBuffer> indirect_args, uint64_t indirect_offset,
                                         uint32_t threads_per_group_x, uint32_t threads_per_group_y, uint32_t threads_per_group_z) {
	FaultSignpost(MetalContext::Instance().GetActiveComputeEncoder());
	id<MTLComputeCommandEncoder> encoder = MetalContext::Instance().GetActiveComputeEncoder();
	if (!encoder || !m_has_compute_pipeline || !indirect_args) {
		CountDispatchStat(!encoder ? 4 : 3);
		return;
	}
	if (indirect_offset % 4 != 0 || indirect_offset > indirect_args.length ||
	    indirect_args.length - indirect_offset < sizeof(MTLDispatchThreadgroupsIndirectArguments)) return;

	MTLSize threads_per_group = MTLSizeMake(threads_per_group_x, threads_per_group_y, threads_per_group_z);
	// MetalRenderer prepares these counts on the GPU, in order after their
	// producer. Host reads/writes here would race that producer.
	CountDispatchStat(2);
	PassTimerItem();
	[encoder dispatchThreadgroupsWithIndirectBuffer:indirect_args
	                           indirectBufferOffset:static_cast<NSUInteger>(indirect_offset)
	                          threadsPerThreadgroup:threads_per_group];
	if (SyncEachActive()) {
		SyncEachOp("indirect(gpu-counts)", 0, 0, 0, threads_per_group_x, threads_per_group_y, threads_per_group_z);
	} else if (SplitOpsActive()) {
		SplitOp(0, 0, 0, threads_per_group_x, threads_per_group_y, threads_per_group_z);
	}
}

} // namespace Libs::Graphics::Metal
