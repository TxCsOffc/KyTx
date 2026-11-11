# Native EXEC/DPP and read-only texture import — 2026-11-10/11

Implemented and GPU-tested two missing native contracts. The game still hangs on the GPU with normal batching, then displays black at about one window FPS. Shadows, a working menu/match, and a performance improvement are **not** established.

## Implementations

`MslEmitter.cpp` now treats scalar EXECZ/VCCZ branches as whole-native-group mask tests (`simd_all` of the already-negated predicate); EXECNZ/VCCNZ use `simd_any`. Expression, scalar-instruction and SCC predicates keep their existing meaning. Per-lane scalar branches previously let invocations leave loops before later guest EXEC reactivation and reductions.

Native non-pair DPP now honors source EXEC/FI, row-edge validity, destination row/bank masks, destination EXEC and bounds control. The pair64 path also checks row-edge validity when FI is enabled. Unsupported RDNA DPP controls are rejected instead of silently becoming identity moves. Zero here is the defined DPP invalid-source value, not a substitute for an unsupported shader.

This fixes tested native32 contracts. Compute/mesh pair packing supports both halves of a guest64 wave; ordinary fragment wave64 still has a native32 host group. The wave64 pixel descriptor tests do **not** prove general cross-half fragment semantics. Full fragment wave64 emulation remains open.

`metalTiling.mm` now imports into a raw view without ShaderWrite through a real writable private uint texture, then bit-preserving texture→buffer→destination blits. This is needed for renderable sRGB resources: their raw view inherits read/render/view usage, and a compute store against it is invalid. Adding ShaderWrite to the sRGB parent is not a valid general solution. The bridge retains its temporary resources through command completion, including unretained command buffers, and participates in encoder fence ordering. It preserves the destination origin and performs no numerical conversion. Temporary bridge resources are currently allocated per import; this is a correctness change without a measured speed claim.

Six render fault signposts in `metalDraw.mm` were moved immediately before actual draw API calls. Their old placement could label a draw that returned before encoding work. This improves attribution; a failed encoder label still does not identify its guilty stage or instruction. No guest draw conditions were changed.

No shader skip, fake varying, iteration cap, work-size cap, LDS truncation, automatic input loop, or new game-specific bypass was added.

## Regression evidence

The stage workspace is `/Users/tx/Documents/Codex/2026-11-09/new-chat/work/metal-wave-control`.

| Validation | Result |
| --- | --- |
| Wave suite, structured emitter, API + GPU Validation | 147 GPU cases / 24,384 lane values PASS |
| Wave suite, dispatcher emitter, API + GPU Validation | Same 147 / 24,384 PASS |
| Full native MSL suite, API Validation | PASS after fixing the test-helper union setup |
| Old emitter against new scalar branch test | Expected failure: EXEC/VCC branch split the native wave |
| Old emitter against new DPP test | Expected failure: control101, FI0/BC0, source-inactive lane3 returned104/111 instead of0/7 |
| Full coherence, API + GPU Validation | PASS; includes 18 read-only format/layout cases, 54 selected mip/layer round trips, 3,105,792 exact bytes |
| Full pipeline / multi-draw / mesh / mip / depth / bounds, API + GPU Validation | PASS; color mip pass tests now60 cases /5,928,960 physical bytes including RGBA8/BGRA8 sRGB |
| Product rebuild | PASS |

The wave fixtures cover EXEC/VCC masks0/1/17/32, negative controls preserving other predicate meanings, DPP row shifts/mirror/share, source-active/FI, BC, row/bank masks, native32 and real pair64 compute, and a genuine seven-iteration reactivation/reduction loop in compute and pixel stages. Five unsupported DPP controls reject explicitly. The DPP CPU oracle calculates source and destination rules independently of the generated MSL.

The new read-only image fixtures use private resources, raw views lacking ShaderWrite, odd37x21 dimensions, six mips, three layers, three tile families, and unretained command buffers. Full guest-allocation comparison verifies selected mip/layer bytes plus preservation of other subresources, padding and guards. Render-pass tests exercise actual load/store, reopen, clear and partial draws.

An intermediate pair64 fixture failed because the existing `Compile` helper assigned the compute member of a union and then overwrote it by setting the vertex member to null. The helper now selects exactly one member by stage, and the full MSL suite was rerun. An earlier test used a nonexistent UMod32 IR opcode and was corrected to div/mul/sub. Those are test-development failures, retained in the logs, not emulator regressions. The final source manifest records all eight code/test files.

