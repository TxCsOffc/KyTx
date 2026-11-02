#include "graphics/host_gpu/renderer/renderDraw.h"

#include "common/assert.h"
#include "common/common.h"
#include "graphics/presentation/traceWindow.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/BufferFormat.h"
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
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

// ---- Vulkan reference pass trace: the oracle for the native Metal backend ----
// KYTY_VK_PASS_TRACE_TRIGGER=<file>: when the file appears (consumed once), every draw and dispatch of the next KYTY_PASS_TRACE_FLIPS
// presented frames (default 1) is followed by a wait and a readback of its colour targets / written storage images, printed in the
// Metal backend's [PassTrace] format (Metal pixel-format numbers) so the two backends can be compared pass by pass on the same scene.
bool VkPassTraceActive() {
	static const char* trigger = std::getenv("KYTY_VK_PASS_TRACE_TRIGGER");
	if (trigger == nullptr || *trigger == '\0') return false;
	static const uint64_t span = [] {
		const char* value = std::getenv("KYTY_PASS_TRACE_FLIPS");
		return std::clamp<uint64_t>(value != nullptr ? std::strtoull(value, nullptr, 10) : 1u, 1u, 16u);
	}();
	static uint64_t first = ~0ull;
	const uint64_t  flip  = TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (first != ~0ull && flip >= first && flip - first < span) return true;
	if (first != ~0ull) {
		std::fprintf(stderr, "[PassTrace] end frame flip=%llu\n", static_cast<unsigned long long>(first));
		first = ~0ull;
	}
	if (std::remove(trigger) == 0) {
		first = flip;
		std::fprintf(stderr, "[PassTrace] begin frame flip=%llu (vulkan)\n", static_cast<unsigned long long>(flip));
		return true;
	}
	return false;
}

namespace {

float VkTraceHalf(uint16_t h) {
	const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1fu, man = h & 0x3ffu;
	float          v    = 0.0f;
	if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
	else if (exp == 31) v = man != 0 ? std::nanf("") : INFINITY;
	else v = std::ldexp(static_cast<float>(man | 0x400u), static_cast<int>(exp) - 25);
	return sign != 0 ? -v : v;
}

float VkTraceSmallFloat(uint32_t bits, uint32_t man_bits) {
	const uint32_t exp = bits >> man_bits, man = bits & ((1u << man_bits) - 1u);
	if (exp == 31) return man != 0 ? std::nanf("") : INFINITY;
	if (exp == 0) return std::ldexp(static_cast<float>(man), -14 - static_cast<int>(man_bits));
	return std::ldexp(1.0f + static_cast<float>(man) / static_cast<float>(1u << man_bits), static_cast<int>(exp) - 15);
}

// Metal pixel-format number and bytes per pixel of a Vulkan colour format (0 when not traced).
std::pair<int, uint32_t> VkTraceFormat(vk::Format format) {
	switch (format) {
		case vk::Format::eB10G11R11UfloatPack32: return {92, 4};
		case vk::Format::eR16G16B16A16Sfloat: return {115, 8};
		case vk::Format::eR16G16B16A16Unorm: return {110, 8};
		case vk::Format::eR16G16B16A16Uint: return {113, 8};
		case vk::Format::eR16G16Sfloat: return {65, 4};
		case vk::Format::eR16Sfloat: return {25, 2};
		case vk::Format::eR32Sfloat: return {55, 4};
		case vk::Format::eR32Uint: return {53, 4};
		case vk::Format::eR32G32Sfloat: return {105, 8};
		case vk::Format::eR32G32B32A32Sfloat: return {125, 16};
		case vk::Format::eR8G8B8A8Unorm: return {70, 4};
		case vk::Format::eR8G8B8A8Srgb: return {71, 4};
		case vk::Format::eB8G8R8A8Unorm: return {80, 4};
		case vk::Format::eB8G8R8A8Srgb: return {81, 4};
		case vk::Format::eA2B10G10R10UnormPack32: return {90, 4};
		case vk::Format::eR8Unorm: return {10, 1};
		case vk::Format::eR8Uint: return {13, 1};
		case vk::Format::eR8G8Unorm: return {30, 2};
		default: return {0, 0};
	}
}

// Read level 0 / layer 0 of a cache image back to the host (waits for the GPU) and describe it like the Metal PassTraceStats.
std::string VkTraceImageStats(RenderContext& context, ImageId id) {
	auto& cache = context.GetTextureCache();
	auto& image = cache.GetImage(id);
	const auto [metal_format, bpp] = VkTraceFormat(image.backing.format);
	const uint32_t w = image.backing.extent.width, h = image.backing.extent.height;
	char           head[96];
	std::snprintf(head, sizeof(head), "%ux%u f%d", w, h, metal_format != 0 ? metal_format : static_cast<int>(image.backing.format) + 1000);
	if (bpp == 0 || w == 0 || h == 0 || image.backing.image == nullptr || image.backing.image_type != vk::ImageType::e2D ||
	    static_cast<uint64_t>(w) * h * bpp > 32ull * 1024 * 1024) {
		return std::string(head) + " n/a";
	}
	const uint64_t size     = static_cast<uint64_t>(w) * h * bpp;
	auto&          download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset]   = download.Map(size, 256);
	if (mapped == nullptr) return std::string(head) + " n/a(map)";
	download.Commit();
	vk::BufferImageCopy copy {};
	copy.bufferOffset      = offset;
	copy.imageSubresource  = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.imageExtent       = vk::Extent3D {w, h, 1};
	image.Download(std::span {&copy, 1u}, download.Handle(), offset, size);
	context.GetCommandScheduler().FlushAndWait();
	download.Invalidate(offset, size);
	double   vmin = 1e30, vmax = -1e30;
	uint64_t nan = 0, inf = 0;
	uint32_t bmin = 255, bmax = 0;
	bool     is_float = true;
	const auto stat = [&](float v) {
		if (std::isnan(v)) { ++nan; return; }
		if (std::isinf(v)) { ++inf; return; }
		vmin = std::min<double>(vmin, v);
		vmax = std::max<double>(vmax, v);
	};
	for (uint64_t p = 0; p < static_cast<uint64_t>(w) * h; ++p) {
		const uint8_t* px = mapped + p * bpp;
		switch (metal_format) {
			case 92: {
				uint32_t v;
				std::memcpy(&v, px, 4);
				stat(VkTraceSmallFloat(v & 0x7ffu, 6));
				stat(VkTraceSmallFloat((v >> 11) & 0x7ffu, 6));
				stat(VkTraceSmallFloat((v >> 22) & 0x3ffu, 5));
				break;
			}
			case 115:
				for (int k = 0; k < 4; ++k) { uint16_t hv; std::memcpy(&hv, px + 2 * k, 2); stat(VkTraceHalf(hv)); }
				break;
			case 65:
				for (int k = 0; k < 2; ++k) { uint16_t hv; std::memcpy(&hv, px + 2 * k, 2); stat(VkTraceHalf(hv)); }
				break;
			case 25: { uint16_t hv; std::memcpy(&hv, px, 2); stat(VkTraceHalf(hv)); break; }
			case 55: { float f; std::memcpy(&f, px, 4); stat(f); break; }
			case 105: for (int k = 0; k < 2; ++k) { float f; std::memcpy(&f, px + 4 * k, 4); stat(f); } break;
			case 125: for (int k = 0; k < 4; ++k) { float f; std::memcpy(&f, px + 4 * k, 4); stat(f); } break;
			default:
				is_float = false;
				for (uint32_t k = 0; k < bpp; ++k) { bmin = std::min<uint32_t>(bmin, px[k]); bmax = std::max<uint32_t>(bmax, px[k]); }
				break;
		}
	}
	char tail[160];
	if (is_float) {
		std::snprintf(tail, sizeof(tail), " nan=%llu inf=%llu min=%g max=%g", static_cast<unsigned long long>(nan),
		              static_cast<unsigned long long>(inf), vmin > vmax ? 0.0 : vmin, vmin > vmax ? 0.0 : vmax);
	} else {
		std::snprintf(tail, sizeof(tail), " bytes min=%u max=%u", bmin, bmax);
	}
	return std::string(head) + tail;
}

} // namespace

// One [PassTrace] line for a traced draw (outputs = colour targets) or dispatch (outputs = written storage images).
void VkPassTraceReport(RenderContext& context, bool draw, uint64_t vs_hash, uint64_t ps_or_cs_hash, const uint32_t* groups,
                       const std::vector<std::pair<ImageId, uint64_t>>& outputs) {
	static uint64_t draw_n = 0, cs_n = 0;
	std::string     line;
	char            head[200];
	if (draw) {
		std::snprintf(head, sizeof(head), "[PassTrace] draw n=%llu vs=0x%016llx ps=0x%016llx", static_cast<unsigned long long>(draw_n++),
		              static_cast<unsigned long long>(vs_hash), static_cast<unsigned long long>(ps_or_cs_hash));
	} else {
		std::snprintf(head, sizeof(head), "[PassTrace] n=%llu cs=0x%016llx grp=%ux%ux%u", static_cast<unsigned long long>(cs_n++),
		              static_cast<unsigned long long>(ps_or_cs_hash), groups != nullptr ? groups[0] : 0u, groups != nullptr ? groups[1] : 0u,
		              groups != nullptr ? groups[2] : 0u);
	}
	line = head;
	uint32_t slot = 0;
	for (const auto& [id, addr]: outputs) {
		char tag[64];
		if (draw) std::snprintf(tag, sizeof(tag), " | RT%u 0x%llx ", slot, static_cast<unsigned long long>(addr));
		else std::snprintf(tag, sizeof(tag), " | OUT r%u 0x%llx ", slot, static_cast<unsigned long long>(addr));
		line += tag;
		line += id ? VkTraceImageStats(context, id) : std::string("none");
		++slot;
	}
	std::fprintf(stderr, "%s\n", line.c_str());
}

namespace {

thread_local bool g_draw_commit_worker = false;

class DrawCommitGate {
public:
	static DrawCommitGate& Get() {
		static DrawCommitGate gate;
		return gate;
	}

	[[nodiscard]] bool Enabled() const noexcept { return m_enabled; }
	[[nodiscard]] bool OnWorker() const noexcept { return g_draw_commit_worker; }

	void Enqueue(std::function<void()> job) {
		std::lock_guard lock(m_mutex);
		m_job = std::move(job);
		m_busy = true;
		m_queued++;
		m_cv.notify_one();
	}

	void Wait(int frame) {
		std::unique_lock lock(m_mutex);
		if (m_busy) {
			const auto begin = std::chrono::steady_clock::now();
			m_cv.wait(lock, [&] { return !m_busy; });
			m_wait_ns += static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(
			        std::chrono::steady_clock::now() - begin)
			        .count());
		}
		PublishLocked(frame);
	}

private:
	DrawCommitGate() {
		const char* flag = std::getenv("KYTY_DRAW_COMMIT_THREAD");
		m_enabled        = flag != nullptr && std::strcmp(flag, "1") == 0;
		if (!m_enabled) {
			return;
		}
		const char* path = std::getenv("KYTY_DRAW_COMMIT_TRACE");
		if (path == nullptr || path[0] == '\0') {
			path = "D:\\PS5\\ufc5-draw-commit.csv";
		}
		m_file = std::fopen(path, "w");
		if (m_file != nullptr) {
			std::fprintf(m_file, "frame,queued,commit_ms,wait_ms\n");
			std::fflush(m_file);
		}
		m_thread = std::jthread([this](std::stop_token stop) { Loop(stop); });
	}

	~DrawCommitGate() {
		{
			std::lock_guard lock(m_mutex);
			m_stop = true;
			m_cv.notify_one();
		}
		if (m_thread.joinable()) {
			m_thread.request_stop();
			m_thread.join();
		}
		if (m_file != nullptr) {
			WriteLocked();
			std::fclose(m_file);
		}
	}

