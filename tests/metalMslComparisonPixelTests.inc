// Exercise implicit derivatives in a real fragment shader, independently of
// explicit-LOD compute tests. UV varies across an 8x8 render target.
static void ComparisonPixelSampling() {
	using namespace ShaderRecompiler;
	ComparisonHarness source(Decoder::ImageDimension::Dim2D,false,false);
	Fixture f(0); f.program.stage=ShaderType::Pixel;
	f.program.info.inputs.push_back({.kind=IR::StageInputKind::Parameter,.location=0,.component_count=4});
	f.program.info.outputs.push_back({.kind=IR::StageOutputKind::Mrt,.index=0});
	f.program.export_info.push_back({.kind=IR::ExportTargetKind::Mrt,.index=0,.en=15});
	IR::ImageResource image {};
	image.resource_class=IR::ImageResourceClass::Sampled; image.numeric_class=Prospero::TextureNumericClass::Float;
	image.dimension=Decoder::ImageDimension::Dim2D; image.mip_mode=IR::ImageMipMode::Dynamic; image.mip_count=3;
	image.read=image.depth_compare=true;
	f.program.info.images.push_back(image); f.program.info.samplers.emplace_back(); f.program.info.samplers[0].depth_compare=true;
	f.program.bindings.descriptors.push_back({*IR::DescriptorBindingForImage(image),{0}});
	f.program.bindings.descriptors.push_back({IR::DescriptorBindingKind::Samplers,{0}});
	const auto tex=f.Emit(0,ValueOpcode::GetImageResource,{Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u)});
	const auto sampler=f.Emit(0,ValueOpcode::GetSamplerResource,{Value(0u),Value(0u),Value(0u),Value(0u)});
	const auto u=f.Emit(0,ValueOpcode::GetAttribute,{Value(0u),Value(0u)}),v=f.Emit(0,ValueOpcode::GetAttribute,{Value(0u),Value(1u)});
	const auto address=f.Emit(0,ValueOpcode::MakeImageAddress,{Value(std::bit_cast<uint32_t>(.425f)),u,v,Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u)});
	const auto index=f.program.memory_info.size();
	IR::MemoryInfo memory {}; memory.kind=IR::ResourceKind::Image; memory.dmask=15;
	memory.image_sample_flags=Decoder::ImageSampleFlagCompare; memory.image_dimension=Decoder::ImageDimension::Dim2D; memory.image_address_components=3;
	f.program.memory_info.push_back(memory);
	const auto sampled=f.Emit(0,ValueOpcode::ImageSampleRaw,{tex,sampler,address},index);
	const auto scalar=f.Emit(0,ValueOpcode::CompositeExtractU32x4,{sampled,Value(0u)});
	f.Emit(0,ValueOpcode::SetAttribute,{f.Emit(0,ValueOpcode::CompositeConstructU32x4,{scalar,scalar,scalar,scalar}),Value(true)});
	ShaderPixelInputInfo pixel {}; pixel.input_num=1;
	id<MTLFunction> fragment=Compile(f,&pixel);
	NSString* vertex_source=@"#include <metal_stdlib>\nusing namespace metal;\n"
	    "struct V { float4 pos [[position]]; float4 uv [[user(locn0)]]; };\n"
	    "vertex V fullscreen(uint id [[vertex_id]], constant float& scale [[buffer(1)]]) {\n"
	    " const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};\n"
	    " V o; o.pos=float4(p[id],0,1); o.uv=float4((float2(p[id].x,-p[id].y)*.5f+.5f)*scale,0,0); return o; }";
	NSError* error=nil; id<MTLLibrary> library=[device newLibraryWithSource:vertex_source options:nil error:&error];
	Check(library!=nil,"comparison vertex shader failed");
	MTLRenderPipelineDescriptor* pd=[MTLRenderPipelineDescriptor new];
	pd.vertexFunction=[library newFunctionWithName:@"fullscreen"]; pd.fragmentFunction=fragment;
	pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Float;
	id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
	if (!pipeline) std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);
	Check(pipeline!=nil,"comparison fragment pipeline failed");
	id<MTLArgumentEncoder> args=[fragment newArgumentEncoderWithBufferIndex:0];
	id<MTLBuffer> ab=[device newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];
	[args setArgumentBuffer:ab offset:0]; [args setTexture:source.texture atIndex:64];
	MTLTextureDescriptor* td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:8 height:8 mipmapped:NO];
	td.usage=MTLTextureUsageRenderTarget; td.storageMode=MTLStorageModeShared;
	id<MTLTexture> target=[device newTextureWithDescriptor:td];
	id<MTLBuffer> readback=[device newBufferWithLength:256*8 options:MTLResourceStorageModeShared];
	Check(target && readback,"comparison render target failed");
	unsigned checked=0;
	for (unsigned mode=0;mode<4;++mode) for (unsigned func : {1u,3u,4u,6u}) {
		const float scale=mode==0?.5f:mode==3?1.f:4.f;
		ComparisonCase c; c.function=func; c.mip=2; c.min_linear=mode==1; c.mag_linear=!c.min_linear;
		c.lod=std::log2(scale/2.f); c.bias=mode==1?128:mode==2?-128:mode==3?256:0;
		if (mode==3) c.min_lod=128;
		ShaderSamplerResource guest {};
		guest.fields[0]=2u|(2u<<3u)|(2u<<6u)|(func<<12u); guest.fields[1]=c.min_lod|(c.max_lod<<12u);
		guest.fields[2]=uint32_t(c.bias&0x3fff)|(c.min_linear?1u<<22u:0u)|(c.mag_linear?1u<<20u:0u)|(2u<<26u);
		[args setSamplerState:Metal::GetSampler(device,guest) atIndex:128];
		[args setSamplerState:Metal::GetSampler(device,guest,true) atIndex:Msl::ComparisonPointSamplerBase];
		uint32_t push[32] {},sizes[Msl::SizeConstantWordCount] {}; sizes[64]=func;
		sizes[Msl::ImageMetadataWordBase]=Metal::SupportsNativeDepthCompare(source.texture.pixelFormat)?1u:0u;
		std::copy_n(guest.fields,4,sizes+Msl::SamplerDescriptorWordBase);
		MTLRenderPassDescriptor* pass=[MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture=target; pass.colorAttachments[0].loadAction=MTLLoadActionClear;
		pass.colorAttachments[0].storeAction=MTLStoreActionStore; pass.colorAttachments[0].clearColor=MTLClearColorMake(-1,-1,-1,-1);
		id<MTLCommandBuffer> command=[source.queue commandBuffer]; id<MTLRenderCommandEncoder> render=[command renderCommandEncoderWithDescriptor:pass];
		[render setRenderPipelineState:pipeline]; [render setCullMode:MTLCullModeNone];
		[render setVertexBytes:&scale length:sizeof(scale) atIndex:1]; [render setFragmentBuffer:ab offset:0 atIndex:0];
		[render setFragmentBytes:push length:sizeof(push) atIndex:1]; [render setFragmentBytes:sizes length:sizeof(sizes) atIndex:25];
		[render useResource:source.texture usage:MTLResourceUsageRead];
		[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [render endEncoding];
		id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
		[blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1)
		          toBuffer:readback destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256*8];
		[blit endEncoding]; [command commit]; [command waitUntilCompleted];
		Check(command.status==MTLCommandBufferStatusCompleted,"comparison fragment execution failed");
		for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) {
			c.u=(x+.5f)/8.f*scale; c.v=(y+.5f)/8.f*scale;
			const auto expected=source.Model(c)[0];
			const auto* values=reinterpret_cast<const float*>(static_cast<const uint8_t*>(readback.contents)+y*256)+x*4;
			for (unsigned component=0;component<4;++component) {
				if (!std::isfinite(values[component]) || std::fabs(values[component]-expected)>1.f/64) {
					std::fprintf(stderr,"fragment compare mode=%u cmp=%u xy=%u,%u: expected %g got %g\n",mode,func,x,y,expected,values[component]);
					throw std::runtime_error("implicit comparison sampling mismatch");
				}
				++checked;
			}
		}
	}
	std::printf("Fragment comparison: %u GPU components, varying UV/implicit LOD/min-mag/bias/clamps PASS\n",checked);
}
