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
	return DecodeTiledVolumeLevel(source, layout, 0, linear);
}

bool DecodeTiledVolumeLevel(std::span<const uint8_t> source, const TileSurfaceLayout& layout,
                            uint32_t level, std::vector<uint8_t>& linear) {
	linear.clear();
	const auto& block = layout.texture.block;
	if (layout.description.dimension != TileSurfaceDimension::Dim3D || block.block_width == 0 || block.block_height == 0 ||
	    block.block_depth == 0 || block.bytes_per_element == 0 || layout.description.depth == 0 || level >= 16 || level >= layout.description.levels) return false;
	const auto& mip = layout.mips[level];
	const uint32_t w = mip.width, h = mip.height, d = std::max(layout.description.depth >> level, 1u);
	if (w == 0 || h == 0 || d == 0 || mip.padded_width % block.block_width != 0) return false;
	const bool tail = level >= layout.first_tail_level;
	const uint32_t bpe = block.bytes_per_element;
	if (uint64_t(w) * h > source.size() / bpe / d) return false;
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

bool UploadTiledVolumeMipChain(std::span<const uint8_t> source, const TileSurfaceLayout& layout, id<MTLTexture> target) {
	const uint32_t levels = layout.description.levels, eb = layout.texture.block.bytes_per_element;
	if (target == nil || target.textureType != MTLTextureType3D || levels == 0 || levels > 16 ||
	    target.storageMode != MTLStorageModeShared ||
	    target.mipmapLevelCount < levels || target.pixelFormat != TileElementFormat(eb) || source.size() < layout.total_size ||
	    target.width != layout.mips[0].width || target.height != layout.mips[0].height || target.depth != layout.description.depth) return false;
	std::vector<std::vector<uint8_t>> decoded(levels);
	for (uint32_t level = 0; level < levels; ++level) {
		if (!DecodeTiledVolumeLevel(source, layout, level, decoded[level])) return false;
	}
	for (uint32_t level = 0; level < levels; ++level) {
		const auto& mip = layout.mips[level];
		const uint32_t depth = std::max(layout.description.depth >> level, 1u);
		[target replaceRegion:MTLRegionMake3D(0,0,0,mip.width,mip.height,depth) mipmapLevel:level slice:0
		             withBytes:decoded[level].data() bytesPerRow:NSUInteger(mip.width)*eb bytesPerImage:NSUInteger(mip.width)*mip.height*eb];
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

bool UploadTiledArrayMipChain(std::span<const uint8_t> source, const TileSurfaceLayout& layout, id<MTLTexture> target) {
	const uint32_t levels = layout.description.levels, layers = layout.description.layers;
	const uint32_t eb = layout.texture.block.bytes_per_element;
	if (target == nil || target.textureType != MTLTextureType2DArray ||
	    layout.description.dimension != TileSurfaceDimension::Dim2D || levels == 0 || levels > 16 || layers == 0 ||
	    target.storageMode != MTLStorageModeShared || target.mipmapLevelCount < levels || target.arrayLength < layers ||
	    target.pixelFormat != TileElementFormat(eb) || source.size() < layout.total_size ||
	    target.width != layout.mips[0].width || target.height != layout.mips[0].height) return false;
	std::vector<std::vector<uint8_t>> decoded(size_t(levels) * layers);
	std::vector<uint32_t> pitches(size_t(levels) * layers);
	for (uint32_t layer = 0; layer < layers; ++layer) {
		for (uint32_t level = 0; level < levels; ++level) {
			const size_t index = size_t(layer) * levels + level;
			if (!DecodeTiledTextureLevel(source, layout, level, layer, decoded[index], pitches[index])) return false;
		}
	}
	for (uint32_t layer = 0; layer < layers; ++layer) {
		for (uint32_t level = 0; level < levels; ++level) {
			const size_t index = size_t(layer) * levels + level;
			const auto& mip = layout.mips[level];
			[target replaceRegion:MTLRegionMake2D(0, 0, std::max(target.width >> level, NSUInteger(1)),
			                                               std::max(target.height >> level, NSUInteger(1)))
			          mipmapLevel:level slice:layer withBytes:decoded[index].data()
			          bytesPerRow:pitches[index] bytesPerImage:NSUInteger(pitches[index]) * mip.height];
		}
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
	out.in_z.resize(block.block_depth);
	for (uint32_t x = 0; x < block.block_width; ++x) {
		if (!TileGetBlockOffset(block, x, 0, 0, out.in_x[x])) return false;
	}
	for (uint32_t y = 0; y < block.block_height; ++y) {
		if (!TileGetBlockOffset(block, 0, y, 0, out.in_y[y])) return false;
	}
	for (uint32_t z = 0; z < block.block_depth; ++z) {
		if (!TileGetBlockOffset(block, 0, 0, z, out.in_z[z])) return false;
	}
	// The generic kernel relies on offset(x, y) == in_x[x] ^ in_y[y]: verify it against the reference on a grid.
	for (uint32_t z = 0; z < block.block_depth; ++z) {
		for (uint32_t y = 0; y < block.block_height; y += (block.block_height > 16 ? 3 : 1)) {
			for (uint32_t x = 0; x < block.block_width; x += (block.block_width > 16 ? 5 : 1)) {
				uint32_t ref = 0;
				if (!TileGetBlockOffset(block, x, y, z, ref) || ref != (out.in_x[x] ^ out.in_y[y] ^ out.in_z[z])) return false;
			}
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
struct P { uint width, height, columns, rows, block_w, block_h, block_bytes, eb, bank_v0, layer_xor, ncomp, comp_bytes, tile_x, tile_y, origin_x, origin_y; };
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
	g += uint2(p.origin_x,p.origin_y);
	uint4 v = src.read(g);
	ulong off = tiled_offset(g, p, ix, iy, bxt, byt);
	for (uint i = 0; i < p.eb; ++i) T[off + i] = uchar((v[i / p.comp_bytes] >> ((i % p.comp_bytes) * 8u)) & 0xffu);
}
kernel void kyty_tile_import(texture2d<uint, access::write> dst [[texture(0)]], device const uchar* T [[buffer(0)]], constant P& p [[buffer(1)]],
                             device const uint* ix [[buffer(2)]], device const uint* iy [[buffer(3)]],
                             device const uint* bxt [[buffer(4)]], device const uint* byt [[buffer(5)]], uint2 g [[thread_position_in_grid]]) {
	if (g.x >= p.width || g.y >= p.height) return;
	g += uint2(p.origin_x,p.origin_y);
	ulong off = tiled_offset(g, p, ix, iy, bxt, byt);
	uint4 v = uint4(0u);
	for (uint i = 0; i < p.eb; ++i) v[i / p.comp_bytes] |= uint(T[off + i]) << ((i % p.comp_bytes) * 8u);
	dst.write(v, g);
}
struct V { uint width, height, depth, columns, block_w, block_h, block_d, block_bytes, eb, comp_bytes,
                bank_v0, layer_banks, level, tile_x, tile_y, pad; ulong slab_bytes, mip_offset; };
static ulong volume_offset(uint3 g, constant V& p, device const uint* ix, device const uint* iy,
                            device const uint* iz, device const uint* bxt, device const uint* byt) {
	g.xy += uint2(p.tile_x, p.tile_y);
	uint bx = g.x / p.block_w, by = g.y / p.block_h, bz = g.z / p.block_d;
	uint within = ix[g.x % p.block_w] ^ iy[g.y % p.block_h] ^ iz[g.z % p.block_d];
	uint layer = p.layer_banks ? (((bz & 8u) << 5u) ^ ((bz & 4u) << 7u) ^ ((bz & 2u) << 9u) ^ ((bz & 1u) << 11u)) : 0u;
	uint bank = bxt[bx] ^ byt[by] ^ p.bank_v0 ^ layer;
	return ulong(bz) * p.slab_bytes + p.mip_offset + ulong(by * p.columns + bx) * p.block_bytes + (within ^ bank);
}
kernel void kyty_volume_import(texture3d<uint, access::write> dst [[texture(0)]], device const uchar* T [[buffer(0)]],
    constant V& p [[buffer(1)]], device const uint* ix [[buffer(2)]], device const uint* iy [[buffer(3)]],
    device const uint* iz [[buffer(4)]], device const uint* bx [[buffer(5)]], device const uint* by [[buffer(6)]], uint3 g [[thread_position_in_grid]]) {
	if (g.x >= p.width || g.y >= p.height || g.z >= p.depth) return;
	ulong off = volume_offset(g, p, ix, iy, iz, bx, by);
	uint4 v = uint4(0u);
	for (uint i = 0; i < p.eb; ++i) v[i / p.comp_bytes] |= uint(T[off + i]) << ((i % p.comp_bytes) * 8u);
	dst.write(v, g, p.level);
}
kernel void kyty_volume_export(texture3d<uint, access::read> src [[texture(0)]], device uchar* T [[buffer(0)]],
    constant V& p [[buffer(1)]], device const uint* ix [[buffer(2)]], device const uint* iy [[buffer(3)]],
    device const uint* iz [[buffer(4)]], device const uint* bx [[buffer(5)]], device const uint* by [[buffer(6)]], uint3 g [[thread_position_in_grid]]) {
	if (g.x >= p.width || g.y >= p.height || g.z >= p.depth) return;
	ulong off = volume_offset(g, p, ix, iy, iz, bx, by);
	uint4 v = src.read(g, p.level);
	for (uint i = 0; i < p.eb; ++i) T[off + i] = uchar(v[i / p.comp_bytes] >> ((i % p.comp_bytes) * 8u));
}
)MSL";

struct Pipelines {
	id<MTLComputePipelineState> exp = nil;
	id<MTLComputePipelineState> imp = nil;
	id<MTLComputePipelineState> exp3d = nil;
	id<MTLComputePipelineState> imp3d = nil;
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
		s_p.exp3d = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"kyty_volume_export"] error:&err];
		s_p.imp3d = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"kyty_volume_import"] error:&err];
	});
	return s_p;
}

