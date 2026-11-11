# Production readback dependencies: measurement results

2026-10-06, session 64. Paused fight, PID 36204, native resolution,
compiled SRT and dirty-only BDA enabled. The user confirmed the scene looks
the same. This build adds measurement; graphics and synchronization behavior
remain unchanged.

## Result and next test

**The strongest candidate for an earlier-dependency readback is CMask region
`0x1167f00000`.** All 128 observed reads had a fully covered, already submitted
writer bound, 18–20 submissions before the queue tail. The GPU interval from
that writer batch ending to the current copy starting was **56.166 ms median**.
Its command-thread timeline waits totaled **4,320.565 ms** in this 30.363-second
window (27.95% of observed readback blocking).

The larger CMask region `0x1164b80000` also passed the observational dependency
checks, but its situation differs: just **0.560 ms median** elapsed from its
required writer batch ending to its copy starting. That required batch itself
took **47.519 ms median**, including **37.896 ms of detile scope elapsed** and
only **0.005856 ms of guest compute scope elapsed**. Waiting on that same batch
from another queue would still wait for the detiles inside it. This measures
batch granularity as an obstacle; it does not isolate active detile computation.

The concrete next optimization experiment is a separately controlled,
fallback-protected last-writer transfer readback for the `0x1167f00000` metadata
reads. Before enabling it, validate queue sharing/availability, every actual
copied span, pending writes, aliases, publication and source-buffer lifetime.
Compare off/shadow/on in the same paused fight. No such path was implemented
or enabled in this measurement session. **Blocking attribution is not a predicted
saving**; another queue may still contend for execution or memory bandwidth.

Indirect arguments require a separate approach: all 254 reads had required
writes in the current, unsubmitted recording tick and overlapping pending
bindings. They fail the earlier-submitted-writer eligibility checks. Native
indirect draw arguments remain a separate candidate. Never use stale CPU bytes.

## Coverage and observer cost

- Producer epochs **1912–1975**, 64 total, 62 interiors; 128 present API calls.
  Epochs are guest SuspendPoint hints, not displayed frames.
- **Zero GPU timer drops, CPU event drops, split scopes or uncollected batches.**
  All 62 interior epochs now have complete GPU coverage, unlike the previous
  18/62 subset. There are 153,600 GPU ranges across producer/presenter and
  21,160 queue submissions.
- Median epoch: 5,068 draws /724 computes. The previous capture had a different
  draw count, build and profiling configuration; this is not a controlled FPS
  comparison against it.
- Lifetime tracing enabled, fine CPU tracing disabled, draw detail every eight
  epochs. Seven interior epochs contain detailed draw intervals.
- Serialization cost is material: **51.370 ms/epoch median**, query collection
  **7.443 ms**. Instrumented throughput does not establish normal gameplay FPS.
- Foreground writer recording came from one thread, **18248**. No observed opaque
  device-address writer, watched buffer retirement or mapping change. This is
  window coverage, not a guarantee for every game scene.

## Readback evidence

All observed synchronous watched transactions, including boundary epochs:

| Drain region / role | Calls | Timeline wait total ms | Average wait ms | Earlier bound | Intervening submissions |
| --- | ---: | ---: | ---: | ---: | --- |
| `0x1164b80000`, CMask | 128 | **6,855.099** | 53.555 | 128/128 | 3 in every case |
| `0x1167f00000`, CMask | 128 | **4,320.565** | 33.754 | 128/128 | 18:51; 19:69; 20:8 cases |
| `0x1140008000`, indirect arguments | 254 | **4,266.022** | 16.795 | 0/254 | Required current recording tick |
| `0x1165e00000`, comparison CMask | 127 | 18.789 | 0.148 | 127/127 | 1 in every case |

Total timeline blocking **15,460.475 ms**, approximately **50.9%** of the
30,363.429 ms producer window. Metadata accounts for 72.29%, arguments 27.59%,
comparison 0.12%. These figures are measured in this run; the earlier ETW
98.46% pre-DMA percentage is not a measurement of this window.

The drain address can differ from the original guest request. The classifier
uses **every actual copied span**, including the widened 524,288-byte CMask
drains and all packed, disjoint argument spans. It verifies native source
identity and 64-byte packing, rather than using just the requested bytes.

All three CMask drains use native buffer `0x10128ae5860`, size **285,802,496
bytes**, with stable observed identity. The arguments use `0x1005f1096d0`,
16,384 bytes. Different metadata spans sharing one large buffer makes range
barriers, aliases and concurrent queue access relevant to a future port.

### GPU context, separate from CPU wait

Same producer queue and timestamp clock, all 128/127 corresponding pairs:

| Region | Writer batch end → queue-tail batch end, median ms | Writer batch end → copy start, median ms | Copy scope median ms | Copy scope total ms |
| --- | ---: | ---: | ---: | ---: |
| `0x1164b80000` | 0.351 | **0.560** | 0.072528 | 9.002144 |
| `0x1167f00000` | 56.146 | **56.166** | 0.016784 | 1.583616 |
| `0x1165e00000` | 0.021760 | 0.088672 | 0.070176 | 8.084896 |
| Indirect arguments | unavailable | unavailable | 0.001440 | 0.370528 |

