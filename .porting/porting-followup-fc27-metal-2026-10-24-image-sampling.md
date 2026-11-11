# FC27 native Metal follow-up — 2026-10-24: image sampling и актуальные GPU-трассы

## Текущее состояние

Native ARM64 **FEX + direct RDNA→MSL**, `KYTY_MSL_ONLY=1`, без Rosetta и Vulkan renderer. Репозиторий `/Users/tx/Developer/KyTx`, ветка `metal-native-checkpoint`, HEAD `c30d5d81ab23`. Коммитов нет; сохранить весь dirty/untracked tree.

Цель **не завершена**: интро/значки/эффекты/сцена ещё имеют дефекты, корректный матч с измеренными 30+ FPS не достигнут. FPS красного экрана не является результатом производительности игры.

Предыстория: [handover 17:38](porting-handoff-fc27-metal-2026-10-24-1738.md) описывает сохранённые guard ReadFirstLane, KYTY_LOOP_CAP и signposts. [Handover логотипа](porting-handoff-fc27-metal-2026-10-24.md) содержит подробный метод Xcode и доказательство flat/no-perspective фикса; его не откатывать. Более ранние утверждения «A/B не запускался» и «последняя трасса пустая» относятся к старым попыткам.

## Новое исправление: MslEmitter.cpp

1. **Слой sample/gather texture array** раньше передавался как `uint(AddressU32(...))`: float1.0 превращался в индекс1065353216 и выбирал последний слой. Теперь `SampleArrayIndex` читает float/half через AddressF32, округляет `rint` и ограничивает индекс размером массива перед преобразованием в uint. Load/store остаются с целочисленными адресами; cube path не менялся.
2. **Производные texture2d_array/texture3d** раньше отклонялись как `non-2D derivatives`. Теперь передаётся array index и gradient2d либо gradient3d. 1D/MSAA derivatives по-прежнему явно unsupported.
3. **Gather component** раньше числом попадал в аргумент offset, и зелёный gather возвращал красный. Теперь передаются `int2(0), component::x/y/z/w`; существующее применение динамического packed offset сохранено.

`tests/metalMslTests.mm::ImageSampling()` исполняет 13 реальных GPU случаев: различные слои/границы/округление, explicit LOD, 2D/array/3D gradients, gather R/G/B/A. Каждый mip и слой имеет различимое RGBA32Float значение.

- До правки: **13 cases / 9 failures**.
- После: **13 / 0**, API + shader validation включены. Полный native MSL suite тоже проходит.
- Основной ARM64 emulator пересобран, `git diff --check` проходит.
- [До](artifacts/2026-10-24/resume-image-sampling/image-sampling-before.log), [после](artifacts/2026-10-24/resume-image-sampling/image-sampling-after.log), [build](artifacts/2026-10-24/resume-image-sampling/build-image-sampling.log), [fingerprints](artifacts/2026-10-24/resume-image-sampling/build-fingerprints.json).

[Focused review patch](artifacts/2026-10-24/resume-image-sampling/image-sampling.patch) отделяет новые изменения от ранее dirty tree. Это проверяет перечисленные float texture случаи, не весь набор image instructions, packed A16 или все integer texture форматы.

## Runtime и MRT A/B

Полные логи и JSON находятся в `/Users/tx/Developer/fex-spike/runs/validation-20261024/`. Компактные результаты и флаги сохранены [рядом](artifacts/2026-10-24/resume-image-sampling/runtime-results.json).

| Run | Код/режим | Последний DrawStats | Результат |
| --- | --- | --- | --- |
| mrt-gui-0-174650 | До image fix, MRT0 |1625| Heavy red, dropped_ps25, GPU error strings0 |
| mrt-gui-1-174838 | До image fix, MRT1 |1875| Heavy red, dropped_ps25, активны6–7RT, GPU error strings0 |
| mrt-gui-1-181054 | После image fix, MRT1 |2575| Heavy red, dropped_ps0, GPU error strings0 до попытки capture; SIGBUS во время capture |
| mrt-gui-1-181658 | После image fix, MRT1, LLDB |2300| Heavy red, dropped_ps0, GPU error strings0; capture успешен; окно дошло до2429, затем Cmd-Q |

Счётчики DrawStats и frame в заголовке окна различаются; не смешивать их. Это диагностические запуски с подробным логом, не FPS benchmark. Начало heavy scene зависит от ввода; запуск181054 вошёл в неё раньше, чем повтор181658, поэтому одинаковые номера кадров не означают matched сцену.

Проблемный PS raw guest **69484ce5ea9fbf78** раньше давал unsupported native68c638fce0a6badf. После исправления компилируется **2b740773366a9312**, содержит texture2d_array sample с gradient2d. Пропуск этого PS исчез. [Исходник](artifacts/2026-10-24/resume-image-sampling/array-gradient-ps-2b740773366a9312.metal).

В повторе181658 через computer use визуально проверено корректное окно ошибки EA Servers на фоне (frame1282), затем J/ОК и тяжёлая красная сцена. В A/B до image fix J также вводился через CUA. Red остаётся при MRT0 и MRT1. Прежний Hang не повторился в этих ограниченных запусках; это поддерживает сохранённые исправления17:38, а не доказывает стабильность всех сцен.

