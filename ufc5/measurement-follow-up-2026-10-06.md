# UFC 5: materializer expressions and CPU attribution

## Result

The paused-fight capture again identifies **CPU command preparation and synchronous
buffer readbacks** as substantial latency contributors. The separate light observer
shows that materialization has measurable cost, primarily in FlatSRT refresh. It
does not support expecting a large FPS gain from direct descriptor getters alone.
The first getter experiment had zero eligible whole descriptors; these measurements
explain why: 99.65% of sampled descriptor words have a ReadConst root with SRT
dependencies.

No optimization was enabled. The user confirmed the paused fight looks the same.
The emulator remains running, with both measurement windows finished and controls off.

## Captures and coverage

Prefix: `D:/PS5/ufc5-profiles/production-20261006-064620.csv`.
The adjacent manifest records the executable hash and configuration. PID 1344,
foreground thread 35696. Descriptor gather and targeted lifetime diagnostics were off.

1. Light materializer window: 20.000007 seconds, completion marker present. The
   heavier production profile remained disarmed.
2. Separate production window in the same paused scene: epochs 60801–60864,
   35.800 seconds, 127 present API calls. Analysis uses 62 interior epochs;
   finer CPU detail is available in seven. Zero timer/event drops, split scopes
   or uncollected batches.

Epochs are command-processor enqueue boundaries, **not displayed frames**. This
heavy capture observed 3.55 present calls/second; it is not a new uninstrumented FPS
baseline or an optimization A/B. GPU queries, CPU probes and synchronous output
perturb the run. User-reported normal performance remains about 6–7 FPS.

## Light materializer observations

| Observation | Recorded result |
|---|---:|
| Shader hashes | 375 |
| Materializations, all calls | 242,016 |
| Detailed sampled calls | 7,637 |
| Materializer elapsed, all calls | 2,083.408 ms / 20 s |
| Matching sampled parent elapsed | 84.073 ms |
| Phase closure failures | 0 |

All-call elapsed is approximately 10.4% of the window wall time. It includes
sample observer cost and any host preemption inside the bracket, and is not a
thread busy-time measurement. This is useful evidence of scale, not a guaranteed
recoverable budget. It must not be combined with measurements from the separate
heavy window as though they came from the same frames.

| Exclusive phase | Raw sampled elapsed (ms) |
|---|---:|
| FlatSRT refresh | 52.900 |
| Buffers | 14.724 |
| Images | 11.033 |
| Samplers | 2.093 |
| Specialization | 0.808 |
| Uniform fill | 0.247 |
| Unbracketed setup/remainder | 2.268 |

FlatSRT is about 62.9% of the matching sampled parent time. These are raw sample
totals, without multiplication into a per-frame budget. Descriptor word timings
are nested in these phases and cannot be added to them.

The top two hashes by all-call elapsed are `d3dcf81c43080fd0` (135.624 ms,
12,470 calls, mean 10.876 us) and `df3633d8030ed2a3` (131.872 ms,
39,362 calls, mean 3.350 us). No single shader accounts for most materializer cost.

Root words: ReadConst 274,652; GetUserData 956; CompositeExtractU32x2 2;
immediate 2. Graphs describe 17,020 descriptor words, with 90 structurally pure
roots and no truncated walks. A common dependency chain contains GetUserData,
LoadAddressU32, GetAddressResource and GetSrtResource beneath ReadConst.
Graph counts are structural, not execution counts or proof of safe reuse.

Observed interpreter visits include 937,312 GetUserData (875,828 memo hits),
787,984 LoadAddressU32 (319,815 hits), and 283,917 ReadConst (3,494 hits).
These counts do not isolate guest memory-read latency. Root elapsed includes
dependencies/memo handling; sub-microsecond timings are sensitive to timer cost.

## Production latency contributors

Medians over 62 interior epochs, in milliseconds:

| Category | Median |
|---|---:|
| Epoch wall elapsed | 476.019 |
| Foreground command CPU, excluding recorded waits/submits/profiler work | 257.835 |
| Foreground explicit waits | 117.457 |
| Queue submit API elapsed | 9.229 |
| Profiler serialization | 27.994 |
| Profiler query collection | 9.184 |
| Observed recorder emission intervals | 0.380 |
| Known-complete submission gap lower bound | 23.668 |
| GPU batch elapsed union | 161.947 |
| Detile dispatch-stage elapsed union | 36.118 |

