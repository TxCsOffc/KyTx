static void CheckRemappedImageBacking(id<MTLDevice> device,id<MTLCommandQueue> queue) {
	constexpr uint64_t va=0x8100000000ull,chunk=65536;
	TileSurfaceLayout old_layout{},reader{};
	Check(TileGetTiledTextureLayout({Prospero::BufferFormat::k32UInt,Prospero::TileMode::kStandard4KB,TileSurfaceDimension::Dim2D,128,64,1,8,1},old_layout),"old remap layout");
	Check(TileGetTiledTextureLayout({Prospero::BufferFormat::k32UInt,Prospero::TileMode::kStandard64KB,TileSurfaceDimension::Dim2D,37,21,1,1,1},reader),"new remap layout");
	Check(old_layout.total_size<=chunk && reader.total_size==chunk,"remap control extents");
	std::vector<PhysicalMapping> original{{va,chunk,0}},remapped{{va,chunk,chunk}};
	PhysicalBackingSnapshot old;
	Check(old.Capture(original,va,old_layout.total_size),"capture render physical identity");
	Check(old.Matches(original,va),"original physical identity");
	auto unrelated=original;unrelated.push_back({va+2*chunk,chunk,3*chunk});
	Check(old.Matches(unrelated,va),"unrelated mapping must not invalidate render identity");
	std::vector<PhysicalMapping> split{{va,16384,0},{va+16384,chunk-16384,16384}};
	Check(old.Matches(split,va),"contiguous mapping split must not invalidate image identity");
	std::vector<PhysicalMapping> alias{{va+4*chunk,chunk,0}};
	Check(old.Matches(alias,va+4*chunk),"same physical allocation through another VA");
	Check(!old.Matches(remapped,va),"VA reuse must invalidate the old render image");
	auto partial=split;partial[1].backing_offset+=chunk;
	Check(!old.Matches(partial,va),"partial physical remap must invalidate image identity");
	std::vector<PhysicalMapping> gap{{va,16384,0}};
	Check(!old.Matches(gap,va),"missing current mapping cannot validate the old image");
	PhysicalBackingSnapshot unknown;
	Check(!unknown.Valid() && !unknown.Matches(original,va),"untracked producer has no physical proof");
	const ImageWriterIdentity rendered{1,10};
	const CanonicalImageHistory rendered_writer{unknown,rendered,false};
	std::vector<CanonicalImageHistory> history{{old,rendered,true}};
	Check(CanImportRemappedImage(history,remapped,va,rendered_writer),"storage bind must import remapped backing before creating a detached writer");
	Check(CanImportRemappedImage(history,remapped,va,std::nullopt),"retired canonical image without a newer writer");
	Check(!CanImportRemappedImage(history,original,va,rendered_writer),"unchanged allocation is not remapped");
	Check(!CanImportRemappedImage(history,remapped,va,CanonicalImageHistory{unknown,{2,11},false}),"newer detached compute writer requires its own materialization");
	Check(!CanImportRemappedImage(history,remapped,va,CanonicalImageHistory{unknown,{1,11},false}),"texture pointer equality cannot retire a newer writer serial");
	const CanonicalImageHistory computed{old,{2,11},true};
	Check(CanImportRemappedImage(history,remapped,va,computed),"canonical storage writer with tiled writeback to the retired allocation must not block remap");
	Check(CanImportRemappedImage(history,partial,va,computed),"canonical storage writer survives partial physical remap proof");
	PhysicalBackingSnapshot current;Check(current.Capture(remapped,va,old_layout.total_size),"capture current storage allocation");
	Check(!CanImportRemappedImage(history,remapped,va,CanonicalImageHistory{current,{2,12},true}),"current canonical writer is not a retired allocation");
	Check(!CanImportRemappedImage(history,remapped,va,CanonicalImageHistory{unknown,{2,11},true}),"canonical tag without physical bytes cannot retire a writer");
	Check(!CanImportRemappedImage(history,remapped,va,CanonicalImageHistory{old,{2,11},false}),"physical identity alone cannot prove writeback");
	history[0].canonical=false;
	Check(!CanImportRemappedImage(history,remapped,va,rendered_writer),"legacy producer cannot authorize a guest-memory import");
	history[0]={unknown,rendered,true};
	Check(!CanImportRemappedImage(history,remapped,va,rendered_writer),"unknown physical identity cannot authorize an import");
	Check(!CanImportRemappedImage({},remapped,va,std::nullopt),"missing producer cannot authorize an import");

	auto physical=[device newBufferWithLength:chunk*2+256 options:MTLResourceStorageModeShared];
	std::memset(physical.contents,0x11,chunk);std::memset(static_cast<uint8_t*>(physical.contents)+chunk,0x5a,chunk+256);
	std::vector<PhysicalSpan> spans;Check(ResolvePhysicalSpans(remapped,va,reader.total_size,spans),"resolve new sampled allocation");
	CanonicalMemoryView current_memory;
	Check(current_memory.Initialize(device,spans,reader.total_size,chunk,[&](uint64_t p,uint64_t n)->std::optional<CanonicalMemoryView::BufferSpan>{
		if(p>physical.length || n>physical.length-p)return std::nullopt;
		return CanonicalMemoryView::BufferSpan{physical,p};
	}),"materialize remapped canonical backing");
	auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:37 height:21 mipmapped:NO];
	td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsagePixelFormatView;
	auto image=[device newTextureWithDescriptor:td];Check(image!=nil,"remapped reader image");
	auto sampled=[device newTextureWithDescriptor:td];Check(sampled!=nil,"remapped storage consumer image");
	NSError* error=nil;
	auto library=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void store(texture2d<uint,access::write> t [[texture(0)]],uint2 p [[thread_position_in_grid]]){t.write(uint4(0x52000000u+p.x+101u*p.y),p);}"
	    options:nil error:&error];
	Check(library!=nil,"remapped storage shader compilation");
	auto pipeline=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"store"] error:&error];
	Check(pipeline!=nil,"remapped storage pipeline");
	auto read=[device newBufferWithLength:256*21 options:MTLResourceStorageModeShared];
	uint64_t checked=0;
	for(unsigned pass=0;pass<3;++pass) {
		if(pass==1){const uint32_t value=0xdeadbeef;std::memcpy(static_cast<uint8_t*>(physical.contents)+chunk+MipByteOffset(reader,0,0,0,0),&value,4);}
		auto command=[queue commandBuffer];
		if(pass==0){auto b=[command blitCommandEncoder];[b fillBuffer:physical range:NSMakeRange(chunk,chunk) value:0x73];[b endEncoding];}
		// No CPU readback/wait between the GPU producer and sampled import.
		Check(current_memory.Gather(command) && EncodeTileTextureImport(device,command,current_memory.buffer,current_memory.offset,image,reader,0),"remapped image GPU import");
		id<MTLTexture> result=image;
		if(pass==2) {
			auto e=[command computeCommandEncoder];OrderBegin(e);
			[e setComputePipelineState:pipeline];[e setTexture:image atIndex:0];
			[e dispatchThreads:MTLSizeMake(37,21,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
			OrderEnd(e);[e endEncoding];
			std::vector<FootprintRange> footprint;
			Check(TiledMipFootprint(reader,0,0,footprint) && EncodeTileTextureExport(device,command,image,current_memory.buffer,current_memory.offset,reader,0) &&
			    current_memory.Scatter(command,footprint),"remapped storage writeback");
			Check(current_memory.Gather(command) && EncodeTileTextureImport(device,command,current_memory.buffer,current_memory.offset,sampled,reader,0),"reader after remapped storage writeback");
			result=sampled;
		}
		auto b=[command blitCommandEncoder];
		[b copyFromTexture:result sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(37,21,1) toBuffer:read destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256*21];
		[b endEncoding];[command commit];[command waitUntilCompleted];Check(command.status==MTLCommandBufferStatusCompleted,"remapped image GPU execution");
		for(unsigned y=0;y<21;++y)for(unsigned x=0;x<37;++x) {
			uint32_t value;std::memcpy(&value,static_cast<uint8_t*>(read.contents)+y*256+x*4,4);
			const uint32_t expected=pass==2?0x52000000u+x+101u*y:(pass==1&&x==0&&y==0)?0xdeadbeefu:0x73737373u;
			Check(value==expected,"reader sampled old allocation or missed current bytes");checked+=4;
		}
		for(uint64_t i=0;i<chunk;++i)Check(static_cast<const uint8_t*>(physical.contents)[i]==0x11,"reader changed the retired physical allocation");
		for(uint64_t i=2*chunk;i<physical.length;++i)Check(static_cast<const uint8_t*>(physical.contents)[i]==0x5a,"remap reader damaged allocation guard");
		checked+=chunk+256;
	}
	std::vector<PhysicalSpan> missing;
	Check(!ResolvePhysicalSpans(gap,va,reader.total_size,missing) && missing.empty(),"unmapped sampled range must fail atomically");
	std::printf("Remapped image: raw GPU/CPU updates + image store -> tiled writeback -> reader in one command buffer, 8 identity + 13 writer controls, %llu checked bytes PASS\n",(unsigned long long)checked);
}
