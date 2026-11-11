# Launch EA Sports UFC 5 (PPSA03541) in KytyPS5 with the configuration that
# measured 5.0-5.4 fps in-fight on 2026-09-11.
#
#   .\run_ufc5.ps1                 # the 5 fps configuration
#   .\run_ufc5.ps1 -Tag myrun      # same, log to KytyLog-PPSA03541-myrun.txt
#   .\run_ufc5.ps1 -Baseline       # everything optional turned OFF, for A/B
#   .\run_ufc5.ps1 -Validate       # add --shader-validation (names a bad shader
#                                  # at compile time instead of a device loss)
#   .\run_ufc5.ps1 -Verify         # KYTY_SRT_LINEAR=verify: run both SRT
#                                  # evaluators and compare. SLOW - correctness
#                                  # check only, the fps from this run is
#                                  # meaningless because it does the work twice.
#   .\run_ufc5.ps1 -Panel          # enable the ImGui GPU inspector (diagnostics;
#                                  # performance numbers are not comparable)
#   .\run_ufc5.ps1 -NoStubImageClear
#                                  # diagnostic A/B: skip the wave64 occlusion CS
#                                  # without clearing its two storage images
#
# Reading the result: NEVER quote fps from a single FrameProfile line. Scenes
# vary 2,400-4,100 draws/frame, so two runs of the "same" fight differ by 70%.
# Take the median us/draw over the whole run - see "Replicating the 5 fps
# configuration" in D:\PS5\ledger.md.

[CmdletBinding()]
param(
    [string] $Tag       = 'run',
    [switch] $Baseline,
    [switch] $Validate,
    [switch] $Verify,
    [switch] $Panel,
    [switch] $Timestamps,
    [switch] $NoStubImageClear,
    [switch] $ExecuteHangCs,
    [string] $GameDir   = 'D:\PS5\Games\UFC5',
    [string] $BinDir    = 'D:\PS5\Emulators\KytyPS5-Bin',
    [string] $LogDir    = 'D:\PS5'
)

$ErrorActionPreference = 'Stop'

$exe = Join-Path $BinDir 'kyty_emulator.exe'
if (-not (Test-Path $exe))     { throw "emulator not found: $exe (build + deploy first)" }
if (-not (Test-Path $GameDir)) { throw "game dump not found: $GameDir" }

# Always close the running emulator first - copying over a live exe fails, and
# two instances fight over the same log file.
Get-Process kyty_emulator -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

# --- The configuration -------------------------------------------------------
# KYTY_SKIP_CS_HASH   REQUIRED to reach a fight at all (default). The Frostbite
#                     occlusion CS is wave64 and TDRs on a wave32-only RTX 3070.
#                     Its replacement clears storage images; not visually correct.
#                     Pass -ExecuteHangCs to run it for real (expect TDR risk).
# KYTY_SRT_LINEAR     Flat SRT evaluator. getprog -31%; does not show
#                     end-to-end, but it is verified equivalent and free to run.
if ($ExecuteHangCs) {
    Remove-Item Env:\KYTY_SKIP_CS_HASH -ErrorAction SilentlyContinue
    $env:KYTY_EXECUTE_CS_HASH = '0xea0aceac518ec52d'
    $Tag = "$Tag-hangcs-exec"
} else {
    $env:KYTY_SKIP_CS_HASH = '0xea0aceac518ec52d'
    Remove-Item Env:\KYTY_EXECUTE_CS_HASH -ErrorAction SilentlyContinue
}
$env:KYTY_SRT_LINEAR     = '1'
# KYTY_CMASK_SKIP_IMPOSSIBLE  Skip CMask readbacks that cannot yield a clear (session 65:
#                     output unchanged, no guest CPU faults; steadier pacing, no FPS gain).
$env:KYTY_CMASK_SKIP_IMPOSSIBLE = '1'
# KYTY_FUNCTION_LDS_BOUND  On by default in code; '0' restores 32KB/thread pixel-shader LDS
#                     (session 65: frees ~2.1GB VRAM at 4K, detile back to replay speed).
if ($NoStubImageClear) {
    $env:KYTY_STUB_CLEAR_IMAGES = '0'
    $Tag = "$Tag-no-stub-image-clear"
} else {
    Remove-Item Env:\KYTY_STUB_CLEAR_IMAGES -ErrorAction SilentlyContinue
}
if ($Panel) {
    $env:KYTY_DEBUG_PANEL = '1'
} else {
    Remove-Item Env:\KYTY_DEBUG_PANEL -ErrorAction SilentlyContinue
}
# KYTY_GPU_TIMESTAMPS is NOT on by default. GpuTimestamps::Arm() resets the query pool from the
# HOST while command buffers from the previous window can still be in flight, so a slot can be
# written twice with only one intervening reset - Vulkan validation reports
# "vkCmdWriteTimestamp2(): query N: query not reset", which is undefined behaviour and a
# plausible cause of the intermittent ErrorDeviceLost seen on 2026-09-11. Opt in with
# -Timestamps when you actually need per-draw/per-dispatch GPU attribution; frame_stats.py
# reads FrameProfile and does not need it. Opted in AFTER the clear loop below.

