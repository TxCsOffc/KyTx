// Decode real RDNA words, translate to IR, then check both GPU compiler paths.
#include "common/assert.h"
#include "graphics/shader/recompiler/backend/msl/MslEmitter.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/SsaRewrite.h"

#import <Metal/Metal.h>
#include <array>
#include <cstring>
#include <random>
#include <stdexcept>
using namespace Libs::Graphics;
namespace IR = ShaderRecompiler::IR;
using IR::Value;
using IR::ValueOpcode;
static id<MTLDevice> device;
static void          Check(bool value, const char* message) {
	if (!value) throw std::runtime_error(message);
}
struct Fixture {
	IR::Program program;
	uint32_t    lds_dwords = 0;
	Fixture() {
		program.stage     = ShaderType::Compute;
		program.wave_size = 32;
		program.info.buffers.resize(2);
		program.bindings.descriptors.push_back({IR::DescriptorBindingKind::Buffers, {0, 1}});
		program.block_storage.push_back(std::make_unique<IR::Block>());
		program.blocks.push_back(program.block_storage.back().get());
		program.block_info.push_back({.id = 0});
	}
	Value Emit(ValueOpcode op, std::initializer_list<Value> a, uint64_t flags = 0) {
		return Value(&program.blocks[0]->AppendNewInst(op, a, flags));
	}
};
#include "metalShaderPathReference.inc"
struct Form {
	uint32_t opcode;
	bool     clamp;
};
static constexpr Form forms[] = {{0x14d, false}, {0x15a, false}, {0x15a, true},
                                 {0x15b, false}, {0x15b, true},  {0x15c, false},
                                 {0x15c, true},  {0x15d, false}, {0x15d, true}};
