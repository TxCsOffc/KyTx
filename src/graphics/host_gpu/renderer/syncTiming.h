#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SYNCTIMING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SYNCTIMING_H_

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace Libs::Graphics {

// KYTY_SYNC_TIMING=1 splits each synchronous CPU->GPU readback drain into marshal, submit, GPU
// wait and copy/publish, and prints one summary line per 240 readbacks. Off by default: the
// variable is read once, and every hook is guarded by t_sync_sample, which stays null when off.
inline bool SyncTimingEnabled() noexcept {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_SYNC_TIMING");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

inline uint64_t SyncTimingNowNs() noexcept {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

struct SyncReadbackSample {
	bool     issued     = false; // a copy was recorded and the guest thread blocks on it
	uint64_t marshal_ns = 0;     // ReadMemory entry until the GPU thread starts the readback
	uint64_t submit_ns  = 0;     // Submit() of the open command buffer, inside CommandScheduler::Wait
	uint64_t wait_ns    = 0;     // vkWaitSemaphores time, inside MasterSemaphore::Wait
	uint64_t copy_ns    = 0;     // WaitPriorityOperations + publish after the GPU wait
	uint64_t total_ns   = 0;     // ReadMemory entry until return, as seen by the guest thread
	uint64_t pending    = 0;     // submits and draws/dispatches between the done and waited tick
};

// The sample filled by the readback running on this thread, or nullptr.
inline thread_local SyncReadbackSample* t_sync_sample = nullptr;

class SyncReadbackScope {
public:
	explicit SyncReadbackScope(SyncReadbackSample* sample) noexcept : m_previous(t_sync_sample) {
		t_sync_sample = sample;
	}
	~SyncReadbackScope() { t_sync_sample = m_previous; }
	SyncReadbackScope(const SyncReadbackScope&)            = delete;
	SyncReadbackScope& operator=(const SyncReadbackScope&) = delete;

private:
	SyncReadbackSample* m_previous;
};

struct SyncStat {
	double mean;
	double p95;
};

// Mean and p95 of one field over the batch, divided by `scale` (1000 for ns -> us, 1 for counts).
inline SyncStat SyncTimingStat(const std::vector<SyncReadbackSample>& batch,
                               uint64_t SyncReadbackSample::*field, double scale) {
	std::vector<uint64_t> values;
	values.reserve(batch.size());
	uint64_t sum = 0;
	for (const auto& s: batch) {
		values.push_back(s.*field);
		sum += s.*field;
	}
	std::sort(values.begin(), values.end());
	const size_t index = (values.size() * 95 + 99) / 100 - 1;
	return {static_cast<double>(sum) / static_cast<double>(values.size()) / scale,
	        static_cast<double>(values[index]) / scale};
}

// Thread-safe: readbacks complete on many guest threads. Prints one line per 240 samples.
inline void SyncTimingRecord(const SyncReadbackSample& sample) {
	constexpr size_t BatchSize = 240;
	struct State {
		std::mutex                      mutex;
		std::vector<SyncReadbackSample> batch;
		uint64_t                        window_begin_ns = 0;
	};
	static State state;

	const auto now = SyncTimingNowNs();
	std::lock_guard lock(state.mutex);
	if (state.window_begin_ns == 0) {
		state.window_begin_ns = now;
	}
	state.batch.push_back(sample);
	if (state.batch.size() < BatchSize) {
		return;
	}

	double total_ms = 0;
	for (const auto& s: state.batch) {
		total_ms += static_cast<double>(s.total_ns) / 1e6;
	}
	const double wall_s = static_cast<double>(now - state.window_begin_ns) / 1e9;
	const auto   marshal = SyncTimingStat(state.batch, &SyncReadbackSample::marshal_ns, 1000.0);
	const auto   submit  = SyncTimingStat(state.batch, &SyncReadbackSample::submit_ns, 1000.0);
	const auto   wait    = SyncTimingStat(state.batch, &SyncReadbackSample::wait_ns, 1000.0);
	const auto   copy    = SyncTimingStat(state.batch, &SyncReadbackSample::copy_ns, 1000.0);
	const auto   total   = SyncTimingStat(state.batch, &SyncReadbackSample::total_ns, 1000.0);
	const auto   pending = SyncTimingStat(state.batch, &SyncReadbackSample::pending, 1.0);
	std::fprintf(stderr,
	             "SYNC n=%zu marshal %.1f/%.1f submit %.1f/%.1f wait %.1f/%.1f copy %.1f/%.1f "
	             "total %.1f/%.1f pending %.1f | summed_ms_per_wall_s %.1f (sum over concurrent threads)\n",
	             BatchSize, marshal.mean, marshal.p95, submit.mean, submit.p95, wait.mean, wait.p95,
	             copy.mean, copy.p95, total.mean, total.p95, pending.mean,
	             wall_s > 0 ? total_ms / wall_s : 0.0);
	std::fflush(stderr);
	state.batch.clear();
	state.window_begin_ns = now;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SYNCTIMING_H_