	void Loop(std::stop_token stop) {
		g_draw_commit_worker = true;
		for (;;) {
			std::function<void()> job;
			{
				std::unique_lock lock(m_mutex);
				m_cv.wait(lock, [&] { return m_stop || stop.stop_requested() || m_busy; });
				if ((m_stop || stop.stop_requested()) && !m_busy) {
					return;
				}
				job = std::move(m_job);
			}
			const auto begin = std::chrono::steady_clock::now();
			if (job) {
				job();
			}
			const auto elapsed = static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(
			        std::chrono::steady_clock::now() - begin)
			        .count());
			{
				std::lock_guard lock(m_mutex);
				m_commit_ns += elapsed;
				m_busy = false;
				m_job  = nullptr;
				m_cv.notify_all();
			}
		}
	}

	void PublishLocked(int frame) {
		if (m_frame < 0) {
			m_frame = frame;
			return;
		}
		if (frame == m_frame) {
			return;
		}
		WriteLocked();
		m_frame     = frame;
		m_queued    = 0;
		m_commit_ns = 0;
		m_wait_ns   = 0;
	}

	void WriteLocked() {
		if (m_file == nullptr || m_frame < 0) {
			return;
		}
		std::fprintf(m_file, "%d,%u,%.3f,%.3f\n", m_frame, m_queued, m_commit_ns / 1.0e6,
		             m_wait_ns / 1.0e6);
		std::fflush(m_file);
	}

	bool                      m_enabled = false;
	FILE*                     m_file    = nullptr;
	std::mutex                m_mutex;
	std::condition_variable   m_cv;
	std::function<void()>     m_job;
	bool                      m_busy  = false;
	bool                      m_stop  = false;
	std::jthread              m_thread;
	int                       m_frame     = -1;
	uint32_t                  m_queued    = 0;
	uint64_t                  m_commit_ns = 0;
	uint64_t                  m_wait_ns   = 0;
};

} // namespace

static bool BreadcrumbsOn() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_BREADCRUMBS");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

bool DrawCommitEnabled() noexcept {
	return DrawCommitGate::Get().Enabled();
}

bool DrawCommitOnWorker() noexcept {
	return g_draw_commit_worker;
}

// KYTY_BIND_TRACE=1: per-stage BIND line in the same field layout as the Metal backend.
static void TraceBindVk(char stage, const PreparedBindings& pb) {
	static const bool s_on = std::getenv("KYTY_BIND_TRACE") != nullptr;
	if (!s_on || !TraceWindow::Active()) return;
	// One draw index per draw: the vertex stage advances it, the pixel stage reuses it.
	const uint64_t draw_index = stage == 'V' ? TraceWindow::NextDraw()
	                                         : TraceWindow::g_draw_in_frame.load() - 1;
	const uint64_t hash = (pb.runtime && pb.runtime->program) ? pb.runtime->program->shader_hash : 0;
	std::string ud;
	for (size_t i = 0; i < 16 && i < pb.shader_data.size(); ++i) {
		char d[16];
		std::snprintf(d, sizeof(d), "%s%08x", i ? "," : "", pb.shader_data[i]);
		ud += d;
	}
	std::string bufs;
	for (const auto& src: pb.buffer_sources) {
		char b[64];
		std::snprintf(b, sizeof(b), " %c:buf@%llx:%llu", stage, (unsigned long long)src.address,
		              (unsigned long long)src.size);
		bufs += b;
	}
	std::string imgs;
	for (const auto& tex: pb.images) {
		char b[96];
		std::snprintf(b, sizeof(b), " %c:img@%llx:%ux%u", stage,
		              (unsigned long long)tex.desc.info.data.address, tex.desc.info.extent.width,
		              tex.desc.info.extent.height);
		imgs += b;
	}
	std::fprintf(stderr, "BINDVK seq=%llu %c=%016llx ud=[%s] %s%s\n", (unsigned long long)draw_index,
	             stage, (unsigned long long)hash, ud.c_str(), bufs.c_str(), imgs.c_str());
}

void RenderExecutor::FinishDrawCommit() {
	if (!DrawCommitEnabled()) {
		return;
	}
	ProfileCpuScope profile(m_context.GetCommandScheduler(), "draw_commit_wait");
	DrawCommitGate::Get().Wait(m_context.DiagnosticFrameNum());
	if (m_commit_reset_bindings) {
		ResetBindings();
		m_commit_reset_bindings = false;
	}
}

// The phase totals are opt-in and intentionally aggregate thousands of draws per frame.
class CpuDrawPhaseTimer {
public:
	CpuDrawPhaseTimer(CommandScheduler& scheduler, CommandScheduler::CpuDrawPhase phase)
	    : m_scheduler(scheduler), m_phase(phase), m_enabled(scheduler.CpuDrawTimingEnabled()) {
		if (m_enabled) m_begin = std::chrono::steady_clock::now();
	}
	~CpuDrawPhaseTimer() { Record(); }
	CpuDrawPhaseTimer(const CpuDrawPhaseTimer&) = delete;
	CpuDrawPhaseTimer& operator=(const CpuDrawPhaseTimer&) = delete;

	void Next(CommandScheduler::CpuDrawPhase phase) {
		Record();
		m_phase = phase;
		if (m_enabled) m_begin = std::chrono::steady_clock::now();
	}

private:
	void Record() {
		if (!m_enabled) return;
		const auto elapsed = std::chrono::duration<double, std::milli>(
		    std::chrono::steady_clock::now() - m_begin).count();
		m_scheduler.NoteCpuDrawPhase(m_phase, elapsed);
		static constexpr const char* names[] {"draw_state_cpu", "draw_bindings_cpu", "draw_vertex_index_cpu", "draw_pipeline_cpu", "draw_targets_cpu", "draw_commit_cpu"};
		if (m_scheduler.ProfileDetailedCpu()) m_scheduler.ProfileEvent(names[static_cast<size_t>(m_phase)],
		    std::chrono::duration_cast<std::chrono::nanoseconds>(m_begin.time_since_epoch()).count(),
		    ProfileClockNs(), 0, 0, std::source_location::current());
	}
	CommandScheduler& m_scheduler;
	CommandScheduler::CpuDrawPhase m_phase;
	bool m_enabled;
	std::chrono::steady_clock::time_point m_begin {};
};

std::pair<int32_t, uint32_t> ResolveDrawOffsets(uint32_t index_offset,
	                                           const ShaderVertexInputInfo& vs_input_info) {
	auto     vertex_offset   = static_cast<int32_t>(index_offset);
	uint32_t instance_offset = 0;
	if (!vs_input_info.fetch_embedded) {
		return {vertex_offset, instance_offset};
	}

	EXIT_IF(!vs_input_info.stage);
	const auto& program   = *vs_input_info.stage.program;
	const auto& resources = *vs_input_info.stage.resources;
	if (index_offset == 0 &&
	    program.info.vertex_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.vertex_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			vertex_offset = static_cast<int32_t>(resources.user_data[index]);
		}
	}
	if (program.info.instance_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.instance_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			instance_offset = resources.user_data[index];
		}
	}

	return {vertex_offset, instance_offset};
}

static std::atomic<uint32_t> g_draw_state_log_count   = 0;
static std::atomic<uint32_t> g_draw_input_log_count   = 0;
static std::atomic<uint32_t> g_mrt_state_log_count    = 0;

static std::atomic<uint32_t> g_framebuffer_skip_log_count = 0;

static float ConvertPolygonOffsetConstantFactor(float guest_factor, const HW::PolyOffset& offset,
                                                vk::Format host_depth_format) {
	if (offset.db_is_float_fmt) {
		return guest_factor;
	}

	int host_depth_bits = 0;
	switch (host_depth_format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint: host_depth_bits = 16; break;
		case vk::Format::eD24UnormS8Uint: host_depth_bits = 24; break;
		default:
			// A fixed-point guest bias cannot be represented exactly by a floating-point host
			// attachment without VK_EXT_depth_bias_control.
			return guest_factor;
	}
	return std::ldexp(guest_factor, host_depth_bits + offset.neg_num_db_bits);
}

static const char* RenderColorTypeName(const RenderColorInfo& color) {
	return color.image_id ? "RenderTexture" : "NoColorOutput";
}

static void LogFramebufferSkip(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               uint32_t index_count, uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	auto log_id = g_framebuffer_skip_log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 128) {
		return;
	}

	LOGF(
	    "DrawFramebufferSkip[%u]: %s color=%s color_addr=0x%010" PRIx64 " color_size=0x%016" PRIx64
	    " color_image=%s depth_format=%s depth_image=%s depth_vaddr_num=%d target_mask=0x%08" PRIx32
	    " prim=%u index_count=%u flags=0x%08" PRIx32 "\n",
	    log_id, draw_name, RenderColorTypeName(color), color.desc.info.data.address,
	    color.desc.info.data.size, color.image_id ? "yes" : "no",
	    vk::to_string(depth.desc.view_info.format).c_str(), depth.image_id ? "yes" : "no",
	    static_cast<int>(!depth.desc.info.data.Empty()) +
	        static_cast<int>(depth.desc.info.HasStencil()),
	    ctx.GetRenderTargetMask(), static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags);
}

static void LogMrtState(const char* draw_name, const CommandBuffer& buffer,
                        const ShaderPixelInputInfo& ps_input_info) {
	const auto& ctx            = buffer.GetRegisters();
	const auto& sh_regs        = ctx.GetShaderRegisters();
	const auto  rt_mask        = ctx.GetRenderTargetMask();
	const auto  cb_shader_mask = sh_regs.m_cbShaderMask;
	const auto& bc0            = ctx.GetBlendControl(0);

	auto log_id = g_mrt_state_log_count.fetch_add(1);
	if (log_id >= 32) {
		return;
	}

	LOGF("MrtState[%u]: %s rt_mask=0x%08" PRIx32 " cb_shader_mask=0x%08" PRIx32
	     " blend0=%s src=%u dst=%u alpha_src=%u alpha_dst=%u sep_alpha=%s\n",
	     log_id, draw_name, rt_mask, cb_shader_mask, bc0.enable ? "true" : "false",
	     bc0.color_srcblend, bc0.color_destblend, bc0.alpha_srcblend, bc0.alpha_destblend,
	     bc0.separate_alpha_blend ? "true" : "false");

	for (uint32_t i = 0; i < 8; i++) {
		const auto& rt  = ctx.GetRenderTarget(i);
		const auto& bc  = ctx.GetBlendControl(i);
		const auto  ctm = (rt_mask >> (i * 4u)) & 0x0fu;
		const auto  csm = (cb_shader_mask >> (i * 4u)) & 0x0fu;

		if (rt.base.addr == 0 && ps_input_info.target_output_mode[i] == 0 && ctm == 0 && csm == 0 &&
		    !bc.enable) {
			continue;
		}

		LOGF("MrtState[%u]: slot=%u addr=0x%010" PRIx64
		     " target_mask=0x%x shader_mask=0x%x out_mode=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32
		     " width=%u height=%u tile=%u"
		     " blend=%s src=%u dst=%u alpha_src=%u alpha_dst=%u\n",
		     log_id, i, rt.base.addr, ctm, csm, ps_input_info.target_output_mode[i],
		     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
		     static_cast<uint32_t>(rt.info.channel_order), rt.attrib2.width + 1,
		     rt.attrib2.height + 1, static_cast<uint32_t>(rt.attrib3.tile_mode),
		     bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.alpha_srcblend,
		     bc.alpha_destblend);
	}
}

