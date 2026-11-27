#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {

struct FastIrOptStats {
	uint32_t simplified          = 0;
	uint32_t cse_eliminated      = 0;
	uint32_t bitcasts_eliminated = 0;
	uint32_t selects_eliminated  = 0;
};

// Linear-time algebraic, bitcast, masked-select, and per-block pure CSE optimizer.
// Controlled by KYTY_FAST_IR_OPT=0 (enabled by default).
FastIrOptStats OptimizeFast(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
