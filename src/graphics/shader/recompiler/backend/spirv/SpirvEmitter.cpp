#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

namespace {

struct StorageAccessBounds {
	uint32_t              constant_ops = 0;
	uint32_t              dynamic_ops  = 0;
	uint64_t              max_end      = 0;
	std::set<std::string> dynamic_producers;

	void Note(IR::Value address, uint32_t offset, uint32_t bytes) {
		address = address.Resolve();
		if (address.IsImmediate()) {
			constant_ops++;
			max_end = std::max(max_end, uint64_t {address.U32()} + offset + bytes);
			return;
		}
		dynamic_ops++;
		const auto* producer = address.TryInstruction();
		dynamic_producers.emplace(producer != nullptr
		                              ? std::string(IR::ValueOpcodeName(producer->GetOpcode()))
		                              : std::string("?"));
	}

	[[nodiscard]] std::string Format(const char* name) const {
		std::string producers;
		for (const auto& producer: dynamic_producers) {
			producers += producers.empty() ? "" : "|";
			producers += producer;
		}
		char text[160];
		std::snprintf(text, sizeof(text), " %s_const=%u %s_dyn=%u %s_max_const_end=%" PRIu64, name,
		              constant_ops, name, dynamic_ops, name, max_end);
		return std::string(text) + " " + name + "_dyn_from=" + (producers.empty() ? "-" : producers);
	}
};

std::string FormatExpression(IR::Value value, int depth) {
	value = value.Resolve();
	if (value.IsImmediate()) {
		if (value.GetType() != IR::Type::U32) {
			return "imm";
		}
		char text[16];
		std::snprintf(text, sizeof(text), "0x%x", value.U32());
		return text;
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		return "?";
	}
	auto text = std::string(IR::ValueOpcodeName(inst->GetOpcode()));
	if (inst->NumArgs() == 0) {
		return text;
	}
	if (depth == 0) {
		return text + "(..)";
	}
	text += "(";
	for (size_t index = 0; index < inst->NumArgs(); index++) {
		text += index == 0 ? "" : ",";
		text += FormatExpression(inst->Arg(index), depth - 1);
	}
	return text + ")";
}

// Largest byte address (exclusive of the access) or nullopt when not statically bounded.
// LaneId is SubgroupLocalInvocationId (+32 for the high half), and subgroup masks cap it at 128.
std::optional<uint64_t> BoundedLdsAddress(IR::Value address, uint32_t lane_limit) {
	address = address.Resolve();
	if (address.IsImmediate()) {
		return address.GetType() == IR::Type::U32 ? std::optional<uint64_t>(address.U32())
		                                          : std::nullopt;
	}
	const auto* inst = address.TryInstruction();
	if (inst == nullptr) {
		return std::nullopt;
	}
	if (inst->GetOpcode() == IR::ValueOpcode::LaneId) {
		return lane_limit - 1u;
	}
	if (inst->GetOpcode() == IR::ValueOpcode::ShiftLeftLogical32 && inst->NumArgs() == 2) {
		const auto shift = inst->Arg(1).Resolve();
		const auto base  = inst->Arg(0).Resolve();
		const auto* lane = base.TryInstruction();
		if (shift.IsImmediate() && shift.GetType() == IR::Type::U32 && shift.U32() < 8u &&
		    lane != nullptr && lane->GetOpcode() == IR::ValueOpcode::LaneId) {
			return uint64_t {lane_limit - 1u} << shift.U32();
		}
	}
	return std::nullopt;
}

// Size per-thread LDS of workgroup-less stages to the proven reach (KYTY_FUNCTION_LDS_BOUND=0
// restores the fixed 8192-dword array).
uint32_t ProvenFunctionLdsDwords(const Emitter::EmitterState& state) {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_FUNCTION_LDS_BOUND");
		return value == nullptr || value[0] != '0';
	}();
	if (!enabled || !state.requirements.function_lds ||
	    ShaderWorkgroupInput(state.program.stage, state.input_info) != nullptr) {
		return 0;
	}
	const uint32_t lane_limit = 128u + (state.lane_count == 2 ? 32u : 0u);
	uint64_t       max_end    = 0;
	for (const auto* block: state.program.blocks) {
		for (const auto& inst: *block) {
			const auto opcode = inst.GetOpcode();
			const bool address_op =
			    IR::AddressOpcodeInfoOf(opcode).access != IR::AddressAccess::None;
			const bool shared_op = IR::SharedAccessOf(opcode) != IR::SharedAccess::None;
			if (!address_op && !shared_op) {
				continue;
			}
			const auto& mem = state.program.memory_info[inst.Flags<IR::MemoryFlags>().index];
			if (address_op) {
				if (mem.kind == IR::ResourceKind::FlatLocal) {
					return 0;
				}
				continue;
			}
			if (mem.kind != IR::ResourceKind::Lds) {
				continue;
			}
			if (mem.secondary_offset != 0 || inst.NumArgs() == 0) {
				return 0;
			}
			const auto address = BoundedLdsAddress(inst.Arg(0), lane_limit);
			if (!address.has_value()) {
				return 0;
			}
			const auto bytes =
			    std::max({mem.data_dwords, IR::SharedComponentCount(opcode), 4u}) * 4u;
			max_end = std::max(max_end, *address + mem.offset + bytes);
		}
	}
	const auto dwords = (max_end + 3u) / 4u;
	return dwords == 0 || dwords >= 8192u ? 0u : static_cast<uint32_t>(dwords);
}

