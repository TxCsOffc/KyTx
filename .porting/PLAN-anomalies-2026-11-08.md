# Plan: fix the Metal render anomalies (2026-11-08)

Status of data: user session play1 (fresh profile, build with the RECTLIST fix) with 5 dumped frames + PassTrace of 14026 / 14339 / 14444 / 15233 / 15863, 5 Desktop screenshots, the earlier traces/logs (deleted by the cleanup; findings are in `CODEX-clothes-metal-2026-11-07.md`).
Nothing below is committed. Local only (no FC 27 data may be published).

## 1. Anomaly catalog (what the user sees -> what the traces show)

| # | Symptom | Evidence | Status |
|---|---------|----------|--------|
| A1 | Neon green/red contour stripes, white silhouettes, white-yellow glow blobs in menus (14335 glow, 15229 Match Preview full neon; gone again in the match 15861) | PassTrace: f92 (RG11B10) 1920x1080 surfaces hold millions of NaN/inf that no pass wrote this frame; TAA (guest PS 3741f71a) samples them and writes NaN into its history (RT 0x82483d0000 had 50 % NaN) | OPEN - main target |
| A1a | source 1: transient RT reused with another format | 14339: draw c88c6b9b renders to 7 RTs; RT3 0x8240160000 f92 is loaded (`load=1`) and already has 1.7 M NaN; the same address was an f90 (RGB10A2) RT earlier in the frame (63710..65023 log lines). RTMETA of slot 3: no dcc/cmask/fast clear | OPEN |
| A1b | source 2: second lighting output never written | 1045 / 15233 / 15863: kernels 1691cb86 ... / 10a008ed have 0 groups, their OUT f92 (0x248c40000 / 0x8242190000) holds 100k NaN spread over the frame | OPEN |
| A1c | amplifier: NaN history is never cleaned | TAA RT0/RT1 NaN grows frame to frame (14339: 1.4 M / 3.7 M NaN) | OPEN (decision needed, see 5) |
| A2 | Near-black shadows, no fill light | RECTLIST front_facing inverted -> light list B empty -> classes 4-6 never ran | FIXED, uncommitted (MslEmitter.cpp rect wrapper + `msl-s3`) |
| A3 | Moire/chevron stripes under the blurred dialog backdrop (Desktop 06.12.26, mc41 frame 1841, right before a guest crash) | native-resolution stripes break every ~256x128 px = wrong tile mode / stale guest bytes; T# tile=9 vs tile=27 seen for the TAA input in 1045 | OPEN, probably A1 family |
| A4 | Club crests white / ghosted in the onboarding screens (Desktop 06.37.48: Man City crest = white silhouette, 06.37.58: faded emblem; they load fine a moment later) | not traced yet; texture appears before its data (white default or a GPU-encoded BC level not laid over yet) | OPEN, minor |
| A5 | Banding in the shaded part of the stadium (mc42 frame 2741) | screenshot only | LOW |
| A6 | Guest crash `Unhandled host exception` runtimeLinker.cpp:790 (1 of ~7 runs, mc41) | log | separate track, not render |

## 2. Working hypothesis for A1 (one family)
The game assumes memory it takes from its transient pool is either (a) cleared by its own passes, or (b) fully overwritten. In Metal, render targets are linear textures over the SAME guest memory (UMA wrapped buffers) and compute images are separate textures, so:
1. an RT that appears at an address last used with another format/size is loaded with the old bytes reinterpreted (A1a). The Vulkan reference creates such an image EMPTY (comment in `GraphicsImageFor`: "creates a new image that starts empty ... uploads guest memory only for CPU-written pages") - the oracle semantics to copy;
2. a compute-written image that is only partly written (empty class lists, 0 groups) keeps whatever the memory held (A1b);
3. TAA (and the bloom pyramid) spread the first NaN over the frame (A1c).

## 3. Phases

### Phase 0 - hygiene (before more code)
- Commit the RECTLIST fix on its own (MslEmitter.cpp rect wrapper hunk + metalShader.mm `msl-s3`), after the user agrees; the tree also holds other agents' hunks.
- Add a GPU test for the rect wrapper winding (`tests/metalMslTests.mm`).

