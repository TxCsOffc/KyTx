Current update: the user subsequently accepted an average90% duty policy and game launch. The historical block described below has been superseded after finite controls and recovery. See `REPORT-host-budget-2026-11-11.md` for the implemented policy, limits, and new run results; a NEW failure still blocks retries.

# FPS continuation, 2026-11-11 — host runs blocked

The requested FPS/picture recovery is not achieved. User reported a second host freeze and reiterated a90% resource ceiling. No more GPU tests are authorized under the existing policy. At the final inventory there were no emulator/replay processes, and uptime indicated the host had already restarted. Do not request another reboot based on the old test state.

## Completed code and checks

- Preserved Claude's `metalCanonicalMemory.h` footprint cache; independently ran full coherence under Metal API + GPU validation: PASS.
- Added `metalShaderGuestWrites.h` and routed the renderer through it. Existing `has_address_writes` metadata distinguishes unknown address stores from address-table loads; bounded V# stores and atomics still invalidate their declared physical spans.31 CPU/alias generation checks PASS; the old `uses_dma` policy fails the new read-only preservation check. Full coherence after this policy change PASS; product builds PASS.
- Added full GDS before/after files to opt-in dispatch capture. Global BDA capture remains unsupported and is explicitly refused. Default single-flip multi-hash capture behavior is preserved. Optional `KYTY_REPLAY_DUMP_COUNT` captures a bounded series, one matching dispatch per frame.
- Workspace `replay_gds.mm` rebinds captured GDS, checks all written buffers/images/GDS, returns failure on differences, refuses missing GDS, and has a10-second host watchdog. Input-only mode explicitly makes no output comparison claim.
- Synthetic GDS test under API/GPU validation: all65536 GDS bytes +1024 output bytes match; missing input and corrupt output controls detected.
- FP4 f386 `c29caf0c1cb0ad94-d6a4c426ea6d4065`, GDS count14: native replay6.540ms, all74045440 written output bytes match capture exactly. This establishes replay fidelity only, not equivalence to RDNA/Vulkan. Paired SPIR-V file is empty.
- FP6 f332 count23: replay7.638ms completes and GDS matches; buffers10/11 and144 image1 bytes differ. Read-only buffer slices outside written aliases are unchanged between before/after. Cause of differences unresolved.
- FP9 saved15 complete dispatches (f171..185, GDS count17..23). Watchdog then stopped a later draw command buffer, status2 (`Committed`) with GPU/kernel timestamps0. This is not evidence that the named shader was executing an infinite loop.
- Watchdog diagnostics now retain and print the command buffer's real Metal status/timestamps at timeout.20-second threshold remains unchanged; no shader work limits or skips introduced.

## Failed runs / host recovery

FP1 failed binding0x8103980018+0x80000. FP2/5/6 watchdog reports named c29 native hashd6a4c426; FP8/9 stopped earlier/later draw command buffers. White/black scenes remained; there is no valid FPS win. All launch tags, hashes and logs are in `game-fp*.json`.

After FP9, even the baseline RW NoCopy microkernel and an ordinary Metal allocation timed out at5s; the formerly passing GDS fixture timed out at10s without API/GPU validation. These tests do not establish a protection/fence defect: the host GPU was no longer completing independent control work. User subsequently reported the host freeze. Final inventory after recovery contained no test processes.

## Resource policy correction

40GiB and utility/nice do not impose90% GPU utilization. The old policy did not meet the user's requirement. Added `.porting/work/HOST_GPU_TESTS_BLOCKED` and an early exit75 in both `with_resource_budget.sh` and `run1.sh`. Print-limit query remains read-only. Verified shell syntax and that an ordinary launch request is refused without executing its target. Original launchers otherwise retained; temporary app wrapper has separate bundle identifier to avoid a stale LaunchServices process entry.

Do not remove either block merely to continue testing. CPU-only inspection/build work can continue. A hard GPU percentage ceiling is not presently implemented or verified. Do not claim an average duty-cycle limiter can preempt a single hung kernel. No automatic reboot, driver reset, commits, staging or pushes.

## Primary references inspected

- [Apple command-buffer status](https://developer.apple.com/documentation/metal/mtlcommandbuffer/status): Committed resolves dependencies before scheduling.
- [Apple fence synchronization](https://developer.apple.com/documentation/metal/synchronizing-passes-with-a-fence): fences can synchronize passes across command buffers; reuse alone is not a demonstrated bug. No fence implementation changes made.

Artifacts and isolated before snapshots: `/Users/tx/Documents/Codex/2026-11-09/new-chat/work/metal-fps-validation`.
