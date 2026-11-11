# Native Metal sampled remap and mip texture identity — 2026-11-10

## Scope and evidence

Repository `/Users/tx/Developer/KyTx`, branch `metal-native-checkpoint`, HEAD `9534b33718bd10104a4baf708758d91f61e2ef8d`. Inherited dirty and untracked work retained. No staging, commit, push or reset. Ledger CLI is unavailable; STATE/HANDOVER/worklog are the fallback memory.

After the user unlocked the Mac, ML5 PID9723 was already absent. It had naturally aborted on a sampled image lookup at VA `0x810d000000`: reader RGBA8 3840x1080, historical RGBA8 RT 2048x2048 with 10 levels. The old error's “another texel size” wording is misleading: guest format56 and Metal70 are both RGBA8, four bytes per texel. This was a size/provenance mismatch, not evidence that AMD tiling is unavailable on Metal.

SR3 and SR4 diagnostics resolved the current reader's full 17,694,720-byte tiled allocation. It did not overlap the canonical physical ranges of the old writer. SR5 established the precise transition: old canonical target at VA `0x8113000000` had physical backing `0x8cc00000`, 22,413,312 bytes; current reader resolved to `0x161200000`. Its last image writer was a different GPU texture (serial18589 versus RT18580). The registry also contains depth/color RT writers, so the pointer/serial observation alone does not establish a compute shader producer. A graphics-only remap check was replaced by a shared sampled/storage binding check; the uncovered producer is still being traced.

## Implemented behavior

1. Mip RT cache identity is now the exact tuple `(address, format, width, height, levels)`, represented by `RenderTextureKey` and an ordered map. The former XOR encoding has real collisions for distinct page-aligned VAs and sizes. A GPU negative control obtains two wrong shapes under that encoding. This bug is proven independently; it has not been proven to be the cause of the earlier SR1/SR2 load/import errors.
2. Canonical RT history records exact physical spans of its tiled allocation and its last writer serial. Physical identity comparison covers the entire allocation and normalizes contiguous mapping splits. Unrelated mappings do not invalidate it. Full/partial remaps and gaps invalidate the old image.
3. `CanImportRemappedImage` requires every retained RT producer at that VA to have known canonical backing that changed. A newer detached compute writer, including a different serial on the same texture pointer, blocks guest-memory import. No age, sampled hash, blank texture or missing producer can establish this proof.
4. `CanonicalMipImage` applies this proof before both sampled and storage binding, including the preparatory image pass. The first supported image at the reused VA imports the actual current physical GPU bytes and registers that range. Existing storage writeback then exports real tiled bytes at the producer boundary. Subsequent graphics, compute, CPU and raw-buffer readers use that physical range. This covers supported tiled 2D/2D-array descriptors; it is not a global conversion of every legacy resource path. The game still encounters an uncovered writer transition (SR7 below), so do not report the full game remap issue as solved.
5. Failed alias, render-load and image-import diagnostics now include the relevant layouts, host shape, tile-table extent and physical mapping identity. Failures remain explicit.

The separate Metal parent/view lifetime experiment retained the parent with a live view and did not reuse its pointer across 10,000 allocations. It did not establish a view-cache lifetime defect; no workaround was added for that hypothesis.

## BC physical transfers

SR6 reached the adapter's explicit missing BC transfer error. Added `metalBcTransfer.inc` and public `EncodeTileBcImport/Export` functions. They move exact BC1–BC7 compressed blocks between guest tiled bytes, a uint element texture, a GPU buffer and a native Metal BC texture. No CPU readback, decoder approximation, clamping, recompression or sentinel overlay participates. Import/export handle physical mip selection, shared tails, 2D array slices, partial edge blocks and mips smaller than4x4. Bridge resources survive unretained command buffers. Canonical sampled images now use this transfer and ShaderRead usage; a writable BC descriptor still explicitly requires an uncompressed block descriptor, as used by block-producing kernels.

The first new test incorrectly attempted a BC render-target layout, which the existing AMD format/layout validation rejects. Changed the positive layout set to the four supported Standard256B/4KB/64KB/PRT families; did not relax render-target validation. The corrected full suite passes.

Apple's buffer/texture blit contract specifies compressed row strides in block rows and clamps partial block extents at the texture edge: [buffer to texture](https://developer.apple.com/documentation/metal/mtlblitcommandencoder/copy%28from%3Asourceoffset%3Asourcebytesperrow%3Asourcebytesperimage%3Asourcesize%3Ato%3Adestinationslice%3Adestinationlevel%3Adestinationorigin%3Aoptions%3A%29?language=objc), [texture to buffer](https://developer.apple.com/documentation/metal/mtlblitcommandencoder/copy%28from%3Asourceslice%3Asourcelevel%3Asourceorigin%3Asourcesize%3Ato%3Adestinationoffset%3Adestinationbytesperrow%3Adestinationbytesperimage%3Aoptions%3A%29?language=objc).

## Verification

