#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/syncTiming.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#if defined(__APPLE__)
#include "graphics/host_gpu/metal/metalRenderer.h"
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <bit>
#include <charconv>

namespace Libs::Graphics {

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

namespace {

void ReportVulkanFatal(const char* what, vk::Result result, uint64_t tick, uint32_t debug_op,
                       uint64_t debug_submit, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                       uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	            debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
}

} // namespace

CommandScheduler::CommandPool::CommandPool(GraphicContext& graphics, MasterSemaphore& master)
    : m_graphics(graphics), m_master(master) {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	EXIT_IF(graphics.queue_family == static_cast<uint32_t>(-1));
	vk::CommandPoolCreateInfo create {};
	create.queueFamilyIndex = graphics.queue_family;
	create.flags            = vk::CommandPoolCreateFlagBits::eTransient |
	                          vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	const auto result       = graphics.device.createCommandPool(&create, nullptr, &m_pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_pool == nullptr);
}

CommandScheduler::CommandPool::~CommandPool() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	m_graphics.device.destroyCommandPool(m_pool, nullptr);
}

size_t CommandScheduler::CommandPool::Grow() {
	const auto first = m_ticks.size();
	m_ticks.resize(first + GrowStep);
	m_buffers.resize(first + GrowStep);

	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = m_pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = static_cast<uint32_t>(GrowStep);
	EXIT_IF(m_graphics.device.allocateCommandBuffers(&allocate, m_buffers.data() + first) !=
	        vk::Result::eSuccess);
	return first;
}

vk::CommandBuffer CommandScheduler::CommandPool::Commit() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return nullptr;
	}
