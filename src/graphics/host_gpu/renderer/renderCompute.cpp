#include "graphics/host_gpu/renderer/productionProfile.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/shader/shaderBindings.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

// Vulkan reference pass trace (renderDraw.cpp).
bool VkPassTraceActive();
void VkPassTraceReport(RenderContext& context, bool draw, uint64_t vs_hash, uint64_t ps_or_cs_hash, const uint32_t* groups,
                       const std::vector<std::pair<ImageId, uint64_t>>& outputs);

// Direct dispatches are ordered only by the guest's own synchronization (CS_PARTIAL_FLUSH and
// cache events become global barriers). A host barrier after every dispatch kept the GPU on one
// cloth workgroup of EA SPORTS FC 27 for tens of milliseconds. KYTY_DISPATCH_OVERLAP=1 drops the
// per-dispatch host barriers. Off by default: they are the only ordering between dispatches that
// the guest does not order itself, and that has not been checked on this host.
static bool DispatchOverlap() {
    static const bool enabled = [] {
        const char* value = std::getenv("KYTY_DISPATCH_OVERLAP");
#if defined(__APPLE__)
        if (value == nullptr) {
            return true;
        }
#endif
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// TASK-0010 instrumentation: KYTY_DISPATCH_RANGES_CSV=<path> logs one row per compute dispatch
// with its thread-group dimensions and every buffer and image range it reads or writes, tagged
// bw/br/iw/ir, so an offline pass can test consecutive same-shader dispatches for WAW, RAW and
// WAR hazards (EXP-0015, EXP-0016). Diagnostic only; barrier elision on this basis was measured
// and reverted in EXP-0017.
// Off unless the variable is set; writes from the single producer thread that records dispatches.
static void LogDispatchRanges(uint64_t shader_hash, uint32_t gx, uint32_t gy, uint32_t gz,
                              const ShaderStageRuntime& runtime) {
	static FILE* csv = [] () -> FILE* {
		const char* path = std::getenv("KYTY_DISPATCH_RANGES_CSV");
		if (path == nullptr) {
			return nullptr;
		}
		FILE* file = std::fopen(path, "w");
		if (file != nullptr) {
			std::fprintf(file, "seq,shader,groups_x,groups_y,groups_z,res,slot,base,size\n");
		}
		return file;
	}();
	if (csv == nullptr || !runtime) {
		return;
	}
	static uint64_t    sequence  = 0;
	const auto         ordinal   = sequence++;
	const auto&        program   = *runtime.program;
	const auto&        resources = *runtime.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		return;
	}
	bool any = false;
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const char* tag = program.info.buffers[i].written ? "bw" : "br";
		const auto& value = resources.buffers[i];
		if (value.dword_count < 4) {
			continue;
		}
		ShaderBufferResource descriptor;
		std::memcpy(descriptor.fields, value.dwords.data(), sizeof(descriptor.fields));
		if (descriptor.Base48() == 0 || descriptor.NumRecords() == 0) {
			continue;
		}
		any = true;
		std::fprintf(csv, "%llu,0x%016llx,%u,%u,%u,%s,%u,0x%llx,%llu\n",
		             static_cast<unsigned long long>(ordinal),
		             static_cast<unsigned long long>(shader_hash), gx, gy, gz, tag, i,
		             static_cast<unsigned long long>(descriptor.Base48()),
		             static_cast<unsigned long long>(descriptor.GetSize()));
	}
	// Written storage images. Exact byte extents need format and tile math, so log Base40 with
	// the extent as an identifier: two dispatches writing the same base definitely overlap, which
	// is the direction that matters for deciding merge safety (EXP-0014's remaining gap).
	if (resources.images.size() == program.info.images.size()) {
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			const auto& image = program.info.images[i];
			const char* tag   = image.written ? "iw" : "ir";
			const auto  r = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
			if (r.IsNull()) {
				continue;
			}
			any = true;
			const auto extent = static_cast<uint64_t>(static_cast<uint32_t>(r.Width5()) + 1u) *
			                    (static_cast<uint32_t>(r.Height5()) + 1u) *
			                    (static_cast<uint32_t>(r.Depth()) + 1u);
			std::fprintf(csv, "%llu,0x%016llx,%u,%u,%u,%s,%u,0x%llx,%llu\n",
			             static_cast<unsigned long long>(ordinal),
			             static_cast<unsigned long long>(shader_hash), gx, gy, gz, tag, i,
			             static_cast<unsigned long long>(r.Base40()),
			             static_cast<unsigned long long>(extent));
		}
	}
	if (!any) {
		// Record the dispatch even with no written buffers or images so runs stay countable.
		std::fprintf(csv, "%llu,0x%016llx,%u,%u,%u,,,,\n", static_cast<unsigned long long>(ordinal),
		             static_cast<unsigned long long>(shader_hash), gx, gy, gz);
	}
}
static bool FillSourcesDisjoint(std::span<const ShaderRecompiler::IR::DescriptorValue> sources,
                                 GuestRange destination, uint32_t output_buffer = UINT32_MAX) {
	for (uint32_t i = 0; i < sources.size(); ++i) {
		if (i == output_buffer) continue;
		const auto source = DecodeNativeDescriptor<ShaderBufferResource>(sources[i]);
		const auto bytes  = source.GetSize();
		if (source.Base48() < destination.End() && destination.address < source.Base48() + bytes)
			return false;
	}
	return true;
}

