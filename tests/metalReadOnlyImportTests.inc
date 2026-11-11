// The raw view of a renderable sRGB image inherits usage without ShaderWrite.
// Import must use a writable bridge and a bit-preserving buffer blit.
static void CheckReadOnlyImports(id<MTLDevice> device,id<MTLCommandQueue> queue) {
	const MTLPixelFormat formats[]{MTLPixelFormatR8Unorm,MTLPixelFormatR16Float,MTLPixelFormatRGBA8Unorm_sRGB,
	    MTLPixelFormatBGRA8Unorm_sRGB,MTLPixelFormatRGBA16Float,MTLPixelFormatRGBA32Float};
	const Prospero::BufferFormat elements[]{Prospero::BufferFormat::k8UInt,Prospero::BufferFormat::k16UInt,Prospero::BufferFormat::k32UInt,
	    Prospero::BufferFormat::k32UInt,Prospero::BufferFormat::k32_32UInt,Prospero::BufferFormat::k32_32_32_32UInt};
	unsigned cases=0,subresources=0;uint64_t checked=0;
	for(auto tile:{Prospero::TileMode::kStandard4KB,Prospero::TileMode::kStandard64KB,Prospero::TileMode::kRenderTarget})for(unsigned f=0;f<6;++f) {
		TileSurfaceLayout layout{};Check(TileGetTiledTextureLayout({elements[f],tile,TileSurfaceDimension::Dim2D,37,21,1,6,3},layout),"read-only image layout");
		const uint32_t eb=layout.texture.block.bytes_per_element;
		id<MTLBuffer> guest=[device newBufferWithLength:layout.total_size+512 options:MTLResourceStorageModeShared];
		id<MTLBuffer> output=[device newBufferWithLength:guest.length options:MTLResourceStorageModeShared];
		Check(guest && output,"read-only physical buffers");
		for(uint64_t i=0;i<guest.length;++i)static_cast<uint8_t*>(guest.contents)[i]=uint8_t(i*37+i/17+9);
		std::memset(output.contents,0xa5,output.length);std::vector<uint8_t> expected(output.length,0xa5);
		MTLTextureDescriptor* td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:formats[f] width:37 height:21 mipmapped:YES];
		td.textureType=MTLTextureType2DArray;td.arrayLength=3;td.mipmapLevelCount=6;td.storageMode=MTLStorageModePrivate;
		td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;
		id<MTLTexture> parent=[device newTextureWithDescriptor:td],raw=[parent newTextureViewWithPixelFormat:TileElementFormat(eb)];
		Check(raw && !(raw.usage & MTLTextureUsageShaderWrite),"read-only test accidentally enabled texture stores");
		auto descriptor=[MTLCommandBufferDescriptor new];descriptor.retainedReferences=NO;
		id<MTLCommandBuffer> cmd=[queue commandBufferWithDescriptor:descriptor];
		for(auto selected:{std::array<uint32_t,2>{0,0},std::array<uint32_t,2>{2,1},std::array<uint32_t,2>{5,2}}) {
			const auto [mip,layer]=selected;const auto& m=layout.mips[mip];const auto& b=layout.texture.block;
			Check(EncodeTileTextureImport(device,cmd,guest,256,raw,layout,mip,layer,mip,layer),"read-only raw import");
			Check(EncodeTileTextureExport(device,cmd,raw,output,256,layout,mip,layer,mip,layer),"read-only raw export");
			for(uint32_t y=0;y<m.height;++y)for(uint32_t x=0;x<m.width;++x) {
				const uint32_t tx=x+(mip>=layout.first_tail_level?m.tail_x:0),ty=y+(mip>=layout.first_tail_level?m.tail_y:0);uint32_t within=0,bank=0;
				Check(TileGetBlockOffset(b,tx%b.block_width,ty%b.block_height,0,within)&&TileGetBlockXor(b,tx/b.block_width,ty/b.block_height,layer,bank),"read-only scalar address");
				const uint64_t at=256+uint64_t(layer)*layout.block_slice_size+m.offset+
				    (uint64_t(ty/b.block_height)*(m.padded_width/b.block_width)+tx/b.block_width)*b.block_size+(within^bank);
				std::memcpy(expected.data()+at,static_cast<uint8_t*>(guest.contents)+at,eb);
			}++subresources;
		}
		[cmd commit];[cmd waitUntilCompleted];Check(cmd.status==MTLCommandBufferStatusCompleted,"read-only import GPU execution");
		Check(!std::memcmp(output.contents,expected.data(),expected.size()),"read-only import converted bits or changed another mip/layer/padding/guard");
		checked+=expected.size();++cases;
	}
	std::printf("Read-only typed imports: %u GPU formats/layout cases, %u mip/layer round trips, %llu exact bytes; sRGB/float/raw bits, unretained command lifetime, padding/guards PASS\n",cases,subresources,(unsigned long long)checked);
}