Сохраняются unsupported compute GDS, compute cube opt-in и formatted StoreBufferU32 desc_format0. Их не включать/не маскировать ради цифры FPS.

## Две валидные новые single-frame трассы

Каждый захват: boundary3 CAMetalLayer, **count1**, shared-mem-opt disabled. Флаги disable-unused/backtrace не применялись.

1. `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261024/mrt-current-red-one.gputrace` (~17GiB, ДО image fix): **19CB /497draw /246dispatch**. CLI ready, Xcode Replay успешен. В этом продолжении именно эту актуальную относительно ring/cache/flat/17:38 фиксов трассу использовали для повторного vertex debug.
2. `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261024/image-sampling-fixed-debug-one.gputrace` (~15GiB, ПОСЛЕ image fix): **22CB /811encoder /720draw /252dispatch**,19316resources. CLI ready, Xcode Replay успешен, Xcode Summary266render/335compute/210blit. [Workload](artifacts/2026-10-24/resume-image-sampling/fixed-workload.log).

Первая попытка после image fix `image-sampling-fixed-one` оборвалась0/1, файл отсутствует, process9959 завершился SIGBUS (launchd termination2,10,4). Это не GPU Hang в runtime логе; причина именно этого обрыва не установлена. Не выдавать её за валидную трассу.

Повтор под LLDB успешен. FEX нормально использует SIGSEGV/SIGBUS в JIT; для диагностики сначала выставили `platform.plugin.darwin.ignored-exceptions=EXC_BAD_ACCESS` **до attach** и пропуск SIGSEGV/ILL/USR1/USR2. Stop hook [capture_fault_hook.py](artifacts/2026-10-24/resume-image-sampling/capture_fault_hook.py) продолжал остановки в диапазоне JIT0x7000000000..0x9000000000, оставляя native faults. Это только диагностический фильтр, не исправление SIGBUS и не доказательство причины первого обрыва. LLDB detached/quit после capture.

## Красный слой: факты, а не исправление цвета

### Xcode до image fix, но после остальных сохранённых фиксов

В `mrt-current-red-one`: **CB16 / render64 / API14882**, Triangle6. Geometry → Debug vertex0 → main0 → kyty_vs_impl → строка присваивания `v10`.

`ab.buffers[2]` начинается `2566914303 = 0x990000ff`; UNORM8 выдаёт `(1,0,0,.6)`. Все6 post-VS вершин красные. Это подтверждение на новой трассе, а не только на старой shared-fixed-one. VS guest b2c4c2e311c5bc76, PS guest2ae1ba74c42662f1.

### Трасса ПОСЛЕ image fix

- Final present: CLI **cb21/re0/draw0**, вход @tex95 / MTLTexture96 RGB10A2.
- Composite: **cb18/ce434/disp0**, API33050, CS7615e9bfcae5c55a. Вход scene @tex1180 / MTLTexture1181 (1600×904 RGB10A2) уже содержит tiled/полосатый мусор; UI @tex1024 / MTLTexture1025 (1920×1080RGBA8) красный. Это две отдельные наблюдаемые проблемы до presentation.
- Первый UI draw: **cb18/re30/draw0, API25195**, XcodeCB19/render64, TriangleStrip4. Все4 исходных цвета **ffffffff**, UV0..1. Текстура3840×1080 @tex1270 — BGRA-swizzled view родителя @tex1269. PNG fetch view и parent содержит одинаковые raw pixels, поэтому swizzle отдельно применён в GPU probe; не считать PNG доказательством применения view swizzle.
- Найден красный draw **cb18/re40/draw0, API25390**, Triangle6, PSOrps19 / MTLRenderPipelineState20. VSlib62, PSlib61, нормальный SourceAlpha/OneMinusSourceAlpha, depthAlways/noWrite.
- Его6 исходных цветов **990000ff**; UV **(-65536,-65536)** — sentinel сплошной заливки. VS преобразует его в param0=(0,0,1,0), param1=0, param2=(1,0,0,.6). Следовательно текстурная заглушка не объясняет красный RGB этого draw: PS использует цвет вершин.
- Native VS в этих запусках **bc0210100fe76732**, PS **ae49c6a306380213**. Native keys получать из текущего `[MSL] variant ... from shader ...`, старые2b83/c745 не hardcode. Пробный KYTY_PROBE_HASH2b83 в запуске181658 поэтому не дал целевых логов.

Argument buffer этой красной операции сохранён [ui-red-args.bin](artifacts/2026-10-24/resume-image-sampling/ui-red-args.bin). Buffer2 GPU VA **0x107bf885470**, backing @buf751 DynamicRingBuffer GPU base **0x107bf328000**, offset **0x55d470**. **Это GPU VA, не гостевой CPU адрес0x819...**; для CPU watchpoint нужно сначала получить guest V# address в runtime.

### Изолированное воспроизведение на GPU

