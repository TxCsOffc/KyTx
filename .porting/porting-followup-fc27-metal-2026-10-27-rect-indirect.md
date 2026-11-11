> Актуальное состояние и завершение: [handoff 2026-10-27](/Users/tx/Developer/KyTx/.porting/porting-handoff-fc27-metal-2026-10-27.md). Конфликт сессий разрешён; пользователь остановил старый CLI. Старые строки о незавершённых запусках ниже являются историей.

# FC27 — indirect RECTLIST, 2026-10-27

Продолжение найденной Claude-сессии `a890ad9a-fc56-4a1c-bd0c-4830627c7a46` («Передача Review KyTx»). Её последняя содержательная запись26 октября23:14 совпадает со скриншотом пользователя. Журнал прочитан; Claude CLI не перезапускали и не останавливали. Новых агентов нет. Контекст HDR и завершённые три диагностических прогона — [предыдущая запись](porting-followup-fc27-metal-2026-10-26-hdr-resume.md), функциональные исправления Claude — [night handoff](porting-handoff-fc27-metal-2026-10-26.md).

**FEX + direct RDNA→MSL. Без Rosetta/Vulkan renderer. Один эмулятор через `.porting/work/run1.sh`, лимит20GB и времени. Никаких коммитов. Сохранить весь dirty/untracked tree.** Ledger CLI отсутствует, память обновлена markdown.

## Исправление

В игре есть indirect RECTLIST с GPU-аргументами3 вершины, переменное число экземпляров. `DrawAuto` видел лишь CPU placeholder1 и выключал синтез четвёртого угла; `DrawIndirect` рисовал triangle strip из3 вершин. Это отдельный доказанный дефект, но не доказанная причина всех артефактов HDR.

- `metalIndirect.h/.mm`: GPU compute превращает только vertexCount3→4; сохраняет instanceCount, vertexStart, baseInstance и оригинальные аргументы рядом. Поддерживает массив/stride/offset. Не читает GPU-аргументы на CPU, не меняет гостевой буфер. Пул tracked Metal3 buffers; конверсия выполняется до render state, pass открывается снова с Load для color/depth/stencil.
- `metalRenderer.mm`: выбирает текущие MDI-аргументы после existing draw-count clamp, расширяет nonindexed indirect RECTLIST, ставит wrapper mode3, привязывает snapshot в vertex buffer26. Остальные VS получают валидный нулевой buffer26. Indexed draw сбрасывает предыдущие nonindexed RECTLIST controls. Indexed indirect RECTLIST остаётся за пределами этой правки.
- `MslEmitter.cpp`: mode3 синтезирует четвёртую вершину из base,base+1,base+2, когда исходный count==3; не читает отсутствующую четвёртую запись. Сохранены direct/indexed режимы предыдущего фикса.
- `metalDraw.mm`: nonindexed indirect RECTLIST выбирает TriangleStrip; indexed path остаётся Triangle. Удалено устаревшее предупреждение «no fourth-vertex expansion».

## Проверка

`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 bash tools/test_metal_msl.sh` — весь native MSL GPU suite проходит. Новый regression использует настоящий EmitProgram и production MetalRectListIndirect; аргументы создаёт GPU blit в private buffer. Проверяет full coverage/UV с bounds3 записи, nonzero vertexStart5, instanceCount2/baseInstance7, zero instance/count, non-three count4, offset16/stride32, original argument preservation и source bounds.

Старый контроль: **528/1024 неверных пикселей**. Пять исправленных случаев: **0/1024 каждый**. Лог `artifacts/2026-10-26/hdr-resume/rect-indirect-test.log`. ARM64 build `build-rect.log` успешен. `git diff --check` прошёл.

Один `rectind1`, PID16711, max190s, завершён run1 по лимиту. Лог `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/rectind1-000350.log`. PassTrace flips2183/3578 подтверждает текущий VSd0195896f0814231 и indirect RECTLIST исходные `[3,8,0,0]`/`[3,149,0,0]`, ненулевой FRAGS. Последний DrawStats4475, dropped_vs/ps/pipe0, logged GPU errors0, peak observed footprint13475MB. **Это не сравнение FPS и не доказательство правильного полного меню.**

