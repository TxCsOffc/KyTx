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

# Latest FC27 follow-up — 2026-10-25: UI mip-tail fix restores X icons

Read `.porting/porting-followup-fc27-metal-2026-10-25-ui-mip-tail.md` FIRST. Xcode oldAPI3457 traced missing button icon to blank32x32 input: actual T# declares2mips, level0 starts in shared mip tail64,0. Exact guest bytes were nonempty; old upload yielded all-zero RGBA. Native tiled2D upload now decodes full mip layout and loads all levels; cache checks mip count. GPU58 tiling +8 mip chains/0fail, ARM64 build/diff check pass. In-game X verified in Press X to Skip, Cancel/Offline, EA unavailable/OK, Let's Play. New single-flip capture `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261025/ui-mip-tail/frame500.gputrace`16CB/189draw/142dispatch, CLI GPU-ready/fetch + Xcode Replay confirmed; current API3727/CB15/render48 texture108 has2mips/correct alpha. Red scene/full intro/correct match30+FPS remain unresolved. First CLI capture0/1 crashed/disconnected; retry500 under LLDB succeeded. Emulator/LLDB/gpudebug stopped, Xcode active on new trace/icon. Artifacts/repro `.porting/artifacts/2026-10-25/ui-mip-tail/`. Native FEX+direct RDNA→MSL only, no commits, preserve dirty/untracked tree. Ledger CLI unavailable.

---

# Latest FC27 follow-up — 2026-10-25: six separate Xcode GPU traces

Read `.porting/porting-followup-fc27-metal-2026-10-25-gputrace-series.md` FIRST for current captures/process state; previous image/logo fixes remain preserved. User clarified snapshots meant `.gputrace`. Separate500/600/700/800/900/1000 captures all pass nonempty workload + GPU ready + color0 fetch/read. Directory `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261024/frames500-1000-193356/`;1000 is valid retry run193916 after incomplete first capture, others run193433. Xcode CUA replay1000 verified Frame ready22CB/864draw/267dispatch; Summary remains red. Capture comma-list support built successfully, diff check passes. No shader/rendering fix or FPS win claimed. Emulator/LLDB/gpudebug stopped; Xcode workload1000 left active. Artifacts `.porting/artifacts/2026-10-25/gputrace-series/`. Native FEX+direct RDNA→MSL only, no commits, preserve dirty tree; correct match30+FPS still incomplete. Ledger CLI unavailable.

---

# Latest FC27 follow-up — 2026-10-24: image sampling and current red evidence

Read `.porting/porting-followup-fc27-metal-2026-10-24-image-sampling.md` FIRST. Native FEX+direct RDNA→MSL only. Fixed float array layers, array/3D gradients and gather component; GPU13cases old9fail ->0, full native MSL suite/build pass. Heavy PS69484ce5ea9fbf78 now compiles as2b740773366a9312, dropped_ps0. MRT0/1 bounded heavy runs have no logged GPU errors; red persists. After-fix single-frame trace image-sampling-fixed-debug-one valid22CB/720draw/252dispatch, Xcode Replay works. API25390 red6vertex data990000ff + UV sentinel; standalone GPU probes reproduce red153/0/0/153, while first white/opaque background draw overwrites red correctly; raw/encoded AB both pass4render cases. CPU origin not established; no forced-clear/color replacement. Artifacts/repro saved. Emulator/LLDB/gpudebug stopped; Xcode last observed replay active atAPI25195 before CUA became unavailable. Goal incomplete/no correct match30+FPS; no commits, preserve dirty tree. Previous pending/empty-trace notices are historical.

---

# Latest FC27 handover — 2026-10-24

