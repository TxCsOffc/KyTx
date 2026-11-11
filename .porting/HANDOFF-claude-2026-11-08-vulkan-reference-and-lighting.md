# Handoff for Codex, 2026-11-08 (Claude, account limit almost used up)

Read this first, then `CODEX-clothes-metal-2026-11-07.md` (chronological log of everything below, much more detail, last sections newest).

## 0. The task the user chose

The Metal product renders FC 27 menu scenes with lighting that differs from a real PS5 (see section 6). To find which lighting/shadow term is wrong
we need the SAME scene rendered by both the native Metal build and the Vulkan reference, pass by pass. The Vulkan reference renders shadows and
lighting correctly (its saved GPU traces match the PS5 frames), but it goes into NaN / flickering / white screens after a few hundred flips, so it
cannot be used in a live run. User decision (their words): "if we need identical scenes, fix the Vulkan build, because even with the flicker and white
screens it renders shadows and lighting correctly". So:

1. **Make the Vulkan reference stable (no NaN storm, no white screen)** - section 4.
2. Use it (or its saved traces) as the oracle for the Metal lighting/shadow differences - section 6/7.
3. Fix the Metal product accordingly (never put Vulkan into the product).

Fallback if (1) stalls: capture ONE Metal GPU trace (`.porting/work/ftrace.sh <label>`) of a scene that already exists as a Vulkan trace
(`vk_f1635` = Paris street, white tracksuit; `vk_f1487` = Roma studio) and compare offline with `gpudebug` - section 5.

## 1. Rules from the user (do not break)

- NO emulator runs unless the user says so. At the moment of writing: runs were paused ("I will tell you when"). Ask/announce before any run.
- One emulator instance at a time (two at once caused a WindowServer watchdog kernel panic). Never `pkill` by name; start the run, note its PID
  (`pgrep -x kyty_emulator_s3` / `kyty_vkref`), stop only your own PID (`kill`, `kill -9` only for a process hung in a shader compile).
- The user closes the Vulkan reference windows themselves: no timer kills for the reference. For Metal runs a time limit of your own run is fine.
- No Vulkan/MoltenVK in the product. The reference lives outside `~/Developer/KyTx` (`~/Developer/KyTx-vkref*`, Mesa in `~/Developer/vkref-mesa`).
- Never commit unless asked; NO "Co-Authored-By" AI trailers; never publish FC 27 data (screenshots, logs, dumps, generated MSL) - `.porting/`
  stays local. Never touch a user's gpudebug session you did not create.
- No skip-shader / hide-the-body hacks. Do not bypass the classifier-blocked `rm -rf` on `_SaveData` (copy the single differing file instead, below).
- Restore the clean game state before each comparison run: `cp ~/Developer/fex-spike/runs/_SaveData.claude-20261103-2225/PPSA34015/Settings20261017084726/DATA ~/Developer/fex-spike/runs/_SaveData/PPSA34015/Settings20261017084726/DATA` and check `diff -rq _SaveData _SaveData.claude-20261103-2225`.
- The `CLAUDE.md` files in KyTx and KyTx-vkref-src are STALE (black-screen task, wrong build target). Real target: `kyty_emulator`.
- The FC NEWS cards/text in menu screenshots are online content: ignore them.

## 2. Repo state

Branch `metal-native-checkpoint` of `~/Developer/KyTx` (local only, nothing pushed):
- `945d467d` MSL dispatcher fallback takes `s_setpc_b64` jump tables (fixes skin through street clothes). Confirmed in two runs.
- `9534b337` Metal presenter: bilinear on-screen present + drawable sized to the window pixels (fixes the wobbly FC 27 logo/UI edges; the cause was
  a point `read()` at a 1728x957 drawable). Opt-outs `KYTY_PRESENT_NEAREST=1`, `KYTY_PRESENT_NATIVE_SIZE=0`.

