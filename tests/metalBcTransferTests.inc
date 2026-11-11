static void CheckBcPhysicalTransfers(id<MTLDevice> device,id<MTLCommandQueue> queue) {
	using F=Prospero::BufferFormat;
	const std::pair<F,MTLPixelFormat> formats[]={
		{F::kBc1UNorm,MTLPixelFormatBC1_RGBA},{F::kBc1Srgb,MTLPixelFormatBC1_RGBA_sRGB},
		{F::kBc2UNorm,MTLPixelFormatBC2_RGBA},{F::kBc2Srgb,MTLPixelFormatBC2_RGBA_sRGB},
		{F::kBc3UNorm,MTLPixelFormatBC3_RGBA},{F::kBc3Srgb,MTLPixelFormatBC3_RGBA_sRGB},
		{F::kBc4UNorm,MTLPixelFormatBC4_RUnorm},{F::kBc4SNorm,MTLPixelFormatBC4_RSnorm},
		{F::kBc5UNorm,MTLPixelFormatBC5_RGUnorm},{F::kBc5SNorm,MTLPixelFormatBC5_RGSnorm},
		{F::kBc6UFloat,MTLPixelFormatBC6H_RGBUfloat},{F::kBc6SFloat,MTLPixelFormatBC6H_RGBFloat},
		{F::kBc7UNorm,MTLPixelFormatBC7_RGBAUnorm},{F::kBc7Srgb,MTLPixelFormatBC7_RGBAUnorm_sRGB}
	};
	const Prospero::TileMode modes[]={Prospero::TileMode::kStandard4KB,Prospero::TileMode::kStandard64KB,Prospero::TileMode::kPrt,Prospero::TileMode::kStandard256B};
	uint64_t checked=0;unsigned cases=0,negatives=0;
	for(const auto [format,metal]:formats)for(const auto mode:modes) {
		TileSurfaceLayout layout{};
		Check(TileGetTiledTextureLayout({format,mode,TileSurfaceDimension::Dim2D,37,21,1,6,3},layout),"BC physical layout");
		const uint32_t eb=layout.texture.block.bytes_per_element;
		std::vector<uint8_t> source(layout.total_size+1024,0x6d),expected(source.size(),0xa5);
		for(uint32_t layer=0;layer<3;++layer)for(uint32_t m=0;m<6;++m)for(uint32_t y=0;y<layout.mips[m].height;++y)for(uint32_t x=0;x<layout.mips[m].width;++x) {
			const uint64_t pos=MipByteOffset(layout,m,layer,x,y);
			for(uint32_t c=0;c<eb;++c)source[256+pos+c]=expected[512+pos+c]=uint8_t(3+layer*73+m*29+x*7+y*11+c*17);
		}
		auto upload=[device newBufferWithBytes:source.data() length:source.size() options:MTLResourceStorageModeShared];
		auto tiled=[device newBufferWithLength:source.size() options:MTLResourceStorageModePrivate];
		auto output=[device newBufferWithLength:source.size() options:MTLResourceStorageModeShared];
		std::memset(output.contents,0xa5,output.length);
		auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:metal width:37 height:21 mipmapped:YES];
		td.mipmapLevelCount=6;td.textureType=MTLTextureType2DArray;td.arrayLength=3;
		td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageShaderRead;
		auto image=[device newTextureWithDescriptor:td];Check(image!=nil,"BC array GPU allocation");
		auto command=[queue commandBuffer];auto blit=[command blitCommandEncoder];
		[blit copyFromBuffer:upload sourceOffset:0 toBuffer:tiled destinationOffset:0 size:source.size()];[blit endEncoding];
		for(uint32_t layer=0;layer<3;++layer)for(uint32_t m=0;m<6;++m) {
			Check(EncodeTileBcImport(device,command,tiled,256,image,layout,m,layer,m,layer),"BC GPU import mip/layer");
			Check(EncodeTileBcExport(device,command,image,output,512,layout,m,layer,m,layer),"BC GPU export mip/layer");
		}
		// Invalid contracts must leave all guest bytes unchanged.
		Check(!EncodeTileBcImport(device,command,tiled,256,image,layout,6),"BC invalid mip accepted");++negatives;
		Check(!EncodeTileBcExport(device,command,image,output,512,layout,0,3),"BC invalid layer accepted");++negatives;
		Check(!EncodeTileBcImport(device,command,tiled,tiled.length,image,layout),"BC invalid source bounds accepted");++negatives;
		Check(!EncodeTileBcExport(device,command,image,output,output.length,layout),"BC invalid destination bounds accepted");++negatives;
		Check(!EncodeTileBcImport(device,command,tiled,256,image,layout,0,0,1),"BC wrong pixel mip shape accepted");++negatives;
		Check(!EncodeTileBcImport(device,command,tiled,256,image,layout,0,0,0,3),"BC invalid host slice accepted");++negatives;
		auto wrong=layout;wrong.texture.block.bytes_per_element=eb==8?16:8;
		Check(!EncodeTileBcImport(device,command,tiled,256,image,wrong),"BC wrong block byte size accepted");++negatives;
		[command commit];[command waitUntilCompleted];Check(command.status==MTLCommandBufferStatusCompleted,"BC physical transfer GPU execution");
		Check(std::memcmp(output.contents,expected.data(),expected.size())==0,"BC blocks, tail, padding, layer or guard mismatch");checked+=expected.size();

		// BASE_MIP selection: a physical tail mip has its own pixel dimensions,
		// independent of the full chain's block-coordinate mip rounding.
		auto single_desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:metal width:2 height:1 mipmapped:NO];
		single_desc.storageMode=MTLStorageModePrivate;single_desc.usage=MTLTextureUsageShaderRead;
		auto single=[device newTextureWithDescriptor:single_desc];Check(single!=nil,"BC selected tail allocation");
		std::memset(output.contents,0xa5,output.length);std::fill(expected.begin(),expected.end(),0xa5);
		const uint64_t tail=MipByteOffset(layout,4,2,0,0);
		std::copy_n(source.begin()+256+tail,eb,expected.begin()+512+tail);
		command=[queue commandBuffer];
		Check(EncodeTileBcImport(device,command,tiled,256,single,layout,4,2),"BC selected tail import");
		Check(EncodeTileBcExport(device,command,single,output,512,layout,4,2),"BC selected tail export");
		[command commit];[command waitUntilCompleted];Check(command.status==MTLCommandBufferStatusCompleted,"BC selected tail execution");
		Check(std::memcmp(output.contents,expected.data(),expected.size())==0,"BC selected tail changed neighbours");checked+=expected.size();++cases;
	}

	// Independent decoded-colour check with a valid all-red BC1 block, including
	// pixels on partial 4x4 edge blocks. No lossy recompression occurs in transfer.
	TileSurfaceLayout red_layout{};
	Check(TileGetTiledTextureLayout({F::kBc1UNorm,Prospero::TileMode::kStandard4KB,TileSurfaceDimension::Dim2D,7,5,1,1,1},red_layout),"BC1 red layout");
	auto red_bytes=[device newBufferWithLength:red_layout.total_size options:MTLResourceStorageModeShared];
	std::memset(red_bytes.contents,0,red_bytes.length);
	for(uint32_t y=0;y<2;++y)for(uint32_t x=0;x<2;++x){const uint16_t red=0xf800;std::memcpy(static_cast<uint8_t*>(red_bytes.contents)+MipByteOffset(red_layout,0,0,x,y),&red,2);}
	auto red_desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC1_RGBA width:7 height:5 mipmapped:NO];
	red_desc.storageMode=MTLStorageModePrivate;red_desc.usage=MTLTextureUsageShaderRead;
	auto red_image=[device newTextureWithDescriptor:red_desc];
	NSError* error=nil;
	auto lib=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void read_bc(texture2d<float,access::read> t [[texture(0)]],device float4* out [[buffer(0)]],uint2 p [[thread_position_in_grid]]){out[p.y*7+p.x]=t.read(p);}"
	    options:nil error:&error];Check(lib!=nil,"BC sample shader");
	auto pipeline=[device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"read_bc"] error:&error];Check(pipeline!=nil,"BC sample pipeline");
	auto values=[device newBufferWithLength:7*5*16 options:MTLResourceStorageModeShared];auto command=[queue commandBuffer];
	Check(EncodeTileBcImport(device,command,red_bytes,0,red_image,red_layout),"BC1 red import");
	auto e=[command computeCommandEncoder];OrderBegin(e);[e setComputePipelineState:pipeline];[e setTexture:red_image atIndex:0];[e setBuffer:values offset:0 atIndex:0];
	[e dispatchThreads:MTLSizeMake(7,5,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];OrderEnd(e);[e endEncoding];
	[command commit];[command waitUntilCompleted];Check(command.status==MTLCommandBufferStatusCompleted,"BC1 decoded sample execution");
	for(unsigned i=0;i<7*5*4;++i)Check(static_cast<const float*>(values.contents)[i]==((i%4==0 || i%4==3)?1.0f:0.0f),"BC1 decoded edge texel mismatch");
	std::printf("BC canonical blocks: %u format/layout cases, %u negatives, %llu exact bytes, selected tails/3 layers/odd extents, 35 decoded BC1 texels PASS\n",cases,negatives,(unsigned long long)checked);
}
