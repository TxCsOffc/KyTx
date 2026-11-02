// Focused native IR -> MSL -> Metal GPU tests. No graphics backend is initialized.
#include "graphics/shader/recompiler/backend/msl/MslEmitter.h"
#include "graphics/host_gpu/metal/metalIndirect.h"
#include "common/assert.h"

#import <Metal/Metal.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

using namespace Libs::Graphics;
namespace IR = ShaderRecompiler::IR;
namespace CFG = ShaderRecompiler::CFG;
using IR::Value;
using IR::ValueOpcode;

static void Check(bool value, const char* message) {
	if (!value) throw std::runtime_error(message);
}

struct Fixture {
	IR::Program program;
	uint32_t lds_dwords = 0;
	Fixture(uint32_t buffers = 1) {
		program.stage = ShaderType::Compute;
		program.wave_size = 32;
		program.info.buffers.resize(buffers);
		std::vector<uint32_t> resources;
		for (uint32_t i = 0; i < buffers; ++i) resources.push_back(i);
		program.bindings.descriptors.push_back({IR::DescriptorBindingKind::Buffers, resources});
		Block();
	}
	uint32_t Block() {
		program.block_storage.push_back(std::make_unique<IR::Block>());
		program.blocks.push_back(program.block_storage.back().get());
		uint32_t id = static_cast<uint32_t>(program.block_info.size());
		program.block_info.push_back({.id = id});
		return id;
	}
	Value Emit(uint32_t block, ValueOpcode opcode, std::initializer_list<Value> args, uint64_t flags = 0) {
		return Value(&program.blocks[block]->AppendNewInst(opcode, args, flags));
	}
	uint64_t Memory(uint32_t resource, bool formatted = false, bool planning = false) {
		uint64_t index = program.memory_info.size();
		program.memory_info.push_back({.kind = IR::ResourceKind::Buffer, .resource = resource,
		                               .formatted = formatted, .planning_only = planning});
		return index;
	}
	Value Buffer(uint32_t block) { return Emit(block, ValueOpcode::GetBufferResource, {Value(0u),Value(0u),Value(0u),Value(0u)}); }
	void Store(uint32_t block, Value handle, uint32_t offset, Value value, uint64_t memory) {
		Emit(block, ValueOpcode::StoreBufferU32, {handle,Value(0u),Value(offset),Value(0u),value,Value(true)}, memory);
	}
};

static id<MTLDevice> device;

static id<MTLFunction> Compile(Fixture& fixture, const ShaderPixelInputInfo* pixel_options = nullptr) {
	ShaderComputeInputInfo compute {};
	compute.lds_size_dwords = fixture.lds_dwords;
	ShaderPixelInputInfo pixel {};
	pixel.dual_source_blending = true;
	ShaderStageInputInfo input {};
	input.compute = &compute;
	if (fixture.program.stage == ShaderType::Pixel) input.pixel = pixel_options ? pixel_options : &pixel;
	auto result = ShaderRecompiler::Msl::EmitProgram(fixture.program, input);
	for (const auto& reason: result.unsupported) std::fprintf(stderr, "%s\n", reason.c_str());
	Check(result.success, "native MSL emission failed");
	NSError* error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:result.source.c_str()] options:nil error:&error];
	if (!library) std::fprintf(stderr, "%s\n%s\n", error.localizedDescription.UTF8String, result.source.c_str());
	Check(library != nil, "emitted MSL failed to compile");
	return [library newFunctionWithName:@"main0"];
}

