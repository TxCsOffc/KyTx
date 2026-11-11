#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-clear-decode.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
xcrun clang++ -std=c++20 -fobjc-arc -I"$repo_dir/src" "$repo_dir/tests/metalClearDecodeTests.mm" -framework Metal -framework Foundation -o "$test_dir/test"
"$test_dir/test"
