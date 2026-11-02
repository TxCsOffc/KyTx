#include "graphics/host_gpu/gpuCaptureApple.h"
#include <sys/stat.h>
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_video_out_overlay_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include <bit>
#include "graphics/presentation/window/windowInternal.h"
#include "graphics/presentation/window/hostInput.h"

#if defined(__APPLE__)
#include "graphics/host_gpu/metal/metalPresenter.h"
#include "graphics/host_gpu/metal/metalRenderer.h"
#include "graphics/presentation/traceWindow.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <limits>
#include <memory>
#include <vector>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>


// KYTY_AUTO_INPUT="flip:key:hold,flip:key:hold,...": scripted key presses (key is one character or up/down/left/right/enter, hold is in
// presented flips), delivered to the host input layer at the given flip. Lets reproducible runs pass dialogs without a focused window.
// KYTY_AUTO_INPUT_FILE=<path>: while the game runs, a file at that path holding "key[:hold] key[:hold] ..." (e.g. "down:6 down:6 j:8") is
// consumed and played back one press after another, so a session can be driven interactively from outside.
static int AutoInputKeyCode(const std::string& name) {
	if (name == "up") return SDLK_UP;
	if (name == "down") return SDLK_DOWN;
	if (name == "left") return SDLK_LEFT;
	if (name == "right") return SDLK_RIGHT;
	if (name == "enter") return SDLK_RETURN;
	return name.size() == 1 ? static_cast<unsigned char>(name[0]) : 0;
}

static void AutoInputTick(uint64_t flip) {
	struct Press {
		uint64_t flip;
		int      key;
		uint64_t hold;
		bool     down_sent;
		bool     up_sent;
	};
	static std::vector<Press> presses = [] {
		std::vector<Press> v;
		if (const char* env = std::getenv("KYTY_AUTO_INPUT")) {
			const char* p = env;
			while (*p) {
				char*          end = nullptr;
				const uint64_t at  = std::strtoull(p, &end, 10);
				if (end == p || *end != ':' || end[1] == '\0') break;
				const char* key_end = std::strchr(end + 1, ':');
				if (key_end == nullptr) break;
				const int key = AutoInputKeyCode(std::string(end + 1, static_cast<size_t>(key_end - (end + 1))));
				if (key == 0) break;
				const uint64_t hold = std::strtoull(key_end + 1, &end, 10);
				v.push_back({at, key, hold == 0 ? 6 : hold, false, false});
				if (*end != ',') break;
				p = end + 1;
			}
		}
		return v;
	}();
	static const char* s_file = std::getenv("KYTY_AUTO_INPUT_FILE");
	if (s_file != nullptr && flip % 15 == 0) {
		if (std::FILE* f = std::fopen(s_file, "r")) {
			std::string text;
			char        buf[256];
			while (std::fgets(buf, sizeof(buf), f) != nullptr) text += buf;
			std::fclose(f);
			std::remove(s_file);
			uint64_t cursor = flip + 2;
			size_t   pos    = 0;
			while (pos < text.size()) {
				const size_t sp = text.find_first_of(" \t\r\n", pos);
				const size_t e  = sp == std::string::npos ? text.size() : sp;
				const std::string token = text.substr(pos, e - pos);
				pos                     = e + 1;
				if (token.empty()) continue;
				const size_t colon = token.find(':');
				const std::string name = token.substr(0, colon);
				uint64_t hold = colon == std::string::npos ? 6 : std::strtoull(token.c_str() + colon + 1, nullptr, 10);
				if (hold == 0) hold = 6;
				if (name == "wait") {
					cursor += hold;
					continue;
				}
				if (const int key = AutoInputKeyCode(name); key != 0) {
					presses.push_back({cursor, key, hold, false, false});
					cursor += hold + 8;
				}
			}
		}
	}
	for (auto& press: presses) {
		if (!press.down_sent && flip >= press.flip) {
			press.down_sent = true;
			std::fprintf(stderr, "[AutoInput] key 0x%x down at flip %llu\n", press.key, static_cast<unsigned long long>(flip));
			Libs::Graphics::HostInputKey(press.key, true);
		} else if (press.down_sent && !press.up_sent && flip >= press.flip + press.hold) {
			press.up_sent = true;
			Libs::Graphics::HostInputKey(press.key, false);
		}
	}
}

namespace Libs::Graphics {

std::atomic<uint64_t> g_present_wait_ns {0};

struct Presenter::Frame {
#if defined(__APPLE__)
	uint64_t metal_guest_vaddr = 0;
	uint32_t metal_width = 0;
	uint32_t metal_height = 0;
	uint32_t metal_pitch = 0;
	uint32_t metal_format = 0;
#endif
	VulkanImage   image;
	vk::ImageView view         = nullptr;
	uint64_t      present_tick = 0;
	bool          busy         = false;

	void Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format);
	void Transit(vk::CommandBuffer command, vk::ImageLayout layout, vk::AccessFlags2 access);
	void CopyFrom(CommandBuffer& command, Image& source);
	void Clear(CommandBuffer& command, const vk::ClearColorValue& color);
};

class FramePool final {
public:
	FramePool(WindowContext& window, CommandScheduler& scheduler)
	    : m_window(window), m_scheduler(scheduler) {}
	~FramePool() {
#if defined(__APPLE__)
		if (Metal::UseNativeMetalBackend()) {
			return;
		}
#endif
		m_scheduler.Wait(m_scheduler.CurrentTick() - 1);
		for (auto& frame: m_frames) {
			m_window.graphic_ctx.device.destroyImageView(frame->view, nullptr);
			if (frame->image.image != nullptr) {
				m_window.graphic_ctx.DeleteImage(frame->image);
			}
		}
	}
	KYTY_CLASS_NO_COPY(FramePool);

	void Initialize(uint32_t count, vk::Format format) {
		if (count == 0 || format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool requires at least one frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!m_frames.empty()) {
			EXIT("prepared-frame pool was initialized twice\n");
		}
		m_format = format;
		m_frames.reserve(count);
		m_free.reserve(count);
		for (uint32_t i = 0; i < count; i++) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
	}

	void SetFormat(vk::Format format) {
		if (format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool requires a presentation format\n");
		}
		Common::LockGuard lock(m_mutex);
		m_format = format;
	}

	vk::Format GetFormat() {
		Common::LockGuard lock(m_mutex);
		if (m_format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool has no presentation format\n");
		}
		return m_format;
	}

	Presenter::Frame* Acquire(vk::Extent2D extent, vk::Format format) {
		m_mutex.Lock();
		if (m_frames.empty()) {
			EXIT("prepared-frame pool was used before swapchain initialization\n");
		}
		// A synchronized flip may need more frames than the swapchain has images.
		// Queue capacity bounds growth; waiting here can prevent its master from recording.
		if (m_free.empty()) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
		const auto compatible = std::ranges::find_if(m_free, [&](const auto* frame) {
			return frame->image.extent.width == extent.width &&
			       frame->image.extent.height == extent.height && frame->image.format == format;
		});
		auto*      frame      = compatible == m_free.end() ? m_free.back() : *compatible;
		if (compatible != m_free.end()) {
			*compatible = m_free.back();
		}
		m_free.pop_back();
		if (frame->busy) {
			EXIT("prepared-frame pool returned an invalid frame\n");
		}
		frame->busy = true;
		m_mutex.Unlock();

		m_scheduler.Wait(frame->present_tick);
		return frame;
	}

	void ValidateForPresent(Presenter::Frame* frame) {
		Common::LockGuard lock(m_mutex);
		if (frame == nullptr || !frame->busy) {
			EXIT("prepared frame has invalid presentation ownership\n");
		}
	}

