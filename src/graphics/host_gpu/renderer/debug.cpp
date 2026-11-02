#include "graphics/host_gpu/renderer/debug.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/render.h"

#include <vector>
#include <string>
#include <set>
#include <mutex>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <fmt/format.h>

namespace Libs::Graphics {

uint32_t render_target_mask_slot(uint32_t mask, uint32_t slot) {
	return (mask >> (slot * 4u)) & 0x0fu;
}

static bool RenderTargetMaskHasMrt(uint32_t mask) {
	return (mask & ~0x0fu) != 0;
}

static bool RenderTargetMaskHasBoundMrt(const CommandBuffer& buffer) {
	const auto& hw   = buffer.GetRegisters();
	const auto  mask = hw.GetRenderTargetMask();

	if (!RenderTargetMaskHasMrt(mask)) {
		return false;
	}

	uint32_t bound_targets = 0;
	for (uint32_t i = 0; i < 8; i++) {
		if (render_target_mask_slot(mask, i) != 0 && hw.GetRenderTarget(i).base.addr != 0) {
			bound_targets++;
		}
	}

	return bound_targets > 1;
}

uint32_t render_target_first_bound_slot(const CommandBuffer& buffer) {
	const auto& hw   = buffer.GetRegisters();
	const auto  mask = hw.GetRenderTargetMask();
	for (uint32_t i = 0; i < 8; i++) {
		if (render_target_mask_slot(mask, i) != 0 && hw.GetRenderTarget(i).base.addr != 0) {
			return i;
		}
	}

	return 0;
}

bool graphics_debug_dump_enabled() {
	return Config::GraphicsDebugDumpEnabled() &&
	       Config::GetPrintfDirection() != Config::LogDirection::Silent;
}

void uc_print(const char* func, const HW::UserConfig& uc) {
	LOGF("%s\n", func);

	const auto& ge_cntl = uc.GetGeControl();
	const auto& user_en = uc.GetGeUserVgprEn();

	LOGF("\t GetPrimType()         = 0x%08" PRIx32 "\n"
	     "\t GetIndexOffset()      = 0x%08" PRIx32 "\n"
	     "\t GetObjectId()         = 0x%08" PRIx32 "\n"
	     "\t primitive_reset       = 0x%08" PRIx32 "\n"
	     "\t primitive_group_size  = 0x%04" PRIx16 "\n"
	     "\t vertex_group_size     = 0x%04" PRIx16 "\n"
	     "\t en_user_vgpr1         = %s\n"
	     "\t en_user_vgpr2         = %s\n"
	     "\t en_user_vgpr3         = %s\n",
	     static_cast<uint32_t>(uc.GetPrimType()), uc.GetIndexOffset(), uc.GetObjectId(),
	     uc.GetPrimitiveResetControl(), ge_cntl.primitive_group_size, ge_cntl.vertex_group_size,
	     user_en.vgpr1 ? "true" : "false", user_en.vgpr2 ? "true" : "false",
	     user_en.vgpr3 ? "true" : "false");
}

void uc_check(const HW::UserConfig& uc) {
	const auto& user_en = uc.GetGeUserVgprEn();

	EXIT_NOT_IMPLEMENTED(user_en.vgpr1 != false);
	EXIT_NOT_IMPLEMENTED(user_en.vgpr2 != false);
	EXIT_NOT_IMPLEMENTED(user_en.vgpr3 != false);
}

std::string rt_print(const char* func, const HW::RenderTarget& rt) {
	std::string dst;
	dst.reserve(4096);

	dst += fmt::format("{}\n", func);

	dst += fmt::format("\t base.addr                       = 0x{:016x}\n", rt.base.addr);
	dst += fmt::format("\t view.base_array_slice_index     = 0x{:08x}\n",
	                  rt.view.base_array_slice_index);
	dst += fmt::format("\t view.last_array_slice_index     = 0x{:08x}\n",
	                  rt.view.last_array_slice_index);
	dst += fmt::format("\t view.current_mip_level          = 0x{:08x}\n", rt.view.current_mip_level);
	dst += fmt::format("\t info.fmask_compression_enable   = {}\n",
	                  rt.info.fmask_compression_enable ? "true" : "false");

	dst += fmt::format("\t info.fmask_data_compression_disable = {}\n",
	                  rt.info.fmask_data_compression_disable ? "true" : "false");
	dst += fmt::format("\t info.fmask_one_frag_mode        = {}\n",
	                  rt.info.fmask_one_frag_mode ? "true" : "false");

	dst += fmt::format("\t info.cmask_fast_clear_enable    = {}\n",
	                  rt.info.cmask_fast_clear_enable ? "true" : "false");
	dst += fmt::format("\t info.dcc_compression_enable     = {}\n",
	                  rt.info.dcc_compression_enable ? "true" : "false");
	dst += fmt::format("\t info.format                     = 0x{:08x}\n",
	                  static_cast<uint32_t>(rt.info.format));
	dst += fmt::format("\t info.channel_type               = 0x{:08x}\n",
	                  static_cast<uint32_t>(rt.info.channel_type));
	dst += fmt::format("\t info.channel_order              = 0x{:08x}\n",
	                  static_cast<uint32_t>(rt.info.channel_order));
	dst += fmt::format("\t info.blend_bypa                 = {}\n",
	                  rt.info.blend_bypass ? "true" : "false");
	dst += fmt::format("\t info.blend_clamp                = {}\n",
	                  rt.info.blend_clamp ? "true" : "false");
	dst += fmt::format("\t info.round_mode                 = {}\n",
	                  rt.info.round_mode ? "true" : "false");
	dst += fmt::format("\t attrib.force_dest_alpha_to_one  = {}\n",
	                  rt.attrib.force_dest_alpha_to_one ? "true" : "false");
	dst += fmt::format("\t attrib.num_samples              = 0x{:08x}\n", rt.attrib.num_samples);
	dst += fmt::format("\t attrib.num_fragments            = 0x{:08x}\n", rt.attrib.num_fragments);
	dst += fmt::format("\t attrib2.width                   = 0x{:08x}\n", rt.attrib2.width);
	dst += fmt::format("\t attrib2.height                  = 0x{:08x}\n", rt.attrib2.height);
	dst += fmt::format("\t attrib2.num_mip_levels          = 0x{:08x}\n", rt.attrib2.num_mip_levels);
	dst += fmt::format("\t attrib3.depth                   = 0x{:08x}\n", rt.attrib3.depth);
	dst += fmt::format("\t attrib3.tile_mode               = 0x{:08x}\n",
	                  static_cast<uint32_t>(rt.attrib3.tile_mode));
	dst += fmt::format("\t attrib3.dimension               = 0x{:08x}\n", rt.attrib3.dimension);
	dst += fmt::format("\t attrib3.metadata_pipe_aligned   = {}\n",
	                  rt.attrib3.metadata_pipe_aligned ? "true" : "false");
	dst += fmt::format("\t attrib3.write_vrs_rate_hint_to_cmask = {}\n",
	                  rt.attrib3.write_vrs_rate_hint_to_cmask ? "true" : "false");
	dst += fmt::format("\t dcc.max_uncompressed_block_size = 0x{:08x}\n",
	                  rt.dcc.max_uncompressed_block_size);
	dst += fmt::format("\t dcc.max_compressed_block_size   = 0x{:08x}\n",
	                  rt.dcc.max_compressed_block_size);
	dst += fmt::format("\t dcc.color_transform             = 0x{:08x}\n", rt.dcc.color_transform);
	dst += fmt::format("\t dcc.overwrite_combiner_disable  = {}\n",
	                  rt.dcc.overwrite_combiner_disable ? "true" : "false");
	dst += fmt::format("\t dcc.independent_block_size      = 0x{:02x}\n",
	                  static_cast<uint8_t>(rt.dcc.independent_block_size));
	dst += fmt::format("\t data_write_on_dcc_clear_to_reg  = {}\n",
	                  rt.dcc.data_write_on_dcc_clear_to_reg ? "true" : "false");
	dst += fmt::format("\t dcc.dcc_clear_key_enable        = {}\n",
	                  rt.dcc.dcc_clear_key_enable ? "true" : "false");
	dst += fmt::format("\t cmask.addr                      = 0x{:016x}\n", rt.cmask.addr);
	dst += fmt::format("\t fmask.addr                      = 0x{:016x}\n", rt.fmask.addr);
	dst += fmt::format("\t clear_word0.word0               = 0x{:08x}\n", rt.clear_word0.word0);
	dst += fmt::format("\t clear_word1.word1               = 0x{:08x}\n", rt.clear_word1.word1);
	dst += fmt::format("\t dcc_addr.addr                   = 0x{:016x}\n", rt.dcc_addr.addr);

	return dst;
}

static bool RenderIsColorTileMode(Prospero::TileMode tile_mode) {
	// AGC CxRenderTarget::TileMode shifted by CB_COLOR*_ATTRIB3.COLOR_SW_MODE.
	switch (tile_mode) {
		case Prospero::TileMode::kLinear:
		case Prospero::TileMode::kStandard256B:
		case Prospero::TileMode::kStandard4KB:
		case Prospero::TileMode::kStandard64KB:
		case Prospero::TileMode::kPrt:
		case Prospero::TileMode::kDepth:
		case Prospero::TileMode::kRenderTarget: return true;
		default: return false;
	}
}

bool RenderIsColorTileModeLinear(Prospero::TileMode tile_mode) {
	return tile_mode == Prospero::TileMode::kLinear;
}

static bool RenderIsColorDimension(uint32_t dimension) {
	return dimension <= 0x02;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void RtCheck(const HW::RenderTarget& rt) {
	if (rt.base.addr != 0) {
		//  EXIT_NOT_IMPLEMENTED(rt.base_addr == 0);

		EXIT_NOT_IMPLEMENTED(rt.view.base_array_slice_index > rt.view.last_array_slice_index);
		if (rt.view.base_array_slice_index != 0x00000000 ||
		    rt.view.last_array_slice_index != 0x00000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: using color target array slice range %" PRIu32 "..%" PRIu32
				     "\n",
				     rt.view.base_array_slice_index, rt.view.last_array_slice_index);
				logged = true;
			}
		}
		if (rt.view.current_mip_level != 0x00000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: using PS5 color target mip level %" PRIu32 "\n",
				     rt.view.current_mip_level);
				logged = true;
			}
		}
		if (rt.info.fmask_compression_enable) {
			EXIT_NOT_IMPLEMENTED(rt.attrib.num_samples == 0 && rt.attrib.num_fragments == 0);
			// Native MSAA stores expanded samples, independent of FMASK metadata compression.
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: using expanded native Vulkan MSAA samples, "
				     "fmask=0x%016" PRIx64 "\n",
				     rt.fmask.addr);
				logged = true;
			}
		}

		if (rt.info.blend_bypass) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: using PS5 blend bypass as disabled Vulkan "
				     "blending\n");
				logged = true;
			}
		}
		// EXIT_NOT_IMPLEMENTED(rt.info.blend_clamp != false);
		if (rt.info.round_mode) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: ignoring PS5 color round mode\n");
				logged = true;
			}
		}
		//		 EXIT_NOT_IMPLEMENTED(rt.format != 0x0000000a);
		// EXIT_NOT_IMPLEMENTED(rt.channel_type != 0x00000006);
		// EXIT_NOT_IMPLEMENTED(rt.channel_order != 0x00000001);
		if (rt.attrib.force_dest_alpha_to_one) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: accepting PS5 force destination alpha-to-one\n");
				logged = true;
			}
		}
		if (rt.attrib.num_samples != 0x00000000 || rt.attrib.num_fragments != 0x00000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: using native PS5 MSAA color target, "
				     "samples=0x%08" PRIx32 " fragments=0x%08" PRIx32 "\n",
				     rt.attrib.num_samples, rt.attrib.num_fragments);
				logged = true;
			}
		}

		if (rt.attrib2.width == 0x00000000 || rt.attrib2.height == 0x00000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: accepting PS5 raw 1-pixel color target extent "
				     "fields width_minus1=0x%08" PRIx32 " height_minus1=0x%08" PRIx32 "\n",
				     rt.attrib2.width, rt.attrib2.height);
				logged = true;
			}
		}
		if (rt.attrib2.num_mip_levels != 0x00000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: using PS5 color target mip count field 0x%08" PRIx32 "\n",
				     rt.attrib2.num_mip_levels);
				logged = true;
			}
		}
		if (!RenderIsColorTileMode(rt.attrib3.tile_mode)) {
			EXIT("unknown PS5 render-target tile mode: 0x%08" PRIx32 "\n",
			     static_cast<uint32_t>(rt.attrib3.tile_mode));
		}
		if (!RenderIsColorDimension(rt.attrib3.dimension)) {
			EXIT("unknown PS5 render-target dimension: 0x%08" PRIx32 "\n", rt.attrib3.dimension);
		}
		if (!rt.attrib3.metadata_pipe_aligned) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: accepting unaligned PS5 metadata pipe flag\n");
				logged = true;
			}
		}
		EXIT_NOT_IMPLEMENTED(rt.attrib3.write_vrs_rate_hint_to_cmask);

		// EXIT_NOT_IMPLEMENTED(rt.dcc_max_uncompressed_block_size != 0x00000002);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.max_compressed_block_size != 0x00000000);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.color_transform != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.dcc.overwrite_combiner_disable != false);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.data_write_on_dcc_clear_to_reg != false);
		EXIT_NOT_IMPLEMENTED(rt.dcc.dcc_clear_key_enable != false);
		if (rt.cmask.addr != 0x0000000000000000 || rt.fmask.addr != 0x0000000000000000 ||
		    rt.dcc_addr.addr != 0x0000000000000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: ignoring PS5 metadata addresses cmask=0x%016" PRIx64
				     " fmask=0x%016" PRIx64 " dcc=0x%016" PRIx64 "\n",
				     rt.cmask.addr, rt.fmask.addr, rt.dcc_addr.addr);
				logged = true;
			}
		}
	}
}

