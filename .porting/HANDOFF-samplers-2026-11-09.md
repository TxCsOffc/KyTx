# Следующая проверяемая очередь — sampler stage, 2026-11-09

Эталон upstream16a7bbe3 и независимые GPU/CPU проверки применять до игры. Игру sp3 не перезапускать без проверки текущего процесса.

1. ImageSampleFlagClamp (`_cl`): актуальный upstream Layout хранит clamp и EmitSample передаёт MinLod либо FMax с explicit LOD. Наш direct MSL Layout не хранит clamp. Сначала один IR с computed LOD ниже/выше `_cl`, CPU footprint и обе Metal paths; затем реализация без игнора.
2. ImageGatherRaw explicit LOD: direct MSL вызывает gather только на базовом image, хотя frontend передаёт LOD/T#/S# control words. Upstream выбирает mip view через GatherMip/EmitImageMipSwitch и различает nearest/linear primary mip. Нужны fixture с разными pattern каждого mip, point-preclamp и base/min/last constraints, потом runtime per-mip views.
3. Software anisotropic/independent Z: native normalized2D depth path проверен2x–16x. Dynamic sample offsets/прочие color formats/1D/3D используют реальный isotropic footprint, не полноценный AMD anisotropy/Z_FILTER. Не объявлять этот участок завершённым.
4. AddressMode host mapping сейчас сводит MirrorOnceHalfBorder/MirrorOnceBorder к MirrorClampToEdge; custom border к transparent black. Это подтверждённые неполные семантики, нужен цветной border mini-test до адаптера.
5. D16/A16 отдельные differential matrices, cube/depth cases, runtime 3D view paths. Текущий mip fix относится к2D; не распространять утверждение автоматически на3D/cube.
6. Runtime owner/version/coherence и compressed metadata DCC/CMASK/HTILE остаются отдельным большим этапом. Тёмные и шумные игровые артефакты пользователь назвал исходными; это не признак регрессии текущего stage.

Не маскировать расхождения finite clamps, NaN/TAA sanitizers, shader skips или zero textures. Default fallback эмулятора ещё есть; при валидном ненулевом дескрипторе искать конкретную причину.

Current completed implementation/test report: .porting/REPORT-sampler-emulation-2026-11-09.md. See current STATE for game/process outcome.

## Проверено после возобновления, 2026-11-09

Upstream latest: e1292528ffb6a71b758f58373c31602b25b483d4. Новые четыре коммита относительно16a просмотрены; image emitter не изменён. Разобрать отдельно4fa10abb (depth mip-stats id),26ad49a6 (stack traversal),937b3766 (present lock),e1292528 (PSSR temporal inputs). Не переносить новый resource planning целиком поверх нашего BindingLayout.

Дополнение stage: tail BASE=LAST7/8 декодируются из выбранного physical mip в настоящий1×1 Metal texture; cache key учитывает first level. Dynamic sample offsets не отбрасываются: Metal target contract разрешает конкретную SPIR-V Offset operation в emitter/optimizer/validator. Vulkan требует maintenance8. SPVC function key теперь учитывает фактические module bytes.28files focused patch в outputs.

Не повторять ошибку sp3: общий Vulkan-only gate dynamic offset оборвал Metal fallback, хотя нативная Metal операция поддерживает offset. Это исправлено и проверено production optimizer +2496GPU cases.23modules проходят целевой validator; четыре dynamic sample modules должны отклоняться strict Vulkan validator без соответствующего feature.
