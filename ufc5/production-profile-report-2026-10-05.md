# UFC 5 production graphics measurements — 2026-10-05

## What the measurements establish

There are **two substantial recurring costs** in this fight: command-processor host
work and synchronous buffer readbacks. There is also a separate, intermittent episode
of inflated detile/copy GPU elapsed time. Faster isolated detiles alone will not remove
the recurring host cost. The measurements do not yet identify the hardware cause of
the intermittent GPU episode or prove which barriers can safely be narrowed.

No renderer optimization, memory-placement policy, or synchronization change was
made in this session. Profiling is opt-in, bounded, and can be armed again in the
running game. The main deployed executable was preserved; the profiler uses a
separately staged executable.

## Capture and coverage

The user drove the restarted profiling build into the fight and reported “in.” The
requested scene was the paused fight. Native guest output resolution was restored;
the earlier broken 1080p override was disabled. No validation layer was enabled for
the performance capture.

| Item | Lighter production capture |
|---|---|
| Trace | `D:\PS5\ufc5-profiles\production-20261005-193728.csv` |
| Manifest / executable identity | Same base + `.manifest.json`; SHA256 `D79696746746B5D64739FF44617275A41EF71FBBA3B71023928A7279B1E09B57` |
| Process | PID 30960, `kyty_emulator.production-profile.exe` |
| Window | Guest epochs 895–958, 64 epochs |
| Wall duration / presentation calls | 27.868 seconds / 127 calls, about 4.56 calls per second |
| GPU ranges / queue submissions | 175,890 / 21,851 across producer and presenter |
| CPU detail | Every 16 epochs; 3 complete detailed epochs in the selected sample |
| Coverage | 32 GPU scopes dropped in epochs 927, 928, 930; zero event drops, split scopes or missing submitted-batch scopes |
| Aggregate sample | 59 complete interior epochs; boundaries and the three dropped epochs excluded |
| Event trace size | About 199 MB, versus 1.54 GB for the earlier dense capture |

**Epochs are not displayed frames.** Kyty's counter advances when a guest SuspendPoint
is enqueued. There are approximately two presentation calls per epoch here. All tables
below use milliseconds per epoch unless explicitly stated otherwise. Presentation-call
rate is not proof that every presented image contains a new simulated game frame.

The capture-off rate immediately before arming reached 5.526 FPS. Other capture-off
rows vary considerably. This capture is slower than the user's earlier approximately
6–7 FPS runs; it must not be represented as an exact budget for those runs.

## Main recurring costs

| Measurement | Median ms / epoch |
|---|---:|
| Producer epoch wall interval | 390.937 |
| Guest command-processing elapsed, excluding recorded waits/submits/profiler work | **248.667** |
| Foreground explicit waits, overlapping intervals counted once | **91.184** |
| Queue submission CPU scopes | 8.088 |
| Profiler serialization / query collection | 25.894 / 8.975 |
| Observed interval with no outstanding captured queue batch, lower bound | 23.997 |
| GPU submitted-batch elapsed envelopes, union | 142.392 |
| Graphics render scopes, union | 37.370 |
| Guest compute dispatch scopes, union | 64.512 |
| Detile dispatch scopes, union | **12.544** |
| Copy/clear scopes, union | 14.353 |
| Batch envelope not covered by classified work scopes | 8.769 |

These are elapsed scopes, not CPU cycles or shader-active counters. Host scopes can
include thread descheduling. CPU and GPU clocks are not calibrated against one another;
their budgets cannot be added. GPU categories can overlap across pipeline stages, and
independent medians are not an additive frame budget. The unclassified GPU interval is
**not** a measured barrier-cost total.

### 1. Command processing and draw preparation

Command processing has the largest recurring foreground attribution. In the three
complete sampled epochs (896, 912, 944), draw recording excluding explicit waits has a
192.552 ms median; translation/other host work has a 75.815 ms median. Those medians
come from a smaller sample than the 248.667 ms whole-window result.

| Draw phase, recorded elapsed minus explicit waits | Median over those 3 epochs (ms) |
|---|---:|
| State preparation: `PrepareDrawRenderState` | 79.483 |
| Resource/binding preparation | 72.298 |
| Draw commitment | 31.027 |
| Target preparation | 5.454 |
| Vertex/index preparation | 2.077 |
| Pipeline preparation | 1.760 |

State preparation and bindings are the first CPU areas to investigate with sampled
call stacks. These timings do not establish that arbitrary draw work can safely move
to another thread. Guest ordering, mutable render state and cache ownership must be
understood before proposing parallel recording.

### 2. Synchronous readbacks repeatedly stop the foreground processor

`BufferCache::DownloadBufferMemory<false>` records a copy, submits/waits for its batch,
waits for priority operations, then publishes bytes into guest memory. The actual
timeline wait at `bufferCache.cpp:257` occurred **746 times**, totaling **8,422.401 ms**
over this window. The longest single call was 124.607 ms. The wait is for a submitted
batch; it is not the isolated transfer time of the returned bytes.

