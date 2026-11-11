# UFC 5 detile replay: buffer bindings and native residency

## Finding

All seven slow captures associate with scratch-memory creation groups containing
non-local system-memory backing. The only fast capture uses a different Vulkan
memory block whose remaining native backing candidate is local. Residency is now
the strongest measured lead for replay inflation.

These are **CPU creation-interval and shared-binding associations**, not an
exported Vulkan-to-driver handle map. They do not establish the game's primary
FPS cause or pure shader execution time. No rendering, allocation flags or
synchronization was optimized.

## Capture and coverage

- Stem: `D:/PS5/ufc5-profiles/gpuview-20261006-132832-26a81c1c`.
- ETL: 991,952,896 bytes, WPR 26100, **zero lost events/buffers**.
- Game PID 6380, unchanged build `7FEA0B84...`; heavy profiler/FlatSRT off.
- Replay PID 1468, identity build
  `8DDEA7AC763D336D1ED256C528B3493D8CD782BAA4E593E08AB2FA1BC0CDBE99`.
- All eight references MATCH; 104 unique app joins, zero drops/splits.
- **80/80 measured native packet joins**; 16 warmups uniquely join, eight first
  warmups have two native candidates and are excluded from native attribution.
- All 104 submits have zero explicit semaphore wait dependencies.
- No unmatched packet stops in the replay window; whole-trace boundary diagnostics
  retained. No replay DMA stop marked preempted; no queue timeout.
- QPC origin 6734504830774 ticks at 10 MHz; 12 raw/decoded anchors span nine ticks
  (0.9 us). Replay interval 638406.2–4473960.0 us relative to ETL. GPU clock is
  not calibrated to CPU; CPU/ETW align through QPC.

## Timings and placement

Ten measured iterations per row. Placement is the last observed page-in before
DMA entry for lifetime-valid candidates associated with Vulkan memory creation.
RTX segment 2 is local; segment 3 is non-local/system memory. All input/scratch
properties still report DEVICE_LOCAL (0x1, type 1), not physical residency.

| Guest address | GPU median ms | Before DMA ms | DMA residence ms | Input candidates | Scratch candidates |
|---|---:|---:|---:|---|---|
| `0x1164920000` | 4.150160 | 0.0335 | 4.5725 | local* | system, system |
| `0x11673b0000` | 33.380300 | 0.0670 | 33.5110 | system, system | system, system |
| `0x1167460000` | 13.238200 | 0.0415 | 13.4610 | system, system | system, system |
| `0x11687e0000` | 4.233825 | 0.0290 | 4.3030 | local* | system, system |
| `0x1168bd0000` | 19.720050 | 3.5990 | 20.1620 | system, system | system, system |
| `0x11691a0000` | 4.390785 | 1.3270 | 4.6085 | local* | system, local |
| `0x116abb0000` | **0.415904** | 0.0270 | 0.5635 | local* | local* |
| `0x1172520000` | 13.796150 | 2.4525 | 13.9755 | system, system | system, system |

\* The local block's constructor interval also contains a transient 64 KiB native
allocation retiring before use. The association is **partial**; the remaining
32 MiB candidate pages into segment 2. Its transient entry is not assigned to a
particular buffer. Native subrange mapping to VMA offsets is unknown for mixed groups.

Slowest capture: CPU recording **0.022900 ms**, submit bracket **0.093750 ms**;
clear **1.887328 ms**, dispatch **31.426512 ms**, post **0.005504 ms**. Its 33.380 ms
GPU bracket/33.511 ms DMA residence persist after the initial 0.067 ms median
queue delay. Recording/submission/initial queue delay cannot explain this GPU
range. Stage brackets include stalls; medians are not additive; DMA residence
is not pure execution.

All candidate placement states are stable across ten measured iterations per
capture. Tracked page-in/eviction/migration/sysmem events for these candidates
do not occur inside the measured DMA spans. This indicates already-established
placement, not an observed migration during every dispatch. No complete per-page
access, PCIe bandwidth or scheduler execution measurement is claimed.

## Binding and lifetime evidence

VkDeviceMemory `0x21c6a5db7a0` is created within capture 2's input constructor
(1703278.3–1703722.2 us). Two 32 MiB native allocations start on that thread at
1703453/1703587 us: `0xffff8d8ddb9bacf0` / `0xffff8d8dd646e910` (indices 116/117).
Input/scratch share that VkDeviceMemory; capture 2 offsets 0/21168128, capture 3
reuses it at 0/24576000. Both candidates page into segment 3 before all twenty DMA spans.

VkDeviceMemory pointer `0x21c6a5dbf20` is reused between captures 1/8. Native
114/115 retire; new constructor creates 120/121. Analyzer replaces the binding
group, rather than joining later uses to a retired pointer lifetime.

Fast input/scratch share `0x21c6a567f90`, offsets 12779584/21626944. Surviving
candidate `0xffff8d8dbcde9d60` pages into segment 2 at 1220029 us and lives through
4291299 us. The transient 64 KiB entry remains listed as retired/partial.
Creation associations are not direct exported identity, even when status is `complete`.

RTX adapter `0xffffe0817f721000`: ReportSegment 2 is local group 0 (8,407,482,368
bytes), segment 3 non-local group 1 (34,276,306,944 bytes). Native sysmem commit
and page-in events identify system-memory placement. See [Microsoft segment flags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_segmentflags)
and [GPU segment model](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-segments).

## Ranked investigation targets

1. **Replay residency/access cost:** seven slow scratch groups include non-local
   backing; the fast group has a local surviving candidate. Strongest measured
   lead; access cost versus other in-DMA stalls is not causally separated.
2. **Separate queue backlog:** before-DMA maxima for slow captures 5.983–20.550 ms.
   These affect host latency, not detile GPU execution. Earlier production data
   found 98.46% of matched readback wait before DMA entry, with tiny copy scopes.
3. **Production CPU feed/waiting:** earlier traces show substantial command-thread
   readback blocking and feed gaps. Game profiler is off here; this capture does
   not provide a new per-frame production CPU budget.

No measured basis here for removing barriers, changing synchronization, overlapping
guest work or rewriting detile. Broad barriers remain an audit finding, not latency attribution.

## Next discriminating measurement

Briefly stop emulator CPU submissions while **retaining allocations**, run replay,
then resume. This avoids another boot/menu cycle. Trace actual progress/placement:
stopping threads does not guarantee GPU drain or unchanged WDDM placement. Compare
matched placement conditions. Persistent latency with non-local placement and no
competing game work would separate access/residency from ongoing contention; a
controlled local-backing comparison remains necessary before a causal residency claim.

Also correlate actual production source/scratch resources. Replay adds its own
buffers and cannot establish the game's FPS cause. The lower-resolution/no-FPS-gain
observation remains; it did not identify physical placement of each hot buffer.
No additional normal replay requested this session.

## Tools and artifacts

`tools/analyze-replay-residency.py` reads existing profiler/GPUView exports, adding
no runtime collector. It retains multiple candidates, missing/partial associations,
retired entries and pointer reuse. Four focused regressions pass for shared blocks,
retired lifetimes, token reuse and bookkeeping/partial cases.

Stem sidecars: `.traceheaders.txt`, `.qpc-anchor.txt`, `.clock-alignment.json`,
`.dxg.csv`, `.dxg-compact.csv`, `.replay.native-manifest.json`, `.replay.native-analysis.json`
and `.packets.json`, `.replay.correlated.json`, `.replay.phases.json`,
`.replay.residency.json`, `.replay.residency-summary.json`. PID6380 remains running.
No emulator restart, optimization, build or commit this session.
