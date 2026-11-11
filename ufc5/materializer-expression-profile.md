# Materializer expression and CPU attribution measurement

2026-10-06. Measurement only; reference evaluator retained. The first descriptor
experiment had zero whole-source coverage for shader `0xd3dcf81c43080fd0`.
This follow-up measures all shader hashes encountered in a bounded fight window.

## Capture without restarting between windows

```powershell
.\ufc5\tools\profile-production.ps1 -MaterializerDiagnostics -MaterializerSeconds 20 -FineCpu -FrameCount 64 -CpuDetailEvery 8
# Reach the paused fight, then collect the light materializer window first:
.\ufc5\tools\profile-production.ps1 -Action ArmMaterializer
# After its completion marker, collect the separate production CPU/GPU window:
.\ufc5\tools\profile-production.ps1 -Action Arm
python .\ufc5\tools\analyze-production-profile.py D:\PS5\ufc5-profiles\production-TIMESTAMP.csv
```

The launch manifest saves the binary hash, environment, window duration and separate
control file. `ArmMaterializer` is consumed at a 500 ms poll and automatically ends
after the configured duration. It uses the original materializer during the window,
including original reads, memo behavior, activity/clean handling and specialization.
The heavier GPU profile can stay disarmed for the first window. Subsequent captures
in this build need no restart. Runtime options: `KYTY_MATERIALIZER_PROFILE_CONTROL_FILE`,
`KYTY_MATERIALIZER_PROFILE_CSV`, and `KYTY_MATERIALIZER_PROFILE_SECONDS` (1–120).
Without configuration, the diagnostics wrapper returns immediately.

## Observations

`<production trace>.materializer.csv` contains:

- Every-call materializer count/CPU elapsed, grouped by thread and shader hash.
- Approximately 1/32 sampled materializations: flat-SRT refresh, uniform-fill
  evaluation, buffers, images, samplers and final specialization timings.
- Sampled interpreter visits, memo hits/misses and failed instruction evaluations.
- Sampled descriptor word counts and elapsed evaluation, grouped by root opcode.
- Once-per-live-plan descriptor graphs: source/word root type, structural pure-word
  classification, existing complete-gather eligibility, and reachable instruction
  counts. ReadConst links include the referenced SRT expression. Walks are bounded
  to 2,048 unique instructions and mark truncated graphs.

Counters do not time every recursive instruction. Word elapsed includes dependency
evaluation and memo accesses; it is not pure opcode execution cost. Structural graph
counts are not execution counts, and plan pointers are process-local identities.
Hash aggregates can include multiple static variants/stages. Sampling, timers and
host preemption affect observed time; no inverse-sampling frame budget is claimed.
Exclusive phase sums are checked against the same sampled parent, with unbracketed
setup retained as remainder. No extra guest reads are performed.

The existing analyzer exports `materializer-report.md`, `materializer-shaders.csv`,
`materializer-phases.csv`, `materializer-opcodes.csv` and `descriptor-graphs.csv`.
For just the light sidecar:

```powershell
python .\ufc5\tools\materializer_profile.py D:\PS5\ufc5-profiles\production-TIMESTAMP.csv.materializer.csv
```

## Correcting recorder overhead

Production event rows now include `probe_end_ns`: the host time after constructing
and appending a record under the recorder lock. The interval after the measured scope
end is recorder work. The analyzer unions those intervals and removes them from
foreground CPU and finer self attribution, including same-thread events from either
scheduler. It reports `profile_record_emission_cpu_ms` and recording time excluding
waits/submits/profiler work. Older captures lack this observation and default to zero;
their unknown overhead cannot be reconstructed.

This does not measure every hook cost: constructor branches, lock release, trailing
clock reads and other uninstrumented overhead remain. CSV serialization is still
synchronous and separately timed. Failed sampling closure still withholds scaled
budgets. A direct `cpu_prepare_bda` bracket is present in the finer production capture.

## Validation / current run

Emulator/harness and resource tests build. Materialization tests compare profiled and
unprofiled complete outputs, verify memo counters and check phase sum closure.
Resource-tracking tests pass; all eight saved detiles MATCH (0.080–0.477 ms isolated).
Nine analyzer tests pass, including explicit recorder-overhead exclusion and separate
materializer thread/root categories. No commit or renderer optimization was made.

Launched PID 1344, trace prefix
`D:/PS5/ufc5-profiles/production-20261006-064620.csv`.
Both paused-fight windows completed and controls are off. User confirmed the scene
looks the same. The separate 20-second light window recorded 375 hashes, 242,016
materializations and 2,083.408 ms all-call elapsed. FlatSRT accounts for 52.900 ms
of 84.073 ms matching sampled parent elapsed (62.9%); no phase closure failures.
ReadConst is the root of 99.65% of sampled descriptor words. The main capture has
zero drops/splits/uncollected batches, but binding/BDA sampling closure still fails,
so its scaled budget is withheld. Read the
[completed findings](measurement-follow-up-2026-10-06.md) for waits, submission gaps,
measurement limits and ranked next steps. No optimization was enabled.
