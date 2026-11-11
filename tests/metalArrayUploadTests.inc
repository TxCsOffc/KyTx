// Distinct contents in every mip/layer catch the former uninitialized array.
static void ArrayUploadTests(id<MTLDevice> device) {
	unsigned       arrays = 0, subresources = 0, negatives = 0;
	NSError*       error = nil;
	id<MTLLibrary> library =
	    [device newLibraryWithSource:
	                @"#include <metal_stdlib>\nusing namespace metal;kernel void "
					@"read_array(texture2d_array<uint,access::read> tex [[texture(0)]],device "
					@"uint4* out [[buffer(0)]],constant uint4& p [[buffer(1)]],uint3 id "
					@"[[thread_position_in_grid]]){if(id.x<p.x&&id.y<p.y&&id.z<p.z)out[(id.z*p.y+"
					@"id.y)*p.x+id.x]=tex.read(id.xy,id.z,p.w);}"
	                         options:nil
	                           error:&error];
	Check(library != nil, "array GPU library", "array-upload", 0);
	if (!library) return;
	id<MTLComputePipelineState> pipeline =
	    [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"read_array"]
	                                          error:&error];
	Check(pipeline != nil, "array GPU pipeline", "array-upload", 0);
	if (!pipeline) return;
	id<MTLCommandQueue> queue = [device newCommandQueue];
	for (auto tile: {Prospero::TileMode::kStandard4KB, Prospero::TileMode::kStandard64KB,
	                 Prospero::TileMode::kRenderTarget}) {
		for (auto format: {Prospero::BufferFormat::k16Float, Prospero::BufferFormat::k8_8_8_8UNorm,
		                   Prospero::BufferFormat::k16_16_16_16Float}) {
			for (uint32_t layers: {3u, 6u, 64u}) {
				const uint32_t    width = layers == 64 ? 128 : 37, height = layers == 64 ? 128 : 23,
				                  levels = layers == 64 ? 8 : 5;
				TileSurfaceLayout layout {};
				Check(TileGetTiledTextureLayout({format, tile, TileSurfaceDimension::Dim2D, width,
				                                 height, 1, levels, layers},
				                                layout),
				      "array layout", "array-upload", layers);
				if (layout.total_size == 0) continue;
				const auto&                       block = layout.texture.block;
				const uint32_t                    eb    = block.bytes_per_element;
				std::vector<uint8_t>              source(layout.total_size, 0xa5);
				std::vector<std::vector<uint8_t>> expected(size_t(layers) * levels);
				for (uint32_t layer = 0; layer < layers; ++layer)
					for (uint32_t level = 0; level < levels; ++level) {
						const auto& mip   = layout.mips[level];
						auto&       bytes = expected[size_t(layer) * levels + level];
						bytes.resize(size_t(mip.width) * mip.height * eb);
						const bool tail = level >= layout.first_tail_level;
						for (uint32_t y = 0; y < mip.height; ++y)
							for (uint32_t x = 0; x < mip.width; ++x) {
								uint32_t       within = 0, bank = 0;
								const uint32_t bx = tail ? 0 : x / block.block_width,
								               by = tail ? 0 : y / block.block_height;
								Check(TileGetBlockOffset(
								          block, tail ? x + mip.tail_x : x % block.block_width,
								          tail ? y + mip.tail_y : y % block.block_height, 0,
								          within),
								      "array reference address", "array-upload", eb, layer);
								Check(TileGetBlockXor(block, bx, by, layer, bank),
								      "array reference bank", "array-upload", eb, layer);
								const uint64_t off =
								    uint64_t(layer) * layout.block_slice_size + mip.offset +
								    uint64_t(by * (mip.padded_width / block.block_width) + bx) *
								        block.block_size +
								    (within ^ bank);
								for (uint32_t byte = 0; byte < eb; ++byte) {
									const uint8_t value = uint8_t(x * 31 + y * 47 + byte * 13 +
									                              level * 23 + layer * 71);
									bytes[(size_t(y) * mip.width + x) * eb + byte] = value;
									if (off + byte < source.size())
										source[off + byte] = value;
									else
										Check(false, "array reference bounds", "array-upload", eb,
										      layer);
								}
							}
					}
				const auto            pixel = eb == 2   ? MTLPixelFormatR16Float
				                              : eb == 4 ? MTLPixelFormatRGBA8Unorm
				                                        : MTLPixelFormatRGBA16Float;
				MTLTextureDescriptor* td    = [MTLTextureDescriptor new];
				td.textureType              = MTLTextureType2DArray;
				td.pixelFormat              = pixel;
				td.width                    = width;
				td.height                   = height;
				td.arrayLength              = layers;
				td.mipmapLevelCount         = levels;
				td.storageMode              = MTLStorageModeShared;
				td.usage              = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
				id<MTLTexture> target = [device newTextureWithDescriptor:td];
				Check(target != nil, "array texture", "array-upload", eb, layers);
				if (!target) continue;
				id<MTLTexture> raw = [target newTextureViewWithPixelFormat:TileElementFormat(eb)];
				Check(raw != nil, "array raw view", "array-upload", eb, layers);
				const bool skip = std::getenv("KYTY_ARRAY_TEST_NO_UPLOAD") != nullptr;
				if (!skip)
					Check(UploadTiledArrayMipChain(source, layout, raw),
					      "upload actual guest array", "array-upload", eb, layers);
				// A bad final byte must not overwrite even the first valid slice/mip.
				auto truncated = source;
				for (auto& byte: truncated)
					byte ^= 0xff;
				truncated.pop_back();
				Check(!UploadTiledArrayMipChain(truncated, layout, raw),
				      "reject truncated array atomically", "array-upload", eb, layers);
				++negatives;
				std::vector<std::vector<uint32_t>> gpu(levels);
				for (uint32_t level = 0; level < levels; ++level) {
					const auto&    mip      = layout.mips[level];
					const uint32_t params[] = {mip.width, mip.height, layers, level};
					const size_t   words    = size_t(mip.width) * mip.height * layers * 4;
					id<MTLBuffer>  output =
					    [device newBufferWithLength:words * 4 options:MTLResourceStorageModeShared];
					id<MTLCommandBuffer>         command = [queue commandBuffer];
					id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
					[encoder setComputePipelineState:pipeline];
					[encoder setTexture:raw atIndex:0];
					[encoder setBuffer:output offset:0 atIndex:0];
					[encoder setBytes:params length:sizeof(params) atIndex:1];
					[encoder dispatchThreads:MTLSizeMake(mip.width, mip.height, layers)
					    threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
					[encoder endEncoding];
					[command commit];
					[command waitUntilCompleted];
					Check(command.status == MTLCommandBufferStatusCompleted,
					      "array GPU read execution", "array-upload", eb, level);
					gpu[level].resize(words);
					std::memcpy(gpu[level].data(), output.contents, words * 4);
				}
				for (uint32_t layer = 0; layer < layers; ++layer)
					for (uint32_t level = 0; level < levels; ++level) {
						const auto&          mip    = layout.mips[level];
						const auto&          wanted = expected[size_t(layer) * levels + level];
						std::vector<uint8_t> actual(wanted.size());
						[target getBytes:actual.data()
						      bytesPerRow:mip.width * eb
						    bytesPerImage:wanted.size()
						       fromRegion:MTLRegionMake2D(0, 0, mip.width, mip.height)
						      mipmapLevel:level
						            slice:layer];
						bool         gpuMatches = true;
						const size_t count      = size_t(mip.width) * mip.height;
						for (size_t pixelIndex = 0; pixelIndex < count; ++pixelIndex)
							gpuMatches &= std::memcmp(gpu[level].data() +
							                              (size_t(layer) * count + pixelIndex) * 4,
							                          wanted.data() + pixelIndex * eb, eb) == 0;
						Check(actual == wanted && gpuMatches,
						      "all guest array layers/mips preserved", "array-upload", eb, layer);
						++subresources;
					}
				++arrays;
			}
		}
	}
	std::printf(
	    "guest array upload: %u arrays, %u GPU subresource readbacks, %u atomic negatives\n",
	    arrays, subresources, negatives);
}
