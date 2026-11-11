#include "graphics/host_gpu/metal/metalDepthBounds.h"
#include "graphics/host_gpu/metal/metalPassTimer.h"
#include "graphics/host_gpu/metal/metalOrder.h"
#include "graphics/host_gpu/metal/metalEnv.h"
#include "graphics/host_gpu/metal/metalUnsupported.h"
#include "graphics/host_gpu/metal/metalDraw.h"
#include "graphics/host_gpu/metal/metalIndirect.h"
#include "graphics/host_gpu/metal/metalViewport.h"
#include "graphics/host_gpu/renderer/debug.h"
#include <mutex>
#include <chrono>
#include <atomic>
#include <unordered_map>
#include <set>
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
			RejectMetalState("line-loop topology requires explicit closing indices");
		case Prospero::PrimitiveType::kLineStrip:
			return MTLPrimitiveTypeLineStrip;
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
			return MTLPrimitiveTypeTriangle;
		case Prospero::PrimitiveType::kTriStrip:
			return MTLPrimitiveTypeTriangleStrip;
		default:
			RejectMetalState("primitive topology " + std::to_string(uint32_t(type)));
	}
}

MTLIndexType TranslateIndexType(Prospero::IndexType type) {
	switch (type) {
		case Prospero::IndexType::kIndex16:
			return MTLIndexTypeUInt16;
		case Prospero::IndexType::kIndex32:
			return MTLIndexTypeUInt32;
		default:
			RejectMetalState("index element type " + std::to_string(uint32_t(type)));
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
		m_color_regions={};m_scissors={{0,0,m_render_width,m_render_height}};
		MTLRenderPassDescriptor* desc = [MTLRenderPassDescriptor renderPassDescriptor];
		// Guest vertex shaders may write render_target_array_index; Metal needs a nonzero array length.
		desc.renderTargetArrayLength = pass_desc.render_target_array_length;
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
		for (uint32_t i = 0; i < pass_desc.color_attachment_count; ++i) {
			const auto& color = pass_desc.color_attachments[i];
			if (color.texture && color.load_action == MTLLoadActionLoad && color.before_load) color.before_load(cmd_buf);
		}

		for (const auto* plane: {pass_desc.has_depth ? &pass_desc.depth_attachment : nullptr,
		                         pass_desc.has_stencil ? &pass_desc.stencil_attachment : nullptr})
			if (plane && plane->texture && plane->load_action == MTLLoadActionLoad && plane->before_load) plane->before_load(cmd_buf);

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
		for (uint32_t i = 0; i < pass_desc.color_attachment_count; ++i) {
			const auto& color = pass_desc.color_attachments[i];
			if (color.texture && color.store_action == MTLStoreActionStore) {
				if(color.after_store_regions) {
					auto regions=std::make_shared<std::vector<PixelRect>>();m_color_regions[i]=regions;
					if(color.load_action==MTLLoadActionClear)regions->push_back({0,0,uint32_t(std::max(color.texture.width>>color.level,NSUInteger(1))),uint32_t(std::max(color.texture.height>>color.level,NSUInteger(1)))});
					auto writeback=color.after_store_regions;
					MetalContext::Instance().QueueEncoderWriteback([regions,writeback](id cmd){writeback(cmd,*regions);});
				} else if(color.after_store)MetalContext::Instance().QueueEncoderWriteback(color.after_store);
			}
		}
		for (const auto* plane: {pass_desc.has_depth ? &pass_desc.depth_attachment : nullptr,
		                         pass_desc.has_stencil ? &pass_desc.stencil_attachment : nullptr})
			if (plane && plane->texture && plane->store_action == MTLStoreActionStore && plane->after_store)
				MetalContext::Instance().QueueEncoderWriteback(plane->after_store);
		m_has_render_pipeline = false;
		return true;
	}
}

uint64_t MetalDrawExecutor::LastPassVisibleSamples() const {
	return m_visibility_buffer != nil ? *static_cast<const uint64_t*>(m_visibility_buffer.contents) : 0;
}

