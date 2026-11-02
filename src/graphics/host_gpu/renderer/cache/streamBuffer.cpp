#include "graphics/host_gpu/renderer/productionProfile.h"
#include <bit>
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <cstdlib>
#include <cstring>
#include <numeric>
#include <vk_mem_alloc.h>
#if defined(__APPLE__)
#include "graphics/host_gpu/metal/metalRenderer.h"
#include "graphics/host_gpu/metal/metalBuffer.h"
#endif

namespace Libs::Graphics {

namespace {

constexpr size_t WATCHES_INITIAL_RESERVE = 0x4000;
constexpr size_t WATCHES_RESERVE_CHUNK   = 0x1000;

[[nodiscard]] VmaAllocationCreateFlags AllocationFlags(MemoryUsage usage) {
	switch (usage) {
		case MemoryUsage::Upload:
		case MemoryUsage::Stream:
			return VMA_ALLOCATION_CREATE_MAPPED_BIT |
			       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
		case MemoryUsage::Download:
			return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
		case MemoryUsage::DeviceLocal: {
#if defined(__APPLE__)
			// Unified memory: keep cache buffers host-visible so GPU results can be read
			// directly (see BufferCache::DownloadBufferMemory). Opt-in: KYTY_UMA_READBACK=1 (default on on Apple).
			static const bool uma = [] {
				const char* value = std::getenv("KYTY_UMA_READBACK");
				if (value != nullptr) {
					return value[0] != '0';
				}
				return true;
			}();
			if (uma) {
				return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
			}
#endif
			return {};
		}
	}
	return {};
}

[[nodiscard]] VmaMemoryUsage AllocationUsage(MemoryUsage usage) {
	switch (usage) {
		case MemoryUsage::DeviceLocal:
		case MemoryUsage::Stream: return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		case MemoryUsage::Upload:
		case MemoryUsage::Download: return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
	}
	return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
}

[[nodiscard]] bool AlignUp(uint64_t value, uint64_t alignment, uint64_t& result) {
	if (alignment == 0) {
		result = value;
		return true;
	}
	const auto aligned = Common::AlignUp(value, alignment);
	if (aligned < value) {
		return false;
	}
	result = aligned;
	return true;
}

} // namespace

Buffer::Buffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage,
               uint64_t cpu_address, vk::BufferUsageFlags flags, uint64_t size)
    : m_graphics(&graphics), m_scheduler(&scheduler), m_usage(usage), m_cpu_address(cpu_address),
      m_size(size) {
	ProfileDetailScope allocation_profile("cpu_buffer_allocate", cpu_address, size);
	// Selected-window identity for native allocation/residency joins. VMA can reuse
	// a VkDeviceMemory block, so record both the constructor bounds and binding.
	ProfileCpuScope native_allocation_profile(scheduler, "buffer_native_allocate", cpu_address,
	                                          size, true);
	KYTY_PROFILER_FUNCTION();

#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		auto mb = std::make_unique<Metal::MetalBuffer>();
		if (mb->Allocate(size, "KytyBuffer")) {
			m_mapped = {static_cast<uint8_t*>(mb->GetContents()), static_cast<size_t>(size)};
			m_device_address = mb->GetGpuAddress();
			m_coherent = true;
			m_metal_buffer = mb.release();
		} else {
			void* ptr = nullptr;
			posix_memalign(&ptr, 16384, size);
			if (ptr != nullptr) {
				m_mapped = {static_cast<uint8_t*>(ptr), static_cast<size_t>(size)};
				m_device_address = reinterpret_cast<uint64_t>(ptr);
				m_coherent = true;
			}
		}
		return;
	}
