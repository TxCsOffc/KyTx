#include "graphics/host_gpu/metal/metalPresenter.h"
#include "graphics/host_gpu/metal/metalBuffer.h"
#include "graphics/host_gpu/metal/metalContext.h"
#include "graphics/host_gpu/metal/metalTexture.h"
#include "common/logging/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Libs::Graphics::Metal {

void DumpTrackedImages(uint64_t present_index); // metalRenderer.mm: KYTY_DUMP_IMG diagnostic, no-op unless its trigger file appears

struct PresentUniforms {
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t format;
	uint32_t is_tiled;
};

struct MetalPresenter::Impl {
	SDL_Window*   window     = nullptr;
	SDL_MetalView metal_view = nullptr;
	CAMetalLayer* layer      = nil;
	uint32_t      width      = 0;
	uint32_t      height     = 0;

	id<MTLRenderPipelineState> blit_tex_pipeline = nil;
	id<MTLRenderPipelineState> blit_buf_pipeline = nil;
	id<MTLSamplerState>        linear_sampler    = nil;

	bool InitPipeline(id<MTLDevice> device) {
		NSError* error = nil;
		NSString* shader_src = @""
			"#include <metal_stdlib>\n"
			"using namespace metal;\n"
			"struct VertexOut {\n"
			"    float4 position [[position]];\n"
			"    float2 uv;\n"
			"};\n"
			"vertex VertexOut vs_present(uint vid [[vertex_id]]) {\n"
			"    VertexOut out;\n"
			"    float2 pos = float2((vid << 1) & 2, vid & 2);\n"
			"    out.position = float4(pos * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
			"    out.uv = pos;\n"
			"    return out;\n"
			"}\n"
			"struct PresentUniforms {\n"
			"    uint width;\n"
			"    uint height;\n"
			"    uint pitch;\n"
			"    uint format;\n"
			"    uint is_tiled;\n"
			"};\n"
			"inline float decode_r11(uint val) {\n"
			"    uint exponent = (val >> 6u) & 0x1Fu;\n"
			"    uint mantissa = val & 0x3Fu;\n"
			"    if (exponent == 0) return float(mantissa) / 64.0 * (1.0 / 16384.0);\n"
			"    return (1.0 + float(mantissa) / 64.0) * exp2(float(exponent) - 15.0);\n"
			"}\n"
			"inline float decode_b10(uint val) {\n"
			"    uint exponent = (val >> 5u) & 0x1Fu;\n"
			"    uint mantissa = val & 0x1Fu;\n"
			"    if (exponent == 0) return float(mantissa) / 32.0 * (1.0 / 16384.0);\n"
			"    return (1.0 + float(mantissa) / 32.0) * exp2(float(exponent) - 15.0);\n"
			"}\n"
			"inline uint calc_rdna2_rt_offset(uint x, uint y, uint width) {\n"
			"    uint block_x = x >> 7u;\n"
			"    uint block_y = y >> 7u;\n"
			"    uint blocks_per_row = (width + 127u) >> 7u;\n"
			"    uint block_idx = block_y * blocks_per_row + block_x;\n"
			"    uint rt_off = ((y << 4) & 0x070u) ^ ((y << 5) & 0xf00u) ^\n"
			"                  ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u) ^\n"
			"                  ((x << 2) & 0x00cu) ^ ((x << 5) & 0x380u) ^\n"
			"                  ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u);\n"
			"    return block_idx * 65536u + rt_off;\n"
			"}\n"
			"fragment float4 fs_present_tex(VertexOut in [[stage_in]],\n"
			"                              texture2d<float> src [[texture(0)]],\n"
			"                              sampler smp [[sampler(0)]],\n"
			"                              constant PresentUniforms& u [[buffer(0)]]) {\n"
			"    float uv_y = (u.is_tiled & 2u) ? (1.0 - in.uv.y) : in.uv.y;\n"
			"    float4 c;\n"
			"    if ((u.is_tiled & 5u) == 4u) {\n"
			"        // On-screen scaling: bilinear. A point read dropped every tenth source pixel when the drawable is smaller than the frame\n"
			"        // (1728x957 for 1920x1080) and gave wobbly edges.\n"
			"        c = src.sample(smp, float2(in.uv.x, uv_y));\n"
			"    } else {\n"
			"    uint x = clamp(uint(in.uv.x * float(u.width)), 0u, u.width - 1u);\n"
			"    uint y = clamp(uint(uv_y * float(u.height)), 0u, u.height - 1u);\n"
			"    uint tex_x = x;\n"
			"    uint tex_y = y;\n"
			"    if (u.is_tiled & 1u) {\n"
			"        uint byte_off = calc_rdna2_rt_offset(x, y, u.width);\n"
			"        uint px_idx = byte_off >> 2u;\n"
			"        tex_x = px_idx % u.width;\n"
			"        tex_y = px_idx / u.width;\n"
			"    }\n"
			"    c = src.read(uint2(tex_x, tex_y));\n"
			"    }\n"
			"    float3 rgb = clamp(c.rgb, 0.0, 1.0);\n"
			"    // Display-referred formats (the game's final composite: 8-bit and RGB10A2 flip images) are already\n"
			"    // encoded for the display, so they are copied straight; only linear HDR targets (RG11B10F, RGBA16F)\n"
			"    // get the sRGB transfer function.\n"
			"    if (u.format == 92u || u.format == 115u) {\n"
			"        rgb = select(rgb * 12.92, 1.055 * pow(rgb, float3(1.0 / 2.4)) - 0.055, rgb > 0.0031308);\n"
			"    }\n"
			"    return float4(rgb, 1.0);\n"
			"}\n"
			"fragment float4 fs_present_buf(VertexOut in [[stage_in]],\n"
			"                              constant uint32_t* raw_pixels [[buffer(0)]],\n"
			"                              constant PresentUniforms& u [[buffer(1)]]) {\n"
			"    if (raw_pixels == nullptr) return float4(0.0, 0.0, 0.0, 1.0);\n"
			"    uint x = clamp(uint(in.uv.x * float(u.width)), 0u, u.width - 1u);\n"
			"    uint y = clamp(uint(in.uv.y * float(u.height)), 0u, u.height - 1u);\n"
			"    uint byte_off = 0;\n"
			"    if (u.is_tiled != 0) {\n"
			"        byte_off = calc_rdna2_rt_offset(x, y, u.width);\n"
			"    } else {\n"
			"        byte_off = y * u.pitch + x * 4u;\n"
			"    }\n"
			"    uint raw = raw_pixels[byte_off >> 2u];\n"
			"    if (u.format == 92) {\n"
			"        float r = decode_r11(raw & 0x7FFu);\n"
			"        float g = decode_r11((raw >> 11u) & 0x7FFu);\n"
			"        float b = decode_b10((raw >> 22u) & 0x3FFu);\n"
			"        return float4(r, g, b, 1.0);\n"
			"    } else if (u.format == 70 || u.format == 71 || u.format == 37 || u.format == 43) {\n"
			"        float r = float(raw & 0xFFu) / 255.0;\n"
			"        float g = float((raw >> 8u) & 0xFFu) / 255.0;\n"
			"        float b = float((raw >> 16u) & 0xFFu) / 255.0;\n"
			"        return float4(r, g, b, 1.0);\n"
			"    } else if (u.format == 80 || u.format == 81 || u.format == 44 || u.format == 50) {\n"
			"        float b = float(raw & 0xFFu) / 255.0;\n"
			"        float g = float((raw >> 8u) & 0xFFu) / 255.0;\n"
			"        float r = float((raw >> 16u) & 0xFFu) / 255.0;\n"
			"        return float4(r, g, b, 1.0);\n"
			"    } else if (u.format == 90 || u.format == 91 || u.format == 58 || u.format == 64) {\n"
			"        float r = float(raw & 0x3FFu) / 1023.0;\n"
			"        float g = float((raw >> 10u) & 0x3FFu) / 1023.0;\n"
			"        float b = float((raw >> 20u) & 0x3FFu) / 1023.0;\n"
			"        return float4(r, g, b, 1.0);\n"
			"    } else {\n"
			"        float r = float(raw & 0xFFu) / 255.0;\n"
			"        float g = float((raw >> 8u) & 0xFFu) / 255.0;\n"
			"        float b = float((raw >> 16u) & 0xFFu) / 255.0;\n"
			"        return float4(r, g, b, 1.0);\n"
			"    }\n"
			"}\n";

		MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
#if defined(MTLLanguageVersion4_0)
		opts.languageVersion    = MTLLanguageVersion4_0;
#elif defined(MTLLanguageVersion3_2)
		opts.languageVersion    = MTLLanguageVersion3_2;
#else
		opts.languageVersion    = MTLLanguageVersion3_0;
#endif


		id<MTLLibrary> lib = [device newLibraryWithSource:shader_src options:opts error:&error];
		if (!lib) {
			LOGF_COLOR(Log::Color::BrightRed, "MetalPresenter: shader compile error: %s\n",
			           error ? [[error localizedDescription] UTF8String] : "unknown");
			return false;
		}

		id<MTLFunction> vs = [lib newFunctionWithName:@"vs_present"];
		id<MTLFunction> fs_tex = [lib newFunctionWithName:@"fs_present_tex"];
		id<MTLFunction> fs_buf = [lib newFunctionWithName:@"fs_present_buf"];

		MTLRenderPipelineDescriptor* desc_tex = [[MTLRenderPipelineDescriptor alloc] init];
		desc_tex.vertexFunction   = vs;
		desc_tex.fragmentFunction = fs_tex;
		desc_tex.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

		blit_tex_pipeline = [device newRenderPipelineStateWithDescriptor:desc_tex error:&error];
		if (!blit_tex_pipeline) {
			LOGF_COLOR(Log::Color::BrightRed, "MetalPresenter: blit_tex_pipeline error: %s\n",
			           error ? [[error localizedDescription] UTF8String] : "unknown");
			return false;
		}

		MTLRenderPipelineDescriptor* desc_buf = [[MTLRenderPipelineDescriptor alloc] init];
		desc_buf.vertexFunction   = vs;
		desc_buf.fragmentFunction = fs_buf;
		desc_buf.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

		blit_buf_pipeline = [device newRenderPipelineStateWithDescriptor:desc_buf error:&error];
		if (!blit_buf_pipeline) {
			LOGF_COLOR(Log::Color::BrightRed, "MetalPresenter: blit_buf_pipeline error: %s\n",
			           error ? [[error localizedDescription] UTF8String] : "unknown");
			return false;
		}

		MTLSamplerDescriptor* s_desc = [[MTLSamplerDescriptor alloc] init];
		s_desc.minFilter = MTLSamplerMinMagFilterLinear;
		s_desc.magFilter = MTLSamplerMinMagFilterLinear;
		linear_sampler   = [device newSamplerStateWithDescriptor:s_desc];

		return true;
	}
};

