[CmdletBinding()]
param(
    [string] $Captures = 'D:\PS5\ufc5-replays\fight',
    [ValidateRange(1,10000)] [int] $Iterations = 10,
    [ValidateRange(0,10000)] [int] $Warmup = 3,
    [string] $Csv,
    [switch] $Build,
    [switch] $SelfTest,
    [switch] $UploadPair,
    [string] $StencilCapture,
    [string] $ProfileOutput,
    [string] $BuildDirectory
)
$ErrorActionPreference = 'Stop'
if ($UploadPair -and ($SelfTest -or -not $StencilCapture -or -not (Test-Path -LiteralPath $StencilCapture))) {
    throw 'UploadPair requires a depth -Captures file and a valid -StencilCapture; it cannot be combined with SelfTest.'
}
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ($ProfileOutput -and ($UploadPair -or $SelfTest)) { throw 'ProfileOutput is for default detile replay only.' }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo '_Build\windows' }
$binary = Join-Path $BuildDirectory 'shader_recompiler_compute_tests.exe'
if ($Build) {
    & cmake --build $BuildDirectory --target shader_recompiler_compute_tests -j 8
    if ($LASTEXITCODE -ne 0) { throw 'Replay build failed.' }
}
if (-not (Test-Path -LiteralPath $binary)) { throw 'Replay executable missing. Run with -Build.' }
if (-not $SelfTest -and -not (Test-Path -LiteralPath $Captures)) {
    throw "Capture path missing: $Captures. Capture a fight once using capture-detile.ps1."
}
if (-not $Csv) {
    $Csv = Join-Path 'D:\PS5' ('ufc5-detile-replay-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.csv')
}
$mode = if ($UploadPair) { '--detile-upload-pair' } elseif ($SelfTest) { '--detile-replay-self-test' } else { '--detile-replay' }
$argsForReplay = @($mode, $Captures, '--iterations', "$Iterations", '--warmup', "$Warmup", '--csv', $Csv)
if ($UploadPair) { $argsForReplay += @('--stencil', $StencilCapture) }
if ($ProfileOutput) { $argsForReplay += @('--profile-output', $ProfileOutput) }
function Get-EmulatorSnapshot {
    @(Get-Process -Name 'kyty_emulator*' -ErrorAction SilentlyContinue | ForEach-Object {
        [ordered]@{ pid = $_.Id; path = $_.Path; responding = $_.Responding }
    })
}
$metadata = [ordered]@{
    mode = $mode; captures = $Captures; stencil_capture = $StencilCapture; profile_output = $ProfileOutput
    iterations = $Iterations; warmup = $Warmup
    executable = $binary; executable_sha256 = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
    start_utc = [DateTime]::UtcNow.ToString('o'); emulator_before = @(Get-EmulatorSnapshot)
}
$previous = @{}
foreach ($name in @('KYTY_GPU_TIMING_CSV', 'KYTY_DETILE_CAPTURE_DIR', 'KYTY_SPARSE_BDA')) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    [Environment]::SetEnvironmentVariable('KYTY_GPU_TIMING_CSV', $null, 'Process')
    [Environment]::SetEnvironmentVariable('KYTY_DETILE_CAPTURE_DIR', $null, 'Process')
    if (-not $previous['KYTY_SPARSE_BDA']) { $env:KYTY_SPARSE_BDA = '1' }
    if ($ProfileOutput) {
        # StartProcess exposes the child PID for native ETW joins. Quote each CRT
        # argument; no shell is involved. Logging is outside the measured loop.
        $quotedArgs = @($argsForReplay | ForEach-Object {
            '"' + ([regex]::Replace($_, '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"'
        })
        $replayProcess = Start-Process -FilePath $binary -ArgumentList $quotedArgs -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput ($Csv + '.stdout.txt') -RedirectStandardError ($Csv + '.stderr.txt')
        $metadata.replay_pid = $replayProcess.Id
        $null = $replayProcess.Handle
        $replayProcess.WaitForExit()
        $replayExit = $replayProcess.ExitCode
        Get-Content -LiteralPath ($Csv + '.stdout.txt')
        $replayErrors = Get-Content -LiteralPath ($Csv + '.stderr.txt')
        if ($replayErrors) { Write-Output $replayErrors }
    } else {
        & $binary @argsForReplay
        $replayExit = $LASTEXITCODE
    }
} finally {
    $metadata.end_utc = [DateTime]::UtcNow.ToString('o')
    $metadata.emulator_after = @(Get-EmulatorSnapshot)
    $metadata.exit_code = $replayExit
    foreach ($name in $previous.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
    }
    $metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath ($Csv + '.meta.json') -Encoding utf8
}
if ($replayExit -ne 0) { throw "Replay failed (exit $replayExit). 2 means output changed; 1 means invalid input or setup." }
Write-Output "Results: $Csv"