#endif
	auto       gpu_tick = m_master.KnownGpuTick();
	const auto search   = [this, &gpu_tick](size_t begin, size_t end) -> std::optional<size_t> {
		for (size_t index = begin; index < end; ++index) {
			if (gpu_tick >= m_ticks[index]) {
				m_ticks[index] = m_master.CurrentTick();
				return index;
			}
		}
		return std::nullopt;
	};

	auto found = search(m_hint, m_ticks.size());
	if (!found) {
		m_master.Refresh();
		gpu_tick = m_master.KnownGpuTick();
		found    = search(m_hint, m_ticks.size());
	}
	if (!found) {
		found = search(0, m_hint);
	}
	if (!found) {
		found           = Grow();
		m_ticks[*found] = m_master.CurrentTick();
	}

	m_hint = (*found + 1) % m_ticks.size();
	return m_buffers[*found];
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics, const char* profile_role)
    : m_master(graphics), m_context(context), m_graphics(graphics),
      m_command_pool(graphics, m_master), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	const char* environment_path = std::getenv("KYTY_GPU_TIMING_CSV");
	if (environment_path == nullptr || *environment_path == '\0') return;
	m_profile_role = profile_role;
	static std::atomic<uint32_t> instance {0};
	const uint32_t id = instance++;
	std::string path_storage = environment_path;
	if (id) path_storage += ".scheduler-" + std::to_string(id) + ".csv";
	const char* path = path_storage.c_str();
	m_gpu_timers.resize(GpuTimerCount);
	if (FILE* metadata = std::fopen((path_storage + ".meta.csv").c_str(), "w")) {
		std::fprintf(metadata, "scheduler,role,queue_family,queue_handle,timer_pages,timer_page_size,timer_index_bits\n%u,%s,%u,0x%llx,%u,%u,%u\n", id,
		    profile_role, graphics.queue_family, std::bit_cast<uint64_t>(graphics.queue),
		    GpuTimerCount / GpuTimerPageSize, GpuTimerPageSize, GpuProfileTimerToken::IndexBits); std::fclose(metadata);
	}
	uint32_t count = 0;
	graphics.physical_device.getQueueFamilyProperties(&count, nullptr);
	std::vector<vk::QueueFamilyProperties> families(count);
	graphics.physical_device.getQueueFamilyProperties(&count, families.data());
	if (graphics.queue_family >= count || families[graphics.queue_family].timestampValidBits == 0)
		return;
	const auto bits = families[graphics.queue_family].timestampValidBits;
	m_timestamp_mask = bits >= 64 ? UINT64_MAX : (uint64_t {1} << bits) - 1;
	m_timestamp_period_ns = graphics.physical_device_properties.limits.timestampPeriod;
	vk::QueryPoolCreateInfo query_info {};
	query_info.queryType = vk::QueryType::eTimestamp;
	query_info.queryCount = GpuTimerCount * 2;
	if (graphics.device.createQueryPool(&query_info, nullptr, &m_gpu_query_pool) !=
	    vk::Result::eSuccess) return;
	m_gpu_timing_file = std::fopen(path, "w");
	if (m_gpu_timing_file == nullptr) {
		graphics.device.destroyQueryPool(m_gpu_query_pool, nullptr);
		m_gpu_query_pool = nullptr;
		return;
	}
	std::fprintf(m_gpu_timing_file,
	             "kind,frame,shader,tick,gpu_ms,debug_op,arg4,draws,computes,start_raw,end_raw,start_stage,end_stage,transaction,address,split,timestamp_period_ns,timestamp_mask\n");
	const auto cpu_path = std::string {path} + ".cpu.csv";
	m_cpu_timing_file = std::fopen(cpu_path.c_str(), "w");
	if (m_cpu_timing_file != nullptr) {
		std::fprintf(m_cpu_timing_file,
		             "frame,wall_ms,draws,computes,submits,submit_cpu_ms,waits,wait_cpu_ms,"
		             "draw_state_ms,draw_bindings_ms,draw_vertex_index_ms,draw_pipeline_ms,"
		             "draw_targets_ms,draw_commit_ms,begin_ns,end_ns,timer_drops,event_drops,barrier_records,broad_barrier_records\n");
	}
	auto number = [](const char* name, uint64_t fallback) {
		const char* text = std::getenv(name);
		if (!text || !*text) return fallback;
		uint64_t value = 0;
		const auto [end, error] = std::from_chars(text, text + std::strlen(text), value);
		return error == std::errc {} && *end == '\0' ? value : fallback;
	};
	m_profile_start = number("KYTY_GPU_PROFILE_START_FRAME", 0);
	m_profile_count = number("KYTY_GPU_PROFILE_FRAME_COUNT", UINT64_MAX);
	m_profile_detail_every = std::max<uint64_t>(1, number("KYTY_GPU_PROFILE_CPU_DETAIL_EVERY", 16));
	m_profile_fine_cpu = number("KYTY_GPU_PROFILE_FINE_CPU", 0) != 0;
	m_profile_lifetime = number("KYTY_GPU_PROFILE_LIFETIME", 0) != 0;
	if (m_profile_lifetime) {
		m_profile_watch_ranges = {{0x1164b80000ull, 512 * 1024}, {0x1140008000ull, 512 * 1024},
		                          {0x1167f00000ull, 512 * 1024}, {0x1165e00000ull, 512 * 1024}};
	}
	if (const auto* control = std::getenv("KYTY_GPU_PROFILE_CONTROL_FILE")) {
		m_profile_control_path = control;
		m_profile_start = UINT64_MAX;
	}
	m_profile_events_file = std::fopen((std::string(path) + ".events.csv").c_str(), "w");
	m_profile_submits_file = std::fopen((std::string(path) + ".submits.csv").c_str(), "w");
	if (m_profile_events_file) {
		std::setvbuf(m_profile_events_file, nullptr, _IOFBF, 1 << 20);
		std::fprintf(m_profile_events_file, "kind,frame,begin_ns,end_ns,thread,tick,transaction,address,resource,bytes,src_stage,dst_stage,src_access,dst_access,old_layout,new_layout,src_queue,dst_queue,guest_queue,file,line,function,context,probe_end_ns\n");
	}
	if (m_profile_submits_file) {
		std::setvbuf(m_profile_submits_file, nullptr, _IOFBF, 1 << 20);
		std::fprintf(m_profile_submits_file, "frame,tick,queue_family,queue_handle,thread,build_begin_ns,submit_begin_ns,queue_enter_ns,submit_end_ns,previous_submit_ns,known_idle_since_ns,prior_complete,draws,guest_computes,wait_dependencies,signal_dependencies,timer_page\n");
	}
	m_master.SetProfiler(this);
	SetProfileFrame(0);
	m_cpu_frame_start = std::chrono::steady_clock::now();
	const char* control_path = std::getenv("KYTY_GPU_TIMING_CONTROL_FILE");
	if (control_path != nullptr && *control_path != '\0') {
		m_gpu_control_path = control_path;
		m_gpu_timing_enabled = false;
	}
}