Read `.porting/porting-handoff-fc27-metal-2026-10-24.md` FIRST. Detailed logo/UI fix: Xcode cache-intro-one CB6/Render176/API7722 pixel(960,1030) reveals packed param3 flags lost through smooth interpolation; native emitter now respects forced/SPI flat and no-perspective. Actual GPU regression old6cases/5fail -> fixed6/0; user confirms loading logo appears, complete intro/icons not verified. Shader mapping39ad955b656767cc -> a4d8ebd8084f007c. Native cache/LDS/depth-only fixes retained; depth runtime PSO errors0, red persists. New MRT support implemented/tested (old19 bad pixel components -> fixed0), but runtime Hang after flip1075; MRT off/on A/B prepared NOT run, `KYTY_MSL_MRT=0` opt-out. New mrt-fixed-one trace has0CB/0draw in Xcode despite CLI1/1: unusable for pixel work. Red vertex debug: ab.buffers[2] begins0x990000ff, source color(1,0,0,.6); blend normal; PS751942 is old variant of guest2ae1ba74c42662f1. Processes emulator/gpucapture/gpudebug/lldb absent; Xcode replay stopped. Goal incomplete, FEX+RDNA→MSL only, no commits, preserve tree. Ledger CLI unavailable; older notices below are historical.

---

# Latest FC27 native Metal follow-up — 2026-10-23 23:50

Read `.porting/porting-followup-fc27-metal-2026-10-23-2350.md` first. Native shader cache keys now include full specialized MSL and failure identity (real manager GPU old2fail -> fixed0). Pixel flat/forced-flat/no-perspective fixed (6 GPU old5fail -> fixed0, packed flags preserved). Depth-only color/blend masking implemented in both draw paths; actual pipeline GPU test2cases/0fail, final ARM64 build passes. LDS append/consume ungated from GDS,6 GPU cases/full MSL validation suite pass. Xcode replay of cache-intro-one succeeds and localizes white rectangle to API7722/Render176, PS39ad955b656767cc; RGBA~(.972,.972,.972,0), blendOne/OneMinusSrcAlpha. Heavy scene still red after flat fix; no gameplay/FPS success. Failed post-flat heavy capture0/1 produced no trace. New depth-only runtime verification active; current unique log pointer `/Users/tx/Developer/fex-spike/runs/validation-20261023/latest-diagnostic-log.txt`. Xcode replay stopped; no gpudebug sessions. Preserve dirty tree/no commits. Ledger CLI unavailable. Older process/next-bug notices below are historical.

---

# Latest resumed follow-up: native ALU

FPCmpClass32 and three64-bit shifts implemented,50 class-mask and27 shift GPU cases plus full MSL suite pass under API+shader validation; ARM64 build passes. Newer compressed-linear guard found already present, preserved; texture test4/0. Game frame800 connecting UI,1000 still red; no gameplay/FPS claim. Process stopped. See class-shifts-* artifacts and resumed section in latest handover.

# Latest FC27 handover — 2026-10-23 15:12 Asia/Tashkent

Read `/Users/tx/Developer/KyTx/.porting/porting-handoff-fc27-metal-2026-10-23-1512.md` FIRST. Native ARM64 FEX + RDNA→MSL only. Goal incomplete, no correct match/30+ gameplay FPS. New valid single-present shared-fixed-one.gputrace replayed in Xcode: composite955520d8831f23b0 samples red Texture449; API21316 actually returns red from vertex color(1,0,0,0.6), VSb2c4c2e311c5bc76/PS751942f534c2702e. Prior raw buffer/LDS/storage image/scissor/topology fixes retained and tested. New transient-ring rollover/reset fix retained: queued-read GPU test old3cases/2failures -> fixed3/0. Vertex-range/256KB snapshot follow-up caused GPU Hang and separate compressed-linear-format assertion; experimental changes fully reverted, patch/log preserved. Latest final binary rebuilt from retained source; verify build-handoff-final.log and ring-final.log. Emulator/LLDB stopped; Xcode replay stopped. All work uncommitted; preserve dirty/untracked files. Older notes below include obsolete next-bug/process states. Ledger CLI unavailable in macOS session.

---

# Current FC27 / KyTx notice — 2026-10-23

