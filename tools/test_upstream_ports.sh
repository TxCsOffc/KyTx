#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
build_dir=${KYTX_BUILD_DIR:-"$repo_dir/_Build/macos-arm64"}
cmake --build "$build_dir" --target kyty_emulator kernel_file_system_tests virtual_memory_allocation_tests sync_on_address_tests -j8
if [[ $(uname -s) == Darwin && -f "$build_dir/libMoltenVK.dylib" ]]; then
    export SDL_VULKAN_LIBRARY=${SDL_VULKAN_LIBRARY:-"$build_dir/libMoltenVK.dylib"}
fi
"$build_dir/kernel_file_system_tests" --port-contracts-only
"$build_dir/virtual_memory_allocation_tests" --mlock-only
"$build_dir/sync_on_address_tests"
bash "$repo_dir/tools/test_tls_stub_flags.sh"
bash "$repo_dir/tools/test_emulation_memory_stage.sh"
