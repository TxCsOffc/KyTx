#include "graphics/host_gpu/metal/metalTexture.h"
#include "graphics/host_gpu/metal/metalContext.h"
#include "common/logging/log.h"

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

namespace Libs::Graphics::Metal {

MTLPixelFormat MetalFormatFromBufferFormat(Prospero::BufferFormat format) {
	switch (format) {
		case Prospero::BufferFormat::k8UNorm: return MTLPixelFormatR8Unorm;
		case Prospero::BufferFormat::k8SNorm: return MTLPixelFormatR8Snorm;
		case Prospero::BufferFormat::k8UScaled: return MTLPixelFormatR8Uint;
		case Prospero::BufferFormat::k8SScaled: return MTLPixelFormatR8Sint;
		case Prospero::BufferFormat::k8UInt: return MTLPixelFormatR8Uint;
		case Prospero::BufferFormat::k8SInt: return MTLPixelFormatR8Sint;

		case Prospero::BufferFormat::k16UNorm: return MTLPixelFormatR16Unorm;
		case Prospero::BufferFormat::k16SNorm: return MTLPixelFormatR16Snorm;
		case Prospero::BufferFormat::k16UScaled: return MTLPixelFormatR16Uint;
		case Prospero::BufferFormat::k16SScaled: return MTLPixelFormatR16Sint;
		case Prospero::BufferFormat::k16UInt: return MTLPixelFormatR16Uint;
		case Prospero::BufferFormat::k16SInt: return MTLPixelFormatR16Sint;
		case Prospero::BufferFormat::k16Float: return MTLPixelFormatR16Float;

		case Prospero::BufferFormat::k8_8UNorm: return MTLPixelFormatRG8Unorm;
		case Prospero::BufferFormat::k8_8SNorm: return MTLPixelFormatRG8Snorm;
		case Prospero::BufferFormat::k8_8UScaled: return MTLPixelFormatRG8Uint;
		case Prospero::BufferFormat::k8_8SScaled: return MTLPixelFormatRG8Sint;
		case Prospero::BufferFormat::k8_8UInt: return MTLPixelFormatRG8Uint;
		case Prospero::BufferFormat::k8_8SInt: return MTLPixelFormatRG8Sint;

		case Prospero::BufferFormat::k32UInt: return MTLPixelFormatR32Uint;
		case Prospero::BufferFormat::k32SInt: return MTLPixelFormatR32Sint;
		case Prospero::BufferFormat::k32Float: return MTLPixelFormatR32Float;

		case Prospero::BufferFormat::k16_16UNorm: return MTLPixelFormatRG16Unorm;
		case Prospero::BufferFormat::k16_16SNorm: return MTLPixelFormatRG16Snorm;
		case Prospero::BufferFormat::k16_16UScaled: return MTLPixelFormatRG16Uint;
		case Prospero::BufferFormat::k16_16SScaled: return MTLPixelFormatRG16Sint;
		case Prospero::BufferFormat::k16_16UInt: return MTLPixelFormatRG16Uint;
		case Prospero::BufferFormat::k16_16SInt: return MTLPixelFormatRG16Sint;
		case Prospero::BufferFormat::k16_16Float: return MTLPixelFormatRG16Float;

		case Prospero::BufferFormat::k11_11_10Float: return MTLPixelFormatRG11B10Float;
		case Prospero::BufferFormat::k10_10_10_2UNorm: return MTLPixelFormatRGB10A2Unorm;
		case Prospero::BufferFormat::k10_10_10_2SNorm: return MTLPixelFormatRGB10A2Unorm;
		case Prospero::BufferFormat::k10_10_10_2UInt: return MTLPixelFormatRGB10A2Uint;

		case Prospero::BufferFormat::k8_8_8_8UNorm: return MTLPixelFormatRGBA8Unorm;
		case Prospero::BufferFormat::k8_8_8_8SNorm: return MTLPixelFormatRGBA8Snorm;
		case Prospero::BufferFormat::k8_8_8_8UScaled: return MTLPixelFormatRGBA8Uint;
		case Prospero::BufferFormat::k8_8_8_8SScaled: return MTLPixelFormatRGBA8Sint;
		case Prospero::BufferFormat::k8_8_8_8UInt: return MTLPixelFormatRGBA8Uint;
		case Prospero::BufferFormat::k8_8_8_8SInt: return MTLPixelFormatRGBA8Sint;

		case Prospero::BufferFormat::k32_32UInt: return MTLPixelFormatRG32Uint;
		case Prospero::BufferFormat::k32_32SInt: return MTLPixelFormatRG32Sint;
		case Prospero::BufferFormat::k32_32Float: return MTLPixelFormatRG32Float;

		case Prospero::BufferFormat::k16_16_16_16UNorm: return MTLPixelFormatRGBA16Unorm;
		case Prospero::BufferFormat::k16_16_16_16SNorm: return MTLPixelFormatRGBA16Snorm;
		case Prospero::BufferFormat::k16_16_16_16UScaled: return MTLPixelFormatRGBA16Uint;
		case Prospero::BufferFormat::k16_16_16_16SScaled: return MTLPixelFormatRGBA16Sint;
		case Prospero::BufferFormat::k16_16_16_16UInt: return MTLPixelFormatRGBA16Uint;
		case Prospero::BufferFormat::k16_16_16_16SInt: return MTLPixelFormatRGBA16Sint;
		case Prospero::BufferFormat::k16_16_16_16Float: return MTLPixelFormatRGBA16Float;

		case Prospero::BufferFormat::k32_32_32UInt: return MTLPixelFormatRGBA32Uint;
		case Prospero::BufferFormat::k32_32_32SInt: return MTLPixelFormatRGBA32Sint;
		case Prospero::BufferFormat::k32_32_32Float: return MTLPixelFormatRGBA32Float;

		case Prospero::BufferFormat::k32_32_32_32UInt: return MTLPixelFormatRGBA32Uint;
		case Prospero::BufferFormat::k32_32_32_32SInt: return MTLPixelFormatRGBA32Sint;
		case Prospero::BufferFormat::k32_32_32_32Float: return MTLPixelFormatRGBA32Float;

		case Prospero::BufferFormat::k8Srgb: return MTLPixelFormatR8Unorm;
		case Prospero::BufferFormat::k8_8Srgb: return MTLPixelFormatRG8Unorm;
		case Prospero::BufferFormat::k8_8_8_8Srgb: return MTLPixelFormatRGBA8Unorm_sRGB;
		case Prospero::BufferFormat::k9_9_9_5Float: return MTLPixelFormatRGB9E5Float;

		case Prospero::BufferFormat::k5_6_5UNorm: return MTLPixelFormatB5G6R5Unorm;
		case Prospero::BufferFormat::k5_5_5_1UNorm: return MTLPixelFormatA1BGR5Unorm;
		case Prospero::BufferFormat::k1_5_5_5UNorm: return MTLPixelFormatBGR5A1Unorm;
		case Prospero::BufferFormat::k4_4_4_4UNorm: return MTLPixelFormatABGR4Unorm;

		// Block compressed formats
		case Prospero::BufferFormat::kBc1UNorm: return MTLPixelFormatBC1_RGBA;
		case Prospero::BufferFormat::kBc1Srgb: return MTLPixelFormatBC1_RGBA_sRGB;
		case Prospero::BufferFormat::kBc2UNorm: return MTLPixelFormatBC2_RGBA;
		case Prospero::BufferFormat::kBc2Srgb: return MTLPixelFormatBC2_RGBA_sRGB;
		case Prospero::BufferFormat::kBc3UNorm: return MTLPixelFormatBC3_RGBA;
		case Prospero::BufferFormat::kBc3Srgb: return MTLPixelFormatBC3_RGBA_sRGB;
		case Prospero::BufferFormat::kBc4UNorm: return MTLPixelFormatBC4_RUnorm;
		case Prospero::BufferFormat::kBc4SNorm: return MTLPixelFormatBC4_RSnorm;
		case Prospero::BufferFormat::kBc5UNorm: return MTLPixelFormatBC5_RGUnorm;
		case Prospero::BufferFormat::kBc5SNorm: return MTLPixelFormatBC5_RGSnorm;
		case Prospero::BufferFormat::kBc6UFloat: return MTLPixelFormatBC6H_RGBUfloat;
		case Prospero::BufferFormat::kBc6SFloat: return MTLPixelFormatBC6H_RGBFloat;
		case Prospero::BufferFormat::kBc7UNorm: return MTLPixelFormatBC7_RGBAUnorm;
		case Prospero::BufferFormat::kBc7Srgb: return MTLPixelFormatBC7_RGBAUnorm_sRGB;

		default: return MTLPixelFormatInvalid;
	}
}

MTLPixelFormat MetalFormatFromRenderTarget(Prospero::ChannelLayout layout,
                                           Prospero::ChannelType   type,
                                           Prospero::ChannelOrder  order) {
	const auto encoding = Prospero::ResolveRenderTargetFormat(layout, type);
	if (!encoding.IsValid() || !encoding.SupportsOrder(order)) {
		return MTLPixelFormatInvalid;
	}

	if (order == Prospero::ChannelOrder::kAlt) {
		switch (encoding.buffer_format) {
			case Prospero::BufferFormat::k8_8_8_8UNorm: return MTLPixelFormatBGRA8Unorm;
			case Prospero::BufferFormat::k8_8_8_8Srgb: return MTLPixelFormatBGRA8Unorm_sRGB;
			case Prospero::BufferFormat::k10_10_10_2UNorm: return MTLPixelFormatBGR10A2Unorm;
			default: break;
		}
	}

	return MetalFormatFromBufferFormat(encoding.buffer_format);
}

MTLPixelFormat MetalFormatFromDepthFormat(Prospero::DepthFormat format, bool has_stencil) {
	switch (format) {
		case Prospero::DepthFormat::kZ16:
			return has_stencil ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatDepth16Unorm;
		case Prospero::DepthFormat::kZ32F:
			return has_stencil ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatDepth32Float;
		default:
			return has_stencil ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatDepth32Float;
	}
}

MTLPixelFormat MetalFormatFromVulkanFormat(uint32_t vk_format) {
	switch (vk_format) {
		case 9:   return MTLPixelFormatR8Unorm;              // VK_FORMAT_R8_UNORM
		case 10:  return MTLPixelFormatR8Snorm;              // VK_FORMAT_R8_SNORM
		case 13:  return MTLPixelFormatR8Uint;               // VK_FORMAT_R8_UINT
		case 14:  return MTLPixelFormatR8Sint;               // VK_FORMAT_R8_SINT
		case 16:  return MTLPixelFormatRG8Unorm;             // VK_FORMAT_R8G8_UNORM
		case 17:  return MTLPixelFormatRG8Snorm;             // VK_FORMAT_R8G8_SNORM
		case 20:  return MTLPixelFormatRG8Uint;              // VK_FORMAT_R8G8_UINT
		case 21:  return MTLPixelFormatRG8Sint;              // VK_FORMAT_R8G8_SINT
		case 37:  return MTLPixelFormatRGBA8Unorm;           // VK_FORMAT_R8G8B8A8_UNORM
		case 38:  return MTLPixelFormatRGBA8Snorm;           // VK_FORMAT_R8G8B8A8_SNORM
		case 41:  return MTLPixelFormatRGBA8Uint;            // VK_FORMAT_R8G8B8A8_UINT
		case 42:  return MTLPixelFormatRGBA8Sint;            // VK_FORMAT_R8G8B8A8_SINT
		case 43:  return MTLPixelFormatRGBA8Unorm_sRGB;      // VK_FORMAT_R8G8B8A8_SRGB
		case 44:  return MTLPixelFormatBGRA8Unorm;           // VK_FORMAT_B8G8R8A8_UNORM
		case 50:  return MTLPixelFormatBGRA8Unorm_sRGB;      // VK_FORMAT_B8G8R8A8_SRGB
		case 58:  return MTLPixelFormatBGR10A2Unorm;         // VK_FORMAT_A2R10G10B10_UNORM_PACK32
		case 64:  return MTLPixelFormatRGB10A2Unorm;         // VK_FORMAT_A2B10G10R10_UNORM_PACK32
		case 68:  return MTLPixelFormatRGB10A2Uint;          // VK_FORMAT_A2B10G10R10_UINT_PACK32
		case 70:  return MTLPixelFormatR16Unorm;             // VK_FORMAT_R16_UNORM
		case 71:  return MTLPixelFormatR16Snorm;             // VK_FORMAT_R16_SNORM
		case 74:  return MTLPixelFormatR16Uint;              // VK_FORMAT_R16_UINT
		case 75:  return MTLPixelFormatR16Sint;              // VK_FORMAT_R16_SINT
		case 76:  return MTLPixelFormatR16Float;             // VK_FORMAT_R16_SFLOAT
		case 77:  return MTLPixelFormatRG16Unorm;            // VK_FORMAT_R16G16_UNORM
		case 78:  return MTLPixelFormatRG16Snorm;            // VK_FORMAT_R16G16_SNORM
		case 81:  return MTLPixelFormatRG16Uint;             // VK_FORMAT_R16G16_UINT
		case 82:  return MTLPixelFormatRG16Sint;             // VK_FORMAT_R16G16_SINT
		case 83:  return MTLPixelFormatRG16Float;            // VK_FORMAT_R16G16_SFLOAT
		case 91:  return MTLPixelFormatRGBA16Unorm;          // VK_FORMAT_R16G16B16A16_UNORM
		case 92:  return MTLPixelFormatRGBA16Snorm;          // VK_FORMAT_R16G16B16A16_SNORM
		case 95:  return MTLPixelFormatRGBA16Uint;           // VK_FORMAT_R16G16B16A16_UINT
		case 96:  return MTLPixelFormatRGBA16Sint;           // VK_FORMAT_R16G16B16A16_SINT
		case 97:  return MTLPixelFormatRGBA16Float;          // VK_FORMAT_R16G16B16A16_SFLOAT
		case 98:  return MTLPixelFormatR32Uint;              // VK_FORMAT_R32_UINT
		case 99:  return MTLPixelFormatR32Sint;              // VK_FORMAT_R32_SINT
		case 100: return MTLPixelFormatR32Float;             // VK_FORMAT_R32_SFLOAT
		case 101: return MTLPixelFormatRG32Uint;             // VK_FORMAT_R32G32_UINT
		case 102: return MTLPixelFormatRG32Sint;             // VK_FORMAT_R32G32_SINT
		case 103: return MTLPixelFormatRG32Float;            // VK_FORMAT_R32G32_SFLOAT
		case 107: return MTLPixelFormatRGBA32Uint;           // VK_FORMAT_R32G32B32A32_UINT
		case 108: return MTLPixelFormatRGBA32Sint;           // VK_FORMAT_R32G32B32A32_SINT
		case 109: return MTLPixelFormatRGBA32Float;          // VK_FORMAT_R32G32B32A32_SFLOAT
		case 122: return MTLPixelFormatRG11B10Float;         // VK_FORMAT_B10G11R11_UFLOAT_PACK32
		case 123: return MTLPixelFormatRGB9E5Float;          // VK_FORMAT_E5B9G9R9_UFLOAT_PACK32
		case 124: return MTLPixelFormatDepth16Unorm;         // VK_FORMAT_D16_UNORM
		case 126: return MTLPixelFormatDepth32Float;         // VK_FORMAT_D32_SFLOAT
		case 129: return MTLPixelFormatDepth32Float_Stencil8;// VK_FORMAT_D24_UNORM_S8_UINT
		case 130: return MTLPixelFormatDepth32Float_Stencil8;// VK_FORMAT_D32_SFLOAT_S8_UINT
		case 133: return MTLPixelFormatBC1_RGBA;             // VK_FORMAT_BC1_RGBA_UNORM_BLOCK
		case 134: return MTLPixelFormatBC1_RGBA_sRGB;        // VK_FORMAT_BC1_RGBA_SRGB_BLOCK
		case 135: return MTLPixelFormatBC2_RGBA;             // VK_FORMAT_BC2_UNORM_BLOCK
		case 136: return MTLPixelFormatBC2_RGBA_sRGB;        // VK_FORMAT_BC2_SRGB_BLOCK
		case 137: return MTLPixelFormatBC3_RGBA;             // VK_FORMAT_BC3_UNORM_BLOCK
		case 138: return MTLPixelFormatBC3_RGBA_sRGB;        // VK_FORMAT_BC3_SRGB_BLOCK
		case 139: return MTLPixelFormatBC4_RUnorm;           // VK_FORMAT_BC4_UNORM_BLOCK
		case 140: return MTLPixelFormatBC4_RSnorm;           // VK_FORMAT_BC4_SNORM_BLOCK
		case 141: return MTLPixelFormatBC5_RGUnorm;          // VK_FORMAT_BC5_UNORM_BLOCK
		case 142: return MTLPixelFormatBC5_RGSnorm;          // VK_FORMAT_BC5_SNORM_BLOCK
		case 143: return MTLPixelFormatBC6H_RGBUfloat;       // VK_FORMAT_BC6H_UFLOAT_BLOCK
		case 144: return MTLPixelFormatBC6H_RGBFloat;        // VK_FORMAT_BC6H_SFLOAT_BLOCK
		case 145: return MTLPixelFormatBC7_RGBAUnorm;        // VK_FORMAT_BC7_UNORM_BLOCK
		case 146: return MTLPixelFormatBC7_RGBAUnorm_sRGB;   // VK_FORMAT_BC7_SRGB_BLOCK
		default:  return MTLPixelFormatInvalid;
	}
}

bool IsDepthFormat(MTLPixelFormat format) {
	return format == MTLPixelFormatDepth16Unorm ||
	       format == MTLPixelFormatDepth32Float ||
	       format == MTLPixelFormatDepth32Float_Stencil8;
}

bool IsStencilFormat(MTLPixelFormat format) {
	return format == MTLPixelFormatStencil8 ||
	       format == MTLPixelFormatDepth32Float_Stencil8;
}

bool IsCompressedFormat(MTLPixelFormat format) {
	return (format >= MTLPixelFormatBC1_RGBA && format <= MTLPixelFormatBC7_RGBAUnorm_sRGB);
}

NSUInteger LinearTextureRowAlignment(id<MTLDevice> device, MTLPixelFormat format) {
	if (device == nil || format == MTLPixelFormatInvalid || IsCompressedFormat(format)) {
		return 0;
	}
	return [device minimumLinearTextureAlignmentForPixelFormat:format];
}

uint32_t BytesPerPixel(MTLPixelFormat format) {
	if (IsCompressedFormat(format)) {
		return 0;
	}
	switch (format) {
		case MTLPixelFormatR8Unorm:
		case MTLPixelFormatR8Snorm:
		case MTLPixelFormatR8Uint:
		case MTLPixelFormatR8Sint:
		case MTLPixelFormatStencil8:
			return 1;

		case MTLPixelFormatR16Unorm:
		case MTLPixelFormatR16Snorm:
		case MTLPixelFormatR16Uint:
		case MTLPixelFormatR16Sint:
		case MTLPixelFormatR16Float:
		case MTLPixelFormatRG8Unorm:
		case MTLPixelFormatRG8Snorm:
		case MTLPixelFormatRG8Uint:
		case MTLPixelFormatRG8Sint:
		case MTLPixelFormatB5G6R5Unorm:
		case MTLPixelFormatA1BGR5Unorm:
		case MTLPixelFormatABGR4Unorm:
		case MTLPixelFormatBGR5A1Unorm:
		case MTLPixelFormatDepth16Unorm:
			return 2;

		case MTLPixelFormatR32Uint:
		case MTLPixelFormatR32Sint:
		case MTLPixelFormatR32Float:
		case MTLPixelFormatRG16Unorm:
		case MTLPixelFormatRG16Snorm:
		case MTLPixelFormatRG16Uint:
		case MTLPixelFormatRG16Sint:
		case MTLPixelFormatRG16Float:
		case MTLPixelFormatRGBA8Unorm:
		case MTLPixelFormatRGBA8Unorm_sRGB:
		case MTLPixelFormatRGBA8Snorm:
		case MTLPixelFormatRGBA8Uint:
		case MTLPixelFormatRGBA8Sint:
		case MTLPixelFormatBGRA8Unorm:
		case MTLPixelFormatBGRA8Unorm_sRGB:
		case MTLPixelFormatRGB10A2Unorm:
		case MTLPixelFormatRGB10A2Uint:
		case MTLPixelFormatRG11B10Float:
		case MTLPixelFormatRGB9E5Float:
		case MTLPixelFormatBGR10A2Unorm:
		case MTLPixelFormatDepth32Float:
			return 4;

		case MTLPixelFormatRG32Uint:
		case MTLPixelFormatRG32Sint:
		case MTLPixelFormatRG32Float:
		case MTLPixelFormatRGBA16Unorm:
		case MTLPixelFormatRGBA16Snorm:
		case MTLPixelFormatRGBA16Uint:
		case MTLPixelFormatRGBA16Sint:
		case MTLPixelFormatRGBA16Float:
		case MTLPixelFormatDepth32Float_Stencil8:
			return 8;

		case MTLPixelFormatRGBA32Uint:
		case MTLPixelFormatRGBA32Sint:
		case MTLPixelFormatRGBA32Float:
			return 16;

		default:
			return 4;
	}
}

MetalTexture::MetalTexture() = default;

MetalTexture::~MetalTexture() {
	Release();
}

bool MetalTexture::Initialize(const MetalTextureDesc& desc, const char* label) {
	Release();
	if (desc.pixel_format == MTLPixelFormatInvalid || desc.width == 0 || desc.height == 0) {
		return false;
	}

	@autoreleasepool {
		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		if (!device) {
			return false;
		}

		MTLTextureDescriptor* tex_desc = [[MTLTextureDescriptor alloc] init];
		tex_desc.pixelFormat = desc.pixel_format;
		tex_desc.width = desc.width;
		tex_desc.height = desc.height;
		tex_desc.depth = desc.depth > 0 ? desc.depth : 1;
		tex_desc.mipmapLevelCount = desc.mip_levels > 0 ? desc.mip_levels : 1;
		tex_desc.arrayLength = desc.array_length > 0 ? desc.array_length : 1;
		tex_desc.sampleCount = desc.sample_count > 0 ? desc.sample_count : 1;
		tex_desc.textureType = desc.texture_type;
		tex_desc.usage = desc.usage;
		tex_desc.storageMode = MTLStorageModeShared;

		m_handle = [device newTextureWithDescriptor:tex_desc];
		if (!m_handle) {
			LOGF_COLOR(Log::Color::BrightRed, "Metal: Failed to allocate texture %ux%u format=%lu\n",
			           desc.width, desc.height, static_cast<unsigned long>(desc.pixel_format));
			return false;
		}

		if (label) {
			m_handle.label = [NSString stringWithUTF8String:label];
		}

		m_desc = desc;
		return true;
	}
}

void MetalTexture::Release() {
	@autoreleasepool {
		m_handle = nil;
		m_desc = {};
	}
}

static MTLTextureSwizzle TranslateSwizzle(uint32_t s) {
	switch (s) {
		case 0: return MTLTextureSwizzleRed;
		case 1: return MTLTextureSwizzleGreen;
		case 2: return MTLTextureSwizzleBlue;
		case 3: return MTLTextureSwizzleAlpha;
		case 4: return MTLTextureSwizzleZero;
		case 5: return MTLTextureSwizzleOne;
		default: return MTLTextureSwizzleZero;
	}
}

id<MTLTexture> MetalTexture::CreateView(MTLPixelFormat format,
                                        uint32_t swizzle_r,
                                        uint32_t swizzle_g,
                                        uint32_t swizzle_b,
                                        uint32_t swizzle_a) {
	if (!m_handle) {
		return nil;
	}

	@autoreleasepool {
		MTLTextureSwizzleChannels swizzle = MTLTextureSwizzleChannelsMake(
		    TranslateSwizzle(swizzle_r),
		    TranslateSwizzle(swizzle_g),
		    TranslateSwizzle(swizzle_b),
		    TranslateSwizzle(swizzle_a));

		return [m_handle newTextureViewWithPixelFormat:format
		                                   textureType:m_desc.texture_type
		                                        levels:NSMakeRange(0, m_desc.mip_levels)
		                                        slices:NSMakeRange(0, m_desc.array_length)
		                                       swizzle:swizzle];
	}
}

bool MetalTexture::Upload(const void* bytes, size_t bytes_per_row, size_t bytes_per_image,
                          uint32_t mip_level, uint32_t slice) {
	if (!m_handle || !bytes) {
		return false;
	}

	@autoreleasepool {
		const uint32_t mip_width = std::max(1u, m_desc.width >> mip_level);
		const uint32_t mip_height = std::max(1u, m_desc.height >> mip_level);
		const uint32_t mip_depth = std::max(1u, m_desc.depth >> mip_level);

		MTLRegion region = MTLRegionMake3D(0, 0, 0, mip_width, mip_height, mip_depth);

		[m_handle replaceRegion:region
		            mipmapLevel:mip_level
		                  slice:slice
		              withBytes:bytes
		            bytesPerRow:bytes_per_row
		          bytesPerImage:bytes_per_image];

		return true;
	}
}

} // namespace Libs::Graphics::Metal
