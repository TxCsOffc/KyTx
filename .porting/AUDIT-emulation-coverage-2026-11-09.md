# KyTx: пробелы эмуляции и порядок реализации

> Актуализация 2026-11-09: продолжение начинать с `.porting/HANDOFF-codex-2026-11-09-emulation.md`. Ниже сохранён исходный аудит. Строки про no-op acquire относятся к старому состоянию: PM4 parsing/order уже реализованы; физический tracker проверен отдельно, но ещё не подключён ко всем runtime caches. Последний пакет добавил preserving AliasConvert и canonical/remap-aware linear views. Полная coherence и metadata transitions остаются открытыми.

9 ноября 2026. Репозиторий `/Users/tx/Developer/KyTx`, ветка `metal-native-checkpoint`.

Это первый проверенный список по текущему коду, а не заявление о полном аудите всех инструкций и библиотек PS5. Отсутствие реализации, ограничение и недостаток независимой проверки разделены. Количество TODO/return false не используется как процент готовности.

## Уже реализованный первый пакет

### 1. Stride-swizzled адресация буферов AMD в MSL

До изменения `BufferAddressable` отклонял дескриптор с ненулевым stride и установленным swizzle bit. Нативный перевод таких load/store/atomic завершался unsupported. Это **не** swizzle каналов RGBA и **не** раскладка поверхности; это отдельный аппаратный способ адресации записей буфера.

Теперь поддерживаются группы из 8/16/32/64 индексов, смещение внутри записи, instruction offset и scalar offset. Векторные компоненты адресуются по отдельности: соседний компонент swizzled-записи не обязан находиться в следующем физическом DWORD. Исправлены raw vector loads/stores и formatted stores; subword loads и buffer atomics используют тот же расчёт адреса. Нулевой stride сохраняет линейный режим.

Основание: раздел 8.1 [AMD RDNA2 ISA](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna2-shader-instruction-set-architecture.pdf) и существующий независимый SPIR-V расчёт `CalculateBufferAddress`. Используется арифметика uint32, scalar offset добавляется после swizzle.

Проверка: 4806 результатов на реальном GPU, полный побайтный контроль записываемого буфера. Покрыты четыре index-stride, нечётный и нулевой stride, raw/formatted vectors, byte/halfword reads, atomic add и выход за связанный физический диапазон. Регрессионный тест с переводчиком до изменения падает на unsupported. Полный native MSL GPU suite после изменения проходит.

Граница проверки: это реализация адресного преобразования и проверенных операций. Она не доказывает полноту всей RDNA buffer OOB/NumRecords семантики или всех форматов tbuffer. Их нужно проверять отдельно.

### 2. GPU tiling объёмных 3D-текстур

До изменения GPU-конвертер был texture2d-only: таблицы не содержали вклад z, а kernels работали с uint2. Добавлены `EncodeTileVolumeImport` и `EncodeTileVolumeExport`, таблицы адресации z, проход по блокам глубины, обратные смещения mip-уровней и общий thick mip-tail. Реализованы Standard4KB3D, Standard64KB3D и Prt64KB3D для элементов размером 1/2/4/8/16 байт.

API принимает полную раскладку гостевого allocation и отдельно номер гостевого и Metal mip. Проверяются тип текстуры, mip-уровни, размеры и границы buffer allocation. Экспорт меняет только адресованные элементы и сохраняет padding, соседние уровни и защитные области.

Проверка: 30 объёмов и 133 GPU mip round-trip; сравнение с CPU-адресацией каждого уровня и побайтная проверка всего выходного allocation. Включены небольшие объёмы, начинающиеся внутри mip-tail, нечётные размеры и несколько блоков по каждой оси.

Важное ограничение: эталон адресации здесь — существующий `guest_gpu/tile.cpp`. GPU и CPU реализации согласованы, но для заявления о полном соответствии PS5 ещё нужен независимый эталон Sony/AMD для используемого поколения swizzle. Это не полная реализация DCC/HTILE или sparse residency.

