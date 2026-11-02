#include "graphics/host_gpu/cpWriteTrace.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/presentation/traceWindow.h"
#if defined(__APPLE__)
#include "graphics/host_gpu/metal/metalRenderer.h"
#endif

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/platform/sysAppleAutorelease.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/shader/shader.h"
#if defined(__APPLE__)
#include "graphics/host_gpu/metal/metalRenderer.h"
#endif
#include "kernel/memory.h"
#include "libs/agc.h"
#include "libs/errno.h"

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(__APPLE__)
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void  objc_autoreleasePoolPop(void* pool);
#endif
#include <vector>

namespace Libs::VideoOut {
extern std::atomic<uint64_t> g_flip_pending_checks;
extern std::atomic<uint64_t> g_flip_pending_stalls;
} // namespace Libs::VideoOut

namespace Libs::Graphics {

static thread_local CommandProcessor* g_current_processor = nullptr;
static thread_local Pm4Execution*     g_current_execution = nullptr;
static thread_local bool              g_gpu_thread        = false;
static thread_local GuestGpu*         g_gpu_state         = nullptr;

// WAIT_REG_MEM diagnostics, written by the GPU thread: last unsatisfied wait address and the count.
std::atomic<uint64_t> g_pm4_wait_addr {0};
std::atomic<uint64_t> g_pm4_wait_retries {0};

enum class Pm4SuspendReason : uint8_t { None, WaitRegMem, Other };

static thread_local Pm4SuspendReason g_pm4_suspend_reason = Pm4SuspendReason::None;

// One row per presented frame while KYTY_QUEUE_OVERLAP_TRACE is set. Counts are
// taken on the guest GPU thread and do not change which queue runs next.
struct QueueOverlapTrace {
	FILE*    file                   = nullptr;
	int      frame                  = -1;
	uint32_t gfx_slices             = 0;
	uint32_t gfx_with_compute       = 0;
	uint64_t compute_queued_sum     = 0;
	uint32_t compute_queued_peak    = 0;
	uint32_t compute_runnable_peak  = 0;
	uint32_t compute_slices         = 0;
	uint32_t compute_with_gfx       = 0;
	uint32_t gfx_wait_episodes      = 0;
	uint32_t gfx_wait_retries       = 0;
	uint32_t gfx_wait_with_compute  = 0;
	uint32_t compute_wait_blocks    = 0;
	uint32_t gfx_other_blocks       = 0;
	uint64_t gfx_wait_span_us       = 0;
	uint64_t idle_wait_us           = 0;
	uint64_t idle_gfx_wait_us       = 0;
	uint32_t idle_polls             = 0;
	bool     gfx_wait_open          = false;
	std::chrono::steady_clock::time_point gfx_wait_since {};

	QueueOverlapTrace() {
		const char* path = std::getenv("KYTY_QUEUE_OVERLAP_TRACE");
		if (path == nullptr || path[0] == '\0') {
			return;
		}
		file = std::fopen(path, "w");
		if (file == nullptr) {
			return;
		}
		std::fprintf(file,
		             "frame,gfx_slices,gfx_with_compute,compute_queued_avg_x100,"
		             "compute_queued_peak,compute_runnable_peak,compute_slices,compute_with_gfx,"
		             "gfx_wait_episodes,gfx_wait_retries,gfx_wait_with_compute,compute_wait_blocks,"
		             "gfx_other_blocks,gfx_wait_span_us,idle_wait_us,idle_gfx_wait_us,idle_polls\n");
		std::fflush(file);
	}

	~QueueOverlapTrace() {
		Flush();
		if (file != nullptr) {
			std::fclose(file);
			file = nullptr;
		}
	}

	[[nodiscard]] bool Enabled() const noexcept { return file != nullptr; }

	void Publish(int next_frame) {
		if (file == nullptr) {
			return;
		}
		if (frame < 0) {
			frame = next_frame;
			return;
		}
		if (next_frame == frame) {
			return;
		}
		WriteFrame();
		Reset(next_frame);
	}

	void Flush() {
		if (file == nullptr || frame < 0) {
			return;
		}
		WriteFrame();
		frame = -1;
	}

	void NoteSlice(bool graphics, uint32_t compute_queued, uint32_t compute_runnable,
	               uint32_t graphics_queued) {
		if (file == nullptr) {
			return;
		}
		if (graphics) {
			gfx_slices++;
			if (compute_runnable > 0) {
				gfx_with_compute++;
			}
			compute_queued_sum += compute_queued;
			compute_queued_peak   = std::max(compute_queued_peak, compute_queued);
			compute_runnable_peak = std::max(compute_runnable_peak, compute_runnable);
		} else {
			compute_slices++;
			if (graphics_queued > 0) {
				compute_with_gfx++;
			}
		}
	}

	void NoteGfxWait(bool episode, bool compute_pending) {
		if (file == nullptr) {
			return;
		}
		gfx_wait_retries++;
		if (!episode) {
			return;
		}
		gfx_wait_episodes++;
		if (compute_pending) {
			gfx_wait_with_compute++;
		}
	}

	void NoteComputeWait() {
		if (file != nullptr) {
			compute_wait_blocks++;
		}
	}

	void NoteGfxOther() {
		if (file != nullptr) {
			gfx_other_blocks++;
		}
	}

	void OpenGfxWait() {
		if (file == nullptr || gfx_wait_open) {
			return;
		}
		gfx_wait_open  = true;
		gfx_wait_since = std::chrono::steady_clock::now();
	}

	void CloseGfxWait() {
		if (file == nullptr || !gfx_wait_open) {
			return;
		}
		gfx_wait_span_us += static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::microseconds>(
		        std::chrono::steady_clock::now() - gfx_wait_since)
		        .count());
		gfx_wait_open = false;
	}

	void NoteIdle(uint64_t wait_us, bool graphics_wait) {
		if (file == nullptr) {
			return;
		}
		idle_wait_us += wait_us;
		idle_polls++;
		if (graphics_wait) {
			idle_gfx_wait_us += wait_us;
		}
	}

private:
	void Reset(int next_frame) {
		frame                 = next_frame;
		gfx_slices            = 0;
		gfx_with_compute      = 0;
		compute_queued_sum    = 0;
		compute_queued_peak   = 0;
		compute_runnable_peak = 0;
		compute_slices        = 0;
		compute_with_gfx      = 0;
		gfx_wait_episodes     = 0;
		gfx_wait_retries      = 0;
		gfx_wait_with_compute = 0;
		compute_wait_blocks   = 0;
		gfx_other_blocks      = 0;
		gfx_wait_span_us      = 0;
		idle_wait_us          = 0;
		idle_gfx_wait_us      = 0;
		idle_polls            = 0;
	}

	void WriteFrame() {
		if (file == nullptr || frame < 0) {
			return;
		}
		const uint32_t queued_avg = gfx_slices == 0
		                                ? 0
		                                : static_cast<uint32_t>(compute_queued_sum * 100ull / gfx_slices);
		std::fprintf(file,
		             "%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%llu,%llu,%llu,%u\n", frame, gfx_slices,
		             gfx_with_compute, queued_avg, compute_queued_peak, compute_runnable_peak,
		             compute_slices, compute_with_gfx, gfx_wait_episodes, gfx_wait_retries,
		             gfx_wait_with_compute, compute_wait_blocks, gfx_other_blocks,
		             static_cast<unsigned long long>(gfx_wait_span_us),
		             static_cast<unsigned long long>(idle_wait_us),
		             static_cast<unsigned long long>(idle_gfx_wait_us), idle_polls);
		std::fflush(file);
	}
};

QueueOverlapTrace& GetQueueOverlapTrace() {
	static QueueOverlapTrace trace;
	return trace;
}

struct DrawIndirectArgs {
	uint32_t vertex_count_per_instance;
	uint32_t instance_count;
	uint32_t start_vertex_location;
	uint32_t start_instance_location;
};

struct DrawIndexedIndirectArgs {
	uint32_t index_count_per_instance;
	uint32_t instance_count;
	uint32_t start_index_location;
	uint32_t base_vertex_location;
	uint32_t start_instance_location;
};

static bool GraphicsRunDebugDumpEnabled() {
	return Config::GraphicsDebugDumpEnabled() &&
	       Config::GetPrintfDirection() != Config::LogDirection::Silent;
}

GuestGpu::GuestGpu(RenderContext& renderer): m_renderer(renderer) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	GraphicsInitJmpTables();
	m_gfx_cp = std::make_unique<CommandProcessor>(renderer, 0);
	m_gfx_cp->Reset();
	m_thread = std::jthread(ThreadRun, this);
}

GuestGpu::~GuestGpu() {
	Shutdown();
}

void GuestGpu::Shutdown() {
	std::lock_guard shutdown_lock(m_shutdown_mutex);
	if (m_shutdown_complete) {
		return;
	}
	{
		Common::LockGuard lock(m_queue_mutex);
		m_accepting = false;
		m_stopping  = true;
		m_work_available.SignalAll();
	}
	if (m_thread.joinable()) {
		m_thread.join();
	}
	m_shutdown_complete = true;
}

bool GuestGpu::IsStopping() {
	Common::LockGuard lock(m_queue_mutex);
	return m_stopping;
}

void GuestGpu::SendCommand(Common::UniqueFunction<void>&& command) {
	EXIT_IF(!command);
	if (IsGpuThread()) {
		command();
		return;
	}
	Common::LockGuard lock(m_queue_mutex);
	EXIT_IF(!m_accepting);
	m_commands.push_back(std::move(command));
	m_pending_commands.fetch_add(1, std::memory_order_release);
	m_work_available.Signal();
}

void GuestGpu::ProcessCommands() {
	EXIT_IF(!IsGpuThread());
	// Early out before taking a scope: this is called on every ProcessPm4 loop iteration and
	// is usually idle, so a scope per call would cost more than it measures. When it does run,
	// it executes work marshalled from other threads, which includes the readback downloads.
	if (m_pending_commands.load(std::memory_order_acquire) == 0) {
		return;
	}
	Libs::Graphics::ProfileCpuScope profile(m_renderer.GetCommandScheduler(), "cp_marshalled_commands");
	while (m_pending_commands.load(std::memory_order_acquire) != 0) {
		Common::UniqueFunction<void> command;
		{
			Common::LockGuard lock(m_queue_mutex);
			EXIT_IF(m_commands.empty());
			command = std::move(m_commands.front());
			m_commands.pop_front();
			EXIT_IF(m_pending_commands.fetch_sub(1, std::memory_order_acq_rel) == 0);
		}
		command();
	}
}

void GuestGpu::SendCommandSync(Common::UniqueFunction<void>&& command) {
	EXIT_IF(!command);
	if (IsGpuThread()) {
		command();
		return;
	}
	std::binary_semaphore done {0};
	SendCommand([operation = std::move(command), &done]() mutable {
		operation();
		done.release();
	});
	done.acquire();
}

void GuestGpu::Submit(std::span<const uint32_t> draw_commands,
                      std::span<const uint32_t> constant_commands) {
	if (draw_commands.empty()) {
		return;
	}
	Submission submission;
	submission.type              = SubmissionType::Graphics;
	submission.queue_id          = 0;
	submission.commands          = draw_commands;
	submission.constant_commands = constant_commands;
	Enqueue(std::move(submission));
}

void GuestGpu::SubmitCompute(uint32_t queue, std::span<const uint32_t> commands) {
	EXIT_IF(commands.empty());

	EXIT_NOT_IMPLEMENTED(queue < ComputeQueueBase || queue >= ComputeQueueBase + ComputeQueueCount);

	const auto compute_queue = queue - ComputeQueueBase;
	Submission submission;
	submission.type     = SubmissionType::Compute;
	submission.queue_id = 1 + compute_queue;
	submission.commands = commands;
	Enqueue(std::move(submission));
}

void GuestGpu::SubmitFlipPreparation(uint64_t request_id) {
	Submission submission;
	submission.type            = SubmissionType::FlipPreparation;
	submission.queue_id        = 0;
	submission.flip_request_id = request_id;
	Enqueue(std::move(submission));
}

void GuestGpu::SuspendPoint() {
	EXIT_IF(IsGpuThread() || CommandScheduler::InDeferredOperation());
	// Do not hold a queue lock while waiting: asynchronous work may be needed to
	// finish the preceding graphics frame. The first point returns immediately.
	{ Libs::Graphics::ProfileCpuScope profile(m_renderer.GetCommandScheduler(), "guest_suspend_point_wait");
	  m_suspend_point_ready->acquire(); }
	Submission submission;
	submission.type = SubmissionType::SuspendPoint;
	Enqueue(std::move(submission));
	m_done_num++;
}

int GuestGpu::GetFrameNum() const {
	return m_done_num;
}

CommandProcessor& GuestGpu::GetProcessor(uint32_t queue_id) {
	EXIT_IF(queue_id >= QueueCount);
	if (queue_id == 0) {
		return *m_gfx_cp;
	}
	auto& processor = m_compute_cp[queue_id - 1];
	if (processor == nullptr) {
		processor = std::make_unique<CommandProcessor>(m_renderer, ComputeQueueBase + queue_id - 1);
	}
	return *processor;
}

void CommandProcessor::Reset() {
	m_sh_ctx.Reset();
	m_ucfg.Reset();
	m_ctx.Reset();
	m_saved_ctx.Reset();
	m_context_state_pushed             = false;
	m_index_type_and_size              = 0;
	m_index_buffer_size                = 0;
	m_index_base_addr                  = 0;
	m_num_instances                    = 1;
	m_predicate_skip                   = false;
	m_user_data_marker                 = HW::UserSgprType::Unknown;
	m_draw_indirect_args_base_addr     = 0;
	m_dispatch_indirect_args_base_addr = 0;

	std::memset(m_const_ram, 0, sizeof(m_const_ram));
}

