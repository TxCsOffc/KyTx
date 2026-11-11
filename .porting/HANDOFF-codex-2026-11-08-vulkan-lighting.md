# Хендофф Codex — Vulkan oracle и освещение Metal, 2026-11-08

## Прочитать первым

Пользователь остановил исследование: **«все пиши хендофф»**. Продолжать запуски сейчас не надо: сначала получить новую команду продолжать. Ранее в этой сессии пользователь разрешил любое число запусков: **«да уже можно сколько хочешь запусков, можно»**; это отменило старый запрет из хендоффа Claude, а не ограничения безопасности ниже.

**Результат: визуальный баг освещения НЕ исправлен; стабильность Vulkan НЕ исправлена.** Есть новые captures, несколько исключённых гипотез и проверенная отдельным GPU-тестом ошибка shadow compare. Изменение compare по умолчанию убрано после неуспешной визуальной проверки. Не выдавать функциональный тест за исправление картинки.

На момент передачи:

- Все наши эмуляторы остановлены. Последний native PID89550/cmt2 остановлен SIGTERM по просьбе пользователя. Нет активных helper run1/vk_timeline.
- Наши gpudebug219 (белый кадр) и226 (новый чистый кадр) закрыты. Чужие сессии не трогались.
- Native MslEmitter.cpp и tests/metalMslTests.mm **побайтно восстановлены к состоянию перед нашим shadow-экспериментом**, с сохранением прежних чужих dirty hunks. Эксперимент лежит отдельным patch, см. ниже.
- Native `kyty_emulator` заново собран после восстановления; `kyty_emulator_s3` атомарно заменён тем же бинарём. SHA256 обоих: `98c55d7f453bcbfe49742fdb6188672609933ec29da7c864fd7acfc3addc19e1`.
- `.porting/work/extra_env_s1.sh` восстановлен к прежнему mc35, без CMP_FUNC. Vulkan env.sh восстановлен к kk10/KosmicKrisp. Никаких экспериментальных default compare в обычном запуске не осталось.
- Внешний Vulkan reference содержит только новые opt-in диагностические probes; никаких новых rendering fixes в нём не включено.
- Коммитов и публикаций не было.

Сначала прочитать предыдущий `/Users/tx/Developer/KyTx/.porting/HANDOFF-claude-2026-11-08-vulkan-reference-and-lighting.md`, затем этот документ. Подробный рабочий журнал: `.porting/CODEX-vulkan-lighting-2026-11-08.md` (в конце есть финальные исправления ранних утверждений). Более старые `.porting/STATE.md` и CLAUDE.md содержат исторические, уже неверные сведения.

## Задача и ограничения

Продолжить работу Claude: стабилизировать внешний Vulkan-эталон, сравнить ОДНУ И ТУ ЖЕ сцену по проходам, исправить native Metal освещение/тени. В Metal тени часто слишком чёрные, освещённые области пересвечены; в студии пол полностью светлый вместо тёмного ковра с оконным пятном солнца. Vulkan сохранённые хорошие кадры ближе к PS5. Fallback из Claude: работать с существующими saved GPU traces, не застревать навсегда на Vulkan.

- Продукт только native Metal, Vulkan/Kosmic/MoltenVK остаётся снаружи репозитория.
- Один эмулятор одновременно. Никаких pkill по имени, останавливать только свой известный PID. Не таймером убивать Vulkan. Не повторять MasterSemaphore/label-hold эксперимент: уже отвергнут пользователем за падение FPS и отсутствие визуальной пользы.
- Сохранять огромный dirty tree, не git add ., не коммитить без команды. Не выкладывать FC27 screenshots/logs/dumps/MSL. Не скрывать тело/шейдеры ради картинки.
- Перед каждым сравнительным запуском копировать только `Settings20261017084726/DATA` из `_SaveData.claude-20261103-2225`, затем `diff -rq` всего save. Не обходить блокировку rm-rf.
- Не трогать пользовательские gpudebug sessions. Каждый trace занимает десятки GB; закрывать свои после выгрузки.
- Ledger CLI в этом окружении отсутствует; AGENTS.md требует его, но доступен только старый .ledger. Активные сведения здесь и в свежем Claude handoff.