# Cleared explicitly so a value left over in the shell cannot silently change
# the run. All of these are measured NEUTRAL or REGRESSIONS - see the ledger.
#   KYTY_XFER_QUEUE       neutral (time relocates into dispatch recording)
#   KYTY_GC_CRITICAL_MB   transient only; VRAM climbs to whatever line you set
#   KYTY_TEXGC_AGE_FIX    +14us/draw of texture re-upload thrash
#   KYTY_TEXGC_BUDGET_FIX texgc 3ms -> 71ms; the GC runs ~175x per frame
#   KYTY_BUFGC_OWN_SHARE  0 restores the pre-fix BufferCache behaviour
#   KYTY_SKIP_PS_HASH     diagnostic; drops draws, so the frame is incomplete
foreach ($name in @('KYTY_XFER_QUEUE','KYTY_GC_CRITICAL_MB','KYTY_GC_TRIGGER_MB',
                    'KYTY_TEXGC_AGE_FIX','KYTY_TEXGC_BUDGET_FIX',
                    'KYTY_BUFGC_OWN_SHARE','KYTY_SKIP_PS_HASH',
                    'KYTY_GPU_TIMESTAMP_PS','KYTY_DEFER_READBACK','KYTY_GPU_TIMESTAMPS')) {
    Remove-Item "Env:\$name" -ErrorAction SilentlyContinue
}

if ($Timestamps) { $env:KYTY_GPU_TIMESTAMPS = '1' }

if ($Baseline) {
    # A/B control: the emulator as it behaves without this session's opt-ins.
    Remove-Item Env:\KYTY_SRT_LINEAR -ErrorAction SilentlyContinue
    $env:KYTY_BUFGC_OWN_SHARE = '0'
    $Tag = "$Tag-baseline"
}
if ($Verify) { $env:KYTY_SRT_LINEAR = 'verify'; $Tag = "$Tag-verify" }

$log  = Join-Path $LogDir "KytyLog-PPSA03541-$Tag.txt"
if (Test-Path $log) { Remove-Item $log -Force }

# --printf-direction File is NOT optional. printf_direction defaults to Silent
# and graphics_debug_dump_enabled() keys off it, so without this EVERY counter
# is silently discarded and the log looks fine but is empty of measurements.
$cliArgs = @('--game', $GameDir, '--printf-direction', 'File', '--printf-output-file', $log)
if ($Validate) { $cliArgs += @('--shader-validation', 'true') }

Write-Host ''
Write-Host 'KytyPS5 / UFC 5 (PPSA03541)' -ForegroundColor Cyan
Write-Host "  log      $log"
foreach ($n in @('KYTY_SKIP_CS_HASH','KYTY_EXECUTE_CS_HASH','KYTY_STUB_CLEAR_IMAGES','KYTY_GPU_TIMESTAMPS','KYTY_SRT_LINEAR','KYTY_BUFGC_OWN_SHARE','KYTY_DEBUG_PANEL')) {
    $v = [Environment]::GetEnvironmentVariable($n)
    if ($v) { Write-Host ("  {0,-22} {1}" -f $n, $v) }
}
if ($Validate) { Write-Host '  --shader-validation    true' }
Write-Host ''
Write-Host 'Navigate into a fight, then let it run ~2 minutes for steady state.' -ForegroundColor Yellow
Write-Host 'Expect ~5 fps in-fight. The tonemapper exposure image is initialized automatically.'
Write-Host ''
Write-Host 'Median us/draw over the whole run:' -ForegroundColor Cyan
Write-Host "  python `"$(Join-Path $PSScriptRoot 'frame_stats.py')`" `"$log`""
Write-Host ''

$proc = Start-Process -PassThru -FilePath $exe -ArgumentList $cliArgs -WorkingDirectory $BinDir
Write-Host "started pid=$($proc.Id)"