static void ZPrint(const char* func, const HW::DepthRenderTarget& z) {
	LOGF("%s\n", func);

	LOGF("\t z_info.format                         = 0x%08" PRIx32 "\n"
	     "\t z_info.num_samples                    = 0x%08" PRIx32 "\n"
	     "\t z_info.texture_compatibility          = 0x%08" PRIx32 "\n"
	     "\t z_info.htile_acceleration             = %s\n"
	     "\t z_info.expclear_enabled               = %s\n"
	     "\t z_info.z_compare_base                 = 0x%08" PRIx32 "\n"
	     "\t z_info.partially_resident             = %s\n"
	     "\t z_info.max_mip_level                  = 0x%02" PRIx8 "\n"
	     "\t stencil_info.format                   = 0x%08" PRIx32 "\n"
	     "\t stencil_info.texture_compatibility    = 0x%08" PRIx32 "\n"
	     "\t stencil_info.htile_stencil_disabled   = %s\n"
	     "\t stencil_info.expclear_enabled         = %s\n"
	     "\t stencil_info.partially_resident       = %s\n"
	     "\t depth_view.slice_start                = 0x%08" PRIx32 "\n"
	     "\t depth_view.slice_max                  = 0x%08" PRIx32 "\n"
	     "\t depth_view.current_mip_level          = 0x%02" PRIx8 "\n"
	     "\t depth_view.depth_write_disable        = %s\n"
	     "\t depth_view.stencil_write_disable      = %s\n"
	     "\t z_read_base_addr                      = 0x%016" PRIx64 "\n"
	     "\t stencil_read_base_addr                = 0x%016" PRIx64 "\n"
	     "\t z_write_base_addr                     = 0x%016" PRIx64 "\n"
	     "\t stencil_write_base_addr               = 0x%016" PRIx64 "\n"
	     "\t htile_data_base_addr                  = 0x%016" PRIx64 "\n"
	     "\t shading_rate_encoding                 = 0x%02" PRIx8 "\n"
	     "\t size.x_max                            = 0x%04" PRIx16 "\n"
	     "\t size.y_max                            = 0x%04" PRIx16 "\n"
	     "\t size.valid                            = %s\n",
	     static_cast<uint32_t>(z.z_info.format), z.z_info.num_samples,
	     static_cast<uint32_t>(z.z_info.texture_compatibility),
	     z.z_info.htile_acceleration ? "true" : "false",
	     z.z_info.expclear_enabled ? "true" : "false",
	     static_cast<uint32_t>(z.z_info.z_compare_base),
	     z.z_info.partially_resident ? "true" : "false", z.z_info.max_mip_level,
	     static_cast<uint32_t>(z.stencil_info.format),
	     static_cast<uint32_t>(z.stencil_info.texture_compatibility),
	     z.stencil_info.htile_stencil_disabled ? "true" : "false",
	     z.stencil_info.expclear_enabled ? "true" : "false",
	     z.stencil_info.partially_resident ? "true" : "false", z.depth_view.slice_start,
	     z.depth_view.slice_max, z.depth_view.current_mip_level,
	     z.depth_view.depth_write_disable ? "true" : "false",
	     z.depth_view.stencil_write_disable ? "true" : "false", z.z_read_base_addr,
	     z.stencil_read_base_addr, z.z_write_base_addr, z.stencil_write_base_addr,
	     z.htile_data_base_addr, z.shading_rate_encoding, z.size.x_max, z.size.y_max,
	     z.size.valid ? "true" : "false");
}