static void LogDrawTargetState(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               const ShaderPixelInputInfo& ps_input_info, uint32_t index_count,
                               uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!color.image_id) {
		return;
	}

	auto log_id = g_draw_state_log_count.fetch_add(1);
	if (log_id >= 192) {
		return;
	}

	const auto& cc             = ctx.GetColorControl();
	const auto& bc             = ctx.GetBlendControl(color.target_slot);
	const auto& dc             = ctx.GetDepthControl();
	const auto& vp             = ctx.GetScreenViewport();
	const auto& vp0            = vp.viewports[0];
	const auto& ps_resources   = ps_input_info.stage.program->info;
	const auto  sampled_images = std::count_if(
	    ps_resources.images.begin(), ps_resources.images.end(), [](const auto& image) {
		    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
	    });

	const auto extent = color.Extent();
	const auto sc     = calc_final_scissor(vp, ctx.GetScanModeControl(), extent, 0);

	LOGF(
	    "DrawTargetState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64
	    " extent=%ux%u prim=%u index_count=%u flags=0x%08" PRIx32 " color_mask=0x%08" PRIx32
	    " cc_mode=%u cc_op=0x%02x"
	    " blend=%s src=%u dst=%u comb=%u ps_tex=%d sampled=%d storage=%d ps_kill=%s target_mode0=%u"
	    " depth_test=%s depth_write=%s depth_func=%u depth_clear=%s viewport=(%.1f,%.1f %.1fx%.1f) "
	    "scissor=(%d,%d)-(%d,%d)\n",
	    log_id, buffer.GetContext().DiagnosticFrameNum(), draw_name, RenderColorTypeName(color),
	    color.desc.info.data.address, extent.width, extent.height,
	    static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags, ctx.GetRenderTargetMask(),
	    cc.mode, cc.op,
	    bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.color_comb_fcn,
	    static_cast<int>(ps_resources.images.size()), static_cast<int>(sampled_images),
	    static_cast<int>(ps_resources.images.size() - sampled_images),
	    ps_input_info.ps_pixel_kill_enable ? "true" : "false", ps_input_info.target_output_mode[0],
	    dc.z_enable ? "true" : "false", dc.z_write_enable ? "true" : "false", dc.zfunc,
	    depth.depth_clear_enable ? "true" : "false", vp0.xoffset - vp0.xscale,
	    vp0.yoffset - vp0.yscale, vp0.xscale * 2.0f, vp0.yscale * 2.0f, sc.left, sc.top, sc.right,
	    sc.bottom);

	LogMrtState(draw_name, buffer, ps_input_info);
}

static void LogDrawInputState(const CommandBuffer& buffer, const RenderColorInfo& color,
                              const ShaderVertexInputInfo& vs_input_info,
                              uint32_t index_type_and_size, uint32_t index_count,
                              const void* index_addr) {
	auto log_id = g_draw_input_log_count.fetch_add(1);
	if (log_id >= 64) {
		return;
	}

	LOGF("DrawInputState[%u]: frame=%d target=%s addr=0x%010" PRIx64
	     " index_type=%u index_count=%u index_addr=0x%016" PRIx64
	     " vs_resources=%d vs_buffers=%d\n",
	     log_id, buffer.GetContext().DiagnosticFrameNum(), RenderColorTypeName(color),
	     color.desc.info.data.address, index_type_and_size, index_count,
	     reinterpret_cast<uint64_t>(index_addr), vs_input_info.resources_num,
	     vs_input_info.buffers_num);

	for (int bi = 0; bi < vs_input_info.buffers_num; bi++) {
		const auto& b = vs_input_info.buffers[bi];
		LOGF("DrawInputState[%u]: vb[%d] addr=0x%010" PRIx64
		     " stride=%u records=%u fetch_index=%u\n",
		     log_id, bi, b.addr, b.stride, b.num_records, b.fetch_index);

		const auto* bytes = reinterpret_cast<const uint8_t*>(b.addr);
		if (bytes != nullptr && b.stride != 0) {
			const uint32_t records = std::min<uint32_t>(b.num_records, 4u);
			for (uint32_t rec = 0; rec < records; rec++) {
				const auto* rec_bytes = bytes + static_cast<uint64_t>(rec) * b.stride;
				const auto  dword_num = std::min<uint32_t>(b.stride / 4u, 12u);
				uint32_t    raw[12]   = {};
				float       flt[12]   = {};
				for (uint32_t i = 0; i < dword_num; i++) {
					std::memcpy(&raw[i], rec_bytes + i * 4u, sizeof(raw[i]));
					std::memcpy(&flt[i], rec_bytes + i * 4u, sizeof(flt[i]));
				}
				LOGF("DrawInputState[%u]: vb[%d].rec[%u] stride=%u dwords=%u raw=%08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " f=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f)\n",
				     log_id, bi, rec, b.stride, dword_num, raw[0], raw[1], raw[2], raw[3], raw[4],
				     raw[5], raw[6], raw[7], raw[8], flt[0], flt[1], flt[2], flt[3], flt[4], flt[5],
				     flt[6], flt[7], flt[8]);

				for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
					const auto& r  = vs_input_info.resources[ai];
					const auto& rd = vs_input_info.resources_dst[ai];
					if (rd.buffer_index != bi) {
						continue;
					}
					const auto offset = static_cast<uint32_t>(r.Base48() - b.addr);
					if (offset + 4u <= b.stride &&
					    r.Format() == Prospero::BufferFormat::k8_8_8_8UNorm) {
						uint32_t packed = 0;
						std::memcpy(&packed, rec_bytes + offset, sizeof(packed));
						const auto r8 = (packed >> 0u) & 0xffu;
						const auto g8 = (packed >> 8u) & 0xffu;
						const auto b8 = (packed >> 16u) & 0xffu;
						const auto a8 = (packed >> 24u) & 0xffu;
						LOGF("DrawInputState[%u]: vb[%d].rec[%u].attr[%d] dst=v%d fmt=56 "
						     "rgba8=%02" PRIx32 "%02" PRIx32 "%02" PRIx32 "%02" PRIx32
						     " rgba=(%.3f,%.3f,%.3f,%.3f)\n",
						     log_id, bi, rec, ai, rd.register_start, r8, g8, b8, a8,
						     static_cast<double>(r8) / 255.0, static_cast<double>(g8) / 255.0,
						     static_cast<double>(b8) / 255.0, static_cast<double>(a8) / 255.0);
					}
				}
			}
		}

		for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
			const auto& r  = vs_input_info.resources[ai];
			const auto& rd = vs_input_info.resources_dst[ai];
			if (rd.buffer_index != bi) {
				continue;
			}
			LOGF("DrawInputState[%u]: attr[%d] offset=%u dst=v%d regs=%d fetch_index=%u "
			     "sharp=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			     log_id, ai, static_cast<uint32_t>(r.Base48() - b.addr), rd.register_start,
			     rd.registers_num, rd.fetch_index, r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
		}
	}
}

static void SetGraphicsDynamicParams(const GraphicContext& graphics, const HW::Context& ctx,
                                     vk::CommandBuffer vk_buffer,
                                     const ShaderVertexInputInfo& vs_input_info,
                                     const RenderDepthInfo& depth, const RenderState& rendering) {
	KYTY_PROFILER_FUNCTION();

	const auto&        vp  = ctx.GetScreenViewport();
	const vk::Extent2D framebuffer_extent {rendering.width, rendering.height};
	const auto& outputs = vs_input_info.stage.program->info.outputs;
	const bool  indexed_viewports =
	    std::any_of(outputs.begin(), outputs.end(), [](const auto& output) {
		    return output.kind == ShaderRecompiler::IR::StageOutputKind::ViewportIndex;
	    });
	constexpr uint32_t viewport_slots = std::size(HW::ScreenViewport {}.viewports);
	std::array<vk::Viewport, viewport_slots> viewports {};
	std::array<vk::Rect2D, viewport_slots>   scissors {};
	const uint32_t viewport_count = indexed_viewports ? viewport_slots : 1;
	for (uint32_t i = 0; i < viewport_count; i++) {
		const auto& guest    = vp.viewports[i];
		auto&       viewport = viewports[i];
		if (ctx.GetClipControl().clip_disable) {
			const auto& limits = graphics.GetPhysicalDeviceProperties().limits;
			viewport.width  = static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u));
			viewport.height = static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u));
		} else {
			viewport.x      = guest.xoffset - guest.xscale;
			viewport.y      = guest.yoffset - guest.yscale;
			viewport.width  = guest.xscale * 2.0f;
			viewport.height = guest.yscale * 2.0f;
		}
		viewport.minDepth =
		    guest.zoffset - (ctx.GetClipControl().dx_clip_space ? 0.0f : guest.zscale);
		viewport.maxDepth = guest.zscale + guest.zoffset;

		const auto final_scissor =
		    calc_final_scissor(vp, ctx.GetScanModeControl(), framebuffer_extent, i);
		auto& scissor  = scissors[i];
		scissor.offset = {final_scissor.left, final_scissor.top};
		scissor.extent = {static_cast<uint32_t>(final_scissor.right - final_scissor.left),
		                  static_cast<uint32_t>(final_scissor.bottom - final_scissor.top)};
		if (viewport.width == 0.0f) {
			// Keep empty slots at their guest index; Vulkan requires a positive viewport width.
			viewport.width = 1.0f;
			scissor.extent = {0, 0};
		}
	}
	vk_buffer.setViewportWithCount(viewport_count, viewports.data());
	vk_buffer.setScissorWithCount(viewport_count, scissors.data());

	float line_width = ctx.GetLineWidth();
	if (line_width != 1.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("Render: temporary: clamping Vulkan line width %f to 1.0 because wideLines is "
			     "not enabled\n",
			     line_width);
			logged = true;
		}
		line_width = 1.0f;
	}
	vk_buffer.setLineWidth(line_width);
	const auto&      blend = ctx.GetBlendColor();
	const std::array blend_constants {blend.red, blend.green, blend.blue, blend.alpha};
	vk_buffer.setBlendConstants(blend_constants.data());
	vk_buffer.setDepthTestEnable(depth.depth_test_enable ? VK_TRUE : VK_FALSE);
	vk_buffer.setDepthWriteEnable(depth.depth_write_enable ? VK_TRUE : VK_FALSE);
	vk_buffer.setDepthCompareOp(depth.depth_compare_op);

	const auto& mode              = ctx.GetModeControl();
	const auto& poly_offset       = ctx.GetPolyOffset();
	const bool  use_front         = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back          = mode.poly_offset_back_enable && !mode.cull_back;
	const bool  depth_bias_enable = use_front || use_back;
	vk_buffer.setDepthBiasEnable(depth_bias_enable ? VK_TRUE : VK_FALSE);
	if (depth_bias_enable) {
		const float guest_constant_factor =
		    use_front ? poly_offset.front_offset : poly_offset.back_offset;
		const float constant_factor = ConvertPolygonOffsetConstantFactor(
		    guest_constant_factor, poly_offset, depth.desc.view_info.format);
		const float slope_factor =
		    (use_front ? poly_offset.front_scale : poly_offset.back_scale) / 16.0f;
#if defined(__APPLE__)
		const float apple_scale = poly_offset.db_is_float_fmt ? 2.0f : 1.0f;
		vk_buffer.setDepthBias(constant_factor * apple_scale, poly_offset.clamp, slope_factor * apple_scale);
#else
		vk_buffer.setDepthBias(constant_factor, poly_offset.clamp, slope_factor);
#endif
	}

	vk_buffer.setStencilTestEnable(depth.stencil_test_enable ? VK_TRUE : VK_FALSE);
	if (depth.stencil_test_enable) {
		const auto set_stencil = [&](vk::StencilFaceFlagBits face, const vk::StencilOpState& state) {
			vk_buffer.setStencilOp(face, state.failOp, state.passOp, state.depthFailOp, state.compareOp);
			vk_buffer.setStencilCompareMask(face, state.compareMask);
			vk_buffer.setStencilWriteMask(face, state.writeMask);
			vk_buffer.setStencilReference(face, state.reference);
		};
		set_stencil(vk::StencilFaceFlagBits::eFront, depth.stencil_front);
		set_stencil(vk::StencilFaceFlagBits::eBack, depth.stencil_back);
	}

#if defined(__APPLE__)
	// MoltenVK has no VK_EXT_color_write_enable; the pipeline is created without the
	// eColorWriteEnableEXT dynamic state and relies on the static colorWriteMask instead.
#else
	vk::Bool32 enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t slot = 0; slot < rendering.num_color_attachments; slot++) {
		enable[slot] = rendering.color_attachments[slot].image_view != nullptr;
	}
	if (rendering.num_color_attachments != 0) {
		vk_buffer.setColorWriteEnableEXT(rendering.num_color_attachments, enable);
	}
#endif
}

static bool DrawHasValidVertexShader(const HW::Shader& sh_ctx) {

	const auto& vs = sh_ctx.GetVs();
	return vs.es_regs.data_addr != 0;
}

