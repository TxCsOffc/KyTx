#include "graphics/host_gpu/renderer/productionProfile.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/image/tiler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <tuple>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

namespace {

[[nodiscard]] bool DropTexturesEnabled() noexcept {
	static const bool enabled = [] {
		const char* flag = std::getenv("KYTY_DROP_TEXTURES");
		return flag != nullptr && std::strcmp(flag, "1") == 0;
	}();
	return enabled;
}

struct ColorMetaProfile {
	FILE* file = nullptr;
	uint64_t frame = UINT64_MAX;
	uint64_t calls = 0;
	uint64_t dirty = 0;
	uint64_t requested_bytes = 0;
	uint64_t decoded_slices = 0;
	uint64_t uniform_clears = 0;
	uint64_t nonuniform_slices = 0;
	double readback_ms = 0;
	std::unordered_set<uint64_t> ranges;

	ColorMetaProfile() {
		const char* path = std::getenv("KYTY_COLOR_META_PROFILE");
		if (path != nullptr && *path != '\0') {
			file = std::fopen(path, "w");
			if (file != nullptr) {
				std::fprintf(file, "frame,calls,dirty_readbacks,requested_bytes,readback_ms,"
				                   "distinct_ranges,decoded_slices,uniform_clears,nonuniform_slices\n");
			}
		}
	}

	~ColorMetaProfile() {
		Flush();
		if (file != nullptr) {
			std::fclose(file);
		}
	}

	void Flush() {
		if (file == nullptr || frame == UINT64_MAX) {
			return;
		}
		std::fprintf(file, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
		                   ",%.3f,%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
		             frame, calls, dirty, requested_bytes, readback_ms, ranges.size(),
		             decoded_slices, uniform_clears, nonuniform_slices);
		std::fflush(file);
	}

	void NextFrame(uint64_t current_frame) {
		if (file == nullptr || frame == current_frame) {
			return;
		}
		Flush();
		frame = current_frame;
		calls = dirty = requested_bytes = decoded_slices = uniform_clears = nonuniform_slices = 0;
		readback_ms = 0;
		ranges.clear();
	}
};

ColorMetaProfile& GetColorMetaProfile() {
	static thread_local ColorMetaProfile profile;
	return profile;
}

struct ColorMetaTrace {
	FILE* file = nullptr;
	std::vector<GuestRange> ranges;

	ColorMetaTrace() {
		const char* path = std::getenv("KYTY_COLOR_META_TRACE");
		if (path != nullptr && *path != '\0') {
			file = std::fopen(path, "w");
			if (file != nullptr) {
				std::fprintf(file, "event,frame,address,size,value,detail\n");
			}
		}
	}

	~ColorMetaTrace() {
		if (file != nullptr) {
			std::fclose(file);
		}
	}
};

ColorMetaTrace& GetColorMetaTrace() {
	static thread_local ColorMetaTrace trace;
	return trace;
}

// Per-range evidence for color-metadata readbacks. A CMask slice can only clear from code 0
// with a decodable render-target clear register, so failing that is known before reading.
struct ColorMetaPrecheckProfile {
	struct Sample {
		uint64_t calls = 0, early_return = 0, dirty = 0;
		uint64_t impossible = 0, dirty_impossible = 0;
		uint64_t not_render_target = 0, no_clear_register = 0, undecodable_clear = 0;
		uint64_t decoded_slices = 0, uniform_clears = 0, impossible_clears = 0;
		uint64_t skipped = 0, skip_cpu_faults = 0;
		uint32_t view_format = 0, clear_word = 0;
		double   readback_ms = 0, impossible_readback_ms = 0;
	};
	using Key = std::tuple<uint32_t, uint64_t, uint64_t>;

	std::mutex                            lock;
	FILE*                                 file = nullptr;
	std::chrono::steady_clock::time_point start   = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point flushed = start;
	std::map<Key, Sample>                 ranges;

	ColorMetaPrecheckProfile() {
		const char* path = std::getenv("KYTY_CMASK_PRECHECK_CSV");
		if (path != nullptr && *path != '\0') {
			file = std::fopen(path, "w");
			if (file != nullptr) {
				std::fprintf(file, "window_end_s,window_s,kind,address,size,calls,early_return,dirty,"
				                   "impossible,dirty_impossible,not_render_target,no_clear_register,"
				                   "undecodable_clear,readback_ms,impossible_readback_ms,decoded_slices,"
				                   "uniform_clears,impossible_clears,skipped,skip_cpu_faults,"
				                   "view_format,clear_word\n");
				std::fflush(file);
			}
		}
	}

	~ColorMetaPrecheckProfile() {
		std::scoped_lock guard {lock};
		Flush(std::chrono::steady_clock::now());
		if (file != nullptr) {
			std::fclose(file);
		}
	}

	void Flush(std::chrono::steady_clock::time_point now) {
		if (file == nullptr) {
			return;
		}
		const auto seconds = [this](std::chrono::steady_clock::time_point at) {
			return std::chrono::duration<double>(at - start).count();
		};
		const double window = std::chrono::duration<double>(now - flushed).count();
		for (const auto& [key, s]: ranges) {
			const auto& [kind, address, size] = key;
			std::fprintf(file,
			             "%.3f,%.3f,%s,0x%016" PRIx64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
			             ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
			             ",%.3f,%.3f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
			             ",%u,0x%08x\n",
			             seconds(now), window,
			             kind == static_cast<uint32_t>(ImageMetadataKind::Cmask) ? "cmask" : "dcc",
			             address, size, s.calls, s.early_return, s.dirty, s.impossible,
			             s.dirty_impossible, s.not_render_target, s.no_clear_register,
			             s.undecodable_clear, s.readback_ms, s.impossible_readback_ms,
			             s.decoded_slices, s.uniform_clears, s.impossible_clears, s.skipped,
			             s.skip_cpu_faults, s.view_format, s.clear_word);
		}
		std::fflush(file);
		ranges.clear();
		flushed = now;
	}

	void Commit(const Key& key, const Sample& sample) {
		std::scoped_lock guard {lock};
		auto& s = ranges[key];
		s.calls += sample.calls;
		s.early_return += sample.early_return;
		s.dirty += sample.dirty;
		s.impossible += sample.impossible;
		s.dirty_impossible += sample.dirty_impossible;
		s.not_render_target += sample.not_render_target;
		s.no_clear_register += sample.no_clear_register;
		s.undecodable_clear += sample.undecodable_clear;
		s.decoded_slices += sample.decoded_slices;
		s.uniform_clears += sample.uniform_clears;
		s.impossible_clears += sample.impossible_clears;
		s.skipped += sample.skipped;
		s.skip_cpu_faults += sample.skip_cpu_faults;
		s.view_format = sample.view_format;
		s.clear_word  = sample.clear_word;
		s.readback_ms += sample.readback_ms;
		s.impossible_readback_ms += sample.impossible_readback_ms;
		const auto now = std::chrono::steady_clock::now();
		if (now - flushed >= std::chrono::seconds(2)) {
			Flush(now);
		}
	}
};

ColorMetaPrecheckProfile* GetColorMetaPrecheckProfile() {
	static ColorMetaPrecheckProfile profile;
	return profile.file != nullptr ? &profile : nullptr;
}

bool CmaskSkipImpossibleEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_CMASK_SKIP_IMPOSSIBLE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

struct ColorMetaPrecheckScope {
	ColorMetaPrecheckProfile*        profile = nullptr;
	ColorMetaPrecheckProfile::Key    key {};
	ColorMetaPrecheckProfile::Sample sample {};

	~ColorMetaPrecheckScope() {
		if (profile != nullptr) {
			profile->Commit(key, sample);
		}
	}
};

// One row per detile dispatch. Slot and group counts match TileManager::Prepare/Record
// for the detile direction: slot = family * 5 + countr_zero(bytes_per_element),
// groups = ceil(width/8) x ceil(height/8) x depth.
struct DetileTrace {
	FILE*                              file = nullptr;
	uint64_t                           frame = UINT64_MAX;
	uint32_t                           call  = 0;
	std::unordered_map<uint64_t, uint32_t> repeats;

	DetileTrace() {
		const char* path = std::getenv("KYTY_DETILE_TRACE");
		if (path != nullptr && *path != '\0') {
			file = std::fopen(path, "w");
			if (file != nullptr) {
				std::fprintf(file,
				             "frame,call,repeat,image,addr,size,binding,buf_mod,cpu_dirty,"
				             "maybe_cpu,gpu_mod,linear_bytes,dispatch,dispatches,family,bpe,"
				             "slot,width,height,depth,pitch,gx,gy,gz,tiled_bytes,tail,reuse\n");
			}
		}
	}

	~DetileTrace() {
		if (file != nullptr) {
			std::fclose(file);
		}
	}
};

DetileTrace& GetDetileTrace() {
	static DetileTrace trace;
	return trace;
}

const char* DetileBindingName(TextureCache::BindingType binding) {
	switch (binding) {
		case TextureCache::BindingType::Texture: return "texture";
		case TextureCache::BindingType::Storage: return "storage";
		case TextureCache::BindingType::RenderTarget: return "rt";
		case TextureCache::BindingType::DepthTarget: return "depth";
		case TextureCache::BindingType::VideoOut: return "video";
	}
	return "other";
}

uint64_t HashGpuTiles(std::span<const GpuTileInfo> tiles) {
	uint64_t hash = 0x9e3779b97f4a7c15ull;
	const auto mix = [&hash](uint64_t value) {
		hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
	};
	mix(tiles.size());
	for (const auto& tile: tiles) {
		mix(static_cast<uint64_t>(tile.family));
		mix(tile.bytes_per_element);
		mix(tile.linear_offset);
		mix(tile.linear_size);
		mix(tile.tiled_offset);
		mix(tile.tiled_size);
		mix(tile.linear_slice_stride);
		mix(tile.width);
		mix(tile.height);
		mix(tile.depth);
		mix(tile.pitch);
		mix(tile.tail_x);
		mix(tile.tail_y);
		mix(static_cast<uint64_t>(tile.tail));
		mix(tile.tiled_width);
		mix(tile.tiled_height);
		mix(tile.surface_z);
	}
	return hash;
}

bool DetileReuseEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DETILE_REUSE");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

void TraceDetileUpload(CommandScheduler& scheduler, ImageId id, const Image& image,
                       TextureCache::BindingType binding, uint64_t guest_address,
                       uint64_t guest_size, uint64_t linear_bytes,
                       std::span<const GpuTileInfo> tiles, bool reuse) {
	auto& trace = GetDetileTrace();
	if (trace.file == nullptr || tiles.empty()) {
		return;
	}
	const uint64_t frame = static_cast<uint64_t>(scheduler.Context().DiagnosticFrameNum());
	if (frame != trace.frame) {
		trace.frame = frame;
		trace.call  = 0;
		trace.repeats.clear();
	}
	const uint32_t repeat = trace.repeats[guest_address]++;
	const uint32_t call   = trace.call++;
	const int      buf_mod = image.IsBufferModified() ? 1 : 0;
	const int      cpu_dirty = image.IsDefinitelyCpuDirty() ? 1 : 0;
	const int      maybe_cpu = image.IsCpuDirty() && !image.IsDefinitelyCpuDirty() ? 1 : 0;
	const int      gpu_mod = image.IsGpuModified() ? 1 : 0;
	const auto     dispatches = static_cast<uint32_t>(tiles.size());
	for (uint32_t index = 0; index < dispatches; index++) {
		const auto& tile = tiles[index];
		const uint32_t element =
		    tile.bytes_per_element == 0 ? 0u : std::countr_zero(tile.bytes_per_element);
		const uint32_t slot = static_cast<uint32_t>(tile.family) * 5u + element;
		std::fprintf(trace.file,
		             "%" PRIu64 ",%u,%u,%u,0x%016" PRIx64 ",%" PRIu64
		             ",%s,%d,%d,%d,%d,%" PRIu64 ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%" PRIu64
		             ",%u,%u\n",
		             frame, call, repeat, id.index, guest_address, guest_size,
		             DetileBindingName(binding), buf_mod, cpu_dirty, maybe_cpu, gpu_mod,
		             linear_bytes, index, dispatches, static_cast<uint32_t>(tile.family),
		             tile.bytes_per_element, slot, tile.width, tile.height, tile.depth, tile.pitch,
		             (tile.width + 7u) / 8u, (tile.height + 7u) / 8u, tile.depth, tile.tiled_size,
		             tile.tail ? 1u : 0u, reuse ? 1u : 0u);
	}
	std::fflush(trace.file);
}

// DispatchDirect runs on the guest GPU thread. Polling at most twice per second keeps
// the experiment switchable in a live fight without touching the frame hot path.
bool ColorMetaCpuFillEnabled() {
	struct Control {
		bool enabled = [] {
			const char* flag = std::getenv("KYTY_COLOR_META_CPU_FILL");
			return flag != nullptr && std::strcmp(flag, "1") == 0;
		}();
		std::string path = [] {
			const char* value = std::getenv("KYTY_COLOR_META_CONTROL_FILE");
			return value != nullptr ? std::string {value} : std::string {};
		}();
		std::chrono::steady_clock::time_point next_check {};
	};
	static Control control;
	if (control.path.empty()) {
		return control.enabled;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now < control.next_check) {
		return control.enabled;
	}
	control.next_check = now + std::chrono::milliseconds {500};
	if (FILE* file = std::fopen(control.path.c_str(), "r"); file != nullptr) {
		char value[16] {};
		if (std::fgets(value, sizeof(value), file) != nullptr) {
			if (value[0] == '1' || std::strncmp(value, "on", 2) == 0) {
				control.enabled = true;
			} else if (value[0] == '0' || std::strncmp(value, "off", 3) == 0) {
				control.enabled = false;
			}
		}
		std::fclose(file);
	}
	return control.enabled;
}

constexpr uint64_t NumFramesBeforeRemoval = 32;

[[nodiscard]] bool DecodeColorClear(const TextureCache::ImageDesc& desc, uint8_t code,
                                  vk::ClearColorValue& clear) {
	const auto& metadata = desc.info.metadata;
	const auto  format   = desc.view_info.format;
	const bool  cmask    = metadata.kind == ImageMetadataKind::Cmask;
	if (cmask ? code == 0 : code == 0x20) {
		// Register clears belong to the color buffer; the texture pipe cannot decode them.
		return desc.type == TextureCache::BindingType::RenderTarget &&
		       metadata.clear_register_valid &&
		       DecodePackedColorClear(format, metadata.clear_word, clear);
	}
	if (cmask) {
		return false;
	}
	switch (code) {
		case 0x00:
		case 0x40:
		case 0x80:
		case 0xc0: break;
		default: return false;
	}
	clear = {};
	if (code == 0x00) {
		return true;
	}
	switch (format) {
		case vk::Format::eR8Unorm:
		case vk::Format::eR8G8Unorm:
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2B10G10R10UnormPack32:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eR5G6B5UnormPack16:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR4G4B4A4UnormPack16:
		case vk::Format::eR16Unorm:
		case vk::Format::eR16G16Unorm:
		case vk::Format::eR16G16B16A16Unorm:
		case vk::Format::eR16Sfloat:
		case vk::Format::eR16G16Sfloat:
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR32Sfloat:
		case vk::Format::eR32G32Sfloat:
		case vk::Format::eR32G32B32A32Sfloat:
		case vk::Format::eB10G11R11UfloatPack32: break;
		default: return false;
	}
	const float          rgb   = (code & 0x80u) != 0 ? 1.0f : 0.0f;
	const float          alpha = (code & 0x40u) != 0 ? 1.0f : 0.0f;
	std::array<float, 4> channels {rgb, rgb, rgb, alpha};
	if (!metadata.dcc_alpha_msb) {
		std::swap(channels[0], channels[3]);
	}
	// DCC clear decoding clamps missing lanes before applying the format swizzle.
	const auto components = vk::componentCount(format);
	if (components == 1) {
		channels[0] = channels[3];
	} else if (components == 2) {
		channels[1] = channels[3];
	}
	switch (format) {
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR5G6B5UnormPack16: std::swap(channels[0], channels[2]); break;
		case vk::Format::eR4G4B4A4UnormPack16:
			std::reverse(channels.begin(), channels.end());
			break;
		default: break;
	}
	clear.float32 = channels;
	return true;
}

[[nodiscard]] std::vector<vk::BufferImageCopy> BuildDepthCopies(const ImageInfo& info,
                                                              uint64_t slice_stride,
                                                              vk::ImageAspectFlags aspect) {
	std::vector<vk::BufferImageCopy> copies(info.resources.layers);
	for (uint32_t layer = 0; layer < info.resources.layers; ++layer) {
		auto& copy             = copies[layer];
		copy.bufferOffset      = slice_stride * layer;
		copy.bufferRowLength   = info.pitch;
		copy.bufferImageHeight = info.extent.height;
		copy.imageSubresource  = {aspect, 0, layer, 1};
		copy.imageExtent       = {info.extent.width, info.extent.height, 1};
	}
	return copies;
}

[[nodiscard]] std::vector<GpuTileInfo> BuildDepthTiles(const ImageInfo& info) {
	TileBlockLayout block {};
	EXIT_NOT_IMPLEMENTED(
	    !TileGetBlockLayout(TileBlockFamily::Depth64KB, info.bytes_per_block, block));
	const auto               full_slice_size = info.data.size / info.resources.layers;
	std::vector<GpuTileInfo> tiles;
	tiles.reserve(info.resources.layers);
	for (uint32_t layer = 0; layer < info.resources.layers; ++layer) {
		const auto offset = full_slice_size * layer;
		tiles.push_back({block.family, block.bytes_per_element, offset, full_slice_size, offset,
		                 full_slice_size, 0, info.extent.width, info.extent.height, 1, info.pitch});
		tiles.back().surface_z = layer;
	}
	return tiles;
}

} // namespace

