# FC27 Metal — красный слой найден: не было stencil (2026-10-25, Claude)

Продолжение чата Codex «Review KyTx handover». Только native ARM64 **FEX + прямой RDNA→MSL**. Vulkan **не** используется в продукте: эталонная Vulkan-сборка запускалась лишь как визуальный oracle (разрешено пользователем в чате Codex). Ветка `metal-native-checkpoint`, HEAD `c30d5d81`, **коммитов нет**, весь прежний dirty/untracked tree сохранён.

## Итог одним абзацем

**Красный экран = отсутствие stencil в Metal-бэкенде.** UI рисует «шторки» (VS `bc0210100fe76732`, PS `ae49c6a306380213`) через stencil-клиппинг: по ~9 300 draw'ов `func=NEVER, fail-op=REPLACE_TEST, writemask=0x1` в двух вариантах (ref=0 и ref=255: они только пишут/стирают маску в stencil и не должны трогать цвет) и ~7 600 draw'ов `func=EQUAL, ref=1` (рисуют внутри маски); счётчики кумулятивные до кадра ~2200. Metal-путь рисовал в depth-only цель и молча игнорировал весь stencil, поэтому каждый из ~24 таких draw'ов красил весь экран с alpha 0.6 и слой насыщался в чистый красный. После реализации stencil главное меню FC27 рисуется корректно (FC NEWS … TOURNAMENTS, «Welcome to the Club!», FC HUB, «Press X to reconnect»). **3D-сцена под UI всё ещё неверна** (шум/цветовые полосы) — следующая задача.

Сравнение: [before-after-vs-reference.png](artifacts/2026-10-25/stencil-fix/before-after-vs-reference.png) (Vulkan-эталон / Metal до / Metal после / Metal после + полное разрешение + GDS).

## Что изменено в коде (всё под git diff, не закоммичено)

| Файл | Изменение | Флаг |
| --- | --- | --- |
| `metalPipeline.h/.mm` | Реальный перевод guest stencil → Metal: порт `ConvertStencilOp/State` из `renderer/depthRenderTarget.cpp` (кодировка AMD: REPLACE_TEST=3, REPLACE_OP=4, …; общий reference на грань, свёртка test/op value; неподдерживаемые операции → Keep + лог). Новая перегрузка `GetOrCreateDepthStencilState(…, has_stencil_attachment, stencil_ops_disabled, MetalStencilRefs*)`. Ключ расширен (4-битные операции вместо `&7`, opval, testval_bf, writemask_bf, флаги). Старая сигнатура сохранена. | — |
| `metalRenderer.mm` | `GuestPassHasStencil()`; отдельная **Stencil8** текстура на guest stencil-адрес (живёт между проходами как depth-поверхность), `has_stencil` в pass desc, clear на fresh/`stencil_clear_enable` (`GetStencilClearValue`), учёт в `same_pass`; `pipe_key.stencil_format = Stencil8`; per-draw `setStencilFront/BackReferenceValue`. | `KYTY_NO_STENCIL=1` — старое поведение |
| `metalDraw.h/.mm` | `SetStencilReferences(front, back)`. | — |
| `metalRenderer.mm` (DispatchDirect) | **`USE_THREAD_DIMENSIONS`** (бит 5 mode): размеры диспатча — это потоки, а не группы (заливки/копирования памяти шлют «1 поток на dword»). Metal брал значение как число групп → ×`tg_w·tg_h·tg_d` лишних потоков (до 141 млн потоков на заливку 8,8 МБ; тысячи таких диспатчей за запуск). Теперь `groups = ceil(threads / group_size)` — как делает Vulkan `RenderExecutor::DispatchDirect`. | `KYTY_CS_RAW_GROUPS=1` — старое |
| `graphicsRun.cpp` | `KYTY_EOP_CLOCK_FREEZE=1`: все 64-битные EOP-таймстемпы одного флипа равны (см. ниже «динамическое разрешение»). `KYTY_LOG_DMA_RT=1`: лог DMA в арену RT. | opt-in |
| `swapchain.cpp` | `KYTY_AUTO_INPUT="флип:клавиша:удержание_во_флипах,…"` — сценарные нажатия без фокуса окна (например `1500:j:8,2500:j:8`). J = Cross. | opt-in |
| `metalRenderer.mm` | Диагностика: `KYTY_TRACE_RANGE=lo,hi,flip_lo,flip_hi` (все операции над диапазоном guest-памяти в порядке кодирования), `KYTY_LOG_STENCIL=1` (гистограмма stencil-состояний по размеру RT и по паре шейдеров), `KYTY_FC_CLEAR=1` (эксперимент, **не помог**, см. «опровергнуто»). `metalDraw.mm`: `KYTY_LOG_VP` теперь печатает flip и размер RT. | opt-in |
| `tests/metalPipelineTests.mm` | Новый GPU-тест `StencilMasking`: NEVER+REPLACE-маска не пишет цвет, EQUAL-заливка клипуется маской; 64 пикселя, **0 ошибок** при Metal API+shader validation. Остальные тесты `tools/test_metal_pipeline.sh` проходят. | — |

