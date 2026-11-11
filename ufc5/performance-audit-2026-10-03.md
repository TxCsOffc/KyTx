# UFC 5 performance and threading audit — 2026-10-03

Latest measurement update (2026-10-05): read
[the production profiling report](production-profile-report-2026-10-05.md).
It separates foreground waits from host work, adds transaction/readback resources,
and records profiler overhead and coverage. Historical "frame" GPU totals below
use the guest counter; that counter advances on SuspendPoint enqueue and must not
be assumed to equal presented-frame count. The new capture observes about two
presentation calls per epoch. This note does not retroactively infer the ratio for
each historical run.

## Decision

At 4–6 FPS, UFC 5 needs roughly **5–8 times** more throughput to approach 30 FPS.
Later GPU timestamp captures narrowed the large, intermittent GPU cost to
Kyty's internal detile passes: about 331 ms/frame in the slow state versus
18 ms/frame in the fast state. A follow-up split attributes most of that jump
to the detile dispatch interval. The earlier synchronous metadata readback is
real, but its GPU copy is about 0.3 ms/frame in both states; it does not explain
the large fast-to-slow change. See runs 16–17 below for the current priority.

Update 2026-10-05: a 1080p guest output report reduced actual internal surfaces
and brought driver usage below its budget, but the user reported identical FPS
and a black/cyan scene regression. This weakens shared-VRAM spill alone as the
dominant cause. Prioritize command ordering, resource transitions and CPU/GPU
serialization around detiles; isolated replay is fast. The corrupted image limits
the performance comparison, and no rendering cause has yet been established.

## Scope and evidence

- Build: `ufc5-v2` working tree, current sparse BDA experiment enabled
  (`KYTY_SPARSE_BDA=1`), shader optimization `None`, shader validation on. The user
  held the game on a visually stable pause-menu fight scene and reported 4–6 FPS.
- The present-rate log showed 3.63, 4.75 and 5.73 FPS in its first three marked
  10-second windows before debugger sampling. Later windows are affected by repeated
  debugger attaches and should not be used as a benchmark.
- Thirty one-second `nvidia-smi` samples in that scene: **67.9% mean GPU utilization**
  (36–92% range), **93.1 W mean board power**. These are whole-device counters, not
  per-process attribution. The emulator consumed **0.74 CPU cores** over that interval.
- A separate 12-second per-thread CPU sample found guest GPU thread TID 30008 at
  **0.56 core**; the next six threads each used 0.02–0.03 core. The process had about
  84 threads. Spare cores exist, but the recording thread was often waiting.
- This was a paused fight scene. An active fight can change draw count, dispatch
  mix, and metadata traffic, so the ranking needs confirmation there.
- Fifteen all-thread CDB snapshots and twelve deeper guest-GPU-thread snapshots were
  taken 2 seconds apart. **13 of 27 snapshots** caught that thread in
  `BufferCache::DownloadBufferMemory<false>` → `CommandScheduler::Wait` →
  `MasterSemaphore::Wait`. **Seven of the twelve deeper snapshots** trace the caller
  specifically to `TextureCache::MaterializeColorClear` while resolving a render
  target. These are snapshots, not a count of distinct readbacks or an exact time
  percentage; adjacent snapshots can capture the same long wait.
- In the same scene, texture census showed roughly **3,800 images / 3.2–3.3 GB**,
  local driver usage **7.5–7.6 GB** against a **~7.15 GB budget**, and Windows shared
  GPU memory near **1.78 GB**. The prior sparse-table A/B removed ~0.6–0.7 GB of
  shared use without an FPS gain; memory pressure remains relevant, but spill alone
  has not explained 4–6 FPS.

Raw evidence: `D:/PS5/ufc5-audit-2026-10-03-gpu-cpu.csv`,
`D:/PS5/ufc5-audit-stacks-2026-10-03/`,
`D:/PS5/ufc5-audit-gpu-thread-2026-10-03/`, `D:/PS5/fps-pressure-test.txt`, and
`D:/PS5/tex-census.txt`. Historical A/B results are in `ufc5/ledger.md`.

## Critical path found in the current build

```text
guest PM4 submission
  → one GuestGpu worker processes draws and compute
  → DrawIndex / DrawAuto holds RenderContext::m_mutex
  → PrepareDrawRenderState → ResolveRenderColorTarget → FindImage
  → MaterializeColorClear checks DCC/CMask metadata
  → ReadMemory → GPU-to-host buffer copy → submit current command buffer
  → wait for its timeline tick → inspect metadata on CPU → resume the draw
```

