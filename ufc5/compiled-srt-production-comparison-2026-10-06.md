# Production CPU/GPU comparison after compiled SRT — 2026-10-06

## What changed in the measured budget

Compiled SRT reduces command-thread work excluding recorded waits, submits and
profiling scopes: **257.944 → 209.635 ms/epoch** (~48ms). Sampled draw-state
preparation excluding waits falls **92.557 → 46.637 ms/epoch**. The continuing
evaluator CSV confirms actual candidate execution during this capture, with
99.83% candidate calls and zero mismatches in fully contained interior windows.

The on capture also contains substantially larger readback waits and detile GPU
elapsed scopes. These measured costs mask the CPU-path saving. Their association
with the on window does not establish that the evaluator caused them; native GPU
residency, scheduling, competing work and pipeline dependency latency were not
measured by this capture. No overall FPS win or proven causal regression claimed.

## Capture and coverage

Same paused-fight process24900 and binary as the first live experiment, native
guest output. On captured first, then off, then on restored at the user's request.
No rebuild/restart or synchronization change. Profiler CPU detail every8epochs;
fine CPU and targeted lifetime modes were not configured in this build's launch.

| Item | Compiled on | Compiled off |
| --- | ---: | ---: |
| Producer epochs | 7361–7424 | 7629–7692 |
| Captured / complete interior epochs | 64 / 62 | 64 / 62 |
| GPU ranges | 162,947 | 163,437 |
| Queue submits | 21,659 | 21,691 |
| Timer / event drops | 0 / 0 | 0 / 0 |
| Split scopes / uncollected batches | 0 / 0 | 0 / 0 |
| Window wall time | 32.842s | 30.682s |
| Present API calls | 126 | 127 |
| Detailed interior CPU epochs | 7 | 8 |
| Median draws / computes per epoch | 5,432 / 750 | 5,432 / 750 |

Freezing selects completed producer epoch ranges from the same rearmed recorder
and preserves producer/presenter traces and queue metadata. Reports use the
existing production analyzer, without a second instrumentation system. No heavy
analysis ran during the selected capture windows. Initial/last epochs excluded
from median budgets. Capture overhead is significant; these are not normal
gameplay FPS measurements. User visual confirmation remains pending.

## Main budgets

All values below are medians over complete interior epochs, milliseconds/epoch.
An epoch is a guest SuspendPoint enqueue hint, not a displayed frame. CPU/GPU
clocks are uncalibrated; the columns and individual medians cannot be added.
GPU scopes include stalls and possible preemption, not shader-active time.

| Metric | On | Off |
| --- | ---: | ---: |
| Epoch wall interval | 454.068 | 382.528 |
| Guest work excluding waits/submits/profiler | **209.635** | **257.944** |
| Foreground explicit waits | **140.249** | **71.560** |
| Recording excluding waits/submits/profiler, detailed epochs | 159.081 | 206.006 |
| Translation/other CPU, detailed epochs | 53.197 | 58.959 |
| Submission CPU | 7.996 | 8.221 |
| Profiler serialization | 26.092 | 26.639 |
| Query collection | 8.276 | 8.446 |
| No captured outstanding batch, observed lower bound | 20.885 | 25.930 |
| GPU submitted-batch elapsed union | 157.687 | 126.020 |
| Graphics GPU scope union | 29.293 | 34.645 |
| Guest compute GPU scope union | 51.760 | 49.764 |
| Detile dispatch GPU scope union | **46.252** | **15.541** |
| Copy/clear GPU scope union | 21.028 | 14.627 |

Nearly identical operation counts support the comparison. The GPU inflation
concentrates in detile/copy elapsed scopes, while guest compute stays near50ms.
There is no evidence here that the detile algorithm itself became slower.
Uncovered GPU timestamp gaps are not proof of device idleness or its cause.

## Remaining CPU preparation

Unioned per-phase intervals on foreground thread26868, subtracting overlap with
recorded waits. These use7on/8off detailed epochs, include preemption/probe and
uninstrumented child costs, and are not a full fine-function census.

