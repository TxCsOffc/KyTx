#include "graphics/host_gpu/cpWriteTrace.h"
#include "graphics/host_gpu/renderer/productionProfile.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/syncTiming.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "common/platform/sysAppleAutorelease.h"
#include "kernel/memory.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;

} // namespace

// KYTY_GUEST_IMPORT=1: buffers alias guest memory (EnsureGuestImport). Default off.
// Diagnostics owned by the GPU thread (graphicsRun.cpp): the last suspended WAIT_REG_MEM address and
// the number of unsatisfied re-checks, for the label thread's stall dump.
extern std::atomic<uint64_t> g_pm4_wait_addr;
extern std::atomic<uint64_t> g_pm4_wait_retries;

// Pages where a guest CPU read has found an imported label GPU-dirty: the guest polls that label.
// KYTY_WAIT_BY_ORDER never skips a wait on such a page. GPU thread only.
static std::unordered_set<uint64_t> g_label_polled_pages;
static void NotePolledLabelPages(uint64_t vaddr, uint64_t size) {
	for (uint64_t page = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	     page < vaddr + size; page += TRACKER_PAGE_SIZE) {
		g_label_polled_pages.insert(page);
	}
}
bool LabelPageCpuPolled(uint64_t address) {
	return g_label_polled_pages.count(Common::AlignDown(address, TRACKER_PAGE_SIZE)) != 0;
}

static bool GuestImportEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GUEST_IMPORT");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

// KYTY_IMPORT_LISTENER=1 with KYTY_GUEST_IMPORT=1: filter mode. GPU-written imported pages stay
// protected until a guest fault finds their writer complete. No thread is started.
static bool ImportFilterEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_IMPORT_LISTENER");
		return GuestImportEnabled() && value != nullptr && value[0] == '1';
	}();
	return enabled;
}

// KYTY_GPU_LABELS (default on with KYTY_GUEST_IMPORT=1; 0 disables): EOP labels are written by
// the GPU in command order instead of by a CPU memcpy at record time.
static bool GpuLabelsEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GPU_LABELS");
		return GuestImportEnabled() && !(value != nullptr && value[0] == '0');
	}();
	return enabled;
}

// Diagnostics for isolating import behaviour (all off by default).
static bool ImportEnvFlag(const char* name) {
	const char* value = std::getenv(name);
	return value != nullptr && value[0] == '1';
}
// KYTY_IMPORT_NO_ADOPT=1: overlapping buffers join through device copies (no aliasing).
static bool ImportNoAdopt() {
	static const bool enabled = ImportEnvFlag("KYTY_IMPORT_NO_ADOPT");
	return enabled;
}
// KYTY_IMPORT_KEEP_UPLOADS=1: no buffer aliases guest memory; every buffer keeps record-time uploads.
static bool ImportKeepUploads() {
	static const bool enabled = ImportEnvFlag("KYTY_IMPORT_KEEP_UPLOADS");
	return enabled;
}
// KYTY_IMPORT_ALL=1: old behaviour, every overlap-free guest range aliases guest memory at creation.
static bool ImportAll() {
	static const bool enabled = ImportEnvFlag("KYTY_IMPORT_ALL");
	return enabled;
}
// Default import mode: device buffers with normal uploads; GPU-written ranges alias guest memory.
static bool HybridImport() {
	return GuestImportEnabled() && !ImportAll() && !ImportKeepUploads();
}
// KYTY_IMPORT_CHECK=1: re-validate each imported buffer's backing offset on use (logs only).
static bool ImportCheck() {
	static const bool enabled = ImportEnvFlag("KYTY_IMPORT_CHECK");
	return enabled;
}

bool BufferCache::SparseBdaEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_SPARSE_BDA");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

