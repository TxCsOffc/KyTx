// CPU address oracle uses the scalar guest tiler, independently of GPU GF(2) tables.
static uint64_t MipByteOffset(const TileSurfaceLayout& l, uint32_t m, uint32_t layer, uint32_t x, uint32_t y) {
	const auto& p=l.mips[m]; const auto& b=l.texture.block; const bool tail=m>=l.first_tail_level;
	const uint32_t bx=tail?0:x/b.block_width, by=tail?0:y/b.block_height;
	uint32_t within=0,bank=0;
	Check(TileGetBlockOffset(b,tail?x+p.tail_x:x%b.block_width,tail?y+p.tail_y:y%b.block_height,0,within),"mip oracle within");
	Check(TileGetBlockXor(b,bx,by,layer,bank),"mip oracle bank");
	return uint64_t(layer)*l.block_slice_size+p.offset+uint64_t(by*(p.padded_width/b.block_width)+bx)*b.block_size+(within^bank);
}

static void CheckMipTransfers(id<MTLDevice> device,id<MTLCommandQueue> queue) {
	NSError* error=nil;
	id<MTLLibrary> lib=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void patch(texture2d<uint,access::write> t [[texture(0)]],constant uint2& sm [[buffer(0)]],uint2 g [[thread_position_in_grid]]) { if(g.x>=t.get_width()||g.y>=t.get_height()||((g.x+g.y)%3u)==0u)return; uint v=sm.x+g.x*73u+g.y*997u;t.write(uint4(v,v^0xabcdef01u,v+0x12345678u,~v)&sm.y,g); }\nkernel void bytes(device uchar* p [[buffer(0)]],uint i [[thread_position_in_grid]]){if(i<127u)p[i*19u]=uchar(i*37u+5u);}" options:nil error:&error];
	Check(lib!=nil,"mip patch library");
	id<MTLComputePipelineState> patch=[device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"patch"] error:&error];
	id<MTLComputePipelineState> bytes=[device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"bytes"] error:&error];
	Check(patch!=nil && bytes!=nil,"mip patch pipelines");
	uint64_t checked=0,texels=0; unsigned cases=0,negatives=0;
	const struct {uint32_t w,h,levels,layers,eb,tile;} configs[]={
		{97,53,7,3,4,27},{31,19,5,2,8,27},{19,11,5,2,16,27},
		{43,29,6,3,2,24},{67,35,7,2,1,1},{51,27,6,2,4,5},
		{37,21,6,3,8,9},{29,17,5,2,16,17},{1920,1080,11,1,4,27}};
	for(const auto c:configs) {
		// Representative formats are used only for element widths, never colour conversion.
		const auto format=c.eb==1?Prospero::BufferFormat::k8UInt:c.eb==2?Prospero::BufferFormat::k16UInt:
		    c.eb==4?Prospero::BufferFormat::k32UInt:c.eb==8?Prospero::BufferFormat::k32_32UInt:Prospero::BufferFormat::k32_32_32_32UInt;
		TileSurfaceLayout layout{},reader{};
		Check(TileGetTiledTextureLayout({format,static_cast<Prospero::TileMode>(c.tile),TileSurfaceDimension::Dim2D,c.w,c.h,1,c.levels,c.layers},layout),"mip writer layout");
		const auto rf=c.eb==8?Prospero::BufferFormat::k32UInt:Prospero::BufferFormat::k32_32UInt;
		Check(TileGetTiledTextureLayout({rf,Prospero::TileMode::kRenderTarget,TileSurfaceDimension::Dim2D,c.w,c.h,1,1,1},reader),"mip reader layout");
		constexpr NSUInteger prefix=256,guard=512;
		const NSUInteger extent=std::max(layout.total_size,reader.total_size);
		id<MTLBuffer> guest=[device newBufferWithLength:prefix+extent+guard options:MTLResourceStorageModeShared];
		Check(guest!=nil,"mip guest allocation");
		std::vector<uint8_t> truth(guest.length);
		for(size_t i=0;i<truth.size();++i)truth[i]=uint8_t(i*71u+i/23u+13u);
		std::memcpy(guest.contents,truth.data(),truth.size());
		MTLTextureDescriptor* d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:TileElementFormat(c.eb) width:c.w height:c.h mipmapped:YES];
		d.textureType=MTLTextureType2DArray;d.arrayLength=c.layers;d.mipmapLevelCount=c.levels;
		d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsagePixelFormatView;
		id<MTLTexture> texture=[device newTextureWithDescriptor:d];Check(texture!=nil,"mip array texture");
		id<MTLCommandBuffer> cmd=[queue commandBuffer];
		for(uint32_t layer=0;layer<c.layers;++layer)for(uint32_t m=0;m<c.levels;++m) {
			std::vector<FootprintRange> owned;
			Check(TiledMipFootprint(layout,m,layer,owned),"mip ownership footprint");
			uint64_t owned_bytes=0;for(const auto& r:owned)owned_bytes+=r.size;
			Check(owned_bytes==uint64_t(layout.mips[m].width)*layout.mips[m].height*c.eb,"mip footprint includes padding");
			for(uint32_t y=0;y<layout.mips[m].height;++y)for(uint32_t x=0;x<layout.mips[m].width;++x) {
				const auto at=MipByteOffset(layout,m,layer,x,y);
				auto it=std::upper_bound(owned.begin(),owned.end(),at,[](uint64_t p,auto r){return p<r.begin;});
				Check(it!=owned.begin(),"mip footprint misses first element");--it;
				Check(at>=it->begin && at+c.eb<=it->begin+it->size,"mip footprint misses element bytes");
			}
			Check(EncodeTileTextureImport(device,cmd,guest,prefix,texture,layout,m,layer,m,layer),"mip GPU import");
			// Deliberately skip alternate mips: no export is allowed to steal their shared tail bytes.
			if((m+layer)%2u==0u)continue;
			id<MTLTexture> view=[texture newTextureViewWithPixelFormat:texture.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(m,1) slices:NSMakeRange(layer,1)];
			const uint32_t seed=0x87654321u+m*0x150000u+layer*0x100u;
			id<MTLComputeCommandEncoder> enc=[cmd computeCommandEncoder];OrderBegin(enc);
			[enc setComputePipelineState:patch];[enc setTexture:view atIndex:0];// Metal saturates out-of-range writes to 8/16-bit uint formats; keep the value inside the element width.
			const uint32_t seed_mask[2]={seed,c.eb==1?0xffu:c.eb==2?0xffffu:0xffffffffu};
			[enc setBytes:seed_mask length:8 atIndex:0];
			[enc dispatchThreads:MTLSizeMake(view.width,view.height,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];OrderEnd(enc);[enc endEncoding];
			Check(EncodeTileTextureExport(device,cmd,texture,guest,prefix,layout,m,layer,m,layer),"mip GPU export");
			for(uint32_t y=0;y<view.height;++y)for(uint32_t x=0;x<view.width;++x)if((x+y)%3u) {
				const uint32_t v=seed+x*73u+y*997u,words[]={v,v^0xabcdef01u,v+0x12345678u,~v};
				const auto off=prefix+MipByteOffset(layout,m,layer,x,y);
				for(uint32_t b=0;b<c.eb;++b)truth[off+b]=uint8_t(words[b/std::min(c.eb,4u)]>>((b%std::min(c.eb,4u))*8));
			}
		}
		// A later raw-buffer producer overwrites some of the same physical bytes.
		id<MTLComputeCommandEncoder> enc=[cmd computeCommandEncoder];OrderBegin(enc);
		[enc setComputePipelineState:bytes];[enc setBuffer:guest offset:prefix atIndex:0];
		[enc dispatchThreads:MTLSizeMake(127,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];OrderEnd(enc);[enc endEncoding];
		for(uint32_t i=0;i<127;++i)truth[prefix+i*19]=uint8_t(i*37u+5u);
		MTLTextureDescriptor* rd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:TileElementFormat(reader.texture.block.bytes_per_element) width:c.w height:c.h mipmapped:NO];
		rd.storageMode=MTLStorageModeShared;rd.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
		id<MTLTexture> target=[device newTextureWithDescriptor:rd];Check(target!=nil,"mip alias reader");
		Check(EncodeTileTextureImport(device,cmd,guest,prefix,target,reader),"different-size alias import");
		[cmd commit];[cmd waitUntilCompleted];Check(cmd.status==MTLCommandBufferStatusCompleted,"mip GPU execution");
		if(std::memcmp(guest.contents,truth.data(),truth.size())!=0) {
            const auto* actual=static_cast<const uint8_t*>(guest.contents);size_t errors=0;
            for(size_t i=0;i<truth.size();++i)if(actual[i]!=truth[i]) { if(errors++<8)std::fprintf(stderr,"mip %ux%u eb%u tile%u byte%zu actual%02x expected%02x\n",c.w,c.h,c.eb,c.tile,i,actual[i],truth[i]); }
            std::fprintf(stderr,"mip byte errors=%zu\n",errors);Check(false,"mip export damaged padding, other mips/layers or guard");
        }checked+=truth.size();
		std::vector<uint8_t> expected;uint32_t pitch=0;
		Check(DecodeTiledTextureLevel(std::span(truth).subspan(prefix,extent),reader,0,0,expected,pitch),"mip alias CPU decode");
		std::vector<uint8_t> actual(expected.size());[target getBytes:actual.data() bytesPerRow:pitch fromRegion:MTLRegionMake2D(0,0,c.w,c.h) mipmapLevel:0];
		Check(actual==expected,"mip mixed-width alias differs from physical-byte oracle");texels+=uint64_t(c.w)*c.h;
		// A CPU update after completion must be seen too, without age or sampled hashes.
		static_cast<uint8_t*>(guest.contents)[prefix+17]^=0x5a;truth[prefix+17]^=0x5a;
		cmd=[queue commandBuffer];Check(EncodeTileTextureImport(device,cmd,guest,prefix,target,reader),"CPU update import");
		[cmd commit];[cmd waitUntilCompleted];Check(cmd.status==MTLCommandBufferStatusCompleted,"CPU import GPU completion");
		Check(DecodeTiledTextureLevel(std::span(truth).subspan(prefix,extent),reader,0,0,expected,pitch),"CPU update oracle");
		[target getBytes:actual.data() bytesPerRow:pitch fromRegion:MTLRegionMake2D(0,0,c.w,c.h) mipmapLevel:0];Check(actual==expected,"CPU byte update lost");texels+=uint64_t(c.w)*c.h;
		// Invalid requests must encode no mutation, even when an earlier valid pass exists.
		cmd=[queue commandBuffer];
		Check(!EncodeTileTextureExport(device,cmd,texture,guest,prefix,layout,c.levels,0,0,0),"accept invalid mip");++negatives;
		Check(!EncodeTileTextureExport(device,cmd,texture,guest,prefix,layout,0,c.layers,0,0),"accept invalid physical layer");++negatives;
		Check(!EncodeTileTextureExport(device,cmd,texture,guest,prefix,layout,0,0,0,c.layers),"accept invalid texture slice");++negatives;
		Check(!EncodeTileTextureExport(device,cmd,texture,guest,guest.length-1,layout),"accept truncated backing");++negatives;
		Check(!EncodeTileTextureImport(device,cmd,guest,prefix,target,layout),"accept wrong element width");++negatives;
		[cmd commit];[cmd waitUntilCompleted];Check(cmd.status==MTLCommandBufferStatusCompleted,"negative mip completion");
		Check(std::memcmp(guest.contents,truth.data(),truth.size())==0,"invalid mip mutated guest");++cases;
	}
	std::printf("Mip physical transfers: %u layouts, %u atomic negatives, %llu guest bytes + %llu alias texels PASS\n",cases,negatives,(unsigned long long)checked,(unsigned long long)texels);
}
