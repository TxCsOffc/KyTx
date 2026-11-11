#include "common/assert.h"
#include "graphics/shader/recompiler/backend/msl/MslEmitter.h"
#include "graphics/shader/recompiler/frontend/translate/Translate.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/SsaRewrite.h"

#import <Metal/Metal.h>
#include <array>
#include <cstring>
#include <stdexcept>
using namespace Libs::Graphics;
namespace IR = ShaderRecompiler::IR;
using IR::Value;
using IR::ValueOpcode;
static void Check(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}
namespace Log {
void WriteToConsoleAndLog(std::string_view s) {
	std::fwrite(s.data(), 1, s.size(), stderr);
}
} // namespace Log
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
	throw std::runtime_error("NGG assertion");
}
} // namespace Common
int main() {
	@autoreleasepool {
		try {
			auto device = MTLCreateSystemDefaultDevice();
			Check(device != nil, "Metal device");
			unsigned cases = 0, checked = 0;
			for (uint32_t wave: {32u, 64u})
				for (auto topology:
				     {Prospero::PrimitiveType::kTriList, Prospero::PrimitiveType::kTriStrip,
				      Prospero::PrimitiveType::kTriFan, Prospero::PrimitiveType::kLineList})
					for (bool indexed: {false, true}) {
						ShaderVertexInputInfo vi {};
						vi.logical_stage   = ShaderType::Mesh;
						auto& m            = vi.mesh;
						m.native_execution = m.ngg_vertex = true;
						m.wave_size                       = wave;
						m.host_subgroup_size              = 32;
						m.input_primitive                 = uint32_t(topology);
						m.primitives_per_group = topology == Prospero::PrimitiveType::kTriList ? 42
						                         : topology == Prospero::PrimitiveType::kLineList
						                             ? 64
						                             : 126;
						m.vertices_per_group   = m.InputVertexCount(m.primitives_per_group);
						m.max_vertices         = m.vertices_per_group;
						m.max_primitives       = m.primitives_per_group;
						m.lds_size_dwords      = 129;
						m.threads_num[0]       = 128;
						m.threads_num[1] = m.threads_num[2] = 1;
						ShaderStageInputInfo input {};
						input.vertex          = &vi;
						const uint32_t code[] = {
						    0xbefc0380u, 0xbf900009u,
						    0xbf810000u}; // s_mov m0,0; s_sendmsg GS_ALLOC_REQ; s_endpgm
						ShaderRecompiler::Decoder::Program decoded;
						ShaderRecompiler::Decoder::DecodeProgram(code, decoded);
						Check(decoded.instructions[1].opcode ==
						          ShaderRecompiler::Decoder::Opcode::S_SENDMSG,
						      "sendmsg encoding");
						auto cfg = ShaderRecompiler::CFG::BuildGraph(decoded);
						ShaderRecompiler::Frontend::TranslateOptions options {.stage =
						                                                          ShaderType::Mesh,
						                                                      .wave_size = wave,
						                                                      .user_data_count = 0,
						                                                      .input_info = input};
						auto                                         p =
						    ShaderRecompiler::Frontend::TranslateProgram(decoded, cfg, options);
						auto emit = [&](ValueOpcode op, std::initializer_list<Value> args,
						                uint64_t flags = 0) {
							return Value(&p.blocks.back()->AppendNewInst(op, args, flags));
						};
						auto local = emit(
						    ValueOpcode::GetBuiltin,
						    {Value(uint32_t(IR::StageInputKind::LocalInvocationIndex)), Value(0u)});
						auto group =
						    emit(ValueOpcode::GetBuiltin,
							     {Value(uint32_t(IR::StageInputKind::WorkgroupId)), Value(0u)});
						auto instance =
						    emit(ValueOpcode::GetBuiltin,
							     {Value(uint32_t(IR::StageInputKind::WorkgroupId)), Value(1u)});
						auto row =
						    emit(ValueOpcode::IAdd32,
							     {emit(ValueOpcode::IMul32, {instance, Value(384u)}),
							      emit(ValueOpcode::IAdd32,
							           {emit(ValueOpcode::IMul32, {group, Value(128u)}), local})});
						auto handle = emit(ValueOpcode::GetBufferResource,
						                   {Value(0u), Value(0u), Value(0u), Value(0u)});
						p.info.buffers.resize(1);
						p.bindings.descriptors.push_back({IR::DescriptorBindingKind::Buffers, {0}});
						p.memory_info.push_back({.kind = IR::ResourceKind::Buffer, .resource = 0});
						p.memory_info.push_back({.kind = IR::ResourceKind::Lds});
						auto lds_offset = emit(ValueOpcode::IMul32, {local, Value(4u)});
						auto payload =
						    emit(ValueOpcode::IAdd32,
							     {emit(ValueOpcode::IMul32, {local, Value(3u)}), Value(1u)});
						emit(ValueOpcode::WriteSharedU32, {lds_offset, payload, Value(true)}, 1);
						emit(ValueOpcode::Barrier, {});
						auto neighbor =
						    emit(ValueOpcode::BitwiseAnd32,
							     {emit(ValueOpcode::IAdd32, {local, Value(65u)}), Value(127u)});
						auto shared = emit(
						    ValueOpcode::LoadSharedU32,
						    {emit(ValueOpcode::IMul32, {neighbor, Value(4u)}), Value(true)}, 1);
						auto first = emit(ValueOpcode::IEqual32, {local, Value(0u)});
						emit(ValueOpcode::WriteSharedU32, {Value(512u), Value(0u), first}, 1);
						emit(ValueOpcode::Barrier, {});
						emit(ValueOpcode::SharedAtomicIAdd32, {Value(512u), Value(1u), Value(true)},
						     1);
						emit(ValueOpcode::Barrier, {});
						auto counter =
						    emit(ValueOpcode::LoadSharedU32, {Value(512u), Value(true)}, 1);
						const std::array<Value, 7> values = {
						    emit(ValueOpcode::GetVectorRegister, {Value(IR::VectorReg(0))}),
						    emit(ValueOpcode::GetVectorRegister, {Value(IR::VectorReg(5))}),
						    emit(ValueOpcode::GetVectorRegister, {Value(IR::VectorReg(8))}),
						    emit(ValueOpcode::GetScalarRegister, {Value(IR::ScalarReg(2))}),
						    emit(ValueOpcode::GetScalarRegister, {Value(IR::ScalarReg(3))}),
						    shared,
						    counter};
						for (unsigned c = 0; c < values.size(); ++c) {
							auto offset =
							    emit(ValueOpcode::IAdd32,
								     {emit(ValueOpcode::IMul32, {row, Value(28u)}), Value(c * 4u)});
							emit(ValueOpcode::StoreBufferU32,
							     {handle, Value(0u), offset, Value(0u), values[c], Value(true)}, 0);
						}
						IR::RewriteToSsa(p.blocks);
						IR::EliminateDeadCode(p.blocks);
						p.info.outputs = {{.kind = IR::StageOutputKind::Position}};
						auto result    = ShaderRecompiler::Msl::EmitProgram(p, input);
						for (auto& reason: result.unsupported)
							std::fprintf(stderr, "%s\n", reason.c_str());
						Check(result.success, "NGG frontend emission");
						NSError* e                      = nil;
						auto     opts                   = [MTLCompileOptions new];
						opts.mathMode                   = MTLMathModeSafe;
						opts.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
						auto lib                        = [device
						    newLibraryWithSource:[NSString
						                             stringWithUTF8String:result.source.c_str()]
						                 options:opts
						                   error:&e];
						if (!lib)
							std::fprintf(stderr, "%s\n%s", e.localizedDescription.UTF8String,
							             result.source.c_str());
						Check(lib != nil, "NGG frontend compile");
						auto fs                            = [device
						    newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace "
						                         @"metal;fragment float4 color(){return float4(1);}"
						                 options:nil
						                   error:&e];
						auto pd                            = [MTLMeshRenderPipelineDescriptor new];
						pd.meshFunction                    = [lib newFunctionWithName:@"main0"];
						pd.fragmentFunction                = [fs newFunctionWithName:@"color"];
						pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
						auto pipeline =
						    [device newRenderPipelineStateWithMeshDescriptor:pd
						                                             options:MTLPipelineOptionNone
						                                          reflection:nil
						                                               error:&e];
						if (!pipeline)
							std::fprintf(stderr, "%s\n", e.localizedDescription.UTF8String);
						Check(pipeline != nil, "NGG frontend pipeline");
						std::array<uint32_t, 1024> indices {};
						for (unsigned i = 0; i < indices.size(); ++i)
							indices[i] = 1000u - i;
						auto ib     = [device newBufferWithBytes:indices.data()
						                                  length:sizeof(indices)
						                                 options:MTLResourceStorageModeShared];
						auto output = [device newBufferWithLength:3 * 2 * 128 * 28
						                                  options:MTLResourceStorageModeShared];
						std::memset(output.contents, 0xcd, output.length);
						uint64_t ab[64] = {output.gpuAddress};
						uint32_t sizes[ShaderRecompiler::Msl::SizeConstantWordCount] {};
						sizes[0]               = uint32_t(output.length);
						const uint32_t count   = m.vertices_per_group + 5;
						const uint32_t draw[8] = {count, 0xfffffff8u, 7, indexed ? 4u : 0u,
						                          0,     0,           0, 0};
						auto           td      = [MTLTextureDescriptor
						    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
						                                 width:1
						                                height:1
						                             mipmapped:NO];
						td.usage               = MTLTextureUsageRenderTarget;
						auto color             = [device newTextureWithDescriptor:td];
						auto pass              = [MTLRenderPassDescriptor new];
						pass.colorAttachments[0].texture     = color;
						pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
						pass.colorAttachments[0].storeAction = MTLStoreActionStore;
						auto cb  = [[device newCommandQueue] commandBuffer];
						auto enc = [cb renderCommandEncoderWithDescriptor:pass];
						[enc setRenderPipelineState:pipeline];
						[enc setMeshBytes:ab length:sizeof(ab) atIndex:0];
						[enc setMeshBytes:sizes length:128 atIndex:1];
						[enc setMeshBytes:sizes length:sizeof(sizes) atIndex:25];
						[enc setMeshBytes:draw length:sizeof(draw) atIndex:26];
						[enc setMeshBuffer:ib offset:0 atIndex:24];
						[enc useResource:output
						           usage:MTLResourceUsageWrite
						          stages:MTLRenderStageMesh];
						[enc drawMeshThreadgroups:MTLSizeMake(3, 2, 1)
						    threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
						      threadsPerMeshThreadgroup:MTLSizeMake(wave == 64 ? 64 : 128, 1, 1)];
						[enc endEncoding];
						[cb commit];
						[cb waitUntilCompleted];
						if (cb.error)
							std::fprintf(stderr, "%s\n", cb.error.localizedDescription.UTF8String);
						Check(cb.status == MTLCommandBufferStatusCompleted, "NGG frontend GPU");
						const uint32_t* actual = (const uint32_t*)output.contents;
						for (unsigned instance_id = 0; instance_id < 2; ++instance_id)
							for (unsigned g = 0; g < 3; ++g)
								for (unsigned l = 0; l < 128; ++l) {
									const unsigned step =
									    topology == Prospero::PrimitiveType::kTriList    ? 3
										: topology == Prospero::PrimitiveType::kLineList ? 2
										                                                 : 1;
									const unsigned size =
									    topology == Prospero::PrimitiveType::kLineList ? 2 : 3;
									const unsigned chunk = g * m.primitives_per_group * step;
									const unsigned nv =
									    chunk < count
									        ? std::min(count - chunk, m.vertices_per_group)
									        : 0;
									const unsigned np    = nv < size ? 0 : (nv - size) / step + 1;
									const unsigned wbase = (l / wave) * wave;
									const unsigned vc = nv > wbase ? std::min(nv - wbase, wave) : 0;
									const unsigned pc = np > wbase ? std::min(np - wbase, wave) : 0;
									const uint32_t s3 =
									    ((128 / wave) << 28) | ((l / wave) << 24) | (pc << 8) | vc;
									unsigned a = l * step, b = a + 1, c = size == 3 ? a + 2 : 0;
									if (topology == Prospero::PrimitiveType::kTriStrip &&
									    ((g * m.primitives_per_group + l) & 1))
										std::swap(b, c);
									if (topology == Prospero::PrimitiveType::kTriFan) a = 0;
									const uint32_t packed = a | (b << 10) | (c << 20);
									unsigned       v =
									    topology == Prospero::PrimitiveType::kTriFan && l == 0
									        ? 0
									        : chunk + l;
									uint32_t vid = indexed ? (l < nv ? indices[v] : 0) : v;
									vid += draw[1];
									const std::array<uint32_t, 7> expected = {
									    packed,
									    vid,
									    7 + instance_id,
									    (nv << 12) | (np << 22) |
									        ((instance_id * ((((count - size) / step + 1) +
											                  m.primitives_per_group - 1) /
											                 m.primitives_per_group) +
											  g) &
											 0xfff),
									    s3,
									    ((l + 65) % 128) * 3 + 1,
									    128};
									for (unsigned c = 0; c < 7; ++c) {
										unsigned row = (instance_id * 384 + g * 128 + l) * 7 + c;
										if (actual[row] != expected[c]) {
											std::fprintf(
											    stderr,
											    "NGG mismatch wave=%u topology=%u indexed=%d "
											    "group=%u lane=%u field=%u got=%08x wanted=%08x\n",
											    wave, unsigned(topology), indexed, g, l, c,
											    actual[row], expected[c]);
											throw std::runtime_error("NGG register ABI");
										}
										++checked;
									}
								}
						++cases;
					}
			std::printf("Native NGG frontend: %u GPU configurations, %u register values PASS\n",
			            cases, checked);
			return 0;
		} catch (const std::exception& e) {
			std::fprintf(stderr, "%s\n", e.what());
			return 1;
		}
	}
}
