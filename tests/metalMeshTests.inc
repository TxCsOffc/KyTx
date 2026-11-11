// A real mesh raster test: connectivity, allocation and null-primitive bits come
// from GPU memory. Changing them must change coverage without changing the VS.
static void NativeMeshExports() {
	unsigned cases = 0, checked = 0, wrong = 0;
	for (uint32_t wave: {32u, 64u})
		for (uint32_t vertices: {32u, 64u, 256u}) {
			Fixture f;
			f.program.stage     = ShaderType::Mesh;
			f.program.wave_size = wave;
			ShaderVertexInputInfo vi {};
			auto&                 mesh = vi.mesh;
			mesh.wave_size             = wave;
			mesh.host_subgroup_size    = 32;
			mesh.threads_num[0]        = vertices;
			mesh.threads_num[1] = mesh.threads_num[2] = 1;
			mesh.max_vertices                         = vertices;
			mesh.max_primitives                       = vertices;
			mesh.input_primitive                      = uint32_t(Prospero::PrimitiveType::kTriList);
			mesh.primitives_per_group                 = 4;
			f.program.info.outputs                    = {{.kind = IR::StageOutputKind::Position}};
			auto local =
			    f.Emit(0, ValueOpcode::GetBuiltin,
				       {Value(uint32_t(IR::StageInputKind::LocalInvocationIndex)), Value(0u)});
			auto handle   = f.Buffer(0);
			auto memory   = f.Memory(0);
			auto offset   = f.Emit(0, ValueOpcode::IMul32, {local, Value(16u)});
			auto position = f.Emit(0, ValueOpcode::LoadBufferU32x4,
			                       {handle, Value(0u), offset, Value(0u), Value(true)}, memory);
			f.program.export_info.push_back({.kind = IR::ExportTargetKind::Position, .en = 15});
			f.Emit(0, ValueOpcode::SetAttribute, {position, Value(true)}, 0);
			offset = f.Emit(0, ValueOpcode::IAdd32,
			                {Value(4096u), f.Emit(0, ValueOpcode::IMul32, {local, Value(4u)})});
			auto packed    = f.Emit(0, ValueOpcode::LoadBufferU32,
			                        {handle, Value(0u), offset, Value(0u), Value(true)}, memory);
			auto primitive = f.Emit(0, ValueOpcode::CompositeConstructU32x4,
			                        {packed, Value(0u), Value(0u), Value(0u)});
			f.program.export_info.push_back({.kind = IR::ExportTargetKind::Primitive, .en = 1});
			f.Emit(0, ValueOpcode::SetAttribute, {primitive, Value(true)}, 1);
			auto nv    = f.Emit(0, ValueOpcode::MeshDrawParameter, {Value(0u)});
			auto np    = f.Emit(0, ValueOpcode::MeshDrawParameter, {Value(6u)});
			auto alloc = f.Emit(0, ValueOpcode::BitwiseOr32,
			                    {nv, f.Emit(0, ValueOpcode::ShiftLeftLogical32, {np, Value(12u)})});
			f.Emit(0, ValueOpcode::MeshAllocate, {alloc});
			auto     ms    = Compile(f, nullptr, &vi);
			NSError* error = nil;
			auto     lib   = [device
			    newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; fragment "
				                     @"float4 mesh_color(uint id [[primitive_id]]) {return "
				                     @"float4(float(id+1u)/255.0f,1,1,1);}"
			                 options:nil
			                   error:&error];
			Check(lib != nil, "mesh fragment compile");
			MTLMeshRenderPipelineDescriptor* pd = [MTLMeshRenderPipelineDescriptor new];
			pd.meshFunction                     = ms;
			pd.fragmentFunction                 = [lib newFunctionWithName:@"mesh_color"];
			pd.colorAttachments[0].pixelFormat  = MTLPixelFormatRGBA8Unorm;
			id<MTLRenderPipelineState> pipeline =
			    [device newRenderPipelineStateWithMeshDescriptor:pd
			                                             options:MTLPipelineOptionNone
			                                          reflection:nil
			                                               error:&error];
			if (!pipeline) std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
			Check(pipeline != nil, "native mesh pipeline");
			for (unsigned mode = 0; mode < 6; ++mode) {
				id<MTLBuffer> buffer = [device newBufferWithLength:5120
				                                           options:MTLResourceStorageModeShared];
				std::memset(buffer.contents, 0, 5120);
				const uint32_t base     = vertices == 256 ? 248 : vertices == 64 ? 32 : 0;
				const float    xy[8][2] = {{-1, -1}, {0, -1}, {-1, 1}, {0, 1},
				                           {0, -1},  {1, -1}, {0, 1},  {1, 1}};
				for (unsigned i = 0; i < 8; ++i) {
					float p[4] = {xy[i][0], xy[i][1], 0.5f, 1};
					std::memcpy((char*)buffer.contents + (base + i) * 16, p, 16);
				}
				uint32_t*      prim          = (uint32_t*)((char*)buffer.contents + 4096);
				const uint32_t indices[4][3] = {{0, 1, 2}, {1, 3, 2}, {4, 5, 6}, {5, 7, 6}};
				for (unsigned i = 0; i < 4; ++i) {
					prim[i] = 0;
					for (unsigned c = 0; c < 3; ++c)
						prim[i] |= (base + indices[(mode == 1 && i >= 2) ? i - 2 : i][c])
						           << (c * 10);
					if (mode == 2 && i >= 2) prim[i] = 0xffffffffu;
					if (mode == 5) prim[i] |= (1u << 9) | (1u << 19) | (1u << 29);
				}
				const uint32_t npv     = mode == 3 ? 2 : mode == 4 ? 0 : 4;
				uint32_t       draw[8] = {vertices, 0, 0, 0, 0, 0, npv, 0};
				uint32_t       sizes[ShaderRecompiler::Msl::SizeConstantWordCount] {};
				sizes[0]                             = 5120;
				uint64_t args[64]                    = {buffer.gpuAddress};
				auto     td                          = [MTLTextureDescriptor
				    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				                                 width:64
				                                height:32
				                             mipmapped:NO];
				td.storageMode                       = MTLStorageModeShared;
				td.usage                             = MTLTextureUsageRenderTarget;
				id<MTLTexture> color                 = [device newTextureWithDescriptor:td];
				auto           pass                  = [MTLRenderPassDescriptor new];
				pass.colorAttachments[0].texture     = color;
				pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
				pass.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 0);
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				auto cb                              = [[device newCommandQueue] commandBuffer];
				auto enc                             = [cb renderCommandEncoderWithDescriptor:pass];
				[enc setRenderPipelineState:pipeline];
				[enc setMeshBytes:args length:sizeof(args) atIndex:0];
				[enc setMeshBytes:sizes length:128 atIndex:1];
				[enc setMeshBytes:sizes length:sizeof(sizes) atIndex:25];
				[enc setMeshBytes:draw length:sizeof(draw) atIndex:26];
				[enc useResource:buffer usage:MTLResourceUsageRead stages:MTLRenderStageMesh];
				[enc drawMeshThreadgroups:MTLSizeMake(1, 1, 1)
				    threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
				      threadsPerMeshThreadgroup:MTLSizeMake(wave == 64 ? vertices / 2 : vertices, 1,
					                                        1)];
				[enc endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
				if (cb.error)
					std::fprintf(stderr, "%s\n", cb.error.localizedDescription.UTF8String);
				Check(cb.status == MTLCommandBufferStatusCompleted, "mesh GPU execution");
				std::array<uint8_t, 64 * 32 * 4> pixels {};
				[color getBytes:pixels.data()
				    bytesPerRow:64 * 4
				     fromRegion:MTLRegionMake2D(0, 0, 64, 32)
				    mipmapLevel:0];
				for (unsigned y = 0; y < 32; ++y)
					for (unsigned x = 0; x < 64; ++x) {
						// Triangle diagonal is excluded here; the rasterizer's top-left rule is
						// checked by the full-square coverage rather than inferred from its ID.
						bool lit    = mode != 4 && (mode == 0 || mode == 5 || x < 32);
						bool actual = pixels[(y * 64 + x) * 4 + 1] != 0;
						wrong += actual != lit;
						++checked;
						if (lit && mode != 1 && (x % 32) != y) {
							unsigned p = (x >= 32 ? 2 : 0) + ((x % 32) < y ? 0 : 1);
							wrong += pixels[(y * 64 + x) * 4] != p + 1;
						}
					}
				++cases;
			}
		}
	std::printf("Native NGG mesh: %u GPU cases, %u coverage texels, %u failures\n", cases, checked,
	            wrong);
	Check(wrong == 0, "NGG connectivity/allocation/culling/primitive ID mismatch");
}

