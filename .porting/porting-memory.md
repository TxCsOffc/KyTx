## Актуальное продолжение — 2026-10-28: ci3/ci4, чёрный экран не исправлен

Сначала читать [ci3/ci4 follow-up](/Users/tx/Developer/KyTx/.porting/porting-followup-fc27-metal-2026-10-28-ci3-ci4.md). ci3 доказал indirect instanceCount3562971914; добавлен GPU safeguard какCPU (tests3fail→0). Bounded replay сохранённогоproducer доказывает, что cap256 может обрывать validcopyиоставлятьargsмусором; вернутьобычный4096. Отдельноразрешённыйci4доflip3200/240sбезwatchdog, instance178валиден, **картинка всёещёчёрная/белоечёрныеблоки**.17pixel+5boundaryGPUtestsпроходят. CSBUF-afterвci4былнесинхронизированпослеSplit (WaitUntilIdle сnilCB=noop), теперьявныйqueuefence. Подготовленplainci5(one-framecaptureпослеLLDBsetup) иmutex-safecapturecode; сборкаготова, **новогоOKнаci5ещёнет**. Игры/LLDBостановлены, activeenvci4. БезVulkan/Rosetta;dirtyсохранён,коммитовнет;30+gameplayнеподтверждены.

## Актуальное продолжение Codex — 2026-10-28: hang в draw, исправлен indirect draw

Сначала читать [MDI follow-up](/Users/tx/Developer/KyTx/.porting/porting-followup-fc27-metal-2026-10-28-mdi.md). Один разрешённый codex_ci2 завершён watchdog: отдельный CB с draw_idx3266 VS4b2fb528/PS74014bc2, idx1. **idx1 может быть placeholder MDI, это не доказательство пустого direct draw.** Сохранены251MSL/все4подозрительных VS/PS. Исправлены CPU-чтение GPU count до native multi routing, CPU args в single indirect, GPU bounds indexed args, потеря остальных MDI draws при split, misleading empty-draw skip/labels. Production GPU tests + MSL regression/build; runtime этих новых изменений ещё не было. codex_ci3 с GPU argument trace только подготовлен, новое разрешение запрошено. Процессы игры/replayer остановлены. Меню/матч30+FPS не подтверждены; dirty changes сохранены.

## Продолжение Codex — 2026-10-28: compute-indirect исправлен, hang воспроизведён без UMA

Читать [follow-up](/Users/tx/Developer/KyTx/.porting/porting-followup-fc27-metal-2026-10-28-dispatch-indirect.md). Native compute-indirect использует текущие CS/resources/local size и GPU snapshot args (7GPU cases/0fail, MSL regression и build проходят). Один явно разрешённый codex_ci1 (wave64 ON/UMA OFF, run1/20GB/240s/watchdog8s) завершён watchdog на flip775 с тем же пакетом idx1×9/idx456, как uk5/uk8. **UMA не обязательна для этого hang.** MSL не сохранились из-за отсутствовавшего каталога: запись исправлена и отдельно проверена, новая сборка проходит. codex_ci2 с split draw+compute от500 только подготовлен; отдельное разрешение запрошено, ответа ещё нет. Эмуляторы остановлены; правильное меню/матч30+FPS не подтверждены. Все dirty изменения сохранены, коммитов нет.

## Актуальный handoff — 2026-10-27 00:42

Читать [новый handoff](/Users/tx/Developer/KyTx/.porting/porting-handoff-fc27-metal-2026-10-27.md). Старую Claude-сессию пользователь остановил и подтвердил продолжение здесь; конфликт разрешён. На передачу все эмуляторы/LLDB/gpudebug остановлены, Xcode frame2300 открыт с Replay остановленным. Сохранены3 части RECTLIST fix: GPU3→4, TriangleStrip, packed constant bits. GPU suite/API+shader validation и build проходят. acc1 с final binary дошёл до flip9675 без logged GPU errors, но зелёные блоки/шум остаются; правильного матча/30+FPS нет. Пользователь попросил только handoff, работа остановлена. Ниже исторические состояния.