Latest14:22+ follow-up: GPU validation attributed RG11B10Float WRITE violations to cube array kernels173f6bb015ca4194/70c1167216da9737. Enabled ShaderWrite for RG11B10Float after 2DArray+3D GPU read/write tests(64pixels each) pass under API+GPU validation. Those texture violations disappear on repeat; red/black visuals remain, no success/FPS claim. Compute PSOs now labelled CS+hash for validation/Xcode. Next concrete bug: uint4 buffer loads clamp base to final dword then read+1..3 OOB (CS4231266a78a83ef3 line118). Global GPU validation stopped by LDS64KB>32KB; runtime API-only afterfix reached800 thenstalled, explicitlystopped. See latest handoff and shader-validation-*.log artifacts.

Latest 14:15+ follow-up: Metal API validation caught scissor2048 exceeding pass1080, then unconditional Triangle PSO used with non-triangle draws. Fixed scissor intersection against actual attachment mip extents (including empty draw skips) in metalDraw, and topology-key/class selection in renderer/pipeline. Build/diff checks pass; validation run reached flip875 without Metal API errors, then known PM4 ffffffff. Frame800 remains red; no gameplay/FPS claim. Native emulator stopped. Valid old single-frame trace reopened in Xcode, at Replay screen. Full evidence in .porting/artifacts/2026-10-23/api-*.log.

The current macOS task is EA SPORTS FC 27 PPSA34015, native ARM64 FEX + RDNA→MSL Metal, not the historical Windows UFC task below. See `/Users/tx/Developer/KyTx/HANDOVER.md` and `.porting/porting-handoff-fc27-metal-2026-10-23.md`.

Valid single-present GPU trace (9.6 GiB) captured and replayed in Xcode through computer use: `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261023/single-frame/lldb-present.gputrace`. Captured after guest flip700, not exact1400. Overlay Texture2364 RGBA8 is red before composite; final present input Texture71 RGB10A2 already red. First red producer unresolved. Earlier frame800 trace corrupt. Graphics image conversions now precede pipeline/state setup; failed native draws return instead of reusing old pipeline. Mip-RT format views enabled; standalone GPU read/write checks pass (8 cases, Metal API validation enabled), main ARM64 build succeeds. Final game repeat under LLDB remains red at flip600/800/1000; no visual fix claimed. Current rendering goal remains incomplete. GDS OFF. No playable match or valid 30+ gameplay FPS measurement. Native EOP completion-label ordering remains untested. No commit made; working tree preserved. Emulator/debugger stopped, Xcode trace open with replay stopped. Full handoff records exact evidence and flags.

The following 2026-10-08 state is HISTORICAL and does not establish current FC27 rendering correctness or performance. The ledger CLI was unavailable in this macOS session.

Follow-up 2026-10-23: Xcode fragment debug of API9049 pixel(405,539) succeeds; sample and return are black, alpha0.215686. No direct red output in this pixel. The sampled-image registry also records graphics reads. Attempted broad latest-writer selection for compute caused GPU Hang; fully reverted, previous source rebuilt. Do not repeat without validating dimension/layout/storage-write usage. Emulator/debugger stopped; Xcode replay stopped.

---

# Historical Current State

## Architecture

UFC 5 is being developed on the `ufc5-v2` Kyty fork, which renders correctly; the only remaining
problem is frame rate (FACT-0005). The current path uses native 4K output and sparse BDA. Bounded
Function LDS, the CMask impossible-clear skip, compiled SRT and dirty-only BDA are now ALL default
on in code, with per-feature opt-outs (`KYTY_FUNCTION_LDS_BOUND=0`, `KYTY_CMASK_SKIP_IMPOSSIBLE`,
`KYTY_COMPILED_SRT=0`, `KYTY_BDA_SYNC=0`). Still record exact flags per run. The repository has
extensive uncommitted work; do not discard it.

## Working

