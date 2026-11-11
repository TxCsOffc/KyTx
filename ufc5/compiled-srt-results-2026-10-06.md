# Compiled SRT paused-fight results — 2026-10-06

## Result

The compiled evaluator reduces measured production materializer elapsed time
per call by approximately **71% (3.4× faster)**. This is a repeatable saving in
the measured path. It has **not demonstrated an FPS improvement**. Both on
windows had lower presentation rates than their preceding off windows;
possible game-level regression remains unresolved. Keep the candidate off
pending a correlated CPU/GPU comparison and the user's visual check.

## Run identity and method

- RTX 3070 / i7-10700, native guest output; same paused fight supplied by user.
- PID24900, same process throughout; no restarts between stages.
- Binary SHA256 `B83FE45ECBFFF8645026EEE7B71184F24F51FBA3107CAA221D11917B6BC5E7B3`.
- Manifest: `D:\PS5\ufc5-profiles\production-20261006-143328.csv.manifest.json`.
- General production epoch profiler remained off. FlatSRT and descriptor-gather
  experiments were unconfigured. Compiled evaluator controls alone changed.
- Each stage measured approximately40s. Only complete >=1.5s CSV windows fully
  contained in the stage enter the materializer aggregates. Mode boundaries,
  initial compilation and inter-stage gaps are excluded from those aggregates.
- Host presentation reports use fresh shared-file EOF offsets, exclude each
  stage's first report that could cross the boundary, and weight by reported
  elapsed time. Three complete reports per stage; title FPS sampled every5s.
- This measures elapsed host scopes, including read callbacks and preemption,
  rather than pure executing CPU time. Identical controller overhead exists
  in off/on; polling and CSV flushing sit outside those scopes.

## Stable windows

| Stage | Calls | Actual candidate | µs/call | Materializer ms/s | Host presents/s | Median sampled title FPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| off1 | 663,890 | 0% | 8.072 | 146.90 | 4.34 | 3.5 |
| shadow | 712,091 | 0% | 8.276 | 153.79 | 3.62 | 3.5 |
| on1 | 806,640 | 99.82% | 2.355 | 49.59 | 3.96 | 3.5 |
| off2 | 839,167 | 0% | 8.075 | 177.53 | 5.32 | 6 |
| on2 | 523,253 | 99.77% | 2.351 | 33.73 | 3.14 | 3 |

Sampled materializer durations:36.48,38.32,38.31,38.17,36.47seconds respectively.
The remaining on calls are full reference/candidate comparisons, not unsupported
fallbacks. Zero unsupported calls and zero reference-only calls in the stable
on windows. Call and elapsed accounting close in every stage.

Presentation rates and title FPS are distinct counters. The short, sequential
windows do not establish the cause of the apparent game-level slowdown. They
also do not support claiming a whole-game performance win. Raw stage wall time
and per-process CPU time are retained in the JSON; whole-process CPU is not
the command-thread budget.

## Validation and shader examples

Across the frozen selection, including transition/gap rows, **21,742** full
checks, **zero mismatches**. Stable shadow contains11,303 checks; stable on1
1,463 and on2 1,180. Shadow never follows the candidate's pointers into guest
memory: it replays the reference's ordered ordinary/strict reads. The on
experiment uses actual production readers between periodic checks.

1,327,250 actual candidate calls across the two stable on windows. Scope
includes supported observed shader plans, beyond the former narrow FlatSRT
recipe experiment. Compilation was11.56ms in the contained shadow rows and
zero in both stable on windows; initial/edge compilation is excluded from
the stable timing totals.

| Shader | off1 µs/call | off2 µs/call | on1 µs/call | on2 µs/call |
| --- | ---: | ---: | ---: | ---: |
| d3dcf81c43080fd0 | 11.11 | 11.17 | 4.13 | 4.04 |
| df3633d8030ed2a3 | 3.36 | 3.37 | 0.77 | 0.77 |
| 682f2b7a701aeed7 | 17.54 | 17.74 | 8.15 | 7.88 |
| 1bcc68ffb7b0469e | 39.13 | 38.77 | 13.89 | 13.93 |

These common-shader savings support the path-specific conclusion despite
different call rates. They do not explain the presentation-rate difference.
User visual comparison was requested while on1 was active; reply is pending
as this report is written. No emulator crash observed in these windows.

## Artifacts and next measurement

All under `D:\PS5\ufc5-profiles\production-20261006-143328.csv`:

- `.compiled-srt.csv`: continuing live output.
- `.compiled-stage-{off1,shadow,on1,off2,on2}.json`: QPC/UTC boundaries, EOF
  offsets, process CPU and sampled titles.
- `.compiled-fight.csv`: frozen selected completed rows.
- `.compiled-fight.json`: stable totals and top shader breakdowns.

Reproduce summaries:

```powershell
python .\ufc5\tools\analyze-compiled-srt.py D:\PS5\ufc5-profiles\production-20261006-143328.csv
```

Analysis excludes incomplete CSV rows, avoids summing per-shader and total rows
together, and keeps replay-only shadow timings separate from production costs.
See [implementation and controls](compiled-srt-experiment.md).

Next: establish unchanged visuals, then arm the existing production profiler
for short off/on windows in this running process. Compare command-thread work,
readback host waits, GPU batch durations and queue gaps. Determine whether
presentation drift is independent or correlated with enabling the candidate
before retaining it or attempting dirty-only BDA. Do not bundle another change.

Final state: emulator running/responding, compiled control **off**, detailed
production profiler **off**. No new commit.

Follow-up: user prefers leaving the candidate on for continued testing to retain
the measured path saving while investigating other bottlenecks. Live control
returned to **on** for PID24900; validation gates and periodic comparisons remain.
This does not resolve the presentation-rate difference or establish an FPS gain.
The launcher still starts off for a controlled baseline.
