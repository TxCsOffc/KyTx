#include "graphics/host_gpu/metal/metalSampler.h"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>

namespace Libs::Graphics::Metal {

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

MTLSamplerDescriptor* MakeSamplerDescriptor(const ShaderSamplerResource& r) {
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
	return d;
}

id<MTLSamplerState> GetSampler(id<MTLDevice> device, const ShaderSamplerResource& resource) {
	if (device == nil) return nil;
	using Key = std::array<uint32_t, 4>;
	static std::mutex mutex;
	static std::map<Key, id<MTLSamplerState>> cache;
	const Key key {resource.fields[0], resource.fields[1], resource.fields[2], resource.fields[3]};
	std::lock_guard lock(mutex);
	if (const auto it = cache.find(key); it != cache.end()) return it->second;
	id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:MakeSamplerDescriptor(resource)];
	if (sampler != nil) cache.emplace(key, sampler);
	return sampler;
}

} // namespace Libs::Graphics::Metal
