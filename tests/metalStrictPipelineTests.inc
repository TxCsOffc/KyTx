static unsigned StrictPipelineContracts(Libs::Graphics::Metal::MetalPipelineManager& manager) {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::Metal;
	unsigned failures = 0, rejected = 0;
	auto     reject = [&](auto action) {
		try {
			action();
			++failures;
		} catch (const std::runtime_error&) {
			++rejected;
		}
	};
	reject([] { TranslateBlendFactor(0xff); });
	reject([] { TranslateBlendOperation(0xff); });
	reject([] { TranslateCompareFunction(8); });
	reject([] { TranslateStencilOperation(8); });
	{
		HW::DepthControl dc {}; dc.depth_bounds_enable=true;
		if(SupportsNativeDepthBounds(MetalContext::Instance().GetDevice()))
			failures+=manager.GetOrCreateDepthStencilState(dc,{},{})==nil;
		else reject([&]{(void)manager.GetOrCreateDepthStencilState(dc,{},{});});
	}
	reject([&] {
		HW::DepthControl dc {};
		dc.stencil_enable = true;
		dc.stencilfunc    = 2;
		HW::StencilControl sc {};
		sc.stencil_zpass = uint8_t(Prospero::StencilOp::kReplaceOp);
		HW::StencilMask sm {};
		sm.stencil_mask = sm.stencil_writemask = 0xff;
		sm.stencil_testval                     = 1;
		sm.stencil_opval                       = 2;
		(void)manager.GetOrCreateDepthStencilState(dc, sc, sm, true, false, nullptr);
	});
	std::printf("Strict pipeline: %u unsupported states rejected, %u failures\n", rejected,
	            failures);
	return failures;
}

