# FC27 native Metal — исправлен косвенный compute-dispatch, 2026-10-28

Продолжение после чтения handoff Claude за27/28 октября. Ветка metal-native-checkpoint, HEADc30d5d81, все предыдущие dirty/untracked изменения сохранены. Коммитов нет. Только FEX ARM64 + direct RDNA→MSL. Vulkan/Rosetta не запускались. Новых агентов/чатов нет; Ledger CLI отсутствует.

## Подтверждённый дефект и уточнение предыдущего handoff

Предыдущий handoff предполагал, что DispatchIndirect повторяет прошлый pipeline. По фактическому коду BeginComputePass всегда завершает encoder, создаёт новый и сбрасывает m_has_compute_pipeline=false. Native MetalRenderer::DispatchIndirect не получал текущую CS-программу, не привязывал pipeline/resources и вызывал executor с64×1×1: **обычный indirect dispatch пропускался**. Это подтверждено отдельным GPU control через production MetalDrawExecutor.

В CommandProcessor::DispatchIndirect режим USE_THREAD_DIMENSIONS до native routing читал12байтов аргументов на CPU и вызывал DispatchDirect. Когда аргументы пишет GPU, это гонка с незавершённым producer. Host clamp в executor также читал/менял args раньше GPU-записи и изменял гостевой буфер.

**Это доказанный дефект, но его причинность для зависаний UMA+wave64/полного HDR-мусора пока не установлена.**

## Исправление

- MetalRenderer.h/.mm: direct/indirect теперь входят в общий private DispatchCompute. Текущие CS program/pipeline, user data, buffers, images, residency и реальные размеры группы готовятся одинаково. Используется существующее решение о физической host-геометрии с учётом скомпилированного wave64-pair режима. Отсутствующий pipeline приводит к явному dropped dispatch. Косвенные размеры больше не заменяются64×1×1.
- graphicsRun.cpp: native routing indirect выполняется до CPU-ветки USE_THREAD_DIMENSIONS. Legacy non-native path сохранён; не запускался.
- metalIndirect.h/.mm: production MetalComputeIndirect. Отдельный простой GPU kernel (3потока, без циклов) читает аргументы после producer, считает ceil(threads/guest-workgroup) при USE_THREAD_DIMENSIONS, ограничивает по KYTY_MAX_INDIRECT_GROUPS (default65535), пишет32-byte snapshot. Новые Metal group counts: offset0; оригинальные3uint:offset16. Guest args не меняются. Ceil не переполняется при UINT32_MAX. Деление идёт по **guest** local size, до wave64 packing; физическая группа назначается затем по compiled metadata. Pooled tracked shared buffer, snapshot не читается на CPU в обычном dispatch.
- metalDraw.mm: удалён CPU clamp; executor проверяет extent/alignment и выдаёт Metal indirect command с переданной геометрией. Исправлено encoded accounting для indirect. Sync/split logs помечают GPU counts неизвестными (нули в diagnostic begin, не реальные размеры).
- PassTraceReport печатает реальные sanitized groups и original raw после GPU queue completion. То же для CS-probe expected count. Обычный путь не ждёт GPU ради прочтения args.
- Для direct USE_THREAD_DIMENSIONS ceil также заменён на деление+remainder без uint32 overflow. Metadata fast-clear/HTILE heuristics не применяются к indirect, где полный охват поверхности на CPU неизвестен.

Крупная общая resource-binding логика DispatchDirect не продублирована. Общий путь не меняет UMA enablement, wave64 defaults, pitch, stencil, RECTLIST/packed/UI fixes или old hang diagnostics.

## Проверка

1. До правок `bash tools/test_metal_pipeline.sh`: исходные GPU tests проходят.
2. После: тот же suite с включёнными Metal API и shader validation проходит; добавлены7GPU cases в tests/metalPipelineTests.mm. Используются production MetalComputeIndirect и MetalDrawExecutor. Аргументы пишет предыдущий **GPU compute kernel в private buffer** (CPU читать его не может), source offset16. После GPU prepare заново привязываются consumer pipeline, другой output buffer и меняющийся uniform tag.
   - groups3×2×1/local2×3×1:36 threads, проверены local x/y/z sums;
   - thread counts5×4×2/local2×3×2:groups3×2×1,72 threads;
   - zero axis:0 work;
   - GPU clamp17×6×1→4×4×1;
   - UINT32_MAX thread count:ceil без overflow, затем clamp4;
   - guest64/local host32 для paired geometry:1group,32 physical threads;
   - all-zero counts:0 work.
   Все7cases в одном queued command buffer, переиспользуется один argument snapshot; после completion сверены новые counts, original snapshot, **неизменность source args**, число threads, local IDs, uniforms и output bindings. Есть3 invalid-source checks.
3. Legacy control: BeginComputePass без pipeline + DispatchIndirect пропускается, как в старом renderer. Лог это явно показывает. Старый negative depth-only pipeline control в stderr `create failed` ожидаемый, не новая ошибка.
4. Полный `tools/test_metal_msl.sh` с MTL_DEBUG_LAYER=1/MTL_SHADER_VALIDATION=1 проходит, включая все предыдущие RECTLIST coverage/packed fixes.
5. ARM64 `cmake --build _Build/macos-arm64 --target kyty_emulator -j8` проходит; git diff --check проходит.

