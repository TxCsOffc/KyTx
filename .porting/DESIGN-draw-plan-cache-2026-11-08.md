# DESIGN: кэш «draw plan» для потока командного процессора KyTx (PM4 -> Metal)

Дата: 2026-11-08. Автор анализа: Claude (только чтение; эмулятор не запускался, исходники не менялись).
Ветка `metal-native-checkpoint`, FC 27, M5 Max. Скрипты анализа: `.porting/tools/draw-plan-cache/` (README там же).

---------------------------------------------------------------------------------------------------

## 0. Цифры сначала (коротко)

**Куда уходит поток CP (профили r11, r12, 1 мс/сэмпл, поток с `CommandProcessor::Process`):**

| | r11 | r12 |
|---|---|---|
| сэмплов в потоке | 1809 | 2362 |
| `ProcessPm4` | 1216 (67.2%) | 1611 (68.2%) |
| draw-путь (`DrawIndex`+`DrawAuto`+`DrawIndirectMulti`) | 590 (32.6%) | 930 (39.4%) |
| dispatch-путь (`DispatchDirect`+`DispatchIndirect`) | 475 (26.3%) | 540 (22.9%) |
| остальной PM4 (SetContextReg, Flush, WaitRegMem ...) | 151 (8%) | 141 (6%) |

Привязка к кадру (предположение: сцена заголовка держит 30 FPS, значит окно r12 = ~71 кадр, r11 = ~54; это согласуется с 22 мс/кадр из задачи: 1611/71 = 22.7, 1216/54 = 22.5; состав кадра 2100 draw + 250 dispatch взят из PassTrace r6/r8/r9, другой прогон, поэтому +-15%):

| на кадр | r12 | r11 |
|---|---|---|
| draw-путь | 13.1 мс = **6.2 мкс/draw** | 10.9 мс = 5.2 мкс/draw |
| dispatch-путь | 7.6 мс = **30 мкс/dispatch** | 8.8 мс = 35 мкс/dispatch |
| прочее PM4 | 2.0 мс | 2.8 мс |

**Главный вывод 1: «11 мкс на операцию» - это среднее по двум разным популяциям.** Dispatch в 5-6 раз дороже draw и составляет 11% операций, но 33% времени потока. Draw-plan кэш лечит только draw-путь.

**Главный вывод 2: стоимость не только в «нашей логике».** По владельцу сэмпла (`a5.py`, r12, весь `ProcessPm4`): собственный код KyTx (`kyty_emulator_s3`) 37.2%; Metal API (драйвер AGX + обёртки GPUToolsCapture) 27.6%; ожидание HUD (`FPMTLMetrics` -> `kevent_id`) 12.1%; ObjC runtime (retain/release/msgSend) 8.9%; memcpy/memset 5.0%; mutex 2.8%; часы (`clock_gettime`, `mach_continuous_time`) 2.2%; printf 1.8%. Все профильные прогоны шли с `MTL_HUD_ENABLED=1` (`.porting/work/extra_env_s1.sh`): нижняя оценка вклада HUD/capture-слоя - **13.5% (r12) / 16.6% (r11) от `ProcessPm4`** (кадры `GPUToolsCapture`, всё под `FPMTLMetrics`/`GTTrace*`/`GTMemWatch*`). Создание compute-энкодера в профиле стоит 6.3 мкс (112 мс / 17 750 dispatch) против 1.6 мкс в `metal_encode_cost.mm` (режим B, без HUD): ~4x раздувание. Прежде чем судить о выигрыше любого кэша, нужен профиль без HUD.

**Кэшируемость (PassTrace, 3 лога, 8 пар соседних кадров):**
- На кадр 1700-2290 draw, 248-252 dispatch, ровно **126 смен render pass** (122 в r9) и 36-37 серий подряд идущих dispatch (по ~7).
- Различных ключей на кадр: `(vs,ps)` 186-194; `(vs,ps,форматы RT/DS)` 182-198; `(vs,ps,RT/DS с адресами и размерами)` 204-219; с адресами входных картинок 320-369; с адресами буферов 1659-2238 (то есть ~1 на draw: константы каждый раз по новому адресу).
- Доля draw кадра 2, чей ключ был в кадре 1: `(vs,ps,форматы RT/DS)` **99.7-100%** во всех 8 парах (включая кадры со сменой динамического разрешения); тот же ключ с адресами и размерами - 96-100% в 6 парах из 8 и 34-35% в двух парах со сменой DRS 1920x1080 -> 1600x904; ключ с адресами буферов - 1.8-23%.
- Внутри одного кадра (первый кадр окна, без прогрева): 88-91% draw повторяют уже виденный `(vs,ps,форматы RT/DS)`; топ-10 планов покрывают 56% draw, топ-40 - 78%; планов с единственным draw нет.
- Dispatch: `cs` - 93-100 различных на кадр, 97-100% повторов; `(cs,grp,io-адреса)` - 11-81% (зависит от ротации целей).

**Рекомендованный первый этап:** «Этап 0+1» одним патчем: убрать чистые потери (ленивая метка `fault_label`, дедуп `BindGuestAddressSpace`, счётчики времени в `MaterializeCompiledSrtExperiment`, второй `GraphicsImageFor` в Bind, массивы `ArgCapture`) и ввести `StagePlan` с ключом `program->shader_hash` (неизменяемый, без инвалидации). Ожидание: -2.0..-2.9 мс/кадр (9-13% потока), без риска для картинки. Затем `PassStatePlan`+`PipelinePlan` (этап 2).

---------------------------------------------------------------------------------------------------

## 1. Метод и оговорки

