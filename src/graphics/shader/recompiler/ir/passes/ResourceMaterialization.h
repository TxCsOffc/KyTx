#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Canonical module-affecting resource state. Runtime addresses and descriptor payloads remain in
// ResourceSnapshot and therefore do not create shader permutations.
struct ResourceSpecialization {
	struct Buffer {
		uint32_t               packed_stride                   = 0;
		Prospero::BufferFormat descriptor_format               = Prospero::BufferFormat::kInvalid;
		uint32_t               descriptor_swizzle              = DstSel(4, 5, 6, 7);
		bool                   zero_stride_oob                 = false;
		bool                   operator==(const Buffer&) const = default;
	};

	struct Image {
		Prospero::TextureNumericClass numeric_class = Prospero::TextureNumericClass::Unsupported;
		Decoder::ImageDimension       dimension     = Decoder::ImageDimension::Unknown;
		uint32_t                      mip_count     = 1;
		Prospero::BufferFormat        conversion_format          = Prospero::BufferFormat::kInvalid;
		uint32_t                      shader_swizzle             = ShaderImageIdentitySwizzle;
		uint32_t                      indirect_root              = ImageResource::NoIndirectImage;
		uint32_t                      indirect_mapping_offset    = 0;
		uint32_t                      indirect_search_iterations = 0;
		bool                          cube                       = false;
		bool                          fmask                      = false;
		bool                          operator==(const Image&) const = default;
	};

	std::vector<Buffer> buffers;
	std::vector<Image>  images;

	bool operator==(const ResourceSpecialization&) const = default;
};

// Extracts the descriptor/SRT value graph before resource specialization. The returned plan owns
// its values and is independent of the translated shader CFG.
ResourcePlan ExtractResourcePlan(const Program& program);

// Refreshes cached resources and specialization in place. A failed refresh must not be used.
bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization,
                          DescriptorEvaluationMode mode = DescriptorEvaluationMode::Off,
                          DescriptorEvaluationStats* stats = nullptr);

// Returns reference output. Candidate materialization replays observed reads instead of
// calling guest memory readers again, comparing failures, complete outputs and read order.
bool ShadowMaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                                ResourceSnapshot& snapshot, ResourceSpecialization& specialization,
                                DescriptorEvaluationStats& stats);

// FlatSRT candidate uses the same captured-read replay and complete-output checks.
bool ShadowFlatSrtMaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                                      ResourceSnapshot& snapshot, ResourceSpecialization& specialization,
                                      FlatSrtEvaluationStats& stats);

struct CompiledSrtEvaluationStats {
	uint64_t full_checks = 0, full_mismatches = 0, reference_ns = 0, candidate_ns = 0;
};
// Explicit opt-in entry point. Unsupported plans use the reference evaluator.
bool MaterializeCompiledSrtResources(const ResourcePlan& program, const SrtRuntime& runtime,
                                     ResourceSnapshot& snapshot, ResourceSpecialization& specialization);
bool ShadowCompiledSrtMaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                                            ResourceSnapshot& snapshot, ResourceSpecialization& specialization,
                                            CompiledSrtEvaluationStats& stats);

// Applies an already-derived specialization to native IR before layout and emission.
void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_ */
