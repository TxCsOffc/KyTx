#include "graphics/host_gpu/metal/metalSampler.h"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>

namespace Libs::Graphics::Metal {

bool SupportsNativeDepthCompare(MTLPixelFormat format) {
	return format == MTLPixelFormatR32Float || format == MTLPixelFormatDepth16Unorm ||
	       format == MTLPixelFormatDepth32Float || format == MTLPixelFormatDepth32Float_Stencil8;
}

static MTLSamplerAddressMode AddressMode(uint8_t mode) {
	switch (static_cast<Prospero::SamplerClampMode>(mode)) {
		case Prospero::SamplerClampMode::kWrap: return MTLSamplerAddressModeRepeat;
		case Prospero::SamplerClampMode::kMirror: return MTLSamplerAddressModeMirrorRepeat;
		case Prospero::SamplerClampMode::kClampLastTexel: return MTLSamplerAddressModeClampToEdge;
		case Prospero::SamplerClampMode::kMirrorOnceLastTexel:
		case Prospero::SamplerClampMode::kMirrorOnceHalfBorder:
		case Prospero::SamplerClampMode::kMirrorOnceBorder:
			return MTLSamplerAddressModeMirrorClampToEdge;
		case Prospero::SamplerClampMode::kClampHalfBorder:
		case Prospero::SamplerClampMode::kClampBorder:
			return MTLSamplerAddressModeClampToBorderColor;
	}
	return MTLSamplerAddressModeClampToEdge;
}

MTLSamplerDescriptor* MakeSamplerDescriptor(const ShaderSamplerResource& r, bool comparison_point) {
	MTLSamplerDescriptor* d = [[MTLSamplerDescriptor alloc] init];
	d.supportArgumentBuffers = YES;
	d.minFilter = (r.XyMinFilter() & 1u) != 0 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
	d.magFilter = (r.XyMagFilter() & 1u) != 0 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
	d.sAddressMode = AddressMode(r.ClampX());
	d.tAddressMode = AddressMode(r.ClampY());
	d.rAddressMode = AddressMode(r.ClampZ());
	d.normalizedCoordinates = !r.ForceUnormCoords();
	d.mipFilter = r.MipFilter() == 0 ? MTLSamplerMipFilterNotMipmapped
	              : r.MipFilter() == 2 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
	d.lodMinClamp = r.MipFilter() != 0 ? static_cast<float>(r.MinLod()) / 256.0f : 0.0f;
	d.lodMaxClamp = r.MipFilter() != 0 ? static_cast<float>(r.MaxLod()) / 256.0f : 0.0f;
	d.maxAnisotropy = (r.XyMinFilter() >= 2 || r.XyMagFilter() >= 2)
	                      ? 1u << std::min<uint32_t>(r.MaxAnisoRatio(), 4u) : 1u;
	d.compareFunction = static_cast<MTLCompareFunction>(r.DepthCompareFunc());
	if (@available(macOS 26.0, *)) {
		// S# FILTER_MODE selects weighted average, minimum or maximum.
		// Comparison footprints perform their own weighted 0/1 interpolation.
		if (!comparison_point && r.FilterMode() <= 2)
			d.reductionMode = static_cast<MTLSamplerReductionMode>(r.FilterMode());
	}
	switch (static_cast<Prospero::SamplerBorderColor>(r.BorderColorType())) {
		case Prospero::SamplerBorderColor::kOpaqueBlack: d.borderColor = MTLSamplerBorderColorOpaqueBlack; break;
		case Prospero::SamplerBorderColor::kOpaqueWhite: d.borderColor = MTLSamplerBorderColorOpaqueWhite; break;
		default: d.borderColor = MTLSamplerBorderColorTransparentBlack; break;
	}
	if (@available(macOS 26.0, *)) {
		const int32_t bias = static_cast<int32_t>(r.LodBias() ^ 0x2000u) - 0x2000;
		d.lodBias = static_cast<float>(bias) / 256.0f;
	}
	// Pixel-coordinate samplers cannot use mipmaps or anisotropy in Metal.
	if (r.ForceUnormCoords()) {
		// Metal requires identical min/mag filters for pixel coordinates.
		// This path has LOD zero, hence the guest's magnification filter.
		d.minFilter = d.magFilter;
		d.sAddressMode = MTLSamplerAddressModeClampToEdge;
		d.tAddressMode = MTLSamplerAddressModeClampToEdge;
		d.rAddressMode = MTLSamplerAddressModeClampToEdge;
		d.mipFilter = MTLSamplerMipFilterNotMipmapped;
		d.lodMinClamp = 0.0f;
		d.lodMaxClamp = 0.0f;
		d.maxAnisotropy = 1;
		d.compareFunction = MTLCompareFunctionNever;
		if (@available(macOS 26.0, *)) d.lodBias = 0.0f;
	}
	if (comparison_point) {
		// Fetch one texel at an explicitly chosen mip. PCF must compare each
		// texel, rather than compare an already filtered depth value.
		d.minFilter = d.magFilter = MTLSamplerMinMagFilterNearest;
		d.mipFilter = r.ForceUnormCoords() ? MTLSamplerMipFilterNotMipmapped : MTLSamplerMipFilterNearest;
		d.lodMinClamp = 0.0f;
		d.lodMaxClamp = r.ForceUnormCoords() ? 0.0f : 16.0f;
		d.maxAnisotropy = 1;
		d.compareFunction = MTLCompareFunctionNever;
		if (@available(macOS 26.0, *)) d.lodBias = 0.0f;
	}
	return d;
}

id<MTLSamplerState> GetSampler(id<MTLDevice> device, const ShaderSamplerResource& resource, bool comparison_point) {
	if (device == nil) return nil;
	using Key = std::array<uint32_t, 5>;
	static std::mutex mutex;
	static std::map<Key, id<MTLSamplerState>> cache;
	const Key key {resource.fields[0], resource.fields[1], resource.fields[2], resource.fields[3], comparison_point ? 1u : 0u};
	std::lock_guard lock(mutex);
	if (const auto it = cache.find(key); it != cache.end()) return it->second;
	id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:MakeSamplerDescriptor(resource, comparison_point)];
	if (sampler != nil) cache.emplace(key, sampler);
	return sampler;
}

} // namespace Libs::Graphics::Metal
