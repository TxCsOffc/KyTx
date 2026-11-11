#include "graphics/host_gpu/metal/metalPassTimer.h"
#include "graphics/host_gpu/metal/metalEnv.h"
#include <mach/mach.h>
#include <mach/task_info.h>
#include <algorithm>
#include <unistd.h>
#include <thread>
#include <deque>
#include <map>
#include <string>
#include <mutex>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <vector>
#include "graphics/host_gpu/metal/metalContext.h"
#include "graphics/host_gpu/metal/metalHostBudget.h"
#include "graphics/host_gpu/metal/metalOrder.h"
#include "graphics/host_gpu/metal/metalIndirect.h"
#include "common/logging/log.h"

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

namespace Libs::Graphics::Metal {
std::string MetalCacheReport();
}

namespace Libs::Graphics::Metal {

std::string KytyTakeCbOps();  // metalDraw.mm: op labels since the last commit (KYTY_MTL_GPU_TIME)
std::string KytyDescribeGds(); // metalRenderer.mm: the first GDS dwords (work-queue counters of a hung kernel)

void KytyBudgetedCommit(id commandBuffer) {
	HostBudget::Commit((id<MTLCommandBuffer>)commandBuffer);
}

MetalContext& MetalContext::Instance() {
	static MetalContext context;
	return context;
}

MetalContext::MetalContext() = default;

MetalContext::~MetalContext() {
	Shutdown();
}

bool MetalContext::Initialize() {
	std::lock_guard lock(m_mutex);
	if (m_initialized) {
		return true;
	}
	(void)HostBudget::Get(); // Validate policy and start the footprint monitor before GPU setup.

	@autoreleasepool {
		m_device = MTLCreateSystemDefaultDevice();
		if (!m_device) {
			LOGF_COLOR(Log::Color::BrightRed, "Metal: Failed to create default Metal device!\n");
			return false;
		}

		m_command_queue = [m_device newCommandQueue];
		if (!m_inflight_semaphore) {
			// KYTY_MAX_INFLIGHT_CBS=<n> (diagnostic): how many submitted-but-unfinished command buffers the CPU may run ahead.
			intptr_t inflight = kMaxInflightCommandBuffers;
			if (const char* value = std::getenv("KYTY_MAX_INFLIGHT_CBS"); value != nullptr && std::atoi(value) > 0) inflight = std::atoi(value);
			m_inflight_semaphore = dispatch_semaphore_create(inflight);
		}
		if (!m_command_queue) {
			LOGF_COLOR(Log::Color::BrightRed, "Metal: Failed to create command queue!\n");
			m_device = nil;
			return false;
		}

		QueryCapabilities();
		m_initialized = true;

		LOGF_COLOR(Log::Color::BrightGreen,
		           "Metal backend initialized: %s (Apple10: %s, Metal 4: %s, UMA: %s)\n",
		           m_capabilities.device_name.c_str(),
		           m_capabilities.supports_family_apple10 ? "Yes" : "No",
		           m_capabilities.supports_metal4 ? "Yes" : "No",
		           m_capabilities.has_unified_memory ? "Yes" : "No");
	}

	return true;
}

void MetalContext::Shutdown() {
	std::lock_guard lock(m_mutex);
	if (!m_initialized) {
		return;
	}

	WaitUntilIdle();
	EndCurrentEncoder();
	HostBudget::Finish();

	@autoreleasepool {
		m_current_command_buffer = nil;
		m_command_queue          = nil;
		m_device                 = nil;
	}

	m_initialized = false;
	LOGF("Metal backend shut down.\n");
}

void MetalContext::QueryCapabilities() {
	if (!m_device) {
		return;
	}

	m_capabilities.device_name = [m_device.name UTF8String] ? [m_device.name UTF8String] : "Unknown Metal Device";
	m_capabilities.has_unified_memory = [m_device hasUnifiedMemory];

	if (@available(macOS 13.0, *)) {
		m_capabilities.supports_metal3 = [m_device supportsFamily:MTLGPUFamilyMetal3];
	} else {
		m_capabilities.supports_metal3 = false;
	}

#if defined(MTLLanguageVersion4_0)
	m_capabilities.supports_metal4 = true;
#endif

	if (@available(macOS 11.0, *)) {
		m_capabilities.supports_family_apple7 = [m_device supportsFamily:MTLGPUFamilyApple7];
	}
	if (@available(macOS 12.0, *)) {
		m_capabilities.supports_family_apple8 = [m_device supportsFamily:MTLGPUFamilyApple8];
	}
	if (@available(macOS 14.0, *)) {
		m_capabilities.supports_family_apple9 = [m_device supportsFamily:MTLGPUFamilyApple9];
	}
	m_capabilities.supports_family_apple10 = [m_device supportsFamily:MTLGPUFamilyApple10];

	m_capabilities.supports_barycentrics = [m_device supportsShaderBarycentricCoordinates];
	m_capabilities.supports_argument_buffers_tier2 =
	    (m_device.argumentBuffersSupport == MTLArgumentBuffersTier2);
	m_capabilities.max_threads_per_threadgroup =
	    static_cast<uint32_t>(m_device.maxThreadsPerThreadgroup.width *
	                          m_device.maxThreadsPerThreadgroup.height *
	                          m_device.maxThreadsPerThreadgroup.depth);

	if (@available(macOS 12.0, *)) {
		m_capabilities.max_buffer_length_mb =
		    static_cast<uint32_t>(m_device.maxBufferLength / (1024 * 1024));
	} else {
		m_capabilities.max_buffer_length_mb = 256;
	}
}

static uint64_t GpuWatchdogCommitted(const std::string& ops, id<MTLCommandBuffer> command);
static void     GpuWatchdogCompleted(uint64_t id);

id<MTLCommandBuffer> MetalContext::GetCurrentCommandBuffer() {
	std::lock_guard lock(m_mutex);
	// KYTY_SYNC_ENCODERS=1 (diagnostic): every encoder gets a command buffer of its own that finishes before the next one is
	// recorded. A frame that is right only with this flag points at a missing dependency between encoders (guest memory is reached
	// through raw GPU addresses and through buffer-backed texture views of the same pages, which Metal's tracking cannot relate).
	static const bool s_sync_encoders = KYTY_ENV("KYTY_SYNC_ENCODERS") != nullptr;
	if (s_sync_encoders && m_current_command_buffer && m_command_buffer_handed_out && !m_active_render_encoder && !m_active_compute_encoder) {
		dispatch_semaphore_t semaphore = m_inflight_semaphore;
		const uint64_t wd_id = GpuWatchdogCommitted("sync-encoders: " + (KYTY_ENV("KYTY_MTL_GPU_TIME") != nullptr ? KytyTakeCbOps() : std::string()), m_current_command_buffer);
		[m_current_command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
			GpuWatchdogCompleted(wd_id);
			if (semaphore) dispatch_semaphore_signal(semaphore);
		}];
		PassTimerOnCommit(m_current_command_buffer);
		if (m_cb_order_event != nil) [m_current_command_buffer encodeSignalEvent:m_cb_order_event value:++m_cb_order_value];
		KytyBudgetedCommit(m_current_command_buffer);
		[m_current_command_buffer waitUntilCompleted];
		m_current_command_buffer = nil;
	}
	if (!m_current_command_buffer) {
		m_command_buffer_handed_out = false;
		if (!m_command_queue) {
			return nil;
		}
		// Backpressure: never let the number of submitted-but-unfinished command
		// buffers reach IOGPU's internal limit, where -commandBuffer blocks forever.
		if (m_inflight_semaphore) {
			dispatch_semaphore_wait(m_inflight_semaphore, DISPATCH_TIME_FOREVER);
		}
		// KYTY_MTL_FAULT_INFO=1: ask Metal for per-encoder execution status when a command buffer fails.
		if (KYTY_ENV("KYTY_MTL_FAULT_INFO") != nullptr) {
			MTLCommandBufferDescriptor* cbd = [[MTLCommandBufferDescriptor alloc] init];
			cbd.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
			m_current_command_buffer = [m_command_queue commandBufferWithDescriptor:cbd];
		} else {
			m_current_command_buffer = [m_command_queue commandBuffer];
		}
		// KYTY_CB_SERIAL=1 (experiment): each command buffer waits on the GPU for the previous one to finish (shared event), without
		// stalling the CPU. Tells "two command buffers overlap on the GPU and race on aliased guest memory" (gone with this) from "the
		// guest CPU overwrites data the GPU still reads" (still there), the two causes of frames that flicker between right and wrong.
		static const bool s_cb_serial = KYTY_ENV("KYTY_CB_SERIAL") != nullptr;
		if (s_cb_serial && m_current_command_buffer) {
			if (m_cb_order_event == nil) m_cb_order_event = [m_device newSharedEvent];
			if (m_cb_order_value != 0) [m_current_command_buffer encodeWaitForEvent:m_cb_order_event value:m_cb_order_value];
		}
		if (!m_current_command_buffer && m_inflight_semaphore) {
			dispatch_semaphore_signal(m_inflight_semaphore);
		}
	}
	m_command_buffer_handed_out = true;
	return m_current_command_buffer;
}

