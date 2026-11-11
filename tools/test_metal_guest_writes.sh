#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-guest-writes.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
xcrun clang++ -O1 -std=c++20 -fobjc-arc -I"$repo_dir/tools/test_shims" -I"$repo_dir/src" -I"$repo_dir/_Build/macos-arm64" -I"$repo_dir/3rdparty/fmt/include" -I"$repo_dir/3rdparty/magic_enum/include/magic_enum" \
    "$repo_dir/tests/metalGuestWritesTests.mm" -framework Foundation -o "$test_dir/test"
"$test_dir/test"
