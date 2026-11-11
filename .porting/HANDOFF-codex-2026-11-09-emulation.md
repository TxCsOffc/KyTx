> **Более новое продолжение, 2026-11-09:** Claude продолжил эту сессию, затем Codex завершил следующий пакет реальных upstream-исправлений. Текущая сборка/игровой PID/проверки и очередь — в `.porting/STATE.md` и `.porting/REPORT-claude-continuation-2026-11-09.md`. Ниже сохранён прежний хендофф как историческое состояние.

# Хендофф: продолжить реализацию эмуляции KyTx

Срез 9 ноября 2026, 03:09 Asia/Tashkent. Следующему агенту: продолжать исправлять реальные контракты эмуляции, начиная с runtime coherence физических диапазонов. Каждую законченную часть сначала проверять автономными программами на CPU/Metal; затем собирать и запускать игру. Не подменять реализацию маскировкой изображения.

## Текущее состояние и решения пользователя

- Репозиторий: `/Users/tx/Developer/KyTx`; ветка `metal-native-checkpoint`; HEAD `9534b33718bd10104a4baf708758d91f61e2ef8d`.
- Есть много прежних и новых незакоммиченных изменений. Ничего не коммитили и не публиковали. Не делать reset/clean и не откатывать чужие hunks. Новые исходники и тесты частично untracked — это существенная часть работы, их нельзя потерять.
- Пользователь требует сначала выяснять отсутствующие/слабые реализации и реализовывать по зависимостям. Заглушки, очистка NaN в TAA, пропуски шейдеров, подмена ресурсов ради чистой картинки отвергнуты. Наш эксперимент TAA sanitizer **полностью удалён из исходников и бинарей**; старый план Claude «продолжаю делать защиту TAA» больше не является разрешением.
- Пользователь дважды уточнил: мозаика, крупные блоки освещения и прочие знакомые артефакты были **до наших изменений 3D** и сохраняются сейчас. Не считать их новой регрессией по одному кадру после сборки. Связь каждой правки с конкретным дефектом доказывать отдельно.
- Последнее разрешение: «продолжай и в конце запускай». Игровой запуск после мини-тестов разрешён; повторно спрашивать это не нужно. Только один эмулятор одновременно, остановка — по проверенному собственному PID.
- Не отправлять игровые assets, дампы, screenshots или shader captures в сеть. `.gputrace` не снимать: прежние захваты занимали 25–40 ГБ. Не обещать FPS gain по синтетике или одному игровому прогону.
- `AGENTS.md` требует Ledger и обновления STATE. В этом checkout нет ни `ledger` в PATH, ни `.ledger`; это проверено. Состояние сохраняли в `.porting/STATE.md`, `HANDOVER.md`, `.porting/WORKLOG-anomalies-2026-11-08.md`.

**Процессы сейчас:** при проверке для хендоффа PID 97150 и других `kyty_emulator* --game` не найдено. Ранее игру оставили работающей по просьбе пользователя; в этапе записи хендоффа её не останавливали. Причина/код завершения не установлены. Старые записи «PID 97150 работает» — исторические; перед любым запуском заново проверить процессы.

## Что действительно реализовано