static std::array<uint8_t, 32> Run(Fixture& fixture, uint32_t size0 = 32) {
	id<MTLFunction> function = Compile(fixture);
	NSError* error = nil;
	id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
	Check(pipeline != nil, "native MSL pipeline failed");
	id<MTLBuffer> a = [device newBufferWithLength:32 options:MTLResourceStorageModeShared];
	id<MTLBuffer> b = [device newBufferWithLength:32 options:MTLResourceStorageModeShared];
	std::memset(a.contents, 0xcd, 32);
	std::memset(b.contents, 0xcd, 32);
	static_cast<uint8_t*>(a.contents)[1] = 0x34;
	static_cast<uint8_t*>(a.contents)[2] = 0x12;
	id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:0];
	id<MTLBuffer> argbuf = [device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
	[arguments setArgumentBuffer:argbuf offset:0];
	[arguments setBuffer:a offset:0 atIndex:0];
	if (fixture.program.info.buffers.size() > 1) [arguments setBuffer:b offset:0 atIndex:1];
	std::vector<id<MTLBuffer>> address_resources;
	for (const auto& binding: fixture.program.bindings.descriptors) {
		if (binding.kind != IR::DescriptorBindingKind::BdaPagetable) continue;
		constexpr uint64_t page = 16384, base = 0x8123400000ull;
		id<MTLBuffer> physical = [device newBufferWithLength:page * 3 options:MTLResourceStorageModeShared];
		id<MTLBuffer> root = [device newBufferWithLength:98304 * 8 options:MTLResourceStorageModeShared];
		id<MTLBuffer> leaves = [device newBufferWithLength:3 * 1024 * 8 options:MTLResourceStorageModeShared];
		std::memset(root.contents, 0, root.length);
		std::memset(leaves.contents, 0, leaves.length);
		std::memset(physical.contents, 0, physical.length);
		auto* bytes = static_cast<uint8_t*>(physical.contents);
		bytes[4] = 0x78; bytes[5] = 0x56; bytes[6] = 0x34; bytes[7] = 0x12;
		bytes[page - 2] = 0xaa; bytes[page - 1] = 0xbb;
		bytes[page] = 0x22; bytes[page + 1] = 0x11;
		bytes[page * 2 + 0x20] = 0x42;
		auto* roots = static_cast<uint64_t*>(root.contents);
		auto* entries = static_cast<uint64_t*>(leaves.contents);
		roots[base >> 24] = leaves.gpuAddress;
		entries[(base >> 14) & 1023] = physical.gpuAddress;
		entries[((base >> 14) & 1023) + 1] = physical.gpuAddress + page;
		roots[0x8100000020ull >> 24] = leaves.gpuAddress + 1024 * 8;
		entries[1024] = physical.gpuAddress + page * 2;
		roots[65536] = leaves.gpuAddress + 2048 * 8; // packed extended memory
		entries[2048] = physical.gpuAddress;
		[arguments setBuffer:root offset:0 atIndex:162];
		address_resources = {root, leaves, physical};
	}
	uint32_t sizes[64] {};
	sizes[0] = size0;
	sizes[1] = 32;
	uint32_t push[32] {};
	id<MTLCommandQueue> queue = [device newCommandQueue];
	id<MTLCommandBuffer> command = [queue commandBuffer];
	id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
	[encoder setComputePipelineState:pipeline];
	[encoder setBuffer:argbuf offset:0 atIndex:0];
	[encoder setBytes:push length:sizeof(push) atIndex:1];
	[encoder setBytes:sizes length:sizeof(sizes) atIndex:25];
	[encoder useResource:a usage:MTLResourceUsageRead | MTLResourceUsageWrite];
	[encoder useResource:b usage:MTLResourceUsageRead | MTLResourceUsageWrite];
	for (id<MTLBuffer> resource: address_resources) [encoder useResource:resource usage:MTLResourceUsageRead];
	[encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
	[encoder endEncoding];
	[command commit];
	[command waitUntilCompleted];
	Check(command.status == MTLCommandBufferStatusCompleted, "native MSL GPU dispatch failed");
	std::array<uint8_t, 32> output;
	std::memcpy(output.data(), fixture.program.info.buffers.size() > 1 ? b.contents : a.contents, 32);
	return output;
}

// Exercise texture address interpretation and overloads on the GPU. Each mip and
// array layer has a distinct RGBA value, so a wrong layer/LOD/channel cannot pass.
static void ImageSampling() {
	using namespace ShaderRecompiler;
	using Dimension = Decoder::ImageDimension;
	struct Case {
		const char* name;
		Dimension dimension;
		float layer;
		bool derivative;
		int gather_component; // -1 = sample
		uint32_t expected_layer;
	};
	const Case cases[] = {
	    {"2D explicit LOD", Dimension::Dim2D, 0, false, -1, 0},
	    {"2D gradients", Dimension::Dim2D, 0, true, -1, 0},
	    {"array middle layer", Dimension::Dim2DArray, 1, false, -1, 1},
	    {"array round down", Dimension::Dim2DArray, 0.49f, false, -1, 0},
	    {"array round up", Dimension::Dim2DArray, 0.51f, false, -1, 1},
	    {"array negative layer", Dimension::Dim2DArray, -3, false, -1, 0},
	    {"array large layer", Dimension::Dim2DArray, 9, false, -1, 2},
	    {"array gradients", Dimension::Dim2DArray, 1, true, -1, 1},
	    {"3D gradients", Dimension::Dim3D, 0, true, -1, 0},
	    {"gather red", Dimension::Dim2D, 0, false, 0, 0},
	    {"gather green", Dimension::Dim2D, 0, false, 1, 0},
	    {"array gather blue", Dimension::Dim2DArray, 1, false, 2, 1},
	    {"array gather alpha", Dimension::Dim2DArray, 0.51f, false, 3, 1},
	};
	unsigned failures = 0;
	for (const auto& test: cases) {
		try {
			const bool arrayed = test.dimension == Dimension::Dim2DArray;
			const bool volume = test.dimension == Dimension::Dim3D;
			const bool gather = test.gather_component >= 0;
			const uint32_t spatial = volume ? 3 : 2;
			Fixture f;
			IR::ImageResource image {};
			image.resource_class = IR::ImageResourceClass::Sampled;
			image.numeric_class = Prospero::TextureNumericClass::Float;
			image.dimension = test.dimension;
			image.mip_mode = IR::ImageMipMode::Dynamic;
			image.mip_count = 5;
			image.read = true;
			f.program.info.images.push_back(image);
			f.program.info.samplers.emplace_back();
			f.program.bindings.descriptors.push_back({*IR::DescriptorBindingForImage(image), {0}});
			f.program.bindings.descriptors.push_back({IR::DescriptorBindingKind::Samplers, {0}});
			const auto output_handle = f.Buffer(0);
			const auto output_memory = f.Memory(0);
			const auto texture_handle = f.Emit(0, ValueOpcode::GetImageResource,
			    {Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u)});
			const auto sampler_handle = f.Emit(0, ValueOpcode::GetSamplerResource,
			    {Value(0u),Value(0u),Value(0u),Value(0u)});
			std::array<Value, 13> address;
			address.fill(Value(0u));
			uint32_t cursor = 0;
			const auto coordinate = [&](float value) { address[cursor++] = Value(std::bit_cast<uint32_t>(value)); };
			if (test.derivative) {
				for (uint32_t i = 0; i < spatial; ++i) coordinate(i == 0 ? 0.25f : 0.0f);
				for (uint32_t i = 0; i < spatial; ++i) coordinate(i == 1 ? 0.25f : 0.0f);
			}
			for (uint32_t i = 0; i < spatial; ++i) coordinate(0.5f);
			if (arrayed) coordinate(test.layer);
			if (!test.derivative && !gather) coordinate(2.0f);
			const auto coords = f.Emit(0, ValueOpcode::MakeImageAddress,
			    {address[0],address[1],address[2],address[3],address[4],address[5],address[6],
			     address[7],address[8],address[9],address[10],address[11],address[12]});
			const uint64_t image_memory = f.program.memory_info.size();
			IR::MemoryInfo memory {};
			memory.kind = IR::ResourceKind::Image;
			memory.dmask = gather ? (1u << test.gather_component) : 0xfu;
			memory.image_sample_flags = test.derivative ? Decoder::ImageSampleFlagDerivative
			    : gather ? 0u : Decoder::ImageSampleFlagLod;
			memory.image_dimension = test.dimension;
			memory.image_address_components = cursor;
			f.program.memory_info.push_back(memory);
			const auto sampled = gather
			    ? f.Emit(0, ValueOpcode::ImageGatherRaw, {texture_handle, sampler_handle, coords,
			          Value(0u),Value(0u),Value(0u),Value(0u)}, image_memory)
			    : f.Emit(0, ValueOpcode::ImageSampleRaw, {texture_handle, sampler_handle, coords}, image_memory);
			for (uint32_t i = 0; i < 4; ++i)
				f.Store(0, output_handle, i * 4,
				    f.Emit(0, ValueOpcode::CompositeExtractU32x4, {sampled, Value(i)}), output_memory);

			id<MTLFunction> function = Compile(f);
			NSError* error = nil;
			id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
			Check(pipeline != nil, "image sampling pipeline failed");
			MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
			    width:16 height:16 mipmapped:YES];
			descriptor.textureType = arrayed ? MTLTextureType2DArray : volume ? MTLTextureType3D : MTLTextureType2D;
			descriptor.arrayLength = arrayed ? 3 : 1;
			descriptor.depth = volume ? 16 : 1;
			descriptor.storageMode = MTLStorageModeShared;
			descriptor.usage = MTLTextureUsageShaderRead;
			id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
			Check(texture != nil, "image sampling texture allocation failed");
			for (uint32_t layer = 0; layer < descriptor.arrayLength; ++layer) {
				for (uint32_t mip = 0; mip < 5; ++mip) {
					const uint32_t size = 16 >> mip, depth = volume ? size : 1;
					std::vector<float> pixels(size * size * depth * 4);
					for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = 1 + layer * 100 + mip * 10 + i % 4;
					[texture replaceRegion:MTLRegionMake3D(0,0,0,size,size,depth) mipmapLevel:mip slice:layer
					    withBytes:pixels.data() bytesPerRow:size * 16 bytesPerImage:size * size * 16];
				}
			}
			MTLSamplerDescriptor* sampler_descriptor = [MTLSamplerDescriptor new];
			sampler_descriptor.minFilter = sampler_descriptor.magFilter = MTLSamplerMinMagFilterNearest;
			sampler_descriptor.mipFilter = MTLSamplerMipFilterNearest;
			sampler_descriptor.supportArgumentBuffers = YES;
			id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];
			id<MTLBuffer> output = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
			std::memset(output.contents, 0xcd, 16);
			id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:0];
			id<MTLBuffer> argbuf = [device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
			[arguments setArgumentBuffer:argbuf offset:0];
			[arguments setBuffer:output offset:0 atIndex:0];
			[arguments setTexture:texture atIndex:64];
			[arguments setSamplerState:sampler atIndex:128];
			uint32_t push[32] {}, sizes[64] {16};
			id<MTLCommandBuffer> command = [[device newCommandQueue] commandBuffer];
			id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
			[encoder setComputePipelineState:pipeline]; [encoder setBuffer:argbuf offset:0 atIndex:0];
			[encoder setBytes:push length:sizeof(push) atIndex:1]; [encoder setBytes:sizes length:sizeof(sizes) atIndex:25];
			[encoder useResource:texture usage:MTLResourceUsageRead]; [encoder useResource:output usage:MTLResourceUsageWrite];
			[encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
			[encoder endEncoding]; [command commit]; [command waitUntilCompleted];
			Check(command.status == MTLCommandBufferStatusCompleted, "image sampling dispatch failed");
			const auto* actual = static_cast<const float*>(output.contents);
			for (uint32_t i = 0; i < 4; ++i) {
				const float expected = 1 + test.expected_layer * 100 + (gather ? test.gather_component : 20 + i);
				if (actual[i] != expected) {
					std::fprintf(stderr, "%s: component %u expected %g, got %g\n", test.name, i, expected, actual[i]);
					throw std::runtime_error("image sampling result mismatch");
				}
			}
		} catch (const std::exception& error) {
			++failures;
			std::fprintf(stderr, "%s: %s\n", test.name, error.what());
		}
	}
	std::printf("Image sampling: %zu GPU cases / %u failures\n", std::size(cases), failures);
	Check(failures == 0, "image sampling regressions");
}

