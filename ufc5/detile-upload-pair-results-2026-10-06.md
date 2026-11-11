# Depth/stencil upload pair investigation — 2026-10-06

## Coverage

The saved application trace's epoch 9339 / tick 2794298 has a 51.428320 ms
GPU submission bracket. It contains detiles at `0x1168bd0000` (3840×2160,
four-byte elements, 33,423,360-byte output) and `0x116abb0000` (same extent,
one-byte elements, 8,847,360-byte output). Both uploads' resource-ready scopes
identify native image handle `0x1006d571ee0`; these are a depth/stencil pair.

The existing default replay already calls production `TileManager::Detile`, so
its timings include the initial barriers, target fill, dispatch and final barrier.
Describing that replay as timing just the isolated dispatch would be incorrect.

The new `--detile-upload-pair` mode uses the captured inputs/layouts, production
`Detile` and production `Image::Upload` for both transactions into one D32S8 image,
with one submission and no host wait between transactions. Five GPU timestamps
export elapsed detile/upload brackets and batch total; CPU recording and combined
flush/wait are separate. Both image aspects are read back after measurement and
all active texel bytes compare against the saved references. Padding is excluded
from image checks, but remains covered by default full-buffer replay.

**This is a constructed transaction pair, not an exact production batch replay.**
KDR v1 does not capture native image format, prior image contents/layout state,
guest draw/compute commands, native queue dependencies or whole-game residency.
D32S8 is an explicit assumption. The saved batch also contains a draw and guest
compute work that this reproducer omits. Saved payloads were captured on October 5,
not simultaneously with the October 6 correlated trace.

## Verification and live measurements

Release headless target builds successfully. Executable SHA256:
`16D1F6D6EB37E19AC535BD44E198624FDA802A8FB3BBAE22FF88D4B6E9992B53`.
No production renderer source or running emulator binary changed. PID 7600
remained responsive; live controls remained `off` / `off all`.

With the game still running, three warmups and ten measured pair iterations:

- GPU batch median **2.739 ms**, range **2.492–6.577 ms**.
- Final depth image **MATCH**, stencil image **MATCH**.
- CSV: `D:/PS5/ufc5-profiles/upload-pair-20261006-live.csv`.

The first two measured iterations have depth detile brackets 3.606/3.844 ms;
later ones are 0.246–0.271 ms. This is already a variable concurrent workload,
not an uncontended benchmark or proof of an FPS improvement.

The original default replay also ran, three warmups / ten measured iterations,
with the game running. **All eight full output buffers MATCH.**

| Guest address | Current concurrent GPU median (ms) | Prior game-closed median (ms, Oct 5) |
|---|---:|---:|
| `0x1164920000` | 6.666 | 0.082 |
| `0x11673b0000` | 34.406 | 0.154 |
| `0x1167460000` | 13.279 | 0.175 |
| `0x11687e0000` | 6.673 | 0.084 |
| `0x1168bd0000` | 20.434 | 0.250 |
| `0x11691a0000` | 0.081 | 0.083 |
| `0x116abb0000` | 0.424 | 0.477 |
| `0x1172520000` | 10.906 | 0.175 |

CSV: `D:/PS5/ufc5-profiles/detile-control-20261006-live.csv`. Prior baseline
files remain `D:/PS5/ufc5-detile-replay-baseline-20261005-a.csv` and `-b.csv`.
These historical baselines have different warmup/iteration counts and an earlier
build. The same-build game-closed repeat below avoids those build/setting differences.

Invalid four-byte depth supplied as the stencil input is rejected with exit 1,
before allocating replay buffers/images. CLI wrapper parses and writes metadata
with build hash, settings and emulator process snapshots before/after each run.
Metadata support was added after the two live timings above; no retrospective
start/end metadata is claimed for those two files.

## Same-build game-closed comparison

User closed Kyty and authorized testing. Both new runs use the exact executable
SHA above and the same three warmups / ten measured iterations as the live runs.
Metadata records no emulator process before or after either closed run. No rebuild,
renderer change, source payload change or emulator restart occurred between them.
Default replay verifies every reference byte, with identical hashes in live/closed
CSVs; the upload pair verifies both final image aspects.