void CommandProcessor::ApplyContextStateOperation(ContextStateOperation operation) {
	switch (operation) {
		case ContextStateOperation::Clear: m_ctx.Reset(); break;
		case ContextStateOperation::Push:
			EXIT_IF(m_context_state_pushed);
			m_saved_ctx            = m_ctx;
			m_context_state_pushed = true;
			break;
		case ContextStateOperation::Pop:
			EXIT_IF(!m_context_state_pushed);
			m_ctx                  = m_saved_ctx;
			m_saved_ctx            = {};
			m_context_state_pushed = false;
			break;
		case ContextStateOperation::PushClear:
			EXIT_IF(m_context_state_pushed);
			m_saved_ctx            = m_ctx;
			m_context_state_pushed = true;
			m_ctx.Reset();
			break;
		default: EXIT("unknown context state operation: %u\n", static_cast<uint32_t>(operation));
	}
}

// TASK-0014 instrumentation: these run inside guest_process_slice but outside the recording
// scopes, so they land in translation_other_cpu_ms -- 39.6 ms/epoch of which EXP-0022
// attributed only 11.2 to garbage collection. Neither carried a scope before.
void CommandProcessor::BufferInit() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		Metal::MetalRenderer::Instance().Initialize();
	}
#endif
	Libs::Graphics::ProfileCpuScope profile(GetScheduler(), "cp_buffer_init");
	GetScheduler().Begin(m_ctx, m_ucfg, m_sh_ctx);
}

// Lazy submission, EXP-0025. Every flush measured in a paused fight came from our own
// per-submission policy (cp_buffer_flush, 329.6/epoch, 20.26 ms) and NONE from a guest
// end-of-pipe request (cp_flush_eop, zero), while we recorded 16.9 draws per submit against
// DXVK's documented 50-100 per command list. So accumulate across guest submissions.
//
// Safety: a guest EOP always submits, since that is the guest asking to observe completion.
// Everything that waits submits on its own -- CommandScheduler::Wait calls Submit() when the
// requested tick is the current one, and Finish() submits before waiting -- so deferring
// cannot deadlock the readback path at bufferCache.cpp:276.
//
// KYTY_SUBMIT_DRAW_THRESHOLD=0 restores a submit per guest submission; default 64.
static uint32_t SubmitDrawThreshold() {
	static const uint32_t value = [] () -> uint32_t {
		const char* raw = std::getenv("KYTY_SUBMIT_DRAW_THRESHOLD");
		if (raw == nullptr) {
			return 64;
		}
		return static_cast<uint32_t>(std::strtoul(raw, nullptr, 10));
	}();
	return value;
}

// Submission-policy diagnostics (GPU thread only): flush counts by reason, flips.
static constexpr size_t kFlushReasonCount = 8;
static const char* const kFlushReasonNames[kFlushReasonCount] = {
    "cp_buffer_flush", "cp_flush_eop", "cp_flip_wait", "cp_suspend_point",
    "cp_wait_regmem",  "budget",       "idle",         "other"};
static std::atomic<uint64_t> g_flush_by_reason[kFlushReasonCount] {};
static std::atomic<uint64_t> g_flip_count {0};

// KYTY_LABEL_MODE=cpu_on_complete (opt-in): EOP labels are written by the CPU when their tick
// completes, with no GPU write and no submit per label. Default is gpu (GPU-written labels).
static bool CpuLabelMode() {
	static const bool enabled = [] {
		const char* mode = std::getenv("KYTY_LABEL_MODE");
		return mode != nullptr && std::strcmp(mode, "cpu_on_complete") == 0;
	}();
	return enabled;
}

// KYTY_WAIT_LOG=1: log WAIT_REG_MEM suspend/resume and EOP event delivery (GPU thread).
static bool WaitLogEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_WAIT_LOG");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}
static const void* g_wait_log_addr = nullptr;

// Flushes the guest must not be kept waiting on: an interrupt or flip the guest waits for, the
// suspend point, a WAIT_REG_MEM on a value only pending work can produce.
static bool IsMandatoryFlushReason(const char* reason) {
	return std::strcmp(reason, "cp_flush_eop") == 0 || std::strcmp(reason, "cp_flip_wait") == 0 ||
	       std::strcmp(reason, "cp_suspend_point") == 0 || std::strcmp(reason, "cp_wait_regmem") == 0;
}

static size_t FlushReasonIndex(const char* reason) {
	for (size_t i = 0; i + 1 < kFlushReasonCount; ++i) {
		if (std::strcmp(reason, kFlushReasonNames[i]) == 0) {
			return i;
		}
	}
	return kFlushReasonCount - 1;
}

// KYTY_LABEL_MODE=hybrid: as cpu_on_complete, except that a label address which a CP WAIT_REG_MEM
// has ever polled stays GPU-written (sticky). The CP's own polls are observable; guest CPU polls
// are not in cpu_on_complete, because those pages are not GPU-dirty and never fault.
static bool HybridLabelMode() {
	static const bool enabled = [] {
		const char* mode = std::getenv("KYTY_LABEL_MODE");
		return mode != nullptr && std::strcmp(mode, "hybrid") == 0;
	}();
	return enabled;
}
static bool CpuLabelsEnabled() {
	return CpuLabelMode() || HybridLabelMode();
}
// GPU thread only: label addresses pinned to GPU writes by hybrid mode.
static std::unordered_set<uint64_t> g_gpu_pinned_labels;

// KYTY_WAIT_RETRY_ELIDE=1: a suspended WAIT_REG_MEM is re-run by the GPU thread about every 100 us
// (CondVar::WaitFor takes microseconds). Each re-run used to flush the open buffer. Only the first
// block of a wait flushes now; a re-run of the same packet flushes again only after another slice
// has been processed, or while a label or priority operation sits in the open tick.
static bool WaitRetryElideEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_WAIT_RETRY_ELIDE");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}
static uint64_t g_wait_block_key     = 0;  // GPU thread only: packet of the last block
static uint64_t g_slices_since_block = 0;  // GPU thread only: submissions run since that block

// WAIT_REG_MEM flush decisions (GPU thread): 0 flag off (always flush), 1 label in the open buffer,
// 2 priority operation in the open buffer, 3 open buffer dirty, 4 clean and elided.
static constexpr size_t kWaitReasonCount = 5;
static const char* const kWaitReasonNames[kWaitReasonCount] = {"flag_off", "label_open", "priority_open", "dirty_open", "clean_elided"};
static std::atomic<uint64_t> g_wait_reason_counts[kWaitReasonCount] {};
static std::atomic<uint32_t> g_wait_reason_logged {0};

static void NoteWaitReason(size_t reason, bool retry) {
	g_wait_reason_counts[reason].fetch_add(1, std::memory_order_relaxed);
	if (reason != 4 && g_wait_reason_logged.fetch_add(1, std::memory_order_relaxed) < 50) {
		std::fprintf(stderr, "[wait-flush] reason=%s retry=%d key_changed=%d\n", kWaitReasonNames[reason],
		             retry ? 1 : 0, retry ? 0 : 1);
	}
}

// KYTY_WAIT_BY_ORDER=1: a WAIT_REG_MEM on an address that a GPU label recorded earlier in the same
// command processor's stream already holds the awaited value for needs neither a submit nor a
// suspend: stream order guarantees the value at that point, so only a barrier is recorded. Other
// queues, pending values that do not satisfy the compare, and CPU-written addresses still suspend.
// GPU thread only.
static bool WaitByOrderEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_WAIT_BY_ORDER");
		if (value != nullptr) {
			return value[0] != '0';
		}
		return true;
	}();
	return enabled;
}
struct PendingGpuLabel {
	const void* owner;  // the CommandProcessor (queue) that recorded the label
	uint64_t    value;  // latest GPU-recorded value at this address
	uint64_t    tick;   // open tick it was recorded into
	uint32_t    size;
};
static std::unordered_map<uint64_t, PendingGpuLabel> g_pending_gpu_labels;
bool LabelPageCpuPolled(uint64_t address);  // bufferCache.cpp
static std::atomic<uint32_t> g_wait_by_order_logged {0};
static std::atomic<uint64_t> g_wait_by_order_count {0};

// KYTY_LABEL_COALESCE (default on; 0 disables; lazy label mode only): a GPU label that would be
// submitted at once stays in the open buffer. The label watchdog submits it within LabelWindowNs(),
// and a WAIT_REG_MEM on it or a guest read of its page submits it before the value is needed.
static bool LabelCoalesceEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_LABEL_COALESCE");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

// KYTY_RATE_LOG=0 turns off the per-2 s line: submits/s, flips/s and submits per flip (GPU thread).
static void MaybePrintRates() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_RATE_LOG");
		return value == nullptr || value[0] != '0';
	}();
	if (!enabled) {
		return;
	}
	static auto     last_time    = std::chrono::steady_clock::now();
	static uint64_t last_submits = 0;
	static uint64_t last_flips   = 0;
	const auto      now          = std::chrono::steady_clock::now();
	const double    elapsed      = std::chrono::duration<double>(now - last_time).count();
	if (elapsed < 2.0) {
		return;
	}
	const uint64_t submits   = Libs::Graphics::SubmitCountTotal();
	const uint64_t flips     = Libs::VideoOut::VideoOutDriver::FlipsCompletedTotal();
	const uint64_t d_submits = submits - last_submits;
	const uint64_t d_flips   = flips - last_flips;
	extern std::atomic<uint64_t> g_fault_writes_count;
	extern std::atomic<uint64_t> g_fault_reads_count;
	extern std::atomic<uint64_t> g_fault_time_ns;
	extern std::atomic<uint64_t> g_present_wait_ns;

	const uint64_t d_fault_w     = g_fault_writes_count.exchange(0);
	const uint64_t d_fault_r     = g_fault_reads_count.exchange(0);
	const uint64_t d_fault_ns    = g_fault_time_ns.exchange(0);
	const uint64_t d_presents_ns = g_present_wait_ns.exchange(0);
	const uint64_t d_fp_checks   = Libs::VideoOut::g_flip_pending_checks.exchange(0);
	const uint64_t d_fp_stalls   = Libs::VideoOut::g_flip_pending_stalls.exchange(0);

	const double avg_present_ms =
	    d_flips > 0 ? (static_cast<double>(d_presents_ns) / d_flips / 1e6) : 0.0;
	const double fault_cpu_pct = (static_cast<double>(d_fault_ns) / (elapsed * 1e9)) * 100.0;

	std::fprintf(stderr,
	             "[rate] FPS=%.1f submits/s=%.0f | faults/s=%.0f (w=%.0f r=%.0f cpu=%.1f%%) | present_wait=%.1fms flip_stall=%.0f/%.0f",
	             static_cast<double>(d_flips) / elapsed,
	             static_cast<double>(d_submits) / elapsed,
	             static_cast<double>(d_fault_w + d_fault_r) / elapsed,
	             static_cast<double>(d_fault_w) / elapsed,
	             static_cast<double>(d_fault_r) / elapsed,
	             fault_cpu_pct,
	             avg_present_ms,
	             static_cast<double>(d_fp_stalls) / elapsed,
	             static_cast<double>(d_fp_checks) / elapsed);

	for (size_t i = 0; i < kWaitReasonCount; ++i) {
		std::fprintf(stderr, " wait_%s=%llu", kWaitReasonNames[i],
		             static_cast<unsigned long long>(g_wait_reason_counts[i].exchange(0)));
	}
	std::fprintf(stderr, "\n");
	last_time    = now;
	last_submits = submits;
	last_flips   = flips;
}

// Submits the open command buffer. Callers choose the reason; `cp_buffer_flush` (the per-round
// default) is not mandatory and submits only past the draw threshold, when the GPU is idle with
// work, or when the lazy-label budget is exceeded.
void CommandProcessor::BufferFlush(const char* reason) {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		Metal::MetalRenderer::Instance().Flush();
	}
#endif
	Libs::Graphics::ProfileCpuScope profile(GetScheduler(), reason);
	MaybePrintRates();
	auto& cache = m_renderer.GetBufferCache();
	const auto submit = [this, &cache](size_t index) {
		g_flush_by_reason[index].fetch_add(1, std::memory_order_relaxed);
		if (cache.LabelPendingInOpen()) {
			cache.NoteLazyFlush(Libs::Graphics::BufferCache::LazyFlush::Other);
		}
		GetScheduler().Flush();
	};
	if (cache.LabelBudgetExceeded()) {
		cache.NoteLazyFlush(Libs::Graphics::BufferCache::LazyFlush::Budget);
		submit(5);
		return;
	}
	if (IsMandatoryFlushReason(reason)) {
		submit(FlushReasonIndex(reason));
		return;
	}
	const auto threshold = SubmitDrawThreshold();
	const bool below     = threshold != 0 && GetScheduler().CurrentSubmitDrawCount() < threshold &&
	                   !GetScheduler().HasPriorityOperationAtCurrentTick();
	if (below) {
		if (CpuLabelsEnabled() && cache.GpuIdleWithOpenWork(GetScheduler().CurrentSubmitDrawCount() > 0)) {
			submit(6);
		}
		return;
	}
	submit(FlushReasonIndex(reason));
}

void CommandProcessor::BufferWait() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		Metal::MetalRenderer::Instance().WaitIdle();
	}
#endif
	BufferInit();
	GetScheduler().Finish();
}

void CommandProcessor::ResetDeCe() {
	m_de_count    = 0;
	m_ce_count    = 0;
	m_ce_complete = false;
}

void CommandProcessor::WaitCe() {
	if (m_ce_count <= m_de_count && !m_ce_complete) {
		SuspendPm4();
	}
}

void CommandProcessor::WaitDeDiff(uint32_t diff) {
	EXIT_IF(m_de_count > m_ce_count);
	if (m_ce_count - m_de_count >= diff) {
		SuspendPm4();
	}
}

