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

# WORKLOG: Metal anomalies (NaN / neon / overexposure), 2026-11-08

Модель сабагентов в окружении: `flash` (в `invoke_subagent` доступны `inherit`, `flash_lite`, `flash`, `pro`; выбран `flash` как ближайший аналог `gemini 3.8flash`).

## WP0. Подготовка (16:35–16:43 local)

1. **Чтение документов (раздел 0):**
   - Прочитаны `.porting/HANDOFF-claude-2026-11-08-anomalies-nan-neon.md`, `.porting/PLAN-anomalies-2026-11-08.md`, конец `.porting/CODEX-clothes-metal-2026-11-07.md`, `.porting/porting-handoff-fc27-metal-2026-11-01-claude.md` (1..70), `.porting/porting-followup-fc27-metal-2026-10-27-rect-indirect.md` (1..60), а также комментарии и код вокруг `CsStorageEntry`, `RecordComputeStorage`, `LastGpuWriterTexture`, `SyncStorageIntoGuestTarget`, `GraphicsImageFor`, `AliasConvert` в `src/graphics/host_gpu/metal/metalRenderer.mm`.

2. **Проверка git / диска / процессов:**
   - Команда: `git status --short && git log --oneline -3`
   - Результат: ветка `metal-native-checkpoint`, HEAD `9534b337` (под ним `945d467d`, `878ee277`). Список модифицированных файлов (`HANDOVER.md`, `metalAddressSpace.mm`, `metalBuffer.h/.mm`, `metalRenderer.mm`, `metalShader.mm`, `traceWindow.h`, `window.cpp`, `MslEmitter.cpp`, `kernel/*`, `fex_guest_cpu.*`, `tests/metalMslTests.mm`, `metalNullImage.h`) полностью совпадает с разделом 3 хендоффа.
   - Команда: `df -g /System/Volumes/Data` -> свободно **722 ГБ** (≥ 100 ГБ).
   - Команда: `pgrep -lf kyty_emulator` -> пусто (`no kyty_emulator running`).

3. **Сборка `kyty_emulator` и тестовых целей:**
   - Команда: `cmake --build /Users/tx/Developer/KyTx/_Build/macos-arm64 --target kyty_emulator shader_cfg_tests shader_optimizer_tests shader_recompiler_compute_tests resource_materialization_tests resource_tracking_tests image_page_table_tests -j$(sysctl -n hw.logicalcpu)`
   - Результат: `EXIT=0`.
   - Проверка `strings -a _Build/macos-arm64/kyty_emulator | grep -c KYTY_RECT_LEGACY_FACING` -> **1** (≥ 1).

4. **Аудит архивного лога `play1-063459.log.gz`:**
   - Команда: `gunzip -c .porting/artifacts/2026-11-08-play1/play1-063459.log.gz > /tmp/play1.log && python3 .porting/work/nan_audit.py /tmp/play1.log --top 4 --bufwrite && python3 .porting/work/lighting_counts.py /tmp/play1.log`
   - Результат полностью совпал с таблицей 4.2 хендоффа:
     - `flip 14339`: 1843 passes, 714 bad; `n=1750 draw c88c6b9b RT3/load1 0x8240160000 1920x1080 f92 nan=1697379 inf=2242`; `n=278 cs 1691cb86 OUT 0x824c120000 1920x1080 f92 nan=158001 inf=1675 grp=0x1x1` (сырые записи в `0x824c120000`: `[(258, '98ad4fa4', 8294400), (320, 'ddfb6d46', 130023424), (321, 'ddfb6d46', 130023424), (383, '98ad4fa4', 8294400)]`); `n=2195 draw 3741f71a RT0/load1 0x8240020000 1920x1080 f92 nan=1381087 inf=997`.
     - `flip 14444`: 2352 passes, 12 bad; `n=4076 draw 3741f71a RT1/load1 0x824c120000 f92 nan=424 inf=2`; `n=4205 draw 30a69d9e RT0/load1 0x8249540000 960x540 f92 nan=14495`.
     - `flip 15233`: 1532 passes, 130 bad; `n=775 cs 1691cb86 OUT 0x8248c40000 1920x1080 f92 nan=90824 inf=2023 grp=0x1x1` (сырые записи в `0x8248c40000`: `[(751, '98ad4fa4', 8294400), (872, '98ad4fa4', 8294400)]`); `n=5751 draw 3741f71a RT0/load1 0x8242da0000 f92 nan=3266 inf=49`.
     - `flip 15863`: 2545 passes, 60 bad; `n=1000 cs 10a008ed OUT 0x8242190000 1920x1080 f92 nan=114395 inf=238 grp=0x1x1`; `n=7357 draw 3741f71a RT0/load1 0x8247490000 f92 nan=5364295 inf=16`.
     - `lighting_counts.py`: в классах 4–6 ненулевые группы (например flip 14339 block 0: `[10944, 0, 0, 7040, 9408, 4352]`).

5. **Базовый прогон всех тестов из раздела 2 (на текущем дереве ДО наших правок):**

| Тест | Статус | Подробности |
|------|--------|-------------|
| `tools/test_metal_msl.sh` | **PASS** (`EXIT=0`) | Все GPU-тесты MSL (LDS, bit scans, trig, float cmp, PackFloat2x16Rtz, Indirect RECTLIST, RECTLIST packed payload, formatted stores и др.) прошли |
| `tools/test_metal_pipeline.sh` | **FAIL** (`EXIT=1`) | Ошибка линковки standalone-скрипта: `Undefined symbols: Libs::Graphics::Metal::PassTimerItem()`, `PassTimerAttachRender`, `PassTimerComputeDescriptor` в `metalDraw.o` (скрипт не передаёт `metalPassTimer.mm` или стаб) |
| `tools/test_metal_sampler.sh` | **PASS** (`EXIT=0`) | `Metal sampler GPU tests passed` |
| `tools/test_metal_buffer.sh` | **FAIL** (`EXIT=1`) | Ошибка линковки standalone-скрипта: `Undefined symbols: Libs::LibKernel::Memory::GetGuestBackingAlias / TryGetGuestBackingOffset / GetGuestBackingGeneration` в `metalBuffer.o` (из-за чужих незакоммиченных правок в `metalBuffer.mm` и `kernel/memory.cpp`) |
| `tools/test_metal_texture.sh` | **PASS** (`EXIT=0`) | `Compressed linear texture path: 4 cases, 0 failures` |
| `tools/test_metal_tiling.sh` | **PASS** (`EXIT=0`) | `tiling: 58 cases, 0 failures; guest mip decode: 8 chains, 0 cumulative failures` |
| `tools/test_metal_shader_cache.sh` | **PASS** (`EXIT=0`) | `Legacy/Specialized native function cache ... 0 failures` |
| `shader_cfg_tests` | **FAIL** (`EXIT=134`) | `ShaderCfgTests: failed: repeated exports duplicated Position or its clipping-error plane` |
| `shader_optimizer_tests` | **PASS** (`EXIT=0`) | `ShaderOptimizerTests: ok` |
| `shader_recompiler_compute_tests` | **FAIL** (`EXIT=134`) | Host-тесты прошли, падение на `VulkanHarness failed at dispatch: vkCreateInstance returned ErrorIncompatibleDriver` (нет Vulkan ICD в окружении) |
| `resource_materialization_tests` | **PASS** (`EXIT=0`) | `ResourceMaterializationTests: all cases passed` |
| `resource_tracking_tests` | **PASS** (`EXIT=0`) | `resource tracking tests passed` |
| `image_page_table_tests` | **PASS** (`EXIT=0`) | `ImagePageTableTests: all cases passed` |

**Статус WP0:** WP0 готов. Проверено `git diff --stat`: ни `shader_cfg_tests`, ни файлы shader CFG не затронуты (`shader_cfg_tests` — «красный и до наших правок»; `pipeline`/`buffer` — линковки standalone-скриптов; `compute_tests` — нет Vulkan ICD). Пользователь подтвердил переход дальше.

---

## Шаг A (оффлайн-разбор `/tmp/play1.log`: кадры 14339 и 15233, поверхности `0x824c120000` и `0x8248c40000`)

### 1) Проходы, пишущие поверхность как образ (`OUT`) до TAA (`gps=0x3741f71ae437da64`), и число групп

В каждом окне трассировки (`begin frame flip=N`) записаны два подкадра (`sub`), в каждом — блок из 8 классов косвенных проходов освещения (`loc=8x8x1 | INDIRECT`). Первый выход освещения (`0x8244580000` в 14339, `0x82455f0000` в 15233) привязан во **всех** 8 классах; второй выход освещения (`0x824c120000` в 14339, `0x8248c40000` в 15233) привязан как `OUT` **только в 2 из 8 классов** (класс 2 и класс 5):