// KYTY_SHADER_STORAGE_LOG=<path>: per-thread Function-storage sizes and static access bounds.
void LogFunctionStorage(const Emitter::EmitterState& state) {
	using Emitter::LdsDwordCount;
	static const char* path = std::getenv("KYTY_SHADER_STORAGE_LOG");
	if (path == nullptr || path[0] == '\0' ||
	    (!state.requirements.function_scratch && !state.requirements.function_lds)) {
		return;
	}
	const auto&         program = state.program;
	StorageAccessBounds scratch, flat_local, lds;
	std::string         lds_ops;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto opcode = inst.GetOpcode();
			if (IR::AddressOpcodeInfoOf(opcode).access != IR::AddressAccess::None) {
				const auto& mem = program.memory_info[inst.Flags<IR::MemoryFlags>().index];
				if (mem.kind == IR::ResourceKind::Scratch || mem.kind == IR::ResourceKind::FlatLocal) {
					auto& bounds = mem.kind == IR::ResourceKind::Scratch ? scratch : flat_local;
					bounds.Note(inst.Arg(1), mem.offset, mem.data_dwords * 4u);
				}
			} else if (IR::SharedAccessOf(opcode) != IR::SharedAccess::None) {
				const auto& mem = program.memory_info[inst.Flags<IR::MemoryFlags>().index];
				if (mem.kind == IR::ResourceKind::Lds) {
					const auto bytes = std::max(mem.data_dwords, IR::SharedComponentCount(opcode)) * 4u;
					lds.Note(inst.Arg(0), mem.offset, bytes);
					char head[128];
					std::snprintf(head, sizeof(head), "  lds_op %s off=%u sec=%u bytes=%u addr=",
					              std::string(IR::ValueOpcodeName(opcode)).c_str(), mem.offset,
					              mem.secondary_offset, bytes);
					lds_ops += head + FormatExpression(inst.Arg(0), 6).substr(0, 1500) + "\n";
				}
			}
		}
	}
	const uint32_t scratch_bytes =
	    state.requirements.function_scratch ? program.scratch_dwords * 4u * state.lane_count : 0u;
	const uint32_t lds_bytes = state.requirements.function_lds ? LdsDwordCount(state) * 4u : 0u;
	const auto line = [&]() {
		char head[256];
		std::snprintf(head, sizeof(head),
		              "hash=%016" PRIx64 " stage=%u wave=%u lanes=%u scratch_dwords=%u"
		              " lds_dwords=%u fn_scratch=%d fn_lds=%d per_thread_bytes=%u",
		              program.shader_hash, static_cast<unsigned>(program.stage), program.wave_size,
		              state.lane_count, program.scratch_dwords, LdsDwordCount(state),
		              state.requirements.function_scratch ? 1 : 0,
		              state.requirements.function_lds ? 1 : 0, scratch_bytes + lds_bytes);
		return std::string(head) + scratch.Format("scratch") + flat_local.Format("flat") +
		       lds.Format("lds") + "\n" + lds_ops;
	}();
	static std::mutex           lock;
	const std::lock_guard guard(lock);
	if (auto* file = std::fopen(path, "a"); file != nullptr) {
		std::fputs(line.c_str(), file);
		std::fclose(file);
	}
}

