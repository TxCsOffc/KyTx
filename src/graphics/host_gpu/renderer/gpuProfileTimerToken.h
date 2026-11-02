#pragma once
#include <cstdint>

namespace Libs::Graphics {
// A larger in-flight pool needs an index wider than the old 14-bit field.
struct GpuProfileTimerToken {
    static constexpr uint32_t IndexBits = 16;
    static constexpr uint32_t Count = 1u << IndexBits;
    static constexpr uint32_t IndexMask = Count - 1;
    static constexpr uint32_t GenerationMask = UINT32_MAX >> IndexBits;
    static constexpr uint32_t NextGeneration(uint32_t generation) {
        return (generation + 1) & GenerationMask;
    }
    static constexpr uint32_t Encode(uint32_t generation, uint32_t index) {
        const auto token = (generation << IndexBits) | index;
        return token == UINT32_MAX ? index : token;
    }
    static constexpr uint32_t Index(uint32_t token) { return token & IndexMask; }
};
} // namespace Libs::Graphics