## База и пути

- `/Users/tx/Developer/KyTx`, branch `metal-native-checkpoint`, HEAD9534b337.
- 945d467d уже починил одежду: dispatcher fallback должен переходить по IndirectBranch/s_setpc_b64, а не return. Маски слоёв одежды теперь пишутся, результат Claude подтвердил дважды. Не начинать заново старую body-index/mask ветку.
- 9534b337 уже починил presenter/UI wobble: bilinear + drawable в пикселях.
- `/Users/tx/Developer/KyTx-vkref-src` — plain copy, НЕ git repo. Reference binary `/Users/tx/Developer/KyTx-vkref/kyty_vkref`.
- Mesa/Kosmic `/Users/tx/Developer/vkref-mesa`; прежние два opt-in/math/OOM patches не менять без причины.
- Игра `/Users/tx/Developer/PPSA34015`, рабочий каталог `/Users/tx/Developer/fex-spike/runs`.
- Все игровые логи `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean`.
- Рабочие файлы этой сессии `/Users/tx/Documents/Codex/2026-11-07/new-chat/work/vulkan-lighting` (далее WORK).
- Локальные переносимые patches/logs `/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-08-codex-lighting` (далее ART).

## 1. Неверная «первая NaN»: важная поправка к Claude handoff

Старый `pt_summary.py` считает любой nan= в строках, включая INPUTS и dispatch с нулём групп. Это НЕ локализует первый вычисленный NaN.

Новый `WORK/audit_outputs.py` смотрит только RT/OUT и отдельно учитывает zero-group kernels.

- vk9 flip725: оба NaN OUTPUT принадлежат1691cb86/15ef8445 с grp0x1x1. Адрес8246360000 перед этим использовался как R8Uint, затем как RGB11F. Нулевой dispatch не мог записать эти NaN; это переинтерпретация/старые неиспользованные пиксели.
- cvk1 flip537 аналогично.
- mc25 «первый a155861e» — NaN INPUT, не output. Первый NaN output — TAA draw467 RT1@824b000000 f92; до этого адрес был RGBA8. Может быть частично незаписанной областью, не доказано, что это видимый white-frame в Metal.
- История TAA всё ещё подозрительна, но нельзя брать первый nan= как автора порчи. `output-audit.txt`, `vk9-flip725.txt` в WORK.

PassTrace fN — **Metal MTLPixelFormat даже в Vulkan логах**, не VkFormat: f92RGB11F,115RGBA16F,65RG16F,25R16F,55R32F,53R32Uint,105RG32F,70RGBA8,71SRGB,90RGB10A2,13R8Uint,10R8Unorm,30RG8Unorm.

## 2. Что изменено во внешнем reference

Файл `KyTx-vkref-src/src/graphics/host_gpu/renderer/cache/textureCache.cpp`:

1. ColorMetaTrace больше не `thread_local` с повторным fopen(path,"w") из разных потоков. Общий journal + mutex в Write/Watch/Overlaps. Mutex не держится через GPU wait.
2. К F/R/W/P/T добавлены B (binding metadata decision), S (mixed/undecodable bytes), C (реальный clear и цвет). Только при `KYTY_COLOR_META_TRACE=<csv>`.
3. `KYTY_VK_TRACE_IMAGE_EXTENT=1`: FindImage пишет уникальные logical-vs-actual размеры sampled/storage view после разрешения кэша, с flip,address,type,base_mip,format,metadata. На default rendering не влияет.

Все reference builds успешно собирались. Текущий installed binary — cvk3 с обоими probes. Backups binary `kyty_vkref.before-cvk1`/`.before-cvk3` и source backups в WORK. Patch целиком `ART/vkref-texture-cache-diagnostics.patch` (от source до наших probes, без старых правок Claude).

Main repo `.porting/work/vk_timeline.sh` улучшен: single-file save restore+diff, guard также проверяет обычный kyty_emulator, footprint stop TERM вместо KILL. `ART/vk-timeline-guard-changes.patch`.

