# Depth/stencil memory и depth bounds — 10 ноября 2026

Продолжен native RDNA→MSL путь после отказа `Depth32Float → RGBA8Unorm`. Реализована материализация физических Z/S-байтов, выбранных слоёв и обычных 2D-array views; затем следующий запуск выявил depth bounds, для него подключён настоящий Metal API. Предыдущие mip/NGG изменения сохранены. Это не исправление всей картинки: в реальной игре по-прежнему видны сильные полосы/блоки; последняя загрузка остановилась позже на отсутствующем mesh→pixel varying.

## Реализация

- `metalDepthMemory.h`: отдельные D16, D32Float и S8 плоскости переносятся между private Metal depth/stencil textures и канонической физической гостевой памятью. Буферный bridge сохраняет сырые биты, затем общий tile kernel применяет реальную раскладку. Выбранные слои учитывают guest bank XOR, несколько физических отображений, chunk crossings и exact texel footprints; padding и другие слои не перезаписываются.
- Depth/stencil attachments получили load/store adapters, как mip color targets. Import перед проходом видит CPU/raw-buffer/другие GPU writes; после прохода текущие байты возвращаются в backing. Повторное открытие прохода сохраняет adapters. Создание нового host depth/stencil объекта больше не вызывает фиктивную первую очистку: импортируются реальные guest bytes. Явные DB/metadata clears сохраняются и материализуются до последующего load.
- Сам адрес чтения не означает владения всей плоскостью по адресу записи. Разные VA с одинаковым физическим backing допустимы; **разные физические read/write planes при записи пока явно отвергаются**: для правильной частичной записи нужен per-fragment coverage, а копировать весь read target в write target неверно. Read-only pass не меняет отдельную write allocation. Эти адреса и stencil/depth write protection входят в pass key.
- `CanonicalMipImage` теперь обслуживает обычные 2D arrays и выбранный BASE_ARRAY, а graphics/compute предпочитают канонические байты прежним depth/layered caches. Depth lookup больше не считает RGBA8 допустимым native depth value-view только из-за одинакового texel size. R128 входит в cache key.
- Переподготовка одной image в пределах draw/dispatch зависит от **её физических диапазонов**, а не общей mapping generation. Посторонний remap больше не сбрасывает подготовленный pipeline во время binding; фактический remap этих байтов инвалидирует image.
- `metalDepthBounds.h` и `MetalDrawExecutor::SetDepthBounds`: bounds применяются динамически к каждому draw на Apple10/macOS26+. Проверяется **сохранённая** глубина, а не Z входящего фрагмента; отключение сбрасывает ограничения encoder. Включение больше не является общим pipeline reject на поддержанном GPU.
- Для D16 Metal округляет bounds к UNorm16. Нижняя/верхняя границы переводятся в первый/последний представимый depth code, удовлетворяющий исходным float comparisons. Контрольные значения непосредственно за границей раньше давали 4 ошибочных случая, после исправления — 0. Нет clamps параметров к придуманному диапазону.