- **Кадр 14339, поверхность `0x824c120000` (`@0x7740e43880`, `via=lastwriter`)**:
  - Подкадр 1 (`sub=224023`, до TAA `draw n=2195`): привязана как `OUT` в 12 проходах (`n=278..283` и `n=296..301`), из них **4 прохода с `grp > 0`** (сумма **2816 групп** = `180 224` текселя из `2 073 600`, т.е. `8.7%` экрана) и **8 проходов с `grp = 0`**:
    - `n=278` `1691cb86` (`grp=0`), `n=279` `45ff7d9d` (`grp=0`), `n=280` `661481cd` (`grp=0`), `n=281` `77b2d6ce` (`grp=0`), `n=282` `a4c06b45` (`grp=0`), **`n=283` `28e424b7` (`grp=192`)** (`nan=158001` без изменений);
    - **`n=296` `15ef8445` (`grp=960`)** (`nan: 158001 -> 157705`, `-296`), `n=297` `2168dc94` (`grp=0`), `n=298` `ff98d6cd` (`grp=0`), **`n=299` `5dcd923b` (`grp=256`)** (`nan=157705`), `n=300` `055a9d8d` (`grp=0`), **`n=301` `1f8e335b` (`grp=1408`)** (`nan: 157705 -> 157410`, `-295`).
  - Подкадр 2 (`sub=224045`, до TAA `draw n=2993`): те же 12 проходов (`n=403..408`, `n=421..426`), из них **4 с `grp > 0`** (сумма **2752 группы** = `8.5%` экрана): **`n=408` `28e424b7` (`grp=128`)**, **`n=421` `15ef8445` (`grp=896`, `nan: 158486 -> 158116`)**, **`n=424` `5dcd923b` (`grp=256`)**, **`n=426` `1f8e335b` (`grp=1472`, `nan: 158116 -> 157735`)**; остальные 8 — `grp=0`.
  - Поверхность `0x8248c40000` в кадре 14339 не используется.

- **Кадр 15233, поверхность `0x8248c40000` (`@0x773ce4bbc0`, `via=lastwriter`)**:
  - Подкадр 1 (`sub=242674`, до TAA `draw n=5751`): привязана как `OUT` в 16 проходах (`n=775..782` и `n=799..806`), из них **только 1 проход с `grp > 0`** и **15 проходов с `grp = 0`**:
    - `n=775..782` (`1691cb86`, `45ff7d9d`, `661481cd`, `8ea0c09c`, `77b2d6ce`, `a4c06b45`, `28e424b7`, `d208a8ea`) — все `grp=0`;
    - `n=799..805` (`15ef8445`, `2168dc94`, `ff98d6cd`, `c7a03c4b`, `5dcd923b`, `055a9d8d`, `1f8e335b`) — все `grp=0`;
    - **`n=806` `ab9b6255` (`grp=3712`)** (`237 568` текселей = `11.5%` экрана; `nan=90824` не меняется).
  - Подкадр 2 (`sub=242694`, до TAA `draw n=6396`): те же 16 проходов (`n=896..903`, `n=920..927`), из них **1 с `grp > 0`** (**`n=927` `ab9b6255` (`grp=3712`)**, `nan=91178`) и 15 с `grp=0`.
  - Поверхность `0x824c120000` в кадре 15233 не используется.

### 2) Число NaN-текселей vs число записей `98ad4fa4` (проверка гипотезы «остатки списка»)

Каждая запись append-ядра `98ad4fa4` (`cs=0xa0878743459f76a4`, `loc=64x1x1`, до 2 мест записи на поток в MSL: строки 4933 и 5617) имеет размер **16 байт = 4 текселя `f92`** (`uint4`, 4×32-бит dword):

| Кадр / подкадр | Поверхность | Групп `98ad4fa4` (`потоков = grp×64`) | Макс. записей `98ad4fa4` (1..2 на поток) | Эквивалент в текселях `f92` (`4 × записей`) | Факт `nan` / `inf` в поверхности (до и после освещения) | Шапка `nan_row0` / `nan_top8` / `nan_bbox` |
|---|---|---|---|---|---|---|
| 14339 sub 1 (`n=258`) | `0x824c120000` | `192` (`12 288` потоков) | `12 288 .. 24 576` | `49 152 .. 98 304` (первые `25..51` строк) | `158 001 -> 157 410` (`inf=1675`) | `row0=65`, `top8=651`, `bbox=(0,0)-(1919,1069)` |
| 14339 sub 2 (`n=383`) | `0x824c120000` | `160` (`10 240` потоков) | `10 240 .. 20 480` | `40 960 .. 81 920` (первые `21..42` строк) | `158 486 -> 157 735` (`inf=2375->2373`) | `row0=88` (`+23`), `top8=672` (`+21`), `first=(11,0)` |
| 15233 sub 1 (`n=751`) | `0x8248c40000` | `2880` (`184 320` потоков) | `184 320 .. 368 640` | `737 280 .. 1 474 560` (до `768` строк) | `90 824` (`inf=2023`) | `row0=123`, `top8=1442`, `bbox=(0,0)-(1919,1079)` |
| 15233 sub 2 (`n=872`) | `0x8248c40000` | `2880` (`184 320` потоков) | `184 320 .. 368 640` | `737 280 .. 1 474 560` (до `768` строк) | `91 178` (`+354`, `inf=1973`) | `row0=323` (`+200`), `top8=1795` (`+353`), `first=(74,0)` |

- **Вывод по соотношению:** один 16-байтовый элемент списка даёт 4 dword-текселя `f92`, но в `RG11B10Float` `NaN` возникает только когда 5 бит экспоненты R или G или B равны `0x1f` с ненулевой мантиссой (~7.5–12% упакованных dword-ов списка). Поэтому:
  - В `15233` при `2880` группах (`184 320` потоков, `~737 тыс. .. 1.47 млн` текселей списка) `90 824 .. 91 178` NaN-текселей (~8–12% от `4 × число_записей`) + мгновенный скачок `nan_row0: 123 -> 323` и `nan_top8: 1442 -> 1795` сразу после второго вызова `98ad4fa4` (`n=872`) **прямо подтверждают**, что `98ad4fa4` пишет свой список поверх памяти, которую `0x8248c40000` (`@0x773ce4bbc0`) видит напрямую.
  - В `14339` сам `98ad4fa4` (`grp=192` и `160`, до `49..98 тыс.` текселей в верхних строках) меняет шапку (`nan_row0: 65 -> 88`, `first_nan: (69,0) -> (11,0)`), а хвост `bbox` до `y=1069` (`~158 тыс.` NaN = 7.7% от всей поверхности) остаётся от предыдущих более крупных списков `98ad4fa4` и/или `ddfb6d46` (`BUFWRITE addr=0x824c120000 size=130023424`), так как 91.3% плиток `0x824c120000` не перезаписывается освещением. (Для сравнения: в чистом кадре `14026` `98ad4fa4` пишет список в `0x824b560000` — это **первый** выход освещения, который затем класс 0 перезаписывает на всех `32 448` группах = 100% экрана, поэтому там `nan=0`.)

### 3) Классы освещения с 0 групп в кадрах 14339 и 15233

По 8 косвенным таблицам аргументов (`args@...`) внутри каждого прохода освещения:
- **Кадр 14339 (`sub=224023` / `sub=224045`, по 50 dispatch на подкадр)**:
  - Полностью с `0` групп (**3 из 8 классов**):
    - **Класс 3** (`args@0x824001c800..60`, `n=284..289` / `409..414`, `daa8c31f..10129471`, пишет только 1-й выход `0x8244580000`): `[0, 0, 0, 0, 0, 0]`.
    - **Класс 6** (`args@0x8240040000..60`, `n=302..307` / `427..432`, `cc985cd0..bcd6caf1`, пишет только 1-й выход `0x8244580000`): `[0, 0, 0, 0, 0, 0]`.
    - **Класс 7** (`args@0x824813c000..60`, `n=308..313` / `433..438`, `9a3f27ce..eeb62f7c`, пишет только 1-й выход `0x8244580000`): `[0, 0, 0, 0, 0, 0]`.
  - Частично с `0` групп на **втором выходе `0x824c120000`**:
    - **Класс 2** (`args@0x8240013800..60`, `n=278..283` / `403..408`): 5 из 6 шейдеров = `0` (`[0, 0, 0, 0, 0, 192]` / `[0, 0, 0, 0, 0, 128]`).
    - **Класс 5** (`args@0x8240040c00..60`, `n=296..301` / `421..426`): 3 из 6 шейдеров = `0` (`[960, 0, 0, 256, 0, 1408]` / `[896, 0, 0, 256, 0, 1472]`).
