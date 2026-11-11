static void CheckRectPhysicalTransfers(id<MTLDevice> device,id<MTLCommandQueue> queue) {
	TileSurfaceLayout layout{};
	Check(TileGetTiledTextureLayout({Prospero::BufferFormat::k32UInt,Prospero::TileMode::kRenderTarget,
	    TileSurfaceDimension::Dim2D,321,193,1,9,3},layout),"rectangle physical layout");
	const uint64_t split=layout.total_size/2,second=layout.total_size*2;
	const std::vector<PhysicalSpan> spans{{0,0,split},{split,second,layout.total_size-split}};
	auto a=[device newBufferWithLength:split+512 options:MTLResourceStorageModeShared];
	auto b=[device newBufferWithLength:layout.total_size-split+512 options:MTLResourceStorageModeShared];
	CanonicalMemoryView memory;
	Check(memory.Initialize(device,spans,layout.total_size,layout.total_size*4,[&](uint64_t p,uint64_t n)->std::optional<CanonicalMemoryView::BufferSpan>{
		if(p<split && n<=split-p)return CanonicalMemoryView::BufferSpan{a,256+p};
		if(p>=second && p-second<=b.length-512 && n<=b.length-512-(p-second))return CanonicalMemoryView::BufferSpan{b,256+p-second};
		return std::nullopt;
	}),"rectangle discontiguous physical view");
	NSError* error=nil;
	auto library=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;kernel void paint(texture2d<uint,access::write> t [[texture(0)]],uint2 p [[thread_position_in_grid]]){t.write(uint4(0xa5000000u+p.x+101u*p.y),p);}"
	    options:nil error:&error];Check(library!=nil,"rectangle GPU shader");
	auto pipeline=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"paint"] error:&error];Check(pipeline!=nil,"rectangle GPU pipeline");
	uint64_t checked=0;unsigned cases=0,negatives=0;
	for(const auto selection:{std::array<uint32_t,2>{0,1},std::array<uint32_t,2>{3,2},std::array<uint32_t,2>{8,0}}) {
		const auto [level,layer]=selection;const auto& mip=layout.mips[level];
		const PixelRect rect=mip.width>8 && mip.height>6?PixelRect{3,2,mip.width-7,mip.height-5}:PixelRect{0,0,mip.width,mip.height};
		auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:mip.width height:mip.height mipmapped:NO];
		td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
		auto texture=[device newTextureWithDescriptor:td];Check(texture!=nil,"rectangle GPU image");
		std::memset(a.contents,0x97,a.length);std::memset(b.contents,0x97,b.length);
		std::memset(static_cast<uint8_t*>(a.contents)+256,0x5a,split);std::memset(static_cast<uint8_t*>(b.contents)+256,0x5a,layout.total_size-split);
		std::vector<uint8_t> expected(layout.total_size,0x37);
		for(uint32_t y=rect.y;y<rect.y+rect.height;++y)for(uint32_t x=rect.x;x<rect.x+rect.width;++x) {
			const uint32_t value=0xa5000000u+x+101u*y;std::memcpy(expected.data()+MipByteOffset(layout,level,layer,x,y),&value,4);
		}
		auto command=[queue commandBuffer];Check(memory.Gather(command),"rectangle gather");
		// Raw GPU writes after gather must survive outside the export region.
		auto blit=[command blitCommandEncoder];OrderBegin(blit);
		[blit fillBuffer:a range:NSMakeRange(256,split) value:0x37];[blit fillBuffer:b range:NSMakeRange(256,layout.total_size-split) value:0x37];OrderEnd(blit);[blit endEncoding];
		auto encoder=[command computeCommandEncoder];OrderBegin(encoder);[encoder setComputePipelineState:pipeline];[encoder setTexture:texture atIndex:0];
		[encoder dispatchThreads:MTLSizeMake(mip.width,mip.height,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];OrderEnd(encoder);[encoder endEncoding];
		std::vector<FootprintRange> footprint;
		Check(TiledMipRectFootprint(layout,level,layer,rect,footprint) &&
		    EncodeTileTextureRectExport(device,command,texture,memory.buffer,memory.offset,layout,rect,level,layer) && memory.Scatter(command,footprint),"rectangle export and exact physical scatter");
		for(const PixelRect bad: {PixelRect{mip.width,0,1,1},PixelRect{0,mip.height,1,1},PixelRect{0,0,0,1},PixelRect{UINT32_MAX,0,2,1}}) {
			Check(!EncodeTileTextureRectExport(device,command,texture,memory.buffer,memory.offset,layout,bad,level,layer),"invalid rectangle accepted");
			Check(!TiledMipRectFootprint(layout,level,layer,bad,footprint) && footprint.empty(),"invalid rectangle footprint accepted");++negatives;
		}
		[command commit];[command waitUntilCompleted];Check(command.status==MTLCommandBufferStatusCompleted,"rectangle GPU execution");
		Check(std::memcmp(static_cast<const uint8_t*>(a.contents)+256,expected.data(),split)==0 &&
		    std::memcmp(static_cast<const uint8_t*>(b.contents)+256,expected.data()+split,layout.total_size-split)==0,"rectangle scatter damaged newer raw bytes or neighbouring mips/layers");
		for(auto buffer:{a,b})for(NSUInteger i=0;i<256;++i)Check(static_cast<const uint8_t*>(buffer.contents)[i]==0x97 && static_cast<const uint8_t*>(buffer.contents)[buffer.length-1-i]==0x97,"rectangle changed allocation guards");
		checked+=layout.total_size+1024;++cases;
	}
	std::printf("Rectangle physical export: %u GPU cases, %u negatives, %llu exact bytes, tails/layer XOR/discontiguous scatter/newer raw writes PASS\n",cases,negatives,(unsigned long long)checked);
}
