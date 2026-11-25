#!/bin/bash
set -euo pipefail

source_dir=$(cd "$(dirname "$0")/.." && pwd)
fex_dir=${KYTY_FEX_DIR:-"$(dirname "$source_dir")/fexcore"}
build_root=${1:-"$(dirname "$source_dir")/build"}
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-4}
fex_revision=eee10cc54a173e79ee16e2169cd4157c7269921b
fex_patch="$source_dir/docs/bringup/fexcore-optional-snapshot.patch"

if [[ $(uname -s) != Darwin || $(uname -m) != arm64 ]]; then
    echo "Use a native arm64 shell on macOS." >&2
    exit 1
fi
if [[ $(git -C "$fex_dir" rev-parse HEAD 2>/dev/null) != "$fex_revision" ]]; then
    echo "The build requires FEXCore $fex_revision at $fex_dir." >&2
    exit 1
fi
if ! git -C "$fex_dir" apply --reverse --check --unidiff-zero "$fex_patch" 2>/dev/null; then
    git -C "$fex_dir" apply --check --unidiff-zero "$fex_patch"
    git -C "$fex_dir" apply --unidiff-zero "$fex_patch"
fi

xcodebuild -version
cmake --version
ninja --version
git -C "$source_dir" rev-parse HEAD
git -C "$source_dir" submodule update --init
mkdir -p "$build_root"
build_root=$(cd "$build_root" && pwd)
fex_dir=$(cd "$fex_dir" && pwd)

cmake -S "$fex_dir" -B "$build_root/fex" -G Ninja \
    -DBUILD_FEXCORE_ONLY=ON -DBUILD_FEXCORE_SMOKE=OFF -DBUILD_TESTING=OFF \
    -DTUNE_CPU=none -DENABLE_LTO=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$build_root/fex" --parallel "$jobs"

cmake -S "$source_dir" -B "$build_root/kytx" -G Ninja \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release \
    -DKYTY_BUILD_LAUNCHER=OFF -DKYTY_FEX_DIR="$fex_dir" \
    -DKYTY_FEX_BUILD_DIR="$build_root/fex" -DKYTY_FEX_SHIMS_DIR="$fex_dir"
cmake --build "$build_root/kytx" --target kyty_emulator --parallel "$jobs"
file "$build_root/kytx/kyty_emulator"
codesign --verify "$build_root/kytx/kyty_emulator"