std::pair<Buffer&, uint64_t> BufferCache::GetBdaLeaf(uint64_t leaf_index) {
	constexpr uint64_t chunk_size = 64 * MiB;
	constexpr uint64_t leaves_per_chunk = chunk_size / BDA_LEAF_SIZE;
	auto [it, inserted] = m_bda_leaf_slots.try_emplace(leaf_index, m_bda_leaf_count);
	if (inserted) {
		if (m_bda_leaf_count == 0) {
			m_bda_pagetable_buffer.Fill(0, m_bda_pagetable_buffer.Size(), 0);
		}
		if (m_bda_leaf_count % leaves_per_chunk == 0) {
			auto chunk = std::make_unique<Buffer>(
		    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
		    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, chunk_size);
			chunk->Fill(0, chunk_size, 0);
			m_bda_leaf_chunks.emplace_back(std::move(chunk));
		}
		const auto slot = m_bda_leaf_count++;
		const auto& chunk = *m_bda_leaf_chunks[slot / leaves_per_chunk];
		const auto offset = (slot % leaves_per_chunk) * BDA_LEAF_SIZE;
		const auto address = chunk.BufferDeviceAddress() + offset;
		WriteDataBuffer(m_bda_pagetable_buffer, leaf_index * sizeof(address),
		                &address, sizeof(address));
	}
	const auto slot = it->second;
	return {*m_bda_leaf_chunks[slot / leaves_per_chunk],
	        (slot % leaves_per_chunk) * BDA_LEAF_SIZE};
}

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	NoteWriterTick(address, size);
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		buffer.CopyFrom(m_scheduler.Current(), m_staging_buffer, offset, buffer.Offset(address),
		                chunk, vk::AccessFlagBits::eHostWrite);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id) {
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	auto& buffer = m_slot_buffers[id];
	PageTable::PageRange pages {};
	EXIT_IF(!(GuestRange {buffer.CpuAddress(), buffer.Size()}.Valid()) ||
	        !PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	const auto first_page = PageIndex(buffer.CpuAddress());
	if constexpr (insert) {
		const auto [it, inserted] = m_buffers.emplace(buffer.CpuAddress(), id);
		(void)it;
		EXIT_IF(!inserted);
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		if (SparseBdaEnabled()) {
			for (uint64_t position = 0; position < size_pages;) {
				const auto page = first_page + position;
				const auto leaf_index = page >> BDA_LEAF_BITS;
				const auto leaf_offset = page & (BDA_LEAF_ENTRIES - 1);
				const auto count = std::min<uint64_t>(size_pages - position,
				                                      BDA_LEAF_ENTRIES - leaf_offset);
				auto [leaf, offset] = GetBdaLeaf(leaf_index);
				WriteDataBuffer(leaf, offset + leaf_offset * sizeof(vk::DeviceAddress),
				                addresses.data() + position, count * sizeof(vk::DeviceAddress));
				position += count;
			}
		} else {
			WriteDataBuffer(m_bda_pagetable_buffer, first_page * sizeof(vk::DeviceAddress),
			                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
		}
	} else {
		const auto found = m_buffers.find(buffer.CpuAddress());
		EXIT_IF(found == m_buffers.end() || found->second != id);
		m_buffers.erase(found);
		EXIT_IF(buffer.Size() > m_total_used_memory);
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		if (SparseBdaEnabled()) {
			for (uint64_t position = 0; position < size_pages;) {
				const auto page = first_page + position;
				const auto leaf_index = page >> BDA_LEAF_BITS;
				const auto leaf_offset = page & (BDA_LEAF_ENTRIES - 1);
				const auto count = std::min<uint64_t>(size_pages - position,
				                                      BDA_LEAF_ENTRIES - leaf_offset);
				auto [leaf, offset] = GetBdaLeaf(leaf_index);
				leaf.Fill(offset + leaf_offset * sizeof(vk::DeviceAddress),
				          count * sizeof(vk::DeviceAddress), 0);
				position += count;
			}
		} else {
			m_bda_pagetable_buffer.Fill(first_page * sizeof(vk::DeviceAddress),
			                            size_pages * sizeof(vk::DeviceAddress), 0);
		}
		buffer.is_deleted = true;
	}
	++m_register_epoch;
	m_scheduler.ProfileBufferUse(insert ? "buffer_owner_register" : "buffer_owner_retire",
	    buffer.CpuAddress(), buffer.Size(), std::bit_cast<uint64_t>(buffer.Handle()));
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	if (!buffer.is_deleted) {
		m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
	}
}

void BufferCache::DeleteBuffer(BufferId id) {
	if (IsBufferInvalid(id)) {
		return;
	}
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

// KYTY_UMA_READBACK=1 reads GPU-written buffers straight from host-visible memory.
static bool UmaReadbackEnabled() {
#if defined(__APPLE__)
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_UMA_READBACK");
		if (value != nullptr) {
			return value[0] != '0';
		}
		return true;
	}();
	return enabled;
#else
	return false;
#endif
}

// [readback] summary every 4 s: UMA reads whose writer was done / still running / not yet
// submitted, and full-drain copies.
static void NoteReadbackKind(int kind) {
	static std::atomic<uint64_t> counts[4] {};
	static std::atomic<int64_t>  next_print {0};
	counts[kind].fetch_add(1, std::memory_order_relaxed);
	const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
	                     std::chrono::steady_clock::now().time_since_epoch()).count();
	auto expected = next_print.load(std::memory_order_relaxed);
	if (now >= expected && next_print.compare_exchange_strong(expected, now + 4000)) {
		std::fprintf(stderr, "[readback] uma_done=%llu uma_running=%llu uma_unsubmitted=%llu drain=%llu\n",
		             static_cast<unsigned long long>(counts[0].exchange(0)),
		             static_cast<unsigned long long>(counts[1].exchange(0)),
		             static_cast<unsigned long long>(counts[2].exchange(0)),
		             static_cast<unsigned long long>(counts[3].exchange(0)));
	}
}

void BufferCache::NoteWriterTick(uint64_t vaddr, uint64_t size) {
	if (!UmaReadbackEnabled() || size == 0) {
		return;
	}
	const auto tick = m_scheduler.CurrentTick();
	const auto end  = vaddr + size;
	auto       it   = m_writer_ticks.lower_bound(vaddr);
	if (it != m_writer_ticks.begin() && std::prev(it)->second.first > vaddr) {
		it = std::prev(it);
	}
	while (it != m_writer_ticks.end() && it->first < end) {
		const auto [begin, value] = *it;
		it = m_writer_ticks.erase(it);
		if (begin < vaddr) {
			m_writer_ticks.emplace(begin, std::pair {vaddr, value.second});
		}
		if (value.first > end) {
			m_writer_ticks.emplace(end, std::pair {value.first, value.second});
		}
	}
	m_writer_ticks.emplace(vaddr, std::pair {end, tick});
	// Ranges whose writer completed need no wait; drop them to keep lookups cheap.
	if (m_writer_ticks.size() > 65536) {
		auto& master = m_scheduler.GetMasterSemaphore();
		master.Refresh();
		const auto known = master.KnownGpuTick();
		for (auto entry = m_writer_ticks.begin(); entry != m_writer_ticks.end();) {
			entry = entry->second.second <= known ? m_writer_ticks.erase(entry) : std::next(entry);
		}
	}
}

uint64_t BufferCache::WriterTick(uint64_t vaddr, uint64_t size) const {
	const auto end    = vaddr + size;
	uint64_t   result = 0;
	auto       it     = m_writer_ticks.lower_bound(vaddr);
	if (it != m_writer_ticks.begin() && std::prev(it)->second.first > vaddr) {
		it = std::prev(it);
	}
	for (; it != m_writer_ticks.end() && it->first < end; ++it) {
		result = std::max(result, it->second.second);
	}
	return result;
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Keep packed ranges on separate cache lines, as in shadPS4.
			    total_size += Common::AlignUp(end - start, 64);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
	    });
	if (copies.empty()) {
		return false;
	}
	if constexpr (!async) {
		if (t_sync_sample != nullptr) t_sync_sample->issued = true;
	}
	if constexpr (!async) {
		// Unified memory: the cache buffer is host-visible, so wait for the last writer of these
		// ranges and copy on the CPU instead of queueing a GPU copy behind all recorded work.
		if (UmaReadbackEnabled() && !DebugUmaReadbackOff() && !buffer.Mapped().empty() &&
		    buffer.IsCoherent() && !DrawCommitEnabled()) {
			uint64_t writer  = 0;
			bool     unknown = false;
			for (const auto& copy: copies) {
				const auto tick = WriterTick(buffer_address + copy.srcOffset, copy.size);
#if defined(__APPLE__)
				if (tick == 0) {
					writer = std::max(writer, m_scheduler.CurrentTick() > 0 ? m_scheduler.CurrentTick() - 1 : 0);
				} else {
					writer = std::max(writer, tick);
				}
#else
				unknown         = unknown || tick == 0;
				writer          = std::max(writer, tick);
#endif
			}
			if (!unknown) {
				ProfileCpuScope uma_profile(m_scheduler, "buffer_readback_uma", vaddr, total_size, true);
				const auto current = m_scheduler.CurrentTick();
				NoteReadbackKind(writer >= current ? 2 : (m_scheduler.IsFree(writer) ? 0 : 1));
				m_scheduler.Wait(std::min(writer, current));
				SyncReadbackSample* const sync = t_sync_sample;
				const uint64_t copy_begin = sync ? SyncTimingNowNs() : 0;
				m_scheduler.WaitPriorityOperations(std::min(writer, current));
				for (const auto& copy: copies) {
					Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
					                                      buffer.Mapped().data() + copy.srcOffset, copy.size);
				}
				if (sync) sync->copy_ns += SyncTimingNowNs() - copy_begin;
				return true;
			}
		}
	}
	if constexpr (!async) NoteReadbackKind(3);
	ProfileCpuScope readback_profile(m_scheduler, async ? "buffer_readback_async" : "buffer_readback_sync", vaddr, total_size, true);
	buffer.ProfileAllocation("readback_source_allocation");
	if (m_scheduler.ProfileWatched(vaddr, size)) {
		m_scheduler.ProfileEvent("readback_dependency_checkpoint", ProfileClockNs(), ProfileClockNs(),
		    std::bit_cast<uint64_t>(buffer.Handle()), m_scheduler.CurrentTick() - 1,
		    std::source_location::current());
		m_scheduler.ProfileEvent("readback_owner_epoch", ProfileClockNs(), ProfileClockNs(),
		    m_register_epoch, buffer.Size(), std::source_location::current());
		for (const auto& copy: copies)
			m_scheduler.ProfileReadbackSpan(buffer_address + copy.srcOffset,
			    copy.size, std::bit_cast<uint64_t>(buffer.Handle()));
	}

	auto [mapped, offset] = m_download_buffer.Map(total_size, 64);
	std::unique_ptr<Buffer> temporary;
	if (mapped == nullptr) {
		temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                     vk::BufferUsageFlagBits::eTransferDst, total_size);
		mapped = temporary->Mapped().data();
	} else {
		m_download_buffer.Commit();
	}
	const auto& download = temporary ? *temporary : m_download_buffer;
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto gpu_timer = m_scheduler.StartGpuTimer("download", m_scheduler.GpuFrameHint());
	const auto              native = command.Handle();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	ProfilePipelineBarrier(native, vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	const auto profile_copy_1 = m_scheduler.StartGpuTimer(m_scheduler.ProfileWatched(vaddr, total_size)
	    ? "copy_target_readback" : "copy_copyBuffer", m_scheduler.GpuFrameHint(), total_size);
	native.copyBuffer(buffer.Handle(), download.Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());
	m_scheduler.EndGpuTimer(profile_copy_1);

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.Handle();
	after.offset        = offset;
	after.size          = total_size;
	ProfilePipelineBarrier(native, vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	m_scheduler.EndGpuTimer(gpu_timer);
	auto publish = [this, mapped, offset, total_size, buffer_address,
	                copies = std::move(copies), owner = std::move(temporary)] {
		ProfileCpuScope publish_profile(m_scheduler, "readback_publish_cpu", buffer_address, total_size);
		(owner ? *owner : m_download_buffer).Invalidate(offset, total_size);
		for (const auto& copy: copies) {
			if constexpr (!async) m_scheduler.ProfileReadbackData(buffer_address + copy.srcOffset,
			    mapped + (copy.dstOffset - offset), copy.size);
			Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
			                                      mapped + (copy.dstOffset - offset), copy.size);
			m_scheduler.ProfileBufferUse("readback_published", buffer_address + copy.srcOffset, copy.size, 0);
		}
	};
	if constexpr (async) {
		m_scheduler.DeferPriorityOperation(std::move(publish));
	} else {
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		SyncReadbackSample* const sync = t_sync_sample;
		const uint64_t copy_begin = sync ? SyncTimingNowNs() : 0;
		m_scheduler.WaitPriorityOperations(tick);
		publish();
		if (sync) sync->copy_ns += SyncTimingNowNs() - copy_begin;
	}
	return true;
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             SparseBdaEnabled() ? BDA_ROOT_SIZE : BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));
}

