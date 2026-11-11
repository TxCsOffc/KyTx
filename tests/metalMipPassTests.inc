// Exercise production pass load/store/reopen hooks, not only standalone kernels.
static unsigned MipPassTransfers(id<MTLDevice> device) {
	using namespace Libs::Graphics;using namespace Libs::Graphics::Metal;
	auto& ctx=MetalContext::Instance();auto& draw=MetalDrawExecutor::Instance();
	unsigned errors=0,cases=0;uint64_t checked=0;
	NSError* error=nil;
	auto library=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;vertex float4 v(uint i[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);}fragment float4 f(){return float4(0);}"
	    options:nil error:&error];
	if(!library)return 1;
	for(auto fmt:{MTLPixelFormatRGBA8Unorm,MTLPixelFormatRG11B10Float,MTLPixelFormatRGBA16Float,MTLPixelFormatRGBA8Unorm_sRGB,MTLPixelFormatBGRA8Unorm_sRGB})
	for(const auto shape:{std::array<uint32_t,3>{97,53,7},std::array<uint32_t,3>{97,53,1},std::array<uint32_t,3>{1,1,1}}) {
		const uint32_t eb=fmt==MTLPixelFormatRGBA16Float?8:4;
		const auto [width,height,levels]=shape;
		TileSurfaceLayout layout{};
		if(!TileGetTiledTextureLayout({eb==8?Prospero::BufferFormat::k32_32UInt:Prospero::BufferFormat::k32UInt,
		    Prospero::TileMode::kRenderTarget,TileSurfaceDimension::Dim2D,width,height,1,levels,1},layout))return 1;
		id<MTLBuffer> guest=[device newBufferWithLength:layout.total_size+512 options:MTLResourceStorageModeShared];
		std::vector<uint8_t> truth(guest.length);
		for(size_t i=0;i<truth.size();++i)truth[i]=uint8_t(i*67u+i/7u+3u);
		std::memcpy(guest.contents,truth.data(),truth.size());
		MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:width height:height mipmapped:levels>1];
		d.mipmapLevelCount=levels;
		d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
		if(fmt!=MTLPixelFormatRGBA8Unorm_sRGB && fmt!=MTLPixelFormatBGRA8Unorm_sRGB)d.usage|=MTLTextureUsageShaderWrite;
		id<MTLTexture> texture=[device newTextureWithDescriptor:d];
		id<MTLTexture> raw=[texture newTextureViewWithPixelFormat:TileElementFormat(eb)];
		if(!texture||!raw)return 1;
		for(uint32_t mip:{0u,3u,6u}) {
			if(mip>=levels)continue;
			MetalRenderPassDesc pass{};pass.color_attachment_count=1;auto& c=pass.color_attachments[0];
			c.texture=texture;c.level=mip;c.load_action=MTLLoadActionLoad;c.store_action=MTLStoreActionStore;
			c.before_load=[=](id cmd){if(!EncodeTileTextureImport(device,cmd,guest,256,raw,layout,mip,0,mip))throw std::runtime_error("pass mip load");};
			c.after_store_regions=[=](id cmd,std::span<const PixelRect> regions){for(auto rect:regions)if(!EncodeTileTextureRectExport(device,cmd,raw,guest,256,layout,rect,mip,0,mip))throw std::runtime_error("pass mip store");};
			// A load/store pass and an interrupted/reopened pass must be bit-exact,
			// including packed floats, NaN payloads, untouched mips and guards.
			if(!draw.BeginRenderPass(pass))return 1;draw.EndRenderPass();
			if(!draw.BeginRenderPass(pass))return 1;draw.EndRenderPass();
			id<MTLCommandBuffer> command=ctx.GetCurrentCommandBuffer();ctx.CommitCommandBuffer(true);
			if(command.status!=MTLCommandBufferStatusCompleted || std::memcmp(guest.contents,truth.data(),truth.size()))++errors;
			checked+=truth.size();++cases;
			// Store of an actual clear affects only this mip's visible elements.
			c.load_action=MTLLoadActionClear;c.clear_color[0]=c.clear_color[1]=c.clear_color[2]=0;c.clear_color[3]=0;
			if(!draw.BeginRenderPass(pass))return 1;draw.EndRenderPass();command=ctx.GetCurrentCommandBuffer();ctx.CommitCommandBuffer(true);
			if(command.status!=MTLCommandBufferStatusCompleted)return 1;
			const auto&p=layout.mips[mip];const auto&b=layout.texture.block;const bool tail=mip>=layout.first_tail_level;
			for(uint32_t y=0;y<p.height;++y)for(uint32_t x=0;x<p.width;++x) {
				uint32_t within=0,bank=0,bx=tail?0:x/b.block_width,by=tail?0:y/b.block_height;
				if(!TileGetBlockOffset(b,tail?x+p.tail_x:x%b.block_width,tail?y+p.tail_y:y%b.block_height,0,within) || !TileGetBlockXor(b,bx,by,bank))return 1;
				uint64_t off=256+p.offset+uint64_t(by*(p.padded_width/b.block_width)+bx)*b.block_size+(within^bank);
				std::fill_n(truth.data()+off,eb,0);
			}
			if(std::memcmp(guest.contents,truth.data(),truth.size()))++errors;checked+=truth.size();++cases;
		}
		if(levels==1) {
			// CPU replacement before an actual partial draw must be loaded too.
			for(size_t i=0;i<truth.size();++i)truth[i]=uint8_t(i*67u+i/7u+3u);
			std::memcpy(guest.contents,truth.data(),truth.size());
			auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[library newFunctionWithName:@"v"];pd.fragmentFunction=[library newFunctionWithName:@"f"];
			pd.colorAttachments[0].pixelFormat=fmt;
			auto pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];if(!pipeline)return 1;
			MetalRenderPassDesc pass{};pass.color_attachment_count=1;auto& c=pass.color_attachments[0];
			c.texture=texture;c.load_action=MTLLoadActionLoad;c.store_action=MTLStoreActionStore;
			c.before_load=[=](id cmd){if(!EncodeTileTextureImport(device,cmd,guest,256,raw,layout))throw std::runtime_error("single mip partial load");};
			c.after_store_regions=[=](id cmd,std::span<const PixelRect> regions){for(auto rect:regions)if(!EncodeTileTextureRectExport(device,cmd,raw,guest,256,layout,rect))throw std::runtime_error("single mip partial store");};
			const uint32_t x0=width==1?0:2,y0=height==1?0:3,w=width==1?1:7,h=height==1?1:5;
			if(!draw.BeginRenderPass(pass))return 1;
			draw.SetRenderPipeline(pipeline);draw.SetViewport(0,0,width,height,0,1);draw.SetScissor(x0,y0,w,h);
			draw.Draw(Prospero::PrimitiveType::kTriList,3,1,0,0);draw.EndRenderPass();
			auto command=ctx.GetCurrentCommandBuffer();ctx.CommitCommandBuffer(true);if(command.status!=MTLCommandBufferStatusCompleted)return 1;
			const auto&m=layout.mips[0];const auto&b=layout.texture.block;
			for(uint32_t y=y0;y<y0+h;++y)for(uint32_t x=x0;x<x0+w;++x) {
				const uint32_t bx=x/b.block_width,by=y/b.block_height;uint32_t within=0,bank=0;
				if(!TileGetBlockOffset(b,x%b.block_width,y%b.block_height,0,within)||!TileGetBlockXor(b,bx,by,bank))return 1;
				const uint64_t offset=256+m.offset+uint64_t(by*(m.padded_width/b.block_width)+bx)*b.block_size+(within^bank);
				std::fill_n(truth.data()+offset,eb,0);
			}
			if(std::memcmp(guest.contents,truth.data(),truth.size())) {
				++errors;unsigned printed=0;
				for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
					uint32_t within=0,bank=0,bx=x/b.block_width,by=y/b.block_height;
					if(!TileGetBlockOffset(b,x%b.block_width,y%b.block_height,0,within)||!TileGetBlockXor(b,bx,by,bank))return 1;
					const uint64_t offset=256+m.offset+uint64_t(by*(m.padded_width/b.block_width)+bx)*b.block_size+(within^bank);
					uint64_t actual=0,wanted=0;std::memcpy(&actual,static_cast<const uint8_t*>(guest.contents)+offset,eb);std::memcpy(&wanted,truth.data()+offset,eb);
					if(actual!=wanted && printed++<8)std::fprintf(stderr,"[SingleMipMismatch] fmt=%lu size=%ux%u pixel=%u,%u inside=%d actual=%llx expected=%llx\n",(unsigned long)fmt,width,height,x,y,x>=x0&&x<x0+w&&y>=y0&&y<y0+h,(unsigned long long)actual,(unsigned long long)wanted);
				}
			}checked+=truth.size();++cases;
		}
	}
	std::printf("Mip render load/store/reopen/clear + single-level/1x1 partial draws: %u cases, %llu physical bytes, %u failures\n",cases,(unsigned long long)checked,errors);
	return errors;
}