TextureCache::TextureCache(GraphicContext& graphics, CommandScheduler& scheduler,
                           PageManager& page_manager, BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_page_manager(page_manager),
      m_blit_helper(graphics, scheduler),
      m_tiler(graphics, scheduler, buffer_cache.GetUtilityBuffer(MemoryUsage::Stream)),
      m_buffer_cache(buffer_cache),
      m_readback_linear_images(Config::ReadbackLinearImagesEnabled()) {
	if (m_graphics.CanReportMemoryUsage()) {
		constexpr int64_t GiB = 1024ll * 1024 * 1024;
		const auto        budget =
		    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
		const auto threshold = std::min<int64_t>(budget, 8 * GiB);
		m_pressure_gc_memory = static_cast<uint64_t>(
		    std::max<int64_t>(std::min(budget - 6 * threshold / 10, budget - GiB), GiB + GiB / 2));
		m_critical_gc_memory = static_cast<uint64_t>(
		    std::max<int64_t>(std::min(budget - 2 * threshold / 10, budget - GiB / 2), 3 * GiB));
		m_trigger_gc_memory = static_cast<uint64_t>(std::max<int64_t>((budget - threshold) / 2, 0));
	}
}

TextureCache::~TextureCache() {
	m_slot_images.ForEach([&](ImageId id, const Image& image) {
		if (image.registered) {
			UnregisterImage(id);
		}
	});
}

bool TextureCache::SameBacking(const ImageInfo& cached, const ImageInfo& requested,
                               bool exact_format) {
	if (cached.data.address != requested.data.address) {
		return false;
	}
	if (cached.data.size != requested.data.size) {
		return false;
	}
	if (cached.extent != requested.extent) {
		return false;
	}
	if (cached.resources.levels < requested.resources.levels ||
	    cached.resources.layers < requested.resources.layers) {
		return false;
	}
	if (cached.samples != requested.samples) {
		return false;
	}
	if (cached.bytes_per_block != requested.bytes_per_block) {
		return false;
	}
	if (cached.tile_mode != requested.tile_mode) {
		return false;
	}
	if (!ImageViewOps::FormatsCompatible(cached.pixel_format, requested.pixel_format) ||
	    cached.type != requested.type) {
		return false;
	}
	if (exact_format && cached.pixel_format != requested.pixel_format) {
		return false;
	}
	return true;
}

TextureCache::BindingType TextureCache::UploadBinding(const Image& image) {
	if (image.info.IsDepth()) {
		if (image.info.tile_mode == Prospero::TileMode::kDepth ||
		    image.info.tile_mode == Prospero::TileMode::kLinear) {
			return BindingType::DepthTarget;
		}
		return BindingType::Texture;
	}
	if (image.usage.render_target) {
		return BindingType::RenderTarget;
	}
	if (image.usage.video_out) {
		return BindingType::VideoOut;
	}
	return image.usage.storage ? BindingType::Storage : BindingType::Texture;
}

ImageId TextureCache::InsertImage(const ImageInfo& info) {
	ProfileDetailScope profile("cpu_image_insert", info.data.address, info.data.size);
	const auto id = m_slot_images.insert(m_graphics, m_scheduler, info);
	if (!info.data.Empty()) {
		RegisterImage(id);
	}
	return id;
}

void TextureCache::RegisterImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.registered || image.info.data.Empty()) {
		EXIT("TextureCache: invalid image registration\n");
	}
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: image registration is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size, [this, id](uint64_t page) {
		m_image_page_table[page].push_back(id);
	});
	image.registered = true;
	image.lru_id     = m_lru_cache.Insert(id, m_gc_tick);
	m_total_used_memory += image.AccountedSize();
}

void TextureCache::UnregisterImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	UntrackImage(id);
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: registered image is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size, [this, id](uint64_t page) {
		auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr || !owners->Erase(id)) {
			EXIT("TextureCache: image missing from page owner index\n");
		}
	});
	m_lru_cache.Free(image.lru_id);
	const auto accounted = image.AccountedSize();
	if (accounted > m_total_used_memory) {
		EXIT("TextureCache: image accounting underflow\n");
	}
	m_total_used_memory -= accounted;
	image.registered = false;
}

void TextureCache::DeleteImage(ImageId id) {
	auto* image = m_slot_images.try_get(id);
	if (image == nullptr || !image->registered) {
		return;
	}
	if (!image->depth_id) {
		std::vector<ImageId> associations;
		m_slot_images.ForEach([&](ImageId candidate, const Image& associated) {
			if (associated.depth_id == id) {
				associations.push_back(candidate);
			}
		});
		for (const auto association: associations) {
			FreeImage(association);
		}
	}
	if (image->IsGpuModified()) {
		EXIT("TextureCache: deleting a GPU-modified image without resolving its contents\n");
	}
	m_download_images.erase(id);
	if (image->info.HasMetadata()) {
		const auto metadata = m_surface_metas.find(image->info.metadata.range.address);
		if (metadata != m_surface_metas.end() &&
		    image->info.metadata.kind == ImageMetadataKind::Htile &&
		    metadata->second.type == MetaDataInfo::Type::HTile) {
			// A later binding may have reused this address for another metadata type.
			m_surface_metas.erase(metadata);
		}
	}
	UnregisterImage(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_images.erase(id); });
	} else {
		m_slot_images.erase(id);
	}
}

void TextureCache::FreeImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.IsGpuModified()) {
		image.ClearGpuModified();
	}
	DeleteImage(id);
}

void TextureCache::TouchImage(Image& image) {
	if (image.registered) {
		m_lru_cache.Touch(image.lru_id, m_gc_tick);
	}
}

void TextureCache::MarkAsMaybeDirty(ImageId id, Image& image) {
	image.MarkMaybeCpuDirty();
	if (image.NeedsMaybeCpuHash()) {
		image.SetMaybeCpuHash(image.HashGuestEdges());
	}
	UntrackImage(id);
}

void TextureCache::TrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	const auto image_end   = image.info.data.End();
	if (image_begin == image.track_addr && image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked()) {
		image.track_addr     = image_begin;
		image.track_addr_end = image_end;
		m_page_manager.UpdatePageWatchers<true>(image_begin, image.info.data.size);
		return;
	}
	if (image_begin < image.track_addr) {
		TrackImageHead(id);
	}
	if (image.track_addr_end < image_end) {
		TrackImageTail(id);
	}
}

void TextureCache::TrackImageHead(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	if (image_begin == image.track_addr) {
		return;
	}
	if (!image.IsTracked() || image_begin > image.track_addr) {
		EXIT("TextureCache: invalid image head tracking range\n");
	}
	const auto size  = image.track_addr - image_begin;
	image.track_addr = image_begin;
	m_page_manager.UpdatePageWatchers<true>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_end = image.info.data.End();
	if (image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked() || image.track_addr_end > image_end) {
		EXIT("TextureCache: invalid image tail tracking range\n");
	}
	const auto address   = image.track_addr_end;
	const auto size      = image_end - address;
	image.track_addr_end = image_end;
	m_page_manager.UpdatePageWatchers<true>(address, size);
}

void TextureCache::UntrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.IsTracked()) {
		return;
	}
	const auto address   = image.track_addr;
	const auto size      = image.track_addr_end - image.track_addr;
	image.track_addr     = 0;
	image.track_addr_end = 0;
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::UntrackImageHead(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto begin = image.info.data.address;
	if (!image.IsTracked() || begin < image.track_addr) {
		return;
	}
	const auto address = Common::AlignDown(begin + TRACKER_PAGE_SIZE, TRACKER_PAGE_SIZE);
	const auto size    = address - begin;
	image.track_addr   = address;
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(begin, size);
	}
}

void TextureCache::UntrackImageTail(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto end   = image.info.data.End();
	if (!image.IsTracked() || image.track_addr_end < end) {
		return;
	}
	const auto address   = Common::AlignDown(end, TRACKER_PAGE_SIZE);
	const auto size      = end - address;
	image.track_addr_end = address;
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::TrackImageDownload(ImageId id, Image& image) {
	if (m_readback_linear_images && !image.info.IsTiled() && !image.info.data.Empty()) {
		if (!image.IsGpuModified()) {
			EXIT("TextureCache: cannot enroll a non-GPU-owned image for download\n");
		}
		m_download_images.insert(id);
	}
}

TextureCache::ImageIds TextureCache::FindImagesInRegion(uint64_t address, uint64_t size,
                                                        bool page_overlap) const {
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(address, size, pages)) {
		return {};
	}

	uint32_t query_epoch = ++m_image_query_epoch;
	if (query_epoch == 0) {
		m_slot_images.ForEach([](ImageId, const Image& image) { image.query_epoch = 0; });
		query_epoch = ++m_image_query_epoch;
	}

	ImageIds result;
	ForEachPage(address, size, [&](uint64_t page) {
		const auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr) {
			return;
		}
		owners->ForEach([&](ImageId id) {
			auto* image = m_slot_images.try_get(id);
			if (image == nullptr) {
				return;
			}
			if (image->query_epoch == query_epoch) {
				return;
			}
			image->query_epoch = query_epoch;
			if (image->Overlaps(address, size, page_overlap)) {
				result.push_back(id);
			}
		});
	});
	return result;
}

ImageId TextureCache::GetNullImage(const ImageDesc& desc) {
	const auto format = desc.info.pixel_format;
	if (const auto found = m_null_images.find(format); found != m_null_images.end()) {
		return found->second;
	}
	ImageInfo info {};
	info.pixel_format    = desc.info.pixel_format;
	info.guest_format    = desc.info.guest_format;
	info.type            = Prospero::ImageType::kColor2D;
	info.extent          = {1, 1, 1};
	info.resources       = {1, 1};
	info.pitch           = 1;
	info.bytes_per_block = std::max(desc.info.bytes_per_block, 1u);
	info.samples         = 1;
	info.tile_mode       = Prospero::TileMode::kLinear;
	info.mip_layout[0]   = {0, info.bytes_per_block, 1, 1};
	const auto id        = InsertImage(info);
	m_null_images.emplace(format, id);
	return id;
}

