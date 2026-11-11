# Production graphics measurement

This is measurement instrumentation. It retains the existing Vulkan barriers, waits,
queue submits, image layouts, shaders and copies. It extends CommandScheduler's existing
GPU timers and CPU draw-phase counters, plus the existing headless detile replay harness.
No renderer optimization is part of this change.

## Capture a paused fight

From `D:\PS5\src\KytyPS5`:

```powershell
.\ufc5\tools\profile-production.ps1 -Action Launch -Build -FrameCount 120
# Drive into the paused fight, then:
.\ufc5\tools\profile-production.ps1 -Action Arm
.\ufc5\tools\profile-production.ps1 -Action Status
python .\ufc5\tools\analyze-production-profile.py D:\PS5\ufc5-profiles\production-TIMESTAMP.csv
```

The launcher stages `kyty_emulator.production-profile.exe` separately, reports the normal
guest resolution, and disables detile capture/verbose detile tracing. It saves a launch
manifest including the binary hash and inherited Kyty options. It does not terminate a
running emulator. Arming needs no restart; the producer consumes `on` within approximately
500 ms and captures its configured number of epochs. The window stops automatically.
`off` in the control file means the request was consumed, not necessarily that capture has
finished. Completion is shown by the CPU trace stopping at its final epoch.

For a known counter range:

```powershell
.\ufc5\tools\profile-production.ps1 -StartFrame 2700 -FrameCount 120
```

Equivalent runtime options are `KYTY_GPU_TIMING_CSV`,
`KYTY_GPU_PROFILE_START_FRAME`, `KYTY_GPU_PROFILE_FRAME_COUNT`, and optional
`KYTY_GPU_PROFILE_CONTROL_FILE`. Without a timing CSV, no query pool or event files are
created. Outside the window, scope hooks return immediately, counters remain inactive,
and only bounded control-file polling plus collection of previously submitted queries
remain. The old `KYTY_GPU_TIMING_CONTROL_FILE` switch still works, but should not be mixed
with the bounded capture switch.

All six CPU draw-phase counters are collected in every selected epoch. Detailed per-draw
CPU intervals are sampled every 16 epochs by default to limit serialization cost; use
`-CpuDetailEvery 1` or `KYTY_GPU_PROFILE_CPU_DETAIL_EVERY=1` for a very short dense CPU
capture. The analyzer marks missing detailed intervals as unavailable, retains inclusive
phase counters, and computes exact wait subtraction only where intervals exist.

### Targeted readbacks and finer CPU preparation

### Last-writer dependency study

Use the existing bounded lifetime recorder to trace source buffer identity,
every packed readback span, the current recording tick and last submitted tick:

```powershell
.\ufc5\tools\profile-production.ps1 -CompiledSrt -CompiledSrtInitialMode on -BdaSync -BdaSyncInitialMode on -Lifetime -FrameCount 64 -CpuDetailEvery 8
# Reach the paused fight, then arm. No restart is needed to arm again.
.\ufc5\tools\profile-production.ps1 -Action Arm
```

Writer declarations become recorded operations only at the marker after command
emission. Exact fills/copies/uploads and conservative image downloads carry
native buffer handles. Retirement/unmapping invalidate observed range history.
An initial current-tick floor accounts conservatively for work recorded before
capture; unknown device-address writers constrain all watched readbacks.

The existing analyzer also emits `readback-writer-bounds.csv` and `.json`, plus
`readback-writer-gpu-context.json` for same-scheduler batch/copy query comparisons.
It requires complete actual copy coverage and rejects pending overlapping or
opaque writes, unsubmitted required ticks, identity mismatches, multiple writer
recording threads and dropped CPU events. A reported earlier dependency bound
is an eligibility observation: it does not prove transfer-queue safety or measure
recoverable wait. Queue-family sharing, aliases, publication and lifetime still
need implementation validation. Missing history remains a fallback.

