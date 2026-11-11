static uint64_t VolumeByteOffset(const TileSurfaceLayout& l,uint32_t level,uint32_t x,uint32_t y,uint32_t z) {
	const auto& m=l.mips[level];const auto& b=l.texture.block;
	if(l.description.tile_mode==Prospero::TileMode::kLinear)return uint64_t(z)*l.block_slice_size+m.offset+(uint64_t(y)*m.padded_width+x)*b.bytes_per_element;
	const uint32_t tx=x+(level>=l.first_tail_level?m.tail_x:0),ty=y+(level>=l.first_tail_level?m.tail_y:0);
	uint32_t within=0,bank=0;
	Check(TileGetBlockOffset(b,tx%b.block_width,ty%b.block_height,z%b.block_depth,within),"volume scalar offset");
	Check(TileGetBlockXor(b,tx/b.block_width,ty/b.block_height,z/b.block_depth,bank),"volume scalar bank");
	return uint64_t(z/b.block_depth)*l.block_slice_size+m.offset+
	    (uint64_t(ty/b.block_height)*(m.padded_width/b.block_width)+tx/b.block_width)*b.block_size+(within^bank);
}
static ShaderTextureResource VolumeDescriptor(uint32_t depth,uint32_t levels,uint32_t base,uint32_t last,Prospero::TileMode tile) {
	ShaderTextureResource t{};t.fields[3]=uint32_t(Prospero::ImageType::kColor3D)<<28|uint32_t(tile)<<20|base<<12|last<<16;
	t.fields[4]=depth-1;t.fields[5]=(levels-1)<<4;return t;
}
static void CheckVolumeMemory(id<MTLDevice> device,id<MTLCommandQueue> queue) {
	NSError* error=nil;
	id<MTLLibrary> library=[device newLibraryWithSource:@R"(
#include <metal_stdlib>
using namespace metal;
kernel void read_volume(texture3d<uint,access::read> t [[texture(0)]],device uint4* p [[buffer(0)]],uint3 g [[thread_position_in_grid]]) {
 p[(g.z*t.get_height()+g.y)*t.get_width()+g.x]=t.read(g);
}
kernel void write_volume(texture3d<uint,access::read_write> t [[texture(0)]],constant uint& eb [[buffer(1)]],uint3 g [[thread_position_in_grid]]) {
 uint i=(g.z*t.get_height()+g.y)*t.get_width()+g.x;
 uint mask=eb==1?0xffu:eb==2?0xffffu:0xffffffffu;
 if((g.x+g.y+g.z)%3==0)t.write(uint4(0x12345678u^i,0x87654321u^i,0x35791357u^i,0xfedcba98u^i)&mask,g);
}
kernel void typed_volume(texture3d<float,access::write> t [[texture(0)]],uint3 g [[thread_position_in_grid]]) {t.write(float4(1,2,4,1),g);}
)" options:nil error:&error];
	if(!library)std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);
	Check(library!=nil,"volume memory shader library");
	const auto pipeline=[&](NSString* name){id<MTLComputePipelineState> p=[device newComputePipelineStateWithFunction:[library newFunctionWithName:name] error:&error];Check(p!=nil,"volume memory pipeline");return p;};
	const auto read=pipeline(@"read_volume"),write=pipeline(@"write_volume"),typed=pipeline(@"typed_volume");
	const auto encode=[&](id<MTLCommandBuffer> cmd,id<MTLComputePipelineState> p,id<MTLTexture> t,id<MTLBuffer> out) {
		id<MTLComputeCommandEncoder> e=[cmd computeCommandEncoder];OrderBegin(e);[e setComputePipelineState:p];[e setTexture:t atIndex:0];
		if(out)[e setBuffer:out offset:0 atIndex:0];
		if(p==write){const uint32_t eb=t.pixelFormat==MTLPixelFormatR8Uint?1:t.pixelFormat==MTLPixelFormatR16Uint?2:4;[e setBytes:&eb length:sizeof(eb) atIndex:1];}
		[e dispatchThreads:MTLSizeMake(t.width,t.height,t.depth) threadsPerThreadgroup:MTLSizeMake(4,4,4)];OrderEnd(e);[e endEncoding];
	};
	const auto finish=[&](id<MTLCommandBuffer> cmd){[cmd commit];[cmd waitUntilCompleted];Check(cmd.status==MTLCommandBufferStatusCompleted,"volume GPU completion");};
	const MTLPixelFormat native[]{MTLPixelFormatR8Unorm,MTLPixelFormatR16Float,MTLPixelFormatRG11B10Float,MTLPixelFormatRGBA16Float,MTLPixelFormatRGBA32Float};
	const Prospero::BufferFormat format[]{Prospero::BufferFormat::k8UInt,Prospero::BufferFormat::k16UInt,Prospero::BufferFormat::k32UInt,
	    Prospero::BufferFormat::k32_32UInt,Prospero::BufferFormat::k32_32_32_32UInt};
	unsigned cases=0,negative=0,typed_cases=0;uint64_t checked=0;
	for(auto tile:{Prospero::TileMode::kStandard4KB,Prospero::TileMode::kStandard64KB,Prospero::TileMode::kPrt,Prospero::TileMode::kLinear})
	for(unsigned fi=0;fi<5;++fi)for(unsigned mode=0;mode<3;++mode) {
		const uint32_t eb=1u<<fi,w=mode==2?1:67,h=mode==2?1:35,d=mode==2?33:19;
		const bool repeated_tail=mode==2 && tile!=Prospero::TileMode::kLinear;
		const uint32_t levels=repeated_tail?8:6,base=mode==0?0:mode==1?2:levels-1,last=mode==0?levels-1:mode==1?3:levels-1;
		auto descriptor=VolumeDescriptor(d,levels,base,last,tile);
		GuestMipViewPlan plan;Check(PlanGuestMipView(descriptor,w,h,false,plan,d),"volume mip plan");
		Check(!repeated_tail || (plan.first_level==7 && plan.texture_levels==1 && plan.physical_levels==8),"volume repeated tail plan");
		TileSurfaceLayout layout{};Check(PlanVolumeLayout({format[fi],tile,TileSurfaceDimension::Dim3D,w,h,d,levels,1},layout),"volume physical layout");
		const uint64_t size=layout.total_size,part=size/2;
		id<MTLBuffer> physical=[device newBufferWithLength:size+512 options:MTLResourceStorageModeShared];Check(physical!=nil,"volume physical bytes");
		std::vector<uint8_t> truth(physical.length),logical(size);
		for(size_t i=0;i<truth.size();++i)truth[i]=uint8_t(i*71+i/127+11);
		std::memcpy(physical.contents,truth.data(),truth.size());
		const std::vector<PhysicalSpan> spans{{0,256+part,part},{part,256,size-part}};
		const auto physical_at=[&](uint64_t at){for(const auto& s:spans)if(at>=s.offset && at<s.offset+s.size)return s.physical+at-s.offset;throw std::runtime_error("volume physical offset");};
		for(const auto& s:spans)std::memcpy(logical.data()+s.offset,truth.data()+s.physical,s.size);
		CanonicalMemoryView memory;
		Check(memory.Initialize(device,spans,size,4096,[&](uint64_t p,uint64_t n)->std::optional<CanonicalMemoryView::BufferSpan>{
			if(p>physical.length || n>physical.length-p)return std::nullopt;return CanonicalMemoryView::BufferSpan{physical,p};}),"volume physical chunks");
		MTLTextureDescriptor* td=[MTLTextureDescriptor new];td.textureType=MTLTextureType3D;td.pixelFormat=native[fi];
		td.width=std::max(w>>plan.first_level,1u);td.height=std::max(h>>plan.first_level,1u);td.depth=std::max(d>>plan.first_level,1u);
		td.mipmapLevelCount=plan.texture_levels;td.storageMode=MTLStorageModePrivate;
		td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsagePixelFormatView;
		id<MTLTexture> parent=[device newTextureWithDescriptor:td],raw=[parent newTextureViewWithPixelFormat:TileElementFormat(eb)];Check(raw!=nil,"typed volume raw view");
		VolumeMemoryTransfer transfer;
		Check(transfer.Initialize(device,raw,layout,plan,base,last,memory),"production volume transfer");
		const uint32_t selected=base-plan.first_level;
		id<MTLTexture> view=[raw newTextureViewWithPixelFormat:raw.pixelFormat textureType:MTLTextureType3D levels:NSMakeRange(selected,last-base+1) slices:NSMakeRange(0,1)];
		Check(view!=nil && view.width==layout.mips[base].width && view.depth==std::max(d>>base,1u),"volume BASE_LEVEL view shape");
		id<MTLBuffer> output=[device newBufferWithLength:view.width*view.height*view.depth*16 options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cmd=[queue commandBuffer];Check(transfer.Load(cmd),"volume initial gather/import");encode(cmd,read,view,output);finish(cmd);
		const auto check_read=[&] {
			const auto* values=static_cast<const uint32_t*>(output.contents);
			for(uint32_t z=0;z<view.depth;++z)for(uint32_t y=0;y<view.height;++y)for(uint32_t x=0;x<view.width;++x) {
				const uint64_t at=VolumeByteOffset(layout,base,x,y,z),i=(uint64_t(z)*view.height+y)*view.width+x;
				for(uint32_t byte=0;byte<eb;++byte) {
					const auto got=uint8_t(values[i*4+byte/4]>>(8*(byte%4)));
					if(got!=logical[at+byte])std::fprintf(stderr,"volume mismatch case=%u tile=%u eb=%u mode=%u mip=%u xyz=%u,%u,%u byte=%u off=%llu got=%02x expected=%02x\n",cases,unsigned(tile),eb,mode,base,x,y,z,byte,(unsigned long long)at,got,logical[at+byte]);
					Check(got==logical[at+byte],"volume import scalar oracle");
				}
			}
		};check_read();
		// CPU byte update must appear on the next command, including unchanged SRD.
		const uint64_t change=VolumeByteOffset(layout,base,0,0,0);logical[change]^=0x93;truth[physical_at(change)]^=0x93;
		static_cast<uint8_t*>(physical.contents)[physical_at(change)]^=0x93;
		cmd=[queue commandBuffer];Check(transfer.Load(cmd),"volume CPU refresh");encode(cmd,read,view,output);finish(cmd);check_read();
		cmd=[queue commandBuffer];Check(transfer.Load(cmd),"volume producer load");
		// New raw GPU data outside BASE..LAST after gather must survive scatter.
		std::vector<bool> owned(size,false);
		for(uint32_t m=base;m<=last;++m) {
			std::vector<FootprintRange> fp;Check(VolumeMipFootprint(layout,m,fp),"volume mip footprint");uint64_t count=0;
			for(auto range:fp){count+=range.size;for(uint64_t p=range.begin;p<range.begin+range.size;++p)owned[p]=true;}
			Check(count==uint64_t(layout.mips[m].width)*layout.mips[m].height*std::max(d>>m,1u)*eb,"volume footprint includes padding");
			// Independently cover every scalar address, including cut z slabs.
			for(uint32_t z=0;z<std::max(d>>m,1u);++z)for(uint32_t y=0;y<layout.mips[m].height;++y)for(uint32_t x=0;x<layout.mips[m].width;++x)
				for(uint32_t b=0;b<eb;++b)Check(owned[VolumeByteOffset(layout,m,x,y,z)+b],"volume footprint misses scalar byte");
		}
		uint64_t untouched=0;while(untouched<size && owned[untouched])++untouched;Check(untouched<size,"volume padding exists");
		id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder];OrderBegin(blit);[blit fillBuffer:physical range:NSMakeRange(physical_at(untouched),1) value:0xe7];OrderEnd(blit);[blit endEncoding];truth[physical_at(untouched)]=0xe7;
		encode(cmd,write,view,nil);
		for(uint32_t z=0;z<view.depth;++z)for(uint32_t y=0;y<view.height;++y)for(uint32_t x=0;x<view.width;++x)if((x+y+z)%3==0) {
			const uint32_t i=(z*view.height+y)*view.width+x,values[]{0x12345678u^i,0x87654321u^i,0x35791357u^i,0xfedcba98u^i};
			const uint64_t at=VolumeByteOffset(layout,base,x,y,z);
			for(uint32_t b=0;b<eb;++b){logical[at+b]=uint8_t(values[b/4]>>(8*(b%4)));truth[physical_at(at+b)]=logical[at+b];}
		}
		Check(transfer.Store(cmd),"volume writeback");
		// A separate reader sees the GPU-written physical bytes without CPU waits.
		VolumeMemoryTransfer reader;id<MTLTexture> other=[device newTextureWithDescriptor:td],other_raw=[other newTextureViewWithPixelFormat:raw.pixelFormat];
		Check(reader.Initialize(device,other_raw,layout,plan,base,last,memory)&&reader.Load(cmd),"volume second reader");
		id<MTLTexture> other_view=[other_raw newTextureViewWithPixelFormat:raw.pixelFormat textureType:MTLTextureType3D levels:NSMakeRange(selected,last-base+1) slices:NSMakeRange(0,1)];
		encode(cmd,read,other_view,output);finish(cmd);check_read();
		Check(!std::memcmp(physical.contents,truth.data(),truth.size()),"volume export changed padding/mips/raw updates/guards");checked+=truth.size();
		VolumeMemoryTransfer bad;GuestMipViewPlan invalid=plan;invalid.physical_levels=17;
		Check(!bad.Initialize(device,raw,layout,invalid,base,last,memory),"invalid physical levels accepted");++negative;
		Check(!bad.Initialize(device,raw,layout,plan,last+1,last,memory),"inverted volume mip range accepted");++negative;
		std::vector<FootprintRange> rejected{{1,1}};Check(!VolumeMipFootprint(layout,levels,rejected)&&rejected.empty(),"invalid volume footprint level accepted");++negative;
		++cases;
	}
	// Actual floating-point image stores: the exact packed bytes must reach
	// memory, rather than a widened RGBA32Float surrogate or numeric conversion.
	for(auto tile:{Prospero::TileMode::kStandard64KB,Prospero::TileMode::kLinear})
	for(auto fmt:{MTLPixelFormatR16Float,MTLPixelFormatRG11B10Float}) {
		const uint32_t eb=fmt==MTLPixelFormatR16Float?2:4,w=200,h=113,d=7;
		TileSurfaceLayout layout{};Check(PlanVolumeLayout({eb==2?format[1]:format[2],tile,TileSurfaceDimension::Dim3D,w,h,d,1,1},layout),"typed volume layout");
		id<MTLBuffer> bytes=[device newBufferWithLength:layout.total_size options:MTLResourceStorageModeShared];std::memset(bytes.contents,0xa5,bytes.length);
		const PhysicalSpan span{0,0,bytes.length};CanonicalMemoryView memory;
		Check(memory.Initialize(device,std::span(&span,1),bytes.length,bytes.length,[&](uint64_t p,uint64_t)->std::optional<CanonicalMemoryView::BufferSpan>{return CanonicalMemoryView::BufferSpan{bytes,p};}),"typed volume physical");
		MTLTextureDescriptor* td=[MTLTextureDescriptor new];td.textureType=MTLTextureType3D;td.pixelFormat=fmt;td.width=w;td.height=h;td.depth=d;
		td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsagePixelFormatView;
		id<MTLTexture> parent=[device newTextureWithDescriptor:td],raw=[parent newTextureViewWithPixelFormat:TileElementFormat(eb)];
		VolumeMemoryTransfer transfer;Check(transfer.Initialize(device,raw,layout,{},0,0,memory),"typed volume transfer");
		id<MTLCommandBuffer> cmd=[queue commandBuffer];Check(transfer.Load(cmd),"typed volume load");encode(cmd,typed,parent,nil);Check(transfer.Store(cmd),"typed volume store");finish(cmd);
		const uint32_t packed=eb==2?0x3c00u:(15u<<6)|((16u<<6)<<11)|((17u<<5)<<22);
		std::vector<uint8_t> expected(bytes.length,0xa5);
		for(uint32_t z=0;z<d;++z)for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)std::memcpy(expected.data()+VolumeByteOffset(layout,0,x,y,z),&packed,eb);
		Check(!std::memcmp(expected.data(),bytes.contents,bytes.length),"typed volume store packed representation");checked+=bytes.length;++typed_cases;
	}
	// A queued volume producer keeps its original physical allocation through
	// a VA remap. Reusing that VA with a different shape/format reads the new
	// allocation; remapping back exposes the first producer's packed bytes.
	{
		TileSurfaceLayout a{},b{};
		Check(TileGetTiledTextureLayout({format[2],Prospero::TileMode::kStandard64KB,TileSurfaceDimension::Dim3D,64,64,64,1,1},a),"volume old shape layout");
		Check(TileGetTiledTextureLayout({format[1],Prospero::TileMode::kStandard64KB,TileSurfaceDimension::Dim3D,200,113,7,1,1},b),"volume new shape layout");
		const uint64_t allocation=std::max(a.total_size,b.total_size),va=0x8244b10000;
		id<MTLBuffer> physical=[device newBufferWithLength:allocation*2+512 options:MTLResourceStorageModeShared];
		std::vector<uint8_t> expected(physical.length,0xa5);std::memcpy(physical.contents,expected.data(),expected.size());
		std::vector<PhysicalMapping> maps{{va,allocation,256}};
		PhysicalBackingSnapshot snapshot;Check(snapshot.Capture(maps,va,a.total_size),"volume original physical snapshot");
		const auto memory_for=[&](uint64_t size) {
			std::vector<PhysicalSpan> spans;Check(ResolvePhysicalSpans(maps,va,size,spans),"volume remap resolution");CanonicalMemoryView memory;
			Check(memory.Initialize(device,spans,size,65536,[&](uint64_t p,uint64_t n)->std::optional<CanonicalMemoryView::BufferSpan>{
				if(p>physical.length || n>physical.length-p)return std::nullopt;return CanonicalMemoryView::BufferSpan{physical,p};}),"volume remap chunks");return memory;
		};
		const auto image_for=[&](const TileSurfaceLayout& l,MTLPixelFormat format) {
			MTLTextureDescriptor* td=[MTLTextureDescriptor new];td.textureType=MTLTextureType3D;td.pixelFormat=format;
			td.width=l.description.width;td.height=l.description.height;td.depth=l.description.depth;td.storageMode=MTLStorageModePrivate;
			td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsagePixelFormatView;
			return [device newTextureWithDescriptor:td];
		};
		id<MTLTexture> old=image_for(a,MTLPixelFormatRG11B10Float),old_raw=[old newTextureViewWithPixelFormat:MTLPixelFormatR32Uint];
		VolumeMemoryTransfer producer;Check(producer.Initialize(device,old_raw,a,{},0,0,memory_for(a.total_size)),"remapped producer transfer");
		id<MTLCommandBuffer> cmd=[queue commandBuffer];Check(producer.Load(cmd),"remapped producer load");encode(cmd,typed,old,nil);
		maps[0].backing_offset=256+allocation;Check(!snapshot.Matches(maps,va),"volume remap kept old cache identity");++negative;
		Check(producer.Store(cmd),"old producer physical writeback after remap");
		const uint32_t packed=(15u<<6)|((16u<<6)<<11)|((17u<<5)<<22);
		for(uint32_t z=0;z<64;++z)for(uint32_t y=0;y<64;++y)for(uint32_t x=0;x<64;++x)
			std::memcpy(expected.data()+256+VolumeByteOffset(a,0,x,y,z),&packed,4);
		id<MTLTexture> replacement=image_for(b,MTLPixelFormatR16Float),replacement_raw=[replacement newTextureViewWithPixelFormat:MTLPixelFormatR16Uint];
		VolumeMemoryTransfer consumer;Check(consumer.Initialize(device,replacement_raw,b,{},0,0,memory_for(b.total_size)),"replacement shape/format transfer");
		id<MTLBuffer> out=[device newBufferWithLength:200*113*7*16 options:MTLResourceStorageModeShared];
		Check(consumer.Load(cmd),"replacement allocation load");encode(cmd,read,replacement_raw,out);finish(cmd);
		const auto verify=[&](uint64_t physical_base) {
			const uint32_t* values=static_cast<const uint32_t*>(out.contents);
			for(uint32_t z=0;z<7;++z)for(uint32_t y=0;y<113;++y)for(uint32_t x=0;x<200;++x) {
				uint16_t value=0;std::memcpy(&value,expected.data()+physical_base+VolumeByteOffset(b,0,x,y,z),2);
				Check(values[((z*113+y)*200+x)*4]==value,"same VA shape/format/remap read stale volume");
			}
		};verify(256+allocation);
		Check(!std::memcmp(expected.data(),physical.contents,expected.size()),"volume remap redirected producer or damaged guards");
		maps[0].backing_offset=256;Check(snapshot.Matches(maps,va),"volume remap-back physical identity");
		VolumeMemoryTransfer alias;Check(alias.Initialize(device,replacement_raw,b,{},0,0,memory_for(b.total_size)),"same physical alternate shape/format");
		cmd=[queue commandBuffer];Check(alias.Load(cmd),"packed producer -> R16 physical alias");encode(cmd,read,replacement_raw,out);finish(cmd);verify(256);
		checked+=expected.size();
		std::puts("Volume rebind: same VA 64x64x64/RG11B10 -> 200x113x7/R16, queued producer + physical remap + remap-back alias PASS");
	}
	// Same VA with changed shape/format/depth/mips/R128 needs a distinct image.
	const auto original=VolumeDescriptor(19,6,0,5,Prospero::TileMode::kStandard64KB);
	const auto key=MakeVolumeImageKey(0x8244b10000,67,35,original,false);
	for(unsigned field=0;field<8;++field){auto changed=original;changed.fields[field]^=1;Check(key!=MakeVolumeImageKey(0x8244b10000,67,35,changed,false),"volume cache ignores SRD word");++negative;}
	Check(key!=MakeVolumeImageKey(0x8244b10000,200,113,original,false)&&key!=MakeVolumeImageKey(0x8244b10000,67,35,original,true),"volume cache ignores shape/R128");negative+=2;
	// Fixed GFX10 linear-layout fixture from AddrLib's descending mip records.
	// Ceil-sized physical mips differ from floor-sized visible extents, and each
	// z slice retains the complete 27,392-byte chain at every mip.
	TileSurfaceLayout linear{};
	Check(PlanVolumeLayout({format[2],Prospero::TileMode::kLinear,TileSurfaceDimension::Dim3D,67,35,19,6,1},linear),"linear GFX10 fixture");
	const uint64_t offsets[]{9472,4864,2560,1280,512,0},sizes[]{17920,4608,2304,1280,768,512};
	Check(linear.block_slice_size==27392 && linear.total_size==520448,"linear GFX10 slice/volume size");
	for(unsigned m=0;m<6;++m)Check(linear.mips[m].offset==offsets[m] && linear.mips[m].size==sizes[m],"linear GFX10 reverse mip offsets");
	auto invalid_linear=linear.description;invalid_linear.depth=0;
	Check(!PlanVolumeLayout(invalid_linear,linear),"zero depth linear layout accepted");++negative;
	invalid_linear.depth=19;invalid_linear.layers=2;
	Check(!PlanVolumeLayout(invalid_linear,linear),"array of linear volumes accepted");++negative;
	// Depth contributes to the natural mip limit (1x1x33 has six levels).
	auto depth_descriptor=VolumeDescriptor(33,6,0,5,Prospero::TileMode::kStandard64KB);GuestMipViewPlan depth_plan;
	Check(PlanGuestMipView(depth_descriptor,1,1,false,depth_plan,33)&&depth_plan.texture_levels==6,"volume mip limit ignored depth");
	std::printf("Volume physical memory: %u GPU cases, %u native float stores, %u negative controls, %llu exact bytes; thick slabs/tails/mips/discontiguous chunks/CPU+raw GPU writes/partial stores PASS\n",cases,typed_cases,negative,(unsigned long long)checked);
}
