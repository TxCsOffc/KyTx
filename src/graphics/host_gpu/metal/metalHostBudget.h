#pragma once

#include "graphics/host_gpu/metal/metalDutyCycle.h"

#import <Metal/Metal.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <string>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace Libs::Graphics::Metal::HostBudget {

[[noreturn]] inline void Stop(const char* reason) {
	if (const char* path = std::getenv("KYTY_HOST_FAILURE_FILE")) {
		if (FILE* file = std::fopen(path,"w")) {
			std::fprintf(file,"GPU launches stopped: %s\n",reason);
			std::fclose(file);
		}
	}
	std::fflush(stderr);
	std::_Exit(75);
}

inline uint64_t ParseBudget(const char* name, uint64_t fallback, uint64_t maximum) {
	const char* value = std::getenv(name);
	if (!value) return fallback;
	char* end = nullptr;
	const uint64_t parsed = std::strtoull(value, &end, 10);
	if (value == end || *end || parsed == 0 || parsed > maximum) {
		std::fprintf(stderr, "[HostBudget] invalid %s: expected 1..%llu\n", name,
		             static_cast<unsigned long long>(maximum));
		Stop("invalid resource budget");
	}
	return parsed;
}

struct State {
	std::mutex submission;
	uint32_t percent = 90;
	uint64_t memory_bytes = 0;
	uint64_t submitted = 0, wall_busy_ns = 0, idle_ns = 0;
	std::atomic<uint64_t> pending_since {0};
	std::atomic<bool> closed {false};
	std::thread monitor;
	bool owns_marker = false;
	std::string marker;
};

inline uint64_t NowNanoseconds() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void CheckMemory(const State& state) {
	task_vm_info_data_t info {};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
		std::fprintf(stderr, "[HostBudget] cannot read process footprint; stopping\n");
		Stop("process footprint unavailable");
	}
	if (info.phys_footprint >= state.memory_bytes) {
		std::fprintf(stderr, "[HostBudget] footprint=%llu MiB reached budget=%llu MiB; stopping\n",
		             static_cast<unsigned long long>(info.phys_footprint >> 20),
		             static_cast<unsigned long long>(state.memory_bytes >> 20));
		Stop("process memory budget reached");
	}
}

inline void Finish();

