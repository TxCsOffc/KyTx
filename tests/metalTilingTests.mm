// Standalone GPU regression test: tools/test_metal_tiling.sh
// Tiled guest memory -> texture -> tiled guest memory must reproduce the reference layout of guest_gpu/tile.cpp exactly.
#include "graphics/host_gpu/metal/metalTiling.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bit>
#include <random>

using namespace Libs::Graphics;
using namespace Libs::Graphics::Metal;

static int s_failures = 0;
static void Check(bool ok, const char* what, const char* family, uint32_t eb, uint32_t layer = 0) {
	if (!ok) {
		++s_failures;
		std::fprintf(stderr, "FAIL %s (%s eb=%u layer=%u)\n", what, family, eb, layer);
	}
}

int main() {
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil) { std::fprintf(stderr, "Metal device unavailable\n"); return 1; }
		id<MTLCommandQueue> queue = [device newCommandQueue];
		const TileBlockFamily families[] = {TileBlockFamily::Standard256B, TileBlockFamily::Standard4KB, TileBlockFamily::Standard64KB,
		                                    TileBlockFamily::Prt64KB, TileBlockFamily::RenderTarget64KB, TileBlockFamily::Depth64KB};
		const char* names[] = {"Std256B", "Std4KB", "Std64KB", "Prt64KB", "RT64KB", "Depth64KB"};
		std::mt19937 rng(12345);
		uint32_t cases = 0;
		for (int f = 0; f < 6; ++f) for (uint32_t eb: {1u, 2u, 4u, 8u, 16u}) for (uint32_t layer: {0u, 5u}) {
			TileBlockLayout block {};
			if (!TileGetBlockLayout(families[f], eb, block)) continue;
			// A surface that spans 2+ block columns/rows with a partial last block in both directions.
			const uint32_t width = block.block_width * 2 + std::max(block.block_width / 3, 1u);
			const uint32_t height = block.block_height * 2 + std::max(block.block_height / 3, 1u);
			const uint32_t pitch = ((width + block.block_width - 1) / block.block_width) * block.block_width;
			TileTables tables;
			Check(BuildTileTables(block, pitch, height, tables), "BuildTileTables", names[f], eb, layer);
			if (!tables.valid) continue;
			const size_t tiled_bytes = size_t(tables.columns) * tables.rows * block.block_size;
			id<MTLBuffer> tiled = [device newBufferWithLength:tiled_bytes options:MTLResourceStorageModeShared];
			uint8_t* tb = static_cast<uint8_t*>(tiled.contents);
			for (size_t i = 0; i < tiled_bytes; ++i) tb[i] = uint8_t(rng());
			MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:TileElementFormat(eb) width:width height:height mipmapped:NO];
			td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
			td.storageMode = MTLStorageModeShared;
			id<MTLTexture> tex = [device newTextureWithDescriptor:td];
			id<MTLCommandBuffer> cmd = [queue commandBuffer];
			Check(EncodeTileImport(device, cmd, tiled, 0, tex, tables, width, height, layer), "import encode", names[f], eb, layer);
			[cmd commit]; [cmd waitUntilCompleted];
			std::vector<uint8_t> pixels(size_t(width) * height * eb);
			[tex getBytes:pixels.data() bytesPerRow:width * eb fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
			// Reference: the per-element addressing used by the CPU detiler.
			size_t mismatches = 0;
			for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
				uint32_t within = 0, bank = 0;
				const uint32_t bx = x / block.block_width, by = y / block.block_height;
				if (!TileGetBlockOffset(block, x % block.block_width, y % block.block_height, 0, within) || !TileGetBlockXor(block, bx, by, layer, bank)) { ++mismatches; continue; }
				const size_t off = size_t(by * tables.columns + bx) * block.block_size + (within ^ bank);
				if (std::memcmp(pixels.data() + (size_t(y) * width + x) * eb, tb + off, eb) != 0) ++mismatches;
			}
			Check(mismatches == 0, "import matches reference layout", names[f], eb, layer);
			// Export back into a zeroed buffer: every addressed element must round-trip.
			id<MTLBuffer> out = [device newBufferWithLength:tiled_bytes options:MTLResourceStorageModeShared];
			std::memset(out.contents, 0, tiled_bytes);
			cmd = [queue commandBuffer];
			Check(EncodeTileExport(device, cmd, tex, out, 0, tables, width, height, layer), "export encode", names[f], eb, layer);
			[cmd commit]; [cmd waitUntilCompleted];
			size_t round_bad = 0;
			const uint8_t* ob = static_cast<const uint8_t*>(out.contents);
			for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
				uint32_t within = 0, bank = 0;
				const uint32_t bx = x / block.block_width, by = y / block.block_height;
				TileGetBlockOffset(block, x % block.block_width, y % block.block_height, 0, within);
				TileGetBlockXor(block, bx, by, layer, bank);
				const size_t off = size_t(by * tables.columns + bx) * block.block_size + (within ^ bank);
				if (std::memcmp(ob + off, tb + off, eb) != 0) ++round_bad;
			}
			Check(round_bad == 0, "export round-trips", names[f], eb, layer);
			++cases;
		}
		std::printf("tiling: %u cases, %d failures\n", cases, s_failures);
		// The GPU lays out a full reference tile independently of the CPU mip
		// decoder. Sampling a small multi-mip image starts inside this tile's
		// shared tail, including level 0 (the FC27 button-icon regression).
		uint32_t mip_cases = 0;
		for (const auto format : {Prospero::BufferFormat::k8UNorm, Prospero::BufferFormat::k8_8_8_8UNorm,
		                          Prospero::BufferFormat::k32_32_32_32UInt, Prospero::BufferFormat::kBc1UNorm}) {
			for (uint32_t width : {32u, 40u}) {
				TileSurfaceLayout layout {};
				const TileSurfaceDescription desc {format, Prospero::TileMode::kStandard64KB,
				    TileSurfaceDimension::Dim2D, width, 32, 1, static_cast<uint32_t>(std::bit_width(width)), 1};
				if (!TileGetTiledTextureLayout(desc, layout)) continue;
				const auto& block = layout.texture.block;
				Check(layout.first_tail_level < desc.levels, "chain includes mip tail", "mip", block.bytes_per_element);
				const uint32_t bw = block.block_width, bh = block.block_height, eb = block.bytes_per_element;
				std::vector<uint8_t> reference(size_t(bw) * bh * eb);
				for (auto& b : reference) b = uint8_t(rng());
				MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:TileElementFormat(eb)
				    width:bw height:bh mipmapped:NO];
				td.storageMode = MTLStorageModeShared;
				td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
				id<MTLTexture> tile_tex = [device newTextureWithDescriptor:td];
				[tile_tex replaceRegion:MTLRegionMake2D(0,0,bw,bh) mipmapLevel:0
				    withBytes:reference.data() bytesPerRow:bw * eb];
				id<MTLBuffer> tiled = [device newBufferWithLength:layout.total_size options:MTLResourceStorageModeShared];
				TileTables tables;
				Check(BuildTileTables(block,bw,bh,tables), "mip tables", "mip", eb);
				id<MTLCommandBuffer> cb = [queue commandBuffer];
				for (size_t offset = 0; offset < layout.total_size; offset += block.block_size)
					Check(EncodeTileExport(device,cb,tile_tex,tiled,offset,tables,bw,bh), "mip reference export", "mip", eb);
				[cb commit]; [cb waitUntilCompleted];
				Check(cb.status == MTLCommandBufferStatusCompleted, "mip GPU reference completed", "mip", eb);
				const std::span<const uint8_t> source(static_cast<const uint8_t*>(tiled.contents), tiled.length);
				for (uint32_t level = 0; level < desc.levels; ++level) {
					std::vector<uint8_t> decoded;
					uint32_t pitch = 0;
					Check(DecodeTiledTextureLevel(source,layout,level,0,decoded,pitch), "mip decode", "mip", eb, level);
					const auto& mip = layout.mips[level];
					const bool tail = level >= layout.first_tail_level;
					bool equal = decoded.size() == size_t(mip.width) * mip.height * eb && pitch == mip.width * eb;
					for (uint32_t y = 0; equal && y < mip.height; ++y)
						equal = std::memcmp(decoded.data() + y*pitch,
						    reference.data() + (size_t(y+(tail?mip.tail_y:0))*bw+(tail?mip.tail_x:0))*eb, pitch) == 0;
					Check(equal, "mip pixels match independent GPU export", "mip", eb, level);
				}
				std::vector<uint8_t> decoded; uint32_t pitch = 0;
				Check(!DecodeTiledTextureLevel(source.first(1),layout,0,0,decoded,pitch) && decoded.empty(),
				      "truncated source rejected", "mip", eb);
				Check(!DecodeTiledTextureLevel(source,layout,desc.levels,0,decoded,pitch), "invalid mip rejected", "mip", eb);
				Check(!DecodeTiledTextureLevel(source,layout,0,1,decoded,pitch), "invalid layer rejected", "mip", eb);
				++mip_cases;
			}
		}
		std::printf("guest mip decode: %u chains, %d cumulative failures\n", mip_cases, s_failures);
	}
	return s_failures == 0 ? 0 : 1;
}