void MetalContext::SetActiveRenderEncoder(id<MTLRenderCommandEncoder> encoder) noexcept {
	if (m_active_render_encoder == encoder) {
		return;
	}
	if (m_active_render_encoder && encoder != nil) {
		OrderEnd(m_active_render_encoder); [m_active_render_encoder endEncoding];
	}
	if (m_active_compute_encoder && encoder != nil) {
		OrderEnd(m_active_compute_encoder); [m_active_compute_encoder endEncoding];
		m_active_compute_encoder = nil;
	}
	m_active_render_encoder = encoder;
	if (encoder != nil) ++m_encoder_serial;
}

void MetalContext::SetActiveComputeEncoder(id<MTLComputeCommandEncoder> encoder) noexcept {
	if (m_active_compute_encoder == encoder) {
		return;
	}
	if (m_active_compute_encoder && encoder != nil) {
		OrderEnd(m_active_compute_encoder); [m_active_compute_encoder endEncoding];
	}
	if (m_active_render_encoder && encoder != nil) {
		OrderEnd(m_active_render_encoder); [m_active_render_encoder endEncoding];
		m_active_render_encoder = nil;
	}
	m_active_compute_encoder = encoder;
	if (encoder != nil) ++m_encoder_serial;
}

void MetalContext::QueueEncoderWriteback(std::function<void(id)> writeback, uint64_t key) {
	if (key != 0) for (auto& pending:m_pending_writebacks) if (pending.key == key) { pending.encode=std::move(writeback); return; }
	m_pending_writebacks.push_back({key,std::move(writeback)});
}

