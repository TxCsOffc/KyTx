# FC 27 / Metal: разбор Xcode GPU traces, 2026-11-06

Новые исправления эмулятора не внесены. По небу установлен конкретный проход и прослежена цепочка от больших значений текстуры искажения до смещённых UV панорамы. Окончательный источник этих значений и причины остальных пяти дефектов **не найдены**. Ни один визуальный дефект не объявлен исправленным.

## База и воспроизводимость

- Репозиторий `/Users/tx/Developer/KyTx`, ветка `metal-native-checkpoint`, HEAD `878ee277f560f941c413e3123a411354b723805b`. Исходное изменённое рабочее дерево сохранено; коммитов и push нет. Сборка, codesign, Vulkan/Rosetta, SPIRV-Cross не затрагивались.
- Трассы находятся в `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261106/`. Из старых подробно открыты `frame978` и `frame1471`; `frame1637` и `frame1997` не прошли отдельный полный разбор в этой сессии.
- Для новой проверки запущен один экземпляр `cx608`, PID 18687, через `.porting/work/run1.sh cx608 3600`. Пользователь снял `frame1458`, `frame1778`, `frame2489`; затем наш процесс остановлен SIGTERM. Проверка `pgrep -x kyty_emulator_s3` после исследования не нашла процесса.
- Лог запуска: `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/cx608-152721.log`. Начало/конец capture: 1458 — строки 6679/6686; 1778 — 8163/8171; 2489 — 11582/11591. Пользователь подтвердил: **frame1778 — Бавария, небо**.
- Env `.porting/work/extra_env_s1.sh` содержит `RUN_TAG=cx608`, capture/dump triggers, `KYTY_LOG_DEPTH_ARRAY_READ=1`, `KYTY_LOG_BC_WRITEBACK=1`; прежний env — `.porting/work/xcode-review/extra_env_s1.before.sh`.
- PPM этих трёх захватов не записались из-за отсутствовавшего каталога `cx608`: строки лога 6675, 8159, 11577. Это ошибка подготовки данного запуска; каталог затем создан. Показанные пользователю превью были выгрузками альбедо из GPU replay. Остатки другой сцены в G-buffer **не доказывают переход сцен**: возможны загруженное старое содержимое вложения и повторное использование памяти.
- `ledger` недоступен (`command not found`); итог добавлен в `.porting/STATE.md`. Исторические хендоффы ниже помечены отдельно от новых измерений.

### Как читать ссылки и номера

Все новые данные — `.porting/artifacts/2026-11-06/codex/`. `session21-*` относится к frame978; `session29-*` — 1778; `session32-*` — 2489; `session38-*` — 1471; 26/35 — 1458. ID сессий временные, имена трасс и API-команд устойчивые. CLI `cbN`, `drawN`, `dispN` нумеруются с нуля; Xcode `MTLCommandBuffer` — с единицы. Здесь draw/dispatch всегда относится к указанному encoder.

Деревья команд, API-ссылки и соответствия исходников сохранены в `session*-commands.json`, `session*-calls.json`, `session*-shader-map.json`; соответствие хэшам проверялось сравнением содержимого MSL с локальными dump-файлами. Полные CLI-команды и ответы — `session*-evidence.txt`. Инструменты воспроизведения — `.porting/work/xcode-review/{gpu.py,map_trace.py,enrich.py,shader_map.py,scan_float_buffers.py}`.

PNG float/depth не заменяет численные данные: тональное преобразование и квантование скрывают диапазон/NaN. Для неба и одежды ниже использован именно Xcode Pixel Debugger. Для блюра сканировались только известные half-float texels в BIN, без padding и без интерпретации packed integer buffers как float.

## 1. Тени

**Факты — новая трасса frame2489.**

