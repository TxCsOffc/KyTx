#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/backend/metal/MetalBackend.h"
#include "graphics/shader/recompiler/backend/msl/MslEmitter.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/Block.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/FastIrOptimizer.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace Libs::Graphics;
namespace Decoder      = ShaderRecompiler::Decoder;
namespace IR           = ShaderRecompiler::IR;
namespace MetalBackend = ShaderRecompiler::MetalBackend;

void Check(bool cond, const char* msg) {
	if (!cond) {
		std::fprintf(stderr, "FastTranslatorTests FAILED: %s\n", msg);
		std::abort();
	}
}

constexpr uint32_t Sop2(uint32_t op, uint32_t sdst, uint32_t ssrc0, uint32_t ssrc1) {
	return 0x80000000u | ((op & 0x7fu) << 23u) | ((sdst & 0x7fu) << 16u) |
	       ((ssrc1 & 0xffu) << 8u) | (ssrc0 & 0xffu);
}

constexpr uint32_t Sop1(uint32_t op, uint32_t sdst, uint32_t ssrc0) {
	return 0xBE800000u | ((sdst & 0x7fu) << 16u) | ((op & 0xffu) << 8u) | (ssrc0 & 0xffu);
}

constexpr uint32_t Sopk(uint32_t op, uint32_t sdst, uint16_t simm16) {
	return 0xB0000000u | ((op & 0x1fu) << 23u) | ((sdst & 0x7fu) << 16u) |
	       static_cast<uint32_t>(simm16);
}

constexpr uint32_t Sopp(uint32_t op, uint16_t simm16 = 0) {
	return 0xBF800000u | ((op & 0x7fu) << 16u) | static_cast<uint32_t>(simm16);
}

