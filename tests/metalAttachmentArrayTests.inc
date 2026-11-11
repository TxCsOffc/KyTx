#include "graphics/host_gpu/metal/metalAttachmentArray.h"
#include "graphics/host_gpu/metal/metalDepthClearState.h"
static unsigned AttachmentArrays(id<MTLDevice>                                device,
                                 Libs::Graphics::Metal::MetalPipelineManager& manager) {
	using namespace Libs::Graphics;
	using namespace Metal;
	NSError*       error = nil;
	id<MTLLibrary> library =
	    [device newLibraryWithSource:
	                @"#include <metal_stdlib>\nusing namespace metal;struct V{float4 p "
	                @"[[position]];uint layer [[render_target_array_index]];};vertex V "
	                @"layers_vs(uint id [[vertex_id]],uint instance [[instance_id]]){float2 "
	                @"p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return "
	                @"{float4(p[id],0,1),instance};}struct D{float z [[depth(any)]];};fragment D "
	                @"write_z(constant float& value [[buffer(0)]]){return {value};}kernel void "
	                @"read_z(texture2d_array<float,access::read> t [[texture(0)]],device float* "
	                @"out [[buffer(0)]],uint3 id "
	                @"[[thread_position_in_grid]]){if(id.x<8&&id.y<8&&id.z<t.get_array_size())out["
	                @"id.z*64+id.y*8+id.x]=t.read(id.xy,id.z).x;}kernel void "
	                @"read_s(texture2d_array<uint,access::read> t [[texture(0)]],device uint* out "
	                @"[[buffer(0)]],uint3 id "
	                @"[[thread_position_in_grid]]){if(id.x<8&&id.y<8&&id.z<t.get_array_size())out["
	                @"id.z*64+id.y*8+id.x]=t.read(id.xy,id.z).x;}"
	                         options:nil
	                           error:&error];
	if (!library) {
		std::fprintf(stderr, "Attachment array library: %s\n",
		             error.localizedDescription.UTF8String);
		return 1;
	}
	id<MTLComputePipelineState> depthRead =
	    [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"read_z"]
	                                          error:&error];
	id<MTLComputePipelineState> stencilRead =
	    [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"read_s"]
	                                          error:&error];
	if (!depthRead || !stencilRead) return 1;
	auto&    context  = MetalContext::Instance();
	auto&    draw     = MetalDrawExecutor::Instance();
	unsigned failures = 0, checks = 0;
	auto     verify = [&](id<MTLTexture> view, const std::vector<float>& expected, bool stencil) {
		context.CommitCommandBuffer(true);
		id<MTLCommandQueue>  q   = [device newCommandQueue];
		id<MTLCommandBuffer> cb  = [q commandBuffer];
		id<MTLBuffer>        out = [device newBufferWithLength:expected.size() * 64 * 4
		                                               options:MTLResourceStorageModeShared];
		id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
		[encoder setComputePipelineState:stencil ? stencilRead : depthRead];
		[encoder setTexture:view atIndex:0];
		[encoder setBuffer:out offset:0 atIndex:0];
		[encoder dispatchThreads:MTLSizeMake(8, 8, expected.size())
		    threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
		[encoder endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		if (cb.status != MTLCommandBufferStatusCompleted) {
			++failures;
			return;
		}
		for (size_t layer = 0; layer < expected.size(); ++layer)
			for (unsigned pixel = 0; pixel < 64; ++pixel) {
				float actual = stencil ? float(((const uint32_t*)out.contents)[layer * 64 + pixel])
				                       : ((const float*)out.contents)[layer * 64 + pixel];
				failures += std::fabs(actual - expected[layer]) > 1.f / 65535.f;
				++checks;
			}
	};
	for (auto format: {MTLPixelFormatDepth16Unorm, MTLPixelFormatDepth32Float}) {
		MetalAttachmentArray   surface;
		MetalRenderPipelineKey key {};
		key.vs_hash        = 0xa000 + format;
		key.fs_hash        = 0xb000 + format;
		key.depth_format   = format;
		key.primitive_type = uint32_t(Prospero::PrimitiveType::kTriList);
		id<MTLRenderPipelineState> pipeline =
		    manager.GetOrCreateRenderPipeline(key, [library newFunctionWithName:@"layers_vs"],
			                                  [library newFunctionWithName:@"write_z"]);
		if (!pipeline) return 1;
		HW::DepthControl dc {};
		dc.z_enable                 = true;
		dc.z_write_enable           = true;
		dc.zfunc                    = 7;
		auto               state    = manager.GetOrCreateDepthStencilState(dc, {}, {});
		const float        values[] = {.125f, .375f, .75f};
		std::vector<float> expected;
		for (unsigned layer = 0; layer < 3; ++layer) {
			failures +=
			    !surface.Ensure(device, context.GetCurrentCommandBuffer(), 8, 8, format, layer + 1);
			failures += !surface.Clear(context.GetCurrentCommandBuffer(), layer, 1, .9, 0, true);
			MetalRenderPassDesc pass {};
			pass.has_depth = true;
			pass.depth_attachment.texture =
			    surface.View(std::getenv("KYTY_DEPTH_ARRAY_LEGACY") ? 0 : layer, 1);
			pass.depth_attachment.load_action = MTLLoadActionLoad;
			failures += !draw.BeginRenderPass(pass);
			draw.SetRenderPipeline(pipeline);
			draw.SetDepthStencil(state);
			draw.SetFragmentBytes(&values[layer], 4, 0);
			draw.Draw(Prospero::PrimitiveType::kTriList, 3);
			draw.EndRenderPass();
			expected.push_back(values[layer]);
			verify(surface.Texture(), expected, false);
		}
		// A full metadata clear is consumed per attachment view, preserving
		// already rendered cascades until their own view is cleared.
		PendingDepthClears metadata;
		metadata.MarkAll();
		for (unsigned layer = 0; layer < 3; ++layer) {
			const auto selected = metadata.Take(layer, 1);
			failures += selected != std::vector<uint32_t> {layer};
			if (std::getenv("KYTY_HTILE_CLEAR_ALL_LEGACY"))
				failures += !surface.Clear(context.GetCurrentCommandBuffer(), 0, 3, .875, 0);
			else
				for (uint32_t clearLayer: selected)
					failures +=
					    !surface.Clear(context.GetCurrentCommandBuffer(), clearLayer, 1, .875, 0);
			expected[layer] = .875f;
			verify(surface.Texture(), expected, false);
			failures += !metadata.Take(layer, 1).empty();
		}
		metadata.MarkAll();
		failures += !metadata.Take(2047, 2).empty();
		failures += metadata.Take(2047, 1) != std::vector<uint32_t> {2047};

		MetalRenderPassDesc pass {};
		pass.has_depth                    = true;
		pass.render_target_array_length   = 2;
		pass.depth_attachment.texture     = surface.View(1, 2);
		pass.depth_attachment.load_action = MTLLoadActionLoad;
		const float value                 = .25f;
		failures += !draw.BeginRenderPass(pass);
		draw.SetRenderPipeline(pipeline);
		draw.SetDepthStencil(state);
		draw.SetFragmentBytes(&value, 4, 0);
		draw.Draw(Prospero::PrimitiveType::kTriList, 3, 2);
		draw.EndRenderPass();
		verify(surface.Texture(), {.875f, .25f, .25f}, false);
		verify(surface.View(1, 2), {.25f, .25f}, false);
		const auto original = surface.Texture();
		failures += surface.View(3, 1) != nil;
		failures += surface.View(2, 2) != nil;
		failures += surface.Ensure(device, context.GetCurrentCommandBuffer(), 8, 8, format, 0);
		failures += surface.Texture() != original;
	}
	MetalAttachmentArray stencil;
	for (unsigned layer = 0; layer < 3; ++layer) {
		failures += !stencil.Ensure(device, context.GetCurrentCommandBuffer(), 8, 8,
		                            MTLPixelFormatStencil8, layer + 1);
		failures += !stencil.Clear(context.GetCurrentCommandBuffer(), layer, 1, 0, 17 + layer);
		std::vector<float> expected;
		for (unsigned i = 0; i <= layer; ++i)
			expected.push_back(float(17 + i));
		verify(stencil.Texture(), expected, true);
	}
	HW::Context       stencilContext {};
	HW::RenderControl renderControl {};
	renderControl.stencil_clear_enable = true;
	stencilContext.SetRenderControl(renderControl);
	for (bool readOnly: {true, false}) {
		HW::DepthRenderTarget target {};
		target.depth_view.stencil_write_disable = readOnly;
		stencilContext.SetDepthRenderTarget(target);
		if (GuestStencilClearEnabled(stencilContext))
			failures += !stencil.Clear(context.GetCurrentCommandBuffer(), 0, 3, 0, 31);
		verify(stencil.Texture(),
		       readOnly ? std::vector<float> {17, 18, 19} : std::vector<float> {31, 31, 31}, true);
	}
	std::printf("Depth/stencil array growth, selected views, HTile clears, layered draws: %u GPU "
	            "texels, %u failures\n",
	            checks, failures);
	return failures;
}
