# Production budget with compiled SRT and dirty-only BDA enabled

2026-10-06. Same running paused-fight process26444; no rebuild/restart or
graphics/synchronization change. Both CPU candidates stayed on. User previously
reported seeing 9 FPS briefly; this is an observation, not a sustained-rate claim.

## Main finding

The command thread spent **20,071 ms waiting for synchronous buffer readbacks**
in a **33,515 ms** production capture: approximately **59.9% of its wall window**.
The two previously identified CMask metadata regions account for **73.94%** of
that wait; indirect draw arguments account for **25.94%**.

The next synchronization investigation should target these readback dependencies.
This capture measures blocking time; it does not establish that the waits are
redundant or predict how much frame time an optimization could recover.

## Coverage: CPU and GPU require different cohorts

Captured producer epochs7147–7210, 64 total /62 interiors, detail every8epochs.
Zero CPU/event drops, zero split scopes, zero uncollected queried batches.
4932 draws/724 computes per median epoch;127 present API calls. Epochs are guest
SuspendPoint hints, not displayed frames. Instrumented throughput is not normal
gameplay FPS. No graphics confirmation answer has been received.

There were **715 GPU timer drops**, and only **18/62** interior epochs had complete
GPU coverage (7192–7209). Those are the later, faster portion of the capture.
The existing general analyzer selects complete epochs for all its medians, so
its 101.444ms foreground-wait median does **not** describe the full CPU sample.

The added offline `summarize-production-budget.py` reuses its event database,
interval operations and exports, separating all CPU-valid epochs from the
GPU-complete subset. It excludes incomplete GPU metrics from the CPU cohort.
No new instrumentation or rendering behavior was introduced.

Query capacity diagnosis:715 producer submits have no timer page, exactly matching
the dropped count; maximum recorded timers per queried batch84 (<256 limit),
zero batches at256. Existing recorder has64 reusable pages. Together with
`BeginGpuTimerPage`, this identifies page availability exhaustion rather than
the per-batch query limit. No extra host wait was added to force query collection.
The recorder needs more in-flight page capacity before a complete GPU comparison
of similarly long-backlogged runs. This capture cannot provide a representative
full-window GPU timing rank.

## CPU-valid frame budget

Medians over all62 interior epochs, milliseconds/epoch:

| Metric | Median |
| --- | ---: |
| Wall interval | 595.676 |
| Foreground explicit waits | **399.113** |
| Guest work excluding recorded waits, submits and profiler | **152.758** |
| Submission CPU | 7.234 |
| Profiler serialization | 24.828 |
| Query collection | 7.109 |
| Known-complete captured-batch gap lower bound | 16.171 |
| Queue submits | 333 |
| Maximum submission gap | 99.903 |

Eight detailed interior epochs: recording excluding waits/submits/profiler112.663,
translation/other43.265ms median. Draw phase unions excluding recorded waits:
state42.824, bindings32.957, commit29.084, targets5.053, vertex/index2.038,
pipeline1.653ms. Phases include observer/preemption and other unclassified costs.

The previous compiled-SRT-only capture had209.635ms guest work and69.617ms binding
phase medians, but also different draw/compute counts (5432/750) and GPU/wait
conditions. Those old values provide context, not a controlled BDA delta. The
five lightweight off/on stages already establish the BDA CPU-path saving.

CPU/GPU clocks are uncalibrated. Independent medians and overlapping categories
are not additive. A known-complete gap is a bound for captured batches, not proof
of whole-device idleness. Background timeline waits overlap foreground work and
are not added to the command-thread budget.

## Readback breakdown

All captured foreground transactions, including boundary epochs:

| Guest address / previously identified role | Calls | Host wait ms | Wait share |
| --- | ---: | ---: | ---: |
| 0x1164b80000, CMask metadata | 128 | **9483.064** | 47.25% |
| 0x1167f00000, CMask metadata | 127 | **5357.925** | 26.69% |
| 0x1140008000, indirect arguments | 254 | **5206.377** | 25.94% |
| 0x1165e00000, comparison readback | 127 | 24.111 | 0.12% |

Wait site remains `BufferCache::DownloadBufferMemory`, bufferCache.cpp:263,
waiting on the current scheduler tick before CPU publication. Source uses the
ordinary graphics scheduler copy and full source-buffer visibility barrier.
There is no local per-range last-writer transfer-readback path yet.

Prior correlated native evidence found98.46% of matched wait before the readback
packet's DMA entry, with small copy residence. This new capture has no simultaneous
ETW data; that old percentage is not a measurement of this run. It supports
investigating queue/dependency placement, not deleting waits or reusing stale data.

## Limited GPU observations

Only the18 complete late interior epochs: GPU batch elapsed union151.147ms,
graphics37.417, guest compute61.244, detile dispatch30.540, copies/clears12.299
medianms/epoch. These ranges include stalls/preemption; they are not active shader
time. The complete subset cannot characterize the earlier44 slow interior epochs.
No causal conclusion about the remaining detile inflation follows from this run.

## Next concrete work

1. Observe last-writer identity, submitted timeline tick, pending writes, BDA
   writes and buffer lifetime for the two metadata regions and argument reads.
   Reuse existing lifetime hooks; this launch did not enable fineCPU/lifetime.
2. Determine eligibility for the GTA `33b16934` last-writer transfer-readback
   subset. Keep the current readback when a writer is unsubmitted, ownership is
   ambiguous or device-address writes prevent proof. Preserve CPU publication,
   aliases, deferred callbacks and source-buffer lifetime.
3. Improve the existing query-page capacity in the next profiling build before
   relying on a full GPU budget. Do not add synchronization to eliminate drops.
4. Only after eligibility is measured, implement a separately controlled path
   and compare the same paused fight. Native indirect draws remain a separate
   candidate for the argument-readback quarter of the observed wait.

Both CPU candidates remain on; production capture ended automatically and its
request is off. No optimization, commit or additional restart in this session.

## Artifacts

Prefix `D:\PS5\ufc5-profiles\production-20261006-155515`:

- `.both-on.csv` and suffixes: frozen64-epoch producer/presenter traces.
- `.both-on.csv.selection.json`: frozen selection and file counts.
- `.both-on.csv.analysis/`: existing report, per-frame, readbacks, wait/barrier
  sites, transactions, scopes and event database.
- `.both-on.csv.budget.json`: separate CPU/GPU cohorts and readback/phase summary.
- `.both-on-controls.json` and frozen candidate CSVs: bounded actual mode guard.

Guard rows whose report endpoints lie inside the capture: BDA19222calls /
19185candidate /37checks, compiled527887/526862/1025. Both on, zero mismatches,
BDA zero raced checks, neither rejected. Edge reporting rows span slightly beyond
the capture; these guard counts are not per-epoch budgets. No heavy analysis ran
during the selected window.
