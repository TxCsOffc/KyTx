# FC27 native Metal handover — 2026-10-23, 15:12 Asia/Tashkent

## Start here

User requested this handover and stopped active debugging. Task is **incomplete**: intro, icons/effects and heavy-scene artifacts remain; no correct playable match or 30+ gameplay FPS evidence. Use **native ARM64 FEX + direct RDNA → MSL**. No Rosetta, no Vulkan renderer; Vulkan was authorized only as a visual reference. No reference run/comparison was obtained in this work.

Repo `/Users/tx/Developer/KyTx`, branch `metal-native-checkpoint`, HEAD `c30d5d81`. Many preexisting tracked and untracked changes: preserve everything, no clean/reset. No commits made or authorized. `/Users/tx/Developer/kytyps5-arm64` is a symlink to this repo. Game is `/Users/tx/Developer/PPSA34015`, FC27 01.000.004. Run cwd `/Users/tx/Developer/fex-spike/runs`.

Previous detailed history: [earlier handoff](porting-handoff-fc27-metal-2026-10-23.md). This file supersedes its runtime/process state and its old “next bug” entries.

## Current source and binary

Final verification after reverting the experiment: ARM64 build **passes**, `git diff --check` **passes**, transient-buffer test with **API + GPU shader validation enabled:3 cases,0 failures**. No post-revert game repeat was run after the user's handover request. Emulator is no longer running; the last experimental run exited on the API assertion described below.

Retained from the preceding work (see earlier handoff for details):

- Native samplers, BDA address loads, formatted memory and structured MSL control flow; GPU regression suite exists.
- Generic GF(2) tile/untile converter, persistent per-address depth, CPU libVLC YUV pitch correction, buffer-backed format reinterpretation, graphics cube sampling enabled. Cube compute remains opt-in; **GDS remains OFF** (previous on experiment produced green output).
- Graphics image conversion happens before pipeline/state setup; failed shader/PSO draws return. Mipped RTs allow full-chain format views. No broad latest-writer selection: the earlier trial caused GPU Hang and was reverted.
- API fixes: scissor clips against actual attachment mip extents; point/line/triangle PSO topology matches the guest draw.
- Compute pipelines labelled `CS <hash>` for Xcode/validation. RG11B10Float array/3D and RGBA16Unorm ShaderWrite usage enabled after actual standalone GPU write/read tests.
- Raw buffer x1–x4 loads/stores check each component/predicate, handle size0 and near-UINT_MAX; formatted reads bounded. LDS uses declared compute size, checks predicates/bounds. Graphics storage images use writable base objects, bypass sampled swizzle. Graphics buffer size constants clamp to actual backing and are uploaded after binding.
- `tests/metalMslTests.mm`: 144 raw-buffer boundary cases plus LDS final-word/predicate/OOB tests, in addition to existing cases. Whole suite passed with API+shader validation before the ring follow-up (`test-shared-final.log`). Light-scene `shared.log` reached flip1075 without Invalid Metal usage/API assertions; this does **not** validate the heavy scene.

**New retained fix in this follow-up:** `metalBuffer.h/.mm`: transient dynamic buffer no longer wraps to offset0 over queued GPU reads. Overflow/reset starts new backing; retained Metal command buffers keep the previous backing alive. Added `tests/metalBufferTests.mm` and `tools/test_metal_buffer.sh`. The test deliberately queues reads, rolls over and resets BEFORE submission: old code **3 cases / 2 failures**, fixed code **3 / 0**, API validation enabled. Final API+shader-validation repeat and final build logs are named below. This fixes a demonstrated corruption mechanism; **red remains** in the game. Chunk allocation cost/pooling is not profiled.

**Rejected follow-up, fully reverted before final build:** expanded CPU snapshots from16KB to256KB, wrapped graphics buffers using full V# extent instead of static max_byte_extent, and clamped sizes to allocation.size. The experiment reached the heavy red scene but later logged GPU Hang and a separate compressed-linear-texture API assertion. Causality is not proven; do not reapply as a bundle. Preserved patch: `artifacts/2026-10-23/rejected-vertex-range.patch`. The vertex-range idea still warrants a targeted test: a static attribute offset is not the full `vertex_id * stride` access extent. Current snapshot helper still only copies the CPU arena `0x8190000000..0x81a0000000`, up to16KB.

## Valid NEW single-present trace and Xcode results

`/Users/tx/Developer/fex-spike/runs/captures/fc27-20261023/shared-fixed-one.gputrace`

