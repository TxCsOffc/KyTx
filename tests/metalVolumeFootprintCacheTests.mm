#include "graphics/host_gpu/metal/metalCanonicalMemory.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>

using namespace Libs::Graphics;
using namespace Libs::Graphics::Metal;
static void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}

// Independent per-texel occupancy oracle: no block-coverage shortcut, cache,
// range sorting, or merging from the implementation under test.
static std::vector<FootprintRange> Oracle(const TileSurfaceLayout& l,uint32_t level) {
	std::vector<uint8_t> mask(l.total_size);
	const auto& m=l.mips[level];const auto& b=l.texture.block;
	for(uint32_t z=0;z<std::max(l.description.depth>>level,1u);++z)
	for(uint32_t y=0;y<m.height;++y)for(uint32_t x=0;x<m.width;++x) {
		const uint32_t tx=x+(level>=l.first_tail_level?m.tail_x:0),ty=y+(level>=l.first_tail_level?m.tail_y:0);
		uint32_t within=0,bank=0;
		Check(TileGetBlockOffset(b,tx%b.block_width,ty%b.block_height,z%b.block_depth,within),"oracle offset");
		Check(TileGetBlockXor(b,tx/b.block_width,ty/b.block_height,z/b.block_depth,bank),"oracle XOR");
		const uint64_t at=uint64_t(z/b.block_depth)*l.block_slice_size+m.offset+
		    (uint64_t(ty/b.block_height)*(m.padded_width/b.block_width)+tx/b.block_width)*b.block_size+(within^bank);
		Check(at<=mask.size() && b.bytes_per_element<=mask.size()-at,"oracle bounds");
		std::fill_n(mask.begin()+at,b.bytes_per_element,1);
	}
	std::vector<FootprintRange> out;
	for(size_t at=0;at<mask.size();) {
		if(!mask[at]){++at;continue;}
		const size_t begin=at;while(at<mask.size() && mask[at])++at;
		out.push_back({begin,at-begin});
	}
	return out;
}
static bool Same(const std::vector<FootprintRange>& a,const std::vector<FootprintRange>& b) {
	return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](auto x,auto y){return x.begin==y.begin && x.size==y.size;});
}
int main(){try {
	unsigned cases=0,negatives=0;
	const Prospero::BufferFormat formats[]{Prospero::BufferFormat::k8UInt,Prospero::BufferFormat::k16UInt,
	    Prospero::BufferFormat::k32UInt,Prospero::BufferFormat::k32_32UInt,Prospero::BufferFormat::k32_32_32_32UInt};
	for(auto tile:{Prospero::TileMode::kStandard4KB,Prospero::TileMode::kStandard64KB,Prospero::TileMode::kPrt})
	for(auto format:formats)for(auto shape:{std::array<uint32_t,3>{67,35,19},{1,1,33},{200,113,7}}) {
		TileSurfaceLayout layout{};
		Check(TileGetTiledTextureLayout({format,tile,TileSurfaceDimension::Dim3D,shape[0],shape[1],shape[2],8,1},layout),"test layout");
		for(uint32_t mip=0;mip<8;++mip) {
			const auto expected=Oracle(layout,mip);std::vector<FootprintRange> out;
			Check(TiledVolumeMipFootprint(layout,mip,out) && Same(out,expected),"cold footprint oracle");
			out={{123,456}};
			Check(TiledVolumeMipFootprint(layout,mip,out) && Same(out,expected),"warm footprint oracle");++cases;
		}
		// Identical X/Y layout with a new logical depth must not reuse old bytes.
		auto changed=layout;--changed.description.depth;std::vector<FootprintRange> out;
		Check(TiledVolumeMipFootprint(changed,0,out) && Same(out,Oracle(changed,0)),"depth absent from cache key");++cases;
		for(unsigned field=0;field<9;++field) {
			auto bad=layout;
			switch(field){case 0:bad.description.dimension=TileSurfaceDimension::Dim2D;break;
			case 1:bad.description.layers=2;break;case 2:bad.description.levels=0;break;
			case 3:bad.texture.block.block_depth=0;break;case 4:bad.texture.block.block_size=0;break;
			case 5:bad.texture.block.bytes_per_element=0;break;case 6:bad.total_size=1;break;
			case 7:bad.mips[0].width=0;break;case 8:bad.mips[0].padded_height=0;break;}
			out={{123,456}};Check(!TiledVolumeMipFootprint(bad,0,out) && out.empty(),"invalid layout reused cached ranges");++negatives;
		}
	}
	TileSurfaceLayout l{};Check(TileGetTiledTextureLayout({Prospero::BufferFormat::k32UInt,Prospero::TileMode::kStandard64KB,
	    TileSurfaceDimension::Dim3D,200,113,7,8,1},l),"benchmark layout");
	std::vector<FootprintRange> out;const auto start=std::chrono::steady_clock::now();
	for(unsigned i=0;i<100;++i)Check(TiledVolumeMipFootprintCompute(l,0,out),"reference benchmark");
	const auto middle=std::chrono::steady_clock::now();
	for(unsigned i=0;i<100;++i)Check(TiledVolumeMipFootprint(l,0,out),"cached benchmark");
	const auto end=std::chrono::steady_clock::now();
	std::printf("Volume footprint cache: %u scalar-oracle layouts/mips, %u negatives PASS; 100 calls compute %.3fms / cache %.3fms (CPU only, not FPS)\n",
	    cases,negatives,std::chrono::duration<double,std::milli>(middle-start).count(),std::chrono::duration<double,std::milli>(end-middle).count());
	return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
