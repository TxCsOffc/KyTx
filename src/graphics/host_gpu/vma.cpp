#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/renderer/productionProfile.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace Libs::Graphics {

namespace {

struct VramAttribution {
	struct Totals {
		uint64_t calls  = 0;
		int64_t  driver = 0;
		int64_t  blocks = 0;
	};

	std::mutex                            lock;
	std::FILE*                            file = nullptr;
	std::map<std::string, Totals>         kinds;
	std::atomic<uint32_t>                 in_flight {0};
	std::atomic<uint64_t>                 generation {0};
	bool                                  started      = false;
	uint64_t                              start_driver = 0;
	uint64_t                              start_blocks = 0;
	std::chrono::steady_clock::time_point start;
	std::chrono::steady_clock::time_point flushed;

	VramAttribution() {
		const char* path = std::getenv("KYTY_VRAM_ATTRIBUTION_CSV");
		if (path != nullptr && *path != '\0') {
			file = std::fopen(path, "w");
			if (file != nullptr) {
				std::fprintf(file, "elapsed_s,kind,calls,driver_delta_mb,vma_delta_mb,gap_delta_mb\n");
				std::fflush(file);
			}
		}
	}

	~VramAttribution() {
		if (file != nullptr) {
			std::fclose(file);
		}
	}

	// Cumulative since the first sample; the last block in the file is the current answer.
	void Flush(std::chrono::steady_clock::time_point now, uint64_t driver, uint64_t blocks) {
		constexpr double Mb      = 1024.0 * 1024.0;
		const double     elapsed = std::chrono::duration<double>(now - start).count();
		int64_t          charged = 0;
		for (const auto& [kind, totals]: kinds) {
			charged += totals.driver - totals.blocks;
			std::fprintf(file, "%.1f,%s,%" PRIu64 ",%.1f,%.1f,%.1f\n", elapsed, kind.c_str(),
			             totals.calls, static_cast<double>(totals.driver) / Mb,
			             static_cast<double>(totals.blocks) / Mb,
			             static_cast<double>(totals.driver - totals.blocks) / Mb);
		}
		const auto driver_delta = static_cast<int64_t>(driver) - static_cast<int64_t>(start_driver);
		const auto blocks_delta = static_cast<int64_t>(blocks) - static_cast<int64_t>(start_blocks);
		const auto gap_delta    = driver_delta - blocks_delta;
		std::fprintf(file, "%.1f,unattributed,0,0,0,%.1f\n", elapsed,
		             static_cast<double>(gap_delta - charged) / Mb);
		std::fprintf(file, "%.1f,total,0,%.1f,%.1f,%.1f\n", elapsed,
		             static_cast<double>(driver_delta) / Mb, static_cast<double>(blocks_delta) / Mb,
		             static_cast<double>(gap_delta) / Mb);
		std::fprintf(file, "%.1f,now,0,%.1f,%.1f,%.1f\n", elapsed, static_cast<double>(driver) / Mb,
		             static_cast<double>(blocks) / Mb,
		             static_cast<double>(static_cast<int64_t>(driver) - static_cast<int64_t>(blocks)) / Mb);
		std::fflush(file);
		flushed = now;
	}
};

VramAttribution& GetVramAttribution() {
	static VramAttribution attribution;
	return attribution;
}

thread_local uint32_t t_vram_attribution_depth = 0;

void SampleDeviceLocal(const GraphicContext& graphics, uint64_t& driver, uint64_t& blocks) {
	vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget {};
	vk::PhysicalDeviceMemoryProperties2         properties {};
	properties.pNext = &budget;
	graphics.physical_device.getMemoryProperties2(&properties);
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(graphics.allocator, budgets);
	const auto& heaps    = properties.memoryProperties;
	const bool  discrete = graphics.physical_device_properties.deviceType ==
	                      vk::PhysicalDeviceType::eDiscreteGpu;
	driver = blocks = 0;
	for (uint32_t heap = 0; heap < heaps.memoryHeapCount; heap++) {
		if (!discrete || (heaps.memoryHeaps[heap].flags & vk::MemoryHeapFlagBits::eDeviceLocal)) {
			driver += budget.heapUsage[heap];
			blocks += budgets[heap].statistics.blockBytes;
		}
	}
}

} // namespace

