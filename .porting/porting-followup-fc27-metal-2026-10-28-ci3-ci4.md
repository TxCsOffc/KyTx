# FC27 — indirect instanceCount, loop cap, чёрное изображение; ci3/ci4, 2026-10-28

Актуальное продолжение после MDI follow-up. FEX ARM64 + direct RDNA→MSL, без Vulkan/Rosetta. HEADc30d5d81/metal-native-checkpoint, dirty/untracked сохранены; коммитов/новых агентов нет. Ledger CLI отсутствует. Пользователь сейчас просит **чинить чёрное изображение**. Цель корректные интро/меню/матч30+FPS не достигнута.

## Выполненные ровно2 отдельно согласованных запуска

- **codex_ci3**, PID6169, binarycfb0a4e38de9d8ce1af799059b59c9b4864789bd6ca29c7ba929f074d093a3b2, log `fex-spike/runs/validation-20261025/lean/codex_ci3-142339.log`. Wave64ON/UMAOFF, cap256, splitdraw+CSот500. Watchdog8s на indexed indirect VS4b2fb528/PS74014bc2. GPU snapshot **доказал** реальные args: `[23694,3562971914,0,0,0]`, args0x81a0013b00/max1/stride20/count0, ib0x81c66b1000/49152bytes, index_limit24576. Причина огромного объёма работы:3.56млрд instances (~84трлн index invocations). Это не пустой idx1 — idx1placeholder. Предыдущие packet1 args49152/24 и packet2 zero/zero. CPU count/index checks не ограничивали этот GPU instanceCount. PPM1499 полностью чёрный (всеRGB0).
- **codex_ci4**, PID6596, binary648a909f40ae260e2f90f37dd10bf0ef025f8cc6fb180571c2d47fcb949c4b75, log `.../codex_ci4-143539.log`. Отдельное OK на cap4096 + instance guard; та жеUMAOFF/wave64/split500. Дошёлдоflip3200, завершён run1 по240s limit, **без logged GPU-watchdog/errors**. MdiArgs `[49152,46,0,0,0]`, `[24576,134,0,0,0]`, `[23694,178,0,0,0]` и следующиевалидные. Снимок CUA на3133/FPS16: белое поле/чёрныеблоки/точки; ранний экранчёрный, PPM1194сохранён. Правильнаякартинка не восстановлена,16FPSэтоdiagnostic,не gameplay win. Оба запуска уже закончены, третьего разрешения нет. pgrep пуст.

Артефакты обоих запусков: `.porting/artifacts/2026-10-28/dispatch-indirect/`: summary.json, approval.json, logs.gz, MSLdirsci3/ci4, PPM. ci4сохранил876MSL. Не печатать полный лог (>600тысстрок).

## Найденный producer и bounded replay

CS e44a2bb59715251f (cap256), b4 пишет по адресам0x81a0012000/12a00/13b00. Threads1×1×1; в конце, только в финальной группеy1/xlast, пишет20byte draw args. До финальных stores есть loops. Normal cap4096 меняетnative source hash на **e36e6cbaf1be933f** (вычисленFNVprefix/inverse, подтверждён свежим ci4 dump).

Сохранённый настоящийMSL проигран **отдельно от игры**, 2×1 workgroups, всеinputs малы, originalpoisoninstance0xd45ea30a. Artifact `ci3-producer/replay.mm`, binary, replay.log. API+shader validation:
- cap256,N24/N64полностьюкопирует/финализируетargs;
- cap256,N96/N924копирует83строки, возвращаетсяпоfuelcap, оставляетinstance3562971914 незаписанным;
- cap4096,N24/64/96/924 полностьюкопирует и пишетправильныйinstanceCount (924строки~5.3ms).
Это **доказанный механизм порчиargs при низкомcap**. Точныеci3входыproducerнебылиполностьюсняты, поэтому не объявлять этот synthetic N924 точными входамиигры. Вci4нормальныесчётчики/отсутствиеповторногозависания подтверждают пользу возврата4096, но этоn1 и все прочие hangs не закрыты. Не снижатьcapдо256вследующих тестах; он может самсоздаватьпорчуиGPUhang. MslEmitterпоумолчаниюуже4096, emitterнеизменён. `.porting/work/hang_run.sh` default исправлен4096.

## Изменения послеci3

