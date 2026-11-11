# Raster state and shadow atlas — 2026-11-09

## Implemented

* `metalViewport.h` now translates signed guest/Vulkan viewports to Metal as `(x, y+h, w, -h, znear, zfar)`. `MetalDrawExecutor::SetViewport` uses it for every draw. The old implementation discarded the reflection for **positive** guest heights. It already matched for negative heights. No shader Y workaround or default winding override was added.
* Both indexed and automatic draws now use `SetGuestScissor`, sharing the existing production `calc_final_scissor` with Vulkan. It intersects screen, window, generic, enabled viewport and supported clip rectangles, applies their window offsets, and clamps to the actual attachment mip extent. Previously the native renderer used only screen scissor.
* Optional `KYTY_TRACE_RASTER_STATE=1` logs distinct raster/depth states and the actual scissor reductions. Default rendering has no diagnostic readbacks, extra GPU passes, sanitizers, shader skips or substitute resources from this change.
* Added standalone GPU mini-programs and included them in `tools/test_emulation_memory_stage.sh`.

## Evidence

| Check | Old behavior | Corrected behavior |
|---|---:|---:|
| Signed viewport, 36 GPU cases / 140,832 pixel checks | 5,328 mismatches | 0 |
| Scissor, 256 GPU cases / 1,048,576 color+D16 pixels | 222,879 mismatches | 0 |

Viewport tests independently calculate asymmetric-triangle coverage and depth at pixel centers for positive/negative heights and normal/reversed depth ranges. Scissor tests evaluate per-pixel Boolean membership from guest registers, including signed offsets, all 16 intersection clip rules, empty rectangles and empty clip rules. A full-screen triangle writes both color and D16 depth; the test checks that pixels outside each region retain their clear values. The scissor test links the **product's actual `debug.cpp.o`**, rather than duplicating its rectangle calculation. Standalone diagnostic sinks throw on unexpected product assertions/warnings; they replace no emulation functionality.

`cmake --build _Build/macos-arm64 --target kyty_emulator -j6` PASS. Both new GPU tests PASS, shell syntax checks and targeted diff whitespace checks PASS. Previous full memory/shader suite was not repeated: these changes touch raster state, and the directly affected GPU programs plus product build were run.

Reference examined: the exact running KosmicKrisp source at `/Users/tx/Developer/vkref-mesa/src/src/kosmickrisp/vulkan/kk_cmd_draw.c` (viewport conversion near lines 790–805, front-face forwarding near 1395), and Kyty Vulkan's `calc_final_scissor`. Latest Kyty upstream inspected earlier was e1292528ffb6a71b758f58373c31602b25b483d4; no bulk merge. KosmicKrisp project documentation: https://docs.mesa3d.org/drivers/kosmickrisp.html.

## Real game verification and limits

Final native run **sn3 PID54466**, started **2026-11-09T19:35:59.856748+05:00**, main/s3 SHA256 **8286f3a33a07823b86107cb252281cc1073644738b1c601aa329a35723bff74f**. Log: `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sn3-193558.log`. One emulator, original 40 GB guard, no time kill or input loop (one observed EA Servers dialog was dismissed with j:6), capture/validation layers off. Original `extra_env_s1.sh` was restored byte-for-byte. Screenshot at frame3070 confirms The Grounds menu. This is a menu smoke, not a full-match validation or an FPS claim.

Real shadow atlas bindings use a 3072×3072 D16 texture. Logs now show intended 64×64, 512×512 and 1024×1024 rectangles at distinct atlas origins instead of the whole screen rectangle. The final journal explicitly confirms **rt=3072x3072**, so this is additional guest clipping, not merely clamping a large screen rectangle to a smaller target. `viewport_enabled=0` in these cases: the restriction comes from window/generic scissor, so implementing only viewport scissor would have missed them. Runtime log scan at19:37:48: 0 GPU faults/command-buffer errors, 0 progress timeouts, 0 fatal/assertion patterns, 0 skipped-compute patterns.

Observed shadow draw viewports are negative-height, and D16 polygon offsets use fixed depth with negbits=-16, constant=-10, slope=-4. Thus the viewport correction and the proposed D16 bit-depth rescaling do **not** explain the remaining shadows in those observed passes. The viewport correction is retained as a proven semantic fix; no arbitrary depth-bias scaling was added.

Old black sky, temporal trails and visual shadow/lighting problems remain visible. These fixes do **not** establish that all old shadows are corrected. Next compare actual VS/PS depth/geometry output and HTILE/depth ownership with the reference. Previous same-input compute replay already matched all 18 tested nonzero lighting kernels between native MSL and SPIR-V→Metal.

Known shared limitations remain: non-intersection clip-rectangle Boolean rules are only reported by the existing Vulkan/common calculation; native indexed viewports, complete depth bounds and depth read/write split need further implementation. Do not claim complete raster emulation.

Vulkan reference sv1 PID52963, SHA3c1d5856e49efb4790886ce9c1f55a9cd591896650ac2c71654a289e9a34ce7b, reached its known white-frame condition. It was stopped by its verified own PID; its white screenshot was **not** used as a clean visual oracle. The preceding sh3, sn1 and sn2 processes were also stopped by their verified own PIDs before the next single run.

All earlier dirty/untracked changes are preserved. No commits/pushes. Ledger CLI/store unavailable. Artifacts and test logs: `/Users/tx/Documents/Codex/2026-11-09/new-chat/work/shadow-fix`.
