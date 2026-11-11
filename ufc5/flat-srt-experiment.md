# FlatSRT address-read recipe experiment

2026-10-06. Default off. Initial bounded target `d3dcf81c43080fd0`.

The all-shader observer measured FlatSRT refresh at 62.9% of matching sampled
materializer elapsed. Most descriptor roots are ReadConst; the first direct-getter
candidate had zero coverage. This experiment moves to their address-read dependencies.

## Candidate

After extracting the immutable ResourcePlan, predecode eligible LoadAddressU32 and
ReadConstBuffer structure: memory kind/offset, resolved operand values, literal bits
and direct user-data registers with existing dense memo indices. Other operands use
the original evaluator in their original order. The parent instruction still uses
the original memo, in-progress cycle detection, failure reset and result publication.

Recipes run only while RefreshFlatBuffer is active, including its separate clean
evaluator. Descriptor evaluation, control-flow traversal, source activity, specialization
and read capture remain shared. Current guest reads are retained; no runtime bytes,
cross-draw result, readiness state or GPU resource is cached. Buffer bounds, signed
address arithmetic, 48-bit masking, strict readers and short-circuit order are retained.
Unsupported raw reads/operands use the original path. No Vulkan work is changed.

## Live controls

```powershell
.\ufc5\tools\profile-production.ps1 -FlatSrt -FrameCount 64 -CpuDetailEvery 8
# Reach the same paused fight; keep the heavier GPU capture disarmed for the A/B.
.\ufc5\tools\set-flat-srt.ps1 -Mode off
.\ufc5\tools\set-flat-srt.ps1 -Mode shadow
.\ufc5\tools\set-flat-srt.ps1 -Mode on
.\ufc5\tools\set-flat-srt.ps1 -Mode off
python .\ufc5\tools\analyze-flat-srt.py D:\PS5\ufc5-profiles\production-TIMESTAMP.csv.flat-srt.csv
```

The new binary needs one restart to load this code. Its mode switches need no
further restart. Configuration: KYTY_FLAT_SRT_CONTROL_FILE, KYTY_FLAT_SRT_CSV,
KYTY_FLAT_SRT_SHADER. The launcher initializes off, records settings/binary hash in
the manifest, clears unrelated experiment variables and rejects combining descriptor
gather and FlatSRT flags. All new launch variables are restored afterward.

Shadow runs complete reference/candidate materializations on first encounter of each
plan and roughly every 64 target materializations thereafter. It returns reference
output and replays already observed memory reads for the candidate. It compares
success/failure, complete snapshot/specialization and exact strict/ordinary read order,
including failed reads, without reading guest memory twice. On requires a successful
comparison in which a recipe actually executed. New/unvalidated plans stay reference.
A mismatch rejects the plan and disables the experiment on the observing thread.

Two-second CSV aggregates include requested mode, all/fast/blocked materializations,
whole materializer elapsed, raw-read eligibility, actual recipe evaluations, direct
user-data/memo counts and full comparison failures. Shadow counters include reference
and candidate work. Raw eligibility includes reads outside refresh; **actual recipe
evaluations** establish executed coverage. Shadow reference/candidate timings have
different read-record/replay costs and must not be used as a speedup comparison.
Use stable off/on windows, then reverse the order, in the same scene with zero
mismatches and high validated coverage. The analyzer omits short transition windows
from performance groups but retains their mismatch/check totals.

The initial hash contributed only 135.624 ms materializer elapsed in the previous
20-second light window. A shader-local win is not a demonstrated large FPS gain.
Expand coverage only after exact validation and measured useful CPU savings.

## Validation / status

Focused tests cover moving plans, changed runtime values, shared memos, unsupported
operand fallback, current memory reads, user-data/buffer bounds, failed-read prefixes,
strict/ordinary separation, inactive control-flow paths, negative address underflow,
cycle detection and deliberate recipe corruption/rejection. The existing finite-image
refresh test also exercises compiled mode to verify clean reads and specialization.
`resource_materialization_tests --benchmark-flat-srt` is a synthetic full-materializer
benchmark with 96 scalar reads; its results cannot establish production speedup.

Emulator, harness and resource tests build successfully. Materialization and resource
tracking tests pass; nine production analyzer regressions pass. Eight saved fight
detiles return MATCH (0.082–0.479 ms isolated, five measured/two warmup). PowerShell
launch/control scripts parse successfully. No commit made.

Synthetic alternating off/on benchmark: six windows of 5,000 complete materializations
per mode. Median per-call elapsed off 12.952 us, on 11.217 us, approximately 13.4%
lower for this artificial 96-read plan. No FPS or production saving inferred.
Raw: `D:/PS5/ufc5-flat-srt-synthetic-benchmark.csv`.

New binary running PID 20052, trace prefix
`D:/PS5/ufc5-profiles/production-20261006-080708.csv`; FlatSRT off, heavy GPU capture
disarmed. User reached the paused fight; live off/shadow/on/off completed.

## Live result

Selected 289.059-second CPU window; 678 complete shadow comparisons, zero mismatches.
On was gated after 652 comparisons and observed recipe execution. Transition windows
can contribute correctness counters while being excluded from stable performance
groups. Actual stable on coverage is 63,414 / 63,414 fast materializations, zero
blocked, 5,263,362 recipe executions: 83 per materialization. Unlike the earlier
getter test, the candidate really executed.