MetalPresenter& MetalPresenter::Instance() {
	static MetalPresenter instance;
	return instance;
}

MetalPresenter::MetalPresenter() = default;
MetalPresenter::~MetalPresenter() {
	Shutdown();
}

bool MetalPresenter::Initialize(SDL_Window* window, uint32_t width, uint32_t height) {
	if (m_active) return true;
	if (!window) return false;

	@autoreleasepool {
		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		if (!device) {
			if (!MetalContext::Instance().Initialize()) {
				LOGF_COLOR(Log::Color::BrightRed, "MetalPresenter: MetalContext init failed\n");
				return false;
			}
			device = MetalContext::Instance().GetDevice();
		}

		m_impl = std::make_unique<Impl>();
		m_impl->window     = window;
		m_impl->width      = width;
		m_impl->height     = height;
		m_impl->metal_view = SDL_Metal_CreateView(window);
		if (!m_impl->metal_view) {
			LOGF_COLOR(Log::Color::BrightRed, "MetalPresenter: SDL_Metal_CreateView failed: %s\n",
			           SDL_GetError());
			return false;
		}

		m_impl->layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(m_impl->metal_view);
		if (!m_impl->layer) {
			LOGF_COLOR(Log::Color::BrightRed, "MetalPresenter: SDL_Metal_GetLayer failed\n");
			return false;
		}

		m_impl->layer.device          = device;
		m_impl->layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
		m_impl->layer.drawableSize    = CGSizeMake(width, height);
		m_impl->layer.framebufferOnly = YES;
		m_impl->layer.opaque          = YES;

		NSView* nsview = (__bridge NSView*)m_impl->metal_view;
		if (nsview) {
			[nsview setWantsLayer:YES];
			m_impl->layer.frame = nsview.bounds;
			m_impl->layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
			CGFloat scale = [nsview.window backingScaleFactor];
			if (scale <= 0.0) scale = 2.0;
			m_impl->layer.contentsScale = scale;
		}

		if (!m_impl->InitPipeline(device)) {
			LOGF_COLOR(Log::Color::BrightRed, "MetalPresenter: InitPipeline failed\n");
			return false;
		}

		m_active = true;
		std::fprintf(stderr,
		           "[MetalPresenter] Initialized CAMetalLayer (%ux%u) with native Metal 4 (view=%p layer=%p)\n",
		           width, height, nsview, m_impl->layer);
		return true;
	}
}

