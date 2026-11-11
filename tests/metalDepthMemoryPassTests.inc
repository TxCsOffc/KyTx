#include "graphics/host_gpu/metal/metalDepthMemory.h"
#include "graphics/host_gpu/metal/metalAttachmentArray.h"
static unsigned DepthMemoryPassTests(id<MTLDevice> device) {
	using namespace Libs::Graphics;using namespace Libs::Graphics::Metal;
	auto&ctx=MetalContext::Instance();auto&draw=MetalDrawExecutor::Instance();NSError*error=nil;
	id<MTLLibrary>lib=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;struct V{float4 p[[position]];uint layer[[render_target_array_index]];};vertex V v(uint i[[vertex_id]],uint instance[[instance_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return {float4(p[i],0,1),instance};}struct D{float z[[depth(any)]];};fragment D f(){return {0.0f};}" options:nil error:&error];
	if(!lib){std::fprintf(stderr,"DepthMemoryPass failure line=%d: %s\n",__LINE__,error?error.localizedDescription.UTF8String:"contract");return 1;}unsigned errors=0,cases=0;uint64_t checked=0;
	for(auto fmt:{MTLPixelFormatDepth16Unorm,MTLPixelFormatDepth32Float}) {
		const uint32_t eb=fmt==MTLPixelFormatDepth16Unorm?2:4;
		TileSurfaceLayout zlayout{},slayout{};
		if(!TileGetTiledTextureLayout({eb==2?Prospero::BufferFormat::k16UInt:Prospero::BufferFormat::k32UInt,Prospero::TileMode::kDepth,TileSurfaceDimension::Dim2D,37,23,1,1,4},zlayout) ||
		   !TileGetTiledTextureLayout({Prospero::BufferFormat::k8UInt,Prospero::TileMode::kDepth,TileSurfaceDimension::Dim2D,37,23,1,1,4},slayout)){std::fprintf(stderr,"DepthMemoryPass failure line=%d: %s\n",__LINE__,error?error.localizedDescription.UTF8String:"contract");return 1;}
		id<MTLBuffer>z=[device newBufferWithLength:zlayout.total_size+512 options:MTLResourceStorageModeShared];
		id<MTLBuffer>s=[device newBufferWithLength:slayout.total_size+512 options:MTLResourceStorageModeShared];
		std::vector<uint8_t>zt(z.length,0xa3),st(s.length,0x29);
		auto at=[](const TileSurfaceLayout&l,uint32_t layer,uint32_t x,uint32_t y){const auto&b=l.texture.block;uint32_t within=0,bank=0;if(!TileGetBlockOffset(b,x%b.block_width,y%b.block_height,0,within)||!TileGetBlockXor(b,x/b.block_width,y/b.block_height,layer,bank))throw std::runtime_error("depth pass oracle");return 256+uint64_t(layer)*l.block_slice_size+uint64_t((y/b.block_height)*(l.mips[0].padded_width/b.block_width)+x/b.block_width)*b.block_size+(within^bank);};
		const uint32_t one=eb==2?65535:0x3f800000u;
		for(uint32_t layer=0;layer<4;++layer)for(uint32_t y=0;y<23;++y)for(uint32_t x=0;x<37;++x)std::memcpy(zt.data()+at(zlayout,layer,x,y),&one,eb);
		std::memcpy(z.contents,zt.data(),zt.size());std::memcpy(s.contents,st.data(),st.size());
		auto memory=[&](id<MTLBuffer>b,uint64_t size){CanonicalMemoryView m;const PhysicalSpan span{0,256,size};if(!m.Initialize(device,std::span(&span,1),size,size+512,[=](uint64_t a,uint64_t)->std::optional<CanonicalMemoryView::BufferSpan>{return CanonicalMemoryView::BufferSpan{b,a};}))throw std::runtime_error("depth pass memory");return m;};
		MetalAttachmentArray za,sa;auto command=ctx.GetCurrentCommandBuffer();
		if(!za.Ensure(device,command,37,23,fmt,4)||!sa.Ensure(device,command,37,23,MTLPixelFormatStencil8,4)){std::fprintf(stderr,"DepthMemoryPass failure line=%d: %s\n",__LINE__,error?error.localizedDescription.UTF8String:"contract");return 1;}
		DepthMemoryTransfer ztfr,stfr;
		if(!ztfr.Initialize(device,za.View(1,2),zlayout,1,2,memory(z,zlayout.total_size),memory(z,zlayout.total_size)) ||
		   !stfr.Initialize(device,sa.View(1,2),slayout,1,2,memory(s,slayout.total_size),memory(s,slayout.total_size))){std::fprintf(stderr,"DepthMemoryPass failure line=%d: %s\n",__LINE__,error?error.localizedDescription.UTF8String:"contract");return 1;}
		if(!za.MarkImported(1,2)||za.NeedsClear(1)||za.NeedsClear(2)||!za.NeedsClear(0)||!za.NeedsClear(3)){std::fprintf(stderr,"DepthMemoryPass failure line=%d: %s\n",__LINE__,error?error.localizedDescription.UTF8String:"contract");return 1;}
		MetalRenderPassDesc pass{};pass.has_depth=pass.has_stencil=true;pass.render_target_array_length=2;
		pass.depth_attachment.texture=za.View(1,2);pass.stencil_attachment.texture=sa.View(1,2);
		pass.depth_attachment.load_action=pass.stencil_attachment.load_action=MTLLoadActionLoad;
		pass.depth_attachment.before_load=[=](id c){if(!ztfr.Load(c))throw std::runtime_error("depth pass load");};
		pass.depth_attachment.after_store=[=](id c){if(!ztfr.Store(c))throw std::runtime_error("depth pass store");};
		pass.stencil_attachment.before_load=[=](id c){if(!stfr.Load(c))throw std::runtime_error("stencil pass load");};
		pass.stencil_attachment.after_store=[=](id c){if(!stfr.Store(c))throw std::runtime_error("stencil pass store");};
		MTLRenderPipelineDescriptor*pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[lib newFunctionWithName:@"v"];pd.fragmentFunction=[lib newFunctionWithName:@"f"];pd.inputPrimitiveTopology=MTLPrimitiveTopologyClassTriangle;pd.depthAttachmentPixelFormat=fmt;pd.stencilAttachmentPixelFormat=MTLPixelFormatStencil8;
		id<MTLRenderPipelineState>pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];if(!pipeline){std::fprintf(stderr,"DepthMemoryPass failure line=%d: %s\n",__LINE__,error?error.localizedDescription.UTF8String:"contract");return 1;}
		for(unsigned phase=0;phase<4;++phase) {
			if(phase==3){pass.depth_attachment.load_action=pass.stencil_attachment.load_action=MTLLoadActionClear;pass.depth_attachment.clear_depth=0.;pass.stencil_attachment.clear_stencil=0x51;}
			if(!draw.BeginRenderPass(pass)){std::fprintf(stderr,"DepthMemoryPass failure line=%d: %s\n",__LINE__,error?error.localizedDescription.UTF8String:"contract");return 1;}
			if(phase==1||phase==2) {
				MTLDepthStencilDescriptor*ds=[MTLDepthStencilDescriptor new];ds.depthCompareFunction=MTLCompareFunctionAlways;ds.depthWriteEnabled=phase==1;
				MTLStencilDescriptor*sd=[MTLStencilDescriptor new];sd.stencilCompareFunction=MTLCompareFunctionAlways;sd.depthStencilPassOperation=MTLStencilOperationReplace;sd.writeMask=phase==1?0xff:0;
				ds.frontFaceStencil=ds.backFaceStencil=sd;
				draw.SetRenderPipeline(pipeline);draw.SetDepthStencil([device newDepthStencilStateWithDescriptor:ds]);draw.SetStencilReferences(0x5a,0x5a);
				draw.SetViewport(0,0,37,23,0,1);draw.SetScissor(phase==1?3:25,2,17,11);draw.Draw(Prospero::PrimitiveType::kTriList,3,2,0,0);
			}
			draw.EndRenderPass();command=ctx.GetCurrentCommandBuffer();ctx.CommitCommandBuffer(true);
			if(command.status!=MTLCommandBufferStatusCompleted){std::fprintf(stderr,"DepthMemoryPass failure line=%d: %s\n",__LINE__,error?error.localizedDescription.UTF8String:"contract");return 1;}
			if(phase==1||phase==3)for(uint32_t layer=1;layer<=2;++layer)for(uint32_t y=0;y<23;++y)for(uint32_t x=0;x<37;++x) {
				if(phase==1&&(x<3||x>=20||y<2||y>=13))continue;
				std::fill_n(zt.data()+at(zlayout,layer,x,y),eb,0);st[at(slayout,layer,x,y)]=phase==1?0x5a:0x51;
			}
			if(std::memcmp(z.contents,zt.data(),zt.size())||std::memcmp(s.contents,st.data(),st.size()))++errors;
			checked+=zt.size()+st.size();++cases;
		}
	}
	std::printf("Depth/stencil canonical passes: load, partial instanced layers, read-only reopen, clear: %u cases, %llu bytes, %u failures\n",cases,(unsigned long long)checked,errors);return errors;
}
