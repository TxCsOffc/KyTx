// Standalone GPU regression test: tools/test_metal_sampler.sh
#include "graphics/host_gpu/metal/metalSampler.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace Libs::Graphics;

static void Check(bool ok, const char* message) {
	if (!ok) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}

int main() {
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		Check(device != nil, "Metal device unavailable");
		std::array<ShaderSamplerResource, 9> descriptors {};
		descriptors[1].fields[0] = 2u | (2u << 3u) | (2u << 6u); // edge clamp
		descriptors[2] = descriptors[1];
		descriptors[2].fields[2] = (1u << 20u) | (1u << 22u); // bilinear
		descriptors[3] = descriptors[1];
		descriptors[3].fields[1] = 256u << 12u; // max LOD 1
		descriptors[3].fields[2] = 1u << 26u; // nearest mip
		descriptors[4].fields[0] = 6u | (6u << 3u) | (6u << 6u);
		descriptors[4].fields[3] = 2u << 30u; // opaque white border
		descriptors[5] = descriptors[1];
		descriptors[5].fields[0] |= 1u << 15u; // pixel coordinates
		descriptors[6] = descriptors[2]; descriptors[6].fields[0] |= 1u << 29u; // minimum
		descriptors[7] = descriptors[2]; descriptors[7].fields[0] |= 2u << 29u; // maximum
		descriptors[8] = descriptors[5]; descriptors[8].fields[2] = 1u << 20u; // pixel: min point, mag linear

		NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
		    "struct Args { array<sampler, 9> s [[id(0)]]; };\n"
		    "kernel void test(constant Args& a [[buffer(0)]], device float4* out [[buffer(1)]], texture2d<float> t [[texture(0)]]) {\n"
		    "out[0] = t.sample(a.s[0], float2(1.25, 0.5), level(0));\n"
		    "out[1] = t.sample(a.s[1], float2(1.25, 0.5), level(0));\n"
		    "out[2] = t.sample(a.s[2], float2(0.5, 0.5), level(0));\n"
		    "out[3] = t.sample(a.s[3], float2(0.5, 0.5), level(1));\n"
		    "out[4] = t.sample(a.s[4], float2(-0.5, 0.5), level(0));\n"
		    "out[5] = t.sample(a.s[5], float2(1.5, 0.5));\n"
		    "out[6] = t.sample(a.s[6], float2(0.5, 0.5), level(0));\n"
		    "out[7] = t.sample(a.s[7], float2(0.5, 0.5), level(0));\n"
		    "out[8] = t.sample(a.s[8], float2(1.0, 0.5));\n}\n";
		NSError* error = nil;
		id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
		if (!library) std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
		Check(library != nil, "test shader failed to compile");
		id<MTLFunction> function = [library newFunctionWithName:@"test"];
		id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
		Check(pipeline != nil, "test pipeline failed");
		id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:0];
		id<MTLBuffer> argumentBuffer = [device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
		[arguments setArgumentBuffer:argumentBuffer offset:0];
		for (NSUInteger i = 0; i < descriptors.size(); ++i) {
			id<MTLSamplerState> sampler = Metal::GetSampler(device, descriptors[i]);
			Check(sampler != nil && sampler.gpuResourceID._impl != 0, "sampler missing argument-buffer support");
			Check(sampler == Metal::GetSampler(device, descriptors[i]), "sampler cache failed");
			[arguments setSamplerState:sampler atIndex:i];
		}
		MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:2 height:1 mipmapped:YES];
		textureDescriptor.storageMode = MTLStorageModeShared;
		id<MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor];
		const float pixels[8] {1, 0, 0, 1, 0, 1, 0, 1};
		const float mip[4] {0, 0, 1, 1};
		[texture replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0 withBytes:pixels bytesPerRow:sizeof(pixels)];
		[texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:1 withBytes:mip bytesPerRow:sizeof(mip)];
		id<MTLBuffer> output = [device newBufferWithLength:descriptors.size() * sizeof(mip) options:MTLResourceStorageModeShared];
		id<MTLCommandQueue> queue = [device newCommandQueue];
		id<MTLCommandBuffer> command = [queue commandBuffer];
		id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
		[encoder setComputePipelineState:pipeline];
		[encoder setBuffer:argumentBuffer offset:0 atIndex:0];
		[encoder setBuffer:output offset:0 atIndex:1];
		[encoder setTexture:texture atIndex:0];
		[encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
		[encoder endEncoding];
		[command commit];
		[command waitUntilCompleted];
		Check(command.status == MTLCommandBufferStatusCompleted, "GPU sampling failed");
		const float expected[36] {1,0,0,1, 0,1,0,1, 0.5,0.5,0,1, 0,0,1,1, 1,1,1,1, 0,1,0,1,
		                          0,0,0,1, 1,1,0,1, 0.5,0.5,0,1};
		const auto* values = static_cast<const float*>(output.contents);
		for (size_t i = 0; i < std::size(expected); ++i) {
			if (std::fabs(values[i] - expected[i]) > 0.001f || !std::isfinite(values[i])) {
				std::fprintf(stderr, "sample %zu component %zu: %g expected %g\n", i / 4, i % 4, values[i], expected[i]);
				return 1;
			}
		}
		std::puts("Metal sampler GPU tests passed (wrap, clamp, bilinear, mip, border, pixel coordinates, min/max reduction, argument buffers)");
	}
}