## Координация сессий — 2026-10-27,00:31

Старая Claude-сессия a890ad9a автоматически возобновилась после лимита и через killmine.sh остановила финальный тест Codex rectpacked1 (последний flip1125; это не подтверждённый crash). Затем возобновила af6cef93351f3493a, запущен acc1. Codex больше ничего не запускает и ждёт выбора пользователя, какая сессия продолжает. **Сохранён полный source/test checkpoint с новыми RECTLIST/packed fixes: `/Users/tx/Developer/KyTx/.porting/artifacts/2026-10-26/hdr-resume/codex-checkpoint-0031/`**. Подробности и Xcode-доказательства в follow-up2026-10-27 ниже. Итоговая packed-сборка проверена GPU suite/API+shader validation и build; последняя runtime-проверка меню была прервана другой сессией.

## Актуальное продолжение — 2026-10-27, indirect RECTLIST

**Обновление:** Xcode нашёл и помог закрыть3 части RECTLIST: GPU count3→4, настоящий TriangleStrip (подтверждён в новом frame2300), побитное сохранение постоянных packed-атрибутов четвёртого угла (GPU old528/528bad→fixed0 во всех3 режимах). В Xcode CB18/API13272: args `[4,160,0,0]` / original `[3,160,0,0]`, все4 позиции корректны; packed-fix сделан после этой трассы. Native full GPU suite + API/shader validation и ARM64 build проходят. Завершающий rectpacked1 без capture ещё проверяется; смотреть актуальное состояние в follow-up.

Читать [.porting/porting-followup-fc27-metal-2026-10-27-rect-indirect.md](/Users/tx/Developer/KyTx/.porting/porting-followup-fc27-metal-2026-10-27-rect-indirect.md). Найдена сессия Claude со скриншота a890ad9a…; продолжена здесь. Исправлен nonindexed indirect RECTLIST (GPU args3→4, wrapper vertexStart, без CPU readback): old528/1024 bad pixels → fixed5 cases/0; полный MSL GPU suite с Metal API+shader validation и ARM64 build проходят. Один rectind1 дошёл до flip4475 без logged GPU errors, но зелёный/шумный фон меню остаётся. Одиночный frame2600.gputrace сохранён18GiB (rectcap1), эмулятор завершён; Xcode готовит Replay, проверить итог в follow-up. FEX + direct RDNA→MSL, один процесс через run1, без Vulkan/Rosetta; коммитов нет. Ledger CLI недоступен. Состояния ниже исторические, красный экран уже исправлен stencil-реализацией Claude26 октября.

# Current FC27 Metal state — 2026-10-25 (Claude update at top)

**Newest: read [stencil follow-up](porting-followup-fc27-metal-2026-10-25-stencil.md) FIRST.** Red screen root cause = no stencil in Metal (UI clip masks drawn as full-screen red). Stencil now implemented (Stencil8 attachment, AMD op translation ported from Vulkan depthRenderTarget.cpp, per-draw refs; `KYTY_NO_STENCIL=1` disables). Also fixed: USE_THREAD_DIMENSIONS in Metal DispatchDirect (`KYTY_CS_RAW_GROUPS=1` = old). Menu UI is correct; 3D scene backdrop still wrong. Game's dynamic resolution (1600x904) is driven by EOP timestamps taken from encode-time clock; `KYTY_EOP_CLOCK_FREEZE=1` forces full 1920x1080 (workaround). Working flags: `KYTY_EOP_CLOCK_FREEZE=1 KYTY_MSL_GDS=1 KYTY_AUTO_INPUT=1500:j:8,2500:j:8` + lean launcher flags. Vulkan reference build used ONLY as visual oracle. Refuted hypotheses (scissor, DMA clear, CMASK clear, fill size, GDS alone, FEX) are listed in the follow-up. Older sections below are historical; "red persists" statements there are superseded.

# Current FC27 Metal state — 2026-10-25

