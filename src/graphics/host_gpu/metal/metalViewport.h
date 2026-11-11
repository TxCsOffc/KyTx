#pragma once
#import <Metal/Metal.h>

namespace Libs::Graphics::Metal {

// Guest/Vulkan signed viewport coordinates, before Metal's Y-down rasterization.
inline MTLViewport TranslateGuestViewport(double x, double y, double width, double height,
                                          double znear, double zfar) {
	// Metal's NDC Y axis is opposite to the guest/Vulkan axis. Preserve the
	// signed guest transform, including positive guest heights used by offscreen
	// targets. Moving the origin alone loses the reflection for those viewports.
	return {x, y + height, width, -height, znear, zfar};
}

} // namespace Libs::Graphics::Metal
