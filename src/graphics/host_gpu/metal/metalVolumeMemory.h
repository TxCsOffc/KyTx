#pragma once
#include "graphics/host_gpu/metal/metalCanonicalMemory.h"
#include "graphics/host_gpu/metal/metalGuestMipView.h"
#include "graphics/host_gpu/metal/metalTiling.h"
#include <array>
#include <tuple>

namespace Libs::Graphics::Metal {
using VolumeImageKey=std::tuple<uint64_t,uint32_t,uint32_t,std::array<uint32_t,8>,bool>;
inline VolumeImageKey MakeVolumeImageKey(uint64_t address,uint32_t width,uint32_t height,
                                        const ShaderTextureResource& descriptor,bool r128) {
	std::array<uint32_t,8> words;std::copy_n(descriptor.fields,8,words.begin());
	return {address,width,height,words,r128};
}

// GFX10 AddrLib HwlComputeSurfaceInfoLinear + ComputeSurfaceAddrFromCoordLinear:
// 256-byte rows, smallest mip first within a slice, one full mip-chain stride
// between z slices. Physical slice count stays at mip-0 depth; visible image
// depth shrinks with LOD. This differs from packing whole mip volumes in order.
inline bool PlanVolumeLayout(const TileSurfaceDescription& desc,TileSurfaceLayout& out) {
	if(desc.tile_mode!=Prospero::TileMode::kLinear)return TileGetTiledTextureLayout(desc,out);
	out={};TileTextureElementLayout element{};
	if(desc.dimension!=TileSurfaceDimension::Dim3D || desc.layers!=1 || !desc.width || !desc.height || !desc.depth ||
	   !desc.levels || desc.levels>16 || !TileGetTextureElementLayout(desc.format,element) ||
	   element.texel_width!=1 || element.texel_height!=1 || !element.bytes || element.bytes>16 || !std::has_single_bit(element.bytes))return false;
	TileSurfaceLayout l{};l.description=desc;l.first_tail_level=desc.levels;l.texture.block.bytes_per_element=element.bytes;
	for(int mip=int(desc.levels)-1;mip>=0;--mip) {
		auto& m=l.mips[mip];
		const uint64_t ceil_width=(uint64_t(desc.width)+(1ull<<mip)-1)>>mip;
		const uint64_t ceil_height=(uint64_t(desc.height)+(1ull<<mip)-1)>>mip;
		const uint64_t pitch_bytes=(ceil_width*element.bytes+255)&~255ull;
		if(pitch_bytes/element.bytes>UINT32_MAX || ceil_height>UINT32_MAX || ceil_height>UINT64_MAX/pitch_bytes)return false;
		m.width=std::max(desc.width>>mip,1u);m.height=std::max(desc.height>>mip,1u);
		m.padded_width=uint32_t(pitch_bytes/element.bytes);m.padded_height=uint32_t(ceil_height);
		m.offset=l.block_slice_size;m.size=pitch_bytes*ceil_height;
		if(m.size>UINT64_MAX-l.block_slice_size)return false;l.block_slice_size+=m.size;
	}
	if(desc.depth>UINT64_MAX/l.block_slice_size)return false;
	l.total_size=l.block_slice_size*desc.depth;out=l;return true;
}
inline bool VolumeMipFootprint(const TileSurfaceLayout& l,uint32_t level,std::vector<FootprintRange>& out) {
	if(l.description.tile_mode!=Prospero::TileMode::kLinear)return TiledVolumeMipFootprint(l,level,out);
	out.clear();if(l.description.dimension!=TileSurfaceDimension::Dim3D || level>=l.description.levels || level>=16)return false;
	const auto& m=l.mips[level];const uint32_t eb=l.texture.block.bytes_per_element;
	if(!eb || !m.width || !m.height || m.width>m.padded_width || m.height>m.padded_height)return false;
	const uint64_t row=uint64_t(m.padded_width)*eb;
	for(uint32_t z=0;z<std::max(l.description.depth>>level,1u);++z)for(uint32_t y=0;y<m.height;++y) {
		const uint64_t at=uint64_t(z)*l.block_slice_size+m.offset+y*row,bytes=uint64_t(m.width)*eb;
		if(at>l.total_size || bytes>l.total_size-at){out.clear();return false;}
		if(!out.empty() && out.back().begin+out.back().size==at)out.back().size+=bytes;
		else out.push_back({at,bytes});
	}return !out.empty();
}

// The private typed texture is a view of canonical physical memory for one
// command. The producer exports BASE..LAST, never padding or unrelated mips.
// A transfer captures its physical buffers, so a later VA remap cannot redirect
// an already queued producer into the replacement allocation.
class VolumeMemoryTransfer {
public:
	bool Initialize(id<MTLDevice> device,id<MTLTexture> raw,const TileSurfaceLayout& layout,
	                const GuestMipViewPlan& plan,uint32_t base,uint32_t last,const CanonicalMemoryView& memory) {
		valid_=false;footprints_.clear();
		if(!device || !raw || raw.textureType!=MTLTextureType3D || layout.description.dimension!=TileSurfaceDimension::Dim3D ||
		   layout.description.layers!=1 || !memory.buffer || memory.size!=layout.total_size ||
		   !plan.texture_levels || plan.physical_levels!=layout.description.levels || plan.physical_levels>16 ||
		   plan.first_level>=plan.physical_levels || plan.texture_levels>plan.physical_levels-plan.first_level ||
		   base>last || base<plan.first_level || last>=plan.first_level+plan.texture_levels ||
		   raw.pixelFormat!=TileElementFormat(layout.texture.block.bytes_per_element) ||
		   raw.width!=std::max(layout.description.width>>plan.first_level,1u) ||
		   raw.height!=std::max(layout.description.height>>plan.first_level,1u) ||
		   raw.depth!=std::max(layout.description.depth>>plan.first_level,1u) || raw.mipmapLevelCount!=plan.texture_levels)return false;
		for(uint32_t m=base;m<=last;++m) {
			std::vector<FootprintRange> ranges;
			if(!VolumeMipFootprint(layout,m,ranges))return false;
			footprints_.push_back(std::move(ranges));
		}
		device_=device;raw_=raw;layout_=layout;plan_=plan;base_=base;last_=last;memory_=memory;valid_=true;return true;
	}
	bool Load(id<MTLCommandBuffer> command)const {
		if(!valid_ || !command || !memory_.Gather(command))return false;
		if(layout_.description.tile_mode==Prospero::TileMode::kLinear)return Linear(command,false);
		for(uint32_t m=0;m<plan_.texture_levels;++m)
			if(!EncodeTileVolumeImport(device_,command,memory_.buffer,memory_.offset,raw_,layout_,plan_.first_level+m,m))return false;
		return true;
	}
	bool Store(id<MTLCommandBuffer> command)const {
		if(!valid_ || !command)return false;
		if(layout_.description.tile_mode==Prospero::TileMode::kLinear) {
			if(!Linear(command,true))return false;
			for(const auto& footprint:footprints_)if(!memory_.Scatter(command,footprint))return false;
			return true;
		}
		for(uint32_t m=base_;m<=last_;++m)
			if(!EncodeTileVolumeExport(device_,command,raw_,memory_.buffer,memory_.offset,layout_,m,m-plan_.first_level) ||
			   !memory_.Scatter(command,footprints_[m-base_]))return false;
		return true;
	}
private:
	bool Linear(id<MTLCommandBuffer> command,bool exporting)const {
		id<MTLBlitCommandEncoder> e=[command blitCommandEncoder];if(!e)return false;OrderBegin(e);
		const uint32_t begin=exporting?base_:plan_.first_level,end=exporting?last_+1:plan_.first_level+plan_.texture_levels;
		const uint32_t eb=layout_.texture.block.bytes_per_element;
		for(uint32_t mip=begin;mip<end;++mip) {
			const auto& m=layout_.mips[mip];const uint64_t row=uint64_t(m.padded_width)*eb;
			for(uint32_t z=0;z<std::max(layout_.description.depth>>mip,1u);++z) {
				const uint64_t offset=memory_.offset+uint64_t(z)*layout_.block_slice_size+m.offset;
				if(exporting)[e copyFromTexture:raw_ sourceSlice:0 sourceLevel:mip-plan_.first_level sourceOrigin:MTLOriginMake(0,0,z)
				    sourceSize:MTLSizeMake(m.width,m.height,1) toBuffer:memory_.buffer destinationOffset:offset destinationBytesPerRow:row destinationBytesPerImage:row*m.height];
				else [e copyFromBuffer:memory_.buffer sourceOffset:offset sourceBytesPerRow:row sourceBytesPerImage:row*m.height
				    sourceSize:MTLSizeMake(m.width,m.height,1) toTexture:raw_ destinationSlice:0 destinationLevel:mip-plan_.first_level destinationOrigin:MTLOriginMake(0,0,z)];
			}
		}OrderEnd(e);[e endEncoding];return true;
	}
	bool valid_=false;
	id<MTLDevice> device_=nil;id<MTLTexture> raw_=nil;
	TileSurfaceLayout layout_{};GuestMipViewPlan plan_{};uint32_t base_=0,last_=0;
	CanonicalMemoryView memory_;
	std::vector<std::vector<FootprintRange>> footprints_;
};
}