void TextureCache::ValidateImageDesc(const ImageDesc& desc) const {
	ImageOps::Validate(desc.info);
	if (desc.view_info.format == vk::Format::eUndefined || desc.view_info.level_count == 0 ||
	    desc.view_info.layer_count == 0 ||
	    desc.view_info.base_level >= desc.info.resources.levels ||
	    desc.view_info.level_count > desc.info.resources.levels - desc.view_info.base_level ||
	    (!desc.info.IsVolume() &&
	     (desc.view_info.base_layer >= desc.info.resources.layers ||
	      desc.view_info.layer_count > desc.info.resources.layers - desc.view_info.base_layer))) {
		EXIT("TextureCache: invalid image view description\n");
	}
	if (desc.type == BindingType::DepthTarget && !IsSupportedDepthTargetFormat(desc.info)) {
		EXIT("TextureCache: unsupported depth image description\n");
	}
	if (desc.type == BindingType::VideoOut && !IsSupportedVideoOutFormat(desc.info)) {
		EXIT("TextureCache: unsupported video-out image description\n");
	}
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression == VideoOutCompression::Unsupported) {
		EXIT("TextureCache: unsupported compressed video-out description\n");
	}
}

void TextureCache::PrepareImageCopy(Image& image) {
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
}

void TextureCache::RefreshCopySource(ImageId id) {
	auto& image = m_slot_images[id];
	RefreshImage(id);
	if (image.IsDefinitelyCpuDirty()) {
		EXIT("TextureCache: image copy source remained CPU-dirty after refresh\n");
	}
}

bool TextureCache::CopyD16(Image& destination, Image& source) {
	const bool source_depth      = source.info.IsDepth();
	const bool destination_depth = destination.info.IsDepth();
	if (source_depth == destination_depth) {
		return false;
	}
	auto&      depth          = source_depth ? source : destination;
	auto&      color          = source_depth ? destination : source;
	const auto transfer_bytes = DepthAspectTransferBytes(depth.backing.format);
	if (depth.info.bytes_per_block != sizeof(uint16_t) ||
	    color.info.bytes_per_block != sizeof(uint16_t) || transfer_bytes != sizeof(uint32_t)) {
		return false;
	}
	EXIT_IF(source.backing.samples != 1 || destination.backing.samples != 1 ||
	        source.info.resources.levels != 1 || destination.info.resources.levels != 1 ||
	        source.info.extent != destination.info.extent ||
	        source.info.resources.layers != destination.info.resources.layers);

	const auto     layers = depth.info.resources.layers;
	const uint64_t depth_slice =
	    static_cast<uint64_t>(depth.info.pitch) * depth.info.extent.height * transfer_bytes;
	const uint64_t color_slice =
	    static_cast<uint64_t>(color.info.pitch) * color.info.extent.height * sizeof(uint16_t);
	EXIT_IF(layers == 0 || depth_slice > UINT64_MAX / layers || color_slice > UINT64_MAX / layers);
	const auto                       depth_size = depth_slice * layers;
	const auto                       color_size = color_slice * layers;
	std::vector<vk::BufferImageCopy> depth_copies(layers);
	std::vector<vk::BufferImageCopy> color_copies(layers);
	for (uint32_t layer = 0; layer < layers; layer++) {
		depth_copies[layer].bufferOffset      = depth_slice * layer;
		depth_copies[layer].bufferRowLength   = depth.info.pitch;
		depth_copies[layer].bufferImageHeight = depth.info.extent.height;
		depth_copies[layer].imageSubresource  = {vk::ImageAspectFlagBits::eDepth, 0, layer, 1};
		depth_copies[layer].imageExtent       = depth.info.extent;
		color_copies[layer].bufferOffset      = color_slice * layer;
		color_copies[layer].bufferRowLength   = color.info.pitch;
		color_copies[layer].bufferImageHeight = color.info.extent.height;
		color_copies[layer].imageSubresource  = {vk::ImageAspectFlagBits::eColor, 0, layer, 1};
		color_copies[layer].imageExtent       = color.info.extent;
	}

	auto                         depth_buffer = m_tiler.GetScratchBuffer(depth_size);
	auto                         color_buffer = m_tiler.GetScratchBuffer(color_size, depth_buffer.buffer);
	const TileManager::D16Layout promote_layout {
	    .width               = depth.info.extent.width,
	    .height              = depth.info.extent.height,
	    .layers              = layers,
	    .source_row_stride   = static_cast<uint64_t>(color.info.pitch) * sizeof(uint16_t),
	    .target_row_stride   = static_cast<uint64_t>(depth.info.pitch) * transfer_bytes,
	    .source_slice_stride = color_slice,
	    .target_slice_stride = depth_slice,
	};
	const bool d32 = DepthAspectTransferFormat(depth.backing.format) == vk::Format::eD32Sfloat;
	if (source_depth) {
		source.Download(depth_copies, depth_buffer.buffer, depth_buffer.offset, depth_buffer.size);
		m_tiler.ConvertD16(depth_buffer, color_buffer, TileManager::D16Direction::Demote, d32,
		                   {.width               = promote_layout.width,
		                    .height              = promote_layout.height,
		                    .layers              = promote_layout.layers,
		                    .source_row_stride   = promote_layout.target_row_stride,
		                    .target_row_stride   = promote_layout.source_row_stride,
		                    .source_slice_stride = promote_layout.target_slice_stride,
		                    .target_slice_stride = promote_layout.source_slice_stride});
		destination.Upload(color_copies, color_buffer.buffer, color_buffer.offset,
		                   color_buffer.size);
	} else {
		source.Download(color_copies, color_buffer.buffer, color_buffer.offset, color_buffer.size);
		m_tiler.ConvertD16(color_buffer, depth_buffer, TileManager::D16Direction::Promote, d32,
		                   promote_layout);
		destination.Upload(depth_copies, depth_buffer.buffer, depth_buffer.offset,
		                   depth_buffer.size);
	}
	return true;
}

void TextureCache::CopyImage(ImageId destination_id, ImageId source_id) {
	RefreshCopySource(source_id);
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	TrackImage(destination_id);
	if (source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: cannot issue an unequal-sample image copy\n");
	}
	PrepareImageCopy(destination);
	if (source.IsBufferModified()) {
		if (source.info.data == destination.info.data) {
			destination.MarkBufferModified();
		}
		return;
	}
	const bool source_depth = source.info.IsDepth();
	const bool dest_depth   = destination.info.IsDepth();
	const bool direct_copy =
	    (source.backing.image_type == destination.backing.image_type ||
	     (source.backing.image_type != vk::ImageType::e1D &&
	      destination.backing.image_type != vk::ImageType::e1D)) &&
	    (source.backing.format == destination.backing.format ||
	     (!source_depth && !dest_depth &&
	      vk::blockSize(source.backing.format) == vk::blockSize(destination.backing.format)));
	if (direct_copy) {
		destination.CopyImage(source);
	} else if (!CopyD16(destination, source)) {
		if (source.backing.samples != 1 || destination.backing.samples != 1) {
			EXIT("TextureCache: cross-format multisample image copy is unsupported\n");
		}
		auto& copy_buffer = m_buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
		destination.CopyImageWithBuffer(source, copy_buffer);
	}
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
	destination.ClearBufferModified();
}

void TextureCache::CopyImageMip(ImageId destination_id, ImageId source_id, uint32_t mip,
                                uint32_t layer) {
	RefreshCopySource(source_id);
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	TrackImage(destination_id);
	if (source.IsBufferModified() || source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: invalid mip-copy ownership or sample count\n");
	}
	destination.CopyMip(source, mip, layer);
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
                                          ImageId cached_id) {
	auto& cached = m_slot_images[cached_id];
	if ((!cached.info.IsDepth() && !requested.IsDepth()) ||
	    cached.info.tile_mode != requested.tile_mode) {
		return {};
	}
	const bool stencil_match = requested.HasStencil() == cached.info.HasStencil();
	const bool bpp_match     = requested.bytes_per_block == cached.info.bytes_per_block;
	// PPSA04264
	const bool raw_d16_texture =
	    binding == BindingType::Texture && cached.info.IsDepth() &&
	    cached.info.guest_format == Prospero::BufferFormat::k16UNorm &&
	    requested.guest_format == Prospero::BufferFormat::k16UInt &&
	    requested.pixel_format == vk::Format::eR16Uint && cached.backing.samples == 1 &&
	    requested.samples == 1 && requested.data == cached.info.data &&
	    requested.extent == cached.info.extent && requested.resources == cached.info.resources &&
	    requested.type == cached.info.type && requested.pitch == cached.info.pitch &&
	    !requested.HasStencil() && !cached.info.HasStencil() && !requested.HasMetadata() &&
	    !cached.info.HasMetadata();
	// PPSA04264
	const bool retain_cached_layout =
	    requested.samples == 1 && cached.info.samples == 1 && cached.backing.samples == 1 &&
	    requested.bytes_per_block == cached.info.bytes_per_block &&
	    requested.data.address == cached.info.data.address &&
	    requested.data.size < cached.info.data.size && requested.extent == cached.info.extent &&
	    requested.resources.levels == 1 && cached.info.resources.levels == 1 &&
	    requested.resources.layers != 0 && cached.info.resources.layers != 0 &&
	    requested.resources.layers < cached.info.resources.layers &&
	    requested.type == cached.info.type && requested.pitch == cached.info.pitch &&
	    requested.mip_layout[0].offset == 0 &&
	    cached.info.mip_layout[0].offset == 0 &&
	    requested.mip_layout[0].size == requested.data.size &&
	    cached.info.mip_layout[0].size == cached.info.data.size &&
	    requested.data.size % requested.resources.layers == 0 &&
	    cached.info.data.size % cached.info.resources.layers == 0 &&
	    requested.data.size / requested.resources.layers ==
	        cached.info.data.size / cached.info.resources.layers &&
	    !requested.HasStencil() && !cached.info.HasStencil() && !requested.HasMetadata() &&
	    !cached.info.HasMetadata();
	bool recreate = cached.info.resources < requested.resources;
	switch (binding) {
		case BindingType::Texture:
			recreate |= requested.IsDepth() && !cached.info.IsDepth();
			recreate |= raw_d16_texture;
			break;
		case BindingType::Storage: recreate |= cached.info.IsDepth(); break;
		case BindingType::RenderTarget: recreate |= cached.info.IsDepth(); break;
		case BindingType::DepthTarget:
			recreate |= !cached.info.IsDepth();
			recreate |= cached.info.IsDepth() && !(stencil_match && bpp_match);
			break;
		case BindingType::VideoOut: recreate |= cached.info.IsDepth(); break;
	}
	if (!recreate) {
		return cached_id;
	}
	RefreshImage(cached_id);
	auto info = requested;
	if (retain_cached_layout) {
		info.data       = cached.info.data;
		info.resources  = cached.info.resources;
		info.mip_layout = cached.info.mip_layout;
	} else {
		info.resources = std::max(requested.resources, cached.info.resources);
	}
	info.htile_clear_mask     = 0;
	const auto replacement_id = InsertImage(info);
	auto&      replacement    = m_slot_images[replacement_id];
	replacement.usage         = cached.usage;
	if (cached.binding.is_bound || cached.binding.is_target) {
		cached.binding.needs_rebind = true;
	}
	if (cached.backing.samples == replacement.backing.samples) {
		const bool copy_supported =
		    cached.backing.samples == 1 || cached.backing.format == replacement.backing.format ||
		    (!cached.info.IsDepth() && !replacement.info.IsDepth() &&
		     ImageViewOps::FormatsCompatible(cached.backing.format, replacement.backing.format));
		if (copy_supported) {
			CopyImage(replacement_id, cached_id);
		} else {
			LOGF_COLOR(Log::Color::BrightYellow,
			           "TextureCache: unsupported cross-format multisample depth copy\n");
		}
	} else if (cached.backing.samples == 1 && replacement.backing.samples > 1 &&
	           replacement.info.IsDepth()) {
		RefreshCopySource(cached_id);
		if (cached.IsBufferModified() || cached.IsDefinitelyCpuDirty()) {
			EXIT("TextureCache: multisample depth conversion source is not native-current\n");
		}
		PrepareImageCopy(replacement);
		m_blit_helper.ReinterpretColorAsMsDepth(cached, replacement);
		CommitGpuWrite(replacement);
	} else {
		LOGF_COLOR(Log::Color::BrightYellow,
		           "TextureCache: unsupported unequal-sample depth overlap copy (%u -> %u)\n",
		           cached.backing.samples, replacement.backing.samples);
	}
	FreeImage(cached_id);
	return replacement_id;
}

