[CmdletBinding()]
param(
    [Parameter(Mandatory)] [int] $EmulatorId,
    [Parameter(Mandatory)] [string] $Prefix,
    [Parameter(Mandatory)] [string] $Stage,
    [ValidateRange(10,55)] [int] $Seconds = 40,
    [string] $PresentationLog = 'D:\PS5\fps-pressure-test.txt',
    [string] $ModeControlFile = 'D:\PS5\ufc5-compiled-srt.control',
    [ValidateSet('compiled','bda')] [string] $FileTag = 'compiled'
)
$ErrorActionPreference = 'Stop'
function Get-SharedLength([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return 0 }
    $stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try { return $stream.Length } finally { $stream.Dispose() }
}
function Get-Boundary {
    $process = Get-Process -Id $EmulatorId
    [ordered]@{
        utc=[DateTime]::UtcNow.ToString('o')
        host_ns=[long]([decimal][Diagnostics.Stopwatch]::GetTimestamp()*1000000000/[Diagnostics.Stopwatch]::Frequency)
        fps_bytes=Get-SharedLength $PresentationLog
        cpu_ms=$process.TotalProcessorTime.TotalMilliseconds
        title=$process.MainWindowTitle
        mode=(Get-Content -LiteralPath $ModeControlFile -Raw).Trim()
        compiled_srt_mode=if (Test-Path 'D:\PS5\ufc5-compiled-srt.control') { (Get-Content 'D:\PS5\ufc5-compiled-srt.control' -Raw).Trim() } else { $null }
    }
}
$measurement=[ordered]@{ stage=$Stage; pid=$EmulatorId; mode_control_file=$ModeControlFile; start=Get-Boundary; samples=@(); end=$null }
Write-Output "Measuring $Stage for $Seconds seconds."
$timer=[Diagnostics.Stopwatch]::StartNew()
while ($timer.Elapsed.TotalSeconds -lt $Seconds) {
    Start-Sleep -Milliseconds ([Math]::Min(5000,[Math]::Max(1,($Seconds-$timer.Elapsed.TotalSeconds)*1000)))
    $measurement.samples+=Get-Boundary
}
$measurement.end=Get-Boundary
$path=$Prefix+'.'+$FileTag+'-stage-'+$Stage+'.json'
$measurement | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $path -Encoding UTF8
Write-Output "Saved $path"
Write-Output $measurement.end.title