struct Params {
	uint32_t width, height, columns, rows, block_w, block_h, block_bytes, eb, bank_v0, layer_xor, ncomp, comp_bytes, tile_x, tile_y, origin_x, origin_y;
};

// Layer term of TileGetBlockXor for the render-target and depth families (zero for every other family and for layer 0).
uint32_t LayerXor(const TileBlockLayout& block, uint32_t layer) {
	if (block.family != TileBlockFamily::RenderTarget64KB && block.family != TileBlockFamily::Depth64KB) return 0;
	return ((layer & 8u) << 5u) ^ ((layer & 4u) << 7u) ^ ((layer & 2u) << 9u) ^ ((layer & 1u) << 11u);
}

// setBytes is limited to 4 KiB. Wide surfaces need ordinary buffers for bank
// tables; keep them alive even for command buffers using unretained references.
bool SetTable(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLComputeCommandEncoder> encoder,
              const std::vector<uint32_t>& values, NSUInteger index) {
	const NSUInteger bytes = values.size() * sizeof(uint32_t);
	if (bytes <= 4096) { [encoder setBytes:values.data() length:bytes atIndex:index]; return true; }
	id<MTLBuffer> buffer = [device newBufferWithBytes:values.data() length:bytes options:MTLResourceStorageModeShared];
	if (buffer == nil) return false;
	[encoder setBuffer:buffer offset:0 atIndex:index];
	[cmd addCompletedHandler:^(id<MTLCommandBuffer>) { (void)buffer.length; }];
	return true;
}

