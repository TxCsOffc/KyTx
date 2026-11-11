#pragma once

#include "graphics/host_gpu/metal/metalBuffer.h"

namespace Libs::Graphics::Metal {

// A virtual address can be remapped while its dimensions/format stay unchanged.
// Match the current buffer allocation, not just the guest descriptor. The buffer
// manager resolves physical backing and revalidates mappings before this check.
inline bool GuestMemoryViewMatches(id<MTLTexture> texture, const MetalBufferAllocation& allocation,
                                   MTLPixelFormat format, NSUInteger width, NSUInteger height,
                                   NSUInteger bytes_per_row) {
	return texture != nil && allocation.IsValid() && height != 0 && bytes_per_row != 0 &&
	       height <= allocation.size/bytes_per_row && allocation.offset <= allocation.buffer.length &&
	       height <= (allocation.buffer.length-allocation.offset)/bytes_per_row && texture.buffer == allocation.buffer &&
	       texture.bufferOffset == allocation.offset && texture.bufferBytesPerRow == bytes_per_row &&
	       texture.pixelFormat == format && texture.width == width && texture.height == height;
}

inline id<MTLTexture> CreateGuestMemoryView(const MetalBufferAllocation& allocation,
                                          MTLTextureDescriptor* descriptor, NSUInteger bytes_per_row) {
	if (!allocation.IsValid() || descriptor == nil || descriptor.height == 0 || bytes_per_row == 0 ||
	    descriptor.height > allocation.size / bytes_per_row || allocation.offset > allocation.buffer.length ||
	    descriptor.height > (allocation.buffer.length - allocation.offset) / bytes_per_row)
		return nil;
	return [allocation.buffer newTextureWithDescriptor:descriptor offset:allocation.offset bytesPerRow:bytes_per_row];
}

} // namespace Libs::Graphics::Metal