BufferCache::~BufferCache() {
	m_cpu_label_stop.store(true, std::memory_order_release);
	if (m_cpu_label_thread.joinable()) {
		m_cpu_label_thread.join();
	}
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
	m_buffers.clear();
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	m_scheduler.ProfileBufferUse("cpu_invalidate_intent", vaddr, size, 0);
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid memory-invalidation range\n");
	}
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write, std::source_location site) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto request_thread = m_scheduler.ProfileLifetime() ? ProfileThreadId() : 0;
	const auto request_begin = m_scheduler.ProfileLifetime() ? ProfileClockNs() : 0;
	const auto* request_context = ProfileThread().operation;
	const uint64_t sync_entry = SyncTimingEnabled() ? SyncTimingNowNs() : 0;
	if (!m_scheduler.Context().HasGpu()) {
		// GPU test harness: no GuestGpu, so there is no GPU thread to marshal onto and no frame
		// loop holding a command buffer open. Running the download inline would reach
		// Finish() -> Submit() with an invalid command. Tests own their buffer contents directly
		// and never observe guest memory, so there is nothing to download back.
		return;
	}
	SyncReadbackSample sync_sample;
	m_scheduler.Context().GetGpu().SendCommandSync([this, vaddr, size, is_write, site, request_thread, request_begin, request_context, sync_entry, &sync_sample] {
		SyncReadbackScope sync_scope(sync_entry ? &sync_sample : nullptr);
		if (sync_entry) sync_sample.marshal_ns = SyncTimingNowNs() - sync_entry;
		ProfileCpuScope request(m_scheduler, "readback_request_cpu", vaddr, size, false, site);
		if (m_scheduler.ProfileWatched(vaddr, size)) {
			request.NewTransaction(vaddr);
			ProfileThread().operation = request_context;
			const auto now = ProfileClockNs();
			m_scheduler.ProfileEvent("readback_request_origin", request_begin, now, request_thread, size, site);
			m_scheduler.ProfileEvent(is_write ? "readback_write_fault" : "readback_read_request", now, now, vaddr, size, site);
		}
		if (is_write && !IsRegionRegistered(vaddr, size)) {
			return;
		}
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];

		// Read back only the requested range (upstream 0ce84e59): a widened window drained unrelated writers too.
		if (!is_write && DebugNoReadback()) {
			// Unprotect without downloading: the guest reads stale memory, the GPU copy stays valid.
			m_gpu_modified_ranges.Subtract(vaddr, size);
			m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
			return;
		}
		// Only meaningful with the import flags; the default path never evaluates this.
		const bool import_dirty = GuestImportEnabled() && m_memory_tracker.IsRegionGpuModified(vaddr, size);
		if (ImportFilterEnabled()) {
			if (m_lazy_label_pages.Intersects(vaddr, size)) {
				++m_import_stats.lazy_label_polls;
				NotePolledLabelPages(vaddr, size);
			}
			FilterImportedPages(vaddr, size, buffer.CpuAddress(), buffer.CpuAddress() + buffer.Size());
		}
		if (buffer.IsImported()) {
			// Imported pages already hold the GPU's bytes in guest memory: nothing to download.
			if (import_dirty) {
				++m_import_stats.readbacks_skipped;
			}
			m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
		} else if (DownloadBufferMemory<false>(buffer, vaddr, size)) {
			++m_import_stats.readbacks;
			m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
		}
		if (is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		}
	});
	// Only a readback that recorded a copy and blocked on it counts as a synchronous drain.
	if (sync_entry && sync_sample.issued) {
		sync_sample.total_ns = SyncTimingNowNs() - sync_entry;
		SyncTimingRecord(sync_sample);
	}
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	ProfileDetailScope profile("cpu_buffer_find", vaddr, size);
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			ProfileDetailEvent("cache_buffer_hit", vaddr, size);
			return *owner;
		}
	}
	ProfileDetailEvent("cache_buffer_miss", vaddr, size);
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Reserve space in the incoming stream's direction of growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, (vaddr < LOWER_ADDRESS_SIZE ? LOWER_ADDRESS_SIZE
				                                       : LibKernel::Memory::kExtendedMemoryBase +
				                                             LibKernel::Memory::kExtendedMemorySize) - end);
			}
			if (expands_right) {
				const auto minimum = vaddr < LOWER_ADDRESS_SIZE
				                         ? CACHING_PAGESIZE * 2
				                         : LibKernel::Memory::kExtendedMemoryBase;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	NoteWriterTick(overlap.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	ProfileDetailScope profile("cpu_buffer_create", vaddr, size);
	EXIT_IF(m_scheduler.Current().IsInvalid());
	const auto end = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	vaddr = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	const auto         usage = AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress;
	GuestImportBinding binding {};
	// Imported buffers alias the same guest bytes, so overlapping buffers are absorbed without a
	// copy of imported data. Only a non-contiguous guest range falls back to a device buffer.
	const bool may_import = GuestImportEnabled() && ImportAll() && !ImportKeepUploads() &&
	    !(ImportNoAdopt() && overlap.first != overlap.last);
	const bool imported = may_import &&
	    TryGuestImportBinding(overlap.begin, overlap.end - overlap.begin, binding);
	const auto id = imported
	    ? m_slot_buffers.insert(m_graphics, m_scheduler, overlap.begin, usage,
	                            overlap.end - overlap.begin, binding.memory, binding.offset,
	                            binding.memory_type, binding.memory_properties)
	    : m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	                            usage, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		if (imported) {
			AdoptIntoImported(id, old_id, !overlap.has_stream_leap);
		} else {
			if (GuestImportEnabled()) {
				ReleaseImportedToDevice(old_id);
				++m_import_stats.device_overlap_joins;
			}
			JoinOverlap(id, old_id, !overlap.has_stream_leap);
		}
	}
	Register(id);
	return id;
}

