#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALTEXTURE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALTEXTURE_H_

#include "common/common.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"

#include <cstdint>
#include <memory>
#include <string>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
typedef void* id;
typedef unsigned long NSUInteger;
enum MTLPixelFormat : NSUInteger;
enum MTLTextureType : NSUInteger;
enum MTLTextureUsage : NSUInteger;
#endif

namespace Libs::Graphics::Metal {

// Format translation functions
MTLPixelFormat MetalFormatFromBufferFormat(Prospero::BufferFormat format);
MTLPixelFormat MetalFormatFromRenderTarget(Prospero::ChannelLayout layout,
                                           Prospero::ChannelType   type,
                                           Prospero::ChannelOrder  order);
MTLPixelFormat MetalFormatFromDepthFormat(Prospero::DepthFormat format, bool has_stencil = false);
MTLPixelFormat MetalFormatFromVulkanFormat(uint32_t vk_format);

bool IsDepthFormat(MTLPixelFormat format);
bool IsStencilFormat(MTLPixelFormat format);
bool IsCompressedFormat(MTLPixelFormat format);
uint32_t BytesPerPixel(MTLPixelFormat format);
// Metal forbids compressed formats as linear/buffer-backed textures. Returns 0
// without querying the device for those formats (the query itself asserts).
#ifdef __OBJC__
NSUInteger LinearTextureRowAlignment(id<MTLDevice> device, MTLPixelFormat format);
#else
NSUInteger LinearTextureRowAlignment(id device, MTLPixelFormat format);
#endif

struct MetalTextureDesc {
	MTLPixelFormat  pixel_format       = static_cast<MTLPixelFormat>(0);
	uint32_t        width              = 1;
	uint32_t        height             = 1;
	uint32_t        depth              = 1;
	uint32_t        mip_levels         = 1;
	uint32_t        array_length       = 1;
	uint32_t        sample_count       = 1;
	MTLTextureType  texture_type       = static_cast<MTLTextureType>(2); // MTLTextureType2D
	MTLTextureUsage usage              = static_cast<MTLTextureUsage>(0x1 | 0x4); // Read | RenderTarget
	bool            is_shared_storage  = true;
};

class MetalTexture {
public:
	MetalTexture();
	~MetalTexture();

	KYTY_CLASS_NO_COPY(MetalTexture);

	bool Initialize(const MetalTextureDesc& desc, const char* label = nullptr);
	void Release();

#ifdef __OBJC__
	[[nodiscard]] id<MTLTexture> GetHandle() const noexcept { return m_handle; }
	[[nodiscard]] id<MTLTexture> CreateView(MTLPixelFormat format,
	                                        uint32_t swizzle_r = 0,
	                                        uint32_t swizzle_g = 1,
	                                        uint32_t swizzle_b = 2,
	                                        uint32_t swizzle_a = 3);
#else
	[[nodiscard]] id GetHandle() const noexcept { return m_handle; }
	[[nodiscard]] id CreateView(MTLPixelFormat format,
	                            uint32_t swizzle_r = 0,
	                            uint32_t swizzle_g = 1,
	                            uint32_t swizzle_b = 2,
	                            uint32_t swizzle_a = 3);
#endif

	bool Upload(const void* bytes, size_t bytes_per_row, size_t bytes_per_image = 0,
	            uint32_t mip_level = 0, uint32_t slice = 0);

	[[nodiscard]] const MetalTextureDesc& GetDesc() const noexcept { return m_desc; }
	[[nodiscard]] bool                    IsValid() const noexcept { return m_handle != nullptr; }
	[[nodiscard]] MTLPixelFormat          GetPixelFormat() const noexcept { return m_desc.pixel_format; }
	[[nodiscard]] uint32_t                GetWidth() const noexcept { return m_desc.width; }
	[[nodiscard]] uint32_t                GetHeight() const noexcept { return m_desc.height; }

private:
#ifdef __OBJC__
	id<MTLTexture>   m_handle = nil;
#else
	id               m_handle = nullptr;
#endif
	MetalTextureDesc m_desc {};
};

} // namespace Libs::Graphics::Metal

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALTEXTURE_H_
