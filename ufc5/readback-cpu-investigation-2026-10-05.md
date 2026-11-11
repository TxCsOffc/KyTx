# UFC 5: targeted readbacks and CPU preparation

Date: 2026-10-05. Measurement only; no optimization applied.

## Conclusions

1. The tiny readbacks are **indirect draw arguments**, not a status/query polling loop.
   `CommandProcessor::DrawIndirect` reads them with `memcpy` and immediately derives
   draw parameters and instance state. Native GPU copies also update this argument
   region. Keeping old bytes or returning before publication is not justified.
2. The expensive large regions contain **CMask color metadata**. The CPU reads them
   in `TextureCache::MaterializeColorClear`, before deciding how to handle a target.
   This is an immediate CPU decision in the current architecture.
3. Actual transfer-stage copy brackets are tiny beside the foreground waits.
   These synchronous transactions wait on completion of a batch containing prior
   work, not just their copied payload. Copy bandwidth is not the dominant measured
   cost of these transactions. The trace does not isolate a particular pre-barrier
   or preceding shader as the cause of each wait.
4. Resource materialization on shader-cache hits and repeated buffer synchronization
   are concrete CPU investigation targets. Fine samples identify these functions,
   but their scaled budgets fail reconciliation with the measured phase envelopes.
   **Do not claim that materialization costs exactly 58 ms/epoch or that binding
   preparation's own body costs exactly 67 ms/epoch.** Those are withheld estimates.
5. Recommend a bounded, shadow-validated **direct descriptor evaluation path** in
   the resource materializer first. It can avoid repeated interpretation while
   continuing to read current inputs. Eligibility and the actual saving still need
   measurement. No FPS gain has been demonstrated.

## Capture and confidence

Corrected trace: `D:/PS5/ufc5-profiles/production-20261005-203855.csv`.
Analysis: the adjacent `.analysis` directory, including `readback-lifetimes.csv`,
`readback-addresses.csv`, `cpu-fine-calls.csv`, `cpu-fine-summary.csv`,
`cpu-fine-per-epoch.csv`, `cpu-fine-counters.csv`, `cpu-repetitions.csv`, and
`cpu-sampling-closure.csv`.

The user reached the paused fight and confirmed **looks the same**. Native guest
resolution was restored; sparse BDA remained enabled. The manifest records binary,
options and SHA256. The capture covers epochs 900–963: 64 epochs, 26.026 seconds,
127 present calls (~4.88 calls/s during capture). These epochs are SuspendPoint
enqueue hints, not displayed frames. Capture-off rates varied and later reached
about 6.1–6.2 FPS; these are not an A/B performance improvement.

Zero CPU event drops. There were 49 dropped GPU scopes; incomplete GPU epochs are
excluded from the main complete-epoch aggregates. CPU detail uses eight interior
epochs without CPU drops: 904, 912, 920, 928, 936, 944, 952, 960. A mixed counter
selects approximately 1/32 outer fine scopes, recording all their fine descendants.
CPU analysis does not require a complete GPU query epoch.

The preceding trace `production-20261005-202527.csv` overflowed the CPU event buffer
in its dense detail epochs. It is partial evidence, not a complete CPU breakdown.
That failure prompted call sampling and one extra restart.

The main corrected trace's medians over complete interior epochs are:

| Scope | ms/epoch |
|---|---:|
| Wall interval | 384.682 |
| Command processing excluding recorded waits/submits/profiler | 233.321 |
| Foreground explicit waits | 72.593 |
| Submit CPU elapsed | 7.812 |
| Profiler serialization | 49.913 |
| Profiler collection | 8.066 |
| Conservative no-outstanding-captured-batch gap | 20.821 |

Profiler serialization is substantial. It is measured and excluded from host-work
attribution, but changes submission cadence. CPU scope elapsed also includes host
preemption. GPU timestamps are stage elapsed intervals, including stalls; CPU and
GPU clocks are not calibrated. Do not add these overlapping categories or compare
their raw timestamps across clock domains. The earlier lighter trace remains the
better reference for the unoptimized overall budget (~249 ms host, ~91 ms waits).

## Per-address results

Numbers below cover the full corrected window, including its edges. Equality counts
are exact comparisons of previously downloaded **same-address, same-size spans**,
not hash-only matches. A first observation has no prior comparison.