// Guest-memory import: one VkDeviceMemory importing the whole shared backing alias, created on
// first use. Imported buffers bind into it at the backing offset of their guest range.
bool BufferCache::EnsureGuestImport() {
	auto& gi = m_guest_import;
	if (gi.attempted) {
		return gi.ready;
	}
	gi.attempted = true;
	if (!m_graphics.external_memory_host_enabled) {
		std::fprintf(stderr, "[import] VK_EXT_external_memory_host not enabled; buffers stay device-owned\n");
		return false;
	}
	if (!Libs::LibKernel::Memory::GetGuestBackingAlias(&gi.base, &gi.size) || gi.base % 4096 != 0) {
		std::fprintf(stderr, "[import] guest backing alias unavailable; buffers stay device-owned\n");
		return false;
	}
	VkMemoryHostPointerPropertiesEXT host_properties {
	    VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
	if (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetMemoryHostPointerPropertiesEXT == nullptr ||
	    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetMemoryHostPointerPropertiesEXT(
	        static_cast<VkDevice>(m_graphics.device), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
	        reinterpret_cast<const void*>(gi.base), &host_properties) != VK_SUCCESS) {
		std::fprintf(stderr, "[import] vkGetMemoryHostPointerPropertiesEXT failed; buffers stay device-owned\n");
		return false;
	}
	// Buffer requirements for the usage every guest buffer uses.
	vk::BufferCreateInfo probe_info {};
	probe_info.size  = CACHING_PAGESIZE;
	probe_info.usage = AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress;
	vk::Buffer probe = nullptr;
	if (m_graphics.device.createBuffer(&probe_info, nullptr, &probe) != vk::Result::eSuccess) {
		return false;
	}
	const auto requirements = m_graphics.device.getBufferMemoryRequirements(probe);
	m_graphics.device.destroyBuffer(probe, nullptr);
	const uint32_t bits = host_properties.memoryTypeBits & requirements.memoryTypeBits;
	if (bits == 0) {
		std::fprintf(stderr, "[import] no memory type is both host-importable and buffer-compatible\n");
		return false;
	}
	uint32_t type = 0;
	while (((bits >> type) & 1u) == 0) {
		++type;
	}
	vk::ImportMemoryHostPointerInfoEXT import_info {};
	import_info.handleType   = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;
	import_info.pHostPointer = reinterpret_cast<void*>(gi.base);
	vk::MemoryAllocateInfo allocate_info {};
	allocate_info.pNext           = &import_info;
	allocate_info.allocationSize  = gi.size;
	allocate_info.memoryTypeIndex = type;
	if (m_graphics.device.allocateMemory(&allocate_info, nullptr, &gi.memory) != vk::Result::eSuccess ||
	    gi.memory == nullptr) {
		std::fprintf(stderr, "[import] vkAllocateMemory(import) failed; buffers stay device-owned\n");
		return false;
	}
	gi.memory_type       = type;
	gi.memory_properties = static_cast<uint32_t>(static_cast<VkMemoryPropertyFlags>(
	    m_graphics.physical_device_memory_properties.memoryTypes[type].propertyFlags));
	gi.alignment         = requirements.alignment == 0 ? 1 : requirements.alignment;
	gi.ready             = true;
	std::fprintf(stderr, "[import] imported guest backing base=0x%llx size=0x%llx memoryType=%u alignment=%llu\n",
	             static_cast<unsigned long long>(gi.base), static_cast<unsigned long long>(gi.size),
	             type, static_cast<unsigned long long>(gi.alignment));
	return true;
}

bool BufferCache::TryGuestImportBinding(uint64_t vaddr, uint64_t size, GuestImportBinding& out) {
	if (!EnsureGuestImport()) {
		++m_import_stats.fallback;
		++m_import_stats.fallback_import;
		return false;
	}
	uint64_t offset = 0;
	if (!Libs::LibKernel::Memory::TryGetGuestBackingOffset(vaddr, size, &offset)) {
		++m_import_stats.fallback;
		++m_import_stats.fallback_backing;
		return false;
	}
	const auto& gi = m_guest_import;
	if (offset % gi.alignment != 0 || size > gi.size || offset > gi.size - size) {
		++m_import_stats.fallback;
		++m_import_stats.fallback_alignment;
		return false;
	}
	out = {gi.memory, offset, gi.memory_type, gi.memory_properties};
	++m_import_stats.bufs_imported;
	return true;
}

// Records that [vaddr, vaddr+size) was last written at `tick` (0: written by a device buffer,
// which takes ownership of the pages). Pages are tracked at tracker granularity.
void BufferCache::NoteImportWrite(uint64_t vaddr, uint64_t size, uint64_t tick) {
	if (size == 0) {
		return;
	}
	const auto begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	auto       it    = m_import_writes.lower_bound(begin);
	if (it != m_import_writes.begin() && std::prev(it)->second.first > begin) {
		it = std::prev(it);
	}
	while (it != m_import_writes.end() && it->first < end) {
		const auto piece_begin = it->first;
		const auto piece_end   = it->second.first;
		const auto piece_tick  = it->second.second;
		it                     = m_import_writes.erase(it);
		if (piece_begin < begin) {
			m_import_writes.emplace(piece_begin, std::pair {begin, piece_tick});
		}
		if (piece_end > end) {
			m_import_writes.emplace(end, std::pair {piece_end, piece_tick});
		}
	}
	if (tick != 0) {
		m_import_writes.emplace(begin, std::pair {end, tick});
	}
}

// Unprotects pending imported pieces inside [begin, end) whose writer tick is <= done.
uint64_t BufferCache::ReleaseImportPieces(uint64_t begin, uint64_t end, uint64_t done) {
	uint64_t released = 0;
	auto     it       = m_import_writes.lower_bound(begin);
	if (it != m_import_writes.begin() && std::prev(it)->second.first > begin) {
		it = std::prev(it);
	}
	while (it != m_import_writes.end() && it->first < end) {
		const auto piece_begin = it->first;
		const auto piece_end   = it->second.first;
		const auto piece_tick  = it->second.second;
		if (piece_tick > done) {
			++it;
			continue;
		}
		const auto clip_begin = std::max(piece_begin, begin);
		const auto clip_end   = std::min(piece_end, end);
		m_memory_tracker.UnmarkRegionAsGpuModified(clip_begin, clip_end - clip_begin);
		it = m_import_writes.erase(it);
		if (piece_begin < begin) {
			m_import_writes.emplace(piece_begin, std::pair {begin, piece_tick});
		}
		if (piece_end > end) {
			m_import_writes.emplace(end, std::pair {piece_end, piece_tick});
		}
		++released;
	}
	return released;
}

// Filter: a guest fault on imported pages. If their newest writer has completed, release them
// with no wait; otherwise wait only for that writer (Wait submits the open command buffer only
// when the writer is in it). The rest of the buffer's completed pages are released too.
void BufferCache::FilterImportedPages(uint64_t vaddr, uint64_t size, uint64_t buffer_begin,
                                      uint64_t buffer_end) {
	if (m_import_writes.empty() || size == 0) {
		return;
	}
	const auto begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	const auto first_piece = [this](uint64_t position) {
		auto it = m_import_writes.lower_bound(position);
		if (it != m_import_writes.begin() && std::prev(it)->second.first > position) {
			it = std::prev(it);
		}
		return it;
	};
	auto& master = m_scheduler.GetMasterSemaphore();
	master.Refresh();
	auto     done      = master.KnownGpuTick();
	uint64_t wait_tick = 0;
	bool     pending   = false;
	for (auto it = first_piece(begin); it != m_import_writes.end() && it->first < end; ++it) {
		pending = true;
		if (it->second.second > done) {
			wait_tick = std::max(wait_tick, it->second.second);
		}
	}
	if (!pending) {
		return;
	}
	if (wait_tick != 0) {
		const auto start = std::chrono::steady_clock::now();
		m_scheduler.Wait(wait_tick);
		m_import_stats.filter_wait_ns += static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
		        .count());
		++m_import_stats.filter_waited;
		master.Refresh();
		done = master.KnownGpuTick();
	} else {
		++m_import_stats.filter_instant;
	}
	ReleaseImportPieces(begin, end, done);
	if (buffer_begin < buffer_end) {
		ReleaseImportPieces(Common::AlignDown(buffer_begin, TRACKER_PAGE_SIZE),
		                    Common::AlignUp(buffer_end, TRACKER_PAGE_SIZE), done);
	}
}

// KYTY_LABEL_BUDGET_US: how long a label may wait in an open submission before the GPU thread
// submits it (default 1000 us). Serves as the label watchdog window and the lazy-label budget.
static int64_t LabelWindowNs() {
	static const int64_t ns = [] {
		const char* value = std::getenv("KYTY_LABEL_BUDGET_US");
		const long long us = value != nullptr ? std::atoll(value) : 1000;
		return us > 0 ? static_cast<int64_t>(us) * 1000 : int64_t {1'000'000};
	}();
	return ns;
}

int BufferCache::TryRecordGpuLabelWrite(CommandBuffer& command, uint64_t address,
                                        const void* value, uint32_t size, bool lazy) {
	if (!GpuLabelsEnabled() || value == nullptr || (size != 4 && size != 8) ||
	    (address & (size - 1)) != 0) {
		return 0;
	}
	// Device-owned GPU-written bytes would be overwritten by this label on their download.
	if (m_gpu_modified_ranges.Intersects(address, size)) {
		return 0;
	}
	uint64_t offset = 0;
	if (!Libs::LibKernel::Memory::TryGetGuestBackingOffset(address, size, &offset) ||
	    !EnsureGuestImport()) {
		return 0;
	}
	if (m_label_buffer == nullptr) {
		// One VkBuffer over the whole imported backing; labels address it by backing offset.
		m_label_buffer = std::make_unique<Buffer>(m_graphics, m_scheduler, uint64_t {0},
		    vk::BufferUsageFlagBits::eTransferDst, m_guest_import.size, m_guest_import.memory,
		    uint64_t {0}, m_guest_import.memory_type, m_guest_import.memory_properties);
	}
	const auto tick = m_scheduler.CurrentTick();
	const auto now  = std::chrono::duration_cast<std::chrono::nanoseconds>(
	                      std::chrono::steady_clock::now().time_since_epoch()).count();
	// A lazy label is observable: its page stays GPU-dirty, so a guest poll faults and the filter
	// waits for this tick (which submits the open buffer). Otherwise the caller submits now.
	int  result = 2;
	const auto page = Common::AlignDown(address, TRACKER_PAGE_SIZE);
	if (lazy && ImportFilterEnabled() && !m_gpu_modified_ranges.Intersects(page, TRACKER_PAGE_SIZE) &&
	    !m_memory_tracker.IsRegionCpuModified(page, TRACKER_PAGE_SIZE)) {
		m_memory_tracker.MarkRegionAsGpuModified(page, TRACKER_PAGE_SIZE);
		NoteImportWrite(page, TRACKER_PAGE_SIZE, tick);
		m_lazy_label_pages.Add(page, TRACKER_PAGE_SIZE);
		result = 1;
	}
	if (lazy && m_last_label_tick != tick) {
		m_last_label_tick = tick;
		m_label_since_ns  = now;
	}
	if (lazy && !m_cpu_label_thread.joinable()) {
		// The watchdog that submits lazy labels within LabelWindowNs() (see CpuLabelLoop).
		m_cpu_label_thread = std::thread([this] { CpuLabelLoop(); });
	}
	command.EndRendering();
	const auto native = command.Handle();
	// Every earlier write must complete before the label becomes visible.
	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
	ProfilePipelineBarrier(native, vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 1, &before, 0, nullptr, 0,
	                       nullptr);
	native.updateBuffer(m_label_buffer->Handle(), offset, size, value);
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	ProfilePipelineBarrier(native, vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eHost, {}, 1, &after, 0, nullptr, 0, nullptr);
	return result;
}