void MetalPresenter::Shutdown() {
	if (!m_active) return;
	@autoreleasepool {
		if (m_impl && m_impl->metal_view) {
			SDL_Metal_DestroyView(m_impl->metal_view);
		}
		m_impl.reset();
		m_active = false;
	}
}

void MetalPresenter::Resize(uint32_t width, uint32_t height) {
	if (!m_active || !m_impl || !m_impl->layer) return;
	@autoreleasepool {
		m_impl->width  = width;
		m_impl->height = height;
		m_impl->layer.drawableSize = CGSizeMake(width, height);
		NSView* nsview = (__bridge NSView*)m_impl->metal_view;
		if (nsview) {
			m_impl->layer.frame = nsview.bounds;
		}
	}
}

bool MetalPresenter::PresentTexture(void* texture) {
	if (!m_active || !m_impl || !m_impl->layer || !texture) return false;
	id<MTLTexture> src_tex = (__bridge id<MTLTexture>)texture;

	@autoreleasepool {
		// The drawable follows the window's backing pixels (SDL leaves it at the size in points, 1728x957 here, so the 1920x1080 frame
		// was shrunk by 0.9 and then doubled by the compositor). KYTY_PRESENT_NATIVE_SIZE=0 keeps the old size.
		static const bool s_native_size = [] {
			const char* env = std::getenv("KYTY_PRESENT_NATIVE_SIZE");
			return env == nullptr || std::atoi(env) != 0;
		}();
		if (s_native_size) {
			const CGSize  bounds = m_impl->layer.bounds.size;
			const CGFloat scale  = m_impl->layer.contentsScale > 0.0 ? m_impl->layer.contentsScale : 1.0;
			const CGSize  want   = CGSizeMake(std::round(bounds.width * scale), std::round(bounds.height * scale));
			const CGSize  have   = m_impl->layer.drawableSize;
			if (want.width >= 64.0 && want.height >= 64.0 && (std::fabs(want.width - have.width) > 0.5 || std::fabs(want.height - have.height) > 0.5)) {
				m_impl->layer.drawableSize = want;
				std::fprintf(stderr, "[MetalPresenter] drawable %.0fx%.0f -> %.0fx%.0f (layer %.0fx%.0f points, scale %.1f)\n", have.width, have.height, want.width,
				             want.height, bounds.width, bounds.height, scale);
			}
		}
		id<CAMetalDrawable> drawable = [m_impl->layer nextDrawable];
		if (!drawable) {
			return false;
		}

		id<MTLCommandQueue> queue = MetalContext::Instance().GetCommandQueue();
		if (!queue) return false;
		id<MTLCommandBuffer> cmd_buf = [queue commandBuffer];
		if (!cmd_buf) return false;

		static std::atomic<uint64_t> s_tex_present_count {0};
		uint64_t p_idx = s_tex_present_count.fetch_add(1, std::memory_order_relaxed);
		if (p_idx < 5 || p_idx % 300 == 0) {
			std::fprintf(stderr,
			           "[MetalPresenter] PresentTexture #%llu: %lux%lu fmt=%lu\n",
			           (unsigned long long)p_idx, (unsigned long)src_tex.width,
			           (unsigned long)src_tex.height, (unsigned long)src_tex.pixelFormat);
		}

		PresentUniforms u {};
		u.width = static_cast<uint32_t>(src_tex.width);
		u.height = static_cast<uint32_t>(src_tex.height);
		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		NSUInteger align = LinearTextureRowAlignment(device, src_tex.pixelFormat);
		if (align == 0) {
			align = 256;
		}
		if (align < 64) align = 64;
		u.pitch = static_cast<uint32_t>((u.width * 4 + align - 1) & ~(align - 1));
		u.format = static_cast<uint32_t>(src_tex.pixelFormat);
		static int s_env_flip_y = -1;
		if (s_env_flip_y == -1) {
			const char* env = std::getenv("KYTY_FLIP_Y");
			s_env_flip_y = (env && std::atoi(env) != 0) ? 2 : 0;
		}
		static int s_env_detile = -1;
		if (s_env_detile == -1) {
			const char* env = std::getenv("KYTY_DETILE_RT");
			s_env_detile = (env && std::atoi(env) != 0) ? 1 : 0;
		}
		u.is_tiled = static_cast<uint32_t>(s_env_flip_y | s_env_detile);
		// Bit 2: bilinear sampling for the on-screen present (not for the 1:1 frame dump). KYTY_PRESENT_NEAREST=1 keeps the old point read.
		static const bool s_present_nearest = std::getenv("KYTY_PRESENT_NEAREST") != nullptr;
		const uint32_t    present_filter    = s_present_nearest ? 0u : 4u;

		// KYTY_DUMP_FRAME=<n[,n...]> KYTY_DUMP_PATH=<file.ppm>: dump the listed
		// presented frames to disk (PPM P6, top-down) so pixel content can be
		// verified without macOS Screen Recording permission. If the path contains
		// '%' it is used as a printf format with the frame index.
		static std::vector<uint64_t> s_dump_frames;
		static const char* s_dump_path = nullptr;
		static bool s_dump_env_read = false;
		if (!s_dump_env_read) {
			s_dump_env_read = true;
			if (const char* env = std::getenv("KYTY_DUMP_FRAME")) {
				const char* p = env;
				while (*p) {
					char* endp = nullptr;
					unsigned long long v = std::strtoull(p, &endp, 10);
					if (endp == p) break;
					s_dump_frames.push_back(static_cast<uint64_t>(v));
					p = endp;
					while (*p == ',' || *p == ' ') ++p;
				}
			}
			s_dump_path = std::getenv("KYTY_DUMP_PATH");
			if (!s_dump_path || !*s_dump_path) s_dump_path = "frame_dump.ppm";
		}
		bool do_dump = !s_dump_frames.empty() &&
		               std::find(s_dump_frames.begin(), s_dump_frames.end(), p_idx) != s_dump_frames.end();
		// Optional process-local trigger for a diagnostic image from the renderer itself.
		// Consume the marker once; no desktop/window capture or GUI permissions are involved.
		static const char* s_dump_trigger = std::getenv("KYTY_DUMP_TRIGGER");
		if (s_dump_trigger != nullptr && *s_dump_trigger != '\0' && std::remove(s_dump_trigger) == 0) do_dump = true;
		char dump_path_buf[1024];
		const char* dump_path = s_dump_path;
		if (do_dump && std::strchr(s_dump_path, '%')) {
			std::snprintf(dump_path_buf, sizeof(dump_path_buf), s_dump_path,
			              static_cast<unsigned long long>(p_idx));
			dump_path = dump_path_buf;
		}
		id<MTLBuffer> dump_buf = nil;
		NSUInteger dump_row_bytes = 0;
		uint32_t dump_w = 0, dump_h = 0;

		if (do_dump) {
			dump_w = static_cast<uint32_t>(src_tex.width);
			dump_h = static_cast<uint32_t>(src_tex.height);
			dump_row_bytes = (static_cast<NSUInteger>(dump_w) * 4 + 255) & ~static_cast<NSUInteger>(255);
			dump_buf = [device newBufferWithLength:dump_row_bytes * dump_h
			                               options:MTLResourceStorageModeShared];
			MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			                                                                             width:dump_w
			                                                                            height:dump_h
			                                                                         mipmapped:NO];
			dd.usage = MTLTextureUsageRenderTarget;
			dd.storageMode = MTLStorageModePrivate;
			id<MTLTexture> dump_tex = [device newTextureWithDescriptor:dd];
			if (dump_buf && dump_tex) {
				// Use a private command buffer: never wait on the presented one,
				// otherwise an occluded/invisible drawable can wedge the queue.
				id<MTLCommandBuffer> dump_cb = [queue commandBuffer];
				MTLRenderPassDescriptor* dpass = [MTLRenderPassDescriptor renderPassDescriptor];
				dpass.colorAttachments[0].texture     = dump_tex;
				dpass.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
				dpass.colorAttachments[0].storeAction = MTLStoreActionStore;
				id<MTLRenderCommandEncoder> denc = [dump_cb renderCommandEncoderWithDescriptor:dpass];
				if (denc) {
					[denc setRenderPipelineState:m_impl->blit_tex_pipeline];
					[denc setFragmentTexture:src_tex atIndex:0];
					[denc setFragmentSamplerState:m_impl->linear_sampler atIndex:0];
					[denc setFragmentBytes:&u length:sizeof(u) atIndex:0];
					[denc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
					[denc endEncoding];
					id<MTLBlitCommandEncoder> blit = [dump_cb blitCommandEncoder];
					if (blit) {
						[blit copyFromTexture:dump_tex
						          sourceSlice:0
						          sourceLevel:0
						         sourceOrigin:MTLOriginMake(0, 0, 0)
						           sourceSize:MTLSizeMake(dump_w, dump_h, 1)
						             toBuffer:dump_buf
						    destinationOffset:0
						       destinationBytesPerRow:dump_row_bytes
						     destinationBytesPerImage:dump_row_bytes * dump_h];
						[blit endEncoding];
					}
				}
				KytyWatchCommandBuffer(dump_cb, "metalPresenter.mm:dump_cb#1");
				KytyBudgetedCommit(dump_cb);
				[dump_cb waitUntilCompleted];

				FILE* f = std::fopen(dump_path, "wb");
				if (f) {
					std::fprintf(f, "P6\n%u %u\n255\n", dump_w, dump_h);
					const uint8_t* base = static_cast<const uint8_t*>(dump_buf.contents);
					for (uint32_t y = 0; y < dump_h; ++y) {
						const uint8_t* row = base + y * dump_row_bytes;
						for (uint32_t x = 0; x < dump_w; ++x) {
							const uint8_t rgb[3] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0]};
							std::fwrite(rgb, 1, 3, f);
						}
					}
					std::fclose(f);
					std::fprintf(stderr, "[MetalPresenter] dumped frame #%llu to %s\n",
					             (unsigned long long)p_idx, dump_path);
				} else {
					std::fprintf(stderr, "[MetalPresenter] failed to open dump path %s\n", dump_path);
				}
			}
		}
		DumpTrackedImages(p_idx);

		MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture     = drawable.texture;
		pass.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;

		id<MTLRenderCommandEncoder> enc = [cmd_buf renderCommandEncoderWithDescriptor:pass];
		if (enc) {
			[enc setRenderPipelineState:m_impl->blit_tex_pipeline];
			[enc setFragmentTexture:src_tex atIndex:0];
			[enc setFragmentSamplerState:m_impl->linear_sampler atIndex:0];
			u.is_tiled |= present_filter;
			[enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[enc endEncoding];
		}

		[cmd_buf presentDrawable:drawable];
		KytyBudgetedCommit(cmd_buf);
		return true;
	}
}