inline State& Get() {
	static State* state = [] {
		if (const char* path = std::getenv("KYTY_HOST_FAILURE_FILE"); path && access(path,F_OK) == 0) {
			std::fprintf(stderr,"[HostBudget] previous failure blocks GPU work: %s\n",path);
			std::_Exit(75);
		}
		auto* s = new State;
		if (const char* path = std::getenv("KYTY_HOST_FAILURE_FILE")) s->marker = path;
		s->percent = static_cast<uint32_t>(ParseBudget("KYTY_GPU_BUDGET_PERCENT", 90, 90));
		uint64_t physical = 0;
		size_t size = sizeof(physical);
		if (sysctlbyname("hw.memsize", &physical, &size, nullptr, 0) != 0 || physical == 0) {
			std::fprintf(stderr, "[HostBudget] cannot read physical memory; stopping\n");
			Stop("physical memory unavailable");
		}
		const uint64_t requested = ParseBudget("KYTY_HOST_MEMORY_MIB", 40u * 1024u, 40u * 1024u) << 20;
		s->memory_bytes = std::min(requested, physical / 10u * 9u);
		CheckMemory(*s);
		s->monitor = std::thread([s] {
			while (!s->closed.load(std::memory_order_acquire)) {
				CheckMemory(*s);
				const uint64_t pending = s->pending_since.load(std::memory_order_acquire);
				if (pending && NowNanoseconds() - pending > 2'000'000'000ull &&
				    s->pending_since.load(std::memory_order_acquire) == pending) {
					std::fprintf(stderr,"[HostBudget] packet submission/completion stalled for 2s; stopping (GPU preemption is unavailable)\n");
					Stop("GPU submission/completion timeout; inspect host before another launch");
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		});
		// The emulator's window loop exits with quick_exit(0), bypassing Metal
		// destructors. Close the submission gate and drain its current packet
		// on that path too. Fault exits use _Exit and keep the marker.
		if (std::at_quick_exit(Finish) != 0) Stop("cannot register GPU budget shutdown");
		std::fprintf(stderr, "[HostBudget] GPU average duty<=%u%%; one submitted packet; footprint<=%llu MiB (100ms monitor)\n",
		             s->percent, static_cast<unsigned long long>(s->memory_bytes >> 20));
		return s;
	}();
	return *state;
}

// All native backend submissions, including presenter/readback queues, go
// through this function. No guest work or shader iterations are discarded.
// Wall time from commit to GPU completion conservatively bounds GPU busy time.
// Paying its idle interval before returning prevents both queue accumulation
// and banking idle credits for a later burst.
inline void Commit(id<MTLCommandBuffer> command) {
	if (!command) throw std::invalid_argument("missing command buffer");
	auto& state = Get();
	std::unique_lock gate(state.submission);
	if (state.closed.load(std::memory_order_acquire)) throw std::logic_error("host GPU budget has shut down");
	CheckMemory(state);
	if (command.status >= MTLCommandBufferStatusCommitted)
		throw std::logic_error("command buffer submitted more than once");
	if (!state.marker.empty() && !state.owns_marker) {
		const int file = open(state.marker.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
		if (file < 0) {
			std::fprintf(stderr,"[HostBudget] cannot claim exclusive GPU session: %s\n",state.marker.c_str());
			std::_Exit(75);
		}
		dprintf(file,"GPU session PID=%d active; this marker is removed only after a clean drain\n",getpid());
		close(file);
		state.owns_marker = true;
	}
	const auto begin = std::chrono::steady_clock::now();
	state.pending_since.store(NowNanoseconds(),std::memory_order_release);
	[command commit];
	while (command.status < MTLCommandBufferStatusCompleted) {
		const auto elapsed = std::chrono::steady_clock::now() - begin;
		if (elapsed > std::chrono::seconds(2)) {
			std::fprintf(stderr, "[HostBudget] packet did not finish in 2s, Metal status=%ld; stopping submissions (this cannot preempt a hung GPU kernel)\n",
			             static_cast<long>(command.status));
			if (command.label) std::fprintf(stderr, "[HostBudget] packet: %.6000s\n", command.label.UTF8String);
			std::fflush(stderr);
			Stop("GPU packet timeout; inspect host before another launch");
		}
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	const auto finished = std::chrono::steady_clock::now();
	state.pending_since.store(0,std::memory_order_release);
	if (command.status != MTLCommandBufferStatusCompleted) {
		std::fprintf(stderr, "[HostBudget] Metal packet failed: %s\n",
		             command.error ? command.error.localizedDescription.UTF8String : "unknown error");
		Stop("Metal packet failed");
	}
	const uint64_t busy = std::chrono::duration_cast<std::chrono::nanoseconds>(finished - begin).count();
	const uint64_t idle = IdleNanoseconds(busy, state.percent);
	std::this_thread::sleep_until(finished + std::chrono::nanoseconds(idle));
	state.wall_busy_ns += busy;
	state.idle_ns += idle;
	if (++state.submitted % 1000u == 0) {
		std::fprintf(stderr, "[HostBudget] packets=%llu accounted_busy=%.3fs reserved_idle=%.3fs ceiling=%u%%\n",
		             static_cast<unsigned long long>(state.submitted), state.wall_busy_ns / 1.0e9,
		             state.idle_ns / 1.0e9, state.percent);
	}
}

// Call only after the application's final GPU work has drained. A crash, signal,
// timeout or memory-budget exit intentionally leaves the launch marker in place.
inline void Finish() {
	auto& state = Get();
	std::unique_lock gate(state.submission);
	state.closed.store(true,std::memory_order_release);
	if (state.monitor.joinable()) state.monitor.join();
	if (state.owns_marker) {
		unlink(state.marker.c_str());
		state.owns_marker = false;
	}
	std::fprintf(stderr, "[HostBudget] closed after %llu completed packets; busy=%.3fs idle=%.3fs ceiling=%u%%\n",
	             static_cast<unsigned long long>(state.submitted),state.wall_busy_ns / 1.0e9,
	             state.idle_ns / 1.0e9,state.percent);
}

} // namespace Libs::Graphics::Metal::HostBudget