static unsigned StencilXor(id<MTLDevice>                                device,
                           Libs::Graphics::Metal::MetalPipelineManager& manager,
                           id<MTLFunction> vertex, id<MTLLibrary> library) {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::Metal;
	MetalRenderPipelineKey key {};
	key.vs_hash                       = 0x58524f;
	key.fs_hash                       = 0x58524f;
	key.color_target_count            = 1;
	key.color_targets[0].pixel_format = MTLPixelFormatRGBA8Unorm;
	key.stencil_format                = MTLPixelFormatStencil8;
	key.primitive_type                = uint32_t(Prospero::PrimitiveType::kTriList);
	auto pipeline =
	    manager.GetOrCreateRenderPipeline(key, vertex, [library newFunctionWithName:@"redc"]);
	if (!pipeline) return 1;
	unsigned wrong = 0, cases = 0;
	for (uint8_t mask: {0u, 0x0fu, 0xffu})
		for (uint8_t operand: {0u, 0x5au, 0xffu})
			for (uint8_t initial: {0u, 0xa5u, 0xffu}) {
				HW::DepthControl dc {};
				dc.stencil_enable = true;
				dc.stencilfunc    = 7;
				HW::StencilControl sc {};
				sc.stencil_zpass = uint8_t(Prospero::StencilOp::kXor);
				HW::StencilMask sm {};
				sm.stencil_writemask = mask;
				sm.stencil_opval     = operand;
				auto state = manager.GetOrCreateDepthStencilState(dc, sc, sm, true, false, nullptr);
				MTLTextureDescriptor* td             = [MTLTextureDescriptor
				    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				                                 width:8
				                                height:8
				                             mipmapped:NO];
				td.storageMode                       = MTLStorageModeShared;
				td.usage                             = MTLTextureUsageRenderTarget;
				auto color                           = [device newTextureWithDescriptor:td];
				td.pixelFormat                       = MTLPixelFormatStencil8;
				td.storageMode                       = MTLStorageModePrivate;
				auto                     stencil     = [device newTextureWithDescriptor:td];
				MTLRenderPassDescriptor* pass        = [MTLRenderPassDescriptor new];
				pass.colorAttachments[0].texture     = color;
				pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.stencilAttachment.texture       = stencil;
				pass.stencilAttachment.loadAction    = MTLLoadActionClear;
				pass.stencilAttachment.clearStencil  = initial;
				pass.stencilAttachment.storeAction   = MTLStoreActionStore;
				auto cb                              = [[device newCommandQueue] commandBuffer];
				auto enc                             = [cb renderCommandEncoderWithDescriptor:pass];
				[enc setRenderPipelineState:pipeline];
				[enc setDepthStencilState:state];
				[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[enc endEncoding];
				auto result = [device newBufferWithLength:256 * 8
				                                  options:MTLResourceStorageModeShared];
				auto blit   = [cb blitCommandEncoder];
				[blit copyFromTexture:stencil
				                 sourceSlice:0
				                 sourceLevel:0
				                sourceOrigin:MTLOriginMake(0, 0, 0)
				                  sourceSize:MTLSizeMake(8, 8, 1)
				                    toBuffer:result
				           destinationOffset:0
				      destinationBytesPerRow:256
				    destinationBytesPerImage:256 * 8];
				[blit endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
				if (cb.status != MTLCommandBufferStatusCompleted) return 1;
				const uint8_t expected = initial ^ (mask & operand);
				for (unsigned y = 0; y < 8; ++y)
					for (unsigned x = 0; x < 8; ++x)
						wrong +=
						    static_cast<const uint8_t*>(result.contents)[y * 256 + x] != expected;
				++cases;
			}
	std::printf("Guest stencil XOR: %u GPU cases / %u texels / %u failures\n", cases, cases * 64,
	            wrong);
	return wrong;
}

static unsigned GuestViewportArray(id<MTLDevice>                                device,
                                   Libs::Graphics::Metal::MetalPipelineManager& manager) {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::Metal;
	NSError* error = nil;
	auto     library =
	    [device newLibraryWithSource:
	                @"#include <metal_stdlib>\nusing namespace metal;struct O{float4 p "
					@"[[position]];float size [[point_size]];uint vp "
					@"[[viewport_array_index]];};vertex O vs(uint i [[instance_id]]){return "
					@"{float4(0,0,.5,1),8,i};}fragment float4 fs(){return float4(1);}"
	                         options:nil
	                           error:&error];
	if (!library) return 1;
	MetalRenderPipelineKey key {};
	key.vs_hash                       = 0x565041;
	key.fs_hash                       = 0x565041;
	key.primitive_type                = uint32_t(Prospero::PrimitiveType::kPointList);
	key.color_target_count            = 1;
	key.color_targets[0].pixel_format = MTLPixelFormatRGBA8Unorm;
	auto pso = manager.GetOrCreateRenderPipeline(key, [library newFunctionWithName:@"vs"],
	                                             [library newFunctionWithName:@"fs"]);
	if (!pso) return 1;
	MTLTextureDescriptor* td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	                                                       width:64
	                                                      height:32
	                                                   mipmapped:NO];
	td.storageMode            = MTLStorageModeShared;
	td.usage                  = MTLTextureUsageRenderTarget;
	auto                color = [device newTextureWithDescriptor:td];
	MetalRenderPassDesc pass {};
	pass.color_attachment_count            = 1;
	pass.color_attachments[0].texture      = color;
	pass.color_attachments[0].load_action  = MTLLoadActionClear;
	pass.color_attachments[0].store_action = MTLStoreActionStore;
	auto& draw                             = MetalDrawExecutor::Instance();
	draw.BeginRenderPass(pass);
	draw.SetRenderPipeline(pso);
	HW::ScreenViewport screen {};
	screen.screen_scissor_right = screen.window_scissor_right = screen.generic_scissor_right = 64;
	screen.screen_scissor_bottom = screen.window_scissor_bottom = screen.generic_scissor_bottom =
	    32;
	for (auto& v: screen.viewports) {
		v.xscale                  = 16;
		v.xoffset                 = 16;
		v.yscale                  = -16;
		v.yoffset                 = 16;
		v.zscale                  = 1;
		v.zoffset                 = 0;
		v.viewport_scissor_right  = 64;
		v.viewport_scissor_bottom = 32;
	}
	screen.viewports[1].xoffset               = 48;
	screen.viewports[1].viewport_scissor_left = 48;
	HW::ScanModeControl scan {};
	scan.vport_scissor_enable = true;
	HW::ClipControl clip {};
	clip.dx_clip_space = true;
	draw.SetGuestViewports(screen, scan, clip, true);
	draw.Draw(Prospero::PrimitiveType::kPointList, 1, 2);
	draw.EndRenderPass();
	MetalContext::Instance().CommitCommandBuffer(true);
	std::array<uint8_t, 64 * 32 * 4> bytes {};
	[color getBytes:bytes.data()
	    bytesPerRow:64 * 4
	     fromRegion:MTLRegionMake2D(0, 0, 64, 32)
	    mipmapLevel:0];
	unsigned wrong = 0;
	for (unsigned y = 0; y < 32; ++y)
		for (unsigned x = 0; x < 64; ++x) {
			const bool inside = y >= 12 && y < 20 && ((x >= 12 && x < 20) || (x >= 48 && x < 52));
			wrong += (bytes[(y * 64 + x) * 4] != 0) != inside;
		}
	std::printf("Guest 16-viewport/scissor array: 2048 GPU texels, %u failures\n", wrong);
	return wrong;
}