static bool PixelShaderHasDepthOrCoverageSideEffects(const HW::ShaderRegisters& sh_regs) {
	const auto& db = sh_regs.db_shader_control;
	return db.shader_kill_enable || db.shader_z_export_enable || db.shader_mask_export_enable ||
	       db.shader_dual_export_enable || db.shader_execute_on_noop;
}

struct DrawRenderState {
	RenderDepthInfo       depth_info;
	RenderColorInfo       color_info[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	uint32_t              color_count                              = 0;
	bool                  ps_active                                = true;
	std::array<ShaderVertexInputInfo, 3> vertex_info;
	ShaderPixelInputInfo  ps_input_info;
	PipelineCache::GraphicsPrograms programs;
};

struct DrawCallInfo {
	CommandBufferDebugOp debug_op       = CommandBufferDebugOp::DrawIndex;
	uint32_t             index_count    = 0;
	uint32_t             instance_count = 0;
	uint32_t             first_instance = 0;

	[[nodiscard]] bool IsIndexed() const { return debug_op == CommandBufferDebugOp::DrawIndex; }
	[[nodiscard]] const char* Name() const { return IsIndexed() ? "DrawIndex" : "DrawIndexAuto"; }
};

// Stencil written through a color attachment: the guest binds the stencil plane of a depth
// buffer as an R8_UINT color target (Frostbite writes material bits this way). Vulkan cannot
// render to a stencil aspect, so the draw renders into a scratch R8_UINT image seeded from the
// current stencil, and the result is copied back into the depth image's stencil aspect.
struct StencilColorScratch {
	VulkanImage   image;
	vk::ImageView view   = nullptr;
	vk::ImageLayout layout = vk::ImageLayout::eUndefined;
	int           slot   = -1; // color slot rendered through the scratch for the current draw
};
static StencilColorScratch g_stencil_scratch;

static void StencilScratchBarrier(vk::CommandBuffer command, vk::ImageLayout to,
                                  vk::AccessFlags2 access, vk::PipelineStageFlags2 stage) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask       = vk::AccessFlagBits2::eMemoryWrite;
	barrier.dstStageMask        = stage;
	barrier.dstAccessMask       = access;
	barrier.oldLayout           = g_stencil_scratch.layout;
	barrier.newLayout           = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = g_stencil_scratch.image.image;
	barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
	g_stencil_scratch.layout = to;
}

static void BufferTransferBarrier(vk::CommandBuffer command, vk::Buffer buffer) {
	vk::BufferMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	barrier.srcAccessMask       = vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eTransferRead;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	barrier.dstAccessMask       = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = buffer;
	barrier.offset              = 0;
	barrier.size                = VK_WHOLE_SIZE;
	vk::DependencyInfo dependency {};
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
}

// Copies the stencil aspect of `depth` into the scratch (to_scratch) or back (!to_scratch),
// through the device-local utility buffer in row chunks. Both images must already be in the
// matching transfer layouts.
static void CopyStencilScratch(vk::CommandBuffer command, Image& depth, Buffer& buffer,
                               bool to_scratch) {
	const uint32_t width  = depth.backing.extent.width;
	const uint32_t height = depth.backing.extent.height;
	const uint32_t rows   = static_cast<uint32_t>(
        std::max<uint64_t>(1, std::min<uint64_t>(height, buffer.Size() / width)));
	for (uint32_t y = 0; y < height; y += rows) {
		const uint32_t      count = std::min(rows, height - y);
		vk::BufferImageCopy stencil {};
		stencil.imageSubresource = {vk::ImageAspectFlagBits::eStencil, 0, 0, 1};
		stencil.imageOffset      = vk::Offset3D {0, static_cast<int32_t>(y), 0};
		stencil.imageExtent      = vk::Extent3D {width, count, 1};
		auto color               = stencil;
		color.imageSubresource   = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		BufferTransferBarrier(command, buffer.Handle());
		if (to_scratch) {
			command.copyImageToBuffer(depth.backing.image, vk::ImageLayout::eTransferSrcOptimal,
			                          buffer.Handle(), 1, &stencil);
			BufferTransferBarrier(command, buffer.Handle());
			command.copyBufferToImage(buffer.Handle(), g_stencil_scratch.image.image,
			                          vk::ImageLayout::eTransferDstOptimal, 1, &color);
		} else {
			command.copyImageToBuffer(g_stencil_scratch.image.image,
			                          vk::ImageLayout::eTransferSrcOptimal, buffer.Handle(), 1, &color);
			BufferTransferBarrier(command, buffer.Handle());
			command.copyBufferToImage(buffer.Handle(), depth.backing.image,
			                          vk::ImageLayout::eTransferDstOptimal, 1, &stencil);
		}
	}
	BufferTransferBarrier(command, buffer.Handle());
}

static bool EnsureStencilScratch(RenderContext& context, uint32_t width, uint32_t height) {
	auto& scratch = g_stencil_scratch;
	if (scratch.image.image != nullptr && scratch.image.extent.width == width &&
	    scratch.image.extent.height == height) {
		return true;
	}
	auto& graphics = context.GetGraphics();
	if (scratch.image.image != nullptr) {
		auto* old = new StencilColorScratch {};
		old->image.image      = scratch.image.image;
		old->image.allocation = scratch.image.allocation;
		old->view             = scratch.view;
		scratch.image.image      = nullptr;
		scratch.image.allocation = nullptr;
		scratch.view             = nullptr;
		context.GetCommandScheduler().DeferOperationAfterTicks(
		    [&graphics, old] {
			    graphics.device.destroyImageView(old->view);
			    graphics.DeleteImage(old->image);
			    delete old;
		    },
		    8);
	}
	vk::ImageCreateInfo info {};
	info.imageType     = vk::ImageType::e2D;
	info.format        = vk::Format::eR8Uint;
	info.extent        = vk::Extent3D {width, height, 1};
	info.mipLevels     = 1;
	info.arrayLayers   = 1;
	info.samples       = vk::SampleCountFlagBits::e1;
	info.tiling        = vk::ImageTiling::eOptimal;
	info.usage         = vk::ImageUsageFlagBits::eColorAttachment |
	                     vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
	info.initialLayout = vk::ImageLayout::eUndefined;
	if (!graphics.CreateImage(info, scratch.image)) {
		return false;
	}
	vk::ImageViewCreateInfo view {};
	view.image            = scratch.image.image;
	view.viewType         = vk::ImageViewType::e2D;
	view.format           = vk::Format::eR8Uint;
	view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	auto created = graphics.device.createImageView(view);
	if (created.result != vk::Result::eSuccess) {
		return false;
	}
	scratch.view = created.value;
	scratch.layout        = vk::ImageLayout::eUndefined;
	return scratch.view != nullptr;
}

// KYTY_CLAMP_RENDER_AREA=1: a pass with no color or depth target takes its render area from the
// guest screen scissor instead of the device's maximum framebuffer. calc_final_scissor only
// intersects the screen scissor with the other scissors, so every pixel that can be rasterized
// lies inside it; a smaller render area changes nothing the pass can write. Off by default.
static bool RenderAreaClampEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_CLAMP_RENDER_AREA");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

RenderState RenderExecutor::AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
                                                 uint32_t color_count, RenderDepthInfo& depth,
                                                 vk::ImageAspectFlags& feedback_aspects,
                                                 std::span<PreparedBindings* const> stages) {
	EXIT_IF(colors == nullptr || color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	feedback_aspects = {};
	auto&       cache = m_context.GetTextureCache();
	RenderState state {};
	state.width                 = std::numeric_limits<uint32_t>::max();
	state.height                = std::numeric_limits<uint32_t>::max();
	state.num_layers            = std::numeric_limits<uint32_t>::max();
	state.num_color_attachments = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		auto& target = colors[i];
		EXIT_IF(!target.image_id);
		if (static_cast<int>(i) == g_stencil_scratch.slot) {
			const auto extent           = target.Extent();
			state.width                 = std::min(state.width, extent.width);
			state.height                = std::min(state.height, extent.height);
			state.num_layers            = std::min(state.num_layers, 1u);
			state.num_color_attachments = std::max(state.num_color_attachments, target.target_slot + 1);
			auto& attachment            = state.color_attachments[target.target_slot];
			attachment.image_view       = g_stencil_scratch.view;
			attachment.image_layout     = vk::ImageLayout::eColorAttachmentOptimal;
			continue;
		}
		const auto owner = cache.m_slot_images.try_get(target.image_id);
		if (owner == nullptr || (!owner->registered && !owner->info.data.Empty()) ||
		    owner->binding.needs_rebind) {
			EXIT("color target changed after render-state discovery\n");
		}
		const auto image_view = cache.FindRenderTarget(target.image_id, target.desc);
		auto&      image      = cache.GetImage(target.image_id);
		EXIT_IF(image.backing.samples != target.desc.info.samples || image_view == nullptr);
		const auto& view   = target.desc.view_info;
		const auto  layout = image.binding.is_bound ? vk::ImageLayout::eGeneral
		                                            : vk::ImageLayout::eColorAttachmentOptimal;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access =
		    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
		image.Transit(layout, image.binding.attachment_access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer);
		const auto extent       = target.Extent();
		state.width             = std::min(state.width, extent.width);
		state.height            = std::min(state.height, extent.height);
		state.num_layers        = std::min(state.num_layers, view.layer_count);
		state.num_color_attachments = std::max(state.num_color_attachments, target.target_slot + 1);
		auto& attachment            = state.color_attachments[target.target_slot];
		attachment.image_view   = image_view;
		attachment.image_layout = layout;
	}
	if (depth.image_id) {
		const auto owner = cache.m_slot_images.try_get(depth.image_id);
		if (owner == nullptr || !owner->registered || owner->binding.needs_rebind) {
			EXIT("depth target changed after render-state discovery\n");
		}
		const auto  image_view = cache.FindDepthTarget(depth.image_id, depth.desc);
		const auto& metadata   = depth.desc.info.metadata;
		if (metadata.kind == ImageMetadataKind::Htile && depth.depth_clear_enable &&
		    !cache.ClearMeta(metadata.range.address)) {
			EXIT("failed to acquire HTile metadata for a depth clear\n");
		}
		const bool meta_clear =
		    metadata.kind == ImageMetadataKind::Htile &&
		    cache.IsMetaCleared(metadata.range.address, depth.desc.view_info.base_layer);
		depth.depth_load_clear_enable = depth.depth_clear_enable || meta_clear;
		if (meta_clear &&
		    !cache.TouchMeta(metadata.range.address, depth.desc.view_info.base_layer, false)) {
			EXIT("failed to consume HTile clear state\n");
		}
		auto& image = cache.GetImage(depth.image_id);
		EXIT_IF(image_view == nullptr || image.backing.samples != depth.desc.info.samples);
		const auto draw_writes = depth.AttachmentWriteAspects();
		vk::ImageAspectFlags sampled_aspects;
		for (const auto* stage: stages) {
			for (const auto& binding: stage->images) {
				if (binding.image_id != depth.image_id ||
				    binding.desc.type != TextureCache::BindingType::Texture) continue;
				const auto native =
				    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
				EXIT_IF(native == image.views.end());
				sampled_aspects |= native->info.aspect;
				feedback_aspects |= DepthFeedbackAspects(draw_writes, depth.desc.view_info,
				                                         native->info);
			}
		}
		if (feedback_aspects && !m_context.GetGraphics().attachment_feedback_loop_enabled) {
			// MoltenVK has no VK_EXT_attachment_feedback_loop_layout. Sampling a bound depth
			// attachment in GENERAL layout works on Metal; warn instead of aborting the game.
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
				LOGF("warning: depth attachment feedback loop without host support, using "
				     "GENERAL layout (aspects=0x%x)\n",
				     static_cast<uint32_t>(feedback_aspects));
			}
		}
		auto layout = depth_attachment_layout(depth);
		if (sampled_aspects & ~DepthReadableAspects(layout)) {
			layout = m_context.GetGraphics().attachment_feedback_loop_enabled
			             ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
			             : vk::ImageLayout::eGeneral;
		}
		// The attachment store writes even when guest depth/stencil tests do not.
		const auto access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
		                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access = access;
		const auto& view                = depth.desc.view_info;
		image.Transit(layout, access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer);
		state.width               = std::min(state.width, depth.desc.info.extent.width);
		state.height              = std::min(state.height, depth.desc.info.extent.height);
		state.num_layers          = std::min(state.num_layers, view.layer_count);
		const auto aspects        = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		auto&      attachment     = state.depth_stencil_attachment;
		attachment.image_view     = image_view;
		attachment.image_layout   = layout;
		attachment.clear_value[0] = std::bit_cast<uint32_t>(depth.depth_clear_value);
		attachment.clear_value[1] = depth.stencil_clear_value;
		attachment.has_depth      = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.depth_clear    = depth.depth_load_clear_enable;
		attachment.has_stencil    = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eStencil);
		attachment.stencil_clear  = depth.stencil_clear_enable;
	}
	if (color_count == 0 && !depth.image_id) {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		state.width        = limits.maxFramebufferWidth;
		state.height       = limits.maxFramebufferHeight;
		if (RenderAreaClampEnabled()) {
			const auto& screen = buffer.GetRegisters().GetScreenViewport();
			if (screen.screen_scissor_right > 0 && screen.screen_scissor_bottom > 0) {
				state.width  = std::min(state.width, static_cast<uint32_t>(screen.screen_scissor_right));
				state.height = std::min(state.height, static_cast<uint32_t>(screen.screen_scissor_bottom));
			}
		}
	}
	if (state.num_layers == std::numeric_limits<uint32_t>::max()) {
		state.num_layers = 1;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.width == std::numeric_limits<uint32_t>::max() ||
	        state.height == std::numeric_limits<uint32_t>::max());
	return state;
}

