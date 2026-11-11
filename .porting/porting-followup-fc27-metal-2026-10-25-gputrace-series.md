# FC27 — шесть отдельных GPU-трасс 500…1000, проверка 2026-10-25

Пользователь уточнил: под «снимками кадров 500,600,…1000» имелись в виду **Xcode `.gputrace`**, а не PNG. Захваты сняты 2026-10-24; 2026-10-25 закончена проверка всех шести. Основная задача исправления графики остаётся открытой. FEX + direct RDNA→MSL, без Rosetta/Vulkan renderer; коммитов нет, dirty tree сохранён.

Предыдущие исправления и доказательства: [image sampling / red draw](porting-followup-fc27-metal-2026-10-24-image-sampling.md), [подробный фикс логотипа через Xcode](porting-handoff-fc27-metal-2026-10-24.md). Их не откатывать. В этом продолжении менялось управление захватом, а не shaders/rendering.

## Файлы и проверка

Каталог: `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261024/frames500-1000-193356/`.

| Файл | GiB на диске | CB | Encoders | Draw | Dispatch | Проверка |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| frame500.gputrace | 4.00 | 19 | 476 | 185 | 128 | GPU ready + color0 fetch |
| frame600.gputrace | 8.39 | 18 | 572 | 422 | 193 | GPU ready + color0 fetch |
| frame700.gputrace | 9.60 | 22 | 952 | 752 | 370 | GPU ready + color0 fetch |
| frame800.gputrace | 13.38 | 23 | 858 | 814 | 286 | GPU ready + color0 fetch |
| frame900.gputrace | 10.49 | 24 | 989 | 715 | 405 | GPU ready + color0 fetch |
| frame1000.gputrace | 13.37 | 22 | 838 | 864 | 267 | GPU ready + color0 fetch; Xcode Replay |

Каждый документ охватывает отдельный интервал native flip N−1…N: `startCapture`, затем flush/wait и `stopCapture`. Между целевыми кадрами capture выключен. Это не непрерывный захват 500…1000. Номер логического flip не гарантирует одинаковую фазу игры между запусками и не обязан совпадать с PPM/present счётчиком. Не называть извлечённые color0 финальным display drawable без проверки attachment: только frame500 в этой проверке выбран именно из CAMetalLayer drawable; остальные — из последнего найденного render encoder с color0.

Для каждого файла создан отдельный persistent `gpudebug` session, дождались `Replayer: ready`, извлекли color0 в PNG и полностью прочитали его через Pillow. Все sessions завершены. Внешний `validation.json` содержит attachment paths, разрешения, workload counts, размеры и run tags; `frameN-validation.log` — точные команды/ответы. Проверка валидности трассы не означает исправления картинки: красная сцена сохраняется.

### Почему 1000 из другого запуска

500…900 сняты в `mrt-gui-1-193433`, PID14876. Все пять имеют `[GpuCapture] stopped at flip N`. Во время первой записи 1000 процесс завершился; `stopped 1000` отсутствовал, gpudebug выдавал `cannot open trace: Assertion failed: archive != ((void*)0)`. Неполный файл сохранён в `failed/frame1000-incomplete-first-run.gputrace`, ошибка рядом `failed/frame1000-incomplete-first-run-check.log`. Не использовать его для анализа.

Переснимали только 1000 в `mrt-gui-1-193916`, PID14982: start/stop 1000 подтверждены логом, затем игра продолжила работу до stats1750. Текущий `frame1000.gputrace` — этот успешный повтор. Он не представляет продолжение первого запуска. Причина завершения первого процесса не установлена; не объявлять её GPU Hang/FEX fault без evidence.

## Проверка Xcode через computer use

2026-10-25 через CUA открыли точный `…/frames500-1000-193356/frame1000.gputrace`, нажали Replay. Xcode 27.0 показал **Frame ready**, Summary: **22 command buffers /286 render /335 compute /217 blit /864 draw /267 dispatch**. Скриншот Summary визуально показывает красный кадр, Resolution1280×720 BGRA8Unorm. Это подтверждает GUI replay именно текущего успешного retry1000. Для остальных пяти в этом продолжении проверен CLI replay/fetch; GUI replay всех шести не утверждать.

Xcode остался открыт с активным workload frame1000, Summary, ~16.38GiB captured memory. В навигаторе есть RuntimeIssue на `IndexBuffer newBufferWithBytesNoCopy`, Summary показывает193 API Usage insights; их причины ещё не разобраны. Это не помешало replay и не является доказательством повреждения документа.

## Управление захватом и сборка

`src/graphics/host_gpu/metal/metalContext.mm::GpuCaptureTick` теперь принимает `KYTY_GPU_CAPTURE=500,600,700,800,900,1000`: parse/sort/unique targets, отдельный start/stop каждого, лог пропущенных/неудачных целей. Одиночное число сохраняет прежнее поведение. Перед stop pending CB commit/wait сохранён. ARM64 `kyty_emulator` успешно пересобран, `git diff --check` проходит. Артефакты: [capture-control.patch](artifacts/2026-10-25/gputrace-series/capture-control.patch), [build log](artifacts/2026-10-25/gputrace-series/build-multi-frame-capture.log), [SHA256](artifacts/2026-10-25/gputrace-series/build-fingerprints.txt).

Внешний `runs/validation-20261024/mrt-gui-launch.py` передаёт опциональные `KYTY_GPU_CAPTURE` и `KYTY_GPU_CAPTURE_DIR`; при capture отключает `KYTY_DUMP_FRAME`, чтобы не смешивать с PNG. Без capture обычный launcher по-прежнему настроен на PNG500…1000 после раннего неверного понимания запроса.

Пример для следующего запуска — **новый пустой output directory**, существующие трассы не перезаписывать:

```bash
open -a /Users/tx/Developer/fex-spike/runs/KyTxDiagnostic.app \
  --env KYTY_MRT_COMPARE_MODE=1 \
  --env KYTY_GPU_CAPTURE=500,600,700,800,900,1000 \
  --env KYTY_GPU_CAPTURE_DIR=/absolute/new-empty-capture-directory
# После создания Metal device, до первого захвата:
gpucapture config --pid ACTUAL_PID --shared-mem-opt disabled
```

В этих запусках `shared-mem-opt disabled` применялся к процессу до frame500/1000. Первый запуск был под LLDB с прежним FEX JIT signal filtering, retry1000 — без LLDB. Оба metadata JSON содержат полные диагностические flags: native metal, MRT1, validation/MSL logging и т.д. Это не performance baseline.

## Артефакты и следующий шаг

Компактная копия metadata, validation logs, capture start/stop logs, launcher, patch и build fingerprints: [artifacts/2026-10-25/gputrace-series/](artifacts/2026-10-25/gputrace-series/). Огромные `.gputrace` остаются во внешнем каталоге, в репозиторий не копировались. `validate-series.py` можно повторить bundled Python с Pillow; использует только один GPU replayer одновременно. В исходной проверке frame500 использовал уже открытый session71; сохранённый переносимый script при повторе создаёт каждый session заново.

Теперь доступны ранний frame500 и последующие проблемные стадии для сравнения UI/scene writers. Дальнейший ремонт вести по предыдущему image follow-up: установить CPU origin красных вершин990000ff с UV sentinel и отдельно writer/layout испорченного scene input. Принудительную замену цвета/clear и broad256KB snapshots не вводили. Корректный матч/30+ игровых FPS не достигнуты.

Эмулятор/LLDB/gpudebug завершены; Xcode workload frame1000 активен. Ledger CLI на этом Mac отсутствует; обновлены markdown memory и `ufc5/.ledger/STATE.md`.