- Captured before the ring fix, after the retained shader/resource fixes listed above. Exactly one CAMetalDrawable present boundary, **not** exact guest frame1400. Capture succeeded:22.1s,11957 resources,24563MiB downloaded,~16GiB on disk. Full Xcode replay **succeeded** on M5 Max/macOS27.2.
- Summary:22 command buffers,225 render encoders,445 compute,155 blit,578 draws,373 dispatches,15.72GiB tracked memory; present1280×720. These are trace statistics, not gameplay FPS.
- **CB20 / ComputeEncoder382 / API26395**: groups30×1080×1, threads64×1×1, `CS 955520d8831f23b0`, main0(69). New trace does NOT have old composite b3300709.
- Bound: Texture92/93 3D LUT; Texture94 tiny default; **Texture95** RGB10A2 output red; **Texture449** RGBA8Unorm mipmapped overlay red; Texture454 scene black. Overlay already red before final composite.
- Navigator filter `Texture 449` works and is more selective than just449. First matching draw **CB19 / RenderEncoder74 / API21316**, triangles6. PS main0(66), VS main0(67), pipeline22; color Texture449 Load|Store, depth757, sampled Texture106 1×1 RGBA8, vertex inputs include GuestMemoryUMA357.
- **Pixel shader debug actually succeeded** at pixel(405,539). Entry param0=(0,0,1,0), param1=(0,0,0,0), **param2=(1,0,0,0.6)**. Final executed scope returns **RGBA=(1,0,0,approximately0.6)**. Therefore THIS draw produces red from its vertex color input; do not assume it merely preserves red. Post-VS all6 vertices have the same param2. VS/PS sources match dumped guest hashes **b2c4c2e311c5bc76 / 751942f534c2702e**. Need compare raw vertex bytes at encode time and draw time; cannot yet conclude whether guest intended that red or bindings/data were overwritten.
- Xcode marks overlay and depth contents invalid; this alone does not prove why. The earlier “uninitialized Load is root cause” hypothesis was not established.
- The OLD trace `single-frame/lldb-present.gputrace` remains valid too,9.6GiB, but its indices2364/9049/etc apply only there. Old API9049 pixel debug returned black/alpha0.215686. That does not contradict the NEW API21316 red draw.
- **Xcode replay is now stopped**, both trace windows may remain open at Replay. No LLDB active. No emulator running at handover. Do not call CUA getApp on the game unless intending to relaunch it.

## Latest game runs and failures

Run evidence directory: `/Users/tx/Developer/fex-spike/runs/validation-20261023/`.

- `interactive-ring.log`: retained ring fix, baseline FEX/MSL plus API validation, no SYNC_FLUSH. Heavy red reproduced at frame647; after J, still red at1212. Music overlay and Hold prompt visible. No correct gameplay proof.
- `interactive-vertex.log`: REJECTED vertex-range/snapshot experiment. Heavy red and occasional white bottom rectangles persisted. Another launch displayed the correct light FC27 title (e.g.1210), then after holding J transitioned to heavy red again. CUA reported the app changed/restarted and log path was overwritten; do not use these differing light/heavy states as matched A/B evidence.
- Preserved final `interactive-vertex.log` contains **GPU Hang** (`kIOGPUCommandBufferCallbackErrorHang`, line4890 and subsequent submissions ignored), followed by **`minimumLinearTextureAlignmentForPixelFormat: Linear textures do not support compressed pixel formats`** at line6559. Stack reaches ComputeGuestImage → GraphicsImageFor → DrawAuto. This is a concrete next API defect: compressed guest image must not enter the linear texture alignment/buffer-view path. Do not suppress the assertion. The experiment was reverted; that assertion may also be reachable on baseline later scenes.
- Unsupported native operations still observed: `FPCmpClass32` in CSfe9cb9768456f361; `ShiftLeftLogical64` and DataAppend in CSc29caf0c1cb0ad94; formatted buffer store desc_format0 in CSe191ab754420b413; several compute cube samplers remain opt-in. **No implementation of FPCmpClass32 or 64-bit shifts was made.** Their local semantic references are in `backend/spirv/spirvEmitterAluHelpers.cpp` (EmitClassMaskF32) and `spirvEmitterAlu.cpp` (EmitShift64/EmitConstantShift64). Reading those source files is not executing a Vulkan renderer.
- Large LDS instrumentation still can exceed32KB, e.g.5632dwords kernel3ef1bf7a988c6913 becomes45056bytes under validation. Earlier per-pipeline environment selectors did not avoid that assertion in practice. Do not claim full heavy-scene GPU validation passes.

## Reproduction, artifacts and next steps

