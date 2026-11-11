# Standalone replay alongside paused UFC 5 — 2026-10-06

Latest: [buffer identity/native residency analysis of the 13:28 capture](replay-residency-results-2026-10-06.md).
All 80 measured packet joins complete; seven slow scratch creation groups include
non-local backing. Whole-game FPS causality remains unestablished.

## First attempt: application timings saved, native ETL missing

Output stem: `D:/PS5/ufc5-profiles/gpuview-20261006-110119-0ec3b4ec`.
The user's first pasted Saved trace line points to the older 09:39 capture; it
is not the native trace for this replay. The new replay ran at UTC 10:01:21–25,
with emulator PID 6380 and replay PID 32860. Three warmups and ten measured
iterations per capture, same headless hash
`1CE40F4C14CD1BB3855A485F1AFB01D0EEA26504D62FEDF07B2553C7B7B3748F`.

All eight full outputs **MATCH**. All **104 iterations** uniquely join scheduler
submits; **zero event/query drops and zero split scopes**. The existing stage
timestamps locate elapsed inflation within clears and compute brackets:

| Capture guest address | Whole detile median (ms) | Clear bracket (ms) | Dispatch bracket (ms) | Post bracket (ms) |
|---|---:|---:|---:|---:|
| `0x1164920000` | 6.086960 | 0.978176 | 5.056000 | 0.002496 |
| `0x11673b0000` | 11.334784 | 0.891072 | 10.439136 | 0.007872 |
| `0x1167460000` | 6.069840 | 1.446496 | 4.639184 | 0.002944 |
| `0x11687e0000` | 0.082176 | 0.025264 | 0.053248 | 0.001936 |
| `0x1168bd0000` | 14.039568 | 3.058480 | 10.978592 | 0.003440 |
| `0x11691a0000` | 0.082128 | 0.024464 | 0.053248 | 0.001840 |
| `0x116abb0000` | 0.424544 | 0.024752 | 0.393200 | 0.003600 |
| `0x1172520000` | 0.183568 | 0.060032 | 0.117760 | 0.003088 |

The original replay's own outer query medians differ slightly from these
production-profiler detile brackets because their timestamp boundaries differ.
Category medians are not additive. Clear uses TRANSFER→TRANSFER, dispatch uses
COMPUTE_SHADER→COMPUTE_SHADER; these are elapsed brackets including relevant
stalls, not pure execution. Small post brackets do not prove barriers are free.

