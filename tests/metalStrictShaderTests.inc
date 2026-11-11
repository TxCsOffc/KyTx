// Real IR loops must finish; no host-selected iteration limit may change their result.
static void LongShaderLoops() {
	unsigned cases = 0;
	for (uint32_t trips: {0u, 8192u, 8193u, 131073u, 262145u}) {
		Fixture    f;
		const auto header = f.Block(), body = f.Block(), done = f.Block();
		const auto buffer = f.Buffer(0);
		const auto memory = f.Memory(0);
		const auto seed   = f.Emit(0, ValueOpcode::LoadBufferU32,
		                           {buffer, Value(0u), Value(28u), Value(0u), Value(true)}, memory);
		f.program.block_info[0].terminator = {.kind       = CFG::TerminatorKind::Branch,
		                                      .true_block = header};
		auto& i =
		    f.program.blocks[header]->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(IR::Type::U32));
		auto& acc =
		    f.program.blocks[header]->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(IR::Type::U32));
		i.AddPhiOperand(f.program.blocks[0], Value(0u));
		acc.AddPhiOperand(f.program.blocks[0], seed);
		f.program.block_info[header].condition =
		    f.Emit(header, ValueOpcode::ULessThan32, {Value(&i), Value(trips)});
		f.program.block_info[header].terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
		                                           .true_block     = body,
		                                           .false_block    = done,
		                                           .merge_block    = done,
		                                           .continue_block = body,
		                                           .loop_header    = true};
		const auto product = f.Emit(body, ValueOpcode::IMul32, {Value(&acc), Value(1664525u)});
		const auto sum     = f.Emit(body, ValueOpcode::IAdd32, {product, Value(1013904223u)});
		const auto nextAcc = f.Emit(body, ValueOpcode::BitwiseXor32, {sum, Value(&i)});
		const auto nextI   = f.Emit(body, ValueOpcode::IAdd32, {Value(&i), Value(1u)});
		i.AddPhiOperand(f.program.blocks[body], nextI);
		acc.AddPhiOperand(f.program.blocks[body], nextAcc);
		f.program.block_info[body].terminator = {.kind       = CFG::TerminatorKind::Branch,
		                                         .true_block = header};
		f.Store(done, buffer, 0, Value(&acc), memory);
		f.Store(done, buffer, 4, Value(&i), memory);
		const auto result = Run(f);
		uint32_t   actual[2];
		std::memcpy(actual, result.data(), 8);
		uint32_t expected = 0xcdcdcdcdu;
		for (uint32_t n = 0; n < trips; ++n)
			expected = (expected * 1664525u + 1013904223u) ^ n;
		if (actual[0] != expected || actual[1] != trips)
			std::fprintf(stderr, "loop trips=%u: got %08x/%u expected %08x/%u\n", trips, actual[0],
			             actual[1], expected, trips);
		Check(actual[0] == expected && actual[1] == trips,
		      "shader was truncated by an artificial loop cap");
		for (size_t n = 8; n < result.size(); ++n)
			Check(result[n] == 0xcd, "long shader loop damaged its buffer guard");
		++cases;
	}
	std::printf("Untruncated shader loops: %u GPU cases PASS (%s)\n", cases,
	            std::getenv("KYTY_MSL_DISPATCHER") ? "dispatcher" : "structured");
}

static void RejectedShaderApproximations() {
	Fixture f;
	f.program.stage = ShaderType::Vertex;
	f.program.info.outputs.push_back({.kind = IR::StageOutputKind::CullDistance});
	auto result = ShaderRecompiler::Msl::EmitProgram(f.program, {});
	Check(!result.success, "cull distances were silently accepted and dropped");
	Fixture lds;
	lds.program.stage = ShaderType::Vertex;
	lds.program.memory_info.push_back({.kind = IR::ResourceKind::Lds});
	result = ShaderRecompiler::Msl::EmitProgram(lds.program, {});
	Check(!result.success, "graphics LDS was replaced with private scratch");
	std::puts("Unsupported cull-distance/graphics-LDS stages explicitly rejected");
}

