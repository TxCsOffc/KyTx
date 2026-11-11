# UFC 5 (PPSA03541) work

Fork-specific notes and tooling for getting **EA Sports UFC 5** running in KytyPS5. Kept in its
own directory so the fork's additions stay separate from upstream and rebases onto
`KytyPS5/KytyPS5` stay clean.

## Start here

**[`ledger.md`](ledger.md)** is the source of truth. It records what was measured, what was
tried and failed, and *why* — including several changes that looked like wins and were
withdrawn. Read the top section before doing any performance work; it will save you from
repeating experiments that are already ruled out.

Latest: the [compiled SRT experiment](compiled-srt-experiment.md) is built behind
live off/shadow/on controls with per-plan validation and interpreter fallback.
9,000 differential refreshes and recovery tests pass; all eight detile replays
still match. [Paused-fight results](compiled-srt-results-2026-10-06.md) show ~71%
less materializer elapsed per call, 99.8% candidate coverage and zero mismatches.
No FPS win demonstrated; both on windows presented more slowly. The candidate
was initially returned to off for further investigation.

The [subsequent production comparison](compiled-srt-production-comparison-2026-10-06.md)
confirms ~48ms/epoch less command-thread work with compiled SRT, alongside
larger readback waits and detile GPU elapsed in the on capture. Causes remain
unresolved. The user prefers retaining the CPU saving; compiled SRT is now on.
The [dirty-only BDA port](bda-sync-results-2026-10-06.md) now shows79% less BDA
elapsed per call and99.8% fewer buffer visits, with validated live controls.
Both CPU candidates are on. FPS varies; the user observed9FPS briefly.
[The remaining production budget](after-bda-production-budget-2026-10-06.md)
finds20.1s of readback blocking in33.5s,74% metadata/26% indirect arguments.
GPU coverage is limited by query-page exhaustion; CPU events are complete.
The [subsequent writer-dependency measurement](readback-writer-results-2026-10-06.md)
corrects query capacity and records all62 interior epochs without drops.
CMask `0x1167f00000` has an earlier submitted writer bound and56ms median
GPU elapsed before its current copy; it is the first contained transfer-readback
candidate. The larger CMask wait includes detiles inside its required writer
batch; indirect arguments still require unsubmitted work. No synchronization
optimization or FPS improvement is claimed by this measurement. Graphics match.

The [GTA fork source review](gta-fork-review-2026-10-06.md) ranks native
indirect draws, last-writer transfer-queue readbacks and dedicated graphics
pipeline threads against our measured UFC 5 bottlenecks.
The revised first contained port is compiled SRT evaluation with reference/shadow
validation, then dirty-only BDA synchronization if measured cost warrants it.
Readback and threading candidates remain; FPS gains are unknown. The
retained-allocation control is still pending.

The [buffer/native residency report](replay-residency-results-2026-10-06.md)
associates seven slow replay scratch groups with non-local backing, while the
fast capture's surviving candidate is local. All 80 measured native packet joins
are complete. These creation/binding associations do not establish the game's
primary FPS cause. The next discriminating control retains game allocations while
briefly stopping submissions; no optimization or another normal replay yet.

