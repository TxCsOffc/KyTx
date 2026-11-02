#include "graphics/host_gpu/metal/metalEnv.h"
#include <chrono>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <cstdlib>
#include "graphics/host_gpu/metal/metalBuffer.h"
#include "graphics/host_gpu/metal/metalContext.h"
#include "common/logging/log.h"

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <unistd.h>
#include <cstring>

namespace Libs::Graphics::Metal {

MetalBuffer::MetalBuffer() = default;

MetalBuffer::~MetalBuffer() {
	Release();
}

bool MetalBuffer::Allocate(size_t length, const char* label) {
	Release();
	if (length == 0) {
		return false;
	}

	@autoreleasepool {
		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		if (!device) {
			return false;
		}

		m_handle = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
		if (!m_handle) {
			LOGF_COLOR(Log::Color::BrightRed, "Metal: Failed to allocate shared UMA buffer of size %zu bytes\n", length);
			return false;
		}

		if (label) {
			m_handle.label = [NSString stringWithUTF8String:label];
		}

		m_contents = [m_handle contents];
		m_length = length;
		m_offset = 0;
		m_is_wrapped = false;

		if (@available(macOS 13.0, *)) {
			if ([device respondsToSelector:@selector(supportsFamily:)] && [device supportsFamily:MTLGPUFamilyMetal3]) {
				m_gpu_address = [m_handle gpuAddress];
			}
		}

		return true;
	}
}

bool MetalBuffer::WrapHostMemory(void* host_ptr, size_t length, const char* label) {
	Release();
	if (!host_ptr || length == 0) {
		return false;
	}

	@autoreleasepool {
		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		if (!device) {
			return false;
		}

		const size_t page_size = static_cast<size_t>(getpagesize());
		const uintptr_t raw_addr = reinterpret_cast<uintptr_t>(host_ptr);
		const uintptr_t page_aligned_addr = raw_addr & ~(page_size - 1);
		const size_t page_offset = raw_addr - page_aligned_addr;
		const size_t total_span = page_offset + length;
		const size_t aligned_length = (total_span + page_size - 1) & ~(page_size - 1);

		m_handle = [device newBufferWithBytesNoCopy:reinterpret_cast<void*>(page_aligned_addr)
		                                     length:aligned_length
		                                    options:MTLResourceStorageModeShared
		                                deallocator:nil];

		if (!m_handle) {
			LOGF_COLOR(Log::Color::BrightRed,
			           "Metal: Failed to wrap host memory at %p (length: %zu, aligned: %zu)\n",
			           host_ptr, length, aligned_length);
			return false;
		}

		// KYTY_MTL_FAULT_INFO=1: report the host protection of the wrapped range (write tracking uses mprotect).
		if (KYTY_ENV("KYTY_MTL_FAULT_INFO") != nullptr) {
			static std::atomic<uint32_t> s_prot_log {0};
			if (s_prot_log.fetch_add(1) < 40) {
				vm_address_t addr = static_cast<vm_address_t>(page_aligned_addr);
				vm_size_t vsize = 0;
				vm_region_basic_info_data_64_t info {};
				mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
				mach_port_t object = 0;
				kern_return_t kr = vm_region_64(mach_task_self(), &addr, &vsize, VM_REGION_BASIC_INFO_64,
				                                reinterpret_cast<vm_region_info_t>(&info), &count, &object);
				std::fprintf(stderr, "[WrapProt] %s page=0x%llx len=%zu kr=%d prot=%d max=%d region=0x%llx+%llu\n",
				             label, (unsigned long long)page_aligned_addr, aligned_length, kr, info.protection,
				             info.max_protection, (unsigned long long)addr, (unsigned long long)vsize);
			}
		}

		if (label) {
			m_handle.label = [NSString stringWithUTF8String:label];
		}

		m_contents = host_ptr;
		m_length = length;
		m_offset = page_offset;
		m_is_wrapped = true;

		if (@available(macOS 13.0, *)) {
			if ([device respondsToSelector:@selector(supportsFamily:)] && [device supportsFamily:MTLGPUFamilyMetal3]) {
				m_gpu_address = [m_handle gpuAddress] + page_offset;
			}
		}

		return true;
	}
}

void MetalBuffer::Release() {
	@autoreleasepool {
		m_handle = nil;
		m_contents = nullptr;
		m_length = 0;
		m_offset = 0;
		m_gpu_address = 0;
		m_is_wrapped = false;
	}
}

void MetalBuffer::Write(const void* data, size_t size, size_t dest_offset) {
	if (!m_contents || !data || size == 0 || (dest_offset + size > m_length)) {
		return;
	}
	uint8_t* dst = static_cast<uint8_t*>(m_contents) + dest_offset;
	std::memcpy(dst, data, size);
}

id<MTLTexture> MetalBuffer::CreateTextureView(MTLTextureDescriptor* desc,
                                              NSUInteger offset,
                                              NSUInteger bytes_per_row) {
	if (!m_handle || !desc) {
		return nil;
	}
	@autoreleasepool {
		return [m_handle newTextureWithDescriptor:desc
		                                   offset:(m_offset + offset)
		                              bytesPerRow:bytes_per_row];
	}
}

MetalDynamicRingBuffer::MetalDynamicRingBuffer(size_t capacity)
    : m_capacity(capacity), m_head(0) {}

MetalDynamicRingBuffer::~MetalDynamicRingBuffer() {
	Shutdown();
}

bool MetalDynamicRingBuffer::Initialize() {
	std::lock_guard lock(m_mutex);
	m_head = 0;
	return m_buffer.Allocate(m_capacity, "DynamicRingBuffer");
}

void MetalDynamicRingBuffer::Shutdown() {
	std::lock_guard lock(m_mutex);
	m_buffer.Release();
	m_head = 0;
}

MetalBufferAllocation MetalDynamicRingBuffer::Allocate(size_t size, size_t alignment) {
	std::lock_guard lock(m_mutex);
	if (!m_buffer.IsValid() || size == 0) {
		return {};
	}

	size_t aligned_head = (m_head + alignment - 1) & ~(alignment - 1);
	if (size > m_capacity) {
		LOGF_COLOR(Log::Color::BrightRed, "Metal: Dynamic ring buffer overflow! Requested %zu bytes\n", size);
		return {};
	}
	if (aligned_head > m_capacity - size) {
		// Encoded commands can still read the old allocation. Never overwrite its
		// beginning: Metal's retained command buffers keep that backing alive until
		// completion, including indirect references declared with useResource.
		if (!m_buffer.Allocate(m_capacity, "DynamicRingBuffer")) return {};
		aligned_head = 0;
	}

	m_head = aligned_head + size;

	MetalBufferAllocation alloc {};
	alloc.buffer = m_buffer.GetHandle();
	alloc.offset = m_buffer.GetOffset() + aligned_head;
	alloc.size = size;
	alloc.cpu_addr = static_cast<uint8_t*>(m_buffer.GetContents()) + aligned_head;
	alloc.gpu_addr = m_buffer.GetGpuAddress() ? (m_buffer.GetGpuAddress() + aligned_head) : 0;
	return alloc;
}

void MetalDynamicRingBuffer::Reset() {
	std::lock_guard lock(m_mutex);
	// Reset does not imply that all previously encoded GPU work has completed.
	m_buffer.Allocate(m_capacity, "DynamicRingBuffer");
	m_head = 0;
}

MetalBufferManager& MetalBufferManager::Instance() {
	static MetalBufferManager manager;
	return manager;
}

MetalBufferManager::MetalBufferManager() {
	m_page_size = static_cast<size_t>(getpagesize());
	m_dynamic_ring = std::make_unique<MetalDynamicRingBuffer>(32 * 1024 * 1024); // 32 MB ring buffer
	m_dynamic_ring->Initialize();
}

MetalBufferManager::~MetalBufferManager() {
	ClearCache();
}

void MetalBufferManager::CacheStats(size_t* count, uint64_t* bytes) {
	std::lock_guard lock(m_cache_mutex);
	*count = m_page_cache.size();
	uint64_t total = 0;
	for (const auto& [addr, e]: m_page_cache) total += e.buffer->GetLength();
	*bytes = total;
}

MetalBufferAllocation MetalBufferManager::GetBufferForGuestRange(uint64_t vaddr, size_t size) {
	if (vaddr == 0 || size == 0) {
		return {};
	}

	const uint64_t page_aligned = vaddr & ~(static_cast<uint64_t>(m_page_size) - 1);
	const uint64_t offset_in_page = vaddr - page_aligned;
	const uint64_t total_bytes = offset_in_page + size;
	const uint64_t aligned_size = (total_bytes + m_page_size - 1) & ~(static_cast<uint64_t>(m_page_size) - 1);

	std::lock_guard lock(m_cache_mutex);
	const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	// Wrappers nobody looked up for KYTY_PAGE_CACHE_EVICT_S seconds (opt-in, e.g. 30; unset or 0 keeps all) are released, swept every 2 s: the cache
	// kept one wrapper per guest page range the game ever bound (26k wrappers, 8+ GB of wired views after a menu tour and a match).
	// A wrapper is rebuilt on the next lookup; textures made from one keep their own reference to it.
	static const int64_t s_evict_ms = [] {
		const char* v = std::getenv("KYTY_PAGE_CACHE_EVICT_S");
		return (v != nullptr ? std::strtoll(v, nullptr, 10) : 0ll) * 1000ll;
	}();
	if (s_evict_ms > 0 && now_ms - m_page_cache_swept_ms >= 2000) {
		m_page_cache_swept_ms = now_ms;
		for (auto e = m_page_cache.begin(); e != m_page_cache.end();) {
			if (now_ms - e->second.used_ms > s_evict_ms) e = m_page_cache.erase(e);
			else ++e;
		}
	}
	auto it = m_page_cache.find(page_aligned);
	if (it != m_page_cache.end()) {
		it->second.used_ms = now_ms;
		const auto& cached = it->second.buffer;
		if (cached->GetLength() >= total_bytes) {
			MetalBufferAllocation alloc {};
			alloc.buffer = cached->GetHandle();
			alloc.offset = cached->GetOffset() + offset_in_page;
			alloc.size = size;
			alloc.cpu_addr = reinterpret_cast<void*>(vaddr);
			alloc.gpu_addr = cached->GetGpuAddress() ? (cached->GetGpuAddress() + offset_in_page) : 0;
			return alloc;
		}
	}

	auto buf = std::make_shared<MetalBuffer>();
	if (!buf->WrapHostMemory(reinterpret_cast<void*>(page_aligned), aligned_size, "GuestMemoryUMA")) {
		return {};
	}

	m_page_cache[page_aligned] = {buf, now_ms};

	MetalBufferAllocation alloc {};
	alloc.buffer = buf->GetHandle();
	alloc.offset = buf->GetOffset() + offset_in_page;
	alloc.size = size;
	alloc.cpu_addr = reinterpret_cast<void*>(vaddr);
	alloc.gpu_addr = buf->GetGpuAddress() ? (buf->GetGpuAddress() + offset_in_page) : 0;
	return alloc;
}

MetalBufferAllocation MetalBufferManager::AllocateDynamic(size_t size, size_t alignment) {
	if (!m_dynamic_ring) {
		return {};
	}
	return m_dynamic_ring->Allocate(size, alignment);
}

void MetalBufferManager::ResetTransientBuffers() {
	if (m_dynamic_ring) {
		m_dynamic_ring->Reset();
	}
}

MetalBufferAllocation MetalBufferManager::SnapshotReadOnly(const MetalBufferAllocation& src, size_t size) {
	if (!src.IsValid() || size == 0 || size > src.size || src.offset > src.buffer.length ||
	    size > src.buffer.length - src.offset || src.buffer.contents == nullptr) {
		return {};
	}
	auto copy = AllocateDynamic(size, 16);
	if (!copy.IsValid() || copy.cpu_addr == nullptr) return {};
	std::memcpy(copy.cpu_addr, static_cast<const uint8_t*>(src.buffer.contents) + src.offset, size);
	return copy;
}

void MetalBufferManager::ClearCache() {
	std::lock_guard lock(m_cache_mutex);
	m_page_cache.clear();
	if (m_dynamic_ring) {
		m_dynamic_ring->Shutdown();
	}
}

} // namespace Libs::Graphics::Metal
