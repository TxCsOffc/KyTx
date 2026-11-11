#pragma once
#include "graphics/host_gpu/metal/metalCanonicalMemory.h"

namespace Libs::Graphics::Metal {
// Lossless bridge for independent PS5 Z/S planes. Metal depth/stencil formats
// cannot have uint pixel-format views. Stage through a buffer, preserving raw
// D16/D32/S8 bits, then use the same physical tiler as colour images.
class DepthMemoryTransfer {
public:
	bool Initialize(id<MTLDevice> device,id<MTLTexture> texture,const TileSurfaceLayout& layout,
	                uint32_t first,uint32_t count,const CanonicalMemoryView& read,const CanonicalMemoryView& write) {
		if(!device || !texture || !count || layout.description.levels!=1 ||
		    first>=layout.description.layers || count>layout.description.layers-first ||
		    (texture.textureType!=MTLTextureType2D && texture.textureType!=MTLTextureType2DArray) ||
		    texture.sampleCount!=1 || texture.mipmapLevelCount!=1 || texture.arrayLength<count ||
		    texture.width!=layout.mips[0].width || texture.height!=layout.mips[0].height)return false;
		const uint32_t eb=texture.pixelFormat==MTLPixelFormatDepth16Unorm?2:
		    texture.pixelFormat==MTLPixelFormatDepth32Float?4:texture.pixelFormat==MTLPixelFormatStencil8?1:0;
		if(!eb || eb!=layout.texture.block.bytes_per_element || read.size<layout.total_size || write.size<layout.total_size)return false;
		device_=device;texture_=texture;layout_=layout;first_=first;count_=count;read_=read;write_=write;
		MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:TileElementFormat(eb)
		    width:texture.width height:texture.height mipmapped:NO];
		d.textureType=MTLTextureType2DArray;d.arrayLength=count;
		d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsagePixelFormatView;
		raw_=[device newTextureWithDescriptor:d];
		row_=(texture.width*eb+255u)&~NSUInteger(255u);plane_=row_*texture.height;
		linear_=[device newBufferWithLength:plane_*count options:MTLResourceStorageModePrivate];
		footprint_.clear();
		for(uint32_t layer=first;layer<first+count;++layer) {
			std::vector<FootprintRange> ranges;if(!TiledMipFootprint(layout,0,layer,ranges))return false;
			footprint_.insert(footprint_.end(),ranges.begin(),ranges.end());
		}
		return raw_!=nil && linear_!=nil;
	}
	bool Load(id<MTLCommandBuffer> command)const {
		if(!command || !raw_ || !texture_ || !read_.Gather(command))return false;
		for(uint32_t layer=0;layer<count_;++layer)
			if(!EncodeTileTextureImport(device_,command,read_.buffer,read_.offset,raw_,layout_,0,first_+layer,0,layer))return false;
		return CopyPlane(command,false);
	}
	bool Store(id<MTLCommandBuffer> command)const {
		if(!command || !raw_ || !texture_ || !CopyPlane(command,true))return false;
		for(uint32_t layer=0;layer<count_;++layer)
			if(!EncodeTileTextureExport(device_,command,raw_,write_.buffer,write_.offset,layout_,0,first_+layer,0,layer))return false;
		return write_.Scatter(command,footprint_);
	}
private:
	bool CopyPlane(id<MTLCommandBuffer> command,bool exporting)const {
		id<MTLBlitCommandEncoder>b=[command blitCommandEncoder];if(!b)return false;OrderBegin(b);
		const MTLSize size=MTLSizeMake(texture_.width,texture_.height,1);
		for(uint32_t layer=0;layer<count_;++layer) {
			const id<MTLTexture> source=exporting?texture_:raw_,target=exporting?raw_:texture_;
			[b copyFromTexture:source sourceSlice:layer sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size
			    toBuffer:linear_ destinationOffset:plane_*layer destinationBytesPerRow:row_ destinationBytesPerImage:plane_];
			[b copyFromBuffer:linear_ sourceOffset:plane_*layer sourceBytesPerRow:row_ sourceBytesPerImage:plane_ sourceSize:size
			    toTexture:target destinationSlice:layer destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
		}
		OrderEnd(b);[b endEncoding];return true;
	}
	id<MTLDevice> device_=nil;
	id<MTLTexture> texture_=nil,raw_=nil;
	id<MTLBuffer> linear_=nil;
	TileSurfaceLayout layout_{};
	uint32_t first_=0,count_=0;
	NSUInteger row_=0,plane_=0;
	CanonicalMemoryView read_,write_;
	std::vector<FootprintRange> footprint_;
};
} // namespace Libs::Graphics::Metal
