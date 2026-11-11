[CmdletBinding()]
param(
    [ValidateSet('Prepare','Arm','Off','Status')] [string] $Mode = 'Status',
    [string] $Captures = 'D:\PS5\ufc5-replays\fight',
    [string] $ControlFile = 'D:\PS5\ufc5-detile-capture-control.txt',
    [switch] $Launch,
    [switch] $Build,
    [string] $BuildDirectory,
    [string] $BinDirectory = 'D:\PS5\Emulators\KytyPS5-Bin',
    [string] $GameDirectory = 'D:\PS5\Games\UFC5'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo '_Build\windows' }
if ($Launch) {
    $running = Get-Process -Name 'kyty_emulator','kyty_emulator.detile-capture' -ErrorAction SilentlyContinue
    if ($running) { throw 'Kyty is already running. Close it before starting the capture build.' }
    if ($Build) {
        & cmake --build $BuildDirectory --target kyty_emulator -j 8
        if ($LASTEXITCODE -ne 0) { throw 'Capture build failed.' }
    }
    $source = Join-Path $BuildDirectory 'kyty_emulator.exe'
    if (-not (Test-Path -LiteralPath $source)) { throw 'Capture executable missing. Run with -Build.' }
    if (-not (Test-Path -LiteralPath $GameDirectory)) { throw "Game directory missing: $GameDirectory" }
    # Stage beside the normal executable to reuse assets and caches without replacing it.
    $exe = Join-Path $BinDirectory 'kyty_emulator.detile-capture.exe'
    Copy-Item -LiteralPath $source -Destination $exe -Force
    $Mode = 'Prepare'
}
if ($Mode -in @('Prepare','Arm')) {
    New-Item -ItemType Directory -Path $Captures -Force | Out-Null
}
switch ($Mode) {
    'Prepare' { Set-Content -LiteralPath $ControlFile -Value 'off' -NoNewline }
    'Arm' { Set-Content -LiteralPath $ControlFile -Value 'on' -NoNewline }
    'Off' { Set-Content -LiteralPath $ControlFile -Value 'off' -NoNewline }
}
if ($Launch) {
    $previous = @{}
    foreach ($name in @('KYTY_DETILE_CAPTURE_DIR','KYTY_DETILE_CAPTURE_CONTROL_FILE','KYTY_GPU_TIMING_CSV','KYTY_SPARSE_BDA')) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    try {
        $env:KYTY_DETILE_CAPTURE_DIR = $Captures
        $env:KYTY_DETILE_CAPTURE_CONTROL_FILE = $ControlFile
        [Environment]::SetEnvironmentVariable('KYTY_GPU_TIMING_CSV', $null, 'Process')
        if (-not $previous['KYTY_SPARSE_BDA']) { $env:KYTY_SPARSE_BDA = '1' }
        $process = Start-Process -PassThru -FilePath $exe -WorkingDirectory $BinDirectory -ArgumentList @('--game', ('"' + $GameDirectory + '"'))
        Write-Output "Capture build running: PID $($process.Id). Drive to the paused fight, then run this script with -Mode Arm."
    } finally {
        foreach ($name in $previous.Keys) {
            [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
        }
    }
}
Write-Output "Control: $ControlFile"
if (Test-Path -LiteralPath $ControlFile) { Write-Output ('Switch: ' + (Get-Content -LiteralPath $ControlFile -Raw)) }
if (Test-Path -LiteralPath $Captures) {
    Get-ChildItem -LiteralPath $Captures -Filter '*.kdr' | Select-Object Name,Length,LastWriteTime
}