bool BufferCache::DeferCpuLabel(uint64_t address, uint64_t value, uint32_t size) {
	if ((size != 4 && size != 8) || (address & (size - 1)) != 0) {
		return false;
	}
	uint64_t offset = 0;
	if (!Libs::LibKernel::Memory::TryGetGuestBackingOffset(address, size, &offset)) {
		return false;
	}
	const auto tick = m_scheduler.CurrentTick();
	const auto now  = std::chrono::duration_cast<std::chrono::nanoseconds>(
	                      std::chrono::steady_clock::now().time_since_epoch()).count();
	{
		std::lock_guard lock {m_cpu_label_mutex};
		m_cpu_labels.push_back({tick, address, value, size, now});
	}
	if (m_last_label_tick != tick) {
		m_last_label_tick = tick;
		m_label_since_ns  = now;
	}
	if (!m_cpu_label_thread.joinable()) {
		m_cpu_label_thread = std::thread([this] { CpuLabelLoop(); });
	}
	return true;
}

// Label thread: publishes each CPU label once its tick completes, and is the watchdog for work
// queued into the open tick: CPU labels, GPU-recorded lazy labels (KYTY_LABEL_COALESCE) and
// priority operations (interrupts, flips, suspend points). The GPU thread submits that tick once it
// has been open for LabelWindowNs(), and again every window while it stays open. The decision is
// taken from the open tick, not from the oldest label: a label whose tick is already in flight must
// not hide work queued behind it.
// KYTY_LABEL_LOG=1: log each watchdog request and dump the state when a label has waited over 1 s.
void BufferCache::CpuLabelLoop() {
	const int64_t WatchdogNs      = LabelWindowNs();
	constexpr int64_t StallDumpNs = 1'000'000'000;
	static const bool log_enabled = [] {
		const char* value = std::getenv("KYTY_LABEL_LOG");
		return value != nullptr && value[0] == '1';
	}();
	auto&    master        = m_scheduler.GetMasterSemaphore();
	uint64_t open_seen     = 0;  // label thread only
	int64_t  open_seen_ns  = 0;  // label thread only
	int64_t  dumped_ns     = 0;  // label thread only
	const auto now_ns = [] {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
		           std::chrono::steady_clock::now().time_since_epoch())
		    .count();
	};
	while (!m_cpu_label_stop.load(std::memory_order_acquire)) {
		ScopedAutoreleasePool autorelease_pool;
		CpuLabel front {};
		uint64_t newest_tick = 0;
		{
			std::lock_guard lock {m_cpu_label_mutex};
			if (!m_cpu_labels.empty()) {
				front       = m_cpu_labels.front();
				newest_tick = m_cpu_labels.back().tick;
			}
		}
		const int64_t  now            = now_ns();
		const uint64_t open           = m_scheduler.CurrentTick();
		const bool     priority_open  = m_scheduler.HasPriorityOperationAtCurrentTick();
		if (open != open_seen) {
			open_seen    = open;
			open_seen_ns = now;
		}
		const bool open_work = (newest_tick != 0 && newest_tick >= open) || priority_open ||
		                       LabelPendingInOpen();
		if (!open_work && front.tick == 0) {
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			continue;
		}
		master.Refresh();
		const bool due = open != m_cpu_label_flush_tick || now - m_cpu_label_flush_ns > WatchdogNs;
		if (open_work && due && now - open_seen_ns > WatchdogNs &&
		    !m_cpu_label_stop.load(std::memory_order_acquire) && m_scheduler.Context().HasGpu()) {
			m_cpu_label_flush_tick = open;
			m_cpu_label_flush_ns   = now;
			if (log_enabled) {
				std::fprintf(stderr,
				             "[label] watchdog: open tick %llu open for %lld us, requesting submit"
				             " (labels_in_open=%d priority_in_open=%d gpu_known=%llu)\n",
				             static_cast<unsigned long long>(open),
				             static_cast<long long>((now - open_seen_ns) / 1000),
				             newest_tick >= open ? 1 : 0, priority_open ? 1 : 0,
				             static_cast<unsigned long long>(master.KnownGpuTick()));
			}
			m_scheduler.Context().GetGpu().SendCommand([this] { FlushOpenLabelBuffer(); });
		}
		if (front.tick == 0) {
			std::this_thread::sleep_for(std::chrono::microseconds(50));
			continue;
		}
		if (master.KnownGpuTick() < front.tick) {
			const int64_t age = now - front.enqueued_ns;
			if (age > 100'000'000 && m_cpu_label_report_tick != front.tick) {
				m_cpu_label_report_tick = front.tick;
				std::fprintf(stderr,
				             "[label] cpu label queued >100 ms: addr=0x%llx size=%u tick=%llu"
				             " gpu_known=%llu open_tick=%llu\n",
				             static_cast<unsigned long long>(front.address), front.size,
				             static_cast<unsigned long long>(front.tick),
				             static_cast<unsigned long long>(master.KnownGpuTick()),
				             static_cast<unsigned long long>(open));
			}
			if (age > StallDumpNs && now - dumped_ns > StallDumpNs) {
				dumped_ns = now;
				std::fprintf(stderr,
				             "[label] stall: front addr=0x%llx tick=%llu age=%lld ms gpu_known=%llu"
				             " open_tick=%llu open_for=%lld us labels_in_open=%d priority_in_open=%d"
				             " last_label_tick=%llu flush_tick=%llu flush_age=%lld us"
				             " pm4_wait_addr=0x%llx pm4_wait_retries=%llu\n",
				             static_cast<unsigned long long>(front.address),
				             static_cast<unsigned long long>(front.tick),
				             static_cast<long long>(age / 1'000'000),
				             static_cast<unsigned long long>(master.KnownGpuTick()),
				             static_cast<unsigned long long>(open),
				             static_cast<long long>((now - open_seen_ns) / 1000),
				             newest_tick >= open ? 1 : 0, priority_open ? 1 : 0,
				             static_cast<unsigned long long>(m_last_label_tick.load()),
				             static_cast<unsigned long long>(m_cpu_label_flush_tick),
				             static_cast<long long>((now - m_cpu_label_flush_ns) / 1000),
				             static_cast<unsigned long long>(g_pm4_wait_addr.load()),
				             static_cast<unsigned long long>(g_pm4_wait_retries.load()));
			}
			std::this_thread::sleep_for(std::chrono::microseconds(50));
			continue;
		}
		std::vector<CpuLabel> ready;
		{
			std::lock_guard lock {m_cpu_label_mutex};
			while (!m_cpu_labels.empty() && m_cpu_labels.front().tick <= master.KnownGpuTick()) {
				ready.push_back(m_cpu_labels.front());
				m_cpu_labels.pop_front();
			}
		}
		for (const auto& label: ready) {
			uint64_t value = label.value;
			Libs::LibKernel::Memory::WriteBacking(label.address, &value, label.size);
		}
		if (!ready.empty() && !m_cpu_label_stop.load(std::memory_order_acquire) &&
		    m_scheduler.Context().HasGpu()) {
			// Wake the GPU thread so a suspended WAIT_REG_MEM on these labels retries now.
			m_scheduler.Context().GetGpu().SendCommand([] {});
		}
	}
}

void BufferCache::FlushOpenLabelBuffer() {
	if (!m_scheduler.Active()) {
		return;
	}
	// The guest waits on labels and on priority operations (interrupts, flips, suspend points)
	// queued into the open tick alike. Either one is reason enough to submit that tick.
	if (LabelPendingInOpen() || m_scheduler.HasPriorityOperationAtCurrentTick()) {
		NoteLazyFlush(LazyFlush::Watchdog);
		m_scheduler.Flush();
	}
}

// True when every submitted buffer has completed and the open buffer has work: submitting it
// cannot make the GPU wait on anything it does not already have.
bool BufferCache::GpuIdleWithOpenWork(bool has_draws) {
	if (!has_draws && !LabelPendingInOpen()) {
		return false;
	}
	auto& master = m_scheduler.GetMasterSemaphore();
	master.Refresh();
	return master.KnownGpuTick() + 1 >= m_scheduler.CurrentTick();
}

bool BufferCache::LabelPendingInOpen() const {
	return m_last_label_tick != 0 && m_last_label_tick == m_scheduler.CurrentTick();
}

bool BufferCache::LabelBudgetExceeded() const {
	const int64_t BudgetNs = LabelWindowNs();
	if (!LabelPendingInOpen()) {
		return false;
	}
	const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
	                     std::chrono::steady_clock::now().time_since_epoch()).count();
	return now - m_label_since_ns > BudgetNs;
}

void BufferCache::NoteLazyFlush(LazyFlush reason) {
	switch (reason) {
		case LazyFlush::Budget: ++m_import_stats.lazy_budget; break;
		case LazyFlush::WaitRegMem: ++m_import_stats.lazy_wait_regmem; break;
		case LazyFlush::Other: ++m_import_stats.lazy_other; break;
		case LazyFlush::Watchdog: ++m_import_stats.lazy_watchdog; break;
	}
}