	void Release(Presenter::Frame* frame) {
		if (frame == nullptr) {
			EXIT("cannot release a null prepared frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!frame->busy) {
			EXIT("prepared frame was released twice\n");
		}
		frame->busy = false;
		m_free.push_back(frame);
	}

private:
	WindowContext&                                 m_window;
	CommandScheduler&                              m_scheduler;
	Common::Mutex                                  m_mutex;
	std::vector<std::unique_ptr<Presenter::Frame>> m_frames;
	std::vector<Presenter::Frame*>                 m_free;
	vk::Format                                     m_format = vk::Format::eUndefined;
};

void Presenter::Frame::Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format) {
	if (extent.width == 0 || extent.height == 0 || format == vk::Format::eUndefined) {
		EXIT("unsupported prepared frame, extent=%ux%u format=%d\n", extent.width, extent.height,
		     static_cast<int>(format));
	}
	auto&      dst        = image;
	const bool compatible = dst.image != nullptr && dst.extent.width == extent.width &&
	                        dst.extent.height == extent.height && dst.format == format;
	if (compatible) {
		return;
	}

	const auto features = graphics.GetFormatProperties(format).optimalTilingFeatures;
	const auto required =
	    vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
	    vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferSrc |
	    vk::FormatFeatureFlagBits::eTransferDst;
	if ((features & required) != required) {
		EXIT("prepared presentation format lacks optimal blit support: format=%d features=0x%x\n",
		     static_cast<int>(format), static_cast<vk::FormatFeatureFlags::MaskType>(features));
	}

	if (dst.image != nullptr) {
		graphics.device.destroyImageView(view, nullptr);
		view = nullptr;
		graphics.DeleteImage(dst);
	}

	vk::ImageCreateInfo create {};
	create.sType         = vk::StructureType::eImageCreateInfo;
	create.imageType     = vk::ImageType::e2D;
	create.extent        = {extent.width, extent.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
	               vk::ImageUsageFlagBits::eSampled;
	create.sharingMode = vk::SharingMode::eExclusive;
	create.samples     = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, dst)) {
		EXIT("failed to allocate prepared presentation image, extent=%ux%u format=%d\n",
		     extent.width, extent.height, static_cast<int>(format));
	}
}

void Presenter::Frame::Transit(vk::CommandBuffer command, vk::ImageLayout layout,
                               vk::AccessFlags2 access) {
	const auto     stage  = access == vk::AccessFlagBits2::eTransferRead ||
	                                access == vk::AccessFlagBits2::eTransferWrite
	                            ? vk::PipelineStageFlagBits2::eTransfer
	                            : vk::PipelineStageFlagBits2::eAllCommands;
	constexpr auto writes = vk::AccessFlagBits2::eTransferWrite |
	                        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eMemoryWrite;
	if (image.state.layout == layout && image.state.access_mask == access &&
	    !static_cast<bool>(image.state.access_mask & writes)) {
		return;
	}
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask                    = image.state.pl_stage;
	barrier.srcAccessMask                   = image.state.access_mask;
	barrier.dstStageMask                    = stage;
	barrier.dstAccessMask                   = access;
	barrier.oldLayout                       = image.state.layout;
	barrier.newLayout                       = layout;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = image.image;
	barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel   = 0;
	barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	ProfilePipelineBarrier2(command, dependency);
	image.state = {stage, access, layout};
	image.subresource_states.clear();
}

void Presenter::Frame::CopyFrom(CommandBuffer& command_buffer, Image& source) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, source.backing.layers};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, image.layers};
	copy.extent         = {std::min(source.backing.extent.width, image.extent.width),
	                       std::min(source.backing.extent.height, image.extent.height), 1};
	EXIT_IF(copy.srcSubresource.layerCount != copy.dstSubresource.layerCount);
	auto* copy_scheduler = ProfileThread().scheduler;
	const auto copy_timer = copy_scheduler ? copy_scheduler->StartGpuTimer("copy_present_image", copy_scheduler->GpuFrameHint()) : UINT32_MAX;
	command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, image.image,
	                  vk::ImageLayout::eTransferDstOptimal, copy);
	if (copy_scheduler) copy_scheduler->EndGpuTimer(copy_timer);
}

void Presenter::Frame::Clear(CommandBuffer& command_buffer, const vk::ClearColorValue& color) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.clearColorImage(image.image, vk::ImageLayout::eTransferDstOptimal, &color, 1, &range);
}

class Swapchain final {
public:
	enum class Status : uint8_t { Success, Recreate, SurfaceLost };

	explicit Swapchain(WindowContext& window): m_window(window) {}
	~Swapchain();
	KYTY_CLASS_NO_COPY(Swapchain);

	void                 Create();
	void                 Recreate(bool surface_lost = false);
	[[nodiscard]] bool   NeedsResize() const;
	[[nodiscard]] Status AcquireNextImage();
	[[nodiscard]] bool   PrepareSystemOverlay();
	void     RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
	                               const Presenter::Layer& overlay, bool draw_system_overlay);
	uint64_t Submit(CommandScheduler& scheduler);
	[[nodiscard]] Status Present();

	[[nodiscard]] uint32_t ImageCount() const noexcept {
		return static_cast<uint32_t>(m_images.size());
	}
	[[nodiscard]] vk::Format Format() const noexcept { return m_format; }

private:
	void Destroy();
	void DrawOverlay(vk::CommandBuffer command, const Presenter::Layer& layer);

	WindowContext&   m_window;
	vk::SwapchainKHR m_handle = nullptr;
	vk::Format       m_format = vk::Format::eUndefined;
	vk::Extent2D     m_extent {};
	// Drawable pixel size observed when this swapchain was created.
	vk::Extent2D                   m_window_extent {};
	std::vector<vk::Image>         m_images;
	std::vector<vk::ImageView>     m_image_views;
	std::vector<vk::Semaphore>     m_image_acquired;
	std::vector<vk::Semaphore>     m_render_complete;
	std::unique_ptr<SystemOverlay> m_system_overlay;
	vk::DescriptorSetLayout        m_overlay_descriptors = nullptr;
	vk::PipelineLayout             m_overlay_layout      = nullptr;
	vk::Pipeline                   m_overlay_pipeline    = nullptr;
	vk::Sampler                    m_overlay_sampler     = nullptr;
	uint32_t                       m_image_index         = static_cast<uint32_t>(-1);
	uint32_t                       m_frame_index         = 0;
};

struct Presenter::Impl {
	explicit Impl(WindowContext& owner)
	    : renderer(*owner.render_context), window(owner), swapchain(owner),
	      present_scheduler(renderer, owner.graphic_ctx, "present"), frames(owner, present_scheduler) {
		EXIT_IF(owner.render_context == nullptr);
#if defined(__APPLE__)
		if (Metal::UseNativeMetalBackend()) {
			frames.Initialize(3, vk::Format::eB8G8R8A8Unorm);
			return;
		}
#endif
		swapchain.Create();
		frames.Initialize(swapchain.ImageCount(), swapchain.Format());
	}

	void RecoverSwapchain(Swapchain::Status status) {
		LOGF("Recovering Vulkan swapchain%s\n",
		     status == Swapchain::Status::SurfaceLost ? " and surface" : "");
		swapchain.Recreate(status == Swapchain::Status::SurfaceLost);
		frames.SetFormat(swapchain.Format());
	}