`MaterializeColorClear` (`textureCache.cpp:1120–1190`) runs from `FindImage`
(`textureCache.cpp:1317`), including render-target lookup. If **any part of the
metadata range** is GPU-dirty, it reads back the range before checking the selected
slices. It then examines the first byte and every byte in each 4 KiB metadata slice
to recognize uniform fast-clear codes, clears the native image, and writes an expanded
marker into guest backing. This preserves correctness because native Vulkan color
draws do not produce PS5 DCC/CMask state. The transfer cannot simply be deleted.

`BufferCache::ReadMemory` (`bufferCache.cpp:313–335`) widens the requested range toward
a 512 KiB nearby window when the containing buffer allows it. Its synchronous download records barriers and a
copy, then waits for the current graphics timeline tick (`bufferCache.cpp:178–253`).
`CommandScheduler::Wait` submits the current command buffer when that tick has not
yet been submitted (`commandScheduler.cpp:194–208`). The wait can therefore include
earlier graphics/compute work on the same queue, not just the metadata copy. The
guest GPU thread cannot prepare the next draw while it waits.

This is distinct from the older branch's guest-CPU reads of GPU-produced indirect
arguments. Those older experiments showed that deferring a generic readback moved
time between counters and yielded little or no end-to-end gain. The present trace
locates a specific **renderer-internal color-metadata consumer** on the current
branch, so it deserves a focused measurement before borrowing a generic solution.

## Ranked opportunities

| Priority | Opportunity | Evidence and possible gain | Constraint / proof needed |
|---|---|---|---|
| **1** | Remove repeated CPU readbacks for color fast-clear metadata | 7/12 deep snapshots in this exact path; the wait blocks the only draw-recording thread and may drain preceding GPU work. This is the best evidenced current target. | Preserve DCC/CMask clear semantics. First count dirty checks, readback calls, distinct ranges, producer types, bytes, wait time, and successful uniform clears per frame. Track known GPU fill patterns in a CPU shadow; use the existing path for unknown writes. If unknown writes dominate, investigate a GPU-side metadata resolve. A worker that merely waits for the same value will not unblock the dependent draw. |
| **2** | Attribute actual GPU time to shaders, draws and dispatches | GPU utilization averaged 68%, with 92% peaks. Removing a CPU wait may expose a GPU limit. Historical GPU timestamps showed that one old structurizer fix made three pixel shaders 80–220 times cheaper per draw, but the analogous pre-pass on this upstream `GotoStructurizer` was already tried and gave **no FPS gain**. | Add low-overhead command-buffer and per-pipeline GPU timestamps in the **current** build; record shader hash, draw/dispatch count, triangles/pixels, and frame totals. Do not assume the old shader result transfers. Seek a repeatable scene-normalized GPU bottleneck before modifying shader code. |
| **3** | Reduce unnecessary draws, if a real visibility path is missing | Thousands of draws per frame were observed in earlier fights, so a large reduction could improve both CPU prep and GPU work. `graphicsRun.cpp:1352–1375` currently publishes synthetic **always-visible** results for event `0x39` occlusion queries. | Count event `0x39` in this UFC 5 fight and show that its results actually control draw submission. The game also has a software occlusion compute shader that now executes on this branch; do not equate that with the synthetic query path. Implement real host queries only if a trace proves meaningful culling is lost. |
| **4** | Parallelize independent command preparation | `GuestGpu::ThreadRun` (`graphicsRun.cpp:442–539`) serially processes graphics and compute submissions. Presentation and priority completion callbacks already have separate threads. | The measured worker used only 0.56 core while it waited; more workers cannot erase a same-draw readback. `DrawIndex`, `DrawAuto`, `DispatchDirect`, and `DispatchIndirect` hold `RenderContext::m_mutex` across preparation, and `BufferCache` relies on GPU-thread serialization. Parallelism requires splitting immutable PM4 decode/preparation from ordered cache mutation and command recording, with per-queue dependency tracking. Benchmark only after priority 1, or if profiling shows substantial *independent* work ready during waits. |
| **5** | Narrow per-draw resource work and memory pressure | `PrepareBda` walks mapped ranges and synchronizes cached buffers when a shader uses DMA (`renderContext.cpp:121–130`, `descriptors.cpp:918–949`); it appeared in one all-thread snapshot. Texture lookup, SRT evaluation, and deferred image destruction also appeared. Driver local use remains ~0.3–0.5 GB over budget. | Add phase timings and counts before optimizing. Prior current-branch measurements put `MaterializeResources` at only ~4% of wall time, sparse BDA cut shared use with no FPS gain, a texture-eviction scan regressed FPS, and PR #1015 shader optimization was neutral. These are not credible standalone 6× levers today. |