Read [UI mip-tail fix](porting-followup-fc27-metal-2026-10-25-ui-mip-tail.md) FIRST, then [GPU trace series follow-up](porting-followup-fc27-metal-2026-10-25-gputrace-series.md), [image follow-up](porting-followup-fc27-metal-2026-10-24-image-sampling.md), [17:38 handover](porting-handoff-fc27-metal-2026-10-24-1738.md) and [logo handover](porting-handoff-fc27-metal-2026-10-24.md). Logo diagnosis and limits remain preserved.

## Latest UI fix — overrides process notices below

- X restored in Press X to Skip, EA connecting/cancel, unavailable/OK and Let's Play. Xcode old API3457 source32x32 was blank because uploader ignored T#2-mip chain: level0 lives in tail64,0. Exact guest bytes decode old0 ->3696/4400 nonzero bytes. Tiled2D uploader now loads every declared mip via common TileSurfaceLayout; cache match includes mip count. No synthetic icon/color/text-position replacement.
- GPU validation58tiling +8mip chains/0fail, ARM64 build/diff check pass. New after-fix single-flip trace `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261025/ui-mip-tail/frame500.gputrace`16CB/189draw/142dispatch, CLI ready/fetch and Xcode Replay verified. CurrentAPI3727/CB15/render48, MTLTexture108 has2mips and correct alpha/X; ROI dark pixels0 ->494. Do not confuse with older same-named frame500.
- Fixed runtime021054 no logged GPU errors/PSO failures to stats2700; red persists after J/Let's Play. First CLI count1 capture crashed/disconnected0/1; repeated programmatic500 under LLDB succeeded. Emulator/LLDB/gpudebug ended; Xcode active on newAPI3727/texture108 with alpha enabled (~7.84GiB). Compact reproducible evidence `.porting/artifacts/2026-10-25/ui-mip-tail/`.
- Full intro/red3D/correct match30+FPS unresolved. Occasional truncated live EA text vs full PPM observed, cause unknown. Broader texture ownership/linear mips/BaseLevel view limits not declared fixed. Native FEX+RDNA→MSL only, no commits.

## Latest capture state — overrides older process notices below

- User wanted Xcode `.gputrace`, not PNG. Six separate one-flip captures500…1000 exist in `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261024/frames500-1000-193356/`. All6 have nonempty workload, GPU replayer ready and successful color0 PNG fetch/read; validation.json/logs saved. Capture control now accepts a sorted/unique comma-list; ARM64 build/diff check pass.
- First500…900 are run193433;1000 first file incomplete and preserved under failed/, retry run193916 supplies the valid frame1000. Do not treat1000 as the same gameplay run. Counts respectively CB19/18/22/23/24/22, draw185/422/752/814/715/864, dispatch128/193/370/286/405/267.
- Xcode CUA opens/replays current frame1000: Frame ready,22CB/286render/335compute/217blit/864draw/267dispatch; red Summary preview persists. Other five verified via CLI, not all via GUI. Xcode remains active on frame1000 (~16.38GiB); emulator/LLDB/gpudebug ended. No new graphics correctness/performance claim. Compact evidence `.porting/artifacts/2026-10-25/gputrace-series/`; Ledger CLI still unavailable.

## Boundaries

- FC27 PPSA34015 01.000.004. Native ARM64 **FEX + direct RDNA→MSL** only. No Rosetta/Vulkan renderer; Vulkan reference alone authorized, none run in this follow-up.
- Branch `metal-native-checkpoint`, HEAD `c30d5d81ab23`; all work uncommitted. Preserve dirty/untracked files.
- Goal incomplete: correct intro/icons/effects/scene, then a playable match measured at30+FPS. Red-image FPS is not gameplay evidence.

## Feature status