static void VertexAuxiliaryOutputs() {
	unsigned checked = 0, wrong = 0;
	for (bool layered: {false, true}) {
		Fixture f(0);
		f.program.stage        = ShaderType::Vertex;
		f.program.info.outputs = {{.kind = IR::StageOutputKind::Position},
		                          {.kind = IR::StageOutputKind::PointSize}};
		if (layered) {
			f.program.info.outputs.push_back({.kind = IR::StageOutputKind::Layer});
			f.program.info.outputs.push_back({.kind = IR::StageOutputKind::ViewportIndex});
		}
		ShaderVertexInputInfo vi {};
		vi.pa_cl_vs_out_cntl = (1u << 21) | (1u << 16) | (layered ? ((1u << 18) | (1u << 19)) : 0);
		auto iid      = f.Emit(0, ValueOpcode::GetBuiltin,
		                       {Value(uint32_t(IR::StageInputKind::InstanceIndex)), Value(0u)});
		auto layer    = f.Emit(0, ValueOpcode::IAdd32, {iid, Value(1u)});
		auto viewport = f.Emit(0, ValueOpcode::ShiftLeftLogical32, {iid, Value(16u)});
		auto packed   = f.Emit(0, ValueOpcode::BitwiseOr32, {layer, viewport});
		packed =
		    f.Emit(0, ValueOpcode::BitwiseOr32,
			       {packed, Value(0x8000f800u)}); // unused bits must not leak into either index
		auto position = f.Emit(0, ValueOpcode::CompositeConstructU32x4,
		                       {Value(0u), Value(0u), Value(std::bit_cast<uint32_t>(.5f)),
		                        Value(std::bit_cast<uint32_t>(1.f))});
		auto misc     = f.Emit(0, ValueOpcode::CompositeConstructU32x4,
		                       {Value(std::bit_cast<uint32_t>(8.f)), Value(0u), packed, Value(0u)});
		f.program.export_info.push_back(
		    {.kind = IR::ExportTargetKind::Position, .index = 0, .en = 15});
		f.Emit(0, ValueOpcode::SetAttribute, {position, Value(true)}, 0);
		f.program.export_info.push_back(
		    {.kind = IR::ExportTargetKind::Position, .index = 1, .en = layered ? 5u : 1u});
		f.Emit(0, ValueOpcode::SetAttribute, {misc, Value(true)}, 1);
		auto           vs    = Compile(f, nullptr, &vi);
		NSError*       error = nil;
		id<MTLLibrary> lib =
		    [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;fragment "
			                             @"float4 white(){return float4(1);}"
		                         options:nil
		                           error:&error];
		Check(lib != nil, "auxiliary output fragment compile");
		MTLRenderPipelineDescriptor* pd     = [MTLRenderPipelineDescriptor new];
		pd.vertexFunction                   = vs;
		pd.fragmentFunction                 = [lib newFunctionWithName:@"white"];
		pd.colorAttachments[0].pixelFormat  = MTLPixelFormatRGBA8Unorm;
		pd.inputPrimitiveTopology           = MTLPrimitiveTopologyClassPoint;
		id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd
		                                                                             error:&error];
		if (!pipeline) std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
		Check(pipeline != nil, "auxiliary output pipeline");
		MTLTextureDescriptor* td =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		                                                       width:64
		                                                      height:32
		                                                   mipmapped:NO];
		td.textureType       = MTLTextureType2DArray;
		td.arrayLength       = 4;
		td.storageMode       = MTLStorageModeShared;
		td.usage             = MTLTextureUsageRenderTarget;
		id<MTLTexture> color = [device newTextureWithDescriptor:td];
		Check(color != nil, "auxiliary output target");
		MTLRenderPassDescriptor* pass        = [MTLRenderPassDescriptor new];
		pass.renderTargetArrayLength         = 4;
		pass.colorAttachments[0].texture     = color;
		pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLCommandBuffer>        cb       = [[device newCommandQueue] commandBuffer];
		id<MTLRenderCommandEncoder> enc      = [cb renderCommandEncoderWithDescriptor:pass];
		[enc setRenderPipelineState:pipeline];
		uint32_t zero[ShaderRecompiler::Msl::SizeConstantWordCount] {};
		[enc setVertexBytes:zero length:128 atIndex:0];
		[enc setVertexBytes:zero length:128 atIndex:1];
		[enc setVertexBytes:zero length:sizeof(zero) atIndex:25];
		[enc setVertexBytes:zero length:16 atIndex:26];
		const MTLViewport    vp[2] = {{0, 0, 32, 32, 0, 1}, {32, 0, 32, 32, 0, 1}};
		const MTLScissorRect sc[2] = {{0, 0, 32, 32}, {32, 0, 32, 32}};
		[enc setViewports:vp count:2];
		[enc setScissorRects:sc count:2];
		[enc drawPrimitives:MTLPrimitiveTypePoint
		        vertexStart:0
		        vertexCount:1
		      instanceCount:layered ? 2 : 1];
		[enc endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		Check(cb.status == MTLCommandBufferStatusCompleted, "auxiliary output GPU draw");
		std::array<uint8_t, 64 * 32 * 4> bytes {};
		for (unsigned slice = 0; slice < 4; ++slice) {
			[color getBytes:bytes.data()
			      bytesPerRow:64 * 4
			    bytesPerImage:bytes.size()
			       fromRegion:MTLRegionMake2D(0, 0, 64, 32)
			      mipmapLevel:0
			            slice:slice];
			for (unsigned y = 0; y < 32; ++y)
				for (unsigned x = 0; x < 64; ++x) {
					bool wanted = false;
					if (layered && slice == 1) wanted = x >= 12 && x < 20 && y >= 12 && y < 20;
					if (layered && slice == 2) wanted = x >= 44 && x < 52 && y >= 12 && y < 20;
					if (!layered && slice == 0) wanted = x >= 12 && x < 20 && y >= 12 && y < 20;
					wrong += (bytes[(y * 64 + x) * 4] != 0) != wanted;
					++checked;
				}
		}
	}
	std::printf("Native point size/layer/viewport output: %u GPU texels, %u failures\n", checked,
	            wrong);
	Check(wrong == 0, "auxiliary position outputs dropped or packed fields decoded incorrectly");
}

