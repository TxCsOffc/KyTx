# Detile capture and replay

The first replay tool runs the production `TileManager` without loading UFC 5.
Capture a paused fight once, then rebuild and benchmark the saved detiles directly.
This is the first part of a renderer replay workflow, not a full fight save state.

## Saved fight workload (2026-10-05)

Eight real fight captures are already saved in `D:\PS5\ufc5-replays\fight`.
They replayed in a fresh process in 4.30 seconds, with every output byte matching
the captured reference (three warmups and ten timed iterations each). Results:
`D:\PS5\ufc5-detile-replay-fight-20261005.csv`.

The game was still running during verification. For performance comparisons,
close it and other GPU workloads first, keep the capture files fixed, and compare
the same iteration settings before and after a source change. GPU timings from
the initial verification are not an uncontended baseline.

With the game closed, two fresh-process runs (ten warmups and fifty measured
iterations each) matched all eight references and agreed on GPU medians of
0.082–0.477 ms. Each complete command took about 1.7 seconds. Baseline CSVs:
`D:\PS5\ufc5-detile-replay-baseline-20261005-a.csv` and
`D:\PS5\ufc5-detile-replay-baseline-20261005-b.csv`.

To compare against those settings:

```powershell
& D:\PS5\src\KytyPS5\ufc5\tools\replay-detile.ps1 -Build -Warmup 10 -Iterations 50
```

## Capture once

From PowerShell:

```powershell
& D:\PS5\src\KytyPS5\ufc5\tools\capture-detile.ps1 -Launch -Build
```

Drive UFC 5 to the desired paused fight, then:

```powershell
& D:\PS5\src\KytyPS5\ufc5\tools\capture-detile.ps1 -Mode Arm
& D:\PS5\src\KytyPS5\ufc5\tools\capture-detile.ps1 -Mode Status
& D:\PS5\src\KytyPS5\ufc5\tools\capture-detile.ps1 -Mode Off
```

Arming records up to eight distinct guest addresses whose linear outputs are at least
8 MiB, with a total input/reference budget of 512 MiB. Each buffer is limited to
256 MiB. The file switch is checked every 500 ms. Capturing synchronously reads
back the actual GPU input and output, so the capture window is not a benchmark.
Capture polling and readbacks are disabled unless both environment paths are set.

The capture executable is staged as `kyty_emulator.detile-capture.exe` beside the
normal emulator. The launcher does not replace the normal executable. It inherits
other experiment settings, enables sparse BDA if unset, and disables the live GPU
timing ring that crashed in prior runs.

Files default to `D:\PS5\ufc5-replays\fight\*.kdr`. Save and share these as logical
inputs; they contain no Vulkan handles or saved shader binaries. Format version 1
stores little-endian fields, all tile layouts, source alignment prefix, GPU input
bytes, reference output bytes, guest address/frame, and input/output checksums.
Captured reference bytes are an output regression check, not proof that the
original rendering was correct.

## Normal development loop

Change the tiler/shader, then:

```powershell
& D:\PS5\src\KytyPS5\ufc5\tools\replay-detile.ps1 -Build
```

The script rebuilds the headless harness against the current source, replays all
captures, discards three warmup iterations, times ten iterations per capture, and
compares every output byte against the reference. CSV records GPU median/min/max,
wall median, byte counts, layout count, and output match/hash. Override `-Captures`,
`-Iterations`, `-Warmup`, and `-Csv` as needed. A standalone file is also accepted.

Exit 2 means an output mismatch; exit 1 means invalid input or setup. Both become
a PowerShell error. Upload/allocation and the reference readback are outside timed
iterations. GPU timing uses two query slots and waits for completion before reuse.
The production shader is rebuilt; a checkpoint is not tied to its capture build.

The default mode includes the production detile's pre-barriers, target clear,
dispatches and post-barrier, not just the compute shader dispatch. It does not exercise image cache reuse,
D16 promotion, BGRA conversion, draw commands, game CPU work, or full-game driver
residency. Use occasional game runs for visual checks and whole-game FPS. A future
paused-frame replay must capture command ordering, resource updates, and cache
state before drawing performance or VRAM conclusions.

## Depth/stencil upload pair (2026-10-06)

The [same-build live/closed results](detile-upload-pair-results-2026-10-06.md)
are now saved. All eight default detiles match at 0.083–0.478 ms closed; the
constructed upload pair matches both aspects at 2.803 ms. Several concurrent
default replays were much slower. This contrast does not identify its mechanism.

The slow production tick 2794298 contains consecutive detiles of `0x1168bd0000`
(four-byte depth) and `0x116abb0000` (one-byte stencil), both 3840×2160, followed
by uploads whose resource-ready records identify the same native image handle.
Both tile layouts and input/reference bytes are already captured. The new mode
reuses production `TileManager::Detile` **and `Image::Upload`**, including their
barriers, clears, buffer-to-image copies and transitions, in one submission.

```powershell
& D:\PS5\src\KytyPS5\ufc5\tools\replay-detile.ps1 -UploadPair `
    -Captures D:\PS5\ufc5-replays\fight\detile-0000001168bd0000-f805.kdr `
    -StencilCapture D:\PS5\ufc5-replays\fight\detile-000000116abb0000-f805.kdr `
    -Warmup 10 -Iterations 50 -Csv D:\PS5\ufc5-profiles\upload-pair.csv
