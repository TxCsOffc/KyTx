> Superseded for current state by [15:12 handover](porting-handoff-fc27-metal-2026-10-23-1512.md). Keep this file as earlier evidence/history.

# FC27 native Metal — 2026-10-23

Current task: fix intro, icons/effects and rendering artifacts, then reach 30+ FPS in a correct playable match. Goal remains incomplete. FEX + direct RDNA→MSL only; no Rosetta, Vulkan allowed only as a visual reference. No commits made. Branch `metal-native-checkpoint`, HEAD `c30d5d81`; preserve all existing dirty work.

## Valid single-frame GPU trace

User asked for one frame per capture and inspection in Xcode with computer use. Obtained:

`/Users/tx/Developer/fex-spike/runs/captures/fc27-20261023/single-frame/lldb-present.gputrace`

- 9.6 GiB, contains index, gpudebug static parsing succeeds, Xcode 27 replay succeeds on Apple M5 Max/macOS 27.2.
- Exactly **one CAMetalDrawable present boundary**, captured after the guest flip log reached 700; NOT an exact capture of guest flip 1400. Do not relabel it as frame 1400.
- 12 command buffers, 827 encoders (270 render, 340 compute, 217 blit), 424 draws, 254 dispatches, 17811 API calls. These are trace counts, not evidence of correct rendering or gameplay FPS.
- Xcode showed the same solid red image with small `Hold` text. Replay stopped afterwards to release GPU memory; trace remains open with Replay available.
- Earlier `frame800.gputrace` is incomplete/corrupt (missing index); do not use it. Earlier failures included SIGILL during programmatic `stopCapture` and PM4 `ffffffff` before the requested frame. One-frame count alone did not prevent those failures.

Successful capture route (emulator PID was 57967 and is now stopped):

```sh
# Same baseline as tools/run_fc27.sh, plus MTL_CAPTURE_ENABLED=1 KYTY_SYNC_FLUSH=1.
# No KYTY_GPU_CAPTURE: external CLI captures the present interval.
xcrun lldb --batch \
  -o 'settings set platform.plugin.darwin.ignored-exceptions EXC_BAD_ACCESS' \
  -o 'process handle SIGSEGV SIGBUS -n false -p true -s false' \
  -o run -o 'thread backtrace -c 32' -o 'register read pc' \
  -- /Users/tx/Developer/KyTx/_Build/macos-arm64/kyty_emulator \
     --game /Users/tx/Developer/PPSA34015
# From the run working directory /Users/tx/Developer/fex-spike/runs:
gpucapture config --pid <pid> --shared-mem-opt disabled
gpucapture start --pid <pid> --boundary 3 --count 1 --output <new-path.gputrace>
```

LLDB needs ignored Mach EXC_BAD_ACCESS so FEX/mprotect faults reach the BSD signal handlers. Do not claim LLDB proves the cause of the earlier SIGILL: it did not recur in the successful run. Capture completion log is in `artifacts/2026-10-23/gpucapture-single-present.log`.

## Xcode findings (computer use, actual replay)

1. CB12 / RenderEncoder270 / API17807: final fullscreen triangle, pipeline `vs_present – fs_present_tex`. Fragment texture0 is **MTLTexture71**, RGB10A2Unorm, 1920×1080; already red. Presentation is not the first red producer.
2. CB9 / ComputeEncoder255 / API16087: groups30×1080×1, threads64×1×1, pipeline MTLComputePipelineState3/main0(5). Inputs include **MTLTexture2364** RGBA8Unorm with mips, already red; scene **MTLTexture13462** (view RG11B10Float) appears black; MTLTexture68/69 is 3D LUT; MTLTexture70 is tiny 128-byte default. Output is Texture71.
3. Filter navigator by `2364` (just number; `MTLTexture 2364 ` missed attachment matches) to find overlay RT draws. First matching draw: CB9 / RenderEncoder54 / API9049, triangle strip4, PS main0(49), VS main0(50), pipeline19. Color attachment Texture2364 Load|Store; input Texture2372 RGBA8, 3840×1080, has real background image. Output remains red, with prior `Hold` text present.
4. Pipeline19: write mask All; SourceAlpha/OneMinusSourceAlpha RGB, One/OneMinusSourceAlpha alpha. CullNone, viewport/scissor1920×1080. Post-VS positions are (-3840,+/-1920,1899,1920), (+3840,+/-1920,1899,1920), UV corners0..1; other params `(1,1,1,1)/(0,0,0,0)/(1,1,1,1)`. No obvious missing fullscreen geometry. Pixel shader source is generated MSL with ballot/WQM and alpha math, not an unconditional red probe.
5. This trace does NOT yet identify the first frame that introduced red. The overlay loads persistent content; current frame can inherit red. Do not infer a root cause only from these observations.

