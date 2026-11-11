# UFC 5 dirty-only BDA results

## Measured result

BDA preparation elapsed CPU time decreased **79.1% per call** across repeated
off/on stages: weighted **104.43 → 21.80 µs**. Buffer synchronization visits per
call fell **870.26 → 1.94**, including periodic reference checks (**99.78% fewer**).
This is a repeatable CPU-path saving. A large overall FPS improvement is not
established by this run.

## Configuration and window

2026-10-06, RTX 3070 / i7-10700, running PID26444. User confirmed paused fight.
Same process and native resolution throughout; sparse BDA enabled; compiled SRT
on throughout; detailed production profiler disarmed. Only BDA mode changed.
Five 40-second stages, in order, without a restart. User graphics comparison
answer is still pending; no visual confirmation is claimed.

Executable SHA256:
`AFCAC22B9A3AE616C5CD4DF150358800CE761929B2C36536AF8BF7B4BC3842E8`.

## Bounded measurements

Only fully contained reporting windows >=1.5 seconds contribute to these timings.
Partial edge windows are excluded. Every stage passed count, elapsed-time and
comparison-count closure checks.

| Stage | BDA µs/call | Calls | Candidate execution | Checks | Raced checks | Host presents/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| off1 | 107.91 | 22,484 | 0% | 0 | 0 | 3.69 |
| shadow | 106.12 | 32,186 | 0% | 503 | 3 | 5.09 |
| on1 | 22.07 | 22,022 | 99.80% | 43 | 0 | 3.82 |
| off2 | 100.37 | 19,250 | 0% | 0 | 0 | 3.30 |
| on2 | 21.61 | 30,700 | 99.81% | 59 | 0 | 5.06 |

On-stage BDA totals are 486.09 and 663.28 ms, versus off totals 2,426.18 and
1,932.19 ms. Stage throughput differs, so per-call comparison is the useful
CPU-path metric. These elapsed scopes include recording/uploads and possible
thread preemption; they are not an exclusive whole-frame CPU budget.

97.62% of on calls took the unchanged-epoch shortcut. Actual upload volume per
call was similar, **37.38 KiB off / 37.16 KiB on**. The candidate removes repeated
clean-buffer visits while continuing the existing upload/barrier path for dirty
buffers. Similar volume alone does not prove equal output.

## Correctness evidence

Across the entire frozen selection, including stage edges and gaps:

- **684 BDA comparisons: 681 matched, 3 concurrent-write checks inconclusive,
  0 stable mismatches.** All three raced checks were in shadow mode, which
  executed the original uploads. On-stage checks had no raced misses.
- **56,923 actual candidate calls**; stable on-stage execution coverage 99.8%.
- No recorded rejection or emulator exit. PID26444 remained responsive.
- Compiled SRT stayed on: 4,210,814 calls, 4,202,587 candidate calls,
  8,227 comparisons, 0 mismatches/rejections in the bounded guard selection.
- Pre-game tests passed actual Vulkan upload bytes, clean skipping, dirty-run
  batching, registration/remapping, GPU-owned bytes and reference recovery after
  an injected selection error. MemoryTracker concurrent-write and permanent
  rejection tests passed separately.

Coverage checks compare candidate-selected buffer coverage with the reference
path's actual uploads. Periodic checks are not a complete race detector or a
byte-by-byte gameplay oracle. User graphics confirmation remains pending.

## FPS interpretation and next work

Host presentation rates are **3.30–5.09/s** in these samples. On stages beat
their preceding off stages, but shadow (reference execution) also reached
5.09/s. There are only 2–3 complete ten-second presentation reports per stage,
after excluding the first report crossing the start boundary. Title FPS is a
different counter and fluctuates. This variability prevents attributing the
larger on2 presentation increase solely to BDA.

Keep this CPU candidate available with live off/shadow/on controls; current
process is left on with compiled SRT on. Await graphics confirmation. Next
measurement is a targeted production budget with both CPU candidates on to see
which remaining cost dominates. Previous captures identified long metadata and
indirect-argument readback waits; this BDA change does not modify those waits.
Do not infer the old wait durations or a new frame budget from this lightweight
capture. Any subsequent synchronization optimization needs separate producer/
consumer evidence and validation.

## Artifacts

Prefix: `D:\PS5\ufc5-profiles\production-20261006-155515`.

- `.csv.manifest.json`: launch settings and executable identity.
- `.bda-stage-{off1,shadow,on1,off2,on2}.json`: wall/CPU/FPS boundaries and samples.
- `.bda-fight.csv`: frozen BDA selection.
- `.bda-compiled-guard.csv`: frozen compiled-SRT guard.
- `.bda-fight.json` / `.bda-analysis.txt`: bounded analysis and checks.
- Live source: `.csv.bda-sync.csv` (continues growing).

[Implementation and live controls](bda-sync-experiment.md).
No commit, queue/barrier policy change or further restart.
