#include "graphics/host_gpu/metal/metalEnv.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <cstdlib>
#include <mutex>
#include <set>
#include "graphics/host_gpu/metal/metalBuffer.h"
#include "graphics/host_gpu/metal/metalContext.h"
#include "kernel/memory.h"
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

// A replaced ring backing can still be read by encoded or committed work, also through raw GPU addresses inside argument
// buffers (flattened SRTs), which Metal does not keep alive for us. It is retained until the open command buffer completes:
// one queue completes in order, so every earlier buffer that could reference it has completed by then.
static void RetainRingUntilCurrentWorkCompletes(id<MTLBuffer> retired) {
	if (retired == nil) return;
	static std::atomic<uint64_t> s_wraps {0};
	const uint64_t n = s_wraps.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n <= 8 || n % 256 == 0) std::fprintf(stderr, "[DynRing] backing replaced #%llu (old kept until the open command buffer completes)\n", (unsigned long long)n);
	id<MTLCommandBuffer> cb = MetalContext::Instance().GetCurrentCommandBuffer();
	if (cb == nil) return;
	[cb addCompletedHandler:^(id<MTLCommandBuffer>) { (void)retired; }];
}

MetalBufferAllocation MetalDynamicRingBuffer::Allocate(size_t size, size_t alignment) {
	id<MTLBuffer> retired = nil;
	MetalBufferAllocation alloc {};
	{
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
		// Encoded commands can still read the old allocation: never overwrite it, and keep it alive (see above).
		retired = m_buffer.GetHandle();
		if (!m_buffer.Allocate(m_capacity, "DynamicRingBuffer")) return {};
		aligned_head = 0;
	}

	m_head = aligned_head + size;

	alloc.buffer = m_buffer.GetHandle();
	alloc.offset = m_buffer.GetOffset() + aligned_head;
	alloc.size = size;
	alloc.cpu_addr = static_cast<uint8_t*>(m_buffer.GetContents()) + aligned_head;
	alloc.gpu_addr = m_buffer.GetGpuAddress() ? (m_buffer.GetGpuAddress() + aligned_head) : 0;
	}
	RetainRingUntilCurrentWorkCompletes(retired);
	return alloc;
}