| Guest readback address | Recurring packed payload | Actual timeline waits | Total foreground wait (ms) |
|---|---:|---:|---:|
| `0x1164b80000` | 524,288 bytes | 128 | 3,602.109 |
| `0x1140008000` | 128 or 448 bytes | 255 | 2,750.567 |
| `0x1167f00000` | 131,072 bytes | 128 | 1,983.474 |
| `0x1165e00000` | 524,288 bytes | 127 | 35.046 |

The first three addresses account for about **99%** of this call site's wait time.
Even the tiny 128/448-byte readback waits for much more than its byte-transfer cost.
This is direct evidence of repeated submit/wait cycles on the foreground path.
The next investigation should identify the producing work and the guest's required
visibility point for these addresses. It would be unsafe to simply remove their waits.

Other foreground timeline waits: `renderContext.cpp:104`, 248 calls / 213.631 ms;
`bufferCache.cpp:756`, 12 calls / 29.676 ms. Background completion-worker waits total
14,738.314 ms on thread 37292; **do not add them** to foreground latency. There were
no observed hot-path queue-idle calls, native fence waits, ownership transfers, or
noncoherent flush/invalidate operations in this window.

### 3. Submission starvation is present, but its full cause is not isolated

Median submissions per epoch: 339 command buffers, classified by guest commands as
95 graphics-only, 50 compute-only, 18 mixed and 175 other. These classifications are
medians and need not sum exactly. Detile/transfer-only work can be in “other.” All use
**one physical Vulkan queue**, not independently running graphics/compute queues.
Median explicit submit wait dependencies: 2 per epoch, including presentation.

Mean spacing between submits within an epoch has a 1.121 ms median; maximum spacing
has a 27.511 ms median. The stronger completion-observation check establishes at
least 23.997 ms per epoch with no outstanding *captured* command buffer. It does not
prove whole-device idleness, and some gaps include profiler serialization. Across
the window, uncovered GPU submit-envelope gaps total 15,149.598 ms; these are a
coverage signal, not a hardware idle counter.

The measured host work, foreground waits and submit cadence support a GPU-feeding
problem as a substantial part of this run. A native scheduling trace is needed to
separate host descheduling, true GPU idleness and driver/preemption intervals.

### 4. Barriers are very broad and frequent; their causal cost remains unknown

Median barrier resource records: 6,313 per epoch, of which 5,542 have ALL_COMMANDS
on at least one side. `CommandProcessor::EmitGlobalBarrier` emitted **171,156**
ALL_COMMANDS→ALL_COMMANDS records across 64 epochs. At this particular call site
there is one global memory entry per call, so this count also counts its calls.

`shaderResourceBarrier.cpp` emitted 50,414 ALL_COMMANDS→COMPUTE_SHADER entries and
50,414 COMPUTE_SHADER→ALL_COMMANDS entries. Masks, accesses, layouts, queue families
and resources are retained in `barrier-sites.csv` and the raw events. These counts
identify candidates for a dependency audit; no measured millisecond cost can yet be
assigned to them individually.

## The intermittent detile inflation is real in production

The lighter capture contains a slow episode in epochs 925–934. The three epochs
with dropped queries are excluded from these medians.

| Metric (ms / epoch) | Other complete epochs (52) | Slow episode, complete epochs (7) |
|---|---:|---:|
| Wall interval | 387.015 | 652.572 |
| Guest host work excluding waits/submits/profiler | 249.536 | 244.742 |
| Foreground explicit waits | 87.790 | **344.883** |
| Detile compute-stage elapsed scopes | 11.730 | **309.694** |
| Copy/clear elapsed scopes | 13.812 | **71.908** |
| Render elapsed scopes | 37.001 | 38.741 |
| Guest compute elapsed scopes | 62.877 | 69.032 |
| Profiler serialization | 25.898 | 24.316 |

The increased latency localizes to detile/copy elapsed scopes and associated host
waits; draw/guest-compute and host-processing costs do not grow similarly.

For address `0x116abb0000`, median COMPUTE_SHADER-bracketed detile elapsed time is
0.750 ms in epochs 896–924, **29.857 ms** in epochs 925–934, and 0.926 ms in epochs
935–957. The older isolated capture of this address replays at 0.477 ms. Address
matching does not prove identical bytes/push arguments in every production operation.

The inflated duration remains **inside the compute-stage bracket**. It therefore
cannot be explained solely by an older TOP_OF_PIPE→BOTTOM_OF_PIPE range including
the pre-detile barrier. It still is not proof of 30 ms of active shader execution:
timestamp elapsed time can include stalls/preemption. The first dense capture also
observed production dispatch inflation, but its roughly 131 ms/epoch serialization
cost made it unsuitable for the normal performance budget.

### Memory placement is a measured difference to investigate

Of the 3,137 detile transactions, the examined `0x116abb0000`, `0x1172520000` and
`0x11673b0000` sources consistently report memory type 0 / property flags `0x0`.
The same flags occur before, during and after the slow episode. Overall source
allocation metadata includes 2,989 upload observations with `0x0`, 98 with `0x6`
and 50 with `0x7`; readbacks include 611 with `0x0`, 256 with `0x1` and 25 with `0x7`.
These are observations, not counts of unique allocations.