Сохранены точные MSL, inline VS args/sizes и содержимое ring. [repro.sh](artifacts/2026-10-24/resume-image-sampling/repro.sh) восстанавливает входы в tempdir, компилирует ObjC++ probes и запускает Metal API + shader validation, без игры/FEX/Vulkan.

- VS compute probe вызывает тот же helper/quad entry и показывает: первый фон покрывает NDC x[-2,2], y[-1,1], цвет white/alpha1; красный draw действительно выводит red/.6. В архивном повторе проверены все6 красных вершин.
- Render probe использует реальные VS+PS и stream/constant bytes. **Background draw на заранее красном RT** даёт центр **(0,1,1,255)**, точно source texel с BGRA swizzle: он способен полностью перезаписать фон.
- Red draw на прозрачном чёрном RT даёт **(153,0,0,153)**. Для него оставлена конечная входная текстура background: UV sentinel отключает её влияние на цвет; это проверка solid-color ветки, не точная копия всех ресурсных bindings красной операции.
- Каждый render повторён с MTLArgumentEncoder и с raw pointer/resource-ID array: **4 GPU render cases /0failures**. Это проверяет ABI/логику этих shaders в изолированном случае, не правильность всех runtime bindings.
- [Полный repro log](artifacts/2026-10-24/resume-image-sampling/repro.log), [исходники](artifacts/2026-10-24/resume-image-sampling/trace-render-probe.mm).

Не считать `MTLLoadActionLoad` доказанной первопричиной. Сам по себе корректный непрозрачный background draw должен перезаписывать фон; связь с runtime порядком/скрытым scissor/следующими draw и число красных заливок ещё требуют проверки. Не включали принудительный Clear, не заменяли цвета и не делали широких snapshots/alias reuse.

## Ловушка gpudebug

В vertex/fragment bindings бывает несколько строк `buf[0]`: inline args2048bytes и косвенные resident buffers. `fetch buf[0]` выбирает первую строку, иногда это64KB default buffer либо32MB ring. **Всегда проверять размер**; такой файл нельзя декодировать как argument buffer. В `--json go .../vertex` выбраны только операции, где первый buf[0] — inline. Удалось корректно прочитать69 из201UI draws; [ui-colors.json](artifacts/2026-10-24/resume-image-sampling/ui-colors.json). API25390 красный среди проверенных, но это не доказательство, что он первый красный во всей трассе.

## Следующий шаг и процессы

1. Целевой CPU-side probe nativeVSbc0210100fe76732, resource2/color990000ff: сопоставить V# guest address и `TryReadBacking` **до SnapshotBytes** с bytes, реально скопированными в ring. Имеющиеся KYTY_LOG_DESC/KYTY_TRACE_VB ограничены первыми2/12 случаями, могут поймать только белый фон; для точной проверки нужен ограниченный фильтр hash/resource/word0. Не расширять snapshot до256KB.
2. Проверить guest V# DST_SEL/формат и источник CPU записи, прежде чем объявлять FEX повреждение или менять цвет. UV sentinel делает этот draw сплошной заливкой.
3. Для независимого scene corruption проследить writer/alias @tex1180; это не финальный presenter. Старый broad latest-writer reuse уже приводил к Hang — не возвращать без тестов dimension/layout/usage.
4. Только после корректной сцены измерять playable match FPS.

Эмулятор, LLDB и gpudebug завершены. Последнее наблюдение Xcode: новая after-image-fix трасса успешно replayed, выбран API25195 /CB19/render64; GPU workload оставался открыт. После смены подключения CUA tool недоступен, дальнейший анализ выполнен gpudebug/standalone Metal. Текущее GUI состояние после этого не перепроверялось; новые действия Xcode не выдумывать.

## Снимки500..1000 по запросу пользователя (19:22)

**Уточнение пользователя:** требовались Xcode `.gputrace`, не PNG. После этого сняты и проверены все6 одиночных GPU-трасс; актуальные файлы, retry1000 и Xcode Replay описаны в [продолжении2026-10-25](porting-followup-fc27-metal-2026-10-25-gputrace-series.md). Следующий абзац — история раннего PNG-прогона, не результат GPU capture.

В обоих launch paths KyTxDiagnostic.app сохранён KYTY_DUMP_FRAME=500,600,700,800,900,1000. Backup прежних внешних launchers: `/Users/tx/Developer/fex-spike/runs/validation-20261024/launcher-backup-frames500-1000-192234`. Запущен MRT1 run `mrt-gui-1-192234`, PID14595. Все6 PPM успешно прочитаны полностью, PNG1920×1080 и обзор/manifest находятся в `/Users/tx/Developer/fex-spike/runs/validation-20261024/mrt-gui-1-192234-frames/`.

Визуально:500 — FC27 logo/background;600/700 — EA connecting dialog;800 — начало красной сцены с music overlay;900/1000 — почти полностью красный экран. Это конкретный run, не одинаковая стадия игры по тому же frame number в других запусках. К концу подготовки PNG процесс14595 уже отсутствовал; агент не отправлял команду закрытия в этом запуске.
