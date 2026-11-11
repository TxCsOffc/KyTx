# FC27 Metal — handoff, 2026-10-27 00:42 Asia/Tashkent

## Текущее состояние

**Продолжать здесь, в Codex.** Пользователь остановил старую Claude-сессию и явно подтвердил: «Остановил, продолжай здесь». Затем попросил завершить работу и написать handoff. Новых агентов/чатов не создавали. Старый CLI автоматически возобновился после лимита и однажды остановил наш тест; конфликт уже разрешён пользователем, повторного разрешения не спрашивать.

**FEX ARM64 + direct RDNA→MSL. Без Rosetta/Vulkan renderer.** Чтение Vulkan reference source — только как эталон логики. Один эмулятор за раз, через `.porting/work/run1.sh <tag> <max-seconds>`; guard20GB и GPU watchdog20s. Два эмулятора ранее вызвали WindowServer kernel panic на этом48GB Mac. Не запускать тяжёлый GPU replayer параллельно игре. Все GPU captures строго по одному кадру в отдельный файл.

Ветка `metal-native-checkpoint`, HEAD `c30d5d81ab23`. **Коммитов нет; сохранить весь dirty/untracked tree.** Ledger CLI отсутствует; память — этот файл, предыдущие handoff и markdown STATE. В конце проверен `git diff --check`, без ошибок.

**На момент передачи эмуляторов, run1, LLDB и gpudebug нет. Xcode открыт на frame2300.gputrace, Replay остановлен: Finished running / кнопка Replay.** Последняя попытка Debug Pixel отменена по просьбе пользователя закончить работу; shader values из неё не получены.

Полная сцена по-прежнему неверная: зелёные/белые пересветы, блоки и точки/шум. Правильный матч и30+ игровых FPS **не достигнуты/не измерены**. Цифры FPS в заголовке окна заставки не являются игровым результатом.

## Что прочитать дальше

1. [Подробный follow-up RECTLIST и Xcode](/Users/tx/Developer/KyTx/.porting/porting-followup-fc27-metal-2026-10-27-rect-indirect.md).
2. [Наши HDR-наблюдения и уточнение post/pre draw](/Users/tx/Developer/KyTx/.porting/porting-followup-fc27-metal-2026-10-26-hdr-resume.md).
3. [Claude night handoff: stencil, compute, 3D volumes, memory](/Users/tx/Developer/KyTx/.porting/porting-handoff-fc27-metal-2026-10-26.md).
4. [История UI mip-tail/X-icon](/Users/tx/Developer/KyTx/.porting/porting-followup-fc27-metal-2026-10-25-ui-mip-tail.md) и [stencil](/Users/tx/Developer/KyTx/.porting/porting-followup-fc27-metal-2026-10-25-stencil.md).

Красный экран из старых Codex записей **уже исправлен stencil-реализацией Claude**. Не возобновлять старый поиск красного слоя. Сохранить logo flat/no-perspective fix, UI mip-tail/X-icon fix, stencil и compute-dimension fixes, HTILE/DCC/CMASK handling, volume uploads, lifetime/cache eviction. Wave64-pair эксперимент остаётся OFF: раньше приводил к нестабильным guest-pointer faults. Принудительный CLEAR_HDR_FIRST — диагностический обход, не решение.

## Новые функциональные исправления

### 1. Nonindexed indirect RECTLIST

Игра выдаёт GPU arguments `[3,N,firstVertex,firstInstance]`; CPU placeholder в DrawAuto равен1. Раньше wrapper включался только при CPU count3, DrawIndirect оставлял Triangle и3 вершины — рисовалась половина прямоугольника.

- `src/graphics/host_gpu/metal/metalIndirect.h/.mm`: новый MetalRectListIndirect GPU compute. Только vertexCount3→4; сохраняет instanceCount, vertexStart, baseInstance, исходные аргументы. Output record32bytes: offset0 новые draw args, offset16 оригинальные. Поддерживает offset/stride/multi-entry, bounds checking. Guest args не изменяются, CPU readback нет. Пул обычных tracked Metal3 buffers.
- `metalRenderer.mm`: получает актуальные MDI args после существующего draw-count clamp, выполняет конверсию до установки render state; pass открывается с Load для color/depth/stencil. Wrapper mode3; per-draw snapshot в vertex buffer26. Прочие VS получают валидный zero buffer26. Indexed draws сбрасывают старые nonindexed RECTLIST controls.
- `MslEmitter.cpp`: mode3 учитывает original firstVertex и синтезирует base+3 из base/base+1/base+2, не читая отсутствующую четвёртую V# запись.
- `metalDraw.mm::DrawIndirect`: RECTLIST использует **TriangleStrip**. Общий TranslatePrimitiveType по-прежнему Triangle для indexed path. Это отдельная часть интеграции, найденная Xcode после первого неполного исправления.