static void FormattedStores() {
	Fixture f;
	f.program.info.buffers[0].descriptor_format = Prospero::BufferFormat::k8_8_8_8UNorm;
	const auto buffer = f.Buffer(0);
	const auto rgba = f.Emit(0, ValueOpcode::CompositeConstructU32x4,
	    {Value(std::bit_cast<uint32_t>(0.5f)), Value(std::bit_cast<uint32_t>(-1.0f)),
	     Value(std::bit_cast<uint32_t>(2.0f)), Value(std::bit_cast<uint32_t>(0.25f))});
	const auto memory = f.Memory(0, true);
	f.Emit(0, ValueOpcode::StoreBufferU32x4, {buffer,Value(0u),Value(0u),Value(0u),rgba,Value(true)}, memory);
	f.Store(0, buffer, 4, Value(std::bit_cast<uint32_t>(0.5f)), memory);
	f.Store(0, buffer, 7, Value(0xffffffffu), memory); // complete format is out of bounds
	const auto output = Run(f, 8);
	const uint8_t expected[8] {128,0,255,64,128,0,0,0};
	Check(std::memcmp(output.data(), expected, 8) == 0, "RGBA8 formatted store or missing-component fill incorrect");
	Check(output[8] == 0xcd, "formatted store crossed its descriptor bounds");
}

static void RawBufferBounds() {
	const ValueOpcode loads[] = {ValueOpcode::LoadBufferU32, ValueOpcode::LoadBufferU32x2,
	    ValueOpcode::LoadBufferU32x3, ValueOpcode::LoadBufferU32x4};
	const ValueOpcode stores[] = {ValueOpcode::StoreBufferU32, ValueOpcode::StoreBufferU32x2,
	    ValueOpcode::StoreBufferU32x3, ValueOpcode::StoreBufferU32x4};
	for (uint32_t count = 1; count <= 4; ++count) for (uint32_t size : {0u, 3u, 12u, 32u})
	for (uint32_t offset : {0u, 8u, 28u, 32u, 0xfffffffcu}) {
		Fixture f(2);
		const auto handle = f.Buffer(0);
		const auto read = f.Memory(0), write = f.Memory(1);
		const auto value = f.Emit(0, loads[count-1], {handle,Value(0u),Value(offset),Value(0u),Value(true)}, read);
		f.Emit(0, stores[count-1], {handle,Value(0u),Value(0u),Value(0u),value,Value(true)}, write);
		const auto output = Run(f, size);
		for (uint32_t c=0;c<count;++c) {
			uint32_t actual; std::memcpy(&actual, output.data()+c*4,4);
			const bool valid = offset / 4 < size / 4 && c < size / 4 - offset / 4;
			const uint32_t expected = valid ? (offset==0 && c==0 ? 0xcd1234cdu : 0xcdcdcdcdu) : 0u;
			Check(actual==expected, "raw vector load crossed bounds or aliased the final word");
		}
	}
	for (uint32_t count=1;count<=4;++count) for(uint32_t size:{0u,3u,12u,32u})
	for(uint32_t offset:{8u,28u,32u,0xfffffffcu}) {
		Fixture f;
		const auto h=f.Buffer(0);
		const auto mem=f.Memory(0);
		Value value(0x12345678u);
		if(count>1) {
			const auto op=count==2 ? ValueOpcode::CompositeConstructU32x2 : count==3 ? ValueOpcode::CompositeConstructU32x3 : ValueOpcode::CompositeConstructU32x4;
			value = count==2 ? f.Emit(0,op,{value,value}) : count==3 ? f.Emit(0,op,{value,value,value}) : f.Emit(0,op,{value,value,value,value});
		}
		f.Emit(0,stores[count-1],{h,Value(0u),Value(offset),Value(0u),value,Value(true)},mem);
		const auto output=Run(f,size);
		for(uint32_t c=0;c<8;++c) {
			uint32_t actual; std::memcpy(&actual,output.data()+c*4,4);
			const bool written=c<size/4 && c>=offset/4 && c-offset/4<count;
			Check(actual==(written ? 0x12345678u : c==0 ? 0xcd1234cdu : 0xcdcdcdcdu), "raw vector store crossed bounds or clamped to final word");
		}
	}
}

static void DeclaredLds() {
	Fixture f;
	f.lds_dwords = 384;
	const auto memory = f.program.memory_info.size();
	f.program.memory_info.push_back({.kind=IR::ResourceKind::Lds});
	f.Emit(0,ValueOpcode::WriteSharedU32,{Value(383u*4),Value(0x12345678u),Value(true)},memory);
	f.Emit(0,ValueOpcode::WriteSharedU32,{Value(383u*4),Value(0u),Value(false)},memory);
	f.Emit(0,ValueOpcode::WriteSharedU32,{Value(384u*4),Value(0u),Value(true)},memory);
	const auto value = f.Emit(0,ValueOpcode::LoadSharedU32,{Value(383u*4),Value(true)},memory);
	const auto oob = f.Emit(0,ValueOpcode::LoadSharedU32,{Value(384u*4),Value(true)},memory);
	const auto inactive = f.Emit(0,ValueOpcode::LoadSharedU32,{Value(383u*4),Value(false)},memory);
	const auto handle=f.Buffer(0);
	const auto output_memory=f.Memory(0);
	f.Store(0,handle,0,value,output_memory);
	f.Store(0,handle,4,oob,output_memory);
	f.Store(0,handle,8,inactive,output_memory);
	const auto output=Run(f);
	uint32_t actual;std::memcpy(&actual,output.data(),4);
	Check(actual==0x12345678u,"declared LDS final dword failed");
	std::memcpy(&actual,output.data()+4,4); Check(actual==0,"LDS out-of-range read did not return zero");
	std::memcpy(&actual,output.data()+8,4); Check(actual==0,"inactive LDS read did not return zero");
}

