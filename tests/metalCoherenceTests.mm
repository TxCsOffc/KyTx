#include "graphics/host_gpu/memoryCoherence.h"
#include "graphics/guest_gpu/command_processor/pm4MemorySync.h"
#include "graphics/host_gpu/metal/metalSurfaceCoherence.h"
#include "graphics/host_gpu/metal/metalTiling.h"
#include "graphics/host_gpu/metal/metalCanonicalMemory.h"
#include "graphics/host_gpu/metal/metalVolumeMemory.h"
#include "graphics/host_gpu/metal/metalOrder.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <semaphore>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

using namespace Libs::Graphics;
using namespace Libs::Graphics::Metal;
using namespace Libs::Graphics::Coherence;
static void Check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }

struct Surface {
	TileSurfaceLayout layout;
	TileTables tables;
	id<MTLTexture> texture=nil;
	id<MTLBuffer> exported=nil;
	bool exported_current=false;
	std::vector<Range> footprint;
};

#include "metalAliasPreservationTests.inc"
#include "metalMipTransferTests.inc"
#include "metalPhysicalSpanTests.inc"
#include "metalDepthAliasTests.inc"
#include "metalDepthMemoryTests.inc"
#include "metalBcTransferTests.inc"
#include "metalRectTransferTests.inc"
#include "metalVolumeMemoryTests.inc"
#include "metalReadOnlyImportTests.inc"

