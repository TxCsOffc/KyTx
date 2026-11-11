#pragma once

#include "graphics/shader/shaderBindings.h"

#import <Metal/Metal.h>

namespace Libs::Graphics::Metal {

// Color views with other formats require explicit texel comparisons: a depth
// handle cast alone does not preserve their mip/filter behavior on Apple GPUs.
[[nodiscard]] bool SupportsNativeDepthCompare(MTLPixelFormat format);

// S# describes filtering and addressing independently of the texture's T#.
[[nodiscard]] MTLSamplerDescriptor* MakeSamplerDescriptor(const ShaderSamplerResource& resource,
                                                          bool comparison_point = false);
[[nodiscard]] id<MTLSamplerState> GetSampler(id<MTLDevice> device,
                                            const ShaderSamplerResource& resource,
                                            bool comparison_point = false);

} // namespace Libs::Graphics::Metal