CUA визуально подтвердил главное меню на flip2290: UI читается, но фон зелёный, точки/шум, картинки слева неверны. PPM2182/3576/4018/4391 и `rectind1-series.png` показывают остаточные дефекты. Моменты сцены между прогонами отличаются; численные FRAGS из разных кадров не являются A/B покрытием. Тест GPU выше является изолированным доказательством фикса.

## Проверка подключения в Xcode и вторая часть фикса

Завершён последовательный `rectcap1`, PID17911, один кадр2600, programmatic MTLCaptureManager (`MTL_CAPTURE_ENABLED=1`), run1 max260s, остановлен по лимиту. Без PassTrace и принудительной очистки HDR. `[GpuCapture] started/stopped` подтверждены до остановки процесса; размер18GiB. Лог `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/rectcap1-000744.log`, последняя статистика flip2925, logged GPU errors0, peak observed footprint15803MB. Путь `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261027/rect-indirect-one/frame2600.gputrace`. Эмулятор остановлен. Xcode27 открыл трассу через CUA; Replay пока Preparing frame. Проверить успешное завершение и workload; это ещё не подтверждение pixel debug. Не запускать параллельно emulator и тяжёлый replayer. Env `.porting/work/extra_env_s1.sh` — rectcap1; предыдущий rectind1/env сохранён рядом с артефактами.

Корректное интро, фон меню, матч и30+ игровых FPS ещё не достигнуты. Принудительный CLEAR_HDR_FIRST не является решением. Следующая работа — первый производитель некорректного HDR/кохерентность алиасов и инициализация нового использования памяти, по данным текущей трассы.

Xcode27 через CUA успешно Replay первого frame2600: **27CB,87 render,388 compute,17 blit,1058draw,307dispatch**, меню в Summary с остаточным шумом. Трасса не пустая. По фильтру RECTLIST найдены CB21, CE166/167/168, RE37/38/39; API16443/16483/16526 привязывают snapshot offset16 в vertex buffer26, но API16444/16484/16527 используют **Triangle**. Это выявило неполный integration fix: штатный TranslatePrimitiveType для RECTLIST возвращает Triangle (нужно indexed path), прямой Draw отдельно переключал Strip, а indirect — нет. Четыре вершины Triangle рисовали лишь первый треугольник. Поэтому визуальные прогоны rectind1/rectcap1 относятся к **промежуточной сборке и не доказывают полный фикс**.

Исправлен DrawIndirect: RECTLIST→TriangleStrip. В GPU regression контроль теперь использует именно expanded count4+Triangle:528/1024 bad; fixed Strip cases5/0. API+shader validation/full suite и ARM64 build повторно проходят (`rect-indirect-topology-test.log`, `build-rect-topology.log`). Xcode Replay остановлен, окно первой трассы закрыто.

Запущен один rectcap2 через run1 max360s, capture ровно2300 в `/Users/tx/Developer/fex-spike/runs/captures/fc27-20261027/rect-indirect-strip-one/frame2300.gputrace`; auto input только1500/1850/2200:j. После stopped+PPM остановить только этот RUN_TAG/PID, открыть новый кадр в Xcode и подтвердить реальные TriangleStrip calls. Текущий env rectcap2. Нового подтверждения полного исправления сцены пока нет.

## Итог повторного Xcode Replay и packed-атрибуты

`rectcap2` PID19237, лог `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/rectcap2-001855.log`; started/stopped frame2300 подтверждены. После PPM2356 остановлен только свой PID с проверкой RUN_TAG. Последний DrawStats2925, dropped_vs/ps/pipe0, logged GPU errors0, peak observed footprint15959MB. Трасса занимает15GiB на диске, Xcode считает17.48GiB ресурсов.

