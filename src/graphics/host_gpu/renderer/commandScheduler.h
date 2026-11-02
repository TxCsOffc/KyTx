#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/productionProfile.h"
#include "graphics/host_gpu/renderer/gpuProfileTimerToken.h"

#include <array>
#include <condition_variable>
#include <cstdio>
#include <chrono>
#include <mutex>
#include <string>

#include <queue>

#include <thread>
#include <vector>
#include <atomic>
#include <unordered_map>
#if defined(__APPLE__)
#include "graphics/host_gpu/metal/metalRenderer.h"
#endif

namespace Libs::Graphics {

class CommandScheduler {
public:
	enum class CpuDrawPhase : uint8_t { State, Bindings, VertexIndex, Pipeline, Targets, CommitDraw, Count };
	CommandScheduler(RenderContext& context, GraphicContext& graphics, const char* profile_role = "producer");
	~CommandScheduler();
	KYTY_CLASS_NO_COPY(CommandScheduler);

	void           Begin(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders);
	void           BeginRendering(const RenderState& state);
	void           EndRendering();
	void           Flush();
	void           Flush(SubmitInfo& submit);
	void           FlushAndWait(std::source_location site = std::source_location::current());
	void           Finish(std::source_location site = std::source_location::current());
	CommandBuffer& BeginCommand();
	uint64_t       Submit(SubmitInfo submit = {});
	// Deferred callbacks can observe an externally owned drain, but cannot initiate shutdown:
	// the priority runner cannot join itself.
	void                      Shutdown();
	void                      Wait(uint64_t tick, std::source_location site = std::source_location::current());
	void                      PopPendingOperations();
	void                      DrainPriorityOperations();
	void                      WaitPriorityOperations(uint64_t tick, std::source_location site = std::source_location::current());
	void                      DeferOperation(Common::UniqueFunction<void>&& operation);
	// Run after `extra_ticks` further GPU ticks complete beyond the current one (MoltenVK does not
	// retain resources referenced by in-flight command buffers).
	void DeferOperationAfterTicks(Common::UniqueFunction<void>&& operation, uint32_t extra_ticks);
	void                      DeferPriorityOperation(Common::UniqueFunction<void>&& operation);
	[[nodiscard]] static bool InDeferredOperation() noexcept;