## Threading assessment

The source does have thread capacity: a guest GPU worker, separate presentation
thread, priority completion runner, game threads, and roughly 84 process threads in
the sampled scene. The CPU was not saturated. The problem is the dependency chain on
the guest GPU worker. A draw holds the renderer mutex while it may ask for GPU-produced
metadata; `ReadMemory` then waits on the graphics timeline before the draw can finish.
Adding a thread to copy bytes after the GPU finishes would save only the CPU copy and
cannot let this draw use the missing metadata. Making another queue useful during the
wait needs narrower locks, thread-safe cache ownership, and proof that its commands
are independent. This is a substantial architecture change, not a thread-count knob.

Shader compilation can run on workers to reduce first-use loading stalls, but no
sampled stack in the paused scene was compiling a shader. It is not the identified
steady-frame bottleneck. The earlier `KYTY_DEFER_READBACK`, transfer-queue, and producer-early-flush
experiments in the ledger did not yield a large end-to-end UFC 5 win and should not
be repeated unchanged.

## Next experiment, with a stop condition

1. Add opt-in `ColorMeta` counters around `MaterializeColorClear`: frame number,
   metadata address/size/kind, selected slices, GPU-dirty status, producer category,
   bytes copied, wait milliseconds, uniform-code result, and whether this range was
   already checked in the frame. Keep aggregate output low overhead.
2. Measure a stable pause scene **and an active fight** after warm-up. Report median
   milliseconds per frame, draws/dispatches per frame, readback wait per frame, GPU
   busy time, local/shared memory, and visual/validation status. Normalize by draw
   count when scenes differ. CDB snapshots identify a lead; timers and counts size it.
3. If known constant fills dominate, shadow their DCC/CMask value and invalidate that
   knowledge on every unknown GPU/CPU write. Only the known case bypasses readback.
   Require identical image output and shader validation before calling it a fix.
4. If readback time falls but frame time does not, stop pursuing this route and use
   GPU timestamps to find the exposed cost. If the metadata writes are mostly unknown,
   stop the shadow approach and assess GPU-side resolve instead.

At a representative 4.5 FPS, a 30 FPS target means cutting a ~222 ms frame to
~33 ms. The stack samples make a color-metadata fix promising, but they cannot prove
that one change alone supplies the required ~189 ms saving. The report deliberately
separates the observed wait from that unproven total-frame payoff.

## Follow-up: per-frame metadata profile (2026-10-04)

An opt-in profile in `TextureCache::MaterializeColorClear` now writes one CSV row per
frame with metadata checks, dirty readback calls, requested bytes, elapsed time in
`ReadMemory`, distinct metadata ranges, and successful uniform clears. It is enabled
with `KYTY_COLOR_META_PROFILE=<output.csv>`. A second opt-in event trace,
`KYTY_COLOR_META_TRACE=<output.csv>`, records each `BufferCache::FillBuffer` DMA
constant fill and each dirty DCC/CMask readback with address, range size, frame, and
readback time. Both are off by default. The emulator built successfully with clang-cl.

The first instrumented run reached a repeatable paused-fight pattern for 414 frames:
**1,800 metadata checks, six dirty readback calls, 311,296 requested bytes, and four
successful uniform clears per frame**. Time inside the six `ReadMemory` calls averaged
211 ms and had a 280 ms median, varying substantially as queued GPU work changed.
The six calls are serial on the guest GPU thread; their elapsed time includes the
GPU work submitted before each copy, so 211 ms is **not** an independently removable
CPU cost or a predicted FPS gain. Raw data: `D:/PS5/ufc5-color-meta-profile-run1.csv`.

In the second, event-traced run, a recent eight-second window produced 21 frames
(2.62 FPS) and averaged 83 ms per frame inside metadata `ReadMemory`. The recurring
pattern was two reads each of three CMask ranges. The larger two ranges consumed
most wait time. **No DMA constant-fill event occurred in the event trace**, while
thousands of dirty metadata reads were recorded. The proposed constant-fill shadow
therefore has no observed target in this scene; it was not implemented. Raw data:
`D:/PS5/ufc5-color-meta-profile-run3.csv` and
`D:/PS5/ufc5-color-meta-trace-run3.csv`.
The emulator's diagnostic log also labels one recurring CMask address
(`0x1164be0000`) as a sampled compute texture address. That may reflect guest
memory aliasing or reuse; it does not by itself identify the write producer.

The event trace's first build crashed on Windows because it called `setvbuf` after
writing the CSV header. The crash dump showed `ucrtbase!setvbuf` calling the invalid
parameter handler. That call was removed and the corrected third launch has recorded
frames normally. The failure was confined to the trace build; the first profile run
completed before it.

