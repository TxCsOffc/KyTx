# UFC 5 / KytyPS5 ledger

Session 65 (2026-10-06) — **CMASK IMPOSSIBLE-CLEAR SKIP: CORRECT, NO SPEEDUP.
ON BY DEFAULT IN BOTH LAUNCHERS (user felt fewer stutters); opt out with
`profile-production.ps1 -NoCmaskSkip`. Code still requires the env var.**

AnyPS5 comparison done (reference only). Experiment 1: `KYTY_CMASK_PRECHECK_CSV`
per-range counters in `MaterializeColorClear`. CMask clears need code0 + render
target + valid clear register + `DecodePackedColorClear(view format)`. Precheck run
(PID31820, 30.5s paused fight): `1167f00000` 159/159 dirty readbacks impossible
(undecodable clear), 5062ms; `1164be0000` 158 dirty/158 clears 7607ms;
`1165e60000` 158 dirty/158 clears 712ms. 23519 impossible calls, impossible_clears0.
Skip run `KYTY_CMASK_SKIP_IMPOSSIBLE=1` (`-CmaskSkipImpossible`, PID22128):
`1167f00000` 97 skipped/0ms, skip_cpu_faults0, impossible_clears0, screenshots
`ufc5-profiles/cmask-skip-{off,on}-fight.png` look the same. **Wait moved to
`1164be0000`: ~110→~170ms/call; paused-fight rate ~3.1 guest frames/s, no gain.**
Both readbacks drain the same GPU timeline. `1167f00000` view format 83
(R16G16_SFLOAT), clear word0: impossible only because the decoder lacks that
format, so its clears are missed today with or without the skip (existing gap).
Detile replays 8/8 MATCH.

Detile offline check (session 64 writer CSV, 64 epochs, no new build): 33.9
detiles/epoch, `detile` 182.3ms/epoch (dispatch 163.1, pre 19.1, clear 19.1, post
0.1). Same surfaces replay game-closed at 0.08–0.48ms (~4ms/epoch total); in game
1.8–17.6ms each (20–115x). `detile_pre` (drain barrier, ends at compute stage) only
0.57ms, so dispatch inflation is not the drain. Matches session-62 replays run while
the game was live (up to 33ms, non-local backing). Census now: driver usage
7625MB > budget 7300MB, VMA blocks 4257MB, ~3.4GB driver-side gap still unidentified.
Counter-evidence: lower-resolution run under budget (6.5GB) had no FPS gain, but
had no detile GPU timing. Next: same detile timing while under budget.

**Residency test, same build/flags, 32-epoch GPU windows, paused fight:**
4K mode (`production-20261006-201122`): driver 7974MB > budget 7195MB; detile 59.2/epoch
413.2ms/epoch, top surfaces 42–230x replay; median wall 736ms, waits 465ms; title 3 fps.
`-Output1080p` (`production-20261006-201522`): driver 6606MB < budget 7269MB; detile
57.8/epoch **2.16ms/epoch** (0.01–0.15ms each, replay speed); **median wall 242ms, waits
18ms; title 10 fps**. Picture broken in this mode (black scene, HUD, blue corners), so
not a like-for-like render, but detiles run at native speed once under budget. The
override only reports PS5 resolution codes (1=1080p, 2=4K); the game picks internals,
so no 960p option. `tools/detile_residency.py` summarizes a capture.

**3.4GB DRIVER-SIDE VRAM GAP ATTRIBUTED** (`KYTY_VRAM_ATTRIBUTION_CSV`, `-VramAttribution`;
driver heapUsage vs VMA blockBytes around each Vulkan call, 4K mode, paused fight,
`production-20261006-202802`, 194s): total gap +3445.7MB, unattributed −21.5MB.
- `pipeline_graphics` 545 creates: **+2240.6MB** (~4.1MB each, zero VMA). Compute 235: +73.8MB.
  Session-17 "not pipelines" was only that capping/destroying did not shrink it; creation is
  where it grows. Destroy path not yet measured.
- BDA dedicated buffers: create 15210 driver +11431.7MB vs VMA +6441.1MB; destroy 14002
  −10249.9 vs −6364.6. **Net live overhead +1105MB** (~0.9MB per live buffer; looks like
  per-allocation rounding of `VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT`, from e7385352).
- Images/views/samplers/submits/descriptor pools ~0 (images +23MB net).
Over budget by ~400–780MB, so either lead alone could bring 4K mode under budget.

**`KYTY_BDA_SUBALLOC=1` (`-BdaSuballoc`) FAILED — LEAVE OFF.** Reference (dedicated,
`production-20261006-202802`): detile 323ms/epoch, median wall 584ms, driver 7596/7308MB,
VMA blocks 4313MB, host VMA 1581MB. Suballoc (`production-20261006-203615`): BDA gap
270.1/−270.3MB (overhead gone) but VMA device blocks 5500MB (+1.2GB), host VMA 608MB,
driver 7878/7389MB — unchanged pressure. Dedicated BDA buffers had been landing in host
memory under `WITHIN_BUDGET`; pooled blocks pulled them into VRAM. **Crashed when the
round started:** `failed to create image 1024x1024 format141 layers3 levels11`
(image.cpp:801), device-local allocation failure. The 1.1GB "BDA overhead" was mostly
placement, not rounding waste. Remaining real target: graphics pipelines +2.24GB.

**ONE PIPELINE OWNS THE 2.2GB** (`KYTY_VRAM_EVENTS` now reads exact driver usage — the old
snapshot used VMA's cached estimate, so session-17 per-pipeline numbers were invalid; shader
events log program id→guest hash; `-VramEvents -PipelineBudget 600`, `production-20261006-205222`).
544 graphics pipelines: VS `34e4418c73659659` + PS `d273ce369042ff54` **+2214.4MB**; next
16/8/2/0.1MB; 539 zero. Compute 236: six nonzero, 73.8MB. 151 graphics + 29 compute
destroys returned 0MB (driver keeps a high-water local-memory pool). Cause: GCN per-thread
scratch emitted as `Function` `scratch_dwords` u32 array (×2 halves when lane_count=2,
`spirvEmitterProgram.cpp:712`); NVIDIA sizes local memory per-thread × ~70.6k resident
threads ≈ 32KB/thread. Next: log scratch_dwords/lane_count and static scratch address bounds
for those two shaders; candidate fix sizes the array to the provable max offset.

**IT IS PIXEL-SHADER LDS, NOT SCRATCH** (`KYTY_SHADER_STORAGE_LOG` / `-ShaderStorage`, log-only,
`production-20261006-210413`; reproduced +2213MB on the same pipeline). PS `d273ce369042ff54`:
scratch_dwords=0, `fn_lds=1`, lanes=1, 16 LDS ops all dynamic (`ShiftLeftLogical32` addresses).
Non-compute LDS is a per-thread `Function` u32[`LdsDwordCount`] and pixel stages have no workgroup
info, so it falls back to 8192 dwords = 32KB/thread (`spirvEmitterMemoryHelpers.cpp:63`);
2266496KB / 32KB = 70.8k threads. PS `b512e609035e4c43` is the only other one (4 dynamic LDS
ops; costs nothing more because the pool is high-water). Logger now also prints each LDS
op's address expression and the guest PS `EXTRA_LDS_SIZE` (real per-wave allocation).

**LDS IS PER-LANE PRIVATE** (`production-20261006-211154`). Every op in both shaders is
`U32 at (LaneId<<2) + off`: d273 offs 0..1280 step 256 (6 slots), b512 offs 0/256; guest
`EXTRA_LDS_SIZE` 3 and 1 (×128 dwords = 1536/512B = exactly the reach for 64 lanes). No
cross-lane sharing, so the per-thread array is semantically right, just 21× oversized.
Candidate `KYTY_FUNCTION_LDS_BOUND=1` (`-FunctionLdsBound`, default off): for workgroup-less
stages, if every LDS address is immediate/LaneId/LaneId<<k (k<8), no FlatLocal and no
secondary offsets, size = ceil((lane_max<<k)+off+max(bytes,16))/4 with lane_max=127 (+32 for the
high half; subgroup masks are uvec4, so SubgroupLocalInvocationId<128). Otherwise 8192. Array
and `access.length` both come from `LdsDwordCount`, so every in-range index stays in range and
none ≥ old size existed — bounds-equivalent. d273 → 451 dwords (1.8KB/thread, ~127MB pool).
Baseline screenshot `ufc5-profiles/lds-bound-off.png`.

**LDS BOUND ON: 4K FITS IN VRAM** (`production-20261006-212322`, `-FunctionLdsBound`). Log:
d273 lds_dwords=451 (1804B/thread), b512 195 (780B); nothing else changed. That pipeline now
+128MB (was +2213MB). Driver device-local ~5.9-6.0GB (was 7.97GB vs 7.2GB budget); Task
Manager dedicated 6.3/8.0GB, shared 0.7GB. Pipelines total 154MB (was 2.24GB). Visual: user
saw no difference; screenshot `lds-bound-on.png` renders normally (different camera, not a
pixel diff). Paused-fight present rate steady 7.4-7.6 FPS (last 12 samples) vs 3.2-5.5 in the
4K runs before it and ~8.5 in 1080p. User: "no major improvement" during play.

Last updated: 2026-10-06 session 64 — **COMPLETE GPU COVERAGE;
CMASK READBACK DEPENDENCIES MEASURED. NO SYNCHRONIZATION CHANGE YET.**

User authorized writer/dependency tracing and increased profiling query capacity,
then confirmed the paused fight and unchanged graphics. Built/tested once,
restarted as authorized, current PID36204. Native output/sparse BDA, compiled
SRT and dirty-only BDA on. Lifetime tracing on, fine CPU off, detail every8epochs.

[Writer results, budget, limits and next test](readback-writer-results-2026-10-06.md).
Frozen epochs1912–1975:64 total/62 interiors,30.363s,128 present API calls,
median5068 draws/724 computes. **Zero timer/event drops, split scopes or
uncollected batches; all62 interiors GPU complete.** Query recorder now has
256 pages of256 timer pairs, using16-bit index/generation tokens. Native test
queued192 gated batches, used index49154, then verified reuse; no extra waits
were added to production recording/collection.

637 readback decisions:383 have an earlier submitted dependency bound;
254 indirect reads reject required unsubmitted writes/overlapping pending
bindings. All writer recording observed on thread18248, no opaque address
writer or watched lifetime change. Actual copied spans, source handles,
packing and operation completion are traced; absent coverage blocks eligibility.

Readback timeline waits15,460.475ms =50.9% wall:

- CMask1164b80000:128 calls/6855.099ms, earlier bound by3 submissions in all
  cases. **Writer batch end→copy start only0.560ms median.** Required writer
  batch47.519ms, detile scope sum37.896ms, guest compute scope0.005856ms.
  A transfer copy waiting on that same batch still waits for its detiles.
- CMask1167f00000:128 calls/4320.565ms, earlier bound by18–20 submissions.
  **Writer batch end→copy start56.166ms median**, needed batch1.467ms with no
  detile scope. Strongest contained earlier-dependency candidate, not proof of
  recoverable wait or FPS gain.
- Indirect1140008000:254 calls/4266.022ms. Required current recording tick,
  so no earlier-submitted-writer path demonstrated. Native indirect separate.
- Comparison1165e00000:127 calls/18.789ms, earlier bound by1 submission.

CMask drains share one stable285,802,496-byte native buffer; a future transfer
port must validate range visibility, concurrent queue access/ownership, aliases,
source lifetime, all spans and CPU publication. Next test should target only
1167f00000 with separately controlled fallback/shadow validation. Do not port
the whole GTA33b16934 commit or assume all11.18s metadata blocking removable.

All62 complete interior medians(ms/epoch): wall548.508, foreground waits334.679,
guest work excluding waits/submits/profiler147.508, submit7.074, serialization
51.370, query collection7.443, captured-batch completion gap lower bound18.805.
GPU batch elapsed444.885, graphics35.891, guest compute61.660, detile273.151,
copy/clear67.309. GPU scope elapsed includes stalls/preemption; not active shader
time. Observer cost is material, medians/categories not additive, epochs not FPS.
This build/configuration/draw count differs from session63; no controlled FPS
delta. Earlier ETW percentages belong to earlier captures.

Frozen prefix D:/PS5/ufc5-profiles/production-20261006-173216.writer.csv:
selection/suffixes, .analysis (writer bounds/GPU context/lifetimes/events),
.budget.json, .controls.json, frozen SRT/BDA CSVs. Guard endpoints inwindow:
BDA21169/21127 candidate/42 matched checks/0races/mismatches/rejections;
compiled577360/576188/1172 checks/0mismatches/rejections. User says looks same.

Full emulator/harness builds, native byte/ownership readback probe, 13 writer
analysis tests and9 existing profile tests passed. Executable SHA256
23C8DD3FF2970AF729FF04E8C8FF0717F66763C64EB9F7EDBFB8007A0E36E1A2.
PID36204 remains running, both candidates on, capture request off. No commit.

---

Last updated: 2026-10-06 session 63 — **BOTH CPU CANDIDATES ON;
READBACK WAIT STILL DOMINATES FULL CAPTURE.**

User reported brief9FPS then authorized remaining-budget capture. Samepaused
PID26444, compiledSRT/BDAon, nativeoutput/sparseBDA; no restart/build/optimization.
64epochs7147–7210,33.515s,127presentAPI,median4932draw/724compute (different from
old5432/750). Zeroeventdrops/splits/uncollectedqueriedbatches. Foregroundreadback
wait20071.305ms=59.9%wall. CMask1164b80000 128calls/9483.064ms and1167f00000
127/5357.925 =73.94%wait; indirect1140008000 254/5206.377=25.94%; comparison
1165e00000 127/24.111. SitebufferCache.cpp263 waits currenttick beforepublication.

[Full budget, coverage limits and next steps](after-bda-production-budget-2026-10-06.md).
All62CPUvalidinteriors medianms: wall595.676,foregroundwait399.113,guestwork
excludingwait/submits/profiler152.758,submit7.234,flush24.828,querycollect7.109.
8detailedepochs recording112.663/translation43.265; drawnetwait phasesstate42.824,
bindings32.957,commit29.084. Independentmediansnotadditive/epochsnotframes.
Oldbudgetiscontextnotcontrolleddelta; user9FPSnotasustainedgainclaim.

715GPUtimerdrops:715submitswithoutquerypage,largestqueriedbatch84<256,
0batchesat256;64pagepoolavailabilityexhaustion. Only18/62interiorsGPUcomplete,
late7192–7209, fasterthanearlysample. Existingreportmediansselectthese18;
101mswaitmedianthereisnotwholeCPUcapture. Newofflinecohortsummaryusesall62CPU
withzeroeventdropsandonly18GPUcomplete. GPUsubsetbatch151.147/detile30.540ms
cannotdescribeearly44; scopeelapsedincludesstalls/preemption. Do notclaimcomplete
GPUrank. Enlargeexistinginflightquerycapacitynextprofilingbuildwithoutaddingwaits.

Frozenprefix D:/PS5/ufc5-profiles/production-20261006-155515.both-on.csv:
.selection.json/.analysis/.budget.json, readbacks/perframe/scopes/eventSQLite.
Modeguard .both-on-controls.json +frozenBDA/SRTCSVs; endpointsincaptureBDA19222/
19185candidate/37checks,compiled527887/526862/1025checks;bothon/0mismatch/reject,
BDA0raced. No heavyanalysisduringcapture. CurrentPIDresponding; candidateson,
profilerrequestoff. Graphicsconfirmationpending. No commit.

Next: observe perrange lastwriter/submittedtick/pendingBDAwrites/lifetime for
metadata+arguments, evaluate GTA33b16934transferreadback eligibility/fallback;
preservepublication/ownership/aliases. CurrentfineCPU/lifetimeoff so notmeasured
here. Do notpromiseallblockingremovableorportwholecommit. Prior98.46%preDMA
resultbelongsoldETWrun, notthiscapture. Nativeindirectremainsseparatecandidate.

---

Last updated: 2026-10-06 session 62 — **BDA CPU PATH 79.1% FASTER;
LARGE OVERALL FPS GAIN NOT ESTABLISHED. CANDIDATE ON.**

User confirmed paused fight. PID26444 same-process five40s stages off1/shadow/
on1/off2/on2, compiled SRT on throughout, native output/sparse BDA, detailed
profiler off. BDA us/call107.91/106.12/22.07/100.37/21.61. Weighted repeated
off/on104.43→21.80us (-79.1%); buffer visits/call870.26→1.94 includingchecks
(-99.78%). 97.62% on calls unchanged shortcut. Actual uploadKiB/call37.38/37.16.
Stable on candidatecoverage99.8%; CPU timing/visits win repeatable, not a whole
frame budget or proof of large FPSgain. Hostpresents/s3.69/5.09/3.82/3.30/5.06;
shadow alsofast, only2–3tensecondreports/stage; titleFPSdistinct/variable.

[Results, evidence and limits](bda-sync-results-2026-10-06.md).
Frozen selection684checks=681matched+3racedshadow+0stablemismatches;
56,923actualcandidatecalls, no rejection/exit. Stableonchecks102/0races/misses.
All stage count/time/checkclosurePASS. Compiled guardon4,210,814calls,
4,202,587candidate,8227checks/0mismatches/rejections. Graphicsquestion pending;
do not claim uservisualconfirmation. Live BDAon/compiledon, detailedprofileroff.

Prefix D:/PS5/ufc5-profiles/production-20261006-155515:
.bda-stage-{off1,shadow,on1,off2,on2}.json, .bda-fight.csv/.json,
.bda-compiled-guard.csv/.bda-analysis.txt. Source .csv.bda-sync.csv continues.
Tests/build details below session61. No commit or secondrestart. Next targeted
productionbudget with both CPUcandidateson; earlier metadata/indirect readback
waits remain separate and unchanged. Do not assume old waitcosts/currentFPS
cause without that capture. Live off remains tools/set-bda-sync.ps1 -Mode off.

---

Last updated: 2026-10-06 session 61 — **DIRTY-ONLY BDA PORT BUILT AND TESTED;
LIVE GAMEPLAY COMPARISON PENDING.**

User authorized the next CPU candidate. Adapted GTA commit
e5dd76e7f111d577d957840257216b56f07afcf0 to local BufferCache/MemoryTracker.
Dirty snapshots plus CPU/region, registration and mapping epochs select buffers;
retained buffers keep their original full mapped-intersection upload batches.
No GPU barrier/readback/queue or output changes. Publish dirty hint before epochs;
publish tracker regions before global epoch; remember pre-snapshot epochs;
handle disjoint mappings in the same region together. Unconfigured renderer
uses the original scan; dirty transitions retain extra atomic bookkeeping.

[Implementation, controls and measurement procedure](bda-sync-experiment.md).
Off/reference, shadow/actual upload coverage, on/validated candidate live switch.
Stable missing uploads disable candidate process-wide; concurrent-write misses
are inconclusive and cannot validate it. Periodic counters include elapsed CPU,
visits, actual uploads, dirty bytes, epoch skips, checks and fallback state.

PASS: emulator/harness build; MemoryTracker actual dirty-selection, mapping,
registration, GPU ownership, concurrent write and controller fallback tests;
RTX3070 Vulkan native upload bytes/clean skips/multi-run batching/new buffers/
remapping/GPU ownership/injected reference-recovery checks (default and sparse
BDA); compiled-SRT 9000 differential refreshes plus fallback regression; PS syntax.
First clean-skip test corrected for newly published tracker epochs, no redundant
uploads. Temporary backing allocation failures cleared after old emulator exit;
no memory policy change. Logs D:/PS5/ufc5-profiles/bda-sync-*20261006.*.

Stopped old PID24900 for harness memory availability, then launched new PID26444:
D:/PS5/ufc5-profiles/production-20261006-155515.csv (+manifest/stdout/stderr).
Executable SHA256 AFCAC22B9A3AE616C5CD4DF150358800CE761929B2C36536AF8BF7B4BC3842E8.
Compiled SRT starts ON per user preference; BDA OFF; native output, sparse BDA,
detailed production profiler disarmed (64epochs/detail8 available). Awaiting
user return to paused fight. No gameplay performance/visual result claimed yet.
Plan off1/shadow/on1/off2/on2 live stages, compiled SRT on throughout; counters
and presents measured with bounded stage helper; then targeted production
budget only if needed. No commit. Control D:/PS5/ufc5-bda-sync.control.

---

Last updated: 2026-10-06 session 60 — **AFTER-SRT PRODUCTION BUDGET MEASURED;
DIRTY-ONLY BDA NEXT CPU CANDIDATE.**

User asked nextstep. Captured same pausedfight/process24900 on thenoff, restoredon.
No restart/build/newrendereroptimization. Existing profiler64epochs/detail8;
fineCPU/lifetimeoff. Both64captures62completeinteriors,0timer/eventdrops,
0split/uncollectedbatches. Frozenranges7361–7424on/7629–7692off, existinganalyzer.
[Full comparison and next port](compiled-srt-production-comparison-2026-10-06.md).

Medianms/epoch on/off: guestworkexcludingwait/submits/profiler209.635/257.944
(~48ms reduction), foregroundwait140.249/71.560, GPUbatch157.687/126.020,
detileelapsed46.252/15.541, guestcompute51.760/49.764. Same5432draw/750compute
mediancounts. Captured GPUinflation/readbackwaitsmask CPU saving; cause unknown,
no overallFPSclaim or proven causal candidate regression. CPU/GPU uncalibrated,
independentmediansnotadditive. Onactualcandidate99.83%,909checks/0mismatches in
containedinteriorCSVwindows. Visualconfirmationstillpending.

Sampled7on/8off detailed epochs netofwait phases: bindings69.617/70.866,
state46.637/92.557, commit33.858/34.145ms. RemainingCPUbindingslargestdrawphase.
SourcePrepareBda scansmappedranges/cachedbuffers; fineBDA shareunknown. Next
containedCPUcandidate GTAe5dd76e7dirty-onlyBDA, with counters/livecontrols and
write/map/register/region epoch/concurrent-write validation. Do not claim all
70msrecoverable. Larger metadata/readback dependency optimization remains separate.

Onwholewindow metadatawait7612.521+4899.894ms (~80.5%readbackwait), indirect
2354.189ms; off4771.804+3070.692/1808.005ms. WaitsitebufferCache.cpp262.
Broadglobalentries172460on atgraphicsRun.cpp1574, latencyunmeasured. Epochsnot
frames. Windowon32.842s/126presentAPI;off30.682s/127. Profiler~26msserialization
+8msquerycollectionperepoch; normalgameplayFPSnotderivedfromthese captures.

Frozen stems D:/PS5/ufc5-profiles/production-20261006-143328.compiled-{on,off}.csv;
.selection.json/.analysis reports. PhasecomparisonJSON same stem compiled-phase-
comparison.json. Added tools/freeze-production-window.py. Final24900responding,
compiledon/profileroff. No commit. Readbackwriter/queuepath and native GPU
inflation/residency controls remain pending; no BDA port implemented yet.

---

Session59 follow-up: user prefers retaining/enabling compiled SRT as a measured
CPU-path saving that may help as other bottlenecks are removed. Requested live
on for running PID24900; validation/fallback/periodic checks remain. Preserve
port and live off control. The lower on-window presentation rates remain an
unresolved measurement, not grounds to claim an overall gain or a proven causal
regression. Visual confirmation still pending. Launcher still starts off for
controlled baselines; current continued-testing preference is on. Future traces
must record this mode and compare identical configurations.

Last updated: 2026-10-06 session 59 — **COMPILED SRT: CPU PATH WIN, NO FPS WIN;
POSSIBLE GAME-LEVEL REGRESSION. CANDIDATE OFF.**

User drove PID24900 to pausedfight. Same-process40s off1/shadow/on1/off2/on2
stages complete; no restart/crash. Nativeoutput, detailedproductionprofileroff.
[Report, bounded measurements and next step](compiled-srt-results-2026-10-06.md).
Stable materializer us/call8.072/8.276/2.355/8.075/2.351 respectively: repeatable
~71% lower elapsed per call (~3.4x). Common shader comparisons also improve.
Candidatecoverage99.82%/99.77%,1,327,250 actual candidatecalls; remaining oncalls
fullcomparisons,0unsupported/0reference-only. Counts/elapsedclosurePASS.

Frozen selection21,742checks/0mismatches includes transitions/gaps. Stablechecks
shadow11303/on11463/on21180. Productiononuses actual readers; shadowtimingreplay
isnotproductionperformance. Uservisualconfirmationpending as written.

Hostpresents/s4.34/3.62/3.96/5.32/3.14 (3complete reports/stage, firstboundary
excluded); titleFPSmedians3.5/3.5/3.5/6/3. Both onwindowsbelow precedingoff;
noFPSgain. Sequentialshortwindows do not establish cause; possiblegame-level
regression must be investigated. Do not describe this as a gameperformancewin.
Materializerms/s146.90/153.79/49.59/177.53/33.73 depends on changingcallrates,
includescallbacks/preemption, not exclusive framebudget.

Artifacts stem D:/PS5/ufc5-profiles/production-20261006-143328.csv:
.compiled-stage-{off1,shadow,on1,off2,on2}.json, .compiled-fight.csv/.json,
.compiled-srt.csv. Added tools/measure-compiled-srt-stage.ps1 and
tools/analyze-compiled-srt.py; frozenboundedresults, no doublesummingrows.
Same binary hash as session58. Finalprocess24900responding, compiledcontroloff,
productionprofileroff. No commit. Next: visualcheck then short existingproducer
off/on trace, compare readbackwaits/CPUwork/GPUdurations/queuegaps before retain
decision or dirty-onlyBDA. No further optimization yet.

---

Last updated: 2026-10-06 session 58 — **COMPILED SRT BUILT; LIVE FIGHT TEST NEXT.**

User authorized trying the contained compiled SRT evaluator. Ported evaluator
from GTA e42d611aa0d7a40b80eefb1cc1260abb069894fb, adapting local indirect-image
and EXEC/ReadFirstLane semantics. No GTA memory-cache/BDA/threading/synchronization
changes bundled. Default interpreter preserved. See
[implementation, controls and verification](compiled-srt-experiment.md).

Launch `tools/profile-production.ps1 -CompiledSrt` starts off. Live
`tools/set-compiled-srt.ps1 -Mode off|shadow|on` polls every500ms. All plans in
scope; supported plans need successful full validation before candidate use.
Shadow checks first/every64calls; on checks every512calls per plan. Compares
ordered ordinary/strict reads, success/failure, complete snapshot, specialization
and active sources without rereading guest memory. Returns reference on checks;
any mismatch disables candidate for process. Unsupported/observer paths fallback.
CSV distinguishes actual candidate/reference/comparison calls and compile cost.

Release emulator/harness/materializer buildPASS. 9000 randomized comparisons
PASS (3081successful/5919failed), existing tests and direct readsPASS. Unsupported
graph fallback and deliberately corrupted candidate recovery/live controlsPASS.
Initial synthetic fallback test crashed the test executable by evaluating a
malformed reference expression; corrected case now passes. No game crash/launch
from that test. Eight game-closed detile replays allMATCH, GPU0.082–0.477ms.
Synthetic96slot median13.773us reference/3.777us candidate (~3.65x), not UFCFPS.

Next: launch experimentoff, same pausedfight off/shadow/on/off windows, uservisual
check, analyze actual coverage/materializer elapsed/presents. Detailed production
profiler stays unarmed initially. Gameplay results/FPS gain unknown. Retained-
allocation residency control and other GTA ports remain pending. No commit.

Launched native-resolution build PID25328 at14:31:21BST; responding. SHA256
B83FE45ECBFFF8645026EEE7B71184F24F51FBA3107CAA221D11917B6BC5E7B3.
Manifest D:/PS5/ufc5-profiles/production-20261006-143121.csv.manifest.json;
evaluator CSV same stem +.compiled-srt.csv. Experimentcontroloff, production
profileroff, other evaluator experimentsoff. Waiting for user pausedfight.

User accidentally closed PID25328; restarted same build at14:33:28BST as PID24900,
controlsoff. New manifest D:/PS5/ufc5-profiles/production-20261006-143328.csv.manifest.json;
use this stem for the live fight comparison. No code/build change.

---

Last updated: 2026-10-06 session 57 — **REVISED FIRST PORT: COMPILED SRT.**

User supplied an alternative priority table. Reconsidered port size/testability
alongside measured latency: compiled evaluator e42d611a is the first contained
port to investigate, preserving dynamic/strict reads, EXEC behavior and reference
fallback, with off/shadow/on validation. Separate its evaluator from bundled
guest-memory-cache changes. Dirty-only BDA follows if aggregate cost warrants it;
dirty/map/registration epochs and concurrent-write races need validation.

This revises session56's indirect-first recommendation. Indirect/metadata/last-
writer readbacks remain latency targets; threading remains a larger port. Earlier
materializer elapsed2083.408ms/20s=10.4%, including observer/preemption, not pure
CPU or a recoverable whole-frame budget. GTA evaluator3.8x is not a UFC FPS claim.
98.46% preDMAwait does not prove unrelated or redundant prior work. No code port,
new gameplay capture, build or optimization this session. Review/README updated.

---

Last updated: 2026-10-06 session 56 — **GTA FORK REVIEW COMPLETE; NO PATCH APPLIED.**

User redirected the retained-allocation control to review TheCruZ/KytyPS5-GTA.
Fetched main ef5b8df43ac2074812380123ad0abe65352640fd into gta-audit/main;
refreshed origin/main to 7d2422abbe84f560f5eb40e3b722166768c0bd91. No checkout,
merge, build, emulator change or commit. Fork has 39 unique commits and lacks
nine newer upstream commits. Compared against actual dirty UFC5 working files.

[Full source review and ranked experiments](gta-fork-review-2026-10-06.md).
First new candidate: native indirect draws from 33b16934, observing eligibility
and persistent-instance/index dependencies before a default-off subset port.
Local CPU indirect consumers are measured; GPU path absent. No coverage or FPS
gain established. Next: last-writer transfer-queue readbacks from same commit,
then dedicated execution/resolve/recording pipeline e43ed2c8 + 45328020. These
need lifetime/order/writer tracking changes; they are not simple thread toggles.

Dirty-only BDA epochs e5dd76e7 and compiled SRT e42d611a are additional CPU
candidates. Known-fill 8c0762fe overlaps our prior proven-fill experiment, which
removed two dirty CMask reads without a large FPS win; remaining writers unknown.
Clean-byte backing reads already partly exist locally. Current release handler
already avoids unconditional label-only flushes; GTA submission gains cannot
be copied as a prediction. Async pipelines 57e5ea4b skip eligible draws while
compiling and are deferred for steady-state profiling/output preservation.

Eight read-only patch checks: seven fail against current working tree, only
small guest BDA suballocation 2043a692 cleanly applies. No patch applied. Results:
D:/PS5/ufc5-profiles/gta-fork-audit-20261006.json. Applicability is not validation.
No suspension helper created or test performed. PID6380 was running initially;
final check found no Kyty process, cause unknown. No stop/restart by this review.
Residency/control measurement remains pending; review does not explain slow
replay spans or prove game FPS causality. Next work is an indirect eligibility
observer, followed by a bounded port only if meaningful safe coverage is found.

---

Last updated: 2026-10-06 session 55 — **ALL80 MEASURED BUFFER/PACKET RECORDS
ANALYZED; SEVEN SLOW SCRATCH GROUPS ASSOCIATE WITH NON-LOCAL BACKING.**

Stem D:/PS5/ufc5-profiles/gpuview-20261006-132832-26a81c1c;991952896-byte ETL,
0 lost events/buffers. ReplayPID1468,identitybuild8DDEA7AC...; game6380 unchanged.
All8MATCH;104 appjoins,0drops/splits;80/80 measurednativejoins;16warmupsunique,
8firstwarmupsambiguous excluded. All104 explicitwaitdependencies0. No interior
unmatchedpacketstops. QPCzero6734504830774,10MHz,12anchorsspan9ticks.

GPU medians4.150160,33.380300,13.238200,4.233825,19.720050,4.390785,0.415904,
13.796150ms. Slowest CPUrecord0.022900/submit0.093750ms; clear1.887328,
dispatch31.426512,post0.005504ms; nativeDMA33.511/preDMA0.067ms. Initialqueue
wait/CPUrecord cannot explain GPUrange. Spans not pureexecution.

Buffer creationbounds/VkDeviceMemory+offset associate all7slow scratch groups
with native32MiB candidates page-in system/nonlocalsegment3. Input/scratchflags
DEVICE_LOCAL0x1/type1. Fast0x116abb0000 sharedblock has surviving32MiB candidate
page-inlocalsegment2; transient64KiB candidate retired, associationpartial.
Capture6scratchgroup mixed3/2; native subrange mappingunknown. Placementstable
across10iterations/capture; no trackedcandidate memoryevent insideDMAspans.
Creationintervalassociation is not exportedVulkan/nativehandlemap, not complete
perpage residency/accessproof or whole-gameFPScause.

VkMemory0x21c6a5dbf20 pointer reusedcap1→8; native114/115 retire,120/121 replace.
Offline analyze-replay-residency.py preserves lifetimes/sharedblocks/multiple,
missing,partial associations;89allocationrecords,124native lifetimes. Four
focused regressionsPASS. No emulatorcode/build/restart thissession.

[Residency report, rankings and next measurement](replay-residency-results-2026-10-06.md).
Next control: briefly stop gameCPUsubmissions keepingallocations, replay, resume;
measure actualqueue progress/placement, not assume drained/unchanged. No another
normalreplay requested. Productioncorrelation needed before gameFPS attribution;
lowresolution/noFPSgain observation remains. No optimization or commit.

---

Last updated: 2026-10-06 session 54 — **NATIVE REPLAY TRACE SAVED; ALL80 MEASURED
ITERATIONS JOINED; NON-LOCAL ALLOCATION LEAD; BUFFER IDENTITY BUILD READY.**

Stem D:/PS5/ufc5-profiles/gpuview-20261006-121339-3dd329c3; WPT26100 saves ETL,
739246080 bytes,0 lost events/buffers. GamePID6380 unchanged, replayPID26640,
oldheadless1CE40F4C... . All8 MATCH;104 unique appjoins,0drops/splits,all104
explicit waits0. Native80/80 measured joins;16warmups unique,8firstwarmups
ambiguous excluded. No interior unmatched packet stops. QPCzero6689571321712,
10MHz,12anchorsspan8ticks. No GPU/CPU clock calibration.

Seven replay GPU medians<0.5ms;0x1172520000=13.9799ms. Its native DMA median
14.177ms, preDMA2.536ms (max69.789), clear2.224848,dispatch11.773920,post0.002080;
CPUrecord0.02225/submit0.08745ms. GPU inflation remains inside submitted work,
separate from beforeDMA delay; spans not pure execution, medians not additive.

Two32MiB replay allocations during lastcapture setup page into non-local segment3
after PagingOpSysmemCommit. Native global/allocation handle lifetimes identified;
later MigrateAllocation stops0xc0000001, then sysmem commit/page-in3 again.
ReportSegment2 localgroup0 vs3 nonlocalgroup1. Exact logicalinput/output binding
not recorded in oldbuild, so residency lead not proved gameFPS cause. Replayadds
ownallocations; prior reducedVRAM/noFPSgain observation remains.

Measurementonly: Buffer constructor CPUbounds+VkBuffer identity; existing
ProfileAllocation metadata now VkDeviceMemory/VMAoffset/extent/type/properties,
mappedpointer/usage; explicit replay input/upload/download/params and scratch
roles. No allocationflags/barriers/waits/render changes. Only headless rebuilt,
ReleasePASS SHA8DDEA7AC763D336D1ED256C528B3493D8CD782BAA4E593E08AB2FA1BC0CDBE99.
replay-allocation-check-20261006.csv/.gpu.csv all8MATCH,104joins,0drops/splits,
fiveconstructor scopes/capture,completebindings; input/scratchflags0x1/type1
even slowcapture2/3. No nativeETL for validation; no placement/FPSclaim.

[Full report/artifacts](replay-trace-results-2026-10-06.md).
Next: same elevated capture helper -ReplayDetiles -EmulatorId6380 -Seconds30,
no emulatorrestart; lifetimebounded native creation→binding/residency joins,
ambiguity retained, compare slow/fast allocations. No optimization/commit.

---

Last updated: 2026-10-06 session 53 — **SECOND REPLAY APP CAPTURE COMPLETE;
OLD WPR STOP BUG IDENTIFIED; INSTALLED NEWER RECORDER SELECTED FOR RETRY.**

Stem D:/PS5/ufc5-profiles/gpuview-20261006-115722-6de80d99. Replay PID29216,
game6380, same binaries. All8 references MATCH;104 unique application submit
joins,0 event/query drops,0 splits; all104 explicit wait_dependencies=0.
Saved .replay.analysis.json and .replay.stages.json. Whole detile stage medians
4.326704,22.051808,0.173200,0.081424,14.596816,0.081888,0.419392,0.173392ms.
0x11673b0000 dispatch bracket20.660832ms; 0x1168bd0000 dispatch11.372384ms,
clear3.054112ms. Elapsed GPU brackets include stalls, not pure execution.

Improved native runner retained actual stop exit -2147417850 / 0x80010106:
"Cannot change thread mode after it is set", Profile Id RunningProfile.
Manifest correctly records stop_failed; main graphics ETL missing. WPR now none,
no manual cancellation/recovery needed. Microsoft documents this stop bug and
recommends WPT build19650+. Helper previously selected PATH/System32 WPR
10.0.19041.7548; installed toolkit WPR10.0.26100.7705 already exists.

capture-gpuview.ps1 now prefers installed toolkit executable, accepts explicit
-WprExecutable, rejects affected build<19650, prints recorder version and saves
version/hash in manifest. Same executable used for start/stop; no software install
or emulator change. Windows PowerShell5 read-only preflight passes with26100,
WPRnone, PID6380 responsive, controls off. Actual trace save with newer recorder
still pending elevated user run. No renderer/synchronization optimization.

[Report and Microsoft reference](replay-trace-results-2026-10-06.md).
Next: same -ReplayDetiles -EmulatorId6380 -Seconds30 command, no game restart;
check ETL/event loss/rawQPC then join native replay packets before attribution.

---

Last updated: 2026-10-06 session 52 — **REPLAY INFLATION CAPTURED IN APP DATA;
NATIVE ETL SAVE FAILED; POWERSHELL STDERR HANDLING FIXED FOR RETRY.**

User ran -ReplayDetiles -EmulatorId6380 -Seconds30. New stem
D:/PS5/ufc5-profiles/gpuview-20261006-110119-0ec3b4ec; earlier pasted Saved trace
093956 is old, not this replay's native recording. Replay PID32860, emulator6380,
UTC10:01:21..25, same headless1CE40F4C... hash. All8 outputs MATCH;104 iterations
uniquely join application submits,0 event/query drops,0 split scopes. All104
selected submits have0 explicit wait dependencies/1 completion signal. Own
per-iteration host completion wait remains unchanged; implicit/native stalls not
excluded. Game heavy profiler controls off.

Production stage medians confirm elapsed inflation inside clear/dispatch scopes:
0x1168bd0000 whole14.039568ms, clear3.058480, dispatch10.978592, post0.003440.
0x11673b0000 whole11.334784, clear0.891072, dispatch10.439136, post0.007872.
Other whole medians6.086960,6.069840 and fast0.082176,0.082128,0.424544,0.183568.
Scopes not pure active execution; medians not additive; small post range does not
prove barriers free. Driver/native scheduling/residency mechanism still unknown.

Windows PowerShell NativeCommandError at WPR stop aborted final logging/manifest.
Expected ETL missing; original manifest state recording/endnull preserved. Later
wpr-status not recording; no unrelated instance stopped/cancelled. Only located
instance temp trace is262KiB WPR internal collector marks, not GPU data; copied
to .wpr-internal.etl and decoded .csv. .recovery.json preserves audit. Native exit
code/reason and main lost-event stats unavailable. Disk free C27GB/D101GB.

Helper WPR start/stop now StartProcess with separate stdout/stderr, exact quoting,
cached process handle and real exit code, avoiding Windows PowerShell5 stderr
promotion under ErrorActionStop. Stop invocation exceptions persist manifest.
Windows PowerShell5 regression with blankstderr + exit7 passes; parsing/preflight
passes, AdministratorFalse so retry remains user-triggered. User given same
Admin command; no restart needed. Retry pending returned output/tracepath.

Application artifacts .replay.analysis.json/.replay.stages.json beside stem.
[Full result/failure report](replay-trace-results-2026-10-06.md).
Next: verify retryWPR diagnostics/ETL, loss/rawQPC and replay native packet joins,
then attribute slow/fast elapsed brackets from queue/DMA/dependency/paging evidence.
PID6380 responsive. No renderer/sync optimization, game restart or commit.

---

Last updated: 2026-10-06 session 51 — **CORRELATED STANDALONE REPLAY READY;
USER BACK IN PAUSED FIGHT; ADMIN GPUVIEW CAPTURE COMMAND PROVIDED.**

Following session50's live/closed contrast, added default-replay --profile-output
option, reusing production CommandScheduler profiler (no parallel submit logger).
Per-iteration sidecar records target tick, thread, CPU record/flush bounds and
existing replay's raw GPU stamps, including warmups. Production profiler emits
queue-enter/submit-end, stages/barriers/waits and detile subscopes. FineCPU/Lifetime
off; query stress mode separate. Game's heavy profiler remains off. Replay wrapper
records its child PID through StartProcess, hashes and process snapshots; no shell
used for native arguments. Default unprofiled replay remains available.

Headless Release build PASS, SHA256
1CE40F4C14CD1BB3855A485F1AFB01D0EEA26504D62FEDF07B2553C7B7B3748F.
Verification run D:/PS5/ufc5-profiles/replay-correlation-check.csv and .gpu.csv
prefix: all8 full references MATCH,104 warmup+measured iterations uniquely join
submit bounds and address-tagged GPU scopes,0 event/query drops,0 splits.
This run occurred while new game was loading, not a paused-fight performance test.
Additional profile CPU overhead is visible; do not compare profiled wall spans to
unprofiled as renderer improvement/regression. Scheduler1..8 are replays;0 is
harness setup and unsuffixed submits can be empty. Tick values repeat across
schedulers, so joins require thread/time as well.

Added analyze-replay-profile.py with optional native packet joins requiring raw
QPC origin. Two regressions pass: tick reuse/native PID/exact time selection,
ambiguous native candidates excluded and raw origin required. Real104 joins pass.
No calibration from GPU clock to CPU is claimed, no pure shader active-time claim.

Extended capture-gpuview.ps1 -ReplayDetiles to run replay inside existing ETW
window and embed its metadata, QPC boundaries and output prefixes in manifest.
Default3 warmup/10 measured per saved capture. Cannot combine ArmProduction;
preserves unrelated WPR recording. Both wrapper/helper parse; preflight validates
files, PID and WPR none. Administrator=False, so agent cannot start kernel trace;
user given Administrator PowerShell command with -ReplayDetiles -EmulatorId6380
-Seconds30. Actual ETW capture/native analysis pending returned Saved trace path.

User explicitly continued and drove in after launch. Same emulator SHA
7FEA0B84DDD7084ABF62410568F6C6B2C65319F6394934D334DD4751C014A539;
no emulator rebuild, renderer/synchronization changes or optimizations.
PID6380 responsive, launch trace production-20261006-102259.csv,64 epochs/detail8,
FlatSRT configuredall off, control files off. One restart from closed state to
prepare this capture, user reports in. No commit.
Next: analyze returned ETL loss/clock/packet joins and overlay replay stage brackets
with native queue/DMA/dependency/paging evidence. See [usage](detile-replay.md).

---

Last updated: 2026-10-06 session 50 — **SAME-BUILD CLOSED REPLAY: ALL DETILES
BELOW 0.5 MS; COMPLETE CONSTRUCTED UPLOAD PAIR 2.803 MS, BOTH ASPECTS MATCH.**

User closed emulator and authorized tests. No kyty process present before/after
either run (recorded metadata). No rebuild: headless SHA256 still
16D1F6D6EB37E19AC535BD44E198624FDA802A8FB3BBAE22FF88D4B6E9992B53.
Same captures, three warmups/ten measured as concurrent session49 tests. Both
commands exit0. Eight full linear outputs MATCH with identical hashes to live
CSVs; final constructed depth/stencil image aspects both MATCH.

Default GPU medians in capture filename order:0.084624,0.154032,0.175520,0.085440,
0.249776,0.082608,0.477552,0.174912ms. All below0.5ms. Examples:
0x11673b0000 live34.4056 -> closed0.154032; 0x1168bd0000 live20.4344 ->0.249776.
Six live-slow cases differ62–223x; two were already fast. Sequential comparison,
not randomized off/on/off. These numbers do not identify scheduling/residency/
contention/clocks as cause, nor contradict user's lower-VRAM FPS test.

Constructed pair GPU median2.803455ms (2.80256..3.02838), versus live2.739ms:
no demonstrated pair speedup. Closed bracket medians depthdetile0.249104,
depthupload1.937135, stencildetile0.477984, stencilupload0.138496ms. CPUrecord
0.025500, flush/wait2.919550, wall2.947150ms. Medians not additive; CPU/GPU
overlap; elapsed timestamps not pure execution. Per-iteration GPU stage totals
reconcile within CSV rounding. Pair still fails to reproduce production51.428320ms.
D32S8 is assumed; full draw/guest compute/dependencies/residency are not captured.

Artifacts D:/PS5/ufc5-profiles/detile-control-20261006-closed.csv and
upload-pair-20261006-closed.csv, each with .csv.meta.json; exact comparison/ratios
in detile-upload-comparison-20261006.json. Updated [results report](detile-upload-pair-results-2026-10-06.md).

Next: native scheduling/residency trace of fixed standalone replay alongside
constant paused fight, with replay PID/thread/submit bounds for exact packet
joins. It can reproduce inflation in a separate process without guest stream;
trace that condition before selecting a fix. Exact production batch replay still
requires image format/state and guest draw/compute/dependency capture. No emulator
restart, renderer optimization or synchronization change; emulator remains closed.
No commit. Measurement contrast is established; inflation mechanism remains open.

---

Last updated: 2026-10-06 session 49 — **DEPTH/STENCIL UPLOAD PAIR REPRODUCER
MATCHES; CONCURRENT DEFAULT REPLAY ALSO SHOWS LARGE ELAPSED-TIME INFLATION.**

Inspected production detile/upload architecture and saved tick2794298. Existing
default replay already includes TileManager's clear and pre/post barriers; it is
not just a dispatch benchmark. Transactions1833/1834 upload captured Depth64KB
surfaces0x1168bd0000 (4B) and0x116abb0000 (1B),3840x2160, into the same recorded
native image handle. KDR payloads/layouts saved Oct5; GPU timing trace is Oct6.

Added --detile-upload-pair to existing headless harness, exposed through
ufc5/tools/replay-detile.ps1 -UploadPair -Captures DEPTH.kdr -StencilCapture STENCIL.kdr.
Production TileManager::Detile and Image::Upload, one shared D32S8 image, one
submission, no intermediate host wait. Five stage timestamps measure elapsed
detile/upload brackets, not pure execution. CPU record and flush/wait exported.
Final depth/stencil image readbacks compare all active texel bytes; padding only
covered by default full-buffer replay. CSV's match columns are final checks.

This is a CONSTRUCTED transaction pair, not a full/exact batch replay. KDR v1 lacks
native image format/state, guest draw/compute commands, queue dependencies and
full residency; D32S8 is explicit assumption. No production renderer source or
sync semantics changed, no running emulator replacement/restart, no optimization.

Release target builds; headless executable SHA256
16D1F6D6EB37E19AC535BD44E198624FDA802A8FB3BBAE22FF88D4B6E9992B53.
Live game PID7600 remained responsive, controls off / off all. Three warmups/ten
measured: pair GPU median2.739ms (2.492..6.577), depth MATCH/stencil MATCH.
Saved D:/PS5/ufc5-profiles/upload-pair-20261006-live.csv.
Default replay same build/settings: all eight complete linear references MATCH;
concurrent GPU medians6.666,34.406,13.279,6.673,20.434,0.081,0.424,10.906ms.
Saved D:/PS5/ufc5-profiles/detile-control-20261006-live.csv.

Separate replay process can exhibit substantial inflation while game runs,
without guest command stream/image upload. Cause remains unmeasured; do not
choose scheduling/residency/contention/clock explanation from elapsed values.
Oct5 closed baselines differ in build/iteration settings, so same-build closed
repeat requested before a controlled inference. The fast constructed pair alone
does not reproduce the production51.428ms batch and does not establish FPS gain.

Invalid four-byte stencil input rejected exit1 before replay buffers/images;
wrapper parses and now writes build/settings/emulator-before/after metadata.
Metadata added AFTER live pair/control timings; do not imply those files have
retrospective start/end snapshots. No commit. Emulator closure requested for
clean pair/default repeats; not yet authorized/completed at this ledger update.
Report: [pair results and next steps](detile-upload-pair-results-2026-10-06.md).
Usage/coverage: [detile replay](detile-replay.md).

---

Last updated: 2026-10-06 session 48 — **READBACKS CORRELATED: 98.46% OF MATCHED
WAIT PRECEDES DMA ENTRY; COPY SCOPES TOTAL 12.462 MS. PRIOR BATCHES NEXT.**

User saved D:/PS5/ufc5-profiles/gpuview-20261006-093956-a11b2ab5.etl, correlated
application/ETW capture, 09:39:57–09:40:27 BST. Same PID 7600/executable/profile,
Firefox closed, FlatSRT off/all. Zero lost events/buffers, no interior unmatched
stops, 19,217 nonnegative render/DMA phase joins. No emulator code/build/sync change.

Initial diagnosis of missing app rows was WRONG: Windows directory sizes stayed
stale for open CRT streams. Direct reads reveal valid timings and refresh sizes.
Original manifest's zero growth is preserved; never use it as absence-of-data proof.
Helper now queries EOF through fresh shared handles for app/presentation offsets
and warns when producer CPU CSV does not grow. Parsing/read-only preflight pass.
Read-only non-invasive CDB inspection confirmed request consumed/window ended and
output paths. No memory patch. Diagnostic re-arm after ETW captured epochs 9577–9640;
these are excluded from the saved original-window snapshot.

Original application epochs 9301–9364: 64, zero event/query drops; 52 full epochs
inside ETW. Snapshot D:/PS5/ufc5-profiles/gpuview-20261006-093956-a11b2ab5.app.csv
and suffixes exclude the diagnostic arm. Snapshot app.analysis/report.md covers
all 64 epochs; do not confuse that with ETW overlap. FineCPU/Lifetime disabled.

Offline OpenTrace RAW_TIMESTAMP reader anchors 12 matching Dxg events to xperf,
QPC-origin estimates within 8 ticks (0.8 us), selected zero 6597335391114 ticks.
CPU steady-clock/QPC and ETW align; GPU raw clock is not calibrated to CPU. QPC
selected window 762772.9–30831225.7 us, **30.0684528 s**. Optional QPC boundary
support in analyzer has regression; four queue tests pass, two prior wait tests pass.

Command thread: CPU running **16,252.471 ms**, readback off CPU **12,683.591 ms**,
image frees 788.873, other frees 57.337, allocation 79.963, submission off CPU 4.967,
other GC 37.145, other stacks 164.106. Exact closure **30,068.453 ms**. Saved symbol
ranges reused from same PID/build/PDB baseline; no baseline timings mixed in.

517 application timeline waits join by target tick to producer submit; each matches
one native render packet issued by same thread inside actual queue-submit call.
API wait union **12,602.843 ms**, intersects **12,588.430 ms** of ETW readback off-CPU
time. Distinct populations: 540 switches vs 517 calls; lead-in unprofiled before arm.
Addresses: 0x1164b80000 104 calls/**6,724.372 ms**; 0x1167f00000 104/**3,988.203 ms**;
0x1140008000 206/1,270.387; 0x1165e00000 103/619.881. First two are 85.0% of matched
wait time. Addresses identify transactions, not every widened/published guest span.

Matched host wait partition: **12,408.676 ms before DMA entry (98.46%)**, 170.104
DMA residence, 24.062 after DMA exit. Same 517 GPU transaction/tick scopes: download
total14.458 ms, transfer-copy total**12.462 ms**, mean0.024104 ms, max0.103872 ms.
Nested GPU scopes, CPU waits and native residence are different clocks/boundaries;
no additive GPU/CPU frame budget or pure GPU execution claim.

Longest case epoch9339, transaction1835, address0x1164b80000, targettick2794302:
144.4385 ms APIwait, native6215018 enqueue→DMA144.315 ms, residence0.133 ms,
CPU returns0.0239 ms after native completion. GPUcopy0.075776 ms, 512KiBpayload.
Prior graphics packets include tick2794285/seq6214973 (55.345 ms residence,
55.129696 ms GPUbatch) and tick2794298/seq6215009 (63.199/51.428320 ms). Latter
contains detile dispatch brackets15.746048 and26.161152 ms plus image copies.
These preceding batches keep readback completion late. Their inflated in-batch
detile scopes are NOT explained merely by the readback packet's pre-DMA queue delay.

Native object/value dependencies: 3,158 unique wait→signal joins, 2,204 missing,
zero ambiguity used. Main graphics1,729/second graphics1,429 wait on native copy
context. For preceding wait6215007, copy signal752751 completes21.456 ms before
wait completion, then render6215009 entersDMA31uslater. Visible wait lifetime
includes graphics queue position; do not label all78.922 ms copy dependency latency
or generalize to every Vulkan detile/compute queue.

No Kyty render/DMA outstanding8,427.670 ms, command thread running8,018.333 ms
of it. App flush scopes **1,861.092 ms**, query collection**432.216 ms** add probe
overhead. Do not compare instrumented gaps/throughput directly with baseline as
renderer benefit/regression. Selected broad global ALL_COMMANDS→ALL_COMMANDS
entries139,186 (MEMORY_READ→MEMORY_READ|MEMORY_WRITE); count is not latency.

Next: use saved native dependency/batch data, then reproduce entire earlier detile
transaction/batch (clear, barriers, dispatch, copy) with headless harness. Distinguish
in-batch dependency/memory/scheduling delay from shader work. Smaller dense profiling
window or reduced emission is needed before treating correlated CPU feed gaps as
baseline. No immediate repeat/restart needed; no barrier/readback/cache optimization
is justified by these aggregate measurements alone. Controls off/all and off; PID7600
responsive. No commit. Full results/limitations/artifacts:
[correlated report](gpuview-correlated-report-2026-10-06.md).

---

Last updated: 2026-10-06 session 47 — **FIREFOX-CLOSED REPEAT COMPLETE;
READBACK WAITS STILL DOMINATE. CORRELATED APP/OS CAPTURE NEXT.**

User saved D:/PS5/ufc5-profiles/gpuview-20261006-092654-5ef086b8.etl after closing
Firefox. Selected paused-fight window 09:26:55–09:27:25 BST, 30.017772 s. Same
running PID 7600/executable SHA/WPR profile SHA as session 46; controls off/all and
off, no heavy app arming. Zero lost events/buffers; no interior unmatched stops.

TID 26980 budget: running 12,725.718 ms (42.4%); saved DownloadBufferMemory ->
Wait/WaitMaster -> MasterSemaphore::Wait readback path **16,137.286 ms** off CPU
(53.8%, 513 intervals, max 119.386 ms). Image frees 883.886 ms, other frees 20.671,
allocations 43.453, submission path 8.877, other GC 31.921, other stacks 162.403,
unknown boundary 3.557. Exact closure **30,017.772 ms**; independent xperf CPU
12,725.690 ms (0.028 ms difference). Off-CPU includes ready time, not API/GPU elapsed.

Fixed analyzer omission of a final switch-out without captured return: preserve
known off-CPU tail only when global switch coverage extends through selected end,
mark censored/missing boundary stack and leave cause unknown. Two wait tests pass.
No emulator code/build/graphics/synchronization change or commit in this session.

18,434 complete render packets match DMA sequences, all nonnegative. Mean native
enqueue-to-DMA-entry 7.337 ms (p95 30.054), DMA residence span 2.621 ms, notification
0.004 ms, parent 9.962 ms. Session 46 means were 12.429 / 2.909 / 0.004 / 15.343 ms.
Residence is not pure execution; packet intervals overlap and cannot form a frame
budget by addition. Initial censored render/DMA does not cross selected start;
tail pending renders are conservatively retained, excluded from latency distributions.
Four preempted Kyty DMA exits, no queue timeout.

No Kyty render/DMA outstanding: **4,367.548 ms**; command thread running during
4,037.548 ms of those gaps. These overlap the CPU-running budget. CPU feed limitation
and completion waits both remain. Firefox has no selected DMA residence/paging
entries; DWM residence union drops 10,760.065 -> 1,023.646 ms, remoting 3,025.750 ->
726.371 ms. Background activity is much lower, but readback waits remain large.

OS Present events 87 -> 100 (~2.898 -> 3.331/s), host ten-second reports for repeat
4.581/3.219/3.171/s. Counts differ from title FPS; populations/time/tracing differ.
Do not claim a validated FPS improvement or causal Firefox benefit from this one
sequential pair. UTC/QPC boundary reads differ ~3.6 ms; no sub-ms join from UTC alone.

Next: existing application profile + ETW on the live paused fight, using prepared
capture-gpuview.ps1 -Seconds 30 -ArmProduction -ApplicationProfilePrefix
D:/PS5/ufc5-profiles/production-20261006-083333.csv in Administrator PowerShell.
Read-only preflight passes except agent shell is not elevated; no restart required.
App window is configured for 64 producer epochs, FineCPU/Lifetime disabled. Request
off does not cancel an active window; it ends by count. Correlated mode has not run.
Join actual overlap to existing readback resource/tick/submit scopes, then distinguish
required prior work from dependency/residency delay. Do not remove synchronization
or cache stale guest bytes based on aggregate waits.

Full A/B and limits: [GPUView report](gpuview-report-2026-10-06.md).
Checked JSON comparison: D:/PS5/ufc5-profiles/gpuview-baseline-comparison-20261006.json.
PID 7600 remains responsive. Controls remain off/all and off.

---

Last updated: 2026-10-06 session 46 — **OS TRACE COMPLETE: READBACK TIMELINE WAITS
DOMINATE COMMAND-THREAD BLOCKING. FIREFOX VIDEO CONFIRMED; CLEAN A/B NEXT.**

User recorded D:/PS5/ufc5-profiles/gpuview-20261006-085331-171703d7.etl using the
prepared Administrator helper. Selected paused-fight window 08:53:33–08:54:03 BST,
30.018788 s, PID 7600, same executable SHA as session 45. Candidate off/all,
application heavy profiling unarmed. xperf reports zero lost events/buffers.

TID 26980's stacks identify GuestGpu::ThreadRun/command processor. Full-window
context-switch plus saved-return-stack attribution:
- Running CPU: 12,287.746 ms (40.9% of window).
- DownloadBufferMemory -> Wait/WaitMaster -> MasterSemaphore::Wait: **16,823.725 ms**
  off CPU, 454 intervals, max 154.502 ms (56.0% of window; 94.9% of off-CPU time).
- Dedicated image-memory free path: 613.316 ms, 2,720 intervals; other frees 33.393 ms.
- Allocation 58.603 ms, submission 24.434 ms, other GC 32.772 ms, other stacks
  139.786 ms, one missing boundary stack 5.013 ms. Totals close to 30,018.788 ms.
Off-CPU duration includes ready time, not entire Vulkan API/copy/GPU elapsed.
Independent xperf thread CPU differs by only 0.037 ms. High image-free switch
frequency was checked by duration and is much smaller than readback blocking.

16,094 complete render packets joined to native DMA context/sequence, all nonnegative:
mean enqueue-to-hardware-queue 12.429 ms (p95 40.590), DMA residence span 2.909 ms,
completion notification 0.004 ms, parent 15.343 ms. Residence includes queued wait;
never label it pure GPU execution or add overlapping packet durations to frame time.
4,199.048 ms has no reconstructed Kyty render/DMA outstanding; command thread runs
during 3,857.313 ms of that. CPU feed gaps exist, but are smaller than readback waits.
Boundary pairing diagnostics are retained; initial render/DMA missing retirements use
explicit later same-context completion upper bounds before the selected window.
One third-party signal remains censored and is excluded from render/DMA calculations. No unmatched
stop inside the selected window. Eight Kyty DMA preempted stops; no queue timeout.

Firefox has 13,216.834 ms DMA residence union on the same adapter, DWM 10,760.065 ms;
not active GPU time and not additive. **User confirmed Firefox video was playing
and closed it.** Causal contention effect remains unmeasured. Repeat unchanged
30 s baseline with Firefox closed before assigning all native queue delay to Kyty.
Same Administrator capture-gpuview.ps1 command; no rebuild/restart. Main process
remains responsive. Controls remain off/all and off. No new optimization or commit.

87 OS Present events (~2.90/s) in this trace; host reports drift 2.341–3.673/s.
These are not the historical ~6–7 title FPS. Large ETL/tracing overhead and workload
drift remain limits. GPU resource/hash causality is still unjoined: if waits remain
in the clean baseline, next use existing bounded application profiling alongside
ETW to match readback addresses/ticks/submits to native progress. Helper now has
-ArmProduction + -ApplicationProfilePrefix, verifies live launch PID/hash/control
and records offsets. That correlated mode passed preflight but has not run.
Its request-off file does not cancel an active 64-epoch window; it stops by count.

Added tools/analyze-gpuview.py and analyze-gpuview-waits.py, three queue regressions
and one duration/saved-stack regression (all pass). Tooling only; no emulator rebuild.
Full measurements, reproduction/artifacts and limits in
[GPUView report](gpuview-report-2026-10-06.md). Native packet lifetime cannot alone
explain the old per-detile shader timing; do not invent an active-engine budget.

---

Last updated: 2026-10-06 session 45 — **BROADER FLATSRT FIGHT TEST COMPLETE;
~20% LOWER MATERIALIZER CPU COST, ZERO MISMATCHES. NO RELIABLE FPS GAIN.**

User asked to continue. Extended the controller around the unchanged FlatSRT recipe
semantics to all shader hashes, with live all/hash scope selection. No Vulkan/shader
memory/cache semantics changed. Off remains default. Per-plan shadow sequence validates
first encounter and periodically retries; on mode compares new plans before activation,
keeps unsupported/unverified plans reference, and revalidates active plans every ~512
uses. A mismatch disables the experiment process-wide; changing scope cannot bypass it.

CSV contains per-shader rows and one selected-scope summary per report. Reference,
fast and shadow call/elapsed categories close to the parent. Fast recipe counts exclude
shadow replay. Analyzer deduplicates correctness counters, separates scopes, and reports
matched-population/total costs instead of treating fast-call means as whole-game savings.
Control/map/output overhead is outside materializer brackets and remains a limitation.

Emulator/harness build and materialization tests pass. Gate tests cover new plans,
unsupported/unverified/rejected plans and periodic revalidation. Eight saved fight
replays MATCH (0.082–0.477 ms isolated). Nine production analyzer tests and two FlatSRT
analyzer tests pass. Launch/control scripts parse and accept all/hash scope; no commit.

Previous emulator was not running at start. New launch PID 7600, trace prefix
D:/PS5/ufc5-profiles/production-20261006-083333.csv. User reached the paused fight.
Selected off/shadow/on/off window completed (365.720 s). On was gated after 8,585
zero-mismatch full comparisons and actual recipe execution; new plans remain gated.
Final 20,517 complete comparisons, zero mismatches, zero invalid/skipped rows.
367 hashes matched off/on; 354 executed fast calls. User confirmed the broad on scene
looks the same. No crash observed; PID 7600 responsive. Heavy GPU capture stayed off.

Stable pooled off: 1,883,642 calls, 17,697.222 ms total, 9.395 us/call.
On: 2,050,713 calls, 15,441.742 ms total, 7.530 us/call including revalidation;
1,975,559 fast (96.34%), 71,374 unsupported/reference, 3,780 comparison calls.
125,805,705 actual fast recipe evaluations. On comparison overhead 116.174 ms
(~0.75% of on CPU). All measured count/elapsed categories close to their parents.
Normalizing on per-hash means to off's exact call population projects 14,178.503 ms,
19.88% below measured off. This is shader-population-adjusted CPU elapsed, not
recovered frame wall time; observer/preemption costs and variant mixing remain.

Presentation medians initial off/shadow/on/return off: 4.819 / 2.525 / 2.711 / 2.815/s
(5/8/13/6 samples; first report per stage omitted). On is below initial off, but
return off remains similarly low. Substantial drift prevents a reliable FPS claim.
Candidate restored **off all**, heavy GPU capture **off**, default off unchanged.
Frozen .flat-first-attempt.csv/.json and .flat-stages.jsonl preserve the selected data.
Read [broad fight results](flat-srt-broad-results-2026-10-06.md) for full tables/limits.

Next: OS GPU packet/CPU scheduling trace for the much larger synchronous readback waits.
Installed GPUView/WPR profile parses. Prepared tools/capture-gpuview.ps1 for a 30 s
baseline with no restart, unique WPR instance, hashes and QPC/UTC/log boundaries.
PowerShell parse and read-only preflight pass; no existing WPR recording. Current
agent process is not Administrator; Windows kernel/GPU tracing requires elevation
(also required by installed GPUView README). **No OS trace recorded yet**; the user
must run the prepared helper from an Administrator PowerShell before that analysis.
Do not infer per-resource/shader/barrier causality from OS packet timing alone.
Read [flat-srt-experiment.md](flat-srt-experiment.md) for controls and semantics.

---

Last updated: 2026-10-06 session 44 — **FLATSRT RECIPE EXPERIMENT BUILT;
LIVE TARGET CPU ELAPSED ~22% LOWER. NO RELIABLE FPS GAIN DEMONSTRATED.**

User authorized the next CPU experiment. Added immutable predecoded address-read
recipes to the extracted ResourcePlan: literal operands/current user-data register
indices, existing memo indices, raw memory kind and immediate offset. Recipes are
enabled only inside FlatSRT refresh and its clean evaluator. Other operands/reads
retain original-order interpreter fallback. Parent memo/cycle/failure handling,
control-flow activity, strict readers, capture and specialization remain shared.
No guest bytes/results cached across draws; no Vulkan synchronization/work changes.

Default off, initial target d3dcf81c43080fd0. Launch with -FlatSrt; live
set-flat-srt.ps1 off/shadow/on. Full shadow reuses the existing recorded-read replay,
compares complete snapshots/specializations/read order/failure, returns reference
output and performs no extra guest reads. On requires live validation with actual
recipe execution; mismatches reject the plan and disable the observing thread's
experiment. Aggregate CSV records actual recipe execution, not only requested mode.
Descriptor gather and FlatSRT launches are separate; old controls remain supported.

Emulator/harness build; materialization and resource tracking tests pass. Tests cover
moving plans, changed inputs/bytes, memo reuse, fallback, strict/ordinary readers,
failed reads/bounds, inactive branches, negative underflow, cycles, finite image
specialization and deliberately corrupted recipe rejection. Nine analyzer regressions
pass; launch/control scripts parse. All eight saved fight detiles MATCH, 0.082–0.479 ms
isolated (five measured, two warmup); old emulator closed before replay. No commit.

Synthetic 96-read complete-materializer benchmark, six alternating 5,000-call windows
per mode: median off 12.952 us/call, on 11.217 us/call, 13.4% lower. Raw:
D:/PS5/ufc5-flat-srt-synthetic-benchmark.csv. This is not a production or FPS gain.
The first target's prior materializer cost was only 135.624 ms across 20 seconds;
a bounded success is not expected to resolve the whole slowdown.

Closed old PID 1344 normally; new binary PID 20052, prefix
D:/PS5/ufc5-profiles/production-20261006-080708.csv. User reached the paused fight.
Heavy GPU capture stayed disarmed. Off/shadow/on/off completed in the user-reported
paused fight; stage markers adjacent .flat-stages.jsonl. Selected CPU window spans
289.059 s. Frozen .flat-first-attempt.csv/.json preserve selected rows and analysis.
678 full shadow comparisons, zero mismatches; on was gated after 652 zero-mismatch
checks and actual recipe execution. Remaining comparisons were in the shadow-to-on
transition. These are complete output/read-order checks, not duplicate guest reads.

Stable aggregate rows (short transition windows omitted):
- Off: 67 windows, 118,367 materializations, weighted 11.893 us/call, median window
  11.577 us/call. Initial off weighted 11.569 us; return-off weighted 11.996 us.
- On: 40 windows, 63,414 materializations, **all 63,414 fast**, zero blocked;
  5,263,362 actual recipe evaluations (83 per call). Weighted 9.240 us/call,
  median window 9.000 us/call: about 22.3% lower target materializer elapsed than
  pooled off. Timers/counters/preemption and static-variant mixing remain caveats.
- Shadow: 27 stable windows, 43,290 materializations, 56,274 recipe evaluations.
  Shadow timing is not an A/B saving because recording/replay have different costs.

Presentation reports (drop first report after each stage marker because it can span
the boundary): initial-off median 2.400/s (3 samples), on median 2.621/s (7 samples),
return-off median 3.718/s (9 samples). Weighted rates 2.609 / 3.095 / 3.947/s.
Rates drift rather than showing a repeatable mode-dependent improvement. No reliable
FPS win claimed. This target previously contributed only 135.624 ms materializer
elapsed in 20 s; it cannot supply a massive whole-game gain by itself.

New FlatSRT analyzer regression verifies boundary selection, retained short-window
mismatches and actual/blocked execution counts; it passes in addition to the nine
production analyzer tests. No crash observed; PID 20052 responsive. Candidate and
heavy profiler controls verified **off**. User visual confirmation for this new live
candidate is pending (session 43's visual confirmation applies to the measurement
build, not this experiment). No commit or default-on change made.

Next: retain this as a default-off validated CPU experiment; assess broader shader
coverage in shadow before enabling more plans. Expected whole-game gain remains
unmeasured/modest. The larger unresolved readback waits still need a GPU execution
trace to explain prior-batch work/dependencies/preemption; do not cache old CMask or
indirect bytes or remove synchronization based on this shader-local result.
Read [flat-srt-experiment.md](flat-srt-experiment.md) for semantics, controls and gates.

---

Last updated: 2026-10-06 session 43 — **BOTH PAUSED-FIGHT CAPTURES COMPLETE;
FLATSRT DOMINATES SAMPLED MATERIALIZATION. BDA BUDGET STILL WITHHELD.**

User asked to continue locating CPU time after session 42's zero descriptor coverage.
Previous emulator was closed. Added a default-off bounded materializer observer using
the existing launch/profile workflow. It runs the reference path, measures every-call
materializer elapsed across all shader hashes, and samples about 1/32 calls for six
exclusive phases, descriptor-root word elapsed and interpreter visits/memo hits/misses.
Once-per-plan bounded graph metadata includes ReadConst → SRT dependencies and pure
word classification, so mixed sources can be distinguished from fully unsupported ones.
No shader/resource cache/synchronization changes and no extra guest reads.

Production events add probe_end_ns after recorder construction/lock/vector append.
Analyzer subtracts the union of observed emission intervals and known waits/profiler
work from CPU attribution, across both schedulers on the same thread. Older captures
default missing fields to zero; unknown costs remain uncorrectable. Scope constructor,
lock-release/trailing clock overhead and CSV serialization still require caveats.
Fine sampling closure remains enforced. Direct PrepareBda scope from session 42 is ready.

Build, materializer and resource-tracking tests pass. Profiled/unprofiled complete
snapshots/specializations match; memo reuse counters and exclusive phase closure tested.
All eight saved detiles MATCH (0.080–0.477 ms isolated). Nine Python analyzer tests pass.
Detailed docs: [materializer-expression-profile.md](materializer-expression-profile.md).
No optimization or commit made. User confirmed the paused fight looks the same.

Launched PID 1344 with -MaterializerDiagnostics -MaterializerSeconds 20 -FineCpu
-FrameCount 64 -CpuDetailEvery 8. Trace prefix:
`D:/PS5/ufc5-profiles/production-20261006-064620.csv`.
User drove to the paused fight. Light observer completed 20.000007 seconds before
the separate heavy CPU/GPU window: 375 shader hashes, 242,016 materializations,
7,637 detailed samples, 2,083.408 ms all-call materializer elapsed (~10.4% wall;
includes observer/preemption). Sample parent 84.073 ms; FlatSRT 52.900 ms (62.9%),
buffers 14.724 ms, images 11.033 ms. Zero matching phase closure failures.
ReadConst roots are 274,652 / 275,612 sampled words (99.65%); graphs commonly
include SRT/address-load dependencies. Direct GetUserData whole-source gather does
not cover these. No cross-draw freshness assumption or speedup demonstrated.

Production epochs 60801–60864: 35.800 s, 127 present API calls, zero timer/event
drops/splits/uncollected batches. Interior medians: wall 476.019 ms, foreground CPU
excluding recorded waits/submits/profiler work 257.835 ms, explicit waits 117.457 ms,
submit 9.229 ms, serialization 27.994 ms, collection 9.184 ms, observed recorder
emission 0.380 ms. These are overlapping epoch attributions, not additive display
frame budgets. Heavy capture rate 3.55 presents/s is not an uninstrumented baseline.

Ordinary readback transactions record 14,102.944 ms wait across 636 transactions.
Bases 1164b80000 / 1167f00000 / 1140008000 account for 99.48%, with means
53.497 / 29.043 / 13.754 ms per transaction. Detailed lifetime observer was off;
previous classification is CMask / CMask / indirect draw arguments. No new stale-byte
reuse proof. Median known-complete submission gap lower bound 23.668 ms/epoch;
profiler/other host work can contribute. Broad barrier entries identify patterns,
not measured barrier cost. GPU elapsed detile scopes are not pure compute cost.

Direct BDA bracket: 77 sampled calls, 13.037 ms inclusive net / 8.870 ms self;
34,590 nested buffer-synchronization visits, median 435 per call (391–751).
Binding sample closure still fails (1.564x), so BDA per-epoch budget is withheld.
State closure 1.176x passes the 1.25 guard; estimates remain sampling estimates.

Full results/ranking: [measurement-follow-up-2026-10-06.md](measurement-follow-up-2026-10-06.md).
Next CPU experiment: audit/compile FlatSRT expression structure with unchanged reads,
memo/failure/activity/specialization semantics and full shadow validation. For BDA,
get aggregate timing/clean-vs-dirty/upload counts without per-child event emission
before a clean-range guard. Readback stalls need GPU execution tracing; do not defer
publication/cache old bytes or remove barriers without proof. No massive win proven.
Both controls are off; PID 1344 remains responsive. Further windows can be rearmed
without restarting this build. Raw data and manifest retained at the prefix above.

---

Last updated: 2026-10-05 session 42 — **DIRECT DESCRIPTOR EXPERIMENT BUILT;
FIRST SHADER HAS ZERO ELIGIBLE WHOLE DESCRIPTORS. NO FAST PATH EXECUTED.**

User requested the first proposed materialization experiment. Added immutable gather
plans for complete descriptor sources containing only U32 constants/direct GetUserData.
Plans retain register-base/bounds checks, initialization, failure prefixes and existing
session memos. Unsupported sources retain the interpreter. Materializer activity,
flat-SRT refresh, strict writable provenance, captured reads and specialization remain
shared. Default off. No Vulkan synchronization, resource cache policy or resolution changes.

`profile-production.ps1 -DescriptorGather` launches the bounded shader experiment,
initial target `0xd3dcf81c43080fd0`. `set-descriptor-gather.ps1 -Mode off|shadow|on`
switches it live. Shadow returns reference output, compares eligible descriptors and
periodically compares full snapshots/specializations/read order using recorded-read
replay, without extra guest reads. On requires an eligible full-validated resource plan;
a mismatch rejects the plan and disables the thread's experiment. Aggregate CSV counters
work while the heavier production capture remains disarmed. Direct PrepareBda scope added;
fine sample probe-emission overhead remains unresolved, so existing budget caveats remain.

Build and materialization/resource-tracking tests pass. New tests cover current values,
nonzero register base, bounds/failure prefix, session memo reuse, unsupported fallback,
moved plans, exact snapshots/specializations, strict/ordinary/failed reads, inactive
sources, and deliberately corrupted candidate rejection. All eight fight replays MATCH
(0.083–0.477 ms isolated); seven analyzer tests pass. No commit made.

User drove to paused fight. PID 21632, thread 23756, launch manifest/trace prefix:
`D:/PS5/ufc5-profiles/production-20261005-230631.csv`. Live off/shadow/on/off completed.
Frozen 75 stable aggregate windows: adjacent `.descriptor-first-attempt.csv/.json`.
Off: 66,342 materializations / 729,762 descriptors; shadow: 71,586 / 787,446;
requested-on: 47,506 / 522,566. **Zero eligible calls in every mode.** Shadow ran
1,120 full comparisons, zero mismatches, using the unchanged fallback evaluator.
Every requested-on materialization was blocked from using the candidate; actual fast
materializations = 0. Median per-window materializer CPU/call: off 10.999 us,
shadow 11.378 us, requested-on 10.879 us. Off/on CPU differences are reference-path
variation, not savings. No FPS gain or real candidate A/B demonstrated.

This prototype accepts only fully pure sources. It does not measure individual pure
words inside mixed sources. Next: classify actual descriptor expression types/dependencies
and cost, then assess wordwise pure evaluation with original-order interpreter fallback
and exact validation. Inspect other hot shaders before widening coverage. Previous
materialization estimates remain withheld; this result supplies no readback-reuse proof.

Read [descriptor-gather-experiment.md](descriptor-gather-experiment.md) for controls,
implementation, validation and frozen numbers. **Emulator remains running with experiment
verified off.** The main GPU capture was not armed during these comparisons.

---

Last updated: 2026-10-05 session 41 — **CMASK AND INDIRECT ARGUMENT READBACKS
IDENTIFIED; CPU MATERIALIZATION IS THE FIRST BOUNDED EXPERIMENT.**

Read [readback-cpu-investigation-2026-10-05.md](readback-cpu-investigation-2026-10-05.md)
for address lifetimes, ranked recorded CPU calls, invalidation rules and three proposed
A/B tests. No optimization or commit was made. Existing production profiling now has
caller/PM4 context, request/window/payload distinctions, independent transaction IDs,
transfer copy brackets, publication spans, exact already-downloaded byte comparisons,
GPU reader/declared-writer/copy/fill/upload markers and finer CPU/cache/lock scopes.
Enable `profile-production.ps1 -FineCpu -Lifetime -FrameCount 64 -CpuDetailEvery 8`.
Both features default off; no additional guest reads, GPU copies or waits were introduced.

First trace `production-20261005-202527.csv` overflowed dense CPU detail; incomplete
epochs excluded. Corrected to about 1/32 outer fine-call sampling with all descendants,
added PM4 context, then performed one extra restart. Corrected trace:
`D:/PS5/ufc5-profiles/production-20261005-203855.csv`, epochs 900–963, 26.026 s,
127 present calls (~4.88/s under capture), zero CPU event drops, 49 GPU scope drops.
Incomplete GPU epochs excluded from GPU aggregates. Eight CPU detail epochs available.
These are enqueue epochs, not displayed frames. User confirmed **looks the same**.
Capture-off later reached ~6.1–6.2 FPS; no performance A/B gain is claimed.

Tiny `0x1140008000`: **indirect draw arguments, not a status poll**. All 254 faults have
`guest_draw_indirect` context; existing memcpy immediately derives parameters and updates
instance state. Fault width 1 is a reported byte, not access width. 16 KiB cached buffer,
128/448 B packed payload, mean timeline wait 6.454 ms vs transfer bracket 0.001456 ms.
Native `guest_copy_data` writes and compute descriptor writers overlap the region.
460 equal/164 changed/11 first comparisons are spans, not whole readbacks.

`0x1164b80000`: 512 KiB drain around CMask `0x1164be0000` (81920 B requested), 128
calls, mean wait 12.022 ms vs copy 0.075 ms. Full spans: 36 equal/91 changed/1 first;
changes may be outside metadata. Code 0 / CPU expansion to 0xff recorded 128 times.
Declared producer shader `0x723423f5115aedbd`. `0x1167f00000`: CMask, 36864 B
requested / 131072 B copied, 128 calls, mean wait 11.512 ms vs copy 0.006061 ms.
All 127 repeat spans match exactly. Broad declared writer shader `0xea0aceac518ec52d`
overlaps it; actual stores are unproved. No CPU expansion marker followed there.
Comparison `0x1165e00000`: same 512 KiB payload, mean wait only 0.303 ms. Buffer
handles stayed stable. All consumers currently need immediate visibility; unchanged
old bytes plus intervening dirty declarations do not establish safe caching/deferment.

Corrected medians: host excluding recorded waits/submits/profiler 233.321 ms, foreground
wait 72.593 ms, submit 7.812 ms, profiler serialization 49.913 ms / collection 8.066 ms.
Session-40 lighter trace remains the better overall reference. Whole phase medians over
8 CPU detail epochs: state 84.123 ms, bindings 72.337 ms, commit 32.140 ms. Sampled
state MaterializeResources: 1875 calls, 14.425 ms self / 14.932 ms inclusive (~74% of
the same sampled ProgramCache::Get parent). Binding samples: 48919 SynchronizeBuffer
calls. PrepareBda scans all mapped ranges for DMA draws but lacks its own bracket.

**Do not claim exact 58 ms materialization / 67 ms binding-body budgets.** Scaling
fine samples by 32 overfills state 1.84x and bindings 3.36x. Sampling outliers and
child-probe emission charged to parent self remain unresolved. Analyzer flags/withholds
scaled budgets and retains raw call totals/quantiles, epoch observations and repetitions.
Program IDs / descriptor fingerprints are partial keys, not cache validity proofs.

Next: shadow-validate a descriptor evaluation plan for U32 constants/direct current
user-data references in SrtWalker::EvaluateDescriptor, initially one hot shader. Keep
flat-SRT refresh, active sources, captured reads, clean/writable rules, failures and
specialization; unsupported cases fall back. Measure eligibility and actual savings.
Other proposed tests: current-clean-range BDA guard with protection/concurrency audit,
and pure texture-description memoization retaining all live cache/dirty operations.
Readback deferral, old-result reuse and GPU-native indirect conversion remain unproved.
Include probe-emission accounting and a direct PrepareBda bracket in the next build.

Build/harness passed; all 8 fight replays MATCH (0.080–0.478 ms isolated). 1024-iteration
query reuse/exhaustion test with Khronos synchronization validation: MATCH, no reported
warnings/errors. Seven analyzer tests pass. User confirmed same scene; no screenshot
hash comparison or optimization A/B. Capture finished; the user then closed the emulator
and confirmed the exit was intentional, not a crash. Further windows can be armed
without restart while this build is running.

---

Last updated: 2026-10-05 session 40 — **BOUNDED PRODUCTION PROFILER WORKING;
HOST COMMAND PROCESSING, SYNCHRONOUS READBACKS AND INTERMITTENT GPU INFLATION MEASURED.**

The user requested measurements before optimizations. Extended the existing scheduler
timestamp and CPU draw-phase infrastructure: detile/upload transaction endpoints,
compute-stage dispatch brackets, image first-consumer markers, barriers with unchanged
arguments, host waits, submission cadence, native queue identity, and source allocation
metadata. Producer and presenter traces are now separate so they cannot overwrite one
another. Query pages are reset outside dynamic rendering and reused only after their
own timeline completes; query reads are nonblocking. Exhaustion drops measurements,
not game work. No synchronization, shader, resolution or allocation-policy optimization
was made. The native guest output is restored for this capture.

The user drove the profiling build back into the fight and reported "in". Capture:
`D:/PS5/ufc5-profiles/production-20261005-193728.csv`, epochs 895–958, 64 epochs,
27.868 seconds, 127 presentation calls (~4.56 calls/s). Capture-off immediately before
arming reached 5.526 FPS; rates fluctuate. These are not exact budgets for every prior
6–7 FPS run. **GetFrameNum is a SuspendPoint enqueue counter, not a completed display
frame. About two presentations per epoch here. Do not read 390 ms/epoch as 2.56 FPS.**

Medians over 59 complete interior epochs: wall 390.937 ms; guest processing elapsed
excluding recorded waits/submits/profiler 248.667 ms; foreground explicit waits
91.184 ms; submits 8.088 ms. Measured profiler serialization 25.894 ms and collection
8.975 ms. The CPU categories are elapsed scopes, not thread-cycle measurements.
GPU elapsed unions: batch 142.392 ms, render 37.370 ms, guest compute 64.512 ms,
detile dispatch 12.544 ms, copies/clears 14.353 ms. Different clock domains and
pipeline overlap prevent adding CPU/GPU totals. These are not active GPU counters.

Synchronous buffer readback's actual timeline wait at bufferCache.cpp:257 occurred
746 times / 8,422.401 ms total. Three addresses own ~99% of that wait: 0x1164b80000
(524288 bytes, 3602.109 ms), 0x1140008000 (128/448 bytes, 2750.567 ms), 0x1167f00000
(131072 bytes, 1983.474 ms). A tiny readback can wait for the whole prior batch;
do not call this the payload's copy cost. Background worker waits are separate.
No steady-state queue-idle, native fence-wait or ownership-transfer calls observed.
ALL_COMMANDS→ALL_COMMANDS EmitGlobalBarrier: 171156 calls across 64 epochs.
Broad barrier resource entries median 5542/epoch; counts do not establish cost.

Median submits: 339/epoch on one physical Vulkan queue. A conservative host-completion
check finds 23.997 ms/epoch with no outstanding captured command buffer; some gaps
include profiler work. State and binding preparation are the largest sampled CPU
draw phases after explicit waits are removed (~79.5 and 72.3 ms/epoch, only 3 samples).
Do not infer that these phases can safely be threaded from spare cores alone.

Intermittent episode 925–934: 7 complete epochs show detile dispatch elapsed median
309.694 ms versus 11.730 ms in the other 52 complete epochs; foreground waits grow
87.790→344.883 ms while guest host work stays ~245–250 ms. Source 0x116abb0000's
COMPUTE_SHADER-bracketed median grows 0.750→29.857→0.926 ms before/during/after.
Thus the spike is not explained solely by the earlier TOP/BOTTOM pre-barrier range.
Compute timestamps still include stalls/preemption; pure shader execution is unknown.

Those production source observations report memory type 0 / property flags 0x0
(no DEVICE_LOCAL), unchanged through the episode. Replay requests DeviceLocal but
its actual allocation type was not logged. A confirmed placement comparison is
outstanding. Do not equate Vulkan allocation flags with current physical residency
or claim they caused the slow episode. The earlier 31.944/14.427 ms timings were
from a separate replay process alongside the game, not direct production calls.

Coverage: 32 GPU scopes dropped in epochs 927/928/930; those epochs excluded from
aggregate medians. Zero event drops, split scopes or missing batch scopes. The first
dense trace (production-20261005-190801.csv) had ~131 ms/epoch serialization overhead,
1.54 GB events and 58 dropped scopes; it must not be the normal performance budget.
Lighter trace events are ~199 MB. Sampling draw intervals every 16 epochs preserves
all-epoch phase counters; unavailable per-draw detail is blank, not zero.

Build/harness passed. Eight isolated fight replays remain reference MATCH, medians
0.082–0.477 ms. A 1024-iteration query lifetime/exhaustion test passed with Khronos
synchronization validation, output MATCH, no warnings/errors. Four analyzer regression
tests passed (overlap, thread separation, wrap, secondary scheduler discovery, sampling,
readback resource/metadata/wait joins).

**Report:** `D:/PS5/src/KytyPS5/ufc5/production-profile-report-2026-10-05.md`.
**Usage:** `D:/PS5/src/KytyPS5/ufc5/production-profile.md`.
**Raw generated breakdown:** capture base + `.analysis/report.md`, with per-frame,
detile transactions, readbacks, wait/barrier sites and disk-backed events.sqlite.
Executable identity/options/FPS-log offset are in capture base + `.manifest.json`.
PID30960 is still running, capture finished/control off. Re-arm another bounded
window with `ufc5/tools/profile-production.ps1 -Action Arm`; no reboot needed.
Launcher count/detail options apply at launch; Arm uses the configured window length.

Next: sampled CPU stacks inside state/binding preparation, trace producers and guest
visibility requirements of the three hot readbacks, then native GPU scheduling evidence
during a detile spike and a confirmed replay/production memory-type comparison.
WPR lists CPU/GPU profiles but session25's GPU capture was blocked by Windows policy
(0xc5585011); availability is not permission to record. Reuse working CDB/Tracy tooling
where appropriate. No guessed barrier removal or threading patch yet. No commits made.

Last updated: 2026-10-05 session 39 — **1080P FREES VRAM; USER REPORTS IDENTICAL FPS AND BROKEN SCENE.**
The user reached the fight in the output-resolution test, reported about 6.9 GB
VRAM use and no FPS improvement, and supplied a screenshot with a rendered HUD
over a mostly black scene, cyan/blue along the left edge and a small scene remnant.
The title bar reads 7 FPS; this is a snapshot, not a sustained rate measurement.
The user has seen a similar image in shadPS4; no common cause is established.

The saved trace confirms the game reduced internal surfaces: 1068×600 scene
surfaces, 1600×900 color/depth, and 1920×1080 depth, compared with the earlier
2136×1200 / 3200×1800 / 3840×2160 surfaces. Pitches remain guest-defined
(1152, 1664, 1792, etc.); the override did not resize host images behind the guest.
The final five census rows show native image allocation bytes about 2063–2152 MB
and driver usage 6456–6560 MB against budgets 7249–7296 MB. The earlier native
fight sample had 3460 MB image allocations and driver usage 8000 MB/budget7195 MB.
Units here follow the census's MB label (actually MiB); the user's 6.9 GB display
is a separate reading. This genuinely reduced the measured device-memory pressure.
It did not eliminate all host/shared memory: census host usage stayed about638 MB.

The user reports the FPS stayed identical. Recent present-rate rows include
6.689 and 6.677 FPS, then 3.967 FPS, but the append-only log has no scene markers
and is not a matched native/1080p control. Do not claim an FPS win. Removing the
over-budget state did not deliver the large gain sought, so shared-VRAM spill
alone is not supported as the dominant cause. The visual corruption limits how
far this comparison can be generalized to a correctly rendered frame.

The test process was already gone when inspected; closure versus crash is
unconfirmed. The saved game log has no final explicit fatal message. Logs and
trace remain at the session38 paths. Keep 1080p opt-in as a diagnostic, not the
normal configuration. No guessed shader or image-layout fix was applied.

Next prioritize the surrounding upload/barrier/submission sequence and CPU/GPU
serialization, expanding the replay beyond isolated detiles. Obtain matching
CPU wall/wait and GPU intervals before choosing an optimization. A pure memory
pressure replay is now secondary. For the lower-resolution graphics issue,
identify the first bad pass (scene color → temporal history → final composite)
and check resource/view/pitch handling before assigning a NaN or shader cause;
historical black-frame work used these same dimensions but is not a diagnosis
of this build. Do not require another boot just to repeat the VRAM hypothesis.

Last updated: 2026-10-05 session 38 — **LOWER OUTPUT RESOLUTION TEST PREPARED.**
The user asked whether a temporary low resolution could free VRAM. Code inspection
found `VideoOutGetOutputStatus` reports 4K to UFC 5: `ATTRIBUTE3=6029312` has its
resolution-detection bit clear. A smaller host window does not change that report.
Added an opt-in `KYTY_VIDEO_OUT_1080P=1` override for the main video-out port to
report the existing 1080p output mode. Default behavior is unchanged. This asks
the game to choose smaller internal surfaces; it does not forcibly rescale images
or guarantee the guest changes its rendering size.

The emulator build and PowerShell parser checks passed. The test launcher stages
`kyty_emulator.resolution-test.exe` separately and records registered output sizes,
internal detile sizes, census/memory counters, and present rate. Live GPU timing
and detile capture are disabled. The same script with `-Mode Native` provides a
control with identical settings except the output report. See
[resolution-test.md](resolution-test.md).

The 1080p test has been launched (PID 24456); **fight results are pending**.
The guest queried the overridden output status and registered 1920×1080 output
buffers during boot. Internal fight resolution and VRAM changes are not yet known.
Log: `D:/PS5/KytyLog-UFC5-resolution-1080p-20261005-181434.txt`.
Detile trace: `D:/PS5/ufc5-detile-resolution-1080p-20261005-181434.csv`.
Before launch,
`D:/PS5/tex-census.txt` had 748 lines. Its last baseline fight sample reported
3372 images, 3380 MB guest image bytes, 3460 MB native image allocations, 4419 MB
VMA blocks, and driver usage 8000 MB against a 7195 MB budget. This is an earlier
fight sample, not a matched same-build resolution control.
Next verify the guest queries the override, reaches the same paused fight, and
actually reduces surface dimensions. Compare VRAM and FPS separately: lower
resolution also reduces pixel work, so an FPS gain alone cannot prove spill was
the cause. No performance or VRAM improvement has been measured yet.

Last updated: 2026-10-05 session 37 — **DETILES ARE FAST WITH THE GAME CLOSED.**
The user closed the game; no Kyty emulator process remained. Two fresh-process
replays of the same eight fight captures, each with ten warmup and fifty measured
iterations per capture, succeeded with every output byte matching. The complete
commands each finished in about 1.7 seconds. No renderer source changed between
the game-open verification and these runs.

| Guest address | Game-open GPU median (ms) | Closed baseline A (ms) | Closed baseline B (ms) |
|---|---:|---:|---:|
| `0x1164920000` | 1.253 | 0.082 | 0.083 |
| `0x11673b0000` | 0.153 | 0.154 | 0.154 |
| `0x1167460000` | 0.173 | 0.175 | 0.175 |
| `0x11687e0000` | 1.270 | 0.084 | 0.084 |
| `0x1168bd0000` | 0.268 | 0.250 | 0.250 |
| `0x11691a0000` | 1.726 | 0.083 | 0.083 |
| `0x116abb0000` | 31.944 | 0.477 | 0.477 |
| `0x1172520000` | 14.427 | 0.175 | 0.175 |

CSV: `D:/PS5/ufc5-detile-replay-baseline-20261005-a.csv` and
`D:/PS5/ufc5-detile-replay-baseline-20261005-b.csv`. The original concurrent run
used fewer iterations and a different surrounding GPU load; this is not a
controlled residency-only A/B. The large detiles can execute quickly in isolation.
Closing the game removed its queued GPU work and allocations together, so this
does not identify whether scheduling/contention or residency caused the long
game-open timings. These per-call medians are not whole-frame GPU cost or FPS.

Next: add a bounded, optional memory-pressure mode to the standalone replay,
with resident allocations actually touched and memory-budget measurements, to
test whether pressure alone recreates the long dispatches without another game
boot. Keep the original replay baseline and byte comparisons. If pressure does
not reproduce it, extend replay toward command ordering and upload/cache state.

Last updated: 2026-10-05 session 36 — **REAL FIGHT DETILES REPLAY WITHOUT BOOTING THE GAME.**
Implemented the first part of the faster debugging workflow: save logical tiler
layouts and the actual GPU input/reference output, then replay them through the
production `TileManager` in the headless Vulkan harness against newly built source.
This is a detile workload capture, not a game save state or a full frame replay.

The user drove `kyty_emulator.detile-capture.exe` to the fight once. Arming
`D:/PS5/ufc5-detile-capture-control.txt` saved eight large, distinct guest surfaces
from frames 805–806 in `D:/PS5/ufc5-replays/fight/`. Capture is now **off**; the
game stayed running and responding. All eight replayed in a separate process on
the RTX 3070, **every output byte matched**, and the complete command took
**4.30 seconds** (three warmups plus ten measured iterations per capture).
CSV: `D:/PS5/ufc5-detile-replay-fight-20261005.csv`.

GPU medians ranged from 0.153 to 31.944 ms per isolated detile. The two slowest
were `0x116abb0000` at 31.944 ms and `0x1172520000` at 14.427 ms. The emulator
was still using the GPU throughout this run, so these are a correctness and
workflow demonstration, not an uncontended performance baseline. Do not add
these timings into a game frame estimate or claim they explain the slow state.

Builds of the emulator and replay harness succeeded. Independent CPU-reference
fixtures for color/depth and 4/8-byte elements matched; the actual GPU capture
hook also round-tripped in a fresh process. Corrupt payload, truncated file, and
unsupported format were rejected (exit 1); a valid file with deliberately changed
reference output reported a mismatch (exit 2).

Normal experiment loop, with no game boot or menus:
```powershell
& D:\PS5\src\KytyPS5\ufc5\tools\replay-detile.ps1 -Build
```
The capture executable is staged separately; the normal deployed executable was
not replaced. Live GPU timing remains disabled. Captures use a versioned disk
format with checksums, dimensions/size validation, and bounded readbacks.
See [detile-replay.md](detile-replay.md) for capture and replay commands.

Next: establish an uncontended replay baseline, use these saved workloads for
tiler changes and output regression checks, then extend capture to the surrounding
upload/copy sequence and resource/cache state if needed. This tool does not yet
cover cache reuse, D16 promotion, BGRA conversion, draws, guest CPU execution, or
whole-game residency. Occasional game runs still check visuals and actual FPS.

Last updated: 2026-10-04 session 35 — **TIGHT IMAGE USAGE DOES NOT MOVE THE DRIVER GAP.**
`KYTY_TIGHT_IMAGE_USAGE=1` creates sampled images without storage or color-attachment usage, and adds a flag only when that binding is used (copying the old image across). The fight still renders. `D:/PS5/tex-census.txt` in that fight: ~4,200 images, ~3.3 GB, duplication 1.00. Of that, storage is ~2.2 GB, color targets ~0.5 GB, both ~0.06 GB, and sampled-only ~0.5 GB. Vulkan image allocations match the accounted bytes (~3.3 GB). Driver usage stays ~7.8–7.9 GB against a ~7.0–7.3 GB budget. VMA blocks ~4.3 GB, so the driver-minus-VMA gap is still ~3.5 GB, the same fight as session 17. Upgrades during the fight: ~560. Stripping the flags from the half-gigabyte that never needs them did not shrink the gap. Leave the flag off. Do not treat storage/color usage as the unaccounted copy.

Last updated: 2026-10-04 session 34 — **DROPPING SAMPLED TEXTURES FREES THE CARD AND NEVER REACHES THE MENU.**
`KYTY_DROP_TEXTURES=1` discards images not used this frame and serves sampled lookups a 1×1 image. During the black boot, driver usage sat near 1.4 GB against a 7.4 GB budget, with about 30 images left. The picture never reached the menu, so there is no fight FPS. The gentle drop that kept the picture stayed pinned near the budget because the game rebuilt the textures. Do not use the drop as a frame-time test. Leave the flag off.

Last updated: 2026-10-04 session 33 — **FRAME-BASED CACHE AGING IS ABOUT 1 FPS, NOT THE SUBNAUTICA CUT.**
Ported the presented-frame tick from upstream PR #955 (`TickFrame` on flip;
buffer and texture GC no longer advance age per submission). The SRT invariant-phi
cache from the same PR is included. Lock-free dirty reads were not ported. The
user's fight read was a possible +1 FPS. `D:/PS5/tex-census.txt` during that run
holds ~3,400–3,500 images and ~3.3 GB, duplication factor 1.01, trigger 0 MB,
critical 5,223 MB. Driver usage stays ~7.8–8.0 GB against a ~7.1 GB budget.
Images are not being recreated in the hundreds per frame, so the Subnautica
15.8→21 path (121 image re-creations per frame down to 0) is not this fight.
Leave the frame tick in; it is not a 30 FPS lever.

Last updated: 2026-10-04 session 32 — **OFF-THREAD DRAW COMMIT DOES NOT CHANGE FPS.**
`KYTY_DRAW_COMMIT_THREAD=1` boots after the commit packet keeps its own copy of
the shader stage pointers. `D:/PS5/ufc5-draw-commit.csv` through frame 999, in
the heavy scene (~4,800 draws/frame): commit time is about 37–54 ms and the
guest thread waited about 64–96 ms for it. Wait stays above commit, so the next
command-buffer user arrives immediately and the recording is still serial.
Leave the flag off. Do not treat “prepare the Vulkan commit on another thread”
as a frame-time cut.

Last updated: 2026-10-04 session 31 — **ASYNC COMPUTE QUEUE IS NOT THE NEXT CUT.**
`D:/PS5/ufc5-queue-overlap.csv` covers the fight through frame 1840. Steady fight
frames (about 1457 onward, 384 rows) start ~285 graphics submissions and ~163
compute attempts. Graphics `WAIT_REG_MEM` blocked 4 times in those 384 frames,
about 6.6 ms total, and the all-queues-idle poll matched that. Compute attempts
block on `WAIT_REG_MEM` ~139 times a frame, and every compute attempt starts
while graphics is also queued. Only ~22 graphics submissions a frame start with
a compute queue actually runnable, and the runnable peak is always 1 even though
~4 compute submissions sit queued. Those queued submissions are blocked, not
ready. Do not add a second Vulkan queue on this evidence. The recording thread
is already full. Session 32 tried the Vulkan draw commit on a second thread;
the guest thread waited the whole commit, so that did not shorten the frame.

Last updated: 2026-10-04 session 30 — **DETILE REUSE IS ON FOR A FIGHT CHECK.**
Surfaces of at least 8 MB keep their linear detile until the guest bytes are
invalidated, capped at 160 MB. A later alias with the same address, size, and
tile layout uploads that copy instead of running the tiler again. D16 promotion
and BGRA16 swaps still detile. `KYTY_DETILE_REUSE=0` turns it off. GPU timing
stays off on this executable. The trace column `reuse` is 1 when the copy was
used. Binary: `kyty_emulator.detile-reuse.exe`.

Last updated: 2026-10-04 session 29 — **REPEATED LARGE DETILES ARE SEPARATE
CACHE IMAGES OF THE SAME GUEST SURFACE.** The fight trace is usable. GPU
timestamps are not: turning `KYTY_GPU_TIMING_CONTROL_FILE` on crashed the
emulator, same as run 17, after only partial frames 2884–2885. Timing is off
again. The CPU log still shows the familiar transition at 4,834 draws/frame:
frames 2876–2879 at 555–675 ms, then 2880–2887 at 354–402 ms.

`D:/PS5/ufc5-detile-trace-run18.csv` covers that fight. The large output sizes
from run 16 are stable guest addresses, and each repeat inside a frame is
usually a different image slot, not a second upload of one image. In frame
2884, which matches the surrounding frames:

| Guest address | Linear bytes | What | Repeats | Image slots |
|---|---:|---|---:|---|
| `0x1168bd0000` | 33,423,360 | depth 3840×2160, slot 42, 480×270 groups | 2 | 1056, 2131 |
| `0x1172520000` | 23,040,000 | color 3200×1800, slot 37, 400×225 | 2 | 1903, 1930 |
| `0x1167460000` | 23,040,000 | color 3200×1800, slot 37, 400×225 | 2 | 3049, 2496 |
| `0x11673b0000` | 20,889,600 | color 2136×1200, slot 38, 267×150 | 2 | 2090, 1539 |
| `0x11687e0000` | 11,141,120 | depth 2136×1200, slot 42, 267×150 | 4 | 1903, 2131, 1930, 2344 |

Those copies are marked buffer-modified, CPU-dirty, and GPU-modified, so the
cache believes each alias needs a full detile. One smaller exception,
`0x1164920000`, is the same image slot uploaded twice. The next change should
stop a second alias from detiling a surface that was just detiled for another
image of the same guest bytes. Do not enable the stage-split GPU timers on
this executable; use `kyty_emulator.tiler-timing.exe` if whole-detile timestamps
are needed again.

Last updated: 2026-10-04 session 29, earlier — **DETILE IDENTITY TRACE ADDED.**
Run 16 already showed `0x15f9000` four times and `0x1fe0000` twice per frame in
both GPU states. The trace above identifies those sizes as repeated guest
addresses. The executable with the trace is `kyty_emulator.exe`; the previous
whole-detile timer build remains `kyty_emulator.tiler-timing.exe`.

Last updated: 2026-10-04 session 28 — **INTERNAL DETILING IS THE LARGE GPU
BOTTLENECK IN THE SLOW STATE.** Run 16 timed `TileManager::Record` and captured
the same paused fight in slow and fast states. Across frames 1052–1056 vs
1058–1060, whole GPU submissions averaged 467.6 vs 127.2 ms/frame, rendering
32.7 vs 32.5 ms, and detile calls 330.9 vs 18.2 ms. The detile difference
(312.6 ms) explains most of the submission difference (340.4 ms). Comparable
large detile buffers recur with the same call counts in both states; for
example, two 20,889,600-byte calls total 67.9 ms in slow frame 1053 versus
4.4 ms in fast frame 1059. Run 12's “all compute” markers covered **guest
compute only**; they missed these renderer-internal tiling shaders.

Run 17 split each detile call into pre-barrier/clear, dispatch, and final
barrier. Slow frame 3194: 331.8 ms total detile, of which 36.1 ms was pre,
294.6 ms dispatch, and 0.1 ms post. Fast frames 3196–3204: ~33–35 ms total,
~32–34 ms dispatch and ~1 ms pre. Thus the slow-state jump is mostly within
the detile dispatch interval. This does **not** yet distinguish expensive
shader work from memory residency/bandwidth during that dispatch. The run 17
emulator exited/crashed shortly after the live capture was enabled; no Windows
Application Error entry was found. Treat the extra stage-timing build as
diagnostic only. The previous run 16 executable has been restored to
`D:/PS5/Emulators/KytyPS5-Bin/kyty_emulator.exe`, and live timing is off.
Next: inspect repeated `TextureCache::UploadImage` detile calls by guest
address, tile geometry, pipeline slot, and dispatch group count; determine why
large surfaces are uploaded multiple times per frame. A targeted cache/reuse
or detile shader change should follow only after matching slow/fast calls.
Raw captures: `D:/PS5/ufc5-gpu-timing-run16.csv` and
`D:/PS5/ufc5-gpu-timing-run17.csv`, with corresponding `.cpu.csv` files. See
[performance-audit-2026-10-03.md](performance-audit-2026-10-03.md).

Last updated: 2026-10-04 session 27 — **THE ~400-MS SLOW GPU COST SITS IN
NON-RENDER COMMAND-BUFFER GAPS.** New opt-in timers bracket each interval outside
dynamic rendering. In slow paused-fight frames at ~5,046 draws/724 dispatches,
whole submissions were 449–484 ms/frame; rendering 34–38 ms, non-render gaps
409–444 ms. Two 1-draw/2-compute submissions each spent ~56 ms in the first
gap before a 0.028-ms render pass. Two 128-draw submissions each spent ~36 ms
before their first render pass, which took 4–5 ms. This does not yet identify
whether a particular transfer/barrier or a residency/page-in stall owns the
gap. Next mark commands in those first gaps with transfer byte counts/resource
IDs and compare fast vs slow; avoid changing draw shaders or color metadata on
this evidence. See [performance-audit-2026-10-03.md](performance-audit-2026-10-03.md).

Last updated: 2026-10-04 session 26 — **THE SLOW GPU TIME IS OUTSIDE ACTUAL
RENDERING PASSES.** Opt-in per-pass Vulkan timestamps captured the same paused
fight in both states: 4,790/4,794 draws per frame, whole GPU submissions
479.8/137.9 ms slow/fast, but rendering passes 34.1/31.8 ms and metadata copies
~0.3 ms in both. Run 12 showed guest compute ~32–38 ms in both; it did not
include Kyty's internal detile dispatches. One
60.56-ms submission spent only 0.03 ms in its render pass; two recurring
43–45-ms mixed submissions spent only 0.13 ms in rendering each. The extra
~342 ms is in non-render Vulkan commands or stalls between them. Next time
the intervals between render passes and correlate them with copies/barriers or
device residency. See [performance-audit-2026-10-03.md](performance-audit-2026-10-03.md).

Last updated: 2026-10-04 session 25 — **A MATCHED FAST/SLOW/FAST RUN PUTS THE
EXTRA FRAME WALL ON THE GPU, NOT DRAW-STATE CPU WORK.** Opt-in six-phase CPU draw
timers measured 344/745/364 ms wall and 154/605/157 ms GPU submissions across
fast/slow/fast windows in the same paused fight. Timeline waits grew 86→445→97 ms;
draw-state CPU time grew in step because it includes those waits. Bindings stayed
~63–68 ms and draw recording ~25–28 ms/frame. The same 81-draw submission grew
8.8→54.7 ms and a recurring 4-draw/8-compute submission ~10→57–61 ms. Process
local/nonlocal GPU memory counters changed gradually while GPU time switched
abruptly. WPR GPU trace could not start due Windows profiling policy (`0xc5585011`),
and Kyty `--rd` failed at Vulkan device creation; normal mode remains running.
Next time graphics draw groups and resource transitions inside the long submits.
See [performance-audit-2026-10-03.md](performance-audit-2026-10-03.md).

Last updated: 2026-10-04 session 24 — **THE SLOW GPU PHASE IS NOT COMPUTE OR
METADATA DOWNLOAD.** A short live capture timed every compute dispatch by shader
hash. At ~4,904 draws/frame, complete fast vs slow windows measured whole GPU
submissions 131 → 502 ms/frame, but guest compute 38 → 32 ms/frame and metadata
downloads ~0.3 ms/frame in both. The top compute shader used 11.5 ms/frame fast;
occlusion shader `0xea0aceac518ec52d` used only ~0.66 ms/frame. One recurring
command buffer rose from ~11 to ~90 ms/frame, with only ~4 ms compute per slow
instance. The extra GPU time is in its draws/uploads/barriers or residency, which
the current markers cannot yet separate. CPU non-wait time remains roughly
260-300 ms/frame. Next instrument those exact phases and correlate with WDDM
residency rather than pursuing color metadata or the occlusion compute shader.
See [performance-audit-2026-10-03.md](performance-audit-2026-10-03.md).

Last updated: 2026-10-04 session 23 — **THE METADATA COPY IS TINY; GPU WORK AND
CPU COMMAND PROCESSING BOTH MATTER.** Opt-in Vulkan timestamps and CPU frame totals
were captured in a stable paused fight with the sparse BDA table. Enabling GPU
timestamps live left the first matched rate essentially unchanged (2.99 off,
2.91 on FPS; ~4,875 draws/frame). A later slower phase persisted with timestamps
off and on at 1.33 FPS despite the same draw count. Fast vs slow: GPU submission
time 168 → 627 ms/frame, CPU timeline wait 53 → 457 ms/frame, while CPU submit
cost stayed ~9 ms/frame. The actual GPU metadata download and barriers were only
~0.4 ms/frame. The prior large `ReadMemory` wall time was mostly a wait for
preceding GPU work. About 280-300 ms/frame remains on the CPU outside measured
timeline waits and queue submission; 18 thread stacks spread across draw/resource
preparation, SRT/BDA, GC, and allocation. Driver local use was ~0.4-0.5 GB over
budget, but paging is not yet proven as the reason GPU time quadrupled. A new
build that can timestamp every compute dispatch by shader hash is ready for one
more targeted capture. See [performance-audit-2026-10-03.md](performance-audit-2026-10-03.md).

Last updated: 2026-10-04 session 22 — **LIVE COLOR-METADATA SWITCH WORKS; TWO
READBACKS REMOVED, NO LARGE FPS WIN.** The producer trace identified compute shader
`0x723423f5115aedbd` as a proven uniform buffer fill. Added an opt-in CPU-backed
CMask fill for exact registered metadata ranges and a live `on|off|status` control
script (`ufc5/set-color-meta.ps1`), so the paused fight can be A/B tested without
restarting. The user confirmed identical appearance. In five 12-second alternating
windows, the switch consistently changed six dirty CMask reads/frame to four, but
FPS remained variable and low: off 2.74/1.67/1.92 versus on 1.92/2.67. The two
remaining CMask ranges overlap writable storage bound to compute shader
`0xea0aceac518ec52d`. One also binds the uniform-fill shader but did not meet
the bypass's full-coverage/exact-CMask checks; its rejection needs tracing.
Run 8 used the dense 768 MB BDA table and was roughly 1.1 GB over its local driver
budget, so its absolute FPS is not directly comparable to sparse-table runs. This
path has not shown the massive win required; next measure GPU stages with timestamps
before further CPU/readback changes. Details and raw data are linked in
[performance-audit-2026-10-03.md](performance-audit-2026-10-03.md).

Last updated: 2026-10-04 session 21 — **COLOR-METADATA READBACK IS MEASURED;
DMA FILL SHADOW IS NOT APPLICABLE TO THE TRACED FIGHT.** Added opt-in per-frame
`KYTY_COLOR_META_PROFILE` and producer event `KYTY_COLOR_META_TRACE` diagnostics.
The first paused-fight capture held a repeatable 414-frame pattern of 1,800 metadata
checks, six dirty readbacks, and four uniform clears per frame. The six synchronous
`ReadMemory` calls averaged 211 ms, but each wait can include previously queued GPU
work, so that is not a predicted FPS gain. The event-traced run recorded thousands of
CMask readbacks and zero `FillBuffer` constant fills; a fill shadow has no observed
hits. The two expensive recurring CMask ranges should be traced to their true GPU
producer, then assessed for a GPU-side metadata resolve. See the follow-up section in
[performance-audit-2026-10-03.md](performance-audit-2026-10-03.md). A first event-trace
build crashed due to calling `setvbuf` after `fprintf`; the dump pinpointed the CRT
invalid-parameter abort, and the call was removed. The corrected trace run is stable.

Last updated: 2026-10-03 session 20 — **CURRENT PAUSED-FIGHT AUDIT FINDS A
SYNCHRONOUS COLOR-METADATA READBACK ON THE DRAW PATH.** Full report:
[performance-audit-2026-10-03.md](performance-audit-2026-10-03.md). In 27 non-invasive
guest-GPU-thread stack snapshots, 13 caught a synchronous buffer-download GPU wait;
7 of 12 deeper snapshots traced that wait to `TextureCache::MaterializeColorClear`
during render-target resolution. A 30-s fixed-scene sample averaged 67.9% whole-GPU
utilization and 0.74 process CPU cores; the guest GPU thread used 0.56 core in a
separate 12-s sample. These samples identify a dependency wall, not an exact frame-time
percentage. Measure readback counts and wait time before changing it. More CPU workers
alone cannot unblock a draw that needs GPU-produced DCC/CMask metadata.

Last updated: 2026-10-03 session 19 — **PR #1015 SPIR-V `Performance` OPTIMIZATION
WORKS, BUT FIGHT FPS IS UNCHANGED.**

Integrated [upstream PR #1015](https://github.com/KytyPS5/KytyPS5/pull/1015) into the
`ufc5-v2` working tree. The only manual integration in `pipelineCache.cpp` was passing
`Config::GetShaderOptimizationType()` into guest shader compilation; the PR's other
changes applied directly. Fixed the compute test's pre-existing stale `PipelineCache`
constructor call (it now supplies a `MasterSemaphore`). The emulator and shader test
targets built successfully. `shader_cfg`, `shader_optimizer`, and
`shader_optimization_gpu` all passed.

Launched UFC 5 with `KYTY_SPARSE_BDA=1`, `KYTY_VRAM_TEST_FPS=1`,
`--shader-validation true`, and `--shader-optimization-type Performance`. The user drove
into the fight and confirmed it looked the same as the earlier `None` run. Excluding
first-use shader compilation/loading, ten in-fight 10-second present-rate windows were
**2.871–5.784 FPS, mean 4.334 FPS**. The comparable earlier sparse-table `None` fight
was **2.9–5.8 FPS**, with recent stable windows **4.3–5.8 FPS**. This experiment does
not show a steady fight FPS gain from #1015. First-use shader compilation was visibly
longer in this `Performance` run, but no timed `None` loading baseline was collected.
The game exited cleanly; no new shader validation error was observed. The direct
emulator's default remains `None`, so #1015 is opt-in for this test.

Last updated: 2026-10-03 session 18 — **SPARSE BDA TABLE CUTS LOCAL AND SHARED MEMORY;
FIGHT FPS REMAINS 3-6. SHARED-MEMORY SPILL IS NOT THE MAIN FPS CAUSE.**

Session 18 is an opt-in experiment (`KYTY_SPARSE_BDA=1`), not yet a default change. The
16 KB guest-buffer granularity is unchanged. Instead of reserving a dense 768 MB GPU lookup
table for 1.5 TB of guest address space, shaders read a small root table and a device-addressed
leaf for each 64 MB guest region actually touched. Leaves live in 64 MB GPU chunks. In the fight,
71 leaves fit in one chunk: **64 MB total table allocation, saving about 704 MB**. The user confirmed
that the fight rendered the same as the baseline; `--shader-validation true` found no new SPIR-V
error during this run.

Comparable fight samples: baseline dense table `VMA local=4745 MB`, `driver local=8171 MB`,
`budget=7253 MB`, Windows shared GPU memory about **2.3 GB**, FPS **3-6**. Sparse table:
`VMA local=4292 MB`, `driver local=7627 MB`, `budget=7209 MB`, Windows shared about
**1.66 GB**, FPS windows **2.9-5.8** with recent stable windows **4.3-5.8**. The local-memory
overage fell from about **0.9 GB to 0.4 GB**, and shared use fell by about **0.6-0.7 GB**, yet
the fight remained in the same 3-6 FPS band. About 1.5 GB of current VMA allocations are on
host-visible heaps, so Windows shared usage cannot be expected to reach zero merely by fixing
local-budget pressure. The host-visible VMA number and Windows shared counter are different
measurements; their subtraction is only a rough guide to spill. This A/B strongly argues that
spill is not the dominant FPS limit, although removing the remaining overage would make the test
cleaner. GPU utilization reached 97% in a recent sparse fight sample.

Earlier session 18 diagnostics: a clean-texture pressure sweep cut image count from ~4300 to
~1400 but released only ~0.1-0.2 GB and did not improve FPS. `VK_EXT_memory_budget` snapshots
around shader-module and pipeline creation showed no large in-call increases; a ~2.1 GB rise in
driver-minus-VMA usage happened between those calls, so its exact owner remains unidentified.
The Vulkan pipeline cache is disabled in dirty builds, removing it as the source in these runs.

**Open upstream PR scan, 2026-10-03.** Fetched `origin/main` at `0e8ded3` (five commits ahead of
our base `b11aea8`); the PRs below are still open and absent from this branch. Review their
individual changes rather than merging a performance grab bag:

| PR | UFC 5 decision |
|---|---|
| [#1015](https://github.com/KytyPS5/KytyPS5/pull/1015), configured SPIR-V optimization | Best controlled shader experiment. Current `None/Size/Performance` setting is not applied to guest shader output. PR validates output and reports 4.2-9.5% fewer words across 11 compute fixtures, but no FPS data. Test the three UFC pixel ubershaders and fight FPS with `Performance`; it cannot by itself remove a 161-case state-machine loop. |
| [#506](https://github.com/KytyPS5/KytyPS5/pull/506), scheduling/resource caching | Potential RELEASE_MEM batching and cheaper read-only compute barriers. No in-game FPS A/B. Port and measure those pieces separately. Its texture-retention change raises cache residency, risky while UFC is still over local budget. Old base means the whole commit may not apply cleanly. |
| [#955](https://github.com/KytyPS5/KytyPS5/pull/955), frame-based cache aging/hot paths | Draft, currently conflicts with main, five separable commits, no FPS measurements. Cache-aging changes may reduce reuploads but alter eviction and memory; defer until measured. |
| [#982](https://github.com/KytyPS5/KytyPS5/pull/982) and [#988](https://github.com/KytyPS5/KytyPS5/pull/988), driver/recompiler disk caches | Strong candidates for repeat-launch compile stalls. #982 would enable the driver cache on dirty builds. They do not address steady in-fight frame time. |
| [#977](https://github.com/KytyPS5/KytyPS5/pull/977), pressure eviction | Already A/B tested here: its scan-past-protected-candidates change regressed 3-6 to 1-2 FPS; reverted. Do not reapply wholesale. |
| [#420](https://github.com/KytyPS5/KytyPS5/pull/420) and [#578](https://github.com/KytyPS5/KytyPS5/pull/578), per-draw allocations/SRT memo | Older perf PRs largely superseded: this branch already retains `ResourceSnapshot` in `ProgramCache::SourceEntry` and uses reusable dense `SrtWalker` evaluation context, not a fresh unordered-map memo per draw. |

Other open PRs (#925, #978, #985, #986) address DMA/resource correctness or loop safety,
not the measured steady FPS. #978's BDA page-bounds correction is worth reviewing before making
the sparse BDA experiment default. The old `SplitSharedTerminalBlocks` pre-pass was
already ported to `GotoStructurizer` in session 17: visuals stayed correct but FPS did
not move, so it is not a current performance lead.

Previously: 2026-10-03 session 17 — **THE WALL WAS THE BUFFER GC. 1 → 3-6 FPS ON UPSTREAM.
PR #977 THEN REGRESSED IT TO 1-2; REVERT POINT IS `7c8f2bb`.**

**BEST KNOWN CONFIGURATION: branch `ufc5-v2` at commit `7c8f2bb` — 3-6 fps in-fight, correct
rendering.** To get back to it, drop the two PR #977 commits on top:
`git revert --no-edit f816c6f 04359bd`  (or `git checkout 7c8f2bb -- src/graphics/host_gpu/renderer/cache/`).

`7c8f2bb` is the BufferCache ownership fix: `RunGarbageCollector` gated `aggressive` — download a
dirty buffer (a full GPU drain) or leave it — on `m_total_used_memory`, which was **total device
memory shared with the texture cache**. The texture cache cannot evict GPU-modified tiled images, so
it pins total memory above critical with memory the buffer cache neither owns nor can free; the
buffer GC therefore drained the GPU permanently in response to pressure it could not relieve.
Fix: require this cache to hold a material share (25%, `KYTY_BUFGC_OWN_SHARE=0` disables) of the
budget before going aggressive. Measured **1 → 3-6 fps**, GPU utilisation **33% → 62-78%** and steady
instead of oscillating 3-86%, shared system memory **2,063 → 1,202 MB**, and
`BufferCache::RunGarbageCollector` went from **6 of 6** guest-GPU-thread stack captures to **absent**.

**THEN IT REGRESSED.** Cherry-picking upstream PR #977 (`04359bd` separate cache ownership
accounting, `f816c6f` scan past protected eviction candidates) dropped the fight back to **1-2 fps**,
even though GPU utilisation stayed high (62%). Working hypothesis, **not yet tested**: `f816c6f`
scans past the unevictable tiled images and frees textures that are needed again immediately, so the
GPU looks busy while doing re-upload work instead of rendering. `04359bd` is probably innocent and is
the commit that makes `m_total_used_memory` mean owned bytes again — our fix now depends on it, so
**revert `f816c6f` first and alone** before reverting both.

**~3.3 GB OF VRAM IS DRIVER-MANAGED, INVISIBLE TO VMA, AND STILL UNIDENTIFIED.** `driver usage`
(7,815 MB) minus VMA blocks (4,480 MB) = **~3,335 MB** that no cache owns and no GC can reach. It
pushes the process past `driver budget` (7,291 MB), and **that** is what makes Windows spill to
shared system memory. It **grows with play** — 12 MB at boot → 113 MB loading → 3,284 MB in a fight —
so it accumulates rather than being reserved up front.

**IT IS NOT PIPELINES. That hypothesis was built, tested and refuted.** `PipelineCache` does only
destroy pipelines in `~PipelineCache()`, so they genuinely accumulate (799 and climbing in-fight),
and 3,335 MB / 799 ≈ 4.2 MB each looked like an answer. **It was a division artifact.** Pipeline
eviction was implemented and capped the cache at 500 (293 trims, holding), and the driver-side gap
**did not move**: 3,335 → **3,399 MB** with ~220 fewer pipelines. Driver usage went *up*
(7,649-7,815 → 8,049-8,082 MB) and the shared spill with it (1,202-1,902 → 2,223 MB). **Lesson:
before building a fix on `total / count`, change the count and check the total moves.**

**NEXT CANDIDATES, UNTESTED.** Both fit "driver-managed, invisible to VMA, grows with play, survives
`destroyPipeline`": (a) the **`VkPipelineCache` (`m_driver_cache`)**, which retains compiled binaries
for every pipeline ever created regardless of whether the `VkPipeline` still exists and is only
destroyed at shutdown — measurable with `vkGetPipelineCacheData`, though the 29 MB on-disk file
argues against it; (b) **buffer device address page tables** — every buffer created with
`SHADER_DEVICE_ADDRESS` needs driver-maintained GPU VA mappings across the 1.5 TB space, and there
are thousands of buffers. (b) is the better guess, and it would also explain why coarser
`CACHING_PAGEBITS` made things worse. **Measure live buffer count and the pipeline-cache data size
before building anything.**

Everything else was eliminated by measurement:
- **not duplication** — `dup_factor` **1.01** (3,286 distinct guest addresses for 3,311 images);
- **not the texture cache** — it accounts for **~3.0-3.3 GB** of a 7.5 GB footprint and sits *below*
  its own 5,223 MB critical threshold, which is also why PR #977's harder eviction made things worse:
  it was squeezing the wrong cache;
- **not VMA fragmentation** — blocks 4,225 MB vs allocs 4,212 MB, a **13 MB** gap;
- the driver-side gap **grows with play**: **12 MB at boot → 113 MB loading → 3,284 MB in a fight**,
  so it is accumulation, not a startup reservation.

**FAILED, DO NOT RETRY WITHOUT FIXING BUFFER GRANULARITY FIRST: `CACHING_PAGEBITS` 14 → 16.** The BDA
page table is `(1 TB + 512 GB) / page * 8 B` = **768 MB** of device-local memory at 16 KB pages, 18%
of the whole VMA footprint; 64 KB pages cut it to 192 MB. The 576 MB saving was **entirely eaten by
per-buffer padding** — buffer ranges align to `CACHING_PAGESIZE`. Measured: VMA blocks **unchanged**
(4,281-4,452 → 4,367-4,480 MB), alloc/block fragmentation **13 → 244 MB**, shared spill
**1.2 → 1.9 GB**, over-budget **477 → 524 MB**, fps 3-6 → 3-7 (noise), and it introduced
**multi-second stutters** outside the fight. Reverted; the finding is recorded as a comment on the
constant in `bufferCache.h`.

**Also unexplained and probably worth a look: `trigger=0MB`.** `m_trigger_gc_memory =
max((budget - threshold) / 2, 0)` evaluates to zero, so the texture GC's early-out never fires and it
walks the LRU on every call. Not urgent while the cache is under threshold, but the budget arithmetic
is suspect.

Previously: 2026-10-03 session 16 — **UPSTREAM RENDERS UFC 5 CORRECTLY AT ~1 FPS. THE PLAN INVERTS:
PORT OUR TWO PERF FIXES ONTO `origin/main`, NOT THE OTHER WAY ROUND.**
After a two-week gap `origin/main` is **425 commits** ahead of our branch point (`b847135`,
2026-09-08). A stock upstream build **gets in-game with no graphical issues** and runs **~1 fps** —
slower than our 5 fps but visually correct. That number is exactly our pre-ubershader-fix baseline,
and the mechanism is identified: #883 deleted the strategy ladder and made goto elimination the only
structurizer. `GotoStructurizer` is the guard-variable algorithm (`CaptureVariable`, `RouteVariable`,
`GotoAfter`, `Eliminate`), so the three pixel ubershaders now get the same shape as the old dispatcher
fallback that cost **21,157 us/draw** (census was `645 legacy, 3 dispatch` — those 3 were the
ubershaders). Semantically faithful, hence correct rendering; catastrophically slow.
**`SplitSharedTerminalBlocks` is a CFG pre-pass, not a structurizer strategy** — it makes the graph
reducible before anything consumes it, so it should drop in front of upstream's `Structurize()`
(`ShaderCFG.cpp:1677`) and leave goto elimination nothing to lift. **~91 lines** + 3 trivial helpers
upstream lacks (`AppendClonedSemanticBlock` 16, `ReplaceValue` 13, `ReplaceTerminatorTarget` 12);
drop the `post_dominators.clear()` line (#883 removed that analysis). The BufferCache GC fix is
**still absent upstream** (`bufferCache.cpp:607` still gates `aggressive` on `m_total_used_memory`).
Expected ladder on a fresh branch: **~1 → ~3 → ~5 fps with correct rendering.**
**THE EXPERIMENT IN STEP 1:** privatising shared `Return` blocks is semantics-affecting. Upstream
renders correctly *without* it; we render wrong *with* it. If the black round returns when the
pre-pass is ported, **our own fix was the correctness bug** and sessions 5–15 chased a self-inflicted
wound. That outcome is worth more than any further NaN tracing. See "session 16" below.

Previously: 2026-09-16 session 15 — **WATCHED-IMAGE FIRST-CORRUPTION TRACER (spinner path).**
Generic `WatchImage` tracer landed (no UFC hard-codes). Arms via `KYTY_WATCH_IMAGE_ADDR` /
`KYTY_WATCH_IMAGE_ID`, `D:/PS5/dumps/WATCH_IMAGE`, or inspector **Watch image**. Logs CREATE/
UPLOAD/SAMPLE_BIND/GPU_WRITE/CPU_WRITE/GC_*/VIEW_*/COPY_*/… with content_generation. Break-on-
next-write + GOOD/BAD snapshots write `watch_image_{good,bad}.json` + `watch_image_diff.txt`.
`KYTY_WATCH_UI_PROBE=1` / `run_ufc5.ps1 -UiProbe` lists distinct ≤512² sampled textures to
identify the loading icon. Phase 1 (icon identity) not yet closed — needs a live menu run.
DS_APPEND fix and hang-CS stub-off remain intact.

Previously: 2026-09-15 session 14 — **HANG CS STUB REMOVED; LDS `DS_APPEND` IGNORED `m0=0`.**
Root cause of the TDR was not pair-packed wave64 per se: `AppendConsume` applied a GDS-style
`m0` size window to **LDS** append/consume. UFC does `s_mov m0, 0; ds_append`, so every LDS append
became a no-op, the CS built no work list, and loop 50 walked zeros forever. Fix: LDS uses the DS
offset only and relies on allocated LDS bounds; GDS keeps `(base<<16)|size`. After the fix, full
144×1×1 exec with no GDS/loop caps runs for minutes without `ErrorDeviceLost`.
`run_ufc5.ps1` no longer sets `KYTY_SKIP_CS_HASH` by default (`-StubHangCs` for A/B rollback).
See session 14 notes below.

Previously: 2026-09-15 session 13 — **HANG CS STUB STAYS; SAMPLED/STORAGE SHARE VkImage; WAITCNT WAS A NO-OP.**
CS `0xea0aceac518ec52d` translation is clean (`unsupported=0`, CFG ok, wave=64). Stub reason remains
wave64-on-wave32 TDR (RTX 3070: `subgroup min=max=32`, `wave64=false`). Live identity: storage+sampled
pairs for the two RW surfaces are **same ImageId / same VkImage / different VkImageView / same mip+fmt**.
Texture[2] is depth-tiled (`tile=0x18`) R32F alias of a `D32SfloatS8Uint` depth image — intentional
depth feedback, not a FindImage collapse bug. Generic fix landed: `S_WAITCNT`/`s_barrier` now emit
ImageMemory(+UniformMemory) when the program writes images/buffers (`97250b5`). Checkpoint before stub
removal: `a090a3f` (`KYTY_EXECUTE_CS_HASH` + `CsImageIdentity`/`CsAliasPair`). Real exec with
`KYTY_GDS_LIMIT_CAP=1` still `ErrorDeviceLost` after the first live dispatches — stub **not** removed.
See session 13 notes below.

Previously: 2026-09-15 session 12 — **GUEST MIP9 ON A 256² IMAGE IS A REAL MIP-TAIL SLOT, NOT A DECODE TYPO.**
`MaxMip=9` means highest mip *index* 9 (`levels = MaxMip+1 = 10`). Kyty's Standard64KB tiler packs
mip8 at tail_xy=(0,12) and mip9 at (0,8) inside the same 64KB mip-tail block (footprint stays
`0xb0000`). Vulkan can only host complete levels 0..8, so `Image::` clamps host mips; the old
`FindView` fold mapped both guest slots onto host mip8. That is an unintended alias of distinct
guest subresources. Diagnostic-only: `mipTailDiagnostic.cpp`, no clamp restore. See session 12.

Previously: 2026-09-12 session 11 — **THE NaN COMES FIRST; BLOCK 4 BEING SKIPPED IS ITS EFFECT.**
Block 4 is entered on three `V_CMP_LT_F32` compares, and an ordered compare against NaN is FALSE, so
a NaN colour clears EXEC and skips the block. The real per-pass history is **99.7% NaN** in the three
channels that share the `v1` multiplier and **0% NaN** in the fourth; `v29`/`v30` are already NaN
before that multiplier is applied, and the unguarded reciprocal is innocent on clean data. The two
dispatches are a **chain** — pass 1 reads what pass 0 just wrote, same frame. Read "session 11"
before touching block 4 again; that thread is closed.

Previously: 2026-09-12 session 10 — **THE INSPECTOR CAN TRACE ONE GUEST RESOURCE THROUGH A FRAME.**
Hold a frame, filter `0x1163770000` (or any overlapping range), walk previous-writer / next-reader /
next-writer, inspect inputs vs outputs, and arm a before+after dump of that exact operation. Alias
warnings open an evidence-only inspector. CPU submit RIP is recorded from `AgcDriverSubmit*` return
addresses (one RIP per command-buffer submit, not a per-draw stack walk). This does not fix UFC 5.
Panel-on runs remain **never comparable to the 5 fps baseline.** See "session 10" below.

Previously: 2026-09-12 session 9 — **THE GPU INSPECTOR NOW DUMPS AND TRACES BLACK-RENDER DATA.**
It records every real draw/dispatch in the current frame after resource resolution, numbers repeated
dispatches of one shader, and shows guest addresses, texture-cache image ids, extents, actual Vulkan
view formats and access flags. It is default-off behind `KYTY_DEBUG_PANEL=1`; F10 toggles it. Panel-on
runs add host draws and diagnostic capture overhead and are **never comparable to the 5 fps baseline.**
See "session 9" below.

Previously: 2026-09-12 session 8 — **BLOCK 4 DOES EXECUTE. SESSION 7c IS WITHDRAWN, AND SO IS
EVERY REGISTER VALUE EVER READ BY THE PROBE.** `CS 0xe17349e0d437757b` is dispatched **twice per
frame** with different ping-pong images, and the capture only ever recorded the *first* binding of
the hash — so the probe written by one pass was read back out of the other pass's image. With that
fixed, direct markers show blocks 3, 4 and 5 executing for **100% of waves in both passes across
three frames**. The black round is a **value** bug inside a region that runs, not a skipped region.
See "session 8" below before using any probe number from session 7.

Previously: 2026-09-12 session 7 — **THE BLACK ROUND IS NaN IN THE TEMPORAL UPSCALER'S HISTORY
BUFFER. BOTH IMAGE-ALIAS THEORIES ARE DEAD, BY MEASUREMENT.** The game renders the fight at
1068x600 and temporally upscales to 1600x900. Every *fresh* input to that upscale pass is clean;
its own history buffer is 37-41% NaN and feeds itself. See "session 7" below — read it before
touching the texture cache, which this session rules out as the cause.

Previously: 2026-09-11 session 5 — **THE BLACK ROUND DIRECTLY CONSUMES OUTPUT FROM THE SKIPPED WAVE64 CS.**
In actual-round frame 1073, CS `0xea0aceac518ec52d` declares `0x1163770000` as read-write buffer 9;
immediately afterward PS `0x654607b31fe5f6d6` samples that same address as a 1600x900 HDR image. The
captured input and its downstream output are the same nearly-solid-white image with identical SHA256.
The round therefore needs the real structured CS output; a constant fill is already disproven.
Wave64 support (or a faithful replacement for this shader) is again a correctness requirement. A naive
`lane_count=1` implementation that joins two native subgroups with an LDS atomic-spin rendezvous is now
ruled out: even a one-`V_READLANE` synthetic shader TDRs on the RTX 3070. Preserve the existing two-half
lowering unless the replacement avoids cross-subgroup forward-progress dependencies.

**ACTUAL-ROUND ONE-TILE TIMING (2026-09-12):** `KYTY_GDS_LIMIT_CAP=1` did not previously mean one
workgroup; it still launched all 144. New hash-scoped `KYTY_GDS_GROUP_CAP=1` plus an immediate
`KYTY_GDS_SYNC_CAP=1` finish isolated the real cost. Fifteen walkout/pre-round calls completed in
0.8-1.0 ms (first warm call 3.3 ms), but the first actual-round call took **3426.0 ms** and device-lost
at `debug_op=0` on `0xea0aceac518ec52d`. The user's visible transition and the log agree exactly.
The same shader has a scene-data-dependent slow path; a single real-round work item exceeds TDR.
Dispatch count, GDS chunking, and the 143 losing workgroups are not the root fix.

Session 4's `N == M` conclusion is **withdrawn**: N was logged during the walkout, while M was dumped
in the round after that target retired. The comparison was temporally mismatched and could not decide
round aliasing or show that round draws still targeted `0x1168360000`. See sessions 4/5 below.

Also this session: **IN-FIGHT 1.0 -> 5.0 FPS.** Two independent wins: the 3 pixel
ubershaders now structurize on the Legacy path (1.0 -> 2.9), and `BufferCache` no longer garbage-collects
against memory it does not own (2.7 -> 4.6 median, 5.0-5.4 observed). Details below.

Previously in this session: **1.0 -> 2.9 FPS from the ubershader fix.**
The 3 pixel ubershaders now structurize on the Legacy path (privatise shared `Return` blocks). Per-draw GPU
cost **21,157 us -> 95-262 us (~80-220x)**; graphics GPU **847 -> 96-191 ms/frame**; census **672 legacy,
0 dispatch**; 0 device losses, 0 spirv-val failures. **The GPU is no longer the wall** (187 ms busy in a
366 ms frame). An SRT flat evaluator (`KYTY_SRT_LINEAR=1`) cuts `getprog` **28.0 -> 19.3 us/draw (-31%)**
but does **not** show end-to-end. Transfer queue re-tested: **NEUTRAL**. GC critical-threshold raise:
**a transient, not a fix** — VRAM climbs to whatever the threshold is and pins there. **The wall is now
`drawprep` (~60%), descriptor work in draw recording, and the unbounded growth of GPU-dirty buffers.**
See "2026-09-11 (session 3)".

Previously: 2026-09-11 session 2 — **in-fight wall identified: `IT_DISPATCH_INDIRECT` host arg read, ~45-50% of the frame**; CPU/GPU measured ~50/50 and serialised (per-draw track floors at ~2.1 fps); `vkCmdDispatchIndirect` implemented behind `KYTY_INDIRECT_DISPATCH`, default off, blocked on a depth-alias-sampling bug. See "2026-09-11 (session 2)".

Previously: 2026-09-10 (FPS work: intra-command-buffer EOP-wait skip — menu ~24→~33 fps, in-match cp_rest/finish collapsed but drawprep_ms now the wall; frame instrumentation added; commits 745d4d8 / 8ee5cc8 / aa794cd on fork)

### SESSION 17 (2026-10-03) — THE WALL WAS THE BUFFER GC, FOUND BY MEASUREMENT AFTER TWO WRONG GUESSES

Branch `ufc5-v2` = `origin/main` (`b11aea8`) + our commits. Everything below was measured on this
machine (RTX 3070 8 GB, i7-10700, PCIe 3.0 x16), not inferred.

**WHAT CLOSED, WITH NUMBERS.**
- **The wave64 TDR thread is CLOSED.** On upstream the hang CS `0xea0aceac518ec52d` was dispatched
  **10 times with 0 device losses and 0 shader translation failures**, unstubbed — `KYTY_SKIP_CS_HASH`
  is not read by upstream at all, so `run_ufc5.ps1` setting it is a no-op there. Sessions 5/13/14's
  biggest blocker is gone. The `BranchCondition` ballot finding from session 16 is still real code,
  but it is no longer blocking anything here.
- **Our ubershader privatisation was NOT the correctness bug.** `SplitSharedTerminalBlocks` ported
  onto upstream's `GotoStructurizer` as a pre-pass: built clean, **0 CFG failures**, rendering stayed
  correct — and **fps did not move**. Goto lowering was never the wall. **Reverted.** Sessions 7-11
  were not chasing our own change.
- **`2d1f261` (`KYTY_SRT_LINEAR`) is CLOSED — not worth porting.** A census on the per-draw
  `MaterializeResources` call (`pipelineCache.cpp`, cache-hit path) measured **~5,800 calls/frame,
  8.0 us mean, ~0.09 s per 2 s wall ≈ 4% of frame time**, and only **4.6-13.2%** of calls see
  unchanged SRT inputs, so memoisation cannot help either. Optimising this away entirely buys ~4%.
  Session 3's "does not show end-to-end" was right, for this reason.
- **`a1fc490` (PR #484 scratch reuse) is CLOSED — redundant.** Upstream's `d6dac92` already pools
  evaluation contexts on the `ResourcePlan` (`SrtWalker::AcquireContext`, `generation += 2`,
  stamps at `SrtWalker.cpp:359-377`). Arguably better than the thread-local version we ported.

**HOW THE REAL WALL WAS FOUND.** Three measurements, none of which need a code change:
1. **`nvidia-smi` over an 8 minute in-fight run (240 samples):** GPU utilisation **mean 33.4%**, only
   **1.7%** of samples >=90%, **60.7 W of a 220 W** budget, memory bandwidth 2-6%. The GPU was
   starved, oscillating 3-86%. **Not GPU-bound.**
2. **Process CPU:** **1.10 cores of 16**, 83 threads, hottest thread only 44.9% of one core. Nothing
   saturated — everything waiting. A synchronisation problem, not a throughput one.
3. **`cdb` non-invasive stack sampling** (`-pv -p <pid> -c "~*k8; q"`, 18 sweeps, **1,494
   thread-stacks**): **every one of the 6 captures of the guest GPU thread was inside
   `BufferCache::RunGarbageCollector`**, with `DownloadBufferMemory` on the stack twice.
   `MaterializeResources` and the SRT walker appeared **once each**.

Plus VRAM pinned at **7,404 / 8,192 MiB**, which holds `m_total_used_memory >= m_critical_gc_memory`
permanently true. That is the condition `7c8f2bb` fixes. Session 3 reached the same conclusion
independently on a tree 425 commits older.

**METHOD NOTE — STACK SAMPLING BEAT EVERYTHING ELSE, BUT ONLY WHEN AGGREGATED.** An early read of
**6 samples of one thread** put the hot path in the SRT walker and was **wrong** — those `IR::`
frames (`ValidateRuntimeValue`, `Value::GetType`, `_Hash<_Uset_traits<IR::Inst>>`) are also used by
`ResourceTracking` at translation time. Do not draw conclusions from a handful of samples of a single
thread; sweep all threads repeatedly and aggregate. Also: `std::vector<unsigned>` frames in a Release
build are usually inlining artifacts, not real allocation.

**TOOLING THAT WORKS (use this next time instead of building a profiler).**
- `cdb.exe` at `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe`. Attach **non-invasively**
  with `-pv` so quitting does not kill the game; set `_NT_SYMBOL_PATH` to the bin dir so the
  freshly-built PDB resolves. Two minutes to a real answer.
- `nvidia-smi --query-gpu=utilization.gpu,memory.used,power.draw` and `nvidia-smi dmon -s t` for PCIe
  RX/TX. Windows shared-memory spill: `Get-Counter '\GPU Adapter Memory(*)\Shared Usage'`.
- Tracy **is** instrumented upstream (85 `KYTY_PROFILER_FUNCTION` zones, `--profile` flag) and the CLI
  tools now build at `build-tracy/{capture,csvexport}`. Five failed attempts first: the root Tracy
  CMakeLists does not include the CLI tools, and they need `-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl`
  (clang-cl makes CMake pick `llvm-ar`, which is then handed MSVC-style flags). Not needed in the end.
- **`devenv.ps1` sets `$ErrorActionPreference = 'Stop'` and dot-sourcing leaves it in the shell.** In
  PowerShell 5.1 any native stderr output then becomes a *terminating* error, so cmake aborts at its
  own banner. **Always `$ErrorActionPreference = 'Continue'` after dot-sourcing it.** This single
  issue caused four consecutive build "failures" that were not failures.

**BUILD/RUN NOTES.**
- Configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl
  -DCMAKE_CXX_COMPILER=clang-cl -DKYTY_BUILD_LAUNCHER=OFF`. Cold build ~938 steps, no ccache on this box.
- Upstream switched **SDL2 → SDL3** (`release-3.4.x`) and moved `ffmpeg-core` to KytyPS5's own fork, so
  a branch switch needs `git submodule update --init --recursive`. SDL3 links **static** — no new DLL.
- `_PipelineCache` is keyed by git revision, so it misses after every rebuild; a dirty tree keeps the
  same revision, so **move it aside manually** when the SPIR-V changes or you will reuse stale pipelines.
- Run without the log firehose: `kyty_emulator.exe --game D:\PS5\Games\UFC5`. `printf_direction`
  defaults to Silent. **Logging was tested and is NOT a factor** — still 1 fps with it off, though it
  does write ~1 MB/s (`Equeue wait:` from `eventQueue.cpp:413`) when `--printf-direction File` is set.
  The heavy per-draw graphics dump needs `--graphics-debug-dump` as well, which `run_ufc5.ps1` never passes.
- The old 5 fps fork binary is preserved at `Emulators/KytyPS5-Bin/kyty_emulator.ufc5-fork-5fps.exe`.

**REMAINING GRAPHICAL ISSUES (user-confirmed, not chased this session).** Everything renders and is
"playable", but: the whole scene is **flat / washed out**, consistent with session 13's `8108f2d`
(the 1x1 auto-exposure texture has no producer and reads 1.05e-19 — upstream does not read our
`KYTY_STUB_CLEAR_IMAGES` workaround); and **white UFC logos are speckled with colour**, suspected
session 12 mip-tail aliasing, which our `1e724e0` implements for real but which upstream may already
cover via `8589731`/`041874f`. **Compare before porting.**

### SESSION 16 (2026-10-03) — UPSTREAM CAUGHT UP AND OVERTOOK US ON CORRECTNESS. REBASE DIRECTION INVERTS.

No emulator was run this session. Everything below is read out of `origin/main`, the GitHub API, or
our own tree. **The user reports a stock upstream build gets in-game with no graphical issues at
~1 fps.** That single observation reframes the whole project.

**UPSTREAM STATE.** `origin/main` = `b11aea8` (2026-10-03 19:13, "loader: native trampoline for
VRSQRTPS under `--amd-cpu`", #893). **425 commits** ahead of our merge-base `b847135` (2026-09-08).
20 of those landed on 2026-10-03 alone — upstream is moving fast; re-fetch before trusting any count
here. 355 files changed, +51,697/-23,960.

**WHY UPSTREAM IS AT ~1 FPS — IDENTIFIED, NOT GUESSED.** `0791f92` ("UFC 5 in-game", #883, nmzik,
2026-09-28, 19 commits, 34 files) **deleted the structurizer ladder**: `ShaderCFG.cpp` went
3311 → 1772 lines, and `StructurizeLegacy` / `StructurizerKind::LegacySplit` / the retry ladder are
gone. `Structurize()` (`ShaderCFG.cpp:1677`) now calls `GotoStructurizer(graph).Run()` and nothing
else. That class is the textbook guard-variable goto-elimination algorithm — `CaptureVariable`,
`RouteVariable`, `GotoAfter`, `SetBefore`, `Eliminate`. On a CFG with a shared exit reached from many
predecessors it introduces a route variable per goto and lifts them through enclosing constructs,
turning straight-line code into guarded loops. **That is the same shape as the old dispatcher
fallback, which session 3 measured at 21,157 us/draw.** Session 3's census — `645 legacy, 3 dispatch`
before our fix, `672 legacy, 0 dispatch` after — says only three shaders ever failed to structurize
natively, and those three were the ubershaders. Upstream never had our win; it did not regress.

**OUR FIXES THAT ARE NOW UPSTREAM — DROP ON REBASE.**
- **LDS `DS_APPEND` `m0`** (session 14, committed this session as `ea81081`). Upstream
  `EmitAppendConsume` gates the `(base<<16)|size` window on `mem.kind == IR::ResourceKind::Gds`;
  LDS uses the DS offset only. **Identical fix, independently derived** — ours was never pushed
  (fork last pushed 2026-09-12 01:49, the finding is 2026-09-15) and was still uncommitted until
  today. Upstream's label: "shader: fix LDS counter bounds".
- **`KYTY_INDIRECT_DISPATCH`** (session 2, ~45–50% of frame, parked default-off on a depth-alias bug).
  `79530b4` "Read indirect dispatch counts from GPU buffers" ships it **unconditionally, no env gate**.
  And `da33a1c` "respect physical tile layout when resolving depth aliases" **fixes the blocker that
  kept ours off**. Note session 3 already recorded `op0x16` collapsing on its own, which killed this
  lever's value anyway.
- **Mip tails.** `8589731` "Preserve PS5 mip-tail views and GPU data during image expansion" and
  `041874f` "resolve equivalent terminal mip views without expanding backing". Our `1e724e0`
  logical-tail implementation may be redundant — **compare before porting it.**

**OUR FIXES STILL ABSENT UPSTREAM — KEEP.**
- **BufferCache GC ownership** (`4572ebe`, the 2.7 → 4.6 median step). Verified: upstream
  `bufferCache.cpp:607` still reads `const bool aggressive = m_total_used_memory >= m_critical_gc_memory;`
  — total device memory, shared with the texture cache, exactly the bug. Core of our fix is ~5 lines
  (`&& own_pressure`, where `own_pressure` compares `m_registered_bytes` against a share of critical);
  the rest of that commit's +331/-10 across 4 files is census logging. **`m_registered_bytes` is ours
  — upstream has no equivalent counter, so the port must add it.** PR **#977** adds cache-owned
  accounting from the HFW side; cherry-pick it first and our gate collapses to ~1 line.
- **`SplitSharedTerminalBlocks`** (`0ffcecb`, the 1.0 → 2.9 step). See the plan below.
- **Per-phase frame instrumentation.** Upstream has **none** — grepped `drawprep` / `cp_rest` across
  `origin/main:src/`, zero hits. `gpuTimestamps.cpp` (311 lines) / `.h` (85) don't exist upstream, so
  they port with zero conflict. **Port these first or you cannot measure anything on the new branch.**

**NEW FINDING — THE FOURTH `*Zero` SITE, AND IT IS LIVE IN OUR TREE.** `aad25ff` fixed two EXEC/VCC
read paths and `b238b33` fixed the branch terminator via `Translator::MaskIsZero`
(`Translate.cpp:126`, ORs both mask halves and compares to zero — correct, and immune to ballot
under-population because it reads the *modelled* mask words). **There is a fourth site we missed:**
`BranchCondition()` in `spirvEmitterProgram.cpp:251` begins
`if (ctx.other_half == nullptr || ...) return ctx.Def(info.condition);` — so `MaskIsZero` is used only
when `lane_count == 1`. **When `other_half != nullptr` (wave64 lowered as two wave32 halves — exactly
the RTX 3070 path our hang CS takes) it discards `info.condition` and re-derives the test from
`ctx.Ballot(info.condition)`, then compares `AND(low,high)` against `~0u`.** `info.condition` for
`ExecZero` is already a wave-uniform boolean; balloting a uniform `true` sets bits for *active lanes
only*, so `== ~0u` is false whenever the wave is not fully populated — even when guest EXEC genuinely
is zero. **The `s_cbranch_execz` exit is never taken and the loop does not terminate.** That is a
credible mechanism for `ErrorDeviceLost` on `0xea0aceac518ec52d`. Upstream has the same code, moved to
`EmitConditionRef` (`spirvEmitterFlow.cpp:656`) — which is why grepping our tree for
`EmitConditionRef` finds nothing. Open PR **#985** describes this bug exactly ("a wave64 shader run as
two wave32 halves therefore never sees all-ones") and fixes it by inverting the ballot. **We do not
need #985: `MaskIsZero` is already wave-correct, so the minimal fix is to stop `BranchCondition`
overriding `info.condition` for the `*Zero` kinds.** Smaller and more obviously right than #985, and
#985 is still unmerged — this is the one piece of our work clearly ahead of upstream.
**If upstream does not TDR on the hang CS, this whole thread closes anyway — test that first.**

**OPEN PRs WORTH PULLING, RANKED** (all still open as of 2026-10-03):
| PR | why |
|---|---|
| **#985** | wave-wide `*Zero` branch as "no lane failing". The bug above. Astro Bot world scene, ~50 s/frame. |
| **#986** | loop iteration cap, default 65536, `KYTY_SHADER_LOOP_LIMIT`. Turns a TDR into an instrumentable slow frame. |
| **#988** | recompiler plans + SPIR-V cached on disk. 8.6 s → 0.56 s recompiler time per launch. Pure iteration speed. |
| **#977** | cache accounting + eviction under memory pressure. Upstream's version of `4572ebe`; also fixes protected textures exhausting the eviction candidate budget (cf. our `deleted=0` across 128 TexGc runs). |
| **#935** | mask `BufferLane` by guest wave size instead of hardcoded `63`. One line. |
| **#983** | `FirstLane` via `FindILsb` instead of `OpGroupNonUniformBallotFindLSB`. macOS/MoltenVK only, but touches the emitter path `EmitAppendConsume` uses. |

Also landed upstream and relevant to the `drawprep` wall: `3bd5de6`, `d646fd9`, `684330c`, `37501b5`.
Nothing upstream or in PRs solves wave64-on-wave32 generally.

**REBASE COST.** 88 of our 122 changed files are also in upstream's 355, and the churn is concentrated
exactly where we work: `ShaderRecompilerComputeTests.cpp` 10,877 lines, `shaderCfgTests.cpp` 4,823,
`ShaderCFG.cpp` 1,598, `spirvEmitterMemory.cpp` 1,474, `ResourceTracking.cpp` 1,471, `SrtWalker.cpp`
1,283. This is rewrite-grade, not a replay. **Our 34 diagnostics-only files do not overlap upstream at
all** — `dispatchInspector`, `sceneDrawDebug`, `watchedImageTrace`, `mipTailDiagnostic`, `ProbeConfig`,
`gpuTimestamps`, all of `ufc5/`. The tooling survives intact either way.

**WHY WE PORT ONTO UPSTREAM AND NOT THE REVERSE.** Pulling "the graphics fix" is ill-defined: we do not
know which of 425 commits did it (candidates include #883, `da33a1c`, `73615c3`, `44ba657`, `0f1ff37`,
`8589731`). Even if it is #883, that is 19 commits — individually fetchable via `refs/pull/883/head` —
but **23 of the 34 files it touches are files we have also modified**, on a tree ~300 commits past our
base. Against that, our perf delta is **~100 lines of real logic**. Port the small thing onto the big
thing. No clone needed: `origin/main` is already local, so this is `git switch -c ufc5-v2 origin/main`.

**PLAN, IN ORDER.**
1. `git switch -c ufc5-v2 origin/main`. Confirm it still renders correctly and still runs ~1 fps.
   Also test the hang CS (`KYTY_EXECUTE_CS_HASH`) — if it does not TDR, close the wave64 thread.
2. Port the instrumentation (`gpuTimestamps.*`, per-phase counters from `6842843` / `b8e0bdd`).
   No conflict surface. Without it you cannot tell a real win from a lighter scene.
3. Port `SplitSharedTerminalBlocks` as a pre-pass before `GotoStructurizer`. Expect ~1 → ~3 fps.
   **Watch for the black round.** Binary outcome: still correct → best of both, and worth upstreaming;
   black round returns → our privatisation was the correctness bug all along.
4. Port the GC gate (via #977 if it applies). Expect ~3 → ~5.

**DROP IF UPSTREAM RENDERS CORRECTLY** — the temporal upscaler NaN (sessions 7–11), the wave64
`0xea0aceac518ec52d` TDR, the 1x1 auto-exposure texture at 1.05e-19, the mip-tail alias,
`KYTY_INDIRECT_DISPATCH`, the `DS_APPEND` fix, `KYTY_SRT_LINEAR` (no end-to-end), and the transfer
queue (measured neutral).

**CONVERGENCE / ATTRIBUTION, for the record.** Our PR **#549** was open **53 seconds** (2026-09-09
21:29:08 → 21:30:01), contained 4 commits, none of them CFG or LDS work, and has no maintainer
interaction in its timeline. Our fork is public but last pushed **2026-09-12 01:49**, 0 stars, 1 fork
(`ibrahimrifatovicc-ui`, 2026-09-18). UFC 5 was already a filed upstream target before our fork
existed — issues **#100** (2026-07-22) and **#113** (2026-07-27). The LDS convergence is provably
independent: our finding is 2026-09-15, three days after our last public push, and was uncommitted
until today. nmzik's structurizer answer is also architecturally different and far more expensive than
ours. Six genuine convergences total (LDS, shared-exit CFG, wave-wide EXEC/VCC, BufferCache GC,
indirect dispatch, mip tails). Separately: **PR #470** (brandostrong, "model EXEC and VCC words as
subgroup ballots", cited in `aad25ff`) was **closed unmerged on 2026-09-15**. New UFC 5 report from a
third party on an official build: issue **#905**, `masterSemaphore.cpp:53` after the difficulty
screen, RTX 4090 — i.e. others are now downstream of where we were.

### 2026-09-11 (session 3) — the übershaders structurize; the cause was a SHARED RETURN BLOCK

**FIX (uncommitted): `SplitSharedTerminalBlocks` in `ShaderCFG.cpp`, wired into `StructurizeLegacy`'s
retry ladder after the routing loop.** `legacy=0 -> legacy=1` for all three ubershader fixtures.

**The diagnosis in session 2 was aimed one step short of the cause.** The dumped CFG is:
```
16: cond -> true=192(Return) false=17      192: Return, preds=[16,191]
17: cond -> true=191         false=18      191: -> 192,   preds=[17]
18: -> 19                                  19:  join,     preds=[15,18]
```
i.e. `if (a) return; if (b) return; <join>` where **both early exits land on ONE shared return block**.
Because 192 is reached from 16 *and* from 191, it is dominated by 16, not by 17. `FindSelectionMerge(16)`
then takes the `false_reaches_true` branch at **:1322** (17 reaches 192, 192 does not reach 17) and returns
**192** — stretching 16's construct to the function exit, after which block 18's edge to the shared join 19
is an unstructured exit. **It never reaches the `global_merge == UINT32_MAX` gate at :1325 that the previous
session identified as "the gap".** That gate is real but is not on this path.

**Why privatising the return block is the right fix, not widening a merge gate.** SPIR-V allows leaving a
construct by returning, but *branching to a block that returns* is only legal if that block is inside the
construct. Sharing the return block converts a legal return into an illegal branch-out. Give 191 its own
clone and everything falls out of the existing code with no merge-selection change at all:
`FindSelectionMerge`'s `HasLinearPathToTerminal` rule now picks **17** for block 16 and **18** for block 17,
the tight merges. **This is why it does not disturb phis** — the constraint session 2 identified. Terminal
blocks have no successors, so cloning one adds no phi sites anywhere.
- Implementation: clone with the existing `AppendClonedSemanticBlock` (keeps `inst_begin/end`, so the
  epilogue is re-executed, which is correct — exactly one copy runs per invocation); first predecessor keeps
  the original. Only `Branch`/`ConditionalBranch` preds are retargeted (an `IndirectBranch`/`DispatchSwitch`
  keeps its own target tables and cannot be retargeted by id).
- Cost on these shaders: **+1 block** (193 -> 194). Not a code-size event.

**Verified, in this order:**
- `shader_cfg_tests` green, including new `TestUfc5PsLegacyStructurizedSpirv` which runs **all three**
  fixtures through the legacy strategy and gates on `spirv-val` + phi-parent validity + `OpSwitch == 0`.
  `1bcc68ff` 164,996 words / 1,981 phis, `b808b388` 164,526, `f7030726` 164,488 — all `switches=0 fallback=0`.
- `resource_materialization_tests`, `scalar_provenance_tests`, `resource_tracking_tests`: pass (the latter
  still logs the known pre-existing invalid-mip-range message). `shader_recompiler_compute_tests` fails at
  `renderContext.cpp:49 m_gpu == nullptr` — **confirmed pre-existing by stashing the change and re-running**,
  unrelated to CFG.
- **Menu run with `--shader-validation true`: 153 shaders, `[CFG-STRUCT] strategy=legacy` ×153,
  0 `CFG-STRUCT-FAIL`, 0 spirv-val failures, ~30-32 fps (unchanged).** No repeat of attempt 5's device loss.

**SIZE IS NOT THE METRIC — this corrects session 2.** The ledger attributed the `5,432 GCN dwords -> 166,690
SPIR-V words = 30.7x` expansion to the dispatcher. It is not: the **structured** module is the same size
(164,996 vs 166,203). The 30x is **wave64 lane-pair duplication**, not control flow. The dispatcher's cost is
the 161-case `OpSwitch` in a per-pixel loop where divergent lanes never reconverge — an algorithmic blowup in
wave utilisation, invisible to word count. The new test therefore asserts `OpSwitch == 0`, not a size bound.
A size assertion was written first and **failed**, which is what surfaced this.

**COMPILE TIME (asked directly).** `ShaderCompile: ... ms=` is the recompiler; `GfxPipeline: ms=` is the
driver. Per übershader: **structurize ~83 ms + SPIR-V emit ~50 ms ≈ 135 ms**, so **~400 ms for the three**,
once, at first draw. Driver pipeline creation is negligible (menu max **5.7 ms**) — the second is ours, not
NVIDIA's. **The fix does not add to this: baseline legacy took the same ~83 ms and then FAILED**, after which
DispatcherFull ran on top. Measured both ways by stashing the change.
- **A 4x structurize speedup exists and was deliberately NOT taken.** Moving the terminal-split retry
  *before* the routing loop (which copies and re-structurizes the whole graph once per block, ~190 times)
  gives **83 ms -> 20 ms**. It regresses three tests that assert a terminal epilogue is *not* duplicated when
  routing can repair the shape without duplication (`...AlternatingSharedReturns`,
  `...NestedEarlyExitSharedTerminal`, `...OverlappingEarlyExitLadder`). Duplicating code in shaders that
  already work, to save 250 ms of one-off compile, is the wrong trade. A gate that picks the split early only
  for the shared-terminal failure shape would get both; unproven, and probably does not separate those three.

**HARNESS: `shader_cfg_tests` now reports ALL failures instead of aborting on the first.** `Check` throws
`CheckFailed`; every test in `main` runs through `RUN(...)`, failures are collected and printed as a summary,
exit code 1. This paid for itself immediately — the retry-reorder experiment above showed all three
regressions in one run instead of one per rebuild.

**NEW DIAGNOSTIC: `KYTY_SKIP_PS_HASH=<hex>[,...]` (uncommitted, `renderDraw.cpp`, default off).** Drops the
`vkCmdDraw*` for draws binding those pixel shaders and *keeps every bit of host CPU work* (pipeline,
descriptors, render targets) — so the delta is GPU time only. Purpose: the "3 shaders = 79% of the frame"
claim rests on per-draw timestamp brackets, which is **attribution, not causation** — begin/end timestamps
around a draw on a pipelined GPU can still absorb work from other draws in flight, and this ledger has
already shipped one wrong number from exactly that (the 96% wave64 figure). Not issuing the draws is the
causal control, and it gives the **upper bound on what any fix to these shaders can ever be worth**.
- Decision table for the next fight session: fix-run fps jumps → done. Fix flat + skip jumps → the shaders
  are the cost but the structured version is still slow. **Fix flat + skip flat → the 79% attribution is
  wrong, stop working on these shaders.** The fix-run alone cannot separate the last two.
- **Mechanism verified on menu shaders** (the ubershaders never compile at the menu): skipping
  `0x69dadabfa1cfd7db,0x0e28dfb7b7c0b292,0xba0aad8a10da19a9` took them from 5.1-7.2 us/draw to
  **0.0 us/draw** in `GpuDraws`, `watched=12 draws 0.00ms`, no crash, no device loss. The env parsing and the
  skip path both work, so a fight run cannot be wasted on a typo.
- The frame is **not correct** with this set — draws are missing. Measurement tool only.

**STABILITY: 5m40s menu soak on the fixed build, `--shader-validation true`.** Survived (stopped by the
harness, not a crash); **0 crashes, 0 `ErrorDeviceLost`, 0 Vulkan validation errors, 0 spirv-val failures,
153/153 `strategy=legacy`, 0 `CFG-STRUCT-FAIL`**, fps flat at 34-36 for the whole run. Repeated after the
`KYTY_SKIP_PS_HASH` change with the flag unset: identical. **Caveat: none of the three ubershaders compile at
the menu** (grepped all three hashes, 0 hits) — this proves no regression in the 153 shaders the menu uses
and that the build is stable, NOT that the ubershaders execute correctly on the GPU.

### MEASURED IN-FIGHT: 1.0 -> 2.9 fps. The mechanism was right.

Log `KytyLog-PPSA03541-fight-fix.txt` (fix on, xfer off), real fight, HUD correct, round still black.

| measure | baseline (session 2) | after | change |
|---|---|---|---|
| `ps=0x1bcc68ffb7b0469e` per draw | **21,157 us** | **95-262 us** | **~80-220x** |
| graphics GPU / frame | 847 ms | 137-191 ms | ~5x |
| total GPU busy / frame | 924 ms | 240-325 ms | ~3x |
| **GPU time per draw** | **236 us** | **14-19 us** | **~13-16x** |
| strategy census | 645 legacy, 3 dispatch | **672 legacy, 0 dispatch** | — |
| device losses / spirv-val | — | **0 / 0** | — |
| fps | ~1.0 heavy scenes | **2.9** | — |

`GpuBusy` windows are exactly one frame (`NoteFrame`: Arm -> Recording -> Reading across two boundaries), so
these are directly comparable. **Per-draw GPU time is the honest headline** — the fix run's scene carried
7,434-10,044 draws/frame vs the baseline's ~3,700, so the graphics-time and fps rows *understate* the win.

**THE GPU IS NO LONGER THE WALL.** 187 ms busy in a 366 ms frame (~51%). `process_ms ≈ wall` again, for real
this time. Every remaining lever is CPU-side or shape.

**`op0x16` COLLAPSED ON ITS OWN — and this kills the `vkCmdDispatchIndirect` project.**
`IndirectArgs: total=10.8ms slow=5 slow_total=8.5ms` vs session 2's `total=1666ms slow=17` at ~98 ms each.
Per-call `op0x16` **11-22 ms -> 1.5-1.7 ms**. Session 2 hypothesised the stall was the CPU blocking on a
saturated GPU rather than a PM4 problem; **confirmed**. The remaining `op0x16` cost is dispatch recording,
not the arg read. The depth-alias-sampling blocker no longer needs solving *for this reason* (it is still a
real bug).

### FAILED: `KYTY_XFER_QUEUE=1` re-tested in-fight and is NEUTRAL. Do not retry.

Log `KytyLog-PPSA03541-fight-xfer.txt`. It did everything it claims — `XferReadback on_transfer_queue=256
(100.0%)`, `finish` count 56 -> 15, **`finish_ms` 85 -> 17 ms/frame, `gc_ms` 46 -> 10 ms/frame, ~103 ms/frame
removed**. And the frame did not get faster.

| | finish | gc | draws/frame | ms/frame | **us per draw** |
|---|---|---|---|---|---|
| xfer off | 85 | 46 | 2,478 | 344 | **139** |
| xfer on | 17 | 10 | 2,657 | 366 | **138** |

**COUNTER-READING TRAP (cost this session an hour of wrong emphasis): every `FrameProfile` field accumulates
over the whole window and is reset together; `frames=N` is the window length.** So `draws=7971 frames=3` is
**2,657 draws/frame, not 7,971**. Same for every `_ms` field. Divide by `frames` before quoting anything.
`GpuBusy`/`GpuDraws` are different - those windows are exactly one frame (see `NoteFrame`).

**Identical per draw.** The apparent fps drop (2.9 -> 2.7) is a 7% heavier scene, not a regression. The 103 ms
came back in compute dispatch recording — `Pm4Ops` same window: `op0x15 (IT_DISPATCH_DIRECT)` **0.112 ->
0.146 ms per call (+31%)**, `op0x16` 1.58 -> 1.71, while `op0x27` (draws) was **unchanged per call**
(0.0780 -> 0.0782). This is the ledger's own warning firing verbatim ("with the transfer queue on, that GPU
wait hides inside `op0x16`/`cp_rest`") and the same "time relocated, it did not vanish" that killed
`KYTY_DEFER_READBACK`. **Method note: the 443 -> 35 ms `finish_ms` figure from session 2 was read as a win.
It is not one. Quote fps normalised by draw count, never a bucket ratio.**

### THE WALL NOW: `drawprep` 60%, at ~138 us of host CPU PER DRAW

Per frame at 2.7-2.9 fps (~366 ms, **~2,650 draws**). These buckets nest and double-count — do not sum them.

| bucket | ms/frame | share |
|---|---|---|
| **drawprep** | **220** | **60%** |
| ├ draw recording (`DrawPhase bindings` 27-30 us/draw) | 92 | 25% |
| └ getprog / materialize | 73 | 20% |
| `cp_rest` | 85 | 23% |
| dispatch | 39 | 11% |
| finish (xfer on) | 17 | 5% |
| gc (xfer on) | 10 | 3% |

Sub-counters: `BindPhase findbuffers=10.4-11.0us rebind=5.2-5.5us textures=2.5us`;
`Materialize snapshot=16.7us/call`; `SrtEval sources=9.75us srtreads=4.97us cfg=1.24us`.

### PER-DRAW COST CHAIN, MEASURED THE SAME WAY EACH TIME (in-fight medians, us of wall per draw)

**Method that made these comparable:** fps alone is useless here - scenes vary 2,400-4,100 draws/frame, so
two runs of the "same" fight differ by 70%. Take every `FrameProfile`, divide by `frames`, drop the first
third (warm-up) and frames under 1,500 draws (menu), and quote the **median us/draw**. A single sample said
the GC change was worth 36%; the median said 14%. **Always use the median over the run, never one line.**

| run | getprog | finish | gc | **total us/draw** |
|---|---|---|---|---|
| ubershader CFG fix only | 28.0 | 35.0 | 17.5 | **141.9** |
| + SRT flat evaluator | **19.3** | 31.3 | 18.5 | **140.2** |

**The only solid per-draw win beyond the ubershader fix is `getprog` 28.0 -> 19.3 (-31%), and it did NOT
show end-to-end.** A third row claiming 105.4 us/draw from the GC threshold change was withdrawn - it was
measured before that run reached steady state; see the GC section below. **The 1.0 -> 2.9 fps from the
ubershader fix remains the session's real result.**

### SRT FLAT EVALUATOR (`KYTY_SRT_LINEAR=1`, uncommitted, default OFF) - bucket win real, frame win NOT

Compiles the SRT value DAG once per shader into a topologically ordered op array
(`ResourcePlan::srt_linear`, `LinearCompiler` in `SrtWalker.cpp`); per draw it is a flat loop over a slot
array - no recursion, no `unordered_map` find + `insert_or_assign` per node, no visiting-stack linear scan,
and memoisation falls out free because each `Inst` appears once.
- **Isolated cost: 14.5-15.2us -> 2.9-3.0us per call (~5x).** Coverage 57-78% of calls depending on scene
  (gated to programs with no CFG-conditional sources and no clean/specialization slots).
- **`getprog` 28.0 -> 19.3 us/draw (-31%)**, over 4,000+ samples. The bucket moved exactly as predicted.
- **BUT end-to-end it was ~flat: 141.9 -> 140.2 us/draw.** A projection of "~46ms/frame = 11%" was made from
  the isolated per-call saving and **was wrong** - the frame is not bound by that bucket alone. **Size a
  change by the total us/draw before and after, never by its own bucket.**
- Correctness: `KYTY_SRT_LINEAR=verify` runs both evaluators and compares. **0 mismatches over ~573,000
  calls across 579 shader hashes, in-fight.** All three unit suites green with the path on and off.
- **TRAP THAT PRODUCED A FALSE CLEAN: the verify comparison shared a bug with the code it was checking.**
  `DescriptorSource::dwords` is `std::array<Value, 8>` (image/sampler descriptors are 8 dwords); both the
  flat path and the comparison used `d < 4`, so it validated only the half that was correct and reported
  `mismatched=0`. The truncation surfaced instead as `unsupported storage texture ... dwords=...,00000000,
  00000000,00000000,00000000` at `descriptors.cpp:406`. **A validator that shares an assumption with the
  code under test cannot check that assumption.**
- Second bug, caught by `TestSrtWalkerRealSmemTranslation` before it ever ran in-game: the raw-read address
  aligns `base`, the `memory_info` immediate and the runtime offset **each** down to 4 bytes independently -
  not sum-then-align.
- Wiring gotcha: `ExtractResourcePlan` rebuilds every `Inst` into its own `value_storage`, so a program
  compiled against the source `Program`'s pointers is useless to the extracted copy. It must be compiled on
  the object that is evaluated per draw - hence `BuildSrtLinearProgram()` called at the end of
  `ExtractResourcePlan`. Getting this wrong showed up as `SrtLinear: calls=0`.

### THE GC WAS DOING SPECULATIVE READBACK IT DID NOT NEED TO - the session's clearest win

`RunGarbageCollector` skips dirty buffers entirely unless `aggressive`
(`m_total_used_memory >= m_critical_gc_memory`); in aggressive mode it downloads **every GPU-dirty range of
the whole buffer** - 10 MB in one copy for `0x113d000000`. New `BufferGc/128` counter:
```
BufferGc/128: aggressive=128/128 used=5287MB trigger=2611MB critical=5222MB
```
**Every GC run was aggressive, and `used` was sitting ~1% (30-65 MB) over `critical`, permanently.** A
knife-edge: 65 MB the other way and none of that download traffic happens.
- **RAISING THE THRESHOLD IS A TRANSIENT, NOT A FIX. VRAM DOES NOT PLATEAU - it climbs until it reaches
  whatever `critical` is set to, then aggressive GC pins it there.** Full-run evidence:

  | critical | VRAM first -> last | aggressive windows |
  |---|---|---|
  | 5222 (default) | 4,439 -> 5,760 MB | 183/246 (74%) |
  | **6400** | 4,427 -> **6,496 MB** | **1,928/2,547 (76%)** |
  | 6000 | 4,410 -> 6,237 MB | 290/555 (52%) |

  The fraction of a run spent below the threshold is a function of **run length**, not threshold value -
  a longer run spends more of itself at equilibrium. Steady state always pays.
- **A -25% figure was published here and then withdrawn.** It came from reading `aggressive=0/128` and
  `total=105.4 us/draw` **early in the 6400 run**, while the run was still filling VRAM; the same run's
  full-length median is **127.6 us/draw** with 76% aggressive windows. The early window was not
  representative. This is the exact trap documented two sections above - **take the median over the whole
  run, and check the run actually reached steady state before quoting it.**
- What IS established: `gc` us/draw tracks the aggressive fraction (3.7 at 52% aggressive, 14.0-16.6 at
  74-76%), so the download really is the cost. The lever is just not the threshold.
- **Correctness is unaffected either way:** below the threshold the GC *keeps* dirty buffers resident
  instead of downloading-and-evicting them; genuine guest reads still fault and download on demand through
  `ReadMemoryOnGpu`. The aggressive path downloads buffers the guest may never read, purely to evict them.
### ROOT CAUSE FOUND AND MEASURED: the texture GC can never evict a GPU-modified TILED image

**It is not the buffer cache.** `BufferGc/128` census: the buffer cache holds a **flat ~500-540 MB** with no
trend (`dirty 27/439MB clean 451/97MB`) while total device memory climbs to `critical` and pins there.
`GetDeviceMemoryUsage()` is **total VMA, shared with the texture cache** - so `used` rising was never
evidence that buffers were growing, which this ledger had been assuming.

**`TexGc/128` with skip-reason counters names it exactly:**
```
used=6245MB critical=5222MB | images=4471 deleted=0/128runs
cand=7680 skip: unreg=0 tiled=7680 unpressured=0 dlfail=0
```
**100% of LRU candidates are GPU-modified + safe-to-download + tiled, and tiled images are skipped**
(`textureCache.cpp` `if (safe && owner->info.IsTiled()) continue;` - correct in itself: downloading one
would write detiled data into guest memory). The texture GC therefore **deletes literally zero images**
while sitting 1 GB over its critical threshold holding 4,471 images.

**The chain, end to end:** textures can never be evicted -> total VRAM pins at `critical` ->
`BufferCache::RunGarbageCollector` sees critical pressure permanently -> `aggressive=128/128` -> it
downloads dirty buffers forever, **in response to pressure it does not own and cannot relieve.**

**TWO FIXES TRIED, BOTH NET REGRESSIONS. Flag-gated, default OFF, do not enable without re-measuring.**

| variant | total us/draw | draw | finish | gc | fps |
|---|---|---|---|---|---|
| baseline | **133.7** | 37.9 | 32.9 | 17.5 | 2.65-2.83 |
| `KYTY_TEXGC_AGE_FIX=1` + budget | 153.3 | **60.7** | 29.9 | 21.3 | - |
| `KYTY_TEXGC_BUDGET_FIX=1` alone | **342.6** | 79.8 | 33.8 | 0.6 | 1.72-1.82 |

1. **Age-order inversion** (`aggressive ? 16 : pressured ? 80 : 160`, reversing the original). It *worked*
   as intended - `deleted` 0 -> 200-1,185/128 runs, images 4,500 -> 500-1,200, `used` 6,250 -> ~5,200,
   and **`BufferGc aggressive` fell 128/128 -> 12-30/128 on its own**, confirming the chain above. But
   `draw` went **37.9 -> 60.7 us/draw**: it evicts images last used ~16 ticks ≈ 0.1 frames ago, i.e. the
   current frame, and the game immediately re-uploads them. **The original `160` is deliberate anti-thrash,
   not a bug.** The working set really is ~4,500 images.
2. **Budget accounting alone** - scan past unevictable candidates (`scan_limit = deletions*16`) and charge
   the deletion budget only for an image actually freed. Logically correct, catastrophic in practice:
   `GcSplit texgc` **3.0ms -> 50.8-71.2ms** per 512 GC calls. **The GC runs ~175x per FRAME**, so a
   16x-deeper scan means ~650 candidates x 175 runs = ~114k `SafeToDownload()` calls per frame.

**THE FIX, AND IT IS THE SESSION'S SECOND BIG WIN: gate `BufferCache` on the memory it actually owns.**
`RunGarbageCollector` now requires **both** total pressure **and** this cache holding a material share of
the budget (25%, ~1.3 GB) before going aggressive. It holds ~500-760 MB, so it stays non-aggressive and
stops downloading dirty buffers entirely. `KYTY_BUFGC_OWN_SHARE=0` restores the old behaviour.

**Measured, median over the whole run (n=138), not a single window:**

| | before | after | |
|---|---|---|---|
| **total** | 133.7 | **83.8 us/draw** | **-37%** |
| `finish` | 32.9 | 14.9 | -55% |
| `gc` | 17.5 | 4.4 | -75% |
| `draw` | 37.9 | 22.4 | -41% |
| **fps median** | **2.725** | **4.615** | **+69%** |

User observed **5.0-5.4 fps consistently** in-fight. `aggressive=0/128`, `downloaded=0MB`, 0 device losses.
`draw` falling 41% was not predicted - fewer GPU drains means less stalling inside draw recording too.
- Implementation note: the cache's footprint is a **running counter** maintained in the symmetric
  `ChangeRegister<insert>` hook, NOT a walk of `m_slot_buffers`. The GC runs ~175x/frame; walking it there
  is exactly what made the texture-GC budget fix 20x worse.
- A drift check compares the counter against the full walk each census window. **Test it with a tolerance,
  not equality** - the walk skips `is_deleted` buffers while the counter tracks register/unregister, so
  they legitimately differ by bytes in flight. Exact equality produced 769 false warnings.
- **WATCH: the buffer cache grew ~500 -> ~760 MB and plateaued** (dirty ~658 MB), and total VRAM now
  oscillates **6,463-6,782 MB against a 6,528 MB budget** - i.e. at or slightly over. Not fatal here (no
  device loss, no allocation failure, fps stable) but it is the thing this change could break in a heavier
  scene. If it bites, lower `KYTY_BUFGC_OWN_SHARE` so the cache goes aggressive sooner.

This was rejected earlier in the session as "hacking around the root cause". The root cause is now measured
and it is **not** in the buffer cache - so removing a false signal was the principled fix after all.
- If the texture GC is ever revisited: **the ~175 GC runs per frame is the thing to fix first.** At once
  per frame a deep scan would be affordable.
- Still unexplained and worth a look: why are ~4,500 images resident and all of them GPU-modified+tiled?
- Ruled out by reading the code, not guessing: the demand path clips to a 512 KiB window
  (`RangeSet::ForEachIntersection` does clip), and `AppendSmallDirtyDownloads` caps seeds at 64 KiB and
  extra at 4 MiB. Neither can emit a 10 MB copy. Only the GC path can.

### SETTLED BY MEASUREMENT: GPU CULLING CANNOT REDUCE THE DRAW COUNT. Wave64 Stage 2 is a CORRECTNESS fix only.

The plan "un-stub the occlusion CS -> most draws disappear -> fps" is **dead**. Three independent lines:
1. **190 sampled `BufferDownload` addresses: ZERO fall inside either visibility buffer's address range**
   (`buffer[9]` 0x1163920000+18,874,368; `buffer[10]` 0x1170776e00+37,748,736).
2. **The new `BufferDlCensus/256` bounds it.** 8th-place cutoff is 53.4 MB of readback in a ~13-frame window.
   For `buffer[9]` (18.9 MB/read) to hide below that it could have been read **at most 2x**; `buffer[10]`
   (37.7 MB) **at most 1x**. A guest deciding draws from visibility reads it *every frame* - tens of reads.
   By comparison `0x113d000000` shows **164x**.
3. **99.9% of draws are direct** (below). Even for an indirect draw the host still records it, so the host-side
   count would not drop regardless.

**The guest CPU cannot act on data it never reads.** The visibility output is consumed GPU-side - `0x1163920000`
is bound as `CS texture[3/4/6]`, read-only sampled, 1068x600, i.e. a Hi-Z pyramid for other compute passes.
- This is a **bound, not a proof of zero**. But it is far below what a per-frame dependency requires.
- **Wave64 Stage 2 remains the fix for the BLACK ROUND (issue #0/#1) and should still be done for that.**
  Do not expect fps from it. Budget it as correctness work.
- New counter `BufferDlCensus/256` (`bufferCache.cpp`, ungated): accumulates bytes+count per download address
  over *every* copy and dumps the top 8. **Gotcha: the first version gated on
  `Config::GraphicsDebugDumpEnabled()`, which needs `--graphics-debug-dump` that no other counter requires, so
  it printed nothing.** The pre-existing `BufferDownload #N` line only logs `copies.front()` and samples
  1-in-64, and batches carry 3-16 copies, so it cannot answer "is address X ever read back".
- **NEXT LEAD (chase after the per-draw work): `0x113d000000 = 164x / 1,679,360 KB` in one census window**
  = one 10 MB buffer re-downloaded **~12x per frame, ~120 MB/frame**. The ledger already named this buffer in
  2026-09-10. Re-downloading the same 10 MB a dozen times a frame smells like missing dirty-tracking, and each
  one is a stall. Small investigation, possibly a bug rather than a cost.

**THE DRAWS ARE ~99.9% DIRECT, WHICH UNDERMINES THE "FEWER DRAWS" PLAN.** `Pm4Ops` opcode mix in-fight:
```
op0x27 IT_DRAW_INDEX_2      15,498   (direct indexed)
op0x2d IT_DRAW_INDEX_AUTO      369   (direct)
op0x24 IT_DRAW_INDIRECT         21   <- twenty-one
```
The guest CPU issues essentially every draw itself. So "un-stub the occlusion CS and most draws disappear"
is **an assumption, not a measurement** — it requires the guest's own draw decisions to depend on CS output
it reads back, and the buffer sizes alone do not show that. The ledger's description of `buffer[9]`/
`buffer[10]` as "indirect-draw data" is contradicted by 21 indirect draws per window. **Session 5 later
proved a different dependency:** buffer 9 is sampled directly as the HDR scene input. That justifies
wave64 work for correctness, but still does not support an FPS win from reduced draw count.

**THE REAL NUMBER IS ~138 us OF HOST CPU PER DRAW.** That is the anomaly: shadPS4 runs the same engine family
at 30-58 fps on ONE core with no threaded rasterizer, so its per-draw cost must be single-digit us. Kyty is
~20-50x off, and 60% of it is `drawprep`. **Cost per draw, not count of draws, is where the multiple is.**
The two sub-targets are already measured and already designed:
- `getprog`/materialize 73 ms/frame — the **flat/bytecode linearisation of the SRT value DAG** (designed in
  detail above; NOT the Xbyak JIT, which buys ~4%). Sized at ~44 ms/frame.
- draw recording 92 ms/frame — `DrawPhase bindings` 27-30 us/draw, of which `findbuffers` 10.4 us and
  `rebind` 5.2 us. Descriptor reuse across draws has never been seriously attempted.

## Replicating the 5 fps configuration — START HERE

```
D:\PS5\tools\run_ufc5.bat                 # the 5 fps configuration
D:\PS5\tools\run_ufc5.bat -Baseline       # optional wins OFF, for A/B
D:\PS5\tools\run_ufc5.bat -Validate       # + --shader-validation
D:\PS5\tools\run_ufc5.bat -Verify         # SRT equivalence check (slow, fps meaningless)

python D:\PS5\tools\frame_stats.py D:\PS5\KytyLog-PPSA03541-run.txt
```
`run_ufc5.ps1` is the real script; the `.bat` is a wrapper. It kills any running
emulator first, clears every env var known to be neutral or a regression (so a value left in
your shell cannot silently change the run), and prints what it set.

**Measured 2026-09-11: `fps median 5.00`, `TOTAL 79.0 us/draw` over 137 in-fight samples**
(baseline the same day: `fps median 2.69`, `137.7 us/draw`).

**What is ON, and why:**

| setting | why |
|---|---|
| `KYTY_SKIP_CS_HASH=0xea0aceac518ec52d` | **Required to reach a fight at all.** The CS is wave64 and TDRs on a wave32-only RTX 3070. The black round directly samples its skipped buffer-9 output as the first HDR scene input (session 5). |
| `KYTY_GPU_TIMESTAMPS` unset | Off by default: validation found unreset timestamp queries (UB). `frame_stats.py` uses `FrameProfile` and does not need them. |
| `KYTY_SRT_LINEAR=1` | Flat SRT evaluator. `getprog` -31%; verified equivalent (0 mismatches / 573k calls). |
| *(no env var)* | The two big wins are **in the code, always on**: the ubershader structurizer fix, and BufferCache no longer GC'ing against memory it does not own. |

**What is deliberately OFF — all measured, do not re-enable without re-measuring:**

| setting | result |
|---|---|
| `KYTY_XFER_QUEUE` | **Neutral.** `finish_ms` 85 -> 17 but identical us/draw; time relocates into dispatch recording. |
| `KYTY_GC_CRITICAL_MB` | **Transient.** VRAM climbs to whatever line you set, then pins there. |
| `KYTY_TEXGC_AGE_FIX` | **+14 us/draw** of texture re-upload thrash. The original age of 160 is deliberate. |
| `KYTY_TEXGC_BUDGET_FIX` | **`texgc` 3ms -> 71ms**, fps 2.7 -> 1.8. The GC runs ~175x per frame. |
| `KYTY_BUFGC_OWN_SHARE=0` | Restores the pre-fix BufferCache behaviour (i.e. undoes a 37% win). |
| `KYTY_SKIP_PS_HASH` | Diagnostic only — drops draws, so the frame is incomplete. |

**HOW TO READ A RUN — this is not optional.** `fps` from a single `FrameProfile` line is
worthless: scenes vary 2,400-4,100 draws/frame, so two runs of the "same" fight differ by 70%.
**Use `frame_stats.py`: it normalises by draws, drops the first third as warm-up, and takes the
median.** Two changes were called wins this session and withdrawn — the transfer queue (judged
on a bucket ratio) and the GC threshold (judged on an early window before steady state). Both
were caught by this method. Also: every `FrameProfile` field accumulates over `frames=N`, so
`draws=7971 frames=3` is **2,657 draws/frame**, not 7,971. `GpuBusy`/`GpuDraws` windows *are*
one frame.

**`--printf-direction File` is not optional either** — `printf_direction` defaults to `Silent`
and `graphics_debug_dump_enabled()` keys off it, so without it every counter is silently
discarded and the log looks fine but contains no measurements. The script always passes it.

### SESSION 7 (2026-09-12) — THE BLACK ROUND IS NaN IN THE TEMPORAL UPSCALER HISTORY

**Both alias theories are refuted by direct per-frame measurement. Do not write descriptor-exact
cache resolution for this bug; it would fix nothing.**

Method for both: `ResourceTrace` now logs the resolved cache `image_id` (`descriptors.cpp` for
shader bindings, `renderDraw.cpp` for colour targets). Compare, per frame, the id the producer
wrote against the id the consumer read. Image ids are recreated most frames, so a match is a fresh
agreement each frame, not one sticky coincidence.

```
0x1162c00000  composite CS read id  vs  1600x900 scene target id : IDENTICAL in 240/240 in-round frames
0x1163770000  CS write id           vs  PS read id (1600x900)    : IDENTICAL in 400/400 in-round frames
0x1162c00000  400x225 twin          vs  1600x900 twin            : never share an id (0/239)
```
The twins are real and `FindImageFromRange`'s score does tie — but nothing resolves wrongly.
Session 6's "the fix is descriptor-exact resolution" is withdrawn.

**Why session 6 read it as empty: the dump had the same defect as the old hardcoded address list.**
`DumpUfcSurfaces` called `FindImageFromRange` once and dumped the single winner, which in-round is
the 400x225 twin. The 1600x900 image the composite actually reads had never been dumped. It now
dumps **every** image at an address, extent in the filename (`rt62c00000-1600x900-i2547.bmp`), via
`TextureCache::FindAllImagesAtAddress`. **A diagnostic that resolves an address the same ambiguous
way as the code under study cannot detect the ambiguity.** That is twice this exact trap.

**THE ACTUAL PIPELINE.** The game renders at **1068x600** and temporally upscales to 1600x900
(1.5x). The complete, correct, per-frame-changing fight scene lives at `0x1163770000` as a
**1068x600 R8G8B8A8_SRGB** image — that is the native render, not a side artifact.

```
CS 0xe17349e0d437757b   temporal upscale, ping-pong 0x12c0800000 <-> 0x12c1800000 (R16G16B16A16_SFLOAT)
CS 0x3c6058d66788f544   -> 0x1163770000 @1600x900 B10G11R11      50.6% NaN
PS 0x654607b31fe5f6d6   -> 0x116d300000 @1600x900                50.6% NaN (identical counts: passthrough)
PS 0x811633d84dc57d64   -> 0x1164650000 @1600x900
PS 0x708f302a5ca890f0   -> 0x1162c00000 @1600x900                54-67% NaN in, ~0.9% nonzero out
CS 0x3fcdc0204856be9a   -> 0x111a800000 scanout                  HUD only
```

**THE ORIGIN: the upscaler's own history.** Every fresh input is clean; only the history is NaN:
```
0x11af8b0000  vk=37  R8G8B8A8_UNORM        0.00%
0x1161fb0000  vk=37  R8G8B8A8_UNORM        0.00%
0x115f6d0000  vk=122 B10G11R11_UFLOAT      0.00%
0x12c0000000  vk=122 B10G11R11_UFLOAT      0.00%
0x11ae800000  vk=91  R16G16B16A16_UNORM    0.00%  (integer - cannot be NaN)
0x12c1800000  vk=97  R16G16B16A16_SFLOAT   36.93% -> 40.83%   <- its own history
```
`0x1162c00000`'s 1600x900 image is genuinely black (0.06-0.88% nonzero). The tonemap is not
broken; it is faithfully tonemapping NaN. **The prime suspect is now arithmetic inside
`CS 0xe17349e0d437757b` generating NaN (rcp/sqrt/0*inf, or an uninitialised read from our
recompiler), which then spreads through the sampling kernel into its own history.**

**MY OWN ERROR, RECORDED SO IT IS NOT REDISCOVERED.** I first called `0x11ae800000` the NaN source:
channels 1 and 2 are uniformly `0x7FFF` in all 640,800 pixels. That is only NaN if the buffer is
float16. It is `vk=91` **R16G16B16A16_UNORM**, where `0x7FFF` = 32767/65535 = 0.5 — the correct
"zero motion" value for a biased motion-vector buffer. **Read `actual_vk` from `CaptureBinding`
before decoding any raw dump; never infer a format from bytes-per-pixel.** `.bin` captures are raw
(`DumpGpuImage(..., raw=true)`) and `.bmp` companions are converted and clamped, so an HDR buffer
whose values all exceed 1.0 looks like solid white in the `.bmp` and says nothing about its
contents. `0x116d300000`'s "binary 0/255 histogram" was that artifact, not evidence.

**SETTLED BY EXPERIMENT: `CS 0xe17349e0d437757b` GENERATES THE NaN FROM CLEAN INPUTS.**
A one-shot zeroing of the ping-pong pair (`ClearImagesAtAddress`, file-triggered, below) gives:
```
frame 1047   clear fired on both history buffers
frame 1048   history NaN =  0.00%     clear fully effective
frame 1050   history NaN = 74.34%     fully back within TWO frames
frame 1053   history NaN = 74.74%
```
The user sees exactly one clean frame — "it flashed and the screen went blue then went back to
black". With **every fresh input at 0.00% NaN** and the history verified at 0.00%, the pass still
produces ~74% NaN two frames later. Nothing is propagating in; the arithmetic itself produces NaN.
**This is a shader translation bug in our recompiler, not a cache, alias, or data-flow problem.**
Poisoning-once is refuted: clearing does not fix it even briefly beyond one frame.

Note the ping-pong addresses are **allocated per run** (`0x12c0800000`/`0x12c1800000` one run,
`0x12c4000000`/`0x12c3000000` the next). Do not hardcode them; read them from the pass's
`CaptureBinding` list each run (the read is `image=3`, the write `image=12`).

### SESSION 7b — NaN IS A SYMPTOM. THE CORRUPTION TRACKS THE EXEC-PREDICATED REGION.

**`KYTY_NAN_SANITIZE=1` (new, default OFF, `Memory.cpp` `SanitizeStoredNaN`) replaces float NaN
with 0 in every stored image component. It is a symptom-hider, not a fix**, and it exists to answer
one question. With it on, the round **renders**:
```
                     before sanitize      after sanitize
scanout 0x111a800000   ~9% nonzero (HUD)   53.30% nonzero
scene   0x1162c00000  0.06-0.88%           87.28% nonzero
upscaler history       37-74% NaN          0.00% NaN AND 0.00% Inf
```
The arena, crowd and cage are all visibly there - but posterized to two extremes (black / saturated
cyan). So NaN was never the defect; the arithmetic is wrong and NaN was its loudest product.
Removing NaN does not restore correct values, and no Inf remains either.

**THE SHARPEST CLUE SO FAR - the corruption tracks EXEC predication, channel by channel.**
The fp16x4 history after sanitizing:
```
ch0  min=-3.805e4  max=1.638e4  96.6% zero     GARBAGE
ch1  min=-6.55e4   max=1.638e4  69.8% zero     GARBAGE   (negative colour, 7.42% pinned at f16 max)
ch2  min=-3.782e4  max=6.55e4   69.8% zero     GARBAGE
ch3  min=0.1981    max=5.07     mean=3.67      SANE
```
Tracing the final `IMAGE_STORE v0, v47, s12 (dmask=0xf)` in block 6 back to the last write before
that block:
```
v29 0x15c8  block 5  (inside the V_CMPX-predicated region)  -> ch0   GARBAGE
v36 0x15cc  block 5  (inside the V_CMPX-predicated region)  -> ch1   GARBAGE
v30 0x15d0  block 5  (inside the V_CMPX-predicated region)  -> ch2   GARBAGE
v32 0x0928  block 2  (always executes)                      -> ch3   CORRECT
v33 0x112c  block 2  (always executes)                      -> ch3   CORRECT
```
**Every corrupt channel comes from the predicated region; the one correct channel does not.** The
region is entered by `0x1138 V_CMPX_LT_F32 exec_lo, 0, v87` + `S_CBRANCH_EXECZ`, narrowed at
`0x1484 S_AND_SAVEEXEC_B32 vcc_hi, vcc_lo`, and restored at `0x15b0 S_MOV_B32 exec_lo, vcc_hi`.
`vcc_hi` is not rewritten between the save and the restore, so the guest idiom is sound.

**AUDITED AND CORRECT - do not re-read these.** Eight candidates, all faithful to the ISA:
`V_MAD_MIX` source selection (`ReadMixF32`), MAD_MIX inline constants (`Translate.cpp:488`),
MAD_MIX destination half-preservation (`sdwa_dst_unused=2`), `V_CNDMASK_B32` polarity
(`Select(cond, src1, src0)`), `S_SAVEEXEC` save-before-update ordering (`Control.cpp:25`),
`WriteCompareResult` ANDing V_CMPX with incoming EXEC, `WriteRawU32` to `ExecLo` rebuilding the
lane predicate via `ThreadBit`, and the structurizer (clean acyclic CFG, 7 blocks, no loops, no
failure; zero `OpUndef`, no private variables, so no uninitialised reads).

**THE UNGUARDED RECIPROCAL IS INNOCENT (probe measurement, 2026-09-12).** `KYTY_PROBE_VGPR`
(below) read the live registers at the store:
```
v33  min=0.47217  max=1.249  mean=0.745  ==0: 0.00%  NaN: 0.00%
v87  ==0 on 70.42% of pixels, never negative
v33+v87 == 0 : 0.00%      v33+v87 < 1e-3 : 0.00%
```
`v5 = v33 + v87 >= 0.472`, so `0x1630 V_RCP_F32 v2, v5 <= 2.12`. It cannot produce Inf, so the
`Inf * 0 = NaN` route through `0x164c V_MUL_F32 v2, v2, v33` is **dead**. This was the leading
hypothesis. The bound rests only on v33's own minimum and v87 being non-negative, so it holds
regardless of the probe-scale doubt below.

**PROBE CAVEATS - the tool is not yet trustworthy beyond v33.** Do not build on these readings:
- `v87` measured 0..4 in one run and 0..1.53e-5 in the next, same register and shader. One is wrong.
- `v29`/`v30` came back near-constant across the whole image, impossible for per-pixel data that
  measures +/-65504 in the same buffer.
Two known causes, both fixable: **the shader has two `IMAGE_STORE`s** (`0x16c4` dmask=0x7 and
`0x16fc` dmask=0xf) and the probe hijacks *both*, so the captured buffer may not be the one being
read; and **block 6 rewrites v29/v30 at `0x166c`/`0x1674` before the store**, so probing them there
reads post-accumulate values, not the block-5 values. Fix: let the probe select a store index, and
probe only registers block 6 does not overwrite.

### SESSION 12 (2026-09-15) — MIP-TAIL DIAGNOSTICS FOR GUEST MIP9 ON 256² (`0x1237200000`)

**No rendering fix.** Diagnostic imageView (no FindView fold) remains active. Added
`mipTailDiagnostic.{h,cpp}`: dumps Kyty `TileGetTiledTextureLayout` when guest levels exceed the
Vulkan complete chain; `GUEST_LOGICAL_GT_HOST_PHYSICAL` before FindView EXIT; per-prepare
multi-mip correlation (`SAME_PREPARE_MULTI_MIP`).

**MaxMip semantics (from Kyty code, not assumption):**
`ShaderTextureResource::MaxMip()` is `(fields[5]>>4)&0xF`. ResolveTexture does
`levels = max_mip + 1`. So `max=9` ⇒ **highest mip index = 9**, **level count = 10**.

**Evidence from `KytyLog-PPSA03541-imageview-diag.txt`:** after CS `0x056cd586eca8fb63`, storage
views of `0x1237200000` resolve as base/last = 0,1,2,…,8,9 with `max=9`, then host clamp
`levels=10 complete=9`, then EXIT on view `mip=9+1` against `image_levels=9`.

**Guest layout (Kyty tiler, format 69=`k16_16_16_16UInt`, tile 9=`kStandard64KB`):**
`first_tail_level=2`, total `0xb0000` (matches ImageMipTrace). mip8 and mip9 are both 1×1 in the
shared 64KB mip-tail block at **distinct** `tail_xy` `(0,12)` vs `(0,8)`. Footprint is identical
for levels 8..11 once the tail exists — mip9 costs no extra guest bytes.

**Host clamp location:** `Image::Image` ctor (`image.cpp`) sets
`backing.mip_levels = bit_width(max_dim)` when guest asks for more. Required by Vulkan; not a
descriptor decode bug. The *old* FindView fold onto `mip_levels-1` was the compatibility workaround
that collapses distinct tail slots.

**Would mapping mip9→mip8 alias?** Yes at the subresource level: two distinct guest mip-tail slots
become one VkImage mip. Whether Frostbite READ8/WRITE9 in one dispatch still needs
`SAME_PREPARE_MULTI_MIP` from the new binary at that load point (both traced views are
`storage=1` / written).

**Outcome:** **C** (extra logical mip is a mip-tail subresource with distinct guest storage) with
**A** (Kyty lacks a host representation for that slot). Not B/D.

### SESSION 11 (2026-09-12) — THE NaN COMES FIRST AND BLOCK 4 SKIPPING IS ITS **EFFECT**. THE TWO PASSES ARE CHAINED.

**Five probe configurations in ONE navigation, re-aimed live through `D:/PS5/dumps/PROBE`
(generations 1-4, no restart).** That is the tooling from session 8 paying for itself; every number
below would previously have cost a separate fight.

**1. THE TWO DISPATCHES ARE A CHAIN, NOT A PAIR.** Straight from `CaptureBinding` with
`KYTY_CAPTURE_OCCURRENCES=2`:
```
pass=0  reads 0x12a1800000   writes 0x12a2800000  (image_id 3064.2)
pass=1  reads 0x12a2800000   writes 0x12a1800000  (image_id 2630.3)
```
**Pass 1 reads exactly what pass 0 just wrote, same frame, same image id.** The history is advanced
twice per frame. Any measurement that does not say which pass it came from is averaging two
different stages of one pipeline — which is what every session-7 number did.

**2. THE REAL HISTORY, UNPROBED, PER PASS. Session 7's "37-41% NaN" is superseded.**
```
            NaN        the non-NaN remainder
ch0      99.70%        exactly 0        (1 distinct value)
ch1      99.70%        exactly -65504   (1 distinct value)
ch2      99.70%        exactly +65504   (1 distinct value)
ch3       0.00%        3067 distinct, 0.502..4.0, mean 3.33   HEALTHY
```
Both passes agree (99.70% / 99.79%). **The surviving non-NaN pixels carry no per-pixel information
at all** — one saturated constant per channel. This was measured with no probe running, so it is the
shader's own output.

**3. WHY THOSE THREE CHANNELS AND NOT THE FOURTH — and it is not what the ledger assumed.**
```
0x16ec V_MUL_F32 v1, v0, v1     <- shared multiplier
0x16f0 V_MUL_F32 v0, v1, v36    -> ch0
0x16f4 V_MUL_F32 v2, v1, v30    -> ch2
0x16f8 V_MUL_F32 v1, v1, v29    -> ch1
       (ch3 = v3, which never touches v1)
```
The three corrupt channels are exactly the three multiplied by `v1`. **But the multiplier is a
victim, not the cause:** with the history left authentic, `v29` and `v30` are **already NaN at
100%** by `0x16b8`, and `v5 = 1 - max3(v30,v36,v29)` is NaN as a consequence.

**4. THE UNGUARDED RECIPROCAL IS INNOCENT — re-confirmed on clean per-pass data.** The old proof
rested on a pass-mixed reading and had to be redone:
```
v5 (rcp input, = v33+v87)  179-185 distinct   0.156 .. 12     0% NaN, never 0
v2 (rcp product)           345-355 distinct   0.013 .. 1.0    0% NaN
```
`0x1630 V_RCP_F32 v2, v5` cannot be producing this. Nor can the `0x166c` MAC — `v2` is clean and
`v29` is already NaN going in.

**5. `v4` IS ZERO EVERYWHERE, AND THAT IS FINE.** `0x15fc IMAGE_LOAD v4` (texel 0,0, dmask=0x1)
reads **0 on 100% of pixels** in both passes. The guest guards it — `0x16dc V_CMP_EQ_F32 vcc, 0, v4`
+ `0x16e0 V_CNDMASK_B32 v1, v4, 1.0` substitutes 1.0 — so a zero there is expected, not a defect.

**6. THE HEADLINE: BLOCK 4 IS SKIPPED *BECAUSE* OF THE NaN, NOT THE OTHER WAY ROUND.**
Block 4 is entered on `V_CMP_LT_F32 vcc, 0, v13` / `v15` / `v2` OR'd together. **All three compares
are FALSE when their operand is NaN** — that is IEEE ordered-compare semantics, faithfully
implemented — so `vcc` is zero, `S_AND_SAVEEXEC` clears EXEC, and `S_CBRANCH_EXECZ` skips block 4.
Measured both ways within this session: the block-4 entry marker read **1.0 on 100%** of pixels in
one frame and **0 on 100%** in another, tracking how much of the frame is NaN.
**This closes the thread sessions 7c and 8 were both pulling on.** Block-4 execution is an effect.
Session 7c's uniform registers, session 8's markers, and the whole
`S_AND_SAVEEXEC -> EXEC -> structurizer` lead were all downstream of a NaN that arrives earlier.
The loop is self-sustaining: NaN history -> compares false -> block 4 skipped -> `v29/v30/v31` keep
their NaN history values -> stored -> NaN history.

**7. METHOD — PROBING THE RGBA16F STORE POISONS THE THING IT MEASURES.** The probe at `0x16fc`
writes into the ping-pong the shader samples as its own history, so the probe's output becomes its
input one frame later, and `v5`/`v29` read back as the probe's own NaN. **Probe `0x16c4` instead**
(the `B10G11R11` store): the RGBA16F history stays authentic. Cost: that format is **unsigned with
~6-bit mantissa**, so negatives clamp to 0 and the sign is lost — a reading of exactly 0 there means
"zero, negative, or never written", which is why a `mark` in the same block is mandatory as a
control. Taps must also be at a pc **before** the hijacked store, or they have not run when it
executes.

**8. THE GUEST'S DIVIDE-BY-ZERO GUARD IS NOT LEAKING — HYPOTHESIS TESTED AND DEAD.** Block 4
deliberately computes an unguarded reciprocal and masks the result:
`0x1500 V_RCP_F32 v13, v12.abs` -> `0x1554 V_MAD_F32 v9, v9.abs, v13, v10` (Inf, or NaN when
`v9 == 0`) -> `0x1520 V_CMP_NEQ_F32 s3, 0, v12` + `0x156c V_CNDMASK_B32 v9, v10, v9, s3` discards it.
A leak there would fit every symptom, including why the culprit hides (block 4 stops running once
NaN exists). **It does not leak.** Probing `v9` immediately before (`0x156c`) and after (`0x1580`)
the select, on every frame where the block-4 entry marker fired:
```
v9 before cndmask   0.496094    0% NaN   0% Inf
v9 after  cndmask   0.492188    0% NaN   0% Inf
```
Consistent across both passes and five sampled frames plus four post-clear frames. **Wherever block 4
executes, the guarded region is finite.**

**9. TWO QUANTITATIVE SURPRISES FROM THAT RUN.**
- **In steady state block 4 runs on 0.02-0.07% of pixels**, not zero and not many — consistent with
  the NaN lock, and it means a marker reading of "0%" and "0.02%" are different states worth
  distinguishing.
- **Clearing the history does NOT restore a healthy frame.** One frame after clearing both halves,
  block 4 ran on only **5.57%** of pixels. A zeroed history is a *degenerate* state, not a clean one:
  with `v29/v30/v31 = 0` the block-3 differences that feed the entry compares are mostly zero too, so
  the compares stay false. **Session 7's clear experiment therefore never demonstrated "one clean
  frame" in the sense it was read as.** Design any future clear experiment around this.
- Caution on the readout format: through `0x16c4` these values came back as **2 distinct values per
  channel**. `B10G11R11` has a ~6-bit mantissa and crushes real data. It is fine for boolean
  questions (is it NaN / Inf / zero) and useless for value bisection. Use the f16 store for values,
  accepting the history poisoning, or ask only boolean questions here.

**NEXT.** The origin question is now narrow: with the history authentic, what injects the first NaN?
Redo session 7's `CLEAR_IMAGES` experiment **per pass** (it reported one clean frame then NaN back
within two, from mixed passes). With block 4 skipped under NaN, the fresh-colour path never runs, so
look at what block 2 puts in `v29/v30/v31` from the history sample and at the first frame after a
clear. Probe through `0x16c4` so the loop stays real.

### SESSION 12 (2026-09-13) — THE DECODE IS CONFIRMED BY A SECOND TRANSLATOR, AND THE THIRD DEAD CHANNEL IS **v36**

No navigation spent. Everything here is offline, from a cross-check against **SharpEmu**
(`D:\PS5\src\sharpemu`, an independent C# PS5 emulator with its own gfx10 decoder). The point of
keeping both emulators is that they are not behind in the same places — where one is blind the other
usually is not.

**1. KYTY'S DECODE OF `CS 0xe17349e0d437757b` IS CORRECT. This retires the standing caveat that the
RDNA2 block structure was one person's reading.**

`SharpEmu.Tools.ShaderDump --inspect <shader.bin>` runs raw Gen5 words through SharpEmu's real
decode pipeline. It stopped at `unknown-vop1 op=0x54`; SharpEmu's VOP1 table has **no f16 family at
all**. Our shader's only VOP1 f16 op is `V_RCP_F16`, so one line unblocked the whole thing
(`Gen5ShaderTranslator.cs:950`, `0x54 => "VRcpF16"`, **uncommitted local edit**, no open upstream PR
covers it — #784 explicitly does not add individual opcodes).

Against `shader-dumps/hang_cs/CS_e17349e0d437757b.rdna2`:
```
instruction boundaries   969 vs 969, ZERO disagreements
opcode names             858/969 exact; all 111 mismatches explained
```
The boundary result is the important one: a decode-length bug garbles everything after it, and there
is none. The 111 name mismatches are `V_MAD_MIXLO/HI_F16`->`VFmaMixlo/hiF16` (94, gfx9 vs gfx10
naming for the same op), `V_ADD_NC_U32`->`VAddI32` (6, naming era), `V_FMA_F32`->`VFmaMixF32` (3, see
below) and `V_MIN3/MAX3_F16`->`Vop3Raw351/354` (8, SharpEmu gaps — Kyty is ahead).

**2. THE THIRD DEAD CHANNEL IS `v36`. The ledger has only ever tracked v29/v30.**
```
0x16F0  v0 = v1 * v36
0x16F4  v2 = v1 * v30
0x16F8  v1 = v1 * v29
0x16FC  IMAGE_STORE dmask=0xf, data=v0..v3
```
`v3` is never touched at the store — **that is why channel 3 is the clean one**. The three dead
channels are v36, v30, v29. **v36 has never been measured.** Measure it next to v29/v30: if v36 is
not NaN where they are, the "three channels, one cause" story is wrong.

All three converge on `V_MAC_F32 <reg>, X, v2` at `0x166c`/`0x1670`/`0x1674`, and before that on
adds involving `v31` at `0x15c8`/`0x15cc`/`0x15d0`.

**3. NEGATIVE RESULT — the shared multiplier `v2` is not it.** The three MACs above share `v2`, which
would explain "three dead, one clean" from a single register. `v2` traces to `0x1630 V_RCP_F32 v2,
v5` -> `0x164c V_MUL_F32 v2, v2, v33` — **already on the do-not-redo list**, measured at 0.013..1.0,
0% NaN. Hypothesis dead before it cost anything. The list worked exactly as intended.

**4. WITHDRAWN, same session: "Kyty's dump hides the mix ops."** It prints `.opsel(...)` on the three
VOP3P mix-F32 at `0x7ec`/`0x7fc`/`0x804`, which distinguishes them from the genuine VOP3 `V_FMA_F32`
at `0xa98`. The dump is not misleading here.

**Reusable:** the `--inspect` path now works on any `.bin` in `shader-dumps/` at near-zero cost. Full
disassembly kept at `scratchpad/sharpemu_disasm.txt`.

**5. THE SURVIVING PIXELS ARE AN EDGE BAND, AND THE EDGE MOVES. THE NaN SPREADS SPATIALLY.**

Pure offline analysis of `input-...-i3-*.bin` already on disk (binding i3, `actual_vk=97` =
`R16G16B16A16_SFLOAT`, 1600x900, verified from `CaptureBinding`, not assumed). No navigation spent.

MEASURED — interior survivors (outer 1px border excluded), non-NaN in ch0:
```
frame  pass  alive    x percentiles 1/25/50/99      where
 3907   p0    2020    1 / 1 / 2 / 864               LEFT edge
 3907   p1    3300    1 / 1 / 2 / 875               LEFT edge
 5507   p0   11690    1 / 4 / 7 / 1598              LEFT edge
 5528   p1     812    1597 / 1598 / 1598 / 1598     RIGHT edge
 5534   p0    6116    1375 / 1593 / 1595 / 1598     RIGHT edge
 5534   p1    4753    1410 / 1594 / 1596 / 1598     RIGHT edge
```
In f5534 p1 the right band is near-solid at x=1596..1598 (~898/900 per column) and decays inward
(1595:623, 1594:572, 1593:290, 1592:173, 1591:95, 1590:17) with **zero interior survivors anywhere
left of x=1300**. The survivors are never interior — always a band hugging one edge.

INTERPRETATION (not yet measured): these are the pixels whose history reprojection lands outside the
history image, so the history sample is rejected, block 4's fresh-colour path runs, and the pixel
comes out clean. Which edge survives should then depend on camera motion direction that frame —
which matches the left/right flip above.

**WHY THIS MATTERS MORE THAN THE GEOMETRY.** If NaN re-enters through the history sample, it spreads
spatially frame over frame, and the only pixels that recover are the disocclusion band at the
leading edge. **So the injection event only ever had to corrupt ONE pixel, ONCE.** Every hunt so far
has looked for a mechanism that could NaN 99.5% of pixels, and has correctly rejected candidates for
being too rare. That filter was wrong. A mechanism that fires on one pixel in one frame is now
sufficient and should be treated as a live candidate.

Corroborating: **f55 (menu-era) is 0.0000% NaN in all four channels, both passes.** The history does
start clean and turn, so a transition exists.

**Within-frame erosion:** f5534 p0 history 6116 interior survivors -> p1 history 4753. Overlap 4520,
1596 lost, 233 gained. Net erosion in one pass step, with a small gain where reprojection newly went
out of bounds.

**ANOMALIES, PROVENANCE UNKNOWN — do not read into these before checking which experiment produced
them** (several of these captures came from sessions that were firing `CLEAR_IMAGES`):
- `f4075` both passes: ch0-2 **0%** NaN but ch3 **100%** NaN — the exact inverse channel pattern.
  Possibly a different resource at that address; get `CaptureBinding` for that frame before using it.
- `f4232/4319/4446/4563`: ch0 exactly **100.0000%** NaN, zero survivors, while ch3 stays healthy.
- `f5341` and `f5361`: both passes **0%** NaN — fully clean mid-series.

Tooling: `scratchpad/nanmap.py`, `nanmap2.py`, `nanmap3.py`, `series.py`. **Decode half data with
`.view(np.float16)`, never `.astype`** — astype converts the integer value and silently produces a
clean-looking, entirely wrong result (cost one iteration here).

**6. RETRACTED, SAME SESSION: "the NaN spreads from a seed." IT DOES NOT. ch0/ch1/ch2 ARE DEGENERATE
IN EVERY CAPTURE EVER TAKEN, INCLUDING THE MENU.**

Point 5 read the non-NaN edge band as healthy pixels that had escaped the corruption. **They are not
healthy — they are saturated.** Checking content instead of just NaN-ness:
```
f5341 p0  ("0.0000% NaN", read as CLEAN)
  ch0  100.00% zeros        distinct 1   value 0
  ch1    0.00% zeros        distinct 1   value -65504
  ch2    0.00% zeros        distinct 1   value +65504
  ch3                       distinct 997  0.27759 .. 1.2461     HEALTHY
```
Those are **the same three constants** as the "non-NaN remainder" in the corrupt frames. Every
capture behaves this way. `f55` (menu) p0 is all-zeros in all four channels — an unwritten buffer —
and p1 is ch0=0, ch1=-7.6294e-06, ch2=1.5259e-05 (one distinct value each), ch3 966 distinct and
healthy. Same degeneracy, smaller magnitudes.

**In no capture, at any frame, has ch0, ch1 or ch2 ever carried per-pixel information. Only ch3
ever has.**

CONSEQUENCES, and they are large:
- **There is no transition to find.** The history was never clean, so "capture across the walk-in and
  find the frame where it turns" cannot work. The walk-in capture plan built this session
  (`tools/capture_walkin.ps1`) is **not** the priority any more.
- **"The injection only had to corrupt one pixel once" is withdrawn.** Nothing is spreading. The
  three channels are wrong for every pixel from the first dispatch.
- **The whole framing of the NaN as a temporal accumulation failure is suspect.** NaN vs saturated
  looks like two failure states of the same broken computation, not cause and effect.
- **This should reproduce in a SINGLE dispatch with synthetic clean inputs**, which makes the offline
  replay through `tests/ShaderRecompilerComputeTests.cpp` (raw RDNA2 code + real GPU readback) the
  decisive next experiment. **It needs no navigation at all.**

What the constants say, given the store is `v0=v1*v36`, `v1=v1*v29`, `v2=v1*v30`, `v3` untouched:
ch0 pinned to 0 wants `v36 == 0`; ch1 to -65504 and ch2 to +65504 want `v1*v29` and `v1*v30` to
saturate opposite ways. That is consistent with the old session-7c observation of uniform
`v11=0, v12=v13=0.49976, v17=0, v31=0` across 100% of pixels — which was withdrawn as a claim about
block 4 executing, but the **uniformity** in it was never the part that was wrong.

The survivor geometry in point 5 is a real measurement and stands; only the interpretation hung on it
is withdrawn.

**7. POINT 6 WAS ALSO TOO AGGRESSIVE. THERE *IS* A TRANSITION, AND IT IS THE FIGHT'S RENDER-TARGET
REALLOCATION. THE UPSCALER CANNOT COLD-START FROM A ZEROED HISTORY. REPLICATED IN TWO RUNS.**

Point 6 concluded "the history was never clean" from the archived captures. **Every one of those
captures was taken after the reallocation**, i.e. already in the fight — which is why they all looked
degenerate. Two fresh instrumented runs (2026-09-13, `dumps/run1-walkout-*`, `dumps/run2-walkout-*`,
150 history captures) show the actual sequence:
```
run 1                                      run 2
f1260 @11a4800000  ch0 15834 distinct      f862 @11a5800000  ch0 23310 distinct
f1266 @11a4800000  ch0 12598 distinct      f876 @11a5800000  ch0  1329 distinct
                   ALL FOUR HEALTHY                          ALL FOUR HEALTHY   [walkout]
        ---------------- fight begins, render targets reallocated ----------------
f1339 @12c2000000  all four ALL ZERO       f944 @12a2000000  all four ALL ZERO   [fresh alloc]
f1340 @12c3000000  0 / -65504 / +65504     f945 @12a4800000  all four ALL ZERO
f1343 @12c3000000  ch0-2 100% NaN          f948 @12a4800000  ch0-2 100% NaN      [dead]
```
Healthy -> zero-filled on reallocation -> dead within 3-4 frames. **Twice, at four different
addresses, in runs whose walkouts looked completely different on screen.** Deterministic, not seeded.

**THE UPSCALER WORKS.** During the walkout all four channels carry 12,000-27,000 distinct values.
This is not a broken shader. It is a shader that cannot bootstrap.

This also explains the user-visible variability: the walkout runs on the **old, pre-fight** history,
so how good it looks depends on what that history holds. Run 1's red/white washout was very likely
photographed at or just after the reallocation, not mid-walkout, which is why its appearance
disagreed with its (healthy) capture numbers.

Menu-era captures on the pre-fight allocation are degenerate (ch0=0, ch1=-2^-17, ch2=+2^-16) simply
because the menu does not drive the upscaler with a real scene; the same allocation goes healthy the
moment the walkout starts.

**NEXT, AND IT NEEDS NO NAVIGATION:** feed the shader an all-zero history through
`tests/ShaderRecompilerComputeTests.cpp` (raw RDNA2 code + real GPU readback) and see whether one
dispatch produces NaN. That either confirms the cold-start failure outright or kills it, and it
becomes a permanent regression test either way.

**HYPOTHESIS, DECLARED AND NOT ADOPTED.** `run_ufc5.ps1` skips CS `0xea0aceac518ec52d` (wave64,
TDRs on the RTX 3070) and its own comment says "Skipping it is why the fight round renders BLACK".
The unification would be: the skipped occlusion CS is what should write into the freshly-allocated
targets; without it they stay zero; the upscaler cannot bootstrap from zero; NaN; black round. That
would make one cause of all of it. **Session 5 proposed a version of this and it was withdrawn** -
treat it as a hypothesis and test it, do not adopt it because it is tidy.

**NEGATIVE RESULT — the on-screen green/red speckle is NOT the history.** The user's screenshots show
a fine dotted lattice in hard-edged rectangular regions that persists into the match. Tested the NaN
mask for lattice structure: exactly 50/50 on `x&1`, `y&1`, `(x+y)&1` and 25/25/25/25 on `x&3`, `y&3`
— perfectly uniform, no checkerboard. The speckle comes from a different buffer. Note the ledger
already records "walkout-floor speckle" (see the mip-clamp entry) and the rainbow skin (issue 4b) as
separate symptoms with a suspected shared root in the stubbed occlusion CS. **Three distinct
symptoms — do not conflate them.**

Tooling added: `tools/capture_walkin.ps1` (fires DUMP_INPUTS on a cadence so a walk-in needs no
keyboard; writes the occurrence count into the trigger file, which removes the
`KYTY_CAPTURE_OCCURRENCES` footgun entirely), `tools/history_content.py`, `tools/nan_seed.py`.

**8. THE ANSWER. THE UPSCALER IS NOT BROKEN - ITS FRESH SCENE INPUTS ARE EMPTY IN THE FIGHT. THE NaN
HAS BEEN A DOWNSTREAM SYMPTOM FOR TWELVE SESSIONS.**

Three seeding experiments, each killing a hypothesis (`KYTY_SEED_NEW_IMAGES`, added this session):
```
history contents at the fight's first dispatch      pass 0's output
  all zero (the natural case)                       0 / -65504 / +65504 -> NaN
  uniform 0.5 (constant seed)                       0 / -65504 / +65504 -> NaN
  varied noise, 2400+ distinct/channel              0 / -65504 / +65504 -> NaN
```
**The history contents are irrelevant.** "Zero is the poison" and "uniform is the poison" are both
dead. Seeding is confirmed reaching the image (captures show exactly the seeded values in `i3`
immediately before the dispatch that saturates).

The real cause, straight from the `VideoOut dump` `nonzero=` counters, percent of non-zero texels:
```
frame        i4%      i7%      i9%     i10%
85-1691        0        0      100        0     entire menu
1694        42.6     69.2      100     68.6     WALKOUT - inputs light up
1696-1707   ~100     ~100      100      100     WALKOUT - fully populated
1775+          0        0      100        0     FIGHT - empty again
```
`i4`/`i7` (1600x900 `B10G11R11` scene colour) and `i10` (1068x600 `R8G8B8A8`) are **empty during the
round**. They are populated in exactly one window - the walkout - which is the one window the
picture looks right. **The upscaler has nothing to upscale**, and faithfully produces saturation and
then NaN from an empty scene.

**EXACT CONSISTENCY CHECK: `i9` is the only input that is 100% populated at every frame, and `ch3` is
the only output channel healthy at every frame.** The one live input feeds the one live output. That
relationship has held in every measurement all session and now has an explanation.

**CONSEQUENCE.** Every shader-level hypothesis this project has chased - `V_RCP_F32`, MAD_MIX
lowering, block 4's compares, EXEC/`S_AND_SAVEEXEC`, the structurizer, EXECZ/VCCZ, the f16 path -
was investigating a shader doing the correct thing with empty data. The decode cross-check in point
1 said the shader translates correctly; it does, and it was never the problem.

**This substantially rehabilitates SESSION 5's withdrawn hypothesis** ("the black round directly
consumes output from the skipped wave64 CS"), and matches what `run_ufc5.ps1` has asserted in a
comment all along: *"KYTY_SKIP_CS_HASH ... Skipping it is why the fight round renders BLACK - that is
expected, not a regression."*

**NOT YET PROVEN:** that the skipped occlusion CS `0xea0aceac518ec52d` is the reason the inputs are
empty. We have measured that the inputs ARE empty, not why. That is the leading candidate and now
has real evidence behind it, but it is the next thing to test, not a conclusion.

**THE QUESTION IS NOW:** why does the scene stop rendering into `i4`/`i7`/`i10` when the round
starts? Use previous-writer tracing on those addresses in a round (the inspector already does this),
and check whether the skipped CS is a producer for them. **Stop looking inside
`CS 0xe17349e0d437757b`.**

Caveat on the seeding tool: it must be kept narrow. The first noise run seeded every `RGBA16F`
surface including the bloom/downsample pyramid - **4,206 recreations of a 400x225 target in one
walkout** - which blew the frame to white and destroyed the visual readout.
`KYTY_SEED_NEW_IMAGES_MIN_WIDTH` (default 1280) now keeps it to full-resolution surfaces.

### SESSION 15 (2026-09-13) — THE 1x1 AUTO-EXPOSURE TEXTURE IS UNINITIALISED VRAM (1.05e-19),
SCALING EVERY COLOUR BY ~1e-16. FIXED IN THE TEXTURE CACHE; THE VISIBLE EFFECT IS NOT YET MEASURED.

**The whole render is multiplied by 1.29e-16 before the tonemap. That is the "colours wrong, hue
wrong, black round" defect.** Measured end to end in one run, with one build of the emulator and no
fight navigation needed for the decisive part.

**1. THE CHAIN, ALL MEASURED.**
```
1x1 R32_SFLOAT auto-exposure texture @0x1162e90000   reads 1.048e-19   <- NOBODY WRITES IT
  x  rcp(cb[104]) = rcp(0.000813802) = 1228.8         (correct, verified)
  =  net scale 1.29e-16 applied to the scene colour in CS 0x0c399ab0b1e7fa33
  -> tonemapped RGB ~1e-19..1e-12, i.e. 0 in any 8/16-bit UNORM store
  -> the temporal upscaler saturates to (0, -65504, +65504) and then goes 100% NaN
  -> NaN is self-sustaining through the history feedback; only disocclusion pixels escape
  -> black round, with a surviving band at the frame edge (the cage outline the user always saw)
```

**2. THE BISECTION THAT FOUND IT — four taps, one dispatch, no navigation.**
`CS 0x0c399ab0b1e7fa33` is dispatched **twice per frame** in menus, pre-fight and round alike. Its
`IMAGE_STORE` at `0x234` targets the `R16G16B16A16_UNORM` image that the upscaler re-reads as its
`i9` (same `image_id`, verified per frame), so `PROBE pc=234 vgpr=...` gives a free 16-bit readout
of any four registers. Taps fire **BEFORE** the instruction at their pc, so each pc is the *next*
instruction after the one that produced the value.
```
tap  pc   register                                healthy     MEASURED
a8   v5 = rcp(1 + max(exposed rgb))               ~0.051      1.0      <- max is zero
84   v3 = exposure AFTER the ==0 -> 1.0 guard     1.0         0        <- THE BREAK
64   v0 = i0.r * rcp(cb[104])                     saturated   saturated, 365 distinct  OK
38   v0 = raw i0.r straight out of IMAGE_LOAD     ~0.015      0.024, 705 distinct      OK
```
**The image read and the constant-buffer multiply both work.** Everything dies on the exposure.

**3. THE GUARD IS NOT BROKEN — THE TEXEL IS NOT ZERO. AND HOW TO MEASURE 1e-19 THROUGH A UNORM
STORE.** The obvious reading of "v3 = 0 after a `V_CNDMASK` whose whole job is `0 -> 1.0`" is that
the cndmask is mistranslated. It is not. A UNORM8/16 store **cannot tell 0 from 1e-19**, so that
reading was unfalsifiable as posed.

The shader carries its own logarithm: `0x1f8 V_LOG_F32 v7, |v5|` -> `0x220 v7 *= 1/6` ->
`0x230 V_EXP_F32 v3, v7`, i.e. channel 3 of the `0x234` store is `|v5|^(1/6)`. Forcing `v5` to the
register under test turns that channel into a **log-domain readout with ~30 decades of range**:
```
tap=7c:3:92     # v92 = the raw exposure texel, before the cndmask
tap=1f8:92:5    # v5 = v92, just before the V_LOG_F32
vgpr=88,89,90,3 # channel 3 = the shader's own exp2 result = |texel|^(1/6)
```
Read back **raw 45/65535**, so `|texel| = (45/65535)^6 = 1.048e-19` (bracket 9.6e-20..1.14e-19).
**Bit-identical in the menu, the pre-fight and the round** — a dead constant, not an adaptation that
has drifted. Exactly 0 would have read back as 0, so the `V_CNDMASK` at `0x7c` is correct and the
ISA guard simply does not apply.
**Reusable: when a value is outside a store's range, look for a `V_LOG`/`V_EXP` pair already in the
shader and redirect its input. It costs one tap and no rebuild.**

**4. THE TEXTURE HAS NO PRODUCER. `TRACE_ADDRS` on `0x1162e90000`, one frame, deduplicated:**
```
READ  by stage=4 hash=0x0c399ab0b1e7fa33  image=1   1x1 vk=100 (R32_SFLOAT)
READ  by stage=4 hash=0xe17349e0d437757b  image=6   1x1 vk=100   (the upscaler)
READ  by stage=4 hash=0x3c6058d66788f544  image=0   1x1 vk=100
WRITE by                     -- nothing, anywhere in the log --
```
Its guest memory reads `0x00000000` (dumped directly with the new `CS texture[n] guest texel` line),
and the dispatch inspector has always called it `coherency=uninitialized`. **1.048e-19 is
uninitialised VRAM.**

**THE LEAD FOR THE FIX, NOT YET PROVEN:** the same guest address is written as a **BUFFER**:
```
WRITE stage=4 hash=0x1f4dc8c408985d4f buffer=6 addr=0x1162e90000 stride=4 records=33644
READ  stage=4 hash=0x9cbc1ec72e110491 buffer=2 addr=0x1162e90000 stride=4 records=33644
READ  stage=4 hash=0xe327a0730ba4f226 buffer=1 addr=0x1162e90000 stride=4 records=33644
```
i.e. the exposure may be produced into a structured buffer and consumed as a 1x1 image. BufferCache
and TextureCache are separate, so the image would never receive it. **Next experiment, and it is
cheap:** `KYTY_CAPTURE_INPUT_BUFFERS=1` with `CAPTURE_HASHES=9cbc1ec72e110491` dumps that buffer's
real GPU contents. If dword 0 is a sane exposure the fix is a buffer->image sync; if it is garbage
too, then `CS 0x1f4dc8c408985d4f` is the broken producer. **Do not assume the first branch** — the
address is heavily aliased (the same trace shows 54x30 and 54x600 `R16G16B16A16_SFLOAT` colour
targets there), so a 33,644-dword arena sharing a base address with a 1x1 texture may be an accident
of the allocator rather than a producer/consumer pair.

**5. CONFIRMED ON SCREEN, TWICE. `mark=` IS AN ACTUATOR, NOT JUST A PROBE.** `mark=<pc>:<dst>`
writes **1.0f** into `v<dst>` — exactly the fallback the exposure guard was meant to apply — so it
overrides a live register with no rebuild and no restart. And a probe with no `vgpr=` hijacks **no**
store (`ProbeStoredVgpr` returns early when `vgpr[0] < 0`), so the frame is otherwise untouched.
- `mark=84:3` (exposure -> 1.0, keeping the x1228.8): the cage band at the frame edge, which the
  user reports has **always** been visible, changed from **blue to pure white**. A x1229
  over-exposure is exactly what should blow the survivors out. Interior still black.
- `mark=60:4` + `mark=84:3` (net scale 1.0, plain Reinhard on the raw HDR): **the tonemapper's own
  output becomes genuinely healthy.**
```
upscaler i9 (= this shader's 0x234 store)      BEFORE              AFTER
  ch0                                          0,     1 distinct   0.0062, 27982 distinct
  ch1                                          32767, 1 distinct   0.5009, 22407 distinct
  ch2                                          32767, 1 distinct   0.4999, 13647 distinct
  ch3                                          ~0.001              0.62,   48778 distinct
```
The "three dead channels, one live channel" signature is now explained: `ch1`/`ch2` are
`0.5 + 0.5(R-B)` and `0.5 - 0.25R + 0.5G - 0.25B`, which are **0.5 for any input scaled to zero**,
and `ch3` passes through a 6th root, the only channel with the dynamic range to show 1e-16.
**The identical signature in the upscaler history (`0 / -65504 / +65504`, ch3 alone healthy) has the
same cause and is not a separate mystery.**

**6. WITHDRAWN — SESSION 12 POINT 8, "THE UPSCALER IS NOT BROKEN, ITS FRESH SCENE INPUTS ARE EMPTY."
HALF RIGHT. THE INPUTS WERE EMPTY *BECAUSE OF THIS BUG*, AND THE UPSCALER IS **ALSO** BROKEN.**
With the exposure neutralised and the history cleared, the upscaler is fed the measurably healthy
`i9` above and still produces:
```
history p0  ch0/1/2  0% NaN but only 3 DISTINCT VALUES: -65504, 0, +65504   ch3 1061 distinct, fine
history p1  ch0/1/2  98.81% NaN                                             ch3 1962 distinct, fine
its own B10G11R11 scene output i4/i7   the single constant 3.9375 across all 1600x900 texels
```
**That is a real, independent defect in `CS 0xe17349e0d437757b`, now reproducible with a KNOWN-GOOD
INPUT for the first time in nine sessions.** Every previous attempt on that shader was debugging it
against ~1e-16 garbage. Captures of the good-input case are on disk as `dumps/input-*-f2501.bin`.
Also settled: the round's scene input `i0 @0x1176ae0000` is **99.3% nonzero with real HDR content**
in the round — the scene never stopped rendering; only everything downstream of the exposure did.

**7. NEGATIVE RESULT — CLEARING THE HISTORY IS NOT A FIX.** `CLEAR_IMAGES` on all four ping-pong
buffers (`12c1000000,12c2000000,12c0800000,11be000000`; note it matches an image by **exact base
address**, not by range — a 32 MB span cleared only the one image at its base) gives **one white
frame and then black again**. The NaN regenerates within a frame or two even with a healthy input.
So the upscaler is a NaN *generator*, not merely a NaN *accumulator* — a different question from the
one sessions 7-12 asked.

**8. THE SAME HASH HAS TWO PROGRAM VARIANTS. This has silently broken binding-index readings.**
`0x0c399ab0b1e7fa33` appears as **5 images (`sampled=2`)** early in boot and as **6 images
(`sampled=3`)** later, from different `shader=0x...` code addresses. In the 6-image variant the
read-modify-written surface occupies **two** binding slots (`image=2` read-only, `image=3`
write-only, same `image_id`); in the 5-image variant it is one read-write slot at `image=2`. **A
binding index is only meaningful together with the variant it came from.** The 6-image map:
```
i0  1068x600 vk=122 B10G11R11    read    the HDR scene colour        (SRT offset 0)
i1  1x1      vk=100 R32_SFLOAT   read    THE AUTO-EXPOSURE TEXTURE   (SRT offset 32)
i2  1068x600 vk=37  RGBA8        read  \ the RMW lightness history   (SRT offset 128)
i3  1068x600 vk=37  RGBA8        write /   store at 0x228
i4  1068x600 vk=91  RGBA16_UNORM write  YCoCg + L*^(1/6) = upscaler i9 (SRT offset 96, store 0x234)
i5  1068x600 vk=98  R32_UINT     write  the EXEC-gated d16 zero store  (SRT offset 64, store 0x25c)
```

**9. WHAT THE SHADER ACTUALLY IS, from the ISA (it was only ever guessed at before).** Reinhard
tonemap + CIE L* + YCoCg, ~118 instructions:
```
rgb = load(i0) * rcp(cb[104]);  exposure = load(i1)[0,0];  if (exposure == 0) exposure = 1.0
rgb = exposure * max(0, rgb);   rgb = rgb / (1 + max3(0, rgb))        Reinhard
Y   = 0.2126 R + 0.7152 G + 0.0722 B
L*  = Y <= 0.008856 ? 903.2963*Y : 116*Y^(1/3) - 16                   the LOG/EXP pair, EXEC-gated
store i4 = (0.25R+0.5G+0.25B, 0.5+0.5(R-B), 0.5-0.25R+0.5G-0.25B, (L*/100)^(1/6))
store i3 = (old_i2.g, old_i2.b, L*/100, flicker)   a 3-frame rolling lightness history
store i5 = 0                                       gated on (x < cb[0] && y < cb[4])
```
`cb[0..12]` are literally `1068, 600, 1600, 900` as integers, which is what the `V_CMPX_GT_I32` pair
at `0x240`/`0x244` bounds-tests against — **independent confirmation that the scalar buffer resolves
to the right guest bytes**, immediate offsets included.

**10. THE EXEC-GATED THIRD STORE IS A RED HERRING.** It stores a literal **zero**, gated on the
in-bounds test above. It was flagged as "the same shape that silently skipped block 4 in the
upscaler"; it is a clear-to-zero and cannot carry colour. Likewise `cb[28] = 0 or 1`, so
`S_CMP_GT_U32 vcc_lo, 3` is false and block 3 never runs — which is why `i3`'s channel 3 is 0. Both
are correct behaviour. Stop looking at them.

**11. NEW TOOLING, COMMITTED.** `KYTY_DUMP_BUFFER_DWORDS=<n>` (default 0, capped at 64 dwords and 4
dispatches) prints, for the shaders named by `KYTY_DUMP_SHADER_HASH`, the first `n` dwords of every
bound scalar/constant buffer **and the single texel of every 1x1 image**, as hex AND float, read
straight out of guest memory via `TryReadBacking`. Labels are **byte** offsets, so `[104]` is read
directly off the disassembly's `S_BUFFER_LOAD_DWORD ... offset=104`. This produced
`cb[104] = 0x3a555555` in one run at the menu.
- **Trap, and it cost a rebuild:** the first version scoped itself with `watch_cs`, which is also
  true for `kUfcHangCsHash` and for every skipped shader. Those ate the whole dispatch budget before
  the shader under study reached its first dispatch. It is now scoped to the hash actually **named**
  in `KYTY_DUMP_SHADER_HASH`.
- Caveat the line carries itself: for a GPU-produced surface the guest dword is stale. Cross-check
  the inspector's `coherency` field before trusting it. Here it was right because nothing on the GPU
  ever wrote the exposure image.

**12. THE BUFFER LEAD IS DEAD, AND THE REAL MECHANISM IS TRANSIENT-SLOT ALIASING.**
Point 4 proposed that the exposure was produced into the structured buffer written at the same
address by `CS 0x1f4dc8c408985d4f`. **Killed by measurement** (`KYTY_CAPTURE_INPUT_BUFFERS=1`): in a
fresh run neither `0x1f4dc8c408985d4f` nor `0x9cbc1ec72e110491` has a buffer at that address at all.
Both are `groups=743x1x1 local=64x1x1`, **no textures**, with 141k/1.1M-record tables and a
bit-pattern LUT (`0x08102040, 0x40810204, ...`) — skinning or particles. The shared base address was
an allocator coincidence, exactly as the caveat warned.

**What `0x1162e90000` really is: a recycled transient-allocator slot.** The inspector says so
directly — `ALIAS addr=0x0000001162e90000 image_ids=116.5,4849.1,4851.1` — and in ONE frame it hosts
four unrelated resources:
```
tick 423761  CS 0c399ab0b1e7fa33 / e17349e0d437757b / 3c6058d66788f544  READ  1x1   R32_SFLOAT
tick 423763  PS f1ff054b0973bcad  DRAW indices=3   WRITE 54x600 RGBA16F   ) a luminance
tick 423764  PS ceea5f712ba42fda  DRAW indices=3   READ  54x600           ) reduction
tick 423764  PS 116e6210be83a632  DRAW indices=3   WRITE 54x30  RGBA16F   ) chain, all
tick 423765  PS 654607b31fe5f6d6  DRAW indices=3   READ  54x30            ) fullscreen tris
             CS 1f4dc8c408985d4f                   WRITE 134,576-byte buffer
```
**The three consumers read the 1x1 BEFORE the reduction chain writes the slot**, and the reduction
targets hold healthy data (2,982-7,597 distinct, 0% NaN) whose first bytes reinterpret to
2.7e-15 / 1.2e-14 / 2.0e-36 — **none of them 1.048e-19**. So the 1x1 is not a reinterpreted view of
them either. It is simply an image nothing ever writes.

**Also identified, and it is NOT the missing producer:** `CS 0x1a19faa210398edd` is the real
auto-exposure/eye-adaptation pass — it reads the 1068x600 HDR scene, read-modify-writes a **1x1
R32G32_SFLOAT at `0x12a2810000`**, atomically accumulates into a **1x1 R32_UINT** and writes the
534x300 luminance downsample. It runs, and it writes a different address and a different format from
what the tonemapper reads. **Nothing in the frame writes a 1x1 `R32_SFLOAT` anywhere.**

**13. THE FIX — Kyty left newly created images with UNDEFINED contents. Now zeroed.**
```
TextureCache::InitializeImage:  uploads only if IsBufferModified() || IsCpuDirty()
TextureCache::RefreshImage:     returns EARLY when the image is not dirty
```
so an image that nothing has ever written never reaches an initialisation path at all, and keeps
whatever its `VkImage` allocation happened to contain. **A shader reading such an image reads
undefined VRAM** — nondeterministic, and here catastrophic.
- `TextureCache::ZeroNewImages(CommandBuffer&)` sweeps images created since the last sweep and
  zero-clears the ones that are **still** uninitialised, so a target created and immediately
  rendered into is untouched. Modelled on the existing `SeedNewImages` path (`m_pending_zero_init`
  filled in `InsertImage`, swept once per frame from `DumpUfcSurfaces`), because that is the only
  place with both a new-image list and a valid command buffer.
- Guarded by `Image::IsUninitialized()` = `!gpu_modified && !buffer_modified && !cpu_dirty &&
  !zero_initialized` — **exactly the inspector's own `coherency=uninitialized` classification**.
  One-shot via the new `m_zero_initialized`.
- Size-capped by `KYTY_CLEAR_UNINIT_MAX_BYTES`, default **64 KiB**, `0` disables and restores the
  old undefined-contents behaviour. A large render target is about to be fully written anyway, so
  clearing it is pure cost; this is for the lookup/1x1 class whose contents are read as DATA.
- **Why zero:** deterministic, what PS5 memory reads before anything writes it, and precisely what
  the game's own `if (exposure == 0) exposure = 1.0` guard is written against.

**FIRST ATTEMPT FAILED AND THE FAILURE IS THE USEFUL PART.** The hook was first placed in
`InitializeImage`, which looks like the right function and never fires: `RefreshImage` returns
before calling it whenever the image is clean, which is exactly the case under study. **Zero log
lines, zero effect.** If a change to the texture cache appears to do nothing, check that the
function you edited is on the path for a *clean* image before assuming the logic is wrong.

**VERIFIED IN-RUN, at the menu, with the probe still armed:**
```
tap                                    BEFORE           AFTER
|texel|^(1/6)  (log-domain readout)    45/65535         0        -> the texel is EXACTLY 0
v3 after the ==0 -> 1.0 guard          0                65535    -> THE GUARD NOW FIRES, v3 = 1.0
raw texel through a UNORM store        0                0        (says nothing either way)
```
Sweep volume is 1-4 images per frame, so the cost is negligible by inspection; confirm with
`frame_stats.py` against the 5.00 baseline before treating that as settled.

**STILL OPEN AFTER THE FIX, and it is the next thing:** 1.0 is the game's *fallback*, not
necessarily its intended exposure — with `rcp(cb[104]) = x1228.8` and a scene median of ~0.008, a
1.0 exposure puts the median pixel near 0.9 after the Reinhard, which would be washed out. Whether
that is right depends on whether the scene is genuinely stored pre-divided by 1228.8. **It cannot be
judged by eye until the upscaler defect (point 6) is fixed**, because the upscaler still destroys the
frame regardless. Do not tune the exposure against a broken upscaler.

**14. WHAT THE FIX DOES ON SCREEN IS NOT YET ESTABLISHED. A CLAIM WAS MADE AND WITHDRAWN THE SAME
HOUR.**

**Written and immediately retracted: "the pre-fight now renders where it was black before."** That
was wrong, and the user corrected it directly: *"I told you the pre-fight scenes always render, it's
just the colours that are wrong. It's only the fight that has the black issue."* This matches the
original brief exactly — *"the walkout isn't actually correct, it's just less corrupted, colours are
wrong, hue is wrong, sometimes it's bright red and white."* The post-fix screenshots show a legible
but red/white-washed pre-fight, which is **the symptom that was already being reported**, not a new
improvement. **The error was reading a screenshot as a before/after without a before.**

**AND IT OPENS A REAL PROBLEM WITH THE CAUSAL STORY.** The bisection at frames 1767 and 1869 was
taken **in the pre-fight**, and it measured `v3` (exposure after the guard) = 0 and
`rcp(1+max)` = 1.0 there — i.e. the tonemapper's colour output was already dead in the pre-fight.
Yet the pre-fight was rendering a visible scene at that moment. **Those two facts cannot both be
explained by "everything visible comes through `CS 0x0c399ab0b1e7fa33`".**
Possibilities, none tested:
- the pre-fight's visible image reaches the screen by a path that does not go through this
  tonemapper (a different variant, a different present path, or a UI/composite layer);
- or it does, and 1e-16 through this shader is not actually what the eye is seeing.
**Resolve this before claiming any visual effect for the exposure fix.** The correct experiment is
an A/B on one scene: same build, `KYTY_CLEAR_UNINIT_MAX_BYTES=0` versus default, screenshot each.

**WHAT IS MEASURED AND STANDS, independent of any visual claim:**
```
                                            BEFORE          AFTER
exposure texel (via the 6th-root readout)   1.048e-19       exactly 0
v3 after the game's ==0 -> 1.0 guard        0               1.0  (the guard now fires)
tonemapper output, per channel              1 distinct      27982 / 22407 / 13647 / 48778 distinct
```
Those are readbacks, not interpretations. The shader's arithmetic is demonstrably repaired. Whether
that repair is visible yet is a separate question, and the answer is "unknown", because the round is
still gated behind the upscaler defect (point 6) and the pre-fight comparison was never run.

**A remaining prediction, still untested:** 1.0 is the game's *fallback*, not its real exposure, and
`rcp(cb[104]) = x1228.8` is applied on top. Scene median ~0.008 x 1228.8 x 1.0 ~ 9.8, which Reinhard
maps to ~0.91 — white. So if the visible frame does come through this shader, the fix should make it
**more** blown out, not less, and the intended exposure is nearer 0.01. That is a falsifiable
prediction for the A/B above.

**Do not conflate the other symptoms in those screenshots.** The rainbow noise on the name plates,
the dithered band across the lower half, and the menu corruption are pre-existing and separate
(issue 4b and the speckle entry); the ledger has said since session 12 that these are **three
distinct symptoms**.

**15. THE FIX REGRESSED PERFORMANCE BY 29%, THE MECHANISM WAS ALREADY IN THIS LEDGER, AND IT IS
FIXED. A/B ON THE SAME BUILD, SAME SESSION:**
```
                              samples   fps    us/draw   p25/p75        gc
without the zero-init sweep      392    4.92     82.7    80.6 / 85.5    2.7
with it                           76    3.45    106.5    92.0 / 136.4   6.8   <- gc 2.5x
```
`ClearImage` ends in `CommitGpuWrite`, which calls `MarkGpuModified()` — and this ledger already
root-caused that **the texture GC can never evict a GPU-modified TILED image**. Every image the
sweep zeroed became a permanent resident. The `gc` bucket, not the total, is what identified it:
2.7 -> 6.8 us/draw is the signature of the mechanism, and it is what makes this an explanation
rather than a number.
**Fix: `ClearGpuModified()` immediately after the clear.** Zeroing creates nothing the guest needs
read back, so ownership goes straight back to guest memory; `m_zero_initialized` is what stops the
sweep repeating, not the GPU-modified flag.

**RE-MEASURED, three-way, all in-fight on the same build family:**
```
                              samples   fps    us/draw   p25/p75         gc
no zero-init (control)           392    4.92     82.7    80.6 /  85.5    2.7
zero-init, no GC fix              88    3.75     99.7    90.0 / 120.3    6.4
zero-init + ClearGpuModified      64    4.70     80.5    77.7 /  92.1    4.2
```
**The regression is gone — 80.5 vs 82.7 is within noise.** Stated with its limits: 64 samples
against the control's 392, a wider p25/p75 band, and `gc` still somewhat elevated (4.2 vs 2.7). Good
enough to keep the fix on by default; re-measure with a longer in-fight run before treating 80.5 as
a number to quote.
- **Method note: the free A/B.** The comparison did not cost a navigation. An earlier run *in the
  same session on the same build* had already reached the fight, so `frame_stats.py` over both logs
  was the control. Before asking for a run, check whether one already on disk answers the question.
- **And the trap the ledger warns about, hit live:** the first reading of this was `samples 6,
  fps 2.16, TOTAL 93.6, p25 50.6 p75 169.3` at 5,994 draws/frame — a transition scene, not steady
  state. It drifted to 106.5 as samples accumulated. **A wide p25/p75 means the run has not
  settled; do not quote it.**

**16. THE ZERO SWEEP WIPED REAL TEXTURES. THE USER CAUGHT IT FROM A COLOUR, NOT FROM A NUMBER.**
*"The stamina bar has lost its colour now, it used to be yellow?"* — and it had. So had the
scorebar's "UFC 4:48" text. Both are HUD gradients, and the sweep was zeroing the textures they are
drawn from:
```
ZeroNewImages: addr=0x1155f20000 bytes=65536 extent=128x1   fmt=71    <- a colour ramp LUT
ZeroNewImages: addr=0x11e0201000 bytes=12288 extent=128x128 fmt=134   <- a BC-compressed texture
ZeroNewImages: addr=0x11e0858000 bytes=8192  extent=64x64   fmt=138
```
**`Image::IsUninitialized()` could not tell "never had contents" from "was uploaded and is now
clean".** From the code, and it is worth writing out because the two states really are identical
without an extra bit:
```
IsCpuDirty() == m_cpu_dirty || m_maybe_cpu_dirty
a new image is marked MAYBE-cpu-dirty, so the first bind reaches InitializeImage
InitializeImage uploads real guest content ... then RefreshComplete() clears BOTH dirty bits
and the upload path never sets m_gpu_modified
=> by end of frame an UPLOADED texture looks exactly like one that never had contents
```
The sweep runs at end of frame, so it caught every small texture that had been uploaded that frame.
**Fix: `image.MarkContentsDefined()` on a successful upload** (flag renamed from
`m_zero_initialized`, which was the wrong concept — it is "contents are defined", not "we zeroed
it"). An image that has not been bound yet is still maybe-cpu-dirty and was always skipped; the only
images the sweep now touches are those that were bound, found clean, and received no upload — which
is exactly the exposure texture's state.

**THE LESSON IS ABOUT THE GUARD, NOT THE FEATURE.** The guard was copied from the dispatch
inspector's `coherency=uninitialized` classification on the grounds that it was "exactly the
inspector's own classification". That was true and still wrong: the inspector uses it to *describe*,
where a miss costs a confusing log line, and the sweep uses it to *destroy*, where a miss costs real
texture data. **A predicate is only as good as the consequence attached to it.**

**And on how it was found:** three quantitative checks passed on this change — the texel went to 0,
the guard fired, the tonemapper's channels went from 1 distinct value to ~28k, and the perf A/B came
back clean. **None of them looked at anything the sweep was not aimed at.** The regression was found
by a person noticing a yellow bar had gone grey. Keep asking.

**17. THE FIX'S VERIFICATION WAS WORTHLESS: EVERY "AFTER" READING WAS TAKEN AT THE MENU, WHERE THE
BUG DOES NOT OCCUR. THE EXPOSURE TEXTURE MOVES ADDRESS BETWEEN SCENES.**
```
run        frame  scene       addr          image_id   texel
sweepoff      52  MENU        0x1162c00000  150.1      0
zeroinit4    303  MENU        0x1162c00000  ...        0
zeroinit2     70  MENU        0x1162c00000  ...        0        <- the "verified, fix works" reading
cbdump2     1767  PRE-FIGHT   0x1162e90000  3555.7     1.048e-19  <- the actual bug
```
**`0x1162e90000` is the aliased transient slot; `0x1162c00000` is a different, clean allocation that
reads 0 with or without the fix.** Every "after" measurement landed on the clean one, so "the texel
is now exactly 0 and the guard fires" was true at the menu *before* the fix as well and demonstrated
nothing. Proved by running with `KYTY_CLEAR_UNINIT_MAX_BYTES=0` (sweep fully disabled): the texel
still read 0 and the guard still fired — with the feature switched off.
- **The diagnosis is unaffected.** 1.048e-19 at `0x1162e90000` in the pre-fight and the round is
  measured, repeatedly, and undefined VkImage contents remain the only explanation offered for it.
  What is retracted is the claim that the sweep was shown to change it.
- **The trap, and it is a general one: measure where the bug lives, not where measuring is cheap.**
  The menu was chosen for every verification because it needs no navigation. The bug needs a scene.
  A control that cannot exhibit the failure cannot confirm a fix, and three "confirmations" in a row
  agreed with each other precisely because none of them was looking at the defect.
- **The A/B that actually settles it** is one navigation, because the control already exists on disk
  (`cbdump2`, 1.048e-19 at `0x1162e90000`, frames 1767/1869/1941): run with the sweep ON, get to the
  pre-fight, read the texel at `0x1162e90000`. Do not accept a reading from any other address.

**18. THE ROUND'S GEOMETRY IS PERFECT. LOOK AT THE PICTURE, NOT THE OCCUPANCY NUMBER.**
The user asked whether the black round could be exploding geometry, and pushed back when that was
dismissed on the grounds that `i0` is "99.3% nonzero". **They were right to push back — that is an
occupancy number, and this ledger's own first trap is that occupancy is not correctness.** Exploded
geometry would pass it.

Decoding the round's `i0` capture (`input-0c399ab0b1e7fa33-p0-i0-1176ae0000-f1941.bin`, raw
B10G11R11, 1068x600) and applying a sane exposure renders **a completely correct scene**: both
fighters, the referee, the cage, the mat, the crowd, all properly lit and posed.
**So: geometry, transforms, culling, materials and lighting are all fine in the black round. The
frame is drawn correctly at render resolution and destroyed entirely downstream.** Wireframe would
have shown nothing wrong; do not spend a build on it for this bug.
**The tool is three lines of numpy and it is on disk** — decode the B10G11R11 dump, divide by the
median, Reinhard, gamma. A picture answered in one minute a question that a build-and-navigate cycle
would have answered in an hour, and answered it better.

**19. THE SPECKLE AND THE RAINBOW SKIN ARE ALREADY IN `i0`, UPSTREAM OF THE TONEMAPPER.**
Visible directly in that render: white salt-and-pepper dots across the whole frame and the oil-slick
colours on the fighters' skin (issue 4b) are **present in the render-resolution scene colour before
`CS 0x0c399ab0b1e7fa33` runs**. Session 12 established the speckle "comes from a different buffer"
than the upscaler history; this identifies which one, and it is much earlier in the frame than
assumed.
```
scene median luma                      0.0063741
pixels > 1000x the median              33,938 = 5.30%
those pixels: median luma 38,121, max 64,512 (saturated); 11,313 saturated in R alone
lattice test  x&1 49.9/50.1   y&1 49.8/50.2   (x+y)&1 49.9/50.1
              x&3 25.0/25.0/24.9/25.1        y&3 24.6/25.2/25.2/25.0
```
**Spatially random, not a checkerboard or quad pattern** — the same negative lattice result session
12 got for the NaN mask, now measured on the buffer where the speckle actually lives. ~5% of pixels
blown to saturation at random, in a buffer that is otherwise a correct render.
**This is a much better starting point for issue 4b than "somewhere in post".**

**20. ATTEMPT 3 AT THE ZERO-INIT FIX ALSO FAILED. STOPPING.** Retaining undecided images across
sweeps (rather than considering each once) still clears **nothing**, and at the scene address the
texel is unchanged across three consecutive frames:
```
sweepon2, addr=0x1162e90000, frames 810 / 821 / 833
  |texel|^(1/6) = 45/65535 -> 1.048e-19   (identical to the control)
  v3 after the guard = 0                  (the guard still does not fire)
  images cleared by the sweep = 0
```
**The sweep has never once touched the image it was written for, in any build.** The 16 images it
cleared in the earlier builds were all *uploaded* textures — which is what wiped the HUD gradients.
Cause: the `maybe-cpu-dirty` lifecycle never leaves the image in the "clean, with no contents" state
the guard tests for. A new image is marked maybe-dirty; the first bind only records a guest hash and
returns; and re-marking keeps `IsCpuDirty()` true whenever the aliased guest range is touched.
**The one approach not yet tried:** clear at image **creation** in `InsertImage` using
`m_scheduler.Current()` directly. At creation the image definitionally has no contents, which
sidesteps the lifecycle entirely. The risk is calling `EndRendering` mid-recording.

**21. THE ZERO-INIT FIX IS REVERTED. THE DIAGNOSIS IS KEPT.**
Three attempts, none of which ever touched the image the fix was written for, and one of which
wiped real uploaded textures. Carrying a default-on feature that does not work and has broken
rendering once is worse than carrying nothing, and a default-off version would be dead code whose
knowledge already lives here. `textureCache.cpp/.h`, `image.h` and `swapchain.cpp` are back to their
pre-fix state; the analysis in points 1-13 and the failure modes in 15-17 and 20 stand.
**The approach to try next is clearing at image CREATION in `InsertImage`, using
`m_scheduler.Current()` directly** — at creation the image definitionally has no contents, which
sidesteps the `maybe-cpu-dirty` lifecycle that defeated all three attempts. The risk is calling
`EndRendering` mid-recording.

**22. WIREFRAME ON F12.** `SceneDrawDebug::ToggleWireframe()` overrides
`static_params.polygon_mode` to `eLine` for every graphics pipeline.
- **It self-invalidates the pipeline cache for free:** `polygon_mode` is already part of
  `GraphicsPipelineKey::static_params`, so flipping it produces a different key and the cache builds
  a second permutation instead of serving the filled one. No generation counter needed - unlike the
  shader probe, which had to add one.
- `fillModeNonSolid` is already a required device feature (`vulkanWindow.cpp` enables it) and
  `rasterizer.lineWidth` is already `1.0f`, so `eLine` needs nothing new from the device.
- **It is global**, so the UI draws wireframe too and the menus become hard to read. Toggle off to
  navigate.
- **It will not help with the black round** - see point 18, the geometry is already proven correct.
  It is the right tool for missing or mis-transformed geometry, which is not this bug.

**23. WIREFRAME CONFIRMS IT AT THE TRIANGLE LEVEL: THE BLACK ROUND'S GEOMETRY IS SOUND.**
F12 wireframe ON, in the round, captured through the tonemapper's `i0` and decoded offline
(`dumps/round-wireframe-f929.png`). **The mat's triangle fan radiates cleanly from the centre, the
cage is a regular mesh, both fighters and the referee are correctly posed and tessellated, and the
crowd tiers are in place.** No stray vertices, no triangles reaching off-screen, no degenerate fans -
and the mat fan is precisely where exploding geometry would show, so this is the strongest possible
negative. **Exploding geometry is dead as a hypothesis.**
This is a stronger result than point 18: not "the shaded image looks right" but "the triangles are
right".

**Why the live wireframe view showed almost nothing:** the scene is rasterised into the 1068x600 HDR
buffer, which is destroyed downstream, so its wireframe is destroyed with it. What survives to the
screen is only the fullscreen post-process triangles (the corner-to-corner white diagonal is one
such triangle's hypotenuse - these passes are `DRAW indices=3 instances=1`) and the UI quads (visible
as diagonals in the scorebar). **The technique that works is wireframe ON + capture `i0` + decode
offline**, which sidesteps the broken post chain entirely.
Also expected, not a bug: a GLOBAL wireframe leaves the depth prepass rasterising only edges, so
every depth-sampling pass downstream produces the blocky garbage seen on screen.

**24. THE SPECKLE IS ON THE SURFACES, NOT IN SCREEN SPACE.** At the exposure used for the wireframe
capture, the stippling sits ON the fighters, the crowd and the cage - it follows the geometry. With
point 19 (already in `i0`, ~5% of pixels at saturation, no lattice) this places issue 4b in shading
or texture sampling **at the point the scene is drawn**, not in any post pass and not in the
compositor. That is a much smaller search space than "somewhere in post".

**TOOLING: `tools/scene_shot.py`** (see point 25 for the full workflow).
**Reach for this before theorising about what a buffer contains.** It cost a minute and settled two
hypotheses that would otherwise have cost builds and navigations. Reference images kept:
`dumps/round-scene-f1941.png` (shaded) and `dumps/round-wireframe-f929.png` (wireframe).

**25. ON-SCREEN WIREFRAME CANNOT WORK IN THIS TITLE. SCENE SHOTS ARE NOW A KEYPRESS.**
Two independent reasons, neither a defect in the wireframe path:
- **The scene wireframe never reaches the screen.** The scene is rasterised into the 1068x600 HDR
  buffer that post destroys, so its wireframe dies with it. Only the fullscreen post triangles (one
  corner-to-corner diagonal each) and the UI quads survive to be seen.
- **A GLOBAL polygon-mode override collapses the depth prepass.** Depth is written only along
  triangle edges, so the depth buffer is nearly empty and every pass that samples it - lighting,
  occlusion, the compositor - reads garbage. That garbage is the blocky red/white/green mess on
  screen. Inherent to the override; not tunable.

**The workflow that does work, now bound to a key:**
```
F12        toggle wireframe
Ctrl+F12   capture the scene colour buffer (SceneDrawDebug::DumpSceneShot)
python tools/scene_shot.py            decode the newest capture to a PNG
python tools/scene_shot.py --watch    decode every new capture as it lands
```
`DumpSceneShot` just writes the existing `CAPTURE_HASHES` + `DUMP_INPUTS` trigger files (hash
overridable with `KYTY_SCENE_SHOT_HASH`), so it reuses the proven capture path rather than adding a
second one. It writes `2` as the occurrence count, which removes the `KYTY_CAPTURE_OCCURRENCES`
footgun for this shader permanently.

**`tools/scene_shot.py` replaces the ad-hoc decode script.** It auto-exposes off the median, then
Reinhard + gamma, and prints the exposure it chose along with the NaN fraction and max - all three
are diagnostic. **This is why the emulator's own `.bmp` dumps looked empty and cost this project
time: they clamp HDR to 8 bits, and the scene's median luminance is ~0.002, so every one of them is
essentially black.**

**METHOD NOTES WORTH KEEPING**
- **"Occupancy is not correctness" has a twin: a UNORM store cannot tell 0 from 1e-19.** The first
  bisection concluded "v3 == 0, so the cndmask is broken" and was wrong for exactly that reason.
  Check the readout's dynamic range against the hypothesis before believing the reading.
- **Taps fire BEFORE the instruction at their pc.** To read a value produced at pc P, tap at P+1.
- **A probe with no `vgpr=` hijacks no store**, so `mark=`/`tap=` can *change* the game's behaviour
  live without disturbing any output. That turns the probe from an instrument into an actuator, and
  it is how the on-screen confirmation was obtained with no rebuild.
- The whole session needed **one** build. Everything else was live file writes (`PROBE`,
  `CAPTURE_HASHES`, `DUMP_INPUTS`, `TRACE_ADDRS`, `CLEAR_IMAGES`) against a running game.

**NEXT, IN ORDER**
1. **`CS 0xe17349e0d437757b`, the upscaler** - now the only thing between a correct tonemap and a
   visible frame, and reproducible against known-good input for the first time
   (`dumps/input-*-f2501.bin`). It saturates a healthy input to 3 distinct values and then NaNs.
2. Only once that is fixed: decide whether the exposure fallback of 1.0 is the game's intended
   value, or whether a real producer for the 1x1 `R32_SFLOAT` is still missing. It cannot be judged
   by eye before then.
3. Re-measure `frame_stats.py` against the 5.00 / 79.0 us-per-draw baseline with the zero-init
   sweep on.

### SESSION 14 (2026-09-13) — THE STUB IS EXONERATED, i4/i7 WERE MISREAD, AND A 118-INSTRUCTION SHADER IS THE NEW LEAD

**1. THE STUB DOES NOT CLEAR THE UPSCALER'S INPUTS. Hypothesis dead.** Added per-image logging to the
skip stub (`renderCompute.cpp`). It clears:
```
addr=0x1164b20000  1600x904  fmt=97  (R16G16B16A16_SFLOAT)
addr=0x1165750000  1600x904  fmt=100 (R32_SFLOAT)
```
The upscaler's B10G11R11 surfaces are **1600x900 fmt=122** — different format, extent and surface.
Tracing both stub addresses through the whole log: touched by **nothing but the occlusion CS itself**
(read-write storage + read-only sampled, 75/70 hits). They are its private Hi-Z working buffers and
zeroing them is the intended "nothing occludes".

**2. CORRECTION TO SESSION 12: i4/i7 ARE THE UPSCALER'S OWN OUTPUT PING-PONG, NOT SCENE INPUTS.**
```
frame 705 pass 0:  115f570000 is image=7   read
frame 706 pass 1:  115f570000 is image=4   read
frame 706 pass 1:  115f570000 is image=11  WRITE
```
`11bc800000` <-> `115f570000` is a B10G11R11 ping-pong written by `i11`, exactly as
`12a2800000` <-> `12a3800000` is the RGBA16F history ping-pong written by `i12`. **So "the fresh
scene inputs i4/i7 are empty in the round" was wrong** — their emptiness is an EFFECT of the
upscaler producing nothing. The genuine render-resolution inputs are i0, i1, i2, i9, i10 at 1068x600.

**3. THE IGNORED-REGISTER INVENTORY IS SMALL, CONSTANT, AND DOES NOT VARY BY SCENE.** Added
`NoteIgnoredRegister` (first value + counts at 1k/100k; `KYTY_LOG_IGNORED_REGS=0` silences).
Everything the game actually writes, identical in menu and walkout:
```
DB_ALPHA_TO_MASK          0x0000aa00                      x100000+
TA_BORDER_COLOR_BASE      0x1115239d / 0x11500000 / 0      x100000+
DB_depth_metadata(HTILE)  0x11000100                       x1000+
PA_SC_AA_MASK             0x00000000                       x1000+
PA_SC_MODE_CNTL_1         0x06020000                       x1000+
```
**FSR, CB_DCC_CONTROL, PA_SC_FOV_WINDOW and every VGT_* never fire at all.** Values never change
between scenes, so ignored registers cannot explain the walkout/round difference. They remain
candidates for *constant* baseline corruption only.

**4. OCCLUSION QUERIES: real bug fixed, no visible change.** The synthetic result wrote only the EVEN
(begin) slots with a counter that incremented every dump, leaving ODD (end) slots as stale guest
memory — the guest computed `end - begin` from a drifting value minus garbage. Now writes both halves
with a fixed positive difference (`KYTY_OCCLUSION_LEGACY=1` restores the old path). **Round still
black.** Honest negative: real defect, not this one.

**5. THE NEW LEAD — `CS 0x0c399ab0b1e7fa33`, 944 bytes, ~118 instructions.**
Found by putting `TRACE_ADDRS` on the upscaler's i10 (`0x1161fb0000`, 1068x600 `R8G8B8A8_UNORM`),
which reads all-zero in **every** scene:
```
ResourceTrace frame=1635 hash=0x0c399ab0b1e7fa33 image=2  read=true  write=false addr=0x1161fb0000
ResourceTrace frame=1635 hash=0x0c399ab0b1e7fa33 image=3  read=false write=true  addr=0x1161fb0000
ResourceTrace frame=1635 hash=0xe17349e0d437757b image=10 read=true  write=false addr=0x1161fb0000
```
It read-modify-writes that surface and the upscaler samples it. **Measured, same dispatch:**
```
i0 @0x1176ae0000  1068x600 RGBA8   97.6% nonzero, 0..255, 256 distinct   REAL DATA IN
i2 @0x1161fb0000  1068x600 RGBA8    0.0% nonzero, all zeros              NOTHING OUT
```
Opcode profile is **tonemap-shaped**: `V_LOG_F32` x2 + `V_EXP_F32` x2 (= pow), `V_RCP_F32` x5,
`V_MAX_F32` x8, `V_MIN_F32` x3, `V_MUL_F32` x19, 3 `IMAGE_LOAD`, 3 `IMAGE_STORE`. That fits the
user's symptoms — "colours wrong, hue wrong, sometimes bright red and white" is what a broken
tonemapper looks like — and it is present in the WALKOUT too, not only the round.

Structure; the third store is EXEC-gated exactly like the upscaler's block 4:
```
0x0228  IMAGE_STORE v3, v10, s4    dmask=0xf
0x0234  IMAGE_STORE v0, v10, s12   dmask=0xf
0x0240  V_CMPX_GT_I32 exec_lo, vcc_lo, v10
0x0244  V_CMPX_GT_I32 exec_lo, vcc_hi, v11
0x0248  S_CBRANCH_EXECZ 0x0264
0x025c  IMAGE_STORE v0, v10, s4    dmask=0x1 d16=1
```
**WHY THIS IS THE BEST TARGET THE PROJECT HAS HAD:** 118 instructions instead of 969; it dispatches
in menus and walkouts so it needs **no fight navigation**; one clearly good input and one clearly
dead output; and it feeds the upscaler. Small enough to be a realistic offline-replay candidate once
`RunCase` can bind more than one sampled image.

**METHOD TRAP HIT AGAIN:** `input-*` captures are dumped at BIND time — **before** the dispatch. They
show what a shader READ, never what it wrote. To see output, capture on a LATER dispatch (the next
dispatch's input is the previous dispatch's output) or use the panel's armed before+after dump. An
all-zero `i2` on input is also consistent with a self-sustaining zero: if this is a read-modify-write
accumulator whose operation is multiplicative in its own previous value, zero in gives zero out
forever — the same shape as the upscaler's NaN loop.

### SESSION 13 (2026-09-13) — THE GPU TEST SUITE IS ALIVE AGAIN, WAVE64 IS CORRECT, AND STAGE 2 IS ARCHITECTURALLY BLOCKED

**THE HEADLINE: there is now a fast local loop. Seconds, no game, no navigation, no TDR.**
```
cd D:\PS5\src\KytyPS5\_Build\windows
.\shader_recompiler_compute_tests.exe --wave64-only      # ~26 cases, exit 0
```
~30 other selectors exist (`--ds-inc-only`, `--scheduler-only`, `--descriptor-heap-only`, …), so most
of the suite is reachable. **Use this before spending a navigation on anything instruction-level.**

**1. THE SUITE WAS DEAD, NOT FAILING — TWO PRE-EXISTING BREAKAGES, BOTH FIXED.** It aborted before a
single GPU case ran; confirmed pre-existing by building HEAD with **no** local changes.
- Diagnostic instrumentation reads the frame number unconditionally through
  `GetGpu().GetFrameNum()` (`descriptors.cpp:974` is on every dispatch's binding path), and
  `BufferCache` marshals readbacks with `GetGpu().SendCommandSync()`. The harness builds a
  `RenderContext` directly and never calls `InitializeGpu`, so every one hit
  `EXIT_IF(m_gpu == nullptr)`. Fixed with `RenderContext::DiagnosticFrameNum()` (returns 0) and
  `HasGpu()`; emulator behaviour unchanged. Commit `d68fc24`.
- `CheckPm4WaitResume` drives a `CommandProcessor` without `scheduler.Begin()`, and is the one PM4
  case that reaches a `Flush` (via `IT_WAIT_REG_MEM` suspend/resume) → `Submit` on an invalid
  command buffer. Commit `902fede`. **0 → 58 tests.**
- Still broken further along: a `PageManager` teardown fail-fast ("destroyed with live page state")
  around the RenderExecutor discovery cases. **Not worth fixing — the selectors bypass it.**
- Environment trap, again: `--wave64-only` needs the full 13,824 MB commit reservation
  (`PhysicalMemory::TotalSize()`, hardcoded constexpr, correct — it is the PS5's real memory size).
  It failed at 10.2 GB free commit and passed at 13.2 GB. **Close apps or raise the page file.**

**2. WAVE64 EMULATION IS CORRECT. All 26 compute cases pass, exit 0.**
```
DsBpermuteWave64UsesIndependentHalves   Wave64CrossHalfLaneAndLds
Wave64RawMasksAndScalarBranch           Wave64PartialMultidimensionalWorkgroup
Wave64AppendConsumeHighHalf             Wave32VccMasksPreserveOtherHalf
+ ScalarSaveExecOps, ScalarOrn2Saveexec, ScalarWqmB64*, ScalarMaskProvenance*,
  BranchVccnzUsesWaveMask, DispatcherIrreducibleControlFlow, ScratchIsPrivatePerInvocation
```
**The pair-packed wave64 path is slow, not wrong.** It is not silently corrupting anything these
cover — cross-half lane ops, LDS interaction, raw mask reads, scalar branches on wave masks,
partial workgroups, append/consume on the high half. This closes a long-standing suspicion. Passing
tests prove only what they cover, but this is real coverage of exactly the suspect mechanisms.

**3. WAVE64 STAGE 2 IS BLOCKED BY DESIGN, NOT UNFINISHED. DO NOT RESUME IT.**
A prior attempt is parked in `git stash` ("wave64 stage2 WIP (Cursor/Grok) - broken"). It is 308
lines and the hard part is written, but it cannot work:
- `EmitWave64SharedBallot` assembles the 64-bit mask from two subgroups via LDS +
  `EmitWave64WaveBarrier`, which emits **`OpControlBarrier` at Workgroup scope**.
- It is called from the Ballot lowering (`spirvEmitterProgram.cpp:701`) — i.e. **every** EXEC/VCC
  evaluation — and this shader evaluates masks **inside divergent control flow** (34
  `S_CBRANCH_EXECZ`, nested four deep). A workgroup barrier in non-uniform flow is undefined
  behaviour, and it over-synchronises all 4 waves of the 256-thread group.
- The escape (one wave per workgroup, so the barrier is uniform by construction) is closed: the
  shader uses `s_barrier` (`0x159c`, `0x1658`) and `DS_APPEND` across the full group with shared LDS.
- **Root reason:** pair-packing exists *because* it keeps all 64 GCN lanes inside one 32-wide host
  subgroup where subgroup intrinsics work with no cross-subgroup traffic. `lane_count=1` on a
  32-wide host makes every wave-wide value cross a subgroup boundary, and Vulkan offers no
  primitive for that which is legal in divergent flow. NVIDIA cannot widen the subgroup
  (`VK_EXT_subgroup_size_control` reports min=max=32).

**4. AND IT WOULD NOT HAVE PAID ANYWAY.** Selects are ~1,771 of ~8,000 instructions; Stage 1's 26%
select cut yielded "~10-15% fewer instrs". Eliminating **every** select caps out near **15-20%**.
Against a measured **3,400×** gap (one work item: 1 ms walkout vs 3,426 ms round) no codegen work
is relevant. **The round's blowup is data-dependent, not throughput.**

**5. LOOP 50 IS A LINKED-LIST WALK WHOSE NEXT POINTER COMES FROM A STALE BUFFER.** Bisecting the
6 nested loops localised the runaway to loop 50 (header block 50, `0x1130`):
```
0x112c  S_MOV_B64 s60, exec_lo            save EXEC for loop exit
0x1130  V_CMPX_NE_U32 exec_lo, -1, v55    EXEC = (v55 != 0xffffffff)   <- header
0x1134  S_CBRANCH_EXECZ 0x1488            all lanes hit sentinel -> exit
0x1148  S_LOAD_DWORDX4 s40, s0, offset=240
0x1190  BUFFER_LOAD_DWORD v54, v46, s40   <- THE NEXT POINTER, from a buffer
0x1478  S_MOV_B64 exec_lo, s62            restore EXEC
0x1480  V_MOV_B32 v55, v54                v55 = next
0x1484  S_BRANCH 0x1130
```
`v55` is initialised at `0x1124 V_CNDMASK_B32 v55, -1, v10` — `-1` is the sentinel. **The list lives
in a buffer the CS builds itself, and the stub logs `cleared_buffers=0`, so those buffers are
STALE.** Zero is not a terminator, so a zeroed/garbage buffer walks forever. **Structural
consequence: one-shot execution can never succeed** — the shader builds the list it later walks, so
executing it once into a buffer it has never populated chases garbage regardless of loop caps.

**6. THE PROBE READOUT IS FORMAT-LIMITED — READ `actual_vk` FIRST (again).** For this CS:
`image 0/3 @1170210000 = R16G16B16A16_SFLOAT`, `image 1/4 @1170e40000 = R32_SFLOAT`,
`image 2 = D32_SFLOAT_S8_UINT`. Taps are **integers**; through the f16 store at `0x1600` any small
integer becomes a denormal and **flushes to 0**, indistinguishable from a real zero. Use the
`R32_SFLOAT` store at `0x1618` (dmask=0x1, channel 0 only) for values that must survive. Also:
`group_cap=1` writes ~256 pixels into a 1.44M-pixel image, so the probe's output cannot be found by
inspection — that image already holds 1.39M small integers (it is the shader's own head/count
buffer, typed float).

**PROBE syntax gotchas:** `loop_header` is a **block index** (`50`), not a pc; taps/marks/execlo are
pcs. The `GDS capped dispatch done … ms=` timing line only appears when the GDS cap actually binds
(`gds_limit` below the real item count).

**THE MEASUREMENT DEFECT, AND IT INVALIDATES EVERY PROBE NUMBER IN SESSION 7.**
`CS 0xe17349e0d437757b` is dispatched **twice per frame**, and the two dispatches bind **different**
ping-pong images (one capture: `p0` history `0x12a3800000`, `p1` history `0x12a1800000`; identical
`groups=200x113x1 local=8x8x1`, so they are indistinguishable in the dispatch log). Both are real
game passes, not an emulator replay — the bindings differ. `CaptureShaderInputs` recorded only the
**first** binding of a hash per trigger (`captured.insert(hash).second`), so the probe values written
by one pass were read back out of the other pass's image. **The same probe at the same pc gave
`MARKX@0x113c` = 99.87% ones in one run and 0% in the next.** That is the contradiction that exposed
it; it also explains session 7's `v87` reading "0..4 in one run and 0..1.53e-5 in the next".
- **FIX: `KYTY_CAPTURE_OCCURRENCES=<n>` (default 1, i.e. old behaviour).** The pass index is now in
  the dump filename (`input-<hash>-p<N>-i<M>-<addr>-f<frame>.bin`) and in `CaptureBinding: pass=N`.
  **Set it to 2 for this shader or the reading is meaningless.**
- With it on, three consecutive frames in one run agree to within a couple of percent. Cross-run
  agreement is still not guaranteed — `v87` is genuinely scene-dependent (47-56% zero in one run,
  0.13% zero in another), so **correlate channels inside one image; never compare across runs.**

**THE RESULT (both passes, 3 frames, 1,440,000 pixels each, no exceptions):**
```
waves entering blocks 3-5 (EXECZ@0x113c == 0) : 100.00% of pixels
block 3 entry marker (raw, at 0x1140) set     : 100.00%
block 4 entry marker (raw, at 0x148c) set     : 100.00%
block 6 entry marker (control, at 0x15d4)     : 100.00%
```
**Blocks 3, 4 and 5 execute, exactly where the branch condition says they should.** Control flow
through the `V_CMPX` region is correct. Also measured: `EXECZ@0x190` (the history-validity test that
guards block 1) is **0 on 100%** of pixels, so block 1 runs too.

**WHY 7c WAS WRONG — the inference had a hole, not the measurement.** 7c probed `v11,v12,v13,v17`
at the store, found them uniform, and concluded block 4 never ran. But **all four are also written
in block 3** (`v11`@`0x13b0`, `v12`@`0x1414`, `v13`@`0x1460`, `v17`@`0x1368`), so "block 4 skipped"
predicts block 3's per-pixel values there, not constants — the reading never fit either hypothesis.
And the positive control was no control: **`v63` is written in block 2 at `0x1108` and `v74` at
`0xfd4`**, so their per-pixel variance never proved block 3 runs either. The uniform triple has a
plain identity: `0x00000ccc IMAGE_LOAD v11, v10, s44 ; dmask=0x7` writes `v11,v12,v13`, and
`(0, 0.49976, 0.49976)` is `(0, 0.5, 0.5)` — the zero-motion motion-vector value — with 0.5 rounded
toward zero in f16 (`0x37FF`). Those were block-2 values, read out of the wrong pass's image.
- **Withdrawn: "block 4 never executes", "block 3 does run (v63 has 1065 distinct values)", and the
  `S_AND_SAVEEXEC_B32 -> EXEC -> structurizer` lead that followed from them.** The EXECZ fix in
  `b238b33` is still a genuine ISA conformance fix and is kept; it changed nothing here (re-probed:
  `v11,v12,v13,v17` bit-identical before and after).
- **Rule this cost us twice now: a register that is written in more than one block cannot tell you
  which block ran.** Use a marker written only at the pc under test, and a control marker in a block
  known to execute.

**THE EXECZ LOWERING IS CORRECT, from the emitted IR (not from reading the translator):**
```
%3995 = FPOrdLessThan32 0f, %3698        ; v87 > 0, per lane      (uses=163)
%3996 = Ballot %3995
%3997 = CompositeExtractU32x4 %3996, 0   ; the EXEC mask WORD
%4001 = IEqual32 %3997, 0                ; the branch condition
```
`%3698` is the `V_MIN3_F32 v87, v56, v73, v61` at `0xce4`. This is the ISA-correct wave-wide test.
Note `v61` enters block 2 as `Phi [0x00000000, $1], [%3085, $2]` — a hard zero if block 1 is
skipped — so *if* the history-validity branch ever did mis-fire, `v87` would go to 0 and the region
would be skipped for free. It does not mis-fire (measured above), but that is the chain to re-check
if this ever changes.

**Also settled, and it narrows the search:** the shader's **other** store (`0x16c4`, dmask=0x7, into
the `B10G11R11` ping-pong) is **healthy** — 1,220 distinct values, **0% NaN, 0% Inf**, and its
channels match the ISA reading exactly (`v0 = 2*s36 + v9 >= 2`, `v2 = min(1.0, v3) <= 1`). Block 6's
arithmetic works. The corruption is confined to the `v29/v30/v31` path.

**NEW TOOLING — an arbitrary-pc probe, which is what closes the "cannot tell whether a block ran"
gap the last session recorded.** All opt-in, default inert, scoped by `KYTY_PROBE_HASH`:
```
KYTY_PROBE_MARK=<pc>:<dst>[,...]        at <pc>, v<dst> = 1.0 for every invocation that reaches it,
                                        ignoring EXEC          -> "was this block executed"
KYTY_PROBE_MARKX=<pc>:<dst>[,...]       same, but only for lanes whose EXEC bit is set
                                        -> "was this lane active here"
KYTY_PROBE_TAP=<pc>:<src>:<dst>[,...]   at <pc>, v<dst> = v<src>, ignoring EXEC
KYTY_PROBE_EXECLO=<pc>:<dst>[,...]      at <pc>, store the wave-wide EXEC mask WORD
KYTY_PROBE_EXECZ=<pc>:<dst>[,...]       at <pc>, store MaskIsZero(exec) as 1.0/0.0 - the exact
                                        condition an S_CBRANCH_EXECZ branches on
```
`<pc>` hex, registers decimal. Destinations should be scratch registers **above everything the
shader allocates** (v88+ here; the file is 256 deep and `vector_limit` is raised to cover them).
Every destination is zeroed at pc 0, so 0 means "never written", not "undefined". Read them back
with `KYTY_PROBE_VGPR` at the store as before.
- **EXEC has two representations and they are separate state**: the per-lane bit (`GetExec()`, what
  `MARKX` measures, what predicates VGPR writes in `WriteOperand`) and the mask word (`GetExecLo()`,
  what `MaskIsZero` and therefore every EXECZ branch tests). Probe the one your question is about.
- **A marker written inside the region under test still has to survive the merge to be read at the
  store.** That is why the run that settled this measured the branch condition *and* the markers in
  one image and cross-tabulated them per pixel: markers set on exactly the pixels whose wave entered
  is proof of execution; markers clear where the wave entered would have been proof of a lost merge.
- Validated offline first: `shader_cfg_tests` green with the probes set and unset (and note the
  scratch writes are dead-code-eliminated when nothing reads them, so word counts do not move —
  that test proves the build is sound, not that a tap fired; the in-run control marker does that).

**THE PROBE IS NOW LIVE-RECONFIGURABLE — a re-aim costs a file write, not a restart.**
`D:/PS5/dumps/PROBE` (override with `KYTY_PROBE_FILE`) is re-read whenever it changes, next to the
existing `TRACE_ADDRS`/`CAPTURE_HASHES` reloads. It is seeded from the `KYTY_PROBE_*` environment at
startup, **replaces** the configuration wholesale when present, and reverts to the environment when
deleted. `D:/PS5/dumps/PROBE.example` documents the keys (`hash`, `pc`, `vgpr`, `mark`, `markx`,
`tap`, `execlo`, `execz`).
- **Why it re-translates instead of serving the cached shader:** changing the probe bumps
  `ShaderRecompiler::ProbeConfigGeneration()`, which is part of `ProgramKey` in `pipelineCache.cpp`.
  The old entry is deliberately **not** erased — `CompiledShaderInfo` pointers handed out earlier
  stay valid, and pipelines are keyed by shader id which is never reused, so the stale permutation
  simply stops being looked up. A re-aim leaks one shader module and one pipeline; that is the right
  trade for saving a navigation.
- **Measured at the menu, no fight needed:** writing the file logs
  `ProbeConfig: reloaded generation=N ...` and the targeted shader's `ShaderCompile: CS` count goes
  up by one on the next dispatch. Three re-aims plus a delete, 381 shader compiles, **0 device
  losses, 0 validation errors, 0 CFG failures.**
- **Trap found by that test, and it would have been silent:** Windows PowerShell's
  `Set-Content -Encoding utf8` writes a **BOM**, so a file starting with `hash=` parsed as the key
  `<BOM>hash` and the whole configuration came back empty — while still bumping the generation and
  re-translating, so it looked like it had worked. The BOM is now stripped and unrecognised keys are
  counted and logged. **A probe that reports `taps=0` is telling you it parsed nothing.**
- The config is taken as a snapshot once per `Translator`, so one translation always sees one
  consistent configuration even if the file changes mid-compile.

**NEXT, and it is a clean slate: every value reading in session 7 needs redoing per-pass.** The
region runs, so the defect is in what it computes. Re-probe `v29,v30,v31` and the block-4 outputs
with `KYTY_CAPTURE_OCCURRENCES=2`, then bisect the arithmetic with `KYTY_PROBE_TAP` at chosen pcs —
the tool can now read any register at any point, so this no longer costs one navigation per guess.

**Housekeeping:** the emulator reserves **13,824 MB of commit up front**; two startup failures this
session were `could not reserve 13824 MB for guest direct memory`, not crashes, because the previous
instance had not released its commit yet (and one `clang-cl` OOM from the same shortage). Check
commit headroom before blaming the build.

### SESSION 9 (2026-09-12) — P0 DISPATCH/DRAW INSPECTOR BUILT AND VALIDATED AT THE MENU

**What landed.** `KYTY_DEBUG_PANEL=1` enables an opt-in recorder and opens an ImGui GPU inspector;
F10 toggles panel visibility and is consumed by the host, never sent to the guest. With the env var
unset the renderer takes only a cached false branch: it allocates no capture state, does not create
the ImGui Vulkan backend, and does not add presentation draws. The panel reuses `SystemOverlay`'s
existing ImGui context, dynamic-rendering path and `imgui_impl_vulkan` backend.

For every real Vulkan draw and dispatch in the current frame the shared recorder captures:
- operation order and draw/dispatch type;
- all active shader stages and full 64-bit shader hashes;
- dispatch groups/local size (or indirect argument address), draw index/instance counts;
- every resolved buffer and image binding with guest address/size and read/write/atomic flags;
- texture-cache `image_id.generation`, extent, and **the format of the actual returned VkImageView**;
- colour and depth attachments with the same address/id/extent/format/access information.

Repeated dispatches are numbered `pass=N/total` per shader hash within one frame. The UI has live,
hold-current and step-one-frame modes, shader-hash filtering, draw/dispatch filters, a clipped
operation table, and a separate resolved-resource table. A loud banner says that panel-on
measurements are not performance-comparable.

**The file/env interface remains authoritative.** The panel only observes resolved renderer state;
it does not replace `PROBE`, `CAPTURE_HASHES`, `TRACE_ADDRS`, `DUMP_INPUTS`, or their environment
seeds. For headless use, create `D:/PS5/dumps/DUMP_INSPECTOR`; its optional contents are a shader
hash. The trigger is consumed and the exact panel snapshot is written to
`D:/PS5/dumps/dispatch-inspector-f<N>.txt`. This parser tolerates a UTF-8 BOM.

**Menu proof, no fight navigation.** With filter `48b8d705615cffba`, frame 459 contained 202 total
operations and showed that this supposedly simple menu CS actually ran **eight times**. Its passes
had identical `groups=134x75x1 local=8x8x1` but different buffer addresses and four distinct output
images, for example:
```
pass 1/8  buffer=0x1115b9c9d0  image=0x1166ec0000  image_id=142.1  1068x600 actual_vk=122
pass 2/8  buffer=0x1115b9b9c0  image=0x1167190000  image_id=140.2  1068x600 actual_vk=122
pass 3/8  buffer=0x11158c5fb0  image=0x1163920000  image_id=51.3   1068x600 actual_vk=122
pass 4/8  buffer=0x11158c5fa0  image=0x1163bf0000  image_id=30.4   1068x600 actual_vk=122
```
That is the exact defect class P0 was intended to reveal. A busier validation-menu frame contained
8,357 operations and exposed the initial 8,192 runaway guard; the guard was raised to 32,768
operations / 524,288 resources. The rebuilt binary then captured frame 475 with 101 operations,
four filtered occurrences, and **zero dropped operations/resources**.

**Validation:** release build succeeded with `ninja -j 4`; `shader_cfg_tests: all tests passed`.
The panel ran at the menu for 187 seconds under `--shader-validation true`: 193 `FrameProfile`
windows and 703 shader compiles, **0 device losses, 0 Vulkan VUID/validation errors, and 0 shader/CFG
compile failures**. The existing `CFG dispatcher fallback` for PS `0x496ccab77f7f9ba4` is a successful
fallback for an indirect `S_SETPC_B64` jump table, not a failed compilation.

The first two validation relaunches failed before Vulkan initialization with the already-known
`could not reserve 13824 MB for guest direct memory`. The previous process's commit released later;
the same binary then started normally. At diagnosis the machine had 18.9/31.9 GB physical RAM used
but 31.8/46.6 GB committed; Firefox's 28 processes accounted for 8.1 GB private commit. Do not call
this startup failure an emulator crash.

**Session 9 extension — dump and black-render diagnosis tools.** The panel now exposes the existing
headless machinery as controls without creating a parallel state path:

- **Dump frame / exact hash / selected operation** writes paired human-readable `.txt` and
  machine-readable `.json` snapshots. JSON keeps 64-bit values as hex strings, includes operation
  order and every resolved binding, and reports exact-address image aliases. The same writer is used
  by `DUMP_INSPECTOR`.
- Selecting a resource builds an ordered producer/consumer timeline of every overlapping range in
  the frame. Exact guest addresses that resolve to multiple `image_id.generation` values are called
  out both in the panel and dumps. The panel can write the selected address to the authoritative
  `TRACE_ADDRS` or one-shot `CLEAR_IMAGES` files.
- A repeated-dispatch comparison shows every binding changed since the previous occurrence of the
  same shader. This makes ping-pong outputs and per-pass constants explicit.
- **Arm shader input capture** writes `CAPTURE_HASHES` plus `DUMP_INPUTS`. The trigger file may now
  contain a decimal occurrence count; the panel writes the selected dispatch's observed total so a
  two/eight-pass shader is not silently truncated to pass one.
- The raw PROBE editor reads/writes/removes the same `PROBE` file (binary text, no BOM), shows the
  active generation, and confirms when the target hash has actually been retranslated for it.

**Extension proof:** release `ninja -j 4` build and `shader_cfg_tests` passed. A live filtered dump
for `0x48b8d705615cffba` parsed successfully as JSON: frame 191 had 196 captured operations, eight
matching dispatches, and zero drops. The final unfiltered writer captured frame 216 with 98/98
operations, zero drops, and **nine exact-address image alias warnings** in both JSON and text. A live
`DUMP_INPUTS=8` test logged exactly one `InputCapture: armed occurrences=8` and consumed the trigger;
the first attempt found and fixed a Windows open-file deletion bug. A temporary PROBE advanced to
generation 1 and forced the target CS to compile as shader id 183, then deletion restored env state.
A validation-enabled panel run accumulated 82 `FrameProfile` windows with zero VUID/validation
errors, device losses, or compile failures. Temporary `PROBE`, `CAPTURE_HASHES`, and `DUMP_INPUTS`
control files were removed after testing.

### SESSION 10 (2026-09-12) — RESOURCE/ALIAS TRACING DEBUGGER ON THE EXISTING INSPECTOR

**Success criterion (not a UFC 5 fix):** deterministically identify the GPU operation at which a
known-good PS5 image becomes incorrect, with shader hash, alias/resource identities, and preferably
the Frostbite CPU submission callsite.

**Implementation plan (from the code, not a redesign).** The session-9 inspector already recorded
draws/dispatches after resource resolution (`CaptureInspectorStage` / `RecordInspectorOperation`)
and showed an O(n) resource timeline plus an exact-address alias warning. Dump/capture already
existed as `DumpGpuImage` / `DumpShaderInput` (command-buffer copy + deferred host write, no
`Finish`). Image identity already lived in `TextureCache` (`image_id.generation`, guest range,
guest format, `tick_accessed_last`, cpu-dirty / gpu-modified / buffer-modified). CPU RIP is
available cheaply at the SYSV ABI submit wrappers (`AgcDriverSubmit*`), not at each PM4 draw —
guest execution is on another thread from the GPU worker. The overlay was extended in place.

#### Files touched
- `src/graphics/host_gpu/renderer/dispatchInspector.{h,cpp}` — resource metadata, per-frame
  index, address match classification, capture arming, JSON dump fields, callsite helpers
- `src/graphics/presentation/systemOverlay.cpp` — guest-address filter, alias inspector, I/O
  tables, navigation, timeline-from-index, before/after arming, operation details
- `src/graphics/host_gpu/renderer/renderDraw.cpp` / `renderCompute.cpp` — viewport/scissor,
  submit id/tick, input/output capture around the real draw/dispatch, resolve recording
- `src/graphics/host_gpu/renderer/cache/textureCache.cpp` — compact COPY operations
- `src/graphics/guest_gpu/graphicsRun.{h,cpp}` — submit RIP copied onto the queued submission
- `src/libs/agc.cpp` — `_ReturnAddress()` at ABI submit entry (not inside `submit_dcb`)
- `src/graphics/host_gpu/renderer/debug.h`, `presentation/window/swapchain.cpp` —
  `DumpInspectorGpuImage` / `DumpInspectorGpuBuffer` reuse the existing copy path

#### Data structures added
- `InspectorCallsite` — guest RIP, module name/base/offset
- Extra `InspectorResource` identity: buffer id, guest format, Vk handle, last-access tick,
  coherency flags Kyty already tracks (`gpu_modified` / `cpu_dirty` / `buffer_modified`). No
  invented stale/current bit.
- `InspectorOperationKind::{Copy,Resolve}` plus submit id, tick, viewport/scissor
- `InspectorFrameIndex` built once per held/copied frame: uses, `uses_by_image`, `ops_by_shader`,
  1 MiB guest-page buckets, alias groups (union-find on overlapping image ranges with more than
  one `image_id.generation`)
- `InspectorCaptureArm` — user-armed, one-shot, matches kind + shader hashes + occurrence
  (index is a hint for Step)

#### Capture synchronization
User-armed only. Inputs copy before the draw/dispatch; outputs copy after. Reuses
`DumpGpuImage`: inserts a GPU copy into the **current** command buffer, restores image layout,
writes `.bin`/`.bmp`/`.json` later via `DeferPriorityOperation`. **Does not call `Finish`/host
wait.** Output capture **ends the current Vulkan render pass** (same as existing input dumps).
Names look like
`frame-<n>_op-<i>_CS-<hash>_input-slot1_addr-<guest>_img-<id>.bin` plus a JSON sidecar
(frame, op, hashes, dimensions, addresses, image ids, formats, extents, ticks, alias id,
access, and a capture_sync note). Compare uses XXH3-64 of captured bytes, labelled as `xxh3`.

#### CPU callsite — what is actually available
Cheap and reliable: `NoteInspectorSubmitCallsite(_ReturnAddress())` in each `AgcDriverSubmit*`
ABI function, copied onto `GuestGpu::Submission`, applied on the GPU worker as TLS for every
draw/dispatch in that command buffer. Display:
`Engine.Render.Core2.PlatformPs5.prx + 0x18A72F0` when `RuntimeLinker::FindProgramByAddr`
resolves the RIP; otherwise `guest_rip=... (unresolved module)`. This is the **submit**
callsite, not a per-draw Ghidra stack. Per-draw walking was not added.

#### Known limitations
- Live path still copies the in-progress frame under the inspector mutex (pre-existing). Index
  rebuilds when the operation count changes; UI queries are O(uses of the filter), not
  O(ops × resources × ops).
- Thumbnails are not silent GPU readbacks. Preview shows `xxh3` after an explicit Capture;
  otherwise a Capture button arms the next use.
- Copy recording can add cache-internal copies to the ~5k operation list. Copies checkbox
  can hide them. Resolve color-targets are recorded as `RESOLVE/COPY`.
- Coherency labels are Kyty's existing flags, not an authoritative stale/current oracle.
- Fight-frame validation of `0x1163770000` / `0x116d300000` / `0x1164650000` / `0x1162c00000`
  was **not** run in this session (no in-fight navigation). The release `kyty_emulator` link
  succeeded after these changes.

#### Overlay overhead
Unmeasured. Keep the banner: do not compare panel-on fps to the 5 fps baseline.

#### Validation workflow (for the next fight hold)
1. Hold a fight frame.
2. Filter guest address `0x1163770000` — list should shrink to overlapping uses only.
3. Select a use, inspect INPUTS/OUTPUTS, `< Previous Writer` / `> Next Reader` / `>> Next Writer`.
4. Arm Capture Before + After, Step one frame, confirm dumps+JSON for that op.
5. Repeat for `0x116d300000`, `0x1164650000`, `0x1162c00000`.

### SUPERSEDED (session 8 refutes this by direct markers): BLOCK 4 OF THE UPSCALER NEVER EXECUTES

**MEASURED, and this is the strongest result of the session.** Nine registers have their last
write in the whole shader inside block 4 (`0x148c-0x15b0`): `v18 v13 v14 v15 v16 v17 v12 v11 v31`.
Nothing downstream can touch them, so their value at the store is block 4's output. Probed four:
```
v11 = 0        100% of pixels   (0x1588 V_MUL_F32 v11.clamp, v11, v4 - would be per-pixel in [0,1])
v12 = 0.49976  100%
v13 = 0.49976  100%
v17 = 0        100%
v31 = 0        100%             (0x1598 V_MAD_F32 v31, v9, v5, v2)
```
Four different formulas, four uniform results. **Block 4 does not run.** Block 3 *does* - `v63`
(last write `0x11d8`) has 1065 distinct values and `v74` (`0x125c`) has 1983, both per-pixel.

**Why this explains everything.** Block 4 holds the last write before the output for `v29`
(`0x15a0`), `v30` (`0x15a8`) and `v31` (`0x1598`) - exactly the registers feeding the three corrupt
channels. `v32`/`v33` (channel 3, the one that was always sane) are last written in block 2. With
`v31 = 0`, block 5's seven adds degenerate, which produces the +/-65504 and the negative colour,
and NaN downstream.

**AND THE CONDITION SAYS IT SHOULD RUN.** Block 4 is entered by:
```
0x1470 V_CMP_LT_F32 vcc_lo, 0, v13      0x147c S_OR_B32 vcc_lo, s4, vcc_lo
0x1474 V_CMP_LT_F32 s5, 0, v2           0x1480 S_OR_B32 vcc_lo, vcc_lo, s5
0x1484 S_AND_SAVEEXEC_B32 vcc_hi, vcc_lo    0x1488 S_CBRANCH_EXECZ 0x15b0
```
`v13` is written only at `0x1500`, *inside block 4*, so the `v13` measured at the store is the same
`v13` seen at `0x1470`: **0.49976, i.e. > 0 for every lane**. So `vcc_lo` is all-ones, EXEC is
unchanged and non-zero (`v87 > 0` on 29.6% of lanes), and the EXECZ branch must not be taken.

**FIX APPLIED, CORRECT, BUT IT DID NOT FIX THE ROUND.** `AddBranchCondition` lowered
`S_CBRANCH_EXECZ`/`VCCZ` with `LogicalNot(GetExec())` - the *current lane's* bit - where the ISA
defines a wave-wide "is the whole mask word zero" test. `MaskIsZero`'s own comment records this
exact defect being fixed in two *read* paths and predicts the symptom ("a branch on EXECZ took the
wrong direction whenever some other lane was still active"); **the branch terminator was a third
site and was missed.** Now uses `MaskIsZero(false)` / `MaskIsZero(true)`. It is a genuine ISA
conformance fix and is kept, but measured after it: scene `0x1162c00000` 1600x900 still 0.87%
nonzero, scanout still 12.5% (HUD only). **The round is unchanged.** The shader *was* recompiled
that run (5 recompile phases for the hash; `_PipelineCache` caches driver pipelines, not our
SPIR-V), so the test was valid.

**THE OPEN QUESTION, and the first thing to measure next:** re-probe `v11,v12,v13,v17` with the
EXECZ fix in place. If they now vary, block 4 runs and the black round has a further cause
downstream. If they are still uniform, the branch is not what is suppressing block 4 and the
`S_AND_SAVEEXEC_B32` -> EXEC -> structurizer path is where to look.
**ANSWERED IN SESSION 8: they are bit-identical after the fix, and the whole inference was wrong —
both branches of that "if" were false. Block 4 executes. See session 8.**

**Audited and correct, on top of the eight above - do not re-read:** `S_OR_B32`/`S_AND_B32` via
`SimpleInteger` -> `WriteOperand` (updates both the mask word and the per-lane bit),
`WriteMask` for SGPR destinations (explicitly does not clobber the neighbouring SGPR in wave32),
and `ReadMask(VccLo)` (derives the lane bit from the word in wave32, consistent with the writes).

**Probe tooling (all opt-in, default OFF):**
```
KYTY_PROBE_VGPR=33,87,29,30   up to four registers, one per stored channel
KYTY_PROBE_HASH=<hash>        REQUIRED for low register numbers, or every image store in the
                              game is replaced and the menus cannot be navigated
KYTY_PROBE_PC=16fc            which store to hijack, by pc - this shader has two
KYTY_CAPTURE_OCCURRENCES=2    MANDATORY for this shader (session 8) - it is dispatched twice per
                              frame and without this the readback mixes the two passes
```
**Session 8 adds arbitrary-pc probes (`KYTY_PROBE_MARK/MARKX/TAP/EXECLO/EXECZ`) - see session 8.**
Read the result out of the upscaler's history via `CAPTURE_HASHES` + `DUMP_INPUTS`, decoding
`input-<hash>-i3-<addr>-f<N>.bin` as `<f2` (it is `vk=97`, R16G16B16A16_SFLOAT).
- **Shader dumps live in `D:\PS5\shader-dumps\hang_cs\`, not the root** - the root copies were
  deleted and I wasted a navigation regenerating what was already on disk.
- The probe writes through an f16 store, so values outside f16 range or below its denormal floor
  are distorted. Trust sign, zero/non-zero and *variance*; do not trust absolute magnitude.
- No register has its last write in block 1, so the store probe **cannot** test whether block 1
  executes. That needs a probe that can store at an arbitrary pc.

**NEXT: bisect inside the shader, do not keep auditing opcodes.** Reading one implementation at a
time has now cost eight correct answers. Add a debug path that stores a chosen VGPR at a chosen PC
into a scratch image, then walk backwards from `v29/v30/v36` through blocks 3-5 to the first value
that diverges from a plausible range. The suspects it should confirm or kill, in order: the
unguarded `0x1630 V_RCP_F32 v2, v5` where `v5 = v33 + v87` (a 0/0 there gives NaN and a near-zero
denominator gives the ±65504 magnitudes), and whether blocks 3-5 apply their results to the right
lanes at all.

### SESSION 7a: find the NaN-producing operation in `CS 0xe17349e0d437757b`. The NaN fraction (~74%) is
large and stable, so it is a common path, not a rare edge case. The shader is already dumped:
`D:\PS5\shader-dumps\CS_e17349e0d437757b.{rdna2,ir.txt,spv,cfg.txt}` (dump more with
`KYTY_DUMP_SHADER_HASH`). Opcode mix, ~1500 instructions:
```
150 V_MUL_F32   94 V_MAC_F32   77 V_MAD_F32   50 V_MAD_MIXLO_F16   44 V_MAD_MIXHI_F16
37 V_ADD_F32   26 V_SUB_F32   25 V_MIN_F32   23 V_MAX_F32   18 V_RCP_F32
15 V_MIN3_F32  14 V_MAX3_F32   8 V_CVT_PKRTZ_F16_F32   6 V_PK_MAX_F16
```

**Already checked and NOT the bug — do not re-audit these:**
- `V_MAD_MIXLO/HI_F16` source selection. `ReadMixF32` (`Translate.cpp`) implements the ISA rule
  correctly: `op_sel_hi[i]=1` -> fp16 (half chosen by `op_sel[i]`), `op_sel_hi[i]=0` -> fp32.
- MAD_MIX with **inline-constant** sources, which is what this shader uses almost everywhere
  (`1.000000.opsel(lo=0,hi=1)`). It looks like the `op_sel_hi=1` path would bit-extract the low
  half of `0x3F800000` and yield 0.0, but `ReadF16LaneAsF32` special-cases
  `FloatInlineConstant` (Translate.cpp:488) and round-trips the fp32 value through f16, so `1.0`
  stays `1.0`. `packed` is false on this path, so the `use_zero` branch does not apply.
- Half-preservation on the destination. `Write16Bits` sets `sdwa_dst_unused=2` (preserve), and
  `DecodeVop3p` sets `dst.sdwa_sel` 4 for MIXLO / 5 for MIXHI.

The 18 `V_RCP_F32` remain the most likely NaN origin (a reciprocal of 0 gives Inf, and `Inf-Inf`
or `0*Inf` downstream gives NaN). **Suggested approach: bisect empirically rather than by reading.**
The clear-trigger makes this cheap - clear the history, then A/B a candidate change and watch
whether the NaN fraction returns within two frames.

**Tooling added this session — all opt-in, default inert, and it removes the restart tax.**
A restart costs a full manual re-navigation into a fight, which was the dominant cost of every
measurement here. Both watch lists are now reloadable at runtime, refreshed once per frame from
`DumpUfcSurfaces` (before its dump check, so it stays live on non-dump frames):
- `D:/PS5/dumps/TRACE_ADDRS` — watched addresses, seeded from `KYTY_TRACE_RESOURCES`
- `D:/PS5/dumps/CAPTURE_HASHES` — shader hashes for full input capture, seeded from `KYTY_CAPTURE_INPUTS_HASH`
- `D:/PS5/dumps/DUMP_NOW` / `DUMP_INPUTS` — existing file triggers; dumps can be taken without the user pressing anything
Both lists are lock-free fixed arrays, not a vector behind a mutex: `TraceResourceAddress` runs on
every binding of every draw. `CaptureShaderInputs` is called per draw, so its hash check must not
copy a set.
- The `ResourceTrace` dedupe key excludes the frame, so each distinct binding logs once with its
  first sighting. Ids churn per frame, so the key never collapses and the budget is really "how
  many traced frames fit" — it was 4096 and was fully spent in the menus before the round; now 200k.

### SUPERSEDED (session 7 refutes the conclusion, by per-frame id comparison): THE BLACK ROUND IS AN IMAGE ALIAS AT 0x1162c00000 (2026-09-12, session 6)

**Two different cache images live at guest address `0x1162c00000`. The final scene pass writes the
1600x900 one; the presenter reads the 400x225 one, which is empty.**

Isolated with the live scene-draw stepper (F2 capture / F3 next / F4 toggle, `sceneDrawDebug.*`).
Only **three** draws write that address, all fullscreen triangles (`idx=3`, no depth test or write),
sharing one vertex shader:

```
id=0 vs=0x85ef53ced24014e7 ps=0x487864de9705a169 target=400x225   img=3366
id=1 vs=0x85ef53ced24014e7 ps=0x5855add5c464dd25 target=400x225   img=3366
id=2 vs=0x85ef53ced24014e7 ps=0x708f302a5ca890f0 target=1600x900  img=2239   <-- the scene
VideoOut dump: rt62c00000 extent=400x225 nonzero=0/90000                     <-- what is presented
```
Suppressing id=2 replaces the black with raw uninitialised memory, so id=2 is what produces the
final image; it also costs ~214us/draw, i.e. it is doing real work, not a no-op. Image ids differ
between captures (images get recreated) but the shape is stable: two extents, one address.

**The upstream chain is HEALTHY - this retires the occlusion-CS theory for the black round:**
```
0x1163770000  (CS buffer 9)  1068x600   nonzero 640,445/640,800  = 99.9%
0x116d300000                 1600x900   nonzero 1,440,000/1,440,000 = 100%
0x1162c00000  (final)        400x225    nonzero 0/90,000 = 0%
```
Session 5 concluded the round "directly consumes output from the skipped wave64 CS" and that wave64
Stage 2 was therefore a correctness requirement. **The CS's buffer 9 is 99.9% full and the
intermediate is 100% full** - the data is alive right up to the last step. Whatever wave64 is needed
for, it is not this.

**REFINED, AND THE FIRST FIX DIRECTION WAS WRONG.** `0x1162c00000` is not "the scene address" - it
is a **recycled scratch address** the game binds as at least five different resources in one frame:
```
CS buffer[4]   read-only   stride=4 records=182400            (raw buffer)
CS texture[0]  33x33x33    fmt=50 type=10                     (3D colour-grading LUT)
CS texture[0]  1920x1080   fmt=22
CS texture[0]  1920x1080   fmt=36  (read-only AND read-write)
CS texture[0]  1600x900    fmt=36                             (the scene)
draw targets   1600x900 and 400x225                           (the three fullscreen passes)
```
So the cache holds a *family* of images at one address and every consumer gets whichever one
lookup returns. `FindImageFromRange`'s scoring (+4 size, +8 gpu-modified, +16 render-target,
+32 RGBA8) cannot distinguish a 33-cubed LUT from a 1600x900 scene, and the two scene-sized twins
**tie at 24, so the winner is iteration order.** The fix is descriptor-exact resolution
(extent + format + tile), not "prefer the bigger/newer twin".

**THE PRESENTER IS INNOCENT - do not chase it.** `Presenter::PrepareFrame`'s `consider()` rejects
any candidate under 1280x720, so it cannot pick the 400x225 twin, and its `0x1162c00000` fallback
only runs when `!native_scanout`. In-fight the scanout IS GPU-written, so the presenter never
consults that address; it presents the scanout (0x111a800000), which carries only the HUD (8.6%).

**STILL UNPROVEN - this is the next measurement.** That the aliases exist and that resolution
between them is arbitrary is established. That the *composite* reads the wrong one is NOT. The
scanout is written by a compute dispatch binding `CS texture[3] read-write @0x111a800000
1920x1080 fmt=50`, whose inputs include `0x1162c00000`. **Log which ImageId that dispatch binds
for its 0x1162c00000 input and compare it against the 1600x900 scene image.** Same-id means the
alias is not the bug and the composite is failing for another reason.

**Tooling added (uncommitted):** `sceneDrawDebug.{h,cpp}` - lock-free per-draw id assignment, a
one-frame armed capture (F2 twice), single-draw stepping (F3/F5), suppression toggle (F4), select
all (F6), and Skip vs Hide (F10; Hide disables colour writes only, via the `eColorWriteEnableEXT`
dynamic state the pipeline already declares, so depth behaviour is preserved). `KYTY_CENSUS_TARGET`
picks the address to bisect, `KYTY_DUMP_SURFACES` the addresses to dump. **Capture logs the full
list to the log file** - relying on the console lost one result to a closed window.
- **Do not take a lock per draw here.** The first version did, which on a target carrying thousands
  of draws put a mutex on the GPU worker thread's hot path; ids are now atomic and the mutex is
  only taken during the single armed frame.
- F7 is unavailable: it is intercepted early in `ProcessEvent` for mouse-to-joystick.

### SUPERSEDED (see the alias finding above): THE BLACK ROUND DIRECTLY CONSUMES OUTPUT FROM THE SKIPPED WAVE64 CS (2026-09-11 session 5)

Targeted input capture finally connects the black round to compute shader
`0xea0aceac518ec52d` without relying on a guessed render-target address. In actual-round frame 1073,
the skipped dispatch declares:

```
CS buffer[9]:  read-write addr=0x1163770000 stride=4 records=4718592
CS buffer[10]: read-write addr=0x116de10000 stride=4 records=9437184
```

Immediately afterward, fullscreen PS `0x654607b31fe5f6d6` binds `0x1163770000` as image 1, a
1600x900 B10G11R11_UFLOAT input, and writes `0x116d300000`. The captured input and downstream output
are visually the same nearly-solid-white image and have the **same SHA256**:
`83C35FE2C2F3B0FD457AA06F2E2BB0F614AC2B6524538DFECC1D9231F1269BA4`.
The next passes read `0x116d300000`, produce/read `0x1164650000`, and finally produce the black
1600x900 `0x1162c00000` image under the live HUD.

This restores a concrete justification for wave64 Stage 2: the game directly consumes a large buffer
that this CS should write, but `KYTY_SKIP_CS_HASH` prevents those writes. This does **not** make the old
constant-zero experiment a solution; zeroing the two output buffers stayed black and corrupted the HUD.
The required result is the real structured CS output, not an arbitrary fill value.

**FAILED, REVERTED — `lane_count=1` with an LDS atomic-spin bridge (2026-09-12).** The opt-in implementation
mapped one GCN lane to one invocation and paired two native wave32 subgroups through per-wave LDS slots.
It initialized the arrival counter at uniform function entry, published ballot/shuffle data, and tested
both (a) one elected lane announcing then spinning and (b) all 32 lanes announcing and spinning together.
`spirv-val` accepted the modules. A split-tagged 64-lane initialization-only GPU test passed exactly, but
a minimal shader containing one cross-half `V_READLANE_B32` timed out and left its output buffer untouched
under both rendezvous variants; the fuller lane/LDS test did the same. This is the Vulkan subgroup
forward-progress hole in practice, not an uninitialized counter. The code and temporary tests were removed.
Do not retry a spin-wait bridge between subgroups. Viable directions are optimising the existing two-half
lowering, obtaining a true 64-wide subgroup on capable hardware, or a faithful hash-specific replacement.

The same run also establishes that the 400x225 `0x1162c00000` view is intentionally bound as image 2
by PS `0x811633d84dc57d64`; generic `FindImageFromRange` selecting it is not by itself an alias bug.
For any future alias claim, trace the actual consumer binding as `CaptureBinding` does.

### WITHDRAWN: THE FIGHT HDR TARGET IS FROZEN; ALIASING RULED OUT (2026-09-11 session 4)

**Withdrawal:** `WatchedDrawTarget img=1356` was captured during the walkout/faceoff, while the later
`DumpResolve img=1356` was taken in the actual round after that target's last write. The equality was
temporally mismatched and therefore could not decide aliasing or prove that round draws still targeted
`0x1168360000`. A large-target census shows that address retires at the transition; steady-state round
frames use a different fullscreen chain. Depth and stencil forced off for all large round targets did
not restore the scene.

The actual fight-round A/B test resolves candidate **(B)**:

```
WatchedDrawTarget: ... addr=0x0000001168360000 ... img=1356 mip=0 layer=0
DumpResolve: rt68360000 addr=0x0000001168360000 img=1356
```

The ubershader draws and the dump refer to the same cache image, mip, and layer. This rules out the
`FindImage` stale-twin hypothesis. On-demand dumps at frames 2826 and 3050 were byte-identical for
`rt68360000` (`SHA256 17A88E17E7EB95BF43A55FC9EE0B992E326EFA047ADF3AC4C29C478586DED5E6`),
while `display` and `present` had different hashes across those frames and matched each other within
each frame. The user's visible blue/cyan top/right edge changes too. Presentation is live; the HDR scene
target is not.

**Conclusion:** the old cage/walkout image is retained in `0x1168360000`, but fight-round draws stop
changing it despite being issued against it. Next distinguish depth rejection, shader discard/no
rasterized fragments, and ineffective colour output/blend. Do not call this a compositor/present bug.
Do not infer that the skipped occlusion CS is or is not responsible until one of those mechanisms is
measured.

### WITHDRAWN: THE BLACK ROUND IS A COMPOSITE/PRESENT BUG. THE SCENE RENDERS FINE. (2026-09-11 session 3)

**The fight scene is fully rendered every frame and never reaches the scanout.**

```
frame 1703, in-fight, screen is black:
  rt68360000  0x1168360000  1920x1080 fmt=122 (B10G11R11_UFLOAT)  nonzero 2,044,948/2,073,600 = 98.6%
  present     0x111a800000  1920x1080                              nonzero   177,654/2,073,600 =  8.6%  (HUD only)
```
Brightness distribution of that HDR buffer (8.5% black, 28% 1-3, 22% 4-15, **37% 16-63**, 4% 64-191,
0.2% 192+) matches a known-good scene, and an ASCII render of it shows obvious structure - a lit figure,
floor, gradients. It is a real image, not noise.

**HOW THIS WAS MISSED FOR DAYS, AND THE LESSON.** `swapchain.cpp`'s surface dump used a **hardcoded
address list** that never contained `0x1168360000`; it contained the neighbours `0x1168270000` and
`0x1168260000`. So every in-fight dump found only unrelated buffers, and this ledger's
*"scene RT `0x1162c00000` collapses to 400x225 empty"* was measuring a **quarter-res buffer that reuses
that address** (1600x900 / 4 = 400x225). That one observation was the entire evidence for "the stubbed
occlusion CS starves the fight", and it was an artifact of looking at the wrong surface.
**A hardcoded list of addresses in a diagnostic is a trap: it silently answers a different question.**
`KYTY_DUMP_SURFACES=<hex>[,...]` now overrides it, and the in-fight targets are in the default list.

**How to find the right surfaces:** `WatchedDrawTarget` (`renderDraw.cpp`, rate-limited to 48 lines, keyed
off the shared `IsWatchedDrawPixelShader` watch list) logs the bound colour attachments for the ubershader
draws. In-fight they render to `0x1168360000` (1920x1080) and `0x1166f40000` (1600x904), both fmt=122.

**WHAT THIS INVALIDATES:**
- **The black round is NOT blocked on the occlusion CS, therefore NOT on wave64 Stage 2.** Stage 2's whole
  justification was "the only way to make the round render". That is now false.
- The ubershaders are not doing expensive work for nothing - they are drawing **the actual scene**.
- The buffer-zeroing experiment below was aimed at the wrong layer entirely.

**NEXT: why does the game's tonemap/composite pass not get `0x1168360000` into the scanout?** That is in
the presentation path - code this fork has already worked in - not in wave64 emulation.

### FAILED, AND IT CLOSES OPTION (a) FOR THE BLACK ROUND: zeroing the occlusion CS's two big buffers

`KYTY_STUB_CLEAR_BUFFERS=1` (`renderCompute.cpp`, default OFF, kept only so this is not retried).
Extends the existing stub — which already clears the CS's written *images* to 0 — to also zero its
written *buffers*, i.e. `0x1163920000` (18 MB) and `0x1170776e00` (36 MB).

**Hypothesis, and why it looked good:** the image clear uses "0 = nothing occludes, reverse-Z far
plane", and that is exactly what made the intro and corner scenes render. The long-standing objection
to filling the buffers — "sizes don't reveal the layout; filling blind risks a GPU hang from a wrong
value in an indirect-args buffer" — had been weakened by measurement this session: `0x1163920000` is
bound as **`CS texture[3/4/6]`, read-only sampled, 1068x600** (a Hi-Z pyramid consumed by *other*
compute shaders, not draw args), and the whole frame issues only **~21 `IT_DRAW_INDIRECT`**.

**Result: refuted, and a regression.** `cleared_images=2 cleared_buffers=2` over 450 firings, so it
did clear exactly the two intended buffers. The fight round **stayed black**, and the **HUD became
corrupted** (health bars scrambled; clean with the flag off). The walkout still rendered.

**What this establishes:** those buffers carry structured state the rest of the frame consumes, not a
visibility mask that a constant satisfies. Zero is not an "all visible" value, and nothing suggests
another single constant would be. **Option (a) in "Next steps" — extend the stub to fill the buffers —
is closed.** The black round needs the real CS running, i.e. wave64 Stage 2, or a hand-written
replacement of the occlusion algorithm.
- Cost note if anything like this is tried again: the fill ran per stub firing, 54 MB x 450 = ~24 GB
  of `vkCmdFillBuffer` in one session. Any future experiment here should fill once, not per dispatch.

### Upstream sync — what was hand-woven in, and the trap next to it (2026-09-11)

**Policy: do NOT merge or cherry-pick from upstream. Hand-apply what is relevant.** Our branch has
diverged across **46 files that upstream also changed** — `ShaderCFG.cpp` (ours +995/-26 vs theirs
+48/-87, refactors that *delete* code we build on), `spirvEmitterMemory.cpp` (+104/-111 rewritten),
`ShaderRecompilerComputeTests.cpp` (theirs +1278/-101). A rebase is a large, risky change and there is
no reason to take it while the 5 fps is this fresh.

**Woven in (commit `aad25ff`), all measured NEUTRAL:**
- Wave-wide `VCCZ`/`EXECZ` **operand** reads (`Translator::MaskIsZero`). They test whether the whole
  mask word is zero, not the current lane's bit. Upstream fixed one site (`1af19ea`, `ReadRawU32`);
  **our tree had the same bug in the U1 operand path too**, so both are fixed.
- `S_ORN2_SAVEEXEC_B32` (upstream `32ea086`) and `S_WQM_B32` (`c354657`) — we had B64 only.
- Neutral confirmed by fair A/B, same build, validation off, comparable scenes (2,439 vs 2,536
  draws/frame): **76.2 vs 79.0 us/draw**, buckets within noise. Expected: `unsupported=0` before and
  after means no UFC5 shader used either opcode, and `MaskIsZero` only fires on a *data operand* read.
- **Method note: an earlier reading of 90.3 us/draw for these was `--shader-validation` plus a 47%
  heavier scene.** Always A/B with the same flags and check `draws/frame` before believing a delta.

**DO NOT "FIX" `AddBranchCondition` (`Translate.cpp` ~line 801).** It branches `S_CBRANCH_EXECZ`/`VCCZ`
on the invocation-local EXEC/VCC boolean, which looks like exactly the per-lane bug fixed above. It is
**deliberate** — the comment above it says so: Kyty models each lane as its own invocation, so branching
on the lane's own bit lets inactive invocations leave the region without reconstructing a host-subgroup
mask. Upstream's merged fix does not touch this site either; PR #470, which would, is **open and
conflicting**. Changing it is a semantic change to the hottest control-flow path and needs its own
measurement. This was nearly "fixed" on the assumption it was the same bug.

**Upstream work worth revisiting later (inspected 2026-09-11, none pulled):**
- Wave64/EXEC: merged `c913951` (RDNA2 subvector loop mask/branch semantics); open **#470** (model EXEC
  and VCC as subgroup ballots) — the right foundation for Stage 2, but conflicts with our Stage 1
  `ThreadBit` constant-folding. Upstream's `ThreadBit` is the wave-wide `(word >> lane) & 1` form using
  `program.wave_size`; ours folds compile-time-constant masks using `current_wave_size`. **Ours is an
  optimisation that belongs on top of theirs**, not an alternative.
- **Duplicated work:** upstream `e04007a` implements `DS_INC_RTN_U32`/`DS_DEC_RTN_U32`, which this fork
  implemented independently across 5 files. Theirs ships 228 lines of tests. Adopting theirs and
  dropping ours would cut divergence in files that already conflict.
- Perf PRs in our exact area, unevaluated: **#506** (FPS overhead in GPU scheduling and resource
  caching), **#473** (query host readability once per region during the SRT walk), **#562** (BDA upload
  from CPU-dirty hints), **#537** (publish linear storage images before CPU access).
- Possibly relevant to open correctness bugs: `a305a6c`/`fde550e`/`23e7df2` (shared texture channel
  layouts and **swizzle decoding** — hypotheses 2 and 3 for the skin corruption, issue 4b);
  `0b4e78c` + PR **#545** (DB_RENDER_OVERRIDE depth/stencil copies, stencil sync — our depth-alias bugs).
- **DO NOT PULL #483** (skip GPU sync when unmapping non-GPU memory): already tried here, **livelocked**
  the guest CPU on a stale value. #484 (SRT scratch reuse) is already in the tree as `a1fc490`.

## FPS profiling (2026-09-10) — read this before more perf work

`FrameProfile:` now carries `process_ms` (whole `GuestGpu::Process(submission)` on the GPU worker thread — the single thread that does PM4 decode + Vulkan record + stalls + GC), `drawprep_ms` (`RenderExecutor::DrawIndex/DrawAuto` incl. pipeline lookup + `AcquireRenderTargets` + descriptor resolve + `hw_check`; nests `draw_ms`), `gc_ms`, `faultbuf_ms`, `flush_ms`, `sendcmd_ms`, `cp_rest` (= `process_ms` − drawprep − dispatch − submit − gc − flush − sendcmd = **raw PM4 decode + SET_*_REG / WAIT / EVENT handlers + per-`Process()` boilerplate**), and `process`/`gc`/`faultbuf` counts. `FlushAndWait` now counts as `Finish`.

**GPU worker thread is 100% saturated** (`process_ms/frame` ≈ wall) both on menu and in-match — the bottleneck is CPU-side command processing, not GPU or guest-thread idle.

After the EOP-wait skip (`aa794cd`), remaining in-match cost (heavy close-up scene, ~8-9k draws/frame, ~1.9 fps): **`drawprep_ms` ≈ 400 ms/frame is now the wall** (30 µs/draw prep × 8-9k), then `draw_ms` ~120, `dispatch_ms` ~40, `finish_ms` ~25, `cp_rest` ~45, `gc_ms` ~4. A second in-match regime (~7.4k draws, ~1.1 fps) still shows `finish_ms` ~450 ms/frame — residual **cross-queue** WAIT_REG_MEM (async-compute label, graphics waits) that the intra-buffer skip can't touch.

Menu (~33 fps now): `drawprep_ms` ~11 ms/frame, `cp_rest` ~5, rest small.

### FPS work log (2026-09-10, continued)

- **`aa794cd` intra-command-buffer EOP-wait skip.** `WAIT_REG_MEM` on a label an EOP earlier in the *same* command buffer writes no longer does `BufferFlushAndWait()` (submit + full CPU-blocking GPU idle); GPU command order already serialises it. **Menu ~24 → ~33 fps.** In-match: removed one wall, revealed the next.
- **`e46defd` cross-queue EOP-wait skip.** Pending-label map moved from per-CommandProcessor to file-static (shared across graphics + async-compute CPs). A graphics `WAIT_REG_MEM` on an async-compute EOP label: `PopPendingOperations()` to drain retired submissions, re-test the actual guest value; if the write landed, skip. **One in-match scene ~1.1 → ~2.1 fps.** `cp_rest` ~435 → ~50 ms/frame; the stall cost relocated into `gc_ms`/`finish_ms`.
- **`0811b71` cached hot-path getenv/config polls** (`GraphicsRunDebugDumpEnabled`, `graphics_debug_dump_enabled`, `EnvListContainsHash`). Neutral in-fight, small menu help.
- **`hw_check` — measured ~0 ms.** The `drawprep_ms` cost is `getprog_ms` (`PrepareProgram`: shader-map lookup + heap alloc + resource-decl parse, VS+PS) ~100-130 ms/frame in-match. A "same as last draw" memo (register-struct + context memcmp) was **0.02 % hit rate in a fight** (per-object data goes through shader SGPR user_data) — reverted. Menu-only, dropped.
- **THE REMAINING WALL: synchronous `bufdl` buffer-download `Finish`.** `SchedFinish/200: bufdl=200/2740ms` = 100 % buffer-download stalls. ~17/frame, ~13.6 ms each (full GPU idle to drain a buffer's producing work before the CPU copies it back). Guest CPU reading GPU-computed data (a recurring 10 MB buffer at `0x113d000000` ×105, a 4-byte counter cluster `0x1140008204` ×397, etc.). **~230-550 ms/frame.**

### 2026-09-10 late session — root-caused the device losses, and the readback wall fell

**`431672f` USE-AFTER-FREE ON GC'd BUFFERS — this was causing every `ErrorDeviceLost`.**
`BufferCache::RunGarbageCollector()` retired dirty buffers with an immediate
`m_slot_buffers.erase(id)` (destroying the `VkBuffer`) directly after
`DownloadBufferMemory()` had recorded GPU->staging copies against those same buffers into
the still-recording command buffer. Validation says it exactly:
`vkCmdPipelineBarrier(): ... VkBuffer 0x... was destroyed` /
`VUID-vkCmdPipelineBarrier-commandBuffer-recording`, then the submit returns
`ErrorDeviceLost`. `DeleteBuffer()` already retires via `CommandScheduler::DeferOperation`;
the GC path just did not use it. Fixed by doing the same.
- 5 device losses across 6 sessions before; **0 "was destroyed" errors and no device loss after.**
- **Pre-existing upstream bug, not caused by this session's work.** The synchronous readback
  path masked it: `Finish()` drained and `BeginNext()` opened a fresh command buffer before
  the erase. `KYTY_DEFER_READBACK` removes that accidental drain, so it became frequent.
  **This fix is a prerequisite for deferred readback being safe.**
- **Method note: three hypotheses were wrong** (`DiscardMemory`/unmap-discard, deferred
  readback itself, the stubbed CS) — each disproven by experiment. `--vulkan-validation true`
  found it in one run. For intermittent device loss, go to validation immediately.

**`KYTY_DEFER_READBACK=all` — the `bufdl` wall is gone.** `finish_ms` **450 -> 8-20 ms/frame**
(56x); `finish` count 24/frame -> ~10. Contradicts the shadPS4 prior above (prefetch fails)
because this is not a prefetch: the GPU->staging copy still rides the demand fault, only the
staging->guest writeback is deferred a frame.
- **But fps only moved 2.20 -> 2.40 at ~9k draws/frame (+8%).** The time relocated, it did not
  vanish. Quote the fps, not the `finish_ms` ratio.
- Not yet default. Needs `431672f` first (it does).

**Corrected frame budget (in-match, ~8k draws/frame, ~2.4 fps, deferral on):**
`drawprep_ms` ~300 ms/frame is the wall. Per draw: **getprog/materialize 17.2 us (45%)**,
draw recording 13.7 us (36%), rtresolve 1.4, other 5.5. `finish` now ~2% of frame.
- **Earlier "80% CPU / readback only 24%" claim was from a light 2.4 fps scene and was wrong
  for a real fight** (there it was 62% readback). Always sample a real fight frame.
- `gc_ms` is NOT a lever — `GcSplit/512: fault=0.0 dlimg=0.1 texgc=0.0 bufgc=0.0`. The
  apparent 192 ms was `Finish` nested inside GC being double-counted.

**`MaterializeResources` — measured to the sub-phase.** It re-walks the SRT graph from guest
memory on *every* draw even on a full program-cache hit. `ProgLookup/8192` shows the rest of
the cache-hit path is free: `key=0.1ms materialize=117ms permscan=0.0ms`.
`Materialize/8192: snapshot=15us specialize=0.4us avgbuf=7 avgimg=1 avgsmp=0.4` — the cost is
fixed per call, NOT proportional to the ~8 bound resources.
`SrtEval/8192: setup=0.15us cfg=2.22us sources=7.89us srtreads=4.55us | avg_cfg=42 avg_srtreads=64 avg_srcs=8.1`
- **CFG BFS (15%) is prunable** — walks all ~42 blocks every draw.
- **`srt_reads` (31%) is NOT prunable by dependency closure** (the idea suggested externally):
  `flattened_srt` is uploaded as a GPU buffer and indexed *dynamically* by the shader
  (`spirvEmitterMemory.cpp:1129`), so the host cannot know statically which entries are read.
  **Safe variant instead:** shaders with no `IR::DescriptorBindingKind::FlattenedSrt` binding
  never read that buffer — for those the whole 4.5 us table build is dead. Needs a per-entry
  flag in `ProgramCache::SourceEntry`; measure what fraction of UFC5 shaders qualify first.
- `sources` (54%) is irreducible — that IS the bound-descriptor evaluation.

**Instrumentation added this session** (all gated behind `--printf-direction`, cheap counters):
`FinishSplit/200` (submit/gpuwait/post — proved 91% of `Finish` is real GPU execution, not
bookkeeping), `GcSplit/512`, `ProgLookup/8192`, `Materialize/8192`, `SrtEval/8192`,
`SchedFlushAndWait/200`. Keep these; they are what made the above findable.

**GOTCHA THAT COST TWO BOOTS: `LOGF` goes nowhere by default.** `printf_direction` defaults to
`Silent` and `graphics_debug_dump_enabled()` keys off it, so every counter is silently
disabled. **Always launch with `--printf-direction File --printf-output-file <path>`.**
Shell-redirecting stdout only captures `::printf`, not `LOGF`, and is block-buffered.

Still uncommitted / unproven: `DiscardMemory` + unmap fast-path (`bufferCache`,
`gpuResourceManager`) — suspected of the crashes, disproven, and never shown to help. Lean
toward reverting.

Logs: `KytyLog-PPSA03541-valid.txt` (validation caught the UAF), `-uaffix-val.txt` (clean
after fix), `-deferall.txt` / `-uaffix.txt` (deferral), `-srteval.txt`, `-finsplit.txt`.

### 2026-09-11 — transfer-queue readback landed; parallelism plan and its blocker

**`847e20f` READBACK NOW RUNS ON A DEDICATED TRANSFER QUEUE.** The GPU->staging copy used to
be recorded into the graphics command buffer, so waiting for it drained every draw queued
ahead of it (`FinishSplit` proved 91% of a `Finish()` was `m_master.Wait` - real GPU
execution, ~8ms each, ~24/frame). Now:
- device creation enumerates *all* queue families (it used to `return` on the first match, so
  nothing else was visible). RTX 3070 exposes family 1 `{Transfer|SparseBinding}` = a real DMA
  engine; falls back to a second queue of the universal family, then to the old path.
- buffers become `SharingMode::eConcurrent` across the two families (one place:
  `Buffer::Buffer`), avoiding ownership-transfer barriers.
- `Buffer::NoteGpuWrite(CurrentTick())` in `ObtainBuffer(is_written)` gives each buffer its
  producer tick.
- `CommandScheduler::SubmitTransferReadback` waits on the *graphics timeline at the producer
  tick*, signals its own timeline; the CPU waits only on that.
- **`finish_ms` 443 -> 35 ms/frame, `finish` 47 -> 10 per 2 frames, WITHOUT deferral** - so
  guest memory stays current, no 1-frame staleness. `XferReadback` shows a **100% hit rate**:
  the producing submission had essentially always been submitted already.
- Opt-in behind `KYTY_XFER_QUEUE=1`; every failure path falls back. Not default yet.
- **Design note: neither half works alone.** Producer-tick waiting alone is useless (single
  in-order queue - the copy is recorded *after* the draws, so you cannot wait for it without
  waiting for them). A transfer queue alone is useless (it would still wait on "all graphics
  work so far"). Only the pair works.
- Graphics corruption in-match is unchanged by this (confirmed by the user) - it is the
  pre-existing issue, not a readback regression.

**`b8e0bdd` per-phase counters** - `Pm4Ops/262144`, `ProgLookup/8192`, `Materialize/8192`,
`SrtEval/8192`, `DrawPhase/8192` (+ `GcSplit`, `FinishSplit`, `SchedFlushAndWait`,
`XferReadback`). Attribution by subtraction (`cp_rest`) sent this work down two wrong paths;
measure directly.

**Measured in-match budget, ~2.7k draws/frame, transfer queue on: 184 us of host CPU PER
DRAW.**

| bucket | ms/frame | us/draw |
|---|---|---|
| draw recording | 144 | 53 |
| getprog (MaterializeResources) | 96 | 35 |
| cp_rest (PM4 decode + reg handlers) | 117 | 43 |
| dispatch | 60 | 22 |
| finish (readback) | 35 | 13 |
| gc + rest | 48 | 18 |

`DrawPhase/8192: bindings=12.3us vbuf=1.2us rendertargets=1.4us pipeline=0.6us` - vertex
buffers, render targets and the pipeline-cache lookup are all fine. Descriptor work dominates:
~12us in `PrepareGraphicsBindings` plus ~38us after it (`CommitBindings`/`CommitVertexBuffers`).
**Descriptors ~50us + materialize ~35us = 46% of all CPU time, both recomputed from scratch
every draw.**

`MaterializeResources` mechanism (confirmed, still unfixed): every **4-byte** descriptor dword
read goes through `TryReadGpuCleanBacking`, which calls `TextureCache::IsRegionGpuModified`
(takes a lock, runs `FindImagesInRegion`, allocates a vector) and `HasGpuDirtyBytes`. ~48 such
reads per draw per stage. **A per-read page memo was tried and REVERTED** - it regressed
(12.2 -> 19.4us on identical menu shaders), because memoising inside the per-read callback pays
TLS + scan costs 48x to avoid a cost that should be *hoisted out* of the read path. The right
fix is one clean/dirty check per descriptor range, or use the existing
`read_specialization_block` batch reader. That is a `SrtWalker` change, not a `memory.cpp` one.

**THE BLOCKER FOR ANY PARALLELISM: `BufferCache` has ZERO synchronisation.** Not one mutex. It
is safe only because exactly one thread touches it, and it owns the page table, LRU, memory
tracker and GPU-dirty range sets - every draw hits it several times (`ObtainBuffer`,
`FindBuffer`, `SynchronizeBuffer`). By contrast `TextureCache` has `TrackingSpinLock m_lock`,
`PipelineCache`/`SamplerCache` have mutexes, and the CP context globals
(`g_current_processor`, `g_current_execution`, `g_gpu_thread`) are already `thread_local`.

`process_ms` has equalled wall time in **every** sample, menu and in-match: one GPU worker
thread does all PM4 decode, draw prep and Vulkan recording; 7 cores idle; the GPU is mostly
idle waiting for work. The GPU is not the saturated resource - the CPU thread feeding it is.

**Parallelism plan, and why it is ordered this way:**
0. **Reduce per-draw work first** (descriptor reuse, materialize). ~46% of CPU, no
   architectural risk, and it shrinks whatever has to be parallelised later. Parallelising
   redundant work just burns 8 cores on the same waste.
1. Async-compute queue on its own thread - **not free**, compute dispatches bind buffers too,
   so it also needs a thread-safe `BufferCache`.
2. Pipeline decode vs record - **gains little**: the decode thread must keep all cache access,
   so the record thread only gets the cheap `vkCmd*` calls.
3. Parallel draw preparation into secondary command buffers - **where the 5x is, prerequisite
   is thread-safe `BufferCache`**, the largest and riskiest change in the hot path. Every draw
   touches it several times, so it may end up contention-bound well under the theoretical 8x.

Realistic ceiling: eliminating *both* known hot spots entirely is ~3 fps, not 10. 10 fps needs
stage 3.

### 2026-09-11 (later) — fourth failure, and the tools that were there all along

4. **Per-slot flat cache in the evaluator** (`EvaluateSlot`: evaluate an SRT slot once, then
   read it back by index - the direct analogue of shadPS4's `flattened_ud_buf`/`ReadUdSharp`).
   **Regression: `Materialize snapshot` 15-16 -> 19.5-24.4us, `SrtEval sources` 7.9 -> 9.7-13.5us.**
   Reverted.
   **Why:** `EvaluatorScratch::cache` already memoises by `Inst*`, and a `ReadConst` node *is*
   an `Inst`, so a repeat hits `m_cache.find` and returns before reaching the slot cache. It
   only helps when *different* `ReadConst` nodes share a slot, which is rare here. Added bounds
   checks + stamp compare + store to a path that already short-circuited.

**Four optimisation attempts, four failures, one repeated error: assuming a mechanism is
expensive without confirming the branch is hot.** Three of the four were already
short-circuiting or already memoised. Do not add another cache to this function without a
profile first.

**TOOLS THAT WERE ALREADY IN THE TREE AND WENT UNUSED:**
- **Tracy is vendored and built** (`3rdparty/tracy`, `_Build/windows/3rdparty/tracy`), the hot
  functions already carry `KYTY_PROFILER_FUNCTION()` (`PrepareBindings`, `FindBuffers`,
  `RebindBuffers`, `CommitBindings`), and `--profiler-direction Network` starts the client.
  **Use this before writing another counter.** Needs the Tracy server GUI to connect.
- **Xbyak AND Zydis are already fetched** (`3rdparty/CMakeLists.txt`, `FetchContent_MakeAvailable(xbyak zydis)`)
  and **Kyty already JITs x86 with Xbyak in `src/loader/redZonePatcher.cpp`**
  (`Xbyak::CodeGenerator patch_gen/trampoline_gen`, plus `<Zydis/Zydis.h>` for decoding).
  An earlier note in this ledger said Xbyak was not vendored - **that was wrong**. Both
  dependencies shadPS4's SRT walker needs are present, with a working in-tree example of
  emitting and installing generated code. The JIT port is therefore much smaller than assumed;
  shadPS4's `flatten_extended_userdata_pass.cpp` is 328 lines to follow.
  - Note their JIT chases raw pointer chains that can fault, handled by a signal handler that
    decodes the faulting insn with Zydis and self-patches `mov rdi,[rdi+off]` -> `xor rdi,rdi`.
    On Windows that is a vectored exception handler. Kyty's interpreter gets this free via
    `TryReadBacking` returning false.

Suggested order from here: **profile with Tracy first**, then port the walker JIT if the
profile confirms interpretation overhead.

### 2026-09-11 (session 2) — the real in-fight wall is ONE PM4 OPCODE, and the CPU/GPU split

**`cp_rest` is not "PM4 decode + reg handlers". In a heavy fight it is `IT_DISPATCH_INDIRECT` (op 0x16).**
`Pm4Ops` in-fight: `op0x16=2314-3323ms / 136-204 calls` = **11-22 ms PER CALL**, ~30 calls/frame =
**~420-545 ms/frame, ~45-50% of the frame** - more than all ~3,500 draws combined (op0x27 ≈ 345 ms/frame).
It is invisible to every bucket: not `Finish`, not `dispatch`, not `gc`, so it lands in `cp_rest`.

**Mechanism, proved not assumed.** `CommandProcessor::DispatchIndirect` (`graphicsRun.cpp:1272`)
dereferences the GPU-written arg block straight out of guest memory. New `IndirectArgs/128` counter
times *only* that dereference: `total=1666ms slow=17 slow_total=1663ms` - **17 of 128 reads block, ~98 ms
each**; the other 111 cost ~27 us total. Per-call average matches `op0x16` exactly (13 vs 14.2 ms), so
the dereference IS the opcode cost. `EnsureCurrentForCpu` is a no-op (deferred readback off) and
`DispatchDirect`'s unscoped prologue (`CheckBuffer`) is innocent - both were candidates, both cleared.

**CPU or GPU? Both, ~50/50, and serialised.** `FinishSplit/200: submit=7.7ms gpuwait=2392.1ms post=674.3ms`
- **78% of a blocking Finish is real GPU execution.** Frame at ~1,100 ms: **~630 ms CPU busy / ~470 ms
blocked on GPU**. `process_ms ≈ wall` was read as "CPU-bound, GPU idle" - wrong: a thread blocked in
`vkWaitSemaphore` is still inside `Process()`. Both sides are ~50-100x too slow (a 3070 should do 3,500
draws + 500 dispatches in <10 ms). **Consequence: the entire per-draw track (SRT JIT, bda, descriptors)
has a hard floor at the GPU's ~470 ms ≈ 2.1 fps.** Past that it must be GPU-side, i.e. wave64→wave32.

**Transfer-queue A/B (same fight, same binary).** `op0x16` per call 13.8-14.2 (on) vs 11.3-13.6 (off) -
**the transfer queue does NOT cause or amplify this stall**; an early hypothesis that `847e20f` was
responsible was wrong. What it *does* do is confirmed strongly: `finish` 25→4/frame, `finish_ms`
603→5, `gc_ms` 100→24. The indirect-arg stall is immune because its producer is the compute shader
that just ran (the µs lead-time pattern); the wait is genuine GPU execution, not scheduling slack.
With the queue off the stall appears as `finish_ms`, with it on as `cp_rest`. Same cost, different column.

**`PrepareBda` - the `findbuffers` bucket was mismeasured.** The `BindPhase` timer at
`descriptors.cpp:1075` spans `FindBuffers` ×2 **plus `PrepareBda()`**, which walks every mapped range ×
every buffer in the cache (`ForEachUploadRange` = spinlock + bitmap scan per tracker region) on every
draw whose shader has `uses_dma`. New `BdaSplit` counter: in-fight `bda=8-9us/draw, bda_draws≈1000/8192`
(12% of draws, ~75 us/call) = **~33 ms/frame, ~2%**; menu ~1 ms/call but ~0.5 calls/frame. Real but small.
`findbuffers` proper is **0.7-0.9 us**, `clamp` ~0.15 us/call - so `ClampRangeSize`'s global kernel
mutex and `FindBuffer` are both innocent, contrary to the earlier note. Upstream shadPS4 `origin/main`
has the identical unguarded per-draw sweep; there is no `!fault_process_pending` guard to port.

**`vkCmdDispatchIndirect` - implemented behind `KYTY_INDIRECT_DISPATCH=1`, DEFAULT OFF, BLOCKED.**
Kyty uses **zero** Vulkan indirect commands anywhere in `host_gpu/` - every indirect draw and dispatch
is CPU-converted. shadPS4 issues `cmdbuf.dispatchIndirect` / `drawIndexedIndirect` / `drawIndirectCount`
(`vk_rasterizer.cpp:1364-1377,1497`) and never reads the args. Cache buffers already carry
`eIndirectBuffer` usage (`streamBuffer.h:33`, `bufferCache.cpp:779`) and the existing post-dispatch
`ShaderAccessBarrier` (dst `eMemoryRead` at `eAllCommands`) already covers `eIndirectCommandRead`, so
neither staging nor a new barrier is needed. Legal only when `mode & 0x20` (USE_THREAD_DIMENSIONS) is
clear - that bit folds the counts into the compute shader's specialization key.
- **BLOCKER: the host still binds descriptors for dispatches that turn out to be zero-group.** The
  zero-check (`renderCompute.cpp:579`) currently skips binding entirely for GPU-driven passes that
  resolve to "nothing to do"; the indirect path cannot know. CS `0x45cde74926a779f8` logs
  `groups=0x1x1` (**thread_group_x = 0**) and is skipped for free today. Going indirect binds its 15
  textures and dies: `invalid image view: image_format=130 view_format=43 aspect=0x1` - a **D32_SFLOAT_S8_UINT
  image sampled through an R8G8B8A8_SRGB descriptor**. Pre-existing latent bug, same family as issue #3
  but on the compute *sampling* path, only ever masked by that lucky early-out.
- Narrowing the gate to "only when the read would actually stall"
  (`HasGpuDirtyBytes(args_addr, 12)`, ~12-14% of calls carrying ~99% of the cost) **did not help** -
  that dispatch's args are GPU-dirty too. Kept anyway: strictly better than the broad gate.
- **FAILED FIX, REVERTED: `ResolveDepthOverlap` Texture rule.** Adding
  `recreate |= cached.info.IsDepth() && !requested.IsDepth()` (the rule Storage/RenderTarget/VideoOut
  already have) fixes the view crash but replaces images that are simultaneously the draw's bound depth
  target → `depth target changed after render-state discovery` (`renderDraw.cpp:553`) on the next draw.
  Texture bindings legitimately alias the live depth target - that is what depth-feedback support is for.
  Comment left in place at the site so it is not retried.
- **Next idea, untested:** promote per-CS-hash - first indirect dispatch of a hash takes the direct path
  and records whether its counts were non-zero; only hashes known to do real work go indirect. Would
  spare `0x45cde74926a779f8` (zero both times it appears per run). Still a heuristic; a
  sometimes-zero shader would reopen the hole. The principled fix is the depth-alias-sampling bug.

**Instrumentation added (keep):** `IndirectArgs/128`, `IndirectEligible/128`, `BdaSplit/8192`
(bda / bda_draws / clamp / lookup / descriptors-per-draw), `SrtShape/8192`. Cheap counters, gated like the rest.

**WAVE64 IS NOT THE PERFORMANCE CEILING - MEASURED, AND IT OVERTURNS THIS LEDGER'S ASSUMPTION.**
`GpuDispatchTime` (real GPU timestamps, `KYTY_GPU_TIMESTAMPS=1`, two per dispatch) in-fight:
`total=13.9-47.6ms over 281-954 dispatches | wave64=3.4-40.6ms (24-85%)`.
**Total GPU compute is 14-48 ms/frame in a ~1,000 ms frame = 1.4-4.8%. Wave64 compute is 0.3-4%.**
Stage 2 is worth **at most ~4%**, not the ceiling. It remains the fix for the **black fight round**
(correctness, unaffected) - do it for that, but it is the *weakest* remaining perf lever, not the
strongest. The earlier "~470 ms/frame of GPU time, wave64 is the ceiling" reasoning in this ledger was
inference from CPU wait times plus the 62% wave64 *invocation* share, and it was wrong.
- **METHOD WARNING that nearly shipped a 96% figure.** The first implementation wrote ONE timestamp per
  dispatch and diffed consecutive ones. That measures **gap + execution**: GPU idle waiting for the CPU
  gets charged to whichever dispatch preceded it, and since >90% of dispatches are wave64, wave64
  collected the idle by construction. It reported `total=1078ms, wave64=96%` **for a frame that takes
  550-620 ms** - the frame-time contradiction is the only thing that caught it. Always bracket each
  region with its own begin/end pair, and always sanity-check a GPU total against wall time.
- Implementation: `GpuDispatchTimer` in `renderCompute.cpp`, `KYTY_GPU_TIMESTAMPS=1`, default off.
  Uses `hostQueryReset` (added to `RequiredVulkan12Features`) so the pool is reset from the host only
  while nothing references it - avoids ordering `vkCmdResetQueryPool` against the command buffers the
  scheduler opens and closes mid-frame, which is the lifetime-bug class that caused the device losses.
  Results polled without the WAIT bit; a frame that is not ready simply retries.

**CONFIRMED WITH EXACT SIZING: 3 SHADERS OF 648 OWN ~79% OF THE FRAME.**
Watched-shader brackets (always on for the 3 ubershader hashes, not sampled):
```
graphics=847.1ms of 924.3ms total GPU busy
ps=0x1bcc68ffb7b0469e  719.33ms over 34 draws (21156.7us EACH)
ps=0xb808b3887f76ff0c    6.72ms over  2 draws ( 3360.8us each)
ps=0x0000000000000000    0.32ms over 344 draws (   0.9us each)   <- healthy
```
A single draw was measured at **408.70 ms**. Strategy census the same run: **645 legacy, 3 dispatch**.
Ubershaders = 726 ms of 847 ms graphics = **86% of graphics, ~79% of the whole frame, from 0.5% of
the shaders.** Menu control: 6 us/draw.

**MECHANISM, read straight out of `PS_1bcc68ffb7b0469e.cfg.txt`:**
```
block_1  condition=dispatch_done  merge=412  continue=411  loop_header=1
block_2  successors=[3..163,410]  indirect_targets=[3..163]  selector_values=[3..163]
```
i.e. `loop { switch (state) { 161 cases } if (dispatch_done) break; }` - every pixel runs a 161-case
switch inside a loop, one iteration per basic block visited. SPIR-V expansion **5,432 GCN dwords ->
166,690 words = 30.7x**. Three compounding costs, the third being the 5,000x:
1. no structured control flow - the driver cannot unroll or reconverge;
2. register pressure - every value live across a state transition survives the switch, so occupancy
   collapses and there is no latency hiding;
3. **divergence never reconverges** - lanes sit in different states, so each iteration the wave
   executes the union of every case any lane is in. With 161 cases that is an algorithmic blowup in
   wave utilisation, not a constant-factor codegen loss.

**THE FIX IS NOT "improve DispatcherFull"** - the dispatcher is inherently this shape. It is to stop
these three shaders needing it. This ledger already records exactly why Legacy fails on them:
`block %82 exits the selection headed by %80, but not via a structured exit` - a fall-through block
jumping to a grandparent selection merge, past the discard-pad merges of two enclosing selections.
**One specific edge pattern**, and Legacy already has related machinery (shared-merge splits, clone
retry for external selection tails / the 16->177 shape). Teach it to handle a fall-through targeting a
grandparent merge (clone the tail, or thread it through the intervening merges), with
`ValidateStructuredExits` + `spirv-val` as the correctness gate and `tests/shaderCfgTests.cpp` as the
harness. **`ValidateStructuredExits` stays** - it is what stops the invalid-SPIR-V GPU hangs; we are
removing the *need* for the fallback, not the safety net.

**THE CFG TEST SUITE IS GREEN AGAIN, AND IT ALREADY CONTAINS THE UBERSHADER.**
`shader_cfg_tests` loads `_Shaders\cfg_fail\PS_1bcc68ffb7b0469e.bin` (the real 5,432-word shader),
runs BOTH strategies and prints the failure reason:
`UFC PS strategy compare: legacy=0 blocks=193 reason=block 18 exits the construct headed by 16 to 19
without a structured exit (merge 192) | dispatch=1 blocks=413 cases=161 synthetic=252`.
**The whole problem is iterable in-process in seconds - no emulator, no navigating into a fight.**
That was the dominant cost on this all day. `TestUfc5PsDispatcherFullOracle`, tests/shaderCfgTests.cpp.
- Fixed two **stale** tests that pinned `mode=dispatcher` for patterns legacy now handles, so they
  failed as an *improvement* and (harness aborts on first failure) hid every later test:
  `...NestedLoopNonlocalExitDispatcher` and the mixed continue/nonmerge case. Both now assert the
  property that matters - **valid SPIR-V** - plus mode/control-flow consistency.
  The other three `mode=dispatcher` assertions were LEFT ALONE: irreducible CFGs and `S_SETPC_B64`
  jump tables genuinely cannot be structured, so pinning the dispatcher there stays correct.
- Learned: **a structured module may legitimately contain `OpSwitch`** - `RouteSharedSelectionArm`
  introduces routing variables that lower to one. "structured implies switch-free" is false.
- **TODO: make the harness continue after a failed `Check` and report all failures.** It aborts on the
  first one, so a regression elsewhere hides the UFC result you are trying to measure.

**THE CFG SHAPE AT THE FAILURE (dumped from the FAILED graph, not a fresh BuildGraph - the ids in the
reason refer to the post-split 193-block graph; 177 and 192 are synthetic and absent from the
161-block original):**
```
block_16  preds=[11]      succs=[17,192]  cond: true=192 false=17   doms=[0,11,16]
block_17  preds=[16]      succs=[18,191]  cond: true=191 false=18   doms=[0,11,16,17]
block_18  preds=[17]      succs=[19]                                doms=[0,11,16,17,18]
block_19  preds=[15,18]   succs=[20,57]                             doms=[0,11,19]  <- NOT dom by 16
block_192 preds=[16,191]  succs=[]        Return
```
Block 16 is `if (c) return; else ...`. `FindSelectionMerge` returns the nearest common post-dominator,
which is **192, the function's return block**, stretching the construct to the function exit. Block 19
is a join shared with block 15 (reached under block 11 by a different path), so `18 -> 19` is an exit
from 16's construct that is neither the merge nor a loop break/continue.

**FAILED ATTEMPT 6, REVERTED - widen the terminal-merge gate.** `FindSelectionMerge` ALREADY has the
right special cases, commented *"a return can leave a selection without reaching its merge"*
(ShaderCFG.cpp:1334-1343), but they are gated on `global_merge == UINT32_MAX` so they never fire when
a merge IS found and simply *is* a return block. **That gate at :1325 is the gap.** Widening it to also
fire when the merge block is terminal (`successors.empty()`) **regressed a phi test**
("consecutive typed Phis were not emitted as two native OpPhi instructions") - merge placement and phi
emission are coupled - and the harness aborted before reaching the UFC oracle, so it is still unknown
whether it fixes the ubershader. Reverted; suite green.
- **The real constraint: a fix must move the merge WITHOUT disturbing phi placement.** That likely
  means choosing the tighter merge *only* when the wider one would produce an illegal exit -
  information `FindSelectionMerge` does not currently have, since it runs before
  `ValidateStructuredExits`. Consider computing the exit check first, or retrying with a tighter
  merge only for constructs the validator rejects.

**THE EXACT STRUCTURIZER FAILURE (same for all 3 ubershaders):**
```
blocks=193 loops=12 failure=StructuredControlFlow block=18
reason=block 18 exits the construct headed by 16 to 19 without a structured exit (merge 192)
first_pass=selection header block 13 has externally entered region block 177
           preds=[13,15,16] merge=14 true=177 false=14
```
`FindSelectionMerge(16)` returns **192** of 193 blocks - the arms only reconverge at the function end -
and block 18 inside that construct branches to 19, which is neither the merge nor dominated by 16.

**FAILED ATTEMPT 5, REVERTED - dominance-based external-entry test.** `SplitOneSelectionMerge` repairs
regions defined by **reachability** (`SelectionRegion`, ShaderCFG.cpp:1571 - walks successors from the
arms to the merge) while `ValidateStructuredExits` validates regions defined by **dominance**
(`DominatedBlocks`, :1050). Those are not equivalent, so "no external entry into the reachable region"
does not imply "no unstructured exit from the dominated region". Added `!graph.Dominates(block_id,
member)` to the external-entry test at :1811 so non-dominated members get a private clone.
**Result: the 3 ubershaders STILL routed to dispatcher (`578 legacy, 3 dispatch`), AND it caused
`ErrorDeviceLost` on a COMPUTE dispatch** (`debug_op=0 args=32768,1,1,97`) - the extra cloning
miscompiled some *other* shader into invalid SPIR-V. Reverted; menu back to 30 fps.
- The inconsistency is real, but it is **not** what blocks these shaders. The blocker is upstream:
  either `SplitExternalSelectionEntry` declines this shape, or the 16->19 exit survives cloning.
- **`ValidateStructuredExits` did not catch the bad module it produced** - the validator is necessary
  but not sufficient. **Run any structurizer experiment with `--shader-validation true`** so spirv-val
  names the offending shader at compile time instead of learning about it from a device loss.
- **The CFG test suite is RED and cannot gate this work.** `shader_cfg_tests` fails at
  `TestNewShaderRecompilerCfgNestedLoopNonlocalExitDispatcher` ("did not select dispatcher fallback") -
  a **stale** test asserting the dispatcher should win a pattern Legacy now handles. Pre-existing
  (confirmed via `git status`), and the harness stops at the first failure so everything after it is
  unverified. **Fix that test first** - no structurizer change is safe without this net.
- Next attempt needs the actual CFG shape, not the error string: dump the pre-structurization graph
  around blocks 13-19, 16, 19, 177, 192 and trace what `SplitExternalSelectionEntry` decides for each.

**Superseded first-pass numbers (1-in-16 sampling, kept for the method lesson):**
`GpuDraws` (sampled 1-in-16 draw brackets, `KYTY_GPU_TIMESTAMP_DRAWS`):
`sampled=483 every 16 | total=13.35ms avg=27.6us max=11.18ms`, and by pixel shader:
```
ps=0x1bcc68ffb7b0469e  11.58ms over   2 sampled draws (5788.2us each)
ps=0xb808b3887f76ff0c   0.42ms over   2 sampled draws ( 212.5us each)
ps=0x0000000000000000   0.37ms over 369 sampled draws (   1.0us each)   <- depth/shadow, healthy
```
Another frame recorded a **single draw at 37.47 ms**. Menu control: 6 us/draw.
**Both top entries are the übershaders this ledger already names** (`0x1bcc68ffb7b0469e`,
`0xb808b3887f76ff0c`, `0xf7030726b9470dd8` - 5,400+ GCN words, 161 blocks, 12 loops). The
distribution is NOT "all draws are slow": 369/483 sampled draws cost 1.0 us. It is a handful of
draws at ~5,000x the cost of a normal one.

**The chain, end to end:**
```
3 übershaders fail Legacy structurization (invalid SPIR-V -> real GPU hangs)
  -> ValidateStructuredExits routes them to DispatcherFull
    -> dispatcher codegen = a state-machine loop, per pixel, at 1080p
      -> 5.8-37 ms per draw
        -> graphics 513-746 ms/frame, GPU saturated
          -> the CPU's indirect-arg read blocks ~98 ms -> op0x16 ~50% of frame
```
`ValidateStructuredExits` was correct and necessary - it stopped real hangs. It traded a hang for a
5,000x slowdown on three shaders, and nothing measured the second half of that trade until now.
**The fix is to make Legacy structurization handle these three shapes** (or to improve DispatcherFull
codegen) - not any of the CPU-side levers. This outranks everything else in this ledger.
- Sizing caveat: per-draw costs are measured directly and solid; the *total* share is noisy because
  1-in-16 sampling on draws this rare extrapolates to anywhere from 214 ms to 1,284 ms per frame.
  **Next: always bracket draws binding those three hashes** instead of blind sampling.
- Also refuted here: "no culling -> overdraw" does not fit. Overdraw would raise cost uniformly;
  369/483 sampled draws are 1 us. And the stubbed CS makes the round *black*, i.e. draws render
  nothing - which should be fast, not slow.

**ANSWERED: THE `op0x16` STALL IS THE CPU WAITING FOR A SATURATED GPU, AND THE GPU IS BUSY DRAWING.**
`GpuBusy` brackets whole command buffers (draws included), not just dispatches:
`cmdbuffers=958.1ms over 391 buffers | dispatches=79.4ms over 1362 (wave64=71.4ms) | graphics=878.7ms`
(range across scenes: 90 -> 958 ms, matching the two regimes).
- **Graphics is 92% of GPU time; compute 8%; wave64 7.5%.**
- GPU busy **958 ms in a ~920 ms frame**, and `process_ms` is 911 ms/frame: **CPU and GPU are BOTH
  ~100% saturated at the same time.** Neither is "the" bottleneck; the `op0x16` stall is simply the CPU
  blocking on a GPU that is genuinely flat out.
- **~236 us of GPU time PER DRAW** (878 ms / 3,728 draws). A 3070 should retire a draw in single-digit
  us - this is 50-100x off and is now **the largest single item in the system**, larger than `op0x16`
  (which is mostly this seen from the CPU side), larger than `drawprep`.
- Candidate causes, compounding: (1) the **3 pixel übershaders** (5,400+ GCN words, 161 blocks, 12 loops)
  that fail Legacy structurization and fall back to **`DispatcherFull`** - a state-machine loop, which
  is catastrophic codegen for a per-pixel shader at 1080p; (2) **no culling** - the occlusion CS is
  stubbed so everything is drawn, i.e. overdraw through those shaders.
- **Next: attribute graphics GPU time per pipeline/shader** the same way dispatches were attributed
  (`GpuTimestamps::BeginDispatch/EndDispatch` generalises), and check what fraction of draws bind a
  dispatcher-structurized übershader. That names the target instead of guessing between (1) and (2).

**Two collector bugs that produced silence rather than errors** (both fixed, both worth remembering):
dropping still-open regions at the window boundary left permanently unwritten slots, and
`getQueryPoolResults` returns `eNotReady` for the **entire range** if any single query is unavailable -
so the collector wedged in Reading forever and printed nothing at all. Fixed with
`eWithAvailability` (skip holes per query) plus a poll limit so one straggler cannot stall a window.

**SUPERSEDED: what is the `op0x16` stall actually waiting for?** Frame ~1,000 ms, `process_ms` ~1,001 (worker
100% busy), `cp_rest` 502 ms (50%), `drawprep` 368 ms (37%), dispatch recording 93 ms, rest 55 ms. If GPU
compute is only ~48 ms, the ~450 ms the CPU blocks on when reading indirect args is **not** the compute
that produces them. Candidates: (1) **graphics work** - the readback drains the whole queue and the
timestamps cover dispatches ONLY; 3,700 draws through the 5,400-GCN-word pixel übershaders at 1080p are
unmeasured; (2) CPU-side cost in the fault/download path, not GPU wait at all; (3) queue/semaphore
stalls. **Next measurement: bracket whole command buffers with two timestamps** for total GPU busy
including draws; subtracting the compute figure isolates graphics. Cheap, same machinery.

**DO NOT PORT shadPS4's SRT JIT. Build a flat program in plain C++ instead.** `SrtReads/8192` splits
the walk into interpreter dispatch vs the guest reads themselves:
`SrtEval setup=0.23 cfg=0.44 sources=11.76 srtreads=1.57` (14.0us/call) vs
`SrtReads guest_read=0.55us/call reads=22.2/call ns_per_read=25`.
**The guest reads are 3.9% of the walk; 96% is interpreter dispatch.** A flat/bytecode rewrite keeps
`read_memory` and addresses the 96%. An Xbyak walker would additionally save 3.9% - **not worth a
vectored exception handler, Zydis decoding and self-patching generated code.** No Xbyak, no Zydis, no
fault machinery, no platform-specific code.
- **Why shadPS4 needed the JIT and we do not:** their walker is *already* a minimal pointer chase, so
  their residual cost genuinely is the reads (hence codegen + signal handler). Kyty's cost is a
  tree-walking interpreter layered on top. Different bottleneck; porting their fix buys ~4%.
- `avg_srtreads=22.0` vs `reads=22.2/call`: ~one guest read per SRT read and **none** from the
  `sources` evaluation - yet `sources` is 11.76us, the largest bucket. Descriptor-source evaluation
  touches no memory at all; it is pure graph walking plus `unordered_map` cache hits over values
  already resolved. The purest possible case for linearisation.
- **Design:** at ResourcePlan build time (`ResourceMaterialization.cpp:954`, where `srt_reads` and
  `descriptor_sources` are cloned) linearise the value DAG into a topologically ordered op array -
  one op per `Inst`, operands as slot indices - then per draw evaluate ops in order into a reused
  `slots` scratch. Kills traversal, hash lookups and `Arg()` recursion; memoisation falls out for free
  since each Inst appears once. Bail to the interpreter for `Phi`, `ReadFirstLane`, `ReadConstBuffer`,
  any clean_flat_slot, and anything outside the ~18 hot opcodes (98% of traffic - see SrtOps census).
  Gate behind an env flag and run both paths comparing results to prove equivalence on real workloads.
- **Validation harness now exists:** `resource_materialization_tests`, `resource_tracking_tests` and
  `scalar_provenance_tests` could not link (undefined `Common::Timer::QueryPerformanceCounter`,
  `Log::Write`) because they compile `SrtWalker.cpp` - which carries the profiling counters - but only
  linked `fmt::fmt`. Added `common` to all three. They are `EXCLUDE_FROM_ALL`, so this had been broken
  invisibly since the counters landed. `resource_materialization_tests` passes and validates this work
  without a navigation cycle.
- **Pre-existing failure now visible:** `resource_tracking_tests` fails with
  `shader resource specialization failed: storage image descriptor 1 has an invalid mip range`.
  Not caused by this session (only `SrtWalker.cpp` counters were touched among that target's sources).
  Possibly the same bug as issue #5's `vkCreateImage mipLevels exceeds extent-derived max`, with a unit
  test already written for it.

**WAVE CENSUS - `op0x16` AND WAVE64 ARE THE SAME PROBLEM FROM TWO ENDS.** `WaveCensus/16f` in-fight:
`wave64=539 disp/f (22.7M inv/f) wave32=120 disp/f (14.0M inv/f) share=61.8%`; menu `82 disp/f, 24M inv/f`.
So **~660 compute dispatches and ~36.7M invocations per frame in a fight, 62% wave64-emulated** - wave64
is in play for the compute that actually runs, not just the skipped occlusion CS. 36.7M invocations
should be single-digit ms on a 3070; heavy scenes spend ~450 ms/frame waiting on compute. ~100x off.
**The thing the CP blocks on in `op0x16` IS compute output** (the indirect dispatch args): slow wave64
compute → args arrive late → the host read of the args blocks ~98 ms → `op0x16` ≈ 50% of the heavy
frame. `vkCmdDispatchIndirect` stops the CPU *waiting*; wave64 lowering makes the GPU *finish sooner*.
They compound rather than compete.
- Caveat: 62% of *invocations* is a prior, not proof the GPU time is inside those shaders (could be the
  wave32 passes or the pixel übershaders). **GPU timestamp queries would name them - do that before Stage 2.**
- Also measured: the census sits after the early-outs and sees ~112 real dispatches/frame where
  `FrameProfile` counts ~606, so **~80% of dispatches never reach a pipeline** (meta-clear, image-clear,
  UI-mask fallback, zero-size). `dispatch_ms` ~96 ms/frame is spent by the ~112, not the 606.
- Counter gotcha: the first version logged every 64 frames. At ~1 fps that is over a minute, so in-fight
  lines still showed menu data and looked like a stuck counter. Now 16 frames, frame number stamped.
- **Two regimes confirmed, and they matter for prioritising.** ~2.1 fps scenes: CPU-bound, `op0x16` 7-18%,
  `finish_ms` ~18 ms/f. ~1.0 fps scenes: `cp_rest`/`op0x16` ~50%, i.e. **the indirect-dispatch fix matters
  most exactly where the framerate is worst.** With the transfer queue on, that GPU wait hides inside
  `op0x16`/`cp_rest`, not `finish_ms` - do not read a small `finish_ms` as "the GPU is keeping up".

**THE COST IS SPREAD - there is no single wall left.** Measured frame, 2.07 fps, 483 ms, ~2,500 draws:

| bucket | ms/frame | share |
|---|---|---|
| draw recording (`draw_ms`) | 101 | 21% |
| `cp_rest` (incl. `op0x16` ~42 in this scene) | 88 | 18% |
| `getprog` / materialize | 67 | 14% |
| dispatch | 46 | 10% |
| finish (GPU wait) | 29 | 6% |
| rtresolve / gc / submit / flush / present | 25 | 5% |

**No CPU lever exceeds ~21%.** `op0x16` was ~48% in one heavy scene but ~7-18% in others - it is spiky,
not universal. This explains the whole history in this ledger: every single optimisation has returned
~10% because **~10% is the size of the individual items.** Stacking all known CPU levers (SRT JIT,
op0x16, PrepareBda, descriptor reuse) lands near 2x total ≈ 3 fps, matching the earlier estimate.
A bigger multiple must change the *shape*: fewer draws (the occlusion CS is stubbed, so nothing is
culled), the GPU side (wave64 lowering), or parallelism (deprioritised - shadPS4 does this on one core).
The first two are both Option A.

**SRT walker JIT - scoped and GREEN, sized at ~10%.** `SrtShape/8192` in-fight:
`cfg=1890 clean=0 jittable=6302` = **77% compilable in-fight, 96% in menu**, and **`clean=0` always** -
the dual clean/specialization evaluator, one of the two things making Kyty's walker harder to compile
than shadPS4's, never executes. Sizing: materialize is 67 ms/frame, `sources+srtreads` = 86% of it,
× 77% coverage = **~44 ms/frame → 2.07 → ~2.28 fps**. Real, steady, present in every scene, but not
transformative; it costs a code generator. Insertion points:
| shadPS4 | Kyty |
|---|---|
| `GenerateSrtProgram` (compile-time x86 emit) | `BuildSrtPlan(Program&)` - `SrtWalker.cpp:1155` |
| `Info::RefreshFlatBuf()` (per draw, one native call) | `EvaluateRuntimeSourcesImpl` - `SrtWalker.cpp:1027` |
| `ReadUdSharp` (array index) | `flattened_srt` - already built, not used as the read path |
Xbyak + Zydis are fetched in `3rdparty/CMakeLists.txt`; `src/loader/redZonePatcher.cpp` already emits
and installs generated x86 with `Xbyak::CodeGenerator` - a working in-tree example of the mechanic.
**Correction to the earlier note: this is NOT a 328-line copy.** That is shadPS4's pass against *their*
IR; Kyty's `SrtWalker.cpp` is 1,214 lines with CFG-conditional source activation they do not have.
Shape: JIT the simple case, keep the interpreter as fallback for the 23%.
Also measured: `Materialize/8192 snapshot=16.5us specialize=0.5us` - `BuildResourceSpecialization` is
free, it is all the walk. `SrtEval cfg=1.53us` (13%) is not worth chasing even if halved (~1% of frame).

### 2026-09-11 — what the per-draw cost is NOT (three failed optimisations)

All three were measured and reverted. Recorded so they are not retried.

1. **Skip the flattened-SRT build for shaders with no `FlattenedSrt` binding.** Safe and
   correct, but `FlatSkip/8192` showed only **4.4%** of draws qualify - ~0.2us of a 15us call.
   Not worth the plumbing. (The broader "prune srt_reads by dependency closure" idea is
   *unsound*: `flattened_srt` is a GPU buffer the shader indexes **dynamically**
   (`spirvEmitterMemory.cpp:1129`), so the host cannot know statically which entries are read.)

2. **Memoise the GPU-clean check per page in `thread_local` storage** (inside
   `TryReadGpuCleanBacking`). Regression: identical menu shader mix went **12.2 -> 19.4us**.

3. **Same memo, but on the stack**, passed via `SrtRuntime::userdata` (already plumbed, was
   unused) to kill the TLS cost. Still a regression: **15-16 -> 16.9-20.7us**, `SrtEval
   sources` **7.9 -> 9.3-10.6us**.

**Why 2 and 3 both failed - the hypothesis was wrong, not the implementation.**
`TryReadGpuCleanBacking` only enters its expensive branch (texture-cache lock +
`FindImagesInRegion` + vector alloc) when `IsGpuAddressRange(vaddr, size)` is true. Shader
descriptor tables mostly live in ordinary guest memory, so that branch rarely runs and the
probe was already near-free. Both caches added a scan + store to an already-cheap path.
**Check whether a branch is actually taken before optimising it.**

**By elimination, the ~35us of `MaterializeResources` is the IR-graph interpretation itself,
not memory access**: `SrtEval` splits it as sources ~7.9us + srtreads ~4.6us + CFG walk ~2.2us
per call, x2 stages per draw. That is ~250ns per evaluated node - i.e. the cost of a
tree-walking interpreter. It cannot be cached away, because the inputs genuinely change every
draw (same reason the `GetGraphicsPrograms` memo hit 0.02%).

**The only approach with evidence behind it is shadPS4's: compile the walk instead of
interpreting it.** `flatten_extended_userdata_pass.cpp` emits x86 with Xbyak
(`static Xbyak::CodeGenerator g_srt_codegen(32_MB)`, `RegisterWalkerCode`) once per shader;
per draw `Info::RefreshFlatBuf()` makes **one** native call to flatten the SRT, after which
every descriptor read is `ReadUdSharp` = a plain array index. Kyty already *computes*
`flattened_srt` per draw but does not use it as the read path.

Suggested order: **(B)** restructure so the existing `flattened_srt` is the read path for
descriptor sources (no JIT, tests the premise cheaply), then **(A)** vendor Xbyak (header-only,
not currently in `3rdparty/`) and emit a native walker in `BuildSrtPlan`.

The same "per-resource cache lookup per draw" shape is the other two costs, from
`BindPhase/8192`: `ResolveTexture` **10.7us** (texture-cache lock + lookup per image) and
`FindBuffers` **9.6us** (`ClampRangeSize` + `FindBuffer` per buffer); samplers 0.4, shaderdata
0.4, rebind 1.9, commit 3.1. shadPS4 does all of this from a flat buffer + push descriptors
(Kyty already uses push descriptors where the pipeline supports them - no gap there).

**shadPS4 has NO threaded rasterizer** (no `std::thread` in `vk_rasterizer.cpp`) and reaches
30-58 fps on UFC4. A comparable emulator does this job on one core, so Kyty's problem is
constant factor, not parallelism - **deprioritise the `BufferCache` thread-safety project.**

### Readback: what the shadPS4 `ufc4-research` ledger already established (`D:\PS4\src\shadPS4`, branch `ufc4-research`)

Same engine family. Their conclusions transfer:
- **Prefetch / eager frame-boundary sweep (= `KYTY_DEFER_READBACK`'s family) does NOT work for this workload.** Lead time between GPU-marks-page and CPU-reads-it is **microseconds** — "the game reads what the GPU just produced, no window exists." Every prefetch variant delivered the broken-30fps result. Plus **amplification**: a blanket sweep moves every page the GPU *marks* vs. today only pages the CPU *reads* — multiples more data + GPU work.
- **What worked: `SHADPS4_UFC4_DEP_SUBMIT=2` — producer-aware early flush.** Stamp writable backing buffers with their scheduler producer tick; when a consumer draw/dispatch that reads *or writes* the buffer is recorded and its producer is still in the current **unsubmitted** command buffer → `EndRendering()` + `Flush()` right then. Genuine CPU read faults keep the synchronous path. The GPU work is submitted earlier → done by the time the CPU reads it → **the read doesn't fault → no synchronous download.** Result: **30 → 56-60 fps in-fight**, mostly intact geometry, occasional accepted 30 Hz dips. Became the CUSA14209 default.
  - **Fragile.** "Broad mode 2 submits far too often (~15-49k submissions/character-select). All narrower discriminators (DEPNARROW, DEPWRITESET, DEPONCE, SCANPAGE) LOST CORRECTNESS. Do not ship as a general solution." Took many failed iterations.
- Also: GETENV (one cached getenv → 43 % off draw pipeline lookup — the finding behind `0811b71`).

### FPS work log — round 3 (2026-09-10, evening)

- **`0811b71` cached hot-path getenv/config polls** — neutral in-fight, small menu help.
- **`a1fc490` ported upstream KytyPS5 PR #484** (SRT evaluator scratch reuse) — neutral on UFC5 `getprog_ms`; Kyty's per-draw shader-prep cost isn't in the SRT walker like shadPS4's. Kept as a harmless upstream port.
- **KytyPS5 PR #483 (skip GPU sync when unmapping non-GPU memory) — TRIED, LIVELOCKED.** Hit **60 fps** in-fight then the guest CPU livelocked spinning on a stale value (`handle=1 / data=0x7ec55f320` flood; `draws=0 dispatch=0`). Its author's own caveat fired: skipping `InvalidateMemory` on unmap left stale buffer-cache entries. **Reverted.** BUT it proved the point — kill the readback and it's 60 fps; the readback IS 100% of the in-match cost.
- **Owner's GC frame-retention change — TRIED, net negative, reverted.** Moved readback out of the GC (`gc_ms` 155→10) but 5×'d the per-Finish `bufdl` cost (13.7→66 ms) — delaying readbacks just concentrates them.
- **DEP_SUBMIT-style producer-aware early flush (`KYTY_DEP_SUBMIT=2`) — IMPLEMENTED, NO EFFECT ON UFC5, reverted.** Stamped every GPU-written `Buffer` with `CommandScheduler::CurrentTick()`; in `ObtainBuffer`, when a consumer (read+write, mode 2) saw its producer still in the unsubmitted command buffer → `EndRendering()`+`Flush()`. 102k flushes fired in-fight (incl. the 10 MB `0x113d000000` buffer). `finish_ms` / `bufdl` **unchanged**. Why it worked on UFC4 but not here: UFC4's producer is early-frame and the CPU read is much later, so early-submit buys a CPU/GPU overlap window; **UFC5 reads each buffer immediately after the GPU produces it** (the µs lead-time the shadPS4 ledger measured) — the flush submits the producer earlier but it's still executing when the CPU read faults. Also confirmed: **Kyty has no speculative over-reading** (no `GuestHasData`-style scan) — every one of the ~17 `bufdl`/frame is a genuine guest CPU read.

### Where the readback wall stands

`SchedFinish/200: bufdl=200/~4000ms` = 100 % buffer-download Finish, ~13-20 ms each, **~230-450 ms/frame**. It does not yield to: prefetch/defer (µs lead time), producer-early-flush (read follows write too tightly), scan removal (no scan), GC tuning (delay = concentrate). What's left is **architecture**:

1. **Async readback servicing thread** — render thread records the GPU→staging copy + a fence and moves on; a dedicated thread waits the fence and does the staging→guest memcpy. Removes the render-thread block without reducing GPU work. Multi-day, real risk, but the only remaining shape that fits UFC5's tight read-after-write pattern.
2. **Completion-gated 1-frame deferred readback** — on the CPU fault, wait only the *specific* producing tick instead of a full pipeline drain. `KYTY_DEFER_READBACK` infra is a start; shadPS4 tried 1-frame latency and got "30 fps, broken fighters" — likely a correctness dead end here too.

### Committed FPS wins this session (net)

- Menu **~22 → ~34 fps** (EOP intra-buffer skip `aa794cd` + getenv cache).
- In-match: one close-up scene **~1.1 → ~2.1** (cross-queue EOP skip `e46defd`); other scenes ~1.0-1.9. The `bufdl` readback wall is unbroken.

### Non-readback next levers (smaller)

- Draw / dispatch record time (~95 / ~55 ms/frame in-fight) — the actual Vulkan command emission.
- `getprog_ms` ~100 ms/frame — `PrepareProgram` (shader-map lookup + `ShaderGetStaticInputInfo*` parse), still uncached per draw; a shader-addr-keyed memo of the *static* input layout (not the per-draw resource snapshot) could work where the full-result memo (0.02 % hit) didn't.

Goal: get **EA Sports UFC 5** (`PPSA03541`, dump `D:\PS5\Games\UFC5`) drawing in KytyPS5. UFC-only. Real kernel / FS / GPU. Stub PSN, network, and trophies. Do not commit unless asked.

Source: `D:\PS5\src\KytyPS5`
Binary: `D:\PS5\Emulators\KytyPS5-Bin\kyty_emulator.exe`
Build: Ninja + clang-cl Release at `D:\PS5\src\KytyPS5\_Build\windows`
Helper: `D:\PS5\tools\devenv.ps1`
If devenv fails with "input line is too long", reset PATH to Machine+User and clear `INCLUDE` / `LIB` / `LIBPATH` first. Always close `kyty_emulator` before build/copy.
Incremental build is ~7 s. Deploy = copy `_Build\windows\kyty_emulator.exe` to `Emulators\KytyPS5-Bin\`.

Launch from `D:\PS5\Emulators\KytyPS5-Bin`:

```
.\kyty_emulator.exe --game "D:\PS5\Games\UFC5" --printf-direction File --printf-output-file "D:\PS5\KytyLog-PPSA03541-….txt"
```

`EXIT()` is code **321**. FPS is in `FrameProfile:` lines. Default CFG strategy is **Auto**. **F9** dumps GPU surfaces.

GPU: **NVIDIA GeForce RTX 3070**. `Vulkan subgroup: default=32 min=32 max=32 size_control=false wave64=false` — wave32-only, no way to get a 64-wide subgroup. This is the root of the hang-CS problem.

All of this work is uncommitted on `main` (`b847135-dirty`).

---

## Fixes applied

### Boot / platform

- POSIX `truncate` / `KernelTruncate` (and `KernelFtruncate` NID wiring) so the dump can size files.
- `sceNetResolverAbort`.
- NP entitlement / partner libs return signed-out instead of unresolved NIDs (`NpEntitlementAccessPft`, `NpPartner001`).
- Cap noisy `nanosleep` / equeue wait logs (first 16 only) so printf files stay usable.

### Presentation

- Swapchain / scanout / compositor path so the **title screen** actually shows in the window.
- UFC surface dumps on present; **F9** GPU dump.
- Host input: F8/F9/F11 not eaten as game keys.

### Host GPU

- Texture cache: compute image clears, DCC fill tracking, download / view work needed by UFC compute.
- Pipeline cache compile timing logs (`ShaderCompile:`, `CsPipeline:`, `GfxPipeline:`).
- Compute dispatch logging (groups, local size, buffers, textures).
- Diagnostic skip: `KYTY_SKIP_CS_HASH` (not a product fix).
- Hang-CS GDS **chunked dispatch** (hash `0xea0aceac518ec52d`): windows of `KYTY_GDS_CHUNK` (default 4) with `Finish()` between submits. **Not a playable fix.** `KYTY_GDS_LIMIT_CAP` caps items for diagnosis.
- **`FindImage` depth-as-color (2026-09-09):** when a `RenderTarget` desc resolves to a depth-associated image (`info.IsDepth()` or `depth_id` set), drop it and `InsertImage` a fresh color image instead of returning the depth image. `FindRenderTarget` fatally rejects any depth-aliased image ("color target requires rediscovery before final acquisition"), and `AcquireRenderTargets`' re-resolve guard was missing the `depth_id` case. Fixes the crash on UFC surface `0x1163740000` (a 512 KB depth surface the engine reuses as a color target). Also enriched the `FindRenderTarget` EXIT message with `registered/depth_id/needs_rebind/addr/format/tile/type`.
- **`AcquireRenderTargets` (renderDraw.cpp):** added `old_image->depth_id` to the re-resolve condition.

### Shader recompiler / SPIR-V

- **`ShaderInfoCollection` PS-input tolerance (2026-09-09):** a `GetAttribute` / `GetInterpolationParameter` that references a parameter slot `>= SPI_PS_IN_CONTROL.NUM_INTERP` no longer aborts the frame — it logs once (`PS param past NUM_INTERP`) and continues; `EmitAttribute` already yields constant 0 for an unbound slot. Still hard-fails if the index is `>= 32` (would OOB the 32-slot arrays). **Diagnostic finding:** PS `0x4c22cd8526171dbb` gets `attr=0, input_num=0` — `SPI_PS_IN_CONTROL` reads as **0** at compile time. Real bug still open (see Current issues #2).
- **`spirvEmitterFlow.cpp` null guard (2026-09-09):** `EmitInterpolationParameter` dereferenced `InputBindingForParameter(...)` without a null check (unlike `EmitAttribute`). Guarded.
- **`vulkanWindow.cpp` debug messenger (2026-09-09):** validation-layer errors are now logged as `[Vulkan][E][..][VALIDATION-ERROR]` instead of `EXIT`, so `--vulkan-validation true` runs past Kyty's own benign errors (e.g. `vkCreateFramebuffer renderPass VK_NULL_HANDLE`) and captures the full error sequence before a device-lost. DIAGNOSTIC — consider gating behind a flag before committing.
- **`textureCache.cpp` `FindTexture` packed-float alias reinterpret (2026-09-09):** the encoding-mismatch block (`!ViewEncodingCompatible && SameTexelBlockSize`) previously *always* forced `view_info.format = image.backing.format` — right for UFC's RGBA8 title compositor bound through an 11-11-10 descriptor, wrong for the reverse (GPU wrote a `B10G11R11_UFLOAT` / `E5B9G9R9_UFLOAT` HDR surface, shader binds it as `A2R10G10B10_SNORM` #98 etc.) which produced rainbow marbling on skin + garbage HUD panels. Now: when backing IS packed-float, descriptor is NOT, and `ImageViewOps::FormatsCompatible(backing, descriptor)` holds, we KEEP the descriptor format and let the mutable-format image serve a true bitcast reinterpret view. All other mismatches keep the old force-to-backing. Kill-switch: `KYTY_NO_ALIAS_REINTERPRET=1`. Log: `TextureCache: reinterpret packed-float alias: ...` vs `... sampling backing encoding: ...`.
  - **RESULT (2026-09-09, tested in-game):** fix fires as designed — `TextureCache: reinterpret packed-float alias: backing format 122 vs descriptor format 98 addr=0x0000001156990000` (the skin/probe surface), path taken ~every sample after warmup. **No regression** (menus/title unchanged) but **no visible improvement** either — rainbow skin / corruption looks identical. So a naive bitcast `B10G11R11_UFLOAT → A2R10G10B10_SNORM` view is NOT what the shader wants here; the real mismatch is elsewhere (wrong *source* image entirely, tiling/swizzle, or the surface genuinely never gets the right data written because of the stubbed occlusion CS). Fix is landed + inert; leave it (harmless, correct in principle) and look upstream next.
  - Note: a *separate* pre-existing diagnostic line `TextureCache: sampling GPU-written alias fmt=122 instead of req_fmt=98 addr=0x...1156990000` also fires at the same address from another code site — both paths touch this surface.
  - `KYTY_NO_ALIAS_REINTERPRET` getenv is now read once into a `static const bool` (was per-texture-bind per-draw).

- **Menu FPS regression — round 1 (2026-09-09):** `EnsureCurrentForCpu()` (added at the 5 indirect draw/dispatch/count sites in `graphicsRun.cpp` to protect deferred readback) unconditionally did a `ReadMemory` → `SendCommandSync` GPU round-trip + `DrainDeferredReadbacks` on **every indirect draw**. Fix: now a **no-op unless `BufferCache::DeferredReadbackEnabled()`** (`KYTY_DEFER_READBACK` set). `KYTY_DEFER_READBACK` parsing factored into cached `BufferCache::DeferMinCopyBytes()` / `DeferredReadbackEnabled()`. **Did NOT recover menu fps** (still ~21 at "press any button") — so this was real overhead but not the main cause.
- **Menu FPS regression — round 2, THE cause (2026-09-09):** `FrameProfile` on the "press any button" screen showed ~23fps / 43ms per frame but only ~13ms accounted (draw+dispatch+submit+finish+present); ~30ms/frame missing. Log analysis: **821k `cmd = 0x…` / `address = 0x…` lines + 191k `acb[N] = …` lines** — `src/libs/agc.cpp` has **~79 ungated `LOGF(...)` diagnostic dumps** (pointer/param traces), including in the hot per-frame AGC template-patch path: `AgcWaitRegMemPatchAddress`, `AgcWaitRegMemPatchReference`, `AgcQueueEndOfPipeActionPatch{Address,Data}`, `AgcCondExecPatchSet*`, `AgcJumpPatchSetTarget`, `submit_acb` (8-dword acb dump per compute submit), `AgcDriverSubmit*`, `AgcDcb/AcbDispatchIndirect`, `AgcDcbEventWrite`, `AgcDcbAcquireMem`, `AgcCbReleaseMem`, `AgcDcbCopyData`, predication setters. Frostbite pre-builds command buffers and re-patches GPU addresses **every frame** → hundreds of synchronous `LOGF`→file writes per frame on the submit thread. These are pre-existing upstream (`147188b`), not added this session — the earlier ~60fps run likely predated `--printf-direction File` being standard. **Fix: blanket `s/LOGF(/AgcTrace(/` in `agc.cpp`** (all except `AgcTrace`'s own body, line 50). `AgcTrace` already gates on `Config::GraphicsDebugDumpEnabled()` (false unless `--graphics-debug-dump true`); every `LOGF` in that file was a trace dump, none were warnings/errors. **RESULT: did NOT move menu fps** (still ~21 on the "press any button" attract screen — Kyty's logger is buffered/async, so those writes weren't on the critical path). Kept anyway (disk + log readability). The ~30 ms/frame gap is the guest thread (Frostbite genuinely rebuilds ~2700 GPU commands/frame for that real-time-lit attract screen) + ~242 vkQueueSubmit/frame. Likely the "~60 fps menu" memory was the earlier EA/Frostbite logo screens, not this one.

- **Log-spam gating round 2 (2026-09-10):** an in-match run produced a **425 MB** log (~8.7M lines). Sources, all ungated per-frame / per-audio-callback `LOGF`: `ajm.cpp` AJM decoder param dumps (~5.9M `instance = …`), `graphicsRun.cpp` `WriteReferenceClock` `copy_data reference clock` (~1.8M), `sync.cpp` `EndOfPipe Signal!!!` LOGF_COLOR (~0.9M), `audio.cpp` AudioOut param dumps (~0.96M), `faultManager.cpp` `Accessed non-GPU cached memory` (~102k). Fixes: `ajm.cpp` blanket `LOGF→AjmTrace` (new helper gated on the file's `PRINT_NAME_ENABLED`, always false there); deleted the `EndOfPipe Signal!!!` and `copy_data reference clock` debug lines; rate-limited `faultManager` to 64. `audio.cpp` left (has two `LIB_NAME` blocks, structurally awkward; 10× smaller than ajm). Built + deployed as `-quiet2`.

- **Mip-chain over-declaration clamp (2026-09-10):** the user's session added `ImageMipTrace`/`TextureMipTrace` diagnostics which caught UFC 5 creating storage mip chains **one level over the Vulkan max** (`mipLevels > floor(log2(max_dim))+1` — e.g. a 512² image asking for 11 levels, a 256² asking for 10). The extra level has undefined contents → RGB speckle when sampled. Fix (3 points): `image.cpp` ctor clamps `backing.mip_levels` to the complete chain; `imageView.cpp FindView` folds a descriptor that still points at the dropped level back into the real range (instead of `EXIT`); `image.cpp GetBarriers` clamps the transition range + uses `full_levels = min(guest_levels, backing.mip_levels)` so no barrier is emitted for a level the image no longer has (would have been device-lost). **RESULT: clamp verified firing (`… (clamped)` in log, no crash) but NO visible change** — the walkout-floor speckle is not this. It's downstream of the stubbed occlusion CS (same root as the black match / rainbow skin). Clamp kept as a correctness fix (Vulkan spec violation + latent crash class).

- **Present path — native-scanout rework (user session, 2026-09-10, uncommitted):** `swapchain.cpp PrepareFrame` no longer *always* substitutes the flip-alias / last-color / `0x1162c00000` compositor. It now presents the game's real scanout directly when `native_scanout = scanout.SafeToDownload() && (scanout.usage.storage || scanout.usage.render_target)`, keeping the old fallbacks only when that's false. **Effect: in-match the real composited frame now shows (walkout scene renders — black from stubbed CS + entrance-stage light strip + speckled floor); menu + HUD broke** (the `native_scanout` gate is too permissive on menu paths, presents the wrong image). Follow-up: also require the scanout was GPU-written *this* frame / matches flip dims, or restrict to gameplay context. Also in this session: `textureCache.cpp CopyImage` new `crosses_1d` path (1D↔2D copy via buffer, "UFC reuses a 1D R32 image as a 2D row"); `vulkanWindow.cpp` enables `dualSrcBlend` when supported.
  - **Committed as checkpoint `745d4d8`** (menu + pre-fight + walkout render, HUD present). Also folded in that commit: the mip-chain clamp, the round-2 log gating, the UFC UI-presence-mask fallback (`KYTY_UFC_UI_MASK_FALLBACK`, fills the exact 1080p mask kernel with full coverage — CMask metadata is empty because Vulkan colour draws don't write it), full dual-source-blend support (pipelineCache detect Src1* factors → `ps_dual_source_blend`; spirvEmitterModule emits Location 0 / Index 0|1).

- **VideoOut flip-buffer format pin — ROOT-CAUSE FIX (`8ee5cc8`, 2026-09-10):** the A2R/A2B scanout mismatch. UFC double-buffers the scanout; the game registers both flip buffers as A2R10G10B10 (`attribute=58`), but `TextureCache::FindImage` stamps `backing.format` from whichever binding creates the cache image first, and the paths disagree — the VideoOut resolve decodes the registered token (→ **58 A2R**), a CS storage write maps `k10_10_10_2 → A2B` unconditionally (**64**). Whichever wins the `FindImage` race labels the image; `0x111a800000` (VideoOut first) got 58, `0x111b800000` (CS storage first) got 64. Bits are identical (CS packs raw); only the label diverged, so `PrepareFrame`/`CopyFrom` misread the 64-labelled buffer. Fix: `VideoOutRegisterBuffers2` (which already computes the authoritative `ImageInfo` and discarded it) now calls new `TextureCache::RegisterVideoOutSurface(addr,size,fmt)`; `InsertImage` forces `info.pixel_format` to the registered format for any image created inside a flip range, *before* the VkImage is made → one allocation, display-format label, all binding paths agree. Mutable-format lets the CS still view it as A2B for its own stores. Removed the now-dead `swapchain.cpp` per-present `IsPacked10Unorm` `frame_format` override (the user's present-time workaround). **Verified:** `pinning VideoOut surface 0x111b800000 to registered format 58 (was 64)` once; `VideoOut present format mismatch` count ~10/session → **0**; both buffers `backing=58`; splash + menu colours correct.
  - Kept (different axis, still legitimately variable): `Frame::CopyFrom`'s `IsPacked10Unorm && IsPacked10Unorm` verbatim-copy branch (scanout backing vs *swapchain image* format).

### Wave64 lowering — Option A (in progress, 2026-09-09)

Goal: stop the wave64-on-wave32 emulation from being ~1000× too slow. Disassembly of the hang CS (`strategy=legacy`, clean structurize) showed the cost is: **1771 `OpSelect`** (exec-mask predication — every `v_cmpx` region lowered to "compute both sides, `result = active ? new : old`"), **× `lane_count == 2`** scalar lane-pair packing (`SpirvEmitter.cpp:326`), plus 279 paired subgroup ops. `OpIAdd`+`OpFAdd` only 704 — real arithmetic is modest.

- **Stage 1 (LANDED + MEASURED): fold a compile-time-uniform exec mask.** `Translator::ThreadBit` (`Translate.cpp`) returns immediate `true`/`false` when the mask is an all-ones / all-zero compile-time constant. `s_mov_b64 exec, -1` (emitted after every `v_cmpx` region) lowered to `SetExec(ThreadBit({0xFFFFFFFF,0xFFFFFFFF}))` → runtime `(0xFFFFFFFF >> laneid) & 1` chain constant-prop couldn't fold → every downstream `Select(exec,new,old)` survived. Now: SsaRewrite forwards immediate `true`, `FoldSelect` collapses them, DCE drops the old-value loads.
  - **Measured on the hang CS** (in-match dump): `OpSelect` **1771 → 1309 (−26%)**, OpPhi 1021→895, OpLoad 524→498, SPIR-V 320KB→294KB (−8%), `OpLabel` 1209→1209 (structure intact), `spirv-val` clean, menu byte-identical.
  - Remaining 1309 selects: the hang CS also restores exec via `s_mov exec, <saved-sgpr>` (×12, `s_and_saveexec`), `s_mov exec, vcc` (×9), `s_andn2 exec` (×7) — not compile-time constant, so still per-lane. Stage 1 alone ~10-15% fewer instrs — **not enough to beat the TDR**.
  - Pre-Stage-1 dump saved: `shader-dumps/CS_ea0aceac518ec52d.spv.pre-stage1`; post: `.spv` + `CS_stage1_dis.txt`.

- **Occlusion-CS clear-to-zero stub (LANDED, `renderCompute.cpp`):** when `KYTY_SKIP_CS_HASH` matches, instead of dropping the dispatch, clear the CS's read-write images to 0 ("nothing occludes" — 0 is what the real shader IMAGE_STOREs into an empty tile; reverse-Z far plane). Logs `stubbing watched compute shader ... cleared_images=N`.
  - **Result: intro + pre-round corner scenes now RENDER** (octagon, crowd, cage, lighting, cornerman+bucket — full 3D). `display nonzero` 99.8%. No crash. ~1.2-1.7 fps. **This proves the whole engine works** — CFG fix + Stage 1 + depth-alias + PS-input fixes all compounding.
  - **The actual fight round is still black.** The CS also writes two big read-write buffers — `buffer[9]` (4718592×4 = 18 MB) and `buffer[10]` (9437184×4 = 36 MB, exactly 2× buffer[9]); their addresses rotate between runs. Session 5 traced buffer 9 directly into the next 1600x900 HDR fullscreen pass, so leaving it stale is causal. It is not merely visibility/indirect data, and filling it with zero is not sufficient.
- **Stage 2 (TODO): `lane_count = 1`.** 256 GCN threads → 256 invocations (8 subgroups) instead of 128 scalar lane-pairs. Removes the `%X`/`%X+1` op duplication. Only 23 `lane_count` refs to audit (mostly `spirvEmitterProgram.cpp`/`Mesh`/`Module`); the `state.wave_size == 64` cross-lane helpers (`spirvEmitterHelpers.cpp:213`, `spirvEmitterFlow.cpp`) must bridge 2 subgroups via LDS instead of assuming pair-packing.
- **Stage 3 (TODO): native subgroup arithmetic.** Declare `GroupNonUniformArithmetic`; manual shuffle-tree reductions → `OpGroupNonUniformIAdd`/`FAdd`.

### Tooling (2026-09-09)

- **`D:\PS5\tools\drive_ufc.py`** — virtual Xbox pad via `vgamepad` + ViGEmBus (both installed). Navigates menus into a match: `python drive_ufc.py --seq "A:6, A:2, A:6, START:8, A:8"` (BUTTON:seconds-after). space→A, enter→START. Confirmed Kyty picks up the pad (`Controller axis: 0 …` in log); XInput is not focus-gated so no window juggling. Pad timing is non-deterministic — menu load times vary, so the same seq can land in different scenes (an `A,A,A,START,A` run hit a `draws=8443` scene vs the manual path's `~5084`).
- **GPU surface dump on demand:** `touch D:/PS5/dumps/DUMP_NOW` → next present dumps `display`/`present` + 9 RT surfaces as BMP to `D:/PS5/dumps/`, each with a `nonzero=X/Y` count in the log. (Also frames 1/30/90/180/360/720/1500, or F9.)

### Shader CFG

- `StructurizeWithStrategy` with Legacy vs DispatcherFull; env `KYTY_SHADER_CFG_STRATEGY` (default Auto). Auto = try Legacy, fall back to DispatcherFull `if (!success && IsStructuralPathology(graph))`.
- Shared-merge splits and **clone retry** for UFC pixel CFGs (external selection tails / 16→177 shape).
- SRT / resource tracking import onto rewritten IR.
- SPIR-V emit / SSA updates for dispatcher state.
- Tests in `tests/shaderCfgTests.cpp` plus fixtures under `tests/shaders/`.
- **`ValidateStructuredExits()` (2026-09-09):** after `StructurizeImpl` assigns merge blocks, walk every construct region (`DominatedBlocks(header, merge)`) and verify no member block's successor leaves the construct except via its own merge, or a break/continue of an *enclosing loop*. On violation → `SetFailure(FailureKind::StructuredControlFlow)` + `return false`, which routes through the existing Auto fallback to DispatcherFull for that one shader.
  - **Why:** `StructurizeImpl` assigned each selection a merge but never checked the interior edges. The 3 UFC pixel übershaders (`0x1bcc68ffb7b0469e`, `0xb808b3887f76ff0c`, `0xf7030726b9470dd8` — 5400+ GCN words, 161 blocks / 12 loops) got Legacy-structurized into **invalid SPIR-V**: `block %82 exits the selection headed by %80, but not via a structured exit` (a fall-through block jumping to a grandparent selection merge, past the discard-pad merges of two enclosing selections). NVIDIA accepted the module (validation is advisory; `--shader-validation` was off by default) and miscompiled the malformed control flow into a **GPU hang** — this was the `commandScheduler.cpp:401` / `ErrorDeviceLost` device-lost we were chasing (the EA engine wrote its own `_hang.st.dat` too).
  - **Verified:** with the fix, `3 [CFG-STRUCT-FAIL] strategy=legacy` → `3 [CFG-STRUCT] strategy=dispatch` for the übershaders, `546 [CFG-STRUCT] strategy=legacy` for everything else (no over-trigger), and `0` spirv-val failures. Run now clears the übershaders and reaches the hang CS.

### DS / GDS (hang CS family)

- Decoder + translate + SPIR-V for **wrap** `DS_INC` / `DEC` / `RSUB` (with and without return), LDS and GDS.
  - ISA: `tmp = MEM; MEM = (tmp >= DATA) ? 0 : tmp+1; return tmp`.
  - UFC encoding: `ds_inc_rtn_u32 v3, v2, v3 offset:4 gds` (`0xd88e0004 0x03000302`).
- **M0 GDS window** on regular GDS DS ops (`Translator::ApplyGdsWindow`). UFC uses size-only `m0=0xC000` (base 0).
- Host **GDS probe** on the watched hang CS: GPU copy of GDS[0..7] through the download buffer, then `Finish()`, then log `limit[0]` / `counter[1]`.
- GPU tests: `shader_recompiler_compute_tests.exe --ds-inc-only` (wrap semantics + captured UFC GDS inc + M0 base + 144-WG GDS contention).
- Wrap-INC CAS loop memory semantics (SPIR-V valid): load Acquire, CmpXchg equal AcquireRelease, unequal Acquire, then barrier.
- Diagnostic `KYTY_GDS_LIMIT_CAP` and `KYTY_GDS_CHUNK`.

---

## Current issues

### 1. Hang CS TDR (blocking) — cap=1 no longer survives

Compute shader **`hash=0xea0aceac518ec52d`**, addr `0x11409ec000`, 1612 GCN words → ~80,080 SPIR-V words. Wave64, local **16×16×1**, **144** groups. Frostbite GPU software occlusion culling: persistent threads claim 16×16 screen tiles via `ds_inc` on GDS[1] vs limit GDS[0] (~5500–5700 items), each rasterizes an occluder Hi-Z tile and `IMAGE_STORE`s to a 1600×904 target.

- **Root cause:** RTX 3070 is wave32-only. The shader is wave64. Kyty emulates wave64 as **2 GCN lanes per host invocation** (256 GCN threads → 128 invocations), every ALU op doubled, every ballot/shuffle/readlane done twice + recombined through LDS/barriers, `exec` mask emulated as data. For this ballot-saturated 6-nested-loop shader that is ~1000× too slow — **~0.67 s per 16×16 tile**.
- Full 144-group dispatch → instant TDR (`vkQueueSubmit` → `ErrorDeviceLost`).
- **Measured cleanly 2026-09-12:** `KYTY_GDS_LIMIT_CAP=1` alone still dispatches all 144 workgroups, so the old “one tile” attribution was incomplete. With new `KYTY_GDS_GROUP_CAP=1` and `KYTY_GDS_SYNC_CAP=1`, fifteen pre-round/walkout one-item calls complete in **0.8-1.0 ms** (first warm call 3.3 ms). At the user's actual-round transition the next one-item call takes **3426.0 ms** and device-lost at `debug_op=0` DispatchDirect (`0xea0aceac518ec52d`). The shader's real-round data selects a radically more expensive path, and that one work item exceeds TDR. Chunking/group-count changes cannot fix it.
- `KYTY_SKIP_CS_HASH=0xea0aceac518ec52d` (full skip) still gets past — used for fast iteration on later blockers. It is not visually correct: the round directly samples the skipped buffer-9 output as its HDR scene source.
- Dumps: `D:\PS5\shader-dumps\CS_ea0aceac518ec52d.{bin,rdna2,cfg.txt,ir.txt,spv}`

**This is now the sole hard blocker. The remaining work is the wave64→wave32 lowering rework (Option A) — see "Next steps" #1.**

**Render state with the CS skipped (corrected 2026-09-11):** `KYTY_SKIP_CS_HASH` gets past the TDR and into a match (HUD/health-bars/banner composite fine — presentation path verified good), but the **3D scene is black + a garbage RGB band**. The old `0x1162c00000` 400x225 dump was not proof; it is an intentionally consumed quarter-res view. The direct proof is frame 1073: skipped CS read-write buffer 9 at `0x1163770000` is immediately sampled as the 1600x900 HDR input to PS `0x654607b31fe5f6d6`, and its stale-white contents propagate byte-identically into the next stage. Getting a correct scene needs the CS actually working (Option A) or a faithful replacement. A heavier auto-navigated scene (`draws=8443`) in this broken state hit `ErrorDeviceLost` (debug_op=3) once — not cleanly attributed (Stage 1 change + different scene); the reverted A/B build was clean where it reached but didn't get to the same scene.

### 2. Pixel `input_num=0` (worked around; real bug open)

PS `0x4c22cd8526171dbb` (116 words): `SPI_PS_IN_CONTROL & 0x3f` decodes to **0**, so `input_num=0`, but the shader reads `GetAttribute(0, ...)`. Worked around in `ShaderInfoCollection` (log + continue; `EmitAttribute` → 0). Real fix: find why that PS is compiled before `SPI_PS_IN_CONTROL` is written — register-capture ordering in the PM4 → pipeline-key path. Reachable with `KYTY_SKIP_CS_HASH` set.

### 3. depth-surface-as-color-target (FIXED 2026-09-09)

`0x1163740000`, 512 KB, depth format, bound as a color RT. Fixed in `FindImage` / `AcquireRenderTargets`.

### 4. CFG structurizer invalid SPIR-V (FIXED 2026-09-09)

3 pixel übershaders → invalid structured control flow → GPU hang. Fixed by `ValidateStructuredExits()` + Auto fallback to DispatcherFull.

### 4b. Skin corruption — rainbow oil-slick on all exposed skin (open, isolated 2026-09-09)

With the intro/corner scenes rendering, the visible "graphical issues": **all exposed skin** (fighters' arms/faces/torsos, foreground cornerman arm) shows a psychedelic rainbow marbled pattern that follows surface curvature; hair has green+magenta speckle. Clothing, cage, crowd, arena all correct.

Isolated to a **texture format alias**: 40× `TextureCache: sampling backing format 122 instead of descriptor format 98 addr=0x0000001156990000` (and `sampling GPU-written alias fmt=122 instead of req_fmt=98`). `0x1156990000` is a **256×256, 9-mip** render target written as `fmt=0x6 nfmt=0x7` (→ `VK_FORMAT_B10G11R11_UFLOAT_PACK32` = 122, an HDR/prefiltered-cube style buffer); the skin shader samples that same memory with a descriptor for `VK_FORMAT_A2R10G10B10_SNORM_PACK32` = 98 (signed packed — normal/SSS style). Both 32-bit so the cache aliases and `FindTexture` overrides the view to the backing format (`textureCache.cpp:1623`), and the `FindImage` `encoding_mismatch && same_block` branch (`~1417`, comment "UFC 5 draws the title as RGBA8, composites with 11-11-10") deliberately samples the written encoding as-is. That's right for the title screen but wrong for SNORM-vs-UFLOAT skin: the shader wants the 32 bits *reinterpreted*, not resampled as float. Likely fix: for this case create a real format-reinterpret `ImageView` (mutable format, both in the 32-bit compatibility class) with the descriptor's format instead of forcing the backing format. Or check whether the guest descriptor→vk 98 decode is even correct (256×256×9-mip suggests a cubemap/prefilter, which shouldn't be SNORM).

**UPDATE 2026-09-09 — reinterpret-view fix landed, DID NOT help.** `FindTexture` now serves a true bitcast `ImageView` in the descriptor format (122→98) when backing is packed-float and `FormatsCompatible` (see "Fixes applied → Shader recompiler" entry). Confirmed firing in-game on `0x1156990000`; **skin looks identical, no regression.** So the bug is NOT a view-encoding issue at the sample site. Revised hypotheses for next session, in order:
  1. **Wrong source image / stale contents.** `0x1156990000` may be getting bound when the shader actually wants a *different* surface, or this RT is never correctly populated because it's downstream of the stubbed occlusion CS. Check what writes `0x1156990000` and when, relative to the skin draw.
  2. **Tiling / swizzle mismatch** — the 256×256×9-mip surface may be macro-tiled on PS5 and we're reading it linear (or wrong micro-tile mode). Dump the raw image and eyeball for tile-block scrambling vs pure colour-space wrongness.
  3. **Guest descriptor decode** — verify the `nfmt`/`dfmt` → vk-98 path; a 9-mip 256² buffer smells like a prefiltered cube (should be a UFLOAT/half, not SNORM). If the *descriptor* decode is wrong, 98 is a red herring.
  4. It may simply be a **lighting/SSS term that reads garbage from a stubbed CS buffer** — i.e. same root cause as the black round, just a codepath that survives. Lowest priority to chase independently.

### 5. Other Vulkan validation errors (open — real, but NOT the hang)

From `--vulkan-validation true` (`KytyLog-PPSA03541-valid2.txt`):

- **`dstColorBlendFactor VK_BLEND_FACTOR_SRC1_ALPHA` but `dualSrcBlend` feature never enabled.** One-liner: `device_features.dualSrcBlend = supported_features2.features.dualSrcBlend;` in `vulkanWindow.cpp` (~line 707). Also the fragment shader emits `out_mrt_1` at **Location 1** instead of dual-source **Location 0 / Index 1** — needs an `Index` decoration in the SPIR-V backend when the blend state uses SRC1_* factors.
- **`image_10` compute sampler wants UINT, bound descriptor format is `VK_FORMAT_B10G11R11_UFLOAT_PACK32`** — type-confused image read (near `TextureCache: sampling GPU-written alias fmt=122 instead of req_fmt=98`).
- `vkCmdCopyImage` srcImage type 1D ≠ dstImage type 2D.
- `vkCreateImage mipLevels (9/10/11)` exceeds the extent-derived max (8/9/10) — creation fails, null image used later.

### 6. 3D slideshow

Menus ~20–60 fps. In-match / 3D scene ~**1 fps** with **8k–10k draws** after compiles finish. Made worse by skipping the occlusion-cull CS. Separate from the hang CS.

---

## Next steps

0. **Hang CS (session 14 — DONE):** LDS `DS_APPEND`/`DS_CONSUME` no longer apply a GDS `m0` size
   window. Stub removed from `run_ufc5.ps1` default; use `-StubHangCs` only for A/B. Keep
   Waitcnt/Barrier ImageMemory (`97250b5`) and EXECUTE/SKIP hooks for diagnostics.
1. **Fight round still black — hang CS now executes; re-check the consumer.** Dynamic buffer 9
   (18 MB; `0x1163770000` in frame 1073) is reinterpreted as the 1600x900 HDR scene input. With
   real occlusion CS output, verify whether the round still stays black; if so, chase buffer/image
   producers downstream of this CS rather than re-stubbing.
2. **Skin corruption** (issue 4b) — the `fmt 98↔122` reinterpret-view fix is landed but **did not change the visuals**. Not a sample-site encoding bug. Next: chase the 4 revised hypotheses in issue 4b (wrong/stale source image → tiling → descriptor decode → stubbed-CS garbage). Start by tracing what writes `0x1156990000`.
3. **Wave64 → wave32 lowering rework (Option A).** Stage 1 landed (−26% OpSelect, measured). Next: **Stage 1b** — at structurize time mark the structured-`if`-body entry exec as identity so the `s_and_saveexec`/`s_mov exec,<saved>` restores fold too (catches most of the remaining 1309 selects). The proposed `lane_count = 1` / two-subgroup LDS bridge is **ruled out on this RTX 3070**: a minimal one-readlane test TDRs despite valid SPIR-V and correctly initialized counters. Keep the two-half model unless a design avoids cross-subgroup waiting.
   - Per-lane ALU: stop the 2×-per-invocation packing → 1 GCN thread = 1 SPIR-V invocation (kills the doubling on most of the shader).
   - Cross-lane ops: one `combine(subgroup0, subgroup1)` per wave through a single LDS slot + one barrier, computed once per loop edge — not twice per use. `readlane`→`subgroupShuffle`/LDS; `readfirstlane`→`subgroupBroadcastFirst`+LDS hop; `ballot`→two `subgroupBallot`+LDS halves; wave reduce→`subgroupAdd`/etc.
   - **Declare `GroupNonUniformArithmetic`** (currently not declared — reductions are hand-rolled 6-step shuffle trees ×2).
   - `exec` mask: recognize the `v_cmpx / s_cbranch_execz / s_mov_b64 exec,-1` idiom as a plain SPIR-V `if` and let the driver handle divergence; only emulate `exec` for the genuinely weird cases (`s_not_b64 exec` then continue, cross-mask value reuse).
   - Pin with `VK_EXT_subgroup_size_control` + `requiredSubgroupSize=32` + `FULL_SUBGROUPS`.
   - Expected: ~3–6× for this shader (tile ~0.67 s → ~0.1–0.2 s). Might survive with chunking; helps every compute-heavy Frostbite shader. If not enough, the fallback is a hand-written wave32 replacement of the occlusion-cull algorithm (detect the hash, substitute the pipeline) or an AMD wave64 GPU.
4. **`input_num=0`** — fix at the source (PS `0x4c22cd8526171dbb` register-capture timing).
5. **dualSrcBlend** — enable the feature; emit the dual-source PS output at Location 0 / Index 1.
6. **Slideshow** — draw/submit cost on the 3D scene once the GPU stays alive.

## Diagnostic env / flags

```
$env:KYTY_SKIP_CS_HASH       = '0xea0aceac518ec52d'   # skip hang CS entirely — fast iteration (cap=1 no longer survives)
$env:KYTY_EXECUTE_CS_HASH    = '0xea0aceac518ec52d'   # force real dispatch even when SKIP lists the hash (A/B; TDR risk)
$env:KYTY_GDS_LIMIT_CAP      = '1'                    # cap hang-CS GDS work limit; still DeviceLost on RTX 3070 (session 13)
$env:KYTY_CS_IDENTITY_HASH   = '0xea0aceac518ec52d'   # optional; hang CS always logs CsImageIdentity/CsAliasPair
$env:KYTY_SHADER_DUMP_DIR    = 'D:\PS5\shader-dumps'
$env:KYTY_DUMP_SHADER_HASH   = '0x…,0x…'              # dump .spv/.rdna2/.cfg.txt/.ir.txt for a hash (comma list)
$env:KYTY_SHADER_CFG_STRATEGY= 'auto'                 # legacy | dispatch | auto (default). auto now falls back to dispatch on invalid legacy output.
$env:KYTY_GDS_LIMIT_CAP      = '1'                    # cap hang-CS work items (no longer prevents the TDR as of frame ~571)
$env:KYTY_GDS_GROUP_CAP      = '1'                    # diagnostic: cap the hash's actual X workgroup count (normally 144)
$env:KYTY_GDS_SYNC_CAP       = '1'                    # diagnostic: Finish + exact wall time after each capped hash dispatch
$env:KYTY_GDS_CHUNK          = '4'
```

CLI flags:
- `--shader-validation true` — run spvtools `Validate` on emitted SPIR-V; on failure dump `.spv` to the shader-log folder + print friendly disassembly, then `EXIT`. **Off by default** (this is why the übershader invalid SPIR-V shipped silently for so long).
- `--vulkan-validation true` — `VK_LAYER_KHRONOS_validation`; errors now non-fatal (logged `[VALIDATION-ERROR]`). `VK_LAYER_LUNARG_crash_diagnostic` is also installed if a device-lost needs pinpointing.
- `--gpu-assisted-validation true` — bounds-checks shader accesses on the GPU; very slow, implies `--vulkan-validation`.

Key logs this session: `KytyLog-PPSA03541-valid2.txt` (validation sweep), `KytyLog-PPSA03541-shval.txt` (übershader dump), `KytyLog-PPSA03541-dispatch.txt` (forced dispatch — reached frame 571), `KytyLog-PPSA03541-cfgfix.txt` (the `ValidateStructuredExits` fix — auto fallback working).