| Часть | Изменение и граница готовности |
|---|---|
| MSL buffer stride swizzle | Добавлено AMD-преобразование адресов для групп индексов 8/16/32/64, scalar/instruction offsets, отдельных компонентов vector load/store, formatted stores, subword loads и atomics. Non-swizzled строки генерации сохранялись, чтобы не инвалидировать весь shader cache. Логическая NumRecords/record-OOB семантика ещё требует отдельного набора проверок. |
| Tiled volumes | GPU import/export Standard4KB3D/Standard64KB3D/Prt64KB3D для элементов 1/2/4/8/16 байт; вклад z, slabs, reversed mip offsets и shared thick mip-tail. Большие lookup tables передаются MTLBuffer вместо `setBytes` >4 КиБ, с lifetime до completion. |
| 3D CPU-owned upload | `GuestTexture3D` создаёт запрошенную mip-цепочку и загружает всю tiled chain, включая сохранённый packed-float widen путь. Это настоящий runtime loader. Автоматические переходы живых GPU-owned 3D representations ещё не подключены. Кэш по адресу и прежняя ownership policy сохранены; linear 3D input ещё пропускается. |
| Физическая identity views | Buffer-backed RT, `GpuSurfaceFor` и обычные linear `GuestTexture` получают backing через общий `MetalBufferManager`. Reuse проверяет buffer/offset/row/format/extent/span; remap не должен оставлять старую физическую страницу в этих путях. Mapping generation участвует в совместимости открытого render pass. Это ещё не инвалидация всех private/mip/depth/BC caches. |
| PM4 parsing/order | Разбираются legacy/GFX10/AGC acquire и аппаратный/AGC release, подключён native `IT_RELEASE_MEM`. GCR acquire/release имеют разные раскладки. Сохраняются raw controls, ranges, sentinel, label/value/context. Metal MemoryBarrier закрывает активные encoders/pass и overlap group, сохраняя fence frontier, без CPU wait/отдельного submit. **Полных private-image writeback/invalidate действий пока нет.** MES/execute RELEASE явно отвергаются как unsupported. |
| `MemoryCoherence` | Готово самостоятельное C++ ядро owners/versions физических subranges: планы актуальных переносов, partial-write preservation, stale-plan rejection, dirty-eviction refusal, lifetime IDs без повторного использования. Оно проверено на CPU и с настоящими GPU-переносами. **Всеми runtime caches эмулятора оно пока не управляет.** |
| Runtime tiled alias conversion | `AliasConvert` больше не начинает scratch с нулей. `EncodeTileAlias` копирует настоящие guest bytes, накладывает видимые texels writer в его tile layout и импортирует по layout reader. Guest/source не изменяются, padding и reader-only bytes сохраняются. Конверсия переиспользуется только внутри одного draw/dispatch через `s_image_read_epoch`, чтобы следующий command увидел новые гостевые байты вне writer footprint. Depth/BC staging blits включены в Order fences. |

Важное ограничение последней правки: внутри footprint частной writer-текстуры источником истины по-прежнему считается writer, выбранный старой политикой. Более новая raw-buffer/CPU запись туда ещё не согласована глобальным трекером. Сохранение байтов вне footprint — завершённое локальное исправление, а не полная coherence.

## С чего продолжить

Первый незаконченный этап — **подключение физических owners/versions к рабочим переходам buffer ↔ texture**, включая CPU writes и mapping lifetime. Не начинать с косметического TAA или массовой смены ключей кэша.

1. Выбрать ограниченный, реально используемый путь: например, один mip uncompressed 2D storage image и raw buffer той же физической памяти. Сделать runtime adapter и использовать его и в эмуляторе, и в мини-программе. Не завершать этап одним новым helper, который применяется только тестом.
2. Идентифицировать данные через physical backing + offset + lifetime, а не только vaddr, MTLTexture pointer, flip или «последний bind». Точки входа ядра памяти: `TryGetGuestBackingOffset`, `GetGuestBackingMappings`, `GetGuestBackingGeneration` в `src/kernel/memory.h`. Snapshot mappings может вернуть false, когда generation не изменился; учитывать его контракт.
3. Описать точное покрытие texels/blocks. Padding и соседние allocations не принадлежат writer автоматически. Потенциальный writable descriptor не доказывает, что shader записал каждый байт. При консервативном footprint частичная запись требует актуального prefill (`PreserveUnwritten`); `OverwriteRange` допустим для известной полной записи заявленного диапазона.
4. До чтения разрешать только нужные устаревшие ranges, кодировать реальные import/export/copies с GPU ordering и удержанием ресурсов. Не экспортировать всю старую texture поверх более новой частичной CPU/raw записи. Перед удалением dirty owner материализовать актуальные байты; `ForgetView` специально отказывает, когда исчезла бы единственная актуальная копия.
5. Подключить raw writers, DMA/fills, storage writers, render-target writes, CPU dirty и remap/lifetime. Разобрать способ точного обнаружения CPU writes: native Metal сейчас обходит page protection, а проверка больших texture copies хэширует только четыре окна по 512 байт. Это эвристика, не точная инвалидация. Не считать acquire объявлением «CPU полностью перезаписал весь диапазон», не выполнять тяжёлый Metal wait/lock из signal handler.
6. Уточнить каноническую GPU identity для диапазонов, пересекающих границы 64 МиБ backing chunks. `GetBufferForGuestRange` для таких ranges может идти в отдельную page-wrapper ветку. Нельзя закрыть вопрос простым увеличением chunk size или утверждением, что физически одинаковые страницы автоматически когерентны между разными Metal objects.
7. Передать decoded acquire/release cache actions адаптеру памяти. Различать GPU order, copy visibility и CPU completion. Адрес label в RELEASE — **не диапазон flush**. После необходимых GPU writebacks публиковать EOP label/interrupt в правильном порядке; проверить production EOP/interrupt path, а не только completion callback в мини-тесте.

