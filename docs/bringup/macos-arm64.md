# macOS ARM64 bring-up

This experimental build uses [alamshafil/fexcore-darwin](https://github.com/alamshafil/fexcore-darwin)
at revision `eee10cc54a173e79ee16e2169cd4157c7269921b`. The compatibility patch beside this
document removes a diagnostic callback specific to that FEX port's original embedding project.
Keep the KyTx and FEXCore checkouts adjacent as `source` and `fexcore`.
Install native CMake, Ninja and SPIRV-Cross via Homebrew. Xcode is required.

From the parent directory, if the FEXCore checkout is missing, fetch it once:

```sh
git clone https://github.com/alamshafil/fexcore-darwin.git fexcore
git -C fexcore checkout --detach eee10cc54a173e79ee16e2169cd4157c7269921b
```

Configure and build in the existing `build` directory:

```sh
bash source/tools/build_prospero_macos.sh build
```

The script checks the FEXCore revision, applies the compatibility patch once,
and verifies the arm64 executable signature. Set `CMAKE_BUILD_PARALLEL_LEVEL`
to change the default of four build jobs. The reference toolchain is Xcode
27.2 (27B52), CMake 4.5.1 and Ninja 1.13.2. The guide reproduces the source and
build options. Toolchain or SDK changes can yield different binaries.

From `build/kytx`, run with a legally obtained game directory:

```sh
KYTY_BACKEND=metal KYTY_MSL_ONLY=1 KYTY_MSL_GDS=1 KYTY_VIDEO_OUT_1080P=1 \
  ./kyty_emulator --game /path/to/game
```

On a Mac mini M4 with 16 GiB and macOS 27.2.1, *Is This Seat Taken?*
(PPSA37001) reached the title menu, the Barcelona map and the first level.
Metal frame captures showed readable text and no recurrence of the reported
purple rectangle or corruption on the first level's Perfect 6/6 results screen.
The focused vertex layout cache-key regression passed. The full `shader_cfg`
suite failed the same SPIR-V repeated-export check before and after the fix.
Audio quality and regression coverage for other titles remain unverified.

The native attribute path supports unswizzled descriptors within a bounded range
of argument buffer slots. Cross-thread exception delivery on this Darwin path is
cooperative at guest waits.