void MetalContext::EndCurrentEncoder() {
	if (m_active_render_encoder) {
		OrderEnd(m_active_render_encoder); [m_active_render_encoder endEncoding];
		m_active_render_encoder = nil;
	}
	if (m_active_compute_encoder) {
		OrderEnd(m_active_compute_encoder); [m_active_compute_encoder endEncoding];
		m_active_compute_encoder = nil;
	}
	if (!m_pending_writebacks.empty()) {
		std::vector<PendingWriteback> writebacks;
		writebacks.swap(m_pending_writebacks);
		for (const auto& writeback: writebacks) writeback.encode(m_current_command_buffer);
	}
	if (!m_pending_fills.empty()) {
		std::vector<PendingBlitFill> fills;
		fills.swap(m_pending_fills);
		if (m_current_command_buffer) {
			id<MTLBlitCommandEncoder> blit = [m_current_command_buffer blitCommandEncoder];
				OrderBegin(blit);
			for (const auto& f: fills) {
				[blit fillBuffer:f.buffer range:NSMakeRange(static_cast<NSUInteger>(f.offset), static_cast<NSUInteger>(f.length)) value:f.value];
			}
			OrderEnd(blit);
				[blit endEncoding];
		}
	}
	if (!m_pending_texture_copies.empty()) {
		std::vector<PendingTextureCopy> copies;
		copies.swap(m_pending_texture_copies);
		if (m_current_command_buffer) {
			id<MTLBlitCommandEncoder> blit = [m_current_command_buffer blitCommandEncoder];
			OrderBegin(blit);
			for (const auto& c: copies) {
				[blit copyFromTexture:(id<MTLTexture>)c.src sourceSlice:0 sourceLevel:0 toTexture:(id<MTLTexture>)c.dst destinationSlice:0
				     destinationLevel:0 sliceCount:c.slices levelCount:c.levels];
			}
			OrderEnd(blit);
			[blit endEncoding];
		}
	}
}

void MetalContext::QueueTextureCopy(id src, id dst, uint32_t slices, uint32_t levels) {
	if (src == nil || dst == nil || slices == 0 || levels == 0) return;
	m_pending_texture_copies.push_back({src, dst, slices, levels});
}