Критерий закрытия первой runtime части: одни и те же adapter/copy routines используются продуктом и мини-тестом; CPU→GPU, GPU image→raw buffer, raw buffer→image, GPU→CPU после release, два vaddr одного backing, частичный overwrite, remap, mixed owners и eviction проходят побайтную проверку, включая нетронутые bytes/guards. Для асинхронной записи не вводится глобальный CPU wait на каждый PM4 пакет.

Контракт `MemoryCoherence` принципиален: metadata описывает encode order, а не завершение GPU. `CompleteRead` после кодирования copy не разрешает CPU сразу читать destination. CPU дополнительно ждёт соответствующего completion. `Read` удерживает mutex во время copier callback; callback не должен повторно входить в tracker. Version 0 — существующие гостевые bytes, **не нулевая инициализация**. Внешний adapter обязан сериализовать фактический доступ к данным, удерживать source objects и проверять ошибки transfers.

Две разные no-copy обёртки физических aliases в отдельном эксперименте дали GPU consumer `00000000` вместо `b9f90000`, хотя после завершения CPU видел producer values `12340000/abcd0000` по обоим адресам. С каноническим Metal backing тест проходит. Это доказательство необходимости правильной identity в этом тесте; причина игровых артефактов этим не доказана.

## Очередь после runtime coherence

| Порядок | Следующая реализация |
|---:|---|
| 2 | DCC/CMASK/HTILE: block state, fast clear, partial coverage, сохранение untouched tiles и настоящие eliminate/decompress transitions. Сейчас `DrawAuto` при CB modes 2/5/6 возвращает без полного metadata transition; HTILE clear выводится из некоторых writes; CMASK touched-marking заполняет слишком широкий диапазон. Обычный PS draw не является заменой metadata operation. |
| 3 | Linear 3D row/slice pitch, CPU/GPU ownership и 3D aliases/mips; точные форматные преобразования вместо unsupported-format substitutes. Не заводить раздельные GPU объекты по полному layout key без согласования владельцев. |
| 3 | Sampler compare: все функции и compare-before-filter/PCF/gather; border/MirrorOnce modes, LOD. Исторический compare experiment проходил 256 GPU значений, но был откачен и не является текущей реализацией; его наличие не доказывало устранение визуальных дефектов. См. `.porting/artifacts/2026-11-08-codex-lighting/`. |
| 4 | MSL VS/PS builtins PointSize/CullDistance/Layer/ViewportIndex/SampleId; независимые wave64/LDS/divergence/barrier tests. Это не утверждение, что wave64/LDS полностью отсутствуют. |
| 5 | ARM64 guest FiberSave/Restore через FEX guest context, затем guest PthreadExit unwind/cleanup/join. Не сохранять host ARM64 registers вместо guest x86 context. |
| 6 | HLE/AJM codecs и фактически отсутствующие guest API. Слово stub в ABI bridge не является доказательством функции-пустышки. |

Подробный исходный аудит: `/Users/tx/Developer/KyTx/.porting/AUDIT-emulation-coverage-2026-11-09.md`. Его старые строки «CpOpAcquireMem игнорирует payload» относятся к состоянию **до** текущего пакета; parsing/order уже реализованы, resource coherence остаётся открытой.

## Точки входа в код

| Файл под `/Users/tx/Developer/KyTx/` | Что читать |
|---|---|
| `src/graphics/host_gpu/memoryCoherence.h` | RegisterBacking/RegisterView, PlanRead/CompleteRead/Read, RecordWrite, ForgetView; чистое ядро, ещё без полного runtime adapter. |
| `src/graphics/host_gpu/metal/metalRenderer.mm` | `RecordComputeStorage`, `LastGpuWriterTexture`, `GpuSurfaceFor`, `AliasConvert`, `s_surface_conv`, `s_image_read_epoch`, `MemoryBarrier`, `GuestTexture`, `GuestTexture3D`, PrepareGraphicsImages/PrepareGpuDetiledSources, storage bindings и raw-buffer writers. Большой файл; читать по функциям. |
| `src/graphics/host_gpu/metal/metalBuffer.h/.mm` | `GetBufferForGuestRange`, `GetBackingChunk`, 64 MiB chunks и page-wrapper fallback. Не откатывать прежние исправления менеджера. |
| `src/graphics/host_gpu/metal/metalGuestMemoryView.h` | CreateGuestMemoryView/GuestMemoryViewMatches и mapped span. |
| `src/graphics/host_gpu/metal/metalTiling.h/.mm` | EncodeTileAlias, EncodeTileImport/Export, volume APIs, CPU mip decode/upload, SetTable. |
| `src/graphics/host_gpu/metal/metalOrder.h` | OrderBegin/End, overlap frontier/group, OrderMemoryBarrier. |
| `src/graphics/guest_gpu/command_processor/pm4MemorySync.h` | Декодер PM4/GCR, native/transformed packets, range sentinel и release context. |
| `src/graphics/guest_gpu/command_processor/pm4Handlers.cpp`, `pm4Dispatch.cpp`; `src/graphics/guest_gpu/graphicsRun.cpp` | CpOpAcquireMem/CpOpReleaseMem, native dispatch, CommandProcessor::AcquireMem/EmitGlobalBarrier. |
| `src/kernel/memory.h/.cpp`, `memoryAddressSpace.inc`; `src/graphics/host_gpu/pageManager.cpp` | Physical mappings/generation и native Metal protection bypass. |
| `src/graphics/shader/recompiler/backend/msl/MslEmitter.cpp` | BufferByte/BufferDword/BufferSwizzled, vector and formatted access; sampler/builtins. |

