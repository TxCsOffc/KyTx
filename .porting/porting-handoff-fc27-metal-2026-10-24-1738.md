# FC27 native Metal handover — 2026-10-24, 17:38 Asia/Tashkent

## 1. Контекст и ограничения
- **Стек:** Native ARM64 FEX + direct RDNA → MSL (Metal backend на Apple Silicon).
- **Запреты:** Никакого SPIR-V в runtime, никакого Vulkan-рендерера, никакой Rosetta.
- **Git-политика:** Не коммитить в git без прямого указания пользователя. Все изменения в рабочей директории сохранены.
- **Репозиторий:** `/Users/tx/Developer/KyTx`, ветка `metal-native-checkpoint`.
- **Игра:** `/Users/tx/Developer/PPSA34015` (EA SPORTS FC 27).

---

## 2. Устранённые проблемы (GPU Hang / TDR на флипах >1000)

### Симптомы
- Прогоны эмулятора стабильно зависали с ошибкой `kIOGPUCommandBufferCallbackErrorHang` (GPU Hang Error 00000003) на флипах 1025–1325.
- В логе Metal диагностировался аварийный командный энкодер:
  `[FaultInfo] encoder=render state=4 signposts=(null)`

### Локализация и инструментация signposts
1. В `src/graphics/host_gpu/metal/metalDraw.mm` вызов `FaultSignpost(encoder)` отсутствовал в `DrawIndexed`, `DrawIndexedIndirect`, `DispatchIndirect`.
2. В `src/graphics/host_gpu/metal/metalRenderer.mm` в `DrawIndex` и `DispatchIndirect` не вызывался `KytyFaultOpSet`.
3. После добавления полных signposts первый же тестовый прогон с `KYTY_MTL_FAULT_INFO=1` выдал точный стек операций упавшего энкодера:
   ```text
   [FaultInfo] encoder=render state=4 signposts=
     draw_idx #181436 rt0=0x824a080000 vs=0x7ac82a648d182516 ps=0x8ade76768797ce89 prim=4 idx=8196 |
     draw_idx #181437 rt0=0x824a080000 vs=0x039f7076f9f2f1ff ps=0x8ade76768797ce89 prim=4 idx=291 |
     draw_idx #181438 rt0=0x824a080000 vs=0xea2c80fbf42e2890 ps=0x86168ce712fed4ab prim=4 idx=1152
   ```

### Первопричины зависания
1. **Undefined Behavior в `ReadFirstLane` при пустом ballot (`MslEmitter.cpp`):**
   - Пиксельные шейдеры `0x86168ce712fed4ab` и `0x8ade76768797ce89` содержат RDNA waterfall-циклы с cross-lane инструкцией `ReadFirstLane`.
   - В MSL генерировался код:
     ```metal
     simd_shuffle(src, (ushort)min(ctz(kyty_ballot(cond).x), 31u))
     ```
   - Если условие `cond` ложно для всех 32 потоков в simdgroup, `kyty_ballot(cond).x == 0u`.
   - В стандарте Metal Shading Language `ctz(0)` не определен (**Undefined Behavior**). Clang для ARM64 оптимизирует код с предположением ненулевого аргумента, что ломало логику выхода из waterfall-цикла и приводило к чтению неактивных lane через `simd_shuffle`.
2. **Завышенный лимит цикла (`1048576u`):**
   - В `MslEmitter.cpp` лимит циклов был жестко зафиксирован на $1\,048\,576$ итерациях.
   - Для тяжелых циклов (~2000 инструкций) на разрешении 1600×904 это приводило к миллиардам инструкций на пиксель и превышению 2-секундного таймера watchdog macOS (`kIOGPUCommandBufferCallbackErrorHang`).

### Исправление
- В `src/graphics/shader/recompiler/backend/msl/MslEmitter.cpp`:
  1. Добавлен безопасный хелпер `kyty_first_lane`:
     ```metal
     inline ushort kyty_first_lane(uint4 ballot) {
         return ballot.x != 0u ? (ushort)ctz(ballot.x) : (ballot.y != 0u ? (ushort)min(ctz(ballot.y) + 32u, 31u) : 0u);
     }
     ```
  2. В `EmitWave` для `ValueOpcode::ReadFirstLane` заменен вызов `ctz(kyty_ballot(...).x)` на `kyty_first_lane(...)`.
  3. Реализована функция `MslLoopCap()` с чтением переменной окружения `KYTY_LOOP_CAP` и безопасным лимитом по умолчанию в $4\,096$ итераций вместо $1\,048\,576$.

