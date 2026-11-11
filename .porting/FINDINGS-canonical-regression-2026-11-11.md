# FC27 Metal regression since 2026-11-10: canonical memory model (findings, no fix yet) — 2026-11-11 03:15 +05

## Facts
- sd1–sd3 (2026-11-10 00:19–00:48, SHA 2e384a69 for sd3): title shows The Grounds 3D scene at ~30 FPS, main menu shows the
  street scene at ~25 FPS (Codex screenshots work/shadow-depth/sd3-first.png, outputs/shadow-depth-grounds.png). [rate] median 30.
- Every later run: dm2/c9 (07:29–07:31) 27 → 0.4–1 FPS; ml*/sr*/sp*/vm*/wc* 0.5–4 FPS; AL1/AL2 (absent-lane builds)
  title white at ~1 FPS, menu (AL2) covered in blocky texture garbage at ~3 FPS. ms*/ng*/mp* runs died early on strict
  "Unsupported Metal emulation" aborts, so stage-by-stage bisection by game runs is not possible.
- The per-draw re-import arrived with canonical-mip (work/metal-mip-alias, 07:10): `CanonicalMipImage` caches
  "within a single draw/dispatch only"; `s_image_read_epoch` is bumped by every DrawIndex/DrawAuto/Dispatch. For every
  sampled texture overlapping a canonical target it ends the render pass, gathers guest memory, re-imports every mip/layer
  and reopens the pass with Load; a Load reopen runs the target's `before_load` (full tiled import) and the earlier pass's
  `after_store_regions` (export). Since color-ownership (18:49) `canonical_rt = mipped_rt || tiled` — every tiled RT.
- AL3 (KYTY_GPU_PASS_TIME=1, build 11dd451a, log lean/al3-030038.log) at the title: 50.4% of timed GPU time in render
  passes with zero draws (209 passes, 243 ms, ~1.2 ms each); many PS run one draw per pass; import/export kernels are not
  in the timer. GPU command buffers in AL2: median 28.7 ms, p90 128 ms, max 585 ms.
- No [CanonicalRenderReject]/[CanonicalImportReject] in AL2; the garbage's producer is not identified yet.
- The native Metal path does not page-protect guest memory (comment at GuestTexSourceHash); exact caching of canonical
  images therefore needs write tracking for CPU writes, shader buffer stores, DMA and canonical exports (Scatter).

## Not established
Which import/export (or other path) produces the menu garbage and the white title; whether reverting to the pre-07:10
texture path alone restores the image.

## Stage 3 — exact write tracking for canonical copies (2026-11-11 03:40 +05)

New `metalGuestWrites.h/.inc` (included at the end of metalRenderer.mm): write generations per 16 KiB physical page.
CPU writes: watched pages are write-protected through every guest alias; `HandleGpuFault` → `HandleCpuWriteFault`
unprotects and advances; `InvalidateMemory` (file reads, AIO) and guest `mprotect`/`ProtectGuestMemory` unwatch and
advance with a 500 ms cooldown (a racing re-protect cannot EFAULT a host syscall); `TryWriteBacking` advances. GPU writes,
at encode time: canonical `Scatter` (physical footprint), written/atomic stage buffers over their V# range for every draw
and dispatch, buffer-backed linear RTs per draw, GDS→memory DMA; `uses_dma` programs advance everything. A mapping
change drops all watches and makes every copy stale. Unwatched pages are never current.

Consumers: `CanonicalMipImage` reuses its copy across draws while `Current()`; storage writeback re-syncs it. Canonical RT
`before_load` skips the import when the level was last loaded/cleared and stored and nothing wrote the surface since.

Tests: `tools/test_metal_guest_writes.sh` 27 checks PASS (aliases, faults through either alias, GPU/physical/unknown writes,
cooldown, mapping change). `test_metal_coherence.sh` (links the tracker via test shim) API+GPU Validation PASS.
Build: CMake reconfigure needed `-DFETCHCONTENT_SOURCE_DIR_ZARCHIVE_SOURCE=<build>/_deps/zarchive_source-src` (the
FetchContent PATCH_COMMAND is not idempotent); product SHA 0c5f8f4c53a0694c44d0f665944493fcc18965047fae0729f8875bf488d26f06.

Game AL4 (PID 4749, log lean/al4-033129.log, KYTY_GPU_PASS_TIME): main menu renders correctly — Manager Career /
Grounds stadium with sky and players, no blocky garbage (was AL2). 0 command buffer errors after 5 min. Empty render
passes 50% → 1.5% of timed GPU. Totals: image copies reused 138168 / imported 67168; RT imports skipped 108100 / 12653.
Still ~1.5–2 FPS: compute guest c29caf0c1cb0ad94 (16x16 wave64, LDS 3968 dw) ~400 ms per dispatch, 6b5ae237c41892cb
~170 ms, c8aa351920a3b786 ~130 ms — the same shader ran within a 30 FPS frame on sd3, so this is a separate regression.
`unknown` writes 400–500/s (uses_dma programs) still invalidate every copy often.