- Final full coherence suite, Metal API and GPU shader validation: PASS, `/Users/tx/Developer/kytx-bc-coherence-final.log`.
- New cache test: three real GPU mip allocations, 174,720 bytes verified, two legacy wrong-shape controls, unaffected mips preserved.
- New remap test: 8 physical identity controls and 8 writer-authority controls. Raw GPU updates and CPU updates are observed. An actual storage shader writes an image, exports tiled bytes and imports a separate sampled image in the same command buffer without an intervening CPU wait/readback. 206,700 bytes verified; retired backing and allocation guards preserved.
- Existing alias/mip/physical/depth suites remain green: 28 alias pairs; 9 mip layouts and45 negative cases; D16/D32/S8 exact physical memory tests.
- BC suite: 14 format variants times4 tile families =56 cases; 392 invalid-contract controls. 11,954,176 exact bytes, all six mips and three layers,37x21 odd pixel extents, a separately selected2x1 tail mip with untouched neighbours. A valid BC1 all-red image independently decodes to the expected35 RGBA texels in an actual GPU shader.
- Full pipeline suite with API+GPU validation: PASS, `/Users/tx/Developer/kytx-sampled-pipeline-final.log`. It ran before the final relocation of the remap proof; renderer source then rebuilt, and the final coherence suite above includes the shared proof and storage test. No pipeline executor or shader emitter behavior changed in this stage.
- Final product build PASS: `/Users/tx/Developer/kytx-bc-build.log`. Main and launched SR7 SHA256 `095a29fcb3100df3200966517b23bf74c9dd0441f5d2535c4d6a959e7bc791c1`. Intermediate SR6 SHA `760b415407d5499138faa70bb41223d1ed5752d825fd9d3b9f607591821abf7a` is preserved separately.
- Full MSL/CFG was not repeated in this stage. Previous MSL success and the historical Apple clip-guard CFG failure remain as recorded in the preceding stage.

## Game runs and continuation

All intentional runs used one instance, the existing40GB guard, no timer kill and no scripted input loop. Capture/validation layers were disabled in the game. `extra_env_s1.sh` was restored by each launcher's finally block. Exact PIDs, SHAs, launch settings and logs are in workspace `work/metal-sampled-remap/game-sr*.json` and launcher files.

- SR1/SR2 naturally aborted on canonical image import/render load; detailed metadata was added afterwards.
- SR3 proved the old/new physical allocation difference; SR4 showed that the graphics-only attempt did not cover a preceding newer writer.
- SR5 proved the newer writer serial and motivated the shared binding fix.
- CUA accidentally auto-relaunched default c9 after SR3 died. Frame127 belonged to that new process, not SR3. Its exact owned PID12005 was checked and SIGTERM sent to stop this accidental run before rebuilding. This is the only signal sent in this stage. Future observations first inspect running state and never select a dead app.
- SR6 PID13214 started13:58:16+05, log `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sr6-135815.log`, source pairs `/Users/tx/Developer/kytx-native-sr6-sources`. Visual check and final outcome recorded below.
- SR6 naturally aborted on missing canonical BC transfer; the implementation and tests above followed. Readable EA message frame356, then loading frame400/458, no validated3D scene.
- SR7 PID13687 started14:09:17+05, log `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sr7-140916.log`. EA message frame424; one manual500ms j, then frame470 observed. It naturally aborted on the old 3840x1080 sampled lookup at VA `0x810c000000`, flip398 (guest trace and presentation counters differ). Old canonical history still referred to physical `0x8b800000`, current reader to `0x161600000`; the new shared proof did not authorize this transition. Do not weaken its newer-writer guard to make the lookup succeed.
- Crucially, SR7 had GPU hang/recovery errors before that rejection (first at log line2983). SR6 and SR3 already have the same class of error, so this predates the BC implementation and the current remap work. This establishes chronology, not its cause. CPU shader translation continued after GPU recovery; later coverage counts do not prove successful GPU execution. No valid full-game image or performance result was obtained.
- SR8 uses the same final binary with existing `KYTY_MTL_FAULT_INFO=1`, `KYTY_MTL_GPU_TIME=1`, `KYTY_LOG_CS_STORES=1` to identify the first failing encoder and image writer; diagnostic timing is not an FPS comparison. Final observation is appended below.

The old shadow squares, missing lighting, clothing, hair, neon and upscale defects are not declared fixed. A readable EA dialog or a loading logo is not validation of a 3D scene. The first GPU failure and the uncovered remap writer transition take priority over visual claims. Legacy age/hash/new-image heuristics elsewhere, general BC producer ownership, within-dispatch alias behavior, distinct physical writable depth/stencil planes, general PS LDS and the previously listed unsupported shader features remain open.

Baseline snapshots, source hashes, validation logs and a stage-only patch are preserved separately from the preceding `metal-mesh-link` artifacts. Do not regenerate that historical patch from these newer files.

### SR8 live checkpoint — 2026-11-10 14:21 +05

PID13951 is still running with SHA095a29fc…; its log has no matched GPU fault/reject yet in the current checkpoint. Mac automatically locked again. `cua.getState({emit:false})` returned empty apps **and an explicit Mac-locked error**. Empty apps was initially misread as process exit; `ps` corrected that immediately. No app was launched or signal sent. A new async request asks the user to unlock the Mac; no input or screenshot is possible until then. This handoff is pending, not a completed visual verification. Original environment restoration is verified. The verbose diagnostic log remains in Developer; a compact event snapshot is in workspace artifacts. Do not blindly select a dead app when resuming.

### Follow-up: SR8 final and completed color-memory stage

SR8 naturally aborted before the unlock reply. Its FaultInfo state4 identifies VS1d58ae6f8ed54b0c / PSbaf34dd57c65d299; exact guest pair and further implementation are recorded in `.porting/REPORT-metal-color-ownership-2026-11-10.md`. That follow-up added canonical storage provenance, single-level tiled RTs and scissor-bounded physical writeback. Final SP4 materializes the former3840x1080 and1920x1080 remapped readers but still has the GPU failure and then a stale3D format rejection. The previous live/locked checkpoint above is historical, not the current process state. No visual fix established.