- **Кадр 15233 (`sub=242674` / `sub=242694`, по 66 dispatch на подкадр)**:
  - Полностью с `0` групп (**3 из 8 основных классов** + хвост в sub 1):
    - **Класс 1** (`args@0x8240001c00..70`, `n=767..774` / `888..895`, `699d695a..98c04bd1`, 1-й выход `0x82455f0000`): все 8 шейдеров = `0`.
    - **Класс 2** (`args@0x82400e0800..70`, `n=775..782` / `896..903`, `1691cb86..d208a8ea`, **пишет 2-й выход `0x8248c40000`**): **все 8 шейдеров = `0`**.
    - **Класс 3** (`args@0x8240018000..70`, `n=783..790` / `904..911`, `daa8c31f..5ad4f734`, 1-й выход `0x82455f0000`): все 8 шейдеров = `0`.
    - **Хвост** (`args@0x8240019400..10`, `n=823..824` в sub 1): `[0, 0]` (в sub 2 `n=944..945`: `[0, 64]`).
  - В **Классе 5** (`args@0x8240018800..70`, `n=799..806` / `920..927`, **пишет 2-й выход `0x8248c40000`**): 7 из 8 шейдеров = `0` (`[0, 0, 0, 0, 0, 0, 0, 3712]`). В остальных классах 1-го выхода активен только 1 шейдер из 8 (Класс 0: `[0]*7 + [32000]`, Класс 4: `[0]*7 + [3264/3328]`, Класс 6: `[0]*7 + [512]`, Класс 7: `[0]*7 + [128/64]`).

**Итог Шага A:** Поверхности `0x824c120000` (14339) и `0x8248c40000` (15233) пишутся как `OUT` только классами освещения 2 и 5 на 8.5–11.5% экрана (`2816`/`2752` и `3712` групп из `32400`; в 15233 весь класс 2 = 0 групп и 7/8 класса 5 = 0 групп), поэтому в остальных ~90% тайлов остаются неперезаписанные биты буфера объединения (`98ad4fa4` / `ddfb6d46`), дающие ~8–12% `NaN` на тексель списка (`90.8k..158.5k` NaN), которые напрямую читает TAA (`slot64`).

---

## Шаг B (запуск `r1`, Antigravity 18:02-18:17) и передача работы Claude (19:12)

- `idle_repro.sh r1 15 90`: лог `fex-spike/runs/validation-20261025/lean/r1-180207.log`, `.porting/work/out/r1/audit.txt` (84 снимка), 9 трасс (флипы 4017 ... 18019), холодный кэш шейдеров, экран «Press Any Button» (3D-сцена, чёрное небо).
- **Репродукция WP1 удалась автономно.** История TAA (`3741f71a`, RT0/RT1 f92, 0x824b570000 / 0x824b240000) содержит NaN **3.4-4.07 млн** в флипах 7794, 9389, 13051, 16499, 18019 и не очищается. На снимке `shot-00884.png` (кадр 18148) справа от игрока виден маленький синий неоновый блик.
- **Раньше всего NaN входит через RT3 draw `c88c6b9b`** (`0x8241260000`, f92, `load=1`, NaN уже в первом draw пасса, дальше убывает по мере перекрытия): флип 5909 = 50152, 9389 = 15759, 11162 = 10598, 13051 = 117, 14849 = 0. То есть это A1a (содержимое адреса от другого ресурса), а не только второй выход освещения.
- Критерий `max>30000` в `nan_audit.py` даёт ложные срабатывания (флип 14849: 858 «bad» без NaN): для приёмки смотреть только `nan`/`inf`.
- Сессия Antigravity с 18:22 до 19:11 получала `API error: stream idle timeout (5m0s)` (7 попыток подряд), работа передана Claude. Исходники Antigravity не менял.
- Claude нашёл старый opt-in **`KYTY_RT_PRIVATE=1`** (`PrivateRenderTargets()` ~1177, ветка ~4272; заметки `porting-handoff-fc27-metal-2026-11-01-claude.md`, строки 37 и 43: «у Metal draw, покрывающий 13% тайлов HDR, оставляет в остальных 82% NaN чужого ресурса, у эталона 0.2%»). В хендоффе его не было. Запущен A/B `r2` (те же 15 минут, `EXTRA_ENV="KYTY_RT_PRIVATE=1"`), результат ниже.

## A/B `r2` = `KYTY_RT_PRIVATE=1` (Claude, 19:15-19:31; лог `lean/r2-191517.log`, `out/r2/audit.txt`)

- **Результат: не лечит и не годится как включение по умолчанию.** TAA RT0 по-прежнему NaN: 3.77 млн (флип 4150, 1640x920), 4.58 млн (флип 4537, 1784x1008), 208 тыс. (4786). RT3 `c88c6b9b` почти чист (NaN только 1796 во флипе 3035; у `r1` 117-50152), то есть A1a частично снят, A1b нет.
- **Цена:** память 39 ГБ против 14.6-20 ГБ у `r1`, FPS 12 против 26-27, за 15 минут 5036 кадров против 18148. Игра сама ушла в демо-матч (MUN-LIV), поэтому часть кадров другого разрешения (динамическое: 1640x920, 1784x1008).
- **Механизм источника NaN в `r2` (кадр 4150, поверхность 0x8244000000, f92 1640x920):** один и тот же адрес и размер за кадр используется как **f53 (R32Uint, n=714, `91c4e2e9`) -> f55 (R32F, n=716/721, значения до 0.84) -> f92 (n=726..749, второй выход освещения, `via=lastwriter`)**. На n=726 (0 групп, ничего не писал) образ f92 уже содержит 88069 NaN: это биты R32F/R32Uint, прочитанные как RG11B10 (`ReinterpretFormat`: тот же размер текселя = 4 байта). Освещение дописывает только свои плитки (n=729: 24960 групп, NaN 88069 -> 437, потом 111 -> 9), остальное остаётся мусором.
- Это **та же семья, что и список `98ad4fa4`** (память транзитного пула читается другим форматом), но в `r2` без участия сырой записи. Вывод: источник не «сырая запись», а **повторное использование транзитного адреса с другим форматом того же размера текселя, при которой Metal отдаёт старые биты (`via=lastwriter` + `ReinterpretFormat`), а эталон (Vulkan) создаёт пустой образ**.
- Не решено: чем это отличается от PS5 (там тоже те же байты). Гипотеза для проверки: на RDNA чтение RG11B10 с экспонентой 31 даёт не NaN, либо игра не читает такие тексели (маска классов). Без этого любое «обнуление» = поведение эталона (Vulkan), а не железа.
- **Следующий шаг (код, ещё не сделан):** под переключателем (например `KYTY_STORAGE_REINTERPRET_EMPTY=1`) в привязке storage-образа (`via=lastwriter`, около строк 9560-9620 `metalRenderer.mm`) не брать `ReinterpretFormat` старого писателя другого формата как содержимое, а создавать пустой образ нового формата (как делает ветка `gpu_image`/`AliasedImageFor`). Риск: ядра, которые намеренно пакуют R32Uint в RG11B10 (`ReinterpretFormat`, комментарий «keep the same-texel-size reinterpretation for intentional packing»). A/B: `r1` против `r3` по `nan_audit.py` (TAA RT0/RT1 и `RT3`) и скриншотам.

## `r3`/`r4` (правило «тот же формат» для storage-образов) и первая правка кода (Claude, 20:00-20:30)

