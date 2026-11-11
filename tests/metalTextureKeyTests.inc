#include "graphics/host_gpu/metal/metalTextureKey.h"
#include <map>

static void CheckRenderTextureKeys(id<MTLDevice> device, id<MTLCommandQueue> queue) {
	using Libs::Graphics::Metal::RenderTextureKey;
	const uint64_t address=0x8100000000ull;
	const RenderTextureKey cases[]={
		{address,uint32_t(MTLPixelFormatR32Uint),64,64,6},
		{address^((64ull^128ull)<<20u),uint32_t(MTLPixelFormatR32Uint),128,64,6},
		{address^((64ull^320ull)<<8u),uint32_t(MTLPixelFormatR32Uint),64,320,6}
	};
	auto old_key=[](const RenderTextureKey& k){return k.address^(uint64_t(k.format)<<48u)^(uint64_t(k.width)<<20u)^(uint64_t(k.height)<<8u)^k.levels;};
	Check(old_key(cases[0])==old_key(cases[1]) && old_key(cases[0])==old_key(cases[2]),"legacy texture key control must collide");
	std::map<RenderTextureKey,id<MTLTexture>> cache;
	std::map<uint64_t,id<MTLTexture>> legacy;
	for(const auto& key:cases) {
		Check((key.address&0x3fffu)==0,"collision control uses real page-aligned addresses");
		auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:key.width height:key.height mipmapped:YES];
		td.mipmapLevelCount=key.levels;td.storageMode=MTLStorageModeShared;
		td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
		auto texture=[device newTextureWithDescriptor:td];Check(texture!=nil,"texture key GPU allocation");
		for(uint32_t mip=0;mip<key.levels;++mip) {
			const uint32_t width=std::max(key.width>>mip,1u),height=std::max(key.height>>mip,1u);
			std::vector<uint32_t> initial(size_t(width)*height,0x5a5a5a5a);
			[texture replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:mip withBytes:initial.data() bytesPerRow:width*4];
		}
		cache.emplace(key,texture);legacy.emplace(old_key(key),texture);
	}
	Check(cache.size()==3 && legacy.size()==1,"distinct texture allocations must not alias in cache");
	auto command=[queue commandBuffer];
	for(uint32_t i=0;i<3;++i) {
		auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture=cache.at(cases[i]);pass.colorAttachments[0].level=2;
		pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
		pass.colorAttachments[0].clearColor=MTLClearColorMake(i+1,0,0,0);
		auto encoder=[command renderCommandEncoderWithDescriptor:pass];[encoder endEncoding];
	}
	[command commit];[command waitUntilCompleted];Check(command.status==MTLCommandBufferStatusCompleted,"texture key render clear");
	uint64_t checked=0;unsigned legacy_wrong_shapes=0;
	for(uint32_t i=0;i<3;++i) {
		const auto& key=cases[i];auto texture=cache.at(key);
		auto wrong=legacy.at(old_key(key));legacy_wrong_shapes+=wrong.width!=key.width || wrong.height!=key.height;
		Check(texture.width==key.width && texture.height==key.height && texture.mipmapLevelCount==key.levels,"cache returns the requested texture shape");
		for(uint32_t mip=0;mip<key.levels;++mip) {
			const uint32_t width=std::max(key.width>>mip,1u),height=std::max(key.height>>mip,1u);
			std::vector<uint32_t> actual(size_t(width)*height);
			[texture getBytes:actual.data() bytesPerRow:width*4 fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:mip];
			for(auto value:actual)Check(value==(mip==2?i+1:0x5a5a5a5a),"texture key collision changed another allocation/mip");
			checked+=actual.size()*4;
		}
	}
	Check(legacy_wrong_shapes==2,"legacy collision control must return two wrong shapes");
	std::printf("Render texture identities: 3 GPU allocations, %llu bytes, 2 legacy wrong-shape controls, distinct mips preserved PASS\n",(unsigned long long)checked);
}