void CommandProcessor::WaitForRewind(bool valid) {
	if (!valid) {
		SuspendPm4();
	}
}

void CommandProcessor::IncrementDe() {
	m_de_count++;
}

void CommandProcessor::IncrementCe() {
	m_ce_count++;
}

void CommandProcessor::WriteConstRam(uint32_t offset, const uint32_t* src, uint32_t dw_num) {
	memcpy(m_const_ram + offset / 4, src, static_cast<size_t>(dw_num) * 4);
}

void CommandProcessor::DumpConstRam(uint32_t* dst, uint32_t offset, uint32_t dw_num) {
	memcpy(dst, m_const_ram + offset / 4, static_cast<size_t>(dw_num) * 4);
}

bool TestWaitRegMemValue(uint64_t value, uint64_t ref, uint64_t mask, uint32_t func) {
	switch (func) {
		case 0: return true;
		case 1: return (value & mask) < ref;
		case 2: return (value & mask) <= ref;
		case 3: return (value & mask) == ref;
		case 4: return (value & mask) != ref;
		case 5: return (value & mask) >= ref;
		case 6: return (value & mask) > ref;
		default: EXIT("unknown wait compare function: %" PRIu32 "\n", func);
	}

	return false;
}

template <typename T>
void CommandProcessor::WaitRegMem(uint32_t func, const T* addr, T ref, T mask, uint32_t poll,
                                  uint32_t wait_op) {
	EXIT_IF(addr == nullptr);
	if ((wait_op & ~1u) != 0) {
		EXIT("unsupported wait_reg_mem operation: 0x%08" PRIx32 "\n", wait_op);
	}

	(void)poll;
	const bool satisfied = TestWaitRegMemValue(*addr, ref, mask, func);
	if (WaitLogEnabled()) {
		static std::atomic<int> printed {0};
		if (printed.fetch_add(1) < 4000) {
			if (!satisfied) {
				g_wait_log_addr = reinterpret_cast<const void*>(addr);
				std::fprintf(stderr, "[wait] suspend addr=%p func=%u ref=0x%llx mask=0x%llx value=0x%llx\n",
				             reinterpret_cast<const void*>(addr), func,
				             static_cast<unsigned long long>(ref), static_cast<unsigned long long>(mask),
				             static_cast<unsigned long long>(*addr));
			} else if (g_wait_log_addr == reinterpret_cast<const void*>(addr)) {
				g_wait_log_addr = nullptr;
				std::fprintf(stderr, "[wait] resume addr=%p value=0x%llx\n",
				             reinterpret_cast<const void*>(addr),
				             static_cast<unsigned long long>(*addr));
			}
		}
	}
	if (!satisfied) {
		if (WaitByOrderEnabled()) {
			const auto found = g_pending_gpu_labels.find(reinterpret_cast<uint64_t>(addr));
			// Only graphics-queue waits on a label the guest CPU never polls: the buffers a wait
			// guards are snapshotted at record time, so a CPU write made while the real CP would
			// have been stalled must still be visible to them.
			const bool eligible = found != g_pending_gpu_labels.end() && found->second.owner == this &&
			                      !IsAsyncComputeQueue() && found->second.size == sizeof(T) &&
			                      !LabelPageCpuPolled(reinterpret_cast<uint64_t>(addr)) &&
			                      !GetScheduler().IsFree(found->second.tick) &&
			                      TestWaitRegMemValue(found->second.value, ref, mask, func);
			if (eligible) {
				if (g_wait_by_order_logged.fetch_add(1, std::memory_order_relaxed) < 50) {
					std::fprintf(stderr,
					             "[wait-by-order] addr=0x%llx func=%u ref=0x%llx mask=0x%llx pending=0x%llx"
					             " size=%u writer_tick=%llu open_tick=%llu gpu_known=%llu\n",
					             static_cast<unsigned long long>(reinterpret_cast<uint64_t>(addr)), func,
					             static_cast<unsigned long long>(ref), static_cast<unsigned long long>(mask),
					             static_cast<unsigned long long>(found->second.value), found->second.size,
					             static_cast<unsigned long long>(found->second.tick),
					             static_cast<unsigned long long>(GetScheduler().CurrentTick()),
					             static_cast<unsigned long long>(GetScheduler().GetMasterSemaphore().KnownGpuTick()));
				}
				// Recorded earlier in this stream with the value the wait needs: stream order
				// already guarantees it. Order the label write before what follows, then go on.
#if defined(__APPLE__)
				if (Metal::UseNativeMetalBackend() && std::getenv("KYTY_NO_WBO_SHORTCUT") == nullptr) {
					g_wait_by_order_count.fetch_add(1, std::memory_order_relaxed);
					return;
				}
#endif
				CurrentBuffer().EndRendering();
				vk::MemoryBarrier barrier {};
				barrier.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
				barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
				CurrentBuffer().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
				                                         vk::PipelineStageFlagBits::eAllCommands, {}, 1,
				                                         &barrier, 0, nullptr, 0, nullptr);
				g_wait_by_order_count.fetch_add(1, std::memory_order_relaxed);
				return;
			}
		}
		g_pm4_wait_addr.store(reinterpret_cast<uint64_t>(addr), std::memory_order_relaxed);
		g_pm4_wait_retries.fetch_add(1, std::memory_order_relaxed);
		// The awaited value may come from work in the open buffer (a label, a copy, a shader). A
		// suspended CP makes no progress, so nothing else would submit that work: flush it.
		// Identity of the packet this submission is suspended on: its stream and offset.
		uint64_t key = 0;
		if (g_current_execution != nullptr && !g_current_execution->m_buffer_stack.empty()) {
			const auto& cursor = g_current_execution->m_buffer_stack.back();
			key = reinterpret_cast<uint64_t>(cursor.commands.data()) + 4u * uint64_t {cursor.offset_dw} + 1u;
		}
		const bool retry = key == g_wait_block_key && g_slices_since_block == 0;
		// Should the open buffer be submitted for this wait? With KYTY_WAIT_RETRY_ELIDE=1 only when it
		// holds work: a recorded command since the last submit, a label, or a priority operation. A clean
		// buffer cannot contain the producer, which was submitted (or is in flight) already.
		auto&       cache        = m_renderer.GetBufferCache();
		const bool label_open    = cache.LabelPendingInOpen();
		const bool priority_open = GetScheduler().HasPriorityOperationAtCurrentTick();
		const bool dirty         = GetScheduler().OpenBufferDirty();
		size_t     reason        = 0;  // WaitReason index
		if (WaitRetryElideEnabled()) {
			reason = label_open ? 1 : priority_open ? 2 : dirty ? 3 : 4;
		}
		NoteWaitReason(reason, retry);
		if (reason != 4) {
			BufferFlush("cp_wait_regmem");
		}
		g_wait_block_key     = key;
		g_slices_since_block = 0;
		if (HybridLabelMode()) {
			g_gpu_pinned_labels.insert(reinterpret_cast<uint64_t>(addr));
		}
		g_pm4_suspend_reason = Pm4SuspendReason::WaitRegMem;
		SuspendPm4();
	}
}

template void CommandProcessor::WaitRegMem<uint32_t>(uint32_t, const uint32_t*, uint32_t, uint32_t,
                                                     uint32_t, uint32_t);
template void CommandProcessor::WaitRegMem<uint64_t>(uint32_t, const uint64_t*, uint64_t, uint64_t,
                                                     uint32_t, uint32_t);

void CommandProcessor::WriteData(uint32_t* dst, const uint32_t* src, uint32_t dw_num,
                                 uint32_t write_control) {
	const uint32_t dst_sel = ((write_control >> 30u) & 0x1u) | ((write_control >> 7u) & 0x1eu);
	const bool     write_one_address = ((write_control >> 16u) & 0x1u) != 0;

	switch (dst_sel) {
		case 0:
		case 2:
		case 4:
		case 5:
		case 6: break;
		default: EXIT("unsupported writeData destination selector 0x%02" PRIx32 "\n", dst_sel);
	}
	if (dw_num == 0) {
		return;
	}

	Libs::Graphics::CpTraceWrite(reinterpret_cast<uint64_t>(dst), write_one_address ? sizeof(uint32_t) : static_cast<uint64_t>(dw_num) * sizeof(uint32_t), "write-data", src[0]);
	if (write_one_address) {
		for (uint32_t i = 0; i < dw_num; i++) {
			dst[0] = src[i];
		}
	} else {
		memcpy(dst, src, static_cast<size_t>(dw_num) * sizeof(uint32_t));
	}
}

void CommandProcessor::WriteReferenceClock(uint64_t dst_address, uint32_t num_bytes) {
	if (dst_address == 0 || (num_bytes != sizeof(uint32_t) && num_bytes != sizeof(uint64_t)) ||
	    (dst_address & (num_bytes - 1u)) != 0) {
		EXIT("invalid reference-clock copy, dst=0x%016" PRIx64 " size=%u\n", dst_address,
		     num_bytes);
	}
	const auto value = Sync::ReadReferenceClock();
	std::memcpy(reinterpret_cast<void*>(dst_address), &value, num_bytes);
	static std::atomic<uint32_t> clock_log_count {0};
	if (clock_log_count.fetch_add(1) < 64) {
		LOGF("\t copy_data reference clock: dst=0x%016" PRIx64 " value=0x%016" PRIx64
		     " size=%u\n",
		     dst_address, value, num_bytes);
	}
}

void CommandProcessor::DmaData(uint8_t engine, uint8_t dst_sel, uint8_t dst_cache_policy,
                               uint64_t dst_address_or_offset, uint8_t src_sel,
                               uint8_t  src_cache_policy,
                               uint64_t src_address_or_offset_or_immediate, uint32_t num_bytes,
                               uint8_t wait_for_previous, uint8_t write_confirm,
                               uint8_t block_engine) {
	EXIT_NOT_IMPLEMENTED(engine > 1);
	if (num_bytes == 0) {
		return;
	}
	{
		// Watch guest DMA traffic to the video-out flip buffers.
		constexpr uint64_t kFlipLo = 0x8170000000ull;
		constexpr uint64_t kFlipHi = 0x8180000000ull;
		const bool src_memory = src_sel != 2;
		const bool dst_hit = dst_address_or_offset >= kFlipLo && dst_address_or_offset < kFlipHi;
		const bool src_hit = src_memory && src_address_or_offset_or_immediate >= kFlipLo &&
		                     src_address_or_offset_or_immediate < kFlipHi;
		if (dst_hit || src_hit) {
			static std::atomic<uint32_t> s_flip_dma {0};
			if (s_flip_dma.fetch_add(1) < 30) {
				std::fprintf(stderr,
				             "[FlipWatch] dma src_sel=%u dst_sel=%u src=0x%llx dst=0x%llx bytes=%u\n",
				             static_cast<unsigned>(src_sel), static_cast<unsigned>(dst_sel),
				             static_cast<unsigned long long>(src_memory
				                                                 ? src_address_or_offset_or_immediate
				                                                 : 0),
				             static_cast<unsigned long long>(dst_address_or_offset), num_bytes);
			}
		}
	}
	EXIT_NOT_IMPLEMENTED(dst_cache_policy > 3);
	EXIT_NOT_IMPLEMENTED(src_cache_policy > 3);
	EXIT_NOT_IMPLEMENTED(wait_for_previous > 1);
	EXIT_NOT_IMPLEMENTED(write_confirm > 1);
	EXIT_NOT_IMPLEMENTED(block_engine > 1);
	if (static_cast<uint32_t>(dst_address_or_offset) == 0x3022cu) {
		return;
	}
	auto decode_gds = [](uint8_t selector, bool& is_gds) {
		switch (selector) {
			case 0:
			case 3: is_gds = false; return true;
			case 1: is_gds = true; return true;
			default: return false;
		}
	};
	if (dst_sel == 2) {
		// kNowhere discards the GL2 prefetch destination without a guest-visible write.
		if (src_sel != 3) {
			EXIT("unsupported dmaData nowhere source selector 0x%02" PRIx8 "\n", src_sel);
		}
		return;
	}
	{
		// KYTY_LOG_DMA_RT=1: every DMA into the render-target arena, with the presented-flip counter.
		static const bool s_log_dma_rt = std::getenv("KYTY_LOG_DMA_RT") != nullptr;
		if (s_log_dma_rt && dst_sel != 1 && dst_address_or_offset >= 0x8240000000ull && dst_address_or_offset < 0x8250000000ull) {
			static std::atomic<uint32_t> s_dma_rt_n {0};
			if (s_dma_rt_n.fetch_add(1) < 4000) {
				std::fprintf(stderr, "[DmaRT] flip=%llu src_sel=%u dst_sel=%u src=0x%llx dst=0x%llx bytes=%u\n",
				             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)),
				             static_cast<unsigned>(src_sel), static_cast<unsigned>(dst_sel),
				             static_cast<unsigned long long>(src_address_or_offset_or_immediate),
				             static_cast<unsigned long long>(dst_address_or_offset), num_bytes);
			}
		}
	}
	if (std::getenv("KYTY_LOG_DMA") != nullptr && dst_sel != 1 && src_sel != 1) {
		static std::atomic<uint32_t> s_dma_n {0};
		if (s_dma_n.fetch_add(1) < 80) {
			std::fprintf(stderr, "[Dma] src_sel=%u dst_sel=%u src=0x%llx dst=0x%llx bytes=%u\n", static_cast<unsigned>(src_sel),
			             static_cast<unsigned>(dst_sel), static_cast<unsigned long long>(src_address_or_offset_or_immediate),
			             static_cast<unsigned long long>(dst_address_or_offset), num_bytes);
		}
	}
	if (std::getenv("KYTY_LOG_GDS") != nullptr && (dst_sel == 1 || src_sel == 1)) {
		static std::atomic<uint32_t> s_gds_dma {0};
		if (s_gds_dma.fetch_add(1) < 40) {
			std::fprintf(stderr, "[GdsDma] src_sel=%u dst_sel=%u src=0x%llx dst=0x%llx bytes=%u\n", static_cast<unsigned>(src_sel),
			             static_cast<unsigned>(dst_sel), static_cast<unsigned long long>(src_address_or_offset_or_immediate),
			             static_cast<unsigned long long>(dst_address_or_offset), num_bytes);
		}
	}
	if (Metal::UseNativeMetalBackend() && std::getenv("KYTY_MSL_GDS") != nullptr && (dst_sel == 1 || src_sel == 1)) {
		if (src_sel == 2) {
			Metal::GdsFill(dst_address_or_offset, num_bytes, static_cast<uint32_t>(src_address_or_offset_or_immediate & 0xffffffffu));
		} else {
			Metal::GdsDma(dst_sel == 1, src_sel == 1, dst_address_or_offset, src_address_or_offset_or_immediate, num_bytes);
		}
		return;
	}
	bool dst_gds = false;
	if (!decode_gds(dst_sel, dst_gds)) {
		EXIT("unsupported dmaData destination selector 0x%02" PRIx8 "\n", dst_sel);
	}
	auto& buffer_cache = m_renderer.GetBufferCache();
	if (src_sel == 2) {
		buffer_cache.FillBuffer(
		    dst_address_or_offset, num_bytes,
		    static_cast<uint32_t>(src_address_or_offset_or_immediate & 0xffffffffu), dst_gds);
		return;
	}
	bool src_gds = false;
	if (!decode_gds(src_sel, src_gds)) {
		EXIT("unsupported dmaData source selector 0x%02" PRIx8 "\n", src_sel);
	}
	if (src_gds && dst_gds) {
		EXIT("unsupported dmaData GDS-to-GDS copy\n");
	}
	buffer_cache.CopyBuffer(dst_address_or_offset, src_address_or_offset_or_immediate, num_bytes,
	                        dst_gds, src_gds);
}