bool MetalPresenter::DumpTextureToPpm(void* texture, const char* path) {
	if (!m_impl || !texture || !path) {
		return false;
	}
	id<MTLTexture> src = (__bridge id<MTLTexture>)texture;
	@autoreleasepool {
		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		id<MTLCommandQueue> queue = MetalContext::Instance().GetCommandQueue();
		if (!device || !queue) {
			return false;
		}
		if (src.textureType == MTLTextureType3D) {
			// The blit below samples a 2D texture: copy one depth slice into a 2D texture first.
			const char* slice_env = std::getenv("KYTY_DUMP_3D_SLICE");
			const NSUInteger slice = slice_env != nullptr ? static_cast<NSUInteger>(std::strtoul(slice_env, nullptr, 10)) : 0;
			MTLTextureDescriptor* fd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat
			                                                                              width:src.width
			                                                                             height:src.height
			                                                                          mipmapped:NO];
			fd.storageMode = MTLStorageModePrivate;
			id<MTLTexture> flat = [device newTextureWithDescriptor:fd];
			if (!flat) {
				return false;
			}
			id<MTLCommandBuffer> sb = [queue commandBuffer];
			id<MTLBlitCommandEncoder> bl = [sb blitCommandEncoder];
			[bl copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, slice)
			         sourceSize:MTLSizeMake(src.width, src.height, 1) toTexture:flat destinationSlice:0
			         destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
			[bl endEncoding];
			KytyWatchCommandBuffer(sb, "metalPresenter.mm:sb#2");
			KytyBudgetedCommit(sb);
			[sb waitUntilCompleted];
			std::fprintf(stderr, "[DumpTex] 3D source %lux%lux%lu slice %lu copied\n", (unsigned long)src.width,
			             (unsigned long)src.height, (unsigned long)src.depth, (unsigned long)slice);
			src = flat;
		}
		const uint32_t width  = static_cast<uint32_t>(src.width);
		const uint32_t height = static_cast<uint32_t>(src.height);
		if (width == 0 || height == 0) {
			return false;
		}
		const NSUInteger row_bytes = (static_cast<NSUInteger>(width) * 4 + 255) & ~static_cast<NSUInteger>(255);
		id<MTLBuffer> buffer = [device newBufferWithLength:row_bytes * height
		                                          options:MTLResourceStorageModeShared];

		MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		                                                                             width:width
		                                                                            height:height
		                                                                         mipmapped:NO];
		dd.usage       = MTLTextureUsageRenderTarget;
		dd.storageMode = MTLStorageModePrivate;
		id<MTLTexture> dst = [device newTextureWithDescriptor:dd];
		if (!buffer || !dst) {
			return false;
		}

		PresentUniforms u {};
		u.width  = width;
		u.height = height;
		u.pitch  = static_cast<uint32_t>(row_bytes);
		u.format = static_cast<uint32_t>(src.pixelFormat);
		u.is_tiled = 0;

		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture     = dst;
		pass.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
		if (enc) {
			[enc setRenderPipelineState:m_impl->blit_tex_pipeline];
			[enc setFragmentTexture:src atIndex:0];
			[enc setFragmentSamplerState:m_impl->linear_sampler atIndex:0];
			[enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[enc endEncoding];
		}
		id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
		if (blit) {
			[blit copyFromTexture:dst
			          sourceSlice:0
			          sourceLevel:0
			         sourceOrigin:MTLOriginMake(0, 0, 0)
			           sourceSize:MTLSizeMake(width, height, 1)
			             toBuffer:buffer
			    destinationOffset:0
			       destinationBytesPerRow:row_bytes
			     destinationBytesPerImage:row_bytes * height];
			[blit endEncoding];
		}
		KytyWatchCommandBuffer(cmd, "metalPresenter.mm:cmd#3");
		KytyBudgetedCommit(cmd);
		[cmd waitUntilCompleted];

		FILE* f = std::fopen(path, "wb");
		if (!f) {
			return false;
		}
		std::fprintf(f, "P6\n%u %u\n255\n", width, height);
		const uint8_t* base = static_cast<const uint8_t*>(buffer.contents);
		for (uint32_t y = 0; y < height; ++y) {
			const uint8_t* row = base + y * row_bytes;
			for (uint32_t x = 0; x < width; ++x) {
				const uint8_t rgb[3] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0]};
				std::fwrite(rgb, 1, 3, f);
			}
		}
		std::fclose(f);
		return true;
	}
}

