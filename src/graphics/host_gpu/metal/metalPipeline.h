#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALPIPELINE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALPIPELINE_H_

#include "common/common.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
typedef void* id;
typedef unsigned long NSUInteger;
enum MTLPixelFormat : NSUInteger;
enum MTLCompareFunction : NSUInteger;
enum MTLStencilOperation : NSUInteger;
enum MTLBlendFactor : NSUInteger;
enum MTLBlendOperation : NSUInteger;
enum MTLCullMode : NSUInteger;
enum MTLWinding : NSUInteger;
enum MTLTriangleFillMode : NSUInteger;
enum MTLDepthClipMode : NSUInteger;
enum MTLColorWriteMask : NSUInteger;
#endif

namespace Libs::Graphics::Metal {

// Mapping helper functions
MTLCompareFunction   TranslateCompareFunction(uint8_t func);
MTLStencilOperation  TranslateStencilOperation(uint8_t op);
MTLBlendFactor       TranslateBlendFactor(uint8_t factor);
MTLBlendOperation    TranslateBlendOperation(uint8_t op);
MTLCullMode          TranslateCullMode(const HW::ModeControl& mode);
MTLWinding           TranslateWinding(bool face);
MTLTriangleFillMode  TranslateFillMode(const HW::ModeControl& mode);
MTLDepthClipMode     TranslateDepthClipMode(const HW::ClipControl& clip);
MTLColorWriteMask    TranslateColorWriteMask(uint8_t mask);

struct MetalRasterizerState {
	MTLCullMode         cull_mode         = static_cast<MTLCullMode>(0); // None
	MTLWinding          winding           = static_cast<MTLWinding>(0);  // Clockwise
	MTLTriangleFillMode fill_mode         = static_cast<MTLTriangleFillMode>(0); // Fill
	MTLDepthClipMode    depth_clip_mode   = static_cast<MTLDepthClipMode>(0); // Clip
	bool                depth_bias_enable = false;
	float               depth_bias_constant = 0.0f;
	float               depth_bias_slope  = 0.0f;
	float               depth_bias_clamp  = 0.0f;
};

struct MetalDepthStencilKey {
	uint32_t depth_control   = 0;
	uint32_t stencil_control = 0;
	uint32_t stencil_mask    = 0;
	uint32_t stencil_extra   = 0;
	uint32_t stencil_flags   = 0;

	bool operator==(const MetalDepthStencilKey& other) const noexcept {
		return depth_control == other.depth_control &&
		       stencil_control == other.stencil_control &&
		       stencil_mask == other.stencil_mask &&
		       stencil_extra == other.stencil_extra &&
		       stencil_flags == other.stencil_flags;
	}
};

struct MetalDepthStencilKeyHash {
	size_t operator()(const MetalDepthStencilKey& k) const noexcept {
		size_t h = k.depth_control;
		h ^= static_cast<size_t>(k.stencil_control) + 0x9e3779b9 + (h << 6) + (h >> 2);
		h ^= static_cast<size_t>(k.stencil_mask) + 0x9e3779b9 + (h << 6) + (h >> 2);
		h ^= static_cast<size_t>(k.stencil_extra) + 0x9e3779b9 + (h << 6) + (h >> 2);
		h ^= static_cast<size_t>(k.stencil_flags) + 0x9e3779b9 + (h << 6) + (h >> 2);
		return h;
	}
};

// Stencil reference values of one draw (Metal keeps them in encoder state, not in the depth-stencil state object).
struct MetalStencilRefs {
	uint32_t front = 0;
	uint32_t back  = 0;
};

struct MetalColorAttachmentDesc {
	MTLPixelFormat    pixel_format     = static_cast<MTLPixelFormat>(0); // Invalid
	bool              blending_enabled = false;
	MTLColorWriteMask write_mask       = static_cast<MTLColorWriteMask>(0xF); // All
	MTLBlendFactor    src_rgb          = static_cast<MTLBlendFactor>(1);     // One
	MTLBlendFactor    dst_rgb          = static_cast<MTLBlendFactor>(0);     // Zero
	MTLBlendOperation rgb_op           = static_cast<MTLBlendOperation>(0);  // Add
	MTLBlendFactor    src_alpha        = static_cast<MTLBlendFactor>(1);     // One
	MTLBlendFactor    dst_alpha        = static_cast<MTLBlendFactor>(0);     // Zero
	MTLBlendOperation alpha_op         = static_cast<MTLBlendOperation>(0);  // Add
};

struct MetalRenderPipelineKey {
	uint64_t                                       vs_hash         = 0;
	uint64_t                                       fs_hash         = 0;
	std::array<MetalColorAttachmentDesc, 8>         color_targets   = {};
	uint32_t                                       color_target_count = 0;
	MTLPixelFormat                                 depth_format    = static_cast<MTLPixelFormat>(0);
	MTLPixelFormat                                 stencil_format  = static_cast<MTLPixelFormat>(0);
	uint32_t                                       sample_count    = 1;
	uint32_t                                       primitive_type  = 0;