void MetalContext::QueueBlitFill(id buffer, uint64_t offset, uint64_t length, uint8_t value) {
	if (buffer == nil || length == 0) return;
	for (const auto& f: m_pending_fills) {
		if (f.buffer == buffer && f.offset == offset && f.length == length && f.value == value) return;
	}
	m_pending_fills.push_back({buffer, offset, length, value});
}

static std::atomic<uint64_t> s_cb_committed {0};
static std::atomic<uint64_t> s_cb_completed {0};

// GPU watchdog: a command buffer that does not finish for KYTY_GPU_WATCHDOG_S seconds (default 20, 0 = off) means a runaway kernel. macOS
// then starves WindowServer and, after 120 s, panics the whole machine (observed 2026-10-26), so the emulator exits first.
static std::mutex                       s_wd_mutex;
struct WatchdogEntry {
	std::chrono::steady_clock::time_point when;
	id<MTLCommandBuffer>                   command;
	std::string                           ops; // what the command buffer holds (KYTY_MTL_GPU_TIME=1) and which path committed it
};
// Keyed by commit order; a buffer is removed by its own completion (not "the oldest one"), so every path that commits and waits can register.
static std::map<uint64_t, WatchdogEntry> s_wd_inflight;
static uint64_t                          s_wd_next = 1;
static uint64_t GpuWatchdogCommitted(const std::string& ops, id<MTLCommandBuffer> command) {
	static const double limit = [] {
		const char* v = std::getenv("KYTY_GPU_WATCHDOG_S");
		return v != nullptr ? std::atof(v) : 20.0;
	}();
	uint64_t watchdog_id = 0;
	{
		std::lock_guard<std::mutex> lock(s_wd_mutex);
		watchdog_id = s_wd_next++;
		s_wd_inflight[watchdog_id] = {std::chrono::steady_clock::now(), command, ops};
	}
	static const bool started = [&] {
		if (limit <= 0.0) return false;
		std::thread([] {
			// KYTY_GPU_WATCHDOG_GDS_S=<seconds>: sample GDS while an in-flight command buffer ages.
			const char* sample_env = std::getenv("KYTY_GPU_WATCHDOG_GDS_S");
			const double sample_interval = sample_env != nullptr ? std::atof(sample_env) : 0.0;
			uint64_t last_sample_id = 0;
			double last_sample = 0.0;
			for (;;) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				double      age = 0.0;
				uint64_t    oldest_id = 0;
				std::string ops;
				id<MTLCommandBuffer> oldest_command = nil;
				{
					std::lock_guard<std::mutex> lock(s_wd_mutex);
					if (!s_wd_inflight.empty()) {
						oldest_id = s_wd_inflight.begin()->first;
						age = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_wd_inflight.begin()->second.when).count();
						if (age > limit) { ops = s_wd_inflight.begin()->second.ops; oldest_command = s_wd_inflight.begin()->second.command; }
					}
				}
				if (oldest_id != last_sample_id) {
					last_sample_id = oldest_id;
					last_sample = 0.0;
				}
				if (sample_interval > 0.0 && age >= sample_interval && age - last_sample >= sample_interval) {
					std::fprintf(stderr, "[GpuWatchdog] age=%.1f GDS: %s\n", age, KytyDescribeGds().c_str());
					last_sample = age;
				}
				if (age > limit) {
					std::fprintf(stderr, "[GpuWatchdog] a command buffer has been running for %.0f s (limit %.0f s): exiting before the system watchdog fires\n", age, limit);
					if (oldest_command) std::fprintf(stderr,
					    "[GpuWatchdog] Metal status=%ld GPU=%.9f..%.9f kernel=%.9f..%.9f error=%s\n",
					    (long)oldest_command.status, oldest_command.GPUStartTime, oldest_command.GPUEndTime,
					    oldest_command.kernelStartTime, oldest_command.kernelEndTime,
					    oldest_command.error ? oldest_command.error.localizedDescription.UTF8String : "none");
					if (!ops.empty()) std::fprintf(stderr, "[GpuWatchdog] ops of the stuck command buffer: %s\n", ops.c_str());
					// The shared argument buffers still hold what the GPU wrote last: for a hung indirect op, its own arguments.
					std::fprintf(stderr, "[GpuWatchdog] indirect snapshots: %s\n", Libs::Graphics::Metal::KytyDescribeIndirectArguments().c_str());
					std::fprintf(stderr, "[GpuWatchdog] GDS: %s\n", KytyDescribeGds().c_str());
					std::fflush(stderr);
					_exit(3);
				}
			}
		}).detach();
		return true;
	}();
	(void)started;
	return watchdog_id;
}
static void GpuWatchdogCompleted(uint64_t id) {
	std::lock_guard<std::mutex> lock(s_wd_mutex);
	s_wd_inflight.erase(id);
}