static unsigned GraphicsImageWritebackTests(id<MTLDevice> device) {
	using namespace Libs::Graphics;using namespace Libs::Graphics::Metal;
	auto& ctx=MetalContext::Instance();auto& draw=MetalDrawExecutor::Instance();NSError* error=nil;
	id<MTLLibrary> lib=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;vertex float4 v(uint i[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);}fragment float4 f(float4 p[[position]],texture2d<uint,access::write> image[[texture(0)]]){uint2 g=uint2(p.xy);image.write(uint4(0xa5000000u+g.x+17u*g.y),g);return float4(1,0,0,1);}" options:nil error:&error];
	if(!lib)return 1;
	MTLRenderPipelineDescriptor*pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[lib newFunctionWithName:@"v"];pd.fragmentFunction=[lib newFunctionWithName:@"f"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
	id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];if(!pipeline)return 1;
	TileSurfaceLayout layout{};if(!TileGetTiledTextureLayout({Prospero::BufferFormat::k32UInt,Prospero::TileMode::kRenderTarget,TileSurfaceDimension::Dim2D,32,16,1,6,1},layout))return 1;
	id<MTLBuffer> guest=[device newBufferWithLength:layout.total_size options:MTLResourceStorageModeShared];std::vector<uint8_t> truth(guest.length,0x69);std::memcpy(guest.contents,truth.data(),truth.size());
	MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:8 height:4 mipmapped:NO];d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
	id<MTLTexture> image=[device newTextureWithDescriptor:d];d.pixelFormat=MTLPixelFormatRGBA8Unorm;d.usage=MTLTextureUsageRenderTarget;
	id<MTLTexture> color=[device newTextureWithDescriptor:d];if(!image||!color)return 1;
	MetalRenderPassDesc pass{};pass.color_attachment_count=1;pass.color_attachments[0].texture=color;pass.color_attachments[0].load_action=MTLLoadActionClear;
	unsigned copies=0;
	draw.SetGraphicsImageWritebacks({{image,[&](id command){++copies;if(!EncodeTileTextureExport(device,command,image,guest,0,layout,2))throw std::runtime_error("graphics image writeback");}}});
	if(!draw.BeginRenderPass(pass))return 1;
	for(unsigned i=0;i<2;++i) {
		draw.SetRenderPipeline(pipeline);draw.SetFragmentTexture(image,0);draw.SetViewport(0,0,8,4,0,1);draw.SetScissor(0,0,8,4);
		draw.Draw(Prospero::PrimitiveType::kTriList,3,1,0,0);
	}
	draw.EndRenderPass();auto command=ctx.GetCurrentCommandBuffer();ctx.CommitCommandBuffer(true);draw.SetGraphicsImageWritebacks({});
	if(command.status!=MTLCommandBufferStatusCompleted || copies!=1)return 1;
	const auto&b=layout.texture.block;const auto&m=layout.mips[2];
	for(uint32_t y=0;y<4;++y)for(uint32_t x=0;x<8;++x) {
		uint32_t within=0,bank=0;if(!TileGetBlockOffset(b,x+m.tail_x,y+m.tail_y,0,within)||!TileGetBlockXor(b,0,0,bank))return 1;
		const uint32_t value=0xa5000000u+x+17*y;std::memcpy(truth.data()+m.offset+(within^bank),&value,4);
	}
	const unsigned errors=std::memcmp(guest.contents,truth.data(),truth.size())!=0;
	std::printf("Graphics storage mip writeback: 2 real fragment producers, 1 deduplicated export, %zu bytes checked, %u failures\n",truth.size(),errors);return errors;
}
