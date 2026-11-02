#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-buffer-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
xcrun clang++ -std=c++20 -fobjc-arc -I"$repo_dir/tools/test_shims" -I"$repo_dir/src" \
    -I"$repo_dir/_Build/macos-arm64" -I"$repo_dir/3rdparty/fmt/include" \
    "$repo_dir/tests/metalBufferTests.mm" "$repo_dir/src/graphics/host_gpu/metal/metalBuffer.mm" \
    "$repo_dir/_Build/macos-arm64/3rdparty/fmt/libfmt.a" \
    -framework Metal -framework Foundation -o "$test_dir/metalBufferTests"
"$test_dir/metalBufferTests"
