#include "graphics/host_gpu/metal/metalContext.h"
#include "graphics/host_gpu/metal/metalPipeline.h"
#include "graphics/host_gpu/metal/metalShader.h"
#include "graphics/host_gpu/metal/metalShaderCache.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/backend/metal/MetalBackend.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/Block.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/FastIrOptimizer.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace Libs::Graphics;
namespace IR           = ShaderRecompiler::IR;
namespace MetalBackend = ShaderRecompiler::MetalBackend;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "FastMetalTranslatorTests FAILED: %s\n", message);
		std::abort();
	}
}

IR::Program BuildGpuComputeTestProgram(uint64_t shader_hash, uint32_t multiplier) {
	IR::Program program;
	program.stage       = ShaderType::Compute;
	program.wave_size   = 32u;
	program.shader_hash = shader_hash;
	program.info.buffers.resize(2);
	program.bindings.descriptors.push_back({IR::DescriptorBindingKind::Buffers, {0u, 1u}});
	program.memory_info.push_back({.kind = IR::ResourceKind::Buffer, .resource = 0u});
	program.memory_info.push_back({.kind = IR::ResourceKind::Buffer, .resource = 1u});

	program.block_storage.push_back(std::make_unique<IR::Block>());
	IR::Block* block = program.block_storage.back().get();
	program.blocks.push_back(block);
	program.block_info.push_back({.id = 0u});

	auto& in_buf  = block->AppendNewInst(IR::ValueOpcode::GetBufferResource,
	                                     {IR::Value(0u), IR::Value(0u), IR::Value(0u), IR::Value(0u)}, 0u);
	auto& out_buf = block->AppendNewInst(IR::ValueOpcode::GetBufferResource,
	                                     {IR::Value(0u), IR::Value(0u), IR::Value(0u), IR::Value(0u)}, 1u);
	auto& tid     = block->AppendNewInst(
	    IR::ValueOpcode::GetBuiltin,
	    {IR::Value(static_cast<uint32_t>(IR::StageInputKind::GlobalInvocationId)), IR::Value(0u)});
	auto& offset  = block->AppendNewInst(IR::ValueOpcode::IMul32, {IR::Value(&tid), IR::Value(4u)});
	auto& loaded  = block->AppendNewInst(
	    IR::ValueOpcode::LoadBufferU32,
	    {IR::Value(&in_buf), IR::Value(0u), IR::Value(&offset), IR::Value(0u), IR::Value(true)}, 0u);
	// Add redundant bitcast + algebraic identity to confirm FastIrOptimizer cleans before MSL emit.
	auto& f  = block->AppendNewInst(IR::ValueOpcode::BitCastF32U32, {IR::Value(&loaded)});
	auto& u  = block->AppendNewInst(IR::ValueOpcode::BitCastU32F32, {IR::Value(&f)});
	auto& z  = block->AppendNewInst(IR::ValueOpcode::BitwiseXor32, {IR::Value(&u), IR::Value(&u)});
	auto& v0 = block->AppendNewInst(IR::ValueOpcode::IAdd32, {IR::Value(&u), IR::Value(&z)});
	auto& v1 = block->AppendNewInst(IR::ValueOpcode::IMul32, {IR::Value(&v0), IR::Value(multiplier)});
	auto& v2 = block->AppendNewInst(IR::ValueOpcode::IAdd32, {IR::Value(&v1), IR::Value(7u)});
	block->AppendNewInst(IR::ValueOpcode::StoreBufferU32,
	                     {IR::Value(&out_buf), IR::Value(0u), IR::Value(&offset), IR::Value(0u), IR::Value(&v2),
	                      IR::Value(true)},
	                     1u);
	return program;
}

