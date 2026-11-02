#pragma once

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

namespace Libs::Graphics {

enum class FlatSrtAction { Reference, Compare, Candidate };

inline FlatSrtAction ChooseFlatSrtAction(ShaderRecompiler::IR::DescriptorEvaluationMode mode,
                                        bool has_recipes, bool verified, uint64_t checks,
                                        uint64_t sequence, bool rejected) {
	using ShaderRecompiler::IR::DescriptorEvaluationMode;
	if (rejected || mode == DescriptorEvaluationMode::Off) return FlatSrtAction::Reference;
	if (mode == DescriptorEvaluationMode::Shadow) {
		return checks == 0 || (has_recipes && (sequence & 63u) == 0) ?
		    FlatSrtAction::Compare : FlatSrtAction::Reference;
	}
	if (!has_recipes) return FlatSrtAction::Reference;
	if (checks == 0 || (!verified && (sequence & 63u) == 0) ||
	    (verified && (sequence & 511u) == 0)) return FlatSrtAction::Compare;
	return verified ? FlatSrtAction::Candidate : FlatSrtAction::Reference;
}

} // namespace Libs::Graphics
