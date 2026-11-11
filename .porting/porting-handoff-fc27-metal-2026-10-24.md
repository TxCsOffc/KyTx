# Handover FC27 — 2026-10-24: логотип загрузки, Xcode и оставшиеся дефекты

## Сначала прочитать

Пользователь попросил передать работу и **особенно сохранить, как был найден, исправлен и проверен дефект логотипа загрузки**. Ниже эта цепочка разобрана отдельно. Пользователь сообщил, что логотип после исправлений отображается. Это не означает, что целиком исправлены интро, остальные значки или тяжёлая сцена.

Цель всё ещё не достигнута: корректное интро/UI/эффекты и играбельный матч с измеренными 30+ FPS. Только **native ARM64 FEX + direct RDNA→MSL**. Rosetta запрещена. Vulkan разрешался только как визуальный референс; в этом продолжении он не запускался.

Репозиторий `/Users/tx/Developer/KyTx`, ветка `metal-native-checkpoint`, HEAD `c30d5d81ab23`. Большой dirty/untracked tree содержит работу нескольких продолжений — сохранить полностью. Коммитов не было. Этот handover не объявляет milestone завершённым.

Предыдущий подробный контекст: [follow-up 23:50](porting-followup-fc27-metal-2026-10-23-2350.md), затем [handover 15:12](porting-handoff-fc27-metal-2026-10-23-1512.md). Их сведения о работающих процессах и «следующей ошибке» устарели.

## Важный фикс UI/логотипа: packed flags должны передаваться flat

### Как искали через Xcode

Использовали валидный захват **одного present**:

`/Users/tx/Developer/fex-spike/runs/captures/fc27-20261023/cache-intro-one.gputrace` (~4.9 GiB).

Он успешно воспроизведён в Xcode через computer use. Несмотря на имя `intro`, в нём **экран подключения**, а не ролик EA Sports. Нельзя подписывать его как прямую трассу пикселей самого логотипа или доказательство исправленного ролика.

1. В финальном композите **CB6 / Compute121 / API7768**, CS `fb2cbb810e6fd25d`, выход Texture30 RGB10A2 уже получает дефект из UI-слоя **Texture4 RGBA8**. Другой полноэкранный вход — Texture10250.
2. Сравнили UI-вложение до/после отдельных draw. После **CB5 / Render175 / API7657** нижнего белого прямоугольника ещё нет. **CB6 / Render176 / API7722**, `drawIndexedPrimitives` с 18 индексами, впервые его добавляет. Это сузило поиск до одного draw, а не финальной гаммы/презентера.
3. Bound Resources: VS `main0 (80)`, PS `main0 (79)`, PSO31; три атласа **8192×256 BC3_RGBA_sRGB**, Textures77/78/79; Texture76 — 1×1 RGBA8 stand-in. В начале PS image0/1/2 указывают на Texture76, image3/4/5 — на атласы.
4. В attachment viewer задали **LoupeX=960, LoupeY=1030**, нажали **Debug Pixel**. Проверили входы и значения непосредственно перед экспортом, выбрав исполнившуюся инструкцию в Shader Debugger.
5. Входы: `param0 ≈ (1,1,1,.972)`, `param2 ≈ (1.929,.584,0,0)`, `param3` показан нулевым. Перед `out.color0.x` переменная **`v2125=(1002912711,15303,0,0)`** содержит half-пары `0x3bc73bc7 / 0x00003bc7`: **RGB=.97216796875, alpha=0**. PSO: **One / OneMinusSourceAlpha**. Поэтому этот draw может добавить белый RGB даже при нулевой alpha.
6. Сопоставили исполняемый PS с эмиттером: гостевой hash **`39ad955b656767cc`**, старый native variant **`a6d3f97a759c923f`**. В начале PS флаги читаются через `as_type<uint>(in.param3.x/z)`; от них зависят ветвление и alpha. В MSL поле `param3` было обычным интерполируемым float4.

Сохранённый PS до правки: [white-quad-before-a6d3f97a759c923f.metal](artifacts/2026-10-23/white-quad-before-a6d3f97a759c923f.metal). Не считать отсутствие строк в фильтре `sample` самостоятельным доказательством отсутствия texture sampling — нужно раскрыть дерево исполнения.

