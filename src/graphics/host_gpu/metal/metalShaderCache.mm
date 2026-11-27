#include "graphics/host_gpu/metal/metalShaderCache.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/metal/metalEnv.h"
#include "graphics/host_gpu/metal/metalShader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/sysctl.h>
#include <xxhash.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

namespace Libs::Graphics::Metal {

namespace {

constexpr uint32_t kJournalMagic   = 0x4A54594Bu; // "KYTJ"
constexpr uint32_t kJournalVersion = 1u;

enum class JournalRecordKind : uint8_t {
	Compute = 1,
	Render  = 2,
};

struct SerializedColorAttachment {
	uint32_t pixel_format     = 0;
	uint8_t  blending_enabled = 0;
	uint8_t  reserved[3]      = {};
	uint32_t write_mask       = 0;
	uint32_t src_rgb          = 0;
	uint32_t dst_rgb          = 0;
	uint32_t rgb_op           = 0;
	uint32_t src_alpha        = 0;
	uint32_t dst_alpha        = 0;
	uint32_t alpha_op         = 0;
};

struct SerializedRenderRecord {
	uint64_t                  vs_hash            = 0;
	uint64_t                  fs_hash            = 0;
	uint32_t                  color_target_count = 0;
	uint32_t                  depth_format       = 0;
	uint32_t                  stencil_format     = 0;
	uint32_t                  sample_count       = 1;
	uint32_t                  primitive_type     = 0;
	uint32_t                  reserved           = 0;
	SerializedColorAttachment color_targets[8]   = {};
};

uint64_t ComputeDeviceFingerprint(id<MTLDevice> device) {
	std::string seed = "kyty-fast-metal-v1|";
	if (device != nil && device.name != nil) {
		seed.append(device.name.UTF8String);
	}
	seed.push_back('|');
	char   os_ver[128] = {};
	size_t os_len      = sizeof(os_ver);
	if (sysctlbyname("kern.osversion", os_ver, &os_len, nullptr, 0) == 0 && os_len > 0) {
		seed.append(os_ver, strnlen(os_ver, sizeof(os_ver)));
	}
	return XXH3_64bits(seed.data(), seed.size());
}

size_t DefaultCompileThreadCount() {
	if (const char* env = KYTY_ENV("KYTY_METAL_COMPILE_THREADS")) {
		const int parsed = std::atoi(env);
		if (parsed > 0) {
			return static_cast<size_t>(std::clamp(parsed, 1, 16));
		}
	}
	const unsigned hw = std::thread::hardware_concurrency();
	if (hw == 0) {
		return 2;
	}
	return static_cast<size_t>(std::clamp(hw / 2u, 2u, 6u));
}

bool EnsureJournalHeader(std::FILE* file) {
	if (file == nullptr) {
		return false;
	}
	std::fseek(file, 0, SEEK_END);
	if (std::ftell(file) == 0) {
		const uint32_t header[2] = {kJournalMagic, kJournalVersion};
		if (std::fwrite(header, sizeof(uint32_t), 2, file) != 2) {
			return false;
		}
	}
	return true;
}

} // namespace

MetalCompilePool& MetalCompilePool::Instance() {
	static auto* instance = new MetalCompilePool();
	return *instance;
}

MetalCompilePool::~MetalCompilePool() { Stop(); }

void MetalCompilePool::Start(size_t worker_count) {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_workers.empty()) {
		return;
	}
	m_stopping        = false;
	const size_t n    = worker_count > 0 ? worker_count : DefaultCompileThreadCount();
	m_workers.reserve(n);
	for (size_t i = 0; i < n; ++i) {
		m_workers.emplace_back([this]() { WorkerLoop(); });
	}
}

void MetalCompilePool::Stop() {
	std::vector<std::thread> to_join;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_workers.empty()) {
			return;
		}
		m_stopping = true;
		m_queue.clear();
		to_join.swap(m_workers);
	}
	m_cv.notify_all();
	for (auto& t: to_join) {
		if (t.joinable()) {
			t.join();
		}
	}
}

void MetalCompilePool::WaitIdle() {
	std::unique_lock<std::mutex> lock(m_mutex);
	m_idle_cv.wait(lock, [this]() { return m_queue.empty() && m_active == 0; });
}

