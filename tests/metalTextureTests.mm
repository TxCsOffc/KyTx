// Standalone regression: compressed formats must not enter Metal's linear-texture path.
#include "graphics/host_gpu/metal/metalTexture.h"
#include "graphics/host_gpu/metal/metalContext.h"
#include "common/logging/log.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace Libs::Graphics::Metal {
MetalContext::MetalContext() { m_device = MTLCreateSystemDefaultDevice(); }
MetalContext::~MetalContext() = default;
MetalContext& MetalContext::Instance() { static MetalContext context; return context; }
}

namespace Log {
bool IsSilent() { return false; }
void Write(fmt::text_style, std::string_view text) { std::fprintf(stderr, "%.*s", int(text.size()), text.data()); }
}

static void Check(bool ok, const char* message) {
	if (!ok) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}

int main() {
	@autoreleasepool {
		using namespace Libs::Graphics::Metal;
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		Check(device != nil, "Metal device unavailable");

		unsigned failures = 0;
		const NSUInteger rgba_align = LinearTextureRowAlignment(device, MTLPixelFormatRGBA8Unorm);
		if (rgba_align == 0) {
			std::fprintf(stderr, "RGBA8 linear alignment was 0\n");
			++failures;
		}
		const NSUInteger bc3_align = LinearTextureRowAlignment(device, MTLPixelFormatBC3_RGBA);
		if (bc3_align != 0) {
			std::fprintf(stderr, "BC3 linear alignment was %lu, expected 0\n",
			             static_cast<unsigned long>(bc3_align));
			++failures;
		}
		const NSUInteger bc1_align = LinearTextureRowAlignment(device, MTLPixelFormatBC1_RGBA);
		if (bc1_align != 0) {
			std::fprintf(stderr, "BC1 linear alignment was %lu, expected 0\n",
			             static_cast<unsigned long>(bc1_align));
			++failures;
		}

		// Ordinary 2D upload of compressed blocks (not a linear buffer view).
		MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC3_RGBA
		                                                                              width:4
		                                                                             height:4
		                                                                          mipmapped:NO];
		td.usage = MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModeShared;
		td.allowGPUOptimizedContents = NO;
		id<MTLTexture> texture = [device newTextureWithDescriptor:td];
		if (texture == nil) {
			std::fprintf(stderr, "BC3 shared 2D texture allocation failed\n");
			++failures;
		} else {
			std::vector<uint8_t> blocks(16, 0);
			[texture replaceRegion:MTLRegionMake2D(0, 0, 4, 4)
			           mipmapLevel:0
			             withBytes:blocks.data()
			           bytesPerRow:16];
			if (texture.pixelFormat != MTLPixelFormatBC3_RGBA || texture.width != 4 || texture.height != 4) {
				std::fprintf(stderr, "BC3 shared texture layout mismatch\n");
				++failures;
			}
		}

		std::printf("Compressed linear texture path: 4 cases, %u failures\n", failures);
		return failures ? 1 : 0;
	}
}