#endif

	EXIT_IF(graphics.allocator == nullptr || size == 0);

	vk::BufferCreateInfo buffer_info {};
	buffer_info.size        = size;
	buffer_info.usage       = flags;

	const bool with_bda = bool(flags & vk::BufferUsageFlagBits::eShaderDeviceAddress);
	// Pooled blocks also carry DEVICE_ADDRESS, and the BDA page table adds the in-page offset
	// to the buffer address, so suballocated BDA buffers need no extra alignment.
	static const bool bda_suballocate = [] {
		const char* value = std::getenv("KYTY_BDA_SUBALLOC");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	const VmaAllocationCreateFlags bda_flag =
	    with_bda && !bda_suballocate ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
	VmaAllocationCreateInfo allocation_info {};
	allocation_info.flags =
	    VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag | AllocationFlags(usage);
	allocation_info.usage = AllocationUsage(usage);
#if defined(__APPLE__)
	allocation_info.preferredFlags =
	    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
#else
	allocation_info.preferredFlags = usage == MemoryUsage::DeviceLocal
	                                     ? VkMemoryPropertyFlags {}
	                                     : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
#endif

	VmaAllocationInfo allocation_result {};
	VkBuffer          native_buffer = VK_NULL_HANDLE;
	vk::Result        result        = vk::Result::eSuccess;
	{
		VramAttributionScope vram(graphics, with_bda ? "buffer_create_bda" : "buffer_create");
		result = static_cast<vk::Result>(vmaCreateBuffer(
		    graphics.allocator, static_cast<const VkBufferCreateInfo*>(buffer_info),
		    &allocation_info, &native_buffer, &m_allocation, &allocation_result));
	}
	if (result != vk::Result::eSuccess) {
		graphics.LogMemoryBudget();
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	m_buffer = native_buffer;
	if (with_bda) {
		vk::BufferDeviceAddressInfo address_info {};
		address_info.buffer = m_buffer;
		m_device_address    = graphics.device.getBufferAddress(address_info);
		EXIT_IF(m_device_address == 0);
	}

	VkMemoryPropertyFlags properties = 0;
	vmaGetAllocationMemoryProperties(graphics.allocator, m_allocation, &properties);
	m_memory_type = allocation_result.memoryType;
	m_memory_properties = properties;
	m_coherent = (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	if (allocation_result.pMappedData != nullptr) {
		m_mapped = {static_cast<uint8_t*>(allocation_result.pMappedData),
		            static_cast<size_t>(size)};
	}
	native_allocation_profile.SetIdentity(std::bit_cast<uint64_t>(Handle()), Size());
	ProfileAllocation("buffer_allocation");
}

Buffer::Buffer(GraphicContext& graphics, CommandScheduler& scheduler, uint64_t cpu_address,
               vk::BufferUsageFlags flags, uint64_t size, vk::DeviceMemory memory,
               uint64_t memory_offset, uint32_t memory_type, uint32_t memory_properties)
    : m_graphics(&graphics), m_scheduler(&scheduler), m_usage(MemoryUsage::DeviceLocal),
      m_cpu_address(cpu_address), m_size(size), m_imported(true) {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		m_device_address    = cpu_address;
		m_coherent          = true;
		m_import_offset     = memory_offset;
		return;
	}
#endif
	vk::BufferCreateInfo buffer_info {};
	buffer_info.size  = size;
	buffer_info.usage = flags;
	const auto create = graphics.device.createBuffer(&buffer_info, nullptr, &m_buffer);
	EXIT_NOT_IMPLEMENTED(create != vk::Result::eSuccess || m_buffer == nullptr);
	const auto bind = graphics.device.bindBufferMemory(m_buffer, memory, memory_offset);
	EXIT_NOT_IMPLEMENTED(bind != vk::Result::eSuccess);
	if (static_cast<bool>(flags & vk::BufferUsageFlagBits::eShaderDeviceAddress)) {
		vk::BufferDeviceAddressInfo address_info {};
		address_info.buffer = m_buffer;
		m_device_address    = graphics.device.getBufferAddress(address_info);
		EXIT_IF(m_device_address == 0);
	}
	m_memory_type       = memory_type;
	m_memory_properties = memory_properties;
	m_coherent          = (memory_properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	m_import_offset     = memory_offset;
}

Buffer::~Buffer() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		if (m_metal_buffer != nullptr) {
			delete static_cast<Metal::MetalBuffer*>(m_metal_buffer);
			m_metal_buffer = nullptr;
		} else if (!m_mapped.empty() && m_buffer == nullptr && m_allocation == nullptr) {
			free(m_mapped.data());
			m_mapped = {};
		}
		return;
	}
#endif
	if (m_buffer != nullptr && m_allocation == nullptr) {
		// Imported buffer: only the VkBuffer is ours; the memory is the shared guest import.
		auto* graphics = m_graphics;
		const auto buffer = m_buffer;
		if (m_scheduler != nullptr && m_scheduler->Active()) {
			m_scheduler->DeferOperationAfterTicks(
			    [graphics, buffer] { graphics->device.destroyBuffer(buffer, nullptr); }, 8);
		} else {
			graphics->device.destroyBuffer(buffer, nullptr);
		}
		return;
	}
	if (m_buffer != nullptr) {
#if defined(__APPLE__)
		// Metal validation caught buffers freed while a command buffer still used them; MoltenVK
		// does not retain them, so keep the allocation alive a few more GPU ticks.
		if (m_scheduler != nullptr && m_scheduler->Active()) {
			auto* graphics = m_graphics;
			m_scheduler->DeferOperationAfterTicks(
			    [graphics, buffer = m_buffer, allocation = m_allocation] {
				    VramAttributionScope vram(*graphics, "buffer_destroy");
				    vmaDestroyBuffer(graphics->allocator, buffer, allocation);
			    },
			    8);
			return;
		}
#endif
		VramAttributionScope vram(*m_graphics, "buffer_destroy");
		vmaDestroyBuffer(m_graphics->allocator, m_buffer, m_allocation);
	}
}

