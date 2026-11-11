// Both the native depth state and resource feedback decision must honor Z_READ_ONLY.
static unsigned ReadOnlyDepth(id<MTLDevice>                                device,
                              Libs::Graphics::Metal::MetalPipelineManager& manager,
                              id<MTLFunction> vertex, id<MTLFunction> fragment) {
	using namespace Libs::Graphics;
	using namespace Metal;
	unsigned            failures = 0, cases = 0;
	id<MTLCommandQueue> queue = [device newCommandQueue];
	for (auto format: {MTLPixelFormatDepth16Unorm, MTLPixelFormatDepth32Float}) {
		MetalRenderPipelineKey key {};
		key.vs_hash        = 0xd001;
		key.fs_hash        = 0xd002;
		key.depth_format   = format;
		key.primitive_type = uint32_t(Prospero::PrimitiveType::kTriList);
		id<MTLRenderPipelineState> pipeline =
		    manager.GetOrCreateRenderPipeline(key, vertex, fragment);
		if (!pipeline) return 1;
		for (unsigned function = 0; function < 8; ++function)
			for (bool enabled: {false, true})
				for (bool write: {false, true})
					for (bool readOnly: {false, true}) {
						HW::Context      guest {};
						HW::DepthControl control {};
						control.z_enable       = enabled;
						control.z_write_enable = write;
						control.zfunc          = function;
						guest.SetDepthControl(control);
						HW::DepthRenderTarget target {};
						target.depth_view.depth_write_disable = readOnly;
						guest.SetDepthRenderTarget(target);
						const bool wantedWrite = enabled && write && !readOnly;
						failures += GuestDepthWritesEnabled(guest) != wantedWrite;
						const auto resolved = std::getenv("KYTY_DEPTH_TEST_LEGACY")
						                          ? control
						                          : ResolveGuestDepthControl(guest);
						auto       state    = manager.GetOrCreateDepthStencilState(
						    resolved, guest.GetStencilControl(), guest.GetStencilMask());
						MTLTextureDescriptor* td =
						    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
						                                                       width:8
						                                                      height:8
						                                                   mipmapped:NO];
						td.storageMode                   = MTLStorageModePrivate;
						td.usage                         = MTLTextureUsageRenderTarget;
						id<MTLTexture>           depth   = [device newTextureWithDescriptor:td];
						MTLRenderPassDescriptor* pass    = [MTLRenderPassDescriptor new];
						pass.depthAttachment.texture     = depth;
						pass.depthAttachment.clearDepth  = .5;
						pass.depthAttachment.loadAction  = MTLLoadActionClear;
						pass.depthAttachment.storeAction = MTLStoreActionStore;
						id<MTLBuffer> bytes =
						    [device newBufferWithLength:8 * 256
						                        options:MTLResourceStorageModeShared];
						id<MTLCommandBuffer>        command = [queue commandBuffer];
						id<MTLRenderCommandEncoder> encoder =
						    [command renderCommandEncoderWithDescriptor:pass];
						[encoder setRenderPipelineState:pipeline];
						[encoder setDepthStencilState:state];
						[encoder drawPrimitives:MTLPrimitiveTypeTriangle
						            vertexStart:0
						            vertexCount:3];
						[encoder endEncoding];
						id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
						[blit copyFromTexture:depth
						                 sourceSlice:0
						                 sourceLevel:0
						                sourceOrigin:MTLOriginMake(0, 0, 0)
						                  sourceSize:MTLSizeMake(8, 8, 1)
						                    toBuffer:bytes
						           destinationOffset:0
						      destinationBytesPerRow:256
						    destinationBytesPerImage:8 * 256];
						[blit endEncoding];
						[command commit];
						[command waitUntilCompleted];
						if (command.status != MTLCommandBufferStatusCompleted) {
							++failures;
							continue;
						}
						const bool  passes = !enabled || function == 1 || function == 3 ||
						                     function == 5 || function == 7;
						const float wanted = wantedWrite && passes ? .25f : .5f;
						float       actual = 0;
						if (format == MTLPixelFormatDepth16Unorm) {
							uint16_t v;
							std::memcpy(
							    &v, static_cast<const uint8_t*>(bytes.contents) + 4 * 256 + 4 * 2,
							    2);
							actual = float(v) / 65535.f;
						} else
							std::memcpy(
							    &actual,
							    static_cast<const uint8_t*>(bytes.contents) + 4 * 256 + 4 * 4, 4);
						if (std::fabs(actual - wanted) > 1.f / 65535.f) {
							++failures;
							std::fprintf(stderr,
							             "read-only depth fmt=%lu func=%u enable=%d write=%d ro=%d "
							             "expected=%g got=%g\n",
							             (unsigned long)format, function, enabled, write, readOnly,
							             wanted, actual);
						}
						++cases;
					}
	}
	std::printf("Guest read-only depth: %u GPU D16/D32 cases, %u failures\n", cases, failures);
	return failures;
}
