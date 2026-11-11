#include "graphics/host_gpu/metal/metalDepthBounds.h"
static unsigned NativeDepthBoundsTests(id<MTLDevice>device) {
	using namespace Libs::Graphics;using namespace Libs::Graphics::Metal;
	if(!SupportsNativeDepthBounds(device)){std::fprintf(stderr,"Depth bounds GPU test needs Apple10/macOS26\n");return 1;}
	auto&ctx=MetalContext::Instance();auto&draw=MetalDrawExecutor::Instance();NSError*error=nil;
	id<MTLLibrary>lib=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;vertex float4 v(uint i[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],.9,1);}fragment float4 f(){return float4(1,0,0,1);}" options:nil error:&error];
	if(!lib)return 1;
	unsigned failures=0,cases=0,negatives=0;uint64_t pixels=0;
	constexpr uint32_t w=32,h=8;const NSUInteger row=w*4;
	for(auto fmt:{MTLPixelFormatDepth16Unorm,MTLPixelFormatDepth32Float}) {
		const uint32_t eb=fmt==MTLPixelFormatDepth16Unorm?2:4;
		MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget;
		id<MTLTexture>depth=[device newTextureWithDescriptor:d];d.pixelFormat=MTLPixelFormatRGBA8Unorm;d.storageMode=MTLStorageModeShared;id<MTLTexture>colour=[device newTextureWithDescriptor:d];
		d.pixelFormat=MTLPixelFormatStencil8;d.storageMode=MTLStorageModePrivate;id<MTLTexture>stencil=[device newTextureWithDescriptor:d];if(!depth||!colour||!stencil)return 1;
		std::vector<float>values(w*h);std::vector<uint8_t>raw(row*h);
		const float pattern[]={0.f,.125f,.25f,.375f,.5f,.625f,.75f,1.f};
		for(uint32_t i=0;i<w*h;++i) {
			if(eb==4){values[i]=pattern[i%8];std::memcpy(raw.data()+(i/w)*row+(i%w)*eb,&values[i],eb);}
			else {const uint16_t bits=uint16_t(std::lround(pattern[i%8]*65535.f));values[i]=float(bits)/65535.f;std::memcpy(raw.data()+(i/w)*row+(i%w)*eb,&bits,eb);}
		}
		id<MTLBuffer>source=[device newBufferWithBytes:raw.data() length:raw.size() options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer>command=ctx.GetCurrentCommandBuffer();id<MTLBlitCommandEncoder>b=[command blitCommandEncoder];OrderBegin(b);[b copyFromBuffer:source sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row*h sourceSize:MTLSizeMake(w,h,1) toTexture:depth destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];OrderEnd(b);[b endEncoding];
		MTLRenderPipelineDescriptor*pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[lib newFunctionWithName:@"v"];pd.fragmentFunction=[lib newFunctionWithName:@"f"];pd.depthAttachmentPixelFormat=fmt;pd.stencilAttachmentPixelFormat=MTLPixelFormatStencil8;pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
		id<MTLRenderPipelineState>pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];if(!pipeline)return 1;
		const float lo=values[2],hi=values[6];
		struct Bounds{bool enabled;float min,max;};
		const std::vector<Bounds>bounds{{true,0,1},{true,lo,hi},{true,lo,lo},{true,hi,hi},{true,0,0},{true,1,1},
		    {true,std::nextafter(lo,1.f),hi},{true,lo,std::nextafter(hi,0.f)},{true,.3f,.31f},{true,.25f,.75f},
		    {false,.5f,.5f},{false,NAN,NAN}};
		for(bool z_enabled:{false,true})for(const auto&test:bounds) {
			MetalRenderPassDesc pass{};pass.has_depth=pass.has_stencil=true;pass.depth_attachment.texture=depth;pass.depth_attachment.load_action=MTLLoadActionLoad;pass.stencil_attachment.texture=stencil;pass.stencil_attachment.load_action=MTLLoadActionClear;pass.stencil_attachment.clear_stencil=0x13;pass.color_attachment_count=1;pass.color_attachments[0].texture=colour;pass.color_attachments[0].load_action=MTLLoadActionClear;pass.color_attachments[0].clear_color[3]=0;
			if(!draw.BeginRenderPass(pass))return 1;
			HW::DepthControl dc{};dc.z_enable=z_enabled;dc.z_write_enable=false;dc.zfunc=7;dc.depth_bounds_enable=test.enabled;dc.stencil_enable=true;dc.stencilfunc=7;
			HW::StencilControl sc{};sc.stencil_zpass=uint8_t(Prospero::StencilOp::kReplaceTest);HW::StencilMask sm{};sm.stencil_mask=sm.stencil_writemask=0xff;sm.stencil_testval=0x42;
			MetalStencilRefs refs{};auto ds=MetalPipelineManager::Instance().GetOrCreateDepthStencilState(dc,sc,sm,true,false,&refs);
			draw.SetRenderPipeline(pipeline);draw.SetDepthStencil(ds);draw.SetStencilReferences(refs.front,refs.back);draw.SetViewport(0,0,w,h,0,1);draw.SetScissor(0,0,w,h);
			// Exercise disabling on an encoder that already has restrictive bounds.
			if(!test.enabled)draw.SetDepthBounds(true,lo,lo);
			draw.SetDepthBounds(test.enabled,test.min,test.max);draw.Draw(Prospero::PrimitiveType::kTriList,3,1,0,0);draw.EndRenderPass();
			command=ctx.GetCurrentCommandBuffer();id<MTLBuffer>out=[device newBufferWithLength:row*h options:MTLResourceStorageModeShared];
			b=[command blitCommandEncoder];OrderBegin(b);[b copyFromTexture:stencil sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1) toBuffer:out destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row*h];OrderEnd(b);[b endEncoding];
			ctx.CommitCommandBuffer(true);if(command.status!=MTLCommandBufferStatusCompleted)return 1;
			std::vector<uint8_t>actual(w*h*4);[colour getBytes:actual.data() bytesPerRow:w*4 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
			unsigned wrong=0;for(uint32_t i=0;i<w*h;++i){const bool pass=!test.enabled||(values[i]>=test.min&&values[i]<=test.max);const auto s=static_cast<const uint8_t*>(out.contents)[(i/w)*row+i%w];if(actual[i*4]!=(pass?255:0)||s!=(pass?0x42:0x13))++wrong;}
			if(wrong){std::fprintf(stderr,"DepthBounds fmt=%lu z=%d enabled=%d bounds=%.9g..%.9g wrong=%u\n",(unsigned long)fmt,z_enabled,test.enabled,test.min,test.max,wrong);++failures;}++cases;pixels+=w*h;
		}
		MetalRenderPassDesc pass{};pass.has_depth=true;pass.depth_attachment.texture=depth;pass.depth_attachment.load_action=MTLLoadActionLoad;if(!draw.BeginRenderPass(pass))return 1;
		for(auto bad:std::vector<std::pair<float,float>>{{NAN,1},{0,NAN},{-.1f,1},{0,1.1f},{.8f,.2f}}) {
			bool rejected=false;try{draw.SetDepthBounds(true,bad.first,bad.second);}catch(const std::runtime_error&){rejected=true;}
			if(!rejected)++failures;else ++negatives;
		}
		if(eb==2) {bool rejected=false;try{draw.SetDepthBounds(true,.3f,.3f);}catch(const std::runtime_error&){rejected=true;}if(!rejected)++failures;else ++negatives;}
		draw.EndRenderPass();ctx.CommitCommandBuffer(true);
	}
	std::printf("Native depth bounds: %u GPU cases, %llu colour+stencil pixels, %u invalid/unsupported intervals rejected, %u failures\n",cases,(unsigned long long)pixels,negatives,failures);return failures;
}
