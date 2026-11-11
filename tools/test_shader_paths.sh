#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-shader-paths.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
ir_dir="$repo_dir/src/graphics/shader/recompiler/ir"
spirv_dir="$repo_dir/src/graphics/shader/recompiler/backend/spirv"
spirv_sources=()
for source in "$spirv_dir"/*.cpp; do
    # Include the actual optimizer and its target-specific validation as well.
    spirv_sources+=("$source")
done
xcrun clang++ -std=c++20 -fobjc-arc -DKYTX_COMPARE_SPIRV_PATH=1 \
    -I"$repo_dir/src" -I"$repo_dir/_Build/macos-arm64" -I/opt/homebrew/include \
    -I"$repo_dir/3rdparty/fmt/include" -I"$repo_dir/3rdparty/magic_enum/include/magic_enum" \
    "$repo_dir/tests/metalMslTests.mm" \
    "$repo_dir/src/graphics/shader/recompiler/backend/msl/MslEmitter.cpp" \
    "$repo_dir/src/graphics/host_gpu/metal/metalIndirect.mm" \
    "$repo_dir/src/graphics/host_gpu/metal/metalSampler.mm" \
    "$repo_dir/src/graphics/shader/recompiler/frontend/decode/ImageOps.cpp" \
    "$repo_dir/src/graphics/shader/shaderPixelParameter.cpp" \
    "$repo_dir/src/graphics/guest_gpu/gpu_format.cpp" \
    "${spirv_sources[@]}" \
    "$ir_dir/Block.cpp" "$ir_dir/Program.cpp" "$ir_dir/Type.cpp" "$ir_dir/Value.cpp" \
    "$ir_dir/opcodes/ValueOpcodes.cpp" "$ir_dir/passes/BindingLayout.cpp" \
    "$ir_dir/passes/ShaderInfoCollection.cpp" \
    "$repo_dir/_Build/macos-arm64/3rdparty/fmt/libfmt.a" \
    "$repo_dir/_Build/macos-arm64/3rdparty/SPIRV-Tools/source/opt/libSPIRV-Tools-opt.a" \
    "$repo_dir/_Build/macos-arm64/3rdparty/SPIRV-Tools/source/libSPIRV-Tools.a" \
    "$repo_dir/_Build/macos-arm64/CMakeFiles/kyty_emulator.dir/src/graphics/host_gpu/renderer/cache/bufferCache.cpp.o" \
    -L/opt/homebrew/lib -lspirv-cross-msl -lspirv-cross-glsl -lspirv-cross-core \
    -Wl,-dead_strip -framework Metal -framework Foundation -o "$test_dir/test"
KYTX_MSL_COMPARE_ONLY=1 "$test_dir/test"
