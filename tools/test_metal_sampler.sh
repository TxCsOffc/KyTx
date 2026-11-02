#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-sampler-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
xcrun clang++ -std=c++20 -fobjc-arc -I"$repo_dir/src" -I"$repo_dir/_Build/macos-arm64" \
    "$repo_dir/tests/metalSamplerTests.mm" \
    "$repo_dir/src/graphics/host_gpu/metal/metalSampler.mm" \
    -framework Metal -framework Foundation -o "$test_dir/metalSamplerTests"
"$test_dir/metalSamplerTests"
