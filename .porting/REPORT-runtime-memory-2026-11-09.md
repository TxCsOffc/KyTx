Продолжение реализации памяти, 9 ноября 2026. KyTx `metal-native-checkpoint`, HEAD `9534b337` с сохранёнными прежними dirty changes. Пользователь разрешил запуск игры после автономных проверок.

В рабочем `AliasConvert` устранена потеря гостевых байтов: раньше scratch обнулялся, затем получал только видимые тексели предыдущего GPU-писателя. Читатель с другим размером/форматом/layout получал нули в остальных областях. Теперь общая функция `EncodeTileAlias` копирует настоящие гостевые байты из buffer manager, накладывает тексели писателя в его AMD tile-layout и импортирует результат через layout читателя. Исходная текстура и гостевая память при конверсии не изменяются. Передачи выполняются GPU-командами с fences; CPU wait/submit на каждую конверсию не добавлены.

Конверсия переиспользуется только в пределах подготовки и привязки одного draw/dispatch. Старая версия кэша учитывала только serial писателя, хотя гостевые байты за пределами его footprint могли уже измениться. Новый command epoch заставляет следующий draw/dispatch прочитать актуальную базу. Блиты depth staging и BC-copy в этом пути также включены в общий порядок fences.

Обычные линейные текстуры переведены с отдельных `WrapHostMemory` на `MetalBufferManager` и `CreateGuestMemoryView`. При повторном использовании view проверяется актуальный физический buffer, offset, stride, format, extent и достаточный mapped span. Remap с прежними размерами не оставляет старую физическую страницу в кэше.

Проверки до запуска игры:

- 28 пар tile-layout/element format; 497 233 байта читателя из GPU-писателя и 2 189 447 байт из остальной гостевой памяти. Полный scratch, все выходные тексели, неизменность source/guest, ненулевой padding, guards и ошибочные диапазоны — PASS.
- Повторная конверсия с теми же source/target после изменения одного гостевого байта вне writer footprint — PASS.
- Отрицательный контроль со старым zero-fill алгоритмом завершается ожидаемой ошибкой `alias conversion lost guest bytes outside writer footprint`.
- Metal API Validation нового GPU-теста — PASS.
- Remap/read/write GPU test и уменьшение доступного span у кэшированного view — PASS.
- Полный автономный memory-stage набор — PASS: PM4 266 240 GCR комбинаций, CPU coherence 20 000 операций с побайтным эталоном, прежние GPU coherence tests, 58 2D tile cases, 8 mip chains, 30 volumes / 133 mip round-trips, весь native MSL suite включая 4806 swizzle results.
- Сборка `kyty_emulator` и whitespace check — PASS. Предупреждения deprecated API и linker warnings остаются.

Повторение из `/Users/tx/Developer/KyTx`:

```bash
bash tools/test_emulation_memory_stage.sh
MTL_DEBUG_LAYER=1 bash tools/test_metal_coherence.sh
```

Отрицательный контроль, только в тестовой программе:

```bash
KYTX_ALIAS_TEST_ZERO_BASE=1 bash tools/test_metal_coherence.sh
# Ожидается exit 1.
```

Это завершённые исправления двух рабочих путей, а не завершённая система когерентности всех кэшей. Глобальное ядро MemoryCoherence ещё не связывает все RT/storage/sampled/BC/depth/3D representations и CPU dirty/remap events. Авторитет private writer внутри его собственного footprint всё ещё определяется прежней политикой; эта часть требует дальнейшей реализации. Открыты диапазоны через границы backing chunks, DCC/CMASK/HTILE, 3D runtime alias transitions и другие пункты основного аудита. Эти изменения не объявляются исправлением всех старых игровых артефактов. Новые NaN/TAA-заглушки и пропуски шейдеров не добавлены.

Новая сборка скопирована в игровой slot s3; SHA-256 `8023ff7020033d9aeca3380533c208578d7c0cbeb9d42248a4725672577613e7`. Игра запущена 02:55:07 Asia/Tashkent, PID 97150, tag `rm1-20261109-025507`. Временная смена RUN_TAG отменена, `extra_env_s1.sh` побайтно восстановлен; использованы прежние UMA/GDS/CUBE и прочие настройки. Новый FPS-выигрыш не заявляется.

Игра дошла до 3D-сцены с персонажем (process-local frame 5785); сообщение EA servers unavailable закрыто штатной кнопкой подтверждения. Кадры продолжают поступать. В проверенном журнале нет GPU fault/progress timeout/command-buffer error. Лог [Surface] подтверждает выполнение новых runtime alias conversions. Игра оставлена работающей по команде пользователя. Старые дефекты освещения/одежды остаются; причинность с новыми изменениями не установлена. Снимок: `runtime-memory-game.png`. Полный матч и визуальный A/B этим запуском не проверены. Локальный журнал: `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/rm1-20261109-025507-025507.log`. Игровые данные и дампы в сеть не отправлялись; коммитов нет. Ledger CLI/store отсутствуют.