CommandScheduler::~CommandScheduler() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	Shutdown();
	FlushCpuFrame();
	FlushProfileEvents();
	if (ProfileThread().scheduler == this) ProfileThread() = {};
	if (m_profile_events_file) std::fclose(m_profile_events_file);
	if (m_profile_submits_file) std::fclose(m_profile_submits_file);
	if (m_cpu_timing_file != nullptr) std::fclose(m_cpu_timing_file);
	if (m_gpu_timing_file != nullptr) std::fclose(m_gpu_timing_file);
	if (m_gpu_query_pool != nullptr) m_graphics.device.destroyQueryPool(m_gpu_query_pool, nullptr);
}

void CommandScheduler::Shutdown() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	{
		std::unique_lock lock(m_operation_mutex);
		if (m_operation_state == OperationState::Closed) {
			return;
		}
		if (g_deferred_callback_scheduler == this) {
			EXIT_IF(m_operation_state == OperationState::Open);
			// A priority callback cannot join its own runner, while a normal callback can be
			// executing inside the shutdown owner's final PopPendingOperations. The owning
			// thread will finish shutdown after this callback returns.
			return;
		}
		if (m_operation_state == OperationState::Draining) {
			m_operation_available.wait(
			    lock, [this] { return m_operation_state == OperationState::Closed; });
			return;
		}
		m_operation_state = OperationState::Draining;
	}
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	CollectAllGpuTimers();
	PopPendingOperations();
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(!m_pending_operations.empty() || !m_priority_operations.empty() ||
		        m_priority_active);
		m_operation_state = OperationState::Closed;
	}
	m_operation_available.notify_all();
}

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config,
                             HW::Shader& shaders) {
	m_command.Bind(registers, user_config, shaders);
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
	}
	if (DrawCommitEnabled() && !DrawCommitOnWorker()) {
		Context().GetRenderExecutor().FinishDrawCommit();
	}

	if (m_command.IsInvalid()) {
		BeginNext();
	}
}

void CommandScheduler::BeginRendering(const RenderState& state) {
	Current().BeginRendering(state);
}

void CommandScheduler::EndRendering() {
	if (Active() && !m_command.IsInvalid()) {
		Current().EndRendering();
	}
}

void CommandScheduler::Flush() {
	SubmitInfo submit;
	Flush(submit);
}

void CommandScheduler::Flush(SubmitInfo& submit) {
	Submit(submit);
	BeginNext();
}

void CommandScheduler::FlushAndWait(std::source_location site) {
	const auto tick = Submit();
	WaitMaster(tick, site);
	BeginNext();
}

void CommandScheduler::Finish(std::source_location site) {
	CheckActive();
	if (!m_command.IsInvalid()) {
		Submit();
	}
	WaitMaster(CurrentTick() - 1, site);
	BeginNext();
	PopPendingOperations();
}

void CommandScheduler::Wait(uint64_t tick, std::source_location site) {
	EXIT_IF(tick > CurrentTick());
	if (tick == CurrentTick()) {
		CheckActive();
		// A stream-buffer wrap can wait while a draw is being prepared through a reference to
		// Current(). The wrapper stays stable while its pooled Vulkan buffer is retired. Deferred
		// resources are released only at the next GPU operation boundary.
		SyncReadbackSample* const sync = t_sync_sample;
		const uint64_t submit_begin = sync ? SyncTimingNowNs() : 0;
		const auto submitted_tick = Submit();
		if (sync) sync->submit_ns += SyncTimingNowNs() - submit_begin;
		EXIT_IF(submitted_tick != tick);
		WaitMaster(tick, site);
		BeginNext();
	} else {
		WaitMaster(tick, site);
	}
}

void CommandScheduler::PopPendingOperations() {
	m_master.Refresh();
	for (;;) {
		PendingOperation operation;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty() ||
			    !m_master.IsFree(m_pending_operations.front().tick)) {
				return;
			}
			operation = std::move(m_pending_operations.front());
			m_pending_operations.pop();
		}
		WaitPriorityOperations(operation.tick);
		RunOperation(std::move(operation.callback));
	}
}

void CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		m_pending_operations.push({std::move(operation), CurrentTick()});
		return;
	}
	if (g_deferred_callback_scheduler == this) {
		lock.unlock();
		operation();
		return;
	}
	m_operation_available.wait(lock,
	                           [this] { return m_operation_state == OperationState::Closed; });
	lock.unlock();
	operation();
}