	Image& ResolveSurface(const ImageInfo& info) {
		TextureCache::ImageDesc desc {};
		desc.info                  = info;
		desc.view_info.format      = info.pixel_format;
		desc.view_info.type        = vk::ImageViewType::e2D;
		desc.view_info.aspect      = vk::ImageAspectFlagBits::eColor;
		desc.view_info.base_level  = 0;
		desc.view_info.level_count = 1;
		desc.view_info.base_layer  = 0;
		desc.view_info.layer_count = 1;
		desc.view_info.usage       = vk::ImageUsageFlagBits::eTransferSrc;
		desc.type                  = TextureCache::BindingType::VideoOut;

		auto&      cache      = renderer.GetTextureCache();
		const auto image_id   = cache.FindImage(desc);
		auto&      image      = cache.GetImage(image_id);
		image.usage.video_out = true;
		{
			// KYTY_ORACLE_FLIP=1: first 20 flips, which texture the flip address resolves to and how it was last written.
			static const bool s_oracle = std::getenv("KYTY_ORACLE_FLIP") != nullptr;
			static std::atomic<uint32_t> s_oracle_n {0};
			if (s_oracle && s_oracle_n.fetch_add(1) < 20) {
				const auto& w = image.last_write;
				std::fprintf(stderr,
				             "[Oracle] flip=0x%llx size=%llu image=0x%llx size=%llu pixel_fmt=%d guest_fmt=%d write_kind=%u write_shader=0x%llx write_serial=%llu\n",
				             (unsigned long long)info.data.address, (unsigned long long)info.data.size,
				             (unsigned long long)image.info.data.address, (unsigned long long)image.info.data.size,
				             static_cast<int>(info.pixel_format), static_cast<int>(image.info.guest_format),
				             static_cast<unsigned>(w.kind), (unsigned long long)w.shader, (unsigned long long)w.serial);
			}
		}
		cache.UpdateImage(image_id);
		return image;
	}
	void Present();

	RenderContext&        renderer;
	WindowContext&        window;
	Swapchain             swapchain;
	CommandScheduler      present_scheduler;
	FramePool             frames;
	Common::Mutex         present_mutex;
	std::array<Layer, 2>  layers {};
	std::atomic<uint64_t> presented_overlay_revision {0};
};

void Swapchain::Create() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		m_images.resize(3);
		m_format = vk::Format::eB8G8R8A8Unorm;
		return;
	}
#endif
	auto& graphics = m_window.graphic_ctx;
	EXIT_IF(graphics.device == nullptr);
	EXIT_IF(m_window.surface == nullptr);

	// Capture the size before querying surface capabilities. If the window changes
	// during creation, the next presentation will detect the newer size.
	{
		Common::LockGuard lock(m_window.mutex);
		m_window_extent = {graphics.screen_width, graphics.screen_height};
	}
	EXIT_IF(m_window_extent.width == 0);
	EXIT_IF(m_window_extent.height == 0);
	m_window.RefreshSurfaceCapabilities();
	const auto& surface = m_window.surface_capabilities;
	EXIT_NOT_IMPLEMENTED(surface.formats.empty());

	m_extent = surface.capabilities.currentExtent;
	if (m_extent.width == std::numeric_limits<uint32_t>::max()) {
		m_extent.width =
		    std::clamp(m_window_extent.width, surface.capabilities.minImageExtent.width,
		               surface.capabilities.maxImageExtent.width);
		m_extent.height =
		    std::clamp(m_window_extent.height, surface.capabilities.minImageExtent.height,
		               surface.capabilities.maxImageExtent.height);
	}
	uint32_t image_count = surface.capabilities.minImageCount + 1;
	if (surface.capabilities.maxImageCount != 0) {
		image_count = std::min(image_count, surface.capabilities.maxImageCount);
	}
	const auto transform =
	    surface.capabilities.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity
	        ? vk::SurfaceTransformFlagBitsKHR::eIdentity
	        : surface.capabilities.currentTransform;
	const auto composite =
	    surface.capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque
	        ? vk::CompositeAlphaFlagBitsKHR::eOpaque
	        : vk::CompositeAlphaFlagBitsKHR::eInherit;

	vk::SurfaceFormatKHR format {vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
	if (surface.formats.size() != 1 || surface.formats.front().format != vk::Format::eUndefined) {
		const auto it = std::find_if(surface.formats.begin(), surface.formats.end(),
		                             [](const vk::SurfaceFormatKHR& candidate) {
			                             return candidate.format == vk::Format::eB8G8R8A8Unorm ||
			                                    candidate.format == vk::Format::eR8G8B8A8Unorm;
		                             });
		if (it == surface.formats.end()) {
			EXIT("no supported UNORM swapchain format\n");
		}
		format = *it;
	}
	m_format                      = format.format;
	const auto swapchain_features = graphics.GetFormatProperties(m_format).optimalTilingFeatures;
	if (!static_cast<bool>(swapchain_features & vk::FormatFeatureFlagBits::eBlitDst)) {
		EXIT("swapchain format cannot be a blit destination: format=%d\n",
		     static_cast<int>(m_format));
	}

	vk::SwapchainCreateInfoKHR create_info {};
	create_info.sType            = vk::StructureType::eSwapchainCreateInfoKHR;
	create_info.surface          = m_window.surface;
	create_info.minImageCount    = image_count;
	create_info.imageFormat      = format.format;
	create_info.imageColorSpace  = format.colorSpace;
	create_info.imageExtent      = m_extent;
	create_info.imageArrayLayers = 1;
	create_info.imageUsage =
	    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
	create_info.imageSharingMode = vk::SharingMode::eExclusive;
	create_info.preTransform     = transform;
	create_info.compositeAlpha   = composite;
	switch (Config::GetPresentMode()) {
		case Config::PresentMode::Mailbox:
			create_info.presentMode = vk::PresentModeKHR::eMailbox;
			break;
		case Config::PresentMode::Immediate:
			create_info.presentMode = vk::PresentModeKHR::eImmediate;
			break;
		case Config::PresentMode::Fifo:
		default: create_info.presentMode = vk::PresentModeKHR::eFifo; break;
	}
	if (std::find(surface.present_modes.begin(), surface.present_modes.end(),
	              create_info.presentMode) == surface.present_modes.end()) {
		LOGF("warning: requested present mode is unavailable; falling back to Fifo\n");
		create_info.presentMode = vk::PresentModeKHR::eFifo;
	}
	create_info.clipped = VK_TRUE;
	RequireVulkanSuccess(graphics.device.createSwapchainKHR(&create_info, nullptr, &m_handle),
	                     "vkCreateSwapchainKHR");
	EXIT_IF(m_handle == nullptr);

	m_images = EnumerateVulkan<vk::Image>(
	    "vkGetSwapchainImagesKHR", [&](uint32_t* count, vk::Image* images) {
		    return graphics.device.getSwapchainImagesKHR(m_handle, count, images);
	    });
	EXIT_NOT_IMPLEMENTED(m_images.empty());

	m_image_views.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		vk::ImageViewCreateInfo view {};
		view.sType                           = vk::StructureType::eImageViewCreateInfo;
		view.image                           = m_images[i];
		view.viewType                        = vk::ImageViewType::e2D;
		view.format                          = m_format;
		view.components                      = {};
		view.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
		view.subresourceRange.baseArrayLayer = 0;
		view.subresourceRange.baseMipLevel   = 0;
		view.subresourceRange.layerCount     = 1;
		view.subresourceRange.levelCount     = 1;
		RequireVulkanSuccess(graphics.device.createImageView(&view, nullptr, &m_image_views[i]),
		                     "vkCreateImageView");
		EXIT_IF(m_image_views[i] == nullptr);
	}

	vk::SemaphoreCreateInfo semaphore_info {};
	semaphore_info.sType = vk::StructureType::eSemaphoreCreateInfo;
	m_image_acquired.resize(m_images.size());
	m_render_complete.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_image_acquired[i]),
		    "create swapchain image-acquired semaphore");
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_render_complete[i]),
		    "create swapchain render-complete semaphore");
	}
	m_image_index = static_cast<uint32_t>(-1);
	m_frame_index = 0;
}