- Paused-fight present rate is **~10.2 fps on a plain launch**, up from 6.28 (EXP-0009): compiled
  SRT + dirty BDA default-on took it to 9.26 (EXP-0012), barrier coalescing added ~10% (EXP-0013),
  and lazy submission a further +10-19% (EXP-0027). Live gameplay is at least as fast as paused
  (FACT-0014), so the paused fight is a CONSERVATIVE proxy.
- **MENUS now run 54-60 fps, up from 40-41** (EXP-0027). Lazy submission helps far more there than
  in the fight, because submission cost is per-submission regardless of the work it carries.
- Lazy submission is default-on (`KYTY_SUBMIT_DRAW_THRESHOLD=0` opts out, default 64 draws per
  submit). It cut queue submits 340 -> 46/epoch and foreground waits 52.73 -> 39.61 ms/epoch.
- Global-barrier coalescing is on by default (`KYTY_BARRIER_COALESCE=0` opts out). It cut
  `EmitGlobalBarrier` by 68% per dispatch and barrier resource records by 33% (EXP-0013).
- Bounded LDS already paid a large dividend (FACT-0009): epoch wall 736 -> 347 ms, waits 465 -> 33
  ms, detile 413.2 -> 4.33 ms/epoch. It was never a neutral result; the gain was absorbed by the
  next bottleneck.
- Compiled SRT and dirty-only BDA are default-on and validated: 27919 + 1093 shadow checks, zero
  mismatches (EXP-0011, FACT-0013). They cut recording 230.8 -> 129.8 ms/epoch.
- The paused-fight measurement is reproducible to within ~1% across captures, so A-B comparisons
  are now meaningful. Use `tools/profile-production.ps1`, not the legacy `run_ufc5.ps1` (WARN-0008).
- `KYTY_DISPATCH_RANGES_CSV=<path>` logs per-dispatch thread groups and every buffer/image range
  read or written (tagged bw/br/iw/ir). Off by default; diagnostic only.

## Broken or Blocked

- ~10.4 fps is still ~2.9x short of the 30 fps floor (DEC-0003). Not playable yet.
- The bottleneck is host CPU command recording, not GPU work and no longer readback waits
  (EXP-0009). Readback waits fell to 9.6% of wall; do not port the old branch's transfer-queue
  work until attribution says otherwise (FACT-0008).
- **Four lines of attack are closed; do not re-walk them.** Compute dispatch merging is blocked by
  scattered, differently-sized writes (FACT-0021). Barrier elision fired on 17.9% of dispatches for
  no gain, reverted (EXP-0017). Whole-binding reuse measures a 1% hit rate (FACT-0022). The draw
  commit worker thread HALVES the frame rate (EXP-0020) -- never enable `KYTY_DRAW_COMMIT_THREAD`.
- **Recurring trap, hit three times:** the recording stream is not partitioned the way the logical
  phases suggest, because every stage reaches the one shared command buffer through
  `CommandBuffer::Handle()`. Instrumented adjacency does not prove nothing was recorded between two
  points (FACT-0019), and hazard-freedom does not imply recording-adjacency (EXP-0017).
- Remaining per-draw cost sits in `draw_state_cpu` and `draw_bindings_cpu`: ~123 ms/epoch of a
  ~246 ms wall (FACT-0015), still the largest single item -- but **local optimization is close to
  exhausted**: three attempts returned at most 1.7% (EXP-0019). Scene variation between paused
  fights exceeds that, so sub-2% changes cannot be measured end to end.
- Frame composition has shifted: recording ~48%, GPU batch ~30%, waits ~16% (FACT-0015). Pure CPU
  optimization cannot reach 30 fps alone; zeroing all instrumented CPU work still leaves ~84 ms.
- Waits rose as CPU work fell, so further CPU wins buy less than the early ones did.
- 1080p override breaks the scene/HUD. BDA suballocation crashed at round start.
- Format-83 R16G16_SFLOAT CMask clear decoding remains a correctness gap. LDS-bound output has
  visual observation but no pixel-matched proof (WARN-0007).

## Current Milestone

