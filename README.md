# KyTx

**KyTx** is an experimental PlayStation 5 emulator for **macOS on Apple Silicon**, built on
[KytyPS5](https://github.com/KytyPS5/KytyPS5) (itself based on [Kyty](https://github.com/InoriRus/Kyty)).
It replaces the upstream x86-64 + Rosetta 2 + MoltenVK setup with a native ARM64 build:

- **Guest CPU:** x86-64 game code runs under [FEX](https://github.com/FEX-Emu/FEX) (FEXCore embedded in-process),
  not Rosetta 2.
- **GPU:** RDNA shaders are translated straight to Metal Shading Language and PM4 command streams are executed on a
  native Metal backend (`src/graphics/host_gpu/metal/`). The product path uses no Vulkan, MoltenVK or SPIRV-Cross; the
  inherited Vulkan renderer is kept only as a diagnostic reference.

> [!IMPORTANT]
> KyTx is not affiliated with Sony Interactive Entertainment, PlayStation or any game publisher. The project does not
> distribute games, game data or copyrighted system software. Use only game files that you have obtained legally.

## Status

Early and unstable. Only one title has been worked on so far:

| Game | State on an M5 Max |
|---|---|
| EA SPORTS FC 27 | Boots to the main menu (30 FPS, the game's own cap there) and into a playable match (30-37 FPS; the game asks for 60). Many rendering errors remain, especially in matches; occasional crashes and GPU hangs. |

Nothing else has been tested on this fork, including UFC 5 (the target of the fork this one started from).

- **Platforms:** macOS on Apple Silicon only. There are no Windows or Linux builds of KyTx; the upstream code for those
  platforms is still in the tree but is not built or tested here.
- **Requirements:** an Apple Silicon Mac with a Metal 4 capable GPU (developed on an M5 Max, 48 GB).

## Building

The macOS arm64 build links FEXCore from a FEX-darwin source/build tree that is **not part of this repository yet**
(CMake looks for it in `~/Developer/fex-darwin`, overridable with `KYTY_FEX_DIR` / `KYTY_FEX_BUILD_DIR` /
`KYTY_FEX_SHIMS_DIR`). With that in place:

```bash
cmake --build _Build/macos-arm64 --target kyty_emulator -j$(sysctl -n hw.logicalcpu)
```

Run with the Metal backend:

```bash
KYTY_BACKEND=metal KYTY_MSL_ONLY=1 KYTY_VIDEO_OUT_1080P=1 \
  _Build/macos-arm64/kyty_emulator --game /path/to/your/game
```

## Development notes

Diagnostic switches are environment variables documented next to their code (for example `KYTY_PASS_TRACE_*`,
`KYTY_GPU_BUSY`, `KYTY_GPU_PASS_TIME` in `src/graphics/host_gpu/metal/`). The upstream KytyPS5 documentation is in
[`docs/`](docs/).

## Credits

- [Kyty](https://github.com/InoriRus/Kyty) by InoriRus and contributors.
- [KytyPS5](https://github.com/KytyPS5/KytyPS5) and its contributors.
- The KytyPS5-UFC5 fork by Reece Ketley.
- [FEX](https://github.com/FEX-Emu/FEX) (MIT).

## License

GPL-2.0, as upstream. See [`LICENSE`](LICENSE) and [`LICENSES/`](LICENSES/).