// Two independent producers form an overlap group. The consumer declares the
// other virtual alias, so its apparent ranges do not conflict with that group.
// A PM4 barrier must still make it wait for both producers, without a CPU wait.
static void CheckBarrier(id<MTLDevice> device,id<MTLCommandQueue> queue,id<MTLBuffer> guest,void* alias) {
	const NSUInteger page=getpagesize(),count=page/sizeof(uint32_t);
	id<MTLBuffer> result=[device newBufferWithLength:page options:MTLResourceStorageModeShared];
	Check(result!=nil,"barrier result buffer");
	NSError* error=nil;
	id<MTLLibrary> library=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void produce(device uint* p [[buffer(0)]],constant uint& v [[buffer(1)]],uint i [[thread_position_in_grid]]) {p[i]=v+17u*i;}\nkernel void consume(device const uint* a [[buffer(0)]],device const uint* b [[buffer(1)]],device uint* out [[buffer(2)]],uint i [[thread_position_in_grid]]) {out[i]=a[i]^b[i];}"
	    options:nil error:&error];
	Check(library!=nil,"barrier shader compilation");
	id<MTLComputePipelineState> produce=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"produce"] error:&error];
	id<MTLComputePipelineState> consume=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"consume"] error:&error];
	Check(produce!=nil && consume!=nil,"barrier pipelines");
	id<MTLCommandBuffer> command=[queue commandBuffer];
	OrderMemoryBarrier();
	for(unsigned i=0;i<2;++i) {
		id<MTLComputeCommandEncoder> e=[command computeCommandEncoder];
		const OrderAccess access{0x10000ull+i*page,0x10000ull+(i+1)*page,true};
		OrderBeginCompute(e,&access,1,false);
		[e setComputePipelineState:produce]; [e setBuffer:guest offset:i*page atIndex:0];
		const uint32_t value=i==0?0x12340000u:0xabcd0000u;
		[e setBytes:&value length:sizeof(value) atIndex:1];
		[e dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
		OrderEnd(e); [e endEncoding];
	}
	size_t fences=0;
	{auto& state=OrderDetail::Get(); std::lock_guard lock(state.mutex);
	 Check(state.group_open && state.group_size==2 && state.frontier.size()==2,"independent dispatches did not form a group");
	 fences=state.frontier.size();}
	OrderMemoryBarrier();
	{auto& state=OrderDetail::Get(); std::lock_guard lock(state.mutex);
	 Check(!state.group_open && state.group_size==0 && state.frontier.size()==fences,"barrier lost GPU frontier or kept overlap open");}
	id<MTLComputeCommandEncoder> e=[command computeCommandEncoder];
	const OrderAccess apparent_alias{0x10000000ull,0x10000000ull+2*page,false};
	OrderBeginCompute(e,&apparent_alias,1,false);
	{auto& state=OrderDetail::Get(); std::lock_guard lock(state.mutex);
	 Check(state.current==OrderDetail::Kind::Leader,"post-barrier consumer skipped earlier producer fences");}
	// Resolve both guest addresses to their one physical Metal backing. A second
	// no-copy wrapper of `alias` is not a coherent GPU alias even with fences.
	[e setComputePipelineState:consume]; [e setBuffer:guest offset:0 atIndex:0];
	[e setBuffer:guest offset:page atIndex:1]; [e setBuffer:result offset:0 atIndex:2];
	[e dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
	OrderEnd(e); [e endEncoding];
	[command commit]; [command waitUntilCompleted];
	Check(command.status==MTLCommandBufferStatusCompleted,"barrier GPU command failed");
	const auto* values=static_cast<const uint32_t*>(result.contents);
	for(uint32_t i=0;i<count;++i) if(values[i]!=((0x12340000u+17u*i)^(0xabcd0000u+17u*i))) {
		const auto* physical=static_cast<const uint32_t*>(guest.contents);
		const auto* second=static_cast<const uint32_t*>(alias);
		std::fprintf(stderr,"barrier i=%u actual=%08x expected=%08x physical=%08x/%08x alias=%08x/%08x\n",i,values[i],
		    (0x12340000u+17u*i)^(0xabcd0000u+17u*i),physical[i],physical[count+i],second[i],second[count+i]);
		Check(false,"barrier consumer observed stale alias bytes");
	}
}

#include "metalTextureKeyTests.inc"
#include "metalRemappedImageTests.inc"

int main() {
	@autoreleasepool {
		try {
			id<MTLDevice> device=MTLCreateSystemDefaultDevice(); Check(device!=nil,"Metal unavailable");
			id<MTLCommandQueue> queue=[device newCommandQueue];
			CheckAliasPreservation(device,queue);
			CheckMipTransfers(device,queue);
			CheckRenderTextureKeys(device,queue);
			CheckRemappedImageBacking(device,queue);
			CheckBcPhysicalTransfers(device,queue);
			CheckRectPhysicalTransfers(device,queue);
			CheckVolumeMemory(device,queue);
			CheckReadOnlyImports(device,queue);
			CheckPhysicalSpans(device,queue);
			CheckDepthAlias(device,queue);
			CheckDepthMemory(device,queue);
			constexpr uint32_t w=17,h=13;
			constexpr NSUInteger prefix=256,guest_size=131072;
			FILE* file=tmpfile(); Check(file!=nullptr && ftruncate(fileno(file),guest_size)==0,"shared backing file");
			void* canonical=mmap(nullptr,guest_size,PROT_READ|PROT_WRITE,MAP_SHARED,fileno(file),0);
			void* cpu_alias=mmap(nullptr,guest_size,PROT_READ|PROT_WRITE,MAP_SHARED,fileno(file),0);
			Check(canonical!=MAP_FAILED && cpu_alias!=MAP_FAILED && canonical!=cpu_alias,"physical aliases");
			{
				id<MTLBuffer> guest=[device newBufferWithBytesNoCopy:canonical length:guest_size options:MTLResourceStorageModeShared deallocator:nil];
				Check(guest!=nil,"wrap canonical backing");
				CheckBarrier(device,queue,guest,cpu_alias);
				std::vector<uint8_t> truth(guest_size);
				for(size_t i=0;i<truth.size();++i) truth[i]=uint8_t(i*73+i/13+17);
				std::memcpy(cpu_alias,truth.data(),truth.size());
				// The same adapter the renderer uses: ownership tracking + tile import/export + guest write-back copies.
				SurfaceCoherence coh(device,[&](Range r) -> std::optional<SurfaceCoherence::GuestSpan> {
					if(r.backing!=1 || r.begin>guest_size || r.size>guest_size-r.begin) return std::nullopt;
					return SurfaceCoherence::GuestSpan{guest,r.begin};
				});
				Check(coh.RegisterBacking(1,guest_size),"register physical backing");
				std::map<OwnerId,Surface> views;
				auto offset=[](const Surface& s,uint32_t x,uint32_t y) {
					const auto& b=s.layout.texture.block;
					uint32_t within=0,bank=0;
					Check(TileGetBlockOffset(b,x%b.block_width,y%b.block_height,0,within),"reference within");
					Check(TileGetBlockXor(b,x/b.block_width,y/b.block_height,0,bank),"reference bank");
					return uint64_t(y/b.block_height*s.tables.columns+x/b.block_width)*b.block_size+(within^bank);
				};
				for(OwnerId id:{1u,2u}) {
					auto& s=views[id];
					const TileSurfaceDescription desc{Prospero::BufferFormat::k32UInt,id==1?Prospero::TileMode::kStandard4KB:Prospero::TileMode::kRenderTarget,
					    TileSurfaceDimension::Dim2D,w,h,1,1,1};
					Check(TileGetTiledTextureLayout(desc,s.layout),"layout");
					Check(BuildTileTables(s.layout.texture.block,s.layout.mips[0].padded_width,s.layout.mips[0].padded_height,s.tables),"tables");
					auto* td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:w height:h mipmapped:NO];
					td.storageMode=MTLStorageModeShared; td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
					s.texture=[device newTextureWithDescriptor:td];
					Check(s.texture!=nil,"surface allocation");
					// Per-texel oracle footprint, computed with the reference functions, must equal the adapter's.
					std::vector<uint64_t> addresses;
					for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) addresses.push_back(prefix+offset(s,x,y));
					std::sort(addresses.begin(),addresses.end());
					for(auto address:addresses) {
						if(!s.footprint.empty() && s.footprint.back().begin+s.footprint.back().size==address) s.footprint.back().size+=4;
						else s.footprint.push_back({1,address,4});
					}
					Check(coh.RegisterSurface(id,1,prefix,s.texture,s.tables,w,h),"register surface");
					Check(coh.Footprint(id)==s.footprint,"adapter footprint differs from the per-texel oracle");
				}
				id<MTLCommandBuffer> command=[queue commandBuffer];
				auto read_texture=[&](OwnerId id) { Check(coh.ReadSurface(id,command),"read surface"); };
				auto finish=[&] {
					[command commit]; [command waitUntilCompleted];
					Check(command.status==MTLCommandBufferStatusCompleted,"GPU command failed");
					command=[queue commandBuffer];
				};
				auto compare_texture=[&](OwnerId id) {
					auto& s=views.at(id);
					std::vector<uint8_t> actual(w*h*4),expected;
					uint32_t pitch=0;
					Check(DecodeTiledTextureLevel(std::span<const uint8_t>(truth.data()+prefix,s.layout.total_size),s.layout,0,0,expected,pitch),"reference decode");
					[s.texture getBytes:actual.data() bytesPerRow:w*4 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
					Check(actual==expected,"texture read observed stale/incorrect guest bytes");
				};
				read_texture(1); finish(); compare_texture(1); // CPU -> tiled GPU texture.
				{ // Nothing changed: a second read must not encode a single GPU command.
					id<MTLCommandBuffer> idle=[queue commandBuffer];
					Check(coh.ReadSurface(1,idle),"idle read");
					[idle commit]; [idle waitUntilCompleted];
					Check(idle.status==MTLCommandBufferStatusCompleted,"idle command buffer");
					Check(idle.GPUEndTime<=idle.GPUStartTime || idle.GPUEndTime-idle.GPUStartTime<5e-4,"current surface was imported again");
				}
				NSError* error=nil;
				id<MTLLibrary> library=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void write0(texture2d<uint,access::read_write> t [[texture(0)]], uint2 p [[thread_position_in_grid]]) {if(p.x<t.get_width() && p.y<t.get_height() && ((p.x+p.y)%3u)==0) t.write(uint4(0xa5000000u+p.y*t.get_width()+p.x),p);}"
				    options:nil error:&error];
				Check(library!=nil,"writer shader compilation");
				id<MTLComputePipelineState> pso=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"write0"] error:&error];
				Check(pso!=nil,"writer pipeline");
				id<MTLComputeCommandEncoder> e=[command computeCommandEncoder]; OrderBegin(e);
				[e setComputePipelineState:pso]; [e setTexture:views.at(1).texture atIndex:0];
				[e dispatchThreads:MTLSizeMake(w,h,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
				OrderEnd(e); [e endEncoding];
				Check(coh.WriteSurface(1,WriteMode::PreserveUnwritten),"record partial GPU write");
				for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) if((x+y)%3==0) {
					const uint32_t value=0xa5000000u+y*w+x;
					std::memcpy(truth.data()+prefix+offset(views.at(1),x,y),&value,4);
				}
				Check(!coh.ForgetSurface(1),"only current GPU image was evicted");
				OrderMemoryBarrier();
				read_texture(2); // GPU texture -> differently tiled texture, no CPU wait.
				Check(coh.MaterializeRange({1,0,guest_size},command),"whole backing writeback");
				id<MTLBuffer> readback=[device newBufferWithLength:guest_size options:MTLResourceStorageModeShared];
				Check(readback!=nil,"readback allocation");
				id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder]; OrderBegin(blit);
				[blit copyFromBuffer:guest sourceOffset:0 toBuffer:readback destinationOffset:0 size:guest_size];
				OrderEnd(blit); [blit endEncoding];
				std::atomic<uint64_t> label{0}; std::binary_semaphore complete{0};
				auto* label_ptr=&label; auto* complete_ptr=&complete;
				[command addCompletedHandler:^(id<MTLCommandBuffer> cb) {
					if(cb.status==MTLCommandBufferStatusCompleted) label_ptr->store(0x123456789abcdef0ull,std::memory_order_release);
					complete_ptr->release();
				}];
				Check(label.load()==0,"release label visible before submission");
				[command commit];
				const bool completed=complete.try_acquire_for(std::chrono::seconds(5));
				if(!completed) [command waitUntilCompleted]; // Keep callback targets alive on failure too.
				Check(completed,"completion timeout");
				Check(label.load(std::memory_order_acquire)==0x123456789abcdef0ull,"completion label not published");
				Check(std::memcmp(readback.contents,truth.data(),guest_size)==0,"GPU->CPU data or untouched padding changed");
				compare_texture(2);
				command=[queue commandBuffer];
				// Update only two bytes through a second virtual alias. A later
				// export of the old image must preserve this newer CPU subrange.
				const uint64_t changed=prefix+offset(views.at(1),8,6)+1;
				const uint8_t replacement[]{0x31,0xf2};
				std::memcpy(static_cast<uint8_t*>(cpu_alias)+changed,replacement,2);
				std::memcpy(truth.data()+changed,replacement,2);
				Check(coh.RecordGuestWrite({1,changed,2},WriteMode::OverwriteRange),"record CPU alias update");
				// Resolve a real acquire's virtual range to the same physical ID.
				const uint64_t va=uint64_t(cpu_alias)+(changed&~255ull);
				uint32_t payload[]{0,1,0,uint32_t(va>>8),uint32_t(va>>40),0,(1u<<8)|(1u<<14)};
				auto acquire=Pm4::DecodeAcquireMem(0xc0065800u,payload);
				Check(acquire && acquire->range.size==256,"alias acquire decode");
				const Range physical{1,acquire->range.base-uint64_t(cpu_alias),acquire->range.size};
				Check(coh.MaterializeRange(physical,command),"acquire physical range");
				read_texture(1); read_texture(2);
				Check(coh.MaterializeRange({1,0,guest_size},command),"final guest materialization");
				finish(); compare_texture(1); compare_texture(2);
				Check(std::memcmp(guest.contents,truth.data(),guest_size)==0,"alias CPU write/padding was overwritten");
				Check(coh.ForgetSurface(1) && coh.ForgetSurface(2),"materialized images cannot be retired");
				std::puts("Metal coherence: CPU->GPU, tiled GPU->GPU, GPU->CPU release label, physical aliases, partial writes and padding passed; no game");
			}
			munmap(cpu_alias,guest_size); munmap(canonical,guest_size); fclose(file);
		} catch(const std::exception& ex) { std::fprintf(stderr,"%s\n",ex.what()); return 1; }
	}
}