static void NullDescriptorOperations() {
	using Dim      = ShaderRecompiler::Decoder::ImageDimension;
	unsigned cases = 0;
	for (auto dim: {Dim::Dim1D, Dim::Dim1DArray, Dim::Dim2D, Dim::Dim2DArray, Dim::Dim3D})
		for (auto numeric:
		     {Prospero::TextureNumericClass::Float, Prospero::TextureNumericClass::Uint,
		      Prospero::TextureNumericClass::Sint})
			for (bool storage: {false, true}) {
				Fixture           f;
				IR::ImageResource image {};
				image.dimension     = dim;
				image.numeric_class = numeric;
				image.resource_class =
				    storage ? IR::ImageResourceClass::Storage : IR::ImageResourceClass::Sampled;
				image.read      = true;
				image.written   = storage;
				image.mip_mode  = IR::ImageMipMode::Dynamic;
				image.mip_count = 1;
				f.program.info.images.push_back(image);
				auto binding = IR::DescriptorBindingForImage(image);
				if (!binding) {
					Check(storage && numeric == Prospero::TextureNumericClass::Sint,
					      "unexpected null descriptor binding class");
					continue;
				}
				f.program.bindings.descriptors.push_back({*binding, {0}});
				const auto output       = f.Buffer(0);
				const auto outputMemory = f.Memory(0);
				const auto handle  = f.Emit(0, ValueOpcode::GetImageResource,
				                            {Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
				                             Value(0u), Value(0u), Value(0u)});
				const auto address = f.Emit(0, ValueOpcode::MakeImageAddress,
				                            {Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
				                             Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
				                             Value(0u), Value(0u), Value(0u)});
				const auto imageMemory = f.program.memory_info.size();
				IR::MemoryInfo mem {};
				mem.kind                     = IR::ResourceKind::Image;
				mem.image_dimension          = dim;
				mem.dmask                    = 15;
				mem.image_address_components = 4;
				f.program.memory_info.push_back(mem);
				const auto texel =
				    f.Emit(0, ValueOpcode::ImageRead, {handle, address, Value(true)}, imageMemory);
				const auto dimensions =
				    f.Emit(0, ValueOpcode::ImageQueryDimensions, {handle, address}, imageMemory);
				if (storage)
					f.Emit(0, ValueOpcode::ImageWrite, {handle, address, texel, Value(true)},
					       imageMemory);
				for (unsigned n = 0; n < 4; ++n) {
					f.Store(0, output, n * 4,
					        f.Emit(0, ValueOpcode::CompositeExtractU32x4, {texel, Value(n)}),
					        outputMemory);
					f.Store(0, output, 16 + n * 4,
					        f.Emit(0, ValueOpcode::CompositeExtractU32x4, {dimensions, Value(n)}),
					        outputMemory);
				}
				const auto bytes = Run(f);
				for (uint8_t byte: bytes)
					Check(byte == 0,
					      "null descriptor read/query returned fabricated dimensions or data");
				++cases;
			}
	std::printf("Null handles without substitute textures: %u GPU read/resinfo/store cases PASS\n",
	            cases);
}