Swapchain::~Swapchain() {
	Destroy();
}

void Swapchain::Destroy() {
	if (m_handle == nullptr && m_image_acquired.empty() && m_render_complete.empty() &&
	    m_image_views.empty()) {
		return;
	}
	auto& graphics = m_window.graphic_ctx;

	{
		Common::LockGuard queue_lock(graphics.queue_mutex);
		auto* profiler = m_window.render_context ? &m_window.render_context->GetCommandScheduler() : nullptr;
		const auto start = profiler && profiler->ProfileActive() ? ProfileClockNs() : 0;
		const auto frame = start ? profiler->ProfileFrame() : 0;
		RequireVulkanSuccess(graphics.queue.waitIdle(), "wait for swapchain queue");
		if (start) profiler->ProfileEvent("queue_wait_idle", start, ProfileClockNs(),
		    std::bit_cast<uint64_t>(graphics.queue), 0, std::source_location::current(), frame);
	}
	if (m_system_overlay != nullptr) {
		m_system_overlay->ReleaseVulkan();
	}
	graphics.device.destroyPipeline(m_overlay_pipeline, nullptr);
	graphics.device.destroyPipelineLayout(m_overlay_layout, nullptr);
	graphics.device.destroyDescriptorSetLayout(m_overlay_descriptors, nullptr);
	graphics.device.destroySampler(m_overlay_sampler, nullptr);
	m_overlay_pipeline    = nullptr;
	m_overlay_layout      = nullptr;
	m_overlay_descriptors = nullptr;
	m_overlay_sampler     = nullptr;

	for (const auto semaphore: m_image_acquired) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto semaphore: m_render_complete) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto view: m_image_views) {
		if (view != nullptr) {
			graphics.device.destroyImageView(view, nullptr);
		}
	}
	if (m_handle != nullptr) {
		graphics.device.destroySwapchainKHR(m_handle, nullptr);
	}

	m_handle        = nullptr;
	m_format        = vk::Format::eUndefined;
	m_extent        = {};
	m_window_extent = {};
	m_image_index   = static_cast<uint32_t>(-1);
	m_frame_index   = 0;
	m_images.clear();
	m_image_views.clear();
	m_image_acquired.clear();
	m_render_complete.clear();
}

void Swapchain::Recreate(bool surface_lost) {
	Destroy();
	if (surface_lost) {
#if defined(__APPLE__)
		// Surface recreation goes through SDL_Vulkan_CreateSurface, which touches the
		// window's view/layer and must run on the main thread on macOS.
		EXIT_IF(!SDL_RunOnMainThread(
		    [](void* window) { static_cast<WindowContext*>(window)->RecreateSurface(); }, &m_window,
		    true));
#else
		m_window.RecreateSurface();
#endif
	}
	Create();
}

bool Swapchain::NeedsResize() const {
	Common::LockGuard lock(m_window.mutex);
	return m_window_extent.width != m_window.graphic_ctx.screen_width ||
	       m_window_extent.height != m_window.graphic_ctx.screen_height;
}

Swapchain::Status Swapchain::AcquireNextImage() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return Status::Success;
	}
#endif
	EXIT_IF(m_handle == nullptr || m_frame_index >= m_image_acquired.size());
	m_image_index     = static_cast<uint32_t>(-1);
	auto* profiler = ProfileThread().scheduler;
	const auto profile_start = profiler && profiler->ProfileActive() ? ProfileClockNs() : 0;
	const auto profile_frame = profile_start ? profiler->ProfileFrame() : 0;
	const auto result = m_window.graphic_ctx.device.acquireNextImageKHR(
	    m_handle, std::numeric_limits<uint64_t>::max(), m_image_acquired[m_frame_index], nullptr,
	    &m_image_index);
	if (profile_start) profiler->ProfileEvent("acquire_next_image_wait", profile_start, ProfileClockNs(),
	    std::bit_cast<uint64_t>(m_handle), 0, std::source_location::current(), profile_frame);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorUnknown:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorUnknown\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", vk::to_string(result).c_str());
	}
	EXIT_IF(m_image_index >= m_images.size());
	return Status::Success;
}

bool Swapchain::PrepareSystemOverlay() {
	if (m_system_overlay == nullptr) {
		m_system_overlay = std::make_unique<SystemOverlay>(m_window.graphic_ctx);
	}
	return m_system_overlay->PrepareFrame(m_extent, m_format, ImageCount());
}

void Swapchain::DrawOverlay(vk::CommandBuffer command, const Presenter::Layer& layer) {
	auto device = m_window.graphic_ctx.device;
	if (m_overlay_pipeline == nullptr) {
		vk::SamplerCreateInfo sampler {};
		sampler.magFilter    = vk::Filter::eLinear;
		sampler.minFilter    = vk::Filter::eLinear;
		sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		RequireVulkanSuccess(device.createSampler(&sampler, nullptr, &m_overlay_sampler),
		                     "create video-out overlay sampler");
		const vk::DescriptorSetLayoutBinding binding {0, vk::DescriptorType::eCombinedImageSampler,
		                                              1, vk::ShaderStageFlagBits::eFragment};
		vk::DescriptorSetLayoutCreateInfo    descriptors {};
		descriptors.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		descriptors.bindingCount = 1;
		descriptors.pBindings    = &binding;
		RequireVulkanSuccess(
		    device.createDescriptorSetLayout(&descriptors, nullptr, &m_overlay_descriptors),
		    "create video-out overlay descriptor layout");
		const vk::PushConstantRange alpha {vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32_t)};
		vk::PipelineLayoutCreateInfo layout {};
		layout.setLayoutCount         = 1;
		layout.pSetLayouts            = &m_overlay_descriptors;
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges    = &alpha;
		RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &m_overlay_layout),
		                     "create video-out overlay pipeline layout");
		const auto vertex   = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, device);
		const auto fragment = CompileSPV(GPU_VIDEO_OUT_OVERLAY_SPV, device);
		std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
		stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
		stages[0].module = vertex;
		stages[0].pName  = "main";
		stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
		stages[1].module = fragment;
		stages[1].pName  = "main";
		vk::PipelineVertexInputStateCreateInfo   vertex_input {};
		vk::PipelineInputAssemblyStateCreateInfo assembly {};
		assembly.topology = vk::PrimitiveTopology::eTriangleList;
		vk::PipelineViewportStateCreateInfo viewport {};
		viewport.viewportCount = 1;
		viewport.scissorCount  = 1;
		vk::PipelineRasterizationStateCreateInfo rasterization {};
		rasterization.lineWidth = 1.0f;
		vk::PipelineMultisampleStateCreateInfo multisample {};
		multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
		vk::PipelineColorBlendAttachmentState attachment {};
		attachment.blendEnable         = VK_TRUE;
		attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
		attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
		attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.colorWriteMask = vk::ColorComponentFlagBits::eR |
		                            vk::ColorComponentFlagBits::eG |
		                            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
		vk::PipelineColorBlendStateCreateInfo blend {};
		blend.attachmentCount = 1;
		blend.pAttachments    = &attachment;
		const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
		vk::PipelineDynamicStateCreateInfo dynamic {};
		dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
		dynamic.pDynamicStates    = dynamic_states.data();
		vk::PipelineRenderingCreateInfo rendering {};
		rendering.colorAttachmentCount    = 1;
		rendering.pColorAttachmentFormats = &m_format;
		vk::GraphicsPipelineCreateInfo create {};
		create.pNext               = &rendering;
		create.stageCount          = static_cast<uint32_t>(stages.size());
		create.pStages             = stages.data();
		create.pVertexInputState   = &vertex_input;
		create.pInputAssemblyState = &assembly;
		create.pViewportState      = &viewport;
		create.pRasterizationState = &rasterization;
		create.pMultisampleState   = &multisample;
		create.pColorBlendState    = &blend;
		create.pDynamicState       = &dynamic;
		create.layout              = m_overlay_layout;
		RequireVulkanSuccess(
		    device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &m_overlay_pipeline),
		    "create video-out overlay pipeline");
		device.destroyShaderModule(fragment, nullptr);
		device.destroyShaderModule(vertex, nullptr);
	}
	auto& frame = *layer.frame;
	if (frame.view == nullptr) {
		vk::ImageViewCreateInfo view {};
		view.image            = frame.image.image;
		view.viewType         = vk::ImageViewType::e2D;
		view.format           = frame.image.format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		RequireVulkanSuccess(device.createImageView(&view, nullptr, &frame.view),
		                     "create video-out overlay image view");
	}
	const vk::DescriptorImageInfo image {m_overlay_sampler, frame.view,
	                                     vk::ImageLayout::eShaderReadOnlyOptimal};
	vk::WriteDescriptorSet        write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
	write.pImageInfo      = &image;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_overlay_layout, 0, 1, &write);
	const uint32_t premultiplied = layer.premultiplied_alpha;
	command.pushConstants(m_overlay_layout, vk::ShaderStageFlagBits::eFragment, 0,
	                      sizeof(premultiplied), &premultiplied);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, m_overlay_pipeline);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = m_image_views[m_image_index];
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eLoad;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = m_extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	const vk::Viewport viewport {
	    0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height),
	    0.0f, 1.0f};
	const vk::Rect2D scissor {{0, 0}, m_extent};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

