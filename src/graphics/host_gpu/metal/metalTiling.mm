#include "graphics/host_gpu/metal/metalOrder.h"
#include "graphics/host_gpu/metal/metalTiling.h"

#include <cstdio>
#include <cstring>
#include <mutex>

namespace Libs::Graphics::Metal {

MTLPixelFormat TileElementFormat(uint32_t element_bytes) {
	switch (element_bytes) {
		case 1: return MTLPixelFormatR8Uint;
		case 2: return MTLPixelFormatR16Uint;
		case 4: return MTLPixelFormatR32Uint;
		case 8: return MTLPixelFormatRG32Uint;
		case 16: return MTLPixelFormatRGBA32Uint;
		default: return MTLPixelFormatInvalid;
	}
}

bool DecodeTiledVolume(std::span<const uint8_t> source, const TileSurfaceLayout& layout, std::vector<uint8_t>& linear) {
	linear.clear();
	const auto& block = layout.texture.block;
	if (layout.description.dimension != TileSurfaceDimension::Dim3D || block.block_width == 0 || block.block_height == 0 ||
	    block.block_depth == 0 || block.bytes_per_element == 0) return false;
	const auto& mip = layout.mips[0];
	const uint32_t w = mip.width, h = mip.height, d = layout.description.depth;
	if (w == 0 || h == 0 || d == 0 || mip.padded_width % block.block_width != 0) return false;
	const bool tail = layout.first_tail_level == 0; // a small volume lives in the shared mip tail of its block
	const uint32_t bpe = block.bytes_per_element;
	linear.resize(static_cast<size_t>(w) * h * d * bpe);
	for (uint32_t z = 0; z < d; ++z) for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
		const uint32_t bx = tail ? 0u : x / block.block_width, by = tail ? 0u : y / block.block_height, bz = tail ? 0u : z / block.block_depth;
		uint32_t within = 0, bank = 0;
		if (!TileGetBlockOffset(block, tail ? x + mip.tail_x : x % block.block_width, tail ? y + mip.tail_y : y % block.block_height,
		                        tail ? z : z % block.block_depth, within) ||
		    !TileGetBlockXor(block, bx, by, bz, bank)) {
			linear.clear(); return false;
		}
		const uint64_t offset = uint64_t(bz) * layout.block_slice_size + mip.offset +
		    uint64_t(by * (mip.padded_width / block.block_width) + bx) * block.block_size + (within ^ bank);
		if (offset > source.size() || bpe > source.size() - offset) { linear.clear(); return false; }
		std::memcpy(linear.data() + ((static_cast<size_t>(z) * h + y) * w + x) * bpe, source.data() + offset, bpe);
	}
	return true;
}

bool DecodeTiledTextureLevel(std::span<const uint8_t> source, const TileSurfaceLayout& layout,
                             uint32_t level, uint32_t layer, std::vector<uint8_t>& linear,
                             uint32_t& pitch) {
	linear.clear();
	pitch = 0;
	const auto& block = layout.texture.block;
	if (layout.description.dimension != TileSurfaceDimension::Dim2D ||
	    level >= layout.description.levels || level >= 16 || layer >= layout.description.layers ||
	    block.block_width == 0 || block.block_height == 0 || block.bytes_per_element == 0) return false;
	const auto& mip = layout.mips[level];
	if (mip.width == 0 || mip.height == 0 || mip.padded_width % block.block_width != 0 ||
	    mip.width > UINT32_MAX / block.bytes_per_element) return false;
	pitch = mip.width * block.bytes_per_element;
	linear.resize(static_cast<size_t>(pitch) * mip.height);
	const bool tail = level >= layout.first_tail_level;
	for (uint32_t y = 0; y < mip.height; ++y) for (uint32_t x = 0; x < mip.width; ++x) {
		const uint32_t bx = tail ? 0u : x / block.block_width;
		const uint32_t by = tail ? 0u : y / block.block_height;
		uint32_t within = 0, bank = 0;
		if (!TileGetBlockOffset(block, tail ? x + mip.tail_x : x % block.block_width,
		                        tail ? y + mip.tail_y : y % block.block_height, 0, within) ||
		    !TileGetBlockXor(block, bx, by, layer, bank)) {
			linear.clear(); pitch = 0; return false;
		}
		const uint64_t offset = uint64_t(layer) * layout.block_slice_size + mip.offset +
		    uint64_t(by * (mip.padded_width / block.block_width) + bx) * block.block_size + (within ^ bank);
		if (offset > source.size() || block.bytes_per_element > source.size() - offset) {
			linear.clear(); pitch = 0; return false;
		}
		std::memcpy(linear.data() + static_cast<size_t>(y) * pitch + x * block.bytes_per_element,
		            source.data() + offset, block.bytes_per_element);
	}
	return true;
}

