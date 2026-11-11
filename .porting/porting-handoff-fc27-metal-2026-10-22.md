# Handoff: FC 27 / native Metal — 2026-10-22
Status: work incomplete; user requested handoff for another agent with ~10% context remaining. Do not mark the rendering/performance goal complete.

## Task and rules
- User wants the EA Sports "it's in the game" intro visible, missing button icons/artifacts/effects fixed, and ultimately playable FC 27 at 30+ FPS.
- Implementation MUST remain native RDNA -> MSL, ARM64 + FEX. No Rosetta; no Vulkan/SPIR-V/MoltenVK rendering path.
- User explicitly allowed a Vulkan build ONLY as a visual reference. No verified reference image was obtained this session.
- No commits authorized or made. Do not reset this dirty tree. Do not switch branches or restore files blindly.
- Russian reports, frequent concise progress. User is concerned about long investigations and unverified claims.
- No agents were spawned. Do not inherit the old HANDOVER suggestion to use Haiku: current delegation requires user/instruction authorization.
- Repo: /Users/tx/Developer/KyTx; branch metal-native-checkpoint; HEAD c30d5d81ab23faaf1f6e4d19d4f40bd800c7e92c.
- /Users/tx/Developer/kytyps5-arm64 is a symlink to this repo.
- Game: /Users/tx/Developer/PPSA34015 = EA SPORTS FC 27 01.000.004, NOT UFC 5.
- Run cwd: /Users/tx/Developer/fex-spike/runs.
- AGENTS.md refers to an old Windows UFC Ledger. ledger CLI unavailable; ufc5/.ledger/STATE.md has a current FC27 notice above historical material.
- The skill handoff's commit/completion gates were not used: this is a user-requested transfer of unfinished work, not a completed milestone.

## Current stop state — important
- Emulator stopped cleanly via Cmd+Q. Final ps check: no kyty_emulator or metalMslTests running.
- Last GAME binary includes samplers, formatted memory operations, structured control flow, guest BDA, and cube upload.
- Last source change allows Ballot/ReadLane/ReadFirstLane in vertex MSL. Its focused GPU test PASSED, but the emulator was NOT rebuilt after this final change and no game run validated it.
- Last game experiment enabled KYTY_MSL_GDS=1: BAD regression, solid green screen. Do not use this as a baseline.
- GDS is still opt-in in source. The temporary app launcher has been changed back to GDS off; inspect flags before launch.
- Cube is also opt-in. Cube-on without GDS still produced a red background/main-menu artifacts. It did not fix the intro.
- No playable match reached. 30 FPS in a broken menu/solid-color screen is NOT success.

## Build, tests, launch
```bash
cmake --build /Users/tx/Developer/KyTx/_Build/macos-arm64 --target kyty_emulator -j4
bash /Users/tx/Developer/KyTx/tools/test_metal_msl.sh
bash /Users/tx/Developer/KyTx/tools/test_metal_sampler.sh
cmake --build /Users/tx/Developer/KyTx/_Build/macos-arm64 --target virtual_memory_allocation_tests -j4
/Users/tx/Developer/KyTx/_Build/macos-arm64/virtual_memory_allocation_tests --backing-mappings-only
/Users/tx/Developer/KyTx/tools/run_fc27.sh 100 KYTY_DUMP_FRAME=600,1000,1500 KYTY_DUMP_PATH=/tmp/fc27-next_%llu.ppm > /tmp/fc27-next.log 2>&1
```
Mandatory native run flags are set by tools/run_fc27.sh:
KYTY_MSL_ONLY=1 KYTY_BACKEND=metal KYTY_VIDEO_OUT_1080P=1 KYTY_NP_STATE=signed_in KYTY_LABEL_FLUSH=lazy KYTY_WAIT_RETRY_ELIDE=1 KYTY_WAIT_BY_ORDER=1 KYTY_UMA_READBACK=1.
KYTY_MSL_ONLY also disables disk MSL cache; use it while changing the emitter.
Runtime build performs its existing ARM64 ad-hoc JIT signing. Never run the x86 build through Rosetta.

## Implemented changes and validation limits
1. Native samplers: NEW metalSampler.h/.mm, hooked in metalRenderer.mm for gfx and compute. Cache by all four S# words; filter, address, mip, LOD, anisotropy, comparison, border, bias; argument-buffer support YES.
   - GPU sampler test passed: wrap/clamp, filtering, mip, border, pixel coordinates, cache/argument-buffer behavior.
   - Mirror-once approximated with mirrorClamp; table border remains limited. Single-device static cache.
