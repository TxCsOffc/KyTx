# Продолжение той же сессии Claude — 9 ноября 2026

Репозиторий: `/Users/tx/Developer/KyTx`, ветка `metal-native-checkpoint`, HEAD `9534b33718bd10104a4baf708758d91f61e2ef8d`. Продолжена сессия `/Users/tx/.claude/projects/-Users-tx/52cdae17-1a05-40ea-912d-91012d435db7.jsonl`, а не создано новое дерево разработки. Существовавшие изменения Claude и Codex сохранены. Коммитов и публикаций нет.

## Что оставил Claude

Claude откатил только сессию Antigravity, которая сканировала гостевую память в PM4-потоке и замедляла меню. Эти сканы не возвращены. Его новые footprint/clear-decode тесты, Wait8/16, экспорты testcancel/rmdir, диагностика unbound image и тест сравнения глубины сохранены. SurfaceCoherence adapter существует, но ещё не подключён ко всем runtime owners; его мини-тест не означает завершения интеграции.

Последняя просьба в сессии: сначала довести переносы из upstream, изучая смысл и адаптируя к нашему Metal/FEX backend; не применять игноры и фиктивный успех.

## Реализовано в этом продолжении

- **mlock:** убран безусловный успех. Проверяются переполнение и все страницы гостевого диапазона, выполняется настоящий host `mlock`/`VirtualLock`, ошибки возвращаются гостю. На macOS тест проверяет фактический `user_wired_count`, а не только код возврата. Это закрепление host pages, не реализация всей модели памяти PS5. Основание семантики: [Apple mlock(2)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/mlock.2.html).
- **libc ABI:** по upstream `8c01d8e8` исправлены NID `strcmp`/`strncmp`/`strcpy`; прежний NID `5Xa2ACNECdo` получает настоящий трёхаргументный `strcpy_s`, с границами, terminator, ошибками NULL/overlap/size. Раньше к нему ошибочно привязывался `strcpy` с другой сигнатурой.
- **Файлы:** по смыслу `cbe5b629` независимые открытые FILE handles видят записи и truncate без закрытия; сохранены Darwin access hints. `KernelFsync` вместо пустого успеха выполняет `fflush` и host `fsync`, включая открытый directory descriptor. Добавлен POSIX export. Ошибки реальны. Directory handles сохраняются и переживают rename.
- **rmdir/rename:** удаляется только действительно пустой каталог; защищены mount root, `.`/`..`, directory symlink, nonempty/error cases; повторные `/` нормализуются. `rename` теперь принимает каталоги и использует атомарную host-операцию: при отсутствующем источнике существующий файл назначения не удаляется. Родители автоматически не создаются.
- **PM4 ES/LS:** `SPI_SHADER_PGM_RSRC1_ES` (0x0CA) и LS (0x14A) больше не игнорируются. Полное значение сохраняется раздельно в shader context, декодируются allocation/float/component fields, состояние копируется и сбрасывается. Проверены indirect ES и direct LS dispatch. Это архитектурное состояние, а не завершённый geometry/tessellation backend. Специальные front-stage float modes и отсутствующие празднования этим пакетом не объявляются исправленными. Другие старые ignore handlers ещё существуют.
- **TLS:** по `42bb0da5` корректировка стека в TLS stubs/SafeCall использует LEA, сохраняя guest arithmetic flags; сохранены фиксированные offsets. На M5 x86_64-код реально исполнен через Rosetta, проверены шесть destination registers и два patterns FLAGS, TLS result и сохранность RAX. Возврат старых SUB/ADD только в negative-control тесте воспроизводит отказ.
- **Логирование:** по `be9e1391` форматирование вынесено в noinline helper, чтобы гостевые fiber stacks не получали крупный fmt frame в каждом месте вызова.

Перенос выборочный, не merge всех 904 upstream-коммитов. Binding architecture upstream несовместима с текущим MSL backend, поэтому её не подменяли механически. Наши ATRAC9/audio изменения сохранены. Патч с возвратом выдуманного entitlement-state не переносился.

## Проверки до игры

`bash /Users/tx/Developer/KyTx/tools/test_upstream_ports.sh` завершён с кодом 0. Он собирает main, kernel FS/virtual memory/sync tests, запускает:

