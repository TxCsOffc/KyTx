#include "graphics/host_gpu/metal/metalDepthBounds.h"
#include "graphics/host_gpu/metal/metalEnv.h"
#include "graphics/host_gpu/metal/metalPipeline.h"
#include "graphics/host_gpu/metal/metalContext.h"
#include "graphics/host_gpu/metal/metalRenderer.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/metal/metalUnsupported.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <atomic>

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

namespace Libs::Graphics::Metal {

uint32_t NativeColorTargetMask(const HW::Context& context) {
	const uint32_t write_mask = context.GetRenderTargetMask() & context.GetShaderRegisters().m_cbShaderMask;
	uint32_t slots = 0;
	for (uint32_t i = 0; i < 8u; ++i) {
		// Preserve RT0 for raster/depth-only draws whose color writes are disabled.
		if (context.GetRenderTarget(i).base.addr != 0 && (i == 0 || ((write_mask >> (i * 4u)) & 0xfu) != 0))
			slots |= 1u << i;
	}
	return slots;
}

bool NativePixelShaderActive(const HW::Context& context, uint64_t shader_address) {
	if (shader_address == 0) return false;
	const auto& sh = context.GetShaderRegisters();
	const auto& db = sh.db_shader_control;
	if (db.shader_kill_enable || db.shader_z_export_enable || db.shader_mask_export_enable ||
	    db.shader_dual_export_enable || db.shader_execute_on_noop) return true;
	if (context.GetColorControl().mode == 0u) return false;
	const uint32_t write_mask = context.GetRenderTargetMask() & sh.m_cbShaderMask;
	for (uint32_t slot = 0; slot < 8u; ++slot)
		if (sh.target_output_mode[slot] != 0 && ((write_mask >> (4u * slot)) & 0xfu) != 0)
			return true;
	return false;
}

void ConfigureNativeColorTarget(MetalRenderPipelineKey& key, const HW::Context& context,
                                uint32_t slot, MTLPixelFormat format) {
	if (slot >= key.color_targets.size()) return;
	auto& color = key.color_targets[slot];
	const auto& rt = context.GetRenderTarget(slot);
	const auto& bc = context.GetBlendControl(slot);
	color.pixel_format = format;
	color.write_mask = TranslateColorWriteMask(
	    ((context.GetRenderTargetMask() & context.GetShaderRegisters().m_cbShaderMask) >> (slot * 4u)) & 0xfu);
	color.blending_enabled = bc.enable && !rt.info.blend_bypass && color.write_mask != MTLColorWriteMaskNone;
	if (color.blending_enabled) {
		color.src_rgb = TranslateBlendFactor(bc.color_srcblend);
		color.dst_rgb = TranslateBlendFactor(bc.color_destblend);
		color.rgb_op = TranslateBlendOperation(bc.color_comb_fcn);
		color.src_alpha = TranslateBlendFactor(bc.separate_alpha_blend ? bc.alpha_srcblend : bc.color_srcblend);
		color.dst_alpha = TranslateBlendFactor(bc.separate_alpha_blend ? bc.alpha_destblend : bc.color_destblend);
		color.alpha_op = TranslateBlendOperation(bc.separate_alpha_blend ? bc.alpha_comb_fcn : bc.color_comb_fcn);
	}
	key.color_target_count = std::max(key.color_target_count, slot + 1u);
}

void MaskUnwrittenColorAttachments(MetalRenderPipelineKey& key, uint32_t color_export_mask) {
	static const bool log_exports = KYTY_ENV("KYTY_LOG_COLOR_EXPORTS") != nullptr;
	static std::atomic<uint32_t> logged {0};
	for (uint32_t i = 0; i < key.color_target_count && i < key.color_targets.size(); ++i) {
		auto& color = key.color_targets[i];
		if ((color_export_mask & (1u << i)) == 0) {
			if (log_exports && color.blending_enabled && logged.fetch_add(1) < 24)
				std::fprintf(stderr, "[ColorExports] ps=0x%016llx slot=%u exports=%x old_mask=%lu blend=%lu/%lu/%lu/%lu fmt=%lu\n",
				             (unsigned long long)key.fs_hash, i, color_export_mask, (unsigned long)color.write_mask,
				             (unsigned long)color.src_rgb, (unsigned long)color.dst_rgb,
				             (unsigned long)color.src_alpha, (unsigned long)color.dst_alpha, (unsigned long)color.pixel_format);
			color.write_mask = MTLColorWriteMaskNone;
		}
		if (color.write_mask == MTLColorWriteMaskNone) color.blending_enabled = false;
	}
}

bool MetalRenderPipelineKey::operator==(const MetalRenderPipelineKey& other) const noexcept {
	if (vs_hash != other.vs_hash || fs_hash != other.fs_hash ||
	    color_target_count != other.color_target_count ||
	    depth_format != other.depth_format || stencil_format != other.stencil_format ||
	    sample_count != other.sample_count || primitive_type != other.primitive_type) {
		return false;
	}
	for (uint32_t i = 0; i < color_target_count; ++i) {
		const auto& a = color_targets[i];
		const auto& b = other.color_targets[i];
		if (a.pixel_format != b.pixel_format || a.blending_enabled != b.blending_enabled ||
		    a.write_mask != b.write_mask || a.src_rgb != b.src_rgb || a.dst_rgb != b.dst_rgb ||
		    a.rgb_op != b.rgb_op || a.src_alpha != b.src_alpha || a.dst_alpha != b.dst_alpha ||
		    a.alpha_op != b.alpha_op) {
			return false;
		}
	}
	return true;
}

size_t MetalRenderPipelineKeyHash::operator()(const MetalRenderPipelineKey& k) const noexcept {
	size_t h = k.vs_hash ^ (k.fs_hash << 1);
	h ^= static_cast<size_t>(k.depth_format) + 0x9e3779b9 + (h << 6) + (h >> 2);
	h ^= static_cast<size_t>(k.stencil_format) + 0x9e3779b9 + (h << 6) + (h >> 2);
	h ^= static_cast<size_t>(k.color_target_count) + 0x9e3779b9 + (h << 6) + (h >> 2);
	for (uint32_t i = 0; i < k.color_target_count; ++i) {
		h ^= static_cast<size_t>(k.color_targets[i].pixel_format) + 0x9e3779b9 + (h << 6) + (h >> 2);
	}
	return h;
}

MTLCompareFunction TranslateCompareFunction(uint8_t func) {
	switch (func) {
		case 0: return MTLCompareFunctionNever;
		case 1: return MTLCompareFunctionLess;
		case 2: return MTLCompareFunctionEqual;
		case 3: return MTLCompareFunctionLessEqual;
		case 4: return MTLCompareFunctionGreater;
		case 5: return MTLCompareFunctionNotEqual;
		case 6: return MTLCompareFunctionGreaterEqual;
		case 7: return MTLCompareFunctionAlways;
		default: RejectMetalState("depth/stencil compare function " + std::to_string(func));
	}
}

MTLStencilOperation TranslateStencilOperation(uint8_t op) {
	switch (op) {
		case 0: return MTLStencilOperationKeep;
		case 1: return MTLStencilOperationZero;
		case 2: return MTLStencilOperationReplace;
		case 3: return MTLStencilOperationIncrementClamp;
		case 4: return MTLStencilOperationDecrementClamp;
		case 5: return MTLStencilOperationInvert;
		case 6: return MTLStencilOperationIncrementWrap;
		case 7: return MTLStencilOperationDecrementWrap;
		default: RejectMetalState("stencil operation " + std::to_string(op));
	}
}

MTLBlendFactor TranslateBlendFactor(uint8_t factor) {
	switch (static_cast<Prospero::BlendFactor>(factor)) {
		case Prospero::BlendFactor::kZero: return MTLBlendFactorZero;
		case Prospero::BlendFactor::kOne: return MTLBlendFactorOne;
		case Prospero::BlendFactor::kSrcColor: return MTLBlendFactorSourceColor;
		case Prospero::BlendFactor::kOneMinusSrcColor: return MTLBlendFactorOneMinusSourceColor;
		case Prospero::BlendFactor::kSrcAlpha: return MTLBlendFactorSourceAlpha;
		case Prospero::BlendFactor::kOneMinusSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
		case Prospero::BlendFactor::kDstColor: return MTLBlendFactorDestinationColor;
		case Prospero::BlendFactor::kOneMinusDstColor: return MTLBlendFactorOneMinusDestinationColor;
		case Prospero::BlendFactor::kDstAlpha: return MTLBlendFactorDestinationAlpha;
		case Prospero::BlendFactor::kOneMinusDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
		case Prospero::BlendFactor::kSrcAlphaSaturate: return MTLBlendFactorSourceAlphaSaturated;
		case Prospero::BlendFactor::kConstantColor: return MTLBlendFactorBlendColor;
		case Prospero::BlendFactor::kOneMinusConstantColor: return MTLBlendFactorOneMinusBlendColor;
		case Prospero::BlendFactor::kSrc1Color: return MTLBlendFactorSource1Color;
		case Prospero::BlendFactor::kOneMinusSrc1Color: return MTLBlendFactorOneMinusSource1Color;
		case Prospero::BlendFactor::kSrc1Alpha: return MTLBlendFactorSource1Alpha;
		case Prospero::BlendFactor::kOneMinusSrc1Alpha: return MTLBlendFactorOneMinusSource1Alpha;
		case Prospero::BlendFactor::kConstantAlpha: return MTLBlendFactorBlendAlpha;
		case Prospero::BlendFactor::kOneMinusConstantAlpha: return MTLBlendFactorOneMinusBlendAlpha;
		default: RejectMetalState("blend factor " + std::to_string(factor));
	}
}

MTLBlendOperation TranslateBlendOperation(uint8_t op) {
	switch (static_cast<Prospero::BlendOp>(op)) {
		case Prospero::BlendOp::kAdd: return MTLBlendOperationAdd;
		case Prospero::BlendOp::kSubtract: return MTLBlendOperationSubtract;
		case Prospero::BlendOp::kMin: return MTLBlendOperationMin;
		case Prospero::BlendOp::kMax: return MTLBlendOperationMax;
		case Prospero::BlendOp::kReverseSubtract: return MTLBlendOperationReverseSubtract;
		default: RejectMetalState("blend operation " + std::to_string(op));
	}
}

MTLCullMode TranslateCullMode(const HW::ModeControl& mode) {
	if (mode.cull_front && mode.cull_back) {
		RejectMetalState("culling both primitive faces requires a raster-discard pipeline");
	}
	if (mode.cull_front) {
		return MTLCullModeFront;
	}
	if (mode.cull_back) {
		return MTLCullModeBack;
	}
	return MTLCullModeNone;
}

MTLWinding TranslateWinding(bool face) {
	// face: 0 = CCW, 1 = CW
	return face ? MTLWindingClockwise : MTLWindingCounterClockwise;
}

MTLTriangleFillMode TranslateFillMode(const HW::ModeControl& mode) {
	// poly_mode only enables per-face polygon modes; the mode itself is the ptype
	// (0 = points, 1 = lines, 2 = triangles), as in the Vulkan ResolvePolygonMode.
	if (mode.poly_mode == 0 || (mode.cull_front && mode.cull_back)) {
		return MTLTriangleFillModeFill;
	}
	const auto ptype = mode.cull_front ? mode.polymode_back_ptype : mode.polymode_front_ptype;
	return ptype == 1 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
}

MTLDepthClipMode TranslateDepthClipMode(const HW::ClipControl& clip) {
	if (clip.clip_disable) {
		return MTLDepthClipModeClamp;
	}
	return clip.IsZClipEnabled() ? MTLDepthClipModeClip : MTLDepthClipModeClamp;
}

MTLColorWriteMask TranslateColorWriteMask(uint8_t mask) {
	MTLColorWriteMask m = MTLColorWriteMaskNone;
	if (mask & 0x1) m |= MTLColorWriteMaskRed;
	if (mask & 0x2) m |= MTLColorWriteMaskGreen;
	if (mask & 0x4) m |= MTLColorWriteMaskBlue;
	if (mask & 0x8) m |= MTLColorWriteMaskAlpha;
	return m;
}

MetalPipelineManager& MetalPipelineManager::Instance() {
	static MetalPipelineManager instance;
	return instance;
}

MetalPipelineManager::MetalPipelineManager() = default;

MetalPipelineManager::~MetalPipelineManager() {
	ClearCaches();
}

void MetalPipelineManager::ClearCaches() {
	@autoreleasepool {
		m_fixed_depth_stencil_fragment = nil;
		m_depth_stencil_cache.clear();
		m_render_pipeline_cache.clear();
		m_compute_pipeline_cache.clear();
	}
}

namespace {

struct StencilFace {
	MTLCompareFunction compare    = MTLCompareFunctionAlways;
	MTLStencilOperation fail      = MTLStencilOperationKeep;
	MTLStencilOperation depth_fail = MTLStencilOperationKeep;
	MTLStencilOperation pass      = MTLStencilOperationKeep;
	uint32_t           read_mask  = 0;
	uint32_t           write_mask = 0;
	uint32_t           reference  = 0;
};

// Guest (DB_STENCIL_CONTROL) operation -> Metal, as the Vulkan backend's ConvertStencilOp.
MTLStencilOperation ConvertGuestStencilOp(uint8_t value, uint8_t write_mask, uint8_t op_value) {
	if (write_mask == 0) {
		return MTLStencilOperationKeep;
	}
	switch (static_cast<Prospero::StencilOp>(value)) {
		case Prospero::StencilOp::kKeep: return MTLStencilOperationKeep;
		case Prospero::StencilOp::kZero: return MTLStencilOperationZero;
		case Prospero::StencilOp::kReplaceTest: return MTLStencilOperationReplace;
		case Prospero::StencilOp::kReplaceOp: return (op_value & write_mask) == 0 ? MTLStencilOperationZero : MTLStencilOperationReplace;
		case Prospero::StencilOp::kAddClamp: return MTLStencilOperationIncrementClamp;
		case Prospero::StencilOp::kSubClamp: return MTLStencilOperationDecrementClamp;
		case Prospero::StencilOp::kInvert: return MTLStencilOperationInvert;
		case Prospero::StencilOp::kAddWrap: return MTLStencilOperationIncrementWrap;
		case Prospero::StencilOp::kSubWrap: return MTLStencilOperationDecrementWrap;
		case Prospero::StencilOp::kXor:
			if ((write_mask & op_value) == 0) return MTLStencilOperationKeep;
			if ((write_mask & ~op_value) != 0) {
				RejectMetalState("stencil XOR requires per-operation write masks");
			}
			return MTLStencilOperationInvert;
		default: RejectMetalState("guest stencil operation " + std::to_string(value));
	}
}

// One face. Metal, like Vulkan, has a single reference value per face that serves the comparison and every REPLACE, so the
// guest's separate test / operation values are folded into it exactly as the Vulkan backend does.
StencilFace ConvertGuestStencilFace(uint8_t compare, uint8_t fail_op, uint8_t zpass_op, uint8_t zfail_op, uint8_t op_value,
                                    uint8_t compare_mask, uint8_t write_mask, uint8_t test_value) {
	const std::array<uint8_t, 3> operations = {fail_op, zpass_op, zfail_op};
	const auto keep = uint8_t(Prospero::StencilOp::kKeep), xor_op = uint8_t(Prospero::StencilOp::kXor);
	const bool has_xor = std::find(operations.begin(), operations.end(), xor_op) != operations.end();
	if (has_xor && (write_mask & ~op_value) != 0) {
		// For XOR/KEEP-only faces, restricting the common mask is exact: only
		// bits selected by the XOR operand change, on every stencil/depth path.
		if (!std::all_of(operations.begin(), operations.end(), [&](uint8_t op) { return op == keep || op == xor_op; }))
			RejectMetalState("mixed stencil XOR requires per-operation write masks");
		write_mask &= op_value;
	}
	uint8_t reference     = test_value;
	uint8_t required_bits = compare_mask;
	if (compare == 7u /*always*/ || compare == 0u /*never*/) {
		required_bits = 0;
	}
	std::array<MTLStencilOperation, 3> converted {};
	for (size_t i = 0; i < operations.size(); ++i) {
		converted[i] = ConvertGuestStencilOp(operations[i], write_mask, op_value);
		if (converted[i] != MTLStencilOperationReplace) {
			continue;
		}
		uint8_t replacement = test_value;
		if (static_cast<Prospero::StencilOp>(operations[i]) == Prospero::StencilOp::kReplaceOp) {
			replacement = op_value;
		}
		if (((reference ^ replacement) & required_bits & write_mask) != 0) {
			RejectMetalState("stencil replacement conflicts with its comparison reference");
		}
		reference     = static_cast<uint8_t>((reference & ~write_mask) | (replacement & write_mask));
		required_bits = static_cast<uint8_t>(required_bits | write_mask);
	}
	StencilFace face;
	face.compare    = TranslateCompareFunction(compare);
	face.fail       = converted[0];
	face.pass       = converted[1];
	face.depth_fail = converted[2];
	face.read_mask  = compare_mask;
	face.write_mask = write_mask;
	face.reference  = reference;
	return face;
}

} // namespace

id<MTLDepthStencilState> MetalPipelineManager::GetOrCreateDepthStencilState(
    const HW::DepthControl& depth_ctrl,
    const HW::StencilControl& stencil_ctrl,
    const HW::StencilMask& stencil_mask) {
	return GetOrCreateDepthStencilState(depth_ctrl, stencil_ctrl, stencil_mask, false, false, nullptr);
}

id<MTLDepthStencilState> MetalPipelineManager::GetOrCreateDepthStencilState(
    const HW::DepthControl& depth_ctrl,
    const HW::StencilControl& stencil_ctrl,
    const HW::StencilMask& stencil_mask,
    bool has_stencil_attachment,
    bool stencil_ops_disabled,
    MetalStencilRefs* refs) {
	if (depth_ctrl.depth_bounds_enable && !SupportsNativeDepthBounds(MetalContext::Instance().GetDevice()))
		RejectMetalState("depth bounds requires Apple10 and macOS26 or a software lowering");
	const bool use_stencil = has_stencil_attachment && depth_ctrl.stencil_enable;
	StencilFace front, back;
	if (use_stencil) {
		front = ConvertGuestStencilFace(depth_ctrl.stencilfunc, stencil_ctrl.stencil_fail, stencil_ctrl.stencil_zpass,
		                                stencil_ctrl.stencil_zfail, stencil_mask.stencil_opval, stencil_mask.stencil_mask,
		                                stencil_ops_disabled ? 0 : stencil_mask.stencil_writemask, stencil_mask.stencil_testval);
		back = depth_ctrl.backface_enable
		           ? ConvertGuestStencilFace(depth_ctrl.stencilfunc_bf, stencil_ctrl.stencil_fail_bf, stencil_ctrl.stencil_zpass_bf,
		                                     stencil_ctrl.stencil_zfail_bf, stencil_mask.stencil_opval_bf, stencil_mask.stencil_mask_bf,
		                                     stencil_ops_disabled ? 0 : stencil_mask.stencil_writemask_bf, stencil_mask.stencil_testval_bf)
		           : front;
	}
	if (refs != nullptr) {
		refs->front = front.reference;
		refs->back  = back.reference;
	}

	MetalDepthStencilKey key {};
	key.depth_control = (depth_ctrl.z_enable ? 1u : 0u) |
	                    ((depth_ctrl.z_write_enable ? 1u : 0u) << 1u) |
	                    ((depth_ctrl.depth_bounds_enable ? 1u : 0u) << 2u) |
	                    ((depth_ctrl.zfunc & 0x7u) << 3u) |
	                    ((depth_ctrl.stencil_enable ? 1u : 0u) << 6u) |
	                    ((depth_ctrl.backface_enable ? 1u : 0u) << 7u) |
	                    ((depth_ctrl.stencilfunc & 0x7u) << 8u) |
	                    ((depth_ctrl.stencilfunc_bf & 0x7u) << 11u);

	key.stencil_control = (stencil_ctrl.stencil_fail & 0xfu) |
	                      ((stencil_ctrl.stencil_zfail & 0xfu) << 4u) |
	                      ((stencil_ctrl.stencil_zpass & 0xfu) << 8u) |
	                      ((stencil_ctrl.stencil_fail_bf & 0xfu) << 12u) |
	                      ((stencil_ctrl.stencil_zfail_bf & 0xfu) << 16u) |
	                      ((stencil_ctrl.stencil_zpass_bf & 0xfu) << 20u);

	key.stencil_mask = stencil_mask.stencil_testval |
	                   (static_cast<uint32_t>(stencil_mask.stencil_mask) << 8u) |
	                   (static_cast<uint32_t>(stencil_mask.stencil_writemask) << 16u) |
	                   (static_cast<uint32_t>(stencil_mask.stencil_mask_bf) << 24u);
	key.stencil_extra = stencil_mask.stencil_testval_bf |
	                    (static_cast<uint32_t>(stencil_mask.stencil_writemask_bf) << 8u) |
	                    (static_cast<uint32_t>(stencil_mask.stencil_opval) << 16u) |
	                    (static_cast<uint32_t>(stencil_mask.stencil_opval_bf) << 24u);
	key.stencil_flags = (has_stencil_attachment ? 1u : 0u) | (stencil_ops_disabled ? 2u : 0u);

	auto it = m_depth_stencil_cache.find(key);
	if (it != m_depth_stencil_cache.end()) {
		return it->second;
	}

	@autoreleasepool {
		MTLDepthStencilDescriptor* desc = [[MTLDepthStencilDescriptor alloc] init];
		if (depth_ctrl.z_enable) {
			desc.depthCompareFunction = TranslateCompareFunction(depth_ctrl.zfunc);
			desc.depthWriteEnabled = depth_ctrl.z_write_enable ? YES : NO;
		} else {
			desc.depthCompareFunction = MTLCompareFunctionAlways;
			desc.depthWriteEnabled = NO;
		}

		// Stencil state is only valid (and only needed) when the pass has a stencil attachment.
		if (use_stencil) {
			const auto make_face = [](const StencilFace& f) {
				MTLStencilDescriptor* d = [[MTLStencilDescriptor alloc] init];
				d.stencilCompareFunction = f.compare;
				d.stencilFailureOperation = f.fail;
				d.depthFailureOperation = f.depth_fail;
				d.depthStencilPassOperation = f.pass;
				d.readMask = f.read_mask;
				d.writeMask = f.write_mask;
				return d;
			};
			desc.frontFaceStencil = make_face(front);
			desc.backFaceStencil = make_face(back);
		}

		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		if (!device) {
			return nil;
		}

		id<MTLDepthStencilState> state = [device newDepthStencilStateWithDescriptor:desc];
		m_depth_stencil_cache.emplace(key, state);
		return state;
	}
}

MetalRasterizerState MetalPipelineManager::TranslateRasterizerState(
    const HW::ModeControl& mode_ctrl,
    const HW::ClipControl& clip_ctrl) {
	MetalRasterizerState state {};
	state.cull_mode = TranslateCullMode(mode_ctrl);
	state.winding = TranslateWinding(mode_ctrl.face);
	state.fill_mode = TranslateFillMode(mode_ctrl);
	state.depth_clip_mode = TranslateDepthClipMode(clip_ctrl);
	state.depth_bias_enable = mode_ctrl.poly_offset_front_enable || mode_ctrl.poly_offset_back_enable;
	return state;
}

id<MTLRenderPipelineState> MetalPipelineManager::GetOrCreateRenderPipeline(
    const MetalRenderPipelineKey& key,
    id<MTLFunction> vertex_function,
    id<MTLFunction> fragment_function) {
	const MetalRenderPipelineKey& cache_key = key;
	auto it = m_render_pipeline_cache.find(cache_key);
	if (it != m_render_pipeline_cache.end()) {
		return it->second;
	}

	@autoreleasepool {
		MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
		desc.label = [NSString stringWithFormat:@"VS %016llx PS %016llx",(unsigned long long)key.vs_hash,(unsigned long long)key.fs_hash];
		const bool mesh_pipeline = vertex_function.functionType == MTLFunctionTypeMesh;
		if (!mesh_pipeline) desc.vertexFunction = vertex_function;
		desc.fragmentFunction = fragment_function;

		for (uint32_t i = 0; i < key.color_target_count; ++i) {
			const auto& color_desc = key.color_targets[i];
			if (color_desc.pixel_format == MTLPixelFormatInvalid) {
				continue;
			}
			MTLRenderPipelineColorAttachmentDescriptor* color_att = desc.colorAttachments[i];
			color_att.pixelFormat = color_desc.pixel_format;
			color_att.writeMask = color_desc.write_mask;
			color_att.blendingEnabled = color_desc.blending_enabled ? YES : NO;

			if (color_desc.blending_enabled) {
				color_att.sourceRGBBlendFactor = color_desc.src_rgb;
				color_att.destinationRGBBlendFactor = color_desc.dst_rgb;
				color_att.rgbBlendOperation = color_desc.rgb_op;
				color_att.sourceAlphaBlendFactor = color_desc.src_alpha;
				color_att.destinationAlphaBlendFactor = color_desc.dst_alpha;
				color_att.alphaBlendOperation = color_desc.alpha_op;
			}
		}

		desc.depthAttachmentPixelFormat = key.depth_format;
		desc.stencilAttachmentPixelFormat = key.stencil_format;
		desc.rasterSampleCount = key.sample_count > 0 ? key.sample_count : 1;
		// Metal rejects pipeline creation when a vertex function writes
		// render_target_array_index unless the primitive topology class is stated.
		switch (static_cast<Prospero::PrimitiveType>(key.primitive_type)) {
			case Prospero::PrimitiveType::kPointList:
				desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
				break;
			case Prospero::PrimitiveType::kLineList:
			case Prospero::PrimitiveType::kLineStrip:
			case Prospero::PrimitiveType::kLineLoop:
				desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine;
				break;
			default:
				desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
				break;
		}

		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		if (!device) {
			return nil;
		}

		NSError* error = nil;
		id<MTLRenderPipelineState> pipeline = nil;
		if (mesh_pipeline) {
			MTLMeshRenderPipelineDescriptor* mesh = [MTLMeshRenderPipelineDescriptor new];
			mesh.label = desc.label;
			mesh.meshFunction = vertex_function;
			if (fragment_function == nil) {
				if (key.fs_hash != 0) RejectMetalState("missing active fragment function in mesh pipeline");
				for (uint32_t i = 0; i < key.color_target_count; ++i)
					if (key.color_targets[i].pixel_format != MTLPixelFormatInvalid &&
					    key.color_targets[i].write_mask != MTLColorWriteMaskNone)
						RejectMetalState("PS-disabled mesh pipeline requests color writes");
				// Mesh pipelines on Apple require a fragment entry even for DB-only
				// rasterization. No guest PS is being translated here: preserve the
				// interpolated raster depth and normal stencil operations, exporting
				// no color/depth/mask and touching no guest resources.
				if (!m_fixed_depth_stencil_fragment) {
					auto library = [device newLibraryWithSource:
					    @"#include <metal_stdlib>\nusing namespace metal;\nfragment void kyty_db_fixed_function() {}\n"
					    options:nil error:&error];
					m_fixed_depth_stencil_fragment = [library newFunctionWithName:@"kyty_db_fixed_function"];
					if (!m_fixed_depth_stencil_fragment)
						RejectMetalState(std::string("fixed depth/stencil rasterization compile failed: ") +
						    (error ? error.localizedDescription.UTF8String : "missing entry"));
				}
				mesh.fragmentFunction = m_fixed_depth_stencil_fragment;
			} else mesh.fragmentFunction = fragment_function;
			mesh.depthAttachmentPixelFormat = desc.depthAttachmentPixelFormat;
			mesh.stencilAttachmentPixelFormat = desc.stencilAttachmentPixelFormat;
			mesh.rasterSampleCount = desc.rasterSampleCount;
			for (uint32_t i = 0; i < key.color_target_count; ++i) {
				auto a = desc.colorAttachments[i]; auto b = mesh.colorAttachments[i];
				b.pixelFormat = a.pixelFormat; b.writeMask = a.writeMask; b.blendingEnabled = a.blendingEnabled;
				b.sourceRGBBlendFactor = a.sourceRGBBlendFactor; b.destinationRGBBlendFactor = a.destinationRGBBlendFactor;
				b.rgbBlendOperation = a.rgbBlendOperation; b.sourceAlphaBlendFactor = a.sourceAlphaBlendFactor;
				b.destinationAlphaBlendFactor = a.destinationAlphaBlendFactor; b.alphaBlendOperation = a.alphaBlendOperation;
			}
			pipeline = [device newRenderPipelineStateWithMeshDescriptor:mesh options:MTLPipelineOptionNone reflection:nil error:&error];
		} else {
			pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
		}
		if (!pipeline) {
			static std::atomic<uint32_t> s_pipeline_failures {0};
			const uint32_t count = s_pipeline_failures.fetch_add(1);
			if (count < 20) {
				std::fprintf(stderr,
				             "[MetalPipeline] create failed (%u) vs=0x%016llx ps=0x%016llx fmt=%u: %s\n",
				             count, static_cast<unsigned long long>(key.vs_hash),
				             static_cast<unsigned long long>(key.fs_hash),
				             static_cast<uint32_t>(key.color_targets[0].pixel_format),
				             error ? [[error localizedDescription] UTF8String] : "Unknown error");
			}
			LOGF_COLOR(Log::Color::BrightRed, "Metal: Failed to create render pipeline state: %s\n",
			           error ? [[error localizedDescription] UTF8String] : "Unknown error");
			return nil;
		}

		m_render_pipeline_cache.emplace(key, pipeline);
		return pipeline;
	}
}

id<MTLComputePipelineState> MetalPipelineManager::GetOrCreateComputePipeline(
    uint64_t cs_hash,
    id<MTLFunction> compute_function) {
	auto it = m_compute_pipeline_cache.find(cs_hash);
	if (it != m_compute_pipeline_cache.end()) {
		return it->second;
	}

	@autoreleasepool {
		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		if (!device || !compute_function) {
			return nil;
		}

		NSError* error = nil;
		MTLComputePipelineDescriptor* desc = [[MTLComputePipelineDescriptor alloc] init];
		desc.computeFunction = compute_function;
		desc.label = [NSString stringWithFormat:@"CS %016llx", (unsigned long long)cs_hash];
		id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithDescriptor:desc options:MTLPipelineOptionNone reflection:nil error:&error];
		if (!pipeline) {
			LOGF_COLOR(Log::Color::BrightRed, "Metal: Failed to create compute pipeline state: %s\n",
			           error ? [[error localizedDescription] UTF8String] : "Unknown error");
			return nil;
		}

		m_compute_pipeline_cache.emplace(cs_hash, pipeline);
		return pipeline;
	}
}

id<MTLRenderPipelineState> MetalPipelineManager::GetOrCreateRenderPipeline(
    const MetalRenderPipelineKey& key,
    void* vertex_function,
    void* fragment_function) {
	return GetOrCreateRenderPipeline(
	    key,
	    (__bridge id<MTLFunction>)vertex_function,
	    (__bridge id<MTLFunction>)fragment_function);
}

id<MTLComputePipelineState> MetalPipelineManager::GetOrCreateComputePipeline(
    uint64_t cs_hash,
    void* compute_function) {
	return GetOrCreateComputePipeline(
	    cs_hash,
	    (__bridge id<MTLFunction>)compute_function);
}

} // namespace Libs::Graphics::Metal