	[[nodiscard]] bool Active() const noexcept {
#if defined(__APPLE__)
		if (Metal::UseNativeMetalBackend()) return true;
#endif
		return m_command.m_registers != nullptr;
	}
	// True when the open command buffer was reached (Current()) since the last submit: only then can
	// it hold work a wait is waiting for. KYTY_WAIT_RETRY_ELIDE uses it.
	[[nodiscard]] bool OpenBufferDirty() const noexcept { return m_open_touches != m_submitted_touches; }
	void                           CheckActive() const;
	CommandBuffer&                 Current();
	[[nodiscard]] uint64_t         CurrentTick() const noexcept { return m_master.CurrentTick(); }
	[[nodiscard]] bool             IsFree(uint64_t tick);
	[[nodiscard]] MasterSemaphore& GetMasterSemaphore() noexcept { return m_master; }
	[[nodiscard]] RenderContext&   Context() const noexcept { return m_context; }
	[[nodiscard]] GraphicContext&  Graphics() const noexcept { return m_graphics; }
	// Opt-in Vulkan timestamps. UINT32_MAX means the bounded query ring was busy.
	uint32_t StartGpuTimer(const char* kind, uint64_t frame, uint64_t shader = 0);
	void EndGpuTimer(uint32_t timer);
	void SetGpuTimerDraws(uint32_t timer, uint32_t draws) noexcept;
	void SetGpuTimerArg4(uint32_t timer, uint64_t arg4) noexcept;
	void NoteGpuCommand(uint64_t frame, bool compute);
	[[nodiscard]] uint32_t CurrentSubmitDrawCount() const noexcept { return m_submit_draw_count; }
	[[nodiscard]] bool CpuDrawTimingEnabled() const noexcept { return ProfileActive(); }
	void NoteCpuDrawPhase(CpuDrawPhase phase, double milliseconds) noexcept;
	[[nodiscard]] uint64_t GpuFrameHint() const noexcept { return m_gpu_frame_hint; }
	[[nodiscard]] bool ProfileActive() const noexcept { return m_profile_active.load(std::memory_order_relaxed); }
	[[nodiscard]] uint64_t ProfileFrame() const noexcept { return m_profile_frame.load(std::memory_order_relaxed); }
	[[nodiscard]] bool ProfileDetailedCpu() const noexcept { return ProfileActive() && ProfileFrame() % m_profile_detail_every == 0; }
	[[nodiscard]] bool ProfileFineCpu() const noexcept { return m_profile_fine_cpu && ProfileDetailedCpu() && ProfileFineSelected(); }
	[[nodiscard]] bool ProfileLifetime() const noexcept { return m_profile_lifetime && ProfileActive(); }
	[[nodiscard]] bool ProfileWatched(uint64_t address, uint64_t bytes) const;
	void ProfileBufferUse(const char* kind, uint64_t address, uint64_t bytes, uint64_t handle,
	                      uint64_t shader = 0, uint64_t guest_submit = 0,
	                      std::source_location site = std::source_location::current());
	void ProfileReadbackData(uint64_t address, const void* data, uint64_t bytes);
	void ProfileUnknownWriter(uint64_t shader, uint64_t guest_submit);
	void ProfileWriterOperationEnd();
	void ProfileReadbackSpan(uint64_t address, uint64_t bytes, uint64_t handle);
	void SetProfileFrame(uint64_t frame);
	uint64_t NextProfileTransaction() noexcept { return ++m_profile_transaction; }
	void ProfileEvent(const char* kind, uint64_t begin, uint64_t end, uint64_t resource,
	                  uint64_t bytes, std::source_location site, uint64_t frame = UINT64_MAX);
	void ProfileBarrier(vk::CommandBuffer command, const char* type, uint64_t src, uint64_t dst,
	                    uint64_t src_access, uint64_t dst_access, uint64_t resource, uint64_t bytes,
	                    uint32_t old_layout, uint32_t new_layout, uint32_t src_queue, uint32_t dst_queue,
	                    std::source_location site);
	void ProfileImageReady(vk::Image image, uint64_t address);
	void ProfileImageBinding(vk::Image image);
	void ProfileImageForget(vk::Image image);
	void ProfileImageInfo(vk::Image image, uint64_t bytes);
	void ProfileConsumers(bool compute);

private:
	class CommandPool {
	public:
		CommandPool(GraphicContext& graphics, MasterSemaphore& master);
		~CommandPool();
		KYTY_CLASS_NO_COPY(CommandPool);

		vk::CommandBuffer Commit();

	private:
		static constexpr size_t GrowStep = 4;

		size_t Grow();

		GraphicContext&                m_graphics;
		MasterSemaphore&               m_master;
		vk::CommandPool                m_pool = nullptr;
		std::vector<vk::CommandBuffer> m_buffers;
		std::vector<uint64_t>          m_ticks;
		size_t                         m_hint = 0;
	};

	enum class OperationState { Open, Draining, Closed };

	struct PendingOperation {
		Common::UniqueFunction<void> callback;
		uint64_t                     tick = 0;
	};

	void BeginNext();
	void PriorityOperationsThread(std::stop_token stop);
	void RunOperation(Common::UniqueFunction<void>&& operation);
	void CollectAllGpuTimers();
	void RefreshGpuTimingControl();
	void FlushCpuFrame();
	void WaitMaster(uint64_t tick, std::source_location site = std::source_location::current());
	void BeginGpuTimerPage();
	void FlushProfileEvents();