## Code changes in this session

All in `src/graphics/host_gpu/metal/metalRenderer.mm`:

- `PrepareGraphicsImages` resolves image conversions BEFORE pipeline, viewport and resource binding in DrawIndex and DrawAuto. `AliasConvert` can end/reopen render encoders; previously a conversion in binding erased already-set pipeline state, producing `enc=1 pipe=0` draw skips.
- Conversion cache is keyed by `(addr,width,height,format,tile)`, not just address. Descriptors sharing an address no longer repeatedly evict each other's prepared conversions.
- Failed native shader or nil PSO draws now return. Previously logged as dropped but could draw with a preceding pipeline/resources.
- Shared `DumpRtBeforeDraw` supports DrawIndex AND DrawAuto before state setup; adds `KYTY_DUMP_RT_DIR`, optional `KYTY_DUMP_RT_FORMAT` (numeric MTLPixelFormat), PS hash0 wildcard, selects exact RT format, logs first snapshot inputs. Default path `/tmp/fix6`.
- One-time compute descriptor/object format mismatch diagnostics. Remaining mismatches include guest36 actualRGBA32Uint/wantRG11B10Float and other size-changing aliases; not all are fixed.
- Mipped colour RTs now include PixelFormatView usage, and `ReinterpretFormat` allows full-chain views of compatible same-size formats. Earlier guard returned mipped RGBA8/RGB10A2/RG11B10 objects unchanged to R32Uint raw kernels. Standalone GPU check verifies actual bit reads across mips0/1 and uint writes through mip1 views: **8 cases, 0 failures**, Metal API validation enabled. Source/log in artifacts. Main build succeeds.

Before the final mip-format fix, same-scene runs after encoder-state preparation still showed red at flip1300. Final runtime result after mip-format fix: **still red**, independently inspected PPMs at flip600,800,1000. First plain launch hit the recurring PM4 `ffffffff` assert before the target scene. Repeat with the LLDB settings above, baseline flags plus `KYTY_SYNC_FLUSH=1 MTL_CAPTURE_ENABLED=1`, reached the heavy scene; dumps remained red. Stopped that diagnostic process explicitly after obtaining the three images; emulator/LLDB no longer running. Log and contact sheet: `artifacts/2026-10-23/mip-format-game.log` / `mip-format-game-frames.png`. No valid FPS comparison claimed.

The earlier R32Uint-vs-mipped-RGBA8/RGB10A2/RG11B10 mismatch warnings no longer appeared in this repeat, but other mismatches remained (including guest20 actualRGBA32Uint/wantR32Uint, guest36 actualR32Uint/wantRG11B10Float). These need descriptor/resource-specific investigation. Note that the shared `NoteFallback` text says `default substituted` for this diagnostic even though a mismatched object is retained: the format mismatch is real, the generic suffix is imprecise.

Next focused analysis can reuse the existing valid trace: inspect/debuginvoke the first background fragment draw (CB9/Render54/API9049) to verify its sampled value and fragment output, then track the persistent overlay's prior writer. Do not repeat expensive captures solely to reopen the same red scene.

## Existing work from earlier 2026-10-23 agent

See HANDOVER.md's dated section for generic GF2 tile/untile, per-address persistent depth, linear video pitch, cube graphics bindings and buffer-backed reinterpretation. Generic surface conversion is partial: mips/layers/3D not handled; CPU writes/reuse are not tracked, writer-age2-flip heuristic only. Cube compute remains opt-in; GDS remains OFF because it regressed to green in earlier tests.

Important correction to earlier direction: intro video is CPU libVLC YUV, draw `fe48d4a99493027c`; AvPlayer suspicion is obsolete. Background PS actually observed for overlay RGBA8 draw at flip1000 was `751942f534c2702e`, not older `58effb53710a240a`.

