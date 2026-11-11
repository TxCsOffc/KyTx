// Mini benchmark: what Apple's compute primitives cost, to choose how PS5 compute work is translated.
//
// Build/run: tools/bench/run_metal_encode_cost.sh
//
// One trivial kernel is dispatched N times. Every dispatch writes its own region of one buffer, so the dispatches are independent.
// The modes are the translation choices the emulator can make:
//   A  one encoder per dispatch, no ordering between them        (cost of the encoder alone)
//   B  one encoder per dispatch, fence chain (wait previous, update own): what KyTx does for every encoder today
//   C  one serial encoder for all dispatches                      (Metal orders them, they never overlap)
//   D  one concurrent encoder, no barriers                        (independent work overlaps; the upper bound of overlap)
//   E  one concurrent encoder, a barrier after every dispatch     (the worst case of a conservative dependency analysis)
//   F  one concurrent encoder, a barrier after every 8 dispatches (a realistic group size)
// Columns: CPU = time spent encoding on the CPU per dispatch (us); GPU = GPU time of the whole command buffer (ms).
// "groups" is the size of each dispatch in threadgroups of 64 threads: small dispatches cannot fill the GPU, which is where overlap pays.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

static NSString* const kSource = @R"(
#include <metal_stdlib>
using namespace metal;
kernel void work(device uint* out [[buffer(0)]], constant uint& base [[buffer(1)]], constant uint& iters [[buffer(2)]],
                 uint tid [[thread_position_in_grid]]) {
	uint x = tid + base;
	for (uint i = 0; i < iters; ++i) x = x * 1664525u + 1013904223u;
	out[base + tid] = x;
}
)";

struct Result {
	double cpu_us_per_dispatch;
	double gpu_ms;
};

static double Median(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

enum class Mode { A, B, C, D, E, F };

static Result Run(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLComputePipelineState> pso, id<MTLBuffer> out, Mode mode, uint32_t n,
                  uint32_t groups, uint32_t iters) {
	id<MTLCommandBuffer> cb = [queue commandBuffer];
	std::vector<id<MTLFence>> fences;
	if (mode == Mode::B) {
		for (int i = 0; i < 64; ++i) fences.push_back([dev newFence]);
	}
	const auto t0 = std::chrono::steady_clock::now();
	id<MTLComputeCommandEncoder> shared = nil;
	if (mode == Mode::C) shared = [cb computeCommandEncoder];
	if (mode == Mode::D || mode == Mode::E || mode == Mode::F) shared = [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
	if (shared != nil) [shared setComputePipelineState:pso];
	for (uint32_t i = 0; i < n; ++i) {
		const uint32_t base = i * groups * 64u;
		id<MTLComputeCommandEncoder> e = shared;
		if (e == nil) {
			e = [cb computeCommandEncoder];
			[e setComputePipelineState:pso];
			if (mode == Mode::B && i > 0) [e waitForFence:fences[(i - 1) % 64]];
		}
		[e setBuffer:out offset:0 atIndex:0];
		[e setBytes:&base length:4 atIndex:1];
		[e setBytes:&iters length:4 atIndex:2];
		[e dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
		if (shared == nil) {
			if (mode == Mode::B) [e updateFence:fences[i % 64]];
			[e endEncoding];
		} else if (mode == Mode::E || (mode == Mode::F && (i % 8) == 7)) {
			[e memoryBarrierWithScope:MTLBarrierScopeBuffers];
		}
	}
	if (shared != nil) [shared endEncoding];
	const auto t1 = std::chrono::steady_clock::now();
	[cb commit];
	[cb waitUntilCompleted];
	Result r;
	r.cpu_us_per_dispatch = std::chrono::duration<double, std::micro>(t1 - t0).count() / n;
	r.gpu_ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
	return r;
}

int main() {
	@autoreleasepool {
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		id<MTLCommandQueue> queue = [dev newCommandQueue];
		NSError* err = nil;
		id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:nil error:&err];
		if (!lib) { std::fprintf(stderr, "compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }
		id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"work"] error:&err];
		const uint32_t n = 2000;
		id<MTLBuffer> out = [dev newBufferWithLength:(size_t)n * 4096 * 64 * 4 options:MTLResourceStorageModeShared];
		std::printf("device: %s, %u independent dispatches per run, median of 5 runs\n\n", dev.name.UTF8String, n);
		const struct { const char* name; Mode mode; } modes[] = {
		    {"A one encoder each, no order    ", Mode::A}, {"B one encoder each + fences   ", Mode::B}, {"C one serial encoder         ", Mode::C},
		    {"D one concurrent, no barriers   ", Mode::D}, {"E concurrent, barrier each     ", Mode::E}, {"F concurrent, barrier per 8  ", Mode::F},
		};
		const uint32_t sizes[][2] = {{4, 64}, {64, 64}, {1024, 64}};
		for (const auto& sz: sizes) {
			std::printf("groups=%-5u (%u threads) iters=%u\n  %-32s %12s %12s\n", sz[0], sz[0] * 64, sz[1], "mode", "CPU us/disp", "GPU ms");
			for (const auto& m: modes) {
				std::vector<double> cpu, gpu;
				Run(dev, queue, pso, out, m.mode, n, sz[0], sz[1]); // warm up
				for (int k = 0; k < 5; ++k) {
					const Result r = Run(dev, queue, pso, out, m.mode, n, sz[0], sz[1]);
					cpu.push_back(r.cpu_us_per_dispatch);
					gpu.push_back(r.gpu_ms);
				}
				std::printf("  %-32s %12.2f %12.2f\n", m.name, Median(cpu), Median(gpu));
			}
			std::printf("\n");
		}
	}
	return 0;
}
