// The PS address can remain bound during a depth-only pass. Test the production
// activation decision against real depth, discard, mask and UAV side effects.
static unsigned PixelShaderActivationTests(id<MTLDevice> device,
    Libs::Graphics::Metal::MetalPipelineManager& manager) {
	using namespace Libs::Graphics; using namespace Libs::Graphics::Metal;
	NSError* error=nil;
	id<MTLLibrary> lib=[device newLibraryWithSource:@"#include <metal_stdlib>\n#include <metal_mesh>\nusing namespace metal;\n"
	    "struct V{float4 p[[position]];};using Mesh=metal::mesh<V,void,3,1,metal::topology::triangle>;\n"
	    "[[mesh]] void ms(Mesh m,uint t[[thread_index_in_threadgroup]]){if(t!=0)return;m.set_primitive_count(1);float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};for(uint i=0;i<3;++i){m.set_vertex(i,V{float4(p[i],.25,1)});m.set_index(i,i);}}\n"
	    "vertex float4 v(uint i[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],.25,1);}\n"
	    "struct In{float4 missing[[user(locn0),flat]];};fragment void stale(In in[[stage_in]]){if(in.missing.x>0)discard_fragment();}\n"
	    "fragment void side(device atomic_uint* n[[buffer(0)]]){atomic_fetch_add_explicit(n,1u,memory_order_relaxed);}\n"
	    "fragment void kill(float4 p[[position]]){if(p.x<4)discard_fragment();}\n"
	    "struct D{float z[[depth(any)]];};fragment D depth(){return D{.75f};}\n"
	    "struct M{uint m[[sample_mask]];};fragment M mask(){return M{0u};}\n"
	    "fragment float4 color(){return float4(1,0,0,1);}\n" options:nil error:&error];
	if(!lib){std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 1;}
	unsigned wrong=0,checked=0;
	// 0..4: independently disabled color consumers, despite a bound PS.
	// 5..9: each DB requirement in isolation; 10: color; 11: absent PS address.
	for(bool mesh:{false,true}) for(uint32_t test=0;test<12;++test) {
		HW::Context hw; HW::DepthShaderControl db{};
		hw.SetRenderTargetMask(15);hw.SetShaderMask(15);hw.SetTargetOutputMode(0,1);
		const char* fragment="stale";
		switch(test){
			case 0:hw.SetRenderTargetMask(0);break;
			case 1:hw.SetShaderMask(0);break;
			case 2:hw.SetTargetOutputMode(0,0);break;
			case 3:hw.SetColorControl({.mode=0});break;
			case 4:hw.SetRenderTargetMask(1);hw.SetShaderMask(2);break;
			case 5:db.shader_execute_on_noop=true;fragment="side";break;
			case 6:db.shader_kill_enable=true;fragment="kill";break;
			case 7:db.shader_z_export_enable=true;fragment="depth";break;
			case 8:db.shader_mask_export_enable=true;fragment="mask";break;
			case 9:db.shader_dual_export_enable=true;fragment="side";break;
			case 10:fragment="color";break;
			case 11:db.shader_execute_on_noop=true;break;
		}
		if(test>=5 && test<=9)hw.SetRenderTargetMask(0);
		hw.SetDepthShaderControl(db);
		const bool active=NativePixelShaderActive(hw,test==11?0:0x123400);
		if(active!=(test>=5 && test<=10)){++wrong;continue;}
		MetalRenderPipelineKey key{};key.vs_hash=0x505341+uint32_t(mesh);
		key.fs_hash=active?0x505342+test:0;
		key.primitive_type=uint32_t(Prospero::PrimitiveType::kTriList);
		key.depth_format=MTLPixelFormatDepth32Float;key.stencil_format=MTLPixelFormatStencil8;
		if(test==10){key.color_target_count=1;key.color_targets[0].pixel_format=MTLPixelFormatRGBA8Unorm;key.color_targets[0].write_mask=MTLColorWriteMaskAll;}
		auto pipeline=manager.GetOrCreateRenderPipeline(key,[lib newFunctionWithName:mesh?@"ms":@"v"],active?[lib newFunctionWithName:[NSString stringWithUTF8String:fragment]]:nil);
		if(!pipeline){++wrong;continue;}
		auto dd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:8 height:8 mipmapped:NO];
		dd.storageMode=MTLStorageModePrivate;dd.usage=MTLTextureUsageRenderTarget;
		auto depth=[device newTextureWithDescriptor:dd];
		dd.pixelFormat=MTLPixelFormatStencil8;auto stencil=[device newTextureWithDescriptor:dd];
		MetalRenderPassDesc pass{};pass.has_depth=true;pass.depth_attachment.texture=depth;
		pass.depth_attachment.load_action=MTLLoadActionClear;pass.depth_attachment.store_action=MTLStoreActionStore;pass.depth_attachment.clear_depth=.875;
		pass.has_stencil=true;pass.stencil_attachment.texture=stencil;pass.stencil_attachment.load_action=MTLLoadActionClear;pass.stencil_attachment.store_action=MTLStoreActionStore;pass.stencil_attachment.clear_stencil=0x13;
		if(test==10){dd.pixelFormat=MTLPixelFormatRGBA8Unorm;auto& c=pass.color_attachments[0];c.texture=[device newTextureWithDescriptor:dd];c.load_action=MTLLoadActionClear;c.store_action=MTLStoreActionStore;pass.color_attachment_count=1;}
		auto counter=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];*static_cast<uint32_t*>(counter.contents)=0;
		HW::DepthControl dc{};dc.z_enable=true;dc.z_write_enable=true;dc.zfunc=7;dc.stencil_enable=true;dc.stencilfunc=7;
		HW::StencilControl sc{};sc.stencil_zpass=uint8_t(Prospero::StencilOp::kReplaceTest);
		HW::StencilMask sm{};sm.stencil_mask=sm.stencil_writemask=255;sm.stencil_testval=0x42;
		MetalStencilRefs refs{};auto depth_stencil=manager.GetOrCreateDepthStencilState(dc,sc,sm,true,false,&refs);
		auto& draw=MetalDrawExecutor::Instance();if(!draw.BeginRenderPass(pass))return wrong+1;
		draw.SetRenderPipeline(pipeline);
		ShaderMeshInputInfo mi{};mi.native_execution=mi.ngg_vertex=true;mi.input_primitive=uint32_t(Prospero::PrimitiveType::kTriList);mi.primitives_per_group=1;mi.vertices_per_group=mi.max_vertices=3;mi.max_primitives=1;mi.threads_num[0]=32;mi.threads_num[1]=mi.threads_num[2]=1;
		draw.SetMeshState(mesh?&mi:nullptr);draw.SetDepthStencil(depth_stencil);draw.SetStencilReferences(refs.front,refs.back);
		draw.SetViewport(0,0,8,8,0,1);draw.SetScissor(0,0,8,8);
		if(active)draw.SetFragmentBuffer(counter,0,0);
		draw.Draw(Prospero::PrimitiveType::kTriList,3,1,0,0);draw.EndRenderPass();draw.SetMeshState(nullptr);
		auto read=[device newBufferWithLength:8*256 options:MTLResourceStorageModeShared];
		auto cmd=MetalContext::Instance().GetCurrentCommandBuffer();auto blit=[cmd blitCommandEncoder];
		[blit copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:read destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:8*256];
		auto stencil_read=[device newBufferWithLength:8*256 options:MTLResourceStorageModeShared];
		[blit copyFromTexture:stencil sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:stencil_read destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:8*256];
		id<MTLBuffer> colors=nil;
		if(test==10){colors=[device newBufferWithLength:8*256 options:MTLResourceStorageModeShared];[blit copyFromTexture:pass.color_attachments[0].texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:colors destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:8*256];}
		[blit endEncoding];MetalContext::Instance().CommitCommandBuffer(true);
		if(cmd.status!=MTLCommandBufferStatusCompleted)return wrong+1;
		wrong+=*static_cast<uint32_t*>(counter.contents)!=((test==5||test==9)?64u:0u);
		for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
			const float expected=test==7?.75f:((test==6&&x<4)||test==8)?.875f:.25f;
			float actual;std::memcpy(&actual,static_cast<uint8_t*>(read.contents)+y*256+x*4,4);
			wrong+=actual!=expected;++checked;
			const uint8_t expected_stencil=((test==6&&x<4)||test==8)?0x13:0x42;
			wrong+=static_cast<uint8_t*>(stencil_read.contents)[y*256+x]!=expected_stencil;
			if(colors){const auto*c=static_cast<uint8_t*>(colors.contents)+y*256+x*4;wrong+=c[0]!=255||c[1]!=0||c[2]!=0||c[3]!=255;}
		}
	}
	// Every MRT slot can activate a PS, with partial masks and unrelated slots
	// excluded. No dependence on SPI_PS_INPUT_ENA: an input-free PS can write UAVs.
	unsigned routes=0;
	for(unsigned slot=0;slot<8;++slot)for(unsigned channel=0;channel<4;++channel){
		HW::Context hw;hw.SetTargetOutputMode(slot,1);hw.SetShaderMask(1u<<(4*slot+channel));hw.SetRenderTargetMask(1u<<(4*slot+channel));
		wrong+=!NativePixelShaderActive(hw,1);hw.SetTargetOutputMode(slot,0);wrong+=NativePixelShaderActive(hw,1);routes+=2;
	}
	unsigned rejected=0;
	for(bool missing_active:{false,true}){
		MetalRenderPipelineKey bad{};bad.vs_hash=0x505341+1;bad.fs_hash=missing_active?0xBADu:0;
		bad.primitive_type=uint32_t(Prospero::PrimitiveType::kTriList);bad.depth_format=MTLPixelFormatDepth32Float;
		if(!missing_active){bad.color_target_count=1;bad.color_targets[0].pixel_format=MTLPixelFormatRGBA8Unorm;bad.color_targets[0].write_mask=MTLColorWriteMaskAll;}
		try{(void)manager.GetOrCreateRenderPipeline(bad,[lib newFunctionWithName:@"ms"],nil);++wrong;}catch(const std::runtime_error&){++rejected;}
	}
	std::printf("PS activation: 24 vertex/mesh GPU cases, %u depth+stencil pixels, 128 color pixels, UAV counts, %u MRT routes, %u invalid requests rejected, %u failures\n",checked,routes,rejected,wrong);
	return wrong;
}