void Swapchain::RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
                                      const Presenter::Layer& overlay, bool draw_system_overlay) {
	EXIT_IF(m_image_index >= m_images.size());
	auto       vk_command      = command.Handle();
	const bool draw_overlay    = overlay.frame != nullptr;
	const bool draw_attachment = draw_overlay || draw_system_overlay;
	if (source != nullptr) {
		source->Transit(vk_command, vk::ImageLayout::eTransferSrcOptimal,
		                vk::AccessFlagBits2::eTransferRead);
	}
	if (draw_overlay) {
		overlay.frame->Transit(vk_command, vk::ImageLayout::eShaderReadOnlyOptimal,
		                       vk::AccessFlagBits2::eShaderRead);
	}

	vk::ImageMemoryBarrier to_transfer {};
	to_transfer.sType                           = vk::StructureType::eImageMemoryBarrier;
	to_transfer.dstAccessMask                   = vk::AccessFlagBits::eTransferWrite;
	to_transfer.oldLayout                       = vk::ImageLayout::eUndefined;
	to_transfer.newLayout                       = vk::ImageLayout::eTransferDstOptimal;
	to_transfer.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.image                           = m_images[m_image_index];
	to_transfer.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_transfer.subresourceRange.baseMipLevel   = 0;
	to_transfer.subresourceRange.levelCount     = 1;
	to_transfer.subresourceRange.baseArrayLayer = 0;
	to_transfer.subresourceRange.layerCount     = 1;
	// Match the acquire wait stage so the layout transition cannot precede acquisition.
	ProfilePipelineBarrier(vk_command, vk::PipelineStageFlagBits::eTransfer,
	                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags {}, 0,
	                           nullptr, 0, nullptr, 1, &to_transfer);

	if (source != nullptr) {
		vk::ImageBlit region {};
		region.srcSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.srcSubresource.mipLevel       = 0;
		region.srcSubresource.baseArrayLayer = 0;
		region.srcSubresource.layerCount     = 1;
		region.srcOffsets[1].x               = static_cast<int>(source->image.extent.width);
		region.srcOffsets[1].y               = static_cast<int>(source->image.extent.height);
		region.srcOffsets[1].z               = 1;
		region.dstSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.dstSubresource.mipLevel       = 0;
		region.dstSubresource.baseArrayLayer = 0;
		region.dstSubresource.layerCount     = 1;
		region.dstOffsets[1].x               = static_cast<int>(m_extent.width);
		region.dstOffsets[1].y               = static_cast<int>(m_extent.height);
		region.dstOffsets[1].z               = 1;
		auto* profiler = ProfileThread().scheduler;
		const auto blit_timer = profiler ? profiler->StartGpuTimer("copy_present_blit", profiler->GpuFrameHint()) : UINT32_MAX;
		vk_command.blitImage(source->image.image, vk::ImageLayout::eTransferSrcOptimal,
		                     m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal, 1,
		                     &region, vk::Filter::eLinear);
		if (profiler) profiler->EndGpuTimer(blit_timer);
	} else {
		const vk::ClearColorValue black {};
		vk_command.clearColorImage(m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal,
		                           &black, 1, &to_transfer.subresourceRange);
	}

	vk::ImageMemoryBarrier to_present {};
	to_present.sType               = vk::StructureType::eImageMemoryBarrier;
	to_present.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	to_present.dstAccessMask       = draw_attachment ? vk::AccessFlagBits::eColorAttachmentRead |
	                                                       vk::AccessFlagBits::eColorAttachmentWrite
	                                                 : vk::AccessFlagBits::eMemoryRead;
	to_present.oldLayout           = vk::ImageLayout::eTransferDstOptimal;
	to_present.newLayout           = draw_attachment ? vk::ImageLayout::eColorAttachmentOptimal
	                                                 : vk::ImageLayout::ePresentSrcKHR;
	to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.image               = m_images[m_image_index];
	to_present.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_present.subresourceRange.baseMipLevel   = 0;
	to_present.subresourceRange.levelCount     = 1;
	to_present.subresourceRange.baseArrayLayer = 0;
	to_present.subresourceRange.layerCount     = 1;
	ProfilePipelineBarrier(vk_command, vk::PipelineStageFlagBits::eTransfer,
	                           draw_attachment ? vk::PipelineStageFlagBits::eColorAttachmentOutput
	                                           : vk::PipelineStageFlagBits::eAllCommands,
	                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
	                           &to_present);
	if (draw_attachment) {
		if (draw_overlay) {
			DrawOverlay(vk_command, overlay);
			if (draw_system_overlay) {
				to_present.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
				to_present.oldLayout     = vk::ImageLayout::eColorAttachmentOptimal;
				ProfilePipelineBarrier(vk_command, vk::PipelineStageFlagBits::eColorAttachmentOutput,
				                           vk::PipelineStageFlagBits::eColorAttachmentOutput,
				                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0,
				                           nullptr, 1, &to_present);
			}
		}
		if (draw_system_overlay) {
			m_system_overlay->Record(vk_command, m_image_views[m_image_index]);
		}
		to_present.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
		to_present.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
		to_present.oldLayout     = vk::ImageLayout::eColorAttachmentOptimal;
		to_present.newLayout     = vk::ImageLayout::ePresentSrcKHR;
		ProfilePipelineBarrier(vk_command, vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                           vk::PipelineStageFlagBits::eAllCommands,
		                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
		                           &to_present);
	}
}

uint64_t Swapchain::Submit(CommandScheduler& scheduler) {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return 1;
	}
#endif
	EXIT_IF(m_frame_index >= m_image_acquired.size() || m_image_index >= m_render_complete.size());
	SubmitInfo submit;
	submit.AddWait(m_image_acquired[m_frame_index], 1, vk::PipelineStageFlagBits::eTransfer);
	submit.AddSignal(m_render_complete[m_image_index]);
	return scheduler.Submit(submit);
}

Swapchain::Status Swapchain::Present() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return Status::Success;
	}
