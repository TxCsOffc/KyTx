// Comparison sampling against independent CPU footprints AND Metal depth textures.
#include <map>
#include <limits>

static bool ModelCompare(uint32_t function, float texel, float reference) {
	switch (function) {
		case 0: return false;
		case 1: return reference < texel;
		case 2: return reference == texel;
		case 3: return reference <= texel;
		case 4: return reference > texel;
		case 5: return reference != texel;
		case 6: return reference >= texel;
		default: return true;
	}
}

struct ComparisonCase {
	uint32_t function = 1, clamp = 2, border = 0, mip = 0;
	bool min_linear = true, mag_linear = true, pixel = false;
	float u = .5f, v = .5f, z = .5f, reference = .425f, lod = 0;
	uint32_t slice = 0;
	uint32_t min_lod = 0, max_lod = 768;
	int bias = 0;
	unsigned aniso_ratio = 0;
	float gradient_aspect = 1.f;
	unsigned reduction = 0;
	std::array<int, 3> offset {};
};

struct ComparisonHarness {
	using Dimension = ShaderRecompiler::Decoder::ImageDimension;
	unsigned spatial;
	bool array, gather, has_offset, derivative, compare;
	id<MTLTexture> texture, native_depth;
	id<MTLComputePipelineState> pipeline, oracle_pipeline;
	id<MTLFunction> function;
	id<MTLArgumentEncoder> arguments;
	id<MTLBuffer> argument_buffer, output, input;
	id<MTLCommandQueue> queue;
	std::vector<std::vector<float>> levels;
	unsigned native_cases = 0;
#if KYTX_COMPARE_SPIRV_PATH
	Metal::SpvcAbiLayout reference_abi;
	id<MTLComputePipelineState> reference_pipeline;
	id<MTLArgumentEncoder> reference_arguments;
	id<MTLBuffer> reference_ab, reference_output, reference_sizes;
	id<MTLTexture> reference_texture;
	unsigned reference_cases=0;
#endif