All 104 matched replay submits have **zero explicit semaphore wait dependencies**
and one signal dependency (the scheduler's own completion timeline). The replay
still waits on its own completion after each iteration as before. This does not
exclude driver/native scheduling, implicit dependencies, residency or other GPU
work. Native evidence is required to identify the mechanism.

## Trace-save failure and fix

Windows PowerShell raised NativeCommandError at the WPR stop invocation under
`ErrorActionPreference=Stop`. The native exit code/output were not logged, and
the final manifest remained `recording` with no end boundary. The expected ETL
does not exist. Later `wpr -status` reports **not recording**; no recording was
cancelled or restarted by the agent during recovery.

The only located instance-specific temporary ETL is WPR's 262 KiB internal
collector: marks/description, no graphics packet evidence. It is copied to
`.wpr-internal.etl` and decoded as `.wpr-internal.csv` beside the output stem.
It cannot replace the missing graphics ETL. Original manifest is preserved;
`.recovery.json` records the audit. Native event-loss/packet attribution remains
unavailable for this attempt. Available disk space is not exhausted.

The helper now runs WPR start/stop through StartProcess with separate stdout/stderr
files, preserving the real exit code without PowerShell's native stderr promotion.
Stop-invocation exceptions also persist failure state. A Windows PowerShell 5
regression passed with blank stderr and explicit exit 7; output and exit code
were retained without aborting. Script parses and read-only preflight passes.

User given the same Administrator PowerShell command for a new 30-second trace,
without emulator restart. It will either save the ETL or preserve the actual
WPR diagnostic in `.wpr-stop.stderr.txt`/`.wpr.txt` and final manifest.

## Second attempt: WPR diagnostic identified

Output stem: `D:/PS5/ufc5-profiles/gpuview-20261006-115722-6de80d99`.
Replay PID 29216, emulator PID 6380, same binaries. All eight outputs MATCH;
104 unique application joins, zero event/query drops or split scopes. All 104
submits again have zero explicit semaphore wait dependencies.

| Capture guest address | Whole detile median (ms) | Clear bracket (ms) | Dispatch bracket (ms) | Post bracket (ms) |
|---|---:|---:|---:|---:|
| `0x1164920000` | 4.326704 | 0.990176 | 3.368960 | 0.003840 |
| `0x11673b0000` | 22.051808 | 1.383216 | 20.660832 | 0.002480 |
| `0x1167460000` | 0.173200 | 0.055072 | 0.115072 | 0.001952 |
| `0x11687e0000` | 0.081424 | 0.024928 | 0.053248 | 0.001904 |
| `0x1168bd0000` | 14.596816 | 3.054112 | 11.372384 | 0.002544 |
| `0x11691a0000` | 0.081888 | 0.025328 | 0.053248 | 0.001824 |
| `0x116abb0000` | 0.419392 | 0.019952 | 0.395248 | 0.002112 |
| `0x1172520000` | 0.173392 | 0.054944 | 0.114688 | 0.002032 |

These remain elapsed stage brackets, not pure execution; medians are not additive.
Saved `.replay.analysis.json` and `.replay.stages.json` beside this second stem.

The improved runner retained the actual WPR stop diagnostic: exit -2147417850,
`0x80010106`, "Cannot change thread mode after it is set", Profile Id RunningProfile.
The manifest correctly records `stop_failed`. The graphics ETL is missing; WPR
now reports not recording, so no manual cancellation is needed. Native attribution
is still unavailable.

[Microsoft's WPR start/stop guidance](https://devblogs.microsoft.com/performance-diagnostics/wpr-start-and-stop-commands/)
documents this stop error and recommends WPT build 19650 or later. Local PATH
selected System32 WPR **10.0.19041.7548**, despite toolkit WPR **10.0.26100.7705**
already being installed. This matches the documented old-recorder failure;
the underlying triggering module was not identified.

The helper now prefers that installed toolkit recorder, rejects builds below 19650,
accepts `-WprExecutable`, prints recorder/version and persists version/hash in each
manifest. Both start and stop use the same executable. Windows PowerShell 5
read-only preflight passes with 26100 and no active recording. The actual kernel
trace save with this version remains untested until the elevated user retry.
No emulator restart, software installation or renderer/synchronization change.

## Third attempt: native trace saved and measured iterations joined

Output stem: `D:/PS5/ufc5-profiles/gpuview-20261006-121339-3dd329c3`.
Toolkit WPR 26100 saved the 739,246,080-byte ETL. Trace header reports **zero lost
events and buffers**. Emulator PID 6380, replay PID 26640, original replay build
hash `1CE40F4C14CD1BB3855A485F1AFB01D0EEA26504D62FEDF07B2553C7B7B3748F`.
All eight outputs MATCH; all 104 application joins complete, no query/event drops
or split scopes. All 104 submits have zero explicit semaphore wait dependencies.

Raw-QPC origin **6689571321712 ticks**, frequency 10 MHz. Twelve raw Dxg records
match the decoded export by event order, thread and payload identity; origin
estimates span 8 ticks (0.8 microseconds). Selected recorder window is
785669.2–30800088.0 microseconds relative to the ETL; replay wrapper window is
791988.4–4526991.9 microseconds. GPU clock remains uncalibrated to CPU/ETW.

**All 80 measured iterations uniquely match native render packets**, using replay
PID, thread and actual Vulkan submit bounds. Another 16 warmups uniquely match;
the first warmup of each capture has two native candidates and is excluded from
native attribution. There are no unmatched packet stops in the selected replay
window. Whole-trace boundary pairing diagnostics remain in the analysis JSON.

| Capture guest address | Replay GPU median (ms) | Before DMA median (ms) | DMA residence median (ms) |
|---|---:|---:|---:|
| `0x1164920000` | 0.079888 | 0.0515 | 0.2280 |
| `0x11673b0000` | 0.162032 | 0.0235 | 0.2360 |
| `0x1167460000` | 0.173616 | 0.0245 | 0.3190 |
| `0x11687e0000` | 0.081104 | 0.0490 | 0.2280 |
| `0x1168bd0000` | 0.259776 | 0.0240 | 0.3425 |
| `0x11691a0000` | 0.080256 | 0.0605 | 0.2250 |
| `0x116abb0000` | 0.418288 | 0.0365 | 0.4755 |
| `0x1172520000` | **13.979900** | **2.5360** | **14.1770** |

For the last capture, median clear bracket is **2.224848 ms**, dispatch bracket
**11.773920 ms**, post bracket **0.002080 ms**. CPU command recording median is
**0.022250 ms**, submit API bracket **0.087450 ms**. Before-DMA delay reaches
**69.789 ms** on one measured iteration; that iteration's GPU range is separately
13.9248 ms and native DMA residence 14.090 ms. The GPU timestamp inflation cannot
be explained solely by this initial before-DMA delay or CPU command recording.
Neither DMA residence nor GPU stage brackets represent pure execution; native
notification and pipeline boundaries differ. Category medians are not additive.
[Microsoft's queue documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-hardware-queue)
distinguishes queued packets from the bottom packet currently being processed.

### Native residency evidence and its current limit

Two replay-owned **32 MiB native allocations** are created during setup of the
last capture, at ETL-relative 3562255 and 3562380 microseconds. DeviceAllocation
events map their global handles to per-process allocation handles:

| Global handle | Replay allocation handle | Sysmem commit (us) | Page-in to segment 3 (us) |
|---|---|---:|---:|
| `0xffff8d8d942d4d60` | `0xffffe0821deb0750` | 3693255 | 3694096 |
| `0xffff8d8db73cbcb0` | `0xffffe0821deb0200` | 3699812 | 3700649 |

ReportSegment identifies RTX adapter `0xffffe0817f721000` segment 2 as local
(MemorySegmentGroup 0, 8,407,482,368 bytes) and segment 3 as non-local
(MemorySegmentGroup 1, 34,276,306,944 bytes). Segment 3's flags include
NonLocalBudgetGroup. PagingOpSysmemCommit plus these page-ins establish system
memory placement for these native allocations. Segment flags and system memory
semantics are documented by [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_segmentflags)
and [GPU segments](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-segments).

The same allocation lifetimes later contain migration attempts ending with
status **0xc0000001**, eviction and another sysmem commit/page-in to segment 3.
First allocation: migration stop 3764283 us, page-in 3765853 us. Second allocation:
migration stop 3890838 us, page-in 3892440 us. Pointer reuse occurs elsewhere in
this trace; these joins restrict creation/teardown lifetimes, not pointer alone.

This is a concrete residency lead, **not yet an exact input/output buffer join**.
The original replay did not record its Vulkan memory bindings or CPU allocation
bounds. Replay adds its own allocations beside the game, so its placement cannot
be assumed to represent the game's resources or prove the primary game FPS cause.
The prior lower-resolution result remains no observed FPS improvement after a
material reduction in VRAM usage. A whole-game residency conclusion needs direct
production resource evidence.

### Next measurement prepared; running game unchanged

Reused production `ProfileCpuScope`/`ProfileEvent` and `Buffer::ProfileAllocation`:

- Selected-window `buffer_native_allocate` constructor bounds identify VkBuffer.
- Allocation metadata records VkDeviceMemory, VMA offset/extent, memory type and
  property flags, optional mapped pointer and usage. Vulkan properties describe
  requested memory; they do not prove current OS physical residency.
- Explicit replay input/upload/download/parameters and detile scratch roles.
- No allocation flags, queue, barriers, dispatches, copies or waits changed.
  Inactive profiler returns before clock/query/metadata collection.

Only the headless Release target was rebuilt. New SHA256:
`8DDEA7AC763D336D1ED256C528B3493D8CD782BAA4E593E08AB2FA1BC0CDBE99`.
Validation `D:/PS5/ufc5-profiles/replay-allocation-check-20261006.csv` and
`.gpu.csv`: all eight references MATCH, 104 unique app joins, zero drops/splits,
five constructor scopes per capture and complete role/binding records. Input
and scratch properties are device-local (0x1, memory type 1), including slow
captures. In this validation captures 2/3 are slow and the last is fast; no native
trace accompanies it, so no placement claim is made. It is a coverage/output
check, not an FPS comparison or optimization result.

Next elevated capture uses the same helper command and running emulator PID 6380.
Join native allocation creation intervals to VkDeviceMemory bindings, retaining
one-to-many driver allocations, reused-block cases and ambiguous/unresolved joins.
Compare physical placement for slow versus fast iterations. No emulator restart.

## Artifacts and remaining work

- `.replay.csv`, `.replay.csv.iterations.csv`, `.replay.csv.meta.json`.
- `.replay.gpu.csv` with scheduler suffixes and CPU/event/submit records.
- `.replay.analysis.json`: 104 unique application joins.
- `.replay.stages.json`: exact stage medians above.
- `.recovery.json`, `.wpr-internal.etl`, `.wpr-internal.csv`: failure audit.

Third stem also contains `.traceheaders.txt`, `.qpc-anchor.txt`, `.clock-alignment.json`,
`.dxg.csv`, `.dxg-compact.csv`, `.events-detail.txt`, `.replay.native-manifest.json`,
`.replay.native-analysis.json` and its `.packets.json`, `.game.native-analysis.json`,
`.replay.correlated.json`, `.replay.phases.json`, `.replay.native-memory.json`,
and `.replay.last-allocations.json`. The game packet summary is residence/pending
time, not a measured active GPU utilization percentage or additive frame budget.

Next: capture the allocation identity build, join bindings to lifetime-bounded native
residency and compare slow/fast scopes. No renderer/synchronization optimization or
whole-game causal mechanism claim yet. Game PID 6380 responsive; controls off.
