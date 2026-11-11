// Mini benchmark: what ordering primitives cost on this GPU, Metal 3 (MTLFence, memoryBarrier) versus Metal 4 (encoder barriers, queue
// barriers, MTL4 fences, events). Companion of metal_encode_cost.mm (same kernel, same sizes, same columns).
//
// Build/run: tools/bench/run_metal4_barrier_cost.sh   (needs Xcode command line tools with the macOS 26+ SDK; does not start the game)
//
// Part 1, throughput. One trivial kernel is dispatched N times, every dispatch writes its own region of one buffer (independent work).
//   Metal 3 (queue + command buffer, setBytes):
//     3A  one encoder per dispatch, no ordering
//     3B  one encoder per dispatch + MTLFence chain                (what KyTx does today, metalOrder.h)
//     3D  one concurrent encoder, no barriers
//     3E  one concurrent encoder, memoryBarrierWithScope after every dispatch
//     3F  one concurrent encoder, memoryBarrierWithScope after every 8th dispatch
//   Metal 4 (MTL4CommandQueue/Allocator/CommandBuffer, argument table, residency set):
//     4A  one encoder per dispatch, no ordering
//     4B  one encoder per dispatch + MTLFence chain (updateFence:afterEncoderStages: / waitForFence:beforeEncoderStages:)
//     4Q  one encoder per dispatch, barrierAfterQueueStages:beforeStages: at the start of every encoder (the fence chain replacement)
//     4Q8 same, but only every 8th encoder starts with the queue barrier (groups of 8 encoders may overlap)
//     4D  one encoder, no barriers
//     4E  one encoder, barrierAfterEncoderStages:beforeEncoderStages: after every dispatch, visibility Device
//     4E0 same with visibility None (execution barrier only, no cache flush)
//     4F  one encoder, encoder barrier (Device) after every 8th dispatch
//     4F0 same with visibility None
// Columns: CPU = encode time per dispatch (us); GPU = GPU time of the whole command buffer (ms); wall = commit until the CPU saw completion (ms).
// Metal 3 runs on a buffer with hazard tracking disabled ("untracked"); 3A/3B/3F are repeated on a default (tracked) buffer, which is what
// KyTx allocates (no hazard tracking option is set in metalBuffer.mm), to show the extra driver-side ordering Metal 3 does on its own.
//
// Part 2, correctness. A chain kernel does out[tid] += 1 on 1024 slots; 400 dependent dispatches must end with 400 in every slot. Shows which
// primitive really orders dependent dispatches (and that "no barrier" does not), including the cache-visibility question (None versus Device).
//
// Part 3, cross-queue. 300 dependent steps (one tiny dispatch per command buffer, step i waits for step i-1), on one queue or alternating
// between two queues, with MTLEvent or MTLSharedEvent. This is what async compute ordering would cost. Columns: submit = CPU time per step
// to record and commit; total = wall time per step from the first commit to the last completion.
//
// Part 4, queue overlap. Two independent dependency chains on one queue or on two queues: do queues (or Metal 4 command buffers) overlap?
//
// Part 5, what Metal orders by itself (Metal 3). Dependent read-spin-write steps with NO fence and NO barrier, one encoder per step: wrong slots
// mean the steps overlapped. Buffer/texture, tracked/untracked, setBuffer or raw address + useResource (as KyTx binds guest memory), one command
// buffer or one per encoder; a concurrent encoder without barrier is the control that must fail.
//
// Part 6, same for alternating compute and render encoders (the fence chain exists mostly for these).
//
// BENCH_PARTS="15" runs only the listed parts (default: all). A whole run takes a few seconds.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
kernel void chain(device uint* out [[buffer(0)]], uint tid [[thread_position_in_grid]]) {
	out[tid] = out[tid] + 1u;
}
// Part 5 kernels: read early, spin for `iters` iterations that depend on the value read, write late. An even dispatch spins long, the odd one
// after it does not spin: if the two encoders overlapped, the odd one would read before the even one wrote and an increment would be lost.
kernel void chain_h(device uint* out [[buffer(0)]], constant uint& iters [[buffer(1)]], uint tid [[thread_position_in_grid]]) {
	const uint v = out[tid];
	uint x = v;
	for (uint i = 0; i < iters; ++i) x = x * 1664525u + 1013904223u;
	out[tid] = v + 1u + (x == 0xDEADBEEFu ? 1u : 0u);
}
// The way KyTx reaches guest memory: a raw GPU address inside a buffer, and a texture handle inside a buffer (no setBuffer/setTexture).
kernel void chain_ptr(device const ulong* table [[buffer(0)]], constant uint& iters [[buffer(1)]], uint tid [[thread_position_in_grid]]) {
	device uint* p = reinterpret_cast<device uint*>(table[0]);
	const uint v = p[tid];
	uint x = v;
	for (uint i = 0; i < iters; ++i) x = x * 1664525u + 1013904223u;
	p[tid] = v + 1u + (x == 0xDEADBEEFu ? 1u : 0u);
}
struct ChainTex { texture2d<uint, access::read_write> t; };
kernel void chain_tex(constant ChainTex& a [[buffer(0)]], constant uint& iters [[buffer(1)]], uint tid [[thread_position_in_grid]]) {
	const uint2 xy = uint2(tid & 511u, tid >> 9u);
	const uint v = a.t.read(xy).x;
	uint x = v;
	for (uint i = 0; i < iters; ++i) x = x * 1664525u + 1013904223u;
	a.t.write(uint4(v + 1u + (x == 0xDEADBEEFu ? 1u : 0u)), xy);
}
// Part 6: the same read-early / spin / write-late step as a fragment shader over a 512x512 target (one pixel per slot).
struct VOut { float4 pos [[position]]; };
vertex VOut tri_vs(uint vid [[vertex_id]]) {
	const float2 p = float2((vid << 1) & 2, vid & 2);
	VOut o;
	o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
	return o;
}
fragment void tri_fs(VOut in [[stage_in]], device uint* out [[buffer(0)]], constant uint& iters [[buffer(1)]]) {
	const uint idx = uint(in.pos.y) * 512u + uint(in.pos.x);
	const uint v = out[idx];
	uint x = v;
	for (uint i = 0; i < iters; ++i) x = x * 1664525u + 1013904223u;
	out[idx] = v + 1u + (x == 0xDEADBEEFu ? 1u : 0u);
}
fragment void tri_fs_ptr(VOut in [[stage_in]], device const ulong* table [[buffer(0)]], constant uint& iters [[buffer(1)]]) {
	device uint* out = reinterpret_cast<device uint*>(table[0]);
	const uint idx = uint(in.pos.y) * 512u + uint(in.pos.x);
	const uint v = out[idx];
	uint x = v;
	for (uint i = 0; i < iters; ++i) x = x * 1664525u + 1013904223u;
	out[idx] = v + 1u + (x == 0xDEADBEEFu ? 1u : 0u);
}
)";