static void ClipPrint(const char* func, const HW::ClipControl& c) {
	LOGF("%s\n", func);

	LOGF("\t user_clip_planes                    = 0x%02" PRIx8 "\n"
	     "\t user_clip_plane_mode                = 0x%02" PRIx8 "\n"
	     "\t dx_clip_space                       = %s\n"
	     "\t vertex_kill_any                     = %s\n"
	     "\t min_z_clip_disable                  = %s\n"
	     "\t max_z_clip_disable                  = %s\n"
	     "\t user_clip_plane_negate_y            = %s\n"
	     "\t clip_disable                        = %s\n"
	     "\t user_clip_plane_cull_only           = %s\n"
	     "\t cull_on_clipping_error_disable      = %s\n"
	     "\t linear_attribute_clip_enable        = %s\n"
	     "\t force_viewport_index_from_vs_enable = %s\n",
	     c.user_clip_planes, c.user_clip_plane_mode, c.dx_clip_space ? "true" : "false",
	     c.vertex_kill_any ? "true" : "false", c.min_z_clip_disable ? "true" : "false",
	     c.max_z_clip_disable ? "true" : "false", c.user_clip_plane_negate_y ? "true" : "false",
	     c.clip_disable ? "true" : "false", c.user_clip_plane_cull_only ? "true" : "false",
	     c.cull_on_clipping_error_disable ? "true" : "false",
	     c.linear_attribute_clip_enable ? "true" : "false",
	     c.force_viewport_index_from_vs_enable ? "true" : "false");
}

static void ClipCheck(const HW::ClipControl& c) {
	// dx_linear_attr_clip_enable preserves linear (noperspective) attributes at clip-generated
	// vertices, which Vulkan provides as part of clipping and interpolation.
	EXIT_NOT_IMPLEMENTED(c.user_clip_planes != 0 || c.user_clip_plane_mode != 0 ||
	                     c.vertex_kill_any || c.user_clip_plane_negate_y ||
	                     c.user_clip_plane_cull_only || c.cull_on_clipping_error_disable ||
	                     c.force_viewport_index_from_vs_enable);
}

static void RcPrint(const char* func, const HW::RenderControl& c) {
	LOGF("%s\n", func);

	LOGF("\t depth_clear_enable       = %s\n"
	     "\t stencil_clear_enable     = %s\n"
	     "\t resummarize_enable       = %s\n"
	     "\t stencil_compress_disable = %s\n"
	     "\t depth_compress_disable   = %s\n"
	     "\t copy_centroid            = %s\n"
	     "\t copy_sample              = %" PRIu8 "\n",
	     c.depth_clear_enable ? "true" : "false", c.stencil_clear_enable ? "true" : "false",
	     c.resummarize_enable ? "true" : "false", c.stencil_compress_disable ? "true" : "false",
	     c.depth_compress_disable ? "true" : "false", c.copy_centroid ? "true" : "false",
	     c.copy_sample);
}

static void RcCheck(const HW::RenderControl& c) {
	// EXIT_NOT_IMPLEMENTED(c.depth_clear_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.stencil_clear_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.stencil_compress_disable != false);
	// EXIT_NOT_IMPLEMENTED(c.depth_compress_disable != false);
	EXIT_NOT_IMPLEMENTED(c.copy_centroid != false);
	EXIT_NOT_IMPLEMENTED(c.copy_sample != 0);
}

static void McPrint(const char* func, const HW::ModeControl& c) {
	LOGF("%s\n", func);

	LOGF("\t cull_front               = %s\n"
	     "\t cull_back                = %s\n"
	     "\t face                     = %s\n"
	     "\t poly_mode                = %" PRIu8 "\n"
	     "\t polymode_front_ptype     = %" PRIu8 "\n"
	     "\t polymode_back_ptype      = %" PRIu8 "\n"
	     "\t poly_offset_front_enable = %s\n"
	     "\t poly_offset_back_enable  = %s\n"
	     "\t vtx_window_offset_enable = %s\n"
	     "\t provoking_vtx_last       = %s\n"
	     "\t persp_corr_dis           = %s\n",
	     c.cull_front ? "true" : "false", c.cull_back ? "true" : "false", c.face ? "true" : "false",
	     c.poly_mode, c.polymode_front_ptype, c.polymode_back_ptype,
	     c.poly_offset_front_enable ? "true" : "false",
	     c.poly_offset_back_enable ? "true" : "false",
	     c.vtx_window_offset_enable ? "true" : "false", c.provoking_vtx_last ? "true" : "false",
	     c.persp_corr_dis ? "true" : "false");
}

static void McCheck(const HW::ModeControl& c) {
	// EXIT_NOT_IMPLEMENTED(c.cull_front != false);
	// EXIT_NOT_IMPLEMENTED(c.cull_back != false);
	// EXIT_NOT_IMPLEMENTED(c.face != false);
	if (c.vtx_window_offset_enable) {
		static bool logged = false;
		if (!logged) {
			LOGF("\t temporary: PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE is not fully "
			     "implemented; continuing without vertex window "
			     "offset adjustment\n");
			logged = true;
		}
	}
	EXIT_NOT_IMPLEMENTED(c.persp_corr_dis != false);
}

static void BcPrint(const char* func, const HW::BlendControl& c, const HW::BlendColor& color,
                    const HW::ColorControl& cc) {
	LOGF("%s\n", func);

	LOGF("\t color_srcblend       = %" PRIu8 "\n"
	     "\t color_comb_fcn       = %" PRIu8 "\n"
	     "\t color_destblend      = %" PRIu8 "\n"
	     "\t alpha_srcblend       = %" PRIu8 "\n"
	     "\t alpha_comb_fcn       = %" PRIu8 "\n"
	     "\t alpha_destblend      = %" PRIu8 "\n"
	     "\t separate_alpha_blend = %s\n"
	     "\t enable               = %s\n"
	     "\t red                  = %f\n"
	     "\t green                = %f\n"
	     "\t blue                 = %f\n"
	     "\t alpha                = %f\n"
	     "\t cc.mode              = %" PRIu8 "\n"
	     "\t cc.op                = %" PRIu8 "\n",
	     c.color_srcblend, c.color_comb_fcn, c.color_destblend, c.alpha_srcblend, c.alpha_comb_fcn,
	     c.alpha_destblend, c.separate_alpha_blend ? "true" : "false", c.enable ? "true" : "false",
	     color.red, color.green, color.blue, color.alpha, cc.mode, cc.op);
}

static void BcCheck(const HW::BlendColor& color, const HW::ColorControl& cc) {
	if (color.red != 0.0f || color.green != 0.0f || color.blue != 0.0f || color.alpha != 0.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("BlendControl: temporary: accepting nonzero blend constants (%f, %f, %f, %f)\n",
			     color.red, color.green, color.blue, color.alpha);
			logged = true;
		}
	}
	if (cc.mode != 1 && cc.mode != 0 && cc.mode != 2 && cc.mode != 3 && cc.mode != 5 &&
	    cc.mode != 6) {
		static bool logged = false;
		if (!logged) {
			LOGF("BlendControl: temporary: accepting unsupported color-control mode %" PRIu8 "\n",
			     cc.mode);
			logged = true;
		}
	}
	if (cc.op != 0xCC) {
		static bool logged = false;
		if (!logged) {
			LOGF("BlendControl: temporary: accepting unsupported raster op 0x%02" PRIx8 "\n",
			     cc.op);
			logged = true;
		}
	}
}

static void DPrint(const char* func, const HW::DepthControl& c, const HW::StencilControl& s,
                   const HW::StencilMask& sm) {
	LOGF("%s\n", func);

	LOGF("\t stencil_enable       = %s\n"
	     "\t z_enable             = %s\n"
	     "\t z_write_enable       = %s\n"
	     "\t depth_bounds_enable  = %s\n"
	     "\t zfunc                = %" PRIu8 "\n"
	     "\t backface_enable      = %s\n"
	     "\t stencilfunc          = %" PRIu8 "\n"
	     "\t stencilfunc_bf       = %" PRIu8 "\n"
	     "\t stencil_fail         = %" PRIu8 "\n"
	     "\t stencil_zpass        = %" PRIu8 "\n"
	     "\t stencil_zfail        = %" PRIu8 "\n"
	     "\t stencil_fail_bf      = %" PRIu8 "\n"
	     "\t stencil_zpass_bf     = %" PRIu8 "\n"
	     "\t stencil_zfail_bf     = %" PRIu8 "\n"
	     "\t stencil_testval      = %" PRIu8 "\n"
	     "\t stencil_mask         = %" PRIu8 "\n"
	     "\t stencil_writemask    = %" PRIu8 "\n"
	     "\t stencil_opval        = %" PRIu8 "\n"
	     "\t stencil_testval_bf   = %" PRIu8 "\n"
	     "\t stencil_mask_bf      = %" PRIu8 "\n"
	     "\t stencil_writemask_bf = %" PRIu8 "\n"
	     "\t stencil_opval_bf     = %" PRIu8 "\n",
	     c.stencil_enable ? "true" : "false", c.z_enable ? "true" : "false",
	     c.z_write_enable ? "true" : "false", c.depth_bounds_enable ? "true" : "false", c.zfunc,
	     c.backface_enable ? "true" : "false", c.stencilfunc, c.stencilfunc_bf, s.stencil_fail,
	     s.stencil_zpass, s.stencil_zfail, s.stencil_fail_bf, s.stencil_zpass_bf,
	     s.stencil_zfail_bf, sm.stencil_testval, sm.stencil_mask, sm.stencil_writemask,
	     sm.stencil_opval, sm.stencil_testval_bf, sm.stencil_mask_bf, sm.stencil_writemask_bf,
	     sm.stencil_opval_bf);
}