- Разбор `sample`: `.porting/tools/draw-plan-cache/samp.py`. Поток CP - тот, где есть `CommandProcessor::Process` (r12: `Thread_8398065`, 2362 сэмпла, из них 725 - ожидание в `CondVar::WaitFor`, 1629 в `ThreadRun`).
- «Включительно» (incl) считается без двойного счёта рекурсии; «собственно» (self) = count - сумма детей. Проценты - от всего потока (как в задаче) и от группы.
- Имена в задаче «MetalRenderer::Draw» в коде нет: это `MetalRenderer::DrawIndex` (metalRenderer.mm:7816), `DrawAuto` (:8209); 586-924 из задачи = их сумма (r11 334+252, r12 642+282).
- Профильные прогоны скриптованные: включены `MTL_HUD_ENABLED=1` и `KYTY_PASS_TRACE_TRIGGER`. `PassTraceActive()` (metalRenderer.mm:2479) при заданном триггере зовёт `steady_clock::now()` на каждый вызов (~1% потока в r11; в обычном запуске возвращает false сразу). Эти артефакты в числах ниже оставлены, но отмечены.
- Оценки выигрыша ниже - оценки, а не измерения; каждая привязана к числу сэмплов. Пересчёт: в r12 1 сэмпл draw-группы = 1 мс / (71 кадр x 2100 draw) = 6.7 нс на draw; 1 сэмпл dispatch-группы = 56 нс на dispatch; 1 сэмпл = 0.0141 мс/кадр (r11: 0.0185 мс/кадр).

---------------------------------------------------------------------------------------------------

## 2. Задача 1: где уходят мкс

### 2.1 Потомки верхнего уровня (включительно, сэмплы / % потока)

**Draw-путь** (потомки `MetalRenderer::DrawIndex/DrawAuto`, группа r11 = 586, r12 = 924):

| # | функция | r11 | % потока | r12 | % потока | % группы (r11/r12) |
|---|---|---|---|---|---|---|
| 1 | `BindMetalShaderResources` | 190 | 10.5 | 340 | 14.4 | 32 / 37 |
| 2 | `EnsureActiveRenderPass` (из них `BeginRenderPass` 77/110) | 106 | 5.9 | 150 | 6.4 | 18 / 16 |
| 3 | `PipelineCache::GetGraphicsPrograms` (из них SRT-walker 49/101) | 65 | 3.6 | 137 | 5.8 | 11 / 15 |
| 4 | `GraphicsImageFor` через `PrepareGraphicsImages` (metalRenderer.mm:7660) | 82 | 4.5 | 107 | 4.5 | 14 / 12 |
| 5 | `MetalDrawExecutor::DrawIndexed`+`Draw` (сам вызов отрисовки) | 27 | 1.5 | 36 | 1.5 | 5 / 4 |
| 6 | state-сеттеры (`SetRenderPipeline`, `SetDepthStencil`, `SetRasterizerState`, `SetViewport`, `SetScissor`, `GetOrCreate*`) | ~40 | 2.2 | ~53 | 2.2 | 7 / 6 |
| 7 | `PrepareMultiDrawArguments` | 17 | 0.9 | 14 | 0.6 | 3 / 2 |
| 8 | `snprintf` (метка `fault_label`, строится на каждый draw) | 11 | 0.6 | 16 | 0.7 | 2 / 2 |

**Dispatch-путь** (`MetalRenderer::DispatchCompute`: r11 471, r12 539):

| функция | r11 | % потока | r12 | % потока |
|---|---|---|---|---|
| `MetalDrawExecutor::BeginComputePass` (энкодер на каждый dispatch) | 135 | 7.5 | 112 | 4.7 |
| `PipelineCache::GetComputeProgram` (из них SRT-walker 68/88) | 81 | 4.5 | 100 | 4.2 |
| `ComputeGuestImageFor` (+`DetileToLinear` 15/17 - CPU-детайлинг) | 31 | 1.7 | 59 | 2.5 |
| `MetalDrawExecutor::Dispatch` (encode) | 16 | 0.9 | 30 | 1.3 |
| `MetalComputeIndirect::Encode` | 21 | 1.2 | 29 | 1.2 |
| `UseResource` | 24 | 1.3 | 16 | 0.7 |
| `SnapshotBytes` (копия констант) | 21 | 1.2 | 15 | 0.6 |
| `NoteUniformMetadataFill` (self) | 9 | 0.5 | 15 | 0.6 |

**Внутри `BindMetalShaderResources`** (r12, 340): `SnapshotBytes` 33 (копирование констант; ровно `AllocateDynamic`+memcpy; под HUD ещё `-[CaptureMTLBuffer contents]`->`GTMemWatch_registerBuffer` 37 в draw-группе), `GraphicsImageFor` во второй раз 40, `BindGuestAddressSpace` 30 (!), `GetBufferForGuestRange` 34 (из них `TryGetContiguousBackingOffset` 19), `UseResource` 26, `StageSampler` 13, objc_msgSend/retain/release из самой Bind 63, memset 12.

### 2.2 Восемь самых дорогих «получателей» (по включительным сэмплам, r11/r12, % потока)

