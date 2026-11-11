[CmdletBinding()]
param(
    [ValidateRange(5,60)] [int] $Seconds = 30,
    [int] $EmulatorId = 0,
    [string] $OutputDirectory = 'D:\PS5\ufc5-profiles',
    [string] $ProfileFile = 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\gpuview\log.wprp',
    [string] $WprExecutable,
    [string] $FlatSrtControlFile = 'D:\PS5\ufc5-flat-srt.control',
    [string] $ProductionControlFile = 'D:\PS5\ufc5-production-profile.control',
    [string] $PresentationLog = 'D:\PS5\fps-pressure-test.txt',
    [switch] $ArmProduction,
    [string] $ApplicationProfilePrefix,
    [switch] $ReplayDetiles,
    [string] $ReplayCaptures = 'D:\PS5\ufc5-replays\fight',
    [ValidateRange(1,100)] [int] $ReplayIterations = 10,
    [ValidateRange(0,100)] [int] $ReplayWarmup = 3,
    [string] $ReplayBuildDirectory,
    [switch] $CheckOnly
)
$ErrorActionPreference = 'Stop'
if ($ReplayDetiles -and $ArmProduction) { throw 'Replay tracing requires the game profiler off; do not combine with ArmProduction.' }
$replayScript = Join-Path $PSScriptRoot 'replay-detile.ps1'
if (-not $ReplayBuildDirectory) { $ReplayBuildDirectory = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path '_Build\windows' }
$replayBinary = Join-Path $ReplayBuildDirectory 'shader_recompiler_compute_tests.exe'
if ($ReplayDetiles -and (-not (Test-Path -LiteralPath $ReplayCaptures) -or -not (Test-Path -LiteralPath $replayBinary))) {
    throw 'Replay captures or headless executable missing; build and verify them before tracing.'
}
if (-not $WprExecutable) {
    $toolkitWpr = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Windows Performance Toolkit\wpr.exe'
    $WprExecutable = if (Test-Path -LiteralPath $toolkitWpr) { $toolkitWpr } else { (Get-Command wpr.exe -ErrorAction Stop).Source }
}
$wpr = (Resolve-Path -LiteralPath $WprExecutable).Path
$wprVersion = (Get-Item -LiteralPath $wpr).VersionInfo
# Microsoft documents stop error 0x80010106 on older WPR and recommends WPT
# build 19650+. Prefer the installed toolkit, never PATH's old CoreSystem WPR.
if ($wprVersion.FileMajorPart -eq 10 -and $wprVersion.FileBuildPart -lt 19650) {
    throw "WPR build $($wprVersion.FileVersion) is affected by stop error 0x80010106. Select the installed newer toolkit WPR with -WprExecutable."
}
if (-not (Test-Path -LiteralPath $ProfileFile)) { throw "GPUView profile missing: $ProfileFile" }
$profileOutput = & $wpr -profiles $ProfileFile 2>&1
if ($LASTEXITCODE -ne 0) { throw "WPR cannot parse profile: $profileOutput" }
[xml] $profileXml = Get-Content -LiteralPath $ProfileFile -Raw
if (-not ($profileXml.WindowsPerformanceRecorder.Profiles.Profile | Where-Object { $_.Id -eq 'GPUView.Light.File' })) {
    throw 'Installed profile has no GPUView.Light.File configuration.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$admin = ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$emulators = @(Get-Process -Name 'kyty_emulator*' -ErrorAction SilentlyContinue)
if ($EmulatorId) { $emulators = @($emulators | Where-Object Id -eq $EmulatorId) }
if ($emulators.Count -ne 1) { throw 'Require exactly one running Kyty process; select it with -EmulatorId if necessary.' }
$emulator = $emulators[0]
$applicationManifest = $null
if ($ArmProduction) {
    if (-not $ApplicationProfilePrefix) { throw '-ArmProduction requires -ApplicationProfilePrefix from the running profiling launch.' }
    $applicationManifest = Get-Content -LiteralPath ($ApplicationProfilePrefix + '.manifest.json') -Raw | ConvertFrom-Json
    if ($applicationManifest.pid -ne $emulator.Id -or $applicationManifest.sha256 -ne (Get-FileHash -LiteralPath $emulator.Path -Algorithm SHA256).Hash) {
        throw 'Application profiling launch does not match the running PID/executable.'
    }
    if ($applicationManifest.control_file -ne $ProductionControlFile) { throw 'Application capture control does not match this helper.' }
    if (-not (Test-Path -LiteralPath $ProductionControlFile)) { throw 'Application profile control is missing.' }
}
foreach ($controlPath in @($FlatSrtControlFile, $ProductionControlFile)) {
    if (Test-Path -LiteralPath $controlPath) {
        $value = (Get-Content -LiteralPath $controlPath -Raw).Trim()
        if (($value -split '\s+')[0] -ne 'off') { throw "Set this experiment/profiler off before baseline tracing: $controlPath ($value)" }
    }
}
$status = & $wpr -status 2>&1
if ($LASTEXITCODE -ne 0) { throw "Cannot check WPR state: $status" }
# Fail closed if another recording exists or this English status cannot be recognized.
if (($status -join "`n") -notmatch 'WPR is not recording') { throw "WPR already recording or status unknown; preserve it: $status" }
if ($CheckOnly) {
    [pscustomobject]@{ Administrator=$admin; EmulatorId=$emulator.Id; Profile=$ProfileFile; Seconds=$Seconds; Wpr=$wpr; WprVersion=$wprVersion.FileVersion; Recording='none'; ArmProduction=[bool]$ArmProduction; ReplayDetiles=[bool]$ReplayDetiles; ReadyToRecord=$admin }
    return
}
if (-not $admin) { throw 'Windows kernel/GPU tracing requires Administrator. Run this script in an elevated PowerShell; no emulator restart is needed.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$instance = 'UFC5-' + [guid]::NewGuid().ToString('N')
$base = Join-Path $OutputDirectory ('gpuview-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $instance.Substring(5,8))
$etl = $base + '.etl'
$manifestPath = $base + '.json'
$commandLog = $base + '.wpr.txt'
function Invoke-WprCapture([string[]] $Arguments, [string] $Phase) {
    # Windows PowerShell 5 converts redirected native stderr (even blank lines)
    # into NativeCommandError under ErrorAction=Stop. Preserve the native exit
    # status and both output streams without routing stderr through PowerShell.
    $quoted = @($Arguments | ForEach-Object {
        '"' + ([regex]::Replace($_, '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"'
    })
    $stdout = $base + '.wpr-' + $Phase + '.stdout.txt'
    $stderr = $base + '.wpr-' + $Phase + '.stderr.txt'
    $process = Start-Process -FilePath $wpr -ArgumentList $quoted -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null = $process.Handle
    $process.WaitForExit()
    $output = @(Get-Content -LiteralPath $stdout) + @(Get-Content -LiteralPath $stderr)
    "WPR $Phase exit code: $($process.ExitCode)" | Out-File -LiteralPath $commandLog -Encoding UTF8 -Append
    $output | Out-File -LiteralPath $commandLog -Encoding UTF8 -Append
    return [pscustomobject]@{ ExitCode=$process.ExitCode; Output=$output }
}
function Get-OpenFileLength([string] $Path) {
    # Directory-entry sizes can stay stale while Kyty keeps its CRT streams open.
    # Query EOF through a fresh shared file handle instead.
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, ([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete))
    try { return $stream.Seek(0, [System.IO.SeekOrigin]::End) }
    finally { $stream.Dispose() }
}
function Get-CaptureBoundary {
    $bytes = if (Test-Path -LiteralPath $PresentationLog) { Get-OpenFileLength $PresentationLog } else { $null }
    $flat = if (Test-Path -LiteralPath $FlatSrtControlFile) { (Get-Content -LiteralPath $FlatSrtControlFile -Raw).Trim() } else { $null }
    $production = if (Test-Path -LiteralPath $ProductionControlFile) { (Get-Content -LiteralPath $ProductionControlFile -Raw).Trim() } else { $null }
    [ordered]@{ utc=[DateTime]::UtcNow.ToString('o'); qpc_ticks=[Diagnostics.Stopwatch]::GetTimestamp(); present_log_bytes=$bytes; flat_srt=$flat; production_profile=$production }
}
function Get-ApplicationOffsets {
    if (-not $ApplicationProfilePrefix) { return $null }
    $directory = Split-Path -Parent $ApplicationProfilePrefix
    $stem = Split-Path -Leaf $ApplicationProfilePrefix
    $offsets = [ordered]@{}
    Get-ChildItem -LiteralPath $directory -File | Where-Object { $_.Name -eq $stem -or $_.Name.StartsWith($stem + '.') } | ForEach-Object {
        $offsets[$_.Name] = Get-OpenFileLength $_.FullName
    }
    return $offsets
}
$manifest = [ordered]@{
    state='prepared'; instance=$instance; etl=$etl; seconds_requested=$Seconds
    emulator_id=$emulator.Id; executable=$emulator.Path
    executable_sha256=(Get-FileHash -LiteralPath $emulator.Path -Algorithm SHA256).Hash
    emulator_started=$emulator.StartTime.ToUniversalTime().ToString('o')
    wpr=$wpr; wpr_version=$wprVersion.FileVersion; wpr_sha256=(Get-FileHash -LiteralPath $wpr -Algorithm SHA256).Hash
    profile=$ProfileFile; profile_sha256=(Get-FileHash -LiteralPath $ProfileFile -Algorithm SHA256).Hash
    profile_selection='GPUView.light'; qpc_frequency=[Diagnostics.Stopwatch]::Frequency
    flat_srt_control=$FlatSrtControlFile; production_control=$ProductionControlFile
    presentation_log=$PresentationLog; start=$null; end=$null; error=$null
    capture_kind=$(if ($ReplayDetiles) { 'correlated_standalone_replay' } elseif ($ArmProduction) { 'correlated_application_profile' } else { 'baseline' })
    application_profile_prefix=$ApplicationProfilePrefix; application_manifest=$applicationManifest
    application_offsets_before=$null; application_offsets_after=$null; application_arm=$null
    replay=$(if ($ReplayDetiles) { [ordered]@{
        captures=$ReplayCaptures; iterations=$ReplayIterations; warmup=$ReplayWarmup
        executable=$replayBinary; executable_sha256=(Get-FileHash -LiteralPath $replayBinary -Algorithm SHA256).Hash
        csv=($base + '.replay.csv'); profile=($base + '.replay.gpu.csv'); begin=$null; end=$null; metadata=$null
    } } else { $null })
    limits='Baseline OS packet/scheduling trace. No guest resource IDs or per-shader GPU timing inferred. Check lost events and instrumentation overhead before conclusions.'
}
function Save-Manifest { $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8 }
Save-Manifest
$started = $false
try {
    # The instance name is always last; never stop/cancel an unrelated WPR instance.
    $startResult = Invoke-WprCapture @('-start', ($ProfileFile + '!GPUView.light'), '-filemode', '-instancename', $instance) 'start'
    $startCode = $startResult.ExitCode
    if ($startCode -ne 0) { throw "WPR start failed ($startCode). See $commandLog" }
    $started = $true
    $manifest.start = Get-CaptureBoundary
    $manifest.state = 'recording'
    if ($ArmProduction) {
        $manifest.application_offsets_before = Get-ApplicationOffsets
        $manifest.application_arm = Get-CaptureBoundary
        Set-Content -LiteralPath $ProductionControlFile -Value 'on' -Encoding Ascii
        Write-Output 'Application profiling window requested. Its configured epoch count stops it automatically; this adds profiling overhead.'
    }
    Save-Manifest
    Write-Output "Recorder: $wpr ($($wprVersion.FileVersion))"
    Write-Output "Recording paused-fight $($manifest.capture_kind) for $Seconds seconds; keep the scene constant."
    if ($ReplayDetiles) {
        $manifest.replay.begin = Get-CaptureBoundary
        Save-Manifest
        try {
            & $replayScript -Captures $ReplayCaptures -Iterations $ReplayIterations -Warmup $ReplayWarmup `
                -BuildDirectory $ReplayBuildDirectory -Csv $manifest.replay.csv -ProfileOutput $manifest.replay.profile
        } finally {
            $manifest.replay.end = Get-CaptureBoundary
            $replayMetadata = $manifest.replay.csv + '.meta.json'
            if (Test-Path -LiteralPath $replayMetadata) {
                $manifest.replay.metadata = Get-Content -LiteralPath $replayMetadata -Raw | ConvertFrom-Json
            }
            Save-Manifest
        }
    }
    $elapsed = ([Diagnostics.Stopwatch]::GetTimestamp() - $manifest.start.qpc_ticks) / [Diagnostics.Stopwatch]::Frequency
    $remainingMs = [int][Math]::Max(0, ($Seconds - $elapsed) * 1000)
    if ($remainingMs) { Start-Sleep -Milliseconds $remainingMs }
} catch {
    $manifest.error = $_.Exception.Message
    throw
} finally {
    if ($started) {
        try {
            $manifest.end = Get-CaptureBoundary
            $manifest.application_offsets_after = Get-ApplicationOffsets
            if ($ArmProduction) {
                # Off clears an unconsumed request; it does not cancel an active bounded window.
                Set-Content -LiteralPath $ProductionControlFile -Value 'off' -Encoding Ascii
            }
        } catch {
            $manifest.error = "Capture boundary/control collection failed: $($_.Exception.Message)"
            Write-Warning $manifest.error
        }
        # Attempt WPR stop even if boundary metadata or control collection failed.
        try {
            $stopResult = Invoke-WprCapture @('-stop', $etl, ('UFC5 paused-fight ' + $manifest.capture_kind + ', FlatSRT off'), '-skipPdbGen', '-instancename', $instance) 'stop'
            $stopCode = $stopResult.ExitCode
        } catch {
            $manifest.state = 'stop_failed'
            $manifest.error = "WPR stop invocation failed: $($_.Exception.Message). Instance: $instance"
            Save-Manifest
            throw
        }
        if ($stopCode -eq 0 -and (Test-Path -LiteralPath $etl)) {
            $manifest.state = if ($manifest.error) { 'saved_after_error' } else { 'saved' }
            Write-Output "Saved trace: $etl"
            if ($ArmProduction) {
                $cpuName = (Split-Path -Leaf $ApplicationProfilePrefix) + '.cpu.csv'
                $manifest.application_bytes_grew = $manifest.application_offsets_after[$cpuName] -gt $manifest.application_offsets_before[$cpuName]
                if (-not $manifest.application_bytes_grew) {
                    Write-Warning 'ETW saved, but producer CPU CSV did not grow. Verify application rows before treating this as a correlated capture.'
                }
            }
        } else {
            $manifest.state = 'stop_failed'
            $manifest.error = "WPR stop failed ($stopCode). Only this instance may need manual recovery: $instance. See $commandLog"
            Write-Warning $manifest.error
        }
    } else { $manifest.state = 'start_failed' }
    Save-Manifest
}
if ($manifest.state -eq 'stop_failed') { throw $manifest.error }