static void SubwordLoads() {
	Fixture f(2);
	const auto buffer = f.Buffer(0);
	const auto read = f.Memory(0);
	const auto write = f.Memory(1);
	const auto a = f.Emit(0, ValueOpcode::LoadBufferU16, {buffer,Value(0u),Value(1u),Value(0u),Value(true)}, read);
	const auto b = f.Emit(0, ValueOpcode::LoadBufferU16, {buffer,Value(0u),Value(4u),Value(0u),Value(true)}, read);
	f.Store(0, buffer, 0, a, write);
	f.Store(0, buffer, 4, b, write);
	const auto output = Run(f, 5);
	const uint32_t expected[2] {0x1234,0};
	Check(std::memcmp(output.data(), expected, 8) == 0, "unaligned/boundary subword loads incorrect");
}

static void GuestAddressLoads() {
	Fixture f;
	f.program.bindings.descriptors.push_back({IR::DescriptorBindingKind::BdaPagetable,{}});
	const auto buffer = f.Buffer(0);
	const auto write = f.Memory(0);
	const auto handle = f.Emit(0, ValueOpcode::GetAddressResource, {Value(0xfffffff0u),Value(0x80u)});
	const auto load = [&](uint32_t destination, uint64_t address, IR::ResourceKind kind,
	                      bool full = true, uint32_t offset = 0, bool active = true,
	                      ValueOpcode opcode = ValueOpcode::LoadAddressU32) {
		const auto memory = f.program.memory_info.size();
		f.program.memory_info.push_back({.kind=kind,.offset=offset,.address_is_full=full});
		const auto value = f.Emit(0, opcode,
		    {handle,Value(uint32_t(address)),Value(uint32_t(address >> 32)),Value(active)}, memory);
		f.Store(0, buffer, destination, value, write);
	};
	load(0, 0x8123400007ull, IR::ResourceKind::ScalarAddress); // scalar alignment
	load(4, 0x8123403ffeull, IR::ResourceKind::Flat); // spans independently mapped pages
	load(8, 0x8123408000ull, IR::ResourceKind::Global); // missing page
	load(12, 0x30, IR::ResourceKind::ScalarAddress, false); // carry from base low word
	load(16, 0x80000000004ull, IR::ResourceKind::ScalarAddress); // extended aperture
	load(20, 0x30000000000ull, IR::ResourceKind::Global); // invalid aperture
	load(24, 0x8123400004ull, IR::ResourceKind::Global, true, 0, false);
	load(28, 0x8123400005ull, IR::ResourceKind::Global, true, 0, true, ValueOpcode::LoadAddressU16);
	const auto output = Run(f);
	const uint32_t expected[8] {0x12345678u,0x1122bbaau,0u,0x42u,0x12345678u,0u,0u,0x3456u};
	Check(std::memcmp(output.data(), expected, sizeof(expected)) == 0, "guest address translation, page crossing, alignment, aperture or predication incorrect");
}

static void LoopPhiSwap() {
	Fixture f;
	const auto h = f.Block(), body = f.Block(), done = f.Block();
	const auto buffer = f.Buffer(0);
	const auto memory = f.Memory(0);
	f.program.block_info[0].terminator = {.kind=CFG::TerminatorKind::Branch,.true_block=h};
	auto& a = f.program.blocks[h]->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(IR::Type::U32));
	auto& b = f.program.blocks[h]->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(IR::Type::U32));
	auto& i = f.program.blocks[h]->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(IR::Type::U32));
	a.AddPhiOperand(f.program.blocks[0], Value(1u)); b.AddPhiOperand(f.program.blocks[0], Value(2u)); i.AddPhiOperand(f.program.blocks[0], Value(0u));
	const auto condition = f.Emit(h, ValueOpcode::ULessThan32, {Value(&i),Value(3u)});
	f.program.block_info[h].condition = condition;
	f.program.block_info[h].terminator = {.kind=CFG::TerminatorKind::ConditionalBranch,.true_block=body,.false_block=done,.merge_block=done,.continue_block=body,.loop_header=true};
	const auto next = f.Emit(body, ValueOpcode::IAdd32, {Value(&i),Value(1u)});
	a.AddPhiOperand(f.program.blocks[body], Value(&b)); b.AddPhiOperand(f.program.blocks[body], Value(&a)); i.AddPhiOperand(f.program.blocks[body], next);
	f.program.block_info[body].terminator = {.kind=CFG::TerminatorKind::Branch,.true_block=h};
	f.Store(done, buffer, 0, Value(&a), memory); f.Store(done, buffer, 4, Value(&b), memory);
	const auto output = Run(f);
	const uint32_t expected[2] {2,1};
	Check(std::memcmp(output.data(), expected, 8) == 0, "loop or simultaneous phi assignments incorrect");
}

static void DiamondPhi(bool take_true) {
	Fixture f;
	const auto yes = f.Block(), no = f.Block(), done = f.Block();
	const auto buffer = f.Buffer(0);
	const auto memory = f.Memory(0);
	f.program.block_info[0].condition = Value(take_true);
	f.program.block_info[0].terminator = {.kind=CFG::TerminatorKind::ConditionalBranch,.true_block=yes,.false_block=no,.merge_block=done};
	f.program.block_info[yes].terminator = {.kind=CFG::TerminatorKind::Branch,.true_block=done};
	f.program.block_info[no].terminator = {.kind=CFG::TerminatorKind::Branch,.true_block=done};
	auto& phi = f.program.blocks[done]->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(IR::Type::U32));
	phi.AddPhiOperand(f.program.blocks[yes], Value(17u));
	phi.AddPhiOperand(f.program.blocks[no], Value(29u));
	f.Store(done, buffer, 0, Value(&phi), memory);
	// The frontend uses stable block IDs, which need not match block positions.
	for (auto& info : f.program.block_info) {
		info.id += 10;
		if (info.terminator.true_block != UINT32_MAX) info.terminator.true_block += 10;
		if (info.terminator.false_block != UINT32_MAX) info.terminator.false_block += 10;
		if (info.terminator.merge_block != UINT32_MAX) info.terminator.merge_block += 10;
	}
	const auto output = Run(f);
	uint32_t value;
	std::memcpy(&value, output.data(), sizeof(value));
	Check(value == (take_true ? 17u : 29u), "diamond phi or stable block ID mapping incorrect");
}