bool BuildTileTables(const TileBlockLayout& block, uint32_t pitch, uint32_t height, TileTables& out) {
	out = {};
	if (block.block_width == 0 || block.block_height == 0 || block.bytes_per_element == 0 || pitch == 0 || height == 0) return false;
	out.block   = block;
	out.columns = (pitch + block.block_width - 1) / block.block_width;
	out.rows    = (height + block.block_height - 1) / block.block_height;
	out.in_x.resize(block.block_width);
	out.in_y.resize(block.block_height);
	for (uint32_t x = 0; x < block.block_width; ++x) {
		if (!TileGetBlockOffset(block, x, 0, 0, out.in_x[x])) return false;
	}
	for (uint32_t y = 0; y < block.block_height; ++y) {
		if (!TileGetBlockOffset(block, 0, y, 0, out.in_y[y])) return false;
	}
	// The generic kernel relies on offset(x, y) == in_x[x] ^ in_y[y]: verify it against the reference on a grid.
	for (uint32_t y = 0; y < block.block_height; y += (block.block_height > 16 ? 3 : 1)) {
		for (uint32_t x = 0; x < block.block_width; x += (block.block_width > 16 ? 5 : 1)) {
			uint32_t ref = 0;
			if (!TileGetBlockOffset(block, x, y, 0, ref) || ref != (out.in_x[x] ^ out.in_y[y])) return false;
		}
	}
	out.bank_x.resize(out.columns);
	out.bank_y.resize(out.rows);
	if (!TileGetBlockXor(block, 0, 0, out.bank_v0)) return false;
	for (uint32_t bx = 0; bx < out.columns; ++bx) {
		uint32_t v = 0;
		if (!TileGetBlockXor(block, bx, 0, v)) return false;
		out.bank_x[bx] = v ^ out.bank_v0;
	}
	for (uint32_t by = 0; by < out.rows; ++by) {
		uint32_t v = 0;
		if (!TileGetBlockXor(block, 0, by, v)) return false;
		out.bank_y[by] = v ^ out.bank_v0;
	}
	for (uint32_t by = 0; by < out.rows; by += (out.rows > 8 ? 7 : 1)) {
		for (uint32_t bx = 0; bx < out.columns; bx += (out.columns > 8 ? 7 : 1)) {
			uint32_t ref = 0;
			if (!TileGetBlockXor(block, bx, by, ref) || ref != (out.bank_x[bx] ^ out.bank_y[by] ^ out.bank_v0)) return false;
		}
	}
	out.valid = true;
	return true;
}

namespace {

const char* kTileMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct P { uint width, height, columns, rows, block_w, block_h, block_bytes, eb, bank_v0, layer_xor, ncomp, comp_bytes, tile_x, tile_y; };
static ulong tiled_offset(uint2 g, constant P& p, device const uint* ix, device const uint* iy, device const uint* bxt, device const uint* byt) {
	g += uint2(p.tile_x, p.tile_y);
	uint bx = g.x / p.block_w, by = g.y / p.block_h;
	uint within = ix[g.x % p.block_w] ^ iy[g.y % p.block_h];
	uint bank = bxt[bx] ^ byt[by] ^ p.bank_v0 ^ p.layer_xor;
	return ulong(by * p.columns + bx) * p.block_bytes + (within ^ bank);
}
kernel void kyty_tile_export(texture2d<uint, access::read> src [[texture(0)]], device uchar* T [[buffer(0)]], constant P& p [[buffer(1)]],
                             device const uint* ix [[buffer(2)]], device const uint* iy [[buffer(3)]],
                             device const uint* bxt [[buffer(4)]], device const uint* byt [[buffer(5)]], uint2 g [[thread_position_in_grid]]) {
	if (g.x >= p.width || g.y >= p.height) return;
	uint4 v = src.read(g);
	ulong off = tiled_offset(g, p, ix, iy, bxt, byt);
	for (uint i = 0; i < p.eb; ++i) T[off + i] = uchar((v[i / p.comp_bytes] >> ((i % p.comp_bytes) * 8u)) & 0xffu);
}
kernel void kyty_tile_import(texture2d<uint, access::write> dst [[texture(0)]], device const uchar* T [[buffer(0)]], constant P& p [[buffer(1)]],
                             device const uint* ix [[buffer(2)]], device const uint* iy [[buffer(3)]],
                             device const uint* bxt [[buffer(4)]], device const uint* byt [[buffer(5)]], uint2 g [[thread_position_in_grid]]) {
	if (g.x >= p.width || g.y >= p.height) return;
	ulong off = tiled_offset(g, p, ix, iy, bxt, byt);
	uint4 v = uint4(0u);
	for (uint i = 0; i < p.eb; ++i) v[i / p.comp_bytes] |= uint(T[off + i]) << ((i % p.comp_bytes) * 8u);
	dst.write(v, g);
}
)MSL";