The opt-in GPU recorder now has 256 pages of 256 timer pairs (65,536 pairs),
using 16-bit timer indices and generation validation. This increases in-flight
capacity without adding waits or changing page reuse rules. Metadata records
the actual capacity. Check drops and uncollected batches before interpreting
GPU budgets; a timestamp range includes stalls/preemption within its stages.

### Fine CPU scopes and lifetime context

The bounded descriptor evaluation experiment can be added with `-DescriptorGather`.
It starts off and uses a separate live switch and aggregate CSV; see
[descriptor-gather-experiment.md](descriptor-gather-experiment.md). The heavier capture
can stay disarmed during off/shadow/on comparisons.

```powershell
.\ufc5\tools\profile-production.ps1 -Action Launch -FrameCount 64 -CpuDetailEvery 8 -FineCpu -Lifetime
# Reach the same paused fight, then arm and analyze as above.
```

`KYTY_GPU_PROFILE_FINE_CPU=1` enables finer RAII scopes in state/shader preparation,
descriptor preparation, image/buffer/sampler lookup, dirty synchronization, allocation,
and metadata handling. Within eligible epochs, a deterministic mixed counter selects
outer fine scopes with probability 1/32; all descendants of a selected scope are timed.
This avoids fixed-period aliasing with the state/bindings/commit call sequence. The
manifest records `fine_cpu_root_every=32`. Coarse draw-phase counters and intervals
keep their existing epoch sampling. Fine call counts are **recorded samples**, not a
full census. Inverse-probability epoch contributions are estimates and retain probe
overhead. Self time excludes direct instrumented children, known waits and separately
recorded fingerprint/byte-comparison work. Uninstrumented children remain in self time.

The analyzer checks scaled fine totals against the measured phase envelopes. It
withholds scaled budgets when they exceed an envelope by more than 25%, or when no
matching envelope exists. Raw call observations remain available. In the corrected
fight capture, state, bindings and vertex/index fail this check; sampling outliers and
unaccounted probe emission prevent using those estimates as a frame budget. See
[readback-cpu-investigation-2026-10-05.md](readback-cpu-investigation-2026-10-05.md).

`KYTY_GPU_PROFILE_LIFETIME=1` watches 512 KiB regions beginning at `0x1164b80000`,
`0x1140008000`, `0x1167f00000` and comparison `0x1165e00000`. It records operation
context even for unsampled CPU calls. The original read request, widened drain window,
packed payload, timeline waits, GPU transfer-stage copy brackets and each published
span are distinct events. A new diagnostic transaction identifies each watched request.
Prepared buffer readers/declared writers are marked immediately before actual command
emission, with shader and guest submit IDs; copies/fills/uploads have range markers.
Declared writable descriptors can overapproximate actual stores. Opaque address writers
are explicitly unknown, and CPU write coverage is incomplete. Absence of a marker is
not proof of immutable contents.

Already downloaded synchronous spans are compared by exact `memcmp` with the previous
same-address, same-size span; their XXH3 fingerprints are also recorded. No extra guest
memory reads or GPU copies are introduced. Comparison storage is bounded to 128 spans,
at most 1 MiB each. Async publication is not compared. The first/changed/equal/untracked
events are per span, not per entire packed readback. Diagnostic comparison cost is
recorded separately. Request faults only know the faulting byte, not access width.
Conditional, indirect and copy PM4 handlers supply context where available; consumer
markers do not guarantee coverage of every resumed faulting instruction.

Extra exports: `focused-report.md`, `cpu-fine-calls.csv`, `cpu-fine-summary.csv`,
`cpu-fine-per-epoch.csv`, `cpu-sampling-closure.csv`,
`cpu-fine-counters.csv`, `cpu-repetitions.csv`, `readback-lifetimes.csv`, and
`readback-addresses.csv`. Repetition keys are argument/fingerprint **subsets**; shader
hash, vertex program ID, descriptor words or identical bytes are not complete cache
validity keys. Lock acquisition scopes report elapsed acquisition, not verified blocked
contention. Image allocation's parameter is pixel volume, not byte size.