#endif
	EXIT_IF(m_image_index >= m_render_complete.size());
	const auto         ready = m_render_complete[m_image_index];
	vk::PresentInfoKHR present {};
	present.sType              = vk::StructureType::ePresentInfoKHR;
	present.swapchainCount     = 1;
	present.pSwapchains        = &m_handle;
	present.pImageIndices      = &m_image_index;
	present.pWaitSemaphores    = &ready;
	present.waitSemaphoreCount = 1;

	auto* profiler = ProfileThread().scheduler;
	const auto start = profiler && profiler->ProfileActive() ? ProfileClockNs() : 0;
	const auto frame = start ? profiler->ProfileFrame() : 0;
	vk::Result result;
	{
		const auto present_begin = std::chrono::steady_clock::now();
		Common::LockGuard lock(m_window.graphic_ctx.queue_mutex);
		result = m_window.graphic_ctx.queue.presentKHR(&present);
		const auto present_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
		    std::chrono::steady_clock::now() - present_begin).count();
		g_present_wait_ns.fetch_add(present_elapsed, std::memory_order_relaxed);
	}
	GpuCaptureOnPresent();
	if (start) profiler->ProfileEvent("queue_present_cpu", start, ProfileClockNs(),
	    std::bit_cast<uint64_t>(m_window.graphic_ctx.queue), 0, std::source_location::current(), frame);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkQueuePresentKHR failed: %s\n", vk::to_string(result).c_str());
	}
	m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
	return Status::Success;
}

Presenter::Presenter(WindowContext& window): m_impl(std::make_unique<Impl>(window)) {}

Presenter::~Presenter() = default;

Presenter::Frame& Presenter::PrepareFrame(CommandBuffer& buffer, const ImageInfo& info) {
	KYTY_PROFILER_FUNCTION();
	TraceWindow::OnFlip();
#if defined(__APPLE__)
	if (!Metal::UseNativeMetalBackend()) {
		Metal::VkGpuCaptureTick(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed) - 1);
		// The Vulkan reference run (visual oracle) needs the same scripted input to get past the boot dialogs.
		AutoInputTick(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed));
	}
	if (Metal::UseNativeMetalBackend()) {
		auto frame_format = info.pixel_format;
		auto* frame = m_impl->frames.Acquire({info.extent.width, info.extent.height}, frame_format);
		Metal::MetalRenderer::Instance().CaptureFrameTick(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed) - 1);
		Metal::MetalDrawFrameStatsTick(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed));
		AutoInputTick(Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed));
		frame->metal_guest_vaddr = info.data.address;
		frame->metal_width = info.extent.width;
		frame->metal_height = info.extent.height;
		frame->metal_pitch = (info.pitch >= info.extent.width * 4) ? info.pitch : (info.pitch > 0 ? info.pitch * 4 : info.extent.width * 4);
		frame->metal_format = static_cast<uint32_t>(info.pixel_format);
		Metal::RegisterFlipBuffer(info.data.address,
		                          static_cast<uint64_t>(frame->metal_pitch) * info.extent.height);

		static std::atomic<uint32_t> s_prep_cnt {0};
		uint32_t pc = s_prep_cnt.fetch_add(1, std::memory_order_relaxed);
		if (pc < 10 || pc % 120 == 0) {
			std::fprintf(stderr, "[Swapchain] PrepareFrame #%u: vaddr=0x%llx extent=%ux%u fmt=%u pitch=%u\n",
			             pc, (unsigned long long)info.data.address, info.extent.width, info.extent.height,
			             (unsigned int)info.pixel_format, info.pitch);
		}
		static const bool s_trace_flip = std::getenv("KYTY_TRACE_FLIP_WRITES") != nullptr;
		if (s_trace_flip && pc % 60 == 0 && info.data.address != 0) {
			// Guest-CPU writes into the flip buffer (CPU side, before any GPU work).
			const size_t words = static_cast<size_t>(frame->metal_pitch) * info.extent.height / 4;
			const auto*  p32   = reinterpret_cast<const uint32_t*>(info.data.address);
			size_t       nz    = 0;
			for (size_t i = 0; i < words; ++i) {
				nz += (p32[i] != 0);
			}
			std::fprintf(stderr, "[FlipMem] #%u addr=0x%llx nz_words=%zu/%zu\n", pc,
			             (unsigned long long)info.data.address, nz, words);
		}
		return *frame;
	}
#endif
	EXIT_IF(buffer.IsInvalid());
	auto frame_format = info.pixel_format;
	switch (frame_format) {
		case vk::Format::eR8G8B8A8Srgb: frame_format = vk::Format::eR8G8B8A8Unorm; break;
		case vk::Format::eB8G8R8A8Srgb: frame_format = vk::Format::eB8G8R8A8Unorm; break;
		default: break;
	}
	auto* frame = m_impl->frames.Acquire({info.extent.width, info.extent.height}, frame_format);
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
	auto&             image = m_impl->ResolveSurface(info);
	if (image.backing.format == vk::Format::eUndefined) {
		EXIT("unsupported presentation source, image=%p\n", static_cast<const void*>(&image));
	}
	frame->Configure(m_impl->window.graphic_ctx,
	                 {image.backing.extent.width, image.backing.extent.height}, frame_format);
	frame->CopyFrom(buffer, image);
	return *frame;
}

Presenter::Frame& Presenter::PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
                                               CommandBuffer* producer) {
	KYTY_PROFILER_FUNCTION();
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		auto* frame = m_impl->frames.Acquire({width, height}, m_impl->frames.GetFormat());
		frame->metal_guest_vaddr = 0;
		frame->metal_width = width;
		frame->metal_height = height;
		frame->metal_pitch = width * 4;
		frame->metal_format = 0;
		return *frame;
	}
#endif
	auto              format = m_impl->frames.GetFormat();
	auto*             frame  = m_impl->frames.Acquire({width, height}, format);
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
	frame->Configure(m_impl->window.graphic_ctx, {width, height}, format);
	vk::ClearColorValue clear {};
	clear.float32[3] = opaque ? 1.0f : 0.0f;
	if (producer != nullptr) {
		EXIT_IF(producer->IsInvalid());
		frame->Clear(*producer, clear);
	} else {
		auto& command = m_impl->present_scheduler.BeginCommand();
		frame->Clear(command, clear);
		frame->present_tick = m_impl->present_scheduler.Submit();
	}
	return *frame;
}

bool Presenter::PresentLastFrame() {
	Common::LockGuard lock(m_impl->present_mutex);
	if (m_impl->layers[0].frame == nullptr && m_impl->layers[1].frame == nullptr) {
		return false;
	}
	m_impl->Present();
	return true;
}

bool Presenter::IsGuestPaused() const noexcept {
	return m_impl->window.loop.paused.load(std::memory_order_acquire);
}

bool Presenter::NeedsSystemOverlayRefresh() const noexcept {
	const auto visual = GetSystemOverlayVisualState();
	return visual.active ||
	       visual.revision != m_impl->presented_overlay_revision.load(std::memory_order_acquire);
}

RenderContext& Presenter::Renderer() const noexcept {
	return m_impl->renderer;
}

void Presenter::Present(Frame& frame) {
	const Layer layer {&frame, 0, false};
	Present(std::span(&layer, 1));
}

void Presenter::Present(std::span<const Layer> layers) {
	Common::LockGuard lock(m_impl->present_mutex);
	for (const auto& layer: layers) {
		EXIT_IF(layer.bus < 0 || layer.bus >= static_cast<int>(m_impl->layers.size()));
		m_impl->frames.ValidateForPresent(layer.frame);
		auto& previous = m_impl->layers[layer.bus];
		if (previous.frame != nullptr) {
			m_impl->frames.Release(previous.frame);
		}
		previous = layer;
	}
	m_impl->Present();
}