void GraphicContext::SampleDeviceLocalUsage(uint64_t& driver, uint64_t& blocks) const {
	driver = blocks = 0;
	if (!memory_budget_ext_enabled || allocator == nullptr) {
		return;
	}
	SampleDeviceLocal(*this, driver, blocks);
}

VramAttributionScope::VramAttributionScope(const GraphicContext& graphics, const char* kind) noexcept {
	auto& attribution = GetVramAttribution();
	if (attribution.file == nullptr || !graphics.memory_budget_ext_enabled ||
	    graphics.allocator == nullptr) {
		return;
	}
	m_nested = t_vram_attribution_depth++ != 0;
	if (m_nested) {
		return;
	}
	m_graphics   = &graphics;
	m_kind       = kind;
	m_overlapped = attribution.in_flight.fetch_add(1) != 0;
	m_generation = attribution.generation.fetch_add(1) + 1;
	SampleDeviceLocal(graphics, m_driver, m_blocks);
}

VramAttributionScope::~VramAttributionScope() {
	if (m_graphics == nullptr) {
		if (m_nested) {
			t_vram_attribution_depth--;
		}
		return;
	}
	t_vram_attribution_depth--;
	auto&    attribution = GetVramAttribution();
	uint64_t driver      = 0;
	uint64_t blocks      = 0;
	SampleDeviceLocal(*m_graphics, driver, blocks);
	const bool overlapped = m_overlapped || attribution.generation.load() != m_generation;
	attribution.in_flight.fetch_sub(1);

	std::scoped_lock lock {attribution.lock};
	const auto       now = std::chrono::steady_clock::now();
	if (!attribution.started) {
		attribution.started      = true;
		attribution.start_driver = m_driver;
		attribution.start_blocks = m_blocks;
		attribution.start = attribution.flushed = now;
	}
	auto& totals = attribution.kinds[overlapped ? std::string("overlapped") : std::string(m_kind)];
	totals.calls++;
	totals.driver += static_cast<int64_t>(driver) - static_cast<int64_t>(m_driver);
	totals.blocks += static_cast<int64_t>(blocks) - static_cast<int64_t>(m_blocks);
	if (now - attribution.flushed >= std::chrono::seconds(2)) {
		attribution.Flush(now, driver, blocks);
	}
}

bool GraphicContext::CreateAllocator() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(instance == nullptr || physical_device == nullptr || device == nullptr ||
	        allocator != nullptr);

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = instance;
	info.physicalDevice   = physical_device;
	info.device           = device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (memory_budget_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	}

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", vk::to_string(result).c_str());
		return false;
	}
	return true;
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

void GraphicContext::LogMemoryBudget() const {
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		LOGF("VMA heap %u: usage=%" PRIu64 ", budget=%" PRIu64 ", allocation=%" PRIu64
		     ", blocks=%" PRIu64 "\n",
		     i, static_cast<uint64_t>(budgets[i].usage), static_cast<uint64_t>(budgets[i].budget),
		     static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		     static_cast<uint64_t>(budgets[i].statistics.blockBytes));
	}
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	if (!CanReportMemoryUsage() || allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t usage = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const bool device_local =
		    static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                      vk::MemoryHeapFlagBits::eDeviceLocal);
		if (!discrete || device_local) {
			usage += budgets[heap].usage;
		}
	}
	return usage;
}