Native depth bounds: [Apple API](https://developer.apple.com/documentation/metal/mtlrendercommandencoder/setdepthtestminbound%3Amaxbound%3A?language=objc), [feature tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf). Ориентир для сравнения сохранённой глубины и порядка coverage: [Khronos fragment operations](https://docs.vulkan.org/spec/latest/chapters/fragops.html). Буферный bridge использует документированные [texture→buffer](https://developer.apple.com/documentation/metal/mtlblitcommandencoder/copy%28from%3Asourceslice%3Asourcelevel%3Asourceorigin%3Asourcesize%3Ato%3Adestinationoffset%3Adestinationbytesperrow%3Adestinationbytesperimage%3A%29?language=objc) и [buffer→texture](https://developer.apple.com/documentation/metal/mtlblitcommandencoder/copy%28from%3Asourceoffset%3Asourcebytesperrow%3Asourcebytesperimage%3Asourcesize%3Ato%3Adestinationslice%3Adestinationlevel%3Adestinationorigin%3Aoptions%3A%29?language=objc) операции. Поведение Vulkan по игнорированию фич не переносилось.

## Проверки

| Проверка | Итог |
|---|---|
| D16/D32/S8 physical coherence, selected layers, CPU/raw-buffer updates, discontiguous physical ranges, depth→larger color view | 12 GPU-сценариев, 9 отрицательных запросов, 3 172 296 байт; PASS |
| D32 raw patterns в том же наборе | Сохранены signed zero, subnormal, NaN payload, ±Inf, отрицательные и >1 float bits; PASS |
| Реальные depth/stencil render load, partial instanced layer writes, read-only reopen, clear | 8 сценариев, 4 202 496 байт; 0 расхождений |
| Native depth bounds D16/D32, inclusive endpoints/nextafter, Z-test enabled/disabled, reset, color+stencil coverage | 48 GPU-сценариев, 12 288 pixels (проверяется и цвет, и stencil), 0 расхождений |
| Depth bounds rejection contracts | 10 invalid interval requests + 1 неподдержанный empty D16 interval отвергнуты |
| Старые mip/alias/physical span/coherence наборы | PASS |
| Полный pipeline, MRT/raster/stencil/depth arrays/read-only, MDI, mesh, mip/graphics image writeback | PASS с Metal API + shader validation |
| Product build, diff --check, source manifest и patch baseline check | PASS |

Финальные логи в `work/metal-depth-memory`: `kytx-depth-coherence-final.log`, `kytx-depth-pipeline-final.log`, `kytx-depth-bounds.log`, `kytx-depth-build-bounds-final.log`. Shader emitters не менялись, весь MSL/CFG набор не повторялся; старый Apple clip-guard CFG failure не объявляется исправленным. В pipeline strict test прежнее blanket-ожидание reject для bounds заменено positive support check на Apple10 и прежним reject на неподдержанной платформе; новые GPU cases проверяют сам результат.

## Реальная игра

DM1 (PID6520, SHA c224ec49...) прошёл до новой materialization, затем остановился на `draw without an active encoder/pipeline`. Причина — избыточная переподготовка при изменении общего mapping generation; исправлено сравнением физических диапазонов.

DM2 (PID6684, SHA f2d3d3b...) прошёл прежний Depth32→RGBA8 отказ и достиг следующего release reject: `depth bounds testing has no native Metal lowering`. Этот lowering затем реализован и протестирован.

**Последний DM3**: PID **7510**, старт **2026-11-10T07:37:54+05:00**, SHA256 main и s3 **`a780c6bec76236cc1e13deca9e8c1ac7c8f2244c6309c8cec897cff997a318e5`**. Лог `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/dm3-073753.log`.

Запуск достиг сообщения EA Servers (CUA frame614), один ручной `j` закрыл именно наблюдаемое сообщение; затем появилась загрузка с логотипом FC (frame695), но с сильными полосами, цветными блоками и нечитаемой сценой. Это НЕ проверенное меню/матч и НЕ доказательство улучшения старых теней. Сравнимый baseline кадр с этими строгими стадиями отсутствует, поэтому видимые артефакты не приписываются автоматически последнему изменению.

DM3 после этого **естественно завершился** на создании нового pipeline, последняя печатная coverage **250 translated / 0 unsupported**. Это не полный итоговый счётчик всех шейдеров после этой строки. Сейчас эмулятора нет. В проверенном логе нет GPU command-buffer fault; причина остановки — явный pipeline contract.

Точный следующий отказ на flip580:

```
[MetalPipeline] create failed (0) vs=0xf4bf0e46487b8570 ps=0x61522947b13064ef fmt=0:
Fragment input(s) `user(locn0)` mismatching mesh shader output type(s) or not written by mesh shader
```

Guest VS/NGG **`4ad8b17ce657eb68`**, guest PS **`7facde60b1bc62d4`**. Сохранённые `.metal/.ir/.json` пары находятся в `work/metal-depth-memory/next-pipeline`. Mesh `KytyStageOut` содержит только position, `KytyPrimitiveOut` — culled и primitive_id; PS требует `float4 param0 [[user(locn0), flat]]` и также имеет primitive_id builtin. Это пока только наблюдение. Нельзя добавлять нулевой varying ради успешной линковки. Следует проверить SPI_PS_INPUT_CNTL / DEFAULT_VAL / VGT_PRIMITIVEID_EN и аппаратную маршрутизацию system-generated primitive ID, а также frontend outputs. Причина отсутствия param0 пока не доказана.

Режим запусков: один экземпляр, diagnostic validation/capture layers в игре выключены, исходный 40-GB guard, без timer kill и без автоматического ввода. `extra_env_s1.sh` восстановлен побайтно, исходники/диагностика в постоянных каталогах Developer/workspace. После завершения DM2 повторный CUA screenshot автоматически открыл лишний c9 PID6899 с тем же binary; к моменту проверки он уже сам завершился на том же depth-bounds reject. Его frame124 НЕ относится к DM2. Больше такой UI-запуск не делался. Ни одного процесса сигналом в этом этапе не завершено.

## Границы

Не завершены: независимые физические read/write Z/S targets с частичным покрытием, MSAA/mipped-depth allocation, canonical BC views, generic within-dispatch aliasing/lazy ownership. Native bounds для unsupported hardware, out-of-range/nonfinite/inverted bounds и интервал D16 без единого представимого значения по-прежнему явно отвергаются; draw не пропускается для имитации пустого coverage. Унаследованные HTILE detection/clear-value эвристики ещё действуют; этот этап не доказал правильность декодирования всех HTILE codes. Другие legacy age/hash/new-image пути также остаются за рамками нового canonical path.

Новых shader skips, заглушек, нулевых image/varying substitutions или произвольных лимитов не добавлялось. Ledger недоступен; STATE/HANDOVER/worklog обновлены. Huge inherited dirty tree сохранён, ветка `metal-native-checkpoint`, HEAD `9534b33718bd10104a4baf708758d91f61e2ef8d`, без staging/commit/push/reset. Патч `canonical-depth-and-bounds.patch` — 16 файлов относительно сохранённого dirty baseline `work/metal-depth-memory/before`, не чистого HEAD; `patch-manifest.json` содержит before/after SHA256.