Артефакты: `/Users/tx/Developer/KyTx/.porting/artifacts/2026-10-28/dispatch-indirect/`: before/ и after/ с manifests, change.patch (только эта работа относительно прежнего dirty baseline), pipeline-baseline.log, pipeline-after.log, msl-regression.log, build-final.log, binary-sha256.txt. Не накатывать полные snapshots поверх последующих чужих изменений без diff.

## Первый согласованный прогон codex_ci1

Пользователь явно ответил «Да, можно запускать сейчас» на один прогон до240секунд с wave64 ON, UMA OFF, run1/20ГБ/watchdog8s. Выполнен ровно один такой запуск, PID4660. Код indirect fix включён. Binary SHA256 до dump-fix: `94cf02e4ac814c679e86b498f723ef2d3ec1fe506042a3aad85694aa979ac681`.

Лог: `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/codex_ci1-133823.log`, копия в artifact dir. Последняя статистика: flip775, submitted4866/executed4567, dropped_vs/ps/pipe=0. Footprint максимум9252MB. Watchdog через9секунд (limit8) завершил процесс. Процесс исчез из pgrep; никаких эмуляторов/replayer на момент записи нет. До меню не дошёл, новых screenshots/gputrace нет. Vulkan/Rosetta не использовались.

**Новое доказательство:** зависание воспроизвелось с отключённой UMA. UMA не является необходимым условием этого зависания. Пакет совпадает с uk5/uk8: CS38dd66287e3b3c9d×2, CS2f4dadde068a486e, CS0471bc49224eb9d9, drawVS9044bad2824f92e4/PS5fcb277d135cb042, затем9×indexed triangle-list idx1 inst1 fi0 bv0 VS4b2fb528c369ebff/PS74014bc2ef93d2c9, затем idx456 VS c8cfa7d23e64e28f/PSf2d2f61d255beb2e; CS938a121888afce63, CS0122e6902be1a23a, CS44c8d0b4ea6e280c. Полная строка в run-summary.json. В логе нет indirect labels; текущую причинность исправленного indirect dispatch для данного hang не установили.

**Ошибка диагностирования:** каталог `KYTY_MSL_DUMP_DIR` не был создан перед запуском. Старая запись `fopen` молча пропустила ошибку, MSL НЕ сохранились. В commentary сначала неверно было заявлено об их сохранении; затем явно исправлено. Искомых4 VS/PS исходников в доступных старых каталогах тоже не найдено. Старые gputrace из предыдущих handoff сейчас отсутствуют в `fex-spike/runs/captures`; ссылку на старый trace не принимать за существующий файл.

## Исправление записи MSL после codex_ci1

`metalShader.mm`: автоматическое создание вложенного каталога, запись UTF8 с atomically:YES, явные ошибки `[MslDump]` при неверном пути/ошибке directory/write. Убран фиксированный512-byte path. Нормальный shader pipeline не менялся. Backup, dump-fix.patch, after-dump-fix-metalShader.mm и новый binary hash сохранены отдельно.

Проверено существующим `tools/test_metal_shader_cache.sh` с новым ранее отсутствовавшим вложенным dump dir: legacy control2fail ожидаемо, specialized0fail; записаны3полных исходника правильного содержимого (verified.json), ARM64 build и git diff --check проходят. Логи shader-cache-dump-check.log/build-dump-fix.log. Полные MSL/pipeline suites ранее этого turn проходили после indirect fix; после только dump-fix не повторяли их без необходимости.

## Продолжение и разрешение

На дополнительный **один** codex_ci2 до240секунд отправлен новый async-вопрос; ответа на момент записи ещё нет. Запуск без ответа запрещён сохранённой пользовательской просьбой получать отдельное OK на каждый запуск (прошлые тесты вызывали GPU hang/reboot). Текущий `extra_env_s1.sh` всё ещё codex_ci1; planned ci2 env лежит отдельно в artifacts, ничего не запущено.

Подготовлен `env-codex_ci2-planned.sh`: wave64 ON, UMA OFF, LOOP_CAP256/watchdog8s, тот же run1/20GB. **Оба** KYTY_MTL_SPLIT_OPS=1 и KYTY_MTL_SPLIT_DRAWS=1 с флипа500 (старый FROM900 был слишком поздним для ci1), KYTY_MTL_GPU_TIME=1 и FAULT_INFO=1. Не пропускать idx1/другие shaders пока: цель — локализовать пакет до одного guest draw/dispatch вместе с его helper work, сохранив фактическое поведение. Эти flags диагностические, не исправление/performance result. Последовательные submit могут менять воспроизводимость. Watchdog ограничивает время ожидания процесса, но не гарантирует остановку GPU.

После разрешения скопировать planned env в extra_env_s1.sh и выполнить ровно один `.porting/work/run1.sh codex_ci2 240`, мониторить лог и dump errors, снять PPM по trigger при достижении сцены. Не запускать тяжёлый GPU replayer параллельно. После: проверить наличие4MSL, разобрать controlflow/barrier/bounds; при hang выделить первый незавершённый CB, а не считать последний op в общем пакете виновным. `idx1` не может давать треугольник, но VS всё равно запускается, поэтому одного отсутствия примитивов недостаточно для вывода о безопасности.

Без разрешения — offline investigation. Не заявлять исправленные меню/интро/матч30+FPS. UMA/pitch/HDR NaN, wave64 stability и отдельный assert uk6 «commit command buffer with uncommitted encoder» ещё не решены.