### Причина и изменение кода

`BuildInterface` в `src/graphics/shader/recompiler/backend/msl/MslEmitter.cpp` учитывал location входа PS, но **игнорировал `StageInput.force_flat`, SPI flat и no-perspective**. Упакованные uint-флаги идут через float varying без арифметического преобразования. Обычная интерполяция меняет их биты; субнормальное float-представление packed flags может стать нулём. После этого PS выбирает неверную ветку.

Теперь:

- `input.force_flat || ShaderPixelParameterIsFlat(...)` → `[[user(locnN), flat]]`;
- `ps_no_perspective` для не-flat входов → `center_no_perspective`;
- обычные perspective-параметры сохраняют свой режим.

У реального PS **`39ad955b656767cc`** новый native key **`a4d8ebd8084f007c`**; в нём:

```metal
float4 param3 [[user(locn3), flat]];
```

Исходник после правки: [ui-after-a4d8ebd8084f007c.metal](artifacts/2026-10-24/ui-after-a4d8ebd8084f007c.metal).

Не откатывать этот фикс, пытаясь исправить красную сцену. Перенос флагов — отдельно доказанный дефект. Полная barycentric/per-vertex семантика этим изменением не реализована.

### Как проверяли

В `tests/metalMslTests.mm`, **`PixelInterpolation()`**, добавлен реальный GPU render pass: native emitter строит PS из IR; вершины имеют разные W и разные значения. Считывается центральный пиксель RGBA32Uint. Шесть случаев проверяют perspective, no-perspective, forced flat, SPI flat и побитовую сохранность packed flag **`0x00010003`**.

- До фикса: **6 GPU cases / 5 failures**. Flat получал интерполированное `1.65455`; packed flags становились `0x00000000`.
- После фикса: **6 / 0**, packed flags сохраняются побитово.
- Включены **MTL_DEBUG_LAYER и MTL_SHADER_VALIDATION**; полный native MSL suite также проходит. Основной ARM64 эмулятор пересобран.
- Артефакты: [interpolation-before.log](artifacts/2026-10-23/interpolation-before.log), [interpolation-after.log](artifacts/2026-10-23/interpolation-after.log), [build-interpolation.log](artifacts/2026-10-23/build-interpolation.log).

**Граница доказательства:** пользователь подтвердил появление логотипа. GPU-тест независимо доказывает исправление механизма передачи параметров. Захват до фикса точно локализует связанный UI-дефект; matched до/после pixel trace самого логотипа не снимался. Не превращать это в утверждение «исправлены все значки/интро»: нижний белый прямоугольник и красная сцена оставались в поздних запусках.

## Остальные сохранённые исправления перед этим handover

- **Native MSL cache specialization identity**: `NativeMslFunctionKey` учитывает исходный hash, stage, полный сгенерированный MSL и успех emission. `pipelineCache.cpp` использует ключ в обеих direct-MSL ветках. Один гостевой shader с float/uint/stride вариантами больше не получает чужую функцию. Реальный manager GPU-test: old2fail → fixed0. Нужен совместно с flat-фиксом, чтобы новая specialization не подменялась старой.
- **LDS append/consume не требуют включать GDS**: 6 реальных wave32 GPU случаев; GDS остаётся OFF, wave64 cross-half не решён.
- **Depth-only fragment/color state**: `MaskUnwrittenColorAttachments` отключает запись/смешивание в неэкспортируемые слоты, сохраняя форматы. Ранее PS `eb921951d9dd9403` / native `9de2fa0a26654c26` давал `Fragment shader does not write to render target color(0)...required for blending`. Depth-only и обычный color GPU-test проходят.
- Depth-only runtime **проверен**: `/Users/tx/Developer/fex-spike/runs/validation-20261023/depth-fixed-20261023-234827-16939.log` содержит новый PS, **0 pipeline creation errors**; игра достигла тяжёлой сцены. Красный экран остался. Сжатая копия: [depth-fixed-runtime.log.gz](artifacts/2026-10-24/depth-fixed-runtime.log.gz).

## Новый разбор красного слоя через Xcode