vk::DeviceAddress Buffer::BufferDeviceAddress() const noexcept {
	return m_device_address;
}

bool Buffer::IsInBounds(uint64_t address, uint64_t size) const noexcept {
	return address >= m_cpu_address && size <= Size() && address - m_cpu_address <= Size() - size;
}

void Buffer::Flush(uint64_t offset, uint64_t size) {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	ProfileCpuScope profile(Scheduler(), IsCoherent() || size == 0 ? "host_flush_noop" : "host_flush", std::bit_cast<uint64_t>(Handle()), size);
	EXIT_IF(m_mapped.empty() || offset > Size() || size > Size() - offset);
	if (!IsCoherent() && size != 0) {
		const auto result =
		    vmaFlushAllocation(m_graphics->allocator, m_allocation, offset, size);
		EXIT_NOT_IMPLEMENTED(static_cast<vk::Result>(result) != vk::Result::eSuccess);
	}
}

void Buffer::Invalidate(uint64_t offset, uint64_t size) {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	ProfileCpuScope profile(Scheduler(), IsCoherent() || size == 0 ? "host_invalidate_noop" : "host_invalidate", std::bit_cast<uint64_t>(Handle()), size);
	EXIT_IF(m_usage != MemoryUsage::Download || offset > Size() || size > Size() - offset);
	if (!IsCoherent() && size != 0) {
		const auto result =
		    vmaInvalidateAllocation(m_graphics->allocator, m_allocation, offset, size);
		EXIT_NOT_IMPLEMENTED(static_cast<vk::Result>(result) != vk::Result::eSuccess);
	}
}

void Buffer::ProfileAllocation(const char* kind) {
	if (!Scheduler().ProfileActive()) return;
	const auto now = ProfileClockNs();
	Scheduler().ProfileEvent(kind, now, now, std::bit_cast<uint64_t>(Handle()), Size(), std::source_location::current());
	// Companion metadata row: resource carries VkMemoryPropertyFlags, bytes carries
	// memoryTypeIndex. Transaction identity associates it with the preceding buffer.
	Scheduler().ProfileEvent("allocation_memory_properties", now, now, m_memory_properties,
	    m_memory_type, std::source_location::current());
	if (m_allocation == nullptr) {
		return;  // imported buffer: no VMA allocation to describe
	}
	VmaAllocationInfo info {};
	vmaGetAllocationInfo(Graphics().allocator, m_allocation, &info);
	// These are Vulkan bindings, not evidence of current physical residency.
	// Native handles must be joined within their captured creation lifetimes.
	Scheduler().ProfileEvent("allocation_memory_binding", now, now,
	    std::bit_cast<uint64_t>(info.deviceMemory), info.offset, std::source_location::current());
	Scheduler().ProfileEvent("allocation_memory_extent", now, now,
	    std::bit_cast<uint64_t>(Handle()), info.size, std::source_location::current());
	Scheduler().ProfileEvent("allocation_host_mapping", now, now,
	    reinterpret_cast<uint64_t>(info.pMappedData), static_cast<uint64_t>(Usage()),
	    std::source_location::current());
}

vk::BufferMemoryBarrier Buffer::Barrier(uint64_t offset, uint64_t size, vk::AccessFlags source,
                                        vk::AccessFlags destination) const {
	if (Handle() == nullptr || size == 0 || offset > Size() || size > Size() - offset) {
		EXIT("Buffer: invalid DMA barrier, handle=%p offset=0x%016" PRIx64 " size=0x%016" PRIx64
		     " capacity=0x%016" PRIx64 "\n",
		     static_cast<const void*>(Handle()), offset, size, Size());
	}
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask       = source;
	barrier.dstAccessMask       = destination;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = Handle();
	barrier.offset              = offset;
	barrier.size                = size;
	return barrier;
}