using Clock = std::chrono::steady_clock;
static double Us(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::micro>(b - a).count(); }
static double Median(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

struct Result {
	double cpu_us_per_dispatch = 0;
	double gpu_ms              = 0;
	double wall_ms             = 0; // commit until completion seen on the CPU (includes scheduling latency)
};

struct Env {
	id<MTLDevice>               dev;
	id<MTLCommandQueue>         q3, q3b;
	id<MTL4CommandQueue>        q4, q4b;
	id<MTL4CommandAllocator>    alloc;
	id<MTL4ArgumentTable>       table;
	id<MTLResidencySet>         residency;
	id<MTLComputePipelineState> work, chain, chain_h, chain_ptr, chain_tex;
	id<MTLRenderPipelineState>  tri, tri_ptr;
	id<MTLTexture>              rt;
	id<MTLBuffer>               out_untracked, out_tracked, consts;
	uint32_t                    n = 0;
	size_t                      out_bytes = 0;
};

// ---------------------------------------------------------------- Part 1: Metal 3

enum class M3 { A, B, D, E, F };

static Result RunM3(Env& env, id<MTLBuffer> out, M3 mode, uint32_t groups, uint32_t iters) {
	const uint32_t n = env.n;
	id<MTLCommandBuffer> cb = [env.q3 commandBuffer];
	std::vector<id<MTLFence>> fences;
	if (mode == M3::B) {
		for (int i = 0; i < 64; ++i) fences.push_back([env.dev newFence]);
	}
	const auto t0 = Clock::now();
	id<MTLComputeCommandEncoder> shared = nil;
	if (mode == M3::D || mode == M3::E || mode == M3::F) shared = [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
	if (shared != nil) [shared setComputePipelineState:env.work];
	for (uint32_t i = 0; i < n; ++i) {
		const uint32_t base = i * groups * 64u;
		id<MTLComputeCommandEncoder> e = shared;
		if (e == nil) {
			e = [cb computeCommandEncoder];
			[e setComputePipelineState:env.work];
			if (mode == M3::B && i > 0) [e waitForFence:fences[(i - 1) % 64]];
		}
		[e setBuffer:out offset:0 atIndex:0];
		[e setBytes:&base length:4 atIndex:1];
		[e setBytes:&iters length:4 atIndex:2];
		[e dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
		if (shared == nil) {
			if (mode == M3::B) [e updateFence:fences[i % 64]];
			[e endEncoding];
		} else if (mode == M3::E || (mode == M3::F && (i % 8) == 7)) {
			[e memoryBarrierWithScope:MTLBarrierScopeBuffers];
		}
	}
	if (shared != nil) [shared endEncoding];
	const auto t1 = Clock::now();
	const auto tc = Clock::now();
	[cb commit];
	[cb waitUntilCompleted];
	const auto tw = Clock::now();
	Result r;
	r.cpu_us_per_dispatch = Us(t0, t1) / n;
	r.gpu_ms              = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
	r.wall_ms             = Us(tc, tw) / 1000.0;
	return r;
}

// ---------------------------------------------------------------- Part 1: Metal 4

enum class M4 { A, B, Q, Q8, D, E, E0, F, F0 };

static void CommitM4(id<MTL4CommandQueue> q, id<MTL4CommandBuffer> cb, double* gpu_start, double* gpu_end, dispatch_semaphore_t done) {
	MTL4CommitOptions* opts = [MTL4CommitOptions new];
	[opts addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
		if (gpu_start != nullptr) *gpu_start = fb.GPUStartTime;
		if (gpu_end != nullptr) *gpu_end = fb.GPUEndTime;
		if (fb.error != nil) std::fprintf(stderr, "MTL4 command buffer error: %s\n", fb.error.localizedDescription.UTF8String);
		dispatch_semaphore_signal(done);
	}];
	id<MTL4CommandBuffer> arr[1] = {cb};
	[q commit:arr count:1 options:opts];
}

static Result RunM4(Env& env, id<MTL4CommandBuffer> cb, const std::vector<id<MTLFence>>& fence_pool, M4 mode, uint32_t groups, uint32_t iters) {
	const uint32_t n = env.n;
	[env.alloc reset];
	uint32_t*       c        = (uint32_t*)env.consts.contents;      // entry i: base at +0 (16 bytes per entry), iters at the end
	const uint64_t  out_addr = env.out_untracked.gpuAddress;
	const uint64_t  c_addr   = env.consts.gpuAddress;
	const uint32_t  iters_at = n * 4; // in uint32 units
	c[iters_at]              = iters;
	const auto t0 = Clock::now();
	[cb beginCommandBufferWithAllocator:env.alloc];
	id<MTL4ComputeCommandEncoder> shared = nil;
	const bool one_encoder = (mode == M4::D || mode == M4::E || mode == M4::E0 || mode == M4::F || mode == M4::F0);
	const MTL4VisibilityOptions vis_device = MTL4VisibilityOptionDevice;
	if (one_encoder) {
		shared = [cb computeCommandEncoder];
		[shared setComputePipelineState:env.work];
		[shared setArgumentTable:env.table];
	}
	[env.table setAddress:out_addr atIndex:0];
	[env.table setAddress:c_addr + (uint64_t)iters_at * 4 atIndex:2];
	for (uint32_t i = 0; i < n; ++i) {
		c[i * 4]        = i * groups * 64u;
		[env.table setAddress:c_addr + (uint64_t)i * 16 atIndex:1];
		id<MTL4ComputeCommandEncoder> e = shared;
		if (e == nil) {
			e = [cb computeCommandEncoder];
			[e setComputePipelineState:env.work];
			[e setArgumentTable:env.table];
			if (mode == M4::B && i > 0) [e waitForFence:fence_pool[(i - 1) % 64] beforeEncoderStages:MTLStageDispatch];
			if (mode == M4::Q && i > 0) [e barrierAfterQueueStages:MTLStageDispatch beforeStages:MTLStageDispatch visibilityOptions:vis_device];
			if (mode == M4::Q8 && i > 0 && (i % 8) == 0) [e barrierAfterQueueStages:MTLStageDispatch beforeStages:MTLStageDispatch visibilityOptions:vis_device];
		}
		[e dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
		if (!one_encoder) {
			if (mode == M4::B) [e updateFence:fence_pool[i % 64] afterEncoderStages:MTLStageDispatch];
			[e endEncoding];
		} else if (mode == M4::E) {
			[e barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:vis_device];
		} else if (mode == M4::E0) {
			[e barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionNone];
		} else if (mode == M4::F && (i % 8) == 7) {
			[e barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:vis_device];
		} else if (mode == M4::F0 && (i % 8) == 7) {
			[e barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionNone];
		}
	}
	if (shared != nil) [shared endEncoding];
	[cb endCommandBuffer];
	const auto t1 = Clock::now();
	double gs = 0, ge = 0;
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	const auto tc = Clock::now();
	CommitM4(env.q4, cb, &gs, &ge, done);
	dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
	const auto tw = Clock::now();
	Result r;
	r.cpu_us_per_dispatch = Us(t0, t1) / n;
	r.gpu_ms              = (ge - gs) * 1000.0;
	r.wall_ms             = Us(tc, tw) / 1000.0;
	return r;
}

// ---------------------------------------------------------------- Part 2: correctness

static const uint32_t kChainSlots = 1024, kChainSteps = 400;

static uint32_t CountWrong(id<MTLBuffer> buf) {
	const uint32_t* p = (const uint32_t*)buf.contents;
	uint32_t        bad = 0;
	for (uint32_t i = 0; i < kChainSlots; ++i) bad += (p[i] != kChainSteps);
	return bad;
}

enum class Ch { M3Serial, M3Fence, M3None, M3MemBarrier, M4None, M4EncBarrierDevice, M4EncBarrierNone, M4QueueBarrierDevice, M4QueueBarrierNone, M4Fence };

static const char* ChName(Ch c) {
	switch (c) {
		case Ch::M3Serial: return "3  one serial encoder (Metal orders dispatches)";
		case Ch::M3Fence: return "3  encoder per dispatch + MTLFence";
		case Ch::M3None: return "3  concurrent encoder, NO barrier";
		case Ch::M3MemBarrier: return "3  concurrent encoder + memoryBarrier";
		case Ch::M4None: return "4  one encoder, NO barrier";
		case Ch::M4EncBarrierDevice: return "4  one encoder + encoder barrier (Device)";
		case Ch::M4EncBarrierNone: return "4  one encoder + encoder barrier (None)";
		case Ch::M4QueueBarrierDevice: return "4  encoder per dispatch + queue barrier (Device)";
		case Ch::M4QueueBarrierNone: return "4  encoder per dispatch + queue barrier (None)";
		case Ch::M4Fence: return "4  encoder per dispatch + MTLFence";
	}
	return "";
}

static uint32_t RunChain(Env& env, Ch mode) {
	id<MTLBuffer> buf = env.out_untracked;
	std::memset(buf.contents, 0, kChainSlots * 4);
	const MTLSize groups = MTLSizeMake(kChainSlots / 64, 1, 1), tg = MTLSizeMake(64, 1, 1);
	if (mode == Ch::M3Serial || mode == Ch::M3Fence || mode == Ch::M3None || mode == Ch::M3MemBarrier) {
		id<MTLCommandBuffer> cb = [env.q3 commandBuffer];
		std::vector<id<MTLFence>> fences;
		for (int i = 0; i < 64; ++i) fences.push_back([env.dev newFence]);
		id<MTLComputeCommandEncoder> shared = nil;
		if (mode == Ch::M3Serial) shared = [cb computeCommandEncoder];
		if (mode == Ch::M3None || mode == Ch::M3MemBarrier) shared = [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
		if (shared != nil) { [shared setComputePipelineState:env.chain]; [shared setBuffer:buf offset:0 atIndex:0]; }
		for (uint32_t i = 0; i < kChainSteps; ++i) {
			id<MTLComputeCommandEncoder> e = shared;
			if (e == nil) {
				e = [cb computeCommandEncoder];
				[e setComputePipelineState:env.chain];
				[e setBuffer:buf offset:0 atIndex:0];
				if (i > 0) [e waitForFence:fences[(i - 1) % 64]];
			}
			[e dispatchThreadgroups:groups threadsPerThreadgroup:tg];
			if (shared == nil) { [e updateFence:fences[i % 64]]; [e endEncoding]; }
			else if (mode == Ch::M3MemBarrier) [e memoryBarrierWithScope:MTLBarrierScopeBuffers];
		}
		if (shared != nil) [shared endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		return CountWrong(buf);
	}
	id<MTL4CommandBuffer> cb = [env.dev newCommandBuffer];
	[env.alloc reset];
	[cb beginCommandBufferWithAllocator:env.alloc];
	[env.table setAddress:buf.gpuAddress atIndex:0];
	std::vector<id<MTLFence>> fences;
	for (int i = 0; i < 64; ++i) fences.push_back([env.dev newFence]);
	id<MTL4ComputeCommandEncoder> shared = nil;
	if (mode == Ch::M4None || mode == Ch::M4EncBarrierDevice || mode == Ch::M4EncBarrierNone) {
		shared = [cb computeCommandEncoder];
		[shared setComputePipelineState:env.chain];
		[shared setArgumentTable:env.table];
	}
	for (uint32_t i = 0; i < kChainSteps; ++i) {
		id<MTL4ComputeCommandEncoder> e = shared;
		if (e == nil) {
			e = [cb computeCommandEncoder];
			[e setComputePipelineState:env.chain];
			[e setArgumentTable:env.table];
			if (i > 0 && mode == Ch::M4Fence) [e waitForFence:fences[(i - 1) % 64] beforeEncoderStages:MTLStageDispatch];
			if (i > 0 && mode == Ch::M4QueueBarrierDevice) [e barrierAfterQueueStages:MTLStageDispatch beforeStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
			if (i > 0 && mode == Ch::M4QueueBarrierNone) [e barrierAfterQueueStages:MTLStageDispatch beforeStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionNone];
		}
		[e dispatchThreadgroups:groups threadsPerThreadgroup:tg];
		if (shared == nil) {
			if (mode == Ch::M4Fence) [e updateFence:fences[i % 64] afterEncoderStages:MTLStageDispatch];
			[e endEncoding];
		} else if (mode == Ch::M4EncBarrierDevice) {
			[e barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
		} else if (mode == Ch::M4EncBarrierNone) {
			[e barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionNone];
		}
	}
	if (shared != nil) [shared endEncoding];
	[cb endCommandBuffer];
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	CommitM4(env.q4, cb, nullptr, nullptr, done);
	dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
	return CountWrong(buf);
}

// ---------------------------------------------------------------- Part 3: cross-queue events

enum class X { M3_OneQueue_NoEvent, M3_OneQueue_Event, M3_OneQueue_Shared, M3_TwoQueues_Event, M3_TwoQueues_Shared,
               M4_OneQueue_NoEvent, M4_OneQueue_Event, M4_OneQueue_Shared, M4_TwoQueues_Event, M4_TwoQueues_Shared };

static const char* XName(X x) {
	switch (x) {
		case X::M3_OneQueue_NoEvent: return "3  one queue, no event";
		case X::M3_OneQueue_Event: return "3  one queue, MTLEvent chain";
		case X::M3_OneQueue_Shared: return "3  one queue, MTLSharedEvent chain";
		case X::M3_TwoQueues_Event: return "3  two queues, MTLEvent handoff";
		case X::M3_TwoQueues_Shared: return "3  two queues, MTLSharedEvent handoff";
		case X::M4_OneQueue_NoEvent: return "4  one queue, no event";
		case X::M4_OneQueue_Event: return "4  one queue, MTLEvent chain";
		case X::M4_OneQueue_Shared: return "4  one queue, MTLSharedEvent chain";
		case X::M4_TwoQueues_Event: return "4  two queues, MTLEvent handoff";
		case X::M4_TwoQueues_Shared: return "4  two queues, MTLSharedEvent handoff";
	}
	return "";
}

static void RunCross(Env& env, X x, uint32_t steps, double* submit_us, double* total_us) {
	const bool m4         = x >= X::M4_OneQueue_NoEvent;
	const bool use_event  = !(x == X::M3_OneQueue_NoEvent || x == X::M4_OneQueue_NoEvent);
	const bool shared_ev  = (x == X::M3_OneQueue_Shared || x == X::M3_TwoQueues_Shared || x == X::M4_OneQueue_Shared || x == X::M4_TwoQueues_Shared);
	const bool two_queues = (x == X::M3_TwoQueues_Event || x == X::M3_TwoQueues_Shared || x == X::M4_TwoQueues_Event || x == X::M4_TwoQueues_Shared);
	id<MTLEvent> ev = nil;
	if (use_event) ev = shared_ev ? (id<MTLEvent>)[env.dev newSharedEvent] : [env.dev newEvent];
	id<MTLSharedEvent> done_ev = [env.dev newSharedEvent]; // completion marker, the same for every variant
	const uint32_t groups = 1, iters = 64;
	uint32_t* c = (uint32_t*)env.consts.contents;
	c[0] = 0; c[4] = iters;
	std::vector<id<MTL4CommandBuffer>> cbs4;
	id<MTLCommandBuffer> last3 = nil;
	if (m4) {
		[env.alloc reset];
		for (uint32_t i = 0; i < steps; ++i) cbs4.push_back([env.dev newCommandBuffer]);
		[env.table setAddress:env.out_untracked.gpuAddress atIndex:0];
		[env.table setAddress:env.consts.gpuAddress atIndex:1];
		[env.table setAddress:env.consts.gpuAddress + 16 atIndex:2];
	}
	const auto t0 = Clock::now();
	for (uint32_t i = 0; i < steps; ++i) {
		const uint64_t wait_value = i, signal_value = i + 1;
		if (!m4) {
			id<MTLCommandQueue> q = (two_queues && (i & 1)) ? env.q3b : env.q3;
			id<MTLCommandBuffer> cb = [q commandBuffer];
			if (use_event && i > 0) [cb encodeWaitForEvent:ev value:wait_value];
			id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
			[e setComputePipelineState:env.work];
			[e setBuffer:env.out_untracked offset:0 atIndex:0];
			uint32_t base = 0;
			[e setBytes:&base length:4 atIndex:1];
			[e setBytes:&iters length:4 atIndex:2];
			[e dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
			[e endEncoding];
			if (use_event) [cb encodeSignalEvent:ev value:signal_value];
			if (i + 1 == steps) [cb encodeSignalEvent:done_ev value:1];
			[cb commit];
			last3 = cb;
		} else {
			id<MTL4CommandQueue> q = (two_queues && (i & 1)) ? env.q4b : env.q4;
			id<MTL4CommandBuffer> cb = cbs4[i];
			[cb beginCommandBufferWithAllocator:env.alloc];
			id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
			[e setComputePipelineState:env.work];
			[e setArgumentTable:env.table];
			[e dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
			[e endEncoding];
			[cb endCommandBuffer];
			if (use_event && i > 0) [q waitForEvent:ev value:wait_value];
			id<MTL4CommandBuffer> arr[1] = {cb};
			[q commit:arr count:1];
			if (use_event) [q signalEvent:ev value:signal_value];
			if (i + 1 == steps) [q signalEvent:done_ev value:1];
		}
	}
	const auto t1 = Clock::now();
	if (!m4 && !two_queues) { [last3 waitUntilCompleted]; }
	(void)[done_ev waitUntilSignaledValue:1 timeoutMS:20000];
	const auto t2 = Clock::now();
	*submit_us = Us(t0, t1) / steps;
	*total_us  = Us(t0, t2) / steps;
}


// ---------------------------------------------------------------- Part 4: do two queues overlap independent chains?

// Two independent dependency chains of n/2 dispatches each. Metal 3: one encoder per dispatch with a MTLFence chain (KyTx today).
// Metal 4: one encoder, a barrier after every dispatch. Both chains go to the same queue (one after the other in the queue) or to two queues.
static double RunTwoChains(Env& env, bool m4, bool two_queues, uint32_t groups, uint32_t iters) {
	const uint32_t half = env.n / 2;
	double wall_ms = 0;
	if (!m4) {
		id<MTLCommandBuffer> cbs[2];
		for (int c = 0; c < 2; ++c) {
			id<MTLCommandQueue> q = (c == 1 && two_queues) ? env.q3b : env.q3;
			cbs[c] = [q commandBuffer];
			std::vector<id<MTLFence>> fences;
			for (int i = 0; i < 64; ++i) fences.push_back([env.dev newFence]);
			for (uint32_t i = 0; i < half; ++i) {
				const uint32_t base = (c * half + i) * groups * 64u;
				id<MTLComputeCommandEncoder> e = [cbs[c] computeCommandEncoder];
				[e setComputePipelineState:env.work];
				if (i > 0) [e waitForFence:fences[(i - 1) % 64]];
				[e setBuffer:env.out_untracked offset:0 atIndex:0];
				[e setBytes:&base length:4 atIndex:1];
				[e setBytes:&iters length:4 atIndex:2];
				[e dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
				[e updateFence:fences[i % 64]];
				[e endEncoding];
			}
		}
		const auto t0 = Clock::now();
		[cbs[0] commit];
		[cbs[1] commit];
		[cbs[0] waitUntilCompleted];
		[cbs[1] waitUntilCompleted];
		wall_ms = Us(t0, Clock::now()) / 1000.0;
	} else {
		[env.alloc reset];
		id<MTL4CommandBuffer> cbs[2] = {[env.dev newCommandBuffer], [env.dev newCommandBuffer]};
		uint32_t* c = (uint32_t*)env.consts.contents;
		c[env.n * 4] = iters;
		[env.table setAddress:env.out_untracked.gpuAddress atIndex:0];
		[env.table setAddress:env.consts.gpuAddress + (uint64_t)env.n * 16 atIndex:2];
		for (int k = 0; k < 2; ++k) {
			[cbs[k] beginCommandBufferWithAllocator:env.alloc];
			id<MTL4ComputeCommandEncoder> e = [cbs[k] computeCommandEncoder];
			[e setComputePipelineState:env.work];
			[e setArgumentTable:env.table];
			for (uint32_t i = 0; i < half; ++i) {
				const uint32_t idx = k * half + i;
				c[idx * 4] = idx * groups * 64u;
				[env.table setAddress:env.consts.gpuAddress + (uint64_t)idx * 16 atIndex:1];
				[e dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
				[e barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
			}
			[e endEncoding];
			[cbs[k] endCommandBuffer];
		}
		dispatch_semaphore_t done = dispatch_semaphore_create(0);
		const auto t0 = Clock::now();
		CommitM4(env.q4, cbs[0], nullptr, nullptr, done);
		CommitM4((two_queues ? env.q4b : env.q4), cbs[1], nullptr, nullptr, done);
		dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
		dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
		wall_ms = Us(t0, Clock::now()) / 1000.0;
	}
	return wall_ms;
}


// ---------------------------------------------------------------- Part 5: does Metal's own hazard tracking order separate encoders?

// 400 dependent dispatches (even ones spin long, odd ones do not), one encoder each, NO fence and NO barrier. A correct result means the driver ordered the encoders by itself.
// Variants: how the resource is reached (setBuffer, a raw GPU address in a buffer + useResource, a texture handle in a buffer + useResource,
// as KyTx does) and whether the resource is hazard tracked (the default) or untracked.
enum class Trk { SeparateCommandBuffersTracked, SeparateCommandBuffersUntracked, ControlConcurrentNoBarrier, SetBufferTracked, SetBufferUntracked, AddrUseResTracked, AddrUseResUntracked, TexHandleUseResTracked, TexHandleUseResUntracked };

static const char* TrkName(Trk t) {
	switch (t) {
		case Trk::SeparateCommandBuffersTracked: return "buffer via setBuffer, tracked, every encoder in its own command buffer";
		case Trk::SeparateCommandBuffersUntracked: return "buffer via setBuffer, untracked, every encoder in its own command buffer";
		case Trk::ControlConcurrentNoBarrier: return "CONTROL: one concurrent encoder, no barrier (must be wrong)";
		case Trk::SetBufferTracked: return "buffer via setBuffer, tracked (default)";
		case Trk::SetBufferUntracked: return "buffer via setBuffer, untracked";
		case Trk::AddrUseResTracked: return "buffer via GPU address + useResource(R|W), tracked";
		case Trk::AddrUseResUntracked: return "buffer via GPU address + useResource(R|W), untracked";
		case Trk::TexHandleUseResTracked: return "texture via handle in a buffer + useResource(R|W), tracked";
		case Trk::TexHandleUseResUntracked: return "texture via handle in a buffer + useResource(R|W), untracked";
	}
	return "";
}

static const uint32_t kTrkSlots = 262144, kTrkSteps = 100; // big grids: 4096 threadgroups, the tail of one overlaps the next if the driver lets it

static uint32_t RunTracking(Env& env, Trk mode) {
	const bool untracked = (mode == Trk::SeparateCommandBuffersUntracked || mode == Trk::SetBufferUntracked || mode == Trk::AddrUseResUntracked || mode == Trk::TexHandleUseResUntracked);
	const bool tex_mode  = (mode == Trk::TexHandleUseResTracked || mode == Trk::TexHandleUseResUntracked);
	id<MTLBuffer>  buf = nil;
	id<MTLTexture> tex = nil;
	if (!tex_mode) {
		buf = [env.dev newBufferWithLength:kTrkSlots * 4 options:MTLResourceStorageModeShared | (untracked ? MTLResourceHazardTrackingModeUntracked : MTLResourceHazardTrackingModeDefault)];
		std::memset(buf.contents, 0, kTrkSlots * 4);
	} else {
		MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:512 height:kTrkSlots / 512 mipmapped:NO];
		d.usage               = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
		d.storageMode         = MTLStorageModeShared;
		d.hazardTrackingMode  = untracked ? MTLHazardTrackingModeUntracked : MTLHazardTrackingModeTracked;
		tex                   = [env.dev newTextureWithDescriptor:d];
		std::vector<uint32_t> zero(kTrkSlots, 0);
		[tex replaceRegion:MTLRegionMake2D(0, 0, 512, kTrkSlots / 512) mipmapLevel:0 withBytes:zero.data() bytesPerRow:512 * 4];
	}
	id<MTLBuffer> table = [env.dev newBufferWithLength:16 options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
	if (tex_mode) *(uint64_t*)table.contents = tex.gpuResourceID._impl;
	else *(uint64_t*)table.contents = buf.gpuAddress;
	id<MTLCommandBuffer> cb = [env.q3 commandBuffer];
	id<MTLCommandBuffer> last_cb = cb;
	id<MTLComputeCommandEncoder> control = mode == Trk::ControlConcurrentNoBarrier ? [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent] : nil;
	for (uint32_t i = 0; i < kTrkSteps; ++i) {
		if ((mode == Trk::SeparateCommandBuffersUntracked || mode == Trk::SeparateCommandBuffersTracked) && i > 0) { [cb commit]; cb = [env.q3 commandBuffer]; last_cb = cb; }
		id<MTLComputeCommandEncoder> e = control != nil ? control : [cb computeCommandEncoder];
		if (mode == Trk::SeparateCommandBuffersTracked || mode == Trk::SeparateCommandBuffersUntracked || mode == Trk::ControlConcurrentNoBarrier || mode == Trk::SetBufferTracked || mode == Trk::SetBufferUntracked) {
			[e setComputePipelineState:env.chain_h];
			[e setBuffer:buf offset:0 atIndex:0];
		} else {
			[e setComputePipelineState:tex_mode ? env.chain_tex : env.chain_ptr];
			[e setBuffer:table offset:0 atIndex:0];
			if (tex_mode) [e useResource:tex usage:MTLResourceUsageRead | MTLResourceUsageWrite];
			else [e useResource:buf usage:MTLResourceUsageRead | MTLResourceUsageWrite];
		}
		const uint32_t iters = (i % 2 == 0) ? 4000u : 0u;
		[e setBytes:&iters length:4 atIndex:1];
		[e dispatchThreadgroups:MTLSizeMake(kTrkSlots / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
		if (control == nil) [e endEncoding];
	}
	if (control != nil) [control endEncoding];
	[cb commit];
	[last_cb waitUntilCompleted];
	std::vector<uint32_t> out(kTrkSlots, 0);
	if (!tex_mode) std::memcpy(out.data(), buf.contents, kTrkSlots * 4);
	if (tex_mode) [tex getBytes:out.data() bytesPerRow:512 * 4 fromRegion:MTLRegionMake2D(0, 0, 512, kTrkSlots / 512) mipmapLevel:0];
	uint32_t bad = 0;
	for (uint32_t v: out) bad += (v != kTrkSteps);
	return bad;
}


// ---------------------------------------------------------------- Part 6: compute and render encoders without a fence

// 100 dependent steps on one buffer, alternating compute and render encoders, no fence. `slow_render`: the render steps spin long and the compute
// steps do not (tests render -> compute); otherwise the compute steps spin long (tests compute -> render). `fenced` adds the KyTx fence chain
// as the control that must be correct.
static uint32_t RunMixed(Env& env, bool slow_render, bool untracked, bool fenced, bool raw_address) {
	id<MTLBuffer> buf = [env.dev newBufferWithLength:kTrkSlots * 4 options:MTLResourceStorageModeShared | (untracked ? MTLResourceHazardTrackingModeUntracked : MTLResourceHazardTrackingModeDefault)];
	std::memset(buf.contents, 0, kTrkSlots * 4);
	id<MTLBuffer> table = [env.dev newBufferWithLength:16 options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
	*(uint64_t*)table.contents = buf.gpuAddress;
	id<MTLCommandBuffer> cb = [env.q3 commandBuffer];
	std::vector<id<MTLFence>> fences;
	for (int i = 0; i < 64; ++i) fences.push_back([env.dev newFence]);
	for (uint32_t i = 0; i < kTrkSteps; ++i) {
		const bool render = (i % 2) == 1;
		const uint32_t iters = (render == slow_render) ? 4000u : 0u;
		if (!render) {
			id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
			if (fenced && i > 0) [e waitForFence:fences[(i - 1) % 64]];
			if (raw_address) {
				[e setComputePipelineState:env.chain_ptr];
				[e setBuffer:table offset:0 atIndex:0];
				[e useResource:buf usage:MTLResourceUsageRead | MTLResourceUsageWrite];
			} else {
				[e setComputePipelineState:env.chain_h];
				[e setBuffer:buf offset:0 atIndex:0];
			}
			[e setBytes:&iters length:4 atIndex:1];
			[e dispatchThreadgroups:MTLSizeMake(kTrkSlots / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
			if (fenced) [e updateFence:fences[i % 64]];
			[e endEncoding];
		} else {
			MTLRenderPassDescriptor* pd = [MTLRenderPassDescriptor renderPassDescriptor];
			pd.colorAttachments[0].texture     = env.rt;
			pd.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
			pd.colorAttachments[0].storeAction = MTLStoreActionDontCare;
			id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pd];
			if (fenced) [e waitForFence:fences[(i - 1) % 64] beforeStages:MTLRenderStageVertex];
			if (raw_address) {
				[e setRenderPipelineState:env.tri_ptr];
				[e setFragmentBuffer:table offset:0 atIndex:0];
				[e useResource:buf usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:MTLRenderStageFragment];
			} else {
				[e setRenderPipelineState:env.tri];
				[e setFragmentBuffer:buf offset:0 atIndex:0];
			}
			[e setFragmentBytes:&iters length:4 atIndex:1];
			[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			if (fenced) [e updateFence:fences[i % 64] afterStages:MTLRenderStageFragment];
			[e endEncoding];
		}
	}
	[cb commit];
	[cb waitUntilCompleted];
	const uint32_t* p = (const uint32_t*)buf.contents;
	uint32_t bad = 0;
	for (uint32_t i = 0; i < kTrkSlots; ++i) bad += (p[i] != kTrkSteps);
	return bad;
}

// ---------------------------------------------------------------- main

// BENCH_PARTS="15" runs only parts 1 and 5 (default: all).
static bool PartOn(int n) {
	const char* v = std::getenv("BENCH_PARTS");
	return v == nullptr || std::strchr(v, '0' + n) != nullptr;
}

int main() {
	@autoreleasepool {
		Env env;
		env.dev = MTLCreateSystemDefaultDevice();
		std::printf("device: %s, OS %s\n", env.dev.name.UTF8String, NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);
		if (![env.dev supportsFamily:MTLGPUFamilyMetal4]) {
			std::printf("MTLGPUFamilyMetal4 not supported on this device: the Metal 4 parts cannot run.\n");
			return 2;
		}
		env.q3  = [env.dev newCommandQueue];
		env.q3b = [env.dev newCommandQueue];
		env.q4  = [env.dev newMTL4CommandQueue];
		env.q4b = [env.dev newMTL4CommandQueue];
		env.alloc = [env.dev newCommandAllocator];
		NSError* err = nil;
		id<MTLLibrary> lib = [env.dev newLibraryWithSource:kSource options:nil error:&err];
		if (!lib) { std::fprintf(stderr, "compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }
		env.work  = [env.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"work"] error:&err];
		env.chain = [env.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"chain"] error:&err];
		env.chain_h = [env.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"chain_h"] error:&err];
		env.chain_ptr = [env.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"chain_ptr"] error:&err];
		env.chain_tex = [env.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"chain_tex"] error:&err];
		{
			MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
			rd.vertexFunction   = [lib newFunctionWithName:@"tri_vs"];
			rd.fragmentFunction = [lib newFunctionWithName:@"tri_fs"];
			rd.colorAttachments[0].pixelFormat    = MTLPixelFormatR8Unorm;
			rd.colorAttachments[0].writeMask      = MTLColorWriteMaskNone;
			env.tri = [env.dev newRenderPipelineStateWithDescriptor:rd error:&err];
			rd.fragmentFunction = [lib newFunctionWithName:@"tri_fs_ptr"];
			env.tri_ptr = [env.dev newRenderPipelineStateWithDescriptor:rd error:&err];
			MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:512 height:512 mipmapped:NO];
			td.usage = MTLTextureUsageRenderTarget;
			td.storageMode = MTLStorageModePrivate;
			env.rt = [env.dev newTextureWithDescriptor:td];
			if (!env.tri || !env.tri_ptr) { std::fprintf(stderr, "render pipeline failed: %s\n", err.localizedDescription.UTF8String); return 1; }
		}
		if (!env.work || !env.chain || !env.chain_h || !env.chain_ptr || !env.chain_tex) { std::fprintf(stderr, "pipeline failed: %s\n", err.localizedDescription.UTF8String); return 1; }

		env.n             = 1000;
		const uint32_t max_groups = 1024;
		env.out_bytes     = (size_t)env.n * max_groups * 64 * 4;
		env.out_untracked = [env.dev newBufferWithLength:env.out_bytes options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
		env.out_tracked   = [env.dev newBufferWithLength:env.out_bytes options:MTLResourceStorageModeShared];
		env.consts        = [env.dev newBufferWithLength:(size_t)(env.n + 4) * 16 options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
		MTLResidencySetDescriptor* rd = [MTLResidencySetDescriptor new];
		env.residency = [env.dev newResidencySetWithDescriptor:rd error:&err];
		[env.residency addAllocation:env.out_untracked];
		[env.residency addAllocation:env.consts];
		[env.residency commit];
		[env.residency requestResidency];
		[env.q4 addResidencySet:env.residency];
		[env.q4b addResidencySet:env.residency];
		MTL4ArgumentTableDescriptor* ad = [MTL4ArgumentTableDescriptor new];
		ad.maxBufferBindCount = 4;
		env.table = [env.dev newArgumentTableWithDescriptor:ad error:&err];
		if (!env.table) { std::fprintf(stderr, "argument table failed: %s\n", err.localizedDescription.UTF8String); return 1; }
		id<MTL4CommandBuffer> cb4 = [env.dev newCommandBuffer];
		std::vector<id<MTLFence>> fence_pool;
		for (int i = 0; i < 64; ++i) fence_pool.push_back([env.dev newFence]);

		const uint32_t sizes[][2] = {{4, 64}, {64, 64}, {1024, 64}};
		if (PartOn(1)) {
		std::printf("\n=== Part 1: %u independent dispatches per run, median of 4 runs (1 warm-up). Metal 3 untracked buffer unless noted ===\n", env.n);
		const struct { const char* name; bool m4; int mode; bool tracked; } modes[] = {
		    {"3A  encoder each, no order      ", false, (int)M3::A, false},
		    {"3B  encoder each + MTLFence     ", false, (int)M3::B, false},
		    {"3D  concurrent, no barrier      ", false, (int)M3::D, false},
		    {"3E  concurrent, memBarrier each ", false, (int)M3::E, false},
		    {"3F  concurrent, memBarrier /8   ", false, (int)M3::F, false},
		    {"3A' tracked buffer, no order    ", false, (int)M3::A, true},
		    {"3B' tracked buffer + MTLFence   ", false, (int)M3::B, true},
		    {"3F' tracked buffer, memBarrier/8", false, (int)M3::F, true},
		    {"4A  encoder each, no order      ", true, (int)M4::A, false},
		    {"4B  encoder each + MTLFence     ", true, (int)M4::B, false},
		    {"4Q  encoder each + queue barrier", true, (int)M4::Q, false},
		    {"4Q8 queue barrier every 8th enc ", true, (int)M4::Q8, false},
		    {"4D  one encoder, no barrier     ", true, (int)M4::D, false},
		    {"4E  enc barrier each (Device)   ", true, (int)M4::E, false},
		    {"4E0 enc barrier each (None)     ", true, (int)M4::E0, false},
		    {"4F  enc barrier /8 (Device)     ", true, (int)M4::F, false},
		    {"4F0 enc barrier /8 (None)       ", true, (int)M4::F0, false},
		};
		for (const auto& sz: sizes) {
			std::printf("\ngroups=%-5u (%u threads) iters=%u\n  %-34s %12s %12s %12s\n", sz[0], sz[0] * 64, sz[1], "mode", "CPU us/disp", "GPU ms", "wall ms");
			for (const auto& m: modes) {
				std::vector<double> cpu, gpu, wall;
				for (int k = 0; k < 5; ++k) {
					@autoreleasepool {
						const Result r = m.m4 ? RunM4(env, cb4, fence_pool, (M4)m.mode, sz[0], sz[1])
						                      : RunM3(env, m.tracked ? env.out_tracked : env.out_untracked, (M3)m.mode, sz[0], sz[1]);
						if (k > 0) { cpu.push_back(r.cpu_us_per_dispatch); gpu.push_back(r.gpu_ms); wall.push_back(r.wall_ms); }
					}
				}
				std::printf("  %-34s %12.2f %12.2f %12.2f\n", m.name, Median(cpu), Median(gpu), Median(wall));
				std::fflush(stdout);
			}
		}

		}
		if (PartOn(2)) {
		std::printf("\n=== Part 2: dependent chain, %u dispatches of out[tid] += 1 on %u slots; slots with a wrong final value (0 = correct), 5 runs ===\n", kChainSteps, kChainSlots);
		const Ch chains[] = {Ch::M3Serial, Ch::M3Fence, Ch::M3None, Ch::M3MemBarrier, Ch::M4None, Ch::M4EncBarrierDevice, Ch::M4EncBarrierNone,
		                     Ch::M4QueueBarrierDevice, Ch::M4QueueBarrierNone, Ch::M4Fence};
		for (Ch c: chains) {
			std::printf("  %-52s wrong slots per run:", ChName(c));
			for (int k = 0; k < 5; ++k) { @autoreleasepool { std::printf(" %u", RunChain(env, c)); } }
			std::printf("\n");
			std::fflush(stdout);
		}

		}
		if (PartOn(3)) {
		std::printf("\n=== Part 3: 300 dependent steps (one tiny dispatch per command buffer), median of 4 runs (1 warm-up) ===\n");
		std::printf("  %-42s %14s %14s\n", "variant", "submit us/step", "total us/step");
		const X xs[] = {X::M3_OneQueue_NoEvent, X::M3_OneQueue_Event, X::M3_OneQueue_Shared, X::M3_TwoQueues_Event, X::M3_TwoQueues_Shared,
		                X::M4_OneQueue_NoEvent, X::M4_OneQueue_Event, X::M4_OneQueue_Shared, X::M4_TwoQueues_Event, X::M4_TwoQueues_Shared};
		for (X x: xs) {
			std::vector<double> sub, tot;
			for (int k = 0; k < 5; ++k) {
				@autoreleasepool {
					double s = 0, t = 0;
					RunCross(env, x, 300, &s, &t);
					if (k > 0) { sub.push_back(s); tot.push_back(t); }
				}
			}
			std::printf("  %-42s %14.2f %14.2f\n", XName(x), Median(sub), Median(tot));
			std::fflush(stdout);
		}

		}
		if (PartOn(4)) {
		std::printf("\n=== Part 4: two independent dependency chains of %u dispatches each, wall ms until both are done, median of 4 runs (1 warm-up) ===\n", env.n / 2);
		std::printf("  %-52s %10s %10s %10s\n", "variant", "groups=4", "groups=64", "groups=1024");
		const struct { const char* name; bool m4; bool two; } tc[] = {
		    {"3  fence chain per dispatch, both chains on one queue", false, false}, {"3  fence chain per dispatch, one queue per chain", false, true},
		    {"4  encoder barrier each, both chains on one queue", true, false}, {"4  encoder barrier each, one queue per chain", true, true}};
		for (const auto& v: tc) {
			std::printf("  %-52s", v.name);
			for (const auto& sz: sizes) {
				std::vector<double> w;
				for (int k = 0; k < 5; ++k) { @autoreleasepool { const double t = RunTwoChains(env, v.m4, v.two, sz[0], sz[1]); if (k > 0) w.push_back(t); } }
				std::printf(" %10.2f", Median(w));
			}
			std::printf("\n");
			std::fflush(stdout);
		}

		}
		if (PartOn(5)) {
		std::printf("\n=== Part 5: %u dependent dispatches of %u threads, one encoder each, NO fence and NO barrier (Metal 3); wrong slots per run (0 = the driver ordered them), 5 runs ===\n", kTrkSteps, kTrkSlots);
		const Trk trks[] = {Trk::ControlConcurrentNoBarrier, Trk::SeparateCommandBuffersUntracked, Trk::SeparateCommandBuffersTracked, Trk::SetBufferTracked, Trk::SetBufferUntracked, Trk::AddrUseResTracked, Trk::AddrUseResUntracked,
		                    Trk::TexHandleUseResTracked, Trk::TexHandleUseResUntracked};
		for (Trk t: trks) {
			std::printf("  %-62s wrong slots per run:", TrkName(t));
			for (int k = 0; k < 5; ++k) { @autoreleasepool { std::printf(" %u", RunTracking(env, t)); } }
			std::printf("\n");
			std::fflush(stdout);
		}
		}

		if (PartOn(6)) {
			std::printf("\n=== Part 6: %u dependent steps on one buffer, compute and render encoders alternate (Metal 3), wrong slots per run of %u, 5 runs ===\n", kTrkSteps, kTrkSlots);
			const struct { const char* name; bool slow_render; bool untracked; bool fenced; bool raw; } mx[] = {
			    {"setBuffer: compute (slow) -> render, untracked, no fence", false, true, false, false},
			    {"setBuffer: compute (slow) -> render, tracked, no fence", false, false, false, false},
			    {"setBuffer: render (slow) -> compute, untracked, no fence", true, true, false, false},
			    {"setBuffer: render (slow) -> compute, tracked, no fence", true, false, false, false},
			    {"raw address + useResource: compute (slow) -> render, tracked, no fence", false, false, false, true},
			    {"raw address + useResource: render (slow) -> compute, tracked, no fence", true, false, false, true},
			    {"raw address + useResource: compute (slow) -> render, tracked, FENCE (control)", false, false, true, true}};
			for (const auto& v: mx) {
				std::printf("  %-80s wrong slots per run:", v.name);
				for (int k = 0; k < 5; ++k) { @autoreleasepool { std::printf(" %u", RunMixed(env, v.slow_render, v.untracked, v.fenced, v.raw)); } }
				std::printf("\n");
				std::fflush(stdout);
			}
		}
	}
	return 0;
}
