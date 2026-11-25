# Prospero · GT7 on native Metal

Gran Turismo 7 1.71 reaches the World Map on a Mac mini M4. The PS5 reference
shows the coastline, terrain, water, buildings and readable menu labels. The
Metal capture preserves the coastline and some icons. The red overlay is gone.
The landscape is still mostly black, and menu labels are missing.

## What changed since KyTx

| Commit | Change |
| --- | --- |
| `859671c` | Adapted the Darwin FEX integration and Unity startup. |
| `2964105`, `d5a3160` | Read embedded vertex attributes and specialize vertex pulling by layout. |
| `44e1f49`, `a288f2a` | Pinned FEXCore and documented and scripted the ARM64 build. |
| `52d074d` | Corrected the Metal presenter's sRGB conversion. |
| `188c68b` | Added an archive adapter for streaming game packages. |
| `5f07318` | Fixed GT7 startup on ARM64. |
| `efc1923` | Added GT7 mesh translation and descriptor tracing. Added support for compute cubemap sampling. |
| `48b42f6`, `bd29a0b` | Preserved guest compute LDS bounds and backed oversized workgroups with private GPU memory. |
| `0ccde5e` | Added `.pkg` and `.fpkg` import to the Prospero library, using the existing streaming archive adapter and a local `read-fpkg` helper. |
| `52fa466` | Applied guest component selectors to Metal storage-image writes and fixed sparse store masks. |

KyTx is based on KytyPS5, a fork of Kyty. The
[macOS ARM64 build guide](macos-arm64.md) identifies the fork's FEX dependency
and the referenced toolchain.

## What the World Map trace shows

- With `KYTY_LOG_UNBOUND=1`, a baseline GT7 run reported 26 unique non-null
  image failures. They pointed at the same 1×1 cubemap. All four component
  selectors request zero. The native Metal bindings now use a compatible
  six-slice texture with the guest selectors, with no instances of those 26
  failures in the repeated scene.
- The device reports 32,768 bytes of threadgroup memory. For GT7's first mesh
  pipeline, staging three indices in one word reduced Metal's original
  34,000-byte requirement without reducing the guest allocation. The native
  compute path now runs two kernels with their full 65,536 bytes of LDS per
  group. It can allocate up to 64 MiB for an oversized dispatch, and reports
  allocation failures with the shader hash.
- The display pass with Metal function key `4f4e29ec510fee3b` reads an
  HDR image containing the coastline and a nearly black landscape, then
  samples a 3D color table before drawing the interface. The table's ABGR
  descriptor exposed the alpha written into the physical alpha channel as red.
  The corrected Metal store puts alpha in the physical red channel. The table
  now reads as black at the origin and has independent RGB axes. In final map
  captures, a sampled region went from 44,720 of 73,080 pixels with R ≥ 240 and
  G/B ≤ 16 to zero. The dark HDR input still needs investigation.
- Capturing the same scene before and after `4aa7c29794420db2` shows the pink
  sky and the mountain silhouette in its input at `0x920ec00000`, with most of
  the terrain already black. Its output at `0x9223690000` (color attachment 1)
  is darker still. `a6c1f1e9298a6dcd` then writes that same attachment. The trace
  also shows both passes using the scene depth surface at `0x9210c10000`.
- With the fixes enabled, the output of `4aa7c29794420db2` shows the terrain and
  trees before the last composition passes. Its 3D inputs at `0x923dba0000`,
  `0x923fd60000`, `0x926ef76000` and `0x926f296000` trace back to compute writers
  `1922aae815846978`, `d2a8505196a1078e` and `e297bb22e6dc4335`. The ordered
  `[RtVolume]` readback found nonzero packed texels in all four images and
  zero center texels in the two 240×135×128 images. Their expected PS5 values
  remain unverified.
- The dispatch for `ed3ba044f1410a06` writes to a 128×128, six-layer descriptor
  at `0x9296cd0000`, but the old storage allocation was a single 2D texture.
  Several World Map pixel shaders sampled a six-face cube decoded from guest
  backing at the same address. That copy had zero nonzero bytes. The experimental
  GPU array path retains all six layers and the mip chain for the writer and
  reuses the texture for compatible array and cube readers. It is enabled by
  default for `PPSA01317`, `APP_VER=01.710.000`. `KYTY_METAL_GPU_ARRAYS=0`
  disables it for comparison, `=1` enables it for other titles, and
  `KYTY_GPU_ARRAY_ADDRESS=9296cd0000` isolates this image.
  The two compute writers at `ed3ba044f1410a06` and `4269bcac84fccc8c` share
  the same six-layer allocation in the GT7 trace.
  An M4 test writes the sixth layer through a view of mip level 1 and verifies
  the stored channels. In the World Map run, 92 sampled bindings of
  `0x9296cd0000` reused the six-face GPU texture. Trees, roads and terrain became
  visible. A fixed 4K terrain region went from 7,548 of 7,548 sampled pixels
  with all channels ≤3 to 1,490 of 7,548. The sample covers 4–27% of image width
  and 39–64% of image height at an eight-pixel stride. The game reached the map
  with the default title profile as well. The scene still lacks the reference
  brightness and menu text.