## Как нашли (методика — повторяемая)

1. **Эталон как oracle.** Vulkan-сборка `KyTx-diag/_Build2` (ARM64+FEX+MoltenVK) показывает правильную последовательность: чёрный → заставка → синий ролик **EA SPORTS** (кадры ~629–1000, 60 fps) → «Press X to Skip» → «Kyty is connecting to the EA Servers…» → «IS YOURS / Let's Play!» → главное меню с полной 3D-сценой на 24–27 fps. Запуск через враппер `tools/KyTxReference.app` (нужны `SDL_VULKAN_LIBRARY`, `KYTY_FORCE_PIPELINE_CACHE=0`). Эталонные Metal-трассы (через `KYTY_VK_GPU_CAPTURE=<flip>`): `…/runs/captures/fc27-20261025/ref/vk_f760.gputrace` и `vk_f1700.gputrace` (по ~9 ГБ).
2. **Хронология окна**: `tools/timeline.sh` + `tools/winid.swift` снимают конкретное окно по ID (`screencapture -l`), даже если оно перекрыто; `tools/sheet.py` собирает контактные листы. Один прогон = ~100 с.
3. **Бисект по проходам** в готовой `frame1000.gputrace` через `gpudebug` (`tools/gd.py`): 207 из 286 render-проходов пишут в один RT `ColorRT (4)` 1920×1080 RGBA8 с `Load`; вывод каждого прохода показал, как мусорная база насыщается красным.
4. **Лог операций над памятью** (`KYTY_TRACE_RANGE`) показал, что память UI-RT используется и как compute-картинка 1600×904, и как 1920×1080 RT, а очистки нет.
5. Перебор подозреваемых отсёк scissor, DMA-очистку, CMASK и т.д. (ниже). `KYTY_LOG_STENCIL` дал решающую гистограмму: красные шейдеры — это ровно stencil-пары NEVER+REPLACE / EQUAL.

## Побочные находки (проверены)