```

This is a **constructed D32S8 transaction pair**, not an exact replay of the whole
production submission. KDR v1 does not save native image format, original image
state, guest compute/draw commands, native queue dependencies or full residency.
Only single-layout 2D Depth64KB captures with four-byte depth and one-byte stencil
of the same extent are accepted. Pitch and capacities come from each capture.
The first iteration starts from an undefined image; subsequent ones from general.

Five timestamps mark batch entry at TOP_OF_PIPE, then cumulative BOTTOM_OF_PIPE
completion after each detile and upload. Adjacent differences are **elapsed stage
brackets**, including relevant dependencies/stalls, not pure execution or barrier
cost. No host wait occurs between the two transactions. Allocation/input upload,
query retrieval and final output verification are outside the timed batch. CPU
record, combined flush/wait, and wall spans are exported separately per iteration.

After the last iteration, both image aspects are copied back and every active
texel byte is compared to the captured reference. Padding has no image equivalent
and is excluded; default replay still checks the entire linear output including
padding. CSV repeats the final verification result on each row, not a per-iteration
verification. Metadata beside each CSV records executable hash, settings and
emulator processes at start/end. Close the game for uncontended comparisons.

## Correlated standalone replay trace

Default replay now accepts `-ProfileOutput PATH` to reuse the existing production
scheduler profiler. It records submit CPU bounds, queue handle/family, thread,
tick, barriers, waits and per-stage detile scopes. Fine CPU/lifetime tracing and
live control inheritance are disabled for this explicit replay mode. The existing
query-ring stress self-test is separate and cannot be combined with it.

The benchmark CSV gets a `.iterations.csv` sidecar containing warmup/measured
iteration IDs, target ticks, thread, CPU record/flush bounds and raw GPU timestamps.
Iteration rows accumulate in memory and are emitted after each capture's timed
loop. The wrapper records the replay child PID, hash and emulator process snapshots
in `.csv.meta.json`. Scheduler files with role `replay` have `.scheduler-N.csv`
suffixes; the first unsuffixed scheduler belongs to harness setup and can be empty.
The analyzer joins using thread, tick **and submit bounds**, since different
schedulers reuse the same tick numbers.

After returning to a constant paused fight, run in Administrator PowerShell:

```powershell
& D:\PS5\src\KytyPS5\ufc5\tools\capture-gpuview.ps1 -ReplayDetiles -Seconds 30
```

This starts the existing GPUView.light profile, runs all eight saved detiles with
three warmups/ten measured iterations, and records the remaining window. ETL,
replay CSV, scheduler CSVs, PID/hash metadata and QPC boundaries share the output
stem. The game's heavy profiler stays off; `-ArmProduction` cannot be combined
with this mode. The helper preserves unrelated WPR recordings and stops only its
own instance. Preflight with `-CheckOnly` checks the running game, files and WPR.

The helper prefers the installed Windows Performance Toolkit `wpr.exe` and prints
its version. `-WprExecutable` overrides the selection. Builds below 19650 are
rejected because [Microsoft documents stop error 0x80010106](https://devblogs.microsoft.com/performance-diagnostics/wpr-start-and-stop-commands/)
and recommends a newer toolkit. This machine has toolkit build 26100; its older
System32 build 19041 failed both replay trace saves. Start/stop diagnostics and
recorder version/hash are preserved beside each capture. A successful start does
not confirm that the final graphics ETL was saved.

`analyze-replay-profile.py REPLAY.csv PROFILE.gpu.csv --output RESULT.json`
checks application joins, drops and split scopes. It optionally accepts
`--packets NATIVE.analysis.json.packets.json --qpc-zero-ticks VERIFIED_ORIGIN`
after existing GPUView export/analysis. Native joins require a raw-QPC anchor and
the replay PID/thread inside exact submit bounds; ambiguous candidates are
reported and excluded. CPU/ETW QPC alignment does not calibrate the raw GPU clock.
GPU stage brackets and native DMA residence remain elapsed spans, not pure work.

The profiled replay adds CPU emission/query collection and additional stage
timestamps. Use it for attribution; default replay remains the performance control.
Do not treat increased profiled CPU wall time as a renderer regression or game FPS.

Allocation identity tracing uses these same scheduler `.events.csv` files:
`buffer_native_allocate` records constructor bounds and VkBuffer; `buffer_allocation`
and explicit `replay_*_allocation` / `detile_scratch_allocation` roles are followed
by memory property/type, VkDeviceMemory+VMA offset, allocation extent and host mapping
metadata rows. `allocation_memory_binding.bytes` is the VMA **offset**;
`allocation_host_mapping.bytes` is the MemoryUsage **enum**. The preceding resource
marker identifies the buffer; metadata rows share its timestamp/transaction.
Constructor bounds allow native allocation joins; VMA may reuse a memory block and
the driver may create multiple native allocations for one block. Keep unresolved
and ambiguous cases explicit. Vulkan device-local flags do not establish current
OS residency. No memory placement or synchronization is changed by these markers.

## Offline residency associations

After packet correlation, use the same field-named Dxg export and verified QPC
origin with `analyze-replay-residency.py PROFILE.gpu.csv DXG.csv CORRELATED.json
--qpc-zero-ticks ORIGIN --output RESULT.json`. It reads existing allocation role,
binding and constructor events. Native creation intervals associate candidate
allocations with a shared Vulkan memory token; this is not an exported driver
handle map. Multiple candidates, missing creation, retired side allocations and
pointer reuse are retained. `complete` means all creation candidates remain alive,
not that exact physical subranges are known. See the [measured residency result](replay-residency-results-2026-10-06.md).

## Verification without UFC 5

```powershell
& D:\PS5\src\KytyPS5\ufc5\tools\replay-detile.ps1 -SelfTest -Build `
    -Captures D:\PS5\ufc5-replays\self-test
```

The fixtures are explicitly synthetic. They compare real GPU detiling against
independent CPU address calculations for color/depth, 4/8-byte elements, nonzero
source offsets, partial blocks, surface-Z swizzling, and cleared row padding.
Use a new self-test directory for each run because existing capture files are preserved.