void CommandScheduler::DeferOperationAfterTicks(Common::UniqueFunction<void>&& operation,
                                                uint32_t                       extra_ticks) {
	if (extra_ticks == 0) {
		DeferOperation(std::move(operation));
		return;
	}
	DeferOperation([this, operation = std::move(operation), extra_ticks]() mutable {
		if (Active()) {
			DeferOperationAfterTicks(std::move(operation), extra_ticks - 1);
		} else {
			operation();
		}
	});
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		m_priority_operations.push({std::move(operation), CurrentTick()});
		lock.unlock();
		m_operation_available.notify_one();
		return;
	}
	if (g_deferred_callback_scheduler == this) {
		lock.unlock();
		operation();
		return;
	}
	m_operation_available.wait(lock,
	                           [this] { return m_operation_state == OperationState::Closed; });
	lock.unlock();
	operation();
}

bool CommandScheduler::HasPriorityOperationAtCurrentTick() {
	std::lock_guard lock(m_operation_mutex);
	// The priority thread pops an operation before waiting for its tick, so check it too.
	const auto current = CurrentTick();
	return (m_priority_active && m_priority_active_tick >= current) ||
	       (!m_priority_operations.empty() && m_priority_operations.back().tick >= current);
}

void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
	while (!stop.stop_requested()) {
		PendingOperation operation;
		{
			std::unique_lock lock(m_operation_mutex);
			m_operation_available.wait(lock, [this, &stop] {
				return stop.stop_requested() || !m_priority_operations.empty();
			});
			if (stop.stop_requested()) {
				return;
			}
			operation = std::move(m_priority_operations.front());
			m_priority_operations.pop();
			m_priority_active      = true;
			m_priority_active_tick = operation.tick;
		}
		m_master.Wait(operation.tick);
		if (!stop.stop_requested()) {
			RunOperation(std::move(operation.callback));
		}
		{
			std::lock_guard lock(m_operation_mutex);
			m_priority_active      = false;
			m_priority_active_tick = 0;
		}
		m_operation_available.notify_all();
	}
}

void CommandScheduler::DrainPriorityOperations() {
	ProfileCpuScope profile(*this, "priority_drain_wait");
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(
	    lock, [this] { return m_priority_operations.empty() && !m_priority_active; });
}

void CommandScheduler::WaitPriorityOperations(uint64_t tick, std::source_location site) {
	ProfileCpuScope profile(*this, "priority_callback_wait", tick, 0, false, site);
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(lock, [this, tick] {
		const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
		const bool queued_before_or_at =
		    !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
		return !active_before_or_at && !queued_before_or_at;
	});
}

void CommandScheduler::RunOperation(Common::UniqueFunction<void>&& operation) {
	auto* previous                = g_deferred_callback_scheduler;
	g_deferred_callback_scheduler = this;
	operation();
	g_deferred_callback_scheduler = previous;
}

bool CommandScheduler::IsFree(uint64_t tick) {
	if (m_master.IsFree(tick)) {
		return true;
	}
	m_master.Refresh();
	return m_master.IsFree(tick);
}

void CommandScheduler::CheckActive() const {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	EXIT_IF(!Active());
}

CommandBuffer& CommandScheduler::Current() {
	CheckActive();
	++m_open_touches;
	if (DrawCommitEnabled() && !DrawCommitOnWorker()) {
		Context().GetRenderExecutor().FinishDrawCommit();
	}
	return m_command;
}

CommandBuffer& CommandScheduler::BeginCommand() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return m_command;
	}
#endif
	if (DrawCommitEnabled() && !DrawCommitOnWorker()) {
		Context().GetRenderExecutor().FinishDrawCommit();
	}
	EXIT_IF(!m_command.IsInvalid());
	m_command.m_buffer = m_command_pool.Commit();
	ProfileThread().scheduler = this;
	m_command.Begin();
	m_command.m_debug_op = 0;
	m_command.m_debug_arg4 = 0;
	RefreshGpuTimingControl();
	BeginGpuTimerPage();
	m_profile_cpu_build_start = ProfileActive() ? ProfileClockNs() : 0;
	if (ProfileActive() && m_profile_last_submit && m_master.IsFree(CurrentTick() - 1) && !m_profile_idle_observed)
		m_profile_idle_observed = m_profile_cpu_build_start;
	m_submit_draw_count = 0;
	m_submit_compute_count = 0;
	m_submit_gpu_timer = StartGpuTimer("submit", m_gpu_frame_hint);
	m_command.BeginGpuGap();
	return m_command;
}

static std::atomic<uint64_t> g_submit_count {0};

uint64_t SubmitCountTotal() {
	return g_submit_count.load(std::memory_order_relaxed);
}

uint64_t CommandScheduler::Submit(SubmitInfo submit) {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return 1;
	}
