#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-guest-view-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
xcrun clang++ -std=c++20 -fobjc-arc -I"$repo_dir/src" -I"$repo_dir/_Build/macos-arm64" \
    -I"$repo_dir/3rdparty/fmt/include" "$repo_dir/tests/metalGuestMemoryViewTests.mm" \
    -framework Metal -framework Foundation -o "$test_dir/test"
"$test_dir/test"
