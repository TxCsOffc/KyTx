// Regression for a depth-only fragment shader with stale guest color/blend registers.
#include "graphics/host_gpu/metal/metalPipeline.h"
#include "graphics/host_gpu/metal/metalContext.h"
#include "graphics/host_gpu/metal/metalDraw.h"
#include "graphics/host_gpu/metal/metalIndirect.h"
#include "common/logging/log.h"
#include <cstdio>
#include <cstring>
#include <cmath>

namespace Libs::Graphics::Metal {
MetalContext::MetalContext() { m_device = MTLCreateSystemDefaultDevice(); m_command_queue = [m_device newCommandQueue]; }
MetalContext::~MetalContext() = default;
MetalContext& MetalContext::Instance() { static MetalContext context; return context; }
id<MTLCommandBuffer> MetalContext::GetCurrentCommandBuffer() {
	if (!m_current_command_buffer) m_current_command_buffer = [m_command_queue commandBuffer];
	return m_current_command_buffer;
}
void MetalContext::SetActiveRenderEncoder(id<MTLRenderCommandEncoder> encoder) noexcept { m_active_render_encoder = encoder; }
void MetalContext::SetActiveComputeEncoder(id<MTLComputeCommandEncoder> encoder) noexcept { m_active_compute_encoder = encoder; }
void MetalContext::EndCurrentEncoder() {
	[m_active_render_encoder endEncoding]; m_active_render_encoder = nil;
	[m_active_compute_encoder endEncoding]; m_active_compute_encoder = nil;
}
void MetalContext::CommitCommandBuffer(bool wait) {
	EndCurrentEncoder();
	if (m_current_command_buffer) {
		[m_current_command_buffer commit];
		if (wait) [m_current_command_buffer waitUntilCompleted];
		m_current_command_buffer = nil;
	}
}
uint64_t MetalDrawSequence() { return 0; }
void CountDispatchStat(int) {}
}
namespace Log {
bool IsSilent() { return true; }
void Write(fmt::text_style, std::string_view text) { std::fprintf(stderr, "%.*s", int(text.size()), text.data()); }
}