1. `BindMetalShaderResources` - 10.5 / 14.4%
2. `EnsureActiveRenderPass` - 5.9 / 6.4% (пасс-зависимо: 126 смен/кадр = ~16 мкс на пасс в r12, из них `BeginRenderPass` 12 мкс)
3. `MaterializeCompiledSrtExperiment` (draw+dispatch: walker `CompiledSrtWalker`) - 6.5 / 8.0% (117 / 189 сэмплов; в нём `EvaluateNode` self 81, `RefreshFlatBuffer` self 24)
4. `BeginComputePass` - 7.5 / 4.7%
5. `GraphicsImageFor` (подготовка картинок; плюс повтор в Bind) - 5.3 / 6.2% (96/147, внутри `GuestTexture`/`GuestTexSourceHash`)
6. `GetGraphicsPrograms` без SRT-walker - 0.9 / 1.5%; `GetComputeProgram` без SRT - 0.7 / 0.5%
7. `ComputeGuestImageFor` - 1.7 / 2.5%
8. `SnapshotBytes` (draw+dispatch) - 2.7 / 2.0%

### 2.3 Сводка по категориям (draw-путь, доля группы r11 / r12)

(`a7.py`: каждый self-сэмпл относится к первой подходящей категории по пути вызовов.)

| категория | r11 | r12 | можно ли убрать «планом» |
|---|---|---|---|
| преобразование картинок (`GraphicsImageFor`, `GuestTexture`, `LastGpuWriterTexture`, `ApplyDstSel` ...) | 15.8% | 13.3% | частично: мемо с эпохой (этап 3) |
| Bind: заполнение таблицы аргументов, sizes[], push data, сэмплеры | 11.7% | 13.3% | да (этап 0-1) |
| begin/end энкодера, command buffer (Metal) | 12.2% | 10.6% | нет (на пасс, не на draw); HUD раздувает |
| SRT-materialize (обход гостевой таблицы ресурсов) | 8.3% | 10.9% | нет (читает память гостя на каждый draw) |
| буферы: `GetBufferForGuestRange`, `SnapshotBytes`, `AllocateDynamic`, BDA | 7.5% | 10.4% | частично (кэш чанка, этап 3) |
| сам draw-вызов | 8.0% | 6.5% | нет |
| фиксированные состояния (pipeline/depth/raster/viewport/scissor) | 6.8% | 6.2% | да (этап 2) |
| диагностика/трассировка (`snprintf`, PassTrace, часы, счётчики) | 6.6% | 3.8% | да, без кэша (этап 0) |
| прочее тело Draw/Dispatch | 4.6% | 5.7% | |
| тело `EnsureActiveRenderPass` (раскладка целей, сравнение) | 4.4% | 3.7% | да (этап 2) |
| хэш свежести гостевой текстуры (`GuestTexSourceHash`) | 2.9% | 4.6% | частично |
| поиск программы без SRT (static key, hash, permutation) | 2.7% | 3.9% | да, но выигрыш мал |
| residency (`UseResource` + map дедупликации) | 3.1% | 2.8% | мелко |
| setBytes/setBuffer | 3.2% | 2.3% | нет |
| ожидание HUD (FPMTLMetrics/kevent) | 2.4% | 2.0% | артефакт HUD |

Dispatch-путь (доля группы r11 / r12): begin/end энкодера 24.0 / 18.1; сам dispatch 14.7 / 15.9; SRT 14.3 / 16.3; картинки 9.9 / 14.1; прочее 9.9 / 9.3; буферы 7.6 / 7.2; HUD-ожидание 6.7 / 6.1; residency 5.1 / 3.0; поиск программы 2.7 / 2.2.

**Что из этого «plan-able»** (статическое состояние: шейдеры, форматы целей, blend/depth/raster, раскладка ресурсов): program lookup без SRT 3.9% + фиксированные состояния 6.2% + тело `EnsureActiveRenderPass` 3.7% + доля Bind (заполнение, ~50% из 13%) + диагностика 4-7% ~ **25-30% draw-группы = 1.5-1.9 мкс из 6.2 мкс**. Остальные ~70% - ресурсы гостя (SRT, картинки, буферы), пассы и сам вызов; планом статического состояния они не берутся. Поэтому «draw plan» как «pipeline + раскладка + таблица аргументов» даёт 12-15% потока, а не 2x; больше - только с мемоизацией разрешения ресурсов (этап 3) и пакетированием compute (этап 4).

---------------------------------------------------------------------------------------------------

## 3. Задача 2: кэшируемость по PassTrace

Логи: `r9-203309` (кадры 1530, 1752, 1895, 1897, 1899), `r8-202801` (1554...1562 подряд), `r6-201323` (3807...3813 подряд). Подряд идущие кадры отличаются на 2 flip. Разбор: `pt_parse.py`, `pt_cache.py`, `pt_cache2.py`.

### 3.1 Состав кадра

| лог/кадр | draw | dispatch | смен RT/DS-набора (пассов) | draw/пасс | draw сразу после dispatch |
|---|---|---|---|---|---|
| r8 1554 | 2242 | 248 | 126 | 17.8 | 36 |
| r8 1556 | 2149 | 249 | 126 | 17.1 | 36 |
| r8 1558 | 2084 | 250 | 126 | 16.5 | 36 |
| r8 1560 | 2031 | 250 | 126 | 16.1 | 36 |
| r6 3807 | 1873 | 248 | 126 | 14.9 | 36 |
| r6 3809 | 1775 | 248 | 126 | 14.1 | 36 |
| r6 3811 | 1699 | 251 | 126 | 13.5 | 37 |
| r9 1895 | 2288 | 252 | 122 | 18.8 | 36 |

Структура очень стабильна: те же 126 пассов и ~36 серий dispatch в кадре, меняется только число draw (LOD/видимость). Среднее на draw: 4.0 входных картинок (макс. 24), 1.4 BUF + 5.5 VBUF (макс. 15 / 13), 2.1 цели RT; 22% draw без входных картинок. 8842 привязки картинок на кадр (4.1/draw) на 543 различных картинки: каждая разрешается ~16 раз за кадр; 99.8% привязок кадра 2 уже были в кадре 1 (адрес+размер+формат); 18% привязок - картинки, записанные GPU (из них 100% повтор).

