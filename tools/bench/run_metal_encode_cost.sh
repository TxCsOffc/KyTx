#!/bin/bash
# Builds and runs the Metal encode-cost mini benchmark (see metal_encode_cost.mm). Needs only Xcode command line tools; does not start the game.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/kyty_metal_encode_cost"
xcrun clang++ -std=c++20 -O2 -fobjc-arc -framework Metal -framework Foundation "$HERE/metal_encode_cost.mm" -o "$OUT"
"$OUT"
