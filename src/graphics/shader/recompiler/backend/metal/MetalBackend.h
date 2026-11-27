#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::MetalBackend {

enum class MetalBackendKind : uint8_t {
	FastMsl    = 0,
	AirBitcode = 1,
};

struct MetalEmitOptions {
	ShaderStageInputInfo input_info;
	bool                 prefer_fast_math = false;
};

struct MetalEmitResult {
	std::string              msl_source;
	std::vector<uint8_t>     air_bitcode;
	std::vector<std::string> unsupported;
	uint64_t                 content_hash = 0;
	bool                     wave64_pair  = false;
	bool                     success      = false;
	MetalBackendKind         kind         = MetalBackendKind::FastMsl;
};

// Pluggable interface allowing RDNA2/gfx1013 IR to lower either to Fast MSL (with
// MTLBinaryArchive caching) or a future direct LLVM/AIR bitcode emitter.
class IMetalShaderBackend {
public:
	virtual ~IMetalShaderBackend() = default;

	[[nodiscard]] virtual MetalBackendKind Kind() const noexcept = 0;
	[[nodiscard]] virtual MetalEmitResult  Emit(const IR::Program&      program,
	                                            const MetalEmitOptions& options) const = 0;
};

[[nodiscard]] MetalBackendKind           SelectedMetalBackendKind();
[[nodiscard]] const IMetalShaderBackend& GetMetalShaderBackend(MetalBackendKind kind);

} // namespace Libs::Graphics::ShaderRecompiler::MetalBackend
