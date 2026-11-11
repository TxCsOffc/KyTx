# UFC 5 OS scheduling trace — 2026-10-06

## Findings

**Follow-up completed:** [correlated application / OS report](gpuview-correlated-report-2026-10-06.md)
joins 517 readbacks to native progress. 98.46% of their matched host wait is before
DMA entry; their copy scopes total only 12.462 ms. Earlier detile/upload batches are
the next investigation target. The baseline comparison below remains unchanged.

The **Firefox-closed repeat is complete**. Readback completion waits remain the
largest measured command-thread contribution: **16.137 seconds off CPU in a
30.018-second window**, versus 12.726 seconds running. Closing Firefox did not
remove the long readback stalls or the CPU feed gaps. Native queue latency is
lower in this second window; one sequential pair does not establish how much
of that change was caused by Firefox rather than workload drift/tracing overhead.

Both recordings used the same live PID, executable SHA, WPR profile SHA and
controls (`off all`, application profiler unarmed). No graphics or synchronization
change was made. The linked follow-up now correlates application resource/tick/submit
scopes with ETW; investigate the preceding batches next.

## Firefox-closed repeat and comparison

- ETL: `D:/PS5/ufc5-profiles/gpuview-20261006-092654-5ef086b8.etl` (729,808,896 bytes).
- Local window: **09:26:55–09:27:25 BST**, selected ETL bounds
  391,748–30,409,520 us, **30.017772 seconds**.
- **Zero lost events/buffers**; no unmatched packet stop inside the selected window.
- 18,434 complete render packets matched DMA sequences; all phase intervals
  nonnegative, and mean phases reconcile to the parent lifetime.
- Initial unmatched render retirement is bounded before the selected window.
  Render starts near the selected end without captured stops remain censored
  through the boundary; they are included conservatively in outstanding-work
  coverage and excluded from latency distributions. No censored initial render/DMA
  crosses the selected start. Four Kyty preempted DMA exits; zero queue timeouts.
- Helper QPC duration 30.014195 s differs from UTC selection by ~3.6 ms because
  boundary reads are sequential. This is a separate issue from the off-CPU tail below.

| Measurement | Firefox video playing | Firefox closed |
|---|---:|---:|
| Window (s) | 30.018788 | 30.017772 |
| Command thread running (ms) | 12,287.746 | 12,725.718 |
| Readback off CPU (ms) | **16,823.725** | **16,137.286** |
| Readback off-CPU intervals | 454 | 513 |
| Longest readback interval (ms) | 154.502 | 119.386 |
| Image-free off CPU (ms) | 613.316 | 883.886 |
| No Kyty render/DMA outstanding (ms) | 4,199.048 | 4,367.548 |
| Command thread running inside those gaps (ms) | 3,857.313 | 4,037.548 |
| Mean enqueue → DMA queue entry (ms) | 12.429 | 7.337 |
| p95 enqueue → DMA queue entry (ms) | 40.590 | 30.054 |
| Mean DMA residence span (ms) | 2.909 | 2.621 |
| Mean complete render-packet lifetime (ms) | 15.343 | 9.962 |
| OS Present events | 87 | 100 |
| OS Present events/s | 2.898 | 3.331 |

The second command-thread budget closes exactly: running 12,725.718 ms, readback
16,137.286, image frees 883.886, other frees 20.671, allocations 43.453,
submission path 8.877, other GC 31.921, other stacks 162.403 and unknown boundary
3.557 = **30,017.772 ms**. Readbacks occupy **53.8%** of the window and **93.3%**
of off-CPU time. Independent xperf CPU is 12,725.690 ms (0.028 ms difference).

The 3.557 ms tail is a captured switch-out without a captured return; global
context-switch coverage extends past the selected end. The analyzer now retains
that interval as **censored, missing boundary stack**, without assigning its cause
to readback. A regression tests both valid clipping and refusal to extend beyond
trace coverage. Two wait-attribution tests pass; the three prior queue tests remain.

Kyty has no reconstructed render/DMA outstanding for **4.368 s** (14.5% of the
window). Command processing runs during **4.038 s** of this (~92.4%). Across 4,025
gaps, median 0.185 ms, p95 7.448 ms, maximum 20.663 ms. This running-in-gap time
is a subset of running CPU, not another additive budget row. Other adapter DMA
residence overlaps only 281.063 ms of Kyty-empty gaps in the repeat.

