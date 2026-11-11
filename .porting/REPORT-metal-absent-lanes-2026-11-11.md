# Absent lanes in partly covered pixel groups + DPP BOUND_CTRL — 2026-11-11

Continues `.porting/REPORT-metal-wave-control-2026-11-10.md` (Codex). The GPU hang that ended every WC1–WC5 run
(state 4 render encoder, VS `a8092251af057bb6` + PS `dec48a1373748c4f`, RECTLIST/RG11B10Float) is explained,
reproduced in a mini-test, fixed in the MSL emitter, and **no longer occurs in the game run AL1 up to frame 415**.
Shadows/old visual artifacts and FPS are **not** established by this stage.

## Cause

PS `dec48a1373748c4f` is wave64 and runs as a native32 fragment group. Its loop is a light-list scalarization loop
(IR blocks $4–$6): every lane walks its own ascending index list; each trip the wave minimum is taken with the
standard whole-wave idiom and the lanes whose index equals the minimum process that light and advance:

- whole-wave EXEC: `s_orn2_saveexec` → IR `%414 = LogicalOr(LogicalNot(%390), %390)`, inactive lanes get `0xffffffff`;
- `v_min_u32_dpp row_shr:1/2/4/8` (FI0/BC0), `v_permlanex16_b32` (selectors all 15), `v_readlane 31` + `v_readlane 63`, min.

On real hardware a lane without a pixel exists with EXEC=0; the whole-wave code switches it on holding the identity,
so the minimum is the minimum of the real lanes. On Apple a partly covered fragment SIMD group has lanes that **never
launched** (Apple fills a group from lane 0: masks `0000000f`, `000000ff`, `00000fff`, `0000ffff`, `000fffff`, …
measured with `simd-occupancy-probe.swift`). A `simd_shuffle` from such a lane returns 0 on M5 Max. The MSL path:

- `permlanex16` from absent lane 31 → source EXEC shuffle = 0 → value 0;
- `readlane 31` / `readlane 63 & 31` → absent lane 31 → 0;

so the "minimum" becomes 0, no lane's index (≥ 1) matches it, nobody advances, and the loop never ends → Metal
`kIOGPUCommandBufferCallbackErrorHang`, recovery victims, ignored submissions, black window at ~1 FPS.
Partly covered groups appear only for some draw rectangles (dynamic-resolution targets: 256×256, 1624×912, …;
a full 256×256 or 1624×912 rect has none on M5 Max, 200×113 or a 250×250 rect has some), which is why the hang
appeared after a varying number of frames.

The SPIR-V/MoltenVK path already handles this (`ValueEmitContext::Shuffle`/`LanePresent`, comment: "light-list
scalarization loops spin forever on garbage"); the native MSL path did not.

## Fix (`MslEmitter.cpp`, non-pair native32 path)

Guest rule implemented: a lane that never launched is a guest lane with EXEC=0. It is live only under a
whole-wave EXEC, recognised statically (`WholeWaveExec`: immediate true, `x | !x`, or AND/OR of such), where it holds
the reduction identity. Lanes that launched keep exactly the previous semantics; full groups are unchanged.

- `kyty_live_lanes()` = `simd_ballot(true)` at the point of use; ballots/shuffles are separate statements, never
  inside `?:` (see the SPIRV-Cross ternary lesson).
- **ReadLane**: a target that is not running reads the highest running lane at or below it (else the lowest).
  For a scan this is exactly the guest's value at that lane (identity above the last launched lane), and the
  result stays uniform. Wave64 `& 31` mapping kept (63 → 31).
- **DPP move/update**: present source lanes: RDNA DPP16 rules. Absent source under whole-wave EXEC: the update keeps
  the destination — that is `op(identity, x)` for in-place ops (`v_min_u32_dpp v1, v1, v1`) and for moves into an
  identity-filled register (LLVM `update.dpp(identity, …)` style), for any op. Absent source otherwise: inactive.
- **Permlane16**: absent source under whole-wave EXEC reads the lane's own value (neutral for min/max/and/or, same as
  the SPIR-V path); otherwise inactive (0 with FI0, stale with FI1).

### DPP BOUND_CTRL correction (both native32 and pair64)

LLVM AMDGPU modifier docs: DPP16 `fi:0` — "interaction with inactive lanes is controlled by bound_ctrl";
hardware BC=1 reads zero from an invalid lane, BC=0 disables the write. The previous code wrote `op(0, x)` for an
inactive source with BC=0 (only row-edge invalidity suppressed the write), and Codex's DPP oracle encoded that.
Now: write = dest EXEC && row/bank masks && (BC || (in-row && (FI || source active))). The oracle in
`metalWaveControlTests.inc` was updated to the ISA rule. Permlane16 with FI0/BC0 and a present inactive source
still returns 0 instead of the old VDST (the IR has no old-VDST operand) — open item below.