bool ValidTransfer(id<MTLDevice> device, id<MTLTexture> texture, id<MTLBuffer> tiled, NSUInteger tiled_offset,
                   const TileTables& t, uint32_t width, uint32_t height, uint32_t tile_x=0, uint32_t tile_y=0) {
	const uint64_t blocks=uint64_t(t.columns)*t.rows;
	if (device == nil || !t.valid || texture == nil || tiled == nil || t.block.block_depth != 1 ||
	    t.block.block_size == 0 || blocks == 0 || blocks > UINT64_MAX/t.block.block_size ||
	    texture.textureType != MTLTextureType2D || width == 0 || height == 0 || tiled_offset > tiled.length ||
	    blocks*t.block.block_size > tiled.length-tiled_offset || texture.device != device || tiled.device != device) return false;
	if (tile_x > uint64_t(t.columns) * t.block.block_width || tile_y > uint64_t(t.rows) * t.block.block_height ||
	    width > uint64_t(t.columns) * t.block.block_width - tile_x || height > uint64_t(t.rows) * t.block.block_height - tile_y) return false;
	return texture.pixelFormat == TileElementFormat(t.block.bytes_per_element) && width <= texture.width && height <= texture.height;
}

bool Encode(id<MTLDevice> device, id<MTLCommandBuffer> cmd, bool exporting, id<MTLTexture> texture, id<MTLBuffer> tiled,
	            NSUInteger tiled_offset, const TileTables& t, uint32_t width, uint32_t height, uint32_t layer, uint32_t tile_x, uint32_t tile_y,
	            uint32_t origin_x=0,uint32_t origin_y=0) {
	if (cmd == nil || origin_x>UINT32_MAX-width || origin_y>UINT32_MAX-height ||
	    !ValidTransfer(device,texture,tiled,tiled_offset,t,origin_x+width,origin_y+height,tile_x,tile_y)) return false;
	// Renderable sRGB and other read-only typed resources can expose a uint
	// view, but that view inherits usage without ShaderWrite. Import through a
	// real writable uint image and a raw buffer blit; never dispatch a texture
	// store against the read-only view or numerically convert its bits.
	if(!exporting && !(texture.usage & MTLTextureUsageShaderWrite)) {
		const uint64_t row=(uint64_t(width)*t.block.bytes_per_element+255u)&~255ull;
		if(height>UINT64_MAX/row || row*height>device.maxBufferLength)return false;
		MTLTextureDescriptor* d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:texture.pixelFormat width:texture.width height:texture.height mipmapped:NO];
		d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
		id<MTLTexture> writable=[device newTextureWithDescriptor:d];
		id<MTLBuffer> linear=[device newBufferWithLength:row*height options:MTLResourceStorageModePrivate];
		if(!writable || !linear || !Encode(device,cmd,false,writable,tiled,tiled_offset,t,width,height,layer,tile_x,tile_y,origin_x,origin_y))return false;
		id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder];if(!blit)return false;OrderBegin(blit);
		[blit copyFromTexture:writable sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(origin_x,origin_y,0)
		    sourceSize:MTLSizeMake(width,height,1) toBuffer:linear destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row*height];
		[blit copyFromBuffer:linear sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row*height sourceSize:MTLSizeMake(width,height,1)
		    toTexture:texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(origin_x,origin_y,0)];
		OrderEnd(blit);[blit endEncoding];
		[cmd addCompletedHandler:^(id<MTLCommandBuffer>){(void)writable.width;(void)linear.length;}];
		return true;
	}
	Pipelines& pl = GetPipelines(device);
	id<MTLComputePipelineState> pso = exporting ? pl.exp : pl.imp;
	if (pso == nil) return false;
	Params p {width, height, t.columns, t.rows, t.block.block_width, t.block.block_height, t.block.block_size, t.block.bytes_per_element,
	          t.bank_v0, LayerXor(t.block, layer), std::max(t.block.bytes_per_element / 4u, 1u), std::min(t.block.bytes_per_element, 4u), tile_x, tile_y,origin_x,origin_y};
	id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
	OrderBegin(enc);
	[enc setComputePipelineState:pso];
	[enc setTexture:texture atIndex:0];
	[enc setBuffer:tiled offset:tiled_offset atIndex:0];
	[enc setBytes:&p length:sizeof(p) atIndex:1];
	const bool tables_ok = SetTable(device, cmd, enc, t.in_x, 2) && SetTable(device, cmd, enc, t.in_y, 3) &&
	    SetTable(device, cmd, enc, t.bank_x, 4) && SetTable(device, cmd, enc, t.bank_y, 5);
	if (!tables_ok) { OrderEnd(enc); [enc endEncoding]; return false; }
	[enc useResource:tiled usage:MTLResourceUsageRead | MTLResourceUsageWrite];
	[enc dispatchThreads:MTLSizeMake(width, height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
	OrderEnd(enc);
	[enc endEncoding];
	return true;
}

struct VolumeParams {
	uint32_t width, height, depth, columns, block_w, block_h, block_d, block_bytes, eb, comp_bytes;
	uint32_t bank_v0, layer_banks, level, tile_x, tile_y, pad;
	uint64_t slab_bytes, mip_offset;
};

bool EncodeVolume(id<MTLDevice> device, id<MTLCommandBuffer> cmd, bool exporting, id<MTLTexture> texture,
                   id<MTLBuffer> tiled, NSUInteger offset, const TileSurfaceLayout& layout, uint32_t level, uint32_t texture_level) {
	if (device == nil || cmd == nil || texture == nil || tiled == nil ||
	    layout.description.dimension != TileSurfaceDimension::Dim3D || level >= 16 || level >= layout.description.levels ||
	    texture.textureType != MTLTextureType3D || texture_level >= texture.mipmapLevelCount || offset > tiled.length ||
	    layout.total_size > tiled.length - offset || layout.description.depth == 0 || layout.block_slice_size == 0) return false;
	const auto& m = layout.mips[level]; const auto& b = layout.texture.block;
	const uint32_t depth = std::max(layout.description.depth >> level, 1u);
	if (texture.pixelFormat != TileElementFormat(b.bytes_per_element) || m.width == 0 || m.height == 0 ||
	    m.width > std::max<NSUInteger>(texture.width >> texture_level, 1) ||
	    m.height > std::max<NSUInteger>(texture.height >> texture_level, 1) ||
	    depth > std::max<NSUInteger>(texture.depth >> texture_level, 1)) return false;
	TileTables t;
	if (!BuildTileTables(b, m.padded_width, m.padded_height, t)) return false;
	const bool tail = level >= layout.first_tail_level;
	const uint32_t tx = tail ? m.tail_x : 0, ty = tail ? m.tail_y : 0;
	if (uint64_t(tx) + m.width > uint64_t(t.columns) * b.block_width ||
	    uint64_t(ty) + m.height > uint64_t(t.rows) * b.block_height) return false;
	const uint64_t slab = (depth - 1u) / b.block_depth;
	if (slab > layout.total_size / layout.block_slice_size) return false;
	const uint64_t start = slab * layout.block_slice_size;
	if (start > layout.total_size || m.offset > layout.total_size - start ||
	    uint64_t(t.columns) * t.rows * b.block_size > layout.total_size - start - m.offset) return false;
	auto& pipelines = GetPipelines(device);
	id<MTLComputePipelineState> pso = exporting ? pipelines.exp3d : pipelines.imp3d;
	if (pso == nil) return false;
	VolumeParams p {m.width, m.height, depth, t.columns, b.block_width, b.block_height, b.block_depth,
	                b.block_size, b.bytes_per_element, std::min(b.bytes_per_element, 4u), t.bank_v0,
	                b.family == TileBlockFamily::RenderTarget64KB || b.family == TileBlockFamily::Depth64KB,
	                texture_level, tx, ty, 0, layout.block_slice_size, m.offset};
	id<MTLComputeCommandEncoder> encoder = [cmd computeCommandEncoder];
	OrderBegin(encoder); [encoder setComputePipelineState:pso]; [encoder setTexture:texture atIndex:0];
	[encoder setBuffer:tiled offset:offset atIndex:0]; [encoder setBytes:&p length:sizeof(p) atIndex:1];
	const bool ok = SetTable(device, cmd, encoder, t.in_x, 2) && SetTable(device, cmd, encoder, t.in_y, 3) &&
	    SetTable(device, cmd, encoder, t.in_z, 4) && SetTable(device, cmd, encoder, t.bank_x, 5) && SetTable(device, cmd, encoder, t.bank_y, 6);
	if (ok) [encoder dispatchThreads:MTLSizeMake(m.width,m.height,depth) threadsPerThreadgroup:MTLSizeMake(4,4,4)];
	OrderEnd(encoder); [encoder endEncoding];
	return ok;
}

} // namespace

