#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALPRESENTER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALPRESENTER_H_

#include "common/common.h"
#include <cstdint>
#include <memory>

struct SDL_Window;

namespace Libs::Graphics::Metal {

class MetalPresenter {
public:
	static MetalPresenter& Instance();

	MetalPresenter();
	~MetalPresenter();

	KYTY_CLASS_NO_COPY(MetalPresenter);

	bool Initialize(SDL_Window* window, uint32_t width, uint32_t height);
	void Shutdown();
	[[nodiscard]] bool IsActive() const noexcept { return m_active; }

	// Present a native Metal texture directly to the CAMetalLayer swapchain
	bool PresentTexture(void* texture);

	// Debug: render any metal texture to an RGBA8 PPM file (top-down).
	bool DumpTextureToPpm(void* texture, const char* path);

	// Present a guest framebuffer directly to the CAMetalLayer swapchain
	bool PresentFrame(const void* guest_vaddr, uint32_t width, uint32_t height,
	                  uint32_t stride_bytes, uint32_t format = 0);

	void Resize(uint32_t width, uint32_t height);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
	bool m_active = false;
};

} // namespace Libs::Graphics::Metal

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALPRESENTER_H_
