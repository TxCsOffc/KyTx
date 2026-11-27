#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALSHADERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALSHADERCACHE_H_

#include "common/common.h"
#include "graphics/host_gpu/metal/metalPipeline.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

namespace Libs::Graphics::Metal {

// Bounded worker pool for parallel MSL / MTLPipelineState compilation and journal prewarm.
class MetalCompilePool {
public:
	static MetalCompilePool& Instance();

	MetalCompilePool() = default;
	~MetalCompilePool();

	KYTY_CLASS_NO_COPY(MetalCompilePool);

	void Start(size_t worker_count = 0);
	void Stop();
	void WaitIdle();
	void Enqueue(std::function<void()> task);

	[[nodiscard]] size_t WorkerCount() const noexcept { return m_workers.size(); }

private:
	void WorkerLoop();

	mutable std::mutex             m_mutex;
	std::condition_variable        m_cv;
	std::condition_variable        m_idle_cv;
	std::deque<std::function<void()>> m_queue;
	std::vector<std::thread>       m_workers;
	size_t                         m_active  = 0;
	bool                           m_stopping = false;
};

// Persistent MTLBinaryArchive + disk .metal cache + binary pipelines.journal for Metal GPU pipelines.
class MetalBinaryCache {
public:
	struct StatsSnapshot {
		uint64_t archive_adds            = 0;
		uint64_t msl_disk_hits           = 0;
		uint64_t msl_disk_writes         = 0;
		uint64_t journal_entries_loaded  = 0;
		uint64_t journal_entries_written = 0;
		uint64_t prewarmed_pipelines     = 0;
	};

	static MetalBinaryCache& Instance();

	MetalBinaryCache() = default;
	~MetalBinaryCache();

	KYTY_CLASS_NO_COPY(MetalBinaryCache);

#ifdef __OBJC__
	bool Initialize(id<MTLDevice> device, const std::string& cache_root = {});
	void AttachToRenderDescriptor(MTLRenderPipelineDescriptor* desc) const;
	void AttachToComputeDescriptor(MTLComputePipelineDescriptor* desc) const;
	void AddRenderPipeline(MTLRenderPipelineDescriptor* desc, const MetalRenderPipelineKey& key);
	void AddComputePipeline(MTLComputePipelineDescriptor* desc, uint64_t cs_hash);
#else
	bool Initialize(void* device, const std::string& cache_root = {});
#endif

	void Shutdown();
	void Flush();

	[[nodiscard]] bool                  IsEnabled() const noexcept { return m_enabled.load(std::memory_order_acquire); }
	[[nodiscard]] std::filesystem::path MslDir() const;
	[[nodiscard]] std::filesystem::path ArchivePath() const;
	[[nodiscard]] std::filesystem::path JournalPath() const;

	size_t PrewarmJournalSync();
	void   PrewarmJournalAsync();

	void RecordMslDiskHit() noexcept { m_stats.msl_disk_hits.fetch_add(1, std::memory_order_relaxed); }
	void RecordMslDiskWrite() noexcept { m_stats.msl_disk_writes.fetch_add(1, std::memory_order_relaxed); }

	[[nodiscard]] StatsSnapshot GetStats() const noexcept;

private:
	struct Stats {
		std::atomic<uint64_t> archive_adds {0};
		std::atomic<uint64_t> msl_disk_hits {0};
		std::atomic<uint64_t> msl_disk_writes {0};
		std::atomic<uint64_t> journal_entries_loaded {0};
		std::atomic<uint64_t> journal_entries_written {0};
		std::atomic<uint64_t> prewarmed_pipelines {0};
	};

	void LoadJournalLocked();
	bool AppendJournalComputeLocked(uint64_t cs_hash);
	bool AppendJournalRenderLocked(const MetalRenderPipelineKey& key);
	void SerializeArchiveLocked();

	mutable std::mutex    m_mutex;
	std::atomic<bool>     m_enabled {false};
	bool                  m_archive_dirty = false;
	std::filesystem::path m_cache_dir;
	std::filesystem::path m_msl_dir;
	std::filesystem::path m_archive_path;
	std::filesystem::path m_journal_path;

#ifdef __OBJC__
	id<MTLDevice>        m_device  = nil;
	id<MTLBinaryArchive> m_archive = nil;
#else
	void* m_device  = nullptr;
	void* m_archive = nullptr;
#endif

	std::vector<uint64_t>               m_loaded_compute_hashes;
	std::vector<MetalRenderPipelineKey> m_loaded_render_keys;
	std::unordered_set<uint64_t>        m_recorded_compute_hashes;
	std::unordered_set<MetalRenderPipelineKey, MetalRenderPipelineKeyHash> m_recorded_render_keys;
	Stats m_stats;
};

} // namespace Libs::Graphics::Metal

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALSHADERCACHE_H_