// A command buffer some path commits and waits on itself (a helper copy, a readback, a dump): registered with the watchdog so a hang in it is
// named too. Call before commit. `what` says which path.
void KytyWatchCommandBuffer(id commandBuffer, const char* what) {
	if (commandBuffer == nil) return;
	const uint64_t id_ = GpuWatchdogCommitted(std::string(what), (id<MTLCommandBuffer>)commandBuffer);
	[(id<MTLCommandBuffer>)commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) { GpuWatchdogCompleted(id_); }];
}

void MetalContext::CommitCommandBuffer(bool wait) {
	std::lock_guard lock(m_mutex);
	EndCurrentEncoder();

	if (m_current_command_buffer) {
		dispatch_semaphore_t semaphore = m_inflight_semaphore;
		std::string cb_ops;
		if (KYTY_ENV("KYTY_MTL_GPU_TIME") != nullptr) {
			cb_ops = KytyTakeCbOps();
			m_current_command_buffer.label = [NSString stringWithUTF8String:cb_ops.c_str()];
		}
		const uint64_t wd_id = GpuWatchdogCommitted(cb_ops.empty() ? std::string("commit") : cb_ops, m_current_command_buffer);
		if (semaphore) {
			[m_current_command_buffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
				// KYTY_GPU_BUSY=1: share of wall time the GPU spent executing our command buffers, every ~3 s (is the frame GPU-bound?).
				// Overlapping buffers are merged: only the part after the latest end seen so far counts.
				static const bool s_busy = KYTY_ENV("KYTY_GPU_BUSY") != nullptr;
				if (s_busy && buffer.GPUEndTime > buffer.GPUStartTime) {
					static std::mutex s_busy_mutex;
					static double     s_window = 0.0, s_sum = 0.0, s_last_end = 0.0;
					std::lock_guard<std::mutex> busy_lock(s_busy_mutex);
					const double start = std::max(buffer.GPUStartTime, s_last_end);
					if (buffer.GPUEndTime > start) s_sum += buffer.GPUEndTime - start;
					s_last_end = std::max(s_last_end, buffer.GPUEndTime);
					if (s_window == 0.0) s_window = buffer.GPUStartTime;
					if (buffer.GPUEndTime - s_window >= 3.0) {
						std::fprintf(stderr, "[GpuBusy] %.0f%% of %.1f s\n", 100.0 * s_sum / (buffer.GPUEndTime - s_window), buffer.GPUEndTime - s_window);
						s_window = buffer.GPUEndTime;
						s_sum    = 0.0;
					}
				}
				// KYTY_MTL_GPU_TIME=<ms> sets the threshold (default 20 ms; "1" keeps the default).
				static const double s_gpu_time_min = [] {
					const char* v = KYTY_ENV("KYTY_MTL_GPU_TIME");
					const double ms = v != nullptr ? std::atof(v) : 0.0;
					return ms > 1.0 ? ms / 1000.0 : 0.02;
				}();
				if (KYTY_ENV("KYTY_MTL_GPU_TIME") != nullptr && buffer.GPUEndTime > buffer.GPUStartTime &&
				    buffer.GPUEndTime - buffer.GPUStartTime > s_gpu_time_min) {
					std::fprintf(stderr, "[GpuTime] command buffer %.1f ms ops: %s\n", (buffer.GPUEndTime - buffer.GPUStartTime) * 1000.0, buffer.label ? [buffer.label UTF8String] : "");
				}
				if (buffer.status == MTLCommandBufferStatusError) {
					static std::atomic<uint32_t> s_gpu_errors {0};
					const uint32_t count = s_gpu_errors.fetch_add(1);
					if (count < 10) {
						std::fprintf(stderr, "[Metal] command buffer error (%u): %s\n", count,
						             buffer.error ? [[buffer.error localizedDescription] UTF8String]
						                          : "unknown");
						// With KYTY_MTL_GPU_TIME=1 the label lists the ops of the buffer: the first error names the hung shader.
						if (count == 0 && buffer.label != nil && buffer.label.length != 0) {
							std::fprintf(stderr, "[Metal] ops of the failed command buffer: %.6000s\n", [buffer.label UTF8String]);
						}
					}
					// KYTY_MTL_FAULT_INFO=1: report which encoder faulted and its last signpost.
					if (count < 10 && buffer.error && KYTY_ENV("KYTY_MTL_FAULT_INFO") != nullptr) {
						NSArray* infos = buffer.error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
						for (id<MTLCommandBufferEncoderInfo> info in infos) {
							NSString* signposts = [[info debugSignposts] componentsJoinedByString:@" | "];
							std::fprintf(stderr, "[FaultInfo] encoder=%s state=%ld signposts=%s\n",
							             [[info label] UTF8String], (long)[info errorState],
							             [signposts UTF8String]);
						}
					}
				}
				GpuWatchdogCompleted(wd_id);
				s_cb_completed.fetch_add(1, std::memory_order_relaxed);
				dispatch_semaphore_signal(semaphore);
			}];
		}
		s_cb_committed.fetch_add(1, std::memory_order_relaxed);
		PassTimerOnCommit(m_current_command_buffer);
		if (m_cb_order_event != nil) [m_current_command_buffer encodeSignalEvent:m_cb_order_event value:++m_cb_order_value];
		KytyBudgetedCommit(m_current_command_buffer);
		if (wait) {
			[m_current_command_buffer waitUntilCompleted];
		}
		m_current_command_buffer = nil;
	}
}

