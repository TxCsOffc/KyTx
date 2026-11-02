#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/bdaSyncPlan.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <deque>
#include <thread>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>
#include <source_location>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class TextureCache;

using BufferId = Common::SlotId;
inline constexpr BufferId NULL_BUFFER_ID {0};

class BufferCache {
public:
	// Session 17: tried 16 (64 KB) to shrink the 768 MB BDA page table to 192 MB. The saving was
	// entirely eaten by per-buffer padding (buffer ranges align to CACHING_PAGESIZE): VMA blocks
	// unchanged at ~4.4 GB, alloc/block fragmentation 13 MB -> 244 MB, shared spill 1.2 -> 1.9 GB,
	// and it introduced multi-second stutters outside the fight. Do not retry without also fixing
	// buffer granularity. The sparse BDA-table experiment below keeps the 16 KB page size.
	static constexpr uint32_t CACHING_PAGEBITS  = 14;
	static constexpr uint64_t CACHING_PAGESIZE  = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES  = (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> CACHING_PAGEBITS;
	static constexpr uint64_t BDA_PAGETABLE_SIZE =
	    CACHING_NUMPAGES * sizeof(vk::DeviceAddress);
	static constexpr uint32_t BDA_LEAF_BITS = 12;
	static constexpr uint64_t BDA_LEAF_ENTRIES = uint64_t {1} << BDA_LEAF_BITS;
	static constexpr uint64_t BDA_LEAF_SIZE = BDA_LEAF_ENTRIES * sizeof(vk::DeviceAddress);
	static constexpr uint64_t BDA_ROOT_SIZE =
	    ((CACHING_NUMPAGES + BDA_LEAF_ENTRIES - 1) / BDA_LEAF_ENTRIES) * sizeof(vk::DeviceAddress);
	[[nodiscard]] static bool SparseBdaEnabled();

	static constexpr uint64_t PageIndex(uint64_t address) {
		return (address < LOWER_ADDRESS_SIZE
		            ? address
		            : address - LibKernel::Memory::kExtendedMemoryBase + LOWER_ADDRESS_SIZE) >>
		       CACHING_PAGEBITS;
	}
	static constexpr uint64_t GuestAddress(uint64_t offset) {
		return offset < LOWER_ADDRESS_SIZE
		           ? offset
		           : offset - LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemoryBase;
	}

	BufferCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	            TextureCache& texture_cache);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
	// Deletes imported buffers overlapping a remapped range (KYTY_GUEST_IMPORT).
	void                   DropImportedBuffers(uint64_t vaddr, uint64_t size);
	// Records a 4/8-byte EOP label as a GPU write into imported memory, in command order
	// (KYTY_GPU_LABELS, default on with KYTY_GUEST_IMPORT). False: caller writes the label on the CPU.
	// Records a 4/8-byte EOP label as a GPU write into imported memory, in command order.
	// Returns 0: not recorded (caller writes it on the CPU); 1: recorded and observable (a guest
	// read of its page waits for it, so the caller may defer the submit); 2: recorded, submit now.
	// lazy=true only in KYTY_LABEL_FLUSH=lazy mode.
	[[nodiscard]] int      TryRecordGpuLabelWrite(CommandBuffer& command, uint64_t address,
	                                              const void* value, uint32_t size, bool lazy);
	enum class LazyFlush : uint8_t { Budget, WaitRegMem, Other, Watchdog };
	// KYTY_LABEL_MODE=cpu_on_complete: the label is written by the CPU once its tick completes.
	[[nodiscard]] bool DeferCpuLabel(uint64_t address, uint64_t value, uint32_t size);
	[[nodiscard]] bool GpuIdleWithOpenWork(bool has_draws);
	void               FlushOpenLabelBuffer();  // GPU thread: watchdog target
	[[nodiscard]] bool     LabelPendingInOpen() const;
	[[nodiscard]] bool     LabelBudgetExceeded() const;
	void                   NoteLazyFlush(LazyFlush reason);
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false,
	                                  std::source_location site = std::source_location::current());
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size);
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBuffer(uint64_t vaddr, uint64_t size,
	                                                        bool     is_written,
	                                                        bool     is_texel_buffer = false,
	                                                        BufferId id              = {});
	[[nodiscard]] StreamBuffer&                GetUtilityBuffer(MemoryUsage usage) noexcept {
		switch (usage) {
			case MemoryUsage::Upload: return m_staging_buffer;
			case MemoryUsage::Stream: return m_stream_buffer;
			case MemoryUsage::Download: return m_download_buffer;
			case MemoryUsage::DeviceLocal: return m_device_buffer;
		}
		EXIT("BufferCache: invalid utility-buffer usage\n");
	}
	[[nodiscard]] const Buffer* GetGdsBuffer() const noexcept { return &m_gds_buffer; }
	[[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept { return &m_bda_pagetable_buffer; }
	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return m_fault_manager.GetFaultBuffer(); }
	[[nodiscard]] size_t RegisteredBufferCount() const noexcept { return m_buffers.size(); }
	[[nodiscard]] uint64_t RegisteredBufferBytes() const noexcept { return m_total_used_memory; }
	[[nodiscard]] uint64_t BdaTableAllocatedBytes() const noexcept {
		return m_bda_pagetable_buffer.Size() + m_bda_leaf_chunks.size() * (64ull * 1024 * 1024);
	}
	[[nodiscard]] uint64_t BdaLeafCount() const noexcept { return m_bda_leaf_count; }
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImage(uint64_t vaddr, uint64_t size);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void FillBufferFromProvenComputeFill(uint64_t vaddr, uint64_t size, uint32_t value);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	void               ProcessFaultBuffer();
	void               SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	BdaSyncCounters SynchronizeBdaRanges(const RangeSet& ranges, uint64_t mapping_version,
	                                    BdaSyncAction action);
	[[nodiscard]] uint64_t RegisterEpoch() const { return m_register_epoch; }
	void               TickFrame();
	void               RunGarbageCollector();
	[[nodiscard]] bool BufferCovers(BufferId id, uint64_t address, uint64_t size) const;
	[[nodiscard]] bool RetainBuffer(BufferId id, uint64_t address, uint64_t size);
	void               NoteShaderWrite(uint64_t address, uint64_t size);
	// Ranges whose metadata readback was skipped stay GPU-modified; guest CPU reads of them
	// must still fault and download, which these counters prove.
	void                   WatchSkippedReadback(uint64_t vaddr, uint64_t size);
	void                   NoteCpuReadFault(uint64_t vaddr);
	[[nodiscard]] uint64_t TakeSkippedReadbackFaults();

private:
	friend struct BufferCacheTestAccess;

	bool IsBufferInvalid(BufferId id) const {
		const auto* buffer = m_slot_buffers.try_get(id);
		return buffer == nullptr || buffer->is_deleted;
	}

	using BufferMap = std::map<uint64_t, BufferId>;
	struct OverlapResult {
		BufferMap::iterator first;
		BufferMap::iterator last;
		uint64_t            begin;
		uint64_t            end;
		bool                has_stream_leap;
	};

	using PageTable = MultiLevelPageTable<BufferId, CACHING_PAGEBITS, 44, 20>;
	static_assert(CACHING_PAGESIZE == (uint64_t {1} << PageTable::kPageBits));
	void WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source, uint64_t size);
	[[nodiscard]] std::pair<Buffer&, uint64_t> GetBdaLeaf(uint64_t leaf_index);
	void TouchBuffer(const Buffer& buffer);
	[[nodiscard]] OverlapResult ResolveOverlaps(uint64_t vaddr, uint64_t size);
	void JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size);
	void                   Register(BufferId id);
	void Unregister(BufferId id);
	template <bool insert>
	void ChangeRegister(BufferId id);
	void DeleteBuffer(BufferId id);
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);

	// Guest-memory import (KYTY_GUEST_IMPORT=1). Buffers for overlap-free guest ranges bind into one
	// imported VkDeviceMemory at their backing offset and skip uploads. Filter mode
	// (KYTY_IMPORT_LISTENER=1) keeps GPU-written imported pages protected, tagged with their writer
	// tick, and releases them on the first guest fault. All of it runs on the GPU thread.
	struct GuestImport {
		bool             attempted         = false;
		bool             ready             = false;
		vk::DeviceMemory memory            = nullptr;
		uint64_t         base              = 0;
		uint64_t         size              = 0;
		uint32_t         memory_type       = 0;
		uint32_t         memory_properties = 0;
		vk::DeviceSize   alignment         = 1;
	};
	struct GuestImportBinding {
		vk::DeviceMemory memory            = nullptr;
		uint64_t         offset            = 0;
		uint32_t         memory_type       = 0;
		uint32_t         memory_properties = 0;
	};
	struct ImportStats {
		uint64_t bufs_imported       = 0;
		uint64_t fallback            = 0;
		uint64_t fallback_overlap    = 0;
		uint64_t fallback_backing    = 0;
		uint64_t fallback_alignment  = 0;
		uint64_t fallback_import     = 0;
		uint64_t readbacks_skipped   = 0;
		uint64_t filter_instant      = 0;
		uint64_t filter_waited       = 0;
		uint64_t filter_wait_ns      = 0;
		uint64_t uploads_skipped     = 0;
		uint64_t adopted_overlaps    = 0;
		uint64_t adopt_copies        = 0;
		uint64_t released_to_device  = 0;
		uint64_t device_overlap_joins = 0;
		uint64_t checks              = 0;
		uint64_t remap_mismatch      = 0;
		uint64_t aliased_ranges      = 0;
		uint64_t demotions           = 0;
		uint64_t readbacks           = 0;
		uint64_t lazy_budget         = 0;
		uint64_t lazy_wait_regmem    = 0;
		uint64_t lazy_other          = 0;
		uint64_t lazy_label_polls    = 0;
		uint64_t lazy_watchdog       = 0;
		int64_t  next_print_ns       = 0;
	};
	// Page-aligned begin -> {end, writer tick} for imported pages still GPU-written.
	using ImportPieces = std::map<uint64_t, std::pair<uint64_t, uint64_t>>;
	[[nodiscard]] bool EnsureGuestImport();
	[[nodiscard]] bool TryGuestImportBinding(uint64_t vaddr, uint64_t size, GuestImportBinding& out);
	void               NoteImportWrite(uint64_t vaddr, uint64_t size, uint64_t tick);
	void               FilterImportedPages(uint64_t vaddr, uint64_t size, uint64_t buffer_begin,
	                                       uint64_t buffer_end);
	uint64_t           ReleaseImportPieces(uint64_t begin, uint64_t end, uint64_t done);
	void               AdoptIntoImported(BufferId new_id, BufferId old_id, bool accumulate_stream_score);
	std::unique_ptr<Buffer> m_label_buffer;  // imported alias of the whole backing (label writes)
	void               ReleaseImportedToDevice(BufferId id);
	void               CheckImportedMapping(const Buffer& buffer);
	// Hybrid mode: a device range written by the GPU becomes an alias; an alias that the CPU
	// writes again becomes a device buffer, and its range is never aliased again.
	BufferId           ConvertToImported(BufferId id);
	BufferId           DemoteImported(BufferId id);
	void               MaybePrintImportStats();
	GuestImport m_guest_import;
	ImportStats m_import_stats;
	ImportPieces m_import_writes;
	RangeSet    m_no_alias;  // ranges demoted from alias back to device buffers
	// Lazy EOP labels: pages kept GPU-dirty until a guest read finds their writer complete.
	RangeSet    m_lazy_label_pages;
	std::atomic<uint64_t>    m_last_label_tick {0};
	int64_t     m_label_since_ns  = 0;
	struct CpuLabel {
		uint64_t tick;
		uint64_t address;
		uint64_t value;
		uint32_t size;
		int64_t  enqueued_ns;
	};
	std::deque<CpuLabel>  m_cpu_labels;
	std::mutex            m_cpu_label_mutex;
	std::thread           m_cpu_label_thread;
	std::atomic<bool>     m_cpu_label_stop {false};
	uint64_t              m_cpu_label_flush_tick = 0;  // label thread only
	int64_t               m_cpu_label_flush_ns = 0;   // label thread only: last watchdog request
	uint64_t              m_cpu_label_report_tick = 0;  // label thread only: last >100 ms report
	void                  CpuLabelLoop();

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	std::vector<std::unique_ptr<Buffer>>              m_bda_leaf_chunks;
	std::unordered_map<uint64_t, uint64_t>            m_bda_leaf_slots;
	uint64_t                                          m_bda_leaf_count = 0;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	// Last GPU writer tick per written range (begin -> {end, tick}); lets a readback wait for
	// its writer instead of draining every queued command.
	std::map<uint64_t, std::pair<uint64_t, uint64_t>> m_writer_ticks;
	void                                              NoteWriterTick(uint64_t vaddr, uint64_t size);
	[[nodiscard]] uint64_t                            WriterTick(uint64_t vaddr, uint64_t size) const;
	MemoryTracker                                     m_memory_tracker;
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
	uint64_t m_register_epoch = 0;
	BdaSyncHistory m_bda_history;
	BdaSyncCounters* m_bda_counters = nullptr;
	const RangeSet* m_bda_expected_uploads = nullptr;
	std::mutex                                  m_skipped_readback_lock;
	std::vector<std::pair<uint64_t, uint64_t>>  m_skipped_readbacks;
	std::atomic<bool>                           m_watch_skipped_readbacks {false};
	std::atomic<uint64_t>                       m_skipped_readback_faults {0};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
