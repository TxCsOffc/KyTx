#include "common/assert.h"
#include "graphics/host_gpu/metal/metalShaderAbi.h"
#include "graphics/shader/recompiler/backend/msl/MslEmitter.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvOptimizer.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"

#import <Metal/Metal.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <spirv_cross/spirv_msl.hpp>
#include <stdexcept>
using namespace Libs::Graphics;
namespace IR = ShaderRecompiler::IR;
using IR::Value;
using IR::ValueOpcode;
static void Check(bool value, const char* message) {
	if (!value) throw std::runtime_error(message);
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
	throw std::runtime_error("vertex IR assertion");
}
} // namespace Common
struct Fixture {
	IR::Program program;
	Fixture(bool packed) {
		program.stage     = ShaderType::Vertex;
		program.wave_size = 32;
		program.info.buffers.resize(1);
		program.bindings.descriptors.push_back({IR::DescriptorBindingKind::Buffers, {0}});
		program.block_storage.push_back(std::make_unique<IR::Block>());
		program.blocks.push_back(program.block_storage.back().get());
		program.block_info.push_back({.id = 0});
		auto emit = [&](ValueOpcode op, std::initializer_list<Value> a, uint64_t flags = 0) {
			return Value(&program.blocks[0]->AppendNewInst(op, a, flags));
		};
		auto vertex = emit(ValueOpcode::GetBuiltin,
		                   {Value(uint32_t(IR::StageInputKind::VertexIndex)), Value(0u)});
		auto offset = emit(ValueOpcode::IMul32, {vertex, Value(16u)});
		auto buffer =
		    emit(ValueOpcode::GetBufferResource, {Value(0u), Value(0u), Value(0u), Value(0u)});
		program.memory_info.push_back({.kind = IR::ResourceKind::Buffer, .resource = 0});
		Value parts[4];
		for (unsigned i = 0; i < 4; ++i) {
			auto laneOffset = emit(ValueOpcode::IAdd32, {offset, Value(i * 4u)});
			parts[i]        = emit(ValueOpcode::LoadBufferU32,
			                       {buffer, Value(0u), laneOffset, Value(0u), Value(true)}, 0);
		}
		auto position =
		    emit(ValueOpcode::CompositeConstructU32x4, {parts[0], parts[1], parts[2], parts[3]});
		program.export_info.push_back({.kind  = IR::ExportTargetKind::Position,
		                               .index = 0,
		                               .en    = 15,
		                               .done  = true,
		                               .compr = packed});
		emit(ValueOpcode::SetAttribute, {position, Value(true)}, 0);
	}
};
static id<MTLFunction> Compile(id<MTLDevice> device, const std::string& code, const char* entry) {
	NSError*           e               = nil;
	MTLCompileOptions* options         = [MTLCompileOptions new];
	options.mathMode                   = MTLMathModeSafe;
	options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
	id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:code.c_str()]
	                                          options:options
	                                            error:&e];
	if (!lib) std::fprintf(stderr, "%s\n", e.localizedDescription.UTF8String);
	Check(lib != nil, "clip Metal compilation");
	return [lib newFunctionWithName:[NSString stringWithUTF8String:entry]];
}
struct Shot {
	std::array<uint8_t, 64 * 64 * 4> color;
	std::array<float, 64 * 64>       depth;
};
static Shot Draw(id<MTLDevice> device, id<MTLFunction> vertex, float z, float w, double near,
                 double far, bool packed) {
	NSError* error    = nil;
	auto     fragment = Compile(
	    device,
	    "#include <metal_stdlib>\nusing namespace metal;fragment float4 fs(){return float4(1);}",
	    "fs");
	MTLRenderPipelineDescriptor* pd     = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction                   = vertex;
	pd.fragmentFunction                 = fragment;
	pd.colorAttachments[0].pixelFormat  = MTLPixelFormatRGBA8Unorm;
	pd.depthAttachmentPixelFormat       = MTLPixelFormatDepth32Float;
	id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd
	                                                                             error:&error];
	if (!pipeline) std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
	Check(pipeline != nil, "clip pipeline");
	const float positions[] = {-.75f, -.7f, z, w, .65f, -.45f, z, w, -.35f, .8f, z, w};
	uint32_t    inputWords[12] {};
	std::memcpy(inputWords, positions, sizeof(positions));
	if (packed)
		for (unsigned vertex = 0; vertex < 3; ++vertex) {
			auto pack = [](float a, float b) {
				return uint32_t(std::bit_cast<uint16_t>(_Float16(a))) |
				       (uint32_t(std::bit_cast<uint16_t>(_Float16(b))) << 16);
			};
			inputWords[vertex * 4]     = pack(positions[vertex * 4], positions[vertex * 4 + 1]);
			inputWords[vertex * 4 + 1] = pack(z, w);
			inputWords[vertex * 4 + 2] = inputWords[vertex * 4 + 3] = 0;
		}
	id<MTLBuffer> positionsBuffer = [device newBufferWithBytes:inputWords
	                                                    length:sizeof(inputWords)
	                                                   options:MTLResourceStorageModeShared];
	id<MTLArgumentEncoder> args   = [vertex newArgumentEncoderWithBufferIndex:0];
	id<MTLBuffer>          ab     = [device newBufferWithLength:args.encodedLength
	                                                    options:MTLResourceStorageModeShared];
	[args setArgumentBuffer:ab offset:0];
	const bool reference = [vertex.name isEqualToString:@"reference_main"];
	[args setBuffer:positionsBuffer offset:0 atIndex:reference ? 1 : 0];
	MTLTextureDescriptor* td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	                                                       width:64
	                                                      height:64
	                                                   mipmapped:NO];
	td.storageMode                       = MTLStorageModeShared;
	td.usage                             = MTLTextureUsageRenderTarget;
	id<MTLTexture> color                 = [device newTextureWithDescriptor:td];
	td.pixelFormat                       = MTLPixelFormatDepth32Float;
	td.storageMode                       = MTLStorageModePrivate;
	id<MTLTexture>           depth       = [device newTextureWithDescriptor:td];
	MTLRenderPassDescriptor* pass        = [MTLRenderPassDescriptor new];
	pass.colorAttachments[0].texture     = color;
	pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.depthAttachment.texture         = depth;
	pass.depthAttachment.loadAction      = MTLLoadActionClear;
	pass.depthAttachment.storeAction     = MTLStoreActionStore;
	pass.depthAttachment.clearDepth      = 1;
	MTLDepthStencilDescriptor* ds        = [MTLDepthStencilDescriptor new];
	ds.depthCompareFunction              = MTLCompareFunctionAlways;
	ds.depthWriteEnabled                 = YES;
	id<MTLCommandQueue>         q        = [device newCommandQueue];
	id<MTLCommandBuffer>        cb       = [q commandBuffer];
	id<MTLRenderCommandEncoder> enc      = [cb renderCommandEncoderWithDescriptor:pass];
	[enc setRenderPipelineState:pipeline];
	[enc setDepthStencilState:[device newDepthStencilStateWithDescriptor:ds]];
	[enc setVertexBuffer:ab offset:0 atIndex:0];
	uint32_t push[32] {}, sizes[288] {};
	sizes[0] = sizes[1]          = sizeof(positions);
	id<MTLBuffer> referenceSizes = [device newBufferWithBytes:sizes
	                                                   length:sizeof(sizes)
	                                                  options:MTLResourceStorageModeShared];
	if (reference) {
		[args setBuffer:referenceSizes offset:0 atIndex:0];
		[enc useResource:referenceSizes usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
	}
	[enc setVertexBytes:push length:sizeof(push) atIndex:1];
	[enc setVertexBytes:sizes length:sizeof(sizes) atIndex:25];
	[enc setVertexBytes:push length:16 atIndex:26];
	[enc useResource:positionsBuffer usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
	[enc setViewport:MTLViewport {0, 0, 64, 64, near, far}];
	[enc setCullMode:MTLCullModeNone];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[enc endEncoding];
	id<MTLBuffer>             bytes = [device newBufferWithLength:64 * 256
	                                                      options:MTLResourceStorageModeShared];
	id<MTLBlitCommandEncoder> blit  = [cb blitCommandEncoder];
	[blit copyFromTexture:depth
	                 sourceSlice:0
	                 sourceLevel:0
	                sourceOrigin:MTLOriginMake(0, 0, 0)
	                  sourceSize:MTLSizeMake(64, 64, 1)
	                    toBuffer:bytes
	           destinationOffset:0
	      destinationBytesPerRow:256
	    destinationBytesPerImage:64 * 256];
	[blit endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	if (cb.status != MTLCommandBufferStatusCompleted)
		std::fprintf(stderr, "%s\n", cb.error.localizedDescription.UTF8String);
	Check(cb.status == MTLCommandBufferStatusCompleted, "clip GPU render");
	Shot shot {};
	[color getBytes:shot.color.data()
	    bytesPerRow:256
	     fromRegion:MTLRegionMake2D(0, 0, 64, 64)
	    mipmapLevel:0];
	std::memcpy(shot.depth.data(), bytes.contents, sizeof(shot.depth));
	return shot;
}
int main() {
	@autoreleasepool {
		try {
			id<MTLDevice> device = MTLCreateSystemDefaultDevice();
			Check(device != nil, "Metal unavailable");
			size_t   checked = 0, nativeWrong = 0, referenceWrong = 0;
			unsigned cases = 0;
			for (bool packed: {false, true})
				for (bool remap: {false, true}) {
					Fixture               fixture(packed);
					ShaderVertexInputInfo vi {};
					vi.clip_space.remap_z = remap;
					ShaderStageInputInfo input {};
					input.vertex                 = &vi;
					auto& p                      = fixture.program;
					p.resource_tracking_complete = true;
					p.srt_plan_complete          = true;
					IR::CollectShaderInfo(p, input);
					IR::AllocateBindings(p);
					ShaderVertexInputInfo nativeVertex = vi;
					if (std::getenv("KYTY_VERTEX_CLIP_LEGACY"))
						nativeVertex.clip_space.remap_z = false;
					ShaderStageInputInfo nativeInput = input;
					nativeInput.vertex               = &nativeVertex;
					auto nativeCode = ShaderRecompiler::Msl::EmitProgram(p, nativeInput);
					Check(nativeCode.success, "native clip emitter rejected fixture");
					std::ofstream(std::string("/tmp/kytx_clip_native_") + (remap ? "1" : "0") +
					              ".metal")
					    << nativeCode.source;
					auto native = Compile(device, nativeCode.source, "main0");
					auto words  = ShaderRecompiler::Spirv::EmitProgram(p, input, true);
					Check(!words.empty(), "reference SPIR-V empty");
					std::string diagnostics;
					Check(
					    ShaderRecompiler::Spirv::Optimize(
					        words, Config::ShaderOptimizationType::Performance, diagnostics, true),
					    "SPIR-V optimization/validation");
					spirv_cross::CompilerMSL compiler(words);
					auto                     entry = compiler.get_entry_points_and_stages()[0];
					compiler.rename_entry_point(entry.name, "reference_main",
					                            entry.execution_model);
					spirv_cross::CompilerMSL::Options options;
					options.set_msl_version(3, 2);
					options.platform                  = spirv_cross::CompilerMSL::Options::macOS;
					options.argument_buffers          = true;
					options.enable_decoration_binding = true;
					options.buffer_size_buffer_index  = 25;
					compiler.set_msl_options(options);
					auto abi = Metal::ComputeSpvcAbiLayout(p.bindings, p.stage);
					for (const auto& mapping: abi.entries) {
						spirv_cross::MSLResourceBinding binding {};
						binding.stage      = entry.execution_model;
						binding.desc_set   = 0;
						binding.binding    = mapping.binding;
						binding.count      = mapping.count;
						binding.msl_buffer = mapping.base_id;
						compiler.add_msl_resource_binding(binding);
					}
					auto referenceSource = compiler.compile();
					std::ofstream(std::string("/tmp/kytx_clip_reference_") + (remap ? "1" : "0") +
					              ".metal")
					    << referenceSource;
					auto reference = Compile(device, referenceSource, "reference_main");
					for (float z: {-1.5f, -.75f, 0.f, .75f, 1.5f})
						for (float w: {.5f, 1.f, 2.f})
							for (bool reverse: {false, true}) {
								auto         previousNative    = nativeWrong,
								             previousReference = referenceWrong;
								double       near = reverse ? .9 : .1, far = reverse ? .2 : .8;
								auto         a = Draw(device, native, z, w, near, far, packed),
								             b = Draw(device, reference, z, w, near, far, packed);
								const double clipZ       = remap ? (double(z) + w) * .5 : z;
								const bool   valid       = clipZ >= 0 && clipZ <= w;
								const float  wantedDepth = float(near + (far - near) * clipZ / w);
								auto         quantize    = [packed](float f) {
									return packed ? double(_Float16(f)) : double(f);
								};
								const double px[] = {32 + 32 * quantize(-.75f) / w,
								                     32 + 32 * quantize(.65f) / w,
								                     32 + 32 * quantize(-.35f) / w},
								             py[] = {32 - 32 * quantize(-.7f) / w,
											         32 - 32 * quantize(-.45f) / w,
											         32 - 32 * quantize(.8f) / w};
								auto cross        = [](double ax, double ay, double bx, double by) {
									return ax * by - ay * bx;
								};
								double area = cross(px[1] - px[0], py[1] - py[0], px[2] - px[0],
								                    py[2] - py[0]);
								for (unsigned y = 0; y < 64; ++y)
									for (unsigned x = 0; x < 64; ++x) {
										double bx = cross(x + .5 - px[0], y + .5 - py[0],
										                  px[2] - px[0], py[2] - py[0]) /
										            area,
										       cx = cross(px[1] - px[0], py[1] - py[0],
											              x + .5 - px[0], y + .5 - py[0]) /
											        area,
										       ax = 1 - bx - cx;
										if (std::min({std::fabs(ax), std::fabs(bx),
										              std::fabs(cx)}) < .02)
											continue;
										bool wanted = valid && ax > 0 && bx > 0 && cx > 0;
										auto check  = [&](const Shot& shot) {
											bool drawn = shot.color[(y * 64 + x) * 4] != 0;
											return drawn != wanted ||
											       (wanted && std::fabs(shot.depth[y * 64 + x] -
											                            wantedDepth) > 1e-6);
										};
										nativeWrong += check(a);
										referenceWrong += check(b);
										++checked;
									}
								if (nativeWrong != previousNative ||
								    referenceWrong != previousReference)
									std::printf("remap=%d z=%g w=%g reverse=%d native=%zu "
									            "reference=%zu depthCenter=%g/%g\n",
									            remap, z, w, reverse, nativeWrong - previousNative,
									            referenceWrong - previousReference,
									            a.depth[32 * 64 + 32], b.depth[32 * 64 + 32]);
								++cases;
							}
				}
			std::printf("Vertex clip Z: %u same-IR native/SPIR-V GPU cases, %zu pixels, native "
			            "mismatches=%zu reference=%zu\n",
			            cases, checked, nativeWrong, referenceWrong);
			Check(referenceWrong == 0, "reference disagrees with independent clip/depth oracle");
			Check(nativeWrong == 0, "native clip Z disagrees with oracle");
			return 0;
		} catch (const std::exception& e) {
			std::fprintf(stderr, "%s\n", e.what());
			return 1;
		}
	}
}
