#include "graphics/shader/recompiler/ir/passes/FastIrOptimizer.h"

#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

bool FastIrOptEnabled() {
	static const bool s_enabled = [] {
		const char* value = std::getenv("KYTY_FAST_IR_OPT");
		return value == nullptr || value[0] != '0';
	}();
	return s_enabled;
}

bool SameImmediateOrInst(const Value& a, const Value& b) {
	return a == b;
}

std::optional<uint32_t> TryConstU32(const Value& v) {
	if (v.TryInstruction() == nullptr && v.GetType() == Type::U32) {
		return v.U32();
	}
	return std::nullopt;
}

std::optional<bool> TryConstU1(const Value& v) {
	if (v.TryInstruction() == nullptr && v.GetType() == Type::U1) {
		return v.U1();
	}
	return std::nullopt;
}

Value UnwrapInverseBitcast(const Value& v, ValueOpcode inner_op) {
	const auto* inst = v.TryInstruction();
	if (inst != nullptr && inst->GetOpcode() == inner_op && inst->NumArgs() == 1u) {
		return inst->Arg(0);
	}
	return Value {};
}

bool IsCommutativePureOp(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::IAdd32:
		case ValueOpcode::IMul32:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::IEqual32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::SMin32:
		case ValueOpcode::UMin32:
		case ValueOpcode::SMax32:
		case ValueOpcode::UMax32:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalXor: return true;
		default: return false;
	}
}

bool IsPureCseEligible(const Inst& inst) {
	if (inst.MayHaveSideEffects() || inst.NumArgs() > 4u) {
		return false;
	}
	switch (inst.GetOpcode()) {
		case ValueOpcode::BitCastU16F16:
		case ValueOpcode::BitCastF16U16:
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::BitCastU64F64:
		case ValueOpcode::BitCastF64U64:
		case ValueOpcode::ConvertU16U32:
		case ValueOpcode::ConvertU32U16:
		case ValueOpcode::ConvertU8U32:
		case ValueOpcode::ConvertU32U8:
		case ValueOpcode::ConvertF32F16:
		case ValueOpcode::ConvertF16F32:
		case ValueOpcode::ConvertS32F32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32S32:
		case ValueOpcode::ConvertF64S32:
		case ValueOpcode::ConvertF32F64:
		case ValueOpcode::ConvertF64F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::ConvertF64U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeConstructU32x3:
		case ValueOpcode::CompositeConstructF32x2:
		case ValueOpcode::CompositeConstructU32x4:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::CompositeExtractU32x3:
		case ValueOpcode::CompositeExtractU32x4:
		case ValueOpcode::PackHalf2x16:
		case ValueOpcode::PackSnorm2x16:
		case ValueOpcode::PackUnorm2x16:
		case ValueOpcode::PackFloat2x16Rtz:
		case ValueOpcode::FPAbs32:
		case ValueOpcode::FPNeg32:
		case ValueOpcode::FPSaturate32:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::IAddCarry32:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::IMul64:
		case ValueOpcode::UDiv32:
		case ValueOpcode::SMulHi:
		case ValueOpcode::UMulHi:
		case ValueOpcode::IAbs32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::BitReverse32:
		case ValueOpcode::BitCount32:
		case ValueOpcode::BitCount64:
		case ValueOpcode::FindUMsb32:
		case ValueOpcode::FindUMsb64:
		case ValueOpcode::FindILsb32:
		case ValueOpcode::SMin32:
		case ValueOpcode::UMin32:
		case ValueOpcode::SMax32:
		case ValueOpcode::UMax32:
		case ValueOpcode::SMinTri32:
		case ValueOpcode::UMinTri32:
		case ValueOpcode::SMaxTri32:
		case ValueOpcode::UMaxTri32:
		case ValueOpcode::SMedTri32:
		case ValueOpcode::UMedTri32:
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThan64:
		case ValueOpcode::SLessThanEqual64:
		case ValueOpcode::ULessThanEqual64:
		case ValueOpcode::UGreaterThanEqual64:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThan64:
		case ValueOpcode::IEqual32:
		case ValueOpcode::IEqual64:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::UGreaterThan64:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::INotEqual64:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::UGreaterThanEqual32:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdEqual32:
		case ValueOpcode::FPOrdEqual64:
		case ValueOpcode::FPOrdLessThanEqual64:
		case ValueOpcode::FPOrdGreaterThanEqual64:
		case ValueOpcode::FPUnordEqual32:
		case ValueOpcode::FPOrdNotEqual32:
		case ValueOpcode::FPUnordNotEqual32:
		case ValueOpcode::FPOrdLessThan32:
		case ValueOpcode::FPUnordLessThan32:
		case ValueOpcode::FPOrdGreaterThan32:
		case ValueOpcode::FPUnordGreaterThan32:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPUnordLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPUnordGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPCmpClass32:
		case ValueOpcode::FPAdd32:
		case ValueOpcode::FPSub32:
		case ValueOpcode::FPFma32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPAdd64:
		case ValueOpcode::FPMul64:
		case ValueOpcode::FPFma64:
		case ValueOpcode::FPRecip64:
		case ValueOpcode::FPMin32:
		case ValueOpcode::FPMax32:
		case ValueOpcode::FPMinTri32:
		case ValueOpcode::FPMaxTri32:
		case ValueOpcode::FPMedTri32:
		case ValueOpcode::FPRecip32:
		case ValueOpcode::FPRecipIFlag32:
		case ValueOpcode::FPRecipSqrt32:
		case ValueOpcode::FPSqrt:
		case ValueOpcode::FPSin:
		case ValueOpcode::FPCos:
		case ValueOpcode::FPExp2:
		case ValueOpcode::FPLog2:
		case ValueOpcode::FPLdexp:
		case ValueOpcode::FPRoundEven32:
		case ValueOpcode::FPFloor32:
		case ValueOpcode::FPCeil32:
		case ValueOpcode::FPTrunc32:
		case ValueOpcode::FPFract32: return true;
		default: return false;
	}
}

