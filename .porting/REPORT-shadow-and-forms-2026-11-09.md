# Эмуляция: sampling offsets, RDNA packed arithmetic и реальный lighting replay

Checkpoint 2026-11-09T19:02:33+05:00. Старые артефакты теней, неба, освещения и одежды остаются открытыми. Пользователь отметил улучшения логотипов и бород; причинную связь с отдельным изменением не утверждаем.

## Реализации

- Обычный ImageSampleRaw больше не теряет signed6-bit texel offsets. Metal native fast path используется в его допустимом диапазоне[-8,+7], полный RDNA[-32,+31] выполняет настоящий per-texel sampling в выбранных mips. Проверены bias, clamps, min/mag, min/max reduction, pixel coordinates,2D/array/3D. Большие постоянные Dref offsets также больше не усекаются Metal.
- Реализованы V_LERP_U8(0x14d), V_SAD_U8(0x15a), V_SAD_HI_U8(0x15b), V_SAD_U16(0x15c): декодирование настоящих VOP3 words, перевод в типизированный IR и исполнение MSL/SPIR-V. В SAD добавлено unsigned saturation при CLAMP, включая существующую V_SAD_U32. LERP использует только младший бит каждого отдельного байта src2; переносы между байтами не смешиваются. Неподдерживаемые modifiers не принимаются молча.
- Никаких новых shader skips, NaN/TAA sanitizers, подставных ресурсов или констант для скрытия картинки.

