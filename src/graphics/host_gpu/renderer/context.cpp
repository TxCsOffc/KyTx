#include "common/assert.h"
#include "common/common.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#if defined(__APPLE__)
#include "graphics/host_gpu/metal/metalRenderer.h"
#endif

#include <algorithm>
#include <bit>
#include <cstring>
namespace Libs::Graphics {

CommandBuffer::CommandBuffer(CommandScheduler& scheduler)
    : m_scheduler(scheduler), m_context(scheduler.Context()), m_graphics(scheduler.Graphics()) {}

bool CommandBuffer::IsInvalid() const {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return false;
	}
#endif
	return m_buffer == nullptr;
}

namespace {
const char* HandleProbePath() {
	static const char* path = std::getenv("KYTY_HANDLE_SITE_PROBE");
	return path;
}
void NoteHandleSite(const std::source_location& site) {
	if (HandleProbePath() == nullptr) {
		return;
	}
	struct Entry { const char* file; uint32_t line; uint64_t count; };
	static Entry    entries[256] {};
	static uint32_t used  = 0;
	static uint64_t total = 0;
	static uint64_t next  = 0;
	const auto line = static_cast<uint32_t>(site.line());
	uint32_t i = 0;
	for (; i < used; i++) {
		if (entries[i].line == line && entries[i].file == site.file_name()) break;
	}
	if (i == used && used < 256) { entries[used++] = {site.file_name(), line, 0}; }
	if (i < used) { entries[i].count++; }
	++total;
	if (total < next) {
		return;
	}
	next = total + 200000;
	if (FILE* f = std::fopen(HandleProbePath(), "w")) {
		std::fprintf(f, "total %llu\nsites %u\n", (unsigned long long)total, used);
		for (uint32_t k = 0; k < used; k++) {
			std::fprintf(f, "%llu %s:%u\n", (unsigned long long)entries[k].count,
			             entries[k].file, entries[k].line);
		}
		std::fclose(f);
	}
}
} // namespace

vk::CommandBuffer CommandBuffer::Handle(std::source_location site) const {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return m_buffer;
	}
#endif
	NoteHandleSite(site);
	if (DrawCommitEnabled() && !DrawCommitOnWorker()) {
		m_context.GetRenderExecutor().FinishDrawCommit();
	}
	EXIT_IF(IsInvalid());
	// Any recording into this buffer must come through here, so treat handing out the handle as
	// "something may have been recorded" for global-barrier coalescing.
	m_recorded_since_barrier.store(true, std::memory_order_relaxed);
	return m_buffer;
}

void CommandBuffer::Begin() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	if (DrawCommitEnabled() && !DrawCommitOnWorker()) {
		m_context.GetRenderExecutor().FinishDrawCommit();
	}
	// Begin() starts a newly allocated Vulkan buffer. A commit thread can leave the
	// wrapper's render-pass flag set after the previous buffer was already retired.
	if (m_rendering) {
		if (m_render_gpu_timer != UINT32_MAX) {
			m_scheduler.EndGpuTimer(m_render_gpu_timer);
			m_render_gpu_timer = UINT32_MAX;
		}
		m_rendering    = false;
		m_render_state = {};
	}
	EXIT_IF(IsInvalid());
	EXIT_IF(m_render_gpu_timer != UINT32_MAX || m_gap_gpu_timer != UINT32_MAX);
	m_gap_ordinal = 0;
	auto buffer = Handle();

	vk::CommandBufferBeginInfo begin_info {};
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

	auto result = buffer.begin(&begin_info);

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::End() const {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	EndRendering();
	EndGpuGap();
	auto buffer = Handle();

	auto result = buffer.end();

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::BeginGpuGap() const {
	if (m_gap_gpu_timer != UINT32_MAX || m_rendering) return;
	auto& scheduler = m_scheduler;
	m_gap_gpu_timer = scheduler.StartGpuTimer("gap", scheduler.GpuFrameHint(), m_gap_ordinal++);
}

void CommandBuffer::EndGpuGap() const {
	if (m_gap_gpu_timer == UINT32_MAX) return;
	m_scheduler.EndGpuTimer(m_gap_gpu_timer);
	m_gap_gpu_timer = UINT32_MAX;
}

void CommandBuffer::SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0, uint32_t arg1,
                                 uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	m_debug_op        = op;
	m_debug_submit_id = submit_id;
	m_debug_arg0      = arg0;
	m_debug_arg1      = arg1;
	m_debug_arg2      = arg2;
	m_debug_arg3      = arg3;
	m_debug_arg4      = arg4;
}

void CommandBuffer::BeginRendering(const RenderState& state) const {
	if (m_rendering && m_render_state == state) {
		return;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.num_color_attachments > RENDER_COLOR_ATTACHMENTS_MAX);
	EndRendering();
	EndGpuGap();
	auto& scheduler = m_scheduler;
	// The current draw was counted when its guest command began.
	m_render_draw_start = scheduler.CurrentSubmitDrawCount() > 0
	                          ? scheduler.CurrentSubmitDrawCount() - 1 : 0;
	const auto dimensions = (static_cast<uint64_t>(state.width) << 32u) | state.height;
	m_render_gpu_timer = scheduler.StartGpuTimer("render", scheduler.GpuFrameHint(), dimensions);

	std::array<vk::RenderingAttachmentInfo, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
	for (uint32_t i = 0; i < state.num_color_attachments; i++) {
		const auto& attachment = state.color_attachments[i];
		colors[i].imageView    = attachment.image_view;
		colors[i].imageLayout  = attachment.image_layout;
		colors[i].loadOp =
		    attachment.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
		colors[i].storeOp                 = vk::AttachmentStoreOp::eStore;
		colors[i].clearValue.color.uint32 = attachment.clear_value;
	}

	const auto&                 depth_stencil = state.depth_stencil_attachment;
	vk::RenderingAttachmentInfo depth {};
	depth.imageView   = depth_stencil.image_view;
	depth.imageLayout = depth_stencil.image_layout;
	depth.loadOp =
	    depth_stencil.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	depth.storeOp                       = vk::AttachmentStoreOp::eStore;
	depth.clearValue.depthStencil.depth = std::bit_cast<float>(depth_stencil.clear_value[0]);

	vk::RenderingAttachmentInfo stencil {};
	stencil.imageView   = depth_stencil.image_view;
	stencil.imageLayout = depth_stencil.image_layout;
	stencil.loadOp =
	    depth_stencil.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	stencil.storeOp                         = vk::AttachmentStoreOp::eStore;
	stencil.clearValue.depthStencil.stencil = depth_stencil.clear_value[1];

	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = {state.width, state.height};
	rendering.layerCount           = state.num_layers;
	rendering.colorAttachmentCount = state.num_color_attachments;
	rendering.pColorAttachments    = colors.data();
	rendering.pDepthAttachment     = depth_stencil.has_depth ? &depth : nullptr;
	rendering.pStencilAttachment   = depth_stencil.has_stencil ? &stencil : nullptr;
	Handle().beginRendering(rendering);
	m_render_state = state;
	m_rendering    = true;
}

void CommandBuffer::EndRendering() const {
	if (!m_rendering) {
		return;
	}
	Handle().endRendering();
	m_rendering    = false;
	m_render_state = {};
	auto& scheduler = m_scheduler;
	scheduler.SetGpuTimerDraws(m_render_gpu_timer,
	                           scheduler.CurrentSubmitDrawCount() - m_render_draw_start);
	scheduler.EndGpuTimer(m_render_gpu_timer);
	m_render_gpu_timer = UINT32_MAX;
	BeginGpuGap();
}

} // namespace Libs::Graphics