void MetalContext::WaitUntilIdle() {
	EndCurrentEncoder();
	if (m_current_command_buffer) {
		dispatch_semaphore_t semaphore = m_inflight_semaphore;
		// The CPU waits here for everything recorded since the last commit (the CP's BufferWait): the watchdog must see this buffer too.
		const uint64_t wd_id = GpuWatchdogCommitted("WaitUntilIdle: " + (KYTY_ENV("KYTY_MTL_GPU_TIME") != nullptr ? KytyTakeCbOps() : std::string()), m_current_command_buffer);
		[m_current_command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
			GpuWatchdogCompleted(wd_id);
			if (semaphore) dispatch_semaphore_signal(semaphore);
		}];
		PassTimerOnCommit(m_current_command_buffer);
		if (m_cb_order_event != nil) [m_current_command_buffer encodeSignalEvent:m_cb_order_event value:++m_cb_order_value];
		KytyBudgetedCommit(m_current_command_buffer);
		[m_current_command_buffer waitUntilCompleted];
		m_current_command_buffer = nil;
	}
}

// KYTY_MEM_LOG=1: process footprint and Metal allocation every ~3 s (finds unbounded growth).
static void MemoryLogTick(uint64_t frame) {
	static const bool on = KYTY_ENV("KYTY_MEM_LOG") != nullptr;
	if (!on) return;
	static std::atomic<uint64_t> s_last_ns {0};
	const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
	uint64_t last = s_last_ns.load();
	if (now - last < 3000000000ull || !s_last_ns.compare_exchange_strong(last, now)) return;
	task_vm_info_data_t info {};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	double footprint_mb = 0;
	if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
		footprint_mb = static_cast<double>(info.phys_footprint) / (1024.0 * 1024.0);
	}
	id<MTLDevice> dev = MetalContext::Instance().GetDevice();
	const double alloc_mb = dev ? static_cast<double>([dev currentAllocatedSize]) / (1024.0 * 1024.0) : 0.0;
	std::fprintf(stderr, "[Mem] flip=%llu footprint=%.0f MB metal_alloc=%.0f MB%s\n", (unsigned long long)frame, footprint_mb, alloc_mb, Libs::Graphics::Metal::MetalCacheReport().c_str());
}

static std::atomic<bool> s_capture_requested {false};
static std::mutex s_capture_msg_mutex;
static std::string s_capture_msg;
static std::chrono::steady_clock::time_point s_capture_msg_until {};

static void SetCaptureMessage(const std::string& text, double seconds) {
	std::lock_guard<std::mutex> lock(s_capture_msg_mutex);
	s_capture_msg = text;
	s_capture_msg_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000.0));
}

