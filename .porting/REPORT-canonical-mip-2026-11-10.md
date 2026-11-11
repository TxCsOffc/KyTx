# Каноническая mip-память Metal — 10 ноября 2026

Прочитана сессия Claude `1234`, UUID `677b85d0-c7ab-4ad2-9f34-ef46774c2230` в `~/.claude/projects/-Users-tx-Developer`. Claude исправил насыщение 8/16-битных uint-записей в новом тесте и получил PASS; runtime-интеграцию ещё не сделал. Его исправление сохранено. Остальные унаследованные изменения, включая native NGG, сохранены; коммитов, staging, push или reset не было. Ledger по-прежнему недоступен.

## Реализация

- GPU import/export отдельного физического 2D mip/layer поддерживает реальные смещения полной аллокации, bank XOR и общие mip-tail. Экспорт пишет только видимые элементы, не весь tile.
- Новый `metalCanonicalMemory.h` разрешает все физические диапазоны VA, объединяет физически соседние отображения, работает через канонические Metal backing chunks. Для раздельных диапазонов/границ chunks выполняются GPU gather/scatter; scatter ограничен точным footprint mip, поэтому более новые записи в padding/другие mip сохраняются.
- `metalCanonicalMip.inc` интегрирует eager writeback для mip render targets и пересекающихся с ними 2D color views. Load импортирует выбранный mip; store после прохода возвращает его байты в физическую гостевую память. Canonical ownership не зависит от возраста текстуры или sampled hash. Очередь writeback выполняется после производителя, перед отложенными DMA fills/copies.
- Compute-ресурсы готовятся до создания их encoder, затем читают канонические байты в своей раскладке. Storage-записи возвращаются после dispatch, включая indirect/split-команды. BASE/LAST mip сохраняются.
- Graphics storage images возвращают записи после настоящих draw, включая mesh/indexed/indirect. Повторные записи одной image в открытом encoder объединяются в один export. Render pass description сохраняет load/store adapters при повторном открытии.
- Compute read теперь может использовать прежнее настоящее tile-alias преобразование до binding; staging D16/D32 в сырые элементы проверен отдельно. Это НЕ завершённый учёт владения depth-памятью: последний запуск обнаружил следующий отказ именно там.

Новые ресурсы импортируются из реальных гостевых байтов; подстановка нулей, shader skip, разрешение старых writers по возрасту, sampled-hash доказательство и ослабление форматной проверки не добавлялись. Этап использует eager materialization; это ещё не интеграция всего lazy `SurfaceCoherence` tracker. Цена дополнительных GPU-копий не измерялась, FPS выигрыш не заявляется.

## Проверки

| Набор | Результат |
|---|---|
| Mip transfers/footprint, все видимые элементы каждого mip/layer | 9 раскладок, 45 отрицательных запросов; 18 029 312 байт гостевой памяти и 4 171 556 texel разных размеров элементов; PASS |
| Раздельные физические диапазоны, соседние VA mappings, chunk crossings, более новая raw-buffer запись вне footprint | 1 049 088 байт; 4 отрицательных контроля; PASS |
| Настоящие render load/store/reopen/clear для RGBA8, RG11B10, RGBA16F | 18 случаев, 2 368 512 физических байт; 0 расхождений |
| Настоящий fragment storage writer | 2 draw, 1 объединённый export, 65 536 байт; 0 расхождений |
| Depth→raw color alias | D16/D32, побитовый CPU oracle и сохранение guest-only байтов; PASS |
| Прежние tiled aliases | 28 пар; 497 233 writer bytes и 2 189 447 guest-only bytes; PASS |
| Полный pipeline GPU-набор, MDI и native mesh producer | PASS, API + shader validation |
| Product build / diff --check | PASS |

