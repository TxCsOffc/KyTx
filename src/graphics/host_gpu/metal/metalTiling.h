#pragma once

// GPU conversion between a Metal texture and guest memory in the PS5's tiled layout.
//
// Guest memory is the authority for a surface: the CPU, DMA engines and other passes that alias the same address all see the
// tiled bytes. Metal needs linear textures for sampling and rendering, so surfaces move between the two through these kernels.
// Every guest tile layout (Standard 256B/4KB/64KB, PRT, render target, depth) is GF(2)-linear in (x, y), so one generic
// kernel driven by small lookup tables handles all of them; BuildTileTables self-checks that against the reference
// implementation in guest_gpu/tile.cpp.

#include "graphics/guest_gpu/tile.h"

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
	std::vector<uint32_t> bank_x;      // bank xor contribution of the block column (relative to block 0)
	std::vector<uint32_t> bank_y;      // ... of the block row
	bool                  valid = false;
};

// `width`/`height` are in addressable elements (a BC block is one element). `pitch` is the padded row width in elements.
bool BuildTileTables(const TileBlockLayout& block, uint32_t pitch, uint32_t height, TileTables& out);

// Decode one 2D array slice/mip from the complete guest allocation. Small mip
// chains share a tile: level 0 can start at tail_x/tail_y rather than (0,0).
// Output rows contain addressable elements (BC blocks for compressed formats).
// Level 0 of a volume (tile.h Dim3D layout) as tightly packed linear voxels (width * height * depth elements).
bool DecodeTiledVolume(std::span<const uint8_t> source, const TileSurfaceLayout& layout, std::vector<uint8_t>& linear);

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
                      id<MTLTexture> texture, const TileTables& tables, uint32_t width, uint32_t height, uint32_t layer = 0);

// The uint pixel format whose texels are exactly `element_bytes` wide, or Invalid.
MTLPixelFormat TileElementFormat(uint32_t element_bytes);

} // namespace Libs::Graphics::Metal
