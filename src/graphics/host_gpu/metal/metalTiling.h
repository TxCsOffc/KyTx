#pragma once

// GPU conversion between a Metal texture and guest memory in the PS5's tiled layout.
//
// Guest memory is the authority for a surface: the CPU, DMA engines and other passes that alias the same address all see the
// tiled bytes. Metal needs linear textures for sampling and rendering, so surfaces move between the two through these kernels.
// The implemented tile layouts (Standard 256B/4KB/64KB, PRT, render target, depth) are GF(2)-linear in (x, y, z), so a generic
// kernel driven by small lookup tables handles all of them; BuildTileTables self-checks that against the reference
// implementation in guest_gpu/tile.cpp.

#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/metal/metalPixelRect.h"

#import <Metal/Metal.h>

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics::Metal {

struct TileTables {
	TileBlockLayout       block {};
	uint32_t              columns = 0; // blocks per row of the surface
	uint32_t              rows    = 0; // block rows
	uint32_t              bank_v0 = 0; // bank xor of block (0, 0)
	std::vector<uint32_t> in_x;        // in-block byte offset contribution of x
	std::vector<uint32_t> in_y;        // ... of y
	std::vector<uint32_t> in_z;        // ... of z within a thick (3D) block
	std::vector<uint32_t> bank_x;      // bank xor contribution of the block column (relative to block 0)
	std::vector<uint32_t> bank_y;      // ... of the block row
	bool                  valid = false;
};

// `width`/`height` are in addressable elements (a BC block is one element). `pitch` is the padded row width in elements.
bool BuildTileTables(const TileBlockLayout& block, uint32_t pitch, uint32_t height, TileTables& out);

// Decode one 2D array slice/mip from the complete guest allocation. Small mip
// chains share a tile: level 0 can start at tail_x/tail_y rather than (0,0).
// Output rows contain addressable elements (BC blocks for compressed formats).
// A volume (tile.h Dim3D layout) as tightly packed linear voxels. The legacy
// entry point decodes level 0; DecodeTiledVolumeLevel selects a mip explicitly.
bool DecodeTiledVolume(std::span<const uint8_t> source, const TileSurfaceLayout& layout, std::vector<uint8_t>& linear);
bool DecodeTiledVolumeLevel(std::span<const uint8_t> source, const TileSurfaceLayout& layout,
                            uint32_t level, std::vector<uint8_t>& linear);
// CPU-owned snapshot upload. Decode every level before changing the destination;
// target must be a shared uint element texture/view with the full mip chain.
bool UploadTiledVolumeMipChain(std::span<const uint8_t> source, const TileSurfaceLayout& layout,
                               id<MTLTexture> target);

// Materialize every layer/mip of a CPU-owned 2D array snapshot. The target is
// a shared uint element view; reject incomplete snapshots before any writes.
bool UploadTiledArrayMipChain(std::span<const uint8_t> source, const TileSurfaceLayout& layout,
                              id<MTLTexture> target);

bool DecodeTiledTextureLevel(std::span<const uint8_t> source, const TileSurfaceLayout& layout,
                             uint32_t level, uint32_t layer, std::vector<uint8_t>& linear,
                             uint32_t& pitch);

// Texture <-> tiled memory for a 2D level-0 surface. `texture` is read/written as raw elements, so it must be a uint
// format of the element size (R8Uint, R16Uint, R32Uint, RG32Uint, RGBA32Uint) with the matching usage.
// `tiled` + `tiled_offset` is the first byte of the surface. Each call encodes one compute pass into `cmd`.
bool EncodeTileExport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLTexture> texture, id<MTLBuffer> tiled,
                      NSUInteger tiled_offset, const TileTables& tables, uint32_t width, uint32_t height, uint32_t layer = 0,
                      uint32_t tile_x = 0, uint32_t tile_y = 0);
