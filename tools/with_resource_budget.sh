#!/bin/bash
# Host policy only: no guest-memory, shader-work or emulation changes.
# Keep at least 10% of physical RAM outside this process; never exceed 40 GiB.
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
# Keep abnormal/unfinished sessions blocked until their cause is reviewed.
# The verified native backend below enforces the average GPU duty budget.
if [ "${1:-}" != '--print-limit-mib' ] && [ -f "$repo_dir/.porting/work/HOST_GPU_TESTS_BLOCKED" ]; then
    echo 'GPU launches blocked: previous GPU session/failure must be reviewed before restart.' >&2
    exit 75
fi
requested=${FPMAX:-40}
case "$requested" in
    [1-9]|[1-9][0-9]) ;;
    *) echo 'FPMAX must be a positive integer number of GiB (1..40).' >&2; exit 2 ;;
esac
if [ "$requested" -gt 40 ]; then
    echo "FPMAX=$requested exceeds the user budget; using 40 GiB." >&2
    requested=40
fi
physical_bytes=$(/usr/sbin/sysctl -n hw.memsize)
limit_mib=$((physical_bytes / 1048576 * 9 / 10))
if [ "$limit_mib" -gt $((requested * 1024)) ]; then
    limit_mib=$((requested * 1024))
fi
if [ "${1:-}" = '--print-limit-mib' ]; then
    echo "$limit_mib"
    exit 0
fi
if [ "$#" -eq 0 ]; then
    echo 'usage: bash tools/with_resource_budget.sh <program> [arguments...]' >&2
    exit 2
fi
verified="$repo_dir/.porting/work/HOST_GPU_BUDGET_VERIFIED.sha256"
if [ ! -f "$verified" ] || [ ! -f "$1" ]; then
    echo 'No verified budgeted emulator binary; refusing GPU launch.' >&2
    exit 75
fi
IFS= read -r expected_sha < "$verified"
actual_sha=$(/usr/bin/shasum -a 256 -- "$1")
actual_sha=${actual_sha%% *}
if [ "$actual_sha" != "$expected_sha" ]; then
    echo 'Emulator hash differs from the verified budgeted build; refusing launch.' >&2
    exit 75
fi
export KYTY_GPU_BUDGET_PERCENT=${KYTY_GPU_BUDGET_PERCENT:-90}
case "$KYTY_GPU_BUDGET_PERCENT" in
    [1-9]|[1-8][0-9]|90) ;;
    *) echo 'GPU budget must be 1..90 percent.' >&2; exit 75 ;;
esac
export KYTY_HOST_MEMORY_MIB=$limit_mib
export KYTY_HOST_FAILURE_FILE="$repo_dir/.porting/work/HOST_GPU_TESTS_BLOCKED"
echo "[HostBudget] memory budget=${limit_mib} MiB; GPU average duty<=${KYTY_GPU_BUDGET_PERCENT}%; QoS=utility; nice=10" >&2
# SETEXEC keeps the PID across taskpolicy/nice/the target. This memory
# policy is not a verified fatal allocation limit; run1.sh separately monitors
# process footprint. The verified native backend additionally paces GPU packets
# and samples its own process footprint every 100ms.
# QoS/nice let interactive macOS work take precedence; they are not a CPU/GPU
# percentage cap. The GPU cap above is an average duty cycle, not kernel preemption.
exec /usr/sbin/taskpolicy -m "$limit_mib" -c utility /usr/bin/nice -n 10 "$@"
