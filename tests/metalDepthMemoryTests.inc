#include "graphics/host_gpu/metal/metalDepthMemory.h"
static void CheckDepthMemory(id<MTLDevice> device,id<MTLCommandQueue> queue) {
	unsigned scenarios=0,negatives=0;uint64_t checked=0;
	// Unrelated VA mapping changes must not invalidate an already prepared
	// draw; an actual remap of its physical bytes must invalidate it.
	std::vector<PhysicalMapping> maps{{0x1000,0x2000,0x8000}};
	std::vector<PhysicalSpan> before,after;
	Check(ResolvePhysicalSpans(maps,0x1800,0x1000,before),"depth mapping identity initial");
	maps.push_back({0x9000,0x1000,0x10000});
	Check(ResolvePhysicalSpans(maps,0x1800,0x1000,after)&&before==after,"unrelated mapping changed prepared depth identity");
	maps[0].backing_offset=0x18000;
	Check(ResolvePhysicalSpans(maps,0x1800,0x1000,after)&&before!=after,"physical remap retained prepared depth identity");

	for(auto fmt:{MTLPixelFormatDepth16Unorm,MTLPixelFormatDepth32Float,MTLPixelFormatStencil8}) {
		const uint32_t eb=fmt==MTLPixelFormatDepth16Unorm?2:fmt==MTLPixelFormatDepth32Float?4:1;
		const auto guest_fmt=eb==2?Prospero::BufferFormat::k16UInt:eb==4?Prospero::BufferFormat::k32UInt:Prospero::BufferFormat::k8UInt;
		TileSurfaceLayout layout{},colour{};
		Check(TileGetTiledTextureLayout({guest_fmt,Prospero::TileMode::kDepth,TileSurfaceDimension::Dim2D,37,23,1,1,4},layout),"depth canonical layout");
		Check(TileGetTiledTextureLayout({Prospero::BufferFormat::k32_32UInt,Prospero::TileMode::kRenderTarget,TileSurfaceDimension::Dim2D,37,23,1,1,1},colour),"depth colour reader");
		const uint64_t size=layout.total_size,chunk=65536;
		id<MTLBuffer> physical=[device newBufferWithLength:size+512 options:MTLResourceStorageModeShared];Check(physical!=nil,"depth physical memory");
		std::vector<uint8_t> truth(physical.length);for(size_t i=0;i<truth.size();++i)truth[i]=uint8_t(i*59+i/13+7);
		const std::vector<PhysicalSpan> spans{{0,chunk*2,chunk},{chunk,0,chunk},{chunk*2,chunk*3,chunk},{chunk*3,chunk,chunk}};
		auto physicalAt=[&](uint64_t at){for(const auto&s:spans)if(at>=s.offset&&at<s.offset+s.size)return s.physical+at-s.offset;throw std::runtime_error("depth oracle physical address");};
		auto put=[&](uint32_t layer,uint32_t x,uint32_t y,uint32_t bits){const uint64_t at=MipByteOffset(layout,0,layer,x,y);for(uint32_t b=0;b<eb;++b)truth[physicalAt(at+b)]=uint8_t(bits>>(b*8));};
		for(uint32_t layer=0;layer<4;++layer)for(uint32_t y=0;y<23;++y)for(uint32_t x=0;x<37;++x) {
			const uint32_t raw_float[]={0x00000000u,0x80000000u,0x00000001u,0x7fc12345u,0x7f800000u,0xff800000u,0xbe000000u,0x3fa00000u};
			const uint32_t bits=eb==4?raw_float[(layer+y+x)%8]:eb==2?(x*997u+y*313u+layer*0x2000u)&65535u:(x*17u+y*29u+layer*53u)&255u;
			put(layer,x,y,bits);
		}
		std::memcpy(physical.contents,truth.data(),truth.size());
		CanonicalMemoryView memory;Check(memory.Initialize(device,spans,size,chunk,[&](uint64_t at,uint64_t n)->std::optional<CanonicalMemoryView::BufferSpan>{if(at+n>size)return std::nullopt;return CanonicalMemoryView::BufferSpan{physical,at};}),"depth canonical staging");
		MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:37 height:23 mipmapped:NO];
		d.textureType=MTLTextureType2DArray;d.arrayLength=2;d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
		id<MTLTexture> selected=[device newTextureWithDescriptor:d];Check(selected!=nil,"depth selected layer view");
		DepthMemoryTransfer transfer;Check(transfer.Initialize(device,selected,layout,1,2,memory,memory),"depth memory transfer plan");
		DepthMemoryTransfer invalid;
		Check(!invalid.Initialize(device,selected,layout,1,0,memory,memory),"depth zero layers accepted");++negatives;
		Check(!invalid.Initialize(device,selected,layout,3,2,memory,memory),"depth out of range layers accepted");++negatives;
		auto bad=layout;bad.texture.block.bytes_per_element=eb==4?2:4;
		Check(!invalid.Initialize(device,selected,bad,1,2,memory,memory),"depth wrong raw element accepted");++negatives;
		for(unsigned phase=0;phase<3;++phase) {
			id<MTLCommandBuffer> command=[queue commandBuffer];
			if(phase==1) {
				// A CPU update after earlier GPU completion must replace cached depth.
				put(1,9,7,eb==4?0x3f400000u:eb==2?0x1234u:0x7du);
				std::memcpy(physical.contents,truth.data(),truth.size());
			}
			if(phase==2) {
				const uint32_t bits=eb==4?0x3e800000u:eb==2?0x2345u:0x6bu;
				const uint64_t at=MipByteOffset(layout,0,2,3,5),physical_at=physicalAt(at);
				id<MTLBuffer> produced=[device newBufferWithBytes:&bits length:4 options:MTLResourceStorageModeShared];
				id<MTLBlitCommandEncoder>b=[command blitCommandEncoder];OrderBegin(b);[b copyFromBuffer:produced sourceOffset:0 toBuffer:physical destinationOffset:physical_at size:eb];OrderEnd(b);[b endEncoding];put(2,3,5,bits);
			}
			Check(transfer.Load(command)&&transfer.Store(command),"depth/stencil lossless load/store");
			[command commit];[command waitUntilCompleted];Check(command.status==MTLCommandBufferStatusCompleted,"depth canonical GPU completion");
			Check(!std::memcmp(physical.contents,truth.data(),truth.size()),"depth/stencil lost input bits/CPU/raw update or touched other layers/padding");checked+=truth.size();++scenarios;
		}
		id<MTLCommandBuffer> command=[queue commandBuffer];
		Check(transfer.Load(command),"depth load before partial layer clear");
		// Clear host slice 1 only: it is physical slice 2, not slice zero.
		MTLRenderPassDescriptor*p=[MTLRenderPassDescriptor renderPassDescriptor];
		if(eb==1){p.stencilAttachment.texture=selected;p.stencilAttachment.slice=1;p.stencilAttachment.loadAction=MTLLoadActionClear;p.stencilAttachment.storeAction=MTLStoreActionStore;p.stencilAttachment.clearStencil=0x5a;}
		else {p.depthAttachment.texture=selected;p.depthAttachment.slice=1;p.depthAttachment.loadAction=MTLLoadActionClear;p.depthAttachment.storeAction=MTLStoreActionStore;p.depthAttachment.clearDepth=eb==2?1.:.375;}
		id<MTLRenderCommandEncoder> render=[command renderCommandEncoderWithDescriptor:p];OrderBegin(render);OrderEnd(render);[render endEncoding];
		Check(transfer.Store(command),"depth selected clear export");
		const uint32_t clear=eb==4?0x3ec00000u:eb==2?65535u:0x5au;
		for(uint32_t y=0;y<23;++y)for(uint32_t x=0;x<37;++x)put(2,x,y,clear);
		// Read the same bytes with a larger element size and another swizzle.
		Check(memory.Gather(command),"depth colour gather");
		MTLTextureDescriptor*r=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Uint width:37 height:23 mipmapped:NO];r.storageMode=MTLStorageModeShared;r.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
		id<MTLTexture> reader=[device newTextureWithDescriptor:r];Check(reader!=nil,"depth colour image");
		Check(EncodeTileTextureImport(device,command,memory.buffer,memory.offset,reader,colour),"depth colour physical import");
		[command commit];[command waitUntilCompleted];Check(command.status==MTLCommandBufferStatusCompleted,"depth clear+alias completion");
		Check(!std::memcmp(physical.contents,truth.data(),truth.size()),"depth clear corrupted other slices/guards");
		std::vector<uint8_t> logical(size);for(const auto&s:spans)std::memcpy(logical.data()+s.offset,truth.data()+s.physical,s.size);
		std::vector<uint8_t> expected;uint32_t pitch=0;Check(DecodeTiledTextureLevel(logical,colour,0,0,expected,pitch),"depth colour oracle");
		std::vector<uint8_t> actual(expected.size());[reader getBytes:actual.data() bytesPerRow:pitch fromRegion:MTLRegionMake2D(0,0,37,23) mipmapLevel:0];
		Check(actual==expected,"depth -> colour alias changed physical byte meaning");checked+=truth.size()+actual.size();++scenarios;
	}
	std::printf("Depth canonical memory: D16/D32/S8, selected layers, CPU+raw updates, discontiguous backing, colour aliases: %u cases, %u negatives, %llu bytes PASS\n",scenarios,negatives,(unsigned long long)checked);
}
