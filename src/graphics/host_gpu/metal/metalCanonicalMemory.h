#pragma once
#include "graphics/host_gpu/metal/metalGuestWrites.h"
#include "graphics/host_gpu/metal/metalOrder.h"
#include "graphics/host_gpu/metal/metalSurfaceFootprint.h"
#include <array>
#include <functional>
#include <unordered_map>
#include <optional>

namespace Libs::Graphics::Metal {
struct PhysicalMapping { uint64_t address,size,backing_offset; };
struct PhysicalSpan { uint64_t offset,physical,size; bool operator==(const PhysicalSpan&) const = default; };
// Resolve every byte, not just the first page. Adjacent VA mappings need not
// share a physical allocation. Gaps and overflow are rejected atomically.
inline bool ResolvePhysicalSpans(std::span<const PhysicalMapping> mappings,uint64_t address,uint64_t size,std::vector<PhysicalSpan>& out) {
	out.clear();if(!size || size>UINT64_MAX-address)return false;
	const uint64_t end=address+size;uint64_t pos=address;
	auto it=std::upper_bound(mappings.begin(),mappings.end(),address,[](uint64_t a,const PhysicalMapping&m){return a<m.address;});
	if(it!=mappings.begin())--it;
	for(;it!=mappings.end() && pos<end;++it) {
		const auto&m=*it;
		if(m.size>UINT64_MAX-m.address || m.backing_offset>UINT64_MAX-m.size) {out.clear();return false;}
		if(m.address+m.size<=pos)continue;
		if(m.address>pos)break;
		const uint64_t count=std::min(end-pos,m.address+m.size-pos),physical=m.backing_offset+pos-m.address;
		if(!out.empty() && out.back().physical+out.back().size==physical)out.back().size+=count;
		else out.push_back({pos-address,physical,count});
		pos+=count;
	}
	if(pos!=end) {out.clear();return false;}return true;
}

// A host image cannot survive a change of the physical allocation behind its VA.
// Compare exact spans, so unrelated mappings and harmless mapping splits do not
// invalidate it. Missing current bytes also make the old image unusable.
struct PhysicalBackingSnapshot {
	uint64_t size=0;
	std::vector<PhysicalSpan> spans;
	bool Capture(std::span<const PhysicalMapping> mappings,uint64_t address,uint64_t bytes) {
		size=0;spans.clear();
		if(!ResolvePhysicalSpans(mappings,address,bytes,spans))return false;
		size=bytes;return true;
	}
	bool Valid()const{return size!=0 && !spans.empty();}
	bool Matches(std::span<const PhysicalMapping> mappings,uint64_t address)const {
		std::vector<PhysicalSpan> current;
		return Valid() && ResolvePhysicalSpans(mappings,address,size,current) && current==spans;
	}
};

struct ImageWriterIdentity {
	uintptr_t texture=0;
	uint64_t serial=0;
	bool operator==(const ImageWriterIdentity&)const=default;
};
struct CanonicalImageHistory {
	PhysicalBackingSnapshot physical;
	ImageWriterIdentity writer;
	bool canonical=false;
};
// A detached newer writer is not covered by the retired render-target history.
// Its bytes must be materialized by its own producer, never guessed from memory.
inline bool CanImportRemappedImage(std::span<const CanonicalImageHistory> history,std::span<const PhysicalMapping> mappings,
                                  uint64_t address,std::optional<CanonicalImageHistory> last_writer) {
	if(history.empty())return false;
	for(const auto& h:history)if(!h.canonical || !h.physical.Valid() || h.physical.Matches(mappings,address))return false;
	if(!last_writer)return true;
	// A canonical storage image may have written after the render target. Its
	// queued writeback owns the recorded old physical bytes, not the reused VA.
	if(last_writer->canonical && last_writer->physical.Valid())return !last_writer->physical.Matches(mappings,address);
	return std::any_of(history.begin(),history.end(),[&](const auto& h){return h.writer==last_writer->writer;});
}

// A contiguous GPU tile-kernel view over possibly discontiguous physical bytes.
// Every segment uses the canonical physical Metal buffer, shared with raw GPU
// pointers. Export copies only visible texel ranges, preserving newer bytes in
// other mips/padding even when they share the staging allocation.
class CanonicalMemoryView {
public:
	struct BufferSpan {id<MTLBuffer> buffer=nil;uint64_t offset=0;};
	using Resolver=std::function<std::optional<BufferSpan>(uint64_t,uint64_t)>;
	id<MTLBuffer> buffer=nil;uint64_t offset=0,size=0;
	bool Initialize(id<MTLDevice> device,std::span<const PhysicalSpan> spans,uint64_t bytes,uint64_t chunk_size,const Resolver& resolve) {
		segments_.clear();buffer=nil;offset=0;size=bytes;spans_.assign(spans.begin(),spans.end());
		if(!device || !bytes || !chunk_size)return false;
		for(const auto&s:spans)for(uint64_t done=0;done<s.size;) {
			const uint64_t count=std::min(s.size-done,chunk_size-(s.physical+done)%chunk_size);
			const auto resolved=resolve(s.physical+done,count);
			if(!resolved || !resolved->buffer || resolved->offset>resolved->buffer.length || count>resolved->buffer.length-resolved->offset ||
			    s.offset+done>bytes || count>bytes-s.offset-done)return false;
			segments_.push_back({s.offset+done,count,resolved->buffer,resolved->offset});done+=count;
		}
		uint64_t covered=0;for(const auto&s:segments_) {if(s.local!=covered)return false;covered+=s.size;}if(covered!=bytes)return false;
		if(segments_.size()==1) {buffer=segments_[0].buffer;offset=segments_[0].offset;return true;}
		buffer=[device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];return buffer!=nil;
	}
	bool Gather(id<MTLCommandBuffer> command)const {
		if(!command || !buffer)return false;if(segments_.size()==1)return true;
		id<MTLBlitCommandEncoder> e=[command blitCommandEncoder];OrderBegin(e);
		for(const auto&s:segments_)[e copyFromBuffer:s.buffer sourceOffset:s.offset toBuffer:buffer destinationOffset:s.local size:s.size];
		OrderEnd(e);[e endEncoding];return true;
	}
	bool Scatter(id<MTLCommandBuffer> command,std::span<const FootprintRange> ranges)const {
		if(!command || !buffer)return false;
		for(const auto&r:ranges)if(r.begin>size || r.size>size-r.begin)return false;
		NoteWrites(ranges);
		if(segments_.size()==1)return true;
		id<MTLBlitCommandEncoder> e=[command blitCommandEncoder];OrderBegin(e);
		for(const auto&r:ranges)for(const auto&s:segments_) {
			const uint64_t begin=std::max(r.begin,s.local),end=std::min(r.begin+r.size,s.local+s.size);
			if(begin<end)[e copyFromBuffer:buffer sourceOffset:begin toBuffer:s.buffer destinationOffset:s.offset+begin-s.local size:end-begin];
		}
		OrderEnd(e);[e endEncoding];return true;
	}
private:
	// The physical pages this export writes, for the write generations of cached copies (metalGuestWrites.h).
	void NoteWrites(std::span<const FootprintRange> ranges)const {
		std::vector<Metal::GuestWrites::PhysicalRange> written;
		for(const auto&r:ranges)for(const auto&s:spans_) {
			const uint64_t begin=std::max(r.begin,s.offset),end=std::min(r.begin+r.size,s.offset+s.size);
			if(begin<end)written.push_back({s.physical+begin-s.offset,end-begin});
		}
		Metal::GuestWrites::NotePhysicalWrite(written);
	}
	struct Segment {uint64_t local,size;id<MTLBuffer> buffer;uint64_t offset;};
	std::vector<Segment> segments_;
	std::vector<PhysicalSpan> spans_;
};

// Exact physical footprint of one mip/layer, including a subrectangle in a
// shared tail tile. Use the scalar address oracle for cut blocks only.
// Computes the footprint. Blocks are visited in increasing address order (row-major block index), so only the elements of a
// partly covered block need sorting, locally; the result is sorted and merged without a global sort.
inline bool TiledMipRectFootprintCompute(const TileSurfaceLayout& l,uint32_t level,uint32_t layer,PixelRect rect,std::vector<FootprintRange>& out) {
	out.clear();if(level>=l.description.levels || level>=16 || layer>=l.description.layers || l.description.dimension!=TileSurfaceDimension::Dim2D)return false;
	const auto&m=l.mips[level];const auto&b=l.texture.block;
	if(!rect.width || !rect.height || rect.x>m.width || rect.y>m.height || rect.width>m.width-rect.x || rect.height>m.height-rect.y)return false;
	if(!b.block_width || !b.block_height || !b.block_size || !b.bytes_per_element)return false;
	const bool tail=level>=l.first_tail_level;
	const uint32_t tx=(tail?m.tail_x:0)+rect.x,ty=(tail?m.tail_y:0)+rect.y;
	if(uint64_t(tx)+rect.width>m.padded_width || uint64_t(ty)+rect.height>m.padded_height)return false;
	const uint64_t base=uint64_t(layer)*l.block_slice_size+m.offset;
	const auto append=[&](uint64_t begin,uint64_t size) {
		if(!out.empty() && out.back().begin+out.back().size>=begin) {
			out.back().size=std::max(out.back().begin+out.back().size,begin+size)-out.back().begin;
		} else out.push_back({begin,size});
	};
	std::vector<uint32_t> within;
	for(uint32_t by=ty/b.block_height;by<(ty+rect.height+b.block_height-1)/b.block_height;++by)
	for(uint32_t bx=tx/b.block_width;bx<(tx+rect.width+b.block_width-1)/b.block_width;++bx) {
		const uint32_t x0=bx*b.block_width,y0=by*b.block_height;
		const uint64_t block=base+uint64_t(by*(m.padded_width/b.block_width)+bx)*b.block_size;
		if(block<(out.empty()?0:out.back().begin))return false; // not in address order: layout outside this model
		if(x0>=tx && y0>=ty && uint64_t(x0)+b.block_width<=uint64_t(tx)+rect.width && uint64_t(y0)+b.block_height<=uint64_t(ty)+rect.height) {
			append(block,b.block_size);
			continue;
		}
		uint32_t bank=0;if(!TileGetBlockXor(b,bx,by,layer,bank))return false;
		within.clear();
		for(uint32_t y=std::max(y0,ty);y<std::min(y0+b.block_height,ty+rect.height);++y)
		for(uint32_t x=std::max(x0,tx);x<std::min(x0+b.block_width,tx+rect.width);++x) {
			uint32_t offset=0;if(!TileGetBlockOffset(b,x-x0,y-y0,0,offset))return false;
			within.push_back(offset^bank);
		}
		std::sort(within.begin(),within.end());
		for(uint32_t offset:within) {
			if(offset>=b.block_size)return false;
			append(block+offset,b.bytes_per_element);
		}
	}
	for(const auto& r:out)if(r.begin>l.total_size || r.size>l.total_size-r.begin)return false;
	return !out.empty();
}
// The same scissor rectangles of the same surfaces are exported every frame: the footprint is cached per thread, keyed by
// everything it depends on.
inline bool TiledMipRectFootprint(const TileSurfaceLayout& l,uint32_t level,uint32_t layer,PixelRect rect,std::vector<FootprintRange>& out) {
	if(level>=16){out.clear();return false;}
	const auto& m=l.mips[level];const auto& b=l.texture.block;
	const std::array<uint64_t,20> key{uint64_t(b.family),b.bytes_per_element,b.block_size,b.block_width,b.block_height,
	    uint64_t(l.description.dimension),l.description.levels,l.description.layers,l.first_tail_level,l.block_slice_size,l.total_size,
	    m.offset,(uint64_t(m.width)<<32)|m.height,(uint64_t(m.padded_width)<<32)|m.padded_height,(uint64_t(m.tail_x)<<32)|m.tail_y,
	    level,layer,(uint64_t(rect.x)<<32)|rect.y,(uint64_t(rect.width)<<32)|rect.height,0};
	struct KeyHash {
		size_t operator()(const std::array<uint64_t,20>& k)const {uint64_t h=1469598103934665603ull;for(uint64_t v:k)h=(h^v)*1099511628211ull;return size_t(h);}
	};
	thread_local std::unordered_map<std::array<uint64_t,20>,std::vector<FootprintRange>,KeyHash> cache;
	if(const auto it=cache.find(key);it!=cache.end()){out=it->second;return true;}
	if(!TiledMipRectFootprintCompute(l,level,layer,rect,out))return false;
	if(cache.size()>=1024)cache.clear();
	cache.emplace(key,out);
	return true;
}
inline bool TiledMipFootprint(const TileSurfaceLayout& l,uint32_t level,uint32_t layer,std::vector<FootprintRange>& out) {
	if(level>=l.description.levels || level>=16){out.clear();return false;}
	return TiledMipRectFootprint(l,level,layer,{0,0,l.mips[level].width,l.mips[level].height},out);
}
// Exact visible bytes of a thick-tiled volume mip. Z slabs share the same
// per-slab reversed mip chain; tail coordinates are in the x/y tile plane.
inline bool TiledVolumeMipFootprintCompute(const TileSurfaceLayout& l,uint32_t level,std::vector<FootprintRange>& out) {
	out.clear();
	if(l.description.dimension!=TileSurfaceDimension::Dim3D || l.description.layers!=1 ||
	   level>=l.description.levels || level>=16)return false;
	const auto& m=l.mips[level];const auto& b=l.texture.block;
	if(!b.block_width || !b.block_height || !b.block_depth || !b.block_size || !b.bytes_per_element)return false;
	const uint32_t tx=level>=l.first_tail_level?m.tail_x:0,ty=level>=l.first_tail_level?m.tail_y:0;
	const uint32_t depth=std::max(l.description.depth>>level,1u);
	if(!m.width || !m.height || uint64_t(tx)+m.width>m.padded_width || uint64_t(ty)+m.height>m.padded_height)return false;
	for(uint32_t bz=0;bz<(depth+b.block_depth-1)/b.block_depth;++bz)
	for(uint32_t by=ty/b.block_height;by<(ty+m.height+b.block_height-1)/b.block_height;++by)
	for(uint32_t bx=tx/b.block_width;bx<(tx+m.width+b.block_width-1)/b.block_width;++bx) {
		const uint32_t x0=bx*b.block_width,y0=by*b.block_height,z0=bz*b.block_depth;
		const uint64_t block=uint64_t(bz)*l.block_slice_size+m.offset+
		    (uint64_t(by)*(m.padded_width/b.block_width)+bx)*b.block_size;
		if(x0>=tx && y0>=ty && uint64_t(x0)+b.block_width<=uint64_t(tx)+m.width &&
		   uint64_t(y0)+b.block_height<=uint64_t(ty)+m.height && uint64_t(z0)+b.block_depth<=depth)
			out.push_back({block,b.block_size});
		else {
			uint32_t bank=0;if(!TileGetBlockXor(b,bx,by,bz,bank)){out.clear();return false;}
			for(uint32_t z=z0;z<std::min(z0+b.block_depth,depth);++z)
			for(uint32_t y=std::max(y0,ty);y<std::min(y0+b.block_height,ty+m.height);++y)
			for(uint32_t x=std::max(x0,tx);x<std::min(x0+b.block_width,tx+m.width);++x) {
				uint32_t within=0;if(!TileGetBlockOffset(b,x-x0,y-y0,z-z0,within)){out.clear();return false;}
				out.push_back({block+(within^bank),b.bytes_per_element});
			}
		}
	}
	std::sort(out.begin(),out.end(),[](auto a,auto b){return a.begin<b.begin;});
	size_t merged=0;for(auto range:out) {
		if(range.begin>l.total_size || range.size>l.total_size-range.begin){out.clear();return false;}
		if(merged && out[merged-1].begin+out[merged-1].size>=range.begin)
			out[merged-1].size=std::max(out[merged-1].begin+out[merged-1].size,range.begin+range.size)-out[merged-1].begin;
		else out[merged++]=range;
	}out.resize(merged);return !out.empty();
}

// Volumes are rebound each command, but their visible physical offsets depend
// only on layout. Cache those offsets, never guest bytes or physical backing.
// A bounded per-thread cache avoids repeating per-texel edge sorting each frame.
inline bool TiledVolumeMipFootprint(const TileSurfaceLayout& l,uint32_t level,std::vector<FootprintRange>& out) {
	if(level>=16){out.clear();return false;}
	const auto& m=l.mips[level];const auto& b=l.texture.block;
	using Key=std::array<uint64_t,18>;
	const Key key{uint64_t(b.family),b.bytes_per_element,b.block_size,b.block_width,b.block_height,b.block_depth,
	    uint64_t(l.description.dimension),l.description.levels,l.description.layers,l.description.depth,
	    l.first_tail_level,l.block_slice_size,l.total_size,m.offset,(uint64_t(m.width)<<32)|m.height,
	    (uint64_t(m.padded_width)<<32)|m.padded_height,(uint64_t(m.tail_x)<<32)|m.tail_y,level};
	struct Hash {
		size_t operator()(const Key& k)const {uint64_t h=1469598103934665603ull;for(uint64_t v:k)h=(h^v)*1099511628211ull;return size_t(h);}
	};
	struct Cache {std::unordered_map<Key,std::vector<FootprintRange>,Hash> entries;size_t bytes=0;};
	thread_local Cache cache;
	if(const auto it=cache.entries.find(key);it!=cache.entries.end()){out=it->second;return true;}
	if(!TiledVolumeMipFootprintCompute(l,level,out))return false;
	constexpr size_t max_bytes=8u<<20;
	if(out.size()<=max_bytes/sizeof(FootprintRange)) {
		const size_t bytes=out.size()*sizeof(FootprintRange);
		if(cache.entries.size()>=256 || bytes>max_bytes-cache.bytes){cache.entries.clear();cache.bytes=0;}
		cache.entries.emplace(key,out);cache.bytes+=bytes;
	}
	return true;
}

}
