> **Уточнение Codex 2026-11-09:** ниже сохранён исторический аудит Claude. Mlock больше не пустой успех; ES/LS 0x0CA/0x14A теперь сохраняются/декодируются, но полный GS/HS backend не завершён. Другие ignore handlers в PM4 остаются, поэтому фраза «тихо игнорируемых регистров нет» неверна. Unsupported=0 означает покрытие перевода встреченных инструкций, а не доказательство всех аппаратных семантик. Актуальный пакет и ограничения: `.porting/REPORT-claude-continuation-2026-11-09.md`.

# Исследование внешних проектов: что у них есть, а у нас нет (9 ноября 2026)

Источники (клонированы только для чтения в `/Users/tx/Developer/external-ref/`, GPL-2.0, совместимо с нашей Kyty-базой):
`KytyPS5/KytyPS5` (HEAD 16a7bbe3, 8 ноября), `boykopovar/AnyPS5`, `sharpemu/sharpemu` (C#, пока не разбирался). Нашу базу в истории KytyPS5 нашёл по хэшу файла:
`bd611bfe` (3 октября). С тех пор в upstream **904 коммита** (387 «shader», 130 «graphics», 40 «kernel», 40 «gpu», 14 «audio», 12 «ngs2», 12 «agc», 11 «net»…).

## 1. Насколько мы отстали и что можно слить

Три файла разбора шейдерных ресурсов у нас **нетронутый снимок базы**, а upstream их многократно переписал (`ResourceTracking.cpp` 115 коммитов,
`ResourceMaterialization.cpp` 102, `ShaderIR.h` 82). Но сердцевина этой работы (≈2/3 shader-коммитов) это **разрешение динамических дескрипторов**
(GPU-выбираемые таблицы материалов, bindless, waterfall) и **смена представления привязок**: `BindingLayout.descriptors` заменён на `Bindings`
(`descriptor_counts` + push-данные), `BindingLayout.*`, `shaderBindings.cpp`, `SpirvOptimizer.*` удалены. Наш MSL-эмиттер и Metal-рендерер целиком стоят на старом
`descriptors`, поэтому слияние shader-фронтенда = перенос бэкенда, а не merge.

Сухой прогон трёхстороннего слияния (скрипт `merge_dry.py`: база = наш commit e5afd173, наши файлы, upstream HEAD):

| Слой | берётся у upstream как есть | чистое 3-way | новые | конфликты (файлов/фрагментов) |
|---|---:|---:|---:|---|
| `graphics/shader` | 57 | 4 | 3 | 17 файлов / ≈38 фрагментов (в т.ч. ShaderIR.h: наши `guest_hash`, `native_msl`, `BindingLayout`) |
| `libs` | 37 | 6 | 14 | 6 файлов (agc.cpp/.h, ajm.cpp, libAudio2.cpp, libNet.cpp 8, network.cpp 14) |
| `common` | 29 | 1 | 11 | 3 файла; 6 файлов удалены upstream (dateTime, debug, hash, timer) |
| `loader` | 9 | 1 | 3 | runtimeLinker.cpp |
| `kernel` | 5 | 2 | 1 | memory.h, fileSystem.*, **memoryAddressSpace.inc (5)** — тут наша нативная UMA-память |

Вывод: **libs/common/loader сливаются дёшево** (десятки файлов, ~15 конфликтных фрагментов), kernel требует аккуратности (наша память), shader-фронтенд
дорого. Не делал ни одного массового слияния без решения по порядку: бэкап `src/tests/tools/CMakeLists.txt` лежит в
`.porting/work/backup-before-upstream-merge-053330/`.

Важно: не всё, что у upstream названо «fix», нужно нам. Пример: `b6bd9eca` (inline-константа 1/(2π) на 16-битных операндах) обходит округление `PackHalf2x16` драйвера AMD;
на Metal `half(x)` округляет к ближайшему и ошибки нет. Значит, переносить надо по уликам, а не по списку.

## 2. Реальный список импортов FC27, оставшихся заглушками

Запуск `kyty_emulator --game … --printf-direction Console` на 28 с даёт 115 строк `Relocate: unresolved … patched to stub`. Имена восстановлены хэшированием
идентификаторов из четырёх кодовых баз (алгоритм SHA-1 + соль, проверен на `sceKernelUsleep=1jfXLRVzisc`): **101 из 113 NID названы**.
(База имён AnyPS5 не публична: в репозитории только 5 387 их реализованных экспортов; из наших заглушек она знает лишь 3.)

Заглушек всего 115, но **за 10-минутный сценарий меню→матч вызвана одна** (`WxLcN76q19o`, у AnyPS5 тоже безымянная `sceNpUnknown_…`, у них «not implemented»).
Состав: Vdecsw 15 (программный видеодекодер), libkernel 14, VideoRecording 9, NpCommerce 7, NpWebApi2 6, диалоги PlayerSelection/Invitation/WebBrowser/PlayGo 19,
Voice/ProprietaryVoiceChatHelper 8, ContentExport 4, LoginService 3, Http 3, Json2 3, SystemService 3, NpPartner001 2 (`sceNpHasEAAccessSubscription`,
`sceNpEAAccessTerminate`), NpPremiumAddcont 2 (безымянные), Psml 2 (`scePsmlMfsrCreateContext1100`, `…GetDispatchMfsrPacket1100`, апскейлер),
NpTrophy2 2, Coredump 2, Net 2, NpManager 2, прочее по 1.
У KytyPS5 реализованы 23 из них (Vdecsw 12, SystemService 2, Psml 2, NpCommerce 2, NpWebApi2 2, libkernel `rmdir` и `AioWaitRequests`).

**Сделано сегодня** (все собираются, тест `sync_on_address_tests` проходит):
- `sceKernelSyncOnAddressWait8` (`dFym5b41JLU`) и `…Wait16` (`XgVs4jRehcY`): новые `SyncOnAddress::Wait8/Wait16`, на Linux узкие ячейки ждут через переносимый реестр, Wake будит оба; тест `TestNarrowCells`.
- `scePthreadTestcancel` (`LapIb799SSE`): функция уже была в kernel/pthread, не была экспортирована.
- POSIX `rmdir` (`c7ZnT7V1B98`) поверх существующего `KernelRmdir`.
- `sceKernelMlock` (`3k6kx-zOOSQ`): успех без действия для валидного диапазона (память гостя не вытесняется).

Осталось из списка (нужна семантика/реализация KytyPS5): Vdecsw (если игра будет декодировать видео программно), `sceKernelChmod`/`Utimes`/`AioCancelRequest`/`AioWaitRequests`,
`sceKernelInternalMemoryGetModuleSegmentInfo`, три безымянных libkernel-NID, Psml MFSR, SystemService player dialog, NpCommerce dialog.

## 3. Что говорят про EA-игры на том же Frostbite (issue #1044 FC27, #1101 NHL 27, коммиты)

- **NHL 27**: падение `unknown sh reg 0x14a`. Upstream a7491cc6 игнорирует регистры `0x0CA` и `0x14A` («GS/HS-front allocation metadata»). **Сделано**: оба в индирект-таблице
  `g_hw_sh_indirect_func` (работает и для записи нескольких регистров подряд). У нас FC27 их не писал, но другой режим мог бы уронить парсер.
- **UFC 6**: быстрая очистка CMASK цели R8_UINT каждый кадр терялась (upstream b2c9ec76). **Сделано**: `DecodeRegisterClear` вынесен в `metalClearDecode.h` и расширен R8Uint, RG8Uint, R16Uint,
  R16Float/Unorm, RG16Unorm, RGBA16Unorm/Uint/Sint, R32Float, RG32Float; 32-битные целые отказываются, а не округляются через float. Тест `tests/metalClearDecodeTests.mm`
  (`tools/test_clear_decode.sh`, 1200 сравнений с независимой моделью) входит в `tools/test_emulation_memory_stage.sh`.
- **NHL 27, белая одежда и толпа**: у них лечилось увеличением `KYTY_DBG_LOOP_CAP` 1024→65536 (предохранитель циклов шейдера). У нас такой же в MSL-эмиттере
  (`MslLoopCap`, 8192 на вход в цикл, в графике при превышении `return` из всего шейдера). Лимит выше и считается иначе, прямого доказательства влияния на FC27 нет; счётчик срабатываний для
  графических стадий не реализован. **Идея**: счётчик в отладочном буфере, чтобы измерить, а не гадать.
- **FC27 #1044**: падение на стековом канарейке `fs:[0x28]` (FS base 0), `sceKernelWritev` не реализован (не сохранялся INDEX сейва), `FULLGAMEUNLOCK00`/`FCLIVEUNLOCK0000` → `0x817d0007`,
  compute-шейдер `1e542024957cf5df` с динамическим `S_SETPC_B64`. У нас: Writev есть (`kAt6VDbHmro`), kqueue/kevent/NpPartner есть, игра доходит до матча.
  Не проверено: сохраняются ли настройки между запусками (перед матчем нужно нажимать «Ок» на диалогах при каждом запуске).

## 4. AnyPS5: аппаратный оракул (самое ценное)

- `docs/dev/HW_ORACLE.md`, `tools/hw-oracle/`: несколько строк RDNA-ассемблера исполняются на реальной AMD GPU (gfx103x ≈ PS5 gfx1013), результаты (по строке входов на lane) закрепляются как ожидаемые
  значения в `core/libs/prx/libSceAgcDriver/tests/execution/` (**624 теста**, 345 из них чисто вычислительные: `Code[]` машинного кода + входные строки + ожидаемые значения).
  Семантика режимов float (ieee, dx10_clamp, denorm32/16, round32/16, fp16_overflow), NaN у min/max/clamp, деление и fixup, DPP, DS, атомики, форматы буферов, D16 и т.д.
- Это **эталон, которого у нас не было**: можно прогнать те же машинные коды через наш конвейер (декодер → MSL → Metal) и сравнить. Тесты написаны под их API
  (`ShaderRecompiler::Recompile(RecompileRequest)` + `VulkanDevice::Dispatch`), поэтому нужна прослойка: подмена `Recompiler.hpp`/`VulkanTestDevice.hpp`/`Draw.hpp(Require)`
  своей реализацией на `TranslateProgram` + `Msl::EmitProgram` + Metal compute. Оценка: прослойка ≈ 400–600 строк плюс разбор нескольких десятков тестов; результат даёт список
  реальных расхождений инструкций (NaN/denormal/clamp/min-max, которые вероятнее всего и дают белый шум и неон).
- «Shader forms decoded 1460/1460» и «Unsupported-throw sites 148» у них считаются из кода (`tools/`): годится как образец для нашего счёта непереведённых форм.
- Статус AnyPS5: библиотеки 33,23 % (12 589 из 37 890 функций), форматы изображений Sampled 82/159, Typed buffer 81/159.

## 5. Данные наших прогонов (c4–c9), не доказывающие причин артефактов

См. `AUDIT-gaps-from-game-runs-2026-11-09.md`: null-дескрипторы (865+445) законны; один реальный необработанный compute-слот 64x64 RGBA32UInt tile 5;
`[MSL-coverage] unsupported=0`; счётчики `AliasConvert` (`KYTY_LOG_ALIAS_STATS=1`): ≈14 000 вызовов на кадр, ≈2 кодируемых преобразования на кадр, 1–3 отказа «старше 2 кадров» для
чтения в другой раскладке на кадр.
**Отброшено как артефакт счётчика**: «84 % draw не выполняется» (счётчик `executed` увеличивается только в ветке multi-draw).

## 6. Рекомендуемый порядок

1. Прослойка к аппаратному оракулу AnyPS5 (дешёвый старт: F32/F16 NaN/denormal/min-max/clamp, затем DPP/DS/атомики) — реальные расхождения инструкций с железом.
2. Слияние `libs`+`common`+`loader` с upstream (механически, с проверкой сборкой и прогоном), затем kernel без `memory*`.
3. Счётчик срабатываний предохранителя циклов в графических стадиях.
4. Решение по shader-фронтенду upstream: полный перенос с адаптацией MSL-бэкенда (дорого) либо точечный перенос семантических исправлений по уликам.
5. Этап 1 Codex (адаптер физических владельцев → рендерер) — по-прежнему не подключён.