- Маска `MTLTexture 43` / @tex42: RG8Unorm, 1920×1080. Каскадные проходы находятся в **CB42**, encoders **39/40/41/42** (`/commands/cb41/re0,re2,re4,re6`). PS `16dd63273cf03654` найден в re0/draw2, re2/draw0/1, re4/draw0/1. Источники: [карта проходов](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/session32-passes.txt), [карта шейдеров](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/session32-shader-map.json).
- В re4/draw1 привязана `MTLTexture 572` / @tex571: **Depth16Unorm, private, 2DArray, 2048², arrayLength=1**. Родитель @tex570 — array view, его родитель @tex569 — тот же depth allocation. Следовательно, новый путь однослойного depth read действительно присутствует в новой трассе. [Метаданные и привязки](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/session32-evidence.txt).
- Там же привязаны `MTLTexture 579` / @tex578 и @tex576: **R16Unorm, shared, однослойные array views** общего allocation 203498843. Родитель @tex572 имеет arrayLength=3 и 12 mip levels; @tex578 относится к slice1. Выгрузка [slice1](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame2489-r16-array-slice1.png) в PNG целиком чёрная. Пока не сопоставлен точный shader slot, это **не доказательство отсутствующего depth-каскада**: shader читает массивы и для иных данных.
- [Маска после re6/draw0, api17535](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame2489-shadow-mask.png) не постоянна: диапазоны R/G 0…255; ненулевых texels R=1 011 676, G=168 260. На ней различимы тени объектов и область у ног игрока. Это подтверждает работу маски в данном кадре, но не полноту всех теней или правильность их силы.
- Тайловое освещение `CS 028077f24b695ca4`, guest `5936aa1834648dea`: **CB50 / compute encoder219 / dispatch0, api18429**, `/commands/cb49/ce5/disp0`. В связях есть @tex356 — **private Depth16Unorm 3072²** и его view @tex357. Гипотеза «атлас 3072 всегда читается из пустой копии guest RAM» этой привязкой не подтверждается.
- Stencil volume re0/draw1: `MTLDepthStencilState 4`, depth GreaterEqual/no write; stencil Always, Keep/Keep/Replace. Shadow state `MTLDepthStencilState 5`: GreaterEqual/no write, stencil Equal, Keep/Keep/Keep. Это Metal state; соответствие каждому guest `stop/opv` отдельно ещё не доказано.

**Причина остаточных теней: не найдена.** Существующий `DepthWriterForLayeredRead` (`src/graphics/host_gpu/metal/metalRenderer.mm:1559`) ограничен base slice0, depth0, совпадением размеров и writer не старше двух flips. Само ограничение не доказывает текущий дефект.

**Предложенное исправление:** нового обоснованного исправления нет. Для многослойного случая сначала сопоставить `image_2/3/4` с конкретными views и писателями каждого slice. Проверка stencil относится к `ConvertGuestStencilFace` (`src/graphics/host_gpu/metal/metalPipeline.mm:274`); заменять операции по виду картинки оснований нет.

## 2. Небо — frame1778, Бавария

**Установленный проход.** До неба, **CB59 / encoder56 / draw1, api17283** (`/commands/cb58/re1/draw1`), небо чёрное. В **CB60 / encoder61 / draw0, api17942** (`/commands/cb59/re11/draw0`) появляются облака и горизонтальные разрывы. Pipeline: VS `4666ab97d3eb3ad9`, PS **`1317481e3a1daeff`**, guest PS **`05e3ea8dcdc67f2d`**. Target `MTLTexture 51` / @tex50 — RG11B10Float 1920×1080, depth GreaterEqual без записи. Проверено в окне Xcode: [до](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-before-sky-api17283.png), [после](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-after-sky-api17942.png). Содержимое после следующего draw api17963 совпало побайтово с выгрузкой после api17942; полосы уже есть до него.

**Привязки подтверждены аргументами Pixel Debugger**, а не порядком списка resident textures:

| Аргумент | Ресурс в frame1778 | Наблюдение |
|---|---|---|
| image_4 / image_5 | MTLTexture230/232, BC6H_RGBUfloat, 512² | Две непрерывные серые облачные текстуры: [первая](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-sky-bc6-229.png), [вторая](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-sky-bc6-231.png) |
| **image_7** | **MTLTexture180 / @tex179**, BC6H_RGBUfloat, **256×512**, shared | [Пёстрая текстура](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-sky-bc6-179.png), используемая для искажения координат панорамы; native sample даёт большие значения |
| image_9 | MTLTexture157 / @tex156, BC6H_RGBUfloat, **4096×1024** | [Цельная панорама неба](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-sky-source-bc6.png), без прямоугольных полос |
| image_11 | MTLTexture677, R16Float 1920×1080, view R16Uint allocation | Записываемый ресурс; не выдаётся здесь за обычный float color |
| image_12 | MTLTexture64, RG11B10Float 960×540 | Screen-space промежуточная картинка; в проверенной точке sample RGB=(0,0,0) |

Источник привязок: [полный AX с аргументами](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-sky-noise7-pixel.txt). У parent texture179 и panorama156 один mip level (в `info --all` многомиповость отсутствует); панорама и image_7 читаются с явным `level(0)`. Текстуры облаков image_4/5 используют implicit sampling. Исторический лог `user6-045829.log:84145–84159` подтверждает тот же shader и T# image_7: 256×512, guest format179/Metal151, tile5, lv1/1; адрес из **того запуска** — 0x81ece3a000, не адрес cx608.

**Численная цепочка из Xcode, пиксель (800,100), lane0, primitive1:**

1. В [MSL из трассы](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/session29-lib114-source.metal:4758) `image_7.sample` возвращает `v1898.x = 1109966848`, то есть **42.1875**. [Измерение](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-sky-noise7-800-100.txt).
2. `v1940 = sample.x × v1576 ≈ 42.1875 × 0.380437 ≈ 16.04968`. Затем `v1951 = v1700 × v1940 × 4 ≈ -16.057`. [Произведение](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-sky-noise-product-800-100.txt), [искажение](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-sky-distortion-800-100.txt).
3. Вертикальная координата `image_9` становится `fma(-0.0327655, -16.05686, ≈0.5463) ≈ **1.072433**`; до добавки была ≈0.546. Горизонтальная координата ≈1.039566. [UV и последующая выборка](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-sky-panorama-uv-800-100.txt), [скриншот](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-sky-distortion-800-100.png). Точное sampler address mode здесь не измерено: нельзя автоматически называть это clamp или repeat.
4. Контрольная точка (1000,100): `image_7.x ≈0.895`; выборка image_9 RGB=(0.1755371,0.2526855,0.3776855). Итоговый PS RGB=(1.13671875,1.5947265625,2.37109375), alpha=0.70263671875 после half packing. Target BIN в (1000,100): packed `0x815f33c8`, RGB=(1.125,1.59375,2.3125), отличие укладывается в шаги packed float. [Вход](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-sky-panorama9.txt), [выход](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1778-sky-output.txt).

**Причина:** локализован механизм сильного искажения — большие значения **image_7** изменяют UV цельной панорамы. Первопричина таких значений **не найдена**. Без правильных исходных BC-блоков нельзя доказанно выбрать между повреждённым содержимым, неправильным layout/дескриптором/кэшем и ожидаемыми HDR-данными плюс ошибкой дальнейшей арифметики. Сам факт BC6H допускает значения больше1. PNG alone не доказывает повреждение формата. Наличие `[BcWritebackDesc]` в cx608 не связывает его с этим конкретным allocation.

**Предложенное исправление:** пока нет безопасного patch. Следующая точечная проверка — raw guest blocks → `DetileToLinear` (`metalRenderer.mm:5019`) → `GuestTexture` (`:5169`) → captured texture180; для cache path — `GuestTexStillCurrent` (`:4931`) и `GraphicsImageFor` (`:1454`). Исправлять конкретный доказанный этап загрузки/привязки; ограничивать sample до1 или UV до[0,1] без эталона нельзя.