bool RenderExecutor::TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
                                                const CommandBuffer&          buffer) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		EXIT("compute runtime buffer count does not match shader metadata\n");
	}
	auto& cache = buffer.GetContext().GetTextureCache();
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		// A metadata resource that is also read is not proven to be a full overwrite. Execute it
		// conservatively instead of replacing the dispatch with a coarse full-surface clear.
		if ((!resource.written || resource.read) && cache.IsMeta(descriptor.Base48())) {
			return false;
		}
	}

	if (!program.info.has_bitwise_xor) {
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& resource = program.info.buffers[i];
			if (resource.written) {
				const auto descriptor =
				    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
				if (cache.ClearMeta(descriptor.Base48())) {
					return true;
				}
			}
		}
	}
	return false;
}

bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode,
                              ShaderBufferResource& resolved_descriptor, uint32_t& resolved_clear,
                              uint64_t& resolved_size) {
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	if (fill.kind != ShaderRecompiler::IR::UniformFillKind::Buffer) {
		return false;
	}
	const auto element_size = fill.words * sizeof(uint32_t);
	const auto descriptor =
	    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[fill.resource]);
	constexpr std::array formats {
	    Prospero::BufferFormat::k32UInt, Prospero::BufferFormat::k32_32UInt,
	    Prospero::BufferFormat::k32_32_32UInt, Prospero::BufferFormat::k32_32_32_32UInt};
	if (descriptor.Stride() != element_size || descriptor.Format() != formats[fill.words - 1] ||
	    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid() ||
	    descriptor.Base48() == 0) {
		return false;
	}
	if (input.threads_num[0] == 0 || input.threads_num[0] != fill.group_stride[0] ||
	    input.threads_num[1] != 1 || input.threads_num[2] != 1 || group_x == 0 || group_y != 1 ||
	    group_z != 1 || mode != (input.dispatch_thread_dimensions ? 0x61u : 0x41u)) {
		return false;
	}
	const uint64_t invocations = input.dispatch_thread_dimensions
	                                 ? group_x
	                                 : static_cast<uint64_t>(group_x) * input.threads_num[0];
	const auto     size        = descriptor.GetSize();
	if (invocations != descriptor.NumRecords() || size == 0 || size > UINT32_MAX ||
	    (input.dispatch_thread_dimensions &&
	     (group_x % input.threads_num[0] != 0 || input.dispatch_threads_num[0] != group_x ||
	      input.dispatch_threads_num[1] != 1 || input.dispatch_threads_num[2] != 1))) {
		return false;
	}
	if (!FillSourcesDisjoint(resources.buffers, {descriptor.Base48(), size}, fill.resource))
		return false;
	resolved_descriptor = descriptor;
	resolved_clear      = fill.value;
	resolved_size       = size;
	return true;
}

bool RenderExecutor::TryConsumeComputeImageClear(const ShaderComputeInputInfo& input,
                                                CommandBuffer& command, uint32_t group_x,
                                                uint32_t group_y, uint32_t group_z, uint32_t mode) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	auto&       cache     = command.GetContext().GetTextureCache();
	if (fill.kind == ShaderRecompiler::IR::UniformFillKind::Image) {
		if (mode != 0x41u || input.dispatch_thread_dimensions || fill.value > 255 ||
		    input.threads_num[2] != 1)
			return false;
		const auto  descriptor = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[0]);
		const auto& resource   = program.info.images[0];
		if (descriptor.IsNull() || descriptor.Format() != Prospero::BufferFormat::k8UInt ||
		    descriptor.Type() != Prospero::ImageType::kColor2DArray || descriptor.MetaCompress() ||
		    descriptor.WriteCompress() || descriptor.BaseLevel() > descriptor.LastLevel() ||
		    descriptor.BaseLevel() > descriptor.MaxMip() ||
		    descriptor.BaseArray5() > descriptor.Depth() || descriptor.DstSelX() != 4)
			return false;
		const std::array extents {
		    std::max(1u, (descriptor.Width5() + 1u) >> descriptor.BaseLevel()),
		    std::max(1u, (descriptor.Height5() + 1u) >> descriptor.BaseLevel()),
		    descriptor.Depth() - descriptor.BaseArray5() + 1u};
		const std::array groups {group_x, group_y, group_z};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const uint64_t threads = input.threads_num[axis];
			// Guest image writes outside the descriptor dimensions are discarded. Only the
			// final workgroup may extend beyond the selected image view.
			if (threads == 0 || threads != fill.group_stride[axis] ||
			    groups[axis] != (extents[axis] + threads - 1) / threads ||
			    groups[axis] * threads > UINT32_MAX) return false;
		}
		const auto  binding     = ResolveTexture(resource, resources.images[0]);
		const auto& destination = binding.desc.info.data;
		if (!FillSourcesDisjoint(resources.buffers, destination)) return false;
		std::scoped_lock lock {cache.m_lock};
		const auto&      image = cache.GetImage(binding.image_id);
		const auto&      view  = binding.desc.view_info;
		if (image.backing.format != vk::Format::eD32SfloatS8Uint || image.info.samples != 1 ||
		    image.info.stencil != destination || view.base_level >= image.backing.mip_levels ||
		    view.base_layer >= image.backing.layers || view.layer_count != extents[2] ||
		    view.layer_count > image.backing.layers - view.base_layer ||
		    std::max(1u, image.info.extent.width >> view.base_level) != extents[0] ||
		    std::max(1u, image.info.extent.height >> view.base_level) != extents[1]) return false;
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eStencil, view.base_level,
		                                       1, view.base_layer, view.layer_count};
		vk::ClearValue clear {};
		clear.depthStencil = vk::ClearDepthStencilValue {0.0f, fill.value};
		cache.ClearImage(command, binding.image_id, image.backing.format, range, clear);
		return true;
	}
	ShaderBufferResource descriptor;
	uint32_t             packed_clear = 0;
	uint64_t             size         = 0;
	if (!ResolveComputeBufferFill(input, group_x, group_y, group_z, mode, descriptor, packed_clear,
	                              size)) {
		return false;
	}
	if (!cache.ClearImageFromBuffer(command, descriptor.Base48(), size, packed_clear) &&
	    !cache.TryConsumeColorMetaUniformFill(descriptor.Base48(), size, packed_clear)) {
		return false;
	}
	static std::atomic<uint32_t> logged_clears {0};
	if (logged_clears.fetch_add(1, std::memory_order_relaxed) < 32) {
		LOGF("GraphicsRenderDispatchDirect: compute image clear shader=0x%016" PRIx64
		     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " value=0x%08" PRIx32 "\n",
		     input.stage.program->shader_hash, descriptor.Base48(), size, packed_clear);
	}
	return true;
}