- `kernel_file_system_tests --port-contracts-only`: independent handle coherence, реальные libc exports через FEX ABI на ARM64, fsync/rmdir/rename, PM4 ES/LS state.
- `virtual_memory_allocation_tests --mlock-only`: реальное wiring, unaligned/multiple pages, NULL/overflow/unmapped/partly mapped/host-only pointers, доступ после lock и отказ после unmap.
- SyncOnAddress, исполняемые TLS stubs (12 cases), весь прежний `test_emulation_memory_stage.sh`.
- PM4: 266240 GCR combinations; coherence byte-oracle: 20000 операций; clear decode: 1200 случаев; guest views/remap; alias preservation: 28 layout/format pairs; Metal CPU→GPU→GPU→CPU; 2D/3D tiling/mips; native MSL GPU suite, включая 4806 stride-swizzle results — PASS.
- TLS old-code negative control: ожидаемый exit 1, `TLS load changed caller arithmetic flags`.
- `git diff --check`, обратная проверка focused patch — PASS.

Полный старый default FS harness на ARM64 не заявлен прошедшим: в прежних тестах есть прямые native-вызовы x86 guest stubs. Новый port-contracts mode вызывает их корректно через FEX. Windows не запускался. Cross-guest-mount EXDEV для mount points на одном host filesystem отдельно не моделируется. Изменение Common::File::Flush теперь даёт durable fsync; производительность shader cache отдельно не измерена.

## Игра после тестов

BUILD_SHA256: `2d8593610d3a6352218a55229f31ecbf615d6b440d0798a70ca23077b06f7e56` (main и s3).

Первый прогон `cu1`, PID22275, был остановлен самим тестовым guard на 26 ГБ перед матчем: это ошибочно пониженный нами лимит, не GPU fault. Повторный `cu2` использует прежние 40 ГБ Claude. Запуск 06:27:35 Asia/Tashkent, PID22969, один эмулятор. Сценарий титул→меню→Kick Off→Manchester United/Liverpool. Флаги как c9: `KYTY_NO_UMA_SURFACES=1`, `KYTY_MSL_GDS=1`, `KYTY_MSL_CUBE=1`, `KYTY_FEX_FAULT_RIP=1`, Metal HUD, capture выключен. Только process-local frame dumps/window screenshots, без GPU trace.

Cu2 завершил десятиминутный сценарий без остановки эмулятора. Матч Manchester United — Liverpool подтверждён несколькими кадрами (игровое время 02:52 → 20:02 → 39:51, финальный кадр13565); в просмотренных игровых кадрах Metal HUD примерно 22–27 FPS, с отдельными просадками. GPU fault/command-buffer errors/progress timeout/сигналов падения в проверенном журнале не найдено. На 2026-11-09T06:37:41.867289+05:00 игра оставлена работающей, PID22969; auto-input сценарий завершён, исходный extra_env_s1.sh побайтно восстановлен. Монитор исходного лимита40GB остаётся. Последний screenshot: `/Users/tx/Developer/KyTx/.porting/work/out/cu2/shot-0600.png`; доставленная копия `claude-continuation-match.png`. Лог: `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/cu2-062735.log`. Это smoke menu→match, не полный матч и не доказательство визуальной корректности.

Старые блоки освещения и другие артефакты видны; по уточнению пользователя они существовали до наших 3D изменений. Этот прогон не является контролируемым A/B и не доказывает прирост FPS или исправление визуальных проблем.

## Следующие реализации

1. Довести runtime memory coherence: события CPU writes/remap/lifetime и GPU owners, raw↔texture transitions, physical identity через границы chunks; не возвращать полные сканы памяти в PM4. Сначала независимые byte-oracle/GPU мини-тесты.
2. Разобрать и перенести shader resource/binding изменения совместно с MSL backend, сохраняя его контракт; не копировать upstream layout изолированно.
3. DCC/CMASK/HTILE, eliminate/FMASK transitions: текущий clear decoder не заменяет их полную семантику.
4. Реальный ненулевой unbound compute image 64x64 RGBA32UInt tile5 (shader `5541c203985b0576`) и неизвестный импорт `WxLcN76q19o`: установить причину/контракт, не придумывать успешный ответ.
5. Sampler depth compare/PCF: существующий opt-in тест Claude имеет 13/360 отказов, это незакрытый дефект. `KYTX_MSL_DEPTH_COMPARE_TEST=1` не входит в зелёный default suite. Далее GS/HS и остальные найденные пробелы; missing celebrations пока не локализованы.

Патч `claude-continuation-ports.patch` содержит только 19 файлов этого продолжения относительно сохранённого состояния Claude (15 существующих и 4 новых), а не весь dirty diff репозитория. Начальные версии лежат в `work/claude-upstream-continuation/before/` рабочего каталога Codex. Тестовый лог `claude-continuation-tests.log` содержит полный вывод сборки и тестов.