## Diagnostics cautions / pending

### Sustained fixes follow-up (14:35–14:50 Asia/Tashkent)

User explicitly asks to keep fixing defects, not just report one at a time. New retained changes:

- MslEmitter raw U32/x2/x3/x4 loads now check EACH component and activity condition, return0 beyond bounds; stores likewise skip out-of-bounds components rather than clamp to the last word. Handles size0 and indicesnearUINT_MAX without underflow/overflow. Formatted component reads also guarded by component byte extent and activity.
- LDS allocation uses compute input's lds_size_dwords (fallback8192 only when absent); atomic/append limits use same count. Shared loads AND stores honor predicates and per-component bounds. This removed initial validation64KB allocation on small-LDS kernels; truly larger instrumented kernels can still exceed32KB (example3ef1bf7a988c6913,5632dwords -> validation45056bytes). Do not claim all LDS instrumentation limits solved.
- Graphics native storage image bindings now bypass sampled DST_SEL swizzle, use Read|Write residency, and record storage writes. Shader500ea6b64a68d26c line1739 was writing image12, previously wrongly bound through a read-only swizzled view. Linear RT creation includes GuestTextureUsage(fmt), so storage aliases can be writable.
- RGBA16Unorm added to writable formats after standalone GPU write/read test passes under API+GPU validation. CS243db11c16193888 line10326 writes RGBA16Unorm image7; previous usage17 lacked ShaderWrite.
- Graphics buffer size constants clamped to real allocation/snapshot extent and re-uploaded after binding. Vertex b2c4c2e311c5bc76 line226 previously read past32768-byte backing while sizes[] overclaimed accessible bytes.
- Added144 raw buffer boundary GPU cases plus LDS final-word/predication/OOB checks to tests/metalMslTests.mm. Whole suite passes under API+GPU validation; ARM64 build and diff check pass.

Full shader validation follow-up logs: bounds-lds.log showed further storage violations, storage.log removed them but showed LDS predicate/vertex buffer-size errors, shared.log after latest fixes has no `Invalid Metal usage` or `failed assertion` through flip1000. Visual shared600 is connecting UI, shared800 is expected EA-unavailable dialog. This is LIGHT scene, not a proof red-heavy scene fixed. Tests/logs in validation-20261023/; durable copy pending.

Interactive debug launcher now exists at `/Users/tx/Developer/fex-spike/runs/KyTxDiagnostic.app`, identifier org.kytx.diagnostic, executable shell execs ARM64 current binary with FEX+MSL baseline. CUA getApp(path) WORKS and sees the game window (unlike earlier bare executable). Launcher logs to validation-20261023/interactive.log; includes MTL_CAPTURE_ENABLED=1, no SYNC_FLUSH or shader validation. Keyboard Cross is J. Currently launching to dismiss EA dialog and reach a later scene. Do not leave this app running unintentionally or re-open it with CUA after stopping: getApp auto-launches.

Interactive result: heavy red scene still reproduces (frame592); pressed J once, later titleframe639 FPS26 (not a gameplay FPS metric). Captured NEW SINGLE PRESENT successfully without LLDB: PID5431, `gpucapture config --pid5431 --shared-mem-opt disabled`, then boundary3/count1. Capture took22.1s,11957 resources,24563MiB downloaded,16GiB on disk. New path `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261023/shared-fixed-one.gputrace`. App explicitly stopped after capture. Xcode opened new trace through computer use and replay is preparing. Old trace replay stopped first. No capture corruption reported; full replay confirmation pending.

Important continuing state: source changes above retained, main build and GPU suite pass; validation shared.log reached1075 with no errors in its LIGHT scene; heavy non-validation interactive scene still red. Durable gzip logs bounds-lds/storage/shared plus new-capture and tests copied to artifacts. Need trace NEW capture for actual first red writer; old indices2364/etc do not apply.

### Shader validation follow-up (14:22 Asia/Tashkent)

Global API+shader validation confirmed enabled. `shader.log` in the new run directory reports texture usage violations from ConformToDeclaredArray views, a buffer OOB load at offset196608 in a196608-byte GuestMemoryUMA buffer (near CS4231266a78a83ef3), then stops at threadgroup memory65536>32768 while instrumenting a kernel with a static32768-byte LDS array. Do not claim global shader validation passes.

