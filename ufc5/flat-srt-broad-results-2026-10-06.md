# Broad FlatSRT fight results — 2026-10-06

## Result

The broad candidate reduced measured whole-materializer CPU elapsed per call by
about **20%**, with **20,517 complete comparisons and zero mismatches**. The user
confirmed that the paused fight looked the same with the candidate on. This has
**not demonstrated a reliable FPS improvement**. The candidate remains default off
and the live controls were restored to `off all`, with heavy GPU profiling disarmed.

The selected off/shadow/on/off window lasted 365.720 seconds in process 7600,
using `D:/PS5/ufc5-profiles/production-20261006-083333.csv`. No crash was observed.
367 shader hashes were present in both off/on; 354 actually executed fast calls.
Count and elapsed categories reconcile; no incomplete/invalid rows were skipped.

## CPU cost and actual coverage

Stable aggregate rows omit short mode-transition windows. Summary rows already
contain their shader rows: do not add both populations.

| Mode | Materializations | Whole CPU elapsed (ms) | Weighted us/call | Fast calls | Comparison calls |
|---|---:|---:|---:|---:|---:|
| Off, pooled | 1,883,642 | 17,697.222 | 9.395 | 0 | 0 |
| Shadow | 1,090,706 | 10,674.654 | 9.787 | 0 | 16,724 |
| On | 2,050,713 | 15,441.742 | 7.530 | 1,975,559 | 3,780 |

On executed recipes in 96.34% of counted materializations, totaling 125,805,705
actual recipe evaluations. 71,374 unsupported/no-recipe calls stayed reference;
they are not failed validation. On CPU cost includes 15,280.796 ms fast,
44.773 ms reference and 116.174 ms periodic comparison (about 0.75% of on CPU).
Full comparison totals also include transition rows, hence 20,517 overall.

Normalizing the on per-hash means to the exact off call population gives
14,178.503 ms versus the measured off 17,697.222 ms: **19.88% lower**. This is a
population-normalized projection, not measured recovered frame wall time.
Initial-off weighted elapsed was 9.321 us/call, return-off 9.452 us/call.
Per-call timers include observer/preemption effects; control polling, map lookup
and CSV output are outside the bracket. Hashes can combine static variants.

## Presentation cadence

These rates count successful host presentation calls over approximately ten-second
windows. They are not title FPS or exact displayed-frame counts. The first report
after each stage marker is omitted because it can span the preceding mode.

| Stage | Duration (s) | Samples | Median presents/s | Range | Weighted presents/s |
|---|---:|---:|---:|---:|---:|
| Initial off | 55.397 | 5 | 4.819 | 2.512–5.337 | 4.443 |
| Shadow | 90.715 | 8 | 2.525 | 2.409–5.261 | 2.928 |
| On | 150.426 | 13 | 2.711 | 2.486–4.884 | 3.381 |
| Return off | 69.182 | 6 | 2.815 | 2.472–4.463 | 3.053 |

On was below initial off, while return off stayed similarly low. The stages drift
substantially; they do not establish a repeatable mode-dependent improvement or
slowdown. The local CPU reduction does not explain the missing frame budget.

## Verification and artifacts

Build, materialization/controller tests, nine production analyzer tests and two
FlatSRT analyzer tests passed. Eight saved fight detiles still reference MATCH,
0.082–0.477 ms isolated. These replay latencies are separate from production waits.

Frozen data, analysis and stage markers:

- `D:/PS5/ufc5-profiles/production-20261006-083333.csv.flat-first-attempt.csv`
- `D:/PS5/ufc5-profiles/production-20261006-083333.csv.flat-first-attempt.json`
- `D:/PS5/ufc5-profiles/production-20261006-083333.csv.flat-stages.jsonl`
- `D:/PS5/ufc5-flat-srt-broad-replays.csv`

Regenerate with `ufc5/tools/summarize-flat-srt-stages.py` and that production prefix.
Only the broad candidate's visual check is confirmed here; this does not replace
the previously pending visual check for session 44's separate run.

## Next measurement: OS GPU packet and CPU scheduling trace

**Subsequent session 46:** the OS trace has now been recorded and analyzed.
Read the [GPUView report](gpuview-report-2026-10-06.md). The measurement preparation
below records session 45's state; a Firefox-closed repeat is now the next A/B.

The larger unresolved synchronous readback waits still dominate the measured wait
budget. Prior traces attribute 99.48% of readback waits to CMask buffers
`1164b80000`, `1167f00000` and indirect argument buffer `1140008000`. These are
whole submitted-batch completion waits, not proof that copying those bytes is slow.
See [measurement follow-up](measurement-follow-up-2026-10-06.md) for scope limits.

GPUView and its Microsoft GPU/CPU scheduling profile are installed locally. The
prepared [capture helper](tools/capture-gpuview.ps1) takes a bounded baseline trace
without restarting Kyty. It records executable/profile hashes, PID, UTC/QPC
boundaries and presentation-log offsets. It checks that experiment/heavy controls
are off and refuses an existing WPR recording. Start/stop use a unique instance.

Windows kernel/GPU tracing requires Administrator (the installed GPUView README
also requires this). The agent's current process is not elevated. Profile parsing
and helper preflight passed; **no OS trace has been recorded yet**. Run from an
Administrator PowerShell while the same paused fight remains visible:

```powershell
& 'D:\PS5\src\KytyPS5\ufc5\tools\capture-gpuview.ps1' -Seconds 30
```

The next analysis must check lost events first, then distinguish CPU submission
gaps, packets queued behind other work, GPU execution, preemption and paging.
Compare wait-thread scheduling with queue progress. A baseline OS trace has no
guest resource IDs or shader timings; do not infer per-resource/barrier causality
from packet timing alone. Symbolize using the matching build PDB in
`D:/PS5/src/KytyPS5/_Build/windows`; correlate application scopes only in a separately
marked capture if needed. No synchronization/cache changes are justified yet.