uint64_t EncodeOperandKey(const Value& v) {
	if (const auto* inst = v.TryInstruction()) {
		return (static_cast<uint64_t>(Type::Void) << 56u) ^ reinterpret_cast<uintptr_t>(inst);
	}
	const uint64_t tag = static_cast<uint64_t>(v.GetType()) << 56u;
	switch (v.GetType()) {
		case Type::U1: return tag | (v.U1() ? 1ull : 0ull);
		case Type::U8: return tag | v.U8();
		case Type::U16: return tag | v.U16();
		case Type::U32: return tag | v.U32();
		case Type::F16: return tag | v.F16Bits();
		case Type::F32: return tag | std::bit_cast<uint32_t>(v.F32Value());
		case Type::U64: return (0xfeull << 56u) ^ v.U64();
		case Type::ScalarReg: return tag | static_cast<uint32_t>(v.ScalarRegister());
		case Type::VectorReg: return tag | static_cast<uint32_t>(v.VectorRegister());
		default: return tag;
	}
}

struct CseKey {
	uint32_t                opcode   = 0;
	uint32_t                num_args = 0;
	uint64_t                flags    = 0;
	std::array<uint64_t, 4> args     = {};

	bool operator==(const CseKey& other) const noexcept = default;
};

struct CseKeyHash {
	size_t operator()(const CseKey& k) const noexcept {
		uint64_t h = (static_cast<uint64_t>(k.opcode) << 32u) | k.num_args;
		h ^= k.flags * 0x9e3779b97f4a7c15ull;
		for (uint32_t i = 0; i < k.num_args; ++i) {
			h ^= k.args[i] + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u);
		}
		return static_cast<size_t>(h);
	}
};