bool EncodeTileExport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLTexture> texture, id<MTLBuffer> tiled,
                      NSUInteger tiled_offset, const TileTables& tables, uint32_t width, uint32_t height, uint32_t layer,
                      uint32_t tile_x, uint32_t tile_y) {
	return Encode(device, cmd, true, texture, tiled, tiled_offset, tables, width, height, layer, tile_x, tile_y);
}

bool EncodeTileImport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLBuffer> tiled, NSUInteger tiled_offset,
                      id<MTLTexture> texture, const TileTables& tables, uint32_t width, uint32_t height, uint32_t layer,
                      uint32_t tile_x, uint32_t tile_y) {
	return Encode(device, cmd, false, texture, tiled, tiled_offset, tables, width, height, layer, tile_x, tile_y);
}

static bool EncodeTextureLevel(id<MTLDevice> device, id<MTLCommandBuffer> cmd, bool exporting, id<MTLTexture> texture,
                               id<MTLBuffer> tiled, NSUInteger offset, const TileSurfaceLayout& layout,
                               uint32_t level, uint32_t layer, uint32_t texture_level, uint32_t texture_slice,
                               const PixelRect* region=nullptr) {
	if (device == nil || cmd == nil || texture == nil || tiled == nil ||
	    layout.description.dimension != TileSurfaceDimension::Dim2D || layout.description.levels == 0 ||
	    layout.description.levels > 16 || level >= layout.description.levels || layer >= layout.description.layers ||
	    texture_level >= texture.mipmapLevelCount ||
	    (texture.textureType != MTLTextureType2D && texture.textureType != MTLTextureType2DArray) ||
	    texture_slice >= texture.arrayLength ||
	    texture.pixelFormat != TileElementFormat(layout.texture.block.bytes_per_element) ||
	    offset > tiled.length || layout.total_size > tiled.length - offset) return false;
	const auto& mip = layout.mips[level];
	const PixelRect rect=region?*region:PixelRect{0,0,mip.width,mip.height};
	if(!rect.width || !rect.height || rect.x>mip.width || rect.y>mip.height || rect.width>mip.width-rect.x || rect.height>mip.height-rect.y)return false;
	if (std::max(texture.width >> texture_level, NSUInteger(1)) != mip.width ||
	    std::max(texture.height >> texture_level, NSUInteger(1)) != mip.height) return false;
	TileTables tables;
	if (!BuildTileTables(layout.texture.block, mip.padded_width, mip.padded_height, tables)) return false;
	const uint64_t base = uint64_t(layer) * layout.block_slice_size + mip.offset;
	const uint64_t extent = uint64_t(tables.columns) * tables.rows * tables.block.block_size;
	if (base > layout.total_size || extent > layout.total_size - base) return false;
	id<MTLTexture> view = texture;
	if (texture.textureType != MTLTextureType2D || texture_level != 0 || texture.mipmapLevelCount != 1) {
		view = [texture newTextureViewWithPixelFormat:texture.pixelFormat textureType:MTLTextureType2D
		                                      levels:NSMakeRange(texture_level, 1) slices:NSMakeRange(texture_slice, 1)];
	}
	const bool tail = level >= layout.first_tail_level;
	return Encode(device, cmd, exporting, view, tiled, offset + base, tables, rect.width, rect.height, layer,
	              tail ? mip.tail_x : 0, tail ? mip.tail_y : 0,rect.x,rect.y);
}

