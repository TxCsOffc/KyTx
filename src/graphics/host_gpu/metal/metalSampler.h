#pragma once

#include "graphics/shader/shaderBindings.h"

#import <Metal/Metal.h>

namespace Libs::Graphics::Metal {

// S# describes filtering and addressing independently of the texture's T#.
[[nodiscard]] MTLSamplerDescriptor* MakeSamplerDescriptor(const ShaderSamplerResource& resource);
[[nodiscard]] id<MTLSamplerState> GetSampler(id<MTLDevice> device,
                                            const ShaderSamplerResource& resource);

} // namespace Libs::Graphics::Metal