Example focused reproduction:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 KYTX_MSL_WAVE_ONLY=1 bash tools/test_metal_msl.sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 KYTX_MSL_WAVE_ONLY=1 KYTY_MSL_DISPATCHER=1 bash tools/test_metal_msl.sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 bash tools/test_metal_coherence.sh
bash tools/test_metal_pipeline.sh
```

The full MSL run used API Validation without global Shader Validation. Historical shader_cfg/Apple clip-guard failure remains open; that separate target was not rerun. Game-wide Shader Validation is still blocked by an earlier real4608-dword static-LDS compute kernel: instrumentation expands18,432 bytes to36,864, exceeding32,768. Do not reduce guest LDS or thread count to get past the validator. This stage's game API Validation did work and found the read-only import violation.

## Game observations

All runs used the existing40GB guard, maximum-seconds0, one emulator instance, no timer kill, and no automatic input loop. The launcher restored `extra_env_s1.sh` byte-for-byte. API and shader validation are different; the game runs below used no Shader Validation. Starts, PIDs, binary hashes, full errors and state4 labels are in `game-observations.json`; full logs and exact shader pair are archived locally.

| Run | Build / flags | Observed result |
| --- | --- | --- |
| WC1 PID23056,19:38:31+05 | 2abf08c2…; normal batching | Intro232, black266, GPU Hang persists after wave fixes |
| WC2 PID23727,19:47:48+05 | b5a43cf1…; new signposts; split requested from230 | Intro250, black289, same encoder pair fails. Threshold230 was not reached in TraceWindow g_flip, so splitting was not active before the fault |
| WC3 PID24157,19:51:18+05 | b5a43cf1…; API Validation; same threshold mistake | Natural abort at kyty_tile_import writing dst usage0x15 without ShaderWrite; motivated the bridge |
| WC4 PID29531,23:51:31+05 | e0b1e410…; bridge + API Validation + split draws/ops from0 | Black234/257/317; no API assertion or command-buffer error found before closing. Altered timing/encoder boundaries, no validated image or speed result |
| WC5 PID29829,23:55:46+05 | e0b1e410…; bridge + API Validation, normal batching | Intro240; one actual J after fresh AX at266; black268 and337, window fps1; GPU Hang followed by recovery/submissions ignored |

WC5 log: `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/wc5-235545.log`. First command-buffer GPU Hang is line46532. The buffer contains draws12745/12746/12747. FaultInfo state4 is at lines46708 and48883: render encoders12747/12948 use VS native1d58ae6f8ed54b0c (guest a8092251af057bb6), PS native9543bc4cc4708443 (guest dec48a1373748c4f), RECTLIST/RG11B10Float. The PS hash changed with the emitter, while its guest identity matches the older failing pair. This locates involved encoders, not a proven PS bug. The labels' vtx1 may describe an indirect submission and do not prove zero raster work.

Later WC5 commands are discarded as recovery victims or ignored for prior/excessive GPU errors. Thus its one-FPS black window is not a healthy steady-state performance measurement. This chronology does not establish a general memory leak or prove that this stage caused the old artifacts. VM2's three-second CPU sample had only three samples per thread, including a canonical footprint sort; it is too weak for quantitative profiling. That sample is retained without treating it as an FPS diagnosis.

VM2, WC1, WC2, WC4 and WC5 were closed normally through CUA/CmdQ after observing their diagnostic outcome; WC3 aborted naturally. WC5 App quit was observed and PID29829 was confirmed absent. No emulator remains running at the checkpoint. Do not select a dead app with CUA without first checking the PID: app selection can auto-launch the default c9 run. Failed standalone input attempts in WC1/WC2 were rejected by the UI state guard and are not counted as delivered input.

## Upstream research and remaining work

Reviewed [shadPS4 #5665](https://github.com/shadps4-emu/shadPS4/pull/5665): it recognizes whole-wave reduction regions and replaces proven patterns with subgroup reductions. It was not copied or represented as implemented here. Our scalar branch and DPP contract fixes were tested separately. The [guarded subgroup32 follow-up](https://github.com/jaime7981/shadPS4/pull/1) is another candidate to inspect against this shader, not general wave64 emulation. Its parent [#5667](https://github.com/shadps4-emu/shadPS4/pull/5667) is closed. An upstream report in another game is a research lead, not proof of our hang's cause.

AMD's [RDNA ISA](https://docs.amd.com/v/u/en-US/rdna-shader-instruction-set-architecture), DPP16 field table87 and table88, specifies separate FI, bounds and destination-mask behavior. [GPUOpen's earlier cross-lane overview](https://gpuopen.com/learn/amd-gcn-assembly-cross-lane-operations/) supplies context; its GCN-specific controls must not be substituted for RDNA controls.

Next isolate the faulting render pair with the actual indirect arguments, NGG primitive payload/flat parameters, resources, samplers, attachment and raster state **before the first GPU fault**. The existing ReplayDumpPhase instrumentation is compute-only; the archived shader sources are not a complete replay capture. Comparing normal batching with WC4 suggests ordering/lifetime/encoder boundaries are useful axes, but WC4's black image prevents claiming a fix. Keep real producer data and exact physical ownership; do not skip the shader, cap its loop, zero its inputs, or introduce a timing-only default workaround. The guarded reduction matcher can only be adopted after its semantic preconditions are proven and tested with negative controls.

General fragment wave64, compressed3D, simultaneous aliases, private legacy producer transitions, linear2D interop and general lazy coherence remain open. Old shadows/clothes/lighting/hair/neon/upscale artifacts remain unverified. Build SHA256: `e0b1e410f1d1789c37e167ddf60082d859dc95fdcc8d20915698c12a69c8fc3f`.

Ledger CLI is unavailable in this Mac environment. Updated STATE/HANDOVER/worklog instead, retaining historical entries. The inherited dirty/untracked tree was preserved. The stage patch is against the captured dirty-before baseline, not HEAD; no staging, commits, pushes or resets. Previous stage patches were preserved unchanged.
