#include "graphics/host_gpu/metal/metalViewport.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using Libs::Graphics::Metal::TranslateGuestViewport;
static void Check(bool v, const char* m) {
	if (!v) throw std::runtime_error(m);
}
struct Vertex {
	float x, y, z, w;
};
static constexpr Vertex vertices[] = {
    {-.75f, -.7f, .25f, 1.f}, {.65f, -.45f, .25f, 1.f}, {-.35f, .8f, .25f, 1.f}};
static double Cross(double ax, double ay, double bx, double by) {
	return ax * by - ay * bx;
}
int main() {
	@autoreleasepool {
		try {
			id<MTLDevice> d = MTLCreateSystemDefaultDevice();
			Check(d != nil, "Metal device unavailable");
			NSError*  error = nil;
			NSString* src =
			    @"#include <metal_stdlib>\nusing namespace metal; struct Out{float4 position "
				@"[[position]];}; vertex Out vs(uint id [[vertex_id]],constant float4*p "
				@"[[buffer(0)]]){return {p[id]};} fragment float4 ps(bool front "
				@"[[front_facing]]){return float4(1.0f,front?1.0f:0.0f,0.0f,1.0f);}";
			id<MTLLibrary> lib = [d newLibraryWithSource:src options:nil error:&error];
			Check(lib != nil, error.localizedDescription.UTF8String);
			MTLRenderPipelineDescriptor* pd     = [MTLRenderPipelineDescriptor new];
			pd.vertexFunction                   = [lib newFunctionWithName:@"vs"];
			pd.fragmentFunction                 = [lib newFunctionWithName:@"ps"];
			pd.colorAttachments[0].pixelFormat  = MTLPixelFormatRGBA8Unorm;
			pd.depthAttachmentPixelFormat       = MTLPixelFormatDepth32Float;
			id<MTLRenderPipelineState> pipeline = [d newRenderPipelineStateWithDescriptor:pd
			                                                                        error:&error];
			Check(pipeline != nil, "viewport pipeline");
			MTLDepthStencilDescriptor* ds        = [MTLDepthStencilDescriptor new];
			ds.depthCompareFunction              = MTLCompareFunctionAlways;
			ds.depthWriteEnabled                 = YES;
			id<MTLDepthStencilState> depthState  = [d newDepthStencilStateWithDescriptor:ds];
			id<MTLCommandQueue>      queue       = [d newCommandQueue];
			unsigned                 caseCount   = 0;
			size_t                   totalPixels = 0, totalWrong = 0;
			for (double height: {48., -48., 32., -32.})
				for (double znear: {0., .2, .9})
					for (double zfar: {1., .7, .1}) {
						const double          x = 8., y = height > 0 ? 8. : 56., width = 48.;
						MTLTextureDescriptor* td             = [MTLTextureDescriptor
						    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
						                                 width:64
						                                height:64
						                             mipmapped:NO];
						td.usage                             = MTLTextureUsageRenderTarget;
						td.storageMode                       = MTLStorageModeShared;
						id<MTLTexture> color                 = [d newTextureWithDescriptor:td];
						td.pixelFormat                       = MTLPixelFormatDepth32Float;
						td.storageMode                       = MTLStorageModePrivate;
						id<MTLTexture>           depth       = [d newTextureWithDescriptor:td];
						MTLRenderPassDescriptor* pass        = [MTLRenderPassDescriptor new];
						pass.colorAttachments[0].texture     = color;
						pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
						pass.colorAttachments[0].storeAction = MTLStoreActionStore;
						pass.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 0);
						pass.depthAttachment.texture         = depth;
						pass.depthAttachment.loadAction      = MTLLoadActionClear;
						pass.depthAttachment.storeAction     = MTLStoreActionStore;
						pass.depthAttachment.clearDepth      = 1.;
						id<MTLCommandBuffer>        cmd      = [queue commandBuffer];
						id<MTLRenderCommandEncoder> enc =
						    [cmd renderCommandEncoderWithDescriptor:pass];
						[enc setRenderPipelineState:pipeline];
						[enc setDepthStencilState:depthState];
						[enc setVertexBytes:vertices length:sizeof(vertices) atIndex:0];
						[enc setCullMode:MTLCullModeNone];
						[enc setFrontFacingWinding:MTLWindingCounterClockwise];
						[enc setViewport:TranslateGuestViewport(x, y, width, height, znear, zfar)];
						[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
						[enc endEncoding];
						id<MTLBuffer> depthBytes =
						    [d newBufferWithLength:64 * 64 * 4
						                   options:MTLResourceStorageModeShared];
						id<MTLBlitCommandEncoder> bl = [cmd blitCommandEncoder];
						[bl copyFromTexture:depth
						                 sourceSlice:0
						                 sourceLevel:0
						                sourceOrigin:MTLOriginMake(0, 0, 0)
						                  sourceSize:MTLSizeMake(64, 64, 1)
						                    toBuffer:depthBytes
						           destinationOffset:0
						      destinationBytesPerRow:256
						    destinationBytesPerImage:64 * 256];
						[bl endEncoding];
						[cmd commit];
						[cmd waitUntilCompleted];
						Check(cmd.status == MTLCommandBufferStatusCompleted,
						      "viewport render execution");
						std::array<uint8_t, 64 * 64 * 4> pixels {};
						[color getBytes:pixels.data()
						    bytesPerRow:256
						     fromRegion:MTLRegionMake2D(0, 0, 64, 64)
						    mipmapLevel:0];
						double px[3], py[3];
						for (unsigned i = 0; i < 3; ++i) {
							px[i] = x + width * (vertices[i].x + 1.) * .5;
							py[i] = y + height * (vertices[i].y + 1.) * .5;
						}
						const double area =
						    Cross(px[1] - px[0], py[1] - py[0], px[2] - px[0], py[2] - py[0]);
						const float expectedDepth = float(znear + (zfar - znear) * .25);
						size_t      wrong = 0, checked = 0;
						for (unsigned row = 0; row < 64; ++row)
							for (unsigned col = 0; col < 64; ++col) {
								const double cx = col + .5, cy = row + .5;
								const double b = Cross(cx - px[0], cy - py[0], px[2] - px[0],
								                       py[2] - py[0]) /
								                 area,
								             c = Cross(px[1] - px[0], py[1] - py[0], cx - px[0],
											           cy - py[0]) /
											     area,
								             a = 1. - b - c;
								// Pixel centers away from triangle edges avoid
								// implementation-specific fill ties.
								if (std::min({std::abs(a), std::abs(b), std::abs(c)}) < .015)
									continue;
								const bool inside = a > 0 && b > 0 && c > 0;
								const bool actual = pixels[(row * 64 + col) * 4] != 0;
								++checked;
								if (actual != inside) {
									++wrong;
									continue;
								}
								if (inside) {
									const float actualDepth =
									    ((const float*)depthBytes.contents)[row * 64 + col];
									if (std::abs(actualDepth - expectedDepth) > 1e-6f) ++wrong;
								}
							}
						if (wrong)
							fprintf(stderr, "viewport h=%g zn=%g zf=%g wrong=%zu/%zu\n", height,
							        znear, zfar, wrong, checked);
						totalWrong += wrong;
						totalPixels += checked;
						++caseCount;
					}
			printf("Signed guest viewport: %u GPU cases, %zu checked pixels, %zu mismatches\n",
			       caseCount, totalPixels, totalWrong);
			Check(totalWrong == 0, "signed guest viewport Y/depth mapping regression");
			return 0;
		} catch (const std::exception& e) {
			fprintf(stderr, "%s\n", e.what());
			return 1;
		}
	}
}