void GuestGpu::Enqueue(Submission submission) {
	EXIT_IF(submission.queue_id >= QueueCount);
	Common::LockGuard lock(m_queue_mutex);
	EXIT_IF(!m_accepting);
	m_queues[submission.queue_id].push_back(std::move(submission));
	m_submission_count++;
	m_work_available.Signal();
}

void GuestGpu::WaitForIdle() {
	Libs::Graphics::ProfileCpuScope profile(m_renderer.GetCommandScheduler(), "guest_idle_completion_wait");
	EXIT_IF(IsGpuThread() || CommandScheduler::InDeferredOperation());
	Common::LockGuard lock(m_queue_mutex);
	while (m_processing || !m_commands.empty() || m_submission_count != 0) {
		m_idle.Wait(&m_queue_mutex);
	}
}

void GuestGpu::ThreadRun(void* data) {
	auto* gpu = static_cast<GuestGpu*>(data);
	EXIT_IF(gpu == nullptr);
	KYTY_PROFILER_THREAD("Thread_Gpu");
	g_gpu_thread = true;
	g_gpu_state  = gpu;
#if defined(__APPLE__)
	// KYTY_GPU_THREAD_QOS=1 (opt-in): the highest QoS class for the command processor. On by default it coincided with guest
	// crashes in Job threads (heap walks on garbage, NULL+0x10) from boot to match start (um85-um91); without it the match loads
	// (um92) - the game's own threading is sensitive to the changed thread timing.
	if (const char* qos = std::getenv("KYTY_GPU_THREAD_QOS"); qos != nullptr && qos[0] == '1') {
		pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	}
#endif

	auto& overlap = GetQueueOverlapTrace();
	for (;;) {
		ScopedAutoreleasePool        autorelease_pool;
		Submission                   submission;
		Common::UniqueFunction<void> command;
		bool                         has_submission   = false;
		bool                         should_stop      = false;
		uint32_t                     compute_queued   = 0;
		uint32_t                     compute_runnable = 0;
		uint32_t                     graphics_queued = 0;
		auto& profile_scheduler = gpu->m_renderer.GetCommandScheduler();
		profile_scheduler.SetProfileFrame(gpu->GetFrameNum());
		overlap.Publish(gpu->GetFrameNum());
		{
			Common::LockGuard lock(gpu->m_queue_mutex);
			while (gpu->m_commands.empty() && gpu->m_submission_count == 0 && !gpu->m_stopping) {
				gpu->m_processing = false;
				gpu->m_idle.Signal();
				Libs::Graphics::ProfileCpuScope profile(profile_scheduler, "guest_no_work_wait");
				gpu->m_work_available.Wait(&gpu->m_queue_mutex);
			}
			if (gpu->m_stopping && gpu->m_commands.empty() && gpu->m_submission_count == 0) {
				gpu->m_processing = false;
				gpu->m_idle.SignalAll();
				should_stop = true;
			} else if (!gpu->m_commands.empty()) {
				command = std::move(gpu->m_commands.front());
				gpu->m_commands.pop_front();
				EXIT_IF(gpu->m_pending_commands.fetch_sub(1, std::memory_order_acq_rel) == 0);
				gpu->m_processing = true;
				// Another thread may have produced a value a suspended wait is polling (labels
				// are published off the GPU thread): let every blocked submission retry now.
				for (auto& queue: gpu->m_queues) {
					if (!queue.empty()) {
						queue.front().blocked = false;
					}
				}
			} else {
				int selected_queue = -1;
				for (uint32_t offset = 0; offset < QueueCount; offset++) {
					const auto id = (gpu->m_next_queue + offset) % QueueCount;
					if (!gpu->m_queues[id].empty() && !gpu->m_queues[id].front().blocked) {
						selected_queue = static_cast<int>(id);
						break;
					}
				}
				if (selected_queue < 0) {
					const bool graphics_wait =
					    !gpu->m_queues[0].empty() && gpu->m_queues[0].front().wait_blocked;
					const auto idle_start = std::chrono::steady_clock::now();
					gpu->m_processing     = false;
					{
						Libs::Graphics::ProfileCpuScope profile(profile_scheduler,
						    graphics_wait ? "guest_pm4_blocked_wait" : "guest_blocked_wait");
						gpu->m_work_available.WaitFor(&gpu->m_queue_mutex, 100);
					}
					const auto idle_us = static_cast<uint64_t>(
					    std::chrono::duration_cast<std::chrono::microseconds>(
					        std::chrono::steady_clock::now() - idle_start)
					        .count());
					overlap.NoteIdle(idle_us, graphics_wait);
					for (auto& queue: gpu->m_queues) {
						if (!queue.empty()) {
							queue.front().blocked = false;
						}
					}
					continue;
				}
				if (overlap.Enabled()) {
					graphics_queued = static_cast<uint32_t>(gpu->m_queues[0].size());
					for (uint32_t id = 1; id < QueueCount; id++) {
						compute_queued += static_cast<uint32_t>(gpu->m_queues[id].size());
						if (!gpu->m_queues[id].empty() && !gpu->m_queues[id].front().blocked) {
							compute_runnable++;
						}
					}
				}
				auto& queue = gpu->m_queues[static_cast<uint32_t>(selected_queue)];
				submission  = std::move(queue.front());
				queue.pop_front();
				gpu->m_submission_count--;
				gpu->m_next_queue = (static_cast<uint32_t>(selected_queue) + 1) % QueueCount;
				gpu->m_processing = true;
				has_submission    = true;
				if (submission.queue_id == 0) {
					graphics_queued = graphics_queued > 0 ? graphics_queued - 1 : 0;
				}
			}
		}
		if (should_stop) {
			overlap.Flush();
			gpu->m_gfx_cp->BufferWait();
			g_gpu_state  = nullptr;
			g_gpu_thread = false;
			return;
		}

		if (command) {
			EXIT_IF(g_current_processor != nullptr);
			{ Libs::Graphics::ProfileCpuScope profile(profile_scheduler, "guest_callback_cpu"); command(); }

			Common::LockGuard lock(gpu->m_queue_mutex);
			gpu->m_processing = false;
			if (gpu->m_commands.empty() && gpu->m_submission_count == 0) {
				gpu->m_idle.SignalAll();
			}
			continue;
		}

		EXIT_IF(!has_submission);
		const bool graphics_slice = submission.type == SubmissionType::Graphics;
		const bool compute_slice  = submission.type == SubmissionType::Compute;
		const bool retrying_wait  = submission.wait_blocked;
		if (!retrying_wait) {
			++g_slices_since_block;
		}
		submission.wait_blocked   = false;
		if (retrying_wait && graphics_slice) {
			overlap.CloseGfxWait();
		}
		if (!retrying_wait && (graphics_slice || compute_slice)) {
			overlap.NoteSlice(graphics_slice, compute_queued, compute_runnable, graphics_queued);
		}
		const bool complete = [&] {
			Libs::Graphics::ProfileCpuScope profile(profile_scheduler, "guest_process_slice");
			profile.SetGuestQueue(submission.queue_id);
#if defined(__APPLE__)
			// This C++ thread has no autorelease pool of its own: every autoreleased Metal object made while processing (command
			// buffers, descriptors, and through the command buffers every texture and buffer they reference) stayed alive for the
			// whole run, 50-100 GB per instance. Drain once per slice.
			struct AutoreleaseScope {
				void* pool = objc_autoreleasePoolPush();
				~AutoreleaseScope() { objc_autoreleasePoolPop(pool); }
			} autorelease_scope;
#endif
			return gpu->Process(submission);
		}();
		const bool wait_reg_mem = g_pm4_suspend_reason == Pm4SuspendReason::WaitRegMem;

		Common::LockGuard lock(gpu->m_queue_mutex);
		if (!complete) {
			submission.blocked = true;
			if (graphics_slice && wait_reg_mem) {
				bool compute_pending = false;
				for (uint32_t id = 1; id < QueueCount; id++) {
					if (!gpu->m_queues[id].empty()) {
						compute_pending = true;
						break;
					}
				}
				overlap.NoteGfxWait(!retrying_wait, compute_pending);
				submission.wait_blocked = true;
				overlap.OpenGfxWait();
			} else if (graphics_slice) {
				overlap.NoteGfxOther();
			} else if (compute_slice && wait_reg_mem) {
				overlap.NoteComputeWait();
			}
			gpu->m_queues[submission.queue_id].push_front(std::move(submission));
			gpu->m_submission_count++;
		} else {
			for (auto& queue: gpu->m_queues) {
				if (!queue.empty()) {
					queue.front().blocked = false;
				}
			}
		}
		gpu->m_processing = false;
		if (gpu->m_commands.empty() && gpu->m_submission_count == 0) {
			gpu->m_idle.SignalAll();
		}
	}
}

bool GuestGpu::Process(Submission& submission) {
	g_pm4_suspend_reason   = Pm4SuspendReason::None;
	const bool first_slice = !submission.started;
	auto&      cp          = GetProcessor(submission.queue_id);

	if (first_slice) {
		submission.started = true;
		cp.SetSubmitId(++m_submit_id);
		cp.ResetDeCe();
		cp.SetFlip({});
	}

	cp.BufferInit();
	bool complete = true;

	switch (submission.type) {
		case SubmissionType::Graphics: {
			bool progressed = false;
			submission.constant_complete |= submission.constant_commands.empty();
			for (;;) {
				bool round_progress = false;
				if (!submission.constant_complete) {
					submission.constant_complete =
					    cp.Process(submission.constant_execution, submission.constant_commands) ==
					    Pm4ProcessResult::Complete;
					round_progress |= submission.constant_execution.MadeProgress();
				}
				cp.SetCeComplete(submission.constant_complete);
				if (!submission.command_complete) {
					submission.command_complete =
					    cp.Process(submission.command_execution, submission.commands) ==
					    Pm4ProcessResult::Complete;
					round_progress |= submission.command_execution.MadeProgress();
				}
				progressed |= round_progress;
				complete = submission.command_complete && submission.constant_complete;
				if (complete || !round_progress) {
					break;
				}
			}
			if (progressed) {
				if (complete) {
					m_renderer.RunGarbageCollector();
				}
				cp.BufferFlush();
			} else if (complete) {
				m_renderer.RunGarbageCollector();
			}
			break;
		}
		case SubmissionType::Compute: {
			const auto      num_dw = static_cast<uint32_t>(submission.commands.size());
			const auto*     buffer = submission.commands.data();
			static uint32_t compute_batch_log_count = 0;
			if (first_slice && num_dw <= 128 && compute_batch_log_count++ < 32) {
				LOGF("compute direct batch: data=0x%016" PRIx64 ", num_dw=%" PRIu32 "\n",
				     reinterpret_cast<uint64_t>(buffer), num_dw);
				for (uint32_t i = 0; i < std::min<uint32_t>(num_dw, 16); i++) {
					LOGF("\t compute[%02" PRIu32 "] = 0x%08" PRIx32 "\n", i, buffer[i]);
				}
			}
			if (first_slice) {
				GraphicsDbgDumpDcb("cc", num_dw, buffer);
			}
			complete = cp.Process(submission.command_execution, submission.commands) ==
			           Pm4ProcessResult::Complete;
			if (submission.command_execution.MadeProgress()) {
				if (complete) {
					m_renderer.RunGarbageCollector();
				}
				cp.BufferFlush();
			} else if (complete) {
				m_renderer.RunGarbageCollector();
			}
			break;
		}
		case SubmissionType::FlipPreparation:
			g_flip_count.fetch_add(1, std::memory_order_relaxed);
			m_renderer.RunGarbageCollector();
			cp.PrepareCpuFlip(submission.flip_request_id);
			break;
		case SubmissionType::SuspendPoint:
			cp.EmitGlobalBarrier();
			m_renderer.GetCommandScheduler().DeferPriorityOperation(
			    [ready = m_suspend_point_ready] { ready->release(); });
			cp.BufferFlush();
			cp.Reset();
			break;
	}

	return complete;
}