Финальные логи: `work/metal-mip-alias/kytx-mip-coherence-final.log`, `kytx-mip-pipeline-final.log`, `kytx-mip-build-graphics-view.log`. Полный MSL/CFG набор в этом этапе не повторялся: shader emitter не изменялся. Старый Apple CFG failure из NGG-отчёта не объявляется исправленным. Отрицательные проверки pipeline намеренно печатают Unsupported Metal; итоговые наборы завершились exit 0.

## Игра и точный следующий пробел

Все запуски — один экземпляр, без автоматического ввода, с исходным 40-GB guard, без timer kill и без diagnostic validation layers. Каждый процесс завершился сам на явном Unsupported; никакие процессы не убивались. `extra_env_s1.sh` восстановлен побайтно. Диагностика теперь хранится в `~/Developer`, не в очищаемом при перезагрузке `/tmp`.

MP1/MP2 обнаружили ограничение API поиска backing: VA-цепочка `0x8242d00000`, 12 648 448 байт пересекала три отображения по 8 MiB; они физически соседние (`faa00000`, `fb200000`, `fba00000`), но старый TryGetGuestBackingOffset требовал одно entry. Добавлен отдельный resolver и GPU transfer plan, прежний memory API не ослаблен.

MP3 прошёл прежний NG5 mip alias и mip compute-пирамиду. MP4 дошёл до graphics storage writeback, затем этот путь реализован и проверен настоящим fragment producer.

Последний **MP5**, PID **5508**, запуск **2026-11-10T07:07:09+05:00**, SHA256 main/s3 **`1b4a34c7789fe252e6b05da47bc95c31920f8e69d74b2095115d1e2a983d5406`**. Лог `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/mp5-070708.log`. Сейчас игры нет. Последняя shader coverage: 100 translated / 0 unsupported; это не полный подсчёт всех переведённых после этой строки шейдеров.

Следующий отказ:

```
[ImageFormatReject] shader=a3cdaafc076823b8 res=0 addr=8242480000 1920x1080 tile=27 via=depth-layer storage=0
Unsupported Metal emulation: compute image format differs from descriptor: key=56 extra=252/70
```

`252` — Depth32Float, `70` — RGBA8Unorm. `DepthWriterForLayeredRead` допускает любой формат того же texel size и возвращает depth object даже для цветного побитового читателя. Прежний AliasConvert может отказать по своему возрастному/геометрическому контракту; точная причина его nil для этого запуска ещё не установлена. Не ослаблять форматную проверку и не включать старые writers по возрасту. Нужен настоящий physical depth ownership/materialization, включая слои/HTILE/CPU writes, затем color decode. GPU D16/D32 staging проверен, но этим сам по себе вопрос владельца не решён.

Уточнение прежнего NGG-отчёта: Metal format **113 — RGBA16Uint**, не RGBA16Float. Guest format69 тоже uint. Следовательно NG5 требовал побитовую смену размера элемента RG11B10Float→RGBA16Uint, а не численную конверсию в half. Новый путь сохраняет именно физические байты.

Старые тени, чёрные квадраты и прочие артефакты исправленными не объявляются: до визуально проверенного меню/матча последний запуск не дошёл.

## Границы и продолжение

Далее: depth→color ownership по указанному отказу. За пределами этого этапа остаются compressed BC views поверх канонических mip-диапазонов (сейчас явный reject), полноценная интеграция arrays/3D и generic CPU write tracking/lazy range ownership. В остальных legacy image paths ещё есть прежние age/hash/new-image эвристики; этот этап не является удалением всех заглушек Metal или полной эмуляцией PS5. Остальные незавершённые shader/primitive функции перечислены в NGG-отчёте.

Артефакт `work/metal-mip-alias/canonical-mip-memory.patch` — только этот этап относительно сохранённого dirty baseline `before`, 16 файлов; `patch-manifest.json` содержит SHA256 before/after. Включает исправление теста Claude. Не применять поверх чистого HEAD без предыдущих этапов. Исходная ветка `metal-native-checkpoint`, HEAD `9534b33718bd10104a4baf708758d91f61e2ef8d`.