void MetalDynamicRingBuffer::Reset() {
	id<MTLBuffer> retired = nil;
	{
		std::lock_guard lock(m_mutex);
		// Reset does not imply that all previously encoded GPU work has completed.
		retired = m_buffer.GetHandle();
		m_buffer.Allocate(m_capacity, "DynamicRingBuffer");
		m_head = 0;
	}
	RetainRingUntilCurrentWorkCompletes(retired);
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

std::shared_ptr<MetalBuffer> MetalBufferManager::GetBackingChunk(uint64_t chunk) {
	uint64_t alias = 0, alias_size = 0;
	if (!LibKernel::Memory::GetGuestBackingAlias(&alias, &alias_size)) return nullptr;
	const uint64_t start = chunk * kBackingChunkSize;
	if (start >= alias_size) return nullptr;
	std::lock_guard lock(m_chunk_mutex);
	auto& buffer = m_backing_chunks[chunk];
	if (!buffer) {
		auto made = std::make_shared<MetalBuffer>();
		if (!made->WrapHostMemory(reinterpret_cast<void*>(alias + start), std::min(kBackingChunkSize, alias_size - start), "GuestAddressBacking")) {
			m_backing_chunks.erase(chunk);
			return nullptr;
		}
		buffer = std::move(made);
	}
	return buffer;
}

MetalBufferAllocation MetalBufferManager::GetBufferForGuestRange(uint64_t vaddr, size_t size) {
	if (vaddr == 0 || size == 0) {
		return {};
	}
	// A wrapper per start page gave the same physical bytes several Metal buffers: a longer lookup made a longer wrapper over the same
	// pages, and within that frame a kernel's writes through one buffer were not what the next kernel read through the other. The garments'
	// cloth started from those stale bytes in their first frame and turned to NaN (shirts, shorts and tracksuits missing until the solver
	// settled). A range inside one physical 64 MiB chunk now always goes through that chunk's buffer. KYTY_GUEST_BUFFER_PER_PAGE=1
	// restores the per-page wrappers.
	// KYTY_GUEST_BUFFER_MAPPING=1 (experiment): every guest range is bound through ONE wrapper over the whole committed guest mapping
	// that holds it (a VirtualRanges entry), made on first use and kept: no chunk buffers and no per-start-page wrappers, so a guest
	// byte has one GPU address. c29's node pool (non-contiguous backing -> page wrapper) was written by other kernels through the 64 MiB
	// chunk buffers of the same physical pages: two GPU mappings of one page, not coherent, cyclic lists and an endless walk.
	static const bool s_mapping = std::getenv("KYTY_GUEST_BUFFER_MAPPING") != nullptr;
	if (s_mapping) {
		uint64_t m_start = 0, m_size = 0;
		if (LibKernel::Memory::TryGetGuestMappedRange(vaddr, size, &m_start, &m_size)) {
			std::lock_guard lock(m_cache_mutex);
			const uint64_t generation = LibKernel::Memory::GetGuestBackingGeneration();
			auto& entry = m_mapping_wrappers[m_start];
			if (entry.buffer == nullptr || entry.size != m_size || entry.generation != generation) {
				auto buf = std::make_shared<MetalBuffer>();
				if (buf->WrapHostMemory(reinterpret_cast<void*>(m_start), m_size, "GuestMapping")) {
					static std::atomic<uint32_t> s_log {0};
					if (s_log.fetch_add(1) < 400) {
						std::fprintf(stderr, "[GuestMapping] wrap start=0x%llx size=%llu (%s)\n", (unsigned long long)m_start,
						             (unsigned long long)m_size, entry.buffer == nullptr ? "new" : "remapped");
					}
					entry = {buf, m_size, generation};
				}
			}
			if (entry.buffer != nullptr) {
				MetalBufferAllocation alloc {};
				alloc.buffer = entry.buffer->GetHandle();
				alloc.offset = entry.buffer->GetOffset() + (vaddr - m_start);
				alloc.size = size;
				alloc.cpu_addr = reinterpret_cast<void*>(vaddr);
				alloc.gpu_addr = entry.buffer->GetGpuAddress() ? (entry.buffer->GetGpuAddress() + (vaddr - m_start)) : 0;
				return alloc;
			}
		}
	}
	static const bool s_per_page = std::getenv("KYTY_GUEST_BUFFER_PER_PAGE") != nullptr;
	if (!s_per_page) {
		uint64_t backing = 0;
		const bool have_backing = LibKernel::Memory::TryGetGuestBackingOffset(vaddr, size, &backing);
		// KYTY_LOG_BUFFER_PATH=1: which way each range of 1 MiB or more is bound (first 300 distinct): "chunk" shares the one buffer that
		// the kernels writing those bytes use, "page" (a range over a chunk boundary, or without one backing) gets a wrapper of its own.
		if (static const bool s_log_path = std::getenv("KYTY_LOG_BUFFER_PATH") != nullptr; s_log_path && size >= (1u << 20)) {
			static std::mutex                         s_path_mutex;
			static std::set<std::pair<uint64_t, size_t>> s_path_seen;
			std::lock_guard<std::mutex> path_lock(s_path_mutex);
			if (s_path_seen.size() < 300 && s_path_seen.emplace(vaddr, size).second) {
				const bool in_chunk = have_backing && backing % kBackingChunkSize + size <= kBackingChunkSize;
				std::fprintf(stderr, "[BufferPath] vaddr=0x%llx size=%zu %s backing=%s0x%llx in_chunk=0x%llx\n", static_cast<unsigned long long>(vaddr), size,
				             in_chunk ? "chunk" : "page", have_backing ? "" : "none/", static_cast<unsigned long long>(backing),
				             static_cast<unsigned long long>(have_backing ? backing % kBackingChunkSize : 0));
			}
		}
		if (have_backing && backing % kBackingChunkSize + size <= kBackingChunkSize) {
			if (auto chunk = GetBackingChunk(backing / kBackingChunkSize); chunk && chunk->GetLength() >= backing % kBackingChunkSize + size) {
				const uint64_t inside = backing % kBackingChunkSize;
				MetalBufferAllocation alloc {};
				alloc.buffer = chunk->GetHandle();
				alloc.offset = chunk->GetOffset() + inside;
				alloc.size = size;
				alloc.cpu_addr = reinterpret_cast<void*>(vaddr);
				alloc.gpu_addr = chunk->GetGpuAddress() ? (chunk->GetGpuAddress() + inside) : 0;
				return alloc;
			}
		}
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
	// A wrapper holds the physical pages that were behind its guest range when it was made. When the game maps that range to other
	// backing (a garment's buffers in a new scene), a cached wrapper kept the old pages: one kernel wrote the new data through it while the
	// next read the guest range, which held garbage, and cloth started from NaN (shirts, shorts and tracksuits missing for half a minute).
	// After a mapping change the entry is checked against the backing again. KYTY_PAGE_CACHE_NO_REVALIDATE=1 restores the old cache.
	static const bool s_no_revalidate = std::getenv("KYTY_PAGE_CACHE_NO_REVALIDATE") != nullptr;
	const uint64_t generation = s_no_revalidate ? 0 : LibKernel::Memory::GetGuestBackingGeneration();
	auto it = m_page_cache.find(page_aligned);
	if (it != m_page_cache.end() && !s_no_revalidate && it->second.generation != generation) {
		uint64_t backing = UINT64_MAX;
		if (!LibKernel::Memory::TryGetGuestBackingOffset(page_aligned, it->second.buffer->GetLength(), &backing)) backing = UINT64_MAX - 1;
		if (backing != it->second.backing_offset) {
			static std::atomic<uint32_t> s_log {0};
			if (s_log.fetch_add(1) < 200) {
				std::fprintf(stderr, "[PageCacheStale] page=0x%llx len=%zu backing 0x%llx -> 0x%llx\n", (unsigned long long)page_aligned,
				             it->second.buffer->GetLength(), (unsigned long long)it->second.backing_offset, (unsigned long long)backing);
			}
			m_page_cache.erase(it);
			it = m_page_cache.end();
		} else {
			it->second.generation = generation;
		}
	}
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

	uint64_t backing = UINT64_MAX;
	if (!LibKernel::Memory::TryGetGuestBackingOffset(page_aligned, aligned_size, &backing)) backing = UINT64_MAX - 1;
	m_page_cache[page_aligned] = {buf, now_ms, backing, generation};

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