Added compute PSO descriptors labelled `CS <hash>` in metalPipeline.mm. This aids both validation and future Xcode traces. Labelled repeat attributes the texture WRITE violations to CS173f6bb015ca4194 (source line591, image_1.write) and CS70c1167216da9737 (line337). Both target RG11B10Float cube/array images created with usage17=ShaderRead|PixelFormatView, lacking ShaderWrite. This is a rights error, not merely missing parent residency. Sources dumped to validation-20261023/msl/. The 3D path had the same unsupported-format assumption and substituted RGBA32Float.

Standalone Metal tests with API+shader validation demonstrate real RG11B10Float writes/reads succeed on this M5 Max for 2DArray AND 3D,64 pixels each. Source: validation-20261023/packed-write.mm and packed-write-3d.mm. Added RG11B10Float to GuestTextureUsage's ShaderWrite set. Cube creation already uses that helper; 3D no longer substitutes RGBA32Float for this format. Build passes. Runtime repeat uses API validation globally and shader validation ONLY on CS173f6bb015ca4194,70c1167216da9737,17aeefc07e25295e,92283f13a80c2cee,b3300709113f5521 (DEFAULT_STATE=none, ENABLE_PIPELINES labels), avoiding the instrumentation LDS limit in unrelated kernels. Result pending in packed.log.

Results: the targeted-selector attempt (`packed.log`) ALSO hit the LDS65536>32768 assertion; selecting pipelines did not avoid it in this runtime. Next global trial with THREADGROUP_MEMORY=0 (`packed2.log`) still hit that assertion. **However, both trials no longer report the RG11B10 texture-write violations** seen before the format usage fix. Buffer OOB remains.

Actual buffer OOB cause identified in dumped CS4231266a78a83ef3, line118: uint4 load clamps its BASE index to `(sizes[0]>>2)-1`, then reads base+1,+2,+3 past the buffer. Need per-component bounds/zeroing, including zero-size handling, and focused GPU regression. No emitter change made yet.

Plain API validation run after packed-format fix (`packed-api.log`) reached800+ without API assertion, then stalled with wait_dirty_open rising; stopped explicitly. Frame600 remains red; frame800 is completely black. These do not establish visual correctness or an improvement. Packed format rights fix is validated at resource-access level only. All emulator test processes stopped. New compute PSO labels remain useful for future Xcode capture (old trace predates them). Source builds and diff check pass. Selected new logs/tests copied into artifacts/2026-10-23.

Correction: Metal format125 is RGBA32Float, not RGBA32Uint; earlier notes describing125 as Uint were imprecise.

### API validation follow-up (14:15 Asia/Tashkent)

Temporary `/tmp/kytx-20261023-overlay` was no longer present. New evidence directory is `/Users/tx/Developer/fex-spike/runs/validation-20261023/`. Existing durable artifacts and the valid trace remain present.

Running baseline with `MTL_DEBUG_LAYER=1 KYTY_SYNC_FLUSH=1` hit a real Metal API assertion: SetScissorRect y0+height2048 exceeds render-pass height1080. Stack is DrawIndex. Existing renderer guard queried a texture cache by guest address and could select another surface/format/size instead of the actual attachment; it also ignored mip levels.

Changed `metalDraw.h/.mm`: BeginRenderPass saves the minimum actual colour/depth/stencil attachment mip extents; SetScissor intersects the guest rectangle with those extents using overflow-safe subtraction; empty scissors skip direct/indexed/indirect draws. Removed both address-cache scissor clamps from renderer; all draw paths pass the guest scissor to the executor. ARM64 build and diff whitespace check pass. Repeat `fixed.log` passed the earlier scissor failure and reached flip550+, then caught the NEXT API error: PSO inputPrimitiveTopology Triangle used with a non-triangle primitive.

Second fix: DrawIndex/DrawAuto now put guest primitive type into the existing pipeline cache key (equality already compares it); `metalPipeline.mm` chooses Point/Line/Triangle topology class to match TranslatePrimitiveType's supported mapping. Previously topology was unconditionally Triangle and primitive_type key stayed0. ARM64 rebuild passed. Third validation run is `topology.log`. User specifically reminded to keep using Xcode: reopened the valid `lldb-present.gputrace` via computer use; it is open at the Replay screen. Do not interpret this old trace as a capture of the new binary.