	static constexpr uint32_t GpuTimerPageSize = 256;
	static constexpr uint32_t GpuTimerCount = GpuProfileTimerToken::Count;
	struct GpuTimer {
		const char* kind = nullptr;
		uint64_t frame = 0;
		uint64_t shader = 0;
		uint64_t tick = 0;
		uint64_t arg4 = 0;
		uint32_t debug_op = 0;
		uint32_t draws = 0;
		uint32_t computes = 0;
		bool recording = false;
		bool ended = false;
		uint32_t token = UINT32_MAX;
		uint32_t page = UINT32_MAX;
		uint64_t transaction = 0, address = 0;
		vk::PipelineStageFlagBits start_stage = vk::PipelineStageFlagBits::eTopOfPipe;
		vk::PipelineStageFlagBits end_stage = vk::PipelineStageFlagBits::eBottomOfPipe;
		bool split = false;
	};
	struct TimerPage { uint64_t tick = 0; uint32_t used = 0; bool recording = false; };
	std::array<TimerPage, GpuTimerCount / GpuTimerPageSize> m_timer_pages {};
	uint32_t m_current_timer_page = UINT32_MAX, m_timer_generation = 0;
	uint64_t m_gpu_timer_drops = 0;
	struct ProfileRecord {
		const char* kind; uint64_t frame, begin, end, thread, tick, transaction, address, resource, bytes;
		uint64_t src = 0, dst = 0, src_access = 0, dst_access = 0;
		uint32_t old_layout = UINT32_MAX, new_layout = UINT32_MAX;
		uint32_t src_queue = VK_QUEUE_FAMILY_IGNORED, dst_queue = VK_QUEUE_FAMILY_IGNORED, guest_queue = UINT32_MAX;
		std::source_location site;
		const char* context = "";
		uint64_t probe_end = 0;
	};
	std::mutex m_profile_mutex;
	std::vector<ProfileRecord> m_profile_records;
	std::FILE* m_profile_events_file = nullptr;
	std::FILE* m_profile_submits_file = nullptr;
	std::atomic_bool m_profile_active {false};
	std::atomic<uint64_t> m_profile_frame {0}, m_profile_transaction {0};
	uint64_t m_profile_start = 0, m_profile_count = UINT64_MAX;
	uint64_t m_profile_detail_every = 16;
	bool m_profile_fine_cpu = false, m_profile_lifetime = false;
	std::atomic_bool m_profile_writer_operation_pending {false};
	std::vector<std::pair<uint64_t, uint64_t>> m_profile_watch_ranges;
	struct ProfileReadbackSnapshot { uint64_t address; std::vector<uint8_t> bytes; };
	std::vector<ProfileReadbackSnapshot> m_profile_readback_snapshots;
	uint64_t m_profile_records_dropped = 0, m_profile_barriers = 0, m_profile_broad_barriers = 0;
	uint64_t m_profile_cpu_build_start = 0, m_profile_last_submit = 0, m_profile_idle_observed = 0;
	std::string m_profile_control_path;
	std::string m_profile_role;
	struct PendingImage { uint64_t transaction, address, frame; };
	std::unordered_map<uint64_t, PendingImage> m_profile_images;
	std::unordered_map<uint64_t, uint64_t> m_profile_image_bytes;
	std::vector<uint64_t> m_profile_bound_images;

	MasterSemaphore              m_master;
	RenderContext&               m_context;
	GraphicContext&              m_graphics;
	CommandPool                  m_command_pool;
	CommandBuffer                m_command;
	uint64_t                     m_open_touches      = 0;  // Current() calls (GPU thread)
	uint64_t                     m_submitted_touches = 0;  // value of m_open_touches at the last submit
	std::queue<PendingOperation> m_pending_operations;
	std::queue<PendingOperation> m_priority_operations;

public:
	// True when an EOP/flip/label operation waits on the not-yet-submitted command buffer.
	[[nodiscard]] bool HasPriorityOperationAtCurrentTick();

private:
	std::mutex                   m_operation_mutex;
	std::condition_variable      m_operation_available;
	std::jthread                 m_priority_thread;
	bool                         m_priority_active      = false;
	uint64_t                     m_priority_active_tick = 0;
	OperationState               m_operation_state      = OperationState::Open;
	vk::QueryPool                m_gpu_query_pool = nullptr;
	std::FILE*                   m_gpu_timing_file = nullptr;
	std::FILE*                   m_cpu_timing_file = nullptr;
	std::string                  m_gpu_control_path;
	std::chrono::steady_clock::time_point m_next_gpu_control_check {};
	std::chrono::steady_clock::time_point m_cpu_frame_start {};
	bool                         m_gpu_timing_enabled = true;
	std::vector<GpuTimer> m_gpu_timers;
	uint32_t                     m_submit_gpu_timer = UINT32_MAX;
	uint64_t                     m_gpu_frame_hint = 0;
	uint32_t                     m_submit_draw_count = 0;
	uint32_t                     m_submit_compute_count = 0;
	uint64_t                     m_cpu_frame_draws = 0;
	uint64_t                     m_cpu_frame_computes = 0;
	uint64_t                     m_cpu_frame_submits = 0;
	uint64_t                     m_cpu_frame_waits = 0;
	double                       m_cpu_frame_submit_ms = 0;
	double                       m_cpu_frame_wait_ms = 0;
	std::array<double, static_cast<size_t>(CpuDrawPhase::Count)> m_cpu_draw_phase_ms {};
	uint64_t                     m_timestamp_mask = UINT64_MAX;
	double                       m_timestamp_period_ns = 0;
};

// Total submissions so far (diagnostics).
uint64_t SubmitCountTotal();

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
