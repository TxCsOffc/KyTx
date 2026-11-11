#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-viewport.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
xcrun clang++ -std=c++20 -fobjc-arc -I"$repo_dir/src" "$repo_dir/tests/metalViewportTests.mm" -framework Metal -framework Foundation -o "$test_dir/metalViewportTests"
"$test_dir/metalViewportTests"