### 3. Mip-цепочка CPU-owned tiled 3D-текстуры при создании в эмуляторе

До изменения `GuestTexture3D` описывал гостевую цепочку, но создавал Metal-текстуру с одним уровнем и переносил только level 0. Добавлены декодирование произвольного уровня и `UploadTiledVolumeMipChain`. Этот helper подключён в настоящий загрузчик, а не только в тест.

При создании нового ресурса теперь выделяется запрошенное количество Metal mip-уровней и переносится вся tiled-цепочка. Существующий packed-float fallback также переносит все уровни. Исходная политика владения 3D-кэшем по адресу пока сохранена: разные представления GPU-written memory нужно согласовать общим трекером. Изменять ключ на независимые layout-объекты до этого нельзя считать корректным исправлением aliasing. Динамическая смена layout/mip count на одном адресе остаётся частью этапа coherence.

Проверка: тот же набор из 30 объёмов/133 уровней загружается через uint-view нативных R8Unorm/R16Float/RGBA8Unorm/RGBA16Float/RGBA32Float и packed RGB10A2/RG11B10 текстур. Каждый уровень сверяется побайтно с CPU-декодом.

Различие путей: GPU import/export теперь доступны библиотеке преобразований; CPU snapshot upload уже подключён в обычный загрузчик. Перевод всех живых GPU-поверхностей на автоматический GPU import/export требует следующего этапа — учёта владельцев памяти и порядка переходов. Вставлять compute-конвертер внутрь уже открытого render/compute encoder без такого порядка нельзя.

### 4. Таблицы адресации больше 4 КиБ

В старом пути все lookup tables передавались через `setBytes`, у которого есть предел 4 КиБ. Теперь большие таблицы передаются отдельным MTLBuffer, удерживаемым до завершения command buffer. Малые таблицы используют прежний путь. Добавлен GPU-тест широкой поверхности, а также проверки размеров входного buffer range.

## Следующие этапы по зависимостям