These categories overlap/use different clocks, and medians are not an additive
frame budget. The CPU attribution still includes unobserved hook costs and host
preemption. Recorder construction/lock/vector append intervals are now excluded;
scope construction, lock release and trailing timer costs are not fully observed.

### Readbacks

Ordinary transaction records locate 14,102.944 ms of host waits in 636 synchronous
readback transactions across the full window:

| Buffer base | Calls | Total wait ms | Mean wait ms | Prior classification |
|---|---:|---:|---:|---|
| `1164b80000` | 128 | 6,847.655 | 53.497 | CMask |
| `1167f00000` | 127 | 3,688.435 | 29.043 | CMask |
| `1140008000` | 254 | 3,493.415 | 13.754 | Indirect draw arguments |
| `1165e00000` | 127 | 73.439 | 0.578 | CMask |

The first three bases account for 99.48% of these waits. Detailed producer/content
lifetime tracing was off; classifications come from the previous capture, not a
new content-equality proof. Waiting for a batch does not measure the cost of its
copy payload alone. Timings are strongly skewed; full-window means and interior
epoch medians describe different aspects of the distribution.

### BDA and CPU sampling limits

The direct PrepareBda bracket recorded 77 sampled calls, 13.037 ms inclusive net
elapsed and 8.870 ms self elapsed. Inside those calls are 34,590 SynchronizeBuffer
visits: median 435 per call, range 391–751. Inspection confirms PrepareBda walks
mapped ranges and SynchronizeBuffersInRange visits overlapping cached buffers.

However, scaled binding self totals are 1.564 times the enclosing measured binding
phase. **Binding/BDA per-epoch estimates remain withheld.** State closure is 1.176,
within the analyzer's 1.25 guard, but that guard is not proof of an exact budget.
The light materializer census is the stronger evidence for materializer scale.

### Submission cadence and barriers

Median 333 submits/command buffers per epoch: 94 graphics-only, 48 compute-only,
18 mixed, 173 other. All share one observed native queue. The median epoch-average
CPU submission interval is 1.411 ms; median epoch maximum is 37.330 ms. The
23.668 ms known-complete gap proves no captured command buffer remained outstanding
during those intervals. It does not establish device-wide idleness or distinguish
translator delays from profiler output/other host work.

Median 5,745 barrier resource records per epoch, 5,005 broad; these are resource
entries, not barrier API calls. The full window includes 168,539 global
ALL_COMMANDS-to-ALL_COMMANDS records at graphicsRun.cpp:1574. No queue idle call
was observed. Conservatism is identified, but its causal latency is unmeasured.
Stage-scoped detile elapsed remains distinct from pure shader execution.

## Ranked findings and next steps

1. **CPU command preparation:** largest median foreground attribution. Within
   materialization, target FlatSRT refresh before widening the zero-coverage getter
   prototype. Audit repeated dependency evaluation, clean/ordinary evaluator memos
   and control-flow traversal. Any candidate should compile immutable expression
   structure while preserving current values, read order, failures, activity and
   specialization; validate full outputs in shadow mode before live A/B.
2. **Synchronous readbacks:** large measured waits concentrated at three bases.
   Previous-byte caching/deferred consumption is still unsupported by the producer
   and consumer evidence. A native GPU execution trace around these waits is needed
   to distinguish earlier batch work, dependency stalls and preemption.
3. **BDA scans:** repeated hundreds-of-buffer traversal is now directly observed.
   Before enabling a clean-range guard, collect bounded aggregate BDA time/counts
   without per-child event emission, and measure clean/dirty visits and actual upload
   ranges. Audit tracker creation, page protection and concurrent faults. A guard
   that duplicates tracker work may lose.
4. **Broad barriers/submission gaps:** measured patterns, not demonstrated savings.
   Do not remove barriers or move work to another thread based on these totals alone.

No single massive win has been proven. This narrows the CPU experiment and confirms
readback latency remains important; it does not fully explain every GPU scope stall.

## Validation and artifacts

Emulator/harness build; materialization and resource-tracking tests pass. Profiled
and unprofiled complete snapshot/specialization outputs match. All eight saved
detiles MATCH (0.080–0.477 ms isolated). Nine analyzer tests pass. User visual
confirmation is not a frame-hash comparison. No optimization or commit made.

Analysis directory: `D:/PS5/ufc5-profiles/production-20261006-064620.csv.analysis/`.
Inspect report.md, focused-report.md, per-frame.csv, readbacks.csv,
cpu-sampling-closure.csv and the materializer tables. The separate light observer
can be rearmed without restarting this emulator using the documented controls.
