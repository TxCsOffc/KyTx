# Сэмплеры, mip-представления и сверка shader paths — 2026-11-09

Продолжена та же сессия Claude `52cdae17-1a05-40ea-912d-91012d435db7` и задача реальной эмуляции KyTx. Предыдущие dirty/untracked изменения сохранены. Коммитов и публикаций нет. Никаких новых NaN/TAA sanitizers, пропусков шейдеров или фиктивных текстур для исправления картинки не добавлено. Прежние fallback paths эмулятора ещё существуют.

## Эталон и источники

Последний `git fetch origin --prune` после возобновления проверил `KytyPS5/KytyPS5` main: **e1292528ffb6a71b758f58373c31602b25b483d4**, 2026-11-09T05:39:16+01:00. SPIR-V image emitter и sampler cache не изменились относительно проверенного снимка16a7bbe3; их семантики по-прежнему актуальны. Четыре новых коммита (depth mip-stats id, resource-plan traversal, present locks, PSSR temporal inputs) просмотрены и отмечены для следующего отдельного переноса. Локальный read-only reference: `/Users/tx/Developer/external-ref/KytyPS5_KytyPS5/repo`.

По просьбе пользователя сравниваем один IR и одинаковые данные ресурсов через прямой MSL и **production IR → SPIR-V → SPIRV-Cross 1.4.363 → Metal**. Это настоящий GPU differential тест шейдерной части Vulkan/MoltenVK; Vulkan driver/runtime и KosmicKrisp этим тестом не запускаются. Это не полная сборка свежего upstream frontend: актуальные операции адаптированы к нашему BindingLayout. Генерируемые SPIR-V и обе версии MSL сохранены для проверки.

Первичные источники:

- [KytyPS5: отдельные comparison sampler variants](https://github.com/KytyPS5/KytyPS5/commit/bc553dca0e0d7cb8f667ebe5f9820061c15c61de) — перенесён смысл изменения без замены всего frontend.
- [Актуальный upstream SPIR-V image emitter](https://github.com/KytyPS5/KytyPS5/blob/16a7bbe3cb463519035e8a953e3d28e92bef462a/src/graphics/shader/recompiler/backend/spirv/spirvEmitterImage.cpp) и [sampler cache](https://github.com/KytyPS5/KytyPS5/blob/16a7bbe3cb463519035e8a953e3d28e92bef462a/src/graphics/host_gpu/renderer/cache/samplerCache.cpp).
- [Vulkan depth comparison](https://docs.vulkan.org/spec/latest/chapters/textures.html#textures-depth-compare-operation) и [LOD selection](https://docs.vulkan.org/spec/latest/chapters/textures.html#textures-level-of-detail-operation) — reference op texel, comparison до фильтрации, bias/clamps и min/mag.
- [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) и [Metal Shading Language specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf) — нативные depth sample/gather операции.
- [AMD PAL gfx9Device.cpp](https://github.com/GPUOpen-Drivers/pal/blob/dev/src/core/hw/gfxip/gfx9/gfx9Device.cpp) — allocation mip count отдельно от base/last view; uncompressed BC views считают размеры в блоках; compareFunc передаётся непосредственно.

## Реализовано

1. **Отдельный Compare sampler class.** Обычное чтение и Dref одного гостевого S# получают отдельные backend variants с общим descriptor snapshot; comparison больше не меняет обычный sampler. GPU tests дополнены проверкой refresh и integer variants.
2. **Правильный Dref в прямом MSL.** Все восемь compare functions, reference первым операндом, сравнение отдельных texels до PCF; point/linear, mip filtering, LOD bias/clamps/min-mag, explicit LOD, gradients, implicit fragment derivatives, signed offsets, arrays и pixel coordinates. Сняты фиксированный GEQUAL и экспериментальный `KYTY_MSL_CMP_FUNC`.
3. **Нативная операция Metal как у SPIRV-Cross.** Для совместимых normalized 2D/2DArray textures используется depth handle cast + `sample_compare`/`gather_compare`. Это сохраняет нативные PCF/anisotropic footprints. Формат проверяется у реально привязанного Metal-ресурса и передаётся в stage constants. R32Float и depth formats совместимы; остальные color formats обрабатываются через реальные per-texel comparisons. Для dynamic sample offsets и размеров без Metal depth type также используется явный footprint.
4. **IR result shape.** Dref sample возвращает scalar в x и нули в неиспользованных компонентах. Репликацию в гостевые VGPR делает frontend. Differential тест обнаружил прежнюю ошибочную репликацию внутри MSL emitter; fragment fixture теперь повторяет настоящее frontend поведение.
5. **Sampler ABI и host filtering.** Point sampler aliases192..223, четыре исходных S# слова на слот, отдельные graphics/compute constants, per-image native-depth capability. Raw argument-buffer encoding проверен против MTLArgumentEncoder. FILTER_MODE min/max использует настоящий Metal reductionMode на macOS26+; unnormalized sampler использует mag filter при LOD0, как требует API.
6. **Реальная причина одного unbound resource исправлена.** Игра читает 64×64 RGBA32UInt block view с LAST0/MAX8. Старый код создавал девять Metal mip levels для 64×64 и отклонял ресурс. Теперь физическая цепочка из девяти уровней сохраняется при вычислении guest offsets, а host texture содержит только достижимые view levels. Обе величины входят в cache identity. Для BASE=LAST=7/8 за естественной длиной uint-chain выбранный физический tail mip декодируется в настоящий отдельный Metal texture1×1. Его first_level также входит в cache identity, поэтому разные1×1 tail levels не смешиваются. MAX_MIP не обрезается, данные/padding не подменяются. R128 правило проверено отдельно.
7. **SPIR-V sample offsets.** Из свежего upstream адаптированы signed-six-bit constant texel offsets и правильная маска ConstOffset. Раньше sample-инструкции их теряли. Dynamic offsets также передаются настоящим Offset operand. Явный target contract `spirv_for_metal` проходит через emitter, production optimizer и validator: Metal SPIRV-Cross поддерживает эту операцию, тогда как Vulkan требует [maintenance8](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_maintenance8.html). Меняется только разрешение конкретного operand, все остальные проверки сохранены. Direct MSL реализует dynamic comparison offsets через per-texel footprint. Non-LOD gather больше не декодирует UINT32_MAX sentinel как номер компонента.

8. **SPIRV-Cross cache identity.** В ключ Metal-функции включён hash фактического специализированного SPIR-V module. Bind-side shader_hash использует тот же ключ; изменения translated operations больше не переиспользуют старую функцию по одному guest hash.

## Проверки

- Main build и CPU resource materialization/tracking/optimizer suites — PASS.
- Shader differential — **2496 совпавших GPU cases**; **23 SPIR-V modules** проходят `spirv-val --target-env vulkan1.3 --allow-offset-texture-operand`. Без флага четыре dynamic sample modules ожидаемо отклоняются; полный JSON содержит оба результата. Это Vulkan validator с точечным разрешением операции Metal, а не утверждение включённой maintenance8 на Vulkan device.
- Comparison matrix — **4360 isotropic GPU cases**, **128 anisotropic cases (2×/4×/8×/16×)**, **2352 независимые native depth GPU cross-checks**, включая R16Unorm/R16Float/RG32Float/RGBA16Float/RGBA32Float.
- Fragment mini-program — **4096 GPU components**, varying UV/implicit LOD/min-mag/bias/clamps — PASS.
- Mip views — **18 GPU readbacks** разных активных диапазонов BC1/BC7 allocation ↔ uint blocks; ошибочное усечение physical chain меняет **98285 bytes**. Padding/source unchanged; старые tiling/volume/alias suites — PASS.
- Negative control filter-depth-before-compare воспроизводит ошибку (`CPU=0.7, MSL=1`, exit1), как и должен. Это только тестовый флаг.
- `MTL_DEBUG_LAYER=1 bash tools/test_emulation_memory_stage.sh` — PASS: PM4, coherence, clear, footprints, guest views, tiling/volume/mips, ordinary samplers и весь native MSL suite (включая4806 stride-swizzle GPU results). После дополнения tail mip/target contract повторены затронутые GPU tiling/mip tests, same-IR differential с настоящим production optimizer, shader optimizer CPU suite и main build.
- `git diff --check` и reverse-check focused patch — PASS.

Разработческие находки: min/mag bit packing первоначально был перепутан в helper и fixture; ordinary sampler oracle обнаружил это, исправлено до игры (mag20/min22). Наивный native depth-cast для всех color formats провалился на R16Unorm fractional mip test (`0.427696` против `~0.5087`); после введения фактической format capability этот тест проходит. Эти результаты сохранены, визуальная яркость не использовалась критерием правильности.

## Игра и точная сборка

Main/s3 SHA256: **68ff21a1318b797a286748670917c7d123490633bf5943fe08a009b3d4f0c529**. HEAD `9534b33718bd10104a4baf708758d91f61e2ef8d`, branch `metal-native-checkpoint`.

**Sp4** запущен13:30:47 Asia/Tashkent, PID **39864**, с исходными c9 flags NO_UMA/GDS/CUBE и guard40GB. Один эмулятор. Десятиминутный сценарий завершился без остановки игры; исходный extra_env восстановлен побайтно. Процесс и точный hash s3 проверены после завершения сценария, игра оставлена работающей.

Матч Manchester United — Liverpool подтверждён на screenshot0400: игровое время02:50, счёт0:0, titleframe8141, HUD28.57FPS. Screenshot0530 показывает угол поля и Corner Kick tutorial; на крупном плане виден прежний дефект одежды. Подсказки пройдены обычным confirm. Screenshot0600 показывает тёмный POV/replay кадр; это не доказательство полного визуального исправления. FPS gain не заявлен: крупные планы и другие сцены заметно медленнее.

В проверенном журнале **0 ненулевых unbound descriptors**, **0 GPU faults/command-buffer errors/progress timeouts/fatal emission failures**. Новый tail-view путь действительно выполнен: есть `[TexMip] ... first=7/8`, в том числе128×64 raw-block views с10 physical mips. Прежде падавший guest shaderc468ee1c8c104d8c теперь достигает sampler binding. Это runtime evidence закрытых случаев, а не утверждение завершённой coherence.

Log: `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sp4-133046.log`. Screenshots: `/Users/tx/Developer/KyTx/.porting/work/out/sp4/`. Проверенные metadata: `work/sampler-stage/game-sp4.json`; game source/hash manifest в outputs.


Промежуточный sp3 завершился на добавленном нами чрезмерно широком Vulkan-only gate динамического sample offset (`c468ee1c8c104d8c`). Это была реальная ошибка проверки target contract, не старый визуальный артефакт. Gate исправлен описанным выше способом и покрыт мини-программами. Также в sp3 найден корректный BASE=LAST7 tail view, который теперь реализован и проверен. Не объявлять sp3 успешным матчем.


Финальная дополнительная проверка: `work/sampler-stage/sp4-left-running.png`, titleframe14855, матч **24:53**, MUN1:0LIV. Старый дефект вытянутой геометрии/одежды виден на угловом. HUD9.11FPS на этом крупном плане; resource allocation HUD вырос за прогон (Metal54.88GB/App37.94GB), это не измерение физической памяти и не доказательство причины замедления. Отдельный `footprint -p39864` после финального кадра измерил **phys_footprint36GB**. Исходный guard40GB не изменялся.

## Оставшиеся границы и следующая очередь

Полная PS5 эмуляция не завершена. Отдельно нужны: независимый Z_FILTER, custom border/corner rules, анизотропия software footprints при dynamic offsets/несовместимых color formats, gather с явным LOD, D16/A16 дополнительные matrices, cube/depth cases, диапазоны нескольких повторяющихся1×1 tail mips в одном Metal view и runtime 3D view integration. В памяти остаются общий owner/version/coherence adapter и DCC/CMASK/HTILE. Следующие shader mismatches следует сводить к тому же IR/resources и сверять с актуальным Vulkan path; использовать пользовательское наблюдение о MoltenVK/KosmicKrisp как направление проверки, а не доказательство конкретной причины.

Пользователь дважды уточнил, что тёмное освещение/шум и другие видимые артефакты были раньше. Не приписывать их текущим изменениям без воспроизводимого A/B. Этот этап не объявляет тени или все визуальные дефекты исправленными и не заявляет FPS gain.

## Артефакты

В Codex workspace `/Users/tx/Documents/Codex/2026-11-09/new-chat`:

- `outputs/sampler-emulation-2026-11-09.patch` — только изменения этого этапа от сохранённого dirty baseline, 28 файлов.
- `outputs/sampler-emulation-source-state.json` — source hashes, HEAD/upstream/build, затем game metadata.
- `outputs/sampler-emulation-tests.log`, `outputs/sampler-emulation-negative.log` — проверки.
- `outputs/sampler-emulation-spirv-validation.json` — 23 модуля с разрешённым Metal offset contract и строгим Vulkan negative control.
- `work/sampler-stage/` — полные development logs, baseline `before/`, generated shader artifacts, game metadata/log pointers.

Ledger CLI/store отсутствуют; актуальные итоги записаны в STATE/HANDOVER/worklog.