## Проверки и пределы доказательств

Все положительные проверки ниже прошли **до** последнего игрового запуска. При записи хендоффа исходники эмулятора не меняли и игру не перезапускали.

| Набор | Результат |
|---|---|
| PM4 | 266 240 GCR combinations, поля трёх acquire и двух release encodings, ranges/sentinels/overflow/truncation — PASS. Это не полноценная end-to-end проверка всех production interrupts. |
| CPU coherence | 20 000 операций с независимым плотным побайтным эталоном, stale plans/eviction/lifetime/bounds — PASS; ASan/UBSan — PASS. |
| GPU coherence | CPU→GPU, differently tiled GPU→GPU, GPU→CPU через completion label, aliases, partial writes/padding — PASS. Реальный overlap group + explicit barrier — PASS; no-op barrier negative control ожидаемо FAIL. |
| Runtime alias helper | 28 layout/format pairs; 497 233 writer bytes + 2 189 447 guest-only reader bytes; весь scratch/output, source/guest preservation, nonzero padding/guards/bounds и повторное чтение после CPU byte update — PASS. Старый zero-fill negative control ожидаемо FAIL. Metal API Validation — PASS. |
| Views/remap | Старый wrapper остаётся на старой физической странице, новый view читает/пишет remapped backing; identity/offset/stride/extent/format/span — PASS. |
| Tiling | 58 2D cases, 8 mip chains, 30 volumes/133 GPU mip round-trips — PASS, включая native-parent reads после uint-view upload. CPU-эталон адресации — проектный `tile.cpp`, не независимое железо Sony. |
| Native MSL | Полный suite — PASS, включая 4806 buffer swizzle results и побайтный контроль всех изменённых buffers. |
| Build | `kyty_emulator` — PASS; deprecated API/linker warnings остались; whitespace check PASS. |

Команды из репозитория:

```bash
cd /Users/tx/Developer/KyTx
bash tools/test_emulation_memory_stage.sh
MTL_DEBUG_LAYER=1 bash tools/test_metal_coherence.sh
cmake --build _Build/macos-arm64 --target kyty_emulator -j8
```

Первая команда запускает только синтетические программы. Подкоманды: `test_pm4_memory_sync.sh`, `test_memory_coherence.sh`, `test_metal_guest_memory_view.sh`, `test_metal_coherence.sh`, `test_metal_tiling.sh`, `test_metal_msl.sh` в `tools/`.

Отрицательный контроль прежнего alias алгоритма:

```bash
KYTX_ALIAS_TEST_ZERO_BASE=1 bash tools/test_metal_coherence.sh
# Ожидается exit 1: alias conversion lost guest bytes outside writer footprint.
```

Основные fixtures: `tests/MemoryCoherenceTests.cpp`, `tests/Pm4MemorySyncTests.cpp`, `tests/metalCoherenceTests.mm`, `tests/metalAliasPreservationTests.inc`, `tests/metalGuestMemoryViewTests.mm`, `tests/metalMslBufferSwizzleTests.inc`, `tests/metalTilingVolumeTests.inc`.

## Последняя сборка и игровой запуск

`_Build/macos-arm64/kyty_emulator` и `kyty_emulator_s3` побайтно совпадают. SHA-256 обоих при хендоффе:

```text
8023ff7020033d9aeca3380533c208578d7c0cbeb9d42248a4725672577613e7
```

Запуск `rm1-20261109-025507`, 02:55:07 Asia/Tashkent, бывший PID 97150. Лог: `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/rm1-20261109-025507-025507.log`.