void Presenter::ClearLayer(int bus) {
	if (static_cast<size_t>(bus) >= m_impl->layers.size()) {
		return;
	}
	Common::LockGuard lock(m_impl->present_mutex);
	auto& layer = m_impl->layers[bus];
	if (layer.frame != nullptr) {
		m_impl->frames.Release(layer.frame);
		layer = {};
	}
}

void Presenter::Impl::Present() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		Metal::MetalRenderer::Instance().Flush();
		if (Metal::MetalPresenter::Instance().IsActive()) {
			static const bool s_force_flip = (std::getenv("KYTY_FORCE_FLIP_BUF") != nullptr);
			void* last_rt_tex = Metal::MetalRenderer::Instance().GetLastRenderTexture();
			auto* f = layers[0].frame;

			const void* flip_vaddr = (f != nullptr && f->metal_guest_vaddr != 0)
			                             ? reinterpret_cast<const void*>(f->metal_guest_vaddr)
			                             : nullptr;

			// If last_rt_tex is available and not forced to flip buf, present the rendered GPU texture:
			// The flip address names the display buffer to show. The game rotates several display buffers,
			// so each flip resolves its own address: the compute image written there (composite output) first,
			// then a draw target written at that address. No fallback to the last drawn target: a missing
			// texture presents black (below).
			void* flip_rt = nullptr;
			if (!s_force_flip && flip_vaddr != nullptr) {
				const auto flip_addr = reinterpret_cast<uint64_t>(flip_vaddr);
				flip_rt = Metal::GetComputeWrittenImageForAddress(flip_addr);
				if (flip_rt == nullptr) {
					flip_rt = Metal::MetalRenderer::Instance().GetRenderTextureForAddress(flip_addr);
				}
			}
			{
				static std::atomic<uint32_t> s_flip_rt_log {0};
				if (!s_force_flip && flip_vaddr != nullptr && flip_rt != nullptr && std::getenv("KYTY_DUMP_FLIP_RT") != nullptr) {
					static std::atomic<uint32_t> s_flip_dump {0};
					const uint32_t n = s_flip_dump.fetch_add(1);
					if (n == 30 || n == 60) {
						char path[128];
						std::snprintf(path, sizeof(path), "/tmp/flip_rt_%u.ppm", n);
						// Wait for the GPU: the writer's dispatch may still be pending at present time.
						Metal::MetalContext::Instance().CommitCommandBuffer(true);
						Metal::MetalPresenter::Instance().DumpTextureToPpm(flip_rt, path);
						std::fprintf(stderr, "[Swapchain] dumped flip texture %p -> %s\n", flip_rt, path);
						// Compute-written LUT images at their guest addresses (registered by the bind sites).
						if (n == 30 && std::getenv("KYTY_DUMP_LAST_RT") != nullptr) {
							Metal::MetalContext::Instance().CommitCommandBuffer(true);
							if (void* lrt = Metal::MetalRenderer::Instance().GetLastRenderTexture()) {
								Metal::MetalPresenter::Instance().DumpTextureToPpm(lrt, "/tmp/lastrt_n30.ppm");
								std::fprintf(stderr, "[Swapchain] dumped last RT %p\n", lrt);
							}
						}
						if (n == 30 && std::getenv("KYTY_DUMP_ALL_RTS") != nullptr) {
							Metal::MetalContext::Instance().CommitCommandBuffer(true);
							int k = 0;
							for (const auto& trt: Metal::MetalRenderer::Instance().GetTrackedRenderTargets()) {
								if (k++ >= 12 || trt.texture == nullptr) continue;
								char all_path[160];
								std::snprintf(all_path, sizeof(all_path), "/tmp/allrt_%llx_f%u.ppm", (unsigned long long)trt.addr, trt.format);
								Metal::MetalPresenter::Instance().DumpTextureToPpm(trt.texture, all_path);
							}
						}
						for (const uint64_t rt_addr: {uint64_t {0x8240000000ull}, uint64_t {0x8240870000ull}}) {
							if (void* rt = Metal::MetalRenderer::Instance().GetRenderTextureForAddress(rt_addr)) {
								char rt_path[160];
								std::snprintf(rt_path, sizeof(rt_path), "/tmp/scene_%llx_n%u.ppm", (unsigned long long)rt_addr, n);
								Metal::MetalPresenter::Instance().DumpTextureToPpm(rt, rt_path);
								std::fprintf(stderr, "[Swapchain] dumped scene RT %p -> %s\n", rt, rt_path);
							}
						}
						for (const uint64_t lut_addr: {uint64_t {0x81b3c80000ull}, uint64_t {0x81b3cc0000ull}}) {
							if (void* lut = Metal::GetComputeWrittenImageForAddress(lut_addr)) {
								char lut_path[160];
								std::snprintf(lut_path, sizeof(lut_path), "/tmp/lut_%llx_n%u.ppm", (unsigned long long)lut_addr, n);
								const char* slice_env = std::getenv("KYTY_DUMP_3D_SLICE");
								(void)slice_env;
								Metal::MetalPresenter::Instance().DumpTextureToPpm(lut, lut_path);
								std::fprintf(stderr, "[Swapchain] dumped LUT %p -> %s\n", lut, lut_path);
							}
						}
					}
				}
				if (!s_force_flip && flip_vaddr != nullptr && s_flip_rt_log.fetch_add(1) < 10) {
					std::fprintf(stderr, "[Swapchain] flip vaddr=%p rt_at_flip=%p last_rt=%p\n", flip_vaddr,
					             flip_rt, last_rt_tex);
				}
			}
			if (!s_force_flip && flip_rt == nullptr) {
				// No texture for this flip's display buffer yet: present black, and say so once.
				static std::atomic<bool> s_no_flip_tex_logged {false};
				if (!s_no_flip_tex_logged.exchange(true)) {
					std::fprintf(stderr, "[Swapchain] no texture at flip vaddr=%p; presenting black\n", flip_vaddr);
				}
				Metal::MetalPresenter::Instance().PresentFrame(nullptr, 1920, 1080, 1920 * 4, 0);
				goto cleanup_metal_layers;
			}
			if (!s_force_flip) {
				static std::atomic<uint64_t> s_tex_present_log {0};
				uint64_t tp_count = s_tex_present_log.fetch_add(1, std::memory_order_relaxed);

				void* present_tex = flip_rt;
				static const int s_present_fmt = [] {
					const char* value = std::getenv("KYTY_PRESENT_FMT");
					return value != nullptr ? std::atoi(value) : -1;
				}();
				if (s_present_fmt >= 0) {
					if (void* forced = Metal::MetalRenderer::Instance().GetRenderTextureForFormat(
					        static_cast<uint32_t>(s_present_fmt))) {
						present_tex = forced;
					}
				}
				static const uint64_t s_present_addr = [] {
					const char* value = std::getenv("KYTY_PRESENT_ADDR");
					return value != nullptr ? std::strtoull(value, nullptr, 0) : 0ull;
				}();
				static const int s_present_addr_fmt = [] {
					const char* value = std::getenv("KYTY_PRESENT_ADDR_FMT");
					return value != nullptr ? std::atoi(value) : -1;
				}();
				if (s_present_addr != 0) {
					void* forced = nullptr;
					if (s_present_addr_fmt >= 0) {
						forced = Metal::MetalRenderer::Instance().GetRenderTextureForAddressFormat(
						    s_present_addr, static_cast<uint32_t>(s_present_addr_fmt));
					}
					if (forced == nullptr) {
						forced =
						    Metal::MetalRenderer::Instance().GetRenderTextureForAddress(s_present_addr);
					}
					if (forced != nullptr) {
						present_tex = forced;
					}
				}

				if (tp_count < 10 || tp_count % 120 == 0) {
					auto lrt = Metal::MetalRenderer::Instance().GetLastRenderTarget();
					std::fprintf(stderr, "[Swapchain] Presenting Metal texture %p (#%llu) (last_rt=0x%llx fmt=%u) forced_addr=0x%llx forced_fmt=%d\n",
					             present_tex, (unsigned long long)tp_count,
					             (unsigned long long)lrt.addr, lrt.format,
					             (unsigned long long)s_present_addr, s_present_fmt);
				}

				static const uint64_t s_dump_rts_frame = [] {
					const char* value = std::getenv("KYTY_DUMP_RTS_FRAME");
					return value != nullptr ? std::strtoull(value, nullptr, 0) : 0ull;
				}();
				bool trigger_dump = (s_dump_rts_frame != 0 && tp_count == s_dump_rts_frame);
				std::error_code ec;
				if (!trigger_dump && std::filesystem::exists("/tmp/trigger_rt_dump", ec)) {
					trigger_dump = true;
					std::filesystem::remove("/tmp/trigger_rt_dump", ec);
				}
				if (trigger_dump) {
					mkdir("/tmp/rt_dump", 0755);
					const char* guest_rt_env = std::getenv("KYTY_PRESENT_ADDR");
					const uint64_t guest_rt =
					    guest_rt_env != nullptr ? std::strtoull(guest_rt_env, nullptr, 0) : 0ull;
					if (guest_rt != 0) {
						const auto* bytes = reinterpret_cast<const uint8_t*>(guest_rt);
						if (FILE* f = std::fopen("/tmp/rt_dump/rt_cpu_guest.ppm", "wb");
						    f != nullptr) {
							std::fprintf(f, "P6\n1920 1080\n255\n");
							for (uint32_t y = 0; y < 1080; ++y) {
								const uint8_t* row = bytes + static_cast<size_t>(y) * 7680;
								for (uint32_t x = 0; x < 1920; ++x) {
									std::fwrite(row + x * 4, 1, 3, f);
								}
							}
							std::fclose(f);
						}
					}
					const auto rts =
					    Metal::MetalRenderer::Instance().GetTrackedRenderTargets();
					for (const auto& rt: rts) {
						char path[256];
						std::snprintf(path, sizeof(path),
						              "/tmp/rt_dump/rt_%llx_fmt%u_%ux%u.ppm",
						              static_cast<unsigned long long>(rt.addr), rt.format,
						              rt.width, rt.height);
						const bool ok = Metal::MetalPresenter::Instance().DumpTextureToPpm(
						    rt.texture, path);
						std::fprintf(stderr, "[DumpRT] addr=0x%llx fmt=%u %ux%u ok=%d\n",
						             static_cast<unsigned long long>(rt.addr), rt.format,
						             rt.width, rt.height, ok ? 1 : 0);
					}
				}
				Metal::MetalPresenter::Instance().PresentTexture(present_tex);
			} else {
				uint32_t p_width = 1920;
				uint32_t p_height = 1080;
				uint32_t p_pitch = 1920 * 4;
				uint32_t p_format = 0;

				if (flip_vaddr != nullptr) {
					p_width = f->metal_width;
					p_height = f->metal_height;
					p_pitch = f->metal_pitch;
					p_format = f->metal_format;
				} else if (last_rt_tex != nullptr) {
					Metal::MetalPresenter::Instance().PresentTexture(last_rt_tex);
					goto cleanup_metal_layers;
				} else {
					auto last_rt = Metal::MetalRenderer::Instance().GetLastRenderTarget();
					if (last_rt.addr != 0) {
						flip_vaddr = reinterpret_cast<const void*>(last_rt.addr);
						p_width = last_rt.width > 0 ? last_rt.width : 1920;
						p_height = last_rt.height > 0 ? last_rt.height : 1080;
						p_pitch = p_width * 4;
						p_format = last_rt.format;
					}
				}

				static std::atomic<uint64_t> s_swap_present_log {0};
				uint64_t sp_count = s_swap_present_log.fetch_add(1, std::memory_order_relaxed);
				if (sp_count < 10 || sp_count % 120 == 0) {
					std::fprintf(stderr, "[Swapchain] PresentBuffer #%llu: vaddr=%p (%ux%u fmt=%u pitch=%u)\n",
					             (unsigned long long)sp_count, flip_vaddr,
					             p_width, p_height, p_format, p_pitch);
				}

				Metal::MetalPresenter::Instance().PresentFrame(flip_vaddr, p_width, p_height, p_pitch, p_format);
			}
		}