static uint32_t DrawColorOutputMask(const HW::Context& ctx) {
	const auto& sh_regs     = ctx.GetShaderRegisters();
	const auto  write_mask  = ctx.GetRenderTargetMask() & sh_regs.m_cbShaderMask;
	uint32_t    output_mask = 0;
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if (sh_regs.target_output_mode[slot] != 0 &&
		    render_target_mask_slot(write_mask, slot) != 0) {
			output_mask |= 1u << slot;
		}
	}
	return output_mask;
}

enum class CbColorMode : uint8_t {
	Disable            = 0,
	Normal             = 1,
	EliminateFastClear = 2,
	Resolve            = 3,
	FmaskDecompress    = 5,
	DccDecompress      = 6,
};

static bool ConsumeMetadataColorOperation(const CommandBuffer& buffer) {
	const auto& ctx  = buffer.GetRegisters();
	const auto  mode = ctx.GetColorControl().mode;
	// These special modes run color-buffer metadata or decompression operations. The shader is a
	// vehicle for that operation, and its exported color must not be applied as a normal draw.
	// Kyty stores expanded Vulkan images rather than compressed guest surfaces, so no equivalent
	// hardware pass is emitted. Tracked DCC clear state is materialized on attachment bind;
	// future CMask/FMask support can consume its state through the same TextureCache path.
	return mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear) ||
	       mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ||
	       mode == static_cast<uint8_t>(CbColorMode::DccDecompress);
}

struct DrawEmitInfo {
	int32_t  vertex_offset = 0;
	uint32_t first_vertex  = 0;
	uint32_t first_instance = 0;
};

struct DrawIndexBufferSource {
	uint64_t      address   = 0;
	const void*   host_data = nullptr;
	uint64_t      size      = 0;
	vk::IndexType type      = vk::IndexType::eUint16;
	uint32_t      guest_element_size = 0;
};

struct PreparedIndexBuffer {
	vk::Buffer     buffer = nullptr;
	vk::DeviceSize offset = 0;
	vk::IndexType  type   = vk::IndexType::eUint16;
};

static uint64_t VertexBufferDescriptorSize(int binding, const ShaderVertexInputInfo& info) {
	const auto& buffer = info.buffers[binding];
	if (buffer.stride != 0 || buffer.num_records == 0) {
		return static_cast<uint64_t>(buffer.stride) * buffer.num_records;
	}

	uint64_t size = 0;
	for (int i = 0; i < info.resources_num; i++) {
		if (info.resources_dst[i].buffer_index != binding) {
			continue;
		}
		const auto& resource = info.resources[i];
		// RDNA2 OOB_SELECT=2 only checks NumRecords != 0. A constant attribute still
		// fetches its entire format; NumRecords is not a byte count in this mode.
		const uint64_t extent = resource.OutOfBounds() == 2
		                            ? resource.Base48() - buffer.addr +
		                                  ShaderRecompiler::Format::GetFormatInfo(resource.Format()).byte_size
		                            : buffer.num_records;
		size = std::max(size, extent);
	}
	return size;
}

struct VertexBufferRange {
	uint64_t                     base_address  = 0;
	uint64_t                     requested_end = 0;
	uint64_t                     acquired_end  = 0;
	std::pair<Buffer*, uint64_t> binding;

	[[nodiscard]] uint64_t RequestedSize() const { return requested_end - base_address; }
};

struct PreparedVertexBuffers {
	static constexpr uint32_t MaxBuffers = ShaderVertexInputInfo::RES_MAX;

	std::array<vk::Buffer, MaxBuffers>     buffers {};
	std::array<vk::DeviceSize, MaxBuffers> offsets {};
	std::array<vk::DeviceSize, MaxBuffers> sizes {};
	uint32_t                               count = 0;
};

static PreparedVertexBuffers AcquireVertexBuffers(CommandBuffer&               buffer,
                                                  const ShaderVertexInputInfo& vs_input_info) {
	EXIT_IF(vs_input_info.buffers_num < 0 ||
	        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX);

	// Collect the non-empty guest vertex ranges.
	std::array<uint64_t, ShaderVertexInputInfo::RES_MAX>          sizes {};
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> ranges {};
	uint32_t                                                      range_count = 0;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(i, vs_input_info);
		sizes[i]           = size;
		if (size == 0) {
			continue;
		}
		if (vertex.addr == 0 || size > UINT64_MAX - vertex.addr) {
			EXIT("invalid vertex buffer range: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vertex.addr, size);
		}
		ranges[range_count++] = {vertex.addr, vertex.addr + size};
	}

	std::sort(ranges.begin(), ranges.begin() + range_count,
	          [](const VertexBufferRange& left, const VertexBufferRange& right) {
		          return left.base_address < right.base_address;
	          });

	// Merge overlapping or touching ranges before acquiring host buffers.
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> merged_ranges {};
	uint32_t                                                      merged_count = 0;
	for (uint32_t i = 0; i < range_count; i++) {
		const auto& range = ranges[i];
		if (merged_count != 0 &&
		    merged_ranges[merged_count - 1].requested_end >= range.base_address) {
			merged_ranges[merged_count - 1].requested_end =
			    std::max(merged_ranges[merged_count - 1].requested_end, range.requested_end);
			continue;
		}
		merged_ranges[merged_count++] = {range.base_address, range.requested_end};
	}

	auto& cache = buffer.GetContext().GetBufferCache();
	for (uint32_t i = 0; i < merged_count; i++) {
		auto& range = merged_ranges[i];
		// PPSA20298
		const auto size =
		    Libs::LibKernel::Memory::ClampRangeSize(range.base_address, range.RequestedSize());
		range.acquired_end = range.base_address + size;
		range.binding      = cache.ObtainBuffer(range.base_address, size, false);
	}

	// Rebuild slot bindings, offsetting non-empty slots into their acquired merged range.
	PreparedVertexBuffers prepared;
	prepared.count         = static_cast<uint32_t>(vs_input_info.buffers_num);
	vk::Buffer null_buffer = nullptr;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = sizes[i];
		if (size == 0) {
			if (null_buffer == nullptr) {
				null_buffer = cache.GetBuffer(NULL_BUFFER_ID).Handle();
			}
			prepared.buffers[i] = null_buffer;
			prepared.offsets[i] = 0;
			continue;
		}

		const auto range = std::find_if(merged_ranges.begin(), merged_ranges.begin() + merged_count,
		                                [&](const VertexBufferRange& value) {
			                                return vertex.addr >= value.base_address &&
			                                       vertex.addr < value.acquired_end;
		                                });
		if (range == merged_ranges.begin() + merged_count) {
			EXIT("vertex buffer address is outside the acquired range: addr=0x%016" PRIx64 "\n",
			     vertex.addr);
		}

		prepared.buffers[i] = range->binding.first->Handle();
		prepared.offsets[i] = range->binding.second + vertex.addr - range->base_address;
		prepared.sizes[i]   = std::min(size, range->acquired_end - vertex.addr);
	}

	return prepared;
}

static void SetDrawDebugPhase(CommandBuffer& buffer, uint64_t submit_id, const DrawCallInfo& draw,
                              uint32_t phase) {
	buffer.SetDebugInfo(static_cast<uint32_t>(draw.debug_op), submit_id, phase, draw.index_count, 0,
	                    draw.instance_count, draw.first_instance);
}

static bool GetDrawTopology(const HW::UserConfig& ucfg, vk::PrimitiveTopology& topology) {

	topology = vk::PrimitiveTopology::ePointList;

	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kNone: return false;
		case Prospero::PrimitiveType::kPointList:
			topology = vk::PrimitiveTopology::ePointList;
			break;
		case Prospero::PrimitiveType::kLineList: topology = vk::PrimitiveTopology::eLineList; break;
		case Prospero::PrimitiveType::kLineStrip:
			topology = vk::PrimitiveTopology::eLineStrip;
			break;
		case Prospero::PrimitiveType::kTriList:
			topology = vk::PrimitiveTopology::eTriangleList;
			break;
		case Prospero::PrimitiveType::kTriFan:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			topology = vk::PrimitiveTopology::eTriangleStrip;
			break;
		case Prospero::PrimitiveType::kPatch:
			if (!Config::TessellationEnabled()) return false;
			[[fallthrough]];
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy: {
			// KYTY_RECTLIST_AS_TRI=1 (diagnostic): draw only the first triangle of each rect
			// instead of expanding it through the tessellation stages.
			static const bool as_tri = [] {
				const char* value = std::getenv("KYTY_RECTLIST_AS_TRI");
				return value != nullptr && value[0] == '1';
			}();
			topology = as_tri ? vk::PrimitiveTopology::eTriangleList
			                  : vk::PrimitiveTopology::ePatchList;
			break;
		}
		case Prospero::PrimitiveType::kQuadListLegacy:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		default: {
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				std::printf("Skipping draw with unknown primitive type: %u\n",
				            static_cast<uint32_t>(ucfg.GetPrimType()));
			}
			return false;
		}
	}

	return true;
}

static bool ResolvePrimitiveRestart(const CommandBuffer& buffer,
                                    const DrawIndexBufferSource& source) {
	const auto control = buffer.GetUserConfig().GetPrimitiveResetControl();
	EXIT_NOT_IMPLEMENTED((control & ~0x3u) != 0);
	if ((control & 0x1u) == 0) {
		return false;
	}
	switch (buffer.GetUserConfig().GetPrimType()) {
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip: break;
		default: return false;
	}

	const auto element_size = source.guest_element_size;
	const auto index_mask   = UINT32_MAX >> ((4 - element_size) * 8);
	const auto reset_index  = buffer.GetRegisters().GetPrimitiveResetIndex();
	if ((control & 0x2u) != 0 && (reset_index & ~index_mask) != 0) {
		return false;
	}
	const auto restart_index = reset_index & index_mask;
	if (restart_index == index_mask) {
		// Use native restart; the 8-bit path widens its marker to 0xffff.
		return true;
	}

	// A game can set a custom reset value without using it in the index buffer.
	// Keep restart off in that case; fail if we actually find the value.
	// Scan before preparing draw resources: readback can restart the command buffer.
	EXIT_NOT_IMPLEMENTED(source.address == 0);
	const auto* indices = reinterpret_cast<const uint8_t*>(source.address);
	for (uint64_t offset = 0; offset < source.size; offset += element_size) {
		uint32_t index = 0;
		std::memcpy(&index, indices + offset, element_size);
		EXIT_NOT_IMPLEMENTED(index == restart_index);
	}
	return false;
}