### 3.2 Draw: доля кадра 2, повторяющая ключ кадра 1

| пара | draw | `vs+ps` | `vs+ps+RT/DS` (адреса, размеры) | `+входные картинки` (адреса) | `+адреса буферов` | `vs+ps+форматы RT/DS` (без адресов и размеров) |
|---|---|---|---|---|---|---|
| r9 1895->1897 | 2286 | 99.9 | 99.9 | 98.7 | 10.9 | **99.9** |
| r8 1554->1556 | 2149 | 100 | 100 | 99.4 | 23.1 | **100** |
| r8 1556->1558 | 2084 | 99.9 | **35.2** (DRS) | 34.8 | 1.8 | **99.9** |
| r8 1558->1560 | 2031 | 99.8 | 99.8 | 99.5 | 10.9 | **99.8** |
| r8 1560->1562 | 1893 | 100 | 99.9 | 99.6 | 4.0 | **100** |
| r6 3807->3809 | 1775 | 99.8 | 99.8 | 97.7 | 4.4 | **99.8** |
| r6 3809->3811 | 1699 | 100 | **34.3** (DRS) | 33.7 | 3.4 | **100** |
| r6 3811->3813 | 1674 | 99.7 | 99.7 | 96.1 | 9.7 | **99.7** |

Причина провалов 35%: динамическое разрешение, главная сцена 1920x1080 -> 1600x904 (RT0 `f90` 720 draw 1920x1080 -> 692 draw 1600x904; DS 1920x1080 -> 1600x904; половинные цели 960x540 -> 800x452). Ключ с форматами вместо размеров и адресов в этом случае не промахивается. Вывод для дизайна: **размеры и адреса целей не входят в ключ плана**.

Число различных планов на кадр: `(vs,ps)` 186-194, `(vs,ps,форматы RT/DS)` 182-198, `(vs,ps,RT/DS-раскладка с размерами)` 201-219, + форматы входных картинок 204-227, + адреса входных картинок 318-369, + адреса буферов 1499-2238. Различных RT/DS-наборов (без шейдеров) на кадр: 61-62.

Внутри кадра (повтор уже виденного ключа): форматный план 88-91%; с форматами картинок 84-87%; с адресами картинок 81-85%. Топ-1 план = 16.2% draw, топ-5 = 44.8%, топ-10 = 56.4%, топ-20 = 67.4%, топ-40 = 77.9%, топ-60 = 84.3% (196 планов в кадре r8 1556).

Совпадение с предыдущим draw того же кадра: `vs+ps` 68-75%, RT/DS 92.5-94.7%, `vs+ps+RT/DS` 68-69%. Позиционное совпадение (i-й draw кадра 2 == i-й draw кадра 1) всего 14-66%: порядок draw не стабилен, значит угадывание «по номеру draw в кадре» не годится, нужен поиск по хэшу.

### 3.3 Dispatch

| пара | `cs` (повтор) | `cs+grp` | `cs+grp+io(адрес,формат,размер)` |
|---|---|---|---|
| r9 1895->1897 | 100% (100 различных) | 75.0 | 74.6 |
| r8 1554->1556 | 99.6 | 76.7 | 76.3 |
| r8 1556->1558 | 97.6 | 50.0 | 11.2 |
| r8 1558->1560 | 100 | 81.2 | 73.6 |
| r8 1560->1562 | 100 | 84.4 | 80.8 |
| r6 3807->3809 | 100 | 56.9 | 55.6 |
| r6 3809->3811 | 97.2 | 42.2 | 11.2 |
| r6 3811->3813 | 100 | 82.0 | 80.4 |

Ключ `cs` стабилен (93-100 различных на кадр, ~2.5 dispatch на шейдер); `grp` и адреса картинок меняются с DRS/ротацией целей, поэтому они должны быть данными, а не ключом.

---------------------------------------------------------------------------------------------------

## 4. Задача 3: путь draw в коде - что стабильно, что per-draw

### 4.1 Инвентарь вычислений `DrawIndex` (metalRenderer.mm; `DrawAuto` :8209 зеркален, строки +~330)

