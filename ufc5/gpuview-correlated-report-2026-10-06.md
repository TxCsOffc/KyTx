# UFC 5 correlated application / OS trace — 2026-10-06

## What this measurement establishes

Readback stalls are principally **completion waits behind already submitted work**.
The readback copies themselves have short GPU timestamp scopes. In the selected
30.068453 s window, 517 application readback waits join to their target producer
submit ticks and exactly one native render packet issued inside each submit call.
Of their **12,602.843 ms** host wait, **12,408.676 ms (98.46%)** precedes native DMA
hardware-queue entry. This does not prove those preceding dependencies are redundant.

The long readback waits and inflated detile GPU scopes are related through preceding
batches, but they are distinct measurements. Waiting in the CPU queue before the
readback copy does **not by itself explain an inflated detile GPU timestamp** inside
an earlier executing batch. Pure shader execution versus in-batch dependency, memory
or scheduling latency remains unresolved. No renderer optimization was made.

## Capture, clocks and data quality

- ETL: `D:/PS5/ufc5-profiles/gpuview-20261006-093956-a11b2ab5.etl`.
- Local recording: **09:39:57–09:40:27 BST**, same live PID 7600/build/profile as
  the two baselines. Firefox closed; FlatSRT `off all`.
- xperf: **zero lost buffers/events**, no unmatched packet stop in selected window.
  19,217 complete render packets match DMA sequences; all phases nonnegative.
- Original application window: **epochs 9301–9364**, 64 producer epochs, zero
  event/query drops. These are guest SuspendPoint hints, not displayed frame IDs.
  **52 complete epochs fit inside ETW**; original app capture extends beyond it.
- Application snapshot: `gpuview-20261006-093956-a11b2ab5.app.csv` and suffix files.
  A later diagnostic arm captured epochs 9577–9640 in the original live CSVs.
  Snapshot filtering excludes that entire later window; no mixed-window results.
- `ProfileClockNs()` uses MSVC steady-clock QPC converted to nanoseconds. An offline
  `OpenTrace` reader with `PROCESS_TRACE_MODE_RAW_TIMESTAMP` recovered raw QPC for
  12 initial Dxg events. Order/TID and decoded fence/mutex payloads identify the
  corresponding xperf records. QPC-origin estimates span **8 ticks = 0.8 us**.
  Selected origin **6,597,335,391,114 ticks**; xperf timestamps are quantized to us.
- QPC boundaries: ETL-relative **762,772.9–30,831,225.7 us**. Duration
  **30.0684528 s**. Application CPU timestamps and ETW are aligned through QPC;
  GPU raw timestamp ticks are **not calibrated to this CPU clock**.
- Saved emulator symbol ranges reuse the preceding trace's matching build/PDB;
  no timing measurements are imported from it. Main module base `0x140000000`.

### Open-file size issue and correction

The original helper manifest records zero application CSV growth. Directory-entry
sizes were stale while Kyty retained open CRT streams. Direct shared file reads
revealed valid rows; read-only stream/handle inspection confirmed the intended paths,
and direct reads refreshed reported lengths. The initial status report claiming no
app data was therefore incorrect. The original manifest is retained as captured,
not rewritten to hide this discrepancy.

`capture-gpuview.ps1` now obtains file EOF through fresh shared handles for offsets,
including the presentation log. It warns when producer CPU CSV bytes do not grow.
PowerShell parsing and read-only preflight pass. The emulator was not restarted,
patched in memory or rebuilt; the debugger used non-invasive read-only inspection
after ETW had finished. Its observations are not performance measurements.

## Command-thread budget

TID 26980 context-switch intervals plus saved return stacks reconcile exactly.

| Category | ms over selected window |
|---|---:|
| Running CPU | **16,252.471** |
| Readback path off CPU | **12,683.591** |
| Image dedicated-memory free path | 788.873 |
| Other dedicated-memory frees | 57.337 |
| Allocation path | 79.963 |
| Submission path off CPU | 4.967 |
| Other GC path | 37.145 |
| Other saved stacks | 164.106 |
| **Total** | **30,068.453** |

Off-CPU includes ready time, not full API elapsed or GPU execution. Readback path
contains 540 off-CPU intervals, maximum 144.412 ms. Application readback API waits
overlap **12,588.430 ms** of those intervals; the rest includes the unprofiled lead-in
before arming and any scope/coverage differences. The 517 API waits and 540 switches
are different populations: one API call can be switched out more than once.

