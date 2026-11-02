#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/syncTiming.h"
#include <bit>
#include <cinttypes>
#include <cstdio>

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		m_semaphore = nullptr;
		return;
	}
#endif
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	if (result != vk::Result::eSuccess) {
		std::fprintf(stderr, "GPU semaphore read failed (%d): gpu_tick=%" PRIu64 "\n",
		             static_cast<int>(result), KnownGpuTick());
		DebugDumpRecentWork(1500, KnownGpuTick());
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick, std::source_location site) {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	SyncReadbackSample* const sync = t_sync_sample;
	const uint64_t sync_begin = sync ? SyncTimingNowNs() : 0;
	if (sync) sync->pending = DebugPendingWork(KnownGpuTick(), tick);
	const auto start = m_profiler && m_profiler->ProfileActive() ? ProfileClockNs() : 0;
	const auto frame = start ? m_profiler->ProfileFrame() : 0;
	// Stall watchdog: report what is being waited for instead of hanging silently.
	vk::Result result;
	for (uint32_t attempt = 0;; attempt++) {
		result = m_graphics.device.waitSemaphores(&wait_info, 3'000'000'000ull);
		if (result != vk::Result::eTimeout) {
			break;
		}
		Refresh();
		std::fprintf(stderr, "GPU wait stall %us: waiting tick=%" PRIu64 " gpu_tick=%" PRIu64
		     " current(unsubmitted)=%" PRIu64 " at %s:%u\n",
		     (attempt + 1) * 3, tick, KnownGpuTick(), CurrentTick(), site.file_name(), site.line());
		std::fflush(stderr);
		if (attempt == 0) {
			DebugDumpRecentWork(400, KnownGpuTick());
		}
	}
	if (result != vk::Result::eSuccess) {
		std::fprintf(stderr, "GPU wait failed (%d): waiting tick=%" PRIu64 " gpu_tick=%" PRIu64 "\n",
		             static_cast<int>(result), tick, KnownGpuTick());
		DebugDumpRecentWork(1500, KnownGpuTick());
	}
	if (sync) sync->wait_ns += SyncTimingNowNs() - sync_begin;
	if (start) m_profiler->ProfileEvent("timeline_wait", start, ProfileClockNs(),
	    std::bit_cast<uint64_t>(m_semaphore), tick, site, frame);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Refresh();
}

} // namespace Libs::Graphics