| Очередь | Подсистема | Проверенный пробел | Что реализовать / критерий |
|---:|---|---|---|
| 1 | PM4 видимость памяти | `CpOpAcquireMem` пропускает payload; Metal-ветка `EmitGlobalBarrier` возвращает без передачи cache actions модели ресурсов. Порядок GPU частично обеспечивают существующие encoder fences. | Декодировать реальные и внутренние acquire/release; различать диапазон, invalidate, writeback и completion. Проверять CPU→GPU, GPU→GPU и GPU→CPU litmus-тестами. Не заменять это CPU wait на каждый пакет. |
| 1 | Единое состояние памяти | Несколько самостоятельных реестров по vaddr/размеру/flip. `AliasConvert` работает через scratch, не всегда материализует гостевые байты для raw-потребителя. | Physical backing + subranges + sequence/owner/layout/version. Регистрировать потенциальные записи, разрешать dirty owners до конфликтующего доступа, сохранять нетронутые байты. |
| 1 | CPU writes/remap | Metal `PageManager::Protect` отключает защиту страниц; hash больших текстур проверяет четыре окна по 512 байт. Часть caches всё ещё не проверяет физический mapping; проверка логических NumRecords/record bounds также нуждается в отдельном наборе тестов, сверх physical byte bounds. | Точное обнаружение dirty ranges и инвалидация всех physical aliases, включая sampled/mip/layered/volume/metadata caches. Предыдущая правка закрыла только два buffer-backed пути. |
| 2 | DCC/CMASK | Есть распознавание некоторых uniform metadata fills и clear codes; общего codec/state machine для произвольного metadata нет. CMASK touched-marking использует заполнение диапазона вместо точного учёта raster coverage. | Учёт metadata по блокам, fast clear, частичное покрытие и переходы compressed→readable. Отдельные тесты адресации metadata и preservation untouched tiles. |
| 2 | HTILE | Запись compute-only в зарегистрированный HTILE base интерпретируется как очистка depth. Это ограниченное распознавание, а не декодирование любого HTILE содержимого. | Связать реальное содержимое metadata с depth/stencil и областями очистки; проверить mixed cleared/written tiles. |
| 2 | Eliminate/decompress | `CB_COLOR_CONTROL.MODE` 2/5/6 обрабатываются ранним выходом из draw. | Выполнять соответствующий переход состояния fast-clear/FMASK/DCC. Не запускать обычный PS как замену, но и не считать отсутствие pixel draw достаточной эмуляцией metadata operation. |
| 3 | Linear 3D input | CPU upload в GuestTexture3D по-прежнему пропускает linear tile mode; GPU-written volumes имеют отдельное правило владения. | Реализовать linear row/slice pitch и согласование CPU/GPU владельцев вместо чтения устаревшей копии. |
| 3 | Shadow sampling | По умолчанию MSL depth compare использует фиксированную функцию вместо всех функций sampler; PCF явно отсутствует. Есть экспериментальные переключатели. | Сначала корректный порядок reference/texel и все 8 compare functions, затем compare-before-filter и gather. Проверить на текстурных шаблонах с разной глубиной; отдельно сравнить картинку игры. |
| 3 | Sampler modes | MirrorOnceHalfBorder/MirrorOnceBorder сведены к MirrorClampToEdge; ненормализованные координаты принудительно ограничивают режимы из-за API Metal. | Воспроизвести недоступные режимы координатной логикой MSL; проверить края, border, LOD и mip. |
| 3 | Форматы | Не каждому формату PS5 соответствует нативный формат Metal. Есть расширения до RGBA и подмены при invalid format. | Таблица точной совместимости/битовых размеров и программные преобразования. Исключать unsupported-format substitution только после реализации правильного пути. |
| 4 | VS/PS интерфейс | PointSize, CullDistance, Layer, ViewportIndex принимаются и отбрасываются в MSL; pixel Layer/SampleId помечаются unsupported. | Нативные Metal builtins и необходимое состояние pipeline/render targets; GPU-тесты выбора слоя, viewport, точки и отсечения. |
| 4 | Wave64/LDS | Есть pair lowering и LDS/GDS, но дополнительные LDS barriers экспериментальны. Полное соответствие межлейнового порядка не установлено. | Отдельные тесты всех межполовинных операций, divergent control flow, LDS exchange и барьеров. Это пункт независимой проверки, а не утверждение «LDS отсутствует». |
| 5 | ARM64 fibers | ARM64 `FiberSaveContext/FiberRestoreContext` завершаются `EXIT`; публичные FiberRun/Switch/Return используют эти функции. | Реализовать переключение гостевого x86-контекста через механизм FEX, не сохранение регистров ARM64 вместо гостевых. Проверить TLS, stack, return и nested callbacks. |
| 5 | pthread exit через FEX | `CallGuest` путь содержит TODO об unwind при guest PthreadExit. | Корректный возврат из JIT callback и завершение guest thread, с тестами cleanup/join/cancellation. |
| 6 | HLE/audio и остальные службы | Поддерживается ограниченный набор AJM codecs; вне списка возвращается CODEC_NOT_SUPPORTED. Остальные библиотеки ещё не проверены целиком. | Продолжить аудит фактических guest API/codec вызовов и implement реальные отсутствующие контракты. Не путать ABI trampolines FEX с функциями-пустышками. |

Ограничения, обнаруженные чтением кода, ещё не доказывают причину конкретных артефактов. Нужны независимые проверки результата каждой реализации, даже если текущая игра не вызывает этот путь.

## Точки входа в код