static void NestedLoops() {
	Fixture f;
	const auto outer = f.Block(), inner = f.Block(), body = f.Block();
	const auto latch = f.Block(), done = f.Block();
	const auto buffer = f.Buffer(0);
	const auto memory = f.Memory(0);
	f.program.block_info[0].terminator = {.kind=CFG::TerminatorKind::Branch,.true_block=outer};
	auto& i = f.program.blocks[outer]->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(IR::Type::U32));
	auto& total = f.program.blocks[outer]->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(IR::Type::U32));
	i.AddPhiOperand(f.program.blocks[0], Value(0u));
	total.AddPhiOperand(f.program.blocks[0], Value(0u));
	f.program.block_info[outer].condition = f.Emit(outer, ValueOpcode::ULessThan32, {Value(&i),Value(3u)});
	f.program.block_info[outer].terminator = {.kind=CFG::TerminatorKind::ConditionalBranch,.true_block=inner,.false_block=done,.merge_block=done,.continue_block=latch,.loop_header=true};
	auto& j = f.program.blocks[inner]->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(IR::Type::U32));
	auto& sum = f.program.blocks[inner]->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(IR::Type::U32));
	j.AddPhiOperand(f.program.blocks[outer], Value(0u));
	sum.AddPhiOperand(f.program.blocks[outer], Value(&total));
	f.program.block_info[inner].condition = f.Emit(inner, ValueOpcode::ULessThan32, {Value(&j),Value(4u)});
	f.program.block_info[inner].terminator = {.kind=CFG::TerminatorKind::ConditionalBranch,.true_block=body,.false_block=latch,.merge_block=latch,.continue_block=body,.loop_header=true};
	const auto next_j = f.Emit(body, ValueOpcode::IAdd32, {Value(&j),Value(1u)});
	const auto next_sum = f.Emit(body, ValueOpcode::IAdd32, {Value(&sum),Value(&i)});
	j.AddPhiOperand(f.program.blocks[body], next_j);
	sum.AddPhiOperand(f.program.blocks[body], next_sum);
	f.program.block_info[body].terminator = {.kind=CFG::TerminatorKind::Branch,.true_block=inner};
	const auto next_i = f.Emit(latch, ValueOpcode::IAdd32, {Value(&i),Value(1u)});
	i.AddPhiOperand(f.program.blocks[latch], next_i);
	total.AddPhiOperand(f.program.blocks[latch], Value(&sum));
	f.program.block_info[latch].terminator = {.kind=CFG::TerminatorKind::Branch,.true_block=outer};
	f.Store(done, buffer, 0, Value(&total), memory);
	const auto output = Run(f);
	uint32_t value;
	std::memcpy(&value, output.data(), sizeof(value));
	Check(value == 12u, "nested loops or loop merge phis incorrect");
}

static void DualSourceOutput() {
	Fixture f;
	f.program.stage = ShaderType::Pixel;
	f.program.info.outputs.push_back({.kind=IR::StageOutputKind::Mrt,.index=0});
	f.program.info.outputs.push_back({.kind=IR::StageOutputKind::Mrt,.index=1});
	id<MTLFunction> fragment = Compile(f);
	NSError* error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; vertex float4 vs(uint id [[vertex_id]]) { return float4(0,0,0,1); }" options:nil error:&error];
	MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
	d.vertexFunction = [library newFunctionWithName:@"vs"];
	d.fragmentFunction = fragment;
	d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	d.colorAttachments[0].blendingEnabled = YES;
	d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSource1Color;
	id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:d error:&error];
	if (!pipeline) std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
	Check(pipeline != nil, "dual-source fragment output did not match its blend state");
}