[[noreturn]] void Fail(const IR::Program& program, const char* reason) {
	EXIT("SPIR-V validation failed: hash=0x%016" PRIx64 " stage=%u reason=%s\n",
	     program.shader_hash, static_cast<unsigned>(program.stage), reason);
	std::abort();
}

void ValidateNativeProgram(const IR::Program& program) {
	using Kind                                             = IR::DescriptorBindingKind;
	constexpr auto                               KindCount = static_cast<size_t>(Kind::Count);
	std::array<std::vector<uint32_t>, KindCount> expected;
	std::array<bool, KindCount>                  present {};
	const auto                                   Dense = [](size_t size) {
		std::vector<uint32_t> values(size);
		for (uint32_t i = 0; i < values.size(); i++) {
			values[i] = i;
		}
		return values;
	};
	auto Expect = [&](Kind kind, std::vector<uint32_t> resources = {}) {
		const auto index = static_cast<size_t>(kind);
		present[index]   = true;
		expected[index]  = std::move(resources);
	};
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto kind = IR::DescriptorBindingForImage(program.info.images[i]);
		if (!kind.has_value()) {
			Fail(program, "native shader plan has an invalid image class");
		}
		present[static_cast<size_t>(*kind)] = true;
		const auto dynamic = program.info.images[i].mip_mode == IR::ImageMipMode::Dynamic;
		const auto count   = dynamic ? program.info.images[i].mip_count : 1u;
		if (count == 0u || (!dynamic && program.info.images[i].mip_count != 1u)) {
			Fail(program, "native shader plan has an invalid image mip descriptor count");
		}
		expected[static_cast<size_t>(*kind)].insert(expected[static_cast<size_t>(*kind)].end(),
		                                            count, i);
	}
	if (!program.info.samplers.empty()) {
		Expect(Kind::Samplers, Dense(program.info.samplers.size()));
	}
	auto& buffers = expected[static_cast<size_t>(Kind::Buffers)];
	const bool uses_gds = IR::CollectMemoryResources(program, buffers);
	present[static_cast<size_t>(Kind::Buffers)] = !buffers.empty();
	if (uses_gds) {
		Expect(Kind::Gds);
	}
	if (program.info.uses_dma) {
		Expect(Kind::BdaPagetable);
		Expect(Kind::FaultBuffer);
	}
	if (IR::UsesFlattenedSrt(program)) {
		Expect(Kind::FlattenedSrt);
	}
	if (program.bindings.ShaderDataDwords() != 0 && !program.bindings.UsesPushData()) {
		Expect(Kind::ShaderData);
	}

	std::array<bool, KindCount> seen {};
	for (const auto& binding: program.bindings.descriptors) {
		const auto kind = static_cast<size_t>(binding.kind);
		if (kind >= KindCount || seen[kind] || !present[kind] ||
		    binding.resources != expected[kind]) {
			Fail(program, "native descriptor groups do not match shader topology");
		}
		seen[kind] = true;
	}
	for (size_t i = 0; i < KindCount; i++) {
		if (present[i] != seen[i]) {
			Fail(program, "native shader plan is missing a required descriptor group");
		}
	}
	const auto has_shader_data_storage = present[static_cast<size_t>(Kind::ShaderData)];
	const auto shader_data_dwords = program.bindings.ShaderDataDwords();
	if ((program.bindings.UsesPushData() &&
	     !IR::PushData::CanFit(program.bindings.push_data_start_dword, shader_data_dwords)) ||
	    program.bindings.memory_offset_dword != program.bindings.user_data_registers.size() ||
	    program.bindings.memory_offset_count != buffers.size() ||
	    has_shader_data_storage != (shader_data_dwords != 0 && !program.bindings.UsesPushData()) ||
	    !std::is_sorted(program.bindings.user_data_registers.begin(),
	                    program.bindings.user_data_registers.end()) ||
	    std::adjacent_find(program.bindings.user_data_registers.begin(),
	                       program.bindings.user_data_registers.end()) !=
	        program.bindings.user_data_registers.end()) {
		Fail(program, "native shader-data layout is inconsistent");
	}

	const auto planning_only_handle = [&](const IR::Inst& handle) {
		return !handle.Uses().empty() &&
		       std::ranges::all_of(handle.Uses(), [&](const IR::Use& use) {
			       const auto op = use.user->GetOpcode();
			       if (op != IR::ValueOpcode::LoadAddressU32 &&
			           op != IR::ValueOpcode::ReadConstBuffer) {
				       return false;
			       }
			       const auto index = use.user->Flags<IR::MemoryFlags>().index;
			       return index < program.memory_info.size() &&
			              program.memory_info[index].planning_only;
		       });
	};
	const auto indirect_buffer_handle = [&](const IR::Inst& handle) {
		return program.info.uses_dma && handle.NumArgs() == 4u && !handle.Uses().empty() &&
		       std::ranges::all_of(handle.Uses(), [&](const IR::Use& use) {
			       if (IR::BufferAccessOf(use.user->GetOpcode()) != IR::BufferAccess::Read) {
				       return false;
			       }
			       const auto index = use.user->Flags<IR::MemoryFlags>().index;
			       return index < program.memory_info.size() &&
			              program.memory_info[index].kind == IR::ResourceKind::IndirectBuffer;
		       });
	};
	const auto local_flat_handle = [&](const IR::Inst& handle) {
		return !handle.Uses().empty() &&
		       std::ranges::all_of(handle.Uses(), [&](const IR::Use& use) {
			       if (IR::AddressOpcodeInfoOf(use.user->GetOpcode()).access == IR::AddressAccess::None)
				       return false;
			       const auto index = use.user->Flags<IR::MemoryFlags>().index;
			       return index < program.memory_info.size() &&
			              program.memory_info[index].kind == IR::ResourceKind::FlatLocal;
		       });
	};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto dense = inst.Flags<uint32_t>();
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::GetBufferResource:
					if (planning_only_handle(inst) || indirect_buffer_handle(inst)) {
						break;
					}
					if (dense >= program.info.buffers.size()) {
						Fail(program, "typed buffer handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::GetAddressResource:
					if (planning_only_handle(inst)) {
						break;
					}
					if (inst.NumArgs() != 2 || (!program.info.uses_dma && !local_flat_handle(inst))) {
						Fail(program, "typed address handle has invalid DMA metadata");
					}
					break;
				case IR::ValueOpcode::GetScratchResource:
					if (inst.NumArgs() != 0 || program.scratch_dwords == 0) {
						Fail(program, "typed scratch handle has invalid shader metadata");
					}
					break;
				case IR::ValueOpcode::GetImageResource:
					if (dense >= program.info.images.size()) {
						Fail(program, "typed image handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::GetSamplerResource:
					if (dense >= program.info.samplers.size()) {
						Fail(program, "typed sampler handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::ReadConst: {
					const auto slot = inst.Arg(1).Resolve();
					if (!slot.IsImmediate() || slot.GetType() != IR::Type::U32 ||
					    slot.U32() >= program.srt_reads.size()) {
						Fail(program, "flattened SRT read has an invalid dense slot");
					}
					break;
				}
				default: break;
			}
		}
	}
}

} // namespace