**Next experiment:** find the producers of those three CMask ranges, including
`CopyBuffer`, shader storage writes, or GPU buffer device address writes. Do not treat
a previous clear value as current without tracking every intervening writer. If the
ranges are shader-produced, prototype a GPU-side CMask resolve: reduce each metadata
slice to a uniform-clear predicate on the GPU, then apply the register clear through
a conditional image draw, retaining the existing CPU readback as a fallback for
unsupported formats. Time that path and the whole frame with GPU timestamps. The
observed readback elapsed time is only an upper bound on potential wall-time savings
because the underlying GPU work still has to finish before presentation.

## Follow-up: proven compute fill and live fight A/B (2026-10-04)

The producer trace corrected the initial DMA-only conclusion. The repeated shader
`0x723423f5115aedbd` is a compute **uniform buffer fill**. The existing
`ResourceSnapshot::uniform_fill` analysis proves its value and full descriptor
coverage. An opt-in `KYTY_COLOR_META_CPU_FILL=1` path now consumes that dispatch
only when its output exactly matches a registered, page-aligned CMask range. It
writes the proven dword pattern into guest backing, transfers tracker ownership
to the CPU, and invalidates texture aliases. Unsupported dispatches retain the
original GPU path. The current recognized fight fill writes zero to a 36,864-byte
CMask range.

The `KYTY_COLOR_META_CONTROL_FILE` setting allows this experiment to be switched
in the running emulator. `ufc5/set-color-meta.ps1 on|off|status` changes the file;
the guest GPU thread polls it at most every 500 ms. In run 8, `T` trace events
confirmed both transitions and `P` events confirmed proven fills only while on.
The user confirmed the paused fight looked the same with the switch on.

In the same paused fight, 12-second windows gave the following results. The
baseline and experiment both varied substantially, so these are observations,
not an estimate of a stable performance improvement:

| Phase | FPS | Dirty CMask reads/frame | Time inside reads/frame |
|---|---:|---:|---:|
| off 1 | 2.74 | 6.18 | 40.6 ms |
| on 1 | 1.92 | 4.00 | 192.5 ms |
| off 2 | 1.67 | 6.00 | 262.3 ms |
| on 2 | 2.67 | 4.00 | 49.6 ms |
| off 3 | 1.92 | 6.00 | 189.9 ms |

The bypass reliably removes two of six readback calls per frame but does **not**
show the massive end-to-end gain needed for playability. The remaining two CMask
ranges (`0x1164be0000`, 81,920 bytes; `0x1167f00000`, 36,864 bytes) are each read
twice per frame. The trace shows both overlap the large writable storage ranges
bound to compute shader `0xea0aceac518ec52d`; the first also has the recognized
uniform-fill shader, but that dispatch did not pass the full-coverage and exact
registered-CMask checks needed for the CPU bypass. The trace establishes the
overlapping writable bindings, not which bytes that shader changed. Those ranges
cannot safely be treated as constant metadata from this evidence. Resolve why the
larger uniform-fill dispatch is rejected, then assess a GPU-side metadata resolve
or a change to the producer/consumer representation for the rest.

Run 8 used the dense 768 MB BDA table: local driver usage was roughly 8.2 GB
against a fluctuating 7.0-7.1 GB budget. Its absolute FPS is therefore below some
earlier sparse-table fights and should only be used for this same-process toggle
comparison. The earlier sparse-table A/B already showed that a 0.6-0.7 GB reduction
in shared memory did not move the fight out of the 3-6 FPS range. Next, collect
GPU timestamps around the occlusion compute dispatch, metadata producer work,
readback submission, and main draw/submit stages. The readback wall time includes
queued GPU work and cannot alone distinguish a GPU bottleneck from a CPU wait.

## Follow-up: GPU/CPU frame timing (2026-10-04)

An opt-in Vulkan timestamp ring (`KYTY_GPU_TIMING_CSV`) measures whole command-buffer
submissions, selected compute dispatches, and the GPU transfer portion of buffer
downloads. A separate CPU CSV records frame wall time, draw/dispatch counts, number
and CPU time of `vkQueueSubmit` calls, and time in graphics-timeline waits. The
timestamp capture can be switched with `KYTY_GPU_TIMING_CONTROL_FILE`; its off/on
paused-fight windows were **2.99/2.91 FPS**, with nearly identical 4,875 draws and
about 335 submissions per frame. Thus enabling this capture did not materially
change the frame rate in the first paired windows. Raw data:
`D:/PS5/ufc5-gpu-timing-run11.csv`,
`D:/PS5/ufc5-gpu-timing-run11.csv.cpu.csv`, and
`D:/PS5/ufc5-color-meta-profile-run11.csv`.