---

## 3. Верификация прохождения флипов 1000–1475+
- Все тесты набора `tools/test_metal_msl.sh` и `tools/test_metal_pipeline.sh` пройдены с 0 ошибок.
- Прогон FC 27 `mrt-ab-1-172536` успешно преодолел барьер и достиг **флипа 1475** (`reason: reached_1400`, `exit_code: 0`, 0 GPU errors).
- Зафиксировано выполнение 250+ тыс. draw calls и стабильная работа Native MRT (`KYTY_MSL_MRT=1`).

---

## 4. Локализация источника красного экрана (Heavy Red Scene)

### Интервальный дамп кадров (прогон `mrt-ab-1-173137`)
Сдамплены кадры 700, 750, 800, 850, 900, 950, 1000:
- **Кадр 700, 750, 800, 850:** `mean RGBA: [24, 102, 112]`. Заставка игры и верхний логотип EA SPORTS FC 27 ([fc27-flip-800.png](fc27-flip-800.png)).
- **Кадр 900:** `mean RGBA: [25, 56, 61]`, 14 764 белых пикселей текста. Полноценное окно диалога подключения к EA Servers ([fc27-flip-900.png](fc27-flip-900.png)).
- **Кадр 950:** `mean RGBA: [217, 16, 17]`. Появление сплошного красного оттенка ([fc27-flip-950.png](fc27-flip-950.png)).
- **Кадр 1000, 1300:** Полное накопление красного цвета `[254, 7, 7]`.

### Анализ причины красного цвета
- Вершинный шейдер `0xb2c4c2e311c5bc76` (вариант `0xbc0210100fe76732`) читает цвет вершин `out.param2` из `ab.buffers[2]` со stride 32 байта:
  Байты в памяти: `255, 0, 0, 153` (`0x990000ff` little-endian), что соответствует `RGBA = (1.0, 0.0, 0.0, 0.6)`.
- Пиксельный шейдер `0x2ae1ba74c42662f1` (вариант `0xae49c6a306380213`) выводит этот цвет в полноэкранный overlay `Texture449` (RGBA8Unorm) с альфа-блендингом.
- Из-за `loadAction = MTLLoadActionLoad` и отсутствия очистки текстуры overlay между кадрами слой многократно накапливает красный цвет с коэффициентом 0.6, быстро доводя весь оверлей до насыщенного красного цвета `(255, 0, 0)`.

---

## 5. Измененные файлы в working tree
1. `src/graphics/host_gpu/metal/metalDraw.mm`:
   - Расставлены вызовы `FaultSignpost` в `DrawIndexedIndirect`, `DrawIndexed`, `DispatchIndirect`.
   - Защита от пустой строки в `FaultSignpost` (подстановка `"unlabelled_op"`).
2. `src/graphics/host_gpu/metal/metalRenderer.mm`:
   - Добавлен вызов `KytyFaultOpSet` в `DrawIndex` с подробным описанием draw call (PSO, hash, blend, format).
   - Добавлен вызов `KytyFaultOpSet` в `DispatchIndirect`.
3. `src/graphics/shader/recompiler/backend/msl/MslEmitter.cpp`:
   - Добавлен хелпер `kyty_first_lane` для устранения `ctz(0)` UB.
   - Добавлена функция `MslLoopCap()` с поддержкой `KYTY_LOOP_CAP` (дефолт 4096).
4. `.porting/artifacts/2026-10-24/mrt-compare-NOT-RUN.py`:
   - Скрипт валидации обновлен для проведения диагностических прогонов с дампом кадров.

---

## 6. Следующие шаги
1. **Проверка буфера вершин `b2c4c2e311c5bc76`:**
   - Выяснить, почему буфер 2 содержит `0x990000ff`: заполняет ли его гостевой CPU ring buffer, либо он генерируется compute-шейдером (например `0x2e98f3995fca8f8f`).
   - Проверить механизм `SnapshotBytes` для буферов размером >16KB (если гостевой CPU перетирает буфер в кольцевом аллокаторе).
2. **Очистка оверлея `Texture449`:**
   - Проверить условия выставления `MTLLoadActionClear` вместо `MTLLoadActionLoad` для оверлей-текстур при начале нового кадра.
