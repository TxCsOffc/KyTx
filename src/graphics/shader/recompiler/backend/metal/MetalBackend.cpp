#include "graphics/shader/recompiler/backend/metal/MetalBackend.h"

#include "graphics/shader/recompiler/backend/msl/MslEmitter.h"

#include <cstdlib>
#include <string_view>
#include <xxhash.h>

namespace Libs::Graphics::ShaderRecompiler::MetalBackend {
namespace {

class FastMslBackend final : public IMetalShaderBackend {
public:
	[[nodiscard]] MetalBackendKind Kind() const noexcept override {
		return MetalBackendKind::FastMsl;
	}

	[[nodiscard]] MetalEmitResult Emit(const IR::Program&      program,
	                                   const MetalEmitOptions& options) const override {
		auto emitted = Msl::EmitProgram(program, options.input_info);
		MetalEmitResult result;
		result.kind        = MetalBackendKind::FastMsl;
		result.msl_source  = std::move(emitted.source);
		result.unsupported = std::move(emitted.unsupported);
		result.wave64_pair = emitted.wave64_pair;
		result.success     = emitted.success;
		result.content_hash =
		    result.msl_source.empty()
		        ? 0ull
		        : XXH3_64bits(result.msl_source.data(), result.msl_source.size());
		return result;
	}
};

class AirBitcodeBackendStub final : public IMetalShaderBackend {
public:
	[[nodiscard]] MetalBackendKind Kind() const noexcept override {
		return MetalBackendKind::AirBitcode;
	}

	[[nodiscard]] MetalEmitResult Emit(const IR::Program&      program,
	                                   const MetalEmitOptions& options) const override {
		(void)program;
		(void)options;
		MetalEmitResult result;
		result.kind    = MetalBackendKind::AirBitcode;
		result.success = false;
		result.unsupported.emplace_back(
		    "Direct AIR bitcode backend is not linked; use FastMsl backend");
		return result;
	}
};

} // namespace

MetalBackendKind SelectedMetalBackendKind() {
	static const MetalBackendKind s_kind = [] {
		const char* env = std::getenv("KYTY_METAL_BACKEND");
		if (env != nullptr && std::string_view(env) == "air") {
			return MetalBackendKind::AirBitcode;
		}
		return MetalBackendKind::FastMsl;
	}();
	return s_kind;
}

const IMetalShaderBackend& GetMetalShaderBackend(MetalBackendKind kind) {
	static const FastMslBackend        s_fast_msl;
	static const AirBitcodeBackendStub s_air_stub;
	return kind == MetalBackendKind::AirBitcode
	           ? static_cast<const IMetalShaderBackend&>(s_air_stub)
	           : static_cast<const IMetalShaderBackend&>(s_fast_msl);
}

} // namespace Libs::Graphics::ShaderRecompiler::MetalBackend