2. MslEmitter.cpp correctness:
   - Dual-source pixel output maps second source to color(0), index(1); pipeline cache hash includes dual-source flags.
   - U8/U16 buffer loads handle unaligned data/bounds; planning-only const-buffer ops skipped.
   - Generic formatted stores: typed/descriptor formats, 8/16/32/packed, normalized round-to-even, half, int/scaled, footprint bounds and missing-component fill.
   - Phi assignments evaluate edge RHS values before simultaneous assignment.
   - Structured if/else/loops emitted where CFG permits; conservative dispatcher fallback retained. KYTY_MSL_DISPATCHER=1 forces fallback; KYTY_LOG_MSL_FLOW logs selection.
   - Standalone IR -> MSL -> Metal GPU suite passed formatted stores, subword/BDA loads, dual-source pipeline, phi swaps, diamonds, stable IDs, nested loops, dispatcher fallback.
   - Latest VertexReadFirstLane test passed with a real render target/pass. An earlier rasterization-disabled vertex-only test hit Internal Error; replacing that test setup resolved it. Do not treat old failing log as current result.
   - Vertex wave64 cross-half semantics still NOT established; existing ReadLane masks index to 31. New vertex allowance must be game-validated.
3. Native guest address loads (BDA): NEW metalAddressSpace.h/.mm; kernel backing mapping snapshots/generation; argument id 162 bound in gfx/compute; MSL raw address load helper.
   - Guest 16 KiB pages, 1024-entry leaves, 98304 root entries, packed extended range. 64 MiB no-copy backing chunks.
   - Rebuild root/leaves on mapping generation; mark indirect Metal resources used; immutable snapshots. Last startup ~97 rebuilds/~400 leaves/~91 chunks, possible overhead not profiled.
   - GPU tests passed alignment, carry, cross-page, missing mappings, extended range, predication.
   - Kernel mapping snapshot/partial unmap test PASSED after adding ARM64 SP asm to test-only runtimeLinker callback.
   - Actual game no longer reported unsupported native address-load errors.
4. Texture cache and views:
   - ReinterpretFormat rejects illegal compressed views; fixed observed RGBA8-sRGB -> BC1 view fatal.
   - Cache validates dimensions/tile/format/compression and retains R8Uint -> R8Unorm glyph view.
5. Cube upload: replaced GuestTextureCube placeholder with per-face/per-mip physical read + detile/upload into 2DArray, preserving BC7/actual format and mip tails; layout-keyed cache.
   - KYTY_MSL_CUBE=1 only. Actual cube logs: 256²x6 levels9 fmt92 all-zero; 1²x6 fmt70 nonzero; 256²x6 BC7 fmt135 nonzero.
   - Most cube shader skips gone in cube run; no visual fix. Dynamic GPU-written cube ownership/refresh and base-level/base-array views need review; fmt92 storage-write compatibility may be incomplete.
6. Diagnostics only:
   - libAmpr.cpp: KYTY_TRACE_APR_READS stderr read status.
   - fex_guest_cpu.cpp: KYTY_FEX_TRACE_CALLER=begin:end filters HLE tracing before quota.
   - pm4Handlers.cpp: raw DccControl values logged before existing independent_64b && independent_128b assertion; assertion unchanged.

## Actual rendering evidence
- Prior baseline/sampler/BDA runs reached FC27 logo, connecting/EA offline error UI; some text truncated, button icons missing.
- Clicking J (Cross) closes EA error; later "IS YOURS"/"Let's Play" then main-menu music banner seen, but background red and text/artifacts remained.
- Latest GDS-on run reached ~6397 presented frames; screenshot and PPM both solid GREEN. Menu title showed 30 FPS; other intervals 0–23 FPS. This is a correctness regression, not a performance result.
- Cube-without-GDS remained red. No match, correct intro video, or effects verification.
- No reliable Vulkan ground truth: native ARM64 reference build started then stalled loading a 137MB pipeline cache until timeout. No A/B visual comparison obtained.
- GPU capture attempt using gpucapture on Metal layer boundary 3 failed at 0 resources: target disconnected/exited. No .gputrace produced. Cause unproven; don't claim capture analysis was done.
- Font/FontFt/AvPlayer/Videodec2 HLE trace during first ~1400 frames showed no actual intro movie decode.
- Engine.MovieVLC.LibVLC.prx mapped 0x8930000000..0x893036c000; caller-range tracing during intro produced no HLE calls from that range. This does not prove there is no movie elsewhere.
- APR trace ~3045 successful reads/~4.07GB/no short reads; no identified producer of missing button texture.
- Button texture 40x32 tfmt56 tile9 dst_sel6547 is zero in GPU AND guest memory (addresses vary).
- KYTY_SNAPSHOT_LINEAR=1 did not reliably fix text and caused ~5 FPS at EA UI. Do not enable by default.