| Domain | Status | Evidence / limitation |
| --- | --- | --- |
| Native specialization cache | Implemented | Full MSL/stage/emission identity; real manager GPU old2fail → fixed0 |
| Pixel flat/no-perspective transport | Implemented |6GPU old5fail → fixed0, raw packed flags preserved |
| Full barycentric/per-vertex interpolation | Partial | Flat transport does not implement every interpolation instruction |
| Depth-only color state | Implemented |2GPU cases/0fail; specific game PSO rejection removed |
| Multiple color attachments | Partial |6RT/sparse/noRT0 GPU tests pass; new heavy MRT0/1 runs have no logged GPU errors, red persists; prior ReadFirstLane/loop-cap fixes retained |
| Metal samplers | Implemented | Standalone GPU tests pass |
| Array/3D sample and gather | Implemented for tested cases |13 GPU old9fail → fixed0; float layer/gradients/component; game PS no longer skipped |
| Tiled2D asset mip upload | Implemented for tested cases | Shared mip-tail/reversed offsets;58 tiling +8 mip-chain GPU tests pass; X restored in skip/EA buttons and confirmed by new Xcode trace |
| Native guest address loads | Partial | Mapping/page-table tests pass; lifecycle/perf audit pending |
| Formatted/subword/raw memory | Partial | Includes144 raw boundary cases; desc_format0 store unsupported |
| Structured MSL/phi | Partial | Control-flow GPU tests pass; fallback remains |
| FPCmpClass32 /64-bit shifts | Implemented |50class +27shift GPU cases/full suite pass |
| Vertex wave operations | Partial | ReadFirstLane GPU case passes; wave64 cross-half incomplete |
| LDS append/consume | Partial |6wave32 GPU cases, ungated from GDS; wave64 incomplete |
| Cube resources | Partial | Graphics enabled; compute opt-in |
| Surface conversion/format views | Partial | Tile and format GPU tests pass; ownership/mip/layer/3D/CPU-reuse incomplete |
| Compressed linear guard | Implemented | Compressed format excluded from linear alignment API |
| Encoder state/API bounds | Implemented | Conversion before state; failed PSO return; scissor/topology fixed |
| Storage image rights | Partial | RG11B10Float/RGBA16Unorm GPU tests; graphics writes use unswizzled base |
| LDS access | Partial | Bounds/predicates fixed; heavy validation instrumentation can exceed32KB |
| Transient buffer lifetime | Implemented | Fresh backing on rollover/reset; queued GPU old2fail → fixed0 |
| GDS | Partial | OFF; earlier opt-in regressed to green |
| Intro/icons/effects | Partial | Loading logo preserved; X in skip/EA/Let's Play restored and verified; full intro/heavy scene remain incorrect |
|30+ gameplay FPS | Not started | Correct match not reached |

## Evidence and watch list

- **2026-10-24 image follow-up:** float array indices, array/3D gradient samples and gather component fixed/tested. New game PS69484ce5ea9fbf78 native2b740773366a9312; heavy dropped_ps0 vs25. Runs181054/181658 show no logged GPU errors to last stats2575/2300; capture failed SIGBUS in first run, successful under LLDB in repeat, cause of first failure unknown.
- **Current valid traces:** mrt-current-red-one19CB/497draw/246dispatch; image-sampling-fixed-debug-one22CB/720draw/252dispatch. Both Xcode Replay verified. Former old mrt-fixed-one0CB remains unusable. Capture exactly1 frame.
- **Current red evidence:** after-image-fix API25390 cb18/re40 has6 vertices990000ff, UV(-65536,-65536), nativeVSbc0210100fe76732/PSae49c6a306380213. Real standalone GPU replay yields red153/0/0/153 on transparent target; background API25195 white/opaque vertices overwrite red target correctly. Four render cases0fail with raw/encoded AB. CPU origin and runtime ordering/scissor remain unresolved; do not assume LoadActionLoad is the root cause or hardcode a replacement color.
- **gpudebug binding ambiguity:** buf[0] may mean resident buffer rather than inline args. Validate2048-byte size. GPU VA107bf885470 is not a guest address for a CPU watchpoint.


