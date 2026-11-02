#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-shader-cache-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
xcrun clang++ -std=c++20 -fobjc-arc -Wl,-dead_strip \
    -I"$repo_dir/tools/test_shims" -I"$repo_dir/src" -I"$repo_dir/_Build/macos-arm64" \
    -I"$repo_dir/3rdparty/fmt/include" -I"$repo_dir/3rdparty/magic_enum/include/magic_enum" \
    -I"$repo_dir/3rdparty/SPIRV-Headers/include" -I"$repo_dir/3rdparty/Vulkan-Headers/include" -I/opt/homebrew/include \
    "$repo_dir/tests/metalShaderCacheTests.mm" "$repo_dir/src/graphics/host_gpu/metal/metalShader.mm" \
    "$repo_dir/_Build/macos-arm64/3rdparty/fmt/libfmt.a" \
    /opt/homebrew/lib/libspirv-cross-msl.a /opt/homebrew/lib/libspirv-cross-glsl.a /opt/homebrew/lib/libspirv-cross-core.a \
    -framework Metal -framework Foundation -o "$test_dir/metalShaderCacheTests"
KYTY_MSL_ONLY=1 "$test_dir/metalShaderCacheTests"
