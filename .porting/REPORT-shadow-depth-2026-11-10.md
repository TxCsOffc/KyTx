# Depth arrays, guest array data and clip Z — 2026-11-10

Пользователь подтвердил, что старые артефакты остаются, и попросил продолжать исправление полноценной эмуляции. Проверялся native RDNA → MSL; новые заглушки, NaN/TAA sanitizers, shader skips не добавлялись. Исходный dirty/untracked код сохранён, коммитов и публикаций нет.

## Подтверждённые пропуски и реализации

1. **Отдельные каскады depth array терялись.** Реальная игра пишет D16 2048×2048 с DB_DEPTH_VIEW slice 0, 1, 2, а прежняя native allocation и pass key не учитывали slices. Теперь MetalAttachmentArray хранит реальные private depth/stencil arrays, сохраняет старые слои GPU-копированием при росте, создаёт отдельные 2D и ranged array views. Pass compatibility учитывает slice/mip/extent. RenderTargetArrayLength передаётся в Metal. Graphics и compute читают тот же authoritative GPU writer с выбранным диапазоном слоёв. Статическая depth map не становится устаревшей только по истечении двух кадров; чтения обновляют её lifetime.
2. **HTILE clear должен потребляться по слоям.** Как в Vulkan ClearMeta/TouchMeta, новый PendingDepthClears сохраняет маску слоёв. Очистка cascade 1 не стирает уже нарисованный cascade 0. В промежуточном SD1 использовалась очистка всего массива; это обнаружено при сравнении с Vulkan и исправлено до SD2/SD3. Clear регистром применяется к выбранному view. Полный decode значений HTILE/DCC этим не реализован: прежний выбор значения far plane для HTILE остаётся ограничением.
3. **Sampled arrays создавались без guest contents.** Initial/growing LayeredImageEnsure теперь читает реальный tiled guest snapshot и загружает все его слои/mips через UploadTiledArrayMipChain. Helper декодирует и проверяет весь snapshot до первой записи. Существующие GPU slices затем перекрывают импорт реальными копиями. PrepareLayeredImages явно сообщает ошибку запрошенной материализации, а не пропускает её до пустой подстановки. Authoritative GPU depth arrays не заменяются CPU snapshot. Реальная RG16Unorm 128×128×64 содержит 4 177 716 ненулевых байтов. R16Float такого размера действительно имел нулевой guest source; его старый нулевой dump не доказывает ошибку именно этой карты.
4. **Z_READ_ONLY игнорировался.** Общий ResolveGuestDepthControl используется для Metal depth state и feedback tracking в indexed/automatic draws. STENCIL_READ_ONLY также блокирует explicit stencil clear, как в Vulkan.
5. **Native position export игнорировал clip_space.remap_z.** Production SPIR-V преобразует GL clip Z в (Z+W)/2, native MSL этого не делал. Теперь преобразование выполнено для обычного и packed position export; cache schema поднята до msl-s4-clip-z. Полные clip_space.enabled XY/clip_disable преобразования остаются отдельной задачей: их нужно согласовать с physical viewport, а не копировать формулу изолированно.

## Проверки перед игрой

| Проверка | Старый/ошибочный контроль | Реализация |
|---|---:|---:|
| Tiled array initial data, 27 arrays, 5013 subresources | blank allocation: 5013 ошибок | 0 ошибок; 27 truncated-source atomic negatives |
| Z_READ_ONLY, 128 D16/D32 GPU cases | 8 повреждений depth | 0 |
| Depth/stencil arrays, growth/views/layered draw | forced layer 0: 640 ошибок в ранней версии 2176 texel suite | финальная версия 3328 texels, 0 |
| Per-layer HTILE mask, часть тех же 3328 texels | ClearAll вместо selected layers: 384 ошибки | 0 |
| Same-IR native/SPIR-V clip Z, 120 GPU cases, 459460 checked pixels | test-only legacy remap: native 27174, reference 0 | native 0, reference 0 |

