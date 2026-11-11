# KyTx PS5 Emulator [Apple Silicon ARM64 + Native Metal RDNA->MSL]

## Project Overview
**KyTx** is a native ARM64 PlayStation 5 emulator for macOS (Apple Silicon M-series) running under FEXCore with a 100% native Apple Metal backend (`MTLLanguageVersion4_0`, `MTLGPUFamilyApple10`).
Strict constraint: **Zero Vulkan, Zero MoltenVK**. All rendering is native Metal/MSL in `src/graphics/host_gpu/metal/`.

## Key Directories
- `src/graphics/host_gpu/metal/`: Native Apple Metal backend
  - `metalRenderer.mm`: PM4 draw/dispatch translator, pipeline state, render targets.
  - `metalPresenter.mm`: CAMetalLayer presentation, linear-to-sRGB blit, PPM dumps.
  - `metalTexture.mm`: RDNA2 texture format mapping, UMA shared memory.
  - `metalContext.mm`: Metal device, command queues, MTLStorageModeShared buffers.
- `src/graphics/presentation/window/`:
  - `swapchain.cpp`: Presentation loop connecting VideoOut to Host GPU presenter.
  - `window.cpp`: SDL3 + CAMetalLayer window management.
- `src/graphics/presentation/videoOut.cpp`: PS5 `sceVideoOut` HLE subsystem.

## Current Priority Task: Fix Black Screen in Metal Backend
- **Root Cause Identified:**
  The game registers a video-out flip buffer at address `0x817c000000` (or `0x817a000000`), but does not write RGB pixel data into it on CPU (`total_nz=0`). Real frame drawing (`DrawAuto`) renders directly on GPU to render targets (e.g. `rt0=0x8241fe0000` / `0x8240000000`).
  In the Vulkan path, `Presenter::Impl::ResolveSurface` resolves the registered address against `TextureCache` and copies the rendered GPU image. In Metal, `swapchain.cpp` was attempting to present the raw CPU `flip_vaddr`, outputting an all-zero (black) buffer.
- **Goal:**
  Ensure `swapchain.cpp` / `metalPresenter.mm` correctly resolves and presents the active GPU Render Target texture (or maps the VideoOut buffer to the corresponding Metal RT texture).

## Build Command
```bash
cmake --build /Users/tx/Developer/KyTx/_Build/macos-arm64 --target kytx_emulator -j$(sysctl -n hw.logicalcpu)
```

## Run Command (EA SPORTS FC 27 - PPSA34015)
```bash
cd /Users/tx/Developer/fex-spike/runs && \
KYTY_BACKEND=metal \
KYTY_FORCE_RT=1 \
KYTY_VIDEO_OUT_1080P=1 \
KYTY_NP_STATE=signed_in \
KYTY_LABEL_FLUSH=lazy \
KYTY_WAIT_RETRY_ELIDE=1 \
KYTY_WAIT_BY_ORDER=1 \
KYTY_UMA_READBACK=1 \
/Users/tx/Developer/KyTx/_Build/macos-arm64/kytx_emulator --game /Users/tx/Developer/PPSA34015
```