| Drain address | Actual request | Packed payload | Calls / epoch | Mean wait | Longest wait | Mean GPU copy bracket | Content comparisons |
|---|---|---:|---:|---:|---:|---:|---|
| `0x1164b80000` | CMask `0x1164be0000`, 81,920 B | 524,288 B | 2.000 | 12.022 ms | 28.898 ms | 0.075000 ms | 36 equal, 91 changed, 1 first |
| `0x1140008000` | Faults at `0x1140008200/9c00/9e00/a000`; reported width 1 B | 128 or 448 B | 3.969 | 6.454 ms | 17.418 ms | 0.001456 ms | 460 equal, 164 changed, 11 first segments |
| `0x1167f00000` | CMask at same address, 36,864 B | 131,072 B | 2.000 | 11.512 ms | 15.896 ms | 0.006061 ms | 127 equal, 0 changed, 1 first |
| `0x1165e00000` (comparison) | CMask `0x1165e60000`, 36,864 B | 524,288 B | 1.984 | 0.303 ms | 2.242 ms | 0.064223 ms | 31 equal, 95 changed, 1 first |

The three expensive regions contribute 4,651.580 ms, **98.3%** of synchronous
readbacks' 4,733.379 ms recorded host-wait union. There are 726 synchronous readback
transactions. The cheap CMask example has the same 512 KiB payload as the expensive
one; size alone does not explain latency.

### `0x1164b80000`

- Semantic role: the widened drain includes CMask at `0x1164be0000`.
- Last observed declared producer: compute shader `0x723423f5115aedbd`, writable
  descriptor covering the 81,920-byte metadata range. Shader and guest-submit IDs,
  scheduler batch tick and CPU submit times are retained for each occurrence.
- Consumer: `MaterializeColorClear` immediately reads metadata code and decides
  whether to clear/expand the target. Code `0x00` and subsequent CPU expansion to
  `0xff` were recorded 128 times. This is repeated production and consumption,
  not proof of a redundant poll.
- Publication: every copied span is recorded after its original `WriteBacking`.
  The first metadata-code consumer marker follows publication.
- Buffer handle stayed stable. The **whole 512 KiB span** changed in 91 comparisons;
  that includes bytes outside the requested metadata. This does not prove which
  metadata bytes changed or justify reusing a previously expanded state.
- Immediate visibility: required by the existing CPU materialization decision.
  Async/deferred return and identity-only caching are unsafe without a new proof.

### `0x1140008000`

- Semantic role: a 16 KiB cached buffer contains multiple indirect argument records.
  The original request is a page-fault byte, not the access width; Kyty copies dirty
  spans and packs them into 128/448-byte staging payloads.
- Request context: **all 254 occurrences are `guest_draw_indirect`**. The actual
  consumer is `DrawIndirect`'s existing `memcpy`, followed by draw-count, offset,
  base-vertex and instance processing. Its resumed instruction has no separate
  consumer timestamp; the operation context and source identify the consumption.
- Last observed writer in the drained buffer: a native 4-byte copy with
  `guest_copy_data` context, commonly at the argument record's `+4` offset.
  Writable compute descriptors also cover this region. The last writer in the
  drain window is not necessarily the last writer of every requested byte.
- Exact equality is per individual span: 635 segment observations across 254
  transactions. There are real changes, so this is not an identical-result loop.
- Immediate visibility: required for current CPU draw translation and persistent
  `m_num_instances` state. A native Vulkan indirect-draw path could remove some
  CPU argument reads, but it must first prove all these CPU dependencies redundant.
  It is not currently a safe async-readback switch.

### `0x1167f00000`

- Semantic role: CMask consulted by `MaterializeColorClear`.
- Last observed declared writer commonly covers a much larger buffer region from
  `0x1165b20000` (37,748,736 bytes), shader `0xea0aceac518ec52d`. That descriptor
  overlaps the metadata. Its declaration is **not proof of actual stores** to it.
- Consumer: metadata code read immediately after publication. Code `0x00` was
  observed 128 times; no metadata expansion marker followed for this address.
  The trace does not identify whether decoding or uniformity rejected a clear.