void MetalDrawExecutor::QueueGraphicsImageWritebacks() {
	for(auto& regions:m_color_regions)if(regions)for(const auto& rect:m_scissors) {
		if(!rect.width || !rect.height || std::any_of(regions->begin(),regions->end(),[&](const auto& r){return r.Contains(rect);}))continue;
		regions->erase(std::remove_if(regions->begin(),regions->end(),[&](const auto& r){return rect.Contains(r);}),regions->end());
		regions->push_back(rect);
	}
	for(const auto& image:m_image_writebacks)
		MetalContext::Instance().QueueEncoderWriteback(image.encode,reinterpret_cast<uint64_t>((__bridge void*)image.image));
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

void MetalDrawExecutor::SetDepthBounds(bool enabled,float minimum,float maximum) {
	EncodeDepthBounds(MetalContext::Instance().GetActiveRenderEncoder(),enabled,minimum,maximum,
	    m_last_pass_desc.has_depth?m_last_pass_desc.depth_attachment.texture.pixelFormat:MTLPixelFormatInvalid);
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

	const MTLViewport vp = TranslateGuestViewport(x, y, width, height, znear, zfar);
	const bool flipped = height < 0.0;
	[encoder setFrontFacingWinding:s_last_winding];
	static const bool s_log_vp2 = KYTY_ENV("KYTY_LOG_VP") != nullptr;
	if (s_log_vp2) {
		std::fprintf(stderr, "[VpView] flip=%llu x=%g y=%g w=%g h=%g zn=%g zf=%g flipped=%d\n", static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)), x, y, width, height, znear, zfar, flipped ? 1 : 0);
	}
	[encoder setViewport:vp];
}

void MetalDrawExecutor::SetGuestViewports(const HW::ScreenViewport& screen, const HW::ScanModeControl& scan,
                                           const HW::ClipControl& clip, bool indexed) {
	if (!indexed) {
		const auto& v = screen.viewports[0];
		SetViewport(v.xoffset-v.xscale, v.yoffset-v.yscale, v.xscale*2.0, v.yscale*2.0,
		            v.zoffset-(clip.dx_clip_space ? 0.0 : v.zscale), v.zoffset+v.zscale);
		SetGuestScissor(screen,scan);
		return;
	}
	auto encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (!encoder) RejectMetalState("viewport array without an active render encoder");
	MTLViewport viewports[16]; MTLScissorRect scissors[16];
	m_scissors.clear();
	m_scissor_empty = true;
	for (uint32_t i=0;i<16;++i) {
		const auto& v = screen.viewports[i];
		viewports[i] = TranslateGuestViewport(v.xoffset-v.xscale,v.yoffset-v.yscale,v.xscale*2.0,v.yscale*2.0,
		    v.zoffset-(clip.dx_clip_space ? 0.0 : v.zscale),v.zoffset+v.zscale);
		const auto r = calc_final_scissor(screen,scan,{m_render_width,m_render_height},i);
		scissors[i] = {NSUInteger(r.left),NSUInteger(r.top),NSUInteger(r.right-r.left),NSUInteger(r.bottom-r.top)};
		m_scissors.push_back({uint32_t(r.left),uint32_t(r.top),uint32_t(r.right-r.left),uint32_t(r.bottom-r.top)});
		m_scissor_empty &= r.left==r.right || r.top==r.bottom;
	}
	[encoder setViewports:viewports count:16]; [encoder setScissorRects:scissors count:16];
}

void MetalDrawExecutor::SetGuestScissor(const HW::ScreenViewport& viewport, const HW::ScanModeControl& scan_mode) {
	// Share the guest screen/window/generic/viewport/clip-rectangle intersection
	// with the Vulkan renderer. A shadow atlas often uses only one small tile.
	const auto rect = calc_final_scissor(viewport, scan_mode, {m_render_width, m_render_height}, 0);
	SetScissor(static_cast<uint32_t>(rect.left), static_cast<uint32_t>(rect.top),
	           static_cast<uint32_t>(rect.right - rect.left), static_cast<uint32_t>(rect.bottom - rect.top));
	static const bool trace = KYTY_ENV("KYTY_TRACE_RASTER_STATE") != nullptr;
	if (trace && (rect.left != viewport.screen_scissor_left || rect.top != viewport.screen_scissor_top ||
	              rect.right != viewport.screen_scissor_right || rect.bottom != viewport.screen_scissor_bottom)) {
		static std::set<std::array<int, 8>> seen;
		const std::array key {viewport.screen_scissor_left, viewport.screen_scissor_top,
		                      viewport.screen_scissor_right, viewport.screen_scissor_bottom,
		                      rect.left, rect.top, rect.right, rect.bottom};
		if (seen.size() < 256 && seen.insert(key).second) {
			std::fprintf(stderr, "[RasterScissor] rt=%ux%u screen=%d,%d,%d,%d final=%d,%d,%d,%d viewport_enabled=%d\n",
			             m_render_width, m_render_height, key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
			             scan_mode.vport_scissor_enable ? 1 : 0);
		}
	}
}