void TestDecoderLutsAndAnyPs5Opcodes() {
	const std::vector<uint32_t> code = {
	    Sop1(0x03u, 0u, 128u + 42u),       // s_mov_b32 s0, 42
	    Sop1(0x19u, 1u, 0u),               // s_sext_i32_i8 s1, s0
	    Sop1(0x1au, 2u, 0u),               // s_sext_i32_i16 s2, s0
	    Sop1(0x0du, 3u, 0u),               // s_bcnt0_i32_b32 s3, s0
	    Sop1(0x0eu, 4u, 0u),               // s_bcnt0_i32_b64 s4, s[0:1]
	    Sop1(0x11u, 5u, 0u),               // s_ff0_i32_b32 s5, s0
	    Sop1(0x17u, 6u, 0u),               // s_flbit_i32 s6, s0
	    Sop1(0x05u, 7u, 1u),               // s_cmov_b32 s7, s1
	    Sop1(0x24u, 8u, 2u),               // s_and_saveexec_b64 s[8:9], s[2:3]
	    Sop1(0x3cu, 10u, 2u),              // s_and_saveexec_b32 s10, s2
	    Sop2(0x2au, 12u, 0u, 128u + 4u),   // s_bfe_i64 s[12:13], s[0:1], 4
	    Sopk(0x02u, 7u, 0x1234u),          // s_cmovk_i32 s7, 0x1234
	    Sopp(0x21u, 1u),                   // s_clause 1
	    Sopp(0x24u, 0u),                   // s_round_mode 0
	    Sopp(0x25u, 0u),                   // s_denorm_mode 0
	    Sopp(0x22u, 0u),                   // s_wait_idle
	    Sopp(0x1fu, 0u),                   // s_code_end (also terminates program)
	};

	Decoder::Program program;
	Decoder::DecodeProgram(code, program);
	const auto& decoded = program.instructions;
	Check(decoded.size() == code.size(), "DecodeProgram did not decode all instructions");
	Check(decoded[0].opcode == Decoder::Opcode::S_MOV_B32, "expected S_MOV_B32");
	Check(decoded[0].src0.kind == Decoder::OperandKind::IntegerInlineConstant &&
	          decoded[0].src0.signed_val == 42,
	      "O(1) source LUT inline const 42 mismatch");
	Check(decoded[1].opcode == Decoder::Opcode::S_SEXT_I32_I8, "expected S_SEXT_I32_I8");
	Check(decoded[2].opcode == Decoder::Opcode::S_SEXT_I32_I16, "expected S_SEXT_I32_I16");
	Check(decoded[3].opcode == Decoder::Opcode::S_BCNT0_I32_B32, "expected S_BCNT0_I32_B32");
	Check(decoded[4].opcode == Decoder::Opcode::S_BCNT0_I32_B64, "expected S_BCNT0_I32_B64");
	Check(decoded[5].opcode == Decoder::Opcode::S_FF0_I32_B32, "expected S_FF0_I32_B32");
	Check(decoded[6].opcode == Decoder::Opcode::S_FLBIT_I32, "expected S_FLBIT_I32");
	Check(decoded[7].opcode == Decoder::Opcode::S_CMOV_B32, "expected S_CMOV_B32");
	Check(decoded[8].opcode == Decoder::Opcode::S_AND_SAVEEXEC_B64, "expected S_AND_SAVEEXEC_B64");
	Check(decoded[9].opcode == Decoder::Opcode::S_AND_SAVEEXEC_B32, "expected S_AND_SAVEEXEC_B32");
	Check(decoded[10].opcode == Decoder::Opcode::S_BFE_I64, "expected S_BFE_I64");
	Check(decoded[11].opcode == Decoder::Opcode::S_CMOVK_I32, "expected S_CMOVK_I32");
	Check(decoded[12].opcode == Decoder::Opcode::S_CLAUSE, "expected S_CLAUSE");
	Check(decoded[13].opcode == Decoder::Opcode::S_ROUND_MODE, "expected S_ROUND_MODE");
	Check(decoded[14].opcode == Decoder::Opcode::S_DENORM_MODE, "expected S_DENORM_MODE");
	Check(decoded[15].opcode == Decoder::Opcode::S_WAIT_IDLE, "expected S_WAIT_IDLE");
	Check(decoded[16].opcode == Decoder::Opcode::S_CODE_END, "expected S_CODE_END");

	// Also verify translation -> fast IR -> FastMslBackend end-to-end works on these opcodes.
	ShaderComputeInputInfo cs_info {};
	cs_info.threads_num[0]      = 64u;
	cs_info.threads_num[1]      = 1u;
	cs_info.threads_num[2]      = 1u;
	cs_info.host_subgroup_size  = 32u;
	ShaderRecompiler::CompileOptions opts {};
	opts.stage              = ShaderType::Compute;
	opts.wave_size          = 64u;
	opts.shader_hash        = 0xCAFEBABE00000001ull;
	opts.dump_ir            = false;
	opts.input_info.compute = &cs_info;

	auto translated = ShaderRecompiler::TranslateProgram(code, opts);
	auto finalized  = ShaderRecompiler::FinalizeProgramForBackend(std::move(translated), opts, {});
	const auto& backend = MetalBackend::GetMetalShaderBackend(MetalBackend::MetalBackendKind::FastMsl);
	auto emitted = backend.Emit(finalized.program, {.input_info = opts.input_info});
	Check(emitted.success, "FastMslBackend failed on AnyPS5 scalar program");
	Check(emitted.content_hash != 0, "FastMslBackend content_hash must be non-zero");
}