void BufferCache::DropImportedBuffers(uint64_t vaddr, uint64_t size) {
	if (!GuestImportEnabled() || size == 0) {
		return;
	}
	const auto end = vaddr + size;
	std::vector<BufferId> doomed;
	auto                  it = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		const auto& buffer = m_slot_buffers[it->second];
		if (buffer.IsImported() && buffer.CpuAddress() + buffer.Size() > vaddr) {
			doomed.push_back(it->second);
		}
	}
	for (const auto id: doomed) {
		DeleteBuffer(id);
	}
}

BufferId BufferCache::ConvertToImported(BufferId id) {
	const auto begin = m_slot_buffers[id].CpuAddress();
	const auto size  = m_slot_buffers[id].Size();
	if (m_no_alias.Intersects(begin, size)) {
		return id;
	}
	GuestImportBinding binding {};
	if (!TryGuestImportBinding(begin, size, binding)) {
		return id;  // non-contiguous backing: the range keeps its readback path
	}
	const auto usage  = AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress;
	const auto imp_id = m_slot_buffers.insert(m_graphics, m_scheduler, begin, usage, size,
	    binding.memory, binding.offset, binding.memory_type, binding.memory_properties);
	// Copy the device's GPU-written bytes into guest memory in GPU order; then the alias owns them.
	AdoptIntoImported(imp_id, id, false);
	Register(imp_id);
	// CPU-dirty bytes already sit in guest memory: consume them so the alias is not re-uploaded.
	m_memory_tracker.ForEachUploadRange(begin, size, false, [](uint64_t, uint64_t) noexcept {},
	                                    []() noexcept {});
	++m_import_stats.aliased_ranges;
	return imp_id;
}

BufferId BufferCache::DemoteImported(BufferId id) {
	const auto begin = m_slot_buffers[id].CpuAddress();
	const auto size  = m_slot_buffers[id].Size();
	const auto usage = AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress;
	const auto dev_id = m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal,
	                                          begin, usage, size);
	// GPU-ordered copy of guest bytes: pending GPU writes to the alias execute before it.
	m_slot_buffers[dev_id].CopyFrom(m_scheduler.Current(), m_slot_buffers[id], 0, 0, size);
	ReleaseImportedToDevice(id);
	DeleteBuffer(id);
	Register(dev_id);
	// CPU-dirty bytes changed after the alias was written: snapshot them now, after the copy.
	(void)SynchronizeBuffer(m_slot_buffers[dev_id], begin, size, false, false);
	m_no_alias.Add(begin, size);
	++m_import_stats.demotions;
	return dev_id;
}

void BufferCache::CheckImportedMapping(const Buffer& buffer) {
	uint64_t offset = 0;
	const bool same = Libs::LibKernel::Memory::TryGetGuestBackingOffset(buffer.CpuAddress(), buffer.Size(), &offset) &&
	    offset == buffer.ImportBackingOffset();
	++m_import_stats.checks;
	if (!same) {
		++m_import_stats.remap_mismatch;
		static std::atomic<int> printed {0};
		if (printed.fetch_add(1) < 16) {
			std::fprintf(stderr, "[import-check] guest 0x%llx size 0x%llx bound offset 0x%llx now 0x%llx\n",
			             static_cast<unsigned long long>(buffer.CpuAddress()),
			             static_cast<unsigned long long>(buffer.Size()),
			             static_cast<unsigned long long>(buffer.ImportBackingOffset()),
			             static_cast<unsigned long long>(offset));
		}
	}
}

void BufferCache::MaybePrintImportStats() {
	if (!GuestImportEnabled()) {
		return;
	}
	static const bool announced = [] {
		std::fprintf(stderr,
		             "[import] mode=%s no_adopt=%d keep_uploads=%d check=%d labels=%d\n",
		             ImportAll() ? "all-alias" : "hybrid", ImportNoAdopt() ? 1 : 0,
		             ImportKeepUploads() ? 1 : 0, ImportCheck() ? 1 : 0,
		             GpuLabelsEnabled() ? 1 : 0);
		return true;
	}();
	(void)announced;
	const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
	                     std::chrono::steady_clock::now().time_since_epoch()).count();
	if (now < m_import_stats.next_print_ns) {
		return;
	}
	static uint64_t last_submits = 0;
	static int64_t  last_ns      = 0;
	const auto      submits      = SubmitCountTotal();
	const double    window_s     = last_ns == 0 ? 2.0 : static_cast<double>(now - last_ns) / 1e9;
	const double    submits_per_s = static_cast<double>(submits - last_submits) / window_s;
	last_submits = submits;
	last_ns      = now;
	m_import_stats.next_print_ns = now + 2'000'000'000;
	const auto& s       = m_import_stats;
	const double avg_us = s.filter_waited != 0
	    ? static_cast<double>(s.filter_wait_ns) / 1000.0 / static_cast<double>(s.filter_waited)
	    : 0.0;
	std::fprintf(stderr,
	             "IMPORT aliased_ranges %llu demotions %llu readbacks %llu"
	             " filter_instant %llu filter_waited %llu avg_wait_us %.1f"
	             " lazy_flush budget %llu wait_regmem %llu other %llu watchdog %llu label_polls %llu"
	             " submits_per_s %.0f"
	             " bufs_imported %llu fallback %llu (backing %llu align %llu import %llu)"
	             " adopted %llu released_to_device %llu uploads_skipped %llu"
	             " checks %llu remap_mismatch %llu readbacks_skipped %llu\n",
	             static_cast<unsigned long long>(s.aliased_ranges),
	             static_cast<unsigned long long>(s.demotions),
	             static_cast<unsigned long long>(s.readbacks),
	             static_cast<unsigned long long>(s.filter_instant),
	             static_cast<unsigned long long>(s.filter_waited), avg_us,
	             static_cast<unsigned long long>(s.lazy_budget),
	             static_cast<unsigned long long>(s.lazy_wait_regmem),
	             static_cast<unsigned long long>(s.lazy_other),
	             static_cast<unsigned long long>(s.lazy_watchdog),
	             static_cast<unsigned long long>(s.lazy_label_polls), submits_per_s,
	             static_cast<unsigned long long>(s.bufs_imported),
	             static_cast<unsigned long long>(s.fallback),
	             static_cast<unsigned long long>(s.fallback_backing),
	             static_cast<unsigned long long>(s.fallback_alignment),
	             static_cast<unsigned long long>(s.fallback_import),
	             static_cast<unsigned long long>(s.adopted_overlaps),
	             static_cast<unsigned long long>(s.released_to_device),
	             static_cast<unsigned long long>(s.uploads_skipped),
	             static_cast<unsigned long long>(s.checks),
	             static_cast<unsigned long long>(s.remap_mismatch),
	             static_cast<unsigned long long>(s.readbacks_skipped));
}

// An overlapping buffer collapses into an imported alias. Imported bytes already live in guest
// memory and need no copy. A device buffer's GPU-written bytes exist only on the device, so
// exactly those are copied into guest memory through the new alias, in GPU order, and then
// tracked like imported writes. No sync download is issued here: the caller may be mid-recording.
void BufferCache::AdoptIntoImported(BufferId new_id, BufferId old_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& old        = m_slot_buffers[old_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(old.StreamScore() + 1);
	}
	++m_import_stats.adopted_overlaps;
	if (!old.IsImported()) {
		std::vector<std::pair<uint64_t, uint64_t>> dirty;
		m_gpu_modified_ranges.ForEachInRange(old.CpuAddress(), old.Size(),
		    [&](uint64_t begin, uint64_t end) { dirty.emplace_back(begin, end); });
		for (const auto& [begin, end]: dirty) {
			new_buffer.CopyFrom(m_scheduler.Current(), old, begin - old.CpuAddress(),
			                    begin - new_buffer.CpuAddress(), end - begin);
			m_gpu_modified_ranges.Subtract(begin, end - begin);
			++m_import_stats.adopt_copies;
			if (ImportFilterEnabled()) {
				NoteImportWrite(begin, end - begin, m_scheduler.CurrentTick());
			} else {
				m_memory_tracker.UnmarkRegionAsGpuModified(begin, end - begin);
			}
		}
	}
	DeleteBuffer(old_id);
}