Value SimplifyInstruction(Inst& inst, FastIrOptStats& stats) {
	const auto op = inst.GetOpcode();
	switch (op) {
		case ValueOpcode::BitCastF32U32:
			if (const auto r = UnwrapInverseBitcast(inst.Arg(0), ValueOpcode::BitCastU32F32);
			    !r.IsEmpty() && r.GetType() == inst.GetType()) {
				++stats.bitcasts_eliminated;
				return r;
			}
			break;
		case ValueOpcode::BitCastU32F32:
			if (const auto r = UnwrapInverseBitcast(inst.Arg(0), ValueOpcode::BitCastF32U32);
			    !r.IsEmpty() && r.GetType() == inst.GetType()) {
				++stats.bitcasts_eliminated;
				return r;
			}
			break;
		case ValueOpcode::BitCastF16U16:
			if (const auto r = UnwrapInverseBitcast(inst.Arg(0), ValueOpcode::BitCastU16F16);
			    !r.IsEmpty() && r.GetType() == inst.GetType()) {
				++stats.bitcasts_eliminated;
				return r;
			}
			break;
		case ValueOpcode::BitCastU16F16:
			if (const auto r = UnwrapInverseBitcast(inst.Arg(0), ValueOpcode::BitCastF16U16);
			    !r.IsEmpty() && r.GetType() == inst.GetType()) {
				++stats.bitcasts_eliminated;
				return r;
			}
			break;
		case ValueOpcode::BitCastF64U64:
			if (const auto r = UnwrapInverseBitcast(inst.Arg(0), ValueOpcode::BitCastU64F64);
			    !r.IsEmpty() && r.GetType() == inst.GetType()) {
				++stats.bitcasts_eliminated;
				return r;
			}
			break;
		case ValueOpcode::BitCastU64F64:
			if (const auto r = UnwrapInverseBitcast(inst.Arg(0), ValueOpcode::BitCastF64U64);
			    !r.IsEmpty() && r.GetType() == inst.GetType()) {
				++stats.bitcasts_eliminated;
				return r;
			}
			break;
		case ValueOpcode::LogicalNot:
			if (const auto r = UnwrapInverseBitcast(inst.Arg(0), ValueOpcode::LogicalNot);
			    !r.IsEmpty() && r.GetType() == inst.GetType()) {
				return r;
			}
			break;
		case ValueOpcode::BitwiseNot32:
			if (const auto r = UnwrapInverseBitcast(inst.Arg(0), ValueOpcode::BitwiseNot32);
			    !r.IsEmpty() && r.GetType() == inst.GetType()) {
				return r;
			}
			break;
		case ValueOpcode::FPNeg32:
			if (const auto r = UnwrapInverseBitcast(inst.Arg(0), ValueOpcode::FPNeg32);
			    !r.IsEmpty() && r.GetType() == inst.GetType()) {
				return r;
			}
			break;
		case ValueOpcode::FPAbs32:
			if (const auto* inner = inst.Arg(0).TryInstruction()) {
				if (inner->GetOpcode() == ValueOpcode::FPAbs32) {
					return inst.Arg(0);
				}
				if (inner->GetOpcode() == ValueOpcode::FPNeg32 &&
				    inner->Arg(0).GetType() == Type::F32) {
					inst.SetArg(0, inner->Arg(0));
				}
			}
			break;
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32: {
			if (const auto* cond_inst = inst.Arg(0).TryInstruction();
			    cond_inst != nullptr && cond_inst->GetOpcode() == ValueOpcode::LogicalNot &&
			    cond_inst->NumArgs() == 1u) {
				const auto inner_cond = cond_inst->Arg(0);
				const auto t_val      = inst.Arg(1);
				const auto f_val      = inst.Arg(2);
				inst.SetArg(0, inner_cond);
				inst.SetArg(1, f_val);
				inst.SetArg(2, t_val);
			}
			if (const auto* t_inst = inst.Arg(1).TryInstruction();
			    t_inst != nullptr && t_inst->GetOpcode() == op &&
			    SameImmediateOrInst(t_inst->Arg(0), inst.Arg(0)) &&
			    t_inst->Arg(1).GetType() == inst.GetType()) {
				inst.SetArg(1, t_inst->Arg(1));
			}
			if (const auto* f_inst = inst.Arg(2).TryInstruction();
			    f_inst != nullptr && f_inst->GetOpcode() == op &&
			    SameImmediateOrInst(f_inst->Arg(0), inst.Arg(0)) &&
			    f_inst->Arg(2).GetType() == inst.GetType()) {
				inst.SetArg(2, f_inst->Arg(2));
			}
			if (const auto c = TryConstU1(inst.Arg(0))) {
				const auto chosen = *c ? inst.Arg(1) : inst.Arg(2);
				if (chosen.GetType() == inst.GetType()) {
					++stats.selects_eliminated;
					return chosen;
				}
			}
			if (SameImmediateOrInst(inst.Arg(1), inst.Arg(2)) &&
			    inst.Arg(1).GetType() == inst.GetType()) {
				++stats.selects_eliminated;
				return inst.Arg(1);
			}
			break;
		}
		case ValueOpcode::IAdd32:
			if (const auto a = TryConstU32(inst.Arg(0)); a && *a == 0u &&
			    inst.Arg(1).GetType() == Type::U32) {
				return inst.Arg(1);
			}
			if (const auto b = TryConstU32(inst.Arg(1)); b && *b == 0u &&
			    inst.Arg(0).GetType() == Type::U32) {
				return inst.Arg(0);
			}
			break;
		case ValueOpcode::ISub32:
			if (const auto b = TryConstU32(inst.Arg(1)); b && *b == 0u &&
			    inst.Arg(0).GetType() == Type::U32) {
				return inst.Arg(0);
			}
			if (SameImmediateOrInst(inst.Arg(0), inst.Arg(1))) {
				return Value(0u);
			}
			break;
		case ValueOpcode::IMul32:
			if (const auto a = TryConstU32(inst.Arg(0))) {
				if (*a == 0u) return Value(0u);
				if (*a == 1u && inst.Arg(1).GetType() == Type::U32) return inst.Arg(1);
			}
			if (const auto b = TryConstU32(inst.Arg(1))) {
				if (*b == 0u) return Value(0u);
				if (*b == 1u && inst.Arg(0).GetType() == Type::U32) return inst.Arg(0);
			}
			break;
		case ValueOpcode::BitwiseAnd32:
			if (const auto a = TryConstU32(inst.Arg(0))) {
				if (*a == 0u) return Value(0u);
				if (*a == 0xffffffffu && inst.Arg(1).GetType() == Type::U32) return inst.Arg(1);
			}
			if (const auto b = TryConstU32(inst.Arg(1))) {
				if (*b == 0u) return Value(0u);
				if (*b == 0xffffffffu && inst.Arg(0).GetType() == Type::U32) return inst.Arg(0);
			}
			if (SameImmediateOrInst(inst.Arg(0), inst.Arg(1)) &&
			    inst.Arg(0).GetType() == Type::U32) {
				return inst.Arg(0);
			}
			break;
		case ValueOpcode::BitwiseOr32:
			if (const auto a = TryConstU32(inst.Arg(0))) {
				if (*a == 0xffffffffu) return Value(0xffffffffu);
				if (*a == 0u && inst.Arg(1).GetType() == Type::U32) return inst.Arg(1);
			}
			if (const auto b = TryConstU32(inst.Arg(1))) {
				if (*b == 0xffffffffu) return Value(0xffffffffu);
				if (*b == 0u && inst.Arg(0).GetType() == Type::U32) return inst.Arg(0);
			}
			if (SameImmediateOrInst(inst.Arg(0), inst.Arg(1)) &&
			    inst.Arg(0).GetType() == Type::U32) {
				return inst.Arg(0);
			}
			break;
		case ValueOpcode::BitwiseXor32:
			if (const auto a = TryConstU32(inst.Arg(0)); a && *a == 0u &&
			    inst.Arg(1).GetType() == Type::U32) {
				return inst.Arg(1);
			}
			if (const auto b = TryConstU32(inst.Arg(1)); b && *b == 0u &&
			    inst.Arg(0).GetType() == Type::U32) {
				return inst.Arg(0);
			}
			if (SameImmediateOrInst(inst.Arg(0), inst.Arg(1))) {
				return Value(0u);
			}
			break;
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightArithmetic32:
			if (const auto b = TryConstU32(inst.Arg(1)); b && (*b & 31u) == 0u &&
			    inst.Arg(0).GetType() == Type::U32) {
				return inst.Arg(0);
			}
			break;
		case ValueOpcode::LogicalAnd:
			if (const auto a = TryConstU1(inst.Arg(0))) {
				if (!*a) return Value(false);
				if (inst.Arg(1).GetType() == Type::U1) return inst.Arg(1);
			}
			if (const auto b = TryConstU1(inst.Arg(1))) {
				if (!*b) return Value(false);
				if (inst.Arg(0).GetType() == Type::U1) return inst.Arg(0);
			}
			if (SameImmediateOrInst(inst.Arg(0), inst.Arg(1)) &&
			    inst.Arg(0).GetType() == Type::U1) {
				return inst.Arg(0);
			}
			break;
		case ValueOpcode::LogicalOr:
			if (const auto a = TryConstU1(inst.Arg(0))) {
				if (*a) return Value(true);
				if (inst.Arg(1).GetType() == Type::U1) return inst.Arg(1);
			}
			if (const auto b = TryConstU1(inst.Arg(1))) {
				if (*b) return Value(true);
				if (inst.Arg(0).GetType() == Type::U1) return inst.Arg(0);
			}
			if (SameImmediateOrInst(inst.Arg(0), inst.Arg(1)) &&
			    inst.Arg(0).GetType() == Type::U1) {
				return inst.Arg(0);
			}
			break;
		case ValueOpcode::LogicalXor:
			if (const auto a = TryConstU1(inst.Arg(0)); a && !*a &&
			    inst.Arg(1).GetType() == Type::U1) {
				return inst.Arg(1);
			}
			if (const auto b = TryConstU1(inst.Arg(1)); b && !*b &&
			    inst.Arg(0).GetType() == Type::U1) {
				return inst.Arg(0);
			}
			if (SameImmediateOrInst(inst.Arg(0), inst.Arg(1))) {
				return Value(false);
			}
			break;
		case ValueOpcode::IEqual32:
			if (SameImmediateOrInst(inst.Arg(0), inst.Arg(1))) return Value(true);
			break;
		case ValueOpcode::INotEqual32:
		case ValueOpcode::SLessThan32:
		case ValueOpcode::ULessThan32:
			if (SameImmediateOrInst(inst.Arg(0), inst.Arg(1))) return Value(false);
			break;
		case ValueOpcode::CompositeExtractU64:
			if (const auto* ctor = inst.Arg(0).TryInstruction();
			    ctor != nullptr && ctor->GetOpcode() == ValueOpcode::CompositeConstructU64) {
				if (const auto idx = TryConstU32(inst.Arg(1));
				    idx && *idx < ctor->NumArgs() && ctor->Arg(*idx).GetType() == inst.GetType()) {
					return ctor->Arg(*idx);
				}
			}
			break;
		case ValueOpcode::CompositeExtractU32x2:
			if (const auto* ctor = inst.Arg(0).TryInstruction();
			    ctor != nullptr && ctor->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
				if (const auto idx = TryConstU32(inst.Arg(1));
				    idx && *idx < ctor->NumArgs() && ctor->Arg(*idx).GetType() == inst.GetType()) {
					return ctor->Arg(*idx);
				}
			}
			break;
		case ValueOpcode::CompositeExtractU32x3:
			if (const auto* ctor = inst.Arg(0).TryInstruction();
			    ctor != nullptr && ctor->GetOpcode() == ValueOpcode::CompositeConstructU32x3) {
				if (const auto idx = TryConstU32(inst.Arg(1));
				    idx && *idx < ctor->NumArgs() && ctor->Arg(*idx).GetType() == inst.GetType()) {
					return ctor->Arg(*idx);
				}
			}
			break;
		case ValueOpcode::CompositeExtractU32x4:
			if (const auto* ctor = inst.Arg(0).TryInstruction();
			    ctor != nullptr && ctor->GetOpcode() == ValueOpcode::CompositeConstructU32x4) {
				if (const auto idx = TryConstU32(inst.Arg(1));
				    idx && *idx < ctor->NumArgs() && ctor->Arg(*idx).GetType() == inst.GetType()) {
					return ctor->Arg(*idx);
				}
			}
			break;
		default: break;
	}
	return Value {};
}

} // namespace

