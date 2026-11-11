# Compiled SRT evaluation experiment

## State — 2026-10-06

Contained evaluator port from TheCruZ/KytyPS5-GTA commit
`e42d611aa0d7a40b80eefb1cc1260abb069894fb`, adapted to the local resource-plan
and EXEC/ReadFirstLane semantics. Default remains the interpreter. The first
[paused-fight test](compiled-srt-results-2026-10-06.md) measured ~71% less
materializer elapsed per call, 99.8% candidate coverage and zero mismatches.
No FPS benefit demonstrated; both on windows were slower in presentation rate.
Visual confirmation and correlated slowdown investigation remain pending.

The compiled graph stores immutable operations and operands. Evaluation scratch
is reused with a fresh generation for each refresh; guest bytes are read anew
through the existing ordinary and strict callbacks. This experiment does not
port the fork's guest-memory cache, BDA, threading or Vulkan synchronization changes.

## Live controls

With Kyty closed, launch from the repository:

```powershell
& .\ufc5\tools\profile-production.ps1 -CompiledSrt
```

Drive to the same paused fight, then switch without restarting:

```powershell
& .\ufc5\tools\set-compiled-srt.ps1 -Mode off
& .\ufc5\tools\set-compiled-srt.ps1 -Mode shadow
& .\ufc5\tools\set-compiled-srt.ps1 -Mode on
```

Control defaults to `D:\PS5\ufc5-compiled-srt.control`; polling is every 500 ms.
Launch configures `KYTY_COMPILED_SRT_CONTROL_FILE` and `KYTY_COMPILED_SRT_CSV`.
Without the control environment variable, normal materialization is unchanged.
Do not arm the detailed production profiler during the initial comparison.
The launcher excludes simultaneous FlatSRT/descriptor-gather experiments.

All shader plans are in scope, with these gates:

- **off:** interpreter; no compiled graph creation.
- **shadow:** interpreter output; full comparison on the first call for each
  eligible plan and every 64 calls thereafter.
- **on:** compiled evaluator only after a successful complete comparison;
  repeat comparison every 512 calls per plan. New or unverified plans remain on
  the reference path until a successful check.
- Unsupported plans and active expression/recipe observers use the interpreter.
- A mismatch permanently disables the candidate for the running process.

Full checks record the interpreter's reads and replay those captured values into
the candidate. They compare result/failure, complete resource snapshot,
specialization, source activation, and exact read address/width/order/strictness.
Checks do not read guest memory twice and return the reference output and
activation state even on mismatch.

## Timing and interpretation

The launch manifest identifies the binary hash, process and output paths.
`production-*.csv.compiled-srt.csv` aggregates every two seconds and on mode
changes, with per-thread shader rows and a total row. Do not add shader rows to
total rows. `window_ms` is elapsed host time, not a frame count.

Columns separate reference calls, actual candidate calls, comparison calls,
unsupported calls, materializer elapsed time, compilation time and mismatches.
Polling, aggregation and file flushing are outside materializer timing but still
have some host overhead. Off/on use the same timing controller.

`shadow_reference_ms` and `shadow_candidate_ms` use recorded/replay readers and
are validation timings; they do not measure production speedup. Compare stable
off/on/off paused-fight windows, candidate coverage, materializer cost per call
and host presents/FPS. Exclude boot, mode-transition and compilation windows.
Unchanged output must also be checked by the user.

## Verification

- Release build: emulator, resource materializer tests and headless Vulkan harness PASS.
- 1,500 randomized graphs × 6 refreshes: **9,000** identical read/output comparisons;
  3,081 successful and 5,919 failed refreshes.
- Existing materializer cases, direct reads, unsupported graph fallback and
  full shadow checks PASS.
- Live off/shadow/on controller tested, including periodic corruption detection,
  reference output preservation and rejection that cannot be reset live: PASS.
- The initial fallback test accidentally evaluated a malformed synthetic
  expression in the reference interpreter and crashed the test executable. The
  case was corrected to exercise an unreferenced unsupported source; final tests pass.
- Existing eight game-closed detile replays: all **reference MATCH**, GPU medians
  0.082–0.477 ms. This verifies the retained replay path, not compiled-SRT gameplay.
- Synthetic 96-slot graph benchmark, six alternating rounds per mode:
  median reference 13.773 µs/call, candidate 3.777 µs/call (~3.65×). Synthetic
  readers and graph are not UFC performance or a predicted FPS increase.

Artifacts under `D:\PS5\ufc5-profiles`:

- `compiled-srt-build-final-20261006.log`
- `compiled-srt-test-build-20261006.log`
- `compiled-srt-tests-20261006.txt`
- `compiled-srt-benchmark-20261006.csv`
- `compiled-srt-detile-check-20261006.csv` and `.txt`

Off/shadow/on/off/on stages are complete; candidate returned to off. Next:
visual confirmation and existing production off/on profiling to explain the
presentation-rate difference before deciding whether to retain the candidate.