static void NativeMeshPrimitiveOutputs() {
	unsigned cases = 0, wrong = 0, checked = 0;
	for (uint32_t wave: {32u, 64u})
		for (bool last: {false, true}) {
			Fixture f(0);
			f.program.stage     = ShaderType::Mesh;
			f.program.wave_size = wave;
			ShaderVertexInputInfo vi {};
			vi.pa_cl_vs_out_cntl = (1u << 21) | (1u << 18) | (1u << 19);
			auto& m              = vi.mesh;
			m.ngg_vertex = m.native_execution = true;
			m.wave_size                       = wave;
			m.host_subgroup_size              = 32;
			m.threads_num[0]                  = 64;
			m.threads_num[1] = m.threads_num[2] = 1;
			m.max_vertices                      = 64;
			m.max_primitives                    = 64;
			m.primitives_per_group              = 1;
			m.input_primitive                   = uint32_t(Prospero::PrimitiveType::kTriList);
			m.provoking_vertex                  = last ? 2 : 0;
			f.program.info.outputs              = {{.kind = IR::StageOutputKind::Position},
			                                       {.kind = IR::StageOutputKind::Layer},
			                                       {.kind = IR::StageOutputKind::ViewportIndex}};
			f.program.info.outputs.push_back({.kind = IR::StageOutputKind::Parameter, .index = 0});
			auto local =
			    f.Emit(0, ValueOpcode::GetBuiltin,
				       {Value(uint32_t(IR::StageInputKind::LocalInvocationIndex)), Value(0u)});
			auto corner = f.Emit(0, ValueOpcode::BitwiseAnd32, {local, Value(31u)});
			auto eq     = [&](unsigned n) {
				return f.Emit(0, ValueOpcode::IEqual32, {corner, Value(n)});
			};
			auto x = f.Emit(
			    0, ValueOpcode::SelectU32,
			    {eq(1), Value(std::bit_cast<uint32_t>(3.f)), Value(std::bit_cast<uint32_t>(-1.f))});
			auto y = f.Emit(
			    0, ValueOpcode::SelectU32,
			    {eq(2), Value(std::bit_cast<uint32_t>(3.f)), Value(std::bit_cast<uint32_t>(-1.f))});
			auto p = f.Emit(
			    0, ValueOpcode::CompositeConstructU32x4,
			    {x, y, Value(std::bit_cast<uint32_t>(.5f)), Value(std::bit_cast<uint32_t>(1.f))});
			f.program.export_info.push_back({.kind = IR::ExportTargetKind::Position, .en = 15});
			f.Emit(0, ValueOpcode::SetAttribute, {p, Value(true)}, 0);
			auto packed =
			    f.Emit(0, ValueOpcode::SelectU32, {eq(2), Value(2u | (1u << 16)), Value(1u)});
			auto aux = f.Emit(0, ValueOpcode::CompositeConstructU32x4,
			                  {Value(0u), Value(0u), packed, Value(0u)});
			f.program.export_info.push_back(
			    {.kind = IR::ExportTargetKind::Position, .index = 1, .en = 4});
			f.Emit(0, ValueOpcode::SetAttribute, {aux, Value(true)}, 1);
			auto connectivity =
			    f.Emit(0, ValueOpcode::CompositeConstructU32x4,
				       {Value(32u | (33u << 10) | (34u << 20)), Value(0u), Value(0u), Value(0u)});
			f.program.export_info.push_back({.kind = IR::ExportTargetKind::Primitive, .en = 1});
			f.Emit(0, ValueOpcode::SetAttribute, {connectivity, Value(true)}, 2);
			f.Emit(0, ValueOpcode::MeshAllocate, {Value(35u | (1u << 12))});
			auto red     = f.Emit(0, ValueOpcode::SelectU32,
			                      {eq(2), Value(0u), Value(std::bit_cast<uint32_t>(1.f))});
			auto green   = f.Emit(0, ValueOpcode::SelectU32,
			                      {eq(2), Value(std::bit_cast<uint32_t>(1.f)), Value(0u)});
			auto varying = f.Emit(0, ValueOpcode::CompositeConstructU32x4,
			                      {red, green, Value(0u), Value(std::bit_cast<uint32_t>(1.f))});
			f.program.export_info.push_back({.kind = IR::ExportTargetKind::Parameter, .en = 15});
			f.Emit(0, ValueOpcode::SetAttribute, {varying, Value(true)}, 3);
			auto     function                  = Compile(f, nullptr, &vi);
			NSError* error                     = nil;
			auto     lib                       = [device
			    newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;struct "
				                     @"I{float4 data [[user(locn0),flat]];};fragment float4 "
				                     @"color(I i [[stage_in]]){return i.data;}"
			                 options:nil
			                   error:&error];
			auto     pd                        = [MTLMeshRenderPipelineDescriptor new];
			pd.meshFunction                    = function;
			pd.fragmentFunction                = [lib newFunctionWithName:@"color"];
			pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
			auto pipeline = [device newRenderPipelineStateWithMeshDescriptor:pd
			                                                         options:MTLPipelineOptionNone
			                                                      reflection:nil
			                                                           error:&error];
			if (!pipeline) std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
			Check(pipeline != nil, "mesh layer/viewport pipeline");
			auto td =
			    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
			                                                       width:64
			                                                      height:32
			                                                   mipmapped:NO];
			td.textureType                       = MTLTextureType2DArray;
			td.arrayLength                       = 4;
			td.storageMode                       = MTLStorageModeShared;
			td.usage                             = MTLTextureUsageRenderTarget;
			auto color                           = [device newTextureWithDescriptor:td];
			auto pass                            = [MTLRenderPassDescriptor new];
			pass.renderTargetArrayLength         = 4;
			pass.colorAttachments[0].texture     = color;
			pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
			pass.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 0);
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			auto cb                              = [[device newCommandQueue] commandBuffer];
			auto enc                             = [cb renderCommandEncoderWithDescriptor:pass];
			[enc setRenderPipelineState:pipeline];
			uint64_t ab[64] {};
			uint32_t zero[ShaderRecompiler::Msl::SizeConstantWordCount] {};
			[enc setMeshBytes:ab length:sizeof(ab) atIndex:0];
			[enc setMeshBytes:zero length:128 atIndex:1];
			[enc setMeshBytes:zero length:sizeof(zero) atIndex:25];
			[enc setMeshBytes:zero length:32 atIndex:26];
			const MTLViewport    vp[2] = {{0, 0, 32, 32, 0, 1}, {32, 0, 32, 32, 0, 1}};
			const MTLScissorRect sc[2] = {{0, 0, 32, 32}, {32, 0, 32, 32}};
			[enc setViewports:vp count:2];
			[enc setScissorRects:sc count:2];
			[enc drawMeshThreadgroups:MTLSizeMake(1, 1, 1)
			    threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
			      threadsPerMeshThreadgroup:MTLSizeMake(wave == 64 ? 32 : 64, 1, 1)];
			[enc endEncoding];
			[cb commit];
			[cb waitUntilCompleted];
			Check(cb.status == MTLCommandBufferStatusCompleted, "mesh layer/viewport GPU");
			std::array<uint8_t, 64 * 32 * 4> pixels {};
			for (unsigned layer = 0; layer < 4; ++layer) {
				[color getBytes:pixels.data()
				      bytesPerRow:64 * 4
				    bytesPerImage:pixels.size()
				       fromRegion:MTLRegionMake2D(0, 0, 64, 32)
				      mipmapLevel:0
				            slice:layer];
				for (unsigned y = 0; y < 32; ++y)
					for (unsigned x = 0; x < 64; ++x) {
						bool lit = layer == (last ? 2 : 1) && (last ? x >= 32 : x < 32);
						wrong += (pixels[(y * 64 + x) * 4 + 3] != 0) != lit;
						if (lit) wrong += pixels[(y * 64 + x) * 4 + (last ? 1 : 0)] != 255;
						++checked;
					}
			}
			++cases;
		}
	std::printf("NGG provoking vertex/layer/viewport: %u GPU cases, %u texels, %u failures\n",
	            cases, checked, wrong);
	Check(wrong == 0, "NGG per-primitive outputs lost or taken from the wrong vertex/half");
}
