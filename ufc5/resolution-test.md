# Lower-resolution output experiment

## Fight result (2026-10-05)

The game selected smaller internal surfaces: 1068×600 scene surfaces,
1600×900 color/depth and 1920×1080 depth. The last census samples show driver
usage about 6456–6560 MiB below a 7249–7296 MiB budget, with native image
allocations about 2063–2152 MiB. The user saw about 6.9 GB VRAM use and reported
**identical FPS**. The picture regressed to a mostly black scene with cyan/blue
at the left edge while the HUD remained visible.

Keep this mode diagnostic. Reduced pressure did not provide the needed FPS gain,
and the corrupted frame prevents treating it as a clean performance baseline.
Shared-VRAM spill alone is not supported as the main FPS cause. No rendering
fix or link to the user's similar shadPS4 observation has been established.
See the ledger session39 for measurements and next steps.

## How the experiment works

Kyty normally reports 4K to UFC 5 through `VideoOutGetOutputStatus` because
the title's `ATTRIBUTE3` does not enable output resolution detection. The host
window defaults to 1280x720; changing its size alone does not change that report.

The diagnostic `KYTY_VIDEO_OUT_1080P=1` reports the existing lower-resolution
output mode (code 1, 1080p) on the main video-out port. It is off by default.
The game must choose smaller internal targets in response. This does not force
all textures to 1080p, rewrite tiled layouts, or implement arbitrary render scaling.
If the game ignores the report, this experiment will not lower its rendering size.

```powershell
& D:\PS5\src\KytyPS5\ufc5\tools\test-output-resolution.ps1 -Build
```

This stages `kyty_emulator.resolution-test.exe` alongside the normal executable
and starts the game with the override. Close any running emulator first. Drive
to the same paused fight, then compare:

- Registered output dimensions in the timestamped `KytyLog-UFC5-resolution-1080p-*.txt`.
- Large internal surface dimensions in `ufc5-detile-resolution-1080p-*.csv`.
- Native image allocations, driver usage/budget and host usage in `D:\PS5\tex-census.txt`.
- Present-rate samples in `D:\PS5\fps-pressure-test.txt`.

Both fixed diagnostic files append; distinguish this run from earlier samples.
Live GPU timestamps, detile capture, dropped textures, tight image usage and the
draw-commit worker are disabled for this test. Other settings are inherited.
For a control run, close the test and run the same script with `-Mode Native`.
The window size and remaining test settings are identical in both modes.

Treat lower VRAM usage and higher FPS as separate observations. A lower image
size also reduces pixel work, so an FPS gain alone would not prove that VRAM
spill caused the slowdown. No live resolution switch is provided: the game
may select its resource sizes only at startup.