The `0x1167f00000` needed writer batch was 1.467 ms median, with no recorded
detiles and 1.381 ms of guest compute scope sum. The large metadata batch was
47.519 ms as described above. Submission-end timestamps conservatively include
other operations in the writer's batch; they are not per-store completion times.
These intervals and CPU wait averages use different clocks and sampling
distributions. They must not be subtracted to predict an FPS improvement.

Declared compute writers include shader `0xea0aceac518ec52d` over broad
18,874,368/37,748,736-byte ranges, plus smaller metadata producers
`0x723423f5115aedbd` and `0x5942a92ebef7b363`. Descriptor write declarations
overapproximate actual stores. Source identities/shaders are in
`.analysis/writer-evidence.json`; full original events retain shader companions.

All 254 argument decisions reject overlapping pending bindings and required
unsubmitted ticks. Their submit-return-at-checkpoint rejection reflects that
the eventual required submission had not happened yet. No decision rejects
missing history, copy footprint, native source identity, opaque pending writes,
multiple recording threads or event drops in this capture.

## Remaining budget: complete interior cohort

Medians over the same 62 complete interior epochs, milliseconds/epoch:

| Category | Median |
| --- | ---: |
| Wall interval | 548.508 |
| Foreground explicit waits | **334.679** |
| Guest work excluding recorded waits/submits/profiler | **147.508** |
| Submission CPU | 7.074 |
| Profiler serialization | 51.370 |
| Query collection | 7.443 |
| Captured batches known complete before next CPU submit, gap lower bound | 18.805 |
| GPU submitted-batch elapsed union | 444.885 |
| GPU graphics scopes | 35.891 |
| GPU guest compute scopes | 61.660 |
| GPU detile dispatch scopes | **273.151** |
| GPU copies/clears | 67.309 |

CPU waits rank first, remaining host translation/recording second, submission
API overhead third. Seven detailed epochs have net-wait phase medians:
state41.059, bindings32.202, commit29.982 ms. Detile ranges dominate GPU elapsed,
but include stalls/preemption; they are not isolated shader active time.
Independent medians and overlapping CPU/GPU categories are not additive.

Median submissions332.5, average submission gap1.625 ms, maximum gap per
epoch81.429 ms. Producer and presenter use the same native queue in this build.
Queue timestamp gaps total9,688.354 ms across the window, but do not prove
whole-device idle time or CPU starvation. The completion-observation lower
bound above only concerns the captured schedulers, with untraced external work
excluded from the claim.

Barrier records:365,896 resource entries,318,680 broad entries,168,847
ALL_COMMANDS→ALL_COMMANDS entries; zero recorded queue ownership transfers.
These are resource entries, not barrier call counts or measured barrier latency.
One3.778 ms queue-idle call occurred on the presentation thread; the dominant
foreground wait site remains `BufferCache::DownloadBufferMemory`, line275.

## Implementation and verification

Reused CommandScheduler's existing event/query infrastructure:

- Lifetime hooks record native ownership, actual copied spans, scheduler ticks,
  pending writable bindings, command-emission completion, conservative image
  downloads and mapping invalidation. No extra copies/waits were introduced.
- Offline range history treats declared stores conservatively, commits them at
  command emission, uses an initial recording-tick floor, and rejects missing
  coverage, pending/unsubmitted writes, identity/packing errors and event loss.
- Query capacity increases64→256 pages of256 pairs. New16-bit index/generation
  token helpers address65,536 pairs; completed pages still reuse only after
  existing timeline completion. Metadata exposes actual capacity.
- Launcher accepts `-BdaSyncInitialMode on` alongside compiled-SRT initial mode.
- Existing analyzer emits writer bounds and same-clock GPU context. GPU-context
  reanalysis reused the saved event database; no additional gameplay capture.

Full emulator/harness builds passed. Native Vulkan query test queued192 gated
batches before release and used timer index49,154, beyond the old14-bit range;
completion/reuse checks passed with zero query/event drops. Native buffer
readback probe verified actual published bytes and GPU ownership, and its
observed dependency classification passed. Offline writer tests **13/13** and
existing production analyzer tests **9/9** passed, including timestamp wrap,
scheduler identity, incomplete coverage and fallback cases.

Executable SHA256:
`23C8DD3FF2970AF729FF04E8C8FF0717F66763C64EB9F7EDBFB8007A0E36E1A2`.

## Saved artifacts and current state

Frozen prefix:
`D:\PS5\ufc5-profiles\production-20261006-173216.writer.csv`.

- `.selection.json`, CPU/GPU/events/submission/metadata suffixes: frozen64epochs.
- `.analysis/`: report, per-frame, readbacks, lifetimes, writer bounds,
  `readback-writer-gpu-context.json`, `writer-evidence.json`, event SQLite.
- `.budget.json`: CPU/GPU cohorts and foreground readback/phase budget.
- `.controls.json` plus frozen BDA/SRT CSVs: bounded mode/validation guard.
- Live base `.csv.manifest.json`: launch options and executable hash.

Guard report endpoints inside the window: BDA21,169calls/21,127candidate,
42 matched checks,0races/mismatches/rejections; compiled SRT577,360calls/
576,188candidate,1,172checks,0mismatches/rejections. Boundary guard rows include
some preceding activity; this is a mode/correctness guard, not an isolated
candidate timing comparison.

PID36204 remains running, both CPU candidates on, capture request off. Graphics
confirmed unchanged by the user. No synchronization optimization or commit.