| Guest address | Game running median (ms) | Game closed median (ms) |
|---|---:|---:|
| `0x1164920000` | 6.666 | 0.084624 |
| `0x11673b0000` | 34.406 | 0.154032 |
| `0x1167460000` | 13.279 | 0.175520 |
| `0x11687e0000` | 6.673 | 0.085440 |
| `0x1168bd0000` | 20.434 | 0.249776 |
| `0x11691a0000` | 0.081456 | 0.082608 |
| `0x116abb0000` | 0.424224 | 0.477552 |
| `0x1172520000` | 10.906 | 0.174912 |

**All eight closed medians are below 0.5 ms.** Six formerly slow cases differ by
62–223×; two cases were already fast with the game running. This is a sequential
condition comparison, not a randomized off/on/off experiment. It demonstrates
that the same executable and captures can run quickly without the live game;
it does not independently identify what running the game changes.

The constructed upload pair is **2.803455 ms** closed (range 2.80256–3.02838),
versus **2.739 ms** live. No clear pair speedup is demonstrated by these medians.
Closed per-stage medians:

| Elapsed bracket | Median (ms) |
|---|---:|
| Depth detile including clear/barriers | 0.249104 |
| Depth image upload including transitions | 1.937135 |
| Stencil detile including clear/barriers | 0.477984 |
| Stencil image upload including transitions | 0.138496 |
| Whole pair | 2.803455 |
| CPU command recording | 0.025500 |
| CPU combined flush/wait | 2.919550 |
| CPU wall span | 2.947150 |

Per-category medians are not additive, and the CPU spans overlap the GPU work.
Timestamp scopes are elapsed brackets, not pure execution. Final depth and stencil
image checks both **MATCH**. The complete modeled pair still does not reproduce
the production batch's **51.428320 ms** GPU bracket.

Artifacts in `D:/PS5/ufc5-profiles/`:

- `detile-control-20261006-closed.csv` and `.csv.meta.json`.
- `upload-pair-20261006-closed.csv` and `.csv.meta.json`.
- `detile-upload-comparison-20261006.json`: exact medians, ratios, verification and limitations.

## What follows from the measurements

1. The clear/barriers/detile/upload pair **can complete much faster** than the saved
   slow production submission. That pair alone has not reproduced its ~51 ms span.
2. The default standalone replay **can also exhibit large elapsed-time inflation
   while the game runs**, even in a separate process without the guest command
   stream or image upload. This undermines a diagnosis based solely on the tiler
   shader's isolated baseline or guest queue dependencies. Same-build closed
   repeats return all eight medians below 0.5 ms. This does not identify
   whether scheduling, contention, residency, clocks or another external condition
   causes that inflation. No pure shader execution claim is made from TOP→BOTTOM.
3. Different captures and the pair vary substantially in the concurrent runs.
   Neither a single fast pair nor a slow default replay is representative proof
   of production frame time. The user's earlier lower-resolution/VRAM experiment
   remains evidence against VRAM pressure being the primary FPS cause.
4. The prior correlated report still establishes late readback completion behind
   earlier work; its 98.46% before-DMA fraction is not a measurement of copy cost.

## Next steps

- The same-build closed comparison is complete; the game remains closed.
- Next, capture native scheduling/residency evidence while the fixed standalone
  replay runs alongside a constant paused fight. Record the replay process/thread
  and submit bounds so native packets can be joined, as was done for the game's
  readbacks. A separate replay has no guest command stream: tracing its inflated
  intervals can distinguish an external scheduling/residency problem from waits
  local to the production transaction. Do not infer a mechanism or undo
  synchronization from the live/closed contrast alone.
- For an exact batch reproducer, capture native image format/state, copy regions,
  guest compute/draw inputs and cross-submission dependencies. The current pair is
  a bounded test that works now without another game boot; it cannot replace that
  missing capture coverage.

No optimization, graphics output change or synchronization change was introduced.