#endif
	g_submit_count.fetch_add(1, std::memory_order_relaxed);
	const auto cpu_begin = std::chrono::steady_clock::now();
	const auto submit_begin_ns = ProfileActive() ? ProfileClockNs() : 0;
	uint64_t queue_enter_ns = 0;
	EXIT_IF(m_command.IsInvalid());
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);
	m_command.EndRendering();
	m_command.EndGpuGap();

	if (m_submit_gpu_timer != UINT32_MAX) {
		auto& timer = m_gpu_timers[GpuProfileTimerToken::Index(m_submit_gpu_timer)];
		timer.frame = m_gpu_frame_hint;
		timer.debug_op = m_command.m_debug_op;
		timer.arg4 = m_command.m_debug_arg4;
		timer.draws = m_submit_draw_count;
		timer.computes = m_submit_compute_count;
		EndGpuTimer(m_submit_gpu_timer);
		m_submit_gpu_timer = UINT32_MAX;
	}
	// No timer may remain open across a command-buffer boundary. Preserve an explicit
	// split marker rather than ending it later in a different, unreset command buffer.
	if (m_current_timer_page != UINT32_MAX) {
		auto& page = m_timer_pages[m_current_timer_page];
		for (uint32_t j = 0; j < page.used; ++j) {
			auto& timer = m_gpu_timers[m_current_timer_page * GpuTimerPageSize + j];
			if (timer.recording && !timer.ended) { timer.split = true; EndGpuTimer(timer.token); }
		}
	}
	m_command.End();
	const auto buffer   = m_command.m_buffer;
	auto&      graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);

	vk::Result result;
	uint64_t   tick;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		tick = m_master.NextTick();
		submit.AddSignal(m_master.Handle(), tick);
		DebugRecordWork("submit", tick, 0, 0, 0);

		vk::TimelineSemaphoreSubmitInfo timeline_info {};
		timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
		timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
		timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
		timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

		vk::SubmitInfo submit_info {};
		submit_info.pNext                = &timeline_info;
		submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
		submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
		submit_info.pWaitDstStageMask    = submit.wait_stages.data();
		submit_info.commandBufferCount   = 1;
		submit_info.pCommandBuffers      = &buffer;
		submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
		submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

		queue_enter_ns = ProfileActive() ? ProfileClockNs() : 0;
		VramAttributionScope vram(graphics, "queue_submit");
		result = graphics.queue.submit(1, &submit_info, nullptr);
	}

	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, tick, m_command.m_debug_op,
		                  m_command.m_debug_submit_id, m_command.m_debug_arg0,
		                  m_command.m_debug_arg1, m_command.m_debug_arg2, m_command.m_debug_arg3,
		                  m_command.m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	if (m_current_timer_page != UINT32_MAX) {
		auto& page = m_timer_pages[m_current_timer_page];
		page.tick = tick; page.recording = false;
		for (uint32_t j = 0; j < page.used; ++j) {
			auto& timer = m_gpu_timers[m_current_timer_page * GpuTimerPageSize + j];
			timer.recording = false; timer.tick = tick;
		}
	}
	if (ProfileActive() && m_profile_submits_file) {
		const auto end = ProfileClockNs();
		const bool prior_complete = m_master.IsFree(tick - 1);
		std::fprintf(m_profile_submits_file, "%llu,%llu,%u,0x%llx,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u\n",
		    m_gpu_frame_hint, tick, graphics.queue_family, std::bit_cast<uint64_t>(graphics.queue),
		    ProfileThreadId(), m_profile_cpu_build_start,
		    submit_begin_ns, queue_enter_ns, end, m_profile_last_submit, m_profile_idle_observed,
		    prior_complete ? 1u : 0u, m_submit_draw_count, m_submit_compute_count,
		    submit.num_wait_semaphores, submit.num_signal_semaphores, m_current_timer_page);
		for (uint32_t i = 0; i < submit.num_wait_semaphores; ++i) {
			ProfileBarrier(buffer, "queue_wait_stage", static_cast<uint32_t>(submit.wait_stages[i]), 0, 0, 0,
			    std::bit_cast<uint64_t>(submit.wait_semaphores[i]), submit.wait_ticks[i], UINT32_MAX, UINT32_MAX, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, std::source_location::current());
			ProfileEvent("queue_semaphore_dependency", submit_begin_ns, submit_begin_ns,
			    std::bit_cast<uint64_t>(submit.wait_semaphores[i]), submit.wait_ticks[i], std::source_location::current());
		}
		m_profile_last_submit = end; m_profile_idle_observed = 0;
	}
	m_current_timer_page = UINT32_MAX;
	m_command.m_buffer = nullptr;
	m_submitted_touches = m_open_touches;
	if (ProfileActive()) {
		m_cpu_frame_submits++;
		m_cpu_frame_submit_ms +=
		    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpu_begin)
		        .count();
	}
	return tick;
}

