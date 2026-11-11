#pragma once
#include "graphics/host_gpu/metal/metalUnsupported.h"
#import <Metal/Metal.h>
#include <cmath>
namespace Libs::Graphics::Metal {
inline bool SupportsNativeDepthBounds(id<MTLDevice> device) {
	if(@available(macOS 26.0,*))return device!=nil && [device supportsFamily:MTLGPUFamilyApple10];
	return false;
}
inline void EncodeDepthBounds(id<MTLRenderCommandEncoder> encoder,bool enabled,float minimum,float maximum,MTLPixelFormat format) {
	if(enabled && (!std::isfinite(minimum)||!std::isfinite(maximum)||minimum<0.f||maximum>1.f||minimum>maximum))
		RejectMetalState("depth bounds outside native finite ordered [0,1] interval");
	if(enabled && format==MTLPixelFormatDepth16Unorm) {
		// Metal quantizes bounds to D16. Select exactly the representable
		// values satisfying the original FLOAT32 guest comparisons instead
		// of allowing round-to-nearest to admit one extra depth code.
		auto first=[&](float bound,bool strict){uint32_t lo=0,hi=65536;while(lo<hi){uint32_t mid=lo+(hi-lo)/2;float z=float(mid)/65535.f;if(strict?z<=bound:z<bound)lo=mid+1;else hi=mid;}return lo;};
		const uint32_t lower=first(minimum,false),after=first(maximum,true);
		if(lower>=after)RejectMetalState("D16 depth bounds contains no representable depth; native empty-interval lowering required");
		minimum=float(lower)/65535.f;maximum=float(after-1)/65535.f;
	}
	if(encoder==nil)RejectMetalState("depth bounds without a render encoder");
	if(SupportsNativeDepthBounds(encoder.device)) {
		if(@available(macOS 26.0,*))[encoder setDepthTestMinBound:enabled?minimum:0.f maxBound:enabled?maximum:1.f];
	} else if(enabled)RejectMetalState("depth bounds needs Apple10 and macOS26 or a software lowering");
}
}