static unsigned ComputeIndirect(id<MTLDevice> device) {
	using namespace Libs::Graphics::Metal;
	NSError* error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	    "kernel void produce(device uint* a [[buffer(0)]], constant uint* n [[buffer(1)]]) { a[4]=n[0]; a[5]=n[1]; a[6]=n[2]; }\n"
	    "kernel void consume(device atomic_uint* out [[buffer(0)]], constant uint& tag [[buffer(1)]], uint3 tid [[thread_position_in_threadgroup]]) {\n"
	    " atomic_fetch_add_explicit(out,1u,memory_order_relaxed);\n"
	    " atomic_fetch_add_explicit(out+1,tid.x,memory_order_relaxed);\n"
	    " atomic_fetch_add_explicit(out+2,tid.y,memory_order_relaxed);\n"
	    " atomic_fetch_add_explicit(out+3,tid.z,memory_order_relaxed);\n"
	    " atomic_fetch_add_explicit(out+4,tag,memory_order_relaxed); }\n" options:nil error:&error];
	if (!library) return 1;
	id<MTLComputePipelineState> producer = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"produce"] error:&error];
	id<MTLComputePipelineState> consumer = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"consume"] error:&error];
	if (!producer || !consumer) return 1;
	auto& draw = MetalDrawExecutor::Instance();
	auto& context = MetalContext::Instance();
	id<MTLBuffer> source = [device newBufferWithLength:64 options:MTLResourceStorageModePrivate];
	id<MTLBuffer> legacy = [device newBufferWithLength:32 options:MTLResourceStorageModeShared];
	std::memset(legacy.contents,0,legacy.length);
	const uint32_t one[] = {1,1,1};
	if (!draw.BeginComputePass()) return 1;
	draw.SetComputePipeline(producer); draw.SetComputeBuffer(source,0,0);
	draw.SetComputeBytes(one,sizeof(one),1); draw.Dispatch(1,1,1,1,1,1);
	draw.SetComputePipeline(consumer); draw.SetComputeBuffer(legacy,0,0);
	const uint32_t old_tag=7; draw.SetComputeBytes(&old_tag,sizeof(old_tag),1);
	// Reproduce the old renderer: BeginComputePass clears the pipeline, then
	// DispatchIndirect is called without preparing the selected shader/resources.
	if (!draw.BeginComputePass()) return 1;
	draw.DispatchIndirect(source,16,64,1,1);
	context.CommitCommandBuffer(true);
	if (*static_cast<const uint32_t*>(legacy.contents)!=0) return 1;
	std::puts("Legacy indirect renderer sequence: dispatch skipped after pipeline reset");
	struct Case { uint32_t raw[3]; MTLSize guest; bool threads; uint32_t cap; MTLSize host; uint32_t expected[3]; };
	const Case cases[] = {
	    {{3,2,1},{2,3,1},false,8,{2,3,1},{3,2,1}},
	    {{5,4,2},{2,3,2},true,8,{2,3,2},{3,2,1}},
	    {{2,0,1},{4,1,1},false,8,{4,1,1},{2,0,1}},
	    {{17,6,1},{1,1,1},false,4,{1,1,1},{4,4,1}},
	    {{UINT32_MAX,1,1},{8,1,1},true,4,{8,1,1},{4,1,1}},
	    {{64,1,1},{64,1,1},true,8,{32,1,1},{1,1,1}}, // paired wave64 host geometry
	    {{0,0,0},{1,1,1},false,8,{1,1,1},{0,0,0}},
	    // Arguments that are image pixels or stale memory (1.0f bits, 0xFFFFFFFF, 65535^3 after the per-axis clamp) are never a real grid: no groups.
	    {{0x3F800000u,0x3F800000u,0x3F800000u},{1,1,1},false,65535,{1,1,1},{0,0,0}},
	    {{UINT32_MAX,UINT32_MAX,UINT32_MAX},{1,1,1},false,65535,{1,1,1},{0,0,0}},
	    {{2048,1024,1},{1,1,1},false,65535,{1,1,1},{2048,1024,1}} // 2Mi groups is below the default total cap and runs
	};
	constexpr size_t count=sizeof(cases)/sizeof(cases[0]);
	id<MTLBuffer> output = [device newBufferWithLength:count*32 options:MTLResourceStorageModeShared];
	id<MTLBuffer> snapshots = [device newBufferWithLength:count*64 options:MTLResourceStorageModeShared];
	std::memset(output.contents,0,output.length); std::memset(snapshots.contents,0,snapshots.length);
	MetalComputeIndirect prepare;
	id<MTLCommandBuffer> command = context.GetCurrentCommandBuffer();
	if (prepare.Encode(command,source,source.length-8,{1,1,1},false,8) ||
	    prepare.Encode(command,source,1,{1,1,1},false,8) ||
	    prepare.Encode(command,source,16,{0,1,1},false,8)) return 1;
	for (size_t i=0;i<count;++i) {
		const auto& test=cases[i];
		if (!draw.BeginComputePass()) return 1;
		draw.SetComputePipeline(producer); draw.SetComputeBuffer(source,0,0);
		draw.SetComputeBytes(test.raw,sizeof(test.raw),1); draw.Dispatch(1,1,1,1,1,1);
		draw.EndComputePass();
		id<MTLBuffer> arguments = prepare.Encode(command,source,16,test.guest,test.threads,test.cap);
		if (!arguments) return 1;
		id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
		[blit copyFromBuffer:arguments sourceOffset:0 toBuffer:snapshots destinationOffset:i*64 size:32];
		[blit copyFromBuffer:source sourceOffset:16 toBuffer:snapshots destinationOffset:i*64+32 size:12];
		[blit endEncoding];
		if (!draw.BeginComputePass()) return 1;
		draw.SetComputePipeline(consumer); draw.SetComputeBuffer(output,i*32,0);
		const uint32_t tag=uint32_t(i+11); draw.SetComputeBytes(&tag,sizeof(tag),1);
		draw.DispatchIndirect(arguments,0,test.host.width,test.host.height,test.host.depth);
		draw.EndComputePass();
	}
	context.CommitCommandBuffer(true);
	if (command.status!=MTLCommandBufferStatusCompleted) return 1;
	unsigned failures=0;
	for (size_t i=0;i<count;++i) {
		const auto& t=cases[i];
		const auto* args=static_cast<const uint32_t*>(snapshots.contents)+i*16;
		const auto* out=static_cast<const uint32_t*>(output.contents)+i*8;
		const uint32_t threads=t.host.width*t.host.height*t.host.depth;
		const uint32_t groups=t.expected[0]*t.expected[1]*t.expected[2];
		const uint32_t expected[]={groups*threads,groups*threads*uint32_t(t.host.width-1)/2,
		    groups*threads*uint32_t(t.host.height-1)/2,groups*threads*uint32_t(t.host.depth-1)/2,
		    groups*threads*uint32_t(i+11)};
		const bool bad=std::memcmp(args,t.expected,12)!=0 || std::memcmp(args+4,t.raw,12)!=0 ||
		    std::memcmp(args+8,t.raw,12)!=0 || std::memcmp(out,expected,sizeof(expected))!=0;
		failures+=bad;
		std::printf("Compute indirect case %zu: groups=%u,%u,%u threads=%u, %s\n",i,args[0],args[1],args[2],out[0],bad?"FAIL":"pass");
	}
	// The total-groups guard zeroed exactly the two all-ones cases (7 and 8): the counter the watchdog thread polls must say so.
	const std::string guard_line=KytyPollIndirectGuard();
	const bool guard_ok=guard_line.find("2 indirect dispatches zeroed")!=std::string::npos && guard_line.find("raw=4294967295,4294967295,4294967295")!=std::string::npos;
	std::printf("Indirect guard counter: %s (%s)\n",guard_ok?"pass":"FAIL",guard_line.c_str());
	failures+=!guard_ok;
	std::printf("Compute indirect GPU producer/offset/clamp/thread dimensions/zero/reuse: %zu cases, %u failures\n",count,failures);
	return failures;
}