	ComparisonHarness(Dimension dimension, bool gather_, bool offset_, bool derivative_ = false,
	                  MTLPixelFormat source_format = MTLPixelFormatR32Float, bool compare_ = true, uint32_t baked_offset = UINT32_MAX) :
	    spatial(dimension == Dimension::Dim1D || dimension == Dimension::Dim1DArray ? 1u : dimension == Dimension::Dim3D ? 3u : 2u),
	    array(dimension == Dimension::Dim1DArray || dimension == Dimension::Dim2DArray), gather(gather_), has_offset(offset_), derivative(derivative_), compare(compare_) {
		MTLTextureDescriptor* descriptor = [MTLTextureDescriptor new];
		descriptor.textureType = spatial == 1 ? (array ? MTLTextureType1DArray : MTLTextureType1D) :
		                        spatial == 3 ? MTLTextureType3D : array ? MTLTextureType2DArray : MTLTextureType2D;
		descriptor.pixelFormat = source_format;
		descriptor.width = 4; descriptor.height = spatial > 1 ? 4 : 1; descriptor.depth = spatial == 3 ? 4 : 1;
		descriptor.arrayLength = array ? 2 : 1; descriptor.mipmapLevelCount = spatial == 1 ? 1 : 3;
		descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageShaderRead;
		texture = [device newTextureWithDescriptor:descriptor]; Check(texture != nil, "comparison source texture failed");
		queue = [device newCommandQueue];
		if (spatial == 2 && compare) {
			descriptor.pixelFormat = MTLPixelFormatDepth32Float; descriptor.storageMode = MTLStorageModePrivate;
			descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
			native_depth = [device newTextureWithDescriptor:descriptor]; Check(native_depth != nil, "native depth oracle texture failed");
		}
		id<MTLCommandBuffer> upload = [queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [upload blitCommandEncoder];
		for (unsigned mip = 0; mip < texture.mipmapLevelCount; ++mip) {
			const unsigned w = std::max(4u >> mip, 1u), h = spatial > 1 ? w : 1, d = spatial == 3 ? w : 1;
			levels.emplace_back(w * h * d * descriptor.arrayLength);
			for (unsigned layer = 0; layer < descriptor.arrayLength; ++layer) {
				float* data = levels.back().data() + layer * w * h * d;
				for (unsigned i = 0; i < w * h * d; ++i) data[i] = float((i * 13 + mip * 7 + layer * 3) % 31 + 1) / 32.f;
				const unsigned channels = source_format == MTLPixelFormatRG32Float ? 2u :
				    source_format == MTLPixelFormatRGBA16Float || source_format == MTLPixelFormatRGBA32Float ? 4u : 1u;
				const bool half = source_format == MTLPixelFormatR16Float || source_format == MTLPixelFormatRGBA16Float;
				const bool unorm = source_format == MTLPixelFormatR16Unorm;
				const unsigned bytes = half || unorm ? 2u : 4u;
				std::vector<uint8_t> packed(w * h * d * channels * bytes);
				for (unsigned i = 0; i < w * h * d; ++i) for (unsigned channel = 0; channel < channels; ++channel) {
					const float value = channel == 0 ? data[i] : .125f * channel;
					auto* destination = packed.data() + (i * channels + channel) * bytes;
					if (unorm) {
						const uint16_t encoded = uint16_t(std::round(value * 65535.f));
						std::memcpy(destination,&encoded,2); if (channel == 0) data[i] = encoded / 65535.f;
					} else if (half) {
						const _Float16 encoded = _Float16(value);
						std::memcpy(destination,&encoded,2); if (channel == 0) data[i] = float(encoded);
					} else std::memcpy(destination,&value,4);
				}
				[texture replaceRegion:MTLRegionMake3D(0,0,0,w,h,d) mipmapLevel:mip slice:layer withBytes:packed.data()
				          bytesPerRow:w * channels * bytes bytesPerImage:w * h * channels * bytes];
				if (native_depth) {
					id<MTLBuffer> staging = [device newBufferWithLength:256 * h options:MTLResourceStorageModeShared];
					for (unsigned y = 0; y < h; ++y) std::memcpy(static_cast<uint8_t*>(staging.contents) + 256 * y, data + w * y, w * 4);
					[blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:256 sourceBytesPerImage:256 * h sourceSize:MTLSizeMake(w,h,1)
					          toTexture:native_depth destinationSlice:layer destinationLevel:mip destinationOrigin:MTLOriginMake(0,0,0)];
				}
			}
		}
		[blit endEncoding]; [upload commit]; [upload waitUntilCompleted];
		Check(upload.status == MTLCommandBufferStatusCompleted, "depth oracle upload failed");

		Fixture f(2);
		f.program.info.buffers[0].written=true;
		IR::ImageResource image {};
		image.resource_class = IR::ImageResourceClass::Sampled; image.numeric_class = Prospero::TextureNumericClass::Float;
		image.dimension = dimension; image.mip_mode = IR::ImageMipMode::Dynamic; image.mip_count = texture.mipmapLevelCount;
		image.read = true; image.depth_compare = compare;
		f.program.info.images.push_back(image); f.program.info.samplers.emplace_back(); f.program.info.samplers[0].depth_compare = compare;
		f.program.bindings.descriptors.push_back({*IR::DescriptorBindingForImage(image), {0}});
		f.program.bindings.descriptors.push_back({IR::DescriptorBindingKind::Samplers, {0}});
		const auto target = f.Buffer(0), params = f.Emit(0, ValueOpcode::GetBufferResource, {Value(1u),Value(0u),Value(0u),Value(0u)},1u);
		const auto read = f.Memory(1), write = f.Memory(0);
		f.program.memory_info[write].data_dwords=4;
		const auto tex = f.Emit(0, ValueOpcode::GetImageResource, {Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u)});
		const auto sam = f.Emit(0, ValueOpcode::GetSamplerResource, {Value(0u),Value(0u),Value(0u),Value(0u)});
		std::array<Value,13> values;
		for (unsigned i = 0; i < values.size(); ++i)
			values[i] = f.Emit(0, ValueOpcode::LoadBufferU32, {params,Value(0u),Value(i * 4u),Value(0u),Value(true)}, read);
#if KYTX_COMPARE_SPIRV_PATH
		// Vulkan before maintenance8 permits sample offsets only as constants.
		// Both emitters receive this same IR; the normal suite also covers dynamic offsets.
		if (has_offset && !gather && std::getenv("KYTX_REFERENCE_CONSTANT_OFFSETS"))
			values[0] = Value(63u | (1u << 8u) | ((spatial == 3 ? 63u : 0u) << 16u));
#endif
		if (has_offset && baked_offset!=UINT32_MAX) values[0]=Value(baked_offset);
		const auto address = f.Emit(0, ValueOpcode::MakeImageAddress, {values[0],values[1],values[2],values[3],values[4],values[5],values[6],values[7],values[8],values[9],values[10],values[11],values[12]});
		const auto index = f.program.memory_info.size();
		IR::MemoryInfo memory {};
		memory.kind = IR::ResourceKind::Image; memory.dmask = 15; memory.image_dimension = dimension;
		memory.image_sample_flags = (compare ? ShaderRecompiler::Decoder::ImageSampleFlagCompare : 0) |
		    (gather ? 0 : derivative ? ShaderRecompiler::Decoder::ImageSampleFlagDerivative : ShaderRecompiler::Decoder::ImageSampleFlagLod) | (has_offset ? ShaderRecompiler::Decoder::ImageSampleFlagOffset : 0);
		memory.image_address_components = unsigned(compare) + spatial + unsigned(array) + unsigned(!gather && !derivative) + unsigned(has_offset) + (derivative ? 2 * spatial : 0);
		f.program.memory_info.push_back(memory);
		const auto result = gather ? f.Emit(0, ValueOpcode::ImageGatherRaw, {tex,sam,address,Value(0u),Value(0u),Value(0u),Value(0u)},index) :
		    f.Emit(0, ValueOpcode::ImageSampleRaw, {tex,sam,address}, index);
		f.Emit(0, ValueOpcode::StoreBufferU32x4, {target,Value(0u),Value(0u),Value(0u),result,Value(true)}, write);
		function = Compile(f);
#if KYTX_COMPARE_SPIRV_PATH
		bool reference_supported=true;
		if (baked_offset!=UINT32_MAX) for (unsigned axis=0;axis<spatial;++axis) {
			const int value=int(((baked_offset>>(axis*8u))&63u)^32u)-32;
			reference_supported &= value>=-8 && value<=7;
		}
		if ((native_depth || !compare) && reference_supported) {
			id<MTLFunction> ref=CompileVulkanShaderPath(f,reference_abi);
			NSError* ref_error=nil;reference_pipeline=[device newComputePipelineStateWithFunction:ref error:&ref_error];
			Check(reference_pipeline!=nil,"reference path pipeline failed");
			reference_arguments=[ref newArgumentEncoderWithBufferIndex:0];
			reference_ab=[device newBufferWithLength:reference_arguments.encodedLength options:MTLResourceStorageModeShared];
			reference_output=[device newBufferWithLength:64 options:MTLResourceStorageModeShared];
			reference_sizes=[device newBufferWithLength:256*4 options:MTLResourceStorageModeShared];
			std::fill_n(static_cast<uint32_t*>(reference_sizes.contents),256,64u);
			[reference_arguments setArgumentBuffer:reference_ab offset:0];
			[reference_arguments setBuffer:reference_sizes offset:0 atIndex:0];
			const auto buffers=reference_abi.BaseOf(IR::DescriptorBindingKind::Buffers);
			[reference_arguments setBuffer:reference_output offset:0 atIndex:buffers];
			
		}
#endif
		NSError* error = nil; pipeline = [device newComputePipelineStateWithFunction:function error:&error];
		Check(pipeline != nil, "comparison pipeline failed");
		arguments = [function newArgumentEncoderWithBufferIndex:0];
		argument_buffer = [device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
		output = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
		input = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
		[arguments setArgumentBuffer:argument_buffer offset:0]; [arguments setBuffer:output offset:0 atIndex:0];
		[arguments setBuffer:input offset:0 atIndex:1]; [arguments setTexture:texture atIndex:64];
		// The production renderer writes gpuAddress/resourceID at id*8,
		// rather than using MTLArgumentEncoder. Check both representations.
		const auto* encoded = static_cast<const uint64_t*>(argument_buffer.contents);
		Check(encoded[0] == output.gpuAddress && encoded[1] == input.gpuAddress && encoded[64] == texture.gpuResourceID._impl,
		      "native raw argument-buffer ABI differs from Metal's encoder");
#if KYTX_COMPARE_SPIRV_PATH
		if (reference_pipeline) {
			[reference_arguments setBuffer:input offset:0 atIndex:reference_abi.BaseOf(IR::DescriptorBindingKind::Buffers)+1];
			reference_texture=!compare || (std::getenv("KYTX_REFERENCE_COLOR_TEXTURE") && Metal::SupportsNativeDepthCompare(texture.pixelFormat)) ? texture : native_depth;
			for (unsigned mip=0;mip<image.mip_count;++mip)
				[reference_arguments setTexture:reference_texture atIndex:reference_abi.BaseOf(*IR::DescriptorBindingForImage(image))+mip];
		}
#endif
		if (native_depth && !has_offset) {
			std::string type = array ? "depth2d_array<float>" : "depth2d<float>";
			std::string source = "#include <metal_stdlib>\nusing namespace metal;\n kernel void oracle(" + type +
			    " t [[texture(0)]], sampler s [[sampler(0)]], constant float* p [[buffer(0)]], device float4* o [[buffer(1)]]) { *o = float4(t." +
			    (gather ? "gather_compare" : "sample_compare") + "(s, float2(p[0],p[1])" + (array ? ", uint(p[3])" : "") +
			    ", p[2]" + (gather ? "" : derivative ? ", gradient2d(float2(p[4],0.0f),float2(0.0f,p[5]))" : ", level(p[4])") +
			    (gather ? ")); }" : "), 0.0f, 0.0f, 0.0f); }");
			id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:nil error:&error];
			if (!library) std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
			Check(library != nil, "native sample_compare oracle compilation failed");
			oracle_pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"oracle"] error:&error];
			Check(oracle_pipeline != nil, "native depth oracle pipeline failed");
		}
	}

	float Texel(const ComparisonCase& c, int x, int y, int z, unsigned mip) const {
		const int w = std::max(4u >> mip, 1u), h = spatial > 1 ? w : 1, d = spatial == 3 ? w : 1;
		bool outside = false;
		const auto address = [&](int value, int extent) {
			if (c.clamp == 0) return (value % extent + extent) % extent;
			if (c.clamp == 1) { const int n = (value % (extent * 2) + extent * 2) % (extent * 2); return n < extent ? n : extent * 2 - 1 - n; }
			if (c.clamp == 6 && (value < 0 || value >= extent)) outside = true;
			return std::clamp(value, 0, extent - 1);
		};
		x = address(x,w); if (spatial > 1) y = address(y,h); if (spatial == 3) z = address(z,d);
		return outside ? (c.border == 2 ? 1.f : 0.f) : levels[mip][((c.slice * d + z) * h + y) * w + x];
	}
	float Level(const ComparisonCase& c, unsigned mip, bool linear) const {
		const unsigned w = std::max(4u >> mip, 1u);
		const std::array<float,3> uv{c.u,c.v,c.z}; std::array<int,3> base {}; std::array<float,3> frac {};
		for (unsigned axis = 0; axis < spatial; ++axis) {
			const float pos = uv[axis] * (c.pixel ? 1.f : float(w)) + c.offset[axis] - (linear ? .5f : 0.f);
			base[axis] = int(std::floor(pos)); frac[axis] = pos - base[axis];
		}
		float result = 0;
		bool have_value=false;
		for (unsigned tap = 0; tap < (linear ? 1u << spatial : 1u); ++tap) {
			auto coord = base; float weight = 1;
			for (unsigned axis = 0; linear && axis < spatial; ++axis) {
				const bool upper = (tap & (1u << axis)) != 0; coord[axis] += upper; weight *= upper ? frac[axis] : 1 - frac[axis];
			}
			const float texel = Texel(c,coord[0],coord[1],coord[2],mip);
			if (!compare && c.reduction && weight>0.f) {
				result=!have_value?texel:c.reduction==1?std::min(result,texel):std::max(result,texel);have_value=true;
			} else if (weight>0.f) result += (compare ? float(ModelCompare(c.function,texel,c.reference)) : texel) * weight;
		}
		return result;
	}
	std::array<float,4> Model(const ComparisonCase& c) const {
		std::array<float,4> expected {};
		if (gather) {
			const int x = int(std::floor(c.u * 4 - .5f)) + c.offset[0], y = int(std::floor(c.v * 4 - .5f)) + c.offset[1];
			const std::array<std::pair<int,int>,4> footprint{{{x,y+1},{x+1,y+1},{x+1,y},{x,y}}};
			for (unsigned i = 0; i < 4; ++i) expected[i] = ModelCompare(c.function,Texel(c,footprint[i].first,footprint[i].second,0,0),c.reference);
			return expected;
		}
		const float lambda = c.mip == 0 || c.pixel ? 0.f : std::clamp(c.lod + c.bias / 256.f,c.min_lod / 256.f,c.max_lod / 256.f);
		const bool linear = lambda <= 0 ? c.mag_linear : c.min_linear;
		const float selected = spatial == 1 ? 0.f : std::clamp(lambda,0.f,float(levels.size()-1));
		const unsigned first = unsigned(c.mip == 2 ? std::floor(selected) : std::ceil(selected + .5f) - 1.f);
		const float a = Level(c,first,linear);
		float value=a;
		if (c.mip==2 && selected>first) {
			const float b=Level(c,first+1,linear);
			value=!compare && c.reduction==1?std::min(a,b):!compare && c.reduction==2?std::max(a,b):a*(1-(selected-first))+b*(selected-first);
		}
		expected[0]=value; if (!compare) expected[3]=1.f; return expected;
	}

	void Run(const ComparisonCase& c, unsigned& blended) {
		ShaderSamplerResource guest {};
		guest.fields[0] = c.clamp | (c.clamp << 3u) | (c.clamp << 6u) | (c.aniso_ratio << 9u) | (c.function << 12u) | (c.pixel ? 0x8000u : 0u);
		guest.fields[0] |= c.reduction<<29u;
		guest.fields[1] = c.min_lod | (c.max_lod << 12u);
		guest.fields[2] = uint32_t(c.bias & 0x3fff) | (c.min_linear ? 1u << 22u : 0u) | (c.mag_linear ? 1u << 20u : 0u) | (c.mip << 26u);
		if (c.aniso_ratio) guest.fields[2] |= (2u << 22u) | (2u << 20u);
		guest.fields[3] = c.border << 30u;
		id<MTLSamplerState> sampler = Metal::GetSampler(device,guest), point = Metal::GetSampler(device,guest,true);
		Check(sampler && point, "comparison sampler state unavailable");
		const bool negative = std::getenv("KYTX_TEST_FILTER_DEPTH_FIRST") != nullptr;
		[arguments setSamplerState:sampler atIndex:128];
		[arguments setSamplerState:negative ? sampler : point atIndex:ShaderRecompiler::Msl::ComparisonPointSamplerBase];
		const auto* encoded = static_cast<const uint64_t*>(argument_buffer.contents);
		Check(encoded[128] == sampler.gpuResourceID._impl &&
		      encoded[ShaderRecompiler::Msl::ComparisonPointSamplerBase] == (negative ? sampler : point).gpuResourceID._impl,
		      "comparison sampler alias differs in the production raw argument-buffer ABI");
		Check(guest.XyMinFilter() == unsigned(c.min_linear) + (c.aniso_ratio ? 2u : 0u) &&
		      guest.XyMagFilter() == unsigned(c.mag_linear) + (c.aniso_ratio ? 2u : 0u) && guest.MaxAnisoRatio() == c.aniso_ratio,
		      "comparison test's S# packing disagrees with the production descriptor decoder");
		std::memset(output.contents,0xcd,output.length); std::memset(input.contents,0,input.length);
		auto* words = static_cast<uint32_t*>(input.contents); unsigned cursor = 0;
		if (has_offset) words[cursor++] = uint32_t(c.offset[0] & 63) | (uint32_t(c.offset[1] & 63) << 8) | (uint32_t(c.offset[2] & 63) << 16);
		const auto put = [&](float v) { words[cursor++] = std::bit_cast<uint32_t>(v); };
		if (compare) put(c.reference);
		if (derivative) {
			for (unsigned axis=0;axis<spatial;++axis) put(axis==0 ? std::exp2(c.lod)/(c.pixel?1.f:4.f) : 0.f);
			for (unsigned axis=0;axis<spatial;++axis) put(axis==(spatial>1?1u:0u) ? std::exp2(c.lod)/((c.pixel?1.f:4.f)*c.gradient_aspect) : 0.f);
		}
		put(c.u); if (spatial > 1) put(c.v); if (spatial == 3) put(c.z);
		if (array) put(float(c.slice)); if (!gather && !derivative) put(c.lod);
		uint32_t push[32] {}, sizes[ShaderRecompiler::Msl::SizeConstantWordCount] {};
		sizes[0] = sizes[1] = 64; sizes[64] = c.function;
		sizes[ShaderRecompiler::Msl::ImageMetadataWordBase] = Metal::SupportsNativeDepthCompare(texture.pixelFormat) ? 1u : 0u;
		std::copy_n(guest.fields,4,sizes + ShaderRecompiler::Msl::SamplerDescriptorWordBase);
		if (negative) sizes[ShaderRecompiler::Msl::SamplerDescriptorWordBase+2] &= ~((3u << 20u) | (3u << 22u));
		id<MTLCommandBuffer> command = [queue commandBuffer];
		id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
		[encoder setComputePipelineState:pipeline]; [encoder setBuffer:argument_buffer offset:0 atIndex:0];
		[encoder setBytes:push length:sizeof(push) atIndex:1]; [encoder setBytes:sizes length:sizeof(sizes) atIndex:25];
		[encoder useResource:texture usage:MTLResourceUsageRead]; [encoder useResource:input usage:MTLResourceUsageRead]; [encoder useResource:output usage:MTLResourceUsageWrite];
		[encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)]; [encoder endEncoding];
		const bool oracle = oracle_pipeline && !c.pixel;
		if (oracle) {
			const float params[6]{c.u,c.v,c.reference,float(c.slice),derivative?std::exp2(c.lod)/4.f:c.lod,
			                      std::exp2(c.lod)/(4.f*c.gradient_aspect)};
			encoder = [command computeCommandEncoder]; [encoder setComputePipelineState:oracle_pipeline];
			[encoder setTexture:native_depth atIndex:0]; [encoder setSamplerState:sampler atIndex:0];
			[encoder setBytes:params length:sizeof(params) atIndex:0]; [encoder setBuffer:output offset:32 atIndex:1];
			[encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)]; [encoder endEncoding]; ++native_cases;
		}