- Follow-up runs intermittently stalled in Metal before reaching the cubemap
  writer. With `KYTY_MTL_SPLIT_OPS=1` and `KYTY_MTL_GPU_TIME=1`, two runs isolated
  the stalled command buffer to `5b8c099adc368053`, one group of 512 Metal
  threads serving a 1024-thread guest group with 64 KiB of device-backed LDS.
  The generated shader includes a threadgroup barrier inside a wave-vote branch.
  A separate M4 test with the same group dimensions and LDS size passes
  cross-wave synchronization and workgroup isolation. An isolated source probe
  replaced the branch predicate with its statically known value. GT7 still
  stalled at the same dispatch. The original shader remains the default.
- `KYTY_TRACE_CS_WORD=5b8c099adc368053,0,288` records the four dwords beginning
  with this shader's loop bound and correlates them with command completion.
  With `KYTY_TRACE_CS_WORD_GPU=1`, a Metal blit queued before the dispatch also
  captures the live input. In a failing run the CPU saw `8` at bind time and
  the GPU read `0x80000514` from the same address. Successful dispatches often
  completed in about 0.5–0.8 ms with the bound at `8`. The failure occurred
  with a six-face texture bound. The PM4 and declared compute-buffer write traces
  did not identify the writer of the changed word.
- The renderer now snapshots this shader's 368-byte read-only buffer into a
  separate retained upload allocation. Sampled cubemaps use the matching
  six-face texture or the guest cube layout if the newest writer has one layer.
  In a World Map run with both changes enabled the traced shader completed
  364 dispatches, with the bound preserved at `8` and six-face views bound.
  That run still used the guest-backed cubemaps. Set
  `KYTY_SNAPSHOT_GT7_LIGHT_CONSTANTS=0` or `KYTY_METAL_STRICT_CUBES=0` to isolate
  the changes in a diagnostic run.
- The PS5 reference is a final frame. It identifies the matching shoreline and
  shows which map elements are missing. Comparing intermediate outputs against
  PS5 requires a capture of the same render target on that platform.

## Checks in the existing build directory

```sh
build=/path/to/existing/build/kytx
cmake --build "$build" --target resource_tracking_tests resource_materialization_tests \
  scalar_provenance_tests shader_vertex_metadata_tests
ctest --test-dir "$build" --output-on-failure \
  -R '^(resource_tracking|resource_materialization|scalar_provenance|shader_vertex_metadata)$'
bash tools/test_metal_msl.sh "$build"
```

The MSL tests include cubemap sampling, full and sparse storage-image writes,
packed D16 writes, local LDS counters, and a dispatch over eight groups that
verifies the final dword of an oversized LDS
allocation, synchronization across SIMD groups and isolation between
workgroups. Replay GT7 and inspect the terrain, water, buildings and menu
text independently to verify the World Map.

For a bounded render-target trace, set `KYTY_LOG_IMAGE_PRODUCER` to the guest
image address and `KYTY_DUMP_RT_MIN_FLIP` to the start of the 12-frame window.
`KYTY_DUMP_RT_DRAW=address,pixel_shader,0,1` dumps the target before the matching
draw and before the next draw to that target. Set `KYTY_DUMP_RT_DIR` to the
existing scratch directory for the capture. The draw trace reports the color
attachment index alongside its shader hash and write masks.

## M4 frame timing

Two World Map runs between guest flips 1525 and 1850 measured a median of 8 fps
(7.5 to 8.5 fps across 35 samples). The game still uses 3840×2160 targets in the
scene when Video Out advertises 1080p. In the run with `KYTY_MTL_GPU_TIME=1`,
completed command buffers above the trace threshold of 20 ms reached 65.8 ms
in this interval. This measurement covers a command buffer, which may contain
several passes and need not align with one frame. Set `KYTY_GPU_BUSY=1` to
measure the proportion of wall time occupied by completed Metal work.
With the same default title profile, the 60 Hz and 120 Hz virtual VBlank runs
both used 3840×2160 in the World Map. Over guest flips 1500–1599, the two runs
measured 8.4 and 8.5 fps, respectively, with six samples in each interval.

## World Map recheck with the Prospero video defaults

Launching GT7 1.71 from Prospero on the M4 initialized a 1920×1080 Metal
drawable and advertised the 1080p Video Out mode with a 60 Hz virtual refresh
rate. The game's presented texture remained 3840×2160. The coastline, roads,
tree silhouettes, and parts of a few buildings were visible. The sky and
backdrop were black, the bay remained too dark to distinguish from the terrain,
and the shoreline appeared as a wide pale band. Some header icons rendered,
while their labels and the pavilion labels were missing in the captures.

The navigation replay used `KYTY_AUTO_INPUT_FILE` for Options (`enter`) and
directional movement, and `KYTY_DUMP_TRIGGER` for the corresponding Metal
frames. Left and right input changed which panel was visible. The panels stayed
unlabeled, and Options did not produce a readable GT Menu. The available PS5
frame shows the expected scene and menu; it was used as a visual reference.