static unsigned MultipleTargets(id<MTLDevice> device, Libs::Graphics::Metal::MetalPipelineManager& manager,
                                id<MTLFunction> vertex, id<MTLFunction> fragment) {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::Metal;
	unsigned failures = 0;
	unsigned legacy_mismatches = 0;
	for (unsigned mode = 0; mode < 4; ++mode) {
		HW::Context context;
		for (uint32_t slot = 0; slot < 8; ++slot) context.SetColorBase(slot, {0x1000u * (slot + 1u)});
		// The third case has holes, partial component masks and an independently blended target5.
		const uint32_t masks = mode == 3 ? 0xf00u : (mode == 2 ? 0x00f0020fu : 0x00ffffffu);
		if (mode == 3) context.SetColorBase(0, {0});
		context.SetRenderTargetMask(masks);
		context.SetShaderMask(0x00ffffffu);
		if (mode == 2) {
			HW::BlendControl blend;
			blend.enable = true;
			blend.color_srcblend = uint8_t(Prospero::BlendFactor::kSrcAlpha);
			blend.color_destblend = uint8_t(Prospero::BlendFactor::kOneMinusSrcAlpha);
			context.SetBlendControl(5, blend);
		}
		MetalRenderPipelineKey key {};
		key.vs_hash = 10; key.fs_hash = 20;
		key.primitive_type = uint32_t(Prospero::PrimitiveType::kTriList);
		MetalRenderPassDesc pass {};
		const uint32_t slots = NativeColorTargetMask(context);
		if (slots != (mode == 3 ? 4u : (mode == 2 ? 0x25u : 0x3fu))) ++failures;
		for (uint32_t slot = 0; slot < 8; ++slot) {
			if ((slots & (1u << slot)) == 0) continue;
			ConfigureNativeColorTarget(key, context, slot, MTLPixelFormatRGBA8Unorm);
			auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
			descriptor.storageMode = MTLStorageModeShared;
			descriptor.usage = MTLTextureUsageRenderTarget;
			auto& target = pass.color_attachments[slot];
			target.texture = [device newTextureWithDescriptor:descriptor];
			uint8_t initial[8 * 8 * 4]; std::memset(initial, 64, sizeof(initial));
			[target.texture replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:initial bytesPerRow:32];
			target.load_action = MTLLoadActionClear; target.store_action = MTLStoreActionStore;
			target.clear_color[0] = target.clear_color[1] = target.clear_color[2] = target.clear_color[3] = 0.25f;
			pass.color_attachment_count = slot + 1u;
		}
		// Reproduce the previous renderer: only RT0 was bound, discarding five exports.
		if (mode == 0) {
			key.color_target_count = 1;
			pass.color_attachment_count = 1;
		}
		MaskUnwrittenColorAttachments(key, 0x3fu);
		auto pipeline = manager.GetOrCreateRenderPipeline(key, vertex, fragment);
		if (!pipeline) return failures + 1;
		auto& draw = MetalDrawExecutor::Instance();
		if (!draw.BeginRenderPass(pass)) return failures + 1;
		draw.SetRenderPipeline(pipeline);
		draw.SetViewport(0,0,8,8,0,1); draw.SetScissor(0,0,8,8);
		draw.Draw(Prospero::PrimitiveType::kTriList, 3, 1, 0, 0);
		draw.EndRenderPass();
		auto command = MetalContext::Instance().GetCurrentCommandBuffer();
		MetalContext::Instance().CommitCommandBuffer(true);
		if (command.status != MTLCommandBufferStatusCompleted) return failures + 1;
		for (uint32_t slot = 0; slot < 6; ++slot) {
			if ((slots & (1u << slot)) == 0) continue;
			uint8_t rgba[8 * 8 * 4] {};
			[pass.color_attachments[slot].texture getBytes:rgba bytesPerRow:32 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
			const uint8_t* pixel = rgba + (4 * 8 + 4) * 4;
			const unsigned nibble = (masks >> (slot * 4u)) & 0xfu;
			const float values[] = {float(slot + 1) / 8.f, 0.5f, 0.75f, 0.5f};
			for (unsigned c = 0; c < 4; ++c) {
				float expected = (nibble & (1u << c)) ? values[c] : 0.25f;
				if (mode == 2 && slot == 5 && c < 3) expected = values[c] * 0.5f + 0.25f * 0.5f;
				if (std::abs(int(pixel[c]) - int(std::lround(expected * 255.f))) > 1) {
					if (mode == 0) ++legacy_mismatches; else ++failures;
				}
			}
		}
	}
	if (legacy_mismatches == 0) ++failures;
	std::printf("Legacy MRT: %u incorrect pixel components with only RT0 attached\n", legacy_mismatches);
	std::printf("Native MRT: 6 attachments, sparse slots0/2/5 and slot2 without RT0, %u failures\n", failures);
	return failures;
}

// The guest stencil state must reach Metal: a NEVER+REPLACE "mask" draw writes the stencil plane and must not touch colour
// (it is what the UI uses to clip), and an EQUAL draw then colours only inside the mask. Without a stencil attachment both
// draws would paint the whole target.
static unsigned StencilMasking(id<MTLDevice> device, Libs::Graphics::Metal::MetalPipelineManager& manager,
                               id<MTLFunction> vertex, id<MTLLibrary> library) {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::Metal;
	unsigned failures = 0;
	MetalRenderPipelineKey key {};
	key.vs_hash = 100; key.fs_hash = 200;
	key.primitive_type = uint32_t(Prospero::PrimitiveType::kTriList);
	key.color_target_count = 1;
	key.color_targets[0].pixel_format = MTLPixelFormatRGBA8Unorm;
	key.depth_format = MTLPixelFormatDepth32Float;
	key.stencil_format = MTLPixelFormatStencil8;
	auto mask_key = key;
	auto fill_key = key; fill_key.fs_hash = 201;
	id<MTLRenderPipelineState> mask_pso = manager.GetOrCreateRenderPipeline(mask_key, vertex, [library newFunctionWithName:@"redc"]);
	id<MTLRenderPipelineState> fill_pso = manager.GetOrCreateRenderPipeline(fill_key, vertex, [library newFunctionWithName:@"greenc"]);
	if (!mask_pso || !fill_pso) { std::fprintf(stderr, "stencil pipelines failed\n"); return 1; }

	HW::DepthControl dc_mask; dc_mask.stencil_enable = true; dc_mask.stencilfunc = 0; // NEVER
	HW::StencilControl sc_mask; sc_mask.stencil_fail = uint8_t(Prospero::StencilOp::kReplaceTest);
	HW::StencilMask sm_mask; sm_mask.stencil_testval = 1; sm_mask.stencil_mask = 0xff; sm_mask.stencil_writemask = 0x01;
	HW::DepthControl dc_fill; dc_fill.stencil_enable = true; dc_fill.stencilfunc = 2; // EQUAL
	HW::StencilControl sc_fill;
	HW::StencilMask sm_fill; sm_fill.stencil_testval = 1; sm_fill.stencil_mask = 0x01; sm_fill.stencil_writemask = 0;
	MetalStencilRefs mask_refs {}, fill_refs {};
	id<MTLDepthStencilState> mask_ds = manager.GetOrCreateDepthStencilState(dc_mask, sc_mask, sm_mask, true, false, &mask_refs);
	id<MTLDepthStencilState> fill_ds = manager.GetOrCreateDepthStencilState(dc_fill, sc_fill, sm_fill, true, false, &fill_refs);
	if (!mask_ds || !fill_ds || mask_refs.front != 1 || fill_refs.front != 1) { std::fprintf(stderr, "stencil state/ref wrong\n"); return 1; }

	MTLTextureDescriptor* color_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
	color_desc.storageMode = MTLStorageModeShared; color_desc.usage = MTLTextureUsageRenderTarget;
	id<MTLTexture> color = [device newTextureWithDescriptor:color_desc];
	MTLTextureDescriptor* depth_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:8 height:8 mipmapped:NO];
	depth_desc.storageMode = MTLStorageModePrivate; depth_desc.usage = MTLTextureUsageRenderTarget;
	MTLTextureDescriptor* stencil_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatStencil8 width:8 height:8 mipmapped:NO];
	stencil_desc.storageMode = MTLStorageModePrivate; stencil_desc.usage = MTLTextureUsageRenderTarget;
	MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = color;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.depthAttachment.texture = [device newTextureWithDescriptor:depth_desc];
	pass.depthAttachment.loadAction = MTLLoadActionClear; pass.depthAttachment.clearDepth = 1;
	pass.depthAttachment.storeAction = MTLStoreActionStore;
	pass.stencilAttachment.texture = [device newTextureWithDescriptor:stencil_desc];
	pass.stencilAttachment.loadAction = MTLLoadActionClear; pass.stencilAttachment.clearStencil = 0;
	pass.stencilAttachment.storeAction = MTLStoreActionStore;
	id<MTLCommandBuffer> command = [[device newCommandQueue] commandBuffer];
	id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
	// Mask draw: left half only (scissor), colour must stay untouched.
	[encoder setRenderPipelineState:mask_pso];
	[encoder setDepthStencilState:mask_ds];
	[encoder setStencilFrontReferenceValue:mask_refs.front backReferenceValue:mask_refs.back];
	[encoder setScissorRect:MTLScissorRect {0, 0, 4, 8}];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	// Masked fill: whole target, but only the stencil==1 half may receive colour.
	[encoder setRenderPipelineState:fill_pso];
	[encoder setDepthStencilState:fill_ds];
	[encoder setStencilFrontReferenceValue:fill_refs.front backReferenceValue:fill_refs.back];
	[encoder setScissorRect:MTLScissorRect {0, 0, 8, 8}];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[encoder endEncoding];
	[command commit]; [command waitUntilCompleted];
	if (command.status != MTLCommandBufferStatusCompleted) { std::fprintf(stderr, "%s\n", command.error.localizedDescription.UTF8String); return 1; }
	uint8_t rgba[8 * 8 * 4];
	[color getBytes:rgba bytesPerRow:32 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
	for (unsigned y = 0; y < 8; ++y) {
		for (unsigned x = 0; x < 8; ++x) {
			const uint8_t* px = rgba + (y * 8 + x) * 4;
			const bool inside = x < 4;
			const uint8_t expected[4] = {0, uint8_t(inside ? 255 : 0), 0, 255}; // never red; green only inside the mask
			if (std::memcmp(px, expected, 4) != 0) ++failures;
		}
	}
	std::printf("Stencil: NEVER+REPLACE mask draw leaves colour untouched, EQUAL fill is clipped to the mask: 64 pixels, %u failures\n", failures);
	return failures;
}