FastIrOptStats OptimizeFast(Program& program) {
	FastIrOptStats stats {};
	if (!FastIrOptEnabled()) {
		return stats;
	}

	std::unordered_map<CseKey, Inst*, CseKeyHash> seen;
	for (auto* block: program.blocks) {
		seen.clear();
		auto& instructions = block->Instructions();
		for (auto it = instructions.begin(); it != instructions.end();) {
			auto& inst = *it;
			const auto op = inst.GetOpcode();
			if (op == ValueOpcode::Void || op == ValueOpcode::Phi || op == ValueOpcode::Identity) {
				++it;
				continue;
			}
			if (const auto simplified = SimplifyInstruction(inst, stats); !simplified.IsEmpty()) {
				inst.ReplaceUsesWith(simplified, false);
				++stats.simplified;
				it = instructions.erase(it);
				continue;
			}
			if (IsPureCseEligible(inst)) {
				CseKey key {};
				key.opcode   = static_cast<uint32_t>(op);
				key.num_args = static_cast<uint32_t>(inst.NumArgs());
				key.flags    = inst.Flags<uint64_t>();
				for (uint32_t i = 0; i < key.num_args; ++i) {
					key.args[i] = EncodeOperandKey(inst.Arg(i));
				}
				if (key.num_args == 2u && IsCommutativePureOp(op) && key.args[0] > key.args[1]) {
					std::swap(key.args[0], key.args[1]);
				}
				const auto [slot, inserted] = seen.emplace(key, &inst);
				if (!inserted && slot->second->GetType() == inst.GetType()) {
					inst.ReplaceUsesWith(Value(slot->second), false);
					++stats.cse_eliminated;
					it = instructions.erase(it);
					continue;
				}
			}
			++it;
		}
	}
	return stats;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