void MetalCompilePool::Enqueue(std::function<void()> task) {
	if (!task) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_workers.empty()) {
			m_stopping     = false;
			const size_t n = DefaultCompileThreadCount();
			m_workers.reserve(n);
			for (size_t i = 0; i < n; ++i) {
				m_workers.emplace_back([this]() { WorkerLoop(); });
			}
		}
		if (m_stopping) {
			return;
		}
		m_queue.push_back(std::move(task));
	}
	m_cv.notify_one();
}

void MetalCompilePool::WorkerLoop() {
	for (;;) {
		std::function<void()> task;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait(lock, [this]() { return m_stopping || !m_queue.empty(); });
			if (m_stopping && m_queue.empty()) {
				return;
			}
			task = std::move(m_queue.front());
			m_queue.pop_front();
			++m_active;
		}
		@autoreleasepool {
			task();
		}
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			--m_active;
			if (m_queue.empty() && m_active == 0) {
				m_idle_cv.notify_all();
			}
		}
	}
}

MetalBinaryCache& MetalBinaryCache::Instance() {
	static auto* instance = new MetalBinaryCache();
	return *instance;
}

MetalBinaryCache::~MetalBinaryCache() { Shutdown(); }

bool MetalBinaryCache::Initialize(id<MTLDevice> device, const std::string& cache_root) {
	if (KYTY_ENV("KYTY_METAL_NO_BINARY_ARCHIVE") != nullptr) {
		m_enabled.store(false, std::memory_order_release);
		return false;
	}
	if (device == nil) {
		return false;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_device == device && m_enabled.load(std::memory_order_relaxed) && cache_root.empty()) {
		return true;
	}
	if (m_archive != nil && m_archive_dirty) {
		SerializeArchiveLocked();
	}

	m_stats.archive_adds.store(0, std::memory_order_relaxed);
	m_stats.msl_disk_hits.store(0, std::memory_order_relaxed);
	m_stats.msl_disk_writes.store(0, std::memory_order_relaxed);
	m_stats.journal_entries_loaded.store(0, std::memory_order_relaxed);
	m_stats.journal_entries_written.store(0, std::memory_order_relaxed);
	m_stats.prewarmed_pipelines.store(0, std::memory_order_relaxed);
	m_loaded_compute_hashes.clear();
	m_loaded_render_keys.clear();
	m_recorded_compute_hashes.clear();
	m_recorded_render_keys.clear();
	m_archive_dirty = false;
	m_archive       = nil;
	m_device        = device;

	std::filesystem::path root_path;
	if (!cache_root.empty()) {
		root_path = cache_root;
	} else if (const char* env_dir = KYTY_ENV("KYTY_METAL_CACHE_DIR");
	           env_dir != nullptr && env_dir[0] != '\0') {
		root_path = env_dir;
	} else {
		root_path = "_PipelineCache/metal_fast";
	}

	const uint64_t fp = ComputeDeviceFingerprint(device);
	char           fp_hex[32];
	std::snprintf(fp_hex, sizeof(fp_hex), "%016llx", static_cast<unsigned long long>(fp));
	m_cache_dir    = root_path / fp_hex;
	m_msl_dir      = m_cache_dir / "msl";
	m_archive_path = m_cache_dir / "archive.metallib";
	m_journal_path = m_cache_dir / "pipelines.journal";

	std::error_code ec;
	std::filesystem::create_directories(m_msl_dir, ec);
	if (ec) {
		LOGF("MetalBinaryCache: failed to create %s: %s\n", m_cache_dir.c_str(), ec.message().c_str());
		m_enabled.store(false, std::memory_order_release);
		return false;
	}

	@autoreleasepool {
		MTLBinaryArchiveDescriptor* bin_desc = [[MTLBinaryArchiveDescriptor alloc] init];
		if (std::filesystem::exists(m_archive_path, ec)) {
			NSString* path_ns = [NSString stringWithUTF8String:m_archive_path.c_str()];
			bin_desc.url      = [NSURL fileURLWithPath:path_ns];
			NSError* err      = nil;
			m_archive         = [device newBinaryArchiveWithDescriptor:bin_desc error:&err];
			if (m_archive == nil) {
				LOGF("MetalBinaryCache: ignoring stale archive %s (%s)\n", m_archive_path.c_str(),
				     err ? err.localizedDescription.UTF8String : "unknown");
			}
		}
		if (m_archive == nil) {
			bin_desc.url = nil;
			NSError* err = nil;
			m_archive    = [device newBinaryArchiveWithDescriptor:bin_desc error:&err];
			if (m_archive == nil) {
				LOGF("MetalBinaryCache: failed to create archive (%s)\n",
				     err ? err.localizedDescription.UTF8String : "unknown");
			}
		}
	}

	LoadJournalLocked();
	MetalCompilePool::Instance().Start();
	m_enabled.store(true, std::memory_order_release);
	return true;
}

