// Runtime EncodeTileAlias uses the same path as this synthetic GPU program.
// Raw guest bytes outside the writer footprint must survive format/layout changes.
static void CheckAliasPreservation(id<MTLDevice> device,id<MTLCommandQueue> queue) {
	const TileBlockFamily families[]{TileBlockFamily::Standard256B,TileBlockFamily::Standard4KB,TileBlockFamily::Standard64KB,
	    TileBlockFamily::Prt64KB,TileBlockFamily::RenderTarget64KB,TileBlockFamily::Depth64KB};
	const uint32_t element_bytes[]{1,2,4,8,16};
	uint32_t cases=0;
	uint64_t guest_only=0,writer_bytes=0;
	auto address=[](const TileTables& t,uint32_t x,uint32_t y) {
		uint32_t within=0,bank=0;
		Check(TileGetBlockOffset(t.block,x%t.block.block_width,y%t.block.block_height,0,within),"alias reference offset");
		Check(TileGetBlockXor(t.block,x/t.block.block_width,y/t.block.block_height,0,bank),"alias reference bank");
		return uint64_t(y/t.block.block_height*t.columns+x/t.block.block_width)*t.block.block_size+(within^bank);
	};
	for(unsigned f=0;f<6;++f) for(unsigned b=0;b<5;++b) {
		TileBlockLayout source_block{},target_block{};
		const uint32_t source_eb=element_bytes[b],target_eb=element_bytes[(b+2)%5];
		if(!TileGetBlockLayout(families[f],source_eb,source_block) ||
		   !TileGetBlockLayout(families[(f+1)%6],target_eb,target_block)) continue;
		const uint32_t sw=source_block.block_width+3,sh=source_block.block_height/2+1;
		const uint32_t tw=target_block.block_width*2+7,th=target_block.block_height+3;
		TileTables source_tables,target_tables;
		Check(BuildTileTables(source_block,sw,sh,source_tables) && BuildTileTables(target_block,tw,th,target_tables),"alias tables");
		const uint64_t need=std::max(uint64_t(source_tables.columns)*source_tables.rows*source_block.block_size,
		    uint64_t(target_tables.columns)*target_tables.rows*target_block.block_size);
		constexpr NSUInteger prefix=256,guard=256;
		id<MTLBuffer> guest=[device newBufferWithLength:prefix+need+guard options:MTLResourceStorageModeShared];
		id<MTLBuffer> scratch=[device newBufferWithLength:need+guard options:MTLResourceStorageModeShared];
		Check(guest!=nil && scratch!=nil,"alias buffers");
		std::vector<uint8_t> baseline(guest.length),expected(need),source_pixels(uint64_t(sw)*sh*source_eb);
		for(size_t i=0;i<baseline.size();++i) baseline[i]=uint8_t(1+(i*37+i/43+cases*17)%255);
		std::memcpy(guest.contents,baseline.data(),baseline.size());
		std::memcpy(expected.data(),baseline.data()+prefix,need);
		std::memset(scratch.contents,0xcd,scratch.length);
		std::vector<bool> owned(need,false);
		for(size_t i=0;i<source_pixels.size();++i) source_pixels[i]=uint8_t(i*73+i/7+cases*31);
		for(uint32_t y=0;y<sh;++y) for(uint32_t x=0;x<sw;++x) {
			const uint64_t at=address(source_tables,x,y);
			std::memcpy(expected.data()+at,source_pixels.data()+(uint64_t(y)*sw+x)*source_eb,source_eb);
			for(uint32_t k=0;k<source_eb;++k) owned[at+k]=true;
		}
		auto make_texture=[&](uint32_t eb,uint32_t w,uint32_t h) {
			auto* td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:TileElementFormat(eb) width:w height:h mipmapped:NO];
			td.storageMode=MTLStorageModeShared; td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
			return [device newTextureWithDescriptor:td];
		};
		id<MTLTexture> source=make_texture(source_eb,sw,sh),target=make_texture(target_eb,tw,th);
		Check(source!=nil && target!=nil,"alias textures");
		[source replaceRegion:MTLRegionMake2D(0,0,sw,sh) mipmapLevel:0 withBytes:source_pixels.data() bytesPerRow:sw*source_eb];
		id<MTLCommandBuffer> command=[queue commandBuffer];
		if(std::getenv("KYTX_ALIAS_TEST_ZERO_BASE")!=nullptr) {
			// Negative control: reproduce the previous runtime algorithm in the test only.
			id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder]; OrderBegin(blit);
			[blit fillBuffer:scratch range:NSMakeRange(0,need) value:0]; OrderEnd(blit); [blit endEncoding];
			Check(EncodeTileExport(device,command,source,scratch,0,source_tables,sw,sh) &&
			      EncodeTileImport(device,command,scratch,0,target,target_tables,tw,th),"old alias encode");
		} else {
			Check(EncodeTileAlias(device,command,guest,prefix,scratch,source,source_tables,sw,sh,target,target_tables,tw,th),"alias encode");
		}
		[command commit]; [command waitUntilCompleted];
		Check(command.status==MTLCommandBufferStatusCompleted,"alias GPU failure");
		Check(std::memcmp(scratch.contents,expected.data(),need)==0,"alias conversion lost guest bytes outside writer footprint");
		Check(std::memcmp(guest.contents,baseline.data(),baseline.size())==0,"alias conversion modified canonical guest bytes");
		for(size_t i=need;i<scratch.length;++i) Check(static_cast<const uint8_t*>(scratch.contents)[i]==0xcd,"alias scratch guard changed");
		std::vector<uint8_t> actual(uint64_t(tw)*th*target_eb),source_after(source_pixels.size());
		[target getBytes:actual.data() bytesPerRow:tw*target_eb fromRegion:MTLRegionMake2D(0,0,tw,th) mipmapLevel:0];
		[source getBytes:source_after.data() bytesPerRow:sw*source_eb fromRegion:MTLRegionMake2D(0,0,sw,sh) mipmapLevel:0];
		Check(source_after==source_pixels,"alias conversion changed source texture");
		uint64_t update_at=UINT64_MAX,update_pixel=0;
		for(uint32_t y=0;y<th;++y) for(uint32_t x=0;x<tw;++x) {
			const uint64_t at=address(target_tables,x,y);
			Check(std::memcmp(actual.data()+(uint64_t(y)*tw+x)*target_eb,expected.data()+at,target_eb)==0,"reader pixels differ from physical byte oracle");
			for(uint32_t k=0;k<target_eb;++k) {
				if(owned[at+k]) ++writer_bytes;
				else { ++guest_only; update_at=at+k; update_pixel=(uint64_t(y)*tw+x)*target_eb+k; }
			}
		}
		if(update_at!=UINT64_MAX) {
			// Same writer/reader objects, newer byte outside the writer. This
			// must be imported again, even though the writer did not change.
			expected[update_at]^=0x80; baseline[prefix+update_at]^=0x80;
			static_cast<uint8_t*>(guest.contents)[prefix+update_at]^=0x80;
			command=[queue commandBuffer];
			Check(EncodeTileAlias(device,command,guest,prefix,scratch,source,source_tables,sw,sh,target,target_tables,tw,th),"alias refresh encode");
			[command commit]; [command waitUntilCompleted];
			Check(command.status==MTLCommandBufferStatusCompleted && std::memcmp(scratch.contents,expected.data(),need)==0,"alias reused stale guest baseline");
			[target getBytes:actual.data() bytesPerRow:tw*target_eb fromRegion:MTLRegionMake2D(0,0,tw,th) mipmapLevel:0];
			Check(actual[update_pixel]==expected[update_at],"reader did not see newer guest-only byte");
			Check(std::memcmp(guest.contents,baseline.data(),baseline.size())==0,"refresh changed guest bytes");
		}
		// Invalid transfers must be rejected before encoding any mutation.
		std::memset(scratch.contents,0x71,scratch.length);
		command=[queue commandBuffer];
		Check(!EncodeTileAlias(device,command,guest,guest.length,scratch,source,source_tables,sw,sh,target,target_tables,tw,th),"short guest backing accepted");
		Check(!EncodeTileAlias(device,command,guest,prefix,guest,source,source_tables,sw,sh,target,target_tables,tw,th),"scratch aliases guest buffer");
		Check(!EncodeTileAlias(device,command,guest,prefix,scratch,source,source_tables,sw+1,sh,target,target_tables,tw,th),"source extent outside texture accepted");
		[command commit]; [command waitUntilCompleted];
		for(size_t i=0;i<scratch.length;++i) Check(static_cast<const uint8_t*>(scratch.contents)[i]==0x71,"rejected transfer changed scratch");
		++cases;
	}
	Check(cases>20 && guest_only>0 && writer_bytes>0,"alias coverage incomplete");
	std::printf("Tiled alias preservation: %u layout/format pairs, %llu writer bytes / %llu guest-only reader bytes, padding/guards/bounds passed\n",
	    cases,(unsigned long long)writer_bytes,(unsigned long long)guest_only);
}