Pm4ProcessResult CommandProcessor::Process(Pm4Execution&             execution,
                                           std::span<const uint32_t> commands) {
	KYTY_PROFILER_BLOCK("CommandProcessor::Process");
	EXIT_IF(g_current_execution != nullptr);
	EXIT_IF(commands.size() > UINT32_MAX);
	if (execution.m_buffer_stack.empty() && !commands.empty()) {
		execution.m_buffer_stack.push_back({commands});
	}
	execution.m_suspended     = false;
	execution.m_made_progress = false;

	struct ExecutionScope {
		ExecutionScope(CommandProcessor& processor, Pm4Execution& execution)
		    : previous_processor(g_current_processor), previous_execution(g_current_execution) {
			g_current_processor = &processor;
			g_current_execution = &execution;
		}
		~ExecutionScope() {
			g_current_processor = previous_processor;
			g_current_execution = previous_execution;
		}

		CommandProcessor* previous_processor;
		Pm4Execution*     previous_execution;
	} execution_scope(*this, execution);

	ProcessPm4(execution);
	return execution.m_buffer_stack.empty() ? Pm4ProcessResult::Complete
	                                        : Pm4ProcessResult::Blocked;
}

void CommandProcessor::ProcessIndirectBuffer(std::span<const uint32_t> commands, bool chain) {
	EXIT_IF(g_current_execution == nullptr);
	EXIT_IF(!g_current_execution->m_next_buffer.empty());
	g_current_execution->m_next_buffer = commands;
	g_current_execution->m_chain       = chain;
}

void CommandProcessor::SuspendPm4() {
	EXIT_IF(g_current_execution == nullptr);
	if (g_pm4_suspend_reason == Pm4SuspendReason::None) {
		g_pm4_suspend_reason = Pm4SuspendReason::Other;
	}
	g_current_execution->m_suspended = true;
}

void CommandProcessor::ProcessPm4(Pm4Execution& execution) {
	// Total packet-dispatch cost. This OVERLAPS the recording scopes, so the dispatch overhead
	// is cp_process_pm4 minus recording, not the raw figure.
	Libs::Graphics::ProfileCpuScope profile(GetScheduler(), "cp_process_pm4");
	while (!execution.m_buffer_stack.empty()) {
		if (g_gpu_state != nullptr) {
			g_gpu_state->ProcessCommands();
		}
		auto& cursor = execution.m_buffer_stack.back();
		EXIT_IF(cursor.offset_dw > cursor.commands.size());
		if (cursor.offset_dw == cursor.commands.size()) {
			execution.m_buffer_stack.pop_back();
			continue;
		}

		const auto* const packet        = cursor.commands.data() + cursor.offset_dw;
		const auto        total_dw      = static_cast<uint32_t>(cursor.commands.size());
		const auto        remaining_dw  = total_dw - cursor.offset_dw;
		const auto        packet_header = packet[0];
		const auto        opcode        = (packet_header >> 8u) & 0xffu;
		if (std::getenv("KYTY_TRACE_PM4") && Libs::Graphics::TraceWindow::Active()) {
			std::fprintf(stderr, "[Pm4] op=0x%02x len=%u\n", opcode, KYTY_PM4_LEN(packet_header));
		}
		EXIT_NOT_IMPLEMENTED(remaining_dw > total_dw);

		if (packet_header == 0x80000000u) {
			cursor.offset_dw++;
			execution.m_made_progress = true;
			continue;
		}

		EXIT_NOT_IMPLEMENTED(remaining_dw < 2);

		if (GraphicsRunDebugDumpEnabled()) {
			LOGF("CP packet: offset=0x%05" PRIx32 " cmd_id=0x%08" PRIx32 " op=0x%02" PRIx32
			     " len=%" PRIu32 "\n",
			     total_dw - remaining_dw, packet_header, opcode, KYTY_PM4_LEN(packet_header));
		}

		if ((packet_header & 1u) != 0 && ShouldSkipPredicatedPackets()) {
			auto packet_dw = KYTY_PM4_LEN(packet_header);
			EXIT_NOT_IMPLEMENTED(packet_dw == 0 || packet_dw > remaining_dw);
			static std::atomic<uint32_t> skip_log_count {0};
			if (skip_log_count.fetch_add(1) < 2048) {
				LOGF("\t predicated skip: op=0x%02" PRIx32 ", r=0x%02" PRIx32 ", len=%" PRIu32
				     ", packet=0x%016" PRIx64 ", cmd_id=0x%08" PRIx32 "\n",
				     opcode, KYTY_PM4_R(packet_header), packet_dw,
				     reinterpret_cast<uint64_t>(packet), packet_header);
			}
			if (opcode == Pm4::IT_NOP && KYTY_PM4_R(packet_header) == Pm4::R_RELEASE_MEM &&
			    packet_dw >= 7) {
				static std::atomic<uint32_t> log_count {0};
				if (log_count.fetch_add(1) < 128) {
					const auto dst = packet[3] | (static_cast<uint64_t>(packet[4]) << 32u);
					const auto val = packet[5] | (static_cast<uint64_t>(packet[6]) << 32u);
					LOGF("\t predicated skip: R_RELEASE_MEM dst=0x%016" PRIx64
					     ", value=0x%016" PRIx64 ", action=0x%08" PRIx32
					     ", gcr/data/int=0x%08" PRIx32 "\n",
					     dst, val, packet[1], packet[2]);
				}
			}
			cursor.offset_dw += packet_dw;
			execution.m_made_progress = true;
			continue;
		}

		auto handler = g_cp_op_func[opcode];

		if (handler == nullptr) {
			static std::atomic<uint32_t> s_unknown_pm4 {0};
			if (s_unknown_pm4.fetch_add(1) < 20) {
				std::fprintf(stderr, "[PM4] unknown opcode=0x%02x", opcode);
				for (uint32_t i = 0; i < 5 && i < remaining_dw; ++i) {
					std::fprintf(stderr, " %08" PRIx32, packet[i]);
				}
				std::fprintf(stderr, "\n");
			}
			const auto offset = total_dw - remaining_dw;
			LOGF("unknown PM4 packet: data=0x%016" PRIx64 ", num_dw=%" PRIu32
			     ", offset=0x%05" PRIx32 ", current=0x%016" PRIx64 "\n",
			     reinterpret_cast<uint64_t>(packet - offset), total_dw, offset,
			     reinterpret_cast<uint64_t>(packet));
			const auto  dump_begin = (offset > 8 ? offset - 8 : 0);
			const auto  dump_end   = std::min<uint32_t>(total_dw, offset + 16);
			auto* const base       = packet - offset;
			for (uint32_t i = dump_begin; i < dump_end; i++) {
				LOGF("\t%05" PRIx32 "%s %08" PRIx32 "\n", i, (i == offset ? ":" : " "), base[i]);
			}
			EXIT("unknown op\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32 "\n",
			     total_dw - remaining_dw, packet_header);
		}

		const auto packet_dw =
		    handler(*this, packet_header & ~1u, packet + 1, remaining_dw, total_dw) + 1;
		EXIT_IF(packet_dw > remaining_dw);
		if (execution.m_suspended) {
			return;
		}
		cursor.offset_dw += packet_dw;
		execution.m_made_progress = true;
		if (!execution.m_next_buffer.empty()) {
			// Chains and taken branches reuse the fetcher; only calls retain a return cursor.
			if (execution.m_chain) {
				cursor = {execution.m_next_buffer};
			} else {
				execution.m_buffer_stack.push_back({execution.m_next_buffer});
			}
			execution.m_next_buffer = {};
		}
	}
}

void CommandProcessor::SetIndexType(uint32_t index_type_and_size) {
	m_index_type_and_size = index_type_and_size & 0x3u;
}

void CommandProcessor::SetIndexBaseAddress(uint64_t index_base_addr) {
	m_index_base_addr = index_base_addr;
}

void CommandProcessor::SetIndexBufferSize(uint32_t index_buffer_size) {
	m_index_buffer_size = index_buffer_size;
}

void CommandProcessor::SetDrawIndirectArgsBaseAddress(uint64_t draw_indirect_args_base_addr) {
	m_draw_indirect_args_base_addr = draw_indirect_args_base_addr;
}

void CommandProcessor::SetDispatchIndirectArgsBaseAddress(
    uint64_t dispatch_indirect_args_base_addr) {
	m_dispatch_indirect_args_base_addr = dispatch_indirect_args_base_addr;
}

void CommandProcessor::SetNumInstances(uint32_t num_instances) {
	if (num_instances == 0) {
		num_instances = 1;
	} else if (num_instances > 65536) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("SetNumInstances: clamping insane num_instances %u (0x%08x) to 1\n",
			     num_instances, num_instances);
		}
		num_instances = 1;
	}

	m_num_instances = num_instances;
}

void CommandProcessor::SetPredication(uint32_t condition, uint32_t op, uint32_t wait_op,
                                      const volatile void* address, uint32_t count_in_dwords) {
	(void)count_in_dwords;
	uint64_t value = 0;

	switch (op) {
		case 0x00:
			m_predicate_skip = false;
			return;
		case 0x01: {
			EXIT_NOT_IMPLEMENTED(address == nullptr);
			// One begin/end pair per DB; bit 63 marks each counter ready.
			constexpr uint64_t ready_bit = 1ull << 63u;
			const auto* results = reinterpret_cast<const volatile uint64_t*>(address);
			for (uint32_t db = 0; db < 16u; db++) {
				const auto begin = results[db * 2u];
				const auto end   = results[db * 2u + 1u];
				if ((begin & end & ready_bit) == 0) {
					if (wait_op == 0) {
						SuspendPm4();
					} else {
						m_predicate_skip = false;
					}
					return;
				}
				value += end - begin;
			}
		} break;
		case 0x03:
			// The wait selector applies only to Z-pass query readiness.
			EXIT_NOT_IMPLEMENTED(address == nullptr);
			value = *reinterpret_cast<const volatile uint64_t*>(address);
			break;
		default: EXIT("unknown predication op: 0x%08" PRIx32 "\n", op);
	}
	switch (condition) {
		case 0x00: m_predicate_skip = (value != 0); break;
		case 0x01: m_predicate_skip = (value == 0); break;
		default: EXIT("unknown predication condition: 0x%08" PRIx32 "\n", condition);
	}
	if (op == 0x03) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 128) {
			LOGF("\t bool predication: addr=0x%016" PRIx64 ", value=0x%016" PRIx64
			     ", condition=%" PRIu32 ", skip=%u, wait_op=%" PRIu32 "\n",
			     reinterpret_cast<uint64_t>(address), value, condition,
			     m_predicate_skip ? 1u : 0u, wait_op);
		}
	}
}

void CommandProcessor::DrawIndex(DrawIndexArgs args) {
	if (std::getenv("KYTY_TRACE_EARLY") && Libs::Graphics::TraceWindow::Active()) { std::fprintf(stderr, "[CpDraw] DrawIndex\n"); }
	args.index_type_and_size = m_index_type_and_size;
	if (args.instance_count == 0 && args.offset_source != DrawOffsetSource::IndirectArgs) {
		args.instance_count = m_num_instances;
	}
	if (args.instance_count > 65536) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("DrawIndex: clamping insane instance_count %u to 1\n", args.instance_count);
		}
		args.instance_count = 1;
	}
	if (args.index_count > 1000000) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("DrawIndex: dropping insane index_count %u\n", args.index_count);
		}
		return;
	}
	if (GraphicsRunDebugDumpEnabled() && (args.base_vertex != 0 || args.first_instance != 0)) {
		LOGF("\t draw indexed offsets: base_vertex = %" PRId32 ", first_instance = %" PRIu32 "\n",
		     args.base_vertex, args.first_instance);
	}
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		if (m_sh_ctx.GetVs().es_regs.data_addr == 0 || args.index_count == 0) {
			if (std::getenv("KYTY_TRACE_EARLY") && Libs::Graphics::TraceWindow::Active()) {
				std::fprintf(stderr, "[MetalDrop] DrawIndex es_addr0=%d index_count=%u\n",
				             m_sh_ctx.GetVs().es_regs.data_addr == 0 ? 1 : 0, args.index_count);
			}
			return;
		}
		// VGT_INDX_OFFSET is added to every fetched index by the hardware (the Vulkan path passes it as the vertex offset).
		// KYTY_METAL_INDEX_OFFSET=1 (experiment) adds it to the Metal base vertex; KYTY_LOG_INDEX_OFFSET=1 reports draws that set it.
		static const bool s_add_index_offset = std::getenv("KYTY_METAL_INDEX_OFFSET") != nullptr;
		static const bool s_log_index_offset = std::getenv("KYTY_LOG_INDEX_OFFSET") != nullptr;
		const uint32_t    index_offset       = m_ucfg.GetIndexOffset();
		if (s_log_index_offset && index_offset != 0) {
			static std::atomic<uint32_t> s_n {0};
			const uint32_t n = s_n.fetch_add(1);
			if (n < 64 || (n & (n - 1)) == 0) {
				std::fprintf(stderr, "[IndexOffset] #%u index_offset=%u base_vertex=%d index_count=%u instances=%u\n", n, index_offset,
				             args.base_vertex, args.index_count, args.instance_count);
			}
		}
		const int32_t base_vertex = args.base_vertex + (s_add_index_offset ? static_cast<int32_t>(index_offset) : 0);
		Metal::MetalRenderer::Instance().DrawIndex(
		    m_submit_id, m_ctx, m_sh_ctx, m_ucfg, m_renderer.GetPipelineCache(),
		    m_ucfg.GetPrimType(), args.index_count, args.index_addr,
		    args.instance_count, args.index_type_and_size, base_vertex, args.first_instance);
		return;
	}