- All 127 repeat comparisons of the 131,072-byte span were exactly equal. The
  requested metadata is inside that span, so it also remained identical in these
  comparisons. Its buffer handle stayed stable.
- This is the strongest unchanged-readback candidate, but dirty producer declarations
  still intervene. Actual write bounds and all alias/CPU writers remain unproved.
  Caching solely because previous returned bytes matched would be unsafe.

### `0x1165e00000`, cheap control

Same materializer and declared clear shader as the first address. Code `0x00` and
expansion to `0xff` were observed 127 times. Its 512 KiB copies averaged 64 microseconds,
but waits averaged only 0.303 ms. The resource's position in queued work/dependencies
matters; this trace does not isolate the particular dependency responsible.

## CPU breakdown

Whole measured phase envelopes, after recorded waits and diagnostic work, have
medians **84.123 ms state**, **72.337 ms bindings**, and **32.140 ms commitment**
over the eight CPU detail epochs. These are elapsed attribution, not pure thread cycles.

The following are **actual recorded sample totals** over those eight epochs, ranked
by self elapsed. They are not full-epoch totals. Inclusive per-call statistics retain
unattributed child work and scheduling; self time subtracts instrumented children.

### State preparation

| Function | Recorded calls | Recorded self ms | Median inclusive call (us) | p95 (us) | Worst (us) |
|---|---:|---:|---:|---:|---:|
| `MaterializeResources` | 1,875 | 14.425 | 5.1 | 24.1 | 128.4 |
| `TextureCache::FindImage` | 3,613 | 9.130 | 0.7 | 1.5 | 3,451.8 |
| `ProgramCache::Get` remaining body | 1,875 | 5.158 | 5.7 | 25.4 | 1,842.0 |
| `ResolveRenderColorTarget` remaining body | 2,368 | 2.976 | 0.8 | 2.3 | 3,452.9 |
| `ResolveRenderDepthTarget` remaining body | 1,255 | 2.464 | 1.5 | 2.1 | 1,958.9 |
| `PrepareDrawRenderState` remaining body | 1,261 | 0.827 | 9.3 | 57.2 | 3,484.7 |

Within these same sampled state `ProgramCache::Get` calls, materialization's inclusive
14.932 ms accounts for about **74% of the parent's 20.244 ms**. Source/permutation
cache hits still rebuild current resource snapshots; these observations are not shader
compilation misses. `BuildStageStaticKey` recorded just 0.166 ms across 1,347 calls.

### Resource/binding preparation

| Function | Recorded calls | Recorded self ms | Median inclusive call (us) | p95 (us) | Worst (us) |
|---|---:|---:|---:|---:|---:|
| `PrepareGraphicsBindings` remaining body | 1,221 | 18.390 | 5.9 | 208.0 | 3,778.7 |
| `ResolveTexture` remaining body | 3,782 | 8.479 | 0.9 | 2.1 | 5,599.3 |
| `ObtainBuffer` remaining body | 8,949 | 8.106 | 0.2 | 0.5 | 3,329.5 |
| `SynchronizeBuffer` remaining body | 48,919 | 5.651 | 0.1 | 0.2 | 21.7 |
| `FindTexture` / image-view preparation | 3,591 | 4.880 | 0.2 | 0.6 | 3,752.1 |
| `FindBuffers` remaining body | 1,701 | 4.477 | 1.1 | 3.4 | 2,400.4 |
| `PrepareBindings` remaining body | 1,885 | 2.473 | 0.3 | 13.4 | 5,614.9 |
| `FindImage` remaining body | 3,761 | 2.024 | 0.3 | 1.2 | 184.8 |

`PrepareGraphicsBindings` contains an uninstrumented `PrepareBda` branch. Source
inspection shows it scans **all mapped ranges** and visits cached buffers before
DMA-capable draws. The repeated `SynchronizeBuffer` samples are consistent with
that call structure, but the parent's remainder is not a measured `PrepareBda` cost.
Several lookup totals are dominated by isolated millisecond outliers; their medians
and p95s are much smaller. Do not classify all such elapsed time as conversion work.