`0x0` lacks DEVICE_LOCAL. Vulkan specifies that DEVICE_LOCAL types belong to a
device-local heap. This establishes allocation type, not instantaneous residency,
bandwidth or its FPS contribution. [Vulkan memory property definitions](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryPropertyFlagBits.html).

The isolated replay requests `MemoryUsage::DeviceLocal` (a VMA preference), but its
actual memory type was **not logged in that replay**. A confirmed replay/production
placement comparison remains necessary. Reducing image VRAM usage in the earlier
resolution test did not by itself measure these source-buffer allocations or
eliminate a possible memory-placement effect. It remains evidence against shared
VRAM pressure *alone* explaining the recurring low FPS.

## Example complete epoch 944

Two presentation calls; wall interval 407.877 ms. Foreground command-processing elapsed
excluding waits/submits/profiler: 251.936 ms; explicit waits: 112.613 ms; submission
scopes: 8.541 ms; measured profiler serialization/collection: 24.687 / 8.363 ms.

GPU batch envelope union: 148.953 ms; graphics: 43.147 ms; guest compute: 72.276 ms;
detile dispatch: 11.783 ms; copies/clears: 13.203 ms. There were 337 submissions,
11 foreground timeline waits, zero queue-idle calls, 5,077 broad barrier entries and
34 detile calls. No dropped, split or missing batch measurements in this epoch.
CPU/GPU categories overlap and must not be added together.

## Measurement limitations and verification

- Measured profiler work is about 35 ms/epoch in the lighter capture. GPU timestamps,
  hook bookkeeping, allocation metadata and mutex overhead add unmeasured cost too.
  Subtracting the measured cost does not reconstruct exact uninstrumented FPS.
- CPU phase detail is sampled. Its three complete epochs provide direction, not a
  robust estimate of every subfunction or a proof that shader/pipeline compilation
  never spikes.
- Image first-consumer tracking is best effort; 2,881 of 3,137 detile transactions have
  a marker. A render-target reference may write rather than read. Missing markers
  are missing evidence, not zero latency.
- C0–C5 host endpoints are exported, including scheduler/batch joins and host wait
  thread IDs. Upload functions commonly return before submission; background waits
  must not be confused with an in-function foreground wait.
- The earlier 31.944 / 14.427 ms contested measurements came from a **separate replay
  process alongside the game**, not production scopes. The new production captures
  independently establish the compute-stage inflation described above.
- Emulator and headless harness build succeeded. All eight stored fight replays remain
  byte-for-byte MATCH; final isolated medians span 0.082–0.477 ms.
- A 1,024-iteration query lifetime/exhaustion test passed with synchronization validation,
  MATCH output and no validation warnings/errors. Intentional drops/split are visible.
- Four analyzer regression tests passed: overlapping intervals, thread separation,
  timestamp wrap, multiple schedulers/sidecars, missing sampled CPU detail and readback
  resource/metadata/wait joins.

## Next measurements, in priority order

1. **Sample the command-processor CPU stacks** in the current paused fight, concentrating
   on state/binding preparation and the three costly readback addresses. Attribute
   cache/resource work inside those phases before choosing a threading strategy.
2. **Capture native GPU scheduling during a detile spike**, alongside CPU sampling, to
   distinguish engine execution, preemption/residency and dependency gaps. Windows
   `wpr.exe` is installed and lists CPU and GPU profiles, but session 25's GPU capture
   attempt was blocked by Windows profiling policy (`0xc5585011`). Listing profiles
   does not establish that recording is permitted. Resolve that recorded limitation
   or use a working GPU execution tracer. This does not require another game boot.
3. **Log actual replay allocation properties and compare them with production**, then
   reproduce the full upload/readback transaction in the existing headless harness.
   Keep identical inputs and synchronization until a cause is established.
4. Audit the observed ALL_COMMANDS barriers against guest producer/consumer requirements.
   Only then design a bounded A/B optimization with correct output and matched captures.

The available data explains why isolated shader speed and reduced image VRAM pressure
do not deliver a large FPS gain: recurring host command processing and synchronous
readbacks remain. The cause of intermittent 30 ms detile-stage elapsed latency is
still unresolved and should be measured with native scheduling evidence.

## Files

- [Profiler usage and semantics](production-profile.md)
- [Ledger](ledger.md)
- [Generated full breakdown](D:/PS5/ufc5-profiles/production-20261005-193728.csv.analysis/report.md)
- [Per-epoch data](D:/PS5/ufc5-profiles/production-20261005-193728.csv.analysis/per-frame.csv)
- [Detile transactions](D:/PS5/ufc5-profiles/production-20261005-193728.csv.analysis/transactions.csv)
- [Readback transactions](D:/PS5/ufc5-profiles/production-20261005-193728.csv.analysis/readbacks.csv)
- [Wait sites](D:/PS5/ufc5-profiles/production-20261005-193728.csv.analysis/wait-sites.csv)
- [Barrier sites](D:/PS5/ufc5-profiles/production-20261005-193728.csv.analysis/barrier-sites.csv)
- First dense trace: `D:\PS5\ufc5-profiles\production-20261005-190801.csv`