TextureCache::OverlapResult TextureCache::ResolveOverlap(const ImageInfo& requested,
                                                         BindingType binding, ImageId cached_id,
                                                         ImageId merged_id) {
	auto owner = m_slot_images.try_get(cached_id);
	if (owner == nullptr) {
		return {merged_id};
	}
	auto&      cached       = *owner;
	const auto current_tick = m_scheduler.CurrentTick();
	const bool safe_to_delete =
	    current_tick - std::min(current_tick, cached.tick_accessed_last) > NumFramesBeforeRemoval;

	const uint32_t requested_block = requested.bytes_per_block * requested.samples;
	const uint32_t cached_block    = cached.info.bytes_per_block * cached.info.samples;
	if (requested.data.address == cached.info.data.address &&
	    requested.BlockExtent() == cached.info.BlockExtent() && requested_block == cached_block) {
		if (const auto depth_id = ResolveDepthOverlap(requested, binding, cached_id)) {
			return {depth_id};
		}
		// Equal pitch does not imply equal mip placement: a changed extent can move
		// a level into or out of the mip tail. These are separate guest layouts.
		if (requested.tile_mode != cached.info.tile_mode ||
		    (requested.resources == cached.info.resources &&
		     requested.mip_layout != cached.info.mip_layout)) {
			if (safe_to_delete) {
				FreeImage(cached_id);
			}
			return {merged_id};
		}
		if (requested.IsBlock() && !cached.info.IsBlock()) {
			return {ExpandImage(requested, cached_id)};
		}
		// Volume depth is not an array-layer count. A larger depth can retain the
		// same block-slice layout while requiring a larger native image.
		if ((requested.IsVolume() || cached.info.IsVolume()) &&
		    (requested.data.size == cached.info.data.size ||
		     (requested.type == cached.info.type && requested.resources == cached.info.resources &&
		      requested.extent.width == cached.info.extent.width &&
		      requested.extent.height == cached.info.extent.height &&
		      requested.extent.depth > cached.info.extent.depth))) {
			return {ExpandImage(requested, cached_id)};
		}
		// PPSA08394
		// A view cannot change the native image type or grow its extent.
		if (requested.data.size == cached.info.data.size &&
		    requested.resources == cached.info.resources &&
		    ImageViewOps::FormatsCompatible(cached.info.pixel_format, requested.pixel_format) &&
		    (requested.type != cached.info.type
		         ? requested.extent == cached.info.extent
		         : requested.extent.width > cached.info.extent.width &&
		               requested.extent.height >= cached.info.extent.height &&
		               requested.extent.depth >= cached.info.extent.depth)) {
			return {ExpandImage(requested, cached_id)};
		}
		// PS5 mip tails can expose more levels without increasing the guest allocation.
		if (requested.pixel_format == cached.info.pixel_format &&
		    requested.type == cached.info.type && requested.resources > cached.info.resources &&
		    (requested.data.size > cached.info.data.size ||
		     (requested.data.size == cached.info.data.size &&
		      requested.extent == cached.info.extent &&
		      cached.info.resources.levels > 1 &&
		      requested.resources.layers == cached.info.resources.layers))) {
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.pixel_format != cached.info.pixel_format ||
		    requested.data.size <= cached.info.data.size) {
			const auto result_id = merged_id ? merged_id : cached_id;
			const auto result    = m_slot_images.try_get(result_id);
			return {result != nullptr && ImageViewOps::FormatsCompatible(result->info.pixel_format,
			                                                             requested.pixel_format)
			            ? result_id
			            : ImageId {}};
		}
		EXIT("TextureCache: unresolvable equal-address image overlap, address=0x%016" PRIx64
		     " requested=%ux%u "
		     "cached=%ux%u requested_size=0x%016" PRIx64 " cached_size=0x%016" PRIx64
		     " type=%u/%u tile=%u/%u\n",
		     requested.data.address, requested.resources.levels, requested.resources.layers,
		     cached.info.resources.levels, cached.info.resources.layers, requested.data.size,
		     cached.info.data.size, static_cast<uint32_t>(requested.type),
		     static_cast<uint32_t>(cached.info.type), static_cast<uint32_t>(requested.tile_mode),
		     static_cast<uint32_t>(cached.info.tile_mode));
	}

	const int32_t requested_mip = requested.MipOf(cached.info);
	if (requested_mip >= 0) {
		const int32_t layer = requested.SliceOf(cached.info, requested_mip);
		return {cached_id, requested_mip, layer};
	}

	const int32_t mip = cached.info.MipOf(requested);
	if (mip >= 0) {
		const int32_t layer = cached.info.SliceOf(requested, mip);
		if (!merged_id) {
			return {ExpandImage(requested, cached_id)};
		}
		cached.binding.needs_rebind |= cached.binding.is_bound || cached.binding.is_target;
		m_slot_images[merged_id].binding.is_target |= cached.binding.is_target;
		CopyImageMip(merged_id, cached_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
		FreeImage(cached_id);
		return {merged_id};
	}
	if (requested.data.address >= cached.info.data.address && safe_to_delete) {
		FreeImage(cached_id);
	}
	return {merged_id};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId source_id) {
	RefreshCopySource(source_id);
	const auto expanded_id = InsertImage(info);
	auto&      source      = m_slot_images[source_id];
	if (source.binding.is_bound || source.binding.is_target) {
		source.binding.needs_rebind = true;
	}
	InitializeImage(expanded_id);
	const int32_t mip = source.info.MipOf(info);
	const int32_t layer = source.info.SliceOf(info, mip);
	if (layer >= 0) {
		CopyImageMip(expanded_id, source_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
	} else {
		CopyImage(expanded_id, source_id);
	}
	FreeImage(source_id);
	return expanded_id;
}

struct TextureCache::TextureTransfer {
	TextureUploadLayout              layout;
	std::vector<vk::BufferImageCopy> regions;
	std::vector<GpuTileInfo>         tiles;
	bool                             swap_bgra16 = false;
	bool                             valid       = false;

	[[nodiscard]] uint64_t LinearSize() const {
		uint64_t size = 0;
		for (const auto& tile: tiles) {
			size = std::max(size, tile.linear_offset + tile.linear_size);
		}
		return size;
	}
};

struct TextureCache::ImageDownload {
	TextureTransfer texture;
	bool                depth_target = false;
	bool                valid        = false;
};

TextureCache::TextureTransfer
TextureCache::BuildTextureTransfer(const Image& image, BindingType binding,
                                    TransferDirection direction) const {
	const auto& info             = image.info;
	const bool  upload           = direction == TransferDirection::Upload;
	const bool  render_target    = binding == BindingType::RenderTarget;
	const bool  video_out        = binding == BindingType::VideoOut;
	uint32_t    layers           = info.TransferLayers();
	bool        volume           = info.IsVolume();
	const char* owner            = "TextureCache readback";

	TextureTransfer transfer;
	transfer.swap_bgra16 = info.bgra16;
	if (upload) {
		if ((render_target || video_out) &&
		    (info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
		     info.samples != 1 || image.backing.samples != 1)) {
			EXIT("TextureCache: invalid color-attachment upload\n");
		}
		owner = "TextureCache";
		if (render_target) {
			owner = "RenderTarget";
		} else if (binding == BindingType::Storage) {
			owner = "StorageTextureCache";
		} else if (video_out) {
			if (info.metadata.compression != VideoOutCompression::Uncompressed) {
				EXIT("TextureCache: invalid color-attachment upload\n");
			}
			layers = info.resources.layers;
			volume = false;
			owner  = "VideoOut";
		}
	}

	transfer.layout = TextureCalcUploadLayout(info.guest_format, info.extent.width,
	                                         info.extent.height, info.resources.levels, layers,
	                                         info.tile_mode, info.data.size, volume, owner);
	transfer.regions = TextureBuildImageCopies(transfer.layout);
	if (info.IsDepth()) {
		for (auto& region: transfer.regions) {
			region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eDepth;
		}
	}
	if (transfer.layout.surface.description.tile_mode != Prospero::TileMode::kLinear) {
		if (!TextureBuildGpuTileInfos(info.data.size, transfer.regions, transfer.layout,
		                              info.resources.levels, transfer.tiles)) {
			return transfer;
		}
	}
	transfer.valid = true;
	return transfer;
}

TextureCache::ImageDownload TextureCache::BuildDownload(const Image& image) const {
	const auto&  info    = image.info;
	const auto   binding = UploadBinding(image);
	ImageDownload transfer {.depth_target = binding == BindingType::DepthTarget};
	if (info.samples != 1 || image.backing.samples != 1) {
		return transfer;
	}
	if (transfer.depth_target) {
		transfer.valid = IsSupportedDepthPlaneReadback(info) && info.resources.layers != 0 &&
		             info.data.size % info.resources.layers == 0 &&
		             Prospero::NumBytesPerElement(info.guest_format) == info.bytes_per_block;
		return transfer;
	}
	if (info.metadata.compression != VideoOutCompression::Uncompressed) {
		return transfer;
	}
	transfer.texture = BuildTextureTransfer(image, binding, TransferDirection::Download);
	transfer.valid   = transfer.texture.valid;
	return transfer;
}

void TextureCache::ForgetDetileReuse(uint64_t address, uint64_t size) {
	if (!DetileReuseEnabled() || size == 0 || address > UINT64_MAX - size) {
		return;
	}
	const uint64_t end = address + size;
	for (auto& entry: m_detile_reuse) {
		if (entry.address != 0 && address < entry.address + entry.guest_size && entry.address < end) {
			entry.address = 0;
		}
	}
}

void TextureCache::SweepDetileReuse() {
	std::erase_if(m_detile_reuse, [this](DetileReuseEntry& entry) {
		if (entry.address != 0) {
			return false;
		}
		if (entry.linear_size <= m_detile_reuse_bytes) {
			m_detile_reuse_bytes -= entry.linear_size;
		} else {
			m_detile_reuse_bytes = 0;
		}
		m_scheduler.DeferOperation(
		    [buffer = std::move(entry.buffer)]() mutable { buffer.reset(); });
		return true;
	});
}

TileManager::Result TextureCache::FindDetileReuse(uint64_t address, uint64_t guest_size,
                                                  uint64_t fingerprint, uint64_t linear_size) {
	if (!DetileReuseEnabled()) {
		return {};
	}
	SweepDetileReuse();
	for (const auto& entry: m_detile_reuse) {
		if (entry.address == address && entry.guest_size == guest_size &&
		    entry.fingerprint == fingerprint && entry.linear_size == linear_size && entry.buffer) {
			return {entry.buffer->Handle(), 0, entry.linear_size};
		}
	}
	return {};
}

void TextureCache::RetainDetile(uint64_t address, uint64_t guest_size, uint64_t fingerprint,
                                TileManager::Result linear) {
	constexpr uint64_t kMinimumReuse = 8ull * 1024 * 1024;
	constexpr uint64_t kReuseBudget  = 160ull * 1024 * 1024;
	if (!DetileReuseEnabled() || address == 0 || linear.buffer == nullptr ||
	    linear.size < kMinimumReuse) {
		return;
	}
	SweepDetileReuse();
	if (linear.size > UINT64_MAX - m_detile_reuse_bytes ||
	    m_detile_reuse_bytes + linear.size > kReuseBudget) {
		return;
	}
	for (const auto& entry: m_detile_reuse) {
		if (entry.address == address && entry.guest_size == guest_size &&
		    entry.fingerprint == fingerprint && entry.linear_size == linear.size) {
			return;
		}
	}
	const uint64_t copy_size = Common::AlignUp(linear.size, 4);
	auto retained = std::make_unique<Buffer>(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
	    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst, copy_size);
	m_scheduler.EndRendering();
	auto command = m_scheduler.Current().Handle();
	vk::BufferMemoryBarrier barriers[2] {};
	barriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
	barriers[0].dstAccessMask = vk::AccessFlagBits::eTransferRead;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].buffer              = linear.buffer;
	barriers[0].offset              = linear.offset;
	barriers[0].size                = copy_size;
	barriers[1]                     = barriers[0];
	barriers[1].srcAccessMask       = {};
	barriers[1].dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
	barriers[1].buffer              = retained->Handle();
	barriers[1].offset              = 0;
	barriers[1].size                = copy_size;
	ProfilePipelineBarrier(command, vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 2, barriers, 0,
	                        nullptr);
	const vk::BufferCopy region {linear.offset, 0, copy_size};
	const auto copy_timer = m_scheduler.StartGpuTimer("copy_detile_retain", m_scheduler.GpuFrameHint(), copy_size);
	command.copyBuffer(linear.buffer, retained->Handle(), 1, &region);
	m_scheduler.EndGpuTimer(copy_timer);
	barriers[1].srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eShaderRead;
	ProfilePipelineBarrier(command, vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eAllCommands, {}, 0, nullptr, 1, &barriers[1],
	                        0, nullptr);
	m_detile_reuse_bytes += linear.size;
	m_detile_reuse.push_back(
	    {address, guest_size, fingerprint, linear.size, std::move(retained)});
}

void TextureCache::UploadImage(ImageId id, Image& image, Buffer& source, uint64_t source_offset) {
	ProfileDetailScope profile("cpu_image_upload", image.info.data.address, image.info.data.size);
	ProfileCpuScope transaction(m_scheduler, "upload_transaction", image.info.data.address, image.info.data.size, true);
	source.ProfileAllocation("upload_source_allocation");
	auto& destination = image.depth_id ? m_slot_images[image.depth_id] : image;
	const auto binding = image.depth_id ? BindingType::DepthTarget : UploadBinding(image);
	const auto  upload  = [&](std::vector<vk::BufferImageCopy>& copies, TileManager::Result linear) {
		for (auto& copy: copies) {
			copy.bufferOffset += linear.offset;
		}
		destination.Upload(copies, linear.buffer, linear.offset, linear.size);
	};

	if (binding != BindingType::DepthTarget) {
		const auto& info = image.info;
		auto transfer = BuildTextureTransfer(image, binding, TransferDirection::Upload);
		if (!transfer.valid) {
			EXIT("TextureCache: invalid texture upload: binding=%u addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " format=%u tile=%u family=%u extent=%ux%ux%u "
			     "pitch=%u levels=%u layers=%u samples=%u\n",
			     static_cast<uint32_t>(binding), info.data.address, info.data.size,
			     static_cast<uint32_t>(info.guest_format), static_cast<uint32_t>(info.tile_mode),
			     static_cast<uint32_t>(transfer.layout.surface.texture.block.family), info.extent.width,
			     info.extent.height, info.extent.depth, info.pitch, info.resources.levels,
			     info.resources.layers, info.samples);
		}
		TileManager::Result linear {source.Handle(), source_offset, info.data.size};
		if (!transfer.tiles.empty()) {
			const auto linear_size = transfer.LinearSize();
			const auto fingerprint = HashGpuTiles(transfer.tiles);
			const auto reused      = transfer.swap_bgra16
			                             ? TileManager::Result {}
			                             : FindDetileReuse(info.data.address, info.data.size,
			                                               fingerprint, linear_size);
			TraceDetileUpload(m_scheduler, id, image, binding, info.data.address, info.data.size,
			                  linear_size, transfer.tiles, reused.buffer != nullptr);
			if (reused.buffer != nullptr) {
				linear = reused;
			} else {
				linear = m_tiler.Detile(source.Handle(), source_offset, info.data.size, linear_size,
				                        transfer.tiles, info.data.address);
				if (!transfer.swap_bgra16) {
					RetainDetile(info.data.address, info.data.size, fingerprint, linear);
				}
			}
		}
		if (transfer.swap_bgra16) {
			linear = m_tiler.SwapBgra16(linear);
		}
		upload(transfer.regions, linear);
		return;
	}

	// The stencil plane has its own row pitch and shares the native image
	// with the depth plane.
	auto info = destination.info;
	if (image.depth_id) {
		info.data            = image.info.data;
		info.guest_format    = Prospero::BufferFormat::k8UInt;
		info.bytes_per_block = 1;
		if (info.IsTiled()) info.pitch = TileGetDepthPitch(info.extent.width, 1, 0);
	}
	if (info.samples != 1 || destination.backing.samples != 1 ||
	    info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
	    Prospero::NumBytesPerElement(info.guest_format) != info.bytes_per_block) {
		EXIT("TextureCache: invalid depth upload\n");
	}
	const auto          layers          = info.resources.layers;
	const auto          full_slice_size = info.data.size / layers;
	auto copies = BuildDepthCopies(info, full_slice_size, image.depth_id
	                                                        ? vk::ImageAspectFlagBits::eStencil
	                                                        : vk::ImageAspectFlagBits::eDepth);
	TileManager::Result linear {source.Handle(), source_offset, source.Size() - source_offset};
	const auto transfer_bytes = image.depth_id ? 1u : DepthAspectTransferBytes(info.pixel_format);
	if (info.IsTiled()) {
		const auto tiles = BuildDepthTiles(info);
		const bool plain = transfer_bytes == info.bytes_per_block;
		const auto fingerprint = HashGpuTiles(tiles);
		const auto reused =
		    plain ? FindDetileReuse(info.data.address, info.data.size, fingerprint, info.data.size)
		          : TileManager::Result {};
		TraceDetileUpload(m_scheduler, id, image, binding, info.data.address, info.data.size,
		                  info.data.size, tiles, reused.buffer != nullptr);
		if (reused.buffer != nullptr) {
			linear = reused;
		} else {
			linear = m_tiler.Detile(source.Handle(), source_offset, info.data.size, info.data.size,
			                        tiles, info.data.address);
			if (plain) {
				RetainDetile(info.data.address, info.data.size, fingerprint, linear);
			}
		}
	}
	if (transfer_bytes != info.bytes_per_block) {
		const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
		EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
		                     transfer_bytes != sizeof(uint32_t) || texels_per_slice > UINT32_MAX ||
		                     texels_per_slice > UINT64_MAX / transfer_bytes);
		const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
		EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
		auto promoted = m_tiler.GetScratchBuffer(transfer_slice * layers, linear.buffer);
		m_tiler.ConvertD16(
		    linear, promoted, TileManager::D16Direction::Promote,
		    info.pixel_format == vk::Format::eD32SfloatS8Uint,
		    {.width               = info.extent.width,
		     .height              = info.extent.height,
		     .layers              = layers,
		     .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
		     .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
		     .source_slice_stride = full_slice_size,
		     .target_slice_stride = transfer_slice});
		linear = promoted;
		for (uint32_t layer = 0; layer < layers; layer++) {
			copies[layer].bufferOffset = transfer_slice * layer;
		}
	}
	upload(copies, linear);
}