Recorded counters: 2,066 program source hits and 2,066 permutation hits, no source/
permutation miss markers; 11,059 buffer hits versus 4 misses; 7,577 image hits versus
8 misses; 1,432 sampler hits and no misses; 2,085 binding rebuild markers and no reuse
markers; 1,261 state rebuild markers. These are samples, not all-epoch counts.
Allocation samples: 8 images (0.920 ms inclusive total), 4 buffers (0.551 ms).
Lock acquisition samples: 2,910 image acquisitions (0.292 ms total, 0.4 us max),
598 sampler acquisitions (0.061 ms total, 0.2 us max). This does not demonstrate
significant blocking contention; clock granularity limits the tiny measurements.

### Why the scaled finer budget is withheld

Multiplying sampled fine self totals by 32 yields **1.84 times the measured state
envelope and 3.36 times the binding envelope**. Finite sampling of outliers and child
probe emission cost charged to parent self can inflate these estimates; their separate
contributions have not been isolated. The analyzer now flags this failure rather than
publishing a reconciled-looking budget. Raw samples, per-call quantiles and per-epoch
observations remain useful. Precise subdivisions require recording probe emission
cost and directly bracketing `PrepareBda` in a subsequent build. More FPS testing of
this instrumentation would not resolve that measurement limitation.

## Repetition and invalidation

| Candidate / observed identity | Recorded repetition | Change evidence | Reuse requirement |
|---|---|---|---|
| Materialization for shader `0xdf3633d8030ed2a3` | 358 sampled state calls across 8 epochs; 1.147 ms self | Same shader, current resource values not proved identical | Preserve current user data, SRT reads, active control flow and specialization |
| Materialization for `0xd3dcf81c43080fd0` | 116 sampled calls; 1.276 ms self | Same shader is not an immutable snapshot | Same rules; useful bounded first-shader candidate |
| Buffer `0x113d000000`, 25,600,000 B | 976 sampled synchronizations across 8 epochs; 0.173 ms self | CPU dirty/version history for this range not fully recorded | Check real dirty tracking; account for creation, unmap and concurrent writes |
| Texture descriptor fingerprint `0x68458022ad47aa37` | 155 sampled resolutions; 0.544 ms self | Fingerprint covers descriptor words only | Include exact words and all IR image-class/policy inputs; never reuse live cache state solely by this hash |
| CMask `0x1167f00000` | 127 exactly equal repeat spans | Dirty declared producer still intervenes | Prove actual write bounds and all aliases before omitting a readback |
| State / binding rebuilds | 1,261 / 2,085 recorded markers | Program IDs / observed descriptor keys are subsets | Full state, resource generations, view liveness, specialization and stream lifetime |

Full occurrences and cumulative times are in `cpu-repetitions.csv`. Generic key `0`
with a stage count is not a resource identity. None of these observations establishes
a safe whole-state or whole-binding cache. Existing optional `KYTY_BIND_REUSE` has
snapshot and liveness checks; it was not enabled or changed by this investigation.

## Top three bounded A/B experiments

All are proposals. Begin with a shadow/reference comparison and a default-off switch.
Keep profiler settings identical between A/B arms, then confirm any timing win with
capture off. Do not predict a specific millisecond saving from the withheld estimates.

### 1. Direct descriptor evaluation plan — recommended first

- Path: `ProgramCache::Get → MaterializeResources → SrtWalker::EvaluateDescriptor`.
  First scope: one measured hot shader, such as `0xd3dcf81c43080fd0`.
- Change: classify descriptor words whose resolved values are U32 constants or direct
  `GetUserData` references into a small immutable gather plan. Read **current** user
  data every time, retaining existing flat-SRT refresh and materialization validation.
  Unsupported expressions, memory loads and control-flow cases use the interpreter.
- Assumptions: reproduce user-data-base subtraction, bounds failures, dword count,
  zero initialization and exact U32 values. Do not bypass active-source logic, clean
  versus writable evaluation, captured reads or specialization generation.
- Invalidation: replace the gather plan when its owning resource plan/program changes;
  input values are not cached. No cross-draw guest-memory freshness assumption.
- Measurable effect: fewer interpreter evaluations within the measured materializer.
  Eligibility fraction and whole-state/host elapsed determine whether it is worthwhile.
  A single shader's scope may save little; expand only after exact validation.