В старой валидной **`shared-fixed-one.gputrace` (~16 GiB)** повторно воспроизвели **CB19 / Render74 / API21316**, Triangle6. Открыли Geometry → Debug для vertex0, раскрыли `main0 → kyty_vs_impl`, отфильтровали и выбрали присваивание `v10 = ...`.

- `ab.buffers[2]` уже начинается с **2566914303 = `0x990000ff`**.
- Загрузка UNORM8 выдаёт `v10=(1065353216,0,0,1058642330)` → **RGBA=(1,0,0,.6)**.
- Все шесть post-VS вершин имеют этот цвет в param2. Это цвет входного буфера, не спонтанный красный результат пиксельной арифметики.
- PSO22: RGBA8Unorm, RGB **SourceAlpha / OneMinusSourceAlpha**, alpha **One / OneMinusSourceAlpha**, Add, write All. Не доказана ошибка blend именно этого draw.
- Исправлено старое имя hash в документации: VS guest **`b2c4c2e311c5bc76`**, PS guest **`2ae1ba74c42662f1`**. **`751942f534c2702e` — старый native/export variant PS, а не raw guest hash.** Текущие native keys: VS **`2b83a227ac38238f`**, PS **`c74589ce64857cd4`**. PS-body совпадает со старым, добавлен только class helper. Трасса снята до ring/cache/flat-фиксов — происхождение CPU-данных нужно проверять в новом запуске, не объявлять старую ownership-гипотезу доказанной.

Текущие VS/PS сохранены рядом с этим handover в `artifacts/2026-10-24/red-*-current-*.metal`.

## Новая правка MRT: GPU-тест проходит, runtime ещё не принят

Обнаружена отдельная реальная неполнота: PS сцены экспортируют до шести `[[color(N)]]`, но **EnsureActiveRenderPass, DrawIndex и DrawAuto подключали только RT0**. Добавлено подключение активных слотов0..7 с сохранением номеров/дырок, per-slot write masks/blend и проверкой смены всех attachment в pass key. Вспомогательные RT не заменяют presenter fallback.

Код: `metalPipeline.h/.mm` (`NativeColorTargetMask`, `ConfigureNativeColorTarget`) и `metalRenderer.mm` (`NativeColorLayout`, `ConfigureNativeColorTargets`, render pass). **Диагностический opt-out `KYTY_MSL_MRT=0`** возвращает RT0-only выбор слотов; по умолчанию новая поддержка включена.

`tests/metalPipelineTests.mm` теперь связывает реальные pipeline manager и `metalDraw.mm`: старый RT0-only draw даёт **19 неверных компонентов**, исправленный — **0 failures** для шести RT, sparse0/2/5 с per-slot масками/blend и slot2 без RT0. Depth-only 2 случая тоже проходят. API+shader validation включены. [mrt-pipeline-gpu.log](artifacts/2026-10-24/mrt-pipeline-gpu.log). Финальная сборка проходит: [build-mrt-final.log](artifacts/2026-10-24/build-mrt-final.log), `git diff --check` проходит.

Runtime `/Users/tx/Developer/fex-spike/runs/validation-20261024/mrt-fixed-20261024-000625-17561.log`: MRT slots=3 (RT0+RT1) реально привязываются; **0 PSO failures**, но красный экран остаётся. **После последнего DrawStats flip1075 начинается GPU Hang**, затем SubmissionsIgnored, всего10 сообщений об ошибке. Поэтому НЕ считать поддержку MRT проверенной в игре/стабильной. FaultInfo render state4 имеет `(null)` signposts; предшествующие compute encoders state1 — Completed, их нельзя объявлять виноватыми по одному соседству в логе. [mrt-fixed-runtime.log.gz](artifacts/2026-10-24/mrt-fixed-runtime.log.gz).

Подготовлен, но **НЕ ЗАПУСКАЛСЯ**, ограниченный A/B harness: [mrt-compare-NOT-RUN.py](artifacts/2026-10-24/mrt-compare-NOT-RUN.py). Аргумент0/1 задаёт MRT off/on при прочих одинаковых флагах; запуск до1400+5s или первой GPU-ошибки, не более110s, свой процесс останавливает. Сначала сравнить без capture. Причинная связь Hang с MRT или захватом не установлена. Не терять старые исправления при возможном откате только MRT-эксперимента.

