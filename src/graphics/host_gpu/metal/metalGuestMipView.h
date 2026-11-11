#pragma once

#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace Libs::Graphics::Metal {

struct GuestMipViewPlan {
	uint32_t physical_levels = 1;
	uint32_t texture_levels = 1;
	uint32_t first_level = 0;
};

// MAX_MIP describes the underlying allocation; BASE/LAST describe a view.
// A uint view of BC blocks can be 64x64 with MAX_MIP=8: its 256x256 BC
// allocation has nine levels, but the active uint view has only mip zero.
// Keep all physical levels for address calculation and create only the host
// levels that the view can reach. Never truncate the guest layout to fit Metal.
inline bool PlanGuestMipView(const ShaderTextureResource& descriptor, uint32_t width, uint32_t height,
                             bool r128, GuestMipViewPlan& out, uint32_t depth = 1) {
	if (width == 0 || height == 0 || depth == 0 || descriptor.BaseLevel() > descriptor.LastLevel()) return false;
	GuestMipViewPlan plan;
	plan.texture_levels = uint32_t(descriptor.LastLevel()) + 1u;
	plan.physical_levels = r128 ? plan.texture_levels :
	    std::max(plan.texture_levels,uint32_t(descriptor.MaxMip()) + 1u);
	if (plan.physical_levels > 16u) return false;
	if (plan.texture_levels > std::bit_width(std::max({width,height,depth}))) {
		// A BC-block view can select a repeated 1x1 physical tail mip beyond
		// its natural uint-image mip limit. Keep the physical offset and decode
		// this single-level view into a real 1x1 host texture.
		if (descriptor.BaseLevel() != descriptor.LastLevel() ||
		    descriptor.TileMode() == Prospero::TileMode::kLinear) return false;
		plan.first_level = descriptor.BaseLevel();
		plan.texture_levels = 1;
	}
	out = plan;
	return true;
}

} // namespace Libs::Graphics::Metal