The same paused scene then entered a persistent slower state, at 1.33 FPS both with
timestamps off and on. Draws and submission counts stayed essentially unchanged.

| Paused scene | Wall/frame | GPU submission time/frame | CPU timeline wait/frame | CPU submit/frame | Color metadata readback wall/frame |
|---|---:|---:|---:|---:|---:|
| Fast, timestamps on | 343 ms | 168 ms | 53 ms | 9 ms | 42 ms |
| Slow, timestamps on | 762 ms | 627 ms | 457 ms | 9 ms | 421 ms |

GPU submission times sum Vulkan timestamp intervals for single-queue command
buffers; CPU wall figures are first guest GPU command of one frame to first command
of the next, so the columns overlap and must not be added. The readback copy plus
its barriers used only **~0.4 ms GPU time per frame** in both states. The specific
occlusion compute shader `0xea0aceac518ec52d` was well under 1 ms per frame in
the fast run, and the uniform-fill compute shader was only microseconds per
dispatch. The long readback wall time in the slow state is predominantly a wait
for preceding GPU work, not the 311 KB metadata copy itself.

The remaining CPU-side frame work is substantial: subtracting measured timeline
wait and queue-submit time from wall time leaves roughly **280-300 ms/frame** in
both states. Eighteen guest GPU thread stack snapshots during this run included
six timeline waits and active stacks spread across draw binding/target preparation,
SRT materialization, BDA preparation, cache GC, allocation and submission. No
single CPU hotspot is established by that small sample. Raw stacks:
`D:/PS5/ufc5-gpu-thread-run11/`.

The slow GPU interval is a second bottleneck. In timestamped frames, the long
submission ending at compute address `0x1140724600` grew from roughly 17 to
135 ms per frame, and several draw-related submissions rose by a similar factor.
The GPU was in P0 at about 1995 MHz and 63 C when checked, arguing against a
simple clock collapse. Driver local usage was ~7.6-7.7 GB against a ~7.2 GB
budget, with ~1.9 GB process shared usage. These counters show memory pressure,
but do not prove that paging caused the 4x GPU-time change. The next targeted
capture timed **all guest compute dispatches by shader hash** for a short window.
It did not include renderer-internal detile compute, found in run 16.

### All guest-compute capture in both GPU states

Run 12 enabled timestamps for every compute dispatch in a short live window,
then disabled them again without restarting. In a stable paused fight with
~4,904 draws, ~724 dispatches and ~330 submissions per frame, complete timestamp
frames 1202-1228 and 1902-1908 gave:

| GPU state | Whole submissions | Guest compute | Metadata downloads | Remainder in submissions |
|---|---:|---:|---:|---:|
| Fast | 131 ms/frame | 38 ms/frame | 0.29 ms/frame | ~93 ms/frame |
| Slow | 502 ms/frame | 32 ms/frame | 0.28 ms/frame | ~470 ms/frame |

The `remainder` is whole-submission time minus nested guest-compute and download
timers. It includes internal detiling, graphics draws, uploads, barriers and other GPU commands;
it is not a measured graphics-only timer. The most costly compute hash in the
fast state, `0xe327a0730ba4f226`, used 11.5 ms/frame. The previously suspected
occlusion hash `0xea0aceac518ec52d` used ~0.66 ms/frame. The compute ranking
does not explain either the 3 FPS steady frame or the slow-state collapse;
renderer-internal detile dispatches explain most of the latter (run 16).

One recurring command buffer whose final debug command carries compute address
`0x1140724600` grew from ~11 to ~90 ms/frame across its two appearances per
frame. In a slow frame, each instance took 41-47 ms but its timed compute
dispatches totaled only 3.9-4.4 ms. The final debug command identifies the
buffer, not the command responsible for the unexplained time. The next narrow
measurement should bracket its graphics draws, uploads and barriers separately,
and correlate the GPU-time jumps with WDDM residency/page activity. CPU phase
timers within draw preparation should simultaneously split the persistent
~260-300 ms of non-wait CPU frame time. Raw data:
`D:/PS5/ufc5-gpu-timing-run12.csv` and
`D:/PS5/ufc5-gpu-timing-run12.csv.cpu.csv`.

### CPU draw phases and another fast/slow transition (run 13)