Emitter::SpirvRequirements Emitter::AnalyzeProgramRequirements(const IR::Program& program) {
	SpirvRequirements requirements {};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			requirements.float64 |= inst.GetType() == IR::Type::F64;
			if (IR::BufferAccessOf(inst.GetOpcode()) == IR::BufferAccess::Atomic &&
			    inst.GetType() == IR::Type::U64) {
				requirements.buffer_int64_atomics = true;
			}
			const auto address_access = IR::AddressOpcodeInfoOf(inst.GetOpcode()).access;
			if (address_access != IR::AddressAccess::None) {
				const auto memory_index = inst.Flags<IR::MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					Fail(program, "address operation has invalid memory metadata");
				}
				const auto kind = program.memory_info[memory_index].kind;
				if (kind == IR::ResourceKind::Scratch || kind == IR::ResourceKind::FlatLocal) {
					if (program.scratch_dwords == 0) {
						Fail(program, "scratch operation has no per-thread storage");
					}
					requirements.function_scratch = true;
					if (kind == IR::ResourceKind::FlatLocal && program.stage != ShaderType::Compute &&
					    program.stage != ShaderType::Mesh) {
						requirements.function_lds = true;
					}
				} else if (address_access == IR::AddressAccess::Write) {
					Fail(program, "writable FLAT/GLOBAL addresses require GPU ownership tracking");
				}
			}
			if (IR::BufferAccessOf(inst.GetOpcode()) != IR::BufferAccess::None) {
				const auto memory_index = inst.Flags<IR::MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					Fail(program, "buffer operation has invalid memory metadata");
				}
				const auto& memory = program.memory_info[memory_index];
				if (memory.kind == IR::ResourceKind::IndirectBuffer) {
					requirements.subgroup_local_invocation_id = true;
				}
				if (memory.kind == IR::ResourceKind::Buffer) {
					requirements.coherent_buffers |= memory.coherent;
					if (memory.resource >= program.info.buffers.size()) {
						Fail(program, "buffer operation has invalid resource metadata");
					}
					if ((program.info.buffers[memory.resource].packed_stride & (1u << 20u)) != 0u) {
						if (program.stage != ShaderType::Compute) {
							Fail(program, "buffer ADD_TID is only valid for compute shaders");
						}
						requirements.subgroup_local_invocation_id = true;
					}
				}
			}
			const auto shared_access = IR::SharedAccessOf(inst.GetOpcode());
			if (shared_access != IR::SharedAccess::None) {
				const auto index = inst.Flags<IR::MemoryFlags>().index;
				if (index >= program.memory_info.size()) {
					Fail(program, "shared operation has invalid memory metadata");
				}
				const auto kind = program.memory_info[index].kind;
				if (kind != IR::ResourceKind::Lds && kind != IR::ResourceKind::Gds) {
					Fail(program, "shared operation has invalid resource kind");
				}
				if (shared_access == IR::SharedAccess::Atomic &&
				    IR::SharedComponentCount(inst.GetOpcode()) == 2u) {
					if (kind != IR::ResourceKind::Lds || program.stage != ShaderType::Compute) {
						Fail(program, "64-bit shared atomics require compute LDS");
					}
					requirements.shared_int64_atomics = true;
				}
				if (program.stage != ShaderType::Compute && program.stage != ShaderType::Mesh &&
				    kind == IR::ResourceKind::Lds) {
					requirements.function_lds = true;
				}
				if (shared_access == IR::SharedAccess::Append ||
				    shared_access == IR::SharedAccess::Consume) {
					requirements.subgroup_ballot              = true;
					requirements.subgroup_shuffle             = true;
					requirements.subgroup_local_invocation_id = true;
				}
			}
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::BvhIntersect: requirements.bvh = true; break;
				case IR::ValueOpcode::Ballot: requirements.subgroup_ballot = true; break;
				case IR::ValueOpcode::ConditionRef: {
					// Single-wave EXECZ/VCCZ branches are decided with a subgroup ballot.
					const auto kind = inst.Flags<CFG::BranchCondition>();
					static const bool uniform_branches = [] {
						const char* value = std::getenv("KYTY_UNIFORM_BRANCH");
						return value != nullptr && value[0] == '1';
					}();
					if (uniform_branches &&
					    (program.stage == ShaderType::Compute || program.stage == ShaderType::Pixel) &&
					    (kind == CFG::BranchCondition::ExecZero ||
					     kind == CFG::BranchCondition::ExecNonZero ||
					     kind == CFG::BranchCondition::VccZero ||
					     kind == CFG::BranchCondition::VccNonZero)) {
						requirements.subgroup_ballot = true;
					}
					break;
				}
				case IR::ValueOpcode::DppMoveU32:
				case IR::ValueOpcode::ReadFirstLane:
				case IR::ValueOpcode::ReadLane: {
					requirements.subgroup_ballot  = true;
					requirements.subgroup_shuffle = true;
					if (inst.GetOpcode() == IR::ValueOpcode::DppMoveU32) {
						requirements.subgroup_local_invocation_id = true;
					}
					break;
				}
				case IR::ValueOpcode::DppUpdateU32:
				case IR::ValueOpcode::WriteLane: {
					requirements.subgroup_ballot              = true;
					requirements.subgroup_local_invocation_id = true;
					break;
				}
				case IR::ValueOpcode::Permlane16U32: {
					requirements.subgroup_ballot              = true;
					requirements.subgroup_shuffle             = true;
					requirements.subgroup_local_invocation_id = true;
					break;
				}
				case IR::ValueOpcode::SwizzleU32:
				case IR::ValueOpcode::BpermuteU32: {
					requirements.subgroup_ballot              = true;
					requirements.subgroup_shuffle             = true;
					requirements.subgroup_local_invocation_id = true;
					break;
				}
				case IR::ValueOpcode::LaneId:
					requirements.subgroup_local_invocation_id |=
					    program.stage != ShaderType::TessellationControl;
					break;
				case IR::ValueOpcode::ImageQueryLod: requirements.compute_derivatives = true; break;
				case IR::ValueOpcode::ImageGatherRaw:
					requirements.image_gather_extended = true;
					break;
				case IR::ValueOpcode::SetAttribute: {
					const auto index = inst.Flags<IR::ExportFlags>().index;
					if (index >= program.export_info.size()) {
						Fail(program, "attribute export has invalid metadata");
					}
					if (program.stage == ShaderType::Pixel &&
					    program.export_info[index].vm) {
						requirements.pixel_valid_mask = true;
					}
					break;
				}
				default: break;
			}
		}
	}
	return requirements;
}

