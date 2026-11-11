# FC27 native Metal — codex_ci2 и исправления indirect draw, 2026-10-28

Текущее продолжение после compute-indirect follow-up. Только ARM64 FEX + direct RDNA→MSL, без Vulkan/Rosetta. Ветка metal-native-checkpoint, HEADc30d5d81; чужие dirty/untracked изменения сохранены, коммитов/stash/reset нет. Ledger CLI отсутствует, память обновлена в markdown. Новых агентов/чатов нет.

## Разрешённый прогон codex_ci2

Пользователь явно ответил «Да, запускай один прогон» на второй запуск до240s: wave64 ON, UMA OFF, run1/20GB/watchdog8s, split draw+compute с flip500. Выполнен ровно один, PID5354, лог `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/codex_ci2-140134.log`. Binary SHA256 `a99e671762a46e38a816aeb9ad5a49f8591463dbbeb4e3d99607573b13420909`.

Последний DrawStats: flip1575 submitted4886/executed4660, dropped_vs/ps/pipe=0. Последний Mem: flip1558 footprint8725MB/Metal7098MB. Watchdog8s завершил процесс. В его **отдельном CB ровно одна метка**:

`draw_idx #3266 rt0=0x8240940000 vs=0x4b2fb528c369ebff ps=0x74014bc2ef93d2c9 prim=4 idx=1 inst=1 fi=0 bv=0 tmask=0xf smask=0xf blend=0/1/0 gfmt=9 mtl=90`

Это сильнее прежнего общего CB ci1/uk5/uk8, но **не доказывает пустой direct triangle**. В native CommandProcessor::DrawIndirectMulti indexed вызывает DrawIndex с `index_count=1, instance_count=1` как заглушкой; реальные counts/offsets находятся в GPU args. Старые labels не различали direct и indirect. Не считать idx1/inst1/fi0/bv0 фактическими параметрами Metal indirect draw! Принадлежность конкретного ci2 draw к MDI ещё требует нового label/аргументов. Гипотеза старого handoff «пустые idx1» не подтверждена; включение SKIP_EMPTY_DRAWS раньше могло удалять целые MDI пакеты по placeholder.

CUA увидел чёрное окно со счётчиком1705/FPS19, хотя GPU уже не завершал работу. Счётчик окна не является счётчиком завершённых GPU кадров. Новых PPM нет (trigger выставлен, завис до выгрузки), gputrace не снимался. Не запускали Xcode replay параллельно. После выхода pgrep не нашёл игры/LLDB/gpudebug.

**Сохранение исходников теперь работает:**251 MSL, ошибок MslDump нет. Все4 подозреваемых VS/PS находятся в `.porting/artifacts/2026-10-28/dispatch-indirect/ci2-msl/0x{hash}.metal`. VS4b2fb528 —1184 строки, без гостевых loops/barriers, только2 bounded helper loops; PS74014bc2 —5016строк, гостевые loops содержат cap256. C8VS569строк, F2PS6042строки. Отсутствие loops в VS и наличие cap в PS сами по себе не доказывают отсутствие GPU fault/слишком тяжёлого draw.

Копия полного ci2 лога сжата в artifact dir, summary в run-ci2-summary.json. Большой лог (~668тыс.строк) вызван FAULT_INFO resource binding logs. Не печатать его целиком.

## Исправленные дефекты после прогона (ещё не проверены игрой)

1. **Native multi-draw читал GPU count на CPU до native routing** и сразу возвращал при старом нуле. В graphicsRun.cpp native routing теперь расположен перед этим чтением. Legacy CPU path сохранён. GPU count выбирает активные записи в GPU helper.
2. **Single indirect draw тоже сначала memcpy аргументы на CPU**, а nonindexed затем мог быть отброшен в DrawIndexAuto по старому vertex_count=0. Native single draw теперь использует тот же native one-record batch, без CPU-чтения arguments/count/offsets. Indexed single также получает GPU arguments и range validation. Native вызов не меняет legacy non-native path. DiStats и MdiStats теперь оба учитывают native single route; их числа нельзя трактовать как число входных multi-PM4 packets.
3. **Indexed indirect не ограничивал indexStart/indexCount индексным диапазоном**, в отличие от проверок direct/legacy. Добавлен production MetalMultiDrawIndirect (metalIndirect.h/.mm): отдельный небольшой GPU kernel после producer. Он копирует4/5 аргументов из guest stride/offset в64-byte records; предицирует instanceCount по GPU count; для indexed ограничивает count доступными индексами после start. Недопустимый start превращается в count0/start0. Ограничение берётся из guest INDEX_BUFFER_SIZE (или прежнего1Mi-index fallback) и фактической оставшейся длины привязанного MTLBuffer. Не введён произвольный лимит instanceCount/vertexCount и не заявлена причинность bounds для ci2 hang. Guest args не меняются. Читаются только4/5dwords, старый loop по произвольному stride удалён; source addressing64-bit.
4. **SplitAfterDraw внутри MDI loop закрывал encoder после первого элемента**. Остальные не имели encoder/pipeline и молча терялись. DrawIndirect/DrawIndexedIndirect принимают last_in_batch, renderer задаёт его только последней записи. Диагностический split теперь делит по guest packet, а не ломает batch; внутри packet могут быть несколько Metal draws.
5. **SKIP_EMPTY_DRAWS больше не принимает MDI placeholder за пустой direct draw**. Pending state потребляется в начале DrawIndex даже при early return. Indexed trace получает args/count/max/stride; labels включают `mdi=1 args=... max=... stride=... count=... ib=... bytes=...`.
6. Старый RunMultiDrawClamp при ошибке мог оставить stale scratch и вызывать draw с ним (return игнорировался). Общий PrepareMultiDrawArguments очищает output и явно пропускает пакет при подготовительной ошибке. Counted nonindexed использует его перед существующим RECTLIST преобразованием, перед binding render state. Original RECTLIST/packed fixes сохранены.