bool EncodeTileTextureImport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLBuffer> tiled, NSUInteger offset,
                             id<MTLTexture> texture, const TileSurfaceLayout& layout, uint32_t level, uint32_t layer,
                             uint32_t texture_level, uint32_t texture_slice) {
	return EncodeTextureLevel(device, cmd, false, texture, tiled, offset, layout, level, layer, texture_level, texture_slice);
}

bool EncodeTileTextureExport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLTexture> texture, id<MTLBuffer> tiled,
                             NSUInteger offset, const TileSurfaceLayout& layout, uint32_t level, uint32_t layer,
                             uint32_t texture_level, uint32_t texture_slice) {
	return EncodeTextureLevel(device, cmd, true, texture, tiled, offset, layout, level, layer, texture_level, texture_slice);
}

bool EncodeTileTextureRectExport(id<MTLDevice> device,id<MTLCommandBuffer> cmd,id<MTLTexture> texture,id<MTLBuffer> tiled,
    NSUInteger offset,const TileSurfaceLayout& layout,PixelRect rect,uint32_t level,uint32_t layer,uint32_t texture_level,uint32_t texture_slice) {
	return EncodeTextureLevel(device,cmd,true,texture,tiled,offset,layout,level,layer,texture_level,texture_slice,&rect);
}

