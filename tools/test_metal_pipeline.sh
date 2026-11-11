#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-pipeline-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
xcrun clang++ -std=c++20 -fobjc-arc -Wl,-dead_strip \
    -I"$repo_dir/tools/test_shims" -I"$repo_dir/src" -I"$repo_dir/_Build/macos-arm64" \
    -I"$repo_dir/3rdparty/fmt/include" -I"$repo_dir/3rdparty/magic_enum/include/magic_enum" \
    -I"$repo_dir/3rdparty/Vulkan-Headers/include" -I/opt/homebrew/include \
    "$repo_dir/tests/metalPipelineTests.mm" "$repo_dir/src/graphics/host_gpu/metal/metalPipeline.mm" \
    "$repo_dir/src/graphics/host_gpu/metal/metalDraw.mm" \
    "$repo_dir/src/graphics/host_gpu/metal/metalPassTimer.mm" \
    "$repo_dir/src/graphics/host_gpu/metal/metalIndirect.mm" \
    "$repo_dir/tools/test_shims/guestWritesLink.mm" \
    "$repo_dir/src/graphics/host_gpu/metal/metalTiling.mm" \
    "$repo_dir/src/graphics/guest_gpu/tile.cpp" \
    "$repo_dir/src/graphics/guest_gpu/gpu_format.cpp" \
    "$repo_dir/_Build/macos-arm64/CMakeFiles/kyty_emulator.dir/src/graphics/host_gpu/renderer/debug.cpp.o" \
    "$repo_dir/_Build/macos-arm64/3rdparty/fmt/libfmt.a" \
    -framework Metal -framework Foundation -o "$test_dir/metalPipelineTests"
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 "$test_dir/metalPipelineTests" ${KYTX_PIPELINE_TEST_ARGS:-}
KYTY_MTL_SPLIT_DRAWS=1 KYTY_MTL_SYNC_EACH_FROM=0 \
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 "$test_dir/metalPipelineTests" --multi-draw

MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 "$test_dir/metalPipelineTests" --mesh

MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 "$test_dir/metalPipelineTests" --mip

MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 "$test_dir/metalPipelineTests" --depth-memory

MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 "$test_dir/metalPipelineTests" --depth-bounds
