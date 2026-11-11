# Canonical color targets, storage provenance and bounded writeback — 2026-11-10

## Result and boundaries

Implemented three related native Metal memory behaviors: physical provenance for canonical storage-image writers, canonical tiled memory for single-level color RTs, and scissor-bounded export that preserves guest bytes outside possible color writes. Removed viewport/default guesses for CB dimensions: zero register fields mean1x1. No shader skip, blank image, substituted varying, loop/work cap or error suppression was added. Render pipeline labels now identify the actual native VS/PS pair for diagnostics.

Final build SHA256: `d859298f01755226ae00dae88a1e5cc076a6441e19e1a9482c87c9e845a2002d`. Repository `/Users/tx/Developer/KyTx`, branch `metal-native-checkpoint`, HEAD `9534b33718bd10104a4baf708758d91f61e2ef8d`. Existing dirty/untracked work retained, no staging/commit/push/reset. Ledger CLI unavailable; STATE/HANDOVER/worklog updated instead. Workspace evidence is in `work/metal-storage-provenance`, separate from the preceding `metal-sampled-remap` artifacts.

The final game materialized both previously rejected remapped readers,3840x1080 and1920x1080. It then explicitly rejected a stale3D image of another format. It also reproduced the previously identified GPU failure before these lookups, so later CPU progress is not evidence of successful GPU execution. No3D scene or shadow/clothing/lighting/hair/neon/upscale fix has been validated.

## Storage-image provenance

SR8 showed that the last writer at the reused VA was a sequence of compute mip writes following the2048x2048 color RT. These were already canonical writes with queued tiled writeback, but only RT history retained its physical backing. Their newer texture/serial therefore blocked remap retirement.

`CanonicalMipBinding` now carries the exact physical span snapshot captured for its import/writeback. Compute and graphics storage bindings pass that snapshot to `RecordComputeStorage`, which replaces it on every writer change. Canonical color-array writers are recorded as well. The retirement proof accepts a newer canonical storage writer only when its recorded allocation changed. Current backing, unknown producer, missing backing, and newer private writer remain distinct rejection cases. Pointer reuse with a changed serial cannot establish authority. Color RT history still has to pass its own backing checks.

Full coherence test under Metal API+GPU validation passes:8 physical identity controls and13 writer controls,206,700 verified bytes, including actual image store -> tiled writeback -> separate reader in one command buffer and CPU/raw GPU updates. This verifies the memory behavior independently of the game's failed GPU stream.

## Single-level RTs and real dimensions

The SP3 failure retained a2048x2048 RGBA8 target with only one level and `canonical=0`. Previously, a tiled RT used canonical memory only if it had mips or overlapped an already registered canonical range. This let ordinary single-level targets use the old linear path.

All tiled color RTs now use the existing physical tiled load/store adapter from their first pass. Linear targets keep their linear path. CB dimensions are exactly `attrib2.width+1` and `attrib2.height+1`; neither the viewport nor a1920x1080 default replaces a valid1x1 dimension. Vulkan's local `colorRenderTarget.cpp` also decodes these registers as width/height minus one, but native execution and GPU tests establish the implemented behavior here.

## Preserve memory outside scissor regions

The new partial draw regression failed before the fix: packed R11G11B10Float payloads changed outside scissor, for example `fdba7734 -> fc3a7734`. A typed Metal render pass can canonicalize NaN representations in its tile storage. Exporting the entire texture copied those changes over guest bytes that could not have been written by the draw.

Added rectangular raw-element GPU export, with independent texture coordinates and guest tail coordinates, plus exact physical rectangle footprints. The draw executor collects a conservative union of active scissor rectangles for each encoder, including all16 possible viewports. Contained duplicate rectangles are removed without imposing a work cap. All direct/indexed/indirect/mesh routes participate through the shared draw-writeback boundary. A clear covers the full attachment; a load/store pass with no draws has no color export. Fresh shared region objects belong to each encoder, so queued callbacks remain valid through pass closure/reopen. Canonical callbacks scatter only those physical byte ranges.

The original byte comparison remains strict, including arbitrary packed floats, NaN payloads, untouched pixels, tile padding and guards. The failing partial draw now passes. This is a conservative scissor bound, not an exact per-fragment/depth-pass coverage mask: payload preservation in geometry holes inside a possible write rectangle is not established by this change. Legacy noncanonical producer interop and broader lazy coherence remain open too.

## Verification