## Most useful next action
1. Rebuild to incorporate the now-passing vertex ReadFirstLane change. One bounded baseline run with GDS OFF, first cube OFF then only if useful cube ON. Inspect real pixels and skip counts; avoid large batches of speculative changes.
2. Investigate native Metal EOP ordering — promising UNTESTED hypothesis:
   - src/graphics/guest_gpu/graphicsRun.cpp: PublishEopLabel around 2014.
   - Native logs gpu_labels=0, many cpu_labels. When cache.TryRecordGpuLabelWrite returns 0 and deferred is false, code memcpy's completion label immediately, before Metal GPU completion.
   - Existing scheduler/deferred callbacks are for the legacy graphics scheduler and may not track native Metal completion.
   - Premature label visibility could let guest recycle staging, indirect command or text buffers while Metal still reads them. This could connect text corruption, zero icons and occasional bad PM4/indirect data. NOT proven.
   - Suggested diagnostic: gated native Metal WaitIdle before publishing an EOP label, one short same-scene visual A/B. Never make per-label waiting the production default or claim FPS from it. Check Metal mutex scope/deadlock. Read synchronization skill before implementation.
   - No EOP code was changed in this session.
3. Remaining unsupported cases in logs: vertex ReadFirstLane (binary predates fix), FPCmpClass, ShiftLeftLogical64, formatted descriptor Invalid, one non-2D derivative sample; GDS intentionally disabled. Fix only with concrete shader evidence.
4. If red/green cause remains opaque, existing per-pass helpers: KYTY_TRACE_PS_RT=<address>, KYTY_RT_AFTER=<hash> and KYTY_RT_AFTER_N=n. Common main RT 0x8240c00000. No debug-red shader flag was present in diagnostic process env.
5. Don't implement a broad wave64/GDS rewrite without matching actual allocation/output data. Current ds_append behavior and simd32 vs wave64 need focused tests.

## Files and artifacts
- All changes uncommitted. Initially tracked tree clean. Preexisting untracked .DS_Store, _DownloadData/, _SaveData/, _TempData/, tools/msl/out/ must be preserved.
- New source: src/graphics/host_gpu/metal/metalSampler.{h,mm}, metalAddressSpace.{h,mm}.
- New tests/scripts: tests/metalMslTests.mm, tests/metalSamplerTests.mm, tools/test_metal_msl.sh, tools/test_metal_sampler.sh.
- Existing modifications: metalRenderer.mm, MslEmitter.cpp, pipelineCache.cpp, kernel memory.cpp/.h/memoryAddressSpace.inc, libAmpr.cpp, fex_guest_cpu.cpp, runtimeLinker.cpp, pm4Handlers.cpp, VirtualMemoryAllocationTests.cpp.
- Durable evidence snapshot: .porting/artifacts/2026-10-22/ (selected runtime logs, latest passing tests, before/after contact sheets, original launch script).
- Full raw data/MSL/PPM: /tmp/kytx-font-video.8uf0mA. This directory is temporary; durable copies cover key logs, not everything.
- cube-run.log = cube ON/GDS OFF/red; bda-completed.log = BDA on/cube OFF; gds-flow-run.log = cube ON/GDS ON/green. trace-run.log = failed capture run, not HLE trace.
- Latest passing test: test-msl-vertex-wave-real-pass.log. Old test-msl-vertex-wave.log is the superseded bad test setup.
- BDA later fatal: DCC independent64/128 assertion. Earlier formats run had indirect user config offset 0xffffffff. Neither fatal cause proven.
- Temporary app /tmp/KyTxDiagnostic.app launches native binary; its launcher appends logs (CUA can auto-launch a dead app). Always check ps BEFORE cua.getApp to avoid accidental relaunch/log contamination.
- SDL keyboard J=Cross, L=Circle, arrows=D-pad, Return=Options. CUA after actions: refresh AX/screenshot.
- Reference-only ARM64 Vulkan binary: /Users/tx/Developer/KyTx-diag/_Build2/macos-arm64/kyty_emulator; reference MoltenVK path /Users/tx/Developer/qt-tools/moltenvk/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib. KYTY_FORCE_PIPELINE_CACHE=0 may avoid cache-load stall, not tested. Never run /Users/tx/Developer/kytyps5-ufc5/_Build/macos/kyty_emulator (x86_64).

## Skills used / next
Already applied: debugging-rendering-issues, managing-metal4-resources, using-game-controller (inspection only), using-gpucapture, using-gpudebug, porting-handoff.
Read managing-metal4-synchronization for EOP work; using-metal-validation before explicitly enabling validation. No MSC or DXIL path.
The earlier HANDOVER sections are historical. In particular claims that samplers are all default, cube faces never upload, and control flow is only a pc-switch are superseded by this handoff.