Firefox has no selected-window DMA residence or paging-start entries in the
repeat. DWM DMA residence union falls from 10,760.065 to 1,023.646 ms, remoting
from 3,025.750 to 726.371 ms. These remain queue residence, not pure execution.
The comparison demonstrates substantially less background queue activity, while
large readback stalls persist. It does not prove an emulator barrier is unnecessary.

Host ten-second presentation reports are 4.581, 3.219 and 3.171/s; they overlap
capture boundaries. Both host and OS counts are distinct from title FPS. The 15%
increase in OS Present count in this one pair is not a validated FPS improvement.
Packet populations differ (16,094 versus 18,434), and the windows are ~33 minutes
apart. No exact detile/resource/native-packet attribution is available yet.

Machine-readable checked comparison:
`D:/PS5/ufc5-profiles/gpuview-baseline-comparison-20261006.json`.

## First trace findings (Firefox video playing)

In a 30.019-second paused-fight window, the command-processing thread ran for
12.288 seconds and spent 16.824 seconds off CPU returning through readback timeline
waits. Those waits are the largest measured contribution on this thread. There are
also intervals where Kyty has no outstanding render/DMA packets while the command
thread is running, so submission starvation exists alongside completion waits.

This trace was captured **while Firefox was playing video**. The user confirmed
that and closed Firefox afterward. Other-process GPU activity is measured, but its
causal effect on Kyty is not established. The completed repeat above retains this
limitation while confirming that the large readback waits persist without Firefox.

No emulator graphics, synchronization or caching changes were made for this analysis.
FlatSRT was `off all`; application GPU profiling was unarmed in this run.

## Capture and quality

- ETL: `D:/PS5/ufc5-profiles/gpuview-20261006-085331-171703d7.etl`
- Client-local measurement window: 2026-10-06 **08:53:33–08:54:03 BST**.
- PID 7600, executable SHA256
  `7FEA0B84DDD7084ABF62410568F6C6B2C65319F6394934D334DD4751C014A539`.
- Microsoft installed `GPUView.light` WPR profile; UTC/QPC/log boundaries in adjacent JSON.
- **Zero lost events and zero lost buffers** according to xperf trace headers.
- Selected interval: ETL-relative 1,719,303–31,738,091 us, duration 30.018788 s.
- Selection uses the helper's UTC boundaries. Its separately recorded QPC duration
  is 30.013157 s; sequential boundary reads differ by about 5.6 ms. This does not
  change the seconds-scale ranking, but UTC alone is insufficient for a precise
  sub-millisecond join to future application scopes.
- Main-image base `0x140000000`, matching running PID/executable. Local matching PDB
  supplies emulator symbols; Windows/NVIDIA internals mostly remain unsymbolized.

87 DxgKrnl Present events occurred in this window, approximately 2.90/s. The host
presentation log reports 2.341, 2.541 and 3.673/s in overlapping ten-second reports.
These are host/OS presentation counts, not proof of the user's reported 6–7 title
FPS or exact displayed frames. Do not extrapolate this capture into a historical
6–7 FPS frame budget. This recording is 1.727 GB; tracing overhead is a limitation.

## Command-thread wall-time breakdown

TID **26980** accounts for 36,458 completed/censored Kyty queue-packet starts in the
selected window. Its captured stacks identify `GuestGpu::ThreadRun` and the command
processor. The table below joins context-switch intervals with the thread's saved
switch-in stacks, including clipped boundary intervals. Each row is disjoint here.

| Category | Duration (ms) | Off-CPU intervals |
|---|---:|---:|
| Running on CPU | 12,287.746 | — |
| Readback timeline wait | **16,823.725** | 454 |
| Image dedicated-memory free path | 613.316 | 2,720 |
| Other dedicated-memory free path | 33.393 | 225 |
| Memory allocation path | 58.603 | 334 |
| Submission path | 24.434 | 405 |
| Other garbage-collector path | 32.772 | 116 |
| Other saved stacks | 139.786 | 1,519 |
| Missing boundary stack | 5.013 | 1 |
| **Total** | **30,018.788** | |

Readback stack chain includes `BufferCache::DownloadBufferMemory`,
`CommandScheduler::Wait/WaitMaster` and `MasterSemaphore::Wait`. Maximum single
off-CPU readback interval: **154.502 ms**. Readbacks account for about **56.0% of the
whole window**, and 94.9% of this thread's off-CPU time. Running CPU is 40.9%.