The CPU CSV now aggregates six draw phases: state preparation, resource bindings,
vertex/index preparation, pipeline lookup, render-target acquisition, and Vulkan
binding/draw recording. The counters are enabled only when `KYTY_GPU_TIMING_CSV` is
set. A build with these counters was deployed and run in the same paused fight.
Kyty's built-in RenderDoc mode (`--rd`) failed at Vulkan device creation on this
machine; the successful run did not use RenderDoc.

The live GPU timestamp switch captured a fast → slow → fast transition with
4,664 draws and 700 dispatches per frame in each window:

| Window | Wall/frame | GPU submissions/frame | CPU timeline wait/frame | Draw state CPU/frame | Bindings CPU/frame | Draw recording CPU/frame |
|---|---:|---:|---:|---:|---:|---:|
| fast, frames 1490–1494 | 344 ms | 154 ms | 86 ms | 124 ms | 63 ms | 25 ms |
| slow, frames 1505–1512 | 745 ms | 605 ms | 445 ms | 469 ms | 68 ms | 28 ms |
| fast, frames 1515–1525 | 364 ms | 157 ms | 97 ms | 120 ms | 64 ms | 26 ms |

The draw-state total includes time blocked in GPU timeline waits; these columns
overlap and must not be summed. In the slow window, the wait rose by ~359 ms and
the draw-state total by ~346 ms, while bindings and draw recording stayed near
their fast values. The bulk of the *extra wall time* is waiting for the GPU,
not newly expensive CPU state work. The fast window still has roughly 250 ms of
frame wall outside timeline waits and `vkQueueSubmit`; bindings (~63 ms) and draw
recording (~25 ms) are measured pieces, with remaining CPU work in other phases
and guest command processing.

In representative frame 1491, an 81-draw submission took 8.8 ms and each of the
two 4-draw/8-compute submissions ending at `0x1140724600` took ~10 ms. In slow
frame 1508, the same 81-draw submission took 54.7 ms and those two mixed
submissions took 57–61 ms. Other 1-draw/2-compute submissions also grew to
~51–56 ms. Shader and metadata timers in run 12 already ruled out compute and
metadata copies as the main extra cost. The last debug operation is an identifier
for a submission, not attribution to that operation.

Windows GPU-process counters sampled during the second capture showed local
usage fluctuating around 6.1–6.4 GiB and nonlocal usage around 1.6–1.9 GiB.
The abrupt slow/fast changes occurred while those usage counters moved gradually;
this neither proves nor excludes residency/page migration. A detailed WPR GPU
trace was attempted but Windows rejected it with `0xc5585011` (system profiling
policy not enabled). The next diagnostic must time groups of graphics draws and
resource transitions within the recurring long command buffers, or use a GPU
capture tool that supports this Vulkan device. Do that before optimizing a
specific shader or cache path. Raw data:
`D:/PS5/ufc5-gpu-timing-run13.csv`,
`D:/PS5/ufc5-gpu-timing-run13.csv.cpu.csv`, and
`D:/PS5/ufc5-gpu-memory-run13.csv`.

### Render-pass GPU time separates drawing from the slow gap (run 14)

The next opt-in build places a Vulkan timestamp pair around each dynamic-rendering
pass, with query resets outside the pass as Vulkan requires. It logs the pass's
submission tick, dimensions, and draw count. The user returned to the paused
fight; two live captures in the same process caught the slow and fast GPU states:

| Window | Wall/frame | Whole GPU submissions/frame | GPU time in rendering passes/frame | Timed selected compute/frame | GPU metadata downloads/frame | CPU timeline wait/frame | Draws/frame |
|---|---:|---:|---:|---:|---:|---:|---:|
| slow, frames 1973–1984 | 635.3 ms | 479.8 ms | 34.1 ms | 0.7 ms | 0.27 ms | 351.2 ms | 4,790 |
| fast, frames 2170–2195 | 345.4 ms | 137.9 ms | 31.8 ms | 0.6 ms | 0.29 ms | 73.4 ms | 4,794 |

This capture timed only the previously selected guest compute hashes; run 12
measured all **guest** compute at ~32–38 ms/frame in both states. It did not
include Kyty's internal detile compute. Rendering passes themselves are
also stable across states. Thus the **~342 ms increase in whole-submission GPU
time** is overwhelmingly outside rendering passes, timed compute, and metadata
copies. It is in non-render Vulkan work or GPU stalls between commands: resource
uploads, layout/ownership barriers, other copies, residency/page-in, or queue
bubbles. These alternatives require finer command-interval timing before a code
change is justified.

