# Нативный Metal NGG / Primitive — 10 ноября 2026

Выполнена отдельная реализация для RDNA→MSL. Поведение Vulkan, который игнорировал Primitive в Vertex-стадии, не перенесено. Полной эмуляцией PS5 или исправлением старых теней этот результат пока не является.

## Что работает

- Native mesh-стадия исполняет NGG с настоящими `s2`/`s3`, входными индексами `v0`, индексом вершины, base vertex и instance ID. Сохраняются GS_ALLOC_REQ, связность, число и отсечение примитивов, PrimitiveId, слой и viewport. Wave64 исполняется парами гостевых потоков на 32 физических SIMD-полосах.
- LDS, барьеры и целочисленные атомарные операции mesh-стадии используют общую память рабочей группы. Прямые, индексированные, косвенные и пакетные команды имеют отдельный Metal-путь. GPU-параметры готовятся после их производителя, без чтения счётчиков на CPU и искусственного уменьшения работы.
- NGG RECTLIST строится после экспорта трёх настоящих вершин: четвёртый угол и два треугольника имеют согласованное направление, сохраняются интерполяция, точные плоские данные, провоцирующая вершина и отсечение. Повторного запуска гостевого шейдера для отсутствующей вершины нет.
- Пиксельные DS_ADDTID spill/reload переводятся в переменные только после доказательства владения ячейками и доминирующей записи. Используется реальная дополнительная LDS из регистра PS. Произвольная общая LDS, неверное резервирование и чтение до записи отвергаются.
- `KYTY_MSL_ONLY` теперь завершает общий IR без генерации и валидации SPIR-V. Новый Metal index ABI не маскируется фиктивным SPIR-V результатом. Для других бэкендов SPIR-V остаётся включён.

Основные реализации: [mesh](/Users/tx/Developer/KyTx/src/graphics/shader/recompiler/backend/msl/MslMeshEmitter.inc), [RECTLIST](/Users/tx/Developer/KyTx/src/graphics/shader/recompiler/backend/msl/MslRectMeshEmitter.inc), [доказательство пиксельных spill/reload](/Users/tx/Developer/KyTx/src/graphics/shader/recompiler/backend/msl/MslPixelLaneMemory.inc).

## Проверки

| Проверка | Результат |
|---|---|
| NGG связность, allocation, null primitive, PrimitiveId, резервные биты | 36 GPU-сценариев, 73 728 пикселей, 0 расхождений |
| Провоцирующая вершина, flat, layer, viewport | 4 сценария, 32 768 пикселей, 0 расхождений |
| RECTLIST: все 6 порядков входа, оба режима cull, интерполяция, raw flat/NaN payload | 96 сценариев, 196 608 пикселей, 0 расхождений |
| Настоящий RDNA frontend, s2/s3/v0/v5/v8, хвосты групп, индексы/instance, межволновая LDS, atomic counter | 16 конфигураций, 86 016 значений, PASS |
| Пиксельное владение LDS и запись перед чтением | 2 GPU + 8 отрицательных сценариев, 1024 пикселя, PASS |
| Настоящий Metal pipeline/draw/index/indirect с GPU-производителем | 5 сценариев, 0 ошибок |
| Полные MSL и pipeline GPU-наборы | PASS; общий MSL-набор с API validation, отдельные новые наборы также с shader validation |
| Преобразование физической раскладки alias-памяти | 28 пар раскладок/форматов, сохранены 497 233 байта писателя и 2 189 447 остальных байтов; PASS |
| Сборка приложения, vertex metadata, tessellation-only, diff --check | PASS |

Общий `shader_cfg_tests` не проходит старое Apple-условие: `TestRepeatedExportsHaveOneInterface` ожидает два SPIR-V выхода и ClipDistance, тогда как `spirvEmitterModule.cpp` явно отключает добавочную clipping-error plane на Apple. Это отдельный незакрытый тест; успешным весь CFG-набор не обозначается. Искусственная плоскость ради прохождения теста не возвращалась.