Build:
```sh
cd /Users/tx/Developer/KyTx
cmake --build _Build/macos-arm64 --target kyty_emulator -j4
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 bash tools/test_metal_buffer.sh
```
Normal build already performs authorized ARM64 ad-hoc JIT signing. No Rosetta.

Interactive launcher `/Users/tx/Developer/fex-spike/runs/KyTxDiagnostic.app`, bundle `org.kytx.diagnostic`, shell executable execs current ARM64 binary with:
`KYTY_MSL_ONLY=1 KYTY_BACKEND=metal KYTY_VIDEO_OUT_1080P=1 KYTY_NP_STATE=signed_in KYTY_LABEL_FLUSH=lazy KYTY_WAIT_RETRY_ELIDE=1 KYTY_WAIT_BY_ORDER=1 KYTY_UMA_READBACK=1 MTL_CAPTURE_ENABLED=1 MTL_DEBUG_LAYER=1`.
Currently also dumps presented600/800/1000 via `KYTY_DUMP_FRAME` and `KYTY_DUMP_PATH=.../vertex-%llu.ppm`, log `interactive-vertex.log`; **rename both log and dump prefix before another launch**. Correct flag is singular `KYTY_DUMP_FRAME`, not DUMP_FRAMES. Cross keyboard mapping is J; Hold prompt needs holding, not just one press.

CUA: `cua.getApp(path)` launches the app if stopped. For Xcode use bundle `com.apple.dt.Xcode`, choose correct trace via Window menu. After context compaction call `cua.rewriteDocumentation()`. Use fresh AX indices; screenshot pixel coordinates are Retina-scaled and caused wrong-window clicks, prefer AX. Draw → Debug Shader → Fragment → Debug Pixel. Expand execution root before filtering. Final scope is last navigator row after setting its scrollbar to1. Stop replay before running game to release GPU memory.

New durable artifacts under `.porting/artifacts/2026-10-23/`: `ring-before.log.gz`, `ring-after.log.gz`, `ring-final.log`, `build-handoff-final.log`, `interactive-ring.log.gz`, `interactive-vertex.log.gz`, `vb-before.log.gz`, `rejected-vertex-range.patch`; earlier shader-validation/MSL tests/capture logs are there too. PPMs stay in the external run directory. `/tmp/kytx-20261023-overlay` no longer exists; do not rely on it. Huge traces stay at their capture paths, not copied into repo.

Next focused work: fix compressed-format linear-path assertion; independently verify vertex-buffer extent/snapshot ownership against NEW trace's red draw; lower missing FPCmpClass32 and 64-bit shifts with actual GPU regressions; then address GDS/cube/alias ownership using exact types, tile layout and ShaderWrite rights. Broad compute latest-writer preference previously hung and must not be repeated blindly. `s_cs_written_by_addr` still records graphics READ bindings, so it is not a reliable last-writer registry. Mipped RTs remain excluded from RecordComputeStorage. `KYTY_DUMP_CS_IMGS` may close an encoder after state setup and alter execution; repair before trusting that diagnostic.

Skills next: debugging-rendering-issues, using-metal-validation, using-gpucapture (new capture only, one present), using-gpudebug (CLI if needed), managing-metal4-resources/synchronization as applicable; honor user's request to inspect Xcode with computer use. No milestone/goal marked complete; no goal-*.md exists here. Project Ledger CLI unavailable on macOS; current notice in `ufc5/.ledger/STATE.md` overrides historical Windows UFC5 content.


## Resumed follow-up: native ALU lowerings (2026-10-23)
Added MSL FPCmpClass32 using IEEE bit classification (10 AMD mask classes, preserving signed zero/subnormal/sNaN/qNaN distinction), plus logical-left/logical-right/arithmetic-right64 with amount masked to63. New actual GPU tests cover50 class/mask combinations and27 shift boundary cases. Full MSL suite passes with API+shader validation; ARM64 build/diff checks pass. Existing newer LinearTextureRowAlignment compressed-format guard/texture tests were discovered already in the working tree and preserved; texture tests pass4/0 under API validation. Do not attribute those preexisting edits to this follow-up.
Runtime class-shifts-game.log: same FEX/MSL baseline with API validation. Frame800 connecting UI renders, frame1000 heavy scene still RED. Reached heavy scene beyond1000; no visual/FPS success claim. Diagnostic process explicitly stopped after collecting images. New tests/build/runtime log and800/1000 images saved under artifacts/2026-10-23. Missing ALU ops now implemented, but GDS/DataAppend, compute cube gates and other ownership defects remain. No new GPU trace captured.
