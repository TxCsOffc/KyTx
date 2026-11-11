# Native Metal: запуск PS, depth/stencil mesh и LDS predicates — 10 ноября 2026

Продолжение после DM3. Исправлена аппаратная активация pixel-стадии, затем расширено строгое доказательство lane-local LDS для неизменяемой маски исполнения через цикл. Прежние mip/depth/bounds/NGG стадии сохранены. Исправление старых визуальных теней этим этапом пока не доказано.

## PS и depth/stencil mesh

DM3 не требовал придумывать новый varying. Диагностический ML2 повторил исходную пару guest NGG `4ad8b17ce657eb68` / PS `7facde60b1bc62d4` (native `f4bf0e46487b8570` / `61522947b13064ef`) и снял реальные регистры:

```
VS_OUT_CONFIG=00000080 POS_FORMAT=00000004 PRIMITIVEID_EN=00000000
PS_IN_CONTROL=00000000 PS_INPUT_ENA=00000000 PS_INPUT_ADDR=00000000
BARYC_CNTL=00000000 PS_INPUT_CNTL[0]=00000000
```

Native renderer раньше считал PS активным просто по ненулевому адресу ранее привязанного кода. `NativePixelShaderActive` теперь учитывает пересечение CB_TARGET_MASK/CB_SHADER_MASK и SPI_SHADER_COL_FORMAT, CB_COLOR_CONTROL.MODE, а также DB kill, Z/mask/dual export и EXEC_ON_NOOP. Отключённые входы PS сами по себе **не являются** условием выключения: input-free shader с EXEC_ON_NOOP и UAV side effects остаётся активным. Отдельные indexed/automatic входы renderer используют одну функцию. Draw и mesh guest-код продолжают выполняться; никаких skips для ошибки компиляции нет.