#if KYTX_COMPARE_SPIRV_PATH
		// The Metal/Vulkan host offset contract is [-8,7]. Wider RDNA offsets
		// are still tested on the GPU against CPU footprints, not an invalid
		// native Offset operand which truncates on this device.
		bool reference_case=reference_pipeline && !c.pixel;
		if (has_offset) for (unsigned axis=0;axis<spatial;++axis)
			reference_case &= c.offset[axis]>=-8 && c.offset[axis]<=7;
		if (reference_case) {
			std::memset(reference_output.contents,0xcd,reference_output.length);
			[reference_arguments setSamplerState:sampler atIndex:reference_abi.BaseOf(IR::DescriptorBindingKind::Samplers)];
			encoder=[command computeCommandEncoder];[encoder setComputePipelineState:reference_pipeline];
			[encoder setBuffer:reference_ab offset:0 atIndex:0];[encoder setBytes:push length:sizeof(push) atIndex:1];
			[encoder setBuffer:reference_sizes offset:0 atIndex:25];
			[encoder useResource:reference_output usage:MTLResourceUsageWrite];[encoder useResource:input usage:MTLResourceUsageRead];
			[encoder useResource:reference_sizes usage:MTLResourceUsageRead];[encoder useResource:reference_texture usage:MTLResourceUsageRead];
			[encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[encoder endEncoding];++reference_cases;
		}
#endif
		[command commit]; [command waitUntilCompleted]; Check(command.status == MTLCommandBufferStatusCompleted,"comparison GPU execution failed");
		Check(c.aniso_ratio == 0 || oracle,"anisotropic comparisons require an independent native GPU oracle");
		const auto expected = Model(c); const auto* actual = static_cast<const float*>(output.contents);
		if (actual[0] > .001f && actual[0] < .999f) ++blended;
		for (unsigned i = 0; i < 4; ++i) {
			if (!std::isfinite(actual[i]) || (c.aniso_ratio == 0 && std::fabs(actual[i]-expected[i]) > 1.f/64) ||
			    (oracle && (!std::isfinite(actual[8+i]) || std::fabs(actual[i]-actual[8+i]) > 1.f/64))) {
				std::fprintf(stderr,"compare dim=%u array=%d gather=%d cmp=%u min/mag=%d/%d mip=%u lod=%g clamp=%u uv=%g,%g,%g ref=%g slice=%u lane=%u: CPU=%g MSL=%g native=%g\n",
				             spatial,array,gather,c.function,c.min_linear,c.mag_linear,c.mip,c.lod,c.clamp,c.u,c.v,c.z,c.reference,c.slice,i,expected[i],actual[i],oracle?actual[8+i]:-1.f);
				std::fprintf(stderr,"  compare=%d gradient=%d pixel=%d reduction=%u bias=%d limits=%u/%u offset=%d,%d,%d\n",compare,derivative,c.pixel,c.reduction,c.bias,c.min_lod,c.max_lod,c.offset[0],c.offset[1],c.offset[2]);
				throw std::runtime_error("comparison sampling mismatch");
			}
		}
#if KYTX_COMPARE_SPIRV_PATH
		if (reference_case) {
			const auto* reference=static_cast<const float*>(reference_output.contents);
			for (unsigned i=0;i<4;++i) {
				if (!std::isfinite(reference[i]) || std::fabs(reference[i]-actual[i])>1.f/64) {
					std::fprintf(stderr,"IR path differential: dim=%u array=%d gather=%d offset=%d derivative=%d cmp=%u lod=%g mip=%u min/mag=%d/%d clamp=%u uv=%g,%g ref=%g slice=%u lane=%u direct=%g SPIR-V->MSL=%g\n",spatial,array,gather,has_offset,derivative,c.function,c.lod,c.mip,c.min_linear,c.mag_linear,c.clamp,c.u,c.v,c.reference,c.slice,i,actual[i],reference[i]);
					throw std::runtime_error("direct and Vulkan shader paths disagree");
				}
			}
		}
#endif
		const auto* guards = static_cast<const uint32_t*>(output.contents);
		for (unsigned i : {4u,5u,6u,7u,12u,13u,14u,15u}) Check(guards[i]==0xcdcdcdcdu,"comparison output crossed its guards");
	}
};