	bool operator==(const MetalRenderPipelineKey& other) const noexcept;
};

struct MetalRenderPipelineKeyHash {
	size_t operator()(const MetalRenderPipelineKey& k) const noexcept;
};

// A guest depth-only pass may leave color attachments and blend registers bound.
// Preserve their formats for render-pass compatibility while disabling unused color writes.
void MaskUnwrittenColorAttachments(MetalRenderPipelineKey& key, uint32_t color_export_mask);

// Keep slot numbers (including holes) consistent between the pass and its pipeline.
uint32_t NativeColorTargetMask(const HW::Context& context);
void ConfigureNativeColorTarget(MetalRenderPipelineKey& key, const HW::Context& context,
                                uint32_t slot, MTLPixelFormat format);

class MetalPipelineManager {
public:
	static MetalPipelineManager& Instance();

	MetalPipelineManager();
	~MetalPipelineManager();

	KYTY_CLASS_NO_COPY(MetalPipelineManager);

#ifdef __OBJC__
	// Translate DB_DEPTH_CONTROL, StencilControl and StencilMask into MTLDepthStencilDescriptor / State
	[[nodiscard]] id<MTLDepthStencilState> GetOrCreateDepthStencilState(
	    const HW::DepthControl& depth_ctrl,
	    const HW::StencilControl& stencil_ctrl,
	    const HW::StencilMask& stencil_mask);

	// Same, for a pass that has a stencil attachment: the guest stencil test, operations and masks are translated (the
	// reference values come back in `refs`). `stencil_ops_disabled` zeroes the write masks (stencil clear / write-disable).
	[[nodiscard]] id<MTLDepthStencilState> GetOrCreateDepthStencilState(
	    const HW::DepthControl& depth_ctrl,
	    const HW::StencilControl& stencil_ctrl,
	    const HW::StencilMask& stencil_mask,
	    bool has_stencil_attachment,
	    bool stencil_ops_disabled,
	    MetalStencilRefs* refs);

	// Translate PA_SU_SC_MODE_CNTL and PA_CL_CLIP_CNTL into rasterizer state
	[[nodiscard]] MetalRasterizerState TranslateRasterizerState(
	    const HW::ModeControl& mode_ctrl,
	    const HW::ClipControl& clip_ctrl);

	// Translate CB_COLOR_CONTROL, CB_BLEND_CONTROL, and render target formats into pipeline descriptor
	[[nodiscard]] id<MTLRenderPipelineState> GetOrCreateRenderPipeline(
	    const MetalRenderPipelineKey& key,
	    id<MTLFunction> vertex_function,
	    id<MTLFunction> fragment_function);

	[[nodiscard]] id<MTLRenderPipelineState> GetOrCreateRenderPipeline(
	    const MetalRenderPipelineKey& key,
	    void* vertex_function,
	    void* fragment_function);

	// Compute pipeline creation
	[[nodiscard]] id<MTLComputePipelineState> GetOrCreateComputePipeline(
	    uint64_t cs_hash,
	    id<MTLFunction> compute_function);

	[[nodiscard]] id<MTLComputePipelineState> GetOrCreateComputePipeline(
	    uint64_t cs_hash,
	    void* compute_function);
#else
	[[nodiscard]] id GetOrCreateDepthStencilState(
	    const HW::DepthControl& depth_ctrl,
	    const HW::StencilControl& stencil_ctrl,
	    const HW::StencilMask& stencil_mask);

	[[nodiscard]] id GetOrCreateDepthStencilState(
	    const HW::DepthControl& depth_ctrl,
	    const HW::StencilControl& stencil_ctrl,
	    const HW::StencilMask& stencil_mask,
	    bool has_stencil_attachment,
	    bool stencil_ops_disabled,
	    MetalStencilRefs* refs);

	[[nodiscard]] MetalRasterizerState TranslateRasterizerState(
	    const HW::ModeControl& mode_ctrl,
	    const HW::ClipControl& clip_ctrl);

	[[nodiscard]] id GetOrCreateRenderPipeline(
	    const MetalRenderPipelineKey& key,
	    void* vertex_function,
	    void* fragment_function);

	[[nodiscard]] id GetOrCreateComputePipeline(
	    uint64_t cs_hash,
	    void* compute_function);
#endif

	void ClearCaches();

private:
#ifdef __OBJC__
	std::unordered_map<MetalDepthStencilKey, id<MTLDepthStencilState>, MetalDepthStencilKeyHash>
	    m_depth_stencil_cache;
	std::unordered_map<MetalRenderPipelineKey, id<MTLRenderPipelineState>, MetalRenderPipelineKeyHash>
	    m_render_pipeline_cache;
	std::unordered_map<uint64_t, id<MTLComputePipelineState>>
	    m_compute_pipeline_cache;
#endif
};

} // namespace Libs::Graphics::Metal

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALPIPELINE_H_