void TextureCache::InitializeImage(ImageId id) {
	ProfileDetailScope profile("cpu_image_initialize");
	auto& image = m_slot_images[id];
	if (image.info.data.Empty()) {
		return;
	}
	TrackImage(id);
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (image.IsCpuDirty()) {
			image.RefreshComplete();
		}
		return;
	}
	if (image.info.samples > 1) {
		return;
	}
	const bool upload = image.IsBufferModified() || image.IsCpuDirty();
	if (upload) {
		const auto [source, source_offset] =
		    m_buffer_cache.ObtainBufferForImage(image.info.data.address, image.info.data.size);
		if (source == nullptr) {
			EXIT("TextureCache: failed to obtain image upload source\n");
		}
		UploadImage(id, image, *source, source_offset);
		image.ClearBufferModified();
	}
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
}

void TextureCache::TraceColorMetaFill(uint64_t address, uint64_t size, uint32_t value,
                                     bool gpu_path) {
	auto& trace = GetColorMetaTrace();
	if (trace.file != nullptr) {
		std::fprintf(trace.file, "F,%" PRIu64 ",0x%016" PRIx64 ",%" PRIu64
		                         ",0x%08" PRIx32 ",%s\n",
		             static_cast<uint64_t>(m_scheduler.Context().DiagnosticFrameNum()), address,
		             size, value,
		             gpu_path ? "gpu" : "cpu");
	}
}

void TextureCache::TraceColorMetaWrite(uint64_t address, uint64_t size,
                                      const char* source, uint64_t source_address) {
	auto& trace = GetColorMetaTrace();
	if (trace.file == nullptr ||
	    std::none_of(trace.ranges.begin(), trace.ranges.end(), [address, size](GuestRange range) {
		    return address < range.address + range.size && range.address < address + size;
	    })) {
		return;
	}
	std::fprintf(trace.file, "W,%" PRIu64 ",0x%016" PRIx64 ",%" PRIu64
	                         ",0x%016" PRIx64 ",%s\n",
	             static_cast<uint64_t>(m_scheduler.Context().DiagnosticFrameNum()), address,
	             size, source_address, source);
}

bool TextureCache::TryConsumeColorMetaUniformFill(uint64_t address, uint64_t size,
                                                  uint32_t value) {
	static bool last_enabled = false;
	const bool enabled = ColorMetaCpuFillEnabled();
	if (enabled != last_enabled) {
		auto& trace = GetColorMetaTrace();
		if (trace.file != nullptr) {
			std::fprintf(trace.file, "T,%" PRIu64 ",0x0000000000000000,0,%u,%s\n",
			             static_cast<uint64_t>(m_scheduler.Context().DiagnosticFrameNum()),
			             static_cast<unsigned>(enabled), enabled ? "on" : "off");
			std::fflush(trace.file);
		}
		last_enabled = enabled;
	}
	if (!enabled || (address & 0xfff) != 0 || (size & 0xfff) != 0) {
		return false;
	}
	bool known_cmask = false;
	{
		std::scoped_lock lock {m_lock};
		m_slot_images.ForEach([&](ImageId, const Image& image) {
			const auto& metadata = image.info.metadata;
			known_cmask |= image.registered && metadata.kind == ImageMetadataKind::Cmask &&
			               metadata.range.address == address && metadata.range.size == size &&
			               image.info.resources.levels == 1;
		});
	}
	if (!known_cmask) {
		return false;
	}
	m_buffer_cache.FillBufferFromProvenComputeFill(address, size, value);
	auto& trace = GetColorMetaTrace();
	if (trace.file != nullptr) {
		std::fprintf(trace.file, "P,%" PRIu64 ",0x%016" PRIx64 ",%" PRIu64
		                         ",0x%08" PRIx32 ",cpu-proven-fill\n",
		             static_cast<uint64_t>(m_scheduler.Context().DiagnosticFrameNum()), address,
		             size, value);
		std::fflush(trace.file);
	}
	return true;
}

void TextureCache::MaterializeColorClear(ImageId id, const ImageDesc& desc,
                                       uint32_t metadata_base_layer) {
	if (desc.info.metadata.kind != ImageMetadataKind::Dcc &&
	    desc.info.metadata.kind != ImageMetadataKind::Cmask) {
		return;
	}
	const auto range = desc.info.metadata.range;
	ProfileDetailScope metadata_profile("cpu_materialize_color_clear", range.address, range.size);
	m_scheduler.ProfileBufferUse(desc.info.metadata.kind == ImageMetadataKind::Dcc ? "role_color_dcc" : "role_color_cmask",
	    range.address, range.size, 0);
	ColorMetaPrecheckScope precheck {.profile = GetColorMetaPrecheckProfile()};
	const bool             skip_impossible = CmaskSkipImpossibleEnabled();
	if (precheck.profile != nullptr || skip_impossible) {
		const bool cmask = desc.info.metadata.kind == ImageMetadataKind::Cmask;
		precheck.key = {static_cast<uint32_t>(desc.info.metadata.kind), range.address, range.size};
		precheck.sample.calls      = 1;
		precheck.sample.view_format = static_cast<uint32_t>(desc.view_info.format);
		precheck.sample.clear_word  = desc.info.metadata.clear_word;
		precheck.sample.skip_cpu_faults = m_buffer_cache.TakeSkippedReadbackFaults();
		if (cmask) {
			vk::ClearColorValue probe {};
			if (desc.type != BindingType::RenderTarget) {
				precheck.sample.not_render_target = 1;
			} else if (!desc.info.metadata.clear_register_valid) {
				precheck.sample.no_clear_register = 1;
			} else if (!DecodePackedColorClear(desc.view_info.format, desc.info.metadata.clear_word,
			                                   probe)) {
				precheck.sample.undecodable_clear = 1;
			}
			precheck.sample.impossible = precheck.sample.not_render_target |
			                             precheck.sample.no_clear_register |
			                             precheck.sample.undecodable_clear;
		}
	}
	{
		std::scoped_lock lock {m_lock};
		auto& image         = m_slot_images[id];
		image.info.metadata = desc.info.metadata;
		// Native color metadata must not retain a reused HTile/CMask/FMask clear flag.
		m_surface_metas.erase(range.address);
		if (range.size == 0 || desc.info.resources.levels != 1) {
			precheck.sample.early_return = 1;
			return;
		}
	}
	const auto layers = desc.info.TransferLayers();
	// These one-mip surfaces use complete 4 KiB color metadata blocks.
	constexpr uint64_t MetadataBlockSize = 0x1000;
	if (!range.Valid() || range.address % MetadataBlockSize != 0 || layers == 0 ||
	    range.size % layers != 0 || (range.size / layers) % MetadataBlockSize != 0) {
		EXIT("TextureCache: color metadata slices must contain aligned 4 KiB blocks\n");
	}
	const auto& view           = desc.view_info;
	const bool  volume_texture = desc.info.IsVolume() && view.type == vk::ImageViewType::e3D;
	const auto  first          = volume_texture ? 0u : metadata_base_layer;
	const auto  image_first    = volume_texture ? 0u : view.base_layer;
	const auto  count          = volume_texture ? desc.info.extent.depth : view.layer_count;
	if (first >= layers || count > layers - first) {
		EXIT("TextureCache: color view exceeds its native metadata slices\n");
	}
	auto& trace = GetColorMetaTrace();
	if (trace.file != nullptr &&
	    std::none_of(trace.ranges.begin(), trace.ranges.end(), [range](GuestRange known) {
		    return known.address == range.address && known.size == range.size;
	    })) {
		trace.ranges.push_back(range);
	}
	auto& profile = GetColorMetaProfile();
	if (profile.file != nullptr) {
		profile.NextFrame(m_scheduler.Context().DiagnosticFrameNum());
		profile.calls++;
		profile.ranges.insert(range.address);
	}
	// Finish native metadata writes before reading backing bytes. This can submit the scheduler,
	// so discovery runs before final draw uploads and never holds the texture lock across it.
	// A CMask slice clears only from code 0 with a decodable render-target clear register, so
	// when that fails no downloaded byte can change emulator state; the range stays
	// GPU-modified and guest CPU reads still fault and download it.
	if (skip_impossible && precheck.sample.impossible != 0 &&
	    m_buffer_cache.IsRegionGpuModified(range.address, range.size)) {
		m_buffer_cache.WatchSkippedReadback(range.address, range.size);
		precheck.sample.dirty            = 1;
		precheck.sample.dirty_impossible = 1;
		precheck.sample.skipped          = 1;
	} else if (m_buffer_cache.IsRegionGpuModified(range.address, range.size)) {
		const auto before = std::chrono::steady_clock::now();
		m_buffer_cache.ReadMemory(range.address, range.size, false);
		const auto elapsed_ms =
		    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before)
		        .count();
		if (profile.file != nullptr) {
			profile.dirty++;
			profile.requested_bytes += range.size;
			profile.readback_ms += elapsed_ms;
		}
		precheck.sample.dirty = 1;
		precheck.sample.dirty_impossible = precheck.sample.impossible;
		precheck.sample.readback_ms = elapsed_ms;
		precheck.sample.impossible_readback_ms = precheck.sample.impossible != 0 ? elapsed_ms : 0.0;
		if (trace.file != nullptr) {
			std::fprintf(trace.file, "R,%" PRIu64 ",0x%016" PRIx64 ",%" PRIu64
			                         ",%u,%.3f\n",
			             static_cast<uint64_t>(m_scheduler.Context().DiagnosticFrameNum()),
			             range.address, range.size,
			             static_cast<unsigned>(desc.info.metadata.kind), elapsed_ms);
			std::fflush(trace.file);
		}
	}
	const auto slice_size = range.size / layers;
	for (uint32_t slice = 0; slice < count; slice++) {
		const auto address = range.address + slice_size * (first + slice);
		uint8_t code = 0;
		if (!LibKernel::Memory::TryReadBacking(address, &code, sizeof(code))) {
			EXIT("TextureCache: failed to read color metadata backing\n");
		}
		m_scheduler.ProfileBufferUse("cpu_consume_metadata_code", address, sizeof(code), code);
		vk::ClearValue clear {};
		if (!DecodeColorClear(desc, code, clear.color)) {
			continue;
		}
		if (profile.file != nullptr) {
			profile.decoded_slices++;
		}
		precheck.sample.decoded_slices++;
		precheck.sample.impossible_clears += precheck.sample.impossible;
		ProfileDetailScope scan_profile("cpu_metadata_uniform_scan", address, slice_size);
		std::vector<uint8_t> bytes(slice_size);
		if (!LibKernel::Memory::TryReadBacking(address, bytes.data(), bytes.size())) {
			EXIT("TextureCache: failed to read color metadata slice\n");
		}
		if (!std::all_of(bytes.begin(), bytes.end(), [code](uint8_t byte) { return byte == code; })) {
			if (profile.file != nullptr) {
				profile.nonuniform_slices++;
			}
			continue;
		}
		scan_profile.Finish();
		if (profile.file != nullptr) {
			profile.uniform_clears++;
		}
		precheck.sample.uniform_clears++;
		{
			std::scoped_lock lock {m_lock};
			ClearImage(m_scheduler.Current(), id, view.format,
			           {vk::ImageAspectFlagBits::eColor, view.base_level, view.level_count,
			            image_first + slice, 1}, clear);
		}
		// Publish the conversion's expanded keys without treating them as guest writes
		// to overlapping image data. Invalidate the buffer before updating its backing.
		if (desc.type != BindingType::VideoOut) {
			std::fill(bytes.begin(), bytes.end(), uint8_t {0xff});
			m_buffer_cache.InvalidateMemory(address, slice_size);
			LibKernel::Memory::WriteBacking(address, bytes.data(), bytes.size());
			m_scheduler.ProfileBufferUse("cpu_writer_metadata_expand", address, bytes.size(), 0);
		}
	}
}