static void EqaaPrint(const char* func, const HW::EqaaControl& c) {
	LOGF("%s\n", func);

	LOGF("\t max_anchor_samples         = %" PRIu8 "\n"
	     "\t ps_iter_samples            = %" PRIu8 "\n"
	     "\t mask_export_num_samples    = %" PRIu8 "\n"
	     "\t alpha_to_mask_num_samples  = %" PRIu8 "\n"
	     "\t high_quality_intersections = %s\n"
	     "\t incoherent_eqaa_reads      = %s\n"
	     "\t interpolate_comp_z         = %s\n"
	     "\t static_anchor_associations = %s\n",
	     c.max_anchor_samples, c.ps_iter_samples, c.mask_export_num_samples,
	     c.alpha_to_mask_num_samples, c.high_quality_intersections ? "true" : "false",
	     c.incoherent_eqaa_reads ? "true" : "false", c.interpolate_comp_z ? "true" : "false",
	     c.static_anchor_associations ? "true" : "false");
}

static void EqaaCheck(const HW::EqaaControl& c, const HW::AaConfig& cf) {
	// The EQAA controls only take effect while multisampling is on, so a single-sample
	// target leaves them inert and has nothing to warn about.
	if (cf.msaa_num_samples == 0) {
		return;
	}
	if (c.max_anchor_samples != 0 || c.ps_iter_samples != 0 || c.mask_export_num_samples != 0 ||
	    c.alpha_to_mask_num_samples != 0 || c.high_quality_intersections ||
	    c.incoherent_eqaa_reads || c.interpolate_comp_z || c.static_anchor_associations) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("\t warning: unsupported PS5 EQAA controls use native Vulkan MSAA defaults\n");
		}
	}
}

static void AaPrint(const char* func, const HW::AaSampleControl& c, const HW::AaConfig& cf) {
	LOGF("%s\n", func);

	LOGF("\t centroid_priority = %016" PRIx64 "\n", c.centroid_priority);
	for (int i = 0; i < 16; i++) {
		LOGF("\t locations[%d] = %08" PRIx32 "\n", i, c.locations[i]);
	}
	LOGF("\t msaa_num_samples      = %" PRIu8 "\n"
	     "\t aa_mask_centroid_dtmn = %s\n"
	     "\t max_sample_dist       = %" PRIu8 "\n"
	     "\t msaa_exposed_samples  = %" PRIu8 "\n",
	     cf.msaa_num_samples, cf.aa_mask_centroid_dtmn ? "true" : "false", cf.max_sample_dist,
	     cf.msaa_exposed_samples);
}

static void AaCheck(const HW::AaSampleControl& c, const HW::AaConfig& cf) {
	bool non_default_locations = (c.centroid_priority != 0);
	for (uint32_t l: c.locations) {
		non_default_locations |= (l != 0);
	}

	if (non_default_locations || cf.msaa_num_samples != 0 || cf.aa_mask_centroid_dtmn ||
	    cf.max_sample_dist != 0 || cf.msaa_exposed_samples != 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("\t warning: unsupported PS5 sample locations use native Vulkan locations: "
			     "samples=%" PRIu8 ", exposed=%" PRIu8 ", max_dist=%" PRIu8 "\n",
			     cf.msaa_num_samples, cf.msaa_exposed_samples, cf.max_sample_dist);
		}
	}
}

void LogDrawPhase(const char* draw_name, const char* phase) {
	if (graphics_debug_dump_enabled()) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 1024) {
			LOGF("DrawPhase: %s %s\n", draw_name, phase);
		}
	}
}

void LogPipelineTrace(const char* phase, uint64_t vertex_program_id, uint64_t pixel_program_id) {
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: %s VS=%" PRIu64 " PS=%" PRIu64 "\n", phase, vertex_program_id,
		     pixel_program_id);
	}
}

static void VpPrint(const char* func, const HW::ScreenViewport& vp,
                    const HW::ScanModeControl& smc) {
	LOGF("%s\n", func);

	LOGF("\t msaa_enable                    = %s\n"
	     "\t vport_scissor_enable           = %s\n"
	     "\t line_stipple_enable            = %s\n"
	     "\t vp[0].zmin                     = %f\n"
	     "\t vp[0].zmax                     = %f\n"
	     "\t vp[0].xscale                   = %f\n"
	     "\t vp[0].xoffset                  = %f\n"
	     "\t vp[0].yscale                   = %f\n"
	     "\t vp[0].yoffset                  = %f\n"
	     "\t vp[0].zscale                   = %f\n"
	     "\t vp[0].zoffset                  = %f\n"
	     "\t vp[0].viewport_scissor_left    = %d\n"
	     "\t vp[0].viewport_scissor_top     = %d\n"
	     "\t vp[0].viewport_scissor_right   = %d\n"
	     "\t vp[0].viewport_scissor_bottom  = %d\n"
	     "\t transform_control              = 0x%08" PRIx32 "\n"
	     "\t screen_scissor_left            = %d\n"
	     "\t screen_scissor_top             = %d\n"
	     "\t screen_scissor_right           = %d\n"
	     "\t screen_scissor_bottom          = %d\n"
	     "\t window_scissor_left            = %d\n"
	     "\t window_scissor_top             = %d\n"
	     "\t window_scissor_right           = %d\n"
	     "\t window_scissor_bottom          = %d\n"
	     "\t generic_scissor_left           = %d\n"
	     "\t generic_scissor_top            = %d\n"
	     "\t generic_scissor_right          = %d\n"
	     "\t generic_scissor_bottom         = %d\n"
	     "\t window_offset_x                = %d\n"
	     "\t window_offset_y                = %d\n"
	     "\t hw_offset_x                    = %u\n"
	     "\t hw_offset_y                    = %u\n"
	     "\t guard_band_horz_clip           = %f\n"
	     "\t guard_band_vert_clip           = %f\n"
	     "\t guard_band_horz_discard        = %f\n"
	     "\t guard_band_vert_discard        = %f\n"
	     "\t clip_rect_rule                 = 0x%04" PRIx16 "\n"
	     "\t clip_rect_0                    = (%d,%d)-(%d,%d), window_offset = %s\n"
	     "\t window_scissor_window_offset_enable                = %s\n"
	     "\t generic_scissor_window_offset_enable               = %s\n",
	     smc.msaa_enable ? "true" : "false", smc.vport_scissor_enable ? "true" : "false",
	     smc.line_stipple_enable ? "true" : "false", vp.viewports[0].zmin, vp.viewports[0].zmax,
	     vp.viewports[0].xscale, vp.viewports[0].xoffset, vp.viewports[0].yscale,
	     vp.viewports[0].yoffset, vp.viewports[0].zscale, vp.viewports[0].zoffset,
	     vp.viewports[0].viewport_scissor_left, vp.viewports[0].viewport_scissor_top,
	     vp.viewports[0].viewport_scissor_right, vp.viewports[0].viewport_scissor_bottom,
	     vp.transform_control, vp.screen_scissor_left, vp.screen_scissor_top,
	     vp.screen_scissor_right, vp.screen_scissor_bottom, vp.window_scissor_left,
	     vp.window_scissor_top, vp.window_scissor_right, vp.window_scissor_bottom,
	     vp.generic_scissor_left, vp.generic_scissor_top, vp.generic_scissor_right,
	     vp.generic_scissor_bottom, vp.window_offset_x, vp.window_offset_y, vp.hw_offset_x,
	     vp.hw_offset_y, vp.guard_band_horz_clip, vp.guard_band_vert_clip,
	     vp.guard_band_horz_discard, vp.guard_band_vert_discard, vp.clip_rect_rule,
	     vp.clip_rect_left[0], vp.clip_rect_top[0], vp.clip_rect_right[0], vp.clip_rect_bottom[0],
	     vp.clip_rect_window_offset_enable[0] ? "true" : "false",
	     vp.window_scissor_window_offset_enable ? "true" : "false",
	     vp.generic_scissor_window_offset_enable ? "true" : "false");
	LOGF("\t viewports[0].viewport_scissor_window_offset_enable = %s\n",
	     vp.viewports[0].viewport_scissor_window_offset_enable ? "true" : "false");
}

