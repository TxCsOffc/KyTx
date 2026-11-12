// Native IR -> MSL -> Metal GPU runner for ShaderRecompilerComputeTests cases.
#include "metalComputeCaseRunner.h"
#include "graphics/shader/recompiler/backend/msl/MslEmitter.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace Libs::Graphics;
namespace IR = ShaderRecompiler::IR;

namespace {

id<MTLDevice> GetDevice() {
	static id<MTLDevice> s_device = MTLCreateSystemDefaultDevice();
	return s_device;
}

bool WaitOrKill(id<MTLCommandBuffer> command) {
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	[command addCompletedHandler:^(id<MTLCommandBuffer>) { dispatch_semaphore_signal(done); }];
	[command commit];
	if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) != 0) {
		FILE* f = fopen("/Users/tx/Developer/KyTx/.porting/work/HOST_GPU_TESTS_BLOCKED", "w");
		if (f) { fputs("GPU packet timeout in metal cases", f); fclose(f); }
		std::_Exit(75);
	}
	return command.status == MTLCommandBufferStatusCompleted;
}

} // namespace

namespace KytxMetalCases {

bool Run(const IR::Program& program, const ShaderComputeInputInfo& compute_info,
         const IR::PushData& push, std::vector<uint32_t>& buffer, const std::vector<uint32_t>& slot_offsets,
         const std::vector<uint32_t>& slot_sizes,
         std::vector<uint32_t>* gds, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z,
         std::string& error) {
	id<MTLDevice> device = GetDevice();
	if (device == nil) {
		error = "no default Metal device";
		return false;
	}

	ShaderStageInputInfo input {};
	input.compute = &compute_info;
	ShaderRecompiler::Msl::EmitResult emitted = ShaderRecompiler::Msl::EmitProgram(program, input);
	if (!emitted.success) {
		for (const auto& reason : emitted.unsupported) error += reason;
		return false;
	}

	NSError* nsError = nil;
	MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
	if (@available(macOS 15.0, *)) {
		options.mathMode = MTLMathModeSafe;
		options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
	} else {
		options.fastMathEnabled = NO;
	}
	id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:emitted.source.c_str()] options:options error:&nsError];
	if (library == nil) {
		error = std::string([nsError.localizedDescription UTF8String]);
		return false;
	}
	id<MTLFunction> function = [library newFunctionWithName:@"main0"];
	if (function == nil) {
		error = "no main0 entry point";
		return false;
	}

	id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&nsError];
	if (pipeline == nil) {
		error = std::string([nsError.localizedDescription UTF8String]);
		return false;
	}

	const IR::DescriptorBinding* buffers_binding = IR::FindBinding(program.bindings, IR::DescriptorBindingKind::Buffers);
	const IR::DescriptorBinding* gds_binding = IR::FindBinding(program.bindings, IR::DescriptorBindingKind::Gds);

	// One data buffer behind every buffer slot (as the Vulkan harness binds one storage buffer), one GDS buffer.
	id<MTLBuffer> data = nil, gds_buffer = nil;
	if (buffers_binding != nullptr) {
		data = [device newBufferWithLength:buffer.size() * 4u options:MTLResourceStorageModeShared];
		std::memset(data.contents, 0, data.length);
		std::memcpy(data.contents, buffer.data(), buffer.size() * 4u);
	}
	if (gds_binding != nullptr) {
		if (gds == nullptr) {
			error = "program has a GDS binding but no GDS storage";
			return false;
		}
		gds_buffer = [device newBufferWithLength:gds->size() * 4u options:MTLResourceStorageModeShared];
		std::memcpy(gds_buffer.contents, gds->data(), gds->size() * 4u);
	}
	id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:0];
	id<MTLBuffer> argbuf = [device newBufferWithLength:std::max<NSUInteger>(arguments.encodedLength, 16) options:MTLResourceStorageModeShared];
	[arguments setArgumentBuffer:argbuf offset:0];
	for (uint32_t i = 0; buffers_binding != nullptr && i < buffers_binding->resources.size(); ++i)
		[arguments setBuffer:data offset:(i < slot_offsets.size() ? slot_offsets[i] : 0u) atIndex:static_cast<NSUInteger>(i)];
	if (gds_buffer != nil) [arguments setBuffer:gds_buffer offset:0 atIndex:161];

	id<MTLCommandQueue> queue = [device newCommandQueue];
	id<MTLCommandBuffer> command = [queue commandBuffer];
	id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
	[encoder setComputePipelineState:pipeline];

	[encoder setBuffer:argbuf offset:0 atIndex:0];
	[encoder setBytes:push.dwords.data() length:sizeof(push.dwords) atIndex:1];
	std::array<uint32_t, 64> sizes {};
	if (buffers_binding != nullptr) {
		for (uint32_t i = 0; i < buffers_binding->resources.size(); ++i) sizes[i] = i < slot_sizes.size() ? slot_sizes[i] : 0u;
	}
	[encoder setBytes:sizes.data() length:sizeof(sizes) atIndex:25];

	if (data != nil) [encoder useResource:data usage:MTLResourceUsageRead | MTLResourceUsageWrite];
	if (gds_buffer != nil) [encoder useResource:gds_buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];

	uint32_t w = compute_info.threads_num[0], h = compute_info.threads_num[1], d = compute_info.threads_num[2];
	if (w == 0) w = 1;
	if (h == 0) h = 1;
	if (d == 0) d = 1;
	uint32_t tg_w = w, tg_h = h, tg_d = d;
	if (emitted.wave64_pair) {
		tg_w = (w * h * d) / 2;
		tg_h = 1;
		tg_d = 1;
	}
	[encoder dispatchThreadgroups:MTLSizeMake(groups_x, groups_y, groups_z)
	    threadsPerThreadgroup:MTLSizeMake(tg_w, tg_h, tg_d)];
	[encoder endEncoding];

	if (!WaitOrKill(command)) {
		error = "GPU dispatch failed (status not completed)";
		return false;
	}

	if (data != nil) std::memcpy(buffer.data(), data.contents, buffer.size() * 4u);
	if (gds_buffer != nil) std::memcpy(gds->data(), gds_buffer.contents, gds->size() * 4u);
	return true;
}

} // namespace KytxMetalCases
