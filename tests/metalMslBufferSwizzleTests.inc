// GPU regression for descriptor stride swizzle, not image-format swizzle.
static void BufferStrideSwizzle() {
	unsigned total = 0;
	for (uint32_t mode = 0; mode < 18; ++mode) {
		Fixture f(2);
		const uint32_t variant = mode % 9;
		const uint32_t stride = variant == 8 ? 0u : variant < 4 ? 48u : 47u;
		const uint32_t index_stride = variant & 3u, records = 8u << index_stride;
		f.program.info.buffers[1].packed_stride = stride | (1u << 14u) | (index_stride << 16u);
		f.program.info.buffers[1].descriptor_format = Libs::Graphics::Prospero::BufferFormat::k32_32_32_32UInt;
		f.program.info.buffers[1].descriptor_swizzle = 0xfacu;
		const auto out = f.Buffer(0);
		const auto input = f.Emit(0, ValueOpcode::GetBufferResource, {Value(1u),Value(0u),Value(0u),Value(0u)});
		const auto out_mem = f.Memory(0), in_mem = f.Memory(1, mode >= 9), raw_mem = f.Memory(1);
		f.program.memory_info[in_mem].offset = 12;
		f.program.memory_info[raw_mem].offset = 12;
		constexpr uint32_t input_size = 32768;
		std::vector<uint32_t> expected_memory(input_size / 4);
		for (uint32_t i = 0; i < expected_memory.size(); ++i) expected_memory[i] = 0x12340000u ^ (i * 0x10203u);
		const auto initial = expected_memory;
		std::vector<uint32_t> expected;
		// Independent record/word layout reference. Scalar offset is outside the
		// interleaved record block; vector components are separate swizzled words.
		auto address = [&](uint32_t index, uint32_t byte, uint32_t scalar) {
			if (stride == 0) return byte + scalar;
			return ((index / records) * stride + (byte / 4) * 4) * records + (index % records) * 4 + byte % 4 + scalar;
		};
		for (uint32_t index: {0u, 7u, 8u, 15u, 16u, 31u, 32u, 63u, 64u, 65u, 0x10000u}) {
			for (uint32_t offset: {0u, 4u, 12u}) {
				const auto load = [&](ValueOpcode opcode, uint32_t component_count) {
					const auto value = f.Emit(0, opcode, {input,Value(index),Value(offset),Value(8u),Value(true)}, in_mem);
					for (uint32_t c = 0; c < component_count; ++c) {
						const auto item = component_count == 1 ? value : f.Emit(0, ValueOpcode::CompositeExtractU32x4, {value,Value(c)});
						f.Store(0, out, uint32_t(expected.size() * 4), item, out_mem);
						const uint32_t a = address(index, offset + 12 + c * 4, 8);
						expected.push_back(a < input_size ? expected_memory[a / 4] : 0u);
					}
				};
				load(ValueOpcode::LoadBufferU32x4, 4);
				const uint32_t base = 0xace00000u + uint32_t(expected.size());
				const auto words = f.Emit(0, ValueOpcode::CompositeConstructU32x4, {Value(base),Value(base+1),Value(base+2),Value(base+3)});
				f.Emit(0, ValueOpcode::StoreBufferU32x4, {input,Value(index),Value(offset),Value(8u),words,Value(true)}, in_mem);
				for (uint32_t c = 0; c < 4; ++c) {
					const uint32_t a = address(index, offset + 12 + c * 4, 8);
					if (a < input_size) expected_memory[a / 4] = base + c;
				}
				load(ValueOpcode::LoadBufferU32x4, 4);
			}
		}
		// Atomics and byte/halfword loads use the same address translation.
		const uint32_t atomic_addr = address(9, 12, 8);
		const auto old = f.Emit(0, ValueOpcode::BufferAtomicIAdd32, {input,Value(9u),Value(0u),Value(8u),Value(7u),Value(true)}, raw_mem);
		f.Store(0, out, uint32_t(expected.size() * 4), old, out_mem);
		expected.push_back(expected_memory[atomic_addr / 4]); expected_memory[atomic_addr / 4] += 7;
		for (uint32_t offset: {1u, 2u}) {
			const auto value = f.Emit(0, offset == 1 ? ValueOpcode::LoadBufferU8 : ValueOpcode::LoadBufferU16,
			    {input,Value(9u),Value(offset),Value(8u),Value(true)}, raw_mem);
			f.Store(0, out, uint32_t(expected.size() * 4), value, out_mem);
			const uint32_t a = address(9, offset + 12, 8);
			uint32_t bits = expected_memory[a / 4] >> ((a % 4) * 8);
			expected.push_back(bits & (offset == 1 ? 0xffu : 0xffffu));
		}
		id<MTLFunction> fn = Compile(f);
		NSError* error = nil;
		id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:fn error:&error];
		Check(pso != nil, "stride-swizzle pipeline failed");
		id<MTLBuffer> output = [device newBufferWithLength:expected.size()*4 options:MTLResourceStorageModeShared];
		id<MTLBuffer> data = [device newBufferWithBytes:initial.data() length:input_size options:MTLResourceStorageModeShared];
		id<MTLArgumentEncoder> args = [fn newArgumentEncoderWithBufferIndex:0];
		id<MTLBuffer> table = [device newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];
		[args setArgumentBuffer:table offset:0]; [args setBuffer:output offset:0 atIndex:0]; [args setBuffer:data offset:0 atIndex:1];
		uint32_t sizes[64] {uint32_t(output.length), input_size}, push[32] {};
		id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
		[e setComputePipelineState:pso]; [e setBuffer:table offset:0 atIndex:0]; [e setBytes:push length:sizeof(push) atIndex:1];
		[e setBytes:sizes length:sizeof(sizes) atIndex:25]; [e useResource:output usage:MTLResourceUsageWrite];
		[e useResource:data usage:MTLResourceUsageRead|MTLResourceUsageWrite];
		[e dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
		Check(cb.status == MTLCommandBufferStatusCompleted, "stride-swizzle GPU execution failed");
		Check(std::memcmp(output.contents, expected.data(), expected.size()*4) == 0, "stride-swizzle vector/subword/atomic load mismatch");
		Check(std::memcmp(data.contents, expected_memory.data(), input_size) == 0, "stride-swizzle store/atomic modified wrong physical bytes");
		total += expected.size();
	}
	std::printf("BufferStrideSwizzle: %u GPU results and full buffer checks passed (4 index strides, odd/zero stride, raw/formatted vectors, subwords, atomics, OOB)\n", total);
}