static unsigned MultiDrawIndirect(id<MTLDevice> device) {
	using namespace Libs::Graphics::Metal;
	NSError* error = nil;
	NSString* code = @"#include <metal_stdlib>\nusing namespace metal;\n"
	    "kernel void produce(device uint* args [[buffer(0)]], device uint* count [[buffer(1)]],\n"
	    "                    constant uint* data [[buffer(2)]], uint i [[thread_position_in_grid]]) {\n"
	    " if (i < 16u) args[i+4u] = data[i]; if (i == 0u) count[2] = data[16]; }\n"
	    "struct V { float4 pos [[position]]; float4 color; };\n"
	    "vertex V vi(uint id [[vertex_id]], uint inst [[instance_id]]) {\n"
	    " float2 p[4] = {float2(-1,-1),float2(0,-1),float2(-1,1),float2(0,1)};\n"
	    " uint side = id/4u; V v; v.pos=float4(p[id%4u]+float2(side,0),0,1);\n"
	    " v.color = (inst == (side ? 9u : 7u)) ? float4(0,1,0,1) : float4(1,0,0,1); return v; }\n"
	    "vertex V vn(uint id [[vertex_id]], uint inst [[instance_id]]) {\n"
	    " uint ids[6]={0,1,2,2,1,3}; float2 p[4]={float2(-1,-1),float2(0,-1),float2(-1,1),float2(0,1)};\n"
	    " uint side=id/6u; V v; v.pos=float4(p[ids[id%6u]]+float2(side,0),0,1);\n"
	    " v.color=(inst == (side ? 9u : 7u)) ? float4(0,1,0,1) : float4(1,0,0,1); return v; }\n"
	    "fragment float4 ps(V v [[stage_in]]) { return v.color; }\n";
	id<MTLLibrary> library = [device newLibraryWithSource:code options:nil error:&error];
	id<MTLComputePipelineState> producer = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"produce"] error:&error];
	if (!producer) { std::fprintf(stderr, "MDI producer: %s\n", error.localizedDescription.UTF8String); return 1; }
	id<MTLRenderPipelineState> pipelines[2];
	for (unsigned i = 0; i < 2; ++i) {
		MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
		desc.vertexFunction = [library newFunctionWithName:i ? @"vi" : @"vn"];
		desc.fragmentFunction = [library newFunctionWithName:@"ps"];
		desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
		pipelines[i] = [device newRenderPipelineStateWithDescriptor:desc error:&error];
		if (!pipelines[i]) { std::fprintf(stderr, "MDI render pipeline: %s\n", error.localizedDescription.UTF8String); return 1; }
	}
	// The left indexed draw uses a negative base vertex; both use a nonzero
	// base instance. Input/count offsets and stride intentionally differ from output.
	const uint16_t indices[] = {0,0,0,0,0,0,0,0, 4,5,6,6,5,7, 4,5,6,6,5,7};
	id<MTLBuffer> index_buffer = [device newBufferWithBytes:indices length:sizeof(indices) options:MTLResourceStorageModeShared];
	id<MTLBuffer> source = [device newBufferWithLength:80 options:MTLResourceStorageModePrivate];
	id<MTLBuffer> count = [device newBufferWithLength:16 options:MTLResourceStorageModePrivate];
	id<MTLBuffer> unchanged = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
	MTLTextureDescriptor* texture_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:8 mipmapped:NO];
	texture_desc.storageMode = MTLStorageModeShared;
	texture_desc.usage = MTLTextureUsageRenderTarget;
	id<MTLTexture> target = [device newTextureWithDescriptor:texture_desc];
	MetalMultiDrawIndirect prepare;
	MetalRectListIndirect rect_prepare;
	MetalDrawExecutor draw;
	auto& context = MetalContext::Instance();
	unsigned failures = 0;
	for (unsigned test = 0; test < 18; ++test) {
		const bool legacy_split = test == 0;
		const bool rect = test == 13 || test == 14;
		const bool indexed = test != 10 && test != 11 && !rect && test != 16 && test != 17;
		const bool has_count = test != 9 && test != 11 && test < 15;
		const unsigned max_draws = test >= 15 ? 1 : 2;
		uint32_t input[17] = {6,1,0,uint32_t(-4),7,0xfeedabcd,0xfeedabcd,0xfeedabcd,
		                      6,1,6,0,9,0xfeedabcd,0xfeedabcd,0xfeedabcd,2};
		unsigned expected_sides = 3;
		uint32_t index_limit = 12;
		if (test == 2 || test == 10) { input[16] = 1; expected_sides = 1; }
		if (test == 3) { input[16] = 0; expected_sides = 0; }
		if (test == 4) input[16] = UINT32_MAX;
		if (test == 5) input[8] = UINT32_MAX;
		if (test == 6) { input[2] = UINT32_MAX; expected_sides = 2; }
		if (test == 7) { input[10] = 12; expected_sides = 1; }
		if (test == 8) { index_limit = 0; expected_sides = 0; }
		if (test == 12) { input[1] = 0; expected_sides = 2; }
		if (test >= 15) expected_sides = 1;
		if (test == 17) { input[0] = 0; expected_sides = 0; }
		if (!has_count) input[16] = 0; // An unused zero counter must not suppress the immediate draw count.
		if (!indexed) {
			input[3] = 7; input[11] = 9;
			input[4] = input[12] = 0xfeedabcd; // padding must not enter a nonindexed argument
		}
		if (rect) {
			input[0] = input[8] = 3; input[10] = 4;
			if (test == 14) { input[16] = 1; expected_sides = 1; }
		}
		id<MTLCommandBuffer> command = context.GetCurrentCommandBuffer();
		id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
		[encoder setComputePipelineState:producer];
		[encoder setBuffer:source offset:0 atIndex:0];
		[encoder setBuffer:count offset:0 atIndex:1];
		[encoder setBytes:input length:sizeof(input) atIndex:2];
		[encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(16,1,1)];
		[encoder endEncoding];
		id<MTLBuffer> arguments = prepare.Encode(command, source, 16, has_count ? count : nil, 8, max_draws, 32, indexed, index_limit);
		if (!arguments) return failures + 1;
		id<MTLBuffer> draw_arguments = rect ? rect_prepare.Encode(command, arguments, 0, 2, MetalMultiDrawIndirect::RecordBytes) : arguments;
		if (!draw_arguments) return failures + 1;
		id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
		[blit copyFromBuffer:source sourceOffset:16 toBuffer:unchanged destinationOffset:0 size:64];
		[blit endEncoding];
		MetalRenderPassDesc pass {};
		pass.color_attachment_count = 1;
		pass.color_attachments[0].texture = target;
		pass.color_attachments[0].load_action = MTLLoadActionClear;
		if (!draw.BeginRenderPass(pass)) return failures + 1;
		draw.SetRenderPipeline(pipelines[indexed || rect]);
		draw.SetViewport(0,0,16,8,0,1);
		draw.SetScissor(0,0,16,8);
		for (unsigned k = 0; k < max_draws; ++k) {
			const bool last = legacy_split || k + 1 == max_draws;
			if (indexed) draw.DrawIndexedIndirect(Libs::Graphics::Prospero::PrimitiveType::kTriList, MTLIndexTypeUInt16,
			                                      index_buffer, 16, arguments, k * MetalMultiDrawIndirect::RecordBytes, last);
			else draw.DrawIndirect(rect ? Libs::Graphics::Prospero::PrimitiveType::kRectList : Libs::Graphics::Prospero::PrimitiveType::kTriList,
			                       draw_arguments, k * (rect ? MetalRectListIndirect::RecordBytes : MetalMultiDrawIndirect::RecordBytes), last);
		}
		context.CommitCommandBuffer(false);
		[command waitUntilCompleted];
		if (command.status == MTLCommandBufferStatusError) {
			std::fprintf(stderr, "MDI GPU error: %s\n", command.error.localizedDescription.UTF8String); return failures + 1;
		}
		unsigned bad = 0;
		if (std::memcmp(unchanged.contents, input, 64) != 0) ++bad;
		const auto* data = static_cast<const uint32_t*>(arguments.contents);
		for (unsigned k = 0; k < max_draws; ++k) {
			const uint32_t* raw = data + k*16 + 8;
			if (std::memcmp(raw,input+k*8,indexed ? 20 : 16) != 0) ++bad;
			if (!indexed && raw[4] != 0) ++bad;
			if (raw[5] != (has_count ? input[16] : max_draws) || raw[6] != index_limit || raw[7] != indexed) ++bad;
			const uint32_t* cooked = data + k*16;
			if (indexed && (cooked[2] > index_limit || cooked[0] > index_limit - cooked[2])) ++bad;
		}
		uint8_t pixels[16*8*4];
		[target getBytes:pixels bytesPerRow:16*4 fromRegion:MTLRegionMake2D(0,0,16,8) mipmapLevel:0];
		unsigned pixel_bad = 0;
		for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 16; ++x) {
			const uint8_t expected[] = {0, uint8_t((expected_sides & (x < 8 ? 1 : 2)) ? 255 : 0), 0, 255};
			pixel_bad += std::memcmp(pixels+(y*16+x)*4,expected,4) != 0;
		}
		if (legacy_split) { if (bad || pixel_bad != 64) ++failures; }
		else failures += bad + pixel_bad;
		std::printf("MDI case %u%s: indexed=%d GPU count=%u sides=%u metadata_bad=%u pixels_bad=%u\n",
		            test, legacy_split ? " legacy split control" : "", indexed, input[16], expected_sides, bad, pixel_bad);
	}
	// Never submit pathological instance counts to the rasterizer, even in the
	// old-control build. Inspect the bounded GPU preparation result first.
	unsigned instance_failures = 0;
	for (uint32_t instances: {0u,65536u,65537u,3562971914u,UINT32_MAX}) {
		const uint32_t input[] = {6,instances,0,uint32_t(-4),7,0,0,0, 6,instances,6,0,9,0,0,0,2};
		id<MTLCommandBuffer> command = context.GetCurrentCommandBuffer();
		id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
		[encoder setComputePipelineState:producer];
		[encoder setBuffer:source offset:0 atIndex:0];
		[encoder setBuffer:count offset:0 atIndex:1];
		[encoder setBytes:input length:sizeof(input) atIndex:2];
		[encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(16,1,1)];
		[encoder endEncoding];
		id<MTLBuffer> args = prepare.Encode(command,source,16,nil,0,2,32,true,12);
		context.CommitCommandBuffer(true);
		if (!args || command.status == MTLCommandBufferStatusError) return failures + 1;
		const auto* data = static_cast<const uint32_t*>(args.contents);
		const uint32_t expected = instances > 65536u ? 1u : instances;
		bool bad = false;
		for (unsigned k = 0; k < 2; ++k) bad |= data[k*16+1] != expected || data[k*16+9] != instances;
		instance_failures += bad;
		std::printf("MDI instance bound: raw=%u cooked=%u expected=%u original=%u %s\n",
		            instances,data[1],expected,data[9],bad ? "FAIL" : "pass");
	}
	failures += instance_failures;
	// Invalid extent/alignment cannot encode a producer that reads beyond inputs.
	id<MTLCommandBuffer> empty = context.GetCurrentCommandBuffer();
	failures += prepare.Encode(empty,source,16,count,8,3,32,true,12) != nil;
	failures += prepare.Encode(empty,source,16,count,8,2,18,true,12) != nil;
	failures += prepare.Encode(empty,source,16,count,14,2,32,true,12) != nil;
	context.CommitCommandBuffer(true);
	std::printf("Multi-draw GPU producer/count/bounds/batch/RECTLIST/single: 17 pixel cases, 5 instance bounds, legacy split and 3 invalid controls, %u failures\n", failures);
	return failures;
}

