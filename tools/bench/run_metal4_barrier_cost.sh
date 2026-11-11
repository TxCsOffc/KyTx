#!/bin/bash
# Builds and runs the Metal 4 barrier-cost mini benchmark (see metal4_barrier_cost.mm). Needs Xcode command line tools with the macOS 26+ SDK;
# does not start the game. Runs at low CPU priority so it does not disturb a measuring emulator on the same Mac.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/kyty_metal4_barrier_cost"
xcrun clang++ -std=c++20 -O2 -fobjc-arc -framework Metal -framework Foundation "$HERE/metal4_barrier_cost.mm" -o "$OUT"
nice -n 10 "$OUT"
