# FC27 — исправлены пропадающие X в UI, 2026-10-25

Пользователь попросил через Xcode разобрать пустое место слева от текста кнопок EA Servers, отсутствие X в «Press X to Skip» и продолжать исправлять графику. **Эти значки X восстановлены и проверены в живой игре и новой GPU-трассе.** Причина — mip tail при загрузке небольших2D UI textures. Общая цель остаётся открытой: красный фон/3D и полное интро ещё некорректны, корректного матча30+FPS нет.

Только native ARM64 FEX + direct RDNA→MSL. Rosetta/Vulkan не запускались. Ветка `metal-native-checkpoint`, HEAD `c30d5d81ab23`, коммитов нет; предыдущие dirty/untracked изменения сохранить. Это продолжает [серии GPU capture](porting-followup-fc27-metal-2026-10-25-gputrace-series.md), [image sampling](porting-followup-fc27-metal-2026-10-24-image-sampling.md) и [исходного фикса логотипа через Xcode](porting-handoff-fc27-metal-2026-10-24.md).

## Как нашли

1. В старом `…/captures/fc27-20261024/frames500-1000-193356/frame500.gputrace` выбрали **API3457**, CLI **cb15/re75/draw0**, Xcode **CB16/render44**. TriangleStrip4 покрывает именно пустое место слева от «Cancel and Play Offline». Source **MTLTexture105** /@tex104, parent@tex103, **32×32 RGBA8, один mip**. Это не проблема выравнивания текста: игра уже оставляет место под значок.
2. В Xcode открыли source texture и пиксель16,16: RGB чёрный. Fetch bound texture также чёрный. UI output при этом не содержит X. В соседнем позднем draw была ещё пустая40×32 texture. PNG fetch по умолчанию скрывал реальный alpha (выводил alpha255); не интерпретировать его как исходную alpha.
3. Добавили opt-in bounded `KYTY_SMALL_TEX_DIR`: CPU `TryReadBacking` до загрузки texture, metadata/hash и не более64 небольших snapshots. В run020450 получили непустые64KiB guest blocks: button32×32 **4636** nonzero bytes, UI40×32 **5530**. Значит allocation с данными, а прежняя CPU распаковка читает не те координаты.
4. Из тех же байтов standalone decoder восстановил mip layout: старый `DetileToLinear` читает level0 из(0,0) и выдаёт полностью нулевой RGBA; для mip chain level0 расположен в **tail(64,0)**. Получили реальные UI masks: **3696 /4400** nonzero decoded bytes. Реальные T# из повторного запуска: **base0 /last1 /max1 =2 mips**, format56, tile9. Width/height не дают право считать texture одноуровневой.

## Что исправили

- `metalTiling.h/.mm`: `DecodeTiledTextureLevel` принимает полный `TileSurfaceLayout`, учитывает reversed mip offsets, mip-tail coordinates, block XOR, array slice и BC addressable elements; проверяет source bounds/level/layer. Это CPU upload helper поверх общего tile.cpp layout, без Vulkan.
- `metalRenderer.mm::ComputeGuestImage` передаёт `max(LastLevel,MaxMip)+1` в `GuestTexture`.
- `GuestTexture` для tiled multi-mip2D assets создаёт native texture со всеми объявленными уровнями, читает guest allocation через `TryReadBacking`, распаковывает и загружает каждый уровень. Cache match учитывает actual mip count. Линейный/одноуровневый путь сохранён; synthetic X, сдвиг текста и замена цветов не вводились.
- Diagnostic `KYTY_SMALL_TEX_DIR`/`[SmallTexDesc]`/`[TexMip]` позволяет повторить source evidence. По умолчанию CPU snapshot probe выключен. Внешний MRT launcher теперь принимает опциональные dump frame/path и small-tex directory; старые defaults500…1000 сохранены.

Фокусированный [patch](artifacts/2026-10-25/ui-mip-tail/mip-tail-fix.patch) отделяет только эти изменения от прежнего большого dirty tree. `metalTiling.{h,mm}` и tests уже были untracked — не считать их мусором. Это не полный аудит asset cache/CPU invalidation/RT ownership. Графические BaseLevel views, linear mip chains и прочие прежние ограничения не объявлены решёнными.

## Проверка

