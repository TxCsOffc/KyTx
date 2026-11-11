#include "graphics/host_gpu/metal/metalTextureKey.h"
#include "graphics/host_gpu/metal/metalGuestWrites.h"
#include "graphics/host_gpu/metal/metalHostBudget.h"
#include "graphics/host_gpu/metal/metalShaderGuestWrites.h"
#include <objc/runtime.h>
#include "graphics/host_gpu/metal/metalPassTimer.h"
#include "graphics/host_gpu/metal/metalEnv.h"
#include "graphics/host_gpu/metal/metalRenderer.h"
#include "graphics/host_gpu/metal/metalShaderAbi.h"
#include "graphics/presentation/traceWindow.h"
#include "graphics/host_gpu/metal/metalBuffer.h"
#include "graphics/host_gpu/metal/metalClearDecode.h"
#include "graphics/host_gpu/metal/metalAttachmentArray.h"
#include "graphics/host_gpu/metal/metalDepthClearState.h"
#include "graphics/host_gpu/metal/metalIndirect.h"
#include "graphics/host_gpu/metal/metalUnsupported.h"
#include "graphics/host_gpu/metal/metalOrder.h"
#include "graphics/host_gpu/metal/metalPresenter.h"
#include "graphics/host_gpu/metal/metalSampler.h"
#include "graphics/host_gpu/metal/metalTiling.h"
#include "graphics/host_gpu/metal/metalCanonicalMemory.h"
#include "graphics/host_gpu/metal/metalVolumeMemory.h"
#include "graphics/host_gpu/metal/metalDepthMemory.h"
#include "graphics/host_gpu/metal/metalAddressSpace.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/guest_gpu/command_processor/pm4MemorySync.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/backend/msl/MslEmitter.h"
#include "common/logging/log.h"
#include "kernel/memory.h"
#include "graphics/host_gpu/metal/metalGuestMemoryView.h"
#include "graphics/host_gpu/metal/metalGuestMipView.h"

#include <algorithm>
#include <bit>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <tuple>
#include <cstring>
#include <chrono>
#include <mutex>
#import <objc/runtime.h>
#include <unordered_set>
#include <set>
#include <vector>

namespace Libs::Graphics {
uint64_t KytyCpLag(uint64_t* flips_enqueued, uint64_t* flips_done, uint64_t* gfx_started); // graphicsRun.cpp
} // namespace Libs::Graphics

namespace Libs::Graphics::Metal {
void KytyFaultOpSet(const char* label);  // metalDraw.mm: per-op label for GPU fault attribution
// Residency usage for a texture must match its creation usage: requesting Write on a texture that was not
// created with ShaderWrite is rejected by Metal validation ("flags ... do not match shader usage").
// Unsupported materialization is an error in every build, never an RGBA8 substitution.
[[noreturn]] static void FailMetalMaterialization(const char* site, uint32_t key, uint32_t a = 0, uint32_t b = 0) {
	RejectMetalState(std::string(site) + ": key=" + std::to_string(key) + " extra=" + std::to_string(a) + "/" + std::to_string(b));
}

static void RequireNullImageDescriptor(const ShaderStageRuntime& stage, uint32_t resource) {
	ShaderTextureResource descriptor {};
	if (stage.resources == nullptr || resource >= stage.resources->images.size())
		RejectMetalState("missing image descriptor for resource " + std::to_string(resource));
	std::copy_n(stage.resources->images[resource].dwords.begin(), 8, descriptor.fields);
	if (!descriptor.IsNull())
		RejectMetalState("unbound non-null image: shader=" + std::to_string(stage.program ? stage.program->shader_hash : 0) +
		                 " resource=" + std::to_string(resource) + " address=" + std::to_string(descriptor.Base40()));
}

// KYTY_MEM_LOG: bytes of every texture / buffer this file allocates, per source line (top lines go to the memory log).
static std::mutex s_newres_mutex;
static std::map<int, std::pair<uint64_t, uint64_t>> s_newres_by_line; // line -> (count, bytes)
static void NoteNewResource(int line, uint64_t bytes) {
	static const bool s_on = KYTY_ENV("KYTY_MEM_LOG") != nullptr;
	if (!s_on) return;
	std::lock_guard<std::mutex> lock(s_newres_mutex);
	auto& e = s_newres_by_line[line];
	++e.first;
	e.second += bytes;
}
static id<MTLTexture> KytyNewTexture(id<MTLDevice> dev, MTLTextureDescriptor* d, int line) {
	id<MTLTexture> t = [dev newTextureWithDescriptor:d];
	if (t != nil) NoteNewResource(line, static_cast<uint64_t>(t.allocatedSize));
	return t;
}
static id<MTLBuffer> KytyNewBuffer(id<MTLDevice> dev, NSUInteger length, MTLResourceOptions opts, int line) {
	id<MTLBuffer> b = [dev newBufferWithLength:length options:opts];
	if (b != nil) NoteNewResource(line, static_cast<uint64_t>(length));
	return b;
}

static MTLResourceUsage ResidencyUsage(id<MTLResource> r) {
	// conformsToProtocol is a slow Objective-C runtime query (about a fifth of the command thread when asked for every resource of every
	// draw), and the class of a resource settles the answer: there are only a few resource classes, so remember it per class.
	static thread_local std::unordered_map<Class, bool> s_is_texture_by_class;
	Class cls = object_getClass(r);
	auto  hit = s_is_texture_by_class.find(cls);
	if (hit == s_is_texture_by_class.end()) hit = s_is_texture_by_class.emplace(cls, [r conformsToProtocol:@protocol(MTLTexture)] ? true : false).first;
	if (hit->second) {
		id<MTLTexture> t = (id<MTLTexture>)r;
		return (t.usage & MTLTextureUsageShaderWrite) != 0 ? (MTLResourceUsageRead | MTLResourceUsageWrite)
		                                                    : MTLResourceUsageRead;
	}
	return MTLResourceUsageRead | MTLResourceUsageWrite;
}

// DEPTH_COMPARE_FUNC of a sampler slot (GCN order = MTLCompareFunction order), for the native MSL depth-compare samples: they read it
// from sizes[64 + slot] (kyty_sample_cmp). LESS_EQUAL when the slot has no descriptor.
static uint32_t StageSamplerCompare(const ShaderStageRuntime& stage, uint32_t resource) {
	if (!stage.program || !stage.resources || resource >= stage.program->info.samplers.size()) return 3u;
	const auto& info = stage.program->info.samplers[resource];
	if (info.snapshot_index >= stage.resources->samplers.size()) return 3u;
	const auto& descriptor = stage.resources->samplers[info.snapshot_index];
	if (descriptor.dword_count < 1) return 3u;
	const uint32_t func = (descriptor.dwords[0] >> 12u) & 0x7u;
	// KYTY_LOG_CMP=1: every (shader, sampler slot, compare function) once.
	static const bool s_log = KYTY_ENV("KYTY_LOG_CMP") != nullptr;
	if (s_log) {
		static std::mutex s_m;
		static std::set<std::tuple<uint64_t, uint32_t, uint32_t>> s_seen;
		std::lock_guard<std::mutex> lock(s_m);
		if (s_seen.insert({stage.program->shader_hash, resource, func}).second)
			std::fprintf(stderr, "[SamplerCmp] shader=0x%016llx guest=0x%016llx slot=%u func=%u s0=%08x\n",
			             (unsigned long long)stage.program->shader_hash, (unsigned long long)stage.program->guest_hash, resource, func,
			             descriptor.dwords[0]);
	}
	return func;
}

static id<MTLSamplerState> StageSampler(const ShaderStageRuntime& stage, uint32_t resource, bool comparison_point = false) {
	if (!stage.program || !stage.resources || resource >= stage.program->info.samplers.size()) return nil;
	const auto& info = stage.program->info.samplers[resource];
	if (info.snapshot_index >= stage.resources->samplers.size()) return nil;
	const auto& descriptor = stage.resources->samplers[info.snapshot_index];
	if (descriptor.dword_count < 4) return nil;
	ShaderSamplerResource sampler {};
	std::copy_n(descriptor.dwords.begin(), 4, sampler.fields);
	// KYTY_LOG_SAMPLERS=1: every distinct (guest shader, sampler slot, S# dwords, forced point) once, decoded (xy filter: 0 point, 1 bilinear,
	// 2 aniso point, 3 aniso bilinear; mip filter: 0 none, 1 point, 2 linear; lods in 1/256).
	static const bool s_log_samplers = KYTY_ENV("KYTY_LOG_SAMPLERS") != nullptr;
	if (s_log_samplers && !comparison_point) {
		static std::mutex s_ls_mutex;
		static std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool>> s_ls_seen;
		std::lock_guard<std::mutex> ls_lock(s_ls_mutex);
		if (s_ls_seen.insert({stage.program->guest_hash, resource, sampler.fields[0], sampler.fields[1], sampler.fields[2], sampler.fields[3],
		                      info.force_point_filtering}).second) {
			std::fprintf(stderr,
			             "[Sampler] guest=0x%016llx slot=%u s=%08x %08x %08x %08x mag=%u min=%u mip=%u minlod=%u maxlod=%u bias=%d aniso=%u cmp=%u unorm=%d "
			             "clamp=%u,%u force_point=%d\n",
			             (unsigned long long)stage.program->guest_hash, resource, sampler.fields[0], sampler.fields[1], sampler.fields[2], sampler.fields[3],
			             sampler.XyMagFilter(), sampler.XyMinFilter(), sampler.MipFilter(), sampler.MinLod(), sampler.MaxLod(),
			             static_cast<int32_t>(sampler.LodBias() ^ 0x2000u) - 0x2000, sampler.MaxAnisoRatio(), sampler.DepthCompareFunc(),
			             sampler.ForceUnormCoords() ? 1 : 0, sampler.ClampX(), sampler.ClampY(), info.force_point_filtering ? 1 : 0);
		}
	}
	if (info.force_point_filtering) sampler.SetPointFiltering();
	if (info.depth_compare) sampler.fields[0] &= ~(3u << 29u); // Compare footprints always average their boolean results.
	else sampler.fields[0] &= ~(7u << 12u);
	return GetSampler(MetalContext::Instance().GetDevice(), sampler, comparison_point);
}
// Buffer 25 carries the actual four S# words for comparison filtering.
// Lookup follows sampler snapshot_index, including typed variants of one S#.
static void StoreSamplerDescriptor(const ShaderStageRuntime& stage, uint32_t resource, uint32_t* sizes) {
	ShaderSamplerResource sampler {};
	sampler.fields[0] = 2u | (2u << 3u) | (2u << 6u) | (3u << 12u);
	sampler.fields[2] = (1u << 20u) | (1u << 22u);
	if (stage.program && stage.resources && resource < stage.program->info.samplers.size()) {
		const auto& info = stage.program->info.samplers[resource];
		if (info.snapshot_index < stage.resources->samplers.size()) {
			const auto& descriptor = stage.resources->samplers[info.snapshot_index];
			if (descriptor.dword_count >= 4) std::copy_n(descriptor.dwords.begin(), 4, sampler.fields);
		}
		if (info.force_point_filtering) sampler.SetPointFiltering();
	}
	std::copy_n(sampler.fields, 4, sizes + ShaderRecompiler::Msl::SamplerDescriptorWordBase + resource * 4u);
}
static bool StageHasPointFetchSampler(const ShaderStageRuntime& stage, uint32_t resource) {
	return stage.program && resource < stage.program->info.samplers.size() &&
	       (stage.program->info.samplers[resource].depth_compare || stage.program->info.samplers[resource].point_fetch);
}
// Size in bytes the V# declares (num_records, times stride for strided buffers). RDNA returns 0 for any access
// past it, so this - not the largest extent the shader statically uses - is the bound the shader must honour.
template <typename Dwords>
static uint32_t VsharpBytes(const Dwords& dwords) {
	const uint32_t stride = (dwords[1] >> 16) & 0x3fffu;
	const uint64_t bytes = static_cast<uint64_t>(dwords[2]) * std::max<uint32_t>(stride, 1u);
	return static_cast<uint32_t>(std::min<uint64_t>(bytes, UINT32_MAX));
}
// Address-table use includes read-only loads and indirect descriptor reads. The IR's
// has_address_writes flag identifies stores whose physical target is not known.
static void NoteStageGuestWrites(const ShaderStageRuntime& stage) {
	if (stage) GuestWrites::NoteShaderWrites(*stage.program,*stage.resources);
}
// Constant data lives in guest memory that the CPU recycles (per-frame rings) while this renderer's GPU work is
// still queued, so a draw/dispatch could read values written for a later frame (observed: a loop count turned
// into a float, hanging the GPU). Small read-only buffers are copied into the dynamic ring at encode time.
// Only the CPU-written constant arena is snapshotted: GPU-produced buffers (counters in the 0x8241.. region) must be
// read live, because their producer has not run yet when the consumer is encoded.
static MetalBufferAllocation SnapshotBytes(const MetalBufferAllocation& src, uint64_t vaddr, size_t bytes,
                                           bool cpu_staging = false) {
	// KYTY_SNAPSHOT_ARENA_KB=<n>: draw-stage limit (default 16 KiB). The skinned players' per-instance table (27 dwords per record,
	// 256 KiB, 0x8190...) is past the default and is read live while the CPU already rewrites it for the next frame.
	static const size_t s_draw_limit = KYTY_ENV("KYTY_SNAPSHOT_ARENA_KB") != nullptr ? std::strtoull(KYTY_ENV("KYTY_SNAPSHOT_ARENA_KB"), nullptr, 0) * 1024u : 16384u;
	const size_t limit = cpu_staging ? 512u * 1024u : s_draw_limit;
	if (vaddr < 0x8190000000ull || vaddr >= 0x81a0000000ull || bytes == 0 || bytes > limit ||
	    bytes > 0x81a0000000ull - vaddr) {
		return src;
	}
	auto dyn = MetalBufferManager::Instance().SnapshotReadOnly(src, bytes);
	return dyn.IsValid() ? dyn : src;
}

// Sample the range-copy kernel's output in GPU order. CPU reads of the live matrix pool at
// encode time are not evidence of what the later vertex shader consumes. This probe copies
// selected output rows immediately after the dispatch, then checks them only on completion.
static void ProbeSkinUpload(const std::array<MetalBufferAllocation, 64>& buffers) {
	static const bool enabled = KYTY_ENV("KYTY_PROBE_SKIN_UPLOAD") != nullptr;
	if (!enabled) return;
	const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load();
	static uint64_t last_flip = UINT64_MAX;
	if (flip < 1500 || (last_flip != UINT64_MAX && flip < last_flip + 100)) return;
	last_flip = flip;
	const auto& ranges = buffers[0];
	const auto& source = buffers[1];
	const auto& dest = buffers[2];
	if (!ranges.IsValid() || !source.IsValid() || !dest.IsValid() || ranges.cpu_addr == nullptr || source.cpu_addr == nullptr) return;
	struct Sample { uint32_t dest_row, source_row; std::array<uint32_t, 4> expected; };
	std::vector<Sample> samples;
	for (size_t r = 0; r < std::min<uint64_t>(ranges.size / 16, 16); ++r) {
		uint32_t range[4];
		std::memcpy(range, static_cast<const uint8_t*>(ranges.cpu_addr) + r * 16, 16);
		const uint64_t count = range[0], dst_row = range[1], src_row = range[2];
		if (count == 0 || src_row + count > source.size / 16 || dst_row + count > dest.size / 16) continue;
		for (uint64_t row: {uint64_t{0}, count / 2, count - 1}) {
			Sample sample {static_cast<uint32_t>(dst_row + row), static_cast<uint32_t>(src_row + row), {}};
			std::memcpy(sample.expected.data(), static_cast<const uint8_t*>(source.cpu_addr) + (src_row + row) * 16, 16);
			samples.push_back(sample);
		}
	}
	if (samples.empty()) return;
	auto& context = MetalContext::Instance();
	id<MTLBuffer> output = KytyNewBuffer(context.GetDevice(), samples.size() * 16, MTLResourceStorageModeShared, __LINE__);
	if (output == nil) return;
	context.EndCurrentEncoder();
	id<MTLCommandBuffer> command = context.GetCurrentCommandBuffer();
	id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
	if (blit == nil) return;
	for (size_t i = 0; i < samples.size(); ++i) {
		[blit copyFromBuffer:dest.buffer sourceOffset:dest.offset + uint64_t{samples[i].dest_row} * 16
		           toBuffer:output destinationOffset:i * 16 size:16];
	}
	[blit endEncoding];
	[command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
		if (completed.status == MTLCommandBufferStatusError) {
			std::fprintf(stderr, "[SkinProbe] flip=%llu GPU error; samples invalid\n", (unsigned long long)flip);
			return;
		}
		const auto* actual = static_cast<const uint32_t*>(output.contents);
		size_t mismatches = 0, first = 0;
		for (size_t i = 0; i < samples.size(); ++i) {
			if (std::memcmp(actual + i * 4, samples[i].expected.data(), 16) != 0) {
				if (mismatches++ == 0) first = i;
			}
		}
		std::fprintf(stderr, "[SkinProbe] flip=%llu rows=%zu mismatches=%zu\n", (unsigned long long)flip, samples.size(), mismatches);
		if (mismatches != 0) {
			const auto& s = samples[first];
			const uint32_t* a = actual + first * 4;
			std::fprintf(stderr, "[SkinProbe] dst_row=%u src_row=%u expected=%08x,%08x,%08x,%08x actual=%08x,%08x,%08x,%08x\n",
			             s.dest_row, s.source_row, s.expected[0], s.expected[1], s.expected[2], s.expected[3], a[0], a[1], a[2], a[3]);
		}
	}];
}
// Set by DrawAuto for a single-rectangle RECTLIST draw; read when the vertex sizes table is built (sizes[63] == 1
// tells the vertex shader wrapper to synthesise the fourth corner).
static bool s_draw_is_rect_list = false;
static bool s_draw_is_indirect_rect_list = false;
static uint32_t s_rect_idx[3] = {0, 0, 0};  // source vertex ids of an indexed rect list (sizes[59..61])
static bool s_rect_indexed = false;
// ---- Colour-metadata clears (DCC / CMASK fast clear) ----
// The game clears many render targets by filling their DCC/CMASK metadata with a clear code (a recognised "uniform fill" compute kernel);
// the pixels are never written, the hardware reads cleared blocks as the clear colour. The Vulkan backend turns such a fill into an image
// clear (TextureCache::TryConsumeColorMetaUniformFill / DecodeColorClear). Metal executes the fill as plain byte writes, so the pixel
// memory kept stale data (a previous frame at another resolution => sheared garbage). Here a fill over a known target's metadata range
namespace {
struct MetaClearRt {
	uint64_t       rt_addr = 0;
	uint32_t       clear_word0 = 0, clear_word1 = 0;
	MTLPixelFormat format = MTLPixelFormatInvalid;
	bool           cmask = false;
};
std::mutex s_meta_mutex;
std::unordered_map<uint64_t, MetaClearRt> s_meta_by_base;          // metadata base address -> target
std::unordered_map<uint64_t, std::array<float, 4>> s_pending_meta_clear;  // target address -> clear colour
// Every recognised uniform fill (metadata clear candidates), by start address, and the order of fills and colour-target passes: a sampled
// DCC image whose metadata a fill cleared after the image's last render pass reads as the clear colour (see SampledMetaClear).
struct MetaFill {
	uint64_t size = 0;
	uint32_t code = 0;
	uint64_t seq  = 0;
};
std::map<uint64_t, MetaFill> s_meta_fills;
std::unordered_map<uint64_t, uint64_t> s_rt_pass_seq; // colour target address -> seq of its last render pass
std::unordered_map<uint64_t, uint64_t> s_meta_pass_seq; // DCC metadata address -> seq of the last render pass of a target using it
std::unordered_map<uint64_t, uint64_t> s_dcc_of_rt;     // colour target address -> its DCC metadata address (compute writes stamp it too)
uint64_t s_meta_seq = 0;

// DecodeRegisterClear (CB_COLOR_CLEAR_WORD0/1 -> colour per target format) lives in metalClearDecode.h, with its tests.

// Clear code -> colour (the Vulkan DecodeColorClear): DCC 0x20 = clear register, 0x00/0x40/0x80/0xc0 = constants; CMASK 0 = register.
bool DecodeMetaClearCode(const MetaClearRt& rt, uint32_t code, std::array<float, 4>& out) {
	if (rt.cmask ? code == 0u : code == 0x20u) {
		return DecodeRegisterClear(rt.format, rt.clear_word0, rt.clear_word1, out);
	}
	if (rt.cmask) return false;
	switch (code) {
		case 0x00: out = {0.f, 0.f, 0.f, 0.f}; return true;
		case 0x40: out = {0.f, 0.f, 0.f, 1.f}; return true;
		case 0x80: out = {1.f, 1.f, 1.f, 0.f}; return true;
		case 0xc0: out = {1.f, 1.f, 1.f, 1.f}; return true;
		default: return false;
	}
}

bool MetaClearEnabled() {
	static const bool s_on = true;
	return s_on;
}
} // namespace

// A depth pass or a compute storage write of a surface: it is no longer "cleared by metadata" for SampledMetaClear.
// `storage`: a shader image store (not a render or depth pass); it also counts as a write of the DCC target registered at the address.
static void NoteSurfaceWrite(uint64_t addr, bool storage = false) {
	if (!MetaClearEnabled() || addr == 0) return;
	std::lock_guard<std::mutex> lock(s_meta_mutex);
	s_rt_pass_seq[addr] = ++s_meta_seq;
	if (!storage) return;
	if (const auto it = s_dcc_of_rt.find(addr); it != s_dcc_of_rt.end()) s_meta_pass_seq[it->second] = s_meta_seq;
}

static void RegisterMetaClearRt(uint64_t rt_addr, uint64_t dcc_addr, uint64_t cmask_addr, bool dcc_enabled, bool cmask_fast_clear,
                                uint32_t w0, uint32_t w1, MTLPixelFormat fmt) {
	if (!MetaClearEnabled() || rt_addr == 0) return;
	std::lock_guard<std::mutex> lock(s_meta_mutex);
	// The target's DCC metadata was filled after its last pass, before this target was ever registered at that metadata (the fill came
	// first in the frame, so NoteUniformMetadataFill found no target to queue): the pass that now loads it must start from the clear colour,
	// not from the bytes of whatever surface used the memory before. The half-res transparency layer (800x452 RGBA16F) loaded NaN/inf
	// from them => the white bloom blob over the street/stadium scenes.
	// KYTY_RT_META_CLEAR=1 (opt-in, not validated): um151-um153 also cleared RG8/RG16F/RGBA8 targets the scenes then missed (sparkles,
	// more bands), while the RGBA16F layer behind the blob was cleared only twice.
	static const bool s_rt_meta_clear = KYTY_ENV("KYTY_RT_META_CLEAR") != nullptr;
	if (s_rt_meta_clear && dcc_enabled && dcc_addr != 0 && s_pending_meta_clear.find(rt_addr) == s_pending_meta_clear.end()) {
		auto it = s_meta_fills.upper_bound(dcc_addr);
		if (it != s_meta_fills.begin()) {
			--it;
			// The last pass of a target with this metadata, not of anything at this address: a transient surface aliasing the memory
			// (written between the fill and this pass) leaves this target's metadata, and so its clear, intact.
			const auto pass = s_meta_pass_seq.find(dcc_addr);
			const uint64_t last_pass = pass != s_meta_pass_seq.end() ? pass->second : 0;
			std::array<float, 4> color {};
			if (dcc_addr < it->first + it->second.size && it->second.seq > last_pass &&
			    DecodeMetaClearCode({rt_addr, w0, w1, fmt, false}, it->second.code, color)) {
				s_pending_meta_clear[rt_addr] = color;
				static std::atomic<uint32_t> s_log {0};
				if (s_log.fetch_add(1) < 200) {
					std::fprintf(stderr, "[MetaClear] rt=0x%llx fmt=%lu: metadata 0x%llx filled (code 0x%02x) before its first pass -> clear (%g,%g,%g,%g)\n",
					             (unsigned long long)rt_addr, (unsigned long)fmt, (unsigned long long)dcc_addr, it->second.code, color[0], color[1],
					             color[2], color[3]);
				}
			}
		}
	}
	s_rt_pass_seq[rt_addr] = ++s_meta_seq;
	if (dcc_enabled && dcc_addr != 0) {
		s_meta_pass_seq[dcc_addr] = s_meta_seq;
		s_dcc_of_rt[rt_addr] = dcc_addr;
	}
	if (dcc_enabled && dcc_addr != 0) s_meta_by_base[dcc_addr] = {rt_addr, w0, w1, fmt, false};
	if (cmask_fast_clear && cmask_addr != 0) s_meta_by_base[cmask_addr] = {rt_addr, w0, w1, fmt, true};
}

// A recognised uniform fill of [addr, addr+size) with `value` (one word per element): if it covers a known target's metadata, queue the
// clear. Called when the dispatch is encoded; the fill itself still runs on the GPU.
static void NoteUniformMetadataFill(uint64_t addr, uint64_t size, uint32_t value) {
	if (!MetaClearEnabled() || addr == 0 || size == 0) return;
	// KYTY_LOG_META_FILL=1: every recognised fill from flip 1500 (the capped [MetaClear] log is used up during boot).
	static const bool s_log_fill = KYTY_ENV("KYTY_LOG_META_FILL") != nullptr;
	if (s_log_fill && Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed) >= 1500) {
		std::lock_guard<std::mutex> lock(s_meta_mutex);
		uint64_t match = 0;
		for (const auto& [base, rt] : s_meta_by_base) {
			if (base >= addr && base < addr + size) match = rt.rt_addr;
		}
		std::fprintf(stderr, "[MetaFill] flip=%llu addr=0x%llx size=%llu value=%08x rt=0x%llx\n",
		             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)),
		             static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size), value, static_cast<unsigned long long>(match));
	}
	const uint32_t code = value & 0xffu;
	if (value != code * 0x01010101u) return;
	std::lock_guard<std::mutex> lock(s_meta_mutex);
	s_meta_fills[addr] = {size, code, ++s_meta_seq};
	if (s_meta_fills.size() > 4096) s_meta_fills.erase(s_meta_fills.begin());
	for (const auto& [base, rt] : s_meta_by_base) {
		if (base < addr || base >= addr + size) continue;
		std::array<float, 4> color {};
		const bool ok = DecodeMetaClearCode(rt, code, color);
		static std::atomic<uint32_t> s_log {0};
		if (s_log.fetch_add(1) < 40) {
			std::fprintf(stderr, "[MetaClear] flip=%llu fill addr=0x%llx size=%llu code=0x%02x -> rt=0x%llx %s fmt=%lu color=(%g,%g,%g,%g) %s\n",
			             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)),
			             static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size), code,
			             static_cast<unsigned long long>(rt.rt_addr), rt.cmask ? "cmask" : "dcc", static_cast<unsigned long>(rt.format),
			             color[0], color[1], color[2], color[3], ok ? "queued" : "unsupported");
		}
		if (ok) s_pending_meta_clear[rt.rt_addr] = color;
	}
}

static bool TakePendingMetaClear(uint64_t rt_addr, float out[4]) {
	if (!MetaClearEnabled()) return false;
	std::lock_guard<std::mutex> lock(s_meta_mutex);
	const auto it = s_pending_meta_clear.find(rt_addr);
	if (it == s_pending_meta_clear.end()) return false;
	for (int i = 0; i < 4; ++i) out[i] = it->second[static_cast<size_t>(i)];
	s_pending_meta_clear.erase(it);
	return true;
}

// A sampled image with DCC (T# META_COMPRESS) whose metadata a uniform fill cleared after the image's last render pass: the hardware returns
// the clear colour for every cleared block, the pixel memory itself was never written (the game "clears" a transient layer it then leaves
// empty, and its bytes belong to whatever surface used that memory before - a G-buffer => NaN/white frames once composited). Returns a
// texture of the image's size and format filled with that colour, or nil. Constant codes only; 0x20 (clear register) uses the colour of a
// render target registered at that metadata, if any.
static id<MTLTexture> SampledMetaClear(const ShaderTextureResource& tex, uint64_t addr, id<MTLTexture> image) {
	if (!MetaClearEnabled() || image == nil || !tex.MetaCompress() || tex.MetaAddr() == 0) return nil;
	if (image.textureType != MTLTextureType2D) return nil; // the stand-in is a plain 2D texture
	switch (image.pixelFormat) {
		case MTLPixelFormatDepth16Unorm:
		case MTLPixelFormatDepth32Float:
		case MTLPixelFormatDepth32Float_Stencil8:
		case MTLPixelFormatStencil8:
		case MTLPixelFormatX32_Stencil8: return nil; // HTILE, not DCC: depth clears go through the depth-metadata path
		default: break;
	}
	const uint64_t meta = tex.MetaAddr() << 8u;
	std::array<float, 4> color {};
	uint32_t code = 0;
	{
		std::lock_guard<std::mutex> lock(s_meta_mutex);
		auto it = s_meta_fills.upper_bound(meta);
		if (it == s_meta_fills.begin()) return nil;
		--it;
		if (meta >= it->first + it->second.size) return nil;
		const auto rt = s_rt_pass_seq.find(addr);
		if (rt != s_rt_pass_seq.end() && rt->second > it->second.seq) return nil;
		code = it->second.code;
		MetaClearRt info {};
		info.format = image.pixelFormat;
		if (const auto known = s_meta_by_base.find(meta); known != s_meta_by_base.end()) info = known->second;
		if (code == 0x20u && s_meta_by_base.find(meta) == s_meta_by_base.end()) return nil;
		if (!DecodeMetaClearCode(info, code, color)) return nil;
	}
	const MTLPixelFormat fmt = image.pixelFormat;
	const bool zero = color[0] == 0.f && color[1] == 0.f && color[2] == 0.f && color[3] == 0.f;
	std::vector<uint8_t> px;
	size_t bpp = 0;
	if (fmt == MTLPixelFormatRGBA16Float) {
		bpp = 8;
		px.resize(8);
		for (int c = 0; c < 4; ++c) {
			const __fp16 h = static_cast<__fp16>(color[static_cast<size_t>(c)]);
			std::memcpy(&px[static_cast<size_t>(c) * 2], &h, 2);
		}
	} else if (fmt == MTLPixelFormatRGBA8Unorm || fmt == MTLPixelFormatRGBA8Unorm_sRGB) {
		bpp = 4;
		px.resize(4);
		for (int c = 0; c < 4; ++c) px[static_cast<size_t>(c)] = static_cast<uint8_t>(std::lround(std::clamp(color[static_cast<size_t>(c)], 0.f, 1.f) * 255.f));
	} else if (zero && BytesPerPixel(fmt) != 0) {
		bpp = BytesPerPixel(fmt);
		px.assign(bpp, 0);
	} else {
		return nil;
	}
	struct Key {
		NSUInteger w, h;
		MTLPixelFormat f;
		uint32_t code;
		bool operator<(const Key& o) const { return std::tie(w, h, f, code) < std::tie(o.w, o.h, o.f, o.code); }
	};
	static std::mutex s_mc_mutex;
	static std::map<Key, id<MTLTexture>> s_mc_cache;
	std::lock_guard<std::mutex> lock(s_mc_mutex);
	const Key key {image.width, image.height, fmt, code};
	if (auto it = s_mc_cache.find(key); it != s_mc_cache.end()) return it->second;
	if (s_mc_cache.size() >= 64) return nil;
	id<MTLDevice> dev = MetalContext::Instance().GetDevice();
	if (dev == nil) return nil;
	MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:image.width height:image.height mipmapped:NO];
	td.usage = MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeShared;
	id<MTLTexture> t = KytyNewTexture(dev, td, __LINE__);
	if (t == nil) return nil;
	std::vector<uint8_t> row(image.width * bpp);
	for (NSUInteger x = 0; x < image.width; ++x) std::memcpy(&row[x * bpp], px.data(), bpp);
	for (NSUInteger y = 0; y < image.height; ++y)
		[t replaceRegion:MTLRegionMake2D(0, y, image.width, 1) mipmapLevel:0 withBytes:row.data() bytesPerRow:row.size()];
	static std::atomic<uint32_t> s_log {0};
	if (s_log.fetch_add(1) < 20) {
		std::fprintf(stderr, "[MetaClear] flip=%llu sampled image 0x%llx %lux%lu fmt=%lu meta=0x%llx code=0x%02x -> color=(%g,%g,%g,%g)\n",
		             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)),
		             static_cast<unsigned long long>(addr), (unsigned long)image.width, (unsigned long)image.height, (unsigned long)fmt,
		             static_cast<unsigned long long>(meta), code, color[0], color[1], color[2], color[3]);
	}
	s_mc_cache[key] = t;
	return t;
}

// ---- Depth-metadata clears (HTILE fast clear) ----
// The game never clears depth with a DEPTH_CLEAR_ENABLE draw: it fills the depth target's HTILE metadata with a compute kernel and the
// hardware then reads cleared tiles as DB_DEPTH_CLEAR. The Vulkan backend treats a compute dispatch that only writes a registered HTILE
// range as that clear (RenderExecutor::TryConsumeComputeMetaClear -> TextureCache::ClearMeta) and reloads the depth clear value at the
// target's next pass. Metal never saw it, so depth survived from frame to frame (reversed-Z GEQUAL tests against last frame's depth).
namespace {
std::unordered_map<uint64_t, uint64_t> s_htile_by_base;   // HTILE base address -> depth surface (z read) address
std::unordered_map<uint64_t, PendingDepthClears> s_pending_depth_clear;       // depth surface addresses cleared through their HTILE since their last pass

bool HtileClearEnabled() {
	static const bool s_on = true;
	return s_on;
}
} // namespace

static void RegisterHtile(uint64_t htile_addr, uint64_t z_addr) {
	if (!HtileClearEnabled() || htile_addr == 0 || z_addr == 0) return;
	std::lock_guard<std::mutex> lock(s_meta_mutex);
	s_htile_by_base[htile_addr] = z_addr;
}

// A compute dispatch that writes (and does not read) a buffer at a registered HTILE base clears that depth target.
static void NoteHtileWrite(uint64_t base) {
	if (!HtileClearEnabled() || base == 0) return;
	std::lock_guard<std::mutex> lock(s_meta_mutex);
	const auto it = s_htile_by_base.find(base);
	if (it == s_htile_by_base.end()) return;
	s_pending_depth_clear[it->second].MarkAll();
	static std::atomic<uint32_t> s_log {0};
	if (s_log.fetch_add(1) < 40) {
		std::fprintf(stderr, "[MetaClear] flip=%llu htile write base=0x%llx -> depth=0x%llx queued\n",
		             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)),
		             static_cast<unsigned long long>(base), static_cast<unsigned long long>(it->second));
	}
}

static std::vector<uint32_t> TakePendingDepthClear(uint64_t z_addr, uint32_t first, uint32_t count) {
	if (!HtileClearEnabled()) return {};
	std::lock_guard<std::mutex> lock(s_meta_mutex);
	const auto it = s_pending_depth_clear.find(z_addr);
	return it == s_pending_depth_clear.end() ? std::vector<uint32_t>{} : it->second.Take(first,count);
}

// RT metadata (CMASK) written by the hardware CB: the game clears it with a compute fill, then a compute pass turns
// non-zero bytes into per-tile visibility bits that the composite consults. Metal never writes the metadata, so a
// draw into a CMASK-tracked target marks its tiles touched with a blit fill, in GPU order (after the game's clear).
// KYTY_LOG_CMASK=1 (diagnostic): the CMASK fill below writes a guessed number of bytes of guest memory. Print every distinct fill and
// warn when the range reaches the base of another render target or CMASK the game used (a fill overwriting somebody else's memory is
// a "false GPU write"). Never ask the guest memory manager from here: its lock can be held by a guest thread that waits for this one.
static void LogCmaskFill(uint64_t addr, size_t bytes, uint64_t rt_addr, uint32_t w, uint32_t h) {
	static std::mutex                        s_mutex;
	static std::set<std::pair<uint64_t, size_t>> s_seen;
	static std::set<uint64_t>                s_bases;   // render target and CMASK bases seen so far
	std::lock_guard<std::mutex> lock(s_mutex);
	s_bases.insert(rt_addr);
	if (!s_seen.insert({addr, bytes}).second) return;
	std::string overlaps;
	for (const uint64_t base: s_bases) {
		if (base > addr && base < addr + bytes) {
			char text[48];
			std::snprintf(text, sizeof(text), " 0x%llx", static_cast<unsigned long long>(base));
			overlaps += text;
		}
	}
	for (const auto& [other, other_bytes]: s_seen) {
		if (other > addr && other < addr + bytes) {
			char text[48];
			std::snprintf(text, sizeof(text), " cmask@0x%llx", static_cast<unsigned long long>(other));
			overlaps += text;
		}
	}
	std::fprintf(stderr, "[CmaskFill] cmask=0x%llx bytes=%zu rt=0x%llx %ux%u%s%s\n",
	             static_cast<unsigned long long>(addr), bytes, static_cast<unsigned long long>(rt_addr), w, h,
	             overlaps.empty() ? "" : "  WARNING reaches:", overlaps.c_str());
	s_bases.insert(addr);
}

static void MarkCmaskTouched(uint64_t addr, size_t bytes) {
	if (addr == 0 || bytes == 0) {
		return;
	}
	auto alloc = MetalBufferManager::Instance().GetBufferForGuestRange(addr, bytes);
	if (!alloc.buffer) {
		return;
	}
	// The fill used to end the render pass after every draw. It only has to land after the draws and before the next reader of the
	// metadata (a later compute pass), so it is queued and runs when the pass ends (KYTY_CMASK_SPLIT=1 restores the per-draw split).
	static const bool s_split = KYTY_ENV("KYTY_CMASK_SPLIT") != nullptr;
	if (!s_split) {
		MetalContext::Instance().QueueBlitFill(alloc.buffer, alloc.offset, bytes, 0xFF);
		return;
	}
	MetalContext::Instance().EndCurrentEncoder();
	id<MTLCommandBuffer> cb = MetalContext::Instance().GetCurrentCommandBuffer();
	if (!cb) {
		return;
	}
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	[blit fillBuffer:alloc.buffer range:NSMakeRange(alloc.offset, bytes) value:0xFF];
	[blit endEncoding];
}

// View caches keyed by the parent texture's address. A view keeps its parent alive, so a cache that is never trimmed keeps every
// texture the game ever bound in memory (tens of GB over a session). Entries are stamped with the presented-frame counter and the
// ones unused for kViewCacheEvictFlips frames are dropped (the key pointers stay valid while their entry lives: the view owns the parent).
static constexpr uint64_t kViewCacheEvictFlips = 90;
template <class Map>
static void SweepViewCache(Map& cache, uint64_t& last_sweep) {
	const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (flip < last_sweep + 30) return;
	last_sweep = flip;
	for (auto it = cache.begin(); it != cache.end();) {
		if (it->second.second + kViewCacheEvictFlips < flip) it = cache.erase(it);
		else ++it;
	}
}
static uint64_t ViewCacheNow() { return Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed); }

static id<MTLTexture> MipRangeView(id<MTLTexture> tex, NSUInteger base, NSUInteger count) {
	static std::mutex s_view_mutex;
	static std::map<std::tuple<void*, NSUInteger, NSUInteger>, std::pair<id<MTLTexture>, uint64_t>> s_views;
	static uint64_t s_views_sweep = 0;
	std::lock_guard<std::mutex> lock(s_view_mutex);
	SweepViewCache(s_views, s_views_sweep);
	const auto key = std::make_tuple((__bridge void*)tex, base, count);
	if (const auto it = s_views.find(key); it != s_views.end()) {
		it->second.second = ViewCacheNow();
		return it->second.first;
	}
	id<MTLTexture> view = [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:tex.textureType
	                                                  levels:NSMakeRange(base, count) slices:NSMakeRange(0, 1)];
	s_views[key] = {view, ViewCacheNow()};
	return view;
}

// The game reuses transient memory: one guest address can hold a full-size render target in one pass and a quarter-size
// image pyramid in another. A compute image whose size differs from every render target at its address is that other
// resource, so it gets its own (optionally mipmapped) texture instead of the render target's object.
static MTLTextureUsage GuestTextureUsage(MTLPixelFormat format);
struct AliasedImage {
	id<MTLTexture> tex = nil;
	uint32_t levels = 1;
	uint64_t used_flip = 0; // presented frame of the last request (entries unused for kAliasedEvictFlips frames are dropped)
};
static std::map<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t>, AliasedImage> s_aliased_images;
static std::map<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t>, uint32_t> s_aliased_level_req;
static std::mutex s_aliased_mutex;
// The key holds the size, and dynamic resolution moves the size of the transient targets every few frames (1600x904, 1624x912,
// 1640x920, ...): one mipped full-screen texture per size, never freed, passed 20 GB within 90 s of the 3D menu scene. Entries not
// requested for kAliasedEvictFlips presented frames are dropped (registries that still hold the texture keep it alive meanwhile).
static constexpr uint64_t kAliasedEvictFlips = 120;
static uint64_t s_alloc_cs_hash; // hash of the compute kernel being recorded (defined with the allocation probe)
static uint64_t s_cs_after_trigger_flip = UINT64_MAX; // KYTY_CS_BUF_AFTER_TRIGGER: the one flip being captured
static id<MTLTexture> AliasedImageFor(uint64_t addr, uint32_t w, uint32_t h, MTLPixelFormat fmt, uint32_t levels_wanted) {
	std::lock_guard<std::mutex> lock(s_aliased_mutex);
	const uint64_t now = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	static uint64_t s_aliased_sweep = 0;
	if (now >= s_aliased_sweep + 30) {
		s_aliased_sweep = now;
		for (auto it = s_aliased_images.begin(); it != s_aliased_images.end();) {
			if (it->second.used_flip + kAliasedEvictFlips < now) {
				s_aliased_level_req.erase(it->first);
				it = s_aliased_images.erase(it);
			} else {
				++it;
			}
		}
	}
	const auto key = std::make_tuple(addr, w, h, static_cast<uint32_t>(fmt));
	uint32_t& req = s_aliased_level_req[key];
	req = std::max(req, std::max<uint32_t>(levels_wanted, 1u));
	// A pyramid kernel chain asks for one more level per dispatch (2, 3, ... 11). Growing the texture level by level re-created it
	// for every dispatch and dropped the levels written so far (the next downsample read an empty level); dynamic resolution brings
	// a new size every few frames, so that happened all the time. The deepest chain ever asked for at this address and format sizes
	// a new texture from its first mipped request. (The textures cannot be grown with a copy here: the dispatch's compute encoder is
	// already open when its images are bound.)
	if (levels_wanted > 1) {
		static std::map<std::pair<uint64_t, uint32_t>, uint32_t> s_level_hint;
		uint32_t& hint = s_level_hint[{addr, static_cast<uint32_t>(fmt)}];
		hint = std::max(hint, levels_wanted);
		req = std::max(req, std::min<uint32_t>(hint, static_cast<uint32_t>(std::bit_width(std::max(w, h)))));
	}
	AliasedImage& img = s_aliased_images[key];
	if (img.tex == nil || img.levels < req) {
		MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:req > 1];
		if (req > 1) d.mipmapLevelCount = std::min<NSUInteger>(req, d.mipmapLevelCount);
		d.usage = GuestTextureUsage(fmt) | MTLTextureUsageShaderRead;
		d.storageMode = MTLStorageModeShared;
		img.tex = KytyNewTexture(MetalContext::Instance().GetDevice(), d, __LINE__);
		img.levels = req;
		std::fprintf(stderr, "[AliasImg] addr=0x%llx %ux%u fmt=%lu levels=%u cs=0x%016llx\n", (unsigned long long)addr, w, h, (unsigned long)fmt, req,
		             (unsigned long long)s_alloc_cs_hash);
	}
	img.used_flip = now;
	return img.tex;
}

// The aliased image of exactly this address, size and format that a kernel already used in the CURRENT presented frame: what a later kernel
// of the frame reads (a pyramid chain: one dispatch writes a level, the next reads it). The registries that normally find it hold one entry
// per ADDRESS, so a kernel that writes a bigger level at the same address (or a registry sweep) hides the level just written, and the
// reader fell back to decoding the stale guest memory (via=guest: min 1.9e-6, max 57344, NaN in the luminance pyramid input; the NaN then fed
// the exposure history and the whole frame turned into a black and white mosaic until the end of the session).
// KYTY_CS_NO_RECENT_ALIAS=1 turns the lookup off.
static id<MTLTexture> RecentAliasedImage(uint64_t addr, uint32_t w, uint32_t h, MTLPixelFormat fmt) {
	static const bool s_off = KYTY_ENV("KYTY_CS_NO_RECENT_ALIAS") != nullptr;
	if (s_off) return nil;
	std::lock_guard<std::mutex> lock(s_aliased_mutex);
	const auto it = s_aliased_images.find(std::make_tuple(addr, w, h, static_cast<uint32_t>(fmt)));
	if (it == s_aliased_images.end() || it->second.tex == nil) return nil;
	if (it->second.used_flip != Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)) return nil;
	return it->second.tex;
}

// T# DST_SEL (0 = zero, 1 = one, 4..7 = X,Y,Z,W) reorders or constant-fills channels when sampling; Metal has no notion
// of it, so a swizzled view of the texture is bound instead (e.g. BGRA selection of an RGBA8 image).
template <typename Tex>
static id<MTLTexture> ApplyDstSel(id<MTLTexture> texture, const Tex& tex) {
	if (texture == nil) {
		return nil;
	}
	const uint32_t sel[4] = {tex.DstSelX(), tex.DstSelY(), tex.DstSelZ(), tex.DstSelW()};
	if (sel[0] == 4 && sel[1] == 5 && sel[2] == 6 && sel[3] == 7) {
		return texture;
	}
	if (@available(macOS 15.0, *)) {
		const auto to_channel = [](uint32_t v, MTLTextureSwizzle identity) -> MTLTextureSwizzle {
			switch (v) {
				case 0: return MTLTextureSwizzleZero;
				case 1: return MTLTextureSwizzleOne;
				case 4: return MTLTextureSwizzleRed;
				case 5: return MTLTextureSwizzleGreen;
				case 6: return MTLTextureSwizzleBlue;
				case 7: return MTLTextureSwizzleAlpha;
				default: return identity;
			}
		};
		static std::mutex s_swz_mutex;
		static std::map<std::tuple<void*, uint32_t>, std::pair<id<MTLTexture>, uint64_t>> s_swz_views;
		static uint64_t s_swz_sweep = 0;
		const uint32_t key = sel[0] | (sel[1] << 4) | (sel[2] << 8) | (sel[3] << 12);
		std::lock_guard<std::mutex> lock(s_swz_mutex);
		SweepViewCache(s_swz_views, s_swz_sweep);
		const auto k = std::make_tuple((__bridge void*)texture, key);
		if (const auto it = s_swz_views.find(k); it != s_swz_views.end()) {
			it->second.second = ViewCacheNow();
			return it->second.first;
		}
		MTLTextureSwizzleChannels ch = MTLTextureSwizzleChannelsMake(
		    to_channel(sel[0], MTLTextureSwizzleRed), to_channel(sel[1], MTLTextureSwizzleGreen),
		    to_channel(sel[2], MTLTextureSwizzleBlue), to_channel(sel[3], MTLTextureSwizzleAlpha));
		id<MTLTexture> view = [texture newTextureViewWithPixelFormat:texture.pixelFormat textureType:texture.textureType
		                                                      levels:NSMakeRange(0, texture.mipmapLevelCount)
		                                                      slices:NSMakeRange(0, texture.arrayLength *
		                                                                                (texture.textureType == MTLTextureTypeCube ||
		                                                                                 texture.textureType == MTLTextureTypeCubeArray ? 6u : 1u))
		                                                     swizzle:ch];
		s_swz_views[k] = {view != nil ? view : texture, ViewCacheNow()};
		return s_swz_views[k].first;
	}
	return texture;
}

// Storage images a compute shader bound at a guest address, with a serial that grows on every bind. A draw that samples the
// same address as a surface of another layout (the UI icon kernel writes 128-bit block elements, the draw reads RGBA8)
// converts them through the shared tiled memory image; the serial tells when that conversion is stale.
struct CsStorageEntry {
	id<MTLTexture> tex = nil;
	uint64_t serial = 0;
	Prospero::TileMode tile = Prospero::TileMode::kLinear; // layout the guest declared for the surface
	uint64_t flip = 0;                                      // presented-frame counter when it was last written
	uint64_t guest_bytes = 0, guest_hash = 0;               // compute writers: hash of the guest bytes when bound (0: not taken)
	uint64_t checked_flip = 0;                              // AliasConvert: flip of the last "guest bytes unchanged" check
	bool     guest_same = false;
	PhysicalBackingSnapshot canonical;
};
static uint64_t GuestTexSourceHash(uint64_t addr, uint64_t bytes);
static std::mutex s_cs_storage_mutex;
static std::unordered_map<uint64_t, CsStorageEntry> s_cs_storage;
static uint64_t s_cs_storage_serial = 0;
// Last GPU writer of a guest address (compute storage image or render target): the surface converter uses it as the source of
// truth when a later pass reads the same memory with another format, size or tile layout.
struct RecentSurfaceOp { const char* op; uint64_t addr, bytes, flip, hash; };
static std::mutex s_recent_surface_mutex;
static std::deque<RecentSurfaceOp> s_recent_surface_ops;
static void TraceRangeOp(const char* op, uint64_t addr, uint64_t bytes);
// hash_guest (compute storage bindings, not the per-draw render-target calls): also hash the guest bytes behind the image, so a reader
// many frames later can tell whether the CPU has replaced them since (AliasConvert).
static uint64_t RecordComputeStorage(uint64_t addr, id<MTLTexture> tex, Prospero::TileMode tile = Prospero::TileMode::kLinear, bool hash_guest = false,
                                     const PhysicalBackingSnapshot* canonical=nullptr) {
	NoteSurfaceWrite(addr, hash_guest);
	const bool depth_array = tex != nil && tex.textureType == MTLTextureType2DArray &&
	    (tex.pixelFormat == MTLPixelFormatDepth16Unorm || tex.pixelFormat == MTLPixelFormatDepth32Float);
	const bool canonical_array=tex!=nil && (tex.textureType==MTLTextureType2DArray || tex.textureType==MTLTextureType3D) && canonical && canonical->Valid();
	if (addr == 0 || tex == nil || (tex.textureType != MTLTextureType2D && !depth_array && !canonical_array)) return 0;
	TraceRangeOp("storage_write", addr, static_cast<uint64_t>(tex.width) * tex.height * 4);
	const uint64_t bytes = hash_guest && !IsCompressedFormat(tex.pixelFormat) ? static_cast<uint64_t>(tex.width) * tex.height * BytesPerPixel(tex.pixelFormat) : 0;
	const uint64_t hash = bytes != 0 ? GuestTexSourceHash(addr, bytes) : 0;
	std::lock_guard<std::mutex> lock(s_cs_storage_mutex);
	CsStorageEntry& e = s_cs_storage[addr];
	e.tex = tex;
	e.serial = ++s_cs_storage_serial;
	e.tile = tile;
	e.flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	e.guest_bytes = hash != 0 ? bytes : 0;
	e.guest_hash = hash;
	e.checked_flip = 0;
	e.canonical=canonical?*canonical:PhysicalBackingSnapshot{};
	return e.serial;
}

// The newest GPU writer (render-target pass or compute storage binding) of a guest address. A sampled compute image must read what
// that writer produced, not an older object the registry still holds for the address: the game reuses addresses for unrelated
// surfaces within one frame (a compute-written image earlier, the lit scene render target later), and a stale entry fed the tonemap
static id<MTLTexture> LastGpuWriterTexture(uint64_t addr, uint64_t* out_flip = nullptr) {
	static const bool s_off = false;
	if (s_off || addr == 0) return nil;
	std::lock_guard<std::mutex> lock(s_cs_storage_mutex);
	const auto it = s_cs_storage.find(addr);
	if (it == s_cs_storage.end()) return nil;
	if (out_flip != nullptr) *out_flip = it->second.flip;
	return it->second.tex;
}

// A render target is a view over the guest pages, a compute storage image is a texture of its own: when the game writes an address with
// a kernel and then renders into the same memory as a target of another format (FC 27 tile passes: R32Uint and RGBA8 kernels, then an
// R11G11B10 target over 0x8243fb0000), the target saw bytes from other passes or frames instead of the kernel's results - the fog
// composite added that garbage as an orange/blue haze over whole scenes. Before such a pass the newest kernel output is copied into the
// KYTY_LOG_ALIAS_SYNC=1 logs the first copies.
static void SyncStorageIntoGuestTarget(uint64_t addr, id<MTLTexture> target) {
	static const bool s_off = false;
	static const bool s_log = KYTY_ENV("KYTY_LOG_ALIAS_SYNC") != nullptr;
	if (s_off || addr == 0 || target == nil || target.buffer == nil || IsCompressedFormat(target.pixelFormat)) return;
	static std::unordered_map<uint64_t, uint64_t> s_synced; // address -> serial of the storage entry already copied
	CsStorageEntry wr;
	{
		std::lock_guard<std::mutex> lock(s_cs_storage_mutex);
		const auto it = s_cs_storage.find(addr);
		if (it == s_cs_storage.end()) return;
		wr = it->second;
	}
	if (wr.tex == nil || wr.tex == target || wr.tex.buffer != nil || wr.tex.textureType != MTLTextureType2D || wr.tex.mipmapLevelCount != 1 ||
	    IsCompressedFormat(wr.tex.pixelFormat) || wr.tex.width != target.width || wr.tex.height != target.height ||
	    BytesPerPixel(wr.tex.pixelFormat) != BytesPerPixel(target.pixelFormat))
		return;
	const uint64_t now_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (now_flip > wr.flip + 2) return;
	uint64_t& done = s_synced[addr];
	if (done == wr.serial) return;
	done = wr.serial;
	MetalContext::Instance().EndCurrentEncoder();
	id<MTLCommandBuffer> cb = MetalContext::Instance().GetCurrentCommandBuffer();
	if (!cb) return;
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	OrderBegin(blit);
	const NSUInteger row = target.bufferBytesPerRow;
	[blit copyFromTexture:wr.tex
	                 sourceSlice:0
	                 sourceLevel:0
	                sourceOrigin:MTLOriginMake(0, 0, 0)
	                  sourceSize:MTLSizeMake(target.width, target.height, 1)
	                    toBuffer:target.buffer
	           destinationOffset:target.bufferOffset
	      destinationBytesPerRow:row
	    destinationBytesPerImage:row * target.height];
	OrderEnd(blit);
	[blit endEncoding];
	static std::atomic<uint32_t> s_logged {0};
	if (s_log && s_logged.fetch_add(1) < 200)
		std::fprintf(stderr, "[AliasSync] flip=%llu addr=0x%llx %lux%lu storage fmt=%lu -> target fmt=%lu (written flip %llu)\n",
		             (unsigned long long)now_flip, (unsigned long long)addr, (unsigned long)target.width, (unsigned long)target.height,
		             (unsigned long)wr.tex.pixelFormat, (unsigned long)target.pixelFormat, (unsigned long long)wr.flip);
}

// Drop the registry entry of `addr` when it still holds `tex` (the surface behind it is being freed).
static void ForgetGpuWriter(uint64_t addr, id<MTLTexture> tex) {
	std::lock_guard<std::mutex> lock(s_cs_storage_mutex);
	const auto it = s_cs_storage.find(addr);
	if (it != s_cs_storage.end() && it->second.tex == tex) s_cs_storage.erase(it);
}

// Same bytes, different pixel format of the same size (compute writes R8Uint, a draw samples R8Unorm): a cached
// pixel-format view of the object, so both sides keep sharing the memory.
static id<MTLTexture> ReinterpretFormat(id<MTLTexture> cand, MTLPixelFormat want) {
	if (cand == nil || want == MTLPixelFormatInvalid || want == cand.pixelFormat ||
	    IsCompressedFormat(want) || IsCompressedFormat(cand.pixelFormat) || BytesPerPixel(want) != BytesPerPixel(cand.pixelFormat)) {
		return cand;
	}
	// Render targets are linear textures over guest memory (a wrapped MTLBuffer), which carry no PixelFormatView usage: a second
	// linear texture of the wanted format over the same buffer shares the memory just as well (and a raw 32-bit copy kernel,
	// declared uint, then really sees the colour bits instead of an RGBA8 float texture it cannot read).
	const bool buffer_backed = cand.buffer != nil;
	if (!buffer_backed && (cand.usage & MTLTextureUsagePixelFormatView) == 0) {
		return cand;
	}
	// A view keeps its parent texture alive, and this cache used to keep every view forever: every texture that was ever
	// reinterpreted (detile copies, render targets, aliased images) stayed in memory for the whole run - the 50-100 GB of
	// growth per instance. Entries not asked for during kViewEvictFlips presented frames are dropped.
	struct CachedView {
		id<MTLTexture> view = nil;
		uint64_t       used_flip = 0;
	};
	static std::mutex s_fv_mutex;
	static std::map<std::pair<void*, uint32_t>, CachedView> s_fv;
	static uint64_t s_fv_sweep_flip = 0;
	constexpr uint64_t kViewEvictFlips = 90;
	std::lock_guard<std::mutex> lock(s_fv_mutex);
	const uint64_t fv_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (fv_flip >= s_fv_sweep_flip + 30) {
		s_fv_sweep_flip = fv_flip;
		for (auto it = s_fv.begin(); it != s_fv.end();) {
			if (it->second.used_flip + kViewEvictFlips < fv_flip) it = s_fv.erase(it);
			else ++it;
		}
	}
	const auto key = std::make_pair((__bridge void*)cand, static_cast<uint32_t>(want));
	if (const auto it = s_fv.find(key); it != s_fv.end()) {
		it->second.used_flip = fv_flip;
		return it->second.view;
	}
	id<MTLTexture> view = nil;
	if (buffer_backed) {
		MTLTextureDescriptor* vd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:want width:cand.width height:cand.height mipmapped:NO];
		vd.usage = GuestTextureUsage(want);
		vd.storageMode = cand.buffer.storageMode;
		view = [cand.buffer newTextureWithDescriptor:vd offset:cand.bufferOffset bytesPerRow:cand.bufferBytesPerRow];
		if (view == nil) {
			std::fprintf(stderr, "[Reinterpret] buffer-backed view failed: %lux%lu fmt %lu -> %lu rowbytes=%lu\n", (unsigned long)cand.width,
			             (unsigned long)cand.height, (unsigned long)cand.pixelFormat, (unsigned long)want, (unsigned long)cand.bufferBytesPerRow);
		}
	} else {
		// A full-chain view preserves every mip. Raw uint copy/clear kernels also alias mipped colour targets.
		view = [cand newTextureViewWithPixelFormat:want];
	}
	s_fv[key] = {view != nil ? view : cand, fv_flip};
	return s_fv[key].view;
}

static bool IsDepthStencilPixelFormat(MTLPixelFormat f) {
	switch (f) {
		case MTLPixelFormatDepth16Unorm:
		case MTLPixelFormatDepth32Float:
		case MTLPixelFormatStencil8:
		case MTLPixelFormatDepth32Float_Stencil8:
		case MTLPixelFormatX32_Stencil8: return true;
		default: return false;
	}
}

// The registry remembers the last texture bound at an address, whatever its format. A registered texture of another texel size is
// another resource that merely lived at the same address, so it must not be reused for this descriptor.
static bool SameTexelSize(id<MTLTexture> t, MTLPixelFormat want) {
	if (t == nil || want == MTLPixelFormatInvalid) return true;
	if (t.pixelFormat == want) return true;
	// BytesPerPixel is 0 for every block-compressed format, so BC1 and BC5 compared "equal" and a texture of another BC format (an
	// earlier image at a reused streaming-pool address) was handed out unchanged: ReinterpretFormat cannot view across BC formats.
	if (IsCompressedFormat(t.pixelFormat) || IsCompressedFormat(want)) return false;
	return BytesPerPixel(t.pixelFormat) == BytesPerPixel(want);
}

// ---- GPU-produced surfaces as views over guest memory ----
// The game aliases transient memory heavily (one address is a depth buffer, a half-resolution image and an HDR target at different times of a
// frame), and the hardware has no resource objects, only bytes. Render targets already are linear views over the guest pages; compute
// images used to be separate textures chosen by "last writer" heuristics, so a reader could pick another object than the writer wrote
// (stale render-target memory: NaN, zeros). GPU-produced images are views over the same memory now: whatever format or size a later pass
// uses on those bytes, it reads exactly what was written. CPU-provided assets (tiled data the GPU never wrote) keep their decoded copies.
static void NoteLinearRenderAddress(uint64_t addr); // defined with the render-target registry
struct GpuSurface {
	id<MTLTexture> tex       = nil;
	id<MTLBuffer>  buf       = nil; // keeps the wrapper of the guest pages alive with the view
	uint64_t       used_flip = 0;
};
static std::mutex                                                                    s_gpusurf_mutex;
static std::map<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>, GpuSurface> s_gpusurf;
static uint64_t                                                                      s_gpusurf_sweep = 0;
// GPU writes are matched by their exact start address: a read surface that begins where a GPU pass wrote one is the same transient
// resource (or an alias of it). Merged byte ranges were tried first and were too generous: adjacent render targets fused into pool-sized
// blocks, so tiled CPU textures that merely lie inside such a pool were taken for GPU data and read as linear pixels (coloured noise).
struct GpuWriteStamp {
	uint64_t bytes = 0;
	uint64_t flip  = 0; // presented-frame counter of the newest write that started at this address
};
static std::unordered_map<uint64_t, GpuWriteStamp> s_gpu_written; // start address -> newest write
// Diagnostic comparison (KYTY_UMA_RANGE=overlap): merged ranges as used by the first version.
struct GpuMergedRange {
	uint64_t end  = 0;
	uint64_t flip = 0;
};
static std::map<uint64_t, GpuMergedRange> s_gpu_written_merged;

static void NoteGpuWrite(uint64_t addr, uint64_t bytes) {
	if (addr == 0 || bytes == 0) return;
	const uint64_t now_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(s_gpusurf_mutex);
	GpuWriteStamp& e = s_gpu_written[addr];
	e.bytes          = std::max(e.bytes, bytes);
	e.flip           = now_flip;
	{
		uint64_t start = addr, end = addr + bytes;
		auto     it    = s_gpu_written_merged.lower_bound(start);
		if (it != s_gpu_written_merged.begin()) {
			auto prev = std::prev(it);
			if (prev->second.end >= start) { start = prev->first; end = std::max(end, prev->second.end); it = s_gpu_written_merged.erase(prev); }
		}
		while (it != s_gpu_written_merged.end() && it->first <= end) { end = std::max(end, it->second.end); it = s_gpu_written_merged.erase(it); }
		s_gpu_written_merged[start] = {end, now_flip};
	}
	static uint64_t s_sweep = 0;
	if (now_flip >= s_sweep + 300) {
		s_sweep = now_flip;
		for (auto it = s_gpu_written.begin(); it != s_gpu_written.end();) {
			if (it->second.flip + 600 < now_flip) it = s_gpu_written.erase(it);
			else ++it;
		}
	}
}
// 2: a GPU write started exactly at `addr`; 1: only inside a merged written range (KYTY_UMA_RANGE=overlap); 0: not GPU-written (or too old).
// `max_age` presented frames: memory the GPU wrote longer ago may have been reused by the CPU since (streamed assets, freed pools), which
// the GPU-write table cannot see; those reads fall back to the per-object lookups (they still reach the same texture through the registries).
static int GpuWrittenKind(uint64_t addr, uint64_t bytes, uint64_t max_age = 4) {
	static const bool s_overlap_mode = [] { const char* v = KYTY_ENV("KYTY_UMA_RANGE"); return v != nullptr && std::strcmp(v, "overlap") == 0; }();
	const uint64_t now_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(s_gpusurf_mutex);
	const auto it = s_gpu_written.find(addr);
	if (it != s_gpu_written.end() && it->second.flip + max_age >= now_flip) return 2;
	if (!s_overlap_mode) return 0;
	auto mit = s_gpu_written_merged.upper_bound(addr + bytes - 1);
	if (mit == s_gpu_written_merged.begin()) return 0;
	--mit;
	return mit->second.end > addr && mit->second.flip + max_age >= now_flip ? 1 : 0;
}
static bool GpuWrittenOverlaps(uint64_t addr, uint64_t bytes, uint64_t max_age = 4) { return GpuWrittenKind(addr, bytes, max_age) != 0; }

static Prospero::BufferFormat RepresentativeFormat(uint32_t element_bytes);

// Row stride of a linear host view of a guest surface. A guest surface is identified by its address and its padded layout, not by the visible
// size of one descriptor: a compute kernel declares the 1600x904 image of a surface a render pass declares as 1616x912, and both share the
// pitch the tiling rules give (1664 texels here). Views of one surface therefore take the guest pitch, so every descriptor of it meets in
// the same bytes. KYTY_UMA_PITCH=tight restores the old rule (visible width only) for comparison.
static size_t GuestViewRowBytes(MTLPixelFormat fmt, uint32_t w, Prospero::TileMode tile) {
	const uint32_t bpp = BytesPerPixel(fmt);
	if (bpp == 0) return 0;
	id<MTLDevice> device = MetalContext::Instance().GetDevice();
	NSUInteger align = LinearTextureRowAlignment(device, fmt);
	if (align == 0) align = 256;
	if (align < 64) align = 64;
	static const bool s_tight = [] { const char* v = KYTY_ENV("KYTY_UMA_PITCH"); return v != nullptr && std::strcmp(v, "tight") == 0; }();
	uint32_t pitch = w;
	if (!s_tight && bpp <= 16) pitch = std::max(w, TileGetTexturePitch(RepresentativeFormat(bpp), w, tile));
	return (static_cast<size_t>(pitch) * bpp + align - 1) & ~(static_cast<size_t>(align) - 1);
}

// A view of `w` x `h` texels of `fmt` over the guest memory at `addr`, laid out like the render-target views (rows at the guest pitch, see
// GuestViewRowBytes). `for_write` records the range as GPU-written; a read only gets a view when the GPU wrote that memory.
static id<MTLTexture> GpuSurfaceFor(uint64_t addr, uint32_t w, uint32_t h, MTLPixelFormat fmt, Prospero::TileMode tile, bool for_write) {
	if (addr == 0 || w == 0 || h == 0 || w > 16384 || h > 16384 || fmt == MTLPixelFormatInvalid || IsCompressedFormat(fmt)) return nil;
	const uint32_t bpp = BytesPerPixel(fmt);
	if (bpp == 0) return nil;
	const size_t bytes_per_row = GuestViewRowBytes(fmt, w, tile);
	const size_t total_bytes   = bytes_per_row * h;
	if (!for_write && !GpuWrittenOverlaps(addr, total_bytes)) return nil;
	const auto allocation = MetalBufferManager::Instance().GetBufferForGuestRange(addr, total_bytes);
	if (!allocation.IsValid()) return nil;
	const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	id<MTLTexture> result = nil;
	id<MTLTexture> retired = nil;
	{
		std::lock_guard<std::mutex> lock(s_gpusurf_mutex);
		if (flip >= s_gpusurf_sweep + 30) {
			s_gpusurf_sweep = flip;
			for (auto sit = s_gpusurf.begin(); sit != s_gpusurf.end();) {
				if (sit->second.used_flip + kViewCacheEvictFlips < flip) sit = s_gpusurf.erase(sit);
				else ++sit;
			}
		}
		const auto key = std::make_tuple(addr, w, h, static_cast<uint32_t>(fmt), static_cast<uint32_t>(bytes_per_row));
		const auto it  = s_gpusurf.find(key);
		if (it != s_gpusurf.end()) {
			if (GuestMemoryViewMatches(it->second.tex, allocation, fmt, w, h, bytes_per_row)) {
				it->second.used_flip = flip;
				result = it->second.tex;
			} else {
				retired = it->second.tex;
				s_gpusurf.erase(it);
			}
		}
	}
	if (retired != nil) ForgetGpuWriter(addr, retired);
	if (result == nil) {
		MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
		td.usage       = GuestTextureUsage(fmt);
		td.storageMode = MTLStorageModeShared;
		result         = CreateGuestMemoryView(allocation, td, bytes_per_row);
		if (result == nil) return nil;
		std::lock_guard<std::mutex> lock(s_gpusurf_mutex);
		s_gpusurf[std::make_tuple(addr, w, h, static_cast<uint32_t>(fmt), static_cast<uint32_t>(bytes_per_row))] = {result, allocation.buffer, flip};
	}
	if (for_write) {
		NoteGpuWrite(addr, total_bytes);
		NoteLinearRenderAddress(addr); // a later sampler must not detile this linear data
	}
	return result;
}

// The GPU surfaces (views over guest memory) whose byte range contains [addr, addr+bytes), as text; empty when none. Diagnostic: guest data
// that a game keeps in memory it also uses as a render target (indirect dispatch arguments) is overwritten by whatever writes that surface.
static std::string GpuSurfacesCovering(uint64_t addr, uint64_t bytes) {
	std::string out;
	char        line[160];
	std::lock_guard<std::mutex> lock(s_gpusurf_mutex);
	int shown = 0;
	for (const auto& [key, surface]: s_gpusurf) {
		const uint64_t start = std::get<0>(key);
		const uint64_t size  = static_cast<uint64_t>(std::get<4>(key)) * std::get<2>(key);
		if (addr + bytes <= start || addr >= start + size) continue;
		if (++shown > 3) break;
		std::snprintf(line, sizeof(line), "%s0x%llx %ux%u fmt=%u bpr=%u +0x%llx used_flip=%llu", out.empty() ? "" : " | ", (unsigned long long)start,
		              std::get<1>(key), std::get<2>(key), std::get<3>(key), std::get<4>(key), (unsigned long long)(addr - start),
		              (unsigned long long)surface.used_flip);
		out += line;
	}
	return out;
}

// KYTY_RT_PRIVATE=1: colour render targets are private textures keyed by (address, format, size) instead of views of guest memory.
static bool PrivateRenderTargets() {
	static const bool on = KYTY_ENV("KYTY_RT_PRIVATE") != nullptr && KYTY_ENV("KYTY_RT_PRIVATE")[0] == '1';
	return on;
}

// KYTY_UMA_SURFACES=1: GPU-produced images are views over guest memory (compute and graphics readers alike). Opt-in until validated.
static bool UmaSurfacesEnabled() {
	static const bool on = KYTY_ENV("KYTY_UMA_SURFACES") != nullptr && KYTY_ENV("KYTY_NO_UMA_SURFACES") == nullptr;
	return on;
}

// The newest render target at a guest address with the sampled surface's size and texel size (defined with the RT registry below).
static id<MTLTexture> NewestRenderTargetFor(uint64_t addr, uint32_t w, uint32_t h, MTLPixelFormat want);
static bool s_draw_writes_depth = true; // the draw whose images are being bound has depth writes on (render thread only)

// A kernel that declares an array image (cube faces are sampled as 2D-array slices) must get an array texture; a plain 2D
// object (a render target aliasing the cube memory) is shown as a one-slice array view of itself.
static id<MTLTexture> ConformToDeclaredArray(id<MTLTexture> tex, bool declared_array) {
	if (tex == nil || !declared_array || tex.textureType != MTLTextureType2D) return tex;
	static std::mutex s_arr_mutex;
	static std::map<void*, std::pair<id<MTLTexture>, uint64_t>> s_arr;
	static uint64_t s_arr_sweep = 0;
	std::lock_guard<std::mutex> lock(s_arr_mutex);
	SweepViewCache(s_arr, s_arr_sweep);
	const auto it = s_arr.find((__bridge void*)tex);
	if (it != s_arr.end()) { it->second.second = ViewCacheNow(); return it->second.first; }
	id<MTLTexture> v = [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:MTLTextureType2DArray
	                                               levels:NSMakeRange(0, tex.mipmapLevelCount) slices:NSMakeRange(0, 1)];
	s_arr[(__bridge void*)tex] = {v != nil ? v : tex, ViewCacheNow()};
	return s_arr[(__bridge void*)tex].first;
}

// ---- Layered GPU images (cube faces, array slices) ----
// Render targets are 2D views over guest memory and storage images 2D textures, so the six faces of a cube the game renders slice by
// slice (FC 27's environment capture: `0x81b8db0000` 256x256 RGBA16F, slices 0..5, then prefiltered by compute into the 6 faces and
// mips of the ambient cube `0x8208000000`) all landed in one 2D texture: the prefilter integrated one face and five black ones, the
// ambient light was ~1e-4 and outdoor ground black. A target bound with an array slice > 0, or a storage image declared as a cube or
// array, now lives in one 2D-array texture per address (all slices and mips); render passes use a 2D view of their slice, readers a
struct LayeredImage {
	id<MTLTexture> tex = nil;
	uint32_t       w = 0, h = 0, slices = 0, levels = 0;
};
static std::mutex                                   s_layered_mutex;
static std::unordered_map<uint64_t, LayeredImage>   s_layered;
static bool LayeredImagesEnabled() {
	static const bool on = true;
	return on;
}
static bool LayeredImageExists(uint64_t addr, uint32_t w, uint32_t h) {
	if (!LayeredImagesEnabled()) return false;
	std::lock_guard<std::mutex> lock(s_layered_mutex);
	const auto it = s_layered.find(addr);
	return it != s_layered.end() && it->second.w == w && it->second.h == h;
}
static uint32_t LayeredSlicesFor(const ShaderTextureResource& tex, uint64_t addr, uint32_t w, uint32_t h, bool cube, bool array) {
	const uint32_t base_slice = (cube || array) ? tex.BaseArray5() : 0u;
	const uint32_t depth      = (cube || array) ? static_cast<uint32_t>(tex.Depth()) + 1u : 1u;
	uint32_t slices = cube ? std::max<uint32_t>(6u, std::max<uint32_t>(depth, base_slice + 6u))
	                       : std::max<uint32_t>(std::max<uint32_t>(depth, base_slice + 1u), (w == 256 && h == 32 ? 20u : 1u));
	static std::unordered_map<uint64_t, uint32_t> s_max_declared_slices;
	static std::mutex s_max_slices_mutex;
	std::lock_guard<std::mutex> lk(s_max_slices_mutex);
	uint32_t& max_s = s_max_declared_slices[addr];
	max_s = std::max(max_s, slices);
	return max_s;
}
// The array texture of `addr` with at least `slices` slices and `levels` mips, created or grown. `can_copy` (no encoder open): a grown
// texture keeps the old slices, and a new one receives `seed` (the 2D target that held slice 0 so far) in slice 0.
static id<MTLTexture> LayeredImageEnsure(uint64_t addr, uint32_t w, uint32_t h, MTLPixelFormat fmt, uint32_t slices, uint32_t levels,
                                         bool can_copy, id<MTLTexture> seed, const ShaderTextureResource* guest = nullptr) {
	if (!LayeredImagesEnabled() || fmt == MTLPixelFormatInvalid || IsCompressedFormat(fmt) || w == 0 || h == 0) return nil;
	std::lock_guard<std::mutex> lock(s_layered_mutex);
	LayeredImage& e = s_layered[addr];
	const bool same_size = e.tex != nil && e.w == w && e.h == h && BytesPerPixel(e.tex.pixelFormat) == BytesPerPixel(fmt);
	if (same_size && e.slices >= slices && e.levels >= levels) return e.tex;
	const uint32_t max_levels = 1u + static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(std::max(w, h)))));
	uint32_t new_slices = std::max<uint32_t>(same_size ? e.slices : 0u, std::max<uint32_t>(slices, 1u));
	// The whole mip chain from the start: the prefilter kernels write the mips one dispatch at a time, and a storage binding cannot copy
	// the old texture into a grown one (its compute encoder is open), so growing by level lost every face written before.
	(void)levels;
	const uint32_t new_levels = max_levels;
	MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
	td.textureType      = MTLTextureType2DArray;
	td.pixelFormat      = same_size ? e.tex.pixelFormat : fmt;
	td.width            = w;
	td.height           = h;
	td.arrayLength      = new_slices;
	td.mipmapLevelCount = new_levels;
	td.usage            = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView |
	           (GuestTextureUsage(td.pixelFormat) & MTLTextureUsageShaderWrite);
	td.storageMode      = MTLStorageModeShared;
	id<MTLTexture> tex = KytyNewTexture(MetalContext::Instance().GetDevice(), td, __LINE__);
	if (tex == nil) return e.tex;
	id<MTLTexture> old = same_size ? e.tex : nil;
	if (can_copy && guest != nullptr) {
		// A sampled array is guest data, not an empty GPU allocation. Import its
		// complete tiled snapshot before any queued writer/seed copy overrides it.
		const uint32_t guest_levels = std::max<uint32_t>(guest->LastLevel(), guest->MaxMip()) + 1u;
		const uint32_t guest_layers = static_cast<uint32_t>(guest->Depth()) + 1u;
		TileSurfaceLayout layout {};
		const TileSurfaceDescription description {guest->Format(), guest->TileMode(), TileSurfaceDimension::Dim2D,
		                                           w, h, 1u, guest_levels, guest_layers};
		if (!TileGetTiledTextureLayout(description, layout) || layout.total_size > (256ull << 20)) {
			FailMetalMaterialization("layered guest layout", static_cast<uint32_t>(guest->Format()));
			return nil;
		}
		std::vector<uint8_t> source(layout.total_size);
		if (!LibKernel::Memory::TryReadBacking(addr, source.data(), source.size())) {
			FailMetalMaterialization("layered guest snapshot unreadable", static_cast<uint32_t>(guest->Format()));
			return nil;
		}
		const MTLPixelFormat element_format = TileElementFormat(layout.texture.block.bytes_per_element);
		id<MTLTexture> elements = tex.pixelFormat == element_format ? tex : [tex newTextureViewWithPixelFormat:element_format];
		if (!UploadTiledArrayMipChain(source, layout, elements)) {
			FailMetalMaterialization("layered guest upload", static_cast<uint32_t>(guest->Format()));
			return nil;
		}
		const size_t nonzero = std::count_if(source.begin(), source.end(), [](uint8_t byte) { return byte != 0; });
		std::fprintf(stderr, "[LayeredImport] addr=0x%llx %ux%u fmt=%lu layers=%u levels=%u bytes=%zu nonzero=%zu\n",
		             static_cast<unsigned long long>(addr), w, h, static_cast<unsigned long>(fmt),
		             guest_layers, guest_levels, source.size(), nonzero);
	}
	if (!can_copy && old != nil) {
		// An encoder is open (storage binding): the old slices are copied when it ends; the open dispatch writes only new slices.
		MetalContext::Instance().QueueTextureCopy(old, tex, static_cast<uint32_t>(old.arrayLength),
		                                          static_cast<uint32_t>(std::min<NSUInteger>(old.mipmapLevelCount, tex.mipmapLevelCount)));
	}
	if (can_copy && (old != nil || seed != nil)) {
		MetalContext::Instance().EndCurrentEncoder();
		if (id<MTLCommandBuffer> cb = MetalContext::Instance().GetCurrentCommandBuffer()) {
			id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
			OrderBegin(blit);
			if (old != nil) {
				const NSUInteger copy_levels = std::min<NSUInteger>(old.mipmapLevelCount, tex.mipmapLevelCount);
				[blit copyFromTexture:old sourceSlice:0 sourceLevel:0 toTexture:tex destinationSlice:0 destinationLevel:0
				           sliceCount:old.arrayLength levelCount:copy_levels];
			} else if (seed.pixelFormat == tex.pixelFormat && seed.width == w && seed.height == h) {
				[blit copyFromTexture:seed sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
				            toTexture:tex destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
			}
			OrderEnd(blit);
			[blit endEncoding];
		}
	}
	std::fprintf(stderr, "[Layered] addr=0x%llx %ux%u fmt=%lu slices=%u levels=%u%s\n", (unsigned long long)addr, w, h,
	             (unsigned long)td.pixelFormat, new_slices, new_levels, old != nil ? " (grown)" : "");
	e = {tex, w, h, new_slices, new_levels};
	return tex;
}
// A view of the array texture: `type` 2D / 2DArray / Cube / CubeArray over [base_slice, +slices) and [base_level, +levels), in `fmt`
// (same texel size). Views are cached per (texture, shape) and dropped when not asked for during 90 presented frames.
static id<MTLTexture> LayeredView(id<MTLTexture> arr, MTLPixelFormat fmt, MTLTextureType type, uint32_t base_level, uint32_t levels,
                                  uint32_t base_slice, uint32_t slices) {
	if (arr == nil || base_level >= arr.mipmapLevelCount || base_slice >= arr.arrayLength) return nil;
	levels = std::max<uint32_t>(1u, std::min<uint32_t>(levels, static_cast<uint32_t>(arr.mipmapLevelCount) - base_level));
	slices = std::max<uint32_t>(1u, std::min<uint32_t>(slices, static_cast<uint32_t>(arr.arrayLength) - base_slice));
	if ((type == MTLTextureTypeCube && slices != 6) || (type == MTLTextureTypeCubeArray && slices % 6 != 0)) return nil;
	if (type == MTLTextureType2D && slices != 1) slices = 1;
	if (BytesPerPixel(fmt) != BytesPerPixel(arr.pixelFormat) || IsCompressedFormat(fmt)) fmt = arr.pixelFormat;
	using Key = std::tuple<void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
	struct Cached { id<MTLTexture> view = nil; uint64_t used = 0; };
	static std::mutex s_m;
	static std::map<Key, Cached> s_views;
	static uint64_t s_sweep = 0;
	std::lock_guard<std::mutex> lock(s_m);
	const uint64_t now = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (now >= s_sweep + 30) {
		s_sweep = now;
		for (auto it = s_views.begin(); it != s_views.end();) it = it->second.used + 90 < now ? s_views.erase(it) : std::next(it);
	}
	const Key key {(__bridge void*)arr, static_cast<uint32_t>(fmt), static_cast<uint32_t>(type), base_level, levels, base_slice, slices};
	if (auto it = s_views.find(key); it != s_views.end()) {
		it->second.used = now;
		return it->second.view;
	}
	id<MTLTexture> view = [arr newTextureViewWithPixelFormat:fmt textureType:type levels:NSMakeRange(base_level, levels)
	                                                  slices:NSMakeRange(base_slice, slices)];
	s_views[key] = {view, now};
	return view;
}
// KYTY_LAYERED_STATS=<n>: every n presented frames, the mean and max of level 0 of each slice of every RGBA16F layered image (waits for
// the GPU; diagnostic only). Shows whether the ambient cube array holds the captured probes.
static float DumpHalfToFloat(uint16_t h);
static void LayeredStats() {
	static const uint64_t every = KYTY_ENV("KYTY_LAYERED_STATS") != nullptr ? std::max<uint64_t>(1u, std::strtoull(KYTY_ENV("KYTY_LAYERED_STATS"), nullptr, 0)) : 0u;
	if (every == 0) return;
	static uint64_t s_last = 0;
	const uint64_t now = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (now < s_last + every) return;
	s_last = now;
	std::vector<std::pair<uint64_t, LayeredImage>> items;
	{
		std::lock_guard<std::mutex> lock(s_layered_mutex);
		for (const auto& [a, e]: s_layered)
			if (e.tex != nil && e.tex.pixelFormat == MTLPixelFormatRGBA16Float) items.push_back({a, e});
	}
	if (items.empty()) return;
	MetalContext::Instance().CommitCommandBuffer(true);
	id<MTLDevice> dev = MetalContext::Instance().GetDevice();
	id<MTLCommandQueue> q = [dev newCommandQueue];
	for (const auto& [addr, e]: items) {
		const size_t row = size_t(e.w) * 8u, slice_bytes = row * e.h;
		id<MTLBuffer> buf = [dev newBufferWithLength:slice_bytes * e.slices options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		for (uint32_t s = 0; s < e.slices; ++s)
			[blit copyFromTexture:e.tex sourceSlice:s sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(e.w, e.h, 1)
			             toBuffer:buf destinationOffset:s * slice_bytes destinationBytesPerRow:row destinationBytesPerImage:slice_bytes];
		[blit endEncoding];
		KytyBudgetedCommit(cb);
		[cb waitUntilCompleted];
		std::string text;
		for (uint32_t s = 0; s < e.slices; ++s) {
			const uint16_t* p = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(buf.contents) + s * slice_bytes);
			double sum = 0, mx = 0;
			for (size_t i = 0; i < size_t(e.w) * e.h; i += 7) {
				const double v = DumpHalfToFloat(p[i * 4 + 1]); // green
				if (std::isfinite(v)) { sum += v; mx = std::max(mx, v); }
			}
			if (mx == 0) continue;
			char t[64];
			std::snprintf(t, sizeof(t), " s%u:%.3g/%.3g", s, sum / (double(e.w) * e.h / 7.0), mx);
			text += t;
		}
		std::fprintf(stderr, "[LayeredStats] flip=%llu addr=0x%llx %ux%u slices=%u G mean/max:%s\n", (unsigned long long)now,
		             (unsigned long long)addr, e.w, e.h, e.slices, text.empty() ? " all zero" : text.c_str());
	}
}

// The layered image a shader image binding should use, or nil (then the usual lookups run). A storage image declared as a cube or array
// creates the array texture; a sampled one only reads an existing one (a single face of it as 2D as well).
static id<MTLTexture> LayeredImageFor(const ShaderRecompiler::IR::ImageResource& img, const ShaderTextureResource& tex, uint64_t addr,
                                      uint32_t w, uint32_t h, bool storage) {
	using Dim = ShaderRecompiler::Decoder::ImageDimension;
	if (!LayeredImagesEnabled() || addr == 0) return nil;
	const MTLPixelFormat fmt = MetalFormatFromBufferFormat(tex.Format());
	if (fmt == MTLPixelFormatInvalid || IsCompressedFormat(fmt)) return nil;
	if (img.dimension != Dim::Dim2D && img.dimension != Dim::Dim2DArray) return nil;
	const bool     cube       = img.cube || tex.Type() == Prospero::ImageType::kCube;
	const bool     array      = img.dimension == Dim::Dim2DArray;
	const uint32_t base_slice = (cube || array) ? tex.BaseArray5() : 0u;
	const uint32_t depth      = (cube || array) ? static_cast<uint32_t>(tex.Depth()) + 1u : 1u;
	const uint32_t base_level = tex.BaseLevel();
	const uint32_t last_level = std::max<uint32_t>(tex.LastLevel(), base_level);
	const bool     layered    = cube || array;
	id<MTLTexture> arr = nil;
	// T# depth: the slices the binding can reach (a writer's view ends at its probe, the cube-array reader declares all of them: FC 27's
	// ambient cubes are 32 probes x 6 faces, written one probe per capture). Growing keeps the old slices (QueueTextureCopy).
	uint32_t slices = LayeredSlicesFor(tex, addr, w, h, cube, array);
	if (storage && layered) {
		arr = LayeredImageEnsure(addr, w, h, fmt, slices, last_level + 1u, false, nil);
	} else if (layered && LayeredImageExists(addr, w, h)) {
		arr = LayeredImageEnsure(addr, w, h, fmt, slices, last_level + 1u, false, nil);
	} else {
		std::lock_guard<std::mutex> lock(s_layered_mutex);
		const auto it = s_layered.find(addr);
		if (it != s_layered.end() && it->second.w == w && it->second.h == h) arr = it->second.tex;
	}
	if (arr == nil) return nil;
	const uint32_t levels = storage ? 1u : last_level - base_level + 1u;
	// KYTY_LOG_LAYERED=1: each (address, access, declared kind, level, slice) once.
	if (KYTY_ENV("KYTY_LOG_LAYERED") != nullptr) {
		static std::mutex s_m;
		static std::set<std::tuple<uint64_t, bool, uint32_t, uint32_t, uint32_t>> s_seen;
		std::lock_guard<std::mutex> lock(s_m);
		const uint32_t kind = (img.cube ? 2u : 0u) | (array ? 1u : 0u);
		if (s_seen.insert({addr, storage, kind, base_level, base_slice}).second)
			std::fprintf(stderr, "[LayeredBind] flip=%llu addr=0x%llx %ux%u fmt=%lu %s %s level=%u..%u slice=%u depth=%u\n",
			             (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(), (unsigned long long)addr, w, h, (unsigned long)fmt,
			             storage ? "store" : "load", kind == 3 ? "cube-array" : kind == 2 ? "cube" : kind == 1 ? "array" : "2d", base_level, last_level, base_slice, depth);
	}
	// The MSL emitter declares cube images as 2D arrays (6 faces per cube, sampled face by face): bind 2D-array views of the faces.
	if (img.cube) {
		return LayeredView(arr, fmt, MTLTextureType2DArray, base_level, levels, base_slice,
		                   array ? std::max<uint32_t>(6u, (static_cast<uint32_t>(arr.arrayLength) - base_slice) / 6u * 6u) : 6u);
	}
	if (array) return LayeredView(arr, fmt, MTLTextureType2DArray, base_level, levels, base_slice, depth > base_slice ? depth - base_slice : 1u);
	return LayeredView(arr, fmt, MTLTextureType2D, base_level, levels, base_slice, 1u);
}
static bool CanonicalTargetWasRemapped(uint64_t addr);
static id<MTLTexture> CanonicalMipRead(const ShaderTextureResource& tex, uint32_t w, uint32_t h, uint64_t addr);
static bool PrepareLayeredImages(const ShaderStageRuntime& stage) {
	if (!LayeredImagesEnabled() || stage.program == nullptr || stage.resources == nullptr) return true;
	const auto& prog = *stage.program;
	const auto& snap = *stage.resources;
	using Dim = ShaderRecompiler::Decoder::ImageDimension;
	for (size_t res = 0; res < snap.images.size() && res < prog.info.images.size(); ++res) {
		const auto& img = prog.info.images[res];
		if (img.dimension != Dim::Dim2D && img.dimension != Dim::Dim2DArray) continue;
		ShaderTextureResource tex {};
		for (size_t f = 0; f < 8; ++f) tex.fields[f] = snap.images[res].dwords[f];
		const uint64_t addr = tex.Base40();
		if (addr == 0) continue;
		if(CanonicalMipRead(tex,tex.Width5()+1u,tex.Height5()+1u,addr)!=nil)continue;
		const uint32_t w = static_cast<uint32_t>(tex.Width5()) + 1u, h = static_cast<uint32_t>(tex.Height5()) + 1u;
		const MTLPixelFormat fmt = MetalFormatFromBufferFormat(tex.Format());
		if (fmt == MTLPixelFormatInvalid || IsCompressedFormat(fmt)) continue;
		const bool cube = img.cube || tex.Type() == Prospero::ImageType::kCube;
		const bool array = img.dimension == Dim::Dim2DArray;
		const uint32_t base_slice = (cube || array) ? tex.BaseArray5() : 0u;
		const uint32_t depth = (cube || array) ? static_cast<uint32_t>(tex.Depth()) + 1u : 1u;
		const uint32_t base_level = tex.BaseLevel();
		const uint32_t last_level = std::max<uint32_t>(tex.LastLevel(), base_level);
		const bool layered = cube || array;
		if (!layered && !LayeredImageExists(addr, w, h)) continue;
		// A depth writer already owns these bytes on the GPU; do not create
		// an unrelated CPU array over its address before the reader binds it.
		if (!img.written && !cube) {
			id<MTLTexture> writer = LastGpuWriterTexture(addr);
			if (writer != nil && writer.width == w && writer.height == h &&
			    writer.textureType == MTLTextureType2DArray &&
			    (writer.pixelFormat == MTLPixelFormatDepth16Unorm || writer.pixelFormat == MTLPixelFormatDepth32Float)) continue;
		}
		uint32_t slices = LayeredSlicesFor(tex, addr, w, h, cube, array);
		if (LayeredImageEnsure(addr, w, h, fmt, slices, last_level + 1u, true, nil, &tex) == nil) return false;
	}
	return true;
}


// Image bound by a draw: an image a compute shader produced at this address (texture conversion/upload kernels write
// the UI atlases this way, guest memory stays empty) wins over a copy of guest memory when the size matches.
static id<MTLTexture> ComputeGuestImage(const ShaderTextureResource& tex, uint32_t w, uint32_t h, uint64_t addr, bool r128 = false);
static id<MTLTexture> AliasConvert(uint64_t addr, uint32_t w, uint32_t h, Prospero::BufferFormat format, Prospero::TileMode tile);
// Last render pass begun by EnsureActiveRenderPass: reused to reopen the pass around a multi-draw clamp, and read by image binding.
static MetalRenderPassDesc s_last_pass_desc;
static bool s_last_pass_valid = false;
static id<MTLTexture> GraphicsImageFor(const ShaderTextureResource& tex, uint32_t w, uint32_t h, uint64_t addr) {
	if (id<MTLTexture> canonical = CanonicalMipRead(tex,w,h,addr)) return canonical;
	// GPU-produced plain 2D images are read from the same guest-memory views the writers used (see GpuSurfaceFor). A depth surface or a mipped
	// image the GPU wrote has no guest-memory form: the lookups below handle those.
	if (UmaSurfacesEnabled() && tex.Type() == Prospero::ImageType::kColor2D && tex.BaseLevel() == 0 && tex.LastLevel() == 0) {
		const MTLPixelFormat uma_fmt = MetalFormatFromBufferFormat(tex.Format());
		if (uma_fmt != MTLPixelFormatInvalid && !IsCompressedFormat(uma_fmt)) {
			bool special_writer = false;
			if (id<MTLTexture> lw = LastGpuWriterTexture(addr)) {
				special_writer = lw.mipmapLevelCount > 1 || lw.pixelFormat == MTLPixelFormatDepth32Float || lw.pixelFormat == MTLPixelFormatDepth16Unorm ||
				                 lw.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
			}
			if (!special_writer) {
				if (id<MTLTexture> uma = GpuSurfaceFor(addr, w, h, uma_fmt, tex.TileMode(), false)) {
					static const bool s_log = KYTY_ENV("KYTY_LOG_UMA") != nullptr;
					if (s_log) {
						static std::mutex s_log_mutex;
						static std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>> s_seen;
						std::lock_guard<std::mutex> lock(s_log_mutex);
						if (s_seen.size() < 600 && s_seen.insert({addr, w, h, static_cast<uint32_t>(tex.Format()), static_cast<uint32_t>(tex.TileMode())}).second) {
							const uint32_t bpp_log = BytesPerPixel(uma_fmt);
							std::fprintf(stderr, "[UmaGfx] flip=%llu addr=0x%llx %ux%u guestfmt=%u metal=%lu tile=%u match=%s\n",
							             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)),
							             static_cast<unsigned long long>(addr), w, h, static_cast<unsigned>(tex.Format()), static_cast<unsigned long>(uma_fmt),
							             static_cast<unsigned>(tex.TileMode()),
							             GpuWrittenKind(addr, static_cast<uint64_t>(w) * h * std::max<uint32_t>(bpp_log, 1u)) == 2 ? "exact" : "overlap");
						}
					}
					return uma;
				}
			}
		}
	}
	// A volume: the lookups below return 2D objects (a transient address can hold a 2D surface of the same width and height), which
	// a texture3d argument must not receive. ComputeGuestImage builds or finds the volume.
	if (tex.Type() == Prospero::ImageType::kColor3D) {
		return ComputeGuestImage(tex, w, h, addr);
	}
	if (id<MTLTexture> conv = AliasConvert(addr, w, h, tex.Format(), tex.TileMode())) {
		return conv;
	}
	// A draw that samples a surface the GPU wrote (the scene depth read by the velocity/TAA passes, a lit render target) must see the
	// writer's own texture. Depth lives only in a private Metal depth texture (guest memory behind it is empty), so without this the
	// draw read a "GuestTex" copy of near-zero memory: garbage velocity, noisy TAA and smeared text. Same-pass reads of the bound
	// depth attachment are left alone (a feedback loop).
	{
		uint64_t wr_flip = 0;
		if (id<MTLTexture> last = LastGpuWriterTexture(addr, &wr_flip)) {
			const uint64_t now_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
			const bool depth_writer = last.pixelFormat == MTLPixelFormatDepth32Float || last.pixelFormat == MTLPixelFormatDepth16Unorm;
			// With depth writes off the attachment does not change, so sampling it is safe, and the game does it (the fog pass PS 789336b5 reads
			// the scene depth under a read-only depth test). Refusing sent the lookup on to whatever older image had lived at this address, an R32F
			// full of NaN in some runs: an orange band with silhouettes of other scenes across the frame, for a whole run (um296).
			const bool in_current_pass = s_last_pass_valid && s_last_pass_desc.has_depth && s_last_pass_desc.depth_attachment.texture == last && s_draw_writes_depth;
			if (last.width == w && last.height == h && last.mipmapLevelCount == 1 && last.textureType == MTLTextureType2D && !in_current_pass) {
				const MTLPixelFormat want_fmt = MetalFormatFromBufferFormat(tex.Format());
				if (depth_writer) {
					const uint32_t web = last.pixelFormat == MTLPixelFormatDepth16Unorm ? 2u : 4u;
					if (now_flip <= wr_flip + 2 && want_fmt != MTLPixelFormatInvalid && !IsCompressedFormat(want_fmt) && BytesPerPixel(want_fmt) == web) {
						return last;
					}
				} else if (SameTexelSize(last, want_fmt)) {
					return ReinterpretFormat(last, want_fmt);
				}
			}
		}
	}
	// The last writer of the address may be an unrelated resource of another size (the game aliases memory inside a frame); a render
	// target of this surface's exact size still holds the scene, whereas the registry below may hold an old copy.
	if (id<MTLTexture> rt = NewestRenderTargetFor(addr, w, h, MetalFormatFromBufferFormat(tex.Format()))) {
		return ReinterpretFormat(rt, MetalFormatFromBufferFormat(tex.Format()));
	}
	// The registry also holds every image a draw sampled (presenter lookup), so a block-compressed entry is only a decoded guest copy, the
	// shadow of a GuestTexture cache entry: never GPU output (GPU-encoded blocks stay in s_bc_level_images and are laid over the decode).
	// Taking it here skipped the cache's freshness check: streaming-pool addresses are reused for other assets of the same size, and draws
	// kept sampling the previous asset (KYTY_GUEST_TEX_VERIFY, cl620: 20 textures, 1-8% of the blocks equal to the current bytes, the cache
	// entry already newer). Block-compressed images go on to GuestTexture. KYTY_BC_REGISTRY_REUSE=1 restores the old shortcut.
	static const bool s_bc_registry = false;
	if (void* wr = GetComputeWrittenImageForAddress(addr)) {
		id<MTLTexture> cand = (__bridge id<MTLTexture>)wr;
		if (cand.width == w && cand.height == h && cand.textureType == MTLTextureType2D &&
		    SameTexelSize(cand, MetalFormatFromBufferFormat(tex.Format())) && (s_bc_registry || !IsCompressedFormat(cand.pixelFormat))) {
			return ReinterpretFormat(cand, MetalFormatFromBufferFormat(tex.Format()));
		}
	}
	// No object of this layout at an address the GPU wrote in the last frames (the game's transient memory): the Vulkan reference
	// creates a new image that starts empty (it uploads guest memory only for CPU-written pages). Decoding the guest bytes instead read
	// another resource's texels in this format - the DoF overlay draw (PS ac6862e0) sampled the R8 CoC mask as RGBA16F and added NaN
	// and 31232 to the scene, which the bloom pyramid spread over the frame. Only a writer of another texel size counts: render targets
	// are views of guest memory, so a reader of the same texel size (a padded 1616x912 view of a 1600x904 target) decodes the
	// writer's real texels. KYTY_GFX_GUEST_DECODE=1 restores the decode in all cases.
	{
		static const bool s_decode = false;
		const MTLPixelFormat want_fmt = MetalFormatFromBufferFormat(tex.Format());
		uint64_t wr_flip = 0;
		id<MTLTexture> writer = s_decode ? nil : LastGpuWriterTexture(addr, &wr_flip);
		if (writer != nil && tex.Type() == Prospero::ImageType::kColor2D && want_fmt != MTLPixelFormatInvalid && !IsCompressedFormat(want_fmt) &&
		    !IsCompressedFormat(writer.pixelFormat) && BytesPerPixel(writer.pixelFormat) != BytesPerPixel(want_fmt) &&
		    Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed) <= wr_flip + 2) {
			return AliasedImageFor(addr, w, h, want_fmt, 1u);
		}
	}
	return ComputeGuestImage(tex, w, h, addr);
}

struct DepthSurface {
	MetalAttachmentArray layers;
	id<MTLTexture> tex = nil;
	uint32_t w = 0, h = 0;
	MTLPixelFormat fmt = MTLPixelFormatInvalid;
	uint64_t used_flip = 0;
	int z_dir = 0;
};
static std::unordered_map<uint64_t, DepthSurface> s_depth_by_addr;

// Sample the actual depth writer, selecting the guest array view. Static shadow
// maps remain valid while read; elapsed frames do not transfer memory ownership.
// The compute and graphics paths both use the same depth-layer lookup.
static bool CanonicalMipOverlap(uint64_t addr,uint64_t size);
static id<MTLTexture> DepthWriterForLayeredRead(const ShaderRecompiler::IR::ImageResource& img, const ShaderTextureResource& tex, uint64_t addr,
                                                uint32_t w, uint32_t h) {
	using Dim = ShaderRecompiler::Decoder::ImageDimension;
	if (addr == 0 || img.cube || (img.dimension != Dim::Dim2D && img.dimension != Dim::Dim2DArray)) return nil;
	id<MTLTexture> last = LastGpuWriterTexture(addr);
	if (last == nil || (last.pixelFormat != MTLPixelFormatDepth32Float && last.pixelFormat != MTLPixelFormatDepth16Unorm) ||
	    last.width != w || last.height != h || tex.BaseLevel() != 0 || tex.LastLevel() != 0) return nil;
	const bool array = img.dimension == Dim::Dim2DArray;
	const uint32_t first = tex.BaseArray5();
	const uint32_t count = array ? static_cast<uint32_t>(tex.Depth()) + 1u - first : 1u;
	if (tex.Depth() < first || count == 0) return nil;
	const MTLPixelFormat want = MetalFormatFromBufferFormat(tex.Format());
	const bool value_view=(last.pixelFormat==MTLPixelFormatDepth32Float && want==MTLPixelFormatR32Float) ||
	    (last.pixelFormat==MTLPixelFormatDepth16Unorm && want==MTLPixelFormatR16Unorm);
	if(!value_view || CanonicalMipOverlap(addr,uint64_t(w)*h*BytesPerPixel(last.pixelFormat)))return nil;
	id<MTLTexture> active = MetalContext::Instance().GetActiveRenderEncoder() != nil && s_last_pass_valid && s_last_pass_desc.has_depth ? s_last_pass_desc.depth_attachment.texture : nil;
	if (s_draw_writes_depth && active != nil && (active == last || active.parentTexture == last ||
	    (last.parentTexture != nil && active.parentTexture == last.parentTexture))) return nil;
	const uint64_t now = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (auto it = s_depth_by_addr.find(addr); it != s_depth_by_addr.end() && it->second.tex == last) it->second.used_flip = now;
	id<MTLTexture> view = nil;
	if (last.textureType == MTLTextureType2DArray) {
		if (first >= last.arrayLength || count > last.arrayLength - first) return nil;
		view = LayeredView(last,last.pixelFormat,array ? MTLTextureType2DArray : MTLTextureType2D,0,1,first,count);
	} else if (last.textureType == MTLTextureType2D && first == 0 && count == 1) {
		view = ConformToDeclaredArray(last,array);
	}
	if (view != nil && KYTY_ENV("KYTY_TRACE_RASTER_STATE") != nullptr && (first != 0 || count > 1)) {
		static std::set<std::tuple<uint64_t,uint32_t,uint32_t>> seen;
		if (seen.emplace(addr,first,count).second) std::fprintf(stderr,"[DepthLayerRead] addr=0x%llx layers=%u:%u parent=%p view=%p\n",
		    (unsigned long long)addr,first,first+count-1,(__bridge void*)last,(__bridge void*)view);
	}
	return view;
}

// Current host protection of the page holding `a` (-1 when unmapped): guest memory uses mprotect write tracking.
static int HostProtectionAt(uint64_t a) {
	vm_address_t addr = static_cast<vm_address_t>(a & ~static_cast<uint64_t>(getpagesize() - 1));
	vm_size_t vsize = 0;
	vm_region_basic_info_data_64_t info {};
	mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object = 0;
	if (vm_region_64(mach_task_self(), &addr, &vsize, VM_REGION_BASIC_INFO_64,
	                 reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS) {
		return -1;
	}
	if (addr > static_cast<vm_address_t>(a)) return -1;
	return info.protection;
}

// Per-flip draw accounting (diagnostic): submitted by the game, executed, and dropped by stage.
namespace {
std::mutex s_draw_stats_mutex;
uint32_t s_ds_submitted = 0, s_ds_executed = 0, s_ds_drop_vs = 0, s_ds_drop_ps = 0, s_ds_drop_pipe = 0;
}
// Last render pass begun by EnsureActiveRenderPass: reused to reopen the pass around a multi-draw clamp.

// ---- Surface conversion: guest tiled memory as the common ground between passes ----
// A pass that reads a guest address with another element size, size or tile layout than its last GPU writer used must see what
// the hardware would: the writer's texels laid out in guest memory by the writer's tile mode, re-read through the reader's.
// The writer's texture is exported to a scratch image of that tiled memory and imported into a texture of the reader's layout
// (both with the generic GPU tiling kernels, in command-buffer order). Same-layout accesses never get here.
struct SurfaceConv {
	id<MTLTexture> elem = nil; // uint texture of the reader's element size, filled by the import kernel
	id<MTLTexture> view = nil; // what the reader binds (a view of elem, or a block-compressed copy)
	uint64_t serial = 0;
	uint64_t read_epoch = 0;
	uint32_t w = 0, h = 0, fmt = 0, tile = 0;
};
using SurfaceConvKey = std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>;
static std::map<SurfaceConvKey, SurfaceConv> s_surface_conv;
// Guest bytes outside a private writer can change between GPU commands. Reuse
// the conversion only while preparing/binding one draw or dispatch.
static uint64_t s_image_read_epoch = 0; // renderer mutex

static Prospero::BufferFormat RepresentativeFormat(uint32_t element_bytes) {
	switch (element_bytes) {
		case 1: return Prospero::BufferFormat::k8UNorm;
		case 2: return Prospero::BufferFormat::k16UNorm;
		case 4: return Prospero::BufferFormat::k8_8_8_8UNorm;
		case 8: return Prospero::BufferFormat::k16_16_16_16UNorm;
		default: return Prospero::BufferFormat::k32_32_32_32UInt;
	}
}

// Tiling tables (and element grid size) of a 2D level-0 surface; cached per layout.
static const TileTables* SurfaceTables(Prospero::BufferFormat format, Prospero::TileMode tile, uint32_t width, uint32_t height,
                                       uint32_t& elem_w, uint32_t& elem_h) {
	TileTextureBlockLayout layout {};
	if (width == 0 || height == 0 || !TileGetTextureBlockLayout(format, tile, false, layout)) return nullptr;
	elem_w = (width + layout.texel_width - 1) / layout.texel_width;
	elem_h = (height + layout.texel_height - 1) / layout.texel_height;
	const uint32_t pitch_elems = TileGetTexturePitch(format, width, tile) / layout.texel_width;
	static std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, TileTables> s_cache;
	const auto key = std::make_tuple(static_cast<uint32_t>(layout.block.family), layout.block.bytes_per_element, std::max(pitch_elems, elem_w), elem_h);
	auto it = s_cache.find(key);
	if (it == s_cache.end()) {
		TileTables t;
		if (!BuildTileTables(layout.block, std::max(pitch_elems, elem_w), elem_h, t)) {
			std::fprintf(stderr, "[Surface] tile tables failed: family=%u eb=%u pitch=%u h=%u\n", key == key ? static_cast<uint32_t>(layout.block.family) : 0u,
			             layout.block.bytes_per_element, std::max(pitch_elems, elem_w), elem_h);
			return nullptr;
		}
		it = s_cache.emplace(key, std::move(t)).first;
	}
	return &it->second;
}

// KYTY_LOG_ALIAS_STATS=1: every 600 flips, how many tiled reads asked AliasConvert, how many found a writer of the address, how many
// were refused because the writer was older than two flips (the age rule that stands in for CPU-write tracking), how many reused an
// earlier conversion and how many encoded GPU work. Counts since the previous line.
struct AliasStats { uint64_t calls = 0, writer = 0, too_old = 0, too_old_differs = 0, reused = 0, encoded = 0, last_flip = 0; };
static AliasStats s_alias_stats;
static void AliasStatsTick() {
	static const bool s_on = KYTY_ENV("KYTY_LOG_ALIAS_STATS") != nullptr;
	if (!s_on) return;
	const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (flip < s_alias_stats.last_flip + 600) return;
	std::fprintf(stderr, "[AliasStats] flip=%llu calls=%llu writer_found=%llu refused_too_old=%llu (reader layout differs from the writer's: %llu) reused=%llu encoded=%llu\n",
	             (unsigned long long)flip, (unsigned long long)s_alias_stats.calls, (unsigned long long)s_alias_stats.writer,
	             (unsigned long long)s_alias_stats.too_old, (unsigned long long)s_alias_stats.too_old_differs,
	             (unsigned long long)s_alias_stats.reused, (unsigned long long)s_alias_stats.encoded);
	s_alias_stats = AliasStats {};
	s_alias_stats.last_flip = flip;
}

// Statistics only: would this reader have needed a conversion of the writer's surface (another element size, grid, layout family or pitch)?
static bool AliasLayoutDiffers(const CsStorageEntry& wr, uint32_t w, uint32_t h, Prospero::BufferFormat format, Prospero::TileMode tile) {
	if (wr.tex == nil || wr.tile == Prospero::TileMode::kLinear || wr.tex.mipmapLevelCount != 1) return false;
	const bool depth = wr.tex.pixelFormat == MTLPixelFormatDepth32Float || wr.tex.pixelFormat == MTLPixelFormatDepth16Unorm;
	const uint32_t web = depth ? (wr.tex.pixelFormat == MTLPixelFormatDepth16Unorm ? 2u : 4u)
	                           : (IsCompressedFormat(wr.tex.pixelFormat) ? 0u : static_cast<uint32_t>(BytesPerPixel(wr.tex.pixelFormat)));
	if (web == 0 || web > 16) return false;
	uint32_t ww = 0, wh = 0, cw = 0, ch = 0;
	const TileTables* wt = SurfaceTables(RepresentativeFormat(web), wr.tile, static_cast<uint32_t>(wr.tex.width), static_cast<uint32_t>(wr.tex.height), ww, wh);
	const TileTables* ct = SurfaceTables(format, tile, w, h, cw, ch);
	if (wt == nullptr || ct == nullptr) return false;
	return !(web == ct->block.bytes_per_element && ww == cw && wh == ch && wt->block.family == ct->block.family && wt->columns == ct->columns);
}

static id<MTLTexture> AliasConvert(uint64_t addr, uint32_t w, uint32_t h, Prospero::BufferFormat format, Prospero::TileMode tile) {
	static const bool s_off = false;
	if (s_off || tile == Prospero::TileMode::kLinear || addr == 0 || w == 0 || h == 0) return nil;
	++s_alias_stats.calls;
	AliasStatsTick();
	CsStorageEntry wr;
	{
		std::lock_guard<std::mutex> lock(s_cs_storage_mutex);
		const auto it = s_cs_storage.find(addr);
		if (it == s_cs_storage.end()) return nil;
		wr = it->second;
	}
	++s_alias_stats.writer;
	// Only a recent writer is trusted: older content may have been replaced by the CPU or a DMA the Metal path does not track.
	// KYTY_ALIAS_OLD_WRITER=1 (experiment): an older compute writer also is while the guest bytes behind it are the ones it was bound
	// over (checked once per flip). The FC News badge (BC3 156x72) is written once by the texture copy kernel (guest e5941d54) as a
	// 39x18 RGBA32Uint image and sampled for minutes: decoded from guest memory it shows block-shaped letters filled with noise, from
	// the old writer it is black (the copy's 64x64 staging source was all zeros when the dispatch was encoded) - neither is right.
	const uint64_t now_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (wr.tex == nil || wr.tile == Prospero::TileMode::kLinear || wr.tex.mipmapLevelCount != 1) return nil;
	if (now_flip > wr.flip + 2) {
		static const bool s_old = false;
		if (!s_old || wr.guest_hash == 0) {
			++s_alias_stats.too_old;
			static const bool s_stats = KYTY_ENV("KYTY_LOG_ALIAS_STATS") != nullptr;
			if (s_stats && AliasLayoutDiffers(wr, w, h, format, tile)) ++s_alias_stats.too_old_differs;
			return nil;
		}
		if (wr.checked_flip != now_flip) {
			wr.guest_same = GuestTexSourceHash(addr, wr.guest_bytes) == wr.guest_hash;
			wr.checked_flip = now_flip;
			std::lock_guard<std::mutex> lock(s_cs_storage_mutex);
			const auto it = s_cs_storage.find(addr);
			if (it != s_cs_storage.end() && it->second.serial == wr.serial) {
				it->second.checked_flip = now_flip;
				it->second.guest_same = wr.guest_same;
			}
		}
		if (!wr.guest_same) return nil;
	}
	const MTLPixelFormat want = MetalFormatFromBufferFormat(format);
	if (want == MTLPixelFormatInvalid) return nil;
	const uint32_t bc_bytes = Prospero::BlockCompressedBytesPerBlock(format);
	const bool writer_is_depth = wr.tex.pixelFormat == MTLPixelFormatDepth32Float || wr.tex.pixelFormat == MTLPixelFormatDepth16Unorm;
	const uint32_t web = writer_is_depth ? (wr.tex.pixelFormat == MTLPixelFormatDepth16Unorm ? 2u : 4u)
	                     : (IsCompressedFormat(wr.tex.pixelFormat) ? 0u : static_cast<uint32_t>(BytesPerPixel(wr.tex.pixelFormat)));
	if (web == 0 || web > 16) return nil;
	uint32_t ww = 0, wh = 0, cw = 0, ch = 0;
	const TileTables* wt = SurfaceTables(RepresentativeFormat(web), wr.tile, static_cast<uint32_t>(wr.tex.width),
	                                     static_cast<uint32_t>(wr.tex.height), ww, wh);
	const TileTables* ct = SurfaceTables(format, tile, w, h, cw, ch);
	if (wt == nullptr || ct == nullptr) return nil;
	const uint32_t ceb = ct->block.bytes_per_element;
	// Same surface (element size, grid, layout family, row pitch): the plain shared-image path handles it.
	if (web == ceb && ww == cw && wh == ch && wt->block.family == ct->block.family && wt->columns == ct->columns) return nil;
	if (TileElementFormat(ceb) == MTLPixelFormatInvalid || TileElementFormat(web) == MTLPixelFormatInvalid) return nil;

	// Several descriptors can read one address through different layouts in the
	// same draw. Keep each conversion so preparation and binding reuse it.
	const SurfaceConvKey key {addr, w, h, static_cast<uint32_t>(want), static_cast<uint32_t>(tile)};
	const auto cit = s_surface_conv.find(key);
	if (cit != s_surface_conv.end() && cit->second.serial == wr.serial && cit->second.read_epoch == s_image_read_epoch &&
	    cit->second.w == w && cit->second.h == h &&
	    cit->second.fmt == static_cast<uint32_t>(want) && cit->second.tile == static_cast<uint32_t>(tile)) {
		++s_alias_stats.reused;
		return cit->second.view;
	}
	id<MTLDevice> dev = MetalContext::Instance().GetDevice();
	id<MTLTexture> src = writer_is_depth ? nil : ReinterpretFormat(wr.tex, TileElementFormat(web));
	if (writer_is_depth) {
		// A depth texture cannot be read as raw elements: stage it through a linear buffer into an uint texture of the same size.
		static std::map<std::tuple<uint32_t, uint32_t, uint32_t>, id<MTLTexture>> s_depth_stage;
		const auto key = std::make_tuple(static_cast<uint32_t>(wr.tex.width), static_cast<uint32_t>(wr.tex.height), web);
		id<MTLTexture> st = s_depth_stage[key];
		if (st == nil) {
			MTLTextureDescriptor* sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:TileElementFormat(web) width:wr.tex.width
			                                                                              height:wr.tex.height mipmapped:NO];
			sd.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
			sd.storageMode = MTLStorageModePrivate;
			st = KytyNewTexture(dev, sd, __LINE__);
			s_depth_stage[key] = st;
		}
		src = st;
	}
	if (src == nil || src.pixelFormat != TileElementFormat(web)) {
		FailMetalMaterialization("surface writer cannot be read as raw elements", static_cast<uint32_t>(wr.tex.pixelFormat), web);
		return nil;
	}
	SurfaceConv& conv = s_surface_conv[key];
	if (conv.elem == nil || conv.w != w || conv.h != h || conv.fmt != static_cast<uint32_t>(want) || conv.tile != static_cast<uint32_t>(tile)) {
		MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:TileElementFormat(ceb) width:cw height:ch mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
		d.storageMode = MTLStorageModeShared;
		conv.elem = KytyNewTexture(dev, d, __LINE__);
		conv.view = nil;
		if (bc_bytes != 0) {
			MTLTextureDescriptor* bd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:want width:cw * 4 height:ch * 4 mipmapped:NO];
			bd.usage = MTLTextureUsageShaderRead;
			bd.storageMode = MTLStorageModeShared;
			conv.view = KytyNewTexture(dev, bd, __LINE__);
		} else if (conv.elem != nil) {
			conv.view = want == conv.elem.pixelFormat ? conv.elem : [conv.elem newTextureViewWithPixelFormat:want];
		}
		if (conv.elem == nil || conv.view == nil) {
			FailMetalMaterialization("surface reader texture", static_cast<uint32_t>(want), ceb);
			conv = SurfaceConv {};
			return nil;
		}
		conv.w = w; conv.h = h; conv.fmt = static_cast<uint32_t>(want); conv.tile = static_cast<uint32_t>(tile);
	}
	const size_t need = std::max<size_t>(static_cast<size_t>(wt->columns) * wt->rows * wt->block.block_size,
	                                     static_cast<size_t>(ct->columns) * ct->rows * ct->block.block_size);
	const auto guest = MetalBufferManager::Instance().GetBufferForGuestRange(addr,need);
	if (!guest.IsValid() || guest.size < need) {
		FailMetalMaterialization("surface alias guest backing", static_cast<uint32_t>(need));
		return nil;
	}
	static id<MTLBuffer> s_scratch = nil;
	if (s_scratch == nil || s_scratch.length < need) {
		s_scratch = KytyNewBuffer(dev, std::max<size_t>(need, 4u << 20), MTLResourceStorageModePrivate, __LINE__);
		if (s_scratch == nil) return nil;
	}
	const bool reopen = MetalContext::Instance().GetActiveRenderEncoder() != nil && s_last_pass_valid;
	MetalDrawExecutor::Instance().EndRenderPass();
	MetalContext::Instance().EndCurrentEncoder();
	id<MTLCommandBuffer> cmd = MetalContext::Instance().GetCurrentCommandBuffer();
	if (cmd == nil) return nil;
	if (writer_is_depth) {
		const size_t drow = static_cast<size_t>(wr.tex.width) * web;
		id<MTLBuffer> dtmp = KytyNewBuffer(dev, drow * wr.tex.height, MTLResourceStorageModePrivate, __LINE__);
		id<MTLBlitCommandEncoder> db = [cmd blitCommandEncoder];
		OrderBegin(db);
		[db copyFromTexture:wr.tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(wr.tex.width, wr.tex.height, 1)
		           toBuffer:dtmp destinationOffset:0 destinationBytesPerRow:drow destinationBytesPerImage:drow * wr.tex.height
		            options:MTLBlitOptionDepthFromDepthStencil];
		[db copyFromBuffer:dtmp sourceOffset:0 sourceBytesPerRow:drow sourceBytesPerImage:drow * wr.tex.height
		        sourceSize:MTLSizeMake(wr.tex.width, wr.tex.height, 1) toTexture:src destinationSlice:0 destinationLevel:0
		 destinationOrigin:MTLOriginMake(0, 0, 0)];
		OrderEnd(db); [db endEncoding];
	}
	bool ok = EncodeTileAlias(dev,cmd,guest.buffer,guest.offset,s_scratch,src,*wt,
	                          static_cast<uint32_t>(wr.tex.width),static_cast<uint32_t>(wr.tex.height),conv.elem,*ct,cw,ch);
	if (ok && bc_bytes != 0) {
		// Metal cannot write block-compressed textures from a kernel: copy the imported blocks across through a buffer.
		const size_t row = static_cast<size_t>(cw) * bc_bytes;
		id<MTLBuffer> tmp = KytyNewBuffer(dev, row * ch, MTLResourceStorageModePrivate, __LINE__);
		id<MTLBlitCommandEncoder> bb = [cmd blitCommandEncoder];
		OrderBegin(bb);
		[bb copyFromTexture:conv.elem sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(cw, ch, 1)
		           toBuffer:tmp destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * ch];
		[bb copyFromBuffer:tmp sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row * ch sourceSize:MTLSizeMake(cw * 4, ch * 4, 1)
		         toTexture:conv.view destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
		OrderEnd(bb); [bb endEncoding];
	}
	if (reopen) {
		MetalRenderPassDesc ro = s_last_pass_desc;
		for (uint32_t i = 0; i < ro.color_attachment_count; ++i) ro.color_attachments[i].load_action = MTLLoadActionLoad;
		if (ro.has_depth) ro.depth_attachment.load_action = MTLLoadActionLoad;
		if (ro.has_stencil) ro.stencil_attachment.load_action = MTLLoadActionLoad;
		MetalDrawExecutor::Instance().BeginRenderPass(ro);
	}
	if (!ok) {
		FailMetalMaterialization("surface tiling kernels", static_cast<uint32_t>(wr.tex.pixelFormat), web, ceb);
		return nil;
	}
	conv.serial = wr.serial;
	conv.read_epoch = s_image_read_epoch;
	++s_alias_stats.encoded;
	static std::atomic<uint32_t> s_log {0};
	if (s_log.fetch_add(1) < 24) {
		std::fprintf(stderr, "[Surface] addr=0x%llx writer %lux%lu eb=%u tile=%u -> reader %ux%u eb=%u tile=%u fmt=%lu\n", (unsigned long long)addr,
		             (unsigned long)wr.tex.width, (unsigned long)wr.tex.height, web, (unsigned)wr.tile, w, h, ceb, (unsigned)tile, (unsigned long)want);
	}
	return conv.view;
}


#include "metalCanonicalMip.inc"
static id<MTLTexture> CanonicalMipRead(const ShaderTextureResource& tex, uint32_t w, uint32_t h, uint64_t addr) {
	id<MTLTexture> image=CanonicalMipImage(tex,w,h,addr,false).image;
	if(image && image.mipmapLevelCount>1 && tex.BaseLevel()>0)
		return MipRangeView(image,tex.BaseLevel(),tex.LastLevel()-tex.BaseLevel()+1u);
	return image;
}

// ---- GDS ----
static id<MTLBuffer> GdsBuffer() {
	static id<MTLBuffer> s_gds = nil;
	if (s_gds == nil) {
		s_gds = KytyNewBuffer(MetalContext::Instance().GetDevice(), 64 * 1024, MTLResourceStorageModeShared, __LINE__);
		std::memset([s_gds contents], 0, 64 * 1024);
	}
	return s_gds;
}

// The first GDS dwords as the GPU left them (the watchdog report: a GDS work queue's count and index when its kernel hangs).
std::string KytyDescribeGds() {
	const auto* d = static_cast<const uint32_t*>([GdsBuffer() contents]);
	std::string text;
	for (uint32_t i = 0; i < 24; ++i) {
		char item[24];
		std::snprintf(item, sizeof(item), "%s[%u]=%u", i == 0 ? "" : " ", i, d[i]);
		text += item;
	}
	// KYTY_LOOP_TRIP_GDS: runaway-loop guard trips per loop id (MslEmitter kMslLoopTripGdsBase, the last 8 dwords).
	if (std::getenv("KYTY_LOOP_TRIP_GDS") != nullptr) {
		text += " loop_trips:";
		for (uint32_t i = 16376; i < 16384; ++i) {
			char item[24];
			std::snprintf(item, sizeof(item), " %u", d[i]);
			text += item;
		}
	}
	return text;
}

static void GdsBlit(id<MTLBuffer> src, NSUInteger src_off, id<MTLBuffer> dst, NSUInteger dst_off, NSUInteger bytes) {
	MetalContext::Instance().EndCurrentEncoder();
	id<MTLCommandBuffer> cmd = MetalContext::Instance().GetCurrentCommandBuffer();
	if (cmd == nil) return;
	id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
	// Like every encoder of the shared command stream this copy has to run after the previous encoder (the kernel that counted into GDS) and
	// before the next one (the kernel or the indirect-argument converter that reads the copied count). KYTY_GDS_BLIT_UNORDERED=1 restores the old
	// unfenced blit.
	static const bool s_unordered = false;
	if (!s_unordered) OrderBegin(blit);
	[blit copyFromBuffer:src sourceOffset:src_off toBuffer:dst destinationOffset:dst_off size:bytes];
	if (!s_unordered) OrderEnd(blit);
	[blit endEncoding];
}

static void GdsDmaLocked(bool dst_gds, bool src_gds, uint64_t dst, uint64_t src, uint32_t bytes) {
	if (bytes == 0 || (dst_gds && src_gds)) return;
	id<MTLBuffer> gds = GdsBuffer();
	if (dst_gds) {
		if (dst + bytes > gds.length) return;
		auto a = MetalBufferManager::Instance().GetBufferForGuestRange(src, bytes);
		if (a.buffer == nil) return;
		GdsBlit(a.buffer, a.offset, gds, dst, bytes);
	} else if (src_gds) {
		if (src + bytes > gds.length) return;
		auto a = MetalBufferManager::Instance().GetBufferForGuestRange(dst, bytes);
		if (a.buffer == nil) return;
		GdsBlit(gds, src, a.buffer, a.offset, bytes);
		GuestWrites::NoteWrite(dst, bytes);
	}
	static std::atomic<uint32_t> s_log {0};
	if (KYTY_ENV("KYTY_LOG_GDS") != nullptr && s_log.fetch_add(1) < 16) {
		std::fprintf(stderr, "[GdsDmaMetal] dst_gds=%d src_gds=%d dst=0x%llx src=0x%llx bytes=%u\n", dst_gds ? 1 : 0, src_gds ? 1 : 0,
		             (unsigned long long)dst, (unsigned long long)src, bytes);
	}
}

static void GdsFillLocked(uint64_t dst_offset, uint32_t bytes, uint32_t value) {
	id<MTLBuffer> gds = GdsBuffer();
	if (bytes == 0 || dst_offset + bytes > gds.length) return;
	auto tmp = MetalBufferManager::Instance().AllocateDynamic(bytes, 16);
	if (!tmp.IsValid()) return;
	uint32_t* d = static_cast<uint32_t*>(tmp.cpu_addr);
	for (uint32_t i = 0; i < bytes / 4; ++i) d[i] = value;
	GdsBlit(tmp.buffer, tmp.offset, gds, dst_offset, bytes);
}

void MetalRenderer::GdsDmaGuarded(bool dst_gds, bool src_gds, uint64_t dst, uint64_t src, uint32_t bytes) {
	if (!m_active) return;
	std::lock_guard lock(m_mutex);
	GdsDmaLocked(dst_gds, src_gds, dst, src, bytes);
}
void MetalRenderer::GdsFillGuarded(uint64_t dst_offset, uint32_t bytes, uint32_t value) {
	if (!m_active) return;
	std::lock_guard lock(m_mutex);
	GdsFillLocked(dst_offset, bytes, value);
}
void GdsDma(bool dst_gds, bool src_gds, uint64_t dst, uint64_t src, uint32_t bytes) {
	MetalRenderer::Instance().GdsDmaGuarded(dst_gds, src_gds, dst, src, bytes);
}
void GdsFill(uint64_t dst_offset, uint32_t bytes, uint32_t value) {
	MetalRenderer::Instance().GdsFillGuarded(dst_offset, bytes, value);
}

static void MslDrawStatSubmitted() { std::lock_guard<std::mutex> l(s_draw_stats_mutex); ++s_ds_submitted; }
static void MslDrawStatExecuted() { std::lock_guard<std::mutex> l(s_draw_stats_mutex); ++s_ds_executed; }
static void MslDrawStatDropped(int stage) {
	std::lock_guard<std::mutex> l(s_draw_stats_mutex);
	if (stage == 0) ++s_ds_drop_vs; else if (stage == 1) ++s_ds_drop_ps; else ++s_ds_drop_pipe;
}


// Per-op trace for one guest flip (KYTY_TRACE_OPS_FLIP=<flip>): before each compute op, the inputs are
// measured (after committing the previous work); the previous op's outputs are measured at the same point.
static uint64_t s_trace_flip = 0;
static bool s_trace_on = false;
static int s_trace_op = 0;
struct TraceRes { uint64_t addr; uint64_t size; };
static std::vector<std::pair<int, TraceRes>> s_trace_prev_outputs;  // (op index, resource)

static double TraceNonZero(uint64_t addr, uint64_t size) {
	auto a = MetalBufferManager::Instance().GetBufferForGuestRange(addr, size);
	if (a.buffer == nil) return -1.0;
	const uint32_t* p = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>([a.buffer contents]) + a.offset);
	const size_t words = std::min<size_t>(size / 4, size_t {1} << 22);
	if (words == 0) return -1.0;
	size_t nz = 0;
	for (size_t i = 0; i < words; ++i) nz += p[i] != 0;
	return 100.0 * static_cast<double>(nz) / static_cast<double>(words);
}

// Compute-written images live on the GPU, so guest memory does not show their content. Read the
// registered texture back (RGBA32F or RGBA8 only) and report non-zero share plus the corner and
// centre texels, so a zero LUT output can be told apart from a missing copy.
static void TraceTextureOutput(int op, void* tex_ptr) {
	id<MTLTexture> tex = (__bridge id<MTLTexture>)tex_ptr;
	// getBytes needs shared storage; private textures cannot be read from the CPU.
	const size_t bpp = tex.storageMode == MTLStorageModePrivate ? 0u
	                 : tex.pixelFormat == MTLPixelFormatRGBA32Float ? 16u
	                 : (tex.pixelFormat == MTLPixelFormatRGBA8Unorm || tex.pixelFormat == MTLPixelFormatBGRA8Unorm) ? 4u : 0u;
	if (bpp == 0) {
		std::fprintf(stderr, "[OP] %d out tex fmt=%lu unreadable\n", op, (unsigned long)tex.pixelFormat);
		return;
	}
	const NSUInteger w = tex.width, h = tex.height, d = tex.depth > 0 ? tex.depth : 1;
	std::vector<uint8_t> bytes(static_cast<size_t>(w) * h * d * bpp);
	// One call for the whole volume: 3D textures take the depth from the region origin/size.
	[tex getBytes:bytes.data() bytesPerRow:w * bpp bytesPerImage:w * h * bpp
	    fromRegion:MTLRegionMake3D(0, 0, 0, w, h, d) mipmapLevel:0 slice:0];
	size_t nz = 0;
	for (uint8_t b: bytes) nz += b != 0;
	auto texel = [&](NSUInteger x, NSUInteger y, NSUInteger z) {
		const size_t off = ((static_cast<size_t>(z) * h + y) * w + x) * bpp;
		if (bpp == 16) {
			float v[4];
			std::memcpy(v, &bytes[off], sizeof(v));
			return std::make_tuple(v[0], v[1], v[2], v[3]);
		}
		return std::make_tuple(bytes[off] / 255.f, bytes[off + 1] / 255.f, bytes[off + 2] / 255.f, bytes[off + 3] / 255.f);
	};
	const auto c0 = texel(0, 0, 0);
	const auto cm = texel(w / 2, h / 2, d / 2);
	std::fprintf(stderr, "[OP] %d out tex %lux%lux%lu fmt=%lu nz=%.2f%% corner=(%g,%g,%g,%g) centre=(%g,%g,%g,%g)\n", op,
	             (unsigned long)w, (unsigned long)h, (unsigned long)d, (unsigned long)tex.pixelFormat,
	             100.0 * static_cast<double>(nz) / static_cast<double>(bytes.size()), std::get<0>(c0), std::get<1>(c0),
	             std::get<2>(c0), std::get<3>(c0), std::get<0>(cm), std::get<1>(cm), std::get<2>(cm), std::get<3>(cm));
}

static void TraceFlushOutputs(const char* why) {
	MetalContext::Instance().CommitCommandBuffer(true);
	for (const auto& [op, res]: s_trace_prev_outputs) {
		if (void* tex = GetComputeWrittenImageForAddress(res.addr)) {
			TraceTextureOutput(op, tex);
			continue;
		}
		std::fprintf(stderr, "[OP] %d out %s addr=0x%llx size=%llu nz=%.2f%%\n", op, why,
		             (unsigned long long)res.addr, (unsigned long long)res.size, TraceNonZero(res.addr, res.size));
	}
	s_trace_prev_outputs.clear();
}

// Draw op: vertex buffers are inputs; the color target 0 (1080p-sized, assumed) is the output.
static void TraceDrawOp(const ShaderVertexInputInfo& vi, uint64_t rt_addr, uint64_t hash) {
	if (!s_trace_on) return;
	TraceFlushOutputs("prev-op");
	const int op = s_trace_op++;
	std::fprintf(stderr, "[OP] %d draw vs=0x%016llx rt=0x%llx\n", op, (unsigned long long)hash, (unsigned long long)rt_addr);
	for (int i = 0; i < vi.buffers_num && i < 8; ++i) {
		const auto& vb = vi.buffers[i];
		const uint64_t size = static_cast<uint64_t>(vb.stride) * vb.num_records;
		if (vb.addr == 0 || size == 0) continue;
		std::fprintf(stderr, "[OP] %d in  vbuf addr=0x%llx size=%llu nz=%.2f%%\n", op, (unsigned long long)vb.addr,
		             (unsigned long long)size, TraceNonZero(vb.addr, size));
	}
	if (rt_addr != 0) {
		s_trace_prev_outputs.push_back({op, {rt_addr, 8294400ull}});
	}
}

// Called per flip: the ops of guest frame (target) run between the ticks of target-1 and target.
static void TraceOpsFlipTick(uint64_t flip) {
	static const uint64_t target = KYTY_ENV("KYTY_TRACE_OPS_FLIP") ? std::strtoull(KYTY_ENV("KYTY_TRACE_OPS_FLIP"), nullptr, 0) : 0ull;
	if (target == 0) return;
	if (flip + 1 == target) {
		s_trace_flip = target;
		s_trace_on = true;
		s_trace_op = 0;
		std::fprintf(stderr, "[OP] window open for flip %llu\n", (unsigned long long)target);
	} else if (flip == target && s_trace_on) {
		TraceFlushOutputs("flip-end");
		s_trace_on = false;
		std::fprintf(stderr, "[OP] window closed, ops=%d\n", s_trace_op);
	}
}

// Called before each compute op with its resources: logs the inputs now, and the previous op's outputs.
static void TraceComputeOp(const ShaderComputeInputInfo& info, uint64_t hash) {
	if (!s_trace_on || info.stage.program == nullptr || info.stage.resources == nullptr) return;
	TraceFlushOutputs("prev-op");
	const auto& prog = *info.stage.program;
	const auto& snap = *info.stage.resources;
	const int op = s_trace_op++;
	std::fprintf(stderr, "[OP] %d compute hash=0x%016llx\n", op, (unsigned long long)hash);
	for (const auto& b: prog.bindings.descriptors) {
		const auto kind = static_cast<uint32_t>(b.kind);
		if (kind < ShaderRecompiler::IR::FirstImageBinding ||
		    kind >= static_cast<uint32_t>(ShaderRecompiler::IR::DescriptorBindingKind::Samplers)) continue;
		const bool storage = kind >= static_cast<uint32_t>(ShaderRecompiler::IR::FirstStorageImageBinding);
		for (uint32_t res: b.resources) {
			if (res >= snap.images.size()) continue;
			ShaderTextureResource tx {};
			for (size_t f = 0; f < 8; ++f) tx.fields[f] = snap.images[res].dwords[f];
			const uint64_t addr = tx.Base40();
			const uint64_t size = static_cast<uint64_t>(tx.Width5() + 1u) * (tx.Height5() + 1u) * 4u;
			if (addr == 0) continue;
			if (storage) {
				s_trace_prev_outputs.push_back({op, {addr, size}});
			} else {
				std::fprintf(stderr, "[OP] %d in  img addr=0x%llx size=%llu nz=%.2f%%\n", op, (unsigned long long)addr,
				             (unsigned long long)size, TraceNonZero(addr, size));
			}
		}
	}
	for (size_t bi = 0; bi < snap.buffers.size() && bi < 64; ++bi) {
		const auto& d = snap.buffers[bi];
		const uint64_t addr = (static_cast<uint64_t>(d.dwords[1] & 0xFFFF) << 32) | d.dwords[0];
		if (addr == 0) continue;
		const uint64_t size = bi < prog.info.buffers.size() && prog.info.buffers[bi].max_byte_extent != 0
		                          ? prog.info.buffers[bi].max_byte_extent : 4096u;
		const bool written = bi < prog.info.buffers.size() && prog.info.buffers[bi].written;
		if (written) {
			s_trace_prev_outputs.push_back({op, {addr, size}});
		} else {
			std::fprintf(stderr, "[OP] %d in  buf addr=0x%llx size=%llu nz=%.2f%%\n", op, (unsigned long long)addr,
			             (unsigned long long)size, TraceNonZero(addr, size));
			// KYTY_TRACE_WORDS=1: also print the first 8 dwords (hex and float) to read small constant buffers.
			static const bool s_words = KYTY_ENV("KYTY_TRACE_WORDS") != nullptr;
			if (s_words) {
				auto wa = MetalBufferManager::Instance().GetBufferForGuestRange(addr, 32);
				if (wa.buffer != nil) {
					const uint32_t* p = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>([wa.buffer contents]) + wa.offset);
					for (int i = 0; i < 8; ++i) {
						float f;
						std::memcpy(&f, &p[i], sizeof(f));
						std::fprintf(stderr, "[OP] %d   dw%d=0x%08x (%g)\n", op, i, p[i], static_cast<double>(f));
					}
				}
			}
		}
	}
}

static std::mutex s_disp_stat_mutex;
static uint64_t s_disp_stat[6] = {};

void CountDispatchStat(int kind) {
	std::lock_guard<std::mutex> l(s_disp_stat_mutex);
	if (kind >= 0 && kind < 6) ++s_disp_stat[kind];
}

static std::mutex s_cs_skip_mutex;
static std::unordered_map<uint64_t, uint64_t> s_cs_skip_count;
static uint64_t s_cs_skip_total = 0;

// Ranking of skipped compute shaders by dispatch count (top 20 with cumulative share).
static void PrintSkippedComputeRanking(uint64_t flip) {
	std::lock_guard<std::mutex> cl(s_cs_skip_mutex);
	if (s_cs_skip_total == 0) return;
	std::vector<std::pair<uint64_t, uint64_t>> ranked;
	for (const auto& [hash, count]: s_cs_skip_count) ranked.emplace_back(count, hash);
	std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	std::fprintf(stderr, "[CsRank] flip=%llu skipped_total=%llu shaders=%zu\n", (unsigned long long)flip,
	             (unsigned long long)s_cs_skip_total, ranked.size());
	uint64_t cum = 0;
	for (size_t i = 0; i < ranked.size() && i < 20; ++i) {
		cum += ranked[i].first;
		std::fprintf(stderr, "[CsRank]   %2zu hash=0x%016llx dispatches=%llu cum=%.1f%%\n", i + 1,
		             (unsigned long long)ranked[i].second, (unsigned long long)ranked[i].first,
		             100.0 * (double)cum / (double)s_cs_skip_total);
	}
}

// Called once per guest flip: prints draw accounting every 25 flips and resets the window.
void MetalDrawFrameStatsTick(uint64_t flip) {
	TraceOpsFlipTick(flip);
	std::lock_guard<std::mutex> l(s_draw_stats_mutex);
	if (flip == 0 || flip % 25 != 0) return;
	if (flip % 100 == 0) PrintSkippedComputeRanking(flip);
	{
		std::lock_guard<std::mutex> dl(s_disp_stat_mutex);
		const double f = static_cast<double>(flip);
		std::fprintf(stderr,
		             "[DispatchPerFlip] flip=%llu cp=%.1f renderer=%.1f encoded=%.1f nopipe=%.1f noenc=%.1f emptygrid=%.1f (totals since start)\n",
		             (unsigned long long)flip, s_disp_stat[0] / f, s_disp_stat[1] / f, s_disp_stat[2] / f,
		             s_disp_stat[3] / f, s_disp_stat[4] / f, s_disp_stat[5] / f);
	}
	std::fprintf(stderr, "[DrawStats] flip=%llu submitted=%u executed=%u dropped_vs=%u dropped_ps=%u dropped_pipe=%u\n",
	             (unsigned long long)flip, s_ds_submitted, s_ds_executed, s_ds_drop_vs, s_ds_drop_ps, s_ds_drop_pipe);
	s_ds_submitted = s_ds_executed = s_ds_drop_vs = s_ds_drop_ps = s_ds_drop_pipe = 0;
}
// KYTY_MSL_ONLY: draws whose vertex or pixel shader has no Metal function are skipped. Counts per
// shader hash feed the coverage ranking (draws cost per unsupported shader).
namespace {
std::mutex s_msl_skip_mutex;
std::unordered_map<uint64_t, uint32_t> s_msl_skip_by_hash;
uint32_t s_msl_skipped_draws = 0;

void MslNoteSkippedDraw(uint64_t vs_hash, uint64_t ps_hash) {
	std::lock_guard<std::mutex> lock(s_msl_skip_mutex);
	++s_msl_skipped_draws;
	++s_msl_skip_by_hash[vs_hash];
	if (ps_hash != 0) {
		++s_msl_skip_by_hash[ps_hash];
	}
	if (s_msl_skipped_draws % 500 == 0) {
		std::vector<std::pair<uint32_t, uint64_t>> ranked;
		for (const auto& [hash, count]: s_msl_skip_by_hash) {
			ranked.emplace_back(count, hash);
		}
		std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		std::fprintf(stderr, "[MSL-skip] skipped_draws=%u top:", s_msl_skipped_draws);
		for (size_t i = 0; i < ranked.size() && i < 8; ++i) {
			std::fprintf(stderr, " 0x%016llx=%u", (unsigned long long)ranked[i].second, ranked[i].first);
		}
		std::fprintf(stderr, "\n");
	}
}
} // namespace


bool UseNativeMetalBackend() noexcept {
	static const bool enabled = [] {
		const char* b = KYTY_ENV("KYTY_BACKEND");
		if (b != nullptr && std::strcmp(b, "metal") == 0) return true;
		const char* m = KYTY_ENV("KYTY_USE_METAL");
		return m != nullptr && (std::strcmp(m, "1") == 0 || std::strcmp(m, "true") == 0);
	}();
	return enabled;
}

MetalRenderer& MetalRenderer::Instance() {
	static MetalRenderer instance;
	return instance;
}

MetalRenderer::MetalRenderer() = default;
MetalRenderer::~MetalRenderer() {
	Shutdown();
}

bool MetalRenderer::Initialize() {
	if (m_active) return true;
	if (!MetalContext::Instance().Initialize()) {
		LOGF_COLOR(Log::Color::BrightRed, "MetalRenderer: Failed to initialize MetalContext\n");
		return false;
	}
	m_active = true;
	LOGF_COLOR(Log::Color::BrightGreen, "MetalRenderer: Native Apple Metal backend initialized successfully\n");
	return true;
}

void MetalRenderer::Shutdown() {
	if (!m_active) return;
	WaitIdle();
	MetalShaderManager::Instance().ClearCache();
	MetalPipelineManager::Instance().ClearCaches();
	MetalContext::Instance().Shutdown();
	m_active = false;
}

void MetalRenderer::Flush() {
	if (!m_active) return;
	std::lock_guard lock(m_mutex);
	MetalDrawExecutor::Instance().EndRenderPass();
	// KYTY_SYNC_FLUSH=1 (diagnostic): wait for the GPU at every flush; a frame that is right only with this flag points at a hazard.
	static const bool s_sync_flush = KYTY_ENV("KYTY_SYNC_FLUSH") != nullptr;
	MetalContext::Instance().CommitCommandBuffer(s_sync_flush);
}

void MetalRenderer::MemoryBarrier(const Pm4::AcquireMemInfo* acquire) {
	if (!m_active) return;
	std::lock_guard lock(m_mutex);
	MetalDrawExecutor::Instance().EndRenderPass();
	MetalContext::Instance().EndCurrentEncoder();
	OrderMemoryBarrier();
	static const bool trace = KYTY_ENV("KYTY_TRACE_MEMORY_SYNC") != nullptr;
	if (trace && acquire != nullptr) {
		std::fprintf(stderr,"[MemoryAcquire] base=0x%llx size=0x%llx full=%d coher=%08x gcr=%08x unknown=%08x\n",
		    (unsigned long long)acquire->range.base,(unsigned long long)acquire->range.size,acquire->range.full,
		    acquire->coher_control,acquire->gcr_control,acquire->cache.unknown);
	}
}

void MetalRenderer::CaptureFrameTick(uint64_t frame) {
	if (!m_active) return;
	// Capture start/stop may commit the shared command buffer. Serialize them
	// with encoding, just like Flush on the present thread.
	std::lock_guard lock(m_mutex);
	GpuCaptureTick(frame);
}

void MetalRenderer::WaitIdle() {
	if (!m_active) return;
	std::lock_guard lock(m_mutex);
	MetalDrawExecutor::Instance().EndRenderPass();
	MetalContext::Instance().WaitUntilIdle();
}

static id<MTLTexture> s_last_rt_tex = nil;
static std::atomic<uint64_t> s_last_rt_addr {0};
static std::atomic<uint32_t> s_last_rt_width {0};
static std::atomic<uint32_t> s_last_rt_height {0};
static std::atomic<uint32_t> s_last_rt_fmt {0};

static id<MTLTexture> s_best_rt_tex = nil;
static std::atomic<uint64_t> s_best_rt_addr {0};
static std::atomic<uint32_t> s_best_rt_width {0};
static std::atomic<uint32_t> s_best_rt_height {0};
static std::atomic<uint32_t> s_best_rt_fmt {0};
static uint64_t s_best_rt_draws = 0;
static std::unordered_map<uint64_t, uint64_t> s_rt_draw_counts;

void* MetalRenderer::GetLastRenderTexture() const {
	if (s_last_rt_tex != nil) {
		return (__bridge void*)s_last_rt_tex;
	}
	return (__bridge void*)s_best_rt_tex;
}

static std::unordered_map<uint32_t, id<MTLTexture>> s_rt_by_format;
struct TrackedRt {
	id<MTLTexture> tex = nil;
	uint32_t width = 0, height = 0, format = 0;
	uint64_t sequence = 0;
	uint64_t flip = 0;                         // last flip a pass rendered into it
	uint64_t guest_bytes = 0, guest_hash = 0;  // legacy sampled-hash path outside canonical ownership
	PhysicalBackingSnapshot physical;
	bool canonical=false;
	uint64_t writer_serial=0;
};
// One entry per (guest address, metal format): the same address is commonly bound
// as both RGBA8 and RG11B10 across passes.
static std::unordered_map<uint64_t, std::unordered_map<uint32_t, TrackedRt>> s_rt_by_addr;
static std::atomic<uint64_t> s_rt_sequence {0};

// Guest addresses we render into with linear texture views. When such a surface is
// later sampled through a guest descriptor, its declared tile mode must be ignored:
// the data in memory is linear because this backend wrote it.
static std::unordered_set<uint64_t> s_render_target_addresses;
static std::mutex                   s_render_target_mutex;
static void NoteLinearRenderAddress(uint64_t addr) {
	std::lock_guard<std::mutex> lock(s_render_target_mutex);
	s_render_target_addresses.insert(addr);
}

static id<MTLTexture> NewestRenderTargetFor(uint64_t addr, uint32_t w, uint32_t h, MTLPixelFormat want) {
	static const bool s_off = false;
	if (s_off || addr == 0 || want == MTLPixelFormatInvalid || IsCompressedFormat(want)) return nil;
	std::lock_guard<std::mutex> lock(s_render_target_mutex);
	const auto it = s_rt_by_addr.find(addr);
	if (it == s_rt_by_addr.end()) return nil;
	const size_t want_bpp = BytesPerPixel(want);
	const TrackedRt* best = nullptr;
	int best_score = 0;
	for (const auto& [f, rt]: it->second) {
		// A render target rendered as the base level of a mip chain (the scene colour that a bloom pyramid is built from) still holds the
		// scene in level 0; consumers read level 0 of the T#.
		if (!rt.tex || rt.tex.width != w || rt.tex.height != h || rt.tex.textureType != MTLTextureType2D) continue;
		const int score = rt.tex.pixelFormat == want ? 4 : (want_bpp != 0 && BytesPerPixel(rt.tex.pixelFormat) == want_bpp ? 3 : 0);
		if (score > best_score || (score == best_score && score > 0 && best != nullptr && rt.sequence > best->sequence)) {
			best = &rt;
			best_score = score;
		}
	}
	return best != nullptr ? best->tex : nil;
}

// Only retire known canonical producers here. A newer private compute writer
// or an untracked producer needs its own materialization, not a guest-memory guess.
static bool CanonicalTargetWasRemapped(uint64_t addr) {
	std::vector<CanonicalImageHistory> targets;
	{
		std::lock_guard<std::mutex> lock(s_render_target_mutex);
		const auto found=s_rt_by_addr.find(addr);
		if(found==s_rt_by_addr.end() || found->second.empty())return false;
		for(const auto& [format,target]:found->second)targets.push_back({target.physical,
		    {reinterpret_cast<uintptr_t>((__bridge void*)target.tex),target.writer_serial},target.canonical});
	}
	std::vector<PhysicalSpan> refresh;
	(void)CanonicalMipSpans(addr,1,refresh);
	std::lock_guard<std::mutex> writer_lock(s_cs_storage_mutex);
	const auto writer=s_cs_storage.find(addr);
	std::optional<CanonicalImageHistory> last_writer;
	if(writer!=s_cs_storage.end())last_writer=CanonicalImageHistory{writer->second.canonical,
	    {reinterpret_cast<uintptr_t>((__bridge void*)writer->second.tex),writer->second.serial},writer->second.canonical.Valid()};
	return CanImportRemappedImage(targets,s_canonical_mappings,addr,last_writer);
}

static bool IsLinearRenderTargetAddress(uint64_t addr) {
	std::lock_guard<std::mutex> lock(s_render_target_mutex);
	return s_render_target_addresses.find(addr) != s_render_target_addresses.end();
}

void* MetalRenderer::GetRenderTextureForFormat(uint32_t format) const {
	const auto it = s_rt_by_format.find(format);
	return it != s_rt_by_format.end() ? (__bridge void*)it->second : nullptr;
}

void* MetalRenderer::GetRenderTextureForAddress(uint64_t addr) const {
	std::lock_guard<std::mutex> lock(s_render_target_mutex);
	const auto it = s_rt_by_addr.find(addr);
	if (it == s_rt_by_addr.end()) {
		return nullptr;
	}
	const TrackedRt* newest = nullptr;
	for (const auto& [format, rt]: it->second) {
		if (newest == nullptr || rt.sequence > newest->sequence) {
			newest = &rt;
		}
	}
	return newest != nullptr ? (__bridge void*)newest->tex : nullptr;
}

void* MetalRenderer::GetRenderTextureForAddressFormat(uint64_t addr, uint32_t format) const {
	std::lock_guard<std::mutex> lock(s_render_target_mutex);
	const auto it = s_rt_by_addr.find(addr);
	if (it == s_rt_by_addr.end()) {
		return nullptr;
	}
	const auto found = it->second.find(format);
	return found != it->second.end() ? (__bridge void*)found->second.tex : nullptr;
}

// KYTY_DUMP_IMG=<addr[,addr...]|all> KYTY_DUMP_IMG_TRIGGER=<file> KYTY_DUMP_IMG_PREFIX=<path prefix>: when the trigger file appears
// (consumed once) the presenter calls this, and every tracked render target / compute storage image at the listed guest addresses
// (all images of at least 200k pixels for "all", half resolution) is read back and written as <prefix>-<present>-<addr>-<rt|cs>-<w>x<h>-f<fmt>.ppm,
// with one [DumpImg] log line carrying min/max/NaN counts. Floats are shown as Reinhard + gamma. This is how the lit scene, TAA and
// tonemap inputs are compared pass by pass without a GPU trace.
static float DumpHalfToFloat(uint16_t h) {
	const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1fu, man = h & 0x3ffu;
	float          v;
	if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
	else if (exp == 31) v = man != 0 ? std::nanf("") : INFINITY;
	else v = std::ldexp(static_cast<float>(man | 0x400u), static_cast<int>(exp) - 25);
	return sign != 0 ? -v : v;
}

static float DumpSmallFloat(uint32_t bits, uint32_t man_bits) {
	const uint32_t exp = bits >> man_bits, man = bits & ((1u << man_bits) - 1u);
	if (exp == 31) return man != 0 ? std::nanf("") : INFINITY;
	if (exp == 0) return std::ldexp(static_cast<float>(man), -14 - static_cast<int>(man_bits));
	return std::ldexp(1.0f + static_cast<float>(man) / static_cast<float>(1u << man_bits), static_cast<int>(exp) - 15);
}

// KYTY_PASS_TRACE_TRIGGER=<file>: a targeted frame snapshot. When the file appears (consumed once) every compute dispatch of the
// next presented frame is run to completion on its own and the float images it binds are read back: one [PassTrace] line per dispatch
// names the kernel and gives NaN/Inf counts and min/max of each image it reads (IN) and writes (OUT). The first dispatch whose OUT has
// NaN while every IN is clean is where a NaN is born.
// KYTY_LOG_ADDR=<addr[,addr...]>: every render-target pass and compute image binding at those guest addresses, with the presented-frame counter.
static bool AddrLogWanted(uint64_t addr) {
	static const std::vector<uint64_t> s_list = [] {
		std::vector<uint64_t> v;
		const char* value = KYTY_ENV("KYTY_LOG_ADDR");
		while (value != nullptr && *value != '\0') {
			char* end = nullptr;
			const uint64_t a = std::strtoull(value, &end, 0);
			if (end == value) break;
			if (a != 0) v.push_back(a);
			value = *end == ',' ? end + 1 : nullptr;
		}
		return v;
	}();
	return !s_list.empty() && std::find(s_list.begin(), s_list.end(), addr) != s_list.end();
}

struct PassTraceItem {
	uint64_t       addr;
	id<MTLTexture> tex;
	bool           store;
	uint32_t       res;
	const char*    via; // which lookup produced the texture (compute image binding)
};
static std::vector<PassTraceItem> s_pass_items;
// KYTY_PASS_TRACE_CS=<hash>: every dispatch of that kernel is traced (inputs and outputs read back), in any frame.
static bool PassTraceCsFilter() {
	static const uint64_t s_hash = KYTY_ENV("KYTY_PASS_TRACE_CS") ? std::strtoull(KYTY_ENV("KYTY_PASS_TRACE_CS"), nullptr, 0) : 0ull;
	return s_hash != 0 && s_alloc_cs_hash == s_hash;
}

static bool PassTraceCsBuffers() {
	static const bool on = KYTY_ENV("KYTY_PASS_TRACE_CSBUF") != nullptr;
	static const uint32_t limit = KYTY_ENV("KYTY_PASS_TRACE_CSBUF_N") ?
	    std::strtoul(KYTY_ENV("KYTY_PASS_TRACE_CSBUF_N"), nullptr, 0) : 16u;
	static uint32_t seen = 0;
	return on && PassTraceCsFilter() && seen++ < limit;
}

// KYTY_PASS_TRACE_CS=<hash>[,<hash>...]: the first 16 dispatches of each listed kernel are traced the same way at any flip (a LUT
// builder runs once, long before a triggered frame), with the first dwords of its buffers. KYTY_PASS_TRACE_DUMP=<dir> also writes the
// 3D volumes such a dispatch reads and writes as raw texels: <dir>/cs<hash>_<n>_<IN|OUT>_r<res>_<addr>_<w>x<h>x<d>_f<pixel format>.raw.
static bool s_pass_force = false;
static bool PassTraceForcedKernel(uint64_t hash) {
	static const std::vector<uint64_t> s_list = [] {
		std::vector<uint64_t> list;
		for (const char* p = KYTY_ENV("KYTY_PASS_TRACE_CS"); p != nullptr && *p != '\0';) {
			char* end = nullptr;
			const uint64_t v = std::strtoull(p, &end, 16);
			if (end == p) break;
			list.push_back(v);
			p = *end == ',' ? end + 1 : end;
		}
		return list;
	}();
	static std::map<uint64_t, uint32_t> s_seen;
	// KYTY_PASS_TRACE_CS_N=<n>: dispatches traced per listed kernel (default 16).
	static const uint32_t s_limit = KYTY_ENV("KYTY_PASS_TRACE_CS_N") ? static_cast<uint32_t>(std::atoi(KYTY_ENV("KYTY_PASS_TRACE_CS_N"))) : 16u;
	// KYTY_PASS_TRACE_CS_FROM=<flip>: dispatches before that flip are not counted (kernels that also run during start-up).
	static const uint64_t s_from = KYTY_ENV("KYTY_PASS_TRACE_CS_FROM") ? std::strtoull(KYTY_ENV("KYTY_PASS_TRACE_CS_FROM"), nullptr, 0) : 0ull;
	if (Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed) < s_from) return false;
	return std::find(s_list.begin(), s_list.end(), hash) != s_list.end() && s_seen[hash]++ < s_limit;
}

// KYTY_MSL_NAN_TRACE=<hash>[,<hash>...]: guest shader hashes whose float results are checked for NaN (pixel: first id into the colour output;
// compute: first id into a debug buffer, printed as [NanTrace] in a traced frame).
static bool NanTraceCs(uint64_t hash) {
	static const std::vector<uint64_t> list = [] {
		std::vector<uint64_t> v;
		if (const char* trace = KYTY_ENV("KYTY_MSL_NAN_TRACE")) {
			for (const char* p = trace; *p != '\0';) {
				char*          end = nullptr;
				const uint64_t h   = std::strtoull(p, &end, 0);
				if (end == p) break;
				v.push_back(h);
				p = *end == ',' ? end + 1 : end;
			}
		}
		return v;
	}();
	return std::find(list.begin(), list.end(), hash) != list.end();
}

static bool PassTraceActive() {
	if (s_pass_force) return true;
	static const char* s_trigger = KYTY_ENV("KYTY_PASS_TRACE_TRIGGER");
	static const uint64_t s_target_frame = [] {
		const char* v = KYTY_ENV("KYTY_PASS_TRACE_FRAME");
		return v ? std::strtoull(v, nullptr, 10) : ~0ull;
	}();
	if ((s_trigger == nullptr || *s_trigger == '\0') && s_target_frame == ~0ull) return false;
	static uint64_t s_frame = ~0ull;
	static uint64_t s_count = 0;
	// A present can split a guest render sequence. Capture consecutive flips so
	// a clear before the lighting passes is not outside the diagnostic window.
	static const uint64_t s_span = [] {
		const char* value = KYTY_ENV("KYTY_PASS_TRACE_FLIPS");
		return std::clamp<uint64_t>(value ? std::strtoull(value, nullptr, 10) : 1u, 1u, 16u);
	}();
	const uint64_t  flip    = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (s_frame != ~0ull && flip >= s_frame && flip - s_frame < s_span) return true;
	if (s_frame != ~0ull) {
		std::fprintf(stderr, "[PassTrace] end frame flip=%llu dispatches=%llu\n", static_cast<unsigned long long>(s_frame), static_cast<unsigned long long>(s_count));
		s_frame = ~0ull;
	}
	if (s_target_frame != ~0ull && s_frame == ~0ull && flip >= s_target_frame && flip < s_target_frame + s_span) {
		s_frame = flip;
		s_count = 0;
		s_pass_items.clear();
		std::fprintf(stderr, "[PassTrace] begin frame flip=%llu (via KYTY_PASS_TRACE_FRAME=%llu)\n",
		             static_cast<unsigned long long>(flip), static_cast<unsigned long long>(s_target_frame));
		return true;
	}
	if (s_trigger == nullptr || *s_trigger == '\0') return false;
	// The trigger file is polled once per flip (or every 200 ms while flips stall): a remove() per draw / dispatch cost about a
	// third of the GPU thread (lstat in the profile) whenever KYTY_PASS_TRACE_TRIGGER was set, which every scripted run does.
	static uint64_t s_polled_flip = ~0ull;
	static auto     s_polled_at   = std::chrono::steady_clock::time_point {};
	const auto      now           = std::chrono::steady_clock::now();
	if (flip == s_polled_flip && now - s_polled_at < std::chrono::milliseconds(200)) return false;
	s_polled_flip = flip;
	s_polled_at   = now;
	if (std::remove(s_trigger) == 0) {
		s_frame = flip;
		s_count = 0;
		s_pass_items.clear();
		std::fprintf(stderr, "[PassTrace] begin frame flip=%llu\n", static_cast<unsigned long long>(flip));
		return true;
	}
	return false;
}

// KYTY_PASS_TRACE_SENTINEL=1: in a traced frame every float colour target starts each draw filled with (1024, 0.5, 0.5, 1), and the trace of
// the draw reports how many pixels still hold that value ("untouched"): the coverage of the draw, told apart from stale memory. The frame's
// later passes see the sentinel instead of what the game had there, so only use it to read per-draw coverage.


// KYTY_PASS_TRACE_FRAGS=1 (or the sentinel mode): every traced draw reports "FRAGS n", the samples it wrote (occlusion counting).
static bool PassTraceFrags() {
	static const bool s_on = KYTY_ENV("KYTY_PASS_TRACE_FRAGS") != nullptr;
	return s_on && PassTraceActive();
}

static void PassTraceNote(uint64_t addr, id<MTLTexture> tex, bool store, uint32_t res, const char* via = "") {
	if (tex == nil || (tex.textureType != MTLTextureType2D && tex.textureType != MTLTextureType3D) || !(PassTraceActive() || PassTraceCsFilter())) return;
	for (const auto& it: s_pass_items) {
		if (it.tex == tex && it.store == store) return;
	}
	s_pass_items.push_back({addr, tex, store, res, via});
}
// The binding replaced a noted image by a view of its T# level range: the trace reports what the shader really reads or writes.
// Only the item just noted for this binding: a kernel that reads level N and writes level N+1 of one texture has two items with it.
static void PassTraceRebind(id<MTLTexture> full, id<MTLTexture> view, bool store) {
	if (full == nil || view == nil || full == view) return;
	for (auto it = s_pass_items.rbegin(); it != s_pass_items.rend(); ++it) {
		if (it->tex == full && it->store == store) {
			it->tex = view;
			return;
		}
	}
}

static std::string PassTraceStatsImpl(id<MTLTexture> tex);
// KYTY_PASS_TRACE_PIXEL=u,v[/u,v...] (fractions; ';' also separates of the image size): every traced image also reports its texel there ("px=" list), so one
// screen position can be followed through the passes of a frame (G-buffer, lighting, HDR, tonemap) whatever the image sizes.
static const std::vector<std::pair<float, float>>& PassTracePixels() {
	static const std::vector<std::pair<float, float>> s_px = [] {
		std::vector<std::pair<float, float>> v;
		for (const char* p = KYTY_ENV("KYTY_PASS_TRACE_PIXEL"); p != nullptr && *p != '\0';) {
			float u = 0, w = 0;
			int   used = 0;
			if (std::sscanf(p, "%f,%f%n", &u, &w, &used) != 2) break;
			v.emplace_back(u, w);
			p += used;
			if (*p == ';' || *p == '/') ++p;
		}
		return v;
	}();
	return s_px;
}
// The probe texels of a read-back level 0 (tightly packed rows of `row` bytes).
static std::string PassTraceProbe(MTLPixelFormat pf, const uint8_t* src, size_t w, size_t h, size_t row, size_t bpp) {
	std::string out;
	for (const auto& [u, v]: PassTracePixels()) {
		const size_t x = std::min(w - 1, static_cast<size_t>(u * static_cast<float>(w)));
		const size_t y = std::min(h - 1, static_cast<size_t>(v * static_cast<float>(h)));
		const uint8_t* px = src + y * row + x * bpp;
		char t[160];
		if (pf == MTLPixelFormatRGBA16Float || pf == MTLPixelFormatRG16Float || pf == MTLPixelFormatR16Float) {
			float c[4] = {0, 0, 0, 0};
			for (size_t k = 0; k < bpp / 2 && k < 4; ++k) { uint16_t hv; std::memcpy(&hv, px + 2 * k, 2); c[k] = DumpHalfToFloat(hv); }
			std::snprintf(t, sizeof(t), " px(%zu,%zu)=%g,%g,%g,%g", x, y, c[0], c[1], c[2], c[3]);
		} else if (pf == MTLPixelFormatRG11B10Float) {
			uint32_t b; std::memcpy(&b, px, 4);
			std::snprintf(t, sizeof(t), " px(%zu,%zu)=%g,%g,%g", x, y, DumpSmallFloat(b & 0x7ffu, 6), DumpSmallFloat((b >> 11) & 0x7ffu, 6),
			              DumpSmallFloat((b >> 22) & 0x3ffu, 5));
		} else if (pf == MTLPixelFormatR32Float || pf == MTLPixelFormatRG32Float || pf == MTLPixelFormatRGBA32Float) {
			float c[4] = {0, 0, 0, 0};
			std::memcpy(c, px, std::min<size_t>(bpp, 16));
			std::snprintf(t, sizeof(t), " px(%zu,%zu)=%g,%g,%g,%g", x, y, c[0], c[1], c[2], c[3]);
		} else {
			std::snprintf(t, sizeof(t), " px(%zu,%zu)=", x, y);
			std::string bytes = t;
			for (size_t k = 0; k < bpp && k < 8; ++k) { char b2[8]; std::snprintf(b2, sizeof(b2), "%02x", px[k]); bytes += b2; }
			out += bytes;
			continue;
		}
		out += t;
	}
	return out;
}
// The readback buffers of a traced frame (hundreds of full-screen copies) must not pile up in the thread's autorelease pool.
static std::string PassTraceStats(id<MTLTexture> tex) {
	@autoreleasepool { return PassTraceStatsImpl(tex); }
}
// Level 0 of a 3D texture as tightly packed texels (waits for the GPU).
static bool ReadbackVolume(id<MTLTexture> tex, std::vector<uint8_t>& out) {
	const size_t bpp = BytesPerPixel(tex.pixelFormat);
	const size_t w = tex.width, h = tex.height, d = tex.depth, rowb = w * bpp;
	if (tex.textureType != MTLTextureType3D || bpp == 0 || bpp > 16 || w * h * d > (4u << 20)) return false;
	id<MTLBuffer>        b  = KytyNewBuffer(MetalContext::Instance().GetDevice(), rowb * h * d, MTLResourceStorageModeShared, __LINE__);
	id<MTLCommandBuffer> c  = [MetalContext::Instance().GetCommandQueue() commandBuffer];
	id<MTLBlitCommandEncoder> bl = [c blitCommandEncoder];
	if (b == nil || c == nil || bl == nil) return false;
	[bl copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, d)
	           toBuffer:b destinationOffset:0 destinationBytesPerRow:rowb destinationBytesPerImage:rowb * h];
	[bl endEncoding];
	KytyWatchCommandBuffer(c, "metalRenderer.mm:c#1");
	KytyBudgetedCommit(c);
	[c waitUntilCompleted];
	out.assign(static_cast<const uint8_t*>(b.contents), static_cast<const uint8_t*>(b.contents) + rowb * h * d);
	return true;
}

static std::string PassTraceStatsImpl(id<MTLTexture> tex) {
	const MTLPixelFormat pf  = tex.pixelFormat;
	const size_t         bpp = BytesPerPixel(pf);
	const size_t         w = tex.width, h = tex.height;
	const bool is_float = pf == MTLPixelFormatRGBA16Float || pf == MTLPixelFormatRG11B10Float || pf == MTLPixelFormatRG16Float ||
	                      pf == MTLPixelFormatR16Float || pf == MTLPixelFormatR32Float || pf == MTLPixelFormatRG32Float || pf == MTLPixelFormatRGBA32Float;
	if (tex.textureType != MTLTextureType2D) {
		// 3D volume (a LUT): byte range over all slices; arrays are not read.
		std::vector<uint8_t> volume;
		if (!ReadbackVolume(tex, volume)) return "3d/array " + std::to_string(w) + "x" + std::to_string(h) + "x" + std::to_string(tex.depth);
		const size_t d = tex.depth, rowb = w * bpp;
		const uint8_t* bytes = volume.data();
		uint32_t lo = 255, hi = 0;
		size_t   nonzero = 0;
		for (size_t i = 0; i < rowb * h * d; ++i) { lo = std::min<uint32_t>(lo, bytes[i]); hi = std::max<uint32_t>(hi, bytes[i]); nonzero += bytes[i] != 0; }
		// The eight corner voxels (x, y, z each 0 or max) of a 4-byte volume as 10:10:10 / bytes: the axes of a colour LUT show up here.
		std::string corners;
		if (bpp == 4) {
			for (int c = 0; c < 8; ++c) {
				const size_t cx = (c & 1) ? w - 1 : 0, cy = (c & 2) ? h - 1 : 0, cz = (c & 4) ? d - 1 : 0;
				uint32_t v;
				std::memcpy(&v, bytes + ((cz * h + cy) * w + cx) * 4, 4);
				char ct[48];
				std::snprintf(ct, sizeof(ct), " [%d%d%d]=%u,%u,%u", int(c & 1), int((c >> 1) & 1), int((c >> 2) & 1), v & 0x3ffu, (v >> 10) & 0x3ffu, (v >> 20) & 0x3ffu);
				corners += ct;
			}
		}
		if (bpp == 4) {
			for (int k = 1; k <= 7; ++k) {
				const size_t cx = (w - 1) * k / 8, cy = (h - 1) * k / 8, cz = (d - 1) * k / 8;
				uint32_t v;
				std::memcpy(&v, bytes + ((cz * h + cy) * w + cx) * 4, 4);
				char ct[48];
				std::snprintf(ct, sizeof(ct), " diag%d/8=%u,%u,%u", k, v & 0x3ffu, (v >> 10) & 0x3ffu, (v >> 20) & 0x3ffu);
				corners += ct;
			}
		}
		char t[160];
		std::snprintf(t, sizeof(t), "3d %zux%zux%zu fmt-bytes min=%u max=%u nonzero=%zu/%zu first_px=%02x%02x%02x%02x last_px=%02x%02x%02x%02x", w, h, d, lo, hi, nonzero, rowb * h * d,
		              bytes[0], bytes[1], bytes[2], bytes[3], bytes[rowb * h * d - 4], bytes[rowb * h * d - 3], bytes[rowb * h * d - 2], bytes[rowb * h * d - 1]);
		return std::string(t) + " corners(x,y,z=0|max; RGB10 as r,g,b):" + corners;
	}
	if (tex.pixelFormat == MTLPixelFormatBC4_RUnorm && w <= 512 && h <= 512 && tex.storageMode == MTLStorageModeShared && tex.textureType == MTLTextureType2D) {
		// Small BC4 masks (the body hide masks and their cutout sources): mean of the first level, same decode as LogSmallBc4.
		const NSUInteger bw = (w + 3) / 4, bh = (h + 3) / 4;
		std::vector<uint8_t> blocks(bw * bh * 8);
		[tex getBytes:blocks.data() bytesPerRow:bw * 8 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
		uint64_t sum = 0, texels = 0;
		for (size_t b = 0; b < bw * bh; ++b) {
			const uint8_t* blk = blocks.data() + b * 8;
			const int a0 = blk[0], a1 = blk[1];
			int pal[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
			if (a0 > a1) { for (int k = 1; k < 7; ++k) pal[1 + k] = ((7 - k) * a0 + k * a1) / 7; }
			else { for (int k = 1; k < 5; ++k) pal[1 + k] = ((5 - k) * a0 + k * a1) / 5; pal[6] = 0; pal[7] = 255; }
			uint64_t bits = 0;
			for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(blk[2 + i]) << (8 * i);
			for (int px = 0; px < 16; ++px) { sum += static_cast<uint64_t>(pal[(bits >> (3 * px)) & 7]); ++texels; }
		}
		char t[64];
		std::snprintf(t, sizeof(t), "bc4 mean=%.1f", texels ? static_cast<double>(sum) / texels : 0.0);
		return t;
	}
	if (bpp == 0 || bpp > 16 || w * h > (4u << 20)) return "n/a";
	if (!is_float) {
		// 8-bit style formats: byte range only (tells an all-black or all-white input apart).
		if (bpp > 4) return "n/a";
		id<MTLDevice>       dev  = MetalContext::Instance().GetDevice();
		id<MTLCommandQueue> que  = MetalContext::Instance().GetCommandQueue();
		const size_t        rowb = w * bpp;
		id<MTLBuffer>       b    = KytyNewBuffer(dev, rowb * h, MTLResourceStorageModeShared, __LINE__);
		id<MTLCommandBuffer> c   = [que commandBuffer];
		id<MTLBlitCommandEncoder> bl = [c blitCommandEncoder];
		if (b == nil || c == nil || bl == nil) return "n/a";
		[bl copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
		           toBuffer:b destinationOffset:0 destinationBytesPerRow:rowb destinationBytesPerImage:rowb * h];
		[bl endEncoding];
		KytyWatchCommandBuffer(c, "metalRenderer.mm:c#2");
		KytyBudgetedCommit(c);
		[c waitUntilCompleted];
		const uint8_t* bytes = static_cast<const uint8_t*>(b.contents);
		uint32_t       lo = 255, hi = 0;
		uint64_t       first_channel_sum = 0;
		for (size_t i = 0; i < rowb * h; ++i) {
			lo = std::min<uint32_t>(lo, bytes[i]); hi = std::max<uint32_t>(hi, bytes[i]);
			if (i % bpp == 0) first_channel_sum += bytes[i];
		}
		char mean_text[40];
		std::snprintf(mean_text, sizeof(mean_text), " mean0=%.2f", static_cast<double>(first_channel_sum) / static_cast<double>(w * h));
		return "bytes min=" + std::to_string(lo) + " max=" + std::to_string(hi) + mean_text + PassTraceProbe(pf, bytes, w, h, rowb, bpp);
	}
	id<MTLDevice>       device = MetalContext::Instance().GetDevice();
	id<MTLCommandQueue> queue  = MetalContext::Instance().GetCommandQueue();
	const size_t        row    = w * bpp;
	id<MTLBuffer>       buf    = KytyNewBuffer(device, row * h, MTLResourceStorageModeShared, __LINE__);
	id<MTLCommandBuffer> cb    = [queue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	if (buf == nil || cb == nil || blit == nil) return "n/a";
	[blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
	             toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
	[blit endEncoding];
	KytyWatchCommandBuffer(cb, "metalRenderer.mm:cb#3");
	KytyBudgetedCommit(cb);
	[cb waitUntilCompleted];
	const uint8_t* src = static_cast<const uint8_t*>(buf.contents);
	double         vmin = 1e30, vmax = -1e30;
	size_t         nan_count = 0, inf_count = 0, first_nan = ~size_t(0);
	double         ch_min[3] = {1e30, 1e30, 1e30}, ch_max[3] = {-1e30, -1e30, -1e30};
	size_t         ch_nan[3] = {0, 0, 0};
	size_t         nan_x0 = ~size_t(0), nan_x1 = 0, nan_y0 = ~size_t(0), nan_y1 = 0, nan_row0 = 0, nan_top8 = 0;
	std::vector<uint8_t> nan_mask;
	const bool     want_nan_map = KYTY_ENV("KYTY_PASS_TRACE_NANMAP") != nullptr;
	if (want_nan_map) nan_mask.assign(w * h, 0);
	const auto     stat = [&](float v, size_t at) {
		if (std::isnan(v)) {
			if (nan_count++ == 0) first_nan = at;
			const size_t nx = at % w, ny = at / w;
			nan_x0 = std::min(nan_x0, nx); nan_x1 = std::max(nan_x1, nx);
			nan_y0 = std::min(nan_y0, ny); nan_y1 = std::max(nan_y1, ny);
			nan_row0 += ny == 0; nan_top8 += ny < 8;
			if (want_nan_map) nan_mask[at] = 255;
			return;
		}
		if (std::isinf(v)) { ++inf_count; return; }
		vmin = std::min<double>(vmin, v);
		vmax = std::max<double>(vmax, v);
	};
	for (size_t y = 0; y < h; ++y) {
		for (size_t x = 0; x < w; ++x) {
			const uint8_t* px = src + y * row + x * bpp;
			const size_t   at = y * w + x;
			if (pf == MTLPixelFormatRGBA16Float) {
				for (int k = 0; k < 4; ++k) { uint16_t hv; std::memcpy(&hv, px + 2 * k, 2); stat(DumpHalfToFloat(hv), at); }
			} else if (pf == MTLPixelFormatRG16Float) {
				for (int k = 0; k < 2; ++k) { uint16_t hv; std::memcpy(&hv, px + 2 * k, 2); stat(DumpHalfToFloat(hv), at); }
			} else if (pf == MTLPixelFormatR16Float) {
				uint16_t hv; std::memcpy(&hv, px, 2); stat(DumpHalfToFloat(hv), at);
			} else if (pf == MTLPixelFormatRG11B10Float) {
				uint32_t v;
				std::memcpy(&v, px, 4);
				const float c3[3] = {DumpSmallFloat(v & 0x7ffu, 6), DumpSmallFloat((v >> 11) & 0x7ffu, 6), DumpSmallFloat((v >> 22) & 0x3ffu, 5)};
				for (int k = 0; k < 3; ++k) {
					stat(c3[k], at);
					if (std::isnan(c3[k])) { ++ch_nan[k]; } else if (!std::isinf(c3[k])) { ch_min[k] = std::min<double>(ch_min[k], c3[k]); ch_max[k] = std::max<double>(ch_max[k], c3[k]); }
				}
			} else {
				for (size_t k = 0; k < bpp / 4; ++k) { float f; std::memcpy(&f, px + 4 * k, 4); stat(f, at); }
			}
		}
	}
	// KYTY_PASS_TRACE_PAIRS=1: the most common (red, green) pairs of an RGBA16F image - the output of a KYTY_PS_PROBE=nan shader.
	std::string pairs;
	static const bool s_pairs = KYTY_ENV("KYTY_PASS_TRACE_PAIRS") != nullptr;
	if (s_pairs && pf == MTLPixelFormatRG11B10Float) {
		// The same for an RG11B10 target: the most common (red, green, blue) triples (NaN id parts of a KYTY_PS_PROBE=nan shader).
		std::map<std::array<float, 3>, size_t> tally;
		for (size_t i = 0; i < w * h; ++i) {
			uint32_t v;
			std::memcpy(&v, src + i * 4, 4);
			++tally[{DumpSmallFloat(v & 0x7ffu, 6), DumpSmallFloat((v >> 11) & 0x7ffu, 6), DumpSmallFloat((v >> 22) & 0x3ffu, 5)}];
		}
		std::vector<std::pair<size_t, std::array<float, 3>>> order;
		for (const auto& [k, v]: tally) order.push_back({v, k});
		std::sort(order.begin(), order.end(), [](const auto& l, const auto& r) { return l.first > r.first; });
		pairs = " triples:";
		for (size_t i = 0; i < order.size() && i < 6; ++i) {
			char t[80];
			std::snprintf(t, sizeof(t), " (%g,%g,%g)x%zu", order[i].second[0], order[i].second[1], order[i].second[2], order[i].first);
			pairs += t;
		}
	}
	if (s_pairs && pf == MTLPixelFormatRGBA16Float) {
		std::map<std::pair<float, float>, size_t> tally;
		for (size_t i = 0; i < w * h; ++i) {
			uint16_t a, b;
			std::memcpy(&a, src + i * 8, 2);
			std::memcpy(&b, src + i * 8 + 2, 2);
			++tally[{DumpHalfToFloat(a), DumpHalfToFloat(b)}];
		}
		std::vector<std::pair<size_t, std::pair<float, float>>> order;
		for (const auto& [k, v]: tally) order.push_back({v, k});
		std::sort(order.begin(), order.end(), [](const auto& l, const auto& r) { return l.first > r.first; });
		pairs = " pairs:";
		for (size_t i = 0; i < order.size() && i < 6; ++i) {
			char t[64];
			std::snprintf(t, sizeof(t), " (%g,%g)x%zu", order[i].second.first, order[i].second.second, order[i].first);
			pairs += t;
		}
	}
	char text[320];
	std::snprintf(text, sizeof(text), "nan=%zu inf=%zu min=%g max=%g%s", nan_count, inf_count, vmin, vmax,
	              nan_count != 0 ? (" first_nan=(" + std::to_string(first_nan % w) + "," + std::to_string(first_nan / w) + ")" +
	                                " nan_bbox=(" + std::to_string(nan_x0) + "," + std::to_string(nan_y0) + ")-(" + std::to_string(nan_x1) + "," + std::to_string(nan_y1) + ")" +
	                                " nan_row0=" + std::to_string(nan_row0) + " nan_top8=" + std::to_string(nan_top8)).c_str() : "");
	// KYTY_PASS_TRACE_NANMAP=<dir>: a PGM mask of the NaN texels (255 = NaN) of every traced float image that has some, named by flip and address.
	static std::atomic<uint32_t> s_nan_map_seq {0};
	if (want_nan_map && nan_count != 0 && s_nan_map_seq.load() < 80u) { // enough masks after 80: a traced frame has hundreds of NaN images
		char path[512];
		std::snprintf(path, sizeof(path), "%s/nan-%llu-%05u-%ux%u.pgm", KYTY_ENV("KYTY_PASS_TRACE_NANMAP"),
		              static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)), s_nan_map_seq.fetch_add(1),
		              static_cast<unsigned>(w), static_cast<unsigned>(h));
		if (FILE* f = std::fopen(path, "wb")) {
			std::fprintf(f, "P5\n%zu %zu\n255\n", w, h);
			std::fwrite(nan_mask.data(), 1, nan_mask.size(), f);
			std::fclose(f);
			std::fprintf(stderr, "[NanMap] %s nan=%zu\n", path, nan_count);
		}
	}
	std::string untouched_text;
	if (KYTY_ENV("KYTY_PASS_TRACE_SENTINEL") != nullptr && (pf == MTLPixelFormatRG11B10Float || pf == MTLPixelFormatRGBA16Float)) {
		size_t untouched = 0;
		for (size_t y = 0; y < h; ++y) {
			for (size_t x = 0; x < w; ++x) {
				const uint8_t* px = src + y * row + x * bpp;
				float a, b;
				if (pf == MTLPixelFormatRGBA16Float) {
					uint16_t ha, hb;
					std::memcpy(&ha, px, 2);
					std::memcpy(&hb, px + 2, 2);
					a = DumpHalfToFloat(ha);
					b = DumpHalfToFloat(hb);
				} else {
					uint32_t v;
					std::memcpy(&v, px, 4);
					a = DumpSmallFloat(v & 0x7ffu, 6);
					b = DumpSmallFloat((v >> 11) & 0x7ffu, 6);
				}
				untouched += a == 1024.0f && b == 0.5f;
			}
		}
		char u[48];
		std::snprintf(u, sizeof(u), " untouched=%zu", untouched);
		untouched_text = u;
	}
	// The smallest mip levels of a mipped float image (the luminance pyramid of the auto exposure ends in a 1x1 level): their values.
	std::string mips;
	if (tex.mipmapLevelCount > 1 && (pf == MTLPixelFormatRG11B10Float || pf == MTLPixelFormatRGBA16Float)) {
		for (NSUInteger lvl = tex.mipmapLevelCount - 1; lvl >= 1 && lvl + 3 >= tex.mipmapLevelCount; --lvl) {
			const size_t lw = std::max<size_t>(1, w >> lvl), lh = std::max<size_t>(1, h >> lvl), lrow = lw * bpp;
			id<MTLBuffer>        lb = [device newBufferWithLength:lrow * lh options:MTLResourceStorageModeShared];
			id<MTLCommandBuffer> lc = [queue commandBuffer];
			id<MTLBlitCommandEncoder> lbl = [lc blitCommandEncoder];
			if (lb == nil || lc == nil || lbl == nil) break;
			[lbl copyFromTexture:tex sourceSlice:0 sourceLevel:lvl sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(lw, lh, 1)
			            toBuffer:lb destinationOffset:0 destinationBytesPerRow:lrow destinationBytesPerImage:lrow * lh];
			[lbl endEncoding];
			KytyWatchCommandBuffer(lc, "metalRenderer.mm:lc#4");
			KytyBudgetedCommit(lc);
			[lc waitUntilCompleted];
			const uint8_t* lp = static_cast<const uint8_t*>(lb.contents);
			double sum[3] = {0, 0, 0};
			size_t cnt = 0;
			for (size_t i = 0; i < lw * lh; ++i) {
				float c3[3];
				if (pf == MTLPixelFormatRG11B10Float) {
					uint32_t v;
					std::memcpy(&v, lp + i * 4, 4);
					c3[0] = DumpSmallFloat(v & 0x7ffu, 6); c3[1] = DumpSmallFloat((v >> 11) & 0x7ffu, 6); c3[2] = DumpSmallFloat((v >> 22) & 0x3ffu, 5);
				} else {
					for (int k = 0; k < 3; ++k) { uint16_t hv; std::memcpy(&hv, lp + i * 8 + 2 * k, 2); c3[k] = DumpHalfToFloat(hv); }
				}
				for (int k = 0; k < 3; ++k) sum[k] += std::isfinite(c3[k]) ? c3[k] : 0.0;
				++cnt;
			}
			char mt[120];
			std::snprintf(mt, sizeof(mt), " mip%lu(%zux%zu)avg=(%g,%g,%g)", static_cast<unsigned long>(lvl), lw, lh, sum[0] / cnt, sum[1] / cnt, sum[2] / cnt);
			mips += mt;
		}
	}
	// KYTY_PASS_TRACE_SAT=<v>: float colour images also report how many texels have a colour channel >= v, their bounding box, a 4x4
	// grid of their counts and the first one's value (is a saturated region the sun, a light, or a block of garbage?).
	std::string sat;
	static const float s_sat = KYTY_ENV("KYTY_PASS_TRACE_SAT") != nullptr ? std::strtof(KYTY_ENV("KYTY_PASS_TRACE_SAT"), nullptr) : 0.0f;
	if (s_sat > 0.0f && (pf == MTLPixelFormatRG11B10Float || pf == MTLPixelFormatRGBA16Float)) {
		size_t n = 0, x0 = w, y0 = h, x1 = 0, y1 = 0, grid[16] = {};
		float  first[3] = {0, 0, 0};
		for (size_t y = 0; y < h; ++y) {
			for (size_t x = 0; x < w; ++x) {
				const uint8_t* px = src + y * row + x * bpp;
				float c3[3];
				if (pf == MTLPixelFormatRG11B10Float) {
					uint32_t v;
					std::memcpy(&v, px, 4);
					c3[0] = DumpSmallFloat(v & 0x7ffu, 6); c3[1] = DumpSmallFloat((v >> 11) & 0x7ffu, 6); c3[2] = DumpSmallFloat((v >> 22) & 0x3ffu, 5);
				} else {
					for (int k = 0; k < 3; ++k) { uint16_t hv; std::memcpy(&hv, px + 2 * k, 2); c3[k] = DumpHalfToFloat(hv); }
				}
				if (!(c3[0] >= s_sat || c3[1] >= s_sat || c3[2] >= s_sat)) continue;
				if (n++ == 0) std::memcpy(first, c3, sizeof(first));
				x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
				++grid[(y * 4 / h) * 4 + x * 4 / w];
			}
		}
		if (n != 0) {
			char t[200];
			std::snprintf(t, sizeof(t), " sat=%zu box=(%zu,%zu)-(%zu,%zu) first=%g,%g,%g grid=", n, x0, y0, x1, y1, first[0], first[1], first[2]);
			sat = t;
			for (int i = 0; i < 16; ++i) sat += std::to_string(grid[i]) + (i == 15 ? "" : i % 4 == 3 ? "/" : ",");
		}
	}
	std::string channels;
	if (pf == MTLPixelFormatRG11B10Float && KYTY_ENV("KYTY_PASS_TRACE_CHANNELS") != nullptr) {
		char t[200];
		std::snprintf(t, sizeof(t), " ch0=[%g,%g]n%zu ch1=[%g,%g]n%zu ch2=[%g,%g]n%zu", ch_min[0], ch_max[0], ch_nan[0], ch_min[1], ch_max[1], ch_nan[1], ch_min[2], ch_max[2], ch_nan[2]);
		channels = t;
	}
	return std::string(text) + untouched_text + pairs + sat + channels + PassTraceProbe(pf, src, w, h, row, bpp);
}

// After a dispatch: finish the GPU work so far, then report the images the dispatch bound.
// Fixed-function state of the draw being recorded (pass trace only): depth/stencil test, blend, target mask, viewport and scissors.
static std::string s_trace_state;
static uint64_t    s_trace_indirect_addr = 0, s_trace_instances = 0; // draw arguments written by the GPU (DrawIndirect), and the CPU-side instance count otherwise
static uint64_t    s_trace_count_addr = 0;                           // GPU-written draw count of a multi-draw (0: single indirect draw)
static uint32_t    s_trace_max_count = 0, s_trace_stride = 0;
// Index data and vertex-stage constants of the draw being recorded (written to files by KYTY_PASS_TRACE_DUMP with KYTY_PASS_TRACE_INPUTS).
static const void*           s_trace_index_addr = nullptr;
static size_t                s_trace_index_bytes = 0;
static uint32_t              s_trace_index_type = 0;
static int32_t               s_trace_base_vertex = 0;
static uint32_t              s_trace_first_instance = 0;
static std::vector<uint32_t> s_trace_vs_srt;
static std::vector<uint32_t> s_trace_vs_push;
// Optional provenance for a vertex stream, including subranges of a compute output.
// This records writable bindings in dispatch order; a masked store may leave bytes untouched.
struct TraceBufferWriter {
	uint64_t addr, bytes, hash, guest_hash, flip;
	uint32_t index;
};
static std::vector<TraceBufferWriter> s_trace_buffer_writers;
static bool TraceBufferWritersEnabled() {
	static const bool enabled = KYTY_ENV("KYTY_TRACE_BUFFER_WRITERS") != nullptr;
	return enabled;
}
static void TraceNoteBufferWriters(const ShaderComputeInputInfo& info, uint64_t hash) {
	if (!TraceBufferWritersEnabled() || !info.stage.program || !info.stage.resources) return;
	const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	static uint64_t previous_flip = UINT64_MAX;
	// KYTY_TRACE_BUFFER_WRITERS_WINDOW=<flips>: how long a writer stays listed (default 8; body index lists are written long before they are drawn).
	static const uint64_t window = KYTY_ENV("KYTY_TRACE_BUFFER_WRITERS_WINDOW") ? static_cast<uint64_t>(std::atoll(KYTY_ENV("KYTY_TRACE_BUFFER_WRITERS_WINDOW"))) : 8u;
	if (flip != previous_flip) {
		std::erase_if(s_trace_buffer_writers, [flip](const auto& w) { return w.flip + window < flip; });
		previous_flip = flip;
	}
	const auto& program = *info.stage.program;
	const auto& buffers = info.stage.resources->buffers;
	for (size_t b = 0; b < buffers.size() && b < program.info.buffers.size(); ++b) {
		if (!program.info.buffers[b].written) continue;
		const auto& d = buffers[b].dwords;
		const uint64_t addr = (static_cast<uint64_t>(d[1] & 0xffffu) << 32) | d[0];
		const uint32_t bytes = VsharpBytes(d);
		if (addr != 0 && bytes != 0)
			s_trace_buffer_writers.push_back({addr, bytes, hash, program.guest_hash, flip, static_cast<uint32_t>(b)});
	}
}
static void TracePrintBufferWriter(uint32_t index, uint64_t addr) {
	if (!TraceBufferWritersEnabled()) return;
	for (auto it = s_trace_buffer_writers.rbegin(); it != s_trace_buffer_writers.rend(); ++it) {
		if (addr < it->addr || addr - it->addr >= it->bytes) continue;
		std::fprintf(stderr, "[PassTrace]   VBUF%u range_writer cs=0x%016llx guest=0x%016llx b%u addr=0x%llx bytes=%llu offset=%llu flip=%llu\n",
		             index, (unsigned long long)it->hash, (unsigned long long)it->guest_hash, it->index,
		             (unsigned long long)it->addr, (unsigned long long)it->bytes, (unsigned long long)(addr - it->addr),
		             (unsigned long long)it->flip);
		return;
	}
}
static void PassTraceNoteDrawState(const HW::Context& context, uint32_t prim, uint32_t count) {
	if (!PassTraceActive()) return;
	const auto& dc = context.GetDepthControl();
	const auto& sm = context.GetStencilMask();
	const auto& sv = context.GetScreenViewport();
	const auto& vp = sv.viewports[0];
	const auto& bc = context.GetBlendControl(0);
	const auto& mc = context.GetModeControl();
	char t[520];
	std::snprintf(t, sizeof(t), " | ST cbm=%u rop=%x shm=%x cull=%d%d face=%d prim=%u cnt=%u z=%d/%d/%u st=%d/%u/%u ref=%u msk=%u blend=%d(%u,%u,%u) tmask=%x vp=(%g,%g,%g,%g) vps=%d,%d,%d,%d gen=%d,%d,%d,%d scr=%d,%d,%d,%d",
	              static_cast<unsigned>(context.GetColorControl().mode), static_cast<unsigned>(context.GetColorControl().op),
	              static_cast<unsigned>(context.GetShaderRegisters().m_cbShaderMask),
	              mc.cull_front ? 1 : 0, mc.cull_back ? 1 : 0, mc.face ? 1 : 0, prim, count, dc.z_enable ? 1 : 0, dc.z_write_enable ? 1 : 0, static_cast<unsigned>(dc.zfunc), dc.stencil_enable ? 1 : 0, static_cast<unsigned>(dc.stencilfunc),
	              static_cast<unsigned>(dc.stencilfunc_bf), static_cast<unsigned>(sm.stencil_testval), static_cast<unsigned>(sm.stencil_mask), bc.enable ? 1 : 0,
	              static_cast<unsigned>(bc.color_srcblend), static_cast<unsigned>(bc.color_destblend), static_cast<unsigned>(bc.color_comb_fcn), context.GetRenderTargetMask(),
	              vp.xscale, vp.xoffset, vp.yscale, vp.yoffset, vp.viewport_scissor_left, vp.viewport_scissor_top, vp.viewport_scissor_right, vp.viewport_scissor_bottom,
	              sv.generic_scissor_left, sv.generic_scissor_top, sv.generic_scissor_right, sv.generic_scissor_bottom, sv.screen_scissor_left, sv.screen_scissor_top,
	              sv.screen_scissor_right, sv.screen_scissor_bottom);
	s_trace_state = t;
	// Depth state the Metal path does not apply (depth bounds, read/write split, override) next to what it does (bias, stencil ops).
	const auto& po  = context.GetPolyOffset();
	const auto& sc  = context.GetStencilControl();
	const auto& drt = context.GetDepthRenderTarget();
	const auto& ov  = context.GetDepthRenderOverride();
	char x[400];
	std::snprintf(x, sizeof(x), " dbe=%d bounds=(%g,%g) poly=%d%d f=(%g,%g) b=(%g,%g) clamp=%g fl=%d stop=%u/%u/%u wm=%x opv=%u zr=0x%llx zw=0x%llx sr=0x%llx sw=0x%llx ovr=%d%d%d%d",
	              dc.depth_bounds_enable ? 1 : 0, context.GetDepthBoundsMin(), context.GetDepthBoundsMax(), mc.poly_offset_front_enable ? 1 : 0,
	              mc.poly_offset_back_enable ? 1 : 0, po.front_scale, po.front_offset, po.back_scale, po.back_offset, po.clamp, po.db_is_float_fmt ? 1 : 0,
	              static_cast<unsigned>(sc.stencil_fail), static_cast<unsigned>(sc.stencil_zpass), static_cast<unsigned>(sc.stencil_zfail),
	              static_cast<unsigned>(sm.stencil_writemask), static_cast<unsigned>(sm.stencil_opval), (unsigned long long)drt.z_read_base_addr,
	              (unsigned long long)drt.z_write_base_addr, (unsigned long long)drt.stencil_read_base_addr, (unsigned long long)drt.stencil_write_base_addr,
	              ov.force_z_valid ? 1 : 0, ov.force_z_dirty ? 1 : 0, ov.force_stencil_valid ? 1 : 0, ov.force_stencil_dirty ? 1 : 0);
	s_trace_state += x;
}

// Submission of the draw / dispatch being recorded (the guest command processor's submit id): tells the graphics queue from the async compute queues in a trace.
static uint64_t s_trace_submit_id = 0;
static uint64_t s_trace_gcs_hash = 0; // guest hash of the traced kernel (the Vulkan reference trace prints guest hashes)
static uint64_t s_trace_cs_args_addr = 0; // guest address of the indirect arguments of the compute dispatch being traced (0: direct dispatch)
static void PassTraceReport(uint64_t cs_hash, uint32_t gx, uint32_t gy, uint32_t gz, uint32_t lx, uint32_t ly, uint32_t lz,
                            id<MTLBuffer> indirect_args = nil) {
	if (!(PassTraceActive() || PassTraceCsFilter()) || (s_pass_items.empty() && !indirect_args)) { s_pass_items.clear(); return; }
	static uint64_t s_n = 0;
	// Split-op may have left no current buffer. An explicit queue fence must
	// exist before commit-and-wait; WaitUntilIdle alone is a no-op in that case.
	(void)MetalContext::Instance().GetCurrentCommandBuffer();
	MetalContext::Instance().CommitCommandBuffer(true);
	std::string line;
	if (indirect_args) {
		// Split-op diagnostics may already have committed without waiting.
		// CPU reads here are observational only and follow a queue completion.
		const auto* args = static_cast<const uint32_t*>(indirect_args.contents);
		gx = args[0]; gy = args[1]; gz = args[2];
		char raw[128];
		std::snprintf(raw, sizeof(raw), " | INDIRECT raw=%ux%ux%u", args[4], args[5], args[6]);
		line += raw;
		if (s_trace_cs_args_addr != 0) {
			// Where the guest keeps these arguments and what that memory holds now (the GPU has finished: the report committed and waited).
			uint32_t now[3] = {0, 0, 0};
			const bool read_ok = Libs::LibKernel::Memory::TryReadBacking(s_trace_cs_args_addr, now, sizeof(now));
			char where[160];
			std::snprintf(where, sizeof(where), " args@0x%llx now=%s%u,%u,%u", static_cast<unsigned long long>(s_trace_cs_args_addr), read_ok ? "" : "(unreadable)", now[0], now[1], now[2]);
			line += where;
		}
	}
	static const char* s_dump_dir = KYTY_ENV("KYTY_PASS_TRACE_DUMP");
	for (const auto& it: s_pass_items) {
		char head[160];
		std::snprintf(head, sizeof(head), " | %s r%u 0x%llx %lux%lu f%d via=%s @%p ", it.store ? "OUT" : "IN", it.res, static_cast<unsigned long long>(it.addr),
		              static_cast<unsigned long>(it.tex.width), static_cast<unsigned long>(it.tex.height), static_cast<int>(it.tex.pixelFormat), it.via,
		              (__bridge void*)it.tex);
		line += head + PassTraceStats(it.tex);
		std::vector<uint8_t> volume;
		if (s_dump_dir != nullptr && s_pass_force && ReadbackVolume(it.tex, volume)) {
			char path[512];
			std::snprintf(path, sizeof(path), "%s/cs%016llx_%llu_%s_r%u_%llx_%lux%lux%lu_f%d.raw", s_dump_dir, static_cast<unsigned long long>(cs_hash),
			              static_cast<unsigned long long>(s_n), it.store ? "OUT" : "IN", it.res, static_cast<unsigned long long>(it.addr),
			              static_cast<unsigned long>(it.tex.width), static_cast<unsigned long>(it.tex.height), static_cast<unsigned long>(it.tex.depth),
			              static_cast<int>(it.tex.pixelFormat));
			if (FILE* f = std::fopen(path, "wb")) {
				std::fwrite(volume.data(), 1, volume.size(), f);
				std::fclose(f);
			}
		}
	}
	s_pass_items.clear();
	std::fprintf(stderr, "[PassTrace] n=%llu cs=0x%016llx gcs=0x%016llx sub=%llu grp=%ux%ux%u loc=%ux%ux%u%s\n", static_cast<unsigned long long>(s_n++),
	             static_cast<unsigned long long>(cs_hash), static_cast<unsigned long long>(s_trace_gcs_hash), static_cast<unsigned long long>(s_trace_submit_id), gx, gy, gz, lx, ly, lz, line.c_str());
}

static uint64_t s_trace_vs_hash = 0, s_trace_ps_hash = 0, s_trace_gps_hash = 0;
struct TraceInput {
	id<MTLTexture> tex;
	uint32_t       slot;
	uint64_t       addr; // guest address of the T# (0 when unknown)
	uint32_t       w, h, tfmt, tile; // what the T# declared
	uint32_t       levels = 0;       // mip levels the T# declared (0: unknown); the IN line prints it next to the bound texture's count
};
static std::vector<TraceInput> s_trace_draw_inputs; // textures of the current draw's pixel stage
struct TraceBuf {
	uint32_t       index;
	id<MTLBuffer>  buf;
	NSUInteger     offset;
	uint32_t       size; // bytes the shader may read (its sizes[] entry)
	uint64_t       addr; // guest address
};
static std::vector<TraceBuf> s_trace_draw_bufs; // buffers of the current draw's pixel stage
static std::vector<TraceBuf> s_trace_vs_bufs;   // buffers of the current draw's vertex stage
static std::vector<TraceBuf> s_position_watch_bufs;
// KYTY_POS_NAN_FULL=<VS hash>[,<VS hash>...] (full hash or its upper 32 bits): after a draw of those vertex shaders, copy each whole
// float3 vertex buffer in GPU order and count NaN / Inf / huge words in a completed handler ([PosNan]). At most once per 30 flips
// per shader and buffer, from flip KYTY_POS_NAN_FROM (default 1500).
extern std::atomic<uint64_t> s_draw_seq;
static const std::vector<uint64_t>& PosNanFullHashes() {
	static const std::vector<uint64_t> list = [] {
		std::vector<uint64_t> v;
		if (const char* s = KYTY_ENV("KYTY_POS_NAN_FULL"); s != nullptr) {
			for (const char* p = s; *p != '\0';) {
				char* end = nullptr;
				const uint64_t h = std::strtoull(p, &end, 16);
				if (end == p) break;
				v.push_back(h);
				p = *end == ',' ? end + 1 : end;
			}
		}
		return v;
	}();
	return list;
}
static bool PosNanFullWanted(uint64_t vs) {
	for (uint64_t h: PosNanFullHashes())
		if (h == vs || h == (vs >> 32)) return true;
	return false;
}
static bool PositionWatchEnabled() {
	static const bool enabled = KYTY_ENV("KYTY_POSITION_WATCH") != nullptr || !PosNanFullHashes().empty();
	return enabled;
}
static void MeterPositionBuffersFull() {
	const uint64_t vs = s_trace_vs_hash;
	if (!PosNanFullWanted(vs)) return;
	const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	static const uint64_t from = KYTY_ENV("KYTY_POS_NAN_FROM") ? std::strtoull(KYTY_ENV("KYTY_POS_NAN_FROM"), nullptr, 0) : 1500ull;
	if (flip < from) return;
	static std::map<std::pair<uint64_t, uint32_t>, uint64_t> last;
	std::set<uint32_t> seen;
	auto& metal = MetalContext::Instance();
	for (auto it = s_position_watch_bufs.rbegin(); it != s_position_watch_bufs.rend(); ++it) {
		const auto b = *it;
		if (!seen.insert(b.index).second || b.buf == nil || b.offset >= b.buf.length || b.size < 12) continue;
		const auto key = std::make_pair(vs, b.index);
		const auto found = last.find(key);
		if (found != last.end() && flip < found->second + 30) continue;
		last[key] = flip;
		const NSUInteger bytes = std::min<NSUInteger>(std::min<NSUInteger>(b.size, b.buf.length - b.offset), 32u << 20) & ~NSUInteger(3);
		if (bytes == 0) continue;
		id<MTLBuffer> copy = KytyNewBuffer(metal.GetDevice(), bytes, MTLResourceStorageModeShared, __LINE__);
		if (copy == nil) continue;
		if (metal.GetActiveRenderEncoder() != nil) MetalDrawExecutor::Instance().EndRenderPass();
		id<MTLCommandBuffer>      command = metal.GetCurrentCommandBuffer();
		id<MTLBlitCommandEncoder> blit    = [command blitCommandEncoder];
		if (blit == nil) continue;
		OrderBegin(blit);
		[blit copyFromBuffer:b.buf sourceOffset:b.offset toBuffer:copy destinationOffset:0 size:bytes];
		OrderEnd(blit);
		[blit endEncoding];
		const uint64_t draw = s_draw_seq.load(std::memory_order_relaxed);
		const uint64_t ps   = s_trace_ps_hash;
		[command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
			if (completed.status != MTLCommandBufferStatusCompleted) return;
			const auto*  w = static_cast<const uint32_t*>(copy.contents);
			const size_t n = bytes / 4;
			size_t nan = 0, inf = 0, huge = 0, bad_vert = 0, zero = 0;
			size_t first = SIZE_MAX;
			float  max_abs = 0.0f;
			for (size_t i = 0; i < n; ++i) {
				const uint32_t x = w[i];
				const bool is_nan = (x & 0x7f800000u) == 0x7f800000u && (x & 0x7fffffu) != 0u;
				const bool is_inf = (x & 0x7fffffffu) == 0x7f800000u;
				float f;
				std::memcpy(&f, &x, 4);
				const bool is_huge = !is_nan && !is_inf && std::fabs(f) > 1.0e5f;
				nan += is_nan;
				inf += is_inf;
				huge += is_huge;
				zero += x == 0u;
				if ((is_nan || is_inf || is_huge) && first == SIZE_MAX) first = i;
				if (!is_nan && !is_inf) max_abs = std::max(max_abs, std::fabs(f));
			}
			for (size_t v = 0; v + 3 <= n; v += 3) {
				bool bad = false;
				for (size_t c = 0; c < 3; ++c) {
					const uint32_t x = w[v + c];
					bad |= (x & 0x7f800000u) == 0x7f800000u;
				}
				bad_vert += bad;
			}
			std::fprintf(stderr, "[PosNan] flip=%llu draw=%llu vs=%08llx ps=%08llx b%u addr=0x%llx bytes=%zu nan=%zu inf=%zu huge=%zu zero=%zu bad_v12=%zu/%zu first=%lld max=%g\n",
			             (unsigned long long)flip, (unsigned long long)draw, (unsigned long long)(vs >> 32), (unsigned long long)(ps >> 32), b.index,
			             (unsigned long long)b.addr, (size_t)bytes, nan, inf, huge, zero, bad_vert, n / 3, first == SIZE_MAX ? -1ll : (long long)first, max_abs);
		}];
	}
}
// Copy a suspect position in GPU order. The CPU can still see an earlier frame's NaN while its producer is queued.
static void WatchPositionBuffers() {
	if (!PosNanFullHashes().empty()) { MeterPositionBuffersFull(); return; }
	if (!PositionWatchEnabled() || MetalContext::Instance().GetActiveRenderEncoder() == nil) return;
	const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (flip < 1500) return;
	static std::map<std::pair<uint64_t, uint32_t>, uint64_t> last;
	std::set<uint32_t> seen;
	for (auto it = s_position_watch_bufs.rbegin(); it != s_position_watch_bufs.rend(); ++it) {
		const auto b = *it;
		if (!seen.insert(b.index).second || b.buf == nil || b.buf.contents == nullptr || b.size < 1200 || b.offset > b.buf.length || b.buf.length - b.offset < 12) continue;
		const auto key = std::make_pair(s_trace_vs_hash, b.index);
		if (flip < last[key] + 120) continue;
		std::array<uint32_t, 3> cpu {};
		std::memcpy(cpu.data(), static_cast<const uint8_t*>(b.buf.contents) + b.offset, 12);
		if (!std::all_of(cpu.begin(), cpu.end(), [](uint32_t x) { return (x & 0x7f800000u) == 0x7f800000u && (x & 0x7fffffu) != 0; })) continue;
		last[key] = flip;
		const uint64_t vs = s_trace_vs_hash;
		std::fprintf(stderr, "[PositionWatchBegin] flip=%llu vs=0x%016llx b%u addr=0x%llx size=%u cpu=%08x,%08x,%08x\n",
		             (unsigned long long)flip, (unsigned long long)vs, b.index, (unsigned long long)b.addr, b.size, cpu[0], cpu[1], cpu[2]);
		TracePrintBufferWriter(b.index, b.addr);
		auto& metal = MetalContext::Instance();
		id<MTLBuffer> copy = KytyNewBuffer(metal.GetDevice(), 12, MTLResourceStorageModeShared, __LINE__);
		if (copy == nil) continue;
		MetalDrawExecutor::Instance().EndRenderPass();
		id<MTLCommandBuffer> command = metal.GetCurrentCommandBuffer();
		id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
		if (blit == nil) continue;
		OrderBegin(blit);
		[blit copyFromBuffer:b.buf sourceOffset:b.offset toBuffer:copy destinationOffset:0 size:12];
		OrderEnd(blit);
		[blit endEncoding];
		[command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
			if (completed.status != MTLCommandBufferStatusCompleted) return;
			const auto* w = static_cast<const uint32_t*>(copy.contents);
			std::fprintf(stderr, "[PositionWatch] flip=%llu vs=0x%016llx b%u addr=0x%llx gpu=%08x,%08x,%08x\n",
			             (unsigned long long)flip, (unsigned long long)vs, b.index, (unsigned long long)b.addr, w[0], w[1], w[2]);
		}];
	}
}

// KYTY_PASS_TRACE_TRIGGER also runs every draw of the traced frame as a render pass of its own and reports the float render targets
// it wrote ([PassTrace] draw ...).
// KYTY_MEM_LOG: Metal allocation growth attributed to the draw / dispatch that was being recorded (device.currentAllocatedSize before
// and after; the positive part is summed per shader pair). The top entries are printed in the memory log line.
static std::map<std::tuple<int, uint64_t, uint64_t>, uint64_t> s_alloc_by_site;
static std::mutex s_alloc_mutex;
struct AllocProbe {
	int      kind;
	uint64_t before = 0;
	explicit AllocProbe(int k): kind(k) {
		static const bool s_on = KYTY_ENV("KYTY_MEM_LOG") != nullptr;
		if (s_on) before = [MetalContext::Instance().GetDevice() currentAllocatedSize];
	}
	~AllocProbe() {
		if (before == 0) return;
		const uint64_t after = [MetalContext::Instance().GetDevice() currentAllocatedSize];
		if (after <= before) return;
		std::lock_guard<std::mutex> lock(s_alloc_mutex);
		s_alloc_by_site[{kind, kind == 2 ? s_alloc_cs_hash : s_trace_vs_hash, kind == 2 ? 0 : s_trace_ps_hash}] += after - before;
	}
};

// Polygon offset and the blend constant, as the Vulkan path sets them per draw (renderDraw.cpp): the Metal rasterizer state only
// carried the enable bit (constant 0, slope 0) and the blend colour was never set (CONSTANT_COLOR factors blended with zero).
static void ApplyGuestDepthBiasAndBlendColor(const HW::Context& context, MetalRasterizerState& rasterizer,
                                             uint64_t vs_hash, uint64_t ps_hash) {
	const auto& mode = context.GetModeControl();
	const auto& poly = context.GetPolyOffset();
	const bool  use_front = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back  = mode.poly_offset_back_enable && !mode.cull_back;
	rasterizer.depth_bias_enable = (use_front || use_back);
	if (rasterizer.depth_bias_enable) {
		// Metal attachments here are Depth32Float: the guest constant is used as is (the Vulkan path's
		// ConvertPolygonOffsetConstantFactor does the same for float host formats), with the Apple factor of 2 on float guest depth.
		const float apple_scale       = poly.db_is_float_fmt ? 2.0f : 1.0f;
		rasterizer.depth_bias_constant = (use_front ? poly.front_offset : poly.back_offset) * apple_scale;
		rasterizer.depth_bias_slope    = (use_front ? poly.front_scale : poly.back_scale) / 16.0f * apple_scale;
		rasterizer.depth_bias_clamp    = poly.clamp;
	}
	{
		const auto& c = context.GetBlendColor();
		MetalDrawExecutor::Instance().SetBlendColor(c.red, c.green, c.blue, c.alpha);
	}
	static const bool s_trace_raster = KYTY_ENV("KYTY_TRACE_RASTER_STATE") != nullptr;
	if (s_trace_raster) {
		const auto& pass = MetalDrawExecutor::Instance().LastRenderPassDesc();
		const auto& vp = context.GetScreenViewport().viewports[0];
		id<MTLTexture> depth = pass.has_depth ? pass.depth_attachment.texture : nil;
		char state[768];
		std::snprintf(state, sizeof(state),
		              "depth=%lu size=%lux%lu slice=%u:%u ro=%d zwrite=%d:%d vp=%g,%g,%g,%g z=%g,%g dx=%d "
		              "cull=%d,%d face=%d bias=%d,%d float=%d negbits=%d "
		              "offset=%g,%g slope=%g,%g clamp=%g hostbias=%g,%g,%g",
		              (unsigned long)depth.pixelFormat, (unsigned long)depth.width, (unsigned long)depth.height,
		              context.GetDepthRenderTarget().depth_view.slice_start, context.GetDepthRenderTarget().depth_view.slice_max,
		              context.GetDepthRenderTarget().depth_view.depth_write_disable ? 1 : 0,
		              context.GetDepthControl().z_write_enable ? 1 : 0, GuestDepthWritesEnabled(context) ? 1 : 0,
		              vp.xoffset, vp.yoffset, vp.xscale, vp.yscale, vp.zoffset, vp.zscale,
		              context.GetClipControl().dx_clip_space ? 1 : 0,
		              mode.cull_front ? 1 : 0, mode.cull_back ? 1 : 0, mode.face ? 1 : 0,
		              use_front ? 1 : 0, use_back ? 1 : 0, poly.db_is_float_fmt ? 1 : 0,
		              static_cast<int>(poly.neg_num_db_bits), poly.front_offset, poly.back_offset,
		              poly.front_scale, poly.back_scale, poly.clamp,
		              rasterizer.depth_bias_constant, rasterizer.depth_bias_slope, rasterizer.depth_bias_clamp);
		static std::unordered_set<std::string> seen;
		if (seen.size() < 512 && seen.insert(state).second) {
			std::fprintf(stderr, "[RasterState] flip=%llu vs=%016llx ps=%016llx %s\n",
			             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)),
			             static_cast<unsigned long long>(vs_hash),
			             static_cast<unsigned long long>(ps_hash), state);
		}
	}
}

struct DrawPassTrace {
	~DrawPassTrace() {
		if (!PassTraceActive()) { WatchPositionBuffers(); return; }
		if (!PassTraceActive() || MetalContext::Instance().GetActiveRenderEncoder() == nil) return;
		const MetalRenderPassDesc desc = MetalDrawExecutor::Instance().LastRenderPassDesc();
		MetalDrawExecutor::Instance().EndRenderPass();
		MetalContext::Instance().CommitCommandBuffer(true);
		static uint64_t s_n = 0;
		std::string     line;
		if (PassTraceFrags()) {
			// Samples that passed the depth / stencil tests and were not discarded (blend-independent coverage of the draw).
			char frags[40];
			std::snprintf(frags, sizeof(frags), " | FRAGS %llu", static_cast<unsigned long long>(MetalDrawExecutor::Instance().LastPassVisibleSamples()));
			line += frags;
		}
		for (uint32_t i = 0; i < desc.color_attachment_count; ++i) {
			id<MTLTexture> tex = desc.color_attachments[i].texture;
			if (tex == nil) continue;
			uint64_t addr = 0;
			{
				std::lock_guard<std::mutex> lock(s_render_target_mutex);
				for (const auto& [a, by_format]: s_rt_by_addr) {
					for (const auto& [format, rt]: by_format) {
						if (rt.tex == tex) addr = a;
					}
				}
			}
			char head[160];
			std::snprintf(head, sizeof(head), " | RT%u 0x%llx %lux%lu f%d lvl%u @%p load=%d ", i, static_cast<unsigned long long>(addr),
			              static_cast<unsigned long>(tex.width), static_cast<unsigned long>(tex.height), static_cast<int>(tex.pixelFormat),
			              desc.color_attachments[i].level, (__bridge void*)tex, static_cast<int>(desc.color_attachments[i].load_action));
			line += head + PassTraceStats(tex);
		}
		if (desc.has_depth && desc.depth_attachment.texture != nil) {
			// The depth surface of the pass: its guest address (the compute registry knows the depth surfaces), how the pass began and what it holds after.
			id<MTLTexture> dtex = desc.depth_attachment.texture;
			uint64_t       daddr = 0;
			{
				std::lock_guard<std::mutex> lock(s_cs_storage_mutex);
				for (const auto& [a, e]: s_cs_storage)
					if (e.tex == dtex) daddr = a;
			}
			char head[160];
			std::snprintf(head, sizeof(head), " | DS 0x%llx %lux%lu f%d @%p load=%d ", static_cast<unsigned long long>(daddr), static_cast<unsigned long>(dtex.width),
			              static_cast<unsigned long>(dtex.height), static_cast<int>(dtex.pixelFormat), (__bridge void*)dtex,
			              static_cast<int>(desc.depth_attachment.load_action));
			line += head + PassTraceStats(dtex);
		}
		std::fprintf(stderr, "[PassTrace] draw n=%llu vs=0x%016llx ps=0x%016llx gps=0x%016llx sub=%llu%s\n", static_cast<unsigned long long>(s_n++),
		             static_cast<unsigned long long>(s_trace_vs_hash), static_cast<unsigned long long>(s_trace_ps_hash),
		             static_cast<unsigned long long>(s_trace_gps_hash), static_cast<unsigned long long>(s_trace_submit_id), (line + s_trace_state).c_str());
		s_trace_state.clear();
		if (s_trace_indirect_addr != 0) {
			// The draw reads its arguments on the GPU: the values the producer wrote by now (vertex count, instance count, first vertex, first instance).
			const uint32_t* a = reinterpret_cast<const uint32_t*>(s_trace_indirect_addr);
			std::fprintf(stderr, "[PassTrace]   INDIRECT draw n=%llu args@0x%llx=[%u,%u,%u,%u] count@0x%llx=%u max=%u stride=%u\n", static_cast<unsigned long long>(s_n - 1),
			             static_cast<unsigned long long>(s_trace_indirect_addr), a[0], a[1], a[2], a[3], static_cast<unsigned long long>(s_trace_count_addr),
			             s_trace_count_addr != 0 ? *reinterpret_cast<const uint32_t*>(s_trace_count_addr) : 0u, s_trace_max_count, s_trace_stride);
		} else if (s_trace_instances > 1) {
			std::fprintf(stderr, "[PassTrace]   INSTANCES draw n=%llu count=%llu\n", static_cast<unsigned long long>(s_n - 1), static_cast<unsigned long long>(s_trace_instances));
		}
		s_trace_indirect_addr = 0;
		s_trace_instances = 0;
		s_trace_count_addr = 0;
		// The first draws whose output holds NaN also report the float images their pixel stage samples.
		static uint32_t s_input_reports = 0;
		// KYTY_PASS_TRACE_INPUTS=<ps hash>[,<ps hash>...], or "all" (every draw of the traced frame), or "size:<w>x<h>" (the draws
		// whose trace line names an image of that size, e.g. size:1920x1080 for the UI passes)
		static const std::vector<uint64_t> s_inputs_ps = [] {
			std::vector<uint64_t> list;
			for (const char* p = KYTY_ENV("KYTY_PASS_TRACE_INPUTS"); p != nullptr && *p != '\0';) {
				char* end = nullptr;
				const uint64_t v = std::strtoull(p, &end, 0);
				if (end == p) break;
				list.push_back(v);
				p = *end == ',' ? end + 1 : end;
			}
			return list;
		}();
		static const std::string s_inputs_mode = KYTY_ENV("KYTY_PASS_TRACE_INPUTS") != nullptr ? KYTY_ENV("KYTY_PASS_TRACE_INPUTS") : "";
		const bool inputs_wanted = s_inputs_mode == "all" ||
		    (s_inputs_mode.starts_with("size:") && line.find(" " + s_inputs_mode.substr(5) + " ") != std::string::npos) ||
		    std::find(s_inputs_ps.begin(), s_inputs_ps.end(), s_trace_ps_hash) != s_inputs_ps.end() ||
		    std::find(s_inputs_ps.begin(), s_inputs_ps.end(), s_trace_vs_hash) != s_inputs_ps.end(); // a VS hash selects as well
		if (inputs_wanted || line.find("nan=") != std::string::npos) {
			bool has_nan = false;
			for (size_t at = line.find("nan="); at != std::string::npos; at = line.find("nan=", at + 4)) has_nan |= line[at + 4] != '0' || (line[at + 5] != ' ');
			if (inputs_wanted || (has_nan && s_input_reports < 8)) {
				++s_input_reports;
				std::string in_line;
				for (const auto& in: s_trace_draw_inputs) {
					const id<MTLTexture> tex = in.tex;
					const uint32_t       slot = in.slot;
					uint64_t addr = in.addr;
					{
						std::lock_guard<std::mutex> lock(s_render_target_mutex);
						for (const auto& [a, by_format]: s_rt_by_addr)
							for (const auto& [format, rt]: by_format)
								if (rt.tex == tex) addr = a;
					}
					if (addr == 0) {
						std::lock_guard<std::mutex> lock(s_cs_storage_mutex);
						for (const auto& [a, e]: s_cs_storage)
							if (e.tex == tex) addr = a;
					}
					char head[256];
					// lv=<bound texture's mip count>/<T# levels>: a mipped material bound as a single-level texture samples level 0 everywhere.
					std::snprintf(head, sizeof(head), "\n[PassTrace]   IN slot%u 0x%llx %lux%lu f%d @%p | T# %ux%u tfmt=%u tile=%u lv=%lu/%u | ", slot,
					              static_cast<unsigned long long>(addr), static_cast<unsigned long>(tex.width), static_cast<unsigned long>(tex.height),
					              static_cast<int>(tex.pixelFormat), (__bridge void*)tex, in.w, in.h, in.tfmt, in.tile,
					              static_cast<unsigned long>(tex.mipmapLevelCount), in.levels);
					in_line += head + PassTraceStats(tex);
					// KYTY_PASS_TRACE_MASK_DIR=<dir>: every level of the small BC4 textures a traced draw samples (the body hide masks), as raw blocks.
					static const char* s_mask_dir = KYTY_ENV("KYTY_PASS_TRACE_MASK_DIR");
					if (s_mask_dir != nullptr && tex.pixelFormat == MTLPixelFormatBC4_RUnorm && tex.width <= 256 && tex.height <= 256 && tex.textureType == MTLTextureType2D) {
						static std::set<std::pair<uint64_t, uint64_t>> s_mask_seen;
						const uint64_t mask_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
						if (s_mask_seen.emplace(addr, mask_flip).second) {
							uint8_t raw_head[64] = {};
							if (Libs::LibKernel::Memory::TryReadBacking(addr, raw_head, sizeof(raw_head))) {
								std::string hex;
								for (uint8_t v: raw_head) { char t[4]; std::snprintf(t, sizeof(t), "%02x", v); hex += t; }
								std::fprintf(stderr, "[MaskRaw] addr=0x%llx %lux%lu tile=%u guest=%s\n", static_cast<unsigned long long>(addr), static_cast<unsigned long>(tex.width),
								             static_cast<unsigned long>(tex.height), in.tile, hex.c_str());
							}
							for (NSUInteger l = 0; l < tex.mipmapLevelCount; ++l) {
								const size_t lw = std::max<size_t>(tex.width >> l, 1), lh = std::max<size_t>(tex.height >> l, 1);
								const size_t rowb = ((lw + 3) / 4) * 8, bytes = rowb * ((lh + 3) / 4);
								id<MTLBuffer> b = KytyNewBuffer(MetalContext::Instance().GetDevice(), bytes, MTLResourceStorageModeShared, __LINE__);
								id<MTLCommandBuffer> c = [MetalContext::Instance().GetCommandQueue() commandBuffer];
								id<MTLBlitCommandEncoder> bl = [c blitCommandEncoder];
								[bl copyFromTexture:tex sourceSlice:0 sourceLevel:l sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(lw, lh, 1)
								           toBuffer:b destinationOffset:0 destinationBytesPerRow:rowb destinationBytesPerImage:bytes];
								[bl endEncoding];
								KytyBudgetedCommit(c);
								[c waitUntilCompleted];
								char path[1024];
								std::snprintf(path, sizeof(path), "%s/%llx-flip%llu-L%lu-%zux%zu.bc4", s_mask_dir, static_cast<unsigned long long>(addr),
								              static_cast<unsigned long long>(mask_flip), static_cast<unsigned long>(l), lw, lh);
								if (FILE* f = std::fopen(path, "wb")) { std::fwrite(b.contents, 1, bytes, f); std::fclose(f); }
							}
						}
					}
					// KYTY_PASS_TRACE_TEX_DIR=<dir> + KYTY_PASS_TRACE_TEX_PS=<guest PS hash prefix,...>: level 0 of every BC1-BC7 texture those
					// pixel shaders sample in the traced frame, as raw blocks (<addr>-flip<n>-<w>x<h>-f<format>.bc).
					static const char* s_tex_dir = KYTY_ENV("KYTY_PASS_TRACE_TEX_DIR");
					static const char* s_tex_ps  = KYTY_ENV("KYTY_PASS_TRACE_TEX_PS");
					if (s_tex_dir != nullptr && s_tex_ps != nullptr && tex.textureType == MTLTextureType2D && tex.pixelFormat >= MTLPixelFormatBC1_RGBA &&
					    tex.pixelFormat <= MTLPixelFormatBC7_RGBAUnorm_sRGB) {
						char gps[24];
						std::snprintf(gps, sizeof(gps), "%016llx", static_cast<unsigned long long>(s_trace_gps_hash));
						static std::set<std::pair<uint64_t, uint64_t>> s_tex_seen;
						const uint64_t tex_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
						if (std::strstr(s_tex_ps, std::string(gps, 8).c_str()) != nullptr && s_tex_seen.emplace(addr, tex_flip).second) {
							const bool   b8   = tex.pixelFormat == MTLPixelFormatBC1_RGBA || tex.pixelFormat == MTLPixelFormatBC1_RGBA_sRGB ||
							                    tex.pixelFormat == MTLPixelFormatBC4_RUnorm || tex.pixelFormat == MTLPixelFormatBC4_RSnorm;
							const size_t lw = tex.width, lh = tex.height;
							const size_t rowb = ((lw + 3) / 4) * (b8 ? 8 : 16), bytes = rowb * ((lh + 3) / 4);
							id<MTLBuffer> b = KytyNewBuffer(MetalContext::Instance().GetDevice(), bytes, MTLResourceStorageModeShared, __LINE__);
							id<MTLCommandBuffer> c = [MetalContext::Instance().GetCommandQueue() commandBuffer];
							id<MTLBlitCommandEncoder> bl = [c blitCommandEncoder];
							[bl copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(lw, lh, 1)
							           toBuffer:b destinationOffset:0 destinationBytesPerRow:rowb destinationBytesPerImage:bytes];
							[bl endEncoding];
							KytyBudgetedCommit(c);
							[c waitUntilCompleted];
							char path[1024];
							std::snprintf(path, sizeof(path), "%s/%llx-flip%llu-%zux%zu-f%lu.bc", s_tex_dir, static_cast<unsigned long long>(addr),
							              static_cast<unsigned long long>(tex_flip), lw, lh, static_cast<unsigned long>(tex.pixelFormat));
							if (FILE* f = std::fopen(path, "wb")) { std::fwrite(b.contents, 1, bytes, f); std::fclose(f); }
						}
					}
					uint64_t writer_flip = 0;
					if (id<MTLTexture> writer = LastGpuWriterTexture(addr, &writer_flip)) {
						char writer_info[200];
						std::snprintf(writer_info, sizeof(writer_info), " | writer=%p %lux%lu f%lu flip=%llu", (__bridge void*)writer,
						              (unsigned long)writer.width, (unsigned long)writer.height, (unsigned long)writer.pixelFormat,
						              (unsigned long long)writer_flip);
						in_line += writer_info;
					}
				}
				{
					uint64_t flips_enq = 0, flips_done = 0, gfx_started = 0;
					const uint64_t gfx_enq = Libs::Graphics::KytyCpLag(&flips_enq, &flips_done, &gfx_started);
					std::fprintf(stderr, "[PassTrace]   cp_lag of draw n=%llu: flips queued=%llu prepared=%llu gfx queued=%llu started=%llu\n",
					             static_cast<unsigned long long>(s_n - 1), (unsigned long long)flips_enq, (unsigned long long)flips_done,
					             (unsigned long long)gfx_enq, (unsigned long long)gfx_started);
				}
				std::fprintf(stderr, "[PassTrace]   inputs of draw n=%llu:%s\n", static_cast<unsigned long long>(s_n - 1), in_line.c_str());
				if (inputs_wanted) {
					for (const auto& b: s_trace_draw_bufs) {
						if (b.buf == nil) continue;
						const uint32_t* dw = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>([b.buf contents]) + b.offset);
						const size_t    avail = b.offset < b.buf.length ? (b.buf.length - b.offset) / 4 : 0;
						std::string     text;
						for (size_t k = 0; k < std::min<size_t>(avail, 160); ++k) {
							float f;
							std::memcpy(&f, &dw[k], 4);
							char t[48];
							std::snprintf(t, sizeof(t), "%s[%zx]=%08x(%g)", (k % 8) == 0 ? "\n[PassTrace]       " : " ", k, dw[k], f);
							text += t;
						}
						std::fprintf(stderr, "[PassTrace]   BUF%u addr=0x%llx size=%u avail=%zu dwords:%s\n", b.index, (unsigned long long)b.addr, b.size, avail * 4, text.c_str());
						// KYTY_PASS_TRACE_BUF_WIN=<byte offset hex>:<dwords>: also print that window of each pixel-stage buffer large enough to hold it.
						char gps_key[24];
						std::snprintf(gps_key, sizeof(gps_key), "%016llx", static_cast<unsigned long long>(s_trace_gps_hash));
						const char* win_ps = KYTY_ENV("KYTY_PASS_TRACE_PS_SRT");
						if (const char* win = KYTY_ENV("KYTY_PASS_TRACE_BUF_WIN"); win != nullptr && (win_ps == nullptr || std::strstr(win_ps, std::string(gps_key, 8).c_str()) != nullptr)) {
							char* end = nullptr;
							const size_t off = std::strtoull(win, &end, 16) / 4;
							const size_t cnt = (end != nullptr && *end == ':') ? std::strtoull(end + 1, nullptr, 10) : 64;
							if (off < avail) {
								std::string wt;
								for (size_t k = off; k < std::min<size_t>(avail, off + cnt); ++k) {
									float f;
									std::memcpy(&f, &dw[k], 4);
									char t[48];
									std::snprintf(t, sizeof(t), "%s[%zx]=%08x(%g)", ((k - off) % 8) == 0 ? "\n[PassTrace]       " : " ", k, dw[k], f);
									wt += t;
								}
								std::fprintf(stderr, "[PassTrace]   BUF%u window @0x%zx:%s\n", b.index, off * 4, wt.c_str());
							}
						}
					}
					for (const auto& b: s_trace_vs_bufs) {
						if (b.buf == nil) continue;
						const uint32_t* dw = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>([b.buf contents]) + b.offset);
						const size_t    avail = b.offset < b.buf.length ? (b.buf.length - b.offset) / 4 : 0;
						std::string     text;
						for (size_t k = 0; k < std::min<size_t>(avail, 48); ++k) {
							float f;
							std::memcpy(&f, &dw[k], 4);
							char t[48];
							std::snprintf(t, sizeof(t), "%s[%zx]=%08x(%g)", (k % 8) == 0 ? "\n[PassTrace]       " : " ", k, dw[k], f);
							text += t;
						}
						std::fprintf(stderr, "[PassTrace]   VBUF%u addr=0x%llx size=%u avail=%zu dwords:%s\n", b.index,
						             static_cast<unsigned long long>(b.addr), b.size, avail * 4, text.c_str());
						TracePrintBufferWriter(b.index, b.addr);
					}
					// KYTY_PASS_TRACE_DUMP=<dir>: the whole vertex-stage buffers (what the shader may read), the index data, the SRT and the
					// push dwords of the draw, for offline replay of the vertex shader. Files: <dir>/draw<n>_vs<hash>_*.
					static const char* s_draw_dump_dir = KYTY_ENV("KYTY_PASS_TRACE_DUMP");
					static uint32_t    s_draw_dumps = 0; // the bone palettes are tens of MB: keep the first few draws
					// KYTY_PASS_TRACE_DUMP_MAX=<n>: how many draws to dump (default 8).
					static const uint32_t s_draw_dump_max = KYTY_ENV("KYTY_PASS_TRACE_DUMP_MAX") != nullptr ? std::atoi(KYTY_ENV("KYTY_PASS_TRACE_DUMP_MAX")) : 8;
					// KYTY_PASS_TRACE_DUMP_PER_VS=<n>: at most n dumped draws per vertex shader (with KYTY_PASS_TRACE_INPUTS=all: a sample of
					// every vertex shader of the frame, for offline replay of all of them).
					static const uint32_t s_draw_dump_per_vs = KYTY_ENV("KYTY_PASS_TRACE_DUMP_PER_VS") != nullptr ? std::atoi(KYTY_ENV("KYTY_PASS_TRACE_DUMP_PER_VS")) : 0;
					static std::unordered_map<uint64_t, uint32_t> s_draw_dumps_by_vs;
					// KYTY_PASS_TRACE_DUMP_VS=<hash>[,<hash>...]: dump only these vertex shaders.
					const auto dump_vs_listed = [&] {
						const char* p = KYTY_ENV("KYTY_PASS_TRACE_DUMP_VS");
						if (p == nullptr) return true;
						while (*p != '\0') {
							char* end = nullptr;
							const uint64_t h = std::strtoull(p, &end, 16);
							if (end == p) break;
							if (h == s_trace_vs_hash || h == (s_trace_vs_hash >> 32)) return true;
							p = *end == ',' ? end + 1 : end;
						}
						return false;
					};
					const bool vs_quota = dump_vs_listed() && (s_draw_dump_per_vs == 0 || s_draw_dumps_by_vs[s_trace_vs_hash]++ < s_draw_dump_per_vs);
					if (s_draw_dump_dir != nullptr && vs_quota && s_draw_dumps++ < s_draw_dump_max) {
						const auto write_file = [&](const char* what, const void* data, size_t bytes) {
							char path[512];
							std::snprintf(path, sizeof(path), "%s/draw%llu_vs%016llx_%s.bin", s_draw_dump_dir, static_cast<unsigned long long>(s_n - 1),
							              static_cast<unsigned long long>(s_trace_vs_hash), what);
							if (FILE* f = std::fopen(path, "wb")) {
								std::fwrite(data, 1, bytes, f);
								std::fclose(f);
							}
						};
						for (const auto& b: s_trace_vs_bufs) {
							if (b.buf == nil || b.offset >= b.buf.length) continue;
							const size_t bytes = std::min<size_t>({b.size, b.buf.length - b.offset, size_t {64} << 20});
							char what[64];
							std::snprintf(what, sizeof(what), "buf%u_%llx", b.index, static_cast<unsigned long long>(b.addr));
							write_file(what, static_cast<const uint8_t*>([b.buf contents]) + b.offset, bytes);
						}
						if (s_trace_index_addr != nullptr && s_trace_index_bytes != 0 && s_trace_index_bytes <= (size_t {64} << 20))
							write_file(s_trace_index_type == 0 ? "index16" : "index32", s_trace_index_addr, s_trace_index_bytes);
						if (!s_trace_vs_srt.empty()) write_file("srt", s_trace_vs_srt.data(), s_trace_vs_srt.size() * 4);
						if (!s_trace_vs_push.empty()) write_file("push", s_trace_vs_push.data(), s_trace_vs_push.size() * 4);
						std::fprintf(stderr, "[PassTrace]   DUMPED draw n=%llu base_vertex=%d first_instance=%u index_bytes=%zu\n", static_cast<unsigned long long>(s_n - 1),
						             s_trace_base_vertex, s_trace_first_instance, s_trace_index_bytes);
					}
				}
			}
		}
	}
};


void DumpTrackedImages(uint64_t present_index) {
	static const char* s_list    = KYTY_ENV("KYTY_DUMP_IMG");
	static const char* s_trigger = KYTY_ENV("KYTY_DUMP_IMG_TRIGGER");
	static const char* s_prefix  = KYTY_ENV("KYTY_DUMP_IMG_PREFIX");
	LayeredStats();
	if (s_list == nullptr || s_trigger == nullptr || *s_trigger == '\0' || std::remove(s_trigger) != 0) return;
	// Tokens: "all" (images of >= 200k pixels, half resolution), "small" (every smaller image, full resolution, raw values of
	// the first pixels logged: the auto-exposure reduction chain), or guest addresses.
	bool all = false, small = false;
	std::vector<uint64_t> wanted;
	for (const char* p = s_list; *p != '\0';) {
		if (std::strncmp(p, "all", 3) == 0) { all = true; p += 3; }
		else if (std::strncmp(p, "small", 5) == 0) { small = true; p += 5; }
		else {
			char*              endp = nullptr;
			const unsigned long long v = std::strtoull(p, &endp, 0);
			if (endp == p) break;
			wanted.push_back(v);
			p = endp;
		}
		while (*p == ',' || *p == ' ') ++p;
	}
	struct Item {
		uint64_t       addr;
		const char*    kind;
		id<MTLTexture> tex;
		uint64_t       note;
	};
	std::vector<Item> items;
	const auto want = [&](uint64_t addr, id<MTLTexture> t) {
		if (t == nil || t.textureType != MTLTextureType2D) return false;
		if (all && t.width * t.height >= 200000) return true;
		if (small && t.width * t.height < 200000) return true;
		return std::find(wanted.begin(), wanted.end(), addr) != wanted.end();
	};
	{
		std::lock_guard<std::mutex> lock(s_render_target_mutex);
		for (const auto& [addr, by_format]: s_rt_by_addr) {
			for (const auto& [format, rt]: by_format) {
				if (want(addr, rt.tex)) items.push_back({addr, "rt", rt.tex, rt.sequence});
			}
		}
	}
	{
		std::lock_guard<std::mutex> lock(s_cs_storage_mutex);
		for (const auto& [addr, e]: s_cs_storage) {
			if (want(addr, e.tex)) items.push_back({addr, "cs", e.tex, e.flip});
		}
	}
	// "all" would otherwise write every surface the game ever bound (thousands); keep the ones touched in the current frame.
	if (all) {
		uint64_t max_rt_note = 0, max_cs_flip = 0;
		for (const Item& it: items) {
			if (std::strcmp(it.kind, "rt") == 0) max_rt_note = std::max(max_rt_note, it.note);
			else max_cs_flip = std::max(max_cs_flip, it.note);
		}
		items.erase(std::remove_if(items.begin(), items.end(),
		                           [&](const Item& it) {
			                           if (std::find(wanted.begin(), wanted.end(), it.addr) != wanted.end()) return false;
			                           if (small && it.tex.width * it.tex.height < 200000) return false;
			                           return std::strcmp(it.kind, "rt") == 0 ? it.note + 20000 < max_rt_note : it.note + 1 < max_cs_flip;
		                           }),
		            items.end());
	}
	id<MTLDevice> device = MetalContext::Instance().GetDevice();
	id<MTLCommandQueue> queue = MetalContext::Instance().GetCommandQueue();
	if (device == nil || queue == nil) return;
	const std::string prefix = (s_prefix != nullptr && *s_prefix != '\0') ? s_prefix : "img";
	for (const Item& it: items) {
		const size_t bpp = BytesPerPixel(it.tex.pixelFormat);
		if (bpp == 0 || bpp > 8) continue;
		const size_t   w = it.tex.width, h = it.tex.height, row = w * bpp;
		id<MTLBuffer>  buf = KytyNewBuffer(device, row * h, MTLResourceStorageModeShared, __LINE__);
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		if (buf == nil || cb == nil || blit == nil) continue;
		[blit copyFromTexture:it.tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
		             toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
		[blit endEncoding];
		KytyWatchCommandBuffer(cb, "metalRenderer.mm:cb#5");
		KytyBudgetedCommit(cb);
		[cb waitUntilCompleted];
		const uint8_t*   src  = static_cast<const uint8_t*>(buf.contents);
		const size_t     step = (all && w * h >= 200000) ? 2 : 1;
		std::string      first_px;
		const size_t     ow = w / step, oh = h / step;
		const MTLPixelFormat pf = it.tex.pixelFormat;
		std::vector<uint8_t> out(ow * oh * 3);
		double  vmin = 1e30, vmax = -1e30;
		size_t  nan_count = 0, inf_count = 0;
		const auto tone = [](float x) {
			if (!(x > 0.0f)) return 0.0f;
			return std::pow(x / (1.0f + x), 1.0f / 2.2f);
		};
		const auto stat = [&](float v) {
			if (std::isnan(v)) { ++nan_count; return; }
			if (std::isinf(v)) { ++inf_count; return; }
			vmin = std::min<double>(vmin, v);
			vmax = std::max<double>(vmax, v);
		};
		const auto q8 = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
		for (size_t y = 0; y < oh; ++y) {
			for (size_t x = 0; x < ow; ++x) {
				const uint8_t* px = src + (y * step) * row + (x * step) * bpp;
				float          c[3] = {0, 0, 0};
				bool           is_float = true;
				if (pf == MTLPixelFormatRGBA16Float) {
					for (int k = 0; k < 3; ++k) { uint16_t hv; std::memcpy(&hv, px + 2 * k, 2); c[k] = DumpHalfToFloat(hv); }
				} else if (pf == MTLPixelFormatRG11B10Float) {
					uint32_t v;
					std::memcpy(&v, px, 4);
					c[0] = DumpSmallFloat(v & 0x7ffu, 6);
					c[1] = DumpSmallFloat((v >> 11) & 0x7ffu, 6);
					c[2] = DumpSmallFloat((v >> 22) & 0x3ffu, 5);
				} else if (pf == MTLPixelFormatRG16Float) {
					for (int k = 0; k < 2; ++k) { uint16_t hv; std::memcpy(&hv, px + 2 * k, 2); c[k] = DumpHalfToFloat(hv); }
				} else if (pf == MTLPixelFormatR16Float) {
					uint16_t hv; std::memcpy(&hv, px, 2); c[0] = c[1] = c[2] = DumpHalfToFloat(hv);
				} else if (pf == MTLPixelFormatR32Float) {
					std::memcpy(&c[0], px, 4); c[1] = c[2] = c[0];
				} else {
					is_float = false;
					if (pf == MTLPixelFormatRGB10A2Unorm) {
						uint32_t v;
						std::memcpy(&v, px, 4);
						c[0] = static_cast<float>(v & 0x3ffu) / 1023.0f;
						c[1] = static_cast<float>((v >> 10) & 0x3ffu) / 1023.0f;
						c[2] = static_cast<float>((v >> 20) & 0x3ffu) / 1023.0f;
					} else if (pf == MTLPixelFormatBGRA8Unorm || pf == MTLPixelFormatBGRA8Unorm_sRGB) {
						c[0] = px[2] / 255.0f; c[1] = px[1] / 255.0f; c[2] = px[0] / 255.0f;
					} else if (bpp >= 3) {
						c[0] = px[0] / 255.0f; c[1] = px[1] / 255.0f; c[2] = px[2] / 255.0f;
					} else {
						c[0] = c[1] = c[2] = px[0] / 255.0f;
					}
				}
				for (int k = 0; k < 3; ++k) {
					stat(c[k]);
					out[(y * ow + x) * 3 + k] = q8(is_float ? tone(c[k]) : c[k]);
				}
				if (w * h <= 4096 && y == 0 && x < 6) {
					char px_text[96];
					std::snprintf(px_text, sizeof(px_text), " (%g %g %g)", c[0], c[1], c[2]);
					first_px += px_text;
				}
			}
		}
		char name[1200];
		std::snprintf(name, sizeof(name), "%s-%llu-%llx-%s-%zux%zu-f%d.ppm", prefix.c_str(), static_cast<unsigned long long>(present_index),
		              static_cast<unsigned long long>(it.addr), it.kind, w, h, static_cast<int>(pf));
		if (FILE* f = std::fopen(name, "wb")) {
			std::fprintf(f, "P6\n%zu %zu\n255\n", ow, oh);
			std::fwrite(out.data(), 1, out.size(), f);
			std::fclose(f);
		}
		std::fprintf(stderr, "[DumpImg] present=%llu addr=0x%llx %s %zux%zu fmt=%d note=%llu min=%g max=%g nan=%zu inf=%zu -> %s%s%s\n",
		             static_cast<unsigned long long>(present_index), static_cast<unsigned long long>(it.addr), it.kind, w, h,
		             static_cast<int>(pf), static_cast<unsigned long long>(it.note), vmin, vmax, nan_count, inf_count, name,
		             first_px.empty() ? "" : " px:", first_px.c_str());
	}
}

std::vector<MetalRenderer::TrackedRenderTarget> MetalRenderer::GetTrackedRenderTargets() const {
	std::lock_guard<std::mutex> lock(s_render_target_mutex);
	std::vector<TrackedRenderTarget> result;
	result.reserve(s_rt_by_addr.size());
	for (const auto& [addr, by_format]: s_rt_by_addr) {
		for (const auto& [format, rt]: by_format) {
			result.push_back({addr, rt.width, rt.height, rt.format, (__bridge void*)rt.tex});
		}
	}
	return result;
}

MetalRenderer::LastRenderTarget MetalRenderer::GetLastRenderTarget() const {
	if (s_last_rt_tex != nil) {
		return {
			.addr = s_last_rt_addr.load(std::memory_order_relaxed),
			.width = s_last_rt_width.load(std::memory_order_relaxed),
			.height = s_last_rt_height.load(std::memory_order_relaxed),
			.format = s_last_rt_fmt.load(std::memory_order_relaxed),
		};
	}
	return {
		.addr = s_best_rt_addr.load(std::memory_order_relaxed),
		.width = s_best_rt_width.load(std::memory_order_relaxed),
		.height = s_best_rt_height.load(std::memory_order_relaxed),
		.format = s_best_rt_fmt.load(std::memory_order_relaxed),
	};
}

static bool IsDisplayableColorFormat(uint32_t fmt_val) {
	MTLPixelFormat fmt = static_cast<MTLPixelFormat>(fmt_val);
	return (fmt == MTLPixelFormatRG11B10Float ||
	        fmt == MTLPixelFormatRGBA8Unorm ||
	        fmt == MTLPixelFormatRGBA8Unorm_sRGB ||
	        fmt == MTLPixelFormatBGRA8Unorm ||
	        fmt == MTLPixelFormatBGRA8Unorm_sRGB ||
	        fmt == MTLPixelFormatRGB10A2Unorm ||
	        fmt == MTLPixelFormatRGBA16Float ||
	        fmt == MTLPixelFormatRGBA16Unorm);
}

namespace {
std::mutex                                     s_flip_mutex;
std::vector<std::pair<uint64_t, uint64_t>>     s_flip_ranges;
std::vector<std::pair<uint64_t, uint64_t>>     s_flip_backing; // (backing offset, bytes)
} // namespace

void RegisterFlipBuffer(uint64_t addr, uint64_t bytes) {
	if (addr == 0 || bytes == 0) return;
	std::lock_guard<std::mutex> lock(s_flip_mutex);
	for (auto& r: s_flip_ranges) {
		if (r.first == addr) {
			r.second = std::max(r.second, bytes);
			return;
		}
	}
	s_flip_ranges.emplace_back(addr, bytes);
	uint64_t backing = 0;
	const bool has_backing = Libs::LibKernel::Memory::TryGetGuestBackingOffset(addr, bytes, &backing);
	if (has_backing) {
		s_flip_backing.emplace_back(backing, bytes);
	}
	std::fprintf(stderr, "[FlipReg] addr=0x%llx bytes=%llu backing=%s0x%llx\n",
	             (unsigned long long)addr, (unsigned long long)bytes, has_backing ? "" : "NONE/",
	             (unsigned long long)backing);
}

// KYTY_TRACE_TEX_BACKING=1: for large guest textures, compare the bytes read through the
// kernel backing path with the bytes read directly at the guest vaddr (host pointer).
static void TraceTexBacking(uint64_t addr, uint64_t bytes) {
	static const bool s_on = KYTY_ENV("KYTY_TRACE_TEX_BACKING") != nullptr;
	if (!s_on || addr == 0 || bytes < (512u << 10)) return;
	static std::unordered_set<uint64_t> s_seen;
	if (s_seen.size() >= 60 || !s_seen.insert(addr).second) return;
	const uint64_t n = std::min<uint64_t>(bytes, 16ull << 20);
	std::vector<uint8_t> via_backing(n);
	const bool ok = Libs::LibKernel::Memory::TryReadBacking(addr, via_backing.data(), n);
	const auto* direct = reinterpret_cast<const uint8_t*>(addr);
	uint64_t nz_b = 0, nz_d = 0, diff = 0;
	for (uint64_t i = 0; i < n; ++i) {
		nz_b += via_backing[i] != 0;
		nz_d += direct[i] != 0;
		diff += via_backing[i] != direct[i];
	}
	std::fprintf(stderr, "[TexBacking] addr=0x%llx bytes=%llu backing_ok=%d nz_backing=%llu nz_direct=%llu diff=%llu\n",
	             (unsigned long long)addr, (unsigned long long)n, ok ? 1 : 0,
	             (unsigned long long)nz_b, (unsigned long long)nz_d, (unsigned long long)diff);
}

// KYTY_TRACE_RANGE=lo,hi,flip_lo,flip_hi: every renderer operation that touches [lo,hi) while the presented-flip counter is
// inside the window, in encode order (render passes, buffer/texture binds, compute storage writes).
static void TraceRangeOp(const char* op, uint64_t addr, uint64_t bytes) {
	// KYTY_CS_BUF_WRITERS also retains recent image writes so a selected buffer can be checked for
	// overlaps. These are encode-time candidates; the image may have separate backing storage.
	if (KYTY_ENV("KYTY_CS_BUF_WRITERS") != nullptr && addr != 0 && bytes != 0 &&
	    (std::strcmp(op, "draw_rt") == 0 || std::strcmp(op, "storage_write") == 0)) {
		std::lock_guard<std::mutex> lock(s_recent_surface_mutex);
		s_recent_surface_ops.push_back({op, addr, bytes, Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed),
		                                std::strcmp(op, "storage_write") == 0 ? s_alloc_cs_hash : 0});
		while (s_recent_surface_ops.size() > 32768) s_recent_surface_ops.pop_front();
	}
	struct Cfg {
		bool on = false;
		uint64_t lo = 0, hi = 0, f0 = 0, f1 = ~0ull;
	};
	static const Cfg cfg = [] {
		Cfg c;
		if (const char* v = KYTY_ENV("KYTY_TRACE_RANGE")) {
			char* e = nullptr;
			c.lo = std::strtoull(v, &e, 0);
			if (*e == ',') c.hi = std::strtoull(e + 1, &e, 0);
			if (*e == ',') c.f0 = std::strtoull(e + 1, &e, 0);
			if (*e == ',') c.f1 = std::strtoull(e + 1, &e, 0);
			c.on = c.hi > c.lo;
		}
		return c;
	}();
	if (!cfg.on || addr == 0 || bytes == 0 || !(addr < cfg.hi && cfg.lo < addr + bytes)) return;
	const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (flip < cfg.f0 || flip > cfg.f1) return;
	static std::atomic<uint32_t> s_seq {0};
	const uint32_t n = s_seq.fetch_add(1);
	if (n < 20000) {
		std::fprintf(stderr, "[RangeOp] #%u flip=%llu %s addr=0x%llx bytes=%llu cs=0x%016llx\n", n, static_cast<unsigned long long>(flip), op,
		             static_cast<unsigned long long>(addr), static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(s_alloc_cs_hash));
	}
}

// KYTY_LOG_STENCIL=1: periodic histogram of the depth/stencil state of draws, per render target size. The Metal pass has no stencil
// attachment, so any draw counted here with sten=1 renders without its guest stencil test.
static void NoteStencilUse(const HW::Context& context, uint64_t vs_hash, uint64_t ps_hash) {
	static const bool s_on = KYTY_ENV("KYTY_LOG_STENCIL") != nullptr;
	if (!s_on) return;
	const auto& dc = context.GetDepthControl();
	const auto& sc = context.GetStencilControl();
	const auto& sm = context.GetStencilMask();
	const auto& rt = context.GetRenderTarget(0);
	const auto& dt = context.GetDepthRenderTarget();
	static std::mutex s_mutex;
	static std::map<std::array<uint32_t, 10>, uint64_t> s_counts;
	static uint64_t s_total = 0;
	const std::array<uint32_t, 10> key = {rt.attrib2.width + 1u, rt.attrib2.height + 1u, dc.stencil_enable ? 1u : 0u, dc.stencilfunc,
	                                      sc.stencil_zpass, sc.stencil_fail, sm.stencil_testval, sm.stencil_writemask,
	                                      dt.stencil_read_base_addr != 0 ? 1u : 0u, dc.z_enable ? 1u : 0u};
	std::lock_guard<std::mutex> lock(s_mutex);
	++s_counts[key];
	// Per-shader histogram of stencil-tested draws into 1920x1080 targets (which guest draws lose their test).
	static std::map<std::array<uint64_t, 6>, uint64_t> s_by_shader;
	if (dc.stencil_enable && rt.attrib2.width + 1u == 1920u) {
		++s_by_shader[{vs_hash, ps_hash, dc.stencilfunc, sc.stencil_zpass, sm.stencil_testval, sm.stencil_writemask}];
	}
	if (s_total % 20000 == 19999) {
		for (const auto& [k, n]: s_by_shader) {
			if (n >= 200) {
				std::fprintf(stderr, "[StencilShader] vs=%016llx ps=%016llx func=%llu zpass=%llu ref=%llu wmask=0x%llx count=%llu\n",
				             (unsigned long long)k[0], (unsigned long long)k[1], (unsigned long long)k[2], (unsigned long long)k[3],
				             (unsigned long long)k[4], (unsigned long long)k[5], static_cast<unsigned long long>(n));
			}
		}
	}
	if (++s_total % 20000 == 0) {
		for (const auto& [k, n]: s_counts) {
			std::fprintf(stderr, "[StencilUse] total=%llu rt=%ux%u sten=%u func=%u zpass=%u sfail=%u ref=%u wmask=0x%x stencil_buf=%u z_en=%u count=%llu\n",
			             static_cast<unsigned long long>(s_total), k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], k[8], k[9],
			             static_cast<unsigned long long>(n));
		}
	}
}

// KYTY_LOG_DEPTH=1: histogram of the depth state every draw runs with (target size, depth enable/write/func, clear value, depth-clear
// draw, HTILE/stencil presence) so a pass whose depth never gets written can be told apart from one that merely tests.
static void NoteDepthUse(const HW::Context& context) {
	static const bool s_on = KYTY_ENV("KYTY_LOG_DEPTH") != nullptr;
	if (!s_on) return;
	const auto& dc = context.GetDepthControl();
	const auto& dt = context.GetDepthRenderTarget();
	const auto& rt = context.GetRenderTarget(0);
	static std::mutex s_mutex;
	static std::map<std::array<uint32_t, 12>, uint64_t> s_counts;
	static uint64_t s_total = 0;
	float clear = context.GetDepthClearValue();
	uint32_t clear_bits;
	std::memcpy(&clear_bits, &clear, sizeof(clear_bits));
	const std::array<uint32_t, 12> key = {rt.attrib2.width + 1u, rt.attrib2.height + 1u, dc.z_enable ? 1u : 0u, dc.z_write_enable ? 1u : 0u,
	                                      dc.zfunc, static_cast<uint32_t>(dt.z_info.format), dt.z_read_base_addr != 0 ? 1u : 0u,
	                                      dt.z_write_base_addr != 0 ? 1u : 0u, clear_bits,
	                                      context.GetRenderControl().depth_clear_enable ? 1u : 0u,
	                                      dt.htile_data_base_addr != 0 ? 1u : 0u, dt.z_info.htile_acceleration ? 1u : 0u};
	std::lock_guard<std::mutex> lock(s_mutex);
	++s_counts[key];
	if (++s_total % 5000 == 0) {
		for (const auto& [k, n]: s_counts) {
			std::fprintf(stderr, "[DepthUse] total=%llu rt=%ux%u z_en=%u z_wr=%u zfunc=%u zfmt=%u z_rd=%u z_wr_addr=%u clear=%g clear_draw=%u htile=%u/%u count=%llu\n",
			             static_cast<unsigned long long>(s_total), k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], [&] { float f; std::memcpy(&f, &k[8], 4); return f; }(),
			             k[9], k[10], k[11], static_cast<unsigned long long>(n));
		}
	}
}

// The guest depth target carries a stencil plane (DB_STENCIL_INFO + stencil base address) that draws test and write. Metal gets it as a
// separate Stencil8 attachment next to the Depth32Float depth attachment. Without it every stencil-tested draw loses its test: the UI's
static bool GuestPassHasStencil(const HW::Context& context) {
	const auto& dt = context.GetDepthRenderTarget();
	return dt.z_read_base_addr != 0 && dt.z_info.format != Prospero::DepthFormat::kInvalid &&
	       dt.stencil_read_base_addr != 0 && dt.stencil_info.format != Prospero::StencilFormat::kInvalid;
}

void TraceFlipOverlap(const char* op, uint64_t addr, uint64_t bytes) {
	TraceRangeOp(op, addr, bytes);
	static const bool s_on = KYTY_ENV("KYTY_TRACE_FLIP_WRITES") != nullptr;
	if (!s_on || addr == 0 || bytes == 0) return;
	std::lock_guard<std::mutex> lock(s_flip_mutex);
	uint64_t backing = 0;
	const bool has_backing = Libs::LibKernel::Memory::TryGetGuestBackingOffset(addr, bytes, &backing);
	for (const auto& [base, size]: s_flip_ranges) {
		if (addr < base + size && base < addr + bytes) {
			static std::atomic<uint32_t> s_hits {0};
			if (s_hits.fetch_add(1) < 400) {
				std::fprintf(stderr, "[FlipWrite] %s addr=0x%llx bytes=%llu flip=0x%llx\n", op,
				             (unsigned long long)addr, (unsigned long long)bytes,
				             (unsigned long long)base);
			}
			return;
		}
	}
	// Aliasing check: same backing (physical) range as a flip buffer under a different vaddr.
	if (has_backing) {
		for (const auto& [fb, fsize]: s_flip_backing) {
			if (backing < fb + fsize && fb < backing + bytes) {
				static std::atomic<uint32_t> s_alias_hits {0};
				if (s_alias_hits.fetch_add(1) < 400) {
					std::fprintf(stderr,
					             "[FlipAlias] %s addr=0x%llx bytes=%llu backing=0x%llx flip_backing=0x%llx\n",
					             op, (unsigned long long)addr, (unsigned long long)bytes,
					             (unsigned long long)backing, (unsigned long long)fb);
				}
				return;
			}
		}
	}
}

// KYTY_TRACE_PS_RT=<rt addr>: log the fragment hash of every draw into that RT.
static void TracePsRt(uint64_t rt_addr, uint64_t ps_hash) {
	static const uint64_t s_trace_rt = [] {
		const char* v = KYTY_ENV("KYTY_TRACE_PS_RT");
		return v != nullptr ? std::strtoull(v, nullptr, 0) : 0ull;
	}();
	if (s_trace_rt != 0 && rt_addr == s_trace_rt) {
		std::fprintf(stderr, "[PsRt] rt0=0x%llx ps=0x%016llx\n", (unsigned long long)rt_addr,
		             (unsigned long long)ps_hash);
	}
}

static void RecordDrawForRT(uint64_t addr) {
	if (addr == 0) return;
	std::lock_guard<std::mutex> lock(s_render_target_mutex);
	uint64_t count = ++s_rt_draw_counts[addr];
	if (count > s_best_rt_draws) {
		auto it = s_rt_by_addr.find(addr);
		if (it != s_rt_by_addr.end() && !it->second.empty()) {
			const TrackedRt* best_trk = nullptr;
			for (const auto& [fmt, trk] : it->second) {
				if (trk.tex != nil && trk.width >= 960 && trk.height >= 540 && IsDisplayableColorFormat(trk.format)) {
					if (!best_trk || trk.format == MTLPixelFormatRGBA8Unorm || trk.format == MTLPixelFormatRG11B10Float) {
						best_trk = &trk;
					}
				}
			}
			if (best_trk != nullptr) {
				s_best_rt_draws = count;
				s_best_rt_tex = best_trk->tex;
				s_best_rt_addr.store(addr, std::memory_order_relaxed);
				s_best_rt_width.store(best_trk->width, std::memory_order_relaxed);
				s_best_rt_height.store(best_trk->height, std::memory_order_relaxed);
				s_best_rt_fmt.store(best_trk->format, std::memory_order_relaxed);
			}
		}
	}
}

static MTLTextureUsage GuestTextureUsage(MTLPixelFormat format);

struct NativeColorTargetLayout {
	uint64_t address = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t levels = 0;
	uint32_t mip = 0;
	uint32_t slice = 0; // CB_COLOR_VIEW base array slice: a pass per slice (see LayeredImageEnsure)
	MTLPixelFormat format = MTLPixelFormatInvalid;
	bool operator==(const NativeColorTargetLayout&) const = default;
};

static std::array<NativeColorTargetLayout, 8> NativeColorLayout(const HW::Context& context) {
	std::array<NativeColorTargetLayout, 8> layout {};
	const uint32_t slots = NativeColorTargetMask(context);
	for (uint32_t slot = 0; slot < layout.size(); ++slot) {
		if ((slots & (1u << slot)) == 0) continue;
		const auto& rt = context.GetRenderTarget(slot);
		auto& color = layout[slot];
		color.address = rt.base.addr;
		color.width = rt.attrib2.width + 1u;
		color.height = rt.attrib2.height + 1u;
		// CB_COLOR_ATTRIB2.MAX_MIP describes this allocation. A sampled SRD at the
		// same VA may describe another allocation or view; it cannot enlarge it.
		color.levels = rt.attrib2.num_mip_levels + 1u;
		color.mip = rt.view.current_mip_level;
		color.slice = rt.view.base_array_slice_index;
		color.format = MetalFormatFromRenderTarget(rt.info.format, rt.info.channel_type, rt.info.channel_order);
		if (color.format == MTLPixelFormatInvalid) {
			FailMetalMaterialization("render target format", static_cast<uint32_t>(rt.info.format),
			             static_cast<uint32_t>(rt.info.channel_type), static_cast<uint32_t>(rt.info.channel_order));
			color.format = MTLPixelFormatBGRA8Unorm;
		}
	}
	return layout;
}

static void ConfigureNativeColorTargets(MetalRenderPipelineKey& key, const HW::Context& context) {
	const auto layout = NativeColorLayout(context);
	for (uint32_t slot = 0; slot < layout.size(); ++slot) {
		if (layout[slot].address != 0) ConfigureNativeColorTarget(key, context, slot, layout[slot].format);
	}
}

static void EnsureActiveRenderPass(const HW::Context& context) {
	const auto& rt0 = context.GetRenderTarget(0);
	const auto& dt  = context.GetDepthRenderTarget();

	if (rt0.base.addr >= 0x8170000000ull && rt0.base.addr < 0x8180000000ull) {
		static std::atomic<uint32_t> s_flip_pass {0};
		if (s_flip_pass.fetch_add(1) < 20) {
			std::fprintf(stderr, "[FlipWatch] render pass targeted flip rt=0x%llx fmt=%u\n",
			             static_cast<unsigned long long>(rt0.base.addr),
			             static_cast<uint32_t>(rt0.info.format));
		}
	}
	const auto color_layout = NativeColorLayout(context);
	if (NativeColorTargetMask(context) == 0 && dt.z_read_base_addr == 0) {
		return;
	}

	const auto& vp0 = context.GetScreenViewport().viewports[0];
	// A viewport with a flipped Y axis has a negative scale: the guard used to fail for it and every depth-only pass (shadow maps, half
	// resolution depth) got the default height of 1080 - the 812x1080 depth surface of an 812x456 pass.
	uint32_t width  = rt0.attrib2.width > 0 ? (rt0.attrib2.width + 1) :
	                  (std::fabs(vp0.xscale) > 0 ? static_cast<uint32_t>(std::fabs(vp0.xscale) * 2.0) : 1920);
	uint32_t height = rt0.attrib2.height > 0 ? (rt0.attrib2.height + 1) :
	                  (std::fabs(vp0.yscale) > 0 ? static_cast<uint32_t>(std::fabs(vp0.yscale) * 2.0) : 1080);
	if (width == 0) width = 1920;
	if (height == 0) height = 1080;

	// Every active MRT slot participates in pass compatibility. A G-buffer draw can
	// change an auxiliary target while retaining RT0 and the same depth buffer.
	static std::array<NativeColorTargetLayout, 8> s_pass_colors {};
	static uint64_t s_pass_z_addr = 0;
	static std::array<uint64_t,5> s_pass_plane_writes {};
	static std::array<uint32_t, 5> s_pass_depth_view {};
	static uint64_t s_pass_stencil_addr = 0;
	static uint32_t s_pass_width = 0;
	static uint32_t s_pass_height = 0;
	static Prospero::DepthFormat s_pass_z_format = Prospero::DepthFormat::kInvalid;
	static uint64_t s_pass_backing_generation = 0;
	const uint64_t backing_generation = LibKernel::Memory::GetGuestBackingGeneration();
	const bool pass_has_stencil = GuestPassHasStencil(context);
	const std::array<uint32_t,5> depth_view {dt.depth_view.slice_start, dt.depth_view.slice_max,
	    dt.depth_view.current_mip_level, dt.size.x_max, dt.size.y_max};
	const std::array<uint64_t,5> plane_writes {dt.z_write_base_addr,dt.stencil_write_base_addr,
	    GuestDepthWritesEnabled(context),dt.depth_view.stencil_write_disable,context.GetDepthControl().stencil_enable};
	const bool same_pass = s_pass_colors == color_layout &&
	                       s_pass_backing_generation == backing_generation && s_pass_plane_writes == plane_writes &&
	                       s_pass_z_addr == dt.z_read_base_addr && s_pass_depth_view == depth_view && s_pass_z_format == dt.z_info.format &&
	                       s_pass_stencil_addr == (pass_has_stencil ? dt.stencil_read_base_addr : 0ull) &&
	                       s_pass_width == width && s_pass_height == height;

	// DB_RENDER_CONTROL.DEPTH_CLEAR_ENABLE: this draw is a depth clear (the hardware writes DB_DEPTH_CLEAR instead of running the
	// shader), so a new pass starts with a clear load action even when the render targets are unchanged.
	const bool depth_clear_draw = context.GetRenderControl().depth_clear_enable && dt.z_read_base_addr != 0;
	const bool stencil_clear_draw = pass_has_stencil && GuestStencilClearEnabled(context);
	if (MetalContext::Instance().GetActiveRenderEncoder() != nil) {
		if (same_pass && !depth_clear_draw && !stencil_clear_draw) {
			return;
		}
		MetalDrawExecutor::Instance().EndRenderPass();
	}
	s_pass_colors = {};
	s_pass_z_addr = 0;
	s_pass_stencil_addr = 0;
	s_pass_width = 0;
	s_pass_height = 0;
	s_pass_z_format = Prospero::DepthFormat::kInvalid;

	MetalRenderPassDesc pass_desc {};

	for (uint32_t slot = 0; slot < color_layout.size(); ++slot) {
		const auto& color = color_layout[slot];
		if (color.address == 0) continue;
		const auto& rt0 = context.GetRenderTarget(slot);
		const uint32_t width = color.width;
		const uint32_t height = color.height;
		const uint32_t rt_mip_levels = color.levels;
		const uint32_t cur_mip = color.mip;
		const MTLPixelFormat fmt = color.format;
		// KYTY_LOG_RT_SLICE=1: colour targets bound with an array slice view (first time per address, slice and size).
		if (KYTY_ENV("KYTY_LOG_RT_SLICE") != nullptr && (rt0.view.base_array_slice_index != 0 || rt0.view.last_array_slice_index != 0)) {
			static std::mutex s_slice_mutex;
			static std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t>> s_slice_seen;
			std::lock_guard<std::mutex> lock(s_slice_mutex);
			if (s_slice_seen.insert({rt0.base.addr, rt0.view.base_array_slice_index, width, static_cast<uint32_t>(fmt)}).second)
				std::fprintf(stderr, "[RtSlice] flip=%llu addr=0x%llx %ux%u fmt=%lu slice=%u..%u mip=%u levels=%u\n",
				             (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(), (unsigned long long)rt0.base.addr, width, height,
				             (unsigned long)fmt, rt0.view.base_array_slice_index, rt0.view.last_array_slice_index, cur_mip, rt_mip_levels);
		}
		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		NSUInteger align = LinearTextureRowAlignment(device, fmt);
		if (align == 0) {
			align = 256;
		}
		if (align < 64) align = 64;

		uint32_t bpp = BytesPerPixel(fmt);
		if (bpp == 0) bpp = 4;
		size_t bytes_per_row = (width * bpp + align - 1) & ~(align - 1);
		// With KYTY_UMA_SURFACES the guest-memory views of one surface share the guest pitch (see GuestViewRowBytes).
		if (UmaSurfacesEnabled() && !IsCompressedFormat(fmt)) bytes_per_row = GuestViewRowBytes(fmt, width, rt0.attrib3.tile_mode);
		size_t total_bytes = bytes_per_row * height;
		TraceFlipOverlap("draw_rt", rt0.base.addr, total_bytes);
		// KYTY_LOG_RT_LAYOUT=1: every distinct render-target layout the guest declares (guest tile mode vs the linear host view).
		{
			static const bool s_log_rtl = KYTY_ENV("KYTY_LOG_RT_LAYOUT") != nullptr;
			if (s_log_rtl) {
				static std::mutex s_rtl_mutex;
				static std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>> s_rtl_seen;
				std::lock_guard<std::mutex> rtl_lock(s_rtl_mutex);
				if (s_rtl_seen.emplace(rt0.base.addr, width, height, static_cast<uint32_t>(fmt),
				                       static_cast<uint32_t>(rt0.attrib3.tile_mode)).second) {
					std::fprintf(stderr, "[RtLayout] flip=%llu slot=%u addr=0x%llx %ux%u mtlfmt=%lu guestfmt=%u tile=%u bpp=%u host_bpr=%zu align=%lu mips=%u cmask=0x%llx dcc=%d\n",
					             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)), slot,
					             static_cast<unsigned long long>(rt0.base.addr), width, height, static_cast<unsigned long>(fmt),
					             static_cast<unsigned>(rt0.info.format), static_cast<unsigned>(rt0.attrib3.tile_mode), bpp, bytes_per_row,
					             static_cast<unsigned long>(align), rt_mip_levels, static_cast<unsigned long long>(rt0.cmask.addr),
					             rt0.info.dcc_compression_enable ? 1 : 0);
				}
			}
		}

		struct CachedRT {
			id<MTLTexture> tex = nil;
			id<MTLBuffer>  buf = nil;
			uint32_t       width = 0;
			uint32_t       height = 0;
			MTLPixelFormat fmt = MTLPixelFormatInvalid;
		};
		static std::unordered_map<uint64_t, std::unordered_map<uint32_t, CachedRT>> s_rt_cache;

		id<MTLTexture> color_tex = nil;
		auto& by_fmt = s_rt_cache[rt0.base.addr];
		auto it = by_fmt.find(static_cast<uint32_t>(fmt));
		const bool mipped_rt = rt_mip_levels > 1u || cur_mip > 0u;
		// A single mip is tiled guest memory too. Routing it through a linear
		// host view loses its physical identity and corrupts later tiled aliases.
		const bool canonical_rt = mipped_rt || rt0.attrib3.tile_mode != Prospero::TileMode::kLinear;
		const bool buffer_backed_rt = !canonical_rt && !IsCompressedFormat(fmt) &&
		    !(PrivateRenderTargets() && !(rt0.base.addr >= 0x8170000000ull && rt0.base.addr < 0x8180000000ull));
		MetalBufferAllocation rt_allocation;
		bool rt_cache_valid = it != by_fmt.end() && it->second.width == width && it->second.height == height;
		if (canonical_rt && rt_cache_valid && it->second.tex.buffer != nil) rt_cache_valid = false;
		if (buffer_backed_rt) {
			// The same virtual address can now refer to another physical page.
			// Reuse the buffer manager's canonical backing, just like raw GPU buffers.
			rt_allocation = MetalBufferManager::Instance().GetBufferForGuestRange(rt0.base.addr, total_bytes);
			GuestWrites::NoteWrite(rt0.base.addr, total_bytes); // this draw renders straight into guest memory
			if (rt_cache_valid && !GuestMemoryViewMatches(it->second.tex, rt_allocation, fmt, width, height, bytes_per_row)) {
				ForgetGpuWriter(rt0.base.addr, it->second.tex);
				rt_cache_valid = false;
			}
		}
		if (mipped_rt) {
			// Real mip chain (not backed by guest memory): sampled mips and per-level passes need separate levels.
			static std::map<RenderTextureKey, id<MTLTexture>> s_mip_rt_cache;
			const RenderTextureKey key{rt0.base.addr, static_cast<uint32_t>(fmt), width, height, rt_mip_levels};
			auto mit = s_mip_rt_cache.find(key);
			if (mit != s_mip_rt_cache.end()) {
				color_tex = mit->second;
			} else {
				MTLTextureDescriptor* md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
				                                                                              width:width
				                                                                             height:height
				                                                                          mipmapped:YES];
				if (rt_mip_levels > md.mipmapLevelCount || cur_mip >= rt_mip_levels) {
					std::fprintf(stderr, "[MetalMip] slot=%u addr=%llx size=%ux%u levels=%u mip=%u host_levels=%lu attrib2_mips=%u attrib3_depth=%u\n",
					             slot, (unsigned long long)rt0.base.addr, width, height, rt_mip_levels, cur_mip,
					             (unsigned long)md.mipmapLevelCount, rt0.attrib2.num_mip_levels, rt0.attrib3.depth);
					RejectMetalState("invalid render-target mip range");
				}
				md.mipmapLevelCount = rt_mip_levels;
				md.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView |
				           (GuestTextureUsage(fmt) & MTLTextureUsageShaderWrite);
				md.storageMode = MTLStorageModeShared;
				color_tex = KytyNewTexture(MetalContext::Instance().GetDevice(), md, __LINE__);
				s_mip_rt_cache[key] = color_tex;
				std::fprintf(stderr, "[RtMip] addr=0x%llx %ux%u levels=%u cur=%u fmt=%lu\n", (unsigned long long)rt0.base.addr, width,
				             height, rt_mip_levels, cur_mip, (unsigned long)fmt);
			}
		} else if (rt_cache_valid) {
			color_tex = it->second.tex;
		} else if (IsCompressedFormat(fmt)) {
			FailMetalMaterialization("compressed render target cannot use a linear buffer view", static_cast<uint32_t>(fmt));
			MTLTextureDescriptor* tex_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
			                                                                                    width:width
			                                                                                   height:height
			                                                                                mipmapped:NO];
			tex_desc.usage = GuestTextureUsage(fmt);
			tex_desc.storageMode = MTLStorageModeShared;
			tex_desc.allowGPUOptimizedContents = NO;
			color_tex = KytyNewTexture(device, tex_desc, __LINE__);
			if (color_tex) {
				by_fmt[static_cast<uint32_t>(fmt)] = {color_tex, nil, width, height, fmt};
			}
		} else if (canonical_rt || (PrivateRenderTargets() && !(rt0.base.addr >= 0x8170000000ull && rt0.base.addr < 0x8180000000ull))) {
			// KYTY_RT_PRIVATE=1: one texture per (address, format, size), like the Vulkan reference's image cache, instead of a view of the guest
			// pages. Resources that share transient memory then keep their own content between uses (a light pass that covers a few tiles of
			// the HDR target no longer leaves the bytes of whatever else lived at that address - NaN - in the rest). Video-out buffers stay
			// guest-memory views.
			MTLTextureDescriptor* tex_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
			                                                                                    width:width
			                                                                                   height:height
			                                                                                mipmapped:NO];
			tex_desc.usage       = MTLTextureUsageRenderTarget | GuestTextureUsage(fmt) | MTLTextureUsagePixelFormatView;
			tex_desc.storageMode = MTLStorageModeShared;
			color_tex            = KytyNewTexture(device, tex_desc, __LINE__);
			if (color_tex) {
				by_fmt[static_cast<uint32_t>(fmt)] = {color_tex, nil, width, height, fmt};
			}
		} else {
			if (rt_allocation.IsValid()) {
				MTLTextureDescriptor* tex_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
				                                                                                    width:width
				                                                                                   height:height
				                                                                                mipmapped:NO];
				tex_desc.usage = MTLTextureUsageRenderTarget | GuestTextureUsage(fmt);
				tex_desc.storageMode = MTLStorageModeShared;

				color_tex = CreateGuestMemoryView(rt_allocation, tex_desc, bytes_per_row);
				if (color_tex) {
					by_fmt[static_cast<uint32_t>(fmt)] = {color_tex, rt_allocation.buffer, width, height, fmt};
				}
			}
		}

		// A target bound with an array slice (cube faces, texture arrays) renders into its slice of the layered image of the address.
		if (color_tex && !canonical_rt && (color.slice > 0 || LayeredImageExists(rt0.base.addr, width, height))) {
			if (id<MTLTexture> arr = LayeredImageEnsure(rt0.base.addr, width, height, fmt, std::max<uint32_t>(color.slice + 1u, 6u), 1u, true,
			                                            color_tex)) {
				if (id<MTLTexture> slice_view = LayeredView(arr, fmt, MTLTextureType2D, 0, 1, color.slice, 1)) color_tex = slice_view;
			}
		}
		if (color_tex && !mipped_rt && !IsCompressedFormat(fmt) && color_tex.buffer != nil) NoteGpuWrite(rt0.base.addr, total_bytes);
		if (color_tex) {
			// Opt-in observation before this pass writes anything. The normal
			// DrawPassTrace statistics are post-draw and cannot establish this.
			static const uint64_t s_pre_rt = [] {
				const char* value = KYTY_ENV("KYTY_PASS_TRACE_PRE_RT");
				return value && std::strcmp(value, "hdr") == 0 ? UINT64_MAX : value ? std::strtoull(value, nullptr, 0) : 0ull;
			}();
			const bool pre_matches = s_pre_rt == UINT64_MAX
			    ? fmt == MTLPixelFormatRG11B10Float && width >= 800 && height >= 450 : s_pre_rt != 0 && rt0.base.addr == s_pre_rt;
			if (pre_matches && PassTraceActive()) {
				MetalDrawExecutor::Instance().EndRenderPass();
				MetalContext::Instance().CommitCommandBuffer(true);
				uint64_t writer_flip = 0;
				id<MTLTexture> writer = LastGpuWriterTexture(rt0.base.addr, &writer_flip);
				std::fprintf(stderr, "[PassTrace] PRE-RT flip=%llu addr=0x%llx fmt=%lu tex=%p writer=%p writer_fmt=%lu writer_flip=%llu %s\n",
				             (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(), (unsigned long long)rt0.base.addr,
				             (unsigned long)fmt, (__bridge void*)color_tex, (__bridge void*)writer,
				             (unsigned long)(writer ? writer.pixelFormat : MTLPixelFormatInvalid),
				             (unsigned long long)writer_flip, PassTraceStats(color_tex).c_str());
			}
			if (rt0.base.addr >= 0x8170000000ull && rt0.base.addr < 0x8180000000ull) {
				static std::atomic<uint32_t> s_flip_rt {0};
				if (s_flip_rt.fetch_add(1) < 20) {
					std::fprintf(stderr, "[FlipWatch] draw rt=0x%llx %ux%u fmt=%u\n",
					             static_cast<unsigned long long>(rt0.base.addr), width, height,
					             static_cast<uint32_t>(fmt));
				}
			}
			{
				PhysicalBackingSnapshot physical;
				if(canonical_rt) {
					TileSurfaceLayout allocation{};std::vector<PhysicalSpan> refresh;
					if(!TileGetTiledTextureLayout({RepresentativeFormat(BytesPerPixel(fmt)),rt0.attrib3.tile_mode,TileSurfaceDimension::Dim2D,
					    width,height,1,rt_mip_levels,color.slice+1},allocation) || !CanonicalMipSpans(rt0.base.addr,allocation.total_size,refresh) ||
					    !physical.Capture(s_canonical_mappings,rt0.base.addr,allocation.total_size))
						RejectMetalState("canonical render-target backing snapshot failed");
				}
				const uint64_t chain_bytes = mipped_rt ? static_cast<uint64_t>(width) * height * BytesPerPixel(fmt) : 0ull;
				const uint64_t chain_hash  = chain_bytes != 0 ? GuestTexSourceHash(rt0.base.addr, chain_bytes) : 0ull;
				std::lock_guard<std::mutex> lock(s_render_target_mutex);
				if (canonical_rt) s_render_target_addresses.erase(rt0.base.addr);
				else s_render_target_addresses.insert(rt0.base.addr);
				s_rt_by_addr[rt0.base.addr][static_cast<uint32_t>(fmt)] = {
				    color_tex, width, height, static_cast<uint32_t>(fmt),
				    s_rt_sequence.fetch_add(1, std::memory_order_relaxed),
				    Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed), chain_hash != 0 ? chain_bytes : 0ull, chain_hash,
				    std::move(physical),canonical_rt};
			}
			// A pass renders into this surface: it is now the last writer of the guest address (see AliasConvert).
			if (!canonical_rt) SyncStorageIntoGuestTarget(rt0.base.addr, color_tex);
			// A chain's pass writes the address too. Left out, the last writer stayed an older surface there: a reader of this 960x540 RGBA16F
			// DoF target (allocated with levels=11) got AliasConvert of last frame's RG11B10 surface, whose NaN and +-31000 the bloom pyramid spread
			// over the frame as washouts, coloured waves and bands. Which targets are chains depends on the game's allocator, so it varied by run.
			const auto writer_serial=RecordComputeStorage(rt0.base.addr, color_tex, rt0.attrib3.tile_mode);
			{
				std::lock_guard<std::mutex> lock(s_render_target_mutex);
				s_rt_by_addr[rt0.base.addr][static_cast<uint32_t>(fmt)].writer_serial=writer_serial;
			}
			pass_desc.color_attachment_count = slot + 1u;
			pass_desc.color_attachments[slot].texture = color_tex;
			pass_desc.color_attachments[slot].level = mipped_rt ? cur_mip : 0u;
			if (canonical_rt) ConfigureCanonicalMipTarget(pass_desc.color_attachments[slot],rt0.base.addr,rt0.attrib3.tile_mode,rt_mip_levels,color.slice);
			bool fc_clear_now = false;
			float fc_color[4] {};
			RegisterMetaClearRt(rt0.base.addr, rt0.dcc_addr.addr, rt0.cmask.addr, rt0.info.dcc_compression_enable,
			                    rt0.cmask.addr != 0 && rt0.info.cmask_fast_clear_enable, rt0.clear_word0.word0, rt0.clear_word1.word1, fmt);
			// Level 0 of a mip chain too: the UI layer (1920x1080 RGBA8, 6 levels, its levels are the blur pyramid) clears through CMASK every
			// frame. Skipping it kept the last frame's pixels, so its translucent panels accumulated into an opaque dark panel over the menu.
			if ((cur_mip == 0u) && TakePendingMetaClear(rt0.base.addr, fc_color)) {
				fc_clear_now = true;
			}
			static const bool s_log_meta_rt = KYTY_ENV("KYTY_LOG_META_FILL") != nullptr;
			if (s_log_meta_rt && rt0.cmask.addr != 0 && width >= 800) {
				static std::mutex s_mr_mutex;
				static std::unordered_map<uint64_t, uint64_t> s_mr_flip;
				const uint64_t mflip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
				std::lock_guard<std::mutex> mr_lock(s_mr_mutex);
				auto& seen = s_mr_flip[rt0.base.addr];
				if (seen != mflip) {
					seen = mflip;
					if (mflip >= 1500) {
						std::fprintf(stderr, "[MetaRt] flip=%llu slot=%u rt=0x%llx %ux%u f%lu cmask=0x%llx fast=%d mipped=%d clear=%d\n", (unsigned long long)mflip, slot,
						             (unsigned long long)rt0.base.addr, width, height, (unsigned long)fmt, (unsigned long long)rt0.cmask.addr,
						             rt0.info.cmask_fast_clear_enable ? 1 : 0, mipped_rt ? 1 : 0, fc_clear_now ? 1 : 0);
					}
				}
			}
			// KYTY_WATCH_RT=<lo>,<hi>: the first bind of each (address, size, format) colour target with lo <= width <= hi in every 300 flips,
			// with the flip (finds when a surface the game keeps across frames was last rendered).
			if (const char* wr = KYTY_ENV("KYTY_WATCH_RT")) {
				unsigned lo = 0, hi = 0;
				std::sscanf(wr, "%u,%u", &lo, &hi);
				if (width >= lo && width <= hi) {
					static std::mutex s_wr_mutex;
					static std::set<std::tuple<uint64_t, uint32_t, uint64_t, uint64_t>> s_wr_seen;
					const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
					std::lock_guard<std::mutex> wr_lock(s_wr_mutex);
					if (s_wr_seen.insert({rt0.base.addr, width * 65536u + height, static_cast<uint64_t>(fmt), flip / 300}).second) {
						std::fprintf(stderr, "[WatchRT] flip=%llu slot=%u 0x%llx %ux%u f%lu\n", (unsigned long long)flip, slot,
						             (unsigned long long)rt0.base.addr, width, height, (unsigned long)fmt);
					}
				}
			}
			// KYTY_LOG_RT_SAMPLES=1: every distinct (address, size, format, samples, fragments) colour target bound for a draw, once, and a running
			// count of MSAA binds. This backend renders every target single-sampled (sample_count is never set from the guest), so a guest
			// MSAA target in the UI pass would show up here.
			if (KYTY_ENV("KYTY_LOG_RT_SAMPLES") != nullptr) {
				static std::mutex s_rs_mutex;
				static std::set<std::tuple<uint64_t, uint32_t, uint64_t, uint32_t, uint32_t>> s_rs_seen;
				static uint64_t s_rs_binds = 0, s_rs_msaa = 0;
				std::lock_guard<std::mutex> rs_lock(s_rs_mutex);
				++s_rs_binds;
				const bool ms = rt0.attrib.num_samples > 0 || rt0.attrib.num_fragments > 0;
				s_rs_msaa += ms;
				if (ms && s_rs_seen.insert({rt0.base.addr, width * 65536u + height, static_cast<uint64_t>(fmt), rt0.attrib.num_samples, rt0.attrib.num_fragments}).second) {
					std::fprintf(stderr, "[RtSamples] flip=%llu slot=%u 0x%llx %ux%u f%lu num_samples=%u num_fragments=%u fmask_one_frag=%d (msaa binds %llu of %llu)\n",
					             (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed), slot, (unsigned long long)rt0.base.addr, width,
					             height, (unsigned long)fmt, rt0.attrib.num_samples, rt0.attrib.num_fragments, rt0.info.fmask_one_frag_mode ? 1 : 0,
					             (unsigned long long)s_rs_msaa, (unsigned long long)s_rs_binds);
				}
				if (s_rs_binds % 20000 == 0) {
					std::fprintf(stderr, "[RtSamples] binds=%llu msaa=%llu\n", (unsigned long long)s_rs_binds, (unsigned long long)s_rs_msaa);
				}
			}
			if (PassTraceActive()) {
				// What the guest declares about this target's metadata: a DCC/CMASK fast-cleared target reads as its clear colour where nothing was drawn.
				std::fprintf(stderr, "[PassTrace]   RTMETA slot=%u 0x%llx %ux%u f%lu tile=%u dcc=%d/0x%llx cmask=0x%llx fast=%d clear=%08x:%08x metaclear=%d\n", slot,
				             static_cast<unsigned long long>(rt0.base.addr), width, height, static_cast<unsigned long>(fmt), static_cast<unsigned>(rt0.attrib3.tile_mode),
				             rt0.info.dcc_compression_enable ? 1 : 0, static_cast<unsigned long long>(rt0.dcc_addr.addr), static_cast<unsigned long long>(rt0.cmask.addr),
				             rt0.info.cmask_fast_clear_enable ? 1 : 0, rt0.clear_word0.word0, rt0.clear_word1.word1, fc_clear_now ? 1 : 0);
			}
			pass_desc.color_attachments[slot].load_action = fc_clear_now ? MTLLoadActionClear : MTLLoadActionLoad;
			pass_desc.color_attachments[slot].store_action = MTLStoreActionStore;
			if (fc_clear_now) {
				for (uint32_t c = 0; c < 4; ++c) pass_desc.color_attachments[slot].clear_color[c] = fc_color[c];
			}

			// Only consider displayable color formats:
			// RG11B10Float (92), RGBA8 (70/71), BGRA8 (80/81), RGB10A2 (90), RGBA16 (115/110)
			// Exclude non-color / auxiliary formats: RG16Float (65 - motion vectors), R8, RG8, etc.
			bool is_displayable_color = (fmt == MTLPixelFormatRG11B10Float ||
			                             fmt == MTLPixelFormatRGBA8Unorm ||
			                             fmt == MTLPixelFormatRGBA8Unorm_sRGB ||
			                             fmt == MTLPixelFormatBGRA8Unorm ||
			                             fmt == MTLPixelFormatBGRA8Unorm_sRGB ||
			                             fmt == MTLPixelFormatRGB10A2Unorm ||
			                             fmt == MTLPixelFormatRGBA16Float ||
			                             fmt == MTLPixelFormatRGBA16Unorm);

			if (slot == 0 && width >= 960 && height >= 540 && is_displayable_color) {
				s_rt_by_format[static_cast<uint32_t>(fmt)] = color_tex;
				s_last_rt_tex = color_tex;
				s_last_rt_addr.store(rt0.base.addr, std::memory_order_relaxed);
				s_last_rt_width.store(width, std::memory_order_relaxed);
				s_last_rt_height.store(height, std::memory_order_relaxed);
				s_last_rt_fmt.store(static_cast<uint32_t>(fmt), std::memory_order_relaxed);
			} else if (slot == 0 && s_last_rt_tex == nil && is_displayable_color) {
				s_last_rt_tex = color_tex;
				s_last_rt_addr.store(rt0.base.addr, std::memory_order_relaxed);
				s_last_rt_width.store(width, std::memory_order_relaxed);
				s_last_rt_height.store(height, std::memory_order_relaxed);
				s_last_rt_fmt.store(static_cast<uint32_t>(fmt), std::memory_order_relaxed);
			}
		} else {
			static std::atomic<uint32_t> s_rt_err {0};
			if (s_rt_err.fetch_add(1) < 5) {
				std::fprintf(stderr, "[MetalRenderer] Failed to create color_tex view for rt=0x%llx %ux%u fmt=%u bpr=%zu!\n",
				             (unsigned long long)rt0.base.addr, width, height, (uint32_t)fmt, bytes_per_row);
			}
		}
	}

	// Depth render target
	if (dt.z_read_base_addr != 0 && dt.z_info.format != Prospero::DepthFormat::kInvalid) {
		MTLPixelFormat depth_fmt = MetalFormatFromDepthFormat(dt.z_info.format, false);
		if (depth_fmt != MTLPixelFormatInvalid) {
			// One depth surface per guest address, kept across passes: depth written by one pass is tested by the next, and a
			// later reader of the same memory finds it through the surface converter.
			id<MTLDevice> dev = MetalContext::Instance().GetDevice();
			// The depth surface has the extent DB_DEPTH_SIZE_XY declares (what the Vulkan path uses), not the render area of the pass.
			const uint32_t depth_w = dt.size.valid ? static_cast<uint32_t>(dt.size.x_max) + 1u : width;
			const uint32_t depth_h = dt.size.valid ? static_cast<uint32_t>(dt.size.y_max) + 1u : height;
			// The game renders depth at transient addresses that change from frame to frame: a surface kept per address forever
			// grew by a 6 MB private texture per frame (hundreds alive, tens of GB in a long session). Surfaces not bound for
			// kDepthEvictFlips presented frames are dropped, together with the registry entries that still point at them.
			constexpr uint64_t kDepthEvictFlips = 120;
			const uint64_t depth_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
			{
				static uint64_t s_depth_sweep_flip = 0;
				if (depth_flip >= s_depth_sweep_flip + 30) {
					s_depth_sweep_flip = depth_flip;
					for (auto it = s_depth_by_addr.begin(); it != s_depth_by_addr.end();) {
						if (it->first != dt.z_read_base_addr && it->second.used_flip + kDepthEvictFlips < depth_flip) {
							ForgetGpuWriter(it->first, it->second.tex);
							it = s_depth_by_addr.erase(it);
						} else {
							++it;
						}
					}
				}
			}
			DepthSurface& ds = s_depth_by_addr[dt.z_read_base_addr];
			ds.used_flip = depth_flip;
			NoteSurfaceWrite(dt.z_read_base_addr);
			if (AddrLogWanted(dt.z_read_base_addr))
				std::fprintf(stderr, "[AddrLog] flip=%llu DEPTH-pass addr=0x%llx pass=%ux%u fmt=%lu existing=%lux%lu\n", static_cast<unsigned long long>(depth_flip),
				             static_cast<unsigned long long>(dt.z_read_base_addr), width, height, static_cast<unsigned long>(depth_fmt),
				             ds.tex != nil ? (unsigned long)ds.tex.width : 0ul, ds.tex != nil ? (unsigned long)ds.tex.height : 0ul);
			if(dt.z_info.num_samples!=0 || dt.z_info.max_mip_level!=0)
				RejectMetalState("canonical depth MSAA/mip allocation requires a physical sample/mip layout");
			const uint32_t first_layer = dt.depth_view.slice_start;
			const uint32_t layer_count = dt.depth_view.slice_max - first_layer + 1u;
			EXIT_IF(dt.depth_view.slice_max < first_layer || dt.depth_view.slice_max >= 2048u || dt.depth_view.current_mip_level != 0);
			MetalContext::Instance().EndCurrentEncoder();
			id<MTLCommandBuffer> depth_command = MetalContext::Instance().GetCurrentCommandBuffer();
			EXIT_IF(!ds.layers.Ensure(dev, depth_command, depth_w, depth_h, depth_fmt, dt.depth_view.slice_max + 1u));
			ds.tex = ds.layers.Texture().arrayLength == 1 ? ds.layers.View(0,1) : ds.layers.Texture();
			ds.w = depth_w; ds.h = depth_h; ds.fmt = depth_fmt;
			bool fresh = false;
			for (uint32_t layer = first_layer; layer < first_layer + layer_count; ++layer) fresh |= ds.layers.NeedsClear(layer);

			if (ds.tex) {
				pass_desc.has_depth = true;
				pass_desc.depth_attachment.texture = ds.layers.View(first_layer, layer_count);
				EXIT_IF(pass_desc.depth_attachment.texture == nil);
				pass_desc.render_target_array_length = layer_count;
				if (dt.z_info.htile_acceleration && dt.htile_data_base_addr != 0) RegisterHtile(dt.htile_data_base_addr, dt.z_read_base_addr);
				const auto pending_clears = TakePendingDepthClear(dt.z_read_base_addr, first_layer, layer_count);
				const bool htile_clear = !pending_clears.empty();
				const bool clear = depth_clear_draw || htile_clear;
				pass_desc.depth_attachment.load_action = MTLLoadActionLoad;
				pass_desc.depth_attachment.store_action = MTLStoreActionStore;
				const auto depth_transfer=ConfigureCanonicalDepthPlane(pass_desc.depth_attachment,dt.z_read_base_addr,
				    dt.z_write_base_addr,first_layer,layer_count,GuestDepthWritesEnabled(context) || depth_clear_draw || htile_clear);

				// A clear through HTILE (kernels write the tile metadata, the clear value is in it and not read here) must not take DB_DEPTH_CLEAR:
				// the register still holds the value of whichever pass set it last. The value 1 left by a forward-Z pass made a reverse-Z G-buffer
				// pass fail its depth test everywhere: no opaque geometry at all at full render size (um285). The far plane of the surface's
				// depth direction is what such a clear means. KYTY_DEPTH_CLEAR_REG=1 restores the register.
				const uint32_t zfunc_now = static_cast<uint32_t>(context.GetDepthControl().zfunc);
				if (context.GetDepthControl().z_enable) {
					if (zfunc_now == 4u || zfunc_now == 6u) ds.z_dir = 1;
					else if (zfunc_now == 1u || zfunc_now == 3u) ds.z_dir = 2;
				}
				static const bool s_depth_clear_reg = KYTY_ENV("KYTY_DEPTH_CLEAR_REG") != nullptr;
				const double depth_clear_value = (htile_clear && !s_depth_clear_reg && ds.z_dir != 0) ? (ds.z_dir == 1 ? 0.0 : 1.0)
				                                                                                       : static_cast<double>(context.GetDepthClearValue());
				pass_desc.depth_attachment.clear_depth = depth_clear_value;
				// Import before explicit metadata/register clears. A newly created
				// host texture is not evidence that guest depth is zero/far.
				if(clear) {
					EXIT_IF(!depth_transfer.Load(depth_command));
					for(uint32_t layer:pending_clears)EXIT_IF(!ds.layers.Clear(depth_command,layer,1,depth_clear_value,0));
					if(depth_clear_draw)EXIT_IF(!ds.layers.Clear(depth_command,first_layer,layer_count,context.GetDepthClearValue(),0));
					EXIT_IF(!depth_transfer.Store(depth_command));
				}
				EXIT_IF(!ds.layers.MarkImported(first_layer,layer_count));
				if (KYTY_ENV("KYTY_TRACE_RASTER_STATE") != nullptr && layer_count + first_layer > 1) {
					static std::set<std::tuple<uint64_t,uint32_t,uint32_t>> seen;
					if (seen.emplace(dt.z_read_base_addr,first_layer,layer_count).second)
						std::fprintf(stderr,"[DepthLayer] addr=0x%llx %ux%u layers=%lu view=%u:%u parent=%p attachment=%p\n",
						    (unsigned long long)dt.z_read_base_addr,depth_w,depth_h,(unsigned long)ds.layers.Texture().arrayLength,
						    first_layer,first_layer+layer_count-1,(__bridge void*)ds.layers.Texture(),(__bridge void*)pass_desc.depth_attachment.texture);
				}
				if (clear) {
					static const bool s_log_depth_clear = KYTY_ENV("KYTY_LOG_DEPTH_CLEAR") != nullptr;
					static std::atomic<uint32_t> s_depth_clear_logs {0};
					if (s_log_depth_clear && Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed) >= 1500 && s_depth_clear_logs.fetch_add(1) < 1500)
						std::fprintf(stderr, "[DepthClear] flip=%llu z=0x%llx %ux%u layer=%u:%u value=%g fresh=%d draw=%d htile=%d zfunc=%u\n",
						             (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed), (unsigned long long)dt.z_read_base_addr, depth_w, depth_h, first_layer, first_layer+layer_count-1,
						             depth_clear_value, fresh ? 1 : 0, depth_clear_draw ? 1 : 0, htile_clear ? 1 : 0, zfunc_now);
				}
				RecordComputeStorage(dt.z_read_base_addr, ds.tex, Prospero::TileMode::kDepth);

				if (pass_has_stencil) {
					// One Stencil8 surface per guest stencil address, kept across passes like the depth surface.
					struct StencilSurface {
						MetalAttachmentArray layers;
						id<MTLTexture> tex = nil;
						uint32_t w = 0, h = 0;
						uint64_t used_flip = 0;
					};
					static std::unordered_map<uint64_t, StencilSurface> s_stencil_by_addr;
					{
						// Same transient addresses as the depth surfaces: drop the ones not used for a while.
						static uint64_t s_stencil_sweep_flip = 0;
						if (depth_flip >= s_stencil_sweep_flip + 30) {
							s_stencil_sweep_flip = depth_flip;
							for (auto it = s_stencil_by_addr.begin(); it != s_stencil_by_addr.end();) {
								if (it->first != dt.stencil_read_base_addr && it->second.used_flip + kDepthEvictFlips < depth_flip) {
									it = s_stencil_by_addr.erase(it);
								} else {
									++it;
								}
							}
						}
					}
					StencilSurface& ss = s_stencil_by_addr[dt.stencil_read_base_addr];
					ss.used_flip = depth_flip;
					EXIT_IF(!ss.layers.Ensure(dev, depth_command, depth_w, depth_h, MTLPixelFormatStencil8, dt.depth_view.slice_max + 1u));
					ss.tex = ss.layers.Texture(); ss.w = depth_w; ss.h = depth_h;

					if (ss.tex) {
						pass_desc.has_stencil = true;
						pass_desc.stencil_attachment.texture = ss.layers.View(first_layer, layer_count);
						const bool sclear = stencil_clear_draw;
						pass_desc.stencil_attachment.load_action = MTLLoadActionLoad;
						pass_desc.stencil_attachment.store_action = MTLStoreActionStore;
						pass_desc.stencil_attachment.clear_stencil = sclear ? context.GetStencilClearValue() : 0u;
						const auto stencil_transfer=ConfigureCanonicalDepthPlane(pass_desc.stencil_attachment,dt.stencil_read_base_addr,
						    dt.stencil_write_base_addr,first_layer,layer_count,(context.GetDepthControl().stencil_enable && !dt.depth_view.stencil_write_disable) || sclear);
						if(sclear) {
							EXIT_IF(!ss.layers.Clear(depth_command,first_layer,layer_count,0,context.GetStencilClearValue()));
							EXIT_IF(!stencil_transfer.Store(depth_command));
						}
						EXIT_IF(!ss.layers.MarkImported(first_layer,layer_count));
					}
				}
			}
		}
	}

	if (pass_desc.color_attachment_count > 0 || pass_desc.has_depth) {
		if (pass_desc.color_attachment_count > 1 && KYTY_ENV("KYTY_LOG_MRT") != nullptr) {
			static std::atomic<uint32_t> seen_slots {0};
			const uint32_t slots = NativeColorTargetMask(context);
			if ((slots & ~seen_slots.fetch_or(slots)) != 0) {
				std::fprintf(stderr, "[MRT] slots=%x target_mask=%08x shader_mask=%08x", NativeColorTargetMask(context),
				             context.GetRenderTargetMask(), context.GetShaderRegisters().m_cbShaderMask);
				for (uint32_t slot = 0; slot < color_layout.size(); ++slot) {
					const auto& color = color_layout[slot];
					if (color.address != 0) std::fprintf(stderr, " rt%u=%llx:%ux%u:%lu:m%u", slot,
					    (unsigned long long)color.address, color.width, color.height, (unsigned long)color.format, color.mip);
				}
				std::fprintf(stderr, "\n");
			}
		}
		s_last_pass_desc = pass_desc;
		s_last_pass_valid = true;
		MetalDrawExecutor::Instance().SetTraceVisibility(PassTraceFrags());
		if (MetalDrawExecutor::Instance().BeginRenderPass(pass_desc)) {
			s_pass_colors = color_layout;
			s_pass_z_addr = dt.z_read_base_addr;
			s_pass_plane_writes = plane_writes;
			s_pass_depth_view = depth_view;
			s_pass_stencil_addr = pass_desc.has_stencil ? dt.stencil_read_base_addr : 0ull;
			s_pass_width = width;
			s_pass_height = height;
			s_pass_z_format = dt.z_info.format;
			s_pass_backing_generation = backing_generation;
		}
	}
}

std::atomic<uint64_t> s_draw_seq {0};

static std::atomic<uint64_t> s_pending_indirect_addr {0};

// Multi-draw predicate: copies max_count entries of `stride` bytes from the game's arguments into a pooled
// scratch buffer and zeroes instanceCount (dword 1 of both AMD and Metal layouts) for entries >= *count.
// The game's own arguments are left untouched. Runs as its own compute encoder; the render pass is ended
// around it and reopened with loadAction = Load so the frame content is kept.
static id<MTLBuffer> s_multi_draw_buffer = nil;

static bool PrepareMultiDrawArguments(const PendingIndirectDraw& p, uint64_t vs_hash) {
	s_multi_draw_buffer = nil;
	if (p.max_count == 0) return false;
	const size_t argument_bytes = p.indexed ? 20u : 16u;
	const size_t bytes = static_cast<size_t>(p.max_count - 1) * p.stride + argument_bytes;
	const auto args = MetalBufferManager::Instance().GetBufferForGuestRange(p.args_addr, bytes);
	const auto count = p.count_addr ? MetalBufferManager::Instance().GetBufferForGuestRange(p.count_addr, 4)
	                                : MetalBufferAllocation {};
	if (args.buffer == nil || (p.count_addr && count.buffer == nil)) return false;
	uint32_t index_limit = 0;
	if (p.indexed) {
		const auto indices = MetalBufferManager::Instance().GetBufferForGuestRange(p.index_base, p.index_bytes);
		if (indices.buffer == nil || indices.offset > indices.buffer.length) return false;
		const uint64_t available = std::min<uint64_t>(p.index_bytes, indices.buffer.length - indices.offset);
		index_limit = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, available / (p.index_type == 1u ? 4u : 2u)));
	}
	MetalDrawExecutor::Instance().EndRenderPass();
	id<MTLCommandBuffer> command = MetalContext::Instance().GetCurrentCommandBuffer();
	static MetalMultiDrawIndirect prepare;
	s_multi_draw_buffer = prepare.Encode(command, args.buffer, args.offset, count.buffer, count.offset,
	                                    p.max_count, p.stride, p.indexed, index_limit);
	if (s_multi_draw_buffer == nil) return false;

	// Optional, bounded CPU inspection after this GPU snapshot has completed,
	// before any draw state is set. Normal rendering never reads these arguments.
	if (const char* hash = KYTY_ENV("KYTY_MDI_TRACE_HASH");
	    hash != nullptr && std::strtoull(hash, nullptr, 0) == vs_hash) {
		static uint32_t packets = 0;
		const uint32_t limit = KYTY_ENV("KYTY_MDI_TRACE_N") ? std::strtoul(KYTY_ENV("KYTY_MDI_TRACE_N"), nullptr, 0) : 4u;
		if (packets < limit) {
			const uint32_t packet = ++packets;
			char label[160];
			std::snprintf(label, sizeof(label), "mdi_snapshot vs=0x%016llx args=0x%llx max=%u stride=%u",
			              (unsigned long long)vs_hash, (unsigned long long)p.args_addr, p.max_count, p.stride);
			KytyFaultOpSet(label);
			MetalContext::Instance().CommitCommandBuffer(true);
			const auto* data = static_cast<const uint32_t*>(s_multi_draw_buffer.contents);
			for (uint32_t k = 0; k < std::min(p.max_count, 8u); ++k) {
				const uint32_t* cooked = data + k * (MetalMultiDrawIndirect::RecordBytes / 4);
				const uint32_t* raw = cooked + MetalMultiDrawIndirect::OriginalArgumentsOffset / 4;
				std::fprintf(stderr, "[MdiArgs] packet=%u k=%u vs=0x%016llx args=0x%llx indexed=%d raw=%u,%u,%u,%d,%u gpu_count=%u index_limit=%u cooked=%u,%u,%u,%d,%u\n",
				             packet, k, (unsigned long long)vs_hash, (unsigned long long)p.args_addr, p.indexed,
				             raw[0], raw[1], raw[2], int32_t(raw[3]), raw[4], raw[5], raw[6],
				             cooked[0], cooked[1], cooked[2], int32_t(cooked[3]), cooked[4]);
			}
			std::fflush(stderr);
		}
	}
	if (s_last_pass_valid) {
		MetalRenderPassDesc reopen = s_last_pass_desc;
		for (uint32_t i = 0; i < reopen.color_attachment_count; ++i)
			reopen.color_attachments[i].load_action = MTLLoadActionLoad;
		if (reopen.has_depth) reopen.depth_attachment.load_action = MTLLoadActionLoad;
		if (reopen.has_stencil) reopen.stencil_attachment.load_action = MTLLoadActionLoad;
		MetalDrawExecutor::Instance().BeginRenderPass(reopen);
	}
	return true;
}

static bool MultiDrawHasPreparedArguments(const PendingIndirectDraw& p) {
	return p.indexed || p.count_addr != 0;
}

static uint32_t MultiDrawArgumentsStride(const PendingIndirectDraw& p) {
	return MultiDrawHasPreparedArguments(p) ? MetalMultiDrawIndirect::RecordBytes : p.stride;
}

static id<MTLBuffer> MultiDrawArguments(const PendingIndirectDraw& p, uint64_t& base_offset) {
	base_offset = 0;
	if (MultiDrawHasPreparedArguments(p)) return s_multi_draw_buffer;
	const auto alloc = MetalBufferManager::Instance().GetBufferForGuestRange(p.args_addr, static_cast<size_t>(p.max_count) * p.stride);
	base_offset = alloc.offset;
	return alloc.buffer;
}

static std::mutex s_pending_indirect_mutex;
static PendingIndirectDraw s_pending_indirect {};

void SetPendingIndirectDraw(PendingIndirectDraw pending) {
	std::lock_guard<std::mutex> lock(s_pending_indirect_mutex);
	s_pending_indirect = pending;
}

PendingIndirectDraw TakePendingIndirectDraw() {
	std::lock_guard<std::mutex> lock(s_pending_indirect_mutex);
	PendingIndirectDraw out = s_pending_indirect;
	s_pending_indirect = PendingIndirectDraw {};
	return out;
}

static std::mutex s_cs_written_mutex;
static std::unordered_map<uint64_t, void*> s_cs_written_by_addr;

void RegisterComputeWrittenImage(uint64_t addr, void* texture) {
	if (addr == 0 || texture == nullptr) {
		return;
	}
	static const bool s_log_reg = KYTY_ENV("KYTY_LOG_REG") != nullptr;
	static std::atomic<uint32_t> s_reg_n {0};
	if (s_log_reg && addr >= 0x8179000000ull && addr < 0x817c000000ull && s_reg_n.fetch_add(1) < 40) {
		id<MTLTexture> rt = (__bridge id<MTLTexture>)texture;
		std::fprintf(stderr, "[Reg] addr=0x%llx tex=%p %lux%lux%lu fmt=%lu usage=%lu storage=%lu\n", (unsigned long long)addr, texture,
		             (unsigned long)rt.width, (unsigned long)rt.height, (unsigned long)rt.depth,
		             (unsigned long)rt.pixelFormat, (unsigned long)rt.usage, (unsigned long)rt.storageMode);
	}
	std::lock_guard<std::mutex> lock(s_cs_written_mutex);
	// The registry owns its textures: a raw pointer dangled once the texture cache replaced the object, and
	// the next lookup then called into freed (or reused) memory. A replaced texture may still be referenced by
	// in-flight command buffers (argument-buffer handles are not retained), so it is released late.
	static std::deque<void*> s_graveyard;
	const auto it = s_cs_written_by_addr.find(addr);
	if (it != s_cs_written_by_addr.end()) {
		if (it->second == texture) {
			return;
		}
		s_graveyard.push_back(it->second);
		while (s_graveyard.size() > 512) {
			CFRelease(s_graveyard.front());
			s_graveyard.pop_front();
		}
		it->second = const_cast<void*>(CFRetain(texture));
		return;
	}
	s_cs_written_by_addr[addr] = const_cast<void*>(CFRetain(texture));
}

void* GetComputeWrittenImageForAddress(uint64_t addr) {
	std::lock_guard<std::mutex> lock(s_cs_written_mutex);
	const auto it = s_cs_written_by_addr.find(addr);
	return it != s_cs_written_by_addr.end() ? it->second : nullptr;
}

uint64_t MetalDrawSequence() {
	return s_draw_seq.load(std::memory_order_relaxed);
}

// Guest textures are sampled and, for storage-image bindings, written by compute. Writes are
// only allowed for formats that Metal exposes as writable; others stay read-only.
static MTLTextureUsage GuestTextureUsage(MTLPixelFormat format) {
	switch (format) {
		case MTLPixelFormatR8Unorm:
		case MTLPixelFormatRG8Unorm:
		case MTLPixelFormatRGBA8Unorm:
		case MTLPixelFormatRGBA8Uint:
		case MTLPixelFormatR16Float:
		case MTLPixelFormatRG16Float:
		case MTLPixelFormatRGBA16Float:
		case MTLPixelFormatRGBA16Unorm:
		case MTLPixelFormatR32Float:
		case MTLPixelFormatRG32Float:
		case MTLPixelFormatRGBA32Float:
		case MTLPixelFormatR32Uint:
		case MTLPixelFormatRG32Uint:
		case MTLPixelFormatRGBA32Uint:
		// Integer views of byte-addressed copies (the UI atlas kernel writes R8 data as R8Uint): writes were dropped
		// with ShaderRead only, leaving the atlas empty.
		case MTLPixelFormatR8Uint:
		case MTLPixelFormatR8Sint:
		case MTLPixelFormatRG8Uint:
		case MTLPixelFormatRG8Sint:
		case MTLPixelFormatRGBA8Sint:
		case MTLPixelFormatR16Uint:
		case MTLPixelFormatR16Sint:
		case MTLPixelFormatRG16Uint:
		case MTLPixelFormatRG16Sint:
		case MTLPixelFormatRGBA16Uint:
		case MTLPixelFormatRGBA16Sint:
		case MTLPixelFormatR32Sint:
		case MTLPixelFormatRG32Sint:
		case MTLPixelFormatRGBA32Sint:
		case MTLPixelFormatRGB10A2Unorm:  // flip/composite output format (fmt 90); writes were dropped with ShaderRead only
		case MTLPixelFormatRG11B10Float:
			return MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
		default:
			return MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
	}
}

struct CachedGuestTex {
	id<MTLTexture> tex = nil;
	bool had_nz = false;
	uint64_t serial = 0;
	bool cs_written = false; // a compute shader owns the content: never refresh it from (empty) guest memory
	Prospero::TileMode tile = Prospero::TileMode::kLinear;
	uint64_t src_bytes = 0;    // extent of the guest bytes the copy was decoded from (0: a live view of guest memory, never stale)
	uint64_t src_hash = 0;     // GuestTexSourceHash of those bytes at decode time
	uint64_t checked_flip = 0;
	uint64_t used_flip = 0;    // last flip that bound it (0: not yet stamped)
	uint64_t decode_flip = 0;  // flip of the decode (KYTY_GUEST_TEX_VERIFY)
	uint32_t physical_levels = 1; // Layout identity is independent of the host/view mip count.
	uint32_t first_level = 0;
};
static std::unordered_map<uint64_t, CachedGuestTex> s_guest_textures;
static std::unordered_set<uint64_t> s_bc_written_addrs; // addresses a kernel wrote BC blocks to (KYTY_BC_DUMP_DIR only)

// GPU-encoded block-compressed textures: the game compresses kits, crests and badges on the GPU, with kernels that store the BC blocks
// into a tiled RGBA32Uint (16-byte blocks) or RGBA16Uint (8-byte blocks) image at the texture's address, and samples the result as a BC
// texture hundreds of flips later. A Metal storage image is a texture of its own (without KYTY_UMA_SURFACES), so the decode of guest
// memory showed noise on every GPU-compressed material. Each level of such an image is a private texture of its own size, and the decode
// lays the blocks the kernel wrote over the guest bytes. The blocks stay on the host on purpose: copying them into guest memory crashed
// the game (pointers 0xfeff, 0xffff and pixel values in its objects, 3 runs of 3 against 0 of 3 without the copy), also when only the written
struct BcLevelImage {
	id<MTLTexture> tex       = nil;
	uint32_t       web       = 0; // bytes per block
	uint64_t       used_flip = 0;
	Prospero::TileMode tile  = Prospero::TileMode::kLinear;
	uint64_t       guest_bytes = 0; // guest range checked for reuse, and its hash when the kernel last wrote the level
	uint64_t       guest_hash  = 0;
};
static std::map<std::pair<uint64_t, uint32_t>, BcLevelImage> s_bc_level_images; // by address and level
static constexpr uint32_t kBcUnwritten = 0xA5C35A3Cu;                           // fill value: blocks still holding it were not written

// Lays the written blocks of that level over the decoded guest bytes. The GPU is done with them: the dispatch that wrote them was waited for.
static uint64_t GuestTexSourceHash(uint64_t addr, uint64_t bytes);
static void BcOverlayLevel(uint64_t addr, uint32_t level, uint32_t block_bytes, std::vector<uint8_t>& linear, uint32_t pitch, Prospero::TileMode tile) {
	const auto it = s_bc_level_images.find({addr, level});
	if (it == s_bc_level_images.end() || it->second.web != block_bytes || pitch == 0) return;
	// The blocks stay on the host, so the kernel never changes these guest bytes. A different tile mode or changed bytes mean the game
	// reused the address for another texture (a CPU-loaded UI atlas of the same size showed the old blocks as coloured noise).
	static const bool s_keep_stale = false;
	if (!s_keep_stale && (it->second.tile != tile || GuestTexSourceHash(addr, it->second.guest_bytes) != it->second.guest_hash)) {
		static std::atomic<uint32_t> s_log {0};
		if (s_log.fetch_add(1) < 60) {
			std::fprintf(stderr, "[BcStale] addr=0x%llx level=%u %lux%lu tile=%u/%u bytes=%llu: address reused, level image dropped\n", (unsigned long long)addr, level,
			             (unsigned long)it->second.tex.width, (unsigned long)it->second.tex.height, (unsigned)it->second.tile, (unsigned)tile, (unsigned long long)it->second.guest_bytes);
		}
		s_bc_level_images.erase(it);
		return;
	}
	const uint32_t columns = pitch / block_bytes;
	const uint32_t rows    = static_cast<uint32_t>(linear.size() / pitch);
	id<MTLTexture> tex = it->second.tex;
	if (tex.width != columns || tex.height != rows) return;
	it->second.used_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	std::vector<uint32_t> blocks(static_cast<size_t>(columns) * rows * block_bytes / 4u);
	[tex getBytes:blocks.data() bytesPerRow:pitch fromRegion:MTLRegionMake2D(0, 0, columns, rows) mipmapLevel:0];
	const uint32_t words = block_bytes / 4u;
	for (size_t b = 0; b < static_cast<size_t>(columns) * rows; ++b) {
		const uint32_t* w = blocks.data() + b * words;
		bool written = false;
		for (uint32_t i = 0; i < words; ++i) written = written || w[i] != kBcUnwritten;
		if (written) std::memcpy(linear.data() + b * block_bytes, w, block_bytes);
	}
}

// Decoded copies are rebuilt from guest memory on demand, so copies no draw has bound for KYTY_GUEST_TEX_EVICT_FLIPS flips
// (opt-in, e.g. 900 = ~30 s; unset or 0 keeps everything) are dropped once per 60 flips. Opt-in because view caches are keyed by
// the texture object's address (ApplyDstSel, ReinterpretFormat): a freed copy whose address is reused by a new texture would get
// the old object's view back. Fix those keys before enabling it by default: the cache grew by every scene the game ever showed
// (10k copies, 3+ GB after a menu tour and a match). Copies a compute shader wrote hold GPU-only content and are kept.
static void EvictIdleGuestTextures(uint64_t flip) {
	static const uint64_t s_after = [] {
		const char* v = KYTY_ENV("KYTY_GUEST_TEX_EVICT_FLIPS");
		return v != nullptr ? std::strtoull(v, nullptr, 10) : 0ull;
	}();
	static uint64_t s_swept = 0;
	if (s_after == 0 || flip < s_swept + 60) return;
	s_swept = flip;
	size_t n = 0, bytes = 0;
	for (auto it = s_guest_textures.begin(); it != s_guest_textures.end();) {
		auto& e = it->second;
		if (e.used_flip == 0) e.used_flip = flip;
		if (!e.cs_written && flip > e.used_flip + s_after) {
			++n;
			bytes += e.tex != nil ? static_cast<size_t>(e.tex.allocatedSize) : 0;
			it = s_guest_textures.erase(it);
		} else {
			++it;
		}
	}
	static uint64_t s_total = 0;
	s_total += n;
	if (n != 0 && KYTY_ENV("KYTY_MEM_LOG") != nullptr) {
		std::fprintf(stderr, "[TexEvict] flip=%llu dropped %zu idle copies (%zu MB), %llu so far\n", (unsigned long long)flip, n, bytes >> 20,
		             (unsigned long long)s_total);
	}
}

// A decoded copy of guest texture bytes goes stale when the CPU writes new content to the same memory (texture streaming into a
// recycled pool block, the FC News promo images, the profile badge): the native Metal path does not page-protect guest memory
// (pageManager.cpp), so nothing else reports the write and the old copy (often garbage that was there first) stayed on screen for
// good. The source bytes are hashed when the copy is made and compared once per flip: the whole range up to 64 KiB, else four
// 512-byte windows spread over it (one guarded read each). KYTY_GUEST_TEX_NO_REFRESH=1 restores the old behaviour.
static uint64_t GuestTexSourceHash(uint64_t addr, uint64_t bytes) {
	constexpr uint64_t kWhole = 64u << 10, kWindow = 512, kWindows = 4;
	uint8_t  buf[kWindow];
	uint64_t h = 14695981039346656037ull;
	const auto mix = [&](const uint8_t* p, size_t n) {
		for (size_t k = 0; k + 8 <= n; k += 8) {
			uint64_t v;
			std::memcpy(&v, p + k, 8);
			h = (h ^ v) * 1099511628211ull;
		}
	};
	if (bytes <= kWhole) {
		std::vector<uint8_t> all(bytes);
		if (!Libs::LibKernel::Memory::TryReadBacking(addr, all.data(), bytes)) return 0;
		mix(all.data(), all.size());
		return h;
	}
	for (uint64_t i = 0; i < kWindows; ++i) {
		const uint64_t off = (bytes - kWindow) * i / (kWindows - 1);
		if (!Libs::LibKernel::Memory::TryReadBacking(addr + off, buf, kWindow)) return 0;
		mix(buf, kWindow);
	}
	return h;
}
// hash_before: the hash of the source taken BEFORE the bytes were read for the decode. Game threads stream into the same pool memory while the
// GPU thread decodes, and a hash taken afterwards described the new bytes while the copy held the old ones: no later check noticed it and the
// copy stayed wrong (KYTY_GUEST_TEX_VERIFY, cl629: all blocks differ, hash unchanged). KYTY_TEX_HASH_AFTER=1 restores the old order.
static void NoteGuestTexSource(uint64_t addr, uint64_t bytes, uint64_t hash_before = 0) {
	static const bool s_after = KYTY_ENV("KYTY_TEX_HASH_AFTER") != nullptr;
	const auto it = s_guest_textures.find(addr);
	if (it == s_guest_textures.end() || bytes == 0) return;
	it->second.src_bytes = bytes;
	it->second.src_hash = hash_before != 0 && !s_after ? hash_before : GuestTexSourceHash(addr, bytes);
	it->second.checked_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	it->second.decode_flip = it->second.checked_flip;
}
// A compute dispatch's read-only images are checked at every dispatch instead: the texture copy kernel (guest e5941d54, Metal
// 0xa3465640...) streams textures and UI images through one staging block that the CPU refills before each copy (six dispatches in a
// row copied 64x64 tiles of one 512x512 atlas from 0x81b4fc0000 and all got the first cached copy).
static bool s_guest_tex_check_each = false;
// False when the guest bytes behind a cached copy changed since it was decoded (checked once per flip, or per compute dispatch).
static bool GuestTexStillCurrent(CachedGuestTex& e, uint64_t addr) {
	static const bool s_off = false;
	if (s_off || e.cs_written || e.src_bytes == 0) return true;
	const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (e.checked_flip == flip && !s_guest_tex_check_each) return true;
	e.checked_flip = flip;
	const uint64_t h = GuestTexSourceHash(addr, e.src_bytes);
	if (h == 0 || h == e.src_hash) return true;
	static uint64_t s_window_flip = 0, s_window_count = 0, s_window_bytes = 0;
	++s_window_count;
	s_window_bytes += e.src_bytes;
	if (flip >= s_window_flip + 300) {
		std::fprintf(stderr, "[TexRefresh] flip=%llu: %llu copies (%llu MB of guest bytes) decoded again since flip %llu\n", static_cast<unsigned long long>(flip),
		             static_cast<unsigned long long>(s_window_count), static_cast<unsigned long long>(s_window_bytes >> 20), static_cast<unsigned long long>(s_window_flip));
		s_window_flip = flip;
		s_window_count = 0;
		s_window_bytes = 0;
	}
	static std::atomic<uint32_t> s_logged {0};
	if (s_logged.fetch_add(1) < 32) {
		std::fprintf(stderr, "[TexRefresh] flip=%llu addr=0x%llx %lux%lu f%lu bytes=%llu: guest bytes changed, decoding again\n",
		             static_cast<unsigned long long>(flip), static_cast<unsigned long long>(addr), static_cast<unsigned long>(e.tex.width),
		             static_cast<unsigned long>(e.tex.height), static_cast<unsigned long>(e.tex.pixelFormat), static_cast<unsigned long long>(e.src_bytes));
	}
	return false;
}
// Zero-content textures are re-detiled on every bind otherwise (no cache hit):
// a 2048x2048 detile per draw is what collapsed FPS to ~0.8. Refresh them only
// every kZeroTexRefreshBinds lookups instead.
static constexpr uint64_t kZeroTexRefreshBinds = 512;
// An empty copy whose guest bytes are hashed stays cached until the bytes change (GuestTexStillCurrent) or this many flips passed: the
// serial above counts every bind of every texture, so "512 binds" was less than a frame and each empty-looking texture was decoded again
static constexpr uint64_t kZeroTexRefreshFlips = 30;
static bool ZeroTexStillCached(const CachedGuestTex& e, uint64_t now, uint64_t flip) {
	static const bool s_old = false;
	if (e.had_nz || e.cs_written || now - e.serial < kZeroTexRefreshBinds) return true;
	return !s_old && e.src_bytes != 0 && flip < e.decode_flip + kZeroTexRefreshFlips;
}
static uint64_t s_guest_tex_serial = 0;
static void MarkGuestTexComputeWritten(uint64_t addr) {
	const auto it = s_guest_textures.find(addr);
	if (it != s_guest_textures.end()) it->second.cs_written = true;
}
static const auto StandardOffset = [](uint32_t x, uint32_t y, uint32_t element_bytes) {
	switch (element_bytes) {
		case 1: return ((y << 4) & 0x1f0u) ^ ((y << 5) & 0x400u) ^ (x & 0x00fu) ^
		                ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u);
		case 2: return ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^
		                ((x << 1) & 0x00eu) ^ ((x << 4) & 0x080u) ^
		                ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u);
		case 4: return ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^
		                ((x << 2) & 0x00cu) ^ ((x << 5) & 0x080u) ^
		                ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u);
		case 8: return ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^
		                ((x << 3) & 0x008u) ^ ((x << 5) & 0x0c0u) ^
		                ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u);
		default: return ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^
		                 ((x << 6) & 0x0c0u) ^ ((x << 7) & 0x200u) ^ ((x << 8) & 0x800u);
	}
};
static const auto Standard64Offset = [](uint32_t x, uint32_t y, uint32_t element_bytes) {
	uint32_t offset = 0;
	switch (element_bytes) {
		case 1: offset = 0; break;
		default: break;
	}
	// Base 2 KB swizzle first.
	switch (element_bytes) {
		case 1: offset = ((y << 4) & 0x1f0u) ^ ((y << 5) & 0x400u) ^ (x & 0x00fu) ^
		                 ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u); break;
		case 2: offset = ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^
		                 ((x << 1) & 0x00eu) ^ ((x << 4) & 0x080u) ^
		                 ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u); break;
		case 4: offset = ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^
		                 ((x << 2) & 0x00cu) ^ ((x << 5) & 0x080u) ^
		                 ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u); break;
		case 8: offset = ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^
		                 ((x << 3) & 0x008u) ^ ((x << 5) & 0x0c0u) ^
		                 ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u); break;
		default: offset = ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^
		                  ((x << 6) & 0x0c0u) ^ ((x << 7) & 0x200u) ^ ((x << 8) & 0x800u); break;
	}
	switch (element_bytes) {
		case 1: return offset ^ ((x << 7) & 0x2000u) ^ ((x << 8) & 0x8000u) ^
		                       ((y << 6) & 0x1000u) ^ ((y << 7) & 0x4000u);
		case 2: return offset ^ ((x << 7) & 0x2000u) ^ ((x << 8) & 0x8000u) ^
		                       ((y << 7) & 0x1000u) ^ ((y << 8) & 0x4000u);
		case 4: return offset ^ ((x << 8) & 0x2000u) ^ ((x << 9) & 0x8000u) ^
		                       ((y << 7) & 0x1000u) ^ ((y << 8) & 0x4000u);
		case 8: return offset ^ ((x << 8) & 0x2000u) ^ ((x << 9) & 0x8000u) ^
		                       ((y << 8) & 0x1000u) ^ ((y << 9) & 0x4000u);
		default: return offset ^ ((x << 9) & 0x2000u) ^ ((x << 10) & 0x8000u) ^
		                        ((y << 8) & 0x1000u) ^ ((y << 9) & 0x4000u);
	}
};

static auto DetileToLinear(const uint8_t* src, uint32_t width, uint32_t height,
                                Prospero::BufferFormat format, Prospero::TileMode tile,
                                std::vector<uint8_t>& out, uint32_t& pitch) -> bool {
	uint32_t element_bytes = Prospero::NumBytesPerElement(format);
	uint32_t block_w = 1;
	uint32_t block_h = 1;
	if (element_bytes == 0) {
		const uint32_t bc_bytes = Prospero::BlockCompressedBytesPerBlock(format);
		if (bc_bytes == 8 || bc_bytes == 16) {
			element_bytes = bc_bytes;
			block_w = 4;
			block_h = 4;
		} else {
			return false;
		}
	}
	const uint32_t w_elems = (width + block_w - 1) / block_w;
	const uint32_t h_elems = (height + block_h - 1) / block_h;
	pitch = w_elems * element_bytes;
	out.assign(static_cast<size_t>(pitch) * h_elems, 0);
	if (tile == Prospero::TileMode::kLinear) {
		std::memcpy(out.data(), src, std::min<size_t>(out.size(),
		                                             static_cast<size_t>(pitch) * h_elems));
		return true;
	}
	uint32_t block_bytes = 0;
	uint32_t block_width = 0;
	uint32_t block_height = 0;
	bool standard64 = false;
	switch (tile) {
		case Prospero::TileMode::kStandard4KB:
			block_bytes = 4096;
			block_width = element_bytes <= 2 ? 64u : (element_bytes <= 8 ? 32u : 16u);
			break;
		case Prospero::TileMode::kStandard64KB:
		case Prospero::TileMode::kPrt:
		case Prospero::TileMode::kDepth:
		case Prospero::TileMode::kRenderTarget:
			block_bytes  = 65536;
			block_width  = element_bytes <= 2 ? 256u : (element_bytes <= 8 ? 128u : 64u);
			standard64   = true;
			break;
		default: return false;
	}
	block_height = block_bytes / (block_width * element_bytes);
	if (block_height == 0) {
		return false;
	}
	const uint32_t blocks_per_row = (w_elems + block_width - 1) / block_width;
	for (uint32_t y = 0; y < h_elems; ++y) {
		for (uint32_t x = 0; x < w_elems; ++x) {
			const uint32_t bx = x / block_width;
			const uint32_t by = y / block_height;
			const uint32_t sx = x % block_width;
			const uint32_t sy = y % block_height;
			const uint32_t block_index = by * blocks_per_row + bx;
			const uint32_t offset = block_index * block_bytes +
			                        (standard64 ? Standard64Offset(sx, sy, element_bytes)
			                                    : StandardOffset(sx, sy, element_bytes));
			uint8_t* dst = out.data() + static_cast<size_t>(y) * pitch + x * element_bytes;
			std::memcpy(dst, src + offset, element_bytes);
		}
	}
	return true;
}
// The guest bytes DetileToLinear reads for that image (whole tiles), 0 when it cannot decode it.
static uint64_t DetileSourceBytes(uint32_t width, uint32_t height, Prospero::BufferFormat format, Prospero::TileMode tile) {
	uint32_t element_bytes = Prospero::NumBytesPerElement(format), block = 1;
	if (element_bytes == 0) {
		element_bytes = Prospero::BlockCompressedBytesPerBlock(format);
		block = 4;
		if (element_bytes != 8 && element_bytes != 16) return 0;
	}
	const uint64_t w_elems = (width + block - 1) / block, h_elems = (height + block - 1) / block;
	if (tile == Prospero::TileMode::kLinear) return w_elems * h_elems * element_bytes;
	const bool     small = tile == Prospero::TileMode::kStandard4KB;
	const uint64_t tile_bytes = small ? 4096u : 65536u;
	const uint64_t tile_w = small ? (element_bytes <= 2 ? 64u : element_bytes <= 8 ? 32u : 16u)
	                              : (element_bytes <= 2 ? 256u : element_bytes <= 8 ? 128u : 64u);
	const uint64_t tile_h = tile_bytes / (tile_w * element_bytes);
	if (tile_h == 0) return 0;
	return ((w_elems + tile_w - 1) / tile_w) * ((h_elems + tile_h - 1) / tile_h) * tile_bytes;
}


// Sizes of the long-lived Metal resource caches, appended to the KYTY_MEM_LOG line (finds which cache grows without bound). The
// command thread computes it (it owns most of these maps); the memory log only reads the text.
static std::mutex  s_cache_report_mutex;
static std::string s_cache_report;
static void UpdateCacheReport() {
	static const bool s_on = KYTY_ENV("KYTY_MEM_LOG") != nullptr;
	if (!s_on) return;
	static std::chrono::steady_clock::time_point s_last {};
	const auto now = std::chrono::steady_clock::now();
	if (now - s_last < std::chrono::seconds(3)) return;
	s_last = now;
	char text[512];
	const auto sum_tex = [](id<MTLTexture> t) -> size_t { return t != nil ? static_cast<size_t>(t.allocatedSize) : 0; };
	size_t gt_n = 0, gt_b = 0, rt_n = 0, rt_b = 0, cs_n = 0, cs_b = 0, al_n = 0, al_b = 0;
	for (const auto& [a, e]: s_guest_textures) { ++gt_n; gt_b += sum_tex(e.tex); }
	{
		std::lock_guard<std::mutex> lock(s_render_target_mutex);
		for (const auto& [a, by_format]: s_rt_by_addr) for (const auto& [f, rt]: by_format) { ++rt_n; rt_b += sum_tex(rt.tex); }
	}
	{
		std::lock_guard<std::mutex> lock(s_cs_storage_mutex);
		for (const auto& [a, e]: s_cs_storage) { ++cs_n; cs_b += sum_tex(e.tex); }
	}
	{
		std::lock_guard<std::mutex> lock(s_aliased_mutex);
		for (const auto& [k, e]: s_aliased_images) { ++al_n; al_b += sum_tex(e.tex); }
	}
	size_t   pc_n = 0;
	uint64_t pc_b = 0;
	MetalBufferManager::Instance().CacheStats(&pc_n, &pc_b);
	std::snprintf(text, sizeof(text), " | guest_tex n=%zu %zuMB | rt n=%zu %zuMB | cs n=%zu %zuMB | aliased n=%zu %zuMB | page_cache n=%zu %lluMB", gt_n, gt_b >> 20, rt_n, rt_b >> 20, cs_n, cs_b >> 20, al_n, al_b >> 20, pc_n, static_cast<unsigned long long>(pc_b >> 20));
	std::string top;
	{
		std::lock_guard<std::mutex> alock(s_alloc_mutex);
		std::vector<std::pair<uint64_t, std::tuple<int, uint64_t, uint64_t>>> order;
		for (const auto& [k, v]: s_alloc_by_site) order.push_back({v, k});
		std::sort(order.begin(), order.end(), [](const auto& l, const auto& r2) { return l.first > r2.first; });
		for (size_t i = 0; i < order.size() && i < 6; ++i) {
			char t[160];
			std::snprintf(t, sizeof(t), " [%s %016llx/%016llx +%lluMB]", std::get<0>(order[i].second) == 2 ? "CS" : "DRAW", static_cast<unsigned long long>(std::get<1>(order[i].second)),
			              static_cast<unsigned long long>(std::get<2>(order[i].second)), static_cast<unsigned long long>(order[i].first >> 20));
			top += t;
		}
	}
	std::string lines;
	{
		std::lock_guard<std::mutex> nlock(s_newres_mutex);
		std::vector<std::pair<uint64_t, int>> order;
		for (const auto& [ln, e]: s_newres_by_line) order.push_back({e.second, ln});
		std::sort(order.begin(), order.end(), [](const auto& l, const auto& r2) { return l.first > r2.first; });
		for (size_t i = 0; i < order.size() && i < 10; ++i) {
			char t[96];
			const auto& e = s_newres_by_line[order[i].second];
			std::snprintf(t, sizeof(t), " L%d:%llux/%lluMB", order[i].second, static_cast<unsigned long long>(e.first), static_cast<unsigned long long>(e.second >> 20));
			lines += t;
		}
	}
	std::lock_guard<std::mutex> lock(s_cache_report_mutex);
	s_cache_report = std::string(text) + " | top allocators:" + top + " | by line:" + lines;
}
std::string MetalCacheReport() {
	std::lock_guard<std::mutex> lock(s_cache_report_mutex);
	return s_cache_report;
}

static id<MTLTexture> GuestTexture(uint64_t addr, uint32_t width, uint32_t height,
                                   Prospero::BufferFormat format,
                                   Prospero::TileMode tile, uint32_t levels, uint32_t physical_levels, uint32_t first_level) {
	// Opt-in, bounded CPU-side evidence for small UI image uploads. Do not change
	// texture/cache contents: compare the guest bytes with the trace's blank icon.
	if (const char* dump_dir = KYTY_ENV("KYTY_SMALL_TEX_DIR");
	    dump_dir != nullptr && addr != 0 && height == 32 && (width == 32 || width == 40)) {
		static std::map<uint64_t, uint64_t> seen_flip;
		static std::map<uint64_t, uint64_t> seen_hash;
		static uint32_t snapshots = 0;
		const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
		if ((!seen_flip.contains(addr) || flip >= seen_flip[addr] + 100) && snapshots < 64) {
			seen_flip[addr] = flip;
			TileSurfaceLayout layout {};
			const TileSurfaceDescription description {format, tile, TileSurfaceDimension::Dim2D,
			                                          width, height, 1, 1, 1};
			const bool tiled = tile != Prospero::TileMode::kLinear && TileGetTiledTextureLayout(description, layout);
			const size_t bytes = tiled ? layout.total_size :
			    static_cast<size_t>(TileGetTexturePitch(format, width, tile)) * height *
			    std::max(Prospero::NumBytesPerElement(format), 1u);
			std::vector<uint8_t> raw(bytes);
			if (bytes > 0 && bytes <= (1u << 20) && Libs::LibKernel::Memory::TryReadBacking(addr, raw.data(), bytes)) {
				uint64_t hash = 14695981039346656037ull;
				size_t nz = 0;
				for (uint8_t b : raw) { hash = (hash ^ b) * 1099511628211ull; nz += b != 0; }
				const auto cached = s_guest_textures.find(addr);
				std::fprintf(stderr, "[SmallTex] flip=%llu addr=0x%llx %ux%u fmt=%u tile=%u bytes=%zu nz=%zu hash=%llx tail=%u off0=%llu cached=%p had_nz=%d cs=%d\n",
				             (unsigned long long)flip, (unsigned long long)addr, width, height,
				             (unsigned)format, (unsigned)tile, bytes, nz, (unsigned long long)hash,
				             layout.first_tail_level, (unsigned long long)layout.mips[0].offset,
				             cached == s_guest_textures.end() ? nullptr : (__bridge void*)cached->second.tex,
				             cached == s_guest_textures.end() ? 0 : cached->second.had_nz,
				             cached == s_guest_textures.end() ? 0 : cached->second.cs_written);
				if (!seen_hash.contains(addr) || seen_hash[addr] != hash) {
					seen_hash[addr] = hash;
					++snapshots;
					char path[1024];
					std::snprintf(path, sizeof(path), "%s/tex-%llx-%ux%u-f%u-t%u-%llx.bin", dump_dir,
					              (unsigned long long)addr, width, height, (unsigned)format, (unsigned)tile,
					              (unsigned long long)hash);
					if (FILE* f = std::fopen(path, "wb")) { std::fwrite(raw.data(), 1, raw.size(), f); std::fclose(f); }
				}
			}
		}
	}
	// KYTY_SCAN_TEX=1: for flips 1..300, log the non-zero ratio of the guest bytes behind each sampled texture
	// (once per flip and address). Diagnostic only: finds the first draw that samples real content.
	if (KYTY_ENV("KYTY_SCAN_TEX") != nullptr && addr != 0 && width > 0 && height > 0) {
		const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
		if (flip >= 1 && flip <= 300) {
			static std::mutex s_scan_mutex;
			static std::set<std::pair<uint64_t, uint64_t>> s_scan_seen;
			std::lock_guard<std::mutex> lock(s_scan_mutex);
			if (s_scan_seen.insert({flip, addr}).second) {
				MTLPixelFormat sfmt = MetalFormatFromBufferFormat(format);
				size_t sbpp = sfmt == MTLPixelFormatInvalid ? 4 : BytesPerPixel(sfmt);
				if (sbpp == 0) sbpp = 4;
				const uint64_t sbytes = std::min<uint64_t>(static_cast<uint64_t>(width) * height * sbpp, 4ull << 20);
				std::vector<uint8_t> sbuf(sbytes);
				if (Libs::LibKernel::Memory::TryReadBacking(addr, sbuf.data(), sbytes)) {
					uint64_t snz = 0;
					for (uint8_t v: sbuf) snz += v != 0;
					std::fprintf(stderr, "[Scan] flip=%llu addr=0x%llx %ux%u fmt=%u tile=%u nz=%.3f%%\n",
					             (unsigned long long)flip, (unsigned long long)addr, width, height,
					             (unsigned)format, (unsigned)tile, 100.0 * (double)snz / (double)sbytes);
				}
			}
		}
	}
	if (addr == 0 || width == 0 || height == 0 || width > 8192 || height > 8192) {
		return nil;
	}
	TraceFlipOverlap("bind_tex", addr, static_cast<uint64_t>(width) * height * 4);
	TraceTexBacking(addr, static_cast<uint64_t>(width) * height * 4);
	bool stale_chain = false, other_image = false;
	{
		std::lock_guard<std::mutex> lock(s_render_target_mutex);
		auto rt_it = s_rt_by_addr.find(addr);
		if (rt_it != s_rt_by_addr.end() && !rt_it->second.empty()) {
			// The game reuses one address for unrelated render targets (RG11B10 colour, then an RG8 buffer, ...). Take the newest
			// target that is the same surface as the descriptor: same Metal format and size, then same texel size and size.
			// A target of another texel size is different data, never a match.
			const MTLPixelFormat want_rt = MetalFormatFromBufferFormat(format);
			const size_t want_bpp = want_rt == MTLPixelFormatInvalid ? 0 : BytesPerPixel(want_rt);
			const TrackedRt* best = nullptr;
			int best_score = 0;
			for (const auto& [f, rt]: rt_it->second) {
				if (!rt.tex) continue;
				const bool dims = rt.tex.width == width && rt.tex.height == height;
				int score = 0;
				if (dims && rt.tex.pixelFormat == want_rt) score = 4;
				else if (dims && want_bpp != 0 && BytesPerPixel(rt.tex.pixelFormat) == want_bpp) score = 3;
				// Another size counts for a padded view of nearly the same surface (dynamic resolution) or a target rendered just now (its mip levels). An
				// older one is a different image: a full-screen menu image (T# 3840x1080) got a 2048x2048 kit target from 842 flips earlier, as rows of noise
				// over the whole screen. KYTY_RT_ANY_SIZE=1 restores it.
				else if (want_bpp != 0 && BytesPerPixel(rt.tex.pixelFormat) == want_bpp) {
					static const bool s_any_size = false;
					const auto near = [](uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) * 4u <= b; };
					const uint64_t lookup_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
					if (s_any_size || lookup_flip <= rt.flip + 2 || (near(static_cast<uint32_t>(rt.tex.width), width) && near(static_cast<uint32_t>(rt.tex.height), height))) {
						score = 2;
					} else {
						other_image = true;
						// Nobody wrote these bytes since that chain rendered (it does not use them): the old bytes are no image. The Vulkan reference
						// starts such an image empty and uploads guest memory only for CPU-written pages.
						stale_chain = stale_chain || (rt.guest_bytes != 0 && GuestTexSourceHash(addr, rt.guest_bytes) == rt.guest_hash);
					}
				}
				if (score > best_score || (score == best_score && score > 0 && best != nullptr && rt.sequence > best->sequence)) {
					best = &rt;
					best_score = score;
				}
			}
			if (best != nullptr) {
				return ReinterpretFormat(best->tex, want_rt);
			}
			// The address held a target long ago and holds a tiled image now. Left marked, the image was read as linear texels: its tiles became
			// horizontal bands over the whole menu (the 3840x1080 panel image, transparent in the middle).
			if (other_image) s_render_target_addresses.erase(addr);
			if (!stale_chain) {
				std::fprintf(stderr,"[RtAliasReject] addr=%llx reader=%ux%u format=%u tile=%u levels=%u physical=%u first=%u flip=%llu\n",(unsigned long long)addr,width,height,unsigned(format),unsigned(tile),levels,physical_levels,first_level,(unsigned long long)Libs::Graphics::TraceWindow::g_flip.load());
				for (const auto& [f,rt]:rt_it->second) std::fprintf(stderr,"[RtAliasReject] writer=%lux%lu fmt=%u levels=%lu flip=%llu seq=%llu guest_bytes=%llu old_hash=%llx current_hash=%llx\n",(unsigned long)rt.tex.width,(unsigned long)rt.tex.height,f,(unsigned long)rt.tex.mipmapLevelCount,(unsigned long long)rt.flip,(unsigned long long)rt.sequence,(unsigned long long)rt.guest_bytes,(unsigned long long)rt.guest_hash,(unsigned long long)(rt.guest_bytes?GuestTexSourceHash(addr,rt.guest_bytes):0));
				TileSurfaceLayout reader_layout{};
				const bool layout_ok=TileGetTiledTextureLayout({format,tile,TileSurfaceDimension::Dim2D,width,height,1,physical_levels,1},reader_layout);
				std::vector<PhysicalSpan> reader_spans;
				const bool mapped=layout_ok && CanonicalMipSpans(addr,reader_layout.total_size,reader_spans);
				std::fprintf(stderr,"[RtAliasReject] layout=%d bytes=%llu mapped=%d spans=%zu canonical_overlap=%d canonical_ranges=%zu\n",
				    layout_ok,(unsigned long long)reader_layout.total_size,mapped,reader_spans.size(),
				    layout_ok && CanonicalMipOverlap(addr,reader_layout.total_size),s_canonical_mip_ranges.size());
				for(const auto&m:s_canonical_mappings)if(m.address<addr+reader_layout.total_size && addr<m.address+m.size)
					std::fprintf(stderr,"[RtAliasReject] mapping VA=%llx bytes=%llu physical=%llx\n",(unsigned long long)m.address,(unsigned long long)m.size,(unsigned long long)m.backing_offset);
				for(const auto& [f,rt]:rt_it->second) {
					std::fprintf(stderr,"[RtAliasReject] canonical=%d valid=%d matches=%d size=%llu serial=%llu\n",rt.canonical,rt.physical.Valid(),rt.physical.Matches(s_canonical_mappings,addr),(unsigned long long)rt.physical.size,(unsigned long long)rt.writer_serial);
					for(const auto&s:rt.physical.spans)std::fprintf(stderr,"[RtAliasReject] old_span offset=%llx physical=%llx bytes=%llu\n",(unsigned long long)s.offset,(unsigned long long)s.physical,(unsigned long long)s.size);
				}
				FailMetalMaterialization("sampled address has only render targets of another texel size", static_cast<uint32_t>(format),
				             static_cast<uint32_t>(want_rt), static_cast<uint32_t>(rt_it->second.size()));
			}
		}
	}
	// KYTY_LOG_RT_LOOKUP=<w>x<h>: what the lookup found for descriptors of that size (first 40).
	if (const char* lk = KYTY_ENV("KYTY_LOG_RT_LOOKUP")) {
		unsigned lw = 0, lh = 0;
		static std::atomic<uint32_t> s_lk {0};
		if (std::sscanf(lk, "%ux%u", &lw, &lh) == 2 && lw == width && lh == height && s_lk.fetch_add(1) < 40) {
			std::lock_guard<std::mutex> lock(s_render_target_mutex);
			std::string text;
			if (const auto r = s_rt_by_addr.find(addr); r != s_rt_by_addr.end()) {
				for (const auto& [f, rt]: r->second) {
					char t[160];
					std::snprintf(t, sizeof(t), " rt[%lux%lu f%u flip=%llu bytes=%llu same=%d]", (unsigned long)rt.tex.width, (unsigned long)rt.tex.height, f, (unsigned long long)rt.flip,
					              (unsigned long long)rt.guest_bytes, rt.guest_bytes != 0 && GuestTexSourceHash(addr, rt.guest_bytes) == rt.guest_hash ? 1 : 0);
					text += t;
				}
			}
			uint64_t wf = 0;
			id<MTLTexture> lw_tex = LastGpuWriterTexture(addr, &wf);
			std::fprintf(stderr, "[RtLookup] flip=%llu addr=0x%llx %ux%u tile=%u levels=%u stale=%d writer=%lux%lu flip=%llu%s\n", (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(),
			             (unsigned long long)addr, width, height, (unsigned)tile, levels, stale_chain ? 1 : 0, lw_tex != nil ? (unsigned long)lw_tex.width : 0ul, lw_tex != nil ? (unsigned long)lw_tex.height : 0ul,
			             (unsigned long long)wf, text.c_str());
		}
	}
	if (stale_chain && physical_levels <= 1) {
		const MTLPixelFormat empty_fmt = MetalFormatFromBufferFormat(format);
		if (empty_fmt != MTLPixelFormatInvalid && !IsCompressedFormat(empty_fmt)) return AliasedImageFor(addr, width, height, empty_fmt, 1u);
	}
	const uint64_t now = ++s_guest_tex_serial;
	const uint64_t bind_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	EvictIdleGuestTextures(bind_flip);
	const auto it = s_guest_textures.find(addr);
	if (it != s_guest_textures.end()) it->second.used_flip = bind_flip;
	MTLPixelFormat want = MetalFormatFromBufferFormat(format);
	if (want == MTLPixelFormatInvalid) {
		FailMetalMaterialization("guest texture cache lookup", static_cast<uint32_t>(format));
	}
	const uint32_t block = IsCompressedFormat(want) ? 4u : 1u;
	const bool upload_mips = physical_levels > 1 && tile != Prospero::TileMode::kLinear && !IsLinearRenderTargetAddress(addr);
	const uint32_t expected_w = upload_mips ? std::max(width >> first_level,1u) : (width + block - 1u) & ~(block - 1u);
	const uint32_t expected_h = upload_mips ? std::max(height >> first_level,1u) : (height + block - 1u) & ~(block - 1u);
	// Transient guest allocations are reused for unrelated images. Equal byte
	// counts do not make RGBA8 and BC1 compatible texture views.
	const bool cache_layout_matches = it != s_guest_textures.end() && it->second.tile == tile &&
	    it->second.tex.width == expected_w && it->second.tex.height == expected_h &&
	    it->second.tex.mipmapLevelCount == (upload_mips ? levels : 1u) &&
	    (!upload_mips || (it->second.physical_levels == physical_levels && it->second.first_level == first_level)) &&
	    (it->second.tex.pixelFormat == want ||
	     (!IsCompressedFormat(want) && !IsCompressedFormat(it->second.tex.pixelFormat) &&
	      BytesPerPixel(want) == BytesPerPixel(it->second.tex.pixelFormat)));
	bool cache_backing_matches = true;
	if (cache_layout_matches && it->second.tex.buffer != nil) {
		// Revalidate live linear views on remap, just like RT/compute views.
		// Dimensions and format can stay unchanged while the physical page changes.
		const NSUInteger row = it->second.tex.bufferBytesPerRow;
		const auto allocation = MetalBufferManager::Instance().GetBufferForGuestRange(addr,row*expected_h);
		cache_backing_matches = GuestMemoryViewMatches(it->second.tex,allocation,it->second.tex.pixelFormat,expected_w,expected_h,row);
		if (!cache_backing_matches) ForgetGpuWriter(addr,it->second.tex);
	}
	if (cache_layout_matches && cache_backing_matches &&
	    ZeroTexStillCached(it->second, now, bind_flip) &&
	    GuestTexStillCurrent(it->second, addr)) {
		return ReinterpretFormat(it->second.tex, want);
	}
	// KYTY_LOG_GUEST_DECODES=1: why a guest texture is decoded again, by reason (miss: nothing cached, layout: cached copy has other
	// size/levels/tile, zero: cached copy was empty, stale: guest bytes changed), with the worst addresses, every 300 flips.
	if (static const bool s_log_decodes = KYTY_ENV("KYTY_LOG_GUEST_DECODES") != nullptr; s_log_decodes) {
		static uint64_t s_reason[4] = {}, s_bytes[4] = {}, s_report_flip = 0;
		static std::map<uint64_t, std::pair<uint32_t, std::string>> s_by_addr;
		const int reason = it == s_guest_textures.end() ? 0 : !cache_layout_matches ? 1 : !ZeroTexStillCached(it->second, now, bind_flip) ? 2 : 3;
		++s_reason[reason];
		s_bytes[reason] += static_cast<uint64_t>(width) * height * (block == 4u ? 1u : BytesPerPixel(want));
		auto& e = s_by_addr[addr];
		++e.first;
		if (reason == 1 && e.second.size() < 160) {
			char t[96];
			std::snprintf(t, sizeof(t), " [%ux%u L%u -> %ux%u L%u]", static_cast<unsigned>(it->second.tex.width), static_cast<unsigned>(it->second.tex.height),
			              static_cast<unsigned>(it->second.tex.mipmapLevelCount), width, height, levels);
			e.second += t;
		}
		if (bind_flip >= s_report_flip + 300) {
			s_report_flip = bind_flip;
			std::fprintf(stderr, "[GuestDecodes] flip=%llu miss=%llu layout=%llu zero=%llu stale=%llu (px bytes %llu/%llu/%llu/%llu)\n", static_cast<unsigned long long>(bind_flip),
			             static_cast<unsigned long long>(s_reason[0]), static_cast<unsigned long long>(s_reason[1]), static_cast<unsigned long long>(s_reason[2]),
			             static_cast<unsigned long long>(s_reason[3]), static_cast<unsigned long long>(s_bytes[0]), static_cast<unsigned long long>(s_bytes[1]),
			             static_cast<unsigned long long>(s_bytes[2]), static_cast<unsigned long long>(s_bytes[3]));
			std::vector<std::pair<uint32_t, uint64_t>> worst;
			for (const auto& [a, v]: s_by_addr) worst.push_back({v.first, a});
			std::sort(worst.rbegin(), worst.rend());
			for (size_t i = 0; i < worst.size() && i < 6 && worst[i].first >= 4; ++i)
				std::fprintf(stderr, "[GuestDecodes]   addr=0x%llx decodes=%u%s\n", static_cast<unsigned long long>(worst[i].second), worst[i].first, s_by_addr[worst[i].second].second.c_str());
		}
	}
	if (upload_mips) {
		// Unlike a single-level image, a small mip chain may place even level 0
		// inside a shared mip tail. Decode the full descriptor layout, not (0,0)
		// in the first tile (that read blank pixels for the 32x32 button icon).
		TileSurfaceLayout layout {};
		const TileSurfaceDescription desc {format, tile, TileSurfaceDimension::Dim2D,
		                                   width, height, 1, physical_levels, 1};
		if (first_level + levels > physical_levels || levels > std::bit_width(std::max(expected_w, expected_h)) ||
		    !TileGetTiledTextureLayout(desc, layout) || layout.total_size > (256ull << 20)) return nil;
		const uint64_t src_hash_before = GuestTexSourceHash(addr, layout.total_size);
		std::vector<uint8_t> source(layout.total_size);
		if (!LibKernel::Memory::TryReadBacking(addr, source.data(), source.size())) return nil;
		MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:want
		                                                                            width:expected_w height:expected_h mipmapped:levels>1];
		td.mipmapLevelCount = levels;
		td.usage = GuestTextureUsage(want);
		td.storageMode = MTLStorageModeShared;
		td.allowGPUOptimizedContents = NO;
		id<MTLTexture> texture = KytyNewTexture(MetalContext::Instance().GetDevice(), td, __LINE__);
		if (texture == nil) return nil;
		bool has_nz = false;
		for (uint32_t level = 0; level < levels; ++level) {
			const uint32_t physical_level = first_level + level;
			std::vector<uint8_t> linear;
			uint32_t pitch = 0;
			if (!DecodeTiledTextureLevel(source, layout, physical_level, 0, linear, pitch)) return nil;
			if (const uint32_t bc = Prospero::BlockCompressedBytesPerBlock(format); bc != 0) BcOverlayLevel(addr, physical_level, bc, linear, pitch, tile);
			if (level == 0) has_nz = std::any_of(linear.begin(), linear.end(), [](uint8_t b) { return b != 0; });
			[texture replaceRegion:MTLRegionMake2D(0, 0, std::max(width >> physical_level, 1u), std::max(height >> physical_level, 1u))
			           mipmapLevel:level withBytes:linear.data() bytesPerRow:pitch];
			// KYTY_BC_DUMP_DIR=<dir>: the blocks of the first levels of a GPU-encoded BC texture, as each decode of it saw them.
			static const char* s_bc_dump_dir = KYTY_ENV("KYTY_BC_DUMP_DIR");
			if (s_bc_dump_dir != nullptr && level < 4 && IsCompressedFormat(want) && width >= 64 && s_bc_written_addrs.contains(addr)) {
				static std::map<uint64_t, uint32_t> s_decodes;
				if (level == 0) ++s_decodes[addr];
				if (s_decodes[addr] <= 4) {
					char path[1024];
					std::snprintf(path, sizeof(path), "%s/%llx-n%u-%ux%u-f%lu-L%uof%u-off%llu-p%u-flip%llu.bin", s_bc_dump_dir, (unsigned long long)addr,
					              s_decodes[addr], width, height, (unsigned long)want, level, levels, (unsigned long long)layout.mips[level].offset, pitch,
					              (unsigned long long)bind_flip);
					if (FILE* f = std::fopen(path, "wb")) { std::fwrite(linear.data(), 1, linear.size(), f); std::fclose(f); }
				}
			}
		}
		s_guest_textures[addr] = {texture, has_nz, now, false, tile};
		s_guest_textures[addr].physical_levels = physical_levels;
		s_guest_textures[addr].first_level = first_level;
		NoteGuestTexSource(addr, layout.total_size, src_hash_before);
		static std::atomic<uint32_t> logged {0};
		static std::atomic<uint32_t> tail_logged {0};
		if (logged.fetch_add(1) < 24 || (first_level != 0 && tail_logged.fetch_add(1) < 32) || (height == 32 && (width == 32 || width == 40)))
			std::fprintf(stderr, "[TexMip] addr=0x%llx %ux%u fmt=%u tile=%u levels=%u physical=%u first=%u tail=%u xy=%u,%u nz=%d\n",
			             (unsigned long long)addr, width, height, (unsigned)format, (unsigned)tile,
			             levels, physical_levels, first_level, layout.first_tail_level, layout.mips[0].tail_x, layout.mips[0].tail_y, has_nz);
		return texture;
	}
	if (tile != Prospero::TileMode::kLinear && !IsLinearRenderTargetAddress(addr)) {
		std::vector<uint8_t> linear;
		uint32_t             pitch = 0;
		const uint64_t       detile_bytes = DetileSourceBytes(width, height, format, tile);
		const uint64_t       src_hash_before = detile_bytes != 0 ? GuestTexSourceHash(addr, detile_bytes) : 0;
		if (DetileToLinear(reinterpret_cast<const uint8_t*>(addr), width, height, format,
		                   tile, linear, pitch)) {
			if (const uint32_t bc = Prospero::BlockCompressedBytesPerBlock(format); bc != 0) BcOverlayLevel(addr, 0, bc, linear, pitch, tile);
			// KYTY_LOG_RT_LOOKUP=<w>x<h> with KYTY_RT_LOOKUP_DUMP=<dir>: the detiled texels and the raw guest bytes of such a descriptor.
			if (const char* dd = KYTY_ENV("KYTY_RT_LOOKUP_DUMP"); dd != nullptr && KYTY_ENV("KYTY_LOG_RT_LOOKUP") != nullptr) {
				unsigned lw = 0, lh = 0;
				static std::atomic<uint32_t> s_dd {0};
				if (std::sscanf(KYTY_ENV("KYTY_LOG_RT_LOOKUP"), "%ux%u", &lw, &lh) == 2 && lw == width && lh == height && s_dd.fetch_add(1) < 12) {
					char path[1024];
					std::snprintf(path, sizeof(path), "%s/%llx-flip%llu-%ux%u-p%u.bin", dd, (unsigned long long)addr, (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(), width, height, pitch);
					if (FILE* f = std::fopen(path, "wb")) { std::fwrite(linear.data(), 1, linear.size(), f); std::fclose(f); }
					std::fprintf(stderr, "[RtLookupDump] %s tile=%u fmt=%u bytes=%zu\n", path, (unsigned)tile, (unsigned)format, linear.size());
				}
			}
			MTLPixelFormat mtl = MetalFormatFromBufferFormat(format);
			if (mtl == MTLPixelFormatInvalid) {
				FailMetalMaterialization("detiled guest texture", static_cast<uint32_t>(format));
			}
			uint32_t block_w = 1;
			uint32_t block_h = 1;
			if (Prospero::BlockCompressedBytesPerBlock(format) != 0) {
				block_w = 4;
				block_h = 4;
			}
			uint32_t alloc_w = (width + block_w - 1) & ~(block_w - 1);
			uint32_t alloc_h = (height + block_h - 1) & ~(block_h - 1);
			if (alloc_w == 0) alloc_w = block_w;
			if (alloc_h == 0) alloc_h = block_h;

			MTLTextureDescriptor* td =
			    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl
			                                                       width:alloc_w
			                                                      height:alloc_h
			                                                   mipmapped:NO];
			td.usage       = GuestTextureUsage(td.pixelFormat);
			td.storageMode = MTLStorageModeShared;
			td.allowGPUOptimizedContents = NO;
			id<MTLTexture> tex =
			    KytyNewTexture(MetalContext::Instance().GetDevice(), td, __LINE__);
			if (tex) {
				[tex replaceRegion:MTLRegionMake2D(0, 0, alloc_w, alloc_h)
				       mipmapLevel:0
				         withBytes:linear.data()
				       bytesPerRow:pitch];
				bool has_nz = false;
				// The whole copy: a texture with a blank first 4 KB (a transparent corner, a black border) was taken for an empty one.
				static const bool s_first_page_only = false;
				const size_t scan = s_first_page_only ? std::min<size_t>(linear.size(), 4096) : linear.size();
				for (size_t i = 0; i < scan; ++i) {
					if (linear[i] != 0) {
						has_nz = true;
						break;
					}
				}
				s_guest_textures[addr] = {tex, has_nz, now, false, tile};
				NoteGuestTexSource(addr, detile_bytes, src_hash_before);
				static std::atomic<uint32_t> s_detile_log {0};
				if (s_detile_log.fetch_add(1) < 16) {
					std::fprintf(stderr, "[Detile] addr=0x%llx %ux%u (alloc %ux%u) fmt=%u mtl=%u tile=%u pitch=%u nz=%d\n",
					             static_cast<unsigned long long>(addr), width, height,
					             alloc_w, alloc_h,
					             static_cast<uint32_t>(format), static_cast<uint32_t>(mtl),
					             static_cast<uint32_t>(tile), pitch, has_nz ? 1 : 0);
				}
				return tex;
			}
		}
	}
	// Metal aborts the process when a texture view fails stride validation
	// (_mtlValidateStrideTextureParameters), so keep the known-good linear view
	// layout for now: RGBA8 with width*4 pitch. The format/tile fields are logged
	// so a validated per-format path can be added later.
	(void)tile;
	MTLPixelFormat mtl_format = MetalFormatFromBufferFormat(format);
	if (mtl_format == MTLPixelFormatInvalid) {
		FailMetalMaterialization("linear guest texture", static_cast<uint32_t>(format));
	}
	id<MTLDevice> device = MetalContext::Instance().GetDevice();
	const uint32_t bc_bytes = Prospero::BlockCompressedBytesPerBlock(format);
	if (bc_bytes != 0 || IsCompressedFormat(mtl_format)) {
		const uint32_t block_bytes = bc_bytes != 0 ? bc_bytes : 16u;
		uint32_t alloc_w = (width + 3u) & ~3u;
		uint32_t alloc_h = (height + 3u) & ~3u;
		if (alloc_w == 0) alloc_w = 4;
		if (alloc_h == 0) alloc_h = 4;
		const size_t pitch = (static_cast<size_t>(alloc_w) / 4u) * block_bytes;
		const size_t total_bytes = pitch * (alloc_h / 4u);
		MetalBuffer buffer;
		if (!buffer.WrapHostMemory(reinterpret_cast<void*>(addr), total_bytes, "GuestTexBC")) {
			static std::atomic<uint32_t> s_wrap_fail {0};
			if (s_wrap_fail.fetch_add(1) < 5) {
				std::fprintf(stderr, "[TexData] wrap FAILED addr=0x%llx %ux%u bc\n",
				             static_cast<unsigned long long>(addr), width, height);
			}
			return nil;
		}
		MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_format
		                                                                              width:alloc_w
		                                                                             height:alloc_h
		                                                                          mipmapped:NO];
		td.usage = GuestTextureUsage(td.pixelFormat);
		td.storageMode = MTLStorageModeShared;
		td.allowGPUOptimizedContents = NO;
		id<MTLTexture> texture = device ? KytyNewTexture(device, td, __LINE__) : nil;
		if (texture) {
			[texture replaceRegion:MTLRegionMake2D(0, 0, alloc_w, alloc_h)
			           mipmapLevel:0
			             withBytes:reinterpret_cast<const void*>(addr)
			           bytesPerRow:pitch];
		}
		if (texture) {
			s_guest_textures[addr] = {texture, true, now, false, tile};
			NoteGuestTexSource(addr, total_bytes);
		}
		return texture;
	}
	uint32_t bpp = BytesPerPixel(mtl_format);
	if (bpp == 0) bpp = 4;
	const size_t row_bytes = static_cast<size_t>(width) * bpp;
	// Linear surfaces carry no pitch in the T#: rows are padded to the hardware's linear alignment (R8 1920 wide -> 2048),
	// as TileGetTexturePitch computes for the Vulkan path. Reading with a tight pitch shears video planes into stripes.
	size_t row_stride = row_bytes;
	if (tile == Prospero::TileMode::kLinear) {
		const uint32_t pitch_texels = TileGetTexturePitch(format, width, tile);
		if (pitch_texels > width) row_stride = static_cast<size_t>(pitch_texels) * bpp;
	}
	const size_t total_bytes = row_stride * (height - 1) + row_bytes;
	const auto allocation = MetalBufferManager::Instance().GetBufferForGuestRange(addr,row_stride*height);
	if (!allocation.IsValid()) {
		// The encoded address/extent does not resolve to a mapped guest range.
		// Do not read it: past the mapping this faults the host process.
		static std::atomic<uint32_t> s_wrap_fail {0};
		if (s_wrap_fail.fetch_add(1) < 5) {
			std::fprintf(stderr, "[TexData] wrap FAILED addr=0x%llx %ux%u\n",
			             static_cast<unsigned long long>(addr), width, height);
		}
		return nil;
	}
	MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_format
	                                                                              width:width
	                                                                             height:height
	                                                                          mipmapped:NO];
	td.usage       = GuestTextureUsage(td.pixelFormat);
	td.storageMode = MTLStorageModeShared;
	{
		static std::atomic<uint32_t> s_view_log {0};
		if (s_view_log.fetch_add(1) < 12) {
			std::fprintf(stderr,
			             "[TexView] addr=0x%llx %ux%u fmt=%u row=%zu total=%zu m_off=%llu len=%zu\n",
			             static_cast<unsigned long long>(addr), width, height, (uint32_t)mtl_format, row_bytes,
			             total_bytes, static_cast<unsigned long long>(allocation.offset),
			             static_cast<size_t>(allocation.size));
		}
	}
	NSUInteger align = LinearTextureRowAlignment(device, mtl_format);
	if (align == 0) {
		align = 256;
	}
	if (align < 64) align = 64;
	id<MTLTexture> texture = nil;
	const bool     copied = row_bytes < 64 || (row_stride % align) != 0;
	if (copied) {
		td.usage       = GuestTextureUsage(td.pixelFormat);
		td.storageMode = MTLStorageModeShared;
		texture = KytyNewTexture(device, td, __LINE__);
		if (texture) {
			[texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
			           mipmapLevel:0
			             withBytes:reinterpret_cast<const void*>(addr)
			           bytesPerRow:row_stride];
		}
	} else {
		texture = CreateGuestMemoryView(allocation,td,row_stride);
	}
	if (texture) {
		s_guest_textures[addr] = {texture, true, now, false, tile};
		if (copied) NoteGuestTexSource(addr, total_bytes);
	} else {
		static std::atomic<uint32_t> s_view_fail {0};
		if (s_view_fail.fetch_add(1) < 5) {
			std::fprintf(stderr, "[TexData] view FAILED addr=0x%llx %ux%u\n",
			             static_cast<unsigned long long>(addr), width, height);
		}
	}
	return texture;
}

// RDNA cube sampling uses face coordinates, so native MSL binds a 2D array.
// Each guest array slice contains the full reversed mip chain, including its
// shared mip tail. Preserve BC formats so Metal decodes the actual blocks.
static id<MTLTexture> GuestTextureCube(const ShaderTextureResource& guest, uint64_t addr,
                                      uint32_t width, uint32_t height) {
	using Key = std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
	static std::map<Key, id<MTLTexture>> cache;
	const uint32_t levels = std::max<uint32_t>(guest.LastLevel(), guest.MaxMip()) + 1u;
	const uint32_t layers = uint32_t(guest.Depth()) + 1u;
	const Key key {addr, width, height, guest.fields[1] & 0x1ff00000u,
	               guest.fields[3] & 0x01fff000u, guest.fields[4], levels};
	if (const auto it = cache.find(key); it != cache.end()) return it->second;
	if (addr == 0 || width == 0 || height == 0 || width > 8192 || height > 8192 ||
	    layers > 2048 || levels > std::bit_width(std::max(width, height))) return nil;
	const auto format = guest.Format();
	const MTLPixelFormat mtl = MetalFormatFromBufferFormat(format);
	if (mtl == MTLPixelFormatInvalid) return nil;
	TileSurfaceLayout layout {};
	const TileSurfaceDescription desc {format, guest.TileMode(), TileSurfaceDimension::Dim2D,
	                                   width, height, 1u, levels, layers};
	if (!TileGetTiledTextureLayout(desc, layout) || layout.total_size > (256ull << 20)) return nil;
	MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
	td.textureType = MTLTextureType2DArray;
	td.pixelFormat = mtl;
	td.width = width;
	td.height = height;
	td.arrayLength = layers;
	td.mipmapLevelCount = levels;
	td.usage = GuestTextureUsage(mtl);
	td.storageMode = MTLStorageModeShared;
	td.allowGPUOptimizedContents = NO;
	id<MTLTexture> texture = KytyNewTexture(MetalContext::Instance().GetDevice(), td, __LINE__);
	if (texture == nil) return nil;
	std::vector<uint8_t> source(layout.total_size);
	const bool readable = LibKernel::Memory::TryReadBacking(addr, source.data(), source.size());
	uint64_t nonzero = 0;
	{
		const auto& block = layout.texture.block;
		for (uint32_t layer = 0; layer < layers; ++layer) {
			for (uint32_t level = 0; level < levels; ++level) {
				const auto& mip = layout.mips[level];
				const uint32_t pitch = mip.width * block.bytes_per_element;
				std::vector<uint8_t> linear(static_cast<size_t>(pitch) * mip.height);
				for (uint32_t y = 0; y < mip.height; ++y) for (uint32_t x = 0; x < mip.width; ++x) {
					const bool tail = level >= layout.first_tail_level;
					const uint32_t bx = tail ? 0u : x / block.block_width;
					const uint32_t by = tail ? 0u : y / block.block_height;
					uint32_t within = 0, bank = 0;
					if (!TileGetBlockOffset(block, tail ? x + mip.tail_x : x % block.block_width,
					                        tail ? y + mip.tail_y : y % block.block_height, 0, within) ||
					    !TileGetBlockXor(block, bx, by, layer, bank)) return nil;
					const uint64_t offset = uint64_t(layer) * layout.block_slice_size + mip.offset +
					    uint64_t(by * (mip.padded_width / block.block_width) + bx) * block.block_size + (within ^ bank);
					if (offset > source.size() || block.bytes_per_element > source.size() - offset) return nil;
					auto* out = linear.data() + size_t(y) * pitch + x * block.bytes_per_element;
					std::memcpy(out, source.data() + offset, block.bytes_per_element);
				}
				for (auto byte: linear) nonzero += byte != 0;
				[texture replaceRegion:MTLRegionMake2D(0, 0, std::max(width >> level, 1u), std::max(height >> level, 1u))
				           mipmapLevel:level slice:layer withBytes:linear.data()
				           bytesPerRow:pitch bytesPerImage:linear.size()];
			}
		}
	}
	cache[key] = texture;
	std::fprintf(stderr, "[TexCube] addr=0x%llx %ux%u layers=%u levels=%u fmt=%lu tile=%u readable=%d nonzero=%llu\n",
	             (unsigned long long)addr, width, height, layers, levels, (unsigned long)mtl,
	             (unsigned)guest.TileMode(), readable, (unsigned long long)nonzero);
	return texture;
}

static id<MTLTexture> ComputeGuestImage(const ShaderTextureResource& tex, uint32_t w, uint32_t h, uint64_t addr, bool r128) {
	if (tex.Type() == Prospero::ImageType::kCube) {
		return GuestTextureCube(tex, addr, w, h);
	}
	if (tex.Type() == Prospero::ImageType::kColor3D) {
		return CanonicalMipImage(tex,w,h,addr,r128).image;
	}
	GuestMipViewPlan mip_view;
	if (!PlanGuestMipView(tex,w,h,r128,mip_view)) return nil;
	const uint32_t levels = mip_view.texture_levels;
	if (KYTY_ENV("KYTY_SMALL_TEX_DIR") != nullptr && h == 32 && (w == 32 || w == 40)) {
		static std::set<uint64_t> seen;
		if (seen.insert(addr).second)
			std::fprintf(stderr, "[SmallTexDesc] addr=0x%llx %ux%u base=%u last=%u max=%u levels=%u T#=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
			             (unsigned long long)addr, w, h, (unsigned)tex.BaseLevel(), (unsigned)tex.LastLevel(),
			             (unsigned)tex.MaxMip(), levels, tex.fields[0], tex.fields[1], tex.fields[2], tex.fields[3],
			             tex.fields[4], tex.fields[5], tex.fields[6], tex.fields[7]);
	}
	return GuestTexture(addr,w,h,tex.Format(),tex.TileMode(),levels,mip_view.physical_levels,mip_view.first_level);
}

// KYTY_GUEST_TEX_VERIFY=1: compares a bound block-compressed copy with a fresh decode of its guest bytes, with the blocks a GPU kernel
// encoded for the address laid over them (they stay on the host), on every mip level, every 30 flips per address. Logs the first result per
// address and every change. cache/registry: the bound object is the GuestTexture entry / the registry entry; decode_flip and hash_checked: flips
// of its decode and of its last freshness check; gpu: a kernel encoded blocks for this address. KYTY_GUEST_TEX_VERIFY_DIR=<dir> also writes both
// sides of the first bad level of the first 8 mismatches.
// Same overlay as BcOverlayLevel, but it changes nothing: that one drops a level image whose address looks reused.
static void BcOverlayPeek(uint64_t addr, uint32_t level, uint32_t block_bytes, std::vector<uint8_t>& linear, uint32_t pitch, Prospero::TileMode tile, bool& gpu) {
	const auto it = s_bc_level_images.find({addr, level});
	if (it == s_bc_level_images.end() || it->second.web != block_bytes || pitch == 0) return;
	if (it->second.tile != tile || GuestTexSourceHash(addr, it->second.guest_bytes) != it->second.guest_hash) return;
	const uint32_t columns = pitch / block_bytes;
	const uint32_t rows    = static_cast<uint32_t>(linear.size() / pitch);
	id<MTLTexture> image = it->second.tex;
	if (image.width != columns || image.height != rows) return;
	gpu = true;
	std::vector<uint32_t> blocks(static_cast<size_t>(columns) * rows * block_bytes / 4u);
	[image getBytes:blocks.data() bytesPerRow:pitch fromRegion:MTLRegionMake2D(0, 0, columns, rows) mipmapLevel:0];
	const uint32_t words = block_bytes / 4u;
	for (size_t b = 0; b < static_cast<size_t>(columns) * rows; ++b) {
		const uint32_t* wd = blocks.data() + b * words;
		bool written = false;
		for (uint32_t i = 0; i < words; ++i) written = written || wd[i] != kBcUnwritten;
		if (written) std::memcpy(linear.data() + b * block_bytes, wd, block_bytes);
	}
}

static void VerifyBoundGuestTex(const ShaderTextureResource& tex, uint64_t addr, uint32_t w, uint32_t h, id<MTLTexture> bound) {
	static const bool s_on = KYTY_ENV("KYTY_GUEST_TEX_VERIFY") != nullptr;
	if (!s_on || bound == nil || addr == 0 || tex.Type() != Prospero::ImageType::kColor2D || tex.TileMode() == Prospero::TileMode::kLinear) return;
	const uint32_t block = Prospero::BlockCompressedBytesPerBlock(tex.Format());
	if (block == 0 || !IsCompressedFormat(bound.pixelFormat) || bound.storageMode != MTLStorageModeShared || bound.textureType != MTLTextureType2D) return;
	GuestMipViewPlan mip_view;
	if (!PlanGuestMipView(tex,w,h,false,mip_view)) return;
	const uint32_t levels = mip_view.texture_levels;
	const bool upload_mips = mip_view.physical_levels > 1 && !IsLinearRenderTargetAddress(addr);
	if (bound.mipmapLevelCount != (upload_mips ? levels : 1u)) return;
	struct State {
		uint64_t flip  = 0;
		int      state = -1;
	};
	static std::unordered_map<uint64_t, State> s_states;
	static uint64_t s_checks = 0, s_mismatches = 0, s_summary_flip = 0;
	const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	State& st = s_states[addr];
	if (st.state != -1 && flip < st.flip + 30) return;
	st.flip = flip;
	std::vector<std::vector<uint8_t>> expect;
	std::vector<uint32_t>             pitches;
	bool                              gpu = false;
	if (upload_mips) {
		TileSurfaceLayout            layout {};
		const TileSurfaceDescription desc {tex.Format(), tex.TileMode(), TileSurfaceDimension::Dim2D,w,h,1,mip_view.physical_levels,1};
		if (levels > std::bit_width(std::max(w, h)) || !TileGetTiledTextureLayout(desc, layout) || layout.total_size > (256ull << 20)) return;
		std::vector<uint8_t> source(layout.total_size);
		if (!LibKernel::Memory::TryReadBacking(addr, source.data(), source.size())) return;
		for (uint32_t level = 0; level < levels; ++level) {
			std::vector<uint8_t> linear;
			uint32_t             pitch = 0;
			if (!DecodeTiledTextureLevel(source, layout, level, 0, linear, pitch)) return;
			BcOverlayPeek(addr, level, block, linear, pitch, tex.TileMode(), gpu);
			expect.push_back(std::move(linear));
			pitches.push_back(pitch);
		}
	} else {
		const uint64_t source_bytes = DetileSourceBytes(w, h, tex.Format(), tex.TileMode());
		if (source_bytes == 0 || source_bytes > (256ull << 20)) return;
		std::vector<uint8_t> source(source_bytes);
		if (!LibKernel::Memory::TryReadBacking(addr, source.data(), source.size())) return;
		std::vector<uint8_t> linear;
		uint32_t             pitch = 0;
		if (!DetileToLinear(source.data(), w, h, tex.Format(), tex.TileMode(), linear, pitch)) return;
		BcOverlayPeek(addr, 0, block, linear, pitch, tex.TileMode(), gpu);
		expect.push_back(std::move(linear));
		pitches.push_back(pitch);
	}
	size_t               diff = 0, total = 0;
	uint32_t             bad_levels = 0, first_bad = 0;
	std::vector<uint8_t> first_copy;
	for (uint32_t level = 0; level < expect.size(); ++level) {
		const auto&    want  = expect[level];
		const uint32_t pitch = pitches[level];
		const NSUInteger lw  = std::max<NSUInteger>(bound.width >> level, 1), lh = std::max<NSUInteger>(bound.height >> level, 1);
		if (pitch == 0 || pitch < ((lw + 3) / 4) * block || want.size() < pitch) return;
		std::vector<uint8_t> copy(want.size());
		[bound getBytes:copy.data() bytesPerRow:pitch fromRegion:MTLRegionMake2D(0, 0, lw, lh) mipmapLevel:level];
		size_t bad = 0;
		for (size_t b = 0; b + block <= want.size(); b += block) bad += std::memcmp(want.data() + b, copy.data() + b, block) != 0;
		total += want.size() / block;
		diff += bad;
		if (bad != 0) {
			if (bad_levels == 0) { first_bad = level; first_copy = std::move(copy); }
			bad_levels |= 1u << level;
		}
	}
	const int state = diff != 0 ? 1 : 0;
	++s_checks;
	s_mismatches += state;
	if (flip >= s_summary_flip + 600) {
		s_summary_flip = flip;
		std::fprintf(stderr, "[GuestTexVerify] flip=%llu checks=%llu mismatches=%llu addresses=%zu\n", static_cast<unsigned long long>(flip),
		             static_cast<unsigned long long>(s_checks), static_cast<unsigned long long>(s_mismatches), s_states.size());
	}
	if (state == st.state) return;
	st.state = state;
	const auto cached = s_guest_textures.find(addr);
	const bool in_cache = cached != s_guest_textures.end();
	std::fprintf(stderr, "[GuestTexVerify] %s flip=%llu addr=0x%llx %ux%u fmt=%u mtl=%lu tile=%u levels=%zu bad=0x%x diff=%zu/%zu gpu=%d cache=%d registry=%d decode_flip=%llu hash_checked=%llu cs_written=%d src_hash_same=%d\n",
	             state != 0 ? "MISMATCH" : "ok", static_cast<unsigned long long>(flip), static_cast<unsigned long long>(addr), w, h,
	             static_cast<unsigned>(tex.Format()), static_cast<unsigned long>(bound.pixelFormat), static_cast<unsigned>(tex.TileMode()), expect.size(), bad_levels, diff, total, gpu ? 1 : 0,
	             in_cache && cached->second.tex == bound ? 1 : 0, GetComputeWrittenImageForAddress(addr) == (__bridge void*)bound ? 1 : 0,
	             in_cache ? static_cast<unsigned long long>(cached->second.decode_flip) : 0ull, in_cache ? static_cast<unsigned long long>(cached->second.checked_flip) : 0ull,
	             in_cache && cached->second.cs_written ? 1 : 0, in_cache && cached->second.src_bytes != 0 && GuestTexSourceHash(addr, cached->second.src_bytes) == cached->second.src_hash ? 1 : 0);
	static uint32_t s_dumps = 0;
	if (const char* dir = KYTY_ENV("KYTY_GUEST_TEX_VERIFY_DIR"); dir != nullptr && state != 0 && s_dumps < 8) {
		++s_dumps;
		char path[1024];
		std::snprintf(path, sizeof(path), "%s/%llx-flip%llu-%ux%u-f%u-L%u-copy.bin", dir, static_cast<unsigned long long>(addr), static_cast<unsigned long long>(flip), w, h, static_cast<unsigned>(tex.Format()), first_bad);
		if (FILE* f = std::fopen(path, "wb")) { std::fwrite(first_copy.data(), 1, first_copy.size(), f); std::fclose(f); }
		std::snprintf(path, sizeof(path), "%s/%llx-flip%llu-%ux%u-f%u-L%u-guest.bin", dir, static_cast<unsigned long long>(addr), static_cast<unsigned long long>(flip), w, h, static_cast<unsigned>(tex.Format()), first_bad);
		if (FILE* f = std::fopen(path, "wb")) { std::fwrite(expect[first_bad].data(), 1, expect[first_bad].size(), f); std::fclose(f); }
	}
}

// KYTY_LOG_MASKS=1: every small BC4 texture (the body hide masks and the sources they are built from) a draw binds, with the average and the
// share of white of its first level, at most once per 120 flips per address. Read it together with the [BcWritebackDesc] lines
// (KYTY_LOG_BC_WRITEBACK=1): a build draw with its source, then the encoder writes of the finished mask, then the body draws that bind it.
static void LogSmallBc4(uint64_t addr, id<MTLTexture> t, uint64_t guest_ps, bool is_ps) {
	static const bool s_on = KYTY_ENV("KYTY_LOG_MASKS") != nullptr;
	if (s_on && guest_ps == 0x0221d01bd7da2ad4ull &&
	    (t == nil || t.pixelFormat != MTLPixelFormatBC4_RUnorm || t.width > 256 || t.height > 256 || t.textureType != MTLTextureType2D ||
	     t.storageMode != MTLStorageModeShared)) {
		// The copy shader also reads other formats; a mask source bound as one of them would be invisible to the BC4 log below.
		static std::mutex s_other_mutex;
		static std::set<std::tuple<uint64_t, uint64_t, uint64_t>> s_other_seen;
		static int s_other_lines = 0;
		const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
		std::lock_guard<std::mutex> lock(s_other_mutex);
		if (s_other_lines < 600 && s_other_seen.insert({addr, t != nil ? static_cast<uint64_t>(t.pixelFormat) : 0, flip / 40}).second) {
			++s_other_lines;
			std::fprintf(stderr, "[MaskLog] flip=%llu ps=%016llx addr=0x%llx OTHER %s fmt=%lu %lux%lu levels=%lu type=%lu storage=%lu\n", static_cast<unsigned long long>(flip),
			             static_cast<unsigned long long>(guest_ps), static_cast<unsigned long long>(addr), t == nil ? "nil" : "tex",
			             t != nil ? static_cast<unsigned long>(t.pixelFormat) : 0ul, t != nil ? static_cast<unsigned long>(t.width) : 0ul,
			             t != nil ? static_cast<unsigned long>(t.height) : 0ul, t != nil ? static_cast<unsigned long>(t.mipmapLevelCount) : 0ul,
			             t != nil ? static_cast<unsigned long>(t.textureType) : 0ul, t != nil ? static_cast<unsigned long>(t.storageMode) : 0ul);
		}
	}
	if (!s_on || t == nil || t.pixelFormat != MTLPixelFormatBC4_RUnorm || t.width > 256 || t.height > 256 || t.textureType != MTLTextureType2D ||
	    t.storageMode != MTLStorageModeShared)
		return;
	static std::unordered_map<uint64_t, uint64_t> s_last;
	const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	uint64_t&      last = s_last[addr ^ (guest_ps << 1)];
	// The generic copy shader (guest 0221d01bd7da2ad4) builds the masks: every one of its draws is logged, since the source of each build is what is asked.
	if (guest_ps != 0x0221d01bd7da2ad4ull && last != 0 && flip < last + 120) return;
	last = flip == 0 ? 1 : flip;
	const NSUInteger bw = (t.width + 3) / 4, bh = (t.height + 3) / 4;
	std::vector<uint8_t> blocks(bw * bh * 8);
	[t getBytes:blocks.data() bytesPerRow:bw * 8 fromRegion:MTLRegionMake2D(0, 0, t.width, t.height) mipmapLevel:0];
	uint64_t sum = 0, white = 0, black = 0, texels = 0;
	for (size_t b = 0; b < bw * bh; ++b) {
		const uint8_t* blk = blocks.data() + b * 8;
		const int a0 = blk[0], a1 = blk[1];
		int pal[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
		if (a0 > a1) { for (int k = 1; k < 7; ++k) pal[1 + k] = ((7 - k) * a0 + k * a1) / 7; }
		else { for (int k = 1; k < 5; ++k) pal[1 + k] = ((5 - k) * a0 + k * a1) / 5; pal[6] = 0; pal[7] = 255; }
		uint64_t bits = 0;
		for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(blk[2 + i]) << (8 * i);
		for (int px = 0; px < 16; ++px) {
			const int v = pal[(bits >> (3 * px)) & 7];
			sum += static_cast<uint64_t>(v);
			white += v >= 250;
			black += v <= 5;
			++texels;
		}
	}
	std::fprintf(stderr, "[MaskLog] flip=%llu %s=%016llx addr=0x%llx %lux%lu levels=%lu mean=%.1f white=%.1f%% black=%.1f%%\n", static_cast<unsigned long long>(flip), is_ps ? "ps" : "vs",
	             static_cast<unsigned long long>(guest_ps), static_cast<unsigned long long>(addr), static_cast<unsigned long>(t.width), static_cast<unsigned long>(t.height),
	             static_cast<unsigned long>(t.mipmapLevelCount), texels ? static_cast<double>(sum) / texels : 0.0, texels ? 100.0 * white / texels : 0.0, texels ? 100.0 * black / texels : 0.0);
}

// Compute image creation: the program's declared dimension decides 3D, not the T# type field (the LUT
// kernel declares texture3d while its descriptor reads as a 2D surface).
// A sampled, linear (tile 0) compute source is a live view of guest memory, but the GPU runs behind the guest CPU: the game
// refills its staging buffer for the next upload before this dispatch executes. Copying the bytes at encode time keeps the
// data the dispatch was recorded with (glyph atlas uploads were garbled otherwise).
static id<MTLTexture> LinearSourceSnapshot(const ShaderTextureResource& tex, uint32_t w, uint32_t h, uint64_t addr) {
	const MTLPixelFormat fmt = MetalFormatFromBufferFormat(tex.Format());
	const size_t bpp = fmt == MTLPixelFormatInvalid ? 0 : BytesPerPixel(fmt);
	if (bpp == 0 || w == 0 || h == 0 || w > 8192 || h > 8192 || Prospero::BlockCompressedBytesPerBlock(tex.Format()) != 0) return nil;
	const size_t bytes = static_cast<size_t>(w) * h * bpp;
	if (bytes > (32u << 20)) return nil;
	std::vector<uint8_t> data(bytes);
	if (!Libs::LibKernel::Memory::TryReadBacking(addr, data.data(), bytes)) return nil;
	MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
	td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
	td.storageMode = MTLStorageModeShared;
	id<MTLTexture> t = KytyNewTexture(MetalContext::Instance().GetDevice(), td, __LINE__);
	[t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:data.data() bytesPerRow:w * bpp];
	return t;
}

// KYTY_GPU_DETILE_SOURCES=1 (experiment): small tiled images a kernel only reads, at addresses no GPU pass wrote, are detiled on the
// GPU from a view of guest memory right before the dispatch (EncodeTileImport) instead of on the CPU while the dispatch is encoded.
// The guest GPU reads such memory when the dispatch runs; data that streams in between (file reads, DMA, CPU writes that a WAIT_REG_MEM
// orders before the dispatch) was missing from the CPU decode, and a one-shot copy kernel (the FC27 logo, match HUD crests and names)
// then kept the wrong blocks for good. Prepared per dispatch; ComputeGuestImageFor hands them out.
static bool GpuDetileSourcesEnabled() {
	static const bool s_on = KYTY_ENV("KYTY_GPU_DETILE_SOURCES") != nullptr;
	return s_on;
}
struct GpuDetiled {
	uint32_t       w = 0, h = 0;
	id<MTLTexture> tex = nil;
};
static std::unordered_map<uint64_t, GpuDetiled> s_gpu_detiled; // this dispatch's prepared inputs, by guest address

static void PrepareGpuDetiledSources(const ShaderStageRuntime& stage) {
	s_gpu_detiled.clear();
	if (!GpuDetileSourcesEnabled() || stage.program == nullptr || stage.resources == nullptr) return;
	const auto& prog = *stage.program;
	const auto& snap = *stage.resources;
	static std::map<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t>, id<MTLTexture>> s_targets; // (addr, w, h, format) -> element texture
	static std::map<std::pair<void*, uint32_t>, id<MTLTexture>> s_views;
	id<MTLDevice> dev = MetalContext::Instance().GetDevice();
	for (size_t res = 0; res < snap.images.size() && res < prog.info.images.size(); ++res) {
		const auto& info = prog.info.images[res];
		if (info.written || info.atomic || info.dimension != ShaderRecompiler::Decoder::ImageDimension::Dim2D) continue;
		ShaderTextureResource tex {};
		for (size_t f = 0; f < 8; ++f) tex.fields[f] = snap.images[res].dwords[f];
		const uint64_t addr = tex.Base40();
		const uint32_t w = static_cast<uint32_t>(tex.Width5()) + 1u, h = static_cast<uint32_t>(tex.Height5()) + 1u;
		if (addr == 0 || tex.TileMode() == Prospero::TileMode::kLinear || tex.Type() != Prospero::ImageType::kColor2D ||
		    tex.BaseLevel() != 0 || tex.LastLevel() != 0 || static_cast<uint64_t>(w) * h > 512ull * 512ull) continue;
		const MTLPixelFormat want = MetalFormatFromBufferFormat(tex.Format());
		if (want == MTLPixelFormatInvalid || IsCompressedFormat(want) || s_gpu_detiled.count(addr) != 0) continue;
		if (LastGpuWriterTexture(addr) != nil) continue;
		{
			std::lock_guard<std::mutex> lock(s_render_target_mutex);
			if (s_rt_by_addr.find(addr) != s_rt_by_addr.end()) continue;
		}
		{
			std::lock_guard<std::mutex> lock(s_cs_storage_mutex);
			if (s_cs_storage.find(addr) != s_cs_storage.end()) continue;
		}
		uint32_t ew = 0, eh = 0;
		const TileTables* tables = SurfaceTables(tex.Format(), tex.TileMode(), w, h, ew, eh);
		if (tables == nullptr || ew != w || eh != h) continue;
		const MTLPixelFormat elem = TileElementFormat(tables->block.bytes_per_element);
		if (elem == MTLPixelFormatInvalid || BytesPerPixel(elem) != BytesPerPixel(want)) continue;
		const uint64_t bytes = DetileSourceBytes(w, h, tex.Format(), tex.TileMode());
		const auto source = MetalBufferManager::Instance().GetBufferForGuestRange(addr, bytes);
		if (!source.IsValid() || source.buffer == nil) continue;
		const auto     tkey   = std::make_tuple(addr, w, h, static_cast<uint32_t>(tex.Format()));
		id<MTLTexture> target = s_targets[tkey];
		if (target == nil) {
			MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:elem width:w height:h mipmapped:NO];
			d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
			d.storageMode = MTLStorageModePrivate;
			target = KytyNewTexture(dev, d, __LINE__);
			if (target == nil) continue;
			s_targets[tkey] = target;
		}
		MetalContext::Instance().EndCurrentEncoder();
		if (!EncodeTileImport(dev, MetalContext::Instance().GetCurrentCommandBuffer(), source.buffer, source.offset, target, *tables, w, h)) continue;
		id<MTLTexture> out = target;
		if (want != elem) {
			const auto     vkey = std::make_pair((__bridge void*)target, static_cast<uint32_t>(want));
			id<MTLTexture> view = s_views[vkey];
			if (view == nil) {
				view = [target newTextureViewWithPixelFormat:want];
				if (view == nil) continue;
				s_views[vkey] = view;
			}
			out = view;
		}
		s_gpu_detiled[addr] = {w, h, out};
		static std::atomic<uint32_t> s_logged {0};
		if (s_logged.fetch_add(1) < 24) {
			std::fprintf(stderr, "[GpuDetile] addr=0x%llx %ux%u fmt=%u tile=%u elem=%lu bytes=%llu\n", (unsigned long long)addr, w, h,
			             static_cast<unsigned>(tex.Format()), static_cast<unsigned>(tex.TileMode()), (unsigned long)elem, (unsigned long long)bytes);
		}
	}
}

static id<MTLTexture> ComputeGuestImageFor(const ShaderRecompiler::IR::CompiledShaderInfo& prog, uint32_t res,
                                           const ShaderTextureResource& tex, uint32_t w, uint32_t h, uint64_t addr,
                                           bool storage = false) {
	static const bool s_snap = KYTY_ENV("KYTY_SNAPSHOT_LINEAR") != nullptr;
	if (s_snap && !storage && tex.TileMode() == Prospero::TileMode::kLinear && tex.Type() != Prospero::ImageType::kCube &&
	    tex.Type() != Prospero::ImageType::kColor3D) {
		if (id<MTLTexture> snap = LinearSourceSnapshot(tex, w, h, addr)) return snap;
	}
	const bool is3d = res < prog.info.images.size() &&
	                  prog.info.images[res].dimension == ShaderRecompiler::Decoder::ImageDimension::Dim3D;
	if (is3d) {
		if(tex.Type()!=Prospero::ImageType::kColor3D)RejectMetalState("3D shader image requires a 3D descriptor");
		return CanonicalMipImage(tex,w,h,addr,prog.info.images[res].r128).image;
	}
	if (!storage) {
		if (const auto it = s_gpu_detiled.find(addr); it != s_gpu_detiled.end() && it->second.w == w && it->second.h == h) return it->second.tex;
	}
	s_guest_tex_check_each = !storage;
	id<MTLTexture> image = ComputeGuestImage(tex,w,h,addr,res < prog.info.images.size() && prog.info.images[res].r128);
	s_guest_tex_check_each = false;
	return image;
}

// Argument capture for the MTLArgumentEncoder graphics path (KYTY_ARGENC_GFX): every write the legacy
// binders make into vs_args/ps_args is also recorded as an object (buffer+offset, texture, or sampler)
// at the same argument index, so the encoder reproduces the legacy binding element by element.
struct ArgCapture {
	uint32_t             index  = 0;
	id<MTLBuffer>        buffer = nil;
	NSUInteger           offset = 0;
	id<MTLTexture>       texture = nil;
	id<MTLSamplerState>  sampler = nil;
};
static std::vector<ArgCapture> s_ps_caps;
static std::vector<ArgCapture> s_vs_caps;
// KYTY_VS_DBG: debug buffer bound at vertex buffer 27 for the probed vertex shader (see ApplyVsDbg).
static id<MTLBuffer> s_vsdbg_buf = nil;

// Same as the GuestGpuAddress lambda inside BindMetalShaderResources, but returns the whole allocation
// (buffer + offset) so argument captures can bind it by object.
static MetalBufferAllocation GuestGpuAllocation(uint64_t vaddr, size_t size) {
	if (vaddr == 0) {
		return {};
	}
	auto alloc = MetalBufferManager::Instance().GetBufferForGuestRange(vaddr, size != 0 ? size : 4096);
	if (alloc.IsValid() && alloc.buffer) {
		MetalDrawExecutor::Instance().UseResource(alloc.buffer, MTLResourceUsageRead);
	}
	return alloc;
}

static std::vector<MetalImageWriteback> s_graphics_image_writebacks;
// Every 2 s: how often cached copies of guest memory were reused instead of imported again (metalGuestWrites.h).
static void ReportGuestWrites() {
	static auto last = std::chrono::steady_clock::now();
	const auto now = std::chrono::steady_clock::now();
	if (now - last < std::chrono::seconds(2)) return;
	last = now;
	const auto st = GuestWrites::TakeStats();
	std::fprintf(stderr, "[GuestWrites] images import=%llu reuse=%llu targets import=%llu skip=%llu | faults=%llu watched=%llu gpu_notes=%llu unknown=%llu host=%llu\n",
	             (unsigned long long)s_canonical_image_imports.exchange(0), (unsigned long long)s_canonical_image_reuses.exchange(0),
	             (unsigned long long)s_canonical_target_imports.exchange(0), (unsigned long long)s_canonical_target_skips.exchange(0),
	             (unsigned long long)st.faults, (unsigned long long)st.watched_pages, (unsigned long long)st.gpu_notes,
	             (unsigned long long)st.unknown, (unsigned long long)st.host_invalidations);
}
static void BindMetalShaderResources(const ShaderVertexInputInfo& vs_info,
                                     const ShaderPixelInputInfo& ps_info,
                                     bool pixel_active) {
	s_graphics_image_writebacks.clear();
	ReportGuestWrites();
	NoteStageGuestWrites(vs_info.stage);
	if (pixel_active) NoteStageGuestWrites(ps_info.stage);
	s_draw_seq.fetch_add(1, std::memory_order_relaxed);
	s_trace_draw_inputs.clear();
	s_trace_draw_bufs.clear();
	s_trace_vs_bufs.clear();
	s_position_watch_bufs.clear();
	s_trace_vs_hash = vs_info.stage.program != nullptr ? vs_info.stage.program->shader_hash : 0ull;
	s_trace_ps_hash = (pixel_active && ps_info.stage.program != nullptr) ? ps_info.stage.program->shader_hash : 0ull;
	// Guest hashes (the Metal function keys above differ) so the lines can be matched with the Vulkan reference trace.
	s_trace_gps_hash = (pixel_active && ps_info.stage.program != nullptr) ? ps_info.stage.program->guest_hash : 0ull;
	if (PassTimerEnabled()) {
		char label[64];
		std::snprintf(label, sizeof(label), "ps=%016llx", static_cast<unsigned long long>(s_trace_ps_hash));
		PassTimerNote(label);
	}
	s_ps_caps.clear();
	s_vs_caps.clear();
	// KYTY_BIND_TRACE=1: one BIND line per draw (hashes, first user dwords, buffers, images).
	const bool bind_trace = KYTY_ENV("KYTY_BIND_TRACE") != nullptr && TraceWindow::Active();
	std::string bt;
	char stage_tag = '-';
	{
		static std::atomic<uint32_t> s_bind_entry {0};
		if (s_bind_entry.fetch_add(1) < 6) {
			std::fprintf(stderr,
			             "[Bind] entry vs_prog=%d vs_res=%d ps_prog=%d ps_res=%d pixel=%d\n",
			             vs_info.stage.program ? 1 : 0, vs_info.stage.resources ? 1 : 0,
			             ps_info.stage.program ? 1 : 0, ps_info.stage.resources ? 1 : 0,
			             pixel_active ? 1 : 0);
		}
	}
	// 1. Buffer 1: Push constants / user data registers. Each stage gets its own block: the VS and
	// PS user-data layouts are independent and must not overwrite each other.
	const auto fill_push_data = [](const ShaderStageRuntime& si, ShaderRecompiler::IR::PushData& out) {
		if (!si.program || !si.resources) {
			return;
		}
		const auto& prog = *si.program;
		const auto& snap = *si.resources;
		const uint32_t start = prog.bindings.push_data_start_dword;
		if (start == ShaderRecompiler::IR::PushData::NoStart) {
			return;
		}
		for (size_t i = 0; i < prog.bindings.user_data_registers.size(); ++i) {
			const uint32_t reg = prog.bindings.user_data_registers[i];
			if (reg >= prog.user_data_base && (reg - prog.user_data_base) < snap.user_data.size() &&
			    start + i < out.dwords.size()) {
				out.dwords[start + i] = snap.user_data[reg - prog.user_data_base];
			}
		}
		// Per-buffer byte adjustment (offset within the 256-byte aligned binding), packed 4 per dword
		// starting at memory_offset_dword. Mirrors pack_memory_offset in the Vulkan path.
		if (prog.bindings.memory_offset_count != 0 && !prog.bindings.descriptors.empty() &&
		    prog.bindings.memory_offset_dword != ShaderRecompiler::IR::PushData::NoStart) {
			const auto& resources = prog.bindings.descriptors.front().resources;
			for (uint32_t i = 0; i < prog.bindings.memory_offset_count && i < resources.size(); ++i) {
				const uint32_t b = resources[i];
				if (b >= snap.buffers.size()) {
					continue;
				}
				const auto& desc = snap.buffers[b];
				const uint64_t addr = (static_cast<uint64_t>(desc.dwords[1] & 0xFFFF) << 32) | desc.dwords[0];
				size_t size = 0;
				if (b < prog.info.buffers.size()) {
					size = prog.info.buffers[b].max_byte_extent;
				}
				const auto ga = GuestGpuAllocation(addr, size);
				const uint32_t adjust = static_cast<uint32_t>(ga.offset & 255u);
				const uint32_t dword = prog.bindings.memory_offset_dword + i / 4u;
				if (dword < out.dwords.size()) {
					out.dwords[dword] |= adjust << ((i % 4u) * 8u);
				}
			}
		}
	};
	ShaderRecompiler::IR::PushData vs_push_data {};
	ShaderRecompiler::IR::PushData ps_push_data {};
	fill_push_data(vs_info.stage, vs_push_data);
	fill_push_data(ps_info.stage, ps_push_data);
	if (PassTraceActive()) {
		s_trace_vs_push.assign(std::begin(vs_push_data.dwords), std::end(vs_push_data.dwords));
		s_trace_vs_srt = vs_info.stage.resources != nullptr ? vs_info.stage.resources->flattened_srt : std::vector<uint32_t> {};
		// KYTY_PASS_TRACE_PS_SRT=<guest PS hash,...>: print the pixel shader's flattened SRT (constants read through scalar loads) for those
		// shaders in the traced frame, one [PassTrace]   PS_SRT line per draw.
		if (const char* want = KYTY_ENV("KYTY_PASS_TRACE_PS_SRT"); want != nullptr && ps_info.stage.program != nullptr && ps_info.stage.resources != nullptr) {
			char key[24];
			std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(ps_info.stage.program->guest_hash));
			if (std::strstr(want, std::string(key, 8).c_str()) != nullptr) {
				const auto& srt = ps_info.stage.resources->flattened_srt;
				std::string text;
				for (size_t i = 0; i < srt.size() && i < 160; ++i) {
					float f;
					std::memcpy(&f, &srt[i], 4);
					char t[48];
					std::snprintf(t, sizeof(t), " [%zx]=%08x(%g)", i, srt[i], f);
					text += t;
				}
				std::fprintf(stderr, "[PassTrace]   PS_SRT gps=0x%s seq=%llu vud25=%08x n=%zu%s\n", key,
				             static_cast<unsigned long long>(s_draw_seq.load(std::memory_order_relaxed)), vs_push_data.dwords[25], srt.size(), text.c_str());
			}
		}
	}
	{
		static const bool s_log_push = KYTY_ENV("KYTY_LOG_PUSH") != nullptr;
		static std::atomic<uint32_t> s_push_log {0};
		static const uint64_t s_push_hash = KYTY_ENV("KYTY_PROBE_HASH") ? std::strtoull(KYTY_ENV("KYTY_PROBE_HASH"), nullptr, 0) : 0ull;
		const bool push_hash_ok = s_push_hash == 0 || (ps_info.stage.program != nullptr && ps_info.stage.program->shader_hash == s_push_hash);
		if (s_log_push && push_hash_ok && s_push_log.fetch_add(1) < 6) {
			const auto dump = [](const char* tag, const ShaderStageRuntime& si, const ShaderRecompiler::IR::PushData& pd) {
				if (!si.program || !si.resources) {
					std::fprintf(stderr, "[Push] %s: no program/resources\n", tag);
					return;
				}
				const auto& prog = *si.program;
				const auto& snap = *si.resources;
				std::fprintf(stderr, "[Push] %s start=%u regs=%zu user_data=%zu base=%u memoff_cnt=%u memoff_dw=%u dw0=%08x dw1=%08x dw2=%08x dw3=%08x\n",
				             tag, prog.bindings.push_data_start_dword, prog.bindings.user_data_registers.size(),
				             snap.user_data.size(), prog.user_data_base, prog.bindings.memory_offset_count,
				             prog.bindings.memory_offset_dword, pd.dwords[0], pd.dwords[1], pd.dwords[2], pd.dwords[3]);
				for (size_t i = 0; i < prog.bindings.user_data_registers.size() && i < 8; ++i) {
					std::fprintf(stderr, "[Push]   reg[%zu]=%u\n", i, prog.bindings.user_data_registers[i]);
				}
			};
			dump("vs", vs_info.stage, vs_push_data);
			dump("ps", ps_info.stage, ps_push_data);
		}
	}
	MetalDrawExecutor::Instance().SetVertexBytes(&vs_push_data, sizeof(vs_push_data), 1);
	MetalDrawExecutor::Instance().SetFragmentBytes(&ps_push_data, sizeof(ps_push_data), 1);

	// 2. Buffer 25: spvBufferSizeConstants (buffer bounds)
	// Separate vertex and fragment stage buffer size arrays so vertex buffer extents
	// do not truncate pixel shader uniform buffer bounds checks.
	// [64 + i]: depth-compare function of sampler slot i (see StageSamplerCompare).
	uint32_t vs_sizes[ShaderRecompiler::Msl::SizeConstantWordCount];
	uint32_t ps_sizes[ShaderRecompiler::Msl::SizeConstantWordCount];
	for (size_t i = 0; i < ShaderRecompiler::Msl::SizeConstantWordCount; ++i) {
		vs_sizes[i] = 0u;
		ps_sizes[i] = 0u;
	}
	for (int i = 0; i < vs_info.buffers_num && i < 64; ++i) {
		const uint32_t sz = vs_info.buffers[i].num_records * vs_info.buffers[i].stride;
		if (sz > 0) {
			vs_sizes[i] = std::max(vs_sizes[i], sz);
		}
	}
	if (vs_info.stage.program) {
		for (size_t b = 0; b < vs_info.stage.program->info.buffers.size() && b < 64; ++b) {
			const uint32_t extent = vs_info.stage.program->info.buffers[b].max_byte_extent;
			if (extent > vs_sizes[b]) {
				vs_sizes[b] = extent;
			}
		}
		if (vs_info.stage.resources) {
			for (size_t b = 0; b < vs_info.stage.resources->buffers.size() && b < 64; ++b) {
				const auto& vd = vs_info.stage.resources->buffers[b];
				const uint64_t vbase = (static_cast<uint64_t>(vd.dwords[1] & 0xFFFF) << 32) | vd.dwords[0];
				if (vbase != 0) {
					vs_sizes[b] = VsharpBytes(vd.dwords);
				}
			}
		}
	}
	if (pixel_active && ps_info.stage.program) {
		for (size_t b = 0; b < ps_info.stage.program->info.buffers.size() && b < 64; ++b) {
			const uint32_t extent = ps_info.stage.program->info.buffers[b].max_byte_extent;
			if (extent > ps_sizes[b]) {
				ps_sizes[b] = extent;
			}
		}
		if (ps_info.stage.resources) {
			for (size_t b = 0; b < ps_info.stage.resources->buffers.size() && b < 64; ++b) {
				const auto& pd = ps_info.stage.resources->buffers[b];
				const uint64_t pbase = (static_cast<uint64_t>(pd.dwords[1] & 0xFFFF) << 32) | pd.dwords[0];
				if (pbase != 0) {
					ps_sizes[b] = VsharpBytes(pd.dwords);
				}
			}
		}
	}
	// KYTY_LOG_VBUF=<VS hash>: V# dwords, size limits and the shader's own extents/strides of that vertex shader's buffers (first draws).
	{
		static const uint64_t s_vb_hash = KYTY_ENV("KYTY_LOG_VBUF") ? std::strtoull(KYTY_ENV("KYTY_LOG_VBUF"), nullptr, 0) : 0ull;
		static std::atomic<uint32_t> s_vb_n {0};
		// KYTY_LOG_VBUF=1 selects the skinning-style vertex shaders instead (buffer 8 declared as a >= 20 MB pool).
		if (s_vb_hash != 0 && vs_info.stage.program && vs_info.stage.resources &&
		    (s_vb_hash == 1 ? vs_sizes[8] >= 20000000u : vs_info.stage.program->shader_hash == s_vb_hash) && s_vb_n.fetch_add(1) < 6) {
			const auto& vp = *vs_info.stage.program;
			const auto& vr = *vs_info.stage.resources;
			std::fprintf(stderr, "[VBuf] draw#%u vs=0x%llx buffers=%zu\n", s_vb_n.load(), (unsigned long long)vp.shader_hash, vr.buffers.size());
			if (vr.buffers.size() > 4) {
				// Instance records (stride 28 here): the first four, as raw dwords.
				const auto& d4 = vr.buffers[4];
				const uint64_t b4 = (static_cast<uint64_t>(d4.dwords[1] & 0xFFFF) << 32) | d4.dwords[0];
				uint32_t rec[28] = {};
				if (Libs::LibKernel::Memory::TryReadBacking(b4, rec, sizeof(rec))) {
					for (int r = 0; r < 4; ++r) {
						std::fprintf(stderr, "[VBuf]   b4 rec%d: %08x %08x %08x %08x %08x %08x %08x\n", r, rec[r * 7], rec[r * 7 + 1], rec[r * 7 + 2],
						             rec[r * 7 + 3], rec[r * 7 + 4], rec[r * 7 + 5], rec[r * 7 + 6]);
					}
					// Matrix pool rows (buffer 8, 16 B per row) at the two indices of each record, 4 rows each.
					if (vr.buffers.size() > 8) {
						const auto& d8 = vr.buffers[8];
						const uint64_t b8 = (static_cast<uint64_t>(d8.dwords[1] & 0xFFFF) << 32) | d8.dwords[0];
						for (int r = 0; r < 3; ++r) {
							for (int which = 0; which < 2; ++which) {
								const uint32_t idx = rec[r * 7 + 2 + which];
								float rows[16] = {};
								if (idx < 1600000u && Libs::LibKernel::Memory::TryReadBacking(b8 + static_cast<uint64_t>(idx) * 16u, rows, sizeof(rows))) {
									std::fprintf(stderr, "[VBuf]   pool[%u]: (%g %g %g %g) (%g %g %g %g) (%g %g %g %g) (%g %g %g %g)\n", idx,
									             static_cast<double>(rows[0]), static_cast<double>(rows[1]), static_cast<double>(rows[2]), static_cast<double>(rows[3]),
									             static_cast<double>(rows[4]), static_cast<double>(rows[5]), static_cast<double>(rows[6]), static_cast<double>(rows[7]),
									             static_cast<double>(rows[8]), static_cast<double>(rows[9]), static_cast<double>(rows[10]), static_cast<double>(rows[11]),
									             static_cast<double>(rows[12]), static_cast<double>(rows[13]), static_cast<double>(rows[14]), static_cast<double>(rows[15]));
								}
							}
						}
					}
				}
			}
			for (size_t b = 0; b < vr.buffers.size() && b < 16; ++b) {
				const auto& d = vr.buffers[b];
				const uint64_t base = (static_cast<uint64_t>(d.dwords[1] & 0xFFFF) << 32) | d.dwords[0];
				const auto& bi = b < vp.info.buffers.size() ? vp.info.buffers[b] : vp.info.buffers[0];
				if (((d.dwords[1] >> 16) & 0x3fffu) == 12u && d.dwords[2] > 0 && d.dwords[2] <= 65536) {
					// 12-byte streams are float3 positions: report their range and how many vertices are zero/non-finite.
					std::vector<float> fv(static_cast<size_t>(d.dwords[2]) * 3u);
					if (Libs::LibKernel::Memory::TryReadBacking(base, fv.data(), fv.size() * sizeof(float))) {
						float lo = 1e30f, hi = -1e30f;
						uint32_t zero = 0, bad = 0;
						for (uint32_t v = 0; v < d.dwords[2]; ++v) {
							const float* f = &fv[v * 3u];
							const bool fin = std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]);
							if (!fin) { ++bad; continue; }
							if (f[0] == 0.f && f[1] == 0.f && f[2] == 0.f) ++zero;
							lo = std::min({lo, f[0], f[1], f[2]});
							hi = std::max({hi, f[0], f[1], f[2]});
						}
						std::fprintf(stderr, "[VBuf]   b%zu positions: n=%u zero=%u nonfinite=%u range=[%g, %g] first=(%g,%g,%g) last=(%g,%g,%g)\n", b,
						             d.dwords[2], zero, bad, static_cast<double>(lo), static_cast<double>(hi), static_cast<double>(fv[0]),
						             static_cast<double>(fv[1]), static_cast<double>(fv[2]), static_cast<double>(fv[fv.size() - 3]),
						             static_cast<double>(fv[fv.size() - 2]), static_cast<double>(fv[fv.size() - 1]));
					}
				}
				std::fprintf(stderr, "[VBuf]  b%zu base=0x%llx dw=%08x %08x %08x %08x stride=%u nrec=%u size=%u sizes[]=%u shader_extent=%u packed_stride=%u read=%d written=%d\n",
				             b, (unsigned long long)base, d.dwords[0], d.dwords[1], d.dwords[2], d.dwords[3], (d.dwords[1] >> 16) & 0x3fffu,
				             d.dwords[2], VsharpBytes(d.dwords), vs_sizes[b], b < vp.info.buffers.size() ? bi.max_byte_extent : 0u,
				             b < vp.info.buffers.size() ? bi.packed_stride : 0u, b < vp.info.buffers.size() ? (bi.read ? 1 : 0) : 0,
				             b < vp.info.buffers.size() ? (bi.written ? 1 : 0) : 0);
			}
		}
	}
	// KYTY_LOG_DESC=1 + KYTY_PROBE_HASH=<PS hash>: descriptor-set contents for the probed draw
	// (size constants, buffer table, image/sampler counts). Diagnostic only, first 2 matches.
	{
		static const bool s_log_desc = KYTY_ENV("KYTY_LOG_DESC") != nullptr;
		static const uint64_t s_desc_hash = KYTY_ENV("KYTY_PROBE_HASH") ? std::strtoull(KYTY_ENV("KYTY_PROBE_HASH"), nullptr, 0) : 0ull;
		static std::atomic<uint32_t> s_desc_n {0};
		if (s_log_desc && ps_info.stage.program && ps_info.stage.program->shader_hash == s_desc_hash && s_desc_n.fetch_add(1) < 2) {
			std::fprintf(stderr, "[Desc] sizes vs[0..7]=%u %u %u %u %u %u %u %u ps[0..7]=%u %u %u %u %u %u %u %u\n",
			             vs_sizes[0], vs_sizes[1], vs_sizes[2], vs_sizes[3], vs_sizes[4], vs_sizes[5], vs_sizes[6], vs_sizes[7],
			             ps_sizes[0], ps_sizes[1], ps_sizes[2], ps_sizes[3], ps_sizes[4], ps_sizes[5], ps_sizes[6], ps_sizes[7]);
			for (int i = 0; i < vs_info.buffers_num && i < 8; ++i) {
				std::fprintf(stderr, "[Desc]   vb%d addr=0x%llx num=%u stride=%u\n", i,
				             (unsigned long long)vs_info.buffers[i].addr, vs_info.buffers[i].num_records, vs_info.buffers[i].stride);
			}
			if (vs_info.stage.resources) {
				const auto& vr = *vs_info.stage.resources;
				std::fprintf(stderr, "[Desc] vs resources buffers=%zu images=%zu samplers=%zu\n", vr.buffers.size(), vr.images.size(), vr.samplers.size());
				for (size_t b = 0; b < vr.buffers.size() && b < 8; ++b) {
					const auto& d = vr.buffers[b];
					std::fprintf(stderr, "[Desc]   vs buf%zu addr=0x%llx dw2=%u dw3=%08x\n", b,
					             (unsigned long long)((static_cast<uint64_t>(d.dwords[1] & 0xFFFF) << 32) | d.dwords[0]), d.dwords[2], d.dwords[3]);
				}
			}
			if (ps_info.stage.resources) {
				const auto& rs = *ps_info.stage.resources;
				std::fprintf(stderr, "[Desc] ps resources buffers=%zu images=%zu samplers=%zu\n", rs.buffers.size(), rs.images.size(), rs.samplers.size());
				for (size_t b = 0; b < rs.buffers.size() && b < 8; ++b) {
					const auto& d = rs.buffers[b];
					std::fprintf(stderr, "[Desc]   ps buf%zu addr=0x%llx dw2=%u dw3=%08x\n", b,
					             (unsigned long long)((static_cast<uint64_t>(d.dwords[1] & 0xFFFF) << 32) | d.dwords[0]), d.dwords[2], d.dwords[3]);
				}
			}
		}
	}
	// 3. Buffer 0: argument buffer. The layout depends on how each stage was
	// compiled: the direct RDNA->MSL backend uses flat ids with GPU addresses
	// (buffers 0..63, images 64..127, samplers 128..159, SRT 160), while the
	// SPIRV-Cross fallback keeps its historical overlapping layout
	// (vertex base 0, pixel base 55).
	uint64_t vs_args[256] = {0};
	uint64_t ps_args[256] = {0};

	// A zero-sized guest buffer has no accessible bytes. The backing pointer is
	// used only by checked buffer loads and the optional RECTLIST argument ABI.
	static id<MTLBuffer> s_default_buf = nil;
	static dispatch_once_t s_init_once;
	dispatch_once(&s_init_once, ^{
		s_default_buf = KytyNewBuffer(MetalContext::Instance().GetDevice(),65536,MTLResourceStorageModeShared,__LINE__);
		if (s_default_buf == nil) RejectMetalState("null-buffer ABI allocation failed");
		std::memset(s_default_buf.contents,0,65536);
	});
	const uint64_t def_buf_addr = s_default_buf.gpuAddress;


	// CPU detile for the standard RDNA2 block layouts (4 KB and 64 KB). The GPU
	// tiler in the Vulkan backend uses the same swizzle; sampling tiled guest memory
	// as a linear texture produces the striped garbage seen before.



	const auto BindMslStage = [&](uint64_t* args, const ShaderStageRuntime& stage,
	                              const ShaderVertexInputBuffer* vertex_buffers, int vertex_count) {
		if (stage.program != nullptr) {
			for (const auto& binding: stage.program->bindings.descriptors) {
				if (binding.kind == ShaderRecompiler::IR::DescriptorBindingKind::BdaPagetable) {
					args[162] = BindGuestAddressSpace();
				}
			}
		}
		const auto BindGuestBuffer = [&](uint32_t resource, uint64_t vaddr, size_t size, size_t snapshot_bytes = 0) {
			if (resource >= 64) RejectMetalState("graphics buffer resource exceeds the native ABI");
			if (vaddr == 0) return;
			TraceFlipOverlap("bind_buf", vaddr, size != 0 ? size : 4096);
			if (bind_trace) {
				char b[160];
				std::snprintf(b, sizeof(b), " %c%u:buf@%llx:%zu", stage_tag, resource,
				              (unsigned long long)vaddr, size);
				bt += b;
			}
			auto alloc = MetalBufferManager::Instance().GetBufferForGuestRange(
			    vaddr, size != 0 ? size : 4096);
			if (!alloc.IsValid()) RejectMetalState("unmapped graphics buffer range: address=" + std::to_string(vaddr) + " size=" + std::to_string(size));
			// Opt-in evidence for the solid red UI draw, before the CPU arena snapshot.
			// Record guest descriptors and backing bytes; never alter the draw's data.
			FILE* red_probe = nullptr;
			std::array<uint8_t, 64> probe_guest {};
			static const char* s_red_probe_dir = KYTY_ENV("KYTY_UI_RED_PROBE_DIR");
			if (s_red_probe_dir && args == vs_args && resource == 2 && stage.program &&
			    stage.program->shader_hash == 0xbc0210100fe76732ull && alloc.buffer &&
			    alloc.offset + probe_guest.size() <= alloc.buffer.length && [alloc.buffer contents]) {
				uint32_t color = 0;
				std::memcpy(&color, static_cast<const uint8_t*>([alloc.buffer contents]) + alloc.offset, sizeof(color));
				static uint32_t s_red_probe_count = 0;
				if (color == 0x990000ff && s_red_probe_count < 24) {
					char path[1024];
					std::snprintf(path, sizeof(path), "%s/red-%02u.txt", s_red_probe_dir, s_red_probe_count++);
					red_probe = std::fopen(path, "w");
					if (red_probe) {
						const bool ok = Libs::LibKernel::Memory::TryReadBacking(vaddr, probe_guest.data(), probe_guest.size());
						std::fprintf(red_probe, "vaddr=%llx size=%zu snapshot=%zu backing_ok=%d rt=%llx\nguest:",
						             (unsigned long long)vaddr, size, snapshot_bytes, ok, (unsigned long long)s_last_rt_addr.load());
						for (auto byte: probe_guest) std::fprintf(red_probe, " %02x", byte);
						std::fprintf(red_probe, "\noriginal:");
						const auto* original = static_cast<const uint8_t*>([alloc.buffer contents]) + alloc.offset;
						for (size_t n = 0; n < probe_guest.size(); ++n) std::fprintf(red_probe, " %02x", original[n]);
						if (stage.resources) {
							for (size_t b = 0; b < stage.resources->buffers.size(); ++b) {
								const auto& words = stage.resources->buffers[b].dwords;
								const uint64_t addr = (uint64_t(words[1] & 0xffff) << 32) | words[0];
								std::array<uint32_t, 16> data {};
								const bool read = Libs::LibKernel::Memory::TryReadBacking(addr, data.data(), sizeof(data));
								std::fprintf(red_probe, "\nres%zu desc=%08x %08x %08x %08x ok=%d data:", b, words[0], words[1], words[2], words[3], read);
								for (auto word: data) std::fprintf(red_probe, " %08x", word);
							}
						}
					}
				}
			}
			// KYTY_ARENA_RACE=1: for read-only vertex-stage buffers in the CPU arena (0x8190.., past the 16 KiB snapshot), a checksum at
			// encode time and again when the command buffer completes; a difference means the guest CPU rewrote the data while the GPU
			// had not run the draw yet ([ArenaRace]).
			static const bool s_arena_race = KYTY_ENV("KYTY_ARENA_RACE") != nullptr;
			if (s_arena_race && args == vs_args && snapshot_bytes > 16384 && vaddr >= 0x8190000000ull && vaddr < 0x81a0000000ull &&
			    alloc.buffer != nil && alloc.offset < alloc.buffer.length) {
				static void* s_race_cb = nullptr;
				static std::set<uint64_t> s_race_seen;
				id<MTLCommandBuffer> cb = MetalContext::Instance().GetCurrentCommandBuffer();
				if ((__bridge void*)cb != s_race_cb) { s_race_cb = (__bridge void*)cb; s_race_seen.clear(); }
				if (cb != nil && s_race_seen.insert(vaddr).second) {
					const size_t n = std::min<size_t>({snapshot_bytes, alloc.buffer.length - alloc.offset, 512u * 1024u}) / 64;
					const uint8_t* base = static_cast<const uint8_t*>([alloc.buffer contents]) + alloc.offset;
					auto sums = std::make_shared<std::vector<uint64_t>>(n);
					for (size_t c = 0; c < n; ++c) {
						uint64_t h = 1469598103934665603ull;
						for (size_t k = 0; k < 64; k += 8) { uint64_t w; std::memcpy(&w, base + c * 64 + k, 8); h = (h ^ w) * 1099511628211ull; }
						(*sums)[c] = h;
					}
					const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
					id<MTLBuffer> keep = alloc.buffer;
					[cb addCompletedHandler:^(id<MTLCommandBuffer>) {
						const uint8_t* live = static_cast<const uint8_t*>([keep contents]) + (base - static_cast<const uint8_t*>([keep contents]));
						size_t changed = 0, first = SIZE_MAX;
						for (size_t c = 0; c < sums->size(); ++c) {
							uint64_t h = 1469598103934665603ull;
							for (size_t k = 0; k < 64; k += 8) { uint64_t w; std::memcpy(&w, live + c * 64 + k, 8); h = (h ^ w) * 1099511628211ull; }
							if (h != (*sums)[c]) { ++changed; first = std::min(first, c); }
						}
						static std::atomic<uint64_t> s_total {0}, s_racy {0};
						const uint64_t total = ++s_total, racy = s_racy += changed != 0;
						if (changed != 0 && racy <= 40) {
							std::fprintf(stderr, "[ArenaRace] vaddr=0x%llx encoded_flip=%llu done_flip=%llu changed=%zu/%zu chunks first=+0x%zx (racy %llu of %llu)\n",
							             (unsigned long long)vaddr, (unsigned long long)flip,
							             (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed), changed, sums->size(),
							             first * 64, (unsigned long long)racy, (unsigned long long)total);
						} else if (total % 2000 == 0) {
							std::fprintf(stderr, "[ArenaRace] racy %llu of %llu\n", (unsigned long long)racy, (unsigned long long)total);
						}
					}];
				}
			}
			if (snapshot_bytes != 0 && alloc.gpu_addr != 0) {
				alloc = SnapshotBytes(alloc, vaddr, snapshot_bytes);
			}
			if (red_probe) {
				const auto* bound = static_cast<const uint8_t*>([alloc.buffer contents]) + alloc.offset;
				std::fprintf(red_probe, "\nbound:");
				for (size_t n = 0; n < probe_guest.size(); ++n) std::fprintf(red_probe, " %02x", bound[n]);
				std::fprintf(red_probe, "\n");
				std::fclose(red_probe);
			}
			if (alloc.gpu_addr != 0) {
				auto* stage_sizes = args == ps_args ? ps_sizes : vs_sizes;
				const size_t available = alloc.offset < alloc.buffer.length ? alloc.buffer.length - alloc.offset : 0;
				const size_t valid = snapshot_bytes != 0 ? std::min(available, snapshot_bytes) : available;
				stage_sizes[resource] = static_cast<uint32_t>(std::min<size_t>(stage_sizes[resource], valid));
				// The raw GPU pointer is stored in the argument buffer, so Metal does not see this
				// buffer as referenced: make it resident or the GPU reads zeros through it.
				MetalDrawExecutor::Instance().UseResource(alloc.buffer, MTLResourceUsageRead);
				args[resource] = alloc.gpu_addr;
				if (KYTY_ENV("KYTY_MTL_FAULT_INFO") != nullptr) {
					static std::atomic<uint32_t> s_drawbuf_log {0};
					static const uint64_t s_drawbuf_hash = KYTY_ENV("KYTY_PROBE_HASH") ? std::strtoull(KYTY_ENV("KYTY_PROBE_HASH"), nullptr, 0) : 0ull;
					const bool hash_match = s_drawbuf_hash != 0 && stage.program && stage.program->shader_hash == s_drawbuf_hash;
					if ((hash_match ? (s_drawbuf_log.fetch_add(1) % 50 == 0) : (s_drawbuf_log.fetch_add(1) < 600)) && alloc.buffer != nil) {
						const uint32_t* dp = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>([alloc.buffer contents]) + alloc.offset);
						if (hash_match && args != ps_args && resource == 0) {
							std::fprintf(stderr, "[VBDump] vaddr=0x%llx:", (unsigned long long)vaddr);
							for (int w = 0; w < 36; ++w) std::fprintf(stderr, " %g", *reinterpret_cast<const float*>(&dp[w]));
							std::fprintf(stderr, "\n");
						}
						std::fprintf(stderr, "[DrawBuf] stage=%c res=%u vaddr=0x%llx size=%zu dw0=0x%08x dw1=0x%08x dw2=0x%08x dw3=0x%08x hash=0x%llx\n",
						             args == ps_args ? 'P' : 'V', resource, (unsigned long long)vaddr, size, dp[0], dp[1], dp[2], dp[3],
						             (unsigned long long)(stage.program ? stage.program->shader_hash : 0));
					}
				}
				(args == ps_args ? s_ps_caps : s_vs_caps).push_back({resource, alloc.buffer, alloc.offset, nil, nil});
				if (args == ps_args && PassTraceActive() && s_trace_draw_bufs.size() < 16) s_trace_draw_bufs.push_back({resource, alloc.buffer, alloc.offset, stage_sizes[resource], vaddr});
				if (args == vs_args && PassTraceActive() && s_trace_vs_bufs.size() < 16) s_trace_vs_bufs.push_back({resource, alloc.buffer, alloc.offset, stage_sizes[resource], vaddr});
				if (PositionWatchEnabled() && args == vs_args && stage.program && resource < stage.program->info.buffers.size() &&
				    stage.program->info.buffers[resource].descriptor_format == Prospero::BufferFormat::k32_32_32Float)
					s_position_watch_bufs.push_back({resource, alloc.buffer, alloc.offset, stage_sizes[resource], vaddr});
				if (KYTY_ENV("KYTY_TRACE_VB") != nullptr && args == vs_args && alloc.buffer != nil &&
				    Libs::Graphics::TraceWindow::Active()) {
					static std::atomic<uint32_t> s_vb2 {0};
					if (s_vb2.fetch_add(1) < 12) {
						const size_t nb = std::min<size_t>(size != 0 ? size : 4096, 256);
						std::vector<uint8_t> gm(nb, 0);
						const bool gok = Libs::LibKernel::Memory::TryReadBacking(vaddr, gm.data(), nb);
						const uint8_t* bp = static_cast<const uint8_t*>([alloc.buffer contents]) + alloc.offset;
						size_t dif = 0;
						for (size_t b = 0; b < nb; ++b) dif += gm[b] != bp[b];
						const uint32_t* g32 = reinterpret_cast<const uint32_t*>(gm.data());
						const uint32_t* b32 = reinterpret_cast<const uint32_t*>(bp);
						std::fprintf(stderr, "[VB2] res=%u vaddr=0x%llx size=%zu off=%llu backing_ok=%d diff=%zu/%zu guest=%08x %08x %08x %08x bound=%08x %08x %08x %08x\n",
						             resource, (unsigned long long)vaddr, size, (unsigned long long)alloc.offset, gok ? 1 : 0, dif, nb,
						             g32[0], g32[1], g32[2], g32[3], b32[0], b32[1], b32[2], b32[3]);
					}
				}
			} else RejectMetalState("graphics buffer has no GPU address: " + std::to_string(vaddr));
		};
		for (int i = 0; vertex_buffers != nullptr && i < vertex_count && i < 64; ++i) {
			// Vertex data written by the CPU into a recycled ring is copied at encode time, like constants.
			const size_t vb_bytes = static_cast<size_t>(vertex_buffers[i].stride) * vertex_buffers[i].num_records;
			BindGuestBuffer(static_cast<uint32_t>(i), vertex_buffers[i].addr, vb_bytes, vb_bytes);
		}
		if (stage.resources) {
			const auto& snap = *stage.resources;
			for (size_t b = 0; b < snap.buffers.size() && b < 64; ++b) {
				const auto& desc = snap.buffers[b];
				const uint64_t addr =
				    (static_cast<uint64_t>(desc.dwords[1] & 0xFFFF) << 32) | desc.dwords[0];
				size_t size = 0;
				if (stage.program && b < stage.program->info.buffers.size()) {
					size = stage.program->info.buffers[b].max_byte_extent;
				}
				size_t snapshot_bytes = 0;
				if (stage.program && b < stage.program->info.buffers.size() && !stage.program->info.buffers[b].written) {
					snapshot_bytes = std::max<size_t>(size, VsharpBytes(desc.dwords));
				}
				// The guest-memory view must cover the whole V# range. max_byte_extent is only the static offset + width of the
				// accesses: a dynamically indexed buffer (the bone matrix pool of the skinned players, 25 MB) got a one-page view,
				// sizes[] was clamped to it and every matrix past the first page read as zeros - vertices at w = 0, the long dark
				// "spikes" and stretched objects across the screen (whether a bigger view of the page was cached decided each run).
				const size_t view_bytes = std::max<size_t>(size, std::min<size_t>(VsharpBytes(desc.dwords), 256u << 20));
				BindGuestBuffer(static_cast<uint32_t>(b), addr, view_bytes, snapshot_bytes);
			}
			{
				static std::atomic<uint32_t> s_srt_log {0};
				static std::atomic<uint32_t> s_nonempty_log {0};
				const bool nonempty = !snap.buffers.empty() || !snap.images.empty();
				if (s_srt_log.fetch_add(1) < 8 || (nonempty && s_nonempty_log.fetch_add(1) < 8)) {
					std::fprintf(stderr, "[Bind] MSL stage=%u srt=%zu bufs=%zu imgs=%zu\n",
					             static_cast<uint32_t>(stage.program ? stage.program->stage : ShaderType::Unknown),
					             snap.flattened_srt.size(), snap.buffers.size(), snap.images.size());
				}
			}
			{
				// KYTY_LOG_DESC=1 + KYTY_PROBE_HASH: CPU-side values behind the probed shader's
				// descriptors (first 8 dwords of each guest buffer, SRT words used for UV/colour).
				static const bool s_log_vals = KYTY_ENV("KYTY_LOG_DESC") != nullptr;
				static const uint64_t s_vals_hash = KYTY_ENV("KYTY_PROBE_HASH") ? std::strtoull(KYTY_ENV("KYTY_PROBE_HASH"), nullptr, 0) : 0ull;
				static std::atomic<uint32_t> s_vals_n {0};
				if (s_log_vals && stage.program && stage.program->shader_hash == s_vals_hash && s_vals_n.fetch_add(1) < 2) {
					for (size_t b = 0; b < snap.buffers.size() && b < 4; ++b) {
						const auto& d = snap.buffers[b];
						const uint64_t a = (static_cast<uint64_t>(d.dwords[1] & 0xFFFF) << 32) | d.dwords[0];
						uint32_t w[8] = {};
						const bool ok = a != 0 && Libs::LibKernel::Memory::TryReadBacking(a, w, sizeof(w));
						std::fprintf(stderr, "[Vals] buf%zu addr=0x%llx read=%d dw=%08x %08x %08x %08x %08x %08x %08x %08x\n", b,
						             (unsigned long long)a, ok ? 1 : 0, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
					}
					if (!snap.flattened_srt.empty()) {
						std::fprintf(stderr, "[Vals] srt n=%zu [24]=%08x [25]=%08x [27]=%08x [28]=%08x\n", snap.flattened_srt.size(),
						             snap.flattened_srt.size() > 24 ? snap.flattened_srt[24] : 0u,
						             snap.flattened_srt.size() > 25 ? snap.flattened_srt[25] : 0u,
						             snap.flattened_srt.size() > 27 ? snap.flattened_srt[27] : 0u,
						             snap.flattened_srt.size() > 28 ? snap.flattened_srt[28] : 0u);
					}
				}
			}
			{
				// KYTY_SRT_SIZE_LOG=1: the flattened SRT size of every shader with more than 256 entries (once per shader and size), to
				// compare with the highest immediate flattened_srt index of its MSL dump.
				static const bool s_srt_size_log = KYTY_ENV("KYTY_SRT_SIZE_LOG") != nullptr;
				static std::mutex s_srt_size_mutex;
				static std::set<std::pair<uint64_t, size_t>> s_srt_size_seen;
				if (s_srt_size_log && stage.program && snap.flattened_srt.size() > 200) {
					std::lock_guard<std::mutex> lock(s_srt_size_mutex);
					if (s_srt_size_seen.insert({stage.program->guest_hash, snap.flattened_srt.size()}).second) {
						const auto at = [&](size_t i) { return i < snap.flattened_srt.size() ? snap.flattened_srt[i] : 0xdeadbeefu; };
						std::fprintf(stderr, "[SrtSize] guest=0x%016llx key=0x%016llx n=%zu [255]=%08x [257]=%08x [311]=%08x [315]=%08x\n",
						             (unsigned long long)stage.program->guest_hash, (unsigned long long)stage.program->shader_hash,
						             snap.flattened_srt.size(), at(255), at(257), at(311), at(315));
					}
				}
			}
			if (!snap.flattened_srt.empty()) {
				// Pad and zero the tail: a stray SRT index in a fallback shader must not
				// walk off the allocation into unmapped pages.
				const size_t bytes = snap.flattened_srt.size() * sizeof(uint32_t);
				const size_t padded = bytes + 1024;
				auto alloc = MetalBufferManager::Instance().AllocateDynamic(padded, 16);
				if (alloc.IsValid()) {
					std::memset(static_cast<uint8_t*>(alloc.cpu_addr) + bytes, 0, 1024);
					std::memcpy(alloc.cpu_addr, snap.flattened_srt.data(), bytes);
					// Raw GPU pointer in the argument buffer: make the SRT buffer resident.
					MetalDrawExecutor::Instance().UseResource(alloc.buffer, MTLResourceUsageRead);
					args[160] = alloc.gpu_addr;
				}
			}
			for (size_t i = 0; i < snap.images.size() && (64 + i) < 128; ++i) {
				const auto& desc = snap.images[i];
				ShaderTextureResource tex {};
				for (size_t f = 0; f < 8; ++f) {
					tex.fields[f] = desc.dwords[f];
				}
				const uint64_t addr = tex.Base40();
				const uint32_t w    = static_cast<uint32_t>(tex.Width5()) + 1u;
				const uint32_t h    = static_cast<uint32_t>(tex.Height5()) + 1u;
				if (KYTY_ENV("KYTY_LOG_TEX") != nullptr) {
					static std::atomic<uint32_t> s_logtex {0};
					if (s_logtex.fetch_add(1) % 400 == 0) {
						const double guest_nz = TraceNonZero(addr, 65536);
						std::fprintf(stderr, "[GfxTex] guest_nz=%.1f%% stage=%c idx=%zu addr=0x%llx %ux%u fmt=%u dst_sel=%u%u%u%u tile=%u flip=%llu\n", guest_nz,
						             args == ps_args ? 'P' : 'V', i, (unsigned long long)addr, w, h, (unsigned)tex.Format(),
						             tex.DstSelX(), tex.DstSelY(), tex.DstSelZ(), tex.DstSelW(), (unsigned)tex.TileMode(),
						             (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load());
					}
				}
				id<MTLTexture> base_image = CanonicalMipRead(tex,w,h,addr);
				if (base_image == nil && stage.program && i < stage.program->info.images.size()) {
					base_image = DepthWriterForLayeredRead(stage.program->info.images[i], tex, addr, w, h);
					if (base_image == nil) base_image = LayeredImageFor(stage.program->info.images[i], tex, addr, w, h, false);
				}
				if (base_image == nil) base_image = GraphicsImageFor(tex, w, h, addr);
				if (id<MTLTexture> cleared = SampledMetaClear(tex, addr, base_image)) base_image = cleared;
				VerifyBoundGuestTex(tex, addr, w, h, base_image);
				LogSmallBc4(addr, base_image, stage.program != nullptr ? stage.program->guest_hash : 0, args == ps_args);
				// KYTY_WATCH_RT also lists the sampled images of that width range (same once-per-300-flips rule).
				if (const char* wr = KYTY_ENV("KYTY_WATCH_RT"); wr != nullptr && args == ps_args) {
					unsigned lo = 0, hi = 0;
					std::sscanf(wr, "%u,%u", &lo, &hi);
					if (w >= lo && w <= hi) {
						static std::mutex s_wt_mutex;
						static std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint64_t>> s_wt_seen;
						const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
						std::lock_guard<std::mutex> wt_lock(s_wt_mutex);
						if (s_wt_seen.insert({addr, w * 65536u + h, static_cast<uint32_t>(tex.Format()), flip / 300}).second) {
							std::fprintf(stderr, "[WatchTex] flip=%llu slot=%zu 0x%llx %ux%u tfmt=%u\n", (unsigned long long)flip, i,
							             (unsigned long long)addr, w, h, (unsigned)tex.Format());
						}
					}
				}
				// KYTY_LOG_TEX_UNIQ=<lo>,<hi>: every distinct sampled image in that flip range, with the non-zero ratio of the
				// bound Metal object (finds images the game samples that nobody filled).
				if (const char* uq = KYTY_ENV("KYTY_LOG_TEX_UNIQ"); uq != nullptr && base_image != nil) {
					unsigned lo = 0, hi = 0;
					std::sscanf(uq, "%u,%u", &lo, &hi);
					const uint64_t fl = Libs::Graphics::TraceWindow::g_flip.load();
					static std::set<std::tuple<uint64_t, uint32_t, uint64_t>> s_seen;
					static std::mutex s_seen_mutex;
					if (fl >= lo && fl <= hi) {
						std::lock_guard<std::mutex> lk(s_seen_mutex);
						if (s_seen.insert({addr, w * 65536u + h, fl / 50}).second) {
							double nzr = -1.0;
							const size_t bpp = BytesPerPixel(base_image.pixelFormat);
							if (bpp != 0 && base_image.storageMode == MTLStorageModeShared && base_image.textureType == MTLTextureType2D &&
							    base_image.width * base_image.height <= 4096u * 4096u) {
								std::vector<uint8_t> b(base_image.width * base_image.height * bpp);
								[base_image getBytes:b.data() bytesPerRow:base_image.width * bpp
								          fromRegion:MTLRegionMake2D(0, 0, base_image.width, base_image.height) mipmapLevel:0];
								size_t nz = 0;
								for (uint8_t v: b) nz += v != 0;
								nzr = 100.0 * nz / b.size();
							}
							{
								std::lock_guard<std::mutex> rl(s_render_target_mutex);
								const auto rit = s_rt_by_addr.find(addr);
								if (rit != s_rt_by_addr.end()) {
									for (const auto& [rf, rt]: rit->second) {
										std::fprintf(stderr, "[TexUniqRt] addr=0x%llx rt=%ux%u fmt=%lu seq=%llu\n", (unsigned long long)addr, rt.width, rt.height,
										             rt.tex ? (unsigned long)rt.tex.pixelFormat : 0ul, (unsigned long long)rt.sequence);
									}
								}
							}
							if (KYTY_ENV("KYTY_DUMP_TEXUNIQ") != nullptr && base_image.pixelFormat == MTLPixelFormatRGBA8Unorm && nzr >= 0.0 &&
							    base_image.width >= 1024) {
								static std::atomic<uint32_t> s_rgba_dumps {0};
								if (s_rgba_dumps.fetch_add(1) < 12) {
									std::vector<uint8_t> pix(base_image.width * base_image.height * 4);
									[base_image getBytes:pix.data() bytesPerRow:base_image.width * 4
									          fromRegion:MTLRegionMake2D(0, 0, base_image.width, base_image.height) mipmapLevel:0];
									char path[160];
									std::snprintf(path, sizeof(path), "/tmp/fix6/texrgba_%llu_%lux%lu_%llx.ppm", (unsigned long long)fl,
									              (unsigned long)base_image.width, (unsigned long)base_image.height, (unsigned long long)addr);
									if (FILE* f = std::fopen(path, "wb")) {
										std::fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)base_image.width, (unsigned long)base_image.height);
										for (size_t i = 0; i < pix.size(); i += 4) std::fwrite(&pix[i], 1, 3, f);
										std::fclose(f);
									}
								}
							}
							static std::atomic<uint32_t> s_texuniq_dumps {0};
							if (KYTY_ENV("KYTY_DUMP_TEXUNIQ") != nullptr && base_image.pixelFormat == MTLPixelFormatR8Unorm && nzr >= 0.0 &&
							    base_image.width >= 256 && s_texuniq_dumps.fetch_add(1) < 24) {
								// KYTY_DUMP_TEXUNIQ=1 (with KYTY_LOG_TEX_UNIQ): R8 images are written as /tmp/fix6/tex_<flip>_<w>x<h>_<addr>.pgm.
								std::vector<uint8_t> pix(base_image.width * base_image.height);
								[base_image getBytes:pix.data() bytesPerRow:base_image.width
								          fromRegion:MTLRegionMake2D(0, 0, base_image.width, base_image.height) mipmapLevel:0];
								char path[160];
								std::snprintf(path, sizeof(path), "/tmp/fix6/tex_%llu_%lux%lu_%llx.pgm", (unsigned long long)fl,
								              (unsigned long)base_image.width, (unsigned long)base_image.height, (unsigned long long)addr);
								if (FILE* f = std::fopen(path, "wb")) {
									std::fprintf(f, "P5\n%lu %lu\n255\n", (unsigned long)base_image.width, (unsigned long)base_image.height);
									std::fwrite(pix.data(), 1, pix.size(), f);
									std::fclose(f);
								}
							}
							std::fprintf(stderr, "[TexUniq] hash=%016llx flip=%llu stage=%c idx=%zu addr=0x%llx %ux%u tfmt=%u tile=%u dst_sel=%u%u%u%u mtl=%lu nz=%.2f%% guest_nz=%.2f%% d4=%08x d5=%08x\n",
							             (unsigned long long)(stage.program ? stage.program->shader_hash : 0), (unsigned long long)fl,
							             args == ps_args ? 'P' : 'V', i, (unsigned long long)addr, w, h, (unsigned)tex.Format(),
							             (unsigned)tex.TileMode(), tex.DstSelX(), tex.DstSelY(), tex.DstSelZ(), tex.DstSelW(),
							             (unsigned long)base_image.pixelFormat, nzr, TraceNonZero(addr, 8192), tex.fields[4], tex.fields[5]);
						}
					}
				}
				bool storage_image = false;
				if (stage.program) for (const auto& binding : stage.program->bindings.descriptors) {
					const auto kind = static_cast<uint32_t>(binding.kind);
					if (kind >= ShaderRecompiler::IR::FirstStorageImageBinding &&
					    kind < static_cast<uint32_t>(ShaderRecompiler::IR::DescriptorBindingKind::Samplers) &&
					    std::find(binding.resources.begin(), binding.resources.end(), i) != binding.resources.end()) storage_image = true;
				}
				PhysicalBackingSnapshot storage_backing;
				if (storage_image) {
					auto canonical=CanonicalMipImage(tex,w,h,addr,false);
					if(canonical.image) {
						if(IsCompressedFormat(canonical.image.pixelFormat))RejectMetalState("compressed storage image requires an uncompressed block descriptor");
						base_image=canonical.image;
						storage_backing=canonical.physical;
						if(base_image.mipmapLevelCount>1 && tex.BaseLevel()>0)
							base_image=MipRangeView(base_image,tex.BaseLevel(),tex.LastLevel()-tex.BaseLevel()+1u);
						s_graphics_image_writebacks.push_back({canonical.image,canonical.writeback});
					}
				}
				if (id<MTLTexture> texture = storage_image ? base_image : ApplyDstSel(base_image, tex)) {
					args[64 + i] = [texture gpuResourceID]._impl;
					(args == ps_args ? ps_sizes : vs_sizes)[ShaderRecompiler::Msl::ImageMetadataWordBase + i] =
					    SupportsNativeDepthCompare(texture.pixelFormat) ? 1u : 0u;
					(args == ps_args ? s_ps_caps : s_vs_caps).push_back({static_cast<uint32_t>(64 + i), nil, 0, texture, nil});
					if (args == ps_args && PassTraceActive() && s_trace_draw_inputs.size() < 64) s_trace_draw_inputs.push_back({texture, static_cast<uint32_t>(64 + i), addr, w, h, static_cast<uint32_t>(tex.Format()), static_cast<uint32_t>(tex.TileMode()),
						                                std::max<uint32_t>(tex.LastLevel(), tex.MaxMip()) + 1u});
					MetalDrawExecutor::Instance().UseResource(texture, storage_image ? (MTLResourceUsageRead | MTLResourceUsageWrite) : MTLResourceUsageRead);
					if (storage_image) RecordComputeStorage(addr, base_image, tex.TileMode(), true,storage_backing.Valid()?&storage_backing:nullptr);
					// Presenter lookup by guest address (the flip buffer is bound as an image by its writer). The image
					// itself, not its DST_SEL view: compute later binds the registered object as a writable storage image.
					RegisterComputeWrittenImage(addr, (__bridge void*)base_image);
				}
			}
		}
		for (uint32_t i = 0; i < 64; ++i) {
			if (args[64 + i] == 0) {
				// Only an explicitly null guest descriptor may leave a declared image handle null.
				if (stage.program != nullptr && i < stage.program->info.images.size()) {
					RequireNullImageDescriptor(stage, i);
					// KYTY_LOG_UNBOUND=1: what the guest descriptor of such a slot holds (null T#, an unsupported type or format, or an
					// address with nothing behind it), once per shader slot.
					static const bool s_log_unbound = KYTY_ENV("KYTY_LOG_UNBOUND") != nullptr;
					if (s_log_unbound) {
						static std::mutex s_ub_mutex;
						static std::set<std::pair<uint64_t, uint32_t>> s_ub_seen;
						std::lock_guard<std::mutex> ub_lock(s_ub_mutex);
						if (s_ub_seen.insert({stage.program->shader_hash, i}).second) {
							ShaderTextureResource tex {};
							const bool has_desc = stage.resources != nullptr && i < stage.resources->images.size();
							if (has_desc) for (size_t f = 0; f < 8; ++f) tex.fields[f] = stage.resources->images[i].dwords[f];
							std::fprintf(stderr, "[Unbound] %c hash=%016llx slot=%u desc=%d null=%d dim=%d addr=0x%llx %ux%u fmt=%u type=%u tile=%u lvls=%u-%u dst_sel=%u%u%u%u d=%08x %08x %08x %08x\n",
							             args == ps_args ? 'P' : 'V', (unsigned long long)stage.program->shader_hash, i, has_desc ? 1 : 0,
							             tex.IsNull() ? 1 : 0,
							             static_cast<int>(stage.program->info.images[i].dimension), (unsigned long long)tex.Base40(),
							             static_cast<unsigned>(tex.Width5()) + 1u, static_cast<unsigned>(tex.Height5()) + 1u, (unsigned)tex.Format(),
							             (unsigned)tex.Type(), (unsigned)tex.TileMode(), (unsigned)tex.BaseLevel(), (unsigned)tex.LastLevel(),
							             tex.DstSelX(), tex.DstSelY(), tex.DstSelZ(), tex.DstSelW(), tex.fields[0], tex.fields[1], tex.fields[2], tex.fields[3]);
						}
					}
				}
			}
		}
		for (uint32_t i = 0; stage.program && i < stage.program->info.samplers.size(); ++i) {
			id<MTLSamplerState> sampler = StageSampler(stage, i);
			if (sampler == nil) RejectMetalState("missing graphics sampler descriptor " + std::to_string(i));
			args[128 + i] = [sampler gpuResourceID]._impl;
			(args == ps_args ? ps_sizes : vs_sizes)[64 + i] = StageSamplerCompare(stage, i);
			StoreSamplerDescriptor(stage, i, args == ps_args ? ps_sizes : vs_sizes);
			if (StageHasPointFetchSampler(stage, i)) {
				id<MTLSamplerState> point = StageSampler(stage, i, true);
				if (point != nil) {
					const auto index = ShaderRecompiler::Msl::ComparisonPointSamplerBase + i;
					args[index] = point.gpuResourceID._impl;
					(args == ps_args ? s_ps_caps : s_vs_caps).push_back({index, nil, 0, nil, point});
				}
			}
			(args == ps_args ? s_ps_caps : s_vs_caps).push_back({128u + i, nil, 0, nil, sampler});
		}
	};

	const auto GuestGpuAddress = [](uint64_t vaddr, size_t size) -> uint64_t {
		if (vaddr == 0) {
			return 0;
		}
		auto alloc = MetalBufferManager::Instance().GetBufferForGuestRange(
		    vaddr, size != 0 ? size : 4096);
		if (alloc.IsValid() && alloc.buffer) {
			MetalDrawExecutor::Instance().UseResource(alloc.buffer, MTLResourceUsageRead);
		}
		return alloc.gpu_addr;
	};

	// Image descriptors are declared as arrays at NativeBinding(stage, kind); element
	// ids are binding + position in that array. The snapshot's image list is indexed by
	// resource, so the slot cannot be derived from the resource index alone.
	const auto ImageBindingSlot = [](const ShaderStageRuntime& stage, uint32_t resource,
	                                 uint32_t& slot) -> bool {
		if (stage.program == nullptr) {
			return false;
		}
		for (const auto& binding: stage.program->bindings.descriptors) {
			const auto kind = static_cast<uint32_t>(binding.kind);
			if (kind < ShaderRecompiler::IR::FirstImageBinding ||
			    kind >= static_cast<uint32_t>(ShaderRecompiler::IR::DescriptorBindingKind::Samplers)) {
				continue;
			}
			const auto it = std::find(binding.resources.begin(), binding.resources.end(), resource);
			if (it == binding.resources.end()) {
				continue;
			}
			static const bool s_spvc_abi_slot = KYTY_ENV("KYTY_SPVC_ABI") != nullptr;
			if (s_spvc_abi_slot) {
				const auto layout = Libs::Graphics::Metal::ComputeSpvcAbiLayout(stage.program->bindings, stage.program->stage);
				slot = layout.BaseOf(binding.kind) + static_cast<uint32_t>(it - binding.resources.begin());
				return true;
			}
			slot = ShaderRecompiler::IR::NativeBinding(stage.program->stage, binding.kind) +
			       static_cast<uint32_t>(it - binding.resources.begin());
			return true;
		}
		return false;
	};

	const auto BindLegacyStage = [&](uint64_t* args, uint32_t base, uint32_t image_base,
	                                 const ShaderStageRuntime& stage,
	                                 const ShaderVertexInputBuffer* vertex_buffers,
	                                 int vertex_count) {
		// KYTY_SPVC_ABI: resource ids come from the per-shader layout, not the legacy offsets.
		static const bool s_spvc_abi_bind = KYTY_ENV("KYTY_SPVC_ABI") != nullptr;
		const bool abi_on = s_spvc_abi_bind && stage.program != nullptr;
		const auto abi_layout = abi_on ? Libs::Graphics::Metal::ComputeSpvcAbiLayout(stage.program->bindings, stage.program->stage)
		                               : Libs::Graphics::Metal::SpvcAbiLayout {};
		using DescKind = ShaderRecompiler::IR::DescriptorBindingKind;
		// Kinds the shader does not declare map to 256 (out of range), so every write below is skipped.
		const auto AbiBaseOr256 = [&](DescKind kind) {
			const uint32_t v = abi_layout.BaseOf(kind);
			return v == Libs::Graphics::Metal::SpvcAbiLayout::kNotPresent ? 256u : v;
		};
		const uint32_t abi_buf  = abi_on ? AbiBaseOr256(DescKind::Buffers) : base;
		const uint32_t abi_srt  = abi_on ? AbiBaseOr256(DescKind::FlattenedSrt) : base + 53;
		const uint32_t abi_samp = abi_on ? AbiBaseOr256(DescKind::Samplers) : base + 49;
		for (int i = 0; vertex_buffers != nullptr && i < vertex_count && i < 5; ++i) {
			const auto ga = GuestGpuAllocation(
			    vertex_buffers[i].addr,
			    static_cast<size_t>(vertex_buffers[i].stride) * vertex_buffers[i].num_records);
			if (ga.gpu_addr != 0) {				if (KYTY_ENV("KYTY_TRACE_VB") != nullptr && args == vs_args && ga.buffer != nil &&
				    Libs::Graphics::TraceWindow::Active()) {
					static std::atomic<uint32_t> s_vb_log {0};
					if (s_vb_log.fetch_add(1) < 12) {
						const uint32_t stride = vertex_buffers[i].stride;
						const uint64_t vaddr = vertex_buffers[i].addr;
						const size_t nbytes = static_cast<size_t>(stride) * 4;
						std::vector<uint8_t> gmem(nbytes, 0);
						const bool gok = Libs::LibKernel::Memory::TryReadBacking(vaddr, gmem.data(), nbytes);
						const uint8_t* bptr = static_cast<const uint8_t*>([ga.buffer contents]) + ga.offset;
						size_t diff = 0;
						for (size_t b = 0; b < nbytes; ++b) diff += gmem[b] != bptr[b];
						std::fprintf(stderr, "[VB] vaddr=0x%llx stride=%u num=%u off=%llu backing_ok=%d bytes_diff=%zu/%zu v0=%08x %08x %08x %08x v1=%08x %08x\n",
						             (unsigned long long)vaddr, stride, vertex_buffers[i].num_records,
						             (unsigned long long)ga.offset, gok ? 1 : 0, diff, nbytes,
						             ((const uint32_t*)gmem.data())[0], ((const uint32_t*)gmem.data())[1],
						             ((const uint32_t*)gmem.data())[2], ((const uint32_t*)gmem.data())[3],
						             stride >= 8 ? ((const uint32_t*)gmem.data())[stride / 4] : 0u,
						             stride >= 8 ? ((const uint32_t*)gmem.data())[stride / 4 + 1] : 0u);
					}
				}
				if (abi_buf + i >= 256u) continue;
				args[abi_buf + i] = ga.gpu_addr;
				(args == ps_args ? s_ps_caps : s_vs_caps).push_back({abi_buf + i, ga.buffer, ga.offset, nil, nil});
			}
		}
		if (stage.resources) {
			const auto& snap = *stage.resources;
			for (size_t b = 0; b < snap.buffers.size() && b < 5; ++b) {
				if (abi_buf + b < 256u && args[abi_buf + b] != 0) {
					continue;
				}
				const auto& desc = snap.buffers[b];
				const uint64_t addr =
				    (static_cast<uint64_t>(desc.dwords[1] & 0xFFFF) << 32) | desc.dwords[0];
				size_t size = 0;
				if (stage.program && b < stage.program->info.buffers.size()) {
					size = stage.program->info.buffers[b].max_byte_extent;
				}
				const auto ga = GuestGpuAllocation(addr, size);
				if (KYTY_ENV("KYTY_TRACE_VB") != nullptr && args == vs_args && ga.buffer != nil &&
				    Libs::Graphics::TraceWindow::Active()) {
					static std::atomic<uint32_t> s_vb3 {0};
					if (s_vb3.fetch_add(1) < 12) {
						const size_t nb = std::min<size_t>(size != 0 ? size : 4096, 256);
						std::vector<uint8_t> gm(nb, 0);
						const bool gok = Libs::LibKernel::Memory::TryReadBacking(addr, gm.data(), nb);
						const uint8_t* bp = static_cast<const uint8_t*>([ga.buffer contents]) + ga.offset;
						size_t dif = 0;
						for (size_t q = 0; q < nb; ++q) dif += gm[q] != bp[q];
						const uint32_t* g32 = reinterpret_cast<const uint32_t*>(gm.data());
						const uint32_t* b32 = reinterpret_cast<const uint32_t*>(bp);
						std::fprintf(stderr, "[VB2] slot=%u vaddr=0x%llx size=%zu off=%llu backing_ok=%d diff=%zu/%zu guest=%08x %08x %08x %08x bound=%08x %08x %08x %08x\n",
						             static_cast<uint32_t>(abi_buf + b), (unsigned long long)addr, size, (unsigned long long)ga.offset,
						             gok ? 1 : 0, dif, nb, g32[0], g32[1], g32[2], g32[3], b32[0], b32[1], b32[2], b32[3]);
					}
				}
				if (ga.gpu_addr != 0) {
					if (abi_buf + b < 256u) args[abi_buf + b] = ga.gpu_addr;
					if (abi_buf + b < 256u) (args == ps_args ? s_ps_caps : s_vs_caps).push_back({static_cast<uint32_t>(abi_buf + b), ga.buffer, ga.offset & ~static_cast<NSUInteger>(255), nil, nil});
				}
			}
			{
				static std::atomic<uint32_t> s_legacy_log {0};
				if (s_legacy_log.fetch_add(1) < 8) {
					std::fprintf(stderr,
					             "[Bind] legacy base=%u stage=%u srt=%zu bufs=%zu imgs=%zu buf0=0x%llx\n",
					             base,
					             static_cast<uint32_t>(stage.program ? stage.program->stage
					                                                 : ShaderType::Unknown),
					             snap.flattened_srt.size(), snap.buffers.size(), snap.images.size(),
					             static_cast<unsigned long long>(args[base]));
				}
			}
			if (!snap.flattened_srt.empty()) {
				static std::atomic<uint32_t> s_srt_stats {0};
				if (s_srt_stats.fetch_add(1) < 10) {
					size_t non_zero = 0;
					for (const uint32_t word: snap.flattened_srt) {
						if (word != 0) ++non_zero;
					}
					std::fprintf(stderr, "[SrtData] base=%u words=%zu nz=%zu first=0x%08x 0x%08x\n",
					             base, snap.flattened_srt.size(), non_zero,
					             snap.flattened_srt[0],
					             snap.flattened_srt.size() > 1 ? snap.flattened_srt[1] : 0u);
				}
				// The SPIRV-Cross shaders read the SRT through a device pointer, so it
				// must live in Metal-visible memory. Pad and zero the tail so stray
				// indices stay inside the allocation.
				const size_t bytes = snap.flattened_srt.size() * sizeof(uint32_t);
				const size_t padded = bytes + 1024;
				auto alloc = MetalBufferManager::Instance().AllocateDynamic(padded, 16);
				if (alloc.IsValid()) {
					std::memset(static_cast<uint8_t*>(alloc.cpu_addr) + bytes, 0, 1024);
					std::memcpy(alloc.cpu_addr, snap.flattened_srt.data(), bytes);
					if (abi_srt < 256u) args[abi_srt] = alloc.gpu_addr;
					if (abi_srt < 256u) (args == ps_args ? s_ps_caps : s_vs_caps).push_back({abi_srt, alloc.buffer, alloc.offset, nil, nil});
					MetalDrawExecutor::Instance().UseResource(alloc.buffer, MTLResourceUsageRead);
				}
			}
			for (size_t i = 0; i < snap.images.size() && (image_base + i) < base + 49; ++i) {
				const auto& desc = snap.images[i];
				ShaderTextureResource tex {};
				for (size_t f = 0; f < 8; ++f) {
					tex.fields[f] = desc.dwords[f];
				}
				const uint64_t addr = tex.Base40();
				const uint32_t w    = static_cast<uint32_t>(tex.Width5()) + 1u;
				const uint32_t h    = static_cast<uint32_t>(tex.Height5()) + 1u;
				{
					static std::mutex s_fmt_mutex;
					static std::set<uint32_t> s_fmt_seen;
					const uint32_t key = (static_cast<uint32_t>(tex.Format()) << 8u) |
					                     static_cast<uint32_t>(tex.TileMode());
					std::lock_guard<std::mutex> lock(s_fmt_mutex);
					if (s_fmt_seen.size() < 24 && s_fmt_seen.insert(key).second) {
						std::fprintf(stderr, "[TexFmt] format=%u tile=%u (%ux%u addr=0x%llx)\n",
						             static_cast<uint32_t>(tex.Format()),
						             static_cast<uint32_t>(tex.TileMode()), w, h,
						             static_cast<unsigned long long>(addr));
					}
				}
				uint32_t image_slot = image_base + static_cast<uint32_t>(i);
				const bool slot_known = ImageBindingSlot(stage, static_cast<uint32_t>(i), image_slot);
				if (abi_on && (!slot_known || image_slot >= 256u)) {
					// Not declared by the shader's layout: encoding it at a legacy slot would miss the argument encoder's range.
					continue;
				}
				if (id<MTLTexture> texture = ApplyDstSel(GraphicsImageFor(tex, w, h, addr), tex)) {
					args[image_slot] = [texture gpuResourceID]._impl;
					(args == ps_args ? s_ps_caps : s_vs_caps).push_back({image_slot, nil, 0, texture, nil});
					if (image_slot == 58 && stage.program != nullptr && KYTY_ENV("KYTY_READBACK_TEX") != nullptr) {
						static const uint64_t s_rb_hash = [] {
							const char* v = KYTY_ENV("KYTY_PROBE_HASH");
							return v != nullptr ? std::strtoull(v, nullptr, 0) : 0ull;
						}();
						static bool s_rb_done = false;
						if (!s_rb_done && stage.program->shader_hash == s_rb_hash) {
							s_rb_done = true;
							std::fprintf(stderr, "[Readback] slot=58 texture %lux%lu fmt=%lu addr=0x%llx\n",
							             (unsigned long)[texture width], (unsigned long)[texture height],
							             (unsigned long)[texture pixelFormat], (unsigned long long)addr);
							MetalPresenter::Instance().DumpTextureToPpm((__bridge void*)texture, "/tmp/readback_tex58.ppm");
							{
								const uint64_t nbytes = static_cast<uint64_t>([texture width]) * [texture height];
								std::vector<uint8_t> via_backing(nbytes);
								const bool ok = Libs::LibKernel::Memory::TryReadBacking(addr, via_backing.data(), nbytes);
								const auto* direct = reinterpret_cast<const uint8_t*>(addr);
								uint64_t nzb = 0, nzd = 0;
								for (uint64_t i = 0; i < nbytes; ++i) {
									nzb += via_backing[i] != 0;
									nzd += direct[i] != 0;
								}
								std::fprintf(stderr, "[Readback] guest bytes=%llu backing_ok=%d nz_backing=%llu nz_direct=%llu\n",
								             (unsigned long long)nbytes, ok ? 1 : 0, (unsigned long long)nzb, (unsigned long long)nzd);
							}
						}
					}
					MetalDrawExecutor::Instance().UseResource(texture, MTLResourceUsageRead);
					static const bool s_dump_bound = [] {
						const char* value = KYTY_ENV("KYTY_DUMP_BOUND_TEX");
						return value != nullptr && value[0] == '1';
					}();
					static const uint64_t s_dump_bound_after = [] {
						const char* value = KYTY_ENV("KYTY_DUMP_BOUND_TEX_AFTER");
						return value != nullptr ? std::strtoull(value, nullptr, 0) : 100000ull;
					}();
					if (s_dump_bound && base == 55 && MetalDrawSequence() > s_dump_bound_after) {
						static std::atomic<uint32_t> s_dump_tex {0};
						const uint32_t index = s_dump_tex.fetch_add(1);
						if (index < 8) {
							char path[128];
							std::snprintf(path, sizeof(path), "/tmp/tex_%02u_%ux%u.ppm", index, w, h);
							MetalPresenter::Instance().DumpTextureToPpm(
							    (__bridge void*)texture, path);
							std::fprintf(stderr,
							             "[TexDump] %s addr=0x%llx fmt=%u tile=%u is_rt=%d\n",
							             path, static_cast<unsigned long long>(addr),
							             static_cast<uint32_t>(tex.Format()),
							             static_cast<uint32_t>(tex.TileMode()),
							             IsLinearRenderTargetAddress(addr) ? 1 : 0);
						}
					}
					static std::atomic<uint32_t> s_tex_stats {0};
					if (s_tex_stats.fetch_add(1) < 10) {
						const auto* bytes = reinterpret_cast<const uint8_t*>(addr);
						const size_t span = std::min<size_t>(static_cast<size_t>(w) * h * 4, 1u << 20);
						size_t non_zero = 0;
						for (size_t k = 0; k < span; k += 251) {
							if (bytes[k] != 0) ++non_zero;
						}
						std::fprintf(stderr,
						             "[TexData] addr=0x%llx %ux%u fmt=%u tile=%u nz=%zu/%zu first=%02x%02x%02x%02x\n",
						             static_cast<unsigned long long>(addr), w, h,
						             static_cast<uint32_t>(tex.Format()),
						             static_cast<uint32_t>(tex.TileMode()), non_zero,
						             span / 251, bytes[0], bytes[1], bytes[2], bytes[3]);
					}
				}
			}
		}
		for (const auto& binding : stage.program->bindings.descriptors) {
			const bool image = ShaderRecompiler::IR::ImageBindingResourceClass(binding.kind) != ShaderRecompiler::IR::ImageResourceClass::None;
			const bool sampler = binding.kind == DescKind::Samplers;
			if (!image && !sampler) continue;
			const uint32_t first = abi_on ? abi_layout.BaseOf(binding.kind) : ShaderRecompiler::IR::NativeBinding(stage.program->stage,binding.kind);
			for (uint32_t j=0;j<binding.resources.size();++j) {
				const uint32_t slot=first+j, resource=binding.resources[j];
				if (slot>=256) RejectMetalState("SPIRV-Cross descriptor slot exceeds its ABI");
				if (args[slot] == 0) {
					if (sampler) {
						id<MTLSamplerState> real = StageSampler(stage,resource);
						if (real == nil) RejectMetalState("missing SPIRV-Cross sampler descriptor");
						args[slot] = real.gpuResourceID._impl;
						(args == ps_args ? s_ps_caps : s_vs_caps).push_back({slot,nil,0,nil,real});
					} else {
						RequireNullImageDescriptor(stage,resource);
						RejectMetalState("SPIRV-Cross null image operations require a null-handle lowering; no texture substituted");
					}
				}
			}
		}
	};

	const bool vs_msl = vs_info.stage.program != nullptr && vs_info.stage.program->native_msl;
	const bool ps_msl = ps_info.stage.program != nullptr && ps_info.stage.program->native_msl;
	stage_tag = 'V';
	if (!vs_msl) {
		BindLegacyStage(vs_args, 0, 1, vs_info.stage, vs_info.buffers, vs_info.buffers_num);
	}
	stage_tag = 'P';
	if (pixel_active && !ps_msl) {
		BindLegacyStage(ps_args, 55, 56, ps_info.stage, nullptr, 0);
	}
	stage_tag = 'V';
	if (vs_info.stage.program == nullptr || vs_msl) {
		BindMslStage(vs_args, vs_info.stage, vs_msl ? nullptr : vs_info.buffers,
		             vs_msl ? 0 : vs_info.buffers_num);
	}
	stage_tag = 'P';
	if (pixel_active && (ps_info.stage.program == nullptr || ps_msl)) {
		BindMslStage(ps_args, ps_info.stage, nullptr, 0);
	}
	if (s_default_buf != nil) {
		MetalDrawExecutor::Instance().UseResource(s_default_buf, MTLResourceUsageRead);
		for (size_t i = 0; i < 64; ++i) {
			if (vs_args[i] == 0) {
				vs_args[i] = def_buf_addr;
				vs_sizes[i] = 0;
			}
			if (ps_args[i] == 0) {
				ps_args[i] = def_buf_addr;
				ps_sizes[i] = 0;
			}
		}
	}
	// These ABI control words are not buffer lengths. Populate them after null
	// buffer initialization, which otherwise clears the RECTLIST mode/sentinel
	// and makes the fourth vertex read past the three-record guest descriptor.
	vs_sizes[59] = s_rect_idx[0];
	vs_sizes[60] = s_rect_idx[1];
	vs_sizes[61] = s_rect_idx[2];
	vs_sizes[62] = 0x00FFFFFFu;
	vs_sizes[63] = s_rect_indexed ? 2u : (s_draw_is_indirect_rect_list ? 3u : (s_draw_is_rect_list ? 1u : 0u));
	// Every native VS declares the indirect RECTLIST snapshot. Non-RECTLIST
	// draws take another branch, but still require a valid default binding.
	MetalDrawExecutor::Instance().SetVertexBuffer(s_default_buf, 0, 26);
	if (vs_info.logical_stage == ShaderType::Mesh) MetalDrawExecutor::Instance().SetVertexBuffer(s_default_buf, 0, 24);
	// Binding can return a shorter allocation or snapshot than the guest descriptor.
	MetalDrawExecutor::Instance().SetVertexBytes(vs_sizes, sizeof(vs_sizes), 25);
	MetalDrawExecutor::Instance().SetFragmentBytes(ps_sizes, sizeof(ps_sizes), 25);

	if (bind_trace) {
		auto dwords = [](const auto& snap) {
			std::string out;
			for (size_t i = 0; i < 16 && i < snap.user_data.size(); ++i) {
				char d[16];
				std::snprintf(d, sizeof(d), "%s%08x", i ? "," : "", snap.user_data[i]);
				out += d;
			}
			return out;
		};
		const uint64_t vs_hash = vs_info.stage.program ? vs_info.stage.program->shader_hash : 0;
		const uint64_t ps_hash = (pixel_active && ps_info.stage.program) ? ps_info.stage.program->shader_hash : 0;
		auto udn = [](const auto* snap, const auto* prog) {
			char b[64];
			std::snprintf(b, sizeof(b), "n=%zu,base=%u,regs=%zu", snap ? snap->user_data.size() : 0,
			              prog ? prog->user_data_base : 0u, prog ? prog->bindings.user_data_registers.size() : 0);
			return std::string(b);
		};
		std::fprintf(stderr, "BIND seq=%llu V=%016llx P=%016llx vud=[%s] pud=[%s] vn=[%s] pn=[%s]%s\n",
		             (unsigned long long)TraceWindow::NextDraw(), (unsigned long long)vs_hash,
		             (unsigned long long)ps_hash, (vs_info.stage.resources ? dwords(*vs_info.stage.resources) : std::string()).c_str(),
		             (ps_info.stage.resources ? dwords(*ps_info.stage.resources) : std::string()).c_str(),
		             udn(vs_info.stage.resources, vs_info.stage.program).c_str(),
		             udn(ps_info.stage.resources, ps_info.stage.program).c_str(), bt.c_str());
	}
	if (KYTY_ENV("KYTY_TRACE_SLOTS") != nullptr && pixel_active && ps_info.stage.program != nullptr) {
		static std::unordered_set<uint64_t> s_seen_slots;
		const uint64_t ps_key = ps_info.stage.program->shader_hash;
		static const uint64_t s_slot_hash = [] {
			const char* v = KYTY_ENV("KYTY_PROBE_HASH");
			return v != nullptr ? std::strtoull(v, nullptr, 0) : 0ull;
		}();
		const bool want = s_slot_hash != 0 ? (ps_key == s_slot_hash) : s_seen_slots.size() < 4;
		if (want && s_seen_slots.insert(ps_key).second) {
			std::string desc;
			for (const auto& b: ps_info.stage.program->bindings.descriptors) {
				desc += " kind=" + std::to_string(static_cast<uint32_t>(b.kind)) + " res=[";
				for (size_t i = 0; i < b.resources.size() && i < 4; ++i) {
					desc += std::to_string(b.resources[i]) + (i + 1 < b.resources.size() ? "," : "");
				}
				desc += "]";
			}
			std::fprintf(stderr, "[Slots] PS=%016llx msl=%d count=%u desc:%s\n", (unsigned long long)ps_key,
			             ps_info.stage.program->native_msl ? 1 : 0,
			             static_cast<unsigned>(ShaderRecompiler::IR::DescriptorBindingKind::Count), desc.c_str());
			std::fprintf(stderr, "[Slots] ps_args 55..60:");
			for (int i = 55; i <= 60; ++i) std::fprintf(stderr, " [%d]=%llx", i, (unsigned long long)ps_args[i]);
			std::fprintf(stderr, " | 103..105:");
			for (int i = 103; i <= 105; ++i) std::fprintf(stderr, " [%d]=%llx", i, (unsigned long long)ps_args[i]);
			std::fprintf(stderr, "\n");
		}
	}
	auto build_gfx_argbuf = [&](id<MTLFunction> fn, const std::vector<ArgCapture>& caps, bool is_pixel) -> id<MTLBuffer> {
		static std::unordered_map<uint64_t, id<MTLArgumentEncoder>> s_gfx_argenc;
		id<MTLDevice> gdev = MetalContext::Instance().GetDevice();
		const uint64_t key = (is_pixel ? 1ull << 63 : 0) ^ (uint64_t)(uintptr_t)(__bridge void*)fn;
		id<MTLArgumentEncoder> gae = nil;
		auto git = s_gfx_argenc.find(key);
		if (git != s_gfx_argenc.end()) {
			gae = git->second;
		} else {
			gae = [fn newArgumentEncoderWithBufferIndex:0];
			s_gfx_argenc[key] = gae;
		}
		id<MTLBuffer> gbuf = KytyNewBuffer(gdev, gae.encodedLength, MTLResourceStorageModeShared, __LINE__);
		std::memset([gbuf contents], 0, gae.encodedLength);
		[gae setArgumentBuffer:gbuf offset:0];
		for (const auto& c: caps) {
			if (c.buffer != nil) {
				[gae setBuffer:c.buffer offset:c.offset atIndex:c.index];
			} else if (c.texture != nil) {
				[gae setTexture:c.texture atIndex:c.index];
				if (is_pixel && PassTraceActive() && s_trace_draw_inputs.size() < 64) s_trace_draw_inputs.push_back({c.texture, c.index, 0, 0, 0, 0, 0});
			} else if (c.sampler != nil) {
				[gae setSamplerState:c.sampler atIndex:c.index];
			}
		}
		return gbuf;
	};
	// KYTY_VS_DBG=<hash>: bind the vertex-stage debug buffer (index 27) when the probed shader is bound.
	if (const char* vsdbg = KYTY_ENV("KYTY_VS_DBG"); vsdbg != nullptr && vs_info.stage.program != nullptr &&
	    std::strtoull(vsdbg, nullptr, 0) == vs_info.stage.program->shader_hash) {
		id<MTLDevice> ddev = MetalContext::Instance().GetDevice();
		s_vsdbg_buf = KytyNewBuffer(ddev, 8 * 8 * sizeof(uint32_t), MTLResourceStorageModeShared, __LINE__);
		std::memset([s_vsdbg_buf contents], 0, 8 * 8 * sizeof(uint32_t));
		MetalDrawExecutor::Instance().SetVertexBuffer(s_vsdbg_buf, 0, 27);
	}
	// Vertex stage through the argument encoder (KYTY_ARGENC_GFX=1), with the same per-index check.
	if (KYTY_ENV("KYTY_ARGENC_GFX") != nullptr && vs_info.stage.program != nullptr) {
		id<MTLFunction> vfn = MetalShaderManager::Instance().GetFunction(vs_info.stage.program->shader_hash);
		if (vfn != nil && ShaderHasArgumentBuffer0(vs_info.stage.program->shader_hash)) {
			if (KYTY_ENV("KYTY_ARGENC_CHECK") != nullptr) {
				static std::atomic<uint32_t> s_vchk_log {0};
				std::unordered_map<uint32_t, size_t> last_for_index;
				for (size_t ci = 0; ci < s_vs_caps.size(); ++ci) last_for_index[s_vs_caps[ci].index] = ci;
				size_t mism = 0;
				for (const auto& kv: last_for_index) {
					const auto& c = s_vs_caps[kv.second];
					uint64_t expect = 0;
					if (c.buffer != nil) expect = [c.buffer gpuAddress] + c.offset;
					else if (c.texture != nil) expect = [c.texture gpuResourceID]._impl;
					else if (c.sampler != nil) expect = [c.sampler gpuResourceID]._impl;
					if (vs_args[c.index] != expect) {
						mism++;
						if (s_vchk_log.fetch_add(1) < 8) {
							std::fprintf(stderr, "[ArgCheck] VS idx=%u raw=0x%llx object=0x%llx\n", c.index,
							             (unsigned long long)vs_args[c.index], (unsigned long long)expect);
						}
					}
				}
				std::fprintf(stderr, "[ArgCheck] VS entries=%zu mismatches=%zu\n", last_for_index.size(), mism);
			}
			id<MTLBuffer> vbuf = build_gfx_argbuf(vfn, s_vs_caps, false);
			MetalDrawExecutor::Instance().SetVertexBuffer(vbuf, 0, 0);
		} else {
			MetalDrawExecutor::Instance().SetVertexBytes(vs_args, sizeof(vs_args), 0);
		}
	} else {
		MetalDrawExecutor::Instance().SetVertexBytes(vs_args, sizeof(vs_args), 0);
	}
	// KYTY_ARGENC_GFX=1: graphics argument buffers built from the captured legacy bindings through
	// MTLArgumentEncoder (buffers by setBuffer, textures/samplers by index). Legacy raw path otherwise.
	if (KYTY_ENV("KYTY_ARGENC_GFX") != nullptr && pixel_active && ps_info.stage.program != nullptr) {
		id<MTLFunction> pfn = MetalShaderManager::Instance().GetFunction(ps_info.stage.program->shader_hash);
		if (pfn != nil && ShaderHasArgumentBuffer0(ps_info.stage.program->shader_hash)) {
			id<MTLBuffer> pbuf = build_gfx_argbuf(pfn, s_ps_caps, true);
			if (KYTY_ENV("KYTY_ARGENC_CHECK") != nullptr) {
				static std::atomic<uint32_t> s_chk_log {0};
				size_t mism = 0, total = 0;
				// The legacy binders may write an index more than once; the last write is what the raw array holds.
				std::unordered_map<uint32_t, size_t> last_for_index;
				for (size_t ci = 0; ci < s_ps_caps.size(); ++ci) last_for_index[s_ps_caps[ci].index] = ci;
				for (const auto& kv: last_for_index) {
					const auto& c = s_ps_caps[kv.second];
					uint64_t expect = 0;
					if (c.buffer != nil) expect = [c.buffer gpuAddress] + c.offset;
					else if (c.texture != nil) expect = [c.texture gpuResourceID]._impl;
					else if (c.sampler != nil) expect = [c.sampler gpuResourceID]._impl;
					const uint64_t raw = ps_args[c.index];
					total++;
					// Buffers: the raw value is the GPU address; textures/samplers: the resource id.
					if (raw != expect) {
						mism++;
						if (s_chk_log.fetch_add(1) < 8) {
							std::fprintf(stderr, "[ArgCheck] PS idx=%u raw=0x%llx object=0x%llx kind=%s\n", c.index,
							             (unsigned long long)raw, (unsigned long long)expect,
							             c.buffer ? "buffer" : (c.texture ? "texture" : "sampler"));
						}
					}
				}
				std::fprintf(stderr, "[ArgCheck] PS entries=%zu mismatches=%zu\n", total, mism);
			}
			MetalDrawExecutor::Instance().SetFragmentBuffer(pbuf, 0, 0);
		} else {
			MetalDrawExecutor::Instance().SetFragmentBytes(ps_args, sizeof(ps_args), 0);
		}
	} else {
		MetalDrawExecutor::Instance().SetFragmentBytes(ps_args, sizeof(ps_args), 0);
	}

	// KYTY_MSL_NAN_TRACE=<pixel guest hash>: that shader was emitted with a debug buffer at fragment index 28 that keeps the smallest
	// (id * 2 + half) of its NaN float results; printed once the draw has run (see the [NanId] lines printed at compile time).
	if (ps_info.stage.program != nullptr && NanTraceCs(ps_info.stage.program->guest_hash)) {
		id<MTLBuffer> nan_buffer = KytyNewBuffer(MetalContext::Instance().GetDevice(), sizeof(uint32_t), MTLResourceStorageModeShared, __LINE__);
		if (nan_buffer != nil) {
			*static_cast<uint32_t*>(nan_buffer.contents) = UINT32_MAX;
			MetalDrawExecutor::Instance().SetFragmentBuffer(nan_buffer, 0, 28);
			static std::atomic<uint32_t> s_nan_ps_draws {0};
			const bool report = s_nan_ps_draws.fetch_add(1) % 30 == 0 || PassTraceActive();
			const uint64_t nan_hash = ps_info.stage.program->guest_hash;
			const uint64_t nan_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
			if (report) {
				[MetalContext::Instance().GetCurrentCommandBuffer() addCompletedHandler:^(id<MTLCommandBuffer> command) {
					if (command.status != MTLCommandBufferStatusCompleted) return;
					const uint32_t code = *static_cast<const uint32_t*>(nan_buffer.contents);
					if (code == UINT32_MAX)
						std::fprintf(stderr, "[NanTrace] ps=0x%016llx flip=%llu: no NaN float result\n", (unsigned long long)nan_hash, (unsigned long long)nan_flip);
					else
						std::fprintf(stderr, "[NanTrace] ps=0x%016llx flip=%llu: first NaN float result v%u%s (see the [NanId] line of this id)\n", (unsigned long long)nan_hash,
						             (unsigned long long)nan_flip, code / 2u, (code & 1u) != 0u ? "_h" : "");
				}];
			}
		}
	}
	{
		static std::atomic<uint64_t> s_bind_seq {0};
		const uint64_t seq = s_bind_seq.fetch_add(1);
		if (seq % 500 == 0) {
			const auto& vs_snap = vs_info.stage.resources;
			const auto& ps_snap = ps_info.stage.resources;
			std::fprintf(stderr,
			             "[Bind] seq=%llu ps_msl=%d vs_srt=%zu vs_bufs=%zu ps_srt=%zu "
			             "ps_bufs=%zu ps_imgs=%zu\n",
			             (unsigned long long)seq, ps_msl ? 1 : 0,
			             vs_snap ? vs_snap->flattened_srt.size() : 0,
			             vs_snap ? vs_snap->buffers.size() : 0,
			             ps_snap ? ps_snap->flattened_srt.size() : 0,
			             ps_snap ? ps_snap->buffers.size() : 0,
			             ps_snap ? ps_snap->images.size() : 0);
		}
	}
}


// Surface conversion uses compute/blit encoders and reopens the render pass.
// Resolve every image before setting pipeline, viewport, or resource bindings;
// a newly opened render encoder does not inherit any of that state.
static void PrepareGraphicsImages(const ShaderStageRuntime& stage) {
	if (!stage.resources) return;
	const auto& images = stage.resources->images;
	for (size_t i = 0; i < images.size() && i < 64; ++i) {
		ShaderTextureResource tex {};
		std::copy_n(images[i].dwords.begin(), 8, tex.fields);
		if (tex.Base40() == 0) continue;
		(void)GraphicsImageFor(tex, tex.Width5() + 1u, tex.Height5() + 1u, tex.Base40());
	}
}

// Preserve the guest linkage state when Metal rejects a pipeline. This diagnostic
// does not change the interface or synthesize missing exports.
static void LogPipelineInterfaceFailure(const HW::Context& context,
                                        const ShaderVertexInputInfo& vertex,
                                        const ShaderPixelInputInfo& pixel) {
	const auto& sh = context.GetShaderRegisters();
	std::fprintf(stderr, "[MetalLink] VS_OUT_CONFIG=%08x POS_FORMAT=%08x PRIMITIVEID_EN=%08x PS_IN_CONTROL=%08x PS_INPUT_ENA=%08x PS_INPUT_ADDR=%08x BARYC_CNTL=%08x\n",
	             sh.m_spiVsOutConfig, sh.m_spiShaderPosFormat, sh.m_vgtPrimitiveIdEn,
	             sh.ps_in_control, sh.ps_input_ena, sh.ps_input_addr, sh.baryc_cntl);
	const auto& db = sh.db_shader_control;
	std::fprintf(stderr, "[MetalLink] target_mask=%08x shader_mask=%08x color_mode=%u kill=%u z_export=%u mask_export=%u dual_export=%u exec_noop=%u other=%08x\n",
	             context.GetRenderTargetMask(), sh.m_cbShaderMask, context.GetColorControl().mode,
	             db.shader_kill_enable, db.shader_z_export_enable, db.shader_mask_export_enable,
	             db.shader_dual_export_enable, db.shader_execute_on_noop, db.other_bits);
	for (uint32_t i = 0; i < pixel.input_num; ++i)
		std::fprintf(stderr, "[MetalLink] PS_INPUT_CNTL[%u]=%08x\n", i, sh.ps_interpolator_settings[i]);
	for (const auto* program : {vertex.stage.program, pixel.stage.program}) {
		if (!program) continue;
		std::fprintf(stderr, "[MetalLink] guest=%016llx inputs=%zu outputs=%zu\n",
		             (unsigned long long)program->guest_hash, program->info.inputs.size(), program->info.outputs.size());
	}
}

static uint32_t FragmentColorExportMask(const ShaderPixelInputInfo& pixel) {
	uint32_t mask = 0;
	if (pixel.stage.program) {
		for (const auto& output: pixel.stage.program->info.outputs) {
			if (output.kind == ShaderRecompiler::IR::StageOutputKind::Mrt && output.index < 8)
				mask |= 1u << output.index;
		}
	}
	return mask;
}

// One-shot RT bisection for both indexed and automatic draws. Sample before
// pipeline/viewport setup: waiting closes the encoder, so the caller must reopen it.
static bool DumpRtBeforeDraw(const HW::Context& context, uint64_t ps_hash,
                             const ShaderPixelInputInfo& pixel_info) {
	const char* spec = KYTY_ENV("KYTY_DUMP_RT_DRAW");
	if (spec == nullptr) return false;
	char* end = nullptr;
	const uint64_t want_addr = std::strtoull(spec, &end, 16);
	const uint64_t want_ps = (end && *end == ',') ? std::strtoull(end + 1, &end, 16) : 0;
	static bool started = false;
	static uint32_t count = 0;
	static uint64_t flip0 = 0;
	const auto& rt = context.GetRenderTarget(0);
	if (rt.base.addr != want_addr) return false;
	const auto fmt = MetalFormatFromRenderTarget(rt.info.format, rt.info.channel_type, rt.info.channel_order);
	if (const char* filter = KYTY_ENV("KYTY_DUMP_RT_FORMAT");
	    filter && (uint32_t)fmt != std::strtoul(filter, nullptr, 10)) return false;
	const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load();
	const char* min_env = KYTY_ENV("KYTY_DUMP_RT_MIN_FLIP");
	const uint64_t min_flip = min_env ? std::strtoull(min_env, nullptr, 10) : 0;
	if (!started && (want_ps == 0 || ps_hash == want_ps) && flip >= min_flip) {
		started = true; count = 0; flip0 = flip;
	}
	if (!started || flip != flip0) return false;
	const uint32_t k = count++;
	bool dumped = false;
	for (const char* q = end; q && *q == ',';) {
		const uint32_t kk = static_cast<uint32_t>(std::strtoul(q + 1, const_cast<char**>(&q), 10));
		if (kk != k) continue;
		MetalRenderer::Instance().WaitIdle();
		void* texture = MetalRenderer::Instance().GetRenderTextureForAddressFormat(want_addr, (uint32_t)fmt);
		char path[256];
		const char* dir = KYTY_ENV("KYTY_DUMP_RT_DIR");
		if (!dir) dir = "/tmp/fix6";
		std::snprintf(path, sizeof(path), "%s/rt_%u.ppm", dir, k);
		if (texture) MetalPresenter::Instance().DumpTextureToPpm(texture, path);
		const auto& bc = context.GetBlendControl(0);
		std::fprintf(stderr, "[RtDraw] k=%u flip=%llu ps=%016llx fmt=%u mask=%x/%x blend=%d/%u/%u -> %s\n", k,
		             (unsigned long long)flip0, (unsigned long long)ps_hash, (unsigned)fmt,
		             context.GetRenderTargetMask(), context.GetShaderRegisters().m_cbShaderMask,
		             bc.enable ? 1 : 0, (unsigned)bc.color_srcblend, (unsigned)bc.color_destblend, path);
		if (k == 0 && pixel_info.stage.resources) {
			const auto& snap = *pixel_info.stage.resources;
			for (size_t i = 0; i < snap.images.size(); ++i) {
				ShaderTextureResource t {};
				std::copy_n(snap.images[i].dwords.begin(), 8, t.fields);
				const uint32_t w = t.Width5()+1u, h = t.Height5()+1u;
				id<MTLTexture> image = GraphicsImageFor(t,w,h,t.Base40());
				if (image == nil || image.textureType != MTLTextureType2D) continue;
				MetalRenderer::Instance().WaitIdle();
				std::snprintf(path,sizeof(path),"%s/input_%zu.ppm",dir,i);
				MetalPresenter::Instance().DumpTextureToPpm((__bridge void*)image,path);
				std::fprintf(stderr,"[RtInput] slot=%zu addr=%llx %ux%u fmt=%u tile=%u mtl=%lu -> %s\n",i,
				             (unsigned long long)t.Base40(),w,h,(unsigned)t.Format(),(unsigned)t.TileMode(),
				             (unsigned long)image.pixelFormat,path);
			}
		}
		dumped = true;
	}
	return dumped;
}

// GPU-ordered diagnostic: CPU dumps can miss compute changes to index lists.
static void ProbeBodyIndices(id<MTLBuffer> source, NSUInteger offset, uint32_t count,
                             uint32_t index_type, const void* guest) {
	static const char* dir = KYTY_ENV("KYTY_INDEX_PROBE_DIR");
	if (!dir || source == nil || index_type != 0 || (count != 60516 && count != 98058)) return;
	const uint64_t flip = TraceWindow::g_flip.load(std::memory_order_relaxed);
	static std::map<std::pair<uint64_t,uint32_t>,uint64_t> last;
	const auto key = std::make_pair(reinterpret_cast<uint64_t>(guest),count);
	if (auto it = last.find(key); it != last.end() && flip < it->second + 120) return;
	last[key] = flip;
	const NSUInteger bytes = NSUInteger(count) * 2;
	if (offset > source.length || bytes > source.length - offset) return;
	if (TraceBufferWritersEnabled()) {
		// Compute dispatches whose writable buffer descriptor covers part of this list, newest first (the window is KYTY_TRACE_BUFFER_WRITERS_WINDOW).
		const uint64_t lo = reinterpret_cast<uint64_t>(guest), hi = lo + bytes;
		int shown = 0;
		for (auto it = s_trace_buffer_writers.rbegin(); it != s_trace_buffer_writers.rend() && shown < 6; ++it) {
			if (it->addr >= hi || it->addr + it->bytes <= lo) continue;
			++shown;
			std::fprintf(stderr, "[IndexWriter] flip=%llu list=0x%llx count=%u cs=0x%016llx guest=0x%016llx b%u desc=0x%llx+%llu written_at_flip=%llu\n",
			             (unsigned long long)flip, (unsigned long long)lo, count, (unsigned long long)it->hash, (unsigned long long)it->guest_hash, it->index,
			             (unsigned long long)it->addr, (unsigned long long)it->bytes, (unsigned long long)it->flip);
		}
		if (shown == 0) std::fprintf(stderr, "[IndexWriter] flip=%llu list=0x%llx count=%u no compute writer in the window\n", (unsigned long long)flip, (unsigned long long)lo, count);
	}
	auto cpu = std::make_shared<std::vector<uint16_t>>(count);
	if (!Libs::LibKernel::Memory::TryReadBacking(reinterpret_cast<uint64_t>(guest),cpu->data(),bytes)) return;
	id<MTLBuffer> result = KytyNewBuffer(MetalContext::Instance().GetDevice(), bytes, MTLResourceStorageModeShared, __LINE__);
	if (result == nil) return;
	MetalDrawExecutor::Instance().EndRenderPass();
	id<MTLCommandBuffer> command = MetalContext::Instance().GetCurrentCommandBuffer();
	id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
	if (blit == nil) return;
	OrderBegin(blit);
	[blit copyFromBuffer:source sourceOffset:offset toBuffer:result destinationOffset:0 size:bytes];
	OrderEnd(blit); [blit endEncoding];
	[command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
		if (completed.status != MTLCommandBufferStatusCompleted) return;
		const auto* v = static_cast<const uint16_t*>(result.contents);
		size_t degenerate = 0, changed = 0;
		for (uint32_t i = 0; i + 2 < count; i += 3)
			degenerate += v[i] == v[i+1] || v[i] == v[i+2] || v[i+1] == v[i+2];
		for (uint32_t i = 0; i < count; ++i) changed += v[i] != (*cpu)[i];
		std::fprintf(stderr,"[IndexProbe] flip=%llu addr=0x%llx count=%u degenerate=%zu/%u gpu_cpu_changed=%zu\n",
		             (unsigned long long)flip,(unsigned long long)reinterpret_cast<uint64_t>(guest),count,degenerate,count/3,changed);
		char path[1024]; std::snprintf(path,sizeof(path),"%s/flip%llu-count%u.bin",dir,(unsigned long long)flip,count);
		if (FILE* f = std::fopen(path,"wb")) { std::fwrite(v,1,bytes,f); std::fclose(f); }
	}];
}

static id<MTLBuffer> PrepareNativeMeshArguments(const ShaderMeshInputInfo& mesh, id<MTLBuffer> source,
                                                uint64_t offset, uint32_t count, uint32_t stride,
                                                bool indexed, uint32_t index_bytes) {
	static MetalMeshIndirect prepare;
	MetalDrawExecutor::Instance().EndRenderPass();
	auto result = prepare.Encode(MetalContext::Instance().GetCurrentCommandBuffer(), source, offset,
	                             count, stride, indexed, index_bytes, mesh.InputPrimitiveSize(),
	                             mesh.InputPrimitiveStep(), mesh.primitives_per_group);
	if (!result) RejectMetalState("native NGG indirect argument preparation failed");
	if (!s_last_pass_valid) RejectMetalState("native NGG indirect draw has no render pass to resume");
	auto reopen=s_last_pass_desc;
	for (uint32_t i=0;i<reopen.color_attachment_count;++i) reopen.color_attachments[i].load_action=MTLLoadActionLoad;
	if (reopen.has_depth) reopen.depth_attachment.load_action=MTLLoadActionLoad;
	if (reopen.has_stencil) reopen.stencil_attachment.load_action=MTLLoadActionLoad;
	if (!MetalDrawExecutor::Instance().BeginRenderPass(reopen)) RejectMetalState("native NGG render pass resume failed");
	return result;
}

void MetalRenderer::DrawIndex(uint64_t submit_id, const HW::Context& context, const HW::Shader& sh_ctx,
                              const HW::UserConfig& ucfg, PipelineCache& pipeline_cache,
                              Prospero::PrimitiveType prim_type,
                              uint32_t index_count, const void* index_addr, uint32_t instance_count,
                              uint32_t index_type_and_size, int32_t base_vertex,
                              uint32_t first_instance) {
	s_draw_writes_depth = GuestDepthWritesEnabled(context);
	DrawPassTrace draw_pass_trace;
	const PendingIndirectDraw pending_index = TakePendingIndirectDraw();
	s_trace_submit_id = submit_id;
	PassTraceNoteDrawState(context, static_cast<uint32_t>(prim_type), index_count);
	s_trace_instances = instance_count;
	s_trace_index_addr = pending_index.args_addr == 0 ? index_addr : nullptr;
	s_trace_index_bytes = static_cast<size_t>(index_count) * (index_type_and_size == 0 ? 2 : 4);
	s_trace_index_type = index_type_and_size;
	s_trace_base_vertex = base_vertex;
	s_trace_first_instance = first_instance;
	s_trace_indirect_addr = pending_index.args_addr;
	s_trace_count_addr = pending_index.count_addr;
	s_trace_max_count = pending_index.max_count;
	s_trace_stride = pending_index.stride;
	AllocProbe alloc_probe(1);
	UpdateCacheReport();
	MslDrawStatSubmitted();
	if (KYTY_ENV("KYTY_TRACE_EARLY") && TraceWindow::Active()) { std::fprintf(stderr, "[Entry] DrawIndex\n"); }
	(void)submit_id;
	if (!m_active || (index_count == 0 && pending_index.args_addr == 0)) {
		if (KYTY_ENV("KYTY_TRACE_EARLY") && TraceWindow::Active()) {
			std::fprintf(stderr, "[Early] DrawIndex m_active=%d index_count=%u\n", m_active ? 1 : 0, index_count);
		}
		return;
	}
	std::lock_guard lock(m_mutex);

	static std::atomic<uint64_t> s_draw_idx_count {0};
	++s_image_read_epoch;
	uint64_t didx = s_draw_idx_count.fetch_add(1, std::memory_order_relaxed);
	if (didx < 10 || didx % 500 == 0) {
		const auto& rt0 = context.GetRenderTarget(0);
		std::fprintf(stderr, "[MetalRenderer] DrawIndex #%llu: prim=%u idx_count=%u rt0=0x%llx\n",
		             (unsigned long long)didx, (uint32_t)prim_type, index_count, (unsigned long long)rt0.base.addr);
	}

	EnsureActiveRenderPass(context);
	NoteDepthUse(context);
	// A DEPTH_CLEAR_ENABLE draw only clears depth (done by the pass that EnsureActiveRenderPass just started).
	if (context.GetRenderControl().depth_clear_enable && context.GetDepthRenderTarget().z_read_base_addr != 0) {
		return;
	}

	// 1. Shaders compilation and retrieval
	std::array<ShaderVertexInputInfo, 3> vertex_info {};
	ShaderPixelInputInfo pixel_info {};
	std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX> target_export_mapping {};
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		const auto& rt = context.GetRenderTarget(slot);
		if (rt.base.addr != 0) {
			target_export_mapping[slot] = TextureGetRenderTargetFormat(
			    rt.info.format, rt.info.channel_type, rt.info.channel_order).export_mapping;
		}
	}
	const bool pixel_active = NativePixelShaderActive(context, sh_ctx.GetPs().ps_regs.data_addr);
	auto programs = pipeline_cache.GetGraphicsPrograms(
	    sh_ctx.GetVs(), sh_ctx.GetPs(), context.GetShaderRegisters(),
	    context, ucfg, target_export_mapping, pixel_active, vertex_info, pixel_info);
	MetalDrawExecutor::Instance().SetMeshState(vertex_info[0].logical_stage == ShaderType::Mesh ? &vertex_info[0].mesh : nullptr);

	uint64_t vs_hash = vertex_info[0].stage.program ? vertex_info[0].stage.program->shader_hash : 0;
	uint64_t ps_hash = (pixel_active && pixel_info.stage.program) ? pixel_info.stage.program->shader_hash : 0;
	PrepareGraphicsImages(vertex_info[0].stage);
	if (pixel_active) PrepareGraphicsImages(pixel_info.stage);
	if (DumpRtBeforeDraw(context, ps_hash, pixel_info)) EnsureActiveRenderPass(context);

	id<MTLFunction> vs_func = MetalShaderManager::Instance().GetFunction(vs_hash);
	id<MTLFunction> ps_func = ps_hash ? MetalShaderManager::Instance().GetFunction(ps_hash) : nil;

	{
		const auto& watch_rt = context.GetRenderTarget(0);
		if (watch_rt.base.addr == 0x8240b50000ull) {
			static std::atomic<uint32_t> s_rt_watch {0};
			if (s_rt_watch.fetch_add(1) < 12) {
				std::fprintf(stderr,
				             "[RTWatch] addr=0x8240b50000 vs=0x%016llx ps=0x%016llx "
				             "vs_msl=%d ps_msl=%d fmt=%u\n",
				             static_cast<unsigned long long>(vs_hash),
				             static_cast<unsigned long long>(ps_hash),
				             vertex_info[0].stage.program && vertex_info[0].stage.program->native_msl ? 1 : 0,
				             pixel_info.stage.program && pixel_info.stage.program->native_msl ? 1 : 0,
				             static_cast<uint32_t>(watch_rt.info.format));
			}
		}
	}

	// 2. Render Pipeline State
	if (pending_index.multi && !PrepareMultiDrawArguments(pending_index, vs_hash)) return;
	id<MTLBuffer> mesh_index_arguments=nil;
	if (vertex_info[0].logical_stage == ShaderType::Mesh && pending_index.multi && pending_index.indexed) {
		uint64_t offset=0; auto source=MultiDrawArguments(pending_index,offset);
		mesh_index_arguments=PrepareNativeMeshArguments(vertex_info[0].mesh,source,offset,pending_index.max_count,
		    MultiDrawArgumentsStride(pending_index),true,pending_index.index_type==1u?4u:2u);
	}
	TraceDrawOp(vertex_info[0], context.GetRenderTarget(0).base.addr, vs_hash);
	const bool msl_draw_ok = vs_func != nil && (ps_hash == 0 || ps_func != nil);
	if (!msl_draw_ok) {
		MslNoteSkippedDraw(vs_hash, ps_hash);
		MslDrawStatDropped(vs_func == nil ? 0 : 1);
		RejectMetalState("graphics shader compilation failed: VS=" + std::to_string(vs_hash) + " PS=" + std::to_string(ps_hash));
	}
	if (msl_draw_ok) {
		MetalRenderPipelineKey pipe_key {};
		pipe_key.primitive_type = static_cast<uint32_t>(prim_type);
		pipe_key.vs_hash = vs_hash;
		pipe_key.fs_hash = ps_hash;
	TracePsRt(context.GetRenderTarget(0).base.addr, ps_hash);
		ConfigureNativeColorTargets(pipe_key, context);
		const auto& rt0 = context.GetRenderTarget(0);
		const auto& dt = context.GetDepthRenderTarget();
		if (dt.z_read_base_addr != 0 && dt.z_info.format != Prospero::DepthFormat::kInvalid) {
			pipe_key.depth_format = MetalFormatFromDepthFormat(dt.z_info.format, false);
			if (GuestPassHasStencil(context)) pipe_key.stencil_format = MTLPixelFormatStencil8;
		}

		MaskUnwrittenColorAttachments(pipe_key, FragmentColorExportMask(pixel_info));
		id<MTLRenderPipelineState> pstate = MetalPipelineManager::Instance().GetOrCreateRenderPipeline(
		    pipe_key, vs_func, ps_func);
		if (pstate == nil) {
			LogPipelineInterfaceFailure(context, vertex_info[0], pixel_info);
			RejectMetalState("render pipeline creation failed: VS=" + std::to_string(vs_hash) + " PS=" + std::to_string(ps_hash));
		}
		if (pstate != nil) {
			MetalDrawExecutor::Instance().SetRenderPipeline(pstate);
		} else {
			static std::atomic<uint32_t> s_nil_pipelines {0};
			if (s_nil_pipelines.fetch_add(1) < 6) {
				std::fprintf(stderr,
				             "[DrawAuto] nil pipeline: rt_info=%u type=%u order=%u mtl=%u vs=0x%llx ps=0x%llx\n",
				             static_cast<uint32_t>(rt0.info.format),
				             static_cast<uint32_t>(rt0.info.channel_type),
				             static_cast<uint32_t>(rt0.info.channel_order),
				             static_cast<uint32_t>(pipe_key.color_targets[0].pixel_format),
				             static_cast<unsigned long long>(vs_hash),
				             static_cast<unsigned long long>(ps_hash));
			}
		}
	}

	// 3. Depth Stencil State
	NoteStencilUse(context, vs_hash, ps_hash);
	const bool draw_has_stencil = GuestPassHasStencil(context);
	MetalStencilRefs stencil_refs {};
	HW::DepthControl depth_control = ResolveGuestDepthControl(context);
	auto depth_stencil = MetalPipelineManager::Instance().GetOrCreateDepthStencilState(
	    depth_control, context.GetStencilControl(), context.GetStencilMask(), draw_has_stencil,
	    context.GetRenderControl().stencil_clear_enable || context.GetDepthRenderTarget().depth_view.stencil_write_disable, &stencil_refs);
	MetalDrawExecutor::Instance().SetDepthStencil(depth_stencil);
	MetalDrawExecutor::Instance().SetDepthBounds(context.GetDepthControl().depth_bounds_enable,context.GetDepthBoundsMin(),context.GetDepthBoundsMax());
	if (draw_has_stencil) MetalDrawExecutor::Instance().SetStencilReferences(stencil_refs.front, stencil_refs.back);

	// 4. Rasterizer & Viewport State
	MetalRasterizerState rasterizer = MetalPipelineManager::Instance().TranslateRasterizerState(
	    context.GetModeControl(), context.GetClipControl());
	ApplyGuestDepthBiasAndBlendColor(context, rasterizer, vs_hash, ps_hash);
	MetalDrawExecutor::Instance().SetRasterizerState(rasterizer);

	const auto& vp0 = context.GetScreenViewport().viewports[0];
	double x      = vp0.xoffset - vp0.xscale;
	double y      = vp0.yoffset - vp0.yscale;
	double width  = vp0.xscale * 2.0;
	double height = vp0.yscale * 2.0;
	double znear  = vp0.zoffset - (context.GetClipControl().dx_clip_space ? 0.0 : vp0.zscale);
	double zfar   = vp0.zscale + vp0.zoffset;
	const auto* vertex_program = vertex_info[0].stage.program;
	const bool viewport_indexed = vertex_program && std::any_of(vertex_program->info.outputs.begin(), vertex_program->info.outputs.end(),
	    [](const auto& o) { return o.kind == ShaderRecompiler::IR::StageOutputKind::ViewportIndex; });
	MetalDrawExecutor::Instance().SetGuestViewports(context.GetScreenViewport(), context.GetScanModeControl(), context.GetClipControl(), viewport_indexed);


	// 5. Index buffer and DrawIndexed
	MetalBuffer index_buffer;
	size_t index_bytes = index_count * (index_type_and_size == 0 ? 2 : 4);
	// Small index lists written by the CPU into a recycled ring are copied at encode time (the GPU runs later and
	// would otherwise see indices written for a later draw: missing or garbage triangles).
	// Larger lists are drawn from a wrapper of the guest pages: the wrapper starts at the page, the list at GetOffset() into it. Metal
	// takes index buffer offsets in multiples of 4 bytes, so a 16-bit list starting at an odd half-word is copied instead.
	MetalBufferAllocation index_snapshot {};
	const bool index_misaligned = (reinterpret_cast<uintptr_t>(index_addr) & 3u) != 0;
	if (index_addr != nullptr && index_bytes > 0 && (index_bytes <= 16384 || (index_misaligned && index_bytes <= (8u << 20)))) {
		index_snapshot = MetalBufferManager::Instance().AllocateDynamic(index_bytes, 4);
		if (index_snapshot.IsValid() && index_snapshot.cpu_addr != nullptr) {
			std::memcpy(index_snapshot.cpu_addr, index_addr, index_bytes);
		} else {
			index_snapshot = {};
		}
	}
	MetalBufferAllocation index_binding {};
	if (index_addr != nullptr && !index_snapshot.IsValid()) {
		// Compute can rewrite these indices (including degenerate triangles for
		// hidden body regions). Use its canonical backing buffer: a second Metal
		// object wrapping the same guest pages does not share resource tracking.
		index_binding = MetalBufferManager::Instance().GetBufferForGuestRange(
		    reinterpret_cast<uint64_t>(index_addr), index_bytes);
		if (!index_binding.IsValid())
			index_buffer.WrapHostMemory(const_cast<void*>(index_addr), index_bytes, "IndexBuffer");
	}

	MTLIndexType idx_type = (index_type_and_size == 0) ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;

	// An indexed RECTLIST has three indices; the fourth corner is synthesised by the vertex wrapper. Draw it as two
	// triangles (i0,i1,i2,i1,i2,SENT) from a small dynamic index buffer.
	s_rect_indexed = false;
	s_draw_is_rect_list = false;
	s_draw_is_indirect_rect_list = false;
	MetalBufferAllocation rect_ib {};
	// KYTY_LOG_DRAWS_FLIP=<lo>,<hi>: every draw of those presented frames (render target, shaders, primitive, counts).
	if (const char* rng = KYTY_ENV("KYTY_LOG_DRAWS_FLIP")) {
		unsigned lo = 0, hi = 0;
		std::sscanf(rng, "%u,%u", &lo, &hi);
		const uint64_t fl = Libs::Graphics::TraceWindow::g_flip.load();
		if (fl >= lo && fl <= hi) {
			const uint64_t lvs = vertex_info[0].stage.program ? vertex_info[0].stage.program->shader_hash : 0;
			const uint64_t lps = (pixel_active && pixel_info.stage.program) ? pixel_info.stage.program->shader_hash : 0;
			const auto& drt = context.GetRenderTarget(0);
			std::fprintf(stderr, "[DrawF] flip=%llu rt0=0x%llx vs=%016llx ps=%016llx prim=%u idx=%u inst=%u cmask=0x%llx fc=%d dcc=%d dcc_addr=0x%llx clr=%08x:%08x fmt=%u z=%d\n",
			             (unsigned long long)fl, (unsigned long long)drt.base.addr, (unsigned long long)lvs, (unsigned long long)lps,
			             (uint32_t)prim_type, (uint32_t)index_count, (uint32_t)instance_count, (unsigned long long)drt.cmask.addr,
			             drt.info.cmask_fast_clear_enable ? 1 : 0, drt.info.dcc_compression_enable ? 1 : 0,
			             (unsigned long long)drt.dcc_addr.addr, drt.clear_word0.word0, drt.clear_word1.word1,
			             (unsigned)drt.info.format, context.GetDepthControl().z_enable ? 1 : 0);
		}
	}
	if (KYTY_ENV("KYTY_LOG_DRAWS") != nullptr) {
		static std::atomic<uint32_t> s_idx_label {0};
		if (s_idx_label.fetch_add(1) < 4000) {
			const uint64_t lvs = vertex_info[0].stage.program ? vertex_info[0].stage.program->shader_hash : 0;
			const uint64_t lps = (pixel_active && pixel_info.stage.program) ? pixel_info.stage.program->shader_hash : 0;
			std::fprintf(stderr, "[IdxLabel] rt0=0x%llx vs=0x%016llx ps=0x%016llx prim=%u idx=%u inst=%u\n",
			             (unsigned long long)context.GetRenderTarget(0).base.addr, (unsigned long long)lvs,
			             (unsigned long long)lps, (uint32_t)prim_type, (uint32_t)index_count, (uint32_t)instance_count);
		}
	}
	if (vertex_info[0].logical_stage != ShaderType::Mesh && Prospero::IsRectList(prim_type) && index_count == 3 && index_addr != nullptr && !(pending_index.multi && pending_index.indexed)) {
		uint32_t ids[3];
		for (int k = 0; k < 3; ++k) {
			ids[k] = index_type_and_size == 0 ? static_cast<const uint16_t*>(index_addr)[k]
			                                  : static_cast<const uint32_t*>(index_addr)[k];
		}
		rect_ib = MetalBufferManager::Instance().AllocateDynamic(6 * sizeof(uint32_t), 4);
		if (rect_ib.IsValid() && rect_ib.cpu_addr != nullptr) {
			uint32_t* dst = static_cast<uint32_t*>(rect_ib.cpu_addr);
			dst[0] = ids[0]; dst[1] = ids[1]; dst[2] = ids[2];
			dst[3] = ids[1]; dst[4] = ids[2]; dst[5] = 0x00FFFFFFu;
			s_rect_idx[0] = ids[0]; s_rect_idx[1] = ids[1]; s_rect_idx[2] = ids[2];
			s_rect_indexed = true;
		}
	}
	{
		char fault_label[512];
		std::snprintf(fault_label, sizeof(fault_label), "draw_idx #%llu rt0=0x%llx vs=0x%016llx ps=0x%016llx prim=%u idx=%u inst=%u fi=%u bv=%d tmask=0x%x smask=0x%x blend=%d/%d/%d gfmt=%u mtl=%u",
		              (unsigned long long)didx, (unsigned long long)context.GetRenderTarget(0).base.addr,
		              (unsigned long long)vs_hash, (unsigned long long)ps_hash, (uint32_t)prim_type, index_count, instance_count, first_instance, base_vertex,
		              context.GetRenderTargetMask() & 0xFu, context.GetShaderRegisters().m_cbShaderMask & 0xFu,
		              context.GetBlendControl(0).enable ? 1 : 0, static_cast<int>(context.GetBlendControl(0).color_srcblend),
		              static_cast<int>(context.GetBlendControl(0).color_destblend),
		              (unsigned)context.GetRenderTarget(0).info.format,
		              (unsigned)MetalFormatFromRenderTarget(context.GetRenderTarget(0).info.format, context.GetRenderTarget(0).info.channel_type, context.GetRenderTarget(0).info.channel_order));
		if (pending_index.multi) {
			const size_t used = std::strlen(fault_label);
			std::snprintf(fault_label + used, sizeof(fault_label) - used,
			              " mdi=1 args=0x%llx max=%u stride=%u count=0x%llx ib=0x%llx bytes=%llu",
			              (unsigned long long)pending_index.args_addr, pending_index.max_count, pending_index.stride,
			              (unsigned long long)pending_index.count_addr, (unsigned long long)pending_index.index_base,
			              (unsigned long long)pending_index.index_bytes);
		}
		KytyFaultOpSet(fault_label);
		if (KYTY_ENV("KYTY_LOG_DRAWS") != nullptr) {
			static std::atomic<uint32_t> s_draw_idx_log {0};
			if (s_draw_idx_log.fetch_add(1) < (KYTY_ENV("KYTY_LOG_DRAWS_N") ? static_cast<uint32_t>(std::atoi(KYTY_ENV("KYTY_LOG_DRAWS_N"))) : 400u)) std::fprintf(stderr, "[DrawIndexLabel] %s\n", fault_label);
		}
	}

	BindMetalShaderResources(vertex_info[0], pixel_info, pixel_active);
	MetalDrawExecutor::Instance().SetGraphicsImageWritebacks(s_graphics_image_writebacks);

	if (s_rect_indexed) {
		MetalDrawExecutor::Instance().UseResource(rect_ib.buffer, MTLResourceUsageRead);
		MetalDrawExecutor::Instance().DrawIndexed(prim_type, 6, rect_ib.buffer, rect_ib.offset, MTLIndexTypeUInt32,
		                                          instance_count, base_vertex, first_instance);
	} else if (pending_index.multi && pending_index.indexed) {
		// Indexed multi-draw: one indexed indirect draw per entry from the scratch copy; the index buffer is at
		// index_base and each entry's start index comes from its arguments.
		auto ibuf = MetalBufferManager::Instance().GetBufferForGuestRange(pending_index.index_base, pending_index.index_bytes);
		const MTLIndexType itype = pending_index.index_type == 1u ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
		uint64_t         args_base = 0;
		id<MTLBuffer>    args_buf  = MultiDrawArguments(pending_index, args_base);
		if (mesh_index_arguments) { args_buf=mesh_index_arguments; args_base=0; }
		if (args_buf != nil && ibuf.buffer != nil) {
			MetalDrawExecutor::Instance().UseResource(args_buf, MTLResourceUsageRead);
			MetalDrawExecutor::Instance().UseResource(ibuf.buffer, MTLResourceUsageRead);
			for (uint32_t k2 = 0; k2 < pending_index.max_count; ++k2) {
				MslDrawStatExecuted();
				MetalDrawExecutor::Instance().DrawIndexedIndirect(prim_type, itype, ibuf.buffer, ibuf.offset,
				                                                  args_buf,
				                                                  args_base + static_cast<uint64_t>(k2) * (mesh_index_arguments ? MetalMeshIndirect::RecordBytes : MultiDrawArgumentsStride(pending_index)),
				                                                  k2 + 1 == pending_index.max_count);
			}
		}
	} else if (index_snapshot.IsValid()) {
		MetalDrawExecutor::Instance().UseResource(index_snapshot.buffer, MTLResourceUsageRead);
		MetalDrawExecutor::Instance().DrawIndexed(
		    prim_type, index_count, index_snapshot.buffer, index_snapshot.offset, idx_type,
		    instance_count, base_vertex, first_instance);
	} else if (index_binding.IsValid()) {
		MetalDrawExecutor::Instance().UseResource(index_binding.buffer, MTLResourceUsageRead);
		MetalDrawExecutor::Instance().DrawIndexed(
		    prim_type, index_count, index_binding.buffer,
		    false ? 0 : index_binding.offset, idx_type,
		    instance_count, base_vertex, first_instance);
	} else 	MetalDrawExecutor::Instance().DrawIndexed(
	    // The offset of the list inside its page-aligned wrapper. It used to be 0: every list over 16 KiB that did not start on a page
	    // boundary was read from the start of its page - another mesh's or sub-mesh's indices. The players' heads, hair and legs (sub-meshes
	    // after the first in one index buffer) became the horizontal bands across the menu scenes, while the torso (the first) was right.
	    prim_type, index_count,
	    // KYTY_INDEX_OFFSET0=1 restores the old offset 0 (A/B only).
	    index_buffer.GetHandle(), false ? 0 : index_buffer.GetOffset(), idx_type,
	    instance_count, base_vertex, first_instance);
	if (pending_index.args_addr == 0 && !s_rect_indexed) {
		id<MTLBuffer> probe_buffer = index_snapshot.IsValid() ? index_snapshot.buffer :
		    index_binding.IsValid() ? index_binding.buffer : index_buffer.GetHandle();
		const NSUInteger probe_offset = index_snapshot.IsValid() ? index_snapshot.offset :
		    index_binding.IsValid() ? index_binding.offset : index_buffer.GetOffset();
		ProbeBodyIndices(probe_buffer, probe_offset, index_count, index_type_and_size, index_addr);
	}
	{
		static std::mutex                            s_exec_mutex;
		static std::unordered_map<uint64_t, uint64_t> s_exec_counts;
		static std::atomic<uint64_t>                  s_exec_total {0};
		const uint64_t total = s_exec_total.fetch_add(1) + 1;
		const uint64_t addr  = context.GetRenderTarget(0).base.addr;
		RecordDrawForRT(addr);
		{
			std::lock_guard<std::mutex> lock(s_exec_mutex);
			++s_exec_counts[addr];
		}
		if (total % 5000 == 0) {
			std::lock_guard<std::mutex> lock(s_exec_mutex);
			std::fprintf(stderr, "[Exec] total=%llu", (unsigned long long)total);
			for (const auto& [a, c]: s_exec_counts) {
				std::fprintf(stderr, " 0x%llx:%llu", (unsigned long long)a,
				             (unsigned long long)c);
			}
			std::fprintf(stderr, "\n");
		}
	}
}

void MetalRenderer::DrawAuto(uint64_t submit_id, const HW::Context& context, const HW::Shader& sh_ctx,
                             const HW::UserConfig& ucfg, PipelineCache& pipeline_cache,
                             Prospero::PrimitiveType prim_type,
                             uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
                             uint32_t first_instance) {
	s_draw_writes_depth = GuestDepthWritesEnabled(context);
	AllocProbe alloc_probe(1);
	DrawPassTrace draw_pass_trace;
	s_trace_submit_id = submit_id;
	PassTraceNoteDrawState(context, static_cast<uint32_t>(prim_type), vertex_count);
	MslDrawStatSubmitted();
	// CB_COLOR_CONTROL.MODE 2 (eliminate fast clear), 5 (FMASK decompress) and 6 (DCC decompress) run colour-buffer
	// metadata passes: the pixel shader is only a vehicle, and its export must not be applied as a draw (the Vulkan
	// path skips these too, ConsumeMetadataColorOperation). The splash overlay (RG-zero export) is one of them.
	{
		const uint8_t cb_mode = context.GetColorControl().mode;
		if (cb_mode == 2u || cb_mode == 5u || cb_mode == 6u) {
			static std::atomic<uint32_t> s_meta_skip {0};
			if (s_meta_skip.fetch_add(1) < 4) {
				std::fprintf(stderr, "[DrawAuto] skip colour-buffer metadata pass (CB_COLOR_CONTROL.MODE=%u)\n", cb_mode);
			}
			return;
		}
	}
	const PendingIndirectDraw pending_indirect = TakePendingIndirectDraw();
	const uint64_t indirect_args_addr = pending_indirect.args_addr;
	s_trace_indirect_addr = indirect_args_addr;
	s_trace_instances = instance_count;
	s_trace_index_addr = nullptr;
	s_trace_base_vertex = 0;
	s_trace_count_addr = pending_indirect.count_addr;
	s_trace_max_count = pending_indirect.max_count;
	s_trace_stride = pending_indirect.stride;
	if (KYTY_ENV("KYTY_TRACE_EARLY") && TraceWindow::Active()) { std::fprintf(stderr, "[Entry] DrawAuto prim=%u vtx=%u rt0=0x%llx\n", (unsigned)prim_type, vertex_count, (unsigned long long)context.GetRenderTarget(0).base.addr); }
	(void)submit_id;
	if (!m_active || (vertex_count == 0 && indirect_args_addr == 0)) {
		if (KYTY_ENV("KYTY_TRACE_EARLY") && TraceWindow::Active()) {
			std::fprintf(stderr, "[Early] DrawAuto m_active=%d vertex_count=%u\n", m_active ? 1 : 0, vertex_count);
		}
		return;
	}
	std::lock_guard lock(m_mutex);

	static std::atomic<uint64_t> s_draw_auto_count {0};
	++s_image_read_epoch;
	uint64_t dauto = s_draw_auto_count.fetch_add(1, std::memory_order_relaxed);
	if (dauto < 10 || dauto % 500 == 0) {
		const auto& rt0 = context.GetRenderTarget(0);
		std::fprintf(stderr, "[MetalRenderer] DrawAuto #%llu: prim=%u vtx_count=%u rt0=0x%llx\n",
		             (unsigned long long)dauto, (uint32_t)prim_type, vertex_count, (unsigned long long)rt0.base.addr);
	}
	{
		char fault_label[96];
		std::snprintf(fault_label, sizeof(fault_label), "draw #%llu rt0=0x%llx", (unsigned long long)dauto,
		              (unsigned long long)context.GetRenderTarget(0).base.addr);
		KytyFaultOpSet(fault_label);
	}

	EnsureActiveRenderPass(context);
	NoteDepthUse(context);
	// A DEPTH_CLEAR_ENABLE draw only clears depth (done by the pass that EnsureActiveRenderPass just started).
	if (context.GetRenderControl().depth_clear_enable && context.GetDepthRenderTarget().z_read_base_addr != 0) {
		return;
	}

	std::array<ShaderVertexInputInfo, 3> vertex_info {};
	ShaderPixelInputInfo pixel_info {};
	std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX> target_export_mapping {};
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		const auto& rt = context.GetRenderTarget(slot);
		if (rt.base.addr != 0) {
			target_export_mapping[slot] = TextureGetRenderTargetFormat(
			    rt.info.format, rt.info.channel_type, rt.info.channel_order).export_mapping;
		}
	}
	const bool pixel_active = NativePixelShaderActive(context, sh_ctx.GetPs().ps_regs.data_addr);
	auto programs = pipeline_cache.GetGraphicsPrograms(
	    sh_ctx.GetVs(), sh_ctx.GetPs(), context.GetShaderRegisters(),
	    context, ucfg, target_export_mapping, pixel_active, vertex_info, pixel_info);
	MetalDrawExecutor::Instance().SetMeshState(vertex_info[0].logical_stage == ShaderType::Mesh ? &vertex_info[0].mesh : nullptr);

	uint64_t vs_hash = vertex_info[0].stage.program ? vertex_info[0].stage.program->shader_hash : 0;
	uint64_t ps_hash = (pixel_active && pixel_info.stage.program) ? pixel_info.stage.program->shader_hash : 0;
	PrepareGraphicsImages(vertex_info[0].stage);
	if (pixel_active) PrepareGraphicsImages(pixel_info.stage);
	if (DumpRtBeforeDraw(context, ps_hash, pixel_info)) EnsureActiveRenderPass(context);

	id<MTLFunction> vs_func = MetalShaderManager::Instance().GetFunction(vs_hash);
	id<MTLFunction> ps_func = ps_hash ? MetalShaderManager::Instance().GetFunction(ps_hash) : nil;

	{
		const auto& watch_rt = context.GetRenderTarget(0);
		if (watch_rt.base.addr == 0x8240b50000ull) {
			static std::atomic<uint32_t> s_rt_watch {0};
			if (s_rt_watch.fetch_add(1) < 12) {
				std::fprintf(stderr,
				             "[RTWatch] addr=0x8240b50000 vs=0x%016llx ps=0x%016llx "
				             "vs_msl=%d ps_msl=%d fmt=%u\n",
				             static_cast<unsigned long long>(vs_hash),
				             static_cast<unsigned long long>(ps_hash),
				             vertex_info[0].stage.program && vertex_info[0].stage.program->native_msl ? 1 : 0,
				             pixel_info.stage.program && pixel_info.stage.program->native_msl ? 1 : 0,
				             static_cast<uint32_t>(watch_rt.info.format));
			}
		}
	}

	if (pending_indirect.multi && MultiDrawHasPreparedArguments(pending_indirect) &&
	    !PrepareMultiDrawArguments(pending_indirect, vs_hash)) return;
	TraceDrawOp(vertex_info[0], context.GetRenderTarget(0).base.addr, vs_hash);
	const bool msl_draw_ok = vs_func != nil && (ps_hash == 0 || ps_func != nil);
	if (!msl_draw_ok) {
		MslNoteSkippedDraw(vs_hash, ps_hash);
		MslDrawStatDropped(vs_func == nil ? 0 : 1);
		RejectMetalState("graphics shader compilation failed: VS=" + std::to_string(vs_hash) + " PS=" + std::to_string(ps_hash));
	}
	// Resolve GPU arguments before setting any render state: RECTLIST conversion
	// uses a compute encoder and therefore ends/reopens the current render pass.
	id<MTLBuffer> draw_args = nil;
	uint64_t draw_args_base = 0;
	uint32_t draw_args_stride = pending_indirect.multi ? MultiDrawArgumentsStride(pending_indirect) : 16u;
	const uint32_t draw_args_count = pending_indirect.multi ? pending_indirect.max_count : 1u;
	const bool indirect_rect = vertex_info[0].logical_stage != ShaderType::Mesh && indirect_args_addr != 0 && Prospero::IsRectList(prim_type);
	if (pending_indirect.multi) {
		draw_args = MultiDrawArguments(pending_indirect, draw_args_base);
	} else if (indirect_args_addr != 0) {
		const auto alloc = MetalBufferManager::Instance().GetBufferForGuestRange(indirect_args_addr, 16);
		draw_args = alloc.buffer;
		draw_args_base = alloc.offset;
	}
	if (vertex_info[0].logical_stage == ShaderType::Mesh && indirect_args_addr != 0) {
		draw_args=PrepareNativeMeshArguments(vertex_info[0].mesh,draw_args,draw_args_base,draw_args_count,
		    draw_args_stride,false,0);
		draw_args_base=0; draw_args_stride=MetalMeshIndirect::RecordBytes;
	}
	if (indirect_rect && draw_args) {
		static MetalRectListIndirect rect_arguments;
		MetalDrawExecutor::Instance().EndRenderPass();
		draw_args = rect_arguments.Encode(MetalContext::Instance().GetCurrentCommandBuffer(), draw_args,
		                                 draw_args_base, draw_args_count, draw_args_stride);
		draw_args_base = 0;
		draw_args_stride = MetalRectListIndirect::RecordBytes;
		if (s_last_pass_valid) {
			auto reopen = s_last_pass_desc;
			for (uint32_t i = 0; i < reopen.color_attachment_count; ++i) reopen.color_attachments[i].load_action = MTLLoadActionLoad;
			if (reopen.has_depth) reopen.depth_attachment.load_action = MTLLoadActionLoad;
			if (reopen.has_stencil) reopen.stencil_attachment.load_action = MTLLoadActionLoad;
			MetalDrawExecutor::Instance().BeginRenderPass(reopen);
		}
		if (!draw_args) { MslDrawStatDropped(2); return; }
	}
	if (msl_draw_ok) {
		MetalRenderPipelineKey pipe_key {};
		pipe_key.primitive_type = static_cast<uint32_t>(prim_type);
		pipe_key.vs_hash = vs_hash;
		pipe_key.fs_hash = ps_hash;
	TracePsRt(context.GetRenderTarget(0).base.addr, ps_hash);
		ConfigureNativeColorTargets(pipe_key, context);
		const auto& rt0 = context.GetRenderTarget(0);
		const auto& dt = context.GetDepthRenderTarget();
		if (dt.z_read_base_addr != 0 && dt.z_info.format != Prospero::DepthFormat::kInvalid) {
			pipe_key.depth_format = MetalFormatFromDepthFormat(dt.z_info.format, false);
			if (GuestPassHasStencil(context)) pipe_key.stencil_format = MTLPixelFormatStencil8;
		}

		MaskUnwrittenColorAttachments(pipe_key, FragmentColorExportMask(pixel_info));
		id<MTLRenderPipelineState> pstate = MetalPipelineManager::Instance().GetOrCreateRenderPipeline(
		    pipe_key, vs_func, ps_func);
		if (pstate == nil) {
			LogPipelineInterfaceFailure(context, vertex_info[0], pixel_info);
			RejectMetalState("render pipeline creation failed: VS=" + std::to_string(vs_hash) + " PS=" + std::to_string(ps_hash));
		}
		if (pstate != nil) {
			MetalDrawExecutor::Instance().SetRenderPipeline(pstate);
		}
	}

	NoteStencilUse(context, vs_hash, ps_hash);
	const bool draw_has_stencil = GuestPassHasStencil(context);
	MetalStencilRefs stencil_refs {};
	HW::DepthControl depth_control = ResolveGuestDepthControl(context);
	auto depth_stencil = MetalPipelineManager::Instance().GetOrCreateDepthStencilState(
	    depth_control, context.GetStencilControl(), context.GetStencilMask(), draw_has_stencil,
	    context.GetRenderControl().stencil_clear_enable || context.GetDepthRenderTarget().depth_view.stencil_write_disable, &stencil_refs);
	MetalDrawExecutor::Instance().SetDepthStencil(depth_stencil);
	MetalDrawExecutor::Instance().SetDepthBounds(context.GetDepthControl().depth_bounds_enable,context.GetDepthBoundsMin(),context.GetDepthBoundsMax());
	if (draw_has_stencil) MetalDrawExecutor::Instance().SetStencilReferences(stencil_refs.front, stencil_refs.back);

	MetalRasterizerState rasterizer = MetalPipelineManager::Instance().TranslateRasterizerState(
	    context.GetModeControl(), context.GetClipControl());
	ApplyGuestDepthBiasAndBlendColor(context, rasterizer, vs_hash, ps_hash);
	MetalDrawExecutor::Instance().SetRasterizerState(rasterizer);

	const auto& vp0 = context.GetScreenViewport().viewports[0];
	double x      = vp0.xoffset - vp0.xscale;
	double y      = vp0.yoffset - vp0.yscale;
	double width  = vp0.xscale * 2.0;
	double height = vp0.yscale * 2.0;
	double znear  = vp0.zoffset - (context.GetClipControl().dx_clip_space ? 0.0 : vp0.zscale);
	double zfar   = vp0.zscale + vp0.zoffset;
	if (KYTY_ENV("KYTY_TRACE_ZRANGE") != nullptr) { std::fprintf(stderr, "[ZRange] znear=%g zfar=%g zscale=%g zoffset=%g dx=%d\n", znear, zfar, vp0.zscale, vp0.zoffset, context.GetClipControl().dx_clip_space ? 1 : 0); }
	const auto* vertex_program = vertex_info[0].stage.program;
	const bool viewport_indexed = vertex_program && std::any_of(vertex_program->info.outputs.begin(), vertex_program->info.outputs.end(),
	    [](const auto& o) { return o.kind == ShaderRecompiler::IR::StageOutputKind::ViewportIndex; });
	MetalDrawExecutor::Instance().SetGuestViewports(context.GetScreenViewport(), context.GetScanModeControl(), context.GetClipControl(), viewport_indexed);


	{
		static std::atomic<uint32_t> s_vp_log2 {0};
		if (s_vp_log2.fetch_add(1) < 10) {
			std::fprintf(stderr,
			             "[DrawIndex] viewport x=%.1f y=%.1f w=%.1f h=%.1f scissor=%u,%u %ux%u\n",
			             x, y, width, height,
			             context.GetScreenViewport().screen_scissor_left, context.GetScreenViewport().screen_scissor_top,
			             context.GetScreenViewport().screen_scissor_right - context.GetScreenViewport().screen_scissor_left,
			             context.GetScreenViewport().screen_scissor_bottom - context.GetScreenViewport().screen_scissor_top);
		}
	}

	s_rect_indexed = false;
	s_draw_is_indirect_rect_list = indirect_rect;
	s_draw_is_rect_list = !indirect_rect && Prospero::IsRectList(prim_type) && vertex_count == 3;
	if (KYTY_ENV("KYTY_LOG_PRIMS") != nullptr) {
		static std::mutex s_prim_mutex;
		static std::map<std::pair<uint32_t, uint32_t>, uint64_t> s_prim_hist;
		static std::atomic<uint64_t> s_prim_n {0};
		std::lock_guard<std::mutex> lock(s_prim_mutex);
		++s_prim_hist[{static_cast<uint32_t>(prim_type), vertex_count}];
		if (s_prim_n.fetch_add(1) % 3000 == 2999) {
			std::fprintf(stderr, "[PrimHist]");
			for (const auto& [k, v]: s_prim_hist) std::fprintf(stderr, " prim%u/vtx%u=%llu", k.first, k.second, (unsigned long long)v);
			std::fprintf(stderr, "\n");
		}
	}

	BindMetalShaderResources(vertex_info[0], pixel_info, pixel_active);
	MetalDrawExecutor::Instance().SetGraphicsImageWritebacks(s_graphics_image_writebacks);
	{
		char fault_label[288];
		std::snprintf(fault_label, sizeof(fault_label), "draw #%llu rt0=0x%llx vs=0x%016llx ps=0x%016llx prim=%u vtx=%u inst=%u tmask=0x%x smask=0x%x blend=%d/%d/%d gfmt=%u mtl=%u",
		              (unsigned long long)dauto, (unsigned long long)context.GetRenderTarget(0).base.addr,
		              (unsigned long long)vs_hash, (unsigned long long)ps_hash, (uint32_t)prim_type, vertex_count, instance_count,
		              context.GetRenderTargetMask() & 0xFu, context.GetShaderRegisters().m_cbShaderMask & 0xFu,
		              context.GetBlendControl(0).enable ? 1 : 0, static_cast<int>(context.GetBlendControl(0).color_srcblend),
		              static_cast<int>(context.GetBlendControl(0).color_destblend),
		              (unsigned)context.GetRenderTarget(0).info.format,
		              (unsigned)MetalFormatFromRenderTarget(context.GetRenderTarget(0).info.format, context.GetRenderTarget(0).info.channel_type, context.GetRenderTarget(0).info.channel_order));
		KytyFaultOpSet(fault_label);
		if (KYTY_ENV("KYTY_LOG_DRAWS") != nullptr) {
			static std::atomic<uint32_t> s_draw_label_log {0};
			if (s_draw_label_log.fetch_add(1) < (KYTY_ENV("KYTY_LOG_DRAWS_N") ? static_cast<uint32_t>(std::atoi(KYTY_ENV("KYTY_LOG_DRAWS_N"))) : 400u)) std::fprintf(stderr, "[DrawLabel] %s\n", fault_label);
		}
	}

	MslDrawStatExecuted();
	if (indirect_args_addr != 0) {
		if (draw_args != nil) {
			MetalDrawExecutor::Instance().UseResource(draw_args, MTLResourceUsageRead);
			for (uint32_t k = 0; k < draw_args_count; ++k) {
				const uint64_t offset = draw_args_base + static_cast<uint64_t>(k) * draw_args_stride;
				if (indirect_rect) MetalDrawExecutor::Instance().SetVertexBuffer(draw_args,
				    offset + MetalRectListIndirect::OriginalArgumentsOffset, 26);
				MetalDrawExecutor::Instance().DrawIndirect(prim_type, draw_args, offset, k + 1 == draw_args_count);
			}
		}
	} else {
	MetalDrawExecutor::Instance().Draw(
	    prim_type, vertex_count,
	    instance_count, first_vertex, first_instance);
	}
	{
		const auto& rtc = context.GetRenderTarget(0);
		if (rtc.base.addr != 0 && rtc.cmask.addr != 0 && rtc.info.cmask_fast_clear_enable &&
		    !rtc.info.dcc_compression_enable) {
			size_t cmask_bytes = 24576;
			if (void* tex_ptr = MetalRenderer::Instance().GetRenderTextureForAddress(rtc.base.addr)) {
				id<MTLTexture> t = (__bridge id<MTLTexture>)tex_ptr;
				const size_t tiles = ((static_cast<size_t>(t.width) + 7) / 8) * ((static_cast<size_t>(t.height) + 7) / 8);
				// The tile swizzle spreads the metadata over more than tiles/2 bytes (the next buffer starts 24 KiB after it).
				cmask_bytes = std::min<size_t>(std::max<size_t>(((tiles / 2 + 255) / 256) * 256, 24576), 65536);
			}
			static std::atomic<uint32_t> s_cmask_log {0};
			if (s_cmask_log.fetch_add(1) < 4) {
				std::fprintf(stderr, "[Cmask] mark touched rt=0x%llx cmask=0x%llx bytes=%zu\n",
				             (unsigned long long)rtc.base.addr, (unsigned long long)rtc.cmask.addr, cmask_bytes);
			}
			if (KYTY_ENV("KYTY_LOG_CMASK") != nullptr) {
				uint32_t lw = 0, lh = 0;
				if (void* tp = MetalRenderer::Instance().GetRenderTextureForAddress(rtc.base.addr)) {
					id<MTLTexture> lt = (__bridge id<MTLTexture>)tp;
					lw = static_cast<uint32_t>(lt.width);
					lh = static_cast<uint32_t>(lt.height);
				}
				LogCmaskFill(rtc.cmask.addr, cmask_bytes, rtc.base.addr, lw, lh);
			}
			MarkCmaskTouched(rtc.cmask.addr, cmask_bytes);
		}
	}
	if (KYTY_ENV("KYTY_RT_AFTER") != nullptr && ps_hash == std::strtoull(KYTY_ENV("KYTY_RT_AFTER"), nullptr, 0) &&
	    Libs::Graphics::TraceWindow::Active()) {
		static uint64_t s_rt_after_seen = 0;
		static const uint64_t s_rt_after_n = [] {
			const char* v = KYTY_ENV("KYTY_RT_AFTER_N");
			return v != nullptr ? std::strtoull(v, nullptr, 10) : 20000ull;
		}();
		static bool s_rt_after_done = false;
		if (!s_rt_after_done && ++s_rt_after_seen >= s_rt_after_n) {
			s_rt_after_done = true;
			MetalRenderer::Instance().Flush();
			void* rt = MetalRenderer::Instance().GetRenderTextureForAddress(context.GetRenderTarget(0).base.addr);
			const auto& rtinfo = context.GetRenderTarget(0);
			const auto mtlfmt = MetalFormatFromRenderTarget(rtinfo.info.format, rtinfo.info.channel_type, rtinfo.info.channel_order);
			std::fprintf(stderr, "[RtAfter] rt0 fmt=%u channel_type=%u order=%u mtl=%u\n", (unsigned)rtinfo.info.format,
			             (unsigned)rtinfo.info.channel_type, (unsigned)rtinfo.info.channel_order, (unsigned)mtlfmt);
			void* rt_exact = MetalRenderer::Instance().GetRenderTextureForAddressFormat(rtinfo.base.addr, (uint32_t)mtlfmt);
			std::fprintf(stderr, "[RtAfter] exact tex=%p rt=%p\n", rt_exact, rt);
			if (rt_exact != nullptr) MetalPresenter::Instance().DumpTextureToPpm(rt_exact, "/tmp/rt_after_exact.ppm");
			const auto& dc = context.GetDepthControl();
			if (s_vsdbg_buf != nil) {
				// Vertex-stage probe rows for this draw: vertex_id, instance_id, position xyzw (float bits).
				MetalContext::Instance().CommitCommandBuffer(true);
				const uint32_t* vd = static_cast<const uint32_t*>([s_vsdbg_buf contents]);
				for (uint32_t r = 0; r < 8; ++r) {
					const uint32_t* row = vd + r * 8;
					if (row[0] == 0 && row[1] == 0 && row[2] == 0 && r != 0) continue;
					std::fprintf(stderr, "[VsRow] id=%u inst=%u pos=(%g, %g, %g, %g) raw=%08x %08x %08x %08x\n",
					             row[0], row[1], *reinterpret_cast<const float*>(&row[2]), *reinterpret_cast<const float*>(&row[3]),
					             *reinterpret_cast<const float*>(&row[4]), *reinterpret_cast<const float*>(&row[5]),
					             row[2], row[3], row[4], row[5]);
				}
			}
			std::fprintf(stderr, "[RtAfter] prim=%u vtx=%u\n", (unsigned)prim_type, vertex_count);
			std::fprintf(stderr, "[RtAfter] rt0=0x%llx tex=%p z_en=%d z_wr=%d zfunc=%u st_en=%d stfunc=%u\n",
			             (unsigned long long)context.GetRenderTarget(0).base.addr, rt, dc.z_enable ? 1 : 0,
			             dc.z_write_enable ? 1 : 0, (unsigned)dc.zfunc, dc.stencil_enable ? 1 : 0,
			             (unsigned)dc.stencilfunc);
			{
				const auto& scr = context.GetScreenViewport();
				const auto& bcf = context.GetBlendControl(0);
				std::fprintf(stderr, "[RtAfter] blend_src=%d blend_dst=%d comb=%d alpha_src=%d alpha_dst=%d alpha_comb=%d\n",
				             (int)bcf.color_srcblend, (int)bcf.color_destblend, (int)bcf.color_comb_fcn,
				             (int)bcf.alpha_srcblend, (int)bcf.alpha_destblend, (int)bcf.alpha_comb_fcn);
				std::fprintf(stderr, "[RtAfter] mask=0x%x blend=%d scissor=[%u,%u,%u,%u]\n",
				             context.GetRenderTargetMask(), context.GetBlendControl(0).enable ? 1 : 0,
				             scr.screen_scissor_left, scr.screen_scissor_top, scr.screen_scissor_right,
				             scr.screen_scissor_bottom);
			}
			if (rt != nullptr) MetalPresenter::Instance().DumpTextureToPpm(rt, "/tmp/rt_after_draw.ppm");
		}
	}
	{
		static std::mutex                            s_exec_mutex;
		static std::unordered_map<uint64_t, uint64_t> s_exec_counts;
		static std::atomic<uint64_t>                  s_exec_total {0};
		const uint64_t total = s_exec_total.fetch_add(1) + 1;
		const uint64_t addr  = context.GetRenderTarget(0).base.addr;
		RecordDrawForRT(addr);
		{
			std::lock_guard<std::mutex> lock(s_exec_mutex);
			++s_exec_counts[addr];
		}
		if (total % 5000 == 0) {
			std::lock_guard<std::mutex> lock(s_exec_mutex);
			std::fprintf(stderr, "[Exec] total=%llu", (unsigned long long)total);
			for (const auto& [a, c]: s_exec_counts) {
				std::fprintf(stderr, " 0x%llx:%llu", (unsigned long long)a,
				             (unsigned long long)c);
			}
			std::fprintf(stderr, "\n");
		}
	}
}

void MetalRenderer::DispatchDirect(uint64_t submit_id, const HW::Context& context, const HW::Shader& sh_ctx,
                                   PipelineCache& pipeline_cache, uint32_t thread_group_x,
                                   uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode) {
	DispatchCompute(submit_id, context, sh_ctx, pipeline_cache, thread_group_x, thread_group_y, thread_group_z, mode, 0);
}

// See BcLevelImage. Candidates of the dispatch being encoded (render thread only).
static std::vector<uint64_t> s_bc_written_now;

static bool BcWritebackEnabled() {
	static const bool s_off = false;
	return !s_off && !UmaSurfacesEnabled(); // UMA storage images already are views of guest memory
}

// What a BC encoder stores blocks through. Kernels also keep screen sized data in 8-byte integer images (half-resolution RG32Uint
// buffers, 800x452 and up with the dynamic resolution): those are no texture.
static bool BcBlockImage(MTLPixelFormat fmt, uint32_t w, uint32_t h) {
	if (fmt == MTLPixelFormatRGBA32Uint) return true;
	if (fmt != MTLPixelFormatRGBA16Uint && fmt != MTLPixelFormatRG32Uint) return false;
	return (std::has_single_bit(w) && std::has_single_bit(h)) || (w < 400 && h < 400);
}

// One level of an image, bound on its own (T# base == last, chain length in MAX_MIP; a 128-bit T# has no MAX_MIP).
static bool BcOneLevel(const ShaderTextureResource& t, uint32_t w, uint32_t h, bool r128, bool dynamic_mip, uint32_t& level, uint32_t& levels) {
	level = t.BaseLevel();
	levels = r128 ? static_cast<uint32_t>(t.LastLevel()) + 1u : static_cast<uint32_t>(t.MaxMip()) + 1u;
	return !dynamic_mip && level == t.LastLevel() && levels > level && (!r128 || levels == 1u) && levels <= std::bit_width(std::max(w, h));
}

// The image a write-only kernel stores blocks through. Copy kernels fill a streaming texture tile by tile, so the written blocks accumulate
// until the texture is replaced by one of another size. Idle ones go after 10 minutes: the decoded copies keep the content.
static id<MTLTexture> BcLevelImageFor(const ShaderTextureResource& t, uint64_t addr, uint32_t w, uint32_t h, bool r128, bool dynamic_mip, bool write_only) {
	if (!BcWritebackEnabled() || !write_only || t.TileMode() == Prospero::TileMode::kLinear || t.Type() != Prospero::ImageType::kColor2D) return nil;
	const MTLPixelFormat fmt = MetalFormatFromBufferFormat(t.Format());
	if (!BcBlockImage(fmt, w, h)) return nil;
	uint32_t level = 0, levels = 1;
	if (!BcOneLevel(t, w, h, r128, dynamic_mip, level, levels)) return nil;
	{
		std::lock_guard<std::mutex> lock(s_render_target_mutex);
		if (s_rt_by_addr.find(addr) != s_rt_by_addr.end()) return nil;
	}
	const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	static uint64_t s_sweep = 0;
	if (flip >= s_sweep + 300) {
		s_sweep = flip;
		std::erase_if(s_bc_level_images, [&](const auto& e) { return e.second.used_flip + 18000 < flip; });
	}
	const uint32_t lw = std::max(w >> level, 1u), lh = std::max(h >> level, 1u), web = fmt == MTLPixelFormatRGBA32Uint ? 16u : 8u;
	BcLevelImage&  img = s_bc_level_images[{addr, level}];
	if (img.tex == nil || img.tex.width != lw || img.tex.height != lh || img.tex.pixelFormat != fmt) {
		MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:lw height:lh mipmapped:NO];
		d.usage = GuestTextureUsage(fmt) | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
		d.storageMode = MTLStorageModeShared;
		img.tex = KytyNewTexture(MetalContext::Instance().GetDevice(), d, __LINE__);
		if (img.tex == nil) return nil;
		img.web = web;
		const std::vector<uint32_t> fill(static_cast<size_t>(lw) * lh * web / 4u, kBcUnwritten);
		[img.tex replaceRegion:MTLRegionMake2D(0, 0, lw, lh) mipmapLevel:0 withBytes:fill.data() bytesPerRow:lw * web];
	}
	img.used_flip = flip;
	img.tile = t.TileMode();
	img.guest_bytes = static_cast<uint64_t>(lw) * lh * web;
	img.guest_hash = GuestTexSourceHash(addr, img.guest_bytes);
	return img.tex;
}

static void NoteBcWriteback(uint64_t addr, id<MTLTexture> tex, const ShaderTextureResource& t, uint32_t w, uint32_t h, bool r128, bool dynamic_mip) {
	uint32_t level = 0, levels = 1;
	if (!BcOneLevel(t, w, h, r128, dynamic_mip, level, levels)) return;
	static const bool s_log = KYTY_ENV("KYTY_LOG_BC_WRITEBACK") != nullptr;
	if (s_log) {
		static std::set<std::tuple<uint64_t, uint32_t, uint32_t>> s_seen;
		// The staging blocks of the copy kernels (hundreds of small single-level images) would fill the log before the kits appear.
		if ((w >= 64 || h >= 64 || levels > 1u) && s_seen.size() < 4000 && s_seen.emplace(addr, level, levels).second)
			std::fprintf(stderr, "[BcWritebackDesc] flip=%llu addr=0x%llx %ux%u fmt=%u tile=%u base=%u last=%u max=%u texture=%lux%lu cs=0x%016llx\n",
			             (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed), (unsigned long long)addr, w, h, (unsigned)t.Format(), (unsigned)t.TileMode(),
			             (unsigned)t.BaseLevel(), (unsigned)t.LastLevel(), (unsigned)t.MaxMip(), (unsigned long)tex.width, (unsigned long)tex.height, (unsigned long long)s_alloc_cs_hash);
	}
	s_bc_written_now.push_back(addr);
	s_bc_written_addrs.insert(addr);
	// The decoded copies of this address are out of date now, and guest memory does not change: decode them again.
	if (const auto g = s_guest_textures.find(addr); g != s_guest_textures.end()) {
		g->second.cs_written = false;
		g->second.src_hash ^= 1u;
	}
}

// After the dispatch: the decode reads the level images on the CPU, whenever a reader first asks. Only load screens encode textures.
static void FinishBcWriteback() {
	if (s_bc_written_now.empty()) return;
	s_bc_written_now.clear();
	MetalContext::Instance().WaitUntilIdle();
}

// KYTY_CS_OVERLAP: how many guest bytes a compute image stands for (its padded guest pitch is not known here: a quarter more, and
// a whole mip chain counts double). Too large only costs overlap, too small would lose a dependency.
static uint64_t CsAccessBytes(id<MTLTexture> t) {
	uint64_t bytes = static_cast<uint64_t>(t.width) * t.height * std::max<NSUInteger>(t.depth, 1) * std::max<NSUInteger>(t.arrayLength, 1) *
	                 std::max<uint32_t>(BytesPerPixel(t.pixelFormat), 1u);
	if (t.mipmapLevelCount > 1) bytes *= 2;
	return std::max<uint64_t>(bytes + bytes / 4, 4096);
}

#include "graphics/host_gpu/metal/metalReplayDump.inc"

void MetalRenderer::DispatchCompute(uint64_t submit_id, const HW::Context& context, const HW::Shader& sh_ctx,
                                    PipelineCache& pipeline_cache, uint32_t thread_group_x,
                                    uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode,
                                    uint64_t indirect_args_addr) {
	AllocProbe alloc_probe(2);
	CountDispatchStat(1);
	// Resources reached only through GPU pointers in arg_ptrs: made resident on the compute encoder below.
	std::vector<id<MTLResource>> cs_residency;
	s_trace_submit_id = submit_id;
	(void)mode;
	if (!m_active || sh_ctx.GetCs().cs_regs.data_addr == 0) return;
	s_bc_written_now.clear();
	if (thread_group_x == 0 || thread_group_y == 0 || thread_group_z == 0) return;
	std::lock_guard lock(m_mutex);
	++s_image_read_epoch;

	ShaderComputeInputInfo compute_info {};
	auto compute_program = pipeline_cache.GetComputeProgram(
	    sh_ctx.GetCs(), context.GetShaderRegisters(), compute_info);
	NoteStageGuestWrites(compute_info.stage);

	id<MTLComputePipelineState> pstate = nil;
	uint64_t cs_hash = compute_info.stage.program ? compute_info.stage.program->shader_hash : 0;
	// KYTY_CS_BUF_WRITERS=<metal or guest hash>,<buffer index>: retain recent compute buffer write bindings and
	// report candidates overlapping the selected input at each dispatch. Bindings are candidates,
	// not proof of actual writes; DMA, render targets and image writes are outside this diagnostic.
	if (const char* watch = KYTY_ENV("KYTY_CS_BUF_WRITERS"); watch != nullptr && compute_info.stage.program && compute_info.stage.resources) {
		struct Writer { uint64_t addr, size, hash, flip; size_t index; uint32_t gx, gy, gz, fill_value; bool fill; };
		static std::deque<Writer> writers;
		unsigned long long target = 0;
		unsigned watched_index = 0;
		const auto& buffers = compute_info.stage.resources->buffers;
		const auto& info = compute_info.stage.program->info.buffers;
		const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
		if (std::sscanf(watch, "%llx,%u", &target, &watched_index) == 2 &&
		    (cs_hash == target || compute_info.stage.program->guest_hash == target) && watched_index < buffers.size()) {
			const auto& desc = buffers[watched_index];
			const uint64_t addr = (static_cast<uint64_t>(desc.dwords[1] & 0xffffu) << 32) | desc.dwords[0];
			const uint64_t size = VsharpBytes(desc.dwords);
			if (addr != 0 && size != 0) {
				std::fprintf(stderr, "[CsBufWriters] consumer=0x%016llx flip=%llu b%u addr=0x%llx size=%llu\n",
				             target, (unsigned long long)flip, watched_index, (unsigned long long)addr, (unsigned long long)size);
				std::set<std::pair<uint64_t, size_t>> seen;
				for (auto it = writers.rbegin(); it != writers.rend() && seen.size() < 12; ++it) {
					if (it->addr < addr + size && addr < it->addr + it->size && seen.insert({it->hash, it->index}).second) {
						std::fprintf(stderr, "[CsBufWriters]   cs=0x%016llx flip=%llu b%zu addr=0x%llx size=%llu groups=%ux%ux%u fill=%d value=%08x\n",
						             (unsigned long long)it->hash, (unsigned long long)it->flip, it->index,
						             (unsigned long long)it->addr, (unsigned long long)it->size, it->gx, it->gy, it->gz,
						             it->fill ? 1 : 0, it->fill_value);
					}
				}
				{
					std::lock_guard<std::mutex> surface_lock(s_recent_surface_mutex);
					std::set<std::pair<const char*, uint64_t>> surfaces;
					for (auto it = s_recent_surface_ops.rbegin(); it != s_recent_surface_ops.rend() && surfaces.size() < 8; ++it) {
						if (it->addr < addr + size && addr < it->addr + it->bytes && surfaces.insert({it->op, it->addr}).second) {
							std::fprintf(stderr, "[CsBufWriters]   image=%s flip=%llu addr=0x%llx bytes=%llu cs=0x%016llx\n", it->op,
							             (unsigned long long)it->flip, (unsigned long long)it->addr, (unsigned long long)it->bytes,
							             (unsigned long long)it->hash);
						}
					}
				}
			}
		}
		for (size_t i = 0; i < buffers.size() && i < info.size(); ++i) {
			if (!info[i].written) continue;
			const auto& desc = buffers[i];
			const uint64_t addr = (static_cast<uint64_t>(desc.dwords[1] & 0xffffu) << 32) | desc.dwords[0];
			const uint64_t size = VsharpBytes(desc.dwords);
			const auto& fill = compute_info.stage.resources->uniform_fill;
			const bool is_fill = fill.kind == ShaderRecompiler::IR::UniformFillKind::Buffer && fill.resource == i;
			if (addr != 0 && size != 0) writers.push_back({addr, size, cs_hash, flip, i, thread_group_x, thread_group_y, thread_group_z, fill.value, is_fill});
		}
		while (writers.size() > 16384) writers.pop_front();
	}
	s_alloc_cs_hash = cs_hash;
	if (PassTimerEnabled()) {
		char label[48];
		std::snprintf(label, sizeof(label), "cs=%016llx", static_cast<unsigned long long>(cs_hash));
		PassTimerNote(label);
	}
	TraceComputeOp(compute_info, cs_hash);
	// KYTY_PASS_TRACE_CS: this dispatch is traced on its own (PassTraceForcedKernel); the first dwords of its buffers are printed here.
	struct PassForce {
		explicit PassForce(bool on) { s_pass_force = on; }
		~PassForce() { s_pass_force = false; }
	} pass_force(PassTraceForcedKernel(cs_hash));
	if (s_pass_force && compute_info.stage.resources) {
		const auto& bufs = compute_info.stage.resources->buffers;
		for (size_t b = 0; b < bufs.size() && b < 4; ++b) {
			const uint64_t base = (static_cast<uint64_t>(bufs[b].dwords[1] & 0xFFFF) << 32) | bufs[b].dwords[0];
			uint32_t       dw[64] = {};
			const size_t   n = std::min<size_t>(64, VsharpBytes(bufs[b].dwords) / 4);
			if (base == 0 || n == 0 || !Libs::LibKernel::Memory::TryReadBacking(base, dw, n * 4)) continue;
			std::string text;
			for (size_t k = 0; k < n; ++k) {
				float f;
				std::memcpy(&f, &dw[k], 4);
				char t[48];
				std::snprintf(t, sizeof(t), "%s[%zx]=%08x(%g)", (k % 8) == 0 ? "\n[PassTrace]       " : " ", k, dw[k], f);
				text += t;
			}
			std::fprintf(stderr, "[PassTrace] cs=0x%016llx flip=%llu BUF%zu 0x%llx bytes=%u:%s\n", static_cast<unsigned long long>(cs_hash),
			             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)), b,
			             static_cast<unsigned long long>(base), VsharpBytes(bufs[b].dwords), text.c_str());
		}
	}
	// KYTY_CS_BUF_STATS=<hash>: the first 64 dispatches of that kernel print every buffer (address, bytes, share of non-zero dwords, the
	// first dwords) as the guest memory holds it when the dispatch is encoded (tells an empty or stale input from a wrong kernel).
	if (const char* bs = KYTY_ENV("KYTY_CS_BUF_STATS"); bs != nullptr && compute_info.stage.resources && [&] {
		    for (const char* p = bs; *p != '\0';) {
			    char* end = nullptr;
			    const uint64_t h = std::strtoull(p, &end, 0);
			    if (end == p) return false;
			    if (h == cs_hash || (compute_info.stage.program && h == compute_info.stage.program->guest_hash)) return true;
			    p = *end == ',' ? end + 1 : end;
		    }
		    return false;
	    }()) {
		static std::atomic<uint32_t> s_bs_n {0};
		// KYTY_CS_BUF_STATS_EVERY=<n>: one report per n dispatches instead of the first 64 only.
		static const uint32_t s_bs_every = KYTY_ENV("KYTY_CS_BUF_STATS_EVERY") != nullptr ? static_cast<uint32_t>(std::strtoul(KYTY_ENV("KYTY_CS_BUF_STATS_EVERY"), nullptr, 10)) : 0u;
		const uint32_t bs_i = s_bs_n.fetch_add(1);
		if (s_bs_every != 0 ? bs_i % s_bs_every == 0 : bs_i < 64) {
			const auto& bufs = compute_info.stage.resources->buffers;
			std::string text;
			for (size_t b = 0; b < bufs.size(); ++b) {
				const uint64_t base  = (static_cast<uint64_t>(bufs[b].dwords[1] & 0xFFFF) << 32) | bufs[b].dwords[0];
				const uint32_t bytes = VsharpBytes(bufs[b].dwords);
				const size_t   n     = std::min<size_t>(bytes / 4, 1u << 20);
				std::vector<uint32_t> dw(n);
				size_t nz = 0, nans = 0;
				uint32_t mx = 0;
				if (base != 0 && n != 0 && Libs::LibKernel::Memory::TryReadBacking(base, dw.data(), n * 4)) {
					for (uint32_t v: dw) {
						nz += v != 0 ? 1 : 0;
						nans += (v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffffu) != 0u ? 1 : 0;
						mx = std::max(mx, v);
					}
				}
				char t[320];
				std::snprintf(t, sizeof(t), "\n[CsBufStats]   b%zu 0x%llx bytes=%u nonzero=%.3f nan=%.3f max=%08x first=%08x %08x %08x %08x desc=%08x,%08x,%08x packed_stride=%08x", b,
				              static_cast<unsigned long long>(base), bytes, n != 0 ? static_cast<double>(nz) / static_cast<double>(n) : 0.0, n != 0 ? static_cast<double>(nans) / static_cast<double>(n) : 0.0, mx,
				              n > 0 ? dw[0] : 0u, n > 1 ? dw[1] : 0u, n > 2 ? dw[2] : 0u, n > 3 ? dw[3] : 0u,
				              bufs[b].dwords[1], bufs[b].dwords[2], bufs[b].dwords[3],
				              compute_info.stage.program && b < compute_info.stage.program->info.buffers.size() ? compute_info.stage.program->info.buffers[b].packed_stride : 0u);
				text += t;
			}
			std::fprintf(stderr, "[CsBufStats] cs=0x%016llx flip=%llu groups=%ux%ux%u:%s\n", static_cast<unsigned long long>(cs_hash),
			             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)),
			             thread_group_x, thread_group_y, thread_group_z, text.c_str());
		}
	}
	// A recognised uniform fill (memory clear kernel): if it clears a colour target's DCC/CMASK metadata, queue the target's clear.
	if (indirect_args_addr == 0 && compute_info.stage.resources &&
	    compute_info.stage.resources->uniform_fill.kind == ShaderRecompiler::IR::UniformFillKind::Buffer) {
		const auto& fill = compute_info.stage.resources->uniform_fill;
		if (fill.resource < compute_info.stage.resources->buffers.size()) {
			const auto& fd = compute_info.stage.resources->buffers[fill.resource];
			const uint64_t fill_addr = (static_cast<uint64_t>(fd.dwords[1] & 0xFFFF) << 32) | fd.dwords[0];
			NoteUniformMetadataFill(fill_addr, VsharpBytes(fd.dwords), fill.value);
		}
	}
	{
		// KYTY_LOG_CS_WRITES=<lo>,<hi>: compute dispatches that write a buffer based inside [lo,hi) (who produces vertex streams etc.).
		static uint64_t s_wlo = 0, s_whi = 0;
		static std::once_flag s_w_once;
		std::call_once(s_w_once, [] {
			if (const char* v = KYTY_ENV("KYTY_LOG_CS_WRITES")) {
				char* e = nullptr;
				s_wlo = std::strtoull(v, &e, 0);
				if (*e == ',') s_whi = std::strtoull(e + 1, nullptr, 0);
			}
		});
		static std::atomic<uint32_t> s_w_n {0};
		const uint64_t wf = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
		if (s_whi > s_wlo && wf >= 2000 && compute_info.stage.program && compute_info.stage.resources && s_w_n.load() < 3000) {
			const auto& wp = *compute_info.stage.program;
			const auto& wr = *compute_info.stage.resources;
			for (size_t bi = 0; bi < wp.info.buffers.size() && bi < wr.buffers.size(); ++bi) {
				if (!wp.info.buffers[bi].written) continue;
				const uint64_t base = (static_cast<uint64_t>(wr.buffers[bi].dwords[1] & 0xFFFF) << 32) | wr.buffers[bi].dwords[0];
				if (base >= s_wlo && base < s_whi && s_w_n.fetch_add(1) < 3000) {
					for (size_t ob = 0; ob < wp.info.buffers.size() && ob < wr.buffers.size() && ob < 6 && s_w_n.load() < 40; ++ob) {
						const uint64_t obase = (static_cast<uint64_t>(wr.buffers[ob].dwords[1] & 0xFFFF) << 32) | wr.buffers[ob].dwords[0];
						std::fprintf(stderr, "[CsWrite]    in-b%zu base=0x%llx nrec=%u stride=%u size=%u read=%d written=%d\n", ob, static_cast<unsigned long long>(obase),
						             wr.buffers[ob].dwords[2], (wr.buffers[ob].dwords[1] >> 16) & 0x3fffu, VsharpBytes(wr.buffers[ob].dwords),
						             wp.info.buffers[ob].read ? 1 : 0, wp.info.buffers[ob].written ? 1 : 0);
					}
					std::fprintf(stderr, "[CsWrite] flip=%llu cs=0x%016llx b%zu base=0x%llx nrec=%u stride=%u groups=%ux%ux%u read=%d\n",
					             static_cast<unsigned long long>(wf), static_cast<unsigned long long>(cs_hash), bi, static_cast<unsigned long long>(base),
					             wr.buffers[bi].dwords[2], (wr.buffers[bi].dwords[1] >> 16) & 0x3fffu, thread_group_x, thread_group_y, thread_group_z,
					             wp.info.buffers[bi].read ? 1 : 0);
				}
			}
		}
	}
	// A dispatch that only writes a depth target's HTILE range is its fast clear (the Vulkan TryConsumeComputeMetaClear rule).
	if (indirect_args_addr == 0 && compute_info.stage.program && compute_info.stage.resources &&
	    !compute_info.stage.program->info.has_bitwise_xor) {
		const auto& hprog = *compute_info.stage.program;
		const auto& hres  = *compute_info.stage.resources;
		for (size_t bi = 0; bi < hprog.info.buffers.size() && bi < hres.buffers.size(); ++bi) {
			if (!hprog.info.buffers[bi].written || hprog.info.buffers[bi].read) continue;
			NoteHtileWrite((static_cast<uint64_t>(hres.buffers[bi].dwords[1] & 0xFFFF) << 32) | hres.buffers[bi].dwords[0]);
		}
	}
	{
		// KYTY_LOG_CS=<hash>: which dispatch path runs for the probed compute shader, and its images.
		static const uint64_t s_log_cs = KYTY_ENV("KYTY_LOG_CS") ? std::strtoull(KYTY_ENV("KYTY_LOG_CS"), nullptr, 0) : 0ull;
		static std::atomic<uint32_t> s_log_cs_n {0};
		if (s_log_cs != 0 && cs_hash == s_log_cs && s_log_cs_n.fetch_add(1) < 4) {
			std::fprintf(stderr, "[CsDispatch] hash=0x%016llx native_msl=%d resources=%d images=%zu groups=%ux%ux%u\n",
			             (unsigned long long)cs_hash, compute_info.stage.program && compute_info.stage.program->native_msl ? 1 : 0,
			             compute_info.stage.resources ? 1 : 0,
			             compute_info.stage.resources ? compute_info.stage.resources->images.size() : 0,
			             thread_group_x, thread_group_y, thread_group_z);
			if (compute_info.stage.resources) {
				const auto& cb = compute_info.stage.resources->buffers;
				std::fprintf(stderr, "[CsDispatch]   snapshot buffers=%zu", cb.size());
				for (size_t bi = 0; bi < cb.size() && bi < 4; ++bi) {
					std::fprintf(stderr, " b%zu=0x%llx/%u", bi,
					             (unsigned long long)((static_cast<uint64_t>(cb[bi].dwords[1] & 0xFFFF) << 32) | cb[bi].dwords[0]),
					             cb[bi].dwords[2]);
				}
				std::fprintf(stderr, "\n");
			}
			if (compute_info.stage.program) {
				for (const auto& b: compute_info.stage.program->bindings.descriptors) {
					const auto kd = static_cast<uint32_t>(b.kind);
					if (kd < ShaderRecompiler::IR::FirstImageBinding ||
					    kd >= static_cast<uint32_t>(ShaderRecompiler::IR::DescriptorBindingKind::Samplers)) {
						continue;
					}
					for (uint32_t res: b.resources) {
						if (compute_info.stage.resources && res < compute_info.stage.resources->images.size()) {
							const auto& d = compute_info.stage.resources->images[res];
							ShaderTextureResource tx {};
							for (size_t f = 0; f < 8; ++f) tx.fields[f] = d.dwords[f];
							std::fprintf(stderr, "[CsDispatch]   image kind=%u class=%d addr=0x%llx %ux%u\n", kd,
							             static_cast<int>(ShaderRecompiler::IR::ImageBindingResourceClass(b.kind)),
							             (unsigned long long)tx.Base40(), tx.Width5() + 1u, tx.Height5() + 1u);
						}
					}
				}
			}
		}
	}
	if (const char* rng = KYTY_ENV("KYTY_LOG_DRAWS_FLIP")) {
		unsigned lo = 0, hi = 0;
		std::sscanf(rng, "%u,%u", &lo, &hi);
		const uint64_t fl = Libs::Graphics::TraceWindow::g_flip.load();
		if (fl >= lo && fl <= hi && compute_info.stage.program != nullptr) {
			std::string imgs;
			if (compute_info.stage.resources) {
				for (const auto& b: compute_info.stage.program->bindings.descriptors) {
					const auto kd = static_cast<uint32_t>(b.kind);
					if (kd < ShaderRecompiler::IR::FirstImageBinding || kd >= static_cast<uint32_t>(ShaderRecompiler::IR::DescriptorBindingKind::Samplers)) continue;
					for (uint32_t res: b.resources) {
						if (res >= compute_info.stage.resources->images.size()) continue;
						ShaderTextureResource tx {};
						for (size_t f = 0; f < 8; ++f) tx.fields[f] = compute_info.stage.resources->images[res].dwords[f];
						char buf[80];
						std::snprintf(buf, sizeof(buf), " %c@%llx(%ux%u)", kd >= ShaderRecompiler::IR::FirstStorageImageBinding ? 'W' : 'R',
						              (unsigned long long)tx.Base40(), tx.Width5() + 1u, tx.Height5() + 1u);
						imgs += buf;
					}
				}
			}
			std::fprintf(stderr, "[CsF] flip=%llu hash=%016llx groups=%u,%u,%u%s\n", (unsigned long long)fl, (unsigned long long)cs_hash,
			             thread_group_x, thread_group_y, thread_group_z, imgs.c_str());
		}
	}
	if (cs_hash != 0) {
		id<MTLFunction> cs_func = MetalShaderManager::Instance().GetFunction(cs_hash);
		if (cs_func) {
			pstate = MetalPipelineManager::Instance().GetOrCreateComputePipeline(
			    cs_hash, cs_func);
		}
		if (pstate == nil) {
			std::lock_guard<std::mutex> cl(s_cs_skip_mutex);
			++s_cs_skip_count[cs_hash];
			++s_cs_skip_total;
		}
		// Skipped dispatch (no Metal function, so no pipeline): log once per hash what it would write.
		if (pstate == nil && compute_info.stage.program != nullptr) {
			static std::mutex s_skip_mutex;
			static std::unordered_set<uint64_t> s_skip_logged;
			std::lock_guard<std::mutex> skip_lock(s_skip_mutex);
			if (s_skip_logged.insert(cs_hash).second) {
				const auto& sprog = *compute_info.stage.program;
				std::fprintf(stderr, "[SKIPPED CS] hash=0x%016llx writes:", (unsigned long long)cs_hash);
				if (compute_info.stage.resources) {
					for (const auto& b: sprog.bindings.descriptors) {
						if (static_cast<uint32_t>(b.kind) < static_cast<uint32_t>(ShaderRecompiler::IR::FirstStorageImageBinding)) continue;
						for (uint32_t res: b.resources) {
							if (res < compute_info.stage.resources->images.size()) {
								ShaderTextureResource wt {};
								for (size_t f = 0; f < 8; ++f) wt.fields[f] = compute_info.stage.resources->images[res].dwords[f];
								std::fprintf(stderr, " img@0x%llx(%ux%u)", (unsigned long long)wt.Base40(),
								             (unsigned)(wt.Width5() + 1u), (unsigned)(wt.Height5() + 1u));
							}
						}
					}
				}
				std::fprintf(stderr, "\n");
			}
		}
		static const uint64_t s_log_pipe = KYTY_ENV("KYTY_LOG_CS") ? std::strtoull(KYTY_ENV("KYTY_LOG_CS"), nullptr, 0) : 0ull;
		static std::atomic<uint32_t> s_log_pipe_n {0};
		if (s_log_pipe != 0 && cs_hash == s_log_pipe && s_log_pipe_n.fetch_add(1) < 4) {
			std::fprintf(stderr, "[CsPipe] hash=0x%016llx cs_func=%p pstate=%p dispatch_group=%ux%ux%u\n",
			             (unsigned long long)cs_hash, (__bridge void*)cs_func, (__bridge void*)pstate, thread_group_x, thread_group_y, thread_group_z);
		}
	}

	if (pstate == nil) RejectMetalState("compute shader/pipeline compilation failed: " + std::to_string(cs_hash));
	// The argument producer can still be queued. Snapshot, convert and clamp on
	// the GPU before binding the consumer's pipeline or resources.
	id<MTLBuffer> indirect_args = nil;
	s_trace_cs_args_addr = indirect_args_addr;
	if (indirect_args_addr != 0) {
		const auto source = MetalBufferManager::Instance().GetBufferForGuestRange(indirect_args_addr, 12);
		if (!source.IsValid()) return;
		const MTLSize guest_threads = MTLSizeMake(compute_info.threads_num[0] > 0 ? compute_info.threads_num[0] : 64u,
		                                        compute_info.threads_num[1] > 0 ? compute_info.threads_num[1] : 1u,
		                                        compute_info.threads_num[2] > 0 ? compute_info.threads_num[2] : 1u);
		MetalContext::Instance().EndCurrentEncoder();
		static MetalComputeIndirect converter;
		indirect_args = converter.Encode(MetalContext::Instance().GetCurrentCommandBuffer(), source.buffer, source.offset,
		                                 guest_threads, (mode & (1u << 5u)) != 0);
		if (!indirect_args) return;
		// Diagnostic for a hung indirect dispatch (printed by the GPU watchdog): which GPU-written images lie over these arguments.
		const std::string cover = GpuSurfacesCovering(indirect_args_addr, 12);
		KytyNoteComputeIndirect(cs_hash, indirect_args_addr, cover.c_str());
		if (!cover.empty()) {
			static std::mutex s_cover_mutex;
			static std::set<std::pair<uint64_t, uint64_t>> s_cover_logged;
			std::lock_guard<std::mutex> cover_lock(s_cover_mutex);
			if (s_cover_logged.size() < 200 && s_cover_logged.insert({cs_hash, indirect_args_addr}).second)
				std::fprintf(stderr, "[IndirectArgs] cs=0x%016llx args=0x%llx covered by: %s\n", (unsigned long long)cs_hash,
				             (unsigned long long)indirect_args_addr, cover.c_str());
		}
	}
	{
		char fault_label[128];
		if (indirect_args_addr != 0)
			std::snprintf(fault_label, sizeof(fault_label), "cs hash=0x%016llx indirect args=0x%llx",
			              (unsigned long long)cs_hash, (unsigned long long)indirect_args_addr);
		else std::snprintf(fault_label, sizeof(fault_label), "cs hash=0x%016llx", (unsigned long long)cs_hash);
		KytyFaultOpSet(fault_label);
	}
	PrepareGpuDetiledSources(compute_info.stage);
	EXIT_IF(!PrepareLayeredImages(compute_info.stage));
	std::map<uint32_t,CanonicalMipBinding> canonical_images;
	std::vector<std::function<void(id)>> canonical_writes;
	if (compute_info.stage.program && compute_info.stage.resources) {
		const auto& prog=*compute_info.stage.program;
		for (const auto& binding:prog.bindings.descriptors) {
			const auto kind=static_cast<uint32_t>(binding.kind);
			if(kind<ShaderRecompiler::IR::FirstImageBinding || kind>=static_cast<uint32_t>(ShaderRecompiler::IR::DescriptorBindingKind::Samplers))continue;
			for(uint32_t res:binding.resources) {
				if(res>=compute_info.stage.resources->images.size() || res>=prog.info.images.size())continue;
				ShaderTextureResource tex{};std::copy_n(compute_info.stage.resources->images[res].dwords.begin(),8,tex.fields);
				auto image=CanonicalMipImage(tex,tex.Width5()+1u,tex.Height5()+1u,tex.Base40(),prog.info.images[res].r128);
				if(!image.image && kind<ShaderRecompiler::IR::FirstStorageImageBinding &&
				    prog.info.images[res].dimension==ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
				    tex.Type()==Prospero::ImageType::kColor2D && tex.BaseLevel()==0 && tex.LastLevel()==0)
					image.image=AliasConvert(tex.Base40(),tex.Width5()+1u,tex.Height5()+1u,tex.Format(),tex.TileMode());
				if(image.image) {
					if(kind>=ShaderRecompiler::IR::FirstStorageImageBinding && IsCompressedFormat(image.image.pixelFormat))
						RejectMetalState("compressed storage image requires an uncompressed block descriptor");
					canonical_images[res]=image;
					if(kind>=ShaderRecompiler::IR::FirstStorageImageBinding)canonical_writes.push_back(image.writeback);
				}
			}
		}
	}

	// KYTY_CS_BUF_ORDERED=<metal hash>,<buffer index>,<first flip>: copy a compute input in GPU order
	// immediately before dispatch. Diagnostic only: this splits the command stream and optionally
	// waits (KYTY_CS_BUF_ORDERED_WAIT=1), but includes earlier queued GPU writes.
	if (const char* probe = KYTY_ENV("KYTY_CS_BUF_ORDERED"); probe != nullptr && compute_info.stage.resources) {
		unsigned long long hash = 0, first_flip = 0;
		unsigned index = 0;
		const int fields = std::sscanf(probe, "%llx,%u,%llu", &hash, &index, &first_flip);
		const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
		const auto& buffers = compute_info.stage.resources->buffers;
		if (fields >= 2 && hash == cs_hash && flip >= first_flip && index < buffers.size()) {
			const auto& desc = buffers[index];
			const uint64_t addr = (static_cast<uint64_t>(desc.dwords[1] & 0xffffu) << 32) | desc.dwords[0];
			const size_t bytes = std::min<size_t>(VsharpBytes(desc.dwords) & ~size_t(3), 4u * 1024u * 1024u);
			const auto src = MetalBufferManager::Instance().GetBufferForGuestRange(addr, bytes);
			auto& metal = MetalContext::Instance();
			id<MTLBuffer> copy = bytes != 0 ? KytyNewBuffer(metal.GetDevice(), bytes, MTLResourceStorageModeShared, __LINE__) : nil;
			if (src.IsValid() && copy != nil && src.offset <= src.buffer.length && bytes <= src.buffer.length - src.offset) {
				metal.EndCurrentEncoder();
				id<MTLCommandBuffer> command = metal.GetCurrentCommandBuffer();
				id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
				if (blit != nil) {
					OrderBegin(blit);
					[blit copyFromBuffer:src.buffer sourceOffset:src.offset toBuffer:copy destinationOffset:0 size:bytes];
					OrderEnd(blit);
					[blit endEncoding];
					[command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
					if (completed.status == MTLCommandBufferStatusCompleted) {
						const auto* words = static_cast<const uint32_t*>(copy.contents);
						size_t nz = 0, large = 0, max_index = 0, tile_large = 0;
						uint32_t maximum = 0, tile_max = 0;
						for (size_t i = 0; i < bytes / 4; ++i) {
							nz += words[i] != 0;
							large += words[i] > 8192u * 64u;
							if (words[i] > maximum) { maximum = words[i]; max_index = i; }
							if (i < 8192) { tile_large += words[i] > 8192u * 64u; tile_max = std::max(tile_max, words[i]); }
						}
						std::fprintf(stderr, "[CsBufOrdered] cs=0x%016llx flip=%llu b%u addr=0x%llx bytes=%zu nonzero=%zu large=%zu max=%08x max_index=%zu first8192_max=%08x first8192_large=%zu first=%08x %08x %08x %08x\n",
						             hash, (unsigned long long)flip, index, (unsigned long long)addr, bytes, nz, large, maximum, max_index, tile_max, tile_large,
						             bytes >= 4 ? words[0] : 0u, bytes >= 8 ? words[1] : 0u, bytes >= 12 ? words[2] : 0u, bytes >= 16 ? words[3] : 0u);
						if (large != 0 && KYTY_ENV("KYTY_CS_BUF_WRITERS") != nullptr) {
							std::lock_guard<std::mutex> surface_lock(s_recent_surface_mutex);
							std::set<std::pair<const char*, uint64_t>> surfaces;
							for (auto it = s_recent_surface_ops.rbegin(); it != s_recent_surface_ops.rend() && surfaces.size() < 12; ++it) {
								if (it->addr < addr + bytes && addr < it->addr + it->bytes && surfaces.insert({it->op, it->addr}).second) {
									std::fprintf(stderr, "[CsBufOrderedOverlap] sample_flip=%llu image=%s flip=%llu addr=0x%llx bytes=%llu cs=0x%016llx\n",
									             (unsigned long long)flip, it->op, (unsigned long long)it->flip,
									             (unsigned long long)it->addr, (unsigned long long)it->bytes, (unsigned long long)it->hash);
								}
							}
						}
					} else {
						std::fprintf(stderr, "[CsBufOrdered] cs=0x%016llx flip=%llu GPU copy failed status=%ld\n", hash, (unsigned long long)flip, (long)completed.status);
					}
					}];
					metal.CommitCommandBuffer(KYTY_ENV("KYTY_CS_BUF_ORDERED_WAIT") != nullptr);
				} else {
					std::fprintf(stderr, "[CsBufOrdered] cs=0x%016llx flip=%llu could not create blit encoder\n", hash, (unsigned long long)flip);
				}
			} else {
				std::fprintf(stderr, "[CsBufOrdered] cs=0x%016llx flip=%llu invalid source or snapshot\n", hash, (unsigned long long)flip);
			}
		}
	}
	// KYTY_GDS_TRIP: drain and read the GDS work count before the consumer is encoded (metalReplayDump.inc).
	const uint64_t trip_guest = compute_info.stage.program ? compute_info.stage.program->guest_hash : 0;
	if (GdsTripWatched(trip_guest, cs_hash)) {
		GdsTripBeginWatch(trip_guest, cs_hash, thread_group_x, thread_group_y, thread_group_z, indirect_args_addr);
	}
	uint32_t   gds_trip_value = 0;
	const bool gds_trip       = compute_info.stage.program && GdsTripCheck(trip_guest, cs_hash, gds_trip_value);
	// KYTY_RACE_WATCH: snapshot the read-only inputs on the CPU now and on the GPU right before the dispatch (metalReplayDump.inc).
	if (compute_info.stage.program && compute_info.stage.resources && RaceWatched(trip_guest, cs_hash)) {
		RaceWatchMaybeStop();
		const auto& rw_info = compute_info.stage.program->info.buffers;
		const auto& rw_snap = *compute_info.stage.resources;
		for (size_t b = 0; b < rw_snap.buffers.size() && b < rw_info.size() && b < 64; ++b) {
			if (!rw_info[b].read || rw_info[b].written) continue;
			const auto&    desc = rw_snap.buffers[b];
			const uint64_t addr = (static_cast<uint64_t>(desc.dwords[1] & 0xFFFF) << 32) | desc.dwords[0];
			RaceWatchBuffer(cs_hash, b, addr, VsharpBytes(desc.dwords));
		}
	}
	const bool gds_snapshot = !gds_trip && GdsTripSnapshot(trip_guest, cs_hash);
	bool replay_dump_here = compute_info.stage.program && compute_info.stage.program->native_msl &&
	    (gds_trip || gds_snapshot || ReplayDumpWanted(compute_info.stage.program->guest_hash, cs_hash));
	if (replay_dump_here) {
		for (const auto& binding: compute_info.stage.program->bindings.descriptors) {
			using K = ShaderRecompiler::IR::DescriptorBindingKind;
			if (binding.kind == K::BdaPagetable) {
				std::fprintf(stderr,"[ReplayDump] unsupported global memory ABI guest=%016llx\n",(unsigned long long)compute_info.stage.program->guest_hash);
				replay_dump_here = false;
			}
		}
		if (ShaderHasArgumentBuffer0(cs_hash)) replay_dump_here = false;
	}
	if (gds_trip && !replay_dump_here) GdsTripFinish(GdsTripDir(trip_guest, cs_hash), trip_guest, cs_hash, gds_trip_value, false);
	const bool compute_pass_ok = MetalDrawExecutor::Instance().BeginComputePass();
	if (!compute_pass_ok) CountDispatchStat(4);
	if (compute_pass_ok && CsOverlapEnabled() && indirect_args_addr != 0) {
		MetalDrawExecutor::Instance().ComputeAccessAdd(indirect_args_addr, indirect_args_addr + 12, false);
	}
	if (compute_pass_ok) {
		if (pstate) {
			MetalDrawExecutor::Instance().SetComputePipeline(pstate);
		}
		if (id<MTLComputeCommandEncoder> cenc = MetalContext::Instance().GetActiveComputeEncoder()) {
			for (id<MTLResource> r: cs_residency) {
				MetalDrawExecutor::Instance().UseResource(r, ResidencyUsage(r));
			}
		}

		// 1. Push constants / user data registers (buffer 1)
		ShaderRecompiler::IR::PushData push_data {};
		if (compute_info.stage.program && compute_info.stage.resources) {
			const auto& prog = *compute_info.stage.program;
			const auto& snap = *compute_info.stage.resources;
			for (size_t i = 0; i < prog.bindings.user_data_registers.size(); ++i) {
				uint32_t reg = prog.bindings.user_data_registers[i];
				if (reg >= prog.user_data_base && (reg - prog.user_data_base) < snap.user_data.size()) {
					size_t dst = prog.bindings.push_data_start_dword + i;
					if (dst < push_data.dwords.size()) {
						push_data.dwords[dst] = snap.user_data[reg - prog.user_data_base];
					}
				}
			}
			for (size_t i = 0; i < 32 && i < snap.user_data.size(); ++i) {
				if (push_data.dwords[i] == 0) {
					push_data.dwords[i] = snap.user_data[i];
				}
			}
		}
		MetalDrawExecutor::Instance().SetComputeBytes(&push_data, sizeof(push_data), 1);

		// 2. Buffer 25: spvBufferSizeConstants (real sizes, so bounds checks work)
		// [64 + i]: depth-compare function of sampler slot i (see StageSamplerCompare).
		uint32_t sizes[ShaderRecompiler::Msl::SizeConstantWordCount];
		for (size_t i = 0; i < ShaderRecompiler::Msl::SizeConstantWordCount; ++i) sizes[i] = 0u;
		for (uint32_t i = 0; i < 32; ++i) {
			sizes[64 + i] = StageSamplerCompare(compute_info.stage, i);
			StoreSamplerDescriptor(compute_info.stage, i, sizes);
		}
		if (compute_info.stage.program) {
			const auto& info = compute_info.stage.program->info.buffers;
			for (size_t b = 0; b < info.size() && b < 64; ++b) {
				if (info[b].max_byte_extent != 0) {
					sizes[b] = info[b].max_byte_extent;
				}
			}
		}
		// The static extent misses dynamically indexed accesses (e.g. a table index in a register). The V#
		// record count is the buffer's real size, as the pixel stage already uses it; take the larger.
		if (compute_info.stage.program && compute_info.stage.resources) {
			const auto& info = compute_info.stage.program->info.buffers;
			const auto& res = *compute_info.stage.resources;
			for (size_t b = 0; b < res.buffers.size() && b < 64; ++b) {
				(void)info;
				const auto& cd = res.buffers[b];
				const uint64_t cbase = (static_cast<uint64_t>(cd.dwords[1] & 0xFFFF) << 32) | cd.dwords[0];
				if (cbase != 0) {
					sizes[b] = VsharpBytes(cd.dwords);
				} else sizes[b] = 0;
			}
		}
		MetalDrawExecutor::Instance().SetComputeBytes(sizes, sizeof(sizes), 25);

		// 3. Buffer 0: argument buffer with Metal GPU addresses.
		uint64_t arg_ptrs[256] = {0};
		std::array<MetalBufferAllocation, 64> cs_buffers {};
		std::array<id<MTLTexture>, 64> replay_images {};
		MetalBufferAllocation replay_srt {};
		if (compute_info.stage.resources) {
			const auto& snap = *compute_info.stage.resources;
			for (size_t b = 0; b < snap.buffers.size() && b < 64; ++b) {
				const auto& desc = snap.buffers[b];
				const uint64_t addr =
				    (static_cast<uint64_t>(desc.dwords[1] & 0xFFFF) << 32) | desc.dwords[0];
				if (addr == 0) {
					continue;
				}
				size_t size = b < 64 ? sizes[b] : 65536;
				// KYTY_LOG_BUF_COVER=<hex addr>: which compute dispatches bind a buffer that covers that guest address.
				if (const char* cov = KYTY_ENV("KYTY_LOG_BUF_COVER")) {
					char* comma = nullptr;
					const uint64_t target = std::strtoull(cov, &comma, 16);
					const uint64_t target_hi = (comma != nullptr && *comma == ',') ? std::strtoull(comma + 1, nullptr, 16) : target + 1;
					static std::atomic<uint32_t> s_cov_n {0};
					if (target < addr + size && target_hi > addr && s_cov_n.fetch_add(1) < 20) {
						std::fprintf(stderr, "[BufCover] hash=0x%016llx buf=%zu addr=0x%llx size=%zu written=%d flip=%llu\n",
						             (unsigned long long)(compute_info.stage.program ? compute_info.stage.program->shader_hash : 0), b,
						             (unsigned long long)addr, size,
						             compute_info.stage.program && b < compute_info.stage.program->info.buffers.size() &&
						                 compute_info.stage.program->info.buffers[b].written ? 1 : 0,
						             (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load());
					}
				}
				TraceFlipOverlap("compute_buf", addr, size != 0 ? size : 4096);
					// KYTY_LOG_UMA_BUF=1 (with KYTY_UMA_SURFACES): a raw buffer binding over guest memory that a guest-memory image view covers. Raw
					// address maths in the kernel assumes the guest tiling of that memory, the image views are linear: the two do not meet.
					{
						static const bool s_log_ub = KYTY_ENV("KYTY_LOG_UMA_BUF") != nullptr;
						if (s_log_ub && UmaSurfacesEnabled() && size != 0) {
							static std::mutex s_ub_mutex;
							static std::set<std::tuple<uint64_t, size_t, uint64_t>> s_ub_seen;
							const uint64_t ub_hash = compute_info.stage.program ? compute_info.stage.program->shader_hash : 0;
							const bool     ub_written = compute_info.stage.program && b < compute_info.stage.program->info.buffers.size() &&
							                        compute_info.stage.program->info.buffers[b].written;
							std::lock_guard<std::mutex> lock(s_gpusurf_mutex);
							for (const auto& [key, surf]: s_gpusurf) {
								const uint64_t iaddr  = std::get<0>(key);
								const uint64_t ibytes = static_cast<uint64_t>(std::get<2>(key)) * std::get<4>(key);
								if (iaddr < addr + size && iaddr + ibytes > addr) {
									std::lock_guard<std::mutex> lk(s_ub_mutex);
									if (s_ub_seen.size() < 200 && s_ub_seen.insert({ub_hash, b, iaddr}).second) {
										std::fprintf(stderr, "[UmaBuf] flip=%llu cs=0x%016llx buf=%zu addr=0x%llx size=%zu written=%d over image 0x%llx %ux%u mtlfmt=%u rowbytes=%u\n",
										             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load()),
										             static_cast<unsigned long long>(ub_hash), b, static_cast<unsigned long long>(addr), size, ub_written ? 1 : 0,
										             static_cast<unsigned long long>(iaddr), std::get<1>(key), std::get<2>(key), std::get<3>(key), std::get<4>(key));
									}
								}
							}
						}
					}
									const auto& buffer_program = *compute_info.stage.program;
				if (PassTraceActive() && b < buffer_program.info.buffers.size() && buffer_program.info.buffers[b].written &&
				    addr >= 0x8240000000ull && addr < 0x8250000000ull) {
					std::fprintf(stderr, "[PassTrace] BUFWRITE cs=0x%016llx flip=%llu res=%zu addr=0x%llx size=%zu read=%d\n",
					             (unsigned long long)buffer_program.shader_hash, (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(), b,
					             (unsigned long long)addr, size, buffer_program.info.buffers[b].read ? 1 : 0);
				}
				auto alloc = MetalBufferManager::Instance().GetBufferForGuestRange(
				    addr, size != 0 ? size : 4096);
				if (GdsTripWatched(trip_guest, cs_hash)) GdsTripNoteBuffer(cs_hash, b, addr, size != 0 ? size : 4096);
				// KYTY_CS_BUF_BINDINGS=<metal hash>[,<metal hash>...]: show actual Metal buffer identities
				// and ranges, to distinguish wrappers over the same guest pages when investigating alias hazards.
				// KYTY_CS_BUF_BINDINGS_FLIP=1: the same for every kernel of the flip KYTY_CS_BUF_AFTER_TRIGGER captures.
				static const bool s_bindings_flip = KYTY_ENV("KYTY_CS_BUF_BINDINGS_FLIP") != nullptr;
				if (s_bindings_flip && Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed) == s_cs_after_trigger_flip) {
					std::fprintf(stderr, "[CsBufAll] cs=0x%016llx b%zu addr=0x%llx size=%zu\n", (unsigned long long)cs_hash, b, (unsigned long long)addr, size);
				}
				if (const char* list = KYTY_ENV("KYTY_CS_BUF_BINDINGS")) {
					const char* p = list;
					while (*p != '\0') {
						char* end = nullptr;
						const uint64_t watched = std::strtoull(p, &end, 0);
						if (end == p) break;
						if (watched == cs_hash) {
							std::fprintf(stderr, "[CsBufBinding] cs=0x%016llx flip=%llu b%zu addr=0x%llx size=%zu buffer=%p offset=%llu gpu=0x%llx\n",
							             (unsigned long long)cs_hash, (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(), b,
							             (unsigned long long)addr, size, (__bridge void*)alloc.buffer,
							             (unsigned long long)alloc.offset, (unsigned long long)alloc.gpu_addr);
							break;
						}
						p = *end == ',' ? end + 1 : end;
					}
				}
				if (alloc.gpu_addr != 0 && compute_info.stage.program && b < compute_info.stage.program->info.buffers.size() &&
				    !compute_info.stage.program->info.buffers[b].written) {
					// Narrow experiment: FC27's range-copy kernel reads b1 from a recycled 512-KiB CPU
					// upload ring. Its destination and all other GPU-produced inputs must stay live.
					static bool s_skin_snapshot = [] {
						const char* value = KYTY_ENV("KYTY_SNAPSHOT_SKIN_UPLOAD");
						return value != nullptr && value[0] == '1';
					}();
					// Optional live A/B switch, scoped to this process by its configured path.
					static const char* s_skin_switch = KYTY_ENV("KYTY_SNAPSHOT_SKIN_UPLOAD_FILE");
					static uint64_t switch_flip = UINT64_MAX;
					const uint64_t current_flip = Libs::Graphics::TraceWindow::g_flip.load();
					const bool skin_copy_kernel = compute_info.stage.program->guest_hash == 0x5d3aaa8f1319d915ull;
					if (skin_copy_kernel && b == 1 && s_skin_switch != nullptr && switch_flip != current_flip) {
						switch_flip = current_flip;
						if (FILE* f = std::fopen(s_skin_switch, "r")) {
							const int ch = std::fgetc(f);
							std::fclose(f);
							if ((ch == '0' || ch == '1') && s_skin_snapshot != (ch == '1')) {
								s_skin_snapshot = ch == '1';
								std::fprintf(stderr, "[SkinUploadMode] flip=%llu enabled=%d\n", (unsigned long long)current_flip, s_skin_snapshot ? 1 : 0);
							}
						}
					}
					// KYTY_SNAPSHOT_STAGING=1: every read-only compute buffer in the CPU arena (up to 512 KiB) is copied at encode time, not
					// only the range-copy kernel's b1 named by its old Metal key (keys change with the MSL text, so that switch no longer
					// matched). The bone matrix pool is filled from a 512-KiB staging ring the CPU refills while the copy is still queued.
					static const bool s_staging_snapshot = KYTY_ENV("KYTY_SNAPSHOT_STAGING") != nullptr;
					const bool skin_upload = (s_skin_snapshot && skin_copy_kernel && b == 1) || s_staging_snapshot;
					const uint64_t live_gpu = alloc.gpu_addr;
					alloc = SnapshotBytes(alloc, addr, size != 0 ? size : 4096, skin_upload);
					if (skin_upload) {
						static uint64_t last_log_flip = UINT64_MAX;
						const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load();
						if (last_log_flip == UINT64_MAX || flip / 100 != last_log_flip / 100) {
							std::fprintf(stderr, "[SkinUpload] flip=%llu addr=0x%llx bytes=%zu copied=%d\n",
							             (unsigned long long)flip, (unsigned long long)addr, size,
							             alloc.gpu_addr != live_gpu ? 1 : 0);
							last_log_flip = flip;
						}
					}
				}
				if (alloc.gpu_addr != 0) {
					cs_buffers[b] = alloc;
					arg_ptrs[b] = alloc.gpu_addr;
					cs_residency.push_back(alloc.buffer);
					if (CsOverlapEnabled()) {
						const bool wr = compute_info.stage.program != nullptr && b < compute_info.stage.program->info.buffers.size() &&
						                compute_info.stage.program->info.buffers[b].written;
						MetalDrawExecutor::Instance().ComputeAccessAdd(addr, addr + (size != 0 ? size : 4096), wr);
					}
					// The shader's bounds check uses sizes[b]; it must not exceed the bytes the Metal
					// allocation actually covers from this offset, or an in-shader access faults on the GPU.
					const size_t avail = static_cast<size_t>([alloc.buffer length]) - alloc.offset;
					sizes[b] = static_cast<uint32_t>(std::min<size_t>(sizes[b], avail));
				}
				if (KYTY_ENV("KYTY_MTL_FAULT_INFO") != nullptr && alloc.gpu_addr == 0) {
					std::fprintf(stderr, "[CsZeroPtr] hash=0x%016llx b=%zu addr=0x%llx size=%zu buffer=%p\n",
					             (unsigned long long)cs_hash, b, (unsigned long long)addr, size, (__bridge void*)alloc.buffer);
				}
				if (KYTY_ENV("KYTY_DUMP_COMPOSITE_CB") != nullptr && b == 0 && alloc.buffer != nil && cs_hash == 0xb3300709113f5521ull) {
					static std::atomic<uint32_t> s_ccb {0};
					const uint32_t ci = s_ccb.fetch_add(1);
					if (ci % 40 == 5 && ci < 400) {
						const uint32_t* cw = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>([alloc.buffer contents]) + alloc.offset);
						std::fprintf(stderr, "[CompCB] #%u flip=%llu addr=0x%llx:", ci, (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(), (unsigned long long)addr);
						for (int w = 0; w < 60; ++w) std::fprintf(stderr, " %x", cw[w]);
						std::fprintf(stderr, "\n");
					}
				}
				if (KYTY_ENV("KYTY_MTL_FAULT_INFO") != nullptr && alloc.buffer != nil) {
					std::fprintf(stderr, "[CsBuf] hash=0x%016llx b=%zu addr=0x%llx size=%zu buf_len=%zu offset=%zu gpu=0x%llx buf=%p resident_gpu=0x%llx prot=%d\n",
					             (unsigned long long)cs_hash, b, (unsigned long long)addr, size != 0 ? size : (size_t)4096,
					             (size_t)[alloc.buffer length], (size_t)alloc.offset, (unsigned long long)alloc.gpu_addr,
					             (__bridge void*)alloc.buffer, (unsigned long long)[alloc.buffer gpuAddress], HostProtectionAt(addr));
				}
				if (addr >= 0x8170000000ull && addr < 0x8180000000ull) {
					static std::atomic<uint32_t> s_flip_cs {0};
					if (s_flip_cs.fetch_add(1) < 20) {
						std::fprintf(stderr,
						             "[FlipWatch] CS buffer=%zu addr=0x%llx size=%zu gpu=0x%llx\n",
						             b, static_cast<unsigned long long>(addr), size,
						             static_cast<unsigned long long>(alloc.gpu_addr));
					}
				}
			}
			// Re-upload sizes: the loop above clamps them to the real allocation extents.
			MetalDrawExecutor::Instance().SetComputeBytes(sizes, sizeof(sizes), 25);
			if (!snap.flattened_srt.empty()) {
				const size_t bytes  = snap.flattened_srt.size() * sizeof(uint32_t);
				const size_t padded = bytes + 1024;
				auto alloc = MetalBufferManager::Instance().AllocateDynamic(padded, 16);
				if (alloc.IsValid()) {
					std::memset(static_cast<uint8_t*>(alloc.cpu_addr) + bytes, 0, 1024);
					std::memcpy(alloc.cpu_addr, snap.flattened_srt.data(), bytes);
					arg_ptrs[53] = alloc.gpu_addr;
					// Native MSL declares the SRT pointer at kMslSrtBinding (160), the SPIR-V ABI at 53.
					arg_ptrs[160] = alloc.gpu_addr;
					if (replay_dump_here) replay_srt = alloc;
					MetalDrawExecutor::Instance().UseResource(alloc.buffer, MTLResourceUsageRead);
				}
			}
		}
		if (compute_info.stage.program != nullptr) {
			for (const auto& gb: compute_info.stage.program->bindings.descriptors) {
				if (gb.kind == ShaderRecompiler::IR::DescriptorBindingKind::Gds) {
					id<MTLBuffer> gds = GdsBuffer();
					arg_ptrs[161] = [gds gpuAddress];
					cs_residency.push_back(gds);
					// GDS is one memory for every kernel that uses it: treated as written, so these kernels keep their order.
					if (CsOverlapEnabled()) MetalDrawExecutor::Instance().ComputeAccessAdd(0xFFFFFFFFFFF00000ull, 0xFFFFFFFFFFF01000ull, true);
				}
				if (gb.kind == ShaderRecompiler::IR::DescriptorBindingKind::BdaPagetable) {
					arg_ptrs[162] = BindGuestAddressSpace();
					// Raw guest pointers: the memory the kernel reaches is not known from its bindings.
					if (CsOverlapEnabled()) MetalDrawExecutor::Instance().ComputeAccessUnknown();
				}
			}
		}
		// Compute images (sampled and storage): the dispatch previously bound no textures at all.
		if (compute_info.stage.resources && compute_info.stage.program) {
			const auto& snap = *compute_info.stage.resources;
			const auto& cprog = *compute_info.stage.program;
			static std::atomic<uint32_t> s_cs_img_log {0};
			for (const auto& binding: cprog.bindings.descriptors) {
				const auto kind = static_cast<uint32_t>(binding.kind);
				if (kind < ShaderRecompiler::IR::FirstImageBinding ||
				    kind >= static_cast<uint32_t>(ShaderRecompiler::IR::DescriptorBindingKind::Samplers)) {
					continue;
				}
				const bool storage = kind >= ShaderRecompiler::IR::FirstStorageImageBinding;
				for (size_t r = 0; r < binding.resources.size(); ++r) {
					const uint32_t res = binding.resources[r];
					if (res >= snap.images.size()) continue;
					const auto& desc = snap.images[res];
					ShaderTextureResource tex {};
					for (size_t f = 0; f < 8; ++f) {
						tex.fields[f] = desc.dwords[f];
					}
					const uint64_t addr = tex.Base40();
					const uint32_t w = static_cast<uint32_t>(tex.Width5()) + 1u;
					const uint32_t h = static_cast<uint32_t>(tex.Height5()) + 1u;
					if (addr == 0) continue;
					// Aliasing: a sampled image at an address that a render target or an earlier compute
					// write already produced must use that object (same dims), not a fresh copy of guest
					// memory, which would read back as zeros.
					id<MTLTexture> texture = nil;
					const char* via = "";
					if (auto canonical=canonical_images.find(res);canonical!=canonical_images.end()) {texture=canonical->second.image;via="canonical";}
					if (texture == nil && !storage && res < cprog.info.images.size()) {
						texture = DepthWriterForLayeredRead(cprog.info.images[res], tex, addr, w, h);
						if (texture != nil) via = "depth-layer";
					}
					if (texture == nil && res < cprog.info.images.size()) {
						if (id<MTLTexture> lay = LayeredImageFor(cprog.info.images[res], tex, addr, w, h, storage)) {
							texture = lay;
							via = "layered";
						}
					}
					if (texture == nil && storage && res < cprog.info.images.size()) {
						const auto& bc_info = cprog.info.images[res];
						if (id<MTLTexture> level_image = BcLevelImageFor(tex, addr, w, h, bc_info.r128, bc_info.mip_mode == ShaderRecompiler::IR::ImageMipMode::Dynamic,
						                                                 bc_info.written && !bc_info.read && !bc_info.atomic)) {
							texture = level_image;
							via = "bclevel";
						}
					}
					// A storage image over levels of a mipped render target (kernel 6b86b9e8 builds the mips of a composited kit, a bloom pyramid...)
					// is that target's own chain. None of the lookups below takes a mipped object, so these levels went to an aliased texture of
					// the same address while every reader got the target (NewestRenderTargetFor) with its levels empty: the next downsample read
					// zeros, and so did the kernels that compress kits into BC, so kits were black from mip 1 on. KYTY_RT_MIP_ALIAS=1 restores it.
					// RGBA8 chains only: the HDR scene pyramids (RGBA16F) go the same way, but binding them here brought neon flares, smeared and
					// black frames (um246, um252); why is not established. KYTY_RT_MIP_HDR=1 includes them for that work.
					static const bool s_rt_mip_alias = KYTY_ENV("KYTY_RT_MIP_ALIAS") != nullptr;
					static const bool s_rt_mip_hdr = KYTY_ENV("KYTY_RT_MIP_HDR") != nullptr;
					const MTLPixelFormat rt_mip_fmt = MetalFormatFromBufferFormat(tex.Format());
					if (texture == nil && storage && !s_rt_mip_alias && (tex.BaseLevel() > 0 || tex.LastLevel() > 0) &&
					    (s_rt_mip_hdr || rt_mip_fmt == MTLPixelFormatRGBA8Unorm)) {
						if (id<MTLTexture> rt = NewestRenderTargetFor(addr, w, h, rt_mip_fmt)) {
							if (rt.pixelFormat == rt_mip_fmt && rt.mipmapLevelCount > tex.LastLevel() && (rt.usage & MTLTextureUsageShaderWrite) != 0) {
								texture = rt;
								via = "rtmip";
							}
						}
					}
					// Neither is the address's surface: the render target already is, and the base level stands for a BC chain.
					const bool other_level_image = std::strcmp(via, "rtmip") == 0 || (std::strcmp(via, "bclevel") == 0 && tex.BaseLevel() != 0);
					if (texture == nil) {
						// GPU-produced images are views over the guest memory (see GpuSurfaceFor): writes and later reads meet in the same bytes.
						// Opt-in until validated (KYTY_UMA_SURFACES=1): with it on, producer/consumer pairs of GPU-produced images meet in the same
						// bytes, but kernels that were fed stale data before now see real data and some of them misbehave (see the handoff).
						static const bool s_uma_off = !UmaSurfacesEnabled();
						// KYTY_UMA_ONLY=<hash prefix,...>: the guest-memory path is used only by those kernels (bisect).
						static const std::vector<std::string> s_uma_only = [] {
							std::vector<std::string> v;
							const char* value = KYTY_ENV("KYTY_UMA_ONLY");
							while (value != nullptr && *value != '\0') {
								const char* end = std::strchr(value, ',');
								v.emplace_back(value, end ? static_cast<size_t>(end - value) : std::strlen(value));
								value = end ? end + 1 : nullptr;
							}
							return v;
						}();
						// KYTY_UMA_SKIP=<hash prefix,...>: those kernels keep the old texture lookups (bisect).
						static const std::vector<std::string> s_uma_skip = [] {
							std::vector<std::string> v;
							const char* value = KYTY_ENV("KYTY_UMA_SKIP");
							while (value != nullptr && *value != '\0') {
								const char* end = std::strchr(value, ',');
								v.emplace_back(value, end ? static_cast<size_t>(end - value) : std::strlen(value));
								value = end ? end + 1 : nullptr;
							}
							return v;
						}();
						bool uma_allowed = s_uma_only.empty();
						if (!s_uma_skip.empty()) {
							char skip_text[32];
							std::snprintf(skip_text, sizeof(skip_text), "0x%016llx", static_cast<unsigned long long>(cprog.shader_hash));
							for (const auto& prefix: s_uma_skip) if (std::strncmp(skip_text, prefix.c_str(), prefix.size()) == 0) uma_allowed = false;
						}
						if (!uma_allowed && s_uma_only.empty() && s_uma_skip.empty()) uma_allowed = true;
						if (!uma_allowed && !s_uma_only.empty()) {
							char hash_text[32];
							std::snprintf(hash_text, sizeof(hash_text), "0x%016llx", static_cast<unsigned long long>(cprog.shader_hash));
							for (const auto& prefix: s_uma_only) if (std::strncmp(hash_text, prefix.c_str(), prefix.size()) == 0) uma_allowed = true;
						}
						const auto uma_fmt = MetalFormatFromBufferFormat(tex.Format());
						const bool plain_2d = res < cprog.info.images.size() &&
						                      cprog.info.images[res].dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
						                      tex.Type() != Prospero::ImageType::kCube && tex.Type() != Prospero::ImageType::kColor3D &&
						                      tex.BaseLevel() == 0 && tex.LastLevel() == 0;
						if (!s_uma_off && uma_allowed && plain_2d && uma_fmt != MTLPixelFormatInvalid && !IsCompressedFormat(uma_fmt) &&
						    (!storage || (GuestTextureUsage(uma_fmt) & MTLTextureUsageShaderWrite) != 0)) {
							bool special_writer = false;
							if (!storage) {
								// A depth surface or a mipped image the GPU wrote has no guest-memory form: the existing lookups handle it.
								if (id<MTLTexture> lw = LastGpuWriterTexture(addr)) {
									special_writer = lw.mipmapLevelCount > 1 || lw.pixelFormat == MTLPixelFormatDepth32Float ||
									                 lw.pixelFormat == MTLPixelFormatDepth16Unorm || lw.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
								}
							}
							if (!special_writer) {
								if (id<MTLTexture> uma = GpuSurfaceFor(addr, w, h, uma_fmt, tex.TileMode(), storage)) {
									texture = uma;
									via = "uma";
								}
							}
						}
					}
					if (texture == nil && !storage) {
						if (id<MTLTexture> last = LastGpuWriterTexture(addr)) {
							const MTLPixelFormat want_fmt = MetalFormatFromBufferFormat(tex.Format());
							if (last.width == w && last.height == h && last.mipmapLevelCount == 1 && last.textureType == MTLTextureType2D &&
							    SameTexelSize(last, want_fmt)) {
								texture = ReinterpretFormat(last, want_fmt);
								via = "lastwriter";
							}
						}
					}
					if (texture == nil && !storage) {
						const MTLPixelFormat want_fmt = MetalFormatFromBufferFormat(tex.Format());
						if (id<MTLTexture> rt = NewestRenderTargetFor(addr, w, h, want_fmt)) {
							texture = ReinterpretFormat(rt, want_fmt);
							via = "rt";
						}
					}
					// A storage image over the surface the GPU last wrote at this address with the same size and format is that surface: a
					// kernel that patches some tiles of a render target (the velocity buffer: draws write it, kernel b2874d62 rewrites the
					// tiles of its list) must write into it, as the Vulkan reference's one image per address and layout does. A separate
					// texture held only the patched tiles plus stale inf/65504, and the readers after it lost the draws' velocities.
					// KYTY_STORAGE_NO_LASTWRITER=1 restores the separate texture.
					// Another format of the same texel size is NOT taken as the same image by default: FC 27 reuses one transient address for
					// the tile kernel's R32F/R32Uint output and, later in the frame, for the tiled lighting's R11G11B10 output, which writes only a
					// few tiles. A view of the R32F writer showed its float bits as RG11B10 in all the other tiles (about 6% of the texels have
					// exponent 31 = NaN/inf: 88069 of 1.5 M in a traced frame) and the TAA spread them over its history (neon stripes, white
					// silhouettes). The Vulkan reference creates an empty image for the new format; here the lookups below do the same.
					// KYTY_STORAGE_LASTWRITER_ANY_FMT=1 restores the view of a writer of another format (the rule of 2026-11-07: it was added
					// against "white frames with black tile blocks" from a separate texture seeded with old data; not seen with the rule below).
					static const bool s_storage_no_lastwriter = false;
					static const bool s_storage_any_fmt = false;
					if (texture == nil && storage && !s_storage_no_lastwriter) {
						if (id<MTLTexture> last = LastGpuWriterTexture(addr)) {
							const MTLPixelFormat want_fmt = MetalFormatFromBufferFormat(tex.Format());
							if (last.width == w && last.height == h && last.mipmapLevelCount == 1 && last.textureType == MTLTextureType2D &&
							    !IsDepthStencilPixelFormat(last.pixelFormat) &&
							    (last.pixelFormat == want_fmt || (s_storage_any_fmt && SameTexelSize(last, want_fmt)))) {
								id<MTLTexture> view = ReinterpretFormat(last, want_fmt);
								if (view != nil && view.pixelFormat == want_fmt && (view.usage & MTLTextureUsageShaderWrite) != 0) {
									texture = view;
									via = "lastwriter";
								}
							}
						}
					}
					if (texture == nil) {
						if (void* wr = GetComputeWrittenImageForAddress(addr)) {
							id<MTLTexture> cand = (__bridge id<MTLTexture>)wr;
							// A depth surface that lived at this address is another resource: a storage image declared with a colour format cannot be
							// written through it (the tile classification kernel's R32F output was bound to the scene depth and its writes were lost;
							// the Vulkan reference binds an R32F image there).
							const bool depth_for_storage = storage && IsDepthStencilPixelFormat(cand.pixelFormat);
							// Same rule as for the last writer above: a storage image is not a view of a registered image of another format (the
							// tile kernel's R32F bits read as RG11B10 give NaN/inf in the tiles the lighting kernel does not rewrite).
							const bool other_fmt_for_storage = storage && !s_storage_any_fmt && cand.pixelFormat != MetalFormatFromBufferFormat(tex.Format());
							// A block-compressed candidate is a stale shadow of a decoded copy (see GraphicsImageFor): GuestTexture checks its freshness.
							static const bool s_bc_registry = false;
							if (!depth_for_storage && !other_fmt_for_storage && cand.width == w && cand.height == h && SameTexelSize(cand, MetalFormatFromBufferFormat(tex.Format())) &&
							    (s_bc_registry || !IsCompressedFormat(cand.pixelFormat))) {
								texture = ReinterpretFormat(cand, MetalFormatFromBufferFormat(tex.Format()));
								via = "csreg";
							}
						}
					}
					// The lookups above match the address, size and texel size only. A transient address the game reuses can hold a 2D
					// surface and a volume of the same width and height in one frame; a 2D texture bound where the kernel declares a
					// texture3d (or the reverse) is undefined on the GPU (kernel f8dd8c4c read 2D 200x113 objects as volumes before
					// the GPU hangs). Such a candidate is dropped: the lookups below build the declared kind.
					if (texture != nil && res < cprog.info.images.size()) {
						const bool want_3d = cprog.info.images[res].dimension == ShaderRecompiler::Decoder::ImageDimension::Dim3D;
						if ((texture.textureType == MTLTextureType3D) != want_3d) {
							static std::atomic<uint32_t> s_dim_log {0};
							if (s_dim_log.fetch_add(1) < 16) {
								std::fprintf(stderr, "[DimMismatch] cs=0x%016llx r%u 0x%llx %ux%u via=%s: bound %s, kernel declares %s\n",
								             static_cast<unsigned long long>(cprog.shader_hash), static_cast<unsigned>(res),
								             static_cast<unsigned long long>(addr), static_cast<unsigned>(w), static_cast<unsigned>(h), via,
								             texture.textureType == MTLTextureType3D ? "3D" : "non-3D", want_3d ? "3D" : "non-3D");
							}
							texture = nil;
						}
					}
					if (texture == nil && !storage) {
						const MTLPixelFormat recent_fmt = MetalFormatFromBufferFormat(tex.Format());
						if (recent_fmt != MTLPixelFormatInvalid) {
							if (id<MTLTexture> recent = RecentAliasedImage(addr, w, h, recent_fmt)) {
								texture = recent;
								via = "alias";
								static std::atomic<uint32_t> s_recent_log {0};
								if (s_recent_log.fetch_add(1) < 40) {
									std::fprintf(stderr, "[RecentAlias] flip=%llu cs=0x%016llx r%u 0x%llx %ux%u f%u: read the image written earlier in this frame\n",
									             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)),
									             static_cast<unsigned long long>(cprog.shader_hash), static_cast<unsigned>(res),
									             static_cast<unsigned long long>(addr), static_cast<unsigned>(w), static_cast<unsigned>(h), (unsigned)tex.Format());
								}
							}
						}
					}
					if (texture == nil) {
						bool other_size = false;
						{
							std::lock_guard<std::mutex> lock(s_render_target_mutex);
							const auto rit = s_rt_by_addr.find(addr);
							if (rit != s_rt_by_addr.end() && !rit->second.empty()) {
								other_size = true;
								for (const auto& [rf, rt]: rit->second) {
									if (rt.width == w && rt.height == h) other_size = false;
								}
							}
						}
						const bool is3d_img = res < cprog.info.images.size() &&
						                      cprog.info.images[res].dimension == ShaderRecompiler::Decoder::ImageDimension::Dim3D;
						// A storage image the GPU produces starts empty, as in the Vulkan reference (its image cache uploads guest memory only
						// when the CPU wrote it): a kernel that writes only some tiles (the tile classification's R32F output) must not leave
						// the bytes of whatever resource last lived at this transient address - +-1e38 and NaN - in the rest. Guest memory is
						// decoded only for images the kernel also reads at addresses the GPU never wrote (CPU-provided data).
						bool gpu_image = false;
						if (storage && !is3d_img && res < cprog.info.images.size()) {
							const auto& img_info = cprog.info.images[res];
							const bool  write_only = img_info.written && !img_info.read && !img_info.atomic;
							bool        gpu_owned  = false;
							{
								std::lock_guard<std::mutex> lock(s_cs_storage_mutex);
								gpu_owned = s_cs_storage.find(addr) != s_cs_storage.end();
							}
							if (!gpu_owned) {
								std::lock_guard<std::mutex> lock(s_render_target_mutex);
								gpu_owned = s_rt_by_addr.find(addr) != s_rt_by_addr.end();
							}
							static const bool s_off = KYTY_ENV("KYTY_STORAGE_FROM_GUEST") != nullptr;
							gpu_image = !s_off && (write_only || gpu_owned);
						}
						if ((other_size || gpu_image) && !is3d_img) {
							MTLPixelFormat afmt = MetalFormatFromBufferFormat(tex.Format());
							if (afmt == MTLPixelFormatInvalid) {
								FailMetalMaterialization("aliased compute image", static_cast<uint32_t>(tex.Format()));
							}
							texture = AliasedImageFor(addr, w, h, afmt, static_cast<uint32_t>(tex.LastLevel()) + 1u);
							via = "alias";
						} else {
							texture = ComputeGuestImageFor(cprog, res, tex, w, h, addr, storage);
							via = "guest";
						}
					}
					// A compute-owned image must survive: a graphics bind of the same address would otherwise refresh the
					// still-empty guest copy over it.
					if (storage && texture != nil && std::strcmp(via, "bclevel") != 0) MarkGuestTexComputeWritten(addr);
					if (storage && texture != nil && !other_level_image) {
						const auto canonical=canonical_images.find(res);
						const auto* backing=canonical!=canonical_images.end()?&canonical->second.physical:nullptr;
						RecordComputeStorage(addr, texture, tex.TileMode(), true,backing);
					}
					// A read-only DCC image cleared through its metadata reads as the clear colour (see SampledMetaClear).
					if (!storage && texture != nil && texture.textureType == MTLTextureType2D && texture.mipmapLevelCount == 1) {
						if (id<MTLTexture> cleared = SampledMetaClear(tex, addr, texture)) texture = cleared;
					}
					{
						// KYTY_LOG_CS_ADDR=<guest address>: every compute image bound at that address (which kernels read/write it).
						// KYTY_LOG_CS_STORES=1: once per (kernel, resource, address, size, format) every compute image that is written - names the kernel
						// behind each image of a KYTY_DUMP_IMG dump.
						{
							static const bool s_stores_on = KYTY_ENV("KYTY_LOG_CS_STORES") != nullptr;
							if (s_stores_on && storage) {
								static std::mutex s_stores_mutex;
								static std::unordered_set<std::string> s_stores_seen;
								char key[160];
								std::snprintf(key, sizeof(key), "%llx/%u/%llx/%ux%u/%u", static_cast<unsigned long long>(cprog.shader_hash), res,
								              static_cast<unsigned long long>(addr), w, h, static_cast<unsigned>(tex.Format()));
								std::lock_guard<std::mutex> lock(s_stores_mutex);
								if (s_stores_seen.insert(key).second) {
									std::fprintf(stderr, "[CsStore] flip=%llu cs=0x%016llx res=%u addr=0x%llx %ux%u fmt=%u\n",
									             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)),
									             static_cast<unsigned long long>(cprog.shader_hash), res, static_cast<unsigned long long>(addr), w, h,
									             static_cast<unsigned>(tex.Format()));
								}
							}
						}
						if (AddrLogWanted(addr))
						std::fprintf(stderr, "[AddrLog] flip=%llu cs=0x%016llx %s addr=0x%llx want %ux%u f%u via=%s got=%lux%lu f%lu\n", static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load()),
						             static_cast<unsigned long long>(cprog.shader_hash), storage ? "STORE" : "load ", static_cast<unsigned long long>(addr), w, h, (unsigned)tex.Format(), via,
						             texture != nil ? (unsigned long)texture.width : 0ul, texture != nil ? (unsigned long)texture.height : 0ul, texture != nil ? (unsigned long)texture.pixelFormat : 0ul);
					if (std::strcmp(via, "guest") == 0 && !storage && PassTraceCsFilter()) {
						// Why every writer registry missed: what each one holds at this address.
						char dbg[600];
						int len = 0;
						{
							std::lock_guard<std::mutex> lock(s_render_target_mutex);
							const auto rit = s_rt_by_addr.find(addr);
							if (rit != s_rt_by_addr.end()) for (const auto& [rf, rt]: rit->second)
								len += std::snprintf(dbg + len, sizeof(dbg) - len, " rt[%ux%u f%lu seq=%llu]", rt.width, rt.height, rt.tex ? (unsigned long)rt.tex.pixelFormat : 0ul, (unsigned long long)rt.sequence);
						}
						{
							std::lock_guard<std::mutex> lock(s_cs_storage_mutex);
							const auto cit = s_cs_storage.find(addr);
							if (cit != s_cs_storage.end() && cit->second.tex != nil)
								len += std::snprintf(dbg + len, sizeof(dbg) - len, " cs[%lux%lu f%lu flip=%llu]", (unsigned long)cit->second.tex.width, (unsigned long)cit->second.tex.height, (unsigned long)cit->second.tex.pixelFormat, (unsigned long long)cit->second.flip);
						}
						std::fprintf(stderr, "[PassTrace]   GUESTMISS addr=0x%llx want %ux%u f%u bpp-tile=%u registries:%s\n", (unsigned long long)addr, w, h, (unsigned)tex.Format(), (unsigned)tex.TileMode(), len > 0 ? dbg : " none");
					}
					PassTraceNote(addr, texture, storage, res, via);
						// A list is accepted (comma separated); KYTY_LOG_CS_ADDR_ANY=1 logs at any flip instead of only 3 of every 50 from flip 9000.
						static const std::vector<uint64_t> s_addr_probes = [] {
							std::vector<uint64_t> list;
							const char* value = KYTY_ENV("KYTY_LOG_CS_ADDR");
							while (value != nullptr && *value != '\0') {
								char* end = nullptr;
								const uint64_t a = std::strtoull(value, &end, 0);
								if (end == value) break;
								if (a != 0) list.push_back(a);
								value = *end == ',' ? end + 1 : nullptr;
							}
							return list;
						}();
						static const bool s_addr_any = KYTY_ENV("KYTY_LOG_CS_ADDR_ANY") != nullptr;
						static std::atomic<uint32_t> s_addr_n {0};
						const uint64_t pf = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
						if (!s_addr_probes.empty() && std::find(s_addr_probes.begin(), s_addr_probes.end(), addr) != s_addr_probes.end() &&
						    (s_addr_any || (pf >= 9000 && pf % 50 < 3)) && s_addr_n.fetch_add(1) < 1500) {
							std::fprintf(stderr, "[CsAddr] flip=%llu cs=0x%016llx res=%u %s addr=0x%llx %ux%u fmt=%u -> tex=%p %lux%lu pf=%lu buf=%d\n",
							             static_cast<unsigned long long>(pf), static_cast<unsigned long long>(cprog.shader_hash), res,
							             storage ? "STORE" : "load ", static_cast<unsigned long long>(addr), w, h, static_cast<unsigned>(tex.Format()),
							             texture != nil ? (__bridge void*)texture : nullptr, texture != nil ? (unsigned long)texture.width : 0ul,
							             texture != nil ? (unsigned long)texture.height : 0ul, texture != nil ? (unsigned long)texture.pixelFormat : 0ul,
							             texture != nil && texture.buffer != nil ? 1 : 0);
						}
					}
					{
						// KYTY_LOG_CS_IMAGES=<hash>[,<hash>...]: trace dependent passes in the same run.
						static const std::vector<uint64_t> s_img_hashes = [] {
							std::vector<uint64_t> hashes;
							const char* value = KYTY_ENV("KYTY_LOG_CS_IMAGES");
							while (value != nullptr && *value != '\0') {
								char* end = nullptr;
								const uint64_t hash = std::strtoull(value, &end, 0);
								if (end == value) break;
								if (hash != 0) hashes.push_back(hash);
								value = *end == ',' ? end + 1 : nullptr;
							}
							return hashes;
						}();
						static std::atomic<uint32_t> s_img_log {0};
						static std::atomic<uint64_t> s_img_last_flip {0};
						const uint64_t img_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
						bool img_late = false;
						if (img_flip >= 3000 && img_flip % 100 < 2 && s_img_last_flip.load() / 100 != img_flip / 100) {
							img_late = true;
						} else if (img_flip >= 3000 && img_flip % 100 < 2) {
							img_late = s_img_last_flip.load() == img_flip;
						}
						if (img_late) s_img_last_flip.store(img_flip);
						if (std::find(s_img_hashes.begin(), s_img_hashes.end(), cprog.shader_hash) != s_img_hashes.end() &&
						    (img_late || s_img_log.fetch_add(1) < 120)) {
							void* rt_same = MetalRenderer::Instance().GetRenderTextureForAddress(addr);
							{
								uint64_t lw_flip = 0;
								id<MTLTexture> lw = LastGpuWriterTexture(addr, &lw_flip);
								std::fprintf(stderr, "[CsImageLW] addr=0x%llx last_writer %lux%lu fmt=%lu flip=%llu depth_fmt=%d\n", (unsigned long long)addr,
								             lw != nil ? (unsigned long)lw.width : 0ul, lw != nil ? (unsigned long)lw.height : 0ul,
								             lw != nil ? (unsigned long)lw.pixelFormat : 0ul, (unsigned long long)lw_flip, 0);
							}
							void* cs_same = GetComputeWrittenImageForAddress(addr);
							std::fprintf(stderr, "[CsImageT#] res=%u T#=%08x %08x %08x %08x %08x %08x %08x %08x levels=%u..%u\n", res, tex.fields[0], tex.fields[1],
							             tex.fields[2], tex.fields[3], tex.fields[4], tex.fields[5], tex.fields[6], tex.fields[7],
							             static_cast<unsigned>(tex.BaseLevel()), static_cast<unsigned>(tex.LastLevel()));
							std::fprintf(stderr, "[CsImage] flip=%llu hash=0x%llx res=%u storage=%d addr=0x%llx %ux%u fmt=%u tile=%u -> tex=%p(%lu buf=%d) rt_at_addr=%p cs_written=%p last_writer=%p\n",
							             static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed)),
							             static_cast<unsigned long long>(cprog.shader_hash), res, storage ? 1 : 0,
							             static_cast<unsigned long long>(addr), w, h, static_cast<unsigned>(tex.Format()),
							             static_cast<unsigned>(tex.TileMode()), texture != nil ? (__bridge void*)texture : nullptr,
							             texture != nil ? static_cast<unsigned long>(texture.pixelFormat) : 0ul,
							             texture != nil && texture.buffer != nil ? 1 : 0, rt_same, cs_same, (__bridge void*)LastGpuWriterTexture(addr));
						}
					}
					if (!texture) continue;
					const auto declared_format = MetalFormatFromBufferFormat(tex.Format());
					const bool depth_value_view = (texture.pixelFormat == MTLPixelFormatDepth16Unorm && declared_format == MTLPixelFormatR16Unorm) ||
					    (texture.pixelFormat == MTLPixelFormatDepth32Float && declared_format == MTLPixelFormatR32Float);
					if (declared_format != MTLPixelFormatInvalid && texture.pixelFormat != declared_format && !depth_value_view) {
						// The surface was found under another format of the same texel size (a kernel packs RG11B10 by hand into an R32Uint
						// image, another reads the same bytes as RG11B10): bind a view of the declared format over the same bytes. Bound
						// as it was, an RG11B10 image read through an R32Uint texture (and back) returned garbage.
						id<MTLTexture> view = nil;
						if (!IsDepthStencilPixelFormat(texture.pixelFormat) && !IsCompressedFormat(texture.pixelFormat) &&
						    !IsCompressedFormat(declared_format) && BytesPerPixel(texture.pixelFormat) == BytesPerPixel(declared_format)) {
							view = ReinterpretFormat(texture, declared_format);
						}
						if (view != nil && view.pixelFormat == declared_format) {
							if (storage) cs_residency.push_back(texture);
							PassTraceRebind(texture, view, storage);
							texture = view;
						} else {
							std::fprintf(stderr,"[ImageFormatReject] shader=%llx res=%u addr=%llx %ux%u tile=%u via=%s storage=%d\n",(unsigned long long)cprog.guest_hash,res,(unsigned long long)addr,w,h,unsigned(tex.TileMode()),via,storage);
							FailMetalMaterialization("compute image format differs from descriptor", (uint32_t)tex.Format(),
							             (uint32_t)texture.pixelFormat, (uint32_t)declared_format);
						}
					}
					// T# base/last level: bind a view of that level range (the surface keeps one base address for all levels).
					bool mip_view_bound = false;
					if ((texture.textureType == MTLTextureType2D || texture.textureType == MTLTextureType3D) && (tex.BaseLevel() > 0 || tex.LastLevel() > 0)) {
						const NSUInteger avail_levels = texture.mipmapLevelCount;
						if (avail_levels > 1 && tex.BaseLevel() < avail_levels) {
							const NSUInteger base_l = tex.BaseLevel();
							const NSUInteger cnt_l = std::min<NSUInteger>(
							    static_cast<NSUInteger>(tex.LastLevel() >= tex.BaseLevel() ? tex.LastLevel() - tex.BaseLevel() + 1 : 1),
							    avail_levels - base_l);
							cs_residency.push_back(texture);
							id<MTLTexture> full_tex = texture;
							texture = MipRangeView(texture, base_l, cnt_l);
							PassTraceRebind(full_tex, texture, storage);
							mip_view_bound = true;
						}
					}
					if (AddrLogWanted(addr) || (PassTraceCsFilter() && (tex.BaseLevel() > 0 || tex.LastLevel() > 0)))
						std::fprintf(stderr, "[CsMip] flip=%llu cs=0x%016llx res=%u %s addr=0x%llx T# levels=%u..%u texture=%lux%lu levels=%lu bound_view=%d\n", static_cast<unsigned long long>(Libs::Graphics::TraceWindow::g_flip.load()),
						             static_cast<unsigned long long>(cprog.shader_hash), (unsigned)res, storage ? "STORE" : "load ", static_cast<unsigned long long>(addr),
						             (unsigned)tex.BaseLevel(), (unsigned)tex.LastLevel(), (unsigned long)texture.width, (unsigned long)texture.height, (unsigned long)texture.mipmapLevelCount, mip_view_bound ? 1 : 0);
					if (std::strcmp(via, "bclevel") == 0)
						NoteBcWriteback(addr, texture, tex, w, h, cprog.info.images[res].r128,
						                cprog.info.images[res].mip_mode == ShaderRecompiler::IR::ImageMipMode::Dynamic);
					if (!mip_view_bound && !other_level_image && ShaderRecompiler::IR::ImageBindingResourceClass(binding.kind) == ShaderRecompiler::IR::ImageResourceClass::Storage) {
						RegisterComputeWrittenImage(addr, (__bridge void*)texture);
					}
					{
						// KYTY_DUMP_CS_INPUTS=<hash>: at the 40th dispatch of that kernel, dump each bound image
						// (sampled and storage) to /tmp/cs_in_<addr>.ppm for non-zero-ratio checks.
						static const uint64_t s_dump_cs = KYTY_ENV("KYTY_DUMP_CS_INPUTS") ? std::strtoull(KYTY_ENV("KYTY_DUMP_CS_INPUTS"), nullptr, 0) : 0ull;
						static std::atomic<uint32_t> s_dump_cs_n {0};
						if (s_dump_cs != 0 && cprog.shader_hash == s_dump_cs && res == binding.resources.front()) {
							static const uint32_t s_dump_at = KYTY_ENV("KYTY_DUMP_CS_AT") ? (uint32_t)std::strtoul(KYTY_ENV("KYTY_DUMP_CS_AT"), nullptr, 10) : 40u;
							const uint32_t n = s_dump_cs_n.fetch_add(1);
							if (n == s_dump_at) {
								MetalContext::Instance().CommitCommandBuffer(true);
								char path[160];
								std::snprintf(path, sizeof(path), "/tmp/cs_in_%llx_%s.ppm", (unsigned long long)addr,
								              ShaderRecompiler::IR::ImageBindingResourceClass(binding.kind) == ShaderRecompiler::IR::ImageResourceClass::Storage ? "st" : "sm");
								MetalPresenter::Instance().DumpTextureToPpm((__bridge void*)texture, path);
								std::fprintf(stderr, "[CsInputs] dumped %s\n", path);
							}
						}
					}
					// Native MSL programs use the direct layout (images at 64 + resource, as the emitter
					// declares them); SPIR-V programs keep the legacy NativeBinding slot.
					const uint32_t slot = cprog.native_msl
					    ? 64u + res
					    : ShaderRecompiler::IR::NativeBinding(cprog.stage, binding.kind) + static_cast<uint32_t>(r);
					const bool declared_array = res < cprog.info.images.size() &&
					    (cprog.info.images[res].dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray);
					texture = ConformToDeclaredArray(texture, declared_array);
					id<MTLTexture> bound_tex = (!storage && !mip_view_bound) ? ApplyDstSel(texture, tex) : texture;
					if (slot < 256) {
						arg_ptrs[slot] = [bound_tex gpuResourceID]._impl;
						if (replay_dump_here && res < replay_images.size()) replay_images[res] = bound_tex;
						if (cprog.native_msl && res < 64u)
							sizes[ShaderRecompiler::Msl::ImageMetadataWordBase + res] = SupportsNativeDepthCompare(bound_tex.pixelFormat) ? 1u : 0u;
					}
					if (CsOverlapEnabled()) MetalDrawExecutor::Instance().ComputeAccessAdd(addr, addr + CsAccessBytes(texture), storage);
					cs_residency.push_back(texture);
					if (bound_tex != texture) cs_residency.push_back(bound_tex);
					if (KYTY_ENV("KYTY_LOG_COMPOSITE_IN") != nullptr && cprog.shader_hash == 0xb3300709113f5521ull &&
					    texture.storageMode == MTLStorageModeShared && texture.textureType == MTLTextureType2D) {
						static std::atomic<uint32_t> s_comp_in {0};
						const uint32_t ci = s_comp_in.fetch_add(1);
						if (ci % 30 < 6 && ci < 6000) {
							MetalContext::Instance().CommitCommandBuffer(true);
							const size_t bpp_c = texture.pixelFormat == MTLPixelFormatRGBA16Float ? 8u : 4u;
							const NSUInteger cw = texture.width, ch = texture.height;
							std::vector<uint8_t> cbytes(static_cast<size_t>(cw) * ch * bpp_c);
							[texture getBytes:cbytes.data() bytesPerRow:cw * bpp_c fromRegion:MTLRegionMake2D(0, 0, cw, ch) mipmapLevel:0];
							size_t cnz = 0;
							for (size_t i = 0; i < cbytes.size(); i += 61) cnz += cbytes[i] != 0;
							double quad[4] = {0, 0, 0, 0};
							{
								size_t qn[4] = {0, 0, 0, 0}, qz[4] = {0, 0, 0, 0};
								for (NSUInteger y = 0; y < ch; y += 8) {
									for (NSUInteger x = 0; x < cw; x += 8) {
										const int q = (y >= ch / 2 ? 2 : 0) + (x >= cw / 2 ? 1 : 0);
										const uint8_t* px = &cbytes[(static_cast<size_t>(y) * cw + x) * bpp_c];
										++qn[q];
										bool nzp = false;
										for (size_t b = 0; b < bpp_c; ++b) nzp = nzp || px[b] != 0;
										qz[q] += nzp ? 1 : 0;
									}
								}
								for (int q = 0; q < 4; ++q) quad[q] = qn[q] ? 100.0 * qz[q] / qn[q] : 0.0;
							}
							std::fprintf(stderr, "[CompQuad] flip=%llu slot=%u addr=0x%llx TL=%.0f TR=%.0f BL=%.0f BR=%.0f\n",
							             (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(), slot, (unsigned long long)addr,
							             quad[0], quad[1], quad[2], quad[3]);
							void* rt_for_addr = MetalRenderer::Instance().GetRenderTextureForAddress(addr);
							std::fprintf(stderr, "[CompIn] #%u flip=%llu slot=%u addr=0x%llx fmt=%lu bound=%p rt_at_addr=%p same=%d nz=%.1f%%\n",
							             ci, (unsigned long long)Libs::Graphics::TraceWindow::g_flip.load(), slot, (unsigned long long)addr,
							             (unsigned long)texture.pixelFormat, (__bridge void*)texture, rt_for_addr,
							             rt_for_addr == (__bridge void*)texture ? 1 : 0, 100.0 * 61.0 * cnz / static_cast<double>(cbytes.size()));
						}
					}
					// KYTY_DUMP_CS_IMGS=<hash>,<flip lo>,<flip hi>: mean colour of every 2D image the kernel binds (and a PPM of
					// the 4-byte ones) in that flip range, read after the earlier GPU work completed.
					if (const char* dspec = KYTY_ENV("KYTY_DUMP_CS_IMGS")) {
						char* dend = nullptr;
						const uint64_t dh = std::strtoull(dspec, &dend, 16);
						unsigned dlo = 0, dhi = 0;
						if (dend && *dend == ',') std::sscanf(dend + 1, "%u,%u", &dlo, &dhi);
						const uint64_t dfl = Libs::Graphics::TraceWindow::g_flip.load();
						static std::atomic<uint32_t> s_dci {0};
						// Every 10th flip only (each sample synchronises the GPU), so long ranges stay usable.
						if (cprog.shader_hash == dh && dfl >= dlo && dfl <= dhi && (dfl % 10) == 0 && texture != nil &&
						    texture.textureType == MTLTextureType2D && texture.storageMode == MTLStorageModeShared && s_dci.fetch_add(1) < 3000) {
							MetalContext::Instance().CommitCommandBuffer(true);
							const size_t bppd = BytesPerPixel(texture.pixelFormat);
							if (bppd != 0 && texture.width * texture.height <= 4096u * 4096u) {
								std::vector<uint8_t> pix(texture.width * texture.height * bppd);
								[texture getBytes:pix.data() bytesPerRow:texture.width * bppd
								       fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height) mipmapLevel:0];
								double sum[4] = {0, 0, 0, 0};
								const size_t npx = texture.width * texture.height;
								for (size_t i = 0; i < npx; i += 7) for (size_t c = 0; c < std::min<size_t>(bppd, 4); ++c) sum[c] += pix[i * bppd + c];
								const double cnt = static_cast<double>((npx + 6) / 7);
								std::fprintf(stderr, "[CsImgDump] flip=%llu res=%u addr=0x%llx %lux%lu mtl=%lu storage=%d mean=%.1f,%.1f,%.1f,%.1f\n",
								             (unsigned long long)dfl, res, (unsigned long long)addr, (unsigned long)texture.width,
								             (unsigned long)texture.height, (unsigned long)texture.pixelFormat, storage ? 1 : 0,
								             sum[0] / cnt, sum[1] / cnt, sum[2] / cnt, sum[3] / cnt);
								static std::atomic<uint32_t> s_dci_ppm {0};
								if (bppd == 4 && texture.width >= 256 && (sum[0] / cnt > 120.0 && sum[1] / cnt < 60.0) && s_dci_ppm.fetch_add(1) < 12) {
									char dpath[128];
									std::snprintf(dpath, sizeof(dpath), "/tmp/fix6/csimg_%llu_%u_%llx.ppm", (unsigned long long)dfl, res, (unsigned long long)addr);
									if (FILE* f = std::fopen(dpath, "wb")) {
										std::fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)texture.width, (unsigned long)texture.height);
										for (size_t i = 0; i < pix.size(); i += 4) std::fwrite(&pix[i], 1, 3, f);
										std::fclose(f);
									}
								}
							}
						}
					}
					if (KYTY_ENV("KYTY_MTL_FAULT_INFO") != nullptr) {
						std::fprintf(stderr, "[CsImgBind] hash=0x%016llx slot=%u res=%u addr=0x%llx T#=%ux%u tfmt=%u tile=%u base_lvl=%u last_lvl=%u type=%lu fmt=%lu usage=%lu storage=%lu %lux%lu storage_img=%d\n",
						             (unsigned long long)cprog.shader_hash, slot, res, (unsigned long long)addr, w, h,
						             (unsigned)tex.Format(), (unsigned)tex.TileMode(),
						             (unsigned)tex.BaseLevel(), (unsigned)tex.LastLevel(),
						             (unsigned long)texture.textureType, (unsigned long)texture.pixelFormat,
						             (unsigned long)texture.usage, (unsigned long)texture.storageMode,
						             (unsigned long)texture.width, (unsigned long)texture.height,
						             ShaderRecompiler::IR::ImageBindingResourceClass(binding.kind) == ShaderRecompiler::IR::ImageResourceClass::Storage ? 1 : 0);
					}
					if (KYTY_ENV("KYTY_TRACE_CSIMG") != nullptr && s_cs_img_log.fetch_add(1) < 40) {
						std::fprintf(stderr, "[CsImg] slot=%u addr=0x%llx %ux%u fmt=%u storage=%d obj=%p id=%llx\n", slot,
						             (unsigned long long)addr, w, h, (unsigned)tex.Format(), storage ? 1 : 0, (__bridge void*)texture,
						             (unsigned long long)[texture gpuResourceID]._impl);
					}
				}
			}
		}
		// Native images preserve true null handles. The emitter guards every operation;
		// missing non-null resources are errors, never synthesized textures.
		if (compute_info.stage.program && compute_info.stage.program->native_msl) {
			const auto& cprog_n = *compute_info.stage.program;
			for (const auto& binding: cprog_n.bindings.descriptors) {
				const auto kd = static_cast<uint32_t>(binding.kind);
				const bool is_img_n = kd >= ShaderRecompiler::IR::FirstImageBinding &&
				                      kd < static_cast<uint32_t>(ShaderRecompiler::IR::DescriptorBindingKind::Samplers);
				if (is_img_n) {
					const bool storage_n = kd >= ShaderRecompiler::IR::FirstStorageImageBinding;
					for (uint32_t res: binding.resources) {
						const uint32_t slot_n = 64u + res;
						if (slot_n < 128 && arg_ptrs[slot_n] == 0 && res < cprog_n.info.images.size()) {
							RequireNullImageDescriptor(compute_info.stage, res);
							// KYTY_LOG_UNBOUND=1: the guest descriptor behind such a slot (null T#, or an address/type/format the binder dropped).
							static const bool s_log_unbound_cs = KYTY_ENV("KYTY_LOG_UNBOUND") != nullptr;
							if (s_log_unbound_cs) {
								static std::mutex s_ubc_mutex;
								static std::set<std::pair<uint64_t, uint32_t>> s_ubc_seen;
								std::lock_guard<std::mutex> ubc_lock(s_ubc_mutex);
								if (s_ubc_seen.insert({cprog_n.shader_hash, res}).second) {
									ShaderTextureResource tex {};
									const auto* snap = compute_info.stage.resources;
									const bool has_desc = snap != nullptr && res < snap->images.size();
									if (has_desc) for (size_t f = 0; f < 8; ++f) tex.fields[f] = snap->images[res].dwords[f];
									std::fprintf(stderr, "[Unbound] C%s hash=%016llx slot=%u desc=%d null=%d dim=%d addr=0x%llx %ux%u fmt=%u type=%u tile=%u lvls=%u-%u written=%d d=%08x %08x %08x %08x\n",
									             storage_n ? "S" : "R", (unsigned long long)cprog_n.shader_hash, res, has_desc ? 1 : 0, tex.IsNull() ? 1 : 0,
									             static_cast<int>(cprog_n.info.images[res].dimension), (unsigned long long)tex.Base40(),
									             static_cast<unsigned>(tex.Width5()) + 1u, static_cast<unsigned>(tex.Height5()) + 1u, (unsigned)tex.Format(),
									             (unsigned)tex.Type(), (unsigned)tex.TileMode(), (unsigned)tex.BaseLevel(), (unsigned)tex.LastLevel(),
									             cprog_n.info.images[res].written ? 1 : 0, tex.fields[0], tex.fields[1], tex.fields[2], tex.fields[3]);
									if (has_desc && !tex.IsNull()) {
										// Bounded diagnostics only: distinguish a missing mapping from
										// unsupported layout/binding without fabricating texture data.
										Libs::LibKernel::Memory::VirtualQueryInfo first {}, last {};
										const auto address = tex.Base40();
										const int first_status = Libs::LibKernel::Memory::KernelVirtualQuery(reinterpret_cast<const void*>(address),0,&first,sizeof(first));
										TileSurfaceLayout layout {};
										const TileSurfaceDescription description {tex.Format(),tex.TileMode(),TileSurfaceDimension::Dim2D,
										    uint32_t(tex.Width5())+1u,uint32_t(tex.Height5())+1u,1u,1u,1u};
										const bool layout_ok = cprog_n.info.images[res].dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
										    TileGetTiledTextureLayout(description,layout);
										const int last_status = layout_ok && layout.total_size != 0 && layout.total_size <= UINT64_MAX-address ?
										    Libs::LibKernel::Memory::KernelVirtualQuery(reinterpret_cast<const void*>(address+layout.total_size-1),0,&last,sizeof(last)) : -1;
										std::fprintf(stderr,"[UnboundRange] hash=%016llx slot=%u r128=%d layout=%d level0_bytes=%llu first=%x [%llx,%llx) committed=%u prot=%x last=%x [%llx,%llx) d4-7=%08x %08x %08x %08x\n",
										    (unsigned long long)cprog_n.shader_hash,res,cprog_n.info.images[res].r128?1:0,layout_ok?1:0,(unsigned long long)layout.total_size,
										    first_status,(unsigned long long)first.start,(unsigned long long)first.end,first.is_committed,first.protection,
										    last_status,(unsigned long long)last.start,(unsigned long long)last.end,tex.fields[4],tex.fields[5],tex.fields[6],tex.fields[7]);
									}
								}
							}

						}
					}
				} else if (binding.kind == ShaderRecompiler::IR::DescriptorBindingKind::Samplers) {
					for (uint32_t i : binding.resources) {
						if (arg_ptrs[128u + i] == 0) {
							id<MTLSamplerState> sampler = StageSampler(compute_info.stage, i);
							if (sampler == nil) RejectMetalState("missing compute sampler descriptor " + std::to_string(i));
							arg_ptrs[128u + i] = [sampler gpuResourceID]._impl;
						}
						if (StageHasPointFetchSampler(compute_info.stage, i)) {
							id<MTLSamplerState> point = StageSampler(compute_info.stage, i, true);
							if (point != nil) arg_ptrs[ShaderRecompiler::Msl::ComparisonPointSamplerBase + i] = point.gpuResourceID._impl;
						}
					}
				}
			}
		}
		if (compute_info.stage.program && !compute_info.stage.program->native_msl) {
			for (const auto& binding : compute_info.stage.program->bindings.descriptors) {
				if (ShaderRecompiler::IR::ImageBindingResourceClass(binding.kind) == ShaderRecompiler::IR::ImageResourceClass::None) continue;
				for (uint32_t j=0;j<binding.resources.size();++j) {
					const uint32_t slot=ShaderRecompiler::IR::NativeBinding(compute_info.stage.program->stage,binding.kind)+j;
					if (slot>=256 || arg_ptrs[slot] == 0) {
						RequireNullImageDescriptor(compute_info.stage,binding.resources[j]);
						RejectMetalState("SPIRV-Cross null compute image requires a null-handle lowering; no texture substituted");
					}
				}
			}
		}
		// KYTY_CS_ARGENC=1 (probe kernel only): build buffer 0 through MTLArgumentEncoder instead of raw
		// handle writes. The GPU reads argument buffers by MSL layout, not by [[id(N)]] numbers.
		// Default for all compute dispatches; KYTY_CS_ARGENC_OFF=1 restores the raw handle writes.
		const bool cs_argenc_here = KYTY_ENV("KYTY_CS_ARGENC_OFF") == nullptr && compute_info.stage.program != nullptr;
		bool cs_encoder_used = false; // buffer 0 must be bound exactly once: by the encoder or by the bytes
		if (cs_argenc_here) {
			id<MTLFunction> fn = MetalShaderManager::Instance().GetFunction(compute_info.stage.program->shader_hash);
			if (fn != nil && ShaderHasArgumentBuffer0(compute_info.stage.program->shader_hash)) {
				static std::unordered_map<uint64_t, id<MTLArgumentEncoder>> s_argenc;
				id<MTLArgumentEncoder> ae = nil;
				auto it = s_argenc.find(compute_info.stage.program->shader_hash);
				if (it != s_argenc.end()) {
					ae = it->second;
				} else {
					ae = [fn newArgumentEncoderWithBufferIndex:0];
					s_argenc[compute_info.stage.program->shader_hash] = ae;
				}
				id<MTLDevice> dev = MetalContext::Instance().GetDevice();
				id<MTLBuffer> argbuf = KytyNewBuffer(dev, ae.encodedLength, MTLResourceStorageModeShared, __LINE__);
				std::memset([argbuf contents], 0, ae.encodedLength);
				[ae setArgumentBuffer:argbuf offset:0];
				// KYTY_SPVC_ABI: compute ids come from the same per-shader layout as the graphics stages.
				static const bool s_spvc_abi_cs = KYTY_ENV("KYTY_SPVC_ABI") != nullptr;
				const bool cs_abi = s_spvc_abi_cs;
				const auto cs_layout = cs_abi ? Libs::Graphics::Metal::ComputeSpvcAbiLayout(compute_info.stage.program->bindings,
				                                                                            compute_info.stage.program->stage)
				                              : Libs::Graphics::Metal::SpvcAbiLayout {};
				const uint32_t cs_buf_base = cs_abi ? cs_layout.BaseOf(ShaderRecompiler::IR::DescriptorBindingKind::Buffers) : 0u;
				if (compute_info.stage.resources) {
					// Buffers: bind by argument index (the buffer index used by the shader's [[id(N)]]).
					const auto& bsnap = *compute_info.stage.resources;
					for (size_t b = 0; b < bsnap.buffers.size() && b < 64; ++b) {
						const auto& desc = bsnap.buffers[b];
						const uint64_t baddr = (static_cast<uint64_t>(desc.dwords[1] & 0xFFFF) << 32) | desc.dwords[0];
						if (baddr == 0) continue;
						const size_t bsize = sizes[b] != 0 ? sizes[b] : 4096;
						auto balloc = MetalBufferManager::Instance().GetBufferForGuestRange(baddr, bsize);
						if (balloc.gpu_addr != 0 && balloc.buffer != nil) {
							[ae setBuffer:balloc.buffer offset:balloc.offset atIndex:static_cast<NSUInteger>(cs_buf_base + b)];
							if (id<MTLComputeCommandEncoder> uenc = MetalContext::Instance().GetActiveComputeEncoder()) {
								[uenc useResource:balloc.buffer usage:(MTLResourceUsageRead | MTLResourceUsageWrite)];
							}
						}
						if (KYTY_ENV("KYTY_CS_PROBE") != nullptr && KYTY_ENV("KYTY_PROBE_HASH") != nullptr &&
						    std::strtoull(KYTY_ENV("KYTY_PROBE_HASH"), nullptr, 0) == compute_info.stage.program->shader_hash) {
							const uint32_t* w = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>([balloc.buffer contents]) + balloc.offset);
							size_t nz = 0;
							const size_t n = bsize / 4;
							for (size_t i = 0; i < n; ++i) nz += w[i] != 0;
							std::fprintf(stderr, "[CsIn] buf%zu guest=0x%llx size=%zu nonzero_dwords=%zu/%zu first:", b,
							             (unsigned long long)baddr, bsize, nz, n);
							for (size_t i = 0; i < 8 && i < n; ++i) std::fprintf(stderr, " %08x", w[i]);
							std::fprintf(stderr, "\n");
						}
					}
					for (const auto& b: compute_info.stage.program->bindings.descriptors) {
						const auto kd = static_cast<uint32_t>(b.kind);
						if (kd < ShaderRecompiler::IR::FirstImageBinding ||
						    kd >= static_cast<uint32_t>(ShaderRecompiler::IR::DescriptorBindingKind::Samplers)) {
							continue;
						}
						for (size_t r = 0; r < b.resources.size(); ++r) {
							const uint32_t res = b.resources[r];
							if (res >= compute_info.stage.resources->images.size()) continue;
							const auto& d = compute_info.stage.resources->images[res];
							ShaderTextureResource tx {};
							for (size_t f = 0; f < 8; ++f) tx.fields[f] = d.dwords[f];
							const uint32_t w = static_cast<uint32_t>(tx.Width5()) + 1u;
							const uint32_t h = static_cast<uint32_t>(tx.Height5()) + 1u;
							id<MTLTexture> tt = ComputeGuestImageFor(*compute_info.stage.program, res, tx, w, h, tx.Base40());
							if (tt == nil && KYTY_ENV("KYTY_MTL_FAULT_INFO") != nullptr) {
								std::fprintf(stderr, "[CsNullImg] hash=0x%016llx res=%u kind=%u addr=0x%llx %ux%u\n",
								             (unsigned long long)compute_info.stage.program->shader_hash, res,
								             static_cast<uint32_t>(b.kind), (unsigned long long)tx.Base40(), w, h);
							}
							if (tt == nil) continue;
							if (ShaderRecompiler::IR::ImageBindingResourceClass(b.kind) == ShaderRecompiler::IR::ImageResourceClass::Storage) {
								RegisterComputeWrittenImage(tx.Base40(), (__bridge void*)tt);
							}
							// Native MSL: direct layout (images at 64 + resource), matching the emitter's declarations.
							const uint32_t slot = compute_info.stage.program->native_msl ? 64u + res
							    : cs_abi ? cs_layout.BaseOf(b.kind) + static_cast<uint32_t>(r)
							    : ShaderRecompiler::IR::NativeBinding(compute_info.stage.program->stage, b.kind) +
							          static_cast<uint32_t>(r);
							tt = ConformToDeclaredArray(tt, res < compute_info.stage.program->info.images.size() &&
							    compute_info.stage.program->info.images[res].dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray);
							[ae setTexture:tt atIndex:slot];
							if (KYTY_ENV("KYTY_MTL_FAULT_INFO") != nullptr) {
								std::fprintf(stderr, "[CsImg] hash=0x%016llx slot=%u res=%u addr=0x%llx type=%lu fmt=%lu usage=%lu storage=%lu %lux%lu tex=%p\n",
								             (unsigned long long)compute_info.stage.program->shader_hash, slot, res,
								             (unsigned long long)tx.Base40(), (unsigned long)tt.textureType, (unsigned long)tt.pixelFormat,
								             (unsigned long)tt.usage, (unsigned long)tt.storageMode, (unsigned long)tt.width,
								             (unsigned long)tt.height, (__bridge void*)tt);
							}
							// The texture encoded here must be resident for this dispatch, whichever factory made it.
							if (id<MTLComputeCommandEncoder> uenc = MetalContext::Instance().GetActiveComputeEncoder()) {
								[uenc useResource:tt usage:ResidencyUsage(tt)];
							}
						}
					}
				}
				MetalDrawExecutor::Instance().SetComputeBuffer(argbuf, 0, 0);
				cs_encoder_used = true;
				std::fprintf(stderr, "[CsArgEnc] encodedLength=%lu slot33 via encoder\n", (unsigned long)ae.encodedLength);
			}
		}
		// Image format capabilities are known only after the image binding loop.
		MetalDrawExecutor::Instance().SetComputeBytes(sizes, sizeof(sizes), 25);
		if (!cs_encoder_used) {
			// Native MSL kernels whose encoder branch did not run still need buffer 0 bound; without
			// it Metal drops the dispatch (validation: missing Buffer binding at index 0 for ab).
			MetalDrawExecutor::Instance().SetComputeBytes(arg_ptrs, sizeof(arg_ptrs), 0);
		}
		if (KYTY_ENV("KYTY_CS_PROBE") != nullptr && compute_info.stage.program != nullptr &&
		    KYTY_ENV("KYTY_PROBE_HASH") != nullptr &&
		    std::strtoull(KYTY_ENV("KYTY_PROBE_HASH"), nullptr, 0) == compute_info.stage.program->shader_hash) {
			std::fprintf(stderr, "[CsArg] slot33=%llu slot49=%llu slot104=%llu\n", (unsigned long long)arg_ptrs[33],
			             (unsigned long long)arg_ptrs[49], (unsigned long long)arg_ptrs[104]);
		}

		uint32_t tg_w = compute_info.threads_num[0] > 0 ? compute_info.threads_num[0] : 64;
		uint32_t tg_h = compute_info.threads_num[1] > 0 ? compute_info.threads_num[1] : 1;
		uint32_t tg_d = compute_info.threads_num[2] > 0 ? compute_info.threads_num[2] : 1;
		// COMPUTE_DISPATCH_INITIATOR.USE_THREAD_DIMENSIONS: the dispatch dimensions are thread counts, not group counts
		// (the game's memory fill/copy kernels dispatch one thread per dword). The hardware launches ceil(threads / group size)
		// groups; taking the counts as groups would run group-size times too many threads per dimension.
		if (indirect_args_addr == 0 && (mode & (1u << 5u)) != 0  /* COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS */) {
			thread_group_x = thread_group_x / tg_w + (thread_group_x % tg_w != 0);
			thread_group_y = thread_group_y / tg_h + (thread_group_y % tg_h != 0);
			thread_group_z = thread_group_z / tg_d + (thread_group_z % tg_d != 0);
		}
		const bool cs_probe_here = KYTY_ENV("KYTY_CS_PROBE") != nullptr && compute_info.stage.program != nullptr &&
		                           KYTY_ENV("KYTY_PROBE_HASH") != nullptr &&
		                           std::strtoull(KYTY_ENV("KYTY_PROBE_HASH"), nullptr, 0) ==
		                               compute_info.stage.program->shader_hash;
		id<MTLBuffer> cs_dbg = nil;
		id<MTLTexture> cs_tex = nil;
		if (cs_probe_here && compute_info.stage.program != nullptr) {
			for (const auto& b: compute_info.stage.program->bindings.descriptors) {
				std::string rs;
				for (size_t r = 0; r < b.resources.size(); ++r) {
					const uint32_t res = b.resources[r];
					uint32_t dims = 0;
					if (compute_info.stage.resources && res < compute_info.stage.resources->images.size()) {
						const auto& dd = compute_info.stage.resources->images[res];
						ShaderTextureResource tt {};
						for (size_t f = 0; f < 8; ++f) tt.fields[f] = dd.dwords[f];
						dims = (static_cast<uint32_t>(tt.Width5()) + 1u) * 1000u + (static_cast<uint32_t>(tt.Height5()) + 1u);
					}
					rs += " res" + std::to_string(res) + "(" + std::to_string(dims) + ")->slot" +
					      std::to_string(ShaderRecompiler::IR::NativeBinding(compute_info.stage.program->stage, b.kind) + static_cast<uint32_t>(r));
				}
				std::fprintf(stderr, "[CsDesc] kind=%u n=%zu%s\n", static_cast<uint32_t>(b.kind), b.resources.size(), rs.c_str());
			}
		}
		if (cs_probe_here && compute_info.stage.program != nullptr) {
			for (const auto& b: compute_info.stage.program->bindings.descriptors) {
				const auto kd = static_cast<uint32_t>(b.kind);
				if (kd < ShaderRecompiler::IR::FirstStorageImageBinding || kd >= static_cast<uint32_t>(ShaderRecompiler::IR::DescriptorBindingKind::Samplers) || b.resources.empty()) continue;
				const uint32_t res0 = b.resources[0];
				if (compute_info.stage.resources && res0 < compute_info.stage.resources->images.size()) {
					const auto& d0 = compute_info.stage.resources->images[res0];
					ShaderTextureResource t0 {};
					for (size_t f = 0; f < 8; ++f) t0.fields[f] = d0.dwords[f];
					cs_tex = ComputeGuestImage(t0, static_cast<uint32_t>(t0.Width5()) + 1u, static_cast<uint32_t>(t0.Height5()) + 1u, t0.Base40());
				}
				break;
			}
		}
		if (cs_probe_here) {
			id<MTLDevice> dev = MetalContext::Instance().GetDevice();
			cs_dbg = KytyNewBuffer(dev, 65536, MTLResourceStorageModeShared, __LINE__);
			{
				uint32_t* init = static_cast<uint32_t*>([cs_dbg contents]);
				for (size_t i = 0; i < 16384; ++i) init[i] = 0xDEADBEEFu;
				init[0] = 0;
			}
			MetalDrawExecutor::Instance().SetComputeBuffer(cs_dbg, 0, 26);
			// Residency test: mark every storage texture of this dispatch resident on the active compute encoder.
			if (id<MTLComputeCommandEncoder> enc = MetalContext::Instance().GetActiveComputeEncoder()) {
				for (const auto& b: compute_info.stage.program->bindings.descriptors) {
					const auto kd = static_cast<uint32_t>(b.kind);
					if (kd < ShaderRecompiler::IR::FirstStorageImageBinding ||
					    kd >= static_cast<uint32_t>(ShaderRecompiler::IR::DescriptorBindingKind::Samplers)) {
						continue;
					}
					for (uint32_t res: b.resources) {
						if (compute_info.stage.resources && res < compute_info.stage.resources->images.size()) {
							const auto& d = compute_info.stage.resources->images[res];
							ShaderTextureResource tx {};
							for (size_t f = 0; f < 8; ++f) tx.fields[f] = d.dwords[f];
							const uint32_t w = static_cast<uint32_t>(tx.Width5()) + 1u;
							const uint32_t h = static_cast<uint32_t>(tx.Height5()) + 1u;
							if (id<MTLTexture> tt = ComputeGuestImage(tx, w, h, tx.Base40())) {
								if (ShaderRecompiler::IR::ImageBindingResourceClass(b.kind) == ShaderRecompiler::IR::ImageResourceClass::Storage) {
									RegisterComputeWrittenImage(tx.Base40(), (__bridge void*)tt);
								}
								[enc useResource:tt usage:ResidencyUsage(tt)];
							}
						}
					}
				}
				std::fprintf(stderr, "[CsDbg] residency marked on active compute encoder %p\n", (__bridge void*)enc);
			} else {
				std::fprintf(stderr, "[CsDbg] NO active compute encoder before dispatch\n");
			}
			std::fprintf(stderr, "[CsDbg] pipeline bound, groups=%u,%u,%u local=%u,%u,%u\n", thread_group_x,
			             thread_group_y, thread_group_z, tg_w, tg_h, tg_d);
		}
	// Residency must be applied after binding: cs_residency is filled while the buffers and images are
	// collected, so the earlier loop (before binding) ran on an empty list and nothing was resident.
	if (compute_pass_ok) {
		if (id<MTLComputeCommandEncoder> renc = MetalContext::Instance().GetActiveComputeEncoder()) {
			size_t tex_n = 0;
			for (id<MTLResource> r: cs_residency) {
				MetalDrawExecutor::Instance().UseResource(r, ResidencyUsage(r));
			}
			if (KYTY_ENV("KYTY_MTL_FAULT_INFO") != nullptr) {
				for (id<MTLResource> r: cs_residency) {
					if ([r conformsToProtocol:@protocol(MTLTexture)]) ++tex_n;
				}
			}
			if (KYTY_ENV("KYTY_MTL_FAULT_INFO") != nullptr && compute_info.stage.program != nullptr &&
			    compute_info.stage.program->shader_hash == 0xb3300709113f5521ull) {
				static std::atomic<uint32_t> s_res_log {0};
				if (s_res_log.fetch_add(1) < 6) {
					std::fprintf(stderr, "[CsResident] composite: %zu resources, %zu textures made resident on encoder %p\n",
					             cs_residency.size(), tex_n, (__bridge void*)renc);
				}
			}
		}
	}
	// A pair-packed wave64 kernel (MslEmitter.h, Wave64Pair) carries two guest lanes per host thread: half as many host threads per
	// group, in one dimension. The group count is the guest's.
	uint32_t host_w = tg_w, host_h = tg_h, host_d = tg_d;
	if (compute_info.stage.program != nullptr) {
		ShaderStageInputInfo pair_info;
		pair_info.compute = &compute_info;
		const bool dispatch_pair = ShaderRecompiler::Msl::Wave64Pair(compute_info.stage.program->stage, compute_info.stage.program->wave_size, pair_info, compute_info.stage.program->shader_hash);
		bool       compiled_pair = false;
		uint32_t   compiled_threads = 0;
		const bool known = ShaderRecompiler::Msl::Wave64PairCompiled(compute_info.stage.program->shader_hash, &compiled_pair, &compiled_threads);
		static std::atomic<uint32_t> s_pair_mismatch {0};
		if (known && (compiled_pair != dispatch_pair || (compiled_pair && compiled_threads != tg_w * tg_h * tg_d)) && s_pair_mismatch.fetch_add(1) < 20) {
			std::fprintf(stderr, "[PairMismatch] cs=0x%016llx compiled pair=%d threads=%u, dispatch pair=%d threads=%ux%ux%u\n",
			             static_cast<unsigned long long>(compute_info.stage.program->shader_hash), compiled_pair ? 1 : 0, compiled_threads,
			             dispatch_pair ? 1 : 0, tg_w, tg_h, tg_d);
		}
		// The kernel that runs is the one compiled: size the group by what it was built for.
		if (known ? compiled_pair : dispatch_pair) {
			host_w = (known ? compiled_threads : tg_w * tg_h * tg_d) / 2u;
			host_h = 1u;
			host_d = 1u;
		}
	}
	// KYTY_MSL_NAN_TRACE=<compute hash>: the kernel was emitted with a debug buffer at index 28 and records the smallest (id * 2 + half) of its
	// NaN float results there (MslEmitter.cpp, [NanId] lines at compile time give the id -> IR instruction map).
	id<MTLBuffer> nan_dbg = nil;
	if (NanTraceCs(cs_hash) || (compute_info.stage.program != nullptr && NanTraceCs(compute_info.stage.program->guest_hash))) {
		// One buffer per dispatch: the completion handler below reads it whenever the GPU is done, outside a traced frame too.
		id<MTLBuffer> s_nan_dbg = [MetalContext::Instance().GetDevice() newBufferWithLength:16 options:MTLResourceStorageModeShared];
		auto* d = static_cast<uint32_t*>(s_nan_dbg.contents);
		d[0] = 0xffffffffu;
		d[1] = 0u;
		MetalDrawExecutor::Instance().SetComputeBuffer(s_nan_dbg, 0, 28);
		nan_dbg = s_nan_dbg;
	}
	// Sample stores inside the shader: descriptor-sized scans also include bytes this dispatch never writes.
	id<MTLBuffer> store_dbg = nil;
	bool store_sample = false;
	if (const char* trace = KYTY_ENV("KYTY_MSL_STORE_TRACE"); trace != nullptr && compute_info.stage.program) {
		bool wanted = false;
		for (const char* p = trace; *p != '\0';) {
			char* end = nullptr;
			const uint64_t hash = std::strtoull(p, &end, 0);
			if (end == p) break;
			wanted |= hash == compute_info.stage.program->guest_hash;
			p = *end == ',' ? end + 1 : end;
		}
		if (wanted) {
			static const uint32_t every = KYTY_ENV("KYTY_MSL_STORE_EVERY") ? std::max(1u, static_cast<uint32_t>(std::strtoul(KYTY_ENV("KYTY_MSL_STORE_EVERY"), nullptr, 0))) : 60u;
			static const uint64_t first = KYTY_ENV("KYTY_MSL_STORE_FIRST") ? std::strtoull(KYTY_ENV("KYTY_MSL_STORE_FIRST"), nullptr, 0) : 0ull;
			static std::map<uint64_t, uint32_t> seen;
			store_sample = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed) >= first && seen[cs_hash]++ % every == 0;
			store_dbg = KytyNewBuffer(MetalContext::Instance().GetDevice(), (1 + 64 * 5 + 32 * 4) * sizeof(uint32_t), MTLResourceStorageModeShared, __LINE__);
			auto* words = static_cast<uint32_t*>(store_dbg.contents);
			std::memset(words, 0, store_dbg.length);
			words[0] = store_sample ? 1u : 0u;
			for (size_t b = 0; b < 64; ++b) words[1 + b * 4] = UINT32_MAX;
			for (size_t r = 0; r < 32; ++r) words[321 + r * 4 + 3] = UINT32_MAX;
			MetalDrawExecutor::Instance().SetComputeBuffer(store_dbg, 0, 27);
		}
	}
	// Scalar diagnostics for guest lanes 0/32 of group zero. Unexecuted values retain a sentinel.
	id<MTLBuffer> value_dbg = nil;
	std::vector<uint32_t> value_ids;
	bool value_sample = false;
	if (const char* trace = KYTY_ENV("KYTY_MSL_VALUE_TRACE"); trace && compute_info.stage.program) {
		char* end = nullptr;
		if (std::strtoull(trace, &end, 0) == compute_info.stage.program->guest_hash) {
			while (end && *end == ',') {
				const char* p = end + 1;
				const auto id = std::strtoul(p, &end, 0);
				if (end == p) break;
				if (id < 8192) value_ids.push_back(static_cast<uint32_t>(id));
			}
			if (!value_ids.empty()) {
				static std::map<uint64_t, uint32_t> seen;
				// KYTY_MSL_VALUE_FIRST=<flip> (default 1500) and KYTY_MSL_VALUE_EVERY=<n> (default 60): which dispatches are sampled.
				static const uint64_t value_first = KYTY_ENV("KYTY_MSL_VALUE_FIRST") ? std::strtoull(KYTY_ENV("KYTY_MSL_VALUE_FIRST"), nullptr, 0) : 1500ull;
				static const uint32_t value_every = KYTY_ENV("KYTY_MSL_VALUE_EVERY") ? std::max(1u, static_cast<uint32_t>(std::strtoul(KYTY_ENV("KYTY_MSL_VALUE_EVERY"), nullptr, 0))) : 60u;
				value_sample = Libs::Graphics::TraceWindow::g_flip.load() >= value_first && seen[cs_hash]++ % value_every == 0;
				value_dbg = KytyNewBuffer(MetalContext::Instance().GetDevice(), value_sample ? (1 + 8192 * 2) * 4 : 4, MTLResourceStorageModeShared, __LINE__);
				std::memset(value_dbg.contents, 0xcd, value_dbg.length);
				static_cast<uint32_t*>(value_dbg.contents)[0] = value_sample ? 1u : 0u;
				MetalDrawExecutor::Instance().SetComputeBuffer(value_dbg, 0, 29);
			}
		}
	}
	NSString* replay_dir = nil;
	if (replay_dump_here) {
		const auto& prog = *compute_info.stage.program;
		static uint64_t sequence = 0;
		replay_dir = gds_trip ? GdsTripDir(prog.guest_hash, cs_hash) : gds_snapshot ? GdsTripSnapshotDir() : [NSString stringWithFormat:@"%s/%016llx-%016llx-f%llu-d%llu", KYTY_ENV("KYTY_REPLAY_DUMP_DIR"),
		    (unsigned long long)prog.guest_hash, (unsigned long long)cs_hash,
		    (unsigned long long)TraceWindow::g_flip.load(), (unsigned long long)sequence++];
		[[NSFileManager defaultManager] createDirectoryAtPath:replay_dir withIntermediateDirectories:YES attributes:nil error:nil];
		const bool before_ok = ReplayDumpPhase(replay_dir,@"before",cs_buffers,replay_images,sizes,arg_ptrs[161] ? GdsBuffer() : nil);
		ReplayWrite(replay_dir,@"push.bin",&push_data,sizeof(push_data));
		ReplayWrite(replay_dir,@"sizes.bin",sizes,sizeof(sizes));
		ShaderRecompiler::IR::PushData reference_push {};
		const auto& snap = *compute_info.stage.resources;
		for (size_t i=0;i<prog.bindings.user_data_registers.size();++i) {
			const uint32_t reg=prog.bindings.user_data_registers[i],dst=prog.bindings.push_data_start_dword+i;
			if(reg>=prog.user_data_base && reg-prog.user_data_base<snap.user_data.size() && dst<reference_push.dwords.size())
				reference_push.dwords[dst]=snap.user_data[reg-prog.user_data_base];
		}
		// Offline buffers bind the exact native slice, so the Vulkan aligned-base byte adjustments are zero.
		ReplayWrite(replay_dir,@"reference-push.bin",&reference_push,sizeof(reference_push));
		if(replay_srt.buffer)ReplayWrite(replay_dir,@"srt.bin",(const uint8_t*)replay_srt.buffer.contents+replay_srt.offset,replay_srt.size);
		NSMutableArray* samplers=[NSMutableArray new];
		for(size_t i=0;i<prog.info.samplers.size();++i) {
			ShaderSamplerResource sampler {};
			std::copy_n(sizes+ShaderRecompiler::Msl::SamplerDescriptorWordBase+i*4,4,sampler.fields);
			if(prog.info.samplers[i].depth_compare)sampler.fields[0]&=~(3u<<29u);else sampler.fields[0]&=~(7u<<12u);
			NSMutableArray* variants=[NSMutableArray new];
			for(bool point:{false,true}) {
				MTLSamplerDescriptor*d=MakeSamplerDescriptor(sampler,point);
				[variants addObject:@{@"min":@(d.minFilter),@"mag":@(d.magFilter),@"mip":@(d.mipFilter),@"s":@(d.sAddressMode),@"t":@(d.tAddressMode),@"r":@(d.rAddressMode),@"normalized":@(d.normalizedCoordinates),@"anisotropy":@(d.maxAnisotropy),@"lod_min":@(d.lodMinClamp),@"lod_max":@(d.lodMaxClamp),@"bias":@(d.lodBias),@"compare":@(d.compareFunction),@"border":@(d.borderColor),@"reduction":@(d.reductionMode)}];
			}
			[samplers addObject:@{@"slot":@(i),@"variants":variants,@"point_fetch":@(StageHasPointFetchSampler(compute_info.stage,i))}];
		}
		const uint32_t* group_values=indirect_args?(const uint32_t*)indirect_args.contents:nullptr;
		ReplayJson(replay_dir,@"dispatch.json",@{@"guest_hash":[NSString stringWithFormat:@"%016llx",(unsigned long long)prog.guest_hash],@"native_hash":[NSString stringWithFormat:@"%016llx",(unsigned long long)cs_hash],@"complete":@(before_ok),@"groups":@[@(group_values?group_values[0]:thread_group_x),@(group_values?group_values[1]:thread_group_y),@(group_values?group_values[2]:thread_group_z)],@"threads":@[@(host_w),@(host_h),@(host_d)],@"samplers":samplers});
		// The snapshot ended the encoder. Rebind the exact pipeline, bytes and residency before the original dispatch.
		if(!MetalDrawExecutor::Instance().BeginComputePass())return;
		MetalDrawExecutor::Instance().SetComputePipeline(pstate);
		MetalDrawExecutor::Instance().SetComputeBytes(&push_data,sizeof(push_data),1);
		MetalDrawExecutor::Instance().SetComputeBytes(sizes,sizeof(sizes),25);
		MetalDrawExecutor::Instance().SetComputeBytes(arg_ptrs,sizeof(arg_ptrs),0);
		for(id<MTLResource>r:cs_residency)MetalDrawExecutor::Instance().UseResource(r,ResidencyUsage(r));
		if(replay_srt.buffer)MetalDrawExecutor::Instance().UseResource(replay_srt.buffer,MTLResourceUsageRead);
		if(nan_dbg)MetalDrawExecutor::Instance().SetComputeBuffer(nan_dbg,0,28);
		if(store_dbg)MetalDrawExecutor::Instance().SetComputeBuffer(store_dbg,0,27);
		if(value_dbg)MetalDrawExecutor::Instance().SetComputeBuffer(value_dbg,0,29);
		std::fprintf(stderr,"[ReplayDump] before complete=%d %s\n",before_ok,replay_dir.UTF8String);
		// The capture holds the exact inputs; the kernel is not encoded.
		if (gds_trip) GdsTripFinish(replay_dir, prog.guest_hash, cs_hash, gds_trip_value, before_ok);
	}
	// KYTY_GDS_TRIP_DRAIN=1 (diagnostic): the checked GDS kernel's pre-dispatch work (bindings, imports) completes on the GPU
	// before the kernel itself is encoded, without capturing anything.
	if (!replay_dir && compute_info.stage.program && GdsTrip().on && KYTY_ENV("KYTY_GDS_TRIP_DRAIN") != nullptr &&
	    (GdsTrip().hash == trip_guest || GdsTrip().hash == cs_hash)) {
		MetalContext::Instance().CommitCommandBuffer(true);
		if (!MetalDrawExecutor::Instance().BeginComputePass()) return;
		MetalDrawExecutor::Instance().SetComputePipeline(pstate);
		MetalDrawExecutor::Instance().SetComputeBytes(&push_data, sizeof(push_data), 1);
		MetalDrawExecutor::Instance().SetComputeBytes(sizes, sizeof(sizes), 25);
		MetalDrawExecutor::Instance().SetComputeBytes(arg_ptrs, sizeof(arg_ptrs), 0);
		for (id<MTLResource> r: cs_residency) MetalDrawExecutor::Instance().UseResource(r, ResidencyUsage(r));
		if (replay_srt.buffer) MetalDrawExecutor::Instance().UseResource(replay_srt.buffer, MTLResourceUsageRead);
		if (nan_dbg) MetalDrawExecutor::Instance().SetComputeBuffer(nan_dbg, 0, 28);
		if (store_dbg) MetalDrawExecutor::Instance().SetComputeBuffer(store_dbg, 0, 27);
		if (value_dbg) MetalDrawExecutor::Instance().SetComputeBuffer(value_dbg, 0, 29);
	}
	// KYTY_GDS_TRIP_SPLIT_WB=1 (diagnostic): the checked kernel goes to the GPU in its own packet and the canonical writeback of
	// its storage images in the next one, so a packet timeout names which of the two did not finish. Same work, same order.
	const bool split_wb = compute_info.stage.program && GdsTrip().on && KYTY_ENV("KYTY_GDS_TRIP_SPLIT_WB") != nullptr &&
	                      (GdsTrip().hash == trip_guest || GdsTrip().hash == cs_hash);
	if (!split_wb)
		for (auto& writeback:canonical_writes) MetalContext::Instance().QueueEncoderWriteback(std::move(writeback));
	if (indirect_args) {
		MetalDrawExecutor::Instance().DispatchIndirect(indirect_args, 0, host_w, host_h, host_d);
	} else {
		MetalDrawExecutor::Instance().Dispatch(thread_group_x, thread_group_y, thread_group_z, host_w, host_h, host_d);
	}
	if (split_wb) {
		std::fprintf(stderr, "[GdsTrip] split: kernel packet then %zu writebacks\n", canonical_writes.size());
		for (const auto& [res, binding]: canonical_images)
			std::fprintf(stderr, "[GdsTrip]   canonical res=%u %lux%lu fmt=%lu levels=%lu type=%lu bytes=%llu spans=%zu\n", res,
			             (unsigned long)binding.image.width, (unsigned long)binding.image.height, (unsigned long)binding.image.pixelFormat,
			             (unsigned long)binding.image.mipmapLevelCount, (unsigned long)binding.image.textureType,
			             (unsigned long long)binding.physical.size, binding.physical.spans.size());
		std::fflush(stderr);
		MetalContext::Instance().CommitCommandBuffer(false);
		for (size_t i = 0; i < canonical_writes.size(); ++i) {
			(void)MetalContext::Instance().GetCurrentCommandBuffer();
			MetalContext::Instance().QueueEncoderWriteback(std::move(canonical_writes[i]));
			MetalContext::Instance().EndCurrentEncoder();
			std::fprintf(stderr, "[GdsTrip] writeback %zu committing\n", i);
			std::fflush(stderr);
			MetalContext::Instance().CommitCommandBuffer(false);
		}
	}
	if (replay_dir && !gds_snapshot) {
		const bool after_ok=ReplayDumpPhase(replay_dir,@"after",cs_buffers,replay_images,sizes,arg_ptrs[161] ? GdsBuffer() : nil);
		std::fprintf(stderr,"[ReplayDump] after complete=%d %s\n",after_ok,replay_dir.UTF8String);
	}
	FinishBcWriteback();
	if (compute_pass_ok) TraceNoteBufferWriters(compute_info, cs_hash);
	if (value_dbg != nil && value_sample) {
		const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load();
		[MetalContext::Instance().GetCurrentCommandBuffer() addCompletedHandler:^(id<MTLCommandBuffer> command) {
			if (command.status != MTLCommandBufferStatusCompleted) return;
			const auto* words = static_cast<const uint32_t*>(value_dbg.contents);
			std::string values;
			for (uint32_t id: value_ids) {
				char text[96];
				std::snprintf(text, sizeof(text), " v%u=%08x/%08x", id, words[1 + id * 2], words[2 + id * 2]);
				values += text;
			}
			std::fprintf(stderr, "[CsValues] cs=0x%016llx flip=%llu%s\n", (unsigned long long)cs_hash, (unsigned long long)flip, values.c_str());
		}];
	}
	if (store_dbg != nil && store_sample) {
		std::array<uint64_t, 64> bases {};
		const auto& buffers = compute_info.stage.resources->buffers;
		for (size_t b = 0; b < buffers.size() && b < bases.size(); ++b)
			bases[b] = (static_cast<uint64_t>(buffers[b].dwords[1] & 0xffffu) << 32) | buffers[b].dwords[0];
		const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
		const uint64_t hash = cs_hash;
		const uint32_t groups = thread_group_x;
		[MetalContext::Instance().GetCurrentCommandBuffer() addCompletedHandler:^(id<MTLCommandBuffer> command) {
			if (command.status != MTLCommandBufferStatusCompleted) return;
			const auto* words = static_cast<const uint32_t*>(store_dbg.contents);
			for (size_t b = 0; b < bases.size(); ++b) {
				const uint32_t* w = words + 1 + b * 4;
				if (w[2] == 0) continue;
				std::fprintf(stderr, "[CsStore] cs=0x%016llx flip=%llu groups=%u b%zu addr=0x%llx range=%u..%u stores=%u float_nan=%u half_nan=%u\n",
				             (unsigned long long)hash, (unsigned long long)flip, groups, b, (unsigned long long)bases[b], w[0], w[1], w[2], w[3], words[257 + b]);
			}
			for (size_t r = 0; r < 32; ++r) {
				const uint32_t* w = words + 321 + r * 4;
				if (w[0] == 0) continue;
				float maximum;
				std::memcpy(&maximum, &w[2], sizeof(maximum));
				std::fprintf(stderr, "[CsImageStore] cs=0x%016llx flip=%llu groups=%u r%zu stores=%u nan=%u max=%g first_nan=%u,%u\n",
				             (unsigned long long)hash, (unsigned long long)flip, groups, r, w[0], w[1], maximum, w[3] & 65535u, w[3] >> 16u);
			}
		}];
	}
	// KYTY_VERT_NAN_METER=<hash>[,<hash>...]: the NaN share of every large buffer of those kernels once the GPU has run them, one line per 5 dispatches of each kernel.
	if (const char* vm = KYTY_ENV("KYTY_VERT_NAN_METER"); vm != nullptr && compute_info.stage.resources) {
		bool wanted = false;
		for (const char* p = vm; *p != '\0' && !wanted;) {
			char* end = nullptr;
			const uint64_t h = std::strtoull(p, &end, 0);
			if (end == p) break;
			wanted = h == cs_hash;
			p = *end == ',' ? end + 1 : end;
		}
		static std::map<uint64_t, uint32_t> s_vm_n;
		if (wanted && s_vm_n[cs_hash]++ % 5 == 0) {
			struct VmBuf { uint32_t index; uint64_t base; uint32_t bytes; };
			std::vector<VmBuf> vb;
			const auto& bufs = compute_info.stage.resources->buffers;
			for (size_t b = 0; b < bufs.size(); ++b) {
				const uint64_t base = (static_cast<uint64_t>(bufs[b].dwords[1] & 0xFFFF) << 32) | bufs[b].dwords[0];
				const uint32_t bytes = VsharpBytes(bufs[b].dwords);
				if (base != 0 && bytes >= 2000 && bytes <= (64u << 20)) vb.push_back({static_cast<uint32_t>(b), base, bytes});
			}
			const uint64_t vflip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
			const uint64_t vcs = cs_hash;
			const uint32_t vgx = thread_group_x;
			[MetalContext::Instance().GetCurrentCommandBuffer() addCompletedHandler:^(id<MTLCommandBuffer>) {
				std::string text;
				for (const auto& v: vb) {
					std::vector<uint32_t> w(v.bytes / 4);
					size_t nans = 0;
					if (!w.empty() && Libs::LibKernel::Memory::TryReadBacking(v.base, w.data(), w.size() * 4)) {
						for (uint32_t x: w) nans += (x & 0x7f800000u) == 0x7f800000u && (x & 0x7fffffu) != 0u ? 1 : 0;
					}
					char t[80];
					std::snprintf(t, sizeof(t), " b%u:%u=%.4f", v.index, v.bytes, w.empty() ? 0.0 : static_cast<double>(nans) / static_cast<double>(w.size()));
					text += t;
				}
				std::fprintf(stderr, "[VertNan] flip=%llu cs=%08llx g=%u%s\n", (unsigned long long)vflip, (unsigned long long)(vcs >> 32), vgx, text.c_str());
			}];
		}
	}
	if (nan_dbg != nil) {
		id<MTLCommandBuffer> nan_cmd = MetalContext::Instance().GetCurrentCommandBuffer();
		const uint64_t nan_flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
		const uint64_t nan_cs = cs_hash;
		id<MTLBuffer> nan_buf = nan_dbg;
		[nan_cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
			const uint32_t code = static_cast<const uint32_t*>(nan_buf.contents)[0];
			if (code == 0xffffffffu) return;
			static std::mutex s_nan_mutex;
			static std::map<std::pair<uint64_t, uint32_t>, uint64_t> s_nan_counts;
			std::lock_guard<std::mutex> nan_lock(s_nan_mutex);
			if (++s_nan_counts[{nan_cs, code}] == 1 && s_nan_counts.size() < 300) {
				std::fprintf(stderr, "[NanLive] flip=%llu cs=0x%016llx first NaN v%u%s\n", (unsigned long long)nan_flip, (unsigned long long)nan_cs, code / 2u, (code & 1u) != 0u ? "_h" : "");
			}
		}];
	}
	// KYTY_CS_BUF_AFTER=<metal or guest hash>,<buffer index>,<first flip>,<minimum bytes>: snapshot an output
	// after this dispatch in GPU order. It splits the command stream without waiting on the CPU.
	// Index 64 samples all bound buffers once per 60 dispatches instead of one buffer every dispatch.
	if (const char* probe = KYTY_ENV("KYTY_CS_BUF_AFTER"); probe != nullptr && compute_info.stage.resources) {
		unsigned long long hash = 0, first_flip = 0, min_bytes = 0, last_flip = UINT64_MAX;
		unsigned index = 0;
		const int fields = std::sscanf(probe, "%llx,%u,%llu,%llu,%llu", &hash, &index, &first_flip, &min_bytes, &last_flip);
		const uint64_t flip = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
		static std::map<uint64_t, uint32_t> all_seen;
		static const uint32_t all_every = KYTY_ENV("KYTY_CS_BUF_AFTER_EVERY") ? std::max(1u, static_cast<uint32_t>(std::strtoul(KYTY_ENV("KYTY_CS_BUF_AFTER_EVERY"), nullptr, 0))) : 60u;
		// Hash zero follows the STORE_TRACE selection, so several linked kernels can be captured together.
		// KYTY_CS_BUF_AFTER_INIT=<guest hash>: capture only in flips where that kernel was dispatched (a garment's first frame), from it on.
		static const uint64_t init_guest = KYTY_ENV("KYTY_CS_BUF_AFTER_INIT") ? std::strtoull(KYTY_ENV("KYTY_CS_BUF_AFTER_INIT"), nullptr, 0) : 0ull;
		static uint64_t init_flip = UINT64_MAX;
		if (init_guest != 0 && compute_info.stage.program && compute_info.stage.program->guest_hash == init_guest) init_flip = flip;
		// KYTY_CS_BUF_AFTER_ALSO=<metal hash>[,<metal hash>...]: more kernels captured with the same buffer and flip selection.
		const auto after_also = [&] {
			const char* p = KYTY_ENV("KYTY_CS_BUF_AFTER_ALSO");
			while (p != nullptr && *p != '\0') {
				char* end = nullptr;
				const uint64_t h = std::strtoull(p, &end, 16);
				if (end == p) break;
				if (h == cs_hash) return true;
				p = *end == ',' ? end + 1 : end;
			}
			return false;
		};
		// KYTY_CS_BUF_AFTER_TRIGGER=<file>: capture only the whole flip after that file appears (the file is removed).
		static const char* after_trigger = KYTY_ENV("KYTY_CS_BUF_AFTER_TRIGGER");
		if (after_trigger != nullptr && access(after_trigger, F_OK) == 0) {
			unlink(after_trigger);
			s_cs_after_trigger_flip = flip + 1;
			std::fprintf(stderr, "[CsBufAfter] trigger: capturing flip %llu\n", (unsigned long long)s_cs_after_trigger_flip);
		}
		if (fields >= 2 && (hash == cs_hash || (compute_info.stage.program && hash == compute_info.stage.program->guest_hash) || (hash == 0 && store_dbg != nil) || after_also()) &&
		    flip >= first_flip && flip <= last_flip && (init_guest == 0 || flip == init_flip) && (after_trigger == nullptr || flip == s_cs_after_trigger_flip) &&
		    (index < cs_buffers.size() || (index == 64 && all_seen[cs_hash]++ % all_every == 0))) {
		static uint64_t next_capture = 0;
		const uint64_t capture = ++next_capture;
		const unsigned first_index = index == 64 ? 0 : index;
		const unsigned end_index = index == 64 ? static_cast<unsigned>(cs_buffers.size()) : index + 1;
		for (index = first_index; index < end_index && index < compute_info.stage.resources->buffers.size(); ++index) {
			const auto& src = cs_buffers[index];
			const size_t bytes = std::min<size_t>(src.size & ~size_t(3), 4u * 1024u * 1024u);
			const auto& desc = compute_info.stage.resources->buffers[index];
			const uint64_t addr = (static_cast<uint64_t>(desc.dwords[1] & 0xffffu) << 32) | desc.dwords[0];
			if (src.IsValid() && bytes >= min_bytes && bytes != 0 && src.offset <= src.buffer.length && bytes <= src.buffer.length - src.offset) {
				auto& metal = MetalContext::Instance();
				id<MTLBuffer> copy = KytyNewBuffer(metal.GetDevice(), bytes, MTLResourceStorageModeShared, __LINE__);
				metal.EndCurrentEncoder();
				id<MTLCommandBuffer> command = metal.GetCurrentCommandBuffer();
				id<MTLBlitCommandEncoder> blit = copy != nil ? [command blitCommandEncoder] : nil;
				if (blit != nil) {
					OrderBegin(blit);
					[blit copyFromBuffer:src.buffer sourceOffset:src.offset toBuffer:copy destinationOffset:0 size:bytes];
					OrderEnd(blit);
					[blit endEncoding];
					const uint64_t captured_hash = cs_hash;
					[command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
						if (completed.status != MTLCommandBufferStatusCompleted) {
							std::fprintf(stderr, "[CsBufAfter] cs=0x%016llx flip=%llu GPU copy failed status=%ld\n", hash, (unsigned long long)flip, (long)completed.status);
							return;
						}
						const auto* words = static_cast<const uint32_t*>(copy.contents);
						size_t nz = 0, large = 0, max_index = 0, nans = 0, first_nan = bytes / 4;
						uint32_t maximum = 0;
						for (size_t i = 0; i < bytes / 4; ++i) {
							nz += words[i] != 0;
							large += words[i] > 8192u * 64u;
							if ((words[i] & 0x7f800000u) == 0x7f800000u && (words[i] & 0x7fffffu) != 0u) {
								++nans;
								first_nan = std::min(first_nan, i);
							}
							if (words[i] > maximum) { maximum = words[i]; max_index = i; }
						}
						std::fprintf(stderr, "[CsBufAfter] cs=0x%016llx flip=%llu b%u addr=0x%llx bytes=%zu nonzero=%zu large=%zu max=%08x max_index=%zu nan=%zu first_nan=%zu first=%08x,%08x,%08x,%08x\n",
						             (unsigned long long)captured_hash, (unsigned long long)flip, index, (unsigned long long)addr, bytes, nz, large, maximum, max_index,
						             nans, first_nan, words[0], bytes >= 8 ? words[1] : 0u, bytes >= 12 ? words[2] : 0u, bytes >= 16 ? words[3] : 0u);
						// KYTY_CS_DUMP_ALL_DIR=<existing dir>: every captured buffer of the window, so a dispatch's inputs can be rebuilt from the
						// dispatches before it in GPU order (an output that aliases an input is already overwritten in its own after-snapshot).
						if (const char* all_dir = KYTY_ENV("KYTY_CS_DUMP_ALL_DIR")) {
							char path[1024];
							std::snprintf(path, sizeof(path), "%s/%016llx-%llu-%llu-b%u-%llx.bin", all_dir, (unsigned long long)captured_hash,
							              (unsigned long long)flip, (unsigned long long)capture, index, (unsigned long long)addr);
							if (FILE* file = std::fopen(path, "wb")) { std::fwrite(words, 1, bytes, file); std::fclose(file); }
						}
						// Preserve one complete failing dispatch for standalone native-Metal replay.
						// The selected output must be a known float stream; packed NaN bits do not qualify.
						if (const char* dir = KYTY_ENV("KYTY_CS_DUMP_NAN_DIR"); dir && store_dbg != nil) {
							static const unsigned output = KYTY_ENV("KYTY_CS_DUMP_NAN_BUFFER") ? static_cast<unsigned>(std::strtoul(KYTY_ENV("KYTY_CS_DUMP_NAN_BUFFER"), nullptr, 0)) : 13u;
							const auto* counts = static_cast<const uint32_t*>(store_dbg.contents);
							if (output < 64 && counts[1 + output * 4 + 3] != 0u) {
								static std::mutex mutex;
								static std::map<uint64_t, uint64_t> selected;
								std::lock_guard guard(mutex);
								auto [it, inserted] = selected.try_emplace(captured_hash, capture);
								if (it->second == capture) {
									char path[1024];
									std::snprintf(path, sizeof(path), "%s/%016llx-%llu-%llu-b%u.bin", dir, (unsigned long long)captured_hash,
									              (unsigned long long)flip, (unsigned long long)capture, index);
									if (FILE* file = std::fopen(path, "wb")) { std::fwrite(words, 1, bytes, file); std::fclose(file); }
									std::fprintf(stderr, "[CsReplayDump] cs=0x%016llx flip=%llu capture=%llu b%u addr=0x%llx bytes=%zu groups=%ux%ux%u host=%ux%ux%u\n",
									             (unsigned long long)captured_hash, (unsigned long long)flip, (unsigned long long)capture, index,
									             (unsigned long long)addr, bytes, thread_group_x, thread_group_y, thread_group_z, host_w, host_h, host_d);
								}
								(void)inserted;
							}
							}
					}];
					if (KYTY_ENV("KYTY_CS_BUF_AFTER_DEFER") == nullptr) metal.CommitCommandBuffer(false);
				} else {
					std::fprintf(stderr, "[CsBufAfter] cs=0x%016llx flip=%llu could not create snapshot\n", hash, (unsigned long long)flip);
				}
			}
		}
	}
	}
	if (nan_dbg != nil && PassTraceActive()) {
		(void)MetalContext::Instance().GetCurrentCommandBuffer();
		MetalContext::Instance().CommitCommandBuffer(true);
		const uint32_t code = static_cast<const uint32_t*>(nan_dbg.contents)[0];
		if (code == 0xffffffffu) {
			std::fprintf(stderr, "[NanTrace] cs=0x%016llx groups=%ux%ux%u: no NaN float result\n", (unsigned long long)cs_hash,
			             thread_group_x, thread_group_y, thread_group_z);
		} else {
			std::fprintf(stderr, "[NanTrace] cs=0x%016llx groups=%ux%ux%u: first NaN float result v%u%s (see the [NanId] line of this id)\n",
			             (unsigned long long)cs_hash, thread_group_x, thread_group_y, thread_group_z, code >> 1, (code & 1u) != 0 ? "_h" : "");
		}
	}
	s_trace_gcs_hash = compute_info.stage.program != nullptr ? compute_info.stage.program->guest_hash : 0ull;
	PassTraceReport(compute_info.stage.program != nullptr ? compute_info.stage.program->shader_hash : 0ull,
	                thread_group_x, thread_group_y, thread_group_z, tg_w, tg_h, tg_d, indirect_args);
	// KYTY_PASS_TRACE_CSBUF=1 (with KYTY_PASS_TRACE_CS=<hash>): the first 48 dwords of every buffer the traced kernel reads or writes,
	// read after the dispatch (the exposure the tone mapper uses lives in such a buffer).
	// KYTY_PASS_TRACE_CSBUF_FRAME=<metal hash> does the same while PassTrace is active, without tracing every dispatch from startup.
	// Forced kernel traces also count as active. It reads the bound allocation, including upload snapshots.
	static const uint64_t s_frame_csbuf_hash = KYTY_ENV("KYTY_PASS_TRACE_CSBUF_FRAME") != nullptr ?
	    std::strtoull(KYTY_ENV("KYTY_PASS_TRACE_CSBUF_FRAME"), nullptr, 0) : 0ull;
	if (PassTraceCsBuffers() || (s_frame_csbuf_hash != 0 && cs_hash == s_frame_csbuf_hash && PassTraceActive())) {
		// A buffer-only kernel returns early from PassTraceReport. Split-op mode
		// may also have committed without waiting. Fence the queue explicitly,
		// and inspect the actual bound allocation (possibly an upload snapshot).
		(void)MetalContext::Instance().GetCurrentCommandBuffer();
		MetalContext::Instance().CommitCommandBuffer(true);
		std::fprintf(stderr, "[PassTrace] CSBUF AFTER cs=0x%016llx groups=%ux%ux%u local=%ux%ux%u\n",
		             (unsigned long long)cs_hash, thread_group_x, thread_group_y, thread_group_z, tg_w, tg_h, tg_d);
		for (size_t bi = 0; bi < cs_buffers.size(); ++bi) {
			const auto& a = cs_buffers[bi];
			if (a.buffer == nil || a.buffer.contents == nullptr || a.offset > a.buffer.length) continue;
			const uint8_t* data = static_cast<const uint8_t*>(a.buffer.contents) + a.offset;
			const size_t n = std::min<size_t>(48, std::min<uint64_t>(a.size, a.buffer.length - a.offset) / 4);
			std::string text;
			for (size_t k = 0; k < n; ++k) {
				uint32_t dw;
				float f;
				std::memcpy(&dw, data + 4 * k, 4);
				std::memcpy(&f, &dw, 4);
				char t[40];
				std::snprintf(t, sizeof(t), " %08x(%g)", dw, f);
				text += t;
			}
			// Whole-buffer summary (same format as the Vulkan reference's CSBUF line): non-zero dwords and a histogram of the top nibble of the
			// even and odd dwords (a table of 8-byte records keeps its class bits there).
			uint64_t hi_even[16] = {}, hi_odd[16] = {}, nonzero = 0;
			const size_t total = std::min<size_t>(8u << 20, std::min<uint64_t>(a.size, a.buffer.length - a.offset) / 4);
			for (size_t k = 0; k < total; ++k) {
				uint32_t dw;
				std::memcpy(&dw, data + 4 * k, 4);
				nonzero += dw != 0;
				++((k & 1) ? hi_odd : hi_even)[dw >> 28];
			}
			std::string hist = " nz=" + std::to_string(nonzero) + "/" + std::to_string(total) + " hi_even=[";
			for (int h = 0; h < 16; ++h) hist += std::to_string(hi_even[h]) + (h < 15 ? "," : "]");
			hist += " hi_odd=[";
			for (int h = 0; h < 16; ++h) hist += std::to_string(hi_odd[h]) + (h < 15 ? "," : "]");
			std::fprintf(stderr, "[PassTrace]   CSBUF b%zu size=%llu%s:%s\n", bi, static_cast<unsigned long long>(a.size), hist.c_str(), text.c_str());
		}
	}
		if (compute_pass_ok && compute_info.stage.program && compute_info.stage.program->guest_hash == 0x5d3aaa8f1319d915ull)
			ProbeSkinUpload(cs_buffers);
		if (cs_probe_here) {
			MetalContext::Instance().CommitCommandBuffer(true);
			if (indirect_args) {
				MetalContext::Instance().WaitUntilIdle();
				const auto* groups = static_cast<const uint32_t*>(indirect_args.contents);
				thread_group_x = groups[0]; thread_group_y = groups[1]; thread_group_z = groups[2];
			}
			const uint32_t* dbg = static_cast<const uint32_t*>([cs_dbg contents]);
			const char* trace_flip_env = KYTY_ENV("KYTY_CS_TRACE_FLIP");
			const bool trace_flip_ok = trace_flip_env == nullptr ||
			    Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed) == std::strtoull(trace_flip_env, nullptr, 0);
			if (KYTY_ENV("KYTY_CS_TRACE_FILE") != nullptr && trace_flip_ok) {
				if (FILE* tf = std::fopen(KYTY_ENV("KYTY_CS_TRACE_FILE"), "w")) {
					for (size_t i = 1; i < 16384; ++i) {
						if (dbg[i] != 0xDEADBEEFu) std::fprintf(tf, "%zu 0x%08x\n", i, dbg[i]);
					}
					std::fclose(tf);
				}
			}
			const uint64_t expected = uint64_t(thread_group_x) * thread_group_y * thread_group_z * tg_w * tg_h * tg_d;
			std::fprintf(stderr, "[CsDbg] dbg0=%u expected=%llu dims(shader)=%ux%ux%u texture(obj)=%lux%lux%lu type=%ld fmt=%lu usage=%lu storage=%ld\n", dbg[0], (unsigned long long)expected, dbg[1], dbg[2], dbg[3], (unsigned long)[cs_tex width], (unsigned long)[cs_tex height], (unsigned long)[cs_tex depth], (long)[cs_tex textureType], (unsigned long)[cs_tex pixelFormat], (unsigned long)[cs_tex usage], (long)[cs_tex storageMode]);
		}
		if (KYTY_ENV("KYTY_TRACE_CSIMG") != nullptr) {
			static std::atomic<uint32_t> s_cs_disp_log {0};
			if (s_cs_disp_log.fetch_add(1) < 20) {
				std::fprintf(stderr, "[CsDispatch] groups=%u,%u,%u local=%u,%u,%u\n", thread_group_x,
				             thread_group_y, thread_group_z, tg_w, tg_h, tg_d);
			}
		}
		if (KYTY_ENV("KYTY_CS_READBACK") != nullptr && compute_info.stage.program != nullptr) {
			static bool s_cs_rb_done = false;
			if (!s_cs_rb_done) {
				// First storage image of this dispatch: commit and wait, then dump it.
				id<MTLTexture> storage_tex = nil;
				for (const auto& binding: compute_info.stage.program->bindings.descriptors) {
					const auto kind = static_cast<uint32_t>(binding.kind);
					if (kind < ShaderRecompiler::IR::FirstStorageImageBinding ||
					    kind >= static_cast<uint32_t>(ShaderRecompiler::IR::DescriptorBindingKind::Samplers) ||
					    binding.resources.empty()) {
						continue;
					}
					const uint32_t res = binding.resources[0];
					if (compute_info.stage.resources && res < compute_info.stage.resources->images.size()) {
						const auto& d = compute_info.stage.resources->images[res];
						ShaderTextureResource tex {};
						for (size_t f = 0; f < 8; ++f) tex.fields[f] = d.dwords[f];
						const uint32_t w = static_cast<uint32_t>(tex.Width5()) + 1u;
						const uint32_t h = static_cast<uint32_t>(tex.Height5()) + 1u;
						storage_tex = ComputeGuestImage(tex, w, h, tex.Base40());
					}
					break;
				}
				if (storage_tex != nil) {
					s_cs_rb_done = true;
					MetalContext::Instance().CommitCommandBuffer(true);
					std::fprintf(stderr, "[CsReadbackObj] obj=%p\n", (__bridge void*)storage_tex);
					std::fprintf(stderr, "[CsReadback] hash=0x%016llx tex %lux%lu fmt=%lu\n",
					             (unsigned long long)compute_info.stage.program->shader_hash,
					             (unsigned long)[storage_tex width], (unsigned long)[storage_tex height],
					             (unsigned long)[storage_tex pixelFormat]);
					if (storage_tex.textureType == MTLTextureType3D) {
						const NSUInteger tw = storage_tex.width, th = storage_tex.height, td_ = storage_tex.depth;
						const size_t bpp = BytesPerPixel(storage_tex.pixelFormat) ? BytesPerPixel(storage_tex.pixelFormat) : 4;
						std::vector<uint8_t> buf(tw * th * td_ * bpp, 0);
						[storage_tex getBytes:buf.data() bytesPerRow:tw * bpp bytesPerImage:tw * th * bpp
						           fromRegion:MTLRegionMake3D(0, 0, 0, tw, th, td_) mipmapLevel:0 slice:0];
						size_t nz = 0;
						for (uint8_t v : buf) nz += v != 0;
						std::fprintf(stderr, "[CsReadback3D] %lux%lux%lu bpp=%zu nonzero_bytes=%zu/%zu\n",
						             (unsigned long)tw, (unsigned long)th, (unsigned long)td_, bpp, nz, buf.size());
					} else {
						MetalPresenter::Instance().DumpTextureToPpm((__bridge void*)storage_tex, "/tmp/cs_out.ppm");
					}
				}
			}
		}
	}
}

void MetalRenderer::DispatchIndirect(uint64_t submit_id, const HW::Context& context, const HW::Shader& sh_ctx,
                                     PipelineCache& pipeline_cache, uint64_t args_addr,
                                     uint32_t mode) {
	if (args_addr == 0 || (args_addr & 3u) != 0) return;
	DispatchCompute(submit_id, context, sh_ctx, pipeline_cache, 1, 1, 1, mode, args_addr);
}

} // namespace Libs::Graphics::Metal

#include "metalGuestWrites.inc"