Xcode27/CUA успешно Replay: **23CB,82render,432compute,19blit,765draw,367dispatch**. Фильтр RECTLIST: CB18, CE231/232/233, RE39/40/41, API13189/13229/13272 — теперь **TriangleStrip**. Vertex buffer26 snapshot offset16. Double-click Indirect в Bound Resources API13272 показывает строки: offset0 `[4,160,0,0]`, offset0x10 `[3,160,0,0]`. Это runtime-доказательство GPU3→4 без потери160 instances.

Double-click Geometry того же API: instance0 позиции0..3 `(0.920,-0.133,1,1)`, `(0.960,-0.133,1,1)`, `(0.920,-0.204,1,1)`, `(0.960,-0.204,1,1)` — четвёртый угол правильный. Однако param0 первых3 вершин одинаков (`~7e-45,~4e-43,0,0`), у четвёртой `(0,0,0,0)`. Это упакованные целые через float bits; прежняя формула B+C-A уничтожает subnormal-значения при FP arithmetic. **Не делать вывода, что это единственная причина HDR-шума: flat provoking vertex может быть исходным углом.**

Исправлено после rectcap2: MslEmitter генерирует `kyty_rect_corner`, для каждого компонента с одинаковыми исходными uint-битами A/B/C копирует B побитно; остальные вычисляет B+C-A. Применяется к mode1/2/3 (direct/indexed/indirect). Unit GPU regression специально ставит синтетическую вершину первой в треугольнике, чтобы flat-интерполянт действительно проверял её raw bits. Константы5,0x10003,1.0f,-0.0f. **До:528/528 неверных пикселей. После:528/528 правильных, во всех3 режимах.** Обычные5 indirect coverage/UV cases0fail, старый Triangle control528bad. Логи `rect-packed-before-test.log`, `rect-packed-after-test.log`, `rect-packed-final-test.log`; полный suite с Metal API+shader validation проходит, ARM64 `build-rect-packed.log` успешен.

**frame2300 снят до packed-правки**, подтверждает топологию/аргументы и показывает причину второй правки. Не объявлять его pixel proof после packed-фикса. Xcode Replay остановлен, документ frame2300 оставлен открытым. Ни gpudebug, ни LLDB не запускались.

Идёт завершающий обычный запуск **rectpacked1** через run1 max180s, auto input1500/1850/2200:j, без capture/forced HDR clear/PassTrace. Текущий extra_env_s1.sh rectpacked1. После PPM2350+ проверить картинку и лог, завершить только свой tagged PID и обновить состояние. Полная сцена/интро/матч30+ остаются незавершёнными, если новая картинка их не подтверждает.

## Остановка проверки из-за автоматического возобновления другой сессии,00:31

Финальный rectpacked1 PID20025 **не завершил проверку меню**: последний DrawStats1125, PPM2350 не получен. Это **не установленный crash/regression**. Журнал исходной Claude root a890ad9a содержит automatic-limit-reset19:30:54Z, Bash19:31:02Z вызвал `.porting/tools/killmine.sh`, затем текст19:31:07Z «Оставшийся экземпляр агента (`rectpacked1`) остановил» и SendMessage19:31:09Z возобновил af6cef93351f3493a. Внешний агент затем запустил acc1 PID20491 через run1.

В этом Codex чате больше эмуляторов не запускаем и production source не меняем, пока пользователь не определит, какая сессия продолжает. Через async-вопрос попросили остановить старую сессию Ctrl+C или оставить работу ей. **Копия всех исходников/тестов с packed-фиксом и manifest сохранена в [codex-checkpoint-0031](artifacts/2026-10-26/hdr-resume/codex-checkpoint-0031/CONTINUATION.txt)**. Это включает прежнее dirty состояние; не применять целиком поверх последующих изменений внешнего агента без diff.

Итог этой работы: source и GPU regression для indirect_RECTLIST/topology/packed constants готовы; API+shader validated suite и ARM64 build проходят. Runtime topology/arguments/4 positions подтверждены Xcode; runtime **после последнего packed-фикса** до меню не проверен из-за внешней остановки. Зелёный/белый HDR-шум оставался на последнем полностью проверенном кадре. Интро, корректный матч и30+FPS не заявлять. Xcode frame2300 открыт с Replay остановленным. Коммитов нет.
