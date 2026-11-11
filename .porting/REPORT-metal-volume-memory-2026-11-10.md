# Canonical 3D volume memory — 2026-11-10

Replaced the native Metal volume path that cached one texture per guest virtual address. It previously returned the old texture after an SRD shape/format change, skipped CPU refresh after any GPU store, silently skipped linear input loading, limited uploads by volume size, and could widen packed formats into a different representation. No shader skips, zero substitutes, guest work caps or LDS truncation were added.

## Implementation

All uncompressed 3D image descriptors now use the canonical physical Metal backing from first use. The image key includes the address, dimensions, all eight SRD words and R128. Exact physical spans are compared even within the current draw/dispatch; later commands reimport CPU/raw-GPU updates. Cache eviction only releases private copies whose producer has queued physical writeback; it is not an ownership rule.

The production `VolumeMemoryTransfer` imports thick-tiled mips with the existing GPU address kernels and exports BASE_LEVEL..LAST_LEVEL at the existing compute/graphics encoder boundaries. Exact visible voxel footprints include partial Z slabs and shared XY tails. Discontiguous physical memory gathers/scatters use the existing canonical chunk resolver. A captured producer retains its old physical buffers across a subsequent VA remap. 3D writer provenance now reaches the shared storage registry, and 3D compute bindings create the selected mip view. Natural host mip limits include depth, while physical MAX_MIP remains separate.

VM1 then exposed genuinely missing linear volume loading. Added a GFX10 linear layout and raw buffer/3D-texture blits: rows align to 256 bytes, descending mip records share a full chain per Z slice, and visible depth shrinks with LOD while the physical slice stride stays fixed. Copy/export touches visible rows only. The fixed 67x35x19/R32/six-level fixture has a 27,392-byte chain, 520,448-byte allocation and mip offsets [9472,4864,2560,1280,512,0]. This follows AMD's layout/address functions; it does not assume mip volumes are packed consecutively.

Primary references inspected:
- [AMD GFX10 AddrLib HwlComputeSurfaceInfoLinear](https://github.com/GPUOpen-Drivers/pal/blob/dev/src/core/imported/addrlib/src/gfx10/gfx10addrlib.cpp)
- [AMD AddrLib ComputeSurfaceAddrFromCoordLinear](https://github.com/GPUOpen-Drivers/pal/blob/dev/src/core/imported/addrlib/src/core/addrlib2.cpp)
- [Apple Metal feature tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf) and [RG11B10Float format](https://developer.apple.com/documentation/metal/mtlpixelformat/rg11b10float). Native writable packed-float support was verified with actual GPU stores on this machine, rather than inferred solely from documentation.

## Validation

Final full coherence suite under Metal API + GPU Validation PASS: `kytx-volume-coherence-final-linear.log`. New coverage: 60 GPU layout/view cases across Standard4KB/64KB/PRT/linear, element sizes 1/2/4/8/16, selected mip ranges, repeated 1x1 thick tails, depth-dominant mips, noncontiguous chunks, CPU refresh, newer raw GPU bytes after gather, partial storage writes and a second reader in the same command buffer. Four actual native float-store cases cover R16Float/RG11B10Float in tiled and linear memory. Complete allocations, unused mips, padding and guards compare exactly: 54,790,656 bytes; 193 negative controls. A separate same-VA 64x64x64/RG11B10 -> 200x113x7/R16 test covers a queued producer, remap and remap-back reinterpretation of real physical data.

The initial test failed because its uint writer sent an out-of-range value to R8Uint and expected truncation; Metal saturates that store. The test generator now supplies representable raw values for narrow uint formats. Strict byte comparisons were retained. The failure and diagnostic logs are archived, not labeled PASS.

Existing alias/physical mip/BC/depth/rectangle suites remained green. Full API+GPU-validated tiling PASS (30 volumes/133 mip round trips plus the full 58-case suite) and pipeline PASS (normal/MDI/mesh/mip/depth-memory/bounds), after the shared mip/footprint changes and before the subsequent volume-only linear addition. Final full coherence and product build were rerun after that addition. Emitters unchanged; full MSL/CFG not rerun. The previously recorded Apple CFG issue remains open.

Final build: `kytx-volume-build-linear.log`, SHA256 `5b4c621ae129f56f2867b1ae82b06b3ccebff6604208bb5fc9416a958dc9fa89`. Eight source hashes are in `validated-build.json`.

## Game observations

VM1 PID19302 started19:06:19+05 using tiled-only SHA34f67f84cb537823fdb24f5f83e247607c88b442f34b1609ab562ee19508173f. It naturally aborted on the explicit missing linear 3D layout at compute guest2e3d4bf7caf5a2b0/nativefd2f84c02fa61484. Linear loading was implemented afterwards.

VM2 PID19549 started19:11:06+05 with the final SHA. Log: `/Users/tx/Developer/fex-spike/runs/validation-20261025/lean/vm2-191105.log`. CUA observed the intro at frame227 and the EA Servers notice at259. One deliberate J press skipped the visible intro; a second dismissed the visible EA notice. Frame312 was black. No automatic input loop was used.

VM2 again reports a GPU hang. FaultInfo state4 identifies draw#14520, RT0x8241cd0000, RECTLIST, Metal RG11B10Float92, VS native1d58ae6f8ed54b0c / guest a8092251af057bb6 and PS nativebaf34dd57c65d299 / guest dec48a1373748c4f. This is the same pair attributed before this stage (SR8/SP3/SP4); it identifies the failed encoder, not which shader operation is faulty. Source/IR/metadata/SPIR-V are archived under `first-fault-pair`. Later CPU translation after GPU recovery cannot validate the generated GPU results.

The original40GB process guard remains; no timer kills, cap changes or process signals. Launcher extra_env was restored byte-for-byte. Do not select a dead app through CUA: it can auto-launch another c9 run. Check exact PID before any further screenshot/input.


Final CPU binding observation (after the earlier GPU failure): native compute5562b272e870e82f / guest1211e65193f45e32 now binds all three volumes at actual200x113. res0/res2 use native RG11B10Float92; res1 guestR16Float13 at VA8244b50000 uses nativeR16Float25. No ImageFormatReject occurred in the inspected log. The transient VA differs from SP4, as expected for this run. This proves the shape/format binding transition proceeds; it does not prove GPU execution after the prior hang. PID19549 remained live at the final process check. Exact observation timestamp/log line evidence is in game-vm2-observation.json.

## Remaining work

Replay/attribute the failing RECTLIST shader pair with real bindings and isolate the faulty operation. Selective Shader Validation still has the previously proven instrumentation-related static-LDS inflation in an earlier compute shader; do not shrink guest LDS or thread counts to bypass it. The old 200x113/R16 descriptor failure must be revisited once the game reaches that command without a prior GPU failure; the standalone rebind regression passes but is not evidence of successful game GPU output.

Compressed 3D transfers explicitly reject. General same-dispatch read/write aliases across separately materialized images, legacy detached producers before their first canonical transition, linear 2D interop and general lazy coherence are not claimed solved. No validated match, shadow fix, scene image or FPS gain. Prior shadows/lighting/clothing/hair/neon/upscale artifacts remain unproven.

Artifacts: `/Users/tx/Documents/Codex/2026-11-09/new-chat/work/metal-volume-memory`. Dirty-before baseline retained; only this stage is packaged. Inherited dirty/untracked work preserved. No staging, commit, push or reset. Ledger CLI unavailable; STATE/HANDOVER/worklog carry the checkpoint.