#include "metalBcTransfer.inc"

bool EncodeTileAlias(id<MTLDevice> device, id<MTLCommandBuffer> cmd,
                     id<MTLBuffer> guest, NSUInteger guest_offset, id<MTLBuffer> scratch,
                     id<MTLTexture> source, const TileTables& source_tables, uint32_t source_width, uint32_t source_height,
                     id<MTLTexture> target, const TileTables& target_tables, uint32_t target_width, uint32_t target_height) {
	if (cmd == nil || guest == nil || scratch == nil || scratch == guest || source == target ||
	    !ValidTransfer(device,source,scratch,0,source_tables,source_width,source_height) ||
	    !ValidTransfer(device,target,scratch,0,target_tables,target_width,target_height)) return false;
	const uint64_t source_bytes=uint64_t(source_tables.columns)*source_tables.rows*source_tables.block.block_size;
	const uint64_t target_bytes=uint64_t(target_tables.columns)*target_tables.rows*target_tables.block.block_size;
	const uint64_t bytes=std::max(source_bytes,target_bytes);
	if (guest.device != device || guest_offset > guest.length || bytes > guest.length-guest_offset ||
	    (source.usage&MTLTextureUsageShaderRead)==0 || (target.usage&MTLTextureUsageShaderWrite)==0) return false;
	auto& pipelines=GetPipelines(device);
	if (pipelines.exp == nil || pipelines.imp == nil) return false;
	id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder];
	if (blit == nil) return false;
	OrderBegin(blit);
	[blit copyFromBuffer:guest sourceOffset:guest_offset toBuffer:scratch destinationOffset:0 size:bytes];
	OrderEnd(blit); [blit endEncoding];
	return EncodeTileExport(device,cmd,source,scratch,0,source_tables,source_width,source_height) &&
	       EncodeTileImport(device,cmd,scratch,0,target,target_tables,target_width,target_height);
}

bool EncodeTileVolumeImport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLBuffer> tiled, NSUInteger tiled_offset,
                            id<MTLTexture> texture, const TileSurfaceLayout& layout, uint32_t level, uint32_t texture_level) {
	return EncodeVolume(device, cmd, false, texture, tiled, tiled_offset, layout, level, texture_level);
}

bool EncodeTileVolumeExport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLTexture> texture, id<MTLBuffer> tiled,
                            NSUInteger tiled_offset, const TileSurfaceLayout& layout, uint32_t level, uint32_t texture_level) {
	return EncodeVolume(device, cmd, true, texture, tiled, tiled_offset, layout, level, texture_level);
}

} // namespace Libs::Graphics::Metal