Область доказанной поддержки — nonindexed indirect count3. Indexed indirect RECTLIST этой правкой не реализован; runtime-пример для него не найден.

### 2. Packed bits четвёртого угла

Xcode показал правильные4 позиции, но param0 первых3 вершин содержал одинаковые малые float bit patterns (`~7e-45,~4e-43,0,0`), а четвёртой — нули. Формула B+C−A пропускала упакованные целые через FP arithmetic и теряла subnormal/signed-zero bits.

`kyty_rect_corner` теперь покомпонентно сравнивает uint-биты A/B/C: если одинаковы, копирует B побитно; для изменяющихся компонентов оставляет B+C−A. Применено к mode1/direct, mode2/indexed и mode3/indirect. Не утверждать, что это единственная причина HDR-шума: у обычного flat triangle provoking vertex может быть исходным углом.

## GPU-тесты и сборка

Команда: `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 bash tools/test_metal_msl.sh`.

Полный native MSL suite проходит. Новые тесты используют настоящий EmitProgram и production MetalRectListIndirect; source args создаёт GPU blit в private buffer.

- Triangle/count4 контроль:528/1024 неверных пикселей. Пять Strip случаев:0 каждый — coverage/UV, bounds3 записи, firstVertex5, instanceCount2/baseInstance7, zero instances/count, non-three count4, offset16/stride32, argument preservation/bounds.
- Packed regression специально ставит синтетическую вершину первой в отдельном треугольнике, чтобы flat output проверял именно её. Константы5,0x10003,1.0,-0.0: **до528/528bad, после0**, отдельно во всех3 режимах.
- ARM64 `cmake --build _Build/macos-arm64 --target kyty_emulator -j8` успешен; `build-rect-packed.log`. Никаких новых production edits после этого build.

Логи и исходники: `/Users/tx/Developer/KyTx/.porting/artifacts/2026-10-26/hdr-resume/` — `rect-indirect-topology-test.log`, `rect-packed-before-test.log`, `rect-packed-final-test.log`, build logs. Source snapshot с manifest: **`codex-checkpoint-0031/`**; все7 production/test файлов до передачи совпадают с manifest. Содержит и прежний dirty код: не накатывать целиком поверх будущих правок.

Текущие normal binary и private_s3 совпадают по SHA256:
`b8f5f82a1a2db079bffd101f2d51451dccce45a620070ce88b6afe80e01123eb`.

## Доказательства Xcode

### Первая, промежуточная трасса

`/Users/tx/Developer/fex-spike/runs/captures/fc27-20261027/rect-indirect-one/frame2600.gputrace` —18GiB. Xcode Replay успешен:27CB,87render,388compute,17blit,1058draw,307dispatch. CB21/RE37–39, API16444/16484/16527 всё ещё Triangle при новых args4 — эта трасса обнаружила пропущенную топологию. **Не использовать как proof полного исправления.**

### Трасса с исправленной топологией, до packed fix

**`/Users/tx/Developer/fex-spike/runs/captures/fc27-20261027/rect-indirect-strip-one/frame2300.gputrace`** —15GiB disk /17.48GiB ресурсов в Xcode. Успешный Replay: **23CB,82render,432compute,19blit,765draw,367dispatch**. CB18, CE231/232/233, RE39/40/41; API13189/13229/13272 — TriangleStrip, snapshot vertex buffer26 offset16.

В API13272 double-click Indirect в Bound Resources: offset0 `[4,160,0,0]`, offset0x10 `[3,160,0,0]`. Geometry instance0 positions0..3:
`(.920,-.133,1,1)`, `(.960,-.133,1,1)`, `(.920,-.204,1,1)`, `(.960,-.204,1,1)`.
Packed param0 четвёртого угла был0, что привело ко второму фиксу. **Эта трасса предшествует packed fix.**

Краткие доказательства: `xcode-first-replay.txt`, `xcode-strip-replay.txt` в artifact directory. Не путать API/texture IDs разных трасс.

### Последняя точка shader-debug, ещё без результата

В этой же frame2300 через CUA: Replay → Filter trace list `13272` → draw → Attachments, LoupeX1544/LoupeY520. Координата внутри прямоугольника instance0 (x1536..1568,y512..544), Debug Pixel становится активным. Нажат Debug Pixel; дошёл до Gathering execution data from GPU. Пользователь попросил handoff, нажата Stop GPU workload; появился alert `Unable to create shader debug session / Unable to connect to the device. (6)`, закрыт Cancel. Replay остановлен. Это **не результат пиксельной отладки** и не установленный самостоятельный сбой отладчика: сообщение появилось при остановке работы. Повторить с этой точки, если полезно; Xcode document оставлен открытым.