Third validation result: reached flip875 without Metal API assertions or GPU errors, then stopped on the known PM4 `ffffffff` assertion. Frame600 shows FC27 logo/background; frame800 is red with a small lower-right overlay. **Red is NOT fixed.** Baseline run's frame800 is connecting UI; scene timing differs, so their contact sheet is not a matched-scene performance or visual A/B. Durable logs: artifacts/2026-10-23/api-before-scissor.log, api-after-scissor.log, api-after-topology.log; contact sheet api-frames.png. Emulator stopped. Source retains ONLY the two validated API fixes in this follow-up (scissor bounds and primitive topology); no broad writer preference or compute-state rewrite.

Correction to a code-reading hypothesis: BeginComputePass always creates a fresh encoder and resets m_has_compute_pipeline. A nil compute PSO therefore does not reuse an old pipeline in this path. No compute guard/state rewrite was made in this follow-up.

### Follow-up: Xcode pixel shader debugging

Replayed the SAME valid trace, selected API9049, clicked Debug Shader, chose Fragment at pixel(405,539), then Debug Pixel. Xcode successfully gathered GPU execution data. The execution navigator can be filtered by source text (e.g. `v7 =`); selecting an executed row shows values after that source instruction.

- Entry: interpolated param0 UV=(0.3555989861,0.4995370209), z/w=1; param1=(0,0,0,0), param2=(1,1,1,1). Argument buffer resolves image_0 to Texture2372.
- Actual `sample` result v7 uint bits = (0,0,0,1046273245), i.e. floatRGBA=(0,0,0,0.2156862766). The earlier dumped background input at that UV also has RGB=(0,0,0).
- Last executed scope/return: RGBA=(0,0,0,approximately0.2). This pixel's background shader does not directly produce red. Its partial alpha can retain preexisting red. This does not identify the first bad write elsewhere in the frame.
- Replay stopped again before running the game.

Follow-up code inspection: `s_cs_written_by_addr` is also overwritten by GRAPHICS READS (`RegisterComputeWrittenImage` in BindMslStage), so it is not a reliable latest-writer registry. Mipped RTs are excluded from `RecordComputeStorage`. Tried recent-writer preference for compute (matching dims/texel size, freshness2flips), plus recording mipped RTs. First trial exposed illegal depth-as-colour candidates; excluded depth and required an exact resulting view format. Corrected trial nevertheless produced **GPU Hang** (`kIOGPUCommandBufferCallbackErrorHang`) at 03:47:28 Asia/Tashkent. **REJECTED AND FULLY REVERTED both writer-preference and mip-writer registration.** Rebuilt the previous source successfully. Log: `artifacts/2026-10-23/rejected-writer-preference.log`. Later dumped frames from this failed run are not valid correctness evidence.

Do not reapply broad writer preference based only on dimensions/byte size. Need to verify declared dimension, tile layout, storage-write capability and actual data ownership for the specific copy/clear kernel. Same-format buffer-backed colour RTs may lack ShaderWrite; selecting such a texture for storage is one untested explanation of this hang, not a proven GPU-fault attribution. Generic mip layout conversion remains unsupported. Next useful validation is targeted Metal validation plus object/descriptor/usage logging for one raw-copy kernel, rather than changing all compute aliases at once.

- `KYTY_DUMP_CS_IMGS` commits in the middle of compute binding after pipeline/push setup, so it can invalidate the encoder and alter dispatch behavior. Repair or move before binding before trusting that diagnostic.
- `KYTY_SYNC_FLUSH=1` did not remove red before mip-view fix; only rules out some flush-level races. Native EOP label publication is still an untested ordering hypothesis; no EOP changes this session.
- PM4 `ffffffff` and DCC flag assertions recur; don't disable them to get more frames.
- ReadLane wave64 masking, GDS counters/GPU-driven geometry, zero button icon producer and further alias formats remain open.
- No correct playable match or 30+ gameplay FPS evidence.
- Main build and `git diff --check` pass. Emulator and own debugger/replayer processes stopped. Xcode keeps the valid trace open, replay stopped to release GPU memory.
- Full temporary evidence: `/tmp/kytx-20261023-overlay/`. Selected durable evidence: `.porting/artifacts/2026-10-23/`. Valid huge trace stays at the capture path above; not copied into the repo.