static void DepthCompareSampling() {
	using D = ShaderRecompiler::Decoder::ImageDimension;
	unsigned total=0, blended=0, native=0;
#if KYTX_COMPARE_SPIRV_PATH
	unsigned reference_total=0;
#endif
	for (auto dimension : {D::Dim1D,D::Dim1DArray,D::Dim2D,D::Dim2DArray,D::Dim3D}) {
		ComparisonHarness h(dimension,false,false);
		for (uint32_t func=0;func<8;++func) for (bool linear : {false,true})
		for (float reference : {0.f,.25f,.425f,.61f,1.f}) for (auto uv : {std::pair{.5f,.5f},{.3f,.7f},{.13f,.41f},{.9f,.1f},{0.f,0.f},{1.f,1.f}}) {
			ComparisonCase c; c.function=func; c.min_linear=c.mag_linear=linear; c.reference=reference; c.u=uv.first; c.v=uv.second;
			c.slice=h.array?1:0; h.Run(c,blended); ++total;
		}
		if (h.spatial>1) for (unsigned mip : {1u,2u}) for (float lod : {-.75f,.25f,.5f,1.f,1.75f,8.f})
		for (bool min : {false,true}) for (bool mag : {false,true}) for (unsigned func : {1u,3u,4u,6u}) {
			ComparisonCase c; c.mip=mip; c.lod=lod; c.min_linear=min; c.mag_linear=mag; c.function=func; c.slice=h.array?1:0;
			h.Run(c,blended); ++total;
		}
		if (h.spatial>1) for (auto limits : {std::pair{0u,0u},{0u,256u},{128u,512u},{512u,512u}})
		for (int bias : {-128,128}) for (float lod : {.25f,1.5f}) for (bool min : {false,true}) {
			ComparisonCase c; c.mip=2; c.min_lod=limits.first; c.max_lod=limits.second; c.bias=bias; c.lod=lod;
			c.min_linear=min; c.mag_linear=!min; c.slice=h.array?1:0; h.Run(c,blended); ++total;
		}
		for (unsigned clamp : {0u,1u,2u,6u}) for (unsigned border : {0u,2u}) for (bool linear : {false,true})
		for (float u : {-1.1f,-.1f,1.1f,2.1f}) {
			ComparisonCase c; c.clamp=clamp; c.border=border; c.min_linear=c.mag_linear=linear; c.u=u; c.v=u; c.z=u;
			h.Run(c,blended); ++total;
		}
		for (bool linear : {false,true}) for (float u : {.1f,1.3f,2.f,3.9f}) {
			ComparisonCase c; c.pixel=true; c.min_linear=c.mag_linear=linear; c.u=u; c.v=4-u; c.z=u;
			h.Run(c,blended); ++total;
		}
		native+=h.native_cases;
#if KYTX_COMPARE_SPIRV_PATH
		reference_total+=h.reference_cases;
#endif
	}
	for (bool array : {false,true}) for (bool gather : {false,true}) for (bool offset : {false,true}) {
		if (!gather && !offset) continue;
		ComparisonHarness h(array?D::Dim2DArray:D::Dim2D,gather,offset);
		for (unsigned func=0;func<8;++func) for (auto uv : {std::pair{.37f,.68f},{0.f,0.f},{1.f,1.f}}) {
			ComparisonCase c; c.function=func; c.u=uv.first; c.v=uv.second; c.slice=array?1:0;
			if (offset) c.offset={-1,1,0};
			h.Run(c,blended); ++total;
		}
		native+=h.native_cases;
#if KYTX_COMPARE_SPIRV_PATH
		reference_total+=h.reference_cases;
#endif
	}
	for (auto dimension : {D::Dim1D,D::Dim1DArray,D::Dim3D}) {
		ComparisonHarness h(dimension,false,true);
		for (unsigned func=0;func<8;++func) for (float u : {.13f,.5f,.91f}) {
			ComparisonCase c; c.function=func; c.u=u; c.offset={-1,1,-1}; c.slice=h.array?1:0;
			if (h.spatial>1) {c.mip=2;c.lod=.75f;}
			h.Run(c,blended); ++total;
		}
	}
	for (auto dimension : {D::Dim2D,D::Dim2DArray,D::Dim3D}) {
		ComparisonHarness h(dimension,false,true,true);
		for (float lod : {-1.f,.5f,1.5f}) for (int bias : {-128,128}) for (unsigned func : {1u,3u,4u,6u}) {
			ComparisonCase c; c.function=func;c.lod=lod;c.bias=bias;c.mip=2;c.offset={-1,1,-1};c.slice=h.array?1:0;
			h.Run(c,blended);++total;
		}
#if KYTX_COMPARE_SPIRV_PATH
		reference_total+=h.reference_cases;
#endif
	}
	// The native Dref handle cast must preserve physical color formats and the
	// selected red component, including quantization and array layers.
	for (auto format : {MTLPixelFormatR16Unorm,MTLPixelFormatR16Float,MTLPixelFormatRG32Float,
	                    MTLPixelFormatRGBA16Float,MTLPixelFormatRGBA32Float})
	for (bool array : {false,true}) {
		ComparisonHarness h(array?D::Dim2DArray:D::Dim2D,false,false,false,format);
		for (unsigned func=0;func<8;++func) for (bool linear : {false,true}) for (float reference : {.25f,.425f})
		for (float lod : {0.f,.75f}) {
			ComparisonCase c;c.function=func;c.min_linear=c.mag_linear=linear;c.reference=reference;
			c.u=.37f;c.v=.68f;c.slice=array?1:0;c.mip=2;c.lod=lod;h.Run(c,blended);++total;
		}
		native+=h.native_cases;
#if KYTX_COMPARE_SPIRV_PATH
		reference_total+=h.reference_cases;
#endif
	}
	unsigned anisotropic=0;
	for (auto dimension : {D::Dim2D,D::Dim2DArray}) {
		ComparisonHarness h(dimension,false,false,true);
		for (unsigned ratio : {1u,2u,3u,4u}) for (float aspect : {2.f,8.f})
		for (unsigned func : {1u,3u,4u,6u}) for (float lod : {1.f,2.5f}) {
			ComparisonCase c;c.aniso_ratio=ratio;c.gradient_aspect=aspect;c.function=func;c.mip=2;c.lod=lod;
			c.u=.37f;c.v=.68f;c.slice=h.array?1:0;h.Run(c,blended);++anisotropic;
		}
		native+=h.native_cases;
#if KYTX_COMPARE_SPIRV_PATH
		reference_total+=h.reference_cases;
#endif
	}
	std::printf("Depth compare: %u GPU cases, %u native sample_compare/gather_compare cross-checks, %u blended PCF results; CPU footprints/guards PASS\n",total,native,blended);
	std::printf("Anisotropic Dref: %u GPU cases against independent native depth textures (2x/4x/8x/16x) PASS\n",anisotropic);
	Check(blended!=0 && native!=0,"comparison matrix did not exercise PCF and the independent GPU oracle");
#if KYTX_COMPARE_SPIRV_PATH
	Check(reference_total!=0,"Vulkan shader path oracle did not execute");
	std::printf("IR->SPIR-V->SPIRV-Cross->Metal differential: %u matching GPU cases\n",reference_total);
#endif
}