Frame time is attributed (EXP-0009, EXP-0010, FACT-0011) and the cheap wins are banked. Now reduce
per-draw CPU work in `draw_state_cpu` / `draw_bindings_cpu` and the graphics draw count, measured
end to end against the reproducible ~246 ms/epoch paused-fight baseline, toward the 30 fps floor.

## Next Work

Getting to 30 FPS needs ARCHITECTURE, not more local tuning. The two candidates, both
multi-session, and they likely have to be combined:

1. **Cut the 5570 graphics draws per epoch (TASK-0012)** -- the untested lever with the most
   headroom. Each draw removed saves per-draw CPU, GPU batch time and barrier records, so it pays
   three times. Start by instrumenting consecutive graphics draws the way the compute path was.
2. **Parallel recording via secondary command buffers (TASK-0011)** -- the one threading approach
   EXP-0020 does not rule out. Real scope is making the texture/buffer/pipeline caches concurrent,
   not just splitting the buffer. Ideal 4-way is ~37% wall, roughly 14-16 fps, so not sufficient
   alone.
3. Close the pixel-matched proof gap on bounded LDS (WARN-0007, TASK-0003).

Last updated 2026-10-08 after EXP-0017. Original `ledger.md` content is DEPRECATED historical
notes; its process IDs and live-mode claims are snapshots, not current state.

## 2026-11-02: separate FC 27 Metal investigation

FC 27/macOS Metal work is separate from the UFC 5/Windows milestone above. See
`.porting/porting-handoff-fc27-metal-2026-11-01-claude.md`, section
“2026-11-02 (afternoon, Codex codex112–codex114)”. The captured FC 27 queue
was progressing slowly rather than demonstrably deadlocked, and the target
vertex shader's buffer[7] was UV parameters rather than positions. Rendering
and queue causes remain unresolved. The `ledger` CLI was unavailable on this
Mac, so no Ledger entries were added.

## 2026-11-03: FC 27 continuation blocked at launcher guard

See the latest section of `.porting/porting-handoff-fc27-metal-2026-11-01-claude.md`. The current Codex process command line contains the user's full prompt and matches the unchanged `run1.sh` single-instance guard; no emulator was started. Resume from a session whose process arguments do not match the guard. `KYTY_CS_BUF_STATS` uses `TryReadBacking`, which takes the guest backing-map mutex; replace it with a mutex-safe, GPU-ordered snapshot before using it on the GPU thread. The requested queue and rendering fixes remain open. Ledger CLI was unavailable.

## 2026-11-04: FC 27 Metal stadium lighting investigation

The 2026-11-03 launcher-guard and unresolved queue statements above are historical. Later work in the FC 27 handoff reports default encoder fences, index-offset and SRT fixes, and image-alias changes; the picture is still not consistently correct. In codex151, current-build stadium traces show nonzero ground G-buffer values but nearly black HDR before tone mapping. No particular dispatch is yet proven responsible. Codex150 also showed persistent horizontal screen-space bands across changing scenes. Codex152 captured lighting shader buffer b3[0]=0x4855 in six traced dispatches, so the bit 0x20 controlling its optional unbound image-6 sample was clear in those dispatches. Added opt-in `KYTY_PASS_TRACE_CSBUF_FRAME` for bound-buffer snapshots during PassTrace; the macOS emulator build passed. No rendering fix or post-fix ten-shot validation was made. See the 2026-11-04 section of `.porting/porting-handoff-fc27-metal-2026-11-01-claude.md` for run flags, evidence, and open questions. The user confirmed encode-time `KYTY_CS_BUF_STATS` is permitted on the command-processor thread. The Ledger CLI remains unavailable on this Mac; no Ledger entries were added.

Follow-up codex154–156 used local single-pixel MSL overrides: the stadium ground invocation sampled image9 RGB around (0.00017, 0.00012, 0.000056), and its v1303 multiplier was approximately 0.08105 in two covered dispatches. These values lack a matched expected/reference comparison and do not establish root cause. See the FC 27 handoff for caveats and local diagnostic details.