// A device buffer that absorbs an imported buffer takes over that buffer's pending GPU writes:
// the pages become device-dirty, so the join copy and later downloads handle them as usual.
void BufferCache::ReleaseImportedToDevice(BufferId id) {
	const auto& buffer = m_slot_buffers[id];
	if (!buffer.IsImported() || !ImportFilterEnabled()) {
		return;
	}
	const auto begin = Common::AlignDown(buffer.CpuAddress(), TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(buffer.CpuAddress() + buffer.Size(), TRACKER_PAGE_SIZE);
	std::vector<std::pair<uint64_t, uint64_t>> pieces;
	auto it = m_import_writes.lower_bound(begin);
	if (it != m_import_writes.begin() && std::prev(it)->second.first > begin) {
		it = std::prev(it);
	}
	for (; it != m_import_writes.end() && it->first < end; ++it) {
		pieces.emplace_back(std::max(it->first, begin), std::min(it->second.first, end));
	}
	for (const auto& [piece_begin, piece_end]: pieces) {
		m_gpu_modified_ranges.Add(piece_begin, piece_end - piece_begin);
		NoteImportWrite(piece_begin, piece_end - piece_begin, 0);
		++m_import_stats.released_to_device;
	}
}

// KYTY_WATCH_DIRTY=<hex>[,<hex>...]: report GPU writes and CPU read faults on these pages.
static bool DirtyWatchHit(uint64_t vaddr, uint64_t size) {
	static const std::vector<uint64_t> pages = [] {
		std::vector<uint64_t> result;
		if (const char* env = std::getenv("KYTY_WATCH_DIRTY")) {
			for (const char* p = env; *p != '\0';) {
				char* end = nullptr;
				const auto value = std::strtoull(p, &end, 16);
				if (end == p) break;
				result.push_back(value / TRACKER_PAGE_SIZE);
				p = *end == ',' ? end + 1 : end;
			}
		}
		return result;
	}();
	if (pages.empty() || size == 0) return false;
	const auto first = vaddr / TRACKER_PAGE_SIZE, last = (vaddr + size - 1) / TRACKER_PAGE_SIZE;
	return std::any_of(pages.begin(), pages.end(), [&](uint64_t page) { return page >= first && page <= last; });
}

bool DebugDirtyWatchHit(uint64_t vaddr, uint64_t size) { return DirtyWatchHit(vaddr, size); }

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	if (is_written && DirtyWatchHit(vaddr, size)) {
		static std::atomic<int> printed {0};
		if (printed.fetch_add(1) < 400) {
			std::fprintf(stderr, "[dirty-watch] gpu-write 0x%llx+0x%llx shader=0x%016llx texel=%d\n",
			             static_cast<unsigned long long>(vaddr), static_cast<unsigned long long>(size),
			             static_cast<unsigned long long>(g_debug_shader_hash), is_texel_buffer ? 1 : 0);
		}
	}
	if (buffer.IsImported()) {
		// The alias already holds the CPU's bytes and the GPU reads guest memory directly, so no
		// upload. Consume CPU-dirty state; mark GPU writes only in filter mode.
		m_memory_tracker.ForEachUploadRange(
		    vaddr, size, is_written && ImportFilterEnabled(),
		    [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
		++m_import_stats.uploads_skipped;
		if (is_texel_buffer && !is_written) {
			return SynchronizeBufferFromImage(buffer, vaddr, size);
		}
		return false;
	}
	ProfileDetailScope profile("cpu_buffer_synchronize", vaddr, size);
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	vk::Buffer                  source;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    if (m_bda_counters) {
			    ++m_bda_counters->upload_calls; m_bda_counters->upload_bytes += bytes;
			    if (m_bda_expected_uploads && !m_bda_expected_uploads->Contains(address, bytes))
				    m_bda_counters->uncovered_upload_bytes += bytes;
		    }
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
	    },
	    [&]() noexcept { source = UploadCopies(buffer, copies, total_size); });
	if (source) {
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto native = command.Handle();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		ProfilePipelineBarrier(native, vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
		const auto profile_copy_2 = m_scheduler.StartGpuTimer("copy_copyBuffer", m_scheduler.GpuFrameHint());
		for (const auto& copy: copies) m_scheduler.ProfileBufferUse("gpu_writer_upload_exact",
		    buffer.CpuAddress() + copy.dstOffset, copy.size, std::bit_cast<uint64_t>(buffer.Handle()));
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		m_scheduler.EndGpuTimer(profile_copy_2);
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		ProfilePipelineBarrier(native, vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
	}
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	if (copies.empty()) {
		return nullptr;
	}

	auto [mapped, base_offset] = m_staging_buffer.Map(total_size, 4);
	if (mapped != nullptr) {
		for (auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			std::memcpy(mapped + copy.srcOffset, reinterpret_cast<const void*>(address), copy.size);
			copy.srcOffset += base_offset;
		}
		m_staging_buffer.Commit();
		return m_staging_buffer.Handle();
	}

	auto temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Upload, 0,
	                                         vk::BufferUsageFlagBits::eTransferSrc, total_size);
	for (const auto& copy: copies) {
		const auto address = buffer.CpuAddress() + copy.dstOffset;
		std::memcpy(temporary->Mapped().data() + copy.srcOffset,
		            reinterpret_cast<const void*>(address), copy.size);
	}
	temporary->Flush(0, total_size);
	const auto handle = temporary->Handle();
	m_scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return handle;
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
	ProfileDetailScope profile("cpu_buffer_obtain", vaddr, size);
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}

	if (!is_written && size <= CACHING_PAGESIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
			std::memcpy(mapped, reinterpret_cast<const void*>(vaddr), size);
			m_stream_buffer.Commit();
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	if (HybridImport()) {
		const auto& current = m_slot_buffers[id];
		if (current.IsImported()) {
			// Record-time snapshots cannot protect an alias from later CPU writes: demote it.
			if (m_memory_tracker.IsRegionCpuModified(current.CpuAddress(), current.Size())) {
				id = DemoteImported(id);
			}
		} else if (is_written) {
			id = ConvertToImported(id);
		}
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	if (ImportCheck() && buffer.IsImported()) {
		CheckImportedMapping(buffer);
	}
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer);
	if (is_written) {
		m_scheduler.ProfileBufferUse("gpu_writer_bind_pending", vaddr, size,
		    std::bit_cast<uint64_t>(buffer.Handle()));
		m_texture_cache.TraceColorMetaWrite(vaddr, size, "writable-buffer-bound");
		if (!buffer.IsImported()) {
			m_gpu_modified_ranges.Add(vaddr, size);
		} else if (!ImportFilterEnabled()) {
			// Without the filter the imported write is never downloaded: the download avoided.
			++m_import_stats.readbacks_skipped;
		}
		if (ImportFilterEnabled()) {
			NoteImportWrite(vaddr, size, buffer.IsImported() ? m_scheduler.CurrentTick() : 0);
		}
		NoteWriterTick(vaddr, size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	ProfileDetailScope profile("cpu_image_source_buffer", vaddr, size);
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid image source\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false);
	}

	auto [staging, stage_offset] = m_staging_buffer.Map(size, 16);
	if (staging == nullptr || !Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, staging, size)) {
		EXIT("BufferCache: failed to read mapped guest image backing\n");
	}
	m_staging_buffer.Commit();
	return {&m_staging_buffer, stage_offset};
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		CpTraceWrite(vaddr, size, "fill-cpu", value);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		m_texture_cache.TraceColorMetaFill(vaddr, size, value, false);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	CpTraceWrite(vaddr, size, "fill-gpu", value);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
	m_texture_cache.TraceColorMetaFill(vaddr, size, value, true);
}