void RequestGpuCapture() {
	s_capture_requested.store(true);
	SetCaptureMessage("CAPTURE REQUESTED (F9)", 10.0);
}

std::string GpuCaptureTitleSuffix() {
	std::lock_guard<std::mutex> lock(s_capture_msg_mutex);
	if (s_capture_msg.empty()) return {};
	if (std::chrono::steady_clock::now() > s_capture_msg_until) {
		s_capture_msg.clear();
		return {};
	}
	return "  *** " + s_capture_msg + " ***";
}

void GpuCaptureTick(uint64_t frame) {
	MemoryLogTick(frame);
	// KYTY_LOG_INFLIGHT=1: how far the GPU runs behind the encoder (committed - completed command buffers) at each presented frame.
	{
		static const bool s_on = KYTY_ENV("KYTY_LOG_INFLIGHT") != nullptr;
		static uint64_t s_max = 0, s_sum = 0, s_n = 0;
		if (s_on) {
			const uint64_t lag = s_cb_committed.load(std::memory_order_relaxed) - s_cb_completed.load(std::memory_order_relaxed);
			s_max = std::max(s_max, lag);
			s_sum += lag;
			++s_n;
			if (frame % 120 == 0) {
				std::fprintf(stderr, "[Inflight] flip=%llu committed=%llu completed=%llu lag_now=%llu lag_avg=%.1f lag_max=%llu\n",
				             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(s_cb_committed.load()),
				             static_cast<unsigned long long>(s_cb_completed.load()), static_cast<unsigned long long>(lag),
				             s_n != 0 ? static_cast<double>(s_sum) / static_cast<double>(s_n) : 0.0, static_cast<unsigned long long>(s_max));
				s_max = 0; s_sum = 0; s_n = 0;
			}
		}
	}
	// A comma-separated list still records separate one-frame documents, never a
	// continuous capture spanning the gaps. A single number keeps the old behavior.
	static std::vector<uint64_t> s_targets = [] {
		std::vector<uint64_t> targets;
		const char* v = KYTY_ENV("KYTY_GPU_CAPTURE");
		if (v == nullptr || *v == '-') return targets;
		while (*v) {
			char* end = nullptr;
			const uint64_t target = std::strtoull(v, &end, 10);
			if (end == v) break;
			if (target > 0) targets.push_back(target);
			v = end;
			while (*v == ',' || *v == ' ') ++v;
		}
		std::sort(targets.begin(), targets.end());
		targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
		return targets;
	}();
	static size_t s_next = 0;
	static bool s_running = false;
	// KYTY_GPU_CAPTURE_TRIGGER=<path>: while the game runs, creating that file captures the next presented frame (the file is removed).
	// Needs MTL_CAPTURE_ENABLED=1 at launch; lets a tester play a match and have a frame recorded on request.
	static const char* s_trigger_path = KYTY_ENV("KYTY_GPU_CAPTURE_TRIGGER");
	const bool hotkey_request = !s_running && s_capture_requested.exchange(false);
	if (hotkey_request || (s_trigger_path != nullptr && !s_running && access(s_trigger_path, F_OK) == 0)) {
		if (!hotkey_request) unlink(s_trigger_path);
		const uint64_t target = frame + 2;
		s_targets.push_back(target);
		std::fprintf(stderr, "[GpuCapture] trigger file seen at flip %llu: capturing frame %llu\n",
		             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(target));
	}
	if (s_next >= s_targets.size()) return;
	MTLCaptureManager* mgr = [MTLCaptureManager sharedCaptureManager];
	if (s_running) {
		if (frame < s_targets[s_next]) return;
		(void)MetalContext::Instance().GetCurrentCommandBuffer();
		MetalContext::Instance().CommitCommandBuffer(true);
		[mgr stopCapture];
		SetCaptureMessage("SAVED frame" + std::to_string(s_targets[s_next]) + ".gputrace", 30.0);
		std::fprintf(stderr, "[GpuCapture] stopped at flip %llu (window = frame %llu)\n",
		             static_cast<unsigned long long>(frame),
		             static_cast<unsigned long long>(s_targets[s_next]));
		s_running = false;
		++s_next;
	}
	while (s_next < s_targets.size() && s_targets[s_next] <= frame) {
		std::fprintf(stderr, "[GpuCapture] missed frame %llu at flip %llu\n",
		             static_cast<unsigned long long>(s_targets[s_next]),
		             static_cast<unsigned long long>(frame));
		++s_next;
	}
	if (s_next >= s_targets.size()) return;
	const uint64_t target = s_targets[s_next];
	// Guest frame N's draws run between the flip N-1 and flip N ticks, so the window opens at
	// flip N-1 and closes at flip N (after committing the pending command buffers).
	if (frame == target - 1) {
		// The renderer holds its encoding lock around this tick. Drain commands
		// recorded before the requested frame, so they cannot enter this capture.
		(void)MetalContext::Instance().GetCurrentCommandBuffer();
		MetalContext::Instance().CommitCommandBuffer(true);
		const char* dir_env = KYTY_ENV("KYTY_GPU_CAPTURE_DIR");
		NSString* dir = dir_env != nullptr ? [NSString stringWithUTF8String:dir_env]
		                                   : @"/Users/tx/Developer/fex-spike/runs";
		[[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
		NSString* path = [NSString stringWithFormat:@"%@/frame%llu.gputrace", dir,
		                  static_cast<unsigned long long>(target)];
		MTLCaptureDescriptor* desc = [[MTLCaptureDescriptor alloc] init];
		desc.captureObject = MetalContext::Instance().GetDevice();
		desc.destination = MTLCaptureDestinationGPUTraceDocument;
		desc.outputURL = [NSURL fileURLWithPath:path];
		NSError* err = nil;
		if ([mgr startCaptureWithDescriptor:desc error:&err]) {
			SetCaptureMessage("RECORDING frame " + std::to_string(target) + " - the game pauses while the trace is written", 600.0);
			s_running = true;
			std::fprintf(stderr, "[GpuCapture] started frame %llu -> %s\n",
			             static_cast<unsigned long long>(target), [path UTF8String]);
		} else {
			SetCaptureMessage("CAPTURE FAILED", 20.0);
			std::fprintf(stderr, "[GpuCapture] start failed: %s\n",
			             err ? [[err localizedDescription] UTF8String] : "unknown");
			++s_next;
		}
	}
}

// KYTY_VK_GPU_CAPTURE=<flip>: Metal device capture of the MoltenVK (Vulkan) build around one flip.
// Captures the default device (the same GPU MoltenVK submits to) from flip N to N+1, written to
// KYTY_GPU_CAPTURE_DIR/vk_f<N>.gputrace. Requires MTL_CAPTURE_ENABLED=1 at launch.
void VkGpuCaptureTick(uint64_t frame) {
	static const long long s_target = [] {
		const char* v = KYTY_ENV("KYTY_VK_GPU_CAPTURE");
		return v != nullptr ? std::atoll(v) : -1LL;
	}();
	static bool s_running = false;
	if (s_target < 0) return;
	MTLCaptureManager* mgr = [MTLCaptureManager sharedCaptureManager];
	if (!s_running && static_cast<long long>(frame) == s_target) {
		const char* dir_env = KYTY_ENV("KYTY_GPU_CAPTURE_DIR");
		NSString* dir = dir_env != nullptr ? [NSString stringWithUTF8String:dir_env]
		                                   : @"/Users/tx/Developer/fex-spike/runs/compare";
		[[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
		NSString* path = [NSString stringWithFormat:@"%@/vk_f%lld.gputrace", dir, s_target];
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		MTLCaptureDescriptor* desc = [[MTLCaptureDescriptor alloc] init];
		desc.captureObject = device;
		desc.destination = MTLCaptureDestinationGPUTraceDocument;
		desc.outputURL = [NSURL fileURLWithPath:path];
		NSError* err = nil;
		if ([mgr startCaptureWithDescriptor:desc error:&err]) {
			s_running = true;
			std::fprintf(stderr, "[VkGpuCapture] started flip %lld -> %s\n", s_target, [path UTF8String]);
		} else {
			std::fprintf(stderr, "[VkGpuCapture] start failed: %s\n",
			             err ? [[err localizedDescription] UTF8String] : "unknown");
		}
	} else if (s_running && static_cast<long long>(frame) == s_target + 1) {
		[mgr stopCapture];
		s_running = false;
		std::fprintf(stderr, "[VkGpuCapture] stopped at flip %llu\n", static_cast<unsigned long long>(frame));
	}
}

} // namespace Libs::Graphics::Metal
