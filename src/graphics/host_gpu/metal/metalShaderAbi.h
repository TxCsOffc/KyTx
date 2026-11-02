#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALSHADERABI_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALSHADERABI_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace Libs::Graphics::Metal {

// KYTY_SPVC_ABI: argument-buffer id layout for SPIRV-Cross MSL.
//
// The legacy layout placed each descriptor kind at NativeBinding(stage, kind) and let array
// elements take consecutive ids, so a multi-element array (e.g. buffers[4] at ids 1..4) collided
// with the next kinds. This allocator gives every descriptor present in the shader its own
// contiguous id range: kind k -> base(k), element j -> base(k) + j. Id 0 is reserved for
// spvBufferSizeConstants, which SPIRV-Cross always places there.
//
// Both the compile side (add_msl_resource_binding) and the bind side (argument encoder) call this
// function on the same program bindings, so they agree without storing a table.
struct SpvcAbiLayout {
	static constexpr uint32_t kNotPresent = UINT32_MAX;
	static constexpr uint32_t kFirstId    = 1u;

	// Base id per DescriptorBindingKind (kNotPresent when the shader does not declare it).
	std::array<uint32_t, static_cast<size_t>(ShaderRecompiler::IR::DescriptorBindingKind::Count)> base {};
	// SPIR-V binding number (NativeBinding) and element count per declared descriptor.
	struct Entry {
		ShaderRecompiler::IR::DescriptorBindingKind kind;
		uint32_t                                    binding;
		uint32_t                                    count;
		uint32_t                                    base_id;
	};
	std::vector<Entry> entries;

	[[nodiscard]] uint32_t BaseOf(ShaderRecompiler::IR::DescriptorBindingKind kind) const {
		return base[static_cast<size_t>(kind)];
	}
};

[[nodiscard]] inline bool AbiKindIsArray(ShaderRecompiler::IR::DescriptorBindingKind kind) {
	using K = ShaderRecompiler::IR::DescriptorBindingKind;
	return kind == K::Buffers || kind == K::Samplers ||
	       static_cast<uint32_t>(kind) >= ShaderRecompiler::IR::FirstImageBinding &&
	           static_cast<uint32_t>(kind) < static_cast<uint32_t>(K::Samplers);
}

[[nodiscard]] inline SpvcAbiLayout ComputeSpvcAbiLayout(const ShaderRecompiler::IR::BindingLayout& bindings,
                                                        Libs::Graphics::ShaderType stage) {
	using namespace ShaderRecompiler::IR;
	SpvcAbiLayout layout;
	layout.base.fill(SpvcAbiLayout::kNotPresent);

	std::vector<SpvcAbiLayout::Entry> entries;
	for (const auto& desc: bindings.descriptors) {
		SpvcAbiLayout::Entry e {};
		e.kind    = desc.kind;
		e.binding = NativeBinding(stage, desc.kind);
		// Non-array descriptors are declared as single variables; arrays use their element count.
		e.count   = AbiKindIsArray(desc.kind) ? std::max<uint32_t>(1u, static_cast<uint32_t>(desc.resources.size())) : 1u;
		entries.push_back(e);
	}
	// Allocate in SPIR-V binding order so the layout is deterministic across compile and bind.
	std::sort(entries.begin(), entries.end(),
	          [](const SpvcAbiLayout::Entry& a, const SpvcAbiLayout::Entry& b) { return a.binding < b.binding; });

	uint32_t next = SpvcAbiLayout::kFirstId;
	for (auto& e: entries) {
		e.base_id = next;
		next += e.count;
		layout.base[static_cast<size_t>(e.kind)] = e.base_id;
	}
	layout.entries = std::move(entries);
	return layout;
}

} // namespace Libs::Graphics::Metal

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALSHADERABI_H_