void BufferCache::FillBufferFromProvenComputeFill(uint64_t vaddr, uint64_t size, uint32_t value) {
	if (!GuestRange {vaddr, size}.Valid() || (vaddr & (TRACKER_PAGE_SIZE - 1)) != 0 ||
	    (size & (TRACKER_PAGE_SIZE - 1)) != 0) {
		EXIT("BufferCache: proven compute fill requires aligned guest pages\n");
	}
	// The recognized dispatch overwrites every byte of this range. Publish its known result
	// directly to guest backing; the next GPU consumer uploads it in queue order.
	std::vector<uint32_t> words(size / sizeof(uint32_t), value);
	CpTraceWrite(vaddr, size, "proven-fill", value);
	LibKernel::Memory::WriteBacking(vaddr, words.data(), size);
	m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
	m_gpu_modified_ranges.Subtract(vaddr, size);
	m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		m_texture_cache.TraceColorMetaWrite(dst_vaddr, size, "copy-cpu", src_vaddr);
		if (CpTraceEnabled()) CpTraceWrite(dst_vaddr, size, "copy-cpu", *reinterpret_cast<const uint32_t*>(src_vaddr));
		std::memcpy(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr),
		            size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.TraceColorMetaWrite(dst_vaddr, size, "copy-gpu", src_vaddr);
		if (CpTraceEnabled()) CpTraceWrite(dst_vaddr, size, "copy-gpu", src_memory ? *reinterpret_cast<const uint32_t*>(src_vaddr) : 0u);
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Intersects(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::TickFrame() {
	++m_gc_tick;
	MaybePrintImportStats();
}

void BufferCache::RunGarbageCollector() {
	// Ages are presented frames. Collection still runs after every submission, and UFC submits
	// hundreds of times a frame, so a per-submission tick retired buffers the next frame reused.
	const auto tick = m_gc_tick;
	// Driver usage includes other caches and allocator reservations. Keep it
	// separate from the owned-byte counter decremented during unregistration.
	const auto used_memory = m_graphics.CanReportMemoryUsage()
	    ? m_graphics.GetDeviceMemoryUsage() : m_total_used_memory;
	if (used_memory < m_trigger_gc_memory) {
		return;
	}

	// Driver pressure alone does not justify a drain: the texture cache cannot evict GPU-modified
	// tiled images, so it pins device usage above critical with memory this cache neither owns nor
	// can free. Gating `aggressive` on device usage alone left the buffer GC downloading dirty
	// buffers forever (each download is a full GPU drain) in response to pressure it could not
	// relieve. Require this cache to hold a material share of the budget before it pays that cost.
	// KYTY_BUFGC_OWN_SHARE=0 restores the old behaviour.
	static const uint64_t own_share_pct = [] {
		const char* env = std::getenv("KYTY_BUFGC_OWN_SHARE");
		if (env == nullptr) {
			return uint64_t {25};
		}
		char*      end    = nullptr;
		const auto parsed = std::strtoull(env, &end, 10);
		return end == env ? uint64_t {25} : parsed;
	}();
	// m_total_used_memory is this cache's own registered bytes now that the driver snapshot is
	// kept separate above.
	const bool own_pressure =
	    own_share_pct == 0 || m_total_used_memory >= m_critical_gc_memory * own_share_pct / 100;
	const bool     aggressive = used_memory >= m_critical_gc_memory && own_pressure;
	const uint64_t age        = std::min<uint64_t>(aggressive ? 80 : 160, tick);
	const size_t   limit      = aggressive ? 64 : 32;

	std::vector<BufferId> dirty_buffers;
	size_t                retire_count = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		EXIT_IF(buffer.is_deleted);
		if (buffer.IsImported()) {
			// Imported buffers have nothing to download. Release pages whose writer completed; a
			// buffer whose pages still await their writer stays resident until then.
			if (ImportFilterEnabled()) {
				auto& master = m_scheduler.GetMasterSemaphore();
				master.Refresh();
				ReleaseImportPieces(Common::AlignDown(buffer.CpuAddress(), TRACKER_PAGE_SIZE),
				                    Common::AlignUp(buffer.CpuAddress() + buffer.Size(), TRACKER_PAGE_SIZE),
				                    master.KnownGpuTick());
			}
			if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
				return false;
			}
			m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
			return ++retire_count == limit;
		}
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !aggressive) {
			return false;
		}
		if (dirty) {
			EXIT_NOT_IMPLEMENTED(!DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
		}
		return ++retire_count == limit;
	});
	if (dirty_buffers.empty()) {
		return;
	}

	// Publish all queued downloads before releasing their tracked pages and owners.
	const auto completion_tick = m_scheduler.CurrentTick();
	m_scheduler.Wait(completion_tick);
	m_scheduler.WaitPriorityOperations(completion_tick);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size()) ||
		    m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: garbage collection retained GPU ownership\n");
		}
		m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

BdaSyncCounters BufferCache::SynchronizeBdaRanges(const RangeSet& ranges, uint64_t mapping_version,
                                                  BdaSyncAction action) {
	BdaSyncCounters counters;
	auto* previous = std::exchange(m_bda_counters, &counters);
	if (action == BdaSyncAction::Reference) {
		ranges.ForEach([&](uint64_t begin, uint64_t end) { SynchronizeBuffersInRange(begin, end - begin); });
		m_bda_counters = previous;
		return counters;
	}
	const auto plan = m_bda_history.Build(m_memory_tracker, ranges, m_register_epoch, mapping_version);
	counters.regions_considered = plan.regions_considered;
	counters.regions_skipped = plan.regions_skipped;
	counters.dirty_bytes = plan.dirty_bytes;
	counters.full_passes = plan.full;
	counters.unchanged_passes = plan.unchanged;
	// Select dirty buffers, then synchronize their original mapped intersections.
	// This keeps each buffer's copies/barriers batched as in the reference path.
	std::map<uint64_t, BufferId> selected;
	plan.dirty_spans.ForEach([&](uint64_t begin, uint64_t end) {
		auto it = m_buffers.upper_bound(begin);
		if (it != m_buffers.begin()) --it;
		for (; it != m_buffers.end() && it->first < end; ++it) {
			const auto& buffer = m_slot_buffers[it->second];
			if (buffer.CpuAddress() + buffer.Size() > begin) selected.emplace(it->first, it->second);
		}
	});
	struct Visit { uint64_t address, size; BufferId id; };
	std::vector<Visit> visits;
	RangeSet expected;
	for (const auto& [address, id]: selected) {
		const auto& buffer = m_slot_buffers[id];
		ranges.ForEachInRange(address, buffer.Size(), [&](uint64_t begin, uint64_t end) {
			visits.push_back({begin, end - begin, id});
			if (action == BdaSyncAction::Compare) {
				const auto page_begin = begin & ~(TRACKER_PAGE_SIZE - 1);
				const auto page_end = (end + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
				expected.Add(page_begin, page_end - page_begin);
			}
		});
	}
	std::sort(visits.begin(), visits.end(), [](const auto& a, const auto& b) { return a.address < b.address; });
	counters.candidate_visits = visits.size();
	if (action == BdaSyncAction::Compare) {
		counters.compared = true;
		m_bda_expected_uploads = &expected;
		ranges.ForEach([&](uint64_t begin, uint64_t end) { SynchronizeBuffersInRange(begin, end - begin); });
		m_bda_expected_uploads = nullptr;
		counters.matched = counters.uncovered_upload_bytes == 0;
		// A later CPU write may legitimately require a reference upload outside
		// the earlier snapshot. Such a check is inconclusive, never validation.
		counters.raced = !counters.matched &&
		    (plan.epochs[0] != RegionManager::CpuDirtyEpoch() || plan.epochs[1] != m_register_epoch);
	} else {
		for (const auto& visit: visits) {
			(void)SynchronizeBuffer(m_slot_buffers[visit.id], visit.address, visit.size, false, false);
		}
	}
	m_bda_history.Commit(plan);
	m_bda_counters = previous;
	return counters;
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start < finish) {
			if (m_bda_counters) ++m_bda_counters->reference_visits;
			(void)SynchronizeBuffer(buffer, start, finish - start, false, false);
		}
	}
}

bool BufferCache::BufferCovers(BufferId id, uint64_t address, uint64_t size) const {
	const auto* buffer = m_slot_buffers.try_get(id);
	return buffer != nullptr && !buffer->is_deleted && buffer->IsInBounds(address, size);
}

bool BufferCache::RetainBuffer(BufferId id, uint64_t address, uint64_t size) {
	if (!BufferCovers(id, address, size)) {
		return false;
	}
	TouchBuffer(m_slot_buffers[id]);
	return true;
}

void BufferCache::NoteShaderWrite(uint64_t address, uint64_t size) {
	if (address == 0 || size == 0) {
		return;
	}
	m_gpu_modified_ranges.Add(address, size);
	NoteWriterTick(address, size);
	m_scheduler.ProfileBufferUse("gpu_writer_bind_pending", address, size, 0);
}

void BufferCache::WatchSkippedReadback(uint64_t vaddr, uint64_t size) {
	// Cover the widened ReadMemory window the skipped readback would have downloaded.
	constexpr uint64_t WindowSize = 512 * 1024;
	const auto         begin      = Common::AlignDown(vaddr, WindowSize);
	const auto         end        = std::max(begin + WindowSize, vaddr + size);
	std::scoped_lock   lock {m_skipped_readback_lock};
	for (const auto& [known_begin, known_end]: m_skipped_readbacks) {
		if (known_begin == begin && known_end == end) {
			return;
		}
	}
	m_skipped_readbacks.emplace_back(begin, end);
	m_watch_skipped_readbacks.store(true, std::memory_order_release);
}

void BufferCache::NoteCpuReadFault(uint64_t vaddr) {
	if (!m_watch_skipped_readbacks.load(std::memory_order_acquire)) {
		return;
	}
	std::scoped_lock lock {m_skipped_readback_lock};
	for (const auto& [begin, end]: m_skipped_readbacks) {
		if (vaddr >= begin && vaddr < end) {
			const auto faults = ++m_skipped_readback_faults;
			if (faults <= 16) {
				std::fprintf(stderr,
				             "[cmask-skip] guest CPU read fault 0x%016" PRIx64
				             " inside skipped readback window 0x%016" PRIx64 "-0x%016" PRIx64 "\n",
				             vaddr, begin, end);
			}
			return;
		}
	}
}

uint64_t BufferCache::TakeSkippedReadbackFaults() {
	return m_skipped_readback_faults.exchange(0);
}

} // namespace Libs::Graphics
