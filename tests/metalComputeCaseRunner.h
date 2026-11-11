#pragma once
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"
#include <string>
#include <vector>
namespace KytxMetalCases {
// Emits MSL for `program`, compiles it, dispatches it once and copies results back into `buffer` (and `gds`).
// Returns false and fills `error` on any failure (emission, MSL compile, pipeline, GPU error).
bool Run(const Libs::Graphics::ShaderRecompiler::IR::Program& program,
         const Libs::Graphics::ShaderComputeInputInfo& compute_info,   // already has host_subgroup_size = 32
         const Libs::Graphics::ShaderRecompiler::IR::PushData& push,
         std::vector<uint32_t>& buffer, uint32_t buffer_bytes,          // contents in/out; bytes bound to every buffer slot
         std::vector<uint32_t>* gds,                                    // nullptr when the program has no GDS binding
         uint32_t groups_x, uint32_t groups_y, uint32_t groups_z, std::string& error);
}
