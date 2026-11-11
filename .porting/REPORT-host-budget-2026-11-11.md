# Native Metal host budget and volume footprint continuation — 2026-11-11

The user accepted an average GPU duty ceiling of90% implemented by idle gaps, retained40GiB memory, and explicitly authorized the game. This supersedes the earlier post-freeze CPU-only block. It does not authorize removing a new fault marker to retry GPU work.

## Implementation

`metalHostBudget.h` is shared by all19 native backend commit sites, including presenter, readback and replay capture. A process-global gate permits one submitted packet at a time. Each packet's commit-to-completion wall time is followed by at least ceil(busy/9) idle before the gate opens. This conservatively bounds this process's average GPU duty, not instantaneous GPU use or other applications' GPU use. No guest/shader work is dropped. Limits1..90 are accepted; values above90 fail closed.

An independent100ms monitor samples task physical footprint. The budget is min(40GiB,90%physical RAM), or a lower requested value. This is sampled termination, not an allocation reservation or a verified hard OS limit; transient overshoot is possible. A packet stalled at submission/completion for2s exits75, as do memory or Metal faults. Host termination cannot preempt a hung GPU kernel.

The launcher checks the exact verified emulator SHA before execution. The first GPU submission creates an exclusive active-session marker. Fault exits retain it and block a later game; clean Finish closes the gate, waits for an in-flight commit to finish, and removes it. `at_quick_exit` handles the emulator's normal window-loop `quick_exit(0)`, which skips destructors. The prior HB1 marker was archived and reviewed after an observed Cmd+Q, an absent process, and no fault in its log; this was a lifecycle bug, not a GPU failure/retry.

## Verification

- 12010 independent CPU duty-cycle bound/rounding/overflow cases PASS.
- CPU-only negative controls: invalid GPU limits0/91/100/text,32MiB footprint overflow, preserved prior-failure marker, and synthetic pending submission observed by independent monitor all exit75 as intended.
- 56 exact finite GPU writes over3 host threads/3 queues, API+GPU Validation: output and all inter-packet idle intervals PASS. The quick-exit variant also removed its active marker and printed56 completed packets.
- Full coherence suite rerun with EVERY staged test commit routed through the same production budget code, API+GPU Validation: PASS, including3D physical aliases, partial stores, padding, mip/depth/BC/read-only imports. Original unbudgeted test files were not executed.
- Actual c29caf0c1cb0ad94/d6a4c426ea6d4065 f386 capture, GDS count14: budgeted API+GPU-validated replay6.416ms, all74045440 written bytes match. Replay fidelity, not a Vulkan/RDNA equivalence proof.
- Old binary SHA and91% launcher policy negative controls refused before target execution.

## FPS follow-up

HB1 reached the main menu at frame369: FC NEWS, street and character visible; no large white/garbage blocks in that observed frame. HUD3.52FPS,66.60ms GPU,284.15ms interval,13.56GB App/15.10GB Metal. Window later frame414 at2FPS. These different time windows and scenes do not establish an FPS gain. No match or general artifact fix is claimed.

Main-menu CPU sample identifies repeated `TiledVolumeMipFootprint` traversal/sorting in VolumeMemoryTransfer::Initialize (multiple dispatch/draw branches). Added a separate per-thread cache of layout-relative3D footprints, keyed by all dependencies and bounded to256 entries/8MiB. Guest bytes, texture contents, writeback, physical mapping and invalidation are unchanged. Existing2D Claude cache preserved.

New CPU-only `metalVolumeFootprintCacheTests.mm`:405 shape/mip/depth cases compare cold and warm results to an independent per-texel occupancy oracle;405 invalid-layout controls ensure old cache entries are not reused. PASS.100 calls for200x113x7 R32: old compute374.751ms, cached3.906ms including a cold fill. This is a local CPU measurement, not FPS. Full budgeted GPU coherence after the cache PASS; product build PASS.

Final build SHA:5e7dac71ae0c0045f928cd3fdb444ba58f68c1d22a4491411aa23013ec340231. HB2 did not validate a final FPS gain: the guard stopped its c29/d6a4c426 compute packet after2s at Metal status3 (Scheduled),05:57:19+05. PID2855 is absent; CUA inventory confirms no game. The fault marker remains and no GPU tests were retried. Latest observed UI was frame270/fps4 during startup. This identifies a packet, not a proven shader loop or cache defect. No claim that this final build reached the clean menu observed in HB1.

Artifacts: `/Users/tx/Documents/Codex/2026-11-09/new-chat/work/host-budget-90`. Historical preceding GDS/failed-run artifacts remain in `work/metal-fps-validation`. Ledger CLI unavailable; no staging, commits, reset, or push.

## Remaining work

The c29 failing inputs were not captured in HB2. Prior exact count14 replay and divergent count23 replay do not reproduce that actual packet. CPU source/IR analysis can continue; do not cap shader loops, silently skip work, infer a root cause from its label, remove the fresh fault marker, or run independent GPU controls after a timeout. Capture/isolate the failing state before asserting a shader fix. General artifacts, shadows, match and FPS remain open.