The [depth/stencil upload pair reproducer](detile-upload-pair-results-2026-10-06.md)
builds and matches both image aspects. Its concurrent median is 2.739 ms; the
original standalone detile replay can also become slow while the game runs.
Same-build game-closed tests now match all eight references at 0.083–0.478 ms;
the full constructed upload pair is 2.803 ms and matches both aspects. The exact
cause of production inflation remains unresolved. See [usage and coverage](detile-replay.md).
The [correlated replay workflow](detile-replay.md#correlated-standalone-replay-trace)
now links each iteration to scheduler submits and runs it inside the GPUView
window. The [replay trace results](replay-trace-results-2026-10-06.md) now include
a successfully saved native ETL with zero lost events and all 80 measured packet
joins. The slow replay's 13.980 ms GPU bracket corresponds to 14.177 ms native DMA
residence, plus separate queue delay. Native replay allocations enter non-local
system memory; exact input/output binding remains unresolved. Allocation identity
instrumentation is built and reference-checked in the headless tool for the next
trace, without restarting the running game. No game FPS improvement is claimed.

Current state (2026-10-06): native-resolution fights render with graphical issues;
the user reports roughly **6–7 FPS**. A lower guest output resolution reduced VRAM
pressure but broke the scene and did not improve the reported FPS. The current
production profiler identifies substantial command-processing and synchronous
readback costs, plus intermittent detile/copy elapsed-time inflation. Read the
[measurement report](production-profile-report-2026-10-05.md) before optimizing.
The [targeted follow-up](readback-cpu-investigation-2026-10-05.md) identifies CMask
and indirect-draw argument readbacks, records finer CPU samples, and proposes three
bounded experiments. Scaled finer budgets failed reconciliation and are withheld;
resource materialization is the first recommended experiment, not an established win.
The default-off [descriptor evaluation experiment](descriptor-gather-experiment.md)
adds live off/shadow/on controls and exact candidate checks for one shader.
The first fight test found zero eligible whole descriptors for that shader; the
candidate never ran. The [all-shader expression observer](materializer-expression-profile.md)
has now completed a paused-fight window: 99.65% of sampled descriptor words have
ReadConst roots, and FlatSRT refresh is 62.9% of sampled materializer elapsed.
The [latest findings](measurement-follow-up-2026-10-06.md) rank command CPU and
synchronous readback waits, withhold the BDA budget that still fails sampling closure,
and recommend a FlatSRT experiment with exact shadow validation. No FPS gain proven.
The default-off [FlatSRT recipe experiment](flat-srt-experiment.md) has completed its
first live off/shadow/on/off test: 678 full comparisons with zero mismatches and
about 22% lower materializer elapsed for the selected shader. Actual fast coverage
is complete for the counted on calls. Presentation rates drifted; no reliable FPS
gain is demonstrated. The experiment is back off; larger readback stalls remain.
The [broader fight test](flat-srt-broad-results-2026-10-06.md) now covers 367 matched
shader hashes: 20,517 full comparisons, zero mismatches, user-confirmed unchanged
scene, and about 20% lower whole-materializer CPU elapsed. It still demonstrates
no reliable FPS gain. Controls remain off. The [OS scheduling trace](gpuview-report-2026-10-06.md)
now includes the Firefox-closed repeat: 16.137 seconds of command-thread readback
blocking versus 12.726 seconds running in a 30.018-second window. Long waits and
CPU feed gaps persist without Firefox; the single pair does not isolate its FPS
effect. The helper needs Administrator PowerShell and requires no emulator restart.
The application/OS capture is now complete in the
[correlated report](gpuview-correlated-report-2026-10-06.md):
517 readbacks join to submits/native progress, with 98.46% of matched host wait before
DMA entry and only 12.462 ms total copy scopes. Earlier detile/upload batches remain
slow in production; this does not yet explain their GPU scope inflation. Native
dependency joins are partial. The capture has measured profiler overhead; no FPS win.

## Tools

| | |
|---|---|
| `tools/run_ufc5.bat` | Launch with the configuration that measured 5 fps. Wrapper over the `.ps1`. |
| `tools/run_ufc5.ps1` | The real script. `-Baseline` / `-Validate` / `-Verify` variants for A/B. |
| `tools/frame_stats.py` | Compare draw-normalized run statistics; use production profiles for wait/GPU attribution. |
| `tools/devenv.ps1` | MSVC/clang-cl build environment. |
| `tools/drive_ufc.py`, `ufc5_pad.py`, `ufc5_keys.py`, `*.route` | Virtual pad menu navigation (vgamepad + ViGEmBus). |
| `tools/capture-kyty.ps1` | Log capture helper. |
| `tools/capture-detile.ps1` | Save large GPU detile inputs from a fight. See [detile-replay.md](detile-replay.md). |
| `tools/replay-detile.ps1` | Rebuild and replay saved detiles without loading UFC 5; check output and GPU latency. |
| `tools/test-output-resolution.ps1` | Test a guest-visible 1080p output report and compare surface sizes/VRAM; see [resolution-test.md](resolution-test.md). |
| `tools/profile-production.ps1` | Launch a separate profiling executable, then arm bounded capture windows in-game without restarting. See [production-profile.md](production-profile.md). |
| `tools/set-descriptor-gather.ps1` | Switch the descriptor experiment off/shadow/on during a running fight; use `profile-production.ps1 -DescriptorGather` to launch it. |
| `tools/analyze-production-profile.py` | Export CPU/GPU epochs, targeted readback lifetimes, sampled CPU calls/repetition, budget quality checks, barriers and submission gaps. |
| `tools/test-production-profile.py` | Check analysis attribution, background wait separation, timestamp wrap and sampling semantics. |
| `tools/capture-gpuview.ps1` | Capture a bounded OS GPU/CPU scheduling baseline in Administrator PowerShell, without restarting Kyty. Use `-CheckOnly` for read-only preflight. |

```
ufc5\tools\run_ufc5.bat
python ufc5\tools\frame_stats.py D:\PS5\KytyLog-PPSA03541-run.txt
```

## Judging a change

`fps` from a single `FrameProfile` line is worthless — scenes vary 2,400–4,100 draws/frame, so
two runs of the "same" fight differ by 70%. `frame_stats.py` normalises by draw count, drops
warm-up, and reports the median. Two changes were called wins during this work and had to be
withdrawn; both were caught by this method and by nothing else.

Also: every `FrameProfile` field accumulates over its window, and `frames=N` is the window
length — `draws=7971 frames=3` is **2,657 draws/frame**, not 7,971. `GpuBusy` and `GpuDraws`
windows *are* one frame.

## Paths

These scripts assume a specific layout and hardcode it as defaults:

```
D:\PS5\Games\UFC5              game dump
D:\PS5\Emulators\KytyPS5-Bin   deployed emulator
D:\PS5\                        log output
```

`run_ufc5.ps1` takes `-GameDir`, `-BinDir` and `-LogDir` to override them. `ledger.md` quotes
absolute paths throughout because it is a working log, not documentation — treat the paths as
belonging to the machine it was written on.
