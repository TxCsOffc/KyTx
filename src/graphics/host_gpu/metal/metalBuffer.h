#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALBUFFER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALBUFFER_H_

#include "common/common.h"

#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
typedef void* id;
#endif

namespace Libs::Graphics::Metal {

struct MetalBufferAllocation {
#ifdef __OBJC__
	id<MTLBuffer> buffer     = nil;
#else
	id            buffer     = nullptr;
#endif
	uint64_t      offset     = 0;
	uint64_t      size       = 0;
	void*         cpu_addr   = nullptr;
	uint64_t      gpu_addr   = 0;

	[[nodiscard]] bool IsValid() const noexcept { return buffer != nullptr && size > 0; }
};

class MetalBuffer {
public:
	MetalBuffer();
	~MetalBuffer();

	KYTY_CLASS_NO_COPY(MetalBuffer);

	// Create a dedicated shared UMA buffer
	bool Allocate(size_t length, const char* label = nullptr);

	// Zero-copy wrap an existing page-aligned or arbitrary host memory pointer
	bool WrapHostMemory(void* host_ptr, size_t length, const char* label = nullptr);

	void Release();

#ifdef __OBJC__
	[[nodiscard]] id<MTLBuffer> GetHandle() const noexcept { return m_handle; }
#else
	[[nodiscard]] id GetHandle() const noexcept { return m_handle; }
#endif

	[[nodiscard]] void*    GetContents() const noexcept { return m_contents; }
	[[nodiscard]] size_t   GetLength() const noexcept { return m_length; }
	[[nodiscard]] uint64_t GetOffset() const noexcept { return m_offset; }
	[[nodiscard]] uint64_t GetGpuAddress() const noexcept { return m_gpu_address; }
	[[nodiscard]] bool     IsValid() const noexcept { return m_handle != nullptr; }

	// Write data directly into unified memory (no copy needed beyond memcpy to shared buffer)
	void Write(const void* data, size_t size, size_t dest_offset = 0);

#ifdef __OBJC__
	[[nodiscard]] id<MTLTexture> CreateTextureView(MTLTextureDescriptor* desc,
	                                               NSUInteger offset,
	                                               NSUInteger bytes_per_row);
#endif

private:
#ifdef __OBJC__
	id<MTLBuffer> m_handle      = nil;
#else
	id            m_handle      = nullptr;
#endif
	void*         m_contents    = nullptr;
	size_t        m_length      = 0;
	uint64_t      m_offset      = 0;
	uint64_t      m_gpu_address = 0;
	bool          m_is_wrapped  = false;
};

// High-speed transient linear ring buffer for per-frame dynamic UMA data (constants, uniforms, dynamic vertices)
class MetalDynamicRingBuffer {
public:
	explicit MetalDynamicRingBuffer(size_t capacity = 16 * 1024 * 1024);
	~MetalDynamicRingBuffer();

	KYTY_CLASS_NO_COPY(MetalDynamicRingBuffer);

	bool Initialize();
	void Shutdown();

	// Allocate a slice with requested alignment (e.g. 256 for uniform buffers, 16 for vectors)
	MetalBufferAllocation Allocate(size_t size, size_t alignment = 256);

	// Start fresh storage; outstanding command buffers retain the previous backing.
	void Reset();

private:
	size_t      m_capacity = 0;
	size_t      m_head     = 0;
	MetalBuffer m_buffer;
	std::mutex  m_mutex;
};

// Memory manager for mapping PS5 guest physical/virtual addresses to Metal UMA buffers
class MetalBufferManager {
public:
	static MetalBufferManager& Instance();

	MetalBufferManager();
	~MetalBufferManager();

	KYTY_CLASS_NO_COPY(MetalBufferManager);

	// Get or wrap a guest memory region into a zero-copy shared Metal buffer
	MetalBufferAllocation GetBufferForGuestRange(uint64_t vaddr, size_t size);

	// Transient allocations
	MetalBufferAllocation AllocateDynamic(size_t size, size_t alignment = 256);

	// Copy CPU-owned input at encode time. Never use for data produced by queued GPU work.
	// Returns an invalid allocation if the full source range cannot be copied safely.
	MetalBufferAllocation SnapshotReadOnly(const MetalBufferAllocation& src, size_t size);

	void ResetTransientBuffers();
	void ClearCache();
	// Number and total length of the cached guest-memory wrappers (diagnostics).
	void CacheStats(size_t* count, uint64_t* bytes);

private:
	std::mutex                                      m_cache_mutex;
	struct PageCacheEntry {
		std::shared_ptr<MetalBuffer> buffer;
		int64_t                      used_ms = 0; // steady-clock time of the last lookup
	};
	std::unordered_map<uint64_t, PageCacheEntry> m_page_cache;
	int64_t                                      m_page_cache_swept_ms = 0;
	std::unique_ptr<MetalDynamicRingBuffer>         m_dynamic_ring;
	size_t                                          m_page_size = 16384;
};

} // namespace Libs::Graphics::Metal

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALBUFFER_H_
