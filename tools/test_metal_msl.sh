#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-msl-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
ir_dir="$repo_dir/src/graphics/shader/recompiler/ir"
xcrun clang++ -std=c++20 -fobjc-arc -I"$repo_dir/src" -I"$repo_dir/_Build/macos-arm64" \
    -I"$repo_dir/3rdparty/fmt/include" -I"$repo_dir/3rdparty/magic_enum/include/magic_enum" \
    "$repo_dir/tests/metalMslTests.mm" \
    "$repo_dir/src/graphics/shader/recompiler/backend/msl/MslEmitter.cpp" \
    "$repo_dir/src/graphics/host_gpu/metal/metalIndirect.mm" \
    "$repo_dir/src/graphics/shader/recompiler/frontend/decode/ImageOps.cpp" \
    "$repo_dir/src/graphics/shader/shaderPixelParameter.cpp" \
    "$ir_dir/Block.cpp" "$ir_dir/Program.cpp" "$ir_dir/Type.cpp" "$ir_dir/Value.cpp" \
    "$ir_dir/opcodes/ValueOpcodes.cpp" "$ir_dir/passes/BindingLayout.cpp" \
    "$repo_dir/_Build/macos-arm64/3rdparty/fmt/libfmt.a" \
    -Wl,-dead_strip -framework Metal -framework Foundation -o "$test_dir/metalMslTests"
"$test_dir/metalMslTests"