## 3. Одежда: кожа сквозь джинсы

**Прямые измерения, frame978.** Тело: **CB18 / encoder12 / draw0, api2210**, VS `55ef8545cc2fb697`, PS `0e1e278ea3de8c83`, 60516 indices. Джинсы: **CB18 / encoder13 / draw5, api2491** и draw6/api2517, VS `7d6909aa397cc97b`, PS `fb92236b77d98c39`, 29976/41820 indices. [MSL-сопоставление](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/session21-shader-map.json), [CLI states](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/session21-evidence.txt).

У тела и джинсов `MTLDepthStencilState 13`: GreaterEqual, depth write yes; stencil GreaterEqual/read mask0x08, Keep/Keep/Replace, write mask0xfe, front/back ref37. В точке **(1355,625)** Xcode Pixel Debugger запускается для обоих draw:

- Тело: primitive12206, lane3, `frag_coord=(1355.5,625.5,**0.0231993776**,0.2320427001)`, world `(-604.8077392578,0.6693674922,601.2308349609)`.
- Джинсы draw5: primitive9965, lane3, world `(-604.8049926758,0.6695151329,601.2286376953)`. Этот PS **не объявляет frag_coord**, поэтому точной глубины джинсов из входов не получено. Его исходник не содержит активного discard (`if (!(true)) discard_fragment()`), depth output отсутствует.

Сохранены [тело](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame978-body-pixel-1355-625.png), [джинсы](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame978-jeans-pixel-1355-625.png) и одноимённые `.txt`; [альбедо](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame978-cb18-re13-draw314-albedo.png) показывает кожу уже в G-buffer. Попытка vertex debugging закончилась ошибкой Xcode **Failed to set up render pipeline state for debugging**: [состояние](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame978-vs-debug-failure.txt).

**Причина отличия от PS5: не найдена.** Наличие обоих фрагментов исключает полное отсутствие геометрии джинсов именно в этой точке. Полная Pixel History с результатами depth/stencil tests и точной глубиной джинсов **не получена**; Pixel Debugger входов не заменяет её. Историческая геометрическая оценка выступания тела до4см находится в `HANDOFF-claude-2026-11-06-skin-vulkan.md`; это отдельное измерение прежней сессии.

**Предложенное исправление:** нет обоснования для bias, глобального скрытия тела или замены белой маски. Нужны обе clip depths и фактический stencil result. Проверять `ConvertGuestStencilFace` (`metalPipeline.mm:274`) и stage resource binding в `metalRenderer.mm` только после обнаружения расхождения; конкретная ошибка этих функций здесь не доказана.

## 4. Блюр: velocity, TAA, DoF

**Frame978, найденные проходы:** TAA `CS b64d47e635bfdd4b`, guest `a418c52ef975c7cd`, **CB89 / compute encoder387 / dispatch0, api27281**, `/commands/cb88/ce0/disp0`. Motion blur PS `879789662fde6eb4`: **CB98 / encoder70 / draw0, api27605**, `/commands/cb97/re0/draw0`. Начальный четырёхтаргетный DoF проход: **CB99 / encoder71 / draw0, api27638**, `/commands/cb98/re0/draw0`. [Дерево](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/session21-commands.json), [привязки/выгрузка](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/session21-evidence.txt).

В motion pass привязаны RG16Float 1920×1080 @tex42 и @tex55, private Depth32Float @tex717 (parent @tex716), RGBA16Float tile texture @tex8990 размером64×36 и scene input @tex35. TAA использует depth @tex344 и velocity @tex41, а также history textures. Указание формата — не доказательство назначения каждого канала.

Численная проверка buffer-backed ресурсов: row pitch7680 у screen targets,512 у tile map; padding allocation исключён. [Полный результат](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame978-float-buffer-scan.json):