- Final full coherence/API+GPU validation PASS: `/Users/tx/Developer/kytx-region-coherence-final.log`.
- New rectangle test:3 GPU cases,12 invalid rectangle controls,5,901,312 exact bytes. Includes base mip, shared tail,1x1 tail, array-layer XOR and discontiguous physical chunks. GPU raw writes occur after gather; export/scatter preserves newer raw bytes outside the rectangle without CPU waits/readbacks between producer and consumer.
- Final full pipeline/API+GPU validation PASS: `/Users/tx/Developer/kytx-region-pipeline-final.log`.
- Color pass tests:36 load/store/reopen/clear/partial draw cases,3,557,376 physical bytes, zero failures. Covers RGBA8, RG11B10Float and RGBA16Float; single-level97x53 and1x1 targets as well as mip chains. Existing graphics storage, MRT, viewport, stencil, indirect/MDI, mesh, depth memory and bounds suites remain green.
- Final product build PASS: `/Users/tx/Developer/kytx-region-build-final.log`, two inherited warnings. Shader emitter source did not change; full MSL/CFG not repeated. The historical Apple clip-guard CFG failure remains open.
- Initial partial draw failure is preserved in `kytx-tiled-partial-diagnostic.log`; it is not counted as PASS. Subsequent region implementation fixed it without relaxing the expected bytes.

## GPU failure attribution and validation limitation

After the user confirmed unlock, SR8 PID13951 had already naturally aborted. Its log identified a faulted render encoder (`MTLCommandEncoderErrorStateFaulted=4`, checked in the installed Apple SDK), not merely an affected/discarded command buffer:

```
draw #877350 rt0=0x8241cd0000
VS native1d58ae6f8ed54b0c guest a8092251af057bb6
PS nativebaf34dd57c65d299 guest dec48a1373748c4f
RECTLIST, R11G11B10Float
```

SP3 and final SP4 reproduced that pair. This identifies an encoder and its mesh/fragment pair; it does not yet isolate which shader stage, input, generated operation or driver behavior caused the hang. Exact metal/IR/JSON/SPV pairs are saved in `first-fault-pair`. Do not cap its loops or replace its output.

SP1/SP2 attempted selective Shader Validation using native pipeline labels. Both aborted earlier on a compute function with guest LDS4608 dwords. The standalone probe compiles the exact captured MSL and reports18,432 static bytes normally but36,864 with the Shader Validation layer, even with default state none, explicit disabled pipeline, threadgroup checking off, and maximum-thread settings0/32/64/128/256/512. The diagnostic exceeds the device's32KB dispatch check. No guest LDS was truncated and no thread-count cap was added. SP1/SP2 are diagnostic failures, not game validation. Probe sources and logs are preserved. [Apple's selective validation documentation](https://developer.apple.com/documentation/xcode/validating-your-apps-metal-shader-usage) informed the attempt; the observed limitation is from the local experiment.

## Final game and next concrete work

SP4 PID18305 started18:41:53+05 with the final SHA above, capture/API/shader validation layers off and existing fault labels enabled. Log:`/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/sp4-184152.log`. CUA observed a black frame291 after the GPU failure; no valid menu/match image. It naturally aborted later, not timer/signal stopped. Previous SP3 showed EA message frame325, one manual500ms j, then loading frame386. No automatic input loop. Original40GB guard and byte-for-byte environment restoration retained.

SP4 logs:

```
[RemappedImage] addr=8112000000 type=9 format=56 size=3840x1080 bytes=17694720
[RemappedImage] addr=8122800000 type=9 format=56 size=1920x1080 bytes=8847360
[ImageFormatReject] shader=1211e65193f45e32 res=1 addr=8244b10000 200x113 tile=9 via=guest storage=1
compute image format differs from descriptor: guest13 / actualMetal92 / expectedMetal25
```

This is the3D path: actual cached RG11B10Float versus requested R16Float. `GuestTexture3D` still caches by VA alone and retains private GPU-written volumes; the log also binds a64x64 volume to a200x113 descriptor. The next implementation needs physical volume ownership, exact shape/format/mip identity and GPU volume import/export through the already available thick-tile kernels. A key-only replacement or a blank new volume would not reconcile the existing GPU data. Requested depth was not printed in this rejection, so do not infer it.

The earlier faulted mesh/fragment pair remains a separate priority. Need exact draw inputs or a focused replay to distinguish bad resources/control flow from mesh output issues; broad Shader Validation currently cannot run through the earlier large-LDS pipeline on this machine.
