static unsigned NativeMeshPipeline(id<MTLDevice> device) {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::Metal;
	NSError*  error = nil;
	NSString* source =
	    @"#include <metal_stdlib>\n#include <metal_mesh>\nusing namespace metal;\n"
	     "struct V {float4 position [[position]];}; using "
		 "M=mesh<V,void,64,64,topology::triangle>;\n"
	     "[[mesh]] void ms(M output, device atomic_uint* stats [[buffer(0)]], device const uchar* "
		 "ib [[buffer(24)]], constant uint* d [[buffer(26)]], uint3 g "
		 "[[threadgroup_position_in_grid]], uint t [[thread_index_in_threadgroup]]) {\n"
	     " if(t!=0) return; output.set_primitive_count(0); uint "
		 "v[10]={1u,d[0],d[1],d[2],d[3],d[4],g.x,g.y+d[2],0u,0u};\n"
	     " if(d[3]==2u)v[8]=reinterpret_cast<device const ushort*>(ib)[d[4]]; "
		 "if(d[3]==4u)v[8]=reinterpret_cast<device const uint*>(ib)[d[4]];\n"
	     " for(uint i=0;i<10;++i)atomic_fetch_add_explicit(stats+i,v[i],memory_order_relaxed); }\n"
	     "fragment float4 fs(){return float4(1);}\n"
	     "kernel void producer(device uint* args [[buffer(0)]], constant uint* values "
		 "[[buffer(1)]], uint i [[thread_position_in_grid]]){args[i]=values[i];}\n";
	auto lib = [device newLibraryWithSource:source options:nil error:&error];
	if (!lib) {
		std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
		return 1;
	}
	MetalRenderPipelineKey key {};
	key.vs_hash                       = 0x4d455348;
	key.fs_hash                       = 1;
	key.color_target_count            = 1;
	key.color_targets[0].pixel_format = MTLPixelFormatRGBA8Unorm;
	key.color_targets[0].write_mask   = MTLColorWriteMaskAll;
	auto pipeline                     = MetalPipelineManager::Instance().GetOrCreateRenderPipeline(
	    key, [lib newFunctionWithName:@"ms"], [lib newFunctionWithName:@"fs"]);
	if (!pipeline) return 1;
	auto producer =
	    [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"producer"]
	                                          error:&error];
	if (!producer) return 1;
	auto           stats = [device newBufferWithLength:40 options:MTLResourceStorageModeShared];
	const uint32_t index_words[] = {777, 123, 456, 789, 2, 5, 7};
	auto           indices       = [device newBufferWithBytes:index_words
	                                                   length:sizeof(index_words)
	                                                  options:MTLResourceStorageModeShared];
	auto td  = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	                                                              width:4
	                                                             height:4
	                                                          mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	auto                color = [device newTextureWithDescriptor:td];
	MetalRenderPassDesc pass {};
	pass.color_attachment_count            = 1;
	pass.color_attachments[0].texture      = color;
	pass.color_attachments[0].load_action  = MTLLoadActionClear;
	pass.color_attachments[0].store_action = MTLStoreActionStore;
	ShaderMeshInputInfo mesh {};
	mesh.native_execution = mesh.ngg_vertex = true;
	mesh.input_primitive                    = uint32_t(Prospero::PrimitiveType::kTriList);
	mesh.primitives_per_group               = 2;
	mesh.threads_num[0]                     = 64;
	mesh.threads_num[1] = mesh.threads_num[2] = 1;
	mesh.wave_size                            = 64;
	auto& context                             = MetalContext::Instance();
	auto& draw                                = MetalDrawExecutor::Instance();
	draw.SetMeshState(&mesh);
	unsigned failures = 0, cases = 0;
	for (unsigned kind = 0; kind < 5; ++kind) {
		std::memset(stats.contents, 0, stats.length);
		uint32_t expected[10] {};
		auto     add = [&](uint32_t count, uint32_t instances, uint32_t base, uint32_t first,
		                   uint32_t index_bytes, uint32_t start, uint32_t value) {
			uint32_t groups = count < 3 ? 0 : ((count / 3) + 1) / 2;
			for (uint32_t y = 0; y < instances; ++y)
				for (uint32_t x = 0; x < groups; ++x) {
					uint32_t row[] = {1,     count, base,      first, index_bytes,
					                  start, x,     y + first, value, 0};
					for (unsigned k = 0; k < 10; ++k)
						expected[k] += row[k];
				}
		};
		id<MTLBuffer>     prepared = nil;
		MetalMeshIndirect prepare;
		if (kind >= 3) {
			const bool     indexed = kind == 4;
			uint32_t       values[20] {};
			const uint32_t a[] = {25, 3, 1, 0xfffffffd, 7}, b[] = {8, 2, 2, 11, 5};
			std::memcpy(values + 4, a, indexed ? 20 : 16);
			std::memcpy(values + 12, b, indexed ? 20 : 16);
			auto args = [device newBufferWithLength:sizeof(values)
			                                options:MTLResourceStorageModePrivate];
			auto cb   = context.GetCurrentCommandBuffer();
			auto enc  = [cb computeCommandEncoder];
			[enc setComputePipelineState:producer];
			[enc setBuffer:args offset:0 atIndex:0];
			[enc setBytes:values length:sizeof(values) atIndex:1];
			[enc dispatchThreads:MTLSizeMake(20, 1, 1) threadsPerThreadgroup:MTLSizeMake(20, 1, 1)];
			[enc endEncoding];
			prepared = prepare.Encode(cb, args, 16, 2, 32, indexed, indexed ? 4 : 0, 3, 3, 2);
			if (!prepared) return 1;
			if (indexed) {
				add(25, 3, 0xfffffffd, 7, 4, 1, 123);
				add(8, 2, 11, 5, 4, 2, 456);
			} else {
				add(25, 3, 1, 0xfffffffd, 0, 0, 0);
				add(8, 2, 2, 11, 0, 0, 0);
			}
		} else
			add(19, 2, kind ? uint32_t(-3) : 11, 7,
			    kind == 1   ? 2
			    : kind == 2 ? 4
			                : 0,
			    0, kind ? 123 : 0);
		if (!draw.BeginRenderPass(pass)) return 1;
		draw.SetRenderPipeline(pipeline);
		draw.SetVertexBuffer(stats, 0, 0);
		draw.SetVertexBuffer(indices, kind == 1 ? 4 : kind == 2 ? 4 : 0, 24);
		draw.UseResource(stats, MTLResourceUsageRead | MTLResourceUsageWrite);
		if (kind == 0)
			draw.Draw(Prospero::PrimitiveType::kTriList, 19, 2, 11, 7);
		else if (kind <= 2)
			draw.DrawIndexed(Prospero::PrimitiveType::kTriList, 19, indices, 4,
			                 kind == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32, 2, -3, 7);
		else
			for (unsigned i = 0; i < 2; ++i) {
				draw.UseResource(prepared, MTLResourceUsageRead);
				if (kind == 4)
					draw.DrawIndexedIndirect(Prospero::PrimitiveType::kTriList, MTLIndexTypeUInt32,
					                         indices, 0, prepared, i * 64, i == 1);
				else
					draw.DrawIndirect(Prospero::PrimitiveType::kTriList, prepared, i * 64, i == 1);
			}
		draw.EndRenderPass();
		auto cb = context.GetCurrentCommandBuffer();
		context.CommitCommandBuffer(true);
		if (cb.status != MTLCommandBufferStatusCompleted) {
			std::fprintf(stderr, "%s\n", cb.error.localizedDescription.UTF8String);
			return 1;
		}
		auto actual = (uint32_t*)stats.contents;
		for (unsigned i = 0; i < 10; ++i)
			if (actual[i] != expected[i]) {
				std::fprintf(stderr, "Mesh host kind=%u field=%u got=%u expected=%u\n", kind, i,
				             actual[i], expected[i]);
				++failures;
			}
		++cases;
	}
	draw.SetMeshState(nullptr);
	std::printf("Native mesh pipeline/draw/index/indirect GPU producer: %u cases, %u failures\n",
	            cases, failures);
	return failures;
}
