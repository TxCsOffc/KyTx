static void PixelLaneStorage() {
	unsigned cases = 0, checked = 0;
	for (uint32_t wave: {32u, 64u}) for (uint32_t loop_case = 0; loop_case < 4; ++loop_case) {
		Fixture f(0);
		f.program.stage        = ShaderType::Pixel;
		f.program.wave_size    = wave;
		f.program.info.inputs  = {{.kind = IR::StageInputKind::FragCoord}};
		f.program.info.outputs = {{.kind = IR::StageOutputKind::Mrt}};
		ShaderPixelInputInfo pi {};
		pi.wave_size             = wave;
		pi.extra_lds_size_dwords = 128;
		pi.target_integer[0]     = 1;
		f.program.memory_info.push_back({.kind = IR::ResourceKind::Lds});
		f.program.memory_info.push_back({.kind = IR::ResourceKind::Lds, .offset = 256});
		auto xbits = f.Emit(0, ValueOpcode::GetBuiltin,
		                    {Value(uint32_t(IR::StageInputKind::FragCoord)), Value(0u)});
		auto x =
		    f.Emit(0, ValueOpcode::ConvertU32F32, {f.Emit(0, ValueOpcode::BitCastF32U32, {xbits})});
		auto lane    = f.Emit(0, ValueOpcode::LaneId, {});
		auto address = f.Emit(0, ValueOpcode::ShiftLeftLogical32, {lane, Value(2u)});
		auto value =
		    f.Emit(0, ValueOpcode::BitwiseXor32,
			       {f.Emit(0, ValueOpcode::IMul32, {x, Value(0x9e3779b9u)}), Value(0xdeadbeefu)});
		auto odd    = f.Emit(0, ValueOpcode::INotEqual32,
		                     {f.Emit(0, ValueOpcode::BitwiseAnd32, {x, Value(1u)}), Value(0u)});
		auto second = f.Emit(0, ValueOpcode::INotEqual32,
		                     {f.Emit(0, ValueOpcode::BitwiseAnd32, {x, Value(2u)}), Value(0u)});
		auto subset = f.Emit(0, ValueOpcode::LogicalAnd, {odd, second});
		f.Emit(0, ValueOpcode::WriteSharedU32, {address, value, Value(true)}, 0);
		f.Emit(0, ValueOpcode::WriteSharedU32, {address, xbits, odd}, 1);
		uint32_t read_block = 0;
		Value read_predicate = subset;
		if (loop_case != 0) {
			const auto header = f.Block(), body = f.Block(), done = f.Block();
			f.program.blocks[0]->AddBranch(f.program.blocks[header]);
			f.program.blocks[header]->AddBranch(f.program.blocks[body]);
			f.program.blocks[header]->AddBranch(f.program.blocks[done]);
			f.program.blocks[body]->AddBranch(f.program.blocks[header]);
			f.program.block_info[0].terminator = {.kind=CFG::TerminatorKind::Branch,.true_block=header};
			auto& mask = f.program.blocks[header]->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(IR::Type::U1));
			mask.AddPhiOperand(f.program.blocks[0], loop_case == 3 ? f.Emit(0,ValueOpcode::UndefU1,{}) : loop_case == 2 ? f.Emit(0,ValueOpcode::LogicalNot,{odd}) : subset);
			mask.AddPhiOperand(f.program.blocks[body], Value(&mask));
			auto& count = f.program.blocks[header]->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(IR::Type::U32));
			count.AddPhiOperand(f.program.blocks[0],Value(0u));
			count.AddPhiOperand(f.program.blocks[body],f.Emit(body,ValueOpcode::IAdd32,{Value(&count),Value(1u)}));
			f.program.block_info[header].condition=f.Emit(header,ValueOpcode::ULessThan32,{Value(&count),Value(3u)});
			f.program.block_info[header].terminator={.kind=CFG::TerminatorKind::ConditionalBranch,.true_block=body,.false_block=done,.merge_block=done,.continue_block=body,.loop_header=true};
			f.program.block_info[body].terminator={.kind=CFG::TerminatorKind::Branch,.true_block=header};
			auto& merged = f.program.blocks[done]->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(IR::Type::U1));
			merged.AddPhiOperand(f.program.blocks[header],Value(&mask));
			read_block=done;read_predicate=Value(&merged);
		}
		auto a = f.Emit(read_block, ValueOpcode::LoadSharedU32, {address, Value(true)}, 0);
		auto b = f.Emit(read_block, ValueOpcode::LoadSharedU32, {address, read_predicate}, 1);
		auto expected = f.Emit(read_block, ValueOpcode::SelectU32, {read_predicate, xbits, Value(0u)});
		auto output = f.Emit(read_block, ValueOpcode::CompositeConstructU32x4, {a, value, b, expected});
		f.program.export_info.push_back({.kind = IR::ExportTargetKind::Mrt, .en = 15});
		f.Emit(read_block, ValueOpcode::SetAttribute, {output, Value(true)}, 0);
		if (loop_case >= 2) {
			ShaderStageInputInfo stage{}; stage.pixel=&pi;
			Check(!ShaderRecompiler::Msl::EmitProgram(f.program,stage).success,
			      "changing/uninitialized loop predicate was accepted for pixel LDS");
			++cases;continue;
		}
		auto     ps    = Compile(f, &pi);
		NSError* error = nil;
		auto     lib =
		    [device newLibraryWithSource:
		                @"#include <metal_stdlib>\nusing namespace metal;vertex float4 vs(uint i "
						@"[[vertex_id]]){const float2 "
						@"p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);}"
		                         options:nil
		                           error:&error];
		Check(lib != nil, "pixel lane vertex");
		auto pd                            = [MTLRenderPipelineDescriptor new];
		pd.vertexFunction                  = [lib newFunctionWithName:@"vs"];
		pd.fragmentFunction                = ps;
		pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Uint;
		auto pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
		if (!pipeline) std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
		Check(pipeline != nil, "pixel lane pipeline");
		auto td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
		                                                             width:64
		                                                            height:8
		                                                         mipmapped:NO];
		td.storageMode                       = MTLStorageModeShared;
		td.usage                             = MTLTextureUsageRenderTarget;
		auto color                           = [device newTextureWithDescriptor:td];
		auto pass                            = [MTLRenderPassDescriptor new];
		pass.colorAttachments[0].texture     = color;
		pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		auto cb                              = [[device newCommandQueue] commandBuffer];
		auto enc                             = [cb renderCommandEncoderWithDescriptor:pass];
		[enc setRenderPipelineState:pipeline];
		uint64_t zero[256] {};
		[enc setFragmentBytes:zero length:512 atIndex:0];
		[enc setFragmentBytes:zero length:128 atIndex:1];
		[enc setFragmentBytes:zero length:sizeof(zero) atIndex:25];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		Check(cb.status == MTLCommandBufferStatusCompleted, "pixel lane GPU");
		std::array<uint32_t, 64 * 8 * 4> pixels {};
		[color getBytes:pixels.data()
		    bytesPerRow:64 * 16
		     fromRegion:MTLRegionMake2D(0, 0, 64, 8)
		    mipmapLevel:0];
		for (unsigned y = 0; y < 8; ++y)
			for (unsigned x = 0; x < 64; ++x) {
				auto at = (y * 64 + x) * 4;
				Check(pixels[at] == pixels[at + 1] && pixels[at + 2] == pixels[at + 3],
				      "pixel lane spilled data mismatch");
				Check(pixels[at] == ((x * 0x9e3779b9u) ^ 0xdeadbeefu),
				      "pixel lane payload was replaced or zeroed");
				++checked;
			}
		++cases;
		if (loop_case != 0) continue;
		// An address into the next lane cannot be privatized, even if it only
		// differs by one dword. Neither can a read before its first store.
		for (unsigned negative = 0; negative < 4; ++negative) {
			Fixture bad(0);
			bad.program.stage     = ShaderType::Pixel;
			bad.program.wave_size = wave;
			bad.program.memory_info.push_back({.kind = IR::ResourceKind::Lds});
			auto l    = bad.Emit(0, ValueOpcode::LaneId, {});
			auto addr = bad.Emit(0, ValueOpcode::ShiftLeftLogical32, {l, Value(2u)});
			auto read_addr =
			    negative == 0 ? bad.Emit(0, ValueOpcode::IAdd32, {addr, Value(4u)}) : addr;
			if (negative != 1)
				bad.Emit(0, ValueOpcode::WriteSharedU32,
				         {addr, Value(123u), Value(negative == 2 ? false : true)}, 0);
			auto loaded = bad.Emit(0, ValueOpcode::LoadSharedU32, {read_addr, Value(true)}, 0);
			auto words =
			    bad.Emit(0, ValueOpcode::CompositeConstructU32x4, {loaded, loaded, loaded, loaded});
			bad.program.info.outputs = {{.kind = IR::StageOutputKind::Mrt}};
			bad.program.export_info.push_back({.kind = IR::ExportTargetKind::Mrt, .en = 15});
			bad.Emit(0, ValueOpcode::SetAttribute, {words, Value(true)}, 0);
			auto options = pi;
			if (negative == 3) options.extra_lds_size_dwords = 0;
			ShaderStageInputInfo stage {};
			stage.pixel = &options;
			auto result = ShaderRecompiler::Msl::EmitProgram(bad.program, stage);
			Check(!result.success, "unproven/shared/uninitialized LDS was privatized");
			++cases;
		}
	}
	std::printf("Pixel LDS ownership and definite stores: %u GPU/negative cases, %u texels PASS\n",
	            cases, checked);
}