void TextureCache::RefreshImage(ImageId id) {
	ProfileDetailScope profile("cpu_image_refresh");
	auto& image = m_slot_images[id];
	if (image.depth_id &&
	    (m_slot_images[image.depth_id].info.metadata.stencil_compressed ||
	     m_slot_images[image.depth_id].info.samples != 1)) {
		return;
	}
	TrackImage(id);
	if (image.IsMaybeCpuDirty()) {
		const auto hash = image.HashGuestEdges();
		if (image.NeedsMaybeCpuHash()) {
			image.SetMaybeCpuHash(hash);
			return;
		}
		(void)image.ResolveMaybeCpuHash(hash);
	}
	bool cpu_dirty = image.IsBufferModified() || image.IsDefinitelyCpuDirty();
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (cpu_dirty) {
			EXIT("TextureCache: compressed guest image refresh is unsupported\n");
		}
		return;
	}
	if (!cpu_dirty) {
		return;
	}
	InitializeImage(id);
}

ImageId TextureCache::AssociateStencil(ImageId depth_id, GuestRange stencil) {
	if (!stencil.Valid()) {
		EXIT("TextureCache: invalid stencil association range\n");
	}
	auto& depth = m_slot_images[depth_id];
	if (!depth.info.IsDepth() || !depth.info.HasStencil()) {
		EXIT("TextureCache: stencil association requires a depth/stencil image\n");
	}

	ImageId association {};
	for (const auto id: FindImagesInRegion(stencil.address, stencil.size, false)) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->info.data == stencil &&
		    owner->info.extent == depth.info.extent) {
			association = id;
		}
	}
	if (!association) {
		ImageInfo info {};
		info.data   = stencil;
		info.extent = depth.info.extent;
		association = InsertImage(info);
	}
	auto& record = m_slot_images[association];
	TouchImage(record);
	record.depth_id = depth_id;
	return association;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_format) {
	ProfileDetailScope profile("cpu_image_find", desc.info.data.address, desc.info.data.size);
	auto& command = m_scheduler.Current();
	if (command.IsInvalid()) {
		EXIT("TextureCache: image lookup requires a valid command buffer\n");
	}
	ValidateImageDesc(desc);
	if (desc.info.data.Empty()) {
		std::scoped_lock lock {m_lock};
		return GetNullImage(desc);
	}
	// Test hack: sampled images stay 1x1 so freed textures are not allocated again.
	// Render targets, depth and video-out still allocate, so a frame can present.
	if (DropTexturesEnabled() &&
	    (desc.type == BindingType::Texture || desc.type == BindingType::Storage)) {
		std::scoped_lock lock {m_lock};
		desc.view_info.type        = vk::ImageViewType::e2D;
		desc.view_info.base_level  = 0;
		desc.view_info.level_count = 1;
		desc.view_info.base_layer  = 0;
		desc.view_info.layer_count = 1;
		return GetNullImage(desc);
	}
	const auto metadata_base_layer = desc.view_info.base_layer;

	ImageId result {};
	{
		ProfileDetailScope lock_profile("cpu_image_lock_acquire", desc.info.data.address);
		std::scoped_lock lock {m_lock};
		lock_profile.Finish();
		const auto       candidates =
		    FindImagesInRegion(desc.info.data.address, desc.info.data.size, false);

		for (const auto id: candidates) {
			const auto& image = m_slot_images[id];
			if (SameBacking(image.info, desc.info, exact_format)) {
				result = id;
			}
		}

		int32_t view_mip   = -1;
		int32_t view_layer = -1;
		if (!result) {
			for (const auto candidate: candidates) {
				view_mip                = -1;
				view_layer              = -1;
				const auto& merged_info = result ? m_slot_images[result].info : desc.info;
				const auto  overlap     = ResolveOverlap(merged_info, desc.type, candidate, result);
				if (overlap.image) {
					result     = overlap.image;
					view_mip   = overlap.mip;
					view_layer = overlap.layer;
				}
			}
		}

		if (result) {
			auto& resolved = m_slot_images[result];
			if (exact_format && resolved.info.pixel_format != desc.info.pixel_format) {
				result = {};
			} else if (resolved.info.resources < desc.info.resources) {
				FreeImage(result);
				result = {};
			}
		}
		if (!result) {
			ProfileDetailEvent("cache_image_miss", desc.info.data.address, desc.info.data.size);
			vk::ImageUsageFlags pending {};
			if (TightImageUsageEnabled()) {
				if (desc.type == BindingType::Storage) {
					pending = vk::ImageUsageFlagBits::eStorage;
				} else if (desc.type == BindingType::RenderTarget ||
				           desc.type == BindingType::VideoOut) {
					pending = vk::ImageUsageFlagBits::eColorAttachment;
				}
			}
			SetPendingTightUsage(pending);
			result = InsertImage(desc.info);
			SetPendingTightUsage({});
			auto& inserted = m_slot_images[result];
			if (m_buffer_cache.HasGpuDirtyBytes(inserted.info.data.address,
			                                    inserted.info.data.size)) {
				inserted.MarkBufferModified();
			}
		} else ProfileDetailEvent("cache_image_hit", desc.info.data.address, desc.info.data.size);
		auto& image = m_slot_images[result];
		if (view_mip >= 0) {
			desc.view_info.base_level = static_cast<uint32_t>(view_mip);
		}
		if (view_layer >= 0) {
			desc.view_info.base_layer = static_cast<uint32_t>(view_layer);
		}
		image.tick_accessed_last = m_scheduler.CurrentTick();
		TouchImage(image);
	}
	MaterializeColorClear(result, desc, metadata_base_layer);
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression != VideoOutCompression::Uncompressed) {
		std::scoped_lock lock {m_lock};
		const auto& image = m_slot_images[result];
		const bool guest_dirty = image.IsBufferModified() || image.IsCpuDirty();
		const bool native_current =
		    (image.usage.render_target || image.IsGpuModified()) && !guest_dirty;
		if (!native_current) {
			EXIT("TextureCache: compressed video-out read requires clean native GPU "
			     "contents\n");
		}
	}
	return result;
}

void TextureCache::UpdateImage(ImageId id) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	TouchImage(image);
	RefreshImage(id);
}

ImageId TextureCache::FindImageFromRange(uint64_t address, uint64_t size, bool ensure_valid) {
	if (!GuestRange {address, size}.Valid()) {
		return {};
	}
	std::scoped_lock lock {m_lock};
	ImageIds         matches;
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr || owner->info.data.address != address) {
			continue;
		}
		if (ensure_valid && owner->depth_id) {
			owner = m_slot_images.try_get(owner->depth_id);
		}
		if (owner == nullptr || (ensure_valid && !owner->SafeToDownload())) {
			continue;
		}
		matches.push_back(id);
	}
	ImageId selected {};
	if (matches.size() == 1) {
		selected = matches.front();
	} else {
		for (const auto id: matches) {
			const auto& image = m_slot_images[id];
			if (image.info.data.size == size) {
				selected = id;
				break;
			}
		}
	}
	if (selected && ensure_valid) {
		const auto owner = m_slot_images.try_get(selected);
		if (owner != nullptr && owner->depth_id) {
			selected = owner->depth_id;
		}
	}
	return selected;
}

vk::ImageView TextureCache::FindTexture(ImageId id, const ImageDesc& desc) {
	ProfileDetailScope profile("cpu_image_view_find", desc.info.data.address, desc.info.data.size);
	ProfileDetailScope lock_profile("cpu_image_lock_acquire", desc.info.data.address);
	std::scoped_lock lock {m_lock};
	lock_profile.Finish();
	auto&            image = m_slot_images[id];
	TouchImage(image);
	if (!image.info.data.Empty()) {
		if (!image.registered || image.depth_id || image.binding.needs_rebind) {
			EXIT("TextureCache: texture requires rediscovery before final acquisition\n");
		}
	}
	if (desc.type == BindingType::Storage) {
		image.MarkGpuModified();
		if (TightImageUsageEnabled()) {
			image.EnsureUsage(vk::ImageUsageFlagBits::eStorage);
		}
	}
	if (!image.info.data.Empty()) {
		RefreshImage(id);
		if (image.info.HasStencil() &&
		    desc.info.data.address >= image.info.stencil.address &&
		    desc.info.data.End() <= image.info.stencil.End()) {
			for (const auto stencil_id:
			     FindImagesInRegion(image.info.stencil.address, image.info.stencil.size, false)) {
				const auto* stencil = m_slot_images.try_get(stencil_id);
				if (stencil != nullptr && stencil->depth_id == id &&
				    stencil->info.data == image.info.stencil) {
					RefreshImage(stencil_id);
					break;
				}
			}
		}
	}
	switch (desc.type) {
		case BindingType::Texture: break;
		case BindingType::Storage:
			if (!image.info.data.Empty()) {
				CommitGpuWrite(image);
			}
			TrackImageDownload(id, image);
			break;
		default: EXIT("TextureCache: invalid texture binding\n");
	}
	m_scheduler.ProfileImageBinding(image.backing.image);
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindRenderTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::RenderTarget) {
		EXIT("TextureCache: invalid color-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: color target requires rediscovery before final acquisition\n");
	}
	TouchImage(image);
	image.MarkGpuModified();
	image.usage.render_target = true;
	image.last_write = {ImageWriteKind::RenderTarget, 0, NextImageWriteSerial()};
	if (TightImageUsageEnabled()) {
		image.EnsureUsage(vk::ImageUsageFlagBits::eColorAttachment);
	}
	RefreshImage(id);
	CommitGpuWrite(image);
	TrackImageDownload(id, image);
	m_scheduler.ProfileImageBinding(image.backing.image);
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindDepthTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::DepthTarget) {
		EXIT("TextureCache: invalid depth-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: depth target requires rediscovery before final acquisition\n");
	}
	TouchImage(image);
	image.MarkGpuModified();
	image.usage.depth_target = true;
	image.info.stencil = desc.info.stencil;
	image.info.metadata = desc.info.metadata;
	if (desc.info.HasMetadata()) {
		m_surface_metas.emplace(desc.info.metadata.range.address,
		                        MetaDataInfo {.type       = MetaDataInfo::Type::HTile,
		                                      .clear_mask = image.info.htile_clear_mask});
	}
	RefreshImage(id);
	CommitGpuWrite(image);
	if (desc.info.HasStencil()) {
		RefreshImage(AssociateStencil(id, desc.info.stencil));
	}
	m_scheduler.ProfileImageBinding(image.backing.image);
	return image.FindView(desc.view_info);
}

void TextureCache::MarkGpuWritten(ImageId id) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id) {
		EXIT("TextureCache: cannot mark an unavailable image GPU-written\n");
	}
	TrackImage(id);
	CommitGpuWrite(image);
	if (image.info.HasStencil()) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
}

void TextureCache::CommitGpuWrite(Image& image) {
	if (!image.depth_id && image.backing.image == nullptr) {
		EXIT("TextureCache: GPU writes require a native image or stencil association\n");
	}
	image.ClearBufferModified();
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
	image.MarkGpuModified();
}

bool TextureCache::ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
                                        uint32_t packed_clear) {
	if (command.IsInvalid() || !GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid image clear\n");
	}
	std::scoped_lock     lock {m_lock};
	ImageId              selected {};
	vk::ImageAspectFlags aspect {};
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		vk::ImageAspectFlags candidate {};
		ImageId              candidate_id = id;
		if (owner->depth_id && owner->info.data.address == address &&
		    owner->info.data.size == size) {
			candidate    = vk::ImageAspectFlagBits::eStencil;
			candidate_id = owner->depth_id;
			owner        = m_slot_images.try_get(candidate_id);
			if (owner == nullptr || owner->backing.image == nullptr || !owner->info.HasStencil()) {
				continue;
			}
		} else if (!owner->depth_id && owner->info.data.address == address &&
		           owner->info.data.size == size) {
			candidate = owner->info.IsDepth() ? vk::ImageAspectFlagBits::eDepth
			                                  : vk::ImageAspectFlagBits::eColor;
		}
		if (!candidate) {
			continue;
		}
		if (selected && selected != candidate_id) {
			return false;
		}
		selected = candidate_id;
		aspect   = candidate;
	}
	if (!selected) {
		return false;
	}
	auto&          image = m_slot_images[selected];
	vk::ClearValue clear {};
	if (aspect == vk::ImageAspectFlagBits::eColor) {
		if (!DecodeColorDwordFill(image.info.pixel_format, packed_clear, clear.color)) {
			return false;
		}
	} else {
		uint8_t stencil_clear = 0;
		if ((aspect == vk::ImageAspectFlagBits::eDepth &&
		     !DecodePackedDepthClear(image.info.pixel_format, packed_clear, clear.depthStencil.depth)) ||
		    (aspect == vk::ImageAspectFlagBits::eStencil &&
		     !DecodePackedStencilClear(packed_clear, stencil_clear))) {
			return false;
		}
		clear.depthStencil.stencil = stencil_clear;
	}
	ClearImage(command, selected, image.backing.format,
	           {aspect, 0, image.info.resources.levels, 0, image.info.TransferLayers()}, clear);
	return true;
}