std::vector<uint32_t> ExecuteComputeAndReadback(id<MTLDevice> device, id<MTLFunction> fn,
                                                id<MTLComputePipelineState> pipeline, uint32_t count) {
	const size_t  bytes = count * sizeof(uint32_t);
	id<MTLBuffer> in_b  = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
	id<MTLBuffer> out_b = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
	auto*         in_p  = static_cast<uint32_t*>(in_b.contents);
	for (uint32_t i = 0; i < count; ++i) {
		in_p[i] = i + 10u;
	}
	std::memset(out_b.contents, 0, bytes);

	id<MTLArgumentEncoder> enc = [fn newArgumentEncoderWithBufferIndex:0];
	id<MTLBuffer>          ab  = [device newBufferWithLength:enc.encodedLength options:MTLResourceStorageModeShared];
	[enc setArgumentBuffer:ab offset:0];
	[enc setBuffer:in_b offset:0 atIndex:0];
	[enc setBuffer:out_b offset:0 atIndex:1];

	uint32_t sizes[64] {};
	sizes[0] = static_cast<uint32_t>(bytes);
	sizes[1] = static_cast<uint32_t>(bytes);
	uint32_t push[32] {};

	id<MTLCommandQueue>         queue   = [device newCommandQueue];
	id<MTLCommandBuffer>        cmd     = [queue commandBuffer];
	id<MTLComputeCommandEncoder> cenc   = [cmd computeCommandEncoder];
	[cenc setComputePipelineState:pipeline];
	[cenc setBuffer:ab offset:0 atIndex:0];
	[cenc setBytes:push length:sizeof(push) atIndex:1];
	[cenc setBytes:sizes length:sizeof(sizes) atIndex:25];
	[cenc useResource:in_b usage:MTLResourceUsageRead];
	[cenc useResource:out_b usage:MTLResourceUsageWrite];
	[cenc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(count, 1, 1)];
	[cenc endEncoding];
	[cmd commit];
	[cmd waitUntilCompleted];
	Check(cmd.status == MTLCommandBufferStatusCompleted, "Metal GPU dispatch failed");

	std::vector<uint32_t> result(count);
	std::memcpy(result.data(), out_b.contents, bytes);
	return result;
}

} // namespace