- `r3` (4 мин 44 с) и `r4` (4 мин, трасса каждые 30 с, 7 трасс) с `KYTY_STORAGE_LASTWRITER_SAME_FMT=1`: **второй выход освещения (f92, `1691cb86`/`10a008ed`...) больше не источник NaN ни в одном из 9 трассированных кадров** (в `r1`/`r2` был в каждом). Скриншоты `r4` (меню с Real Madrid, уличная сцена) без неона, без белых и чёрных блоков, 24-30 FPS.
- Механизм (подтверждён арифметикой): R32F/R32Uint-биты, прочитанные как RG11B10, дают NaN/inf в ~6% текселей (экспонента 31 у R или G): 88069+404 из 1 508 000 = 5.9% в `r2`. Это и были «NaN во всех плитках, которые освещение не перезаписало».
- **Правка (не закоммичена):** `metalRenderer.mm`, привязка storage-образа (`lastwriter`, около строки 9499): по умолчанию НЕ брать вид писателя другого формата того же размера текселя; `KYTY_STORAGE_LASTWRITER_ANY_FMT=1` возвращает старое поведение. Старый `KYTY_STORAGE_LASTWRITER_SAME_FMT` теперь ничего не делает (это и есть умолчание). Сборка `kyty_emulator`: успех.
- **`RT3` `c88c6b9b` (`load=1`, NaN в первом draw) — ложная тревога для TAA:** в кадре 2069 NaN в `RT3` падает с 16435 до 0 к концу пасса (draws перекрывают весь экран), а во втором подкадре снова стартует с 16375: значит NaN приходит от промежуточного пользователя этой памяти, но готовый `RT3` чист. Критерий приёмки по `RT3` отменить: смотреть NaN в ИСТОРИИ TAA и во входах TAA.
- Остаются: R16F-входы целиком из NaN (`05e3ea8d` slot75, `72892782` slot76, `80739e1e` slot70, T# `tile=9`, `tfmt=13`; в `r1` тоже: 2 062 080 NaN из 2 073 600), NaN в bloom (`30a69d9e` RT0, 15-17 тыс.), NaN в истории TAA (`3741f71a` RT1: 21 тыс. - 1.76 млн).

## Итог 20:00-21:00 (Claude): правки, измерения, инструменты

**Правки кода (не закоммичены, `git diff` в `metalRenderer.mm`, `graphicsRun.cpp`):**
1. `metalRenderer.mm` привязка storage-образа: по умолчанию НЕ брать вид писателя другого формата того же размера текселя ни в `lastwriter`, ни в `csreg` (`KYTY_STORAGE_LASTWRITER_ANY_FMT=1` возвращает старое). Убрало цепочку «второй выход освещения -> TAA» (`1691cb86` и др.).
2. `metalRenderer.mm` выбор load action: HDR-цель (f92/f115) транзитного пула при первом использовании объекта (адрес+размер), если на адресе нет писателя той же раскладки, грузится с очисткой один раз (`[RtFirstUse]`, `KYTY_RT_FIRST_USE_KEEP=1` отключает). NaN в истории TAA в первом кадре: 2.38 млн (`r7`) -> 9.8 тыс. (`r9`). Полностью цепочка не ушла.
3. `graphicsRun.cpp`: семь `std::getenv` на горячем пути обработки PM4 (каждый пакет/draw/DMA) заменены на `static const bool`. Доля getenv в потоке командного процессора 4.6% -> 0.56%.

**Инструменты (`.porting/work/`):** `nan_chains.py` (цепочки NaN, классы ignore/EMU/CHECK, `--known`), правило: NaN игры игнорируем, эмуляторный прослеживаем (память `feedback_kytx-nan-tracing-rule.md`).

**Остаток (по `r9`):** EMU: история TAA (`3741f71a`, 9.8 тыс.), bloom (`30a69d9e`), R16F-входы целиком из NaN (`05e3ea8d`, `72892782`, tile=9). CHECK: `30a69d9e`, `c88c6b9b`. Не решено: `c29caf0c` делает `0 * (1/0)` (вход ядра нулевой, какой именно - неизвестно; `KYTY_MSL_VALUE_TRACE`), игра или эмулятор.

**Производительность (измерено):** GPU-профиль `r10` (доли): cs `3ad95c14` 7.9%, ps `dec48a13` 7.0%, cs `5d0d6f16` 5.3%, ... `c29caf0c` 3.0%. В кадре ~1800 draw и ~230 compute, 30% compute с 0 групп. CPU-профиль `r11`: поток командного процессора занят ~67%, Draw 48%, DispatchCompute 40%, привязка ресурсов 18%, разбор SRT 19%. Не трогать: `MaterializeCompiledSrtExperiment` (включённый по умолчанию ускоренный путь). Кандидат: опрос времени гостем (`clock_gettime` через `_mach_boottime_usec`).

## Мини-тесты стоимости переводов (Claude, 21:00-21:40) - `tools/bench/`

Запуск: `tools/bench/run_metal_encode_cost.sh`, `tools/bench/run_metal_rt_layout_cost.sh` (игра не нужна, M5 Max).

**Стоимость compute-примитивов (2000 независимых dispatch, `metal_encode_cost.mm`):**
- CPU на запись одного dispatch: 0.15-0.25 мкс (один энкодер на всё или по энкодеру на запуск), 1.6 мкс с цепочкой fence'ов. Наш поток командного процессора тратит ~11 мкс на операцию (22 мс/кадр на ~2000 draw+dispatch): **издержки в нашей логике перевода (SRT, привязка, кэш пайплайнов), а не в Metal.**
- GPU, мелкие запуски (4 группы): по энкодеру + fence-цепочка (как сейчас) 14.1 мс против 3.5 мс без fence (+5 мкс на энкодер), один concurrent-энкодер без барьеров 0.25 мс, барьер раз в 8 запусков 0.68 мс. Крупные (1024 групп): 18.7 / 9.0 / 6.1 / 6.5 мс.
- Вывод: fence-цепочка (`metalOrder.h`) - самый дорогой способ упорядочения; параллельный dispatch с барьером по группам даёт до 1.5x на крупных и на порядок на мелких ядрах.

**Раскладка цели рисования (1080p, GPU мс на проход, `metal_rt_layout_cost.mm`):** RG11B10: приватная 0.015 / линейная на буфере 0.014 (разницы нет); RGBA16F: 0.013 / 0.020 (+50%); RGBA8: 0.013 / 0.016. **Линейные цели над гостевой памятью НЕ узкое место** (<1 мс на кадр даже при сотне полноэкранных проходов).

**`KYTY_CS_OVERLAP` (код в `metalOrder.h`, `metalDraw.*`, `metalRenderer.mm`, по умолчанию выключен):** на титульной сцене (игра сама держит 30 FPS) фазы ON/OFF внутри одного прогона дали одинаковые 29-30 FPS (`r16`); одно падение `runtimeLinker.cpp:790` в `r13` с ON (не повторилось в `r14`, `r16`). Выигрыш нужно мерить на сцене, где FPS ниже предела (матч: 24-25 FPS, GPU ~29 мс). Сравнивать FPS разных прогонов нельзя: сцена режима ожидания разная.

## ПОПРАВКА по производительности (Claude, 22:00): слой Metal frame capture

Лаунчер `KyTxMetalS1.app/Contents/MacOS/launch` жёстко ставил `MTL_CAPTURE_ENABLED=1` (в продукте `main.cpp` включает его только при `KYTY_GPU_CAPTURE=1`). Слой оборачивает все объекты Metal: 26.6% потока командного процессора в `CaptureMTL*` (мой разбор `r12`), в HUD строка «GPU Frame Capture Enabled». Агент замеров (`PERF-frame-timeline-2026-11-08.md`): с слоем CP 21.8 мс/кадр (65% ядра), без него 5.3 мс (16%); процесс 81.7 против 45.4 мс CPU на кадр. **Все прежние цифры производительности в этом журнале (CP 67%, ~11 мкс на операцию, GPU 20-29 мс, оценки по fence'ам, разбор `DESIGN-draw-plan-cache`, `r10`-`r16`) сняты с включённым слоем и завышены.** Запускатель теперь по умолчанию `MTL_CAPTURE_ENABLED=0` (резерв: `/tmp/launch.before-capture-off.bak`).
Остаются верными: механизмы NaN, правки по NaN, мини-тесты `tools/bench/` (не зависят от игры), вывод что FEX не на критическом пути титульной сцены. Нужно заново замерить: цену операции CP, GPU-время, пользу `KYTY_CS_OVERLAP` - в матче, с выключенным слоем.

## Сессии `play2`/`play3` (матч пользователя, захват Metal выключен) - Claude, 22:30-00:30

**FPS/frametime (HUD по 47 снимкам `play2`):** в матче 25 FPS при интервале 39.9 мс (чередование 33/50 мс: кадр ~35-40 мс не влезает в 33.3 мс, средняя скачет 24-27), пики 60-108 мс, GPU 25-47 мс (пики 64-77). В меню после титульной сцены 21 FPS при GPU 21 мс из интервала 47.7 мс. Metal память растёт 22 -> 44 ГБ за сессию (App 31 ГБ): подозрение на утечку/раздутые кэши, не исследовано. Слой захвата не главная причина (в матче без него те же 25 FPS).

**Искажения кадра:** `play2` (674-777 с, 942 с): разноцветный шум, фиолетово-красный экран, чёрно-белая мозаика. `play3`: с ~1141 с до конца сессии (1340 с) весь 3D-кадр = чёрно-белая мозаика (панели UI целые). Ловушка `anomaly_watch.py` (+ `anomaly_score.py`) поймала и включила трассу флипа 14783 (1263 с). Ложные тревоги: Match Preview (синие полосы), заставки.

**Найдено по трассе 14783 (`nan_chains.py`, `nan_audit.py`):**
1. Пирамида яркости (экспозиция): ядро `af85c98e` (n=398) пишет 15x8 f92 по 0x8240000000 как `via=alias` (нули), следующее `25eaf253` (n=399) читает тот же адрес/размер как **`via=guest`** (устаревшая гостевая память: max 57344, 8 NaN) и порождает NaN дальше по уровням (372, 1125 ...) -> `de02a62a` 960x540 NaN 356k -> TAA история 3.3 млн NaN. В прежних прогонах (`r1`/`r6`/`r9`) вход этого ядра всегда `rt`/`alias`/`csreg`, NaN ни разу; в `play3` 10 из 30 проходов `guest` и все с NaN. Причина: реестр образов один на АДРЕС, запись другого размера на том же адресе (или зачистка) прячет только что записанный уровень, чтение падает в декод гостевой памяти.
2. Постоянное состояние: текстура 256x8 f115 (`cbaa642d`, читают `2c33ee13`, `c34319af`) целиком NaN (8192 из 8192) уже на входе кадра: «носитель» NaN, начало до окна трассы (не найдено).

**Правка (не закоммичена):** `metalRenderer.mm`: `RecentAliasedImage(addr,w,h,fmt)` + вставка перед гостевым фоллбэком compute-чтения: образ с точным ключом (адрес, размер, формат), использованный в ТЕКУЩЕМ кадре, берётся вместо декода гостевой памяти (`via=alias`, лог `[RecentAlias]`, отключение `KYTY_CS_NO_RECENT_ALIAS=1`). Проверка: `play4` (26 минут без игрока, демо-матчи) с ловушкой.

## Автопроход в матч и бисект блоков освещения (Claude, 00:30-01:30)

- Скрипт `.porting/work/drive_to_match.sh <tag> [KEY=VAL ...]`: один прогон, сам доводит игру до матча Kick Off (j каждые 6 с на титуле и диалогах EA, потом 5 раз «вниз» с FC NEWS на KICK OFF, затем j на «Enter Kick Off», втором диалоге EA, Play, Start Match, Ok про сложность), скриншот каждые 10 с, останавливает свой PID через 600 с. Курсор KICK OFF между запусками НЕ запоминается. Первая компиляция шейдеров матча ~3 минуты с замёрзшей картинкой (кэш потом тёплый). Матч достигается примерно на 160-180 с.
- Бисект блоков освещения на траве (чёрные и светлые прямоугольные заплатки по ~64 px, пересвеченные майки на трибунах): `b1` = `KYTY_STORAGE_LASTWRITER_ANY_FMT=1 KYTY_RT_FIRST_USE_KEEP=1` (обе мои правки выключены): блоки на месте -> **не от моих правок**. Пользователь: это «кривые тени и освещение, сырые». Задача A7 (тени/освещение). Гипотеза (не проверена): границы блоков совпадают с сеткой 30x17 ячеек 64x64 ядра отсечения света `1af7217f`.
- Мозаика на весь кадр (play2 777 с, play3 с ~1141 с) и разноцветный шум (play2 674-695 с) в автоматическом матче (без игрока, `b1` и `play5`) не воспроизводились за 10 мин: в сессиях пользователя события другие (угловой с подсказкой, повторы, стандартные положения). Правка `RecentAlias` в сборке, проверять на игре пользователя (`play6` с ловушкой).
- В `b1` на 420-480 с ряд тёмных кадров (почти чёрное поле, повтор/сцена с очень низкой экспозицией) - не разобрано.


## Продолжение Codex — 2026-11-09: physical backing вместо защиты TAA

Найдена сессия Claude `52cdae17-1a05-40ea-912d-91012d435db7`. Пользователь уточнил: заглушки/обнуление NaN в TAA недопустимы. Временный sanitizer был добавлен и GPU-проверен, но **полностью откачен** после этого уточнения: metalShader.mm/.h, pipelineCache.cpp, metalMslTests.mm восстановлены побайтно из before-копий, новый заголовок удалён. Бинарь пересобран, s3 обновлён. `ct1` остановлен, не считается проверкой исправления. Старое разрешение Claude на TAA больше не действует.

Синтетический remap: две страницы общего backing, один гостевой vaddr переназначается с A на B после завершения GPU. CPU читает `2468ace0` (B), старая Metal-обёртка — `13579bdf` (A), canonical physical buffer+offset — B. Это реальный дефект идентичности mapping, но игровая причинность ещё не установлена.

Итоговая правка в существующих исходниках ограничена своими hunks metalRenderer.mm: GpuSurfaceFor и buffer-backed color RT используют GetBufferForGuestRange; кэш сравнивает buffer/offset/stride/format/extent; stale exact writer забывается; mapping generation разрывает same_pass shortcut. Новые metalGuestMemoryView.h, GPU regression, test runner и synthetic benchmark. Прежние dirty hunks сохранены, коммитов/публикаций нет.

Проверки: test_metal_guest_memory_view.sh PASS (GPU read/write после remap, CPU alias и сохранность старой страницы, cache identity и bounds). test_metal_tiling.sh PASS: 58 layout cases, 8 mip chains, дальнейшие mip-tail/padding checks тоже прошли; эталон — tile.cpp, не железо PS5. Сборка kyty_emulator PASS.

Benchmark M5 Max 1920x1080, GPU медианы 7 после 2 warmup, без эмулятора: Standard64KB 4 B/element import/export/copy 0.3149/0.3019/0.0490 ms; RT64KB 4 B 0.1340/0.1283/0.0211; RT64KB 8 B 0.5118/0.4915/0.0976. 257 sampled elements каждого направления точны. Это не FPS-выигрыш.

Открыто: CpOpAcquireMem пропускает payload; Metal EmitGlobalBarrier return не управляет coherence, но есть Order fences. Много реестров по vaddr/flip, AliasConvert работает через scratch, полноценного physical-range owner/version tracker нет. Исправление двух видов НЕ завершает remap-инвалидацию всех sampled/mip/layered/metadata caches. Нужны отдельный tracker и явные PM4 scope/visibility transitions, без глобального CPU wait на каждый пакет. Дизайн: .porting/DESIGN-memory-apple-2026-11-09.md. Ledger CLI/store отсутствуют.

Финальный smoke cm2 (3 минуты, сборка с mapping generation, без sanitizer) дошёл до матча; три PassTrace, штатная остановка по таймеру, GPU Fault/progress timeout не найдено. На финальном локальном снимке крупные блоки теней/освещения сохраняются. NaN/Inf-аудит также не чист, причинность packed bits нужно разбирать отдельно. FPS-выигрыш не установлен. Собственные процессы остановлены, extra_env_s1.sh восстановлен из before; main и s3 побайтно совпадают.

Автовозобновление исходной сессии Claude в 03:10 Asia/Tashkent НЕ отменено: Computer Use запретил управление Terminal по правилам безопасности. Пользователю сообщено нажать Esc в исходной вкладке; обход через другую технологию не предпринимался.


## Аудит полноты и первый пакет — Codex, 2026-11-09

Новая команда пользователя: сначала выявлять отсутствующие/слабые реализации и реализовывать по зависимостям, без заглушек. Каталог и очередь: `.porting/AUDIT-emulation-coverage-2026-11-09.md`. Слово stub в FEX ABI bridge не является доказательством функции-пустышки; TODO/return false не подсчитываются как процент готовности. Ledger отсутствует.

Сделано: MSL descriptor stride swizzle (index groups 8/16/32/64, offset/scalar/immediate, vector components independently translated, raw/formatted stores, subword loads/atomics share addressing). До правки BufferAddressable отвергал swizzle; before regression FAIL на unsupported. После: 4806 GPU results (4 index strides, odd/zero stride, raw/formatted vectors, subwords, atomic add, physical OOB) + full modified-buffer checks PASS, полный native MSL GPU suite PASS. NumRecords/record OOB не объявляются полностью доказанными — отдельный следующий набор.

Сделано: TileTables.in_z, GPU volume import/export Standard4KB3D/Standard64KB3D/Prt64KB3D для eb=1/2/4/8/16, mip offsets/tail/z slabs; DecodeTiledVolumeLevel; CPU-owned shared UploadTiledVolumeMipChain с проверкой всех уровней до изменения target. Настоящий GuestTexture3D выделяет mip chain и загружает все tiled mips (включая сохранённый packed-widen путь). GPU конвертеры доступны библиотеке; автоматическое переключение владельцев/живых GPU representations — следующий coherence этап. Большие bank tables используют MTLBuffer вместо setBytes >4 КиБ, lifetime до completion.

Tiling suite PASS: 30 volumes, 133 GPU mip round-trips, full allocation/padding/guards; full CPU snapshot upload и проверка parent/native view, включая RGB10A2/RG11B10; incomplete snapshots/private destinations rejected. Прежние 58 2D layout cases и 8 mip chains PASS. Эталон адресации — tile.cpp, не независимое железо Sony. Build/diff-check PASS.

Отдельное изменение ключа s_tex3d на полный layout исключено из итогового пакета: aliases GPU-written памяти должны согласовываться physical-range tracker, иначе разные objects сами по себе не обеспечивают видимость. Сохранена прежняя политика владения 3D-кэшем. Динамическая смена layout/mip count/CPU overwrite ещё требует coherence; linear 3D CPU upload тоже открыт.

Игровая проверка: ec1 без GPU fault/timeout, но финальный снимок сильно мозаичный; ec2 после исключения отдельной cache-policy правки дошёл до видимого демо-матча с исходными блоками освещения, штатно остановлен. Пользователь прямо уточнил: эта мозаика/блоки были ДО наших 3D изменений и остаются сейчас. НЕ приписывать их новым функциям и не объявлять изменение cache key доказанной причиной. Эти прогоны не establish FPS gain/visual fix; проверки семантики — отдельные GPU suites.

Следующая очередь: acquire/release + canonical physical range owner/version/dirty subranges; затем DCC/CMASK/HTILE и metadata eliminate/decompress; затем sampler compare/PCF/border и shader interface omissions; далее ARM64 fiber/guest-thread-exit и HLE codecs. Коммитов/публикаций/новой защиты TAA нет; прежние dirty hunks сохранены.


## PM4 и ядро coherence — 2026-11-09, продолжение Codex

Текущая команда пользователя: завершать части эмуляции и сначала проверять их мини-программами; игру запускать позже. **В этом этапе игру не запускали.** Известные мозаика/блоки были до наших 3D изменений по уточнению пользователя; не считать их нашей регрессией без доказательства. Никаких NaN/TAA-заглушек.

В production добавлены декодирование acquire (legacy/GFX10/AGC), dispatch аппаратного RELEASE_MEM и GPU execution boundary Metal, закрывающий overlap group без CPU wait/submit. Ядро MemoryCoherence хранит физические диапазоны/owners/versions и предотвращает stale read publication/dirty eviction; **ещё не подключено ко всем runtime texture caches/CPU writes/remap. Этап полной coherence НЕ закрыт.** Следующая работа — runtime adapters, включая raw buffers и backing chunks; затем metadata DCC/CMASK/HTILE.

PM4 266240 GCR combinations PASS; byte-oracle 20000 operations PASS и ASan/UBSan PASS; GPU CPU→GPU→другой tiled GPU→CPU, physical aliases/partial writes/padding/completion label PASS, Metal API Validation PASS. Два реальных overlap producers + barrier + consumer PASS; no-op barrier negative control FAIL ожидаемо. Две разные no-copy обёртки физического alias показали stale GPU bytes, общий canonical backing PASS — не путать ordering с physical identity. Прежние views/tiling/MSL suites PASS; build kyty_emulator PASS. Общий запуск только мини-программ: `bash tools/test_emulation_memory_stage.sh`.

Подробности/границы: `.porting/REPORT-memory-core-2026-11-09.md`. Production EOP/interrupt end-to-end, все texture adapters и game validation ещё не проверены этим набором. Новая сборка находится в `_Build/macos-arm64/kyty_emulator`; игровые launch slots/extra_env этим этапом не менялись. Коммитов нет, старые dirty hunks сохранены. Ledger CLI/store отсутствуют.



## Runtime memory и запуск игры — 2026-11-09, Codex

Новая команда пользователя: «продолжай и в конце запускай». Автономные проверки выполнены ДО запуска. Игра запущена 02:55:07 Asia/Tashkent и **оставлена работающей**, PID **97150**, slot s3, tag `rm1-20261109-025507`. Не запускать второй эмулятор; перед новой сборкой/запуском учитывать уже открытый процесс. Новая сборка SHA256 `8023ff7020033d9aeca3380533c208578d7c0cbeb9d42248a4725672577613e7`; extra_env_s1.sh побайтно восстановлен.

Закрыты два рабочих дефекта: AliasConvert теперь берёт настоящую гостевую базу вместо zero-fill, сохраняет padding/reader-only bytes и обновляется между draw/dispatch через command epoch; все его staging blits получают Order fences. Обычные linear GuestTexture views используют canonical buffer manager и проверяют физическую identity/offset/extent при reuse/remap. Helper EncodeTileAlias доступен production и мини-тестам.

28 layout/format pairs: 497233 writer / 2189447 guest-only reader bytes, полный scratch и texels, guards, invalid ranges, повторная конверсия после CPU byte update PASS; старый zero-fill negative control FAIL ожидаемо. Metal API Validation PASS; remap/span view test PASS; полный memory stage (CPU/PM4/tiling/MSL) PASS; build/diff-check PASS. Игра дошла до 3D-персонажа, кадр5785 просмотрен. По журналу выполняются Surface conversions, GPU fault/timeout не найдено. После EA servers unavailable нажат штатный confirm; больше выборы меню не делались.

**Полная coherence не завершена:** глобальный owner/version tracker ещё не подключён ко всем runtime owners и CPU dirty/remap events. Не приписывать старые игровые артефакты нашим изменениям; они сохраняются, визуальный A/B/полный матч не проверены, FPS gain не заявлен. Новых NaN/TAA-заглушек/skip shader нет, но прежние unsupported/fallback paths в эмуляторе остаются и видны в логе. Далее — согласование private writer footprint с raw guest writes, runtime 3D/metadata и оставшаяся очередь аудита.

Подробности `.porting/REPORT-runtime-memory-2026-11-09.md`. Лог игры `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/rm1-20261109-025507-025507.log`. Коммитов/публикаций нет, старые dirty hunks сохранены, Ledger отсутствует.



## Хендофф для продолжения эмуляции — 2026-11-09

**Точка входа: `.porting/HANDOFF-codex-2026-11-09-emulation.md`.** Пользователь попросил записать хендофф следующему агенту. Реализацию в этом этапе не продолжали, исходники/бинарные сборки не меняли, игру не перезапускали.

При проверке 03:09 Asia/Tashkent эмуляторов `kyty_emulator* --game` не найдено; прежний PID97150 отсутствует. Ранее игру оставили пользователю. В этапе хендоффа её не останавливали; причина/код завершения неизвестны. Прежние строки «игра оставлена работающей» описывают момент прошлого отчёта и не являются текущим статусом процесса. Main/s3 SHA256 остаётся `8023ff7020033d9aeca3380533c208578d7c0cbeb9d42248a4725672577613e7`.

Следующая работа: production adapter physical backing/subranges/owner/version, raw↔texture transitions, CPU dirty/remap/lifetime и PM4 visibility/completion. Ядро MemoryCoherence и мини-тесты готовы; интеграция всех runtime caches НЕ завершена. Затем DCC/CMASK/HTILE и остальные пункты очереди. Каждый этап сначала проверять мини-программами, после — игра; запуск в конце уже разрешён пользователем. Не возвращать TAA/NaN-заглушки, не приписывать исходные артефакты новым 3D изменениям. Предыдущие dirty/untracked files сохранить, коммитов нет, Ledger отсутствует.



## 2026-11-09 05:05 — сессия Antigravity («Fixing Emulation Handoff Code», c207ca88) откачена по просьбе пользователя
- Что откатано: все успешные `edit_file`/`write_file` этой сессии (она стартовала 03:15:52+05 после хендоффа Codex 03:09). Список восстановлен из её журнала
  `~/.gemini/antigravity/brain/c207ca88-b53f-49bf-a078-4f3e48f37163/.system_generated/logs/transcript_full.jsonl`; каждая правка применена в обратном порядке (Replacement→Target), конфликтов 0.
  Удалены целиком созданные ею `metalMemoryCoherence.h` (адаптер) и `metalSurfaceMetadata.h` (DCC/CMASK/HTILE-заготовка). Откачены хуки `NotifyGuestMemoryWrite`/`ReleaseMem`
  в memory.cpp/graphicsRun.cpp/pm4Handlers.cpp/commandProcessor.h/metalRenderer.h/.mm и переписанный ею `tests/metalCoherenceTests.mm`. Моя заплатка к её адаптеру (`g_submit_epoch`) убрана вместе с ней.
- Проверка: 54 файла исходников/тестов побайтно совпадают с хэшами `handoff-source-state.txt` Codex (03:13); `tools/test_emulation_memory_stage.sh` (с `MTL_DEBUG_LAYER=1`) EXIT=0; `kyty_emulator` собран (BUILD_EXIT=0).
- Бэкап того, что было до отката (в т.ч. оба удалённых файла): `.porting/work/backup-before-ag-revert-050235/`. Если когда-нибудь понадобится сравнить — там же.
- Причина решения: её адаптер сканировал память на CPU-записи прямо в PM4-потоке (93% CP-потока, 2 FPS в меню), её «этап 1 выполнен» проверялся 90-секундными прогонами до меню.
- Следующий шаг: этап 1 хендоффа Codex — физические owners/versions для переходов buffer↔texture, с нуля поверх `memoryCoherence.h` (без переноса кода Antigravity).


## 2026-11-09 05:14 — этап 1 хендоффа Codex: первые две продуктовые части (адаптер ещё не подключён к рендереру)
- Базовый прогон откатанной сборки `c4` (drive_to_match, меню→матч): матч идёт (кадр 8798, 26:42), 25–27 FPS, GPU 32 мс, холодный старт с компиляцией шейдеров как раньше; мозаики/неона в кадре нет. До отката было ~2–3 FPS в меню.
- Новое: `src/graphics/host_gpu/metal/metalSurfaceFootprint.h` — `TiledSurfaceFootprint`: точные байтовые диапазоны видимых элементов тайловой 2D-поверхности (целые блоки одним диапазоном, краевые поэлементно; O(блоки+край)). Тест `tests/metalSurfaceFootprintTests.mm`, `tools/test_surface_footprint.sh`: 290 раскладок/размеров против побайтного оракула на `tile.cpp` (617 млн байт владения, 41,8 млн байт padding), негативный контроль `KYTX_FOOTPRINT_TEST_WHOLE_BLOCKS=1` падает.
- Новое: `src/graphics/host_gpu/metal/metalSurfaceCoherence.h` — `SurfaceCoherence`: общий адаптер `MemoryCoherence` для приватных тайловых 2D-текстур (ReadSurface / WriteSurface / MaterializeRange / RecordGuestWrite / ForgetSurface; тайл import/export + блиты только владеемых подпрограниц; без CPU wait, актуальная поверхность не кодирует GPU-работ). `tests/metalCoherenceTests.mm` переведён на него (лямбды copy_to_guest/read_texture убраны), добавлены проверки «footprint адаптера == поэлементный оракул» и «повторное чтение актуальной поверхности без GPU-работы». В набор `tools/test_emulation_memory_stage.sh` добавлен surface_footprint; весь набор EXIT=0 под MTL_DEBUG_LAYER=1.
- НЕ сделано (критерий закрытия этапа 1 ещё не выполнен): подключение адаптера к рендереру (RecordComputeStorage / RT-проходы / AliasConvert / raw-буферы), обнаружение CPU-записей (события: PM4 WRITE_DATA/DMA, kernel TryWriteBacking/чтение файлов, запись CPU гостя: PageManager.Protect для native Metal сейчас no-op), identity через границы 64 МиБ chunk, EOP/label порядок. Решение по CPU-записям: НЕ сканировать память в PM4-потоке (так сделала Antigravity: 93% CP-потока); события + постраничный write-watch только для страниц, где владелец — приватная GPU-поверхность.


## 2026-11-09 05:50 — внешние проекты (KytyPS5, AnyPS5): исследование и первые переносы
- Полный разбор: `.porting/RESEARCH-upstream-anyps5-2026-11-09.md` (дельта с upstream 904 коммита, что сливается дёшево, список заглушек FC27 с именами, находки EA-тредов, аппаратный оракул AnyPS5).
- Код: `sceKernelSyncOnAddressWait8/16` (+тест `TestNarrowCells`), `scePthreadTestcancel`, POSIX `rmdir`, `sceKernelMlock`; PM4: игнор SH-регистров 0x0CA/0x14A (NHL 27, upstream a7491cc6);
  `metalClearDecode.h` с расширенным декодером CB_COLOR_CLEAR_WORD (UFC 6, upstream b2c9ec76) и тестом `tools/test_clear_decode.sh`; диагностика `KYTY_LOG_UNBOUND` для compute и `KYTY_LOG_ALIAS_STATS`.
- Тест `tests/metalMslDepthCompareTests.inc` (PCF + функция сравнения сэмплера) написан и ПАДАЕТ на текущем эмиттере (13 отказов из 360), поэтому включается только `KYTX_MSL_DEPTH_COMPARE_TEST=1`:
  эмиттер делает одиночное сравнение с фиксированным GEQUAL и без PCF; реализация ждёт данных `KYTY_LOG_CMP=1` о том, какие функции сравнения ставит игра.
- Клоны внешних проектов: `/Users/tx/Developer/external-ref/` (только чтение, 236 МБ).


## Продолжение Claude, реальные upstream-исправления — 2026-11-09, Codex

Актуальный отчёт: `.porting/REPORT-claude-continuation-2026-11-09.md`. Прочитана та же сессия Claude `52cdae17-1a05-40ea-912d-91012d435db7`, продолжена её последняя задача: осмысленно переносить реализации upstream с адаптацией под Metal/FEX, без заглушек. Сохранены его Wait8/16, clear decoder, footprint/SurfaceCoherence tests и остальные dirty изменения. Антигравити-сканы гостевой памяти в PM4 не возвращать.

Новый код: настоящий mlock/VirtualLock с проверкой гостевых диапазонов; исправлены libc NID/ABI и реализован strcpy_s; independent FILE handle coherence; настоящий fsync/POSIX export/directory handles; rmdir ошибки и mount-root/symlink protection; атомарный rename без удаления destination при ошибке; ES/LS RSRC1 сохраняются и декодируются вместо игнора; TLS stubs/SafeCall сохраняют FLAGS через LEA; noinline logging для guest fibers. Full GS/HS/float-mode backend и празднования этим пакетом не реализованы.

`bash tools/test_upstream_ports.sh` PASS: build main + port-contracts через FEX + реальное page wiring + SyncOnAddress + 12 executable TLS cases + полный memory/Metal/MSL stage. Old TLS SUB/ADD negative control FAIL ожидаемо. Полный старый default FS harness на ARM64 не заявлен прошедшим; Windows не проверен. `git diff --check`/reverse patch check PASS.

Cu2 завершил десятиминутный сценарий без остановки эмулятора. Матч Manchester United — Liverpool подтверждён несколькими кадрами (игровое время 02:52 → 20:02 → 39:51, финальный кадр13565); в просмотренных игровых кадрах Metal HUD примерно 22–27 FPS, с отдельными просадками. GPU fault/command-buffer errors/progress timeout/сигналов падения в проверенном журнале не найдено. На 2026-11-09T06:37:41.867289+05:00 игра оставлена работающей, PID22969; auto-input сценарий завершён, исходный extra_env_s1.sh побайтно восстановлен. Монитор исходного лимита40GB остаётся. Последний screenshot: `/Users/tx/Developer/KyTx/.porting/work/out/cu2/shot-0600.png`; доставленная копия `claude-continuation-match.png`. Лог: `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/cu2-062735.log`. Это smoke menu→match, не полный матч и не доказательство визуальной корректности.

Сборка main/s3 SHA256 `2d8593610d3a6352218a55229f31ecbf615d6b440d0798a70ca23077b06f7e56`. Cu1 остановил наш ошибочно заниженный 26GB guard перед матчем; это не GPU fault. Cu2 использует исходный лимит40GB Claude. Старые блоки освещения сохраняются и не приписываются новым 3D изменениям. Не заявлять визуальное исправление/FPS gain.

Далее: runtime owner/version/CPU-write/remap integration без PM4 memory scans; shader resource/binding перенос вместе с MSL; DCC/CMASK/HTILE; настоящий unbound 64x64 RGBA32UInt tile5; неизвестный импорт WxLcN76q19o; depth compare/PCF (opt-in тест Claude 13/360 FAIL); GS/HS. Не переносить выдуманный entitlement-state вместо реализации. Полного merge 904 upstream commits нет. Коммитов/публикаций нет, Ledger CLI/store отсутствуют. Focused patch и отчёт доставлены в Codex workspace `outputs/claude-continuation-*`.



## Актуальный этап — 2026-11-09 19:37 Asia/Tashkent

Исправлены два настоящих пропуска native Metal raster state: signed viewport `(x,y+h,w,-h)` и полный общий с Vulkan расчёт scissor для indexed/automatic draws. До исправления GPU mini-tests дают 5 328 / 222 879 ошибок, после — 0 / 0 (36 viewport cases, 256 scissor cases; 140 832 + 1 048 576 pixel checks). Product build PASS. Новые тесты подключены к tools/test_emulation_memory_stage.sh; прежний полный suite не повторён, выполнены непосредственно затронутые GPU tests.

В реальном D16 shadow atlas3072×3072 подтверждены отдельные scissor64/512/1024 на разных началах atlas. В наблюдаемых shadow draws высота viewport отрицательная и D16 negbits=-16, поэтому viewport fix и предполагаемая масштабировка depth bias не объясняют оставшиеся тени. Старые чёрное небо, шлейфы и shadow/lighting артефакты ещё видны. Не объявлять их исправленными, не добавлять sanitizers/skips/dummies. Следующий шаг: actual VS/PS depth output и HTILE/ownership сравнение; same-input compute18 kernels уже совпадали.

**Игра sn3 PID54466 оставлена работающей**, запуск19:35:59+05:00, main/s3 SHA8286f3a33a07823b86107cb252281cc1073644738b1c601aa329a35723bff74f. Log /Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sn3-193558.log. Один эмулятор, исходный40GB guard, no timerkill/input loop (один j:6 закрыл наблюдаемый EA Servers dialog), capture/validation off, optional deduplicated raster log on; extra_env восстановлен. На19:37:48 лог0 GPU fault/command-buffer/progress-timeout/fatal/skipped-compute patterns; кадр3070 подтверждает The Grounds menu, не gameplay/fullmatch proof. Перед запуском проверить PID. Старые sh3 PID52447, sv1 PID52963 sn1 PID53497 и sn2 PID54116 остановлены нами по проверенным PID, это исторические процессы.

Отчёт: .porting/REPORT-shadow-raster-2026-11-09.md. Work/tests/negative controls в /Users/tx/Documents/Codex/2026-11-09/new-chat/work/shadow-fix. No commits/pushes, dirty baseline сохранён, Ledger отсутствует.



## Продолжение исправления реальных теней — 2026-11-09 19:54 Asia/Tashkent

Пользователь повторно подтвердил, что видимые артефакты НЕ исправлены, и попросил посмотреть скриншот/продолжать. Снятый user-current.png (work/shadow-fix) показывает RMA studio: светлый пол без ожидаемой оконной/игровой тени. Не выдавать предыдущие viewport/scissor tests за исправление картинки.

Текущие изменения: LayeredImageEnsure в PrepareLayeredImages теперь импортирует реальный guest tiled snapshot всех слоёв/mips при создании/growth, вместо необоснованной пустой sampled array. Новый UploadTiledArrayMipChain в metalTiling, atomic reject before writes. 27 synthetic arrays, 5013 GPU kernel reads+raw subresource readbacks, 27 incomplete-snapshot negatives PASS; blank-control5013 failures. Existing mip18/volume30/roundtrip133/tiling58/chain8 PASS. Реальный sa1 показал import128×128×64 RG16Unorm 4 177 716 ненулевых байтов; R16Float128×128×64 source действительно0, поэтому его старый нулевой dump сам по себе не доказывает неверное содержимое именно этой texture.

Также найден и исправлен DB_DEPTH_VIEW.Z_READ_ONLY: ResolveGuestDepthControl/GuestDepthWritesEnabled в metalPipeline.h теперь общие для actual Metal DS state и depth-feedback lookup в DrawAuto/DrawIndex. Native раньше игнорировал depth_write_disable; Vulkan его учитывает. GPU negative8 corruptions; corrected128 D16/D32 cases0 failures. Full metal_pipeline test including MRT/stencil/indirect/multi-draw PASS with validation. Runner linked actual metalPassTimer.mm to repair inherited missing dependency. Product build PASS. Tests source formatted; no new sanitizers/skips/dummy production resources. Need further review initial array import vs GPU ownership/CPU refresh; full layered depth views remain unimplemented (DepthDepthView slice_start/max ignored by native allocation/pass compatibility). New RasterState logs slice and ro flags to establish actual game usage.

**Сейчас единственный экземпляр sa2 PID55452**, started19:53:49+05:00, main/s3 SHAe708af5415418923fef6ec73bac2c16a5a692d108897e829b5d4646206a22d3b, log /Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sa2-195348.log. Original40GBguard/no timer kill, capture/validation off, no input loop, extra_env restored. sa1 PID54987 and sn3 PID54466 stopped by verified owned PIDs. sa1 got one j:6 to dismiss observed EA Servers dialog. Need observe sa2 screenshot before any input; then inspect ro=1 and slice logs, actual old artifacts. Work/backups/tests in /Users/tx/Documents/Codex/2026-11-09/new-chat/work/shadow-array. Ledger missing; dirty baseline preserved, no commits/pushes.



## Реальные depth-array cascades — 2026-11-10, в работе

В sa2 runtime подтверждено: D16 2048×2048 shadow draws используют DB_DEPTH_VIEW slice1:1 и2:2. Native игнорировал slices и сводил три каскада к одной2D texture; pass compatibility тоже игнорировал depth view. Это конкретный пропуск реального shadow path, не очередная гипотеза viewport.

Реализован MetalAttachmentArray (новый header): private depth/stencil arrays, growth сGPUcopy старых layers, отдельные2D/rangedarray views, first-use clears per layer. Renderer различает depth slices/extents вpass key, пишет выбранные слои, хранит authoritative fullarray writer; graphics иcompute читают реальные выбранныеdepth layers. Static depth writer больше не исчезает по возрасту2frames, sampled maps обновляют used_flip дляeviction. HTILE clear обрабатывает все allocatedlayers, depthclear_draw толькоselectedview. RenderTargetArrayLength прокинут вMetalDraw; GPUtest проверяет2layer instanced draw иview1:2. Stencil arrays сохраняются/очищаются отдельно, STENCIL_READ_ONLY теперь блокируетexplicitclear, какVulkan.

Tests: old forced-layer0 control640 corrupted texels; исправленные depth/stencil growth/clears/views/layered draw2560 GPU texels0fail. ReadOnlyDepth128cases0fail; full MRT/stencil/indirect/multidraw test withMetalvalidation0fail. Earlier array import27arrays/5013GPUreads+27atomicnegative andtiling/mips/volume PASS. New sampled array initialdata materializes all guest layers/mips fromtiledsnapshot. PrepareLayeredImages now fails explicitly if requested materialization fails, instead of later creating a blank substitute. It bypasses CPU-array creation for authoritative GPU depth-array writers. This last boundary change builtPASS; still needs finalgame run.

**Current live sd1 PID58106**, start2026-11-10T00:01:46.804756+05:00, SHA8066484025e7ad9da78cc8efb2a0ea37aea212b247b1ef1e817a6b35421f4812, log /Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sd1-000145.log. Oneemu,40GBguard,no timerkill,noinputloop,capture/validationoff. RuntimeDepthLayer/DepthLayerRead prove sameparent includes distinct0/1/2 andread0:2. Parisstreet has staticshadows; Grounds stillblacksky. Studio after fix notyetobserved; cannotclaim alloldshadows fixed. sd1 gotonej:6 atobservedtitle (flip14057), no mainmenu confirmed; avoid inputspamming. Sa2PID55452 disappeared before sd1 launch, reason notestablished; don'tclaim we killed it. Nootheremus atsd1launch. Launch scriptfinalcandidate work/shadow-depth/launch_release.py tag sd2 addsKYTY_LOG_UNBOUND=1 to distinguish legitimate null T# from missing nonzeroresources. New main binary newerthansd1 (STENCIL_READ_ONLY clearguard, formatting, materializationerror boundary), builtPASS; do not overwrite running s3. Next: review/finish checks, stoponlyverifiedownsd1 thenlaunchsd2, observe actualscene/nonnullunbound and finalgame/screenshot. No commits/pushes, Ledgermissing, prior dirty changes preserved.

Sources/backups/logs work/shadow-array and work/shadow-depth; initialuser screenshot shadow-fix/user-current.png showsRMA studio withmissingfloor/windowshadow. Need focusedpatch/report and finalSTATE/HANDOVER update when currentstageverified. Limitations: full metadata decode/alias CPUversioning, mippeddepth, general mixed-color/depth multilayer views stillincomplete; no NaN/TAA sanitizers/skips/newdummyproductionresources.



## PS activation + mesh fixed depth/stencil + LDS loop predicate — 2026-11-10 08:22 +05

NativePixelShaderActive теперь следует реальным color masks/formats и DB kill/Z/mask/dual export/EXEC_ON_NOOP, а не просто ненулевому адресу PS. Прежняя отсутствующая locn0 пара guestNGG4ad8b17ce657eb68/PS7facde60b1bc62d4 оказалась depth-only проходом с выключенными PS inputs. В ML4 она реально выполнила 2048x2048 layers и3072x3072 atlas с ps=0, zwrite=1. Для Metal mesh, который на этом GPU не допускает nil fragment при rasterization, отдельный kyty_db_fixed_function сохраняет rasterized depth/stencil без shader exports/resources. Это НЕ fallback для активного неподдержанного PS: missing active function/color writes явно reject. Удалён адресный s_mip_requests: SRD не увеличивает будущую CB MAX_MIP allocation.

ML4 после550translated/0unsupported в последней печатной coverage остановился на PS62b558eacebed7a3/f33aa6d0f69eb2d1: DS_ADDTID stores0/256/512 reservation2*128dwords. Reader mask Phi(p,Phi(p,self)) эквивалентен writer p. В MslPixelLaneMemory добавлена только нормализация trivial boolean Phi; ownership/dominance/reservation/predicate proof сохранены. Shared/undefined/changing masks reject.

PASS: full pipeline(API+GPU validation),24vertex/mesh GPU activation cases/1536depth+stencil+128color pixels/UAV counts/64routes/2negatives; full coherence; full MSL(APIvalidation), LDS16GPU/negative cases2048texels; final narrow LDS structured+dispatcher(API+GPUvalidation). Build/diff checkPASS. СтарыйAppleclipguardCFGfailure не исправлен/не повторялся.

Последний ML5 PID9723 started08:20:27+05 SHA d2784ebd963ee403ec0be67c16c4ed87a93ab7029ecd0db896ba35b6c50715a0 log /Users/tx/Developer/fex-spike/runs/validation-20261025/lean/ml5-082026.log. После LDS fix запущен; CUA сообщает Mac locked, ожидается ручная разблокировка для проверки изображения. Перед CUA проверить PID, не auto-relaunch мёртвый app. ML4 до LDSabort показывал EA Servers/Let'sPlay нормально, после них чёрная загрузка; нет доказательства меню/матча или исправления старых теней. No timer kills/input loops/signals; исходный40GBguard, env restored. Ledger отсутствует; dirty tree retained, no staging/commit/push/reset. Подробно .porting/REPORT-metal-ps-activation-and-lds-2026-11-10.md; snapshots/logs/patch в workspace work/metal-mesh-link. Ни нулевого varying, ни primitive_id substitution не добавлялось.