static void RefreshShaders(CommandBuffer& buffer, const DrawCallInfo& draw,
                           uint32_t color_output_mask, DrawRenderState& state) {
	ProfileDetailScope profile("cpu_state_refresh_shaders");
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	const auto& vertex_shader_info = sh_ctx.GetVs();
	const auto& pixel_shader_info  = sh_ctx.GetPs();
	const auto& shader_regs        = ctx.GetShaderRegisters();

	state.programs      = {};
	state.ps_input_info = {};
	std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX>
	    target_export_mapping {};
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		const auto& rt = ctx.GetRenderTarget(slot);
		if ((color_output_mask & (1u << slot)) != 0 && rt.base.addr != 0) {
			target_export_mapping[slot] =
			    TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type,
			                                 rt.info.channel_order)
			        .export_mapping;
		}
	}
	auto& pipeline_cache = buffer.GetContext().GetPipelineCache();
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "GetGraphicsPrograms");
	}
	state.programs = pipeline_cache.GetGraphicsPrograms(
	    vertex_shader_info, pixel_shader_info, shader_regs, ctx, buffer.GetUserConfig(),
	    target_export_mapping, state.ps_active, state.vertex_info, state.ps_input_info);
}

bool RenderExecutor::PrepareDrawRenderState(CommandBuffer& buffer, const DrawCallInfo& draw,
                                            uint32_t            render_target_slice_offset,
	                                        DrawRenderState& state) {
	ProfileDetailScope profile("cpu_prepare_draw_state");
	const auto& shader_regs       = buffer.GetRegisters().GetShaderRegisters();
	const auto  color_output_mask = DrawColorOutputMask(buffer.GetRegisters());
	state.ps_active = buffer.GetShaders().GetPs().ps_regs.data_addr != 0 &&
	                  (color_output_mask != 0 ||
	                   PixelShaderHasDepthOrCoverageSideEffects(shader_regs));
	RefreshShaders(buffer, draw, color_output_mask, state);
	if (profile.Active()) profile.SetIdentity(state.programs.vertex[0].id);
	uint32_t mrt_mask = 0;
	if (state.ps_active) {
		for (const auto& output: state.ps_input_info.stage.program->info.outputs) {
			if (output.kind == ShaderRecompiler::IR::StageOutputKind::Mrt) {
				mrt_mask |= 1u << output.index;
			}
		}
	}
	mrt_mask &= color_output_mask;
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "ResolveRenderColorTarget");
	}
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if ((mrt_mask & (1u << slot)) != 0) {
			ResolveRenderColorTarget(buffer, state.color_info[state.color_count],
			                         render_target_slice_offset, slot);
			if (state.color_info[state.color_count].image_id) {
				state.color_count++;
			}
		}
	}
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "ResolveRenderDepthTarget");
	}
	ResolveRenderDepthTarget(buffer, state.depth_info);
	ProfileDetailEvent("state_rebuilt", state.programs.vertex[0].id, state.color_count);

	if (state.color_count == 0 && !state.depth_info.image_id && !state.ps_active) {
		LogFramebufferSkip(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   draw.index_count, 0);
		return false;
	}

	return true;
}

static PreparedIndexBuffer PrepareIndexBuffer(CommandBuffer&               buffer,
                                              const DrawIndexBufferSource& source) {
	PreparedIndexBuffer prepared;
	if (source.size == 0) {
		return prepared;
	}
	prepared.type = source.type;
	if (source.host_data != nullptr) {
		auto& stream = buffer.GetContext().GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		prepared.offset = stream.Copy(source.host_data, source.size, 16);
		prepared.buffer = stream.Handle();
	} else {
		auto [buffer_ptr, offset] =
		    buffer.GetContext().GetBufferCache().ObtainBuffer(source.address, source.size, false);
		prepared.buffer = buffer_ptr->Handle();
		prepared.offset = offset;
	}
	return prepared;
}

static void CommitVertexBuffers(vk::CommandBuffer            vk_buffer,
                                const PreparedVertexBuffers& prepared) {
	for (uint32_t i = 0; i < prepared.count; i++) {
		EXIT_IF(prepared.buffers[i] == nullptr);
	}
	if (prepared.count != 0) {
		// Guest descriptor bounds must survive allocation merging in the cache.
		vk_buffer.bindVertexBuffers2(0, prepared.count, prepared.buffers.data(),
		                             prepared.offsets.data(), prepared.sizes.data(), nullptr);
	}
}

static void CommitIndexBuffer(vk::CommandBuffer vk_buffer, const PreparedIndexBuffer& prepared) {
	if (prepared.buffer == nullptr) {
		return;
	}
	vk_buffer.bindIndexBuffer(prepared.buffer, prepared.offset, prepared.type);
}

static void LogDrawStateIfNeeded(const CommandBuffer& buffer, const DrawCallInfo& draw,
	                             const DrawRenderState& state, uint32_t index_type_and_size,
                                 const void* index_addr) {
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	if (!draw.IsIndexed() && !Prospero::IsRectList(buffer.GetUserConfig().GetPrimType())) {
		return;
	}

	if (state.ps_active) {
		LogDrawTargetState(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   state.ps_input_info, draw.index_count, 0);
	}
	LogDrawInputState(buffer, state.color_info[0], state.vertex_info[0], index_type_and_size,
	                  draw.index_count, index_addr);
}

static void EmitDrawPrimitives(Prospero::PrimitiveType prim, vk::CommandBuffer vk_buffer,
                               const DrawCallInfo& draw, const DrawEmitInfo& emit) {
	if (draw.index_count == 0 || draw.instance_count == 0) {
		return;
	}
	const uint32_t instance_count = std::max(1u, std::min(draw.instance_count, 65536u));
	switch (prim) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
		case Prospero::PrimitiveType::kPatch:
			if (draw.IsIndexed()) {
				vk_buffer.drawIndexed(draw.index_count, instance_count, 0, emit.vertex_offset,
				                      emit.first_instance);
			} else {
				vk_buffer.draw(draw.index_count, instance_count, emit.first_vertex,
				               emit.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				if (draw.IsIndexed()) {
					vk_buffer.drawIndexed(4, instance_count, i, emit.vertex_offset,
					                      emit.first_instance);
				} else {
					vk_buffer.draw(4, instance_count, i + emit.first_vertex,
					               emit.first_instance);
				}
			}
			break;
		default: EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(prim));
	}
}

