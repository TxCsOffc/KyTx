## c29 hang: it is the packet with c29 and its canonical IMPORTS — 2026-11-11 23:05 +05 (Claude)
- KYTY_GDS_TRIP_SPLIT_WB=1 (diagnostic): c29 is committed in its own packet, then each canonical writeback of its storage images in its own packet, with a log line per image/writeback. sw2: the hang came before "writeback 0 committing", i.e. in the packet holding c29's pre-dispatch imports + c29 (an empty "[HostBudget] packet:" label only means no op was labelled without KYTY_MTL_SPLIT_OPS; sw1's reading "the writeback hung" was wrong).
- That packet re-imports two canonical images every frame (GuestWrites says the GPU wrote them): res 0 = scene depth read as R32F, tile mode 24, 1784x1008 (its guest bytes are not depth values: see the capture analysis above) and res 2 = R32F output, both via Gather + EncodeTileTextureImport. The offline replay skips these imports (textures come from the capture), which fits "same inputs, 17 ms offline, hang in game". Next: time/split the tile-24 depth import and the res 2 import from c29 (commit+wait after imports was dd1 and still hung, so check whether that drained packet or c29's was the one), and decide what c29 must see for a depth target (Metal depth texture vs guest bytes).
HOST_GPU_TESTS_BLOCKED present from sw2 (copy in work/gds-trip).

## Image load/store bounds (RDNA semantics) — 2026-11-11 22:40 +05 (Claude)
- MslEmitter: ImageRead returns zero and ImageWrite is dropped outside the bound texture (x/y/z vs get_width/height/depth(lod), array slice vs get_array_size, lod vs get_num_mip_levels), as RDNA does; Metal left both undefined. c29 writes whole 16-pixel tiles (114x16=1824 > 1816 wide, dynamic resolution never a multiple of 16), so its edge tiles wrote past the image every frame.
- tests/metalMslTests.mm ImageBounds: 6 GPU cases (inside, x==width, y==height, x=0xffffffff, lod past last, last texel) — PASS; negative control with the old emitter FAILS (lod past last read returned 37). Full test_metal_msl.sh PASS; test_metal_pipeline.sh PASS (script now links guestWritesLink.mm).
- Game: ob1 ~4 min, no hang, no frame brighter than mean 150 in 62 samples, 4.5-12 FPS with stalls; ob2 hung after ~35 s on the FIRST c29 dispatch (new native hash 94f477b77168bdf5) right after [AliasImg] 1920x1080 RGBA16F. So the bounds fix is an accuracy fix, not the hang fix. Every first-dispatch hang so far follows a new alias image for c29; the hung packet holds c29 and the canonical writeback of its storage images. Next: separate c29 from its writebacks into different packets, or get encoder execution status for the hung buffer.
HOST_GPU_TESTS_BLOCKED from ob2 is present (archived copy in work/gds-trip).

## c29: experiments e1/e1l/e2/dd1; depth input is garbage — 2026-11-11 16:55 +05 (Claude)
- Hang reproducer: first c29 dispatch (ln3, dr1, dd1) or within a few. Drain right before the dispatch (KYTY_GDS_TRIP_DRAIN=1, dd1): hangs on the first dispatch. Snapshot of buffers only (e1: 32 ok, e1l: hang after 3) or images only (e2: hang after 28): no cure. Only the full snapshot sn2 ran 15 min at 2.9 FPS without a hang: not a proven cure (lower rate). Not data or an infinite loop: same inputs replay in 17.7 ms. Residency: every bound buffer/texture is declared with useResource on the dispatch encoder (UseResource dedup per EncoderSerial); no gap found yet.
- c29 image 0 = scene depth (T# addr 0x8246cc0000 1600x904 IMG_FORMAT_32_FLOAT, tile mode 24, read via as_type<uint4>). Captured content is not depth (29% NaN bit patterns, up to 1e35, rows of 0xffffffff): likely raw guest bytes of an HTILE-compressed / not-exported depth target instead of the depth values. Hair occlusion against it is wrong: candidate for the green/white noisy menu frames at menu entry (same moment as the first c29 dispatches and the GDS spikes). Next: check how a depth target read as a 32-bit texture is bound for compute (DepthMemoryTransfer / canonical depth export, TC-compatible HTILE).
- Diagnostics added: KYTY_GDS_TRIP_DRAIN=1, KYTY_REPLAY_SKIP_BUFFERS/IMAGES (snapshot halves).

## c29 hang narrowed; white/green menu captured — 2026-11-11 16:30 +05 (Claude)
- c29 is NOT a data-dependent infinite loop: the sn1 trip capture (GDS[0]=4337, menu entry, 114x64 tiles, max 9511 per tile) replays standalone in 17.7 ms (1/4/16/64/512/2048 items: 8.7..15 ms); image 0 replaced by 0/NaN/Inf/1: 5..17 ms. Replay tool: work/host-budget-90/replay_gds_budgeted (REPLAY_INPUT_ONLY=1, KYTY_HOST_FAILURE_FILE=scratch).
- In game it hangs (>2 s, Metal status Scheduled), often on the FIRST c29 dispatch right after its compile (ln3, dr1), also later (ms1 split ops: the hung packet is c29 alone, 144x128). Runs with KYTY_GDS_TRIP_SNAPSHOT=1 (drain + GPU-blit capture of all inputs AFTER binding/imports, right before encoding c29): sn1 3 dispatches, sn2 2305 dispatches / 15 min, no hang; a drain only BEFORE binding (plain tripwire) did not prevent it (ln3, pf1). So the hang needs c29's pre-dispatch work (image imports/alias textures, canonical blits) in the same command buffer as c29. Encoder fences checked: every main-stream encoder has OrderBegin/OrderEnd, render fences wait before vertex / signal after fragment.
- New: dynamic ring (metalBuffer.mm) keeps a replaced backing alive until the open command buffer completes (flattened SRTs are reached by raw GPU address); correct but did not stop dr1's hang (no wrap near it). KYTY_GDS_TRIP_SNAPSHOT=1 diagnostic.
- c29 image 0 (R32F view, swizzle R001, 1816x1024) holds non-depth data (29% NaN bits, up to 1e35; rows of 0xffffffff): read via as_type<uint4>, so it may be packed integers; unverified whether the import/alias is right.
- White screen = main menu overexposed, green tint, salt noise, blocky bottom-left (ms1 frames 183..236+, work/gds-trip/flash-ms1/*.png); lasts ~50+ frames, at normal speed it fades quickly. Not yet diagnosed (exposure/luminance chain suspected).
Next: drain-before-dispatch without dump to separate ordering vs first-touch; per-encoder labels + errorOptions on the hung packet; pass trace of a white menu frame.

## FPS: Metal wait-by-order for EOP labels (default on) — 2026-11-11 08:02 +05 (Claude)
Cause of ~140 serialized packets/frame: on native Metal every EOP label is published by the priority runner after its tick completes, and none were in g_pending_gpu_labels, so KYTY_WAIT_BY_ORDER never applied (wbo=0). wb4 miss counters: all unsatisfied waits had a label whose tick was ALREADY complete but not yet written by the runner -> each wait still flushed (extra commit through the host-budget gate) and spun in the retry loop.
Fix `graphicsRun.cpp` (KYTY_METAL_WBO, default on for native Metal, =0 disables): deferred EOP labels (priority and CPU-label paths) are recorded as pending; a WAIT_REG_MEM on a recorded label with the awaited value is satisfied by order from any guest queue (one GPU thread encodes all queues into one in-order Metal stream), including a completed tick the runner has not published. Before WRITE_DATA / DMA_DATA / COPY_DATA CPU-side memory ops the CP first waits for the newest elided label's tick (g_wbo_guard_tick) so they still happen after the work the wait guarded; LabelPageCpuPolled pages still suspend. [eop] line now prints wbo / wbo_guard / miss counts.
Measured same config (tripwire guard on, no pass timer), same "EA servers unavailable" scene: wb1-wb3 2.5-3.0 FPS, wbo=0, cp_wait_regmem flushes ~1800/4 s; wb5/wb6 3.9-8.5 FPS (mostly ~4, 7.5-8.5 in stretches), wbo ~1700/4 s, cp_wait_regmem flushes ~120/4 s, wbo_guard 0, frames 589 vs 1150 identical. Not measured: menu/match scenes, without the tripwire drain.
c29 intermittent hang still open (see previous entry). Garbage: the white "Press Any Button" frame (ln1 frame 435, 1 FPS) did not recur in pt1 (default build, frames 149..1740 sampled every ~5 s: from frame ~229 the game sits on the correctly drawn "EA Servers are unavailable" dialog, mean luminance 49); likely a fade/transition held on screen by the low frame rate, not proven. Menu/match not reached without input in these runs.

## c29 hang + FPS diagnosis, runs gt0..ln4 — 2026-11-11 07:35 +05 (Claude)
Commit c3e26bb5 (whole tree, branch metal-native-checkpoint) + this entry's diagnostics. All runs budgeted (90% GPU, 40 GiB), one at a time; artifacts `work/gds-trip`, logs `lean/{gt0,gt1,rw1,ln1..ln4}-*.log`.
- New opt-in diagnostics (inert without env, MSL text unchanged): KYTY_GDS_TRIP=<hash>:<dword>:<max> (+_DIR, _WATCH) drains before the GDS consumer, captures exact inputs + watched producers' bindings and exits 77 without encoding it; KYTY_RACE_WATCH=<hashes> (+KYTY_RACE_STOP) compares read-only inputs at encode vs a GPU-ordered blit; KYTY_MSL_LOOP_TRIP=<guest>:<limit> counts iterations of every structured loop of one GDS kernel and records id in GDS[16368..] (changes results only once it trips).
- c29 hang is NOT the GDS count: ln3 hung on the very first c29 dispatch with GDS[0]=25 (normal). gt1 (drain) 1815 flips no hang; ln1 300 s ok; ln2/ln3 hung (2 s guard, Mac fine); ln4 (loop counters, limit 1e6) 883 flips, no trip, no hang. Race watch (rw1): no CPU-range input differences for the chain. Root cause still open; next suspects: barrier-count divergence between waves (AMD s_barrier is a counter, Metal threadgroup_barrier per instance) and GPU forward progress. Hair chain identified: 25bda6ea (guest ddfb6d46, binning, fp16 positions; 0xFFFF NaN-filled culled strands are game data) -> 6b784196 -> e16c7aab -> 6839dab -> c29 (screen 100x57 tiles at 1600x904).
- FPS (title screen, ~1.5 FPS): KYTY_GPU_PASS_TIME sums to 1318 s GPU over 480 s wall, and c8aa3519 is 78 ms/pass in game vs 0.21 ms standalone (byte-identical) -> per-pass times include dependency waits; they do not show GPU cost. Real limiter: ~215 packets/s (~140 per frame), each waiting for the previous (host budget gate + WAIT_REG_MEM flushes, wait_priority_open ~550/s); packets in flight 46% of wall time. Fix direction: fewer CPU round trips per frame (GPU-side cross-queue waits via MTLSharedEvent, coalesced commits), not shader codegen.
- Garbage: title screen "Press Any Button" is almost entirely white (ln1 frame 435, work/gds-trip/ln1-frame-435.png); menu (HB1) looked right. Exposure chain not yet captured (ln2/ln3 hung before the dump trigger).
HOST_GPU_TESTS_BLOCKED currently absent (ln2/ln3 markers archived in work/gds-trip after review). No push.

## c29 hang: input chain traced (CPU-only, no GPU runs) — 2026-11-11 06:40 +05 (Claude)
Read Codex REPORT-host-budget. HB2 marker `.porting/work/HOST_GPU_TESTS_BLOCKED` left in place; no game/GPU run. Log forensics of fp5/fp6 (lean/fp5-045436.log, fp6-045648.log) + MSL/IR dumps (kytx-native-al2-sources):
- c29caf0c1cb0ad94 (native d6a4c426, 16x16 wave64 pair path) is a persistent work queue: count = GDS[0] (read once), each workgroup's thread 0 takes the next item by ds_inc on GDS[1], exits when item >= count. Normal frames GDS[0]=14..23 (all 17 captures). Hangs: fp5 GDS[0]=6378, fp6 2867, GDS[1]=144 both (items taken stop -> workgroups stuck inside items). c29 GPU time was a steady ~20 ms/frame before the jump: one-frame input change, not growth.
- Chain in one frame (CsBuf order, all compute, GPU-ordered): 84d47217 (guest 4fbf70b2, generic fill) clears values buf 0x8246754000 (865280 B = 6784 entries x 32 dw) -> 25bda6ea writes values -> 20135d2a = guest **6b784196fcba7b41** (32 threads, wave64 native32, the Stage-2 cross-half shader) writes one 8 B entry per workgroup: x = tile | bucket(log2 sum)<<27, y = ballot(value>0) -> 84d47217 clears bins 0x8240032000 -> e16c7aab (guest 7379fb5a) counts entries with y!=0 per bucket -> 6839dab0 (b5427be5) prefix-sums 64 bins, stores GDS[0]=total, GDS[1]=0, compacts list -> c29.
- So GDS[0]=6378 means 6378 of 6784 entries had a non-zero value that frame (capacity 6784). Not established: whether the values (25bda6ea) or 6b784196 entries were wrong, or the scene legitimately had them. No DMA/CP write involved in the last-frame chain as logged.
Next (needs user OK to run): env-gated tripwire before c29: commit+wait, read GDS[0]; above a threshold dump 25bda6ea/6b784196/e16c7aab/6839dab inputs and exit before encoding c29 (no work skipped in a continuing run, and the GPU never gets the runaway kernel). No code changed. No commits.

## Host budget installed; HB2 GPU timeout blocks retries — 2026-11-11 (Codex)
User explicitly accepted <=90% average GPU duty via packet pauses and authorized game with40GiB memory. Implemented ALL19 native commit sites through `metalHostBudget.h`: globally serialized, idle>=commit-to-completion wall time/9, memory100ms monitor,2s submission/completion watchdog, persistent exclusive launch marker, verified binary SHA. This is not instantaneous GPU control or kernel preemption; sampled memory may overshoot. Normal emulator quick_exit(0) now closes the gate/drains/cleans marker via at_quick_exit; faults retain it. Do not remove the NEW HB2 fault marker to retry GPU tests.
Checks PASS:12010 CPU arithmetic cases; negative memory/limit/prior-marker/synthetic-stall guards;56 exact GPU writes across3 queues/3 threads with idle intervals and quick-exit cleanup, API+GPU validation; full budgeted coherence after final code. Actual c29 f386 replay6.416ms/all74045440 output bytes exact. HB1 reached main menu/visible street+person at3.52HUD FPS and13.56GB App, quit Cmd+Q; no general shadow/FPS claim.
HB1 CPU profile found repeated3D footprint sorting. Added256-entry/8MiB per-thread layout-offset cache, all dependencies keyed;405 independent scalar occupancy oracle cases +405 negatives PASS.100 calls374.751ms compute vs3.906ms cache; local CPU gain, not FPS. Claude2D footprint cache and shader work preserved. Final build SHA5e7dac71ae0c0045f928cd3fdb444ba58f68c1d22a4491411aa23013ec340231 builds; full API+GPU coherence PASS.
HB2 PID2855/log lean/hb2-055618.log: ran05:56:18..05:57:19+05, latest UI frame270/fps4. Guard stopped a single cs d6a4c426ea6d4065 (guest c29caf0c1cb0ad94) packet after2s, Metal status3=Scheduled. This identifies the stalled packet, not a proven shader-loop/root cause. Process absent, CUA inventory no game, failure marker retained, no GPU probes/retries after fault. Final build menu/FPS/cache win unconfirmed. Next CPU-only work: isolate c29 actual failing inputs/control and GDS; no work/loop caps. Report `.porting/REPORT-host-budget-2026-11-11.md`, exact artifacts `work/host-budget-90`. Ledger unavailable. No commits/staging/reset/push; preserve inherited dirty tree.

## HOST FREEZE — GPU launches BLOCKED — 2026-11-11 05:21 +0500 (Codex)
User explicitly reported another Mac freeze and reiterated <=90% resource use. STOP all game/GPU tests. `tools/with_resource_budget.sh` now fails closed (exit75) while `.porting/work/HOST_GPU_TESTS_BLOCKED` exists; only `--print-limit-mib` remains available. No GPU percentage cap was enforced by the former40GiB + utility/nice policy. Do not claim one or remove the block merely to continue testing. CPU-only work is allowed. No automatic reboot/reset of user machine.
Before the report: native writer invalidation fix built and31 CPU checks/full API+GPU coherence passed; 15 c29 GDS captures complete. FP9 watchdog measured Metal status2=Committed and GPU/kernel timestamps0, not proof of a shader loop. After FP9, baseline RW NoCopy and ordinary Metal-buffer microkernels both timed out5s; the previously passing GDS fixture also timed out10s WITHOUT Metal validation. Further GPU tests cannot establish emulator correctness in that state. Latest snapshots/launch metadata/logs under `/Users/tx/Documents/Codex/2026-11-09/new-chat/work/metal-fps-validation`. Preserve Claude footprint cache, no shader-work caps, no commits. See next entry for earlier details.

## FPS continuation / GDS replay — 2026-11-11 04:58 +0500 (Codex, in progress)
Preserved Claude's footprint cache. New writer policy: `metalShaderGuestWrites.h` uses existing `CompiledShaderInfo::has_address_writes`, not `info.uses_dma`, for unknown writes; bounded V# stores/atomics still note their whole declared range. 31 guest-write checks PASS; old policy negative fails on read-only address-table loads; full API+GPU coherence PASS; build PASS. No FPS/picture win claimed: FP1 unmapped range 0x8103980018+0x80000; FP2 and FP5/6 hit c29caf0c / native d6a4c426 watchdog. White scenes remain, as before this policy edit.
Added opt-in full GDS capture in `metalReplayDump.inc` (BDA still rejected); workspace `work/metal-fps-validation/replay_gds.mm` rebinds GDS, compares all written buffers/images/GDS, host watchdog10s, supports explicit input-only replay without output claims. FP4 f386 capture (GDS0=14) replays under API+GPU validation in6.540ms with ALL 74,045,440 output bytes identical. This is native replay fidelity, NOT a Vulkan correctness comparison; paired SPIR-V is0 bytes. Synthetic GDS replay + missing/corrupt captures checked. Later FP6 f332 capture has GDS0=23 and completes; hung FP5/6 watchdogs showed GDS0=6378/2867, index144. Next: capture a short series to catch the failing inputs, replay separately, no shader-work caps. Launch wrappers unchanged, FPMAX40, one GPU job at a time, no timer kills or commits.
Artifacts: `/Users/tx/Documents/Codex/2026-11-09/new-chat/work/metal-fps-validation`. Current main SHA8d37226cf6cd800279200b589d49d16ea13ce040f6dd6165ce04350816bc5ce7 (series diagnostic rebuild in progress). Latest launch currently none; verify process inventory before UI (CUA may relaunch a dead app).

## FPS: CPU-bound GPU thread in rect footprints — 2026-11-11 04:20 +05 (Claude, in progress)

AL5 (lean/al5-040540.log): GPU idle (HUD GPU ~67 ms vs frame 513 ms). `sample` of the guest GPU thread: 95% busy, ~60% in canonical RT export `TiledMipRectFootprint` (per-element ranges for partial edge blocks + global std::sort, every pass end) and ~15% in DepthMemoryTransfer::Initialize (same function). Slow "compute" pass times were environment: c8aa351920a3b786 replays in 0.21 ms standalone, output byte-identical (work/metal-fps/replay_time.mm). Fix in metalCanonicalMemory.h: per-thread footprint cache keyed by layout/mip/layer/rect + per-block local sort (blocks are in address order). Build + coherence (rect export/mip/depth exact bytes) PASS. NOT yet measured in game; next: run, re-sample, then uses_dma unknown writes. No commit.

## Exact write tracking for canonical copies — 2026-11-11 03:40 +05 (Claude)

Menu garbage FIXED by replacing per-draw canonical re-import with write generations (metalGuestWrites.h/.inc): CPU writes via write-protect faults on all aliases + announced host writes (cooldown), GPU writes reported at encode (Scatter, stage buffers by V#, linear RTs, GDS DMA, uses_dma = unknown). AL4 (lean/al4-033129.log, PID4749 left running) main menu correct (Grounds sky, players), 0 GPU errors; empty passes 50%->1.5%. FPS still ~2: compute c29caf0c1cb0ad94 ~400 ms/dispatch (+6b5ae237, c8aa3519) — next regression to chase; uses_dma unknown writes 400-500/s. Tests: guest-writes unit 27 PASS, coherence API+GPU PASS. Build dir reconfigured with FETCHCONTENT_SOURCE_DIR_ZARCHIVE_SOURCE (non-idempotent zarchive patch). Details: .porting/FINDINGS-canonical-regression-2026-11-11.md. No commit.

## Menu garbage / FPS collapse investigation — 2026-11-11 03:15 +05 (Claude, findings only)

sd1–sd3 (Nov 10 00:19–00:48) showed The Grounds title and the street menu at 25–30 FPS; every run since dm2 (07:29) is 0.5–4 FPS, AL1/AL2 title white, AL2 menu blocky garbage. Per-draw canonical re-import (canonical-mip stage 07:10: cache only within one draw, s_image_read_epoch bumped per draw/dispatch; ends the pass, gathers, re-imports, reopens with Load -> RT before_load import + after_store export) and color-ownership making every tiled RT canonical. AL3 (KYTY_GPU_PASS_TIME, lean/al3-030038.log, PID39587 left running) title: 50% of timed GPU in empty render passes. Garbage producer not identified. Details .porting/FINDINGS-canonical-regression-2026-11-11.md. No code change in this step.

## Wave64 reductions over the never-launched half — 2026-11-11 02:55 +05 (Claude)

Native32 wave64 programs (pixel/vertex always; compute/mesh when the workgroup fits in 32 lanes) are guest waves whose lanes 32..63 never launched. `op(readlane(V, lo<32), readlane(V, hi>=32))` now uses op's identity for the high read (add/or/xor/umax 0, and/umin ~0, smin/smax, imul 1, fadd -0, fmul 1, fmin +inf, fmax -inf); lane & 31 counted the low half twice (2x add/fadd, 0 for xor). Real case: compute 6b784196fcba7b41 [32,1,1] butterfly add + readlane 31/63 + log2 stored log2(2*sum); now v50 = 0. Whole-wave permlanex16 from a never-launched source after a row_shr:8/row_xmask:8 step reads the highest launched lane of its row (else consumer identity, else own); whole-wave row_xmask butterfly reads the launched lanes of its block. Pixel shaders' cross-half reductions are umin/umax/or only, so their results are unchanged.

Tests WaveUpperHalf 20 cases (6b784196 idiom add/xor/umin/fadd compute+pixel; butterfly readlane 0/32 and scan readlane 31/63 in partial groups n=12/16/20) PASS; previous-stage emitter FAILS all three parts (1056 vs 528, 312 vs 78); block rule removed FAILS (lane 4 row 36 vs 78). Wave suite 183 cases/32,958 lanes API+GPU Validation structured+dispatcher PASS; full MSL API PASS; pipeline API+GPU PASS. Build 11dd451a30336b7512b0da4b4d0f95b56cf5c7ca720f88cd146fe01376cce052.

AL1 (stage-1 build) 41 min on the title: 4.68M lines, 0 errors/hangs; quit via menu. AL2 PID37290 02:44:26+05 log lean/al2-024425.log reached the MAIN MENU (frame 1386; no input sent by Claude, unknown if the user pressed), 0 command buffer errors/GPU Hang/state4. Menu has large blocky texture garbage: not from this stage (pixel results unchanged; 6b784196 first ran after the menu appeared); cause open, no A/B yet. Open: menu garbage A/B; native32 compute 33..63-thread groups; SPIR-V path same 2x; permlane old VDST; swizzle/bpermute absent. Report section "Stage 2" in .porting/REPORT-metal-absent-lanes-2026-11-11.md; patch work/metal-absent-lanes/wave64-upper-half.patch. No commit.

## Absent pixel-group lanes + DPP BOUND_CTRL — 2026-11-11 02:10 +05 (Claude)

Cause of the WC1–WC5 GPU Hang found: PS dec48a1373748c4f is a wave64 light-list scalarization loop (whole-wave EXEC via s_orn2_saveexec, DPP row_shr min scan, permlanex16, readlane 31/63). Apple partly covered fragment SIMD groups have lanes that never launched (filled from lane 0, e.g. mask 0000ffff); simd_shuffle from them returns 0, so the wave "minimum" became 0, no lane matched, the loop never ended. SPIR-V path already substituted (ValueEmitContext::Shuffle/LanePresent); MSL path did not.

MslEmitter native32: kyty_live_lanes(); ReadLane of a non-running lane reads the highest running lane <= target (uniform, exact scan total); WholeWaveExec() recognises x|!x / true EXEC; DPP absent source under whole-wave EXEC keeps the destination (identity result for in-place and identity-filled moves), otherwise inactive; permlane16 absent source under whole-wave EXEC reads own value, otherwise inactive. DPP16 BC=0 now suppresses the write for an FI0 inactive source (LLVM AMDGPU modifier docs), both native32 and pair64; Codex's DPP oracle updated to the ISA rule. No caps/skips/fake data.

New WavePartialGroups (scissors 13x7/10x2/6x2 + control): loop + 12 direct absent-lane cases PASS; all 5 parts FAIL on the old emitter (loop trips=64 count=0). Wave suite 163 cases/27,130 lanes API+GPU Validation PASS structured and dispatcher; full MSL API PASS; pipeline API+GPU PASS; build SHA 8d16c2ac43849309de44fa2e3b443f1b36db6dac95c8d9a7a21480e527d6d31e.

Game AL1 PID33211 01:59:17+05, normal batching, log lean/al1-015916.log: PS native 6edce3ec601dd37b ran on RT 0x8241cd0000; reached "Press Any Button" title, frame 388->415; 0 command buffer errors / 0 state4 / 0 GPU Hang in 225k lines. FPS ~0.4-1 under heavy logging: not a perf result. Background J not confirmed delivered. Run left running for the user.

Open: wave64 add-reductions across the absent upper half (readlane 63 & 31 double-counts add); permlane16 FI0/BC0 old VDST not in IR; swizzle/bpermute absent lanes; pair64 partial groups; shadows/artifacts/FPS unverified. Report .porting/REPORT-metal-absent-lanes-2026-11-11.md, workspace /Users/tx/Documents/Codex/2026-11-09/new-chat/work/metal-absent-lanes. Ledger CLI not on PATH; no staging/commit/push.

## Native EXEC/DPP + read-only tiled import — 2026-11-11 00:05 +05

Implemented scalar native-group EXECZ/VCCZ all and EXECNZ/VCCNZ any tests; normal DPP source EXEC/FI, row edges, destination row/bank/EXEC and BC; pair64 FI row-edge validity; unsupported RDNA DPP controls reject. No shader skip/fake varying/loop or work/LDS cap. General fragment wave64 remains incomplete: native32 pixel tests with wave64 descriptors are not full cross-half emulation. Existing Compile test helper overwrote the compute input union with vertex=null; fixed stage selection and reran full MSL.

API+GPU validation PASS:147 wave GPU cases/24,384 lane values, structured and dispatcher; old emitter fails the new branch and DPP tests. Full MSL API PASS. Full coherence and pipeline API+GPU PASS. Game WC3 API Validation exposed kyty_tile_import storing to a raw texture view with usage0x15 and no ShaderWrite. Implemented real writable uint-image -> raw buffer -> original view bridge, retained through completion and encoder-ordered, without changing bits or requesting invalid sRGB ShaderWrite. New18 format/layout cases,54 mip/layer round trips/3,105,792 exact bytes; actual color pass coverage60 cases/5,928,960 physical bytes includes RGBA8/BGRA8 sRGB load/store/reopen/clear/partial draws. Six render fault signposts now sit immediately before actual Metal draw calls; draw behavior unchanged.

Final product build SHA e0b1e410f1d1789c37e167ddf60082d859dc95fdcc8d20915698c12a69c8fc3f. WC5 PID29829 started2026-11-10 23:55:46+05 with API Validation, normal batching; log /Users/tx/Developer/fex-spike/runs/validation-20261025/lean/wc5-235545.log. Intro240, actual J266, black268/337 and windowfps1. GPU Hang at log46532, state4 render at46708/48883: VS native1d58ae6f8ed54b0c guest a8092251af057bb6 + PS native9543bc4cc4708443 guest dec48a1373748c4f, RECTLIST/RG11B10Float. Subsequent recovery victims/submissions ignored explain why post-fault FPS is not healthy performance. Pair attribution is not proven faulty stage/instruction. No menu/match, shadow or FPS fix established.

WC4 same SHA/API validation with split draws+ops from0 saw black through317 but no GPU/API error before closing; this alters timing/encoder boundaries, not a valid fix. WC2/WC3 threshold230 used the wrong counter (TraceWindow g_flip, not window frame); splitting was not active before those failures. WC3 natural abort preceded bridge. Old VM2 and WC1/WC2/WC4/WC5 closed normally via CUA after diagnostic observation; WC5 App quit and ps dead confirmed, NO GAME RUNNING. All runs original40GBguard/max-seconds0, no timer kills/input loops/signals, extra_env restored byte-for-byte. Check exact PID before CUA app selection, which can auto-launch a dead app.

Next: capture actual pre-fault indirect args, NGG primitive/flat payload, resource/sampler/attachment/raster state and isolate this render pair. Existing ReplayDumpPhase is compute-only; shader sources alone are not a full replay capture. Upstream shadPS4 #5665 and guarded subgroup32 follow-up reviewed as leads, NOT ported or proven to explain our hang. Full game Shader Validation still blocked by instrumentation expanding actual18432-byte static LDS to36864>32768; no caps. Separate historical Apple clip-guard CFG failure remains open/not rerun. General fragmentwave64/legacy producer interop/simultaneous aliases/compressed3D/lazy coherence remain open; old visual artifacts unverified.

Report:.porting/REPORT-metal-wave-control-2026-11-10.md. Workspace work/metal-wave-control contains captured dirty-before baselines,8 code/test hashes,12-file patch/after snapshot, full logs/game-observations/evidence hashes and exact first-fault pair. Patch verified on isolated baseline; prior stage artifacts unchanged. Ledger unavailable; inherited dirty/untracked preserved, no staging/commit/push/reset.

## Canonical 3D volumes, including linear GFX10 — 2026-11-10 19:13 +05

Final VM2 CPU binding follow-up: shader5562b272e870e82f/guest1211e65193f45e32 binds actual200x113 volumes; res1 guestR16Float13 -> MetalR16Float25 (VA8244b50000), res0/res2 RG11B10Float92. No ImageFormatReject in inspected log. This is after the prior GPU hang, so it proves binding progress only. PID19549 still live at final check; see game-vm2-observation.json for timestamp/evidence.

Replaced GuestTexture3D's VA-only/private-GPU-owned cache and size/hash-based upload skips with canonical physical load/store. Exact SRD+shape+R128 identity, per-command reimport, physical span validation, BASE..LAST writeback, thick-slab/tail footprints and 3D storage provenance/mip views. Native packed format retained; no RGBA32Float widening. VM1 exposed the old missing linear input loading; implemented GFX10 256-byte rows/reversed mip records/full-chain Z stride with raw GPU blits. No shader skips/zero substitutes/guest work or LDS caps added.

Final API+GPU-validated full coherence PASS:60 GPU volume cases,4 native R16Float/RG11B10 stores,193 negatives,54,790,656 exact bytes; same-VA64cube/RG11B10 ->200x113x7/R16, queued producer/remap/remap-back alias, CPU/raw updates/discontiguous chunks/partial writes/padding. Full tiling and pipeline passed before the subsequent volume-only linear addition; full coherence and product build rerun after it. Initial R8Uint test failure was a generator assuming truncation rather than Metal saturation; strict byte oracle retained. Emitters unchanged, full MSL/CFG not rerun; historical Apple CFG issue open. Final SHA5b4c621ae129f56f2867b1ae82b06b3ccebff6604208bb5fc9416a958dc9fa89.

VM1 PID19302 naturally aborted at missing linear3D, then implementation added. VM2 PID19549 started19:11:06+05 finalSHA, log lean/vm2-191105.log. CUA saw intro227, EA Servers259, then black312 after two deliberate J presses for those visible screens. GPU hang repeats the pre-stage pair: draw14520 RT8241cd0000 RECTLIST/RG11B10, VS1d58ae6f8ed54b0c guest a8092251af057bb6 + PSbaf34dd57c65d299 guest dec48a1373748c4f, FaultInfo state4. Failed encoder identified, faulty operation not established. Later CPU translation is not successful GPU execution. Exact pair archived. Need isolate this pair with real bindings; prior selective validation still inflates earlier static LDS, do not cap it. Old200x113/R16 game command not yet validated without preceding GPU failure.

Report:.porting/REPORT-metal-volume-memory-2026-11-10.md. Eight source hashes/baselines/validation logs/stage patch in workspace work/metal-volume-memory. Compressed3D, simultaneous aliases/private legacy producer transitions/linear2D interop/general lazy coherence remain open. No visual shadow/match/FPS claim. Original40GBguard/no timer/no loops/no signals; extra_env restored. Preserve inherited dirty/untracked; no staging/commit/push/reset. Ledger unavailable. Check PID19549 before any CUA app selection; a dead app can auto-launchc9.

## Canonical color RTs and scissor-bounded writeback — 2026-11-10 18:47 +05

Completed storage-writer physical provenance, tiled load/store for single-level color RTs, and exact CB dimensions (register0 means1x1). Canonical storage bindings carry their writeback physical spans and serial; unknown/current/private writers remain rejected. All tiled color RTs now use the canonical adapter from their first pass. Linear paths are separate. No shader skip, fake data, varying substitute, iteration/work cap or guest LDS truncation added.

An actual partial draw exposed Metal R11G11B10 NaN payload canonicalization outside scissor. Added rectangular GPU export and exact physical footprints; each encoder retains the union of possible write scissors, including16 viewports. Clears export the full attachment, no-draw load/store exports no color bytes, reopen uses fresh region objects. The strict raw-byte regression now passes without changing expected NaN payloads. This is a conservative scissor bound, not an exact per-fragment/depth-pass mask; holes inside possible write rectangles and legacy producer interop remain open.

Final full coherence and pipeline under API+GPU validation PASS. Rectangle tests:3 GPU cases/12 negatives/5,901,312 exact bytes, tails/layer XOR/discontiguous scatter/newer raw writes after gather. Color pass tests:36 cases/3,557,376 bytes, includes1x1 and actual partial draws in RGBA8/RG11B10Float/RGBA16Float. Storage proof:8 identity+13 writer controls/206,700 bytes. BC/depth/MDI/mesh/bounds and previous suites remain green. Build PASS; no emitter changes or full MSL/CFG rerun. Historical Apple CFG issue remains. SHA:d859298f01755226ae00dae88a1e5cc076a6441e19e1a9482c87c9e845a2002d.

Final SP4 PID18305 started18:41:53+05, log:/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sp4-184152.log. Naturally aborted. Remapped3840x1080 and1920x1080 reader materialization now proceeds; a prior GPU hang makes later CPU progress insufficient to validate GPU data. CUA saw black frame291, no validated3D scene or visual/FPS fix. Next explicit failure:3D compute shader guest1211e65193f45e32 res1 addr8244b10000, descriptor200x113/R16Float, cached RG11B10Float. GuestTexture3D still caches by VA only and keeps private GPU-written volumes. Next needs physical volume ownership and exact shape/format/mip identity with GPU thick-tile import/export, not a blank replacement or key-only loss of prior data.

GPU fault is reproducibly attributed to render encoder VS native1d58ae6f8ed54b0c (guest a8092251af057bb6) + PS nativebaf34dd57c65d299 (guest dec48a1373748c4f), RECTLIST/R11G11B10. SR8, SP3 and SP4 have FaultInfo state4 for this pair. This identifies the pair, not the guilty stage/operation. Exact pair saved in workspace work/metal-storage-provenance/first-fault-pair. Selective Shader Validation SP1/SP2 failed before it: exact4608-dword compute shader uses18,432 static bytes normally,36,864 with the validation layer even when pipeline disabled/default none/TG checking off. Standalone probes confirm; max-thread settings do not change it. No LDS or thread-count cap introduced; SP1/SP2 are failed diagnostics, not valid game runs.

Report:.porting/REPORT-metal-color-ownership-2026-11-10.md. Stage baselines/source hashes/patch/logs in workspace work/metal-storage-provenance; preserve prior metal-sampled-remap artifacts. Ledger unavailable, inherited dirty/untracked retained; no staging/commit/push/reset. Original40GB guard, no timer kills or input loops, env restored. No signals sent this stage. Old shadows/clothing/lighting/hair/neon/upscale remain unproven.

## Mip cache, physical remap proof and BC transfers — 2026-11-10 14:19 +05

Implemented exact mip RT cache keys (address, format, width, height, levels), replacing a colliding XOR. The GPU regression proves two wrong shapes with the old encoding; the new key preserves 174,720 bytes across three allocations and their mips. Canonical RT history now records exact physical spans and its writer serial. The shared sampled/storage binding path can import current physical bytes after a proven remap. It refuses newer detached writers, including a changed serial on the same texture pointer. A GPU mini-test covers raw GPU/CPU updates and storage shader -> tiled writeback -> separate reader in one command buffer: 206,700 bytes, 8 identity and 8 writer controls. The game still encounters an uncovered producer transition; do not call its remap issue solved.

Added exact BC1–BC7 GPU block transfers through a uint texture, buffer and native BC blit. No CPU readback, approximate decoder, recompression or sentinel overlay. Canonical sampled 2D/2D-array images use them. Writable BC descriptors explicitly require an uncompressed block SRD. Final API+GPU-validated coherence suite PASS: 56 format/tile cases, 392 negatives, 11,954,176 exact bytes, six mips/three layers/odd37x21 extents/selected2x1 tail, plus35 decoded BC1 texels. Existing alias/mip/physical/depth suites PASS. Full pipeline PASS before the final shared proof/BC additions; product rebuilt successfully afterwards. Shader emitters unchanged; full MSL/CFG not repeated. Final SHA:095a29fcb3100df3200966517b23bf74c9dd0441f5d2535c4d6a959e7bc791c1. Report: .porting/REPORT-metal-sampled-remap-2026-11-10.md. Baselines, 15-file stage patch, validation logs and SHA manifest: workspace work/metal-sampled-remap; patch applied to an isolated baseline and resulting bytes verified.

ML5 had already naturally aborted when the user unlocked the Mac. SR1/SR2 import/load failures were diagnosed further; causality from the XOR collision is unproven. SR3 proved physical remapping, SR5 a newer GPU writer. That registry includes depth/color RTs too; compute provenance is not established by pointer/serial alone. SR6 aborted on missing BC transfers, then those were implemented. SR7 PID13687 naturally aborted on the old3840x1080 lookup at VA0x810c000000. It had GPU hang/recovery errors first (log line2983); SR3 already has that class of GPU error before this stage's functional remap/BC changes. Chronology does not establish the GPU fault's cause. Translation coverage after recovery is not successful GPU execution. No validated3D scene, visual fix or FPS improvement.

Current SR8 PID13951 started14:13:49+05 with the same final SHA. Log:/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sr8-141348.log. Existing diagnostics KYTY_MTL_FAULT_INFO=1 KYTY_MTL_GPU_TIME=1 KYTY_LOG_CS_STORES=1 aim to identify the first failed encoder/writer; this is not an FPS comparison. Capture/API validation off in game. Original40GB guard, no timer kill or input loop; extra_env restored byte-for-byte. Mac automatically locked again. CUA getState.errors says locked and apps=[]; this does NOT mean the process died (ps confirmed it live). An async unlock request is pending. Check PID and CUA errors before selecting the app; selecting a dead app can auto-launch another run. One accidental c9 PID12005 after SR3 was verified and SIGTERM stopped; no other signals sent.

Next: attribute the first GPU failure using labels/signposts, then identify the newer writer in the uncovered remap transition. Keep serial/physical authority checks; no age/hash/blank fallback. Legacy coherence, within-dispatch aliases, general BC producer ownership, distinct physical writable depth/stencil planes and general PS LDS remain open. Old shadows, lighting, clothes, hair, neon and upscale artifacts are not declared fixed. Ledger CLI unavailable; inherited dirty/untracked tree retained, no staging/commit/push/reset.

## PS activation + mesh fixed depth/stencil + LDS loop predicate — 2026-11-10 08:22 +05

NativePixelShaderActive теперь следует реальным color masks/formats и DB kill/Z/mask/dual export/EXEC_ON_NOOP, а не просто ненулевому адресу PS. Прежняя отсутствующая locn0 пара guestNGG4ad8b17ce657eb68/PS7facde60b1bc62d4 оказалась depth-only проходом с выключенными PS inputs. В ML4 она реально выполнила 2048x2048 layers и3072x3072 atlas с ps=0, zwrite=1. Для Metal mesh, который на этом GPU не допускает nil fragment при rasterization, отдельный kyty_db_fixed_function сохраняет rasterized depth/stencil без shader exports/resources. Это НЕ fallback для активного неподдержанного PS: missing active function/color writes явно reject. Удалён адресный s_mip_requests: SRD не увеличивает будущую CB MAX_MIP allocation.

ML4 после550translated/0unsupported в последней печатной coverage остановился на PS62b558eacebed7a3/f33aa6d0f69eb2d1: DS_ADDTID stores0/256/512 reservation2*128dwords. Reader mask Phi(p,Phi(p,self)) эквивалентен writer p. В MslPixelLaneMemory добавлена только нормализация trivial boolean Phi; ownership/dominance/reservation/predicate proof сохранены. Shared/undefined/changing masks reject.

PASS: full pipeline(API+GPU validation),24vertex/mesh GPU activation cases/1536depth+stencil+128color pixels/UAV counts/64routes/2negatives; full coherence; full MSL(APIvalidation), LDS16GPU/negative cases2048texels; final narrow LDS structured+dispatcher(API+GPUvalidation). Build/diff checkPASS. СтарыйAppleclipguardCFGfailure не исправлен/не повторялся.

Последний ML5 PID9723 started08:20:27+05 SHA d2784ebd963ee403ec0be67c16c4ed87a93ab7029ecd0db896ba35b6c50715a0 log /Users/tx/Developer/fex-spike/runs/validation-20261025/lean/ml5-082026.log. После LDS fix запущен; CUA сообщает Mac locked, ожидается ручная разблокировка для проверки изображения. Перед CUA проверить PID, не auto-relaunch мёртвый app. ML4 до LDSabort показывал EA Servers/Let'sPlay нормально, после них чёрная загрузка; нет доказательства меню/матча или исправления старых теней. No timer kills/input loops/signals; исходный40GBguard, env restored. Ledger отсутствует; dirty tree retained, no staging/commit/push/reset. Подробно .porting/REPORT-metal-ps-activation-and-lds-2026-11-10.md; snapshots/logs/patch в workspace work/metal-mesh-link. Ни нулевого varying, ни primitive_id substitution не добавлялось.

### Предыдущие checkpoint-записи (история; актуальные результаты выше)

## Canonical depth/stencil + native bounds — 2026-11-10 07:44 +05

Реализованы D16/D32/S8 physical import/export через raw buffer bridge + guest tiling, selected layers и exact footprints, CPU/raw-buffer updates, canonical 2D arrays/BASE_ARRAY. Depth/stencil callbacks переживают reopen. Убрана фиктивная first-use clear: новые host planes импортируют guest bytes. Read/write VA допускаются при одинаковом backing; разные физические planes при записи требуют coverage и сейчас explicit reject. Pass key учитывает write addresses/protection/stencil enable. Canonical image cache сравнивает exact physical spans внутри draw, не global mapping generation; unrelated remaps не сбрасывают pipeline, actual remaps инвалидируют.

Отдельный native depth bounds Apple10/macOS26 реализован через Metal API, передаётся на каждом draw. D16 границы переведены в точные представимые depth codes: nextafter mini-cases обнаружили Metal nearest quantization (4 failures), после lowering0. Отсечение по stored depth, independent Z test, disable reset и stencil coverage проверены. Никаких shader skips/нулевыхvaryings/подстановок/лимитов не добавлено.

API+shader validation PASS: 12 depth coherence cases/9 negatives/3,172,296 bytes (в т.ч. D32 signed zero/subnormal/NaN payload/±Inf/out-of-range raw bits); 8 actual partial-instanced/read-only/clear render cases/4,202,496 bytes; 48 bounds cases/12,288 colour+stencil pixels/0fail, 10invalid +1unsupported emptyD16 contract reject. Полный pipeline+MDI+mesh+mip+старыйcoherence PASS. Build/diff/16-file manifest+patch baseline check PASS. MSL/CFG целиком не повторён; прежний Apple CFG issue остаётся.

DM1 abort при повторной материализации и потере pipeline — исправлен physical mapping identity cache. DM2 прошёл старый Depth32->RGBA8 и abort на depth bounds, затем фича реализована. Последний DM3 PID7510, started07:37:54+05, main/s3 SHA a780c6bec76236cc1e13deca9e8c1ac7c8f2244c6309c8cec897cff997a318e5, log lean/dm3-073753.log. Видел EA Servers frame614, один ручнойj закрыл конкретное сообщение, затем FC loading frame695, но очень сильные полосы/блоки. Нет доказательства меню/матча или исправления старых теней; не приписывать все артефакты последнемукоду безbaseline. После этого процесс естественно abort; ИГРЫ СЕЙЧАС НЕТ. Last coverage250translated/0unsupported, не полныйфинальныйсчётчик.

Следующий точный pipeline reject flip580: VS nativef4bf0e46487b8570 guest4ad8b17ce657eb68 mesh63v/21p, PS native61522947b13064ef guest7facde60b1bc62d4. PS требует float4 param0 user(locn0),flat, mesh outputs толькоposition + culled/primitive_id. Сохраненыmetal/IR/json в work/metal-depth-memory/next-pipeline. Не добавлять нулевой varying. Выяснить аппаратную маршрутизацию SPI_PS_INPUT_CNTL/DEFAULT_VAL/VGT_PRIMITIVEID_EN и frontend output mapping; primitiveID/default hypothesis пока НЕ доказана.

Границы: distinct physical read/write targets, MSAA/mipped depth, canonical BC, within-dispatch aliases/lazy tracking, inherited HTILE detection/clear-value heuristics иlegacyage/hash/new-image остаются. Bounds пока nativefiniteordered0..1 nonempty representableD16; otherhardware/unsupported intervals explicitreject, неwhole-drawskip. Полныйотчёт .porting/REPORT-depth-memory-and-bounds-2026-11-10.md, финальныйпатч work/metal-depth-memory/canonical-depth-and-bounds.patch (16files,dirtybeforebaseline), SHAmanifest. Старый canonical-depth-memory.patch там лишь промежуточный checkpoint. No commits/staging/push/reset, dirtytreeсохранён, Ledgerнедоступен. Все запуски40GBguard/no timer/no auto-input; extra_envпобайтновосстановлен. CUAпослеdeadDM2однаждысампереоткрылc9 PID6899; тотсамзавершилсянаbounds, егоframe124неDM2. Ничего сигналомнеубивалось. Не вызыватьCUAнаdeadapp — инструментможетперезапуститьего.

## Canonical mip memory — 2026-11-10 07:12 +05

Прочитана Claude-сессия 1234 (677b85d0-c7ab-4ad2-9f34-ef46774c2230); сохранён его uint8/16 saturation fix в мини-тесте. Реализованы GPU import/export отдельных физических mip/layer и общих tails; exact footprints; resolver соседних/раздельных физических mappings и canonical Metal chunks; gather/scatter сохраняет более новые raw writes вне footprint. Eager RT load/store и compute/graphics storage writeback подключены к encoder boundaries, включая reopen/indirect/mesh, без age/hash/нулевой подстановки в новом пути. Graphics exports одной image за проход deduplicated. Это ещё не весь lazy/coherence tracker.

API+shader validation PASS: 9 mip layouts /45 negatives /18,029,312 guest bytes +4,171,556 alias texels; physical spans1,049,088 bytes/4 negatives; 18 RT load/store/reopen/clear cases2,368,512 bytes; 2 real fragment writers/one export65,536 bytes; D16/D32 raw alias PASS; старые28 alias pairs PASS; полный pipeline+MDI+native mesh PASS. Product build/diff-check/16-file patch baseline check PASS. Полный MSL/CFG не повторён; прежний Apple CFG issue остаётся.

Последний MP5 PID5508 естественно abort, SHA1b4a34c7789fe252e6b05da47bc95c31920f8e69d74b2095115d1e2a983d5406, log lean/mp5-070708.log. ИГРЫ СЕЙЧАС НЕТ. Прежний NG5 mip alias пройден. Новый точный отказ shader a3cdaafc076823b8 res0 addr8242480000 1920x1080 tile27 via=depth-layer: Depth32Float252 выбран для RGBA8Unorm70. DepthWriterForLayeredRead допускает любой same-bpp format; legacy AliasConvert nil причина пока не установлена (возраст/геометрия возможны). Не разрешать old writers по возрасту; дальше реальный depth physical ownership/materialization + color decode, слои/HTILE/CPU updates. D16/D32 bit-staging тест прошёл, сам по себе не решает ownership.

Уточнение: прежний NG5 Metal113/guest69 — RGBA16Uint, НЕ RGBA16Float! Новый mip path сохраняет физические байты, не делает half numeric conversion. Canonical BC views сейчас explicit reject; array/3D/general CPU-write tracking и legacy age/hash/new-image пути ещё не закрыты. Старые тени/артефакты визуально исправленными не объявлять.

Отчёт .porting/REPORT-canonical-mip-2026-11-10.md; work/metal-mip-alias содержит baseline,16-file patch+SHAmanifest,логи и launch metadata. Финальный whitespace-only cleanup модуля пересобран после MP5; точный SHA каждого запуска в game-mp*.json. Все запуски один экземпляр,40GBguard/no timer/no auto-input, env побайтно восстановлен. Диагностика в ~/Developer (не /tmp, очищается reboot). Нет staging/commits/push; inherited dirty tree сохранён; Ledger отсутствует.

## Native NGG / Primitive — результат 2026-11-10, 03:05 +05

Отдельный Metal mesh-путь реализован и проверен для обычных NGG/Primitive и RECTLIST; реальный GS_ALLOC_REQ/индексы/отсечение, s2/s3, shared LDS/barriers/atomics, direct/indexed/indirect. Также доказанное scalar replacement пиксельных DS_ADDTID spill/reload, без общей private-LDS заглушки. Полный MSL API-набор и новые GPU-наборы PASS (304128 пикселей,86016 frontend/LDS/atomic значений); metadata/tessellation-only PASS. Общий CFG остаётся красным на старом Apple clip-guard ожидании. Подробности: .porting/REPORT-native-ngg-2026-11-10.md; work/metal-primitive логи/патч.

NG5 PID69036 SHA2acfc045... естественно abort на реальном missing mip alias: 0x8242d00000, RG11B10Float RT1920x1080,11mips,flip124 -> RGBA16Float read1920x1080,1mip,flip138. Shader coverage100 translated/0unsupported, оба PS-LDS пути успешно доказаны/скомпилированы. Сейчас работающей игры НЕТ. Не заявлять исправление старых теней. Следующий этап: каноническое владение диапазонами/mip export-import; не включать старые возрастные допуски, sampled hashes или пустые AliasedImageFor. Generic coherence/прочие функции всё ещё неполны; это не полная PS5 эмуляция.

NG3 PID68478 остановлен только из-за диагностического fopen в Documents; вывод перенесён в /tmp. Запускать сборки из repo cwd Developer/KyTx (cmake в Documents блокировался), затем копировать логи Codex-инструментом. Последний extra_env восстановлен,40GBguard/no timer/no input. Никаких commits/push/staging. Унаследованный dirty tree сохранён. Ledger недоступен.

## Исторический checkpoint Native Metal NGG — 2026-11-10 02:51 +05 (superseded by result above)

Latest user requires an independent implementation; do NOT copy Vulkan's ignored Primitive export or restore pass-through deletion. Implemented native Metal mesh execution: true subgroup s2 (group vertex/primitive counts + ordered ID), s3 per-wave counts, packed v0 connectivity, actual v5 indices/base vertex/v8 instance, GS_ALLOC_REQ, 9-bit indices at 10-bit spacing, null primitive culling, PrimitiveId, layer/viewport/point/clip exports, real shared LDS/barriers/integer atomics, wave64 pair packing. Native IR-only compilation now skips SPIR-V emission/validation in KYTY_MSL_ONLY. Other backends retain their own execution model.

Native RECTLIST assembly now expands guest exports into two consistently wound triangles with a derived fourth corner, not a fourth guest invocation. Preserves identical packed float bits, smooth interpolation, flat fragment parameters (with PS mapping/duplicate locations), provoking vertex, primitive ID, layer/viewport and null culling. Supports direct/indexed/indirect/multi draws using actual GPU-produced argument preparation (metalIndirect), not CPU readback or work caps. Ordinary mesh output max256; RECTLIST expanded output must also fit256. Full restart/line strip, wireframe edge flags, streamout and very large output execution remain gaps.

Pixel LDS: strict NG2 exposed real DS_ADDTID spilling (guest 0ef96a063e2bc1d3). Added separate MslPixelLaneMemory.inc proof: all live LDS operations must be scalar U32 at lane*4+constant bank separated by a full guest wave, within actual PS extra-LDS reservation; every read must have a dominating store with a sufficient predicate. Only then lower to private variables. Shared, uninitialized, dynamic, atomic or unreserved accesses remain rejected. extra_lds_size_dwords now comes from PS register (GFX10 128-dword units) and is in the static key. Do not broaden proof to arbitrary private LDS. All 10 GPU/negative controls PASS, 1024 texels. Mesh atomics also enabled for true shared mesh execution, not private emulation.

Evidence work/metal-primitive: mesh-validation.log36 connectivity/cull/count/reserved-bit cases73728texels +4 provoking/layer/viewport/flat cases32768texels +96RECTLIST cases196608texels all0fail under API+shader validation. ngg-frontend-final.log16GPU configurations86016 register/shared-LDS/counter valuesPASS (32/64 waves, list/strip/fan/line, raw index/base/instance, group tails, cross-wave LDS and atomic counter). mesh-host.log5real pipeline/draw/index/indirect GPU producer cases0fail plus17MDI etc. pipeline-full.log full suitePASS before only new PS metadata (host behavior unchanged). msl-full-api.log fullPASS before new RECT/pixel stages; msl-full-final.log CURRENTLY RUNNING final API validation; initial full shader instrumentation cancelled only own test PID65357 due compiler latency in BufferStrideSwizzle, no GPU hang. Vertex metadata PASS, tessellation-only PASS. shader_cfg_tests FAILS pre-existing Apple SPIR-V guard expectation (TestRepeatedExports expects2 outputs+ClipDistance; spirvEmitterModule.cpp explicitly reserve_clip_guard=false on Apple). No test deletion or guard reintroduction.

Game NG1 PID66050/SHA9138c2e... passed500draws then explicit unsupportedRECTLIST. NG2 PID67473/SHA9532dd... passedRECTLIST and50 shaders0unsupported, reachedflip129 then rejected PS LDSspill. Both processes aborted naturally on explicit errors, not timer-killed. NG3 launch now in progress with real PS spill proof; read launch-ng3.log/game-ng3.json after it starts. Original extra_env restored by launcherfinally,40GBguard/no timerkill/noautomaticinput. No commits/pushes/staging. Repo /Users/tx/Developer/KyTx branch metal-native-checkpoint HEAD9534b33 inherited dirty tree retained. Ledger unavailable. Backups in work/metal-strict/before; new modules/tests were not present in that baseline. Source staging/report/patch still needed after final game outcome.

Known remaining: broad generic PS LDS and graphics GDS, dynamic flattened SRT, CullDistance/SampleMask/clip-disable XY/native tessellation still explicit failures; alias new-image/coherence heuristics still incomplete. Do NOT claim all stubs/ignores removed or shadows/old artifacts fixed. Latest old street shadow is NOT a visual improvement. User wants mini tests then game, no destructive changes or worktree reset.


## Удаление заглушек Metal / отдельная Primitive-NGG реализация — 2026-11-10, В РАБОТЕ

Пользователь уточнил: тень на улице БЫЛА раньше; не считать прошлый SD3 кадр визуальным улучшением. Просит убрать все игноры/заглушки Metal. Затем прямо потребовал реализовать Primitive отдельно: Vulkan тоже отбрасывает его и не является доказательством корректности. НЕ возвращать молчаливый skip Primitive ради запуска.

Удалены shader skips/manual overrides/red/position/LUT/TAA substitutions, guessed HDR first-use clears, 8192/dispatcher iteration caps, compute group/total-group/instance-count artificial caps, generic image/sampler defaults, fake missing varyings, per-invocation graphics LDS approximation. Unsupported materialization, missing non-null image/sampler/buffer or pipeline now explicit release errors (metalUnsupported.h), not fake data. Native null descriptors use actual null handles with is_null_texture guards. Legacy Metal SPIRV-Cross null-image handling now explicitly unsupported instead of generic4x4. Real point size/layer/viewport exports,16 viewport+scissor setup and representable stencil XOR implemented.

Tests in Codexworkspace work/metal-strict: msl-final.log fullPASS; null-gpu.log25typed read/resinfo/store+26sample/gather GPUcases withvalidation PASS; shader-mini.log longloops5structured+5dispatcher and16384aux-outputtexels0fail; shader-legacy.log oldemittertruncates8193to8192(expectednegative). Pipeline-exact.log fullGPUvalidation PASS incl1728XORtexels,2048multi-viewporttexels,6strictnegativecontracts,3328depth-arraytexels,128ROdepthcases,10indirectcases(3huge-boundary snapshots only),MDI17cases+5 full32bitinstance snapshots. Shader-cache.log corrected0fail/oldcontrol2expectedfail. Earlier pipeline-final log FAILED because old negative controls expected silent skips; controls updated to require rejection, do not cite failed log asPASS. Main build-runtime/reject-ir PASS. No commits/pushes; huge inherited dirty tree preserved. Ledger unavailable.

Strict game MS1 PID62651 (SHAc4eceb9..., log lean/ms1-012108.log) andMS2 PID63289 (SHAc15202f..., log lean/ms2-014928.log) abort on firstfullscreenVSPrimitive export. Processes were not timer-killed; no currently usable strict game. Old SD3 PID60174 absent beforeMS1; not stopped bythis turn. MS2 original extra_env restore needs verify via launcher final status. Guard scripts say instance didnotstart because emulator fails within8sec. Both40GBguard/no timerkill/noinput.

MS2 captured actual IR/disassembly: guest c92d0f21cb19ba9a, native ad9839c5873915b1, stageVertex, prim6TriStrip4. Files work/metal-strict/reject-sources/<guest>-<native>.{ir,json,metal,spv}; disasm /tmp/msl_direct/disasm_c92d0f21cb19ba9a.txt. Shader GS_ALLOC_REQ(s_sendmsg9) then expPrimv0, thenpositions fromv5VertexIndex. Current COMMON frontend Vertex prologue fakes s3=(1<<28)|waveSize and leavesv0zero, soIRPrimitive source is0. Simply accepting constant0 is NOT a correct implementation. Scalar64 shiftmask63 confirmed primary AMD RDNA1 ISA; do not change shifts based on an unverified zero-primitive-count hypothesis.

Next: independent Metal Primitive/NGG execution, real allocation counts/packed connectivity/null-primitive culling, not pass-through deletion. Evaluate native Metal mesh execution (M5supports) with compute-like IR body+shared LDS and per-vertex/per-primitive outputs; prepare correct NGG vertex inputs, real draw/index args and mesh dispatches. Existing common StageMesh/SPIRV implementation is useful for data layout but its Vertex drop is not reference correctness. No Primitive/mesh code implemented in current stage yet. CullDistance, SampleMask, clip-disable XY, dynamic flattened SRT, graphics LDS/GDS, unsupported native stages now explicitly reject (some introduced to expose ignored work). Full metadata/coherence and fresh alias images are STILL incomplete; do not claim all bugs/stubs eliminated or visualfix.

Baselines for current edits: work/metal-strict/before preserves hierarchy, including initialMetal/MSL files, ShaderIR untouched yet, laterDraw.h/Emitter.h/Indirect.{h,mm}/Context.mm/PipelineCache.cpp. New files metalUnsupported.h, tests/metalStrict{Shader,Pipeline}Tests.inc; deletedunused metalNullImage.h backedup. Pipeline runner links actual product debug.cpp.o for scissor math; test Common assertion hooks throw. Tools launch_release.py currentlyMS2 with read-only shader-pair/disasm diagnostics; originalenvbefore files saved.

## Depth-array/HTILE/clip-Z этап — 2026-11-10, актуально

Реализованы реальные depth/stencil array layers и выбранные views, GPU growth preserving old slices; pass key учитывает DB_DEPTH_VIEW. HTILE mask потребляется ТОЛЬКО выбранными слоями (промежуточный SD1 ClearAll исправлен, старые строки ниже исторические). Graphics/compute читают authoritative depth array; sampled maps сохраняют lifetime. Initial/growing sampled arrays импортируют реальные tiled guest layers/mips с атомарным reject; CPU refresh/versioning существующих layered caches ещё не завершён. Z_READ_ONLY и STENCIL_READ_ONLY clear semantics согласованы с Vulkan. Native MSL теперь учитывает remap_z обычного/packed position export; cache msl-s4-clip-z. No new sanitizers/skips/dummy resources.

Tests: arrays27/5013 GPU subresource checks +27 atomic negatives PASS (blank control5013fail); read-only depth128cases0fail (legacy8fail); depth/stencil/HTILE3328 GPU texels0fail (ClearAll control384fail). Clip Z120 same-IR native/SPIR-V GPU cases/459460pixels0/0fail (legacy27174/0). Full metal_msl и metal_pipeline (Metal validation) PASS, tiling/mips/volumes PASS, product build/diff-check PASS. Pipeline/vertex clip включены в memory-stage runner; unchanged CPU suite не повторялся в этом этапе.

**SD3 PID 60174**, 2026-11-10T00:39:00.985365+05:00, main/s3 SHA 2e384a69d5de3b6571d7a39e4691da709bb8ef77a3ed87bd50a68564b1d0c65b, log /Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sd3-003900.log. Одинэмулятор,40GBguard,no timerkill,noinputscenario,capture/validationoff, extra_env восстановлен. SD2 PID59088 остановлен по verifiedownedPID передSD3. Runtime: distinctcascade1/2,reads0:2 sameparent,perlayerHTILE. SD3 frame895 street mainmenu: видимая тень игрока; сравнение сSD2 неsame-scene A/B. Frame1827 Grounds: старые blacksky/одежда/шлейфы остаются. Studio frame4482 проверен: ожидаемая тень на полу всё ещё отсутствует. На00:41:36+05:00 log0 ненулевыхunbound/arrayimport/GPUошибок. Screenshot sd3-studio-check.png. Не заявлять полное исправление теней/FPS gain.

Отчёт .porting/REPORT-shadow-depth-2026-11-10.md; исходные baselines/tests/logs в Codexworkspace work/shadow-array,shadow-depth,shadow-clip. Следующее: realVS/PS shadowmask comparisons, Studio/gameplay, nativeLayer/ViewportIndex иclipXY/clipdisable; full metadata decode/coherence/mippeddepth/mixedlayeredtargets остаются. NativeLayer export НЕ реализован только от прокидывания renderTargetArrayLength. Неткоммитов/публикаций, Ledger недоступен, prior dirty/untracked preserved.

## Реальные depth-array cascades — 2026-11-10, в работе

В sa2 runtime подтверждено: D16 2048×2048 shadow draws используют DB_DEPTH_VIEW slice1:1 и2:2. Native игнорировал slices и сводил три каскада к одной2D texture; pass compatibility тоже игнорировал depth view. Это конкретный пропуск реального shadow path, не очередная гипотеза viewport.

Реализован MetalAttachmentArray (новый header): private depth/stencil arrays, growth сGPUcopy старых layers, отдельные2D/rangedarray views, first-use clears per layer. Renderer различает depth slices/extents вpass key, пишет выбранные слои, хранит authoritative fullarray writer; graphics иcompute читают реальные выбранныеdepth layers. Static depth writer больше не исчезает по возрасту2frames, sampled maps обновляют used_flip дляeviction. HTILE clear обрабатывает все allocatedlayers, depthclear_draw толькоselectedview. RenderTargetArrayLength прокинут вMetalDraw; GPUtest проверяет2layer instanced draw иview1:2. Stencil arrays сохраняются/очищаются отдельно, STENCIL_READ_ONLY теперь блокируетexplicitclear, какVulkan.

Tests: old forced-layer0 control640 corrupted texels; исправленные depth/stencil growth/clears/views/layered draw2560 GPU texels0fail. ReadOnlyDepth128cases0fail; full MRT/stencil/indirect/multidraw test withMetalvalidation0fail. Earlier array import27arrays/5013GPUreads+27atomicnegative andtiling/mips/volume PASS. New sampled array initialdata materializes all guest layers/mips fromtiledsnapshot. PrepareLayeredImages now fails explicitly if requested materialization fails, instead of later creating a blank substitute. It bypasses CPU-array creation for authoritative GPU depth-array writers. This last boundary change builtPASS; still needs finalgame run.

**Current live sd1 PID58106**, start2026-11-10T00:01:46.804756+05:00, SHA8066484025e7ad9da78cc8efb2a0ea37aea212b247b1ef1e817a6b35421f4812, log /Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sd1-000145.log. Oneemu,40GBguard,no timerkill,noinputloop,capture/validationoff. RuntimeDepthLayer/DepthLayerRead prove sameparent includes distinct0/1/2 andread0:2. Parisstreet has staticshadows; Grounds stillblacksky. Studio after fix notyetobserved; cannotclaim alloldshadows fixed. sd1 gotonej:6 atobservedtitle (flip14057), no mainmenu confirmed; avoid inputspamming. Sa2PID55452 disappeared before sd1 launch, reason notestablished; don'tclaim we killed it. Nootheremus atsd1launch. Launch scriptfinalcandidate work/shadow-depth/launch_release.py tag sd2 addsKYTY_LOG_UNBOUND=1 to distinguish legitimate null T# from missing nonzeroresources. New main binary newerthansd1 (STENCIL_READ_ONLY clearguard, formatting, materializationerror boundary), builtPASS; do not overwrite running s3. Next: review/finish checks, stoponlyverifiedownsd1 thenlaunchsd2, observe actualscene/nonnullunbound and finalgame/screenshot. No commits/pushes, Ledgermissing, prior dirty changes preserved.

Sources/backups/logs work/shadow-array and work/shadow-depth; initialuser screenshot shadow-fix/user-current.png showsRMA studio withmissingfloor/windowshadow. Need focusedpatch/report and finalSTATE/HANDOVER update when currentstageverified. Limitations: full metadata decode/alias CPUversioning, mippeddepth, general mixed-color/depth multilayer views stillincomplete; no NaN/TAA sanitizers/skips/newdummyproductionresources.

## Продолжение исправления реальных теней — 2026-11-09 19:54 Asia/Tashkent

Пользователь повторно подтвердил, что видимые артефакты НЕ исправлены, и попросил посмотреть скриншот/продолжать. Снятый user-current.png (work/shadow-fix) показывает RMA studio: светлый пол без ожидаемой оконной/игровой тени. Не выдавать предыдущие viewport/scissor tests за исправление картинки.

Текущие изменения: LayeredImageEnsure в PrepareLayeredImages теперь импортирует реальный guest tiled snapshot всех слоёв/mips при создании/growth, вместо необоснованной пустой sampled array. Новый UploadTiledArrayMipChain в metalTiling, atomic reject before writes. 27 synthetic arrays, 5013 GPU kernel reads+raw subresource readbacks, 27 incomplete-snapshot negatives PASS; blank-control5013 failures. Existing mip18/volume30/roundtrip133/tiling58/chain8 PASS. Реальный sa1 показал import128×128×64 RG16Unorm 4 177 716 ненулевых байтов; R16Float128×128×64 source действительно0, поэтому его старый нулевой dump сам по себе не доказывает неверное содержимое именно этой texture.

Также найден и исправлен DB_DEPTH_VIEW.Z_READ_ONLY: ResolveGuestDepthControl/GuestDepthWritesEnabled в metalPipeline.h теперь общие для actual Metal DS state и depth-feedback lookup в DrawAuto/DrawIndex. Native раньше игнорировал depth_write_disable; Vulkan его учитывает. GPU negative8 corruptions; corrected128 D16/D32 cases0 failures. Full metal_pipeline test including MRT/stencil/indirect/multi-draw PASS with validation. Runner linked actual metalPassTimer.mm to repair inherited missing dependency. Product build PASS. Tests source formatted; no new sanitizers/skips/dummy production resources. Need further review initial array import vs GPU ownership/CPU refresh; full layered depth views remain unimplemented (DepthDepthView slice_start/max ignored by native allocation/pass compatibility). New RasterState logs slice and ro flags to establish actual game usage.

**Сейчас единственный экземпляр sa2 PID55452**, started19:53:49+05:00, main/s3 SHAe708af5415418923fef6ec73bac2c16a5a692d108897e829b5d4646206a22d3b, log /Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sa2-195348.log. Original40GBguard/no timer kill, capture/validation off, no input loop, extra_env restored. sa1 PID54987 and sn3 PID54466 stopped by verified owned PIDs. sa1 got one j:6 to dismiss observed EA Servers dialog. Need observe sa2 screenshot before any input; then inspect ro=1 and slice logs, actual old artifacts. Work/backups/tests in /Users/tx/Documents/Codex/2026-11-09/new-chat/work/shadow-array. Ledger missing; dirty baseline preserved, no commits/pushes.

## Актуальный этап — 2026-11-09 19:37 Asia/Tashkent

Исправлены два настоящих пропуска native Metal raster state: signed viewport `(x,y+h,w,-h)` и полный общий с Vulkan расчёт scissor для indexed/automatic draws. До исправления GPU mini-tests дают 5 328 / 222 879 ошибок, после — 0 / 0 (36 viewport cases, 256 scissor cases; 140 832 + 1 048 576 pixel checks). Product build PASS. Новые тесты подключены к tools/test_emulation_memory_stage.sh; прежний полный suite не повторён, выполнены непосредственно затронутые GPU tests.

В реальном D16 shadow atlas3072×3072 подтверждены отдельные scissor64/512/1024 на разных началах atlas. В наблюдаемых shadow draws высота viewport отрицательная и D16 negbits=-16, поэтому viewport fix и предполагаемая масштабировка depth bias не объясняют оставшиеся тени. Старые чёрное небо, шлейфы и shadow/lighting артефакты ещё видны. Не объявлять их исправленными, не добавлять sanitizers/skips/dummies. Следующий шаг: actual VS/PS depth output и HTILE/ownership сравнение; same-input compute18 kernels уже совпадали.

**Игра sn3 PID54466 оставлена работающей**, запуск19:35:59+05:00, main/s3 SHA8286f3a33a07823b86107cb252281cc1073644738b1c601aa329a35723bff74f. Log /Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sn3-193558.log. Один эмулятор, исходный40GB guard, no timerkill/input loop (один j:6 закрыл наблюдаемый EA Servers dialog), capture/validation off, optional deduplicated raster log on; extra_env восстановлен. На19:37:48 лог0 GPU fault/command-buffer/progress-timeout/fatal/skipped-compute patterns; кадр3070 подтверждает The Grounds menu, не gameplay/fullmatch proof. Перед запуском проверить PID. Старые sh3 PID52447, sv1 PID52963 sn1 PID53497 и sn2 PID54116 остановлены нами по проверенным PID, это исторические процессы.

Отчёт: .porting/REPORT-shadow-raster-2026-11-09.md. Work/tests/negative controls в /Users/tx/Documents/Codex/2026-11-09/new-chat/work/shadow-fix. No commits/pushes, dirty baseline сохранён, Ledger отсутствует.

## Актуальный этап — 2026-11-09T19:02:33+05:00

Итоговый smoke проверен 2026-11-09T19:03:38+05:00: sh3 PID52447 жив; screenshot outputs/emulation-shadow-and-forms-game.png показывает меню подготовки матча Man Utd—Liverpool, Start Match, frame2385. Это не gameplay/full-match proof. В проверенном логе0 GPUfault/command-buffer/progress-timeout ошибок и0 skippedCS; последняя coverage1000translated/0unsupported. Пользователь/контроллер мог перейти вKickOff; нашdriver не посылаетinput. Игра оставлена открытой с исходным40GBguard, безtimerkill иdiagnosticlayers. Старые визуальныеартефакты не объявленыисправленными.

Реализованы ordinary signed6-bit offsets и4 RDNA packed arithmetic ops+SAD CLAMP.104103 результата на каждом GPU shader path PASS; полный memory/Metal stage PASS. Новейший проверенный AnyPS5 snapshot3d7f3478719d22979f2f6582d254d78770a5d201. Полный отчёт: .porting/REPORT-shadow-and-forms-2026-11-09.md. Следующая очередь: .porting/HANDOFF-shadow-and-forms-2026-11-09.md.

Реальный frame505 изsh2:18nonzero kernels exact BEFORE/AFTER replay — nativeMSL,SPIRV→Metal и captured image outputs byte-identical. Counters match, append order multiset. Проверены реальные writes, не только unchanged outputs. Ранние mismatches были ошибкой standalone replay(неиспользуемыйSRT atIndex160), исправлены/invalidated; не переносить их вproduction. Совпадение backend наsameIR не доказывает correct hostresources/VS/PS/commonIR. Старые тени/чёрное небо/одежда остаются. Пользователь заметил улучшения логотипов/бород и просит продолжать полноценные реализации, AnyPS5 как дополнительный reference.

Играsh3 PID52447 стартовала2026-11-09T19:00:11.917785+05:00, main/s3 SHA5a9ade555169b2bd2c247aab9dd05a2d41816a3594b1d28c9acc8ebc353c67dc; log/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sh3-190010.log. Единственный экземпляр, diagnostic layers off, no auto-input,40GBguard,no timerkill,extra_env восстановлен. Проверить меню/текущийPID перед новым запуском. Подробные sources/tests/rawдumps/scripts в /Users/tx/Documents/Codex/2026-11-09/new-chat/work/shadow-stage. Неткоммитов, нетновыхsanitizers/skips/dummyресурсов. Ledger отсутствует. Dirtybaseline сохранён.

## Актуальный итог — 2026-11-09 13:43 Asia/Tashkent

Отчёт: `.porting/REPORT-sampler-emulation-2026-11-09.md`. Следующая очередь: `.porting/HANDOFF-samplers-2026-11-09.md`. Ветка metal-native-checkpoint, HEAD9534b33718bd10104a4baf708758d91f61e2ef8d. Все прежние dirty/untracked изменения сохранены, коммитов нет, Ledger CLI/store отсутствуют.

Main/s3 SHA256 **68ff21a1318b797a286748670917c7d123490633bf5943fe08a009b3d4f0c529**. Latest upstream fetch: **e1292528ffb6a71b758f58373c31602b25b483d4**. Image emitter/sampler semantics не изменились относительно16a7bbe3; новые четыре коммита отмечены в хендоффе, не переносились оптом.

Завершён этап Compare sampler variants, reference-first PCF и sampler ABI, нативных Metal depth operations для совместимых2D formats, Dref scalar IR result, physical/view mip split и старших tail views BASE=LAST7/8 → реальные1×1 texels. Cache учитывает physical levels и first level. Dynamic sample offsets присутствуют в SPIR-V; Metal target contract проходит через emitter/optimizer/validator, без отключения остальных проверок. Fallback Metal function key включает фактический SPIR-V content hash. Sp3 fatal gate исправлен; его нельзя считать успешным прогоном.

Проверки:2496 same-IR GPU cases через production SPIR-V optimizer/SPIRV-Cross/Metal совпали с direct MSL;4360 isotropic+128 anisotropic cases,2352 независимые native depth cross-checks; fragment4096 components PASS.23 SPIR-V modules проходят validator с конкретным Offset feature; четыре dynamic sample modules ожидаемо отклоняются strict Vulkan validator без feature. CPU materialization/tracking/optimizer suites PASS. Общий memory/Metal/MSL suite PASS; после последних tail/target изменений повторены затронутые tests и main build. Mip views18 GPU readbacks, negative truncated layout98285bytes, tiling58/volumes30/roundtrips133/guest chains8 PASS. Focused patch28 files от dirty baseline, reverse/diff checks PASS. Артефакты `/Users/tx/Documents/Codex/2026-11-09/new-chat/outputs/sampler-emulation-*`.

**Игра оставлена работающей: sp4 PID39864**, запуск13:30:47; десятиминутный сценарий завершён без kill, оригинальный extra_env побайтно восстановлен. Один эмулятор, исходный40GB guard сохранён. До нового запуска проверить текущий PID; прошлые PID/строки ниже — исторические. Log `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sp4-133046.log`. Матч MUN—LIV прогрессировал02:50→24:53, счёт1:0 на финальном screenshot sp4-left-running.png (frame14855); старые одежда/вытянутая геометрия видны на угловом.0 ненулевых unbound descriptors,0 GPU/fatal/timeout ошибок в проверенном логе. Реальный first7/8 путь выполняется, прежде падавший guestc468... достигает binding.

Старые дефекты одежды и тёмные кадры остаются; пользователь заранее подтвердил, что они существовали до текущей работы. Не приписывать им новую регрессию без A/B. Полное исправление теней/coherence и FPS gain не заявлены. Никаких новых NaN/TAA sanitizers, shader skips или fake textures. Далее: `_cl`/MinLod, explicit-LOD gather, independent Z/custom border/software anisotropy, D16/A16/cube и несколько повторяющихся1×1 mips в одном view; затем общий owner/version и compressed metadata.

## Продолжение Claude, реальные upstream-исправления — 2026-11-09, Codex

Актуальный отчёт: `.porting/REPORT-claude-continuation-2026-11-09.md`. Прочитана та же сессия Claude `52cdae17-1a05-40ea-912d-91012d435db7`, продолжена её последняя задача: осмысленно переносить реализации upstream с адаптацией под Metal/FEX, без заглушек. Сохранены его Wait8/16, clear decoder, footprint/SurfaceCoherence tests и остальные dirty изменения. Антигравити-сканы гостевой памяти в PM4 не возвращать.

Новый код: настоящий mlock/VirtualLock с проверкой гостевых диапазонов; исправлены libc NID/ABI и реализован strcpy_s; independent FILE handle coherence; настоящий fsync/POSIX export/directory handles; rmdir ошибки и mount-root/symlink protection; атомарный rename без удаления destination при ошибке; ES/LS RSRC1 сохраняются и декодируются вместо игнора; TLS stubs/SafeCall сохраняют FLAGS через LEA; noinline logging для guest fibers. Full GS/HS/float-mode backend и празднования этим пакетом не реализованы.

`bash tools/test_upstream_ports.sh` PASS: build main + port-contracts через FEX + реальное page wiring + SyncOnAddress + 12 executable TLS cases + полный memory/Metal/MSL stage. Old TLS SUB/ADD negative control FAIL ожидаемо. Полный старый default FS harness на ARM64 не заявлен прошедшим; Windows не проверен. `git diff --check`/reverse patch check PASS.

Cu2 завершил десятиминутный сценарий без остановки эмулятора. Матч Manchester United — Liverpool подтверждён несколькими кадрами (игровое время 02:52 → 20:02 → 39:51, финальный кадр13565); в просмотренных игровых кадрах Metal HUD примерно 22–27 FPS, с отдельными просадками. GPU fault/command-buffer errors/progress timeout/сигналов падения в проверенном журнале не найдено. На 2026-11-09T06:37:41.867289+05:00 игра оставлена работающей, PID22969; auto-input сценарий завершён, исходный extra_env_s1.sh побайтно восстановлен. Монитор исходного лимита40GB остаётся. Последний screenshot: `/Users/tx/Developer/KyTx/.porting/work/out/cu2/shot-0600.png`; доставленная копия `claude-continuation-match.png`. Лог: `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/cu2-062735.log`. Это smoke menu→match, не полный матч и не доказательство визуальной корректности.

Сборка main/s3 SHA256 `2d8593610d3a6352218a55229f31ecbf615d6b440d0798a70ca23077b06f7e56`. Cu1 остановил наш ошибочно заниженный 26GB guard перед матчем; это не GPU fault. Cu2 использует исходный лимит40GB Claude. Старые блоки освещения сохраняются и не приписываются новым 3D изменениям. Не заявлять визуальное исправление/FPS gain.

Далее: runtime owner/version/CPU-write/remap integration без PM4 memory scans; shader resource/binding перенос вместе с MSL; DCC/CMASK/HTILE; настоящий unbound 64x64 RGBA32UInt tile5; неизвестный импорт WxLcN76q19o; depth compare/PCF (opt-in тест Claude 13/360 FAIL); GS/HS. Не переносить выдуманный entitlement-state вместо реализации. Полного merge 904 upstream commits нет. Коммитов/публикаций нет, Ledger CLI/store отсутствуют. Focused patch и отчёт доставлены в Codex workspace `outputs/claude-continuation-*`.

## Хендофф для продолжения эмуляции — 2026-11-09

**Точка входа: `.porting/HANDOFF-codex-2026-11-09-emulation.md`.** Пользователь попросил записать хендофф следующему агенту. Реализацию в этом этапе не продолжали, исходники/бинарные сборки не меняли, игру не перезапускали.

При проверке 03:09 Asia/Tashkent эмуляторов `kyty_emulator* --game` не найдено; прежний PID97150 отсутствует. Ранее игру оставили пользователю. В этапе хендоффа её не останавливали; причина/код завершения неизвестны. Прежние строки «игра оставлена работающей» описывают момент прошлого отчёта и не являются текущим статусом процесса. Main/s3 SHA256 остаётся `8023ff7020033d9aeca3380533c208578d7c0cbeb9d42248a4725672577613e7`.

Следующая работа: production adapter physical backing/subranges/owner/version, raw↔texture transitions, CPU dirty/remap/lifetime и PM4 visibility/completion. Ядро MemoryCoherence и мини-тесты готовы; интеграция всех runtime caches НЕ завершена. Затем DCC/CMASK/HTILE и остальные пункты очереди. Каждый этап сначала проверять мини-программами, после — игра; запуск в конце уже разрешён пользователем. Не возвращать TAA/NaN-заглушки, не приписывать исходные артефакты новым 3D изменениям. Предыдущие dirty/untracked files сохранить, коммитов нет, Ledger отсутствует.

## Runtime memory и запуск игры — 2026-11-09, Codex

Новая команда пользователя: «продолжай и в конце запускай». Автономные проверки выполнены ДО запуска. Игра запущена 02:55:07 Asia/Tashkent и **оставлена работающей**, PID **97150**, slot s3, tag `rm1-20261109-025507`. Не запускать второй эмулятор; перед новой сборкой/запуском учитывать уже открытый процесс. Новая сборка SHA256 `8023ff7020033d9aeca3380533c208578d7c0cbeb9d42248a4725672577613e7`; extra_env_s1.sh побайтно восстановлен.

Закрыты два рабочих дефекта: AliasConvert теперь берёт настоящую гостевую базу вместо zero-fill, сохраняет padding/reader-only bytes и обновляется между draw/dispatch через command epoch; все его staging blits получают Order fences. Обычные linear GuestTexture views используют canonical buffer manager и проверяют физическую identity/offset/extent при reuse/remap. Helper EncodeTileAlias доступен production и мини-тестам.

28 layout/format pairs: 497233 writer / 2189447 guest-only reader bytes, полный scratch и texels, guards, invalid ranges, повторная конверсия после CPU byte update PASS; старый zero-fill negative control FAIL ожидаемо. Metal API Validation PASS; remap/span view test PASS; полный memory stage (CPU/PM4/tiling/MSL) PASS; build/diff-check PASS. Игра дошла до 3D-персонажа, кадр5785 просмотрен. По журналу выполняются Surface conversions, GPU fault/timeout не найдено. После EA servers unavailable нажат штатный confirm; больше выборы меню не делались.

**Полная coherence не завершена:** глобальный owner/version tracker ещё не подключён ко всем runtime owners и CPU dirty/remap events. Не приписывать старые игровые артефакты нашим изменениям; они сохраняются, визуальный A/B/полный матч не проверены, FPS gain не заявлен. Новых NaN/TAA-заглушек/skip shader нет, но прежние unsupported/fallback paths в эмуляторе остаются и видны в логе. Далее — согласование private writer footprint с raw guest writes, runtime 3D/metadata и оставшаяся очередь аудита.

Подробности `.porting/REPORT-runtime-memory-2026-11-09.md`. Лог игры `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/rm1-20261109-025507-025507.log`. Коммитов/публикаций нет, старые dirty hunks сохранены, Ledger отсутствует.

## PM4 и ядро coherence — 2026-11-09, продолжение Codex

Текущая команда пользователя: завершать части эмуляции и сначала проверять их мини-программами; игру запускать позже. **В этом этапе игру не запускали.** Известные мозаика/блоки были до наших 3D изменений по уточнению пользователя; не считать их нашей регрессией без доказательства. Никаких NaN/TAA-заглушек.

В production добавлены декодирование acquire (legacy/GFX10/AGC), dispatch аппаратного RELEASE_MEM и GPU execution boundary Metal, закрывающий overlap group без CPU wait/submit. Ядро MemoryCoherence хранит физические диапазоны/owners/versions и предотвращает stale read publication/dirty eviction; **ещё не подключено ко всем runtime texture caches/CPU writes/remap. Этап полной coherence НЕ закрыт.** Следующая работа — runtime adapters, включая raw buffers и backing chunks; затем metadata DCC/CMASK/HTILE.

PM4 266240 GCR combinations PASS; byte-oracle 20000 operations PASS и ASan/UBSan PASS; GPU CPU→GPU→другой tiled GPU→CPU, physical aliases/partial writes/padding/completion label PASS, Metal API Validation PASS. Два реальных overlap producers + barrier + consumer PASS; no-op barrier negative control FAIL ожидаемо. Две разные no-copy обёртки физического alias показали stale GPU bytes, общий canonical backing PASS — не путать ordering с physical identity. Прежние views/tiling/MSL suites PASS; build kyty_emulator PASS. Общий запуск только мини-программ: `bash tools/test_emulation_memory_stage.sh`.

Подробности/границы: `.porting/REPORT-memory-core-2026-11-09.md`. Production EOP/interrupt end-to-end, все texture adapters и game validation ещё не проверены этим набором. Новая сборка находится в `_Build/macos-arm64/kyty_emulator`; игровые launch slots/extra_env этим этапом не менялись. Коммитов нет, старые dirty hunks сохранены. Ledger CLI/store отсутствуют.

> Журнал текущей работы (WP0..WP9): [.porting/WORKLOG-anomalies-2026-11-08.md](.porting/WORKLOG-anomalies-2026-11-08.md)

## Очередь реализаций — 2026-11-09

Новый указатель: [.porting/AUDIT-emulation-coverage-2026-11-09.md](.porting/AUDIT-emulation-coverage-2026-11-09.md). Первый пакет: stride-swizzled buffer MSL, GPU 3D tiling/mips, полный upload 3D mip-chain, lookup tables >4 КиБ. GPU suites/build PASS, финальный smoke ec2 дошёл до демо-матча; визуальное исправление/FPS не установлены. Далее PM4/coherence, затем DCC/CMASK/HTILE; никакой защиты TAA и пропусков вместо реализации. Коммитов нет.

## Продолжение Codex — 2026-11-09: память Apple Silicon

Пользователь отверг защиту TAA и любые заглушки; новый sanitizer полностью удалён из исходников и бинарей. Итоговая правка — physical backing identity для buffer-backed RT/UMA views, с отдельным GPU regression remap/read/write. Подробности и границы доказательств: [.porting/DESIGN-memory-apple-2026-11-09.md](.porting/DESIGN-memory-apple-2026-11-09.md). Это не завершённая переделка памяти и не доказательство исправления всех визуальных ошибок. Старые dirty hunks сохранены, коммитов нет.

## Актуальный handoff — 2026-11-08, вечер (Claude): неон/пересвет/NaN в Metal (заменяет указатель про Vulkan-референс ниже)

**Сначала читать [.porting/HANDOFF-claude-2026-11-08-anomalies-nan-neon.md](.porting/HANDOFF-claude-2026-11-08-anomalies-nan-neon.md).** Фикс освещения (RECTLIST front_facing, `msl-s3`) готов, не закоммичен. Неоновые полосы и белые силуэты = NaN/inf в f92-поверхностях второго выхода освещения, которые читает TAA; гостевое ядро `98ad4fa4` пишет эти поверхности сырым буфером, а Metal читает приватный storage-образ. Доказательства: `.porting/artifacts/2026-11-08-play1/`. Скрипты: `.porting/work/nan_audit.py`, `lighting_counts.py`, `idle_repro.sh`. План: `.porting/PLAN-anomalies-2026-11-08.md`. Стабилизация Vulkan-референса отменена. GPU-трейсы (.gputrace) запрещены: 25-40 ГБ, забили диск.

## Актуальный handoff — 2026-11-08 (Claude): референс Vulkan (белый экран) и освещение Metal

**Сначала читать [.porting/HANDOFF-claude-2026-11-08-vulkan-reference-and-lighting.md](.porting/HANDOFF-claude-2026-11-08-vulkan-reference-and-lighting.md).** Решение пользователя: чинить Vulkan-референс (NaN/белый экран), потому что он рисует тени и освещение правильно, и сравнивать с ним Metal на одной и той же сцене. Закоммичено: `945d467d` (таблицы переходов в диспетчере MSL: кожа сквозь одежду), `9534b337` (презентер: билинейно + drawable по пикселям окна). Запусков без слова пользователя не делать.

## Актуальный handoff — 2026-11-05, ночь (Claude): падения, глубина, динамическое разрешение

**Сначала читать [.porting/porting-handoff-fc27-metal-2026-11-05-depth-drs.md](.porting/porting-handoff-fc27-metal-2026-11-05-depth-drs.md).** BC-блоки больше не пишутся в память игры, исправлены очистка глубины через HTILE и чтение глубины в проходе тумана (оранжевая полоса с силуэтами). Полное разрешение `KYTY_EOP_CLOCK_SCALE=8` теперь по умолчанию (`1` выключает); бывают плохие прогоны с пересветом. Значение по умолчанию ещё не закоммичено (после `4108d699`).

## Актуальный handoff — 2026-11-05 (Claude): mip-уровни формы и BC1/BC4

**Сначала читать [.porting/porting-handoff-fc27-metal-2026-11-05-bc-mips.md](.porting/porting-handoff-fc27-metal-2026-11-05-bc-mips.md).** Форма намного чище, но на торсе Реала местами просвечивает фон. Исправлены пустые mip-уровни RT (ядро `6b86…` писало их в alias), экспорт уровней по правильным смещениям и экспорт 8-байтных блоков BC1/BC4 (`RGBA16Uint`). Кадров с полосами стало 1/31, 1/23 и 1/15 против 8/34 в um240, FPS меню 30. Открыто: красная пелена (um249), чёрный герб Ромы (um250), просвечивание на торсе, HDR-цепочки под `KYTY_RT_MIP_HDR=1`. Матч этой сборкой не запускался. Ничего не закоммичено; эмулятор не запущен.

## Актуальный handoff — 2026-11-01 (Claude): wave64 включён по умолчанию, где ломается освещение

**Обновление 2026-11-02 (Opus, день):** матч 18–21 → 30–37 FPS: опрос файла-триггера трассы на каждый draw, перестройка таблицы страниц BDA по страницам, повторный хэш 3D-LUT — убраны из потока GPU (теперь он ~50% простаивает; предел — Job-потоки игры в FEX). Сверка с Vulkan-путём: в Metal не выставлялись blend constants и depth bias, выбрасывались clip distances (перенесено); нулевые T# заменялись белой текстурой (`KYTY_UNBOUND_ZERO=1` — нули, как AMD/Vulkan, opt-in). `run1.sh` лимит памяти — `FPMAX` (26 ГБ). Детали — в конце handoff-файла.

**Обновление 2026-11-02 (Opus, ночь):** пересвет/бирюзовые ореолы — насыщенные (65024) тексели в HDR-цели RT3 G-buffer `0x8245620000` ещё до проходов кадра (верх неба), bloom разносит их; цель не инициализируется, проход неба фактически не пишет (`KYTY_CLEAR_HDR_FIRST` → чёрное небо). Новые инструменты: `.porting/work/mt_trace.sh` (трасса по детектору белого кадра), `KYTY_PASS_TRACE_SAT`, `KYTY_PASS_TRACE_INPUTS=size:WxH|all`. Исправлено: копии гостевых текстур в Metal никогда не обновлялись после записи CPU (защиты страниц нет) — теперь сверка хэша раз в флип (compute — на каждый диспатч), `KYTY_GUEST_TEX_NO_REFRESH=1` отключает. Логотип FC27 (BC3 от ядра копирования `e5941d54`) — не решено. Детали — в конце handoff-файла.

**Обновление 2026-11-01 (Opus, поздний вечер):** «шипы» от игроков и растянутые объекты исправлены — буферы draw'ов обрезались до одной страницы гостевой памяти (`max_byte_extent`), матрицы костей читались нулями; утечка alias-текстур (до 20 ГБ) исправлена; DoF-оверлей больше не декодирует чужие байты. Остаются пурпурно-белые засветки bloom в части кадров. Детали — в конце handoff-файла.

**Обновление 2026-11-01 (Opus, вечер):** меню больше не белое — видны 3D-сцены (стадион, улицы, игрок) при 27–30 FPS. Исправлено: GDS-операции в MSL (были в LDS), storage-образ пишет в объект последнего GPU-писателя того же формата, предохранитель циклов MSL — счётчик на каждый вход в цикл (старый обнулял BRDF-LUT). Детали и что осталось — в конце того же handoff-файла (разделы «Дополнение 2026-11-01 (Opus…)»).

**Читать [.porting/porting-handoff-fc27-metal-2026-11-01-claude.md](.porting/porting-handoff-fc27-metal-2026-11-01-claude.md).** `KYTY_MSL_WAVE64` теперь включён по умолчанию (`=0` выключает): ~25 запусков подряд без падений, картинка без шума/полос. Текущий вид меню: чистая геометрия, небо/дымка белые (насыщение 64512), поверхности без освещения. Освещённая HDR-цель стартует с мусором (никто её в кадре не инициализирует), входы прохода освещения берутся из чужих объектов реестров (R16F прошлого флипа вместо RG11B10, 1024x512 вместо 1600x904). UMA по-прежнему opt-in и хуже. Предыдущие записи ниже.

## Предыдущий handoff — 2026-10-28 (Claude): зависание UMA+wave64 сдержано, причина — затёртые аргументы indirect dispatch (uk1–uk19)

**Читать [.porting/porting-handoff-fc27-metal-2026-10-28-claude-hang-hunt.md](.porting/porting-handoff-fc27-metal-2026-10-28-claude-hang-hunt.md) вместе с follow-up'ами Codex (ci3/ci4, MDI, dispatch-indirect).** Зависание на флипе ~1585 вызывал indirect compute-dispatch с аргументами, затёртыми белыми пикселями (`0xFFFFFFFF×3`, `1.0f×3`): хост зажимал их до 65535³ групп. Добавлен предохранитель (`KYTY_MAX_INDIRECT_TOTAL_GROUPS`, по умолчанию 4Mi групп; больше — 0 групп), 3 новых GPU-теста проходят; без него 3 из 4 прогонов зависли, с ним 3 из 3 чистые (uk15–17). **КОРЕНЬ НАЙДЕН (uk29–uk38): EOP-метки в Metal-ветке писались на CPU сразу при разборе пакета (`[eop] immediate=30458 gpu_labels=0`) — игра переиспользовала память, пока Metal ещё читал её; теперь метки отложены по умолчанию (`KYTY_EOP_DEFERRED=0` отключает), с этим 0 срабатываний предохранителя в 3 прогонах из 3 против 4 из 5 в базе.** uk38: 420 с, флип 12535, 0 срабатываний. **NaN:** в MSL перенесена семантика RDNA из SPIR-V-пути (сброс денормалов, float→int, med3; тесты проходят), источник NaN сужен до `cs 150f446cffca5aa2` (673 228 NaN за кадр при чистых входах, MSL в `artifacts/2026-10-28/dispatch-indirect/ci4-msl`), эффект не доказан — см. раздел «NaN в освещении» в handoff. **Картинку и NaN в PS `944430…` исправление меток не чинит** (uk39/uk40: NaN остаётся, кадры пере-экспонированы) — следующая задача, не связанная с зависанием. Ниже — прежние выводы (про порядок encoder'ов), частично опровергнутые. Старое: Механизм найден: Metal выполняет encoder'ы над одной гостевой памятью **не по порядку** (разные MTLBuffer над перекрывающимися страницами; без UMA то же самое: uk27 — 333 срабатывания; с `KYTY_SYNC_ENCODERS=1` — 0). Написан `KYTY_ENCODER_FENCES=1` (`metalOrder.h`, GPU-fence между encoder'ами; тесты проходят, **в игре не проверен** — первый шаг следующего раза). Full HD: окно по умолчанию 1920×1080, `KYTY_EOP_CLOCK_FREEZE=1` в `extra_env.sh`. **`KYTY_LOOP_CAP=256` портит аргументы сам — не использовать (дефолт 4096).** Прежние выводы про «idx=1×9» (uk5–uk9) недействительны. Правило guest-pitch **не убрало** NaN в PS `944430…` (есть и с UMA, и без); UMA-картинка испорчена (`artifacts/2026-10-27/uk18/uk18-3259.png`), без UMA меню чистое (`uk19-3221.png`) — UMA оставить выключенным. Вотчдог теперь печатает снимок indirect-аргументов. Прогоны нагружают Mac, пользователь разрешил их 2026-10-28; не закрывать окно эмулятора раньше вотчдога (8 с). Ложное срабатывание `run1.sh` на командах с текстом `kyty_emulator… --game`. Ничего не закоммичено, эмуляторы остановлены.

## Актуальное продолжение — 2026-10-28: ci3/ci4, чёрный экран не исправлен

Сначала читать [ci3/ci4 follow-up](/Users/tx/Developer/KyTx/.porting/porting-followup-fc27-metal-2026-10-28-ci3-ci4.md). ci3 доказал indirect instanceCount3562971914; добавлен GPU safeguard какCPU (tests3fail→0). Bounded replay сохранённогоproducer доказывает, что cap256 может обрывать validcopyиоставлятьargsмусором; вернутьобычный4096. Отдельноразрешённыйci4доflip3200/240sбезwatchdog, instance178валиден, **картинка всёещёчёрная/белоечёрныеблоки**.17pixel+5boundaryGPUtestsпроходят. CSBUF-afterвci4былнесинхронизированпослеSplit (WaitUntilIdle сnilCB=noop), теперьявныйqueuefence. Подготовленplainci5(one-framecaptureпослеLLDBsetup) иmutex-safecapturecode; сборкаготова, **новогоOKнаci5ещёнет**. Игры/LLDBостановлены, activeenvci4. БезVulkan/Rosetta;dirtyсохранён,коммитовнет;30+gameplayнеподтверждены.

## Актуальное продолжение Codex — 2026-10-28: hang в draw, исправлен indirect draw

Сначала читать [MDI follow-up](/Users/tx/Developer/KyTx/.porting/porting-followup-fc27-metal-2026-10-28-mdi.md). Один разрешённый codex_ci2 завершён watchdog: отдельный CB с draw_idx3266 VS4b2fb528/PS74014bc2, idx1. **idx1 может быть placeholder MDI, это не доказательство пустого direct draw.** Сохранены251MSL/все4подозрительных VS/PS. Исправлены CPU-чтение GPU count до native multi routing, CPU args в single indirect, GPU bounds indexed args, потеря остальных MDI draws при split, misleading empty-draw skip/labels. Production GPU tests + MSL regression/build; runtime этих новых изменений ещё не было. codex_ci3 с GPU argument trace только подготовлен, новое разрешение запрошено. Процессы игры/replayer остановлены. Меню/матч30+FPS не подтверждены; dirty changes сохранены.

## Продолжение Codex — 2026-10-28: compute-indirect исправлен, hang воспроизведён без UMA

Читать [follow-up](/Users/tx/Developer/KyTx/.porting/porting-followup-fc27-metal-2026-10-28-dispatch-indirect.md). Native compute-indirect использует текущие CS/resources/local size и GPU snapshot args (7GPU cases/0fail, MSL regression и build проходят). Один явно разрешённый codex_ci1 (wave64 ON/UMA OFF, run1/20GB/240s/watchdog8s) завершён watchdog на flip775 с тем же пакетом idx1×9/idx456, как uk5/uk8. **UMA не обязательна для этого hang.** MSL не сохранились из-за отсутствовавшего каталога: запись исправлена и отдельно проверена, новая сборка проходит. codex_ci2 с split draw+compute от500 только подготовлен; отдельное разрешение запрошено, ответа ещё нет. Эмуляторы остановлены; правильное меню/матч30+FPS не подтверждены. Все dirty изменения сохранены, коммитов нет.

## Предыдущий handoff — 2026-10-27 (день, Claude): wave64, UMA-поверхности, pitch

**Сначала читать [.porting/porting-handoff-fc27-metal-2026-10-27-claude-wave64.md](.porting/porting-handoff-fc27-metal-2026-10-27-claude-wave64.md).** Главное: `KYTY_MSL_WAVE64=1` (эмуляция wave64 для compute) дал самое большое улучшение — связные уличные сцены вместо шума (`artifacts/2026-10-27/uh2`, `uh3`); остались пересвет, белые материалы, полосы в HDR `0x8242360000`. Найден источник NaN в проходе освещения (PS `944430b5642a46c5` читает поверхность 1616x912, которую писали как 1600x904: одна поверхность по pitch, разные размеры дескрипторов) — в UMA-режиме (`KYTY_UMA_SURFACES=1`, по умолчанию выкл.) добавлено правило гостевого pitch, но UMA+wave64 зависает в загрузке (GPU watchdog, виновное ядро не найдено; шаг 1 следующих действий). Ничего не закоммичено; эмуляторы остановлены; один экземпляр за раз. Ниже — предыдущий handoff Codex.

## Handoff Codex — 2026-10-27 00:42

Читать [новый handoff](/Users/tx/Developer/KyTx/.porting/porting-handoff-fc27-metal-2026-10-27.md). Старую Claude-сессию пользователь остановил и подтвердил продолжение здесь; конфликт разрешён. На передачу все эмуляторы/LLDB/gpudebug остановлены, Xcode frame2300 открыт с Replay остановленным. Сохранены3 части RECTLIST fix: GPU3→4, TriangleStrip, packed constant bits. GPU suite/API+shader validation и build проходят. acc1 с final binary дошёл до flip9675 без logged GPU errors, но зелёные блоки/шум остаются; правильного матча/30+FPS нет. Пользователь попросил только handoff, работа остановлена. Ниже исторические состояния.

## Координация сессий — 2026-10-27,00:31

Старая Claude-сессия a890ad9a автоматически возобновилась после лимита и через killmine.sh остановила финальный тест Codex rectpacked1 (последний flip1125; это не подтверждённый crash). Затем возобновила af6cef93351f3493a, запущен acc1. Codex больше ничего не запускает и ждёт выбора пользователя, какая сессия продолжает. **Сохранён полный source/test checkpoint с новыми RECTLIST/packed fixes: `/Users/tx/Developer/KyTx/.porting/artifacts/2026-10-26/hdr-resume/codex-checkpoint-0031/`**. Подробности и Xcode-доказательства в follow-up2026-10-27 ниже. Итоговая packed-сборка проверена GPU suite/API+shader validation и build; последняя runtime-проверка меню была прервана другой сессией.

## Актуальное продолжение — 2026-10-27, indirect RECTLIST

**Обновление:** Xcode нашёл и помог закрыть3 части RECTLIST: GPU count3→4, настоящий TriangleStrip (подтверждён в новом frame2300), побитное сохранение постоянных packed-атрибутов четвёртого угла (GPU old528/528bad→fixed0 во всех3 режимах). В Xcode CB18/API13272: args `[4,160,0,0]` / original `[3,160,0,0]`, все4 позиции корректны; packed-fix сделан после этой трассы. Native full GPU suite + API/shader validation и ARM64 build проходят. Завершающий rectpacked1 без capture ещё проверяется; смотреть актуальное состояние в follow-up.

Читать [.porting/porting-followup-fc27-metal-2026-10-27-rect-indirect.md](/Users/tx/Developer/KyTx/.porting/porting-followup-fc27-metal-2026-10-27-rect-indirect.md). Найдена сессия Claude со скриншота a890ad9a…; продолжена здесь. Исправлен nonindexed indirect RECTLIST (GPU args3→4, wrapper vertexStart, без CPU readback): old528/1024 bad pixels → fixed5 cases/0; полный MSL GPU suite с Metal API+shader validation и ARM64 build проходят. Один rectind1 дошёл до flip4475 без logged GPU errors, но зелёный/шумный фон меню остаётся. Одиночный frame2600.gputrace сохранён18GiB (rectcap1), эмулятор завершён; Xcode готовит Replay, проверить итог в follow-up. FEX + direct RDNA→MSL, один процесс через run1, без Vulkan/Rosetta; коммитов нет. Ledger CLI недоступен. Состояния ниже исторические, красный экран уже исправлен stencil-реализацией Claude26 октября.

## Продолжение — 2026-10-26: найдена сессия со скриншота, исследуется HDR

Сначала читать [.porting/porting-followup-fc27-metal-2026-10-26-hdr-resume.md](.porting/porting-followup-fc27-metal-2026-10-26-hdr-resume.md), затем [.porting/porting-handoff-fc27-metal-2026-10-26.md](.porting/porting-handoff-fc27-metal-2026-10-26.md). Найдена точная Claude-сессия a890ad9a-fc56-4a1c-bd0c-4830627c7a46. Предположение «NaN до первого светового draw» уточняется: старый PassTrace начинается посреди проходов и статистика у него post-draw. Добавлены env-only multi-flip trace и PRE-RT; идёт одиночный hdrpre1 через run1.sh. Исправление всего меню/матча ещё не заявлено. Только один эмулятор, FEX+RDNA→MSL, без коммитов.

## Актуальный хендовер — 2026-10-25 вечер (Claude): HTILE, compute-образы, depth как текстура

**Читать [.porting/porting-handoff-fc27-metal-2026-10-25-evening.md](.porting/porting-handoff-fc27-metal-2026-10-25-evening.md) ПЕРВЫМ** (затем раздел «Третья причина…» в [stencil follow-up](.porting/porting-followup-fc27-metal-2026-10-25-stencil.md)). Исправлено в Metal: HTILE fast clear глубины (меню получило настоящий фон), «последний GPU-писатель» и mip-цепочки RT для compute-картинок (тонмаппинг читал устаревшую копию → блочный мусор в «Classic Match»), depth как текстура у draw'ов (velocity/TAA), CMASK-fill'ы не рвут render pass (Xcode: 8,5 GiB/кадр), заголовок окна `KyTx | игра | frame, fps`. **Не доказано:** что мусор в «Classic Match» ушёл (последний прогон упал), и ничего про матч: поле/игроки там чёрные, G-buffer с «лучами» из схлопнутых вершин игроков; подозрения — пул матриц `0x817d000000` (нулевые/NaN строки, пишется compute из 512-КБ staging без снапшота) и экспозиция (прецедент UFC 5 в `kytyps5-ufc5/ufc5/ledger.md`). Пользователь: чинить по меню, там те же игроки. Также открыты: шум 8×1–2 px после TAA, случайные падения гостя (Job-потоки), шумные превью меню. Ловушка отладки: старые `drive.sh` продолжают слать нажатия в новые запуски. Не убивать эмулятор по имени (`tools/killmine.sh`); трасса `htile/frame9600.gputrace` (96 ГБ) — удалить после разбора. Коммитов нет.

## Актуальное продолжение — 2026-10-25 (Claude): красный слой = не было stencil; исправлено

**Сначала читать [.porting/porting-followup-fc27-metal-2026-10-25-stencil.md](.porting/porting-followup-fc27-metal-2026-10-25-stencil.md).** Красный экран был вызван тем, что Metal рисовал в depth-only цель и игнорировал весь stencil: UI-«шторки» (VS `bc0210100fe76732`/PS `ae49c6a306380213`) — это stencil-клиппинг (NEVER+REPLACE-маски, которые не должны писать цвет, и EQUAL-заливки внутри маски); без stencil каждый из ~24 draw'ов красил весь экран α=0.6 → чистый красный. Реализован stencil (отдельная Stencil8-текстура + перевод состояния по образцу Vulkan-пути + reference per draw); GPU-тест `StencilMasking` 64 px/0 ошибок, `tools/test_metal_pipeline.sh` проходит. Теперь главное меню FC27 рисуется корректно (текст, пункты, FC HUB, «Press X to reconnect»). **Фон-3D сцена всё ещё неверна** (шум/цветовые полосы) — следующая задача; сравнивать с эталонными Metal-трассами Vulkan-сборки `captures/fc27-20261025/ref/vk_f1700.gputrace`. Также: исправлен `USE_THREAD_DIMENSIONS` в Metal DispatchDirect (раньше ×group_size лишних потоков на заливках памяти); найдено, что игра выбирает динамическое разрешение (1600×904 вместо 1920×1080 эталона) по EOP-таймстемпам из часов времени кодирования — обход `KYTY_EOP_CLOCK_FREEZE=1` даёт полное разрешение и структурированную сцену; рабочая комбинация флагов и список опровергнутых гипотез в follow-up. Эталонная Vulkan-сборка использовалась только как визуальный oracle. **Позже в тот же день:** добавлена очистка DCC/CMASK-метаданных (`NoteUniformMetadataFill`, KYTY_NO_META_CLEAR=1 отключает): игра показывает вступление матча (карта, Old Trafford), пользователь в живой игре получил 38–40 FPS; см. раздел «Очистка DCC/CMASK-метаданных» в follow-up. Пользователь играет на этой машине — не убивать эмулятор по имени (`tools/killmine.sh`). Коммитов нет, FEX+RDNA→MSL only. Рабочая комбинация (`KYTY_EOP_CLOCK_FREEZE=1 KYTY_MSL_GDS=1 KYTY_MSL_CUBE=1` + сценарный `KYTY_AUTO_INPUT`) **доходит до экрана Classic Match (Man Utd–Liverpool) и старта загрузки матча** с корректным UI; после старта матча — чёрный/шумный экран и 0–9 fps (не исследовано). Меню держит ≈29–30 fps; 30+ FPS в матче не достигнуто.

## Актуальное продолжение — 2026-10-25: исправлены X у EA и Press X to Skip

**Сначала читать [.porting/porting-followup-fc27-metal-2026-10-25-ui-mip-tail.md](.porting/porting-followup-fc27-metal-2026-10-25-ui-mip-tail.md).** Через Xcode API3457 нашли пустую32×32 icon texture слева от текста EA. Guest bytes непустые: T# объявляет2 mips, base level находится в mip tail(64,0), прежний uploader читал(0,0). Добавлены полноценная tiled2D mip upload и bounded decoder;58 прежних GPU cases +8 mip chains/0fail. В игре подтверждены X в Press X to Skip, Cancel and Play Offline, EA error OK и Let's Play. Новая одиночная after-fix trace `captures/fc27-20261025/ui-mip-tail/frame500.gputrace` валидна16CB/189draw/142dispatch; Xcode Replay/API3727 показывает texture108 с2 mips/правильной alpha mask и X в output. Красный фон/3D и полное интро ещё не исправлены; правильного матча30+FPS нет. Capture fault отдельно остаётся (первый Layer capture0/1, повтор500 под LLDB успешен). Эмулятор/LLDB/gpudebug завершены, Xcode active на новомAPI3727. No commits/FEX+RDNA→MSL only.

## Актуальное продолжение — 2026-10-25: шесть одиночных Xcode GPU-трасс

**Сначала читать [.porting/porting-followup-fc27-metal-2026-10-25-gputrace-series.md](.porting/porting-followup-fc27-metal-2026-10-25-gputrace-series.md).** По уточнённому запросу сняты отдельные `.gputrace`500,600,700,800,900,1000. Все6 имеют workload, GPU replay ready и успешный color0 fetch. Frame1000 переснят отдельно после неполного первого файла, также успешно Replay в Xcode через CUA:22CB/864draw/267dispatch, Frame ready. Каталог `runs/captures/fc27-20261024/frames500-1000-193356/` во внешнем fex-spike. Добавлен comma-list в KYTY_GPU_CAPTURE, каждый захват остаётся одним flip interval. Build/diff check pass. Красный кадр сохраняется; graphics fix/FPS win этим не заявлены. Xcode оставлен с активным frame1000; эмулятор/LLDB/gpudebug завершены. FEX+RDNA→MSL only; no commits. Предыдущий image follow-up и подробный logo fix читать далее.

## Актуальное продолжение — 2026-10-24: image sampling, новые валидные трассы и red draw

**Сначала читать [.porting/porting-followup-fc27-metal-2026-10-24-image-sampling.md](.porting/porting-followup-fc27-metal-2026-10-24-image-sampling.md).** Исправлены float array layer, array/3D gradients и gather component: 13 GPU cases /9 ошибок →0; полный native MSL suite и ARM64 build проходят. Проблемный PS69484ce5ea9fbf78 больше не пропускается. MRT off/on heavy runs без logged GPU errors; red остаётся. Новая after-fix single-frame трасса валидна:22CB/720draw/252dispatch, Xcode Replay успешен. API25390 содержит6 красных исходных вершин с UV sentinel; изолированные GPU probes подтверждают solid red output и корректный непрозрачный background draw (4 render cases/0fail). Происхождение красных CPU данных ещё не установлено; LoadActionLoad сам по себе не доказан причиной. Сохранены воспроизводимые артефакты. FEX+RDNA→MSL only, no commits, цель корректного матча30+FPS не достигнута.

## Актуальный handover — 2026-10-24 17:38: фикс GPU Hang (ReadFirstLane UB + KYTY_LOOP_CAP), рубеж 1475 флипов, локализация красного экрана

**Сначала читать [.porting/porting-handoff-fc27-metal-2026-10-24-1738.md](.porting/porting-handoff-fc27-metal-2026-10-24-1738.md).**
- **GPU Hang полностью устранен:** локализована причина сбоя на флипах >1000 (`state=4 MTLCommandEncoderErrorStateFaulted`). Устранено UB в `ctz(0)` при пустом ballot в RDNA waterfall-циклах (`kyty_first_lane` в `MslEmitter.cpp`), а также введен настраиваемый лимит циклов `KYTY_LOOP_CAP` (дефолт 4096 вместо 1048576, приводившего к 2-секундному watchdog таймауту Metal).
- **Рубеж флипов пройден:** в прогоне `mrt-ab-1-172536` достигнут **флип 1475** со 100% стабильностью (`exit_code: 0`, 0 ошибок GPU, Native MRT активен).
- **Локализован красный экран:** интервальный дамп показал, что до флипа 900 экран чистый (заставка + логотип + четкое окно диалога EA Servers с 14.7k белых пикселей текста). На флипе 950 появляется красный слой `(1.0, 0.0, 0.0, 0.6)` из вершинного шейдера `b2c4c2e311c5bc76`, который накапливается в `Texture449` из-за `MTLLoadActionLoad`.
- Сборка и MSL/pipeline тесты проходят с 0 ошибок. Git tree не коммитить, dirty/untracked tree сохранено.

## Актуальный handover — 2026-10-24: фикс логотипа и метод Xcode

**Сначала читать [.porting/porting-handoff-fc27-metal-2026-10-24.md](.porting/porting-handoff-fc27-metal-2026-10-24.md).** По просьбе пользователя подробно сохранено: как через Xcode локализовали UI draw и прочитали пиксель, как обнаружили потерю packed flags при интерполяции, что изменили в MslEmitter (`flat` / `center_no_perspective`) и как проверили (6 GPU случаев: до5 ошибок → после0). Пользователь подтвердил появление логотипа; полное интро/остальные значки ещё не признаны исправленными. Точные hash/API/pixel coordinates и артефакты — в новом handover.

Новая MRT-поддержка записывает все активные цветовые буферы (GPU old19 неверных компонентов → fixed0), но **стабильность в игре НЕ принята: после flip1075 GPU Hang**, A/B MRT0/1 подготовлен и не запускался. Новый `mrt-fixed-one.gputrace` открылся в Xcode с **0 command buffers**, для pixel debug непригоден. Эмулятор/CLI-debuggers остановлены, Xcode replay остановлен. Цель матча30+FPS не достигнута. FEX + direct RDNA→MSL, без Rosetta/Vulkan renderer. Коммитов нет; dirty/untracked tree сохранить. Ниже исторические состояния.

## Актуальное продолжение — 2026-10-23, 23:50

Сначала читать [.porting/porting-followup-fc27-metal-2026-10-23-2350.md](.porting/porting-followup-fc27-metal-2026-10-23-2350.md). Исправлены коллизии native MSL специализаций (GPU: old2fail -> fixed0), flat/no-perspective параметры (6 GPU случаев: old5fail -> fixed0), color/blend state проходов глубины (2 GPU случая/0fail), а также LDS append/consume независимо от GDS (6 случаев). Full MSL API+shader validation и сборка проходят. Xcode локализовал белый прямоугольник в API7722, до композита. Тяжёлая сцена всё ещё красная; корректного матча и 30+ игровых FPS нет. Последний запуск проверяет depth-only правку; актуальный лог указан в `runs/validation-20261023/latest-diagnostic-log.txt`. Старые состояния процессов ниже устарели. Коммитов нет, dirty tree сохранить.

## Актуальный handover — 2026-10-23 15:12

Читать сначала [.porting/porting-handoff-fc27-metal-2026-10-23-1512.md](.porting/porting-handoff-fc27-metal-2026-10-23-1512.md). FEX + RDNA→MSL; без Rosetta/Vulkan renderer. Задача не завершена: тяжёлая сцена красная, правильного матча и 30+ игровых FPS нет.

Новая одиночная трасса `shared-fixed-one.gputrace` успешно воспроизведена в Xcode. На API21316 fragment shader реально возвращает красный из vertex color `(1,0,0,0.6)`; конкретные ресурсы/шаги в новом handover. Подтверждённая новая правка — transient ring buffer больше не затирает данные ожидающих GPU-команд (тест до:2 ошибки, после:0). Эксперимент расширения vertex-диапазонов дал GPU Hang, сохранён отдельным патчем и полностью откатан. Также пойман отдельный assertion compressed format в linear texture path. Прежние shader/resource/API исправления сохранены. Процессы эмулятора/LLDB и Xcode replay не работают. Коммитов нет; dirty tree сохранить.

Ниже более ранние записи; их состояние процессов, «следующий баг» и пути /tmp могут быть устаревшими.

# Актуальный handoff — 2026-10-23, одиночная GPU-трасса и Xcode

**Начинать с [.porting/porting-handoff-fc27-metal-2026-10-23.md](.porting/porting-handoff-fc27-metal-2026-10-23.md).** Он дополняет и исправляет исторические записи ниже. Краткий статус: [.porting/porting-memory.md](.porting/porting-memory.md).

- Получена **целая трасса одного present**, открыта и успешно воспроизведена в Xcode через computer use: `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261023/single-frame/lldb-present.gputrace` (9.6 GiB). Снята после flip700, **не точный flip1400**. Старый `frame800.gputrace` повреждён и непригоден.
- Xcode подтверждает красный overlay `MTLTexture2364` (RGBA8, mips) ещё до финального композита; источник первого красного содержимого пока не доказан. Корректный вход фонового draw есть; viewport/culling/write mask выглядят ожидаемо.
- Исправлены потеря encoder state при AliasConvert и продолжение draw с прошлым pipeline после ошибки шейдера/PSO. Mip-RT теперь допускают full-chain R32Uint views для raw-копий. GPU-проверка mip-format: 8 случаев, 0 ошибок, API validation включена. Основной ARM64 эмулятор пересобран.
- Цель остаётся незавершённой: нет доказанного исправления интро/иконок/красной тяжёлой сцены и нет корректного играбельного матча с измеренными 30+ FPS. FEX + RDNA→MSL; GDS OFF; без Rosetta/Vulkan renderer. Коммитов нет.

## Исторический handoff — 2026-10-22

**Начинать с [.porting/porting-handoff-fc27-metal-2026-10-22.md](.porting/porting-handoff-fc27-metal-2026-10-22.md).** Там полный самодостаточный контекст, изменения, команды, доказательства и следующий тест. Краткий статус: [.porting/porting-memory.md](.porting/porting-memory.md).

- Цель НЕ достигнута: интро/иконки/артефакты остаются, играбельный матч и 30+ FPS в нём не проверены.
- Только FEX + native RDNA→MSL. Пользователь разрешил Vulkan исключительно для визуального референса; проверенного референсного кадра пока нет. Rosetta запрещена.
- Ветка `metal-native-checkpoint`, HEAD `c30d5d81`. Изменения не закоммичены; не откатывать.
- Реализованы S#-сэмплеры, нативные address loads с таблицей гостевых страниц, форматированные store, структурированные циклы/phi и загрузка cube faces/mips. Есть проходящие GPU/ядровые тесты. Это НЕ подтверждает исправление картинки.
- **Последний тест `ReadFirstLane` в VS прошёл** в полноценном render pass. Основной эмулятор после этой последней правки ещё НЕ пересобран; нужна пересборка и проверка игры.
- **GDS оставить выключенным:** последний запуск с `KYTY_MSL_GDS=1` дал сплошной зелёный экран. Cube без GDS давал красный фон. Экспериментальные флаги не продвигались в defaults.
- Игра остановлена. Следующее полезное направление — проверить публикацию EOP label до завершения Metal GPU, пока только гипотеза; код EOP не меняли.
- Основные логи и тесты сохранены в `.porting/artifacts/2026-10-22/`; все исходные PPM/MSL/логи — `/tmp/kytx-font-video.8uf0mA`.

## Обновление 2026-10-23 (после handoff Codex): красный экран, видео, ореол кнопок
- **Красный экран — не «force red».** Все красные зонды (`KYTY_MSL_PS_RED`, `KYTY_MSL_CS_RED`, `KYTY_DIAG_DEBUG_PS`, `KYTY_PS_PROBE`) включаются только переменными окружения и в прогонах не стояли. Причина: сырые 32-битные compute-копии (`91c4e2e9…`, `R32Uint`, 1920×1080 групп) писали `uint4` в текстуру RGBA8 (render target — это linear-текстура поверх гостевой памяти без `ShaderWrite`/`PixelFormatView`), получалось (255,0,0,255), а слой накапливал красный от кадра к кадру (композит читает этот RT).
- **Исправления:** `ReinterpretFormat` теперь создаёт второй linear-вид нужного формата над тем же `MTLBuffer` (`newTextureWithDescriptor:offset:bytesPerRow:`) для buffer-backed RT — память общая, uint-ядро видит биты цвета; поиск RT по адресу выбирает цель того же формата/размера (раньше — «самая новая» с другим форматом, например RG8 вместо RG11B10), реестр compute-образов отвергает текстуры другого размера texel (`SameTexelSize`); `GuestTexture` для линейных поверхностей берёт реальный pitch (`TileGetTexturePitch`: R8 1920 → 2048) — видеоплоскости YUV (libVLC, декодируется на CPU, три R8-плоскости на draw `fe48d4a99493027c`) теперь корректны, раньше были «полосатыми»; cube сэмплится в graphics-стадиях по умолчанию (draw'ы с cube PS больше не пропадают), в compute — только `KYTY_MSL_CUBE=1`.
- **Видимый результат:** ореол вокруг кнопок плавный (раньше ступенчатый), красной фазы в серии прогонов нет; игра теперь стабильно стоит на диалоге «EA Servers are unavailable» (≈4.6k draw'ов за кадр, 30 FPS). Прежние прогоны, где игра шла дальше в «тяжёлую» фазу (~25k draw'ов, 3D-сцена), больше не воспроизводятся без ввода; для проверки 3D нужен Cross (SDL: клавиша J).
- **«Фоллбеки» теперь видимы:** `NoteFallback(...)` печатает `[Fallback] <место>: key=… extra=…` один раз на ключ (молчаливые подмены формата на RGBA8/BGRA8, белая default-текстура для непривязанного image-слота, нулевая текстура compute, RT другого размера texel); `KYTY_FALLBACK_FATAL=1` — abort. Непривязанных слотов ≈150 (в основном объявленные, но неиспользуемые) — различать «используется/нет» ещё предстоит.
- **Диагностики (новые):** `KYTY_LOG_DRAWS_FLIP=lo,hi` (`[DrawF]` с cmask/dcc/clear и `[CsF]` по каждому dispatch'у), `KYTY_DUMP_RT_DRAW=<rt>,<ps>,<k…>` + `KYTY_DUMP_RT_MIN_FLIP` (дамп RT до k-го draw'а), `KYTY_DUMP_CS_IMGS=<hash>,lo,hi` (средний цвет входов ядра, раз в 10 флипов), `KYTY_LOG_TEX_UNIQ=lo,hi` + `KYTY_DUMP_TEXUNIQ=1`, `KYTY_LOG_DMA`, `KYTY_GPU_CAPTURE=<flip>` (+`MTL_CAPTURE_ENABLED=1`).
- **GPU-трасса:** `KYTY_GPU_CAPTURE` на лёгком кадре (flip 300) пишет .gputrace (2.5 ГБ) и закрывается; на тяжёлом (flip ~2400) процесс падает `SIGILL` при `stopCapture`. Для разбора красной фазы хватило собственных дампов RT/входов ядер.
- **Красная фаза всё ещё воспроизводится** в «тяжёлых» прогонах (≈20–30k draw'ов/кадр, ~7–15 FPS, начинается около flip 1000–1500, в части прогонов игра сама уходит с диалога, без ввода; в «лёгких» прогонах остаётся диалог 4.6k draw'ов и красного нет — состояние недетерминировано). Факты для следующего шага: overlay-RT композита `0x8240c00000` (RGBA8) в этой фазе (255,1,1,255); `res=0` композита (`0x8245230000`) почти чёрный (верно); порядок за кадр: compute-копии `91c4e2e9` (R32Uint, 1920×1080 групп) пишут в `0x8240c00000` из `0x82400e0000`, bloom-цепочка `25eaf253` (480×270), затем `DRAW rt0=0x8240c00000 ps=58effb53` (блит 3840×1080 фона), затем композит `b3300709`; в этой же фазе `0x8246340000` (1920×1080 RGBA8, PS `d5626172c13d2500`) целиком (128,0,0) — буфер RG8, зачищенный в (0.5,0), читается как RGBA8; `0x8241ce0000` (PS `88ec6d529f0942f9`) — мусор раскладки. Гипотеза: ещё одно алиасинг-несоответствие (разные форматы/раскладки на одном адресе), а не «force red». Следующий шаг: `KYTY_DUMP_RT_DRAW` на `0x8240c00000` с `KYTY_DUMP_RT_MIN_FLIP` и `KYTY_DUMP_CS_IMGS` в тяжёлом прогоне (запускать в фоне, `nohup`, окно 200+ с, диагностика замедляет игру и может увести её в лёгкое состояние).
- Computer use: окно эмулятора (`kyty_emulator` без bundle-id) недоступно для `app_*`-инструментов; установленное приложение `org.kytyps5.kytyps5` (KytyPS5) — отдельный bundle. Ввод Cross не потребовался: в тяжёлых прогонах игра уходит с диалога сама.
- **Единая модель поверхностей (начата 2026-10-23, по решению пользователя «гостевая память — источник правды»).** Новый модуль `src/graphics/host_gpu/metal/metalTiling.{h,mm}`: один универсальный GPU-кернел tile/untile для всех раскладок гостевой памяти (Standard 256B/4KB/64KB, PRT, RenderTarget64KB, Depth64KB). Все раскладки GF(2)-линейны по (x, y) (и по блоку для банк-XOR), поэтому адрес = `блок*размер + ((in_x[x]^in_y[y]) ^ (bank_x[bx]^bank_y[by]^v0^layer_xor))`; таблицы строятся из `TileGetBlockOffset/TileGetBlockXor` с самопроверкой. Тест: `bash tools/test_metal_tiling.sh` (58 случаев: семейство × размер элемента × слой, сверка с CPU-эталоном, import и export) — проходит. Профайлер/`Dbg*` для автономной сборки — в `tools/test_shims/`.
- `AliasConvert` в `metalRenderer.mm` переписан на этот модуль: последний GPU-писатель адреса (`RecordComputeStorage(addr, tex, tile)` — compute storage-образы и render pass'ы) экспортируется в scratch тайлового образа и импортируется в текстуру читателя (другой размер элемента/сетка/раскладка), включая BC-читателей; доверяется только писатель не старше 2 флипов. Лог: `[Surface] addr … writer WxH eb tile -> reader …`.
- **Ограничения/следующий шаг:** (1) глубина не записывается как писатель — `s_depth_tex` одна статическая Private-текстура без адреса; без этого чтения «RT-память как глубина» (`tile=24`) берут устаревшего писателя; нужно хранить глубину по адресу и экспортировать через blit в буфер; (2) мип-цепочки/слои/3D не конвертируются; (3) красный экран после этого не исчез — одно красное исправление (raw-копии в RGBA8) уже сделано, источник оставшегося пока не найден; (4) CPU-записи в адреса, которые GPU писал раньше, не отслеживаются (эвристика «писатель не старше 2 флипов»).
- **Глубина (2026-10-23, сделано):** вместо одной статической Private-текстуры глубины и безусловного `Clear 1.0` на каждом проходе — по поверхности на адрес `z_read_base_addr` (живёт между проходами, `Load`), очистка только для свежесозданной и для draw'ов с `DB_RENDER_CONTROL.DEPTH_CLEAR_ENABLE` (значение — `GetDepthClearValue()`; сам clear-draw пропускается). Глубина записывается как писатель (`tile=24`), `AliasConvert` экспортирует её через blit в буфер → uint-текстуру → tile-export. Эффект: сцена-RT `0x8240c00000` теперь стартует чёрной (раньше — красно-чёрные блоки), геометрия при включённой глубине проходит тест (раньше — ничего), но искажена (полосы/«стрелки»: данные вершин/индексов от GPU-driven ядер с `ds_append`).
- **Красный экран — состояние на конец сессии:** остаётся в «тяжёлой» фазе. Факты: композит `b3300709` на тот же флип: `res=0` (сцена, `0x8245230000`) верный чёрный-синий; `res=3` (overlay RGBA8, `0x8240c00000`/`0x82438f0000`) = (254.9, 0.9, 0.9, 255) — красный уже на входе; `res=5` (выход) тоже красный, показанный кадр красный. Значит красный создаётся до композита, в overlay-RT. `KYTY_SYNC_FLUSH=1` (ждать GPU на каждом flush) красный не убирает — это не гонка на уровне кадра. Дамп входов с синхронизацией (`KYTY_DUMP_CS_IMGS`) на другие флипы красного не показывал — диагностика меняет тайминг и «лёгкость» прогона. Писатели overlay-RT за кадр (flip 1300): compute `362ec9ae` (читает), `91c4e2e9` ×2 (raw R32Uint копия из `0x82400e0000`), bloom `25eaf253` (480×270), `DRAW rt0=0x8240c00000 ps=58effb53` (блит 3840×1080 фона), затем композит. Следующий шаг: `KYTY_DUMP_RT_DRAW=8240c00000,58effb53710a240a,0` с `KYTY_DUMP_RT_MIN_FLIP=1000` в тяжёлом прогоне — посмотреть RT до/после блита; проверить, что текстура 3840×1080 (`0x8104000000`…) в этой фазе не красная; сравнить T# blend/цветовую маску блита (`DrawF` печатает cmask/dcc/clear).
- **Новые диагностики:** `KYTY_SYNC_FLUSH=1`; `[Surface]`-лог конвертера; `KYTY_NO_ALIAS=1` отключает конвертер.
- Осталось: белый прямоугольник внизу по центру (редко), иконки кнопок (текстура 40×32 нулевая), 3D-сцена (GPU-driven: `DataAppend`/GDS-ядра нужны для indirect draw'ов; с `KYTY_NO_DEPTH=1` геометрия видна, но искажена), видео интро идёт через libVLC (YUV-плоскости корректны).

**Ниже исторический журнал.** Утверждения старого блока про единственный default sampler, незагруженные cube faces и исключительно pc-switch кодген уже устарели. Старое разрешение на Haiku-делегацию не переносить на нового агента автоматически.

---

# Предыдущее состояние на 2026-10-22 / следующие шаги

Для следующего агента (другой инструмент). Секция самодостаточна; ниже по файлу — хронологический журнал (старые записи про UFC 5 и ранние этапы уже не актуальны).

## Правила
- **KyTx = только Metal**: никакого Vulkan, SPIR-V, MoltenVK, Rosetta. Рендер — нативный RDNA→MSL (`src/graphics/shader/recompiler/backend/msl/`, `src/graphics/host_gpu/metal/`). Vulkan-код в дереве — только справочник (как Kyty делает ту же операцию).
- Не коммитить без явного разрешения пользователя. Всё сейчас не закоммичено (ветка `metal-native-checkpoint`).
- Отчёты пользователю — по-русски. Механическую работу можно отдавать Haiku-субагентам, «угадайки» делать самому.
- Репозиторий: `/Users/tx/Developer/KyTx` (в сообщениях компилятора фигурирует тот же каталог как `/Users/tx/Developer/kytyps5-arm64`). Игра: `/Users/tx/Developer/PPSA34015` (EA SPORTS FC 27). Каталог запуска: `/Users/tx/Developer/fex-spike/runs`.

## Сборка и запуск
```bash
cmake --build /Users/tx/Developer/KyTx/_Build/macos-arm64 --target kyty_emulator -j$(sysctl -n hw.logicalcpu)
# запуск на N секунд (лог — в stdout/stderr, перенаправить); доп. переменные окружения — аргументами
/Users/tx/Developer/KyTx/tools/run_fc27.sh 100 KYTY_DUMP_FRAME=600,1500 KYTY_DUMP_PATH=/tmp/f_%llu.ppm > /tmp/run.log 2>&1
python3 -I /Users/tx/Developer/KyTx/tools/frame_grid.py /tmp/grid.png /tmp/f_600.ppm /tmp/f_1500.ppm   # смотреть кадры
```
- Скрипт задаёт обязательный набор: `KYTY_BACKEND=metal KYTY_VIDEO_OUT_1080P=1 KYTY_NP_STATE=signed_in KYTY_LABEL_FLUSH=lazy KYTY_WAIT_RETRY_ELIDE=1 KYTY_WAIT_BY_ORDER=1 KYTY_UMA_READBACK=1`. `KYTY_MSL_ONLY=1` отключает дисковый кэш MSL (`_PipelineCache/msl/msl-s2`, при смене эмиттера кэш надо чистить или оставлять этот флаг).
- Время идёт неравномерно: номера флипов от прогона к прогону плавают (±десятки), кадры брать сериями.
- Типичный здоровый прогон: 45–60 FPS на старте, ~12 с, потом 30 FPS (лимит игры); `DrawStats` — `dropped_*=0`.
- Метка «кадр N» = `KYTY_DUMP_FRAME` (номер представленного кадра, PPM P6 1920×1080). Для остальных диагностик см. ниже.

## Флаги окружения (важные vs диагностические)
Влияют на поведение (нужны/полезны):
- `KYTY_MSL_ONLY=1` — без дискового кэша MSL.
- `KYTY_MAX_GROUPS=<n>` — потолок групп dispatch'а на измерение (по умолчанию 0xFFFFFF; раньше 65535 обрезало стартовые dispatch'и).
Экспериментальные, **по умолчанию выключены** (включать только для проверки):
- `KYTY_MSL_GDS=1` — лоуэринг `DataAppend/Consume` (GDS-счётчики) + Metal-GDS и DMA_DATA в GDS. Ядра исполняются, но кадр хуже (текст бьётся, FPS до 0) — причина не выяснена.
- `KYTY_MSL_CUBE=1` — сэмплинг cube как 2D-array по граням. Ядра cube исполняются секунды на кадр (кодген «state machine»), грани не загружаются — оставлено выключенным.
- `KYTY_SNAPSHOT_LINEAR=1` — снимок линейных compute-источников на момент кодирования (гипотеза про гонку CPU/GPU у атласа глифов; эффект не доказан).
- `KYTY_OLD_SAMPLER=1`, `KYTY_NO_ALIAS=1` — A/B-выключатели (сэмплер без argument buffers / `AliasConvert`).
Только диагностика (на картинку не влияют): `KYTY_DUMP_FRAME/KYTY_DUMP_PATH`, `KYTY_MSL_DUMP_DIR=<dir>` (MSL каждой программы), `KYTY_LOG_TEX_UNIQ=<lo>,<hi>` (все сэмплируемые образы диапазона флипов с долей ненулей и гостевой памяти), `KYTY_LOG_BUF_COVER=<lo>,<hi>` (какие compute-буферы покрывают адреса), `KYTY_LOG_ALIAS`, `KYTY_LOG_DEFTEX`, `KYTY_LOG_GDS`, `KYTY_MTL_FAULT_INFO=1` (+`[CsImgBind]`), `KYTY_MTL_GPU_TIME=1` (долгие command buffer'ы с хэшами ядер), `KYTY_LOG_CS=<hash>`, `KYTY_LOG_TEX=1`, `KYTY_MEM_LOG=1`; остальные — в журнале ниже.

## Что работает (проверено кадрами)
- Заставка, интро-фон и логотип FC 27 с верными цветами; диалоги «Kyty is connecting…» / «EA Servers are unavailable…» с текстом и кнопками («Ok», «Cancel and Play Offline»). UI-слой доходит до презентуемого кадра.
- Корень этого — раскладка argument buffer: Metal укладывает члены структуры подряд и игнорирует пропуски в `[[id]]`; эмиттер теперь заполняет пропуски массивами `kyty_gap_N` (см. запись «UI и текст отображаются» ниже). Не ломать: каждый новый член argument buffer должен учитываться со своим числом id.
- Атлас глифов (R8, 2048²): compute `e5941d54` пишет в `R8Uint`, draw читает `R8Unorm` через вид; целочисленные форматы writable; compute-образы не перезатираются гостевой копией (`cs_written`).
- `AliasConvert`: BC-сэмплер над 16-байтными блоками compute (копия через буфер) работает; несжатый путь (scatter/gather со свизлом) написан, но на реальном кадре не срабатывал.
- Compute: атомики LDS/буферов, gather, resinfo, форматированные store; пропущены только `DataAppend` (2 ядра) и cube (4 ядра, по умолчанию).

## Известные проблемы (по важности)
1. **Иконки кнопок отсутствуют** («Press [ ] to Skip», «Ok»): текстура 40×32 RGBA8 (`tfmt=56`, `tile=9`, `dst_sel=6547`) нулевая и в гостевой памяти. Её не пишут ни compute-образы, ни compute-буферы (`KYTY_LOG_BUF_COVER`), ни RT. Гипотеза: CPU-растеризация глифа кнопки через системный шрифт/`libSceFont` (в эмуляторе шрифтов кнопок нет) или стриминг ассета, не завершённый в нашем окружении. Следующий шаг: трассировать вызовы `sceFont*` (`src/libs/libFont.cpp`) и чтение файлов около появления «Press».
2. **Видео интро не видно**: после логотипа остаётся статичный фон. Если это видео — AvPlayer/VideoDec (`src/libs/avPlayer.cpp`, `libVideoDec2.cpp`), не GPU. Не проверено, идёт ли декодирование вообще.
3. **Текст «Kyty is connecting…» иногда рисуется ломаными кусками** (атлас глифов неполный в момент draw'а; в части прогонов текст идеален). Гипотезы: гонка CPU/GPU (игра перезаписывает staging до исполнения dispatch'а; пробовать `KYTY_SNAPSHOT_LINEAR=1` серией прогонов), либо порядок compute→draw.
4. **Ореол вокруг кнопки «Ok» ступенчатый** (4×4-блоки, BC3 32×56 через `AliasBC`) — данные ореола декодируются неверно; проверить содержимое BC-текстуры и порядок блоков/мипов.
5. **GDS (`ds_append`)**: нужен корректный перенос счётчиков (игра делает DMA_DATA память→GDS 0x40/0x44/0x48 до ядер и GDS→память после). Моё решение даёт хуже кадр; вероятные причины — wave64 игры против simdgroup 32 (счётчик на simdgroup), порядок GDS→память относительно чтения CPU, `ds_append` возвращает значение с индексом по линии (в SPIR-V-версии Kyty всем линиям отдаётся одно значение — проверить по ISA).
6. **Cube-ядра (4 шт.)**: перфоманс кодгена («state machine» с `pc`-switch: секунды на dispatch) + грани cube не загружаются. Ускорение структурированным кодгеном (циклы вместо switch) — общая задача, даст выигрыш и для других тяжёлых ядер (`0xb541190e…`, `0x89bdd959…`, `0x4fbf70b2…` — сотни мс…секунды).
7. **S# сэмплеров игнорируются**: во всех draw'ах и compute — один линейный/clamp сэмплер (`s_default_sampler`). Нужен кэш `MTLSamplerState` по S# (реф: `host_gpu/renderer/cache/samplerCache.cpp`; поля `ShaderSamplerResource`; для argument buffer нужен `supportArgumentBuffers=YES`).
8. Мелочь: редкий белый прямоугольник внизу по центру (пустая/default-текстура; `KYTY_LOG_DEFTEX=1`), точки под кнопкой; на старте провалы FPS до 5–20 (тяжёлые compute); 1–2 pipeline'а PS с `color(1)` без dual-source падают на старте (`dropped_pipe=0` в установившемся режиме); stalls возможны при запуске из многопоточного present (`MetalRenderer::m_mutex` обязателен для любых операций кодирования вне draw/dispatch — иначе `endEncoding already called`).

## Куда копать дальше (рекомендация)
1) Иконки кнопок и видео интро — выяснить, что именно пишет их содержимое (шрифт/AvPlayer), это самое заметное для пользователя. 2) Структурированный кодген циклов (перф) → затем включить cube/GDS. 3) S# → `MTLSamplerState`. 4) Ореол кнопки (BC-декод).

## Устройство, которое нужно знать
- Compute-привязки: «сырой» путь `arg_ptrs[]` (индекс = id члена argument buffer, поэтому пропуски заполняются эмиттером); `MTLArgumentEncoder`-ветка (`KYTY_CS_ARGENC`) в нативном MSL не используется. SRT: `arg_ptrs[160]`, GDS: `[161]`, образы `64+res`, сэмплеры `128+i`.
- Образ по адресу: `s_cs_written_by_addr` (реестр, регистрирует базовый образ, не DST_SEL-вид), `s_cs_storage` (compute storage + serial, для `AliasConvert`), `s_guest_textures` (кэш гостевых текстур; `cs_written` запрещает обновление из пустой гостевой памяти).
- Алиасинг формата одной памяти: `ReinterpretFormat` (вид того же размера), `AliasConvert` (разные раскладки), `ConformToDeclaredArray` (2D → 2D-array вид для ядер с array-образом).
- Кадры/файлы-журналы прошлых прогонов лежат в `/tmp/fix6/` (временные).

---

# Handover Context for Claude Code CLI (DeepSeek V4.1 Flash)

## Task
Fix the "sound works, black screen" issue in Kyty PS5 emulator running EA Sports UFC 5 (`PPSA34015`) on Apple Silicon Metal.

## Repository & Runs
- **Codebase**: `/Users/tx/Developer/kytyps5-arm64`
- **Runs directory**: `/Users/tx/Developer/fex-spike/runs`
- **Game Title**: `PPSA34015`

## Key Progress & Status
1. **Shader Sanitization**:
   - `src/graphics/host_gpu/metal/metalShader.mm`: `SanitizeMslSource` fixes `[[id(N)]]` overlapping argument buffer IDs and `spvBufferSizeConstants`. All 189 shaders compile with 0 errors.
2. **Compute Dispatch & Shader Resources**:
   - `src/graphics/host_gpu/metal/metalRenderer.mm`: Resource bindings for buffers, push constants, and textures are wired up.
3. **Black Screen Root Cause**:
   - The game was previously capturing `fmt=65` (`MTLPixelFormatRG16Float`), which is the motion vector buffer, as `s_last_rt_tex` because it was the last draw call of the frame.
   - We updated `metalRenderer.mm` to filter `is_displayable_color` so only real color buffers (`MTLPixelFormatRG11B10Float`, `MTLPixelFormatRGBA8Unorm`, `MTLPixelFormatBGRA8Unorm`, `MTLPixelFormatRGB10A2Unorm`) are stored in `s_last_rt_tex`.
   - The PS5 flip buffer (`vaddr=0x8179800000`) has `total_nz=0` (all zeros). We updated `swapchain.cpp` so if `KYTY_FORCE_RT=1` is set (or if flip buffer is blank), it presents `last_rt_tex` (the active rendered surface `0x8240c00000`).
   - Metal rasterizer writes linearly into shared memory textures. Do not scramble linear textures with RDNA2 detiling.

## Execution Command
```bash
cd /Users/tx/Developer/fex-spike/runs && \
KYTY_BACKEND=metal \
KYTY_FORCE_RT=1 \
KYTY_VIDEO_OUT_1080P=1 \
KYTY_NP_STATE=signed_in \
KYTY_LABEL_FLUSH=lazy \
KYTY_WAIT_RETRY_ELIDE=1 \
KYTY_WAIT_BY_ORDER=1 \
KYTY_UMA_READBACK=1 \
/Users/tx/Developer/kytyps5-arm64/_Build/macos-arm64/kyty_emulator --game /Users/tx/Developer/PPSA34015
```

## Next Steps for Claude Code / DeepSeek Flash
1. Re-verify the emulator build (`ninja -C _Build/macos-arm64 kyty_emulator`).
2. Run the game command above.
3. Check the window ID (`kyty_emulator`) and take a screenshot via `screencapture -l <window_id> screen.png`.
4. Inspect the image to verify UI and in-game visuals are displayed properly.

## Session 2026-10-18 status
- Build is current (`_Build/macos-arm64/kyty_emulator`, 23:12). Launch + boot works; game reaches present loop at ~37–46 FPS.
- `screencapture` from the Claude session fails with "could not create image" — the terminal lacks
  macOS Screen Recording permission. Either grant it (System Settings → Privacy & Security →
  Screen Recording) or take the screenshot manually; automated visual verification is currently blocked.
- Logs: `/Users/tx/Developer/fex-spike/runs/metal_run.log`. Present path is active:
  `[MetalPresenter] PresentTexture … 1920x1080 fmt=92` (RG11B10Float), i.e. `KYTY_FORCE_RT=1` presents
  `last_rt_tex`, not the flip buffer. Whether those pixels are black is NOT yet verified.
- Window id lookup: `python3` + Quartz `CGWindowListCopyWindowInfo`, owner `kyty_emulator`.
- Next: verify frame content (screenshot, or add an env-gated `getBytes` dump in
  `MetalPresenter::PresentTexture`), then decide if the fmt filter in
  `EnsureActiveRenderPass` (`metalRenderer.mm:163`) picks the right surface.

## 2026-10-20 — дифференциальный разбор FC 27 (Metal vs Vulkan) и привязка compute

### Подтверждённые факты
- Гостевая сторона по PM4 и HLE-вызовам совпадает с Vulkan-сборкой в кадрах 14–20; `AgcDriverRegisterResource` — 52 вызова в обоих полных логах. Расхождение `CpDraw` в окне кадра 300 (Metal 18 против Vulkan 366 за два кадра) не объяснено и отдельно не закрыто.
- Конвейер и презентация работают: принудительный магента/красный PS заполняет RT.
- Привязка изображений в шейдере `f4cac03b183e6196` верна: `image_3` kind 3 → слот 58 (`kind+55`), сэмплер kind 49 → слот 104 (дефолтный; гостевые S# в Metal не создаются).
- Текстура в слоте 58 (`0x81b5f50000`, 2048², R8) нулевая и в памяти гостя (backing совпадает), и в Metal — загрузка корректна, нули идут от производителя.
- Глубина не причина: `KYTY_NO_DEPTH=1` даёт тот же чёрный кадр. Для draw'ов f4cac глубина выключена (`z_en=0`, `st_en=0`).
- Compute раньше вообще не привязывал изображения. Исправлено: привязка сэмплируемых и storage-образов с дефолтами (белая текстура, линейный сэмплер). Id в compute-MSL (`image_3`=3, `image_31`=31, `samplers`=49) совпадают с `kind+0`. Push на `buffer(1)`, размеры на `buffer(25)` — на месте.
- Размеры диспатча первого storage-kernel (`17aeefc07e25295e`): группы `1,16,32`, локальный `32,2,1` — совпадают с данными гостя.
- Выход этого kernel (storage 32×32, RG11B10) после диспатча нулевой. Кадр 600 чёрный.

### Что не сделано / открыто
- Входы первого storage-kernel: сэмплируемых образов нет (только два storage-образа, `slot=33`); буферы (ids 0/1) не проверены.
- Снижение FPS до 4.5 из-за поиска текстур на каждом compute-диспатче. Временно допустимо (отладка), оптимизировать после картинки.
- Производитель текстуры `0x81b5f50000` неизвестен; не проверено, пишет ли compute в её storage-образ.

### Добавленные env-флаги (все по умолчанию выключены)
- `KYTY_TRACE_FRAMES=a:b` — окно кадров для трассировок (флипы, считаются в `PrepareFrame`).
- `KYTY_BIND_TRACE=1` — строка `BIND`/`BINDVK` на draw (хэши, user data, буферы, образы).
- `KYTY_TRACE_PM4=1` — `[Pm4]` по пакетам и `[Flip]` маркеры.
- `KYTY_TRACE_EARLY=1` — `[Entry]`, `[CpDraw]`, `[MetalDrop]`, `[Early]`.
- `KYTY_NO_WBO_SHORTCUT=1` — отключить Metal-ветку wait-by-order (тест).
- `KYTY_TRACE_FLIP_WRITES=1`, `KYTY_TRACE_TEX_BACKING=1`, `KYTY_TRACE_RT_MASK=1` — трассы записей/текстур/масок.
- `KYTY_MSL_NO_RENUMBER=1` — отключить переименование `[[id]]` в санитайзере (`[MslSanitize]` лог).
- `KYTY_PS_PROBE=red|alpha1|uv|tex|cb`, `KYTY_VS_PROBE=pos`, `KYTY_PROBE_HASH=<hash>` — пробы шейдеров (пробы отключают дисковый кэш MSL).
- `KYTY_DEBUG_PS_ANY=1` — магента-PS для любых RT.
- `KYTY_DUMP_BOUND_TEX_AFTER=n` — порог для `KYTY_DUMP_BOUND_TEX`.
- `KYTY_READBACK_TEX=1` (с `KYTY_PROBE_HASH`) — чтение текстуры в слоте 58 и её backing.
- `KYTY_RT_AFTER=<hash>`, `KYTY_RT_AFTER_N=<n>` — дамп RT после N-го совпадающего draw (`/tmp/rt_after_draw.ppm`, `/tmp/rt_after_exact.ppm`).
- `KYTY_TRACE_SLOTS=1` — таблица дескрипторов и `ps_args` для PS.
- `KYTY_TRACE_PS_RT=<rt>` — хэш PS для каждого draw в RT.
- `KYTY_FEX_TRACE=<n>` — HLE-трасса с возвратами (теперь с окном кадров и `tid=`).
- `KYTY_TRACE_LASTCALL=1` — `[thread] create`, `[lastcall]` (с in-flight вызовами), `[signal]`.
- `KYTY_TRACE_CSIMG=1`, `KYTY_CS_READBACK=1` — compute: привязки, геометрия диспатча, дамп storage в `/tmp/cs_out.ppm`.
- Дисковый кэш MSL: `_PipelineCache/msl/msl-s2/…` (схема в `kMslCacheSchema`), `KYTY_MSL_NO_DISK_CACHE=1` — не читать/писать.

### Команда запуска (CLAUDE.md)
```bash
cd /Users/tx/Developer/fex-spike/runs && \
KYTY_BACKEND=metal \
KYTY_FORCE_RT=1 \
KYTY_VIDEO_OUT_1080P=1 \
KYTY_NP_STATE=signed_in \
KYTY_LABEL_FLUSH=lazy \
KYTY_WAIT_RETRY_ELIDE=1 \
KYTY_WAIT_BY_ORDER=1 \
KYTY_UMA_READBACK=1 \
/Users/tx/Developer/KyTx/_Build/macos-arm64/kyty_emulator --game /Users/tx/Developer/PPSA34015
```

### Следующий шаг
1. Проверить буферы (ids 0/1) и push-константы первого storage-kernel `17aeefc07e25295e`: ненулевые ли входы, и доходит ли до записи шейдера (адреса, маска).
2. Найти производителя `0x81b5f50000`: какой compute-диспатч (или draw) должен его писать, и пишется ли он в Metal.
3. Потом оптимизировать поиск текстур на диспатче (FPS 4.5).

Изменения не закоммичены.

## 2026-10-20 (вечер) — падение WindowServer, защиты, валидация Metal

### Что случилось
- `gpuEvent-kyty_emulator-2026-10-20-221043.ips` (bug 284, GPU-событие/fault, `guilty_dm=3`, `command_buffer_trace_id=2824882137`) — прогон в 22:10 с compute-readback (последний storage-биндинг: `0x8179800000`, 1920×1080, `fmt=50` — адрес флип-буфера).
- `WindowServer-2026-10-20-233921.ips` (bug 409, watchdog «unresponsive») в 23:39:21 — совпадает с прогоном 3D-compute-текстур; в логе `kyty_emulator` в ту же секунду «WindowServer event port death». Перед падением — `[Tex3D]` с гостевыми дескрипторами `33x33x33`, `32x64x32` и т.п.
- Во время пункта «меню» по сообщению пользователя WindowServer перезапускался — это тот же класс сбоя.

### Защиты в Metal-бэкенде (по умолчанию включены)
- `GuestTexture3D`: отказ (`[Tex3D] REJECT`) при `w,h>16384`, `depth>2048`, `w*h*d>2^28`.
- `DispatchDirect`: группы >65535 обрезаются (`[CsClamp]`). В прогоне встретилось `655360,1,1` — подозрение на некорректный размер диспатча от гостя.
- Scissor обрезается по размеру связанного цветового таргета (`[ScissorClamp]`). Без этого Metal падал на `setScissorRect` (16384 против 1920×1080).
- `renderTargetArrayLength = 1` в render pass (VS пишет `render_target_array_index`).
- Stencil в depth-stencil state только при `KYTY_STENCIL_ATTACH` (проход без stencil-вложения).

### Диагностика валидации
- Запуск: `MTL_SHADER_VALIDATION=1 MTL_DEBUG_LAYER=1` + обычная команда из CLAUDE.md, ~45 с.
- Найдено и исправлено: scissor 16384; `renderTargetArrayLength`; stencil без вложения.
- Открыто: `validateComputeFunctionArguments: total used threadgroupMemoryLength(36864) must be <= 32768` для `main0`. В кэше MSL максимум 22528 байт — конкретное ядро не найдено; возможно LDS-объявление не распознано поиском. Это может объяснять нулевой выход compute (диспатч отвергается).
- Выход `17aeefc07e25295e` в этом прогоне: `[CsReadback3D] 32x32x32 nonzero_bytes=0/131072` — 3D-выход тоже нулевой.

### Env-флаги (добавлены в этом шаге)
- `KYTY_STENCIL_ATTACH=1` — включить stencil в depth-stencil state (только при наличии stencil-вложения).

### Следующий шаг
1. Найти ядро с threadgroup 36864 байт (объявления LDS в MSL, включая не-массивные) и понять, почему диспатч не выполняется.
2. Для `17aeefc07e25295e`: проверить, доходит ли диспатч (счётчик выполнения), и входные буферы (ids 0/1, push на buffer 1).
3. Решить, нужен ли клэмп `655360` групп (возможно, неверный размер от гостя).

Изменения не закоммичены.

## 2026-10-20 (поздно) — threadgroup-память compute (36864 > 32768)

### Факты
- Валидация Metal воспроизводится стабильно: `validateComputeFunctionArguments: Functon(main0): total used threadgroupMemoryLength(36864) must be <= 32768`. Ровно одно ядро в прогоне.
- Гостевой `lds_size` у compute в трассах мал: `lds_dwords` от 0 до 1792 (максимум 1792 dwords = 7 КБ). Значит 36864 байта — не гостевой LDS, а объявления в MSL, которые SPIRV-Cross выводит из SPIR-V (Workgroup-переменные).
- В кэше MSL (`_PipelineCache/msl/msl-s2`) максимальная сумма `threadgroup`-объявлений 22528 байт. Диагностика `[TgOver]` (сумма по `threadgroup T name[N];`, порог 32768) в проблемном прогоне не сработала → Metal считает иначе: вероятно, `threadgroup`-переменные внутри структур или выравнивание, которые мой поиск не видит.
- MslEmitter (`kyty_lds[8192]`, 32 КБ) в этом пути не используется (KYTY_MSL_BACKEND выключен) — к ошибке не относится.

### Что добавлено
- `[TgOver]` в `CompileSpirvToFunction` (`metalShader.mm`): печатает хэш и сумму `threadgroup` > 32768. Пока не срабатывает — см. выше.

### Следующий шаг
1. Дампить MSL ядра, которое падает валидацией (писать в `/tmp/msl_dump` при первом компиляции каждого ядра с `threadgroup`, или полный дамп всех CS), и найти все `threadgroup`-объявления в нём.
2. Если реальная потребность ≤ 32 КБ — уменьшить объявленный массив до фактической адресации (обрезка по `lds_size`).
3. Если > 32 КБ — эмуляция LDS в device-памяти для этого ядра (per-threadgroup срез буфера, барьеры `threadgroup_barrier(mem_device|mem_threadgroup)`).
4. Потом проверить, появляется ли ненулевой выход у `17aeefc07e25295e` (сейчас 0/131072 байт).

Изменения не закоммичены.

## 2026-10-20 (ночь) — оракул MoltenVK для compute

### Что сделано
- Vulkan-сборка (без `KYTY_BACKEND`) с `MVK_CONFIG_SHADER_DUMP_DIR=/tmp/mvk_msl`, ~60 с. MoltenVK записал 49 файлов: 7 compute-ядер (`shader-cs-*.spv` + `.metal`) и pipeline-заголовки (`pipeline-*.txt`, только хэши VS/FS). Фрагментные шейдеры MSL не дампятся.
- У всех 7 ядер MoltenVK: только буферы (`device … [[buffer(1..3)]]`, push на `buffer(3)`), **ни одного изображения** (`texture2d`/`texture3d` = 0). Нашего ядра `17aeefc07e25295e` (write в `texture3d`, `image_33`) среди них нет.
- Metal-сборка компилирует 38 compute-ядер за то же время; в Vulkan-прогоне — 7. Значит Vulkan-путь за это время не выполнил то же compute-содержимое (либо не дошёл до кадра, либо хранит изображения иначе). Это расхождение состояния, его надо учитывать при сравнении.

### Открыто (побочное, отдельно от основной задачи)
- Ядро с `threadgroup` 36864 байт (`validateComputeFunctionArguments` в Metal): один шейдер, в MSL-диагностике `[TgOver]` не виден; гостевой LDS у compute мал (≤1792 dwords). Нужен дамп MSL проблемного ядра.

### Следующий шаг
1. Более длинный Vulkan-прогон (до кадра, где Metal выполняет `17aeef`), чтобы MoltenVK записал ядро с `texture3d`.
2. Сравнить MSL MoltenVK и KyTx для него: параметры, аргумент-буфер, порядок `[[id]]`.
3. Дамп MSL ядра с 36 КБ threadgroup (побочная задача).

Изменения не закоммичены.

## 2026-10-21 — 3D LUT (texture3d) и безуспешная запись compute

### Найдено и исправлено
- Графический путь привязывал все изображения через `GuestTexture` (только 2D). Сэмплируемые 3D-изображения (6 фрагментных шейдеров с `texture3d`) получали 2D-текстуру, а шейдер ждал 3D. Исправлено: привязка идёт через `ComputeGuestImage` (3D → `GuestTexture3D`).
- `GuestTexture3D` (постоянный кэш по адресу, без повторной загрузки из гостевой памяти): форматы, которые Metal не даёт писать в storage (например `RG11B10Float`, fmt 92), теперь создаются как `RGBA32Float` (лог `[Tex3D] … not storage-writable`).
- Метаданные: 14 3D-текстур создаются; FPS в этом прогоне 29.4.

### Что проверено и не сработало
- Readback LUT-выхода (`0x81b3c80000`, 32×32×32) после диспатча ядра `17aeefc07e25295e`: **0 ненулевых байт** в обоих форматах (RG11B10 и RGBA32Float).
- Проба `KYTY_CS_PROBE=1` + `KYTY_PROBE_HASH=0x17aeefc07e25295e` вставляет безусловную запись константы в `image_33[0]` в начале `main0` — выход остаётся нулевым. Значит запись не доходит до объекта, который читаем.
- Ядро `17aeef` не содержит threadgroup-массивов (в MSL только встроенный `threadgroup_position_in_grid`); ошибка 36864 байт относится к другому ядру.
- Кадр 600 по-прежнему чёрный (0.0%).

### Гипотезы (не проверены)
1. Слот аргумента: `arg_ptrs[33]` записан, но `gpuResourceID` может не совпадать с тем, что читает шейдер (порядок/смещение в аргумент-буфере).
2. Резидентность: `UseResource` помечает активный compute-энкодер; если он создаётся после вызова, текстура не становится резидентной → запись отбрасывается.

### Следующий шаг
1. Логировать `arg_ptrs[33]` и `[texture gpuResourceID]._impl` для этого диспатча и сверить.
2. Вызвать `useResource` непосредственно перед `Dispatch` на текущем compute-энкодере для всех storage-изображений; проверить, меняется ли результат.
3. Если запись всё ещё отбрасывается — проверить, что `MetalContext` не закрывает compute-энкодер между привязкой и диспатчем.

Изменения не закоммичены.

## 2026-10-21 (позже) — ядро 17aeef: выполняется ли и почему запись теряется

### Отладочный счётчик (`KYTY_CS_PROBE=1` + `KYTY_PROBE_HASH=0x17aeefc07e25295e`)
- В MSL ядра добавлен аргумент `device atomic_uint* kyty_dbg [[buffer(26)]]` и инкремент в начале `main0`; буфер 26 — 64 байта, обнуляется перед диспатчем.
- Результат: `[CsDbg] dbg0=32768 expected=32768` (группы 1×16×32, локальный 32×2×1). **Ядро выполняется, все потоки отработали.**
- Объект, привязанный к слоту 33 (`image_33`), и объект, который читает readback, — **один и тот же** (`obj=0x74c6c6ed00`, id=6).
- Безусловная запись константы в `image_33[0]` (проба) не видна в readback: **0 ненулевых байт** в RGBA32Float (16 байт/тексель) и в RG11B10 раньше. Значит запись теряется уже при выполнении на GPU.
- Явный `useResource` для всех storage-текстур диспатча на активном compute-энкодере (`[CsDbg] residency marked`) результат не меняет.

### Что это исключает
- Не выполнение (счётчик полный).
- Не подмена объекта (один и тот же MTLTexture на записи и чтении).
- Не формат RG11B10 (переведён в RGBA32Float, результат тот же).
- Не отсутствие ShaderWrite в usage (стоит).

### Открыто
- Энкодер, на котором реально идёт диспатч: метка резидентности ставилась на `0x…1fe0`, но после `SetComputeBuffer` активный энкодер другой (`0x…2080` на следующем диспатче); после commit активного энкодера нет. Сравнить энкодер диспатча с энкодером, на котором помечены ресурсы.
- Проверить, что `[[id(33)]]` в аргумент-буфере (после санитайзера) совпадает со смещением `arg_ptrs[33]` (в MSL-структуре ids могли быть перенумерованы).
- Проверить запись другим способом: писать в буфер (device) вместо текстуры — если buffer-запись проходит, проблема в texture-аргументе.

### Побочное
- Ошибка `threadgroupMemoryLength(36864) > 32768` относится к другому ядру (не к 17aeef); дамп MSL ещё не сделан.

Изменения не закоммичены.

## 2026-10-21 (ночь) — ядро 17aeef: шейдер видит текстуру 1×1×1

### Ключевое наблюдение (проба `KYTY_CS_PROBE=1` + `KYTY_PROBE_HASH=0x17aeefc07e25295e`)
- Ядро выполняется (`dbg0=32768`), запись константы выполняется, но **шейдер видит размеры слота 33 как `1×1×1`**, а объект, привязанный на хосте, — `32×32×32` (тип 7 = 3D, `fmt=125`, usage=3, Shared).
- Дескрипторы ядра: `kind=0` (буфер) и `kind=33` (один storage-образ, `res0`, дескриптор 32×32) → слот 33. Коллизий слотов нет.
- Значит GPU по значению `[texture gpuResourceID]._impl`, записанному в аргумент-буфер (`arg_ptrs[33]`), разрешает **не наш** ресурс (1×1×1 — похоже на dummy/чужую текстуру).
- Резидентность не объясняет: явный `useResource` на активном compute-энкодере (в т.ч. после `SetComputeBuffer`, на том же энкодере, что `Dispatch`) результат не меняет.

### Что исключено
- Не выполнение (счётчик полный), не объект на записи/чтении (один и тот же), не формат (RGBA32Float вместо RG11B10), не координаты (проба пишет `(0,0,0)`), не ShaderWrite в usage.

### Гипотезы (следующие проверки)
1. Сам handle: `gpuResourceID._impl` записывается в `constant uint64` по смещению `33*8`. Проверить, что MSL читает `image_33` по тому же смещению (после санитайзера id не сдвинуты для этого ядра — сверить с выводом SPIRV-Cross для `kind 33`).
2. Использовать `MTLArgumentEncoder` (encodedLength / setTexture:atIndex:) вместо сырой записи handle — официальный путь для текстур в argument buffer.
3. Сравнить handle, который видит GPU, с handle от `MTLArgumentEncoder` для того же объекта.

### Побочное
- `threadgroupMemoryLength(36864) > 32768` — другое ядро; дамп MSL не сделан.

Изменения не закоммичены.

## 2026-10-21 (поздно ночью) — проверка аргумент-буфера compute

### Проверено (проба на `17aeefc07e25295e`)
- Аргумент-буфер 0 передаётся через `setBytes` (`SetComputeBytes`), то есть **копируется в момент вызова**. Гонки с переиспользованием общего буфера для аргументов нет.
- Значение в слоте 33 непосредственно перед диспатчем: `[CsArg] slot33=5`. Это в точности `[texture gpuResourceID]._impl` объекта (`[CsImg] … id=5`). Записанный handle правильный.
- Дескрипторы ядра: только `kind=0` (буфер) и `kind=33` (один storage-образ). Слоты 49/104 (сэмплеры) у этого ядра не используются, нули там ни на что не влияют.
- Резидентность: `useResource` для текстуры помечается на том же активном compute-энкодере, который потом выполняет `Dispatch` (`[CsDbg] residency marked on active compute encoder`, печать до `pipeline bound`).
- Буфер счётчика, привязанный по ссылке (`SetComputeBuffer`, buffer 26), работает — счётчик `32768` виден.

### Несмотря на это
- Шейдер всё равно видит слот 33 как **1×1×1**, хотя объект 32×32×32. Значит GPU разрешает handle `5` в другой ресурс или в заглушку.

### Оставшиеся гипотезы
1. Handle `gpuResourceID` на Apple-GPU — индекс в таблице ресурсов, валидный для конкретного набора `useResource`; нужен `MTLArgumentEncoder` (`setTexture:atIndex:` + `useResource`) для argument buffer, а не сырое значение.
2. Текстура не попала в список резидентности энкодера, который GPU реально исполняет (проверить через `MTLCommandBuffer` / `MTLCaptureManager` или `MTL_DEBUG_LAYER` с `MTL_DEBUG_LAYER_GPU_ASSERTIONS`).

### Следующий шаг
- Перевести привязку storage/сэмплируемых текстур compute на `MTLArgumentEncoder` (encodedLength и `setTexture:atIndex:` в буфер аргументов) и повторить пробу. Параллельно — прогон с `MTL_DEBUG_LAYER_GPU_ASSERTIONS=1`.

Изменения не закоммичены.

## 2026-10-21 (утро) — причина неверных ресурсов compute: сырые handle в аргумент-буфере

### Найдено и исправлено
- Аргумент-буфер compute писался сырыми значениями `gpuResourceID` по смещению `id*8`. GPU читает аргумент-буфер по **раскладке MSL-структуры**, а не по `[[id(N)]]` (порядок объявления, выравнивание). Поэтому слот 33 попадал не в тот член: шейдер видел 1×1×1 вместо 32×32×32.
- Исправление: compute всегда кодирует аргумент-буфер через `MTLArgumentEncoder` (`[fn newArgumentEncoderWithBufferIndex:0]`, кэш по хэшу, `setTexture:atIndex:` и `setBuffer:offset:atIndex:` по индексам). Буфер 0 биндится через `setBuffer` (`SetComputeBytes` для него не используется). Отключение: `KYTY_CS_ARGENC_OFF=1`.
- Проверка: с энкодером шейдер видит `32x32x32` (как у объекта), проба записи попадает в текстуру (`nonzero_bytes=7` — ровно один тексель `(1.0, 0.5, 0.25, 1.0)` в RGBA32F). **Запись в LUT работает.**

### Что осталось
- Без пробы LUT-выход ядра `17aeefc07e25295e` **всё ещё нулевой** (`nonzero_bytes=0/524288`), кадр 600 чёрный (0.0%). Значит собственная логика ядра не пишет значений: вероятно, выход зависит от входов (буферы, push-константы, bounds-проверки по `spvBufferSizeConstants`), которые дают ранний выход.
- FPS в этом прогоне 22.8.

### Следующий шаг
1. Счётчики внутри ядра (через тот же `kyty_dbg`): сколько потоков доходят до записи; значения размеров (`spvBufferSizeConstants`) и push-констант (`buffer 1`) в начале ядра.
2. Если входы нулевые — проверить привязку буферов ядра (ids по `setBuffer`), а не только текстур.
3. Перенести `MTLArgumentEncoder` на графические VS/PS (тот же класс ошибки: сырой handle в argument buffer), сравнить кадр.

### Env-флаги
- `KYTY_CS_ARGENC_OFF=1` — вернуть сырые handle для compute (для сравнения).

Изменения не закоммичены.

## 2026-10-21 (день) — MTLArgumentEncoder для графики (частично)

### Сделано
- `ShaderHasArgumentBuffer0(hash)` (`metalShader.mm/.h`): при компиляции и загрузке из дискового кэша запоминается, что MSL объявляет дескрипторный буфер на `[[buffer(0)]]`. Без этого `newArgumentEncoderWithBufferIndex:0` падает на ассерте (`bufferIndex 0 does not identify an argument buffer`) — встретилось у PS без дескрипторов.
- Compute: аргумент-буфер через `MTLArgumentEncoder` по умолчанию (`KYTY_CS_ARGENC_OFF=1` — выключить): текстуры, буферы через `setBuffer`. Шейдер теперь видит `32×32×32`; проба записи в LUT работает (`nonzero_bytes=7`).
- Графика, пиксельная стадия (экспериментально, `KYTY_PS_ARGENC=1`): текстуры и сэмплеры через энкодер, буферы и остальное — ещё сырые (`SetFragmentBytes(ps_args)` для всего остального не используется только при успешном энкодере — **буферы пиксельной стадии в этом пути не портированы**). Ассертов нет.

### Результат
- LUT-выход `17aeefc07e25295e` без пробы: **0 ненулевых байт** (`[CsReadback3D] … nonzero_bytes=0/524288`).
- Презентованный кадр 600: **0.0%** ненулевых (чёрный).
- FPS: ~18–23 в этих прогонах.

### Не сделано
- Вершинная стадия (VS) через энкодер — не портирована; `SetVertexBytes(vs_args)` остаётся.
- Буферы пиксельной стадии и push-константы — не портированы в энкодер.
- Внутренние входы ядра `17aeef` (буферы, `spvBufferSizeConstants`) — не проверены; без них неясно, почему логика ядра даёт ноль.

### Следующий шаг
1. Счётчики в ядре `17aeef` (через `kyty_dbg`): доходят ли потоки до записи, значения `spvBufferSizeConstants` и push-констант в начале ядра.
2. Портировать буферы пиксельной стадии и VS в энкодер (те же `setBuffer`), затем проверить кадр 600.

Изменения не закоммичены.

## 2026-10-21 (вечер) — входы ядра 17aeef: буфер валиден

### Факты
- Ядро читает вход как `buffers[0]->_m0[…]`: указатель на буфер хранится по смещению 0 аргумент-буфера (через `reinterpret_cast` от `spvBufferSizeConstants`, id 0). `setBuffer(…, atIndex:0)` пишет именно этот указатель — раскладка совместима.
- Содержимое буфера 0 (гостевой адрес `0x8190c80000`, 20 байт): **5 из 5 dword ненулевые** — `3f800000 3f800000 42c80000 435edc67 00000001` (float: 1.0, 1.0, 100.0, ~222.2, uint 1). Похоже на реальные параметры.
- Значит входы корректны и доходят до ядра. Нули в LUT возникают в логике ядра (ранний выход, условие на `spvBufferSizeConstants`, второй вход или push-константы), а не в привязке.

### Поправка к предыдущему плану
- Буферы compute уже идут через `setBuffer` в энкодере (не сырыми адресами). Отдельного «портирования буферов» для compute не требуется.

### Открыто
- Почему логика ядра даёт ноль при валидных входах: нужны счётчики ветвлений внутри ядра (через `kyty_dbg`) и проверка `spvBufferSizeConstants` (размеры, по умолчанию 65536 в коде) против индексов чтения.
- Вершинная стадия и буферы пиксельной стадии в энкодер — не портированы; пиксельная стадия через энкодер по-прежнему только экспериментальная (`KYTY_PS_ARGENC=1`).
- Кадр 600 чёрный (0.0%).

Изменения не закоммичены.

## 2026-10-21 — объём порта графики в MTLArgumentEncoder (не выполнен целиком)

### Почему не одним шагом
- Легаси-биндеры `BindLegacyStage` / `BindMslStage` заполняют `vs_args`/`ps_args` из разных источников: вершинные буферы (`args[base+i]`, i<5, адреса через `GuestGpuAddress`), буферы стадии (`GetBufferForGuestRange`), SRT (индекс 53), изображения (`image_base+i`), сэмплеры (49, 104) с дефолтами. Каждый буфер нужен как `MTLBuffer` + смещение, а не сырой адрес.
- Включение по умолчанию без поэлементной проверки заменит рабочий сырой путь графики для всех draw'ов сразу; проверить это за один прогон нельзя (кадр 600 уже чёрный, регрессию не видно).

### Что уже сделано
- Пиксельная стадия: изображения и дефолтный сэмплер через энкодер (`KYTY_PS_ARGENC=1`), без ассертов; буферы пиксельной стадии — сырые.
- Compute: энкодер по умолчанию, буферы через `setBuffer`.

### План (по шагам, с проверкой после каждого)
1. Пиксельная стадия: буферы стадии через `GetBufferForGuestRange` → `setBuffer` (те же источники, что в `BindGuestBuffer`).
2. Вершинная стадия: изображения и сэмплеры через энкодер; затем вершинные буферы (`vertex_buffers[i]`) → `setBuffer` с `MTLBuffer`+смещением.
3. SRT (индекс 53) → `setBuffer`.
4. Убрать сырую запись `vs_args`/`ps_args` для графики, когда каждый шаг проверен.
5. Прогон с `KYTY_DUMP_FRAME=600`: ненулевые проценты и средний цвет кадра.

### Открыто
- Ядро `17aeefc07e25295e` (LUT): входы (буфер 0) валидны; если оно читает текстуры/сэмплеры из предыдущих проходов — их вход зависит от графического пути, проверить после пункта 4.

Изменения не закоммичены.

## 2026-10-21 (порт графики: итог шагов)

### Сделано (за флагом `KYTY_ARGENC_GFX=1`, легаси-путь остаётся запасным)
- Захват привязок: каждая запись легаси-биндеров в `vs_args`/`ps_args` дополнительно пишется как объект (буфер+смещение, текстура, сэмплер) по тому же индексу (`s_ps_caps`, `s_vs_caps`). Покрыты: буферы стадии и вершинные буферы (через `GuestGpuAllocation`), SRT (индекс 53), изображения (`image_base+i` и `64+i`), дефолтные текстура и сэмплер.
- Построитель `build_gfx_argbuf`: `MTLArgumentEncoder` по `newArgumentEncoderWithBufferIndex:0`, буфер аргументов из `encodedLength`, буферы через `setBuffer:offset:atIndex:`, текстуры и сэмплеры по индексу. Биндинг: `SetFragmentBuffer` / `SetVertexBuffer` вместо `SetFragmentBytes(ps_args)` / `SetVertexBytes(vs_args)`.
- Условие: только если `ShaderHasArgumentBuffer0(hash)` (иначе легаси).
- Проверка по элементам (`KYTY_ARGENC_CHECK=1`): для каждого индекса берётся последний захват; сравниваются объект (`gpuAddress+offset` или `gpuResourceID`) и сырое значение легаси. **PS: 0 расхождений, VS: 0 расхождений, ассертов нет.**

### Результат кадра 600 (оба стадии на энкодере)
- Ненулевые пиксели: **0.0%**, средний цвет (0, 0, 0). Кадр чёрный.
- FPS ~22. Флаг **не сделан по умолчанию** — критерий «кадр не чёрный» не выполнен.

### Вывод
- Привязка ресурсов графики теперь эквивалентна легаси (проверено по элементам). Сама по себе она не даёт картинку: причина чёрного кадра — в содержимом (шейдеры/входы/RT), а не в раскладке аргументов.
- Следующий шаг: найти, какие draw'ы пишут в RT, которые презентуются, и какие входы у них нулевые (счётчики и readback по draw, как для LUT). LUT-ядро перепроверить после этого (его входы — из графического пути).

Изменения не закоммичены.

## 2026-10-21 (вечер) — draw'ы f4cac03b183e6196 не пишут в RT: отсечение

### Проверено
- Размер текстуры, которую видит шейдер (проба `dims`, `KYTY_PS_PROBE=dims`): в RT после draw значение не появилось — draw не записывает ни пикселя (тот же результат, что и для красной пробы). Проба применяется (`returns=2`), RT с точным форматом (`rt=…ef80`, RGBA8, `mtl=70`) — тот же объект, что и обычный.
- **Отключение отсечения** (`KYTY_NO_CULL=1`): после draw в RT появляется содержимое — **4.4% ненулевых** (средний цвет 11/0/11). Значит draw отсекается растеризатором по ориентации треугольников.
- Гипотеза «Y-флип виртуального вьюпорта инвертирует обмотку»: введён экспериментальный флаг `KYTY_FLIP_WINDING=1` (инверсия `setFrontFacingWinding` при отрицательной высоте вьюпорта). **Сам по себе результат не меняет** (0.0%). По умолчанию выключен.

### Открыто
- Почему с отсечением draw не виден, а без отсечения виден только частично (4.4%): возможно, ещё одна ошибка (cull mode vs winding, порядок вершин, клиппинг по viewport/scissor).
- Шаг 2 (проход за проходом: доля ненулевых на выходе каждого прохода и диспатча в кадре 600) — не реализован; нужен readback RT/storage в конце каждого прохода.

### Флаги, добавленные в этом шаге
- `KYTY_FLIP_WINDING=1` — инверсия обмотки при Y-флипе вьюпорта (экспериментально, по умолчанию выкл.).
- `KYTY_ARGENC_GFX=1` (диагностика, по умолчанию выкл.), `KYTY_ARGENC_CHECK=1` — проверка захваченных привязок по элементам (PS и VS: 0 расхождений).
- `KYTY_PS_PROBE=dims` — размеры текстуры, видимые шейдеру.

Изменения не закоммичены.

## 2026-10-21 (поправка) — вьюпорт и обмотка

### Факты
- `SetViewport` уже переводит отрицательную высоту в положительную (сдвиг origin + инверсия высоты) до вызова Metal: MTLViewport получает положительную высоту. Вьюпорт не «отрицательный» на стороне Metal.
- Такое преобразование эквивалентно Vulkan-варианту с отрицательной высотой: оба дают одно и то же зеркальное отображение окна, поэтому обмотка в оконных координатах уже совпадает с Vulkan. **Инверсия обмотки при флипе ошибочна.** Флаг `KYTY_FLIP_WINDING` оставлен выключенным, по умолчанию не влияет (результат: 0.0%).
- Трансляция отсечения и обмотки соответствует битам гостя: `face` (1 = CW), `cull_front` → Front, `cull_back` → Back (`TranslateCullMode`, `TranslateWinding`).
- Без отсечения draw `f4cac03b183e6196` даёт 4.4% пикселей; с отсечением — 0. Значит этот draw, вероятно, по замыслу гостя отсекается (или не является основным писателем презентуемого RT). Это не подтверждено.

### Не сделано
- Проверка диапазона глубины (reversed-Z, `zmin/zmax` → `[0,1]`) — не делалась.
- Шаг 2 (доля ненулевых на выходе каждого прохода и диспатча в кадре 600) — основной следующий шаг: найти первый проход с нулевым выходом при ненулевых входах.

Изменения не закоммичены.

## 2026-10-21 (rect lists)

### Факты
- Metal: `TranslatePrimitiveType` переводит `kRectList` и `kRectListLegacy` в `MTLPrimitiveTypeTriangle` без расширения. Поэтому каждый прямоугольник рисуется **одним треугольником** (вершины 0,1,2) — вторая половина пропадает, а единственный треугольник может оказаться обратным и отсекаться.
- Vulkan: rect list идёт через тесселяцию (patch list), то есть четвёртый угол восстанавливается. Диагностика `KYTY_RECTLIST_AS_TRI=1` в Vulkan тоже рисует только первый треугольник — то есть текущее поведение Metal совпадает с этой диагностикой, а не с рабочим путём Vulkan.
- Отсечение у draw `f4cac03b183e6196` (без отсечения 4.4% пикселей, с отсечением 0) объясняется именно этим: одиночный треугольник от прямоугольника.

### Что нужно для правильного фикса
- Четвёртый угол прямоугольника: для rect list с тремя вершинами на прямоугольник v3 = v0 + v2 − v1 (в пространстве вершин до проекции). Metal не умеет генерировать вершину из индексов, поэтому нужно либо расширение в вершинном шейдере (по `vertex_id`), либо тесселяция/геометрия на стороне Metal, либо CPU-расширение вершинного буфера (6 вершин на прямоугольник).
- Счётчик rect list на кадр не добавлен (вывод `[MetalRenderer] DrawAuto` печатает только каждые 500 вызовов).

### Следующий шаг
1. Выбрать путь расширения (рекомендуется CPU-расширение буфера для rect list: ring-буфер на draw, 6 вершин на прямоугольник, триангулярный список).
2. Проверить f4cac03b183e6196 без отсечения и с отсечением.
3. Вернуться к проходу-за-проходом локализации (кадр 600).

Изменения не закоммичены.

## 2026-10-21 (кадр 0–3, первая картинка)

### Факты
- Кадры 0–3 в Metal: по одному draw на кадр — `DrawAuto prim=6 (kTriStrip) vtx_count=4` в RT `0x8240000000` (fmt 92, RG11B10). Это полноэкранный квад (два треугольника стрипом). Презентуемый RT тот же.
- Без отсечения (`KYTY_NO_CULL=1`) кадры 0–1 всё равно чёрные (0.0%) — отсечение не причина.
- Глобальная красная проба PS (`KYTY_PS_PROBE=red`, без отсечения): кадры 0–1 становятся **100% красными** (255,0,0). **Растеризация и запись в RT работают.**
- Значит шейдер первого draw (PS `0b29d645e67744ca`, без изображений) отдаёт ноль. Его данные — пользовательские регистры (user data): в первом draw снимок `pud=[00000000 ×4]`, объявлено 4 регистра (`pn=[n=4,base=0,regs=4]`). То есть константы не доходят до шейдера.

### Открыто
- Откуда Vulkan-путь берёт вход того же PS: если тот же снимок user data нулевой, то Vulkan читает константы из другого источника (SRT/`flattened_srt` или буфер по указателю), а Metal — нет. Сверить для того же draw: какие значения user data и SRT видит Vulkan (`KYTY_BIND_TRACE` в Vulkan-сборке, та же строка `BINDVK`).
- Сравнить `ps_info.stage.resources->user_data` и `flattened_srt` для этого PS: в Metal `push_data` строится только из `user_data`, SRT — отдельным буфером индекса 53 (только если `flattened_srt` непуст).

### Следующий шаг
1. Для PS `0b29d645` в Vulkan-сборке (≈ первый кадр): значения `shader_data` и источник (user data или SRT).
2. Если Vulkan берёт SRT, а Metal — user data (или наоборот), исправить источник в `TranslateRasterizerState`-соседнем месте: выбор между `snap.user_data` и `snap.flattened_srt` по тому же правилу, что в `descriptors.cpp` (`prepared.shader_data`).

Изменения не закоммичены.

## 2026-10-21 (кадр 1: пользовательские регистры PS)

### Факты (Metal-сборка)
- Первый draw (квад, PS `0b29d645e67744ca`): `ps.user_data_addr=0x0`, `ps.data_addr=0x8180001400` (база шейдера валидна), `vs.es.data=0x8180001200`. Пользовательские SGPR PS (`SetPsUserSgpr`) в снимке нулевые (`pud=[00000000 ×4]`).
- `SetPsUserSgpr` сохраняет значение независимо от типа-маркера — отбрасывания по маркеру нет.
- `Shader::Reset()` вызывается только при инициализации (`graphicsRun.cpp:428`), не на каждом draw — пользовательские регистры не стираются между draw'ами.
- Вывод: до первого draw гость не записал PS-SGPR/указатель в контекст шейдера, либо записи PM4 для PS не доходят до контекста. Шейдер читает константы через эти регистры → нули → чёрный выход.

### Открыто (нужно сравнить с Vulkan)
- Совпадает ли первый draw в Vulkan-сборке (тот же PM4). Если в Vulkan PS-SGPR тоже нулевые — картинка в Vulkan берётся из другого draw; если ненулевые — запись PM4 для PS теряется в Metal-пути.
- Проверка: в PM4-потоке до первого draw посчитать `SET_SH_REG` в диапазоне `SPI_SHADER_USER_DATA_PS_0..31` (0x0C..0x2B) и `SPI_SHADER_USER_DATA_ADDR_LO_PS` и их значения в обеих сборках.

### Следующий шаг
1. Лог `HwShSetPsUserSgpr` (регистр, значение, индекс draw) и `pm4Handlers` для `ADDR_LO_PS` — в обеих сборках, первые 3 кадра.
2. Сравнить построчно; первое расхождение — точка сломки.

Изменения не закоммичены.

## 2026-10-21 (пользовательские регистры PS: сравнение с Vulkan)

### Факты
- Vulkan и Metal читают пользовательские регистры шейдера из **одного и того же** места: `ResourceSnapshot.user_data` (= `runtime.user_data` = `params.user_data` кэша конвейеров). Отдельной копии в бэкенде Metal нет; `PrepareBindings` (Vulkan) и push-data (Metal) берут значения из одного снимка.
- Первый PS-draw в Vulkan-сборке (кадры 0–2): `ud=[00000000,00000000,00000000,00000000]` — **то же самое, что в Metal**. Значит нулевые пользовательские регистры — общее состояние гостя на этом draw, а не ошибка Metal-бэкенда.
- Вариант шейдера в Vulkan другой (`P=d502ac0a3a6f47ad` против `0b29d645…` у Metal): спецификация может убирать зависимость от этих регистров, или первая картинка Vulkan берётся из другого draw.

### Гипотеза «Metal читает другой источник» — опровергнута
- Источник один и тот же; значения одинаковые.

### Открыто
- Какой draw даёт видимую картинку в Vulkan (первый или более поздний) и какой вариант PS его рисует. Сопоставить draw'ы по RT-адресу и по порядку, а не по хэшу.
- Почему PS `0b29d645` в Metal не получает константы, если они не записаны гостем до первого draw (`LOAD_SH_REG`/indirect-пакеты — проверить, что обрабатываются в общем CP-коде).

Изменения не закоммичены.

## 2026-10-21 (поиск первого непустого кадра в Vulkan)

### Блокер
- Измерить кадры Vulkan-сборки нельзя: `KYTY_DUMP_FRAME` реализован только в Metal-презентере (`metalPresenter.mm`). В Vulkan-пути (`presentation/window/swapchain.cpp`) нет чтения презентуемого изображения ни для дампа, ни для статистики (`KYTY_DUMP_RTS_FRAME` там дампит только Metal-RT).
- `screencapture` ранее не работал (нет разрешения Screen Recording), поэтому внешний снимок окна не используется.

### Что нужно
- Добавить в Vulkan-презентацию env-гейтированный дамп `KYTY_VK_DUMP_FRAME=<n>`: после блита в swapchain-изображение скопировать его в host-видимый буфер (`vkCmdCopyImageToBuffer`), дождаться, записать PPM. Тот же формат, что у Metal-дампа, чтобы `rt_stats.py` работал без изменений.
- Затем: Vulkan — кадры 1..60, первый кадр с ненулевым выходом → F; Metal — на том же кадре F построчно по draw'ам (non-zero после каждого).

### Следующий шаг
1. Реализовать `KYTY_VK_DUMP_FRAME` в Vulkan-презентации (swapchain.cpp, рядом с Present).
2. Прогнать Vulkan-сборку, найти F.
3. Сопоставить draw'ы кадра F по RT-адресу и порядку с Metal.

Изменения не закоммичены.

## 2026-10-21 (KYTY_VK_DUMP_FRAME — подготовка, не реализовано)

### Что известно о Vulkan-презентации (`presentation/window/swapchain.cpp`)
- `Presenter::Impl::Present()`, Vulkan-ветка (после `#endif`, около строки 1257): `AcquireNextImage` → `RecordPresentCommands(command, layers[0].frame, layers[1], overlay)` → `swapchain.Submit(present_scheduler)` → `swapchain.Present()`.
- В `RecordPresentCommands` изображение свопчейна `m_images[m_image_index]` переходит `eUndefined → eTransferDstOptimal`, затем туда пишется блит кадра; финальный layout — при `Present` (нужно уточнить, обычно `ePresentSrcKHR`).
- Расширение `Swapchain` для дампа: нужны `m_images[m_image_index]`, размер (`m_extent`), устройство (`m_window.graphic_ctx.device`) и очередь. Команды записываются через `present_scheduler.BeginCommand()`.

### План изоляции (чтобы не сломать Vulkan-оракул)
- Отдельные `vk::CommandPool` + `vk::Fence` + буфер (host-visible), создаются лазейно только при `KYTY_VK_DUMP_FRAME`.
- После `Submit` и до `Present`: ожидание тика (`present_scheduler`), barrier `→ eTransferSrcOptimal`, `copyImageToBuffer`, ожидание fence, запись PPM (как у Metal), статистика ненулевых и среднего цвета, восстановление layout.
- Режим `KYTY_VK_DUMP_FRAME=first-nonblack`: дамп каждого кадра до первого ненулевого, затем стоп.

### Статус
- Код не написан: нужно прочитать `CommandScheduler`/`Submit`/`Present` (порядок тиков и layout после блита) и только потом добавлять дамп. Риск: сломать путь, который сейчас служит эталоном.

Изменения не закоммичены.

## 2026-10-21 (первый draw с ненулевой выборкой: кандидат на заставку)

### Сканирование (`KYTY_SCAN_TEX=1`, кадры 1..300, Metal)
- Первая выбираемая текстура с реальным содержимым: `0x819c000000`, 3840×2160, fmt 56, tile 5, **100% ненулевых байт** — кадр **20**.
- Она достигается через SRT пиксельной стадии (`[SrtData] base=55 words=16 nz=13`), то есть через указатель пользовательских регистров, а не через прямую привязку — поэтому в строках `BIND` нет `P:img` для этого draw.
- Draw-кандидат (кадр 20): пиксельный шейдер `724318af5ff62412` (вершинный `3d28ce5308b9b56a`); пользовательский указатель `pud=[7592f3b0, 00000081]` → адрес `0x817592f3b0` — ненулевой.

### Проба красным (`KYTY_PS_PROBE=red`, только этот шейдер)
- Презентуемые кадры 20 и 21: **0.1% ненулевых**, средний цвет (0.2, 0, 0). Draw пишет, но в презентуемый RT попадает малая доля.
- Вывод: либо этот draw пишет в другой RT (не тот, что презентуется), либо покрывает малую область. Не выяснено.

### Следующий шаг
1. Для draw кадра 20: `rt0` (адрес, формат), viewport/scissor, и совпадает ли RT с презентуемым (`last_rt`).
2. Если RT другой — понять, какой RT презентуется, и почему заставка идёт в другой; если та же — сравнить покрытие (vtx, prim).
3. Затем: пробы dims и выборки на фиксированных UV для текстуры `0x819c000000` через SRT.

Изменения не закоммичены.

## 2026-10-21 (кандидат-draw кадра 20: RT и покрытие)

### Native 4K (без `KYTY_VIDEO_OUT_1080P`)
- Презентуемые кадры 25/30/35/40 — 3840×2160, при пробе красным на шейдер `724318af` покрытие 0.0–0.1%. Разрешение не причина.

### Кандидат-draw кадра 20 (шейдер `724318af5ff62412`)
- `rt0` = `0x8240870000`, 1920×1080, RGBA8 (`mtl=70`) — тот же RT, что презентуется в кадре 20 (`last_rt`).
- Вьюпорт 1920×1080 (с обычным Y-флипом), scissor полный `[0,0,1920,1080]`, блендинг выключен (`blend=0`), маска записи `0xf`, глубина/трафарет выключены.
- Пробой красным: после draw в RT **0.2% ненулевых**, средний (0.6, 0, 0). То есть draw пишет, но покрывает малую долю экрана.

### Вывод
- RT, вьюпорт, маска и блендинг для этого draw корректны. Малое покрытие означает, что геометрия маленькая или вырожденная: вероятно, вершинные данные (позиции из вершинного буфера) читаются неверно или нулевые.

### Следующий шаг
1. Для VS этого draw: вершинные буферы (`vs_info.buffers`: адрес, stride, num_records), содержимое первых 12 dword первого буфера (позиции) — ненулевые ли, и совпадают ли размеры с ожидаемыми.
2. Если позиции нулевые — проверить привязку вершинного буфера (энкодер/сырой путь) и источник адреса; если ненулевые — проверить трансформацию (`viewport`, `w`).

Изменения не закоммичены.

## 2026-10-21 (вершинные буферы кандидат-draw кадра 20)

### Факты
- Вершинные буферы VS у кандидата идут через легаси-биндер стадии (`BindLegacyStage`, буферы стадии), не через `vertex_buffers` (там `[VB]` не срабатывает: у draw нет вершинных буферов в этом пути).
- Сверка (`KYTY_TRACE_VB=1`, кадры 20..21, `[VB2]`): для всех 12 первых буферов содержимое MTLBuffer по смещению **побайтно совпадает** с гостевой памятью (`diff=0`), `backing_ok=1`. Привязка буферов корректна.
- Содержимое буферов — константы, не поток вершин: `1080.0` (0x44870000), `1920.0` (0x44f00000), `1/1920` (0x3a088889), `1/1080` (0x3a72b9d6), `-1.0`-подобные `0xffffffff`. То есть шейдер строит позиции из `vertex_id` и этих масштабов.

### Вывод
- Привязка и данные буферов верны. Вырожденная геометрия (покрытие 0.2%) вероятнее всего возникает в вычислении позиции из `vertex_id` (например, `vertex_id` равен одному значению для всех вершин, или база `vertex_id` сдвинута), а не в вершинных данных.

### Следующий шаг
1. Проба VS: записать вычисленную позицию для `vertex_id` 0..3 (через debug-буфер на индексе, не атомики — по `vid` как индексу) и сравнить с ожидаемым квадом (±1 в NDC).
2. Проверить, что `vid` в VS приходит как 0..3 (а не 0 для всех), и значение `first_vertex` в `Draw`.

Изменения не закоммичены.

## 2026-10-21 (тип примитива кандидат-draw)

### Факты
- Кандидат (пиксельный `724318af5ff62412`, RT `0x8240870000`) — **triangle list, 6 вершин** (`prim=4 vtx=6`). Не rect list: гипотеза о неполном прямоугольнике для него не подтверждается.
- Первый draw кадра 0–3 — triangle strip, 4 вершины (`prim=6 vtx=4`), RT `0x8240000000`.
- Номера кадров между прогонами не стабильны (тайминг потоков): прив язка draw к «кадру 20» в одном прогоне может не совпадать с другим. Для сопоставления использовать хэш PS + RT в одном и том же прогоне.

### Вывод
- Для кандидата вершинные буферы верны (побайтно), RT/вьюпорт/маска/блендинг верны, покрытие 0.2%. Тип примитива — список треугольников из 6 вершин. Если VS строит квад по `vertex_id` формулой, рассчитанной на strip или rect, то 6 вершин дают вырожденные треугольники.

### Следующий шаг
1. Проба VS для кандидата: записать `vertex_id` и выходную позицию для вершин 0..5 в debug-буфер (по индексу `vid`), сравнить с ожидаемым квадом. Нужна привязка debug-буфера в VS-энкодер (аналог пробы CS, но для вершинной стадии).
2. Если позиции вырожденные — сверить формулу позиции из MSL с гостевыми константами (1920/1080, обратные).

Изменения не закоммичены.

## 2026-10-21 (проба вершинной стадии кандидата: позиции верны)

### Результат пробы VS (`KYTY_VS_DBG=0x3d28ce5308b9b56a`, draw кандидата PS `724318af5ff62412`, RT `0x8240870000`)
| id | inst | позиция (x, y, z, w) |
|---|---|---|
| 0 | 0 | (-1, 1, 1, 1) |
| 1 | 0 | (1, 1, 1, 1) |
| 2 | 0 | (1, -1, 1, 1) |
| 3 | 0 | (-1, -1, 1, 1) |
| 4 | 0 | (-1, 1, 1, 1) |
| 5 | 0 | (1, -1, 1, 1) |

- `vertex_id` = 0..5 (правильно), позиции — полноэкранный квад в NDC (два треугольника по диагонали), z = w = 1.
- `ZRange`: znear=0, zfar=1, zscale=1, zoffset=0 — диапазон глубины нормальный.
- Порядок вызовов: `SetRasterizerState` → `SetViewport` (так что инверсия обмотки при флипе действует).
- Инверсия обмотки (`KYTY_FLIP_WINDING=1`) для этого draw: покрытие **по-прежнему 0.2%** (средний (0.6, 0, 0)). Значит отсечение не объясняет вырождение.

### Вывод
- Вершинная стадия, буферы, вьюпорт, глубина, scissor, RT — корректны. Покрытие 0.2% при полноэкранном квадрате означает, что фрагментная стадия этого draw не выполняется (или её результат не попадает в RT).

### Следующий шаг
1. Счётчик запусков фрагментного шейдера для этого draw: debug-буфер на PS (индекс свободный), atomic-инкремент в начале PS (как для CS), читать после коммита.
2. Если счётчик 0 — проверить, что `pstate` (пайплайн) создан для этой пары VS/PS и что draw реально кодируется (`[Draw] ok` для этого вызова).
3. Если счётчик полный — проверить, куда пишет PS (`out_mrt_0`, маска, порядок RT в энкодере).

Изменения не закоммичены.

## 2026-10-21 (матрица проб и GPU-захват)

### Таблица (кадры 20 / 100 / 600: доля ненулевых пикселей и средний цвет RGB; cold MSL cache; 120 с)

```
baseline      f20=[0.0%/0.0,0.0,0.0] f100=[0.0%/0.0,0.0,0.0] f600=[0.0%/0.0,0.0,0.0] vsrows=0 asserts=0 FPS=10.5
argenc        f20=[0.0%/0.0,0.0,0.0] f100=[0.0%/0.0,0.0,0.0] f600=[0.0%/0.0,0.0,0.0] vsrows=0 asserts=0 FPS=10.5
no_cull       f20=[0.0%/0.0,0.0,0.0] f100=[0.0%/0.0,0.0,0.0] f600=[no-dump] vsrows=0 asserts=0 FPS=10.4
no_depth      f20=[0.0%/0.0,0.0,0.0] f100=[0.1%/0.2,0.0,0.0] f600=[0.0%/0.0,0.0,0.0] vsrows=0 asserts=0 FPS=11.4
vs_pos        f20=[0.0%/0.0,0.0,0.0] f100=[0.0%/0.0,0.0,0.0] f600=[0.2%/0.4,0.4,0.4] vsrows=6 asserts=0 FPS=10.4
ps_red        f20=[0.1%/0.2,0.0,0.0] f100=[0.1%/0.2,0.0,0.0] f600=[0.0%/0.0,0.0,0.0] vsrows=0 asserts=0 FPS=10.4
ps_tex        f20=[0.0%/0.0,0.0,0.0] f100=[0.0%/0.0,0.0,0.0] f600=[0.0%/0.0,0.0,0.0] vsrows=0 asserts=0 FPS=10.4
ps_uv         f20=[0.1%/0.1,0.2,0.0] f100=[0.1%/0.1,0.2,0.0] f600=[0.0%/0.0,0.0,0.0] vsrows=0 asserts=0 FPS=10.4
ps_cb         f20=[0.0%/0.0,0.0,0.0] f100=[0.0%/0.0,0.0,0.0] f600=[0.0%/0.0,0.0,0.0] vsrows=0 asserts=0 FPS=10.5
msl_direct2   f20=[0.0%/0.0,0.0,0.0] f100=[0.0%/0.0,0.0,0.0] f600=[4.4%/11.3,0.0,11.3] vsrows=0 asserts=0 FPS=10.5
no_cull       f20=[0.0%/0.0,0.0,0.0] f100=[0.0%/0.0,0.0,0.0] f600=[0.0%/0.0,0.0,0.0] vsrows=0 asserts=0 FPS=10.4
```

### Вывод по таблице
- Ни одна конфигурация не даёт непустой кадр 20 или 100 (кроме слабых следов ~0.1%).
- Единственное заметное значение — `msl_direct2` на кадре 600 (4.4%, цвет (11, 0, 11)). Тот же «магентовый» след, что был с отсечением, выключенным на ранней попытке; при `no_cull` на кадре 600 чёрный (0.0%), так что след не объясняется одним отсечением.
- `vs_pos`: позиции VS корректны (6 строк), покрытие кадра 600 — 0.2%.
- Пробы PS (red/tex/uv/cb) для кандидата дают 0.0–0.1%: фрагментная стадия кандидата либо не запускается, либо её результат не попадает в презентуемый RT.

### GPU-захват (Xcode)
- Запуск с `MTL_CAPTURE_ENABLED=1 KYTY_GPU_CAPTURE=<flip>`. Захват от начала кадра N до начала N+1.
- Файлы: `/Users/tx/Developer/fex-spike/runs/frame20.gputrace` (1.6 ГБ, кадр 20 — кандидат-заставка) и `frame600.gputrace` (2.3 ГБ).
- Открыть: `open -a Xcode /Users/tx/Developer/fex-spike/runs/frame20.gputrace` (или двойной клик в Finder). В Xcode: список вызовов, привязанные ресурсы каждого draw/dispatch, геометрия вершинного выхода, вложения.

### Скрипт
- `tools/msl/probe_matrix.sh [индексы]` — конфигурации из массива `CONFIGS`, холодный кэш MSL, `RUN_SECONDS` (по умолчанию 80; для кадра 600 использовать 120). Вывод: `tools/msl/out/summary.txt`, логи и дампы в `tools/msl/out/<имя>/`.
- `tools/msl/rt_stats.py` — статистика PPM (копия из `/tmp`).

### Открыто
- Ни одна проба не даёт картину; следующий шаг — по `frame20.gputrace` посмотреть фрагментные вызовы кандидата (запускаются ли они, какой RT и порядок вложений) и вершинный выход.

Изменения не закоммичены.

## 2026-10-21 (кандидат-VS main0(9): проверка «x == y»)

### Итог
- Гипотеза «VS вырождает квад (x == y)» **не подтверждена**. Прогон транслированного MSL (`/tmp/msl_dump/shader_3d28ce5308b9b56a.metal`, конфиг `vs_raw`, без `KYTY_VS_PROBE`) даёт по `vertex_id` 0..5:
  - id0 (-1, 1), id1 (1, 1), id2 (1, -1), id3 (-1, -1), id4 (-1, 1), id5 (1, -1), z=w=1.
  - Два треугольника (0,1,2) и (3,4,5) одной ориентации покрывают весь экран. Совпадение x == y у id1 и id3 — не вырождение.
- Viewport 1920×1080 (flipped, y=0), scissor полный, cull=0, fill=solid, colour write mask=0xF, blend off, RT0 RGBA8Unorm (mtl=70) совпадает с форматом пайплайна. Глубина: Depth Always, Write No (по замечанию из сессии — ок).
- Несмотря на корректные вершины, RT0 после draw пустой (0.0% ненулевых). Пробный `ps_red` (константный красный) тоже пустой; `vs_pos` (большой треугольник на весь экран) даёт 0.2%. Значит фрагментная стадия не пишет, а не VS.

### Новая зацепка: несовпадение argument buffer
- Metal API validation (`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`) выдаёт:
  `-[MTLDebugArgumentEncoder setBuffer:offset:atIndex:] failed assertion: index (1) is outside of the valid index range [0, 0]`.
- Причина в MSL, который отдаёт SPIRV-Cross (путь по умолчанию): `struct spvDescriptorSetBuffer0` объявляет только `spvBufferSizeConstants [[id(0)]]`, а массив `buffers [[id(1)]][4]` закомментирован как «Overlapping binding». Вершинные буферы читаются через `reinterpret_cast` от адреса члена `spvBufferSizeConstants`, то есть `buffers[0]` попадает на указатель размеров, а не на V#-буфер. Хост же пишет буферы в индексы 1..4 своего энкодера.
- Путь MslEmitter (`KYTY_MSL_DIRECT=2`) объявляет `buffers [[id(kMslBufferBase)]][64]` без перекрытия; на нём чёрный экран тоже сохраняется (`msl_direct2`, кадр 600 — 4.4%), так что это не единственная причина.
- Следующий шаг: вынести буферы в непересекающийся id (SPIR-V binding/argument-buffer layout в `metalShader.mm`, опции SPIRV-Cross) и проверить валидацию снова. Пока не подтверждено, что это устраняет чёрный экран.

### Инструменты
- `KYTY_LOG_VP=1` — логирует viewport/scissor/cull per draw (`[VpView]`, `[VpScissor]`, `[VpRast]`). Ключ по умолчанию выключен.
- `tools/msl/probe_matrix.sh`: конфигурация `vs_raw` (индекс 5) — VS-строки без переопределения позиций.

Изменения не закоммичены.

### Несоответствие ABI argument buffer (уточнение, 2026-10-21)
- Хост (`metalRenderer.mm`, `vs_args[256]`/`ps_args[256]`, `build_gfx_argbuf`) пишет ресурсы по раскладке MslEmitter: буферы `0..63`, образы `64+`, сэмплеры `128+`, SRT `160`.
- Путь по умолчанию — SPIRV-Cross (`metalShader.mm`, `CompileSpirvToFunction`, без `add_msl_resource_binding`): ids назначаются по binding SPIR-V. Пример: `image_3` на id 3, `spvBufferSizeConstants` на id 0, массив `buffers` закомментирован (перекрытие id 1..4 с `image_3`/сэмплерами).
- Результат: `setBuffer:atIndex:` с индексом вне диапазона энкодера ([0,0]) — валидация Metal это ловит; буферы вершинной выборки не попадают в argument buffer, а образы по раскладке хоста могут не совпадать с id SPIRV-Cross.
- Оговорка по данным: строки `[VsRow]` читаются с GPU из отладочного буфера (`KYTY_VS_DBG`), а не вычисляются вручную. Они дают корректный квад (см. выше). Значит, либо отладочный прогон отличается от захвата (другой MSL из-за `KYTY_VS_DBG`), либо значения vertex fetch на GPU совпадают с ожидаемыми. Это нужно сверить: сравнить `[VB]`/`[VB2]` в захватном прогоне без `KYTY_VS_DBG`.
- Исправление требует выравнивания id SPIRV-Cross с раскладкой хоста: `add_msl_resource_binding` для каждого дескриптора (буферы → 0.., образы → 64+, сэмплеры → 128+) и заполнение всех элементов в энкодере. Это изменение ABI между SPIR-V-эмиттером, SPIRV-Cross и хостом; в коммит не вносилось.

### KYTY_SPVC_ABI: карта id (не реализовано, 2026-10-21)
- Раскладка SPIRV-Cross = NativeBinding: id = kind + group×55 (group: VS 0, PS 1). Проверено по `shader_724318af5ff62412.metal` (PS): `spvBufferSizeConstants [[id(0)]]`, `buffers [[id(55)]][1]`, `image_3 [[id(58)]]`, `samplers [[id(104)]]`. Хост `BindLegacyStage` (PS base 55, image_base 56, samplers base+49) пишет тот же набор.
- Ломает другое: массив из N элементов занимает N подряд идущих id. VS `3d28ce…`: `buffers` (kind 0, 4 элемента) → id 1..4, пересекается с образами kinds 1..4; SPIRV-Cross закомментировал массив, а хост пишет буферы в 0+i и образы в 1+i (тот же конфликт).
- Что нужно для KYTY_SPVC_ABI=1 (три места, поэтому не сделано в этом шаге):
  1. Компиляция (`metalShader.mm`, `CompileSpirvToFunction`): для каждого дескриптора набора 0 `add_msl_resource_binding` с `count = размер массива`, базой из непересекающегося аллокатора (buffer-класс → `msl_buffer`, образы → `msl_texture`, сэмплеры → `msl_sampler`; id 0 оставить под `spvBufferSizeConstants`).
  2. Бинд (`metalRenderer.mm`, `BindLegacyStage`): заменить жёсткие смещения (`base+b`, `base+53`, `base+49`, цикл 56..103, `base+104`, `image_slot == 58`) на таблицу `kind → база`, вычисляемую той же функцией из `program->bindings` (или из reflection). Сейчас это ~15 мест.
  3. Отладочные пробы (`ApplyVsDbg` пишет в buffer 27, `ApplyPsProbe`) — проверить, что не зависят от старых id.
- Совпадение пробы и захвата: `[VsRow]` пишется по `KYTY_RT_AFTER=<PS hash>` при первом совпадении PS (`KYTY_RT_AFTER_N=1`), то есть это первый draw с PS 724318, а не обязательно draw #88 кадра 20. Нужно подтвердить номер draw в захватном прогоне (порядковый номер из `MetalDrawSequence`) и сравнить с `frame20.gputrace` #88.

### KYTY_SPVC_ABI=1: реализация и результат (2026-10-21)
- Что сделано (за флагом, по умолчанию выключено):
  - `src/graphics/host_gpu/metal/metalShaderAbi.h`: `ComputeSpvcAbiLayout(bindings, stage)` — непересекающиеся диапазоны id на дескриптор (порядок по SPIR-V binding, id 0 зарезервирован под `spvBufferSizeConstants`). Одна функция для компиляции и бинда.
  - `metalShader.mm` / `.h`: `CompileSpirvToFunction(..., abi)` вызывает `add_msl_resource_binding` для каждого дескриптора (буферы → `msl_buffer`, образы → `msl_texture`, сэмплеры → `msl_sampler`). Диск-кэш MSL отключён под флагом.
  - `pipelineCache.cpp`: передаёт layout при компиляции.
  - `metalRenderer.mm`: `BindLegacyStage` — буферы, SRT, сэмплеры, образы (`ImageBindingSlot`), дефолтные заполнения и compute-биндинг (буферы и текстуры) берут id из layout. Неописанные виды дают 256 и пропускаются (раньше `UINT32_MAX` писал за пределы `args[256]` и валил процесс).
- Результат проверки:
  - Metal validation: ошибка `buffer index out of range [0,0]` устранена; `setTexture index 33 outside [0,2]` устранена (compute-путь переведён на layout, undeclared образы пропускаются).
  - Остался один assertion: `Functon(main0): total used threadgroupMemoryLength(36864) must be <= 32768` (compute). Не связан с ABI, пред-существующий.
  - Аварийного завершения без валидации нет (до исправления `UINT32_MAX` падало на старте).
  - Кадры: frame 20 — 0.1% ненулевых, frame 100 — 0.1%, frame 600 — 0.0%. **Чёрный экран не исправлен**, гипотеза «ABI — основная причина» на текущих данных не подтверждена: валидация чистая, но картинки нет.
- Не сделано: шаг 4 (пробы `ApplyVsDbg`/`ApplyPsProbe` под новую раскладку), проверка «константы 1920/1080 видны в буфере», сверка draw #88 с `frame20.gputrace` (см. предыдущую запись о том, что `[VsRow]` относится к первому draw с PS 724318, а не обязательно к #88).
- Флаг остаётся выключенным по умолчанию; переводить в дефолт не стал, поскольку кадр не восстановлен.

### Push-constant блок (vsharp, Buffer 1) — 2026-10-21
- Исправлено (корректно независимо от первопричины): push-блок был один общий для VS и PS. PS-значения перезаписывали VS по тем же смещениям. Теперь у каждой стадии свой блок (`vs_push_data` → buffer 1 вершинного энкодера, `ps_push_data` → buffer 1 фрагментного). `metalRenderer.mm`, блок «1. Buffer 1: Push constants».
- Добавлен лог `KYTY_LOG_PUSH=1` (первые 6 draw): `start`, число регистров, размер `user_data`, первые dword.
- Результат прогона (frame 20): VS — `start=NoStart`, `regs=0`, `user_data=0`; PS — `start=0`, `regs=4` (reg 0..3), `user_data=4`, все значения 0. Кадр 20 — 0.0%.
- Вывод: нулевой push-блок не из-за склейки стадий. Источник — `snap.user_data` (ResourceMaterialization.cpp:1118 копирует `runtime.user_data`), который строится из `params.user_data` (shader.cpp `GetShaderParams`). Для VS `user_data` пустой, для PS нули. Значит, пользовательские SGPR (SPI_SHADER_USER_DATA_*) для этого draw не захвачены в состояние, которое получает шейдер. Это следующий шаг: найти, откуда берётся `user_data` для VS/PS в PM4 draw (регистры `user_data` в состоянии контекста) и почему для VS он пустой, а для PS нулевой.
- Не сделано: сверка с `vkCmdPushConstants` в Vulkan-пути; проверка аливинга `buffers[4]`/`spvBufferSizeConstants` для VS (см. ABI-запись выше).

### Не сделано: отображение kyty_missing* / SPI_PS_INPUT_CNTL
- В этом шаге не разбиралось: в Metal-пути не найден код генерации `kyty_missing*` через поиск по `src` (только в `metalShader.mm`, функция `AddMissingVertexVaryings`, которую нужно прочитать). Сопоставление SPI_PS_INPUT_CNTL OFFSET/DEFAULT_VAL — не проверено.

### Push-данные: зеркало Vulkan-пути (2026-10-21, по сообщению сессии)
- Vulkan-путь (`renderer/pipeline/descriptors.cpp`): `prepared.shader_data` = user-SGPR из `snapshot.user_data` по `user_data_registers` (в `push_data_start_dword`); затем `RebindBuffers` заполняет `memory_offset_dword + i/4` байтами `adjustment` = `offset - AlignDown(offset, 256)` для каждого буфера (`pack_memory_offset`); буфер привязывается по `aligned_offset`. Источник тот же `snapshot.user_data`.
- Сделано в Metal (`metalRenderer.mm`):
  - в блоке push-данных (`fill_push_data`) — упаковка `adjust = ga.offset & 255` по `memory_offset_dword + i/4` для буферов `descriptors.front().resources[i]`;
  - в биндинге буферов — `ga.offset & ~255` (выравнивание вниз), остаток едет в push-блоке.
- Логи: `KYTY_LOG_PUSH=1 KYTY_PROBE_HASH=0x724318af5ff62412` — показывает push-блок для кандидат-draw (VS 3d28 / PS 7243). Для него: VS `memoff_cnt=4 memoff_dw=0`, PS `memoff_cnt=1 memoff_dw=0`; значения dword 0 ненулевые и меняются от draw к draw.
- Результат: кадр 20 и 600 — 0.0%, чёрный экран не исправлен.
- Открытое расхождение: VS-MSL читает смещения буферов из `vsharp._m0[1]` (dword 1), а bind-time `memory_offset_dword` для VS = 0. PS-MSL читает `_m0[0]`, и это совпадает. Нужно выяснить, какая permutation/версия программы используется при биндинге VS и почему её `memory_offset_dword` не совпадает с компиляцией MSL (см. `pipelineCache.cpp` `permutation`/`push_data_cursor`).
- Не сделано: сверка 32 dword VS/PS с Vulkan-капчей для draw #269 (нужна Vulkan-сборка или `vkCmdPushConstants`-дамп), PS input mapping / `kyty_missing*`.

### Сравнительные GPU-захваты, кадр 20 (2026-10-21)
- KyTx Metal (текущее дерево, `KYTY_MSL_ONLY` не выставлен): `/Users/tx/Developer/fex-spike/runs/compare/kytx_f20.gputrace` (1.5 ГБ). Захват через `KYTY_GPU_CAPTURE=20 KYTY_GPU_CAPTURE_DIR=.../compare MTL_CAPTURE_ENABLED=1`, остановлен на флипе 21.
- Vulkan (MoltenVK, та же сборка без `KYTY_BACKEND`): `/Users/tx/Developer/fex-spike/runs/compare/vk_f20.gputrace` (2.6 ГБ). Захват через новый хук `KYTY_VK_GPU_CAPTURE=20` (`VkGpuCaptureTick` в `metalContext.mm`, вызывается из Vulkan-ветки `PrepareFrame` в `swapchain.cpp`). Захват устройства по умолчанию (`MTLCreateSystemDefaultDevice`), стартует на флипе 20, останавливается на 21.
- Номер командного буфера и индекс draw для логотипного draw CLI не извлекается из bundle. Искать в Xcode: draw с `drawPrimitives: Triangle, vertexCount: 6`, фрагментный шейдер с текстурой 3840×2160 (логотип, MTLTexture 14 у KyTx). В KyTx это draw с VS `3d28ce5308b9b56a` и PS `724318af5ff62412`.
- Захват frame 100 не делался (по просьбе — только кадр 20).

### Окно KYTY_GPU_CAPTURE перенесено на кадр игры (2026-10-21)
- Причина: прежнее окно открывалось на тике флипа N и закрывалось на N+1, а draw кадра N выполняются до тика N. В захвате оставались только blit презентера.
- Сделано (`metalContext.mm`, `GpuCaptureTick`): старт на тике N-1, перед остановкой на тике N вызывается `CommitCommandBuffer(true)`. Захват охватывает draw кадра N.
- Новый захват: `/Users/tx/Developer/fex-spike/runs/compare/kytx_f20.gputrace` (1.6 ГБ). Прежний: `kytx_f20_prev.gputrace` (сохранён).
- Проверка содержимого: trace бинарный (скомпилированные библиотеки), текстовый поиск маркеров (`vsharp`, `spvBufferSizeConstants`) не работает. Логотип-draw (6 вершин, VS `3d28…`, PS `7243…`) нужно подтвердить в Xcode.
- Vulkan-хук (`VkGpuCaptureTick`) не менялся: `vk_f20.gputrace` уже содержит draw #204.

### Сравнение с Vulkan-оракулом для логотипного draw (2026-10-21)
- Теория «vsharp / push-данные» снята: в vk_f20 push-блок `vsharp` (Buffer 8) у draw #204 тоже нулевой, значит нуль — норма. Упаковка смещений, добавленная ранее, сохраняется как соответствие Vulkan-пути, но не объясняет чёрный экран.
- Дескрипторы KyTx для кандидат-draw (VS `3d28…`, PS `7243…`), лог `KYTY_LOG_DESC=1 KYTY_PROBE_HASH=0x724318af5ff62412`:
  - VS: 4 буфера, все в одной области `0x8190c80000`; адреса `…20`, `…2c`, `…28`, `…00` (смещения 0x20, 0x2c, 0x28, 0), dw2 = 6, 6, 6, 1; dw3 = `0004022c`, `0004022c`, `00038fac`, `1004dfac`.
  - PS: 1 буфер `0x8190e80010` (dw2=1), 1 образ, 1 сэмплер.
  - `spvBufferSizeConstants`: все слоты 65536 (значения по умолчанию, т. к. `max_byte_extent` меньше).
- Что сверить в vk_f20 для draw #204 (нужен Xcode): (1) адреса и смещения VS-буферов (Bound Resources), (2) размеры привязанных буферов, (3) PS-ресурсы (буфер, образ 3840×2160, сэмплер), (4) выходы VS (Geometry: out_param_0, out_param_1, позиции) и входы PS.
- Выходы VS в KyTx (старый захват): `out_param_0 = (1,1,1,1)`, `kyty_missing2..` = 0. Совпадение с MoltenVK проверяется сравнением (4).
- Изменения рендеринга не делались; добавлен только лог `KYTY_LOG_DESC`.

### Diff MoltenVK (работает) vs KyTx (чёрный), логотипный draw, 2026-10-21 (от сессии FC27)
1. Argument buffer: MoltenVK — `spvBufferSizeConstants [[id(0)]]`, `buffers [[id(1)]][4]`, размеры через `spvDescriptorSet0.spvBufferSizeConstants[1]`, буфер размеров `[[buffer(29)]]`. KyTx — массив `buffers` закомментирован как «Overlapping binding», а `buffers` = `reinterpret_cast` от `spvBufferSizeConstants`. Вершинные указатели читаются с id(0) вместо своих слотов. Это и есть причина отсутствия картинки (по данным сессии).
2. MoltenVK дописывает `out.gl_Position.y = -(out.gl_Position.y);`. В KyTx этого нет: нужно сверить итоговую ориентацию кадра после исправления п. 1.
3. KyTx VS выдаёт `kyty_missing2..15`, у MoltenVK только `out_param_0/1`. PS скомпилирован с 16-входовым интерфейсом. Нужно сопоставить с построением входов PS по SPI_PS_INPUT_CNTL.
- Статус: исправление п. 1 в SPIRV-Cross-пути — это работа, которую направление «только MslEmitter, без SPIR-V/SPIRV-Cross» останавливало. Решение за пользователем; код не менялся.

### MSL-only (KYTY_MSL_ONLY=1), состояние 2026-10-21
- Решение пользователя (через сессию FC27): без SPIRV-Cross, только MslEmitter, с раскладкой слотов как у MoltenVK. Дисковый кэш MSL под `KYTY_MSL_ONLY` отключён (в кэше лежал SPIRV-Cross-вывод).
- Покрытие (кадры до 600, холодный кэш): скомпилировано через MslEmitter 72, не поддержано 28 (26 уникальных хэшей в логе). Пропущено draw: ~40000 (счётчик `[MSL-skip]` суммирует VS и PS на draw). Топ по пропускам: `a60d2b69d55aaca7` (24021), `5c4469c2d7f047f8` (15942), `04102c34aca70ab7` (14416), `15300f6811462109` (11136), `85070420c7292a12` (9610), `181db9212231a0c4` (4806).
- Причины неподдержки (по частоте): StoreBufferU32x4 (formatted/swizzled store), ImageSampleRaw (cube), DataAppend (нет lowering), ImageQueryDimensions, ImageGatherRaw (integer/compare/offset), BufferAtomicIAdd32, FPRecipIFlag32, SharedAtomicIAdd32/And32, `vertex output kind 8`.
- Логотип: VS `3d28ce5308b9b56a` и PS `724318af5ff62412` транслируются через MslEmitter (`[MSL] direct compiled`), валидация Metal чистая (0 assertion).
- Логотип-draw исполняется (`[RtAfter] prim=4 vtx=6`), но RT0 после него 0.0% ненулевых. VS MSL-only корректен при валидных данных: позиции ±1 (`2*x-1`, `1-2*y*c`), читает `buffers[0..3]` по `v0*24 + offset`, размер через `sizes[k]`. Значит, причина в PS (вывод/входы/образ) или в данных буферов. PS MSL в этом прогоне не выгрузился (нет `shader_724318*.metal` в `/tmp/msl_direct`), это следующий шаг.
- Не сделано: Y-инверсия `gl_Position.y` (сверить с MoltenVK), PS input mapping по SPI_PS_INPUT_CNTL, покрытие неподдержанных инструкций (см. причины выше).

### UMA / residency для MSL-only буферов (2026-10-21, по сообщению сессии FC27)
- Проверено по коду: `GetBufferForGuestRange` (metalBuffer.mm) оборачивает хост-память страницами (`newBufferWithBytesNoCopy`, длина = round-up запрошенного диапазона, например 16 KiB), `gpu_addr = gpuAddress + offset_in_page`. Адреса вида `UMA + 0x20` — это корректные GPU-адреса в пределах буфера; чтения VS через тот же UMA в SPIRV-пути давали корректные позиции (`[VsRow]`, GPU readback).
- Найдена реальная дыра: в `BindMslStage` (direct MSL) буферы кладутся в argument buffer как сырые GPU-указатели, но `UseResource` не вызывался (в legacy-пути и для образов он есть). Добавлено `UseResource(alloc.buffer, MTLResourceUsageRead)` в `BindGuestBuffer`.
- Результат: после исправления логотип-draw (`prim=4 vtx=6`) по-прежнему даёт 0.0% в RT0 и кадр 20 — 0.0%. Residency не была единственной причиной.
- Следующий шаг: найти MSL PS логотипа в `/tmp/msl_direct` (имена файлов по сырому хэшу, не по variant-хэшу; `724318…` — variant). Искать по содержимому: 1 буфер, 1 образ, 1 сэмплер, `out_mrt_0`, проверить `discard_fragment`/нулевой выход и сэмплинг образа.
- Не сделано: Y-инверсия `gl_Position.y`, PS input mapping (SPI_PS_INPUT_CNTL), покрытие неподдержанных инструкций.

### MoltenVK: дамп MSL логотипного пиксельного шейдера (2026-10-21)
- Дамп: `fex-spike/runs/compare/mvk_msl` (1794 файла: `shader-vs/fs/cs/tcs/tes-<hash>.metal`, `.spv`, и `pipeline-<hash>.txt` со списком VS/FS). Кэш `_PipelineCache` на время прогона был перенесён в `_PipelineCache.mvk_aside` и возвращён (проверено).
- Логотипная вершинная стадия по структуре (out_param_0/1, gl_Position, stride 24, buffers[4] при id 1, push при buffer(8)): кандидаты `08782515…`, `1a1e602b…`, `0f830c6c…` и др. Точная пара не подтверждена.
- Кандидаты фрагментного шейдера с 2 входами и одной текстурой: `15cfc007a07c9807` (240 строк, VS `08782515…`), `5ef8b171761cdaae` (191 строк, VS `1a1e602b…`). Совпадение с логотипом не проверено (нет размера текстуры в шейдере).
- Конфигурация MoltenVK, найденная в библиотеке: `MVK_CONFIG_SHADER_CONVERSION_FLIP_VERTEX_Y` (кандидат для вопроса Y-инверсии), `MVK_CONFIG_SHADER_DUMP_DIR`.
- Не сделано: однозначная идентификация логотип-PS (нужна пара VS/FS из захвата vk_f20 draw #204), сравнение с KyTx-PS 7243 (MSL-only дамп не получен), правки эмиттера.

### Сравнение логотипного PS: MoltenVK vs KyTx (2026-10-21)
- Кандидат логотипного FS в MoltenVK: `shader-fs-15cfc007a07c9807.metal` (1 `image_3`, 1 сэмплер, вход `locn0/locn1`). `5ef8b171761cdaae` отброшен: сэмплирует два образа.
- Ключевые различия в арифметике:
  - MoltenVK: UV = `in.in_param_0.{x,y}` × `flattened_srt._m0[24]`, `_m0[25]` (масштаб из SRT). Цвет: ветка `_98 ? x*x : x`, затем `fma(fma(2, c_y*c, -c), k, c)` с константами из SRT/буферов.
  - KyTx `724318af…` (SPIR-V-путь): UV = `in.in_param_0` без масштаба SRT; цвет умножается на константы из `buffers[0]` (смещение из push-блока `vsharp._m0[0]`); SRT в шейдере нет.
  - Общее: оба пишут half-упакованный `out_mrt_0` и делают `discard_fragment`.
- В MSL-only пути SRT передаётся в слот 160 (`BindMslStage`, `args[160]`), эмиттер объявляет `flattened_srt [[id(160)]]`. Слоты совпадают.
- Найдено и исправлено: SRT-буфер в MSL-пути не был resident (добавлен `UseResource`). Результат: RT0 логотипного draw по-прежнему 0.0%.
- Новый найденный пробел (общий для обоих путей): сэмплеры гостя (S#) в Metal-бэкенде не конвертируются в `MTLSamplerState`; везде используется сэмплер по умолчанию (`def_samp_id`). Это влияет на фильтрацию и wrap, но не объясняет пустой RT сам по себе.
- Открыто: значения SRT для логотипного PS (вероятно, нулевой масштаб UV или нулевые константы), MSL-only дамп PS (в `/tmp/msl_direct` его нет для 724318; имя файла по сырому хэшу, не по variant-хэшу). Следующий шаг: логировать `flattened_srt[24,25,27,28]` для PS 724318 и выгрузить MSL PS через `KYTY_MSL_DIRECT=2`.
- Не сделано: Y-инверсия `gl_Position.y`, PS input mapping по SPI_PS_INPUT_CNTL, S#-сэмплеры, покрытие неподдержанных инструкций.

### Проверка гипотез по логотипному PS 724318 (2026-10-21)
- Гипотеза «discard_fragment убивает все фрагменты»: опровергнута статически. В MSL KyTx (`/tmp/msl_dump/shader_724318…metal`) `pixel_valid_mask_active = uint(true)` (строка 106), `discard` под `!(mask != 0)`, то есть недостижим. У MoltenVK `15cfc007…` та же схема (строка 229, `discard` строка 236), тоже константа 1.
- Гипотеза «константы цвета нулевые»: опровергнута. CPU-лог (`KYTY_LOG_DESC`, MSL-only): буфер 0 по адресу `0x8190d80010` содержит `1.0, 1.0, 1.0, 1.0, 0, 0, 0xffffffff, 0` (читается корректно). Множители цвета = 1.0.
- Найдено: `flattened_srt` для этого PS имеет 16 dword (`n=16`), а индексы 24, 25, 27, 28 (которые читает MoltenVK `15cfc007…` для UV-масштаба и констант) выходят за границы и читаются как 0. KyTx SPIR-V-версия SRT не читает, поэтому там это не проявляется. Если MSL-only эмиттер читает SRT по тем же индексам, UV = 0 и выборка идёт в угол текстуры.
- Совпадение шейдера: 15cfc007 (MoltenVK) и 724318 (KyTx) не подтверждены как один и тот же гостевой шейдер (нужен адрес/хэш RDNA-кода логотип-draw в обеих сборках). Арифметика UV (`in × SRT[24/25]` против `in`) зависит от этого.
- Следующий шаг: выгрузить MSL-only PS (`KYTY_MSL_DIRECT=2` с именем по хэшу из лога), проверить индексы `flattened_srt[...]` и сравнить с `n=16`; сверить гостевой адрес шейдера логотип-draw в обеих сборках.
- Не сделано: правки эмиттера, Y-инверсия, PS input mapping, S#-сэмплеры.

### Экспорт PS и входы PS (MSL-only), 2026-10-21
- Гипотеза «COMPR: packed f16 пишется как float4 через as_type» опровергнута по дампам. Оба шейдера (KyTx `724318…`, MoltenVK `15cfc007…`) пишут `out_mrt_0 = float4(float2(as_type<half2>(x)), float2(as_type<half2>(y)))` — распаковка корректная. Цель RT0 — RGBA8Unorm (`fmt=10 → mtl=70`), экспорт float4. Отдельного packed-экспорта под этот RT нет.
- Найдено и исправлено в MSL-only эмиттере: входы PS брались по сырому индексу атрибута, без SPI_PS_INPUT_CNTL OFFSET. SPIR-V-путь это учитывал (`spirvEmitterAnalysis.cpp`, `ShaderPixelParameterLocation`). Теперь `MslEmitter.cpp` использует то же отображение (`[[user(locnN)]]` с отображённым N, без дубликатов). Сборка проходит.
- Результат: RT0 логотипного draw по-прежнему 0.0%, кадр 20 — 0.0%. Отображение входов само по себе пустоту не объясняет.
- Проверено по данным: позиции VS (пиксели 0..1920 и 0..1080 через константы `buf3`) и UV (`buf1`, в [0,1]) корректны; константы буфера 0 = 1.0; сэмплер по умолчанию — linear, clamp-to-edge (не нулевой).
- Открыто: PS сэмплирует текстуру по `in_param_0`, который по данным VS равен цвету `(1,1,1,1)`, то есть углу текстуры. Нужно выяснить `interpolator_settings[0..1]` (OFFSET) для PS `724318…` в этом draw. Если OFFSET указывает на param1, сэмплирование неверно в обоих путях; если на param0, то UV берётся не из этого входа.
- `KYTY_PS_PROBE` работает только на SPIR-V-пути (`ApplyPsProbe` для санитизированного SPIR-V), в MSL-only не применим.
- Не сделано: S#-сэмплеры, Y-инверсия `gl_Position.y`, покрытие неподдержанных инструкций.

### Растеризация vs шейдинг логотипного draw (MSL-only), 2026-10-21
- Добавлен диагностический пробник `KYTY_MSL_PS_RED=<hash>` (`CompileMslToFunction`): каждый return фрагментного шейдера с этим хэшем пишет `color0 = (1,0,0,1)`.
- 1) Красный пробник, логотип `724318…`, читается RT0 сразу после draw (`KYTY_RT_AFTER`):
  - с отсечением: RT0 = 0.0% ненулевых;
  - с `KYTY_NO_CULL=1`: RT0 = 0.0% ненулевых.
  - Вывод: фрагменты не производятся ни в одном режиме отсечения. Отсечение/обмотка не причина. Шейдинг (PS) проверить нельзя, пока фрагменты не появляются.
- Состояние растеризатора на этом draw (`KYTY_LOG_VP=1`, NO_CULL): cull=0, winding=1 (clockwise), fill=0 (Fill), viewport 0,0,1920,1080 (zn=0, zf=1, flipped=1), scissor 0,0,1920,1080. Значения ожидаемые.
- Данные вершин VS корректны (позиции пикселей и UV из `buf0`/`buf1`, нормализация через `buf3`), z=1, w=1, `[[position]]` есть.
- Metal-валидация этого прогона до логотип-draw не дошла за 200 с (валидация замедляет загрузку), поэтому не даёт ответа.
- 2) Промежуточные значения PS не снимались: фрагментов нет.
- Следующий шаг: захват MSL-only логотип-draw (`KYTY_MSL_ONLY=1 KYTY_GPU_CAPTURE=20`, `MTL_CAPTURE_ENABLED=1`) и просмотр Geometry (клип-координаты и число треугольников после растеризации) в Xcode. Это покажет, доходят ли позиции до растеризатора и почему фрагменты не создаются (z-клип при z=1, `depth_clip_mode`, пайплайн без фрагментной функции).
- Не сделано: Y-инверсия как исправление (не связана с пустотой), S#-сэмплеры, покрытие неподдержанных инструкций.

### После исправления fill mode (2026-10-21, по сообщениям сессии)
- Сборка с исправлением `TranslateFillMode` (metalPipeline.mm): запуск MSL-only до кадра 600, `KYTY_DUMP_FRAME=20,600`. Презентованный кадр: кадр 20 — 0.0% ненулевых, кадр 600 — 0.0%, средний цвет 0,0,0. Окно пользователю проверить.
- Пункт 2 (магента): это не фолбэк. Магентовый шейдер `kyty_debug_ps` (metalPipeline.mm) включается только переменной `KYTY_DEBUG_PS=1` и только для целей RG11B10Float (или всех при `KYTY_DEBUG_PS_ANY`). В прогонах без этих переменных его нет.
- Пункт 3 (презентер): причина пустоты кадра — `Presenter::Impl::Present` (swapchain.cpp) показывает `GetLastRenderTexture()`, то есть последний нарисованный RT (в логах `last_rt=0x8240000000 fmt=92`), а не флиповый буфер. Флип-адрес (`metal_guest_vaddr`) в этих прогонах равен 0 (`[Swapchain] PresentBuffer : vaddr=0x0`), поэтому выбор RT по адресу флипа не срабатывает. Нужно: (а) понять, почему `info.data.address` для флипа равен 0 в MSL-пути; (б) при ненулевом адресе выбирать RT по `GetRenderTextureForAddress(flip)`, а не последний RT.
- Пункт 1 (половина квада): не разбирал — нужны данные вершин 0..5 логотип-квада (VS читает `buf0` с шагом 24); в прошлых данных позиции 0..1 корректны, треугольник 2 не проверен.
- Не сделано: исправление презентера, проверка вершин 2..5, MSL-only по существу.

### Презентер и адрес флипа (MSL-only), 2026-10-21
1. Магента: `kyty_debug_ps` включается только переменной `KYTY_DEBUG_PS=1` (`metalPipeline.mm`, гейт `s_debug_ps_enabled`), и без неё не используется. Жёсткого включения по умолчанию в коде нет. Если магента появляется в прогоне без переменной, нужно искать переменную в окружении запуска, а не в шейдере.
2. Адрес флипа не нулевой: `PrepareFrame` получает реальные адреса (`0x8179800000`, `0x817a800000`, `0x817b800000`, формат 58, 1920×1080). Путь записи `metal_guest_vaddr = info.data.address` работает. Нулевой адрес в логах — от `PrepareBlankFrame` (чёрные флипы) и от fallback-ветки.
3. Трассировка `KYTY_TRACE_FLIP_WRITES=1`: по диапазону флипа 400 событий, все `bind_tex` (игра читает флип-буфер как текстуру). Ни одной записи (RT, `bind_buf`, копия) в этот диапазон нашими путями не видно. Поэтому ни один RT не покрывает адрес флипа: `rt_at_flip=0x0` на каждом флипе.
4. Изменение презентера (`swapchain.cpp`, `Presenter::Impl::Present`): если RT по адресу флипа найден, показывается он; иначе — `last_rt` как раньше. Логирование первых 10 флипов: `[Swapchain] flip vaddr=… rt_at_flip=… last_rt=…`. Сейчас RT по адресу не находится, поведение прежнее.
5. Результат: кадры 20, 100, 300 — 0.0% ненулевых (presented-кадры).
- Не сделано: найти источник записи в флип-буфер (возможно CS-запись или DMA/копия, которые наш путь не трассирует), эвристика `last_rt` не удалена, полоса квадра (пункт 5 предыдущего сообщения) не разбиралась.

### Оракул (Vulkan): кто пишет кадр в адрес флипа (2026-10-21)
- Лог `KYTY_ORACLE_FLIP=1` (`Presenter::Impl::ResolveSurface`, Vulkan-сборка, первые 20 флипов): адрес флипа совпадает с адресом образа в текстурном кэше (`0x8179800000`, `0x817a800000`, `0x817b800000`, размер 8847360, pixel_fmt=58, guest_fmt=50). Последняя запись в образ — **Storage** (`write_kind=2`) от шейдера `0xb3300709113f5521` на каждом флипе. Это compute, а не RT, не copy и не CPU-upload.
- Новое поле `Image::last_write` (kind/shader/serial) заполняется при RT, storage-привязке из шейдера (`descriptors.cpp`), CopyImage/Resolve/CopyMip/CopyWithBuffer/Upload.
- Metal, та же сцена: compute `0xb3300709113f5521` выполняется нативным MSL-путём (`[CsDispatch] native_msl=1`) и привязывает storage-образ `kind=31 class=2 addr=0x817a800000 1920x1080`, то есть именно адрес флипа. Прежний трейс `bind_tex` видел только чтения, поэтому писатель не был помечен.
- Изменение: адреса storage-образов регистрируются (`RegisterComputeWrittenImage`) в нативном compute-пути и в image-цикле `BindMslStage`. Презентер (`swapchain.cpp`) ищет RT по адресу флипа, затем зарегистрированный образ (`GetComputeWrittenImageForAddress`), затем `last_rt`. Теперь на 9 флипах адрес находит текстуру (`rt_at_flip` ≠ 0).
- Результат: presented-кадры 20, 100, 300 — 0.0% ненулевых, несмотря на найденную текстуру. Значит compute не записывает в неё содержимое (нулевой вывод шейдера, запись в другой адрес/слой, либо формат/тайлинг у Metal-текстуры отличается от Vulkan-образа).
- Следующий шаг: сдампить содержимое найденного storage-образа сразу после dispatch (`KYTY_LOG_CS=0xb3300709113f5521` + чтение текстуры), сравнить с выходом шейдера; проверить, что компьют-шейдер `0xb330…` выполняет запись (MSL-вывод: `shader_b3300709113f5521.metal`, в нём `.write(` в storage-образ `kind=31`).
- Не сделано: исправление компьюта (если запись не выполняется), удаление эвристики `last_rt`, пункт про полосу квадра.

### Compute-писатель 0xb330 (финальная композиция), 2026-10-21
- Проверено: писатель — нативный MSL compute (`[CsDispatch] native_msl=1`), пайплайн найден и привязан (`[CsPipe] cs_func=… pstate=…`), группы 30×1080×1. Storage-образ `kind=31` указывает на адрес флипа `0x817a800000` и регистрируется. Текстура, которую дампит презентер (`DumpTextureToPpm`, блит через очередь), — тот же объект, что привязан диспатчу (`[Reg] tex=…` = `dumped flip texture …`).
- Исправлено (реальная ошибка для всех нативных compute-изображений): нативные MSL-ядра объявляют образы по прямой раскладке (`image_N [[id(64 + resource)]]`), а биндер клал их в легаси-слот `NativeBinding(kind) + r` (для флипа — слот 31). Теперь для `native_msl` слот = `64 + resource` и в путь с argument encoder, и в путь `arg_ptrs`.
- Тест «безусловный зелёный в (0,0) при входе ядра» (MSL-проба `KYTY_MSL_CS_RED`, компилируется после правки имени `ab.image_5`): флип-текстура на кадрах 30 и 60 остаётся 0.0%. Значит запись ядра не попадает в эту текстуру, или ядро не доходит до записи.
- Не сделано: атомарный счётчик запуска (нужен буфер привязки), проверка валидацией Metal на диспатче, сравнение с SPIR-V-версией этого же ядра (`KYTY_CS_ARGENC_OFF`).
- Файл `metalShader.mm` менялся извне во время работы; изменения другой стороны сохранены.

### Compute-писатель 0xb330: корневая причина найдена и исправлена (2026-10-21)
- Причина «пустого» выхода: нативное MSL-ядро не получало буфер 0 (`ab`). Ветка `SetComputeBytes(arg_ptrs, …, 0)` стояла в `else` от `if (cs_argenc_here)`, а encoder-ветка для ядра не выполнялась, поэтому буфер 0 оставался непривязанным. Metal (с валидацией) сообщает `missing Buffer binding at index 0 for ab`, а без валидации диспатч молча не выполняется. Исправлено: байты аргументов привязываются, если encoder не использовался (`cs_encoder_used`). Валидация после исправления — 0 ассертов.
- Исправлено: нативные MSL-ядра объявляют образы по прямой раскладке (`64 + resource`); биндер в обоих путях (encoder и `arg_ptrs`) писал в легаси-слот `NativeBinding + r`. Теперь для `native_msl` слот `64 + resource`.
- Пробник «зелёный в каждом gid» (`KYTY_MSL_CS_RED=0xb3300709113f5521`): флип-текстура 100% зелёная на кадрах 30 и 60. Значит ядро выполняется и пишет в ту же текстуру, которую дампит презентер.
- Без пробника: флип-текстура 0.0% (выход ядра нулевой), презентованные кадры 20/100/300 — 0.0%.
- Входы ядра (`KYTY_DUMP_CS_INPUTS=0xb3300709113f5521`, 40-й диспатч): единственный дампнутый — LUT `0x81b3c80000` (32×32, sampled), он нулевой. Нулевой LUT даёт нулевой цвет композиции. Нужно выяснить, кто пишет этот LUT (вероятно, другое ядро), и почему его содержимое не попадает в текстуру, которую читает композиция.
- Попытка выравнивания «sampled → RT по адресу» (`GetRenderTextureForAddress` в пути compute) давала access violation в рантайме; ветка отключена. Оставлена только подстановка зарегистрированного storage-образа (dims-check), без падений.
- Файл `metalShader.mm` менялся извне; изменения другой стороны сохранены.

### Цепочка LUT для 0xb330 (2026-10-21)
- Продюсер LUT — ядро `17aeefc07e25295e` (нативный MSL): диспатчится (2 раза за прогон), пишет storage-образ `texture3d<float> image_0` (kind 33). Адреса записи: `0x81b3c80000` и `0x81b3cc0000`. Входов (sampled/буферов) у него нет.
- Найдено: биндер создавал образ как 2D 32×32 (T# не помечен как 3D), тогда как ядро объявляет `texture3d`. Исправлено: для compute-образов размерность берётся из программы (`ComputeGuestImageFor`, 3D через `GuestTexture3D` с глубиной из T#) в обоих путях биндинга.
- Пробник (`KYTY_MSL_CS_RED=0x17aeef…`) теперь компилируется (маркер с координатой `uint3` для 3D). Но чтение результата не работает: `DumpTextureToPpm` — блит 2D, для 3D-текстуры возвращает нули даже при записи маркера. Поэтому нулевой дамп LUT не доказывает отсутствие записи.
- Презентованные кадры 20/100 — 0.0%.
- Не сделано: читатель 3D-текстуры (срез z=0 через блит с origin/z или getBytes на shared), проверка LUT, тождественный LUT-диагностический режим (пункт 2 сообщения), счётчик отброшенных диспатчей (пункт 3). Предлагаемый путь счётчика: в `DispatchDirect` считать диспатчи, где ни encoder-путь, ни байты буфера 0 не выполнены (сейчас это исключено исправлением, поэтому счётчик должен быть 0; добавить проверку при будущих изменениях).
- Файл `metalRenderer.mm`/`metalShader.mm` менялся извне; изменения другой стороны сохранены.

### LUT-цепочка 0xb330: проверка 3D-чтения и записи (2026-10-21)
1. Чтение 3D: `DumpTextureToPpm` для 3D-источника копирует один срез (`KYTY_DUMP_3D_SLICE`, по умолчанию 0) в 2D-текстуру blit-копией, затем работает прежний путь. Проверено: LUT `17aeef…` — 32×32×32, срез 0 и срез 16 нулевые. LUT читается из зарегистрированных storage-образов `0x81b3c80000` и `0x81b3cc0000` на флипах 30 и 60 (дамп сделан вне диспатча).
2. Запись продюсера работает: при пробнике «красный/зелёный в каждом gid» (`KYTY_MSL_CS_RED=0x17aeef…`) оба LUT становятся 100% окрашенными. То есть ядро выполняется и пишет в зарегистрированную 3D-текстуру. Нулевой выход — результат вычислений ядра, а не запись.
3. Входы продюсера: буфер 0 (`ab.buffers[0]`) привязан (`snapshot buffers=1 b0=0x8190c80000/2`, в одном регионе с константами логотипа), других входов нет. Почему вычисления дают ноль — не выяснено. Возможные причины: содержимое буфера по адресу (нулевые константы), проверка размера/клампа (`sizes`), либо ветка state-machine ядра, которая уходит в `default: return` до записи основного значения.
4. Оговорка по дампу входов: `KYTY_DUMP_CS_INPUTS` вызывает `CommitCommandBuffer` внутри биндинга диспатча и тем самым сбрасывает активный compute-энкодер этого же диспатча. Для измерений использовать дамп на флипе (как сделано выше), а не внутри диспатча.
5. Не сделано: проверка, что потребитель 0xb330 привязывает тот же 3D-объект (MSL потребителя `texture3d`/sampled-сторона должна создаваться как 3D — для sampled-биндинга 3D-создание в `ComputeGuestImageFor` уже применяется), тождественный LUT-диагностический режим (пункт 2 сообщения), глобальный счётчик отброшенных диспатчей/отрисовок (пункт 4).
6. Что на экране: презентованные кадры 20/100 — 0.0% (без пробников).

### Заметки на потом (от исследовательской сессии FEX, низкий приоритет)
- (a) Guest-память как `MTLBuffer` (`newBufferWithBytesNoCopy`) на Apple требует выравнивания 16 КиБ; гостевые страницы 4 КиБ. Трекер страниц держать с абстрактным размером страницы.
- (b) Прямой MSL-эмиттер должен выдавать subgroup ballot/shuffle так, чтобы их нельзя было встроить в `?:` (это ошибка MoltenVK/SPIRV-Cross, из-за которой ломалось ночное освещение FC27). Нужен регрессионный тест на такие шейдеры.
- (c) Задержки чтения (readback stalls) не зависят от бэкенда; прежде чем приписывать изменения FPS, профилировать.

### Проверка цепочки LUT → композиция (MSL-only), 2026-10-21
1. LUT чтение (3D-срез в 2D blit): оба LUT `0x81b3c80000`/`0x81b3cc0000` нулевые на флипах 30 и 60 (срезы 0 и 16 проверены). Пробник красного/зелёного заполняет их на 100% — запись работает, выход вычислений нулевой.
2. Потребитель `0xb330…` привязывает тот же 3D-объект, что зарегистрирован продюсером (`[Consumer] … match=1 type3d=1`). Это подтверждено.
3. Тождественный LUT (`KYTY_DIAG_LUT_IDENTITY=1`, 32³ RGBA8, подменяет 3D sampled-вход потребителя): презентованные кадры 20/100/300 — 0.0%, флип-текстура на 30 и 60 — 0.0%. То есть LUT не единственный блокер: потребитель с тождественным LUT всё равно даёт ноль.
4. Счётчик отброшенных диспатчей: НЕ ноль. За прогон из 642000 диспатчей отброшено 238498 (`[DispatchDrop] … encoder=1 pipeline=0`): у этих ядер нет скомпилированного пайплайна (нерасписываемые в MSL-only ядра). Счётчик добавлен (`[DispatchStats]`, `[DispatchDrop]`).
5. Входы потребителя (sampled RT): `0x8240000000` и `0x8240870000` (1920×1080) на флипах 30 и 60 нулевые. Сцена в MSL-only не нарисована — композиция получает пустые RT.
- Вывод: блокер MSL-only — покрытие. Пропущенные draw (≈40k) и отброшенные диспатчи (≈37%) из-за неподдержанных инструкций (StoreBufferU32*, ImageSampleRaw cube, DataAppend, ImageGatherRaw, BufferAtomic*, FPRecipIFlag32, Shared*). Пока не закрыты, сцена не появляется ни с LUT, ни без него.
- Следующий шаг: закрыть неподдержанные инструкции по частоте (см. счётчики в логе `[MSL-unsupported]` и `[DispatchDrop]`), затем повторить проверку сцены.
- Пункт «что на экране»: на экране пусто (0.0% на presented-кадрах) как с обычным LUT, так и с тождественным.

### Диагностика с фолбэком на SPIR-V (KYTY_MSL_FALLBACK_SPIRV=1), 2026-10-21
- Добавлено временное переключатель `KYTY_MSL_FALLBACK_SPIRV` (по умолчанию выключен): шейдер, который MSL-эмиттер не может перевести, продолжает путь SPIR-V (`pipelineCache.cpp`), а не регистрируется без функции Metal. MSL-only остаётся включаемым переменной `KYTY_MSL_ONLY`, по умолчанию выключен (как и раньше).
- Результат прогона: частичный, аварийно завершился (EXC_BAD_ACCESS, PC `0x8940000010` — адрес гостевого диапазона; lldb подтверждает переход по плохому адресу). Причина не установлена; вероятно, смешение пайплайнов SPIR-V (без Metal-модуля, `handle.module = nullptr`) с MSL-путём. Кадры 20/100/300 не получены.
- Счётчик отброшенных диспатчей до падения: 26 из 2000 (~1.3%) против ~37% без фолбэка. Значит фолбэк почти полностью восстанавливает compute-диспатчи.
- Ранжирование неподдержанных возможностей (по прогону без фолбэка, `[MSL-skip]` и `[MSL-unsupported]`; частота строк в логе, не взвешено по draw):
  1. `ImageGatherRaw` (gather с косвенным индексом, `indirect=…/3`, `/1`, `/4`) — самый частый и дорогой по пропущенным draw (хэш `a60d2b69…`, ~24k пропусков).
  2. `vertex output kind 8` + `SetAttribute (auxiliary position export)` — хэш `5c4469c2…` (~16k пропусков).
  3. `StoreBufferU32x4` (formatted/swizzled buffer store).
  4. `ImageSampleRaw` (cube).
  5. `ImageGatherRaw` (integer gather, gather with compare/offset).
  6. `DataAppend`, `BufferAtomicIAdd32`/`BufferAtomicOr32`, `SharedAtomicIAdd32`/`SharedAtomicAnd32`, `FPRecipIFlag32`, `ImageQueryDimensions`, `BitCount64`.
  - Хэши `04102c34…` (~14k), `15300f68…` (~11k), `85070420…` (~10k), `181db921…` (~5k) в логах без строк причин; нужна отдельная выгрузка их MSL/причин.
- Пункт «что на экране»: без фолбэка — 0.0% на кадрах 20/100/300. С фолбэком до падения кадры не получены. Смотреть окно не могу; пользователь должен проверить окно.

### Падения диагностики с фолбэком SPIR-V (KYTY_MSL_FALLBACK_SPIRV=1), 2026-10-21 — для сессии FEX
- Прогон 1 (lldb): `EXC_BAD_ACCESS` при `pc = 0x0000008940000010` — адрес гостевого диапазона, то есть исполнение перешло на гостевой адрес с хостового потока. Классификация: сбой на стороне CPU/FEX (переход guest→host или возврат/инвалидация кода), не шейдер. Последняя строка HLE в логе — стартовый `PthreadMutexTrylock` (лог HLE на поздних кадрах не пишется), поэтому по HLE точку падения установить нельзя.
- Прогон 1 (без lldb): `Unhandled host exception … pc=0x191e27504 address=0x57e4f1656708` (тот же класс, что в других логах: адрес данных не из кучи/стека, PC в системной библиотеке).
- Прогон 2 (повтор, та же команда): `Unhandled host exception … pc=0x191e27504 address=0x7b2ce5b105d0` — то же место в системной библиотеке, другой адрес данных. Падение не воспроизводит адрес, но класс тот же.
- Сравнение с прочими логами: похожие сигнатуры встречаются в `run8.log`, `run9.log` (`pc=…435c, address=0`) и `lldb_exit3/4.log`, `run10.log` (`address=0x80f3404000`). Прямого совпадения с lldb-PC `0x8940000010` в других логах нет.
- Данные до падения: `[DispatchStats] total=2000 dropped=0` (прогон 2). В прогоне 1 было 26 из 2000. Кадры 20/100 — 0.0%; кадр 300 не получен.
- Решение: повтор использован. CPU-сторону в этой сессии не правим; передаю сессии FEX: разобрать переход на гостевой адрес `0x89…` при активном фолбэке (возможная причина: код шейдеров/инвалидация JIT при создании SPIR-V-пайплайнов из потока компиляции).

### Фолбэк SPIR-V вынесен в отдельную копию (2026-10-21)
- В KyTx фолбэка нет: переключатель `KYTY_MSL_FALLBACK_SPIRV` удалён из `pipelineCache.cpp` (восстановлено поведение MSL-only). KyTx не содержит кода Vulkan/SPIR-V-фолбэка для диагностики.
- Диагностическая копия: `/Users/tx/Developer/KyTx-diag` (rsync без `_Build`, фолбэк применён только там). Сборка: `/Users/tx/Developer/KyTx-diag/_Build2/macos-arm64` (CMake с `KYTY_BUILD_LAUNCHER=OFF`, как в основной сборке; без лаунчера Qt не нужен).
- Запуск диагностики — из `KyTx-diag/_Build2/macos-arm64/kyty_emulator` с `KYTY_MSL_FALLBACK_SPIRV=1`.
- Прежние падения диагностики (см. предыдущую запись) относятся к варианту с фолбэком в KyTx до переноса; повторять их на копии.
- Прогон диагностики из копии `KyTx-diag` (с фолбэком SPIR-V): падение `Unhandled host exception … pc=0x191e27504 address=0x0000158a5fdc5bb8` (тот же PC, что и в предыдущих падениях, адрес данных другой). Упало после ~5800 диспатчей; кадров 20/100/300 не получено (0.0%).
- Счётчик отброшенных диспатчей: `total=4000 dropped=0`; первые отбрасывания после `total=4784`; к `total=5768` отброшено 17 (~0.3%). Без фолбэка в KyTx было ~37%. Фолбэк почти полностью восстанавливает compute-диспатчи.
- Классификация падения: PC в системной библиотеке, адрес данных мусорный — класс «повреждение данных/указателя на стороне хоста», не тот же, что переход на гостевой адрес (lldb-случай `0x8940000010`). Для сессии FEX и для этого класса дополнительно: ищется источник неверного указателя при активном фолбэке.

### Совет от внешней ИИ-сессии: разбор по пунктам (2026-10-21)
- Проверка: в KyTx нет `KYTY_MSL_FALLBACK_SPIRV` (0 вхождений в `src`); переключатель удалён, MSL-only сохранён.
- Пункт 1 (сделано): каждый пропущенный compute-диспатч (нет пайплайна) логируется один раз на хэш: `[SKIPPED CS] hash=… writes: img@<адрес>(<w>x<h>) …` (`metalRenderer.mm`, рядом с блоком `cs_hash`). Прогон 150 с: 22 пропущенных хэша. Примеры: `173f6bb0…` пишет `img@0x81b7000000 128x128`, `70c11672…` пишет `img@0x81b7070000 512x3`, `22e3f6a6…` пишет `img@0x81b0820000 1x1`. Отладочная группа Metal не добавлена: при пропуске кодировщика нет.
- Пункт 2 (не сделано): «poison-fill» пропущенных диспатчей (серый/шаблон в выходные текстуры и буферы, env по умолчанию выключен). Нужен для ответа, post-processing это или геометрия/отсечение. Следующий шаг: заливка выходных образов из списка `[SKIPPED CS]` константой в `DispatchDirect` при пропуске.
- Пункт 3 (не сделано): обратный срез от чёрных входов композиции (сцена, LUT) через продюсеров до пропущенного хэша. Продюсер LUT `17aeef…` работает (см. выше), его входы не дают ненулевого результата; срез дальше по цепочке требует логов связей «какой диспатч пишет образ по адресу». Первая цель покрытия — хэш, чьи выходы совпадают с входами сцены.
- Пункт 4 (не сделано в этой сессии): ранжирование по пропущенным диспатчам и реализация в `MslEmitter` (buffer/image format-store и format-load с конвертацией, LDS-атомики и ds_*, wave64 ballot/mbcnt/readlane на SIMD32, буферные и image-атомики — нужен `MTLTextureUsageShaderAtomic`, image_store в 3D/2D-array). Ранжирование по текущим логам: `ImageGatherRaw` (≈24k пропусков), `vertex output kind 8`/`SetAttribute` (≈16k), `StoreBufferU32x4`, `ImageSampleRaw` cube, `DataAppend`, `BufferAtomicIAdd32`, `FPRecipIFlag32`, `SharedAtomicIAdd32`. Реализация — отдельные шаги.
- Пункт 5 (для сессии FEX): падение диагностики в копии (`KyTx-diag`, PC `0x191e27504`, мусорный адрес данных) после ~5800 диспатчей. Перенос трансляции шейдеров на рабочий поток с изоляцией стека/TLS — предложение для сессии FEX, если падение не связано с шейдерами.

### Исследовательские заметки (Grok, октябрь 2026) — справочно, не проверено
- Ориентиры для нативного Metal-бэкенда: `Wooloomooloo2/bloodborne_mac_port` (v0.5.0+, видеоядро shadPS4, кодирование в Metal без Vulkan/MoltenVK); `Huax-1839/bloodborne-macos`; `bloodborne-native/bloodborne-runtime` (экспериментальный Metal). Заявлено: push-дескрипторы (прямой `setBuffer`/`setTexture`) быстрее argument buffer (+13–16% FPS).
- Upstream shadPS4: явное понижение wave64 → wave32 (ballot и MBCNT парой), барьеры shared-памяти, детекция расходящихся циклов. Коммиты стоит прочитать при проектировании wave64 на SIMD32.
- Факты Metal (заявлены, проверить по документации SDK): `simd_ballot` на macOS возвращает 64-битный тип при ширине SIMD 32 (нативного wave64 нет); атомики текстур только для R32Uint/R32Sint (RG32Uint для 64-бит) с `MTLTextureUsageShaderAtomic`; 64-битные буферные атомики поддержаны.
- Известные проблемы Frostbite на shadPS4/KytyPS5: гонки данных в LDS, сэмплирование depth-compare на неверных форматах, «взрыв» вершин. Учитывать, когда сцена начнёт рисоваться.
- Клонирование ориентиров не выполнено: скачивание внешних репозиториев требует явного разрешения пользователя. Чтение их кода — только после разрешения.
- Разрешение на shallow-клонирование (Wooloomooloo2/bloodborne_mac_port, Huax-1839/bloodborne-macos, shadps4-emu/shadPS4 в `/Users/tx/Developer/_refs/`) пришло через сообщение другой сессии, а не напрямую от пользователя. До подтверждения в этой сессии клонирование не выполнено.
- Второе сообщение от другой сессии: «клонирую сам по прямому разрешению пользователя». Пока это тоже пересказ, а не подтверждение в этой сессии. Чтение локальных клонов не делаю до подтверждения пользователем здесь.
- Ограничение KyTx подтверждено пользователем (через сессию): чистый KyTx без Vulkan/MoltenVK/SPIR-V-путей и без Rosetta (native arm64 + FEX + Metal). Референсные проекты можно собирать и запускать отдельно, вне KyTx.
- Не сделано в этом шаге: poison-fill диагностика (нужна заливка storage-текстур и буферов в MSL-only пути), обратный срез, ранжирование покрытия, новые возможности MslEmitter. Рекомендуемый порядок: poison-fill (env, по умолчанию выключен), затем срез по входам композиции, затем покрытие по частоте.

### Чёрный экран: геометрия и рисование (2026-10-21)
- Причина пропусков draw: вершинный шейдер `0x5c4469c2…` (≈20500 пропусков в одном прогоне) не переводился. Причины: выход kind 8 (`Layer`, gl_Layer) и вспомогательный экспорт позиции (`exp.index != 0`, клип/кал-дистанции). Сделано (MslEmitter): выходы Layer/PointSize/ClipDistance/CullDistance/ViewportIndex и вспомогательный экспорт позиции принимаются и отбрасываются. Ограничение: клип-плоскости, рендер в слой и размер точки не работают.
- После этого: `[MSL-skip]` перестал печатать (меньше 500 пропусков), за 200 с выдано 1390 draw с нормальными viewport 1920×1080 и cull 0.
- Диагностика `KYTY_DIAG_DEBUG_PS=1`: draw с нерасписываемым PS рисуется красным PS. Пайплайн создаётся без ошибок (`[MetalPipeline] create failed` — 0).
- Результат: красного нет ни в одной сохранённой цели: отслеживаемые RT `0x8240870000` (fmt 70) и `0x8240000000` (fmt 92), последняя RT презентера — все 0.0%. Dump делается после `CommitCommandBuffer(true)`.
- Гипотезы для следующего шага (не проверены): (а) draw пишет в RT, не попавший в список отслеживаемых (`GetTrackedRenderTargets` возвращает только 2 адреса, а презентер видел `0x8240c00000`, `0x8241b80000`); (б) маска записи цвета / blend / scissor на этих draw; (в) RT пересоздаётся между draw и dump. Следующий шаг: лог на каждый draw с pstate: адрес rt0, маска, blend, scissor, число вершин.
- FEX-сторона (падения диагностики, переход на гостевой адрес) — отложена, как указал пользователь.

### Учёт draw по кадрам (MSL-only, диагностика), 2026-10-21
- Добавлен учёт draw: `[DrawStats] flip=N submitted=… executed=… dropped_vs/ps/pipe=…` (окно 25 флипов, `metalRenderer.mm`, вызов из презентера на каждом флипе). Пропусков по VS/PS/пайплайну в прогоне до флипа 300 нет: окна по 25 флипов дают submitted=225, executed=225, dropped=0.
- Наблюдение 1: только 9 draw на флип (225 на 25 флипов). Ожидалось 150–300 на кадр (по прежним прогонам до MSL-only). Значит draw теряются выше по конвейеру (PM4/состояние), а не в рендерере.
- Наблюдение 2: диспатчи (compute): `total=984000 dropped=365926` (≈37%), пайплайна нет у этих ядер.
- Наблюдение 3: презентер показывает `last_rt=0x8240c00000 fmt=70`; дамп последней RT — 0.0%, даже когда красные draw исполняются. Гипотеза: вершинный шейдер `0x5c4469c2…` пишет вырожденные позиции (входы из SRT/буферов читаются нулями) → фрагментов нет. Проверка: лог позиций вершин (для MSL-пути нужен отладочный выход позиций в буфер).
- Следующие шаги: (1) отладочный буфер с позициями вершин для draw с VS `0x5c44…`; (2) причина малого числа draw на флип (сравнение с прежним прогоном до MSL-only по тем же флипам).

### Покрытие compute: ранжирование и первый шаг (MSL-only), 2026-10-21
- Учёт пропусков: `[CsRank]` (каждые 100 флипов) — пропущенные compute-диспатчи по хэшам с долями. `[DrawStats]` — draw по окнам 25 флипов (submitted/executed/dropped).
- Ранжирование до реализации (прогон ≈300 с, 984k диспатчей, пропусков 37%): `4fbf70b2` (StoreBufferU32, formatted) 52%; `df0a2346` (BufferAtomicIAdd32) 17%; `89bdd959` (StoreBufferU32x4, formatted) 11%; `aaacb6e5` (FPRecipIFlag32) 1%; `3b4f635e` (BufferAtomicIAdd32) 1%; `b5b087ef` (ImageGatherRaw, индекс 4) и `a69ef1ce` (DataAppend) — хвост.
- Сделано (MslEmitter, `StoreBufferU32`): нетипизированный formatted-store с дескрипторным форматом 32-бит (k32UInt/k32SInt/k32Float) — обычный dword-store, как в SPIR-V-пути. Остальные formatted-store не поддержаны (причина в логе: `data_format/number_format`).
- Результат: пропуски compute с 37% до 18% (876k диспатчей, 156,729 пропущено). `4fbf70b2` из списка пропусков исчез.
- Выполненный учёт draw: за прогон 225 отправлено / 225 выполнено на окно из 25 флипов; потерь на уровне рендерера нет. Малое число draw на флип — следствие того, что геометрию рисуют косвенные draw с аргументами из compute (см. ниже).
- Косвенные draw (пункт 3 совета): `CommandProcessor::DrawIndirect` (`guest_gpu/graphicsRun.cpp`) читает аргументы через CPU `memcpy` из гостевой памяти и вызывает `DrawIndexAuto` с этими значениями. Аргументы, записанные compute на GPU, на CPU видны как устаревшие или нулевые: draw уходит с нулевым числом вершин. Metal-бэкенд не имеет пути indirect-buffer draw (нет `drawPrimitives:indirectBuffer`). Это вероятная причина малого числа draw. Нужна реализация: draw с аргументами из GPU-буфера (или ICB) для MSL-пути.
- Следующие шаги: (1) атомики буфера `BufferAtomicIAdd32`/`BufferAtomicOr32` (df0a2346, 3b4f635e) — самые частые оставшиеся; (2) `StoreBufferU32x4` formatted (89bdd959); (3) indirect draw с аргументами из GPU-буфера.
- Краш: одного прогона по MSL-only (exit 65, `pc=0x191e27504`, мусорный адрес) не было в этом прогоне; в предыдущем прогоне диагностики с фолбэком — да. Для сессии FEX остаётся отмеченным.

### Покрытие compute: последовательность шагов и доли пропусков (MSL-only), 2026-10-21
- Доля пропущенных compute-диспатчей по шагам (один и тот же ~300 с прогон на каждом шаге):
  1. Базовый прогон: 37% (365926 из 984000).
  2. `StoreBufferU32` formatted с 32-битным дескрипторным форматом → dword-store: 18% (156729 из 876000).
  3. Буферные атомики 32-бит (`BufferAtomicIAdd32`, `BufferAtomicOr32`) через `atomic_fetch_add/or_explicit` на `device atomic_uint*` с защитой по условию и границе: 9.8% (84425 из 862000).
  4. `StoreBufferU32x2/x3/x4` formatted с 32-битным форматом (та же проверка): 5.8% (51546 из 894000).
  5. `FPRecipIFlag32` = `1.0/x` (как в SPIR-V-пути): 5.0% (44897 из 906000).
- Оставшиеся пропуски (≈7 тыс. диспатчей у каждого, <1% каждый): `ImageGatherRaw` (варианты: косвенный индекс, integer, compare/offset), `DataAppend`, `SharedAtomicAnd32`, `BitCount64`, `StoreBufferU32x4` с `data_format=0` (формат из дескриптора не 32-битный или не декодирован).
- Внимание: строки `[CsRank]` и `[DrawStats]` печатаются при флипах 25/100; причины неподдержки — в `[MSL-unsupported]`.
- Не сделано: косвенные draw на GPU-аргументах (`DRAW_INDIRECT`, `DRAW_INDEX_INDIRECT`, `MULTI_DRAW_INDIRECT(_COUNT)`): сейчас `CommandProcessor::DrawIndirect` (`guest_gpu/graphicsRun.cpp`) читает аргументы через CPU `memcpy`, а Metal-путь не имеет `drawPrimitives:indirectBuffer`. Это вероятная причина малого числа draw на флип (9). Нужен путь: MTLBuffer по адресу аргументов + `drawPrimitives/drawIndexedPrimitives:indirectBuffer:`; для multi/count — цикл по максимуму с проверкой счётчика через compute.
- Не проверено: выводит ли кадр изображение после этих шагов (`[DrawStats]`, RT-дампы).

### Косвенные draw на GPU (Metal), 2026-10-21
- Сделано: одиночный `DRAW_INDIRECT` (не indexed) в Metal-пути читает аргументы на GPU. `CommandProcessor::DrawIndirect` (`guest_gpu/graphicsRun.cpp`) передаёт адрес аргументов через `SetPendingIndirectDraw` (только при `UseNativeMetalBackend`); `MetalRenderer::DrawAuto` берёт его `TakePendingIndirectDraw`, обходит ранний выход при `vertex_count==0` и вызывает `MetalDrawExecutor::DrawIndirect` → `drawPrimitives:indirectBuffer:indirectBufferOffset:`. Буфер аргументов — `GetBufferForGuestRange(addr, 16)` с `UseResource`. Раскладка AMD `DrawIndirectArgs` совпадает с `MTLDrawPrimitivesIndirectArguments` (vertexCount, instanceCount, vertexStart, baseInstance), конвертация не нужна.
- Проверка: сборка без ошибок; падений и ошибок компиляции MSL нет; исполненные draw на 25 флипов остались 225 (как до изменения). Presented-кадры 100/300 — 0.0%. Значит главный поток геометрии идёт не через одиночный DRAW_INDIRECT.
- Причина малого числа draw: `CommandProcessor::DrawIndirectMulti` (`graphicsRun.cpp`) читает счётчик `draw_count = *count_addr` на CPU при записи команд. Если счётчик записан compute этого же кадра, CPU видит ноль и `if (draw_count == 0) return;` — draw не выполняется.
- Не сделано (следующий шаг): multi-indirect на GPU: (1) при `count_addr != nullptr` и Metal выдавать `max_count_or_count` indirect-draw с `offset = i * stride`; (2) перед ними compute-предикат: для `i >= *count` обнулить `instanceCount` в аргументах (тот же буфер, что и аргументы); (3) индексированные варианты: `drawIndexedPrimitives:indirectBuffer:` с индексным буфером из состояния CP. Нужен отдельный MSL-ядро для предиката (через `newLibraryWithSource`, как `DiagRedPixelFunction`).
- Для проверки: `[DrawStats]` (исполненные draw на окно 25 флипов) и RT-дампы на флипах 100/300.

### Multi-indirect на GPU: анализ ограничения (2026-10-21)
- Задача: MULTI_DRAW_INDIRECT(_COUNT) без чтения счётчика на CPU, с предикатом по счётчику на GPU.
- Ограничение Metal-пути: предикатное compute-ядро нужно поставить между draw. Render-проход в `metalDraw.mm` начинается с `loadAction` из дескриптора прохода; завершение и повторное открытие `renderCommandEncoderWithDescriptor` заново применяет load-action (для очищаемой цели — очистка кадра). Вставка compute внутрь открытого render-прохода поэтому портит кадр.
- Варианты: (а) indirect command buffer (ICB): compute заполняет ICB (draw с `instanceCount=0` для `i >= count`), render-проход вызывает `executeCommandsInBuffer:withRange:`; нужны `inheritPipeline`/`inheritBuffers` и ICB-дескриптор с числом команд = max_count; (б) разрыв командного буфера до прохода (коммит, отдельный буфер с предикатом, новый буфер с render-проходом, load-action Load для продолжения) — требует отслеживания того, что проход уже начат.
- Рекомендация: (а) ICB. Это отдельный шаг с новым кодом (создание ICB по размеру max_count, encode в compute, execute в render); не реализовано в этой сессии.
- Состояние: одиночный DRAW_INDIRECT на GPU реализован (см. предыдущую запись); MULTI не изменён (CPU-счётчик ⇒ 0 draw).

### Косвенные draw на GPU: состояние (2026-10-21)
- Реализовано (Metal, MSL-only):
  - одиночный `DRAW_INDIRECT` (не indexed): аргументы читаются на GPU через `drawPrimitives:indirectBuffer:` (`CommandProcessor::DrawIndirect` → `SetPendingIndirectDraw`, `DrawAuto` → `MetalDrawExecutor::DrawIndirect`);
  - multi-draw non-indexed (`CommandProcessor::DrawIndirectMulti` при Metal): без чтения счётчика на CPU; `DrawAuto` выполняет `RunMultiDrawClamp` (compute `kyty_mdi_clamp`: для `i >= count` обнуляет instanceCount в аргументах, in place) и затем до `max_count` indirect-draw. Пассу, закрытую для клампа, переоткрываем с `loadAction = Load` (`s_last_pass_desc`). Счётчик без count-адреса (direct count) тоже идёт этим путём (`multi=true`, без клампа).
  - Ограничение: клампирование пишет в исходные аргументы игры (не в scratch-копию); лимит 4096 entries на пакет; глубина/трафарет после переоткрытия грузятся (DontCare-хранение первой пассы не сохраняет глубину между частями).
- Не реализовано: indexed multi (`0xc0083800`) и indexed single — используют CPU-аргументы (как раньше).
- Измерения (MSL-only, ≈120–240 с):
  - multi/indirect пакеты в этой фазе не встречаются вовсе (`[MdiStats]` и `[DiStats]` не печатаются); `[DrawStats]` исполненных draw: 225 на 25 флипов (фаза на флипе ~100); в более поздней фазе (флип 3725): submitted=4645, executed=4720, dropped_ps=125;
  - presented-кадры 100 и 300: 0.0% ненулевых.
- Вывод: фазы игры различаются по типу draw; для этих кадров косвенный путь не оказывается узким местом. Нужна фаза, где в пакетах есть DRAW_INDIRECT (проверять `[DiStats]`/`[MdiStats]` на длинном прогоне), и отдельно разбор, почему presented-кадр остаётся пустым.
- Для пользователя: окно нужно посмотреть вручную (с этой стороны изображения не видны).

### Residency и MDI-клямп: сверка с bloodborne_mac_port (2026-10-21)
- Residency (сделано): в нативном compute-биндинге буферы и образы, которые передаются GPU-указателями в `arg_ptrs`, раньше не попадали в residency-набор (`UseResource` вызывался до `BeginComputePass`, когда активного compute-энкодера нет, и был no-op). Теперь ресурсы собираются в `cs_residency` (объявлен в начале `DispatchDirect`) и вызывается `useResource` на compute-энкодере после `BeginComputePass`. Валидация Metal (`MTL_DEBUG_LAYER`, `MTL_SHADER_VALIDATION`) после изменения: 0 ассертов, в т.ч. без `not resident`.
- Аналогичный gap уже закрыт ранее: буферы прямого MSL-биндера (`BindGuestBuffer`) и SRT-буфер (`args[160]`).
- MDI-клямп: по сравнению с `bloodborne_mac_port` (`mtl_encoder.mm:1475-1537`, `clamp_indirect_count` в `mtl_encoder.mm:118-130`, `mtl_draw_core.mm:385-420`) наш путь совпадает по схеме: конец рендер-энкодера → compute-клямп → переоткрытие прохода с Load → цикл `max_count` indirect-draw. Отличия: (а) клямп пишет in place в аргументы игры, а не в пулированный scratch-буфер; (б) конверсия layout не нужна (AMD `DrawIndirectArgs`/`DrawIndexedIndirectArgs` совпадают с `MTLDraw(Indexed)PrimitivesIndirectArguments` по порядку полей); (в) indexed multi не реализован.
- Измерения (MSL-only, 240 с, с residency): presented-кадры 20/100/300 — 0.0% ненулевых; dispatch drop 40720 из 822000 (≈5%). Кадры по-прежнему пустые.
- Следующие шаги: (1) заменить in-place клямп на scratch-буфер (пул) с конверсией; (2) indexed multi; (3) разбор пустого presented-кадра: какие RT получают draw в фазе, где есть `DRAW_INDIRECT`.

### MDI: scratch-клямп, indexed multi, захват кадра 100 (2026-10-21)
- Клямп больше не пишет в аргументы игры: ядро `kyty_mdi_clamp` копирует `max_count` записей в пулированный scratch-буфер (`s_multi_scratch`, растёт по требованию, `MTLResourceStorageModePrivate`) и обнуляет instanceCount для записей `>= count`. Цикл draw читает scratch (`s_multi_draw_buffer`). Аргументы игры не меняются.
- Indexed multi (`0xc0083800`, DRAW_INDEX_INDIRECT_MULTI): на Metal через тот же клямп и цикл `drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:` (`MetalDrawExecutor::DrawIndexedIndirect`). Индексный буфер — от `m_index_base_addr` (размер `m_index_buffer_size` или 1M элементов по умолчанию, тип u16/u32 из `m_index_type_and_size`). Стартовый индекс берётся из аргументов, поэтому один буфер на все записи.
- Проверка: сборка проходит. Валидация Metal на прогоне 200 с: 0 ассертов; один прогон упал по хостовому PC `0x191e27504` (та же сигнатура, что в ранее отмеченных падениях), без валидации тот же путь прошёл 240 с без падения. В этих фазах индирект-пакетов нет (`[DiStats]`/`[MdiStats]` не печатаются), поэтому новый путь ещё не выполнялся на реальных данных.
- Захват: `kytx_f100_v2.gputrace` (1.5 ГБ) в `/Users/tx/Developer/fex-spike/runs/compare/`. Исполненные draw по CPU-счётчику на флип 100: 225 за окно из 25 флипов (как и прежде).
- Не сделано: счётчик исполненных draw на GPU (нужен отдельный compute-счётчик); разбор, где цепочка ещё пустая (это по захвату, в Xcode).
- Пункт для проверки: на фазе с индирект-пакетами проверить `[MdiStats]`/`[DiStats]` и `[DrawStats]`.

### Сверка compute-диспатчей по флипам (2026-10-21)
- Счётчики (per-flip, `[DispatchPerFlip]` каждые 25 флипов): `cp` — DISPATCH-пакеты CP (прямые и косвенные), `renderer` — входы в `MetalRenderer::DispatchDirect/Indirect`, `encoded` — реально выданные `dispatchThreadgroups`, `nopipe` — без пайплайна, `noenc` — без compute-энкодера (пасс не начался), `emptygrid` — нулевая сетка.
- Ранняя фаза (флипы 25–350): cp = renderer = 16.4–17.4 на флип, encoded ≈ 16.4–17.2, nopipe ≈ 0.0–0.1. Это совпадает с захватом кадра 100: 16 compute-энкодеров и 10 draw.
- Поздняя фаза (флип ~7400): cp = renderer = 127.2 на флип, encoded = 111.8, nopipe = 5.8 (dispatch без пайплайна). Разница 127.2 − 111.8 − 5.8 ≈ 9.6 на флип на момент последнего прогона до добавления счётчика `noenc` — она должна объясняться отказом `BeginComputePass` (счётчик `noenc` добавлен, но в поздней фазе этот прогон не завершён).
- Вывод по «3300 на флип»: это ошибка в расчёте, а не данные. 984k диспатчей делились на ~300, тогда как флип на тот момент был около 9000. Реально 16–17 на флип в ранней фазе и ≈127 в поздней.
- Падение: хостовое исключение (PC `0x191e27504`, мусорный адрес данных) повторилось в двух последних прогонах (флипы ~250 и ~350) после добавления счётчика `noenc`; ранее тот же бинарник проходил до флипа 7400. Не воспроизводится стабильно; класс — тот же, что в ранее отмеченных падениях. Для сессии FEX.
- Не сделано: подтвердить поздние значения `noenc` (нужен прогон без падения); разбор 9.6 на флип в поздней фазе.

### Таблица операций одного флипа (KYTY_TRACE_OPS_FLIP=100), 2026-10-21
- Инструмент: `TraceOpsFlipTick`, `TraceComputeOp`, `TraceDrawOp` (`metalRenderer.mm`). Входы замеряются до операции (после коммита), выходы — на границе следующей операции. Ненулевая доля: по 32-битным словам в гостевой памяти диапазона (через `GetBufferForGuestRange`). Размер образа: w·h·4 по T#, RT — 8294400 байт (1080p).
- Окно флипа 100: 25 операций (16 compute + 9 draw), совпадает с захватом кадра 100 по числу compute-энкодеров.
- Результат:
  - Все выходы (включая RT-выходы 9 draw, 4-байтные счётчики compute и storage-образ флипа) — 0.00%.
  - Draw: вершинных буферов во входах не найдено (`vertex_info` пуст для этих draw), поэтому входы draw не измерены.
  - Ненулевые входы: compute op 21 — буфер `0x8190e802c0` (8 байт) 100%; op 22 (композит) — буфер `0x8190e802d0` (240 байт) 78%, остальные входы 0% (сцена `0x8240000000`, `0x8240870000`, LUT `0x81b3c80000`, счётчик `0x82410e6000`); op 23 и 24 — 8-байтные буферы 100%.
  - Первая операция «ненулевой вход → нулевой выход»: op 21 (compute, хэш `0x92283f13…`): вход 100%, выход (4-байтный счётчик `0x82410e6000`) 0%.
  - Входы, которые никакая операция флипа не пишет: LUT `0x81b3c80000` (4096 байт). Его продюсер — compute `17aeef…` — в окне флипа 100 не выполняется (он работает на старте и даёт нулевой LUT). Сцена `0x8240000000`/`0x8240870000` записываются draw-операциями этого же флипа, которые выдают нули.
- Вывод (предварительный): чёрный кадр даёт цепочка «LUT нулевой (продюсер на старте даёт 0) + сцена нулевая (draw без измеренных входов)». Первая точка разбора — draw: откуда его вершинные данные (SRT/user-данные, а не `vertex_info`), и почему её RT остаётся нулевым; вторая — продюсер LUT `17aeef…` на старте.
- Не сделано: измерение входов draw (через SRT/user-данные), пересчёт по нескольким флипам, исправление.

### Residency в MSL-only графическом пути (сверка с захватом draw #27), 2026-10-21
- Проверено по коду: residency для буферов и образов, которые биндер передаёт прямому MSL-ядру, уже есть: `BindGuestBuffer` (`UseResource` на буфер гостевого диапазона), образы в цикле `BindMslStage` (`UseResource`), SRT (`UseResource`). `EnsureActiveRenderPass` вызывается в `DrawAuto` (строка ~2413) раньше `BindMetalShaderResources` (~2575), поэтому вызовы попадают в открытый render-энкодер.
- Захват draw #27 (TriangleStrip, 4 вершины, позиции 0): в Bound Resources только `ab`, `pc_data`, `sizes`. Это соответствует логу биндера: для первых draw `[Bind] MSL … bufs=0 imgs=0` (8 первых вызовов), а среди последующих есть `bufs=4 imgs=0` и `bufs=1 imgs=1`. То есть у draw #27 пустой список ресурсов снимка — биндеру нечего делать резидентным.
- Вывод: не residency, а пустой ресурсный снимок для этих draw. Вершинные буферы для MSL-VS берутся из снимка (`bufs=4`), а `vertex_info` в MSL-ветке не используется (`vs_msl ? nullptr : vs_info.buffers`), и в трассировке кадра 100 у draw вершинных буферов из `vertex_info` нет. Причина пустого снимка для intro-draw — не выяснена (вероятно, материализация ресурсов для программы без V#-дескрипторов в bindings или V#, не попавший в снимок).
- Следующий шаг: для draw с `bufs=0` выяснить, откуда VS читает позиции (какой V# или SRT-адрес используется в RDNA-коде), и почему этот дескриптор не материализован в `snap.buffers`.

### Корень нулевых позиций: MSL-диспетчер путал id блока с индексом (2026-10-21)
- Причина: терминатор блока (`CFG::Terminator::true_block/false_block`) ссылается на `BlockInfo::id`, а не на позицию в `program.blocks` (SPIR-V-бэкенд разрешает это через `TargetBlock`). MSL-эмиттер (`backend/msl/MslEmitter.cpp`) писал `pc = <id>` как индекс. Для `0x5c4469c2` вход (блок 0) получал `pc = 0u`, то есть крутился сам в себе до guard `kyty_iterations > 65536` и возвращал нулевой `out`. Отсюда позиции `(0,0,0,0)`, w=0, всё отсекается.
- Проверка «экспорт pos0 → [[position]]»: эмиттер назначает `[[position]]` из `Position` index 0 (`MslEmitter.cpp:145-148`, экспорт в `:788-790`). Вспомогательные экспорты (index != 0) и Layer/PointSize отбрасываются, pos0 не теряется. Все VS-дампы содержат `[[position]]`. Гипотеза «discard теряет pos0» не подтвердилась.
- Фикс: `BlockIndex(id)` (линейный поиск по `program.block_info`) используется для `pc = ...` в обеих ветках терминатора и в `EmitPhiCopies` (теперь принимает id). Неизвестный id даёт `blocks.size()` и пропускается.
- Проверка: перегенерированы 119 дампов MSL (`/tmp/msl_direct`). Ни один не имеет самоцикла входа; все цели переходов лежат в диапазоне `case`. `0x5c4469c2` теперь: `case 0: pc = 1u; break;`.
- Кадр 100 после фикса всё ещё чёрный (0 ненулевых сэмплов в `/tmp/fix1/f100.ppm`). Значит, фикс снимает одну причину, но не единственную; draw #27 и первую «прорисовку» надо перепроверить отдельно.
- Открыто: при `KYTY_DUMP_FRAME` процесс падает после кадра 100 (flip 200–275, `Unhandled host exception … runtimeLinker.cpp:750`). Без dump-переменных тот же билд доходит до flip 4850 без падения. Связь с фиксом не установлена, это может быть дамп-путь или гонка.
- Замечание: CLAUDE.md и run-команда используют цель `kytx_emulator`, но в CMake цель `kyty_emulator` (symlink `kytx_emulator` создаётся при линковке).

### Сквозная проверка после BlockIndex (2026-10-21)
- Захват кадра 100: `/Users/tx/Developer/fex-spike/runs/compare/kytx_f100_v3.gputrace` (переименован из `frame100.gputrace`).
- Таблица операций флипа 100 (`KYTY_TRACE_OPS_FLIP=100`), 25 операций. Раньше все draw писали нули; теперь 8 draw (`0x3d28ce…` и `0x5c4469c2…`) пишут в RT `0x8240870000` с nz=50.97%. Фикс сработал на draw: их выход стал ненулевым.
- Композит `0xb3300709113f5521` (op 22) читает этот RT (nz=50.97%), LUT `0x81b3c80000` (nz=0%) и пишет в `0x8179800000`, но выход nz=0%. Композит даёт чёрный кадр.
- LUT-продюсер `0x17aeefc07e25295e` работает на флипе 2 (не в окне 100): вход `0x8190c80000` 20 байт, nz=100%; выход `0x81b3c80000` nz=0%. Продюсер пишет `ab.image_0.write(...)` (texture3d, id 64) безусловно (`if (true)`), значит пишется нулевой цвет или текстура не та, что в трассе. Формат 125 в `metalTexture.mm` не найден (поиск по «125» пуст). Это следующий блокер.
- Кадры (PPM, `KYTY_DUMP_FRAME=20,100,300`): все три чёрные (nz=0.0%, mean RGB 0,0,0). `/tmp/fix2/frame_*.ppm`.
- Падение из прошлой записи не воспроизвелось в этом прогоне (до flip 5350 без падения). Значит падение нестабильное, и привязка к dump-пути не доказана.
- Ожидание пользователя: посмотреть окно во время запуска. Я этого проверить не могу.

### LUT-продюсер 0x17aeef и identity-LUT (2026-10-21, продолжение)
- Identity-LUT (`KYTY_DIAG_LUT_IDENTITY=1 KYTY_DIAG_CONSUMER=0xb3300709113f5521`): LUT подменён в композите, но кадры 20/100/300 всё равно чёрные (nz=0%). Значит, LUT не единственная причина: композит выдаёт нули и при единичном LUT.
- LUT-продюсер после фикса BlockIndex всё ещё пишет нули. Проверено readback'ом самой текстуры GPU (не гостевой памяти): `32x32x32 fmt=125 (RGBA32F) nz=0%`, угол и центр (0,0,0,0). Ранее «out nz=0» по гостевой памяти для image-выходов вводило в заблуждение: GPU-запись в текстуру гостевой памятью не видна.
- Входной буфер продюсера (`0x8190c80000`, 20 байт): dw0..dw4 = `1.0, 1.0, 100.0, 222.861, 1`, остальное 0 (получено флагом `KYTY_TRACE_WORDS=1`). Промежуточные значения не пробовались. Подозреваемые: ветка `v19 = (v14 > 0)` (по dw4) и цепочка `log2`/`exp2`: при NaN/inf `fmax(0, …)` даёт 0. Не подтверждено.
- Композит (`0xb330`) пишет в `0x8179800000` формат `fmt=90` (BGRA8Unorm, приватная память): readback без blit невозможен, поэтому его выход на GPU пока не измерен.
- RT `0x8240870000` (вывод draw) readback через UMA нестабилен между запусками: 50.97%, 0%, 61.59%. Это, вероятно, гонка чтения, а не реальная разница.
- Падение с `KYTY_DUMP_FRAME` не повторилось в последних трёх запусках (flip до 5975). Нестабильно, причина не найдена.
- Инструменты, добавленные в этом шаге: `KYTY_TRACE_WORDS=1` (первые 8 dword входа), readback текстур в трассе (`TraceTextureOutput`, приватная память пропускается, поддержаны RGBA32F и (BGRA|RGBA)8).
- Следующие шаги: (1) blit приватной текстуры композита в shared-буфер для readback; (2) детерминированная синхронизация перед readback UMA; (3) зонд промежуточных значений продюсера (запись `v19`, `v72`, `v280..v282` и результатов log2/exp2 в отладочный буфер для потока (0,0,0)).

### Зонд входов композита 0xb330 и путь презентации (2026-10-21)
- Добавлен диагностический хук: `KYTY_MSL_OVERRIDE_FILE=<path> KYTY_MSL_OVERRIDE_HASH=<hash>` (`pipelineCache.cpp`, только при `KYTY_MSL_ONLY=1`): компилирует заданный MSL вместо эмитированного для одного шейдера. Используется только для зондов.
- Зонд 0xb330: 4 панели по x (`a` сырой sample `v45`, `b` LUT sample `v340`, `c` константы `v7/v52/v363`, `d` оверлей `v668` сверху / sample `v1842` снизу). Все панели в `/tmp/fix4/p_100.ppm` чёрные.
- Контрольный зонд с константной серой панелью (0.5 в `c`) тоже не виден в кадре 100. Значит, записи композита не доходят до показанного кадра. Зонд входов пока не даёт ответа.
- Причина по логам: презентация берёт `flip_rt` / `last_rt_tex` (`swapchain.cpp:1118-1133, 1195`), и в поздних кадрах всегда `0x7c5fd38c80` (last_rt=0x8240c00000, fmt=70), то есть draw-RT, а не выход композита. Флип-буферы крутятся по три (`0x8179800000`, `0x817a800000`, `0x817b800000`), а композит в трассе кадра 100 пишет только в `0x8179800000`. Для остальных адресов композит-выход не привязан к флипу.
- Следующий шаг: (1) выяснить, в какой флип-адрес пишет композит в каждом кадре (лог регистрации compute-written image по адресу); (2) в свапчейне для текущего `flip_vaddr` брать `GetComputeWrittenImageForAddress` в первую очередь (как Vulkan-путь `ResolveSurface`); (3) повторить зонд и кадры 20/100/300.
- Не закоммичено.

### Презентер: выбор текстуры по адресу флипа; GPU page fault (2026-10-21)
- `swapchain.cpp`: флип резолвит свой адрес: сначала compute-образ по адресу (выход композита), затем draw-RT по этому адресу. Откат на `last_rt_tex` в текстурном пути убран. Если текстуры нет, презентуется чёрный кадр, сообщение `[Swapchain] no texture at flip vaddr=…` выводится один раз.
- Подтверждено логами: презентуемые текстуры теперь меняются по флипам (три адреса `0x8179800000`, `0x817a800000`, `0x817b800000`). Раньше все кадры шли из одной RT `0x7c5fd38c80`.
- `metalRenderer.mm`: `GuestTextureUsage` добавляет `ShaderWrite` для `RGB10A2Unorm` (fmt 90). Композит и флип-образы имеют этот формат. Прежде запись в них молча отбрасывалась. Ожидание проверки: в текущем прогоне эффект не виден, см. ниже.
- Контрольная серая панель зонда не видна ни в кадрах 20/100/300, ни в дампах `flip_rt_30/60` (все нули).
- Причина: в каждом прогоне после фикса BlockIndex есть GPU page fault: `[Metal] command buffer error (2): Caused GPU Address Fault Error (…PageFault)`, затем все следующие командные буферы `Ignored` (`SubmissionsIgnored`). Всё GPU-выполнение после этого мертво, поэтому кадры чёрные. В прогонах до фикса (`/tmp/bind`, `/tmp/ops2`) падений нет.
- Гипотеза: фикс BlockIndex включил реальное тело шейдеров, которые раньше крутились в самоцикле и не обращались к памяти. Один из них (или draw/композит) читает невалидный адрес. Какой именно, не выяснено. Первое падение около флипа 6.
- Следующий шаг: найти падающий шейдер. Например, временный env для пропуска конкретного хэша (bisect по `0xb330`, `0x17aeef`, VS `0x3d28`/`0x5c44`), либо Metal GPU capture сразу после падения.
- Расширен `[Reg]`-лог: теперь все адреса `0x8179…0x817b`, формат/размер/usage текстуры.
- Не закоммичено.

### Бисект GPU page fault по хэшам (2026-10-21)
- Добавлен диагностический env `KYTY_MSL_SKIP_HASH=<hash>[,…]` (`pipelineCache.cpp`, только MSL-only путь): шейдер считается неподдержанным, его draw/dispatch пропускаются.
- Метрика: число `Caused GPU Address Fault` за прогон ~80 с. Базовый прогон: 2–3 падения, первое около первых 2–3 кадров. Числа плавают между запусками, поэтому опора на число падений ненадёжна.
- Результаты (один запуск на хэш):
  - skip `0x17aeef` (LUT-продюсер): 3 падения, первое позже (кадр 6 против 3 в базе). Не устраняет.
  - skip `0x4fbf70`: 2 падения, без изменения картины.
  - skip `0x92283f`: 1 падение, последний флип 325 (остановка раньше).
  - skip `0xb33007` (композит): 1 падение, первое на строке 3452 лога, то есть после многих кадров. Затем host-исключение `runtimeLinker.cpp:750` (тот же класс, что и прежние падения с dump-переменными). Это самый сильный кандидат: без композита падение откладывается.
  - skip `0x7506048` (CS): 2 падения. skip `0x3d28ce` и `0x5c4469` (VS draw): 2 падения.
- Гипотеза: композит читает буферы через `ab.buffers[0]`/`ab.buffers[1]` (указатели на гостевую память, `v399 = ab.buffers[1][…]`) и образы, которые не сделаны резидентными для Metal. Тот же класс, что исправлен для compute в `BindGuestBuffer`, но не покрыт для всех путей указателей из argument buffer. Не проверено.
- Следующий шаг: проверить, что все буферы и образы, на которые ссылается `ab.buffers[*]`/`ab.image_*` композита, попадают в `useResource` перед его dispatch (в т.ч. буферы, адресуемые по указателю из `buffers[1]`). Затем повторить базовый прогон и считать падения.
- Не закоммичено.

### GPU page fault: поиск виновника (2026-10-21, продолжение)
- Исправлено реальное дефектное место: `ReadConstBuffer` (SRT-чтение константного буфера) в `MslEmitter.cpp` читал `ab.buffers[k][(off>>2)+imm]` без проверки границ, в отличие от `LoadBuffer`, который клампит по `sizes[k]`. Теперь чтение робастное: вне границ `0u`. Это корректность, а не лечение падения.
- Падения остаются. Пять прогонов с исправлением: 1–2 падения, первое около 2–6 кадров.
- Бисект по шейдерам (env, по умолчанию выключено):
  - `KYTY_MSL_SKIP_HASH`: пропуск `0x17aeef`, `0x4fbf70`, `0x92283f`, `0xb33007`, `0x7506048`, `0x3d28ce`, `0x5c4469` по отдельности. Падения остаются во всех вариантах (1–3). Пропуск композита откладывает первое падение, но не убирает его.
  - `KYTY_MSL_SKIP_STAGE`: пропуск всех stage 1 или всех stage 2 не убирает падения (2 и 2 падения).
- `KYTY_FORCE_FLIP_BUF=1` (презентация по гостевому буферу, без текстурного пути): 1 падение. Презентер не причина.
- Вывод: падение не связано с одним шейдером и не с презентером. Вероятно, GPU-работа вне шейдеров (копии/blit, загрузки текстур `replaceRegion`, детайлинг, UMA-readback, FlipReg) или доступ, который не покрыт резидентностью.
- Следующий шаг: Metal GPU-захват прямо перед первым падением (`KYTY_GPU_CAPTURE=<flip>` на кадре 2–3) и проверка команд копирования/blit и резидентности всех ресурсов, которые эти команды трогают.
- Не закоммичено.

### Атрибуция GPU fault через MTLCommandBufferEncoderInfo (2026-10-21)
- Инструментация, включается `KYTY_MTL_FAULT_INFO=1` (по умолчанию выключено):
  - командные буферы создаются с `errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus` (`metalContext.mm`);
  - encoder'ы помечаются (`render`, `compute`), на каждый draw и dispatch вставляется signpost (`draw #N rt0=…`, `cs hash=…`) через `KytyFaultOpSet` (`metalDraw.mm`, `metalRenderer.mm`);
  - в completion handler при ошибке печатается `[FaultInfo] encoder=… state=… signposts=…` для каждого encoder (`metalContext.mm`);
  - `[CsBuf]` логирует для каждого compute-буфера адрес, размер, длину Metal-буфера и смещение.
- Результат (1 прогон): падающий encoder в состоянии `Faulted` (state=4) — `cs hash=0xb3300709113f5521` (композит). Остальные dispatch'и в буфере — `Pending` (3) или `Completed` (1). Иногда падает draw (`draw #4 rt0=0x8240000000`), то есть падение плавает между encoder'ами.
- Исправления, которые не сняли падение:
  - `ReadConstBuffer` теперь с проверкой границ (`sizes[k]`), как `LoadBuffer`;
  - `sizes[b]` для compute клампится к реально доступному размеру Metal-аллокации от смещения (`buf_len − offset`) и повторно загружается в буфер 25. Композит: буфер 1 — 4 байта по `max_byte_extent`, буфер 0 — 240 байт, аллокация 16 КБ со смещением 80 и 8 КБ со смещением 8192.
- Следующий шаг: определить ресурс, к которому обращается падающий dispatch композита. Кандидаты: сэмплируемые образы (`image_0`, `image_4`, 3D LUT `image_2`) и их резидентность (`cs_residency`), запись `image_5` (RGB10A2 с недавно добавленным `ShaderWrite`). Для этого логировать резидентность каждого образа и буфера перед dispatch композита.
- Не закоммичено.

### Metal shader validation на compute (2026-10-21, продолжение)
- Запуск с `MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 MTL_DEBUG_LAYER=1`: в режиме по умолчанию процесс прерывается (`Abort trap: 6`) на первом нарушении, поэтому GPU fault не виден. Для подсчёта использован `MTL_SHADER_VALIDATION_FAIL_MODE=allow`.
- Базовый прогон (allow): 1566 сообщений за ~1475 флипов:
  - 1450 `Invalid store … at offset 0` (`main0`);
  - 81 `Invalid load … at offset N` (N до 2564);
  - 21 `Texture is not resident or MTLResourceUsage … does not match`;
  - 12 `Invalid load … at GPU address 0x40`;
  - 2 `Null texture access`.
- Бисект по счётчику валидации (один хэш пропущен за раз):
  - пропуск `0x4fbf70`: 1077 сообщений, стор-ошибок 3 (было 1450). Виновник основной массы сторов.
  - пропуск `0x92283f`: 773 сообщения, стор-ошибок 660.
  - пропуск `0x7506048`: 1845 сообщений, стор-ошибок 1744. Не виновник, но тоже даёт стор-ошибки.
- `0x4fbf70`: единственный стор — `ab.buffers[0][min(((v4*4)>>2), (sizes[0]>>2)-1)] = v0`. Буфер 0 — `0x82410e0000`, 4 байта, стабильный объект (`buf=0x77bed68c80`, `gpu` совпадает с `[buffer gpuAddress]`). Клампа индекса к 0 достаточно, чтобы адрес был в пределах 4 байт, поэтому ошибка «store at offset 0» при валидном буфере требует другого объяснения: не то буферное значение в слоте, либо ресурс без нужного usage/резидентности на момент записи. Не проверено.
- Инструментация (env-only, default off): `[CsBuf]` печатает `gpu`, `buf`, `resident_gpu` для каждой привязки compute.
- Следующий шаг: (1) проверить, что для `0x4fbf70` в `[CsArg]`/дампе ab слот 0 содержит ожидаемый адрес на каждом dispatch (вариант: `KYTY_PROBE_HASH=0x4fbf70` + `KYTY_CS_PROBE`); (2) сверить `useResource` для этого dispatch (Read|Write) с реальным usage записи; (3) разобрать 21 сообщение о резидентности текстур: какие образы не попали в `cs_residency`.
- Не закоммичено.

### Валидация: размеры буферов и нулевые указатели (2026-10-21, продолжение 2)
- Пункт 2 (размер буфера): для compute размер теперь `max(static extent, V#.dwords[2] × stride)` (как для пиксельной стадии, `metalRenderer.mm`, блок `sizes`). Для `0x4fbf70` буфер 0 теперь 24576 байт из 32768 (было 4 байта).
- Результат: `Invalid store … at offset 0` остались (1421 против 1450). Размер не был причиной. Все стор-ошибки идут из `0x4fbf70` (пропуск хэша: 3), и стор всегда по offset 0.
- Что проверено: буфер стабилен (`buf` и `gpu` не меняются между диспатчами, `gpu` совпадает с `[buffer gpuAddress]`), смещение 0, размер достаточный. Остаются гипотезы: (a) буфер обёрнут через `newBufferWithBytesNoCopy` (`metalBuffer.mm:74`) над гостевой страницей, которая не доступна на запись; (b) слот `ab.buffers[0]` в аргументном буфере не совпадает с этим буфером на момент записи. Не проверено.
- Не сделано: пункт 1 (нулевые указатели, `GPU address 0x40`, 19 сообщений) и пункт 4 (21 текстура «not resident»). Пункт 3 (81 invalid load) ждёт пункта 2.
- Не закоммичено.

### Защита страниц гостевой памяти (2026-10-21, продолжение 3)
- Проверка гипотезы «no-copy буфер над write-tracked страницами»: при обёртке (`WrapHostMemory`, `metalBuffer.mm`) и при каждой compute-привязке (`[CsBuf] … prot=`, `metalRenderer.mm`) текущая защита страниц всегда `3` (RW). Ни одной `-1` или read-only. Гипотеза не подтверждена, и мьютекс-доступ к защите не объясняет ошибок.
- Остаётся: `Invalid store … at offset 0` для `0x4fbf70` (≈1400–1700 на прогон), несмотря на RW, валидный размер (24576 из 32768) и стабильный объект буфера.
- Не сделано: пункт про нулевые указатели (`GPU address 0x40`), 21 нерезидентная текстура, 300-флиповый прогон, кадры 20/100/300, проверка окна пользователем.
- Диагностика, добавленная в этой серии (env-only, по умолчанию выключено): `[WrapProt]`, `prot=` в `[CsBuf]`, `[FaultInfo]`, `KYTY_MSL_SKIP_HASH`, `KYTY_MSL_SKIP_STAGE`, `KYTY_MSL_OVERRIDE_FILE`.
- Следующие шаги: (1) выяснить, что validation считает «store at offset 0» для буфера, который Metal видит как корректный: сравнить `[alloc.buffer length]`, `gpuAddress` и адрес, который шейдер получает через `ab.buffers[0]` (подкласс `[CsArg]`/дамп ab на dispatch `0x4fbf70`); (2) проверить, не подменяется ли слот 0 позже, например резервными `def_tex`/`def_samp` (`metalRenderer.mm` строка с `arg_ptrs[slot]==0`); (3) затем пункты про NULL-указатели и текстуры.
- Не закоммичено.

### Invalid store 0x4fbf70: путь привязки и адресное пространство (2026-10-21, продолжение 4)
- MSL 0x4fbf70: `constant KytyArgumentBuffer& ab`, `device uint *buffers[64]` (не const), запись `ab.buffers[0][min(...)] = v0`. Адресное пространство корректно.
- Путь привязки: argument encoder (`cs_argenc_here`, `metalRenderer.mm`) и `SetComputeBytes(arg_ptrs)` дают одинаковую картину. Со `KYTY_CS_ARGENC_OFF=1`: invalid=1927 (store 1820, load 86); по умолчанию: invalid=1696 (store 1554, load 114). Значит, не путь привязки.
- Гипотеза «буфер read-only» не подтверждена: защита страниц RW (prot=3), размер корректный (24576 из 32768), `buf`/`gpu` стабильны.
- Не объяснено, почему Metal считает store в offset 0 невалидным для буфера, который видит как валидный. Возможные причины, не проверены: no-copy буфер (`newBufferWithBytesNoCopy`) с нестандартным выравниванием/длиной аллокации; буфер под тем же адресом, что и другой no-copy wrap (страница 0x82410e0000 входит в несколько wrap'ов: `GuestMemoryUMA` с `page=0x82410e0000` и `page=0x82410e4000`, длина 32768 каждый, то есть они перекрываются).
- Следующий шаг: проверить перекрытие no-copy wrap'ов для одной гостевой страницы (два MTLBuffer над одними и теми же байтами, оба с write-usage). Если перекрываются, сделать один wrap на регион или отдельный буфер с копированием.
- Не сделано: пункт про NULL-указатели (`GPU address 0x40`), 21 нерезидентная текстура, 300-флиповый прогон, кадры 20/100/300, просьба посмотреть окно.
- Не закоммичено.

### Загрузки с адреса 0x40 (2026-10-21, продолжение 5)
- Отложено: 0x4fbf70 (invalid store; см. предыдущий раздел, гипотеза о перекрытии no-copy wrap'ов не проверена).
- `Invalid load … at GPU address 0x40, executing compute function: "main0"` — ровно 8 сообщений в каждом прогоне, независимо от пропуска кандидатов: 0x7506, 0x4fbf70, 0x92283f, 0xb33007, 0x17aeef. Похоже, это одноразовые сообщения от шейдера, которого нет в списке кандидатов.
- Проверено: compute-буферы с нулевым GPU-адресом не встречаются (`[CsZeroPtr]` пуст). Push-константы (buffer 1) и `sizes` (buffer 25) передаются через `SetComputeBytes` на каждом dispatch, то есть не нулевые. Адрес 0x40 = dword 16 в `PushData` (`pc_data.dwords[16]`), но буфер 1 привязан, поэтому гипотеза «не привязан буфер 1» не подтверждена.
- Не сделано: (1) пройти все compute-хэши и найти тот, при пропуске которого 0x40 исчезает; (2) проверить, какой шейдер читает `constant` по смещению 0x40 (`ab` с `buffers[k]` = 0 или `pc_data.dwords[16]`). Для этого: дамп MSL всех compute, поиск `constant`-доступов по смещению 0x40 (`[16]` для `uint`).
- Не сделано: 21 нерезидентная текстура, 300-флиповый прогон с `KYTY_MTL_FAULT_INFO=1`, кадры 20/100/300, проверка окна пользователем.
- Не закоммичено.

### Корень GPU page fault найден и исправлен (2026-10-22)
- Метод: `KYTY_MTL_SYNC_EACH=1` (env, по умолчанию выключено) коммитит и ждёт каждый compute dispatch и печатает `[SyncOp] begin/end <label>`, поэтому отчёты shader validation привязываются к конкретному шейдеру. Скрипт разбора: `/tmp/bisect/attr.py`.
- Корень 1: **residency compute применялась до заполнения.** Цикл `useResource` по `cs_residency` (`metalRenderer.mm`, DispatchDirect) выполнялся раньше, чем список наполнялся в циклах привязки буферов и образов. На GPU не было резидентных ресурсов: отсюда `Invalid store/load at offset N`, `Texture is not resident`, и page fault. Исправлено: второй цикл перед `Dispatch` (после привязки), usage по `ResidencyUsage` (Write только для текстур с `ShaderWrite`).
- Корень 2: **реестр compute-образов хранил сырые указатели** (`s_cs_written_by_addr`). После замены текстуры в кэше указатель висел; при validation это давало `-[MTLGPUDebugBuffer width]: unrecognized selector` и падение. Исправлено: реестр держит `CFRetain`, заменённые текстуры уходят в «кладбище» на 512 записей.
- Дополнительно (корректность): размеры compute-буферов берутся и из V# (`dwords[2] × stride`), `sizes[b]` клампится к реальной аллокации Metal; `ReadConstBuffer` с проверкой границ; argument-encoder делает `useResource` для закодированных текстур и буферов.
- Результат: во всех прогонах после правок `Caused GPU Address Fault` = 0 (до правок 1–3 на прогон, всегда в первые 2–6 кадров). Invalid store 0x4fbf70: 1450 → ~90.
- Кадры 20/100/300 (`/tmp/fix6/f_*.ppm`): уже не чёрные, но равномерный тёмно-зелёный: 100% пикселей ненулевые, mean RGB (0, 22, 0).
- Что осталось:
  1. **GPU Hang** (не fault) на ~250–275 флипе: draw `vs=0xd159685704b5d622 ps=0xf77586259a9897ae`, RT `0x81b6f70000`. PS читает счётчик цикла `v0 = buffers[0][0]` (uint), а буфер `0x8190e80220` содержит `dw0=0x3f800000` (1.0f): цикл на ~1e9 итераций. Либо неверная трактовка (float/uint), либо неверный буфер в слоте 0. Не решено.
  2. **Композит 0xb330** пишет упакованные half-пары: `uint4(v2405, v2412, 0, 0)` как `as_type<float4>` в RGB10A2. Похоже на D16-хранение образа, которое эмиттер не распаковывает (декодер MIMG не передаёт D16 в IR). Отсюда однотонный зелёный кадр. Не решено.
  3. Остаются отчёты validation: `Null texture access` (~800) и `Texture is not resident` (~700) в других шейдерах (незаполненные слоты образов в native-раскладке `64+res` без дефолтной текстуры нужного типа), `Invalid load at GPU address 0x40` (0xe5941d54).
- Диагностика (env-only, default off): `KYTY_MTL_FAULT_INFO`, `KYTY_MTL_SYNC_EACH`, `KYTY_MSL_DUMP_BY_HASH` (дамп `mh_<variant hash>.metal`), `[CsBuf]`, `[CsImgBind]`, `[DrawBuf]`, `[WrapProt]`.
- Не закоммичено.

### Splash FC 27 рендерится; блокеры (2026-10-22)
- Исправления эмиттера и рендерера в этом шаге (все в `MslEmitter.cpp`/`metalRenderer.mm`/`metalDraw.mm`):
  1. **D16-образы**: `IMAGE_LOAD/STORE` с `data_bits==16` теперь паковали/распаковывали пары half (`kyty_d16_pack/unpack` в преамбуле MSL). Раньше композит писал сырые биты как float (однотонный зелёный).
  2. **Сжатые экспорты MRT (`compr`)**: распаковка `half2`/`unorm16` (было: сырые биты как float). Как в SPIR-V-бэкенде (`ExportVector`), невыведенная альфа = 1.
  3. **RECTLIST**: у гостя три вершины, четвёртый угол вычисляет железо. VS теперь emitted как `kyty_vs_impl` + `main0`; при `sizes[63]==1 && vid==3` угол = `v1+v2−v0`. Одиночный rect рисуется как `TriangleStrip` из 4 вершин (`metalDraw.mm`). Раньше рисовался один треугольник.
  4. **Маска записи цвета**: `CB_TARGET_MASK & CB_SHADER_MASK` (как в Vulkan-пути `DrawColorOutputMask`), раньше только `TARGET_MASK`.
  5. Вся серия: residency compute после привязки, реестр образов с retain, V#-размеры, снапшоты констант из арены CPU (0x8190…) — см. предыдущие разделы.
- Проверка: при пропуске шейдера-оверлея (`KYTY_MSL_SKIP_HASH=0x15300f6811462109`) в сцен-RT `0x8240870000` отрисована полная заставка «EA SPORTS FC 27» (100% ненулевых, `/tmp/rt_dump/s3_small.png`). Конвейер сплэша (VS `0x3d28ce…`, PS `0x724318…`, текстура 3840×2160 fmt 56) работает.
- Текущий блокер 1 (оверлей): после каждой отрисовки сплэша идёт rect-draw VS `0x5c4469…`/PS `0x15300f…` (prim=7, vtx=3, blend выключен, `tmask=0xf smask=0x3`): PS экспортирует константный ноль в R,G (`exp compr`, en=0x3). С маской `target & shader` сцен-RT становится (0,0,178), без маски — чёрный. Ни один вариант не даёт заставку; на железе этот проход, скорее всего, пишет в другой RT или другой формат (вероятно, 2-канальный). Нужен референс: что делают эти draw'ы в Vulkan-пути (`vk_f20.gputrace`) и какой формат у их RT. Не решено.
- Блокер 2 (GPU Hang на ~250 флипе): draw PS `0xf77586…` (хэш гостя `0x295efc6a…`) c RT `0x81b6f70000` (128 КБ): цикл `S_CMP_LT_U32 s4, s2` на 4096 итераций × пиксели, диспетчер `switch(pc)` примерно в 20× медленнее нативного кода. Реальное значение счётчика 4096 (`dw0=0x1000`), не ошибка данных. Решение: выпускать структурированные циклы вместо `pc`-диспетчера (в IR есть `loop_header/merge/continue`), либо дробить draw по тайлам.
- Диагностика: `KYTY_DUMP_DISASM`, `KYTY_MSL_DUMP_BY_HASH` (карта `[MSL] variant … from shader …`), `KYTY_LOG_DRAWS` (подпись draw с хэшами, масками, blend), `KYTY_DUMP_RTS_FRAME=<n>` (дамп всех RT).
- Не закоммичено.

### Оверлей-проход (PS 0x15300f / гость 0x374d5c65): это метаданные CB, не рисование (2026-10-22)
- Листинг RDNA: `V_MOV_B32 v0, 0; exp target=0x00 en=0x3 compr=0 v0,v0,v0,v0` — константный ноль в R,G. Draw: rect list (VS `0x5c4469c2`, 3 вершины), `blend=0`, `tmask=0xf smask=0x3`, RT тот же `0x8240870000`, формат гостя 10 (8_8_8_8) → Metal RGBA8Unorm, то же, что у сплэша.
- Причина: `CB_COLOR_CONTROL.MODE = 2` (eliminate fast clear). Такие проходы (MODE 2, 5, 6) пиксельный шейдер использует как носитель операции над метаданными цветового буфера; экспорт не применяется. Vulkan-путь их пропускает (`ConsumeMetadataColorOperation`, `renderDraw.cpp`). В Metal добавлен такой же пропуск в начале `DrawAuto` (`[DrawAuto] skip colour-buffer metadata pass`).
- Сверка с `vk_f20.gputrace`: напрямую не удалась. В `compare/mvk_msl/pipeline-*.txt` нет ни этого VS/PS, ни контрольной пары сплэша (`0x3d28ce…`): хэши Vulkan-сборки другие (вариантные). По коду Vulkan-сборка для этого draw конвейер не создаёт.
- Маска записи: `CB_TARGET_MASK & CB_SHADER_MASK` (как `DrawColorOutputMask` в Vulkan-пути), в Metal добавлено.
- Результат: сцен-RT `0x8240870000` = полная заставка (100% ненулевых), RT `0x8240000000` (HDR fmt 92) нулевой; кадры 20/100/300 пока чёрные.
- Следующий блокер: композит `0xb330` обнуляет результат. LUT 32³ после правок уже не нулевой по альфе, но RGB = 0 во всех вокселях (угол и центр (0,0,0,1)). Зонды продюсера `0x17aeef` (override): гейт корректен (`v19=1`, `v283=1.0`, `v14=1`), но промежуточные `v280..v282` и `v286/v289/v292` = 0. Ошибка в вычислении между входом и `fma`/`exp2`/`log2` (case 1–2 продюсера), не в записи текстуры. Нужен пошаговый зонд (`v57`, `v61`, `v63`, `v71`, `v79`, `v109..v112`).
- Hang (PS 0xf775): без изменений, см. предыдущий раздел (нужны структурированные циклы).
- Не закоммичено.

### LUT-продюсер 0x17aeef исправлен: ConvertU32F32 (2026-10-22)
- Метод: пошаговый след одного потока (tgid=(0,8,16), tid=(16,0), воксель 16,16,16) — MSL-вариант с записью каждого `vN` в debug-буфер (`buffer(26)`, `KYTY_CS_PROBE=1 KYTY_PROBE_HASH=… KYTY_CS_TRACE_FILE=…`, подмена через `KYTY_MSL_OVERRIDE_FILE`). Файлы: `/tmp/fix6/trace17.{metal,map,out}`.
- Причина: в `MslEmitter.cpp` `ConvertU32F32` эмитился как `as_type<uint>(x)` (побитовая перестановка) вместо численного `(uint)x`. Координата 16384.0 превращалась в 0x46800000, затем `& 0xffff` давало 0: все полупакованные значения входа были нулями, RGB LUT = 0.
- Исправление: `ConvertU32F32` → `(uint)(x)` (операнд уже насыщён транслятором). `ConvertS32F32` уже был численным.
- Проверка (readback текстуры, `KYTY_TRACE_OPS_FLIP=2`): LUT `32x32x32 RGBA32F` nz 19% → 84%; центр `(1.004, 1.005, 1.004, 1)`, угол `(0,0,0,1)`.
- Кадры 20/100/300 пока чёрные: композит `0xb330` ещё обнуляет вывод. Класс ошибки (побитовые вместо численных преобразований) стоит проверить в других `Convert*`/`Pack*` в эмиттере.
- Не закоммичено.

### Композит 0xb330: резидентность, null-слоты, зонд входов (2026-10-22)
- Резидентность текстур на compute-энкодере уже применяется после привязки (`[CsResident] composite: 6 resources, 4 textures made resident`, лог под `KYTY_MTL_FAULT_INFO`). Захват Opus `kytx_f100_v4` предшествовал правкам или не показывает `useResource`-ресурсы argument buffer в списке Bound Resources.
- Добавлено: для native-MSL compute каждый объявленный слот образа `64+res`, для которого нет дескриптора гостя, получает нулевую текстуру нужного типа (`NullImageFor`: 1D/2D/3D/cube/array, float/uint/sint, storage), а слоты сэмплеров `128..159` — сэмплер по умолчанию (раньше был 0). Новые объекты попадают в `cs_residency`.
- Валидация (SYNC_EACH, per-dispatch): у композита `0xb330` отчётов больше нет. Остались в других шейдерах: `0xe5941d` (load по 0x40, 986), `0x4231266a` (invalid load 1194, null texture 173), `0x91c4e2e9` (null texture 1134), `0x4b746f1f`/`0xac04a889`/`0xeaf1c360`/… (not resident / usage mismatch: вероятно, storage-образы форматов без `ShaderWrite` из `GuestTextureUsage`).
- Зонд входов композита (override, 4 панели) на flip 30 после исправления: серая контрольная панель в флип-текстуре видна (188) — запись композита доходит; панели `v45` (image_0, сэмпл HDR RT), `v340` (LUT), `v668`/`v1842` (image_3 read, image_4 sample) дают (0, 22, 0) / нули. Вход HDR-RT пустой по условию (это clear), LUT-сэмпл и чтение image_3/4 нулевые.
- Важное наблюдение: композит в установившемся режиме берёт разные RT по кадрам (`0x8245230000`, `0x8240c00000`, `0x8240000000`, `0x8240870000`): игра вращает адреса. Сплэш в `0x8240870000` композит видит лишь в ~300 из ~3000 dispatch'ей. Нужно: для кадра, который показывается, определить, какие RT читает композит, и есть ли в них содержимое (дамп RT по адресу и флипу).
- Не закоммичено.

### Композит: вход корректный, найдены ещё два перепутанных lowering'а (2026-10-22)
- Диагностика `KYTY_LOG_COMPOSITE_IN=1`: в установившемся режиме (флип ≥ 21) композит сэмплирует RT `0x8240870000` с заставкой: это тот же объект Metal, что и отслеженный RT (`same=1`, nz=99.8%). Значит, склейка «RT ↔ сэмплируемое представление» работает; вход не пустой. Выход (`0x817a/b/9800000`, RGB10A2) имеет nz=25%: ненулевая только альфа.
- Трассировка одного потока композита (`tools/msl/mktrace.py` + `tools/msl/chain.py`, пиксель 960,540): итог ≈ `fma(1/1024, dither, base)`, где `base` = 0, то есть RGB считается нулевым до дизеринга. Попутно найдены и исправлены два перепутанных lowering'а в `MslEmitter.cpp`:
  1. `ConvertF16F32` (F32→F16) и `ConvertF32F16` (F16→F32) были поменяны местами. Теперь `ConvertF16F32` → `half(x)`, `ConvertF32F16` → `float(x)` (для U16 — `float(as_type<half>((ushort)x))`).
  2. `BitCastF16U16` (U16→F16) и `BitCastU16F16` (F16→U16) были поменяны местами: числовое `half(uint)` вместо побитового `as_type<half>`.
- После этих правок кадры 20/100/300 всё ещё чёрные. Цепочка к базовому цвету длинная (ветвление без ветвей: условные выборы `v404 ? … : …`), в ней встречаются полупакованные значения `0xfc00` (−inf) и `0xab0a`; источники пока не разобраны. Входные `uint4`-результаты сэмплов (`v45/v88/v340/v668/v1842`) в след не попадают (трассируются только скаляры): расширить след на `uint4`.
- Инструменты: `tools/msl/mktrace.py <hash> gx gy tx ty` (MSL-вариант с записью каждого `vN` потока в debug-буфер), `tools/msl/chain.py <hash> <out> <vN,…> <depth>` (цепочка зависимостей по выполненным присваиваниям), `tools/msl/attr_validation.py <log>` (привязка отчётов validation к шейдеру при `KYTY_MTL_SYNC_EACH=1`).
- Не закоммичено.

### Заставка FC 27 на экране (2026-10-22)
- **Результат:** в представленных кадрах 60 и 100 полная заставка «EA SPORTS FC 27» (135/135 строк с содержимым, `/tmp/fix6/v_60.ppm`, `v_100.ppm`). Кадр 20 чёрный (до появления заставки), кадры 150 и 300 чёрные (затухание или GPU hang, см. ниже).
- **Корень чёрного экрана композита:** метаданные CMASK цветового буфера. Композит читает таблицу битов видимости тайлов (`ab.buffers[1]`, `0x82410e6000`), и только при ненулевом бите читает слой UI/заставки (`image_3`). Таблицу строит `0x92283f` из байтов CMASK RT (`0x82410e0000`, формат `BUFFER_LOAD_FORMAT_X`, бит = байт ≠ 0). Игра обнуляет CMASK compute-заливкой `0x4fbf70` (`BUFFER_STORE_FORMAT_X`, значение из user data), а отметки «тайл записан» должен ставить аппаратный CB при рендеринге. Metal их не пишет, поэтому таблица была нулевой.
- Доказательство: подмена `v399 = 0xffffffffu` в композите (override) сразу давала заставку в флип-текстуре.
- **Исправление (`metalRenderer.mm`, `DrawAuto`):** после draw'а в RT с `cmask.addr != 0 && cmask_fast_clear_enable && !dcc` блит-заливкой (`fillBuffer`, значение 0xFF, в порядке GPU после очистки игрой) отмечаются тайлы. Размер: `max(tiles/2 → 256, 24576)` байт (swizzle раскидывает метаданные шире `tiles/2`; следующий буфер начинается через 24 КиБ). Лог `[Cmask] mark touched …`.
- Остаются: (1) GPU hang на ~250 флипе (draw PS `0xf775…`, структурированные циклы), (2) формат `BUFFER_*_FORMAT_X` эмиттер игнорирует (store/load пишутся как dword; для CMASK размер элемента 1 байт), (3) метаданные DCC (`dcc_addr`) не эмулируются, (4) метаданные записываются при каждом draw, а не по реально покрытым тайлам.
- Прочие исправления этой серии (все в эмиттере): `ConvertU32F32` (численное), перепутанные `ConvertF16F32`/`ConvertF32F16` и `BitCastF16U16`/`BitCastU16F16`, D16-образы, сжатые экспорты, RECTLIST (4-я вершина), маска записи `target & shader`, пропуск проходов метаданных CB (`CB_COLOR_CONTROL.MODE` 2/5/6).
- Не закоммичено.

### GPU hang исправлен: лимит итераций диспетчера (2026-10-22)
- Подтверждение по логам: после первых сабмитов с зависанием (8× `Caused GPU Hang Error`) следующие командные буферы получают `Ignored (for causing prior/excessive GPU errors)`; очередь отравлена, презентер держит последнюю картинку (заморозка на заставке). Устройство не восстанавливается.
- Причина: не медленный код. Измерено подменой счётчика цикла PS `0xf775` (override): время растёт линейно (~0.09 мс/итерация, 1500 итераций = 130 мс, 3000 итераций с поднятым лимитом = 268 мс), но при счётчике ≥ 3000 с лимитом `kyty_iterations > 65536` каждый поток упирается в лимит диспетчера (около 35 итераций диспетчера на итерацию гостевого цикла, то есть ~1900 итераций цикла) и получается hang. Реальное значение счётчика 4096, это легитимный цикл.
- Исправление: лимит итераций диспетчера `65536` → `1048576` (`MslEmitter.cpp`, оба пути: цикл и `kyty_iterations` в ветке `use_goto`). Максимальное время командного буфера 372 мс, GPU-ошибок 0 за 120 с (до ~флипа 2200).
- Структурированный вывод `goto`/метки в MSL невозможен (`labeled statements are not supported in Metal`), код оставлен выключенным (`use_goto=false`); настоящая структуризация (while/if) не нужна для hang.
- Кадры (`/tmp/fix6/x_*.ppm`): 60/100/150 — заставка; 300/600 — другая картинка (54% содержимого): половина заставки + зелёные блоки; 1000 — почти чёрный с красными полосами. То есть игра дошла до следующего экрана, но его рендеринг неверный. Подозрения: RECTLIST с `vertex_count != 3` (диагональный разрез снова), блоки мусора (форматы `BUFFER_*_FORMAT`, DCC), форматированные store/load эмиттер игнорирует.
- Диагностика: `KYTY_MTL_GPU_TIME=1` (лог командных буферов > 20 мс).
- Не закоммичено.

### Интро и промежуточные кадры: исправления рендеринга (2026-10-22)
- Скриншоты пользователя: на интро нижняя правая половина экрана была чёрной (диагональ), в левом верхнем углу виднелись уменьшенные копии кадра с зелёными блоками, дальше красные полосы. Диагностика: RT `0x8240c00000` при ~флипе 641 (`KYTY_LOG_COMPOSITE_IN`, `[CompQuad]`: TL=100, TR=51, BL=51, BR=0), дамп RT на кадре 645 (`KYTY_DUMP_RTS_FRAME`).
- Исправлено:
  1. **Индексы и вершины из кольцевой памяти CPU**: небольшие index/vertex-буферы (≤ 16 КиБ) копируются в динамическое кольцо при кодировании (`SnapshotBytes`, `DrawIndex`); GPU исполняет draw позже, и CPU успевал перезаписать кольцо: терялся второй треугольник квада (чёрная половина) и появлялся мусор. Снапшоты констант (арена 0x8190…) были сделаны раньше.
  2. **Алиасинг транзитной памяти**: один адрес (`0x8240c00000`) хранит то RT 1920×1080, то пирамиду 480×270 из 6 уровней. Compute-образ с размером, не совпадающим ни с одним RT на адресе, теперь получает отдельную текстуру (`AliasedImageFor`, лог `[AliasImg]`). T# `base_level/last_level` → вид по диапазону уровней (`MipRangeView`). Уменьшенные копии и зелёные блоки в углу исчезли.
  3. **Render-to-mip**: `CB_COLOR_VIEW.MIP_LEVEL` и `CB_COLOR_ATTRIB2.MAX_MIP` учитываются; цель с несколькими уровнями создаётся как настоящая mip-текстура и рисуется в нужный уровень (`[RtMip]`).
  4. **Индексированный RECTLIST** (3 индекса): два треугольника `(i0,i1,i2,i1,i2,SENT)` + синтез 4-й вершины в обёртке VS (`sizes[59..62]`, `sizes[63]==2`).
- Результат: кадры 645 и 1000 (`/tmp/fix6/z_*.ppm`) — полноэкранные сцены без диагонали и без мусора; GPU-ошибок нет. Цвет фона на 645 жёлто-зелёный (на заставке был бирюзовый); смена темы игры или канальный порядок — не проверено.
- Неподдерживаемые шейдеры (94 за прогон до флипа 3500): PS — `ImageGatherRaw` (compare/offset, depth) 28+, `LoadAddressU32` 4, куб-сэмплы 3+; CS — `SharedAtomicIAdd32/Or32/UMin32`, форматированные `StoreBufferU32/x4` (формат игнорируется), `DataAppend`, `BufferAtomicSwap32`, `ImageQueryDimensions`, `BitCount64`; VS — `ReadFirstLane`. Draw с неподдерживаемым шейдером пропускается (счётчик `dropped_ps`, 736 к флипу 3500). Красный/пурпурный в меню — вероятно, результат этих пропусков и ещё не транслируемых форматов; фоллбэк-PS включается только `KYTY_DEBUG_PS` (по умолчанию выключен).
- Диагностика (env-only): `KYTY_LOG_PRIMS`, `KYTY_LOG_DRAWS(_N)`, `[IdxLabel]`, `[PrimHist]`, `[CompQuad]`.
- Не закоммичено.

### Шейдеры: неподдерживаемые 94 → 2 причины; гамма; память (2026-10-22)
- Добавлено в `MslEmitter.cpp` (атомики и `BitCount64` написал Haiku-агент по моему ТЗ, остальное сам):
  - LDS-атомики `SharedAtomic*32` (compute: `threadgroup atomic_uint`, остальные стадии — обычное RMW по per-thread массиву), буферные атомики `BufferAtomic*32` (Swap/CmpSwap/IAdd/ISub/And/Or/Xor/UMin/UMax/SMin/SMax), `BitCount64`.
  - Gather с compare/offset: compare — сравнение собранных текселей с опорным значением (LESS_EQUAL), offset — сдвиг координаты на `offset/размер текселя` (упакованное 6-битное знаковое x,y). Depth-compare сэмпл — выборка и сравнение без PCF. Integer gather разрешён. Depth-образы (`depth_compare`) больше не отвергаются.
  - `ImageQueryDimensions` (`get_width/height/depth/array_size/num_mip_levels` на уровне `lod`).
  - Форматированные store: все 32-битные форматы (1–4 канала) как сырые dword, RGBA16F (`desc_format=71`) как `half4`.
- Итог прогона 150 с: `dropped_ps=0`, `compile_fail=0`, 0 GPU-ошибок, пропущено шейдеров 2 причины: `ImageSampleRaw (cube sample)` (4 CS) и `DataAppend` (4 CS, GDS-счётчик без привязки в MSL). Куб-сэмпл нужно обратно собирать направление из (sc, tc, faceid) после `v_cube*`; `DataAppend` нужен GDS-буфер.
- Гамма: презентер (`fs_present_tex`) делал линейный→sRGB для всех текстур, но финальный композит игры уже display-referred. Теперь прямая копия для RGBA8/RGB10A2, sRGB-кривая только для линейных `RG11B10F`/`RGBA16F`. Заставка стала тёмно-бирюзовой (богатые тона, а не размытая яркая).
- Память: `KYTY_MEM_LOG=1` (раз в 3 с: footprint, `currentAllocatedSize`). За ~150 с (4600 флипов) память стабильна: footprint ≈ 6.9–7.2 ГБ (после скачка 1.8→6.9 ГБ в первые ~470 флипов), Metal ≈ 3.2 ГБ, роста нет. Зависание Mac, вероятно, из-за общего объёма (≈7 ГБ процесс + 3.2 ГБ Metal) на машине с ограниченной памятью, либо из-за GPU-нагрузки WindowServer; в 3 прогонах командных буферов > 400 мс не было (один раз раньше наблюдался буфер 26 с, не воспроизвёлся). `KYTY_MTL_GPU_TIME=1` теперь печатает операции каждого длинного буфера.
- Кадры 1000 и 1500 (меню): тёмный оливковый фон со световыми полосами, UI/кнопок не видно, хотя UI-draw'ы (≈367 тыс. за прогон, prim 4 vtx 6) идут в RT `0x8240c00000`. Следующий шаг: найти, почему UI-слой не попадает в композит (метаданные CMASK/DCC для этого RT? `rt_cmask` помечается только при `cmask_fast_clear_enable`) и проверить RT UI на кадре меню.
- Не закоммичено.

### Цвета интро исправлены (T# DST_SEL); причина пустого UI (2026-10-22)
- Отчёт пользователя: на заставке цвета верные, на интро (кадр ~992) фон жёлто-зелёный вместо бирюзового, на экране ошибки EA (кадр ~1461) нет диалога.
- Интро: в T# часть текстур (`fmt=56`, размеры 32×64…3840×1080) имеет `DST_SEL = 6547` (B,G,R,A), Metal-путь DST_SEL не применял, и R/B менялись местами. Теперь для неединичного DST_SEL (0 = ноль, 1 = единица, 4..7 = X,Y,Z,W) биндится swizzle-вид текстуры (`ApplyDstSel`, `newTextureViewWithPixelFormat:...swizzle:`, macOS 15; кэш по (текстура, селектор)); в graphics и compute (только сэмплируемые). Проверено кадром 800: фон тёмно-бирюзовый. Лог `KYTY_LOG_TEX=1` (`[GfxTex]`: формат, размер, dst_sel, доля ненулевых байт гостевой памяти).
- Draw'ы теперь берут compute-записанный образ по адресу (`GraphicsImageFor`), если размер совпал; раньше брали пустую копию гостевой памяти. Это породило первый белый UI-элемент на экране ошибки (кадр 1461), но не сам диалог.
- Корень отсутствия UI: атлас 2048×2048 R8 (`0x81b5540000`, `0x81b5030000`, T# fmt=1, tile=9) гостевая память пуста; его заполняет compute `0xe5941d54` (8×8 потоков, чтение `texture2d<uint>` → запись `texture2d<uint>`): копирование блоков между образами с разными форматами на одной памяти (источник `512×512` fmt 75 = RGBA32Uint, приёмник T# fmt 10/13/70). Это копия памяти «через алиасинг форматов»; в Metal это разные объекты, и запись в RGBA32Uint-образ не попадает в R8-текстуру, которую сэмплирует UI. Проверка зондом: подмена UI PS (`0xf4cac03b…`) на вывод сэмпла атласа давала чёрный кадр; на вывод красного — красный (геометрия полноэкранная, идёт).
- Решение (не сделано): эмулировать алиасинг формата для compute-копий: общий буфер памяти на регион (`MTLBuffer` + линейные текстурные виды разных форматов с одинаковым `bytesPerRow`) либо компилировать такие копии как копию байтов с учётом tile mode источника и приёмника (detile/retile на GPU). Нужно знать tile mode приёмника в T# компьютера (`0xe5941d54`, `res1`).
- Не закоммичено.

### UI и текст отображаются: корень — раскладка argument buffer (2026-10-22)
- Результат: на экране ошибки EA видны текст «EA Servers are unavailable right now…» и кнопка «Ok»; логотип FC 27 на интро чёткий (кадры 400–2500, `/tmp/fix6/g_*.png`, `h_crop.png`). FPS 45–60 при старте, дальше лимит игры 30.
- Корень: Metal раскладывает члены argument buffer-структуры подряд в порядке объявления и **игнорирует пропуски в `[[id(N)]]`** (проверено на тестовом ядре: структура с `[[id(65)]]` читает дескриптор по смещению 0; пара `[[id(65)]]`+`[[id(160)]]` — по смещениям 0 и 8). Хост писал дескриптор N по смещению N·8 (`arg_ptrs[N]`, `args[N]`). Итог: у нативных compute-ядер текстуры/`flattened_srt` читались из чужих слотов (нули → rect копирования = 0, запись в атлас не происходила), у graphics-шейдеров — сэмплеры (id 128) и SRT (id 160).
- Исправление: `MslEmitter.cpp`, структура `KytyArgumentBuffer` теперь заполняет каждый пропуск id массивом фиктивных указателей `device uint *kyty_gap_N [[id(N)]][k]` (как уже было с `buffers[64]`), так что индекс члена == id. Члены хранят число занимаемых id (buffers 64, samplers 32, прочие 1). `metalRenderer.mm`: compute дополнительно пишет `arg_ptrs[160]` (SRT) к старому `arg_ptrs[53]`.
- Попутно исправлено (нужно для атласа): `GuestTextureUsage` — целочисленные форматы (R8/RG8/R16/RG16/RGBA16 Uint/Sint, 32-бит Sint) теперь `ShaderWrite` (раньше запись в R8Uint-атлас молча терялась) + `PixelFormatView`; `ReinterpretFormat` — вид той же памяти в другом формате (compute пишет R8Uint, draw читает R8Unorm); `GuestTexture` отдаёт этот вид при смене формата, не пересоздаёт текстуру по формату и больше не обновляет из пустой гостевой памяти текстуры, в которые писал compute (`cs_written`); граф. регистрация `RegisterComputeWrittenImage` хранит базовый образ, а не DST_SEL-вид (иначе compute получал read-only вид как storage); `allowGPUOptimizedContents = NO` для детайленных текстур.
- Новая диагностика: `KYTY_MSL_DUMP_DIR=<dir>` — сохраняет сгенерированный MSL каждой напрямую скомпилированной программы (`<hash>.metal`).
- Остаётся: (1) мелкие зелёно-чёрные точки под кнопкой «Ok»; (2) 70 draw'ов за прогон не создают pipeline: VS `0x5fa32b53…`/`0x85070420…` не пишет `param0`, а PS `0xd6be0a69…` (depth-only, `depth(any)`) его читает — `Fragment input user(locn0) mismatching`; (3) текст проявляется постепенно (анимация), последние глифы обрезаны — проверить на кадре после полной анимации; (4) прежние пропуски шейдеров (cube sample, `DataAppend`).
- Не закоммичено.

### Продолжение: иконка кнопки «Press [ ] to Skip», алиасинг слоёв, лимит групп (2026-10-22)
- Пользователь: на интро не видно ролика, а «Press … to Skip» без иконки кнопки. Кадры 400–600: фон и логотип есть, между «Press» и «to Skip» пусто.
- Иконка — текстура 40×32 RGBA8 (`tfmt=56`, `tile=9`, `dst_sel=6547`), и в гостевой памяти, и в Metal-образе она нулевая (`KYTY_LOG_TEX_UNIQ=<lo>,<hi>` печатает `nz`/`guest_nz`). Её не пишет ни один compute-образ (`CsImgBind`), ни compute-буфер (`KYTY_LOG_BUF_COVER=<lo>,<hi>` пусто), ни render target (`TexUniqRt` пусто). Вероятный источник — CPU-растеризация глифа кнопки (системный шрифт/`libSceFont`), которого эмулятор не даёт; это не GPU-проблема. Видео интро (если оно есть) — декодер (AvPlayer/VideoDec), в GPU-части его нет.
- Сделано: (1) `AliasConvert` в `metalRenderer.mm` — когда compute писал поверхность другого формата/размера по тому же адресу, draw получает её через общую тайловую память: BC-сэмплер над 8/16-байтными блоками compute (копия через буфер, `[AliasBC]`) и несжатые форматы (GPU-ядра scatter/gather со свизлом Standard4KB/64KB, `kAliasMsl`; срабатывает только если гостевая память пуста). (2) `kMaxGroups` 65535 → 0xFFFFFF: тысячи compute-dispatch'ей (196608…2211840 групп) раньше обрезались. (3) VS, экспортирующий только position, получает `kyty_missing0..7` (user(locn)) — исчезли 70 draw'ов без pipeline (`locn0 mismatching`); остался 1 pipeline PS с `color(1)` без dual-source (раннее, `dropped_pipe=0` в установившемся режиме).
- Диагностика: `KYTY_LOG_TEX_UNIQ`, `KYTY_LOG_BUF_COVER`, `KYTY_LOG_ALIAS`, `KYTY_MSL_DUMP_DIR`.
- Не закоммичено.

### GDS / ds_append в Metal (экспериментально, 2026-10-22)
- Два самых частых пропущенных compute-ядра (`0xa69ef1cef7e524b4` 54 dispatch'а, `0x828eebc8dfcc8951` 53) пропускались из-за `DataAppend` (GDS-счётчики). Игра перед ними грузит счётчики из памяти в GDS (`DMA_DATA` dst_sel=1, смещения 0x40/0x44/0x48), после — выгружает обратно. В Metal-ветке эти DMA шли в Vulkan-`BufferCache`.
- Сделано, под флагом `KYTY_MSL_GDS=1` (по умолчанию выключено): `MslEmitter.cpp` — лоуэринг `DataAppend/DataConsume` (младшая активная линия делает atomic add/sub числа активных линий, результат всем линиям через `simd_shuffle`) и член `device atomic_uint *gds [[id(161)]]`; `metalRenderer.mm` — 64 КиБ `GdsBuffer`, привязка в `arg_ptrs[161]`, `GdsDma/GdsFill` (blit-копии в порядке очереди под `m_mutex`; без блокировки present-поток падал на `endEncoding already called`); `graphicsRun.cpp` — перехват `DmaData` с GDS.
- Итог с флагом: ядра исполняются, но кадр хуже (текст бьётся на блоки, FPS проваливается до 0). Вероятные причины: wave64 игры против simdgroup 32 (счётчик на simdgroup, а не на wave), порядок GDS→память относительно чтения CPU, ядра теперь реально тяжёлые. Без флага — прежнее поведение.
- Также `kMaxGroups` 65535 → 0xFFFFFF: стартовые dispatch'и с 196608…2211840 групп теперь выполняются целиком; на старте FPS местами падает до 5–20, дальше лимит 30.
- Диагностика: `KYTY_LOG_GDS=1`.
- Не закоммичено.