void RenderExecutor::DispatchDirect(uint64_t submit_id, CommandBuffer& buffer,
                                    uint32_t thread_group_x, uint32_t thread_group_y,
                                    uint32_t thread_group_z, uint32_t mode) {
	EXIT_IF(buffer.IsInvalid());
	FinishDrawCommit();
	m_context.GetCommandScheduler().PopPendingOperations();
	m_context.GetCommandScheduler().NoteGpuCommand(m_context.DiagnosticFrameNum(), true);
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), submit_id,
	                    thread_group_x, thread_group_y, thread_group_z, mode,
	                    sh_ctx.GetCs().cs_regs.data_addr);

	if (thread_group_x == 0 || thread_group_y == 0 || thread_group_z == 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: skipping zero-sized dispatch groups=%ux%ux%u "
			     "mode=0x%08" PRIx32 " shader=0x%016" PRIx64 "\n",
			     thread_group_x, thread_group_y, thread_group_z, mode,
			     sh_ctx.GetCs().cs_regs.data_addr);
		}
		return;
	}

	Common::LockGuard lock(m_context.GetMutex());
	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		LOGF("GraphicsRenderDispatchDirect: temporary: ignoring dispatch with null CS shader, "
		     "groups=%ux%ux%u mode=%u\n",
		     thread_group_x, thread_group_y, thread_group_z, mode);
		return;
	}

	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		return;
	}

	constexpr uint32_t DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS = 1u << 5u;
	constexpr uint32_t DISPATCH_INITIATOR_BASE_BITS             = 0x41u;
	constexpr uint32_t DISPATCH_INITIATOR_MODIFIER_BITS         = 0xa038u;
	constexpr uint32_t DISPATCH_INITIATOR_KNOWN_MASK =
	    DISPATCH_INITIATOR_BASE_BITS | DISPATCH_INITIATOR_MODIFIER_BITS;

	const uint32_t unknown_mode_bits = mode & ~DISPATCH_INITIATOR_KNOWN_MASK;
	if (unknown_mode_bits != 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: unknown dispatch initiator bits "
			     "mode=0x%08" PRIx32 " unknown=0x%08" PRIx32 " shader=0x%016" PRIx64
			     " groups=%ux%ux%u\n",
			     mode, unknown_mode_bits, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x,
			     thread_group_y, thread_group_z);
		}
	}

	const auto& cs_regs = sh_ctx.GetCs();
	const auto& sh_regs = ctx.GetShaderRegisters();

	ShaderComputeInputInfo input_info {};
	const bool use_thread_dimensions = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	input_info.dispatch_thread_dimensions = use_thread_dimensions;
	const auto compute_program =
	    m_context.GetPipelineCache().GetComputeProgram(cs_regs, sh_regs, input_info);
	if (use_thread_dimensions) {
		input_info.dispatch_threads_num[0]    = thread_group_x;
		input_info.dispatch_threads_num[1]    = thread_group_y;
		input_info.dispatch_threads_num[2]    = thread_group_z;
	}

	if (DebugSkipShader(input_info.stage.program->shader_hash)) {
		return;
	}
	DebugRecordWork("dispatch", input_info.stage.program->shader_hash, thread_group_x, thread_group_y,
	                thread_group_z);
	if (DebugFrameTracing()) {
		std::fprintf(stderr, "[frame] dispatch cs=0x%016llx groups=%ux%ux%u mode=0x%x threads=%ux%ux%u\n",
		             static_cast<unsigned long long>(input_info.stage.program->shader_hash),
		             thread_group_x, thread_group_y, thread_group_z, mode,
		             input_info.threads_num[0], input_info.threads_num[1],
		             input_info.threads_num[2]);
		const auto& res = *input_info.stage.resources;
		for (uint32_t i = 0; i < input_info.stage.program->info.buffers.size(); i++) {
			const auto r = DecodeNativeDescriptor<ShaderBufferResource>(res.buffers[i]);
			std::fprintf(stderr,
			             "[frame]   buf[%u] source=%u addr=0x%llx stride=%u records=%u fmt=%u "
			             "swz=%d itdx=%u addtid=%d\n",
			             i, input_info.stage.program->info.buffers[i].source,
			             static_cast<unsigned long long>(r.Base48()), r.Stride(), r.NumRecords(),
			             r.RawFormat(), r.SwizzleEnabled() ? 1 : 0, r.IndexStride(),
			             r.AddTid() ? 1 : 0);
		}
	}
	const auto& program   = *input_info.stage.program;
	const auto& resources = *input_info.stage.resources;
	if (resources.specialization_reads.empty() &&
	    (TryConsumeComputeMetaClear(input_info, buffer) ||
	     TryConsumeComputeImageClear(input_info, buffer, thread_group_x, thread_group_y,
	                                 thread_group_z, mode))) {
		ResetBindings();
		return;
	}

	const bool large_workgroup =
	    (input_info.threads_num[0] * input_info.threads_num[1] * input_info.threads_num[2] >= 512);
	const bool                   has_sampler = !program.info.samplers.empty();
	static std::atomic<uint32_t> dispatch_log_count {0};
	if ((large_workgroup || has_sampler) &&
	    dispatch_log_count.fetch_add(1, std::memory_order_relaxed) < 512) {
		const auto sampled_images = std::count_if(
		    program.info.images.begin(), program.info.images.end(), [](const auto& image) {
			    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
		    });
		const uint32_t frame_num = static_cast<uint32_t>(m_context.DiagnosticFrameNum());
		LOGF("GraphicsRenderDispatchDirect: frame=%u shader=0x%016" PRIx64
		     " groups=%ux%ux%u mode=0x%08" PRIx32 " local=%ux%ux%u "
		     "buffers=%zu textures=%zu sampled=%zu storage=%zu samplers=%zu push=%u\n",
		     frame_num, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x, thread_group_y,
		     thread_group_z, mode, input_info.threads_num[0], input_info.threads_num[1],
		     input_info.threads_num[2], program.info.buffers.size(), program.info.images.size(),
		     sampled_images, program.info.images.size() - sampled_images,
		     program.info.samplers.size(),
		     program.bindings.UsesPushData()
		         ? static_cast<uint32_t>(sizeof(ShaderRecompiler::IR::PushData))
		         : 0u);
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& buffer = program.info.buffers[i];
			const auto  r      = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			LOGF("  CS buffer[%u]: source=%u usage=%s addr=0x%012" PRIx64
			     " stride=%u records=%u format=%u\n",
			     i, buffer.source, buffer.written ? "read-write" : "read-only", r.Base48(),
			     r.Stride(), r.NumRecords(), r.RawFormat());
		}
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			const auto& image = program.info.images[i];
			const auto  r     = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
			LOGF("  CS texture[%u]: source=%u usage=%s sampled=%s addr=0x%010" PRIx64
			     " type=%u fmt=%u extent=%ux%u depth=%u levels=%u tile=%u\n",
			     i, image.source, image.written ? "read-write" : "read-only",
			     image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled
			         ? "true"
			         : "false",
			     r.Base40(), static_cast<uint32_t>(r.Type()), static_cast<uint32_t>(r.Format()),
			     static_cast<uint32_t>(r.Width5()) + 1u, static_cast<uint32_t>(r.Height5()) + 1u,
			     static_cast<uint32_t>(r.Depth()) + 1u,
			     r.Type() == Prospero::ImageType::kColor2DMsaa ||
			             r.Type() == Prospero::ImageType::kColor2DMsaaArray
			         ? 1u
			         : static_cast<uint32_t>(image.r128 ? r.LastLevel() : r.MaxMip()) + 1u,
			     static_cast<uint32_t>(r.TileMode()));
		}
		for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
			const auto r = DecodeNativeDescriptor<ShaderSamplerResource>(
			    resources.samplers[program.info.samplers[i].snapshot_index]);
			LOGF("  CS sampler[%u]: source=%u clamp=%u/%u/%u filter=%u/%u/%u mip=%u "
			     "lod=%u-%u bias=%d\n",
			     i, program.info.samplers[i].source, static_cast<uint32_t>(r.ClampX()),
			     static_cast<uint32_t>(r.ClampY()), static_cast<uint32_t>(r.ClampZ()),
			     static_cast<uint32_t>(r.XyMagFilter()), static_cast<uint32_t>(r.XyMinFilter()),
			     static_cast<uint32_t>(r.ZFilter()), static_cast<uint32_t>(r.MipFilter()),
			     static_cast<uint32_t>(r.MinLod()), static_cast<uint32_t>(r.MaxLod()),
			     static_cast<int32_t>(r.LodBias()));
		}
	}

	if (use_thread_dimensions) {
		auto groups_from_threads = [](uint32_t threads, uint32_t group_size) {
			return (threads == 0
			            ? 0u
			            : (threads + std::max(group_size, 1u) - 1u) / std::max(group_size, 1u));
		};

		const uint32_t old_x = thread_group_x;
		const uint32_t old_y = thread_group_y;
		const uint32_t old_z = thread_group_z;
		thread_group_x       = groups_from_threads(thread_group_x, cs_regs.cs_regs.num_thread_x);
		thread_group_y       = groups_from_threads(thread_group_y, cs_regs.cs_regs.num_thread_y);
		thread_group_z       = groups_from_threads(thread_group_z, cs_regs.cs_regs.num_thread_z);

		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: use-thread-dimensions %ux%ux%u / %ux%ux%u -> "
			     "groups %ux%ux%u\n",
			     old_x, old_y, old_z, std::max(cs_regs.cs_regs.num_thread_x, 1u),
			     std::max(cs_regs.cs_regs.num_thread_y, 1u),
			     std::max(cs_regs.cs_regs.num_thread_z, 1u), thread_group_x, thread_group_y,
			     thread_group_z);
		}
	}

	buffer.EndRendering();
	auto& pipeline =
	    m_context.GetPipelineCache().GetComputePipeline(input_info, compute_program);
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	FindBuffers(bindings);
	if (program.info.uses_dma) {
		m_context.PrepareBda();
	}
	RebindImages(bindings);
	RebindBuffers(bindings);

	auto              vk_buffer        = buffer.Handle();
	PreparedBindings* descriptor_stage = &bindings;
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	bool has_storage_writes = HasShaderBufferWrites(input_info.stage);
	has_storage_writes =
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& image) {
		                return image.written &&
		                       image.resource_class ==
		                           ShaderRecompiler::IR::ImageResourceClass::Storage;
	                }) ||
	    has_storage_writes;
	LogDispatchRanges(program.shader_hash, thread_group_x, thread_group_y, thread_group_z,
	                  input_info.stage);
	// Direct dispatches only (see DispatchOverlap).
	const bool overlap = DispatchOverlap();
	if (has_storage_writes && !overlap) {
		// A host fence used to serialize every dispatch. Preserve its read-before-write ordering
		// while allowing the queue to execute asynchronously.
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	const auto shader_hash = program.shader_hash;
	static const bool time_all_compute = [] {
		const char* value = std::getenv("KYTY_GPU_TIMING_ALL_COMPUTE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	const auto gpu_timer = m_context.GetCommandScheduler().ProfileActive() || time_all_compute || shader_hash == 0xea0aceac518ec52dull ||
	                       shader_hash == 0x723423f5115aedbdull
	                           ? m_context.GetCommandScheduler().StartGpuTimer(
	                                 "compute", m_context.DiagnosticFrameNum(), shader_hash)
	                           : UINT32_MAX;
	m_context.GetCommandScheduler().ProfileConsumers(true);
	ProfilePreparedBufferUses(std::span {&descriptor_stage, 1u}, "compute", submit_id);
	const uint32_t max_wg = m_context.GetGraphics().GetPhysicalDeviceProperties().limits.maxComputeWorkGroupCount[0];
	if (thread_group_x > max_wg || thread_group_y > max_wg || thread_group_z > max_wg) {
		static std::atomic<uint32_t> clamp_direct_log {0};
		if (clamp_direct_log.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("DispatchDirect: clamping workgroups %ux%ux%u to limits %u (shader 0x%016" PRIx64 ")\n",
			     thread_group_x, thread_group_y, thread_group_z, max_wg, shader_hash);
		}
		thread_group_x = std::min(thread_group_x, max_wg);
		thread_group_y = std::min(thread_group_y, max_wg);
		thread_group_z = std::min(thread_group_z, max_wg);
	}
	vk_buffer.dispatch(thread_group_x, thread_group_y, thread_group_z);
	RecordBreadcrumb(m_context, vk_buffer);
	m_context.GetCommandScheduler().ProfileWriterOperationEnd();
	m_context.GetCommandScheduler().EndGpuTimer(gpu_timer);

	// The removed host fence also ordered read-only dispatches before later writers.
	if (!overlap) {
		ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	if (VkPassTraceActive()) {
		std::vector<std::pair<ImageId, uint64_t>> outputs;
		for (size_t i = 0; i < program.info.images.size() && i < bindings.images.size(); ++i) {
			if (program.info.images[i].written) outputs.emplace_back(bindings.images[i].image_id, bindings.images[i].desc.info.data.address);
		}
		const uint32_t groups[3] = {thread_group_x, thread_group_y, thread_group_z};
		VkPassTraceReport(m_context, false, 0, program.shader_hash, groups, outputs);
	}
	ResetBindings();
}

// Precompiled compute shader to clamp indirect dispatch arguments via Buffer Device Address (BDA)
constexpr uint32_t CLAMP_INDIRECT_ARGS_BDA_SPV[413] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x00000035u, 0x00000000u, 0x00020011u, 0x00000001u, 0x00020011u,
    0x0000000bu, 0x00020011u, 0x000014e3u, 0x0009000au, 0x5f565053u, 0x5f52484bu, 0x73796870u, 0x6c616369u,
    0x6f74735fu, 0x65676172u, 0x6675625fu, 0x00726566u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu,
    0x3035342eu, 0x00000000u, 0x0003000eu, 0x000014e4u, 0x00000001u, 0x0005000fu, 0x00000005u, 0x00000004u,
    0x6e69616du, 0x00000000u, 0x00060010u, 0x00000004u, 0x00000011u, 0x00000001u, 0x00000001u, 0x00000001u,
    0x00030003u, 0x00000002u, 0x000001c2u, 0x00070004u, 0x415f4c47u, 0x675f4252u, 0x735f7570u, 0x65646168u,
    0x6e695f72u, 0x00343674u, 0x00070004u, 0x455f4c47u, 0x625f5458u, 0x65666675u, 0x65725f72u, 0x65726566u,
    0x0065636eu, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00060005u, 0x00000008u, 0x69646e49u,
    0x74636572u, 0x73677241u, 0x00000000u, 0x00040006u, 0x00000008u, 0x00000000u, 0x00000078u, 0x00040006u,
    0x00000008u, 0x00000001u, 0x00000079u, 0x00040006u, 0x00000008u, 0x00000002u, 0x0000007au, 0x00040005u,
    0x0000000au, 0x73677261u, 0x00000000u, 0x00040005u, 0x0000000cu, 0x68737550u, 0x00000000u, 0x00070006u,
    0x0000000cu, 0x00000000u, 0x73677261u, 0x6164625fu, 0x6464615fu, 0x00000072u, 0x00050006u, 0x0000000cu,
    0x00000001u, 0x5f78616du, 0x00000078u, 0x00050006u, 0x0000000cu, 0x00000002u, 0x5f78616du, 0x00000079u,
    0x00050006u, 0x0000000cu, 0x00000003u, 0x5f78616du, 0x0000007au, 0x00030005u, 0x0000000eu, 0x00006370u,
    0x00030047u, 0x00000008u, 0x00000002u, 0x00050048u, 0x00000008u, 0x00000000u, 0x00000023u, 0x00000000u,
    0x00050048u, 0x00000008u, 0x00000001u, 0x00000023u, 0x00000004u, 0x00050048u, 0x00000008u, 0x00000002u,
    0x00000023u, 0x00000008u, 0x00030047u, 0x0000000au, 0x000014ecu, 0x00030047u, 0x0000000cu, 0x00000002u,
    0x00050048u, 0x0000000cu, 0x00000000u, 0x00000023u, 0x00000000u, 0x00050048u, 0x0000000cu, 0x00000001u,
    0x00000023u, 0x00000008u, 0x00050048u, 0x0000000cu, 0x00000002u, 0x00000023u, 0x0000000cu, 0x00050048u,
    0x0000000cu, 0x00000003u, 0x00000023u, 0x00000010u, 0x00040047u, 0x00000034u, 0x0000000bu, 0x00000019u,
    0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030027u, 0x00000006u, 0x000014e5u,
    0x00040015u, 0x00000007u, 0x00000020u, 0x00000000u, 0x0005001eu, 0x00000008u, 0x00000007u, 0x00000007u,
    0x00000007u, 0x00040020u, 0x00000006u, 0x000014e5u, 0x00000008u, 0x00040020u, 0x00000009u, 0x00000007u,
    0x00000006u, 0x00040015u, 0x0000000bu, 0x00000040u, 0x00000000u, 0x0006001eu, 0x0000000cu, 0x0000000bu,
    0x00000007u, 0x00000007u, 0x00000007u, 0x00040020u, 0x0000000du, 0x00000009u, 0x0000000cu, 0x0004003bu,
    0x0000000du, 0x0000000eu, 0x00000009u, 0x00040015u, 0x0000000fu, 0x00000020u, 0x00000001u, 0x0004002bu,
    0x0000000fu, 0x00000010u, 0x00000000u, 0x00040020u, 0x00000011u, 0x00000009u, 0x0000000bu, 0x00040020u,
    0x00000017u, 0x000014e5u, 0x00000007u, 0x0004002bu, 0x0000000fu, 0x0000001au, 0x00000001u, 0x00040020u,
    0x0000001bu, 0x00000009u, 0x00000007u, 0x0004002bu, 0x0000000fu, 0x00000024u, 0x00000002u, 0x0004002bu,
    0x0000000fu, 0x0000002du, 0x00000003u, 0x00040017u, 0x00000032u, 0x00000007u, 0x00000003u, 0x0004002bu,
    0x00000007u, 0x00000033u, 0x00000001u, 0x0006002cu, 0x00000032u, 0x00000034u, 0x00000033u, 0x00000033u,
    0x00000033u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u,
    0x0004003bu, 0x00000009u, 0x0000000au, 0x00000007u, 0x00050041u, 0x00000011u, 0x00000012u, 0x0000000eu,
    0x00000010u, 0x0004003du, 0x0000000bu, 0x00000013u, 0x00000012u, 0x00040078u, 0x00000006u, 0x00000014u,
    0x00000013u, 0x0003003eu, 0x0000000au, 0x00000014u, 0x0004003du, 0x00000006u, 0x00000015u, 0x0000000au,
    0x0004003du, 0x00000006u, 0x00000016u, 0x0000000au, 0x00050041u, 0x00000017u, 0x00000018u, 0x00000016u,
    0x00000010u, 0x0006003du, 0x00000007u, 0x00000019u, 0x00000018u, 0x00000002u, 0x00000010u, 0x00050041u,
    0x0000001bu, 0x0000001cu, 0x0000000eu, 0x0000001au, 0x0004003du, 0x00000007u, 0x0000001du, 0x0000001cu,
    0x0007000cu, 0x00000007u, 0x0000001eu, 0x00000001u, 0x00000026u, 0x00000019u, 0x0000001du, 0x00050041u,
    0x00000017u, 0x0000001fu, 0x00000015u, 0x00000010u, 0x0005003eu, 0x0000001fu, 0x0000001eu, 0x00000002u,
    0x00000010u, 0x0004003du, 0x00000006u, 0x00000020u, 0x0000000au, 0x0004003du, 0x00000006u, 0x00000021u,
    0x0000000au, 0x00050041u, 0x00000017u, 0x00000022u, 0x00000021u, 0x0000001au, 0x0006003du, 0x00000007u,
    0x00000023u, 0x00000022u, 0x00000002u, 0x00000004u, 0x00050041u, 0x0000001bu, 0x00000025u, 0x0000000eu,
    0x00000024u, 0x0004003du, 0x00000007u, 0x00000026u, 0x00000025u, 0x0007000cu, 0x00000007u, 0x00000027u,
    0x00000001u, 0x00000026u, 0x00000023u, 0x00000026u, 0x00050041u, 0x00000017u, 0x00000028u, 0x00000020u,
    0x0000001au, 0x0005003eu, 0x00000028u, 0x00000027u, 0x00000002u, 0x00000004u, 0x0004003du, 0x00000006u,
    0x00000029u, 0x0000000au, 0x0004003du, 0x00000006u, 0x0000002au, 0x0000000au, 0x00050041u, 0x00000017u,
    0x0000002bu, 0x0000002au, 0x00000024u, 0x0006003du, 0x00000007u, 0x0000002cu, 0x0000002bu, 0x00000002u,
    0x00000008u, 0x00050041u, 0x0000001bu, 0x0000002eu, 0x0000000eu, 0x0000002du, 0x0004003du, 0x00000007u,
    0x0000002fu, 0x0000002eu, 0x0007000cu, 0x00000007u, 0x00000030u, 0x00000001u, 0x00000026u, 0x0000002cu,
    0x0000002fu, 0x00050041u, 0x00000017u, 0x00000031u, 0x00000029u, 0x00000024u, 0x0005003eu, 0x00000031u,
    0x00000030u, 0x00000002u, 0x00000008u, 0x000100fdu, 0x00010038u,
};

static vk::PipelineLayout g_clamp_indirect_layout = nullptr;
static vk::Pipeline       g_clamp_indirect_pipeline = nullptr;
static std::mutex         g_clamp_indirect_mutex;

static void EnsureClampIndirectPipeline(GraphicContext& graphics) {
	if (g_clamp_indirect_pipeline != nullptr) {
		return;
	}
	std::lock_guard lock(g_clamp_indirect_mutex);
	if (g_clamp_indirect_pipeline != nullptr) {
		return;
	}

	vk::PushConstantRange push_range {};
	push_range.stageFlags = vk::ShaderStageFlagBits::eCompute;
	push_range.offset     = 0;
	push_range.size       = 24;

	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push_range;
	const auto layout_res = graphics.device.createPipelineLayout(&layout_info, nullptr, &g_clamp_indirect_layout);
	RequireVulkanSuccess(layout_res, "create clamp indirect layout");

	const auto module = CompileSPV(CLAMP_INDIRECT_ARGS_BDA_SPV, graphics.device);
	vk::PipelineShaderStageCreateInfo stage_info {};
	stage_info.stage  = vk::ShaderStageFlagBits::eCompute;
	stage_info.module = module;
	stage_info.pName  = "main";

	vk::ComputePipelineCreateInfo pipe_info {};
	pipe_info.stage  = stage_info;
	pipe_info.layout = g_clamp_indirect_layout;
	const auto result = graphics.device.createComputePipelines(nullptr, 1, &pipe_info, nullptr, &g_clamp_indirect_pipeline);
	graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create clamp indirect pipeline");
}

// Host-visible copies of recent indirect dispatch arguments, readable while the GPU is hung.
static constexpr uint32_t   IndirectProbeSlots = 4096;
static std::atomic<Buffer*> g_indirect_probe {nullptr};
static std::atomic<uint32_t> g_indirect_probe_next {0};

static std::atomic<Buffer*> g_breadcrumb {nullptr};

static bool BreadcrumbsEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_BREADCRUMBS");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

bool DebugReadBreadcrumb(uint32_t* value) {
	auto* crumb = g_breadcrumb.load(std::memory_order_acquire);
	if (crumb == nullptr) {
		return false;
	}
	crumb->Invalidate(0, 4);
	std::memcpy(value, crumb->Mapped().data(), 4);
	return true;
}

void RecordBreadcrumb(RenderContext& context, vk::CommandBuffer vk_buffer) {
	if (!BreadcrumbsEnabled()) {
		return;
	}
	auto* crumb = g_breadcrumb.load(std::memory_order_acquire);
	if (crumb == nullptr) {
		crumb = new Buffer(context.GetGraphics(), context.GetCommandScheduler(), MemoryUsage::Download, 0,
		                   AllFlags, 256);
		std::memset(crumb->Mapped().data(), 0, 256);
		g_breadcrumb.store(crumb, std::memory_order_release);
	}
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {},
	                          1, &barrier, 0, nullptr, 0, nullptr);
	vk_buffer.fillBuffer(crumb->Handle(), 0, 4, static_cast<uint32_t>(DebugWorkCount()));
}

bool DebugReadIndirectProbe(uint32_t slot, uint32_t out[3]) {
	auto* probe = g_indirect_probe.load(std::memory_order_acquire);
	if (probe == nullptr || slot >= IndirectProbeSlots) {
		return false;
	}
	probe->Invalidate(slot * 16u, 16u);
	std::memcpy(out, probe->Mapped().data() + slot * 16u, 12u);
	return true;
}

void RenderExecutor::DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer,
                                      uint64_t args_addr, uint32_t mode) {
	EXIT_IF(buffer.IsInvalid() || args_addr == 0 || (args_addr & 3u) != 0 ||
	        (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0);
	FinishDrawCommit();
	m_context.GetCommandScheduler().PopPendingOperations();
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchIndirect), submit_id,
	                    static_cast<uint32_t>(args_addr), static_cast<uint32_t>(args_addr >> 32u),
	                    0, mode, buffer.GetShaders().GetCs().cs_regs.data_addr);
	Common::LockGuard lock(m_context.GetMutex());
	const auto& cs_regs = buffer.GetShaders().GetCs();
	if (cs_regs.cs_regs.data_addr == 0) {
		return;
	}
	ShaderComputeInputInfo input_info {};
	const auto compute_program = m_context.GetPipelineCache().GetComputeProgram(
	    cs_regs, buffer.GetRegisters().GetShaderRegisters(), input_info);
	if (DebugSkipShader(input_info.stage.program->shader_hash)) {
		return;
	}
	DebugRecordWork("dispatch-indirect", input_info.stage.program->shader_hash,
	                static_cast<uint32_t>(args_addr), static_cast<uint32_t>(args_addr >> 32), 0);
	if (DebugFrameTracing()) {
		uint32_t guest_args[3] = {};
		LibKernel::Memory::TryReadBacking(args_addr, guest_args, sizeof(guest_args));
		std::fprintf(stderr,
		             "[frame] dispatch-indirect cs=0x%016llx args=0x%llx guest=%ux%ux%u mode=0x%x\n",
		             static_cast<unsigned long long>(input_info.stage.program->shader_hash),
		             static_cast<unsigned long long>(args_addr), guest_args[0], guest_args[1],
		             guest_args[2], mode);
	}
	buffer.EndRendering();
	auto& pipeline = m_context.GetPipelineCache().GetComputePipeline(input_info, compute_program);
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	FindBuffers(bindings);
	const auto& program = *input_info.stage.program;
	if (program.info.uses_dma) {
		m_context.PrepareBda();
	}
	RebindImages(bindings);
	// Acquiring arguments can merge cache buffers; finalize shader bindings afterward.
	const auto [args_buffer, args_offset] = m_context.GetBufferCache().ObtainBuffer(
	    args_addr, sizeof(vk::DispatchIndirectCommand), false);
	EXIT_IF(args_buffer == nullptr || (args_offset & 3u) != 0);
	RebindBuffers(bindings);

	static const uint32_t max_indirect_groups = [] {
		const char* env = std::getenv("KYTY_MAX_INDIRECT_GROUPS");
		if (env != nullptr && env[0] != '\0') {
			return static_cast<uint32_t>(std::strtoul(env, nullptr, 0));
		}
		return 65535u;
	}();

	// Host clamp if mapped and already populated
	if (!args_buffer->Mapped().empty() &&
	    args_offset + sizeof(vk::DispatchIndirectCommand) <= args_buffer->Mapped().size()) {
		auto* cmd = reinterpret_cast<vk::DispatchIndirectCommand*>(args_buffer->Mapped().data() + args_offset);
		if (cmd->x > max_indirect_groups || cmd->y > max_indirect_groups || cmd->z > max_indirect_groups) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
				LOGF("DispatchIndirect: host clamped groups (%u, %u, %u) -> max %u at 0x%" PRIx64 "\n",
				     cmd->x, cmd->y, cmd->z, max_indirect_groups, args_addr);
			}
			cmd->x = std::min(cmd->x, max_indirect_groups);
			cmd->y = std::min(cmd->y, max_indirect_groups);
			cmd->z = std::min(cmd->z, max_indirect_groups);
			args_buffer->Flush(args_offset, sizeof(vk::DispatchIndirectCommand));
		}
	}
	PreparedBindings* descriptor_stage = &bindings;
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	const auto vk_buffer = buffer.Handle();
	const bool has_storage_writes = HasShaderBufferWrites(input_info.stage) ||
	    std::any_of(program.info.images.begin(), program.info.images.end(), [](const auto& image) {
		    return image.written && image.resource_class ==
		                                ShaderRecompiler::IR::ImageResourceClass::Storage;
	    });
	if (has_storage_writes) {
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead | vk::AccessFlagBits::eTransferRead;
	ProfilePipelineBarrier(vk_buffer, vk::PipelineStageFlagBits::eAllGraphics |
	                              vk::PipelineStageFlagBits::eComputeShader |
	                              vk::PipelineStageFlagBits::eTransfer,
	                          vk::PipelineStageFlagBits::eDrawIndirect, {},
	                          1, &barrier, 0, nullptr, 0, nullptr);

	// GPU BDA clamp for arguments written by GPU compute shaders
	static const bool gpu_bda_clamp_enabled = [] {
		const char* env = std::getenv("KYTY_GPU_BDA_CLAMP");
		return env != nullptr && env[0] == '1';
	}();
	if (gpu_bda_clamp_enabled && args_buffer->HasDeviceAddress()) {
		EnsureClampIndirectPipeline(m_context.GetGraphics());
		struct ClampPush {
			uint64_t bda_addr;
			uint32_t max_x;
			uint32_t max_y;
			uint32_t max_z;
		} push_data {
			args_buffer->BufferDeviceAddress() + args_offset,
			max_indirect_groups,
			max_indirect_groups,
			max_indirect_groups,
		};
		vk::MemoryBarrier pre_clamp {};
		pre_clamp.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
		pre_clamp.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
		vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer,
		                          vk::PipelineStageFlagBits::eComputeShader, {}, 1, &pre_clamp, 0, nullptr, 0, nullptr);
		vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, g_clamp_indirect_pipeline);
		vk_buffer.pushConstants(g_clamp_indirect_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push_data), &push_data);
		vk_buffer.dispatch(1, 1, 1);
		vk::MemoryBarrier post_clamp {};
		post_clamp.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
		post_clamp.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead | vk::AccessFlagBits::eShaderRead;
		vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
		                          vk::PipelineStageFlagBits::eDrawIndirect | vk::PipelineStageFlagBits::eComputeShader,
		                          {}, 1, &post_clamp, 0, nullptr, 0, nullptr);
	}

	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	m_context.GetCommandScheduler().ProfileConsumers(true);
	ProfilePreparedBufferUses(std::span {&descriptor_stage, 1u}, "compute", submit_id);
	const auto gpu_timer = m_context.GetCommandScheduler().StartGpuTimer("compute", m_context.DiagnosticFrameNum(), program.shader_hash);
	static const bool probe_enabled = [] {
		const char* env = std::getenv("KYTY_INDIRECT_PROBE");
		return env != nullptr && env[0] == '1';
	}();
	if (probe_enabled) {
		auto* probe = g_indirect_probe.load(std::memory_order_acquire);
		if (probe == nullptr) {
			probe = new Buffer(m_context.GetGraphics(), m_context.GetCommandScheduler(), MemoryUsage::Download, 0,
			                   AllFlags, IndirectProbeSlots * 16u);
			std::memset(probe->Mapped().data(), 0xcd, IndirectProbeSlots * 16u);
			g_indirect_probe.store(probe, std::memory_order_release);
		}
		const uint32_t probe_slot = g_indirect_probe_next.fetch_add(1) % IndirectProbeSlots;
		vk::BufferCopy copy {args_offset, probe_slot * 16u, 12u};
		vk_buffer.copyBuffer(args_buffer->Handle(), probe->Handle(), 1, &copy);
		DebugRecordWork("dispatch-indirect-args", input_info.stage.program->shader_hash, probe_slot, 0, 0);
	}
	vk_buffer.dispatchIndirect(args_buffer->Handle(), args_offset);
	RecordBreadcrumb(m_context, vk_buffer);
	m_context.GetCommandScheduler().ProfileWriterOperationEnd();
	m_context.GetCommandScheduler().EndGpuTimer(gpu_timer);
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	if (VkPassTraceActive()) {
		std::vector<std::pair<ImageId, uint64_t>> outputs;
		for (size_t i = 0; i < program.info.images.size() && i < bindings.images.size(); ++i) {
			if (program.info.images[i].written) outputs.emplace_back(bindings.images[i].image_id, bindings.images[i].desc.info.data.address);
		}
		VkPassTraceReport(m_context, false, 0, program.shader_hash, nullptr, outputs);
	}
	ResetBindings();
}

} // namespace Libs::Graphics