- Rollback: runtime off returns to the original evaluator for every source.
- Validate: shadow old/new descriptor values, failure results, full snapshot and
  specialization; exercise changed user data, invalid indices, inactive sources and
  fallback expressions. Keep existing memory-read order on the fallback path.

### 2. BDA clean-range guard

- Path: `PrepareGraphicsBindings → RenderContext::PrepareBda →
  BufferCache::SynchronizeBuffersInRange → SynchronizeBuffer(false,false)`.
- Change: for this narrow BDA call site, check the existing CPU-dirty tracker and
  skip a buffer's upload-range enumeration only when it reports the complete clamped
  range clean. Keep the mapped-range walk, fault-processing flag and all GPU commands
  for dirty ranges. Do not introduce a global last-epoch cache.
- Assumptions: a clean tracked range makes the original upload enumeration empty;
  no image-to-buffer synchronization or writable dirty marking applies here. Audit
  page protection/concurrent fault ordering and lazy region creation before enabling.
- Invalidation: recheck current dirty state on every visit. Dirty, new or uncertain
  ranges take the original path. A generation shortcut is a separate, unproved change.
- Measurable effect: fewer empty tracker passes/locks, with unchanged actual uploads.
  The guard itself has cost; it may lose if it duplicates checks without saving work.
- Rollback: switch off the guard. Shadow mode still runs the original enumeration;
  a predicted-clean range yielding uploads rejects the fast path.
- Validate: identical upload range/byte streams, newly dirtied pages, simultaneous
  faults, unmap/remap, merged buffers and unchanged guest/GPU outputs.

### 3. Pure texture description memoization

- Path: the descriptor/layout-building portion of `ResolveTexture`, **before**
  `TextureCache::FindImage` and the subsequent live-image validation.
- Change: memoize a normalized `ImageDesc` and view description for an exact input
  key; run all live cache lookup, dirty handling, refresh and binding operations.
- Assumptions: separate only pure conversion. Include all descriptor words and count,
  IR image class/numeric class/dimension, read/write flags, r128, mip mode, atomic/
  depth flags and stable device/format policy inputs. Hash collisions require equality.
- Invalidation: any input/policy change; device/context destruction clears entries.
  Do not cache an ImageId, VkImage, view, metadata result or readiness state.
- Measurable effect: fewer pitch, mip-layout, format and view-description conversions.
  The current outliers are not proven conversion cost, so gain remains unquantified.
- Rollback: runtime off always reconstructs the description.
- Validate: field-wise shadow equality across depth, MSAA, volume, compressed, storage,
  null and atomic views; invalid combinations must fail exactly as before.

### Readback changes withheld

Async staging, deferred consume, previous-byte caching, batching and removing polls
are **not justified** for these observed consumers. No redundant status poll was found.
GPU-native indirect draw consumption is a possible later direction, but CPU instance
state, range preparation, index/vertex offsets, topology conversion and subsequent
guest uses must first be proven compatible. CMask reuse requires producer write-bound
and alias proofs. Existing uniform-clear conversion already has conservative guards;
this report does not authorize weakening them.

## Validation and handoff

Emulator and headless harness build successfully. All eight fight detile captures
still return reference MATCH; isolated medians in the final build are 0.080–0.478 ms.
The 1,024-iteration query lifetime/exhaustion test passed with Khronos synchronization
validation, MATCH and no reported warnings/errors. Its intentional query drops/split
are diagnostics, not game-work changes. Analyzer regression tests cover nested
attribution, waits, lifetime joins, exact-publication markers, unknown writers, timestamp
wrap, schedulers, absent CPU detail and rejection of invalid scaled budgets.

The user confirmed the paused scene looks the same. No screenshot/frame-hash comparison
was performed, and no optimization trial has been run. Future trials need full-output
checks, existing replay MATCH, no new renderer/validation warnings, and changing guest
state tests. Any readback trial additionally needs exact bytes, unchanged publication
requirements and transition coverage—not just a matching screenshot.

The user closed the emulator after the completed capture; this was not a crash.
No shader, barrier,
wait, allocation policy or resolution optimization was applied, and no commit was made.
Next: implement experiment 1 in shadow mode, report eligibility and exact equality,
then measure the enabled path. Include probe-emission accounting and a direct
`PrepareBda` bracket in that next build so fine budgets can be trusted.
