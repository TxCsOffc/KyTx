# GTA fork review for UFC 5 — 2026-10-06

## Recommendation

TheCruZ's fork has relevant implementations for the bottlenecks measured in
UFC 5. After reconsidering port size, testability and correctness risk, the first
contained port to investigate is **compiled SRT evaluation** from `e42d611a`,
followed by dirty-only BDA synchronization after its aggregate budget is measured.
Keep the reference evaluator and use off/shadow/on validation for the compiled
path. This revises the initial recommendation to start with indirect draws.

Our earlier materializer capture measured 2,083.408 ms in a 20 s window (10.4%
elapsed, including measurement/preemption effects). The fork's evaluator speedup
does not establish a whole-game speedup. Metadata and indirect readback work
remain important subsequent targets; the transfer-queue and graphics-thread
pipeline ports need broader ordering and lifetime changes.

This was a source review and patch applicability check. No fork patch was applied,
no graphics behavior changed, no build or gameplay test ran, and no FPS improvement
is claimed. The previously planned retained-allocation suspension control was
paused for this review; no suspension helper was created and no suspension occurred.

## Revisions reviewed

- [TheCruZ/KytyPS5-GTA](https://github.com/TheCruZ/KytyPS5-GTA), fetched `main`:
  `ef5b8df43ac2074812380123ad0abe65352640fd`.
- Latest fetched upstream: `7d2422abbe84f560f5eb40e3b722166768c0bd91`.
- Fork/upstream merge base: `cdb64bfd4fb4d5312b2542a41a0070f25663166a`.
  Git lists 39 fork-only commits and nine newer upstream-only commits. The fork
  is therefore not identical to today's upstream plus its patches.
- Our checkout HEAD: `d1e89a309766d98a9416163d48104d58461a830d`, with substantial
  uncommitted UFC 5 work. Comparisons include the actual working files.
- Fetched into `gta-audit/main` and refreshed `origin/main`; no checkout or merge.

The fork's reported GTA performance is the author's result on a different game
and machine. It is not a forecast for UFC 5 on this RTX 3070.

## Reviewed candidates

The identifiers below retain the initial review's numbering, not the revised
implementation order above.

| ID | Change and source | Match to UFC 5 | Current status / conditions |
|---|---|---|---|
| 1 | Native indirect draws, part of [33b16934](https://github.com/TheCruZ/KytyPS5-GTA/commit/33b169346deb45c0da40462f900c8e7f66413890) | Our `DrawIndirect`/`DrawIndirectMulti` CPU-load GPU-produced arguments; `0x1140008000` incurred 1,270.387 ms of matched host wait in the correlated 30 s window. | Absent locally. Fork uses Vulkan indirect/count commands and rejects CPU-dependent cases. Must preserve persistent instance counts and index/draw semantics. This targets one readback class, not the dominant metadata waits. |
| 2 | Last-writer transfer-queue readback, also `33b16934` | 98.46% of matched readback wait preceded the readback packet's DMA entry. Waiting for the relevant writer rather than all subsequently queued work could help. | Absent locally. Needs range writer ticks, conservative BDA writer tracking, transfer-family buffer sharing, pending-publication ordering and fallback for unsubmitted writers. Still blocks for required data; does not remove its production cost. |
| 3 | Dedicated graphics pipeline threads: [e43ed2c8](https://github.com/TheCruZ/KytyPS5-GTA/commit/e43ed2c8dd03bf859c0b24a209af05f9a1aa5b69), then [45328020](https://github.com/TheCruZ/KytyPS5-GTA/commit/453280207ad34094970638c3a2c857bee06a9893) | Command-thread running time is substantial: 16,252.471 ms in the correlated 30.068 s window, alongside 12,683.591 ms readback off-CPU. | Local code lacks this separate execution/resolve/recording pipeline. Major architecture port with ordering, lifetime and profiler implications. Parallel CPU work does not itself remove a required metadata dependency. |
| 4 | Dirty-only BDA synchronization: [e5dd76e7](https://github.com/TheCruZ/KytyPS5-GTA/commit/e5dd76e7f111d577d957840257216b56f07afcf0) | Local `PrepareBda` traverses mapped ranges and all overlapping buffers; repeated scans were observed. | Fork dirty-region/global epochs and registration/map invalidation guards are absent. UFC 5's aggregate BDA budget remains unreconciled; measure before predicting benefit. |
| 5 | Compiled SRT evaluation: [e42d611a](https://github.com/TheCruZ/KytyPS5-GTA/commit/e42d611aa0d7a40b80eefb1cc1260abb069894fb) | Resource materialization has measured CPU cost. | Fork compiles a general node graph and retains dynamic reads/fallback. Our FlatSRT recipe experiment already reduced materializer cost about 20% without reliable FPS gain; this is broader, but incremental benefit is unknown. |
| 6 | Known metadata fills: [8c0762fe](https://github.com/TheCruZ/KytyPS5-GTA/commit/8c0762fe05ae37b59a48438dd722da9f440dab92) | Metadata addresses account for 85% of matched waits. | Strong mechanism match, but partially explored already. Our proven-fill experiment removed two of six dirty metadata reads with unchanged appearance and no large FPS win. Remaining unknown/overlapping writers still require proof. |
| 7 | Small guest BDA suballocation: [2043a692](https://github.com/TheCruZ/KytyPS5-GTA/commit/2043a692b004fb02f41cb901626d8072087b17bc) | Could reduce buffer creation overhead. | Only reviewed patch that cleanly applies. It changes guest BDA buffers up to 16 MiB; internal buffers remain dedicated. Native allocation-path off-CPU time was 79.963 ms/30 s, not evidence for a large steady-state win. Does not guarantee local VRAM placement. |

These are investigation candidates, not measured recovered frame times. Running
CPU, host wait, GPU brackets and native packet residence are different populations;
their costs cannot simply be added or converted into an FPS prediction.

## Important implementation details

### Indirect execution

Fork `CanDrawIndirectOnGpu` retains CPU fallback for host mesh/geometry stages,
unsupported topologies including rectangles/quads/patches, custom primitive
restart, special color/depth operations and unsupported index formats. The CP
also checks argument/count/stride alignment and bounds its count. An indirect
draw leaves persistent instance state; a later CPU consumer may still need a
readback. Local code also clamps index counts; its equivalence must be reviewed.

The same commit supplies a GPU conversion pass for indirect dispatches expressed
in thread dimensions. Our ordinary indirect-dispatch path already records a
Vulkan indirect command; the thread-dimension branch still CPU-loads counts.
Do not treat every indirect dispatch as currently CPU-driven.

### Transfer queue

The fork refuses the path if a relevant pending writer has not been submitted,
or device-address writes are pending. It waits on the last needed master timeline
value, copies on a transfer queue, then preserves publication ordering before
writing guest backing. Buffers use concurrent sharing across the relevant families.
It currently polls its completion fence. Queue-family availability and contention
must be measured; a separate queue is not proof of independent GPU execution.

### Threading

`e43ed2c8` adds ordered operation snapshots, command streams, SPSC handoffs and
decision-write journaling, rather than simply moving an existing function to a
worker. `45328020` adds ahead-of-execution resolution, revalidates guest reads
before using it, and falls back when they changed. The initial threading commit
touches 29 files; the later changes span memory, caches and compilation too.
The defaults include affinity, priority and spin-wait choices that need tuning
for this i7-10700F. Existing profiling transaction/tick ownership must survive a port.

### Existing overlap and candidates to defer

- Our `TryReadGpuCleanBacking` already checks exact tracked dirty ranges and
  texture ownership. The fork's clean-byte backing-read idea is not wholly new
  to this working tree. Declared write ranges are not proof of actual stores.
- The local PM4 release handler already avoids unconditional flushes for its
  label-only branches. The fork adds bounded batching within its pipeline;
  its advertised submission reduction cannot be assumed to reproduce here.
- Known-fill tracking requires invalidation on every intervening write/alias.
  It cannot safely assume a recurring metadata address still contains a constant.
- [57e5ea4b](https://github.com/TheCruZ/KytyPS5-GTA/commit/57e5ea4ba2aac3dd15d2bfe88f181f607433368c)
  creates pipelines asynchronously and skips eligible draws until ready. This
  changes transient output and primarily addresses cold-pipeline stalls. It is
  not the first experiment for our steady paused-fight slowdown.
- Ray tracing, tessellation and GTA-specific shader fixes are not demonstrated
  explanations for our current UFC 5 slowdown.

## Proposed next experiment

Start with the compiled evaluator subset: preserve dynamic reads, strict-read and
EXEC-mask behavior, keep interpreter fallback, and compare complete snapshots,
specializations, success/failure and read behavior in shadow mode. Use repeatable
off/on/off gameplay windows before concluding there is a frame-time benefit.
Do not combine the commit's guest-memory-cache changes with this first experiment.
BDA is next if measured aggregate cost warrants it; its dirty/map/registration
epochs must account for concurrent CPU writes and invalidation races.

The indirect-path follow-up remains:

1. Add observation only for native indirect eligibility at the existing argument
   consumers: operation/topology/index type, alignment, index bounds, persistent
   instance dependencies, writer/resource identity and associated wait.
2. If meaningful coverage is confirmed, port that subset behind a default-off
   option. Keep all unsupported cases on the current path; preserve barriers,
   ordering and CPU-visible state. Test eligible and fallback behavior explicitly.
3. Compare the same paused fight with visual validation and repeated off/on/off
   windows: CPU argument readbacks, host wait, submit count, presentation cadence
   and native progress. A disappearing wait is not automatically saved frame time.
4. Separately retain the planned residency/control measurement. This source
   review does not explain the 14–33 ms replay inflation or replace its evidence.

The multi-thread pipeline is a larger subsequent project, warranted if the
remaining CPU budget supports it. Avoid merging the whole fork at once: that
would combine rendering, memory, synchronization and asynchronous draw changes.

## Verification and local evidence

Read-only `git apply --check` on the actual dirty working tree: seven of eight
selected commits fail to apply; only `2043a692` passes. Applicability does not
establish correctness or performance. Saved full results:
`D:/PS5/ufc5-profiles/gta-fork-audit-20261006.json`.

Local evidence: [correlated production trace](gpuview-correlated-report-2026-10-06.md),
[readback consumers](readback-cpu-investigation-2026-10-05.md),
[earlier proven-fill experiment](performance-audit-2026-10-03.md),
[FlatSRT results](flat-srt-broad-results-2026-10-06.md), and
[replay residency](replay-residency-results-2026-10-06.md).

PID 6380 was running at the start of this review. By the final process check no
Kyty process remained. The cause of exit was not established; this review did
not suspend, stop or restart it.