**Не включать частые PassTrace бездумно.** По умолчанию helper трогает pass каждые8s с10s; при двух кадрах трассы и0.2FPS получается практически непрерывный trace. Для live картины запускать `TRACE_FIRST=86400` и делать один touch pass вручную. В cvk1/2 helper временно SIGSTOPPED, pendingpass удалялся; при остановке возобновили helper, он восстановил env.

## 3. Запуски и выводы

| run | log | flags/результат |
|---|---|---|
| cvk1 | cvk1-022926.log | default UMA + metadata journal/profile. Холодная 3D-компиляция ~8min, затем18–20FPS. Black main-menu background наблюдался. После trace2633 сам вышел VK_ERROR_DEVICE_LOST, MasterSemaphore.cpp113, waiting98099/gpu98098. |
| cvk2 | cvk2-024005.log | тот же binary + KYTY_UMA_READBACK=0. Drain-path подтверждён логами. Белый экран визуально приframe2038. Значит UMA-off НЕ решение. White GPUcapture2761, stopped2762. Потом остановлен только нашPID84108. |
| cvk3 | cvk3-024938.log | extent probe, без metadata journal, без autoPT. На3266 визуально чистая England studio с правильной оконной тенью. Сохранилиcapture3439, но сцена к нему УЖЕ СМЕНИЛАСЬ: finalPNG доказывает **UK cobble street, белый RMA tracksuit**, НЕ студию. Чистый кадр, динамическая тень есть. После stopped3440 остановлен нашPID86326. |
| cmt1 | cmt1-025523.log | native до нового patch, KYTY_MSL_CMP_FUNC=ref. Чистый save, J1500/1850. RMA studio имеет регулярные цветные sparks и весь светлый пол; Paris street лучше освещена, но SAME-scene A/B нет. Nativecapture2538 (вероятноRMA studio, finalPNG пока НЕ проверен). Остановленнаш87913 ~5200. |
| cmt2 | cmt2-030153.log | rebuilt experiment с KYTY_MSL_CMP_FUNC=legacy, т.е. старая картинка. Остановлен по команде хендоффа на раннем3D, естьPPM1200/1400/1600/1800; позднюю студию НЕ успели сравнить. PID89550 закрыт. |

Дампы native PPM: `WORK/cmt1-frames` и `cmt2-frames`; contact sheets cmt1-contact/cmt1-last.png. Не считать4 дампа cmt2 полноценным контролем отсутствия sparks. В cmt1 белых кадров среди просмотренных дампов не было, но coloured sparks повторялись в студии.

Метаданные CSV cvk1 ~42MB. frame в journal = RenderContext::DiagnosticFrameNum(), **НЕ TraceWindow flip** (примерно половина); не склеивать напрямую с PassTrace. B/S/W/R/C/F событий много, clearcodes0/192/64; smoking gun не получен.

Native RegisterMetaClearRt replay — `KYTY_RT_META_CLEAR` opt-in, defaultOFF. Не портировать во Vulkan как «уже проверенный default fix».

MemoryTracker download расширяет на full16KiB tracked page и копирует все GPU-dirty byte ranges страницы. Простая гипотеза «materialize4KiB теряет соседниеGPUbytes16KiB» не доказана. `KYTY_COLOR_META_CPU_FILL=1` всё ещё упирается в guard4KiB/16KiB, guard не убирать.

## 4. Extent reuse: подтверждённый факт, не исправление

`ImageInfo::BlockExtent()` использует pitch, не logical width. ResolveOverlap может вернуть более широкий native backing для меньшего T# при одинаковых pitch/height/layout. Grow есть, shrink нет. RT reuse большей картинки специально проверен существующим `CheckUnifiedTextureCacheFlow`; нельзя наивно заменить всё на shrink/free: потеряются внешние GPU-written пиксели гостевой allocation.

cvk3 probe реально поймал:

- storage/sampled class tables25x15 vs26x15 или27x15;
- HDR sampled856x484 vs860x484;
- R32 sampled1792x1008 vs1784x1008,1680x944 vs1672x944,1768x992 vs1752x992;
- small mips67x38 vs68x38 и др.

