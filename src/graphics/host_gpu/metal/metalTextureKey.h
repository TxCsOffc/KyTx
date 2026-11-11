#pragma once
#include <compare>
#include <cstdint>

namespace Libs::Graphics::Metal {
// All fields are part of identity. XOR-packing dimensions into a guest address
// makes distinct allocations collide even when both addresses are page-aligned.
struct RenderTextureKey {
	uint64_t address;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t levels;
	auto operator<=>(const RenderTextureKey&) const = default;
};
}