void CommandScheduler::CollectAllGpuTimers() {
	if (!m_gpu_timing_file) return;
	// The counter is read nonblocking. Never add a host wait to retrieve diagnostics.
	if (std::none_of(m_timer_pages.begin(), m_timer_pages.end(), [](const auto& page) { return page.tick != 0; })) return;
	ProfileCpuScope collect_profile(*this, "profile_collect_cpu");
	m_master.Refresh();
	for (uint32_t p = 0; p < m_timer_pages.size(); ++p) {
		auto& page = m_timer_pages[p];
		if (!page.tick || page.recording || !m_master.IsFree(page.tick)) continue;
		std::array<uint64_t, GpuTimerPageSize * 4> data {};
		const auto result = m_graphics.device.getQueryPoolResults(m_gpu_query_pool,
		    p * GpuTimerPageSize * 2, page.used * 2, page.used * 4 * sizeof(uint64_t),
		    data.data(), sizeof(uint64_t) * 2,
		    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
		if (result != vk::Result::eSuccess) continue;
		bool complete = true;
		for (uint32_t j = 0; j < page.used; ++j) {
			const auto index = p * GpuTimerPageSize + j;
			auto& timer = m_gpu_timers[index];
			if (!timer.tick) continue;
			if (!data[j * 4 + 1] || !data[j * 4 + 3]) { complete = false; continue; }
			const auto start = data[j * 4] & m_timestamp_mask, end = data[j * 4 + 2] & m_timestamp_mask;
			std::fprintf(m_gpu_timing_file,
			    "%s,%llu,0x%016llx,%llu,%.6f,%u,0x%016llx,%u,%u,%llu,%llu,%u,%u,%llu,0x%llx,%u,%.6f,%llu\n",
			    timer.kind, timer.frame, timer.shader, timer.tick,
			    double((end - start) & m_timestamp_mask) * m_timestamp_period_ns / 1e6,
			    timer.debug_op, timer.arg4, timer.draws, timer.computes, start, end,
			    static_cast<uint32_t>(timer.start_stage), static_cast<uint32_t>(timer.end_stage),
			    timer.transaction, timer.address, timer.split ? 1u : 0u, m_timestamp_period_ns, m_timestamp_mask);
			timer = {};
		}
		if (complete) {
			// The timeline counter already proved this entire batch completed. Record
			// the host observation; no wait or CPU/GPU clock calibration is introduced.
			ProfileEvent("gpu_batch_complete_observed", ProfileClockNs(), ProfileClockNs(),
			    page.tick, 0, std::source_location::current(), ProfileFrame());
			page = {};
		}
	}
}
void CommandScheduler::BeginGpuTimerPage() {
	m_current_timer_page = UINT32_MAX;
	if (!m_gpu_timing_file) return;
	CollectAllGpuTimers();
	if (!ProfileActive() || !m_gpu_timing_enabled) return;
	for (uint32_t p = 0; p < m_timer_pages.size(); ++p) {
		auto& page = m_timer_pages[p];
		if (page.tick || page.recording) continue;
		page.recording = true; page.used = 0; m_current_timer_page = p;
		// Reset only this completed page, outside dynamic rendering, at batch start.
		m_command.Handle().resetQueryPool(m_gpu_query_pool, p * GpuTimerPageSize * 2, GpuTimerPageSize * 2);
		return;
	}
	++m_gpu_timer_drops;
}
uint32_t CommandScheduler::StartGpuTimer(const char* kind, uint64_t frame, uint64_t shader) {
	if (!ProfileActive() || !m_gpu_timing_enabled || m_current_timer_page == UINT32_MAX || m_command.IsInvalid()) return UINT32_MAX;
	auto& page = m_timer_pages[m_current_timer_page];
	if (page.used == GpuTimerPageSize) { ++m_gpu_timer_drops; return UINT32_MAX; }
	const auto index = m_current_timer_page * GpuTimerPageSize + page.used++;
	auto& timer = m_gpu_timers[index]; timer = {};
	m_timer_generation = GpuProfileTimerToken::NextGeneration(m_timer_generation);
	timer.token = GpuProfileTimerToken::Encode(m_timer_generation, index);
	timer.page = m_current_timer_page; timer.kind = kind; timer.frame = frame;
	timer.shader = shader; timer.recording = true;
	if (ProfileThread().scheduler == this) {
		timer.transaction = ProfileThread().transaction; timer.address = ProfileThread().address;
	}
	const bool compute = std::strcmp(kind, "compute") == 0 || std::strcmp(kind, "detile_dispatch") == 0 || std::strcmp(kind, "tile_dispatch") == 0;
	const bool pre = std::strcmp(kind, "detile_pre") == 0 || std::strcmp(kind, "tile_pre") == 0;
	const bool copy = std::strncmp(kind, "copy_", 5) == 0 || std::strcmp(kind, "detile_clear") == 0;
	if (compute) timer.start_stage = timer.end_stage = vk::PipelineStageFlagBits::eComputeShader;
	if (pre) timer.end_stage = vk::PipelineStageFlagBits::eComputeShader;
	if (copy) timer.start_stage = timer.end_stage = vk::PipelineStageFlagBits::eTransfer;
	if (std::strcmp(kind, "consumer_compute") == 0) timer.start_stage = timer.end_stage = vk::PipelineStageFlagBits::eComputeShader;
	if (std::strcmp(kind, "consumer_draw") == 0) timer.start_stage = timer.end_stage = vk::PipelineStageFlagBits::eAllGraphics;
	m_command.Handle().writeTimestamp(timer.start_stage, m_gpu_query_pool, index * 2);
	return timer.token;
}
void CommandScheduler::EndGpuTimer(uint32_t token) {
	if (token == UINT32_MAX) return;
	const auto index = GpuProfileTimerToken::Index(token);
	auto& timer = m_gpu_timers[index];
	if (timer.token != token || !timer.recording || timer.ended || timer.page != m_current_timer_page) return;
	m_command.Handle().writeTimestamp(timer.end_stage, m_gpu_query_pool, index * 2 + 1);
	timer.ended = true;
}
void CommandScheduler::SetGpuTimerDraws(uint32_t token, uint32_t draws) noexcept {
	if (token == UINT32_MAX) return;
	auto& timer = m_gpu_timers[GpuProfileTimerToken::Index(token)];
	if (timer.token == token) timer.draws = draws;
}
void CommandScheduler::SetGpuTimerArg4(uint32_t token, uint64_t arg4) noexcept {
	if (token == UINT32_MAX) return;
	auto& timer = m_gpu_timers[GpuProfileTimerToken::Index(token)];
	if (timer.token == token) timer.arg4 = arg4;
}

void CommandScheduler::RefreshGpuTimingControl() {
	if (m_gpu_control_path.empty() && m_profile_control_path.empty()) return;
	const auto now = std::chrono::steady_clock::now();
	if (now < m_next_gpu_control_check) return;
	m_next_gpu_control_check = now + std::chrono::milliseconds {500};
	if (!m_profile_control_path.empty() && m_profile_role == "producer") {
		if (FILE* file = std::fopen(m_profile_control_path.c_str(), "r")) {
			char value[32] {}; std::fgets(value, sizeof(value), file); std::fclose(file);
			if (value[0] == '1' || std::strncmp(value, "on", 2) == 0) {
				m_profile_start = m_gpu_frame_hint;
				ProfileArmWindow(m_profile_start);
				if (FILE* out = std::fopen(m_profile_control_path.c_str(), "w")) { std::fputs("off", out); std::fclose(out); }
				SetProfileFrame(m_gpu_frame_hint);
			}
		}
	}
	if (m_gpu_control_path.empty()) return;
	if (FILE* file = std::fopen(m_gpu_control_path.c_str(), "r"); file != nullptr) {
		char value[16] {};
		if (std::fgets(value, sizeof(value), file) != nullptr) {
			const bool enabled = value[0] == '1' || std::strncmp(value, "on", 2) == 0;
			if (enabled != m_gpu_timing_enabled) {
				m_gpu_timing_enabled = enabled;
				std::fprintf(m_gpu_timing_file, "control,%" PRIu64
				                                ",0x0000000000000000,0,0,0,0x0000000000000000,0,%u\n",
				             m_gpu_frame_hint, static_cast<unsigned>(enabled));
				std::fflush(m_gpu_timing_file);
			}
		}
		std::fclose(file);
	}
}

void CommandScheduler::FlushCpuFrame() {
	if (m_cpu_timing_file == nullptr || m_cpu_frame_start.time_since_epoch().count() == 0)
		return;
	const auto now = std::chrono::steady_clock::now();
	const auto wall_ms =
	    std::chrono::duration<double, std::milli>(now - m_cpu_frame_start).count();
	uint64_t drops, barriers, broad;
	{ std::lock_guard lock(m_profile_mutex);
	  drops = m_profile_records_dropped; barriers = m_profile_barriers; broad = m_profile_broad_barriers;
	  m_profile_records_dropped = m_profile_barriers = m_profile_broad_barriers = 0; }
	if (ProfileActive()) std::fprintf(m_cpu_timing_file, "%" PRIu64 ",%.3f,%" PRIu64 ",%" PRIu64 ",%" PRIu64
	                                ",%.3f,%" PRIu64 ",%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu\n",
	             m_gpu_frame_hint, wall_ms, m_cpu_frame_draws, m_cpu_frame_computes,
	             m_cpu_frame_submits, m_cpu_frame_submit_ms, m_cpu_frame_waits,
	             m_cpu_frame_wait_ms, m_cpu_draw_phase_ms[0], m_cpu_draw_phase_ms[1],
	             m_cpu_draw_phase_ms[2], m_cpu_draw_phase_ms[3], m_cpu_draw_phase_ms[4],
	             m_cpu_draw_phase_ms[5],
	             uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(m_cpu_frame_start.time_since_epoch()).count()),
	             ProfileClockNs(), m_gpu_timer_drops, drops, barriers, broad);
	FlushProfileEvents();
	if (m_gpu_timing_file) std::fflush(m_gpu_timing_file);
	if (m_profile_submits_file) std::fflush(m_profile_submits_file);
	m_gpu_timer_drops = 0;
	std::fflush(m_cpu_timing_file);
	if (ProfileActive()) ProfileEvent("profile_flush_cpu", uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()),
	    ProfileClockNs(), 0, 0, std::source_location::current());
	m_cpu_frame_start = now;
	m_cpu_frame_draws = m_cpu_frame_computes = m_cpu_frame_submits = m_cpu_frame_waits = 0;
	m_cpu_frame_submit_ms = m_cpu_frame_wait_ms = 0;
	m_cpu_draw_phase_ms.fill(0);
}

