#!/bin/bash
# usage: tools/run_fc27.sh <seconds> [VAR=val ...]   — runs FC 27 (PPSA34015) under the Metal backend for <seconds>.
# Extra VAR=val arguments are passed as environment (diagnostics, frame dumps). Log to stdout/stderr: redirect it.
secs=${1:-60}; shift
cd /Users/tx/Developer/fex-spike/runs
env KYTY_MSL_ONLY=1 KYTY_BACKEND=metal KYTY_VIDEO_OUT_1080P=1 KYTY_NP_STATE=signed_in KYTY_LABEL_FLUSH=lazy \
    KYTY_WAIT_RETRY_ELIDE=1 KYTY_WAIT_BY_ORDER=1 KYTY_UMA_READBACK=1 "$@" \
    timeout "$secs" /Users/tx/Developer/KyTx/_Build/macos-arm64/kyty_emulator --game /Users/tx/Developer/PPSA34015
