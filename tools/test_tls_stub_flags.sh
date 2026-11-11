#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-tls-flags.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
compiler=(clang++)
runner=()
if [[ $(uname -s) == Darwin ]]; then
    compiler=(xcrun clang++ -arch x86_64)
    runner=(arch -x86_64)
fi
"${compiler[@]}" -O2 -std=c++20 -I"$repo_dir/src" -I"$repo_dir/_Build/macos-arm64" \
    -I"$repo_dir/_Build/macos-arm64/_deps/xbyak-src" "$repo_dir/tests/TlsStubFlagsTests.cpp" -o "$test_dir/test"
"${runner[@]}" "$test_dir/test"