| Ресурс / backing | Объём texels | NaN / Inf | Диапазон |
|---|---:|---:|---|
| @tex41 RG16F / buf136 ColorRT22 | 1920×1080×2 half | 0 / 0 | −0.010994…0.046417 |
| @tex54 RG16F / buf144 ColorRT29 | 1920×1080×2 half | 0 / 0 | R:0.000172…7.83594; G:1.8252…20000 |
| @tex8990 RGBA16F / buf2539 ColorRT932 | 64×36×4 half | 0 / 0 | −3.75391…7.83203 |
| DoF @tex56 RGBA16F / buf145 ColorRT30 | 960×540×4 half | 0 / 0 | 0…0.323486 |
| DoF @tex8991 RGBA16F / buf2540 ColorRT933 | 960×540×4 half | 0 / 0 | 0…0.124634 |

Второй RG16F нельзя считать двумя обычными velocity components: максимум20000 сосредоточен в G; смысл канала ещё не выведен из shader. Это не доказанный мусор. Эти пять снимков не подтверждают отсутствие NaN во всей цепочке, всех mip levels или других кадрах. Private depth численно целиком не просканирован; PNG глубины для этого непригоден. Decode PS guest168b49445ed113a4 отдельно не прошёл численную проверку.

**Причина мыла: не найдена.** В проверенных half-float снимках исторический NaN-дефект DoF не воспроизведён. **Предложенное исправление:** нет; сначала сравнить velocity/depth/history в одном пикселе до/после TAA и motion. Точки проверки — `GraphicsImageFor` (`metalRenderer.mm:1454`), `LastGpuWriterTexture` (`:852`), TAA constants (`:1171`). Отключение эффекта само по себе не исправление эмуляции.

## 5. Волосы: светлые пряди

**Факты, frame978.** Raster guest `c29caf0c1cb0ad94`, Metal `CS 30caea79425a62b0`: **CB75 / compute encoder370 / dispatch0, api25473**, `/commands/cb74/ce0/disp0`. В связях @tex709 (`MTLTexture710`) — RGBA16Float 1920×1080, @tex32 (`MTLTexture33`) — R32Float 1920×1080; depth @tex344 — Depth32Float. Предыдущий `CS 8e8a969d1cce419e` находится в CB59, ce0/ce1 dispatch0. [Дерево](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/session21-commands.json), [связи](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/session21-evidence.txt).

[Извлечённый raster MSL](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame978-hair-raster.metal) совпал с локальным dump 30caea79425a62b0. Строки16662–16668: `image_1` получает `kyty_d16_unpack(uint4(v194,v193,…),15,true)` под guard **v222**. Phi для v193/v194 — строки16510–16511,16544–16545; текущие вычисления v3346/v3365 —16378/16416. То есть цвет надо прослеживать до этих значений, а не брать из тёмного G-buffer mesh hair.

b9 читает half4 (10934/10952); b10/b11 являются упакованными scratch lists с raw uint stores/loads (11165,11457,12445,12475). Их весь диапазон нельзя сканировать как float3 или объявлять альбедо. R32Float output содержит packed payload; NaN-биты сами по себе не доказывают ошибку.

**Причина цвета: не найдена.** Ограничение replay — CPU reset GDS вне capture, приводящий к пустой очереди — описано в `HANDOFF-codex-clothes-hair-2026-11-06.md:137–175` и исходном задании. Нового успешного replay активной очереди либо живого color probe в этой сессии нет. Чистую/пустую replay texture нельзя выдавать за правильные волосы в игре.

**Предложенное исправление:** нет обоснования для затемнения цвета или переноса цвета mesh hair. Следующая измеряемая проверка — активные v222/верхняя lane вместе с v3346/v3365, b10/b11 provenance и входом 8e8a969d. Для живых значений уже есть `kyty_value_dbg`, `EmitAssignment` (`MslEmitter.cpp:1775`), выбор `KYTY_MSL_VALUE_TRACE` (`:2825`). Сначала требуется активный guard; sentinel/неисполненное присваивание не результат цвета.