void MetalBinaryCache::Shutdown() {
	MetalCompilePool::Instance().WaitIdle();
	MetalCompilePool::Instance().Stop();
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_archive != nil && m_archive_dirty) {
		SerializeArchiveLocked();
	}
	m_archive       = nil;
	m_device        = nil;
	m_archive_dirty = false;
	m_enabled.store(false, std::memory_order_release);
}

void MetalBinaryCache::Flush() {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_archive != nil && m_archive_dirty) {
		SerializeArchiveLocked();
	}
}

std::filesystem::path MetalBinaryCache::MslDir() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_msl_dir;
}

std::filesystem::path MetalBinaryCache::ArchivePath() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_archive_path;
}

std::filesystem::path MetalBinaryCache::JournalPath() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_journal_path;
}

void MetalBinaryCache::AttachToRenderDescriptor(MTLRenderPipelineDescriptor* desc) const {
	if (desc == nil || !m_enabled.load(std::memory_order_acquire)) {
		return;
	}
	id<MTLBinaryArchive> archive = nil;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		archive = m_archive;
	}
	if (archive != nil) {
		desc.binaryArchives = @[archive];
	}
}

void MetalBinaryCache::AttachToComputeDescriptor(MTLComputePipelineDescriptor* desc) const {
	if (desc == nil || !m_enabled.load(std::memory_order_acquire)) {
		return;
	}
	id<MTLBinaryArchive> archive = nil;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		archive = m_archive;
	}
	if (archive != nil) {
		desc.binaryArchives = @[archive];
	}
}