static void VertexReadFirstLane() {
	Fixture f;
	f.program.stage = ShaderType::Vertex;
	f.program.info.outputs.push_back({.kind=IR::StageOutputKind::Position,.index=0});
	const auto buffer = f.Buffer(0);
	const auto memory = f.Memory(0);
	const auto vid = f.Emit(0, ValueOpcode::GetBuiltin, {Value(uint32_t(IR::StageInputKind::VertexIndex)),Value(0u)});
	const auto value = f.Emit(0, ValueOpcode::IAdd32, {vid,Value(10u)});
	const auto first = f.Emit(0, ValueOpcode::ReadFirstLane, {value,Value(true)});
	const auto selected = f.Emit(0, ValueOpcode::IEqual32, {vid,Value(1u)});
	f.Emit(0, ValueOpcode::StoreBufferU32, {buffer,Value(0u),Value(0u),Value(0u),first,selected}, memory);
	id<MTLFunction> function = Compile(f);
	MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
	descriptor.vertexFunction = function;
	NSError* error = nil;
	id<MTLLibrary> fragment = [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; fragment float4 fs() { return float4(1); }" options:nil error:&error];
	descriptor.fragmentFunction = [fragment newFunctionWithName:@"fs"];
	descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	if (!pipeline) std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
	Check(pipeline != nil, "vertex wave pipeline failed");
	id<MTLBuffer> result = [device newBufferWithLength:32 options:MTLResourceStorageModeShared];
	std::memset(result.contents, 0, 32);
	id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:0];
	id<MTLBuffer> argbuf = [device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
	[arguments setArgumentBuffer:argbuf offset:0]; [arguments setBuffer:result offset:0 atIndex:0];
	uint32_t sizes[64] {}; sizes[0] = 32;
	uint32_t push[32] {};
	MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
	MTLTextureDescriptor* target = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
	target.usage = MTLTextureUsageRenderTarget;
	pass.colorAttachments[0].texture = [device newTextureWithDescriptor:target];
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	id<MTLCommandQueue> queue = [device newCommandQueue];
	id<MTLCommandBuffer> command = [queue commandBuffer];
	id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
	[encoder setRenderPipelineState:pipeline]; [encoder setVertexBuffer:argbuf offset:0 atIndex:0];
	[encoder setVertexBytes:push length:sizeof(push) atIndex:1];
	[encoder setVertexBytes:sizes length:sizeof(sizes) atIndex:25];
	[encoder setVertexBytes:push length:16 atIndex:26];
	[encoder useResource:result usage:MTLResourceUsageWrite stages:MTLRenderStageVertex];
	[encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:32];
	[encoder endEncoding]; [command commit]; [command waitUntilCompleted];
	if (command.status != MTLCommandBufferStatusCompleted)
		std::fprintf(stderr, "Vertex wave command status=%lu error=%s\n", (unsigned long)command.status, command.error.localizedDescription.UTF8String);
	Check(command.status == MTLCommandBufferStatusCompleted, "vertex wave GPU execution failed");
	Check(*static_cast<uint32_t*>(result.contents) == 10u, "vertex ReadFirstLane did not broadcast the first active vertex");
}

static void IndirectRectList() {
	Fixture f;
	f.program.stage = ShaderType::Vertex;
	f.program.info.outputs.push_back({.kind=IR::StageOutputKind::Position,.index=0});
	f.program.info.outputs.push_back({.kind=IR::StageOutputKind::Parameter,.index=0});
	f.program.info.outputs.push_back({.kind=IR::StageOutputKind::Parameter,.index=1});
	f.program.export_info.push_back({.kind=IR::ExportTargetKind::Position,.index=0,.en=15});
	f.program.export_info.push_back({.kind=IR::ExportTargetKind::Parameter,.index=0,.en=15});
	f.program.export_info.push_back({.kind=IR::ExportTargetKind::Parameter,.index=1,.en=15});
	const auto vid = f.Emit(0, ValueOpcode::GetBuiltin, {Value(uint32_t(IR::StageInputKind::VertexIndex)),Value(0u)});
	const auto offset = f.Emit(0, ValueOpcode::IMul32, {vid,Value(16u)});
	const auto vertex = f.Emit(0, ValueOpcode::LoadBufferU32x4,
	    {f.Buffer(0),Value(0u),offset,Value(0u),Value(true)}, f.Memory(0));
	f.Emit(0, ValueOpcode::SetAttribute, {vertex,Value(true)}, 0);
	f.Emit(0, ValueOpcode::SetAttribute, {vertex,Value(true)}, 1);
	const auto packed = f.Emit(0, ValueOpcode::CompositeConstructU32x4,
	    {Value(5u), Value(0x10003u), Value(0x3f800000u), Value(0x80000000u)});
	f.Emit(0, ValueOpcode::SetAttribute, {packed,Value(true)}, 2);
	id<MTLFunction> function = Compile(f);
	NSError* error = nil;
	id<MTLLibrary> fragment = [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct In { float4 p [[position]]; float4 uv [[user(locn0)]]; float4 packed [[user(locn1), flat]]; };\nfragment float4 fs(In v [[stage_in]]) { bool good = all(as_type<uint4>(v.packed) == uint4(5u,0x10003u,0x3f800000u,0x80000000u)); return float4(v.uv.x*.5f+.5f, .5f-v.uv.y*.5f, good ? 0.0f : 1.0f, 1); }" options:nil error:&error];
	MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = function; pd.fragmentFunction = [fragment newFunctionWithName:@"fs"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
	id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
	Check(pipeline != nil, "indirect RECTLIST pipeline failed");
	const float vertices[][4] = {{-1,1,0,1},{-1,-1,0,1},{1,1,0,1},{0,0,0,0},{0,0,0,0},
	                            {-1,1,0,1},{-1,-1,0,1},{1,1,0,1},{1,-1,0,1}};
	id<MTLBuffer> vb = [device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
	id<MTLArgumentEncoder> ae = [function newArgumentEncoderWithBufferIndex:0];
	id<MTLBuffer> ab = [device newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
	[ae setArgumentBuffer:ab offset:0]; [ae setBuffer:vb offset:0 atIndex:0];
	// Padding, nonzero byte offset/stride, nonzero vertex/base instance, disabled
	// draws and a non-three count. The source is written by a GPU blit before
	// conversion, so CPU-side argument reads cannot make this test pass.
	const uint32_t records[][8] = {{3,1,0,0}, {3,2,5,7}, {3,0,0,9}, {0,1,5,0}, {4,1,5,11}};
	id<MTLBuffer> upload = [device newBufferWithBytes:records length:sizeof(records) options:MTLResourceStorageModeShared];
	id<MTLBuffer> source = [device newBufferWithLength:16+sizeof(records) options:MTLResourceStorageModePrivate];
	const uint32_t synthetic_first[] = {3,1,2,0x00ffffff,1,2};
	id<MTLBuffer> indices = [device newBufferWithBytes:synthetic_first length:sizeof(synthetic_first) options:MTLResourceStorageModeShared];
	id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	[blit copyFromBuffer:upload sourceOffset:0 toBuffer:source destinationOffset:16 size:sizeof(records)]; [blit endEncoding];
	Libs::Graphics::Metal::MetalRectListIndirect converter;
	Check(converter.Encode(cb, source, source.length-4, 1, 16) == nil, "RECTLIST source bounds accepted");
	id<MTLBuffer> converted = converter.Encode(cb, source, 16, 5, 32);
	Check(converted != nil, "RECTLIST GPU conversion failed");
	id<MTLBuffer> readback = [device newBufferWithLength:5*32 options:MTLResourceStorageModeShared];
	blit = [cb blitCommandEncoder];
	[blit copyFromBuffer:converted sourceOffset:0 toBuffer:readback destinationOffset:0 size:5*32]; [blit endEncoding];
	std::array<id<MTLTexture>, 9> targets;
	for (unsigned test=0; test<targets.size(); ++test) {
		auto td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:32 height:32 mipmapped:NO];
		td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageRenderTarget;
		targets[test] = [device newTextureWithDescriptor:td];
		auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = targets[test]; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,0);
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
		[re setRenderPipelineState:pipeline]; [re setVertexBuffer:ab offset:0 atIndex:0];
		uint32_t sizes[64] {}; sizes[0] = test<=1 || test>=6 ? 48 : test==5 ? sizeof(vertices) : 128;
		sizes[59]=0; sizes[60]=1; sizes[61]=2; sizes[62]=0x00ffffff;
		sizes[63] = test==7 ? 2 : test==8 ? 1 : 3; uint32_t push[32] {};
		[re setVertexBytes:push length:sizeof(push) atIndex:1]; [re setVertexBytes:sizes length:sizeof(sizes) atIndex:25];
		[re setVertexBuffer:converted offset:test && test<6 ? (test-1)*32+16 : 16 atIndex:26];
		[re useResource:vb usage:MTLResourceUsageRead];
		// The first Xcode replay caught Triangle still being selected despite
		// converted count=4. Keep that actual integration failure as a control.
		// Put the synthetic corner first in a separate triangle so flat output
		// observes that vertex's bits, rather than one of the original corners.
		if (test>=6) [re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt32 indexBuffer:indices indexBufferOffset:test==7 ? 12 : 0];
		else [re drawPrimitives:test ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle
		    indirectBuffer:converted indirectBufferOffset:test ? (test-1)*32 : 0];
		[re endEncoding];
	}
	[cb commit]; [cb waitUntilCompleted];
	Check(cb.status == MTLCommandBufferStatusCompleted, "indirect RECTLIST GPU execution failed");
	const auto* args = static_cast<const uint32_t*>(readback.contents);
	for (unsigned i=0;i<5;++i) for (unsigned w=0;w<4;++w) {
		Check(args[i*8+4+w] == records[i][w], "original RECTLIST arguments changed");
		Check(args[i*8+w] == (w==0 && records[i][0]==3 ? 4u : records[i][w]), "RECTLIST instance/base/count conversion incorrect");
	}
	for (unsigned test=0;test<targets.size();++test) {
		std::array<float,32*32*4> pixels {};
		[targets[test] getBytes:pixels.data() bytesPerRow:32*16 fromRegion:MTLRegionMake2D(0,0,32,32) mipmapLevel:0];
		if (test>=6) {
			unsigned covered=0, wrong=0;
			for (unsigned p=0;p<32*32;++p) if (pixels[p*4+3]!=0) { ++covered; wrong += pixels[p*4+2]!=0; }
			std::printf("RECTLIST packed payload mode %u: %u covered pixels, %u wrong (subnormal/signed-zero bits)\n", test==7 ? 2 : test==8 ? 1 : 3, covered, wrong);
			Check(covered>0 && wrong==0, "RECTLIST synthetic vertex corrupted packed constant attributes");
			continue;
		}
		unsigned wrong=0;
		for (unsigned y=0;y<32;++y) for (unsigned x=0;x<32;++x) {
			const auto* p=&pixels[(y*32+x)*4]; const bool empty=test==3 || test==4;
			wrong += empty ? p[0]!=0 || p[1]!=0 || p[2]!=0 || p[3]!=0 :
			    std::abs(p[0]-(x+.5f)/32)>1e-5 || std::abs(p[1]-(y+.5f)/32)>1e-5 || p[2]!=0 || p[3]!=1;
		}
		std::printf("Indirect RECTLIST case %u: %u/1024 wrong pixels%s\n", test, wrong, test ? "" : " (Triangle topology control)");
		Check(test ? wrong==0 : wrong>0, "indirect RECTLIST coverage/UV regression");
	}
}

static void PixelInterpolation() {
	NSError* error = nil;
	NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
	    "struct Out { float4 pos [[position]]; float4 param0 [[user(locn0)]]; };\n"
	    "vertex Out vs(uint id [[vertex_id]], constant uint* values [[buffer(2)]]) {\n"
	    " const float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)};\n"
	    " const float w = id == 1 ? 2.0f : 1.0f;\n"
	    " Out o; o.pos = float4(p[id]*w,0,w); o.param0 = as_type<float4>(uint4(values[id])); return o; }";
	id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
	Check(library != nil, "interpolation test vertex shader failed");
	id<MTLCommandQueue> queue = [device newCommandQueue];
	unsigned failures = 0;
	for (unsigned mode = 0; mode < 6; ++mode) {
		Fixture f(0);
		f.program.stage = ShaderType::Pixel;
		f.program.info.inputs.push_back({.kind=IR::StageInputKind::Parameter,.location=0,
		                                .component_count=4,.force_flat=mode == 2 || mode == 4});
		f.program.info.outputs.push_back({.kind=IR::StageOutputKind::Mrt,.index=0});
		f.program.export_info.push_back({.kind=IR::ExportTargetKind::Mrt,.index=0,.en=15});
		const auto value = f.Emit(0, ValueOpcode::GetAttribute, {Value(0u),Value(0u)});
		const auto rgba = f.Emit(0, ValueOpcode::CompositeConstructU32x4,
		    {value,Value(0x1234u),Value(0u),Value(1u)});
		f.Emit(0, ValueOpcode::SetAttribute, {rgba,Value(true)});
		ShaderPixelInputInfo pixel {};
		pixel.input_num = 1;
		pixel.target_integer[0] = 1;
		pixel.ps_no_perspective = mode == 1;
		if (mode == 3 || mode == 5) pixel.interpolator_settings[0] = 0x400;
		id<MTLFunction> fragment = Compile(f, &pixel);
		MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
		descriptor.vertexFunction = [library newFunctionWithName:@"vs"];
		descriptor.fragmentFunction = fragment;
		descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Uint;
		id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
		if (!pipeline) std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
		Check(pipeline != nil, "interpolation render pipeline failed");
		MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint width:8 height:8 mipmapped:NO];
		td.storageMode = MTLStorageModeShared;
		td.usage = MTLTextureUsageRenderTarget;
		id<MTLTexture> target = [device newTextureWithDescriptor:td];
		MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = target;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		const uint32_t normal[] = {std::bit_cast<uint32_t>(1.0f),std::bit_cast<uint32_t>(5.0f),std::bit_cast<uint32_t>(1.0f)};
		const uint32_t packed[] = {0x00010003u,0x00020005u,0x00030009u};
		id<MTLCommandBuffer> command = [queue commandBuffer];
		id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
		[encoder setRenderPipelineState:pipeline];
		[encoder setVertexBytes:mode >= 4 ? packed : normal length:sizeof(normal) atIndex:2];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[encoder endEncoding]; [command commit]; [command waitUntilCompleted];
		Check(command.status == MTLCommandBufferStatusCompleted, "interpolation GPU execution failed");
		uint32_t output[8 * 8 * 4] {};
		[target getBytes:output bytesPerRow:8 * 16 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
		const uint32_t actual = output[(4 * 8 + 4) * 4];
		bool ok = output[(4 * 8 + 4) * 4 + 1] == 0x1234u;
		if (mode >= 4) ok &= actual == packed[0];
		else if (mode >= 2) ok &= actual == normal[0];
		else {
			// Only vertex1 has a different W/value, so the expected result is independent of Y orientation.
			const float b = 4.5f / 16.0f;
			const float expected = mode == 1 ? 1.0f + 4.0f * b : (1.0f + 1.5f * b) / (1.0f - 0.5f * b);
			ok &= std::abs(std::bit_cast<float>(actual) - expected) < 0.00001f;
		}
		if (!ok) { ++failures; std::fprintf(stderr, "Interpolation mode%u returned 0x%08x (%g)\n", mode, actual, std::bit_cast<float>(actual)); }
	}
	std::printf("Pixel interpolation: 6 GPU cases, %u failures (perspective, linear, forced/SPI flat, packed flags)\n", failures);
	Check(failures == 0, "pixel interpolation changed flat values or ignored no-perspective mode");
}

static void LocalAppendConsume() {
    // A native wave32, with inactive lane0 in the partial-mask case.
    for (uint32_t mode = 0; mode < 3; ++mode) for (uint32_t initial : {100u, 0xfffffff0u}) {
        Fixture f;
        f.lds_dwords = 4;
        const auto handle = f.Buffer(0); const auto output_memory = f.Memory(0);
        const auto tid = f.Emit(0, ValueOpcode::GetBuiltin, {Value(uint32_t(IR::StageInputKind::LocalInvocationIndex)), Value(0u)});
        const auto first = f.Emit(0, ValueOpcode::IEqual32, {tid, Value(0u)});
        const uint64_t init_memory = f.program.memory_info.size();
        f.program.memory_info.push_back({.kind = IR::ResourceKind::Lds});
        f.Emit(0, ValueOpcode::WriteSharedU32, {Value(4u), Value(initial), first}, init_memory);
        f.Emit(0, ValueOpcode::Barrier, {});
        Value active(mode == 0);
        if (mode == 1) {
            const auto residue = f.Emit(0, ValueOpcode::BitwiseAnd32, {tid, Value(3u)});
            active = f.Emit(0, ValueOpcode::IEqual32, {residue, Value(1u)});
        }
        const uint64_t counter_memory = f.program.memory_info.size();
        f.program.memory_info.push_back({.kind = IR::ResourceKind::Lds, .offset = 4});
        const auto old = f.Emit(0, ValueOpcode::DataAppend, {Value(0u), active}, counter_memory);
        const auto byte = f.Emit(0, ValueOpcode::IMul32, {tid, Value(4u)});
        f.Emit(0, ValueOpcode::StoreBufferU32, {handle, Value(0u), byte, Value(0u), old, active}, output_memory);
        f.Emit(0, ValueOpcode::Barrier, {});
        const auto consumed = f.Emit(0, ValueOpcode::DataConsume, {Value(0u), active}, counter_memory);
        const auto consume_byte = f.Emit(0, ValueOpcode::IAdd32, {byte, Value(128u)});
        f.Emit(0, ValueOpcode::StoreBufferU32, {handle, Value(0u), consume_byte, Value(0u), consumed, active}, output_memory);
        f.Emit(0, ValueOpcode::Barrier, {});
        const auto counter = f.Emit(0, ValueOpcode::LoadSharedU32, {Value(4u), first}, init_memory);
        f.Emit(0, ValueOpcode::StoreBufferU32, {handle, Value(0u), Value(256u), Value(0u), counter, first}, output_memory);
        id<MTLFunction> function = Compile(f);
        NSError* error = nil;
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        Check(pipeline != nil, "LDS append pipeline failed");
        id<MTLBuffer> result = [device newBufferWithLength:260 options:MTLResourceStorageModeShared];
        std::memset(result.contents, 0xcd, 260);
        id<MTLArgumentEncoder> args = [function newArgumentEncoderWithBufferIndex:0];
        id<MTLBuffer> argbuf = [device newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];
        [args setArgumentBuffer:argbuf offset:0]; [args setBuffer:result offset:0 atIndex:0];
        uint32_t sizes[64] {}; sizes[0] = 260; uint32_t push[32] {};
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline]; [encoder setBuffer:argbuf offset:0 atIndex:0];
        [encoder setBytes:push length:sizeof(push) atIndex:1]; [encoder setBytes:sizes length:sizeof(sizes) atIndex:25];
        [encoder useResource:result usage:MTLResourceUsageWrite];
        [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
        Check(command.status == MTLCommandBufferStatusCompleted, "LDS append dispatch failed");
        const auto* values = static_cast<const uint32_t*>(result.contents);
        const uint32_t count = mode == 0 ? 32 : mode == 1 ? 8 : 0;
        for (uint32_t lane = 0; lane < 32; ++lane) {
            const bool enabled = mode == 0 || (mode == 1 && (lane & 3) == 1);
            Check(values[lane] == (enabled ? initial : 0xcdcdcdcdu), "LDS append old value/mask incorrect");
            Check(values[32+lane] == (enabled ? initial+count : 0xcdcdcdcdu), "LDS consume old value/mask incorrect");
        }
        Check(values[64] == initial, "LDS append/consume changed counter incorrectly");
    }
    std::puts("LDS append/consume: 6 GPU cases passed (full/partial/empty masks and wraparound)");
}

static void FloatClasses() {
    // sNaN, qNaN, -inf, -normal, -subnormal, -zero, +zero, +subnormal, +normal, +inf.
    const uint32_t bits[] = {0x7f800001u, 0xffc12345u, 0xff800000u, 0xbf800000u, 0x80000001u,
                             0x80000000u, 0u, 1u, 0x3f800000u, 0x7f800000u};
    for (uint32_t category = 0; category < 10; ++category) {
        Fixture f;
        const auto handle = f.Buffer(0); const auto memory = f.Memory(0);
        const auto value = f.Emit(0, ValueOpcode::BitCastF32U32, {Value(bits[category])});
        for (uint32_t i = 0; i < 5; ++i) {
            const uint32_t masks[] = {1u << category, 0x3ffu ^ (1u << category), 0u, 0x3ffu, 0xfffffc00u};
            const auto result = f.Emit(0, ValueOpcode::FPCmpClass32, {value, Value(masks[i])});
            f.Store(0, handle, i * 4, f.Emit(0, ValueOpcode::SelectU32, {result, Value(1u), Value(0u)}), memory);
        }
        const auto output = Run(f);
        const uint32_t expected[] = {1,0,0,1,0};
        Check(std::memcmp(output.data(), expected, sizeof(expected)) == 0, "float class mask incorrect");
    }
}

// v_cvt_pkrtz_f16_f32 rounds toward zero: finite overflow saturates to 65504 (half() would give inf), mantissas truncate.
static void PackFloat2x16Rtz() {
	const uint32_t inputs[8] = {0x49742400u /* 1e6 */, 0xc9742400u /* -1e6 */, 0x7f800000u /* inf */, 0x7fc00000u /* qNaN */,
	                            0x3f8fffffu /* 1.1249999 */, 0x35800000u /* 2^-20 */, 0x477fe000u /* 65504 */, 0x477ff000u /* 65520 */};
	const uint16_t expected[8] = {0x7bffu, 0xfbffu, 0x7c00u, 0x7e00u, 0x3c7fu, 0x0010u, 0x7bffu, 0x7bffu};
	Fixture f;
	const auto handle = f.Buffer(0);
	const auto memory = f.Memory(0);
	for (uint32_t i = 0; i < 4; ++i) {
		const auto lo = f.Emit(0, ValueOpcode::BitCastF32U32, {Value(inputs[i * 2])});
		const auto hi = f.Emit(0, ValueOpcode::BitCastF32U32, {Value(inputs[i * 2 + 1])});
		f.Store(0, handle, i * 4, f.Emit(0, ValueOpcode::PackFloat2x16Rtz, {lo, hi}), memory);
	}
	const auto output = Run(f);
	Check(std::memcmp(output.data(), expected, sizeof(expected)) == 0, "v_cvt_pkrtz_f16_f32 rounding/overflow incorrect");
	std::puts("PackFloat2x16Rtz: 8 GPU values passed (overflow saturates, inf/NaN kept, truncation, subnormal)");
}

static void Shifts64() {
	constexpr uint64_t input = 0x8123456789abcdefull;
	for (uint32_t shift : {0u, 1u, 31u, 32u, 33u, 63u, 64u, 65u, 127u}) {
		Fixture f;
		const auto handle = f.Buffer(0);
		const auto memory = f.Memory(0);
		const auto value = f.Emit(0, ValueOpcode::CompositeConstructU64, {Value(uint32_t(input)), Value(uint32_t(input >> 32))});
		const ValueOpcode ops[] = {ValueOpcode::ShiftLeftLogical64, ValueOpcode::ShiftRightLogical64, ValueOpcode::ShiftRightArithmetic64};
		for (unsigned i = 0; i < 3; ++i) {
			const auto shifted = f.Emit(0, ops[i], {value, Value(shift)});
			for (unsigned word = 0; word < 2; ++word)
				f.Store(0, handle, i * 8 + word * 4, f.Emit(0, ValueOpcode::CompositeExtractU64, {shifted, Value(word)}), memory);
		}
		const auto result = Run(f);
		const uint64_t expected[] = {input << (shift & 63), input >> (shift & 63), uint64_t(int64_t(input) >> (shift & 63))};
		for (unsigned i = 0; i < 3; ++i) {
			uint64_t actual; std::memcpy(&actual, result.data() + i * 8, 8);
			Check(actual == expected[i], "64-bit shift boundary/mask incorrect");
		}
	}
}

int main() {
	@autoreleasepool {
		try {
			device = MTLCreateSystemDefaultDevice(); Check(device != nil, "Metal device unavailable");
			ImageSampling(); PixelInterpolation(); LocalAppendConsume(); FloatClasses(); PackFloat2x16Rtz(); Shifts64(); FormattedStores(); RawBufferBounds(); DeclaredLds(); SubwordLoads(); GuestAddressLoads(); DualSourceOutput(); VertexReadFirstLane();
			IndirectRectList(); LoopPhiSwap(); DiamondPhi(true); DiamondPhi(false); NestedLoops();
			setenv("KYTY_MSL_DISPATCHER", "1", 1);
			LoopPhiSwap(); DiamondPhi(true); DiamondPhi(false); NestedLoops();
			unsetenv("KYTY_MSL_DISPATCHER");
			std::puts("Native MSL GPU tests passed (formatted stores, bounds, subword and guest address loads, dual-source blending, phi swaps, diamond, nested loops, dispatcher fallback)");
		} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
	}
}

// Match the focused typed-IR tests' assertion hooks; keep this test independent
// of the emulator runtime, FEX, and every other graphics backend.
namespace Common {
int DbgExitHandler(const char*, int, std::string_view text) { throw std::runtime_error(std::string(text)); }
int DbgExitHandler(const char*, int, fmt::text_style, std::string_view text) { throw std::runtime_error(std::string(text)); }
int DbgExitIfHandler(const char* expression, const char*, int) { throw std::runtime_error(expression); }
int DbgNotImplementedHandler(const char* expression, const char*, int) { throw std::runtime_error(expression); }
void DbgExit(int) { throw std::runtime_error("typed IR assertion failed"); }
}
