static void CheckDepthAlias(id<MTLDevice> device,id<MTLCommandQueue> queue) {
	constexpr uint32_t w=37,h=23;
	for(uint32_t eb:{2u,4u}) {
		const auto raw_fmt=TileElementFormat(eb),depth_fmt=eb==2?MTLPixelFormatDepth16Unorm:MTLPixelFormatDepth32Float;
		MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth_fmt width:w height:h mipmapped:NO];
		d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
		id<MTLTexture> depth=[device newTextureWithDescriptor:d];Check(depth!=nil,"depth alias source");
		MTLTextureDescriptor*r=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:raw_fmt width:w height:h mipmapped:NO];
		r.storageMode=MTLStorageModeShared;r.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
		id<MTLTexture> raw=[device newTextureWithDescriptor:r],target=[device newTextureWithDescriptor:r];
		Check(raw!=nil&&target!=nil,"depth alias raw images");
		TileSurfaceLayout src{},dst{};
		const auto format=eb==2?Prospero::BufferFormat::k16UInt:Prospero::BufferFormat::k32UInt;
		Check(TileGetTiledTextureLayout({format,Prospero::TileMode::kDepth,TileSurfaceDimension::Dim2D,w,h,1,1,1},src),"depth layout");
		Check(TileGetTiledTextureLayout({format,Prospero::TileMode::kRenderTarget,TileSurfaceDimension::Dim2D,w,h,1,1,1},dst),"depth colour alias layout");
		TileTables st,dt;Check(BuildTileTables(src.texture.block,src.mips[0].padded_width,src.mips[0].padded_height,st)&&BuildTileTables(dst.texture.block,dst.mips[0].padded_width,dst.mips[0].padded_height,dt),"depth tables");
		id<MTLBuffer> guest=[device newBufferWithLength:std::max(src.total_size,dst.total_size) options:MTLResourceStorageModeShared];
		std::vector<uint8_t> truth(guest.length);for(size_t i=0;i<truth.size();++i)truth[i]=uint8_t(i*73+11);std::memcpy(guest.contents,truth.data(),truth.size());
		id<MTLBuffer> scratch=[device newBufferWithLength:guest.length options:MTLResourceStorageModePrivate];
		const NSUInteger row=((w*eb+255)/256)*256;
		id<MTLBuffer> linear=[device newBufferWithLength:row*h options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cmd=[queue commandBuffer];
		MTLRenderPassDescriptor*p=[MTLRenderPassDescriptor renderPassDescriptor];p.depthAttachment.texture=depth;p.depthAttachment.loadAction=MTLLoadActionClear;p.depthAttachment.storeAction=MTLStoreActionStore;p.depthAttachment.clearDepth=1.0;
		id<MTLRenderCommandEncoder> render=[cmd renderCommandEncoderWithDescriptor:p];OrderBegin(render);OrderEnd(render);[render endEncoding];
		id<MTLBlitCommandEncoder>b=[cmd blitCommandEncoder];OrderBegin(b);
		[b copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1) toBuffer:linear destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row*h options:MTLBlitOptionDepthFromDepthStencil];
		[b copyFromBuffer:linear sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row*h sourceSize:MTLSizeMake(w,h,1) toTexture:raw destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];OrderEnd(b);[b endEncoding];
		Check(EncodeTileAlias(device,cmd,guest,0,scratch,raw,st,w,h,target,dt,w,h),"depth bit-layout conversion");
		[cmd commit];[cmd waitUntilCompleted];Check(cmd.status==MTLCommandBufferStatusCompleted,"depth alias GPU completion");
		const auto* linear_bytes=static_cast<const uint8_t*>(linear.contents);
		for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)std::memcpy(truth.data()+MipByteOffset(src,0,0,x,y),linear_bytes+y*row+x*eb,eb);
		std::vector<uint8_t> expected;uint32_t pitch=0;Check(DecodeTiledTextureLevel(truth,dst,0,0,expected,pitch),"depth alias CPU oracle");
		std::vector<uint8_t> actual(expected.size());[target getBytes:actual.data() bytesPerRow:pitch fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
		Check(actual==expected,"depth alias numerical conversion replaced raw bits or lost guest-only bytes");
		const uint32_t bits=eb==2?uint32_t(*static_cast<const uint16_t*>(linear.contents)):*static_cast<const uint32_t*>(linear.contents);
		Check(eb==2?bits==65535:bits==0x3f800000,"depth clear bit representation");
	}
	std::printf("Depth -> raw colour alias: D16/D32 exact tiled bytes + guest-only preservation PASS\n");
}