static void VpCheck(const HW::ScreenViewport& vp, const HW::ScanModeControl& smc) {

	if (smc.msaa_enable) {

		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("\t warning: unsupported PS5 MSAA raster controls use native Vulkan defaults\n");
		}
	}
	// EXIT_NOT_IMPLEMENTED(smc.vport_scissor_enable);
	EXIT_NOT_IMPLEMENTED(smc.line_stipple_enable);

	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].xscale != 960.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].xoffset != 960.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].yscale != -540.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].yoffset != 540.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].zscale != 0.500000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].zoffset != 0.500000);
	if (vp.transform_control != 1087) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("\t warning: non-default viewport transform control = 0x%08" PRIx32
			     ", applying enabled scale/offset bits\n",
			     vp.transform_control);
		}
	}
	// EXIT_NOT_IMPLEMENTED(vp.hw_offset_x != 60);
	// EXIT_NOT_IMPLEMENTED(vp.hw_offset_y != 32);
	// EXIT_NOT_IMPLEMENTED(fabsf(vp.guard_band_horz_clip - 33.133327f) > 0.001f);
	// EXIT_NOT_IMPLEMENTED(fabsf(vp.guard_band_vert_clip - 59.629623f) > 0.001f);

	if (vp.guard_band_horz_discard != 1.0f || vp.guard_band_vert_discard != 1.0f) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("\t warning: unsupported PS5 guard band discard = %f, %f, continuing\n",
			     vp.guard_band_horz_discard, vp.guard_band_vert_discard);
		}
	}

	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_left != 0);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_top != 0);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_right != 0);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_bottom != 0);
	// EXIT_NOT_IMPLEMENTED(viewport_scissor &&
	// vp.viewports[0].viewport_scissor_window_offset_enable != true);
}

static bool ScissorRectValid(const ScissorRect& r) {
	return r.right > r.left && r.bottom > r.top;
}

static ScissorRect ScissorRectOffset(ScissorRect r, int x, int y) {
	r.left += x;
	r.right += x;
	r.top += y;
	r.bottom += y;
	return r;
}

static ScissorRect ScissorRectIntersect(const ScissorRect& a, const ScissorRect& b) {
	return {std::max(a.left, b.left), std::max(a.top, b.top),
	        std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}

static ScissorRect ScissorRectClamp(ScissorRect r, uint32_t width, uint32_t height) {
	int max_right  = static_cast<int>(width);
	int max_bottom = static_cast<int>(height);

	r.left   = std::clamp(r.left, 0, max_right);
	r.right  = std::clamp(r.right, 0, max_right);
	r.top    = std::clamp(r.top, 0, max_bottom);
	r.bottom = std::clamp(r.bottom, 0, max_bottom);

	if (!ScissorRectValid(r)) {
		r.right  = r.left;
		r.bottom = r.top;
	}

	return r;
}

static constexpr std::array<uint16_t, 16> MakeScissorIntersectionRules() {
	std::array<uint16_t, 16> rules {};
	for (uint32_t candidate = 0; candidate < rules.size(); candidate++) {
		for (uint32_t combination = 0; combination < 16; combination++) {
			if ((combination & candidate) == candidate) {
				rules[candidate] |= static_cast<uint16_t>(1u << combination);
			}
		}
	}
	return rules;
}

static bool ScissorClipRuleToIntersectionMask(uint16_t rule, uint8_t* mask) {
	EXIT_IF(mask == nullptr);

	static constexpr auto rules = MakeScissorIntersectionRules();
	for (uint32_t candidate = 0; candidate < rules.size(); candidate++) {
		if (rules[candidate] == rule) {
			*mask = static_cast<uint8_t>(candidate);
			return true;
		}
	}

	return false;
}

ScissorRect calc_final_scissor(const HW::ScreenViewport& vp, const HW::ScanModeControl& smc,
                               vk::Extent2D extent, uint32_t viewport_index) {
	EXIT_IF(viewport_index >= std::size(vp.viewports));
	ScissorRect final {vp.screen_scissor_left, vp.screen_scissor_top, vp.screen_scissor_right,
	                   vp.screen_scissor_bottom};
	const auto intersect = [&](ScissorRect rect, bool window_offset) {
		if (window_offset) {
			rect = ScissorRectOffset(rect, vp.window_offset_x, vp.window_offset_y);
		}
		final = ScissorRectIntersect(final, rect);
	};
	intersect({vp.window_scissor_left, vp.window_scissor_top, vp.window_scissor_right,
	           vp.window_scissor_bottom}, vp.window_scissor_window_offset_enable);
	intersect({vp.generic_scissor_left, vp.generic_scissor_top, vp.generic_scissor_right,
	           vp.generic_scissor_bottom}, vp.generic_scissor_window_offset_enable);

	const auto& viewport = vp.viewports[viewport_index];
	if (smc.vport_scissor_enable) {
		intersect({viewport.viewport_scissor_left, viewport.viewport_scissor_top,
		           viewport.viewport_scissor_right, viewport.viewport_scissor_bottom},
		          viewport.viewport_scissor_window_offset_enable);
	}

	if (vp.clip_rect_rule == 0) {
		final = {0, 0, 0, 0};
	} else if (vp.clip_rect_rule != 0xffffu) {
		uint8_t clip_rect_mask = 0;
		if (ScissorClipRuleToIntersectionMask(vp.clip_rect_rule, &clip_rect_mask)) {
			for (uint32_t i = 0; i < 4; i++) {
				if ((clip_rect_mask & (1u << i)) == 0) {
					continue;
				}

				intersect({vp.clip_rect_left[i], vp.clip_rect_top[i], vp.clip_rect_right[i],
				           vp.clip_rect_bottom[i]}, vp.clip_rect_window_offset_enable[i]);
			}
		} else {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
				LOGF("unsupported clip-rect rule 0x%04" PRIx16 ", leaving scissor unchanged\n",
				     vp.clip_rect_rule);
			}
		}
	}

	return ScissorRectClamp(final, extent.width, extent.height);
}

void hw_check(const CommandBuffer& buffer) {
	const auto& hw      = buffer.GetRegisters();
	const auto  rt_slot = render_target_first_bound_slot(buffer);
	const auto& rt      = hw.GetRenderTarget(rt_slot);
	const auto& bclr    = hw.GetBlendColor();
	const auto& vp      = hw.GetScreenViewport();
	const auto& c       = hw.GetClipControl();
	const auto& rc      = hw.GetRenderControl();
	const auto& mc      = hw.GetModeControl();
	const auto& eqaa    = hw.GetEqaaControl();
	const auto& cc      = hw.GetColorControl();
	const auto& smc     = hw.GetScanModeControl();
	const auto& aa      = hw.GetAaSampleControl();
	const auto& ac      = hw.GetAaConfig();

	auto log_phase = [](const char* phase) {
		if (graphics_debug_dump_enabled()) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 512) {
				LOGF("HwCheckPhase: %s\n", phase);
			}
		}
	};

	log_phase("rt");
	RtCheck(rt);
	log_phase("vp");
	VpCheck(vp, smc);
	log_phase("clip");
	ClipCheck(c);
	log_phase("rc");
	RcCheck(rc);
	log_phase("depth");
	log_phase("mode");
	McCheck(mc);
	log_phase("blend");
	BcCheck(bclr, cc);
	log_phase("eqaa");
	EqaaCheck(eqaa, ac);
	log_phase("aa");
	AaCheck(aa, ac);
	log_phase("done");

	if (graphics_debug_dump_enabled() && RenderTargetMaskHasBoundMrt(buffer)) {
		LOGF("MRT render target mask: 0x%08" PRIx32 "\n", hw.GetRenderTargetMask());
		for (uint32_t i = 0; i < 8; i++) {
			const auto& mrt = hw.GetRenderTarget(i);
			LOGF("\t RT%u addr=0x%010" PRIx64 " mask=0x%x fmt=0x%08" PRIx32
			     " width=%u height=%u tile=%u\n",
			     i, mrt.base.addr, (hw.GetRenderTargetMask() >> (i * 4u)) & 0x0fu,
			     static_cast<uint32_t>(mrt.info.format), mrt.attrib2.width + 1,
			     mrt.attrib2.height + 1, static_cast<uint32_t>(mrt.attrib3.tile_mode));
		}
	}
	if (rc.depth_clear_enable && hw.GetDepthClearValue() != 0.0f &&
	    hw.GetDepthClearValue() != 1.0f) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("\t temporary: accepting non-default depth clear value %f\n",
			     hw.GetDepthClearValue());
		}
	}
	// EXIT_NOT_IMPLEMENTED(hw.GetStencilClearValue() != 0);
}