bool EncodeTileImport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLBuffer> tiled, NSUInteger tiled_offset,
                      id<MTLTexture> texture, const TileTables& tables, uint32_t width, uint32_t height, uint32_t layer = 0,
                      uint32_t tile_x = 0, uint32_t tile_y = 0);

// One physical 2D mip/array layer, including shared tails. The buffer offset is
// the start of the entire allocation. Only visible element bytes are written;
// neighbouring mips, tile padding and other layers are never overwritten.
// The uint texture may be a chain or a standalone view of the selected mip.
bool EncodeTileTextureImport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLBuffer> tiled, NSUInteger tiled_offset,
                             id<MTLTexture> texture, const TileSurfaceLayout& layout, uint32_t level = 0, uint32_t layer = 0,
                             uint32_t texture_level = 0, uint32_t texture_slice = 0);
bool EncodeTileTextureExport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLTexture> texture, id<MTLBuffer> tiled,
                             NSUInteger tiled_offset, const TileSurfaceLayout& layout, uint32_t level = 0, uint32_t layer = 0,
                             uint32_t texture_level = 0, uint32_t texture_slice = 0);
bool EncodeTileTextureRectExport(id<MTLDevice> device,id<MTLCommandBuffer> cmd,id<MTLTexture> texture,id<MTLBuffer> tiled,
                                 NSUInteger offset,const TileSurfaceLayout& layout,PixelRect rect,uint32_t level=0,uint32_t layer=0,
                                 uint32_t texture_level=0,uint32_t texture_slice=0);

// Exact compressed blocks, not decompressed/recompressed colours. A uint block
// texture and GPU buffer bridge connect guest tiling to Metal's BC blit layout.
// Pixel mip sizes may be nonmultiples of four or smaller than one block.
bool EncodeTileBcImport(id<MTLDevice> device,id<MTLCommandBuffer> cmd,id<MTLBuffer> tiled,NSUInteger offset,
                        id<MTLTexture> texture,const TileSurfaceLayout& layout,uint32_t level=0,uint32_t layer=0,
                        uint32_t texture_level=0,uint32_t texture_slice=0);
bool EncodeTileBcExport(id<MTLDevice> device,id<MTLCommandBuffer> cmd,id<MTLTexture> texture,id<MTLBuffer> tiled,NSUInteger offset,
                        const TileSurfaceLayout& layout,uint32_t level=0,uint32_t layer=0,uint32_t texture_level=0,uint32_t texture_slice=0);

// Convert a GPU writer to another tiled view of the same guest address. Only
// source texels override guest bytes; padding and reader-only bytes come from
// `guest`. Neither guest nor source is modified. `scratch` must be a distinct
// buffer large enough for both padded layouts. All copies remain on the GPU.
bool EncodeTileAlias(id<MTLDevice> device, id<MTLCommandBuffer> cmd,
                     id<MTLBuffer> guest, NSUInteger guest_offset, id<MTLBuffer> scratch,
                     id<MTLTexture> source, const TileTables& source_tables, uint32_t source_width, uint32_t source_height,
                     id<MTLTexture> target, const TileTables& target_tables, uint32_t target_width, uint32_t target_height);

// One mip of a tiled volume, including thick-block z addressing and shared mip
// tails. `tiled_offset` points to the complete guest allocation, not the mip.
// The uint element texture may be a standalone mip (texture_level=0) or a chain.
bool EncodeTileVolumeImport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLBuffer> tiled, NSUInteger tiled_offset,
                            id<MTLTexture> texture, const TileSurfaceLayout& layout, uint32_t level = 0, uint32_t texture_level = 0);
bool EncodeTileVolumeExport(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLTexture> texture, id<MTLBuffer> tiled,
                            NSUInteger tiled_offset, const TileSurfaceLayout& layout, uint32_t level = 0, uint32_t texture_level = 0);

// The uint pixel format whose texels are exactly `element_bytes` wide, or Invalid.
MTLPixelFormat TileElementFormat(uint32_t element_bytes);

} // namespace Libs::Graphics::Metal
