// The same IR is lowered through SPIR-V and SPIRV-Cross, as in the Vulkan /
// MoltenVK path. This is a shader-path oracle; runtime texture ownership is
// tested separately. No game shader or game resource is needed.
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvOptimizer.h"
#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
#include "graphics/host_gpu/metal/metalShaderAbi.h"
#include <spirv_cross/spirv_msl.hpp>
#include <filesystem>
#include <fstream>

// Standalone test's diagnostic sink; the runner captures stderr as its log.
namespace Log {
void WriteToConsoleAndLog(std::string_view message) { std::fwrite(message.data(),1,message.size(),stderr); }
}

static id<MTLFunction> CompileVulkanShaderPath(Fixture& fixture, Metal::SpvcAbiLayout& abi) {
	ShaderComputeInputInfo compute {}; compute.lds_size_dwords=fixture.lds_dwords;
	compute.threads_num[0]=compute.threads_num[1]=compute.threads_num[2]=1;
	compute.wave_size=compute.host_subgroup_size=32;
	ShaderStageInputInfo input {}; input.compute=&compute;
	// This fixture constructs resolved native resource handles directly. There
	// are no guest descriptor/SRT reads to discover. Run the actual remaining
	// planning and validation passes rather than bypassing the SPIR-V checks.
	const auto direct_bindings=fixture.program.bindings;
	fixture.program.srt_plan_complete=true;
	fixture.program.resource_tracking_complete=true;
	IR::CollectShaderInfo(fixture.program,input);
	IR::AllocateBindings(fixture.program);
	auto words=ShaderRecompiler::Spirv::EmitProgram(fixture.program,input,true);
	Check(!words.empty(),"reference SPIR-V emission failed");
	std::string optimizer_diagnostics;
	Check(ShaderRecompiler::Spirv::Optimize(words,Config::ShaderOptimizationType::Performance,optimizer_diagnostics,true),
	      "reference production SPIR-V optimizer/validator failed");
	abi=Metal::ComputeSpvcAbiLayout(fixture.program.bindings,fixture.program.stage);
	spirv_cross::CompilerMSL compiler(words.data(),words.size());
	const auto entries=compiler.get_entry_points_and_stages();
	Check(!entries.empty(),"reference SPIR-V has no entry point");
	auto entry=entries[0];
	compiler.rename_entry_point(entry.name,"reference_main",entry.execution_model);
	spirv_cross::CompilerMSL::Options options;
	options.set_msl_version(3,2); options.platform=spirv_cross::CompilerMSL::Options::macOS;
	options.argument_buffers=true; options.force_native_arrays=true;
	options.enable_decoration_binding=true; options.buffer_size_buffer_index=25;
	compiler.set_msl_options(options);
	for (const auto& mapping:abi.entries) {
		spirv_cross::MSLResourceBinding binding {};
		binding.stage=entry.execution_model;binding.desc_set=0;binding.binding=mapping.binding;binding.count=mapping.count;
		if (mapping.kind==IR::DescriptorBindingKind::Samplers) binding.msl_sampler=mapping.base_id;
		else if (IR::ImageBindingResourceClass(mapping.kind)!=IR::ImageResourceClass::None) binding.msl_texture=mapping.base_id;
		else binding.msl_buffer=mapping.base_id;
		compiler.add_msl_resource_binding(binding);
	}
	const std::string source=compiler.compile();
	fixture.program.bindings=direct_bindings;
	if (const char* directory=std::getenv("KYTX_SHADER_PATH_ARTIFACTS")) {
		static unsigned index=0;std::filesystem::create_directories(directory);
		const auto prefix=std::filesystem::path(directory)/("reference-"+std::to_string(index++));
		std::ofstream binary(prefix.string()+".spv",std::ios::binary);binary.write(reinterpret_cast<const char*>(words.data()),words.size()*4);
		std::ofstream(prefix.string()+".metal")<<source;
		std::ofstream(prefix.string()+"-direct.metal")<<ShaderRecompiler::Msl::EmitProgram(fixture.program,input).source;
	}
	MTLCompileOptions* metal_options=[MTLCompileOptions new];
	if (@available(macOS 15.0,*)) {
		metal_options.mathMode=MTLMathModeSafe;metal_options.mathFloatingPointFunctions=MTLMathFloatingPointFunctionsPrecise;
	} else metal_options.fastMathEnabled=NO;
	NSError* error=nil;
	id<MTLLibrary> library=[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:metal_options error:&error];
	if (!library) std::fprintf(stderr,"%s\n%s\n",error.localizedDescription.UTF8String,source.c_str());
	Check(library!=nil,"Vulkan->MSL reference library compilation failed");
	return [library newFunctionWithName:@"reference_main"];
}