The first finer capture (`production-20261005-202527.csv`) recorded every fine call
in selected epochs and overflowed the 100,000-event buffer. It supplies partial lifetime
evidence but cannot give a complete CPU ranking. The corrected build samples outer
calls as described above; dropped epochs are excluded, never silently treated as zero.

**Counter semantics:** `GetFrameNum()` reads `m_done_num`, incremented when a guest
SuspendPoint is enqueued. It is an epoch hint, not the completion of a presented frame.
Both scheduler traces use that hint. An epoch can contain multiple presentations; use
the presentation events and wall time for a displayed rate. Do not calculate game FPS
as `1000 / producer epoch wall_ms` without checking the presentation count.

## Trace files

The producer owns the base CSV. Other scheduler instances get
`.scheduler-N.csv`; `.meta.csv` identifies each role and native Vulkan queue. This avoids
the old producer/presentation CSV overwrite. Ticks and transaction IDs are local to a
scheduler; always join them with scheduler identity.

| File suffix | Contents |
|---|---|
| base CSV | GPU scopes, raw timestamps, stage bits, shader/payload, batch tick, transaction and split status |
| `.cpu.csv` | Epoch wall time, draw phases, submits/waits, query/event drops and barrier resource-entry counts |
| `.events.csv` | CPU intervals and synchronization/resource records, thread IDs, call sites, masks and layouts |
| `.submits.csv` | Build-start, submit-start, queue-entry, submit-end, previous submit, dependencies and guest command counts |
| `.meta.csv` | Scheduler role, queue family and native queue handle |
| `.lifecycle.csv` | Device shutdown wait after renderer schedulers have been destroyed; no gameplay epoch attribution |
| `.manifest.json` | Launch identity, options and initial append-only FPS-log offset |

The analyzer creates `report.md`, `per-frame.csv`, `transactions.csv`, `readbacks.csv`, `wait-sites.csv`,
`barrier-sites.csv`, `gpu-scopes.csv`, and a disk-backed `events.sqlite` analysis cache.
Detailed windows can produce large files. Analysis runs after capture so parsing does not
compete with the game during measurement.

## Detile transaction scopes

`upload_transaction` provides an outer transaction ID and guest address; nested
`detile_transaction` measures detile CPU entry/exit. `detile_record_cpu` identifies command
recording after scratch preparation. Submits and actual timeline waits are independently
recorded and joined by scheduler/batch tick. A detile function generally returns before
its Vulkan command buffer is submitted; a later batch submission is not an in-function
CPU point.

The transaction export includes C0 (outer upload entry), C1 (detile recording entry),
C2 (batch queue submission), C3/C4 (actual host timeline waits for that batch), and
C5 (outer upload exit). C2 may follow C5. C3/C4 may belong to the background completion
worker; their thread IDs are exported and must not be treated as foreground blocking.
The CPU and GPU endpoints use different clocks.

`buffer_readback_sync` records the synchronous copy/submit/wait/publish transaction,
including its guest address and packed copy byte count. `buffer_readback_async` records
enqueue time; deferred publication is outside that CPU scope. `readbacks.csv` joins
waits on the same thread inside each recorded transaction and exports source metadata.

`upload_source_allocation` and `readback_source_allocation` identify the VkBuffer handle
and capacity. A companion `allocation_memory_properties` row at the same CPU timestamp
carries **VkMemoryPropertyFlags in resource** and **memoryTypeIndex in bytes**. These
fields are metadata, not a second buffer or a byte count. They come from cached VMA
allocation information. Flags identify the allocation type, not current physical
residency; an unchanged type does not rule out driver migration or preemption.