These are **off-CPU durations**, including any ready-to-run delay after the wait.
They are not entire Vulkan API elapsed durations, copy payload time, or GPU shader
execution time. The 17,509.813 ms generic `Waiting/Executive` state alone was not
used to identify causes; saved return stacks provide the classification.

Image frees appeared in nearly half the captured switches, but their accumulated
off-CPU duration is only 613.316 ms (~2.0% of the window). Frequency would have badly
misranked them above the longer readbacks. Three queue-analysis tests and one saved
stack/duration test pass; the context-switch totals independently reconcile with
xperf thread CPU reporting (12,287.709 ms, 0.037 ms difference).

## Native render-packet latency decomposition

All **16,094** selected complete render queue packets matched their DMA packet(s)
by native context and submission sequence. All decomposed intervals are nonnegative.
Multiple/preempted DMA records for one render packet use the first entry/last exit.

| Phase | Mean (ms) | Median (ms) | p95 (ms) | Maximum (ms) |
|---|---:|---:|---:|---:|
| CPU queue enqueue → DMA hardware-queue entry | **12.429** | 1.958 | **40.590** | 154.424 |
| DMA hardware-queue residence span | 2.909 | 0.417 | 12.493 | 91.558 |
| DMA exit → CPU queue completion notification | 0.004 | 0.004 | 0.008 | 0.107 |
| Entire render queue-packet lifetime | **15.343** | 4.724 | **52.023** | 154.605 |

The means reconcile to the parent lifetime. Quantiles are separate distributions
and must not be added. Approximately 81% of **mean packet lifetime** precedes DMA
hardware-queue entry. This is not 81% of the frame budget: many packets overlap.

DMA residence itself includes hardware-queue waiting. GPUView explicitly distinguishes
queued rectangles from their executing bottom section; interpreting a full packet
rectangle as pure execution is incorrect. See Microsoft's
[hardware-queue explanation](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-hardware-queue)
and [CPU-queue explanation](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/context-cpu-queue).

The trace also contains 4,537 complete native wait packets, mean lifetime 22.525 ms,
p95 114.609 ms. Those lifetimes include position in the queue. They are not measured
time blocking at the head of the queue. Eight Kyty DMA exits were marked preempted;
no selected Kyty queue completion was marked timed out.

This demonstrates substantial **latency around queued work**, but does not yet map
the old 14–32 ms detile scopes to these exact packets. Guest resource IDs/shader
hashes are absent from this baseline ETL; GPU and CPU timestamps remain distinct.

## Submission gaps

The union of Kyty render-queue residence covers 25,819.740 ms. The union of its DMA
residence covers 23,985.453 ms. These are overlapping residence intervals, not GPU
utilization percentages or additive active-engine times.

For **4,199.048 ms** (14.0% of the window), no reconstructed Kyty render/DMA packet
was outstanding. Across 2,672 gaps, median length is 0.332 ms, p95 8.936 ms and
maximum 36.952 ms. The command thread was running during **3,857.313 ms** of those
gaps (~91.9%). This supports a real CPU feed limitation during those intervals.
It does not explain the much larger readback-wait total by itself.

Other processes still had packets in the adapter's hardware queues during
1,179.741 ms of the Kyty-empty gaps. Absence of Kyty packets is not device-wide idle.

Pairing diagnostics retain 23 DMA stop-without-start, 331 queue stop-without-start,
8 unmatched DMA starts and 66 unmatched queue starts across the full trace. None
of the unmatched stops is inside the selected window. Initial unmatched starts
use an explicit conservative retirement upper bound from a later non-preempted
render completion on the same ordered context. They retire before the selected
window for render/DMA records; their remaining unmatched starts are after it. One
third-party remoting signal has no proven retirement and remains explicitly censored;
it does not participate in render/DMA gap calculations. All censored records are
excluded from measured packet-latency distributions. This prevents a startup artifact from falsely claiming
a packet remained outstanding for the entire capture.

## Other-process activity

On the same adapter, DMA residence unions include Firefox 13,216.834 ms, DWM
10,760.065 ms and remoting_desktop 3,025.750 ms. These intervals overlap Kyty and each
other and include queue waiting. They are **not** active GPU time attributable to
those processes, and cannot be summed.