int main() {
	@autoreleasepool {
		// Ensure KYTY_MSL_NO_DISK_CACHE / KYTY_METAL_NO_BINARY_ARCHIVE are unset in test process.
		::unsetenv("KYTY_MSL_NO_DISK_CACHE");
		::unsetenv("KYTY_METAL_NO_BINARY_ARCHIVE");

		Check(Metal::MetalContext::Instance().Initialize(), "MetalContext::Initialize failed");
		id<MTLDevice> device = Metal::MetalContext::Instance().GetDevice();
		Check(device != nil, "Metal device nil");

		char tmpl[] = "/tmp/kytx-fast-metal-test.XXXXXX";
		char* dir   = ::mkdtemp(tmpl);
		Check(dir != nullptr, "mkdtemp failed");
		const std::filesystem::path cache_root(dir);
		struct Cleanup {
			std::filesystem::path path;
			~Cleanup() {
				Metal::MetalPipelineManager::Instance().ClearCaches();
				Metal::MetalShaderManager::Instance().ClearCache();
				Metal::MetalBinaryCache::Instance().Shutdown();
				std::error_code ec;
				std::filesystem::remove_all(path, ec);
			}
		} cleanup {cache_root};

		Metal::MetalPipelineManager::Instance().ClearCaches();
		Metal::MetalShaderManager::Instance().ClearCache();
		Metal::MetalBinaryCache::Instance().Shutdown();
		Check(Metal::MetalBinaryCache::Instance().Initialize(device, cache_root.string()),
		      "MetalBinaryCache::Initialize(temp) failed");

		ShaderComputeInputInfo cs_info {};
		cs_info.threads_num[0]     = 32u;
		cs_info.threads_num[1]     = 1u;
		cs_info.threads_num[2]     = 1u;
		cs_info.host_subgroup_size = 32u;
		ShaderStageInputInfo stage_in {};
		stage_in.compute = &cs_info;

		const auto& backend = MetalBackend::GetMetalShaderBackend(MetalBackend::MetalBackendKind::FastMsl);

		constexpr uint32_t kShaders = 6;
		std::vector<uint64_t> keys;
		const auto            cold_t0 = std::chrono::steady_clock::now();
		for (uint32_t i = 0; i < kShaders; ++i) {
			const uint64_t raw_hash = 0xDEAD000000001000ull + i;
			IR::Program    prog     = BuildGpuComputeTestProgram(raw_hash, i + 2u);
			const auto     opt_st   = IR::OptimizeFast(prog);
			Check(opt_st.bitcasts_eliminated >= 1u, "expected OptimizeFast reduction in GPU shader");
			IR::EliminateDeadCode(prog.blocks);

			auto msl = backend.Emit(prog, {.input_info = stage_in});
			Check(msl.success, "FastMslBackend emit failed");
			const uint64_t key = Metal::NativeMslFunctionKey(
			    raw_hash, static_cast<uint32_t>(ShaderType::Compute), msl.msl_source, true);
			keys.push_back(key);

			auto compiled = Metal::MetalShaderManager::Instance().CompileMslToFunction(
			    msl.msl_source, "main0", static_cast<uint32_t>(ShaderType::Compute), key);
			Check(compiled.success && compiled.function != nil, "CompileMslToFunction failed");

			id<MTLComputePipelineState> pso =
			    Metal::MetalPipelineManager::Instance().GetOrCreateComputePipeline(key, compiled.function);
			Check(pso != nil, "GetOrCreateComputePipeline failed");

			// Run the first and last shader on real Apple Silicon GPU and verify outputs.
			if (i == 0 || i + 1 == kShaders) {
				const auto out = ExecuteComputeAndReadback(device, compiled.function, pso, 32u);
				for (uint32_t lane = 0; lane < 32u; ++lane) {
					const uint32_t expected = (lane + 10u) * (i + 2u) + 7u;
					Check(out[lane] == expected, "cold GPU compute result mismatch");
				}
			}
		}
		const auto cold_t1 = std::chrono::steady_clock::now();

		const auto cold_stats = Metal::MetalBinaryCache::Instance().GetStats();
		Check(cold_stats.msl_disk_writes == kShaders, "expected disk MSL writes for all shaders");
		Check(cold_stats.journal_entries_written == kShaders, "expected journal writes for all pipelines");
		Check(cold_stats.archive_adds == kShaders, "expected MTLBinaryArchive adds for all pipelines");

		const auto archive_path = Metal::MetalBinaryCache::Instance().ArchivePath();
		const auto journal_path = Metal::MetalBinaryCache::Instance().JournalPath();

		// Flush & tear down in-memory caches to simulate a fresh emulator restart.
		Metal::MetalBinaryCache::Instance().Shutdown();
		Check(std::filesystem::exists(archive_path) && std::filesystem::file_size(archive_path) > 0,
		      "serialized archive.metallib missing or empty");
		Check(std::filesystem::exists(journal_path) && std::filesystem::file_size(journal_path) > 0,
		      "pipelines.journal missing or empty");

		Metal::MetalPipelineManager::Instance().ClearCaches();
		Metal::MetalShaderManager::Instance().ClearCache();

		// Warm startup: load MTLBinaryArchive + replay pipelines.journal in parallel on worker pool.
		const auto warm_t0 = std::chrono::steady_clock::now();
		Check(Metal::MetalBinaryCache::Instance().Initialize(device, cache_root.string()),
		      "MetalBinaryCache warm re-initialize failed");
		const size_t warmed = Metal::MetalBinaryCache::Instance().PrewarmJournalSync();
		const auto   warm_t1 = std::chrono::steady_clock::now();

		const auto warm_stats = Metal::MetalBinaryCache::Instance().GetStats();
		Check(warm_stats.journal_entries_loaded == kShaders, "journal_entries_loaded mismatch");
		Check(warmed == kShaders, "PrewarmJournalSync did not restore all pipelines");
		Check(warm_stats.msl_disk_hits == kShaders, "expected msl_disk_hits == kShaders");

		// Execute warmed pipelines on Metal GPU to confirm correctness after warm reload.
		for (uint32_t i : {0u, kShaders - 1u}) {
			id<MTLFunction> fn = Metal::MetalShaderManager::Instance().GetFunction(keys[i]);
			Check(fn != nil, "GetFunction failed after warm prewarm");
			id<MTLComputePipelineState> pso =
			    Metal::MetalPipelineManager::Instance().GetOrCreateComputePipeline(keys[i], fn);
			Check(pso != nil, "GetOrCreateComputePipeline failed after warm prewarm");
			const auto out = ExecuteComputeAndReadback(device, fn, pso, 32u);
			for (uint32_t lane = 0; lane < 32u; ++lane) {
				const uint32_t expected = (lane + 10u) * (i + 2u) + 7u;
				Check(out[lane] == expected, "warm GPU compute result mismatch");
			}
		}

		const double cold_ms = std::chrono::duration<double, std::milli>(cold_t1 - cold_t0).count();
		const double warm_ms = std::chrono::duration<double, std::milli>(warm_t1 - warm_t0).count();
		std::printf("[BenchMetalGPU] cold_compile_6_pipelines=%.2f ms vs warm_archive_parallel_replay=%.2f ms (pool_workers=%zu, archive_bytes=%llu)\n",
		            cold_ms, warm_ms, Metal::MetalCompilePool::Instance().WorkerCount(),
		            static_cast<unsigned long long>(std::filesystem::file_size(archive_path)));
		std::printf("FastMetalTranslatorTests passed.\n");
		return 0;
	}
}