void Buffer::CopyFrom(CommandBuffer& command, const Buffer& source, uint64_t source_offset,
                      uint64_t destination_offset, uint64_t size, vk::AccessFlags source_before,
                      vk::AccessFlags destination_before, vk::AccessFlags source_after,
                      vk::AccessFlags destination_after) {
	if (size == 0 || source_offset > source.Size() || size > source.Size() - source_offset ||
	    destination_offset > Size() || size > Size() - destination_offset) {
		EXIT("Buffer: invalid copy range\n");
	}
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		if (!m_mapped.empty() && !source.Mapped().empty()) {
			std::memcpy(m_mapped.data() + destination_offset,
			            source.Mapped().data() + source_offset, size);
		}
		return;
	}
#endif
	if (source.Handle() == Handle() && source_offset < destination_offset + size &&
	    destination_offset < source_offset + size) {
		EXIT("Buffer: overlapping self-copy\n");
	}
	command.EndRendering();
	const vk::BufferMemoryBarrier before[] = {
	    source.Barrier(source_offset, size, source_before, vk::AccessFlagBits::eTransferRead),
	    Barrier(destination_offset, size, destination_before, vk::AccessFlagBits::eTransferWrite),
	};
	const auto host_access  = vk::AccessFlagBits::eHostRead | vk::AccessFlagBits::eHostWrite;
	auto       before_stage = vk::PipelineStageFlags {vk::PipelineStageFlagBits::eAllCommands};
	if (static_cast<bool>((source_before | destination_before) & host_access)) {
		before_stage |= vk::PipelineStageFlagBits::eHost;
	}
	const auto native = command.Handle();
	ProfilePipelineBarrier(native, before_stage, vk::PipelineStageFlagBits::eTransfer,
	                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 2, before, 0, nullptr);
	const vk::BufferCopy copy {source_offset, destination_offset, size};
	Scheduler().ProfileBufferUse("gpu_reader_copy", source.CpuAddress() + source_offset, size,
	    std::bit_cast<uint64_t>(source.Handle()));
	Scheduler().ProfileBufferUse("gpu_writer_copy_exact", CpuAddress() + destination_offset, size,
	    std::bit_cast<uint64_t>(Handle()));
	const auto copy_timer = Scheduler().StartGpuTimer("copy_buffer", Scheduler().GpuFrameHint(), size);
	native.copyBuffer(source.Handle(), Handle(), 1, &copy);
	Scheduler().EndGpuTimer(copy_timer);
	const vk::BufferMemoryBarrier after[] = {
	    source.Barrier(source_offset, size, vk::AccessFlagBits::eTransferRead, source_after),
	    Barrier(destination_offset, size, vk::AccessFlagBits::eTransferWrite, destination_after),
	};
	auto after_stage = vk::PipelineStageFlags {vk::PipelineStageFlagBits::eAllCommands};
	if (static_cast<bool>((source_after | destination_after) & host_access)) {
		after_stage |= vk::PipelineStageFlagBits::eHost;
	}
	ProfilePipelineBarrier(native, vk::PipelineStageFlagBits::eTransfer, after_stage,
	                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 2, after, 0, nullptr);
}

void Buffer::Fill(uint64_t offset, uint64_t size, uint32_t value) {
	if (((offset | size) & 3u) != 0) {
		EXIT("Buffer: fill range must be dword aligned\n");
	}
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		if (!m_mapped.empty() && offset + size <= Size()) {
			std::fill_n(reinterpret_cast<uint32_t*>(m_mapped.data() + offset), size / 4, value);
		}
		return;
	}
#endif
	auto& command = Scheduler().Current();
	command.EndRendering();
	const auto before =
	    Barrier(offset, size, vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
	            vk::AccessFlagBits::eTransferWrite);
	const auto native = command.Handle();
	ProfilePipelineBarrier(native, vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
	                       0, nullptr, 1, &before, 0, nullptr);
	Scheduler().ProfileBufferUse("gpu_writer_fill_exact", CpuAddress() + offset, size,
	    std::bit_cast<uint64_t>(Handle()), value);
	native.fillBuffer(Handle(), offset, size, value);
	const auto after = Barrier(offset, size, vk::AccessFlagBits::eTransferWrite,
	                           vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite);
	ProfilePipelineBarrier(native, vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands,
	                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
}

