#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "common/threads.h"
#include <bit>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <xxhash.h>

namespace Libs::Graphics {
static std::atomic<uint64_t> g_profile_window_start {UINT64_MAX};
uint64_t ProfileThreadId() noexcept {
	static thread_local const uint64_t id = [] {
		const auto text = Common::Thread::GetThreadId();
		char* end = nullptr;
		const auto value = std::strtoull(text.c_str(), &end, 10);
		return end && *end == '\0' ? static_cast<uint64_t>(value)
		    : static_cast<uint64_t>(std::hash<std::thread::id> {}(std::this_thread::get_id()));
	}();
	return id;
}
void ProfileArmWindow(uint64_t start) noexcept { g_profile_window_start.store(start); }
uint64_t ProfileWindowStart() noexcept { return g_profile_window_start.load(); }
uint64_t ProfileClockNs() noexcept {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
}
ProfileThreadContext& ProfileThread() noexcept {
	static thread_local ProfileThreadContext context;
	return context;
}
ProfileCpuScope::ProfileCpuScope(CommandScheduler& scheduler, const char* kind, uint64_t resource,
    uint64_t bytes, bool transaction, std::source_location site)
    : m_kind(kind), m_site(site), m_resource(resource), m_bytes(bytes) {
	if (!scheduler.ProfileActive()) return;
	// Coherent memory has no Vulkan flush/invalidate operation to audit.
	if (std::strcmp(kind, "host_flush_noop") == 0 || std::strcmp(kind, "host_invalidate_noop") == 0) return;
	m_scheduler = &scheduler; m_frame = scheduler.ProfileFrame(); m_begin = ProfileClockNs();
	m_previous = ProfileThread();
	if (m_previous.scheduler != &scheduler) ProfileThread() = {};
	ProfileThread().scheduler = &scheduler;
	ProfileThread().operation = kind;
	if (transaction && ProfileThread().transaction == 0) {
		ProfileThread().transaction = scheduler.NextProfileTransaction();
		ProfileThread().address = resource;
	}
}
ProfileCpuScope::~ProfileCpuScope() {
	if (!m_scheduler) return;
	m_scheduler->ProfileEvent(m_kind, m_begin, ProfileClockNs(), m_resource, m_bytes, m_site, m_frame);
	ProfileThread() = m_previous;
}
void ProfileCpuScope::SetGuestQueue(uint32_t queue) {
	if (m_scheduler) ProfileThread().guest_queue = queue;
}
static thread_local uint32_t g_fine_depth = 0;
static thread_local uint64_t g_fine_sequence = 0;
static thread_local bool g_fine_selected = true;
bool ProfileFineSelected() noexcept { return g_fine_selected; }
ProfileOperationScope::ProfileOperationScope(const char* operation) {
	auto& context = ProfileThread();
	if (!context.scheduler || !context.scheduler->ProfileLifetime()) return;
	m_previous = context.operation; context.operation = operation;
}
ProfileOperationScope::~ProfileOperationScope() {
	if (m_previous) ProfileThread().operation = m_previous;
}
void ProfileCpuScope::NewTransaction(uint64_t address) {
	if (!m_scheduler) return;
	ProfileThread().transaction = m_scheduler->NextProfileTransaction();
	ProfileThread().address = address;
}
ProfileDetailScope::ProfileDetailScope(const char* kind, uint64_t resource, uint64_t bytes, std::source_location site) {
	auto* scheduler = ProfileThread().scheduler;
	if (!scheduler || !scheduler->ProfileActive()) return;
	m_joined = true;
	m_previous_operation = ProfileThread().operation;
	if (g_fine_depth++ == 0) {
		const bool eligible = scheduler->ProfileFineCpu();
		// SplitMix avoids aliasing a fixed call sequence (state/bindings/commit) with
		// a periodic every-N counter. All descendants of a selected root are timed.
		uint64_t choice = (++g_fine_sequence) + 0x9e3779b97f4a7c15ull;
		choice = (choice ^ (choice >> 30)) * 0xbf58476d1ce4e5b9ull;
		choice = (choice ^ (choice >> 27)) * 0x94d049bb133111ebull;
		choice ^= choice >> 31;
		g_fine_selected = eligible && (choice & 31) == 0;
	}
	if (scheduler->ProfileFineCpu()) m_scope.emplace(*scheduler, kind, resource, bytes, false, site);
	else if (scheduler->ProfileLifetime()) {
		ProfileThread().operation = kind;
	}
}
ProfileDetailScope::~ProfileDetailScope() {
	Finish();
}
void ProfileDetailScope::Finish() {
	if (!m_joined) return;
	m_scope.reset();
	ProfileThread().operation = m_previous_operation;
	if (--g_fine_depth == 0) g_fine_selected = true;
	m_joined = false;
}
void ProfileDetailEvent(const char* kind, uint64_t resource, uint64_t bytes, std::source_location site) {
	if (auto* scheduler = ProfileThread().scheduler; scheduler && scheduler->ProfileFineCpu()) {
		const auto now = ProfileClockNs(); scheduler->ProfileEvent(kind, now, now, resource, bytes, site);
	}
}
uint64_t ProfileFingerprint(const void* data, size_t bytes, uint64_t seed) {
	return XXH3_64bits_withSeed(data, bytes, seed);
}
bool CommandScheduler::ProfileWatched(uint64_t address, uint64_t bytes) const {
	if (!ProfileLifetime() || !bytes || address > UINT64_MAX - bytes) return false;
	return std::any_of(m_profile_watch_ranges.begin(), m_profile_watch_ranges.end(), [=](const auto& range) {
		return address < range.first + range.second && range.first < address + bytes;
	});
}
void CommandScheduler::ProfileBufferUse(const char* kind, uint64_t address, uint64_t bytes,
    uint64_t handle, uint64_t shader, uint64_t guest_submit, std::source_location site) {
	if (!ProfileWatched(address, bytes)) return;
	if (std::strcmp(kind, "gpu_writer_bind_pending") == 0 ||
	    std::strcmp(kind, "gpu_writer_compute_declared") == 0 ||
	    std::strcmp(kind, "gpu_writer_graphics_declared") == 0 ||
	    std::strcmp(kind, "gpu_writer_address_unknown") == 0)
		m_profile_writer_operation_pending.store(true, std::memory_order_relaxed);
	const auto previous = ProfileThread();
	ProfileThread().scheduler = this; ProfileThread().address = address;
	const auto now = ProfileClockNs();
	ProfileEvent(kind, now, now, handle, bytes, site);
	ProfileEvent("buffer_use_shader", now, now, shader, guest_submit, site);
	ProfileThread() = previous;
}
void CommandScheduler::ProfileReadbackData(uint64_t address, const void* data, uint64_t bytes) {
	if (!ProfileWatched(address, bytes) || bytes > 1024 * 1024) return;
	ProfileCpuScope cost(*this, "profile_readback_compare_cpu", address, bytes);
	const auto previous_address = ProfileThread().address;
	ProfileThread().address = address;
	auto found = std::find_if(m_profile_readback_snapshots.begin(), m_profile_readback_snapshots.end(),
	    [=](const auto& r) { return r.address == address && r.bytes.size() == bytes; });
	const char* kind = "readback_content_first";
	if (found != m_profile_readback_snapshots.end()) {
		kind = std::memcmp(found->bytes.data(), data, bytes) == 0 ? "readback_content_equal" : "readback_content_changed";
		std::memcpy(found->bytes.data(), data, bytes);
	} else if (m_profile_readback_snapshots.size() < 128) {
		const auto* begin = static_cast<const uint8_t*>(data);
		m_profile_readback_snapshots.push_back({address, {begin, begin + bytes}});
	} else kind = "readback_content_untracked";
	const auto hash = ProfileFingerprint(data, bytes);
	const auto now = ProfileClockNs(); ProfileEvent(kind, now, now, hash, bytes, std::source_location::current());
	ProfileThread().address = previous_address;
}
void CommandScheduler::ProfileUnknownWriter(uint64_t shader, uint64_t guest_submit) {
	if (!ProfileLifetime()) return;
	for (const auto& [address, size]: m_profile_watch_ranges)
		ProfileBufferUse("gpu_writer_address_unknown", address, size, 0, shader, guest_submit);
}
void CommandScheduler::ProfileWriterOperationEnd() {
	if (!ProfileLifetime() || !m_profile_writer_operation_pending.exchange(false, std::memory_order_relaxed)) return;
	const auto now = ProfileClockNs();
	ProfileEvent("writer_operation_recorded", now, now, 0, 0, std::source_location::current());
}
void CommandScheduler::ProfileReadbackSpan(uint64_t address, uint64_t bytes, uint64_t handle) {
	if (!ProfileLifetime()) return;
	const auto previous = ProfileThread();
	ProfileThread().scheduler = this; ProfileThread().address = address;
	const auto now = ProfileClockNs();
	ProfileEvent("readback_required_bytes", now, now, handle, bytes, std::source_location::current());
	ProfileThread() = previous;
}
void CommandScheduler::ProfileEvent(const char* kind, uint64_t begin, uint64_t end,
    uint64_t resource, uint64_t bytes, std::source_location site, uint64_t frame) {
	if (!m_profile_events_file || (!ProfileActive() && frame == UINT64_MAX)) return;
	const auto& context = ProfileThread();
	ProfileRecord record {kind, frame == UINT64_MAX ? ProfileFrame() : frame, begin, end,
	    ProfileThreadId(), CurrentTick(),
	    context.scheduler == this ? context.transaction : 0,
	    context.scheduler == this ? context.address : 0, resource, bytes};
	record.guest_queue = context.scheduler == this ? context.guest_queue : UINT32_MAX;
	record.site = site;
	record.context = context.scheduler == this ? context.operation : "";
	std::lock_guard lock(m_profile_mutex);
	if (m_profile_records.size() < 100000) {
		m_profile_records.push_back(record);
		// End of record construction, lock acquisition and vector append. This is
		// recorder overhead after the measured scope, not scope workload.
		m_profile_records.back().probe_end = ProfileClockNs();
	}
	else ++m_profile_records_dropped;
}
void CommandScheduler::ProfileBarrier(vk::CommandBuffer command, const char* type,
    uint64_t src, uint64_t dst, uint64_t src_access, uint64_t dst_access,
    uint64_t resource, uint64_t bytes, uint32_t old_layout, uint32_t new_layout,
    uint32_t src_queue, uint32_t dst_queue, std::source_location site) {
	if (!ProfileActive() || command != m_command.m_buffer) return;
	const auto& context = ProfileThread();
	ProfileRecord record {type, ProfileFrame(), ProfileClockNs(), 0,
	    ProfileThreadId(), CurrentTick(),
	    context.transaction, context.address, resource, bytes, src, dst, src_access, dst_access,
	    old_layout, new_layout, src_queue, dst_queue, context.guest_queue, site};
	record.end = record.begin;
	record.context = context.scheduler == this ? context.operation : "";
	std::lock_guard lock(m_profile_mutex);
	if (std::strcmp(type, "barrier_image") == 0) {
		if (const auto found = m_profile_image_bytes.find(resource); found != m_profile_image_bytes.end()) record.bytes = found->second;
	}
	if (std::strncmp(type, "barrier_", 8) == 0) {
		++m_profile_barriers;
		if ((src | dst) & static_cast<uint64_t>(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)) ++m_profile_broad_barriers;
	}
	if (m_profile_records.size() < 100000) m_profile_records.push_back(record);
	else ++m_profile_records_dropped;
}
void CommandScheduler::FlushProfileEvents() {
	if (!m_profile_events_file) return;
	std::vector<ProfileRecord> records;
	{ std::lock_guard lock(m_profile_mutex); records.swap(m_profile_records); }
	for (const auto& r: records) {
		// The function is quoted and escaped because template signatures can contain commas.
		std::string function = r.site.function_name();
		for (size_t p = 0; (p = function.find('"', p)) != std::string::npos; p += 2) function.insert(p, 1, '"');
		std::fprintf(m_profile_events_file,
		    "%s,%llu,%llu,%llu,%llu,%llu,%llu,0x%llx,0x%llx,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,\"%s\",%u,\"%s\",\"%s\",%llu\n",
		    r.kind, r.frame, r.begin, r.end, r.thread, r.tick, r.transaction, r.address, r.resource,
		    r.bytes, r.src, r.dst, r.src_access, r.dst_access, r.old_layout, r.new_layout,
		    r.src_queue, r.dst_queue, r.guest_queue, r.site.file_name(), r.site.line(), function.c_str(), r.context, r.probe_end);
	}
	std::fflush(m_profile_events_file);
}
void CommandScheduler::ProfileImageReady(vk::Image image, uint64_t address) {
	if (!ProfileActive() || ProfileThread().transaction == 0) return;
	{ std::lock_guard lock(m_profile_mutex);
	  m_profile_images[std::bit_cast<uint64_t>(image)] = {ProfileThread().transaction, address, ProfileFrame()}; }
	const auto marker = StartGpuTimer("resource_ready", ProfileFrame(), std::bit_cast<uint64_t>(image));
	EndGpuTimer(marker);
}
void CommandScheduler::ProfileImageBinding(vk::Image image) {
	if (!ProfileActive()) return;
	const auto handle = std::bit_cast<uint64_t>(image);
	std::lock_guard lock(m_profile_mutex);
	if (m_profile_images.contains(handle) &&
	    std::find(m_profile_bound_images.begin(), m_profile_bound_images.end(), handle) == m_profile_bound_images.end())
		m_profile_bound_images.push_back(handle);
}
void CommandScheduler::ProfileConsumers(bool compute) {
	if (!ProfileActive()) return;
	std::lock_guard lock(m_profile_mutex);
	const auto previous = ProfileThread();
	for (auto handle: m_profile_bound_images) {
		auto found = m_profile_images.find(handle);
		if (found == m_profile_images.end()) continue;
		ProfileThread().scheduler = this;
		ProfileThread().transaction = found->second.transaction;
		ProfileThread().address = found->second.address;
		const auto marker = StartGpuTimer(compute ? "consumer_compute" : "consumer_draw", ProfileFrame(), handle);
		EndGpuTimer(marker); // Marker immediately before command; not a shader duration.
		m_profile_images.erase(found);
	}
	m_profile_bound_images.clear();
	ProfileThread() = previous;
}
void CommandScheduler::ProfileImageForget(vk::Image image) {
	if (!m_gpu_timing_file) return;
	std::lock_guard lock(m_profile_mutex);
	m_profile_images.erase(std::bit_cast<uint64_t>(image));
	m_profile_image_bytes.erase(std::bit_cast<uint64_t>(image));
}
void CommandScheduler::ProfileImageInfo(vk::Image image, uint64_t bytes) {
	if (!ProfileActive()) return;
	std::lock_guard lock(m_profile_mutex);
	m_profile_image_bytes[std::bit_cast<uint64_t>(image)] = bytes;
}

void ProfilePipelineBarrier(vk::CommandBuffer command, vk::PipelineStageFlags src,
    vk::PipelineStageFlags dst, vk::DependencyFlags flags, uint32_t memories,
    const vk::MemoryBarrier* memory, uint32_t buffers, const vk::BufferMemoryBarrier* buffer,
    uint32_t images, const vk::ImageMemoryBarrier* image, std::source_location site) {
	if (auto* scheduler = ProfileThread().scheduler; scheduler && scheduler->ProfileActive()) {
		const auto s = static_cast<uint64_t>(static_cast<VkPipelineStageFlags>(src));
		const auto d = static_cast<uint64_t>(static_cast<VkPipelineStageFlags>(dst));
		for (uint32_t i = 0; i < memories; ++i) scheduler->ProfileBarrier(command, "barrier_global", s, d,
		    static_cast<VkAccessFlags>(memory[i].srcAccessMask), static_cast<VkAccessFlags>(memory[i].dstAccessMask),
		    0, 0, UINT32_MAX, UINT32_MAX, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, site);
		for (uint32_t i = 0; i < buffers; ++i) scheduler->ProfileBarrier(command, "barrier_buffer", s, d,
		    static_cast<VkAccessFlags>(buffer[i].srcAccessMask), static_cast<VkAccessFlags>(buffer[i].dstAccessMask),
		    std::bit_cast<uint64_t>(buffer[i].buffer), buffer[i].size, UINT32_MAX, UINT32_MAX,
		    buffer[i].srcQueueFamilyIndex, buffer[i].dstQueueFamilyIndex, site);
		for (uint32_t i = 0; i < images; ++i) scheduler->ProfileBarrier(command, "barrier_image", s, d,
		    static_cast<VkAccessFlags>(image[i].srcAccessMask), static_cast<VkAccessFlags>(image[i].dstAccessMask),
		    std::bit_cast<uint64_t>(image[i].image), 0, static_cast<uint32_t>(image[i].oldLayout),
		    static_cast<uint32_t>(image[i].newLayout), image[i].srcQueueFamilyIndex, image[i].dstQueueFamilyIndex, site);
	}
	command.pipelineBarrier(src, dst, flags, memories, memory, buffers, buffer, images, image);
}
void ProfilePipelineBarrier(vk::CommandBuffer command, vk::PipelineStageFlags src,
    vk::PipelineStageFlags dst, vk::DependencyFlags flags,
    vk::ArrayProxy<const vk::MemoryBarrier> memory, vk::ArrayProxy<const vk::BufferMemoryBarrier> buffer,
    vk::ArrayProxy<const vk::ImageMemoryBarrier> image, std::source_location site) {
	ProfilePipelineBarrier(command, src, dst, flags, memory.size(), memory.data(), buffer.size(), buffer.data(),
	    image.size(), image.data(), site);
}
void ProfilePipelineBarrier2(vk::CommandBuffer command, const vk::DependencyInfo& dep, std::source_location site) {
	if (auto* scheduler = ProfileThread().scheduler; scheduler && scheduler->ProfileActive()) {
		for (uint32_t i = 0; i < dep.memoryBarrierCount; ++i) {
			const auto& b = dep.pMemoryBarriers[i];
			scheduler->ProfileBarrier(command, "barrier_global", static_cast<VkPipelineStageFlags2>(b.srcStageMask),
			    static_cast<VkPipelineStageFlags2>(b.dstStageMask), static_cast<VkAccessFlags2>(b.srcAccessMask),
			    static_cast<VkAccessFlags2>(b.dstAccessMask), 0, 0, UINT32_MAX, UINT32_MAX,
			    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, site);
		}
		for (uint32_t i = 0; i < dep.bufferMemoryBarrierCount; ++i) {
			const auto& b = dep.pBufferMemoryBarriers[i];
			scheduler->ProfileBarrier(command, "barrier_buffer", static_cast<VkPipelineStageFlags2>(b.srcStageMask),
			    static_cast<VkPipelineStageFlags2>(b.dstStageMask), static_cast<VkAccessFlags2>(b.srcAccessMask),
			    static_cast<VkAccessFlags2>(b.dstAccessMask), std::bit_cast<uint64_t>(b.buffer), b.size,
			    UINT32_MAX, UINT32_MAX, b.srcQueueFamilyIndex, b.dstQueueFamilyIndex, site);
		}
		for (uint32_t i = 0; i < dep.imageMemoryBarrierCount; ++i) {
			const auto& b = dep.pImageMemoryBarriers[i];
			scheduler->ProfileBarrier(command, "barrier_image", static_cast<VkPipelineStageFlags2>(b.srcStageMask),
			    static_cast<VkPipelineStageFlags2>(b.dstStageMask), static_cast<VkAccessFlags2>(b.srcAccessMask),
			    static_cast<VkAccessFlags2>(b.dstAccessMask), std::bit_cast<uint64_t>(b.image), 0,
			    static_cast<uint32_t>(b.oldLayout), static_cast<uint32_t>(b.newLayout), b.srcQueueFamilyIndex, b.dstQueueFamilyIndex, site);
		}
	}
	command.pipelineBarrier2(dep);
}
} // namespace Libs::Graphics