void TextureCache::ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
                              const vk::ImageSubresourceRange& range, const vk::ClearValue& clear) {
	auto& image = m_slot_images[id];
	const auto aspects = image.info.IsDepth() ? ImageViewOps::DepthAspectMask(image.backing.format)
	                                          : vk::ImageAspectFlagBits::eColor;
	EXIT_IF(range.baseMipLevel >= image.info.resources.levels);
	const auto layers = image.info.IsVolume()
	                        ? std::max(image.info.extent.depth >> range.baseMipLevel, 1u)
	                        : image.backing.layers;
	EXIT_IF(command.IsInvalid() || image.depth_id || !range.aspectMask || range.levelCount == 0 ||
	        range.levelCount > image.info.resources.levels - range.baseMipLevel ||
	        range.layerCount == 0 || range.baseArrayLayer >= layers ||
	        range.layerCount > layers - range.baseArrayLayer ||
	        (range.aspectMask & aspects) != range.aspectMask);
	const bool full_subresources = range.baseMipLevel == 0 &&
	                               range.levelCount == image.info.resources.levels &&
	                               range.baseArrayLayer == 0 && range.layerCount == layers;
	const bool full_image = range.aspectMask == aspects && full_subresources;
	TrackImage(id);
	if (!full_image && (image.IsBufferModified() || image.IsCpuDirty())) {
		InitializeImage(id);
		if (image.info.samples == 1 && (image.IsBufferModified() || image.IsCpuDirty())) {
			EXIT("TextureCache: image clear retained guest ownership\n");
		}
	}
	if (image.info.HasStencil() && (range.aspectMask & vk::ImageAspectFlagBits::eStencil)) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		if (!full_subresources) {
			RefreshImage(stencil_id);
		} else {
			TrackImage(stencil_id);
			CommitGpuWrite(m_slot_images[stencil_id]);
		}
	}
	command.EndRendering();
	// Transfer clears use the backing format; aliased clears must encode through their view.
	if (format != image.backing.format || (image.info.IsVolume() && !full_image)) {
		EXIT_NOT_IMPLEMENTED(range.aspectMask != vk::ImageAspectFlagBits::eColor ||
		                     range.levelCount != 1);
		ImageViewInfo view {};
		view.format = format;
		view.type   = range.layerCount == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
		view.base_level  = range.baseMipLevel;
		view.base_layer  = range.baseArrayLayer;
		view.layer_count = range.layerCount;
		view.usage       = vk::ImageUsageFlagBits::eColorAttachment;
		image.Transit(vk::ImageLayout::eColorAttachmentOptimal,
		              vk::AccessFlagBits2::eColorAttachmentWrite, {}, command.Handle());
		vk::RenderingAttachmentInfo attachment {};
		attachment.imageView   = image.FindView(view);
		attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		attachment.loadOp      = vk::AttachmentLoadOp::eClear;
		attachment.storeOp     = vk::AttachmentStoreOp::eStore;
		attachment.clearValue  = clear;
		vk::RenderingInfo rendering {};
		rendering.renderArea.extent = {
		    std::max(image.info.extent.width >> range.baseMipLevel, 1u),
		    std::max(image.info.extent.height >> range.baseMipLevel, 1u)};
		rendering.layerCount           = range.layerCount;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments    = &attachment;
		command.Handle().beginRendering(&rendering);
		command.Handle().endRendering();
		CommitGpuWrite(image);
		return;
	}
	image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {},
	              command.Handle());
	auto native_range = range;
	if (image.info.IsVolume()) {
		native_range.baseArrayLayer = 0;
		native_range.layerCount     = 1;
	}
	if (range.aspectMask == vk::ImageAspectFlagBits::eColor) {
		command.Handle().clearColorImage(image.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                                 &clear.color, 1, &native_range);
	} else {
		command.Handle().clearDepthStencilImage(image.backing.image,
		                                        vk::ImageLayout::eTransferDstOptimal,
		                                        &clear.depthStencil, 1, &native_range);
	}
	CommitGpuWrite(image);
}

void TextureCache::InvalidateMemory(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid memory-invalidation range\n");
	}
	std::scoped_lock lock {m_lock};
	ForgetDetileReuse(address, size);
	InvalidateCpuAliases(address, size);
}

void TextureCache::DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset) {
	const auto&    info             = image.info;
	const auto     layers           = info.resources.layers;
	const auto     full_slice_size  = info.data.size / layers;
	const auto     transfer_bytes   = DepthAspectTransferBytes(info.pixel_format);
	const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
	EXIT_NOT_IMPLEMENTED(transfer_bytes == 0 || texels_per_slice > UINT32_MAX ||
	                     texels_per_slice > UINT64_MAX / transfer_bytes ||
	                     texels_per_slice > UINT64_MAX / info.bytes_per_block);
	const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
	const uint64_t guest_slice    = texels_per_slice * info.bytes_per_block;
	EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
	const uint64_t transfer_size = transfer_slice * layers;
	EXIT_NOT_IMPLEMENTED(guest_slice > full_slice_size);
	auto copies = BuildDepthCopies(info, full_slice_size, vk::ImageAspectFlagBits::eDepth);
	if (transfer_bytes == info.bytes_per_block) {
		if (!info.IsTiled()) {
			for (auto& copy: copies) {
				copy.bufferOffset += destination_offset;
			}
			image.Download(copies, destination.Handle(), destination_offset, info.data.size);
			return;
		}
		const auto tiles = BuildDepthTiles(info);
		m_tiler.TileImage(image, copies, destination.Handle(), destination_offset, info.data.size,
		                  info.data.size, tiles);
		return;
	}
	EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
	                     transfer_bytes != sizeof(uint32_t));
	for (uint32_t layer = 0; layer < layers; layer++) {
		copies[layer].bufferOffset = transfer_slice * layer;
	}
	auto host_linear = m_tiler.GetScratchBuffer(transfer_size);
	image.Download(copies, host_linear.buffer, 0, host_linear.size);
	const bool tiled        = info.IsTiled();
	auto       guest_linear = tiled ? m_tiler.GetScratchBuffer(info.data.size, host_linear.buffer)
	                                : TileManager::Result {destination.Handle(), destination_offset,
	                                                       destination.Size() - destination_offset};
	m_tiler.ConvertD16(host_linear, guest_linear, TileManager::D16Direction::Demote,
	                   DepthAspectTransferFormat(info.pixel_format) == vk::Format::eD32Sfloat,
	                   {.width               = info.extent.width,
	                    .height              = info.extent.height,
	                    .layers              = layers,
	                    .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
	                    .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
	                    .source_slice_stride = transfer_slice,
	                    .target_slice_stride = full_slice_size});
	if (!tiled) {
		return;
	}
	const auto tiles = BuildDepthTiles(info);
	m_tiler.Tile(guest_linear.buffer, guest_linear.offset, info.data.size, destination.Handle(),
	             destination_offset, info.data.size, tiles);
}

void TextureCache::DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
                                     uint64_t destination_size, ImageDownload transfer) {
	if (!transfer.valid) {
		EXIT("TextureCache: invalid image download transfer\n");
	}
	if (transfer.depth_target) {
		if (destination_size != image.info.data.size) {
			EXIT("TextureCache: partial depth image download is unsupported\n");
		}
		DownloadDepth(image, destination, destination_offset);
		m_scheduler.ProfileBufferUse("gpu_writer_image_download_conservative",
		    destination.CpuAddress() + destination_offset, destination_size, std::bit_cast<uint64_t>(destination.Handle()));
		return;
	}

	auto&      texture   = transfer.texture;
	const auto transform = texture.swap_bgra16 ? TileManager::ColorTransform::SwapBgra16
	                                           : TileManager::ColorTransform::None;
	if (texture.tiles.empty()) {
		if (transform == TileManager::ColorTransform::SwapBgra16) {
			auto linear = m_tiler.GetScratchBuffer(destination_size);
			image.Download(texture.regions, linear.buffer, 0, linear.size);
			m_tiler.SwapBgra16(linear,
			                   {destination.Handle(), destination_offset, destination_size});
			m_scheduler.ProfileBufferUse("gpu_writer_image_download_conservative",
			    destination.CpuAddress() + destination_offset, destination_size, std::bit_cast<uint64_t>(destination.Handle()));
			return;
		}
		for (auto& copy: texture.regions) {
			copy.bufferOffset += destination_offset;
		}
		image.Download(texture.regions, destination.Handle(), destination_offset, destination_size);
		m_scheduler.ProfileBufferUse("gpu_writer_image_download_conservative",
		    destination.CpuAddress() + destination_offset, destination_size, std::bit_cast<uint64_t>(destination.Handle()));
		return;
	}

	m_tiler.TileImage(image, texture.regions, destination.Handle(), destination_offset,
	                  destination_size, texture.LinearSize(), texture.tiles, transform);
	m_scheduler.ProfileBufferUse("gpu_writer_image_download_conservative",
	    destination.CpuAddress() + destination_offset, destination_size, std::bit_cast<uint64_t>(destination.Handle()));
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	const auto selected = m_texture_cache.FindImageFromRange(vaddr, size);
	if (!selected) {
		return false;
	}

	std::scoped_lock lock {m_texture_cache.m_lock};
	auto& image = m_texture_cache.m_slot_images[selected];
	// The GPU thread owns image retirement; CPU invalidation can dirty this image after lookup.
	if (!image.SafeToDownload()) {
		return false;
	}
	if (!buffer.IsInBounds(image.info.data.address, 1)) {
		return false;
	}
	const auto buf_offset = buffer.Offset(image.info.data.address);
	const auto available  = buffer.Size() - buf_offset;
	uint32_t   levels     = 0;
	uint64_t   copy_size  = 0;
	if (image.info.IsVolume()) {
		// Volume mips contain strided block slices, so a mip's linear span cannot prove that
		// every retained slice fits. Keep volume synchronization whole-image only.
		if (!buffer.IsInBounds(image.info.data.address, image.info.data.size)) {
			return false;
		}
		levels    = image.info.resources.levels;
		copy_size = image.info.data.size;
	} else {
		for (; levels < image.info.resources.levels; ++levels) {
			const auto& mip = image.info.mip_layout[levels];
			if (mip.size == 0 || mip.offset > available || mip.size > available - mip.offset) {
				break;
			}
			copy_size = std::max(copy_size, mip.offset + mip.size);
		}
	}
	if (copy_size == 0) {
		return false;
	}
	auto transfer = m_texture_cache.BuildDownload(image);
	if (!transfer.valid) {
		return false;
	}
	if (transfer.depth_target && copy_size != image.info.data.size) {
		return false;
	}
	if (!transfer.depth_target && levels < image.info.resources.levels) {
		auto& texture = transfer.texture;
		std::erase_if(texture.regions, [levels](const vk::BufferImageCopy& region) {
			return region.imageSubresource.mipLevel >= levels;
		});
		if (texture.regions.empty()) {
			return false;
		}
		if (!texture.tiles.empty()) {
			texture.tiles.clear();
			if (!TextureBuildGpuTileInfos(copy_size, texture.regions, texture.layout, levels,
			                              texture.tiles)) {
				return false;
			}
		}
	}
	m_texture_cache.DownloadImage(image, buffer, buf_offset, copy_size, std::move(transfer));
	return true;
}

bool TextureCache::DownloadImageMemory(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.depth_id) {
		return false;
	}
	auto transfer = BuildDownload(image);
	if (!transfer.valid || !image.SafeToDownload()) {
		return false;
	}
	const auto range    = image.info.data;
	auto&      download = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset] =
	    download.Map(range.size, std::max<uint64_t>(image.info.bytes_per_block, 4));
	if (mapped == nullptr) {
		EXIT("TextureCache: failed to map reusable download buffer\n");
	}
	download.Commit();
	if (!LibKernel::Memory::TryReadBacking(range.address, mapped, range.size)) {
		return false;
	}
	download.Flush(offset, range.size);

	DownloadImage(image, download, offset, range.size, std::move(transfer));
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = download.Handle();
	barrier.offset              = offset;
	barrier.size                = range.size;
	m_scheduler.EndRendering();
	ProfilePipelineBarrier(m_scheduler.Current().Handle(), vk::PipelineStageFlagBits::eAllCommands,
	                                               vk::PipelineStageFlagBits::eHost, {}, 0, nullptr,
	                                               1, &barrier, 0, nullptr);
	m_scheduler.DeferPriorityOperation([&download, range, mapped, offset] {
		download.Invalidate(offset, range.size);
		LibKernel::Memory::WriteBacking(range.address, mapped, range.size);
	});
	return true;
}

void TextureCache::InvalidateMemoryFromGPU(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		return;
	}
	std::scoped_lock lock {m_lock};
	ForgetDetileReuse(address, size);
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto& image = m_slot_images[id];
		if (image.info.data.address != address) {
			continue;
		}
		if (image.IsGpuModified()) {
			image.ClearGpuModified();
		}
		image.MarkBufferModified();
	}
}

bool TextureCache::IsRegionGpuModified(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		return false;
	}
	std::scoped_lock lock {m_lock};
	for (const auto id: FindImagesInRegion(address, size, false)) {
		const auto& image = m_slot_images[id];
		// PPSA17168: S_LOAD_DWORD reads shader data at an address overlapping an old
		// render target whose memory the CPU has reused. The cached image still retains
		// its earlier GPU-modified flag.
		if (!image.depth_id && image.IsGpuModified() && !image.IsDefinitelyCpuDirty()) {
			return true;
		}
	}
	return false;
}

void TextureCache::InvalidateCpuAliases(uint64_t address, uint64_t size) {
	const auto page_begin = Common::AlignDown(address, TRACKER_PAGE_SIZE);
	const auto page_end   = Common::AlignUp(address + size, TRACKER_PAGE_SIZE);
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		if (owner->Overlaps(address, size)) {
			owner->InvalidateCpuWrite(address, size);
			UntrackImage(id);
			continue;
		}
		const auto image_begin = owner->info.data.address;
		const auto image_end   = owner->info.data.End();
		if (page_end < image_end) {
			UntrackImageHead(id);
		} else if (image_begin < page_begin) {
			UntrackImageTail(id);
		} else {
			MarkAsMaybeDirty(id, *owner);
		}
	}
}

bool TextureCache::IsMeta(uint64_t address) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	return found != m_surface_metas.end();
}

bool TextureCache::IsMetaCleared(uint64_t address, uint32_t slice) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end() || slice >= 32) {
		return false;
	}
	return (found->second.clear_mask & (1u << slice)) != 0;
}

bool TextureCache::ClearMeta(uint64_t address) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end()) {
		return false;
	}
	found->second.clear_mask = UINT32_MAX;
	return true;
}

bool TextureCache::TouchMeta(uint64_t address, uint32_t slice, bool is_clear) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end() || slice >= 32) {
		return false;
	}
	if (is_clear) {
		found->second.clear_mask |= 1u << slice;
	} else {
		found->second.clear_mask &= ~(1u << slice);
	}
	return true;
}