Смысл EXEC_ON_NOOP подтверждён первичным [AMD register reference](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/programmer-references/evergreen_3D_registers_v2.pdf); структура современных полей — [PAL ABI metadata](https://github.com/GPUOpen-Drivers/pal/blob/dev/inc/core/g_palPipelineAbiMetadata.h). Vulkan `renderDraw.cpp` использует сходную активацию; простого копирования отсутствующих varying/игнорирования guest-кода не делалось.

Обычный Metal vertex pipeline допускает nil fragment с работающими depth/stencil ([Apple](https://developer.apple.com/documentation/metal/mtlrenderpipelinedescriptor/fragmentfunction)). Фактический запуск ML3 выявил, что mesh pipeline на данном Apple GPU при rasterization требует fragmentFunction: `fragmentFunction must not be nil`. Для выключенной **гостевой** PS-стадии добавлен отдельный `kyty_db_fixed_function`: нет входных параметров, shader-экспортов, ресурсов и операций с гостевой памятью. Это реализация fixed-function растеризации Z/S: глубина берётся из растеризуемой геометрии, stencil определяется штатным состоянием. Не используется для неподдержанного активного PS. При ненулевом fs_hash без функции или при запросе цветовых записей этот режим явно отвергается. Функция кэшируется в MetalPipelineManager и очищается с его caches.

ML4 реально прошёл прежний отказ: `[RasterState] flip=244 vs=f4bf0e46487b8570 ps=0 depth=250 size=2048x2048` с zwrite=1, тремя слоями; затем 3072x3072 depth atlas. Shader/NGG draw не удалён. Нулевой user varying и подмена параметра на primitive_id не добавлялись.

## Mip allocation

Удалён глобальный `s_mip_requests`, который по истории SRD at same VA только увеличивал число mips будущего render target. Чужой view/повторное использование адреса больше не меняет CB_COLOR_ATTRIB2.MAX_MIP. Гостевые физические байты между разными views продолжают переноситься каноническим adapter; это не запрос на генерацию отсутствующих уровней.

ML1 естественно завершился на `invalid render-target mip range` до целевой пары; исходных значений диапазона в том запуске ещё не было в логе. Поэтому причинная связь именно этого ML1 с registry не считается доказанной. Добавлена подробная диагностика следующего такого отказа; ML2/ML4 этого отказа не повторили. Legacy sampled-image heuristics за пределами canonical path остаются.

## LDS: неизменяемая маска через Phi

После прохождения прежней пары ML4 дошёл до guest PS `62b558eacebed7a3` / native `f33aa6d0f69eb2d1` и строго отклонил graphics LDS. Дизассемблер показывает три DS_WRITE_ADDTID_B32 с offsets 0/256/512 и соответствующие DS_READ_ADDTID_B32; reservation `extra_lds_size=2` = 256 dwords. Все адреса — lane*4+bank. Записи в block131 доминируют над чтениями404/440/458.

Реальная причина отказа доказательства: writer predicate `%15531`, reader `%33545 = Phi(%15531, %20565)`, `%20565 = Phi(%15531, self)`. Это один и тот же EXEC mask, переживший цикл. `PreparePixelLaneStorage` теперь нормализует только trivial boolean Phi с общим реальным incoming value; не меняет IR или реальное выполнение. Затем прежние проверки владения, reservation, dominance и predicate implication применяются без ослабления. Phi без определённого входного значения и меняющиеся masks не считаются доказательством. Общие/межполосные/атомарные LDS и reads-before-store остаются reject.

## Проверки

- `tools/test_metal_pipeline.sh`: весь набор PASS с Metal API и GPU shader validation. Новые **24 vertex/mesh GPU cases**, **1536 depth+stencil pixels**, **128 color pixels**, реальные UAV counters, 64 MRT routes, 2 invalid PS-disabled mesh requests rejected; 0 failures. Включены nil PS, разные маски color consumer, CB mode0, discard, shader depth export, sample mask, dual export и EXEC_ON_NOOP.
- Старые MRT/raster/stencil XOR/arrays/read-only depth/MDI/mesh/mip/depth canonical/bounds — PASS. Тест отсутствующего активного PS и цветовых записей не допускает превращать новую fixed-function функцию в fallback.
- `tools/test_metal_coherence.sh`: PASS, включая прежние mip/physical span/depth suites и exact padding preservation.
- Полный `tools/test_metal_msl.sh` с API validation — PASS, включая старые mesh/RECTLIST, raw bits/flags, непрерывные loops, image/sampler/atomic cases. Новые LDS tests: 16 GPU/negative cases, 2048 texels PASS.
- Финальные LDS cases отдельно повторены с **API+GPU shader validation** в structured и dispatcher режимах: оба PASS. Четыре GPU случая (32/64 waves, прямой predicate и неизменяемая loop Phi), 12 отрицательных (shared neighbour, no store, insufficient predicate/reservation, changing/undefined loop predicates).
- Product build PASS. Старый `shader_cfg_tests` Apple clip-guard failure не исправлялся и не объявляется пройденным; frontend ISA/CFG не менялся.

Логи сохранены в `work/metal-mesh-link` (копии Developer kytx-link-*.log). Базовые исходники до этого этапа — `before`, плюс SHA256 baseline.json. Патч относится к этому грязному baseline, не чистому HEAD.

## Запуски и текущая проверка

ML1 PID8250 SHA3b48eab4..., log lean/ml1-075151.log: diagnostic-only, natural abort на invalid mip range.
ML2 PID8505 SHAc1ae516d..., log lean/ml2-075416.log: diagnostic-only, natural abort на исходной mesh/PS паре; реальные SPI regs выше.
ML3 PID8924 SHAe3e20c55..., log lean/ml3-075948.log: новый activation path, затем native mesh API assertion nil fragment; исправлено отдельно и проверено mesh mini-tests.
ML4 PID9244 SHAab696c78..., started08:03:24+05, log lean/ml4-080324.log: прошёл прежний link reject, последнее печатное coverage550 translated/0 unsupported, затем explicit PS LDS reject. Это не финальный общий shader count. EA Servers frame306 и Let's Play frame350 читались нормально; два отдельных ручных j закрыли именно наблюдаемые экраны. После Let's Play frame366/393 оставался чёрный кадр, затем процесс сам завершился на LDS. Меню/матч и старые тени не подтверждены.

**ML5** PID9723, started **2026-11-10T08:20:27+05:00**, main/s3 SHA256 **`d2784ebd963ee403ec0be67c16c4ed87a93ab7029ecd0db896ba35b6c50715a0`**, log `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/ml5-082026.log`. Запущен после LDS proof fix. Визуальный инструмент сообщил заблокированный Mac; пользователю отправлен запрос на ручную разблокировку. Результат следующей сцены пока не зафиксирован; актуальный PID/log надо проверить перед CUA, чтобы не перезапустить мёртвое приложение.

Режим запусков: один экземпляр, capture/diagnostic layers в игре выключены, исходный40GB guard; без timer kills и автоматических input loops. Временный `extra_env_s1.sh` восстанавливается побайтно; два ручных j только в ML4 после наблюдения. Никому сигналом процесс не завершали.

## Остающиеся границы

Весь PS5 не реализован. Нет доказанного исправления старых теней/освещения/3D артефактов. Shared PS LDS общего вида остаётся unsupported; нынешнее расширение допускает только доказанное scalar replacement per-lane storage. Ordinary mesh mapping aliases и raw interpolation modes требуют отдельной проверки. Сохраняются прежние ограничения canonical BC, depth MSAA/mips и независимых физических read/write Z/S planes, HTILE clear heuristics и legacy image ownership. Не считать список закрытым по одному удачному запуску.

Repo `/Users/tx/Developer/KyTx`, branch `metal-native-checkpoint`, HEAD `9534b33718bd10104a4baf708758d91f61e2ef8d`; inherited dirty tree сохранён. Ledger CLI недоступен. Без staging/commit/push/reset.
