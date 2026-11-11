#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-coherence-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
xcrun clang++ -O2 -std=c++20 -I"$repo_dir/src" "$repo_dir/tests/MemoryCoherenceTests.cpp" -o "$test_dir/test"
"$test_dir/test"