void TestFastIrOptimizerAndHelperPruning() {
	IR::Program program;
	program.stage     = ShaderType::Compute;
	program.wave_size = 32u;
	program.block_storage.push_back(std::make_unique<IR::Block>());
	IR::Block* block = program.block_storage.back().get();
	program.blocks.push_back(block);
	program.block_info.push_back({.id = 0u});
	program.memory_info.push_back({.kind = IR::ResourceKind::Lds});

	auto& x = block->AppendNewInst(IR::ValueOpcode::GetBuiltin,
	                               {IR::Value(static_cast<uint32_t>(IR::StageInputKind::GlobalInvocationId)), IR::Value(0u)});
	// Bitcast roundtrip: BitCastF32U32(BitCastU32F32(BitCastF32U32(x))) -> BitCastF32U32(x)
	auto& xf = block->AppendNewInst(IR::ValueOpcode::BitCastF32U32, {IR::Value(&x)});
	auto& xu = block->AppendNewInst(IR::ValueOpcode::BitCastU32F32, {IR::Value(&xf)});
	// Algebraic identity: (xu + 0) | (xu ^ xu) -> xu -> simplifies back to x!
	auto& add_zero = block->AppendNewInst(IR::ValueOpcode::IAdd32, {IR::Value(&xu), IR::Value(0u)});
	auto& xor_self = block->AppendNewInst(IR::ValueOpcode::BitwiseXor32, {IR::Value(&xu), IR::Value(&xu)});
	auto& combined = block->AppendNewInst(IR::ValueOpcode::BitwiseOr32, {IR::Value(&add_zero), IR::Value(&xor_self)});
	// Commutative local CSE: (x + 7) and (7 + x) in the same block -> collapse to 1 IAdd32
	auto& c1 = block->AppendNewInst(IR::ValueOpcode::IAdd32, {IR::Value(&combined), IR::Value(7u)});
	auto& c2 = block->AppendNewInst(IR::ValueOpcode::IAdd32, {IR::Value(7u), IR::Value(&x)});
	auto& res = block->AppendNewInst(IR::ValueOpcode::IMul32, {IR::Value(&c1), IR::Value(&c2)});
	// Double LogicalNot: !!(res == 0), stored as 1 or 0 to LDS
	auto& eq  = block->AppendNewInst(IR::ValueOpcode::IEqual32, {IR::Value(&res), IR::Value(0u)});
	auto& n1  = block->AppendNewInst(IR::ValueOpcode::LogicalNot, {IR::Value(&eq)});
	auto& n2  = block->AppendNewInst(IR::ValueOpcode::LogicalNot, {IR::Value(&n1)});
	auto& sel = block->AppendNewInst(IR::ValueOpcode::SelectU32, {IR::Value(&n2), IR::Value(1u), IR::Value(0u)});
	block->AppendNewInst(IR::ValueOpcode::WriteSharedU32, {IR::Value(0u), IR::Value(&sel), IR::Value(true)});

	const auto stats = IR::OptimizeFast(program);
	IR::EliminateDeadCode(program.blocks);

	Check(stats.bitcasts_eliminated >= 1u, "expected bitcast roundtrip elimination");
	Check(stats.simplified >= 3u, "expected algebraic/logical simplifications");
	Check(stats.cse_eliminated >= 1u, "expected commutative local CSE elimination");

	ShaderComputeInputInfo cs_info {};
	cs_info.threads_num[0]     = 32u;
	cs_info.threads_num[1]     = 1u;
	cs_info.threads_num[2]     = 1u;
	cs_info.lds_size_dwords    = 64u;
	cs_info.host_subgroup_size = 32u;
	ShaderStageInputInfo input {};
	input.compute = &cs_info;

	const auto& fast_msl = MetalBackend::GetMetalShaderBackend(MetalBackend::MetalBackendKind::FastMsl);
	auto msl = fast_msl.Emit(program, {.input_info = input});
	Check(msl.success, "FastMslBackend emit failed on optimized IR");
	// Unused helper functions must be pruned from the generated MSL source.
	Check(msl.msl_source.find("kyty_class_f32") == std::string::npos, "unused kyty_class_f32 not pruned");
	Check(msl.msl_source.find("kyty_f16_rtz") == std::string::npos, "unused kyty_f16_rtz not pruned");
	Check(msl.msl_source.find("kyty_d16_pack") == std::string::npos, "unused kyty_d16_pack not pruned");
	Check(msl.msl_source.find("kyty_med3f") == std::string::npos, "unused kyty_med3f not pruned");

	const auto& air_stub = MetalBackend::GetMetalShaderBackend(MetalBackend::MetalBackendKind::AirBitcode);
	Check(air_stub.Kind() == MetalBackend::MetalBackendKind::AirBitcode, "AirBitcode kind mismatch");
	auto stub_res = air_stub.Emit(program, {.input_info = input});
	Check(!stub_res.success && !stub_res.unsupported.empty(), "AirBitcode stub must report unimplemented cleanly");
}

