# Codex continuation of Vulkan stability / native lighting, 2026-11-08

Read HANDOFF-claude-2026-11-08-vulkan-reference-and-lighting.md for prior work. User explicitly resumed it. After asking about the old run pause, user replied: "да уже можно сколько хочешь запусков, можно". Unlimited diagnostic runs now authorized; do not ask again. Keep one emulator at a time. No commits, no uploads, product stays native Metal. Clothes fixed already by945d467d; presenter9534b337; preserve all dirty work.

## New analysis

- Added offline `work/vulkan-lighting/audit_outputs.py` in Codex workspace. Unlike pt_summary, examines only RT/OUT chunks and separates zero-group dispatches (cannot write) from work actually submitted. On vk9 flip725 the ONLY two NaN-bearing outputs belong to zero-group1691cb86/15ef8445; r0 address8246360000 was earlier R8Uint then interpreted as RGB11F. This frame proves stale unused storage contents, not arithmetic NaN writes. Same in cvk1 flip537. mc25 first a155861e was NaN INPUT, not output; first NaN OUTPUT is TAA draw467 (RT1 824b000000, previously RGBA8 RT). Do not call these corrections a complete white-screen fix.
- Native pre-registration DCC replay in RegisterMetaClearRt is actually gated behind KYTY_RT_META_CLEAR (default OFF), despite the handoff wording. Do not blindly port it as validated.
- MemoryTracker download expands to full tracker pages and copies all GPU-dirty byte ranges, so a naive "partial4KiB metadata readback drops neighboring16KiB page data" hypothesis is NOT established.
- UMA readback (Apple default on) uses WriterTick range tracking, and unknown tick0 falls back to currentTick-1 on Apple. Worth A/B with live `_TempData/uma_off`, or KYTY_UMA_READBACK=0 on fresh run. Not the reverted MasterSemaphore/label-hold experiment.

## Changes (reference diagnostics only so far)

Reference textureCache.cpp: GetColorMetaTrace now one shared synchronized journal instead of thread_local instances reopening the same CSV with w; Write/Watch/Overlaps methods, no mutex held over GPU waits. Added B binding decisions, S mixed/undecodable reasons and C actual clears to existing F/R/W journal, all KYTY_COLOR_META_TRACE only. Native product rendering untouched in this continuation. Build kyty_emulator passes.
Backups in Codex workspace work/vulkan-lighting/textureCache.before-meta-probe.cpp and vk_timeline.before.sh.

.porting/work/vk_timeline.sh now restores only the single Settings DATA file and requires diff-rq clean, checks normal kyty_emulator too, footprint guard uses TERM instead of kill-9. No timed emulator kills.

## Active run

cvk1 PID81855, reference binary installed atomically from signed CMake build; backup KyTx-vkref/kyty_vkref.before-cvk1. Log fex-spike/runs/validation-20261025/lean/cvk1-022926.log.
Flags: default KosmicKrisp, KYTY_COLOR_META_TRACE=<Codexcwd>/work/vulkan-lighting/cvk1-meta.csv, KYTY_CMASK_PRECHECK_CSV=.../cvk1-meta-profile.csv.
Trigger helper PID81842 is SIGSTOPPED intentionally: its two-frame PassTrace every8s became continuous at0.2FPS, so untraced behavior must be checked. Game stays running. Resume helper with kill-CONT only when needed, or manually touch /tmp/kytx_f9/pass for one trace. If ending the run, CONT helper then TERM only own game PID; helper restores env.sh and exits. No other emulator running.

CSV metadata frame column is RenderContext::DiagnosticFrameNum, NOT TraceWindow flip; do not join directly to PassTrace flip. Need mapping/details in next journal revision if required.
cvk1 initial frames537/755/941/1128/1318/1512/1536... captured. Most later outputs clean, occasional small0e838ae6/af85c98e NaN then clears; no verified visual white episode yet. CUA screenshot aroundflip1675 was black transition during compilation, not white. CUA completion summary sent for this observational subtask.

Artifacts/intermediates under /Users/tx/Documents/Codex/2026-11-07/new-chat/work/vulkan-lighting. No result/visual fix claimed yet.

## Updates after cvk1

cvk1 completed its trace2633, then exited itself with VK_ERROR_DEVICE_LOST (-4) waiting tick98099/gpu98098. Its helper81842 was resumed and restored env.sh. CUA observed black main-menu background aroundflip2479. No UMA live toggle was made.

cvk2 PID84108 / helper84099 (helper STOPPED aroundtrace931), log cvk2-024005.log, SAME binary/diagnostic journal but KYTY_UMA_READBACK=0. CPU-copy path disabled, readback log reports drain only. White main-menu background visually confirmed atframe2038. So disabling UMA does NOT fix white screen; do not keep that as the solution. GPU capture requested, vk_f2761.gputrace currently being written; wait for stopped before opening/stopping. No other game.