void CommandScheduler::NoteCpuDrawPhase(CpuDrawPhase phase, double milliseconds) noexcept {
	if (ProfileActive()) {
		m_cpu_draw_phase_ms[static_cast<size_t>(phase)] += milliseconds;
	}
}

void CommandScheduler::SetProfileFrame(uint64_t frame) {
	if (!m_gpu_timing_file) { m_gpu_frame_hint = frame; return; }
	if (!m_profile_control_path.empty()) m_profile_start = ProfileWindowStart();
	if (frame != m_gpu_frame_hint) { FlushCpuFrame(); m_gpu_frame_hint = frame; }
	if (ProfileActive() && !(frame >= m_profile_start && frame - m_profile_start < m_profile_count)) {
		std::lock_guard lock(m_profile_mutex); m_profile_images.clear(); m_profile_bound_images.clear(); m_profile_image_bytes.clear();
		m_profile_readback_snapshots.clear();
	}
	m_profile_frame.store(frame, std::memory_order_relaxed);
	const bool active = m_gpu_timing_file && frame >= m_profile_start && frame - m_profile_start < m_profile_count;
	const bool was_active = m_profile_active.exchange(active, std::memory_order_relaxed);
	if (active && !was_active && m_profile_lifetime) {
		m_profile_writer_operation_pending.store(false, std::memory_order_relaxed);
		ProfileEvent("writer_capture_floor", ProfileClockNs(), ProfileClockNs(), CurrentTick(), 0,
		    std::source_location::current());
	}
}
void CommandScheduler::NoteGpuCommand(uint64_t frame, bool compute) {
	SetProfileFrame(frame);
	if (compute) { m_submit_compute_count++; if (ProfileActive()) m_cpu_frame_computes++; }
	else { m_submit_draw_count++; if (ProfileActive()) m_cpu_frame_draws++; }
}
void CommandScheduler::WaitMaster(uint64_t tick, std::source_location site) {
	const auto begin = std::chrono::steady_clock::now();
	m_master.Wait(tick, site);
	if (ProfileActive()) {
		m_cpu_frame_waits++;
		m_cpu_frame_wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
	}
}

void CommandScheduler::BeginNext() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	CheckActive();
	BeginCommand();
}

} // namespace Libs::Graphics