- ARM64 emulator build успешен; `git diff --check` проходит.
- **Metal API validation:** прежние **58 tiling GPU cases/0fail**, новые **8 mip chains/0fail**. Decoder сравнивается с независимым GPU tile export, включая R8/RGBA8/RGBA32Uint/BC1, base level в tail и отдельный base перед tail, все объявленные mip levels; malformed source/level/layer отвергаются.
- [CPU repro](artifacts/2026-10-25/ui-mip-tail/repro.sh) повторяет на точных игровых байтах: old zero → реальные masks. Архив содержит source blocks, decoder и expected outputs.
- Fixed run **mrt-gui-1-021054 /PID23467**: `after-500.png` показывает **Press ⓧ to Skip**, `after-600.png` — **ⓧ Cancel and Play Offline**, `after-900.png` — actual EA Servers unavailable + **ⓧ OK**. После J/Cross также проверена **ⓧ Let's Play!**. [Сравнение](artifacts/2026-10-25/ui-mip-tail/icon-before-after.png), [окно ошибки](artifacts/2026-10-25/ui-mip-tail/after-900.png). Числа кадров между запусками фазу не синхронизируют.
- На fixed run до stats2700 нет logged GPU errors/PSO failures; затем CLI Layer/count1 capture оборвался **0/1, process crashed or disconnected**, usable file отсутствует. Это отдельная проблема захвата, не доказанный GPU Hang в runtime. Сохранён error log; причина не установлена.

### Новая контрольная трасса после фикса

`/Users/tx/Developer/fex-spike/runs/captures/fc27-20261025/ui-mip-tail/frame500.gputrace`, **~4.5GiB,16CB/499encoders/189draw/142dispatch**. Run **mrt-gui-1-021354 /PID23614**. Programmatic start/stop500 подтверждены. Снята под прежним LLDB FEX signal filter (ignored EXC_BAD_ACCESS до attach, SEGV/ILL/USR pass/no-stop, SIGBUS stop hook пропускает только FEX JIT PC0x700…0x900). Это диагностический фильтр, не исправление capture fault.

CLI GPU ready и output fetch успешны. **Xcode Replay через CUA успешен:**16CB/189render/154compute/156blit/189draw/142dispatch. В Xcode выбрали **API3727**, CLI **cb14/re92/draw0**, Xcode **CB15/render48**. Source **MTLTexture108 /@tex107, parent@tex106**,32×32/**2 mips**. В viewer включили **Filters → Alpha checkbox**, видны circle/X masks и прозрачность в обоих mip thumbnails. Attachment показывает корректный X. ROI805,592..837,624: **0 dark pixels до →494 после**. Не смешивать старый/новый frame500: одинаковое имя, разные абсолютные пути и API IDs.

## Что осталось

После принятия EA error и Let's Play тяжёлая сцена всё ещё красная. Ни root cause красного CPU vertex color990000ff, ни scene layout corruption этим mip fix не доказаны и не исправлены. Полный EA Sports video intro не принят. В живом окне также замечено временами неполное отображение конца текста EA, тогда как PPM фиксирует полную строку; причина отдельно не установлена, не выдавать за исправление шрифтов/синхронизации. Не форсить Clear/другой цвет и не возвращать broad256KB snapshots/latest-writer experiments.

Продолжать по целевому CPU-side probe nativeVSbc0210100fe76732 /resource2 из предыдущего image follow-up: сопоставить guest V#, `TryReadBacking` до SnapshotBytes и bytes в ring; отдельно исследовать scene writer/layout. Shader source и guest data — доказательства, визуально удобная подмена цвета не является исправлением. Не считать FPS этих кадров результатом правильного матча.

## Артефакты / процессы

[Компактный архив](artifacts/2026-10-25/ui-mip-tail/): patch, build fingerprints/logs, GPU tests, CPU repro, guest64KiB blocks, before/after PNG, binding/API evidence, metadata/keylines трёх запусков. Full logs/PPMs — внешние `runs/validation-20261024/` и `runs/validation-20261025/ui-icons/`. Huge GPU traces в repo не копировались.

Эмулятор/LLDB/gpudebug завершены. Xcode оставлен с новой контрольной трассой, active workload API3727, слева texture108 с обоими mips/alpha, справа color/depth attachments (~7.84GiB). Ledger CLI отсутствует; markdown state обновлён.