Референсы: [AnyPS5, pinned3d7f3478](https://github.com/boykopovar/AnyPS5/tree/3d7f3478719d22979f2f6582d254d78770a5d201/core/shader/recompiler), [AMD RDNA2 ISA](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture), [Metal Shading Language](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf). AnyPS5 fetch выполнен; чистый отдельный reference worktree /Users/tx/Developer/external-ref/AnyPS5-3d7f3478. Его tools/rdna2_isa_forms.txt содержит1662 entries:1460 base/VOP3 и202DPP8/DPP16. Это каталог выбранных форм RDNA2, а не доказательство полной PS5-эмуляции и не число GPU-tested операций. Наш Decoder::Opcode имеет607 имен после добавления4; напрямую сравнивать эти числа нельзя.

## Проверки

- Новый tools/test_metal_rdna_forms.sh: **104103 CPU/GPU результата на каждом из двух путей**(208206 всего), настоящие bytes→decoder→translator→SSA→nativeMSL либо productionSPIRVoptimizer/validator→SPIRV-Cross→Metal.3375 комбинаций граничных входов +8192 случайных triplets,9 вариантов включая4 CLAMP.21 illegal-modifier negative checks и output guard. До реализации отрицательный контроль отвергал реальный opcode14d. packed-before.log/packed-final.log.
- Ordinary offsets636 CPU/GPU cases,216 same-IR SPIRV differential в предыдущем targeted run;96 больших constantDref offsets. Обычный sampling negative до исправления CPU0.0625/MSL0.78125; попытка напрямую передавать полныйRDNA offset вMetal далаCPU0.0625/MSL0.125 и была заменена правильным footprint. LOD-gradient test oracle скорректирован после независимого precise log2 probe; production epsilon/clamps не добавлялись.
- **Полный tools/test_emulation_memory_stage.sh PASS**: PM4 acquire/release,20000 coherence operations, clear decode, physical aliases, tiling/3D/mips, sampler,4360Dref+128aniso,636ordinary,96constantDref, formatted/buffer bounds, LDS, interpolation, phi/controlflow и dispatcher. memory-regression.log. Самостоятельные materialization tests также PASS, включая typed point-fetch alias.
- git diff --check PASS. Main build PASS.

## Реальная сцена и причина дальнейшего поиска

sh1(сборка171e40b0...) записал GPU trace frame1313, затем нашPID45579 остановлен после сохранения. Его точный df0a classifier replay совпал native/reference/CPU:357cells,8classcounters и sorted packed-coordinate lists.

sh2(сборка61429127...) достиг меню без нажатий; чёрное небо и шум газона видимы. Добавлена **только опциональная** диагностика KYTY_REPLAY_DUMP_DIR/HASH/TRIGGER: BEFORE/AFTER выбранных dispatch, реальные buffers/aliases, HDR/depth, все mips/layers/3D, swizzles, samplers, SRT, native/reference push ABI, exact group/thread sizes. Нативный dispatch после snapshot выполняется с восстановленными pipeline/bytes/residency. У диагностики есть явные incomplete/unsupported состояния; данных она не заменяет.

В frame505 сохранены75dispatch,31guesthash;18 уникальных kernels имели ненулевой grid. Все18 выполнены отдельно на сохранённых входах. Все записываемые изображения совпадают байт в байт между исходным кадром, directMSL и SPIRV→Metal. Native/reference counters совпадают; append list order может отличаться и проверен как multiset. Это не пустые проверки: captured-write-coverage.json подтверждает реальные изменения выходов, например0e838ae6 изменил79402bytes,49d11bb9 изменил390972bytes. В том числе проверены активные77b2d6ce/a4c06b45/28e424b7 passes с shadow/gather и cube inputs.

**Ограничение вывода:** совпадает backend lowering на одном и том же уже подготовленном IR и наборе Metal-ресурсов. Это не доказывает эквивалентность resource creation, graphics state, VS/PS, common decoder/IR, sync/alias/metadata handling с отдельным Vulkan runtime. Не считать18PASS исправлением старых теней. Дальше искать первое расхождение до этих входов(карты глубины, G-buffer, sky, graphics stages/state), сравнивая с реальным Vulkan reference.

Первоначальная batch summary была недействительной: отдельная утилита ошибочно биндила неиспользуемыйSRT atIndex160 в shader без этого поля, перезаписывая его buffer0. Исправлено, прежняя summary названа summary-invalid-unused-srt.json. Актуальные результаты: replay-results/summary.json и individual .log. Shared backing у read-only textures/buffers допустим; writable aliases/overlapping distinct writable views требуют дальнейшего snapshot support и не подменяются.

## Текущее состояние

Main/s3 SHA256 **5a9ade555169b2bd2c247aab9dd05a2d41816a3594b1d28c9acc8ebc353c67dc**. Итоговый запуск sh3 PID**52447**, начало2026-11-09T19:00:11.917785+05:00. Лог/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sh3-190010.log. Без GPU capture/replay instrumentation, без автоматических нажатий, исходный40GB guard, без timer kill. extra_env_s1.sh побайтно восстановлен. Итоговый smoke проверен 2026-11-09T19:03:38+05:00: sh3 PID52447 жив; screenshot outputs/emulation-shadow-and-forms-game.png показывает меню подготовки матча Man Utd—Liverpool, Start Match, frame2385. Это не gameplay/full-match proof. В проверенном логе0 GPUfault/command-buffer/progress-timeout ошибок и0 skippedCS; последняя coverage1000translated/0unsupported. Пользователь/контроллер мог перейти вKickOff; нашdriver не посылаетinput. Игра оставлена открытой с исходным40GBguard, безtimerkill иdiagnosticlayers. Старые визуальныеартефакты не объявленыисправленными.

Все артефакты локальны в /Users/tx/Documents/Codex/2026-11-09/new-chat/work/shadow-stage. Фокусированный patch от грязного baseline этого этапа:/Users/tx/Documents/Codex/2026-11-09/new-chat/outputs/emulation-shadow-and-forms.patch;18 файлов, source-state JSON рядом. Содержит изменения этого этапа, а не огромный inherited git diff. Предыдущие dirty/untracked сохранены. Коммитов/push нет; Ledger CLI/store отсутствует, STATE/HANDOVER/worklog обновляются.
