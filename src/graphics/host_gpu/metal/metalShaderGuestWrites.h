#pragma once
#include "graphics/host_gpu/metal/metalGuestWrites.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::Metal::GuestWrites {
// uses_dma means a global address table is bound, including for loads. Only
// address stores have an unknown physical target; V# stores/atomics remain bounded.
inline void NoteShaderWrites(const ShaderRecompiler::IR::CompiledShaderInfo& program,
                      const ShaderRecompiler::IR::ResourceSnapshot& resources) {
	if (program.has_address_writes) {
		NoteUnknownWrite();
		return;
	}
	for (size_t i=0;i<resources.buffers.size() && i<program.info.buffers.size();++i) {
		const auto& buffer=program.info.buffers[i];
		if (!buffer.written && !buffer.atomic) continue;
		const auto& d=resources.buffers[i].dwords;
		const uint64_t address=(uint64_t(d[1] & 0xffffu)<<32)|d[0];
		const uint32_t stride=(d[1]>>16)&0x3fffu;
		const uint64_t bytes=std::min<uint64_t>(uint64_t(d[2])*std::max(stride,1u),UINT32_MAX);
		if (address && bytes) NoteWrite(address,bytes);
	}
}

} // namespace Libs::Graphics::Metal::GuestWrites