Для sample normalizedUV и ImageQuerySize SPIRV использует физический backing size; rescaling в descriptors нет. Потенциальное чтение старой истории при DRS, но white causality **не доказана**. Следующий шаг — связать mismatch с конкретным реальным image read/write, не просто менять кэш. В source уже есть старые clock experiments `KYTY_EOP_CLOCK_FREEZE=1`, `KYTY_EOP_CLOCK_SCALE` default8; в этой сессии НЕ тестировались как DRS A/B.

## 5. Независимый writer-tick lead

`BufferCache::SynchronizeBufferFromImage` (reference textureCache.cpp около2515+) ставит DownloadImage→buffer, но не NoteWriterTick. Другие buffer writes отмечают writer tick. UMA может дождаться старого writer. Не доказано для white; UMA-off white остаётся.

Был подготовлен transfer-order regression `--image-readback-order-only`, target собрался, но harness abort ДО теста: нет fragment barycentrics и64-bit LDS/image atomics наMac. Это НЕ failing correctness regression. Наши testsource edits полностью отменены; reference tests source совпадает с backup. `WORK/add_readback_order_test.py`, test/build logs, `ShaderRecompilerComputeTests.before-readback-order.cpp` оставлены. Если продолжать lead — отдельный transfer-only harness, не ослаблять массово graphics requirements.

## 6. Shadow compare: отдельный GPU-дефект доказан, visual default НЕ принят

Нашли старые notes `.porting/porting-handoff-fc27-metal-2026-11-04-um222.md`, строки130+: вывод «CMP_FUNC серит красную комнату» был ошибочным, сравнивались разные команды. Там уже было: reference-first порядок правильный, но даёт sparks/bloom. Свежий Claude handoff это не включил.

Native default сейчас снова старый: `kyty_sample_cmp` всегда texel>=ref (func6), игнорирует S# DEPTH_COMPARE_FUNC. `KYTY_MSL_CMP_FUNC=ref` берёт sampler func, зеркалит comparison к texel-first helper, совпадает с reference (S#GREATER = ref>texel).

Эксперимент:

- Общий helper SampleCompareFunction для sample/gather; default сделали reference-first; `legacy` возвращал старый6, остальные режимы флага сохранялись.
- Добавили ShadowComparisons() в native GPU tests: 8func ×4ref(.25,.5,.75,NaN) ×4channels ×sample/gather =256 значений на реальной GPU, uniformdepth.5.
- BEFORE тест падает `shadow sample func=0 ref=0.25 expected=0 got=1`.
- AFTER256 проходят; весь `tools/test_metal_msl.sh` с MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 проходит; CMake emulator build проходит.
- Однако cmt1 с тем же ref-поведением в игре показывает coloured sparks и не восстанавливает оконную тень пола. Отдельный тест НЕ оправдывает включение default.

**Финально НАШ native patch ОТКАЧЕН** (до прежних dirty source). Он доступен `ART/shadow-comparison-experiment.patch`; применять только как явный эксперимент. Testbefore/after/build logs там же. Не говорить «освещение исправлено» и не оставлять этоdefault без новой визуальной валидации.

Текущий code всё ещё compare-after-filter на обычной float texture, без настоящего PCF. Возможный следующий lead; не был реализован. Не перепутать его с уже доказанным неправильным выбором function/order.

## 7. Captures и gpudebug: что точно есть

Все `/tmp/kytx_f9/gputrace`, могут исчезнуть после reboot:

- Старые vk_f1487 = Roma studio, vk_f1635 = Paris white tracksuit, frame2399 = native UKcobble dark, frame8492 = old broken native.
- **vk_f2761.gputrace** ~29GB: verified WHITE final.40CB/180encoders/682draw/537dispatch. TAA cb37/re7/draw0 color0/1 уже испорчены (preview noise). Finalpresent cb2/re0/draw0. Все inventories в WORK/white2761-*.json, finalPNG, TAA PNGs, MSL. TAA source fetched8695lines from fragment/sources/lib153-source. Own session219 закрыта.
- **vk_f3439.gputrace** ~32GB: verified clean **UK cobble**, белый RMA tracksuit.35CB/220encoders/1094draw/451dispatch. Не называть studio вопреки filenames `WORK/studio3439-*`! Own session226 закрыта.
- **frame2538.gputrace** ~25GB: native cmt1 CMP_FUNCref. Захват завершён, предполагаемRMA studio по соседнимPPM, final ещё НЕ извлекали. Проверить прежде сравнения.