StreamBuffer::StreamBuffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage,
                           uint64_t size)
    : Buffer(graphics, scheduler, usage, 0, AllFlags, size),
      m_current_watches(WATCHES_INITIAL_RESERVE), m_previous_watches(WATCHES_INITIAL_RESERVE) {}

bool StreamBuffer::NormalizeReservation(bool coherent, uint64_t atom, uint64_t& size,
                                        uint64_t& alignment) {
	if (coherent) {
		return true;
	}
	if (!AlignUp(size, atom, size)) {
		return false;
	}
	const auto divisor = std::gcd(alignment, atom);
	if (alignment != 0 && alignment / divisor > UINT64_MAX / atom) {
		return false;
	}
	alignment = alignment == 0 ? atom : alignment / divisor * atom;
	return true;
}

std::pair<uint8_t*, uint64_t> StreamBuffer::Map(uint64_t size, uint64_t alignment,
                                                bool allow_wait) {
	if (Mapped().empty()) {
		return {nullptr, 0};
	}
	uint64_t   mapped_size = size;
	const auto atom        = Graphics().physical_device_properties.limits.nonCoherentAtomSize;
	if (!NormalizeReservation(IsCoherent(), atom, mapped_size, alignment)) {
		return {nullptr, 0};
	}
	if (mapped_size > Size()) {
		return {nullptr, 0};
	}

	uint64_t aligned_offset = 0;
	if (!AlignUp(m_offset, alignment, aligned_offset)) {
		return {nullptr, 0};
	}

	const bool wrap = aligned_offset > Size() - mapped_size;
	if (wrap) {
		aligned_offset = 0;
	}

	auto wait_cursor = wrap ? size_t {0} : m_wait_cursor;
	auto wait_bound  = wrap ? uint64_t {0} : m_wait_bound;
	auto invalidation_mark =
	    wrap ? std::optional<size_t> {m_current_watch_cursor} : m_invalidation_mark;
	auto& pending_watches = wrap ? m_current_watches : m_previous_watches;
	if (!WaitPendingOperations(pending_watches, invalidation_mark, aligned_offset + mapped_size,
	                           allow_wait, wait_cursor, wait_bound)) {
		return {nullptr, 0};
	}

	if (wrap) {
		++m_wrap_epoch;
		m_invalidation_mark    = invalidation_mark;
		m_current_watch_cursor = 0;
		std::swap(m_previous_watches, m_current_watches);
	}
	m_wait_cursor = wait_cursor;
	m_wait_bound  = wait_bound;
	m_offset      = aligned_offset;
	m_mapped_size = mapped_size;
	return {Mapped().data() + m_offset, m_offset};
}

void StreamBuffer::Commit() {
	if (Usage() != MemoryUsage::Download && m_mapped_size != 0) {
		Flush(m_offset, m_mapped_size);
	}

	m_offset += m_mapped_size;
	const auto tick = Scheduler().CurrentTick();
	if (m_current_watch_cursor != 0 && m_current_watches[m_current_watch_cursor - 1].tick == tick) {
		m_current_watches[m_current_watch_cursor - 1].upper_bound = m_offset;
		return;
	}
	if (m_current_watch_cursor + 1 >= m_current_watches.size()) {
		m_current_watches.resize(m_current_watches.size() + WATCHES_RESERVE_CHUNK);
	}
	auto& watch       = m_current_watches[m_current_watch_cursor++];
	watch.upper_bound = m_offset;
	watch.tick        = tick;
}

uint64_t StreamBuffer::Copy(const void* source, uint64_t size, uint64_t alignment) {
	EXIT_IF(source == nullptr);
	const auto [data, offset] = Map(size, alignment);
	EXIT_IF(data == nullptr);
	std::memcpy(data, source, static_cast<size_t>(size));
	Commit();
	return offset;
}

bool StreamBuffer::WaitPendingOperations(const std::vector<Watch>& watches,
                                         std::optional<size_t>     invalidation_mark,
                                         uint64_t requested_upper_bound, bool allow_wait,
                                         size_t& wait_cursor, uint64_t& wait_bound) {
	if (!invalidation_mark.has_value()) {
		return true;
	}
	while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
		const auto& watch = watches[wait_cursor];
		if (!Scheduler().IsFree(watch.tick) && !allow_wait) {
			return false;
		}
		Scheduler().Wait(watch.tick);
		if (Usage() == MemoryUsage::Download) {
			Scheduler().WaitPriorityOperations(watch.tick);
		}
		wait_bound = watch.upper_bound;
		++wait_cursor;
	}
	return true;
}

} // namespace Libs::Graphics