#endif
	m_renderer.GetRenderExecutor().DrawIndex(m_submit_id, CurrentBuffer(), args);
}

void CommandProcessor::DrawIndexOffset(uint32_t index_offset, uint32_t index_count) {
	uint64_t index_size = 0;
	switch (m_index_type_and_size) {
		case 0: index_size = 2; break;
		case 1: index_size = 4; break;
		case 2: index_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
	}

	auto* index_addr = reinterpret_cast<const void*>(
	    m_index_base_addr + static_cast<uint64_t>(index_offset) * index_size);

	DrawIndex({.index_count = index_count, .index_addr = index_addr});
}

void CommandProcessor::DrawIndirect(uint32_t data_offset, uint32_t draw_initiator, bool indexed) {
	{
		static std::atomic<uint32_t> s_di_calls {0}, s_di_indexed {0};
		const uint32_t n = s_di_calls.fetch_add(1) + 1;
		if (indexed) s_di_indexed.fetch_add(1);
		if (n % 200 == 0) {
			LOGF("[DiStats] calls=%u indexed=%u\n", n, s_di_indexed.load());
		}
	}
	Libs::Graphics::ProfileOperationScope origin("guest_draw_indirect");
	EXIT_NOT_IMPLEMENTED((draw_initiator & ~0x20u) != 2u);
	EXIT_NOT_IMPLEMENTED(m_draw_indirect_args_base_addr == 0);

#if defined(__APPLE__)
	if (Libs::Graphics::Metal::UseNativeMetalBackend()) {
		// A single indirect draw is a one-record batch. Keep all GPU-produced
		// arguments out of CPU count/instance checks, including a stale zero.
		DrawIndirectMulti(data_offset, 1, nullptr,
		                  indexed ? sizeof(DrawIndexedIndirectArgs) : sizeof(DrawIndirectArgs),
		                  draw_initiator, indexed);
		return;
	}
#endif

	const auto* args_addr =
	    reinterpret_cast<const void*>(m_draw_indirect_args_base_addr + data_offset);

	if (!indexed) {
		DrawIndirectArgs args {};
		std::memcpy(&args, args_addr, sizeof(args));
		if (args.instance_count > 65536) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1) < 16) {
				LOGF("DrawIndirect: clamping insane instance_count %u to 1\n", args.instance_count);
			}
			args.instance_count = 1;
		}
		DrawIndexAuto({.vertex_count   = args.vertex_count_per_instance,
		               .instance_count = args.instance_count,
		               .first_vertex   = args.start_vertex_location,
		               .first_instance = args.start_instance_location,
		               .offset_source  = DrawOffsetSource::IndirectArgs});
		return;
	}

	DrawIndexedIndirectArgs args {};
	std::memcpy(&args, args_addr, sizeof(args));

	uint64_t index_size = 0;
	switch (m_index_type_and_size) {
		case 0: index_size = 2; break;
		case 1: index_size = 4; break;
		case 2: index_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
	}

	auto* index_addr = reinterpret_cast<const void*>(
	    m_index_base_addr + static_cast<uint64_t>(args.start_index_location) * index_size);

	const uint32_t index_count =
	    (m_index_buffer_size != 0 ? std::min(args.index_count_per_instance, m_index_buffer_size)
	                              : args.index_count_per_instance);
	if (GraphicsRunDebugDumpEnabled() && index_count != args.index_count_per_instance) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
			LOGF("\t DrawIndexIndirect: clamped index_count from %" PRIu32 " to %" PRIu32
			     " using INDEX_BUFFER_SIZE\n",
			     args.index_count_per_instance, index_count);
		}
	}

	if (args.instance_count > 65536) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("DrawIndexIndirect: clamping insane instance_count %u to 1\n", args.instance_count);
		}
		args.instance_count = 1;
	}

	DrawIndex({.index_count    = index_count,
	           .index_addr     = index_addr,
	           .instance_count = args.instance_count,
	           .base_vertex    = static_cast<int32_t>(args.base_vertex_location),
	           .first_instance = args.start_instance_location,
	           .offset_source  = DrawOffsetSource::IndirectArgs});
}

void CommandProcessor::DrawIndirectMulti(uint32_t data_offset, uint32_t max_count_or_count,
                                         const volatile uint32_t* count_addr,
                                         uint32_t stride_in_bytes, uint32_t draw_initiator,
                                         bool indexed) {
	{
		static std::atomic<uint32_t> s_mdi_calls {0}, s_mdi_indexed {0}, s_mdi_counted {0};
		const uint32_t n = s_mdi_calls.fetch_add(1) + 1;
		if (indexed) s_mdi_indexed.fetch_add(1);
		if (count_addr != nullptr) s_mdi_counted.fetch_add(1);
		if (n % 200 == 0) {
			LOGF("[MdiStats] calls=%u indexed=%u with_count_addr=%u\n", n, s_mdi_indexed.load(), s_mdi_counted.load());
		}
	}
	Libs::Graphics::ProfileOperationScope origin("guest_draw_indirect_multi");
	EXIT_NOT_IMPLEMENTED((draw_initiator & ~0x20u) != 2u);
	EXIT_NOT_IMPLEMENTED(m_draw_indirect_args_base_addr == 0);

	if (max_count_or_count == 0) {
		return;
	}

	const auto args_size = indexed ? sizeof(DrawIndexedIndirectArgs) : sizeof(DrawIndirectArgs);
	EXIT_NOT_IMPLEMENTED(stride_in_bytes < args_size);

#if defined(__APPLE__)
	// Route native Metal before dereferencing the GPU-written count. A zero
	// still visible to the CPU must not discard draws produced earlier on the GPU.
	if (Libs::Graphics::Metal::UseNativeMetalBackend()) {
		constexpr uint32_t kMaxMultiDraw = 4096;
		uint32_t max_count = max_count_or_count;
		if (max_count > kMaxMultiDraw) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1) < 8) {
				LOGF("DrawIndirectMulti: capping max_count %u to %u\n", max_count, kMaxMultiDraw);
			}
			max_count = kMaxMultiDraw;
		}
		const uint32_t index_size = indexed ? (m_index_type_and_size == 1u ? 4u : 2u) : 0u;
		Libs::Graphics::Metal::SetPendingIndirectDraw({
		    .args_addr   = static_cast<uint64_t>(m_draw_indirect_args_base_addr + data_offset),
		    .count_addr  = reinterpret_cast<uint64_t>(count_addr),
		    .max_count   = max_count,
		    .stride      = stride_in_bytes,
		    .multi       = true,
		    .indexed     = indexed,
		    .index_base  = static_cast<uint64_t>(m_index_base_addr),
		    .index_bytes = indexed ? static_cast<uint64_t>(m_index_buffer_size != 0 ? m_index_buffer_size : (1u << 20)) * index_size : 0u,
		    .index_type  = m_index_type_and_size == 1u ? 1u : 0u,
		});
		if (indexed) {
			DrawIndex({.index_count    = 1,
			           .index_addr     = reinterpret_cast<const void*>(m_index_base_addr),
			           .instance_count = 1,
			           .base_vertex    = 0,
			           .first_instance = 0,
			           .offset_source  = DrawOffsetSource::IndirectArgs});
		} else {
			DrawIndexAuto({.vertex_count   = 1,
			               .instance_count = 1,
			               .first_vertex   = 0,
			               .first_instance = 0,
			               .offset_source  = DrawOffsetSource::IndirectArgs});
		}
		Libs::Graphics::Metal::SetPendingIndirectDraw({});
		return;
	}
#endif

	uint32_t draw_count = max_count_or_count;
	if (count_addr != nullptr) {
		draw_count = *count_addr;
		draw_count = std::min(draw_count, max_count_or_count);
	}
	if (draw_count == 0) return;

	uint64_t index_size = 0;
	if (indexed) {
		switch (m_index_type_and_size) {
			case 0: index_size = 2; break;
			case 1: index_size = 4; break;
			case 2: index_size = 1; break;
			default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
		}
	}

	for (uint32_t i = 0; i < draw_count; i++) {
		const auto args_addr = m_draw_indirect_args_base_addr + data_offset +
		                       static_cast<uint64_t>(i) * stride_in_bytes;

		if (!indexed) {
			auto* args = reinterpret_cast<const DrawIndirectArgs*>(args_addr);
			uint32_t instance_count = args->instance_count;
			if (instance_count > 65536) {
				static std::atomic<uint32_t> log_count {0};
				if (log_count.fetch_add(1) < 16) {
					LOGF("DrawIndirectMulti: clamping insane instance_count %u to 1\n", instance_count);
				}
				instance_count = 1;
			}
			DrawIndexAuto({.vertex_count   = args->vertex_count_per_instance,
			               .instance_count = instance_count,
			               .first_vertex   = args->start_vertex_location,
			               .first_instance = args->start_instance_location,
			               .offset_source  = DrawOffsetSource::IndirectArgs});
			continue;
		}

		auto* args = reinterpret_cast<const DrawIndexedIndirectArgs*>(args_addr);

		auto* index_addr = reinterpret_cast<const void*>(
		    m_index_base_addr + static_cast<uint64_t>(args->start_index_location) * index_size);

		const uint32_t index_count =
		    (m_index_buffer_size != 0
		         ? std::min(args->index_count_per_instance, m_index_buffer_size)
		         : args->index_count_per_instance);
		if (GraphicsRunDebugDumpEnabled() && index_count != args->index_count_per_instance) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
				LOGF("\t DrawIndexIndirectMulti: clamped index_count from %" PRIu32 " to %" PRIu32
				     " using INDEX_BUFFER_SIZE\n",
				     args->index_count_per_instance, index_count);
			}
		}

		uint32_t instance_count = args->instance_count;
		if (instance_count > 65536) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1) < 16) {
				LOGF("DrawIndexIndirectMulti: clamping insane instance_count %u to 1\n", instance_count);
			}
			instance_count = 1;
		}

		DrawIndex({.index_count    = index_count,
		           .index_addr     = index_addr,
		           .instance_count = instance_count,
		           .base_vertex    = static_cast<int32_t>(args->base_vertex_location),
		           .first_instance = args->start_instance_location,
		           .offset_source  = DrawOffsetSource::IndirectArgs});
	}
}

void CommandProcessor::DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y,
                                      uint32_t thread_group_z, uint32_t mode) {
#if defined(__APPLE__)
	Libs::Graphics::Metal::CountDispatchStat(0);
#endif
	m_sh_ctx.SetCsWaveSize(Pm4::ComputeWaveSize(mode));

	uint32_t frame_num = 0;

	{
		frame_num = m_renderer.DiagnosticFrameNum();
		if (GraphicsRunDebugDumpEnabled()) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 1024) {
				const auto& cs = m_sh_ctx.GetCs().cs_regs;
				const auto& oa = m_ucfg.GetGdsOaCounter(m_ucfg.GetGdsOaState().GetIndex());
				LOGF("QueuePoint DispatchDirect: frame=%u submit=%" PRIu64
				     " groups=%ux%ux%u local=%ux%ux%u mode=0x%08" PRIx32 " wave=%u cs=0x%016" PRIx64
				     " oa_index=%u oa_enabled=%s oa_addr=0x%04" PRIx32 " oa_space=0x%08" PRIx32
				     "\n",
				     frame_num, m_submit_id, thread_group_x, thread_group_y, thread_group_z,
				     std::max(cs.num_thread_x, 1u), std::max(cs.num_thread_y, 1u),
				     std::max(cs.num_thread_z, 1u), mode, static_cast<uint32_t>(cs.wave_size),
				     cs.data_addr, m_ucfg.GetGdsOaState().GetIndex(),
				     oa.IsCounterEnabled() ? "true" : "false", oa.GetAddressBytes(),
				     oa.GetSpaceAvailable());
			}
		}

#if defined(__APPLE__)
		if (Metal::UseNativeMetalBackend()) {
			Metal::MetalRenderer::Instance().DispatchDirect(
			    m_submit_id, m_ctx, m_sh_ctx, m_renderer.GetPipelineCache(),
			    thread_group_x, thread_group_y, thread_group_z, mode);
			return;
		}
#endif
		m_renderer.GetRenderExecutor().DispatchDirect(m_submit_id, CurrentBuffer(), thread_group_x,
		                                              thread_group_y, thread_group_z, mode);
	}
}

void CommandProcessor::DispatchIndirect(uint64_t args_addr, uint32_t mode) {
#if defined(__APPLE__)
	Libs::Graphics::Metal::CountDispatchStat(0);
#endif
	Libs::Graphics::ProfileOperationScope origin("guest_dispatch_indirect");
	EXIT_NOT_IMPLEMENTED(args_addr == 0 || (args_addr & 3u) != 0);
	m_sh_ctx.SetCsWaveSize(Pm4::ComputeWaveSize(mode));
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		Metal::MetalRenderer::Instance().DispatchIndirect(
		    m_submit_id, m_ctx, m_sh_ctx, m_renderer.GetPipelineCache(),
		    args_addr, mode);
		return;
	}
#endif
	if ((mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0) {
		const auto* args = reinterpret_cast<const vk::DispatchIndirectCommand*>(args_addr);
		DispatchDirect(args->x, args->y, args->z, mode);
		return;
	}
	m_renderer.GetRenderExecutor().DispatchIndirect(m_submit_id, CurrentBuffer(), args_addr, mode);
}

