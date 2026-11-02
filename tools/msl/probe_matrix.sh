#!/bin/bash
# Probe matrix: runs each configuration back-to-back with a cold MSL cache, dumps presented frames
# 20/100/600, and prints one row per configuration (non-zero ratio and mean colour per frame).
# Usage: tools/msl/probe_matrix.sh [config-index ...]   (no args = all configurations)
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO/_Build/macos-arm64/kyty_emulator"
RUNS="/Users/tx/Developer/fex-spike/runs"
OUT="$REPO/tools/msl/out"
RUN_SECONDS="${RUN_SECONDS:-80}"
HASH_PS=0x724318af5ff62412   # flip-20 candidate pixel shader
HASH_VS=0x3d28ce5308b9b56a   # its vertex shader
BASE="KYTY_BACKEND=metal KYTY_FORCE_RT=1 KYTY_VIDEO_OUT_1080P=1 KYTY_NP_STATE=signed_in KYTY_LABEL_FLUSH=lazy KYTY_WAIT_RETRY_ELIDE=1 KYTY_WAIT_BY_ORDER=1 KYTY_UMA_READBACK=1 KYTY_MSL_NO_DISK_CACHE=1"
CONFIGS=(
  "baseline|"
  "argenc|KYTY_ARGENC_GFX=1"
  "no_cull|KYTY_NO_CULL=1"
  "no_depth|KYTY_NO_DEPTH=1"
  "vs_pos|KYTY_VS_PROBE=pos"
  "vs_raw|"
  "ps_red|KYTY_PS_PROBE=red KYTY_PROBE_HASH=$HASH_PS"
  "ps_tex|KYTY_PS_PROBE=tex KYTY_PROBE_HASH=$HASH_PS"
  "ps_uv|KYTY_PS_PROBE=uv KYTY_PROBE_HASH=$HASH_PS"
  "ps_cb|KYTY_PS_PROBE=cb KYTY_PROBE_HASH=$HASH_PS"
  "msl_direct2|KYTY_MSL_DIRECT=2"
)
mkdir -p "$OUT"
SEL=("$@"); [ ${#SEL[@]} -eq 0 ] && SEL=($(seq 0 $((${#CONFIGS[@]} - 1))))
for i in "${SEL[@]}"; do
  IFS='|' read -r name extra <<< "${CONFIGS[$i]}"
  dir="$OUT/$name"; rm -rf "$dir"; mkdir -p "$dir"
  # Shared environment; the config's extra variables last.
  vsx=""; [[ $name == vs_* ]] && vsx="KYTY_VS_DBG=$HASH_VS KYTY_TRACE_FRAMES=20:21 KYTY_RT_AFTER=$HASH_PS KYTY_RT_AFTER_N=1"
  env $BASE $extra $vsx KYTY_DUMP_FRAME=20,100,600 KYTY_DUMP_PATH="$dir/f_%llu.ppm" \
      "$BIN" --game /Users/tx/Developer/PPSA34015 > "$dir/run.log" 2>&1 &
  pid=$!
  sleep "$RUN_SECONDS"
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  # Per-frame stats (non-zero %, mean R G B) from the dumps.
  stats=""
  for f in 20 100 600; do
    if [ -f "$dir/f_$f.ppm" ]; then
      row=$(python3 -I "$REPO/tools/msl/rt_stats.py" "$dir" 2>/dev/null | grep "f_$f.ppm" | awk '{print $1"%/"$2","$3","$4}')
      stats="$stats f$f=[${row:-n/a}]"
    else
      stats="$stats f$f=[no-dump]"
    fi
  done
  vsrows=$(grep -ac 'VsRow\]' "$dir/run.log")
  asserts=$(grep -ac 'failed assertion' "$dir/run.log")
  fps=$(grep -aE '^\[rate\]' "$dir/run.log" | awk '{print $2}' | tail -1)
  printf '%-12s %-60s vsrows=%s asserts=%s %s\n' "$name" "$stats" "$vsrows" "$asserts" "$fps" | tee -a "$OUT/summary.txt"
done
