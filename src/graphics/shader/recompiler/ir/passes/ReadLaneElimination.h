#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct ReadLaneStats {
	uint32_t rewritten_reads = 0;
	// Constant-lane reads left as cross-lane shuffles.
	uint32_t remaining_reads = 0;
	// Reads rewritten although some paths never wrote the lane (undefined there).
	uint32_t undefined_path_reads = 0;
};

[[nodiscard]] ReadLaneStats EliminateReadLane(Program& program, uint32_t wave_size);

} // namespace Libs::Graphics::ShaderRecompiler::IR