void hw_print(const CommandBuffer& buffer) {
	const auto& hw      = buffer.GetRegisters();
	const auto  rt_slot = render_target_first_bound_slot(buffer);
	const auto& rt      = hw.GetRenderTarget(rt_slot);
	const auto& bc      = hw.GetBlendControl(rt_slot);
	const auto& bclr    = hw.GetBlendColor();
	const auto& vp      = hw.GetScreenViewport();
	const auto& z       = hw.GetDepthRenderTarget();
	const auto& c       = hw.GetClipControl();
	const auto& rc      = hw.GetRenderControl();
	const auto& d       = hw.GetDepthControl();
	const auto& s       = hw.GetStencilControl();
	const auto& sm      = hw.GetStencilMask();
	const auto& mc      = hw.GetModeControl();
	const auto& eqaa    = hw.GetEqaaControl();
	const auto& cc      = hw.GetColorControl();
	const auto& smc     = hw.GetScanModeControl();
	const auto& aa      = hw.GetAaSampleControl();
	const auto& ac      = hw.GetAaConfig();

	if (graphics_debug_dump_enabled()) {
		LOGF("Context\n"
		     "\t GetRenderTargetMask()   = 0x%08" PRIx32 "\n"
		     "\t GetDepthClearValue()    = %f\n"
		     "\t GetStencilClearValue()  = %" PRIu8 "\n"
		     "\t GetLineWidth()          = %f\n"
		     "\t primitive_reset_index   = 0x%08" PRIx32 "\n",
		     hw.GetRenderTargetMask(), hw.GetDepthClearValue(), hw.GetStencilClearValue(),
		     hw.GetLineWidth(), hw.GetPrimitiveResetIndex());

		LOGF("%s", rt_print("RenderTraget:", rt).c_str());

		ZPrint("DepthRenderTraget:", z);
		VpPrint("ScreenViewport:", vp, smc);
		ClipPrint("ClipControl:", c);
		RcPrint("RenderControl:", rc);
		DPrint("DepthStencilControlMask:", d, s, sm);
		McPrint("ModeControl:", mc);
		BcPrint("BlendColorControl:", bc, bclr, cc);
		EqaaPrint("EqaaControl:", eqaa);
		AaPrint("AaSampleControl:", aa, ac);
	}
}

thread_local uint64_t g_debug_shader_hash = 0;
thread_local int      g_debug_rt_follow   = 0;
static thread_local bool g_debug_log_bindings = false;
static std::atomic<int>  g_frame_trace_left   = 0;

bool DebugSkipShader(uint64_t shader_hash) {
	static std::mutex                            mutex;
	static std::set<uint64_t>                    skip;
	static std::chrono::steady_clock::time_point next_check {};
	static std::string                           last;
	std::lock_guard lock(mutex);
	const auto now = std::chrono::steady_clock::now();
	if (now >= next_check) {
		next_check = now + std::chrono::milliseconds(500);
		std::string text;
		if (FILE* f = std::fopen("_TempData/skip_shaders", "r"); f != nullptr) {
			char chunk[512];
			size_t n = 0;
			while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
				text.append(chunk, n);
			}
			std::fclose(f);
		}
		if (const char* env = std::getenv("KYTY_SKIP_SHADERS"); env != nullptr) {
			if (!text.empty()) text += " ";
			text += env;
		}
		if (text != last) {
			last = text;
			skip.clear();
			for (const char* p = text.c_str(); *p != 0;) {
				char*      end = nullptr;
				const auto h   = std::strtoull(p, &end, 16);
				if (end == p) {
					++p;
					continue;
				}
				skip.insert(h);
				p = end;
			}
			std::fprintf(stderr, "[skip] %zu shaders skipped\n", skip.size());
		}
	}
	return !skip.empty() && skip.count(shader_hash) != 0;
}

bool DebugFrameTracing() {
	return g_frame_trace_left.load(std::memory_order_relaxed) > 0;
}

struct DebugWorkRecord {
	const char* kind;
	uint64_t    hash;
	uint32_t    a, b, c;
};
static std::mutex      g_work_mutex;
static DebugWorkRecord g_work_ring[8192];
static uint64_t        g_work_count = 0;

void DebugRecordWork(const char* kind, uint64_t hash, uint32_t a, uint32_t b, uint32_t c) {
	std::lock_guard lock(g_work_mutex);
	g_work_ring[g_work_count++ % std::size(g_work_ring)] = {kind, hash, a, b, c};
}

uint64_t DebugWorkCount() {
	std::lock_guard lock(g_work_mutex);
	return g_work_count;
}

uint64_t DebugPendingWork(uint64_t done_tick, uint64_t wait_tick) {
	std::lock_guard lock(g_work_mutex);
	const uint64_t n = std::min<uint64_t>(g_work_count, std::size(g_work_ring));
	uint64_t       count    = 0;
	bool           counting = false;
	// Newest first: skip records newer than the waited submit, then count down to the done submit.
	for (uint64_t i = g_work_count; i > g_work_count - n; i--) {
		const auto& r      = g_work_ring[(i - 1) % std::size(g_work_ring)];
		const bool  submit = std::strcmp(r.kind, "submit") == 0;
		if (!counting) {
			if (!submit || r.hash > wait_tick) {
				continue;
			}
			counting = true;
		} else if (submit && r.hash <= done_tick) {
			break;
		}
		count++;
	}
	return count;
}

void DebugOnGpuHang() {
	uint32_t crumb = 0;
	if (!DebugReadBreadcrumb(&crumb)) {
		std::fprintf(stderr, "[hang] breadcrumbs off (KYTY_BREADCRUMBS=1)\n");
		return;
	}
	std::lock_guard lock(g_work_mutex);
	// Stage `crumb` is the first one whose breadcrumb the GPU never wrote.
	std::fprintf(stderr, "[hang] last completed stage %u of %llu recorded\n", crumb,
	             static_cast<unsigned long long>(g_work_count));
	const uint64_t lo = crumb >= 12 ? crumb - 12 : 0;
	for (uint64_t i = lo; i < std::min<uint64_t>(g_work_count, crumb + 12); i++) {
		if (g_work_count - i > std::size(g_work_ring)) continue;
		const auto& r = g_work_ring[i % std::size(g_work_ring)];
		std::fprintf(stderr, "[hang] %s %llu %s 0x%016llx %u %u %u\n", i == crumb ? "==>" : "   ",
		             static_cast<unsigned long long>(i), r.kind, static_cast<unsigned long long>(r.hash),
		             r.a, r.b, r.c);
	}
	std::fflush(stderr);
}

void DebugDumpRecentWork(int count, uint64_t gpu_tick) {
	std::lock_guard lock(g_work_mutex);
	const uint64_t n     = std::min<uint64_t>(g_work_count, std::size(g_work_ring));
	uint64_t       first = g_work_count - std::min<uint64_t>(n, count);
	// Start right after the last completed submission so the first unfinished one is shown.
	for (uint64_t i = g_work_count; i > g_work_count - n; i--) {
		const auto& r = g_work_ring[(i - 1) % std::size(g_work_ring)];
		if (std::strcmp(r.kind, "submit") == 0 && r.hash == gpu_tick) {
			first = i;
			break;
		}
	}
	const uint64_t last = std::min<uint64_t>(g_work_count, first + count);
	if (uint32_t crumb = 0; DebugReadBreadcrumb(&crumb)) {
		std::fprintf(stderr, "[stall] breadcrumb: last completed stage %u\n", crumb);
	}
	std::fprintf(stderr, "[stall] gpu_tick=%llu stages %llu..%llu of %llu (newest last):\n",
	             static_cast<unsigned long long>(gpu_tick), static_cast<unsigned long long>(first),
	             static_cast<unsigned long long>(last), static_cast<unsigned long long>(g_work_count));
	for (uint64_t i = first; i < last; i++) {
		const auto& r = g_work_ring[i % std::size(g_work_ring)];
		uint32_t args[3] = {};
		if (std::strcmp(r.kind, "dispatch-indirect-args") == 0 && DebugReadIndirectProbe(r.a, args)) {
			std::fprintf(stderr, "[stall]   %s 0x%016llx slot=%u groups=%ux%ux%u\n", r.kind,
			             static_cast<unsigned long long>(r.hash), r.a, args[0], args[1], args[2]);
			continue;
		}
		if (std::strcmp(r.kind, "dispatch-indirect") == 0) {
			const uint64_t args_addr = (static_cast<uint64_t>(r.b) << 32) | r.a;
			std::fprintf(stderr, "[stall]   %s 0x%016llx args=0x%016llx\n", r.kind,
			             static_cast<unsigned long long>(r.hash),
			             static_cast<unsigned long long>(args_addr));
			// Latest recorded shader write overlapping the args block (KYTY_TRACK_WRITES=1).
			uint64_t       writer = 0, write_addr = 0, write_size = 0;
			if (DebugLastBufferWriter(args_addr, 12, &writer, &write_addr, &write_size)) {
				std::fprintf(stderr, "[stall]     args=0x%llx last-writer=0x%016llx (0x%llx+0x%llx)\n",
				             static_cast<unsigned long long>(args_addr), static_cast<unsigned long long>(writer),
				             static_cast<unsigned long long>(write_addr), static_cast<unsigned long long>(write_size));
			} else {
				std::fprintf(stderr, "[stall]     args=0x%llx last-writer=none (CPU upload or untracked)\n",
				             static_cast<unsigned long long>(args_addr));
			}
		}
	}
	std::fflush(stderr);
}

