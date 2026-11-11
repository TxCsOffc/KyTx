// Physical backing identity must survive aliases and invalidate remapped views.
#include "graphics/host_gpu/metal/metalGuestMemoryView.h"
#include <cstdio>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

using namespace Libs::Graphics::Metal;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

int main() {
	@autoreleasepool {
		try {
			id<MTLDevice> device = MTLCreateSystemDefaultDevice(); Check(device != nil, "Metal unavailable");
			id<MTLCommandQueue> queue = [device newCommandQueue];
			const size_t page = size_t(getpagesize());
			FILE* file = tmpfile(); Check(file != nullptr, "temporary backing failed");
			Check(ftruncate(fileno(file), 2 * page) == 0, "backing size failed");
			void* backing = mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(file), 0);
			void* guest = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(file), 0);
			Check(backing != MAP_FAILED && guest != MAP_FAILED, "map failed");
			*static_cast<uint32_t*>(backing) = 0x13579bdfu;
			*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(backing) + page) = 0x2468ace0u;
			{
				id<MTLBuffer> physical = [device newBufferWithBytesNoCopy:backing length:page * 2 options:MTLResourceStorageModeShared deallocator:nil];
				id<MTLBuffer> wrapped = [device newBufferWithBytesNoCopy:guest length:page options:MTLResourceStorageModeShared deallocator:nil];
				Check(physical != nil && wrapped != nil, "buffer wrap failed");
				MetalBufferAllocation a {physical, 0, page, backing, 0};
				MetalBufferAllocation b {physical, page, page, static_cast<uint8_t*>(backing) + page, 0};
				MetalBufferAllocation old {wrapped, 0, page, guest, 0};
				const NSUInteger row = [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatR32Uint];
				auto* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:1 height:1 mipmapped:NO];
				td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
				id<MTLTexture> view_a = CreateGuestMemoryView(a, td, row);
				id<MTLTexture> old_view = CreateGuestMemoryView(old, td, row);
				Check(view_a != nil && old_view != nil, "view creation failed");
				Check(GuestMemoryViewMatches(view_a, a, td.pixelFormat, 1, 1, row), "valid allocation rejected");
				Check(!GuestMemoryViewMatches(view_a, b, td.pixelFormat, 1, 1, row), "changed physical offset accepted");
				Check(!GuestMemoryViewMatches(old_view, a, td.pixelFormat, 1, 1, row), "different buffer identity accepted");
				Check(!GuestMemoryViewMatches(view_a, a, td.pixelFormat, 1, 1, row * 2), "changed row stride accepted");
				Check(!GuestMemoryViewMatches(view_a, a, td.pixelFormat, 2, 1, row), "changed extent accepted");
				Check(!GuestMemoryViewMatches(view_a, a, MTLPixelFormatR32Float, 1, 1, row), "changed format accepted");
				auto short_range = a; short_range.size = row - 1;
				Check(CreateGuestMemoryView(short_range, td, row) == nil, "undersized allocation accepted");
				Check(!GuestMemoryViewMatches(view_a, short_range, td.pixelFormat, 1, 1, row), "cached view exceeds current mapping extent");
				NSError* error = nil;
				id<MTLLibrary> lib = [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void read0(texture2d<uint,access::read> t [[texture(0)]], device uint* o [[buffer(0)]]) {o[0]=t.read(uint2(0)).x;}\nkernel void write0(texture2d<uint,access::write> t [[texture(0)]]) {t.write(uint4(0xdeadbeefu),uint2(0));}"
				                                         options:nil error:&error];
				Check(lib != nil, "test shader failed");
				id<MTLComputePipelineState> read_pso = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"read0"] error:&error];
				id<MTLComputePipelineState> write_pso = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"write0"] error:&error];
				Check(read_pso != nil && write_pso != nil, "test pipeline failed");
				id<MTLBuffer> result = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
				auto run = [&](id<MTLTexture> view, bool write) {
					id<MTLCommandBuffer> cb = [queue commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
					[e setComputePipelineState:write ? write_pso : read_pso]; [e setTexture:view atIndex:0];
					if (!write) [e setBuffer:result offset:0 atIndex:0];
					[e dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
					[e endEncoding]; [cb commit]; [cb waitUntilCompleted];
					Check(cb.status == MTLCommandBufferStatusCompleted, "GPU execution failed");
					return *static_cast<uint32_t*>(result.contents);
				};
				Check(run(view_a, false) == 0x13579bdfu, "initial texture read failed");
				Check(mmap(guest, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fileno(file), page) == guest, "guest remap failed");
				Check(*static_cast<uint32_t*>(guest) == 0x2468ace0u, "CPU did not see remap");
				id<MTLTexture> view_b = CreateGuestMemoryView(b, td, row);
				Check(view_b != nil && run(view_b, false) == 0x2468ace0u, "new backing texture read failed");
				const uint32_t old_value = run(old_view, false);
				run(view_b, true);
				Check(*static_cast<uint32_t*>(guest) == 0xdeadbeefu, "GPU write did not reach remapped CPU alias");
				Check(run(view_a, false) == 0x13579bdfu, "new page write damaged old physical page");
				std::printf("Guest memory views: GPU remap/read/write, physical aliases, identity/offset/stride/extent/format/bounds passed; old wrapper read=%08x\n", old_value);
			}
			munmap(guest, page); munmap(backing, 2 * page); fclose(file);
		} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
	}
}
