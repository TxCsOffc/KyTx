# Dirty-only BDA synchronization experiment

2026-10-06. Adapted from local GTA fork commit
`e5dd76e7f111d577d957840257216b56f07afcf0`.
[Gameplay measurements](bda-sync-results-2026-10-06.md): 79.1% lower BDA CPU
elapsed per call; overall FPS gain not established. Graphics confirmation pending.

## Scope

`PrepareBda` currently walks all mapped ranges and overlapping cached buffers.
The candidate uses CPU dirty-page snapshots, global/per-region dirty epochs,
buffer registration epochs and mapping versions to select affected buffers.
It then synchronizes each selected buffer's original mapped intersections,
in reference address order, preserving existing copy/barrier batching.

No changes to GPU readback policy, queue waits, resource barriers, shaders,
render resolution or output. Compiled SRT remains enabled during this test.
The BDA renderer selection is opt-in; without its control environment variable,
the original scan runs. Dirty tracking adds atomic bookkeeping on dirty-state
changes even when the candidate is off.

## Correctness rules

- Publish the CPU dirty hint before advancing local/global epochs.
- Publish new tracker regions before advancing the global epoch.
- Remember epochs captured before snapshots/uploads. A concurrent later write
  must remain visible on the next pass.
- Scan all mapped intersections of a region against the same captured epoch.
- Registration/removal and map/unmap invalidate the clean shortcut.
- Dirty snapshots do not consume pages, guest bytes or GPU ownership.
- Preserve full selected-buffer upload batching, rather than uploading each
  dirty span independently.

## Live controls

Launch once:

```powershell
& 'D:\PS5\src\KytyPS5\ufc5\tools\profile-production.ps1' `
  -CompiledSrt -CompiledSrtInitialMode on -BdaSync -FrameCount 64 -CpuDetailEvery 8
```

Requires the old emulator to be closed. Launcher builds only with `-Build`;
otherwise installs the already-built executable. Starts BDA off, compiled SRT
on, native output, sparse BDA enabled, detailed production profiler disarmed.

Switch without restarting:

```powershell
& 'D:\PS5\src\KytyPS5\ufc5\tools\set-bda-sync.ps1' -Mode shadow
& 'D:\PS5\src\KytyPS5\ufc5\tools\set-bda-sync.ps1' -Mode on
& 'D:\PS5\src\KytyPS5\ufc5\tools\set-bda-sync.ps1' -Mode off
```

Polling is approximately 500 ms. Shadow executes the original uploads and
periodically compares their actual page-aligned ranges with candidate coverage.
On requires a successful comparison with registered-buffer visits, then checks
periodically. Stable uncovered uploads disable the candidate for this process;
the comparison has already executed the correct reference uploads. A mismatch
accompanied by a concurrent CPU epoch change is inconclusive. Such a check cannot
validate an unverified candidate.

Counters report every two seconds and on mode changes to
`<launch-prefix>.bda-sync.csv`: elapsed CPU time, reference/planned/executed buffer
visits, dirty bytes, actual upload runs/bytes, unchanged/full passes, region
skips, comparisons, raced checks, mismatches and rejection state.

## Gameplay measurement

Use the same paused fight for off1 / shadow / on1 / off2 / on2, with compiled SRT
on throughout and the detailed production profiler off initially. Confirm
graphics after enabling. Reuse the bounded capture helper:

```powershell
& 'D:\PS5\src\KytyPS5\ufc5\tools\measure-compiled-srt-stage.ps1' `
  -EmulatorId <PID> -Prefix '<launch-prefix-without-.csv>' -Stage off1 `
  -ModeControlFile 'D:\PS5\ufc5-bda-sync.control' -FileTag bda -Seconds 40
python 'D:\PS5\src\KytyPS5\ufc5\tools\analyze-bda-sync.py' '<launch-prefix-without-.csv>'
```

The analyzer freezes bounded rows, uses only fully contained >=1.5-second
windows for timing, checks count/time closure, and reports host presents and
title FPS separately. Shadow timing includes candidate planning plus reference
execution. BDA CPU elapsed includes preemption and upload recording; multiple
GPU threads may have overlapping reporting windows. Epochs are not display
frames. An FPS benefit must be measured, not inferred from fewer buffer visits.

## Verification

- Emulator and headless Vulkan harness compile.
- `memory_tracker_tests`: PASS. Actual dirty snapshots, clean skips, disjoint
  mappings, GPU ownership, registration/mapping invalidation, concurrent write
  after snapshot, live verification/retry and permanent mismatch fallback.
- `shader_recompiler_compute_tests --bda-sync-only`: PASS on RTX 3070 with
  both sparse and default BDA tables.
  Checks actual native GPU bytes, initial shadow coverage, clean skip, two
  dirty runs batched into one buffer visit, registration and remapping,
  GPU-owned bytes, and reference output after an injected selection error.
- `resource_materialization_tests`: PASS, including 9,000 compiled-SRT
  differential refreshes and live-control fallback (deliberate mismatch).
- Launcher, stage helper and switch script: PowerShell syntax PASS.

During harness development, a clean-fast-skip assertion was corrected to allow
the second pass to observe newly published tracker regions; that pass had zero
uploads. Temporary guest backing allocation failures cleared after closing the
old emulator and allowing Windows to release it. No game code changed for either.

Test/build logs: `D:\PS5\ufc5-profiles\bda-sync-*20261006.*`.
Gameplay timing is recorded in the linked results; graphics confirmation remains pending.
