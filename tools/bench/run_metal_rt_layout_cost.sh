#!/bin/bash
# Builds and runs the render-target layout mini benchmark (see metal_rt_layout_cost.mm). Does not start the game.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/kyty_metal_rt_layout_cost"
xcrun clang++ -std=c++20 -O2 -fobjc-arc -framework Metal -framework Foundation "$HERE/metal_rt_layout_cost.mm" -o "$OUT"
"$OUT"
