static std::vector<uint32_t> RunWaveFixture(Fixture& f,bool pixel,bool pair=false,uint32_t scissor_w=0,uint32_t scissor_h=0) {
	ShaderPixelInputInfo pi{};pi.wave_size=f.program.wave_size;pi.target_integer[0]=1;
	ShaderComputeInputInfo ci{};ci.host_subgroup_size=32;ci.threads_num[0]=64;ci.threads_num[1]=ci.threads_num[2]=1;
	auto function=Compile(f,pixel?&pi:nullptr,nullptr,pair?&ci:nullptr);NSError* error=nil;
	uint32_t zero[512]{};auto cb=[[device newCommandQueue] commandBuffer];
	std::vector<uint32_t> result((pixel?64*8:pair?64:32)*4);
	if(pixel) {
		auto lib=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;vertex float4 vs(uint i [[vertex_id]]){const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);}" options:nil error:&error];
		auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[lib newFunctionWithName:@"vs"];pd.fragmentFunction=function;pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Uint;
		auto pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];Check(pipeline!=nil,"wave pixel pipeline");
		auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint width:64 height:8 mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;
		auto color=[device newTextureWithDescriptor:td];auto pass=[MTLRenderPassDescriptor new];pass.colorAttachments[0].texture=color;
		pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);pass.colorAttachments[0].storeAction=MTLStoreActionStore;
		auto e=[cb renderCommandEncoderWithDescriptor:pass];[e setRenderPipelineState:pipeline];[e setFragmentBytes:zero length:512 atIndex:0];
		[e setFragmentBytes:zero length:128 atIndex:1];[e setFragmentBytes:zero length:sizeof(zero) atIndex:25];
		if(scissor_w!=0)[e setScissorRect:(MTLScissorRect){0,0,scissor_w,scissor_h}];
		[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];[cb commit];[cb waitUntilCompleted];
		Check(cb.status==MTLCommandBufferStatusCompleted,"wave pixel GPU execution");
		[color getBytes:result.data() bytesPerRow:64*16 fromRegion:MTLRegionMake2D(0,0,64,8) mipmapLevel:0];
	}else {
		auto pipeline=[device newComputePipelineStateWithFunction:function error:&error];Check(pipeline!=nil,"wave compute pipeline");
		auto output=[device newBufferWithLength:result.size()*4 options:MTLResourceStorageModeShared];std::memset(output.contents,0xcd,output.length);
		auto args=[function newArgumentEncoderWithBufferIndex:0];auto ab=[device newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];
		[args setArgumentBuffer:ab offset:0];[args setBuffer:output offset:0 atIndex:0];uint32_t sizes[512]{};sizes[0]=output.length;
		auto e=[cb computeCommandEncoder];[e setComputePipelineState:pipeline];[e setBuffer:ab offset:0 atIndex:0];[e setBytes:zero length:128 atIndex:1];
		[e setBytes:sizes length:sizeof(sizes) atIndex:25];[e useResource:output usage:MTLResourceUsageRead|MTLResourceUsageWrite];
		[e dispatchThreads:MTLSizeMake(32,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[e endEncoding];[cb commit];[cb waitUntilCompleted];
		Check(cb.status==MTLCommandBufferStatusCompleted,"wave compute GPU execution");std::memcpy(result.data(),output.contents,output.length);
	}return result;
}
static void WaveOutput(Fixture& f,uint32_t block,bool pixel,Value lane,std::array<Value,4> components) {
	if(pixel) {
		f.program.export_info.push_back({.kind=IR::ExportTargetKind::Mrt,.en=15});
		const auto value=f.Emit(block,ValueOpcode::CompositeConstructU32x4,{components[0],components[1],components[2],components[3]});
		f.Emit(block,ValueOpcode::SetAttribute,{value,Value(true)},0);
	}else {
		const auto buffer=f.Buffer(block);const auto memory=f.Memory(0);const auto at=f.Emit(block,ValueOpcode::IMul32,{lane,Value(16u)});
		for(unsigned c=0;c<4;++c) {
			const auto offset=f.Emit(block,ValueOpcode::IAdd32,{at,Value(c*4)});
			f.Emit(block,ValueOpcode::StoreBufferU32,{buffer,Value(0u),offset,Value(0u),components[c],Value(true)},memory);
		}
	}
}
// A partly covered pixel SIMD group has lanes that never launched: Apple fills a group from lane 0, and a 13x7 scissor
// on the 64x8 target leaves one group with lanes 0..15, 10x2 lanes 0..19, 6x2 lanes 0..11; an unscissored draw
// has only full groups and is the control. On the
// guest those lanes exist with EXEC off; whole-wave code (EXEC = ~EXEC | EXEC) turns them on holding the reduction
// identity, so the wave minimum of a light-list scalarization loop is the minimum of the launched lanes. Outside
// whole-wave code they are inactive sources. The loop's own trip bound is part of the guest test program: a lost
// minimum gives a wrong count instead of hanging the GPU; nothing in the emulator is capped.
static uint32_t PartialHighestBelow(uint32_t live,uint32_t target) {
	const uint32_t below=live&(0xffffffffu>>(31u-(target&31u)));
	return below!=0u?31u-uint32_t(__builtin_clz(below)):uint32_t(__builtin_ctz(live));
}
static void WavePartialGroups(unsigned& cases,unsigned& values) {
	const auto mod=[](Fixture& f,uint32_t b,Value v,uint32_t m){return f.Emit(b,ValueOpcode::ISub32,{v,f.Emit(b,ValueOpcode::IMul32,{f.Emit(b,ValueOpcode::UDiv32,{v,Value(m)}),Value(m)})});};
	const auto live_mask=[](Fixture& f,uint32_t b){return f.Emit(b,ValueOpcode::CompositeExtractU32x4,{f.Emit(b,ValueOpcode::Ballot,{Value(true)}),Value(0u)});};
	// KYTX_WAVE_PARTIAL_PART=loop|0|1|2|3 runs one part (negative controls against an older emitter).
	const char* part=std::getenv("KYTX_WAVE_PARTIAL_PART");
	const auto run=[&](const char* name){return part==nullptr || std::strcmp(part,name)==0;};
	const std::array<std::array<uint32_t,2>,4> scissors{{{0u,0u},{13u,7u},{10u,2u},{6u,2u}}};
	if(run("loop"))for(const auto& sc:scissors) {const bool partial=sc[0]!=0u;
		// Scalarization loop: every lane walks its own ascending list; each trip the wave minimum is taken with the
		// game's idiom (DPP row_shr min scan FI0/BC0, permlanex16 FI0, readlane 31 and 63) and the matching lanes advance.
		Fixture f(0);f.program.stage=ShaderType::Pixel;f.program.wave_size=64;f.program.info.outputs={{.kind=IR::StageOutputKind::Mrt}};
		const auto lane=f.Emit(0,ValueOpcode::LaneId,{});
		const auto n=f.Emit(0,ValueOpcode::IAdd32,{mod(f,0,lane,3u),Value(1u)});
		const auto base=f.Emit(0,ValueOpcode::IAdd32,{mod(f,0,f.Emit(0,ValueOpcode::IMul32,{lane,Value(7u)}),13u),Value(1u)});
		const auto step=f.Emit(0,ValueOpcode::IAdd32,{f.Emit(0,ValueOpcode::BitwiseAnd32,{lane,Value(3u)}),Value(1u)});
		const auto h=f.Block(),body=f.Block(),done=f.Block();f.program.block_info[0].terminator={.kind=CFG::TerminatorKind::Branch,.true_block=h};
		auto phi=[&](){auto& p=f.program.blocks[h]->AppendNewInst(ValueOpcode::Phi,{},uint64_t(IR::Type::U32));p.AddPhiOperand(f.program.blocks[0],Value(0u));return &p;};
		auto* count=phi();auto* acc=phi();auto* iter=phi();
		const auto live=f.Emit(h,ValueOpcode::ULessThan32,{Value(count),n});
		const auto go=f.Emit(h,ValueOpcode::LogicalAnd,{live,f.Emit(h,ValueOpcode::ULessThan32,{Value(iter),Value(64u)})});
		f.program.block_info[h].condition=f.Emit(h,ValueOpcode::ConditionRef,{go},uint64_t(CFG::BranchCondition::ExecNonZero));
		f.program.block_info[h].terminator={.kind=CFG::TerminatorKind::ConditionalBranch,.true_block=body,.false_block=done,.merge_block=done,.continue_block=body,.loop_header=true};
		const auto index=f.Emit(body,ValueOpcode::IAdd32,{base,f.Emit(body,ValueOpcode::IMul32,{Value(count),step})});
		const auto whole=f.Emit(body,ValueOpcode::LogicalOr,{f.Emit(body,ValueOpcode::LogicalNot,{live}),live});
		auto value=f.Emit(body,ValueOpcode::SelectU32,{live,index,Value(UINT32_MAX)});
		for(uint32_t shift:{1u,2u,4u,8u}) {
			IR::DppMoveFlags flags{.control=0x110u+shift};uint64_t packed=0;std::memcpy(&packed,&flags,sizeof(flags));
			const auto move=f.Emit(body,ValueOpcode::DppMoveU32,{value,whole},packed);
			value=f.Emit(body,ValueOpcode::DppUpdateU32,{f.Emit(body,ValueOpcode::UMin32,{move,value}),value,whole},packed);
		}
		IR::PermlaneFlags x16{.x16=true};uint64_t packed=0;std::memcpy(&packed,&x16,sizeof(x16));
		value=f.Emit(body,ValueOpcode::UMin32,{value,f.Emit(body,ValueOpcode::Permlane16U32,{value,Value(UINT32_MAX),Value(UINT32_MAX),whole},packed)});
		const auto minimum=f.Emit(body,ValueOpcode::UMin32,{f.Emit(body,ValueOpcode::ReadLane,{value,Value(31u)}),f.Emit(body,ValueOpcode::ReadLane,{value,Value(63u)})});
		const auto match=f.Emit(body,ValueOpcode::LogicalAnd,{live,f.Emit(body,ValueOpcode::UGreaterThanEqual32,{minimum,index})});
		count->AddPhiOperand(f.program.blocks[body],f.Emit(body,ValueOpcode::IAdd32,{Value(count),f.Emit(body,ValueOpcode::SelectU32,{match,Value(1u),Value(0u)})}));
		acc->AddPhiOperand(f.program.blocks[body],f.Emit(body,ValueOpcode::IAdd32,{Value(acc),f.Emit(body,ValueOpcode::SelectU32,{match,index,Value(0u)})}));
		iter->AddPhiOperand(f.program.blocks[body],f.Emit(body,ValueOpcode::IAdd32,{Value(iter),Value(1u)}));
		f.program.block_info[body].terminator={.kind=CFG::TerminatorKind::Branch,.true_block=h};
		const auto tag=f.Emit(done,ValueOpcode::BitwiseOr32,{f.Emit(done,ValueOpcode::BitwiseOr32,{lane,f.Emit(done,ValueOpcode::ShiftLeftLogical32,{Value(iter),Value(8u)})}),Value(0x10000u)});
		WaveOutput(f,done,true,lane,{Value(count),Value(acc),live_mask(f,done),tag});
		const auto result=RunWaveFixture(f,true,false,sc[0],sc[1]);
		unsigned written=0,in_partial=0;
		for(size_t p=0;p<result.size();p+=4) {
			if(result[p+3]==0u)continue;++written;
			const uint32_t l=result[p+3]&0xffu,trips=(result[p+3]>>8)&0xffu,mask=result[p+2];
			if(!(l<32 && ((mask>>l)&1u)!=0u))std::fprintf(stderr,"partial loop scissor=%ux%u lane=%u mask=%08x out=%08x/%08x/%08x/%08x\n",sc[0],sc[1],l,mask,result[p],result[p+1],result[p+2],result[p+3]);
			Check(l<32 && ((mask>>l)&1u)!=0u,"partial group lane/mask output");if(mask!=0xffffffffu)++in_partial;
			const uint32_t ln=l%3u+1u,lb=(l*7u)%13u+1u,ls=(l&3u)+1u;uint32_t sum=0;for(uint32_t i=0;i<ln;++i)sum+=lb+i*ls;
			if(result[p]!=ln || result[p+1]!=sum)std::fprintf(stderr,"partial loop lane=%u mask=%08x trips=%u count=%u/%u acc=%u/%u\n",l,mask,trips,result[p],ln,result[p+1],sum);
			Check(result[p]==ln && result[p+1]==sum && trips<64,"scalarization loop lost the wave minimum in a partly covered pixel group");values+=2;
		}
		Check(written==(partial?sc[0]*sc[1]:64u*8u),"partial group fixture coverage");
		Check(partial==(in_partial!=0u),"partial group fixture did not produce a partly covered SIMD group");
		++cases;
	}
	// Direct reads of a lane that never launched, against the guest rules above. Values are 100+lane, packed with the
	// reading lane in the high half.
	for(uint32_t form:{0u,1u,2u,3u})if(run(std::to_string(form).c_str()))for(const auto& sc:{std::array<uint32_t,2>{6u,2u},std::array<uint32_t,2>{10u,2u},std::array<uint32_t,2>{13u,7u}}) {
		Fixture f(0);f.program.stage=ShaderType::Pixel;f.program.wave_size=64;f.program.info.outputs={{.kind=IR::StageOutputKind::Mrt}};
		const auto lane=f.Emit(0,ValueOpcode::LaneId,{}),data=f.Emit(0,ValueOpcode::IAdd32,{lane,Value(100u)});
		const auto runtime_true=f.Emit(0,ValueOpcode::ULessThan32,{lane,Value(64u)});
		const auto whole=f.Emit(0,ValueOpcode::LogicalOr,{f.Emit(0,ValueOpcode::LogicalNot,{runtime_true}),runtime_true});
		const auto tagged=[&](Value v){return f.Emit(0,ValueOpcode::IAdd32,{v,f.Emit(0,ValueOpcode::ShiftLeftLogical32,{lane,Value(16u)})});};
		const auto dpp=[&](uint32_t control,bool fi,Value exec){IR::DppMoveFlags fl{.control=control,.fetch_inactive=fi};uint64_t pk=0;std::memcpy(&pk,&fl,sizeof(fl));return f.Emit(0,ValueOpcode::DppMoveU32,{data,exec},pk);};
		const auto perm=[&](bool fi,Value exec){IR::PermlaneFlags fl{.x16=true,.fetch_inactive=fi};uint64_t pk=0;std::memcpy(&pk,&fl,sizeof(fl));return f.Emit(0,ValueOpcode::Permlane16U32,{data,Value(UINT32_MAX),Value(UINT32_MAX),exec},pk);};
		std::array<Value,3> v;
		if(form==0)v={f.Emit(0,ValueOpcode::ReadLane,{data,Value(31u)}),f.Emit(0,ValueOpcode::ReadLane,{data,Value(20u)}),f.Emit(0,ValueOpcode::ReadLane,{data,Value(63u)})};
		else if(form==1)v={dpp(0x101u,false,whole),dpp(0x101u,false,runtime_true),dpp(0x101u,true,runtime_true)};
		else if(form==2)v={perm(false,whole),perm(false,runtime_true),dpp(0x140u,false,whole)};
		else {
			// DppUpdate(move + 7, data): an absent source under a whole-wave EXEC keeps the destination (the identity
			// result); outside it the source is inactive and BOUND_CTRL decides.
			const auto update=[&](bool bc,Value exec){IR::DppMoveFlags fl{.control=0x101u,.bound_control=bc};uint64_t pk=0;std::memcpy(&pk,&fl,sizeof(fl));
				const auto mv=f.Emit(0,ValueOpcode::DppMoveU32,{data,exec},pk);return f.Emit(0,ValueOpcode::DppUpdateU32,{f.Emit(0,ValueOpcode::IAdd32,{mv,Value(7u)}),data,exec},pk);};
			v={update(false,whole),update(true,whole),update(false,runtime_true)};
		}
		WaveOutput(f,0,true,lane,{tagged(v[0]),tagged(v[1]),tagged(v[2]),live_mask(f,0)});
		const auto result=RunWaveFixture(f,true,false,sc[0],sc[1]);
		unsigned in_partial=0;
		for(size_t p=0;p<result.size();p+=4) {
			const uint32_t mask=result[p+3];if(mask==0u)continue;
			const uint32_t l=result[p]>>16;Check(l<32 && (result[p+1]>>16)==l && (result[p+2]>>16)==l && ((mask>>l)&1u)!=0u,"partial group lane tags");
			if(mask!=0xffffffffu)++in_partial;
			const auto present=[&](uint32_t t){return ((mask>>(t&31u))&1u)!=0u;};
			const uint32_t col=l&15u,shl=(l&~15u)|(col+1u),cross=((l&~15u)^16u)|15u,mirror=(l&~15u)|(15u-col);
			std::array<uint32_t,3> expect;
			if(form==0)expect={100u+PartialHighestBelow(mask,31u),100u+PartialHighestBelow(mask,20u),100u+PartialHighestBelow(mask,31u)};
			else if(form==1)expect={col<15u?(present(shl)?100u+shl:100u+l):0u,col<15u?(present(shl)?100u+shl:0u):0u,col<15u?(present(shl)?100u+shl:100u+l):0u};
			else if(form==2)expect={present(cross)?100u+cross:100u+l,present(cross)?100u+cross:0u,present(mirror)?100u+mirror:100u+l};
			else {
				const bool valid=col<15u,src=valid && present(shl);
				expect={src?107u+shl:100u+l,src?107u+shl:(valid?100u+l:7u),src?107u+shl:100u+l};
			}
			for(unsigned c=0;c<3;++c) {
				if((result[p+c]&0xffffu)!=expect[c])std::fprintf(stderr,"absent-lane read form=%u component=%u lane=%u mask=%08x got=%u expected=%u\n",form,c,l,mask,result[p+c]&0xffffu,expect[c]);
				Check((result[p+c]&0xffffu)==expect[c],"cross-lane read of a lane that never launched");++values;
			}
		}
		Check(in_partial!=0u,"absent-lane fixture did not produce a partly covered SIMD group");++cases;
	}
}
// A wave64 program running native32 is a guest wave whose lanes 32..63 never launched. The game's reductions combine the two
// halves as op(readlane(V, 31 or 0), readlane(V, 63 or 32)); the missing half contributes op's identity, so add must not count
// the low half twice (FC 27 compute 6b784196fcba7b41: [32,1,1] workgroup, butterfly add, readlane 31 + 63, then log2).
static void WaveUpperHalf(unsigned& cases,unsigned& values) {
	enum class Op{Add,Xor,UMin,UMax,FAdd};
	const auto combine=[](Fixture& f,uint32_t b,Op op,Value x,Value y)->Value{
		switch(op){
			case Op::Add:return f.Emit(b,ValueOpcode::IAdd32,{x,y});
			case Op::Xor:return f.Emit(b,ValueOpcode::BitwiseXor32,{x,y});
			case Op::UMin:return f.Emit(b,ValueOpcode::UMin32,{x,y});
			case Op::UMax:return f.Emit(b,ValueOpcode::UMax32,{x,y});
			default:return f.Emit(b,ValueOpcode::BitCastU32F32,{f.Emit(b,ValueOpcode::FPAdd32,{f.Emit(b,ValueOpcode::BitCastF32U32,{x}),f.Emit(b,ValueOpcode::BitCastF32U32,{y})})});
		}
	};
	const auto fold=[](Op op,const std::vector<uint32_t>& in)->uint32_t{
		uint32_t r=op==Op::UMin?UINT32_MAX:0u;float fr=0.0f;
		for(uint32_t v:in){switch(op){case Op::Add:r+=v;break;case Op::Xor:r^=v;break;case Op::UMin:r=std::min(r,v);break;case Op::UMax:r=std::max(r,v);break;default:fr+=std::bit_cast<float>(v);}}
		return op==Op::FAdd?std::bit_cast<uint32_t>(fr):r;
	};
	const auto dpp=[](Fixture& f,uint32_t b,uint32_t control,bool bc,Value v,Value exec,Op op,const auto& comb){
		IR::DppMoveFlags fl{.control=control,.bound_control=bc};uint64_t pk=0;std::memcpy(&pk,&fl,sizeof(fl));
		const auto mv=f.Emit(b,ValueOpcode::DppMoveU32,{v,exec},pk);
		return f.Emit(b,ValueOpcode::DppUpdateU32,{comb(f,b,op,mv,v),v,exec},pk);
	};
	const auto perm=[](Fixture& f,uint32_t b,Value v,Value exec){
		IR::PermlaneFlags fl{.x16=true,.bound_control=true};uint64_t pk=0;std::memcpy(&pk,&fl,sizeof(fl));
		return f.Emit(b,ValueOpcode::Permlane16U32,{v,Value(UINT32_MAX),Value(UINT32_MAX),exec},pk);
	};
	const auto live_mask=[](Fixture& f,uint32_t b){return f.Emit(b,ValueOpcode::CompositeExtractU32x4,{f.Emit(b,ValueOpcode::Ballot,{Value(true)}),Value(0u)});};
	const auto data_of=[](Op op,uint32_t l){return op==Op::FAdd?std::bit_cast<uint32_t>(float(l)+0.5f):l+1u;};
	const auto data=[](Fixture& f,uint32_t b,Op op,Value lane){
		if(op!=Op::FAdd)return f.Emit(b,ValueOpcode::IAdd32,{lane,Value(1u)});
		return f.Emit(b,ValueOpcode::BitCastU32F32,{f.Emit(b,ValueOpcode::FPAdd32,{f.Emit(b,ValueOpcode::ConvertF32U32,{lane}),Value::F32(0.5f)})});
	};
	// 1. The 6b784196 idiom in full groups: butterfly row_xmask 1/2/4/8 (BC1) and permlanex16 (BC1) under the launched-lane
	// EXEC, then op(readlane 31, readlane 63). Compute with a 32-lane workgroup and pixel waves.
	// KYTX_WAVE_UPPER_PART=1|2|3 runs one part (negative controls against an older emitter).
	const char* part=std::getenv("KYTX_WAVE_UPPER_PART");
	const auto run=[&](char c){return part==nullptr || part[0]==c;};
	if(run('1'))for(bool pixel:{false,true})for(Op op:{Op::Add,Op::Xor,Op::UMin,Op::FAdd}) {
		Fixture f(pixel?0:1);f.program.stage=pixel?ShaderType::Pixel:ShaderType::Compute;f.program.wave_size=64;
		if(pixel)f.program.info.outputs={{.kind=IR::StageOutputKind::Mrt}};
		const auto lane=f.Emit(0,ValueOpcode::LaneId,{});const auto exec=f.Emit(0,ValueOpcode::ULessThan32,{lane,Value(32u)});
		Value v=data(f,0,op,lane);
		for(uint32_t m:{1u,2u,4u,8u})v=dpp(f,0,0x160u|m,true,v,exec,op,combine);
		const auto p=perm(f,0,v,exec);v=combine(f,0,op,f.Emit(0,ValueOpcode::SelectU32,{exec,p,v}),v);
		const auto total=combine(f,0,op,f.Emit(0,ValueOpcode::ReadLane,{v,Value(31u)}),f.Emit(0,ValueOpcode::ReadLane,{v,Value(63u)}));
		WaveOutput(f,0,pixel,lane,{total,v,lane,pixel?live_mask(f,0):Value(0x77u)});
		const auto result=RunWaveFixture(f,pixel);
		std::vector<uint32_t> in;for(uint32_t l=0;l<32;++l)in.push_back(data_of(op,l));const uint32_t expect=fold(op,in);
		for(size_t q=0;q<result.size();q+=4) {
			if(result[q]!=expect || result[q+1]!=expect)std::fprintf(stderr,"upper half pixel=%d op=%d lane=%u total=%08x row=%08x expected=%08x\n",int(pixel),int(op),result[q+2],result[q],result[q+1],expect);
			Check(result[q]==expect && result[q+1]==expect,"wave64 cross-half reduction counted the launched half twice");values+=2;
		}++cases;
	}
	// 2. Whole-wave butterfly in partly covered pixel groups, LLVM form op(readlane 0, readlane 32): every launched lane holds
	// its row's total after the row steps and the 32-lane total after permlanex16; the missing lanes hold the identity.
	// 3. Scan form of the game's pixel shaders: row_shr 1/2/4/8 (BC0), permlanex16, op(readlane 31, readlane 63).
	for(bool scan:{false,true})if(run(scan?'3':'2'))for(Op op:{Op::Add,Op::UMax})for(const auto& sc:{std::array<uint32_t,2>{6u,2u},std::array<uint32_t,2>{13u,7u},std::array<uint32_t,2>{10u,2u}}) {
		Fixture f(0);f.program.stage=ShaderType::Pixel;f.program.wave_size=64;f.program.info.outputs={{.kind=IR::StageOutputKind::Mrt}};
		const auto lane=f.Emit(0,ValueOpcode::LaneId,{});const auto rt=f.Emit(0,ValueOpcode::ULessThan32,{lane,Value(64u)});
		const auto whole=f.Emit(0,ValueOpcode::LogicalOr,{f.Emit(0,ValueOpcode::LogicalNot,{rt}),rt});
		Value v=data(f,0,op,lane);
		for(uint32_t m:{1u,2u,4u,8u})v=scan?dpp(f,0,0x110u|m,false,v,whole,op,combine):dpp(f,0,0x160u|m,true,v,whole,op,combine);
		const Value row=v;
		v=combine(f,0,op,v,perm(f,0,v,whole));
		const auto total=scan?combine(f,0,op,f.Emit(0,ValueOpcode::ReadLane,{v,Value(31u)}),f.Emit(0,ValueOpcode::ReadLane,{v,Value(63u)}))
		                     :combine(f,0,op,f.Emit(0,ValueOpcode::ReadLane,{v,Value(0u)}),f.Emit(0,ValueOpcode::ReadLane,{v,Value(32u)}));
		// Helper lanes run and take part like any launched lane, so the oracle uses the group's ballot, not the written pixels.
		const auto tagged=[&](Value x){return f.Emit(0,ValueOpcode::IAdd32,{x,f.Emit(0,ValueOpcode::ShiftLeftLogical32,{lane,Value(24u)})});};
		WaveOutput(f,0,true,lane,{total,tagged(row),tagged(v),live_mask(f,0)});
		const auto result=RunWaveFixture(f,true,false,sc[0],sc[1]);
		unsigned in_partial=0;
		for(size_t q=0;q<result.size();q+=4) {
			const uint32_t mask=result[q+3];if(mask==0u)continue;
			const uint32_t l=result[q+1]>>24,r=l&~15u,n=uint32_t(std::popcount(mask));
			Check(l<32 && (result[q+2]>>24)==l && mask==(n==32u?UINT32_MAX:(1u<<n)-1u),"partial group lanes are not filled from lane 0");
			if(n<32u)++in_partial;
			const auto range=[&](uint32_t lo,uint32_t hi){std::vector<uint32_t> in;for(uint32_t k=lo;k<hi && k<n;++k)in.push_back(data_of(op,k));return fold(op,in);};
			const uint32_t all=range(0,n),row_expect=scan?range(r,l+1):range(r,r+16);
			const std::vector<uint32_t> two{row_expect,range(r^16u,(r^16u)+16u)};const uint32_t cross_expect=fold(op,two);
			const uint32_t got_row=result[q+1]&0xffffffu,got_cross=result[q+2]&0xffffffu;
			if(result[q]!=all || got_row!=row_expect || got_cross!=cross_expect)
				std::fprintf(stderr,"partial %s op=%d n=%u lane=%u total=%u/%u row=%u/%u cross=%u/%u\n",scan?"scan":"butterfly",int(op),n,l,result[q],all,got_row,row_expect,got_cross,cross_expect);
			Check(result[q]==all && got_row==row_expect && got_cross==cross_expect,"partly covered wave64 reduction differs from the launched lanes' result");values+=3;
		}
		Check(in_partial!=0u,"wave64 reduction fixture did not produce a partly covered group");++cases;
	}
	std::printf("Wave64 upper half: %u cases PASS\n",cases);
}
static void WaveControlOperations() {
	unsigned cases=0,values=0;
	if(std::getenv("KYTX_WAVE_PARTIAL_PART")){WavePartialGroups(cases,values);std::printf("Partial pixel groups: %u lane values PASS\n",values);return;}
	if(std::getenv("KYTX_WAVE_UPPER_ONLY")){WaveUpperHalf(cases,values);return;}
	// EXEC/VCC branches are uniform tests of a mask. Expression and scalar
	// instruction predicates keep their own meaning and must not be rewritten.
	if(!std::getenv("KYTX_WAVE_DPP_ONLY"))for(bool pixel:{false,true})for(auto kind:{CFG::BranchCondition::ExecZero,CFG::BranchCondition::ExecNonZero,
	    CFG::BranchCondition::VccZero,CFG::BranchCondition::VccNonZero,CFG::BranchCondition::Expression,CFG::BranchCondition::ScalarInstruction})
	for(uint32_t limit:{0u,1u,17u,32u}) {
		Fixture f(pixel?0:1);f.program.stage=pixel?ShaderType::Pixel:ShaderType::Compute;f.program.info.outputs=pixel?std::vector<IR::StageOutput>{{.kind=IR::StageOutputKind::Mrt}}:std::vector<IR::StageOutput>{};
		const auto lane=f.Emit(0,ValueOpcode::LaneId,{}),active=f.Emit(0,ValueOpcode::ULessThan32,{lane,Value(limit)});
		const bool zero=kind==CFG::BranchCondition::ExecZero || kind==CFG::BranchCondition::VccZero;
		const auto argument=zero?f.Emit(0,ValueOpcode::LogicalNot,{active}):active;
		const auto condition=f.Emit(0,ValueOpcode::ConditionRef,{argument},uint64_t(kind));
		const auto bits=f.Emit(0,ValueOpcode::SelectU32,{condition,Value(1u),Value(0u)});
		WaveOutput(f,0,pixel,lane,{bits,lane,Value(limit),Value(0x12abcdefu)});
		const auto result=RunWaveFixture(f,pixel);
		for(size_t p=0;p<result.size();p+=4) {
			const uint32_t l=result[p+1];Check(l<32,"wave lane output");
			const bool uniform=kind!=CFG::BranchCondition::Expression && kind!=CFG::BranchCondition::ScalarInstruction;
			const uint32_t expected=uniform?(zero?limit==0:limit!=0):l<limit;
			Check(result[p]==expected && result[p+3]==0x12abcdefu,"EXEC/VCC branch split the native wave");++values;
		}++cases;
	}
	// DPP row edges, destination masks and source EXEC are independent controls.
	for(bool pair:{false,true})for(uint32_t control:{0x101u,0x107u,0x111u,0x118u,0x140u,0x15fu})
	for(bool fi:{false,true})for(bool bc:{false,true})for(uint32_t masks:{0xffu,0x52u}) {
		Fixture f;f.program.wave_size=pair?64:32;const auto lane=f.Emit(0,ValueOpcode::LaneId,{});
		const auto live=f.Emit(0,ValueOpcode::INotEqual32,{f.Emit(0,ValueOpcode::BitwiseAnd32,{lane,Value(3u)}),Value(0u)});
		const auto data=f.Emit(0,ValueOpcode::IAdd32,{lane,Value(100u)});
		IR::DppMoveFlags flags{.control=control,.row_mask=masks>>4,.bank_mask=masks&15,.fetch_inactive=fi,.bound_control=bc};uint64_t packed=0;std::memcpy(&packed,&flags,sizeof(flags));
		const auto moved=f.Emit(0,ValueOpcode::DppMoveU32,{data,live},packed);
		const auto operation=f.Emit(0,ValueOpcode::IAdd32,{moved,Value(7u)});
		const auto updated=f.Emit(0,ValueOpcode::DppUpdateU32,{operation,data,live},packed);
		WaveOutput(f,0,false,lane,{moved,updated,lane,Value(0x5555u)});
		const auto result=RunWaveFixture(f,false,pair);
		for(uint32_t l=0;l<(pair?64u:32u);++l) {
			int col=l%16,source=0;bool valid=true;
			if(control>=0x101 && control<=0x10f){source=col+int(control&15);valid=source<16;}
			else if(control>=0x111 && control<=0x11f){source=col-int(control&15);valid=source>=0;}
			else source=control==0x140?15-col:15;
			const uint32_t target=(l&~15u)|(uint32_t(source)&15u);
			const bool source_live=(target&3)!=0,dest_live=(l&3)!=0;
			const uint32_t move=valid && (fi || source_live)?100+target:0;
			// RDNA DPP16: with FI = 0 an inactive source lane is invalid like one outside the row; BOUND_CTRL = 0 then
			// suppresses the write, BOUND_CTRL = 1 writes the operation on zero.
			const bool write=dest_live && (((masks&15)>>(l/4%4))&1) && (((masks>>4)>>(l/16))&1) && (bc || (valid && (fi || source_live)));
			const uint32_t update=write?move+7:100+l;
			if(result[l*4]!=move || result[l*4+1]!=update)std::fprintf(stderr,"DPP pair=%d ctrl=%x fi=%d bc=%d mask=%x lane=%u got=%u/%u expected=%u/%u\n",int(pair),control,fi,bc,masks,l,result[l*4],result[l*4+1],move,update);
			Check(result[l*4]==move && result[l*4+1]==update,"DPP row edge/mask/source EXEC semantics");values+=2;
		}++cases;
	}
	if(std::getenv("KYTX_WAVE_DPP_ONLY"))return;
	for(uint32_t control:{0x100u,0x110u,0x130u,0x142u,0x170u}) {
		Fixture bad;const auto lane=bad.Emit(0,ValueOpcode::LaneId,{});IR::DppMoveFlags flags{.control=control};uint64_t packed=0;std::memcpy(&packed,&flags,sizeof(flags));
		const auto move=bad.Emit(0,ValueOpcode::DppMoveU32,{lane,Value(true)},packed);const auto buffer=bad.Buffer(0);bad.Store(0,buffer,0,move,bad.Memory(0));
		Check(!ShaderRecompiler::Msl::EmitProgram(bad.program,{}).success,"unknown DPP control silently became an identity move");
	}
	// Re-enter all lines for a reduction after some logical lanes finish. Seven
	// genuine guest iterations; no diagnostic cap or synthetic early exit.
	for(bool pixel:{false,true})for(uint32_t wave:pixel?std::vector<uint32_t>{32,64}:std::vector<uint32_t>{32}) {
		Fixture f(pixel?0:1);f.program.stage=pixel?ShaderType::Pixel:ShaderType::Compute;f.program.wave_size=wave;
		if(pixel)f.program.info.outputs={{.kind=IR::StageOutputKind::Mrt}};
		const auto lane=f.Emit(0,ValueOpcode::LaneId,{});
		const auto trips=f.Emit(0,ValueOpcode::IAdd32,{f.Emit(0,ValueOpcode::ISub32,{lane,f.Emit(0,ValueOpcode::IMul32,{f.Emit(0,ValueOpcode::UDiv32,{lane,Value(7u)}),Value(7u)})}),Value(1u)});
		const auto h=f.Block(),body=f.Block(),done=f.Block();f.program.block_info[0].terminator={.kind=CFG::TerminatorKind::Branch,.true_block=h};
		auto& count=f.program.blocks[h]->AppendNewInst(ValueOpcode::Phi,{},uint64_t(IR::Type::U32));count.AddPhiOperand(f.program.blocks[0],Value(0u));
		auto& minimum=f.program.blocks[h]->AppendNewInst(ValueOpcode::Phi,{},uint64_t(IR::Type::U32));minimum.AddPhiOperand(f.program.blocks[0],Value(0u));
		const auto live=f.Emit(h,ValueOpcode::ULessThan32,{Value(&count),trips});
		f.program.block_info[h].condition=f.Emit(h,ValueOpcode::ConditionRef,{live},uint64_t(CFG::BranchCondition::ExecNonZero));
		f.program.block_info[h].terminator={.kind=CFG::TerminatorKind::ConditionalBranch,.true_block=body,.false_block=done,.merge_block=done,.continue_block=body,.loop_header=true};
		auto value=f.Emit(body,ValueOpcode::SelectU32,{live,f.Emit(body,ValueOpcode::IAdd32,{lane,Value(1u)}),Value(UINT32_MAX)});
		for(uint32_t shift:{1u,2u,4u,8u}) {
			IR::DppMoveFlags flags{.control=0x110u+shift};uint64_t packed=0;std::memcpy(&packed,&flags,sizeof(flags));
			const auto move=f.Emit(body,ValueOpcode::DppMoveU32,{value,Value(true)},packed);
			const auto combine=f.Emit(body,ValueOpcode::UMin32,{move,value});value=f.Emit(body,ValueOpcode::DppUpdateU32,{combine,value,Value(true)},packed);
		}
		IR::PermlaneFlags permutation{.x16=true,.fetch_inactive=true};uint64_t packed=0;std::memcpy(&packed,&permutation,sizeof(permutation));
		const auto cross=f.Emit(body,ValueOpcode::Permlane16U32,{value,Value(UINT32_MAX),Value(UINT32_MAX),Value(true)},packed);
		value=f.Emit(body,ValueOpcode::UMin32,{value,cross});value=f.Emit(body,ValueOpcode::ReadLane,{value,Value(31u)});
		minimum.AddPhiOperand(f.program.blocks[body],value);
		count.AddPhiOperand(f.program.blocks[body],f.Emit(body,ValueOpcode::IAdd32,{Value(&count),Value(1u)}));
		f.program.block_info[body].terminator={.kind=CFG::TerminatorKind::Branch,.true_block=h};
		WaveOutput(f,done,pixel,lane,{Value(&count),Value(&minimum),lane,Value(0x1234u)});
		const auto result=RunWaveFixture(f,pixel);
		for(size_t p=0;p<result.size();p+=4){Check(result[p]==7 && result[p+1]==7 && result[p+3]==0x1234u,"scalar loop lost reactivated lanes or reduction progress");values+=2;}
		++cases;
	}
	WavePartialGroups(cases,values);
	WaveUpperHalf(cases,values);
	std::printf("Wave control: %u native compute/pixel GPU cases, %u lane values, scalar EXEC/VCC + DPP edge/masks/inactive source + reactivation loop + partial pixel groups + wave64 upper half PASS\n",cases,values);
}