void MetalBinaryCache::AddRenderPipeline(MTLRenderPipelineDescriptor*  desc,
                                         const MetalRenderPipelineKey& key) {
	if (!m_enabled.load(std::memory_order_acquire)) {
		return;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_archive != nil && desc != nil) {
		@autoreleasepool {
			NSError* err = nil;
			if ([m_archive addRenderPipelineFunctionsWithDescriptor:desc error:&err]) {
				m_archive_dirty = true;
				m_stats.archive_adds.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}
	(void)AppendJournalRenderLocked(key);
}

void MetalBinaryCache::AddComputePipeline(MTLComputePipelineDescriptor* desc, uint64_t cs_hash) {
	if (!m_enabled.load(std::memory_order_acquire)) {
		return;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_archive != nil && desc != nil) {
		@autoreleasepool {
			NSError* err = nil;
			if ([m_archive addComputePipelineFunctionsWithDescriptor:desc error:&err]) {
				m_archive_dirty = true;
				m_stats.archive_adds.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}
	(void)AppendJournalComputeLocked(cs_hash);
}

void MetalBinaryCache::SerializeArchiveLocked() {
	if (m_archive == nil || m_archive_path.empty()) {
		return;
	}
	@autoreleasepool {
		std::filesystem::path tmp_path = m_archive_path;
		tmp_path += ".tmp";
		NSString* tmp_ns = [NSString stringWithUTF8String:tmp_path.c_str()];
		NSURL*    url    = [NSURL fileURLWithPath:tmp_ns];
		NSError*  err    = nil;
		if ([m_archive serializeToURL:url error:&err]) {
			std::error_code ec;
			std::filesystem::rename(tmp_path, m_archive_path, ec);
			if (!ec) {
				m_archive_dirty = false;
				return;
			}
		} else if (err != nil) {
			LOGF("MetalBinaryCache: failed to serialize archive: %s\n", err.localizedDescription.UTF8String);
		}
		std::error_code ec;
		std::filesystem::remove(tmp_path, ec);
	}
}

void MetalBinaryCache::LoadJournalLocked() {
	std::FILE* file = std::fopen(m_journal_path.c_str(), "rb");
	if (file == nullptr) {
		return;
	}
	uint32_t header[2] = {};
	if (std::fread(header, sizeof(uint32_t), 2, file) != 2 || header[0] != kJournalMagic ||
	    header[1] != kJournalVersion) {
		std::fclose(file);
		return;
	}

	uint8_t kind_raw = 0;
	while (std::fread(&kind_raw, 1, 1, file) == 1) {
		const auto kind = static_cast<JournalRecordKind>(kind_raw);
		if (kind == JournalRecordKind::Compute) {
			uint64_t cs_hash = 0;
			if (std::fread(&cs_hash, sizeof(cs_hash), 1, file) != 1) {
				break;
			}
			if (m_recorded_compute_hashes.insert(cs_hash).second) {
				m_loaded_compute_hashes.push_back(cs_hash);
				m_stats.journal_entries_loaded.fetch_add(1, std::memory_order_relaxed);
			}
		} else if (kind == JournalRecordKind::Render) {
			SerializedRenderRecord rec {};
			if (std::fread(&rec, sizeof(rec), 1, file) != 1) {
				break;
			}
			MetalRenderPipelineKey key {};
			key.vs_hash            = rec.vs_hash;
			key.fs_hash            = rec.fs_hash;
			key.color_target_count = std::min<uint32_t>(rec.color_target_count, 8u);
			key.depth_format       = static_cast<MTLPixelFormat>(rec.depth_format);
			key.stencil_format     = static_cast<MTLPixelFormat>(rec.stencil_format);
			key.sample_count       = rec.sample_count;
			key.primitive_type     = rec.primitive_type;
			for (uint32_t i = 0; i < 8u; ++i) {
				const auto& src             = rec.color_targets[i];
				auto&       dst             = key.color_targets[i];
				dst.pixel_format            = static_cast<MTLPixelFormat>(src.pixel_format);
				dst.blending_enabled        = src.blending_enabled != 0;
				dst.write_mask              = static_cast<MTLColorWriteMask>(src.write_mask);
				dst.src_rgb                 = static_cast<MTLBlendFactor>(src.src_rgb);
				dst.dst_rgb                 = static_cast<MTLBlendFactor>(src.dst_rgb);
				dst.rgb_op                  = static_cast<MTLBlendOperation>(src.rgb_op);
				dst.src_alpha               = static_cast<MTLBlendFactor>(src.src_alpha);
				dst.dst_alpha               = static_cast<MTLBlendFactor>(src.dst_alpha);
				dst.alpha_op                = static_cast<MTLBlendOperation>(src.alpha_op);
			}
			if (m_recorded_render_keys.insert(key).second) {
				m_loaded_render_keys.push_back(key);
				m_stats.journal_entries_loaded.fetch_add(1, std::memory_order_relaxed);
			}
		} else {
			break;
		}
	}
	std::fclose(file);
}

bool MetalBinaryCache::AppendJournalComputeLocked(uint64_t cs_hash) {
	if (cs_hash == 0 || m_journal_path.empty()) {
		return false;
	}
	if (!m_recorded_compute_hashes.insert(cs_hash).second) {
		return false;
	}
	std::FILE* file = std::fopen(m_journal_path.c_str(), "ab+");
	if (file == nullptr) {
		return false;
	}
	if (!EnsureJournalHeader(file)) {
		std::fclose(file);
		return false;
	}
	const uint8_t kind = static_cast<uint8_t>(JournalRecordKind::Compute);
	const bool    ok   = std::fwrite(&kind, 1, 1, file) == 1 &&
	                     std::fwrite(&cs_hash, sizeof(cs_hash), 1, file) == 1;
	std::fclose(file);
	if (ok) {
		m_stats.journal_entries_written.fetch_add(1, std::memory_order_relaxed);
	}
	return ok;
}

bool MetalBinaryCache::AppendJournalRenderLocked(const MetalRenderPipelineKey& key) {
	if (key.vs_hash == 0 || key.fs_hash == 0 || m_journal_path.empty()) {
		return false;
	}
	if (!m_recorded_render_keys.insert(key).second) {
		return false;
	}
	std::FILE* file = std::fopen(m_journal_path.c_str(), "ab+");
	if (file == nullptr) {
		return false;
	}
	if (!EnsureJournalHeader(file)) {
		std::fclose(file);
		return false;
	}
	SerializedRenderRecord rec {};
	rec.vs_hash            = key.vs_hash;
	rec.fs_hash            = key.fs_hash;
	rec.color_target_count = key.color_target_count;
	rec.depth_format       = static_cast<uint32_t>(key.depth_format);
	rec.stencil_format     = static_cast<uint32_t>(key.stencil_format);
	rec.sample_count       = key.sample_count;
	rec.primitive_type     = key.primitive_type;
	for (uint32_t i = 0; i < 8u; ++i) {
		const auto& src      = key.color_targets[i];
		auto&       dst      = rec.color_targets[i];
		dst.pixel_format     = static_cast<uint32_t>(src.pixel_format);
		dst.blending_enabled = src.blending_enabled ? 1u : 0u;
		dst.write_mask       = static_cast<uint32_t>(src.write_mask);
		dst.src_rgb          = static_cast<uint32_t>(src.src_rgb);
		dst.dst_rgb          = static_cast<uint32_t>(src.dst_rgb);
		dst.rgb_op           = static_cast<uint32_t>(src.rgb_op);
		dst.src_alpha        = static_cast<uint32_t>(src.src_alpha);
		dst.dst_alpha        = static_cast<uint32_t>(src.dst_alpha);
		dst.alpha_op         = static_cast<uint32_t>(src.alpha_op);
	}
	const uint8_t kind = static_cast<uint8_t>(JournalRecordKind::Render);
	const bool    ok   = std::fwrite(&kind, 1, 1, file) == 1 &&
	                     std::fwrite(&rec, sizeof(rec), 1, file) == 1;
	std::fclose(file);
	if (ok) {
		m_stats.journal_entries_written.fetch_add(1, std::memory_order_relaxed);
	}
	return ok;
}

void MetalBinaryCache::PrewarmJournalAsync() {
	if (!m_enabled.load(std::memory_order_acquire)) {
		return;
	}
	std::vector<uint64_t>               computes;
	std::vector<MetalRenderPipelineKey> renders;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		computes = m_loaded_compute_hashes;
		renders  = m_loaded_render_keys;
	}
	auto& pool    = MetalCompilePool::Instance();
	auto& shaders = MetalShaderManager::Instance();
	auto& pipes   = MetalPipelineManager::Instance();

	for (uint64_t cs_hash: computes) {
		pool.Enqueue([this, &shaders, &pipes, cs_hash]() {
			id<MTLFunction> fn = shaders.GetFunction(cs_hash);
			if (fn != nil && pipes.GetOrCreateComputePipeline(cs_hash, fn) != nil) {
				m_stats.prewarmed_pipelines.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
	for (const auto& key: renders) {
		pool.Enqueue([this, &shaders, &pipes, key]() {
			id<MTLFunction> vs = shaders.GetFunction(key.vs_hash);
			id<MTLFunction> fs = shaders.GetFunction(key.fs_hash);
			if (vs != nil && fs != nil && pipes.GetOrCreateRenderPipeline(key, vs, fs) != nil) {
				m_stats.prewarmed_pipelines.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
}

size_t MetalBinaryCache::PrewarmJournalSync() {
	const uint64_t before = m_stats.prewarmed_pipelines.load(std::memory_order_relaxed);
	PrewarmJournalAsync();
	MetalCompilePool::Instance().WaitIdle();
	return static_cast<size_t>(m_stats.prewarmed_pipelines.load(std::memory_order_relaxed) - before);
}

MetalBinaryCache::StatsSnapshot MetalBinaryCache::GetStats() const noexcept {
	return {
	    .archive_adds            = m_stats.archive_adds.load(std::memory_order_relaxed),
	    .msl_disk_hits           = m_stats.msl_disk_hits.load(std::memory_order_relaxed),
	    .msl_disk_writes         = m_stats.msl_disk_writes.load(std::memory_order_relaxed),
	    .journal_entries_loaded  = m_stats.journal_entries_loaded.load(std::memory_order_relaxed),
	    .journal_entries_written = m_stats.journal_entries_written.load(std::memory_order_relaxed),
	    .prewarmed_pipelines     = m_stats.prewarmed_pipelines.load(std::memory_order_relaxed),
	};
}

} // namespace Libs::Graphics::Metal