Paging-queue starts were emitted by Firefox (40,372), DWM (17,639),
remoting_desktop (5,514) and Kyty (5,031). Counts do not measure paging latency or
establish that VRAM pressure caused the game's slowdown. The earlier low-resolution
VRAM A/B remains evidence against VRAM pressure being the primary cause.

The user confirmed Firefox video was playing and closed it. See the completed
repeat above; no isolated causal contention/FPS win is established by this pair.

## CPU sample findings

Across all Kyty threads, total context-switch CPU time was approximately 29.669 s
in this 30.019 s window (about one CPU core on average). The highest resolved
exclusive sampled emulator functions include:

| Function | Sample weight (ms, all Kyty threads) |
|---|---:|
| BufferCache::SynchronizeBuffersInRange | 867.680 |
| SrtWalker::EvaluateWide | 676.524 |
| IR::Value::Resolve | 646.090 |
| RenderExecutor::CommitBindings | 549.924 |
| BufferCache::SynchronizeBuffer | 499.779 |
| KernelGetProcessTimeCounter | 429.861 |
| SrtWalker::EvaluateRawRead | 388.798 |
| TextureCache::FindImagesInRegion | 364.077 |

These are sampled exclusive weights, not enclosing call elapsed or command-thread
budgets. Approximately 9.826 s of sample weight is unresolved; it is not automatically
labelled guest execution. Windows/driver module samples are mostly unsymbolized.

## Ranked next steps

1. **Readback completion waits:** largest measured command-thread contribution.
   Reuse the existing bounded application profiler alongside ETW to join exact
   readback resource/submit/tick scopes to native queue progress. Investigate why
   each required completion is late: preceding batch work, queued dependency,
   residency or another process. No stale-byte caching or synchronization removal.
2. **CPU command preparation/feed:** substantial running time and confirmed gaps.
   Existing buffer-range scans and materialization remain CPU candidates. The
   broad FlatSRT experiment reduces its local cost but has no demonstrated FPS win.
3. **Image/memory churn:** real but much smaller off-CPU contribution in this trace.
   Keep allocation/free counts for later investigation; do not promote switch
   frequency to a dominant latency claim.

The Firefox-closed baseline and the following application/ETW capture are complete.
The command below documents that capture; see the correlated report for results.
For a future capture, run in
the same Administrator PowerShell; Windows kernel tracing requires elevation and
the agent's shell is not elevated. No emulator restart:

```powershell
& 'D:\PS5\src\KytyPS5\ufc5\tools\capture-gpuview.ps1' -Seconds 30 -ArmProduction -ApplicationProfilePrefix 'D:\PS5\ufc5-profiles\production-20261006-083333.csv'
```

The helper verifies launch PID/hash/control, records the request and CSV offsets, and adds
application profiling overhead. Clearing the request file does not cancel an active
window; its configured 64 producer epochs stop automatically. This mode is prepared
and completed in the follow-up. Keep that capture
separate from the Firefox A/B. The current launch has 64 producer epochs configured,
with FineCPU/Lifetime disabled. Analyze actual overlapping captured rows; do not
assume the full 64 epochs fit inside 30 seconds or that this configuration resolves
every resource question. Reuse available parent resource/submit/tick scopes first.

## Reproducibility

Analysis scripts: `tools/analyze-gpuview.py`, `tools/analyze-gpuview-waits.py`.
Export using installed xperf (`-o` precedes `-a`): `tracestats`, field-named
DxgKrnl dumper, Thread dumper, and Thread/StackWalk dumper with `-stacktimeshifting`.
Saved switch-in stack symbols use the xperf butterfly report for PID 7600/TID 26980
and matching image base/PDB. All outputs share the ETL stem:

- `.traceheaders.txt`, `.tracestats.txt`: loss/coverage and event inventory.
- `.analysis.json`, `.analysis.json.packets.json`: residence, joins, CPU intervals.
- `.wait-analysis.json`: full-window stack-based off-CPU classification and closure.
- `.cpu-window-threads.csv`, `.cpu-symbols.csv`: independent thread totals/self samples.
- `.submit-thread-wait-stacks.txt`: xperf HTML butterfly report (despite extension).
- `.dxg-compact.csv`, `.threads-raw.csv`, `.wait-stacks-full.csv`: reproducible exports.

Raw exports are large; the ETL and manifest are the primary capture. All measurement
changes remain Python/PowerShell tooling. No emulator rebuild, new default, graphics
change or commit was made in this session.
