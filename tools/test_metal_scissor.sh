#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
build_dir="$repo_dir/_Build/macos-arm64"
cmake --build "$build_dir" --target kyty_emulator -j6
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kytx-scissor.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
xcrun clang++ -std=c++20 -fobjc-arc -I"$repo_dir/src" -I"$build_dir" -I/opt/homebrew/include \
    -I"$repo_dir/3rdparty/fmt/include" -I"$repo_dir/3rdparty/xxhash" \
    "$repo_dir/tests/metalScissorTests.mm" \
    "$build_dir/CMakeFiles/kyty_emulator.dir/src/graphics/host_gpu/renderer/debug.cpp.o" \
    "$build_dir/3rdparty/fmt/libfmt.a" -Wl,-dead_strip -framework Metal -framework Foundation \
    -o "$test_dir/metalScissorTests"
"$test_dir/metalScissorTests" "$@"
