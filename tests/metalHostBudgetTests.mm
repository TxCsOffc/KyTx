#include "graphics/host_gpu/metal/metalHostBudget.h"
#include <cstring>
#include <atomic>
#include <vector>

namespace HB = Libs::Graphics::Metal::HostBudget;

int main(int argc, char** argv) { @autoreleasepool {
	if (argc == 2 && std::strcmp(argv[1], "--policy-only") == 0) {
		(void)HB::Get();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--memory-probe") == 0) {
		(void)HB::Get();
		// CPU-only negative: commit a small amount beyond a test-only low budget.
		std::vector<unsigned char> data(64u << 20, 0x37);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		HB::CheckMemory(HB::Get());
		return data[10] == 0x37 ? 1 : 2; // reaching here means the guard failed
	}
	if (argc == 2 && std::strcmp(argv[1], "--stall-probe") == 0) {
		// Exercise the independent monitor without submitting any GPU work.
		auto& state = HB::Get();
		state.pending_since.store(HB::NowNanoseconds()-3'000'000'000ull,std::memory_order_release);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		return 1;
	}
	auto device = MTLCreateSystemDefaultDevice();
	auto queue = [device newCommandQueue];
	NSError* error = nil;
	auto lib = [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;kernel void step(device uint* p [[buffer(0)]],constant uint& x [[buffer(1)]]){p[0]=p[0]*1664525u+1013904223u+x;}"
	                              options:nil error:&error];
	auto pipeline = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"step"] error:&error];
	if (!pipeline) { std::fprintf(stderr, "compile: %s\n", error.localizedDescription.UTF8String); return 2; }
	auto buffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
	std::memset(buffer.contents, 0, buffer.length);
	uint32_t expected = 0;
	double gpu_seconds = 0;
	struct Interval { double start, end; };
	std::vector<Interval> intervals;
	const auto begin = std::chrono::steady_clock::now();
	for (uint32_t i = 0; i < 40; ++i) {
		auto command = [queue commandBuffer];
		auto encoder = [command computeCommandEncoder];
		[encoder setComputePipelineState:pipeline];
		[encoder setBuffer:buffer offset:0 atIndex:0];
		[encoder setBytes:&i length:sizeof(i) atIndex:1];
		[encoder dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
		[encoder endEncoding];
		HB::Commit(command);
		[command waitUntilCompleted];
		gpu_seconds += command.GPUEndTime - command.GPUStartTime;
		intervals.push_back({command.GPUStartTime,command.GPUEndTime});
		expected = expected * 1664525u + 1013904223u + i;
		if (static_cast<uint32_t*>(buffer.contents)[0] != expected) return 1;
	}
	// Competing host threads and independent queues still share one budget.
	std::atomic<bool> failed {false};
	std::mutex results;
	const auto concurrent = [&](uint32_t stream) { @autoreleasepool {
		auto q = [device newCommandQueue];
		auto data = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
		std::memset(data.contents,0,data.length);
		uint32_t value = 0;
		for (uint32_t i=0;i<8;++i) {
			uint32_t x = stream * 100 + i;
			auto c = [q commandBuffer]; auto e = [c computeCommandEncoder];
			[e setComputePipelineState:pipeline]; [e setBuffer:data offset:0 atIndex:0];
			[e setBytes:&x length:sizeof(x) atIndex:1];
			[e dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)]; [e endEncoding];
			HB::Commit(c); [c waitUntilCompleted];
			value=value*1664525u+1013904223u+x;
			if (static_cast<uint32_t*>(data.contents)[0]!=value) failed=true;
			std::lock_guard lock(results);
			gpu_seconds+=c.GPUEndTime-c.GPUStartTime;
			intervals.push_back({c.GPUStartTime,c.GPUEndTime});
		}
	}};
	std::thread first(concurrent,1), second(concurrent,2); first.join(); second.join();
	if (failed) return 1;
	const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
	auto& state = HB::Get();
	if (static_cast<unsigned __int128>(state.idle_ns) * state.percent <
	    static_cast<unsigned __int128>(state.wall_busy_ns) * (100u - state.percent)) return 1;
	if (gpu_seconds > wall * (state.percent / 100.0)) return 1;
	std::sort(intervals.begin(),intervals.end(),[](auto a,auto b){return a.start<b.start;});
	for (size_t i=1;i<intervals.size();++i) {
		const double needed=(intervals[i-1].end-intervals[i-1].start)*(100-state.percent)/state.percent;
		if (intervals[i].start-intervals[i-1].end+1e-9<needed) return 1;
	}
	std::printf("GPU budget: 56 exact writes, 3 queues/3 threads, policy=%u%%, actual GPU/wall=%.2f%%, all idle intervals honoured PASS\n",
	            state.percent, 100.0 * gpu_seconds / wall);
	std::fflush(stdout);
	if (argc == 2 && std::strcmp(argv[1], "--quick-exit") == 0) std::quick_exit(0);
	HB::Finish();
	if (const char* marker = std::getenv("KYTY_HOST_FAILURE_FILE"); marker && access(marker,F_OK) == 0) return 1;
	return 0;
}}