No shader skip, iteration cap, work/LDS cap, fake varying or game-specific bypass was added. The loop's trip bound
in the new test is part of the test's guest program (so a regression fails instead of hanging the GPU), not the
emulator.

## Evidence

Workspace: `/Users/tx/Documents/Codex/2026-11-09/new-chat/work/metal-absent-lanes` (before/after sources,
`absent-lanes-and-dpp-bounds.patch`, logs, `hashes.txt`, AL1 shader pair, launcher, probe).
Baseline = Codex's validated state (`MslEmitter.cpp` 50270fb2…, `metalWaveControlTests.inc` 2c2ecea5…).

| Validation | Result |
| --- | --- |
| New `WavePartialGroups`: scalarization loop at scissors none/13×7/10×2/6×2 + 12 direct absent-lane cases (readlane, DPP move FI0/FI1, DPP update BC0/BC1, permlanex16, row_mirror; WWM and non-WWM) | PASS |
| Same tests against the **old** emitter, each part separately | all 5 parts FAIL: loop trips=64 count=0 (mask 0000ffff); readlane/DPP/permlane from absent lane = 0; DPP update wrote 7 |
| Wave suite (163 GPU cases / 27,130 lane values), structured, API+GPU Validation | PASS |
| Same, `KYTY_MSL_DISPATCHER=1`, API+GPU Validation | PASS |
| Full native MSL suite, API Validation | PASS |
| Pipeline / multi-draw / mesh / mip / depth / bounds, API+GPU Validation | PASS |
| Product build | PASS, SHA256 `8d16c2ac43849309de44fa2e3b443f1b36db6dac95c8d9a7a21480e527d6d31e` |

Coherence/tiling suites not rerun (no change in those files).

## Game run AL1

PID 33211, started 2026-11-11 01:59:17 +05, build 8d16c2ac…, normal batching, no API/shader validation, diagnostic
logging as WC1 (`launch_al.py`), original 40 GB guard, no timer kill, no input loop; `extra_env_s1.sh` restored
byte-for-byte. Log `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/al1-015916.log`.

- PS `dec48a1373748c4f` compiled as native `6edce3ec601dd37b`; its MSL contains the whole-wave DPP form
  (`kyty_dp ? … : true`), the guarded permlanex16 and `kyty_live_source` readlanes. It executed on RT
  `0x8241cd0000` (the target of the old hang) in timed command buffers.
- Window title frame 388 → 415: "Press Any Button" title screen (white background with a soft wave and the caption).
  WC1–WC5 were black with GPU Hang at frames 266–337.
- 225,029 log lines at check: **0** command buffer errors, **0** FaultInfo state 4, **0** GPU Hang.
- Guest FPS on that screen ~0.4–1.0 with heavy diagnostic logging (~43k lines/30 s, all shader pairs dumped):
  not a performance measurement; FPS is not claimed improved.
- A background `J` key was delivered as raw input to the non-frontmost window; no menu transition was observed, so
  it is not counted as delivered input. The run was left running for the user.

## Open