bool MetalPresenter::PresentFrame(const void* guest_vaddr, uint32_t width, uint32_t height,
                                  uint32_t stride_bytes, uint32_t format) {
	if (!m_active || !m_impl || !m_impl->layer) return false;

	@autoreleasepool {
		id<CAMetalDrawable> drawable = [m_impl->layer nextDrawable];
		if (!drawable) {
			static std::atomic<uint32_t> s_null_draw {0};
			if (s_null_draw.fetch_add(1) % 120 == 0) {
				std::fprintf(stderr, "[MetalPresenter] nextDrawable is NIL! layer=%p bounds=(%.1f, %.1f) drawableSize=(%.1f, %.1f)\n",
				             m_impl->layer,
				             m_impl->layer.bounds.size.width, m_impl->layer.bounds.size.height,
				             m_impl->layer.drawableSize.width, m_impl->layer.drawableSize.height);
			}
			return false;
		}

		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		if (!device) return false;
		id<MTLCommandQueue> queue = MetalContext::Instance().GetCommandQueue();
		if (!queue) return false;
		id<MTLCommandBuffer> cmd_buf = [queue commandBuffer];
		if (!cmd_buf) return false;

		static std::atomic<uint64_t> s_present_count {0};
		uint64_t p_idx = s_present_count.fetch_add(1, std::memory_order_relaxed);
		if (p_idx < 5 || p_idx % 300 == 0) {
			uint64_t total_nz = 0;
			uint32_t first_nz_val = 0;
			size_t first_nz_idx = 0;
			if (guest_vaddr && width > 0 && height > 0) {
				const uint32_t* p32 = reinterpret_cast<const uint32_t*>(guest_vaddr);
				const size_t word_count = width * height;
				for (size_t i = 0; i < word_count; ++i) {
					if (p32[i] != 0) {
						if (total_nz == 0) {
							first_nz_idx = i;
							first_nz_val = p32[i];
						}
						total_nz++;
					}
				}
			}
			std::fprintf(stderr,
			           "[MetalPresenter] PresentBuffer #%llu: %ux%u fmt=%u bpr=%u addr=%p (total_nz=%llu/%zu, first_nz=[%zu]=0x%08x)\n",
			           (unsigned long long)p_idx, width, height, format, stride_bytes, guest_vaddr,
			           (unsigned long long)total_nz, (size_t)(width * height), first_nz_idx, first_nz_val);
		}

		if (!guest_vaddr) {
			MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture = drawable.texture;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> enc = [cmd_buf renderCommandEncoderWithDescriptor:pass];
			if (enc) [enc endEncoding];
			[cmd_buf presentDrawable:drawable];
			KytyBudgetedCommit(cmd_buf);
			return true;
		}

		MTLPixelFormat fmt = MTLPixelFormatInvalid;
		if (format == 70 || format == 71) {
			fmt = MTLPixelFormatRGBA8Unorm;
		} else if (format == 80 || format == 81) {
			fmt = MTLPixelFormatBGRA8Unorm;
		} else if (format == 90 || format == 91 || format == 58 || format == 64) {
			fmt = MTLPixelFormatRGB10A2Unorm;
		} else if (format == 92) {
			fmt = MTLPixelFormatRG11B10Float;
		} else {
			fmt = MetalFormatFromVulkanFormat(format);
			if (fmt == MTLPixelFormatInvalid) {
				fmt = MTLPixelFormatRGBA8Unorm;
			}
		}

		const size_t bpp = BytesPerPixel(fmt) > 0 ? BytesPerPixel(fmt) : 4;
		const size_t min_bpr = (size_t)width * bpp;
		size_t bpr = stride_bytes;
		if (bpr < min_bpr) {
			if (bpr * bpp >= min_bpr) {
				bpr *= bpp;
			} else {
				bpr = min_bpr;
			}
		}

		const size_t total_size = bpr * height;

		// Wrap guest memory directly (zero copy UMA)
		MetalBuffer buf;
		if (!buf.WrapHostMemory(const_cast<void*>(guest_vaddr), total_size, "DisplayBuffer")) {
			return false;
		}

		PresentUniforms u {};
		u.width = width;
		u.height = height;
		u.pitch = static_cast<uint32_t>(bpr);
		u.format = static_cast<uint32_t>(fmt);
		static int s_env_tiled = -1;
		if (s_env_tiled == -1) {
			const char* env = std::getenv("KYTY_PRESENT_TILED");
			s_env_tiled = env ? std::atoi(env) : 1;
		}
		u.is_tiled = static_cast<uint32_t>(s_env_tiled);

		MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture     = drawable.texture;
		pass.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;

		id<MTLRenderCommandEncoder> enc = [cmd_buf renderCommandEncoderWithDescriptor:pass];
		if (enc) {
			[enc setRenderPipelineState:m_impl->blit_buf_pipeline];
			[enc setFragmentBuffer:buf.GetHandle() offset:buf.GetOffset() atIndex:0];
			[enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[enc endEncoding];
		}

		[cmd_buf presentDrawable:drawable];
		KytyBudgetedCommit(cmd_buf);
		return true;
	}
}

} // namespace Libs::Graphics::Metal
