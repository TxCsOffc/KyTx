#include "graphics/host_gpu/metal/metalGuestMipView.h"

// A BC allocation and its uint block view have the same physical elements,
// including repeated 1x1 block mips beyond the uint view's natural mip limit.
static void MipViewTests(id<MTLDevice> device) {
	unsigned gpu_levels = 0, negative_bytes = 0;
	for (uint32_t eb : {8u,16u}) {
		const auto bc = eb == 8 ? Prospero::BufferFormat::kBc1UNorm : Prospero::BufferFormat::kBc7UNorm;
		const auto raw = eb == 8 ? Prospero::BufferFormat::k32_32UInt : Prospero::BufferFormat::k32_32_32_32UInt;
		const auto format = eb == 8 ? MTLPixelFormatRG32Uint : MTLPixelFormatRGBA32Uint;
		TileSurfaceLayout reference {}, layout {};
		Check(TileGetTiledTextureLayout({bc,Prospero::TileMode::kStandard4KB,TileSurfaceDimension::Dim2D,256,256,1,9,1},reference),
		      "BC allocation layout", "mip-view",eb);
		Check(TileGetTiledTextureLayout({raw,Prospero::TileMode::kStandard4KB,TileSurfaceDimension::Dim2D,64,64,1,9,1},layout),
		      "uint block view layout", "mip-view",eb);
		Check(reference.total_size == layout.total_size && reference.mips[0].offset == layout.mips[0].offset,
		      "BC and uint views share physical allocation", "mip-view",eb);
		std::vector<uint8_t> source(reference.total_size,0xa7);
		std::vector<std::vector<uint8_t>> expected(9);
		for (unsigned level=0;level<9;++level) {
			const auto& mip=reference.mips[level]; const auto& block=reference.texture.block;
			Check(mip == layout.mips[level],"all BC/uint mip positions agree","mip-view",eb,level);
			expected[level].resize(size_t(mip.width)*mip.height*eb);
			const bool tail=level>=reference.first_tail_level;
			for (uint32_t y=0;y<mip.height;++y) for (uint32_t x=0;x<mip.width;++x) {
				uint32_t within=0,bank=0;
				const uint32_t bx=tail?0:x/block.block_width,by=tail?0:y/block.block_height;
				Check(TileGetBlockOffset(block,tail?x+mip.tail_x:x%block.block_width,tail?y+mip.tail_y:y%block.block_height,0,within),
				      "BC block address","mip-view",eb,level);
				Check(TileGetBlockXor(block,bx,by,0,bank),"BC bank xor","mip-view",eb,level);
				const uint64_t offset=mip.offset+uint64_t(by*(mip.padded_width/block.block_width)+bx)*block.block_size+(within^bank);
				for (unsigned b=0;b<eb;++b) {
					const uint8_t value=uint8_t(x*31+y*97+b*43+level*17);
					expected[level][(size_t(y)*mip.width+x)*eb+b]=value;
					if (offset+b<source.size()) source[offset+b]=value;
					else Check(false,"BC source write in range","mip-view",eb,level);
				}
			}
		}
		const auto original=source;
		for (auto range : {std::pair{0u,0u},std::pair{0u,2u},std::pair{2u,3u},std::pair{6u,6u},std::pair{7u,7u},std::pair{8u,8u}}) {
			ShaderTextureResource descriptor {};
			descriptor.fields[1]=(uint32_t(raw)<<20u)|((63u&3u)<<30u);
			descriptor.fields[2]=(63u>>2u)|(63u<<14u);
			descriptor.fields[3]=0xfacu|(range.first<<12u)|(range.second<<16u)|(5u<<20u)|(9u<<28u);
			descriptor.fields[5]=8u<<4u;
			GuestMipViewPlan plan;
			Check(PlanGuestMipView(descriptor,64,64,false,plan) && plan.physical_levels==9 &&
			      plan.first_level==(range.first>=7?range.first:0) && plan.texture_levels==(range.first>=7?1:range.second+1),
			      "allocation and active view planned separately","mip-view",eb);
			const auto host_width=std::max(64u>>plan.first_level,1u);
			MTLTextureDescriptor* td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:host_width height:host_width mipmapped:plan.texture_levels>1];
			td.mipmapLevelCount=plan.texture_levels; td.storageMode=MTLStorageModeShared; td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsagePixelFormatView;
			id<MTLTexture> texture=[device newTextureWithDescriptor:td];
			Check(texture!=nil,"Metal allocates the active mip parent","mip-view",eb);
			if (!texture) continue;
			for (unsigned level=0;level<plan.texture_levels;++level) {
				const auto physical=plan.first_level+level;
				std::vector<uint8_t> bytes; uint32_t pitch=0;
				Check(DecodeTiledTextureLevel(source,layout,physical,0,bytes,pitch) && bytes==expected[physical],
				      "physical mip decode matches opaque BC elements","mip-view",eb,level);
				[texture replaceRegion:MTLRegionMake2D(0,0,std::max(64u>>physical,1u),std::max(64u>>physical,1u)) mipmapLevel:level withBytes:bytes.data() bytesPerRow:pitch];
			}
			id<MTLTexture> view=[texture newTextureViewWithPixelFormat:format textureType:MTLTextureType2D
			                                                  levels:NSMakeRange(range.first-plan.first_level,range.second-range.first+1) slices:NSMakeRange(0,1)];
			Check(view && view.mipmapLevelCount==range.second-range.first+1 && view.width==std::max(64u>>range.first,1u),
			      "BASE/LAST produce the requested Metal view","mip-view",eb);
			id<MTLCommandQueue> queue=[device newCommandQueue];
			for (unsigned level=0;view && level<view.mipmapLevelCount;++level) {
				const unsigned width=std::max(64u>>(range.first+level),1u),row=width*eb,stride=(row+255)&~255u;
				id<MTLBuffer> result=[device newBufferWithLength:stride*width options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];
				[blit copyFromTexture:view sourceSlice:0 sourceLevel:level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,width,1)
				            toBuffer:result destinationOffset:0 destinationBytesPerRow:stride destinationBytesPerImage:stride*width];
				[blit endEncoding];[cb commit];[cb waitUntilCompleted];
				Check(cb.status==MTLCommandBufferStatusCompleted,"mip-view GPU readback completed","mip-view",eb,level);
				for (unsigned y=0;y<width;++y) Check(std::memcmp(static_cast<const uint8_t*>(result.contents)+y*stride,
				    expected[range.first+level].data()+y*row,row)==0,"Metal view keeps real guest elements","mip-view",eb,level);
				++gpu_levels;
			}
		}
		TileSurfaceLayout truncated {};
		Check(TileGetTiledTextureLayout({raw,Prospero::TileMode::kStandard4KB,TileSurfaceDimension::Dim2D,64,64,1,1,1},truncated),
		      "negative-control short layout","mip-view",eb);
		std::vector<uint8_t> wrong;uint32_t pitch=0;
		Check(DecodeTiledTextureLevel(source,truncated,0,0,wrong,pitch),"negative-control decode","mip-view",eb);
		for (size_t i=0;i<wrong.size();++i) negative_bytes+=wrong[i]!=expected[0][i];
		Check(wrong!=expected[0],"clamping physical layout to the view loses data","mip-view",eb);
		Check(source==original,"guest allocation and padding remain unchanged","mip-view",eb);
		ShaderTextureResource r128 {}; r128.fields[5]=8u<<4u;
		GuestMipViewPlan plan;
		Check(PlanGuestMipView(r128,64,64,true,plan) && plan.physical_levels==1 && plan.texture_levels==1,
		      "R128 uses its own LAST_LEVEL allocation rule","mip-view",eb);
	}
	std::printf("Guest mip views: nine-level BC allocation -> uint views, %u GPU mip reads; truncated-layout negative control differs in %u bytes; failures=%d\n",gpu_levels,negative_bytes,s_failures);
}