## 6. Тело: треугольные неровности футболки/шорт

**Новая привязка к конкретным draw, frame1471.** **CB24 / encoder17**, `/commands/cb23/re0`:

| Одежда | Draw / API | VS / PS | Индексы |
|---|---|---|---:|
| Шорты | draw2 / api3448 | 0e9a1f1fffa93072 / 4ffd0efb0af30287 | 10728 |
| Футболка | draw3 / api3490 | 30dc113af0b25d92 / 2a6bc904d76a590e | 24858 |

Шорты появляются между draw0/1 и draw2, футболка — между draw2 и draw3; это подтверждено последовательными albedo attachments: [шорты](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1471-player-draw2-albedo.png), [футболка](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/frame1471-player-draw3-albedo.png). Оба draw: CullNone, front/back stencil ref37, MTLDepthStencilState13. Albedo `MTLTexture40` — RGBA8Unorm_sRGB; normal target `MTLTexture37` — RGB10A2Unorm; depth `MTLTexture610` — private Depth32Float. [MSL-карта](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/session38-shader-map.json), [draw state](/Users/tx/Developer/KyTx/.porting/artifacts/2026-11-06/codex/session38-evidence.txt).

**Причина текущих треугольников: не найдена.** Привязка к mesh установлена, но нечисловая конкретная вершина/нормаль в этом capture не доказана. Normal PNG содержит загруженное содержимое вне текущего mesh; это не полный скан позиций. Исторические NaN и исправление перекрывающихся wrappers описаны в `HANDOFF-codex-clothes-hair-2026-11-06.md:208–218`: там после устранения основного сбоя оставалось около0.2% NaN. Нельзя приписывать текущую картинку этому остатку без сопоставления vertex stream и индексов.

**Предложенное исправление:** нового patch нет. Следующая проверка — индексы именно api3448/api3490 → typed position/normal stream → конечные значения и writer2c861; затем solver inputs. Точки — `MetalBufferManager::GetBufferForGuestRange` (`metalBuffer.mm:259`) и `ProbeSkinUpload` (`metalRenderer.mm:162`); повторять прежнюю правку wrappers без нового mismatch оснований нет.

## Что ещё не проверено

1. **Небо, приоритет:** источник больших значений image_7: исходные BC6-блоки, descriptor/layout, detile, cache freshness, sampler state. Нужен корректный исходник или независимый decode одного и того же блока. BC writeback пока не связан с этим ресурсом.
2. **Тени:** точные image slots R16 array views, писатели всех slice; контакты, дальние каскады и соответствие guest stop/opv Metal stencil. Наличие многоcлойного R16 allocation не доказывает многослойный depth-read дефект.
3. **Кожа:** полная history выбранного пикселя, точная глубина джинсов, stencil before/after, эталон скрытия тела на PS5. Vulkan не принят за окончательный эталон.
4. **Блюр:** численная private depth, temporal history и decode PS; движение между последовательными кадрами, остальные DoF stages. Один конечный snapshot не устанавливает temporal correctness.
5. **Волосы:** значения реальной активной очереди GDS и upstream color/material data. b9/scratch не объявлены цветом на основании формата.
6. **Треугольники:** typed vertices и normals конкретных shorts/shirt draw, их compute provenance; NaN в padding/packed payload не считается дефектом координат.

## Изменения и проверка

Созданы отчёт, карты трасс, диагностические Python-скрипты, численные выгрузки и скриншоты. Исходники эмулятора этой сессией не изменялись, поэтому новый build/test suite не запускался. Float scan воспроизводим `python3 .porting/work/xcode-review/scan_float_buffers.py`; он завершился без ошибок. Пользовательские три захвата — один запуск, **не три валидационных прогона исправления**. Все семь GPU traces сохранены. Собственные CLI replay sessions после выгрузки закрыты; окно Xcode оставлено на frame1778/api17942.