int main(int argc, char** argv) {
	@autoreleasepool {
		using namespace Libs::Graphics::Metal;
		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		if (!device) { std::fprintf(stderr, "Metal GPU unavailable\n"); return 1; }
		if (argc > 1 && std::strcmp(argv[1], "--multi-draw") == 0) return MultiDrawIndirect(device) ? 1 : 0;
		NSError* error = nil;
		NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
		    "vertex float4 vs(uint id [[vertex_id]]) { const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[id],0,1); }\n"
		    "struct D { float depth [[depth(any)]]; }; fragment D depth_only() { D d; d.depth=0.25f; return d; }\n"
		    "fragment float4 color() { return float4(0.5f,0,0,1); }\n"
		    "fragment float4 redc() { return float4(1,0,0,1); }\n"
		    "fragment float4 greenc() { return float4(0,1,0,1); }\n"
		    "struct MRT { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; float4 c4 [[color(4)]]; float4 c5 [[color(5)]]; };\n"
		    "fragment MRT mrt() { MRT m; m.c0=float4(.125,.5,.75,.5); m.c1=float4(.25,.5,.75,.5); m.c2=float4(.375,.5,.75,.5); m.c3=float4(.5,.5,.75,.5); m.c4=float4(.625,.5,.75,.5); m.c5=float4(.75,.5,.75,.5); return m; }";
		MTLCompileOptions* options = [MTLCompileOptions new];
		options.languageVersion = MTLLanguageVersion3_0; // Match the native shader manager.
		id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
		if (!library) { std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
		MetalPipelineManager manager;
		MetalRenderPipelineKey base {};
		base.vs_hash = 1; base.fs_hash = 2;
		base.primitive_type = uint32_t(Libs::Graphics::Prospero::PrimitiveType::kTriList);
		base.color_target_count = 1;
		base.color_targets[0].pixel_format = MTLPixelFormatRGBA16Float;
		base.color_targets[0].blending_enabled = true;
		base.color_targets[0].src_rgb = MTLBlendFactorSource1Alpha;
		base.color_targets[0].src_alpha = MTLBlendFactorSource1Alpha;
		base.color_targets[0].dst_rgb = MTLBlendFactorOneMinusSourceAlpha;
		base.color_targets[0].dst_alpha = MTLBlendFactorOneMinusSourceAlpha;
		base.depth_format = MTLPixelFormatDepth32Float;
		id<MTLFunction> vertex = [library newFunctionWithName:@"vs"];
		id<MTLFunction> depth_only = [library newFunctionWithName:@"depth_only"];
		if (manager.GetOrCreateRenderPipeline(base, vertex, depth_only) != nil) {
			std::fprintf(stderr, "Legacy depth-only blend state unexpectedly accepted\n"); return 1;
		}
		id<MTLCommandQueue> queue = [device newCommandQueue];
		unsigned failures = 0;
		for (unsigned mode = 0; mode < 2; ++mode) {
			auto key = base;
			key.fs_hash += mode;
			if (mode == 1) {
				key.color_targets[0].src_rgb = MTLBlendFactorOne;
				key.color_targets[0].src_alpha = MTLBlendFactorOne;
			}
			MaskUnwrittenColorAttachments(key, mode == 0 ? 0u : 1u);
			id<MTLFunction> fragment = mode == 0 ? depth_only : [library newFunctionWithName:@"color"];
			id<MTLRenderPipelineState> pipeline = manager.GetOrCreateRenderPipeline(key, vertex, fragment);
			if (!pipeline) return 1;
			MTLTextureDescriptor* color_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:8 height:8 mipmapped:NO];
			color_desc.storageMode = MTLStorageModeShared;
			color_desc.usage = MTLTextureUsageRenderTarget;
			id<MTLTexture> color = [device newTextureWithDescriptor:color_desc];
			uint16_t initial[8 * 8 * 4];
			const uint16_t rgba[] = {0x3400,0x3800,0x3a00,0x3c00}; // .25, .5, .75, 1
			for (unsigned i = 0; i < 8 * 8; ++i) std::memcpy(initial + i * 4, rgba, sizeof(rgba));
			[color replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:initial bytesPerRow:8 * 8];
			MTLTextureDescriptor* depth_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:8 height:8 mipmapped:NO];
			depth_desc.storageMode = MTLStorageModePrivate;
			depth_desc.usage = MTLTextureUsageRenderTarget;
			id<MTLTexture> depth = [device newTextureWithDescriptor:depth_desc];
			MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture = color;
			pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			pass.depthAttachment.texture = depth;
			pass.depthAttachment.clearDepth = 1;
			pass.depthAttachment.loadAction = MTLLoadActionClear;
			pass.depthAttachment.storeAction = MTLStoreActionStore;
			MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
			ds.depthCompareFunction = MTLCompareFunctionAlways; ds.depthWriteEnabled = YES;
			id<MTLBuffer> readback = [device newBufferWithLength:8 * 256 options:MTLResourceStorageModeShared];
			id<MTLCommandBuffer> command = [queue commandBuffer];
			id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
			[encoder setRenderPipelineState:pipeline];
			[encoder setDepthStencilState:[device newDepthStencilStateWithDescriptor:ds]];
			[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[encoder endEncoding];
			id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
			[blit copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1)
			            toBuffer:readback destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:8 * 256];
			[blit endEncoding]; [command commit]; [command waitUntilCompleted];
			if (command.status != MTLCommandBufferStatusCompleted) { std::fprintf(stderr, "%s\n", command.error.localizedDescription.UTF8String); return 1; }
			uint16_t output[8 * 8 * 4];
			[color getBytes:output bytesPerRow:8 * 8 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
			if (mode == 0) {
				failures += std::memcmp(initial, output, sizeof(initial)) != 0;
				failures += *(reinterpret_cast<const float*>(static_cast<const uint8_t*>(readback.contents) + 4 * 256) + 4) != 0.25f;
			} else {
				const uint16_t expected[] = {0x3800,0,0,0x3c00};
				failures += std::memcmp(output + (4 * 8 + 4) * 4, expected, sizeof(expected)) != 0;
			}
		}
		std::printf("Fragment color exports: legacy depth-only PSO rejected; 2 GPU cases, %u failures (depth written/color preserved, color blend retained)\n", failures);
		failures += MultipleTargets(device, manager, vertex, [library newFunctionWithName:@"mrt"]);
		failures += StencilMasking(device, manager, vertex, library);
		failures += ComputeIndirect(device);
		return failures ? 1 : 0;
	}
}