Frame 1979 shows the pattern inside individual submissions. A 1-draw/2-compute
submission took 60.56 ms, but its sole rendering pass took 0.03 ms. Two recurring
4-draw/8-compute submissions each took 43–45 ms, with just 0.13 ms in their
rendering passes. The two 82-draw submissions took 40–41 ms, with 2–3 ms in
rendering. The debug argument is only the final guest command's address, not the
source of the elapsed time.

Next bracket the non-render intervals within these submissions and instrument
the resource-copy/barrier call sites in the interval that expands. This should
decide whether there is a specific oversized transfer/synchronization operation
or a device-residency stall before optimizing. Raw data:
`D:/PS5/ufc5-gpu-timing-run14.csv` and
`D:/PS5/ufc5-gpu-timing-run14.csv.cpu.csv`.

### Non-render gaps inside the slow submissions (run 15)

An opt-in follow-up brackets every interval outside dynamic-rendering passes
within each command buffer. It also closes render/gap timers before the enclosing
submission timestamp. The user drove back to a paused fight at ~5,046 draws and
724 dispatches/frame. A short live capture caught a slow GPU window; complete
frames 19977–19988 had whole-submission GPU time roughly 449–484 ms/frame,
rendering passes 34–38 ms/frame, and **non-render intervals 409–444 ms/frame**.
The gap timers and render timers nearly partition each submission; small residuals
are timestamp/write overhead and selected nested compute/download timers.

In slow frame 19980, the largest examples are:

| Submission tick | Guest commands in submission | Whole GPU | First non-render gap | Other large non-render gap | Rendering passes |
|---|---:|---:|---:|---:|---:|
| 3787152 | 1 draw, 2 computes | 56.72 ms | 56.67 ms | none | 0.028 ms |
| 3786987 | 1 draw, 2 computes | 56.00 ms | 55.95 ms | none | 0.028 ms |
| 3787139 | 4 draws, 8 computes | 42.60 ms | 23.84 ms | 18.61 ms | 0.126 ms |
| 3786974 | 4 draws, 8 computes | 42.50 ms | 25.17 ms | 17.18 ms | 0.126 ms |
| 3786875 | 128 draws | 41.77 ms | 36.61 ms | none | 4.86 ms |
| 3787038 | 128 draws | 40.52 ms | 36.04 ms | none | 4.47 ms |

The first gap spans command-buffer begin to the first render pass. It may contain
compute dispatches, transfers, barriers, clears, or a residency/page-in stall.
The existing selected guest-compute timer was only ~0.005 ms in the two 1-draw
examples. Run 12's all-guest-compute result missed the renderer's internal
detile compute, identified in run 16 below. These data exclude draw execution
as the cause of the slow state, but do **not** distinguish detile shader work
from memory stalls within its dispatch. Raw data:
`D:/PS5/ufc5-gpu-timing-run15.csv` and
`D:/PS5/ufc5-gpu-timing-run15.csv.cpu.csv`.

### Internal detiling explains most of the GPU swing (run 16)

Opt-in timestamps around `TileManager::Record` captured a slow-to-fast
transition in the same paused fight. The timer brackets the entire detile call,
including its pre-barrier, target clear, compute dispatches, and final barrier:

| Window | GPU submissions/frame | Rendering/frame | Non-render gaps/frame | Detile/frame | Detile calls/frame |
|---|---:|---:|---:|---:|---:|
| slow, frames 1052–1056 | 467.6 ms | 32.7 ms | 432.1 ms | 330.9 ms | 46–77 |
| fast, frames 1058–1060 | 127.2 ms | 32.5 ms | 92.6 ms | 18.2 ms | 47–68 |

The 312.6-ms detile increase accounts for most of the 340.4-ms GPU submission
increase. The same large output sizes and call counts appear in both states.
For example, two 20,889,600-byte detile calls totaled 67.9 ms in slow frame
1053 versus 4.4 ms in fast frame 1059; two 8,847,360-byte calls totaled
56.7 versus 0.8 ms. Size and call count alone do not explain the slowdown.
The `shader` CSV field for these timers is output capacity in bytes, not a
shader hash. Raw data: `D:/PS5/ufc5-gpu-timing-run16.csv` and its `.cpu.csv`.

### Detile stage split and capture crash (run 17)

A diagnostic build split each detile into pre-barrier/target-clear,
dispatch-loop, and post-barrier intervals. Slow frame 3194 had 331.8 ms total
detile across 65 calls: 36.1 ms pre, **294.6 ms dispatch**, and 0.1 ms post.
Frames 3196–3204 then had ~33–35 ms total detile, ~32–34 ms dispatch, and
~1 ms pre per frame. Rendering stayed near 31–37 ms. The slow-state increase
therefore occurs primarily during the detile dispatch interval. GPU timing
cannot by itself distinguish shader computation from memory bandwidth or
residency stalls while the shader accesses its buffers.

