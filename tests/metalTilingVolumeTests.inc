static void VolumeTiling(id<MTLDevice> device, id<MTLCommandQueue> queue) {
	const Prospero::BufferFormat formats[] {Prospero::BufferFormat::k8UInt, Prospero::BufferFormat::k16UInt,
	    Prospero::BufferFormat::k32UInt, Prospero::BufferFormat::k32_32UInt, Prospero::BufferFormat::k32_32_32_32UInt};
	unsigned cases = 0, mip_cases = 0;
	for (const auto tile: {Prospero::TileMode::kStandard4KB, Prospero::TileMode::kStandard64KB, Prospero::TileMode::kPrt}) {
		for (const auto format: formats) for (bool small: {false, true}) {
			TileTextureBlockLayout texture_layout {};
			if (!TileGetTextureBlockLayout(format, tile, true, texture_layout)) continue;
			const auto& block = texture_layout.block; const uint32_t eb = block.bytes_per_element;
			const uint32_t w = small ? 3 : block.block_width * 2 + 3;
			const uint32_t h = small ? 5 : block.block_height * 2 + 1;
			const uint32_t d = small ? 7 : block.block_depth * 2 + 1;
			const uint32_t levels = std::min(6u, uint32_t(std::bit_width(std::max({w,h,d}))));
			TileSurfaceLayout layout {};
			const TileSurfaceDescription desc {format,tile,TileSurfaceDimension::Dim3D,w,h,d,levels,1};
			Check(TileGetTiledTextureLayout(desc, layout), "3D layout", "volume", eb);
			if (layout.total_size == 0) continue;
			constexpr NSUInteger prefix = 256, suffix = 256;
			const NSUInteger length = prefix + layout.total_size + suffix;
			id<MTLBuffer> source = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
			id<MTLBuffer> output = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
			std::vector<uint8_t> expected(length, 0xa5);
			std::memset(source.contents, 0x5a, length); std::memset(output.contents, 0xa5, length);
			auto* guest = static_cast<uint8_t*>(source.contents) + prefix;
			for (size_t i = 0; i < layout.total_size; ++i) guest[i] = uint8_t((i * 71 + i / 127 + 11) & 255);
			MTLTextureDescriptor* td = [MTLTextureDescriptor new];
			td.textureType = MTLTextureType3D; td.pixelFormat = TileElementFormat(eb);
			td.width = w; td.height = h; td.depth = d; td.mipmapLevelCount = levels;
			td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
			id<MTLTexture> texture = [device newTextureWithDescriptor:td];
			Check(texture != nil, "3D allocation", "volume", eb); if (texture == nil) continue;
			const MTLPixelFormat native_formats[] {MTLPixelFormatR8Unorm,MTLPixelFormatR16Float,MTLPixelFormatRGBA8Unorm,
			    MTLPixelFormatRGBA16Float,MTLPixelFormatRGBA32Float};
			td.pixelFormat = native_formats[std::countr_zero(eb)]; td.usage |= MTLTextureUsagePixelFormatView;
			if (eb == 4 && small) {
				if (tile == Prospero::TileMode::kStandard64KB) td.pixelFormat = MTLPixelFormatRGB10A2Unorm;
				if (tile == Prospero::TileMode::kPrt) td.pixelFormat = MTLPixelFormatRG11B10Float;
			}
			id<MTLTexture> cpu_parent = [device newTextureWithDescriptor:td];
			id<MTLTexture> cpu_target = [cpu_parent newTextureViewWithPixelFormat:TileElementFormat(eb)];
			Check(cpu_target != nil && UploadTiledVolumeMipChain(std::span<const uint8_t>(guest,layout.total_size),layout,cpu_target),
			      "native 3D full-chain snapshot upload", "volume",eb);
			Check(!UploadTiledVolumeMipChain(std::span<const uint8_t>(guest,layout.total_size-1),layout,cpu_target),
			      "incomplete CPU snapshot rejected", "volume",eb);
			if (small && eb == 4 && tile == Prospero::TileMode::kStandard4KB) {
				td.storageMode = MTLStorageModePrivate;
				id<MTLTexture> private_parent = [device newTextureWithDescriptor:td];
				id<MTLTexture> private_view = [private_parent newTextureViewWithPixelFormat:TileElementFormat(eb)];
				Check(private_view != nil && !UploadTiledVolumeMipChain(std::span<const uint8_t>(guest,layout.total_size),layout,private_view),
				      "CPU snapshot rejects private destination", "volume",eb);
			}
			id<MTLCommandBuffer> cb = [queue commandBuffer];
			for (uint32_t level = 0; level < levels; ++level)
				Check(EncodeTileVolumeImport(device,cb,source,prefix,texture,layout,level,level), "3D GPU import", "volume", eb, level);
			[cb commit]; [cb waitUntilCompleted];
			Check(cb.status == MTLCommandBufferStatusCompleted, "3D import completed", "volume", eb);
			cb = [queue commandBuffer];
			for (uint32_t level = 0; level < levels; ++level) {
				const auto& mip = layout.mips[level]; const uint32_t depth = std::max(d >> level, 1u);
				const bool tail = level >= layout.first_tail_level;
				std::vector<uint8_t> cpu, gpu(size_t(mip.width) * mip.height * depth * eb);
				Check(DecodeTiledVolumeLevel(std::span<const uint8_t>(guest,layout.total_size),layout,level,cpu), "3D CPU decode", "volume", eb, level);
				[texture getBytes:gpu.data() bytesPerRow:mip.width*eb bytesPerImage:mip.width*mip.height*eb
				      fromRegion:MTLRegionMake3D(0,0,0,mip.width,mip.height,depth) mipmapLevel:level slice:0];
				Check(cpu == gpu, "3D import matches CPU addressing", "volume", eb, level);
				std::vector<uint8_t> uploaded(gpu.size());
				[cpu_target getBytes:uploaded.data() bytesPerRow:mip.width*eb bytesPerImage:mip.width*mip.height*eb
				           fromRegion:MTLRegionMake3D(0,0,0,mip.width,mip.height,depth) mipmapLevel:level slice:0];
				Check(uploaded == cpu, "native texture/view contains every guest mip", "volume",eb,level);
				[cpu_parent getBytes:uploaded.data() bytesPerRow:mip.width*eb bytesPerImage:mip.width*mip.height*eb
				           fromRegion:MTLRegionMake3D(0,0,0,mip.width,mip.height,depth) mipmapLevel:level slice:0];
				Check(uploaded == cpu, "parent texture sees uint-view mip upload", "volume",eb,level);
				// Full allocation comparison catches writes into other mips, tail
				// padding, the next z slab, and guards around the guest allocation.
				for (uint32_t z=0; z<depth; ++z) for (uint32_t y=0; y<mip.height; ++y) for (uint32_t x=0; x<mip.width; ++x) {
					const uint32_t bx=tail ? 0 : x/block.block_width, by=tail ? 0 : y/block.block_height, bz=z/block.block_depth;
					uint32_t within=0, bank=0;
					TileGetBlockOffset(block,tail ? x+mip.tail_x : x%block.block_width,tail ? y+mip.tail_y : y%block.block_height,z%block.block_depth,within);
					TileGetBlockXor(block,bx,by,bz,bank);
					const uint64_t address=uint64_t(bz)*layout.block_slice_size+mip.offset+
					    uint64_t(by*(mip.padded_width/block.block_width)+bx)*block.block_size+(within^bank);
					std::memcpy(expected.data()+prefix+address,guest+address,eb);
				}
				Check(EncodeTileVolumeExport(device,cb,texture,output,prefix,layout,level,level), "3D GPU export", "volume", eb, level);
				++mip_cases;
			}
			[cb commit]; [cb waitUntilCompleted];
			Check(cb.status == MTLCommandBufferStatusCompleted, "3D export completed", "volume", eb);
			Check(std::memcmp(output.contents,expected.data(),length)==0, "3D export preserves full allocation and guards", "volume", eb);
			id<MTLCommandBuffer> invalid = [queue commandBuffer];
			Check(!EncodeTileVolumeImport(device,invalid,source,prefix,texture,layout,levels,0), "invalid guest mip rejected", "volume", eb);
			Check(!EncodeTileVolumeExport(device,invalid,texture,output,prefix,layout,0,levels), "invalid host mip rejected", "volume", eb);
			Check(!EncodeTileVolumeImport(device,invalid,source,length,texture,layout), "truncated volume rejected", "volume", eb);
			std::vector<uint8_t> bad {1};
			Check(!DecodeTiledVolumeLevel({},layout,0,bad) && bad.empty(), "truncated CPU volume rejected", "volume", eb);
			++cases;
		}
	}
	// A wide 2D bank table exceeds Metal's 4-KiB setBytes limit.
	TileBlockLayout block {}; TileGetBlockLayout(TileBlockFamily::Standard256B,16,block);
	TileTables tables; BuildTileTables(block,8192,5,tables);
	const size_t bytes=size_t(tables.columns)*tables.rows*block.block_size;
	id<MTLBuffer> source=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
	std::memset(source.contents,0x6b,bytes);
	auto* td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint width:8192 height:5 mipmapped:NO];
	td.storageMode=MTLStorageModeShared; td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
	id<MTLTexture> target=[device newTextureWithDescriptor:td];
	id<MTLCommandBuffer> cb=[queue commandBuffer];
	Check(EncodeTileImport(device,cb,source,0,target,tables,8192,5), "large bank table import", "wide",16);
	[cb commit]; [cb waitUntilCompleted];
	Check(cb.status==MTLCommandBufferStatusCompleted, "large bank table GPU completed", "wide",16);
	std::vector<uint8_t> readback(8192*5*16);
	[target getBytes:readback.data() bytesPerRow:8192*16 fromRegion:MTLRegionMake2D(0,0,8192,5) mipmapLevel:0];
	Check(std::all_of(readback.begin(),readback.end(),[](uint8_t b){return b==0x6b;}), "wide import exact", "wide",16);
	std::printf("volume tiling: %u volumes, %u GPU mip round-trips, wide bank table, %d cumulative failures\n",cases,mip_cases,s_failures);
}