1. MetalMultiDrawIndirect наGPU применяет **существующийCPU/legacy safeguard**: instanceCount>65536→1, 0и65536 сохраняются. Originalrawsnapshot не меняется, predicateнеактивныхзаписейприменяетсяпослеguard. Это containment/parity, **не исправление источника произвольного мусора**. Дефектдоказан safe GPU-only boundary test: raw0/65536/65537/3562971914/UINTMAX, до3fail→после0; эти hugecountsвrasterizerнеподавались.17pixelGPUcases и3invalidcontrolsтакжепроходят. Build/diffcheckпроходят. Snapshots/patch/logs вci3-guard/.
2. KYTY_PASS_TRACE_CSBUF теперь читает actual bound MTLBuffer.contents+offset (а не переиспользуемый guestCPU upload address), boundsвыдержаны. ДобавленCSBUF_N default16дляограниченияdiagnosticwaits/logs.
3. **Важное уточнение ci4 CSBUF:** первоначальное Commit(true)+WaitUntilIdle послеSPLIT не синхронизировало очередь! WaitUntilIdle втекущемMetalContext ничего не делает, когдаcurrentCB=nil. Поэтомуci4строкиCSBUF AFTER (b4ещёнулевой) **нельзя считать завершёнными outputsproducer**; MdiArgsпозднее достоверны, т.к. prepсоздал свойcurrentCB иwaitилсяон. Послеci4 CSBUF/PassTraceReport явно создают currentCB передCommit(true), темсамым добавляяqueuefence. СамMetalContext::WaitUntilIdleнепереписывали: Shutdown вызываетего подmutex, вызовGetCurrentоттудасоздалбыdeadlock. Патчдиагностикиci4-fence/; не повторять утверждение«WaitUntilIdle сам дренируетвсюочередь».

## Чёрныйэкран и следующий одно-frame capture

Чёрныйэкран/белоечёрныеблоки ещёне локализованы. Не доказано, что виноват split; EnsureActiveRenderPass проверяетналичиеencoder, Draw oklogsпоказываютлишь5skip/170тысautoDraw. Ранниефреймычёрные, позже3Dестьгрубаяпорча. Не делатьслепойclearHDRилиновыйfallbacktexture. НуженplainA/BиpixelinspectionвXcode.

Подготовлен `plain-capture/env-codex_ci5-planned.sh`: cap4096/wave64ON/UMAOFF, **SPLIT/SYNC/FAULT_INFO/CSBUF/MDI_TRACE выключены**, run1/20GB/watchdog8s, MTL_CAPTURE_ENABLED1, **одноразовыйcapture trigger**, no fixedframe list. ПослеLLDBsetupиflip>=600создать `/tmp/kytx_codex_ci5_cap` **ровноодинраз**: сниметсяодинframeнаследующихflips. `KYTY_GPU_CAPTURE_DIR=.../ci5-gputrace`. MSLdirci5-msl. Ничегоне запускатьбезотдельногоOK.

Передcaptureвосстановитьработавшийrecipe: run1стартуетединственныйпроцесс; найтиегоPIDпометке, `gpucapture config --pid PID --shared-mem-opt disabled`; LLDBattachсFEXsignalsSIGSEGV/SIGILL/SIGBUS/SIGUSR1/2 pass=true,stop=false,notify=false, затемcontinue. Неиспользоватьстарыйcapture_under_lldb.sh напрямую: онсамстартуетвторойнеограниченныйпроцессмимоrun1. TriggerтолькоПОСЛЕreadyLLDB. Послеcapturesavedдождатьсяконцатогоединственногопрогона; только затемXcode replay. Сначалапроверитьфайлналичие/валидность.

Дляcaptureподготовлен код: Native swapchain вызываетMetalRenderer::CaptureFrameTick подrendererrecursive mutex; start/stop capture больше не коммитятобщийCBпараллельносCPencode. Наstartиstop создаётсяqueuefence+wait. Source backups/patch `plain-capture/`, ARM64 build проходит. Это предохранение диагностирования, нефиксчёрногокадра. ФинальныйbinarySHA256: f1dbe598aa75033a76dd2d89a7842c0a816a16c25000a5f1bf8f0856971e763d.

Активныйextra_env_s1.sh **ещёci4**, plannedci5неактивен. Всеигры/LLDB/gpudebugостановлены. Следующеедействие: запроситьOKнаодинplainпрогон+one-framecapture, затемXcodeчерезCUA. Разрешениенакаждыйпрогон — сохранённаяпросьбапользователяизhandoff2026-10-28Claude. Пользовательхочетчинитьчёрныйэкран, неостанавливатьсянаCPU/GPUunittestsкакнаготовойигре.