## Захваты: что пригодно

| Трасса в `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261023/` | Статус |
| --- | --- |
| `cache-intro-one.gputrace` | Валидная, Xcode replay + pixel debug; UI-подключение, до flat/depth/MRT |
| `cache-intro500-one.gputrace` | Single-present, static/gpudebug browse; полный Xcode replay не подтверждён |
| `shared-fixed-one.gputrace` | Валидная, Xcode replay + vertex/pixel debug; старые данные до ring/ALU/cache/flat |
| `single-frame/lldb-present.gputrace` | Старый валидный захват; другие API/resource IDs, не смешивать |
| `flat-fixed-heavy-one.gputrace` | Захват провалился0/1, файла нет |
| `mrt-fixed-one.gputrace` | CLI завершился1/1, ~6.33GB, но Xcode Summary показывает **0 command buffers / 0 draws / No Preview**. Для pixel debug **НЕПРИГОДЕН**, не подтверждает исправление MRT. Причина неполного capture не установлена |
| `frame800.gputrace` | Старый повреждённый файл, не использовать |

Последний захват снимался с `--disable-unused-recording --disable-backtrace-recording`, shared-mem optimization disabled. Не повторять слепо эту комбинацию: файл открылся, но команд для анализа в нём нет. Надёжность проверять по ненулевому workload, replay и нужной сцене, а не только `capture finished`. Захватывать **строго по одному кадру**. Номера guest flip не гарантируют одинаковое содержимое между запусками.

## Состояние при передаче и продолжение

- Эмулятор17561 остановлен через Cmd+Q; `ps` подтверждает отсутствие `kyty_emulator`, `gpucapture`, `gpudebug`, `lldb`. Xcode GPU workload остановлен, открыт `mrt-fixed-one.gputrace` на Replay. Не тратить следующий запуск на анализ его пустого workload.
- Лаунчер `/Users/tx/Developer/fex-spike/runs/KyTxDiagnostic.app` / bundle `org.kytx.diagnostic`. `getApp` в CUA **запускает приложение**, поэтому не вызывать для «проверки остановки».
- Каждый запуск теперь имеет уникальный timestamp/PID log. Текущий указатель: `/Users/tx/Developer/fex-spike/runs/validation-20261024/latest-diagnostic-log.txt`. Базовые flags: MSL_ONLY1/backend metal/1080P/signed_in/lazy/wait_retry_elide/wait_by_order/UMA_READBACK, API validation/capture enabled; launcher также включает MSL dumps, MRT/descriptor/fault logging, PROBE_HASH VS2b83... и PPM500..1800. Это диагностический запуск, не FPS benchmark.
- В sandbox Metal device отсутствует. Реальные GPU-тесты/CLI-процессы требуют escalation; `bash tools/test_metal_pipeline.sh` уже разрешён. GPU capture/GUI выполнять через доступные инструменты и явные разрешения среды.
- CUA после compaction: `await cua.rewriteDocumentation()`. AX full snapshot меняет numeric IDs — искать нужный элемент в свежем snapshot и действовать в том же вызове. Filter trace list лучше focus+paste; `drawIndexedPrimitives` не пропустить regex только для `drawPrimitives`. Для Variables выбирать исполнившуюся инструкцию до выхода из scope.
- Сначала A/B стабильности MRT, затем новый ненулевой single-frame capture **до первого GPU error** и pixel/vertex debug следующего дефекта. Не объявлять красный вход «ошибкой финального композита» без трассировки продюсера/буфера.
- Остаются unsupported image derivatives для arrays/3D (PS guest69484ce5ea9fbf78/native631f646248122252), desc_format0 formatted store (CSe191ab754420b413/native030aa6d6844bc8c8), compute cube opt-in, GDS/wave64, ownership/aliasing и полная barycentric semantics. Полный heavy shader-validation run не установлен: instrumentation больших LDS превышает32KB.

Ledger CLI на этом Mac недоступен; локальные `.porting/porting-memory.md` и `ufc5/.ledger/STATE.md` обновлены. После запроса handover новые A/B game runs не запускались.