| Mode | Stable windows | Calls | Weighted materializer us/call | Median window us/call |
|---|---:|---:|---:|---:|
| Off, pooled | 67 | 118,367 | 11.893 | 11.577 |
| On | 40 | 63,414 | 9.240 | 9.000 |
| Shadow | 27 | 43,290 | 12.291 | 11.740 |

On observed about 22.3% lower target materializer elapsed than pooled off. Initial
off weighted mean was 11.569 us/call; return-off was 11.996 us/call. The opposite
side of each A/B therefore still shows higher target CPU elapsed. Per-call elapsed
includes diagnostic counters/timers and possible host preemption; variant mixtures
remain aggregated under one shader hash. Shadow timing is not a speedup comparison.

This does **not demonstrate a whole-game FPS improvement**. Presentation-rate samples
drifted: initial off median 2.400/s, on 2.621/s, return-off 3.718/s (3, 7, 9 samples).
The first report after each stage boundary is excluded; it can include the preceding
mode. Weighted rates are 2.609 / 3.095 / 3.947/s. Workload/cadence is not stationary,
and the target's small contribution limits its potential whole-game effect.

Frozen selected CSV/JSON: adjacent `.flat-first-attempt.csv` and `.flat-first-attempt.json`.
Stage markers: `.flat-stages.jsonl`. To regenerate the freeze/summary while retaining
the same capture boundaries:

```powershell
python .\ufc5\tools\summarize-flat-srt-stages.py D:\PS5\ufc5-profiles\production-20261006-080708.csv
python .\ufc5\tools\test-flat-srt-analysis.py
```

The analyzer's focused regression passes, including transition mismatches and blocked
coverage. No crash observed. New-candidate visual confirmation remains pending;
prior measurement-build confirmation is not reused as evidence for this change.
Next: broaden immutable-plan coverage only with fresh shadow checks, keeping the
candidate default off. Larger readback stalls remain unresolved by this experiment.

## Broader controller (session 45, 2026-10-06)

The build now supports all shader hashes and live scope changes. It retains the
same recipe/evaluator semantics. Launch and control:

```powershell
.\ufc5\tools\profile-production.ps1 -FlatSrt -FlatSrtShader all -FrameCount 64 -CpuDetailEvery 8
.\ufc5\tools\set-flat-srt.ps1 -Mode shadow -Shader all
.\ufc5\tools\set-flat-srt.ps1 -Mode on
.\ufc5\tools\set-flat-srt.ps1 -Mode off
# Narrow a later test live; no rebuild/restart required:
.\ufc5\tools\set-flat-srt.ps1 -Mode shadow -Shader d3dcf81c43080fd0
```

Omitting -Shader preserves the selected scope. The reference path remains default;
all-shader selection does not itself enable recipes. Descriptor gather remains
separate. Only the control/logging/validation controller has changed in this build.

Each resource plan gets its own observation sequence. Shadow checks its first use
and approximately every 64 uses if it has recipes. In on mode, a newly encountered
plan gets a reference-returning full comparison before it can activate; plans with
no recipes remain reference. Unverified plans get periodic shadow retries, while
verified active plans are revalidated approximately every 512 uses. A failure
disables the experiment process-wide via a sticky atomic flag. Scope changes cannot
reactivate a rejected candidate. Each comparison uses recorded reads, not extra
guest reads; full output/specialization/failure/read-order checks are retained.

CSV adds per-hash `record_kind=shader` rows plus one `record_kind=summary` / `shader=all`
row for the selected scope at each report. The latter is the all-call aggregate
and must **not** be added to its component rows. Correctness counters are counted
once from shader rows. Analyzer groups different scopes separately.

Counters now split fast/reference/shadow materializations and their complete elapsed
costs. Counts and elapsed sums must close to the parent aggregate. Blocked counts
are a subset of reference calls requested under on mode. No-recipe counts indicate
immutable recipe availability, not safe runtime reuse. `fast_recipe_evaluations`
excludes shadow replay, so actual production execution is distinct from comparison
coverage. On-mode total elapsed includes revalidation; that overhead remains part
of the A/B, with its contribution reported separately. Hashes can aggregate static
variants; compare matched shader populations and total cost, not only fast-call means.
Control polling, map lookup and CSV serialization remain outside materializer brackets.

Gate tests cover default off, new/unverified plans, unsupported/rejected plans and
periodic revalidation. Materialization tests pass. Emulator/harness build; eight
saved detiles MATCH (0.082–0.477 ms isolated). Nine production analyzer tests and two
FlatSRT analyzer tests pass, including summary deduplication and cost/count closure.
Launch/control scripts parse and accept all/hash scope commands.

The broad live off/shadow/on/off test is complete in process 7600, prefix
`D:/PS5/ufc5-profiles/production-20261006-083333.csv`. It yielded 20,517 complete
comparisons with zero mismatches; user confirmed the on scene looked unchanged.
354 of 367 matched shader hashes executed fast calls, covering 96.34% of on calls.
Whole-materializer weighted cost fell from 9.395 to 7.530 us/call; normalizing the
on means to off's per-hash call counts gives 19.88% lower CPU elapsed. On total
includes periodic validation overhead. These are CPU-scope measurements, not
recovered frame wall time. Presentation cadence drifted substantially; no reliable
FPS improvement demonstrated. Candidate restored off/all; heavy profiler off.
See [broad fight results](flat-srt-broad-results-2026-10-06.md) for the tables, frozen
artifacts, measurement limits and next GPU packet/scheduling capture.
