// Standalone CPU test: tools/test_surface_footprint.sh
// TiledSurfaceFootprint must list exactly the bytes of the visible elements, as the reference layout of guest_gpu/tile.cpp places them
// (independent of the GF(2) tables the footprint code uses). Padding of edge tiles must not be owned.
#include "graphics/host_gpu/metal/metalSurfaceFootprint.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

using namespace Libs::Graphics;
using namespace Libs::Graphics::Metal;
static void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

int main() {
	try {
		const TileBlockFamily families[] {TileBlockFamily::Standard256B, TileBlockFamily::Standard4KB, TileBlockFamily::Standard64KB,
		                                  TileBlockFamily::Prt64KB, TileBlockFamily::RenderTarget64KB, TileBlockFamily::Depth64KB};
		const uint32_t element_bytes[] {1, 2, 4, 8, 16};
		const bool negative = std::getenv("KYTX_FOOTPRINT_TEST_WHOLE_BLOCKS") != nullptr; // control: pretend every touched block is owned
		uint32_t cases = 0;
		uint64_t owned_total = 0, padding_total = 0, ranges_total = 0;
		for (auto family: families) for (uint32_t eb: element_bytes) {
			TileBlockLayout block {};
			if (!TileGetBlockLayout(family, eb, block)) continue;
			const uint32_t bw = block.block_width, bh = block.block_height;
			const uint32_t sizes[][2] {{1, 1}, {bw, bh}, {bw + 1, bh}, {bw, bh + 1}, {bw * 2 - 1, bh * 3 + 5}, {bw * 3, bh * 2},
			                           {17, 13}, {bw * 5 + 7, 3}, {1920, 1080}, {1600, 904}};
			for (const auto& s: sizes) {
				const uint32_t w = s[0], h = s[1];
				TileTables t;
				Check(BuildTileTables(block, w, h, t), "tables");
				std::vector<FootprintRange> got;
				Check(TiledSurfaceFootprint(t, w, h, got), "footprint");
				if (negative) { // whole touched blocks, not just visible elements
					got.clear();
					for (uint32_t by = 0; by < (h + bh - 1) / bh; ++by) for (uint32_t bx = 0; bx < (w + bw - 1) / bw; ++bx)
						got.push_back({(uint64_t(by) * t.columns + bx) * block.block_size, block.block_size});
				}
				// Oracle: place every visible element with the reference functions of tile.cpp.
				const uint64_t total = uint64_t(t.columns) * t.rows * block.block_size;
				std::vector<uint8_t> owned(total, 0);
				for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
					uint32_t within = 0, bank = 0;
					Check(TileGetBlockOffset(block, x % bw, y % bh, 0, within), "reference offset");
					Check(TileGetBlockXor(block, x / bw, y / bh, 0, bank), "reference bank");
					const uint64_t at = uint64_t(y / bh * t.columns + x / bw) * block.block_size + (within ^ bank);
					Check(at + eb <= total, "reference offset out of surface");
					for (uint32_t k = 0; k < eb; ++k) owned[at + k] = 1;
				}
				std::vector<uint8_t> claimed(total, 0);
				uint64_t prev_end = 0;
				for (size_t i = 0; i < got.size(); ++i) {
					const auto& r = got[i];
					Check(r.size != 0 && r.begin + r.size <= total, "range outside surface");
					Check(i == 0 || r.begin > prev_end, "ranges not sorted or adjacent ranges not merged");
					prev_end = r.begin + r.size;
					for (uint64_t k = 0; k < r.size; ++k) claimed[r.begin + k] = 1;
				}
				for (uint64_t k = 0; k < total; ++k) {
					Check(owned[k] == claimed[k], "footprint differs from the byte oracle (padding owned or texel byte missing)");
					owned_total += owned[k]; padding_total += !owned[k];
				}
				ranges_total += got.size();
				++cases;
			}
		}
		Check(cases > 250 && padding_total > 0 && owned_total > 0, "coverage incomplete");
		std::printf("Surface footprint: %u layout/size cases, %llu owned bytes / %llu padding bytes checked, %llu ranges\n", cases,
		            (unsigned long long)owned_total, (unsigned long long)padding_total, (unsigned long long)ranges_total);
	} catch (const std::exception& ex) { std::fprintf(stderr, "%s\n", ex.what()); return 1; }
}
