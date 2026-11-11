# Descriptor evaluation experiment

Initial scope: shader `0xd3dcf81c43080fd0`, one measured UFC 5 materialization target.
Default off. No descriptor values are cached across refreshes, and no guest reads,
barriers, waits, resource lookups or Vulkan work are removed.

## Run and switch without restarting

```powershell
.\ufc5\tools\profile-production.ps1 -DescriptorGather -FrameCount 64 -CpuDetailEvery 8
# Drive to the paused fight. Start with an off baseline, then:
.\ufc5\tools\set-descriptor-gather.ps1 -Mode shadow
.\ufc5\tools\set-descriptor-gather.ps1 -Mode on
.\ufc5\tools\set-descriptor-gather.ps1 -Mode off
```

The GPU thread polls the separate descriptor control file every 500 ms when this
shader is encountered. `off` runs the reference evaluator. `shadow` returns reference
outputs and compares candidate descriptor bytes, width and success/failure, including
partial failure output. Each resource plan receives a full comparison once, then
approximately every 64 target materializations. Full checks compare all fields of
`ResourceSnapshot` and `ResourceSpecialization`, success/failure, and the ordered
strict/ordinary read requests. The candidate replays the values obtained by the reference;
it does not call the guest memory readers again. Inputs remain current each refresh.

`on` uses the candidate only for a resource plan that passed a full shadow check
with eligible descriptor calls. Unvalidated plans use the reference. Any mismatch
marks the owning plan rejected and returns the thread's experiment to `off` for the
rest of the process. Replacing a plan replaces its gather plan and validation state.
Unsupported expression words, memory loads and indirect-image sources use the original
interpreter. The source activity gate, flat-SRT refresh, strict writable evaluation,
captured specialization reads, validation and specialization generation remain shared.

## Implementation boundary

`ExtractResourcePlan` classifies complete sources containing only resolved U32
constants or direct `GetUserData(ScalarReg)` words. The immutable gather stores constants,
absolute registers and the original instruction's memo index. Runtime evaluation retains
user-data-base subtraction and bounds checks, zero initialization, prefix output on
failure, and the existing per-session instruction memo. It bypasses value resolution
and opcode dispatch for eligible words. It does not bypass the memo's current value.

The live experiment wrapper is in `pipelineCache.cpp`, at both resource materialization
call sites. The standalone materializer/evaluator APIs default to the original behavior;
tests can select modes explicitly. A direct `cpu_prepare_bda` scope was also added for
the next focused capture. Fine sample extrapolation remains unsuitable for failed
closure categories; this experiment does not resolve all probe-emission overhead.

## Measurements

The launcher writes `<production trace>.descriptor.csv` and records the target and
control file in the launch manifest. Rows aggregate two-second windows separately by
thread and mode: target materializations, candidate/blocked calls, CPU materializer
elapsed, descriptor eligibility, fallback counts, words, descriptor checks and full
checks/mismatches. Mode changes flush the previous window. These counters run without
arming the heavier production profiler. Shadow times include diagnostics and must not
be compared to off/on as an optimization result.

Compare stable `off → shadow → on → off` windows in the same paused fight. Exclude
startup, mode transitions and heavy capture windows. Report CPU elapsed per target
materialization alongside guest FPS and eligibility; a shader-local improvement may
have no visible FPS effect. GPU/host waiting and other shaders remain unchanged.
Check changing guest state and visuals before expanding the scope. Existing replay
MATCH remains a regression check, not proof that this CPU path is correct.

## Validation status

Focused tests cover changing user data across refreshes, nonzero user-data base,
lower/upper bounds failures and their partial descriptor outputs, unsupported
arithmetic/non-U32 fallback, moved plans, full snapshot/specialization equality,
strict/ordinary reads without duplicate guest access, failed reads, inactive sources,
and rejection/rollback after a deliberately corrupted candidate.

Emulator, materialization tests, resource-tracking tests and headless harness build.
All focused materialization/tracking cases pass, as do seven analyzer regression tests.
All eight saved fight detiles remain reference MATCH (0.083–0.477 ms isolated medians).

## First paused-fight result: no coverage

2026-10-05, PID 21632, thread 23756. The user reached the paused fight. Live control
ran `off → shadow → on → off`, without another restart. The main GPU profile window
remained disarmed. Source log:
`D:/PS5/ufc5-profiles/production-20261005-230631.csv.descriptor.csv`.
Frozen stable windows and summaries are adjacent `.descriptor-first-attempt.csv` and
`.descriptor-first-attempt.json`. Selected windows start after host tick
621887188886900 and finish by 622046257372500; windows shorter than 1.5 seconds are
excluded. These ticks are host steady-clock values, not GPU timestamps.

| Requested mode | Stable windows | Materializations | Descriptor calls | Eligible calls | Median window CPU / materialization |
|---|---:|---:|---:|---:|---:|
| off | 24 | 66,342 | 729,762 | 0 | 10.999 us |
| shadow | 28 | 71,586 | 787,446 | 0 | 11.378 us |
| on | 23 | 47,506 | 522,566 | 0 | 10.879 us |

All sources for this shader require the interpreter fallback under the current
**whole-source** eligibility rule. This does not tell us whether individual pure words
occur inside mixed sources; their per-word types were not recorded.
Shadow performed 1,120 full snapshot/specialization/read-order comparisons with zero
mismatches, exercising the unchanged fallback evaluator. No eligible descriptor
comparison ran. Consequently the enable gate correctly blocked all 47,506 requested
`on` materializations: **zero materializations used the candidate path**.

This is a coverage result, not an optimization A/B. The small CPU difference between
off and requested-on compares the same reference algorithm; it is not a saving.
No FPS improvement is demonstrated. Raw FPS samples vary with scene/workload and
are not assigned to the candidate, which did not execute. Kyty is still running and
the control is verified back at `off`.

### Next measurement

Record actual per-word descriptor IR expressions and dependencies for this shader,
then rank them by observed frequency/cost. Determine whether constants/current
user-data words occur inside mixed sources. A wordwise plan could keep unsupported
words on the original interpreter in the original order, but requires the same exact
failure/memo/read-order validation. Also inspect the other measured hot materializers
before expanding the target. Do not skip SRT memory reads or cache previous descriptor
payloads based on this result.