static bool LiveFlag(const char* path, std::atomic<int64_t>& next_check, std::atomic<bool>& value) {
	const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
	                     std::chrono::steady_clock::now().time_since_epoch()).count();
	if (now >= next_check.load(std::memory_order_relaxed)) {
		next_check.store(now + 500, std::memory_order_relaxed);
		std::error_code ec;
		const bool exists = std::filesystem::exists(path, ec);
		if (exists != value.exchange(exists)) {
			std::fprintf(stderr, "[flag] %s %s\n", path, exists ? "on" : "off");
		}
	}
	return value.load(std::memory_order_relaxed);
}

bool DebugNoReadback() {
	static std::atomic<int64_t> next_check {0};
	static std::atomic<bool>    value {false};
	return LiveFlag("_TempData/no_readback", next_check, value);
}

bool DebugEopDeferred() {
	static const bool env = [] {
		const char* value = std::getenv("KYTY_EOP_DEFERRED");
		return value != nullptr && value[0] == '1';
	}();
	static std::atomic<int64_t> next_on {0}, next_off {0};
	static std::atomic<bool>    on {false}, off {false};
	return (env || LiveFlag("_TempData/eop_deferred", next_on, on)) &&
	       !LiveFlag("_TempData/eop_immediate", next_off, off);
}

bool DebugUmaReadbackOff() {
	static std::atomic<int64_t> next_check {0};
	static std::atomic<bool>    value {false};
	return LiveFlag("_TempData/uma_off", next_check, value);
}

bool DebugCmaskNoReadback() {
	static std::atomic<int64_t> next_check {0};
	static std::atomic<bool>    value {false};
	const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
	                     std::chrono::steady_clock::now().time_since_epoch()).count();
	if (now >= next_check.load(std::memory_order_relaxed)) {
		next_check.store(now + 500, std::memory_order_relaxed);
		std::error_code ec;
		const bool exists = std::filesystem::exists("_TempData/cmask_no_readback", ec);
		if (exists != value.exchange(exists)) {
			std::fprintf(stderr, "[cmask] no-readback %s\n", exists ? "on" : "off");
		}
	}
	return value.load(std::memory_order_relaxed);
}

void DebugStartFrameTrace(int stages) {
	g_frame_trace_left.store(stages);
	std::fprintf(stderr, "[frame] trace start (%d stages)\n", stages);
}

static bool TraceCubesEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_TRACE_CUBES");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

static std::mutex                                   g_cube_mutex;
static std::vector<std::pair<uint64_t, uint64_t>>   g_cube_ranges;
static std::set<std::string>                        g_cube_seen;

static void CubeLog(std::string line) {
	if (g_cube_seen.size() > 4000 || !g_cube_seen.insert(line).second) {
		return;
	}
	std::fprintf(stderr, "%s\n", line.c_str());
	std::fflush(stderr);
}

static void ReportBufferWriters(uint64_t address, uint64_t size, uint32_t format);
static void RecordImageWrite(uint64_t address, uint64_t size, uint32_t format, uint32_t width,
                             bool render_target);

// KYTY_TRACE_FMT=<vk format number>: trace every binding and render target of that format.
static bool TraceFormatHit(uint32_t format) {
	static const int traced = [] {
		const char* value = std::getenv("KYTY_TRACE_FMT");
		return value != nullptr ? std::atoi(value) : -1;
	}();
	return traced >= 0 && static_cast<uint32_t>(traced) == format;
}

void DebugNoteCubeBinding(uint64_t address, uint64_t size, uint32_t format, uint32_t width,
                          uint32_t levels, uint32_t base_level, uint32_t level_count, bool storage,
                          bool is_cube, uint32_t layers) {
	if (!TraceCubesEnabled()) {
		if (g_debug_log_bindings) {
			std::fprintf(stderr, "[trace]     %s addr=0x%llx fmt=%u w=%u layers=%u view=%u+%u%s\n",
			             storage ? "WRITE" : "read ", static_cast<unsigned long long>(address),
			             format, width, layers, base_level, level_count, is_cube ? " cube" : "");
		}
		return;
	}
	std::lock_guard lock(g_cube_mutex);
	bool known = false, inside = false;
	for (const auto& [a, s]: g_cube_ranges) {
		known |= a == address;
		inside |= address >= a && address < a + s;
	}
	static const std::vector<uint64_t> traced = [] {
		std::vector<uint64_t> list;
		if (const char* value = std::getenv("KYTY_TRACE_ADDRS"); value != nullptr) {
			for (const char* p = value; *p != 0;) {
				char* end = nullptr;
				list.push_back(std::strtoull(p, &end, 16));
				p = (*end == ',') ? end + 1 : end;
				if (end == p && *p != 0) break;
			}
		}
		return list;
	}();
	// KYTY_TRACE_SMALL=1: every write to a small image (<= 64 px wide), plus every binding of the
	// shaders that perform such writes, to reconstruct LUT/average chains.
	static const bool trace_small = [] {
		const char* value = std::getenv("KYTY_TRACE_SMALL");
		return value != nullptr && value[0] == '1';
	}();
	static std::set<uint64_t> small_writers;
	if (trace_small) {
		if (storage && width <= 64) {
			small_writers.insert(g_debug_shader_hash);
		}
		if (small_writers.count(g_debug_shader_hash) != 0) {
			CubeLog(fmt::format("[small] {} addr=0x{:x} fmt={} w={} layers={} view={}+{} shader=0x{:016x}",
			                    storage ? "WRITE" : "read ", address, format, width, layers,
			                    base_level, level_count, g_debug_shader_hash));
		}
	}
	if (g_debug_log_bindings) {
		std::fprintf(stderr, "[trace]     %s addr=0x%llx fmt=%u w=%u layers=%u view=%u+%u%s\n",
		             storage ? "WRITE" : "read ", static_cast<unsigned long long>(address), format,
		             width, layers, base_level, level_count, is_cube ? " cube" : "");
		std::fflush(stderr);
	}
	if (storage) {
		RecordImageWrite(address, size, format, width, false);
	}
	bool traced_hit = TraceFormatHit(format);
	if (traced_hit && !storage) {
		ReportBufferWriters(address, size, format);
	}
	for (const auto a: traced) {
		traced_hit |= a >= address && a < address + std::max<uint64_t>(size, 1);
	}
	if (traced_hit) {
		CubeLog(fmt::format("[trace] {} addr=0x{:x} size=0x{:x} fmt={} w={} layers={} levels={} view={}+{} shader=0x{:016x}",
		                    storage ? "WRITE" : "read ", address, size, format, width, layers,
		                    levels, base_level, level_count, g_debug_shader_hash));
	}
	if (!is_cube && !inside) {
		return;
	}
	if (is_cube && !known) {
		g_cube_ranges.emplace_back(address, size);
	}
	CubeLog(fmt::format("[cube] {} {} addr=0x{:x} size=0x{:x} fmt={} w={} layers={} levels={} view={}+{} shader=0x{:016x}",
	                    storage ? "WRITE" : "read ", is_cube ? "cube " : "alias", address, size,
	                    format, width, layers, levels, base_level, level_count, g_debug_shader_hash));
}

