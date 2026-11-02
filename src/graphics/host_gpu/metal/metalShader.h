#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALSHADER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALSHADER_H_

#include "common/common.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
typedef void* id;
#endif

namespace Libs::Graphics::Metal {

struct SpvcAbiLayout;

// Resource specialization changes native MSL even when the guest shader hash is unchanged.
// Failed emissions have a separate identity so they cannot resolve to a valid cached function.
[[nodiscard]] uint64_t NativeMslFunctionKey(uint64_t shader_hash, uint32_t stage,
                                         std::string_view source, bool emitted = true);

struct MetalShaderCompileResult {
#ifdef __OBJC__
	id<MTLFunction> function = nil;
	id<MTLLibrary>  library  = nil;
#else
	id              function = nullptr;
	id              library  = nullptr;
#endif
	std::string     msl_source;
	std::string     error_log;
	bool            success  = false;
};

class MetalShaderManager {
public:
	static MetalShaderManager& Instance();

	MetalShaderManager();
	~MetalShaderManager();

	KYTY_CLASS_NO_COPY(MetalShaderManager);

	// `abi` (optional): KYTY_SPVC_ABI layout from ComputeSpvcAbiLayout; when set and the flag is on,
	// every descriptor gets an explicit SPIRV-Cross argument-buffer id from the layout.
	MetalShaderCompileResult CompileSpirvToFunction(
	    std::span<const uint32_t> spirv,
	    const char* entry_point,
	    uint32_t stage,
	    uint64_t shader_hash,
	    const SpvcAbiLayout* abi = nullptr);

	// Compiles MSL produced directly by the RDNA->MSL backend (no SPIR-V).
	// shader_hash is the specialization key from NativeMslFunctionKey, not the raw guest hash.
	MetalShaderCompileResult CompileMslToFunction(
	    const std::string& msl_source,
	    const char* entry_point,
	    uint32_t stage,
	    uint64_t shader_hash);

#ifdef __OBJC__
	[[nodiscard]] id<MTLFunction> GetFunction(uint64_t hash);
#else
	[[nodiscard]] id GetFunction(uint64_t hash);
#endif

	void ClearCache();

private:
#ifdef __OBJC__
	std::unordered_map<uint64_t, id<MTLFunction>> m_function_cache;
#else
	std::unordered_map<uint64_t, id> m_function_cache;
#endif
};

// Shader hashes whose compiled MSL declares the descriptor argument buffer at [[buffer(0)]].
bool ShaderHasArgumentBuffer0(uint64_t hash);

} // namespace Libs::Graphics::Metal

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALSHADER_H_