| что делается на каждый draw | строки | зависит от | вердикт |
|---|---|---|---|
| преамбула: `TakePendingIndirectDraw`, десяток `s_trace_*`, `AllocProbe`, `UpdateCacheReport`, `MslDrawStatSubmitted`, `m_mutex` | 7816-7860 | per-draw | дёшево, оставить |
| `EnsureActiveRenderPass(context)` -> `NativeColorLayout` (8 слотов, `MetalFormatFromRenderTarget`) + сравнение `same_pass` | 4110, 4077, 7878 | форматы/число RT (стабильно), адреса/размеры (динамика) | формат в плане, адрес/размер патчить |
| цикл `target_export_mapping` + `TextureGetRenderTargetFormat` | 7886-7893 | форматы RT | в плане |
| `pipeline_cache.GetGraphicsPrograms` -> `PrepareProgram` VS/PS, `BuildStageStaticKey` x2, `unordered_map::find` с вектором `static_state`, `MaterializeDiagnosticWindow` (SRT), перебор `permutations` со сравнением `specialization` | 7895; pipelineCache.cpp:1436, 1010; shader.cpp:699,837,597,646 | регистры шейдера, гостевые таблицы вертекс-фетча (читаются на каждый draw), user data | SRT-walker per-draw; остальное стабильно, но выигрыш мал (36/16 сэмплов) |
| `PrepareGraphicsImages` x2 стадии | 7903, 7660 | дескрипторы T#, состояние писателей GPU | per-draw, первое разрешение |
| `MetalShaderManager::GetFunction(vs/ps)` | 7907-7908 | хэши программ | в плане |
| сборка `MetalRenderPipelineKey`: `ConfigureNativeColorTargets` (второй `NativeColorLayout`), форматы depth/stencil, `MaskUnwrittenColorAttachments`, `FragmentColorExportMask(pixel_info)`, `GetOrCreateRenderPipeline` (хэш-поиск уже есть) | 7927-7960; metalPipeline.mm:432 | vs/ps, prim, форматы RT, blend, depth | в плане |
| depth/stencil: `NoteStencilUse`, `GuestPassHasStencil`, статический вектор `KYTY_DEPTH_ALWAYS_VS`, `ApplyDepthNeverToggle`, `GetOrCreateDepthStencilState` | 7977-8007; metalPipeline.mm:319 | регистры depth/stencil + vs_hash | в плане |
| rasterizer: `TranslateRasterizerState`, `ApplyGuestDepthBiasAndBlendColor`, 6 вызовов энкодера | 8010-8015; metalDraw.mm:227 | регистры mode/clip/poly offset; константы bias/blend color - динамика | трансляция в плане, константы патчить |
| viewport/scissor | 8017-8031; metalDraw.mm:252,282 | регистры viewport (меняются на пасс, не на draw) | сравнить с прошлым значением в энкодере, не звать Metal, если равно |
| индекс-буфер: снимок <=16 KiB (`AllocateDynamic`+memcpy), `GetBufferForGuestRange` | 8032-8070 | адрес/счётчик - per-draw | динамика |
| `snprintf(fault_label[512], ...)` ~25 аргументов, **всегда** | 8111 (DrawAuto 8260/8463, dispatch 9059) | per-draw | убрать/сделать ленивым |
| `BindMetalShaderResources` | 8135; :6337 | см. 4.2 | см. 4.2 |
| фактический вызов `DrawIndexed`, `ProbeBodyIndices`, `TaaBypassAfterDraw` | 8138-8190 | per-draw | оставить |
| счётчики: `std::mutex` + `unordered_map<uint64,uint64> s_exec_counts[addr]`, `RecordDrawForRT` | 8190-8205 (DrawAuto 8580) | per-draw | убрать (счётчик в массив/atomic) |

### 4.2 Инвентарь `BindMetalShaderResources` (metalRenderer.mm:6337-7660)