void MetalDrawExecutor::SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (!encoder) {
		return;
	}

	width = x >= m_render_width ? 0 : std::min(width, m_render_width - x);
	height = y >= m_render_height ? 0 : std::min(height, m_render_height - y);
	m_scissor_empty = width == 0 || height == 0;
	m_scissors={{x,y,width,height}};
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
		if (m_mesh_active) [encoder setMeshBuffer:buffer offset:static_cast<NSUInteger>(offset) atIndex:index];
		else [encoder setVertexBuffer:buffer offset:static_cast<NSUInteger>(offset) atIndex:index];
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
		if (m_mesh_active) [encoder setMeshBytes:bytes length:length atIndex:index];
		else [encoder setVertexBytes:bytes length:length atIndex:index];
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
		const uint32_t declaration = static_cast<uint32_t>(usage) | (m_mesh_active ? 0x10000u : 0x20000u);
		if ((mask & declaration) == declaration) return;
		mask |= declaration;
	}
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (encoder) {
		[encoder useResource:resource usage:usage stages:(m_mesh_active ? MTLRenderStageMesh : MTLRenderStageVertex) | MTLRenderStageFragment];
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

void MetalDrawExecutor::SetMeshState(const ShaderMeshInputInfo* mesh) {
	m_mesh_active = mesh != nullptr;
	if (mesh) m_mesh = *mesh;
}

void MetalDrawExecutor::DrawMesh(uint32_t count, uint32_t instances, uint32_t base_vertex,
                                 uint32_t first_instance, uint32_t index_bytes) {
	const uint32_t primitives = m_mesh.InputPrimitiveCount(count);
	if (primitives == 0 || instances == 0) return;
	if (m_mesh.primitives_per_group == 0) RejectMetalState("mesh draw without a primitive subgroup");
	const uint32_t groups = primitives / m_mesh.primitives_per_group + (primitives % m_mesh.primitives_per_group != 0);
	const uint32_t params[8] = {count, base_vertex, first_instance, index_bytes, 0, 0, 0, 0};
	SetVertexBytes(params, sizeof(params), 26);
	const uint32_t threads = m_mesh.threads_num[0] * m_mesh.threads_num[1] * m_mesh.threads_num[2];
	const uint32_t host_threads = m_mesh.wave_size == 64 ? threads / 2 : threads;
	auto encoder = MetalContext::Instance().GetActiveRenderEncoder();
	PassTimerItem();
	QueueGraphicsImageWritebacks();
	FaultSignpost(encoder);
	[encoder drawMeshThreadgroups:MTLSizeMake(groups, instances, 1)
	        threadsPerObjectThreadgroup:MTLSizeMake(1,1,1)
	        threadsPerMeshThreadgroup:MTLSizeMake(host_threads,1,1)];
	SplitAfterDraw();
}

void MetalDrawExecutor::Draw(Prospero::PrimitiveType prim_type,
                             uint32_t vertex_count,
                             uint32_t instance_count,
                             uint32_t first_vertex,
                             uint32_t first_instance) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (vertex_count != 0 && instance_count != 0 && (!encoder || !m_has_render_pipeline)) RejectMetalState("draw without an active encoder/pipeline");
	{
		static std::atomic<uint64_t> s_calls {0};
		static std::atomic<uint64_t> s_skipped {0};
		const uint64_t call = s_calls.fetch_add(1);
		if (m_scissor_empty || vertex_count == 0 || instance_count == 0) {
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

	if (m_mesh_active) { DrawMesh(vertex_count, instance_count, first_vertex, first_instance, 0); return; }

	MTLPrimitiveType mtl_prim = TranslatePrimitiveType(prim_type);
	// RECTLIST: three corners define a rectangle; the vertex shader wrapper synthesises vertex 3, so a single
	// rectangle is drawn as a four-vertex strip (two triangles).
	if (Prospero::IsRectList(prim_type) && vertex_count == 3) {
		mtl_prim     = MTLPrimitiveTypeTriangleStrip;
		vertex_count = 4;
	}
	PassTimerItem();
	QueueGraphicsImageWritebacks();
	FaultSignpost(encoder);
	[encoder drawPrimitives:mtl_prim
	            vertexStart:first_vertex
	            vertexCount:vertex_count
	          instanceCount:instance_count
	           baseInstance:first_instance];
	SplitAfterDraw();
}

void MetalDrawExecutor::DrawIndirect(Prospero::PrimitiveType prim_type, id<MTLBuffer> args_buffer,
                                     uint64_t args_offset, bool last_in_batch) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (m_scissor_empty) return;
	if (!encoder || !m_has_render_pipeline || args_buffer == nil) RejectMetalState("indirect draw without its pipeline or arguments");
	if (m_mesh_active) {
		SetVertexBuffer(args_buffer,args_offset+MetalMeshIndirect::ParametersOffset,26);
		const uint32_t threads=m_mesh.threads_num[0]*m_mesh.threads_num[1]*m_mesh.threads_num[2];
		PassTimerItem();
		QueueGraphicsImageWritebacks();
		FaultSignpost(encoder);
		[encoder drawMeshThreadgroupsWithIndirectBuffer:args_buffer indirectBufferOffset:args_offset
		    threadsPerObjectThreadgroup:MTLSizeMake(1,1,1) threadsPerMeshThreadgroup:MTLSizeMake(m_mesh.wave_size==64?threads/2:threads,1,1)];
		if (last_in_batch) SplitAfterDraw(); return;
	}
	// Expanded nonindexed RECTLIST arguments contain four vertices, which
	// must form two triangles. The indexed RECTLIST path uses a triangle list.
	const MTLPrimitiveType primitive = Prospero::IsRectList(prim_type) ? MTLPrimitiveTypeTriangleStrip
	                                                                : TranslatePrimitiveType(prim_type);
	PassTimerItem();
	QueueGraphicsImageWritebacks();
	FaultSignpost(encoder);
	[encoder drawPrimitives:primitive
	         indirectBuffer:args_buffer
	   indirectBufferOffset:args_offset];
	if (last_in_batch) SplitAfterDraw();
}

void MetalDrawExecutor::DrawIndexedIndirect(Prospero::PrimitiveType prim_type, MTLIndexType index_type,
                                             id<MTLBuffer> index_buffer, uint64_t index_offset,
                                             id<MTLBuffer> args_buffer, uint64_t args_offset, bool last_in_batch) {
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (m_scissor_empty) return;
	if (!encoder || !m_has_render_pipeline || index_buffer == nil || args_buffer == nil) RejectMetalState("indexed indirect draw without its pipeline, indices or arguments");
	if (m_mesh_active) {
		SetVertexBuffer(index_buffer,index_offset,24); UseResource(index_buffer,MTLResourceUsageRead);
		DrawIndirect(prim_type,args_buffer,args_offset,last_in_batch); return;
	}
	PassTimerItem();
	QueueGraphicsImageWritebacks();
	FaultSignpost(encoder);
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
	id<MTLRenderCommandEncoder> encoder = MetalContext::Instance().GetActiveRenderEncoder();
	if (m_scissor_empty || index_count == 0 || instance_count == 0) return;
	if (!encoder || !m_has_render_pipeline || !index_buffer) RejectMetalState("indexed draw without its pipeline or indices");

	if (m_mesh_active) { SetVertexBuffer(index_buffer, index_buffer_offset, 24); UseResource(index_buffer, MTLResourceUsageRead); DrawMesh(index_count, instance_count, uint32_t(base_vertex), first_instance, index_type == MTLIndexTypeUInt16 ? 2 : 4); return; }

	const MTLPrimitiveType mtl_prim = TranslatePrimitiveType(prim_type);
	PassTimerItem();
	QueueGraphicsImageWritebacks();
	FaultSignpost(encoder);
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
	// KYTY_CS_OVERLAP: the wait for the earlier encoders is recorded right before the dispatch, when the memory it touches is known.
	m_access.clear();
	m_access_unknown = false;
	m_order_pending  = CsOverlapEnabled();
	if (!m_order_pending) OrderBegin(encoder);
	if (FaultInfoOn()) encoder.label = @"compute";

	MetalContext::Instance().SetActiveComputeEncoder(encoder);
	m_has_compute_pipeline = false;
	return true;
}

void MetalDrawExecutor::ComputeAccessAdd(uint64_t begin, uint64_t end, bool write) {
	if (end <= begin) end = begin + 1;
	m_access.push_back({begin, end, write});
}

void MetalDrawExecutor::ComputeAccessUnknown() {
	m_access_unknown = true;
}

void MetalDrawExecutor::BeginDispatchOrder(id<MTLComputeCommandEncoder> encoder) {
	if (!m_order_pending) return;
	m_order_pending = false;
	std::vector<OrderAccess> acc;
	acc.reserve(m_access.size());
	for (const auto& r: m_access) acc.push_back({r.begin, r.end, r.write});
	// a dispatch that declared nothing cannot be shown independent
	OrderBeginCompute(encoder, acc.data(), acc.size(), m_access_unknown || acc.empty());
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
	if (group_x && group_y && group_z && (!encoder || !m_has_compute_pipeline)) RejectMetalState("dispatch without an active encoder/pipeline");
	if (group_x == 0 || group_y == 0 || group_z == 0) {
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
	BeginDispatchOrder(encoder);
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
	if (!encoder || !m_has_compute_pipeline || !indirect_args) RejectMetalState("indirect dispatch without an active encoder, pipeline or arguments");
	if (indirect_offset % 4 != 0 || indirect_offset > indirect_args.length ||
	    indirect_args.length - indirect_offset < sizeof(MTLDispatchThreadgroupsIndirectArguments)) RejectMetalState("indirect dispatch argument range");

	MTLSize threads_per_group = MTLSizeMake(threads_per_group_x, threads_per_group_y, threads_per_group_z);
	// MetalRenderer prepares these counts on the GPU, in order after their
	// producer. Host reads/writes here would race that producer.
	CountDispatchStat(2);
	PassTimerItem();
	BeginDispatchOrder(encoder);
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