std::vector<uint32_t> EmitProgram(const IR::Program& program,
                                  ShaderStageInputInfo input_info, bool allow_dynamic_sample_offset) {
	using namespace Emitter;

	if (program.stage != ShaderType::Compute && program.stage != ShaderType::Vertex &&
	    program.stage != ShaderType::Pixel && program.stage != ShaderType::Mesh &&
	    program.stage != ShaderType::Local && program.stage != ShaderType::TessellationControl &&
	    program.stage != ShaderType::TessellationEvaluation) {
		Fail(program, "binary SPIR-V emitter received an unsupported shader stage");
	}
	if (!program.srt_plan_complete || !program.resource_tracking_complete ||
	    !program.shader_info_complete || !program.binding_layout_complete) {
		Fail(program, "SPIR-V emitter requires a fully planned native shader program");
	}
	ValidateNativeProgram(program);
	IR::ValidateProgram(program, true);
	EmitterState state(program, input_info);
	state.allow_dynamic_sample_offset = allow_dynamic_sample_offset;
	const auto* workgroup = ShaderWorkgroupInput(program.stage, input_info);
	state.lane_count =
	    workgroup != nullptr && program.wave_size == 64u && workgroup->host_subgroup_size == 32u
	        ? 2u
	        : 1u;
	state.function_lds_dwords = ProvenFunctionLdsDwords(state);
	LogFunctionStorage(state);
	DefineModule(state);
	EmitProgram(state);
	state.builder.AddEntryPoint(ExecutionModelForStage(state.program.stage), state.main_func,
	                            "main", state.interface_variables);

	return state.builder.Build();
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