Логи находятся рядом с отчётом в `work/metal-primitive`. `msl-full-final.log`, `mesh-validation.log`, `ngg-frontend-final.log`, `pixel-lds.log`, `mesh-host.log`, `pipeline-full.log`, `coherence-final.log` содержат соответствующие результаты. Первый полный запуск с инструментированием всех старых shader-тестов остановлен во время длительной компиляции BufferStrideSwizzle; это не успешный тест и не GPU execution hang. Итоговый полный набор выполнен с API validation, новые наборы — с обоими слоями.

## Реальная игра

Последний запуск NG5: PID 69036, SHA256 `2acfc045ec00a74bd4138bb28c3f7e09970bca992894efe5d4cddaa48bdc1fe8`, лог `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/ng5-030006.log`. Прошёл прежние остановки Primitive, RECTLIST и два пиксельных LDS-шейдера (`0ef96a063e2bc1d3`, `dec48a1373748c4f`: доказаны 2 ячейки, 10 обращений у каждого). Переведены 100 шейдеров без unsupported shader, затем остановился на материализации памяти. Сейчас запущенной игры нет.

Точный следующий пробел: адрес `0x8242d00000`, читатель 1920×1080 RGBA16Float, tile27, один mip, flip138. Кэш содержит RT RG11B10Float, 11 mip, последний draw на flip124. Прямое texture view между элементами 4 и 8 байт не является преобразованием памяти. Текущий AliasConvert не обслуживает этот старый mipped writer; compute-read путь доходит до GuestTexture. Нужны отслеживание владельца физических диапазонов/подресурсов и корректный export/import соответствующих mip-отпечатков. Существующий GPU tile-alias kernel прошёл отдельные тесты, но его интеграция здесь ещё не выполнена.

Не включать старый `KYTY_ALIAS_OLD_WRITER` и не подставлять пустую AliasedImageFor. Равенство GuestTexSourceHash также недостаточно: для крупных диапазонов он проверяет лишь четыре небольших окна, а не все байты. Возврат к такому угадыванию снова скроет ошибки.

NG3 остановлен по его точному PID после подтверждённой блокировки диагностического fopen в Documents. Диагностика перенесена в `/tmp`; NG4/NG5 затем свободно проходят это место. Для следующих сборок использовать cwd `/Users/tx/Developer/KyTx`: запуск cmake из защищённого Documents тоже блокировался до начала Ninja. Логи из `/tmp` скопированы сюда. Остальные экземпляры не завершались таймером; оригинальный 40-GB guard сохранён, автоматический ввод не включался, extra_env восстановлен.

## Оставшиеся границы

Этот этап не закрывает все заглушки Metal. Ещё нужны generic PS LDS, полноценные coherence/mip alias, primitive restart и другие топологии, wireframe edge flags/streamout, динамический flattened SRT, SampleMask/CullDistance, clip-disable XY и native tessellation. Выходная mesh-раскладка этого этапа ограничена 256 записями; большие/другие модели требуют дополнительной сборки, а не уменьшения счётчиков. Старые тени и остальные артефакты визуально исправленными не объявляются.

Первичные ориентиры: [AMD NGG](https://github.com/GPUOpen-Drivers/llpc/blob/dev/lgc/lowering/NggPrimShader.cpp), [AMD register ABI](https://github.com/GPUOpen-Drivers/llpc/blob/dev/lgc/lowering/ShaderMerger.cpp), [Metal MSL](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf), [LLVM pixel LDS spilling](https://reviews.llvm.org/D130784). Код не использует Vulkan как эталон для игнорируемых операций.

Исходная ветка `metal-native-checkpoint`, HEAD `9534b33718bd10104a4baf708758d91f61e2ef8d`; унаследованные изменения сохранены. Коммитов, staging и push не было. Патч `metal-strict-and-native-ngg.patch` объединяет удаление прежних Metal-заглушек с этим этапом относительно сохранённых рабочих файлов `work/metal-strict/before`, а не относительно чистого HEAD; hashes в `patch-manifest.json`.