- `src/graphics/shader/recompiler/backend/msl/MslEmitter.cpp`: BufferByte/BufferDword, raw vector memory operations, EmitFormattedStore, sample/gather compare и VS/PS interface.
- `src/graphics/host_gpu/metal/metalTiling.h/.mm`: z-таблицы, volume import/export, DecodeTiledVolumeLevel, UploadTiledVolumeMipChain и SetTable.
- `src/graphics/host_gpu/metal/metalRenderer.mm`: GuestTexture3D, metadata clears, Begin/Ensure render pass, cache lookup paths.
- `src/graphics/guest_gpu/command_processor/pm4Handlers.cpp:1307`: acquire parser; `graphicsRun.cpp:2467`: global barrier.
- `src/graphics/host_gpu/pageManager.cpp:198`: Metal protection bypass.
- `src/graphics/host_gpu/metal/metalSampler.mm`: sampler mode mapping.
- `src/libs/libKernel.cpp:2610`: ARM64 fiber context fallback; `src/kernel/pthread.cpp:942`: guest-exit unwind TODO.
- `src/libs/ajm.cpp:197`: codec support policy.

## Команды проверки

```sh
cd /Users/tx/Developer/KyTx
bash tools/test_metal_msl.sh
bash tools/test_metal_tiling.sh
cmake --build _Build/macos-arm64 --target kyty_emulator -j8
```

Новые regression fixtures: `tests/metalMslBufferSwizzleTests.inc` и `tests/metalTilingVolumeTests.inc`. Они включены в существующие GPU suites. Полная сборка прошла. Игровой smoke и его ограничения записываются отдельно ниже. Коммитов, публикации игровых данных и новых NaN-заглушек нет; прежние незакоммиченные изменения сохранены.

Публичные источники для дальнейших реализаций: [AMD RDNA2 ISA](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna2-shader-instruction-set-architecture.pdf), [AMD PAL command packets](https://github.com/GPUOpen-Drivers/pal/blob/dev/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp), [Apple resource synchronization](https://developer.apple.com/documentation/metal/resource-synchronization), [Apple texture capabilities](https://developer.apple.com/metal/capabilities/). Эти документы не заменяют независимую проверку особенностей custom GPU PS5.

## Итог проверки запуска

`ec1`: трёхминутный запуск без sanitizer завершён по таймеру, GPU Fault/progress timeout в логе не найдено, но финальная визуальная проверка выявила сильную мозаичную порчу кадра. Этот запуск НЕ считается успешной проверкой изображения. Пользователь подтвердил, что эта мозаика и блоки были до новых 3D-функций. Это исходные артефакты эмулятора; причинная связь с новыми реализациями не установлена. Независимое изменение ключа 3D-кэша исключено из пакета по архитектурной причине: согласование aliases требует общего трекера владельцев, а не раздельных texture objects. Оно не объявляется причиной исходных артефактов.

Финальные результаты: MSL GPU suite PASS (в том числе 4806 swizzle results), tiling suite PASS (30 volumes/133 mip round-trips, прежние 58 layouts и 8 mip chains), сборка PASS, diff whitespace check PASS. Все собственные эмуляторы остановлены. Launch environment восстановлен.

Следующий самостоятельный этап — PM4 acquire/release + physical-range coherence tracker. После него подключать автоматические GPU import/export на переходах владельца, затем реализовывать metadata state transitions. Не возвращать отвергнутую защиту TAA и не подменять эти этапы обнулениями или пропуском работы.

Дополнительная проверка: родительские нативные 3D-текстуры также читаются после upload через uint-view; их байты совпадают на каждом mip, включая RGB10A2 и RG11B10. Это отдельно исключает ошибку видимости между view и parent в синтетическом тесте.

Финальная проверка `ec2` также дошла до демо-матча, штатно остановлена по таймеру, GPU Fault/progress timeout не найдено. На проверенном кадре игроки/поле/UI видимы, присутствуют исходные блоки освещения. Один прогон не устанавливает ни устранение мозаики, ни её связь с изменением кэша. Прирост FPS не заявляется.

Уточнение пользователя: мозаика и блоки существовали до этих правок и сохраняются сейчас. В дальнейшем считать их исходными проблемами; отдельно доказывать влияние каждой реализации.