Vertex test проходит через настоящий native emitter и product SPIR-V emitter → optimizer/validator → SPIRV-Cross → Metal; CPU независимо рассчитывает clip/coverage/depth. Есть positive W 0.5/1/2, Z ниже/внутри/выше clip range, обычный и reversed viewport depth, normal/half packed exports. В начальной версии harness была неверная SPIRV-Cross argument-buffer binding (id0 занят size pointer); она исправлена до результатов выше. Ранние reference mismatches не являются дефектом эмулятора.

Полный metal_pipeline suite с Metal API/shader validation PASS (MRT, stencil, indirect, multi-draw), полный metal_msl GPU suite PASS (image/depth sampling, offsets, interpolation, buffers, atomics, RECTLIST и остальные проверки). Tiling/mips/volume suites PASS: 58 cases, 18 mip reads, 30 volumes/133 round trips, 8 guest chains. Product build PASS; git diff --check PASS. Новые pipeline и vertex clip проверки включены в tools/test_emulation_memory_stage.sh. Остальные неизменённые CPU stages в этом этапе повторно не запускались; общий suite проходил предыдущий этап.

Основные логи: work/shadow-array/array-after.log, array-before-gpu.log; work/shadow-depth/htile-before.log, htile-after.log, depth-array-before.log; work/shadow-clip/clip-packed-legacy.log, clip-final.log, msl-regressions.log в Codex workspace. Негативные переключатели используются только standalone tests.

## Реальная игра

SD3 запущен 2026-11-10T00:39:00.985365+05:00; PID 60174, main/s3 SHA256 2e384a69d5de3b6571d7a39e4691da709bb8ef77a3ed87bd50a68564b1d0c65b.
Log /Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sd3-003900.log.
Один эмулятор, исходный 40 GB guard, без timer kill, capture/validation off, нет автоматического сценария ввода. extra_env_s1.sh восстановлен. SD2 PID59088 остановлен по проверенному PID перед SD3; SD1 PID58106 ранее также остановлен по проверенному PID. Более старые процессы описаны в истории STATE.

Runtime подтверждает distinct depth views 1:1, 2:2 и чтение 0:2 из одного parent, а также отдельные HTILE clears. Первый SD3 screenshot (frame895) показывает main menu на улице и видимую тень от игрока. У SD2 на другом ракурсе тень игрока не была видна; это не строгий same-scene A/B и не доказательство исправления всех теней. Follow-up frame1827: The Grounds по-прежнему имеет чёрное небо и деформированную/шлейфящую одежду/волосы. Studio проверен отдельно на frame4482: ожидаемая оконная/игровая тень на полу по-прежнему не видна. Скриншот work/shadow-depth/sd3-studio-check.png. Проверка лога на00:41:36+05:00: 0 ненулевых unbound, 0 array import errors, 0 GPU/command-buffer/progress-timeout ошибок; 12 DepthLayer и17 DepthLayerRead записей. Это частичный visual smoke, не gameplay/full match. Ускорение FPS и полная визуальная корректность не заявляются.

## Оставшиеся границы

Глобальная coherence/CPU writes/remap/alias ownership не завершена. Existing layered cache refresh/versioning после CPU writes требует следующей реализации. Нет общего mipped-depth materialization, полных metadata decode и mixed color/depth layered targets. RenderTargetArrayLength backend проверен ручным MSL layer export, но native IR StageOutputKind.Layer/ViewportIndex всё ещё не реализован; отдельно отправленные game cascade draws в этом не нуждаются. Repeated tail mips на arrays и некоторые compressed/3D paths остаются ограничены. Старые fallback/unsupported paths проекта не удалены и не скрыты.

Следующий разбор: сравнить реальные VS/PS shadow-mask inputs/outputs с Vulkan по одному и тому же кадру; проверить Studio и gameplay, затем Layer/ViewportIndex exports и оставшиеся clip-space состояния с mini programs. Не объявлять старые артефакты новой регрессией без A/B.