void GraphicContext::ReportMemoryBreakdown(uint64_t& block_bytes, uint64_t& allocation_bytes,
	                                           uint64_t& driver_usage, uint64_t& driver_budget,
	                                           uint64_t& host_block_bytes,
	                                           uint64_t& host_allocation_bytes,
	                                           uint64_t& host_driver_usage,
	                                           uint64_t& host_driver_budget) const {
	block_bytes = allocation_bytes = driver_usage = driver_budget = 0;
	host_block_bytes = host_allocation_bytes = host_driver_usage = host_driver_budget = 0;
	if (allocator == nullptr) {
		return;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const bool device_local =
		    static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                      vk::MemoryHeapFlagBits::eDeviceLocal);
		if (!discrete || device_local) {
			block_bytes += budgets[heap].statistics.blockBytes;
			allocation_bytes += budgets[heap].statistics.allocationBytes;
			driver_usage += budgets[heap].usage;
			driver_budget += budgets[heap].budget;
		} else {
			host_block_bytes += budgets[heap].statistics.blockBytes;
			host_allocation_bytes += budgets[heap].statistics.allocationBytes;
			host_driver_usage += budgets[heap].usage;
			host_driver_budget += budgets[heap].budget;
		}
	}
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t budget = 0;
	uint64_t local  = 0;
	uint64_t usage  = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		const bool  device_local =
		    static_cast<bool>(properties.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			local += properties.size;
		}
		if (!discrete || device_local) {
			budget += CanReportMemoryUsage() ? budgets[heap].budget : properties.size;
			usage += CanReportMemoryUsage() ? budgets[heap].usage : 0;
		}
	}
	if (discrete) {
		return budget - std::min<uint64_t>(budget / 8, 1024ull * 1024 * 1024);
	}
	constexpr uint64_t system_reserve = 8ull * 1024 * 1024 * 1024;
	const auto         available      = budget > usage ? budget - usage : uint64_t {0};
	return std::max(local, available > system_reserve ? available - system_reserve : uint64_t {0});
}

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image) {
	ProfileDetailScope profile("cpu_image_allocate", 0,
	    uint64_t(image_info.extent.width) * image_info.extent.height * image_info.extent.depth);
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.allocation != nullptr);

	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	VramAttributionScope vram(*this, "image_create");
#if defined(__APPLE__)
	// Metal aborts inside vkCreateImage on descriptors it rejects, so record each request if enabled.
	static const bool dump_images = std::getenv("KYTY_DUMP_IMAGES") != nullptr;
	if (dump_images) {
		std::fprintf(stderr, "[img] type=%d fmt=%d %ux%ux%u mips=%u layers=%u samples=%d usage=0x%x flags=0x%x tiling=%d\n",
		             static_cast<int>(image_info.imageType), static_cast<int>(image_info.format),
		             image_info.extent.width, image_info.extent.height, image_info.extent.depth,
		             image_info.mipLevels, image_info.arrayLayers, static_cast<int>(image_info.samples),
		             static_cast<uint32_t>(image_info.usage), static_cast<uint32_t>(image_info.flags),
		             static_cast<int>(image_info.tiling));
		std::fflush(stderr);
	}
#endif
	vk::Image::CType native_image = VK_NULL_HANDLE;
	const auto        result       = static_cast<vk::Result>(
	    vmaCreateImage(allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
	                   &alloc_info, &native_image, &image.allocation, nullptr));
	image.image = native_image;
	if (result != vk::Result::eSuccess) {
		LogMemoryBudget();
		return false;
	}

	image.format     = image_info.format;
	image.image_type = image_info.imageType;
	image.extent     = image_info.extent;
	image.layers     = image_info.arrayLayers;
	image.mip_levels = image_info.mipLevels;
	image.samples    = static_cast<uint32_t>(image_info.samples);
	image.usage      = image_info.usage;
	image.flags      = image_info.flags;
	image.state      = {.layout = image_info.initialLayout};
	image.subresource_states.clear();

	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.allocation == nullptr);

	VramAttributionScope vram(*this, "image_destroy");
	vmaDestroyImage(allocator, image.image, image.allocation);
	image.image      = nullptr;
	image.allocation = nullptr;
}

} // namespace Libs::Graphics