| GPU scope / marker | Meaning |
|---|---|
| `detile_pre` | TOP_OF_PIPE before dependencies to COMPUTE_SHADER before dispatch; includes the clear and its barrier |
| `detile_clear` | TRANSFER timestamps around fillBuffer |
| `detile_dispatch` | COMPUTE_SHADER timestamps around the dispatch group |
| `detile_post` | TOP_OF_PIPE / BOTTOM_OF_PIPE around the existing compute-to-consumer barrier |
| `copy_detile_retain` | TRANSFER around the reusable linear buffer copy |
| `upload_pre` | Dependency/transition elapsed around the upload pre-barrier |
| `copy_buffer_to_image` | TRANSFER around the actual upload |
| `upload_post` | Existing transition into general consumer layout |
| `resource_ready` | Queue-stage readiness observation after image upload/transition; not a per-resource hardware completion signal |
| `consumer_draw` / `consumer_compute` | Stage marker immediately before first traced use through an image binding |

The consumer tracker is best effort. It tracks an uploaded image handle through texture,
storage and render-target binding to a subsequent draw/dispatch. It does not prove which
shader instruction reads it, and a target reference may write rather than read. Handle
retirement and window completion clear pending tracking. Missing consumers are reported
as missing, not as zero latency. Repeated detiles inside one upload transaction are grouped.

Timestamp pages are reset only at the start of a command buffer, outside dynamic
rendering. Completed timeline ticks gate reuse. Query collection uses availability bits
without a query wait. There are 64 pages, 256 scopes per command buffer, 16,384 scopes
in flight per scheduler. Exhaustion drops measurements, not GPU work; counts are reported.
A timer crossing a command-buffer boundary is closed and marked split; its stale token
cannot end a query in a later command buffer. Split scopes are excluded from duration unions.

## Synchronization audit

The inspected production renderer and presentation path use one native Vulkan queue
and family. Guest graphics and compute queues are logical command-processor lanes; a
graphics/compute/mixed submission classification does not mean separate native queues.
Every CommandScheduler queue submit currently contains one command buffer.

| API / wait | Observed architecture and hooks |
|---|---|
| vkWaitSemaphores | MasterSemaphore::Wait records only actual blocking API calls, semaphore and target tick, source location and CPU duration. Includes stream-buffer wrap, fault parser, readback, descriptor retirement and priority worker waits. |
| Blocking condition waits | Priority callback completion/drain, optional draw-commit worker, guest command-processor idle/blocked waits and foreground flip completion are scoped. Background thread totals are separate from foreground wall time. |
| vkQueueWaitIdle | Swapchain destruction/recreation, recorded if inside an active window. No steady-state scheduler queue-idle call found. |
| vkDeviceWaitIdle | WindowContext shutdown, after renderer destruction; recorded in lifecycle sidecar when profiling is configured. |
| vkWaitForFences / Vulkan events | No production renderer/presentation calls found in the source audit. Zero observed calls must not be generalized to external drivers/layers. |
| Queue semaphore dependencies | Submit wait handles/values and stage bits are recorded, including presentation acquire dependencies. |
| Pipeline barriers | Legacy and synchronization2 barriers preserve exact arguments and log each global/buffer/image entry, masks, layouts, families and resource size where known. Image size is the guest backing extent, not an exact Vulkan allocation size. |
| Queue family transfer | Source/destination family fields are retained and non-ignored differing families flagged by the analyzer; no separate queue-family architecture found. |
| Flush/invalidate | Buffer::Flush/Invalidate records actual noncoherent VMA calls. Coherent/zero-size no-ops perform no Vulkan operation and are omitted in the lighter profiler. |

An ALL_COMMANDS bit on either side is flagged broad. Both bits are additionally flagged
ALL_COMMANDS→ALL_COMMANDS. Counts are **resource entries**, not Vulkan barrier call counts:
one barrier command with three buffers produces three rows. The command processor's
EmitGlobalBarrier is included. Counts reveal conservatism but do not measure its cost.

## Interpreting time correctly

