// Exercise the production guest scissor calculation on a color + D16 shadow atlas.
#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/debug.h"

#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>
using namespace Libs::Graphics;
static void Check(bool value, const char* message) {
	if (!value) throw std::runtime_error(message);
}
// Standalone test diagnostic sinks; unexpected assertions or warnings fail the test.
namespace Common {
int DbgExitIfHandler(const char*, const char*, int) {
	throw std::runtime_error("guest scissor assertion");
}
void DbgExit(int) {
	throw std::runtime_error("guest scissor exit");
}
} // namespace Common
namespace Log {
bool IsSilent() {
	return false;
}
void Write(fmt::text_style, std::string_view message) {
	throw std::runtime_error(std::string(message));
}
} // namespace Log
struct Case {
	HW::ScreenViewport  viewport;
	HW::ScanModeControl mode;
};
static bool Inside(int x, int y, int l, int t, int r, int b, bool offset,
                   const HW::ScreenViewport& v) {
	if (offset) {
		x -= v.window_offset_x;
		y -= v.window_offset_y;
	}
	return x >= l && y >= t && x < r && y < b;
}
// Evaluate membership per pixel instead of reproducing rectangle intersections.
static bool Reference(const Case& c, int x, int y) {
	const auto& v = c.viewport;
	const auto& p = v.viewports[0];
	if (!Inside(x, y, v.screen_scissor_left, v.screen_scissor_top, v.screen_scissor_right,
	            v.screen_scissor_bottom, false, v) ||
	    !Inside(x, y, v.window_scissor_left, v.window_scissor_top, v.window_scissor_right,
	            v.window_scissor_bottom, v.window_scissor_window_offset_enable, v) ||
	    !Inside(x, y, v.generic_scissor_left, v.generic_scissor_top, v.generic_scissor_right,
	            v.generic_scissor_bottom, v.generic_scissor_window_offset_enable, v) ||
	    (c.mode.vport_scissor_enable &&
	     !Inside(x, y, p.viewport_scissor_left, p.viewport_scissor_top, p.viewport_scissor_right,
	             p.viewport_scissor_bottom, p.viewport_scissor_window_offset_enable, v)))
		return false;
	unsigned combination = 0;
	for (unsigned i = 0; i < 4; ++i)
		if (Inside(x, y, v.clip_rect_left[i], v.clip_rect_top[i], v.clip_rect_right[i],
		           v.clip_rect_bottom[i], v.clip_rect_window_offset_enable[i], v))
			combination |= 1u << i;
	return (v.clip_rect_rule & (1u << combination)) != 0;
}
static std::vector<Case> MakeCases() {
	std::vector<Case> cases;
	std::mt19937      rng(0x5c1550);
	for (unsigned index = 0; index < 256; ++index) {
		Case  c {};
		auto& v    = c.viewport;
		auto& p    = v.viewports[0];
		auto  rect = [&](int& l, int& t, int& r, int& b) {
			l = int(rng() % 35) - 10;
			t = int(rng() % 35) - 10;
			r = 32 + int(rng() % 45);
			b = 32 + int(rng() % 45);
		};
		rect(v.screen_scissor_left, v.screen_scissor_top, v.screen_scissor_right,
		     v.screen_scissor_bottom);
		rect(v.window_scissor_left, v.window_scissor_top, v.window_scissor_right,
		     v.window_scissor_bottom);
		rect(v.generic_scissor_left, v.generic_scissor_top, v.generic_scissor_right,
		     v.generic_scissor_bottom);
		rect(p.viewport_scissor_left, p.viewport_scissor_top, p.viewport_scissor_right,
		     p.viewport_scissor_bottom);
		v.window_offset_x                       = int(rng() % 25) - 12;
		v.window_offset_y                       = int(rng() % 25) - 12;
		v.window_scissor_window_offset_enable   = index & 1;
		v.generic_scissor_window_offset_enable  = index & 2;
		p.viewport_scissor_window_offset_enable = index & 4;
		c.mode.vport_scissor_enable             = index & 8;
		const unsigned mask                     = index % 16;
		v.clip_rect_rule                        = 0;
		for (unsigned i = 0; i < 4; ++i) {
			rect(v.clip_rect_left[i], v.clip_rect_top[i], v.clip_rect_right[i],
			     v.clip_rect_bottom[i]);
			v.clip_rect_window_offset_enable[i] = (index >> (i + 1)) & 1;
		}
		for (unsigned combination = 0; combination < 16; ++combination)
			if ((combination & mask) == mask) v.clip_rect_rule |= 1u << combination;
		if (index % 17 == 0) v.clip_rect_rule = 0; // Empty clip rule.
		if (index % 19 == 0)
			v.generic_scissor_right = v.generic_scissor_left - 1; // Empty rectangle.
		cases.push_back(c);
	}
	return cases;
}
int main(int argc, const char* argv[]) {
	@autoreleasepool {
		try {
			const bool screenOnly =
			    argc == 2 && std::strcmp(argv[1], "--screen-only-negative-control") == 0;
			id<MTLDevice> device = MTLCreateSystemDefaultDevice();
			Check(device != nil, "Metal unavailable");
			NSError*       error   = nil;
			id<MTLLibrary> library = [device
			    newLibraryWithSource:
			        @"#include <metal_stdlib>\nusing namespace metal;vertex float4 vs(uint id "
					@"[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return "
					@"float4(p[id],.75f,1);}fragment float4 ps(){return float4(1);}"
			                 options:nil
			                   error:&error];
			Check(library != nil, error.localizedDescription.UTF8String);
			MTLRenderPipelineDescriptor* pd    = [MTLRenderPipelineDescriptor new];
			pd.vertexFunction                  = [library newFunctionWithName:@"vs"];
			pd.fragmentFunction                = [library newFunctionWithName:@"ps"];
			pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
			pd.depthAttachmentPixelFormat      = MTLPixelFormatDepth16Unorm;
			id<MTLRenderPipelineState> pipeline =
			    [device newRenderPipelineStateWithDescriptor:pd error:&error];
			Check(pipeline != nil, "scissor pipeline");
			MTLDepthStencilDescriptor* sd    = [MTLDepthStencilDescriptor new];
			sd.depthCompareFunction          = MTLCompareFunctionAlways;
			sd.depthWriteEnabled             = YES;
			id<MTLDepthStencilState> ds      = [device newDepthStencilStateWithDescriptor:sd];
			id<MTLCommandQueue>      queue   = [device newCommandQueue];
			size_t                   checked = 0, wrong = 0;
			unsigned                 caseCount = 0;
			for (const auto& c: MakeCases()) {
				auto rect = calc_final_scissor(c.viewport, c.mode, {64, 64}, 0);
				if (screenOnly) {
					const auto& v = c.viewport;
					const uint32_t x = static_cast<uint32_t>(v.screen_scissor_left);
					const uint32_t y = static_cast<uint32_t>(v.screen_scissor_top);
					const uint32_t width = x >= 64 ? 0 : std::min<uint32_t>(
					    std::max(v.screen_scissor_right - v.screen_scissor_left, 0), 64 - x);
					const uint32_t height = y >= 64 ? 0 : std::min<uint32_t>(
					    std::max(v.screen_scissor_bottom - v.screen_scissor_top, 0), 64 - y);
					rect = width != 0 && height != 0
					           ? ScissorRect {int(x), int(y), int(x + width), int(y + height)}
					           : ScissorRect {};
				}
				MTLTextureDescriptor* td = [MTLTextureDescriptor
				    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				                                 width:64
				                                height:64
				                             mipmapped:NO];
				td.usage                 = MTLTextureUsageRenderTarget;
				td.storageMode           = MTLStorageModeShared;
				id<MTLTexture> color     = [device newTextureWithDescriptor:td];
				td.pixelFormat           = MTLPixelFormatDepth16Unorm;
				td.storageMode           = MTLStorageModePrivate;
				id<MTLTexture> depth     = [device newTextureWithDescriptor:td];
				Check(depth != nil, "D16 allocation");
				MTLRenderPassDescriptor* pass        = [MTLRenderPassDescriptor new];
				pass.colorAttachments[0].texture     = color;
				pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.depthAttachment.texture         = depth;
				pass.depthAttachment.loadAction      = MTLLoadActionClear;
				pass.depthAttachment.storeAction     = MTLStoreActionStore;
				pass.depthAttachment.clearDepth      = .25;
				id<MTLCommandBuffer>        cb       = [queue commandBuffer];
				id<MTLRenderCommandEncoder> enc      = [cb renderCommandEncoderWithDescriptor:pass];
				[enc setRenderPipelineState:pipeline];
				[enc setDepthStencilState:ds];
				[enc setCullMode:MTLCullModeNone];
				[enc setViewport:MTLViewport {0, 0, 64, 64, 0, 1}];
				if (rect.right > rect.left && rect.bottom > rect.top) {
					[enc setScissorRect:MTLScissorRect {NSUInteger(rect.left), NSUInteger(rect.top),
					                                    NSUInteger(rect.right - rect.left),
					                                    NSUInteger(rect.bottom - rect.top)}];
					[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				}
				[enc endEncoding];
				id<MTLBuffer> bytes = [device newBufferWithLength:64 * 256
				                                          options:MTLResourceStorageModeShared];
				id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
				[blit copyFromTexture:depth
				                 sourceSlice:0
				                 sourceLevel:0
				                sourceOrigin:MTLOriginMake(0, 0, 0)
				                  sourceSize:MTLSizeMake(64, 64, 1)
				                    toBuffer:bytes
				           destinationOffset:0
				      destinationBytesPerRow:256
				    destinationBytesPerImage:64 * 256];
				[blit endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
				Check(cb.status == MTLCommandBufferStatusCompleted, "scissor GPU execution");
				std::array<uint8_t, 64 * 64 * 4> pixels {};
				[color getBytes:pixels.data()
				    bytesPerRow:256
				     fromRegion:MTLRegionMake2D(0, 0, 64, 64)
				    mipmapLevel:0];
				for (int y = 0; y < 64; ++y)
					for (int x = 0; x < 64; ++x) {
						const bool expected = Reference(c, x, y);
						uint16_t   z;
						std::memcpy(
						    &z, static_cast<const uint8_t*>(bytes.contents) + y * 256 + x * 2, 2);
						const int wanted = expected ? 49151 : 16384;
						if ((pixels[(y * 64 + x) * 4] != 0) != expected ||
						    std::abs(int(z) - wanted) > 1)
							++wrong;
						++checked;
					}
				++caseCount;
			}
			std::printf("Guest scissor%s: %u GPU cases, %zu color+D16 pixels, %zu mismatches\n",
			            screenOnly ? " (old screen-only control)" : "", caseCount, checked, wrong);
			Check(wrong == 0, "guest scissor color/depth regression");
			return 0;
		} catch (const std::exception& e) {
			std::fprintf(stderr, "%s\n", e.what());
			return 1;
		}
	}
}