| Readback transaction address | Calls | Clipped API wait ms | Off-CPU overlap ms |
|---|---:|---:|---:|
| `0x1164b80000` | 104 | **6,724.372** | 6,721.339 |
| `0x1167f00000` | 104 | **3,988.203** | 3,985.345 |
| `0x1140008000` | 206 | 1,270.387 | 1,264.643 |
| `0x1165e00000` | 103 | 619.881 | 617.103 |

The first two account for **85.0% of matched API wait time**. These are transaction
addresses; widened/packed downloads can publish other guest spans. FineCPU/Lifetime
were disabled in this launch, so exact content equality/writer lifetimes are absent.

## Matching submits and native progress

Each wait names its target master tick in the `bytes` field. Join that to scheduler
0's `.submits.csv` tick, then native render QueuePacket starts on the same CPU thread
inside `[queue_enter_ns, submit_end_ns]`, allowing 1 us export quantization. All 517
target ticks match and each yields exactly one render packet. This is an API-call
and thread join, **not a decoded native semaphore-identity join**.

The selected waits partition into:

| Portion of application host wait | ms |
|---|---:|
| Before matched render packet enters DMA hardware queue | **12,408.676** |
| During its DMA residence | 170.104 |
| After its DMA exit until CPU API return | 24.062 |
| **Total** | **12,602.843** |

Native DMA residence includes queued waiting; it is not pure execution. For the
same 517 transaction/tick pairs, GPU timestamp scopes show:

| GPU scope | Total ms | Mean ms | Maximum ms |
|---|---:|---:|---:|
| `download`, TOP_OF_PIPE → BOTTOM_OF_PIPE | 14.458 | 0.027965 | 0.110240 |
| `copy_copyBuffer`, TRANSFER → TRANSFER | **12.462** | **0.024104** | **0.103872** |

These are nested GPU stage elapsed scopes and cannot be added to each other or
the CPU wait budget. The copy scope can contain transfer-stage stalls. Short scopes
combined with long pre-DMA delay establish that copying the payload is not the
dominant cause of these readback waits.

## Longest matched wait: epoch 9339

Address `0x1164b80000`, transaction 1835, target tick **2794302**:

- Queue-submit API elapsed: **0.0856 ms**; host wait begins 0.0035 ms after return.
- Host timeline wait: **144.4385 ms**, off-CPU switch interval 144.412 ms.
- Native context `0xffff8d8dcc3d59e0`, sequence **6215018**.
- Native enqueue → DMA entry: **144.315 ms**.
- DMA residence: **0.133 ms**; queue notification follows DMA exit by 0.002 ms.
- CPU API returns **0.0239 ms after native render completion**.
- GPU download scope **0.079712 ms**; transfer copy scope **0.075776 ms**,
  payload **524,288 bytes**.

Earlier batches on the same native graphics context remain ahead of this packet:

| Producer tick | Native sequence | Pre-DMA ms | DMA residence ms | GPU batch elapsed ms |
|---|---|---:|---:|---:|
| 2794285 | 6214973 | 3.136 | **55.345** | **55.129696** |
| 2794292 | 6214993 | 61.405 | 15.237 | 15.079072 |
| 2794297 | 6215005 | 79.525 | 11.666 | 11.611328 |
| 2794298 | 6215009 | 78.909 | **63.199** | **51.428320** |

These batches also emit native copy-context work. The table's intervals overlap;
do not add the rows to reconstruct the 144 ms wait. Tick 2794298 contains detile
compute-stage brackets of **15.746048 ms** (`0x1fe0000` byte payload) and
**26.161152 ms** (`0x870000`), plus image-copy scopes of 2.716864 and 2.911232 ms.
Tick 2794292 contains a `0x15f9000` detile dispatch bracket of **10.946560 ms**.

This localizes the delay to earlier work and its progress. It does not establish
shader active time, or identify the batch's 11.771 ms residence-versus-GPU-scope
difference as a particular scheduler stall. Native packet residence and GPU
timestamps have different stage/notification boundaries.

### Native copy → graphics dependency checks

Exact native sync-object/fence-value joins resolve **3,158** selected wait packets
to unique signals: 1,729 into the main graphics context and 1,429 into a second
graphics context, all from native copy context `0xffff8d8d9a273d30`. **2,204** waits
have no captured matching signal; no ambiguous object/value match was used. This
is partial native coverage, not a map of every Vulkan semaphore or detile.

