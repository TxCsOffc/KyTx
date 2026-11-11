// Synthetic data only. Exercise real KyTx tiling kernels and Metal remapping.
#include "graphics/host_gpu/metal/metalTiling.h"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

using namespace Libs::Graphics;
using namespace Libs::Graphics::Metal;

static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static double Median(std::vector<double> values) {
	std::sort(values.begin(), values.end()); return values[values.size() / 2];
}
static double Finish(id<MTLCommandBuffer> cb) {
	[cb commit]; [cb waitUntilCompleted];
	Check(cb.status == MTLCommandBufferStatusCompleted, "GPU command failed");
	return (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
}

static void ConversionCost(id<MTLDevice> dev, id<MTLCommandQueue> queue,
                           TileBlockFamily family, uint32_t eb, const char* name) {
	const uint32_t w = 1920, h = 1080;
	TileBlockLayout block {}; Check(TileGetBlockLayout(family, eb, block), "block layout failed");
	const uint32_t pitch = ((w + block.block_width - 1) / block.block_width) * block.block_width;
	TileTables tables; Check(BuildTileTables(block, pitch, h, tables), "tile tables failed");
	const size_t bytes = size_t(tables.columns) * tables.rows * block.block_size;
	id<MTLBuffer> guest = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
	id<MTLBuffer> roundtrip = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
	Check(guest != nil && roundtrip != nil, "buffer allocation failed");
	for (size_t i = 0; i < bytes / 4; ++i) static_cast<uint32_t*>(guest.contents)[i] = uint32_t(i * 2654435761u);
	auto* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:TileElementFormat(eb) width:w height:h mipmapped:NO];
	td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite; td.storageMode = MTLStorageModeShared;
	id<MTLTexture> texture = [dev newTextureWithDescriptor:td]; Check(texture != nil, "texture allocation failed");
	std::vector<double> import_ms, export_ms, copy_ms;
	for (unsigned run = 0; run < 9; ++run) {
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		Check(EncodeTileImport(dev, cb, guest, 0, texture, tables, w, h), "tile import encode failed");
		const double a = Finish(cb);
		cb = [queue commandBuffer];
		Check(EncodeTileExport(dev, cb, texture, roundtrip, 0, tables, w, h), "tile export encode failed");
		const double b = Finish(cb);
		cb = [queue commandBuffer]; id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromBuffer:guest sourceOffset:0 toBuffer:roundtrip destinationOffset:0 size:bytes];
		[blit endEncoding]; const double c = Finish(cb);
		if (run >= 2) { import_ms.push_back(a); export_ms.push_back(b); copy_ms.push_back(c); }
	}
	// A distinct export after the copy baseline, so the control cannot hide errors.
	std::memset(roundtrip.contents, 0xa5, bytes);
	id<MTLCommandBuffer> verify = [queue commandBuffer];
	Check(EncodeTileExport(dev, verify, texture, roundtrip, 0, tables, w, h), "verification encode failed");
	Finish(verify);
	std::vector<uint8_t> linear(size_t(w) * h * eb);
	[texture getBytes:linear.data() bytesPerRow:w * eb fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
	for (uint32_t i = 0; i < 257; ++i) {
		const uint32_t x = (i * 977u) % w, y = (i * 541u) % h;
		uint32_t within = 0, bank = 0;
		Check(TileGetBlockOffset(block, x % block.block_width, y % block.block_height, 0, within), "reference offset failed");
		Check(TileGetBlockXor(block, x / block.block_width, y / block.block_height, 0, bank), "reference xor failed");
		const size_t offset = size_t(y / block.block_height * tables.columns + x / block.block_width) * block.block_size + (within ^ bank);
		Check(std::memcmp(static_cast<const uint8_t*>(guest.contents) + offset, linear.data() + (size_t(y) * w + x) * eb, eb) == 0, "import mismatch");
		Check(std::memcmp(static_cast<const uint8_t*>(guest.contents) + offset, static_cast<const uint8_t*>(roundtrip.contents) + offset, eb) == 0, "export mismatch");
	}
	std::printf("%s eb=%u 1920x1080 padded=%zu: import=%.4f ms export=%.4f ms copy=%.4f ms; 257 samples exact\n",
	            name, eb, bytes, Median(import_ms), Median(export_ms), Median(copy_ms));
}

static void RemapProbe(id<MTLDevice> dev, id<MTLCommandQueue> queue) {
	const size_t page = size_t(getpagesize());
	FILE* file = tmpfile(); Check(file != nullptr, "temporary backing failed");
	const int fd = fileno(file); Check(ftruncate(fd, page * 2) == 0, "backing size failed");
	void* backing = mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	void* guest = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	Check(backing != MAP_FAILED && guest != MAP_FAILED, "backing map failed");
	static_cast<uint32_t*>(backing)[0] = 0x13579bdfu;
	*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(backing) + page) = 0x2468ace0u;
	{
		id<MTLBuffer> old = [dev newBufferWithBytesNoCopy:guest length:page options:MTLResourceStorageModeShared deallocator:nil];
		id<MTLBuffer> physical = [dev newBufferWithBytesNoCopy:backing length:page * 2 options:MTLResourceStorageModeShared deallocator:nil];
		id<MTLBuffer> output = [dev newBufferWithLength:page options:MTLResourceStorageModeShared];
		Check(old != nil && physical != nil && output != nil, "remap buffer wrap failed");
		auto read = [&](id<MTLBuffer> buffer, NSUInteger offset) {
			id<MTLCommandBuffer> cb = [queue commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
			[e copyFromBuffer:buffer sourceOffset:offset toBuffer:output destinationOffset:0 size:4]; [e endEncoding]; Finish(cb);
			return *static_cast<uint32_t*>(output.contents);
		};
		Check(read(old, 0) == 0x13579bdfu, "initial alias read failed");
		Check(mmap(guest, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, page) == guest, "guest remap failed");
		const uint32_t cpu = *static_cast<uint32_t*>(guest), cached = read(old, 0), canonical = read(physical, page);
		std::printf("remap: CPU=%08x cached-wrapper=%08x canonical-backing=%08x (old=%08x new=%08x)\n",
		            cpu, cached, canonical, 0x13579bdfu, 0x2468ace0u);
		Check(cpu == 0x2468ace0u && canonical == cpu, "canonical backing did not follow remap");
	}
	munmap(guest, page); munmap(backing, page * 2); fclose(file);
}

int main() {
	@autoreleasepool {
		try {
			id<MTLDevice> device = MTLCreateSystemDefaultDevice(); Check(device != nil, "Metal unavailable");
			id<MTLCommandQueue> queue = [device newCommandQueue];
			std::printf("Device: %s; synthetic data; GPU medians of 7 after 2 warmups; no emulator\n", device.name.UTF8String);
			RemapProbe(device, queue);
			ConversionCost(device, queue, TileBlockFamily::Standard64KB, 4, "Std64KB");
			ConversionCost(device, queue, TileBlockFamily::RenderTarget64KB, 4, "RT64KB");
			ConversionCost(device, queue, TileBlockFamily::RenderTarget64KB, 8, "RT64KB");
		} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
	}
}
