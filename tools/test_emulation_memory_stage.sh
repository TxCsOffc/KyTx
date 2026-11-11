#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
# Synthetic programs only. No guest executable, game assets or launcher.
for test in pm4_memory_sync memory_coherence clear_decode surface_footprint metal_guest_memory_view metal_coherence metal_tiling metal_sampler metal_viewport metal_scissor metal_pipeline metal_vertex_clip metal_msl; do
    printf '\nRunning %s\n' "$test"
    bash "$repo_dir/tools/test_${test}.sh"
done