Во время непосредственной проверки примерно первых четырёх минут кадры продолжали идти, GPU fault/progress timeout/command-buffer error не найдено. Log `[Surface]` подтвердил выполнение нового runtime alias пути. Сообщение EA servers unavailable закрыто одной штатной кнопкой confirm. Просмотрен process-local кадр5785: 3D-сцена с персонажем в красной форме, старые дефекты освещения/одежды видны. Только этот визуально проверенный участок можно считать smoke; последующее содержимое длинного лога **не означает**, что полный матч был визуально проверен нами. A/B и FPS gain не установлены. Игра была оставлена пользователю; сейчас процесса уже нет, причина завершения неизвестна.

Launch environment `/Users/tx/Developer/KyTx/.porting/work/extra_env_s1.sh` восстановлен побайтно после временного RUN_TAG. Сейчас там `RUN_TAG=u1`, `KYTY_UMA_SURFACES=1`, `KYTY_MSL_GDS=1`, `KYTY_MSL_CUBE=1`, `MTL_CAPTURE_ENABLED=0`, input file `/tmp/kytx_drive_input`. Не менять набор диагностических флагов незаметно при сравнении.

Стандартный launcher: `.porting/work/KyTxMetalS1.app`; `.porting/work/run1.sh <tag> <seconds>` сам копирует новую сборку в s3 и требует совпадения RUN_TAG в extra_env. `0` означает отсутствие таймера, memory guard остаётся. Сначала прочитать script и проверить единственный процесс; сохранять/восстанавливать env. Временный Python launcher последнего этапа находится в `/Users/tx/Documents/Codex/2026-11-09/new-chat/work/runtime-memory/launch_game.py`. В нём исправлен поиск процессов: macOS обрезает `ps comm`, нужно смотреть первый token `args`. Не слепо перезапускать его ради чтения хендоффа.

Для локальной проверки кадра без записи рабочего стола: при работающей игре создать `/tmp/kytx_f9/dump`; текущий launcher задаёт game-owned вывод `/tmp/kytx_f9/frame-%llu.ppm`. Text PassTrace — marker `/tmp/kytx_f9/pass`. **Не использовать `.porting/work/ftrace.sh` без переработки:** он создаёт `/tmp/kytx_f9/cap` и пытается записать запрещённый тяжёлый GPU capture. Для текущей работы cap marker не нужен.

## Материалы и восстановление контекста

В проекте:

- `.porting/REPORT-runtime-memory-2026-11-09.md` — последние runtime исправления и smoke.
- `.porting/REPORT-memory-core-2026-11-09.md` — PM4/core, contracts и отрицательные контроли.
- `.porting/DESIGN-memory-apple-2026-11-09.md` — физическая identity/remap и исходное исследование памяти.
- `.porting/AUDIT-emulation-coverage-2026-11-09.md` — очередь пробелов с актуализирующим указателем сверху.
- `.porting/WORKLOG-anomalies-2026-11-08.md` — история. Читать новые записи прежде старых выводов.

Пакеты текущей работы в `/Users/tx/Documents/Codex/2026-11-09/new-chat/outputs/`: `memory-backing-fix.patch`, `emulation-first-implementations.patch`, `emulation-memory-stage.patch`, `runtime-memory-implementations.patch`, соответствующие отчёты и synthetic-test logs. Это последовательные **дельты к сохранённым dirty baselines**, не полный patch от чистого HEAD. В текущем checkout изменения уже применены; повторный `git apply` не делать. Там же `runtime-memory-game.png` — просмотренный кадр5785; не выдавать его за чистую картинку или полный матч.

Baselines/промежуточные файлы: `work/claude-continuation/before/`, `work/emulation-coverage/before/`, `work/coherence-stage/before/`, `work/runtime-memory/before/` под тем же Codex workspace. Свежий `handoff-source-state.txt` рядом с этим хендоффом содержит git status и hashes существенных dirty/untracked source/test файлов. Сохранять существующие исправления, даже когда их происхождение не относится к этому пакету.

Исходная сессия Claude найдена в `/Users/tx/.claude/projects/-Users-tx/52cdae17-1a05-40ea-912d-91012d435db7.jsonl`. Это исторический контекст, его старые планы не отменяют текущих инструкций пользователя. Ранее её auto-resume на03:10 не был отменён: управление Terminal было отклонено CUA; обходов не делали. Перед дальнейшими source edits проверить, нет ли конкурентного Claude-процесса/изменений. Не пытаться обходить тот отказ альтернативным UI-управлением.

Рекомендуемая первая операция следующего агента: прочитать этот файл и актуальный STATE, проверить процессы/dirty tree, затем реализовать и проверить один production adapter физических ranges. Продолжать по очереди выше; не объявлять общую память готовой из-за зелёных helper tests.
