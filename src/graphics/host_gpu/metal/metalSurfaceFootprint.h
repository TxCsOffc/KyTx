#pragma once

// Which bytes of guest memory a tiled 2D surface really owns.
//
// A GPU writer of a tiled surface owns the bytes of its visible texels, not the padding of the macro tiles around them and not the
// neighbouring allocation. MemoryCoherence needs that footprint to decide which bytes a private texture can supply and which ones must
// come from guest memory (see memoryCoherence.h). The offsets use the same GF(2) tables as the tiling kernels, so footprint and
// import/export always agree on where a texel lives.

#include "graphics/host_gpu/metal/metalTiling.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Libs::Graphics::Metal {

struct FootprintRange {
	uint64_t begin = 0; // byte offset from the first byte of the surface
	uint64_t size  = 0;
	bool operator==(const FootprintRange&) const = default;
};

// Sorted, merged byte ranges holding the `width` x `height` visible elements (a block-compressed format counts one element per block)
// of a 2D level-0 surface. A tile block that lies completely inside the visible area is one range; a block cut by the right or bottom
// edge is walked element by element. The cost is O(blocks + edge elements), not O(texels).
inline bool TiledSurfaceFootprint(const TileTables& t, uint32_t width, uint32_t height, std::vector<FootprintRange>& out) {
	out.clear();
	if (!t.valid || width == 0 || height == 0) return false;
	const auto& b = t.block;
	if (b.block_width == 0 || b.block_height == 0 || b.bytes_per_element == 0 || b.block_size == 0) return false;
	const uint32_t cols = (width + b.block_width - 1) / b.block_width;
	const uint32_t rows = (height + b.block_height - 1) / b.block_height;
	if (cols > t.columns || rows > t.rows) return false;
	auto add = [&](uint64_t begin, uint64_t size) {
		if (!out.empty() && out.back().begin + out.back().size == begin) out.back().size += size;
		else out.push_back({begin, size});
	};
	std::vector<FootprintRange> edge; // one block's elements, sorted before they are merged in
	for (uint32_t by = 0; by < rows; ++by) {
		for (uint32_t bx = 0; bx < cols; ++bx) {
			const uint64_t block = (static_cast<uint64_t>(by) * t.columns + bx) * b.block_size;
			const uint32_t x0 = bx * b.block_width, y0 = by * b.block_height;
			if (x0 + b.block_width <= width && y0 + b.block_height <= height) {
				add(block, b.block_size);
				continue;
			}
			const uint32_t bank = t.bank_x[bx] ^ t.bank_y[by] ^ t.bank_v0;
			edge.clear();
			for (uint32_t y = y0; y < std::min(height, y0 + b.block_height); ++y) {
				for (uint32_t x = x0; x < std::min(width, x0 + b.block_width); ++x) {
					const uint32_t within = t.in_x[x - x0] ^ t.in_y[y - y0];
					edge.push_back({block + (within ^ bank), b.bytes_per_element});
				}
			}
			std::sort(edge.begin(), edge.end(), [](const FootprintRange& l, const FootprintRange& r) { return l.begin < r.begin; });
			for (const auto& e: edge) {
				// Blocks are visited in address order, so an element can only touch the previous range of this block.
				if (!out.empty() && out.back().begin + out.back().size >= e.begin && out.back().begin >= block) {
					out.back().size = std::max(out.back().begin + out.back().size, e.begin + e.size) - out.back().begin;
				} else add(e.begin, e.size);
			}
		}
	}
	return !out.empty();
}

} // namespace Libs::Graphics::Metal
