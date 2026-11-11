[CmdletBinding()]
param(
    [ValidateSet('1080p','Native')] [string] $Mode = '1080p',
    [switch] $Build,
    [string] $BuildDirectory,
    [string] $BinDirectory = 'D:\PS5\Emulators\KytyPS5-Bin',
    [string] $GameDirectory = 'D:\PS5\Games\UFC5',
    [string] $LogDirectory = 'D:\PS5'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo '_Build\windows' }
if (Get-Process -Name 'kyty_emulator*' -ErrorAction SilentlyContinue) {
    throw 'Kyty is already running. Close it before starting the resolution test.'
}
if ($Build) {
    & cmake --build $BuildDirectory --target kyty_emulator -j 8
    if ($LASTEXITCODE -ne 0) { throw 'Resolution test build failed.' }
}
$source = Join-Path $BuildDirectory 'kyty_emulator.exe'
if (-not (Test-Path -LiteralPath $source)) { throw 'Executable missing. Run with -Build.' }
if (-not (Test-Path -LiteralPath $GameDirectory)) { throw "Game directory missing: $GameDirectory" }
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
$exe = Join-Path $BinDirectory 'kyty_emulator.resolution-test.exe'
Copy-Item -LiteralPath $source -Destination $exe -Force
$tag = 'resolution-' + $Mode + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$log = Join-Path $LogDirectory ('KytyLog-UFC5-' + $tag + '.txt')
$trace = Join-Path $LogDirectory ('ufc5-detile-' + $tag + '.csv')
$previous = @{}
$names = @('KYTY_VIDEO_OUT_1080P','KYTY_GPU_TIMING_CSV','KYTY_GPU_TIMESTAMPS',
           'KYTY_DETILE_CAPTURE_DIR','KYTY_DETILE_TRACE','KYTY_VRAM_TEST_FPS','KYTY_SPARSE_BDA',
           'KYTY_DROP_TEXTURES','KYTY_TIGHT_IMAGE_USAGE','KYTY_DRAW_COMMIT_THREAD')
foreach ($name in $names) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $env:KYTY_VIDEO_OUT_1080P = if ($Mode -eq '1080p') { '1' } else { '0' }
    foreach ($name in @('KYTY_GPU_TIMING_CSV','KYTY_GPU_TIMESTAMPS','KYTY_DETILE_CAPTURE_DIR')) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    $env:KYTY_DETILE_TRACE = $trace
    $env:KYTY_VRAM_TEST_FPS = '1'
    if (-not $previous['KYTY_SPARSE_BDA']) { $env:KYTY_SPARSE_BDA = '1' }
    $env:KYTY_DROP_TEXTURES = '0'
    $env:KYTY_TIGHT_IMAGE_USAGE = '0'
    $env:KYTY_DRAW_COMMIT_THREAD = '0'
    # Same window and rendering configuration in both modes. Only the guest output report differs.
    $launchArgs = @('--game', ('"' + $GameDirectory + '"'), '--printf-direction', 'File',
                    '--printf-output-file', ('"' + $log + '"'))
    $process = Start-Process -PassThru -FilePath $exe -WorkingDirectory $BinDirectory -ArgumentList $launchArgs
    Write-Output "Resolution test $Mode running: PID $($process.Id)."
    Write-Output "Game log and registered output dimensions: $log"
    Write-Output "Internal detile dimensions: $trace"
    Write-Output 'Memory: D:\PS5\tex-census.txt; present rate: D:\PS5\fps-pressure-test.txt (both append).'
    Write-Output 'Drive to the same paused fight. A 1080p output report is a request to the game, not a guaranteed internal resolution change.'
} finally {
    foreach ($name in $previous.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
    }
}