void CommandProcessor::DrawIndexAuto(DrawAutoArgs args) {
	if (std::getenv("KYTY_TRACE_PSUD") != nullptr) {
		static std::atomic<uint32_t> s_psud {0};
		if (s_psud.fetch_add(1) < 6) {
			const auto& ps = m_sh_ctx.GetPs().ps_regs;
			std::fprintf(stderr, "[PsUd] draw#%u ps.data=0x%llx ps.user_data_addr=0x%llx vs.es.data=0x%llx\n",
			             s_psud.load(), (unsigned long long)ps.data_addr, (unsigned long long)ps.user_data_addr,
			             (unsigned long long)m_sh_ctx.GetVs().es_regs.data_addr);
		}
	}
	if (std::getenv("KYTY_TRACE_EARLY") && Libs::Graphics::TraceWindow::Active()) { std::fprintf(stderr, "[CpDraw] DrawAuto\n"); }
	if (args.instance_count == 0 && args.offset_source != DrawOffsetSource::IndirectArgs) {
		args.instance_count = m_num_instances;
	}
	if (args.instance_count > 65536) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("DrawIndexAuto: clamping insane instance_count %u to 1\n", args.instance_count);
		}
		args.instance_count = 1;
	}
	if (args.vertex_count > 1000000) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("DrawIndexAuto: dropping insane vertex_count %u\n", args.vertex_count);
		}
		return;
	}
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		if (m_sh_ctx.GetVs().es_regs.data_addr == 0 || args.vertex_count == 0) {
			if (std::getenv("KYTY_TRACE_EARLY") && Libs::Graphics::TraceWindow::Active()) {
				std::fprintf(stderr, "[MetalDrop] DrawAuto es_addr0=%d vertex_count=%u\n",
				             m_sh_ctx.GetVs().es_regs.data_addr == 0 ? 1 : 0, args.vertex_count);
			}
			return;
		}
		Metal::MetalRenderer::Instance().DrawAuto(
		    m_submit_id, m_ctx, m_sh_ctx, m_ucfg, m_renderer.GetPipelineCache(),
		    m_ucfg.GetPrimType(), args.vertex_count, args.instance_count,
		    args.first_vertex, args.first_instance);
		return;
	}
#endif
	m_renderer.GetRenderExecutor().DrawAuto(m_submit_id, CurrentBuffer(), args);
}

void CommandProcessor::WaitFlipDone(uint32_t video_out_handle, uint32_t display_buffer_index) {
	BufferFlush("cp_flip_wait");
	Libs::Graphics::ProfileCpuScope profile(GetScheduler(), "video_flip_done_wait", display_buffer_index);

	m_renderer.GetVideoOut().WaitFlipDone(static_cast<int>(video_out_handle),
	                                      static_cast<int>(display_buffer_index));
}

// A guest EOP label tells the guest that the GPU finished everything before it. Written at
// record time (the old behaviour), the guest runs ahead of the host GPU and its next read of
// GPU-written memory faults and drains the queue. Deferred, the value lands from the priority
// thread once the submission completes, so the guest's own wait provides the ordering.
// Labels outside the direct-memory backing keep the record-time write.
static std::atomic<int64_t>  g_eop_labels_in_flight {0};
static std::atomic<uint64_t> g_eop_labels_deferred {0};
static std::atomic<uint64_t> g_eop_labels_immediate {0};
static std::atomic<uint64_t> g_eop_labels_gpu {0};
static std::atomic<uint64_t> g_eop_label_flushes {0};

// KYTY_LABEL_FLUSH (GPU labels only): lazy (default) submits a GPU label only when a guest read
// can wait on it; always submits after every GPU label; never adds no submit.
enum class LabelFlushMode : uint8_t { Lazy, Always, Never };
static LabelFlushMode LabelFlushModeFromEnv() {
	static const LabelFlushMode mode = []() -> LabelFlushMode {
		const char* value = std::getenv("KYTY_LABEL_FLUSH");
		if (value != nullptr && std::strcmp(value, "always") == 0) return LabelFlushMode::Always;
		if (value != nullptr && std::strcmp(value, "never") == 0) return LabelFlushMode::Never;
		return LabelFlushMode::Lazy;
	}();
	return mode;
}

// `recorded` is BufferCache::TryRecordGpuLabelWrite's result (1 observable, 2 must submit).
static bool LabelNeedsFlush(int recorded) {
	switch (LabelFlushModeFromEnv()) {
		case LabelFlushMode::Always: return true;
		case LabelFlushMode::Never: return false;
		case LabelFlushMode::Lazy: return recorded == 2 && !LabelCoalesceEnabled();
	}
	return true;
}

// The native Metal backend publishes every end-of-pipe label when the work before it has finished, not when the packet is read. Written at
// record time (the old behaviour: "immediate=N gpu_labels=0" in the [eop] line) the game saw each submission as finished at once and reused its
// transient memory (indirect-dispatch arguments, ring buffers) while Metal still had the earlier work queued: the arguments the GPU then read
// were another frame's data (hundreds of zeroed dispatches per run; 0 in six runs with deferral). KYTY_EOP_DEFERRED=0 restores the old path.
static bool EopDeferredForMetal() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_EOP_DEFERRED");
		return Metal::UseNativeMetalBackend() && !(value != nullptr && value[0] == '0');
	}();
	return enabled;
}

// Returns 0 when the label was written on the CPU (now or on completion), otherwise the GPU-write
// result from BufferCache::TryRecordGpuLabelWrite.
static int PublishEopLabel(Libs::Graphics::BufferCache& cache,
                           Libs::Graphics::CommandBuffer& command,
                           Libs::Graphics::CommandScheduler& scheduler, void* dst, uint64_t value,
                           uint32_t size, const void* owner) {
	const auto address = reinterpret_cast<uint64_t>(dst);
	uint64_t   probe   = 0;
	// Any write here supersedes an earlier GPU label at this address; a GPU record below re-adds it.
	g_pending_gpu_labels.erase(address);
	// Once any label is deferred, later ones follow the same queue so values never regress.
	const bool deferred =
	    (Libs::Graphics::DebugEopDeferred() || EopDeferredForMetal() ||
	     g_eop_labels_in_flight.load(std::memory_order_acquire) > 0) &&
	    Libs::LibKernel::Memory::TryReadBacking(address, &probe, size);
	{
		static std::atomic<int64_t> next_print {0};
		const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
		                     std::chrono::steady_clock::now().time_since_epoch()).count();
		auto expected = next_print.load(std::memory_order_relaxed);
		if (now >= expected && next_print.compare_exchange_strong(expected, now + 4000)) {
			static uint64_t last_submits = 0;
			static uint64_t last_flips   = 0;
			const auto      submits      = Libs::Graphics::SubmitCountTotal();
			const auto      flips        = Libs::VideoOut::VideoOutDriver::FlipsCompletedTotal();
			const auto      d_submits    = submits - last_submits;
			const auto      d_flips      = flips - last_flips;
			last_submits                 = submits;
			last_flips                   = flips;
			const auto deferred_count    = g_eop_labels_deferred.exchange(0);
			const auto immediate_count   = g_eop_labels_immediate.exchange(0);
			const auto gpu_count         = g_eop_labels_gpu.exchange(0);
			std::fprintf(stderr,
			             "[eop] labels deferred=%llu immediate=%llu gpu_labels=%llu cpu_labels=%llu label_flushes=%llu"
			             " cbs=%llu flips=%llu cbs_per_flip=%.1f\n",
			             static_cast<unsigned long long>(deferred_count),
			             static_cast<unsigned long long>(immediate_count),
			             static_cast<unsigned long long>(gpu_count),
			             static_cast<unsigned long long>(deferred_count + immediate_count),
			             static_cast<unsigned long long>(g_eop_label_flushes.exchange(0)),
			             static_cast<unsigned long long>(d_submits), static_cast<unsigned long long>(d_flips),
			             d_flips != 0 ? static_cast<double>(d_submits) / static_cast<double>(d_flips) : 0.0);
			std::fprintf(stderr,
			             "[flush] cp_buffer_flush=%llu cp_flush_eop=%llu cp_flip_wait=%llu cp_suspend_point=%llu"
			             " cp_wait_regmem=%llu budget=%llu idle=%llu other=%llu\n",
			             static_cast<unsigned long long>(g_flush_by_reason[0].exchange(0)),
			             static_cast<unsigned long long>(g_flush_by_reason[1].exchange(0)),
			             static_cast<unsigned long long>(g_flush_by_reason[2].exchange(0)),
			             static_cast<unsigned long long>(g_flush_by_reason[3].exchange(0)),
			             static_cast<unsigned long long>(g_flush_by_reason[4].exchange(0)),
			             static_cast<unsigned long long>(g_flush_by_reason[5].exchange(0)),
			             static_cast<unsigned long long>(g_flush_by_reason[6].exchange(0)),
			             static_cast<unsigned long long>(g_flush_by_reason[7].exchange(0)));
		}
	}
	const bool cpu_label = CpuLabelsEnabled() && g_gpu_pinned_labels.count(address) == 0;
	if (cpu_label) {
		if (cache.DeferCpuLabel(address, value, size)) {
			g_eop_labels_deferred.fetch_add(1, std::memory_order_relaxed);
			return 0;
		}
	} else if (g_eop_labels_in_flight.load(std::memory_order_acquire) == 0) {
		// Values written by the GPU in order cannot regress behind a CPU write still in flight.
		const int recorded = cache.TryRecordGpuLabelWrite(
		    command, address, &value, size, LabelFlushModeFromEnv() == LabelFlushMode::Lazy);
		if (recorded != 0) {
			g_eop_labels_gpu.fetch_add(1, std::memory_order_relaxed);
			if (g_pending_gpu_labels.size() >= 4096) {
				for (auto it = g_pending_gpu_labels.begin(); it != g_pending_gpu_labels.end();) {
					it = scheduler.IsFree(it->second.tick) ? g_pending_gpu_labels.erase(it) : std::next(it);
				}
			}
			g_pending_gpu_labels[address] = {owner, value, scheduler.CurrentTick(), size};
			return recorded;
		}
	}
	if (!deferred) {
		g_eop_labels_immediate.fetch_add(1, std::memory_order_relaxed);
		std::memcpy(dst, &value, size);
		return 0;
	}
	g_eop_labels_deferred.fetch_add(1, std::memory_order_relaxed);
	g_eop_labels_in_flight.fetch_add(1, std::memory_order_acq_rel);
	scheduler.DeferPriorityOperation([address, value, size] {
		Libs::LibKernel::Memory::WriteBacking(address, &value, size);
		g_eop_labels_in_flight.fetch_sub(1, std::memory_order_acq_rel);
	});
	return 0;
}