Vulkan timestamps describe when a pipeline stage reaches the timestamp, not time spent
actively executing a shader. A COMPUTE_SHADER bracket can include waits or preemption.
TOP_OF_PIPE→BOTTOM_OF_PIPE can include earlier work draining; neighboring scopes at
different stages can overlap. Do not subtract their lengths and call the remainder
"barrier cost". The analyzer reports dispatch **elapsed scope**, clear/copy scope,
pre/dependency elapsed and unclassified batch elapsed with these limitations.

CPU intervals use steady-clock nanoseconds; GPU intervals use the device timestamp
period and valid-bit mask. There is no calibrated host/device clock mapping here.
GPU raw ticks from the same native device queue can be merged across the two schedulers,
but CPU and GPU budgets cannot be added. The analyzer unions overlapping intervals and
excludes partial boundary epochs from aggregates.

Submission starvation has two observations:

1. CPU submit spacing and when one scheduler has observed its previous tick completed.
   That scheduler can be empty while the other still has GPU work.
2. A host completion observation for the **latest captured native-queue batch across both
   schedulers**, before the next CPU submit starts. This provides a conservative interval
   with no outstanding captured command buffer. It does not prove the whole device is
   idle, rule out another process, or identify why no work was submitted.

Uncovered gaps between GPU submit timestamp envelopes are another coverage signal,
not a hardware utilization counter. Broad barriers plus long elapsed compute scopes are
insufficient to assign causation. If scopes remain inflated with small measured host
waits and small submission gaps, a GPU execution trace is the next measurement.

`profile_collect_cpu` and `profile_flush_cpu` expose query collection and serialization
cost. CPU scopes remain inclusive; the analyzer subtracts explicit waits, submit intervals
and measured profiler work from its command-processor CPU attribution. Unmeasured hook
overhead and GPU timestamp overhead still require comparing capture-off and capture-on
rates in the same scene. Never use a validation-layer run as the game performance baseline.

CPU records optionally include `probe_end_ns`, the clock after record construction,
recorder lock and vector append. The analyzer unions these emission intervals across
both schedulers on the same thread, subtracts them from foreground/finer attribution,
and reports `profile_record_emission_cpu_ms`. Older rows default to zero; unobserved
constructor/lock-release/timer overhead remains. The separate bounded
[materializer observer](materializer-expression-profile.md) collects an all-call
census and sampled expressions without the heavier GPU profile being armed.

References: [Vulkan timestamp semantics](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp.html),
[timestamp query sample](https://docs.vulkan.org/samples/latest/samples/api/timestamp_queries/README.html),
[pipeline barriers sample](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_barriers/README.html).

## Verified offline

- Emulator and headless harness build with the existing dirty checkout preserved.
- All eight production fight captures still return reference MATCH (five measured,
  two warmup iterations); isolated medians remain approximately 0.08–0.48 ms.
- Opt-in query test: 1,024 detile iterations, with profiling restricted to synthetic
  epochs 1 and 2. Query page reuse, intentional exhaustion and a split/stale scope pass.
  Intentional exhaustion and one split are visible (46 drops in the initial build,
  47 in the final lighter build); epoch 2 has no drops. Output MATCH.
- Final lighter build: the same 1,024-iteration query test with the installed Khronos
  validation layer and `VK_LAYER_VALIDATE_SYNC=1`: output MATCH, no warnings or errors.
- Analyzer regression tests cover overlapping attribution, background/foreground wait
  separation, timestamp wrap, multiple schedulers/sidecar discovery, and unavailable
  CPU detail on unsampled epochs, fine scope nesting, targeted readback lifetimes and
  rejection of invalid scaled budgets, recorder emission exclusion and separate
  materializer thread/root categories. All nine tests pass. Run
  `python ufc5/tools/test-production-profile.py`.

For the measured fight results and remaining unknowns, read
[production-profile-report-2026-10-05.md](production-profile-report-2026-10-05.md) and
the [2026-10-06 follow-up](measurement-follow-up-2026-10-06.md).

The previous 31.944 / 14.427 ms detile results were from a separate headless replay
process running alongside UFC 5, not direct production detile scopes. They establish
contention-dependent latency, not a measured production barrier/dependency cause.