- **Динамическое разрешение (DRS).** В эталоне сцена 1920×1080 при обоих замерах (кадры 760 и 1700). У нас 1600×904 / 1624×912 / 1616×912 / 1640×920 (разрешение «дышит»), и сцена ветвится через апскейл-compute, где появляется «полосатый мусор». Игра выводит GPU-время из 64-битных EOP-таймстемпов (`event_write_source==4`), которые мы пишем из часов **времени обработки команд** (`Sync::ReadReferenceClock()` при записи), поэтому стоимость кодирования Metal-командника считается «временем GPU». `KYTY_EOP_CLOCK_FREEZE=1` (внутрикадровая длительность = 0) возвращает **полные 1920×1080** (244 обращения compute к картинкам 1920×1080 против 1600×904) и даёт структурированную сцену. Это эксперимент/рабочий обход, не корректное решение: правильнее выдавать таймстемпы по фактическому завершению GPU-работы.
- **Заливки памяти.** `b3e6381fa8d1fc32` — универсальное ядро fill (`dst[(tgid*64+tid)*4] = pc_data.dwords[4]`) с проверкой `byte <= sizes[0]`; ~22 диспатча за кадр. Теперь запускается с правильным числом групп (см. таблицу). На FPS в меню это почти не повлияло (≈29 vs ≈28 — игра упирается в ~30 fps/CPU), но убирает до ×64 лишних потоков.
- **UI-RT и CMASK.** После **каждого** UI-draw в RT с CMASK `MarkCmaskTouched` завершает encoder и делает blit-fill → каждый UI-draw превращается в отдельный render pass (в трассе 207 UI-проходов + 209 blit'ов). Это тяжёлый хак; стоит переделать после того как сцена заработает.
- **Скорости.** Лёгкий запуск (без validation и спам-логов) в меню держит **≈28–30 fps**; со stencil + полным разрешением + GDS — **29,8 fps** в среднем. FPS в матче не измерялся — до матча не дошли.
- **Показ ролика EA SPORTS зависит от сохранённого состояния/ветки**, а не обязательно от рендера: в эталоне он был во втором прогоне и отсутствовал в трассе третьего (после того как Metal-прогоны с нажатием «Let's Play» перезаписали `_SaveData/PPSA34015/Settings…`). Чистое состояние запускает сценарий первого запуска (язык/доступность → «User Agreement»). Недетерминированность сетевой ветки: через «Cancel and Play Offline» идёт «THE CLUB IS YOURS / Let's Play!» (красный фон — тема клуба Bayern, это легитимно), через таймаут — ветка загрузки меню. **Не заявлять отсутствие ролика багом рендера, пока не воспроизведено на восстановленном «нулевом» состоянии.**
- **Красный фон «THE CLUB IS YOURS» — легитимная тема клуба**; не путать с артефактом.

## Опровергнуто (не повторять)

- Scissor: на 1920×1080-проходах всегда `0,0,1920,1080` (251 722 draw'а) — не причина.
- Очистка RT через DMA: `KYTY_LOG_DMA_RT` — нулевое число DMA в арену RT.
- CMASK fast-clear/«накопление»: `KYTY_FC_CLEAR=1` (очистка fast-clear RT на первом проходе каждого флипа) красный не убрал; UI-RT `0x82409c0000` вообще `cmask=0 fc=0`.
- GDS сам по себе красный не лечит (но входит в рабочую комбинацию ниже).
- Размер привязки `b3e6381f`: V# ограничивает заливку 2,5 МиБ и совпадает с числом «групп» (=потоки/dword) — клиппинга привязки нет.
- FEX/CPU: оба дерева линкуют один статический FEX; различия src только в Metal-коде и диагностике. Цвет `0x990000ff` в guest-памяти и V#-декодирование корректны.

## Рабочая комбинация сейчас (доходит до старта матча)

```
KYTY_MSL_ONLY=1 KYTY_BACKEND=metal KYTY_VIDEO_OUT_1080P=1 KYTY_NP_STATE=signed_in KYTY_LABEL_FLUSH=lazy \
KYTY_WAIT_RETRY_ELIDE=1 KYTY_WAIT_BY_ORDER=1 KYTY_UMA_READBACK=1 \
KYTY_EOP_CLOCK_FREEZE=1 KYTY_MSL_GDS=1 KYTY_MSL_CUBE=1 KYTY_AUTO_INPUT=<J каждые ~350 флипов с кадра 1500>
```
(stencil и thread-dimension fix включены по умолчанию.) `KYTY_AUTO_INPUT` для «спама Cross»: `python3 -c "print(','.join(f'{f}:j:8' for f in range(1500,12000,350)))"`.

Результат (прогоны `stc3`, `match1`, [контактные листы](artifacts/2026-10-25/stencil-fix/)): меню клуба (красная тема Bayern — легитимна) → «Kick Off» → **«Classic Match»: Manchester United — Liverpool, Start Match, Old Trafford, Amateur, Half Length 6, Sunny 19:00** — весь UI корректен (гербы, тексты, подсказки) → VS-заставка → загрузка матча. **Дальше экран чёрный/шумный** (экран «Hold for Full Match Intro» на чёрном; фиолетово-розовые шумовые кадры), а скорость падает: в меню 28–30 fps, при загрузке матча много 0.0 fps (долгие стоп-кадры), затем 3–9 fps. Это первый прогон, где автоматический ввод доходит до матча. Что именно тормозит загрузку матча (CPU/FEX-декомпрессия, компиляция ~200 новых шейдеров `[MSL] direct compiled`, ожидание GPU) — **не исследовано**.

Без `KYTY_MSL_CUBE`/`KYTY_MSL_GDS`/FREEZE (только stencil) меню рисуется, но фон — шум (прогон `stc1`); со stencil+FREEZE+GDS фон — структурированный, но глитчи (`stc2`); добавление CUBE (`stc3`) заметно улучшило путь до матча.

## Что дальше (по приоритету)

1. **Матч**: почему после старта матча чёрный экран/шум и 0–9 fps (сначала профиль: сэмплирование потоков хоста `sample`, `[MSL] direct compiled` по времени, число draw'ов/диспатчей на флип), затем сцена матча. Прогон `match1` — стартовая точка (`tools/variant2.sh match1 orig "<ENV>" 260`).
2. **3D-сцена под UI.** Теперь можно анализировать на правильном UI: бисект по проходам сцены в трассе нового запуска (по одному кадру: `KYTY_GPU_CAPTURE=<flip>`, `tools/gd.py`), сравнение с эталонной `vk_f1700` проход-в-проход. Подозреваемые: пропускаемые шейдеры `DataAppend` (GDS), `ImageSampleRaw` cube в compute (`KYTY_MSL_CUBE=1`), formatted `StoreBufferU32 desc_format=0`; неверный цвет/полосы (RG11B10/RGB10A2 каналы), tiling.
3. Заменить `KYTY_EOP_CLOCK_FREEZE` корректной эмуляцией GPU-времени (по завершению работы GPU), решить, включать ли по умолчанию.
4. Убрать пер-draw blit/`EndCurrentEncoder` из `MarkCmaskTouched`.
5. Ролик EA SPORTS: воспроизвести на нулевом состоянии `_SaveData`; затем разбирать, если всё ещё нет.
6. FPS в матче (30+) — только после корректной сцены и устранения стопов загрузки.

## Очистка DCC/CMASK-метаданных (вторая причина «мусора»), 2026-10-25 вечер

Находка по раскладке RT: все RT имеют `tile=27`, многие — `dcc=1` (в т.ч. RG8-маски света), и разрешение сцены (DRS) меняется каждый кадр при том же адресе. Игра очищает такие RT **заливкой метаданных кодом очистки** (распознаваемое ядро «uniform fill», `compute_info.stage.resources->uniform_fill`), пиксели не пишутся. Vulkan-путь превращает такую заливку в очистку образа (`TextureCache::TryConsumeColorMetaUniformFill`, `DecodeColorClear`); Metal исполнял заливку как байты, и пиксельная память хранила устаревшие данные прошлого кадра **с другим pitch** → горизонтально сдвинутые полосы (RG8-маски `cb17`) → почти чёрный свет.

Реализовано в `metalRenderer.mm` (включено по умолчанию, `KYTY_NO_META_CLEAR=1` отключает): `RegisterMetaClearRt` (при каждой привязке RT запоминает базу DCC/CMASK и clear-слова), `NoteUniformMetadataFill` (в `DispatchDirect`: заливка над базой метаданных известного RT → декодирование кода: DCC `0x20`=clear-регистр, `0x00/0x40/0x80/0xC0`=константы, CMASK `0`=регистр; значение должно быть `код·0x01010101`), `TakePendingMetaClear` (следующий render pass этого RT берёт clear-цвет как load action). Заливка метаданных исполняется как и раньше. Формат clear-регистра декодируется для R8/RG8/RGBA8/BGRA8/RGB10A2/RG11B10F/RG16F/RGBA16F. Логи `[MetaClear]`, `KYTY_LOG_RT_LAYOUT=1` (раскладка каждого RT), `KYTY_DCC_CLEAR=1` (старый грубый эксперимент «очищать DCC-RT на первом проходе кадра» — для сравнения, не включать).

**Эффект** (прогон `meta1`, GDS+CUBE, без FREEZE): после меню игра показывает вступление матча — карту Англии (Liverpool, Chester, Newcastle…), затем аэрофотографию **Old Trafford** с гербом и заголовком (раньше — чёрный шум). Пользователь в живой игре сообщил **38–40 FPS** на этой сборке.

**Известно/не проверено**: один из моих прогонов (`match2`) упал внутри гостевого кода (`Unhandled host exception … address=0x936d9f8dffff8080`, поток Job2; регистры содержат пиксельные данные вроде `80 80 ff ff`) на стадии загрузки матча при FPS≈0. Причина не установлена (возможно гонка/запись GPU в память с данными игры или побочный эффект очистки); A/B-серия не завершена, потому что я по ошибке убивал инстанс пользователя (см. ниже). Повторить: `tools/crashtest.sh`.

**ВАЖНО для следующего агента: пользователь сам играет на этой машине.** Никогда не `pkill` эмулятор по имени и не запускай/убивай инстансы без ведома пользователя. Скрипты `tools/` теперь убивают только свои инстансы (`tools/killmine.sh`: по `RUN_TAG` в окружении процесса). Сценарный ввод `KYTY_AUTO_INPUT` действует только внутри процесса, в который передан, и не влияет на чужие инстансы.

## Разбор 3D-сцены в одиночной трассе frame1900 (Xcode + gpudebug)

Трасса: `fex-spike/runs/captures/fc27-20261025/menu-scene/frame1900.gputrace` (19 ГБ; stencil + GDS + CUBE, **без** FREEZE → динамическое разрешение 1600×904). Xcode: Replay проходит; сводка 26 CB / 279 render / 1 473 compute / 216 blit энкодеров, 1 250 draw / 1 405 dispatch, превью = меню с корректным UI и глючным фоном. Граф зависимостей Xcode на таком кадре нечитаем (масштаб 1%) — удобнее `gpudebug` + точечный Debug Pixel.

По проходам ([1](artifacts/2026-10-25/stencil-fix/frame1900-scene-passes-1.png), [2](artifacts/2026-10-25/stencil-fix/frame1900-scene-passes-2.png)):
- **Геометрия и G-buffer верны**: RGB10A2 1600×904 (cb4–cb7) содержат узнаваемую сцену (здания, предметы, футболист), R8Uint (cb9) — корректный силуэт игрока.
- **Ломается дальше**: RG8Unorm 1600×904 в `cb17` (`re4, re9, re11, re13, re15`; маски света/теней/AO — первая «мусорная» ступень) содержат структуру сцены, но с горизонтальными «смещёнными полосами» ([пример](artifacts/2026-10-25/stencil-fix/frame1900-cb17-re4-rg8-mask-garbage.png)) — признак неверной раскладки памяти (tile/pitch) при записи или чтении, а не логики шейдера. Накопленный свет (RG11B10, cb18–cb21) почти чёрный с отдельными яркими окнами → итоговый фон тёмный с глитчами.
- Гипотеза для следующего шага: RT `kRenderTarget` (tiled у игры) проходит через линейные buffer-backed текстуры Metal; у 2-байтовых форматов (RG8: строка 3200 Б) pitch-выравнивание хоста (`LinearTextureRowAlignment`) может не совпадать с тем, как память читают/пишут compute-шейдеры игры. Проверить: pitch/tile этих RT (логи `[DrawF]`/`KYTY_TRACE_RANGE` по адресам RG8-проходов) и работу surface converter.
- Эталонная трасса `ref/vk_f1700` для этого не годится: на момент съёмки эталон показывал диалог EA, 3D-сцена не рендерилась. Нужна эталонная трасса главного меню (потребуется нажать «OK» в эталоне; в Vulkan-сборке нет `KYTY_AUTO_INPUT`).

**Съёмка трасс**: `tools/capture_under_lldb.sh <flip> <outdir> "<ENV>"` (LLDB с пропуском SIGBUS/SIGILL, `gpucapture config --shared-mem-opt disabled`). Обнаружено: при `KYTY_EOP_CLOCK_FREEZE=1` захват падает с SIGILL через ~0,3–0,4 ГБ после старта даже под LLDB; с GDS и CUBE по отдельности и вместе — проходит (14–19 ГБ). Для съёмки сцены не включать FREEZE.

## Третья причина «чёрной/мусорной» 3D-сцены: HTILE-очистка глубины + устаревший compute-образ (вечер 2026-10-25)

**Как нашли.** По матчевой трассе (`play/frame2618`, до исправления) глубина в G-buffer-проходах (216+25+330 draw'ов) осталась нетронутой, а `KYTY_LOG_DEPTH=1` показал, что draw'ы геометрии идут с `z_enable=1 z_write=1 zfunc=GEQUAL` (reversed-Z), `htile=1/1`, и **ни одного** draw'а с `DEPTH_CLEAR_ENABLE`. Игра чистит depth не draw'ом, а быстрой очисткой HTILE (compute пишет в HTILE-память). Vulkan-путь это знает (`RenderExecutor::TryConsumeComputeMetaClear` → `TextureCache::ClearMeta/IsMetaCleared/TouchMeta`: dispatch, который только пишет в зарегистрированный HTILE, = очистка; depth перезагружается значением DB_DEPTH_CLEAR на следующем проходе), Metal — нет, поэтому depth жил от кадра к кадру.

**Исправление 1 — HTILE fast clear (`metalRenderer.mm`).** `RegisterHtile(htile_addr, z_addr)` (при создании depth-surface с `htile_acceleration`), `NoteHtileWrite(base)` в `DispatchDirect` (buffer written && !read, шейдер без `has_bitwise_xor`), `TakePendingDepthClear(z_addr)` → depth-attachment получает Clear с `GetDepthClearValue()` на следующем проходе. Лог `[MetaClear] ... htile write base=… -> depth=…`. `KYTY_NO_HTILE_CLEAR=1` отключает. Эффект: у главного меню появился настоящий фон (тёмно-зелёные световые полосы), G-buffer меню Kick Off содержит геометрию, освещённая сцена 1920×740 корректна.

**Исправление 2 — compute читал устаревшую картинку (`metalRenderer.mm`).** Остаточный мусор (блоки 64-px сверху кадра, `tex390`/`tex394`) в финальном композите (CS `7615e9bf…` ← тонмаппинг CS `fe68b32aafc98c9a`) возникал так: sampled-картинка compute по адресу A разрешалась **сначала** через реестр `GetComputeWrittenImageForAddress` (старый объект, зарегистрированный раньше как compute-storage/draw-bind), а свежий render target по тому же адресу (освещённая сцена, RG11B10 1600×904) игнорировался. Игра переиспользует адреса внутри кадра. Новое правило «последний GPU-писатель выигрывает»: для sampled-картинки сначала `LastGpuWriterTexture(addr)` (запись `s_cs_storage`, её пишут и RT-проходы, и compute-storage), при совпадении размеров/размера texel'а берём её (`ReinterpretFormat`). `KYTY_NO_LAST_WRITER=1` возвращает старый порядок. Диагностика: `KYTY_LOG_CS_IMAGES=<hash cs>` печатает `[CsImage] … -> tex=… rt_at_addr=… cs_written=… last_writer=…` (после исправления tex == rt_at_addr == last_writer ≠ cs_written). Результат: в меню «Training Hub» вместо мусорных блоков — узнаваемая 3D-сцена.

**Исправление 3 — производительность UI-проходов.** Xcode (Summary → Insights → Bandwidth): «Encoder 50 is the first of 245 encoders that can be coalesced — 8.47 GiB» из 11 GiB пропускной способности на кадр меню. Причина: `MarkCmaskTouched` после каждого draw завершал render pass и делал blit-fill CMASK 0xFF. Теперь fill ставится в очередь `MetalContext::QueueBlitFill` и выполняется одним blit-энкодером при завершении прохода (`EndCurrentEncoder`); порядок «после draw'ов, до следующего читателя» сохранён. `KYTY_CMASK_SPLIT=1` возвращает старое поведение. Замер FPS в матче ещё не сделан.

**Инструменты (все в `.porting/work/` и `.porting/tools/`).**
- `KYTY_AUTO_INPUT` понимает `up/down/left/right/enter`; `KYTY_AUTO_INPUT_FILE=/tmp/kytx_input` — файловый канал: `echo "down:8 wait:40 j:8" > /tmp/kytx_input` проигрывается в живом процессе. В меню между нажатиями нужно ≥ ~30 флипов (быстрые нажатия теряются).
- `KYTY_GPU_CAPTURE_TRIGGER=/tmp/kytx_capture_now` (touch) и F9 в окне — трасса следующего флипа; **трассы меню весят 33–98 ГБ** (в трассе остаются все созданные за сеанс ресурсы) — удалять после разбора.
- `.porting/work/KyTxMetal.app` читает `.porting/work/extra_env.sh` (RUN_TAG=…, диагностические флаги); `shot.sh <png> [ширина]` — скриншот окна игры; `gd.py` — JSON-обёртка gpudebug.
- gpudebug: ресурсы по имени — `go /resources/textures` + `fetch tex390 --out f.png`; поиск по трассе `find tex394`; у compute-узлов `buf[0]`/`tex[0]` повторяются, адресовать по порядку нельзя — смотреть через `/api_calls` и `find`.
- Computer use: `~/.claude/computer-use.lock` может остаться от старой сессии (перенёс в `.bak-20261025`); macOS-права Accessibility/Screen Recording после обновления приложения надо снять/выдать заново и перезапустить Claude.

**Открыто.**
1. Шум (чёрные точки/полосы) и смаз у текста в «Training Hub» — трасса `htile/frame10280.gputrace` (98 ГБ) снята для разбора.
2. Превью-картинки в меню (баннеры/иконки) — цветной шум: текстуры декодируются неверно (не проверено).
3. Матч: проверить сцену и измерить FPS с HTILE-фиксом и объединением проходов; сохраняется перемежающийся guest-fault при загрузке матча.
4. Убрать `KYTY_EOP_CLOCK_FREEZE`-обход через честную эмуляцию GPU-времени; EA-интро по-прежнему не воспроизведено.

## Чистка и косметика (вечер 2026-10-25, по просьбе пользователя)

- Удалено (всё пересобирается/пересоздаётся): `kytyps5-ufc5/_Build` (78 ГБ) и `kytyps5-ufc5/runs` (25 ГБ) — старая Kyty/UFC5-сборка и логи прогонов (сам проект и `.ledger` UFC5 сохранены); все старые GPU-трассы (`captures/fc27-20261025/{intro300,menu-scene,play,ui-mip-tail,ref}`, включая Vulkan-эталон `ref/vk_f*.gputrace` — упоминания этих файлов в более ранних документах устарели; эталон снимается заново запуском `KyTxReference.app`) и `validation-20261023/24`. Оставлена одна матчевая трасса `captures/fc27-20261025/htile/frame9600.gputrace` (96 ГБ, текущая сборка).
- Заголовок окна на macOS теперь `KyTx | <игра> (<TITLE_ID>) | frame: N, fps: N` (+ статус F9-захвата), без «Source build … | Release»; код: `WindowContext::UpdateTitle` в `window.cpp`.
- Прецедент из `kytyps5-ufc5/ufc5/ledger.md` (UFC 5, «чёрный раунд»): 1×1 текстура авто-экспозиции никем не записывалась (читалась как 1e-19), сцена умножалась на ~1e-16 → NaN в temporal-upscaler → чёрный кадр; чинили инициализацией в текстурном кэше. Для FC27 в матче проверяется тот же класс причин (экспозиция/пустые 1×1 входы пост-обработки).

## Операционные заметки

- **Фокус окна**: на этой машине параллельно работает Wine/Highball (процесс `wine` перехватывает фокус) — клавиши через CGEvent/System Events ненадёжны. Используйте `KYTY_AUTO_INPUT` (внутри эмулятора). Чужие процессы не трогать.
- Скрипты `tools/` (`variant2.sh <tag> <orig|fresh> "<ENV…>" <сек>`, `freshstate.sh`, `origstate.sh`, `press.sh`, `timeline.sh`) прописаны на scratchpad этой сессии и пути `~/Developer/fex-spike/runs`; при переносе поправьте переменную `S`. Враппер-приложения `tools/KyTxMetal.app` (лёгкие флаги + `extra_env.sh`) и `tools/KyTxReference.app`.
- `_SaveData` в `fex-spike/runs`: `_SaveData.pre-claude-20261025` (копия состояния на начало экспериментов, уже изменённая ночными прогонами; «нулевое» состояние не сохранилось), `_SaveData.fresh-claude` (сценарий первого запуска), `_SaveData` восстановлен из pre-claude. См. `_SaveData.README-claude.txt`.
- Трассы: `captures/fc27-20261025/ref/{vk_f760,vk_f1700}.gputrace` ≈ 17 ГБ; все мои `gpudebug`-сессии закрыты (`gpudebug -l` пусто).
- Эмулятор остановлен. Артефакты: `.porting/artifacts/2026-10-25/stencil-fix/`.