For tick 2794298's preceding native wait **6215007**, sync object
`0xffffe081af2297c0`, value **291667**, matches copy-context signal **752751**.
Signal completion is ETL-relative **23,871,371 us**; graphics wait completion is
**23,892,827 us**, then render **6215009** reaches DMA at **23,892,858 us**.
Thus the signal has completed **21.456 ms before the wait packet's completion**.
The wait's 78.922 ms visible lifetime includes its place in the graphics queue;
it cannot be attributed entirely to waiting for this copy signal. Earlier queued
work remains relevant. No claim that graphics waits after every detile is supported.

## CPU feed gaps, barriers and probe cost

No reconstructed Kyty render/DMA outstanding: **8,427.670 ms**, with command thread
running during **8,018.333 ms**. Maximum gap 117.690 ms. The application profiler
adds substantial CPU work in this window: **1,861.092 ms** of event flush scopes and
**432.216 ms** of query collection scopes. They are elapsed scopes contained in the
thread budget, not separate additive CPU costs. Compared with the unarmed baseline,
these results cannot establish a throughput win or regression caused by renderer code.

In the selected window, recorded barrier entries include 222,022 global, 46,862
buffer and 35,419 image barriers. **139,186 global entries** use
`ALL_COMMANDS → ALL_COMMANDS`, source `MEMORY_READ`, destination
`MEMORY_READ | MEMORY_WRITE` (stage `0x10000`, accesses `0x10000 → 0x18000`).
Counts identify conservatism, **not measured barrier latency**. They are entries,
not necessarily separate API calls. No stage/access/layout/queue family was changed.

## Ranked investigation targets

Follow-up: [constructed depth/stencil transaction pair results](detile-upload-pair-results-2026-10-06.md).
The default isolated replay already includes its internal clear/barriers. The new
pair additionally uses production image uploads, but lacks format/state and guest
batch commands. Concurrent default replay also shows large elapsed inflation;
same-build closed repeats now match all references at 0.083–0.478 ms. The constructed
pair is 2.803 ms closed, still far below the production batch; scheduling/residency
evidence is needed to identify the inflation mechanism.

1. **Earlier detile/upload batches and their dependencies.** The readback payload
   is cheap; these prior batches keep its completion late. Follow the existing
   native copy/graphics wait-signal chains around ticks 2794285–2794302, then compare
   the complete surrounding detile transaction/batch with the headless harness.
   Keep dispatch, clear, copy and barriers in the reproducer. The isolated kernel
   results alone do not represent this production batch.
2. **CPU command feed and profiling emission.** Unarmed baseline already shows
   CPU-running gaps; this capture additionally spends ~2.3 s in explicit flush/query
   scopes. Reduce measurement emission or use a smaller dense window before treating
   correlated capture gaps as representative uninstrumented gameplay.
3. **Broad barriers.** Many are confirmed, but their causal cost is not isolated.
   Audit the specific preceding batches first; counts do not justify removing them.
4. **Allocation/free churn.** Much smaller off-CPU totals than readback completion.

Next analysis can use this saved trace; no immediate restart or repeat recording is
needed. Do not cache stale guest bytes, discard required readbacks or remove barriers
based on this correlation. There is no demonstrated FPS optimization in this session.

## Artifacts and verification

All paths below use `D:/PS5/ufc5-profiles/gpuview-20261006-093956-a11b2ab5`:

- `.analysis.json`, `.analysis.json.packets.json`: native phases/CPU scheduling.
- `.wait-analysis.json`: saved-stack off-CPU durations; full budget closure passes.
- `.clock-alignment.json`, `.qpc-anchor.txt`: raw QPC alignment and quantization.
- `.correlation.json`, `.readback-joins.json`: addresses, API waits and native joins.
- `.native-dependencies.json`: unique native sync-object/value joins and missing coverage.
- `.app.csv` and suffixes: original 64-epoch snapshot; `.app.analysis/report.md`
  covers that **whole application window**, not just the ETW overlap.
- `.traceheaders.txt`, `.dxg-compact.csv`, `.threads-stacks.csv`: reproducibility.

Offline diagnostic sources: `etl-qpc-anchor.cpp`, `correlate-ufc5-20261006.py` in
the profile directory. QPC boundary regression and three prior queue regressions
pass; two saved-stack/tail tests already pass. Exact CPU closure, nonnegative native
phases and interior pairing checks pass on this capture. Controls are `off all` and
`off`; live PID remains responsive.