gpudebug умеет `fetch /commands/.../drawN/color0` — attachment на момент draw. Generic `/resources/textures/texN` может быть END-frame/reused contents; нельзя приписывать их раннему pass. На3439 RG8 mask generic похожа на реальную shadow mask, а после её первого draw(cb23/re1) она жёлтая — это clear/ранний pass, не ошибка.

3439 полезные пути:

- final `/commands/cb2/re0/draw0/color0` → WORK/studio3439-final.png (cobble!).
- sun depth3072x3072 Depth16 tex11722; cb20/re0clear, re1draw41 иre3draw5. PNG показывает player+platform, не все здания; static environment shadow может идти отдельно, это лишь lead.
- depth2048 tex11640, cb20/re5clear/re6draw109.
- shadow mask1920x1080 RG8 tex10852, cb23/re1/re3/re4/re6/re7/re9 draws. Первыйcb23/re1 — uniformyellow. Need inspect later pass, first draw is not final mask.
- TAA cb32/re7/draw0 color0/1.
- `WORK/studio3439-{commands,encoders,operations,op-bindings}.json` + colors.txt — полный inventory.

Из mc33 старой native PassTrace (flip2500) RG8 mask: draw888 guest94d98648 clear,890 guestd71495d36fed5026,895 guestb04ad1f1eb010b31. RT@8249220000; после890 вся255, после895 mean0=226.33, floor probes остаютсяffff. Inputs записаны СЛЕДУЮЩИМИ строками `IN slot64...`, не какINr в той же строке. У d71495: Gbuffer@8244160000 RGBA8, depth@8242dd0000 D32,2048shadow@8249b20000 R16 array,128x128@81b3672000. Новый возможный путь локализации missing static shadows, не готовый вывод.

`WORK/scan_commands.py <session> <prefix>` быстро сканирует и сохраняет inventories без огромного вывода. Interactive prompts **gpudebug <path>>**, parser должен split regex, учитывать пустой go и list--all. IDs opaque Metal gpuResourceID не равны resourceIndex gpudebug. Indirect args generic buffer fetch — не обязательно dispatch-time snapshot!

## 8. Как продолжать

1. Прочитать этот файл и Claude handoff. Проверить что нетэмулятора; запуски только после новой команды пользователя продолжать. Не повторять закрытые A/B.
2. Выбрать конкретный путь: (a) native shadow-mask/lighting с сохранёнными кадрами либо (b) источник реальной порчи history во Vulkan. Избегать неделями расширять probes без реального pass comparison.
3. Для shadow: сначала открыть frame2538 и **проверить его final**. Если этоstudio, нужен тот же team/scene Vulkan — старыйvk_f1487Roma отличается отRMA; новый3439этоUKstreet. Для UK естьnativeframe2399 иVulkan3439, сравнить staticgeometry shadow mask, затем lighting inputs при совпадающих world pixels. Avatar/clothes/camera могут отличаться — не делать вывод по случайным screen pixels.
4. У root cause отделять исходнуюshadow term от exposure/TAA/tonemap. Цветные sparks отrefсравнения не устранять clamp/skip-shader/hide hacks.
5. После настоящего fix: meaningful GPU regression, native build, SAME-scene visual A/B, затем обычный запуск без diagnostic flags. Ничего не коммитить.

PS5 references на Desktop с2026-11-08 00.20.30 (Englandstudio),00.21.14Grounds,00.22.28redstreet,00.22.41UKcobble,00.23.11Paris. Online FCNEWS игнорировать. RAW BC6H environment PANORAMA/cloud PNGs в старыхVulkan/nativecaptures уже byte-identical; это не доказывает равенство всех dynamic probes/lighting inputs.
