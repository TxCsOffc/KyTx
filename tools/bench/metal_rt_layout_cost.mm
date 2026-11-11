// Mini benchmark: what the layout of a render target costs on Apple's tile-based GPU.
//
// Build/run: tools/bench/run_metal_rt_layout_cost.sh
//
// KyTx keeps a PS5 render target as a LINEAR texture over a wrapped buffer of guest memory (so compute and CPU see the same bytes).
// Metal's normal render target is a PRIVATE texture, which the driver may tile and compress losslessly. Each pass here draws a full-screen
// triangle into a 1920x1080 target, N times, one encoder per pass (as the emulator does), and reports the GPU time per pass:
//   private          MTLStorageModePrivate texture
//   shared           MTLStorageModeShared texture (the driver can still choose its layout)
//   linear buffer    a texture made from an MTLBuffer with bytesPerRow (what the emulator uses for guest render targets)
// "clear" starts every pass with a clear, "load" with a load of the previous content (a PS5 pass that draws over what it finds), "blend"
// loads and blends (read-modify-write of every pixel).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstdio>
#include <vector>

static NSString* const kSource = @R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; };
vertex VOut vs(uint vid [[vertex_id]]) {
	VOut o; float2 p = float2((vid << 1) & 2, vid & 2); o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o;
}
fragment float4 fs(VOut in [[stage_in]]) {
	float2 uv = in.pos.xy * (1.0 / 1920.0);
	return float4(uv.x, uv.y, 0.25 + 0.5 * fract(uv.x * 40.0), 0.5);
}
)";

enum class Layout { Private, Shared, Linear };
enum class Load { Clear, Load, Blend };

static double Median(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

static double Run(id<MTLDevice> dev, id<MTLCommandQueue> queue, MTLPixelFormat fmt, Layout layout, Load load, uint32_t passes) {
	const NSUInteger w = 1920, h = 1080;
	MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
	d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	id<MTLTexture> tex = nil;
	if (layout == Layout::Linear) {
		const NSUInteger align = [dev minimumLinearTextureAlignmentForPixelFormat:fmt];
		const NSUInteger row = ((w * 8 + align - 1) / align) * align; // 8 bytes covers every format used here
		id<MTLBuffer> buf = [dev newBufferWithLength:row * h options:MTLResourceStorageModeShared];
		d.storageMode = MTLStorageModeShared;
		tex = [buf newTextureWithDescriptor:d offset:0 bytesPerRow:row];
	} else {
		d.storageMode = layout == Layout::Private ? MTLStorageModePrivate : MTLStorageModeShared;
		tex = [dev newTextureWithDescriptor:d];
	}
	NSError* err = nil;
	id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:nil error:&err];
	MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"vs"];
	pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
	pd.colorAttachments[0].pixelFormat = fmt;
	if (load == Load::Blend) {
		pd.colorAttachments[0].blendingEnabled = YES;
		pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
		pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	}
	id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
	if (!pso) { std::fprintf(stderr, "pipeline: %s\n", err.localizedDescription.UTF8String); return -1; }
	id<MTLCommandBuffer> cb = [queue commandBuffer];
	for (uint32_t i = 0; i < passes; ++i) {
		MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = tex;
		rp.colorAttachments[0].loadAction = load == Load::Clear ? MTLLoadActionClear : MTLLoadActionLoad;
		rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
		[e setRenderPipelineState:pso];
		[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[e endEncoding];
	}
	[cb commit];
	[cb waitUntilCompleted];
	return (cb.GPUEndTime - cb.GPUStartTime) * 1000.0 / passes;
}

int main() {
	@autoreleasepool {
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		id<MTLCommandQueue> queue = [dev newCommandQueue];
		const uint32_t passes = 200;
		std::printf("device: %s, 1920x1080, %u passes per run (one encoder each), GPU ms per pass, median of 5\n\n", dev.name.UTF8String, passes);
		const struct { const char* name; MTLPixelFormat fmt; } fmts[] = {
		    {"RG11B10Float (PS5 HDR light buffers)", MTLPixelFormatRG11B10Float}, {"RGBA16Float", MTLPixelFormatRGBA16Float}, {"RGBA8Unorm", MTLPixelFormatRGBA8Unorm}};
		const struct { const char* name; Layout l; } layouts[] = {{"private      ", Layout::Private}, {"shared       ", Layout::Shared}, {"linear buffer", Layout::Linear}};
		const struct { const char* name; Load l; } loads[] = {{"clear", Load::Clear}, {"load ", Load::Load}, {"blend", Load::Blend}};
		for (const auto& f: fmts) {
			std::printf("%s\n  %-14s %10s %10s %10s\n", f.name, "layout", "clear", "load", "blend");
			for (const auto& l: layouts) {
				std::printf("  %-14s", l.name);
				for (const auto& ld: loads) {
					std::vector<double> t;
					Run(dev, queue, f.fmt, l.l, ld.l, passes);
					for (int k = 0; k < 5; ++k) t.push_back(Run(dev, queue, f.fmt, l.l, ld.l, passes));
					std::printf(" %10.3f", Median(t));
				}
				std::printf("\n");
			}
			std::printf("\n");
		}
	}
	return 0;
}
