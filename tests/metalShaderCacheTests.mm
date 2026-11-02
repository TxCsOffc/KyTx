// Exercise the real function cache with two buffer-stride specializations of one guest shader.
#include "graphics/host_gpu/metal/metalShader.h"
#include "graphics/host_gpu/metal/metalContext.h"
#include "common/logging/log.h"
#include <cstdio>

namespace Libs::Graphics::Metal {
MetalContext::MetalContext() { m_device = MTLCreateSystemDefaultDevice(); }
MetalContext::~MetalContext() = default;
MetalContext& MetalContext::Instance() { static MetalContext context; return context; }
bool MetalContext::Initialize() { return m_device != nil; }
}
namespace Log {
bool IsSilent() { return true; }
void Write(fmt::text_style, std::string_view text) { std::fprintf(stderr, "%.*s", int(text.size()), text.data()); }
}

static unsigned Run(bool legacy) {
	using namespace Libs::Graphics::Metal;
	MetalShaderManager manager;
	constexpr uint64_t guest_hash = 0x123456789abcdef0ull;
	constexpr uint32_t stage = 4; // ShaderType::Compute
	const std::string sources[] = {
	    "#include <metal_stdlib>\nusing namespace metal;\n"
	    "kernel void main0(device uint* out [[buffer(0)]], const device uint* in [[buffer(1)]]) { out[0] = in[1]; }",
	    "#include <metal_stdlib>\nusing namespace metal;\n"
	    "kernel void main0(device uint* out [[buffer(0)]], const device uint* in [[buffer(1)]]) { out[0] = in[2]; }",
	};
	const uint64_t keys[] = {
	    legacy ? guest_hash : NativeMslFunctionKey(guest_hash, stage, sources[0]),
	    legacy ? guest_hash : NativeMslFunctionKey(guest_hash, stage, sources[1]),
	};
	for (unsigned i = 0; i < 2; ++i) {
		const auto compiled = manager.CompileMslToFunction(sources[i], "main0", stage, keys[i]);
		if (!compiled.success) {
			std::fprintf(stderr, "Compile failed: %s\n", compiled.error_log.c_str());
			return 100;
		}
	}
	// A rejected emission must not pick up a valid function from the same guest shader.
	const uint64_t failed_key = legacy ? guest_hash : NativeMslFunctionKey(guest_hash, stage, sources[0], false);
	unsigned failures = manager.GetFunction(failed_key) != nil;
	id<MTLDevice> device = MetalContext::Instance().GetDevice();
	id<MTLCommandQueue> queue = [device newCommandQueue];
	const uint32_t input[] = {0xdeadbeefu, 17, 34};
	id<MTLBuffer> in = [device newBufferWithBytes:input length:sizeof(input) options:MTLResourceStorageModeShared];
	id<MTLBuffer> out = [device newBufferWithLength:3 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
	id<MTLCommandBuffer> command = [queue commandBuffer];
	// Revisit A after compiling B: each cached function must keep its own stride.
	const unsigned variants[] = {0, 1, 0};
	for (unsigned i = 0; i < 3; ++i) {
		NSError* error = nil;
		id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:manager.GetFunction(keys[variants[i]]) error:&error];
		if (!pipeline) {
			std::fprintf(stderr, "Pipeline failed: %s\n", error.localizedDescription.UTF8String);
			return 100;
		}
		id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
		[encoder setComputePipelineState:pipeline];
		[encoder setBuffer:out offset:i * sizeof(uint32_t) atIndex:0];
		[encoder setBuffer:in offset:0 atIndex:1];
		[encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
		[encoder endEncoding];
	}
	[command commit];
	[command waitUntilCompleted];
	if (command.status == MTLCommandBufferStatusError) {
		std::fprintf(stderr, "GPU failed: %s\n", command.error.localizedDescription.UTF8String);
		return 100;
	}
	const auto* values = static_cast<const uint32_t*>(out.contents);
	for (unsigned i = 0; i < 3; ++i) failures += values[i] != input[variants[i] + 1];
	std::printf("%s native function cache: outputs %u/%u/%u, rejected emission %s, %u failures\n",
	            legacy ? "Legacy" : "Specialized", values[0], values[1], values[2],
	            manager.GetFunction(failed_key) == nil ? "unbound" : "reused valid function", failures);
	return failures;
}

int main() {
	@autoreleasepool {
		if (!Libs::Graphics::Metal::MetalContext::Instance().GetDevice()) {
			std::fprintf(stderr, "Metal GPU unavailable\n");
			return 1;
		}
		// Prove that the same test exposes both old failures, then check the corrected identity.
		if (Run(true) != 2) return 1;
		return Run(false) == 0 ? 0 : 1;
	}
}
