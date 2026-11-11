static uint32_t BcElementBytes(MTLPixelFormat format) {
	switch(format) {
		case MTLPixelFormatBC1_RGBA:case MTLPixelFormatBC1_RGBA_sRGB:
		case MTLPixelFormatBC4_RUnorm:case MTLPixelFormatBC4_RSnorm:return 8;
		case MTLPixelFormatBC2_RGBA:case MTLPixelFormatBC2_RGBA_sRGB:
		case MTLPixelFormatBC3_RGBA:case MTLPixelFormatBC3_RGBA_sRGB:
		case MTLPixelFormatBC5_RGUnorm:case MTLPixelFormatBC5_RGSnorm:
		case MTLPixelFormatBC6H_RGBFloat:case MTLPixelFormatBC6H_RGBUfloat:
		case MTLPixelFormatBC7_RGBAUnorm:case MTLPixelFormatBC7_RGBAUnorm_sRGB:return 16;
		default:return 0;
	}
}

static bool EncodeBcTransfer(id<MTLDevice> device,id<MTLCommandBuffer> cmd,bool exporting,id<MTLTexture> texture,
    id<MTLBuffer> tiled,NSUInteger offset,const TileSurfaceLayout& layout,uint32_t level,uint32_t layer,
    uint32_t texture_level,uint32_t texture_slice) {
	if(!device || !cmd || !device.supportsBCTextureCompression || !texture || !tiled || texture.device!=device || tiled.device!=device ||
	    layout.description.dimension!=TileSurfaceDimension::Dim2D || !layout.description.levels || layout.description.levels>16 ||
	    level>=layout.description.levels || layer>=layout.description.layers || texture_level>=texture.mipmapLevelCount ||
	    (texture.textureType!=MTLTextureType2D && texture.textureType!=MTLTextureType2DArray) || texture_slice>=texture.arrayLength ||
	    offset>tiled.length || layout.total_size>tiled.length-offset)return false;
	const auto& m=layout.mips[level];const auto& b=layout.texture.block;
	const uint32_t eb=BcElementBytes(texture.pixelFormat);
	const uint32_t width=std::max(layout.description.width>>level,1u),height=std::max(layout.description.height>>level,1u);
	if(!eb || b.bytes_per_element!=eb || layout.texture.texel_width!=4 || layout.texture.texel_height!=4 ||
	    std::max(texture.width>>texture_level,NSUInteger(1))!=width || std::max(texture.height>>texture_level,NSUInteger(1))!=height ||
	    m.width!=(width+3u)/4u || m.height!=(height+3u)/4u)return false;
	TileTables tables;
	if(!BuildTileTables(b,m.padded_width,m.padded_height,tables))return false;
	const uint64_t base=uint64_t(layer)*layout.block_slice_size+m.offset;
	const uint64_t extent=uint64_t(tables.columns)*tables.rows*b.block_size;
	if(base>layout.total_size || extent>layout.total_size-base)return false;
	const bool tail=level>=layout.first_tail_level;
	if(uint64_t(tail?m.tail_x:0)+m.width>m.padded_width || uint64_t(tail?m.tail_y:0)+m.height>m.padded_height)return false;

	auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:TileElementFormat(eb) width:m.width height:m.height mipmapped:NO];
	td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
	id<MTLTexture> blocks=[device newTextureWithDescriptor:td];
	const NSUInteger row=(NSUInteger(m.width)*eb+255u)&~NSUInteger(255u);
	id<MTLBuffer> linear=[device newBufferWithLength:row*m.height options:MTLResourceStorageModePrivate];
	if(!blocks || !linear)return false;
	// Retain bridge resources for unretained command buffers too.
	[cmd addCompletedHandler:^(id<MTLCommandBuffer>){(void)blocks.width;(void)linear.length;}];
	const MTLSize pixels=MTLSizeMake(width,height,1),elements=MTLSizeMake(m.width,m.height,1);
	const MTLOrigin zero=MTLOriginMake(0,0,0);
	if(exporting) {
		id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder];OrderBegin(blit);
		[blit copyFromTexture:texture sourceSlice:texture_slice sourceLevel:texture_level sourceOrigin:zero sourceSize:pixels
		    toBuffer:linear destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:0];
		[blit copyFromBuffer:linear sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:0 sourceSize:elements
		    toTexture:blocks destinationSlice:0 destinationLevel:0 destinationOrigin:zero];
		OrderEnd(blit);[blit endEncoding];
		return EncodeTileTextureExport(device,cmd,blocks,tiled,offset,layout,level,layer);
	}
	if(!EncodeTileTextureImport(device,cmd,tiled,offset,blocks,layout,level,layer))return false;
	id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder];OrderBegin(blit);
	[blit copyFromTexture:blocks sourceSlice:0 sourceLevel:0 sourceOrigin:zero sourceSize:elements
	    toBuffer:linear destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:0];
	[blit copyFromBuffer:linear sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:0 sourceSize:pixels
	    toTexture:texture destinationSlice:texture_slice destinationLevel:texture_level destinationOrigin:zero];
	OrderEnd(blit);[blit endEncoding];return true;
}

bool EncodeTileBcImport(id<MTLDevice> device,id<MTLCommandBuffer> cmd,id<MTLBuffer> tiled,NSUInteger offset,
    id<MTLTexture> texture,const TileSurfaceLayout& layout,uint32_t level,uint32_t layer,uint32_t texture_level,uint32_t texture_slice) {
	return EncodeBcTransfer(device,cmd,false,texture,tiled,offset,layout,level,layer,texture_level,texture_slice);
}
bool EncodeTileBcExport(id<MTLDevice> device,id<MTLCommandBuffer> cmd,id<MTLTexture> texture,id<MTLBuffer> tiled,NSUInteger offset,
    const TileSurfaceLayout& layout,uint32_t level,uint32_t layer,uint32_t texture_level,uint32_t texture_slice) {
	return EncodeBcTransfer(device,cmd,true,texture,tiled,offset,layout,level,layer,texture_level,texture_slice);
}