void BenchmarkZeroSpirvVsFullSpirv() {
	// Construct a realistic scalar ALU chain in RDNA2 encoding.
	std::vector<uint32_t> code;
	code.push_back(Sop1(0x03u, 0u, 128u + 13u)); // s0 = 13
	for (uint32_t i = 0; i < 40; ++i) {
		code.push_back(Sop2(0x00u, (i & 7u) + 1u, i & 7u, 128u + (i % 15u))); // s_add_u32
		code.push_back(Sop2(0x26u, (i & 7u) + 1u, (i & 7u) + 1u, 128u + 3u)); // s_mul_i32
		code.push_back(Sop1(0x19u, (i & 7u) + 1u, (i & 7u) + 1u));            // s_sext_i32_i8
	}
	code.push_back(Sopp(0x01u, 0u)); // s_endpgm

	ShaderComputeInputInfo cs_info {};
	cs_info.threads_num[0]     = 64u;
	cs_info.threads_num[1]     = 1u;
	cs_info.threads_num[2]     = 1u;
	cs_info.host_subgroup_size = 32u;

	ShaderRecompiler::CompileOptions opts {};
	opts.stage              = ShaderType::Compute;
	opts.wave_size          = 64u;
	opts.shader_hash        = 0xBEEF000000001234ull;
	opts.dump_ir            = false;
	opts.optimization_type  = Config::ShaderOptimizationType::Performance;
	opts.input_info.compute = &cs_info;

	const auto& fast_msl = MetalBackend::GetMetalShaderBackend(MetalBackend::MetalBackendKind::FastMsl);
	constexpr int kIters = 100;

	const auto t0 = std::chrono::steady_clock::now();
	size_t     msl_bytes = 0;
	for (int i = 0; i < kIters; ++i) {
		auto tr  = ShaderRecompiler::TranslateProgram(code, opts);
		auto fin = ShaderRecompiler::FinalizeProgramForBackend(std::move(tr), opts, {});
		auto res = fast_msl.Emit(fin.program, {.input_info = opts.input_info});
		Check(res.success, "benchmark FastMslBackend failed");
		msl_bytes += res.msl_source.size();
	}
	const auto t1 = std::chrono::steady_clock::now();

	const auto t2 = std::chrono::steady_clock::now();
	size_t     spv_words = 0;
	for (int i = 0; i < kIters; ++i) {
		auto tr  = ShaderRecompiler::TranslateProgram(code, opts);
		auto res = ShaderRecompiler::CompileProgram(std::move(tr), opts, {});
		spv_words += res.spirv.size();
	}
	const auto t3 = std::chrono::steady_clock::now();

	const double msl_us =
	    std::chrono::duration<double, std::micro>(t1 - t0).count() / static_cast<double>(kIters);
	const double spv_us =
	    std::chrono::duration<double, std::micro>(t3 - t2).count() / static_cast<double>(kIters);
	std::printf("[Bench] Decode+IR+FastMSL=%.1f us/iter (avg %zu B) vs Decode+IR+SPIRV+Opt=%.1f us/iter (avg %zu words) -> %.2fx\n",
	            msl_us, msl_bytes / kIters, spv_us, spv_words / kIters, spv_us / (msl_us > 0.0 ? msl_us : 1.0));
}

} // namespace

int main() {
	TestDecoderLutsAndAnyPs5Opcodes();
	TestFastIrOptimizerAndHelperPruning();
	BenchmarkZeroSpirvVsFullSpirv();
	std::printf("FastTranslatorTests passed.\n");
	return 0;
}