| блок | строки | стабильно | per-draw |
|---|---|---|---|
| очистка 4 векторов трассировки, `PassTimerEnabled`, `bind_trace` | 6339-6366 | - | дёшево |
| `fill_push_data`: соответствие user_data_registers -> dword push data | 6372-6411 | **пары (dst, индекс user_data)** из `prog.bindings` | значения dwords; `memory_offset` через `GuestGpuAllocation` |
| `vs_sizes[96]`/`ps_sizes[96]`: инициализация 65536/3 и циклы по `info.buffers` | 6469-6518 | `max_byte_extent`, `written` | `VsharpBytes(V#)` |
| `vs_args[256]`/`ps_args[256]` обнуление (4 KiB), `default_tex/sampler/buf` + 3 вызова ObjC (`gpuAddress`, `gpuResourceID` x2) на каждый draw | 6630-6670 | константы | - (кэшировать идентификаторы в static) |
| `BindMslStage`: проверка BDA по `bindings.descriptors` | 6676-6684 | **флаг `has_bda`** | `BindGuestAddressSpace()` с `useResources` на каждый вызов (metalAddressSpace.mm:36-62), без дедупликации по энкодеру |
| `BindGuestBuffer` на каждый V#: `GetBufferForGuestRange`, `SnapshotBytes`, `UseResource`, `args[i]=gpu_addr` | 6689-6830 | таблица слотов: `max_byte_extent`, `written` -> политика снимка | адреса, размеры, копия констант |
| SRT: `AllocateDynamic(bytes+1024)`, memset 1 KiB, memcpy | ~6960 | - | содержимое |
| образы: цикл по `snap.images` (до 64): T# -> `GraphicsImageFor` (второй раз после `PrepareGraphicsImages`), `storage_image` (цикл по `descriptors` на каждый образ!), `ApplyDstSel`, `RegisterComputeWrittenImage` | 6930-7075 | **слот привязки, `storage_image`, `images[i]` из `info`** | дескрипторы T#, состояние писателей |
| заполнение незаполненных слотов (64 итерации) | 7078-7100 | - | - |
| 32 сэмплера на стадию: `StageSampler` (копия S#, `GetSampler`), `[sampler gpuResourceID]`, `StageSamplerCompare`, **`push_back` в `s_*_caps` (структура с `id<>` под ARC - retain/release) только для флага `KYTY_ARGENC_GFX`, который выключен** | 7103-7110 | `samplers[i].snapshot_index`, `force_point_filtering` | 4 dword S# (почти всегда те же) |
| заполнение 64 слотов буферов default, `vs_sizes[59..63]` | 7426-7452 | - | `s_rect_idx` |
| `SetVertexBuffer(default,26)`, `SetVertexBytes(sizes 384 B)`, `SetFragmentBytes(...)`, затем `SetVertexBytes(vs_args 2 KiB)`, `SetFragmentBytes(ps_args 2 KiB)`, push data x2 | 7449-7452, 7569-7586, 6466-6467 | - | всё динамика (6 setBytes, ~5 KiB на draw) |

Следствие: в Bind много циклов, результат которых зависит только от `CompiledShaderInfo` (раскладка), а не от draw. Именно это и есть кандидат №1.

### 4.3 Предлагаемая структура: три таблицы

```
StagePlan   key = program->shader_hash (+ stage)             immutable, per permutation
PassState   key = fingerprint of pass-level registers        immutable
Pipeline    key = {PassState id, vs_hash, ps_hash, prim}     immutable
```

Ключ `StagePlan` - именно `shader_hash` (ключ Metal-функции, различает permutation), а не указатель на `CompiledShaderInfo`: `permutations.reserve(8)` (pipelineCache.cpp:733) не гарантирует стабильности указателей при >8 permutation, а `entry->second.resources` (ResourceSnapshot) один на запись источника и **перезаписывается каждым draw** (pipelineCache.cpp:1043-1047), поэтому ни план, ни кэш не должны хранить указатели на него.

**Содержимое `StagePlan`** (строится при первом draw с этим хэшем из `*stage.program`; всё из 4.2 колонки «стабильно»):
- `push_pairs[]`: (dst_dword, user_data_index) + `memory_offset_count/dword`;
- `buffers[b]`: `max_byte_extent`, `written`, `descriptor_format` для b < min(64, info.buffers.size());
- `images[i]`: `storage_image`, `native_slot = NativeBinding(stage,kind)+pos`, `info.images[i]` (dimension/флаги слойности для `DepthWriterForLayeredRead`/`LayeredImageFor`);
- `samplers[i]`: `snapshot_index`, `force_point_filtering`; дополнительно кэш `last_s_dwords[4] -> {gpuResourceID, compare}` на слот (если 4 dword S# не изменились - не звать `StageSampler`/ObjC);
- флаги: `has_bda`, `native_msl`, `ShaderHasArgumentBuffer0`, `fragment_color_export_mask`;
- `id<MTLFunction>` (сейчас `GetFunction` на каждый draw, :7907-7908; для compute :8983).

**Содержимое `PassState`** (ключ = XXH3 по ~50 словам из `HW::Context`; поля перечислены явно):
- по слотам из `NativeColorTargetMask`: `info.format`, `channel_type`, `channel_order`, `blend_bypass`, `tile_mode`, `view` (mip, slice), число сэмплов; `GetRenderTargetMask()`, `m_cbShaderMask`, `GetColorControl()`, `GetBlendControl(0..7)`;
- depth: `z_info.format`, наличие stencil (`GuestPassHasStencil`), `GetDepthControl()`, `GetStencilControl()`, `GetStencilMask()`, `GetRenderControl()` (depth/stencil clear), `GetModeControl()`, `GetClipControl()`, `GetPolyOffset()`, `GetAaConfig()`, `GetShaderStages()`;
- **не входят** (патчатся или сравниваются): `base.addr` целей, `attrib2.width/height`, viewport/scissor, `GetBlendColor`, значения depth bias.
- содержимое: `NativeColorLayout` без адреса/размеров (форматы, уровни), `target_export_mapping[8]`, `MetalRasterizerState` (cull/winding/fill/depth_clip/bias_enable), `id<MTLDepthStencilState>` + `stencil_refs` + `draw_has_stencil`.

**Содержимое `Pipeline`:** `MetalRenderPipelineKey` целиком (+ `color_targets[]` с blend), `id<MTLRenderPipelineState>`, `vs_func`, `ps_func`, `fragment_color_export_mask`, флаг `dropped` (`vs_func==nil` / `pstate==nil`) с причиной для `MslNoteSkippedDraw`/`MslDrawStatDropped`.

Ключ `Pipeline` включает `vs_hash`/`ps_hash` (известны только после `GetGraphicsPrograms`), поэтому поиск разбит на два шага: `PassState` ищется **до** `EnsureActiveRenderPass` (:7878), `Pipeline` - **после** :7902. Для `DrawAuto` те же места: ~8266 и ~8294.

Быстрый путь: в `MetalDrawExecutor` хранить `last_pipeline_id` + `encoder_serial` (`MetalContext::EncoderSerial()` уже используется в `UseResource`, metalDraw.mm:386). Если оба совпали с текущими - не звать `setRenderPipelineState/setDepthStencilState/setCullMode/...` (68% draw имеют тот же `vs+ps+RT` что предыдущий; Metal-вызовы - 45 сэмплов r12 / 33 r11 в draw-группе).

### 4.4 Правила инвалидации

| событие | реакция |
|---|---|
| смена шейдера (vs/ps/cs) | другой `shader_hash` -> другой `StagePlan`/`Pipeline` (автоматически) |
| смена формата/маски RT, blend, depth/stencil, raster регистров | другой отпечаток -> другой `PassState` (автоматически) |
| смена адреса/размера цели, динамическое разрешение | **не в ключе**; `EnsureActiveRenderPass` сравнивает адрес/размер с `s_pass_colors` как сейчас (:4150-4165); экспериментально DRS даёт 99.7-100% попаданий (разд. 3.2) |
| смена гостевой памяти дескрипторов (V#/T#/S#/SRT, таблицы фетча вертексов) | не кэшируется в плане: SRT-walker и `PrepareProgram` выполняются каждый draw как сейчас; в `StagePlan` S#-кэш сравнивает 4 dword |
| пересоздание шейдера/Metal-функции под тем же хэшем, `MetalPipelineManager` очистил кэш | счётчик поколения в `MetalShaderManager`/`MetalPipelineManager`; план хранит его и пересобирается при несовпадении |
| `vs_func == nil` или `pstate == nil` (асинхронная компиляция) | негативный результат **не** кэшировать дольше текущего draw (пересобирать план пока не появится функция) |
| tessellation/mesh (`logical_stage != Vertex`), `pending_index.multi`, `PassTraceActive()`, любой из диагностических `KYTY_*` флагов в `GetOrCreateRenderPipeline` (`KYTY_DEBUG_PS`, `KYTY_DEBUG_QUAD`), `s_diag_debug_ps`, `KYTY_DEPTH_ALWAYS_VS` | обход плана (идти старым путём) |
| переменная окружения `KYTY_DRAW_PLAN=0` | полный обход (аварийный выключатель); `=verify` - режим сравнения (разд. 5) |
| размер | <= 8192 записей в каждой таблице, вытеснение при переполнении; фактически нужно ~200-400 `Pipeline`, ~60-100 `PassState`, ~100-250 `StagePlan` |

Эпохи вместо отпечатка (этап 2b): `CpOpSetContextReg` (pm4Handlers.cpp:2408) и `CpOpSetShaderReg` (:2454) уже диспетчеризуют по `cmd_offset` через таблицы `g_hw_ctx_func`/`g_hw_sh_func`; там можно бампить `ctx_epoch` только при реальном изменении значения регистра из группы «pass-state». Тогда проверка на draw = сравнение одного числа. Начинать с отпечатка: он локален в `metalRenderer.mm`, проверяем (разд. 5) и не требует касаться PM4.

### 4.5 Чего план не должен делать

- Не кэшировать `flattened_srt`, дескрипторы, адреса буферов, содержимое констант: адреса наборов буферов повторяются лишь у 1.8-23% draw (разд. 3.2). План хранит раскладку (какие слоты, какие размеры, политику снимка), адреса патчатся.
- Не кэшировать результат `GraphicsImageFor` в плане: он зависит от динамического состояния (`LastGpuWriterTexture`, `NewestRenderTargetFor`, `AliasConvert`, `GetComputeWrittenImageForAddress`, `now_flip <= wr_flip+2`, :1474-1575). Для него отдельное мемо с эпохой (этап 3).
- Не использовать позицию draw в кадре как ключ (разд. 3.2: 14-66% совпадений).

---------------------------------------------------------------------------------------------------

## 5. Задача 4: как проверить без игры, и поэтапный план

### 5.1 Проверка

1. **Режим сравнения `KYTY_DRAW_PLAN=verify`**: всегда считать старым путём (как сейчас) **и** брать из кэша; сравнивать по значению (`MetalRenderPipelineKey::operator==`, указатели `pstate`/`depth_stencil`/`vs_func`/`ps_func`, `stencil_refs`, `MetalRasterizerState`, раскладка целей, `target_export_mapping`, push pairs, таблицы слотов). Используется результат старого пути, поэтому картинка не может измениться; при расхождении - печать ключа и обоих значений и отключение плана (как уже сделано для `rejected` в `MaterializeCompiledSrtExperiment`). Реализация естественная: «строитель плана» = вынесенный старый код, «кэш» = его мемо; сравнение = `operator==` строителя с кэшем. Счётчики `[DrawPlan] hit/miss/verify_fail` раз в 2 с.
2. **Тест покрытия отпечатка (без игры, без Metal)**: мини-программа в `tools/test/` (по образцу `tools/bench/`), включающая только `hardwareContext.h`. Список полей отпечатка и список явных исключений (адреса, размеры, viewport, blend color, bias). Тест фаззит каждый байт `HW::Context`: байт из списка отпечатка должен менять хэш; байт не из списка и не в списке исключений - сигнал «поле забыли». Так забытые регистры ловятся механически, а не в игре.
3. **Мини-тесты `StagePlan`**: по `CompiledShaderInfo` синтетического шейдера строится план и сравнивается с результатом старых циклов (push pairs, `storage_image`, native slot) - это чистые функции, не требуют GPU.
4. **Оффлайн-симуляция попаданий по PassTrace** (уже сделана здесь, `pt_cache2.py`): повторять после изменения ключа, ожидание >= 99.5% между кадрами и >= 88% внутри кадра.
5. **A/B в одном прогоне** (в журнале: сравнение FPS разных прогонов недопустимо): пока `verify` и есть режим `=0` по переменной, плюс скриншоты `mt_shots.sh` с планом/без плана на одной и той же сцене; тяжёлую проверку делает агент, запускающий эмулятор.
6. **Профиль без HUD** до и после каждого этапа (`MTL_HUD_ENABLED` убрать) - чтобы не мерить артефакт.

### 5.2 Этапы (оценки по сэмплам; кадр = 2100 draw + 250 dispatch = 2350 операций)

| этап | что | где | выигрыш r12 / r11, мс/кадр | мкс на операцию | риск |
|---|---|---|---|---|---|
| **0** | ленивая метка `fault_label` (лямбда или структура с форматированием при ошибке) -22/-15 сэмплов; `BindGuestAddressSpace`: пропуск `useResources`, если `EncoderSerial` тот же, -25/-4; в `MaterializeCompiledSrtExperiment` не звать `ProfileClockNs()` x3 и не вести `unordered_map` статистики, пока не включён профиль, -20/-20; счётчики `s_exec_counts` (mutex+map, :8190, :8580) и `RecordDrawForRT` -> массив/atomic, -10/-8; второй `GraphicsImageFor` в Bind -> взять результат `PrepareGraphicsImages` из массива [стадия][i], -40/-14; `ArgCapture`/`s_*_caps` и `gpuResourceID` default-объектов (static), -30/-15 | metalRenderer.mm:8111,8190,6312,6630,7903; metalAddressSpace.mm:36; compiledSrtExperiment.h:100-150 | **-2.1 / -1.4** | 0.9 / 0.6 | низкий, побитово тот же результат |
| **1** | `StagePlan` по `shader_hash`: push pairs, таблицы буферов/образов/сэмплеров, флаги, `MTLFunction`; кэш S# -> sampler | metalRenderer.mm:6372-6520, 6676-7110, 7907 | -0.85 / -0.65 (-60/-35 сэмплов из категории «Bind: заполнение» 124/69) | 0.36 / 0.28 | низкий (план неизменяем) |
| **2** | `PassState`+`Pipeline` (отпечаток) + пропуск повторных Metal-сеттеров | metalRenderer.mm:7878-8031 и 8266-8420; `NativeColorLayout`:4077 | -1.25 / -1.0 нетто (брутто -100/-65 сэмплов минус ~0.17 мс/кадр на поиск) | 0.53 / 0.43 | средний: полнота отпечатка, проверяется verify и тестом покрытия |
| 2b | эпохи вместо отпечатка | pm4Handlers.cpp:2408,2454 | -0.15 | 0.06 | средний |
| **3** | мемо разрешения ресурсов с эпохой: образы `(T# dwords, w, h, addr, tag)` -> `id<MTLTexture>` с инвалидацией по регистрации писателя (`RegisterComputeWrittenImage`, конец пасса, смена поколения гостевой памяти, смена flip для правил «<= wr_flip+2»); кэш трансляции VA->чанк по `GetGuestBackingGeneration` (`TryGetContiguousBackingOffset` 19/7 self, `GetBackingChunk`); `GuestTexSourceHash` без `std::vector` (буфер на стеке), 1 раз на flip | metalRenderer.mm:1474, 5046; metalBuffer.mm:261 | -2.3 / -1.9 (-160/-100 сэмплов) | 0.98 / 0.8 | **высокий**: правила 1474-1575 тонкие (писатели GPU, DRS, алиасы), нужна сверка `verify` |
| **4** | compute: один энкодер на серию dispatch (36 серий/кадр вместо 250 энкодеров) вместо энкодера+fence на каждый; уже есть `KYTY_CS_OVERLAP` (metalOrder.h, по умолчанию выкл.) | metalDraw.mm:562-672, metalRenderer.mm:9130 | -1.5 / -2.2 (begin/end/cmdbuf+HUD: 131/146 сэмплов, оставить ~15%) | 0.64 / 0.94 | средний: порядок записей/чтений, GPU-время |
| **5** | микро-оптимизация SRT-walker (`EvaluateNode` self 81, `RefreshFlatBuffer` self 24); пропуск walker по неизменному `user_data`+поколению страниц SRT - малополезен (адреса меняются) | compiledSrtExperiment.h, ResourceMaterialization | -0.35 / -0.37 | 0.15 | низкий |

Итого этапы 0-3: **-6.4 мс/кадр (r12) / -4.9 (r11) = 22-28% потока** (22.7 -> ~16.3 мс; в среднем 9.7 -> ~7.0 мкс на операцию). С этапами 4-5: -8.3 / -7.5 мс (33-37%), ~6.1 мкс на операцию. Отдельно: выключение HUD убирает ещё 13.5-16.6% `ProcessPm4` (3.1-3.7 мс/кадр), частично пересекаясь с этапом 4 (`BeginComputePass`, `BeginRenderPass`, `[buffer contents]`). **Цель «11 мкс -> 2-3 мкс» одним draw-plan кэшем недостижима:** 70% draw-пути - ресурсы гостя и пассы. Дальше нужна структурная мера: запись команд пасса в списки и кодирование во втором потоке (`MTLParallelRenderCommandEncoder`/отдельный encode-поток); план из этого документа ей не мешает и её подготавливает (отделяет декодирование PM4 от Metal-вызовов).

### 5.3 Рекомендуемый порядок и «первый этап»

1. Этап 0 + этап 1 одним патчем (побитово тот же результат, выигрыш -2.0..-3.0 мс/кадр суммарно, ~9-13% потока).
2. Снять профиль без HUD (агент, запускающий эмулятор), сравнить категориями из разд. 2.3.
3. Этап 2 в режиме `verify` минимум на одном матче и меню, затем включить по умолчанию.
4. Этап 3 только для картинок и чанков, по частям (каждую часть под `verify`).
5. Этап 4 (серии dispatch) - отдельная ветка, GPU-метрики на сцене с FPS ниже лимита (в журнале: на титульной сцене 30 FPS это не видно).

---------------------------------------------------------------------------------------------------

## 6. Открытые вопросы и ограничения

- Число кадров в окне `sample` взято из допущения 30 FPS; если сцена шла медленнее, мкс на операцию в разд. 0 и 5.2 увеличатся пропорционально, а доли (%) не изменятся.
- Состав кадра (2100/250) из PassTrace, а не из r11/r12: сэмплы шли на другой сцене/моменте; проверка - повторить `sample` вместе с PassTrace-прогоном.
- Не измерено: сколько draw идёт по tessellation/mesh веткам (их обходим); сколько из 126 пассов/кадр реально тратят на `endEncoding` (69 сэмплов в сводке задачи) и можно ли склеить соседние пассы с одинаковой раскладкой.
- Не изучено в коде: `DrawIndirectMulti` и `RectListIndirect` (63 сэмпла r12): они идут через `DrawIndex` с `pending_index.multi`, в плане обходятся.
- `GuestTexSourceHash` (хэш 64 KiB на текстуру раз в flip, `std::vector` на вызов): отдельная тема для этапа 3.
- В метках исходников номера строк даны по рабочему дереву на 2026-11-08 (много несохранённых изменений от других агентов: строки могут сместиться).
