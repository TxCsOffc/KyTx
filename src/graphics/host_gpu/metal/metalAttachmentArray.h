#pragma once
#import <Metal/Metal.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>
namespace Libs::Graphics::Metal {
// Depth/stencil layers retain their own contents and first-use clear state.
// Call Ensure/Clear with no encoder open on command. Growth preserves old layers.
class MetalAttachmentArray {
public:
	id<MTLTexture> Texture() const { return texture_; }
	bool Ensure(id<MTLDevice> device, id<MTLCommandBuffer> command, uint32_t width, uint32_t height,
	            MTLPixelFormat format, uint32_t layers) {
		if (!device || !command || !width || !height || !layers || layers > 2048 ||
		    (format != MTLPixelFormatDepth16Unorm && format != MTLPixelFormatDepth32Float &&
		     format != MTLPixelFormatStencil8))
			return false;
		const bool compatible = texture_ != nil && texture_.width == width &&
		                        texture_.height == height && texture_.pixelFormat == format;
		if (compatible && texture_.arrayLength >= layers) return true;
		const uint32_t oldLayers = compatible ? uint32_t(texture_.arrayLength) : 0;
		layers                   = std::max(layers, oldLayers);
		MTLTextureDescriptor* d  = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
		                                                                              width:width
		                                                                             height:height
		                                                                          mipmapped:NO];
		d.textureType            = MTLTextureType2DArray;
		d.arrayLength            = layers;
		d.storageMode            = MTLStorageModePrivate;
		d.usage                  = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		id<MTLTexture> next      = [device newTextureWithDescriptor:d];
		if (!next) return false;
		if (compatible) {
			id<MTLBlitCommandEncoder> b = [command blitCommandEncoder];
			[b copyFromTexture:texture_
			         sourceSlice:0
			         sourceLevel:0
			           toTexture:next
			    destinationSlice:0
			    destinationLevel:0
			          sliceCount:oldLayers
			          levelCount:1];
			[b endEncoding];
		} else
			initialized_.clear();
		initialized_.resize(layers, 0);
		texture_ = next;
		views_.clear();
		return true;
	}
	id<MTLTexture> View(uint32_t first, uint32_t count) {
		if (!texture_ || !count || first >= texture_.arrayLength ||
		    count > texture_.arrayLength - first)
			return nil;
		const auto key = std::pair {first, count};
		auto       it  = views_.find(key);
		if (it != views_.end()) return it->second;
		id<MTLTexture> view = [texture_
		    newTextureViewWithPixelFormat:texture_.pixelFormat
		                      textureType:count == 1 ? MTLTextureType2D : MTLTextureType2DArray
		                           levels:NSMakeRange(0, 1)
		                           slices:NSMakeRange(first, count)];
		if (view) views_[key] = view;
		return view;
	}
	bool MarkImported(uint32_t first, uint32_t count) {
		if (!count || first >= initialized_.size() || count > initialized_.size()-first) return false;
		std::fill(initialized_.begin()+first,initialized_.begin()+first+count,1);
		return true;
	}
	bool NeedsClear(uint32_t layer) const {
		return layer >= initialized_.size() || !initialized_[layer];
	}
	bool Clear(id<MTLCommandBuffer> command, uint32_t first, uint32_t count, double depth,
	           uint32_t stencil, bool onlyNew = false) {
		if (!command || !texture_ || !count || first >= texture_.arrayLength ||
		    count > texture_.arrayLength - first)
			return false;
		for (uint32_t layer = first; layer < first + count; ++layer) {
			if (onlyNew && !NeedsClear(layer)) continue;
			id<MTLTexture> view = View(layer, 1);
			if (!view) return false;
			MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor new];
			if (texture_.pixelFormat == MTLPixelFormatStencil8) {
				pass.stencilAttachment.texture      = view;
				pass.stencilAttachment.loadAction   = MTLLoadActionClear;
				pass.stencilAttachment.storeAction  = MTLStoreActionStore;
				pass.stencilAttachment.clearStencil = stencil;
			} else {
				pass.depthAttachment.texture     = view;
				pass.depthAttachment.loadAction  = MTLLoadActionClear;
				pass.depthAttachment.storeAction = MTLStoreActionStore;
				pass.depthAttachment.clearDepth  = depth;
			}
			id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
			if (!encoder) return false;
			[encoder endEncoding];
			initialized_[layer] = 1;
		}
		return true;
	}

private:
	id<MTLTexture>                                          texture_ = nil;
	std::vector<uint8_t>                                    initialized_;
	std::map<std::pair<uint32_t, uint32_t>, id<MTLTexture>> views_;
};
} // namespace Libs::Graphics::Metal