Обычная отрисовка не читает snapshot на CPU. В64-byte record Metal args offset0, original4/5dwords offset32; raw[5]=GPU count (или immediate max), raw[6]=index_limit, raw[7]=indexed. Shared tracked buffer переиспользуется в queue order. Новый дополнительный prepare для indexed packets требует игровой оценки производительности; FPS-выигрыш не заявлен.

Диагностика `KYTY_MDI_TRACE_HASH=<VS> KYTY_MDI_TRACE_N=<packets, default4>`: после GPU prepare, до установки render state, bounded readback через CommitCommandBuffer(true). Печатает MdiArgs для первых8 записей максимум Nпакетов (raw/cooked counts/offsets, gpu_count, bound). Нормально выключена. Перед wait label mdi_snapshot; pass после wait открывается Load. Возможное влияние этой синхронизации на воспроизводимость необходимо учитывать.

## Проверки

- Новый GPU test использует **production MetalMultiDrawIndirect и MetalDrawExecutor**; args и count создаёт GPU producer в **private buffers**, source offset16/count offset8/guest stride32. Legacy split control теряет64из128пикселей (ровно правую половину), исправленный рисует обе.
- 17положительных сценариев: counts0/1/2/UINT32_MAX, immediate count при неиспользуемом нулевом GPU counter, indexed/nonindexed, zero instances, indexCountUINT32_MAX, startUINT32_MAX/start==limit/нулевойlimit, signed baseVertex−4, firstInstance7/9, original source неизменён, repeated helper reuse, MDI→RECTLIST3→4 с двумя записями и predicated count, one-record indexed/nonindexed и GPU zero vertex count. Плюс3 invalid extent/alignment controls. Пиксели проверяются по всей картинке16×8, raw snapshots/диапазоны проверяются отдельно.
- Существующие compute-indirect7cases/MRT/stencil/depth suite проходят. `tools/test_metal_pipeline.sh` включает отдельный запуск MDI tests с SPLIT_DRAWS=1 от0 под API+shader validation. Полный `tools/test_metal_msl.sh` прошёл, в том числе все RECTLIST packed modes.
- ARM64 target build и git diff --check проходят. После последнего изменения single route актуальные build-final.log и pipeline-final.log завершились с exit0. Итоговые17GPU cases/0fail; main binary SHA256 `cfb0a4e38de9d8ce1af799059b59c9b4864789bd6ca29c7ba929f074d093a3b2`.

Артефакты новых изменений: `.porting/artifacts/2026-10-28/dispatch-indirect/mdi/`: before/after snapshots, manifests, change.patch, build-final.log, pipeline-final.log, msl-regression.log, binary-sha256.txt. Не накатывать полные snapshots поверх чужих последующих правок.

## Продолжение и разрешение

После ci2 новых запусков игры не было. На **один codex_ci3 до240секунд** пользователь ответил «Да, один прогон». Этот один согласованный запуск начат; результат будет записан ниже. Подготовлен `mdi/env-codex_ci3-planned.sh`: тот же wave64 ON/UMA OFF/run1/20GB/watchdog8s, split draw+compute от500, MSL dump вci3-msl и MDI_TRACE_HASH=0x4b2fb528c369ebff/TRACE_N12. Active extra_env_s1.sh ещё ci2. Не запускать ci3 без нового явного OK пользователя из-за сохранённого ограничения (предыдущие GPU hangs вызывали Mac reboot).

После OK: финализировать SHA256 и source snapshot, скопировать planned env в active env, запустить ровно1run1, читать MdiArgs **перед** GPU draw. Если clamp сработает — выяснить производителя неверных args, а не объявлять корневую ошибку устранённой. Если зависнет с валидными args — следующий шаг PS/VS GPU bounds/фактические textures, затем одиночный gputrace для Xcode, без тяжёлого параллельного replay. Старые gputrace из прежних handoff сейчас отсутствуют на указанном пути captures; не считать их доступными без проверки файла.

Меню/интро/HDR NaNs/wave64 stability/матч30+FPS ещё не признаны исправленными. UMA не необходима для hang (ci1+ci2 UMA OFF). Отдельный uk6 assert commit-with-uncommitted-encoder пока не локализован. Только native FEX+MSL.