UNCOMMITTED in the working tree (mine are opt-in diagnostics, mixed with other agents' edits in the same files - split by hunk, do not `git add .`):
- `metalRenderer.mm`: `KYTY_PASS_TRACE_NANMAP=<dir>` (PGM NaN masks, max 80) and `nan_bbox/nan_row0/nan_top8` in PassTrace image stats; CSBUF lines now carry
  `nz=` and `hi_even=/hi_odd=` (top-nibble histograms) for the whole buffer; `KYTY_LOG_RT_SAMPLES=1`; `KYTY_LOG_SAMPLERS=1`; `GdsBlit` now ordered with
  OrderBegin/OrderEnd (`KYTY_GDS_BLIT_UNORDERED=1` restores); `KYTY_PASS_TRACE_CS_N/_FROM`; `args@... now=` in INDIRECT lines; older diagnostics.
- `MslEmitter.cpp`: `KYTY_MSL_VALUE_AT`, pixel-stage `KYTY_MSL_NAN_TRACE` buffer (`kyty_nan_dbg`, buffer 28).
- Others' changes (Codex etc.): `metalAddressSpace.mm`, `metalBuffer.*`, `traceWindow.h`, `window.cpp`, `kernel/*`, `fex_guest_cpu.*`, `tests/metalMslTests.mm`, ALU hunks in MslEmitter.cpp.
Build the product: `cmake --build /Users/tx/Developer/KyTx/_Build/macos-arm64 --target kyty_emulator -j6` (last build includes all of the above).

Vulkan reference (`~/Developer/KyTx-vkref-src`, a plain copy, NOT a git repo; installed binary `~/Developer/KyTx-vkref/kyty_vkref`, backups `kyty_vkref.before-*`):
my changes are all diagnostic/opt-in, saved as patches in `.porting/artifacts/2026-11-08-claude-vkref/` (README there). Build: `cd ~/Developer/KyTx-vkref-src && cmake --build _Build/macos-arm64 --target kyty_emulator -j6`, then `cp _Build/macos-arm64/kyty_emulator ~/Developer/KyTx-vkref/kyty_vkref.new && mv ... kyty_vkref` (copy the previous one to `.before-<what>` first; never replace the binary while it runs).
Mesa/KosmicKrisp clone: `~/Developer/vkref-mesa/src` (two uncommitted patches, saved as `mesa-kosmickrisp.patch`); rebuild with
`cd ~/Developer/vkref-mesa && PATH=/opt/homebrew/opt/llvm/bin:/Users/tx/Developer/vkref-mesa/venv/bin:$PATH ninja -C build`.
The reference is launched with `open -n ~/Developer/KyTx-vkref/KyTxVkRef.app`; the launcher sources `~/Developer/KyTx-vkref/env.sh` (default content = `RUN_TAG=kk10` + KosmicKrisp ICD paths + `MTL_HUD_ENABLED=1 KYTY_VK_NO_PORTABILITY_SUBSET=1`). Logs: `~/Developer/fex-spike/runs/validation-20261025/lean/<RUN_TAG>-<time>.log`.

## 3. Tools (all new ones are in `.porting/work/`)

- `vk_timeline.sh <tag> [KEY=VALUE ...]`: one Vulkan reference run that triggers a 2-flip PassTrace every 8 s from t=10 s (so the NaN onset is caught), no timer kill, prints the PID, restores env.sh. Written but NEVER RUN (the equivalent scripts `runvk2..12.sh` were used all day).
- `pt_summary.py <log> [--lighting]`: per traced frame NaN summary (first NaN pass, bbox, max) and the indirect group counts of the lighting kernels. Works on Vulkan and Metal logs.
- `shot_window.sh <KyTxMetalS1|KyTxVkRef> <out.png>`: window-only screenshot.
- Metal run: edit `.porting/work/extra_env_s1.sh` (env of the run; must contain `RUN_TAG=<tag>`), then `.porting/work/run1.sh <tag> <max-seconds>` (0 = no limit; refuses if another instance runs; stops by PID, also past 26 GB footprint). Auto input: `KYTY_AUTO_INPUT=1500:j:8,1850:j:8` (flip:key:hold; keys `up/down/left/right/enter` or one char, `j` confirms), `KYTY_AUTO_INPUT_FILE=<file>` takes "down:6 j:8" while running. The `1500..3300` recipe only cycles menu backgrounds, it does NOT reach a match (5 x down = KICK OFF, then confirms; untested).
- PassTrace: `KYTY_PASS_TRACE_FRAME=<flip> KYTY_PASS_TRACE_FLIPS=<n> KYTY_PASS_TRACE_PIXEL=u,v/u,v` (fractions) on Metal; on Vulkan a trigger file `/tmp/kytx_f9/pass` (touch it). Vulkan lines now show `grp=` = the indirect args the GPU read, `nan_bbox=`; `KYTY_VK_TRACE_CS_BUFS=<guest hash,..>` dumps bound buffers (CSBUF); `KYTY_LOG_SAMPLERS=1` on both. Native frame dump: `KYTY_DUMP_FRAME=<flips> KYTY_DUMP_PATH=<dir>/frame-%llu.ppm`.
- `gpudebug` (Xcode GPU traces, no emulator needed): `gpudebug -t <trace>.gputrace -q --timeout 1800` prints a session id; `gpudebug -s <id> -c 'status' -c 'wait'`, `-c 'find present'`, `-c 'go commands/cbN/re0/draw0' -c 'list'`, `-o <dir> -c 'fetch commands/cbN/re0/draw0/color0'` (final image, PNG under `/private/var/folders/.../gpudebug_out/<trace>/`), `-c 'go resources/textures' -c 'list --all'`, `fetch resources/textures/texNNN`. Terminate your sessions (`gpudebug --terminate <id>`): each holds a 10-37 GB trace in memory. Traces: `/tmp/kytx_f9/gputrace/` (may vanish on reboot).

## 4. The Vulkan reference white screen / NaN: what is known

Symptom: from flip ~370-740 on (depending on run) NaN appears in HDR images (R11G11B10 `f92`, R32F, RG16F), grows in ~15 flips to the whole frame (1446400 NaN = 1600x904 texels at flip 753), sometimes disappears for a few hundred flips, comes back; the window turns white (vk1: all white at flip 3042). Feedback loop: TAA output (PS guest `3741f71a`) -> history input of kernel `0e838ae6` -> lit scene -> TAA. The NaN comes in tile-aligned rectangles written by the lighting/tile-list compute kernels (`1691cb86`, `0e838ae6`, `6c910990` depth pyramid, `af85c98e`), e.g. a 32x32 block in a 1024x1024 R32F image, a 64x112-pixel band in a 1920x1080 R11G11B10, a right-edge rectangle in a 480x270 image. The FIRST pass with NaN differs per run (sometimes a G-buffer draw of PS `55e3fa72`, sometimes `0e838ae6` itself with all earlier outputs clean, kernel `29893364` shows NaN only as stale content: it runs with 0 groups in both backends).
The native Metal build at the same flips has no NaN (mc24, flip 738: 2900 passes, zero) - EXCEPT see section 8.

Ruled out by A/B runs (each one still NaN/white): KosmicKrisp fast-math (`KK_MATH=safe` patch); MoltenVK instead of KosmicKrisp (same NaN); zero-clearing every new colour image at creation (`KYTY_VK_ZERO_NEW_IMAGES=1`, 5601 clears/600 flips); `KYTY_DISPATCH_OVERLAP=0` (host barriers between direct dispatches; NaN already at flip 725 with traces from t=10 s); tile class table content (it is zero or filled depending on the scene, NaN present in both states); NaN counts being whole 4 KiB pages (only 3 % are). SPIR-V min/max/med3 are bit-exact RDNA (non-NaN operand wins), `SignedZeroInfNanPreserve` is declared. `KYTY_COLOR_META_CPU_FILL=1` aborts at start in the reference ("proven compute fill requires aligned guest pages") so it cannot be used as an A/B.
Facts worth knowing: on Apple the reference defaults to `KYTY_DISPATCH_OVERLAP` ON for direct dispatches (no host barriers between them; indirect dispatches always get one); the MSL of `0e838ae6` clamps its colour with `fmin(max,x)`/`fmax(0,x)` (RDNA semantic), so NaN cannot come out of its store when min/max work - the NaN texels are probably stale content of tiles nobody wrote this frame, i.e. the question is why those tiles are not covered / read garbage.

Leads not tested yet (most promising first):
1. RT metadata (DCC/CMASK) clear paths. The native build has `RegisterMetaClearRt`/`NoteUniformMetadataFill` (metalRenderer.mm ~l.380-440): a fill of DCC metadata that happens BEFORE the target is registered used to leave RGBA16F targets with NaN/inf from reused memory (a "white bloom blob"), fixed there. The reference does it on the CPU in `TextureCache::MaterializeColorClear` (reads guest metadata bytes, only `levels==1`, expands the code to 0xff, `KYTY_CMASK_SKIP_IMPOSSIBLE` off by default). Compare the two for the same sequence; `KYTY_COLOR_META_TRACE=<csv>` logs its operations (R/P/T lines).
2. Transient memory aliasing: the game reuses one address as depth / half-res image / HDR target in one frame (`ResolveOverlap`, `ExpandImage`, `EnsureUsage` copies contents between different formats; compare with Metal's alias handling `metalRenderer.mm` ~l.1020).
3. Buffer-cache coherence of GPU-written data the lighting kernels read (tile counters, `df0a2346` lists).
4. A first-NaN-pass bisect inside one frame: with `KYTY_VK_TRACE_CS_BUFS` for the kernel that first shows NaN, compare its input buffers/images with the native build's `CSBUF` of the same kernel in the same scene.
Method that worked: run `vk_timeline.sh` (early traces), `pt_summary.py` to see onset and first NaN pass, then dump that kernel's buffers on both backends. Compare INSIDE one run or the SAME scene only: the two backends pick different shader variants at different dynamic resolutions (Metal 1920x1080 vs Vulkan 1648x928), so kernel-by-kernel comparison across frames is unsafe.

## 5. Reference data (scenes you can compare)

PS5 reference frames (video captures) on the user's Desktop: `Screenshot 2026-11-08 at 00.20.30.png` (England studio, Learn to Play), `00.21.14` (The Grounds, Real Madrid player), `00.22.28` (street "Super Libre", red tracksuit), `00.22.41` (UK cobble street, grey sweatpants), `00.23.11` (Paris street, white tracksuit; `... 1.png` is a duplicate). Game area = y 103..1228 of the 2000-wide display (x1.728 for the 3456 px original).
Saved GPU traces (`/tmp/kytx_f9/gputrace/`): Vulkan `vk_f1487` = Roma STUDIO (floor in shadow with the sunlit window-shaped patch = PS5), Vulkan `vk_f1635` = PARIS street, white tracksuit (= PS5 "paris", ground evenly lit), Vulkan `vk_f77` = flip 77; Metal `frame1885` = black transition frame, `frame2399` = UK COBBLE street (ground almost black, PS5 "cobble" is sunlit), `frame8492` = the old white frame (not opened).
Checked: the BC6H environment textures (4096x1024, 512x512, 256x512) are byte-identical in `vk_f1635` and `frame2399`, so the sky/IBL inputs are not the cause.

## 6. Metal lighting vs PS5 (what to fix after the oracle exists)

Luminance medians Metal/PS5 (framing differs, indicative): studio floor 2.7x BRIGHTER (PS5 floor is dark carpet in shadow with one sunlit patch; Metal lights the whole floor evenly, window/wall shadow missing), studio walls 0.54x, The Grounds pitch 0.64x, red street ground 0.84x, cobble ground 0.11x, Paris ground 0.63x but sunlit facade 1.75x. Pattern: Metal is HARSHER: near-black shadows, brighter and more saturated lit parts; PS5 shadows keep a grey sky/bounce fill and are soft. The Grounds and the red-tracksuit street are close. Street cast shadows exist in Metal (sun shadow map works there: 3072x3072 and 2048x2048 depth maps are rendered, 3072 is read by all lighting kernels).
Candidates: weak ambient/indirect (probe/volume/SSAO/SSR term), a missing static/baked shadow or light-volume input in the studio, exposure. Decisive test: same scene (studio or Paris), per pass at a floor/ground pixel: G-buffer albedo -> shadow factor -> lit scene (`0e838ae6` output `OUT r14` etc.) -> TAA -> tonemap, Vulkan vs Metal.

Lighting chain (guest hashes; identical order in both backends): `bb1415e0` (copies a 128-byte template into the counter blocks) x9 -> `df0a2346` x8 (per-cell classifier, 8x8 threads, one entry per cell; reads params b0 (width, height, increment, bit index, shift), per-cell 32-bit mask b1 (tests bit `b0[3]&31`), 8-byte class table b2 (class = top nibbles of both dwords), `atomic_fetch_add` into the per-class counter b3 which IS the indirect-args record, packed (x<<16|y) into list b4) -> `3b4f635e` -> `0e838ae6` (history input r8 = previous TAA output; lit scene output) -> lighting kernels in families of six (`1691cb86 45ff7d9d 661481cd 77b2d6ce a4c06b45 28e424b7`, `daa8c31f 79babc35 b4459214 ...`, `27ac3024 052a787a c95a37c2 ...`, `15ef8445 2168dc94 ff98d6cd ...`, `cc985cd0 a2b910bd de124eb7 ...`, `9a3f27ce e7ba654f b4fb64cc ...`; group counts come from the counters) -> TAA PS `3741f71a` -> tonemap/compose compute `b3300709` (30x1080 groups). Metal sees only classes 0-2 of every family (3-5 get 0 groups) in the street frames; whether that is right is unknown (the class table is scene dependent).

## 7. Other open items

- Match-start guest crashes (~half of the runs that enter a match; cl611/612/614; see memory/notes): untouched; needs a reproducible way into a match (above).
- White-frame risk in Metal: see section 8.
- Dynamic resolution, FPS: median 29.5-30 FPS in menus.
- `KYTY_LOG_RT_SAMPLES` showed 0 MSAA colour targets in 80 000 binds: the game uses its own TAA, the user decided "AA = TAA, no MSAA".

## 8. Found at the very end, not investigated

Metal run `mc25` (log `fex-spike/runs/validation-20261025/lean/mc25-225340.log`, PassTrace of flip 1684 with CSBUF dumps): the TAA draw (PS `3741f71a`, draw n=467) shows `RT1 0x824b000000 1920x1080 f92 nan=3152245 ... nan_bbox=(0,289)-(1919,1079)`, i.e. the lower ~73 % of the history target is NaN in a NATIVE run (`python3 -I .porting/work/pt_summary.py <log>`). Other traced Metal frames were clean (mc24 flip 738: 0 NaN; mc21 flip 1800: ~160 NaN, stable). It may be an artefact of tracing/waiting (the traced frame commits and waits per pass) or the Metal side of the same bug. Check with window screenshots of an untraced run around that flip before dismissing it (the user says Metal never shows a white screen).

## 9. Index of logs (all under `~/Developer/fex-spike/runs/validation-20261025/lean/`, names `<tag>-<time>.log`)

Vulkan: vk1 (white screen at flip 3042 + PassTrace 3135), vk2 (timeline from flip 738), vk3 (KK_MATH=safe), vk4 (MoltenVK), vk5 (zero-new), vk6/vk9 (overlap off; vk9 has early traces), vk7/vk8 (real indirect args, nan_bbox), vk11 (CSBUF of df0a2346 + samplers). Metal: mc12/mc21 (lit-scene NaN, flip 1800), mc24 (flip 738 clean), mc25 (CSBUF, section 8), mc26-mc32 (splash/presenter), mc33 (street, flip 2500 trace with ground probes), mc34 (9 min menu run, no crash, no match). Scene images from today were in a temporary scratchpad and are not kept; regenerate with `KYTY_DUMP_FRAME`.