void TextureCache::UnmapMemory(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid unmap range\n");
	}
	std::scoped_lock lock {m_lock};
	for (auto metadata = m_surface_metas.begin(); metadata != m_surface_metas.end();) {
		const auto base = metadata->first;
		if (base >= address && base < address + size) {
			metadata = m_surface_metas.erase(metadata);
		} else {
			++metadata;
		}
	}
	auto images = FindImagesInRegion(address, size, false);
	for (const auto id: images) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		FreeImage(id);
	}
}

void TextureCache::TickFrame() {
	std::scoped_lock lock {m_lock};
	++m_gc_tick;
}

void TextureCache::RunGarbageCollector() {
	std::scoped_lock lock {m_lock};
	// Ages are presented frames; see BufferCache::RunGarbageCollector. On an 8 GB card the
	// pressured age is a fraction of one frame when the tick is a submission.
	const uint64_t   tick = m_gc_tick;
	// --- TEMPORARY DIAGNOSTIC (session 17): what is actually holding 7.4 GB of VRAM? ---------
	// UFC 5 renders at 1068x600 and upscales to 1600x900, so a 7+ GB resident texture set is not
	// a plausible working set. distinct_addresses vs images is the duplication test: if images
	// greatly exceeds distinct_addresses we are keeping many host copies of the same guest memory.
	// unevictable = GPU-modified AND tiled AND safe-to-download, the class the GC must skip.
	// Writes D:/PS5/tex-census.txt every 2s. Remove once the question is answered.
	{
		static std::chrono::steady_clock::time_point next_report {};
		const auto                                  now = std::chrono::steady_clock::now();
		if (now >= next_report) {
			next_report = now + std::chrono::seconds(2);
			uint64_t images = 0;
			uint64_t bytes = 0;
			uint64_t gpu_mod = 0;
			uint64_t gpu_mod_bytes = 0;
			uint64_t unevictable = 0;
			uint64_t unevictable_bytes = 0;
			uint64_t vk_bytes = 0;
			uint64_t sampled_bytes = 0;
			uint64_t storage_bytes = 0;
			uint64_t color_bytes = 0;
			uint64_t both_bytes = 0;
			std::unordered_map<uint64_t, uint32_t> by_address;
			m_slot_images.ForEach([&](ImageId, const Image& image) {
				if (!image.registered) {
					return;
				}
				++images;
				bytes += image.AccountedSize();
				if (image.backing.allocation != nullptr) {
					VmaAllocationInfo allocation {};
					vmaGetAllocationInfo(m_graphics.allocator, image.backing.allocation, &allocation);
					vk_bytes += allocation.size;
				}
				const bool storage = static_cast<bool>(image.backing.usage & vk::ImageUsageFlagBits::eStorage);
				const bool color =
				    static_cast<bool>(image.backing.usage & vk::ImageUsageFlagBits::eColorAttachment);
				const auto accounted = image.AccountedSize();
				if (storage && color) {
					both_bytes += accounted;
				} else if (storage) {
					storage_bytes += accounted;
				} else if (color) {
					color_bytes += accounted;
				} else {
					sampled_bytes += accounted;
				}
				++by_address[image.info.data.address];
				if (image.IsGpuModified()) {
					++gpu_mod;
					gpu_mod_bytes += image.AccountedSize();
					if (image.SafeToDownload() && image.info.IsTiled()) {
						++unevictable;
						unevictable_bytes += image.AccountedSize();
					}
				}
			});
			uint32_t worst = 0;
			uint64_t worst_addr = 0;
			for (const auto& [addr, count]: by_address) {
				if (count > worst) {
					worst      = count;
					worst_addr = addr;
				}
			}
			uint64_t vma_blocks = 0;
			uint64_t vma_allocs = 0;
			uint64_t drv_usage  = 0;
			uint64_t drv_budget = 0;
			uint64_t host_blocks = 0;
			uint64_t host_allocs = 0;
			uint64_t host_usage = 0;
			uint64_t host_budget = 0;
			m_graphics.ReportMemoryBreakdown(vma_blocks, vma_allocs, drv_usage, drv_budget,
			                                 host_blocks, host_allocs, host_usage, host_budget);
			if (std::FILE* f = std::fopen("D:/PS5/tex-census.txt", "a"); f != nullptr) {
				std::fprintf(f,
				             "TexCensus: images=%llu bytes=%lluMB | distinct_addr=%zu dup_factor=%.2f "
				             "worst=0x%llx x%u | gpu_mod=%llu/%lluMB unevictable=%llu/%lluMB | "
				             "accounted=%lluMB trigger=%lluMB critical=%lluMB | "
				             "VMA blocks=%lluMB allocs=%lluMB | driver usage=%lluMB budget=%lluMB | "
				             "host VMA=%lluMB allocs=%lluMB usage=%lluMB budget=%lluMB | "
			             "buffers=%zu/%lluMB bda_table=%lluMB leaves=%llu | "
			             "img_vk=%lluMB sampled=%lluMB storage=%lluMB color=%lluMB both=%lluMB "
			             "tight=%u upgrades=%llu\n",
				             static_cast<unsigned long long>(images),
				             static_cast<unsigned long long>(bytes / (1024 * 1024)),
				             by_address.size(),
				             by_address.empty() ? 0.0
				                                : static_cast<double>(images) /
				                                      static_cast<double>(by_address.size()),
				             static_cast<unsigned long long>(worst_addr), worst,
				             static_cast<unsigned long long>(gpu_mod),
				             static_cast<unsigned long long>(gpu_mod_bytes / (1024 * 1024)),
				             static_cast<unsigned long long>(unevictable),
				             static_cast<unsigned long long>(unevictable_bytes / (1024 * 1024)),
				             static_cast<unsigned long long>(m_total_used_memory / (1024 * 1024)),
				             static_cast<unsigned long long>(m_trigger_gc_memory / (1024 * 1024)),
				             static_cast<unsigned long long>(m_critical_gc_memory / (1024 * 1024)),
				             static_cast<unsigned long long>(vma_blocks / (1024 * 1024)),
				             static_cast<unsigned long long>(vma_allocs / (1024 * 1024)),
				             static_cast<unsigned long long>(drv_usage / (1024 * 1024)),
				             static_cast<unsigned long long>(drv_budget / (1024 * 1024)),
				             static_cast<unsigned long long>(host_blocks / (1024 * 1024)),
				             static_cast<unsigned long long>(host_allocs / (1024 * 1024)),
				             static_cast<unsigned long long>(host_usage / (1024 * 1024)),
				             static_cast<unsigned long long>(host_budget / (1024 * 1024)),
				             m_buffer_cache.RegisteredBufferCount(),
			             static_cast<unsigned long long>(m_buffer_cache.RegisteredBufferBytes() / (1024 * 1024)),
			             static_cast<unsigned long long>(m_buffer_cache.BdaTableAllocatedBytes() / (1024 * 1024)),
			             static_cast<unsigned long long>(m_buffer_cache.BdaLeafCount()),
			             static_cast<unsigned long long>(vk_bytes / (1024 * 1024)),
			             static_cast<unsigned long long>(sampled_bytes / (1024 * 1024)),
			             static_cast<unsigned long long>(storage_bytes / (1024 * 1024)),
			             static_cast<unsigned long long>(color_bytes / (1024 * 1024)),
			             static_cast<unsigned long long>(both_bytes / (1024 * 1024)),
			             TightImageUsageEnabled() ? 1u : 0u,
			             static_cast<unsigned long long>(ImageUsageUpgradeCount()));
				std::fclose(f);
			}
		}
	}
	// Driver usage includes other caches and allocator reservations. Keep it
	// separate from the owned-byte counter decremented during unregistration.
	auto used_memory = m_graphics.CanReportMemoryUsage()
	    ? m_graphics.GetDeviceMemoryUsage() : m_total_used_memory;
	if (used_memory < m_trigger_gc_memory) {
		return;
	}
	const auto collect = [&](bool allow_aggressive) {
		bool           pressured  = used_memory >= m_pressure_gc_memory;
		bool           aggressive = allow_aggressive && used_memory >= m_critical_gc_memory;
		const uint64_t age       = std::min<uint64_t>(aggressive ? 160 : pressured ? 80 : 16, tick);
		size_t         deletions = aggressive ? 40 : pressured ? 20 : 10;
		std::vector<ImageId> candidates;
		candidates.reserve(deletions);
		// Deleting depth recursively deletes its stencil association, so finish LRU traversal
		// first.
		m_lru_cache.ForEachItemBelow(tick - age, [&](ImageId id) {
			candidates.push_back(id);
			return candidates.size() == deletions;
		});
		for (const auto id: candidates) {
			if (deletions == 0) {
				break;
			}
			--deletions;
			auto owner = m_slot_images.try_get(id);
			if (owner == nullptr || !owner->registered || owner->depth_id) {
				continue;
			}
			if (owner->IsGpuModified()) {
				const bool safe = owner->SafeToDownload();
				if (safe && owner->info.IsTiled()) {
					continue;
				}
				if (safe && !pressured) {
					continue;
				}
				if (safe && !DownloadImageMemory(id)) {
					continue;
				}
			}
			used_memory -= std::min(used_memory, owner->AccountedSize());
			FreeImage(id);
			if (used_memory < m_critical_gc_memory && aggressive) {
				deletions >>= 2;
				aggressive = false;
			}
			if (used_memory < m_pressure_gc_memory && pressured) {
				deletions >>= 1;
				pressured = false;
			}
		}
	};
	collect(false);
	if (used_memory >= m_critical_gc_memory) {
		collect(true);
	}
	// Opt-in VRAM test. Every frame, drop every image not used this frame, including
	// GPU-owned ones. Sampled lookups above return a 1x1 stand-in, so they stay gone.
	if (DropTexturesEnabled() && tick > 0) {
		static uint64_t dropped_on_tick = UINT64_MAX;
		if (tick != dropped_on_tick) {
			dropped_on_tick = tick;
			std::vector<ImageId> cold;
			m_lru_cache.ForEachItemBelow(tick - 1, [&](ImageId id) { cold.push_back(id); });
			uint64_t freed_bytes = 0;
			size_t   freed_count = 0;
			for (const auto id: cold) {
				auto* image = m_slot_images.try_get(id);
				if (image == nullptr || !image->registered || image->depth_id) {
					continue;
				}
				freed_bytes += image->AccountedSize();
				FreeImage(id);
				++freed_count;
			}
			uint64_t blocks = 0, allocs = 0, driver_usage = 0, driver_budget = 0;
			uint64_t host_blocks = 0, host_allocs = 0, host_usage = 0, host_budget = 0;
			m_graphics.ReportMemoryBreakdown(blocks, allocs, driver_usage, driver_budget,
			                                 host_blocks, host_allocs, host_usage, host_budget);
			if (std::FILE* log = std::fopen("D:/PS5/tex-drop.txt", "a"); log != nullptr) {
				std::fprintf(log,
				             "DropTextures: tick=%llu usage=%lluMB budget=%lluMB host=%lluMB "
				             "freed=%zu/%lluMB cold=%zu\n",
				             static_cast<unsigned long long>(tick),
				             static_cast<unsigned long long>(driver_usage / (1024 * 1024)),
				             static_cast<unsigned long long>(driver_budget / (1024 * 1024)),
				             static_cast<unsigned long long>(host_usage / (1024 * 1024)),
				             freed_count,
				             static_cast<unsigned long long>(freed_bytes / (1024 * 1024)),
				             cold.size());
				std::fclose(log);
			}
		}
	}
	// Diagnostic pressure test. A trigger file lets one fight provide both sides of an A/B.
	// Only retire clean, old images: GPU-owned tiled images cannot be downloaded safely, and
	// downloading other GPU-owned images would introduce a GPU wait into the measurement.
	if ((tick & 127u) == 0) {
		std::FILE* trigger = std::fopen("D:/PS5/dumps/VRAM_PRESSURE_TEST", "rb");
		if (trigger != nullptr) {
			std::fclose(trigger);
			uint64_t blocks = 0, allocs = 0, driver_usage = 0, driver_budget = 0;
			uint64_t host_blocks = 0, host_allocs = 0, host_usage = 0, host_budget = 0;
			m_graphics.ReportMemoryBreakdown(blocks, allocs, driver_usage, driver_budget,
			                                 host_blocks, host_allocs, host_usage, host_budget);
			const uint64_t margin = 128ull * 1024 * 1024;
			if (driver_budget > margin && driver_usage > driver_budget - margin) {
				std::vector<ImageId> candidates;
				candidates.reserve(64);
				size_t examined = 0;
				const auto old_before = tick > 512 ? tick - 512 : uint64_t {0};
				m_lru_cache.ForEachItemBelow(old_before, [&](ImageId id) {
					if (++examined > 8192) return true;
					const auto* image = m_slot_images.try_get(id);
					if (image != nullptr && image->registered && !image->depth_id &&
					    !image->IsGpuModified()) {
						candidates.push_back(id);
					}
					return candidates.size() == 64;
				});
				uint64_t retired_bytes = 0;
				for (const auto id: candidates) {
					const auto* image = m_slot_images.try_get(id);
					if (image == nullptr || !image->registered || image->IsGpuModified()) continue;
					retired_bytes += image->AccountedSize();
					FreeImage(id);
				}
				if (std::FILE* log = std::fopen("D:/PS5/tex-pressure-test.txt", "a"); log != nullptr) {
					std::fprintf(log,
					             "PressureSweep: tick=%llu usage=%lluMB budget=%lluMB examined=%zu "
					             "retired=%zu/%lluMB\n",
					             static_cast<unsigned long long>(tick),
					             static_cast<unsigned long long>(driver_usage / (1024 * 1024)),
					             static_cast<unsigned long long>(driver_budget / (1024 * 1024)),
					             examined, candidates.size(),
					             static_cast<unsigned long long>(retired_bytes / (1024 * 1024)));
					std::fclose(log);
				}
			}
		}
	}
}

void TextureCache::ProcessDownloadImages() {
	std::scoped_lock lock {m_lock};
	for (const auto id: m_download_images) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->registered && owner->IsGpuModified()) {
			(void)DownloadImageMemory(id);
		}
	}
	m_download_images.clear();
}

} // namespace Libs::Graphics