The emulator exited/crashed shortly after live timing was enabled. No Windows
Application Error event for it was found. The recording ends partway through
frame 3205, so use only complete frames above. The extra query load or another
interaction in this diagnostic build may have contributed; causation is not
established. The previous run-16 executable was restored to the deployed path,
and the live timing control was set to `off`. Raw data:
`D:/PS5/ufc5-gpu-timing-run17.csv` and its `.cpu.csv`.

Next, correlate repeated `TextureCache::UploadImage` detiles with guest image
address, tile geometry, pipeline slot, and dispatch group count. Test whether
large unchanged surfaces are needlessly re-uploaded, then target reuse or the
specific tiler shader responsible. Preserve a slow/fast comparison and visual
check for any performance change.

### Faster experiment loop: real detile replay (2026-10-05)

The headless Vulkan harness now replays saved logical layouts and actual GPU
input bytes through the production tiler against the current source. Eight large
fight surfaces captured at frames 805–806 are saved in
`D:/PS5/ufc5-replays/fight/`. A fresh process replayed all eight, with three warmup
and ten measured iterations each, in 4.30 seconds. Every output byte matched
the live GPU reference. CSV: `D:/PS5/ufc5-detile-replay-fight-20261005.csv`.

The game remained running, so the 0.153–31.944 ms GPU medians are affected by
concurrent GPU work and cannot serve as an uncontended baseline or explain the
whole-frame slow state. The replay omits full-game memory pressure, image cache
state, conversion passes, draws, and guest CPU execution. Its immediate purpose
is to test tiler changes in seconds without repeating boot and menus, with output
regression checks. Next obtain an uncontended baseline and extend capture to the
surrounding upload/copy sequence when testing that path. Instructions:
[detile-replay.md](detile-replay.md).

After the user closed the game, two fresh-process replays with ten warmup and
fifty measured iterations each matched all eight references and agreed on
0.082–0.477 ms GPU medians. Each complete command took about 1.7 seconds.
The two formerly slow captures went from 31.944 to 0.477 ms and 14.427 to
0.175 ms. Source and inputs were unchanged. CSVs:
`D:/PS5/ufc5-detile-replay-baseline-20261005-a.csv` and
`D:/PS5/ufc5-detile-replay-baseline-20261005-b.csv`.

This supports investigating the surrounding GPU workload and memory state.
It does not distinguish residency from scheduling/contention: closing the game
removed both its allocations and queued work, and the concurrent verification
used fewer iterations. Next introduce controlled, touched memory allocations
and record budgets in the standalone replay to test pressure without loading
the game. If that cannot reproduce the delay, expand command/resource replay.

### Detile identity trace, before the capture (2026-10-04)

Run 16's slow frames 1052–1056 and fast frames 1058–1060 repeat the same output
sizes the same number of times. `0x15f9000` (23,052,288 bytes) occurs four times
per frame and `0x1fe0000` (33,423,360 bytes) twice, in both states. Those
repeated calls are about 50 ms/frame while slow and under 4 ms/frame while fast.
The swing is those uploads getting slower, not a different upload set. Output
size cannot tell whether they are the same guest image.

`KYTY_DETILE_TRACE` now writes one CPU row per detile dispatch: frame, call,
repeat of that guest address within the frame, image slot, address, size,
binding, buffer/CPU/GPU dirty flags, linear bytes, family, element size,
pipeline slot, dimensions, pitch, and group counts. With `KYTY_GPU_TIMING_CSV`
also enabled, the outer `detile` timer's `arg4` is the guest address, so GPU
time joins to that row. Both stay off unless set.

### Fight trace: aliases, not one image uploaded twice (run 18)

Enabling live GPU timing on the stage-split build crashed the emulator after
partial frames 2884–2885, as in run 17. Those timestamp rows are not a
slow/fast measurement. The CPU log and detile trace survived. At 4,834
draws/frame, CPU frames 2876–2879 were 555–675 ms and 2880–2887 were 354–402 ms.
The large surfaces are the same guest addresses on both sides of that change.
In frame 2884 each repeat is a different image slot of that address: the
33,423,360-byte 3840×2160 depth at `0x1168bd0000` is slots 1056 and 2131; each
23,040,000-byte 3200×1800 color target is its own pair; the 11,141,120-byte
2136×1200 depth at `0x11687e0000` is four slots. They are marked buffer-modified,
CPU-dirty, and GPU-modified. The repeated GPU cost is a full detile per alias.
Raw trace: `D:/PS5/ufc5-detile-trace-run18.csv`.