| Phase | On ms/epoch | Off ms/epoch |
| --- | ---: | ---: |
| Binding/resource preparation | **69.617** | 70.866 |
| State preparation | **46.637** | 92.557 |
| Draw commitment | 33.858 | 34.145 |
| Targets | 5.871 | 5.897 |
| Vertex/index | 2.281 | 2.298 |
| Pipeline lookup | 1.885 | 1.968 |

Bindings are now the largest measured draw preparation category. Source review:
`PrepareGraphicsBindings` calls `PrepareBda` for relevant shader stages;
`RenderContext::PrepareBda` traverses all mapped ranges;
`BufferCache::SynchronizeBuffersInRange` visits every overlapping cached buffer.
The selected trace cannot attribute all69.617ms to BDA because finer hooks were
off. It does not imply that the whole phase is recoverable.

## Readback latency

All captured readback transactions, including boundary epochs, foreground
thread26868. Totals across transactions, not medians or additive CPU/GPU budgets:

| Guest address / known role | On calls | On host wait ms | Off calls | Off host wait ms |
| --- | ---: | ---: | ---: | ---: |
| 0x1164b80000, CMask metadata, 512KiB | 127 | 7,612.521 | 128 | 4,771.804 |
| 0x1167f00000, CMask metadata, 128KiB | 127 | 4,899.894 | 127 | 3,070.692 |
| 0x1140008000, indirect arguments, varying width | 253 | 2,354.189 | 254 | 1,808.005 |
| 0x1165e00000, comparison readback, 512KiB | 126 | 676.940 | 127 | 427.549 |

Metadata regions account for about80.5% of the on-window readback wait. The
wait site remains `bufferCache.cpp:262`: wait on the submitted scheduler tick
before CPU publication. Background scheduler timeline waits overlap other work;
do not add them to foreground wait. This trace does not identify redundant
dependencies or prove that an earlier last-writer wait would be sufficient.

Broad barriers remain visible:172,460 ALL_COMMANDS→ALL_COMMANDS resource entries
at `graphicsRun.cpp:1574` in the on capture. Barrier counts identify conservatism;
their latency is not measured separately, and the barriers were not changed.

## Recommended next port

**Dirty-only BDA synchronization from GTA e5dd76e7** is the next contained CPU
candidate. Build it with counters and reference/shadow/live controls to measure
visited buffers versus required uploads and validate skipped clean passes.
Port graph-independent invalidation for CPU writes, tracking-region creation,
buffer registration/unregistration and mapping changes. Preserve concurrent
write handling and pending-upload semantics; sample epochs before a pass so a
write during synchronization cannot disappear from the next pass's view.

Measure the actual BDA portion and resulting whole binding cost; do not promise
the entire70ms binding budget or reuse GTA's reported gains as a UFC estimate.
Keep compiled SRT enabled as the measured CPU saving during later comparisons.

Readback dependency optimization remains the larger synchronization track.
Last-writer transfer-queue readbacks require validated per-range writer ownership,
unsubmitted/BDA writer fallback and CPU publication ordering. Native indirect
draws target the smaller argument-readback class and require eligibility proof.
The persistent detile-scope inflation still needs native GPU scheduling/residency
controls; a faster resource evaluator does not resolve it by itself.

## Artifacts / final state

Original identity manifest:
`D:\PS5\ufc5-profiles\production-20261006-143328.csv.manifest.json`.
Frozen on/off bases:

- `D:\PS5\ufc5-profiles\production-20261006-143328.compiled-on.csv`
- `D:\PS5\ufc5-profiles\production-20261006-143328.compiled-off.csv`

Each has `.selection.json` and `.analysis/report.md`, `per-frame.csv`,
`readbacks.csv`, `transactions.csv`, `wait-sites.csv`, `barrier-sites.csv` and
the original per-scheduler exports. Phase values retained in
`production-20261006-143328.compiled-phase-comparison.json`.
Live measurement boundaries are in the original base's
`.compiled-stage-profile_on.json` / `.compiled-stage-profile_off.json`.
`tools/freeze-production-window.py` separates rearmed capture ranges for the
existing analyzer. Fine-function/lifetime reports are empty because those
launch options were off; empty reports do not prove zero BDA/readback cost.

Both captures ended automatically. Process24900 remains responding, compiled
SRT **on**, production profiler request **off**. No new renderer optimization,
rebuild, restart or commit in this session.
