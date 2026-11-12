#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MSLEMITTER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MSLEMITTER_H_

#include "common/common.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Msl {

// Native argument-buffer and buffer-25 additions for software comparison filtering.
// The point sampler shares S# addressing; comparisons happen before interpolation.
inline constexpr uint32_t ComparisonPointSamplerBase = 192;
inline constexpr uint32_t SamplerDescriptorWordBase = 96;
inline constexpr uint32_t ImageMetadataWordBase = SamplerDescriptorWordBase + 32 * 4;
// Buffer-25 word with the bound GDS length in dwords (the SPIR-V path checks GDS accesses against the bound buffer's length).
inline constexpr uint32_t GdsDwordsWord = ImageMetadataWordBase + 64;
inline constexpr uint32_t SizeConstantWordCount = GdsDwordsWord + 1;

// Direct RDNA (IR) -> Metal Shading Language translation. Bypasses SPIR-V and
// SPIRV-Cross: the IR already carries typed memory, image and export semantics.
struct EmitResult {
	bool        success = false;
	std::string source;
	// Human-readable list of opcodes/features this emitter does not handle yet.
	std::vector<std::string> unsupported;
	// Compute and mesh execution run each wave64 as 32 host lanes that each carry two guest lanes (see Wave64Pair): the dispatch must launch
	// half as many host threads per group, in one dimension.
	bool wave64_pair = false;
};

[[nodiscard]] EmitResult EmitProgram(const IR::Program& program,
                                     ShaderStageInputInfo input_info);

// A wave64 compute or native NGG program on a 32-lane host, with whole waves in each workgroup, is lowered "pair-packed" (the SPIR-V backend's
// lane_count == 2): host thread t of simdgroup s stands for guest lanes 64*s + t and 64*s + 32 + t of a wave, so wave-wide operations
// (ballot, readlane 63, the DPP scans) see all 64 lanes. The renderer uses the same contract to size its host workgroup.
[[nodiscard]] bool Wave64Pair(ShaderType stage, uint32_t wave_size, ShaderStageInputInfo input_info, uint64_t shader_hash = 0);

// What EmitProgram decided for a shader hash (and the guest workgroup size it assumed): the kernel Metal runs is the one compiled, whatever a
// later dispatch of the same hash looks like. False when the hash was never emitted.
[[nodiscard]] bool Wave64PairCompiled(uint64_t shader_hash, bool* pair, uint32_t* threads);

} // namespace Libs::Graphics::ShaderRecompiler::Msl

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MSLEMITTER_H_