void DebugNoteStage(uint64_t shader_hash) {
	g_debug_shader_hash  = shader_hash;
	g_debug_log_bindings = g_debug_rt_follow > 0;
	if (g_frame_trace_left.load(std::memory_order_relaxed) > 0) {
		const int left = g_frame_trace_left.fetch_sub(1);
		g_debug_log_bindings = left > 0;
		if (left > 0) {
			std::fprintf(stderr, "[frame] stage shader=0x%016llx\n",
			             static_cast<unsigned long long>(shader_hash));
		}
	}
	// KYTY_TRACE_BINDINGS=<hex,...>: log every binding of these shaders.
	static const std::vector<uint64_t> follow = [] {
		std::vector<uint64_t> list;
		if (const char* value = std::getenv("KYTY_TRACE_BINDINGS"); value != nullptr) {
			for (const char* p = value; *p != 0;) {
				char* end = nullptr;
				list.push_back(std::strtoull(p, &end, 16));
				if (end == p) break;
				p = (*end == ',') ? end + 1 : end;
			}
		}
		return list;
	}();
	if (std::find(follow.begin(), follow.end(), shader_hash) != follow.end()) {
		g_debug_log_bindings = true;
		std::fprintf(stderr, "[trace]   draw stage shader=0x%016llx\n",
		             static_cast<unsigned long long>(shader_hash));
	}
	if (g_debug_rt_follow > 0) {
		--g_debug_rt_follow;
		std::fprintf(stderr, "[trace]   draw stage shader=0x%016llx\n",
		             static_cast<unsigned long long>(shader_hash));
		std::fflush(stderr);
	}
}

// KYTY_TRACE_ADDRS also covers storage buffers: log any buffer binding that overlaps a traced
// address, with the stage shader, so data produced through buffers (not images) can be found.
struct BufferWriteRecord {
	uint64_t address;
	uint64_t size;
	uint64_t shader;
};
static std::vector<BufferWriteRecord> g_buffer_writes;

bool DebugLastBufferWriter(uint64_t address, uint64_t size, uint64_t* shader, uint64_t* write_address,
                           uint64_t* write_size) {
	std::lock_guard lock(g_cube_mutex);
	for (auto it = g_buffer_writes.rbegin(); it != g_buffer_writes.rend(); ++it) {
		if (it->address < address + size && address < it->address + it->size) {
			*shader        = it->shader;
			*write_address = it->address;
			*write_size    = it->size;
			return true;
		}
	}
	return false;
}
struct ImageWriteRecord {
	uint64_t address;
	uint64_t size;
	uint64_t shader;
	uint32_t format;
	uint32_t width;
	bool     render_target;
};
static std::vector<ImageWriteRecord> g_image_writes;

static void RecordImageWrite(uint64_t address, uint64_t size, uint32_t format, uint32_t width,
                             bool render_target) {
	if (std::getenv("KYTY_TRACE_FMT") == nullptr) {
		return;
	}
	if (!g_image_writes.empty() && g_image_writes.back().address == address &&
	    g_image_writes.back().shader == g_debug_shader_hash &&
	    g_image_writes.back().format == format) {
		return;
	}
	if (g_image_writes.size() > 400000) {
		g_image_writes.erase(g_image_writes.begin(), g_image_writes.begin() + 100000);
	}
	g_image_writes.push_back({address, size, g_debug_shader_hash, format, width, render_target});
}
static std::set<uint64_t>             g_format_seen;

static void ReportBufferWriters(uint64_t address, uint64_t size, uint32_t format) {
	if (!g_format_seen.insert(address).second) {
		return;
	}
	std::set<std::pair<uint64_t, uint64_t>> printed;
	for (const auto& w: g_buffer_writes) {
		if (w.address < address + size && address < w.address + w.size &&
		    printed.insert({w.address, w.shader}).second) {
			CubeLog(fmt::format("[trace] fmt {} at 0x{:x} was written as buffer 0x{:x}+0x{:x} by "
			                    "shader=0x{:016x}",
			                    format, address, w.address, w.size, w.shader));
		}
	}
	for (const auto& w: g_image_writes) {
		if (w.address < address + size && address < w.address + w.size && w.format != format &&
		    printed.insert({w.address, w.shader ^ w.format}).second) {
			CubeLog(fmt::format("[trace] fmt {} at 0x{:x} was written as {} fmt {} w={} 0x{:x}+0x{:x} "
			                    "by shader=0x{:016x}",
			                    format, address, w.render_target ? "RT" : "storage image", w.format,
			                    w.width, w.address, w.size, w.shader));
		}
	}
	if (printed.empty()) {
		CubeLog(fmt::format("[trace] fmt {} at 0x{:x}: no recorded buffer or image writer", format,
		                    address));
	}
}

void DebugNoteBuffer(uint64_t address, uint64_t size, bool written, uint64_t shader_hash) {
	static const bool record_writes =
	    std::getenv("KYTY_TRACE_FMT") != nullptr || std::getenv("KYTY_TRACE_BINDINGS") != nullptr;
	if (g_debug_log_bindings && (TraceCubesEnabled() || g_frame_trace_left.load() > 0)) {
		std::lock_guard lock(g_cube_mutex);
		std::fprintf(stderr, "[trace]     buf %s addr=0x%llx size=0x%llx\n", written ? "WRITE" : "read ",
		             static_cast<unsigned long long>(address), static_cast<unsigned long long>(size));
		if (!written) {
			static std::set<std::pair<uint64_t, uint64_t>> reported;
			for (auto it = g_buffer_writes.rbegin(); it != g_buffer_writes.rend(); ++it) {
				if (it->address < address + size && address < it->address + it->size &&
				    it->shader != shader_hash && reported.insert({address, it->shader}).second) {
					std::fprintf(stderr, "[trace]       written before by 0x%016llx (0x%llx+0x%llx)\n",
					             static_cast<unsigned long long>(it->shader),
					             static_cast<unsigned long long>(it->address),
					             static_cast<unsigned long long>(it->size));
				}
			}
		}
	}
	static const bool track_writes = std::getenv("KYTY_TRACK_WRITES") != nullptr;
	if (written && (track_writes || (TraceCubesEnabled() && record_writes))) {
		std::lock_guard lock(g_cube_mutex);
		if (g_buffer_writes.empty() || g_buffer_writes.back().address != address ||
		    g_buffer_writes.back().shader != shader_hash) {
			if (g_buffer_writes.size() > 400000) {
				g_buffer_writes.erase(g_buffer_writes.begin(), g_buffer_writes.begin() + 100000);
			}
			g_buffer_writes.push_back({address, size, shader_hash});
		}
	}
	static const std::vector<uint64_t> traced = [] {
		std::vector<uint64_t> list;
		if (const char* value = std::getenv("KYTY_TRACE_ADDRS"); value != nullptr) {
			for (const char* p = value; *p != 0;) {
				char* end = nullptr;
				list.push_back(std::strtoull(p, &end, 16));
				if (end == p) break;
				p = (*end == ',') ? end + 1 : end;
			}
		}
		return list;
	}();
	for (const auto a: traced) {
		if (a >= address && a < address + std::max<uint64_t>(size, 1)) {
			std::lock_guard lock(g_cube_mutex);
			CubeLog(fmt::format("[trace] buf {} addr=0x{:x} size=0x{:x} shader=0x{:016x}",
			                    written ? "WRITE" : "read ", address, size, shader_hash));
			return;
		}
	}
}

void DebugNoteRenderTarget(uint64_t address, uint64_t size, uint32_t format, uint32_t width,
                           uint32_t height, uint32_t mip, uint32_t base_layer, uint32_t layers) {
	if (g_frame_trace_left.load(std::memory_order_relaxed) > 0) {
		std::fprintf(stderr, "[frame] RT addr=0x%llx fmt=%u %ux%u mip=%u layer=%u+%u\n",
		             static_cast<unsigned long long>(address), format, width, height, mip,
		             base_layer, layers);
	}
	if (!TraceCubesEnabled()) {
		return;
	}
	std::lock_guard lock(g_cube_mutex);
	RecordImageWrite(address, size, format, width, true);
	if (TraceFormatHit(format)) {
		CubeLog(fmt::format("[trace] RT    addr=0x{:x} fmt={} {}x{} mip={} layer={}+{} (fmt match)",
		                    address, format, width, height, mip, base_layer, layers));
		g_debug_rt_follow = 2;
	}
	if (const char* value = std::getenv("KYTY_TRACE_ADDRS"); value != nullptr) {
		for (const char* p = value; *p != 0;) {
			char*      end = nullptr;
			const auto a   = std::strtoull(p, &end, 16);
			if (a >= address && a < address + std::max<uint64_t>(size, 1)) {
				CubeLog(fmt::format("[trace] RT    addr=0x{:x} fmt={} {}x{} mip={} layer={}+{}",
				                    address, format, width, height, mip, base_layer, layers));
				g_debug_rt_follow = 2;
			}
			if (end == p) break;
			p = (*end == ',') ? end + 1 : end;
		}
	}
	for (const auto& [a, s]: g_cube_ranges) {
		if (address >= a && address < a + s) {
			CubeLog(fmt::format("[cube] RT    addr=0x{:x} (+0x{:x} in cube 0x{:x}) fmt={} {}x{} mip={} layer={}+{} shader=0x{:016x}",
			                    address, address - a, a, format, width, height, mip, base_layer,
			                    layers, g_debug_shader_hash));
		}
	}
}

} // namespace Libs::Graphics
