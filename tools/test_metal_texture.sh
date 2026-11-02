#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-texture-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
xcrun clang++ -std=c++20 -fobjc-arc -I"$repo_dir/tools/test_shims" -I"$repo_dir/src" \
    -I"$repo_dir/_Build/macos-arm64" -I"$repo_dir/3rdparty/fmt/include" \
    "$repo_dir/tests/metalTextureTests.mm" "$repo_dir/src/graphics/host_gpu/metal/metalTexture.mm" \
    "$repo_dir/src/graphics/guest_gpu/gpu_format.cpp" \
    "$repo_dir/_Build/macos-arm64/3rdparty/fmt/libfmt.a" \
    -framework Metal -framework Foundation -o "$test_dir/metalTextureTests"
"$test_dir/metalTextureTests"