template <typename T>
void CommandProcessor::WriteAtEndOfPipe(uint32_t cache_policy, uint32_t event_write_dest,
                                        uint32_t eop_event_type, uint32_t cache_action,
                                        uint32_t event_index, uint32_t event_write_source,
                                        void* dst_gpu_addr, T value, uint32_t interrupt_selector,
                                        uint32_t interrupt_context_id) {
	static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));

	auto& command = CurrentBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		const auto bits      = static_cast<unsigned>(sizeof(T) * 8u);
		const auto log_width = static_cast<int>(sizeof(T) * 2u);

		LOGF("CommandProcessor::WriteAtEndOfPipe%u()\n"
		     "\t cache_policy        = 0x%08" PRIx32 "\n"
		     "\t event_write_dest    = 0x%08" PRIx32 "\n"
		     "\t eop_event_type      = 0x%08" PRIx32 "\n"
		     "\t cache_action        = 0x%08" PRIx32 "\n"
		     "\t event_index         = 0x%08" PRIx32 "\n"
		     "\t event_write_source  = 0x%08" PRIx32 "\n"
		     "\t interrupt_selector  = 0x%08" PRIx32 "\n"
		     "\t interrupt_context   = 0x%08" PRIx32 "\n"
		     "\t dst_gpu_addr        = 0x%016" PRIx64 "\n"
		     "\t value               = 0x%0*" PRIx64 "\n",
		     bits, cache_policy, event_write_dest, eop_event_type, cache_action, event_index,
		     event_write_source, interrupt_selector, interrupt_context_id,
		     reinterpret_cast<uint64_t>(dst_gpu_addr), log_width, static_cast<uint64_t>(value));
	}

	EXIT_NOT_IMPLEMENTED(cache_policy != 0x00000000);
	EXIT_NOT_IMPLEMENTED(event_write_dest != 0x00000000);

	bool with_interrupt = false;
	switch (interrupt_selector) {
		case 0x00:
		case 0x03: with_interrupt = false; break;
		case 0x01:
			if (!IsAsyncComputeQueue()) {
				Sync::TriggerEopEventAtEndOfPipe(command, m_interrupt_event_id,
				                                 interrupt_context_id);
				return;
			}
			with_interrupt = true;
			break;
		case 0x02: with_interrupt = true; break;
		default: EXIT("unknown interrupt_selector\n");
	}

	auto write32 = [&](bool with_writeback) {
		auto* dst  = static_cast<uint32_t*>(dst_gpu_addr);
		auto  data = static_cast<uint32_t>(value);
		const int gpu_label =
		    PublishEopLabel(m_renderer.GetBufferCache(), command, GetScheduler(), dst, data, sizeof(data), this);

		if (with_interrupt) {
			if (with_writeback) {
				Sync::WriteAtEndOfPipeWithInterruptWriteBack32(m_submit_id, command, dst,
				                                               data, m_interrupt_event_id,
				                                               interrupt_context_id);
			} else {
				Sync::WriteAtEndOfPipeWithInterrupt32(m_submit_id, command, dst, data,
				                                      m_interrupt_event_id, interrupt_context_id);
			}
		} else if (with_writeback) {
			Sync::WriteAtEndOfPipeWithWriteBack32(m_submit_id, command, dst, data);
		} else {
			Sync::WriteAtEndOfPipe32(m_submit_id, command, dst, data);
		}
		if (gpu_label != 0 && LabelNeedsFlush(gpu_label)) {
			g_eop_label_flushes.fetch_add(1, std::memory_order_relaxed);
			BufferFlush("cp_flush_eop");
		}
	};

	switch (event_write_source) {
		case 0x01:
			if constexpr (sizeof(T) == sizeof(uint32_t)) {
				if (eop_event_type == 0x2f && cache_action == 0x00 && event_index == 0x06) {
					auto* dst = static_cast<uint32_t*>(dst_gpu_addr);
					SynchronizeGpu();
					Sync::ReadGds(*m_renderer.GetBufferCache().GetGdsBuffer(), dst, value & 0xffffu,
					              value >> 16u);
					Sync::WriteAtEndOfPipeGds32(m_submit_id, command, dst, value & 0xffffu,
					                            value >> 16u);
					if (with_interrupt) {
						m_renderer.TriggerInterrupt(m_interrupt_event_id, interrupt_context_id);
					}
					return;
				}
			} else if (eop_event_type == 0x04 && cache_action == 0x00 && event_index == 0x05) {
				write32(false);
				return;
			}
			break;
		case 0x02:
		case 0x04:
			if constexpr (sizeof(T) == sizeof(uint32_t)) {
				if (event_write_source == 0x02 && eop_event_type == 0x2f && event_index == 0x06) {
					switch (cache_action) {
						case 0x00: write32(false); return;
						case 0x38: write32(true); return;
						default: break;
					}
				}
			} else {
				if (event_write_source == 0x04) {
					// Experiment (KYTY_EOP_CLOCK_FREEZE=1): every GPU timestamp of one presented frame carries the same
					// clock value, so a frame's measured GPU time is zero (probes whether dynamic resolution follows it).
					static const bool s_freeze = std::getenv("KYTY_EOP_CLOCK_FREEZE") != nullptr;
					if (s_freeze) {
						static std::atomic<uint64_t> s_frozen_flip {~0ull};
						static std::atomic<uint64_t> s_frozen_value {0};
						const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
						if (s_frozen_flip.exchange(flip) != flip) {
							s_frozen_value.store(Sync::ReadReferenceClock());
						}
						value = s_frozen_value.load();
					} else {
						value = Sync::ReadReferenceClock();
					}
				}
				auto write64 = [&](bool with_writeback) {
					auto* dst = static_cast<uint64_t*>(dst_gpu_addr);
					const int gpu_label = PublishEopLabel(m_renderer.GetBufferCache(), command,
					                                       GetScheduler(), dst,
					                                       static_cast<uint64_t>(value), sizeof(value), this);

					if (with_interrupt) {
						if (with_writeback) {
							Sync::WriteAtEndOfPipeWithInterruptWriteBack64(
							    m_submit_id, command, dst, value, m_interrupt_event_id,
							    interrupt_context_id);
						} else {
							Sync::WriteAtEndOfPipeWithInterrupt64(m_submit_id, command, dst,
							                                      value, m_interrupt_event_id,
							                                      interrupt_context_id);
						}
					} else if (with_writeback) {
						Sync::WriteAtEndOfPipeWithWriteBack64(m_submit_id, command, dst,
						                                      value);
					} else {
						Sync::WriteAtEndOfPipe64(m_submit_id, command, dst, value);
					}
					if (gpu_label != 0 && LabelNeedsFlush(gpu_label)) {
						g_eop_label_flushes.fetch_add(1, std::memory_order_relaxed);
						BufferFlush("cp_flush_eop");
					}
				};

				switch (cache_action) {
					case 0x00:
						switch (eop_event_type) {
							case 0x04:
								if (event_index == 0x05) {
									write64(false);
									return;
								}
								break;
							case 0x14:
							case 0x28:
							case 0x2f:
								if (event_index == 0x00) {
									write64(false);
									return;
								}
								break;
							case 0x2b:
							case 0x2d:
							case 0x30:
								if (event_index == 0x00 && !with_interrupt) {
									write64(false);
									return;
								}
								break;
							default: break;
						}
						break;
					case 0x38:
						switch (eop_event_type) {
							case 0x04:
							case 0x14:
							case 0x28:
								if (((eop_event_type == 0x04 || eop_event_type == 0x28) &&
								     event_index == 0x05) ||
								    (event_index == 0x00)) {
									write64(true);
									return;
								}
								break;
							case 0x2b:
							case 0x2d:
								if (event_index == 0x00 && !with_interrupt) {
									write64(true);
									return;
								}
								break;
							case 0x2f:
								if (event_index == 0x06 && !with_interrupt) {
									write64(true);
									return;
								}
								break;
							default: break;
						}
						break;
					case 0x3b:
						if (eop_event_type == 0x04 && event_index == 0x05 && with_interrupt) {
							write64(true);
							return;
						}
						break;
					default: break;
				}
			}
			break;
		default: break;
	}

	EXIT("unknown event type\n");
}

void CommandProcessor::WriteAtEndOfPipe32(uint32_t cache_policy, uint32_t event_write_dest,
                                          uint32_t eop_event_type, uint32_t cache_action,
                                          uint32_t event_index, uint32_t event_write_source,
                                          void* dst_gpu_addr, uint32_t value,
                                          uint32_t interrupt_selector,
                                          uint32_t interrupt_context_id) {
	WriteAtEndOfPipe(cache_policy, event_write_dest, eop_event_type, cache_action, event_index,
	                 event_write_source, dst_gpu_addr, value, interrupt_selector,
	                 interrupt_context_id);
}

void CommandProcessor::WriteAtEndOfPipe64(uint32_t cache_policy, uint32_t event_write_dest,
                                          uint32_t eop_event_type, uint32_t cache_action,
                                          uint32_t event_index, uint32_t event_write_source,
                                          void* dst_gpu_addr, uint64_t value,
                                          uint32_t interrupt_selector,
                                          uint32_t interrupt_context_id) {
	WriteAtEndOfPipe(cache_policy, event_write_dest, eop_event_type, cache_action, event_index,
	                 event_write_source, dst_gpu_addr, value, interrupt_selector,
	                 interrupt_context_id);
}

// Coalesce adjacent global barriers. Two identical all-commands full memory barriers with nothing
// recorded between them are equivalent to one, so a barrier is skipped when no command has been
// recorded since the last one. CommandBuffer::Handle() is the only route to the raw buffer, so it
// marks the flag and the test over-approximates in the safe direction: a Handle() call that records
// nothing costs a redundant barrier, never a missing one. FACT-0017 measured 89.8% of global
// barriers as adjacent duplicates (521222 barriers forming only 53006 runs, runs up to 124 long),
// each one also tearing down the render pass via EndRendering and taking the renderer mutex.
// KYTY_BARRIER_COALESCE=0 restores one barrier per guest flush packet.
static bool BarrierCoalesceEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_BARRIER_COALESCE");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

void CommandProcessor::EmitGlobalBarrier() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	Common::LockGuard lock(m_renderer.GetMutex());

	if (BarrierCoalesceEnabled() && !CurrentBuffer().RecordedSinceGlobalBarrier()) {
		return;
	}

	vk::MemoryBarrier2 barrier {};
	barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
	barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;

	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &barrier;
	GetScheduler().EndRendering();
	Libs::Graphics::ProfilePipelineBarrier2(CurrentBuffer().Handle(), dependency);
	// Cleared last: EndRendering() and Handle() above both mark the buffer as recorded.
	CurrentBuffer().ClearRecordedSinceGlobalBarrier();
}

void CommandProcessor::TriggerEopEventAtEndOfPipe(uint32_t interrupt_context_id) {
	Sync::TriggerEopEventAtEndOfPipe(CurrentBuffer(), m_interrupt_event_id, interrupt_context_id);
}

void CommandProcessor::TriggerEvent(uint32_t event_type, uint32_t event_index,
                                    uint64_t event_address) {
	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::TriggerEvent()\n"
		     "\t event_type  = 0x%08" PRIx32 "\n"
		     "\t event_index = 0x%08" PRIx32 "\n"
		     "\t address     = 0x%016" PRIx64 "\n",
		     event_type, event_index, event_address);
	}

	const auto valid_cache_event_index = event_index == 0x00000000 || event_index == 0x00000007;
	switch (event_type) {
		// CsPartialFlush, GsPartialFlush, PsPartialFlush.
		case 0x00000007:
		case 0x0000000f:
		case 0x00000010: EmitGlobalBarrier(); break;
		// CbDbDataWritebackInvalidate, CbDataWritebackInvalidate.
		case 0x00000016:
		case 0x00000031:
			if (!valid_cache_event_index) {
				EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type,
				     event_index);
			}
			EmitGlobalBarrier();
			break;
		// DbDataWritebackInvalidate, DbMetadataWritebackInvalidate, CbMetadataWritebackInvalidate.
		case 0x0000002a:
		case 0x0000002c:
		case 0x0000002e:
			if (!valid_cache_event_index) {
				EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type,
				     event_index);
			}
			EmitGlobalBarrier();
			break;
		case 0x0000000d:
		case 0x0000000e:
		case 0x00000012:
		case 0x00000017:
		case 0x00000018:
		case 0x00000019:
		case 0x0000001a:
		case 0x0000001b:
		case 0x00000038:
		case 0x0000003a:
			LOGF("\t temporary: ignoring unsupported event_write type 0x%08" PRIx32
			     ", index 0x%08" PRIx32 "\n",
			     event_type, event_index);
			break;
		case 0x00000039: {
			if (event_index != 0x00000001 || event_address == 0 || (event_address & 0x7u) != 0) {
				EXIT("invalid occlusion-counter dump: index=0x%08" PRIx32 ", address=0x%016" PRIx64
				     "\n",
				     event_index, event_address);
			}
			static std::once_flag warning_once;
			std::call_once(warning_once, [] {
				std::printf("Warning: game uses occlusion queries, which are currently treated as "
				            "always visible; GPU usage may be higher and FPS may be lower.\n");
			});

			// Until host occlusion queries are implemented, publish an always-visible result. The
			// PS5 layout contains one interleaved begin/end pair per DB, and bit 63 marks a result
			// ready.
			constexpr uint64_t ready_bit    = 1ull << 63u;
			constexpr uint64_t counter_mask = ready_bit - 1u;
			auto*              results      = reinterpret_cast<volatile uint64_t*>(event_address);
			const auto         value        = ready_bit | m_synthetic_occlusion_counter;
			for (uint32_t db = 0; db < 16u; db++) {
				results[db * 2u] = value;
			}
			m_synthetic_occlusion_counter = (m_synthetic_occlusion_counter + 1u) & counter_mask;
			break;
		}
		default:
			EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type, event_index);
	}
}

void CommandProcessor::Flip() {
	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::Flip()\n");
	}

	auto& command = CurrentBuffer();
	auto request = Sync::PrepareVideoOutFlip(command, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                         m_flip.flip_arg);
	Sync::WriteAtEndOfPipeOnlyFlip(m_submit_id, command, m_flip.handle, m_flip.index,
	                               m_flip.flip_mode, m_flip.flip_arg, request);
	m_renderer.TickFrame();
	GetScheduler().Flush();
}

void CommandProcessor::Flip(void* dst_gpu_addr, uint32_t value) {
	auto& command = CurrentBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::Flip()\n"
		     "\t dst_gpu_addr = 0x%016" PRIx64 "\n"
		     "\t value        = 0x%08" PRIx32 "\n",
		     reinterpret_cast<uint64_t>(dst_gpu_addr), value);
	}

	std::memcpy(dst_gpu_addr, &value, sizeof(value));
	auto request = Sync::PrepareVideoOutFlip(command, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                         m_flip.flip_arg);
	Sync::WriteAtEndOfPipeWithFlip32(m_submit_id, command, static_cast<uint32_t*>(dst_gpu_addr),
	                                 value, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                 m_flip.flip_arg, request);
	m_renderer.TickFrame();
	GetScheduler().Flush();
}

void CommandProcessor::FlipWithInterrupt(uint32_t eop_event_type, uint32_t cache_action,
                                         void* dst_gpu_addr, uint32_t value) {
	auto& command = CurrentBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::FlipWithInterrupt()\n"
		     "\t eop_event_type      = 0x%08" PRIx32 "\n"
		     "\t cache_action        = 0x%08" PRIx32 "\n"
		     "\t dst_gpu_addr        = 0x%016" PRIx64 "\n"
		     "\t value               = 0x%08" PRIx32 "\n",
		     eop_event_type, cache_action, reinterpret_cast<uint64_t>(dst_gpu_addr), value);
	}

	if (eop_event_type != 0x00000004 || cache_action != 0x00000038) {
		EXIT("unknown event type\n");
	}
	std::memcpy(dst_gpu_addr, &value, sizeof(value));
	auto request = Sync::PrepareVideoOutFlip(command, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                         m_flip.flip_arg);
	Sync::WriteAtEndOfPipeWithInterruptWriteBackFlip32(
	    m_submit_id, command, static_cast<uint32_t*>(dst_gpu_addr), value, m_flip.handle,
	    m_flip.index, m_flip.flip_mode, m_flip.flip_arg, request, m_interrupt_event_id);
	m_renderer.TickFrame();
	GetScheduler().Flush();
}

void CommandProcessor::PrepareCpuFlip(uint64_t request_id) {
	auto& command = CurrentBuffer();
	if (g_current_processor != nullptr) {
		EXIT("invalid graphics-thread CPU flip preparation\n");
	}
	struct ProcessorScope {
		explicit ProcessorScope(CommandProcessor& processor) { g_current_processor = &processor; }
		~ProcessorScope() { g_current_processor = nullptr; }
	};
	ProcessorScope processor_scope(*this);

	m_renderer.GetVideoOut().PrepareFlip(request_id, command);
	GetScheduler().DeferPriorityOperation(
	    [this, request_id] { m_renderer.GetVideoOut().CompleteFlip(request_id); });
	m_renderer.TickFrame();
	GetScheduler().Flush();
}

void CommandProcessor::SynchronizeGpu() {
	GetScheduler().Finish();
}

bool GuestGpu::IsGpuThread() noexcept {
	return g_gpu_thread;
}

} // namespace Libs::Graphics
