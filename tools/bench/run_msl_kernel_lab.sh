#!/bin/bash
# Builds and runs the MSL pipeline lab (see msl_kernel_lab.mm). Does not start the emulator. Needs only Xcode command line tools.
#
#   tools/bench/run_msl_kernel_lab.sh <hash|file.metal> [lab options...]
#
# A bare hash (0x...metal is looked up in the MSL dump directories). Runs under nice so the machine stays usable; one file per call.
# MSL_DUMP_DIRS (colon separated) overrides the search path.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/kyty_msl_kernel_lab"
if [ ! -x "$OUT" ] || [ "$HERE/msl_kernel_lab.mm" -nt "$OUT" ]; then
	xcrun clang++ -std=c++20 -O2 -fobjc-arc -Wno-deprecated-declarations -framework Metal -framework Foundation "$HERE/msl_kernel_lab.mm" -o "$OUT"
fi
target="$1"
shift || true
if [ ! -f "$target" ]; then
	h="${target#0x}"
	h="${h%.metal}"
	dirs="${MSL_DUMP_DIRS:-$ROOT/.porting/work/out/r7/msl}"
	found=""
	IFS=':' read -ra arr <<<"$dirs"
	for d in "${arr[@]}"; do
		[ -f "$d/0x$h.metal" ] && found="$d/0x$h.metal" && break
	done
	if [ -z "$found" ]; then
		for d in $(ls -dt "$ROOT"/.porting/artifacts/2026-11-01/*-msl 2>/dev/null); do
			[ -f "$d/0x$h.metal" ] && found="$d/0x$h.metal" && break
		done
	fi
	[ -n "$found" ] || { echo "no dump of $h found" >&2; exit 1; }
	target="$found"
fi
exec nice -n 10 "$OUT" "$@" "$target"