- **Logo/UI fix to preserve:** MslEmitter BuildInterface respects forced/SPI flat and no-perspective. Packed flag0x00010003 was zeroed by smooth transport. PixelInterpolation GPU old6/5fail → fixed6/0 with API+shader validation. User confirms logo appeared; no matched after-fix pixel trace of the logo itself. Latest handover separates these evidence levels.
- **Valid UI trace** cache-intro-one: CB6/Render176/API7722 creates white bottom quad in Texture4; pixel(960,1030) RGBA≈(.972,.972,.972,0), blendOne/OneMinusSrcAlpha. GuestPS39ad955b656767cc oldnativea6d3f97a759c923f → newa4d8ebd8084f007c, param3flat. Connecting dialog, not EA intro; white quad still appears in late scenes.
- **Red vertex debug:** valid shared-fixed-one CB19/Render74/API21316: ab.buffers[2] starts0x990000ff; UNORM8 yields(1,0,0,.6); all6postVS red. PSO22 normal SourceAlpha/OneMinusSrcAlpha. Data/ownership must be checked in a current run. PS751942f534c2702e is old native variant of guest2ae1ba74c42662f1. CurrentVS2b83a227ac38238f/currentPSc74589ce64857cd4.
- **Historical MRT acceptance attempt (superseded by current runs above):** pass and both draw paths bind active slots0..7 with holes/per-slot masks/blend. DefaultON, diagnostic KYTY_MSL_MRT=0 restores RT0-only selection. GPU old19 incorrect components → fixed0; run17561 hangs after last loggedflip1075. Cause not established. Compare off/on without capture; bounded harness saved but NOT run.
- **Historical empty capture:** mrt-fixed-one CLI1/1,6.33GB, Xcode opens/replays but0CB/0draw/NoPreview. Not pixel-debug evidence. disable-unused-recording/backtrace flags were used; empty-workload cause unknown. Capture strictly one frame and verify workload/replay/scene.
- flat-fixed-heavy-one failed0/1/no file; oldframe800 corrupt. Valid old single-frame/lldb-present uses different IDs. Do not mix traces.
- Latest run validation-20261024/mrt-fixed-20261024-000625-17561.log:0PSO failures,10GPU error messages. Faulted render encoder null signposts. Preceding compute state1 means Completed, not attribution. Previousdepth-fixed run specificPSOerrors0; red remained.
- Missing native image derivatives arrays/3D: guestPS69484ce5ea9fbf78/native631f646248122252. Desc_format0 store guestCSe191ab754420b413/native030aa6d6844bc8c8 unsupported.
- Broad latest-writer/256KB vertex-snapshot experiments previously hung and were reverted. s_cs_written_by_addr also records graphics reads, not reliable last-writer ownership. Current CPU snapshots819..81a up to16KB; strided vertex extent warrants isolated work. Validate type/tile/rights/ownership before reuse.
- Whole heavy shader validation not established: large LDS instrumentation exceeds32KB. EOP ordering hypothesis/SYNC_FLUSH did not fix red. KYTY_DUMP_CS_IMGS may close encoder after state; intrusive unless repaired. PM4ffffffff/DCC/indirect assertions recur; do not disable asserts.

## Operational state

- Emulator/gpucapture/gpudebug/lldb absent; Xcode replay stopped. Last window is empty-workload mrt-fixed-one; open a valid trace next.
- CUA getApp launches apps. After compaction call cua.rewriteDocumentation. Full AX snapshots reset IDs; focus+paste filters; select an executed instruction before scope exit for variables.
- Launcher runs/KyTxDiagnostic.app, bundleorg.kytx.diagnostic, J=Cross. Unique timestamp/PID logs; pointer runs/validation-20261024/latest-diagnostic-log.txt. Current launcher enables fault/descriptor/MSL logging and dumps: not a performance baseline.
- Sandbox has no Metal device. GPU tests/process/capture need escalation; bash tools/test_metal_pipeline.sh approved. Final build/diff check pass. No A/B game run after handover request.
- Evidence .porting/artifacts/2026-10-24: GPU/build logs, runtimegz, important MSL, NOT-RUN harness; plus2026-10-23 earlier tests. Huge traces/PPMs remain external. Ledger CLI unavailable on this Mac.