static uint32_t       Expected(Form f, const std::array<uint32_t, 3>& x) {
	if (f.opcode == 0x14d) {
		uint32_t result = 0;
		for (unsigned byte = 0; byte < 4; ++byte) {
			const unsigned shift = byte * 8;
			const uint32_t a = (x[0] >> shift) & 255, b = (x[1] >> shift) & 255,
			               round = (x[2] >> shift) & 1;
			result |= ((a + b + round) / 2) << shift;
		}
		return result;
	}
	uint64_t       sum  = 0;
	const unsigned bits = f.opcode == 0x15c ? 16 : f.opcode == 0x15d ? 32 : 8;
	for (unsigned bit = 0; bit < 32; bit += bits) {
		const uint64_t mask = bits == 32 ? 0xffffffffull : (1ull << bits) - 1,
		               a = (x[0] >> bit) & mask, b = (x[1] >> bit) & mask;
		sum += a > b ? a - b : b - a;
	}
	if (f.opcode == 0x15b) sum <<= 16;
	sum += x[2];
	return f.clamp && sum > UINT32_MAX ? UINT32_MAX : uint32_t(sum);
}
static void RejectIllegalModifiers() {
	for (uint32_t opcode: {0x14du, 0x15au, 0x15bu, 0x15cu, 0x15du}) {
		for (unsigned modifier = 0; modifier < 4; ++modifier) {
			uint32_t words[] = {0xd4000000u | (opcode << 16u) | 3u,
			                    256u | (257u << 9u) | (258u << 18u)};
			if (modifier == 0)
				words[0] |= 1u << 8u;
			else if (modifier == 1)
				words[0] |= 1u << 11u;
			else if (modifier == 2)
				words[1] |= 1u << 27u;
			else
				words[1] |= 1u << 29u;
			ShaderRecompiler::Decoder::Instruction decoded {};
			ShaderRecompiler::Decoder::DecodeInstruction(words, 0, decoded);
			Check(decoded.opcode == ShaderRecompiler::Decoder::Opcode::UNSUPPORTED,
			      "illegal packed arithmetic modifier accepted");
		}
	}
	uint32_t words[] = {0xd4000000u | (0x14du << 16u) | (1u << 15u) | 3u,
	                    256u | (257u << 9u) | (258u << 18u)};
	ShaderRecompiler::Decoder::Instruction decoded {};
	ShaderRecompiler::Decoder::DecodeInstruction(words, 0, decoded);
	Check(decoded.opcode == ShaderRecompiler::Decoder::Opcode::UNSUPPORTED,
	      "unsupported LERP clamp accepted");
}
static Fixture TranslateForms() {
	Fixture       f;
	IR::IREmitter ir(f.program.blocks[0]);
	ir.SetExec(IR::U1(Value(true)));
	const auto index       = f.Emit(ValueOpcode::GetBuiltin,
	                                {Value(uint32_t(IR::StageInputKind::WorkgroupId)), Value(0u)});
	const auto inputOffset = f.Emit(ValueOpcode::IMul32, {index, Value(12u)});
	const auto outputOffset =
	    f.Emit(ValueOpcode::IMul32, {index, Value(uint32_t(std::size(forms) * 4))});
	f.program.memory_info.push_back({.kind = IR::ResourceKind::Buffer, .resource = 0});
	f.program.memory_info.push_back({.kind = IR::ResourceKind::Buffer, .resource = 1});
	const auto input  = f.Emit(ValueOpcode::GetBufferResource,
	                           {Value(0u), Value(0u), Value(0u), Value(0u)}),
	           output = f.Emit(ValueOpcode::GetBufferResource,
			                   {Value(0u), Value(0u), Value(0u), Value(0u)});
	for (unsigned s = 0; s < 3; ++s) {
		const auto offset = f.Emit(ValueOpcode::IAdd32, {inputOffset, Value(s * 4u)});
		const auto data   = f.Emit(ValueOpcode::LoadBufferU32,
		                           {input, Value(0u), offset, Value(0u), Value(true)}, 0);
		ir.SetVectorReg(IR::VectorReg(s), IR::U32(data));
	}
	ShaderRecompiler::Frontend::Translator translator(f.program, f.program.blocks[0], 16, false);
	for (unsigned i = 0; i < std::size(forms); ++i) {
		const auto                             test    = forms[i];
		const uint32_t                         words[] = {0xd4000000u | (test.opcode << 16u) |
		                                                      (test.clamp ? 1u << 15u : 0u) | 3u,
		                                                  256u | (257u << 9u) | (258u << 18u)};
		ShaderRecompiler::Decoder::Instruction decoded {};
		ShaderRecompiler::Decoder::DecodeInstruction(words, 0, decoded);
		if (decoded.opcode == ShaderRecompiler::Decoder::Opcode::UNSUPPORTED) {
			std::fprintf(stderr, "unsupported actual RDNA form: opcode=%03x clamp=%u\n",
			             test.opcode, test.clamp);
			throw std::runtime_error("packed arithmetic decoder coverage gap");
		}
		Check(decoded.family == ShaderRecompiler::Decoder::Family::VOP3 && decoded.src_count == 3 &&
		          decoded.word_count == 2,
		      "packed form decode layout");
		translator.TranslateInstruction(decoded);
		const auto result = ir.GetVectorReg(IR::VectorReg(3));
		const auto offset = f.Emit(ValueOpcode::IAdd32, {outputOffset, Value(i * 4u)});
		f.Emit(ValueOpcode::StoreBufferU32,
		       {output, Value(0u), offset, Value(0u), result, Value(true)}, 1);
	}
	IR::RewriteToSsa(f.program.blocks);
	IR::EliminateDeadCode(f.program.blocks);
	return f;
}
int main() {
	@autoreleasepool {
		try {
			RejectIllegalModifiers();
			device = MTLCreateSystemDefaultDevice();
			Check(device != nil, "Metal device unavailable");
			std::vector<std::array<uint32_t, 3>> inputs;
			const uint32_t edges[] = {0,          1,          0x01010101, 0x80808080, 0x7f7f7f7f,
			                          0x00ff00ff, 0xff00ff00, 0x010080ff, 0xffff,     0xffff0000,
			                          0x7fffffff, 0x80000000, 0xffffff00, 0xfffffffe, 0xffffffff};
			for (auto a: edges)
				for (auto b: edges)
					for (auto c: edges)
						inputs.push_back({a, b, c});
			std::mt19937 rng(0x5ad1e2f8);
			for (unsigned i = 0; i < 8192; ++i)
				inputs.push_back({rng(), rng(), rng()});
			Fixture f                                 = TranslateForms();
			f.program.info.buffers[0].read            = true;
			f.program.info.buffers[0].max_byte_extent = inputs.size() * 12;
			f.program.info.buffers[1].written         = true;
			f.program.info.buffers[1].max_byte_extent = inputs.size() * std::size(forms) * 4;
			Metal::SpvcAbiLayout   abi;
			id<MTLFunction>        reference = CompileVulkanShaderPath(f, abi);
			ShaderComputeInputInfo compute {};
			compute.threads_num[0] = compute.threads_num[1] = compute.threads_num[2] = 1;
			compute.wave_size = compute.host_subgroup_size = 32;
			ShaderStageInputInfo stage {};
			stage.compute     = &compute;
			const auto native = ShaderRecompiler::Msl::EmitProgram(f.program, stage);
			Check(native.success, "native packed IR emission");
			MTLCompileOptions* opts         = [MTLCompileOptions new];
			opts.mathMode                   = MTLMathModeSafe;
			opts.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
			NSError*       error            = nil;
			id<MTLLibrary> lib =
			    [device newLibraryWithSource:[NSString stringWithUTF8String:native.source.c_str()]
			                         options:opts
			                           error:&error];
			if (!lib) fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
			Check(lib != nil, "native packed arithmetic library");
			id<MTLFunction> functions[] = {[lib newFunctionWithName:@"main0"], reference};
			for (unsigned path = 0; path < 2; ++path) {
				id<MTLFunction>             fn = functions[path];
				id<MTLComputePipelineState> pipeline =
				    [device newComputePipelineStateWithFunction:fn error:&error];
				Check(pipeline != nil, "packed arithmetic pipeline");
				id<MTLBuffer> input  = [device newBufferWithBytes:inputs.data()
				                                           length:inputs.size() * 12
				                                          options:MTLResourceStorageModeShared],
				              output = [device
				                  newBufferWithLength:inputs.size() * std::size(forms) * 4 + 64
				                              options:MTLResourceStorageModeShared];
				memset(output.contents, 0xcd, output.length);
				id<MTLArgumentEncoder> args = [fn newArgumentEncoderWithBufferIndex:0];
				id<MTLBuffer> ab = [device newBufferWithLength:args.encodedLength
				                                       options:MTLResourceStorageModeShared];
				memset(ab.contents, 0, ab.length);
				[args setArgumentBuffer:ab offset:0];
				const unsigned base = path ? abi.BaseOf(IR::DescriptorBindingKind::Buffers) : 0;
				[args setBuffer:input offset:0 atIndex:base];
				[args setBuffer:output offset:0 atIndex:base + 1];
				uint32_t sizes[256] {};
				sizes[base]              = uint32_t(input.length);
				sizes[base + 1]          = uint32_t(output.length - 64);
				id<MTLBuffer> sizeBuffer = [device newBufferWithBytes:sizes
				                                               length:sizeof(sizes)
				                                              options:MTLResourceStorageModeShared];
				if (path) [args setBuffer:sizeBuffer offset:0 atIndex:0];
				uint32_t                     push[32] {};
				id<MTLCommandQueue>          queue = [device newCommandQueue];
				id<MTLCommandBuffer>         cmd   = [queue commandBuffer];
				id<MTLComputeCommandEncoder> enc   = [cmd computeCommandEncoder];
				[enc setComputePipelineState:pipeline];
				[enc setBuffer:ab offset:0 atIndex:0];
				[enc setBytes:push length:sizeof(push) atIndex:1];
				[enc setBuffer:sizeBuffer offset:0 atIndex:25];
				[enc useResource:input usage:MTLResourceUsageRead];
				[enc useResource:output usage:MTLResourceUsageRead | MTLResourceUsageWrite];
				[enc useResource:sizeBuffer usage:MTLResourceUsageRead];
				[enc dispatchThreadgroups:MTLSizeMake(inputs.size(), 1, 1)
				    threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
				[enc endEncoding];
				[cmd commit];
				[cmd waitUntilCompleted];
				Check(cmd.status == MTLCommandBufferStatusCompleted, "packed arithmetic dispatch");
				const auto* actual = (const uint32_t*)output.contents;
				for (size_t i = 0; i < inputs.size(); ++i)
					for (unsigned op = 0; op < std::size(forms); ++op) {
						uint32_t expected = Expected(forms[op], inputs[i]);
						if (actual[i * std::size(forms) + op] != expected) {
							fprintf(stderr,
							        "path%u op%03x clamp%u sample%zu inputs=%08x,%08x,%08x "
							        "CPU=%08x GPU=%08x\n",
							        path, forms[op].opcode, forms[op].clamp, i, inputs[i][0],
							        inputs[i][1], inputs[i][2], expected,
							        actual[i * std::size(forms) + op]);
							throw std::runtime_error("RDNA packed arithmetic mismatch");
						}
					}
				for (size_t i = output.length - 64; i < output.length; ++i)
					Check(((const uint8_t*)output.contents)[i] == 0xcd,
					      "packed arithmetic output guard");
				printf("%s: %zu CPU/GPU packed arithmetic results PASS\n",
				       path ? "SPIR-V -> Metal" : "Direct MSL", inputs.size() * std::size(forms));
			}
			return 0;
		} catch (const std::exception& e) {
			fprintf(stderr, "%s\n", e.what());
			return 1;
		}
	}
}
namespace Common {
int DbgExitHandler(const char*, int, std::string_view s) {
	throw std::runtime_error(std::string(s));
}
int DbgExitHandler(const char*, int, fmt::text_style, std::string_view s) {
	throw std::runtime_error(std::string(s));
}
int DbgExitIfHandler(const char* s, const char*, int) {
	throw std::runtime_error(s);
}
int DbgNotImplementedHandler(const char* s, const char*, int) {
	throw std::runtime_error(s);
}
void DbgExit(int) {
	throw std::runtime_error("typed IR assertion");
}
} // namespace Common
