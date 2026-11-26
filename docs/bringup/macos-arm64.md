# macOS ARM64 bring-up

This experimental build uses [alamshafil/fexcore-darwin](https://github.com/alamshafil/fexcore-darwin)
at revision `eee10cc54a173e79ee16e2169cd4157c7269921b`. The compatibility patch beside this
document removes a diagnostic callback specific to that FEX port's original embedding project.
Keep the KyTx and FEXCore checkouts adjacent as `source` and `fexcore`.
Install native CMake, Ninja and SPIRV-Cross via Homebrew. Xcode is required.

From the parent directory:

```sh
git -C source submodule update --init
git -C fexcore apply --unidiff-zero "$PWD/source/docs/bringup/fexcore-optional-snapshot.patch"
cmake -S fexcore -B build/fex -G Ninja \
  -DBUILD_FEXCORE_ONLY=ON -DBUILD_FEXCORE_SMOKE=OFF -DBUILD_TESTING=OFF \
  -DTUNE_CPU=none -DENABLE_LTO=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/fex -j 4
cmake --build build/fex --target FEXCore FEXCore_Base -j 4
cmake -S source -B build/kytx -G Ninja \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release \
  -DKYTY_BUILD_LAUNCHER=OFF -DKYTY_FEX_DIR="$PWD/fexcore" \
  -DKYTY_FEX_BUILD_DIR="$PWD/build/fex" -DKYTY_FEX_SHIMS_DIR="$PWD/fexcore"
cmake --build build/kytx --target kyty_emulator -j 4
file build/kytx/kyty_emulator
codesign --verify build/kytx/kyty_emulator
```

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
