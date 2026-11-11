static void CheckPhysicalSpans(id<MTLDevice> device,id<MTLCommandQueue> queue) {
	std::vector<PhysicalSpan> spans;
	const std::vector<PhysicalMapping> adjacent{{0x1000,0x2000,0x7000},{0x3000,0x3000,0x9000},{0x6000,0x1000,0xc000}};
	Check(ResolvePhysicalSpans(adjacent,0x1800,0x5000,spans) && spans.size()==1 && spans[0].physical==0x7800 && spans[0].size==0x5000,"adjacent mappings merge");
	Check(!ResolvePhysicalSpans(adjacent,0x800,0x1000,spans)&&spans.empty(),"leading mapping gap");
	Check(!ResolvePhysicalSpans(adjacent,0x6800,0x1000,spans)&&spans.empty(),"trailing mapping gap");
	Check(!ResolvePhysicalSpans(adjacent,UINT64_MAX-2,8,spans)&&spans.empty(),"mapping overflow");
	TileSurfaceLayout layout{};
	Check(TileGetTiledTextureLayout({Prospero::BufferFormat::k32UInt,Prospero::TileMode::kRenderTarget,TileSurfaceDimension::Dim2D,129,97,1,8,2},layout),"span tiled layout");
	const uint64_t size=layout.total_size,part=size/4;
	const std::vector<PhysicalMapping> maps{{0x100000,part,size+part},{0x100000+part,part*2,part},{0x100000+part*3,part,0}};
	Check(ResolvePhysicalSpans(maps,0x100000,size,spans)&&spans.size()==3,"discontiguous mapping resolve");
	id<MTLBuffer> physical=[device newBufferWithLength:size*2+512 options:MTLResourceStorageModeShared];
	Check(physical!=nil,"span physical buffer");
	std::vector<uint8_t> truth(physical.length),logical(size);
	for(size_t i=0;i<truth.size();++i)truth[i]=uint8_t(i*37+i/17+5);
	std::memcpy(physical.contents,truth.data(),truth.size());
	for(const auto&s:spans)std::memcpy(logical.data()+s.offset,truth.data()+s.physical,s.size);
	CanonicalMemoryView memory;
	Check(memory.Initialize(device,spans,size,65536,[&](uint64_t p,uint64_t n)->std::optional<CanonicalMemoryView::BufferSpan> {
		if(p>physical.length||n>physical.length-p)return std::nullopt;return CanonicalMemoryView::BufferSpan{physical,p};
	}),"span GPU staging plan");
	Check(memory.buffer!=physical,"discontiguous mappings need a contiguous tile-kernel staging buffer");
	constexpr uint32_t mip=3,layer=1;const auto&m=layout.mips[mip];
	MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:m.width height:m.height mipmapped:NO];
	d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
	id<MTLTexture> raw=[device newTextureWithDescriptor:d];Check(raw!=nil,"span raw mip");
	std::vector<uint32_t> values(size_t(m.width)*m.height);
	for(uint32_t y=0;y<m.height;++y)for(uint32_t x=0;x<m.width;++x)values[size_t(y)*m.width+x]=0x12340000u+y*123+x*17;
	[raw replaceRegion:MTLRegionMake2D(0,0,m.width,m.height) mipmapLevel:0 withBytes:values.data() bytesPerRow:m.width*4];
	std::vector<FootprintRange> footprint;Check(TiledMipFootprint(layout,mip,layer,footprint),"span mip footprint");
	uint64_t visible=0;for(const auto&r:footprint)visible+=r.size;
	Check(visible==uint64_t(m.width)*m.height*4,"mip footprint owns padding");
	id<MTLCommandBuffer> cmd=[queue commandBuffer];Check(memory.Gather(cmd),"span GPU gather");
	// A newer raw-buffer write to padding AFTER the gather must survive scatter.
	uint64_t untouched=0;
	while(std::any_of(footprint.begin(),footprint.end(),[&](auto r){return untouched>=r.begin&&untouched<r.begin+r.size;}))++untouched;
	uint64_t phys_untouched=0;for(const auto&s:spans)if(untouched>=s.offset&&untouched<s.offset+s.size)phys_untouched=s.physical+untouched-s.offset;
	id<MTLBlitCommandEncoder> e=[cmd blitCommandEncoder];OrderBegin(e);[e fillBuffer:physical range:NSMakeRange(phys_untouched,1) value:0xe7];OrderEnd(e);[e endEncoding];truth[phys_untouched]=0xe7;
	Check(EncodeTileTextureExport(device,cmd,raw,memory.buffer,memory.offset,layout,mip,layer),"span tile export");
	Check(memory.Scatter(cmd,footprint),"span footprint scatter");
	for(uint32_t y=0;y<m.height;++y)for(uint32_t x=0;x<m.width;++x) {
		const uint64_t at=MipByteOffset(layout,mip,layer,x,y);const uint32_t value=values[size_t(y)*m.width+x];
		for(uint32_t b=0;b<4;++b)for(const auto&s:spans)if(at+b>=s.offset&&at+b<s.offset+s.size)truth[s.physical+at+b-s.offset]=uint8_t(value>>(b*8));
	}
	const FootprintRange invalid{size-1,2};Check(!memory.Scatter(cmd,std::span(&invalid,1)),"invalid scatter accepted");
	[cmd commit];[cmd waitUntilCompleted];Check(cmd.status==MTLCommandBufferStatusCompleted,"span GPU execution");
	Check(!std::memcmp(physical.contents,truth.data(),truth.size()),"span scatter changed other mip/padding/new raw bytes/physical guards");
	std::printf("Physical spans: adjacent + discontiguous mappings, chunk crossings, 4 negative controls; %llu bytes preserved PASS\n",(unsigned long long)truth.size());
}