CUA — единственная технология для GUI. Доступ к Terminal через CUA раньше был отклонён; обходы не применялись. Xcode и game CUA доступны. Если контекст сжат, начать CUA с `await cua.rewriteDocumentation()`.

## Проверки в игре и конфликт сессий

- rectind1/rectcap1 — промежуточная сборка с ещё Triangle; не доказывают полный fix. rectcap2 — Strip, но ещё до packed fix; PPM2356 `rectcap2-menu.png` показывает остаточные пересветы и шум. Logged GPU errors0.
- Финальный rectpacked1 PID20025 остановил внешний Claude root после автоматического сброса лимита:19:31:02Z killmine.sh, последний flip1125. **Не game crash.** История конфликта в предыдущем follow-up.
- Пользователь остановил старый CLI и подтвердил продолжение здесь. Source изменений старый агент не сделал — проверены7 hashes. Оставил запущенный **acc1 PID20491**, созданный через run1 с той же final packed binary. После Ctrl+C родитель run1 исчез; процесс был orphan, PPID1.
- Мы осмотрели acc1 через CUA: flip8459 и8993 — Press Any Button, зелёный/белый фон, блоки/шум всё ещё есть. После проверки **остановлен только PID20491 с проверкой RUN_TAG=acc1**. Лог `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/acc1-003219.log`, последний DrawStats9675, dropped_vs/ps/pipe0, logged GPU errors0. Это подтверждает длительный запуск final binary, но **не правильное меню/матч**.
- acc1 не дал PassTrace windows и PPM; MEM_LOG не был включён, footprint не измерен. `acc1-summary.json` помечает footprint как null/unavailable.

## HDR: что уже известно, без догадок о фиксе

Добавлены env-gated diagnostics в metalRenderer: PassTrace consecutive flips1..16, PRE_RT address/hdr до записи в цель, last writer format/flip для input textures, BUFWRITE ranges для compute buffers в0x8240..8250. Нормальные DrawPassTrace RT stats — **после draw**, не до него. Counter `dispatches=0` в end-line ошибочный; dispatch events реально присутствуют.

- hdrpre1: RT0x8242360000 после PS944430 содержит41702NaN, но input64x8249bb0000 уже748867NaN. Не доказывает birth внутри PS.
- hdrpre2 flip3596: PRE-RTx8249340000/RG11B10 содержит2606989NaN; last writer — RGBA8 того же flip, CS395d8464bb9983ec. Light draw уменьшает NaN. Input64x82499c0000 конечен, writer RGBA16F flip3584. Проверять смену формата/назначения памяти, инициализацию и выбор актуальной Metal-текстуры.
- hdrpre3: BUFWRITE не перекрывает выбранные HDR/source адреса. Fill0x8241cd0000/6815744 заканчивается0x8242350000, а HDR начинается0x8242360000. Гипотеза пропущенной buffer-fill очистки HDR не доказана. Sparse output CSa7ba нельзя считать целиком определённым после частичного dispatch.
- Принудительная HDR first-use clear улучшала некоторые сцены; широкая clear делала хуже. Не включать это как production fix.

Логи hdrpre1-233224/hdrpre2-234015/hdrpre3-234538 в `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/`; MSL dumps в artifact `msl/`. Резко сужать вывод по нужным events, не печатать все megabytes UI/JSON или16k файлов trace.

## Следующие действия

1. Прочитать этот handoff, убедиться, что нет чужого эмулятора/replayer. Сохранённые source fixes приняты GPU-тестами; не откатывать их из-за ещё зелёного фона.
2. Для следующей диагностики можно начать с существующего frame2300/API13272 и выбранного1544,520: получить shader output/texture input и сравнить с RT до draw. Это старая packed-варианта трасса — учитывать это при выводах. Не объявлять пост-blend NaN ошибкой ALU без сравнения старого RT и входов.
3. Если нужен актуальный trace после packed fix, новый bounded run с одним кадром; не создавать серию повреждённых/пустых captures. Проверять stopped, workload, Replay, а не только наличие bundle.
4. Исследовать первый источник HDR garbage/alias lifetime/clear. Меню и intro частично исправлены, полный матч ещё не проверен. Производительность оценивать только после правильной сцены.

Текущий `.porting/work/extra_env_s1.sh` остался **acc1** (auto input1500/1850/2200:j, PassTrace trigger, CS store logging). Перед новым запуском выбрать новый уникальный RUN_TAG и осознанно настроить env; не использовать его как обычный пользовательский запуск без изменений. run1 сам безопасно обновляет private_s3 после сборки. Пользователь сейчас просил только handoff — ничего дальше не запускать до следующего запроса.