- Wave64 fragment programs run as native32: guest lanes 32..63 are modelled as absent. Idempotent reductions are exact;
  an **add** reduction combining readlane 31 and readlane 63 counts lane 31 twice, and a permlanex16 own-value
  substitution doubles an add. Needs identity-aware lowering (e.g. guarded reduction matcher, shadPS4 #5665 idea) with
  negative controls.
- Permlane16 FI0/BC0 with an inactive (present) source should keep the old VDST; the IR op has no old-VDST operand.
- `ds_swizzle`/`ds_bpermute` from absent lanes still return 0 (not implicated here).
- Pair64 compute groups with fewer than 32 launched lanes are not covered by the absent-lane handling.
- SPIR-V path DPP BC=0 inactive-source behaviour not compared to the corrected rule.
- Shadows, clothes, lighting, hair, neon, upscale artifacts and FPS remain unverified; earlier open items from the
  wave-control report (general fragment wave64, compressed 3D, aliases, lazy coherence) are unchanged.

---

# Stage 2 — wave64 reductions over the half that never launched (2026-11-11 02:50 +05)

## Problem

A wave64 program that runs native32 is a guest wave whose lanes 32..63 never launched: always for pixel and vertex waves,
and for compute/mesh when the whole workgroup fits in 32 lanes (`UpperHalfNeverLaunched`; larger native32 workgroups put
guest lanes 32..63 in another SIMD group, which is not modelled and is left unchanged). The old `readlane hi & 31`
mapping makes `op(readlane(V, 31), readlane(V, 63))` read lane 31 twice: right for min/max/and/or, **2× for add, 0 for
xor, 2× for fadd**.

Real game case: compute `6b784196fcba7b41`, workgroup [32,1,1], wave64, native32. Guest code (`/tmp/msl_direct/
disasm_6b784196fcba7b41.txt`): `v_add_nc_u32 … row_xmask:1/2/4/8 bc1`, `v_permlanex16_b32 … bc1`, `v_add`,
`s_and_b64 exec, 1, saved`, `v_readlane 31`, `v_readlane 63`, `s_add_i32`, then `v_cvt_f32_u32`/`v_log_f32` and a packed
store. The MSL path stored log2(2·sum) (one too high). Game survey (607 shaders in AL1/AL2, 2,917 disassemblies): pixel
wave64 reductions use only umin/umax/or across halves (results unchanged by this stage); compute butterfly/add
reductions are all pair-packed except `6b784196`.

## Implementation (`MslEmitter.cpp`, native32 path)

- `UpperHalfIdentity`: in exactly `op(readlane(V, lo<32), readlane(V, hi>=32))` (also through `BitCastF32U32` for float
  ops) the high read becomes op's identity: add/or/xor/umax 0, and/umin ~0, smin 0x7fffffff, smax 0x80000000, imul 1,
  fadd -0.0, fmul 1.0, fmin +inf, fmax -inf. Any other use keeps the ordinary read.
- `permlane16`/`permlanex16` from a lane that never launched, under whole-wave EXEC: if the value is the last row step of
  a scan (`row_shr:8`) or butterfly (`row_xmask:8`) and the source row has launched lanes, read the highest launched lane
  of that row at or below the source (its prefix / the row total); otherwise the identity of the op that combines the
  result with its own source (`PermlaneIdentity`), else the lane's own value as before.
- DPP `row_xmask:m` (m = 2/4/8) applied to the result of `row_xmask:m/2` under whole-wave EXEC: a source lane that never
  launched holds its block's value, so read the highest launched lane of its aligned m-block at or below it; with no
  launched lane in the block keep the destination (identity), as before.

## Evidence

| Validation | Result |
| --- | --- |
| New `WaveUpperHalf`: (1) the 6b784196 idiom for add/xor/umin/fadd, compute 32-lane and pixel full groups; (2) whole-wave butterfly + `op(readlane 0, readlane 32)` in partial groups n = 12/16/20 (+ full groups), per-lane row and cross values; (3) the game's scan form `row_shr` + permlanex16 + `op(readlane 31, 63)` | 20 cases PASS |
| Same against the previous-stage emitter | part 1: total 0x420 (=1056) vs 0x210 (=528); part 2: 312 vs 78; part 3: 312 vs 78 — all FAIL |
| New emitter with only the `row_xmask` block rule disabled | part 2 FAIL: lane 4 row 36 vs 78 (missed lanes 8..11) |
| Wave suite 183 cases / 32,958 lane values, API+GPU Validation, structured and dispatcher | PASS |
| Full native MSL suite (API Validation); pipeline / multi-draw / mesh / depth bounds (API+GPU Validation) | PASS |
| Product build | SHA256 `11dd451a30336b7512b0da4b4d0f95b56cf5c7ca720f88cd146fe01376cce052` |

AL1 (previous build) ran 41 min on the title screen: 4,685,249 log lines, 0 command buffer errors, 0 GPU Hang; quit via
the app menu, PID gone.

Game AL2: PID 37290, 2026-11-11 02:44:26 +05, build 11dd451a…, same flags as AL1, log
`lean/al2-024425.log`. `6b784196fcba7b41` native `20135d2a0dce19e1`: `v50 = 0x00000000u; v51 = (v49 + v50);`
(AL1 native `5558e9defc7de2f2`: `v50 = simd_shuffle(v40, kyty_live_source(kyty_rl, 0x3f & 31))`, i.e. 2·sum).
AL2 reached the **main menu** (FC NEWS / ULTIMATE TEAM / KICK OFF / LEARN TO PLAY / SEASONS, frame 1386); no input was
sent by Claude in AL2, input logging is off, so whether the user pressed a button is unknown. At 679,245 lines:
0 command buffer errors, 0 GPU Hang, 0 state 4.

The menu shows large blocky/striped texture garbage over the background and news tiles. It is **not** attributed to this
stage: of the 83 shaders whose MSL changed between AL1 and AL2, the full-group behaviour changes only in the
upper-half readlanes, which in the 9 pixel shaders combine with umin/umax/or (identical results), and in compute
`6b784196`, which in AL2 first ran after the menu had appeared (log line 598,858 of 625,294 at that time). Its real
cause is not established; no earlier run in this series reached the menu, so there is no A/B yet.

## Open

- The menu texture garbage (A/B against the previous build at the menu, then trace the affected textures).
- Native32 compute/mesh workgroups of 33..63 or non-multiple-of-64 threads: guest lanes 32..63 live in another SIMD
  group; cross-half ops there remain wrong (would need pair packing for partial waves).
- SPIR-V path has the same `lane & 31` cross-half mapping (2× for add) — parity fix not done.
- Permlane16 FI0/BC0 old VDST, swizzle/bpermute absent lanes, pair64 partial groups (from stage 1) unchanged.