cleanup_metal_layers:
		for (const auto& layer: layers) {
			if (layer.frame != nullptr) {
				layer.frame->present_tick = 1;
			}
		}
		window.UpdateTitle();
		return;
	}
#endif
	present_scheduler.SetProfileFrame(renderer.DiagnosticFrameNum());
	ProfileCpuScope present_profile(present_scheduler, "present_cpu");
	KYTY_PROFILER_FUNCTION();

	const auto overlay_visual = GetSystemOverlayVisualState();
	// Some window systems keep presenting an old swapchain after a resize.
	if (swapchain.NeedsResize()) {
		RecoverSwapchain(Swapchain::Status::Recreate);
	}
	for (uint32_t attempt = 0; attempt < 2; attempt++) {
		auto status = swapchain.AcquireNextImage();
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			continue;
		}
		{
			Common::LockGuard render_lock(renderer.GetMutex());
			auto&             command = present_scheduler.BeginCommand();
			const bool        draw_system_overlay =
			    overlay_visual.active && swapchain.PrepareSystemOverlay();
			swapchain.RecordPresentCommands(command, layers[0].frame, layers[1],
			                                draw_system_overlay);
			const auto tick = swapchain.Submit(present_scheduler);
			for (const auto& layer: layers) {
				if (layer.frame != nullptr) {
					layer.frame->present_tick = tick;
				}
			}
		}
		status = swapchain.Present();
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			continue;
		}

		presented_overlay_revision.store(overlay_visual.revision, std::memory_order_release);
		// Opt-in wall-clock present rate for the live VRAM pressure A/B. Reset the window when the
		// trigger changes so a sample never mixes the two modes.
		static const bool measure_vram_test = std::getenv("KYTY_VRAM_TEST_FPS") != nullptr;
		if (measure_vram_test) {
			static auto start = std::chrono::steady_clock::now();
			static uint64_t frames = 0;
			static bool last_pressure = false;
			std::FILE* trigger = std::fopen("D:/PS5/dumps/VRAM_PRESSURE_TEST", "rb");
			const bool pressure = trigger != nullptr;
			if (trigger != nullptr) std::fclose(trigger);
			const auto now = std::chrono::steady_clock::now();
			if (pressure != last_pressure) {
				start = now;
				frames = 0;
				last_pressure = pressure;
			}
			++frames;
			const auto seconds = std::chrono::duration<double>(now - start).count();
			if (seconds >= 10.0) {
				if (std::FILE* log = std::fopen("D:/PS5/fps-pressure-test.txt", "a"); log != nullptr) {
					std::fprintf(log, "PresentRate: pressure=%u frames=%llu seconds=%.3f fps=%.3f\n",
					             pressure ? 1u : 0u, static_cast<unsigned long long>(frames),
					             seconds, static_cast<double>(frames) / seconds);
					std::fclose(log);
				}
				start = now;
				frames = 0;
			}
		}
		window.UpdateTitle();
		return;
	}
	LOGF("Vulkan presentation retry exhausted; dropping frame\n");
}

void Presenter::Discard(Frame& frame) {
	m_impl->frames.Release(&frame);
}

WindowContext::WindowContext() = default;

} // namespace Libs::Graphics