void RenderExecutor::ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer,
                                         const DrawCallInfo& draw, DrawRenderState& state,
                                         vk::PrimitiveTopology topology, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source,
	                                     bool primitive_restart_enable) {
	if (draw.instance_count > 65536) {
		static std::atomic<uint32_t> insane_draw_log {0};
		if (insane_draw_log.fetch_add(1) < 16) {
			std::fprintf(stderr,
			             "[draw] clamped insane instance_count %u -> 1 (ps=0x%016llx, idx=%u)\n",
			             draw.instance_count,
			             static_cast<unsigned long long>(
			                 state.ps_active && state.ps_input_info.stage.program
			                     ? state.ps_input_info.stage.program->shader_hash
			                     : 0),
			             draw.index_count);
		}
		const_cast<DrawCallInfo&>(draw).instance_count = 1;
	}
	if (draw.index_count == 0 || draw.instance_count == 0) {
		return;
	}
	if (state.ps_active && state.ps_input_info.stage.program &&
	    DebugSkipShader(state.ps_input_info.stage.program->shader_hash)) {
		return;
	}
	DebugRecordWork("draw",
	                state.ps_active && state.ps_input_info.stage.program
	                    ? state.ps_input_info.stage.program->shader_hash
	                    : 0,
	                draw.index_count, draw.instance_count, state.color_count);
	if (DebugFrameTracing()) {
		const auto& ps = state.ps_input_info;
		std::fprintf(stderr,
		             "[frame] draw ps=0x%016llx idx=%u inst=%u colors=%u depth=%d pos=%d%d%d%d "
		             "ff=%d anc=%d sysbase=%u persp=%u inputs=%u cntl0=0x%x custom=0x%x "
		             "nopersp=%d earlyz=%d kill=%d\n",
		             static_cast<unsigned long long>(
		                 state.ps_active && ps.stage.program ? ps.stage.program->shader_hash : 0),
		             draw.index_count, draw.instance_count, state.color_count,
		             state.depth_info.image_id ? 1 : 0, ps.ps_pos_x, ps.ps_pos_y, ps.ps_pos_z,
		             ps.ps_pos_w, ps.ps_front_face, ps.ps_ancillary, ps.ps_system_input_base,
		             ps.ps_perspective_center_vgpr, ps.input_num, ps.interpolator_settings[0],
		             ps.custom_interpolation_mask, ps.ps_no_perspective, ps.ps_early_z,
		             ps.ps_pixel_kill_enable);
		const auto& hw = buffer.GetRegisters();
		const auto& vp = hw.GetScreenViewport();
		const auto& v0 = vp.viewports[0];
		std::fprintf(stderr,
		             "[frame] draw-vp samples=%u msaa=%d vp=(%f,%f,%f,%f,%f,%f) z=[%f,%f] "
		             "vpsc=(%d,%d)-(%d,%d) scr=(%d,%d)-(%d,%d) gen=(%d,%d)-(%d,%d) "
		             "win=(%d,%d)-(%d,%d) xform=0x%x zclip=%d dx=%d\n",
		             hw.GetAaConfig().msaa_num_samples, hw.GetScanModeControl().msaa_enable ? 1 : 0,
		             v0.xscale, v0.xoffset, v0.yscale, v0.yoffset, v0.zscale, v0.zoffset, v0.zmin,
		             v0.zmax, v0.viewport_scissor_left, v0.viewport_scissor_top,
		             v0.viewport_scissor_right, v0.viewport_scissor_bottom, vp.screen_scissor_left,
		             vp.screen_scissor_top, vp.screen_scissor_right, vp.screen_scissor_bottom,
		             vp.generic_scissor_left, vp.generic_scissor_top, vp.generic_scissor_right,
		             vp.generic_scissor_bottom, vp.window_scissor_left, vp.window_scissor_top,
		             vp.window_scissor_right, vp.window_scissor_bottom, vp.transform_control,
		             hw.GetClipControl().IsZClipEnabled() ? 1 : 0,
		             hw.GetClipControl().dx_clip_space ? 1 : 0);
	}
	auto& ucfg = buffer.GetUserConfig();
	const auto vertex_stages =
	    std::span {state.vertex_info.data(), state.programs.VertexStageCount()};
	const bool mesh_active = state.vertex_info[0].stage.program->stage == ShaderType::Mesh;
	uint32_t   mesh_groups = 0;
	if (mesh_active) {
		const auto& mesh = state.vertex_info[0].mesh;
		EXIT_NOT_IMPLEMENTED(mesh.fast_launch && (draw.IsIndexed() || primitive_restart_enable));
		static std::atomic_bool restart_warned = false;
		if (primitive_restart_enable && !restart_warned.exchange(true, std::memory_order_relaxed)) {
			std::printf("Warning: primitive restart is not implemented for mesh shaders; "
			            "continuing draw (primitive=%u indexed=%u)\n",
			            static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed());
		}
		if (mesh.primitives_per_group == 0) {
			EXIT("unsupported mesh draw: primitive=%u indexed=%u restart=%u\n",
			     static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed(), primitive_restart_enable);
		}
		const auto primitives = mesh.InputPrimitiveCount(draw.index_count);
		if (primitives == 0 || draw.instance_count == 0) {
			return;
		}
		mesh_groups        = (primitives - 1u) / mesh.primitives_per_group + 1u;
		const auto& limits = m_context.GetGraphics().mesh_shader_properties;
		if (mesh_groups > limits.maxMeshWorkGroupCount[0] ||
		    draw.instance_count > limits.maxMeshWorkGroupCount[1] ||
		    static_cast<uint64_t>(mesh_groups) * draw.instance_count >
		        limits.maxMeshWorkGroupTotalCount) {
			EXIT("mesh draw exceeds host workgroup limits: %ux%u\n", mesh_groups,
			     draw.instance_count);
		}
	}

	ImageId stencil_depth_id {};
	g_stencil_scratch.slot = -1;
	{
		// A color target that resolved to a stencil association: render through the scratch
		// R8_UINT image, seeded from the current stencil (see StencilColorScratch).
		auto& cache = m_context.GetTextureCache();
		for (uint32_t i = 0; i < state.color_count; i++) {
			const auto* owner = state.color_info[i].image_id
			                        ? cache.m_slot_images.try_get(state.color_info[i].image_id)
			                        : nullptr;
			if (owner == nullptr || !owner->depth_id) {
				continue;
			}
			const auto depth_id = owner->depth_id;
			cache.UpdateImage(depth_id);
			auto&      depth  = cache.GetImage(depth_id);
			const bool usable = state.color_count == 1 && depth.backing.image != nullptr &&
			                    depth.backing.samples == 1 && depth.info.HasStencil() &&
			                    EnsureStencilScratch(m_context, depth.backing.extent.width,
			                                         depth.backing.extent.height);
			if (!usable) {
				static std::atomic_bool warned = false;
				if (!warned.exchange(true, std::memory_order_relaxed)) {
					std::fprintf(stderr,
					             "Warning: dropping draws into a stencil plane bound as color "
					             "(addr=0x%" PRIx64 " %ux%u)\n",
					             owner->info.data.address, owner->info.extent.width,
					             owner->info.extent.height);
				}
				return;
			}
			auto& scheduler = m_context.GetCommandScheduler();
			scheduler.EndRendering();
			const auto command = scheduler.Current().Handle();
			auto& utility = m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::DeviceLocal);
			depth.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
			              {}, command);
			StencilScratchBarrier(command, vk::ImageLayout::eTransferDstOptimal,
			                      vk::AccessFlagBits2::eTransferWrite,
			                      vk::PipelineStageFlagBits2::eTransfer);
			CopyStencilScratch(command, depth, utility, true);
			StencilScratchBarrier(command, vk::ImageLayout::eColorAttachmentOptimal,
			                      vk::AccessFlagBits2::eColorAttachmentRead |
			                          vk::AccessFlagBits2::eColorAttachmentWrite,
			                      vk::PipelineStageFlagBits2::eColorAttachmentOutput);
			{
				static std::atomic_uint32_t logged = 0;
				if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
					std::fprintf(stderr,
					             "[stencil] color write into stencil 0x%" PRIx64 " of depth 0x%" PRIx64
					             " %ux%u\n",
					             owner->info.data.address, depth.info.data.address,
					             depth.backing.extent.width, depth.backing.extent.height);
				}
			}
			g_stencil_scratch.slot = static_cast<int>(i);
			stencil_depth_id       = depth_id;
			break;
		}
	}
	if (!mesh_active && Prospero::IsRectList(ucfg.GetPrimType())) {
		// KYTY_TRACE_RECT=1: log each new (VS, PS) rect-list pair once.
		// KYTY_SKIP_RECT_PS=<hex,hex|all>: drop rect-list draws with these pixel shaders.
		const uint64_t vs_hash = state.vertex_info[0].stage.program
		                             ? state.vertex_info[0].stage.program->shader_hash
		                             : 0;
		const uint64_t ps_hash = state.ps_active && state.ps_input_info.stage.program
		                             ? state.ps_input_info.stage.program->shader_hash
		                             : 0;
		static const bool trace_rect = std::getenv("KYTY_TRACE_RECT") != nullptr;
		if (trace_rect) {
			static std::mutex                             trace_mutex;
			static std::unordered_map<uint64_t, uint32_t> seen;
			std::lock_guard lock(trace_mutex);
			auto&           count = seen[vs_hash * 31u ^ ps_hash];
			if (count++ == 0) {
				std::fprintf(stderr,
				             "[rect] vs=0x%016llx ps=0x%016llx count=%u inst=%u indexed=%d\n",
				             static_cast<unsigned long long>(vs_hash),
				             static_cast<unsigned long long>(ps_hash), draw.index_count,
				             draw.instance_count, draw.IsIndexed() ? 1 : 0);
				std::fflush(stderr);
			}
		}
		if (const char* skip = std::getenv("KYTY_SKIP_RECT_PS"); skip != nullptr) {
			if (std::strcmp(skip, "all") == 0) {
				return;
			}
			for (const char* p = skip; *p != 0;) {
				char*      end = nullptr;
				const auto h   = std::strtoull(p, &end, 16);
				if (end == p) break;
				if (h == ps_hash) {
					return;
				}
				p = (*end == ',') ? end + 1 : end;
			}
		}
	}

	if (mesh_active && draw.IsIndexed()) {
		// Register the original guest indices for shader reads; PrepareGraphicsBindings
		// synchronizes registered BDA ranges before any draw commands are committed.
		(void)m_context.GetBufferCache().FindBuffer(
		    index_source.address, static_cast<uint64_t>(draw.index_count) *
		                              index_source.guest_element_size);
	}
	LogDrawPhase(draw.Name(), "PrepareBindings");
	auto& scheduler = m_context.GetCommandScheduler();
	CpuDrawPhaseTimer cpu_phase(scheduler, CommandScheduler::CpuDrawPhase::Bindings);
	const bool reuse_draws = BindReuseEnabled();
	if (reuse_draws) {
		m_bind_reuse_scope = true;
	}
	auto&                            bindings = m_graphics_bindings;
	std::array<PreparedBindings*, 4> descriptor_stages {};
	uint32_t                         stage_count = 0;
	for (uint32_t i = 0; i < vertex_stages.size(); i++) {
		PrepareBindings(state.vertex_info[i].stage, bindings.vertex[i]);
		TraceBindVk('V', bindings.vertex[i]);
		descriptor_stages[stage_count++] = &bindings.vertex[i];
	}
	if (state.ps_active) {
		if (!bindings.pixel) bindings.pixel.emplace();
		PrepareBindings(state.ps_input_info.stage, *bindings.pixel);
		TraceBindVk('P', *bindings.pixel);
		descriptor_stages[stage_count++] = &*bindings.pixel;
	}
	const auto stages = std::span {descriptor_stages.data(), stage_count};
	PrepareGraphicsBindings(stages, std::span {state.color_info, state.color_count});
	if (reuse_draws) {
		m_bind_reuse_scope = false;
	}
	cpu_phase.Next(CommandScheduler::CpuDrawPhase::VertexIndex);
	PreparedVertexBuffers vertex_bindings;
	PreparedIndexBuffer   index_binding;
	if (!mesh_active) {
		LogDrawPhase(draw.Name(), "PrepareVertexBuffers");
		vertex_bindings = AcquireVertexBuffers(buffer, state.vertex_info[0]);
		index_binding   = PrepareIndexBuffer(buffer, index_source);
	}
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "CreatePipeline");
	}
	cpu_phase.Next(CommandScheduler::CpuDrawPhase::Pipeline);
	auto& pipeline = m_context.GetPipelineCache().GetGraphicsPipeline(
	    std::span {state.color_info, state.color_count}, state.depth_info, vertex_stages, buffer,
	    state.ps_active ? &state.ps_input_info : nullptr, topology, primitive_restart_enable,
	    state.programs);
	cpu_phase.Next(CommandScheduler::CpuDrawPhase::Targets);
	vk::ImageAspectFlags feedback_aspects;
	const auto rendering =
	    AcquireRenderTargets(buffer, state.color_info, state.color_count, state.depth_info,
	                         feedback_aspects, stages);
	cpu_phase.Next(CommandScheduler::CpuDrawPhase::CommitDraw);

	// Resource preparation above may synchronously finish and restart the scheduler. From this
	// point onward, every operation targets the current command buffer and cannot touch guest
	// memory. With the commit thread on, that recording runs beside later guest packets that do
	// not touch the command buffer. The next command-buffer user joins it first.
	vk::PipelineStageFlags shader_write_stages = {};
	for (const auto& stage: vertex_stages) {
		if (HasShaderBufferWrites(stage.stage)) {
			shader_write_stages |= ShaderPipelineStages(NativeShaderStage(stage.logical_stage));
		}
	}
	if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
		shader_write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
	}

	struct CommitWork {
		CommandBuffer*                     buffer = nullptr;
		vk::CommandBuffer                  vk {};
		uint64_t                           submit_id = 0;
		DrawCallInfo                       draw {};
		bool                               mesh_active = false;
		uint32_t                           mesh_groups = 0;
		bool                               ps_active   = false;
		PreparedVertexBuffers              vertex_bindings {};
		PreparedIndexBuffer                index_binding {};
		const PipelineCache::Pipeline*     pipeline = nullptr;
		std::array<PreparedBindings*, 4>   stages {};
		std::array<ShaderStageRuntime, 4>  runtimes {};
		uint32_t                           stage_count = 0;
		DrawEmitInfo                       emit {};
		Prospero::PrimitiveType            prim {};
		HW::Context                        context {};
		ShaderVertexInputInfo              vertex_input {};
		RenderDepthInfo                    depth {};
		RenderState                        rendering {};
		vk::ImageAspectFlags               feedback {};
		bool                               feedback_enabled = false;
		vk::PipelineStageFlags             shader_writes {};
		uint32_t                           guest_element_size = 0;
		uint64_t                           index_address      = 0;
		ImageId                            stencil_depth_id {};
	};
	auto work                 = std::make_shared<CommitWork>();
	work->buffer              = &buffer;
	work->vk                  = buffer.Handle();
	work->submit_id           = submit_id;
	work->draw                = draw;
	work->mesh_active         = mesh_active;
	work->mesh_groups         = mesh_groups;
	work->ps_active           = state.ps_active;
	work->vertex_bindings     = vertex_bindings;
	work->index_binding       = index_binding;
	work->pipeline            = &pipeline;
	work->stage_count         = stage_count;
	std::copy_n(descriptor_stages.data(), stage_count, work->stages.data());
	// PreparedBindings::runtime points at DrawRenderState, which dies when this draw
	// returns. The commit thread outlives that frame, so keep the two cache pointers here.
	for (uint32_t i = 0; i < stage_count; ++i) {
		auto* prepared = work->stages[i];
		EXIT_IF(prepared == nullptr || prepared->runtime == nullptr || !*prepared->runtime);
		work->runtimes[i]  = *prepared->runtime;
		prepared->runtime  = &work->runtimes[i];
	}
	work->emit                = emit;
	work->prim                = ucfg.GetPrimType();
	work->context             = buffer.GetRegisters();
	work->vertex_input        = vertex_stages.back();
	work->depth               = state.depth_info;
	work->rendering           = rendering;
	work->feedback            = feedback_aspects;
	work->feedback_enabled    = m_context.GetGraphics().attachment_feedback_loop_enabled;
	work->shader_writes       = shader_write_stages;
	work->guest_element_size  = index_source.guest_element_size;
	work->index_address       = index_source.address;
	work->stencil_depth_id    = stencil_depth_id;
	g_stencil_scratch.slot    = -1;

	auto record = [this, work] {
		auto& packet    = *work;
		auto  vk_buffer = packet.vk;
		SetDrawDebugPhase(*packet.buffer, packet.submit_id, packet.draw,
		                  packet.draw.IsIndexed() ? 0x100u : 0x200u);
		if (!packet.mesh_active) {
			CommitVertexBuffers(vk_buffer, packet.vertex_bindings);
		}
		if (packet.ps_active && !packet.draw.IsIndexed()) {
			SetDrawDebugPhase(*packet.buffer, packet.submit_id, packet.draw, 0x300u);
		}
		CommitBindings(*packet.buffer, vk::PipelineBindPoint::eGraphics, *packet.pipeline,
		               std::span {packet.stages.data(), packet.stage_count});
		if (packet.mesh_active) {
			const uint32_t draw_data[] {
			    packet.draw.index_count,
			    packet.draw.IsIndexed() ? static_cast<uint32_t>(packet.emit.vertex_offset)
			                            : packet.emit.first_vertex,
			    packet.emit.first_instance, packet.guest_element_size,
			    static_cast<uint32_t>(packet.index_address),
			    static_cast<uint32_t>(packet.index_address >> 32u)};
			static_assert(std::size(draw_data) == ShaderRecompiler::IR::PushData::MeshDrawDwordCount);
			vk_buffer.pushConstants(packet.pipeline->pipeline_layout,
			                        vk::ShaderStageFlagBits::eMeshEXT |
			                            vk::ShaderStageFlagBits::eFragment,
			                        0, sizeof(draw_data), draw_data);
		} else {
			CommitIndexBuffer(vk_buffer, packet.index_binding);
		}
		SetGraphicsDynamicParams(packet.buffer->GetGraphics(), packet.context, vk_buffer,
		                         packet.vertex_input, packet.depth, packet.rendering);
		if (packet.feedback_enabled) {
			vk_buffer.setAttachmentFeedbackLoopEnableEXT(packet.feedback);
		}
		if (!packet.draw.IsIndexed()) {
			SetDrawDebugPhase(*packet.buffer, packet.submit_id, packet.draw, 0x400u);
		}
		m_context.GetCommandScheduler().BeginRendering(packet.rendering);
		vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, packet.pipeline->pipeline);
		if (!packet.draw.IsIndexed()) {
			SetDrawDebugPhase(*packet.buffer, packet.submit_id, packet.draw, 0x500u);
		}
		m_context.GetCommandScheduler().ProfileConsumers(false);
		ProfilePreparedBufferUses(std::span {packet.stages.data(), packet.stage_count},
		                          "graphics", packet.submit_id);
		if (packet.mesh_active) {
			vk_buffer.drawMeshTasksEXT(packet.mesh_groups, packet.draw.instance_count, 1);
		} else {
			EmitDrawPrimitives(packet.prim, vk_buffer, packet.draw, packet.emit);
		}
		if (packet.stencil_depth_id) {
			auto& scheduler = m_context.GetCommandScheduler();
			auto& cache     = m_context.GetTextureCache();
			scheduler.EndRendering();
			auto& depth   = cache.GetImage(packet.stencil_depth_id);
			auto& utility = m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::DeviceLocal);
			StencilScratchBarrier(vk_buffer, vk::ImageLayout::eTransferSrcOptimal,
			                      vk::AccessFlagBits2::eTransferRead,
			                      vk::PipelineStageFlagBits2::eTransfer);
			depth.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
			              {}, vk_buffer);
			CopyStencilScratch(vk_buffer, depth, utility, false);
			cache.MarkGpuWritten(packet.stencil_depth_id);
		}
		if (BreadcrumbsOn()) {
			m_context.GetCommandScheduler().EndRendering();
			RecordBreadcrumb(m_context, vk_buffer);
		}
		m_context.GetCommandScheduler().ProfileWriterOperationEnd();
		if (!packet.draw.IsIndexed()) {
			SetDrawDebugPhase(*packet.buffer, packet.submit_id, packet.draw, 0x600u);
		}
		if (packet.shader_writes) {
			m_context.GetCommandScheduler().EndRendering();
			ShaderWriteBarrier(vk_buffer, packet.shader_writes);
		}
		if (!packet.draw.IsIndexed()) {
			SetDrawDebugPhase(*packet.buffer, packet.submit_id, packet.draw, 0x700u);
		}
		for (uint32_t i = 0; i < packet.stage_count; ++i) {
			if (packet.stages[i] != nullptr && packet.stages[i]->runtime == &packet.runtimes[i]) {
				packet.stages[i]->runtime = nullptr;
			}
		}
	};

	// The stencil write-back touches the texture cache, so it records on this thread. A traced draw records here too: its targets are
	// read back right after it.
	const bool pass_trace = VkPassTraceActive();
	if (DrawCommitEnabled() && !work->stencil_depth_id && !pass_trace) {
		DrawCommitGate::Get().Enqueue(std::move(record));
		m_commit_reset_bindings = true;
		return;
	}
	record();
	if (pass_trace) {
		std::vector<std::pair<ImageId, uint64_t>> outputs;
		for (uint32_t i = 0; i < state.color_count && i < RENDER_COLOR_ATTACHMENTS_MAX; ++i) {
			outputs.emplace_back(state.color_info[i].image_id, state.color_info[i].desc.info.data.address);
		}
		const auto* vs = state.vertex_info[0].stage.program;
		const auto* ps = state.ps_active ? state.ps_input_info.stage.program : nullptr;
		VkPassTraceReport(m_context, true, vs != nullptr ? vs->shader_hash : 0, ps != nullptr ? ps->shader_hash : 0, nullptr, outputs);
	}
}