struct Pipelines {
	id<MTLComputePipelineState> exp = nil;
	id<MTLComputePipelineState> imp = nil;
};

Pipelines& GetPipelines(id<MTLDevice> device) {
	static Pipelines s_p;
	static std::once_flag s_once;
	std::call_once(s_once, [&] {
		NSError*       err = nil;
		id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:kTileMsl] options:nil error:&err];
		if (lib == nil) {
			std::fprintf(stderr, "[Tiling] library failed: %s\n", err ? err.localizedDescription.UTF8String : "?");
			return;
		}
		s_p.exp = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"kyty_tile_export"] error:&err];
		s_p.imp = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"kyty_tile_import"] error:&err];
	});
	return s_p;
}

struct Params {
	uint32_t width, height, columns, rows, block_w, block_h, block_bytes, eb, bank_v0, layer_xor, ncomp, comp_bytes, tile_x, tile_y;
};

// Layer term of TileGetBlockXor for the render-target and depth families (zero for every other family and for layer 0).
uint32_t LayerXor(const TileBlockLayout& block, uint32_t layer) {
	if (block.family != TileBlockFamily::RenderTarget64KB && block.family != TileBlockFamily::Depth64KB) return 0;
	return ((layer & 8u) << 5u) ^ ((layer & 4u) << 7u) ^ ((layer & 2u) << 9u) ^ ((layer & 1u) << 11u);
}

bool Encode(id<MTLDevice> device, id<MTLCommandBuffer> cmd, bool exporting, id<MTLTexture> texture, id<MTLBuffer> tiled,
	            NSUInteger tiled_offset, const TileTables& t, uint32_t width, uint32_t height, uint32_t layer, uint32_t tile_x, uint32_t tile_y) {
	if (!t.valid || texture == nil || tiled == nil || cmd == nil) return false;
	if (tile_x > uint64_t(t.columns) * t.block.block_width || tile_y > uint64_t(t.rows) * t.block.block_height ||
	    width > uint64_t(t.columns) * t.block.block_width - tile_x || height > uint64_t(t.rows) * t.block.block_height - tile_y) return false;
	Pipelines& pl = GetPipelines(device);
	id<MTLComputePipelineState> pso = exporting ? pl.exp : pl.imp;
	if (pso == nil || texture.pixelFormat != TileElementFormat(t.block.bytes_per_element) || width > texture.width || height > texture.height) return false;
	Params p {width, height, t.columns, t.rows, t.block.block_width, t.block.block_height, t.block.block_size, t.block.bytes_per_element,
	          t.bank_v0, LayerXor(t.block, layer), std::max(t.block.bytes_per_element / 4u, 1u), std::min(t.block.bytes_per_element, 4u), tile_x, tile_y};
	id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
	OrderBegin(enc);
	[enc setComputePipelineState:pso];
	[enc setTexture:texture atIndex:0];
	[enc setBuffer:tiled offset:tiled_offset atIndex:0];
	[enc setBytes:&p length:sizeof(p) atIndex:1];
	[enc setBytes:t.in_x.data() length:t.in_x.size() * 4 atIndex:2];
	[enc setBytes:t.in_y.data() length:t.in_y.size() * 4 atIndex:3];
	[enc setBytes:t.bank_x.data() length:t.bank_x.size() * 4 atIndex:4];
	[enc setBytes:t.bank_y.data() length:t.bank_y.size() * 4 atIndex:5];
	[enc useResource:tiled usage:MTLResourceUsageRead | MTLResourceUsageWrite];
	[enc dispatchThreads:MTLSizeMake(width, height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
	OrderEnd(enc);
	[enc endEncoding];
	return true;
}

} // namespace

bool EncodeTileExport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLTexture> texture, id<MTLBuffer> tiled,
                      NSUInteger tiled_offset, const TileTables& tables, uint32_t width, uint32_t height, uint32_t layer,
                      uint32_t tile_x, uint32_t tile_y) {
	return Encode(device, cmd, true, texture, tiled, tiled_offset, tables, width, height, layer, tile_x, tile_y);
}

bool EncodeTileImport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLBuffer> tiled, NSUInteger tiled_offset,
                      id<MTLTexture> texture, const TileTables& tables, uint32_t width, uint32_t height, uint32_t layer) {
	return Encode(device, cmd, false, texture, tiled, tiled_offset, tables, width, height, layer, 0, 0);
}

} // namespace Libs::Graphics::Metal