Possible independent correctness gap: BufferCache::SynchronizeBufferFromImage queues a GPU copy into buffer but never NoteWriterTick; UMA readback may wait on an older writer. Added isolated --image-readback-order-only GPU regression to REFERENCE tests/ShaderRecompilerComputeTests.cpp, test target builds successfully. Has NOT run yet; source synchronization not changed yet. Backup work/vulkan-lighting/ShaderRecompilerComputeTests.before-readback-order.cpp. Run test with KYTY_UMA_READBACK=1, Kosmic ICD, after game stops. No production test or source change in native repo for this lead.

Next stronger lead: ResolveOverlap permits smaller width with same pitch to reuse larger backing. RenderTarget reuse is deliberately tested, but sampled image view dimensions/sample UV can then exceed guest T# dimensions. Needs actual extent-mismatch logging before claiming cause. Dynamic resolution differs between reference and native; native huge TAA NaN appeared during slow tracing. No fix claimed.

## cvk3 and shadow-compare investigation

- cvk2 capture completed: `/tmp/kytx_f9/gputrace/vk_f2761.gputrace` (white). Exact-own session219 terminated after saving command/resource inventories, TAA PNGs/source in Codex work/vulkan-lighting. TAA cb37/re7/draw0 both outputs already corrupt. Disabling UMA not sufficient.
- Added opt-in KYTY_VK_TRACE_IMAGE_EXTENT=1 in reference FindImage. cvk3 PID86326, no automatic PassTrace (TRACE_FIRST=86400), no metadata journal. Actual mismatches confirmed: 25x15 vs26/27x15 storage class tables; HDR856x484 vs860x484; sampled R32 depth1792x1008 vs1784x1008 etc. Does NOT prove white cause.
- cvk3 showed clean England studio aroundflip3266 (floor has window-shaped sun patch). Captured `/tmp/kytx_f9/gputrace/vk_f3439.gputrace`, stopped3440; then stopped only own86326, helper restored env. Session226 is ours, ready on3439. No Vulkan process.
- Important older note omitted from latest handoff: `.porting/porting-handoff-fc27-metal-2026-11-04-um222.md` says earlier claim CMP_FUNC broke red-room colour was invalid (different teams). `ref` probably correct but sparkles/white bloom persisted in old runs.
- Native cmt1 used pre-change binary with KYTY_MSL_CMP_FUNC=ref, PID87913, clean Settings; 1200..6000 frame dumps in Codex work/vulkan-lighting/cmt1-frames. RMA studio still whole floor bright and regular coloured sparks. Outdoors Paris improved sun/shadow but insufficient SAME-scene proof. Captured native `/tmp/kytx_f9/gputrace/frame2538.gputrace` (likely RMA studio; capture stops2538). Stopped only87913 aroundflip5200. No white screen observed among saved frames so far, but sparks recurring studio.
- Added native tests `ShadowComparisons()` in already-dirty metalMslTests.mm: GPU sample+gather,8 comparison funcs ×4 references (<,=,>,NaN) ×4 components =256 values. BEFORE correction fails `func=0 ref=.25 expected0 got1` (old code always6). AFTER correction all256 pass; whole native MSL suite passes with Metal API+shader validation.
- Native MslEmitter.cpp current NEW edit: shared SampleCompareFunction helper; default and explicit `ref` use sampler func with reference-first operand order; `legacy` restores old fixed texel>=ref; other existing flag modes preserve texel-first sampler func. CMake kyty_emulator rebuild passes. **VISUAL DEFAULT CHANGE IS STILL UNDER INVESTIGATION: DO NOT CLAIM IT SOLVES LIGHTING; cmt1 ref had sparks and missing floor shadow. Revert default or fix remaining regression before final acceptance.** Backups of pre-edit native emitter/tests in Codex work/vulkan-lighting/*before-shadow.*; preserve other dirty hunks.
- Baseline cmt2 launched from rebuiltbinary with KYTY_MSL_CMP_FUNC=legacy (same old rendering), cleanSettings; no timer (run1 max0), autoJ1500/1850, frame dumps1200..6000. extra_env_s1.sh temporarily changed, original backed up work/vulkan-lighting/extra_env_s1.before-cmt1.sh. Probe pixels now floor(.5,.9)/(.3,.8)/(.7,.85). Need check run-cmt2.log for PID/log and stop exact PID later. One emulator only.


## FINAL HANDOFF / STOP requested by user

User said "все пиши хендофф". Main handoff HANDOFF-codex-2026-11-08-vulkan-lighting.md supersedes the intermediate active-run/default-change claims above. All ours stopped (last cmt2 PID89550); gpudebug219/226 terminated. cmt2 only saved1200..1800 before stop; NOT a complete visual control. Native shadow experiment archived to artifacts/2026-11-08-codex-lighting/shadow-comparison-experiment.patch then EXACTLY reversed; emitter/tests byte-identical to pre-experiment backups (other dirty source retained). Native rebuild passes after restore; emulator ands3 SHA25698c55d7f453bcbfe49742fdb6188672609933ec29da7c864fd7acfc3addc19e1. Launch envs restored. No visual fix claimed.

CRITICAL CAPTURE CORRECTION: vk_f3439 is UK COBBLE street / white RMA tracksuit, verified by finalPNG, NOT England studio. Scene changed between CUA observation and actual correctcap trigger. WORK filenames studio3439-* are misleading; do not repeat that assumption. Nativeframe2538final notyetfetched.