void RenderExecutor::DrawIndex(uint64_t submit_id, CommandBuffer& buffer,
                               const DrawIndexArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	FinishDrawCommit();
	m_context.GetCommandScheduler().PopPendingOperations();
	m_context.GetCommandScheduler().NoteGpuCommand(m_context.DiagnosticFrameNum(), false);
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndex), submit_id,
	                    args.index_count, 0, 1, args.instance_count,
	                    reinterpret_cast<uint64_t>(args.index_addr));

	Common::LockGuard lock(m_context.GetMutex());
	if (args.index_count == 0 || args.instance_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer, args.render_target_slice_offset)) {
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndex():Shader:\n");
		uc_print("GraphicsRenderDrawIndex():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndex():Parameters:\n"
		     "\t index_type_and_size = 0x%08" PRIx32 "\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t index_addr          = 0x%016" PRIx64 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t base_vertex         = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.index_type_and_size, args.index_count,
		     reinterpret_cast<uint64_t>(args.index_addr), args.instance_count,
		     static_cast<uint32_t>(args.base_vertex), args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		return;
	}

	DrawIndexBufferSource index_source {};
	index_source.address = reinterpret_cast<uint64_t>(args.index_addr);
	switch (static_cast<Prospero::IndexType>(args.index_type_and_size)) {
		case Prospero::IndexType::kIndex16:
			index_source.type               = vk::IndexType::eUint16;
			index_source.guest_element_size = 2;
			break;
		case Prospero::IndexType::kIndex32:
			index_source.type               = vk::IndexType::eUint32;
			index_source.guest_element_size = 4;
			break;
		case Prospero::IndexType::kIndex8:
			index_source.guest_element_size = 1;
			break;
		default: EXIT("unknown index_type_and_size: %u\n", args.index_type_and_size);
	}
	index_source.size = static_cast<uint64_t>(args.index_count) * index_source.guest_element_size;
	const bool primitive_restart = ResolvePrimitiveRestart(buffer, index_source);

	std::vector<uint16_t> expanded_indices;
	if (index_source.guest_element_size == 1) {
		EXIT_NOT_IMPLEMENTED(args.index_addr == nullptr);
		const auto* src = static_cast<const uint8_t*>(args.index_addr);
		expanded_indices.resize(args.index_count);
		for (uint32_t i = 0; i < args.index_count; i++) {
			expanded_indices[i] = primitive_restart && src[i] == 0xffu ? 0xffffu : src[i];
		}
		index_source.host_data = expanded_indices.data();
		index_source.size      = expanded_indices.size() * sizeof(uint16_t);
	}

	const DrawCallInfo draw {CommandBufferDebugOp::DrawIndex, args.index_count,
	                        args.instance_count, args.first_instance};
	DrawRenderState state {};
	{
		CpuDrawPhaseTimer cpu_phase(m_context.GetCommandScheduler(), CommandScheduler::CpuDrawPhase::State);
		if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state)) {
			ResetBindings();
			return;
		}
	}

	LogDrawStateIfNeeded(buffer, draw, state, args.index_type_and_size,
	                     args.index_addr);

	const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
	const auto [vertex_offset, instance_offset] =
	    indirect ? std::pair<int32_t, uint32_t> {0, args.first_instance}
	             : ResolveDrawOffsets(ucfg.GetIndexOffset(), state.vertex_info[0]);

	DrawEmitInfo emit {};
	emit.vertex_offset  = vertex_offset + args.base_vertex;
	emit.first_instance = instance_offset;

	ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source,
	                    primitive_restart);
	if (!m_commit_reset_bindings) {
		ResetBindings();
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderExecutor::DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	FinishDrawCommit();
	m_context.GetCommandScheduler().PopPendingOperations();
	m_context.GetCommandScheduler().NoteGpuCommand(m_context.DiagnosticFrameNum(), false);
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndexAuto), submit_id,
	                    args.vertex_count, 0, args.first_vertex, args.instance_count,
	                    args.first_instance);

	Common::LockGuard lock(m_context.GetMutex());
	if (args.vertex_count == 0 || args.instance_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer, args.render_target_slice_offset)) {
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndexAuto():Shader:\n");
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndexAuto():Parameters:\n"
		     "\t vertex_count        = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t first_vertex        = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	const DrawCallInfo draw {CommandBufferDebugOp::DrawIndexAuto,
	                         args.vertex_count, args.instance_count, args.first_instance};

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		ResetBindings();
		return;
	}
	DrawRenderState state {};
	{
		CpuDrawPhaseTimer cpu_phase(m_context.GetCommandScheduler(), CommandScheduler::CpuDrawPhase::State);
		if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state)) {
			ResetBindings();
			return;
		}
	}

	const bool rect_list = Prospero::IsRectList(ucfg.GetPrimType());
	if (rect_list && state.vertex_info[0].buffers_num == 0 &&
	    state.vertex_info[0].stage.program->param_export_mask == 0 &&
	    state.ps_input_info.input_num != 0) {
		if (graphics_debug_dump_enabled()) {
			LOGF("DrawIndexAuto: skipping rect-list draw with no VS param exports and PS inputs: "
			     "ps_inputs=%u ps=0x%016" PRIx64 " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
			     state.ps_input_info.input_num, sh_ctx.GetPs().ps_regs.data_addr,
			     sh_ctx.GetVs().es_regs.data_addr, sh_ctx.GetVs().gs_regs.data_addr);
		}
		ResetBindings();
		return;
	}

	LogDrawStateIfNeeded(buffer, draw, state, 0, nullptr);

	const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
	const auto [vertex_offset, instance_offset] =
	    indirect ? std::pair<int32_t, uint32_t> {0, args.first_instance}
	             : ResolveDrawOffsets(ucfg.GetIndexOffset(), state.vertex_info[0]);
	DrawEmitInfo emit {};
	emit.first_vertex = static_cast<uint32_t>(vertex_offset + static_cast<int32_t>(args.first_vertex));
	emit.first_instance = instance_offset;

	DrawIndexBufferSource index_source {};
	ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source, false);
	if (!m_commit_reset_bindings) {
		ResetBindings();
	}
}

bool RenderExecutor::ResolveColorTargets(CommandBuffer& buffer, uint32_t render_target_slice_offset) {
	const auto& hw = buffer.GetRegisters();
	if (hw.GetColorControl().mode != 3) {
		return false;
	}

	const auto& src_rt = hw.GetRenderTarget(0);
	const auto& dst_rt = hw.GetRenderTarget(1);
	if (src_rt.base.addr == 0 || dst_rt.base.addr == 0) {
		return false;
	}

	RenderColorInfo src {};
	RenderColorInfo dst {};
	ResolveRenderColorTarget(buffer, src, render_target_slice_offset, 0, true, true);
	ResolveRenderColorTarget(buffer, dst, render_target_slice_offset, 1, true, true);
	if (!src.image_id || !dst.image_id) {
		return false;
	}
	if (src.desc.info.data.address == dst.desc.info.data.address &&
	    src.guest_mip_level == dst.guest_mip_level &&
	    src.guest_array_layer == dst.guest_array_layer) {
		return true;
	}

	auto& cache = m_context.GetTextureCache();
	cache.MarkGpuWritten(dst.image_id);
	auto& source      = cache.GetImage(src.image_id);
	auto& destination = cache.GetImage(dst.image_id);
	destination.Resolve(source, {src.guest_mip_level, 1, src.guest_array_layer, 1},
	                    {dst.guest_mip_level, 1, dst.guest_array_layer, 1});
	return true;
}

} // namespace Libs::Graphics
