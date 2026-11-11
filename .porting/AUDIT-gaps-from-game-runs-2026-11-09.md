> **Уточнение Codex 2026-11-09:** ниже сохранён исторический аудит Claude. Mlock больше не пустой успех; ES/LS 0x0CA/0x14A теперь сохраняются/декодируются, но полный GS/HS backend не завершён. Другие ignore handlers в PM4 остаются, поэтому фраза «тихо игнорируемых регистров нет» неверна. Unsupported=0 означает покрытие перевода встреченных инструкций, а не доказательство всех аппаратных семантик. Актуальный пакет и ограничения: `.porting/REPORT-claude-continuation-2026-11-09.md`.

# Что пропущено в эмуляции: данные реальных прогонов FC27 (9 ноября 2026)

Дополнение к `AUDIT-emulation-coverage-2026-11-09.md` (оно построено чтением кода). Здесь только то, что подтверждено запуском игры:
сборка = состояние Codex 03:13 (после отката сессии Antigravity), сценарий `drive_to_match.sh` (титул → меню → Kick Off → матч), логи в
`/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/c4..c8-*.log`. Ничего здесь не доказывает причину визуальных артефактов.

## Проверено и НЕ является пробелом в этом сценарии
- **Инструкции шейдеров**: `[MSL-coverage] unsupported=0` во всех 37 отчётах (до 950 переведённых шейдеров; VS 178 | PS 216 | CS 202, GS/LS/HS/TES = 0).
- **Пустые слоты изображений** (`[Fallback] unbound image slot`): 865 слотов графики и 445 из 446 слотов compute — это **null-дескрипторы** (адрес 0,
  все поля нули). Подмена нулевой текстурой для них соответствует железу (null T# читается как нули). Диагностика: `KYTY_LOG_UNBOUND=1` (графика уже была,
  compute добавлен сегодня, строки `[Unbound] CR/CS`).
- Неизвестные PM4-опкоды и неизвестные регистры не молчат: диспетчер делает `EXIT(...)`. Тихо игнорируемых регистров нет.

## Реальные пробелы, найденные по прогонам
1. **Один compute-слот с настоящими данными не привязан.** Шейдер `0x5541c203985b0576`, slot 0 (только чтение): Color2D 64x64, формат 75
   (`k32_32_32_32UInt`), tile 5, адрес `0x82df6c2000`. Бинд `ComputeGuestImageFor` вернул nil, слот получил нулевую текстуру. Причина не установлена
   (адрес не отображён? формат/tile 5 не поддержан на этом пути?). Проверить: `KYTY_LOG_CS_ADDR=0x82df6c2000 KYTY_LOG_CS_ADDR_ANY=1`.
2. **Неразрешённый импорт**: `WxLcN76q19o[NpPremiumAddcont_v1][NpManager_v1.1]` вызван игрой 1 раз; `UnresolvedImportStub` молча возвращает 0 (SCE_OK) без
   заполнения выходных параметров. Нужно определить функцию по NID и реализовать (или осознанно вернуть код «нет контента»).
3. **Метаданные DCC/CMASK/HTILE по шаблону**: `[MetaClear]` — HTILE write → clear depth (40), fill CMASK → clear colour (40), DCC sampled images с
   `code=0x00` (динамическое разрешение 892x504…856x484, 2048x2048). `[DrawAuto] skip colour-buffer metadata pass (CB_COLOR_CONTROL.MODE=N)` — 4 раза
   eliminate/FMASK/DCC-переход пропущен. Это этап 2 хендоффа; в данном сценарии замечен только clear code 0.
4. **PM4 без обработчика** (определены в `pm4.h`, нет в таблице `g_cp_op_func`; игра их не выдала, но каждый = аварийный EXIT на другой игре):
   `IT_CONTEXT_CONTROL, IT_CP_DMA, IT_DISPATCH_DRAW, IT_FLIP_DONE, IT_INDIRECT_BUFFER_CNST, IT_MEM_SEMAPHORE, IT_SET_CONFIG_REG, IT_SET_QUEUE_REG, IT_SURFACE_SYNC`.
   Не определены вовсе (есть в наборе RDNA2/GFX10.3): `ATOMIC_MEM, OCCLUSION_QUERY, COND_WRITE, COPY_DW, MEM_WRITE, LOAD_*_REG, SET_SH_REG_OFFSET,
   PRED_EXEC, STRMOUT_BUFFER_UPDATE, DRAW_INDEX_MULTI_AUTO, SWITCH_BUFFER, FRAME_CONTROL, REG_RMW`. Приоритет низкий, пока игра их не выдаёт.
5. **Нагрузка и качество `AliasConvert`** (счётчики `KYTY_LOG_ALIAS_STATS=1`, `[AliasStats]` раз в 600 кадров):
   см. раздел «Результаты счётчиков» ниже; первые данные: до ~15 600 вызовов на кадр, 59–87% найденных GPU-писателей отвергаются правилом
   «старше 2 кадров» (`now_flip > wr.flip + 2`), которое подменяет учёт CPU-записей.

## Результаты счётчиков AliasConvert
(заполняется по итогам c8; формат `calls / writer_found / refused_too_old / layout differs / reused / encoded` на 600 кадров)
- c7 (меню): flip 600: 269 611 / 177 412 / 123 914 / – / 1 622 / 1 622; flip 1200: 2 778 378 / 713 972 / 410 888 / – / 801 / 791;
  flip 1800: 9 349 687 / 1 652 627 / 1 430 288 / – / 1 176 / 1 176.

## Команды
```bash
cd /Users/tx/Developer/KyTx/.porting/work
./drive_to_match.sh <tag> KYTY_LOG_ALIAS_STATS=1 KYTY_LOG_UNBOUND=1   # ~10 минут, один экземпляр; убирает свой PID по лимиту
```
