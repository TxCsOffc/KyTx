// Rectangle assembly is checked independently of the older vertex wrapper:
// all six input orders, smooth interpolation, raw flat payload, culling of both
// output triangles, and wave64's high half use the native NGG mesh entry point.
static void NativeMeshRectangles() {
	unsigned cases = 0, wrong = 0, checked = 0;
	for (uint32_t wave: {32u, 64u})
		for (bool last: {false, true}) {
			Fixture f;
			f.program.stage     = ShaderType::Mesh;
			f.program.wave_size = wave;
			ShaderVertexInputInfo vi {};
			auto&                 m = vi.mesh;
			m.native_execution = m.ngg_vertex = true;
			m.wave_size                       = wave;
			m.host_subgroup_size              = 32;
			m.threads_num[0]                  = 64;
			m.threads_num[1] = m.threads_num[2] = 1;
			m.max_vertices                      = 64;
			m.max_primitives                    = 1;
			m.primitives_per_group              = 1;
			m.vertices_per_group                = 3;
			m.provoking_vertex                  = last ? 2 : 0;
			m.input_primitive                   = uint32_t(Prospero::PrimitiveType::kRectList);
			m.flat_parameter_mask               = 2;
			f.program.info.outputs = {{.kind = IR::StageOutputKind::Position},
			                          {.kind = IR::StageOutputKind::Parameter, .index = 0},
			                          {.kind = IR::StageOutputKind::Parameter, .index = 1}};
			auto handle            = f.Buffer(0);
			auto memory            = f.Memory(0);
			auto local =
			    f.Emit(0, ValueOpcode::GetBuiltin,
				       {Value(uint32_t(IR::StageInputKind::LocalInvocationIndex)), Value(0u)});
			auto offset = f.Emit(0, ValueOpcode::IMul32, {local, Value(48u)});
			for (unsigned n = 0; n < 3; ++n) {
				auto at   = f.Emit(0, ValueOpcode::IAdd32, {offset, Value(n * 16u)});
				auto data = f.Emit(0, ValueOpcode::LoadBufferU32x4,
				                   {handle, Value(0u), at, Value(0u), Value(true)}, memory);
				f.program.export_info.push_back(
				    {.kind  = n ? IR::ExportTargetKind::Parameter : IR::ExportTargetKind::Position,
					 .index = n ? n - 1 : 0,
					 .en    = 15});
				f.Emit(0, ValueOpcode::SetAttribute, {data, Value(true)}, n);
			}
			auto packed = f.Emit(0, ValueOpcode::LoadBufferU32,
			                     {handle, Value(0u), Value(3072u), Value(0u), Value(true)}, memory);
			auto primitive = f.Emit(0, ValueOpcode::CompositeConstructU32x4,
			                        {packed, Value(0u), Value(0u), Value(0u)});
			f.program.export_info.push_back({.kind = IR::ExportTargetKind::Primitive, .en = 1});
			f.Emit(0, ValueOpcode::SetAttribute, {primitive, Value(true)}, 3);
			f.Emit(0, ValueOpcode::MeshAllocate, {Value(35u | (1u << 12))});
			auto     ms    = Compile(f, nullptr, &vi);
			NSError* error = nil;
			auto     lib =
			    [device newLibraryWithSource:
			                @"#include <metal_stdlib>\nusing namespace metal;struct I{float4 uv "
							@"[[user(locn0)]];float4 payload [[user(locn1),flat]];};struct "
							@"O{float4 color [[color(0)]];uint4 bits [[color(1)]];};fragment O "
							@"color(I i [[stage_in]]){return O{i.uv,as_type<uint4>(i.payload)};}"
			                         options:nil
			                           error:&error];
			Check(lib != nil, "NGG rectangle fragment");
			auto pd                            = [MTLMeshRenderPipelineDescriptor new];
			pd.meshFunction                    = ms;
			pd.fragmentFunction                = [lib newFunctionWithName:@"color"];
			pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
			pd.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA32Uint;
			auto pipeline = [device newRenderPipelineStateWithMeshDescriptor:pd
			                                                         options:MTLPipelineOptionNone
			                                                      reflection:nil
			                                                           error:&error];
			if (!pipeline) std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
			Check(pipeline != nil, "NGG rectangle pipeline");
			const unsigned permutations[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2},
			                                     {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
			for (auto& permutation: permutations) {
				unsigned cull_hits[2] {};
				for (unsigned mode = 0; mode < 4; ++mode) {
					std::array<uint32_t, 769> data {};
					const float xy[3][2] = {{-.75f, -.5f}, {.75f, -.5f}, {-.75f, .5f}};
					for (unsigned i = 0; i < 3; ++i) {
						auto  p         = permutation[i];
						float values[8] = {
						    xy[p][0],         xy[p][1], .5f, 1, (xy[p][0] + .75f) / 1.5f,
						    (xy[p][1] + .5f), 0,        1};
						std::memcpy(data.data() + (32 + i) * 12, values, sizeof(values));
						const uint32_t bits[4] = {i + 1u, 0x80000000u, 0x7fc00001u + i,
						                          0xdeadbeefu - i};
						std::memcpy(data.data() + (32 + i) * 12 + 8, bits, sizeof(bits));
					}
					data[768]         = mode == 3 ? 0xffffffffu : 32u | (33u << 10) | (34u << 20);
					auto     buffer   = [device newBufferWithBytes:data.data()
					                                        length:sizeof(data)
					                                       options:MTLResourceStorageModeShared];
					uint64_t args[64] = {buffer.gpuAddress};
					uint32_t sizes[ShaderRecompiler::Msl::SizeConstantWordCount] {};
					sizes[0]       = uint32_t(sizeof(data));
					auto td        = [MTLTextureDescriptor
					    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
					                                 width:64
					                                height:32
					                             mipmapped:NO];
					td.storageMode = MTLStorageModeShared;
					td.usage       = MTLTextureUsageRenderTarget;
					auto target    = [device newTextureWithDescriptor:td];
					td.pixelFormat = MTLPixelFormatRGBA32Uint;
					auto payload   = [device newTextureWithDescriptor:td];
					auto pass      = [MTLRenderPassDescriptor new];
					for (unsigned n = 0; n < 2; ++n) {
						pass.colorAttachments[n].texture     = n ? payload : target;
						pass.colorAttachments[n].loadAction  = MTLLoadActionClear;
						pass.colorAttachments[n].clearColor  = MTLClearColorMake(0, 0, 0, 0);
						pass.colorAttachments[n].storeAction = MTLStoreActionStore;
					}
					auto cb  = [[device newCommandQueue] commandBuffer];
					auto enc = [cb renderCommandEncoderWithDescriptor:pass];
					[enc setRenderPipelineState:pipeline];
					[enc setCullMode:mode == 1   ? MTLCullModeFront
					                 : mode == 2 ? MTLCullModeBack
					                             : MTLCullModeNone];
					[enc setMeshBytes:args length:sizeof(args) atIndex:0];
					[enc setMeshBytes:sizes length:128 atIndex:1];
					[enc setMeshBytes:sizes length:sizeof(sizes) atIndex:25];
					[enc setMeshBytes:sizes length:32 atIndex:26];
					[enc useResource:buffer usage:MTLResourceUsageRead stages:MTLRenderStageMesh];
					[enc drawMeshThreadgroups:MTLSizeMake(1, 1, 1)
					    threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
					      threadsPerMeshThreadgroup:MTLSizeMake(wave == 64 ? 32 : 64, 1, 1)];
					[enc endEncoding];
					[cb commit];
					[cb waitUntilCompleted];
					if (cb.error)
						std::fprintf(stderr, "%s\n", cb.error.localizedDescription.UTF8String);
					Check(cb.status == MTLCommandBufferStatusCompleted, "NGG rectangle GPU draw");
					std::array<float, 64 * 32 * 4>    pixels {};
					std::array<uint32_t, 64 * 32 * 4> bits {};
					[target getBytes:pixels.data()
					     bytesPerRow:64 * 16
					      fromRegion:MTLRegionMake2D(0, 0, 64, 32)
					     mipmapLevel:0];
					[payload getBytes:bits.data()
					      bytesPerRow:64 * 16
					       fromRegion:MTLRegionMake2D(0, 0, 64, 32)
					      mipmapLevel:0];
					unsigned covered = 0;
					for (unsigned y = 0; y < 32; ++y)
						for (unsigned x = 0; x < 64; ++x) {
							unsigned at     = (y * 64 + x) * 4;
							bool     inside = x >= 8 && x < 56 && y >= 8 && y < 24;
							bool     lit    = pixels[at + 3] != 0;
							covered += lit;
							if (mode == 0) wrong += lit != inside;
							if (mode == 3) wrong += lit;
							if (lit) {
								wrong += !inside;
								wrong +=
								    std::abs(pixels[at] - (float(x) + .5f - 8.f) / 48.f) > 0.0001f;
								wrong += std::abs(pixels[at + 1] - (24.f - float(y) - .5f) / 16.f) >
								         0.0001f;
								unsigned       chosen      = last ? 2 : 0;
								const uint32_t expected[4] = {chosen + 1, 0x80000000u,
								                              0x7fc00001u + chosen,
								                              0xdeadbeefu - chosen};
								for (unsigned c = 0; c < 4; ++c)
									wrong += bits[at + c] != expected[c];
							}
							++checked;
						}
					if (mode == 1 || mode == 2) cull_hits[mode - 1] = covered;
					++cases;
				}
				// Both triangles must have the same facing; exactly one cull mode
				// leaves the full 48x16 rectangle, the other leaves no samples.
				wrong += !((cull_hits[0] == 768 && cull_hits[1] == 0) ||
				           (cull_hits[1] == 768 && cull_hits[0] == 0));
			}
		}
	std::printf("Native NGG RECTLIST: %u GPU cases, %u texels, %u failures\n", cases, checked,
	            wrong);
	Check(wrong == 0, "NGG rectangle assembly/interpolation/flat payload/cull mismatch");
}