### Phase 1 - automatic reproduction + detector (no user needed)
- Recipe: fresh profile, onboarding screens idle at 60 fps (anomaly appeared at flip ~14300 = ~4-5 min), `KYTY_AUTO_INPUT_FILE` presses for the onboarding, periodic `touch /tmp/kytx_f9/pass` every ~60 s (PassTrace flips=2, text only, never the 25-40 GB GPU trace).
- `scripts`: a parser (based on the scans used today) that prints per frame: passes with NaN/inf/huge outputs, first writer, alias history of the address (previous format/size/flip) - output goes to the notes, one line per anomaly.
- Screenshot score (neon fraction + white fraction) only as a cheap alarm, not as proof (grass/UI score high).
- Exit criterion: a script run that reproduces A1 within 10 min without the user.

### Phase 2 - prove the source (instrumentation, env-gated, zero behaviour change)
- `KYTY_LOG_ALIAS_LOAD=1`: when a render pass loads an RT whose address was last written with a different format/size, print address, old/new format, old/new flip, guest CPU-write flag.
- `KYTY_LOG_PARTIAL_CS=1`: for each compute pass writing an image, print covered fraction of the surface (from the dispatch group counts) and whether the previous flip's contents are kept.
- Read back: do the NaN surfaces of 14339/15233/15863 all show up in these logs? (A1a vs A1b split.)

### Phase 3 - fixes, one per source, each behind an env switch for A/B, default on after verification
- F1 (A1a): first use of an RT (or storage image) under a new layout at an address -> new empty surface (clear to 0) unless the guest CPU wrote the pages since; keep the same-texel-size reinterpretation for intentional packing (R32Uint <-> RG11B10 kernels, see ReinterpretFormat). Files: render target lookup / `NewestRenderTargetFor`, `GraphicsImageFor`, the pass begin (loadAction).
- F2 (A1b): make a partly written compute image behave like the PS5 memory the game expects: zero-initialise compute-written images on creation and when the guest's clear-after-use tile kernel (the 32x1x1 pixel-list kernel, lib45 in the traces) has nothing to clear; verify against Vulkan clean frames where the second output is black except skin.
- F3 (A3/A4 candidates): check the tile=9 vs 27 T# case in `GraphicsImageFor` (compute-written image must win over a guest-memory decode when address/size/format match, regardless of the tile mode); for crests run `KYTY_LOG_GUEST_DECODES=1`/`KYTY_GUEST_TEX_VERIFY=1` at the onboarding screens.
- Verification per fix: PassTrace NaN/inf count of the 5 stored traces' scenarios drops to 0 outside TAA; TAA history NaN stays 0 for a 10 min idle run; screenshots of the same scene clean; FPS unchanged (30 / 60 in menus).

### Phase 4 - TAA robustness (needs a user decision)
- Option A: faithful only (no sanitising). Option B: opt-in `KYTY_TAA_SANITIZE=1` flushing NaN/inf of the TAA history/output to 0 as a safety net. B hides the source of a bug (the user's standing rule: no hide hacks), so it stays off unless the user asks.

### Phase 5 - regression harness
- After each fix: idle run + the user's flow (onboarding -> Kick Off preview -> match), PassTrace table (NaN passes per frame) + screenshot sheet, saved in the notes; keep one tag per run.

## 4. Order and expected effect
1. Phase 1 harness (needed to measure anything), 2. Phase 2 logs, 3. F1 (probably removes A1a and most of A3), 4. F2, 5. F3 / A4, 6. A5/A6 later.

## 5. Decisions needed from the user
1. OK to commit the RECTLIST fix separately now?
2. TAA sanitising: keep faithful (default) or allow the opt-in safety switch?
3. Is it OK that I run idle sessions (several minutes, one instance) myself while the user is away from the game?

## 6. Safety notes
- GPU traces are 25-40 GB each and filled the disk once (ENOSPC stopped even shell commands); text PassTrace + dumps only, check `df` first.
- `~/Developer/fex-spike/runs` was wiped by the user's cleanup: fresh save, cold shader cache (first launch slow); the old "restore DATA from the backup" recipe is gone.
